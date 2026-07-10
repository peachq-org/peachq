/* iocp_win.c — openq-owned Windows IOCP backend for the ray_poll_* interface.
 *
 * Replaces the frozen ~60-line stub `src/core/iocp.c` (whose Windows object is
 * filtered out of the link in the Makefile; the stub file itself stays
 * byte-identical).  Design: docs/superpowers/plans/2026-07-10-win-iocp-event-loop.md.
 *
 * The ray_poll_* contract is a READINESS/REACTOR contract (the backend says
 * "go read now"; `recv_fn` does the actual recv()), so IOCP is presented as a
 * readiness reactor, not a proactor.  Staging:
 *
 *   C1 (this file today) — selector table + CreateIoCompletionPort + per-register
 *       association + process Winsock init (WSAStartup).  This alone makes the
 *       synchronous client paths work end-to-end (`hopen` / sync `h"expr"` /
 *       `\p` bind), because frozen ipc.c drives those with DIRECT sock.c I/O and
 *       uses the poll only as a selector registry (ray_poll_get) — it never
 *       enters ray_poll_run.  ray_poll_run is minimal: it services only blocking
 *       console/stdin selectors (see note at ray_poll_run) and returns -1 when
 *       there is nothing it can service.  Socket selectors are NOT dispatched.
 *   C2 (next) — real completion loop: GetQueuedCompletionStatusEx drain,
 *       zero-byte WSARecv readiness probes for data sockets, WSAPoll listener
 *       probe, in-flight/teardown-race handling.  Turns on the server half
 *       (`.z.pg/.z.ps/.z.pw`, async push).
 *   C3 — timers: bound the wait by ray_timers_next_deadline_ms and call
 *       ray_timers_fire_expired after each drain.
 *
 * Backend-private state lives in EXTENDED CONTAINER STRUCTS whose first member
 * is the frozen base struct (poll.h must not change; a file-scope id-keyed
 * array would collide across polls — selector ids are dense PER POLL).  Only
 * this file allocates/frees the containers; shared code (poll.c, ipc.c) only
 * ever reads base fields.
 */

#if defined(RAY_OS_WINDOWS)

#include "core/poll.h"
#include "core/timer.h"
#include "mem/sys.h"
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>   /* WSAStartup / WSABUF; pulls in windows.h (IOCP APIs) */

#define RAY_POLL_INITIAL_CAP 16

/* ===== Backend containers (base struct FIRST — pointer-compatible) ===== */

typedef struct {
    ray_selector_t base;
    /* C2 (reserved, zeroed in C1): the zero-byte WSARecv readiness probe. */
    OVERLAPPED     ov;
    WSABUF         probe;
    LONG           in_flight;
} iocp_sel_t;

typedef struct {
    ray_poll_t base;
    HANDLE     port;   /* the completion port (also mirrored in base.fd) */
} iocp_poll_t;

/* ===== Process Winsock init =====
 *
 * Nothing in the engine starts Winsock (sock.c calls socket()/WSAPoll()/recv()
 * with no WSAStartup; the only other init, q_dotz.c's win_wsa_ensure, fires
 * only on the `.z.h`/`.z.a` getters).  Without this every socket op fails
 * WSANOTINITIALISED.  ray_poll_create is the single correct home: both
 * launchers (src/app/main.c, src/qlang/qmain.c) call it before any socket
 * work.  Same one-time-guarded, never-WSACleanup pattern as q_dotz.c (winsock
 * is a process-lifetime resource; WSAStartup itself is refcounted/idempotent,
 * so a benign flag race just calls it twice).  Kept file-local rather than
 * exported from q_dotz.c: src/core must not depend on src/qlang.
 */
static bool iocp_wsa_ensure(void)
{
    static bool started = false;
    if (started) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    started = true;
    return true;
}

/* ===== create / destroy ===== */

ray_poll_t* ray_poll_create(void)
{
    if (!iocp_wsa_ensure()) return NULL;

    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) return NULL;

    iocp_poll_t* ip = (iocp_poll_t*)ray_sys_alloc(sizeof(iocp_poll_t));
    if (!ip) { CloseHandle(port); return NULL; }
    memset(ip, 0, sizeof(*ip));

    ray_poll_t* poll = &ip->base;
    ip->port      = port;
    poll->fd      = (int64_t)(intptr_t)port;
    poll->code    = -1;
    poll->sel_cap = RAY_POLL_INITIAL_CAP;
    poll->sels    = (ray_selector_t**)ray_sys_alloc(
                        poll->sel_cap * sizeof(ray_selector_t*));
    if (!poll->sels) {
        CloseHandle(port);
        ray_sys_free(ip);
        return NULL;
    }
    memset(poll->sels, 0, poll->sel_cap * sizeof(ray_selector_t*));
    return poll;
}

void ray_poll_destroy(ray_poll_t* poll)
{
    if (!poll) return;
    iocp_poll_t* ip = (iocp_poll_t*)poll;

    /* Deregister all selectors (mirrors epoll.c; there is no per-fd
     * dissociate syscall for IOCP — association dies with the socket /
     * the port handle). */
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* sel = poll->sels[i];
        if (!sel) continue;
        if (sel->close_fn) sel->close_fn(poll, sel);
        if (sel->rx.buf) ray_poll_buf_free(sel->rx.buf);
        ray_poll_buf_free(sel->tx.buf);
        ray_sys_free(sel);        /* frees the whole iocp_sel_t container */
        poll->sels[i] = NULL;
    }

    if (poll->sels) ray_sys_free(poll->sels);
    CloseHandle(ip->port);
    if (poll->timers) {
        ray_timers_destroy((ray_timers_t*)poll->timers);
        poll->timers = NULL;
    }
    ray_sys_free(ip);
}

/* ===== register / deregister ===== */

int64_t ray_poll_register(ray_poll_t* poll, ray_poll_reg_t* reg)
{
    if (!poll || !reg) return -1;
    iocp_poll_t* ip = (iocp_poll_t*)poll;

    /* Find free slot or grow (identical to epoll.c). */
    int64_t id = -1;
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        if (!poll->sels[i]) { id = (int64_t)i; break; }
    }
    if (id < 0) {
        if (poll->n_sels >= poll->sel_cap) {
            uint32_t new_cap = poll->sel_cap * 2;
            ray_selector_t** ns = (ray_selector_t**)ray_sys_alloc(
                new_cap * sizeof(ray_selector_t*));
            if (!ns) return -1;
            memcpy(ns, poll->sels, poll->n_sels * sizeof(ray_selector_t*));
            memset(ns + poll->n_sels, 0,
                   (new_cap - poll->n_sels) * sizeof(ray_selector_t*));
            ray_sys_free(poll->sels);
            poll->sels    = ns;
            poll->sel_cap = new_cap;
        }
        id = (int64_t)poll->n_sels;
        poll->n_sels++;
    }

    iocp_sel_t* is = (iocp_sel_t*)ray_sys_alloc(sizeof(iocp_sel_t));
    if (!is) return -1;
    memset(is, 0, sizeof(*is));
    ray_selector_t* sel = &is->base;

    sel->fd       = reg->fd;
    sel->id       = id;
    sel->type     = reg->type;
    sel->data     = reg->data;
    sel->open_fn  = reg->open_fn;
    sel->close_fn = reg->close_fn;
    sel->error_fn = reg->error_fn;
    sel->data_fn  = reg->data_fn;
    sel->rx.recv_fn = reg->recv_fn;
    sel->rx.read_fn = reg->read_fn;
    sel->tx.send_fn = reg->send_fn;

    poll->sels[id] = sel;

    /* Associate socket handles with the completion port, keyed by the
     * selector id (the analogue of epoll's ev.data.u64 = id).  In C1 the
     * association alone is enough — no overlapped op is posted yet (the
     * sync client path does direct sock.c I/O); C2 arms the first
     * readiness probe here.  Non-socket selectors (RAY_SEL_STDIN console
     * handles) cannot be attached to an IOCP — they are registry-only and
     * are serviced by the blocking dispatch in ray_poll_run. */
    if (reg->type == RAY_SEL_SOCKET) {
        if (!CreateIoCompletionPort((HANDLE)(intptr_t)reg->fd, ip->port,
                                    (ULONG_PTR)id, 0)) {
            poll->sels[id] = NULL;
            ray_sys_free(is);
            return -1;
        }
    }

    if (sel->open_fn) sel->open_fn(poll, sel);
    return id;
}

void ray_poll_deregister(ray_poll_t* poll, int64_t id)
{
    if (!poll || id < 0 || (uint32_t)id >= poll->n_sels) return;
    ray_selector_t* sel = poll->sels[id];
    if (!sel) return;

    /* No epoll_ctl(DEL) analogue — IOCP association is implicit and ends
     * when the socket is closed (close_fn → closesocket via ipc_on_close).
     * C1 posts no overlapped ops, so freeing here is trivially safe; C2
     * must defer the free while a probe is in flight (in_flight/CancelIoEx). */
    if (sel->close_fn) sel->close_fn(poll, sel);
    if (sel->rx.buf) ray_poll_buf_free(sel->rx.buf);
    ray_poll_buf_free(sel->tx.buf);
    ray_sys_free(sel);            /* frees the whole iocp_sel_t container */
    poll->sels[id] = NULL;
}

/* ===== run — C1-minimal =====
 *
 * The C1 client paths (hopen / sync h"…" / `\p` bind) never enter this
 * function (frozen ipc.c pumps those synchronously).  It IS entered by two
 * launcher paths once ray_poll_create returns a real object:
 *
 *   1. rayforce.exe interactive console (src/app/repl.c run_interactive):
 *      registers a RAY_SEL_STDIN selector and expects the loop to drive it.
 *      A console handle cannot be watched by IOCP, but the registered
 *      read_fn (repl_read → ray_term_getc → ReadFile) BLOCKS for input, so
 *      dispatching it directly reproduces the pre-IOCP blocking-console REPL
 *      exactly (one byte per call; EOF/`\q` exit via ray_poll_exit).  Without
 *      this the Windows console REPL would exit at launch — a regression
 *      vs the NULL-poll fallback the stub used to force.  (q.exe is not
 *      affected: q_repl_run_poll is stubbed to -1 on Windows.)
 *
 *   2. Server mode after `-p`/`\p` (qmain.c / main.c): C1 cannot service
 *      socket selectors (the completion loop is C2), so with no stdin
 *      selector registered this returns -1 and the process exits — the
 *      design-doc-accepted C1 limitation (bind works, serving is C2).
 */
int64_t ray_poll_run(ray_poll_t* poll)
{
    if (!poll) return -1;

    while (poll->code < 0) {
        /* Find a console/stdin selector we can service. */
        ray_selector_t* sel = NULL;
        for (uint32_t i = 0; i < poll->n_sels; i++) {
            ray_selector_t* s = poll->sels[i];
            if (s && s->type == RAY_SEL_STDIN && s->rx.read_fn) { sel = s; break; }
        }
        if (!sel) return -1;   /* nothing serviceable in C1 (sockets are C2) */

        int64_t id = sel->id;
        ray_t* obj = sel->rx.read_fn(poll, sel);

        /* Re-validate: read_fn may have deregistered the selector or
         * exited the loop (mirrors epoll.c's re-check discipline). */
        if ((uint32_t)id >= poll->n_sels || !poll->sels[id]) continue;
        sel = poll->sels[id];

        if (obj && sel->data_fn)
            sel->data_fn(poll, sel, obj);
    }

    return poll->code;
}

#endif /* RAY_OS_WINDOWS */
