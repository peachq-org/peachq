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
 * Scope: every rostered q verb, including the type-dispatch glyphs (`! ? $ @ .
 * _`, landed in 2c-2+).  A registry miss still means "not resolvable at that
 * valence."  Cells whose q semantics have NO clean rayfall target stay QR_NONE
 * rather than a guessed binding — currently `|` dyadic (element-wise max),
 * monadic `%` (reciprocal glyph), monadic `$`/`@`/`.`/`^`, and the reserved
 * `any`/`all` rows; qSQL forms are q_lower rewrites, not registry cells. */
#ifndef Q_OPS_H
#define Q_OPS_H

#include <stddef.h>
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
#define QR_FNV(t, f)  { QK_FN,   (t),  (q_wrap_fn_t)(f),   0, 0 }
#define QR_QSRC(t)    { QK_QSRC, (t),  NULL,               0, 0 }

/* One manifest row: a q verb name, its lexical class, and its monadic/dyadic
 * build recipes (QR_* above).
 *
 * `adverb_hof` (adverb rows only) — the rayfall higher-order function the q
 * adverb IS: `'`(each)->map, `/`(over)->fold, `\`(scan)->scan.  q adverbs are
 * NOT bespoke objects; in 2b `+/` lowers to a projection of rayfall `fold` over
 * `+`.  NULL for non-adverb rows, and for adverbs whose HOF mapping is deferred
 * (each-prior `':` — no clean rayfall equivalent yet).
 *
 * `deterministic` / `sideeffect` — introspection metadata (feat/q-ops-
 * introspection), exposed verbatim as the like-named `.Q.ops[]` columns.
 * Every row carries both explicitly (the build's -Wmissing-field-initializers
 * forbids relying on a zero default); the AUDIT block in q_ops.c is the roster
 * of the non-default rows and the rationale:
 *   deterministic = 0 iff any valence's result is nondeterministic (rand /
 *                   deal / `?`-roll); 1 otherwise.
 *   sideeffect    = 1 iff evaluation performs an observable side effect (global
 *                   assignment, table mutation, IPC, `system`, console/file
 *                   I/O); 0 otherwise.
 * Both flags stay pure metadata — no verb behaviour depends on them.
 *
 * `family` — the verb's structure-dispatch family (actionable-plans/
 * 2026-07-15-uniform-structure-dispatch.md §2/§3), doc-verified per verb
 * against qdocs at classification time and exposed as the `.Q.ops[]` `family`
 * column.  PURE METADATA in this stage: nothing dispatches on it yet (stage 1
 * of the spec is the first consumer).  Vocabulary (the FAMILY AUDIT block in
 * q_ops.c is the classification roster + border rulings):
 *   atomic     L1: broadcast elementwise, recursing through structure
 *   map        L2: uniform, length-preserving; keys/columns preserved
 *   aggregate  L3: reducing; structure discarded (agg d = agg value d)
 *   index      L4: index-lift (op t = t @ op til count t; dicts on entries)
 *   rowid      row-identity: needs row equality/ordering (engine kernels)
 *   structural defines/inspects the structures themselves (bespoke by nature)
 *   irregular  documented behaviour deviates from its family's law (catalogue)
 *   none       no structure semantics (I/O, iterators, apply, codecs, RNG)
 * A row carries ONE family (the deterministic-flag precedent); when valences
 * diverge, it reflects the valence the structure-dispatch spec consumes, and
 * the divergence is noted at the row. */
typedef struct {
    const char*  name;
    q_lex_class  lex;
    q_recipe_t   mon;           /* monadic build recipe */
    q_recipe_t   dyad;          /* dyadic build recipe  */
    const char*  adverb_hof;
    uint8_t      deterministic; /* 0 = nondeterministic result (see above) */
    uint8_t      sideeffect;    /* 1 = performs an observable side effect  */
    const char*  family;        /* structure-dispatch family (see above)   */
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
