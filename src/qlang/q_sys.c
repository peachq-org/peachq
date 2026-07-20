/* q_sys — see q_sys.h.  The unified `\`-command dispatcher: one line pre-parse
 * (command token / `:n` repeat suffix / first-arg token / whole argument
 * region) feeding a SWITCH on the command char (the q_dotz.c z_slot_ptr idiom),
 * each case calling its handler with only the arguments it needs.  A handler
 * returns an OWNED value (NULL = silent) or an OWNED error — including `\\`
 * (q_exit) and the unknown-token shell miss, both gated by the g_own_process
 * capability rather than by the caller.  \d and \v/\f/\a lean on q_ns.c
 * (context state + member enumeration); \S owns its seed state here. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_sys.h"
#include "qlang/q_ns.h"       /* q_ns_current / q_ns_switch / q_ns_list */
#include "qlang/q_ops.h"      /* q_ops_find — the `\h` doc lookup */
#include "qlang/q_fmt.h"      /* q_fmt_set_prec/q_fmt_prec (`\P`); q_console_str/reset (timed-expr side effects) */
#include "qlang/q_fmt_pipe.h" /* q_pipe_on/enable/disable — `\nonlegacy` display toggle */
#include "qlang/q_repl.h"     /* q_repl_mark_listener_active / q_repl_run_file */
#include "qlang/q_pq.h"       /* q_pq_load — the `\l pq` embedded-stdlib gate */
#include "qlang/q_dotz.h"     /* q_dotz_timer_thunk — the `.z.ts` timer callback */
#include "qlang/q_parse.h"    /* q_parse / q_lower — `\t expr` / `\ts expr` timing */
#include "core/ipc.h"         /* ray_ipc_listen — `\p N` binds a listener */
#include "core/poll.h"        /* ray_poll_get / deregister — `\p 0W`/`\p 0`; poll->timers */
#include "core/runtime.h"     /* ray_runtime_get_poll — the runtime event poll */
#include "core/numparse.h"    /* ray_parse_i64 — the shared engine int parser (reuse) */
#include "core/timer.h"       /* ray_timers_create/add/del — `\t N` timer heap */
#include "core/profile.h"     /* ray_profile_now_ns — `\t`/`\ts` wall clock */
#include "lang/eval.h"        /* ray_eval / ray_eval_get_restricted — timing + guard */
#include "ops/ops.h"          /* ray_is_lazy / ray_lazy_materialize — timed-expr result */
#include <rayforce.h>
#include "mem/heap.h"         /* ray_mem_stats / ray_mem_stats_t — `\w` reuse */
#include <stdlib.h>           /* srand, system, malloc */
#include <string.h>           /* strlen, memcpy, memcmp */
#include <stdio.h>            /* popen / pclose — `system "…"` stdout capture */
#include <ctype.h>           /* tolower — `\h` case-insensitive fuzzy search */
#include <unistd.h>          /* chdir / getcwd / access — `\cd`, `\l` */
#include <limits.h>          /* PATH_MAX */
#include <sys/stat.h>        /* stat / S_ISREG — `\l` regular-file gate */
#ifndef RAY_OS_WINDOWS
#include <sys/wait.h>        /* WIFEXITED / WEXITSTATUS — shell-capture status */
#include "qlang/q_registry.h"   /* q_text_bytes — charv/string text accessor */
#endif                       /* mingw has no <sys/wait.h>; _pclose gives the code directly */

/* `\p 0W` reads the OS-chosen port back off the listener fd (getsockname),
 * mirroring qmain.c's startup `-p 0W` path — the two share one readiness line. */
#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>       /* socklen_t */
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
#endif

/* Reused base fn (src/ops/system.c), declared not edited (frozen-clean): the
 * gc trigger is a no-op stub returning 0 — `\g` reports the mode; real
 * collection is deferred (rule 9). */
ray_t* ray_gc_fn(ray_t** args, int64_t n);

/* ---- `\S` random-seed state (moved from q_ns.c; \S is its only consumer) ----
 * kdb re-initializes its rng to a CONSTANT seed at startup (-314159i,
 * basics/syscmds.md) so scripts using Roll/Deal/rand repeat.  ALL openq
 * randomness (`?` roll/deal/permute + generate arms, `rand`) funnels through
 * libc rand(), so srand IS the whole contract — except the guid generator
 * (src/ops/system.c xorshift64*), which seeds lazily from rand() per thread:
 * `\S` makes guid sequences reproducible only if set before the thread's first
 * guid use (recorded caveat, not fixed here).
 * `\S` displays the LAST-INITIALIZED seed, never evolving rng state; reading
 * the live state (`\S 0N`, V3.6) has no libc counterpart -> 'nyi. */
static int32_t g_last_seed = -314159;

void q_sys_seed_init(void) {
    g_last_seed = -314159;
    srand((unsigned)-314159);
}

/* ---- Tier-2 config state (kdb `\`-command get/set) -------------------------
 * Each command's handler is the single WRITER; its getter reports the stored
 * value.  q_sys_cfg_init resets everything to the kdb default per runtime
 * (called by q_runtime_create), so a `\c 5 5` in one .qcmd file never leaks
 * into the next (the doctest runner builds a fresh runtime per file).
 *
 * BEHAVIOURAL SIDE-EFFECTS ARE MOSTLY DEFERRED (rule 9): `\c` NOW clips the q
 * console DISPLAY (width + height, applied by q_fmt.c's console emitter — see
 * q_fmt_console), but openq does not yet wrap `\C` HTTP output, run real gc
 * for `\g`, apply `\o`/`\W` to temporal display, trap errors for `\e`, or
 * re-tune worker threads for `\s`.  Those commands store + report the kdb-true
 * value only; the effect itself is a tracked PLAN.md gap.  Faking a
 * side-effect would be worse than an honest store-and-report. */
static int32_t g_con_rows,  g_con_cols;   /* \c console size  (default 25 80)   */
static int32_t g_http_rows, g_http_cols;  /* \C HTTP size     (default 36 2000) */

/* Whether the q console DISPLAY truncates by `\c` (width per line + a height
 * row-cap), applied by q_fmt.c's console emitter (q_fmt_console).  ON by
 * default (kdb clips a fresh console at 25 80).  The ONE carve-out is a pure
 * non-tty SCRIPT LOAD (`./q file.q </dev/null`, the qscript harness) — a batch
 * context, NOT a display — where qmain DISARMS clipping so a script's
 * `show`/`.z.f` renders full-width.  The doctest runner + the interactive REPL
 * leave it ON (fresh-per-runtime default). */
static int32_t g_con_trunc;               /* 0 = unlimited display, 1 = clip by \c */
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
    g_con_rows  = 25; g_con_cols  = 80;
    g_con_trunc = 1;             /* display clipping ARMED at the 25 80 default */
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
 * reentrant q_exit from inside the handler skips it and exits with the
 * ORIGINAL code (dotz.md: "The handler cannot cancel the exit"). */
static int g_exit_code;
void q_exit(int code) {
    if (!g_own_process) return;
    if (g_exiting) exit(g_exit_code);
    g_exiting  = 1;
    g_exit_code = code;
    q_repl_console_close();
    q_dotz_exit_fire(code);
    exit(code);
}

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Whole-span signed-int parse: 1 iff ray_parse_i64 (numparse.c) consumes ALL of [s,s+len). */
static int q_parse_i64(const char* s, size_t len, int64_t* out) {
    return len > 0 && ray_parse_i64(s, len, out) == len;
}

/* Parse up to `max` whitespace-separated base-10 integers from [s,s+len),
 * stopping at the first non-integer token (e.g. a trailing `/ comment`).
 * Returns the count stored into out[]. */
static int q_parse_ints(const char* s, size_t len, int64_t* out, int max) {
    int cnt = 0; size_t i = 0;
    while (cnt < max && i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        size_t t0 = i;
        while (i < len && s[i] != ' ' && s[i] != '\t') i++;
        if (i == t0) break;
        int64_t v;
        if (!q_parse_i64(s + t0, i - t0, &v)) break;
        out[cnt++] = v;
    }
    return cnt;
}

/* Build a two-element typed vector for the pair-valued getters (`\c`/`\C`). */
static ray_t* q_pair_i64(int64_t a, int64_t b) {
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
        const char* c = q_ns_current();
        ray_t* s = (*c) ? ray_sym(ray_sym_intern(c, strlen(c)))
                        : ray_sym(ray_sym_intern(".", 1));
        /* DATA sym, not a name-ref: keeps its backtick in q_fmt (`.) */
        if (s && !RAY_IS_ERR(s)) s->attrs |= 0x20; /* Q_ATTR_QUOTED */
        return s;
    }
    return q_ns_switch(arg, alen);               /* NULL (silent) or error */
}

static ray_t* h_S(const char* arg, size_t alen) {
    if (alen == 0)                               /* `\S` — last-initialized seed */
        return ray_i32(g_last_seed);
    if (alen == 2 && arg[0] == '0' && arg[1] == 'N')  /* `\S 0N` — live state */
        return ray_error("nyi", "\\S 0N: libc rand state is not readable");
    int64_t v;
    if (!q_parse_i64(arg, alen, &v))
        return ray_error("parse", NULL);         /* non-integer arg (unpinned) */
    /* The seed is an INT (`\S` displays it as one): out-of-int-range values
     * and the 0Ni sentinel are rejected, never silently truncated (codex P2,
     * 2026-07-09). */
    if (v <= INT32_MIN || v > INT32_MAX)
        return ray_error("parse", NULL);
    srand((unsigned)v);                          /* `\S n` — re-initialize */
    g_last_seed = (int32_t)v;
    return NULL;                                 /* silent, like `\d ns` */
}

/* ---- shared handlers for the still-unimplemented commands ------------------
 * After the dedicated handlers below (\P \c \C \w \g \o \W \e \s, \l \cd \p,
 * \t \ts), h_getset serves the REMAINING commands whose SETTER / ACTION form
 * (arg present) prints NOTHING in kdb — date-parse (\z), file/name actions
 * (\x \r \1 \2 \_), and timeout/TLS/user config (\T \E \u).  We accept the
 * arg-form as a silent no-op: the OUTPUT matches kdb's empty setter output, so
 * the frozen ledger rows that bank that silence (e.g. `\z 1`) stay green.  The
 * side-effect is a tracked gap; the GETTER form (no arg, which kdb prints a
 * value for) can't yet report the value → honest 'nyi.
 *
 * The remaining commands that print a VALUE even in their arg-form or are
 * getter-only — \B (pending views) — inline `ray_error("nyi", NULL)` in the
 * switch: there is no silent form to match, so 'nyi is both honest and non-
 * regressing, the observable "known but not implemented" signal distinct from
 * an unknown-token shell-out. */
static ray_t* h_getset(size_t alen) {
    if (alen > 0) return NULL;                   /* setter/action form → silent */
    return ray_error("nyi", NULL);               /* getter form → not yet */
}

/* `\b` — views.  openq has no view mechanism; the owner ruling (2026-07-15) is
 * that \b always reports an empty list "for now".  This is a DELIBERATE LIE
 * (PLAN.md, Known defects): an empty listing is indistinguishable from "no views
 * defined", so `if[count views[];..]` gets a confidently wrong answer where 'nyi
 * was honest.  The namespace arg is accepted and ignored — nothing to filter.
 * Shape mirrors \a's empty listing (ns_members, q_ns.c): len 0, capacity 1. */
static ray_t* h_b(void) {
    return ray_sym_vec_new(RAY_SYM_W64, 1);
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
    q_repl_mark_listener_active();                       /* keep the process alive past stdin EOF */
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
 *                        poll; the unified REPL loop (q_repl.c) serves it, and
 *                        q_repl_mark_listener_active keeps the process alive past
 *                        stdin EOF (a client that `\p`s a port becomes a server).
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
static ray_t* h_p(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(g_listen_port);        /* getter -> `0i` default */
    /* `0W`/`0w` auto token (same shape as h_S's `0N` probe) — bind port 0. */
    bool port_auto = (alen == 2 && arg[0] == '0' && (arg[1] == 'W' || arg[1] == 'w'));
    int64_t v = 0;
    if (!port_auto) {
        if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
        if (v < 0 || v > 65535) return ray_error("domain", NULL);
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
    if (!q_sys_listen(port_auto ? 0 : (uint16_t)v)) return ray_error("io", NULL);
    return NULL;                                          /* setter: silent */
}

/* `\cd` — change directory (basics/syscmds.md).  getter -> current directory as
 * a char vector; `\cd fp` -> real chdir (the kx "wrong directory" footgun fix,
 * ARCHITECTURE decision: cwd must be controllable + predictable).  DEFERRED:
 * kdb's create-if-missing on a set (a missing target -> 'os here). */
static ray_t* h_cd(const char* arg, size_t alen) {
    if (alen == 0) {                                     /* getter: current dir */
        char buf[PATH_MAX];
        if (!getcwd(buf, sizeof buf)) return ray_error("os", NULL);
        return ray_str(buf, strlen(buf));                /* char vector */
    }
    if (alen >= PATH_MAX) return ray_error("os", NULL);
    char path[PATH_MAX];
    memcpy(path, arg, alen); path[alen] = '\0';
    if (chdir(path) != 0) return ray_error("os", NULL);
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
 * q_repl_run_file (multiline-aware; silent — kdb loads silently).  A
 * still-missing path or a DIRECTORY stays a silent no-op: this preserves the
 * banked `\l .` / `\l /tmp/db*` corpus rows (absent / directory targets) and
 * defers the directory / splayed-table / serialized-object load forms.  Getter
 * form (no arg) stays 'nyi.  `system "l …"` single-homes through here. */
static ray_t* h_l(const char* arg, size_t alen) {
    if (alen == 0) return ray_error("nyi", NULL);        /* `\l` (bare) — reload cwd, deferred */
    if (alen >= PATH_MAX) return NULL;
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
    if (ok) { q_repl_run_file(found, stdout, stderr); return NULL; }  /* disk hit — load (silent) */
    /* openq: `\l pq` — the PeachQ stdlib gate. A dev-override disk file (the
     * a/b/c/d chain above) wins; else a cwd directory literally named `pq`
     * keeps existing dir semantics (no-op, below); ELSE the embedded stdlib.
     * Every OTHER argument keeps its existing behaviour unchanged (the branch is
     * scoped to the exact literal `pq`). */
    if (alen == 2 && arg[0] == 'p' && arg[1] == 'q') {
        struct stat st;
        if (!(stat("pq", &st) == 0 && S_ISDIR(st.st_mode))) {   /* not a `pq` dir → embedded */
            q_pq_load();
            return NULL;
        }
    }
    return NULL;                                          /* missing/dir/unreadable — silent */
}

/* ---- expression timing (`\t expr`, `\t:n`, `\ts expr`, `\ts:n`) ------------
 * Time a q expression string by running it through the SAME pipeline the REPL
 * uses (q_parse → q_lower → ray_eval), `reps` times, discarding each result.
 * On success fills *ms with the TOTAL wall-clock milliseconds and *bytes with
 * the space metric, then returns NULL; a parse/lower/eval error returns the
 * OWNED error instead (ms/bytes untouched).  The `:n` repetition form is kdb's
 * `do[n; e]` — execution repeated, so we parse+lower ONCE and re-evaluate the
 * lowered tree (ray_eval is read-only on the AST; re-running it re-runs the
 * program, assignments and all).  Runtime dispatch is allowed to parse here —
 * the rule-6 prohibition is on registry BUILDERS, not a warm-registry handler.
 *
 * SPACE METRIC — DIVERGES FROM kdb (owner ruling 2026-07-14, best-effort):
 * kdb's `\ts` reports the PEAK transient workspace a computation touches; openq
 * reports the NET `ray_mem_stats().bytes_allocated` delta measured with the
 * final result still live (snapshot taken before release).  So an expression
 * whose result is retained (`til 100000`) shows a positive figure in the
 * ballpark of that result's size, but transient intermediates freed mid-eval
 * are NOT counted, and the ASan debug allocator inflates the number versus a
 * release build.  Tests pin only shape + sign (space > 0 for an allocating
 * expr), never a golden byte count. */
static ray_t* q_time_expr(const char* expr, size_t len, int64_t reps,
                          double* ms, int64_t* bytes) {
    if (reps < 0) reps = 0;                              /* `\t:0` → do[0;e] = no runs */
    /* q_parse wants a NUL-terminated C string; expr is a slice of the line. */
    char   stackbuf[1024];
    char*  s   = stackbuf;
    ray_t* blk = NULL;
    if (len + 1 > sizeof stackbuf) {
        blk = ray_alloc(len + 1);
        if (!blk) return ray_error("oom", NULL);
        s = (char*)ray_data(blk);
    }
    memcpy(s, expr, len);
    s[len] = '\0';

    ray_t* ast = q_parse(s);                             /* strips a trailing /comment */
    if (blk) ray_free(blk);
    if (RAY_IS_ERR(ast)) return ast;
    ast = q_lower(ast);
    if (RAY_IS_ERR(ast)) return ast;

    ray_mem_stats_t before, after;
    ray_mem_stats(&before);
    int64_t t0  = ray_profile_now_ns();
    ray_t*  r   = NULL;
    ray_t*  err = NULL;
    for (int64_t k = 0; k < reps; k++) {
        if (r) { ray_release(r); r = NULL; }             /* keep only the last result */
        r = ray_eval(ast);
        if (ray_is_lazy(r)) r = ray_lazy_materialize(r);
        if (r && RAY_IS_ERR(r)) { err = r; r = NULL; break; }
    }
    int64_t t1 = ray_profile_now_ns();
    ray_mem_stats(&after);                               /* last result still live */
    ray_release(ast);
    if (r) ray_release(r);
    if (err) return err;                                 /* propagate the eval error */
    *ms = (double)(t1 - t0) / 1e6;
    int64_t d = (int64_t)after.bytes_allocated - (int64_t)before.bytes_allocated;
    *bytes = d < 0 ? 0 : d;
    return NULL;
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
        if (!q_parse_i64(arg, alen, &v)) {
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
        ray_t*  err = q_time_expr(rest, restlen, rep < 0 ? 1 : rep, &ms, &bytes);
        if (err) return err;
        return ray_i64((int64_t)ms);                     /* kdb shows whole ms */
    }

    if (v < 0) return ray_error("domain", NULL);
    /* Overflow guard: ray_timers_add computes now+tic_ms with no check (a UBSan
     * trap on a huge value).  Cap at INT32_MAX ms (~24.8 days) — generous. */
    if (v > 2147483647LL) return ray_error("domain", NULL);
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    if (v == 0) {                                        /* stop (silent) */
        if (poll && poll->timers && g_timer_id >= 0)
            ray_timers_del((ray_timers_t*)poll->timers, g_timer_id);
        g_timer_id = -1;
        g_timer_ms = 0;
        return NULL;
    }
    if (!poll) return ray_error("io", NULL);             /* no event poll (qdoctest) */
    if (!poll->timers) {
        poll->timers = ray_timers_create(16);
        if (!poll->timers) return ray_error("oom", NULL);
    }
    /* Replace any existing timer.  Set state to OFF first so an add-failure
     * below leaves an honest "no timer" state, not a stale id. */
    if (g_timer_id >= 0)
        ray_timers_del((ray_timers_t*)poll->timers, g_timer_id);
    g_timer_id = -1;
    g_timer_ms = 0;
    ray_t* thunk = q_dotz_timer_thunk();                 /* rc=1 */
    if (!thunk || RAY_IS_ERR(thunk)) return thunk ? thunk : ray_error("oom", NULL);
    int64_t id = ray_timers_add((ray_timers_t*)poll->timers, v, /*num=*/0, thunk);
    ray_release(thunk);                                  /* add RETAINED its own ref */
    if (id < 0) return ray_error("oom", NULL);           /* state already off */
    g_timer_id = id;
    g_timer_ms = v;
    /* Deliberately NO process-keepalive here.  Timers fire whenever the poll
     * loop is already running — a `-p`/`\p` server (its listener keeps it alive)
     * or an interactive/piped REPL (q_repl_run_poll → ray_poll_run pumps the
     * heap between reads).  A headless, listener-less process (`q script.q
     * </dev/null` with no `-p`) exits at end-of-input rather than blocking a
     * server-only loop with nothing to serve — never a hang.  (Reusing the
     * irreversible listener-active flag OR a timer-keepalive both stranded a
     * `\t N`→`\t 0` process in an idle serve loop — codex r1/r2.) */
    return NULL;                                         /* silent */
}

/* `\ts exp` / `\ts:n exp` — time AND space (basics/syscmds.md).  Runs the
 * expression (n times for the `:n` form, kdb `do[n; exp]`) and returns the
 * `(ms; bytes)` 2-long vector rendered by normal q display (`7 2621568`).
 * Unlike `\t`, `\ts` has NO integer-timer form — every argument is a timed
 * expression, including a lone integer (`\ts 42`).  See q_time_expr for the
 * space-metric divergence from kdb. */
static ray_t* h_ts(size_t alen, const char* rest, size_t restlen, int64_t rep) {
    if (alen == 0) return ray_error("nyi", NULL);        /* `\ts` needs an expression */
    double  ms;
    int64_t bytes;
    ray_t*  err = q_time_expr(rest, restlen, rep < 0 ? 1 : rep, &ms, &bytes);
    if (err) return err;
    return q_pair_i64((int64_t)ms, bytes);               /* `(ms; bytes)` 2-long */
}

/* `\P` — display precision.  `\P`→`7i`; `\P n` sets n∈[0,17] (0 = max = 17),
 * silent.  The float formatter (q_fmt.c) is the sole reader. */
static ray_t* h_P(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(q_fmt_prec());          /* getter */
    int64_t v;
    if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
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
    if (alen != 0) return ray_error("nyi", NULL);
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
 * clips the q console DISPLAY (q_fmt.c q_fmt_console: width per line + a
 * height row-cap); a `\c size` setter (re-)ARMS clipping (g_con_trunc) so a
 * transcript that set it stays clipped.  `\C` (HTTP size) display wrapping is
 * still DEFERRED (openq has no HTTP renderer yet). */
static int64_t q_clamp_cc(int64_t v) {
    return v < 10 ? 10 : v > 2000 ? 2000 : v;
}
static ray_t* h_c(const char* rest, size_t restlen) {
    int64_t p[2];
    int cnt = q_parse_ints(rest, restlen, p, 2);
    if (cnt == 0) return q_pair_i64(g_con_rows, g_con_cols);   /* getter */
    if (cnt >= 2) {                                            /* setter */
        g_con_rows = (int32_t)q_clamp_cc(p[0]);
        g_con_cols = (int32_t)q_clamp_cc(p[1]);
        g_con_trunc = 1;                    /* an explicit \c (re-)arms clipping */
    }
    return NULL;                                              /* silent */
}

/* Display-clipping accessors (read by q_fmt.c's console emitter).
 * q_con_display fills the rows/cols out-params with the live `\c` size and
 * returns true iff clipping is armed. */
bool q_con_display(int32_t* rows, int32_t* cols) {
    if (rows) *rows = g_con_rows;
    if (cols) *cols = g_con_cols;
    return g_con_trunc != 0;
}
/* Disarm display clipping — qmain calls this for a pure non-tty SCRIPT LOAD (a
 * batch context, so `.z.f`/script output renders full-width). */
void q_con_display_disable(void) { g_con_trunc = 0; }
static ray_t* h_C(const char* rest, size_t restlen) {
    int64_t p[2];
    int cnt = q_parse_ints(rest, restlen, p, 2);
    if (cnt == 0) return q_pair_i64(g_http_rows, g_http_cols);
    if (cnt >= 2) {
        g_http_rows = (int32_t)q_clamp_cc(p[0]);
        g_http_cols = (int32_t)q_clamp_cc(p[1]);
    }
    return NULL;
}

/* `\g` — garbage-collection mode.  `\g`→`0i`; `\g 0|1` sets it.  kdb calls
 * .Q.gc[] on a set; openq's ray_gc_fn is a no-op stub (real gc deferred). */
static ray_t* h_g(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(g_gc_mode);
    int64_t v;
    if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
    if (v != 0 && v != 1) return NULL;               /* only 0|1 valid */
    g_gc_mode = (int32_t)v;
    ray_t* g = ray_gc_fn(NULL, 0);                   /* stub call (reuse) */
    if (g) ray_release(g);
    return NULL;
}

/* `\o` — offset from UTC (hours; minutes if abs>23).  `\o`→`0N` (machine
 * offset), else the set value.  Temporal-display wiring is DEFERRED. */
static ray_t* h_o(const char* arg, size_t alen) {
    if (alen == 0) return ray_i64(g_utc_offset);     /* 0N default, or set value */
    int64_t v;
    if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
    g_utc_offset = v;
    return NULL;
}

/* `\W` — start-of-week offset (0 = Saturday, default 2 = Monday).  `\W`→`2i`.
 * Week-start wiring into temporal ops is DEFERRED. */
static ray_t* h_W(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(g_week_offset);
    int64_t v;
    if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
    g_week_offset = (int32_t)v;
    return NULL;
}

/* `\e` — error-trap mode (0 off / 1 suspend / 2 dump).  `\e`→`0i`.  The trap
 * behaviour itself is DEFERRED. */
static ray_t* h_e(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(g_err_trap);
    int64_t v;
    if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
    if (v < 0 || v > 2) return NULL;                 /* modes 0|1|2 */
    g_err_trap = (int32_t)v;
    return NULL;
}

/* `\s` — secondary threads.  `\s`→configured count (`0i`).  Runtime re-tuning
 * (and `\s 0N` = show max) is DEFERRED; `\s 0N` returns a parse error for now. */
static ray_t* h_s(const char* arg, size_t alen) {
    if (alen == 0) return ray_i32(g_sec_threads);
    int64_t v;
    if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
    g_sec_threads = (int32_t)v;
    return NULL;
}

/* `\nonlegacy` — pipe-table display toggle (openq; runtime twin of #198's
 * launch-only `--nonlegacy`, reachable from the argv-less wasm REPL).  Bare form
 * shows the state as a boolean (`1b`/`0b`, like bare `\c`); `1`/`0`/`1b`/`0b`
 * sets it, silent.  A non-boolean arg is a bare `'type` (a boolean is expected). */
static ray_t* h_nonlegacy(const char* arg, size_t alen) {
    if (alen == 0) return ray_bool(q_pipe_on());     /* getter → 1b/0b */
    size_t n = (alen == 2 && arg[1] == 'b') ? 1 : alen;   /* strip the 1b/0b literal */
    if (n != 1 || (arg[0] != '0' && arg[0] != '1')) return ray_error("type", NULL);
    if (arg[0] == '1') q_pipe_enable(); else q_pipe_disable();
    return NULL;                                     /* setter: silent */
}

/* `\h` — verb help + discovery, all composing on q_ops_table() (rule 3: one
 * source of truth) and writing straight to the CONSOLE BUFFER (unquoted, like
 * `show`; no ray_t, so no refcount surface).  Three modes (openq extension; kdb
 * has no `\h`).  Errors are bare 7-byte classes — no per-verb message strings:
 *   `\h`          -> a dense roster of every verb name, grouped by lexical class;
 *   `\h <verb>`   -> exact match: doc, ```syntax form, `expr -> output` example,
 *                    and the derived code.kx.com page URL (when the row cites one);
 *   `\h <query>`  -> no exact row: a case-insensitive SUBSTRING search over every
 *                    verb's name and doc, ranked name-prefix > name-substr >
 *                    doc-substr; no hit at all -> 'domain.
 * The shared dispatcher strips a lone `/` token as a trailing comment (`\P /
 * default`), which would hide the `/` over verb; recover it from `rest` so
 * `\h /` matches the `/` row like `\h \`, rather than searching. */
static void h_line(const char* s) {
    if (!s) return;
    q_console_write(s, strlen(s));
    q_console_write("\n", 1);
}

/* Derive the code.kx.com page URL from a docsrc path at DISPLAY time — never
 * stored (qdocs is transitional, so the scheme lives in this ONE place; #199
 * stored the path for exactly this).  docsrc = `<H_REF_PREFIX><page>.md[#anchor]`;
 * drop prefix, `.md`, and any anchor -> `<H_REF_BASE><page>`.  Writes into buf;
 * returns 1 on success, 0 when docsrc is NULL or off-shape (no URL line). */
static const char H_REF_PREFIX[] = "qdocs/docs/docs/docs/ref/";
static const char H_REF_BASE[]   = "https://code.kx.com/q/ref/";
static int h_doc_url(const char* docsrc, char* buf, size_t bufsz) {
    if (!docsrc) return 0;
    size_t pfx = sizeof H_REF_PREFIX - 1;
    if (strncmp(docsrc, H_REF_PREFIX, pfx) != 0) return 0;
    const char* page = docsrc + pfx;
    const char* dot = strstr(page, ".md");           /* page name ends at `.md` */
    if (!dot) return 0;
    size_t plen = (size_t)(dot - page), base = sizeof H_REF_BASE - 1;
    if (base + plen + 1 > bufsz) return 0;
    memcpy(buf, H_REF_BASE, base);
    memcpy(buf + base, page, plen);
    buf[base + plen] = '\0';
    return 1;
}

/* The two case-insensitive matchers the search ranks on: is needle[0..nlen) a
 * SUBSTRING of hay (h_ci_contains), or a PREFIX of it (h_ci_prefix)? */
static int h_ci_contains(const char* hay, const char* needle, size_t nlen) {
    if (nlen == 0) return 0;
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nlen) return 1;
    }
    return 0;
}
static int h_ci_prefix(const char* hay, const char* needle, size_t nlen) {
    for (size_t i = 0; i < nlen; i++)
        if (!hay[i] || tolower((unsigned char)hay[i]) != tolower((unsigned char)needle[i])) return 0;
    return nlen != 0;
}

/* Bare `\h`: every verb name, grouped by lexical class, wrapped at H_WRAP columns
 * (a FIXED width, deliberately NOT the runtime `\c` console size, so the golden
 * transcript is stable).  Roster-coupled by design — a new verb changes it. */
static void h_list_all(void) {
    enum { H_WRAP = 72 };
    int n; const q_op_t* t = q_ops_table(&n);
    static const struct { int cls; const char* label; } groups[] = {
        { QLEX_GLYPH,     "glyph  " }, { QLEX_KW_INFIX,  "infix  " },
        { QLEX_KW_PREFIX, "prefix " }, { QLEX_ADVERB,    "adverb " },
    };
    for (size_t g = 0; g < sizeof groups / sizeof groups[0]; g++) {
        char line[256];
        size_t lab = strlen(groups[g].label), len = lab;
        memcpy(line, groups[g].label, lab); line[len] = '\0';
        int any = 0;
        for (int i = 0; i < n; i++) {
            if ((int)t[i].lex != groups[g].cls) continue;
            size_t nl = strlen(t[i].name);
            if (len > lab && len + 1 + nl >= sizeof line) continue;   /* guard */
            if (len > lab && len + 1 + nl > H_WRAP) {                 /* wrap */
                h_line(line);
                memset(line, ' ', lab); len = lab; line[len] = '\0';
            }
            if (len > lab) line[len++] = ' ';
            memcpy(line + len, t[i].name, nl); len += nl; line[len] = '\0';
            any = 1;
        }
        if (any) h_line(line);
    }
}

/* `\h <query>` fuzzy search: three ranked passes (name-prefix, name-substr,
 * doc-substr) over q_ops_table(), printing `name  doc` per hit in rank then
 * table order.  Returns the hit count (0 -> caller raises 'domain). */
static int h_search(const char* q, size_t qlen) {
    int n, hits = 0; const q_op_t* t = q_ops_table(&n);
    for (int rank = 3; rank >= 1; rank--) {
        for (int i = 0; i < n; i++) {
            const q_op_t* op = &t[i];
            int r = h_ci_prefix(op->name, q, qlen) ? 3
                  : h_ci_contains(op->name, q, qlen) ? 2
                  : (op->doc && h_ci_contains(op->doc, q, qlen)) ? 1 : 0;
            if (r != rank) continue;
            char line[256];
            snprintf(line, sizeof line, "%-8s %s", op->name, op->doc ? op->doc : "");
            h_line(line);
            hits++;
        }
    }
    return hits;
}

static ray_t* h_h(const char* arg, size_t alen, const char* rest, size_t restlen) {
    /* Recover a lone `/` the dispatcher stripped as a trailing comment (trailing
     * blanks tolerated, so `\h / ` matches the `/` row like `\h \`). */
    if (alen == 0 && restlen >= 1 && rest[0] == '/') {
        size_t k = 1;
        while (k < restlen && (rest[k] == ' ' || rest[k] == '\t')) k++;
        if (k == restlen) { arg = rest; alen = 1; }
    }
    if (alen == 0) { h_list_all(); return NULL; }                    /* bare -> roster */
    const q_op_t* op = q_ops_find(arg, (int)alen);
    if (op && op->doc) {                                             /* exact match */
        h_line(op->doc);
        h_line(op->syntax);
        h_line(op->example);
        char url[256];
        if (h_doc_url(op->docsrc, url, sizeof url)) h_line(url);
        return NULL;
    }
    /* No documented exact row -> fuzzy search (an undocumented verb, if any ever
     * existed, surfaces itself by name at rank 3).  Nothing found -> 'domain. */
    if (h_search(arg, alen) > 0) return NULL;
    return ray_error("domain", NULL);
}


/* Raw console shell for an unknown `\cmd` (capture=0): system(3), stdout
 * inherited, returns the raw status as a long (kdb-true `\foo`).  Capability-
 * gated: a runtime that does not own the process (doctest, wasm) does NOT
 * execute and is SILENT — corpus `\ls`/`\curl` rows must never touch the
 * FS / network, and silence is kdb's display for a succeeding command (banked
 * rows like dict/key's `\mkdir foo` -> "" pin it).  `rem`/`rlen` is a SLICE
 * of the console line; copied NUL-terminated before system(). */
static ray_t* q_sys_shell(const char* rem, size_t rlen) {
    if (!g_own_process) return NULL;
    char   stackbuf[1024];
    char*  cmd = stackbuf;
    ray_t* blk = NULL;
    if (rlen + 1 > sizeof stackbuf) {
        blk = ray_alloc(rlen + 1);
        if (!blk) return ray_error("oom", NULL);
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
static ray_t* q_sys_shell_capture(const char* rem, size_t rlen) {
    char   stackbuf[1024];
    char*  cmd = stackbuf;
    ray_t* blk = NULL;
    if (rlen + 1 > sizeof stackbuf) {
        blk = ray_alloc(rlen + 1);
        if (!blk) return ray_error("oom", NULL);
        cmd = (char*)ray_data(blk);
    }
    memcpy(cmd, rem, rlen);
    cmd[rlen] = '\0';

    FILE* p = popen(cmd, "r");
    if (blk) ray_free(blk);
    if (!p) return ray_error("os", NULL);

    /* Slurp all stdout into a growable buffer. */
    size_t cap = 4096, len = 0;
    char*  out = (char*)malloc(cap);
    if (!out) { pclose(p); return ray_error("oom", NULL); }
    size_t got;
    char   rbuf[4096];
    while ((got = fread(rbuf, 1, sizeof rbuf, p)) > 0) {
        if (len + got > cap) {
            while (len + got > cap) cap *= 2;
            char* nb = (char*)realloc(out, cap);
            if (!nb) { free(out); pclose(p); return ray_error("oom", NULL); }
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
        return ray_error("os", NULL);                    /* nonzero / signalled */
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
        if (!row || RAY_IS_ERR(row)) { ray_release(list); free(out); return row ? row : ray_error("oom", NULL); }
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
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    const char* sp; int64_t sn;
    if (!q_text_bytes(x, &sp, &sn)) return ray_error("type", "system expects a string");
    size_t sl = (size_t)sn;

    char   stackbuf[1024];
    char*  buf = stackbuf;
    ray_t* blk = NULL;
    if (sl + 2 > sizeof stackbuf) {                      /* '\' + cmd + NUL */
        blk = ray_alloc(sl + 2);
        if (!blk) return ray_error("oom", NULL);
        buf = (char*)ray_data(blk);
    }
    buf[0] = '\\';
    if (sl) memcpy(buf + 1, sp, sl);
    buf[sl + 1] = '\0';

    ray_t* out = q_sys_run(buf, sl + 1, 1);
    if (blk) ray_free(blk);
    if (!out) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }  /* silent -> generic null */
    return q_charv_out(out);            /* captured lines cross as char vectors */
}

bool q_sys_is_cmd(const char* line, size_t n) {
    size_t i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    return i < n && line[i] == '\\';
}

ray_t* q_sys_run(const char* line, size_t n, int capture) {
    size_t i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= n || line[i] != '\\') return ray_error("type", NULL);  /* caller guard: q_sys_is_cmd */
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
        if (q_parse_i64(line + d0, i - d0, &rv) && rv >= 0) rep = rv;
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
     * (\ts \cd \nonlegacy).  Case-sensitive (\c ≠ \C), the q_dotz.c z_slot_ptr
     * idiom.  Each case passes ONLY the arguments its handler needs (no uniform
     * signature).  An unmatched single char (no case, no default) or any other
     * unrecognized token falls through to the shell-out; bare `\` is the silent
     * q/k toggle (deferred, no k mode). */
    if (cmd_len == 1) {
        switch (cmd[0]) {
            case 'd': return h_d(arg, alen);
            case 'v': return q_ns_list('v', arg, alen);          /* namespace vars      */
            case 'f': return q_ns_list('f', arg, alen);          /* namespace functions */
            case 'a': return q_ns_list('a', arg, alen);          /* namespace tables    */
            case 'S': return h_S(arg, alen);                     /* random seed         */
            case 'h': return h_h(arg, alen, rest, restlen);      /* verb help (openq)   */
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
            case 'b': return h_b();                              /* views — empty (owner ruling) */
            case 'B': return ray_error("nyi", NULL);             /* pending views — getter-only  */
            /* Silent setter/action form (arg present) → NULL; getter → 'nyi:
             * \z date-parse, \E TLS, \r replicate, \T timeout, \u user-pwd,
             * \x expunge, \1 stdout, \2 stderr, \_ hide-q-code. */
            case 'z': case 'E': case 'r': case 'T':
            case 'u': case 'x': case '1': case '2': case '_':
                return h_getset(alen);
            /* \\ quit — q_exit is capability-gated (a real process exits firing
             * .z.exit; an embedder returns silently, kdb-true either way). */
            case '\\': q_exit(0); return NULL;
        }
    } else if (cmd_len == 2 && cmd[0] == 't' && cmd[1] == 's') {
        return h_ts(alen, rest, restlen, rep);                   /* time and space      */
    } else if (cmd_len == 2 && cmd[0] == 'c' && cmd[1] == 'd') {
        return h_cd(arg, alen);                                  /* change directory    */
    } else if (cmd_len == 9 && memcmp(cmd, "nonlegacy", 9) == 0) {
        return h_nonlegacy(arg, alen);                          /* pipe-table toggle (openq) */
    } else if (cmd_len == 0) {
        return NULL;                                            /* bare \ — silent q/k toggle */
    }

    /* Unrecognized command → shell out on the raw remainder (token..EOL). */
    return capture ? q_sys_shell_capture(line + rem0, n - rem0)
                   : q_sys_shell(line + rem0, n - rem0);
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
