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

/* ─────────────────────────────────────────────────────────────────────────
 * Per-morsel predicate evaluator
 *
 * Phase 1 only handles a single comparison `(== col const)` / `(!= col const)`
 * against an SYM or numeric column.  The compiled state is built once at
 * exec entry (column resolution + constant decode) and reused for every
 * morsel.  fp_eval_cmp writes 0/1 into bits[0..n) for the corresponding
 * row range; OP_NE flips the result via XOR.
 * ──────────────────────────────────────────────────────────────────────── */

typedef enum { FP_EQ = 0, FP_NE = 1 } fp_op_t;

typedef struct {
    fp_op_t      op;
    int8_t       col_type;
    uint8_t      col_attrs;
    uint8_t      col_esz;       /* element size in bytes */
    const void*  col_base;
    int64_t      col_len;
    int64_t      cval;          /* I64-encoded constant; for SYM the dict ID */
    int          cval_in_dict;  /* SYM only: 0 = const not in dict (folded) */
} fp_cmp_t;

/* Fill bits[0..end-start) with 0/1 based on `cmp` over rows [start, end). */
static void fp_eval_cmp(const fp_cmp_t* p, int64_t start, int64_t end,
                        uint8_t* bits)
{
    uint8_t invert = (p->op == FP_NE) ? 1 : 0;
    int64_t n = end - start;

    /* SYM low-card fold: const not in dict ⇒ EQ all-zero / NE all-one. */
    if (p->col_type == RAY_SYM && !p->cval_in_dict) {
        memset(bits, invert, (size_t)n);
        return;
    }

    int64_t cval = p->cval;
    switch (p->col_esz) {
    case 1: {
        const uint8_t* d = (const uint8_t*)p->col_base + start;
        uint8_t t = (uint8_t)cval;
        for (int64_t r = 0; r < n; r++) bits[r] = (uint8_t)((d[r] == t) ^ invert);
        break;
    }
    case 2: {
        const uint16_t* d = (const uint16_t*)p->col_base + start;
        uint16_t t = (uint16_t)cval;
        for (int64_t r = 0; r < n; r++) bits[r] = (uint8_t)((d[r] == t) ^ invert);
        break;
    }
    case 4: {
        const uint32_t* d = (const uint32_t*)p->col_base + start;
        uint32_t t = (uint32_t)cval;
        for (int64_t r = 0; r < n; r++) bits[r] = (uint8_t)((d[r] == t) ^ invert);
        break;
    }
    default: { /* 8 */
        const int64_t* d = (const int64_t*)p->col_base + start;
        for (int64_t r = 0; r < n; r++) bits[r] = (uint8_t)((d[r] == cval) ^ invert);
        break;
    }
    }
}

/* Compile predicate DAG node (OP_EQ / OP_NE with OP_SCAN lhs and OP_CONST
 * rhs) against `tbl`.  Returns 0 on success, -1 if the shape can't be
 * handled (caller should bail and let the unfused path run). */
static int fp_compile_cmp(ray_graph_t* g, ray_op_t* pred_op, ray_t* tbl,
                          fp_cmp_t* out)
{
    if (!pred_op || pred_op->arity != 2) return -1;
    if (pred_op->opcode != OP_EQ && pred_op->opcode != OP_NE) return -1;
    out->op = (pred_op->opcode == OP_EQ) ? FP_EQ : FP_NE;

    ray_op_t* lhs = pred_op->inputs[0];
    ray_op_t* rhs = pred_op->inputs[1];
    if (!lhs || !rhs) return -1;
    if (lhs->opcode != OP_SCAN || rhs->opcode != OP_CONST) return -1;

    ray_op_ext_t* lext = find_ext(g, lhs->id);
    ray_op_ext_t* rext = find_ext(g, rhs->id);
    if (!lext || !rext || !rext->literal) return -1;

    ray_t* col = ray_table_get_col(tbl, lext->sym);
    if (!col) return -1;
    /* Phase 1: bail on parted / mapcommon.  The unfused path handles them. */
    if (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON) return -1;

    out->col_type  = col->type;
    out->col_attrs = col->attrs;
    out->col_esz   = ray_sym_elem_size(col->type, col->attrs);
    out->col_base  = ray_data(col);
    out->col_len   = col->len;

    ray_t* cv = rext->literal;
    if (out->col_type == RAY_SYM) {
        if (cv->type == -RAY_SYM) {
            out->cval = cv->i64;
            out->cval_in_dict = 1;
        } else if (cv->type == -RAY_STR) {
            int64_t did = ray_sym_find(ray_str_ptr(cv), ray_str_len(cv));
            out->cval = (did >= 0) ? did : 0;
            out->cval_in_dict = (did >= 0) ? 1 : 0;
        } else {
            return -1;
        }
    } else {
        /* Numeric / temporal: decode the atom into an int64 via the same
         * type-aware reader used elsewhere in the engine. */
        switch (cv->type) {
        case -RAY_I64:       case -RAY_TIMESTAMP: out->cval = cv->i64;            break;
        case -RAY_I32:       case -RAY_DATE:
        case -RAY_TIME:                            out->cval = (int64_t)cv->i32;  break;
        case -RAY_I16:                             out->cval = (int64_t)cv->i16;  break;
        case -RAY_BOOL:      case -RAY_U8:         out->cval = (int64_t)cv->b8;   break;
        default: return -1;
        }
        out->cval_in_dict = 1;
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Phase-1 serial fused exec: single key + single OP_COUNT.
 *
 * Iterates over the source column in RAY_MORSEL_ELEMS-sized chunks,
 * evaluates the predicate into a stack-resident bits[] array, and updates
 * a single-shard linear-probe hash table keyed by the group column value.
 * Materialises the result as a 2-column TABLE.
 * ──────────────────────────────────────────────────────────────────────── */
ray_t* exec_filtered_group(ray_graph_t* g, ray_op_t* op) {
    if (!g || !op) return ray_error("nyi", NULL);
    ray_t* tbl = g->table;
    if (!tbl || tbl->type != RAY_TABLE) return ray_error("type", NULL);

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext || ext->n_keys != 1 || ext->n_aggs != 1)
        return ray_error("nyi", "fused_group: phase-1 single-key + single-agg only");
    if (ext->agg_ops[0] != OP_COUNT)
        return ray_error("nyi", "fused_group: phase-1 only OP_COUNT");

    ray_op_t* pred_op = ext->base.inputs[0];
    fp_cmp_t fp;
    if (fp_compile_cmp(g, pred_op, tbl, &fp) != 0)
        return ray_error("nyi", "fused_group: predicate shape unsupported");

    /* Resolve key column.  Phase 1: must be an OP_SCAN on a non-parted col. */
    ray_op_t* key_op = ext->keys[0];
    if (!key_op || key_op->opcode != OP_SCAN)
        return ray_error("nyi", "fused_group: phase-1 needs SCAN key");
    ray_op_ext_t* kext = find_ext(g, key_op->id);
    if (!kext) return ray_error("nyi", NULL);
    ray_t* key_col = ray_table_get_col(tbl, kext->sym);
    if (!key_col) return ray_error("schema", NULL);
    if (RAY_IS_PARTED(key_col->type) || key_col->type == RAY_MAPCOMMON)
        return ray_error("nyi", "fused_group: phase-1 needs flat key column");

    int64_t nrows = key_col->len;
    int8_t  kt    = key_col->type;
    uint8_t kesz  = ray_sym_elem_size(kt, key_col->attrs);
    const void* kbase = ray_data(key_col);

    /* Single-shard hash table.  Cap = next power of two ≥ 2 × nrows, capped
     * at 1<<20.  slots[s*2] is occupied flag; slots[s*2+1] is the key. */
    uint64_t cap = 1024;
    while (cap < (uint64_t)nrows && cap < (1ULL << 20)) cap <<= 1;
    uint64_t mask = cap - 1;

    ray_t* k_hdr = NULL;
    ray_t* c_hdr = NULL;
    int64_t* slots  = (int64_t*)scratch_calloc(&k_hdr, (size_t)cap * 2 * sizeof(int64_t));
    int64_t* counts = (int64_t*)scratch_calloc(&c_hdr, (size_t)cap * sizeof(int64_t));
    if (!slots || !counts) {
        if (k_hdr) scratch_free(k_hdr);
        if (c_hdr) scratch_free(c_hdr);
        return ray_error("oom", NULL);
    }

    for (int64_t row = 0; row < nrows; ) {
        int64_t end = row + RAY_MORSEL_ELEMS;
        if (end > nrows) end = nrows;
        int64_t mlen = end - row;

        uint8_t bits[RAY_MORSEL_ELEMS];
        fp_eval_cmp(&fp, row, end, bits);

        for (int64_t r = 0; r < mlen; r++) {
            if (!bits[r]) continue;
            int64_t kv = read_by_esz(kbase, row + r, kesz);
            uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
            h ^= h >> 33;
            uint64_t s = h & mask;
            for (;;) {
                if (!slots[s * 2]) {
                    slots[s * 2]     = 1;
                    slots[s * 2 + 1] = kv;
                    counts[s]        = 1;
                    break;
                }
                if (slots[s * 2 + 1] == kv) { counts[s]++; break; }
                s = (s + 1) & mask;
            }
        }
        row = end;
    }

    /* Gather occupied slots into typed output columns. */
    int64_t n_groups = 0;
    for (uint64_t s = 0; s < cap; s++) if (slots[s * 2]) n_groups++;

    ray_t* k_out = (kt == RAY_SYM)
                 ? ray_sym_vec_new(key_col->attrs & RAY_SYM_W_MASK, n_groups)
                 : ray_vec_new(kt, n_groups);
    ray_t* c_out = ray_vec_new(RAY_I64, n_groups);
    if (!k_out || !c_out || RAY_IS_ERR(k_out) || RAY_IS_ERR(c_out)) {
        if (k_out && !RAY_IS_ERR(k_out)) ray_release(k_out);
        if (c_out && !RAY_IS_ERR(c_out)) ray_release(c_out);
        scratch_free(k_hdr); scratch_free(c_hdr);
        return ray_error("oom", NULL);
    }
    k_out->len = n_groups;
    c_out->len = n_groups;

    void*    k_dst = ray_data(k_out);
    int64_t* c_dst = (int64_t*)ray_data(c_out);
    int64_t  gi    = 0;
    for (uint64_t s = 0; s < cap; s++) {
        if (!slots[s * 2]) continue;
        int64_t kv = slots[s * 2 + 1];
        write_col_i64(k_dst, gi, kv, kt, key_col->attrs);
        c_dst[gi] = counts[s];
        gi++;
    }

    scratch_free(k_hdr);
    scratch_free(c_hdr);

    /* 2-column TABLE: <key_col_name>, <count>.  Phase 2 wires the count
     * column name through the planner; phase 1 uses the literal "count". */
    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(k_out); ray_release(c_out);
        return ray_error("oom", NULL);
    }
    int64_t cnt_sym = ray_sym_intern("count", 5);
    result = ray_table_add_col(result, kext->sym, k_out);
    result = ray_table_add_col(result, cnt_sym,   c_out);
    ray_release(k_out);
    ray_release(c_out);
    return result;
}
