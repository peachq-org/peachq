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
#include "lang/eval.h"     /* RAY_ATTR_NAME */
#include "core/pool.h"     /* ray_pool_get / ray_pool_dispatch */

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
 * Phase-2 parallel fused exec.
 *
 * Per-worker linear-probe hash shards, populated by ray_pool_dispatch
 * over morsel ranges.  Workers lazy-init their shard on first call,
 * grow it 2× when load reaches 0.5, and update it inline as they walk
 * the morsels.  After all workers finish, the main thread walks shard
 * occupants in series and merges them into a global HT before
 * materialising the 2-column [key, count] result table.
 * ──────────────────────────────────────────────────────────────────────── */

#define FP_SHARD_INIT_CAP   1024ULL
#define FP_SHARD_MAX_CAP    (1ULL << 26)   /* 64 M slots ≈ 1 GB per shard */

typedef struct {
    int64_t* slots;       /* cap × 2 (occupied flag, key) */
    int64_t* counts;      /* cap */
    ray_t*   slots_hdr;   /* scratch headers for cleanup */
    ray_t*   counts_hdr;
    uint64_t cap;
    uint64_t mask;
    int64_t  n_filled;
} fp_shard_t;

typedef struct {
    fp_cmp_t   pred;
    int8_t     kt;
    uint8_t    katt;
    uint8_t    kesz;
    const void* kbase;
    fp_shard_t* shards;     /* nw entries */
    uint64_t   init_cap;
    _Atomic(uint32_t) oom;  /* set by any worker on OOM; main bails */
} fp_par_ctx_t;

static int fp_shard_init(fp_shard_t* sh, uint64_t cap) {
    sh->slots  = (int64_t*)scratch_calloc(&sh->slots_hdr,
                                          (size_t)cap * 2 * sizeof(int64_t));
    sh->counts = (int64_t*)scratch_calloc(&sh->counts_hdr,
                                          (size_t)cap * sizeof(int64_t));
    if (!sh->slots || !sh->counts) {
        if (sh->slots_hdr)  { scratch_free(sh->slots_hdr);  sh->slots_hdr  = NULL; }
        if (sh->counts_hdr) { scratch_free(sh->counts_hdr); sh->counts_hdr = NULL; }
        sh->slots = NULL; sh->counts = NULL;
        return -1;
    }
    sh->cap  = cap;
    sh->mask = cap - 1;
    sh->n_filled = 0;
    return 0;
}

static void fp_shard_free(fp_shard_t* sh) {
    if (sh->slots_hdr)  { scratch_free(sh->slots_hdr);  sh->slots_hdr  = NULL; }
    if (sh->counts_hdr) { scratch_free(sh->counts_hdr); sh->counts_hdr = NULL; }
    sh->slots = NULL; sh->counts = NULL;
}

/* Double cap and rehash in place.  Returns -1 on OOM (shard left intact). */
static int fp_shard_grow(fp_shard_t* sh) {
    uint64_t new_cap = sh->cap * 2;
    if (new_cap > FP_SHARD_MAX_CAP) return -1;
    uint64_t new_mask = new_cap - 1;
    ray_t*   ns_hdr = NULL;
    ray_t*   nc_hdr = NULL;
    int64_t* ns = (int64_t*)scratch_calloc(&ns_hdr,
                                           (size_t)new_cap * 2 * sizeof(int64_t));
    int64_t* nc = (int64_t*)scratch_calloc(&nc_hdr,
                                           (size_t)new_cap * sizeof(int64_t));
    if (!ns || !nc) {
        if (ns_hdr) scratch_free(ns_hdr);
        if (nc_hdr) scratch_free(nc_hdr);
        return -1;
    }
    for (uint64_t s = 0; s < sh->cap; s++) {
        if (!sh->slots[s * 2]) continue;
        int64_t kv  = sh->slots[s * 2 + 1];
        int64_t cnt = sh->counts[s];
        uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 33;
        uint64_t t = h & new_mask;
        while (ns[t * 2]) t = (t + 1) & new_mask;
        ns[t * 2]     = 1;
        ns[t * 2 + 1] = kv;
        nc[t]         = cnt;
    }
    scratch_free(sh->slots_hdr);
    scratch_free(sh->counts_hdr);
    sh->slots      = ns;
    sh->counts     = nc;
    sh->slots_hdr  = ns_hdr;
    sh->counts_hdr = nc_hdr;
    sh->cap        = new_cap;
    sh->mask       = new_mask;
    return 0;
}

/* Insert kv into shard, or bump count if already present.  Returns -1 on
 * grow OOM. */
static inline int fp_shard_upsert(fp_shard_t* sh, int64_t kv) {
    if (RAY_UNLIKELY(sh->n_filled * 2 >= (int64_t)sh->cap)) {
        if (fp_shard_grow(sh) != 0) return -1;
    }
    uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 33;
    uint64_t s = h & sh->mask;
    for (;;) {
        if (!sh->slots[s * 2]) {
            sh->slots[s * 2]     = 1;
            sh->slots[s * 2 + 1] = kv;
            sh->counts[s]        = 1;
            sh->n_filled++;
            return 0;
        }
        if (sh->slots[s * 2 + 1] == kv) { sh->counts[s]++; return 0; }
        s = (s + 1) & sh->mask;
    }
}

/* Worker: walk rows [start, end), eval predicate per-morsel, upsert
 * passing rows into per-worker shard. */
static void fp_par_fn(void* raw, uint32_t worker_id, int64_t start, int64_t end) {
    fp_par_ctx_t* c = (fp_par_ctx_t*)raw;
    if (atomic_load_explicit(&c->oom, memory_order_relaxed)) return;
    fp_shard_t* sh = &c->shards[worker_id];
    if (!sh->slots) {
        if (fp_shard_init(sh, c->init_cap) != 0) {
            atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
            return;
        }
    }
    int64_t row = start;
    while (row < end) {
        int64_t mend = row + RAY_MORSEL_ELEMS;
        if (mend > end) mend = end;
        int64_t mlen = mend - row;
        uint8_t bits[RAY_MORSEL_ELEMS];
        fp_eval_cmp(&c->pred, row, mend, bits);
        for (int64_t r = 0; r < mlen; r++) {
            if (!bits[r]) continue;
            int64_t kv = read_by_esz(c->kbase, row + r, c->kesz);
            if (fp_shard_upsert(sh, kv) != 0) {
                atomic_store_explicit(&c->oom, 1, memory_order_relaxed);
                return;
            }
        }
        row = mend;
    }
}

/* Combine: merge per-worker shards into a global linear-probe hash, then
 * materialize the [key, count] result columns.  Caller frees shards
 * afterwards. */
static ray_t* fp_combine_and_materialize(fp_shard_t* shards, uint32_t nw,
                                         int8_t kt, uint8_t katt,
                                         int64_t key_sym)
{
    /* Sum local fills to size the global HT. */
    int64_t total_local = 0;
    for (uint32_t w = 0; w < nw; w++) total_local += shards[w].n_filled;
    if (total_local == 0) {
        /* Empty result.  Build a 2-col table with 0 rows. */
        ray_t* k0 = (kt == RAY_SYM)
                  ? ray_sym_vec_new(katt & RAY_SYM_W_MASK, 0)
                  : ray_vec_new(kt, 0);
        ray_t* c0 = ray_vec_new(RAY_I64, 0);
        if (!k0 || !c0 || RAY_IS_ERR(k0) || RAY_IS_ERR(c0)) {
            if (k0 && !RAY_IS_ERR(k0)) ray_release(k0);
            if (c0 && !RAY_IS_ERR(c0)) ray_release(c0);
            return ray_error("oom", NULL);
        }
        k0->len = 0; c0->len = 0;
        ray_t* result = ray_table_new(2);
        if (!result || RAY_IS_ERR(result)) {
            ray_release(k0); ray_release(c0);
            return ray_error("oom", NULL);
        }
        int64_t cnt_sym = ray_sym_intern("count", 5);
        result = ray_table_add_col(result, key_sym, k0);
        result = ray_table_add_col(result, cnt_sym, c0);
        ray_release(k0); ray_release(c0);
        return result;
    }

    /* Global cap: 2 × total_local rounded up to power of 2.  This is an
     * upper bound (groups can collide across shards) so the global HT
     * never has to grow. */
    uint64_t gcap = 1024;
    while (gcap < (uint64_t)(total_local * 2) && gcap < FP_SHARD_MAX_CAP) gcap <<= 1;
    uint64_t gmask = gcap - 1;
    ray_t* gs_hdr = NULL;
    ray_t* gc_hdr = NULL;
    int64_t* gs = (int64_t*)scratch_calloc(&gs_hdr,
                                           (size_t)gcap * 2 * sizeof(int64_t));
    int64_t* gc = (int64_t*)scratch_calloc(&gc_hdr,
                                           (size_t)gcap * sizeof(int64_t));
    if (!gs || !gc) {
        if (gs_hdr) scratch_free(gs_hdr);
        if (gc_hdr) scratch_free(gc_hdr);
        return ray_error("oom", NULL);
    }
    int64_t global_n = 0;
    for (uint32_t w = 0; w < nw; w++) {
        fp_shard_t* sh = &shards[w];
        if (!sh->slots) continue;
        for (uint64_t s = 0; s < sh->cap; s++) {
            if (!sh->slots[s * 2]) continue;
            int64_t kv  = sh->slots[s * 2 + 1];
            int64_t cnt = sh->counts[s];
            uint64_t h = (uint64_t)kv * 0x9E3779B97F4A7C15ULL;
            h ^= h >> 33;
            uint64_t t = h & gmask;
            for (;;) {
                if (!gs[t * 2]) {
                    gs[t * 2]     = 1;
                    gs[t * 2 + 1] = kv;
                    gc[t]         = cnt;
                    global_n++;
                    break;
                }
                if (gs[t * 2 + 1] == kv) { gc[t] += cnt; break; }
                t = (t + 1) & gmask;
            }
        }
    }

    /* Materialize. */
    ray_t* k_out = (kt == RAY_SYM)
                 ? ray_sym_vec_new(katt & RAY_SYM_W_MASK, global_n)
                 : ray_vec_new(kt, global_n);
    ray_t* c_out = ray_vec_new(RAY_I64, global_n);
    if (!k_out || !c_out || RAY_IS_ERR(k_out) || RAY_IS_ERR(c_out)) {
        if (k_out && !RAY_IS_ERR(k_out)) ray_release(k_out);
        if (c_out && !RAY_IS_ERR(c_out)) ray_release(c_out);
        scratch_free(gs_hdr); scratch_free(gc_hdr);
        return ray_error("oom", NULL);
    }
    k_out->len = global_n;
    c_out->len = global_n;
    void*    k_dst = ray_data(k_out);
    int64_t* c_dst = (int64_t*)ray_data(c_out);
    int64_t  gi    = 0;
    for (uint64_t s = 0; s < gcap; s++) {
        if (!gs[s * 2]) continue;
        int64_t kv = gs[s * 2 + 1];
        write_col_i64(k_dst, gi, kv, kt, katt);
        c_dst[gi] = gc[s];
        gi++;
    }
    scratch_free(gs_hdr);
    scratch_free(gc_hdr);

    ray_t* result = ray_table_new(2);
    if (!result || RAY_IS_ERR(result)) {
        ray_release(k_out); ray_release(c_out);
        return ray_error("oom", NULL);
    }
    int64_t cnt_sym = ray_sym_intern("count", 5);
    result = ray_table_add_col(result, key_sym, k_out);
    result = ray_table_add_col(result, cnt_sym, c_out);
    ray_release(k_out);
    ray_release(c_out);
    return result;
}

ray_t* exec_filtered_group(ray_graph_t* g, ray_op_t* op) {
    if (!g || !op) return ray_error("nyi", NULL);
    ray_t* tbl = g->table;
    if (!tbl || tbl->type != RAY_TABLE) return ray_error("type", NULL);

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext || ext->n_keys != 1 || ext->n_aggs != 1)
        return ray_error("nyi", "fused_group: phase-2 single-key + single-agg only");
    if (ext->agg_ops[0] != OP_COUNT)
        return ray_error("nyi", "fused_group: phase-2 only OP_COUNT");

    ray_op_t* pred_op = ext->base.inputs[0];
    fp_par_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (fp_compile_cmp(g, pred_op, tbl, &ctx.pred) != 0)
        return ray_error("nyi", "fused_group: predicate shape unsupported");

    ray_op_t* key_op = ext->keys[0];
    if (!key_op || key_op->opcode != OP_SCAN)
        return ray_error("nyi", "fused_group: phase-2 needs SCAN key");
    ray_op_ext_t* kext = find_ext(g, key_op->id);
    if (!kext) return ray_error("nyi", NULL);
    ray_t* key_col = ray_table_get_col(tbl, kext->sym);
    if (!key_col) return ray_error("schema", NULL);
    if (RAY_IS_PARTED(key_col->type) || key_col->type == RAY_MAPCOMMON)
        return ray_error("nyi", "fused_group: phase-2 needs flat key column");

    int64_t nrows = key_col->len;
    ctx.kt    = key_col->type;
    ctx.katt  = key_col->attrs;
    ctx.kesz  = ray_sym_elem_size(ctx.kt, ctx.katt);
    ctx.kbase = ray_data(key_col);
    ctx.init_cap = FP_SHARD_INIT_CAP;
    atomic_store_explicit(&ctx.oom, 0, memory_order_relaxed);

    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = pool ? ray_pool_total_workers(pool) : 1;
    ray_t* shards_hdr = NULL;
    ctx.shards = (fp_shard_t*)scratch_calloc(&shards_hdr,
                                             (size_t)nw * sizeof(fp_shard_t));
    if (!ctx.shards) return ray_error("oom", NULL);

    if (pool) {
        ray_pool_dispatch(pool, fp_par_fn, &ctx, nrows);
    } else {
        /* No pool: serial fallback. */
        fp_par_fn(&ctx, 0, 0, nrows);
    }

    if (atomic_load_explicit(&ctx.oom, memory_order_relaxed)) {
        for (uint32_t w = 0; w < nw; w++) fp_shard_free(&ctx.shards[w]);
        scratch_free(shards_hdr);
        return ray_error("oom", "fused_group: shard OOM");
    }

    ray_t* result = fp_combine_and_materialize(ctx.shards, nw,
                                               ctx.kt, ctx.katt, kext->sym);
    for (uint32_t w = 0; w < nw; w++) fp_shard_free(&ctx.shards[w]);
    scratch_free(shards_hdr);
    return result;
}
