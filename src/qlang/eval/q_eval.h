/* q_eval — THE q evaluator (eval-rebuild cutover, spec 2026-07-23):
 * q_parse -> q_eval -> q_fmt; no lowering pass, no ray_eval, no base hooks.
 * Semantics live in the apply module / values, never in walker control flow
 * (the escape-hatch rule).  Fn values travel as (value, manifest-row) pairs
 * from q_registry_lookup_row / q_registry_row_of — never via provenance. */
#ifndef Q_EVAL_H
#define Q_EVAL_H

#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>

struct q_op;

/* Walker over the RAW q_parse tree; returns OWNED (RAY_ERROR on failure). */
ray_t* q_eval(ray_t* node);

/* Drop the walker's once-per-runtime fixed-sym-id cache (control-form and
 * adverb spellings).  q_runtime_destroy MUST call this: sym ids are not
 * stable across runtimes, and a stale cache silently un-recognises if/while/do. */
void q_eval_syms_reset(void);

/* The apply module — THE one dispatch home: valence/rank, projections,
 * family lifts, kernel calls, result construction; the native adverb engine
 * it dispatches into lives in q_adverb.c.  Args BORROWED (C-NULL = elided
 * hole), result OWNED; `row` is fv's manifest row when known, NULL otherwise. */
ray_t* q_eval_apply(ray_t* fv, const struct q_op* row, ray_t** args, int64_t n);

/* THE materialization boundary (materialization phase 1, design 2026-07-24):
 * the ONE home for the force operation.  A lazy DAG handle is forced to a
 * concrete value; a concrete value (or error/NULL) passes through untouched.
 * CONSUMES v (args-borrowed/result-owned): a lazy input is released by the
 * force, a concrete input is returned as-is. */
ray_t* q_eval_apply_concrete(ray_t* v);

/* Debug tripwire (design obligation d): the "no RAY_LAZY crosses an observable
 * boundary" invariant, machine-enforced by the ASan gate.  Compiled out (no
 * release cost) unless DEBUG. */
#ifdef DEBUG
void q_eval_apply_assert_concrete(ray_t* v);
#define Q_ASSERT_CONCRETE(v) q_eval_apply_assert_concrete(v)
#else
#define Q_ASSERT_CONCRETE(v) ((void)(v))
#endif

/* Native adverb application (adv: 0=' 1=/ 2=\ 3=': 4=/: 5=\:) — the
 * q_adverb.c engine's one entry. */
ray_t* q_adverb_apply(int adv, ray_t* fv, const struct q_op* frow,
                      ray_t** args, int64_t n);

/* The ONE value-apply entry (eval-tree vs value-object duality): apply an
 * already-EVALUATED head to already-evaluated args — fn values/carriers
 * through q_eval_apply, nouns through the indexing arm (dict/table/vector/
 * handle).  The seam for IPC (func;args) payloads, `.z` handler firing,
 * `@`/`.` dyadic and every C-side "call this q value" site.  Args BORROWED,
 * result OWNED. */
ray_t* q_eval_apply_value(ray_t* head, ray_t** args, int64_t n);

/* `@` / `.` manifest-row entrypoints (q_ops.c): 2 args Apply/Index; 3 args
 * Trap on a callable head, Amend ternary on a data head; 4 args Amend
 * quaternary (machinery: ops/q_index.c; a sym-atom d name-lifts). */
ray_t* q_eval_at_wrap(ray_t** args, int64_t n);
ray_t* q_eval_dot_wrap(ray_t** args, int64_t n);

/* `value`/`get` one-apply body (ref/value.md); q `eval` is .q.eval:(-6!),
 * the q_bang.c internal arm over q_eval (basics/internal.md). */
ray_t* q_eval_value_wrap(ray_t* x);

/* the ONE typed-vector element writer (result construction + amend leaves):
 * 0 on success, -1 when the width isn't reachable (callers splice). */
int q_eval_apply_store_elem(ray_t* vec, int64_t i, ray_t* e);

/* THE truthiness home (owner ruling 2026-07-15): materialize -> exclude
 * float/real -> `"b"$` cast -> boolean ATOM; only 0 is false, nulls are true.
 * CONSUMES v; on failure returns 0 with the owned error in *err. */
int q_eval_apply_truthy(ray_t* v, ray_t** err);

/* True iff v is applicable as a FUNCTION (native fn value or RAY_QFN
 * carrier) — the q_deriv_is_fn_value successor. */
int q_eval_apply_is_fn(ray_t* v);

/* RAY_QFN carriers (lambda/projection/derived verb/composition): child
 * slots in ray_data, kind in aux[0]; serde/wire refuse them via totality
 * fallbacks. */
enum { Q_EVAL_CAR_LAMBDA = 1, Q_EVAL_CAR_PROJ = 2, Q_EVAL_CAR_DERIV = 3,
       Q_EVAL_CAR_COMP = 4, Q_EVAL_CAR_ITER = 5 };
ray_t* q_eval_apply_lambda_new(ray_t* params, ray_t** body, int64_t nbody,
                               ray_t* src);
ray_t* q_eval_apply_deriv_new(int adv, ray_t* fv, const struct q_op* frow);
/* An iterator IS a value (103h, basics/datatypes.md); a derived function is
 * that value applied to its ONE operand.  `_iter_id` reads the adverb id
 * back out (-1 when v is not an iterator). */
ray_t* q_eval_apply_iter_new(int adv);
int    q_eval_apply_iter_id(ray_t* v);
int    q_eval_apply_carrier_kind(const ray_t* v);   /* 0 = not a carrier */
/* the value's arity as dispatch sees it; -1 when it has no fixed rank */
int64_t q_eval_apply_rank(ray_t* v);
int    q_eval_apply_frame_depth(void);              /* lambda frames live */
ray_t* q_eval_apply_lambda_src(ray_t* v);           /* BORROWED -RAY_STR source,
                                                     * NULL unless a lambda carrier
                                                     * (q_fmt display, q_wire 100h) */

/* Carrier read-out for display (q_fmt renders; slot layout stays opaque):
 * BORROWED parts, NULL when v is not that carrier kind — deriv/proj head
 * value + manifest spelling (NULL spelling = value head), composition's
 * inner g, and a projection's slot count + bound arg (NULL arg = hole). */
int         q_eval_apply_deriv_adv(ray_t* v);       /* adv id, -1 not deriv */
const char* q_eval_apply_car_head_name(ray_t* v);
ray_t*      q_eval_apply_car_head(ray_t* v);
ray_t*      q_eval_apply_comp_inner(ray_t* v);
int64_t     q_eval_apply_proj_nslots(ray_t* v);
ray_t*      q_eval_apply_proj_arg(ray_t* v, int64_t i);

#endif /* Q_EVAL_H */
