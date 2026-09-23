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

/* view carrier ctor/slots (q_view.c owns the semantics): slots [body tree,
 * deps symvec, text charv, cached|NULL]; pending/in-recalc flags in aux[1] */
ray_t*  q_eval_apply_view_new(ray_t* body, ray_t* deps, ray_t* text);
ray_t** q_eval_apply_view_slots(ray_t* v);   /* NULL unless a view carrier */

/* the walker's tree-classification helpers (lam_scan + view dep scan):
 * sym-vector membership, embedded fn VALUE node, control-form spelling */
int q_eval_symvec_has(ray_t* v, int64_t id);
int q_eval_fn_value(ray_t* x);
int q_eval_ctl_sym(int64_t id);
/* a colon assignment's SPELLING lock — q_env_set judges only the re-rooted target */
int q_eval_assign_locked(int64_t sym);
/* the parse-time locals of a lambda body, params excluded — OWNED symvec (ref/value.md `## Lambda`); built ONCE
 * per lambda value by q_eval_apply_lambda_new, read back BORROWED (NULL unless a lambda carrier) */
ray_t* q_eval_lambda_locals(ray_t* params, ray_t* body);
ray_t* q_eval_apply_lambda_locals(ray_t* v);

/* a manifest row's registry value at one valence (borrowed) + its row */
ray_t* q_eval_apply_manifest_value(const struct q_op* r, q_valence_t v,
                                   const struct q_op** out);

/* the dict-pair law every dyadic lift distributes by: x's keys then y's new keys, aligned through the index
 * home's one Find; `f` runs once per aligned GROUP (two gathered collections, or one against the row's fill
 * identity where it has one — a key on one side only passes through otherwise).  `f` borrows and returns an owned
 * value; x, y borrowed; result owned.  A keyed table is a dict here. */
typedef ray_t* (*q_eval_zip_fn)(void* ctx, ray_t* x, ray_t* y);
ray_t* q_eval_apply_dict_pair(const struct q_op* row, ray_t* x, ray_t* y,
                              q_eval_zip_fn f, void* ctx);

/* an FNV overload-matrix row (`?` `!` `@` `.`) — its classic reading is the
 * DYAD, so `(!/)x` reduces at rank 2 */
int q_eval_apply_fnv_matrix_row(const struct q_op* r);

/* keyword-HOF row (each/peach/over/scan/prior): adverb_hof -> adv id, -1
 * when the spelling is not a HOF's */
int q_adverb_hof_id(const char* hof);

#endif /* Q_EVAL_INTERNAL_H */
