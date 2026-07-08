/* q — a minimal q REPL: prompt with `q)`, parse each line to a rayforce ray_t
 * via q_parse, evaluate through the unmodified rayforce engine, print the
 * q-formatted result.  The loop itself lives in q_repl.c so the qcmd tests can
 * drive the identical console behaviour in-process.
 *
 * Phase C: `-p PORT` starts the kdb-protocol IPC listener (src/core/ipc.c) on
 * the runtime poll — `q -p 5001` is a kdb-style server.  `-u PW` / `-U PW`
 * arm the constant-time handshake auth (-U additionally restricts inbound
 * evals).  Service shape mirrors src/app/main.c: with `-p` and stdin NOT a
 * tty (daemon/harness: `</dev/null`, docker, systemd) the REPL is skipped and
 * the poll loop runs directly.  With a tty the REPL runs first and IPC is
 * served only after EOF — a documented Phase C limitation (event-loop REPL
 * integration is future work).
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
    } else if (port > 0 && poll && !stdin_tty) {
        /* Server-only: daemon/harness shape (`</dev/null`, docker,
         * systemd).  SIGPIPE ignored so a broken log pipe can't kill the
         * daemon (writes fail with EPIPE instead). */
#ifndef RAY_OS_WINDOWS
        signal(SIGPIPE, SIG_IGN);
#endif
        fprintf(stderr, "no terminal — running in server-only mode\n");
        ray_poll_run(poll);
    } else if (script && !stdin_tty) {
        /* Ran a startup script with no server on a non-tty (`q file.q
         * </dev/null`): exit 0 without entering the REPL. */
    } else {
        if (port > 0 && stdin_tty)
            fprintf(stderr, "note: -p with an interactive REPL serves IPC "
                            "only after EOF (event-loop REPL integration "
                            "is future work)\n");
        /* Echo input when stdin is not a terminal (piped / redirected) so
         * the output is a faithful console transcript; on a real tty the
         * terminal echoes, so we don't. */
        q_repl_run(stdin, stdout, stderr, !stdin_tty);
        if (port > 0 && poll)
            ray_poll_run(poll);
    }

    if (poll) {
        ray_runtime_set_poll(NULL);
        ray_poll_destroy(poll);
    }
    q_runtime_destroy(rt);
    return script_rc;
}
