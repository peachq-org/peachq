/* q_sys — see q_sys.h.  The unified `\`-command dispatcher: one Q_SYS[] static
 * manifest ({glyph, name, len, valence, flags, handler}, in the q_dotz.c idiom)
 * plus one parse/resolve loop.  Stage 1 folds the five previously working
 * commands (\d \v \f \a \S) into rows with NO behaviour change — the bodies are
 * moved verbatim out of the old q_ns_syscmd; only the fixed {d,v,f,a,S} gate is
 * replaced by a manifest lookup.  \d and \v/\f/\a lean on q_ns.c (context state
 * + member enumeration); \S owns its seed state here (its only consumer). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_sys.h"
#include "qlang/q_ns.h"       /* q_ns_current / q_ns_switch / q_ns_list */
#include <rayforce.h>
#include <stdlib.h>           /* srand, strtoll */
#include <string.h>           /* strlen, memcpy */

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

/* ---- the single-source manifest -------------------------------------------
 * {glyph, name (`system "…"` spelling), len, valence, flags, handler}.  The
 * name/len/valence/flags fields are STAGE-2-facing (the `system "d"` string
 * form + REPL-only/park gating for \\ terminate / quit); stage 1 dispatches by
 * `glyph` alone and leaves them informational. */
enum { Q_SYS_F_NONE = 0 };   /* stage 2 adds Q_SYS_F_REPL_ONLY etc. */

static const struct {
    char        glyph;
    const char* name;
    uint8_t     len;
    uint8_t     valence;     /* max args the command reads (informational) */
    uint8_t     flags;
    ray_t*    (*handler)(const char* arg, size_t alen);
} Q_SYS[] = {
    { 'd', "d", 1, 1, Q_SYS_F_NONE, h_d },
    { 'v', "v", 1, 1, Q_SYS_F_NONE, h_v },
    { 'f', "f", 1, 1, Q_SYS_F_NONE, h_f },
    { 'a', "a", 1, 1, Q_SYS_F_NONE, h_a },
    { 'S', "S", 1, 1, Q_SYS_F_NONE, h_S },
};

ray_t* q_sys_dispatch(const char* line, size_t n, int* handled) {
    *handled = 0;
    size_t i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= n || line[i] != '\\') return NULL;
    i++;
    if (i >= n) return NULL;
    char cmd = line[i++];

    /* Resolve the glyph in the manifest.  A miss is "not a syscmd" (handled
     * stays 0) — identical to the old fixed {d,v,f,a,S} gate. */
    int row = -1;
    for (size_t k = 0; k < sizeof Q_SYS / sizeof *Q_SYS; k++)
        if (Q_SYS[k].glyph == cmd) { row = (int)k; break; }
    if (row < 0) return NULL;

    /* `\dfoo` etc. (a non-space glued to the cmd char) is NOT this command. */
    if (i < n && line[i] != ' ' && line[i] != '\t') return NULL;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    /* first token = the optional argument; the rest of the line is ignored
     * (transcripts carry trailing `/ comments`). */
    size_t a0 = i;
    while (i < n && line[i] != ' ' && line[i] != '\t') i++;
    const char* arg = line + a0;
    size_t alen = i - a0;
    if (alen > 0 && arg[0] == '/') alen = 0;      /* bare comment, no arg */

    *handled = 1;
    return Q_SYS[row].handler(arg, alen);
}
