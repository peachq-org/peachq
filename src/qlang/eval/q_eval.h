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

/* The apply module — THE one dispatch home: valence/rank, projections,
 * native adverbs + monomorphization, family lifts, kernel calls, result
 * construction.  Args BORROWED (C-NULL = elided hole), result OWNED;
 * `row` is fv's manifest row when known, NULL otherwise. */
ray_t* q_eval_apply(ray_t* fv, const struct q_op* row, ray_t** args, int64_t n);

/* Native adverb application (adv: 0=' 1=/ 2=\ 3=': 4=/: 5=\:). */
ray_t* q_eval_apply_adverb(int adv, ray_t* fv, const struct q_op* frow,
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
       Q_EVAL_CAR_COMP = 4 };
ray_t* q_eval_apply_lambda_new(ray_t* params, ray_t** body, int64_t nbody,
                               ray_t* src);
ray_t* q_eval_apply_deriv_new(int adv, ray_t* fv, const struct q_op* frow);
int    q_eval_apply_carrier_kind(const ray_t* v);   /* 0 = not a carrier */
int    q_eval_apply_frame_depth(void);              /* lambda frames live */
ray_t* q_eval_apply_lambda_src(ray_t* v);           /* BORROWED -RAY_STR source,
                                                     * NULL unless a lambda carrier
                                                     * (q_fmt display, q_wire 100h) */

/* Carrier display for q_fmt; returns 1 iff v was rendered as a carrier. */
int q_eval_apply_carrier_fmt(ray_t* v, char* buf, size_t bufsz);

#endif /* Q_EVAL_H */
