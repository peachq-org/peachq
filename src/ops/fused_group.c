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
#include "lang/eval.h"   /* RAY_ATTR_NAME */

#include <string.h>

/* Constructor — mirrors ray_group's trail layout (keys, agg_ops, agg_ins
 * embedded in trailing bytes), and stashes the predicate root in
 * base.inputs[0].  Reusing OP_GROUP's anonymous union variant means
 * exec_filtered_group reads ext->n_keys / ext->keys[] / ext->agg_ops /
 * ext->agg_ins exactly the way OP_GROUP does. */
ray_op_t* ray_filtered_group(ray_graph_t* g, ray_op_t* pred,
                             ray_op_t** keys, uint8_t n_keys,
                             uint16_t* agg_ops, ray_op_t** agg_ins,
                             uint8_t n_aggs)
{
    if (!g || !pred) return NULL;

    uint32_t pred_id = pred->id;
    uint32_t key_ids[256];
    uint32_t agg_ids[256];
    for (uint8_t i = 0; i < n_keys; i++) key_ids[i] = keys[i]->id;
    for (uint8_t i = 0; i < n_aggs; i++) agg_ids[i] = agg_ins[i]->id;

    size_t keys_sz = (size_t)n_keys * sizeof(ray_op_t*);
    size_t ops_sz  = (size_t)n_aggs * sizeof(uint16_t);
    size_t ins_sz  = (size_t)n_aggs * sizeof(ray_op_t*);
    size_t ops_off = keys_sz;
    size_t ins_off = ops_off + ops_sz;
    /* Round ins_off up to pointer alignment */
    ins_off = (ins_off + sizeof(ray_op_t*) - 1) & ~(sizeof(ray_op_t*) - 1);
    ray_op_ext_t* ext = graph_alloc_ext_node_ex(g, ins_off + ins_sz);
    if (!ext) return NULL;

    ext->base.opcode = OP_FILTERED_GROUP;
    ext->base.arity = 1;  /* predicate; keys/aggs live in trail */
    ext->base.out_type = RAY_TABLE;
    ext->base.inputs[0] = &g->nodes[pred_id];
    if (n_keys > 0 && keys[0])
        ext->base.est_rows = g->nodes[key_ids[0]].est_rows / 10;

    char* trail = EXT_TRAIL(ext);
    ext->keys = (ray_op_t**)trail;
    for (uint8_t i = 0; i < n_keys; i++)
        ext->keys[i] = &g->nodes[key_ids[i]];
    ext->agg_ops = (uint16_t*)(trail + ops_off);
    if (ops_sz > 0) memcpy(ext->agg_ops, agg_ops, ops_sz);
    ext->agg_ins = (ray_op_t**)(trail + ins_off);
    for (uint8_t i = 0; i < n_aggs; i++)
        ext->agg_ins[i] = &g->nodes[agg_ids[i]];
    ext->n_keys = n_keys;
    ext->n_aggs = n_aggs;

    g->nodes[ext->base.id] = ext->base;
    return &g->nodes[ext->base.id];
}

/* Phase-1 supported predicate shapes:
 *
 *   pred  = (== col const) | (!= col const)
 *   col   = name reference (-RAY_SYM with RAY_ATTR_NAME)
 *   const = scalar atom literal (any non-name atom)
 *
 * Inspects the Rayfall AST (RAY_LIST), not the DAG.  Caller passes the
 * `where:` value from the select dict. */
int ray_fused_group_supported(ray_t* expr) {
    if (!expr || expr->type != RAY_LIST) return 0;
    if (ray_len(expr) != 3) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (!elems[0] || elems[0]->type != -RAY_SYM) return 0;
    ray_t* op_sym = ray_sym_str(elems[0]->i64);
    if (!op_sym) return 0;
    size_t op_len = ray_str_len(op_sym);
    const char* op = ray_str_ptr(op_sym);
    int is_eq = (op_len == 2 && op[0] == '=' && op[1] == '=');
    int is_ne = (op_len == 2 && op[0] == '!' && op[1] == '=');
    if (!is_eq && !is_ne) return 0;
    ray_t* lhs = elems[1];
    if (!lhs || lhs->type != -RAY_SYM || !(lhs->attrs & RAY_ATTR_NAME))
        return 0;
    ray_t* rhs = elems[2];
    if (!rhs || !ray_is_atom(rhs) || (rhs->attrs & RAY_ATTR_NAME))
        return 0;
    return 1;
}

ray_t* exec_filtered_group(ray_graph_t* g, ray_op_t* op) {
    (void)g; (void)op;
    return ray_error("nyi", "OP_FILTERED_GROUP exec stub");
}
