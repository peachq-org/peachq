/* q — a minimal q REPL: prompt with `q)`, parse each line to a rayforce ray_t
 * via q_parse, evaluate through the unmodified rayforce engine, print the
 * q-formatted result.  The loop itself lives in q_repl.c so the qcmd tests can
 * drive the identical console behaviour in-process.
 *
 * Phase C: `-p PORT` starts the kdb-protocol IPC listener (src/core/ipc.c) on
 * the runtime poll — `q -p 5001` is a kdb-style server.  `-u PW` / `-U PW`
 * arm the constant-time handshake auth (-U additionally restricts inbound
 * evals).  Service shape mirrors src/app/main.c: with `-p`, stdin (tty
 * console OR pipe) is registered on the SAME poll as the listener and one
 * event loop serves both CONCURRENTLY (q_repl_run_poll) — clients round-trip
 * while the REPL sits at its prompt.  stdin EOF (`</dev/null`, docker,
 * systemd) leaves the loop serving IPC only; `\\` exits.  Windows keeps the
 * serial REPL-then-serve shape until IOCP grows a stdin selector.
 *
 * Deliberately thin otherwise: no error traces, progress bar, remote session,
 * or piped bracket accumulation — those live in the (frozen) rayforce repl.c
 * and are reused later.  See ARCHITECTURE.md and the MVP design doc. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/q_repl.h"
#include "qlang/q_runtime.h"
#include "qlang/q_dotz.h"
#include "core/ipc.h"
#include "core/poll.h"
#include "core/runtime.h"
#include <rayforce.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>   /* socklen_t */
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
#endif

/* Parse a `-p` port spec.  Returns 1 → *out holds a concrete port in
 * 1..65535; 2 → the `0W` auto token (bind any OS-chosen free port); 0 →
 * invalid (non-numeric, out of range, negative, zero, or the `0N` null).
 * strtol with a full-consume + range check rejects `0N` / `abc` / `99999`
 * where the old `atoi` silently coerced them to 0 and fell through to a
 * plain REPL with no listener. */
static int parse_port_spec(const char* s, uint16_t* out) {
    if (strcmp(s, "0W") == 0 || strcmp(s, "0w") == 0) return 2;
    errno = 0;
    char* end = NULL;
    long  v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return 0;
    if (v < 1 || v > 65535) return 0;
    *out = (uint16_t)v;
    return 1;
}

/* Read the actual bound port back off a listener fd (getsockname).  The
 * `0W` auto-bind path needs this to report the OS-chosen port so a client
 * can discover it.  Returns 0 on failure. */
static uint16_t listener_bound_port(int64_t fd) {
    struct sockaddr_in sa;
    socklen_t          len = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    if (getsockname((ray_sock_t)fd, (struct sockaddr*)&sa, &len) != 0)
        return 0;
    return ntohs(sa.sin_port);
}

int main(int argc, char** argv) {
    uint16_t    port = 0;
    bool        have_port = false;
    bool        port_auto = false;
    const char* auth_pw = NULL;
    bool        auth_restricted = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "q: %s requires a port argument (1..65535 or 0W)\n", argv[i]);
                return 2;
            }
            const char* spec = argv[++i];
            int k = parse_port_spec(spec, &port);
            if (k == 0) {
                fprintf(stderr, "q: invalid port '%s' (expected 1..65535 or 0W)\n", spec);
                return 2;
            }
            have_port = true;
            port_auto = (k == 2);
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            auth_pw = argv[++i];
            auth_restricted = false;
        } else if (strcmp(argv[i], "-U") == 0 && i + 1 < argc) {
            auth_pw = argv[++i];
            auth_restricted = true;
        }
    }

    ray_runtime_t* rt = q_runtime_create(argc, argv);
    if (!rt) { fprintf(stderr, "runtime init failed\n"); return 1; }

    /* Poll owns the IPC handle namespace; publish it so `.ipc.open` /
     * listeners resolve handles the same way rayforce's main.c does. */
    ray_poll_t* poll = ray_poll_create();
    if (poll) ray_runtime_set_poll(poll);

    if (poll && auth_pw) {
        size_t pw_len = strlen(auth_pw);
        if (pw_len >= sizeof(poll->auth_secret))
            pw_len = sizeof(poll->auth_secret) - 1;
        memcpy(poll->auth_secret, auth_pw, pw_len);
        poll->auth_secret[pw_len] = '\0';
        poll->restricted = auth_restricted;
    }

    if (have_port) {
        /* `0W` (port_auto) binds port 0 → the OS chooses a free port. */
        int64_t lid = poll ? ray_ipc_listen(poll, port_auto ? 0 : port) : -1;
        int     listen_errno = errno;
        if (lid >= 0) {
            /* Report the ACTUAL bound port (read back via getsockname) so
             * a client/test can discover an auto-chosen `0W` port. */
            ray_selector_t* ls = ray_poll_get(poll, lid);
            if (ls) {
                uint16_t bound = listener_bound_port(ls->fd);
                if (bound) port = bound;
            }
            /* port is now the real listener port (>0) — the server-mode
             * checks below key off it identically for fixed and 0W. */
            fprintf(stderr, "listening on port %u\n", port);
        } else {
            /* Strict: an unusable port (invalid, EADDRINUSE, EACCES, or
             * any bind/listen failure) must NOT fall through into a
             * silent ray_poll_run with nothing registered (a hang) or a
             * plain REPL — exit non-zero so a supervisor flags it. */
            if (port_auto)
                fprintf(stderr, "q: failed to bind a free port: %s\n",
                        strerror(listen_errno));
            else
                fprintf(stderr, "q: failed to listen on port %u: %s\n",
                        port, strerror(listen_errno));
            if (poll) {
                ray_runtime_set_poll(NULL);
                ray_poll_destroy(poll);
            }
            q_runtime_destroy(rt);
            return 2;
        }
    }

    int stdin_tty = isatty(STDIN_FILENO);

    /* Startup banner (kdb-style version + build date, via the same macros as
     * .z.K/.z.k).  GUARDRAIL: print ONLY on an interactive tty REPL and NOT
     * under `-q` (.z.q).  Piped/redirected stdin (the qcmd/qscript runners,
     * `printf … | ./q`, `</dev/null` daemons) is NOT a tty, so it never leaks
     * into an equality golden; the qdoctest runner is a separate binary that
     * never reaches this path. */
    if (stdin_tty && !q_dotz_quiet()) {
#ifdef RAYFORCE_BUILD_DATE
        const char* build = RAYFORCE_BUILD_DATE;
#else
        const char* build = "";
#endif
        printf("openq %d.%d %s\n", RAY_VERSION_MAJOR, RAY_VERSION_MINOR, build);
        fflush(stdout);
    }

    /* Startup script (`q file.q`): run it before the REPL / server loop.
     * kdb semantics — the script executes first; then a tty drops to the
     * REPL, a `-p` server serves IPC, and a non-tty non-server run exits 0
     * (the test/daemon shape) rather than blocking on an empty REPL. */
    const char* script = q_dotz_script_path();
    int script_rc = 0;
    if (script)
        script_rc = q_repl_run_file(script, stdout, stderr);

    if (script_rc != 0) {
        /* Startup script could not be opened/read — skip the REPL/server loop
         * and exit non-zero (kdb fails a bad `q file.q`; it must not silently
         * succeed).  q_repl_run_file already printed the open error. */
    } else if ((port > 0 || q_repl_listener_active()) && poll) {
        /* A listener exists — from startup `-p` (port>0) OR a runtime/script
         * `\p N` (q_repl_listener_active) — so serve, don't exit at a non-tty
         * script end.  Live listener + console: register stdin on the SAME poll
         * IPC listener and run ONE event loop (q_repl_run_poll — mirrors
         * rayforce's run_interactive), so clients are served WHILE the REPL
         * reads — no EOF needed.  Covers both the tty console and piped
         * stdin; on stdin EOF the loop keeps serving (the `</dev/null`
         * daemon shape), on `\\`/`exit` it exits.  SIGPIPE ignored so a
         * broken client/log pipe can't kill the server (writes fail with
         * EPIPE instead). */
        int concurrent = 0;
#ifndef RAY_OS_WINDOWS
        signal(SIGPIPE, SIG_IGN);
        concurrent = (q_repl_run_poll(poll, stdout, stderr, stdin_tty,
                                      /*have_listener=*/1) == 0);
#endif
        if (!concurrent) {
            /* Fallback: Windows (no IOCP stdin selector yet) or a stdin the
             * poll backend cannot watch (e.g. a regular-file redirect under
             * epoll).  Legacy serial shape. */
            if (stdin_tty) {
                fprintf(stderr, "note: -p with an interactive REPL serves "
                                "IPC only after EOF on this platform\n");
                q_repl_run(stdin, stdout, stderr, 0);
            } else {
                fprintf(stderr, "no terminal — running in server-only mode\n");
            }
            ray_poll_run(poll);
        }
    } else if (script && !stdin_tty) {
        /* Ran a startup script with no server on a non-tty (`q file.q
         * </dev/null`): exit 0 without entering the REPL. */
    } else {
        /* No listener: run the SAME poll event loop the server uses (Bundle 3
         * — unify on rayforce's run_interactive shape) so an idle client
         * services its open outbound IPC conns (server pushes dispatch the
         * instant they arrive, not only during a sync-send) and a runtime `\p`
         * joins this poll.  have_listener=0 → the loop exits at stdin EOF, so a
         * piped client still finishes.  Fall back to the blocking console where
         * the poll backend can't watch stdin (regular-file redirect, Windows). */
        int concurrent = 0;
        if (poll)
            concurrent = (q_repl_run_poll(poll, stdout, stderr, stdin_tty,
                                          /*have_listener=*/0) == 0);
        if (!concurrent)
            q_repl_run(stdin, stdout, stderr, !stdin_tty);
    }

    if (poll) {
        ray_runtime_set_poll(NULL);
        ray_poll_destroy(poll);
    }
    q_runtime_destroy(rt);
    return script_rc;
}
