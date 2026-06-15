/* src/ops/agg_engine.c — v2 aggregation engine. */
#include "ops/agg_engine.h"
#include "ops/agg_registry.h"
#include "ops/ops.h"

bool ray_agg_engine_v2 = false;   /* knob; default off */

bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    (void)g; (void)op; (void)tbl;
    return false;   /* Task 2 fills the real predicate */
}

ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    (void)g; (void)op; (void)tbl;
    return ray_error("nyi", "agg v2: not reachable until gate admits");
}
