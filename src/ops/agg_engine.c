/* src/ops/agg_engine.c — v2 aggregation engine. */
#include "ops/agg_engine.h"
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include "ops/internal.h"  /* col_vec_new, col_esz */
#include "lang/internal.h" /* sym_domain_rep */
#include "table/sym.h"    /* ray_read_sym */
#include <stdlib.h>
#include <string.h>

bool ray_agg_engine_v2 = false;   /* knob; default off */

/* Read element `row` of an integer/temporal/SYM column widened to int64. */
static inline int64_t agg_read_key_i64(ray_t* col, const void* data, int64_t row);
/* Write a finalized scalar cell into output column slot i, marking nulls. */
static void agg_put_cell(ray_t* out, int64_t i, ray_t* cell);
/* Binary-aggregate (pearson) serial driver; defined below agg_run_one. */
ray_t* agg_run_one_bin(const agg_vtable_t* vt, ray_t* x_col, ray_t* y_col,
                       const uint32_t* gids, int64_t nrows, int64_t ngroups);

bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return false;
    if (ext->n_keys < 1 || ext->n_keys > 16) return false;  /* 1b: 1..16 keys */
    if (ext->n_aggs == 0) return false;        /* need >=1 aggregate  */
    if (!tbl) return false;
    if (g->selection) return false;            /* no active/pushed filter */

    /* every key must be a plain column scan of a supported type */
    for (uint8_t k = 0; k < ext->n_keys; k++) {
        ray_op_t* key = ext->keys[k];
        if (!key || key->opcode != OP_SCAN) return false;
        ray_op_ext_t* kext = find_ext(g, key->id);
        ray_t* kc = kext ? ray_table_get_col(tbl, kext->sym) : NULL;
        if (!kc) return false;
        switch (kc->type) {
            case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
            case RAY_BOOL: case RAY_DATE: case RAY_TIME:
            case RAY_TIMESTAMP: case RAY_SYM: break;
            default: return false;
        }
    }

    /* every aggregate must be a registry-resolvable plain-column scan */
    for (uint8_t a = 0; a < ext->n_aggs; a++) {
        if (ext->agg_k && ext->agg_k[a]) return false;            /* holistic K deferred */
        if (ext->agg_ins2 && ext->agg_ins2[a]) {
            if (ext->agg_ops[a] != OP_PEARSON_CORR) return false; /* only pearson in 2b */
            ray_op_t* xin = ext->agg_ins[a]; ray_op_t* yin = ext->agg_ins2[a];
            if (!xin || xin->opcode != OP_SCAN || !yin || yin->opcode != OP_SCAN) return false;
            ray_op_ext_t* xe = find_ext(g, xin->id); ray_op_ext_t* ye = find_ext(g, yin->id);
            ray_t* xc = xe ? ray_table_get_col(tbl, xe->sym) : NULL;
            ray_t* yc = ye ? ray_table_get_col(tbl, ye->sym) : NULL;
            if (!xc || !yc || xc->type != RAY_F64 || yc->type != RAY_F64) return false;  /* 2b: F64 only */
            if (!agg_resolve(OP_PEARSON_CORR, RAY_F64)) return false;
            continue;  /* admitted */
        }
        if (ext->agg_ops[a] == OP_COUNT) {
            /* count: needs no typed input column */
            if (!agg_resolve(OP_COUNT, RAY_I64)) return false;
            continue;
        }
        ray_op_t* in = ext->agg_ins[a];
        if (!in || in->opcode != OP_SCAN) return false;
        ray_op_ext_t* ie = find_ext(g, in->id);
        ray_t* ic = (ie) ? ray_table_get_col(tbl, ie->sym) : NULL;
        if (!ic) return false;
        if (!agg_resolve(ext->agg_ops[a], ic->type)) return false;
    }
    return true;
}

/* Per-op result-column-name suffix, mirroring emit_agg_columns (group.c).
 * Returns "" / 0 for ops without a suffix. */
static const char* agg_name_suffix(uint16_t agg_op, size_t* slen_out) {
    const char* sfx = ""; size_t slen = 0;
    switch (agg_op) {
        case OP_SUM:   sfx = "_sum";   slen = 4; break;
        case OP_PROD:  sfx = "_prod";  slen = 5; break;
        case OP_COUNT: sfx = "_count"; slen = 6; break;
        case OP_AVG:   sfx = "_mean";  slen = 5; break;
        case OP_MIN:   sfx = "_min";   slen = 4; break;
        case OP_MAX:   sfx = "_max";   slen = 4; break;
        case OP_FIRST: sfx = "_first"; slen = 6; break;
        case OP_LAST:  sfx = "_last";  slen = 5; break;
        case OP_STDDEV:     sfx = "_stddev";     slen = 7; break;
        case OP_STDDEV_POP: sfx = "_stddev_pop"; slen = 11; break;
        case OP_VAR:        sfx = "_var";        slen = 4; break;
        case OP_VAR_POP:    sfx = "_var_pop";    slen = 8; break;
        case OP_MEDIAN:     sfx = "_median";     slen = 7; break;
        case OP_TOP_N:      sfx = "_top";        slen = 4; break;
        case OP_BOT_N:      sfx = "_bot";        slen = 4; break;
        default: break;
    }
    *slen_out = slen;
    return sfx;
}

/* Result column name for a plain-column-input aggregate: the input column's
 * name (for in_sym) plus the per-op suffix, interned.  On overflow, falls back
 * to the input sym itself — byte-identical to emit_agg_columns' inline copy. */
int64_t agg_result_col_name(int64_t in_sym, uint16_t agg_op) {
    ray_t* name_atom = ray_sym_str(in_sym);
    const char* base = name_atom ? ray_str_ptr(name_atom) : NULL;
    size_t blen = base ? ray_str_len(name_atom) : 0;
    size_t slen = 0;
    const char* sfx = agg_name_suffix(agg_op, &slen);
    char buf[256];
    if (base && blen + slen < sizeof(buf)) {
        memcpy(buf, base, blen);
        memcpy(buf + blen, sfx, slen);
        return ray_sym_intern(buf, blen + slen);
    }
    return in_sym;
}

/* Build a result key column of src_col's type by gathering the first-row cell
 * of each group at native (type-exact) byte width.  For SYM, adopts the source
 * domain so the intern ids resolve correctly.  Caller owns the returned column. */
static ray_t* agg_gather_key_col(ray_t* src_col, const int64_t* first_row, int64_t n) {
    ray_t* out = col_vec_new(src_col, n);
    if (!out || RAY_IS_ERR(out)) return out;
    if (out->type == RAY_SYM)
        ray_sym_vec_adopt_domain(out, sym_domain_rep(src_col));
    out->len = n;
    size_t esz = col_esz(src_col);
    const char* src = (const char*)ray_data(src_col);
    char* dst = (char*)ray_data(out);
    for (int64_t gi = 0; gi < n; gi++)
        memcpy(dst + (size_t)gi * esz, src + (size_t)first_row[gi] * esz, esz);
    if (src_col->attrs & RAY_ATTR_HAS_NULLS) out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}

/* ══════════════════════════════════════════
 * Parallel two-phase hash group-by (Phase 1c)
 * ══════════════════════════════════════════ */

#include "core/pool.h"

/* Per-worker (and global merge) local group table: open-addressing hash on the
 * tuple-hash → local gid; AoS per-group state blocks of `block` bytes. */
typedef struct {
    int32_t* ht;          /* [htcap] slot -> local gid, -1 empty */
    int64_t  htcap;
    uint64_t htmask;
    int64_t* first_row;   /* [cap] min row idx per local group */
    char*    states;      /* [cap*block] AoS group state */
    int64_t  ng, cap;
    int      oom;
} agg_local_t;

/* Tuple FNV-1a hash over all keys at row r — identical to agg_group_keys. */
static inline uint64_t agg_tuple_hash(ray_t** key_cols, const void** key_data,
                                      uint8_t n_keys, int64_t r) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t k = 0; k < n_keys; k++) {
        int64_t v = agg_read_key_i64(key_cols[k], key_data[k], r);
        h ^= (uint64_t)v; h *= 1099511628211ULL;
    }
    return h;
}

/* All keys at row ra equal all keys at row rb? */
static inline int agg_tuple_eq(ray_t** key_cols, const void** key_data,
                               uint8_t n_keys, int64_t ra, int64_t rb) {
    for (uint8_t k = 0; k < n_keys; k++)
        if (agg_read_key_i64(key_cols[k], key_data[k], ra) !=
            agg_read_key_i64(key_cols[k], key_data[k], rb))
            return 0;
    return 1;
}

static int agg_local_init(agg_local_t* loc, int64_t cap, int64_t htcap, int64_t block) {
    loc->ng = 0; loc->cap = cap; loc->oom = 0;
    loc->htcap = htcap; loc->htmask = (uint64_t)htcap - 1;
    loc->ht        = malloc((size_t)htcap * sizeof(int32_t));
    loc->first_row = malloc((size_t)cap * sizeof(int64_t));
    loc->states    = malloc((size_t)cap * (size_t)block);
    if (!loc->ht || !loc->first_row || !loc->states) { loc->oom = 1; return -1; }
    for (int64_t i = 0; i < htcap; i++) loc->ht[i] = -1;
    return 0;
}

static void agg_local_destroy(agg_local_t* loc) {
    free(loc->ht); free(loc->first_row); free(loc->states);
    loc->ht = NULL; loc->first_row = NULL; loc->states = NULL;
}

/* Context shared (read-only) across workers; per-worker writes go to locals[wid]. */
typedef struct {
    ray_t**            key_cols;
    const void**       key_data;
    uint8_t            n_keys;
    const agg_vtable_t** vts;
    const size_t*      off;        /* [n_aggs] byte offset of agg a in a block */
    size_t             block;      /* bytes per group block */
    uint8_t            n_aggs;
    const void**       val_data;   /* [n_aggs] base ptr or NULL (COUNT) */
    const int8_t*      val_types;  /* [n_aggs] */
    const bool*        val_hasnull;/* [n_aggs] */
    const uint8_t*     val_esz;    /* [n_aggs] element size or 0 (COUNT) */
    /* Binary aggregates (pearson): second value column.  NULL/0 for unary. */
    const void**       val2_data;  /* [n_aggs] */
    const int8_t*      val2_types; /* [n_aggs] */
    const bool*        val2_hasnull;/* [n_aggs] */
    const uint8_t*     val2_esz;   /* [n_aggs] */
    agg_local_t*       locals;     /* [nw] */
} agg_par_ctx_t;

/* (first_row, group idx) pair, sorted by first_row to recover emit order. */
typedef struct { int64_t fr; int64_t idx; } agg_fr_pair_t;
static int agg_fr_pair_cmp(const void* x, const void* y) {
    int64_t a = ((const agg_fr_pair_t*)x)->fr, b = ((const agg_fr_pair_t*)y)->fr;
    return (a > b) - (a < b);
}

/* Phase A: per-worker local group + accumulate over chunk [start,end). */
static void agg_phaseA_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_par_ctx_t* c = (agg_par_ctx_t*)vctx;
    agg_local_t* loc = &c->locals[wid];
    if (loc->oom) return;
    int64_t n = end - start;
    if (n <= 0) return;

    uint32_t* cgid = malloc((size_t)n * sizeof(uint32_t));
    if (!cgid) { loc->oom = 1; return; }

    for (int64_t r = start; r < end; r++) {
        uint64_t h = agg_tuple_hash(c->key_cols, c->key_data, c->n_keys, r);
        uint64_t slot = h & loc->htmask;
        int32_t g;
        for (;;) {
            int32_t gp = loc->ht[slot];
            if (gp < 0) {                       /* new group */
                if (loc->ng == loc->cap) { loc->oom = 1; free(cgid); return; }
                g = (int32_t)loc->ng;
                loc->ht[slot] = g;
                loc->first_row[g] = r;
                for (uint8_t a = 0; a < c->n_aggs; a++)
                    c->vts[a]->init(loc->states + (size_t)g * c->block + c->off[a]);
                loc->ng++;
                break;
            }
            if (agg_tuple_eq(c->key_cols, c->key_data, c->n_keys, r, loc->first_row[gp])) {
                g = gp;
                if (r < loc->first_row[g]) loc->first_row[g] = r;  /* MIN across chunks */
                break;
            }
            slot = (slot + 1) & loc->htmask;
        }
        cgid[r - start] = (uint32_t)g;
    }

    for (uint8_t a = 0; a < c->n_aggs; a++) {
        if (c->vts[a]->update_batch2) {                 /* binary agg (pearson) */
            const void* vx = (const char*)c->val_data[a]  + (size_t)start * c->val_esz[a];
            const void* vy = (const char*)c->val2_data[a] + (size_t)start * c->val2_esz[a];
            ray_valid_t valid_x = { vx, c->val_types[a],  c->val_hasnull[a] };
            ray_valid_t valid_y = { vy, c->val2_types[a], c->val2_hasnull[a] };
            c->vts[a]->update_batch2(loc->states + c->off[a], c->block, cgid,
                                     vx, vy, &valid_x, &valid_y, n, NULL);
        } else {
            const void* vals = (c->val_data[a])
                ? (const void*)((const char*)c->val_data[a] + (size_t)start * c->val_esz[a])
                : NULL;
            ray_valid_t valid = { vals, c->val_types[a],
                                  c->val_data[a] ? c->val_hasnull[a] : false };
            c->vts[a]->update_batch(loc->states + c->off[a], c->block,
                                    cgid, vals, &valid, n, NULL);
        }
    }
    free(cgid);
}

/* Parallel two-phase path. key_cols/key_syms resolved by caller. */
static ray_t* exec_group_v2_parallel(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
        ray_t** key_cols, int64_t* key_syms,
        const agg_vtable_t** vts, const size_t* off, size_t block) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    uint8_t n_keys = ext->n_keys, n_aggs = ext->n_aggs;
    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = ray_pool_total_workers(pool);

    /* key data bases (read-only across workers) */
    const void* key_data[16];
    for (uint8_t k = 0; k < n_keys; k++) key_data[k] = ray_data(key_cols[k]);

    /* per-agg value column bases / types (resolve once) */
    const void* val_data[16]; int8_t val_types[16]; bool val_hasnull[16]; uint8_t val_esz[16];
    const void* val2_data[16]; int8_t val2_types[16]; bool val2_hasnull[16]; uint8_t val2_esz[16];
    int64_t agg_syms[16];
    for (uint8_t a = 0; a < n_aggs; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]->id);
        agg_syms[a] = ie->sym;
        ray_t* vc = (ext->agg_ops[a] != OP_COUNT) ? ray_table_get_col(tbl, ie->sym) : NULL;
        val_data[a]    = vc ? ray_data(vc) : NULL;
        val_types[a]   = vc ? vc->type : RAY_I64;
        val_hasnull[a] = vc ? ((vc->attrs & RAY_ATTR_HAS_NULLS) != 0) : false;
        val_esz[a]     = vc ? col_esz(vc) : 0;
        /* Binary agg (pearson): resolve the y-side column from agg_ins2[a]. */
        ray_t* vc2 = (ext->agg_ins2 && ext->agg_ins2[a])
            ? ray_table_get_col(tbl, find_ext(g, ext->agg_ins2[a]->id)->sym) : NULL;
        val2_data[a]    = vc2 ? ray_data(vc2) : NULL;
        val2_types[a]   = vc2 ? vc2->type : RAY_I64;
        val2_hasnull[a] = vc2 ? ((vc2->attrs & RAY_ATTR_HAS_NULLS) != 0) : false;
        val2_esz[a]     = vc2 ? col_esz(vc2) : 0;
    }

    /* per-worker ht sized to next_pow2(2*nrows): over-allocates (each worker
     * sees only its morsels) but memory isn't gated in 1c — simple & correct. */
    int64_t htcap = 16;
    while (htcap < nrows * 2) htcap <<= 1;
    int64_t cap = nrows > 0 ? nrows : 1;   /* a worker can't exceed nrows groups */

    agg_local_t* locals = calloc((size_t)nw, sizeof(agg_local_t));
    if (!locals) return ray_error("oom", NULL);

    int alloc_oom = 0;
    for (uint32_t w = 0; w < nw; w++)
        if (agg_local_init(&locals[w], cap, htcap, (int64_t)block) != 0) { alloc_oom = 1; break; }
    if (alloc_oom) {
        for (uint32_t w = 0; w < nw; w++) agg_local_destroy(&locals[w]);
        free(locals);
        return ray_error("oom", NULL);
    }

    agg_par_ctx_t ctx = {
        .key_cols = key_cols, .key_data = key_data, .n_keys = n_keys,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs,
        .val_data = val_data, .val_types = val_types,
        .val_hasnull = val_hasnull, .val_esz = val_esz,
        .val2_data = val2_data, .val2_types = val2_types,
        .val2_hasnull = val2_hasnull, .val2_esz = val2_esz, .locals = locals,
    };
    ray_pool_dispatch(pool, agg_phaseA_fn, &ctx, nrows);

    for (uint32_t w = 0; w < nw; w++)
        if (locals[w].oom) {
            for (uint32_t i = 0; i < nw; i++) agg_local_destroy(&locals[i]);
            free(locals);
            return ray_error("oom", NULL);
        }

    /* ── Phase B: merge per-worker locals into a global table (serial) ── */
    agg_local_t gt = {0};
    if (agg_local_init(&gt, cap, htcap, (int64_t)block) != 0) {
        agg_local_destroy(&gt);
        for (uint32_t i = 0; i < nw; i++) agg_local_destroy(&locals[i]);
        free(locals);
        return ray_error("oom", NULL);
    }

    for (uint32_t w = 0; w < nw; w++) {
        agg_local_t* loc = &locals[w];
        for (int64_t lg = 0; lg < loc->ng; lg++) {
            int64_t fr = loc->first_row[lg];
            uint64_t h = agg_tuple_hash(key_cols, key_data, n_keys, fr);
            uint64_t slot = h & gt.htmask;
            int64_t gg;
            for (;;) {
                int32_t gp = gt.ht[slot];
                if (gp < 0) {                          /* new global group */
                    gg = gt.ng;
                    gt.ht[slot] = (int32_t)gg;
                    gt.first_row[gg] = fr;
                    for (uint8_t a = 0; a < n_aggs; a++)
                        vts[a]->init(gt.states + (size_t)gg * block + off[a]);
                    gt.ng++;
                    break;
                }
                if (agg_tuple_eq(key_cols, key_data, n_keys, fr, gt.first_row[gp])) {
                    gg = gp;
                    if (fr < gt.first_row[gg]) gt.first_row[gg] = fr;  /* MIN */
                    break;
                }
                slot = (slot + 1) & gt.htmask;
            }
            for (uint8_t a = 0; a < n_aggs; a++)
                vts[a]->merge(gt.states + (size_t)gg * block + off[a],
                              loc->states + (size_t)lg * block + off[a], NULL);
        }
    }
    int64_t ng = gt.ng;

    /* Phase A locals no longer needed past the merge. */
    for (uint32_t i = 0; i < nw; i++) agg_local_destroy(&locals[i]);
    free(locals);

    /* ── Phase C: order by first_row, emit key + agg columns ── */
    int64_t* order = malloc((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    int64_t* first_row_ordered = malloc((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    if (!order || !first_row_ordered) {
        free(order); free(first_row_ordered); agg_local_destroy(&gt);
        return ray_error("oom", NULL);
    }
    /* Order group indices by first_row ascending.  first_row is distinct across
     * groups → total order, so the sort is unambiguously deterministic and gives
     * the same first-occurrence emit order as the serial path. */
    {
        agg_fr_pair_t* pairs = malloc((size_t)(ng > 0 ? ng : 1) * sizeof(agg_fr_pair_t));
        if (!pairs) {
            free(order); free(first_row_ordered); agg_local_destroy(&gt);
            return ray_error("oom", NULL);
        }
        for (int64_t i = 0; i < ng; i++) { pairs[i].fr = gt.first_row[i]; pairs[i].idx = i; }
        qsort(pairs, (size_t)ng, sizeof(agg_fr_pair_t), agg_fr_pair_cmp);
        for (int64_t i = 0; i < ng; i++) {
            order[i] = pairs[i].idx;
            first_row_ordered[i] = pairs[i].fr;
        }
        free(pairs);
    }

    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        free(order); free(first_row_ordered); agg_local_destroy(&gt);
        return result ? result : ray_error("oom", NULL);
    }

    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* kc = agg_gather_key_col(key_cols[k], first_row_ordered, ng);
        if (!kc || RAY_IS_ERR(kc)) {
            free(order); free(first_row_ordered); agg_local_destroy(&gt);
            ray_release(result); return kc ? kc : ray_error("oom", NULL);
        }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }

    for (uint8_t a = 0; a < n_aggs; a++) {
        ray_t* out = ray_vec_new(vts[a]->out_type, ng);
        if (!out || RAY_IS_ERR(out)) {
            free(order); free(first_row_ordered); agg_local_destroy(&gt);
            ray_release(result); return out ? out : ray_error("oom", NULL);
        }
        out->len = ng;
        for (int64_t i = 0; i < ng; i++) {
            ray_t* cell = vts[a]->finalize(gt.states + (size_t)order[i] * block + off[a], NULL);
            agg_put_cell(out, i, cell);
            ray_release(cell);
        }
        int64_t agg_name = agg_result_col_name(agg_syms[a], ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, out);
        ray_release(out);
    }

    free(order); free(first_row_ordered); agg_local_destroy(&gt);
    return result;
}

ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    int64_t nrows = ray_table_nrows(tbl);

    ray_t* key_cols[16]; int64_t key_syms[16];
    for (uint8_t k = 0; k < ext->n_keys; k++) {
        ray_op_ext_t* kext = find_ext(g, ext->keys[k]->id);
        key_cols[k] = ray_table_get_col(tbl, kext->sym);
        key_syms[k] = kext->sym;
    }

    /* Precompute AoS state layout for the admitted aggregates. */
    const agg_vtable_t* vts[16]; size_t off[16]; size_t block = 0;
    for (uint8_t a = 0; a < ext->n_aggs; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]->id);
        ray_t* vc = (ext->agg_ops[a] != OP_COUNT) ? ray_table_get_col(tbl, ie->sym) : NULL;
        int8_t in_type = vc ? vc->type : RAY_I64;
        vts[a] = agg_resolve(ext->agg_ops[a], in_type);
        off[a] = block;
        block += vts[a]->state_size;
    }

    ray_pool_t* pool = ray_pool_get();
    if (pool && nrows >= RAY_PARALLEL_THRESHOLD)
        return exec_group_v2_parallel(g, op, tbl, nrows, key_cols, key_syms, vts, off, block);

    agg_groups_t groups = {0};
    if (agg_group_keys(key_cols, ext->n_keys, nrows, &groups) != 0) return ray_error("oom", NULL);

    ray_t* result = ray_table_new(ext->n_keys + ext->n_aggs);
    if (!result || RAY_IS_ERR(result)) { free(groups.gids); free(groups.first_row); return ray_error("oom", NULL); }

    for (uint8_t k = 0; k < ext->n_keys; k++) {
        ray_t* kc = agg_gather_key_col(key_cols[k], groups.first_row, groups.ngroups);
        if (!kc || RAY_IS_ERR(kc)) { free(groups.gids); free(groups.first_row); ray_release(result); return kc ? kc : ray_error("oom", NULL); }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }

    for (uint8_t a = 0; a < ext->n_aggs; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]->id);
        ray_t* col;
        if (ext->agg_ins2 && ext->agg_ins2[a]) {       /* binary agg (pearson) */
            ray_op_ext_t* ye = find_ext(g, ext->agg_ins2[a]->id);
            ray_t* x_col = ray_table_get_col(tbl, ie->sym);
            ray_t* y_col = ray_table_get_col(tbl, ye->sym);
            const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], x_col->type);
            col = agg_run_one_bin(vt, x_col, y_col, groups.gids, nrows, groups.ngroups);
        } else {
            ray_t* val_col = (ext->agg_ops[a] != OP_COUNT) ? ray_table_get_col(tbl, ie->sym) : NULL;
            int8_t in_type = val_col ? val_col->type : RAY_I64;
            const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], in_type);
            col = agg_run_one(vt, val_col, groups.gids, nrows, groups.ngroups);
        }
        if (!col || RAY_IS_ERR(col)) { free(groups.gids); free(groups.first_row); ray_release(result); return col ? col : ray_error("oom", NULL); }
        int64_t agg_name = agg_result_col_name(ie->sym, ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, col);
        ray_release(col);
    }
    free(groups.gids); free(groups.first_row);
    return result;
}

/* Read element `row` of an integer/temporal/SYM column widened to int64. */
static inline int64_t agg_read_key_i64(ray_t* col, const void* data, int64_t row) {
    switch (col->type) {
        case RAY_I64: case RAY_TIMESTAMP: return ((const int64_t*)data)[row];
        case RAY_I32: case RAY_DATE: case RAY_TIME: return ((const int32_t*)data)[row];
        case RAY_I16: return ((const int16_t*)data)[row];
        case RAY_U8:  case RAY_BOOL: return ((const uint8_t*)data)[row];
        case RAY_SYM: return (int64_t)ray_read_sym(data, row, col->type, col->attrs);
        default: return 0;  /* gate guarantees only the above reach here */
    }
}

/* Write a finalized scalar cell into output column slot i, marking nulls.
 * Shared by agg_run_one (serial) and the parallel finalize. */
static void agg_put_cell(ray_t* out, int64_t i, ray_t* cell) {
    switch (out->type) {
        case RAY_F64:
            ((double*)ray_data(out))[i] = cell->f64; break;
        default: /* RAY_I64 (and temporal widths if they arise) */
            ((int64_t*)ray_data(out))[i] = cell->i64; break;
    }
    if (RAY_ATOM_IS_NULL(cell))
        ray_vec_set_null(out, i, true);   /* also sets RAY_ATTR_HAS_NULLS */
}

ray_t* agg_run_one(const agg_vtable_t* vt, ray_t* val_col,
                   const uint32_t* gids, int64_t nrows, int64_t ngroups) {
    char* states = calloc((size_t)(ngroups > 0 ? ngroups : 1), vt->state_size);
    if (!states) return ray_error("oom", NULL);
    for (int64_t gi = 0; gi < ngroups; gi++)
        vt->init(states + (size_t)gi * vt->state_size);

    ray_valid_t valid = { val_col ? ray_data(val_col) : NULL,
                          val_col ? val_col->type : RAY_I64,
                          val_col ? ((val_col->attrs & RAY_ATTR_HAS_NULLS) != 0) : false };
    const void* vals = val_col ? ray_data(val_col) : NULL;
    vt->update_batch(states, vt->state_size, gids, vals, &valid, nrows, NULL);

    ray_t* out = ray_vec_new(vt->out_type, ngroups);
    if (!out || RAY_IS_ERR(out)) { free(states); return ray_error("oom", NULL); }
    out->len = ngroups;
    for (int64_t gi = 0; gi < ngroups; gi++) {
        ray_t* cell = vt->finalize(states + (size_t)gi * vt->state_size, NULL);
        agg_put_cell(out, gi, cell);
        ray_release(cell);
    }
    free(states);
    return out;
}

/* Binary-aggregate (pearson) serial driver: mirrors agg_run_one but feeds two
 * value columns through vt->update_batch2.  A row contributes only when both x
 * and y are valid (the accumulator enforces this via valid_x/valid_y). */
ray_t* agg_run_one_bin(const agg_vtable_t* vt, ray_t* x_col, ray_t* y_col,
                       const uint32_t* gids, int64_t nrows, int64_t ngroups) {
    char* states = calloc((size_t)(ngroups > 0 ? ngroups : 1), vt->state_size);
    if (!states) return ray_error("oom", NULL);
    for (int64_t gi = 0; gi < ngroups; gi++)
        vt->init(states + (size_t)gi * vt->state_size);

    ray_valid_t vx = { ray_data(x_col), x_col->type,
                       (x_col->attrs & RAY_ATTR_HAS_NULLS) != 0 };
    ray_valid_t vy = { ray_data(y_col), y_col->type,
                       (y_col->attrs & RAY_ATTR_HAS_NULLS) != 0 };
    vt->update_batch2(states, vt->state_size, gids,
                      ray_data(x_col), ray_data(y_col), &vx, &vy, nrows, NULL);

    ray_t* out = ray_vec_new(vt->out_type, ngroups);
    if (!out || RAY_IS_ERR(out)) { free(states); return ray_error("oom", NULL); }
    out->len = ngroups;
    for (int64_t gi = 0; gi < ngroups; gi++) {
        ray_t* cell = vt->finalize(states + (size_t)gi * vt->state_size, NULL);
        agg_put_cell(out, gi, cell);
        ray_release(cell);
    }
    free(states);
    return out;
}

int agg_group_keys(ray_t** key_cols, uint8_t n_keys, int64_t nrows, agg_groups_t* out) {
    const void* data[16];
    for (uint8_t k = 0; k < n_keys; k++) data[k] = ray_data(key_cols[k]);

    /* hash table capacity: next pow2 >= 2*nrows, min 16 */
    int64_t cap = 16;
    while (cap < nrows * 2) cap <<= 1;
    uint64_t mask = (uint64_t)cap - 1;

    int32_t* ht_gid = malloc((size_t)cap * sizeof(int32_t));
    out->gids      = malloc((size_t)(nrows > 0 ? nrows : 1) * sizeof(uint32_t));
    out->first_row = malloc((size_t)(nrows > 0 ? nrows : 1) * sizeof(int64_t));
    if (!ht_gid || !out->gids || !out->first_row) {
        free(ht_gid); free(out->gids); free(out->first_row);
        out->gids = NULL; out->first_row = NULL; return -1;
    }
    for (int64_t i = 0; i < cap; i++) ht_gid[i] = -1;

    int64_t ngroups = 0;
    for (int64_t r = 0; r < nrows; r++) {
        uint64_t h = 1469598103934665603ULL;
        for (uint8_t k = 0; k < n_keys; k++) {
            int64_t v = agg_read_key_i64(key_cols[k], data[k], r);
            h ^= (uint64_t)v; h *= 1099511628211ULL;
        }
        uint64_t slot = h & mask;
        for (;;) {
            int32_t gptr = ht_gid[slot];
            if (gptr < 0) {                          /* empty slot → new group */
                ht_gid[slot] = (int32_t)ngroups;
                out->first_row[ngroups] = r;
                out->gids[r] = (uint32_t)ngroups;
                ngroups++;
                break;
            }
            int64_t fr = out->first_row[gptr];
            int eq = 1;
            for (uint8_t k = 0; k < n_keys; k++) {
                if (agg_read_key_i64(key_cols[k], data[k], r) !=
                    agg_read_key_i64(key_cols[k], data[k], fr)) { eq = 0; break; }
            }
            if (eq) { out->gids[r] = (uint32_t)gptr; break; }
            slot = (slot + 1) & mask;                /* linear probe */
        }
    }
    out->ngroups = ngroups;
    free(ht_gid);
    return 0;
}
