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
    QK_SCOV,            /* sample covariance (n-1)                              */
    /* ---- Wave 5 sliding m-windows + ema (dyadic infix) ---- */
    QK_MSUM,            /* N msum x  — sliding sum (nulls -> 0)                 */
    QK_MAVG,            /* N mavg x  — sliding average (nulls excluded)        */
    QK_MMAX,            /* N mmax x  — sliding max (nulls skipped)             */
    QK_MMIN,            /* N mmin x  — sliding min (nulls skipped)             */
    QK_MCOUNT,          /* N mcount x— sliding count of non-null               */
    QK_MDEV,            /* N mdev x  — sliding population std deviation         */
    QK_EMA,             /* a ema x   — exponential moving average               */
    /* ---- list verbs (feat/q-list-verbs) ---- */
    QK_FILL,            /* q `x^y`   — fill nulls in y with x (atom/vector)     */
    QK_ROTATE,          /* q `n rotate x` — cyclic shift left (neg = right)     */
    QK_SUBLIST,         /* q `n sublist x` — first/last n, or i j slice         */
    QK_NEXT,            /* q `next x` — shift left, null-fill vacated tail       */
    QK_PREV,            /* q `prev x` — shift right, null-fill vacated head      */
    /* ---- IPC client verbs (feat/q-ipc-client, Phase D) ---- */
    QK_HOPEN,           /* q `hopen y` — normalize int|string|(conn;timeout) into
                         * the `.ipc.open` "host:port[:user:password]" string form
                         * and connect.  Returns a 1-BASED q handle (raw poll id + 1):
                         * kdb reserves 0 (console) and signs the handle for async,
                         * but openq's raw selector ids START at 0, so the q layer
                         * offsets by 1 and translates back at every handle op.      */
    QK_HCLOSE,          /* q `hclose h` — translate the 1-based q handle back to the
                         * raw poll id and route to `.ipc.close`.                    */
    /* ---- null test (feat/q-atomic-extend) ---- */
    QK_NULL,            /* q `null x` — atomic nil? (broadcasts over vectors +
                           nested lists at every depth), homogeneous top-level
                           bool-atom run collapsed to a bool vector             */
    /* ---- string search family (feat/q-string-fns) ---- */
    QK_LIKE,            /* q `x like p` — glob match sym/str atom|vector -> bool
                         * (reuses ray_like_fn); dict x -> bool vals, rebuilt.   */
    QK_SS,              /* q `s ss p` — string search: 0-based start indices of
                         * every (overlapping) fixed-width glob match of p in s. */
    QK_SSR,             /* q `ssr[s;p;r]` — search-and-replace every match of p
                         * in s with r (a string, or a fn applied to each match) */
    /* ---- set operations (feat/q-setops) ---- */
    QK_UNION,           /* q `x union y` == `distinct x,y` (ref/union.md) —
                         * wrapper because rayfall union KEEPS x-duplicates
                         * (only dedups y against x); q dedups the whole join. */
    QK_INTER,           /* q `x inter y` — items of x in y, x-dups kept
                         * (ref/inter.md).  rayfall `sect` IS this for lists,
                         * but returns a WRONG-shaped dict for dict operands
                         * (kdb returns the common VALUES as a list), so the
                         * wrapper guards dict/table operands with 'nyi and
                         * delegates lists to ray_sect_fn. */
    QK_CROSS,           /* q `x cross y` == {raze x,/:\:y} (ref/cross.md) —
                         * Cartesian product wrapper composing item access +
                         * q join (rayfall concat); no cartesian primitive in
                         * rayfall.  String (char-items) and dict/table cross
                         * are deferred cells ('nyi). */
    /* ---- atomic unary math (feat/q-math-atomic) — implement-via-libm, no
     * rayfall counterpart (rayfall has exp/log/sqrt but not the trig set).
     * Each wrapper handles ONE atom; RAY_FN_ATOMIC broadcasts over vectors and
     * nested lists (like q_sqrt/floor).  Float results route through make_f64,
     * so the sentinel-null float model applies: null in -> null out, and any
     * non-finite (NaN/±Inf) canonicalizes to 0n (so `sin 1%0` -> 0n). ---- */
    QK_SIN,             /* q `sin x`  — sine (radians), f64                     */
    QK_COS,             /* q `cos x`  — cosine (radians), f64                   */
    QK_TAN,             /* q `tan x`  — tangent (radians), f64                  */
    QK_ASIN,            /* q `asin x` — arcsine (radians), f64; |x|>1 -> 0n     */
    QK_ACOS,            /* q `acos x` — arccosine (radians), f64; |x|>1 -> 0n   */
    QK_ATAN,            /* q `atan x` — arctangent (radians), f64               */
    QK_RECIPROCAL,      /* q `reciprocal x` — 1%x as float (recip 0 -> 0n under
                         * the single-null model; kdb's 0w is a deferred cell)  */
    QK_SIGNUM,          /* q `signum x` — sign as INT (i32): null|neg -> -1i,
                         * zero -> 0i, positive -> 1i                           */
    QK_CEILING,         /* q `ceiling x` — least integer >= x, as LONG (mirrors
                         * QK_FLOOR: float->i64, int passes through)            */
    /* ---- sort / bucket family (feat/q-sort-rank) ---- */
    QK_XBAR,            /* q `width xbar list` — interval bucketing.  ARG-SWAP
                         * wrapper: rayfall ray_xbar_fn is (col, bucket) but q
                         * spells it (bucket, col), so the wrapper flips.
                         * Numeric (int/float bucket) + temporal cols reuse the
                         * base kernel; dict/keyed-table/qSQL forms deferred. */
    QK_ASC,            /* q `asc x` — ascending sort.  Flat vectors reuse
                         * ray_asc_fn (VALUE is kdb-true, but rayfall has no
                         * sorted `s#` attribute so attribute display is a
                         * deferred divergence); a DICT arm sorts by value.
                         * mixed-list-by-type / table / keyed-table deferred. */
    QK_DESC,           /* q `desc x` — descending sort (mirror of QK_ASC; desc
                         * carries no `s#` attribute in kdb either, so flat
                         * vectors are exactly kdb-true).  DICT arm sorts by
                         * value descending. */
    QK_IASC,           /* q `iasc x` / monadic `<` — grade up.  Flat vectors
                         * reuse ray_iasc_fn; a DICT arm returns the keys in
                         * ascending-value order. */
    QK_IDESC,          /* q `idesc x` / monadic `>` — grade down (mirror). */
    /* ---- table verbs (feat/q-table-verbs) ---- */
    QK_FLIP,            /* q `flip x` / monadic `+` — transpose: table<->dict,
                         * list-of-lists transpose (atom broadcast).  Keyed
                         * tables and atoms are 'rank cells. */
    QK_KEYS,            /* q `keys x` — primary key column names (empty sym
                         * vector when unkeyed); table by value or by name.   */
    QK_XKEY,            /* q `x xkey y` — set key columns: reorder x first,
                         * enkey count x (reuses q_enkey); by-name rebinds.   */
    QK_XCOL,            /* q `x xcol y` — rename columns (sym vector renames
                         * the first n; dict / all-key keyed map renames
                         * selected; unknown name -> 'length).                */
    QK_XCOLS,           /* q `x xcols y` — reorder columns, x first.          */
    QK_XASC,            /* q `x xasc y` — sort table ascending by columns.
                         * ARG-SWAP over base ray_xasc_fn (QK_XBAR precedent);
                         * by-name sorts the global in place.  No `s#`
                         * attribute (deferred divergence, like QK_ASC).      */
    QK_XDESC,           /* q `x xdesc y` — descending mirror of QK_XASC.      */
    QK_XGROUP,          /* q `x xgroup y` — key by x; remaining columns become
                         * per-group nested lists (first-occurrence order).   */
    QK_UNGROUP,         /* q `ungroup x` — inverse: explode nested list
                         * columns, repeating simple cells ('length on ragged
                         * rows).  Keyed tables flatten first.                */
    QK_INSERT,          /* q `x insert y` — append rows to the NAMED global
                         * table (kdb insert is by reference); unbound name +
                         * table payload creates it; keyed key collision ->
                         * 'insert; returns inserted row indices.             */
    QK_UPSERT,          /* q `x upsert y` — append / keyed update-or-append;
                         * value target returns the table, named target
                         * rebinds and returns the name.                      */
    QK_EXCEPT           /* q `x except y` — table rows of x not in y (row
                         * membership); non-table operands delegate to base
                         * ray_except_fn (the pre-wave QK_ENV pass-through).  */
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
