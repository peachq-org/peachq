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
#include <rayforce.h>
#include <stdlib.h>           /* srand, strtoll, system, exit */
#include <string.h>           /* strlen, memcpy, memcmp */

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

/* ---- per-command handlers: (arg, alen) -> OWNED value | NULL | OWNED error --
 * arg/alen is the already-tokenized first argument (empty when absent). */

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

static ray_t* h_v(const char* arg, size_t alen) { return q_ns_list('v', arg, alen); }
static ray_t* h_f(const char* arg, size_t alen) { return q_ns_list('f', arg, alen); }
static ray_t* h_a(const char* arg, size_t alen) { return q_ns_list('a', arg, alen); }

static ray_t* h_S(const char* arg, size_t alen) {
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

/* ---- shared handlers for enumerated-but-unimplemented commands -------------
 * h_getset serves every command whose SETTER / ACTION form (arg present) prints
 * NOTHING in kdb — precision/console/offset/date-parse config (\P \c \o \z),
 * file/name actions (\l \cd \x \r \1 \2 \_), thread/timeout/trap/gc/port config
 * (\s \T \e \g \p \W \C \E \u).  We accept the arg-form as a silent no-op:
 * the OUTPUT matches kdb's empty setter output, so the many frozen ledger rows
 * that bank that silence (e.g. `\l sp.q`, `\e 2`, `\P 3`) stay green.  The
 * side-effect itself lands in Stage 3 — until then this is an output-faithful
 * no-op, an acknowledged Stage-3 gap.  The GETTER form (no arg, which kdb prints
 * a value for) can't yet report the value → honest 'nyi.
 *
 * h_nyi serves commands that print a VALUE even in their arg-form, and the
 * getter-only listings — \b \B (views), \w (workspace), \t / \ts (timing):
 * there is no silent form to match, so 'nyi is both honest and non-regressing
 * (their rows expect a value → red either way).  It is also the observable
 * "known but not implemented" signal, distinct from an unknown-token shell-out. */
static ray_t* h_getset(const char* arg, size_t alen) {
    (void)arg;
    if (alen > 0) return NULL;                   /* setter/action form → silent */
    return ray_error("nyi", NULL);               /* getter form → not yet */
}

static ray_t* h_nyi(const char* arg, size_t alen) {
    (void)arg; (void)alen;
    return ray_error("nyi", NULL);
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
    ray_t*    (*handler)(const char* arg, size_t alen);
} Q_SYS[] = {
    /* working — Stage 1 behaviour, unchanged */
    { "d",  1, 1, Q_SYS_F_NONE, h_d },
    { "v",  1, 1, Q_SYS_F_NONE, h_v },
    { "f",  1, 1, Q_SYS_F_NONE, h_f },
    { "a",  1, 1, Q_SYS_F_NONE, h_a },
    { "S",  1, 1, Q_SYS_F_NONE, h_S },
    /* silent setter / action form (arg present) → NULL; getter form → 'nyi */
    { "P",  1, 1, Q_SYS_F_NONE, h_getset },   /* precision */
    { "c",  1, 2, Q_SYS_F_NONE, h_getset },   /* console size */
    { "C",  1, 2, Q_SYS_F_NONE, h_getset },   /* HTTP display size */
    { "o",  1, 1, Q_SYS_F_NONE, h_getset },   /* offset from UTC */
    { "z",  1, 1, Q_SYS_F_NONE, h_getset },   /* date parsing */
    { "e",  1, 1, Q_SYS_F_NONE, h_getset },   /* error-trap clients */
    { "E",  1, 1, Q_SYS_F_NONE, h_getset },   /* TLS server mode */
    { "g",  1, 1, Q_SYS_F_NONE, h_getset },   /* gc mode */
    { "l",  1, 1, Q_SYS_F_NONE, h_getset },   /* load file/dir */
    { "p",  1, 1, Q_SYS_F_NONE, h_getset },   /* listening port */
    { "r",  1, 2, Q_SYS_F_NONE, h_getset },   /* replication / rename */
    { "s",  1, 1, Q_SYS_F_NONE, h_getset },   /* secondary threads */
    { "T",  1, 1, Q_SYS_F_NONE, h_getset },   /* client timeout */
    { "u",  1, 1, Q_SYS_F_NONE, h_getset },   /* reload user pwd file */
    { "W",  1, 1, Q_SYS_F_NONE, h_getset },   /* week offset */
    { "x",  1, 1, Q_SYS_F_NONE, h_getset },   /* expunge */
    { "cd", 2, 1, Q_SYS_F_NONE, h_getset },   /* change directory */
    { "1",  1, 1, Q_SYS_F_NONE, h_getset },   /* stdout redirect */
    { "2",  1, 1, Q_SYS_F_NONE, h_getset },   /* stderr redirect */
    { "_",  1, 1, Q_SYS_F_NONE, h_getset },   /* hide q code */
    /* value-printing / getter-only → 'nyi (no silent form to match) */
    { "b",  1, 1, Q_SYS_F_NONE, h_nyi },      /* views (lists) */
    { "B",  1, 1, Q_SYS_F_NONE, h_nyi },      /* pending views (lists) */
    { "w",  1, 1, Q_SYS_F_NONE, h_nyi },      /* workspace stats (6 longs) */
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
    while (i < n && line[i] != ' ' && line[i] != '\t') i++;
    const char* arg = line + a0;
    size_t alen = i - a0;
    /* A LONE `/` token is a bare trailing comment (`\P / default`) → no arg.
     * A multi-char `/…` token is a file PATH (`\l /tmp/db`) → keep it: the
     * Stage-1 `arg[0]=='/'` guard wrongly swallowed absolute paths. */
    if (alen == 1 && arg[0] == '/') alen = 0;

    *handled = 1;
    return Q_SYS[row].handler(arg, alen);
}
