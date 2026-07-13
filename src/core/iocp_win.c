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
 *   C2 — real completion loop: GetQueuedCompletionStatusEx drain,
 *       zero-byte WSARecv readiness probes for data sockets, bounded WSAPoll
 *       listener probe (Option A, zero frozen edits), per-op OVERLAPPED
 *       lifetime + teardown-race handling.  Turns on the server half
 *       (`.z.pg/.z.ps/.z.pw` dispatch runs through the frozen ipc.c read_fn
 *       state machine that this loop drives — identical to epoll.c).
 *   STDIN SELECTOR (THIS FILE) — RAY_SEL_STDIN fires like any readiness edge,
 *       so ONE loop serves console + sockets concurrently (the same contract
 *       epoll/kqueue deliver on POSIX; retires the C1 blocking-console
 *       special case).  Console handles cannot attach to an IOCP, so a
 *       watcher thread bridges them: WaitForSingleObject(hStdin) →
 *       PostQueuedCompletionStatus → block on an ack from the main loop.
 *       The watcher NEVER reads bytes — the registered read_fn (the
 *       ray_term line editor / the piped-line accumulator) keeps sole
 *       ownership of stdin; the thread only converts "input pending" into a
 *       completion packet (the canonical IOCP console bridge — no single
 *       syscall waits on console + sockets in one IOCP wait).  Piped stdin
 *       needs no thread: PeekNamedPipe piggybacks on the bounded wait the
 *       listener probe already imposes.  Other stdin kinds (disk redirect,
 *       NUL) are level-readable until EOF and are dispatched every turn.
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
#include <io.h>         /* _get_osfhandle — CRT stdin fd → HANDLE */

#define RAY_POLL_INITIAL_CAP  16
#define RAY_POLL_MAX_EVENTS    64
#define RAY_LISTENER_POLL_MS   50   /* accept-latency cap (Option A, §4.3) */
#define RAY_ACCEPT_DRAIN_MAX  128   /* bound per-turn accepts (burst safety) */

/* Completion key for the stdin watcher's wake packet.  Socket completions are
 * recovered via CONTAINING_RECORD on lpOverlapped (their key is the selector
 * id, always < n_sels); ~0 can never collide with a selector id. */
#define IOCP_STDIN_WAKE_KEY  ((ULONG_PTR)~(ULONG_PTR)0)

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

    /* Console-stdin watcher bridge (see the header comment).  The events and
     * this struct outlive the thread: the thread is joined before anything it
     * references is freed (deregister/destroy), and a stale wake packet left
     * in the port after a join is harmless (serviced against the selector
     * table, which no longer holds a stdin selector). */
    HANDLE       stdin_h;      /* the console handle the watcher waits on */
    HANDLE       stdin_thread; /* NULL when no watcher is running */
    HANDLE       stdin_ack;    /* auto-reset: main → watcher "edge consumed" */
    HANDLE       stdin_stop;   /* manual-reset: tear the watcher down */
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
/* Join the console-stdin watcher thread (defined with the stdin arm below). */
static void iocp_stdin_watcher_stop(iocp_poll_t* ip);

void ray_poll_destroy(ray_poll_t* poll)
{
    if (!poll) return;
    iocp_poll_t* ip = (iocp_poll_t*)poll;

    /* Stop the stdin watcher FIRST — it posts into the port this teardown is
     * about to drain and close, and it dereferences ip. */
    iocp_stdin_watcher_stop(ip);

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
                /* A stale stdin wake packet (posted by the watcher before it
                 * was stopped above) is NOT a probe completion: its
                 * lpOverlapped is NULL, so CONTAINING_RECORD on it would
                 * fabricate a wild pointer.  Skip it. */
                if (entries[i].lpCompletionKey == IOCP_STDIN_WAKE_KEY ||
                    !entries[i].lpOverlapped)
                    continue;
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
    if (ip->stdin_ack)  CloseHandle(ip->stdin_ack);
    if (ip->stdin_stop) CloseHandle(ip->stdin_stop);
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
     * to an IOCP; the reactor delivers their readiness edges via the stdin
     * arm (console watcher thread / PeekNamedPipe — see iocp_run_reactor). */
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

    /* Console stdin going away (EOF → daemon mode, or teardown): join its
     * watcher.  Safe re-entrantly — when this runs from inside the stdin
     * service dispatch, the watcher is parked on the ack wait, so the stop
     * event releases it immediately. */
    if (sel->type == RAY_SEL_STDIN)
        iocp_stdin_watcher_stop(ip);

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

/* ===== stdin readiness arm ===== */

/* How a RAY_SEL_STDIN selector gets its readiness edges. */
typedef enum {
    IOCP_STDIN_CONSOLE,   /* real console: watcher thread posts wake packets */
    IOCP_STDIN_PIPE,      /* anonymous/named pipe: PeekNamedPipe on the bounded wait */
    IOCP_STDIN_ALWAYS,    /* disk redirect / NUL / unknown: level-readable until
                           * EOF — read() returns instantly, so dispatch every
                           * turn and let the read_fn's own EOF path deregister */
} iocp_stdin_kind_t;

static iocp_stdin_kind_t iocp_stdin_classify(HANDLE h) {
    DWORD mode;
    if (GetConsoleMode(h, &mode)) return IOCP_STDIN_CONSOLE;
    if (GetFileType(h) == FILE_TYPE_PIPE) return IOCP_STDIN_PIPE;
    return IOCP_STDIN_ALWAYS;
}

/* A record is inert when it can never produce a byte for a raw-mode
 * ReadFile: non-key events (mouse/focus/resize), key-UPs, and key-DOWNs of
 * pure modifier/lock keys (Shift alone, Ctrl alone, …).  Everything else —
 * any key-down with a char, and char-less key-downs like arrows/F-keys that
 * the VT input mode translates into ESC sequences — counts as byte-producing
 * and must NOT be discarded (eating an arrow key would break line editing).
 * Modifier state lives in each record's own dwControlKeyState/uChar, already
 * baked in at input time, so discarding a bare Ctrl-down record cannot
 * corrupt a later Ctrl+C record. */
static bool iocp_key_record_inert(const INPUT_RECORD* r) {
    if (r->EventType != KEY_EVENT) return true;
    if (!r->Event.KeyEvent.bKeyDown) return true;
    if (r->Event.KeyEvent.uChar.UnicodeChar != 0) return false;
    switch (r->Event.KeyEvent.wVirtualKeyCode) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:      /* Alt */
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
    case VK_LWIN: case VK_RWIN:
        return true;    /* pure modifier / lock keys never yield bytes */
    default:
        return false;   /* arrows, F-keys, Home/End… → VT escape bytes */
    }
}

/* A signaled console handle does NOT mean ReadFile won't block: inert
 * INPUT_RECORDs (see above) signal the handle but produce no bytes, and a
 * raw-mode ReadFile skips them and then BLOCKS waiting for a real keystroke —
 * which would stall the whole reactor.  Before waking the main loop, discard
 * leading inert records (ReadFile ignores them anyway) and report whether
 * byte-producing input remains.
 * Concurrency: the watcher only touches the console BETWEEN an ack and the
 * next wake post, and the main loop only reads BETWEEN a post and the ack —
 * the protocol serializes all console access. */
static bool iocp_console_has_bytes(HANDLE h) {
    for (;;) {
        DWORD pending = 0;
        if (!GetNumberOfConsoleInputEvents(h, &pending))
            return true;    /* not peekable — let the read_fn decide */
        if (pending == 0)
            return false;   /* buffer drained; handle no longer signaled */
        INPUT_RECORD recs[32];
        DWORD want = pending < 32 ? pending : 32;
        DWORD got  = 0;
        if (!PeekConsoleInput(h, recs, want, &got) || got == 0)
            return true;
        for (DWORD i = 0; i < got; i++) {
            if (!iocp_key_record_inert(&recs[i]))
                return true;
        }
        /* Every peeked record is inert — consume and re-check. */
        if (!ReadConsoleInput(h, recs, got, &got))
            return true;
    }
}

/* Watcher thread: convert "console input pending" into one completion packet,
 * then block until the main loop has consumed the edge (the ack) so input is
 * never raced and at most one wake packet is ever outstanding.  Bytes are
 * NEVER read here — the registered read_fn owns stdin. */
static DWORD WINAPI iocp_stdin_watcher(LPVOID arg) {
    iocp_poll_t* ip = (iocp_poll_t*)arg;
    HANDLE waits[2] = { ip->stdin_stop, ip->stdin_h };
    HANDLE acks[2]  = { ip->stdin_stop, ip->stdin_ack };
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0 + 1)
            return 0;                     /* stop signal (or wait error) */
        if (!iocp_console_has_bytes(ip->stdin_h))
            continue;                     /* inert wakeup consumed */
        PostQueuedCompletionStatus(ip->port, 0, IOCP_STDIN_WAKE_KEY, NULL);
        w = WaitForMultipleObjects(2, acks, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0 + 1)
            return 0;
    }
}

/* Start the watcher lazily (first loop turn that sees a console stdin
 * selector).  Returns false on failure — the caller falls back to treating
 * the console as always-readable (blocking dispatch: console works, sockets
 * starve between keystrokes — strictly no worse than the retired C1 path). */
static bool iocp_stdin_watcher_start(iocp_poll_t* ip, HANDLE h) {
    if (ip->stdin_thread) return true;
    ip->stdin_h = h;
    if (!ip->stdin_ack)  ip->stdin_ack  = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!ip->stdin_stop) ip->stdin_stop = CreateEventW(NULL, TRUE,  FALSE, NULL);
    if (!ip->stdin_ack || !ip->stdin_stop) return false;
    ResetEvent(ip->stdin_stop);
    ip->stdin_thread = CreateThread(NULL, 64 * 1024, iocp_stdin_watcher, ip, 0, NULL);
    return ip->stdin_thread != NULL;
}

/* Join the watcher (idempotent).  Called when the stdin selector is
 * deregistered (EOF → daemon mode) and at poll destroy.  The thread waits on
 * {stop, hStdin} or {stop, ack}, so the stop event always releases it; the
 * events stay alive (per-poll) so a concurrent SetEvent(ack) from the main
 * loop can never touch a closed handle. */
static void iocp_stdin_watcher_stop(iocp_poll_t* ip) {
    if (!ip->stdin_thread) return;
    SetEvent(ip->stdin_stop);
    WaitForSingleObject(ip->stdin_thread, INFINITE);
    CloseHandle(ip->stdin_thread);
    ip->stdin_thread = NULL;
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

/* Find the live stdin selector (first RAY_SEL_STDIN with a read_fn), or NULL.
 * Re-scanned every turn — any callback may deregister it (stdin EOF in
 * daemon mode). */
static ray_selector_t* iocp_find_stdin(ray_poll_t* poll)
{
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* s = poll->sels[i];
        if (s && s->type == RAY_SEL_STDIN && s->rx.read_fn)
            return s;
    }
    return NULL;
}

/* The unified IOCP event loop: sockets + stdin (+ C3 timers) in ONE reactor —
 * the same single loop epoll/kqueue give POSIX, so an interactive `-p`
 * console answers IPC clients between keystrokes (retires the C1 blocking-
 * console special case and its "serves IPC only after EOF" limitation). */
static int64_t iocp_run_reactor(ray_poll_t* poll)
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

        /* 2. Arm the stdin readiness edge.  Console → watcher thread (posts a
         *    wake packet; nothing to do per turn beyond starting it).  Pipe →
         *    PeekNamedPipe now, and bound the wait so new bytes are seen
         *    within RAY_LISTENER_POLL_MS.  Always-readable kinds dispatch
         *    with a zero wait until their read_fn hits EOF and deregisters. */
        ray_selector_t* sin = iocp_find_stdin(poll);
        bool stdin_ready   = false;   /* dispatch this turn without a packet */
        bool stdin_polled  = false;   /* pipe kind: bound the wait */
        if (sin) {
            HANDLE h = (HANDLE)_get_osfhandle((int)sin->fd);
            if (h == INVALID_HANDLE_VALUE) {
                stdin_ready = true;   /* let the read_fn surface the error/EOF */
            } else switch (iocp_stdin_classify(h)) {
            case IOCP_STDIN_CONSOLE:
                if (!iocp_stdin_watcher_start(ip, h))
                    stdin_ready = true;   /* fallback: blocking dispatch */
                break;
            case IOCP_STDIN_PIPE: {
                DWORD avail = 0;
                stdin_polled = true;
                if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
                    stdin_ready = true;   /* broken pipe → read() 0 → EOF path */
                else if (avail > 0)
                    stdin_ready = true;
                break;
            }
            case IOCP_STDIN_ALWAYS:
                stdin_ready = true;
                break;
            }
        }

        /* Nothing serviceable (no listener, no data socket, no stdin) → exit
         * the loop, mirroring C1's "-1 when there is nothing to service" and
         * avoiding an INFINITE wait on an empty port.  (A live server always
         * has a listener, so it stays here.) */
        if (!has_listener && !has_data && !sin) return -1;

        /* 3. Wait.  Zero when stdin is already readable (drain any socket
         *    completions, then service it); bounded when a listener needs its
         *    WSAPoll probe or a pipe-stdin needs re-peeking; else INFINITE —
         *    a console wake packet or a socket completion ends the wait.  C3
         *    will min this with the timer deadline. */
        DWORD wait_ms;
        if (stdin_ready)                       wait_ms = 0;
        else if (has_listener || stdin_polled) wait_ms = RAY_LISTENER_POLL_MS;
        else                                   wait_ms = INFINITE;

        bool stdin_wake = false;   /* console watcher packet drained this turn */
        ULONG n = 0;
        BOOL ok = GetQueuedCompletionStatusEx(ip->port, entries,
                                              RAY_POLL_MAX_EVENTS, &n,
                                              wait_ms, FALSE);
        if (ok) {
            for (ULONG i = 0; i < n; i++) {
                if (entries[i].lpCompletionKey == IOCP_STDIN_WAKE_KEY) {
                    /* Watcher wake packet (lpOverlapped is NULL — it MUST be
                     * checked before CONTAINING_RECORD).  Service after the
                     * drain so a same-batch socket completion isn't starved
                     * by a long console eval. */
                    stdin_wake = true;
                    continue;
                }

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
            /* Timeout → fall through to stdin/listener servicing (+ C3). */
        }

        /* 4. Service stdin: exactly one read_fn dispatch per readiness edge
         *    (one keystroke byte / one pipe chunk — level-triggered, matching
         *    the POSIX loop: the console handle stays signaled while more
         *    input pends, so the watcher immediately re-posts; the pipe is
         *    re-peeked next turn).  Re-scan: a socket callback above may have
         *    deregistered stdin. */
        if (stdin_wake || stdin_ready) {
            sin = iocp_find_stdin(poll);
            if (sin)
                iocp_service_readable(poll, (iocp_sel_t*)sin);
        }
        if (stdin_wake)
            SetEvent(ip->stdin_ack);   /* edge consumed — watcher waits again
                                        * (harmless no-op if the watcher was
                                        * stopped by a deregister just now) */

        /* 5. Accept any pending connections. */
        iocp_service_listeners(poll);

        /* C3 (deferred): if (poll->timers)
         *     ray_timers_fire_expired((ray_timers_t*)poll->timers); */
    }

    return poll->code;
}

/* One loop, all selector kinds — the epoll/kqueue contract.  Both launchers
 * (qmain.c, main.c) reach this via their server AND console paths; the stdin
 * arm above lets a q_repl_run_poll/run_interactive console share the loop
 * with the IPC listener, so clients round-trip while the prompt sits idle. */
int64_t ray_poll_run(ray_poll_t* poll)
{
    if (!poll) return -1;
    return iocp_run_reactor(poll);
}

#endif /* RAY_OS_WINDOWS */
