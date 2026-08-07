/* q op manifest — the SINGLE authoritative table of every C-rostered q verb.
 *
 * One source, two consumers (spec 2026-07-03-q-op-registry-complete-design.md,
 * "Bootstrap — split lexical metadata from runtime values"):
 *   1. the LEXER (src/qlang/q_parse.c) — static classification, zero runtime
 *      dependency (the scanner runs before eval), via q_lex_is_kw_infix();
 *   2. the REGISTRY builder (src/qlang/q_registry.c) — builds the runtime
 *      (name, valence) -> ray_t function value + q-surface provenance, by
 *      iterating q_ops_table().
 *
 * Scope: every C-rostered q verb, including the type-dispatch glyphs (`! ? $ @ .
 * _`, landed in 2c-2+).  NOT every q KEYWORD: q.q-hosted keywords with no lexer
 * or registry involvement (rand, med, md5, ...) have no row — they resolve as
 * `.q` entries (q_eval's bare-name `.q.<name>` fallback).
 * A registry miss still means "not resolvable at that valence."  Cells whose
 * q semantics have NO clean rayfall target stay QR_NONE rather than a guessed
 * binding — currently the reserved `any`/`all` rows and monadic `:`; qSQL
 * statement forms are parse shapes awaiting the plan-router wave, not
 * registry cells.  A GLYPH row's monadic cell is its k UNARY FORM
 * (basics/exposed-infrastructure.md) and is OPERATIVE at one argument: see the
 * unary-form rule in q_eval_apply.c. */
#ifndef Q_OPS_H
#define Q_OPS_H

#include <stddef.h>
#include "rayforce.h"   /* ray_t (q_ops_acc_identity) */
#include <stdint.h>

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

/* How the registry builds the value for one (row, valence).  Three kinds only
 * (the per-verb QK_* members were retired 2026-07-14 — rows now carry the
 * wrapper FUNCTION POINTER directly, so adding a common verb is one manifest
 * row + one wrapper body + one decl, with no enum member or switch case): */
typedef enum {
    QK_NONE = 0,        /* no value at this valence (deferred / not applicable) */
    QK_ENV,             /* reuse env builtin named `target` (pass-through/rename)*/
    QK_FN,              /* bespoke q-semantics wrapper: bind the row's fn per
                         * its arity/atomic fields (see q_recipe_t)             */
    QK_QSRC             /* value is the q.q definition `.q.<target>`: skipped at
                         * registry init (q.q loads after), installed by the
                         * post-bootstrap q_registry_bind_qsrc pass             */
} q_build_kind;

/* Wrapper-fn carrier for the manifest rows.  A generic function-pointer type
 * so this header stays free of runtime types (ray_t) for the lexer's sake;
 * q_registry.c casts back to the exact ray_{unary,binary,vary}_fn signature
 * selected by `arity` before binding (round-tripping a function pointer
 * through another function-pointer type is well-defined C). */
typedef void (*q_wrap_fn_t)(void);

/* One (row, valence) build recipe.  `target` is the rayfall env name (QK_ENV)
 * or the canonical rayfall LOWERING name for a wrapper (QK_FN) — the aux-name
 * the compiler/query DAG route on (RAY_FN_Q_LOWER); NULL when QK_NONE. */
typedef struct {
    q_build_kind kind;
    const char*  target;
    q_wrap_fn_t  fn;      /* QK_FN: the wrapper body (decl: q_registry_internal.h) */
    uint8_t      arity;   /* QK_FN: 1 = ray_unary_fn, 2 = ray_binary_fn, 0 = vary  */
    uint8_t      atomic;  /* QK_FN: build with RAY_FN_ATOMIC (eval auto-broadcast) */
} q_recipe_t;

/* Row-recipe constructors — the manifest's whole build vocabulary.
 * A = RAY_FN_ATOMIC (eval broadcasts over vectors/structures; wrappers
 * without it drive their own iteration or are shape-changing). */
#define QR_NONE       { QK_NONE, NULL, NULL,               0, 0 }
#define QR_ENV(t)     { QK_ENV,  (t),  NULL,               0, 0 }
#define QR_FN1(t, f)  { QK_FN,   (t),  (q_wrap_fn_t)(f),   1, 0 }
#define QR_FN1A(t, f) { QK_FN,   (t),  (q_wrap_fn_t)(f),   1, 1 }
#define QR_FN2(t, f)  { QK_FN,   (t),  (q_wrap_fn_t)(f),   2, 0 }
#define QR_FN2A(t, f) { QK_FN,   (t),  (q_wrap_fn_t)(f),   2, 1 }
/* FNV = FN at Vary arity (ray_vary_fn, args+n), NOT the FNV hash: one entry
 * point per overloaded glyph (`?` `!` `@` `.`) dispatching on call rank */
#define QR_FNV(t, f)  { QK_FN,   (t),  (q_wrap_fn_t)(f),   0, 0 }
#define QR_QSRC(t)    { QK_QSRC, (t),  NULL,               0, 0 }

/* One manifest row: a q verb, its lexical class, its monadic/dyadic build recipes
 * (QR_* above), and the introspection metadata surfaced verbatim as the `.Q.ops[]`
 * columns.  deterministic/sideeffect stay PURE METADATA; family, mono and the
 * keyword-row adverb_hof became OPERATIVE with the eval-rebuild skeleton — the
 * apply module (src/qlang/eval/) dispatches its family lifts, `/`-
 * monomorphization and native HOF routing on them (legacy eval still reads
 * none).  Every row sets all fields explicitly (-Wmissing-field-initializers).
 * Classification rosters + border rulings live in q_ops.c (the deterministic/
 * sideeffect AUDIT and FAMILY AUDIT blocks); the `family` vocabulary is
 * atomic|map|aggregate|index|rowid|structural|irregular|none (defs:
 * actionable-plans/2026-07-15-uniform-structure-dispatch.md).  Per-verb help
 * strings live OUTSIDE the binary in docs/q-ops-help.tsv (archival, keyed by
 * name, loaded by nothing). */
/* Rank-2 sub-law of an `aggregate` row — what the L3 lift does with a NESTED
 * argument (a dict's rank-2 values, or a plain nested list).  Zero-default is
 * "no rank-2 law": first/last reduce to an ITEM, not a column (ref/first.md). */
typedef enum {
    QNEST_NONE = 0,
    QNEST_FOLD,     /* reduce the OUTER axis with the atomic dyad this row is the
                     * `mono` of, so nulls propagate through the dyad rather than
                     * being re-implemented.  Exactly basics/math.md's roster
                     * `avg min max sum` — "for nested x these functions preserve
                     * the nulls" (`max (1 2;0N 4)` is `1 4`, `min` is `0N 2`) */
    QNEST_MEAN,     /* QNEST_FOLD over `+`, scaled by the outer count — ref/avg.md
                     * "If x is a nested list, null items make the average null" */
    QNEST_COLUMNS   /* agg each flip x (on a DYAD, BOTH args' columns zipped):
                     * the 4.1t "traverse columns of tables and general/anymap/
                     * nested lists" rule (ref/dev.md, ref/var.md, ref/avg.md) */
} q_nested_law;

typedef struct q_op {
    const char*  name;          /* q surface spelling — the lookup key + registry provenance */
    q_lex_class  lex;           /* how the scanner treats the token (QLEX_* above) */
    q_recipe_t   mon;           /* monadic build recipe (QR_NONE = no value at this valence) */
    q_recipe_t   dyad;          /* dyadic  build recipe */
    const char*  adverb_hof;    /* adverb rows: the rayfall HOF the adverb IS (' map, / fold, \ scan).
                                 * Keyword-HOF rows (each/peach/over/scan/prior) also set it so the
                                 * evaluator routes them to native adverb arms; else NULL */
    uint8_t      deterministic; /* 0 iff any valence is nondeterministic (rand/deal/?-roll); else 1 */
    uint8_t      sideeffect;    /* 1 iff eval has an observable effect (assign/mutate/IPC/system/I/O); else 0 */
    const char*  family;        /* structure-dispatch family (see above); consumed by the apply module */
    const char*  mono;          /* dyad rows: keyword whose kernel a `/`-derivation monomorphizes to
                                 * (`+`->sum `*`->prd `|`->max `&`->min `,`->raze); else NULL */
    const char*  mono_scan;     /* the same for a `\`-derivation (`+`->sums `&`->mins).  Its OWN
                                 * column, never `mono`+"s": `,` reduces to raze and scans to
                                 * nothing, so the spelling rule would name a verb that does not
                                 * exist.  Zero-default; only rows with a running twin set it. */
    uint8_t      name_lift;     /* 1: a sym-atom first arg in this row's amend forms NAMES a global —
                                 * the apply seam resolves it, runs the pure form, rebinds, returns
                                 * the sym (ref/amend.md handle d).  Flag-gated, never sym-sniffed:
                                 * sym atoms are legal data.  Trailing field, zero-default: only
                                 * lifted rows set it (designated init; insert/upsert adopt later). */
    uint8_t      nested;        /* aggregate rows: the QNEST_* rank-2 sub-law above.  Trailing
                                 * field, zero-default (designated init). */
    int8_t       kdb_op1;       /* kdb primitive code PLUS ONE (0 = none) — set via
                                 * QKOP (q_ops.c), read via q_ops_kdb_op below. */
} q_op_t;

/* The row's kdb primitive code — the 101h/102h wire byte and `value +` -> 1 —
 * or -1 when the verb has none.  The numbering is KX's FROZEN historical
 * constant (javakdb c.java operator tables); openq never allocates into it. */
static inline int q_ops_kdb_op(const struct q_op* row) { return (int)row->kdb_op1 - 1; }

/* The manifest table; sets *n to its length.  Stable storage (static const). */
const q_op_t* q_ops_table(int* n);

/* Accumulator identity element I for a verb spelling (ref/accumulators.md:
 * unary-seed + empty-Over) — manifest-owned metadata.  Owned result; NULL
 * when q knows none (callers fall back to the doc's generic-() rule). */
ray_t* q_ops_acc_identity(const char* spelling);

/* The manifest row named s[0..len), or NULL if the name is not a C-rostered
 * verb (q.q-hosted keywords have no row).  The one lookup into Q_OPS[] by
 * spelling (rule 3: never an env lookup by q spelling). */
const q_op_t* q_ops_find(const char* s, int len);

/* The atomic-dyad row that folds `row`'s outer axis under QNEST_FOLD/QNEST_MEAN
 * — the `mono` column read BACKWARDS (`sum`->`+`, `max`->`|`), so the sub-law
 * derives from the manifest and needs no roster.  NULL for the other laws. */
const q_op_t* q_ops_nested_dyad(const q_op_t* row);

/* True iff s[0..len) is a keyword usable as an INFIX verb (i.e. a QLEX_KW_INFIX
 * row).  Replaces the hardcoded memcmp("div") in the scanner.  Static-only: no
 * runtime registry dependency. */
int q_lex_is_kw_infix(const char* s, int len);

/* True iff s[0..len) is a RESERVED q verb name (any manifest row).  q reserves
 * its primitives — `div:5` raises 'assign (ADR 0003 Decision 1).  Static-only. */
int q_ops_is_reserved(const char* s, int len);

#endif /* Q_OPS_H */
