/* src/ops/agg_engine.h — v2 composable aggregation engine entry (design §3). */
#ifndef RAY_OPS_AGG_ENGINE_H
#define RAY_OPS_AGG_ENGINE_H

#include <rayforce.h>
#include "ops/internal.h"   /* ray_graph_t, ray_op_t, ray_op_ext_t, find_ext */

/* Test/feature knob: route OP_GROUP through the v2 engine when it can handle
 * the query (see agg_v2_can_handle). Default false → zero behavioral change. */
extern bool ray_agg_engine_v2;

/* True iff the v2 engine fully supports this group node over this table.
 * Conservative: any uncertainty → false → caller uses the existing engine. */
bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* Precondition: agg_v2_can_handle(g, op, tbl) returned true. */
ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

#endif /* RAY_OPS_AGG_ENGINE_H */
