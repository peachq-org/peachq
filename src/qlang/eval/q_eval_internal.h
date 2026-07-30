/* q_eval_internal — the eval family's own seam (the q_table.h pattern, #361):
 * machinery q_eval_apply.c and q_adverb.c share.  Dispatch itself stays in the
 * apply module; files outside eval/ keep to the public q_eval.h. */
#ifndef Q_EVAL_INTERNAL_H
#define Q_EVAL_INTERNAL_H

#include "qlang/q_registry.h"   /* q_valence_t */
#include <rayforce.h>
#include <stdint.h>

struct q_op;

#define APPLY_MAX_ARGS 60

/* projection carrier ctor: [fv, row box, slot0..slotR-1]; holes = C NULL;
 * args borrowed, result owned */
ray_t* q_eval_apply_proj_new(ray_t* fv, const struct q_op* row, ray_t** args,
                             int64_t n, int64_t rank);

/* native fn VALUE (RAY_UNARY/BINARY/VARY) — narrower than q_eval_apply_is_fn,
 * which also admits carriers and engine lambdas */
int q_eval_apply_is_fnval(ray_t* v);

/* q_list_collapse that CONSUMES l */
ray_t* q_eval_apply_collapse(ray_t* l);

/* a manifest row's registry value at one valence (borrowed) + its row */
ray_t* q_eval_apply_manifest_value(const struct q_op* r, q_valence_t v,
                                   const struct q_op** out);

/* an FNV overload-matrix row (`?` `!` `@` `.`) — its classic reading is the
 * DYAD, so `(!/)x` reduces at rank 2 */
int q_eval_apply_fnv_matrix_row(const struct q_op* r);

/* keyword-HOF row (each/peach/over/scan/prior): adverb_hof -> adv id, -1
 * when the spelling is not a HOF's */
int q_adverb_hof_id(const char* hof);

#endif /* Q_EVAL_INTERNAL_H */
