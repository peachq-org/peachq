/* q — the launcher: arg parse, runtime/poll/listener bring-up, `-eval-before` texts, the startup script, `-eval`
 * texts, then REPL / server / exit.  Flags are documented in user-docs/cmdline.md; q_dotz.c's option table
 * (q_dotz_opt) is what q consumes from argv.  The interactive loop lives in q_repl.c. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/q_count.h"
#include "qlang/repl/q_repl.h"
#include "qlang/q_ctx.h"   /* q_ctx_run_file/q_ctx_run_src — the non-tty script and -eval doors */
#include "qlang/q_runtime.h"
#include "qlang/q_dotz.h"
#include "qlang/ops/q_sys.h"     /* q_sys_listen_spec_parse / q_sys_listen — the `-p` spec, shared with `\p` */
#include "qlang/q_console.h"  /* q_console_pipe_enable — the modern pipe-table display; q_console_color */
#include "qlang/net/q_tls.h"  /* q_tls_server_mode_set — the `-E` TLS server mode */
#include "qlang/io/q_duckdb.h" /* q_duckdb_main_path_set — the `-duckdb` main database file */
#include "qlang/io/q_io.h"     /* q_io_abs_path — QINIT's startup `\l`; q_io_read_slice — the -conn file */
#include "qlang/io/q_worker.h" /* q_worker_link — a :pq:qspawn: worker's link to the process that launched it */
#include "qlang/q_env.h"       /* q_env_set — the -conn texts bound as q values */
#include "qlang/base/q_err.h"  /* q_err_drop — an unreadable -conn file */
#include "core/poll.h"
#include "core/ipc.h"          /* ray_ipc_auth_file_load — the `-u`/`-U` password file */
#include "core/runtime.h"
#include <rayforce.h>
#include <errno.h>
#include <limits.h>      /* PATH_MAX — the startup `\l` line */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LISTEN_SPEC_FORMS "[rp,][host:](port|0W|lo/hi|servicename), port 1..65535"

/* ---- `-conn`: the remote runner (owner ruling 2026-09-20) -------------------------------------------------------
 * A MODE with no flag in common with the local launcher: C parses argv, boots the runtime, loads the standard
 * library and composes CALLS to .pq.i.conn_* through the script seam; every display, history, error shape and exit
 * is q's (lib/pq.q).  The texts (the file, each -eval, each stdin line) are bound as q values rather than spliced
 * into source, so no escaping happens here.  Exit codes: 2 bad arguments or no connection, 1 a remote error (the
 * q side `exit`s, as a failing script would), 0 otherwise. */
static void conn_bind(const char* name, const char* s, size_t n) {
    ray_t* v = ray_charv(s, (int64_t)n);
    q_env_set(ray_sym_intern_runtime(name, strlen(name)), v);
    ray_release(v);
}

/* One text as one .pq.i.conn_call: two statements, the display and then the notice. */
static int conn_call(const char* src, size_t n, const char* save, int last) {
    conn_bind(".pq.i.conn_src", src, n);
    conn_bind(".pq.i.conn_file", save ? save : "", save ? strlen(save) : 0);
    const char* call = last ? ".pq.i.conn_p:.pq.i.conn_call[.pq.i.conn_h;.pq.i.conn_src;.pq.i.conn_file;1b]\n"
                              ".pq.i.conn_notice .pq.i.conn_p"
                            : ".pq.i.conn_p:.pq.i.conn_call[.pq.i.conn_h;.pq.i.conn_src;.pq.i.conn_file;0b]\n"
                              ".pq.i.conn_notice .pq.i.conn_p";
    return q_ctx_run_src(call, stdout, stderr, NULL) ? 1 : 0;
}

/* The gathered texts: the file's bytes (owned), then argv pointers, then stdin lines (owned copies). */
typedef struct { const char** p; size_t* len; size_t n, cap, nstd; ray_t* file; } conn_texts;

static void conn_texts_free(conn_texts* t) {
    for (size_t i = t->n - t->nstd; i < t->n; i++) free((char*)t->p[i]);
    free(t->p);
    free(t->len);
    if (t->file) ray_release(t->file);
}

static int conn_main(int argc, char** argv) {
    const char* target = NULL;
    const char* script = NULL;
    const char* save   = NULL;
    bool        ls     = false;
    int         n_eval = 0;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        int value = strcmp(a, "-conn") == 0 || strcmp(a, "-eval") == 0 || strcmp(a, "-save") == 0;
        if (value && i + 1 >= argc) { fprintf(stderr, "q: %s requires an argument\n", a); return 2; }
        if      (strcmp(a, "-conn") == 0) target = argv[++i];    /* the mode switch: never a value here */
        else if (strcmp(a, "-eval") == 0) { n_eval++; i++; }
        else if (strcmp(a, "-save") == 0) save = argv[++i];
        else if (strcmp(a, "-ls") == 0)   ls = true;
        else if (a[0] == '-') { fprintf(stderr, "q: %s is not valid with -conn\n", a); return 2; }
        else if (!script && strlen(a) > 2 && strcmp(a + strlen(a) - 2, ".q") == 0) script = a;
        else { fprintf(stderr, "q: unexpected argument '%s' with -conn\n", a); return 2; }
    }
    if (target[0] == '`') target++;               /* the rest is hopen's, exactly as written (owner ruling) */
    int stdin_tty = isatty(STDIN_FILENO);
    if (!script && !n_eval && !ls && stdin_tty) { fprintf(stderr, "q: -conn needs a file, -eval, -ls or piped stdin\n"); return 2; }

    /* The texts in the local process's order — the file's whole text, each -eval, then each stdin line — gathered
     * first because `-save` takes the LAST one's value.  A blank stdin line runs nothing, as at the console. */
    conn_texts t = { calloc((size_t)argc + 1, sizeof *t.p), calloc((size_t)argc + 1, sizeof *t.len),
                     0, (size_t)argc + 1, 0, NULL };
    ray_runtime_t* rt = NULL;
    int            rc = 0;
    if (!t.p || !t.len) { fprintf(stderr, "q: out of memory\n"); rc = 1; goto done; }
    if (script) {
        ray_t* pathv = ray_str(script, strlen(script));
        t.file = pathv ? q_io_read_slice(pathv, 0, -1, NULL) : NULL;
        if (pathv) ray_release(pathv);
        if (!t.file || RAY_IS_ERR(t.file)) {
            if (t.file) { q_err_drop(); ray_error_free(t.file); t.file = NULL; }
            fprintf(stderr, "q: cannot open script '%s'\n", script);
            rc = 2;
            goto done;
        }
        t.p[t.n] = (const char*)ray_data(t.file);
        t.len[t.n++] = (size_t)q_count(t.file);
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-eval") == 0) { t.p[t.n] = argv[++i]; t.len[t.n++] = strlen(argv[i]); }
        else if (strcmp(argv[i], "-conn") == 0 || strcmp(argv[i], "-save") == 0) i++;
    }
    char line[4096];                          /* the piped console's line buffer, q_repl.c */
    while (!stdin_tty && fgets(line, sizeof line, stdin)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (strspn(line, " \t") == len) continue;
        if (t.n == t.cap) {
            const char** np = realloc(t.p, 2 * t.cap * sizeof *t.p);
            size_t*      nl = realloc(t.len, 2 * t.cap * sizeof *t.len);
            if (np) t.p = np;
            if (nl) t.len = nl;
            if (!np || !nl) { fprintf(stderr, "q: out of memory\n"); rc = 1; goto done; }
            t.cap *= 2;
        }
        t.p[t.n] = strdup(line);
        t.len[t.n++] = len;
        t.nstd++;
    }
    if (save && !t.n) { fprintf(stderr, "q: -save needs a query to save\n"); rc = 2; goto done; }

    rt = q_runtime_create(argc, argv);
    if (!rt) { fprintf(stderr, "runtime init failed\n"); rc = 1; goto done; }
    q_sys_own_process(true);
    ray_poll_t* poll = ray_poll_create();
    if (poll) ray_runtime_set_poll(poll);
    q_console_pipe_enable();                  /* the local console's display, as `q` itself shows a table */
    q_console_clip_set(25, isatty(STDOUT_FILENO) ? NULL_I64 : 2000);   /* a pipe gets every column */
    const char* qh = getenv("PEACHQ_QHIST");  /* set-but-empty is the off switch q cannot see through getenv */
    if (q_ctx_run_src("\\l pq", stdout, stderr, NULL) ||
        (qh && !*qh && q_ctx_run_src(".pq.i.QHIST:\"\"", stdout, stderr, NULL))) { rc = 1; goto done; }
    conn_bind(".pq.i.conn_target", target, strlen(target));
    if (q_ctx_run_src(".pq.i.conn_h:.pq.i.conn_open .pq.i.conn_target", stdout, stderr, NULL)) { rc = 2; goto done; }

    for (size_t i = 0; i < t.n && !rc; i++)
        rc = conn_call(t.p[i], t.len[i], save, i + 1 == t.n);
    if (!rc && ls) rc = q_ctx_run_src(".pq.i.conn_ls .pq.i.conn_h", stdout, stderr, NULL) ? 1 : 0;
    q_ctx_run_src("hclose .pq.i.conn_h", stdout, stderr, NULL);
done:
    conn_texts_free(&t);
    if (rt) q_sys_exit(rc);                   /* the exit home once a runtime exists: .z.exit fires, as `q` itself */
    return rc;
}

/* The option at argv[i], applied through its `\` command; false once reported, when that command refuses it. */
static bool option_apply(char** argv, int i) {
    const q_dotz_opt_t* o = q_dotz_opt(argv[i]);
    size_t n = 2 + strlen(o->cmd);
    for (int k = 1; k <= o->nparam; k++) n += 1 + strlen(argv[i + k]);
    char* line = malloc(n);
    if (!line) { fprintf(stderr, "q: out of memory\n"); return false; }
    int at = snprintf(line, n, "\\%s", o->cmd);
    for (int k = 1; k <= o->nparam; k++) at += snprintf(line + at, n - (size_t)at, " %s", argv[i + k]);
    ray_t* r = q_sys_run(line, (size_t)at);
    free(line);
    if (r && RAY_IS_ERR(r)) {
        q_err_drop();
        ray_error_free(r);
        fprintf(stderr, "q: invalid %s value '%s'\n", argv[i], argv[i + 1]);
        return false;
    }
    if (r) ray_release(r);
    return true;
}

int main(int argc, char** argv) {
    int link = q_worker_link();
    if (q_dotz_has_flag(argc, argv, "-conn")) return conn_main(argc, argv);

    const char* script = q_dotz_file_arg(argc, argv);
    const char* port_spec = NULL;
    bool        classic = false;
    const char* auth_file = NULL;
    bool        auth_restricted = false;
    int         tls_mode = 0;
    bool        want_help = false;
    const char* duckdb_main = getenv("PEACHQ_DUCKDB_MAIN");

    for (int i = script ? 2 : 1; i < argc; i++) {
        const char*         a = argv[i];
        const q_dotz_opt_t* o = q_dotz_opt(a);
        if (!o) {
            want_help |= strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0;
            continue;
        }
        if (i + o->nparam >= argc) {
            fprintf(stderr, "q: %s needs %d parameter%s\n", a, o->nparam, o->nparam == 1 ? "" : "s");
            return 2;
        }
        const char* v = o->nparam ? argv[i + 1] : NULL;
        if (o->values && (strlen(v) != 1 || !strchr(o->values, v[0]))) {
            fprintf(stderr, "q: invalid %s value '%s' (expected ", a, v);
            for (const char* c = o->values; *c; c++) fprintf(stderr, "%s%c", c == o->values ? "" : "|", *c);
            fprintf(stderr, ")\n");
            return 2;
        }
        if (strcmp(a, "-p") == 0 || strcmp(a, "--port") == 0) port_spec = v;
        else if (strcmp(a, "-E") == 0) tls_mode = v[0] - '0';
        else if (strcmp(a, "-classic") == 0) classic = true;
        else if (strcmp(a, "-duckdb") == 0) q_duckdb_main_path_set(duckdb_main = v);
        else if (strcmp(a, "-U") == 0) auth_file = v;
        else if (strcmp(a, "-u") == 0) {
            /* basics/cmdline.md: `-u 1` restricts; `-u file` is `-u 1 -U file`. */
            auth_restricted = true;
            if (strcmp(v, "1") != 0) auth_file = v;
        }
        i += o->nparam;
    }

    if (auth_file && ray_ipc_auth_file_load(auth_file) != 0) {
        fprintf(stderr, "q: cannot read password file '%s': %s\n", auth_file, strerror(errno));
        return 2;
    }

    /* Allocated after the argument checks above so their error returns need no cleanup.  `-eval-before` / `-eval`
     * texts, each list in argv order; order between the lists is by FLAG (before the startup script / after it),
     * never by argv position.  argc bounds the counts; `startup` adds the tty console's two `\l`s and its NULL. */
    const char** eval_before = calloc((size_t)argc, sizeof *eval_before);
    const char** eval_after  = calloc((size_t)argc, sizeof *eval_after);
    int*         applied     = calloc((size_t)argc, sizeof *applied);
    const char** startup     = calloc((size_t)argc + 3, sizeof *startup);
    int          n_before = 0, n_after = 0, n_applied = 0;
    if (!eval_before || !eval_after || !applied || !startup) { fprintf(stderr, "q: out of memory\n"); return 1; }
    for (int i = script ? 2 : 1; i < argc; i++) {
        const q_dotz_opt_t* o = q_dotz_opt(argv[i]);
        if (!o) continue;
        if (o->cmd) applied[n_applied++] = i;
        else if (strcmp(argv[i], "-eval") == 0) eval_after[n_after++] = argv[i + 1];
        else if (strcmp(argv[i], "-eval-before") == 0) eval_before[n_before++] = argv[i + 1];
        i += o->nparam;
    }

    q_tls_server_mode_set(tls_mode);   /* before any listener can accept */

    ray_runtime_t* rt = q_runtime_create(argc, argv);
    if (!rt) { fprintf(stderr, "runtime init failed\n"); return 1; }
    q_sys_own_process(true);   /* the real q binary: `\\`/`exit x` exit, `\cmd` shells */

    /* Poll owns the IPC handle namespace; publish it so `.ipc.open` /
     * listeners resolve handles the same way rayforce's main.c does. */
    ray_poll_t* poll = ray_poll_create();
    if (poll) ray_runtime_set_poll(poll);

    if (poll) poll->restricted = auth_restricted;

    /* a worker is never a console, even where its NUL stdin reads as a character device (Windows) */
    int stdin_tty = link < 0 && isatty(STDIN_FILENO);
    /* `QINIT` names a file loaded after init, before any script (basics/by-topic.md): a startup load like the script —
     * batch ahead of `-eval-before` on a non-tty, the console's first `\l` on a tty.  Empty is unset; there is no
     * `$QHOME/q.q` default (#60). */
    const char* qinit = getenv("QINIT");
    if (qinit && !*qinit) qinit = NULL;

    /* Modern mode (the default): switch the console to the pipe-table display
     * and auto-fit the console width (`\c 25 0N` — 0N re-resolves to the live
     * terminal at each render, so a resize follows).  The stdlib is NOT
     * auto-loaded (owner 2026-08-14, startup cost): `\l pq` is explicit.
     * `-classic` skips both (kdb-clean env, legacy display, kdb `\c 25 80`).
     * The non-tty script batch widen below still overrides. */
    if (!classic) {
        q_console_pipe_enable();
        q_console_clip_set(25, NULL_I64);
    }

    /* `\c` console-size DISPLAY clipping is ARMED BY DEFAULT (q_sys_cfg_init)
     * so a fresh interactive tty REPL and a piped `printf … | ./q` (no script,
     * no -eval) truncate at
     * the 25 80 default (kdb-true).  The ONE carve-out: a non-tty SCRIPT
     * LOAD (`./q file.q </dev/null`, the qscript/daemon shape — `-eval` texts
     * are script source from argv, so they count) is a BATCH
     * context, NOT a display — widen the clip to the documented 2000 ceiling
     * (basics/syscmds.md `\c`: values coerce to [10,2000]) so the script's
     * `show`/`.z.f` (an absolute path, often > 80 chars) renders full-width.
     * kdb has no off-switch, so the ceiling IS the batch idiom.  A tty that
     * drops to the REPL after the script, an explicit `-c`, or an explicit `\c`
     * in the script, resets/re-arms the size. */
    if ((script != NULL || qinit != NULL || n_before + n_after > 0) && !stdin_tty)
        q_console_clip_set(2000, 2000);

    /* After the display defaults above, so an explicit `-c` wins over them. */
    bool failed = false;
    q_sys_launching(true);
    for (int i = 0; i < n_applied && !failed; i++) failed = !option_apply(argv, applied[i]);
    q_sys_launching(false);
    free(applied);

    if (port_spec && !failed) {
        /* Parsed only now: a servicename lookup needs Winsock, which the event poll starts.  No port is announced; a
         * supervisor/test reads it back with the `\p` getter.  A bad spec or a failed bind exits non-zero rather
         * than falling through to a listener-less server loop or a plain REPL. */
        q_sys_listen_spec_t spec;
        q_err_e             err;
        failed = true;
        if (!q_sys_listen_spec_parse(port_spec, strlen(port_spec), &spec, &err) || (!spec.any && spec.lo == 0))
            fprintf(stderr, "q: invalid port '%s' (expected %s)\n", port_spec, LISTEN_SPEC_FORMS);
        else if (!poll || !q_sys_listen(&spec))
            fprintf(stderr, "q: failed to listen on '%s': %s\n", port_spec, strerror(errno));
        else
            failed = false;
    }
    if (failed) {
        if (poll) {
            ray_runtime_set_poll(NULL);
            ray_poll_destroy(poll);
        }
        q_runtime_destroy(rt);
        free(eval_before);
        free(eval_after);
        free(startup);
        return 2;
    }

    /* `-h` / `--help` is the launcher's only when no script is named: `q app.q -h` is the app's flag, in .z.x.  The
     * table is .help.cmdline, rendered by the same q as `\?cmdline`, so the C side carries no copy of it. */
    if (want_help && !script) {
        q_console_clip_set(2000, 2000);
        q_ctx_run_src(".help.usage[]", stdout, stderr, NULL);
        q_sys_exit(0);
    }

    /* Startup banner (kdb-style version + build date, via the same macros as
     * .z.v`version / .z.k).  GUARDRAIL: print ONLY on an interactive tty REPL and NOT
     * under `-q` (.z.q).  Piped/redirected stdin (the qcmd/qscript runners,
     * `printf … | ./q`, `</dev/null` daemons) is NOT a tty, so it never leaks
     * into an equality golden. */
    if (stdin_tty && !q_dotz_quiet()) {
#ifdef RAYFORCE_BUILD_DATE
        const char* build = RAYFORCE_BUILD_DATE;
#else
        const char* build = "";
#endif
        printf("peachq %d.%d %s https://peachq.org/\n",
               RAY_VERSION_MAJOR, RAY_VERSION_MINOR, build);
        /* `\?` works in classic too (help is on the always-on bootstrap now), but
         * classic is the kdb-clean env and the qscript runner forces it for
         * byte-identical output — so the doors are advertised only in modern.
         * The commands are peach (256-color 215) when the colour law allows. */
        if (!classic) {
            bool color = q_console_color(isatty(STDOUT_FILENO));
            const char* peach = color ? "\033[38;5;215m" : "";
            const char* grey  = color ? "\033[90m" : "";
            const char* off   = color ? "\033[0m" : "";
            printf("%stype %s\\?%s%s for help · %s\\l pq%s%s loads the standard library%s\n",
                   grey, peach, off, grey, peach, off, grey, off);
        }
        fflush(stdout);
    }

    /* The file (`q file`) is a `\l`.  Non-tty: run it before the server loop /
     * exit — a `-p` server serves IPC, a non-server run exits 0 (the
     * test/daemon shape) rather than blocking on an empty REPL, and an abort
     * exits non-zero.  Tty: it is the console's first `\l` (owner ruling
     * 2026-09-17, #57 — kx suspends a failing `q file.q` into `q))` when stdin
     * is a terminal), so it runs INSIDE the REPL once the debugger's reader is
     * armed, with the `-eval` texts after it. */
    /* `-eval-before` / `-eval` texts are scripts whose source came from argv: the script seam, the script abort
     * law, results NOT echoed.  An abort skips everything after it — on a non-tty the REPL/server loop included.
     * On a tty the `-eval` texts follow the script into the REPL (console-initiated, so they suspend as it does);
     * `-eval-before` precedes the script by definition, so it stays the batch idiom on both. */
    int script_rc = 0;
    /* a file-backed main loads its tables first, like `q dir/` — which implies `\l pq`, the loader's home */
    if (duckdb_main && *duckdb_main)
        script_rc = q_ctx_run_src("\\l pq\n.duckdb.load[.duckdb.main[];::]", stdout, stderr, NULL);
    if (qinit && !stdin_tty && script_rc == 0)
        script_rc = q_ctx_run_file(qinit, stdout, stderr, NULL);
    for (int i = 0; i < n_before && script_rc == 0; i++)
        script_rc = q_ctx_run_src(eval_before[i], stdout, stderr, NULL);
    free(eval_before);

    const char*  files[] = { qinit, script };   /* the tty console's first `\l`s, QINIT ahead of the script */
    char         load[2][PATH_MAX + 4];
    char         abs[PATH_MAX];
    /* QINIT absolute (relative to the start directory, never QHOME); the file as given, so `\l` resolves it */
    if (qinit)
        snprintf(load[0], sizeof load[0], "\\l %s", q_io_abs_path(qinit, abs, sizeof abs) ? abs : qinit);
    if (script)
        snprintf(load[1], sizeof load[1], "\\l %s", script);
    if (stdin_tty) {
        int k = 0;
        for (int i = 0; i < 2; i++)
            if (files[i]) startup[k++] = load[i];
        for (int i = 0; i < n_after; i++) startup[k++] = eval_after[i];
        q_repl_prime(startup);
    } else {
        if (script && script_rc == 0)
            script_rc = q_ctx_run_load(script, stdout, stderr);
        for (int i = 0; i < n_after && script_rc == 0; i++)
            script_rc = q_ctx_run_src(eval_after[i], stdout, stderr, NULL);
    }
    free(eval_after);

    if (link >= 0) {
        /* A :pq:qspawn: worker: its startup ran as any q's (a script error does not stop it, kx's rule), then it serves
         * its link — and any listener its script opened — until the link closes, and prints nothing of its own. */
        if (poll && ray_ipc_serve_link(link) >= 0) ray_poll_run(poll);
        script_rc = 0;
    } else if (script_rc != 0) {
        /* A batch startup stage (the `-duckdb` main load, QINIT, a non-tty
         * script or an `-eval` text) could not be opened or ABORTED at an error
         * (parse or eval — the script seam's law): skip the REPL/server loop and exit non-zero (kdb fails a bad
         * `q file.q` on a non-tty stdin; it must not silently succeed).  The
         * open error / the statement's trace already printed. */
    } else if (q_sys_listen_port() > 0 && poll) {
        /* A listener is LIVE — from startup `-p` OR a runtime/script `\p N` —
         * so serve, don't exit at a non-tty script end.  Keyed off the
         * authoritative `\p` getter state (q_sys_listen_port), not a stale
         * local `port` or a sticky listener flag: a startup-script `\p 0` that
         * closes the `-p` listener now correctly drops OUT of server mode
         * (else a non-tty `q script.q -p 0W </dev/null` with `\p 0` would hang
         * in a listener-less poll loop).  Live listener + console: register
         * stdin on the SAME poll
         * IPC listener and run ONE event loop (q_repl_run_poll — mirrors
         * rayforce's run_interactive), so clients are served WHILE the REPL
         * reads — no EOF needed.  Covers both the tty console and piped
         * stdin; on stdin EOF the loop keeps serving (the `</dev/null`
         * daemon shape), on `\\`/`exit` it exits.  SIGPIPE ignored so a
         * broken client/log pipe can't kill the server (writes fail with
         * EPIPE instead). */
        int concurrent = 0;
#ifndef RAY_OS_WINDOWS
        signal(SIGPIPE, SIG_IGN);   /* no SIGPIPE on Windows; send errors
                                     * surface as WSAECONNRESET instead */
#endif
        concurrent = (q_repl_run_poll(poll, stdout, stderr, stdin_tty) == 0);
        if (!concurrent) {
            /* Fallback: a stdin the poll backend cannot watch (e.g. a
             * regular-file redirect under epoll).  Legacy serial shape. */
            if (stdin_tty) {
                fprintf(stderr, "note: -p with an interactive REPL serves "
                                "IPC only after EOF on this platform\n");
                q_repl_run(stdin, stdout, stderr, 0);
            } else {
                fprintf(stderr, "no terminal - running in server-only mode\n");
            }
            if (q_sys_listen_port() > 0)
                ray_poll_run(poll);   /* serve iff the listener is still LIVE */
        }
    } else if (script && !stdin_tty) {
        /* Ran a startup script with no server on a non-tty (`q file.q
         * </dev/null`): exit 0 without entering the REPL. */
    } else {
        /* No listener: run the SAME poll event loop the server uses (Bundle 3
         * — unify on rayforce's run_interactive shape) so an idle client
         * services its open outbound IPC conns (server pushes dispatch the
         * instant they arrive, not only during a sync-send) and a runtime `\p`
         * joins this poll — and is then LIVE, so EOF serves instead of exiting.
         * A client that never `\p`s still finishes at EOF.  Fall back where
         * the poll backend can't watch stdin (regular-file redirect under
         * epoll; the IOCP backend watches every stdin kind). */
        int concurrent = 0;
        if (poll)
            concurrent = (q_repl_run_poll(poll, stdout, stderr, stdin_tty) == 0);
        if (!concurrent) {
            q_repl_run(stdin, stdout, stderr, !stdin_tty);
            if (poll && q_sys_listen_port() > 0)
                ray_poll_run(poll);   /* a runtime `\p` made this a server after all */
        }
    }

    /* Natural end-of-session (stdin EOF / script end) is a process exit too:
     * route it through THE exit home, so `.z.exit` fires exactly once with the
     * exit status (dotz.md) on EVERY session exit — `\\`/`exit x` never reach
     * here (q_sys_exit already terminated).  q_sys_exit does not return; the OS
     * reclaims the poll/runtime (same as the `exit x` path, kdb-true). */
    free(startup);
    q_sys_exit(script_rc);
    return script_rc;   /* unreachable */
}
