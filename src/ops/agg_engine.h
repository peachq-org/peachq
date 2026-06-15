/* src/ops/agg_engine.h — v2 composable aggregation engine entry (design §3). */
#ifndef RAY_OPS_AGG_ENGINE_H
#define RAY_OPS_AGG_ENGINE_H

#include <rayforce.h>
#include "ops/internal.h"   /* ray_graph_t, ray_op_t, ray_op_ext_t, find_ext */
#include "ops/agg_acc.h"    /* agg_vtable_t */

/* Test/feature knob: route OP_GROUP through the v2 engine when it can handle
 * the query (see agg_v2_can_handle). Default false → zero behavioral change. */
extern bool ray_agg_engine_v2;

/* True iff the v2 engine fully supports this group node over this table.
 * Conservative: any uncertainty → false → caller uses the existing engine. */
bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* Precondition: agg_v2_can_handle(g, op, tbl) returned true. */
ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* Dense group assignment for a single key column, first-occurrence order. */
typedef struct {
    uint32_t* gids;     /* len = nrows; gid per row */
    int64_t*  keys;     /* len = ngroups; representative int-coded key per group */
    int64_t   ngroups;
} agg_groups_t;

/* Single-key grouping. Reads the key as an int64 (intern id for SYM).
 * Assigns gids incrementally on first sight → gid order == first-occurrence
 * order. Returns 0 on success (caller frees out->gids and out->keys via free()),
 * -1 on allocation failure. */
int agg_group_keys_i(ray_t* key_col, agg_groups_t* out);

/* Build a dense SoA per-group state array for one aggregate (vt), run a single
 * update_batch over val_col grouped by gids, and finalize each group into a
 * typed result column of vt->out_type, length ngroups. For COUNT, val_col is
 * NULL. Caller owns the returned column (ray_release). Returns a ray_error atom
 * on allocation failure. Single-threaded (Phase 1a). */
ray_t* agg_run_one(const agg_vtable_t* vt, ray_t* val_col,
                   const uint32_t* gids, int64_t nrows, int64_t ngroups);

/* Result column name for a plain-column-input aggregate: input column name
 * (ray_sym_str of in_sym) + per-op suffix (_sum/_count/_mean/_min/_max/...),
 * interned.  Falls back to in_sym on buffer overflow.  Behavior-identical to
 * emit_agg_columns' inline naming for the OP_SCAN input case. */
int64_t agg_result_col_name(int64_t in_sym, uint16_t agg_op);

/* Build a result key column of src_key_col's type from int-coded group keys
 * (length n), each written at native width.  SYM adopts the source domain.
 * Caller owns the returned column (ray_release). */
ray_t* agg_build_key_col(ray_t* src_key_col, const int64_t* keys, int64_t n);

#endif /* RAY_OPS_AGG_ENGINE_H */
