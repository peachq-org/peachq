/* q_eval — the fresh q evaluator (eval-rebuild skeleton, spec 2026-07-23):
 * q_parse -> q_eval -> q_fmt; no q_lower, no ray_eval, no base hooks.
 * Opt-in behind Q_EVAL=fresh; the legacy pipeline stays the default.
 * Semantics live in the apply module / values, never in walker control flow
 * (the escape-hatch rule).  Fn values travel as (value, manifest-row) pairs
 * from q_registry_lookup_row / q_registry_row_of — never via provenance. */
#ifndef Q_EVAL_H
#define Q_EVAL_H

#include <rayforce.h>
#include <stdint.h>

struct q_op;

/* true iff Q_EVAL=fresh in the environment (re-read per call: the C-unit
 * harness toggles it between in-process runtimes) */
int q_eval_fresh_enabled(void);

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

/* RAY_QFN carriers (lambda/projection/derived verb): child slots in
 * ray_data, kind in aux[0]; serde/wire refuse them via totality fallbacks. */
enum { Q_EVAL_CAR_LAMBDA = 1, Q_EVAL_CAR_PROJ = 2, Q_EVAL_CAR_DERIV = 3 };
ray_t* q_eval_apply_lambda_new(ray_t* params, ray_t** body, int64_t nbody,
                               ray_t* src);
ray_t* q_eval_apply_deriv_new(int adv, ray_t* fv, const struct q_op* frow);
int    q_eval_apply_carrier_kind(const ray_t* v);   /* 0 = not a carrier */
int    q_eval_apply_frame_depth(void);              /* lambda frames live */

/* Carrier display for q_fmt; returns 1 iff v was rendered as a carrier. */
int q_eval_apply_carrier_fmt(ray_t* v, char* buf, size_t bufsz);

#endif /* Q_EVAL_H */
