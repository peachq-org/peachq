/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "ops/fused_group.h"

/* Phase 0 stub — phase 1 fills in the real implementation, and the planner
 * doesn't emit OP_FILTERED_GROUP until ray_fused_group_supported returns 1. */
ray_op_t* ray_filtered_group(ray_graph_t* g, ray_op_t* pred,
                             ray_op_t** keys, uint8_t n_keys,
                             uint16_t* agg_ops, ray_op_t** agg_ins,
                             uint8_t n_aggs)
{
    (void)g; (void)pred; (void)keys; (void)n_keys;
    (void)agg_ops; (void)agg_ins; (void)n_aggs;
    return NULL;
}

int ray_fused_group_supported(ray_t* expr) {
    (void)expr;
    return 0;
}

ray_t* exec_filtered_group(ray_graph_t* g, ray_op_t* op) {
    (void)g; (void)op;
    return ray_error("nyi", "OP_FILTERED_GROUP exec stub");
}
