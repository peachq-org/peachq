/* q op manifest — the SINGLE authoritative table of every in-scope q verb.
 *
 * One source, two consumers (spec 2026-07-03-q-op-registry-complete-design.md,
 * "Bootstrap — split lexical metadata from runtime values"):
 *   1. the LEXER (src/qlang/q_parse.c) — static classification, zero runtime
 *      dependency (the scanner runs before eval), via q_lex_is_kw_infix();
 *   2. the REGISTRY builder (src/qlang/q_registry.c) — builds the runtime
 *      (name, valence) -> ray_t function value + q-surface provenance, by
 *      iterating q_ops_table().
 *
 * Scope = STAGE 2a: pass-through / rename / wrapper verbs only.  Type-dispatch
 * verbs (`! ? $ @ . _`... the polymorphic ones) and qSQL are OUT (2c / piece 3),
 * so they are absent here — a registry miss means "not resolvable at that
 * valence," exactly as today.  Deferred glyphs whose q semantics have NO clean
 * rayfall target (audited: no dyadic element-wise min/max for `&`/`|`, no
 * `flip`, no `reciprocal`, no `match`) carry QK_NONE at that valence rather
 * than a guessed binding. */
#ifndef Q_OPS_H
#define Q_OPS_H

#include <stddef.h>

/* Lexical class — how the scanner treats the token.  Only KW_INFIX is
 * reclassified from a name-ref noun into an infix verb (in noun position);
 * KW_PREFIX keywords stay name-ref nouns (resolved at eval), glyphs are the
 * CL_VERB chars, adverbs the CL_ADVERB chars.  Held so the manifest is the
 * one place the lexer's keyword-verb set is defined. */
typedef enum {
    QLEX_GLYPH = 1,     /* +, -, =, #, <=, ... (single/multi-char verb glyph) */
    QLEX_KW_INFIX,      /* alnum keyword usable infix between nouns (div)      */
    QLEX_KW_PREFIX,     /* alnum keyword monad/prefix (til, count, neg, ...)   */
    QLEX_ADVERB         /* / \ ' /: \: ':                                       */
} q_lex_class;

/* How the registry builds the value for one (row, valence).  QK_ENV reuses a
 * rayfall builtin by name (identity or rename); the QK_* wrappers are the
 * bespoke q-semantics ops whose canonical rayfall lowering name is the target
 * string (== != take drop). */
typedef enum {
    QK_NONE = 0,        /* no value at this valence (deferred / not applicable) */
    QK_ENV,             /* reuse env builtin named `target` (pass-through/rename)*/
    QK_EQ,              /* q `=`  wrapper (element-wise char-str eq, else ==)    */
    QK_NE,              /* q `<>` wrapper (element-wise char-str neq, else !=)   */
    QK_TAKE,            /* q `#`  wrapper (arg-swap take)                        */
    QK_DROP,            /* q `_`  wrapper (count-drop via range-take)            */
    QK_EACH,            /* q `each` wrapper (rayfall map + vector collapse)      */
    QK_MATCH,           /* q `~`  wrapper (recursive whole-value equivalence)    */
    QK_FLOOR,           /* q monadic `_` wrapper (floor to LONG; rayfall keeps f64) */
    QK_BANG,            /* q `x!y` wrapper (dict make over rayfall dict)         */
    QK_KEY,             /* q `key`/monadic `!` wrapper (dict keys)               */
    QK_VALUE,           /* q `value` wrapper (dict vals, collapsed)              */
    QK_DISTINCT,        /* q `distinct`/monadic `?` wrapper (FIRST-OCCURRENCE
                         * order; rayfall's distinct DAG path sorts)             */
    QK_ROLL,            /* q `x?y` wrapper (find with kdb miss->count / roll /
                         * pick; deal + float roll are deferred cells)           */
    QK_CAST,            /* q `t$x` wrapper (rayfall as + kdb float->int ROUNDING;
                         * Tok string-parse and unknown designators deferred)    */
    QK_AT,              /* q `f@x` wrapper (Apply At / Index At: callables
                         * invoke unary, nouns index via q_apply_noun)           */
    QK_DOT,             /* q `v . vx` wrapper (Apply / Index: rhs is the ARG
                         * LIST — spread-call callables, depth-index nouns)      */
    QK_MIN2,            /* q dyadic `&` wrapper (element-wise min / bool-and)     */
    QK_NEG,             /* q `neg`/monadic `-` wrapper: negates a DATE's day
                         * count PRESERVING the type (base ray_neg_fn rejects
                         * temporals); non-date input passes through.  time/
                         * timestamp arms are deferred with their datatypes.   */
    QK_CUT,             /* q `cut` wrapper (int-atom chunk / int-vector cut)     */
    QK_WITHIN,          /* q `within` wrapper (bounds check; base ray_within_fn
                         * is vector-vals-only + width-blind, so atom vals
                         * enlist and widths are guarded)                      */
    /* ---- iterator keyword wrappers (wave-2 adverb completion) ---- */
    QK_OVER,            /* q `f over x` == `f/x` (reduce/converge/do/while)     */
    QK_SCANKW,          /* q `f scan x` == `f\x`                                */
    QK_PRIORKW,         /* q `f prior x` == `(f':)x` (unary each-prior)         */
    QK_DELTAS,          /* q `deltas x` == `(-':)x`                             */
    QK_DIFFER,          /* q `differ x` == `not(~':)x`                          */
    QK_TIL,             /* q `til` wrapper: accepts a boolean (`til 1b`->,0);
                         * base ray_til_fn is int-only.  Delegates otherwise.   */
    QK_WHERE,           /* q `where`/monadic `&` wrapper: an integer vector
                         * repeats each index i, x[i] times (`where 2 3 1` ->
                         * 0 0 1 1 1 2); the boolean form delegates to base.     */
    QK_VS,              /* q `x vs y` — split / base-decompose / byte-encode
                         * (string split, sym split, int base decompose, byte
                         * big-endian encode, bit decompose)                    */
    QK_SV,              /* q `x sv y` — join / base-compose / byte-decode
                         * (inverse of vs)                                      */
    /* ---- Wave 5 aggregate / uniform family ---- */
    QK_SUMS,            /* running sum  (nulls -> 0)                            */
    QK_PRDS,            /* running product (nulls -> 1)                         */
    QK_MAXS,            /* running max (nulls skipped; also runs over chars)    */
    QK_MINS,            /* running min (nulls skipped; also runs over chars)    */
    QK_AVGS,            /* running average (nulls excluded) -> float            */
    QK_RATIOS,          /* pairwise ratio: r[0]=x[0], r[i]=x[i]%x[i-1]          */
    QK_WSUM,            /* weighted sum:  x wsum y == sum x*y (nulls excluded)  */
    QK_WAVG,            /* weighted avg:  (sum x*y) % sum x (nulls excluded)    */
    QK_COV,             /* population covariance                                */
    QK_SCOV             /* sample covariance (n-1)                              */
} q_build_kind;

/* One manifest row: a q verb name, its lexical class, and its monadic/dyadic
 * build recipes.  `*_target` is the rayfall env name (QK_ENV) or the canonical
 * lowering name for a wrapper (QK_EQ/NE/TAKE/DROP), or NULL when the kind is
 * QK_NONE.
 *
 * `adverb_hof` (adverb rows only) — the rayfall higher-order function the q
 * adverb IS: `'`(each)->map, `/`(over)->fold, `\`(scan)->scan.  q adverbs are
 * NOT bespoke objects; in 2b `+/` lowers to a projection of rayfall `fold` over
 * `+`.  NULL for non-adverb rows, and for adverbs whose HOF mapping is deferred
 * (each-right/left/prior `/: \: ':` — no clean rayfall equivalent yet). */
typedef struct {
    const char*  name;
    q_lex_class  lex;
    q_build_kind mon_kind;
    const char*  mon_target;
    q_build_kind dyad_kind;
    const char*  dyad_target;
    const char*  adverb_hof;
} q_op_t;

/* The manifest table; sets *n to its length.  Stable storage (static const). */
const q_op_t* q_ops_table(int* n);

/* True iff s[0..len) is a keyword usable as an INFIX verb (i.e. a QLEX_KW_INFIX
 * row).  Replaces the hardcoded memcmp("div") in the scanner.  Static-only: no
 * runtime registry dependency. */
int q_lex_is_kw_infix(const char* s, int len);

/* True iff s[0..len) is a RESERVED q verb name (any manifest row).  q reserves
 * its primitives — `div:5` raises 'assign (ADR 0003 Decision 1).  Static-only. */
int q_ops_is_reserved(const char* s, int len);

#endif /* Q_OPS_H */
