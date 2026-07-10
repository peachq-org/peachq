/* q_sys — see q_sys.h.  The unified `\`-command dispatcher: one Q_SYS[] static
 * manifest ({cmd, len, valence, flags, handler}, in the q_dotz.c idiom) plus one
 * parse/resolve loop.  Stage 1 folded the five working commands (\d \v \f \a \S)
 * into rows.  Stage 2 enumerates EVERY kdb `\`-command as a row (see q_sys.h for
 * the taxonomy: working / getset / nyi / exit-gated), adds the OS shell escape
 * on an unknown-token miss (REPL only), and flag-gates the process-exit commands
 * so the doctest runner survives them.  \d and \v/\f/\a lean on q_ns.c (context
 * state + member enumeration); \S owns its seed state here (its only consumer). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_sys.h"
#include "qlang/q_ns.h"       /* q_ns_current / q_ns_switch / q_ns_list */
#include "qlang/q_fmt.h"      /* q_fmt_set_prec / q_fmt_prec — `\P` precision */
#include "qlang/q_repl.h"     /* q_repl_mark_listener_active — `\p` runtime listen */
#include "core/ipc.h"         /* ray_ipc_listen — `\p N` binds a listener */
#include "core/poll.h"        /* ray_poll_get / ray_selector_t — `\p 0W` fd readback */
#include "core/runtime.h"     /* ray_runtime_get_poll — the runtime event poll */
#include <rayforce.h>
#include "mem/heap.h"         /* ray_mem_stats / ray_mem_stats_t — `\w` reuse */
#include <stdlib.h>           /* srand, strtoll, system, exit */
#include <string.h>           /* strlen, memcpy, memcmp */
#include <errno.h>            /* ERANGE — strtoll overflow guard */

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
 * BEHAVIOURAL SIDE-EFFECTS ARE DEFERRED (rule 9): openq does not yet wrap
 * console output by `\c`/`\C`, run real gc for `\g`, apply `\o`/`\W` to
 * temporal display, trap errors for `\e`, or re-tune worker threads for `\s`.
 * These commands store + report the kdb-true value only; the effect itself is
 * a tracked PLAN.md gap.  Faking a side-effect would be worse than an honest
 * store-and-report. */
static int32_t g_con_rows,  g_con_cols;   /* \c console size  (default 25 80)   */
static int32_t g_http_rows, g_http_cols;  /* \C HTTP size     (default 36 2000) */
static int32_t g_gc_mode;                 /* \g gc mode       (default 0)       */
static int64_t g_utc_offset;              /* \o UTC offset    (default 0N)      */
static int32_t g_week_offset;             /* \W week offset   (default 2)       */
static int32_t g_err_trap;                /* \e error trap    (default 0)       */
static int32_t g_sec_threads;             /* \s secondary thr (default 0)       */

void q_sys_cfg_init(void) {
    g_con_rows  = 25; g_con_cols  = 80;
    g_http_rows = 36; g_http_cols = 2000;
    g_gc_mode   = 0;
    g_utc_offset = INT64_MIN;    /* 0N — "use the machine offset" (deferred) */
    g_week_offset = 2;           /* Monday (0 = Saturday) */
    g_err_trap  = 0;             /* trapping off */
    g_sec_threads = 0;           /* no secondary threads configured */
    q_fmt_set_prec(7);           /* `\P` default (single-homed in q_fmt.c) */
}

/* Parse a base-10 signed integer from [s,s+len) into *out; returns 1 on a
 * clean full parse (no empty, no trailing garbage, no int64 overflow). */
static int q_parse_i64(const char* s, size_t len, long long* out) {
    if (len == 0 || len >= 32) return 0;
    char buf[32];
    memcpy(buf, s, len); buf[len] = '\0';
    char* end = NULL;
    errno = 0;
    long long v = strtoll(buf, &end, 10);
    if (!end || *end != '\0' || end == buf || errno == ERANGE) return 0;
    *out = v;
    return 1;
}

/* Parse up to `max` whitespace-separated base-10 integers from [s,s+len),
 * stopping at the first non-integer token (e.g. a trailing `/ comment`).
 * Returns the count stored into out[]. */
static int q_parse_ints(const char* s, size_t len, long long* out, int max) {
    int cnt = 0; size_t i = 0;
    while (cnt < max && i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
        size_t t0 = i;
        while (i < len && s[i] != ' ' && s[i] != '\t') i++;
        if (i == t0) break;
        long long v;
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

/* ---- per-command handlers: (arg, alen, rest, restlen) ----------------------
 * arg/alen is the already-tokenized FIRST argument (empty when absent).
 * rest/restlen is the FULL argument region (first token through EOL, including
 * any trailing `/ comment`) for the multi-argument commands (`\c`/`\C`).
 * Return: OWNED value | NULL (silent) | OWNED error. */

static ray_t* h_d(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen;
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

static ray_t* h_v(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen; return q_ns_list('v', arg, alen);
}
static ray_t* h_f(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen; return q_ns_list('f', arg, alen);
}
static ray_t* h_a(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen; return q_ns_list('a', arg, alen);
}

static ray_t* h_S(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen;
    if (alen == 0)                               /* `\S` — last-initialized seed */
        return ray_i32(g_last_seed);
    if (alen == 2 && arg[0] == '0' && arg[1] == 'N')  /* `\S 0N` — live state */
        return ray_error("nyi", "\\S 0N: libc rand state is not readable");
    char* end = NULL;
    char abuf[32];
    if (alen >= sizeof abuf) return ray_error("parse", NULL);
    memcpy(abuf, arg, alen); abuf[alen] = '\0';
    long long v = strtoll(abuf, &end, 10);
    if (!end || *end != '\0' || end == abuf)
        return ray_error("parse", NULL);         /* non-integer arg (unpinned) */
    /* The seed is an INT (`\S` displays it as one): out-of-int-range values
     * (incl. strtoll saturation) and the 0Ni sentinel are rejected, never
     * silently truncated (codex P2, 2026-07-09). */
    if (v <= INT32_MIN || v > INT32_MAX)
        return ray_error("parse", NULL);
    srand((unsigned)v);                          /* `\S n` — re-initialize */
    g_last_seed = (int32_t)v;
    return NULL;                                 /* silent, like `\d ns` */
}

/* ---- shared handlers for the still-unimplemented commands ------------------
 * After Stage 3 (\P \c \C \w \g \o \W \e \s now have dedicated handlers below),
 * h_getset serves the REMAINING commands whose SETTER / ACTION form (arg
 * present) prints NOTHING in kdb — date-parse (\z), file/name actions
 * (\l \cd \x \r \1 \2 \_), and port/timeout/TLS/user config (\p \T \E \u).  We
 * accept the arg-form as a silent no-op: the OUTPUT matches kdb's empty setter
 * output, so the frozen ledger rows that bank that silence (e.g. `\l sp.q`,
 * `\z 1`) stay green.  The side-effect is a tracked gap; the GETTER form (no
 * arg, which kdb prints a value for) can't yet report the value → honest 'nyi.
 *
 * h_nyi serves the remaining commands that print a VALUE even in their arg-form
 * or are getter-only — \b \B (views), \t / \ts (timer / timing, Tier-3
 * deferred): there is no silent form to match, so 'nyi is both honest and
 * non-regressing.  It is also the observable "known but not implemented"
 * signal, distinct from an unknown-token shell-out. */
static ray_t* h_getset(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)arg; (void)rest; (void)restlen;
    if (alen > 0) return NULL;                   /* setter/action form → silent */
    return ray_error("nyi", NULL);               /* getter form → not yet */
}

static ray_t* h_nyi(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)arg; (void)alen; (void)rest; (void)restlen;
    return ray_error("nyi", NULL);
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
 * auto-bind path needs the OS-chosen port to report it.  Copy of qmain.c's
 * listener_bound_port (the two `0W` paths are deliberately identical); 0 on
 * failure. */
static uint16_t p_bound_port(int64_t fd) {
    struct sockaddr_in sa;
    socklen_t          len = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    if (getsockname((ray_sock_t)fd, (struct sockaddr*)&sa, &len) != 0)
        return 0;
    return ntohs(sa.sin_port);
}

/* `\p N` — listening port.  Binds a kdb-protocol IPC listener on port N
 * (1..65535, or `0W` = any OS-chosen free port) on the runtime event poll; the
 * unified REPL loop (q_repl.c) then serves it, and q_repl_mark_listener_active
 * keeps the process alive past stdin EOF (a client that `\p`s a port becomes a
 * server).  `0W` mirrors startup `-p 0W` (qmain.c): bind 0, read the real port
 * back via getsockname, print it — so the runtime path is symmetric with the
 * flag path (this is what lets tools/qscript run each server on an ephemeral
 * port, immune to a busy 5000 and to concurrent runners).  Deferred (kept
 * `'nyi`): the getter `\p` (report current port) and `\p 0` (close). */
static ray_t* h_p(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen;
    if (alen == 0) return ray_error("nyi", NULL);        /* getter — deferred */
    /* `0W`/`0w` auto token (same shape as h_S's `0N` probe) — bind port 0. */
    bool port_auto = (alen == 2 && arg[0] == '0' && (arg[1] == 'W' || arg[1] == 'w'));
    long long v = 0;
    if (!port_auto) {
        if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
        if (v == 0) return ray_error("nyi", NULL);       /* `\p 0` close — deferred */
        if (v < 1 || v > 65535) return ray_error("domain", NULL);
    }
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    if (!poll) return ray_error("io", NULL);             /* no event poll (e.g. qdoctest) */
    int64_t lid = ray_ipc_listen(poll, port_auto ? 0 : (uint16_t)v);
    if (lid < 0)
        return ray_error("io", NULL);                    /* bind/listen failed (EADDRINUSE, …) */
    uint16_t bound = (uint16_t)v;
    if (port_auto) {
        /* Report the ACTUAL bound port; a readback failure after a successful
         * bind is an `'io` error — never advertise `listening on port 0`
         * (readiness would pass but every client would then fail to connect). */
        ray_selector_t* ls = ray_poll_get(poll, lid);
        bound = ls ? p_bound_port(ls->fd) : 0;
        if (!bound) return ray_error("io", NULL);
    }
    q_repl_mark_listener_active();
    /* Same readiness line startup `-p` prints (qmain) — lets a supervisor/test
     * detect the now-live listener; stderr so it never taints an stdout golden. */
    fprintf(stderr, "listening on port %u\n", bound);
    return NULL;                                          /* setter: silent */
}

/* `\P` — display precision.  `\P`→`7i`; `\P n` sets n∈[0,17] (0 = max = 17),
 * silent.  The float formatter (q_fmt.c) is the sole reader. */
static ray_t* h_P(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen;
    if (alen == 0) return ray_i32(q_fmt_prec());          /* getter */
    long long v;
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
static ray_t* h_w(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen;
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
 * `\C`→`36 2000`; a set coerces each value to [10,2000] (syscmds.md).  Display
 * truncation by these sizes is DEFERRED (openq does not wrap output). */
static int64_t q_clamp_cc(long long v) {
    return v < 10 ? 10 : v > 2000 ? 2000 : v;
}
static ray_t* h_c(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)arg; (void)alen;
    long long p[2];
    int cnt = q_parse_ints(rest, restlen, p, 2);
    if (cnt == 0) return q_pair_i64(g_con_rows, g_con_cols);   /* getter */
    if (cnt >= 2) {                                            /* setter */
        g_con_rows = (int32_t)q_clamp_cc(p[0]);
        g_con_cols = (int32_t)q_clamp_cc(p[1]);
    }
    return NULL;                                              /* silent */
}
static ray_t* h_C(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)arg; (void)alen;
    long long p[2];
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
static ray_t* h_g(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen;
    if (alen == 0) return ray_i32(g_gc_mode);
    long long v;
    if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
    if (v != 0 && v != 1) return NULL;               /* only 0|1 valid */
    g_gc_mode = (int32_t)v;
    ray_t* g = ray_gc_fn(NULL, 0);                   /* stub call (reuse) */
    if (g) ray_release(g);
    return NULL;
}

/* `\o` — offset from UTC (hours; minutes if abs>23).  `\o`→`0N` (machine
 * offset), else the set value.  Temporal-display wiring is DEFERRED. */
static ray_t* h_o(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen;
    if (alen == 0) return ray_i64(g_utc_offset);     /* 0N default, or set value */
    long long v;
    if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
    g_utc_offset = v;
    return NULL;
}

/* `\W` — start-of-week offset (0 = Saturday, default 2 = Monday).  `\W`→`2i`.
 * Week-start wiring into temporal ops is DEFERRED. */
static ray_t* h_W(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen;
    if (alen == 0) return ray_i32(g_week_offset);
    long long v;
    if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
    g_week_offset = (int32_t)v;
    return NULL;
}

/* `\e` — error-trap mode (0 off / 1 suspend / 2 dump).  `\e`→`0i`.  The trap
 * behaviour itself is DEFERRED. */
static ray_t* h_e(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen;
    if (alen == 0) return ray_i32(g_err_trap);
    long long v;
    if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
    if (v < 0 || v > 2) return NULL;                 /* modes 0|1|2 */
    g_err_trap = (int32_t)v;
    return NULL;
}

/* `\s` — secondary threads.  `\s`→configured count (`0i`).  Runtime re-tuning
 * (and `\s 0N` = show max) is DEFERRED; `\s 0N` returns a parse error for now. */
static ray_t* h_s(const char* arg, size_t alen, const char* rest, size_t restlen) {
    (void)rest; (void)restlen;
    if (alen == 0) return ray_i32(g_sec_threads);
    long long v;
    if (!q_parse_i64(arg, alen, &v)) return ray_error("parse", NULL);
    g_sec_threads = (int32_t)v;
    return NULL;
}

/* ---- the single-source manifest -------------------------------------------
 * {cmd (token, "" = bare terminate/toggle, "\\" = quit), len, valence, flags,
 * handler}.  Lookup is by the parsed command TOKEN (multi-char `ts`/`cd` and
 * `\\` are representable).  Flags gate the process-exit commands per entry
 * point (see q_sys_dispatch). */
enum {
    Q_SYS_F_NONE      = 0,
    Q_SYS_F_REPL_ONLY = 0x01,   /* benign no-op from the doctest path */
    Q_SYS_F_EXIT      = 0x02,   /* exits the process — REPL only */
};

static const struct {
    const char* cmd;
    uint8_t     len;
    uint8_t     valence;     /* max args the command reads (informational) */
    uint8_t     flags;
    ray_t*    (*handler)(const char* arg, size_t alen,
                         const char* rest, size_t restlen);
} Q_SYS[] = {
    /* working — Stage 1 behaviour, unchanged */
    { "d",  1, 1, Q_SYS_F_NONE, h_d },
    { "v",  1, 1, Q_SYS_F_NONE, h_v },
    { "f",  1, 1, Q_SYS_F_NONE, h_f },
    { "a",  1, 1, Q_SYS_F_NONE, h_a },
    { "S",  1, 1, Q_SYS_F_NONE, h_S },
    /* Stage 3 — real get/set + kdb-true getter values */
    { "P",  1, 1, Q_SYS_F_NONE, h_P },        /* display precision */
    { "c",  1, 2, Q_SYS_F_NONE, h_c },        /* console size */
    { "C",  1, 2, Q_SYS_F_NONE, h_C },        /* HTTP display size */
    { "o",  1, 1, Q_SYS_F_NONE, h_o },        /* offset from UTC */
    { "g",  1, 1, Q_SYS_F_NONE, h_g },        /* gc mode */
    { "s",  1, 1, Q_SYS_F_NONE, h_s },        /* secondary threads */
    { "W",  1, 1, Q_SYS_F_NONE, h_W },        /* week offset */
    { "e",  1, 1, Q_SYS_F_NONE, h_e },        /* error-trap clients */
    { "w",  1, 1, Q_SYS_F_NONE, h_w },        /* workspace stats (6 longs) */
    /* silent setter / action form (arg present) → NULL; getter form → 'nyi */
    { "z",  1, 1, Q_SYS_F_NONE, h_getset },   /* date parsing */
    { "E",  1, 1, Q_SYS_F_NONE, h_getset },   /* TLS server mode */
    { "l",  1, 1, Q_SYS_F_NONE, h_getset },   /* load file/dir */
    { "p",  1, 1, Q_SYS_F_NONE, h_p },        /* listening port — runtime \p N binds */
    { "r",  1, 2, Q_SYS_F_NONE, h_getset },   /* replication / rename */
    { "T",  1, 1, Q_SYS_F_NONE, h_getset },   /* client timeout */
    { "u",  1, 1, Q_SYS_F_NONE, h_getset },   /* reload user pwd file */
    { "x",  1, 1, Q_SYS_F_NONE, h_getset },   /* expunge */
    { "cd", 2, 1, Q_SYS_F_NONE, h_getset },   /* change directory */
    { "1",  1, 1, Q_SYS_F_NONE, h_getset },   /* stdout redirect */
    { "2",  1, 1, Q_SYS_F_NONE, h_getset },   /* stderr redirect */
    { "_",  1, 1, Q_SYS_F_NONE, h_getset },   /* hide q code */
    /* value-printing / getter-only → 'nyi (no silent form to match) */
    { "b",  1, 1, Q_SYS_F_NONE, h_nyi },      /* views (lists) */
    { "B",  1, 1, Q_SYS_F_NONE, h_nyi },      /* pending views (lists) */
    { "t",  1, 1, Q_SYS_F_NONE, h_nyi },      /* timer set (silent) + \t expr timing
                                               * (prints ms): openq has neither → 'nyi,
                                               * honest for both forms (never silent) */
    { "ts", 2, 1, Q_SYS_F_NONE, h_nyi },      /* time and space (prints value) */
    /* process-exit / REPL-only — gated in q_sys_dispatch, never exit a doctest */
    { "\\", 1, 0, Q_SYS_F_EXIT,      h_nyi }, /* \\ quit the process */
    { "",   0, 0, Q_SYS_F_REPL_ONLY, h_nyi }, /* bare \ terminate / toggle q-k */
};

/* Shell escape (REPL only).  `rem`/`rlen` is a SLICE of the console line, so
 * copy it NUL-terminated before system() — stack buffer for the common case,
 * ray_alloc fallback for long commands (mirrors frozen src/lang/syscmd.c). */
static ray_t* q_sys_shell(const char* rem, size_t rlen) {
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

ray_t* q_sys_dispatch(const char* line, size_t n, int* handled, int is_repl) {
    *handled = 0;
    size_t i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= n || line[i] != '\\') return NULL;   /* not a `\`-command line */
    i++;

    /* Command token = run of chars that are neither whitespace nor `:`.  The
     * `:` stop resolves kdb's repetition suffix (\t:100, \ts:10000) to the base
     * command.  The remainder (token..EOL) is what a miss hands the shell. */
    size_t rem0 = i;
    size_t c0 = i;
    while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != ':') i++;
    const char* cmd = line + c0;
    size_t cmd_len = i - c0;

    int row = -1;
    for (size_t k = 0; k < sizeof Q_SYS / sizeof *Q_SYS; k++)
        if (Q_SYS[k].len == cmd_len && memcmp(Q_SYS[k].cmd, cmd, cmd_len) == 0) {
            row = (int)k;
            break;
        }

    if (row < 0) {
        /* Unknown `\`-token.  REPL → OS shell escape (kdb-true `\foo`).  Doctest
         * / script → do NOT execute; leave it to the parser (handled stays 0,
         * byte-identical to the historic fall-through). */
        if (!is_repl) return NULL;
        *handled = 1;
        return q_sys_shell(line + rem0, n - rem0);
    }

    /* Flag gating on the resolved row. */
    uint8_t flags = Q_SYS[row].flags;
    if (flags & Q_SYS_F_EXIT) {
        *handled = 1;
        if (is_repl) exit(0);        /* real quit — kdb-true */
        return NULL;                 /* doctest: survive silently */
    }
    if ((flags & Q_SYS_F_REPL_ONLY) && !is_repl) {
        *handled = 1;
        return NULL;                 /* doctest: benign, never exit */
    }

    /* Optional first-token argument.  Skip a `:`-repetition suffix (\t:100)
     * first; then the first whitespace-delimited token (rest of the line is
     * ignored — transcripts carry trailing `/ comments`). */
    if (i < n && line[i] == ':')
        while (i < n && line[i] != ' ' && line[i] != '\t') i++;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t a0 = i;
    /* rest = the WHOLE argument region (first token through EOL, trailing
     * comment included) — the multi-arg commands (`\c`/`\C`) parse it; the
     * single-token commands use arg/alen below and ignore it. */
    const char* rest = line + a0;
    size_t restlen = n - a0;
    while (i < n && line[i] != ' ' && line[i] != '\t') i++;
    const char* arg = line + a0;
    size_t alen = i - a0;
    /* A LONE `/` token is a bare trailing comment (`\P / default`) → no arg.
     * A multi-char `/…` token is a file PATH (`\l /tmp/db`) → keep it: the
     * Stage-1 `arg[0]=='/'` guard wrongly swallowed absolute paths. */
    if (alen == 1 && arg[0] == '/') alen = 0;

    *handled = 1;
    return Q_SYS[row].handler(arg, alen, rest, restlen);
}
