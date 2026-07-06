/* q op manifest table + static lexical classifier — see q_ops.h.
 *
 * ROSTER GROUND TRUTH — audited against ray_register_builtins (src/lang/eval.c)
 * and the divergence matrix (specs/2026-07-03-q-op-registry-complete-design.md,
 * corrected in adr/0003):
 *   - rayfall `/` is FLOAT-divide (7/2 -> 3.5) and `%` is MODULO, so q `%`
 *     (float divide) renames rayfall `/`.
 *   - q monadic `-` is negate (rayfall `neg`); `*` monadic is `first`; `#`
 *     monadic is `count`; `=` monadic is `group`; `<`/`>` monadic are grade
 *     up/down (`iasc`/`idesc`); `|` monadic is `reverse`; `&` monadic is
 *     `where`; `,` monadic is `enlist`; `~` monadic is `not`.
 *   - q `=`/`<>` dyadic wrap rayfall `==`/`!=` (char-string element-wise case);
 *     q `#`/`_` dyadic are arg-swap `take` / count-`drop` wrappers.
 *   - q `,` dyadic is join -> rayfall `concat`.
 * DEFERRED (QK_NONE — no clean rayfall target, do NOT guess): `|` dyadic
 *   (max is an AGGREGATE, not dyadic element-wise greater), monadic `+`
 *   (flip: no builtin), monadic `%` (reciprocal: no builtin).  Dyadic `&`
 *   is the QK_MIN2 wrapper (element-wise min / bool-and).  Dyadic `~` is the
 *   QK_MATCH wrapper (2c-1).  The remaining type-dispatch verbs `! ? $ @ .`
 *   land in 2c-2.
 *
 * KW_INFIX is {div, each, in, within} — a keyword row here is what makes the
 * lexer reclassify the name as an infix verb in noun position (the retired
 * q_is_kw_verb memcmp, now manifest-driven). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_ops.h"
#include <string.h>

static const q_op_t Q_OPS[] = {
    /* name    lex             mon_kind mon_target   dyad_kind dyad_target  adverb_hof */
    /* ---- arithmetic / compare glyphs ---- */
    { "+",     QLEX_GLYPH,     QK_NONE, NULL,        QK_ENV,   "+",       NULL  },
    { "-",     QLEX_GLYPH,     QK_NEG,  "neg",       QK_ENV,   "-",       NULL  },
    { "*",     QLEX_GLYPH,     QK_ENV,  "first",     QK_ENV,   "*",       NULL  },
    { "%",     QLEX_GLYPH,     QK_NONE, NULL,        QK_ENV,   "/",       NULL  },
    { "<",     QLEX_GLYPH,     QK_ENV,  "iasc",      QK_ENV,   "<",       NULL  },
    { ">",     QLEX_GLYPH,     QK_ENV,  "idesc",     QK_ENV,   ">",       NULL  },
    { "<=",    QLEX_GLYPH,     QK_NONE, NULL,        QK_ENV,   "<=",      NULL  },
    { ">=",    QLEX_GLYPH,     QK_NONE, NULL,        QK_ENV,   ">=",      NULL  },
    { "=",     QLEX_GLYPH,     QK_ENV,  "group",     QK_EQ,    "==",      NULL  },
    { "<>",    QLEX_GLYPH,     QK_NONE, NULL,        QK_NE,    "!=",      NULL  },
    /* ---- structural glyphs ---- */
    { "#",     QLEX_GLYPH,     QK_ENV,  "count",     QK_TAKE,  "take",    NULL  },
    /* monadic `_` is a K-ism (valid q spells it `floor`) — accepting it is a
     * deliberate SUPERSET of q source; the VALUE is kdb-identical (kdb's own
     * floor IS k `_:`).  The q spelling is the `floor` keyword row below. */
    { "_",     QLEX_GLYPH,     QK_FLOOR, "floor",    QK_DROP,  "drop",    NULL  },
    { "|",     QLEX_GLYPH,     QK_ENV,  "reverse",   QK_NONE,  NULL,      NULL  },
    { "&",     QLEX_GLYPH,     QK_ENV,  "where",     QK_MIN2,  "and",     NULL  },
    { ",",     QLEX_GLYPH,     QK_ENV,  "enlist",    QK_ENV,   "concat",  NULL  },
    { "~",     QLEX_GLYPH,     QK_ENV,  "not",       QK_MATCH, "match",   NULL  },
    /* ---- type-dispatch glyphs (2c-2) ---- */
    /* monadic `!` (dict keys) is a K-ism accepted as a deliberate superset
     * (valid q spells it `key`, same value — the `_`/floor precedent). */
    { "!",     QLEX_GLYPH,     QK_KEY,  "key",       QK_BANG,  "dict",    NULL  },
    /* monadic `?` (distinct) is likewise a K-ism superset of q `distinct`. */
    { "?",     QLEX_GLYPH,     QK_DISTINCT, "distinct", QK_ROLL, "rand",  NULL  },
    /* monadic `$` stays QK_NONE: `$x` is the cast-vs-cond ambiguity, deferred.
     * Dyadic `$` is cast; the bracket cond form `$[c;t;f]` (3+ args) is a
     * q_lower rewrite onto rayfall `if`, not a registry cell. */
    { "$",     QLEX_GLYPH,     QK_NONE, NULL,        QK_CAST,  "as",      NULL  },
    /* monadic `@`/`.` stay QK_NONE: `@x` type-of is blocked on the q type
     * renumber; `.x` (value/get) comes with handles/namespaces.  Ternary+
     * Trap/Amend forms are deferred cells (error today via arity). */
    { "@",     QLEX_GLYPH,     QK_NONE, NULL,        QK_AT,    "at",      NULL  },
    { ".",     QLEX_GLYPH,     QK_NONE, NULL,        QK_DOT,   "apply",   NULL  },
    /* ---- keyword-infix ---- */
    { "div",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_ENV,   "div",     NULL  },
    /* q `f each x` == `f'x`: a dyadic wrapper over rayfall map (+ vector
     * collapse, since map returns a boxed list where q wants a simple vec). */
    { "each",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_EACH,  "map",     NULL  },
    { "in",    QLEX_KW_INFIX,  QK_NONE, NULL,        QK_ENV,   "in",      NULL  },
    /* q `x within y` — inclusive bounds check (ref/within.md); wrapper because
     * base ray_within_fn is vector-vals-only and width-blind on the range. */
    { "within",QLEX_KW_INFIX,  QK_NONE, NULL,        QK_WITHIN,"within",  NULL  },
    /* q `n cut x` — chunk (int atom) / positional cut (int vector). */
    { "cut",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_CUT,   "cut",     NULL  },
    /* iterator mnemonic keywords (wave-2): infix `f over/scan/prior/peach x`,
     * same lexical treatment as `each`.  over/scan dispatch reduce/converge/
     * do/while by f rank; prior is unary each-prior; peach == each. */
    { "over",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_OVER,  "over",    NULL  },
    { "scan",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_SCANKW,"scan-kw", NULL  },
    { "prior", QLEX_KW_INFIX,  QK_NONE, NULL,        QK_PRIORKW,"prior",  NULL  },
    { "peach", QLEX_KW_INFIX,  QK_NONE, NULL,        QK_EACH,  "peach",   NULL  },
    /* ---- keyword-prefix monads (pass-through/rename) ---- */
    { "neg",     QLEX_KW_PREFIX, QK_NEG, "neg",      QK_NONE,  NULL,      NULL  },
    { "til",     QLEX_KW_PREFIX, QK_ENV, "til",      QK_NONE,  NULL,      NULL  },
    { "count",   QLEX_KW_PREFIX, QK_ENV, "count",    QK_NONE,  NULL,      NULL  },
    { "first",   QLEX_KW_PREFIX, QK_ENV, "first",    QK_NONE,  NULL,      NULL  },
    { "last",    QLEX_KW_PREFIX, QK_ENV, "last",     QK_NONE,  NULL,      NULL  },
    { "where",   QLEX_KW_PREFIX, QK_ENV, "where",    QK_NONE,  NULL,      NULL  },
    { "reverse", QLEX_KW_PREFIX, QK_ENV, "reverse",  QK_NONE,  NULL,      NULL  },
    { "sum",     QLEX_KW_PREFIX, QK_ENV, "sum",      QK_NONE,  NULL,      NULL  },
    { "group",   QLEX_KW_PREFIX, QK_ENV, "group",    QK_NONE,  NULL,      NULL  },
    { "avg",     QLEX_KW_PREFIX, QK_ENV, "avg",      QK_NONE,  NULL,      NULL  },
    /* q `floor` returns LONGS from floats (kdb `floor 3.7` is 3j); rayfall's
     * env floor keeps f64, so this is the QK_FLOOR wrapper, not a rename. */
    { "floor",   QLEX_KW_PREFIX, QK_FLOOR, "floor",  QK_NONE,  NULL,      NULL  },
    /* q dict accessors: `key`/`value` are wrappers (dict-only in 2c-2 —
     * the file-handle/namespace/enumeration overloads are deferred cells);
     * `distinct` must preserve FIRST-OCCURRENCE order (kdb), while rayfall's
     * distinct routes typed vectors through the DAG group path, which sorts
     * — so it too is a wrapper, not a rename (audited: `distinct 2 3 7 3
     * 5 3` must be 2 3 7 5, env distinct gives 2 3 5 7). */
    { "key",     QLEX_KW_PREFIX, QK_KEY,      "key",      QK_NONE, NULL,  NULL  },
    { "value",   QLEX_KW_PREFIX, QK_VALUE,    "value",    QK_NONE, NULL,  NULL  },
    { "distinct",QLEX_KW_PREFIX, QK_DISTINCT, "distinct", QK_NONE, NULL,  NULL  },
    /* q-implemented keywords: env bindings added by q_builtins_register
     * (same mechanism as `parse`), snapshotted here as pass-through rows. */
    { "string",  QLEX_KW_PREFIX, QK_ENV, "string",   QK_NONE,  NULL,      NULL  },
    { "upper",   QLEX_KW_PREFIX, QK_ENV, "upper",    QK_NONE,  NULL,      NULL  },
    { "lower",   QLEX_KW_PREFIX, QK_ENV, "lower",    QK_NONE,  NULL,      NULL  },
    { "show",    QLEX_KW_PREFIX, QK_ENV, "show",     QK_NONE,  NULL,      NULL  },
    /* ---- additional monadic pass-through keywords (rayfall name == q name,
     * audited kdb-true element-wise / aggregate semantics). See
     * docs/recipes/add-q-keyword-verb.md. ---- */
    { "abs",     QLEX_KW_PREFIX, QK_ENV, "abs",      QK_NONE,  NULL,      NULL  },
    { "dev",     QLEX_KW_PREFIX, QK_ENV, "dev",      QK_NONE,  NULL,      NULL  },
    { "exp",     QLEX_KW_PREFIX, QK_ENV, "exp",      QK_NONE,  NULL,      NULL  },
    { "log",     QLEX_KW_PREFIX, QK_ENV, "log",      QK_NONE,  NULL,      NULL  },
    { "max",     QLEX_KW_PREFIX, QK_ENV, "max",      QK_NONE,  NULL,      NULL  },
    { "min",     QLEX_KW_PREFIX, QK_ENV, "min",      QK_NONE,  NULL,      NULL  },
    { "rank",    QLEX_KW_PREFIX, QK_ENV, "rank",     QK_NONE,  NULL,      NULL  },
    { "raze",    QLEX_KW_PREFIX, QK_ENV, "raze",     QK_NONE,  NULL,      NULL  },
    { "sqrt",    QLEX_KW_PREFIX, QK_ENV, "sqrt",     QK_NONE,  NULL,      NULL  },
    /* each-prior mnemonics: deltas x == (-':)x, differ x == not(~':)x. */
    { "deltas",  QLEX_KW_PREFIX, QK_DELTAS, "deltas", QK_NONE, NULL,      NULL  },
    { "differ",  QLEX_KW_PREFIX, QK_DIFFER, "differ", QK_NONE, NULL,      NULL  },
    /* ---- adverbs — q adverbs ARE rayfall higher-order fns (no bespoke object).
     * `+/` lowers to fold over `+` (q_lower); `/:`/`\:` ARE map-right/map-left
     * (src/ops/collection.c:2279 — map-left iterates LEFT holding right =
     * q each-left, verified against examples/rfl).  Each-prior `':` still has
     * no rayfall counterpart (the scan-* variants are fold-style, not
     * pairwise) — DEFERRED, still lexer-classified. ---- */
    { "'",     QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      "map"       },
    { "/",     QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      "fold"      },
    { "\\",    QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      "scan"      },
    { "':",    QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      NULL        },  /* each-prior: deferred */
    { "/:",    QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      "map-right" },
    { "\\:",   QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      "map-left"  },
};
#define N_Q_OPS ((int)(sizeof Q_OPS / sizeof Q_OPS[0]))

const q_op_t* q_ops_table(int* n) {
    if (n) *n = N_Q_OPS;
    return Q_OPS;
}

int q_lex_is_kw_infix(const char* s, int len) {
    for (int i = 0; i < N_Q_OPS; i++) {
        if (Q_OPS[i].lex != QLEX_KW_INFIX) continue;
        const char* nm = Q_OPS[i].name;
        if ((int)strlen(nm) == len && memcmp(nm, s, (size_t)len) == 0)
            return 1;
    }
    return 0;
}

int q_ops_is_reserved(const char* s, int len) {
    for (int i = 0; i < N_Q_OPS; i++) {
        const char* nm = Q_OPS[i].name;
        if ((int)strlen(nm) == len && memcmp(nm, s, (size_t)len) == 0)
            return 1;
    }
    return 0;
}
