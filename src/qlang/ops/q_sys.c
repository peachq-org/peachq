/* q_sys — see q_sys.h.  The unified `\`-command dispatcher: one line pre-parse
 * (command token / `:n` repeat suffix / first-arg token / whole argument
 * region) feeding a SWITCH on the command char, each case calling its handler
 * with only the arguments it needs.  A handler
 * returns an OWNED value (NULL = silent) or an OWNED error — including `\\`
 * (q_sys_exit) and the unknown-token shell miss, both gated by the g_own_process
 * capability rather than by the caller.  \d owns the current-context state
 * here; \S owns its seed state here. */
#define _POSIX_C_SOURCE 200809L
/* winsock2.h must precede EVERY windows.h (core/profile.h pulls one in) or
 * mingw silently falls back to the winsock v1 declarations. */
#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>      /* socklen_t */
#endif
#include "qlang/ops/q_sys.h"
#include "qlang/base/q_err.h"
#include "qlang/eval/q_eval.h" /* q_eval / q_eval_dot_wrap — timing + .Q.ts */
#include "qlang/q_fmt.h"      /* q_fmt_set_prec/q_fmt_prec (`\P`) */
#include "qlang/q_console.h"  /* q_console_str/reset (timed-expr side effects); q_console_pipe_* (`\classic`) */
#include "qlang/q_ctx.h"           /* the engine context: `\l` source seam, console teardown */
#include "qlang/io/q_io.h"    /* q_io_mkdir_parents — `\1`/`\2` create the path they name */
#include "qlang/q_pq.h"       /* q_pq_load — the `\l pq` embedded-stdlib gate */
#include "qlang/q_env.h"      /* q_env_ctx_set/_ctx + q_env_ns_names — `\d` and the `\v`/`\f`/`\a` rosters */
#include "qlang/q_dotz.h"     /* q_dotz_timer_thunk — the `.z.ts` timer callback */
#include "qlang/eval/q_view.h" /* q_view_names — `\b` / `\B` */
#include "qlang/parse/q_parse.h"    /* q_parse — `\t expr` / `\ts expr` timing */
#include "qlang/net/q_tls.h"  /* q_tls_server_mode — `\E` */
#include "core/ipc.h"         /* ray_ipc_listen — `\p N` binds a listener */
#include "core/poll.h"        /* ray_poll_get / deregister — `\p 0W`/`\p 0`; poll->timers */
#include "core/runtime.h"     /* ray_runtime_get_poll — the runtime event poll */
#include "core/numparse.h"    /* ray_parse_i64 — the shared engine int parser (reuse) */
#include "core/rand.h"        /* ray_rand_seed — `\S` re-seeds THE stream */
#include "core/timer.h"       /* ray_timers_create/add/del — `\t N` timer heap */
#include "core/profile.h"     /* ray_profile_now_ns — `\t`/`\ts` wall clock */
#include "lang/eval.h"        /* ray_eval / ray_eval_get_restricted — timing + guard */
#include "lang/internal.h"    /* ray_getenv_fn / ray_setenv_fn — the getenv/setenv verbs */
#include "ops/ops.h"          /* ray_is_lazy / ray_lazy_materialize — timed-expr result */
#include <rayforce.h>
#include "mem/heap.h"         /* ray_mem_stats / ray_mem_stats_t — `\w` reuse */
#include <stdlib.h>           /* system, malloc */
#include <string.h>           /* strlen, memcpy, memcmp */
#include <stdio.h>            /* popen / pclose — `system "…"` stdout capture */
#include <unistd.h>          /* chdir / getcwd / access — `\cd`, `\l`; dup2 — `\1`/`\2` */
#include <fcntl.h>           /* open — the `\1`/`\2` redirect target */
#include <limits.h>          /* PATH_MAX */
#ifdef RAY_OS_WINDOWS
  #include <io.h>            /* msvcrt spells the POSIX descriptor calls with an underscore */
  #define dup2 _dup2
#endif
#ifndef O_BINARY
  #define O_BINARY 0         /* only Windows has a text mode to opt out of */
#endif
#include <sys/stat.h>        /* stat / S_ISREG — `\l` regular-file gate */
#include "qlang/q_registry.h" /* q_str_text_bytes / q_str_charv_out — charv text accessors */
#include "qlang/q_registry_internal.h" /* q_exit_wrap + the q_type_* atom helpers */
#ifndef RAY_OS_WINDOWS
#include <sys/wait.h>        /* WIFEXITED / WEXITSTATUS — shell-capture status */
#endif                       /* mingw has no <sys/wait.h>; _pclose gives the code directly */

/* `\p 0W` reads the OS-chosen port back off the listener fd (getsockname),
 * mirroring qmain.c's startup `-p 0W` path — the two share one readiness line. */
#ifndef RAY_OS_WINDOWS
  #include <sys/socket.h>
  #include <netinet/in.h>
#endif

/* ---- `\d` current context.  q_env is its ONE home (q_env.h: relative names
 * resolve in it, assignments land in it); this file owns only its RENDERING, so
 * a switch made anywhere — including inside a lambda, whose call boundary may
 * restore the caller's — can never desync from what resolution actually uses. */
void q_sys_ctx_reset(void) { q_env_ctx_set(0); }

int q_sys_prompt(char* buf, size_t cap) {
    ray_t* s = ray_sym_str(q_env_ctx());          /* NULL at root */
    int n = snprintf(buf, cap, "q%.*s)", s ? (int)ray_str_len(s) : 0,
                     s ? ray_str_ptr(s) : "");
    return (n < 0 || (size_t)n >= cap) ? 0 : n;
}

static int ctx_ident_ok(const char* p, size_t len) {
    if (len == 0) return 0;
    if (!((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')))
        return 0;
    for (size_t i = 1; i < len; i++) {
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static ray_t* ctx_switch(const char* name, size_t len) {
    if (len == 1 && name[0] == '.') {          /* `\d .` — back to root */
        q_env_ctx_set(0);
        return NULL;
    }
    /* One level below root only (kdb limitation, q4m3 §12.7): `.ident`. */
    if (len >= 2 && len < 64 && name[0] == '.' &&
        ctx_ident_ok(name + 1, len - 1)) {
        q_env_ctx_set(ray_sym_intern_runtime(name, len));
        return NULL;
    }
    return q_err_name(name, len);       /* `\d .jab.util` -> '.jab.util */
}

/* ---- `\S` random-seed state (moved from q_ns.c; \S is its only consumer) ----
 * kdb re-initializes its rng to the CONSTANT -314159i at startup
 * (basics/syscmds.md) so scripts using Roll/Deal/rand repeat; the stream it
 * seeds is core/rand.h's, shared by every `?` arm including guid.  `\S`
 * displays the LAST-INITIALIZED seed, never evolving rng state; reading the
 * live state (`\S 0N`, V3.6) is 'nyi. */
static int32_t g_last_seed = -314159;

void q_sys_seed_init(void) {
    g_last_seed = -314159;
    ray_rand_seed(-314159);
}

/* ---- Tier-2 config state (kdb `\`-command get/set) -------------------------
 * Each command's handler is the single WRITER; its getter reports the stored
 * value.  q_sys_cfg_init resets everything to the kdb default per runtime
 * (called by q_runtime_create), so a `\c 5 5` in one .qcmd file never leaks
 * into the next (the doctest runner builds a fresh runtime per file).
 *
 * BEHAVIOURAL SIDE-EFFECTS ARE MOSTLY DEFERRED (rule 9): `\c` NOW clips the q
 * console DISPLAY (width + height, applied by q_fmt.c's console emitter — see
 * q_fmt_console), but peachq does not yet wrap `\C` HTTP output, run real gc
 * for `\g`, apply `\o`/`\W` to temporal display, trap errors for `\e`, or
 * re-tune worker threads for `\s`.  Those commands store + report the kdb-true
 * value only; the effect itself is a tracked PLAN.md gap.  Faking a
 * side-effect would be worse than an honest store-and-report. */
static int32_t g_http_rows, g_http_cols;  /* \C HTTP size     (default 36 2000) */
static int32_t g_gc_mode;                 /* \g gc mode       (default 0)       */
static int64_t g_utc_offset;              /* \o UTC offset    (default 0N)      */
static int32_t g_week_offset;             /* \W week offset   (default 2)       */
static int32_t g_err_trap;                /* \e error trap    (default 0)       */
static int32_t g_sec_threads;             /* \s secondary thr (default 0)       */
static int     g_own_process;             /* this runtime may exit/shell process */
static int     g_exiting;                 /* .z.exit reentry guard */

/* \t timer: current interval (ms; 0 = off) and the live timer id (-1 = none). */
static int64_t g_timer_ms = 0;
static int64_t g_timer_id = -1;

bool q_sys_timer_active(void) { return g_timer_ms > 0; }

/* `\p` listening-port state.  g_listen_port is what the `\p` getter reports
 * (0 = not listening, kdb default); g_listen_sel is the poll selector id of the
 * live listener so `\p 0` can deregister it (deregister fires ipc_on_close ->
 * ray_sock_close on the listen fd).  -1 = none. */
static int32_t g_listen_port;
static int64_t g_listen_sel = -1;

void q_sys_cfg_init(void) {
    g_own_process = 0;
    g_exiting = 0;
    q_console_clip_set(25, 80);  /* `\c` clip ARMED at the 25 80 default (q_console.c) */
    g_http_rows = 36; g_http_cols = 2000;
    g_gc_mode   = 0;
    g_utc_offset = NULL_I64;     /* 0N — "use the machine offset" (deferred) */
    g_week_offset = 2;           /* Monday (0 = Saturday) */
    g_err_trap  = 0;             /* trapping off */
    g_sec_threads = 0;           /* no secondary threads configured */
    g_timer_ms  = 0;             /* `\t` off per runtime */
    g_timer_id  = -1;
    g_listen_port = 0;           /* `\p` — no listening port by default */
    g_listen_sel  = -1;          /* no live listener selector */
    q_fmt_set_prec(7);           /* `\P` default (single-homed in q_fmt.c) */
}

void q_sys_own_process(bool on) { g_own_process = on ? 1 : 0; }

/* See q_sys.h.  `.z.exit` runs AFTER the console is restored (its 0N! output
 * must land on a cooked terminal) and cannot cancel or rewrite the exit: a
 * reentrant q_sys_exit from inside the handler skips it and exits with the
 * ORIGINAL code (dotz.md: "The handler cannot cancel the exit"). */
static int g_exit_code;
void q_sys_exit(int code) {
    if (!g_own_process) return;
    if (g_exiting) exit(g_exit_code);
    g_exiting  = 1;
    g_exit_code = code;
    q_ctx_console_close();
    q_dotz_exit_fire(code);
    q_console_flush();   /* the host never gets its drain turn — issue #23 */
    exit(code);
}

/* q `exit x` — terminate with exit code x (ref/exit.md; blocked during reval
 * -> 'access, kdb-true).  All processing lives in q_sys_exit (fires `.z.exit`,
 * capability-gated): under the doctest/wasm runtimes q_sys_exit returns and the
 * verb is a silent null — the runner survives corpus `exit 0` rows. */
ray_t* q_exit_wrap(ray_t* x) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    if (!q_type_is_int_atom(x) || RAY_ATOM_IS_NULL(x)) return q_err(QE_TYPE);
    q_sys_exit((int)q_type_iatom_val(x));
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Whole-span signed-int parse: 1 iff ray_parse_i64 (numparse.c) consumes ALL of [s,s+len). */
static int parse_i64(const char* s, size_t len, int64_t* out) {
    return len > 0 && ray_parse_i64(s, len, out) == len;
}

/* Parse up to `max` whitespace-separated base-10 integers from [s,s+len),
 * stopping at the first non-integer token (e.g. a trailing `/ comment`).
 * Returns the count stored into out[].  allow_null additionally accepts the
 * `0N` token as NULL_I64 (the `\c` auto-size sentinel). */
static int parse_ints_n(const char* s, size_t len, int64_t* out, int max, int allow_null) {
    int cnt = 0; size_t i = 0;
    while (cnt < max && i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        size_t t0 = i;
        while (i < len && s[i] != ' ' && s[i] != '\t') i++;
        if (i == t0) break;
        int64_t v;
        if (allow_null && i - t0 == 2 && s[t0] == '0' && s[t0 + 1] == 'N')
            v = NULL_I64;
        else if (!parse_i64(s + t0, i - t0, &v))
            break;
        out[cnt++] = v;
    }
    return cnt;
}
static int parse_ints(const char* s, size_t len, int64_t* out, int max) {
    return parse_ints_n(s, len, out, max, 0);
}

/* Build a two-element typed vector for the pair-valued getters (`\c`/`\C`). */
static ray_t* pair_i64(int64_t a, int64_t b) {
    ray_t* v = ray_vec_new(RAY_I64, 2);
    if (RAY_IS_ERR(v)) return v;
    v = ray_vec_append(v, &a);
    if (RAY_IS_ERR(v)) return v;
    return ray_vec_append(v, &b);
}

/* ---- per-command handlers ---------------------------------------------------
 * Each handler takes ONLY the arguments its command needs — the switch in
 * q_sys_run passes them tailored (no uniform signature, no ignored params).
 * The common shapes: `arg`/`alen` is the already-tokenized FIRST argument
 * (empty when absent); `rest`/`restlen` is the FULL argument region (first
 * token through EOL, trailing `/ comment` included) for the multi-token
 * commands (`\c`/`\C`/`\t`/`\ts`); `rep` is the `:n` repeat count (`\t:n`).
 * Return: OWNED value | NULL (silent) | OWNED error. */

static ray_t* h_d(const char* arg, size_t alen) {
    if (alen == 0) {                             /* `\d` — show current */
        int64_t ns = q_env_ctx();
        ray_t* s = ray_sym(ns ? ns : ray_sym_intern(".", 1));
        /* DATA sym, not a name-ref: keeps its backtick in q_fmt (`.) */
        if (s && !RAY_IS_ERR(s)) s->attrs |= 0x20; /* Q_ATTR_QUOTED */
        return s;
    }
    return ctx_switch(arg, alen);                /* NULL (silent) or error */
}

/* `\v` variables / `\f` functions / `\a` tables (basics/syscmds.md) — the same
 * roster under three value-kind filters, non-recursive, defaulting to the `\d`
 * context, which need not exist yet (empty listing).  A NAMED missing namespace
 * errors with the name: `\a .n` -> '.n (untruncated — q_err_name payload). */
static ray_t* h_vfa(char cmd, const char* arg, size_t alen) {
    q_env_ns_kind_t kind = cmd == 'v' ? Q_ENV_NS_VARS
                         : cmd == 'f' ? Q_ENV_NS_FNS : Q_ENV_NS_TABLES;
    int64_t ns = alen ? ray_sym_intern_runtime(arg, alen) : q_env_ctx();
    ray_t* out = q_env_ns_names(ns, kind);
    if (out) return out;
    if (alen == 0) return ray_sym_vec_new(RAY_SYM_W64, 1);
    return q_err_name(arg, alen);
}

static ray_t* h_S(const char* arg, size_t alen) {
    if (alen == 0)                               /* `\S` — last-initialized seed */
        return ray_i32(g_last_seed);
    if (alen == 2 && arg[0] == '0' && arg[1] == 'N')  /* `\S 0N` — live state */
        return q_err(QE_NYI);
    int64_t v;
    if (!parse_i64(arg, alen, &v))
        return q_err(QE_PARSE);         /* non-integer arg (unpinned) */
    /* The seed is an INT (`\S` displays it as one): out-of-int-range values
     * and the 0Ni sentinel are rejected, never silently truncated (codex P2,
     * 2026-07-09). */
    if (v <= INT32_MIN || v > INT32_MAX)
        return q_err(QE_PARSE);
    ray_rand_seed(v);                            /* `\S n` — re-initialize */
    g_last_seed = (int32_t)v;
    return NULL;                                 /* silent, like `\d ns` */
}

/* ---- shared handlers for the still-unimplemented commands ------------------
 * After the dedicated handlers below (\P \c \C \w \g \o \W \e \s, \l \cd \p,
 * \t \ts), h_getset serves the REMAINING commands whose SETTER / ACTION form
 * (arg present) prints NOTHING in kdb — date-parse (\z), file/name actions
 * (\x \r \_), and timeout/TLS/user config (\T \E \u).  We accept the
 * arg-form as a silent no-op: the OUTPUT matches kdb's empty setter output, so
 * the frozen ledger rows that bank that silence (e.g. `\z 1`) stay green.  The
 * side-effect is a tracked gap; the GETTER form (no arg, which kdb prints a
 * value for) can't yet report the value → honest 'nyi. */
static ray_t* h_getset(size_t alen) {
    if (alen > 0) return NULL;                   /* setter/action form → silent */
    return q_err(QE_NYI);               /* getter form → not yet */
}

/* `\1 file` / `\2 file` — basics/syscmds.md: files and intermediate directories
 * created if necessary, output APPENDED to an existing one.  FD level (dup2),
 * not a re-pointed FILE*, so every writer follows at once — the console drain,
 * the q handle 2, a child inheriting the descriptor — and `\1 /dev/stdin`
 * restores the default the way the doc says.  Getter form: still 'nyi. */
static ray_t* h_redirect(int fd, const char* arg, size_t alen) {
    if (alen == 0) return q_err(QE_NYI);
    if (alen >= PATH_MAX) return q_err(QE_OS);
    char path[PATH_MAX];
    memcpy(path, arg, alen); path[alen] = '\0';
    q_io_mkdir_parents(path, alen);
    int f = open(path, O_BINARY | O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (f < 0) return q_err(QE_OS);
    fflush(fd == 1 ? stdout : stderr);   /* buffered bytes belong to the OLD target */
    if (f == fd) return NULL;            /* std fd was closed: open landed ON it, already done */
    int rc = dup2(f, fd);
    close(f);
    return rc < 0 ? q_err(QE_OS) : NULL;
}

/* `\b` (views) / `\B` (pending views), basics/syscmds.md — q_view.c owns the
 * roster; the ns arg defaults to the current context like \v/\f/\a, and views
 * live only in the default namespace.  `.q.views` IS `system"b"`. */
static ray_t* h_bB(int pending, const char* arg, size_t alen) {
    int64_t ns = alen ? ray_sym_intern_runtime(arg, alen) : q_env_ctx();
    ray_t* out = q_view_names(ns, pending);
    if (out) return out;
    if (alen == 0) return ray_sym_vec_new(RAY_SYM_W64, 1);
    return q_err_name(arg, alen);
}

/* ---- Stage-3 implemented handlers -----------------------------------------
 * TYPE POLICY (kdb-true, from syscmds.md renderings, clean-room):
 *   - scalar mode getters render as INT with the `i` suffix — matched by the
 *     doc: `\P`→`7i`, and `\s` shows `0i`/`8i`.  `\g`/`\W`/`\e` have no getter
 *     rendering shown in the doc, so their int suffix is inferred from that
 *     same family (flagged for human verification before floor-banking).
 *   - `\c`/`\C` and `\o` render WITHOUT a suffix in the doc (`45 160`,
 *     `10 10`, `0N`) — i.e. LONGS — so those getters return i64. */

/* Read the actual bound port back off a listener fd (getsockname) — the `0W`
 * auto-bind path needs the OS-chosen port to report it.  The SINGLE home of the
 * readback (qmain.c's startup `-p` now calls q_sys_listen below rather than
 * duplicating this); 0 on failure. */
static uint16_t p_bound_port(int64_t fd) {
    struct sockaddr_in sa;
    socklen_t          len = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    if (getsockname((ray_sock_t)fd, (struct sockaddr*)&sa, &len) != 0)
        return 0;
    return ntohs(sa.sin_port);
}

/* Single-home the listen+readback+state-swap shared by `\p N`/`\p 0W` and the
 * startup `-p` path (see q_sys.h).  port==0 → OS-chosen ephemeral.  Silent. */
uint16_t q_sys_listen(uint16_t port) {
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    if (!poll) return 0;                                  /* no event poll (e.g. qdoctest) */
    int64_t sel = ray_ipc_listen(poll, port);            /* port==0 → OS picks a free port */
    if (sel < 0) return 0;                               /* bind/listen failed — old listener intact */
    ray_selector_t* ls = ray_poll_get(poll, sel);
    uint16_t bound = ls ? p_bound_port(ls->fd) : 0;
    if (!bound) {
        /* Readback failed after a successful bind: tear the new listener down
         * (so it doesn't leak) and report failure — never advertise port 0
         * (readiness would pass but every client would fail to connect). */
        ray_poll_deregister(poll, sel);
        return 0;
    }
    /* kdb listens on ONE port: drop the previous listener now the new bind
     * succeeded (a failed bind above left the old one intact). */
    if (g_listen_sel >= 0) ray_poll_deregister(poll, g_listen_sel);
    g_listen_sel  = sel;
    g_listen_port = bound;                               /* authoritative — `system "p"` reports it */
    return bound;
}

/* The authoritative live listening port (0 = none) — see q_sys.h.  qmain reads
 * it for the post-script server-mode decision instead of a stale local port. */
uint16_t q_sys_listen_port(void) { return (uint16_t)g_listen_port; }

/* `\p` — listening port (basics/syscmds.md, listening-port.md).  Merges this
 * feature's getter/close/rebind with #127's `0W` ephemeral bind:
 *   getter `\p`        -> current listening port, `0i` when none (kdb default).
 *   `\p 0`             -> stop listening: deregister the live listener selector
 *                        (fires ipc_on_close -> closes the fd), reset to 0.
 *   `\p N` (1..65535)  -> bind a kdb-protocol IPC listener on the runtime event
 *                        poll; the unified REPL loop (q_repl.c) serves it and
 *                        reads this state at stdin EOF, so a client that `\p`s a
 *                        port becomes a server and a `\p 0` stops being one.
 *   `\p 0W`            -> bind any OS-chosen free port, read it back via
 *                        getsockname and record it (mirrors startup `-p 0W`,
 *                        qmain.c — both call q_sys_listen) — what lets
 *                        tools/qscript run each server on an ephemeral port,
 *                        immune to a busy 5000 / concurrent runners.
 * `\p N`/`0W` rebind drops the previous listener only AFTER the new bind
 * succeeds (a failed bind leaves the old one intact; codex diff P2 — enforced
 * inside q_sys_listen).  The getter reports the ACTUAL bound port (incl. the
 * `0W`-chosen one).  NO port is ANNOUNCED on any path — full kdb fidelity; a
 * supervisor/test reads the port back with the `\p`/`system "p"` getter. */
/* `\E` — DISPLAY the TLS server mode as an int (syscmds.md documents no setter
 * form; the mode is fixed by the `-E` command line).  A setter stays the silent
 * no-op every other display-only `\`-command uses. */
static ray_t* h_E(const char* arg, size_t alen) {
    (void)arg;
    return alen == 0 ? ray_i32(q_tls_server_mode()) : NULL;
}

static ray_t* h_p(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(g_listen_port);        /* getter -> `0i` default */
    /* `0W`/`0w` auto token (same shape as h_S's `0N` probe) — bind port 0. */
    bool port_auto = (alen == 2 && arg[0] == '0' && (arg[1] == 'W' || arg[1] == 'w'));
    int64_t v = 0;
    if (!port_auto) {
        if (!parse_i64(arg, alen, &v)) return q_err(QE_PARSE);
        if (v < 0 || v > 65535) return q_err(QE_DOMAIN);
    }
    if (!port_auto && v == 0) {                          /* `\p 0` — stop listening */
        ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
        if (poll && g_listen_sel >= 0) ray_poll_deregister(poll, g_listen_sel);
        g_listen_sel  = -1;
        g_listen_port = 0;
        return NULL;                                     /* silent */
    }
    /* Bind/readback/state-swap single-homed in q_sys_listen (shared with startup
     * `-p`).  0 → no poll (qdoctest) / bind-listen / readback failure → `'io`. */
    if (!q_sys_listen(port_auto ? 0 : (uint16_t)v)) return q_err(QE_IO);
    return NULL;                                          /* setter: silent */
}

/* `\cd` — change directory (basics/syscmds.md).  getter -> current directory as
 * a char vector; `\cd fp` -> real chdir (the kx "wrong directory" footgun fix,
 * ARCHITECTURE decision: cwd must be controllable + predictable).  DEFERRED:
 * kdb's create-if-missing on a set (a missing target -> 'os here). */
static ray_t* h_cd(const char* arg, size_t alen) {
    if (alen == 0) {                                     /* getter: current dir */
        char buf[PATH_MAX];
        if (!getcwd(buf, sizeof buf)) return q_err(QE_OS);
        return ray_str(buf, strlen(buf));                /* char vector */
    }
    if (alen >= PATH_MAX) return q_err(QE_OS);
    char path[PATH_MAX];
    memcpy(path, arg, alen); path[alen] = '\0';
    if (chdir(path) != 0) return q_err(QE_OS);
    return NULL;                                          /* setter: silent */
}

/* A load candidate is usable iff it is a REGULAR, READABLE file (a directory or
 * a missing/unreadable path is a silent no-op — preserves the banked `\l .`,
 * `\l /tmp/db*` rows).  mingw-portable (stat/S_ISREG/access). */
static int l_is_regular_readable(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, R_OK) == 0;
}

/* `\l name` — load a q script (basics/syscmds.md).  Resolution chain (first hit
 * wins): (a) the literal path relative to cwd; (b) the literal + ".q" (kdb loads
 * `\l script` as `script.q`); (c)/(d) for a RELATIVE name only, `$QHOME/name`
 * then `$QHOME/name.q` — a fixtures/QHOME-style search root (the doctest runner
 * points QHOME at test/qscript).  An absolute path never gets QHOME prepended.
 * The resolved REGULAR readable file is executed line-at-a-time via the public
 * q_ctx_run_file (multiline-aware; silent — kdb loads silently).  A
 * still-MISSING path signals the path as given (kdb: `\l nope.q` -> 'nope.q);
 * an existing DIRECTORY stays a silent no-op — the banked `\l .` / `\l
 * /tmp/db*` corpus rows target live dirs, and the directory / splayed-table /
 * serialized-object load forms are deferred.  Getter form (no arg) stays
 * 'nyi.  `system "l …"` single-homes through here. */
static ray_t* h_l(const char* arg, size_t alen) {
    if (alen == 0) return q_err(QE_NYI);        /* `\l` (bare) — reload cwd, deferred */
    /* hsym spelling `\l :path.q` (TimeStored pkg.q, qunitSurefire.q emit it);
     * tolerant superset on the owner's say-so, 2026-08-11 */
    if (alen >= 2 && arg[0] == ':') { arg++; alen--; }
    if (alen >= PATH_MAX) return q_err_name(arg, alen);
    char lit[PATH_MAX];
    memcpy(lit, arg, alen); lit[alen] = '\0';

    char cand[PATH_MAX];
    char found[PATH_MAX];                                 /* stable copy of the resolved path */
    int  ok = 0;

    /* (a) literal path, relative to cwd (kdb: `\l name` as given). */
    if (l_is_regular_readable(lit)) { memcpy(found, lit, alen + 1); ok = 1; }
    /* (b) literal + ".q" (kdb loads `\l script` as `script.q`). */
    if (!ok && snprintf(cand, sizeof cand, "%s.q", lit) < (int)sizeof cand
        && l_is_regular_readable(cand)) { memcpy(found, cand, strlen(cand) + 1); ok = 1; }
    /* (c)/(d) fixtures/QHOME search — RELATIVE names only (an absolute path is
     * literal in kdb; never prepend a root to it — keeps `\l /tmp/db*` a no-op). */
    if (!ok && lit[0] != '/') {
        const char* qh = getenv("QHOME");
        if (qh && *qh) {
            if (snprintf(cand, sizeof cand, "%s/%s", qh, lit) < (int)sizeof cand
                && l_is_regular_readable(cand)) { memcpy(found, cand, strlen(cand) + 1); ok = 1; }
            if (!ok && snprintf(cand, sizeof cand, "%s/%s.q", qh, lit) < (int)sizeof cand
                && l_is_regular_readable(cand)) { memcpy(found, cand, strlen(cand) + 1); ok = 1; }
        }
    }
    if (ok) {   /* disk hit — load (silent); an ABORTED load signals: eval aborts
                 * re-signal their text (already displayed), parse aborts their class */
        ray_t* esig = NULL;
        int rc = q_ctx_run_file(found, stdout, stderr, &esig);
        if (esig) return esig;
        return rc >= 2 ? q_err((q_err_e)(rc - 2)) : NULL;
    }
    /* peachq: `\l pq` — the PeachQ stdlib gate. A dev-override disk file (the
     * a/b/c/d chain above) wins; else a cwd directory literally named `pq`
     * keeps existing dir semantics (no-op, below); ELSE the embedded stdlib.
     * Every OTHER argument keeps its existing behaviour unchanged (the branch is
     * scoped to the exact literal `pq`). */
    if (alen == 2 && arg[0] == 'p' && arg[1] == 'q') {
        struct stat st;
        if (!(stat("pq", &st) == 0 && S_ISDIR(st.st_mode)))     /* not a `pq` dir → embedded */
            return q_pq_load();
    }
    struct stat st;
    if (stat(lit, &st) == 0 && S_ISDIR(st.st_mode)) return NULL;  /* dir load deferred */
    return q_err_name(lit, alen);                         /* kdb: 'path as given */
}

/* ---- expression timing (`\t expr`, `\t:n`, `\ts expr`, `\ts:n`) ------------
 * Time a q expression string by running it through the SAME pipeline the REPL
 * uses (q_parse → q_eval), `reps` times, discarding each result.  On success
 * fills *ms with the TOTAL wall-clock milliseconds and *bytes with the space
 * metric, then returns NULL; a parse/eval error returns the OWNED error
 * instead (ms/bytes then meaningless).  The `:n` repetition form is kdb's
 * `do[n; e]` — execution repeated, so we parse ONCE and re-evaluate the tree
 * (q_eval is read-only on the AST; re-running it re-runs the program,
 * assignments and all).  Runtime dispatch is allowed to parse here — the
 * rule-6 prohibition is on registry BUILDERS, not a warm-registry handler.
 *
 * SPACE METRIC — DIVERGES FROM kdb (owner ruling 2026-07-14, best-effort):
 * kdb's `\ts` reports the PEAK transient workspace a computation touches; peachq
 * reports the NET `ray_mem_stats().bytes_allocated` delta measured with the
 * final result still live (snapshot taken before release).  So an expression
 * whose result is retained (`til 100000`) shows a positive figure in the
 * ballpark of that result's size, but transient intermediates freed mid-eval
 * are NOT counted, and the ASan debug allocator inflates the number versus a
 * release build.  Tests pin only shape + sign (space > 0 for an allocating
 * expr), never a golden byte count. */
typedef struct { ray_mem_stats_t mem; int64_t t0; } tsmeas_t;
static void ts_begin(tsmeas_t* m) { ray_mem_stats(&m->mem); m->t0 = ray_profile_now_ns(); }
/* Call with the result STILL LIVE — the space metric is the net-alloc delta. */
static void ts_end(const tsmeas_t* m, double* ms, int64_t* bytes) {
    int64_t t1 = ray_profile_now_ns();
    ray_mem_stats_t after;
    ray_mem_stats(&after);
    *ms = (double)(t1 - m->t0) / 1e6;
    int64_t d = (int64_t)after.bytes_allocated - (int64_t)m->mem.bytes_allocated;
    *bytes = d < 0 ? 0 : d;
}

static ray_t* time_expr(const char* expr, size_t len, int64_t reps,
                          double* ms, int64_t* bytes) {
    if (reps < 0) reps = 0;                              /* `\t:0` → do[0;e] = no runs */
    /* q_parse wants a NUL-terminated C string; expr is a slice of the line. */
    char   stackbuf[1024];
    char*  s   = stackbuf;
    ray_t* blk = NULL;
    if (len + 1 > sizeof stackbuf) {
        blk = ray_alloc(len + 1);
        if (!blk) return q_err(QE_OOM);
        s = (char*)ray_data(blk);
    }
    memcpy(s, expr, len);
    s[len] = '\0';

    ray_t* ast = q_parse(s);                             /* strips a trailing /comment */
    if (blk) ray_free(blk);
    if (RAY_IS_ERR(ast)) return ast;

    tsmeas_t m;
    ts_begin(&m);
    ray_t*  r   = NULL;
    ray_t*  err = NULL;
    for (int64_t k = 0; k < reps; k++) {
        if (r) { ray_release(r); r = NULL; }             /* keep only the last result */
        r = q_eval(ast);
        if (ray_is_lazy(r)) r = ray_lazy_materialize(r);
        if (r && RAY_IS_ERR(r)) { err = r; r = NULL; break; }
    }
    ts_end(&m, ms, bytes);                               /* last result still live */
    ray_release(ast);
    if (r) ray_release(r);
    if (err) return err;                                 /* propagate the eval error */
    return NULL;
}

/* See q_sys.h.  `.Q.ts[f;args]` / `-34!(f;args)` (ref/dotq.md#ts-time-and-space):
 * `.[f;args]` under the SAME measurement `\ts` uses -> ((ms;bytes); result). */
ray_t* q_sys_ts_apply(ray_t* f, ray_t* args) {
    ray_t*   fa[2] = { f, args };
    double   ms;
    int64_t  bytes;
    tsmeas_t m;
    ts_begin(&m);
    ray_t* r = q_eval_dot_wrap(fa, 2);
    if (r && ray_is_lazy(r)) r = ray_lazy_materialize(r);
    ts_end(&m, &ms, &bytes);                             /* result still live */
    if (!r) return q_err(QE_TYPE);
    if (RAY_IS_ERR(r)) return r;
    ray_t* ts  = pair_i64((int64_t)ms, bytes);
    ray_t* out = ray_list_new(2);
    out = ray_list_append(out, ts);
    ray_release(ts);
    out = ray_list_append(out, r);
    ray_release(r);
    return out;
}

/* `\t` — timer OR expression timing (basics/syscmds.md), disambiguated by the
 * argument (owner ruling 2026-07-14):
 *   `\t`         getter → current interval as a long (`0` when off, kdb-true).
 *   `\t 0`       stop the repeating timer (silent); works with no poll loop.
 *   `\t N` (N>0) fire `.z.ts` every N ms via a forwarding thunk on the poll
 *                timer heap (silent).  Needs an event poll; under ./qdoctest
 *                there is none → 'io (honest, like `\p`), never a hang.
 *   `\t exp`     a LONE-integer argument is the timer above; ANY other argument
 *                (`\t log til 100000`, the multi-token `\t 2 + 2`) is a timed
 *                expression → run it once, return the elapsed whole ms.
 *   `\t:n exp`   a `:n` suffix (rep >= 0) is ALWAYS expression timing: run exp
 *                n times (`do[n; exp]`), return the TOTAL whole ms.
 * Reentrancy note: `ray_timers_fire_expired` pops the timer before the
 * callback, so a `\t 0`/`\t N` issued from INSIDE `.z.ts` cannot delete the
 * in-flight timer — the thunk's q_sys_timer_active() guard stops a reentrant
 * `\t 0` from re-invoking `.z.ts`; a reentrant interval change may transiently
 * double-fire (documented edge). */
static ray_t* h_t(const char* arg, size_t alen, const char* rest, size_t restlen, int64_t rep) {
    if (alen == 0) return ray_i64(g_timer_ms);           /* getter → bare long */

    /* Timer-vs-expression disambiguation.  A `:n` suffix is always expression
     * timing; without one, only a LONE integer (whole arg region, trailing
     * /comment allowed) is the timer — anything else is a timed expression. */
    bool      expr_form = (rep >= 0);
    int64_t   v         = 0;
    if (!expr_form) {
        if (!parse_i64(arg, alen, &v)) {
            expr_form = true;                            /* non-integer first token */
        } else {
            const char* p   = rest + alen;               /* just past the 1st token */
            const char* end = rest + restlen;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (p < end && *p != '/') expr_form = true;  /* multi-token → `\t 2 + 2` */
        }
    }
    if (expr_form) {                                     /* `\t exp` / `\t:n exp` */
        double  ms;
        int64_t bytes;
        ray_t*  err = time_expr(rest, restlen, rep < 0 ? 1 : rep, &ms, &bytes);
        if (err) return err;
        return ray_i64((int64_t)ms);                     /* kdb shows whole ms */
    }

    if (v < 0) return q_err(QE_DOMAIN);
    /* Overflow guard: ray_timers_add computes now+tic_ms with no check (a UBSan
     * trap on a huge value).  Cap at INT32_MAX ms (~24.8 days) — generous. */
    if (v > 2147483647LL) return q_err(QE_DOMAIN);
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    if (v == 0) {                                        /* stop (silent) */
        if (poll && poll->timers && g_timer_id >= 0)
            ray_timers_del((ray_timers_t*)poll->timers, g_timer_id);
        g_timer_id = -1;
        g_timer_ms = 0;
        return NULL;
    }
    if (!poll) return q_err(QE_IO);             /* no event poll (qdoctest) */
    if (!poll->timers) {
        poll->timers = ray_timers_create(16);
        if (!poll->timers) return q_err(QE_OOM);
    }
    /* Replace any existing timer.  Set state to OFF first so an add-failure
     * below leaves an honest "no timer" state, not a stale id. */
    if (g_timer_id >= 0)
        ray_timers_del((ray_timers_t*)poll->timers, g_timer_id);
    g_timer_id = -1;
    g_timer_ms = 0;
    ray_t* thunk = q_dotz_timer_thunk();                 /* rc=1 */
    if (!thunk || RAY_IS_ERR(thunk)) return thunk ? thunk : q_err(QE_OOM);
    int64_t id = ray_timers_add((ray_timers_t*)poll->timers, v, /*num=*/0, thunk);
    ray_release(thunk);                                  /* add RETAINED its own ref */
    if (id < 0) return q_err(QE_OOM);           /* state already off */
    g_timer_id = id;
    g_timer_ms = v;
    /* Deliberately NO process-keepalive here.  Timers fire whenever the poll
     * loop is already running — a `-p`/`\p` server (its listener keeps it alive)
     * or an interactive/piped REPL (q_repl_run_poll → ray_poll_run pumps the
     * heap between reads).  A headless, listener-less process (`q script.q
     * </dev/null` with no `-p`) exits at end-of-input rather than blocking a
     * server-only loop with nothing to serve — never a hang.  (A timer-keepalive
     * stranded a `\t N`→`\t 0` process in an idle serve loop — codex r1/r2.) */
    return NULL;                                         /* silent */
}

/* `\ts exp` / `\ts:n exp` — time AND space (basics/syscmds.md).  Runs the
 * expression (n times for the `:n` form, kdb `do[n; exp]`) and returns the
 * `(ms; bytes)` 2-long vector rendered by normal q display (`7 2621568`).
 * Unlike `\t`, `\ts` has NO integer-timer form — every argument is a timed
 * expression, including a lone integer (`\ts 42`).  See time_expr for the
 * space-metric divergence from kdb. */
static ray_t* h_ts(size_t alen, const char* rest, size_t restlen, int64_t rep) {
    if (alen == 0) return q_err(QE_NYI);        /* `\ts` needs an expression */
    double  ms;
    int64_t bytes;
    ray_t*  err = time_expr(rest, restlen, rep < 0 ? 1 : rep, &ms, &bytes);
    if (err) return err;
    return pair_i64((int64_t)ms, bytes);               /* `(ms; bytes)` 2-long */
}

/* `\P` — display precision.  `\P`→`7i`; `\P n` sets n∈[0,17] (0 = max = 17),
 * silent.  The float formatter (q_fmt.c) is the sole reader. */
static ray_t* h_P(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(q_fmt_prec());          /* getter */
    int64_t v;
    if (!parse_i64(arg, alen, &v)) return q_err(QE_PARSE);
    /* Range [0,17].  syscmds.md does not specify the out-of-range action, so
     * we make it a silent no-op leaving precision unchanged — we neither
     * corrupt state nor pin an unverified value (rule 9 / clean-room). */
    if (v < 0 || v > 17) return NULL;
    q_fmt_set_prec((int)v);
    return NULL;                                            /* setter: silent */
}

/* `\w` — workspace: 6 longs `used heap peak wmax mmap physical`
 * (syscmds.md#w-workspace).  Reuses ray_mem_stats (src/mem/heap.h) and maps its
 * subset onto the kdb shape; slots without a rayforce counterpart report 0
 * (wmax: no -w limit; physical: not introspected here — deferred).  Values are
 * host-variable; the ledger pins type/count, not the numbers.  `\w 0|1|n` (sym
 * stats / limit-set) is deferred → 'nyi. */
static ray_t* h_w(size_t alen) {
    if (alen != 0) return q_err(QE_NYI);
    ray_mem_stats_t st;
    ray_mem_stats(&st);
    int64_t vals[6] = {
        (int64_t)st.bytes_allocated,   /* 0 used                 */
        (int64_t)st.sys_current,       /* 1 heap                 */
        (int64_t)st.peak_bytes,        /* 2 peak                 */
        0,                             /* 3 wmax (no -w limit)   */
        (int64_t)st.direct_bytes,      /* 4 mmap                 */
        0,                             /* 5 physical (deferred)  */
    };
    ray_t* v = ray_vec_new(RAY_I64, 6);
    if (RAY_IS_ERR(v)) return v;
    for (int i = 0; i < 6; i++) {
        v = ray_vec_append(v, &vals[i]);
        if (RAY_IS_ERR(v)) return v;
    }
    return v;
}

/* `\c` / `\C` — console / HTTP display size (rows cols).  `\c`→`25 80`,
 * `\C`→`36 2000`; a set coerces each value to [10,2000] (syscmds.md).  `\c`
 * clips the q console DISPLAY — its state, coercion and arming now live in
 * q_console.c (q_console_clip_set); this syscmd just forwards.  peachq
 * extension: a `\c` axis may be `0N` — auto, fit the live terminal at render
 * time — and the getter round-trips the SETTING (`25 0N`), not the resolved
 * size.  `\C` (HTTP size) display wrapping is still DEFERRED (peachq has no
 * HTTP renderer yet), so clamp_cc coerces the `\C` values here. */
static int64_t clamp_cc(int64_t v) {
    return v < 10 ? 10 : v > 2000 ? 2000 : v;
}
static ray_t* h_c(const char* rest, size_t restlen) {
    int64_t p[2];
    int cnt = parse_ints_n(rest, restlen, p, 2, 1);
    if (cnt == 0) {                                           /* getter */
        int64_t r, c;
        q_console_clip_setting(&r, &c);
        return pair_i64(r, c);
    }
    if (cnt >= 2)                                             /* setter (coerces + arms) */
        q_console_clip_set(p[0], p[1]);
    return NULL;                                              /* silent */
}

static ray_t* h_C(const char* rest, size_t restlen) {
    int64_t p[2];
    int cnt = parse_ints(rest, restlen, p, 2);
    if (cnt == 0) return pair_i64(g_http_rows, g_http_cols);
    if (cnt >= 2) {
        g_http_rows = (int32_t) clamp_cc(p[0]);
        g_http_cols = (int32_t) clamp_cc(p[1]);
    }
    return NULL;
}

/* `\g` — garbage-collection mode.  `\g`→`0i`; `\g 0|1` sets it.  The mode is
 * ACTED ON solely at the q_ctx statement seam (ctx_statement_end) — kdb also
 * runs .Q.gc[] on the set itself; a `\g 1` line gets that via the seam, a
 * `\g 0` set-time collect is a recorded deferral (one place decides). */
static ray_t* h_g(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(g_gc_mode);
    int64_t v;
    if (!parse_i64(arg, alen, &v)) return q_err(QE_PARSE);
    if (v != 0 && v != 1) return NULL;               /* only 0|1 valid */
    g_gc_mode = (int32_t)v;
    return NULL;
}

int q_sys_gc_mode(void) { return (int)g_gc_mode; }

int q_sys_err_trap_mode(void) { return (int)g_err_trap; }

void q_sys_err_trap_set(int mode) { g_err_trap = (int32_t)mode; }

/* `\o` — offset from UTC (hours; minutes if abs>23).  `\o`→`0N` (machine
 * offset), else the set value.  Temporal-display wiring is DEFERRED. */
static ray_t* h_o(const char* arg, size_t alen) {
    if (alen == 0) return ray_i64(g_utc_offset);     /* 0N default, or set value */
    int64_t v;
    if (!parse_i64(arg, alen, &v)) return q_err(QE_PARSE);
    g_utc_offset = v;
    return NULL;
}

/* `\W` — start-of-week offset (0 = Saturday, default 2 = Monday).  `\W`→`2i`.
 * Week-start wiring into temporal ops is DEFERRED. */
static ray_t* h_W(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(g_week_offset);
    int64_t v;
    if (!parse_i64(arg, alen, &v)) return q_err(QE_PARSE);
    g_week_offset = (int32_t)v;
    return NULL;
}

/* `\e` — error trap CLIENTS (syscmds.md#e-error-trap-clients; owner ruling
 * 2026-08-14: "local calls always suspend, \e should only be client calls").
 * Governs the REMOTE door only: 0 abort+answer (default), 1 suspend on the
 * server console, 2 stack to stderr + answer.  `\e`→`0i`. */
static ray_t* h_e(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(g_err_trap);
    int64_t v;
    if (!parse_i64(arg, alen, &v)) return q_err(QE_PARSE);
    if (v < 0 || v > 2) return NULL;                 /* modes 0|1|2 */
    g_err_trap = (int32_t)v;
    return NULL;
}

/* `\s` — secondary threads.  `\s`→configured count (`0i`).  Runtime re-tuning
 * (and `\s 0N` = show max) is DEFERRED; `\s 0N` returns a parse error for now. */
static ray_t* h_s(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(g_sec_threads);
    int64_t v;
    if (!parse_i64(arg, alen, &v)) return q_err(QE_PARSE);
    g_sec_threads = (int32_t)v;
    return NULL;
}

/* `\classic` — display-mode toggle (peachq; runtime twin of qmain's launch-only
 * `-classic`, reachable from the argv-less wasm REPL).  `1` = classic kdb table
 * display, `0` = modern pipe-table display (the default).  Display ONLY — the
 * startup-time `\l pq` load is not replayed or undone.  Bare form shows the
 * state as a boolean (`1b`/`0b`, like bare `\c`); `1`/`0`/`1b`/`0b` sets it,
 * silent.  A non-boolean arg is a bare `'type` (a boolean is expected). */
static ray_t* h_classic(const char* arg, size_t alen) {
    if (alen == 0) return ray_bool(!q_console_pipe_on());    /* getter → 1b/0b */
    size_t n = (alen == 2 && arg[1] == 'b') ? 1 : alen;   /* strip the 1b/0b literal */
    if (n != 1 || (arg[0] != '0' && arg[0] != '1')) return q_err(QE_TYPE);
    if (arg[0] == '1') q_console_pipe_disable(); else q_console_pipe_enable();
    return NULL;                                     /* setter: silent */
}

/* Raw console shell for an unknown `\cmd` (capture=0): system(3), stdout
 * inherited, returns the raw status as a long (kdb-true `\foo`).  Capability-
 * gated: a runtime that does not own the process (doctest, wasm) does NOT
 * execute and is SILENT — corpus `\ls`/`\curl` rows must never touch the
 * FS / network, and silence is kdb's display for a succeeding command (banked
 * rows like dict/key's `\mkdir foo` -> "" pin it).  `rem`/`rlen` is a SLICE
 * of the console line; copied NUL-terminated before system(). */
static ray_t* sys_shell(const char* rem, size_t rlen) {
    if (!g_own_process) return NULL;
    char   stackbuf[1024];
    char*  cmd = stackbuf;
    ray_t* blk = NULL;
    if (rlen + 1 > sizeof stackbuf) {
        blk = ray_alloc(rlen + 1);
        if (!blk) return q_err(QE_OOM);
        cmd = (char*)ray_data(blk);
    }
    memcpy(cmd, rem, rlen);
    cmd[rlen] = '\0';
    int rc = system(cmd);
    if (blk) ray_free(blk);
    return ray_i64(rc);
}

/* Shell escape for the q `system "…"` STRING form.  Runs the command in the
 * current PROCESS cwd (popen -> /bin/sh -c) and captures its STDOUT as a q
 * LIST of character vectors, one per line, with the line feed and any
 * associated carriage return removed (ref/system.md).  A nonzero shell exit
 * throws 'os (ref/system.md `@[system;"ls egg";…]` -> "error - os"); stderr is
 * NOT captured (popen "r" reads stdout only).  Ownership-heavy: every failure
 * path releases the partial list, the current row, and both scratch buffers. */
static ray_t* sys_shell_capture(const char* rem, size_t rlen) {
    char   stackbuf[1024];
    char*  cmd = stackbuf;
    ray_t* blk = NULL;
    if (rlen + 1 > sizeof stackbuf) {
        blk = ray_alloc(rlen + 1);
        if (!blk) return q_err(QE_OOM);
        cmd = (char*)ray_data(blk);
    }
    memcpy(cmd, rem, rlen);
    cmd[rlen] = '\0';

    FILE* p = popen(cmd, "r");
    if (blk) ray_free(blk);
    if (!p) return q_err(QE_OS);

    /* Slurp all stdout into a growable buffer. */
    size_t cap = 4096, len = 0;
    char*  out = (char*)malloc(cap);
    if (!out) { pclose(p); return q_err(QE_OOM); }
    size_t got;
    char   rbuf[4096];
    while ((got = fread(rbuf, 1, sizeof rbuf, p)) > 0) {
        if (len + got > cap) {
            while (len + got > cap) cap *= 2;
            char* nb = (char*)realloc(out, cap);
            if (!nb) { free(out); pclose(p); return q_err(QE_OOM); }
            out = nb;
        }
        memcpy(out + len, rbuf, got);
        len += got;
    }
    int status = pclose(p);
#ifdef RAY_OS_WINDOWS
    /* _pclose returns the command's exit code directly (-1 on spawn failure),
     * NOT a wait(2)-encoded status — no WIFEXITED/WEXITSTATUS on Windows. */
    if (status != 0) {
#else
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
#endif
        free(out);
        return q_err(QE_OS);                    /* nonzero / signalled */
    }

    /* Split on '\n', dropping a trailing '\r' per line (LF + associated CR
     * removed).  A trailing newline does NOT yield an empty final row. */
    ray_t* list = ray_list_new(1);
    if (RAY_IS_ERR(list)) { free(out); return list; }
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && out[j] != '\n') j++;
        size_t end = j;
        if (end > i && out[end - 1] == '\r') end--;      /* strip associated CR */
        ray_t* row = ray_str(out + i, end - i);
        if (!row || RAY_IS_ERR(row)) { ray_release(list); free(out); return row ? row : q_err(QE_OOM); }
        list = ray_list_append(list, row);               /* retains row */
        ray_release(row);                                /* drop our local ref */
        if (RAY_IS_ERR(list)) { free(out); return list; }
        i = (j < len) ? j + 1 : j;                       /* skip the '\n' */
    }
    free(out);
    return list;                                         /* empty stdout -> empty list */
}

/* The q-owned `system "…"` verb: prepend `\` and PASS THROUGH q_sys_run —
 * `system "X"` ≡ `\X` for every command, one path.  capture=1: an unknown
 * token shells via popen, stdout -> list of char vectors.  `system` is a
 * restricted primitive under IPC reval (kdb blocks it) -> 'access. */
ray_t* q_system_fn(ray_t* x) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    const char* sp; int64_t sn;
    if (!q_str_text_bytes(x, &sp, &sn)) return q_err(QE_TYPE);
    size_t sl = (size_t)sn;

    char   stackbuf[1024];
    char*  buf = stackbuf;
    ray_t* blk = NULL;
    if (sl + 2 > sizeof stackbuf) {                      /* '\' + cmd + NUL */
        blk = ray_alloc(sl + 2);
        if (!blk) return q_err(QE_OOM);
        buf = (char*)ray_data(blk);
    }
    buf[0] = '\\';
    if (sl) memcpy(buf + 1, sp, sl);
    buf[sl + 1] = '\0';

    ray_t* out = q_sys_run(buf, sl + 1, 1);
    if (blk) ray_free(blk);
    if (!out) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }  /* silent -> generic null */
    return q_str_charv_out(out);            /* captured lines cross as char vectors */
}

bool q_sys_is_cmd(const char* line, size_t n) {
    size_t i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    return i < n && line[i] == '\\';
}

ray_t* q_sys_run(const char* line, size_t n, int capture) {
    size_t i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= n || line[i] != '\\') return q_err(QE_TYPE);  /* caller guard: q_sys_is_cmd */
    i++;

    /* Command token = run of chars that are neither whitespace nor `:`.  The
     * `:` stop resolves kdb's repetition suffix (\t:100, \ts:10000) to the base
     * command.  The remainder (token..EOL) is the slice a miss hands back. */
    size_t rem0 = i;
    size_t c0 = i;
    while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != ':') i++;
    const char* cmd = line + c0;
    size_t cmd_len = i - c0;

    /* Optional `:`-repetition suffix (\t:100, \ts:10000) — its count is the
     * `do[n; exp]` reps for the timing commands; rep = -1 means no suffix (a
     * non-integer/negative one also leaves -1, so the handler takes the
     * non-repeat form).  Then the first whitespace-delimited token (arg/alen)
     * and the WHOLE argument region (rest/restlen, first token through EOL,
     * trailing `/ comment` included — parsed by the multi-token commands). */
    int64_t rep = -1;
    if (i < n && line[i] == ':') {
        i++;                                         /* skip the ':' */
        size_t d0 = i;
        while (i < n && line[i] != ' ' && line[i] != '\t') i++;
        int64_t rv;
        if (parse_i64(line + d0, i - d0, &rv) && rv >= 0) rep = rv;
    }
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t a0 = i;
    const char* rest = line + a0;
    size_t restlen = n - a0;
    while (i < n && line[i] != ' ' && line[i] != '\t') i++;
    const char* arg = line + a0;
    size_t alen = i - a0;
    /* A LONE `/` token is a bare trailing comment (`\P / default`) → no arg.
     * A multi-char `/…` token is a file PATH (`\l /tmp/db`) → keep it. */
    if (alen == 1 && arg[0] == '/') alen = 0;

    /* Dispatch on the command TOKEN — a switch on the single command char in
     * the common case, a short length+char check for the multi-char forms
     * (\ts \cd \classic).  Case-sensitive (\c ≠ \C).  Each case passes ONLY
     * the arguments its handler needs (no uniform
     * signature).  An unmatched single char (no case, no default) or any other
     * unrecognized token falls through to the shell-out; bare `\` is the silent
     * q/k toggle (deferred, no k mode). */
    if (cmd_len == 1) {
        switch (cmd[0]) {
            case 'd': return h_d(arg, alen);
            case 'v':                                            /* namespace vars      */
            case 'f':                                            /* namespace functions */
            case 'a': return h_vfa(cmd[0], arg, alen);            /* namespace tables    */
            case 'S': return h_S(arg, alen);                     /* random seed         */
            case 'P': return h_P(arg, alen);                     /* display precision   */
            case 'c': return h_c(rest, restlen);                 /* console size        */
            case 'C': return h_C(rest, restlen);                 /* HTTP display size   */
            case 'o': return h_o(arg, alen);                     /* offset from UTC     */
            case 'g': return h_g(arg, alen);                     /* gc mode             */
            case 's': return h_s(arg, alen);                     /* secondary threads   */
            case 'W': return h_W(arg, alen);                     /* week offset         */
            case 'e': return h_e(arg, alen);                     /* error-trap mode     */
            case 'w': return h_w(alen);                          /* workspace stats     */
            case 'l': return h_l(arg, alen);                     /* load q script       */
            case 'p': return h_p(arg, alen);                     /* listening port      */
            case 't': return h_t(arg, alen, rest, restlen, rep); /* timer / \t exp      */
            case 'b': return h_bB(0, arg, alen);                 /* views               */
            case 'B': return h_bB(1, arg, alen);                 /* pending views       */
            case 'E': return h_E(arg, alen);                     /* TLS server mode     */
            case '1': return h_redirect(1, arg, alen);           /* stdout redirect     */
            case '2': return h_redirect(2, arg, alen);           /* stderr redirect     */
            /* Silent setter/action form (arg present) → NULL; getter → 'nyi:
             * \z date-parse, \r replicate, \T timeout, \u user-pwd,
             * \x expunge, \_ hide-q-code. */
            case 'z': case 'r': case 'T':
            case 'u': case 'x': case '_':
                return h_getset(alen);
            /* \\ quit — q_sys_exit is capability-gated (a real process exits firing
             * .z.exit; an embedder returns silently, kdb-true either way). */
            case '\\': q_sys_exit(0); return NULL;
        }
    } else if (cmd_len == 2 && cmd[0] == 't' && cmd[1] == 's') {
        return h_ts(alen, rest, restlen, rep);                   /* time and space      */
    } else if (cmd_len == 2 && cmd[0] == 'c' && cmd[1] == 'd') {
        return h_cd(arg, alen);                                  /* change directory    */
    } else if (cmd_len == 7 && memcmp(cmd, "classic", 7) == 0) {
        return h_classic(arg, alen);                            /* display-mode toggle (peachq) */
    } else if (cmd_len == 0) {
        return NULL;                                            /* bare \ — silent q/k toggle */
    }

    /* Unrecognized command → shell out on the raw remainder (token..EOL). */
    return capture ? sys_shell_capture(line + rem0, n - rem0)
                   : sys_shell(line + rem0, n - rem0);
}

/* See q_sys.h — the shared console glue.  Console side effects come FIRST in
 * buf, then the value (`\h`'s doc lines, `\t exp`'s show/0N! output precede a
 * result), matching the eval path's display order in every adapter. */
ray_t* q_sys_line(const char* line, size_t n, int print_value,
                  char* buf, size_t cap) {
    if (cap) buf[0] = '\0';
    ray_t* v = q_sys_run(line, n, 0);
    const char* con = q_console_str();
    size_t used = 0;
    if (cap && con && *con) {
        used = strlen(con);
        if (used >= cap) used = cap - 1;
        memcpy(buf, con, used);
        buf[used] = '\0';
    }
    q_console_reset();
    if (v && RAY_IS_ERR(v)) return v;
    if (v) {
        if (print_value && !RAY_IS_NULL(v) && used < cap)
            q_fmt_console(v, buf + used, cap - used);
        ray_release(v);
    }
    return NULL;
}

/* ---- the process-environment verbs: getenv / setenv --------------------
 * (moved off ops/q_io.c 2026-07-31 — process environment, not files.) */

/* q `getenv x` (ref/getenv.md) — x is a SYMBOL atom naming an environment
 * variable; returns its value as a string, or "" when the variable is unset
 * (kdb-true, and exactly what the base ray_getenv_fn already returns for a
 * missing var).  The base primitive wants a -RAY_STR arg, so coerce the
 * symbol's name to a string atom first — the ONLY divergence from the raw C,
 * hence a wrapper rather than a QK_ENV rename.
 * String-model seam: the result is a native -RAY_STR atom, so `type getenv`X`
 * is -10h where kdb's char vector is 10h (a known, tracked divergence). */
ray_t* q_getenv_wrap(ray_t* x) {
    /* .os.getenv is RAY_FN_RESTRICTED; calling the C fn directly bypasses the
     * eval-layer check, so re-assert it here (the q_hopen_wrap/file precedent). */
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    if (!x || x->type != -RAY_SYM)
        return q_err(QE_TYPE);
    ray_t* s = ray_sym_str(x->i64);                     /* borrowed */
    if (!s) return q_err(QE_TYPE);
    ray_t* name = ray_str(ray_str_ptr(s), ray_str_len(s));  /* owned -RAY_STR */
    if (!name || RAY_IS_ERR(name)) return name ? name : q_err(QE_OOM);
    ray_t* r = ray_getenv_fn(name);                     /* "" when unset */
    ray_release(name);
    return q_str_charv_out(r);
}

/* q `x setenv y` (ref/getenv.md#setenv) — x is a SYMBOL atom (the variable
 * name), y is a string.  Sets the environment variable and returns generic
 * null (kdb: setenv's result displays as nothing in the console).  The base
 * ray_setenv_fn takes two -RAY_STR args and echoes y retained; coerce the sym
 * name to a string, discard that echo, and return :: to match kdb. */
static ray_t* setenv_impl(ray_t* x, ray_t* y);
ray_t* q_setenv_wrap(ray_t* x, ray_t* y) {
    ray_t* ys = q_str_in(y);
    ray_t* r = setenv_impl(x, ys);
    ray_release(ys);
    return r;
}
static ray_t* setenv_impl(ray_t* x, ray_t* y) {
    /* .os.setenv is RAY_FN_RESTRICTED; re-assert here (calling the C fn directly
     * bypasses the eval-layer check — the q_hopen_wrap/file-wrapper precedent). */
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    if (!x || x->type != -RAY_SYM)
        return q_err(QE_TYPE);
    if (!y || y->type != -RAY_STR)
        return q_err(QE_TYPE);
    ray_t* s = ray_sym_str(x->i64);                     /* borrowed */
    if (!s) return q_err(QE_TYPE);
    ray_t* name = ray_str(ray_str_ptr(s), ray_str_len(s));  /* owned -RAY_STR */
    if (!name || RAY_IS_ERR(name)) return name ? name : q_err(QE_OOM);
    ray_t* r = ray_setenv_fn(name, y);                  /* echoes y, or error */
    ray_release(name);
    if (r && RAY_IS_ERR(r)) return r;
    if (r) ray_release(r);                              /* discard echoed value */
    return RAY_NULL_OBJ;                                /* kdb: setenv -> :: */
}
