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

#ifndef RAY_OPS_FUSED_GROUP_H
#define RAY_OPS_FUSED_GROUP_H

#include "rayforce.h"
#include "ops/internal.h"

/* Construct an OP_FILTERED_GROUP DAG node.
 *
 *   pred       - predicate root (DAG op, must produce RAY_BOOL vec or be
 *                a recognised comparison/AND/LIKE shape).
 *   keys[]     - n_keys group-key DAG ops (each must produce a column).
 *   agg_ops[]  - n_aggs aggregate opcodes (OP_COUNT/OP_SUM/OP_AVG/...).
 *   agg_ins[]  - n_aggs aggregate input DAG ops.
 *
 * Returns NULL on allocation failure or unsupported shape. */
ray_op_t* ray_filtered_group(ray_graph_t* g,
                             ray_op_t* pred,
                             ray_op_t** keys, uint8_t n_keys,
                             uint16_t* agg_ops, ray_op_t** agg_ins,
                             uint8_t n_aggs);

/* Predicate-shape detection used by the planner before emitting the
 * fused op.  Returns 1 if `expr` (a Rayfall expression, not a DAG node)
 * can be evaluated by the per-morsel predicate evaluator against `tbl`.
 *
 * Pass 1 accepted single (== col const) / (!= col const) on flat
 * SYM/integer columns.  Pass 3 adds (and pred1 pred2 …) of those, plus
 * ordering comparisons (<, <=, >, >=) on numeric (non-SYM) columns. */
int ray_fused_group_supported(ray_t* expr, ray_t* tbl);

/* exec.c calls this for OP_FILTERED_GROUP nodes. */
ray_t* exec_filtered_group(ray_graph_t* g, ray_op_t* op);

#endif /* RAY_OPS_FUSED_GROUP_H */
