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

/* Predicate-shape detection used by the fused operators before emitting a
 * DAG node.  Returns 1 if `expr` (a Rayfall expression, not a DAG node)
 * can be evaluated by the per-morsel predicate evaluator against `tbl`.
 *
 * Accepts single (== col const) / (!= col const) on flat SYM/integer
 * columns, (and pred1 pred2 …) of those, plus ordering comparisons
 * (<, <=, >, >=) on numeric (non-SYM) columns, LIKE on string/SYM
 * columns, and IN over a small integer/temporal value set.
 *
 * Implemented in fused_pred.c.  The only remaining consumer is
 * fused_topk.c (the OP_FILTERED_GROUP fused operator that previously
 * also used it has been retired). */
int ray_fused_group_supported(ray_t* expr, ray_t* tbl);

#endif /* RAY_OPS_FUSED_GROUP_H */
