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
 *   (max: rayfall has NO dyadic element-wise max — `max` is register_unary
 *   AGGREGATE, `and`/`or` are register_vary scalar special forms — so wiring
 *   it needs a q_max2_wrap twin of q_min2_wrap = new logic, HELD), monadic `+`
 *   (flip: no builtin), monadic `%` (reciprocal: no builtin).  Dyadic `&`
 *   is the QK_MIN2 wrapper (element-wise min / bool-and) — the `and` keyword
 *   reuses it.  Dyadic `~` is the QK_MATCH wrapper (2c-1).  The remaining
 *   type-dispatch verbs `! ? $ @ .` land in 2c-2.  The bool/null batch keywords
 *   `null`/`any`/`all` are RESERVED-but-deferred (see their rows below); `or`
 *   (|-max keyword) is not rostered until `|`-max lands.
 *
 * KW_INFIX includes {div, each, in, within, and, ...} — a keyword row here is what makes the
 * lexer reclassify the name as an infix verb in noun position (the retired
 * q_is_kw_verb memcmp, now manifest-driven). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_ops.h"
#include <string.h>

/* ===== deterministic / sideeffect AUDIT (feat/q-ops-introspection) =========
 * Every row carries both flags explicitly (last two initializer fields:
 * `deterministic`, `sideeffect`), the common case being `1, 0`.  This block is
 * the COMPLETE roster of the NON-default rows, audited mechanically against
 * every manifest row (anything unobvious doc-checked against qdocs/), and is
 * exposed verbatim by `.oq.ops[]`.
 *
 * deterministic = 0 (result varies run-to-run — RNG state):
 *   `?`   — dyadic `n?x` roll / `-n?x` deal / permute draw on the RNG
 *           (ref/deal.md, ref/roll.md).  Monadic `?` (distinct) is
 *           deterministic, but a row carries one flag for the whole verb, so
 *           the RNG valence makes the verb nondeterministic.
 *   rand  — `rand x` == {first 1?x}, an RNG draw (ref/rand.md).
 *
 * sideeffect = 1 (observable effect beyond the returned value):
 *   set     — assigns a global through a sym handle (ref/get.md `set`).
 *   insert  — appends rows to a NAMED global table by reference (ref/insert.md).
 *   upsert  — named-target upsert rebinds the global (ref/upsert.md).
 *   show    — writes the console display sink (ref/show.md).
 *   system  — runs a `\`-command / shell command (ref/system.md).
 *   hopen   — opens an IPC connection (ref/hopen.md).
 *   hclose  — closes an IPC connection (ref/hopen.md).
 *   0:      — File Text: Save Text writes / Load CSV reads a file (ref/file-text.md).
 *   read0   — reads a file's lines (ref/read0.md).
 *   setenv  — sets a process environment variable (ref/getenv.md).
 * (getenv READS the environment — deterministic given the process env, no
 * mutation — so it is NOT flagged; value/get resolve data purely, the
 * code-executing `value "…"` arm being the caller's effect, not the verb's.)
 * ========================================================================== */

static const q_op_t Q_OPS[] = {
    /* name    lex             mon_kind mon_target   dyad_kind dyad_target  adverb_hof  det eff */
    /* ---- arithmetic / compare glyphs ---- */
    /* monadic `+` is flip — a k-ism accepted as a deliberate superset (valid q
     * spells it `flip`, same QK_FLIP wrapper — the `_`/floor precedent). */
    { "+",     QLEX_GLYPH,     QK_FLIP, "flip",      QK_ENV,   "+",       NULL, 1, 0 },
    { "-",     QLEX_GLYPH,     QK_NEG,  "neg",       QK_ENV,   "-",       NULL, 1, 0 },
    { "*",     QLEX_GLYPH,     QK_ENV,  "first",     QK_ENV,   "*",       NULL, 1, 0 },
    { "%",     QLEX_GLYPH,     QK_NONE, NULL,        QK_ENV,   "/",       NULL, 1, 0 },
    /* monadic `<`/`>` are grade up/down.  Wrappers (QK_IASC/QK_IDESC) so a DICT
     * arm (grade the values, return the keys in value order) is shared with the
     * `iasc`/`idesc` keyword rows below; flat vectors delegate to ray_iasc_fn/
     * ray_idesc_fn unchanged. */
    { "<",     QLEX_GLYPH,     QK_IASC,  "iasc",     QK_ENV,   "<",       NULL, 1, 0 },
    { ">",     QLEX_GLYPH,     QK_IDESC, "idesc",    QK_ENV,   ">",       NULL, 1, 0 },
    { "<=",    QLEX_GLYPH,     QK_NONE, NULL,        QK_ENV,   "<=",      NULL, 1, 0 },
    { ">=",    QLEX_GLYPH,     QK_NONE, NULL,        QK_ENV,   ">=",      NULL, 1, 0 },
    { "=",     QLEX_GLYPH,     QK_ENV,  "group",     QK_EQ,    "==",      NULL, 1, 0 },
    { "<>",    QLEX_GLYPH,     QK_NONE, NULL,        QK_NE,    "!=",      NULL, 1, 0 },
    /* ---- structural glyphs ---- */
    { "#",     QLEX_GLYPH,     QK_ENV,  "count",     QK_TAKE,  "take",    NULL, 1, 0 },
    /* monadic `_` is a K-ism (valid q spells it `floor`) — accepting it is a
     * deliberate SUPERSET of q source; the VALUE is kdb-identical (kdb's own
     * floor IS k `_:`).  The q spelling is the `floor` keyword row below. */
    { "_",     QLEX_GLYPH,     QK_FLOOR, "floor",    QK_DROP,  "drop",    NULL, 1, 0 },
    { "|",     QLEX_GLYPH,     QK_REV,  "reverse",   QK_NONE,  NULL,      NULL, 1, 0 },
    { "&",     QLEX_GLYPH,     QK_WHERE, "where",    QK_MIN2,  "and",     NULL, 1, 0 },
    { ",",     QLEX_GLYPH,     QK_ENV,  "enlist",    QK_JOIN,  "concat",  NULL, 1, 0 },
    { "~",     QLEX_GLYPH,     QK_ENV,  "not",       QK_MATCH, "match",   NULL, 1, 0 },
    /* q `x^y` — fill: coalesce nulls in y with x.  `^` already lexes as a verb
     * glyph (VERB_CHARS); this row gives it a registry value.  Monadic `^x`
     * (kdb: null-of-type / `fills` sans forward-fill) is a deferred cell. */
    { "^",     QLEX_GLYPH,     QK_NONE, NULL,        QK_FILL,  "fill",    NULL, 1, 0 },
    /* ---- type-dispatch glyphs (2c-2) ---- */
    /* monadic `!` (dict keys) is a K-ism accepted as a deliberate superset
     * (valid q spells it `key`, same value — the `_`/floor precedent). */
    { "!",     QLEX_GLYPH,     QK_KEY,  "key",       QK_BANG,  "dict",    NULL, 1, 0 },
    /* monadic `?` (distinct) is likewise a K-ism superset of q `distinct`. */
    { "?",     QLEX_GLYPH,     QK_DISTINCT, "distinct", QK_ROLL, "rand",  NULL, 0, 0 },
    /* monadic `$` stays QK_NONE: `$x` is the cast-vs-cond ambiguity, deferred.
     * Dyadic `$` is cast; the bracket cond form `$[c;t;f]` (3+ args) is a
     * q_lower rewrite onto rayfall `if`, not a registry cell. */
    { "$",     QLEX_GLYPH,     QK_NONE, NULL,        QK_CAST,  "as",      NULL, 1, 0 },
    /* monadic `@`/`.` stay QK_NONE: `@x` type-of is blocked on the q type
     * renumber; `.x` (value/get) comes with handles/namespaces.  Ternary+
     * Trap/Amend forms are deferred cells (error today via arity). */
    { "@",     QLEX_GLYPH,     QK_NONE, NULL,        QK_AT,    "at",      NULL, 1, 0 },
    { ".",     QLEX_GLYPH,     QK_NONE, NULL,        QK_DOT,   "apply",   NULL, 1, 0 },
    /* ---- keyword-infix ---- */
    { "div",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_ENV,   "div",     NULL, 1, 0 },
    /* q `x mod y` — modulus (ref/mod.md).  PURE RENAME: rayfall `%` IS floored
     * modulo with the sign following the divisor, exactly kdb mod (audited
     * live: -3 -2 mod 3 -> 0 1; 7 mod -2 -> -1; -7 mod -2.5 -> -2; null
     * passes through) — the mirror trick of q `%` -> rayfall `/`.  Base `%`
     * is registered RAY_FN_ATOMIC, so vector/dict broadcast comes free. */
    { "mod",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_ENV,   "%",       NULL, 1, 0 },
    /* q `x xexp y` / `x xlog y` — dyadic atomic libm wrappers (the #84
     * atomic-math recipe at valence 2); see q_ops.h QK_XEXP/QK_XLOG. */
    { "xexp",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_XEXP,  "xexp",    NULL, 1, 0 },
    { "xlog",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_XLOG,  "xlog",    NULL, 1, 0 },
    /* q `f each x` == `f'x`: a dyadic wrapper over rayfall map (+ vector
     * collapse, since map returns a boxed list where q wants a simple vec). */
    { "each",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_EACH,  "map",     NULL, 1, 0 },
    /* q `x in y` — QK_IN wrapper (ref/in.md): typed-vector y left-atomic via
     * base ray_in_fn; generic-list y is whole-item, rank-sensitive; mixed
     * numeric families gated to atom/1-item y. */
    { "in",    QLEX_KW_INFIX,  QK_NONE, NULL,        QK_IN,    "in",      NULL, 1, 0 },
    /* q `and` — keyword spelling of `&` (element-wise min / logical AND,
     * ref/and.md, ref/lesser.md).  REUSES the SAME QK_MIN2 kernel the glyph
     * `&` routes to (q_min2_wrap) — no new logic.  Numeric/bool are kdb-true
     * (`2 and 3`->2, `1010b and 1100b`->1000b); the char-min arm (`"sat" and
     * "cow"`->"cat") is a shared q_min2_wrap gap (DEFERRED, needs a char arm =
     * new logic).  Monadic cell stays QK_NONE: q `and` is dyadic-only, so
     * prefix `and x` misses and eval falls through to rayfall's scalar `and`
     * special form (DEFERRED edge — a monadic wrapper would be new logic). */
    { "and",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_MIN2,  "and",     NULL, 1, 0 },
    /* q `x within y` — inclusive bounds check (ref/within.md); wrapper because
     * base ray_within_fn is vector-vals-only and width-blind on the range. */
    { "within",QLEX_KW_INFIX,  QK_NONE, NULL,        QK_WITHIN,"within",  NULL, 1, 0 },
    /* q `n cut x` — chunk (int atom) / positional cut (int vector). */
    { "cut",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_CUT,   "cut",     NULL, 1, 0 },
    /* q `n rotate x` / `n sublist x` — dyadic infix keywords (kdb rotate.md /
     * sublist.md).  Not KW_INFIX -> the scanner would split `n rotate x` into
     * two statements, so the manifest row is what makes them infix. */
    { "rotate",QLEX_KW_INFIX,  QK_NONE, NULL,        QK_ROTATE, "rotate", NULL, 1, 0 },
    { "sublist",QLEX_KW_INFIX, QK_NONE, NULL,        QK_SUBLIST,"sublist",NULL, 1, 0 },
    /* q `x vs y` / `x sv y` — split-join / base-encode family (dyadic infix
     * keywords; wrappers, native -RAY_STR + sym + base + byte).  Monadic form
     * is out of scope (kdb `vs`/`sv` are strictly dyadic). */
    { "vs",    QLEX_KW_INFIX,  QK_NONE, NULL,        QK_VS,    "vs",      NULL, 1, 0 },
    { "sv",    QLEX_KW_INFIX,  QK_NONE, NULL,        QK_SV,    "sv",      NULL, 1, 0 },
    /* ---- set operations (feat/q-setops) ---- */
    /* q `x except y` — items of x not in y, x-duplicates and order KEPT
     * (ref/except.md).  rayfall's ray_except_fn is exactly this for lists;
     * the QK_EXCEPT wrapper adds a TABLE-pair arm (row membership) and
     * delegates everything else to ray_except_fn unchanged (was QK_ENV
     * pre-table-verbs).  dyad_target stays "except" for provenance/serde. */
    { "except",QLEX_KW_INFIX,  QK_NONE, NULL,        QK_EXCEPT,"except",  NULL, 1, 0 },
    /* q `x union y` == `distinct x,y` — WRAPPER: rayfall union keeps
     * x-duplicates, kdb dedups the whole join (ref/union.md). */
    { "union", QLEX_KW_INFIX,  QK_NONE, NULL,        QK_UNION, "union",   NULL, 1, 0 },
    /* q `x inter y` — items of x that are in y, x-duplicates and order KEPT
     * (ref/inter.md: "uses the result of x in y to return items from x").
     * rayfall spells it `sect` (ray_sect_fn) and is exactly this for lists;
     * the thin wrapper only guards dict/table operands 'nyi (rayfall sect
     * returns a wrong-shaped dict there; kdb returns common values). */
    { "inter", QLEX_KW_INFIX,  QK_NONE, NULL,        QK_INTER, "sect",    NULL, 1, 0 },
    /* q `x cross y` == {raze x,/:\:y} — Cartesian product WRAPPER (no
     * rayfall cartesian primitive; composes item access + q join).  String
     * and dict/table operands are deferred cells (ref/cross.md). */
    { "cross", QLEX_KW_INFIX,  QK_NONE, NULL,        QK_CROSS, "cross",   NULL, 1, 0 },
    /* ---- q join family (feat/q-joins-rebuild) ----
     * Dyadic infix keywords over the engine hash/asof join (pair-relation
     * core in q_registry.c: rowid-augmented key tables through
     * ray_left_join_fn / ray_asof_join_fn, kdb-ordered, gather-assembled). */
    { "lj",    QLEX_KW_INFIX,  QK_NONE, NULL,        QK_LJ,    "lj",      NULL, 1, 0 },
    { "ljf",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_LJF,   "ljf",     NULL, 1, 0 },
    { "ij",    QLEX_KW_INFIX,  QK_NONE, NULL,        QK_IJ,    "ij",      NULL, 1, 0 },
    { "ijf",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_IJF,   "ijf",     NULL, 1, 0 },
    { "uj",    QLEX_KW_INFIX,  QK_NONE, NULL,        QK_UJ,    "uj",      NULL, 1, 0 },
    { "ujf",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_UJF,   "ujf",     NULL, 1, 0 },
    { "pj",    QLEX_KW_INFIX,  QK_NONE, NULL,        QK_PJ,    "pj",      NULL, 1, 0 },
    { "asof",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_ASOF,  "asof",    NULL, 1, 0 },
    /* Bracket-form joins (ej[c;t1;t2], aj[...], wj[w;f;t;spec]): triadic-plus
     * prefix keywords — q-owned env VARY bindings (q_builtins_register),
     * snapshotted here as QK_ENV rows (the ssr precedent: the parser
     * name-refs `ej[a;b;c]`, so it resolves through the env). */
    { "ej",    QLEX_KW_PREFIX, QK_ENV,  "ej",        QK_NONE,  NULL,      NULL, 1, 0 },
    { "aj",    QLEX_KW_PREFIX, QK_ENV,  "aj",        QK_NONE,  NULL,      NULL, 1, 0 },
    { "aj0",   QLEX_KW_PREFIX, QK_ENV,  "aj0",       QK_NONE,  NULL,      NULL, 1, 0 },
    { "ajf",   QLEX_KW_PREFIX, QK_ENV,  "ajf",       QK_NONE,  NULL,      NULL, 1, 0 },
    { "ajf0",  QLEX_KW_PREFIX, QK_ENV,  "ajf0",      QK_NONE,  NULL,      NULL, 1, 0 },
    { "wj",    QLEX_KW_PREFIX, QK_ENV,  "wj",        QK_NONE,  NULL,      NULL, 1, 0 },
    { "wj1",   QLEX_KW_PREFIX, QK_ENV,  "wj1",       QK_NONE,  NULL,      NULL, 1, 0 },
    /* ---- sort / bucket family (feat/q-sort-rank) — dyadic infix ----
     * `bin`/`binr` reuse rayfall verbatim (same arg order: sorted-vec left,
     * value right).  `xrank` is a clean pass-through (n left, vec right).
     * `xbar` is an ARG-SWAP wrapper (rayfall xbar is (col,bucket); q spells it
     * (bucket,col)).  All infix so the scanner treats `a verb b` as one stmt. */
    { "bin",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_ENV,   "bin",     NULL, 1, 0 },
    { "binr",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_ENV,   "binr",    NULL, 1, 0 },
    { "xrank", QLEX_KW_INFIX,  QK_NONE, NULL,        QK_ENV,   "xrank",   NULL, 1, 0 },
    { "xbar",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_XBAR,  "xbar",    NULL, 1, 0 },
    /* ---- Wave 5: running scans (monadic prefix keywords) ---- */
    { "sums",  QLEX_KW_PREFIX, QK_SUMS,  "sums",   QK_NONE,  NULL,      NULL, 1, 0 },
    { "prds",  QLEX_KW_PREFIX, QK_PRDS,  "prds",   QK_NONE,  NULL,      NULL, 1, 0 },
    /* q `prd x` — the multiply-over fold twin of prds (ref/prd.md): nulls
     * are 1s, bool vector -> int, list-of-lists element-wise, dict/table
     * implicit iteration.  Wrapper (no rayfall product aggregate). */
    { "prd",   QLEX_KW_PREFIX, QK_PRD,   "prd",    QK_NONE,  NULL,      NULL, 1, 0 },
    { "maxs",  QLEX_KW_PREFIX, QK_MAXS,  "maxs",   QK_NONE,  NULL,      NULL, 1, 0 },
    { "mins",  QLEX_KW_PREFIX, QK_MINS,  "mins",   QK_NONE,  NULL,      NULL, 1, 0 },
    { "avgs",  QLEX_KW_PREFIX, QK_AVGS,  "avgs",   QK_NONE,  NULL,      NULL, 1, 0 },
    { "ratios",QLEX_KW_PREFIX, QK_RATIOS,"ratios", QK_NONE,  NULL,      NULL, 1, 0 },
    /* ---- Wave 5: weighted (dyadic infix keywords) ---- */
    { "wsum",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_WSUM,  "wsum",    NULL, 1, 0 },
    { "wavg",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_WAVG,  "wavg",    NULL, 1, 0 },
    /* ---- Wave 5: statistical (renames of audited base aggregates) ----
     * kdb `var`/`dev` are POPULATION (÷n); rayfall `var`/`stddev` are SAMPLE
     * (÷n-1) and `var_pop`/`stddev_pop`/`dev` are population — so q `var`->
     * `var_pop`, q `svar`->`var`, q `sdev`->`stddev`, q `dev` stays `dev`. */
    { "med",   QLEX_KW_PREFIX, QK_ENV, "med",       QK_NONE,  NULL,      NULL, 1, 0 },
    { "var",   QLEX_KW_PREFIX, QK_ENV, "var_pop",   QK_NONE,  NULL,      NULL, 1, 0 },
    { "svar",  QLEX_KW_PREFIX, QK_ENV, "var",       QK_NONE,  NULL,      NULL, 1, 0 },
    { "sdev",  QLEX_KW_PREFIX, QK_ENV, "stddev",    QK_NONE,  NULL,      NULL, 1, 0 },
    { "cor",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_ENV,   "pearson_corr", NULL, 1, 0 },
    { "cov",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_COV,   "cov",     NULL, 1, 0 },
    { "scov",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_SCOV,  "scov",    NULL, 1, 0 },
    /* ---- Wave 5: sliding m-windows + ema (dyadic infix keywords) ---- */
    { "msum",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_MSUM,  "msum",    NULL, 1, 0 },
    { "mavg",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_MAVG,  "mavg",    NULL, 1, 0 },
    { "mmax",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_MMAX,  "mmax",    NULL, 1, 0 },
    { "mmin",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_MMIN,  "mmin",    NULL, 1, 0 },
    { "mcount",QLEX_KW_INFIX,  QK_NONE, NULL,        QK_MCOUNT,"mcount",  NULL, 1, 0 },
    { "mdev",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_MDEV,  "mdev",    NULL, 1, 0 },
    { "ema",   QLEX_KW_INFIX,  QK_NONE, NULL,        QK_EMA,   "ema",     NULL, 1, 0 },
    /* iterator mnemonic keywords (wave-2): infix `f over/scan/prior/peach x`,
     * same lexical treatment as `each`.  over/scan dispatch reduce/converge/
     * do/while by f rank; prior is unary each-prior; peach == each. */
    { "over",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_OVER,  "over",    NULL, 1, 0 },
    { "scan",  QLEX_KW_INFIX,  QK_NONE, NULL,        QK_SCANKW,"scan-kw", NULL, 1, 0 },
    { "prior", QLEX_KW_INFIX,  QK_NONE, NULL,        QK_PRIORKW,"prior",  NULL, 1, 0 },
    { "peach", QLEX_KW_INFIX,  QK_NONE, NULL,        QK_EACH,  "peach",   NULL, 1, 0 },
    /* ---- keyword-prefix monads (pass-through/rename) ---- */
    { "neg",     QLEX_KW_PREFIX, QK_NEG, "neg",      QK_NONE,  NULL,      NULL, 1, 0 },
    { "til",     QLEX_KW_PREFIX, QK_TIL, "til",      QK_NONE,  NULL,      NULL, 1, 0 },
    { "count",   QLEX_KW_PREFIX, QK_ENV, "count",    QK_NONE,  NULL,      NULL, 1, 0 },
    { "first",   QLEX_KW_PREFIX, QK_ENV, "first",    QK_NONE,  NULL,      NULL, 1, 0 },
    { "last",    QLEX_KW_PREFIX, QK_ENV, "last",     QK_NONE,  NULL,      NULL, 1, 0 },
    /* q `next x` / `prev x` — shift a vector by one, null-filling the vacated
     * end (kdb next.md / prev.md).  No rayfall counterpart, so wrappers. */
    { "next",    QLEX_KW_PREFIX, QK_NEXT, "next",    QK_NONE,  NULL,      NULL, 1, 0 },
    { "prev",    QLEX_KW_PREFIX, QK_PREV, "prev",    QK_NONE,  NULL,      NULL, 1, 0 },
    /* q `n xprev x` — n-item shift (ref/next.md); dyadic infix like rotate. */
    { "xprev",   QLEX_KW_INFIX,  QK_NONE, NULL,      QK_XPREV, "xprev",   NULL, 1, 0 },
    /* q `fills x` — forward-fill nulls (ref/fill.md; the `^\` fill-scan). */
    { "fills",   QLEX_KW_PREFIX, QK_FILLS, "fills",  QK_NONE,  NULL,      NULL, 1, 0 },
    { "where",   QLEX_KW_PREFIX, QK_WHERE, "where",  QK_NONE,  NULL,      NULL, 1, 0 },
    { "reverse", QLEX_KW_PREFIX, QK_REV, "reverse",  QK_NONE,  NULL,      NULL, 1, 0 },
    /* q `sum` over a boxed LIST sums the items (`sum(dates;times)` ->
     * timestamps, ref/file-text.md); rayfall sum is vector-only, so wrapper. */
    { "sum",     QLEX_KW_PREFIX, QK_SUM, "sum",      QK_NONE,  NULL,      NULL, 1, 0 },
    { "group",   QLEX_KW_PREFIX, QK_ENV, "group",    QK_NONE,  NULL,      NULL, 1, 0 },
    /* ---- sort / grade family (feat/q-sort-rank) — monadic prefix ----
     * asc/desc reuse ray_asc_fn/ray_desc_fn for flat vectors (VALUE kdb-true;
     * the sorted `s#` attribute is a deferred divergence — rayfall has no sorted
     * attribute) and add a DICT-sort-by-value arm.  iasc/idesc reuse ray_iasc_fn/
     * ray_idesc_fn and share the same DICT-grade arm as the `<`/`>` glyphs. */
    { "asc",     QLEX_KW_PREFIX, QK_ASC,   "asc",    QK_NONE,  NULL,      NULL, 1, 0 },
    { "desc",    QLEX_KW_PREFIX, QK_DESC,  "desc",   QK_NONE,  NULL,      NULL, 1, 0 },
    { "iasc",    QLEX_KW_PREFIX, QK_IASC,  "iasc",   QK_NONE,  NULL,      NULL, 1, 0 },
    { "idesc",   QLEX_KW_PREFIX, QK_IDESC, "idesc",  QK_NONE,  NULL,      NULL, 1, 0 },
    { "avg",     QLEX_KW_PREFIX, QK_ENV, "avg",      QK_NONE,  NULL,      NULL, 1, 0 },
    /* q `floor` returns LONGS from floats (kdb `floor 3.7` is 3j); rayfall's
     * env floor keeps f64, so this is the QK_FLOOR wrapper, not a rename. */
    { "floor",   QLEX_KW_PREFIX, QK_FLOOR, "floor",  QK_NONE,  NULL,      NULL, 1, 0 },
    /* q dict accessors: `key`/`value` are wrappers (dict-only in 2c-2 —
     * the file-handle/namespace/enumeration overloads are deferred cells);
     * `distinct` must preserve FIRST-OCCURRENCE order (kdb), while rayfall's
     * distinct routes typed vectors through the DAG group path, which sorts
     * — so it too is a wrapper, not a rename (audited: `distinct 2 3 7 3
     * 5 3` must be 2 3 7 5, env distinct gives 2 3 5 7). */
    { "key",     QLEX_KW_PREFIX, QK_KEY,      "key",      QK_NONE, NULL,  NULL, 1, 0 },
    { "value",   QLEX_KW_PREFIX, QK_VALUE,    "value",    QK_NONE, NULL,  NULL, 1, 0 },
    /* q `get` is a SYNONYM of `value` (ref/get.md: "completely interchangeable")
     * — same QK_VALUE wrapper, one home.  `nam set y` writes the named global
     * (sym-handle assign / `. context restore); file forms are 'nyi cells. */
    { "get",     QLEX_KW_PREFIX, QK_VALUE,    "value",    QK_NONE, NULL,  NULL, 1, 0 },
    { "set",     QLEX_KW_INFIX,  QK_NONE,     NULL,       QK_SETG, "set-g", NULL, 1, 1 },
    { "distinct",QLEX_KW_PREFIX, QK_DISTINCT, "distinct", QK_NONE, NULL,  NULL, 1, 0 },
    /* q `rand x` == {first 1?x} (ref/rand.md) — QK_RAND wrapper reusing the
     * `?` roll/generate arms.  NOT the rayfall binary `rand` (which stays the
     * env-bound engine builtin the QK_ROLL wrapper calls). */
    { "rand",    QLEX_KW_PREFIX, QK_RAND,     "rand-1",   QK_NONE, NULL,  NULL, 0, 0 },
    /* q-implemented keywords: env bindings added by q_builtins_register
     * (same mechanism as `parse`), snapshotted here as pass-through rows. */
    { "string",  QLEX_KW_PREFIX, QK_ENV, "string",   QK_NONE,  NULL,      NULL, 1, 0 },
    { "upper",   QLEX_KW_PREFIX, QK_ENV, "upper",    QK_NONE,  NULL,      NULL, 1, 0 },
    { "lower",   QLEX_KW_PREFIX, QK_ENV, "lower",    QK_NONE,  NULL,      NULL, 1, 0 },
    /* ---- string trim / hash / search family (feat/q-string-fns) ----
     * trim/ltrim/rtrim/md5 are q-owned env unaries (q_builtins_register),
     * snapshotted here as QK_ENV prefix rows (same mechanism as `string`).
     * `like`/`ss` are dyadic infix; `ssr` is a triadic-prefix wrapper. */
    { "trim",    QLEX_KW_PREFIX, QK_ENV, "trim",     QK_NONE,  NULL,      NULL, 1, 0 },
    { "ltrim",   QLEX_KW_PREFIX, QK_ENV, "ltrim",    QK_NONE,  NULL,      NULL, 1, 0 },
    { "rtrim",   QLEX_KW_PREFIX, QK_ENV, "rtrim",    QK_NONE,  NULL,      NULL, 1, 0 },
    { "md5",     QLEX_KW_PREFIX, QK_ENV, "md5",      QK_NONE,  NULL,      NULL, 1, 0 },
    { "like",    QLEX_KW_INFIX,  QK_NONE, NULL,      QK_LIKE,  "like",    NULL, 1, 0 },
    { "ss",      QLEX_KW_INFIX,  QK_NONE, NULL,      QK_SS,    "ss",      NULL, 1, 0 },
    { "ssr",     QLEX_KW_PREFIX, QK_ENV, "ssr",      QK_NONE,  NULL,      NULL, 1, 0 },
    { "show",    QLEX_KW_PREFIX, QK_ENV, "show",     QK_NONE,  NULL,      NULL, 1, 1 },
    /* `system "…"` — q-owned env unary (q_builtins_register), snapshotted here
     * so the parser embeds it; routes through q_sys_dispatch (q_sys.c). */
    { "system",  QLEX_KW_PREFIX, QK_ENV, "system",   QK_NONE,  NULL,      NULL, 1, 1 },
    /* table introspection — q-owned bindings (q_builtins_register), snapshotted
     * here so the parser embeds them over the base env `meta`. */
    { "meta",    QLEX_KW_PREFIX, QK_ENV, "meta",     QK_NONE,  NULL,      NULL, 1, 0 },
    { "cols",    QLEX_KW_PREFIX, QK_ENV, "cols",     QK_NONE,  NULL,      NULL, 1, 0 },
    /* q `attr x` — column attribute as a single symbol (ref/set-attribute.md,
     * ref/attr.md).  Monadic wrapper (QK_ATTR) over the engine's `.attr.get`,
     * collapsing the full-name sym vector to kdb's one letter.  The set/clear
     * side is the `#` verb's symbol-atom arm (q_take_wrap), not a row here. */
    { "attr",    QLEX_KW_PREFIX, QK_ATTR, "attr",    QK_NONE,  NULL,      NULL, 1, 0 },
    /* ---- additional monadic pass-through keywords (rayfall name == q name,
     * audited kdb-true element-wise / aggregate semantics). See
     * docs/recipes/add-q-keyword-verb.md. ---- */
    { "abs",     QLEX_KW_PREFIX, QK_ENV, "abs",      QK_NONE,  NULL,      NULL, 1, 0 },
    /* q `null x` — elementwise null test.  Routes to the engine's atomic
     * `nil?` (RAY_FN_ATOMIC): broadcasts over vectors and nested lists at
     * every depth.  QK_NULL collapses a homogeneous top-level bool-atom run
     * (heterogeneous input list) to a bool vector for kdb-true display. */
    { "null",    QLEX_KW_PREFIX, QK_NULL, "nil?",    QK_NONE,  NULL,      NULL, 1, 0 },
    { "dev",     QLEX_KW_PREFIX, QK_ENV, "dev",      QK_NONE,  NULL,      NULL, 1, 0 },
    { "exp",     QLEX_KW_PREFIX, QK_ENV, "exp",      QK_NONE,  NULL,      NULL, 1, 0 },
    { "log",     QLEX_KW_PREFIX, QK_ENV, "log",      QK_NONE,  NULL,      NULL, 1, 0 },
    { "max",     QLEX_KW_PREFIX, QK_ENV, "max",      QK_NONE,  NULL,      NULL, 1, 0 },
    { "min",     QLEX_KW_PREFIX, QK_ENV, "min",      QK_NONE,  NULL,      NULL, 1, 0 },
    { "rank",    QLEX_KW_PREFIX, QK_ENV, "rank",     QK_NONE,  NULL,      NULL, 1, 0 },
    { "raze",    QLEX_KW_PREFIX, QK_RAZE, "raze",    QK_NONE,  NULL,      NULL, 1, 0 },
    { "sqrt",    QLEX_KW_PREFIX, QK_ENV, "sqrt",     QK_NONE,  NULL,      NULL, 1, 0 },
    /* ---- atomic unary math (feat/q-math-atomic) — implement-via-libm.
     * rayfall has no trig/reciprocal/signum primitive, so these are wrappers
     * (not QK_ENV renames).  All monadic KW_PREFIX; RAY_FN_ATOMIC broadcast.
     * `ceiling` is the QK_FLOOR-twin (float->LONG), NOT rayfall `ceil` (f64). */
    { "sin",       QLEX_KW_PREFIX, QK_SIN,        "sin",       QK_NONE, NULL, NULL, 1, 0 },
    { "cos",       QLEX_KW_PREFIX, QK_COS,        "cos",       QK_NONE, NULL, NULL, 1, 0 },
    { "tan",       QLEX_KW_PREFIX, QK_TAN,        "tan",       QK_NONE, NULL, NULL, 1, 0 },
    { "asin",      QLEX_KW_PREFIX, QK_ASIN,       "asin",      QK_NONE, NULL, NULL, 1, 0 },
    { "acos",      QLEX_KW_PREFIX, QK_ACOS,       "acos",      QK_NONE, NULL, NULL, 1, 0 },
    { "atan",      QLEX_KW_PREFIX, QK_ATAN,       "atan",      QK_NONE, NULL, NULL, 1, 0 },
    { "reciprocal",QLEX_KW_PREFIX, QK_RECIPROCAL, "reciprocal",QK_NONE, NULL, NULL, 1, 0 },
    { "signum",    QLEX_KW_PREFIX, QK_SIGNUM,     "signum",    QK_NONE, NULL, NULL, 1, 0 },
    { "ceiling",   QLEX_KW_PREFIX, QK_CEILING,    "ceiling",   QK_NONE, NULL, NULL, 1, 0 },
    /* ---- table verbs (feat/q-table-verbs) — all QK wrappers in q_registry.c,
     * built over the wave-4 keyed primitives (q_enkey/q_table_flatten) and the
     * base sort kernel (ray_xasc_fn, ARG-SWAPPED like xbar).  `insert`/`upsert`
     * intentionally SHADOW the base env special forms of the same name: q
     * semantics differ (by-name targets, keyed collision/update, row-index
     * results), so the registry value is a wrapper, never the env snapshot. */
    { "flip",   QLEX_KW_PREFIX, QK_FLIP,    "flip",    QK_NONE,   NULL,      NULL, 1, 0 },
    { "keys",   QLEX_KW_PREFIX, QK_KEYS,    "keys",    QK_NONE,   NULL,      NULL, 1, 0 },
    { "ungroup",QLEX_KW_PREFIX, QK_UNGROUP, "ungroup", QK_NONE,   NULL,      NULL, 1, 0 },
    { "xkey",   QLEX_KW_INFIX,  QK_NONE,    NULL,      QK_XKEY,   "xkey",    NULL, 1, 0 },
    { "xcol",   QLEX_KW_INFIX,  QK_NONE,    NULL,      QK_XCOL,   "xcol",    NULL, 1, 0 },
    { "xcols",  QLEX_KW_INFIX,  QK_NONE,    NULL,      QK_XCOLS,  "xcols",   NULL, 1, 0 },
    { "xasc",   QLEX_KW_INFIX,  QK_NONE,    NULL,      QK_XASC,   "xasc",    NULL, 1, 0 },
    { "xdesc",  QLEX_KW_INFIX,  QK_NONE,    NULL,      QK_XDESC,  "xdesc",   NULL, 1, 0 },
    { "xgroup", QLEX_KW_INFIX,  QK_NONE,    NULL,      QK_XGROUP, "xgroup",  NULL, 1, 0 },
    { "insert", QLEX_KW_INFIX,  QK_NONE,    NULL,      QK_INSERT, "insert",  NULL, 1, 1 },
    { "upsert", QLEX_KW_INFIX,  QK_NONE,    NULL,      QK_UPSERT, "upsert",  NULL, 1, 1 },
    /* each-prior mnemonics: deltas x == (-':)x, differ x == not(~':)x. */
    { "deltas",  QLEX_KW_PREFIX, QK_DELTAS, "deltas", QK_NONE, NULL,      NULL, 1, 0 },
    { "differ",  QLEX_KW_PREFIX, QK_DIFFER, "differ", QK_NONE, NULL,      NULL, 1, 0 },
    /* ---- boolean/null batch: RESERVED-but-DEFERRED (feat/q-bool-null) ----
     * `any`/`all` are real q keywords rostered to reserve the name
     * (`any:5`/`all:5` -> 'assign, kdb-true) with QK_NONE (no value) — a
     * documented DEFER: their range is boolean `b` for every domain
     * (ref/all-any.md) and a `max`/`min` rename is type-wrong for non-boolean
     * input (`all 2000.01.02 2010.01.02`->1b, but max is a DATE); the boolean
     * coercion is new logic (HELD).  KW_PREFIX so the reservation does not
     * reclassify the scanner.
     * NB: `null` LANDED separately as a real atomic verb (QK_NULL,
     * feat/q-atomic-extend #67) — its reserved row here was dropped on merge to
     * avoid a duplicate.  `or` is the keyword spelling of `|`-max and waits for
     * `|`-max (a valueless QLEX_KW_INFIX row would expose rayfall's scalar `or`). */
    { "any",     QLEX_KW_PREFIX, QK_NONE, NULL,       QK_NONE, NULL,      NULL, 1, 0 },
    { "all",     QLEX_KW_PREFIX, QK_NONE, NULL,       QK_NONE, NULL,      NULL, 1, 0 },
    /* ---- IPC client verbs (feat/q-ipc-client, Phase D) — thin wrappers over the
     * kdb-speaking `.ipc.*` primitives (Phase C).  `hopen` normalizes int|string|
     * (conn;timeout) into the `.ipc.open` string API and returns the connection's
     * socket fd as the handle (kdb-faithful — 0/1/2 reserved, connections at 3+);
     * `hclose` translates it back and routes to `.ipc.close`.  The sync/async send
     * verb `h"query"` is handle-as-verb application (q_apply.c int-head arm), not a
     * manifest row.  Both are monadic prefix keywords (KW_PREFIX). ---- */
    { "hopen",  QLEX_KW_PREFIX, QK_HOPEN,  "hopen",  QK_NONE, NULL,  NULL, 1, 1 },
    { "hclose", QLEX_KW_PREFIX, QK_HCLOSE, "hclose", QK_NONE, NULL,  NULL, 1, 1 },
    /* ---- File Text (feat/q-file-text) — the `0:` operator + companions.
     * `0:` is tokenized by the scanner's digit-colon arm (a single 0/1/2
     * glued to ':' can never start a clock literal) and dispatches on the
     * LEFT operand's shape (see QK_FILETEXT).  `1:`/`2:` tokenize the same
     * way but have NO row here — binary file formats are an owner-ruled
     * non-goal, so they stay name-refs ('name).  `read0` is also env-bound
     * by q_builtins for the bracket-call form (the ssr/value precedent). */
    { "0:",     QLEX_GLYPH,     QK_NONE,   NULL,     QK_FILETEXT, "file-text", NULL, 1, 1 },
    { "hsym",   QLEX_KW_PREFIX, QK_HSYM,   "hsym",   QK_NONE, NULL,  NULL, 1, 0 },
    { "read0",  QLEX_KW_PREFIX, QK_READ0,  "read0",  QK_NONE, NULL,  NULL, 1, 1 },
    /* ---- environment variables (feat/q-getenv-setenv, ref/getenv.md) ----
     * `getenv` is a monadic prefix keyword (sym -> value string, "" unset);
     * `setenv` is a dyadic infix keyword (`sym setenv str`), so it MUST be
     * KW_INFIX or the scanner would split `x setenv y` into two statements
     * (the `set`/`xkey` precedent).  Both reuse the base .os.getenv/.os.setenv
     * C primitives (ray_getenv_fn/ray_setenv_fn) via q_registry.c wrappers that
     * coerce the q symbol arg to the -RAY_STR the C wants — a real divergence,
     * so QK wrappers, not QK_ENV renames. */
    { "getenv", QLEX_KW_PREFIX, QK_GETENV, "getenv", QK_NONE,   NULL,     NULL, 1, 0 },
    { "setenv", QLEX_KW_INFIX,  QK_NONE,   NULL,     QK_SETENV, "setenv", NULL, 1, 1 },
    /* ---- adverbs — q adverbs ARE rayfall higher-order fns (no bespoke object).
     * `+/` lowers to fold over `+` (q_lower); `/:`/`\:` ARE map-right/map-left
     * (src/ops/collection.c:2279 — map-left iterates LEFT holding right =
     * q each-left, verified against examples/rfl).  Each-prior `':` still has
     * no rayfall counterpart (the scan-* variants are fold-style, not
     * pairwise) — DEFERRED, still lexer-classified. ---- */
    { "'",     QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      "map", 1, 0 },
    { "/",     QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      "fold", 1, 0 },
    { "\\",    QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      "scan", 1, 0 },
    { "':",    QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      NULL, 1, 0 },  /* each-prior: deferred */
    { "/:",    QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      "map-right", 1, 0 },
    { "\\:",   QLEX_ADVERB,    QK_NONE, NULL,        QK_NONE,  NULL,      "map-left", 1, 0 },
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

/* Operator->integer opcode (`value +` -> 1) is DEFERRED: the opcode is a property
 * of the FUNCTION IDENTITY, not the spelling (`get`/`value` are two names for ONE
 * function, kdb `.:`), so a name-keyed table can't model it.  See PLAN.md; the doc
 * rows are pinned red-below-floor in test/q/function/value.qcmd. */
