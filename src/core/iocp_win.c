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
 *   C1 — selector table + CreateIoCompletionPort + per-register association +
 *       process Winsock init (WSAStartup).  Makes the synchronous client paths
 *       work end-to-end (`hopen` / sync `h"expr"` / `\p` bind), because frozen
 *       ipc.c drives those with DIRECT sock.c I/O and uses the poll only as a
 *       selector registry (ray_poll_get) — it never enters ray_poll_run.
 *   C2 (THIS FILE) — real completion loop: GetQueuedCompletionStatusEx drain,
 *       zero-byte WSARecv readiness probes for data sockets, bounded WSAPoll
 *       listener probe (Option A, zero frozen edits), per-op OVERLAPPED
 *       lifetime + teardown-race handling.  Turns on the server half
 *       (`.z.pg/.z.ps/.z.pw` dispatch runs through the frozen ipc.c read_fn
 *       state machine that this loop drives — identical to epoll.c).
 *   C3 (next) — timers: bound the wait by ray_timers_next_deadline_ms and call
 *       ray_timers_fire_expired after each drain.  NOT implemented here.
 *
 * Backend-private state lives in EXTENDED CONTAINER STRUCTS whose first member
 * is the frozen base struct (poll.h must not change; a file-scope id-keyed
 * array would collide across polls — selector ids are dense PER POLL).  Only
 * this file allocates/frees the containers; shared code (poll.c, ipc.c) only
 * ever reads base fields.
 *
 * Readiness reactor (C2), in one paragraph: each turn, arm a zero-byte
 * overlapped WSARecv on every data socket that has no probe in flight (a
 * zero-length recv completes when the socket is readable WITHOUT consuming any
 * bytes — the documented readiness trick, so it never races the frozen sync
 * recv() path).  GetQueuedCompletionStatusEx drains the completed probes;
 * CONTAINING_RECORD recovers the selector container from the OVERLAPPED; the
 * EXACT epoll.c:200-247 inner loop then runs recv_fn/read_fn to drain + dispatch
 * (this is where frozen ipc.c fires .z.pg/.z.ps/.z.pw and writes the reply
 * synchronously).  Listeners have no recv_fn/rx.buf, so they cannot be probed;
 * a bounded WSAPoll(0ms) each turn drives ipc_accept, and the wait is capped at
 * RAY_LISTENER_POLL_MS whenever a listener is registered so a fresh connect
 * still wakes the loop (≤~50ms accept latency — Option A, no AcceptEx / no
 * frozen ipc.c touch).
 *
 * OVERLAPPED / container lifetime (the classic IOCP teardown hazard): a probe
 * is posted iff in_flight==1 and exactly one completion is then pending for it.
 * Deregister of a selector with in_flight==0 frees its container immediately
 * (no completion can reference it).  Deregister with in_flight==1 cannot free —
 * a completion still references the OVERLAPPED — so it CancelIoEx + closes the
 * socket (both force the pending probe to complete, aborted) and defers the
 * free onto a per-poll zombie list; the completion loop recovers the zombie via
 * CONTAINING_RECORD and frees it then.  ray_poll_destroy closes the port
 * (discarding any still-queued packets) and frees every live selector + zombie.
 */

#if defined(RAY_OS_WINDOWS)

#include "core/poll.h"
#include "core/timer.h"
#include "mem/sys.h"
#include <string.h>
#include <errno.h>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>   /* WSAStartup / WSABUF / WSARecv / WSAPoll; pulls in
                         * windows.h (IOCP APIs, OVERLAPPED_ENTRY,
                         * CONTAINING_RECORD). */

#define RAY_POLL_INITIAL_CAP  16
#define RAY_POLL_MAX_EVENTS    64
#define RAY_LISTENER_POLL_MS   50   /* accept-latency cap (Option A, §4.3) */
#define RAY_ACCEPT_DRAIN_MAX  128   /* bound per-turn accepts (burst safety) */

/* ===== Backend containers (base struct FIRST — pointer-compatible) ===== */

typedef struct iocp_sel {
    ray_selector_t   base;
    OVERLAPPED       ov;          /* the zero-byte WSARecv readiness probe */
    WSABUF           probe;       /* points at probe_byte, len 0 */
    char             probe_byte;  /* dummy storage for the zero-length buf */
    LONG             in_flight;   /* 1 while a probe completion is pending */
    bool             zombie;      /* deregistered with a probe in flight;
                                   * free when its completion drains */
    struct iocp_sel* next_zombie; /* zombie free-list link (iocp_poll_t.zombies) */
} iocp_sel_t;

typedef struct {
    ray_poll_t   base;
    HANDLE       port;      /* the completion port (also mirrored in base.fd) */
    iocp_sel_t*  zombies;   /* containers awaiting their final completion */
} iocp_poll_t;

/* ===== Selector-kind predicates (backend stays ipc-agnostic) =====
 *
 * A data socket carries a recv_fn (its rx buffer is pumped by the reactor).  A
 * listener is a "level-readiness" selector: read_fn set, but NO recv_fn and no
 * rx.buf (ray_ipc_listen registers it that way — ipc.c:865-869), so it can only
 * be serviced by a readiness poll (WSAPoll), never a WSARecv. */
static inline bool iocp_is_data(const ray_selector_t* s) {
    return s && s->type == RAY_SEL_SOCKET && s->rx.recv_fn != NULL;
}
static inline bool iocp_is_listener(const ray_selector_t* s) {
    return s && s->type == RAY_SEL_SOCKET && s->rx.recv_fn == NULL
             && s->rx.read_fn != NULL;
}

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
    ip->zombies   = NULL;
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

/* Remove a container from the poll's zombie free-list (no-op if absent). */
static void iocp_zombie_remove(iocp_poll_t* ip, iocp_sel_t* target);

void ray_poll_destroy(ray_poll_t* poll)
{
    if (!poll) return;
    iocp_poll_t* ip = (iocp_poll_t*)poll;

    /* Tear down all live selectors (mirrors epoll.c; there is no per-fd
     * dissociate syscall for IOCP — association dies with the socket / the
     * port handle).  A selector with a probe in flight CANNOT be freed here:
     * CancelIoEx is asynchronous and closesocket forces the pending zero-byte
     * WSARecv to complete LATER — the kernel still writes &is->ov and posts a
     * completion packet after this call returns.  Freeing the container now
     * would let that completion land in freed memory (a shutdown-with-active-
     * connections UAF).  So defer in-flight containers onto the zombie list
     * exactly as ray_poll_deregister does, then DRAIN the port below before
     * freeing them — same lifetime discipline, applied at teardown. */
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* sel = poll->sels[i];
        if (!sel) continue;
        iocp_sel_t* is = (iocp_sel_t*)sel;
        poll->sels[i] = NULL;
        if (is->in_flight)
            CancelIoEx((HANDLE)(intptr_t)sel->fd, &is->ov);
        if (sel->close_fn) sel->close_fn(poll, sel);
        if (sel->rx.buf) ray_poll_buf_free(sel->rx.buf);
        ray_poll_buf_free(sel->tx.buf);
        sel->rx.buf = NULL;
        sel->tx.buf = NULL;
        if (is->in_flight) {
            is->zombie      = true;
            is->next_zombie = ip->zombies;
            ip->zombies     = is;
        } else {
            ray_sys_free(is);    /* frees the whole iocp_sel_t container */
        }
    }

    /* Drain the pending (now-aborted) probe completions so no kernel write
     * lands in freed memory.  CancelIoEx + closesocket guarantee each in-flight
     * op completes promptly, so the zombie list empties well within the bound;
     * the deadline is a defensive escape only.  Any straggler that never
     * completes is LEAKED, not freed (the process is exiting and the OS
     * reclaims it) — leaking is strictly safer than risking a UAF. */
    if (ip->zombies) {
        OVERLAPPED_ENTRY entries[RAY_POLL_MAX_EVENTS];
        int64_t deadline = ray_time_now_ms() + 5000;
        while (ip->zombies && ray_time_now_ms() < deadline) {
            ULONG n = 0;
            BOOL ok = GetQueuedCompletionStatusEx(ip->port, entries,
                                                  RAY_POLL_MAX_EVENTS, &n,
                                                  100, FALSE);
            if (!ok) {
                if (GetLastError() == WAIT_TIMEOUT) continue;
                break;   /* port error — stop draining, leak stragglers */
            }
            for (ULONG i = 0; i < n; i++) {
                iocp_sel_t* is = (iocp_sel_t*)CONTAINING_RECORD(
                    entries[i].lpOverlapped, iocp_sel_t, ov);
                is->in_flight = 0;
                if (is->zombie) {
                    iocp_zombie_remove(ip, is);
                    ray_sys_free(is);
                }
            }
        }
    }
    ip->zombies = NULL;   /* stragglers (if any) intentionally leaked — see above */

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

    /* Associate socket handles with the completion port, keyed by the selector
     * id (the analogue of epoll's ev.data.u64 = id).  The association alone is
     * inert until an overlapped op is posted; the reactor arms the first
     * zero-byte WSARecv probe lazily in ray_poll_run (NOT here) so the
     * synchronous client path — which registers a selector but drives I/O
     * directly and never enters ray_poll_run — is left untouched.
     * Non-socket selectors (RAY_SEL_STDIN console handles) cannot be attached
     * to an IOCP; they are registry-only and serviced by the blocking console
     * dispatch in ray_poll_run. */
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

/* Remove a container from the poll's zombie free-list (no-op if absent). */
static void iocp_zombie_remove(iocp_poll_t* ip, iocp_sel_t* target)
{
    iocp_sel_t** pp = &ip->zombies;
    while (*pp) {
        if (*pp == target) { *pp = target->next_zombie; return; }
        pp = &(*pp)->next_zombie;
    }
}

void ray_poll_deregister(ray_poll_t* poll, int64_t id)
{
    if (!poll || id < 0 || (uint32_t)id >= poll->n_sels) return;
    ray_selector_t* sel = poll->sels[id];
    if (!sel) return;
    iocp_poll_t* ip = (iocp_poll_t*)poll;
    iocp_sel_t*  is = (iocp_sel_t*)sel;

    /* Detach from the table first so shared code / a reused id never observes
     * a half-torn selector.  No epoll_ctl(DEL) analogue — IOCP association is
     * implicit and ends when the socket is closed (close_fn → closesocket via
     * ipc_on_close). */
    poll->sels[id] = NULL;

    /* Teardown-race: a probe may still be in flight (its completion is queued
     * in the port and points at &is->ov).  CancelIoEx + closesocket both force
     * that probe to complete (aborted), but we must NOT free the container yet
     * — the completion still references it.  Defer the free onto the zombie
     * list; the completion loop frees it when it drains.  With no probe in
     * flight, there is no queued completion, so the free is immediate. */
    if (is->in_flight)
        CancelIoEx((HANDLE)(intptr_t)sel->fd, &is->ov);

    if (sel->close_fn) sel->close_fn(poll, sel);
    if (sel->rx.buf) ray_poll_buf_free(sel->rx.buf);
    ray_poll_buf_free(sel->tx.buf);
    sel->rx.buf = NULL;
    sel->tx.buf = NULL;

    if (is->in_flight) {
        is->zombie      = true;
        is->next_zombie = ip->zombies;
        ip->zombies     = is;
    } else {
        ray_sys_free(is);        /* frees the whole iocp_sel_t container */
    }
}

/* ===== run: readiness reactor over IOCP (C2) ===== */

/* Arm one zero-byte WSARecv readiness probe.  Returns false on a hard error
 * (caller should error_fn/deregister the selector). */
static bool iocp_arm_probe(iocp_sel_t* is)
{
    ray_selector_t* sel = &is->base;
    is->probe.buf = &is->probe_byte;
    is->probe.len = 0;
    memset(&is->ov, 0, sizeof(is->ov));
    DWORD flags = 0, bytes = 0;
    int r = WSARecv((SOCKET)(uintptr_t)sel->fd, &is->probe, 1,
                    &bytes, &flags, &is->ov, NULL);
    if (r == 0) { is->in_flight = 1; return true; }   /* immediate: still queued */
    if (WSAGetLastError() == WSA_IO_PENDING) { is->in_flight = 1; return true; }
    return false;
}

/* The EXACT epoll.c:200-247 readable-drain inner loop.  Recovers `sel` from the
 * selector table by id after every callback (a read_fn/data_fn may deregister
 * it).  This is what runs frozen ipc.c's recv_fn/read_fn state machine, which
 * fires .z.pg/.z.ps/.z.pw and writes replies synchronously. */
static void iocp_service_readable(ray_poll_t* poll, iocp_sel_t* is)
{
    ray_selector_t* sel = &is->base;
    uint64_t id = (uint64_t)sel->id;

    for (;;) {
        /* Fill rx buffer */
        if (sel->rx.recv_fn && sel->rx.buf) {
            while (sel->rx.buf->offset < sel->rx.buf->size) {
                int64_t nr = sel->rx.recv_fn(
                    sel->fd,
                    sel->rx.buf->data + sel->rx.buf->offset,
                    sel->rx.buf->size - sel->rx.buf->offset);
                if (nr <= 0) {
                    if (nr < 0 && errno == EINTR) continue;
                    if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        break;
                    /* Error or peer closed mid-read */
                    if (sel->error_fn) sel->error_fn(poll, sel);
                    else ray_poll_deregister(poll, sel->id);
                    return;
                }
                sel->rx.buf->offset += nr;
            }
        }

        /* Not enough data for current phase */
        if (sel->rx.buf && sel->rx.buf->offset < sel->rx.buf->size)
            break;

        /* Advance state machine */
        if (!sel->rx.read_fn) break;
        ray_t* obj = sel->rx.read_fn(poll, sel);

        /* Re-validate: read_fn may have deregistered this selector */
        if (id >= poll->n_sels || !poll->sels[id]) return;
        sel = poll->sels[id];

        if (obj && sel->data_fn)
            sel->data_fn(poll, sel, obj);

        if (id >= poll->n_sels || !poll->sels[id]) return;
        sel = poll->sels[id];

        if (!sel->rx.buf) break;
        if (sel->rx.buf->offset >= sel->rx.buf->size) continue;
        /* Otherwise try reading more (may EWOULDBLOCK → break above) */
    }
}

/* Bounded WSAPoll(0ms) accept probe over every listener; drives ipc_accept
 * (which owns the accept() syscall).  Re-fetches the listener from the table
 * each turn because ipc_accept registers a new selector and may realloc the
 * table. */
static void iocp_service_listeners(ray_poll_t* poll)
{
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* sel = poll->sels[i];
        if (!iocp_is_listener(sel)) continue;

        int64_t id  = sel->id;
        SOCKET  lfd = (SOCKET)(uintptr_t)sel->fd;

        for (int k = 0; k < RAY_ACCEPT_DRAIN_MAX; k++) {
            WSAPOLLFD pfd;
            pfd.fd      = lfd;
            pfd.events  = POLLRDNORM;
            pfd.revents = 0;
            int pr = WSAPoll(&pfd, 1, 0);
            if (pr <= 0) break;   /* not readable / poll error → stop */
            if (!(pfd.revents & (POLLRDNORM | POLLHUP | POLLERR))) break;

            /* Re-fetch: a prior accept may have grown/moved the table. */
            if ((uint32_t)id >= poll->n_sels) break;
            ray_selector_t* ls = poll->sels[id];
            if (!iocp_is_listener(ls) || (SOCKET)(uintptr_t)ls->fd != lfd) break;
            ls->rx.read_fn(poll, ls);   /* ipc_accept: accept + register conn */
        }
    }
}

/* The interactive-console path, preserved BYTE-FOR-BYTE from C1: when a
 * RAY_SEL_STDIN selector is present (rayforce.exe run_interactive), a Windows
 * console handle cannot be watched by IOCP, but its registered read_fn blocks
 * for input, so dispatching it directly reproduces the pre-IOCP blocking
 * console REPL.  This path serves NO sockets (an interactive `-p` REPL on
 * Windows "serves IPC only after EOF" — the pre-existing, user-messaged
 * limitation, unchanged by C2). */
static int64_t iocp_run_console(ray_poll_t* poll)
{
    while (poll->code < 0) {
        ray_selector_t* sel = NULL;
        for (uint32_t i = 0; i < poll->n_sels; i++) {
            ray_selector_t* s = poll->sels[i];
            if (s && s->type == RAY_SEL_STDIN && s->rx.read_fn) { sel = s; break; }
        }
        if (!sel) return -1;

        int64_t id = sel->id;
        ray_t* obj = sel->rx.read_fn(poll, sel);

        if ((uint32_t)id >= poll->n_sels || !poll->sels[id]) continue;
        sel = poll->sels[id];

        if (obj && sel->data_fn)
            sel->data_fn(poll, sel, obj);
    }
    return poll->code;
}

/* The socket (+ C3 timer) IOCP event loop — the server serving loop. */
static int64_t iocp_run_sockets(ray_poll_t* poll)
{
    iocp_poll_t* ip = (iocp_poll_t*)poll;
    OVERLAPPED_ENTRY entries[RAY_POLL_MAX_EVENTS];

    while (poll->code < 0) {
        /* 1. Arm a readiness probe on every data socket with none in flight.
         *    Covers freshly-accepted sockets and re-arms after a drain. */
        bool has_listener = false;
        bool has_data     = false;
        for (uint32_t i = 0; i < poll->n_sels; i++) {
            ray_selector_t* s = poll->sels[i];
            if (!s) continue;
            if (iocp_is_listener(s)) { has_listener = true; continue; }
            if (iocp_is_data(s)) {
                has_data = true;
                iocp_sel_t* is = (iocp_sel_t*)s;
                if (!is->in_flight && !is->zombie) {
                    if (!iocp_arm_probe(is)) {
                        if (s->error_fn) s->error_fn(poll, s);
                        else ray_poll_deregister(poll, s->id);
                    }
                }
            }
        }

        /* Nothing serviceable (no listener, no data socket) → exit the loop,
         * mirroring C1's "-1 when there is nothing to service" and avoiding an
         * INFINITE wait on an empty port.  (A live server always has a
         * listener, so it stays here.) */
        if (!has_listener && !has_data) return -1;

        /* 2. Wait.  Cap the wait when a listener exists so a fresh connect is
         *    picked up within RAY_LISTENER_POLL_MS (Option A).  C3 will min
         *    this with the timer deadline. */
        DWORD wait_ms = has_listener ? RAY_LISTENER_POLL_MS : INFINITE;
        ULONG n = 0;
        BOOL ok = GetQueuedCompletionStatusEx(ip->port, entries,
                                              RAY_POLL_MAX_EVENTS, &n,
                                              wait_ms, FALSE);
        if (ok) {
            for (ULONG i = 0; i < n; i++) {
                iocp_sel_t* is = (iocp_sel_t*)CONTAINING_RECORD(
                    entries[i].lpOverlapped, iocp_sel_t, ov);
                is->in_flight = 0;

                if (is->zombie) {
                    /* Deregistered while this probe was outstanding; its
                     * (aborted) completion has now arrived → safe to free. */
                    iocp_zombie_remove(ip, is);
                    ray_sys_free(is);
                    continue;
                }

                /* Readiness edge on a live data socket: drain + dispatch. */
                iocp_service_readable(poll, is);
            }
        } else {
            DWORD e = GetLastError();
            if (e != WAIT_TIMEOUT) return -1;   /* real port error */
            /* Timeout → fall through to listener servicing (+ C3 timers). */
        }

        /* 3. Accept any pending connections. */
        iocp_service_listeners(poll);

        /* C3 (deferred): if (poll->timers)
         *     ray_timers_fire_expired((ray_timers_t*)poll->timers); */
    }

    return poll->code;
}

/* ===== run — dispatch on selector kind =====
 *
 * A poll carrying a RAY_SEL_STDIN selector is the interactive console
 * (rayforce.exe run_interactive) → preserve C1's blocking-console dispatch.
 * Otherwise it is a socket serving loop (q.exe / rayforce.exe headless server,
 * `-p`) → the IOCP readiness reactor.  Both launchers already call
 * ray_poll_run in their headless-server path (qmain.c server-only branch,
 * main.c server_only), so the server now ENTERS and STAYS in the loop with NO
 * frozen edit — C1 exited only because its ray_poll_run could not service
 * sockets.  The two kinds are mutually exclusive in practice on Windows: q.exe
 * never registers a stdin selector (q_repl_run_poll is #ifndef'd out), and an
 * interactive rayforce.exe console never enters the socket loop. */
int64_t ray_poll_run(ray_poll_t* poll)
{
    if (!poll) return -1;

    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* s = poll->sels[i];
        if (s && s->type == RAY_SEL_STDIN && s->rx.read_fn)
            return iocp_run_console(poll);
    }
    return iocp_run_sockets(poll);
}

#endif /* RAY_OS_WINDOWS */
