/* src/ops/agg_engine.c — v2 aggregation engine. */
#include "ops/agg_engine.h"
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include "ops/internal.h"  /* col_vec_new, col_esz */
#include "lang/internal.h" /* sym_domain_rep */
#include "table/sym.h"    /* ray_read_sym */
#include <stdlib.h>
#include <string.h>

bool ray_agg_engine_v2 = true;   /* knob; default on */

/* Read element `row` of an integer/temporal/SYM column widened to int64. */
static inline int64_t agg_read_key_i64(ray_t* col, const void* data, int64_t row);
/* Write a finalized scalar cell into output column slot i, marking nulls. */
static void agg_put_cell(ray_t* out, int64_t i, ray_t* cell);
/* Dense direct-index serial grouping; defined below agg_run_one. */
static int agg_group_keys_dense(ray_t** key_cols, int64_t nrows,
                                const dense_plan_t* dp, agg_groups_t* out);
/* Binary-aggregate (pearson) serial driver; defined below agg_run_one. */
ray_t* agg_run_one_bin(const agg_vtable_t* vt, ray_t* x_col, ray_t* y_col,
                       const uint32_t* gids, int64_t nrows, int64_t ngroups,
                       int64_t kparam);

bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return false;
    if (ext->n_keys < 1 || ext->n_keys > 16) return false;  /* 1b: 1..16 keys */
    if (ext->n_aggs == 0) return false;        /* need >=1 aggregate  */
    if (!tbl) return false;
    if (g->selection) return false;            /* no active/pushed filter */

    /* Factorized-EXPAND result (synthetic _src key + _count weight column) needs
     * exec_group's dedicated factorized handling (COUNT/SUM(_count) weight by
     * _count). v2 would compute a plain count — defer the whole shape. */
    if (ext->n_keys == 1 && ext->keys[0] && ext->keys[0]->opcode == OP_SCAN) {
        ray_op_ext_t* ke = find_ext(g, ext->keys[0]->id);
        if (ke && ke->sym == ray_sym_intern("_src", 4)) {
            ray_t* cnt = ray_table_get_col(tbl, ray_sym_intern("_count", 6));
            if (cnt && cnt->type == RAY_I64) return false;
        }
    }

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
        if (ext->agg_k && ext->agg_k[a]) {
            if (ext->agg_ops[a] != OP_TOP_N && ext->agg_ops[a] != OP_BOT_N) return false;
            if (ext->agg_k[a] < 1) return false;
            ray_op_t* in = ext->agg_ins[a];
            if (!in || in->opcode != OP_SCAN) return false;
            ray_op_ext_t* ie = find_ext(g, in->id);
            ray_t* ic = ie ? ray_table_get_col(tbl, ie->sym) : NULL;
            if (!ic || !agg_resolve(ext->agg_ops[a], ic->type)) return false;
            continue;  /* admitted */
        }
        if (ext->agg_ins2 && ext->agg_ins2[a]) {
            if (ext->agg_ops[a] != OP_PEARSON_CORR) return false; /* only pearson in 2b */
            ray_op_t* xin = ext->agg_ins[a]; ray_op_t* yin = ext->agg_ins2[a];
            if (!xin || xin->opcode != OP_SCAN || !yin || yin->opcode != OP_SCAN) return false;
            ray_op_ext_t* xe = find_ext(g, xin->id); ray_op_ext_t* ye = find_ext(g, yin->id);
            ray_t* xc = xe ? ray_table_get_col(tbl, xe->sym) : NULL;
            ray_t* yc = ye ? ray_table_get_col(tbl, ye->sym) : NULL;
            /* pearson reads x/y per type (pearson_read_f64): admit any numeric/
             * temporal pair, not just F64.  agg_resolve gates the exact set. */
            if (!xc || !yc) return false;
            if (!agg_resolve(OP_PEARSON_CORR, xc->type)) return false;
            if (!agg_resolve(OP_PEARSON_CORR, yc->type)) return false;
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

/* ── Dense grouping eligibility selector (mirrors group.c DA path) ────────
 * Decides whether the key tuple packs into a bounded direct-index slot space
 * (gid = sum_k (key_k - min_k)*strides[k]) so grouping can skip hashing.
 * Eligible iff: 1..16 keys, every agg is ACC_STREAMING, every key is an
 * integer/temporal/SYM type with no nulls, and the product of per-key ranges
 * stays at or below DENSE_MAX_SLOTS (computed overflow-safely).  Performs one
 * min/max prescan per key column. */
bool agg_dense_plan(ray_t** key_cols, uint8_t n_keys,
                    const agg_vtable_t** vts, uint8_t n_aggs,
                    int64_t nrows, dense_plan_t* out) {
    out->ok = false;
    out->n_keys = n_keys;
    if (n_keys < 1 || n_keys > 16) return false;

    /* Buffered aggregates (median/top-k) can't run on the dense path → defer. */
    for (uint8_t a = 0; a < n_aggs; a++)
        if (vts[a]->kind != ACC_STREAMING) return false;

    /* Per-key type check + min/max prescan. */
    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* kc = key_cols[k];
        switch (kc->type) {
            case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
            case RAY_BOOL: case RAY_DATE: case RAY_TIME:
            case RAY_TIMESTAMP: case RAY_SYM: break;
            default: return false;
        }
        if (kc->attrs & RAY_ATTR_HAS_NULLS) return false;
        if (nrows <= 0) return false;   /* empty → no min/max, max<min guard */

        const void* data = ray_data(kc);
        int64_t mn = agg_read_key_i64(kc, data, 0);
        int64_t mx = mn;
        for (int64_t r = 1; r < nrows; r++) {
            int64_t v = agg_read_key_i64(kc, data, r);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        if (mx < mn) return false;       /* no live values */
        out->mins[k]   = mn;
        out->ranges[k] = mx - mn + 1;
    }

    /* Composite packing; bail if a range is non-positive or the product would
     * exceed the cap.  Overflow-safe: check before multiplying. */
    int64_t total = 1;
    for (uint8_t k = 0; k < n_keys; k++) {
        int64_t rng = out->ranges[k];
        if (rng <= 0) return false;
        if (total > DENSE_MAX_SLOTS / rng) return false;   /* would exceed cap */
        out->strides[k] = total;
        total *= rng;
    }
    if (total > DENSE_MAX_SLOTS) return false;

    out->total_slots = total;
    out->ok = true;
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

/* Run vt->destroy on every init'd per-group state in `loc` for buffered
 * accumulators (destroy != NULL), then free the slab.  Safe to call once a
 * local/global table's groups have been merged/finalized: streaming accs
 * (destroy == NULL) are skipped, and the call is idempotent on the slab via
 * agg_local_destroy nulling the pointers (but must not be invoked twice on the
 * same live buffered state — each caller below destroys a given table once). */
static void agg_table_destroy(agg_local_t* loc, const agg_vtable_t** vts,
                              const size_t* off, size_t block, uint8_t n_aggs) {
    for (uint8_t a = 0; a < n_aggs; a++)
        if (vts[a]->destroy)
            for (int64_t gg = 0; gg < loc->ng; gg++)
                vts[a]->destroy(loc->states + (size_t)gg * block + off[a]);
    agg_local_destroy(loc);
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
static int agg_fr_pair_cmp_fallback(const void* x, const void* y) {
    int64_t a = ((const agg_fr_pair_t*)x)->fr, b = ((const agg_fr_pair_t*)y)->fr;
    return (a > b) - (a < b);
}

/* Sort pairs by .fr ascending.  .fr values are distinct row indices in
 * [0, nrows), so an LSD byte-wise radix sort is O(n) and produces the same
 * total order as a comparison qsort on .fr — no tie-break needed (distinct).
 * Stable per digit; ping-pongs between `a` and a scratch buffer.  On OOM it
 * falls back to the comparison sort (correctness preserved). */
static void agg_sort_pairs_by_fr(agg_fr_pair_t* a, int64_t n) {
    if (n < 2) return;
    /* Number of 8-bit passes needed to cover the largest fr value. */
    int64_t maxfr = 0;
    for (int64_t i = 0; i < n; i++) if (a[i].fr > maxfr) maxfr = a[i].fr;
    int passes = 1;
    while ((uint64_t)maxfr >> (8 * passes)) passes++;

    agg_fr_pair_t* tmp = malloc((size_t)n * sizeof(agg_fr_pair_t));
    if (!tmp) {
        qsort(a, (size_t)n, sizeof(agg_fr_pair_t), agg_fr_pair_cmp_fallback);
        return;
    }
    agg_fr_pair_t* src = a;
    agg_fr_pair_t* dst = tmp;
    for (int p = 0; p < passes; p++) {
        int64_t count[256] = {0};
        int shift = 8 * p;
        for (int64_t i = 0; i < n; i++) count[(src[i].fr >> shift) & 0xFF]++;
        int64_t sum = 0;
        for (int b = 0; b < 256; b++) { int64_t c = count[b]; count[b] = sum; sum += c; }
        for (int64_t i = 0; i < n; i++) dst[count[(src[i].fr >> shift) & 0xFF]++] = src[i];
        agg_fr_pair_t* t = src; src = dst; dst = t;
    }
    /* If sorted result ended up in tmp (odd passes), copy back to `a`. */
    if (src != a) memcpy(a, src, (size_t)n * sizeof(agg_fr_pair_t));
    free(tmp);
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
            /* Phase A ran: workers may hold init'd buffered group states. */
            for (uint32_t i = 0; i < nw; i++)
                agg_table_destroy(&locals[i], vts, off, block, n_aggs);
            free(locals);
            return ray_error("oom", NULL);
        }

    /* ── Phase B: merge per-worker locals into a global table (serial) ── */
    agg_local_t gt = {0};
    if (agg_local_init(&gt, cap, htcap, (int64_t)block) != 0) {
        agg_local_destroy(&gt);   /* gt init failed: no init'd states in gt */
        for (uint32_t i = 0; i < nw; i++)
            agg_table_destroy(&locals[i], vts, off, block, n_aggs);
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

    /* Phase A locals no longer needed past the merge.  Destroy each init'd local
     * per-group state for buffered accumulators (merge copied values into gt) so
     * the local buffers don't leak; do it BEFORE freeing the slabs.  After this
     * point, only gt holds live buffered state — every later path (success +
     * error) destroys gt exactly once and need not touch locals again. */
    for (uint32_t i = 0; i < nw; i++) {
        for (uint8_t a = 0; a < n_aggs; a++)
            if (vts[a]->destroy)
                for (int64_t lg = 0; lg < locals[i].ng; lg++)
                    vts[a]->destroy(locals[i].states + (size_t)lg * block + off[a]);
        agg_local_destroy(&locals[i]);
    }
    free(locals);

    /* ── Phase C: order by first_row, emit key + agg columns ── */
    int64_t* order = malloc((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    int64_t* first_row_ordered = malloc((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    if (!order || !first_row_ordered) {
        free(order); free(first_row_ordered);
        agg_table_destroy(&gt, vts, off, block, n_aggs);
        return ray_error("oom", NULL);
    }
    /* Order group indices by first_row ascending.  first_row is distinct across
     * groups → total order, so the sort is unambiguously deterministic and gives
     * the same first-occurrence emit order as the serial path. */
    {
        agg_fr_pair_t* pairs = malloc((size_t)(ng > 0 ? ng : 1) * sizeof(agg_fr_pair_t));
        if (!pairs) {
            free(order); free(first_row_ordered);
            agg_table_destroy(&gt, vts, off, block, n_aggs);
            return ray_error("oom", NULL);
        }
        for (int64_t i = 0; i < ng; i++) { pairs[i].fr = gt.first_row[i]; pairs[i].idx = i; }
        agg_sort_pairs_by_fr(pairs, ng);
        for (int64_t i = 0; i < ng; i++) {
            order[i] = pairs[i].idx;
            first_row_ordered[i] = pairs[i].fr;
        }
        free(pairs);
    }

    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        free(order); free(first_row_ordered);
        agg_table_destroy(&gt, vts, off, block, n_aggs);
        return result ? result : ray_error("oom", NULL);
    }

    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* kc = agg_gather_key_col(key_cols[k], first_row_ordered, ng);
        if (!kc || RAY_IS_ERR(kc)) {
            free(order); free(first_row_ordered);
            agg_table_destroy(&gt, vts, off, block, n_aggs);
            ray_release(result); return kc ? kc : ray_error("oom", NULL);
        }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }

    for (uint8_t a = 0; a < n_aggs; a++) {
        bool is_list = (vts[a]->out_type == RAY_LIST);
        ray_t* out = is_list ? ray_list_new(ng) : ray_vec_new(vts[a]->out_type, ng);
        if (!out || RAY_IS_ERR(out)) {
            free(order); free(first_row_ordered);
            agg_table_destroy(&gt, vts, off, block, n_aggs);
            ray_release(result); return out ? out : ray_error("oom", NULL);
        }
        out->len = ng;
        int64_t kparam = (ext->agg_k ? ext->agg_k[a] : 0);
        for (int64_t i = 0; i < ng; i++) {
            ray_t* cell = vts[a]->finalize(gt.states + (size_t)order[i] * block + off[a], NULL, kparam);
            if (is_list) {
                out = ray_list_set(out, i, cell);   /* retains cell */
                ray_release(cell);                  /* drop our local ref */
            } else {
                agg_put_cell(out, i, cell);
                ray_release(cell);
            }
        }
        int64_t agg_name = agg_result_col_name(agg_syms[a], ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, out);
        ray_release(out);
    }

    free(order); free(first_row_ordered);
    agg_table_destroy(&gt, vts, off, block, n_aggs);
    return result;
}

/* ══════════════════════════════════════════════════════════════════════
 * Parallel DENSE group-by (low-card int/SYM keys, ACC_STREAMING aggs only).
 * Per-worker flat slabs of `total_slots` AoS group states (slot == gid via the
 * mixed-radix packing) + a per-worker first_row[slot] (INT64_MAX = untouched).
 * Phase A: each worker packs its chunk rows into slots and update_batch's into
 * its own slab.  Phase B: merge per-worker slabs into a global slab.  Phase C:
 * collect occupied slots, order by global first_row (first-occurrence), emit.
 * No hashing, no buffered state, no destroy lifecycle (streaming guaranteed). */

/* Per-worker dense slab. */
typedef struct {
    char*    states;     /* [total_slots * block] AoS, every slot init'd */
    int64_t* first_row;  /* [total_slots] min row idx touching slot, INT64_MAX = none */
    int      oom;
} agg_dense_local_t;

typedef struct {
    ray_t**             key_cols;
    const void**        key_data;
    uint8_t             n_keys;
    const dense_plan_t* dp;
    const agg_vtable_t** vts;
    const size_t*       off;
    size_t              block;
    uint8_t             n_aggs;
    const void**        val_data; const int8_t* val_types; const bool* val_hasnull; const uint8_t* val_esz;
    const void**        val2_data; const int8_t* val2_types; const bool* val2_hasnull; const uint8_t* val2_esz;
    agg_dense_local_t*  locals;   /* [nw] */
} agg_dense_ctx_t;

/* Initialize every slot's agg states in a freshly-allocated slab (min/max need
 * INT64_MAX/MIN seeds, NOT calloc-zero), and first_row to INT64_MAX. */
static int agg_dense_local_init(agg_dense_local_t* loc, int64_t total_slots,
                                const agg_vtable_t** vts, const size_t* off,
                                size_t block, uint8_t n_aggs) {
    loc->oom = 0;
    loc->states    = malloc((size_t)total_slots * block);
    loc->first_row = malloc((size_t)total_slots * sizeof(int64_t));
    if (!loc->states || !loc->first_row) { loc->oom = 1; return -1; }
    for (int64_t s = 0; s < total_slots; s++) {
        loc->first_row[s] = INT64_MAX;
        for (uint8_t a = 0; a < n_aggs; a++)
            vts[a]->init(loc->states + (size_t)s * block + off[a]);
    }
    return 0;
}

static void agg_dense_local_destroy(agg_dense_local_t* loc) {
    free(loc->states); free(loc->first_row);
    loc->states = NULL; loc->first_row = NULL;
}

/* Compute the dense slot for row r via mixed-radix packing. */
static inline int64_t agg_dense_slot(const agg_dense_ctx_t* c, int64_t r) {
    int64_t slot = 0;
    for (uint8_t k = 0; k < c->n_keys; k++)
        slot += (agg_read_key_i64(c->key_cols[k], c->key_data[k], r) - c->dp->mins[k]) * c->dp->strides[k];
    return slot;
}

/* Phase A: per-worker dense accumulate over chunk [start,end). */
static void agg_dense_phaseA_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_dense_ctx_t* c = (agg_dense_ctx_t*)vctx;
    agg_dense_local_t* loc = &c->locals[wid];
    if (loc->oom) return;
    int64_t n = end - start;
    if (n <= 0) return;

    uint32_t* cgid = malloc((size_t)n * sizeof(uint32_t));
    if (!cgid) { loc->oom = 1; return; }

    for (int64_t r = start; r < end; r++) {
        int64_t slot = agg_dense_slot(c, r);   /* provably in [0,total_slots) */
        cgid[r - start] = (uint32_t)slot;
        if (r < loc->first_row[slot]) loc->first_row[slot] = r;
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

/* Parallel dense path.  Precondition: dp->ok, all aggs ACC_STREAMING, per-worker
 * budget already gated by the caller. */
static ray_t* exec_group_v2_parallel_dense(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl,
        ray_t** key_cols, int64_t* key_syms, ray_op_ext_t* ext, int64_t nrows,
        ray_pool_t* pool, const dense_plan_t* dp) {
    uint8_t n_keys = ext->n_keys, n_aggs = ext->n_aggs;
    int64_t total_slots = dp->total_slots;
    uint32_t nw = ray_pool_total_workers(pool);

    /* Re-derive the AoS layout (same order as exec_group_v2). */
    const agg_vtable_t* vts[16]; size_t off[16]; size_t block = 0;
    for (uint8_t a = 0; a < n_aggs; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]->id);
        ray_t* vc = (ext->agg_ops[a] != OP_COUNT) ? ray_table_get_col(tbl, ie->sym) : NULL;
        int8_t in_type = vc ? vc->type : RAY_I64;
        vts[a] = agg_resolve(ext->agg_ops[a], in_type);
        off[a] = block;
        block += vts[a]->state_size;
    }

    const void* key_data[16];
    for (uint8_t k = 0; k < n_keys; k++) key_data[k] = ray_data(key_cols[k]);

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
        ray_t* vc2 = (ext->agg_ins2 && ext->agg_ins2[a])
            ? ray_table_get_col(tbl, find_ext(g, ext->agg_ins2[a]->id)->sym) : NULL;
        val2_data[a]    = vc2 ? ray_data(vc2) : NULL;
        val2_types[a]   = vc2 ? vc2->type : RAY_I64;
        val2_hasnull[a] = vc2 ? ((vc2->attrs & RAY_ATTR_HAS_NULLS) != 0) : false;
        val2_esz[a]     = vc2 ? col_esz(vc2) : 0;
    }

    agg_dense_local_t* locals = calloc((size_t)nw, sizeof(agg_dense_local_t));
    if (!locals) return ray_error("oom", NULL);
    int alloc_oom = 0;
    for (uint32_t w = 0; w < nw; w++)
        if (agg_dense_local_init(&locals[w], total_slots, vts, off, block, n_aggs) != 0) { alloc_oom = 1; break; }
    if (alloc_oom) {
        for (uint32_t w = 0; w < nw; w++) agg_dense_local_destroy(&locals[w]);
        free(locals);
        return ray_error("oom", NULL);
    }

    agg_dense_ctx_t ctx = {
        .key_cols = key_cols, .key_data = key_data, .n_keys = n_keys, .dp = dp,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs,
        .val_data = val_data, .val_types = val_types, .val_hasnull = val_hasnull, .val_esz = val_esz,
        .val2_data = val2_data, .val2_types = val2_types, .val2_hasnull = val2_hasnull, .val2_esz = val2_esz,
        .locals = locals,
    };
    ray_pool_dispatch(pool, agg_dense_phaseA_fn, &ctx, nrows);

    for (uint32_t w = 0; w < nw; w++)
        if (locals[w].oom) {
            for (uint32_t i = 0; i < nw; i++) agg_dense_local_destroy(&locals[i]);
            free(locals);
            return ray_error("oom", NULL);
        }

    /* ── Phase B: merge per-worker slabs into a global slab (serial) ── */
    char*    gstates  = malloc((size_t)total_slots * block);
    int64_t* gfirst   = malloc((size_t)total_slots * sizeof(int64_t));
    if (!gstates || !gfirst) {
        free(gstates); free(gfirst);
        for (uint32_t i = 0; i < nw; i++) agg_dense_local_destroy(&locals[i]);
        free(locals);
        return ray_error("oom", NULL);
    }
    for (int64_t s = 0; s < total_slots; s++) {
        gfirst[s] = INT64_MAX;
        for (uint8_t a = 0; a < n_aggs; a++)
            vts[a]->init(gstates + (size_t)s * block + off[a]);
    }
    for (uint32_t w = 0; w < nw; w++) {
        agg_dense_local_t* loc = &locals[w];
        for (int64_t s = 0; s < total_slots; s++) {
            if (loc->first_row[s] == INT64_MAX) continue;   /* untouched by this worker */
            if (loc->first_row[s] < gfirst[s]) gfirst[s] = loc->first_row[s];
            for (uint8_t a = 0; a < n_aggs; a++)
                vts[a]->merge(gstates + (size_t)s * block + off[a],
                              loc->states + (size_t)s * block + off[a], NULL);
        }
    }
    for (uint32_t i = 0; i < nw; i++) agg_dense_local_destroy(&locals[i]);
    free(locals);

    /* ── Phase C: collect occupied slots, order by first_row (first-occurrence). ── */
    int64_t ng = 0;
    for (int64_t s = 0; s < total_slots; s++) if (gfirst[s] != INT64_MAX) ng++;

    int64_t* occupied_slot     = malloc((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    int64_t* first_row_ordered = malloc((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    agg_fr_pair_t* pairs       = malloc((size_t)(ng > 0 ? ng : 1) * sizeof(agg_fr_pair_t));
    if (!occupied_slot || !first_row_ordered || !pairs) {
        free(occupied_slot); free(first_row_ordered); free(pairs);
        free(gstates); free(gfirst);
        return ray_error("oom", NULL);
    }
    { int64_t i = 0;
      for (int64_t s = 0; s < total_slots; s++)
          if (gfirst[s] != INT64_MAX) { pairs[i].fr = gfirst[s]; pairs[i].idx = s; i++; }
    }
    agg_sort_pairs_by_fr(pairs, ng);
    for (int64_t i = 0; i < ng; i++) {
        occupied_slot[i]     = pairs[i].idx;
        first_row_ordered[i] = pairs[i].fr;
    }
    free(pairs);

    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        free(occupied_slot); free(first_row_ordered); free(gstates); free(gfirst);
        return result ? result : ray_error("oom", NULL);
    }

    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* kc = agg_gather_key_col(key_cols[k], first_row_ordered, ng);
        if (!kc || RAY_IS_ERR(kc)) {
            free(occupied_slot); free(first_row_ordered); free(gstates); free(gfirst);
            ray_release(result); return kc ? kc : ray_error("oom", NULL);
        }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }

    for (uint8_t a = 0; a < n_aggs; a++) {
        ray_t* out = ray_vec_new(vts[a]->out_type, ng);   /* streaming → scalar out_type */
        if (!out || RAY_IS_ERR(out)) {
            free(occupied_slot); free(first_row_ordered); free(gstates); free(gfirst);
            ray_release(result); return out ? out : ray_error("oom", NULL);
        }
        out->len = ng;
        int64_t kparam = (ext->agg_k ? ext->agg_k[a] : 0);
        for (int64_t i = 0; i < ng; i++) {
            ray_t* cell = vts[a]->finalize(gstates + (size_t)occupied_slot[i] * block + off[a], NULL, kparam);
            agg_put_cell(out, i, cell);
            ray_release(cell);
        }
        int64_t agg_name = agg_result_col_name(agg_syms[a], ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, out);
        ray_release(out);
    }

    free(occupied_slot); free(first_row_ordered); free(gstates); free(gfirst);
    return result;
}

/* ══════════════════════════════════════════════════════════════════════
 * Parallel RADIX group-by (high-card int/SYM keys, ACC_STREAMING aggs only).
 *
 * Partition rows by key-hash into AGG_RADIX_P disjoint partitions: every row
 * whose tuple-hash maps to partition p lands in p across ALL workers (same key
 * → same hash → same partition).  Partitions therefore hold DISJOINT key sets,
 * so each can be grouped+accumulated fully in parallel with NO cross-partition
 * merge — eliminating the serial Phase-B bottleneck of the hash path.
 *
 *   Phase 1 (parallel over rows): scatter each row's INDEX into a per-(worker,
 *            partition) growable int64 buffer keyed by RADIX_PART(hash).
 *   Phase 2 (parallel over partitions): gather all workers' index buffers for
 *            partition p; build a small open-addressing hash over those rows
 *            (keys disjoint from other partitions); assign partition-local gids
 *            (first_row = MIN row), init each agg state on first key sight,
 *            batch-update each agg from a gathered value column.
 *   Phase 3 (serial): ngroups = Σ partition group counts; collect every group's
 *            first_row globally, qsort ascending (first-occurrence — partitions
 *            are hash-ordered so a GLOBAL sort is required), emit key + agg
 *            columns in that order, assemble {key cols, agg cols}.
 *
 * Streaming aggs only (selector guarantees it) → no buffered destroy lifecycle.
 * ══════════════════════════════════════════════════════════════════════ */

#define AGG_RADIX_P 256

/* Growable contiguous PAYLOAD buffer (per worker, per partition).  Phase 1
 * scatters one fixed-size record per row instead of a bare row index: the
 * record packs [n_keys×int64 widened keys][agg input value(s) at native esz]
 * [row_idx int64].  Phase 2 then groups+accumulates each partition by walking
 * its records SEQUENTIALLY — hash/equality compare the contiguous packed keys
 * and accumulation reads values from the contiguous record — trading a one-time
 * scatter cost for cache-friendly Phase-2 reads (the memory-bound hot spot was
 * agg_tuple_eq re-reading scattered key columns on every distinct insert). */
typedef struct { char* buf; uint32_t n, cap; } agg_pay_buf_t;  /* n = #records */

/* Reserve room for one more record of `rec` bytes; returns dest ptr or NULL. */
static char* agg_pay_reserve(agg_pay_buf_t* b, size_t rec) {
    if (b->n == b->cap) {
        uint32_t nc = b->cap ? b->cap * 2 : 64;
        char* nb = realloc(b->buf, (size_t)nc * rec);
        if (!nb) return NULL;
        b->buf = nb; b->cap = nc;
    }
    return b->buf + (size_t)b->n++ * rec;
}

/* Per-partition result slot (filled by Phase 2). */
typedef struct {
    char*    states;     /* [ng * block] AoS group state (every group init'd) */
    int64_t* first_row;  /* [ng] MIN row idx per partition-local group */
    int64_t  ng;
    int      oom;
} agg_radix_part_t;

typedef struct {
    ray_t**             key_cols;
    const void**        key_data;
    uint8_t             n_keys;
    const agg_vtable_t** vts;
    const size_t*       off;
    size_t              block;
    uint8_t             n_aggs;
    const void**        val_data; const int8_t* val_types; const bool* val_hasnull; const uint8_t* val_esz;
    const void**        val2_data; const int8_t* val2_types; const bool* val2_hasnull; const uint8_t* val2_esz;
    uint32_t            nw;
    agg_pay_buf_t*      bufs;       /* [nw * AGG_RADIX_P] payload records */
    agg_radix_part_t*   parts;      /* [AGG_RADIX_P] */
    int                 phase1_oom; /* set by any Phase-1 worker on push failure */
    /* Per-row payload record layout (bytes), computed once by the caller:
     *   [0 .. n_keys*8)         packed keys (each widened via agg_read_key_i64)
     *   [val_off[a] ..]         agg a's input value at native esz (if val_data[a])
     *   [val2_off[a] ..]        agg a's 2nd input (pearson) at native esz
     *   [row_off ..]            int64 source row index (for first_row) */
    size_t              rec;        /* total record size, 8-aligned */
    size_t              val_off[16];
    size_t              val2_off[16];
    size_t              row_off;
} agg_radix_ctx_t;

/* Phase 1: scatter one packed payload record per row into per-(worker,
 * partition) contiguous buffers, keyed by RADIX_PART(tuple hash). */
static void agg_radix_scatter_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    agg_pay_buf_t* my = &c->bufs[(size_t)wid * AGG_RADIX_P];
    uint8_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    for (int64_t r = start; r < end; r++) {
        /* Widen keys once; reuse for both the partition hash and the packed
         * record so Phase 2 never re-reads the source key columns. */
        int64_t* kdst;
        uint64_t h = 1469598103934665603ULL;
        uint32_t p;
        {
            int64_t kv[16];
            for (uint8_t k = 0; k < n_keys; k++) {
                int64_t v = agg_read_key_i64(c->key_cols[k], c->key_data[k], r);
                kv[k] = v;
                h ^= (uint64_t)v; h *= 1099511628211ULL;
            }
            p = (uint32_t)(h & (AGG_RADIX_P - 1));
            char* rec = agg_pay_reserve(&my[p], c->rec);
            if (!rec) { c->phase1_oom = 1; return; }
            kdst = (int64_t*)rec;
            for (uint8_t k = 0; k < n_keys; k++) kdst[k] = kv[k];
            for (uint8_t a = 0; a < n_aggs; a++) {
                if (c->val_data[a]) {
                    uint8_t ez = c->val_esz[a];
                    memcpy(rec + c->val_off[a],
                           (const char*)c->val_data[a] + (size_t)r * ez, ez);
                }
                if (c->val2_data[a]) {
                    uint8_t ez2 = c->val2_esz[a];
                    memcpy(rec + c->val2_off[a],
                           (const char*)c->val2_data[a] + (size_t)r * ez2, ez2);
                }
            }
            *(int64_t*)(rec + c->row_off) = r;
        }
    }
}

/* Phase 2: group + accumulate one partition by walking its packed payload
 * records SEQUENTIALLY.  Keys here are disjoint from every other partition, so
 * a partition-local open-addressing hash suffices.  Hash/equality compare the
 * contiguous packed keys at the head of each record (no scattered key-column
 * re-reads), and each agg's dense value buffer is gathered straight from the
 * records in the same sequential pass.  Keys are disjoint from other
 * partitions so the partition-local open-addressing hash suffices. */
static void agg_radix_group_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    uint8_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    size_t key_bytes = (size_t)n_keys * 8;
    for (int64_t pi = start; pi < end; pi++) {
        uint32_t p = (uint32_t)pi;
        agg_radix_part_t* pr = &c->parts[p];

        /* Total rows hashing to p across all workers. */
        int64_t total = 0;
        for (uint32_t w = 0; w < c->nw; w++)
            total += c->bufs[(size_t)w * AGG_RADIX_P + p].n;
        if (total == 0) continue;

        /* Open-addressing hash sized to next_pow2(2*total). */
        int64_t htcap = 16;
        while (htcap < total * 2) htcap <<= 1;
        uint64_t htmask = (uint64_t)htcap - 1;
        int32_t* ht        = malloc((size_t)htcap * sizeof(int32_t));
        int64_t* first_row = malloc((size_t)total * sizeof(int64_t));
        char*    states    = malloc((size_t)total * c->block);
        const int64_t** keyp = malloc((size_t)total * sizeof(int64_t*)); /* gid -> packed keys */
        uint32_t* gid      = malloc((size_t)total * sizeof(uint32_t));/* row i -> local gid */
        if (!ht || !first_row || !states || !keyp || !gid) {
            free(ht); free(first_row); free(states); free(keyp); free(gid);
            pr->oom = 1; return;
        }
        for (int64_t i = 0; i < htcap; i++) ht[i] = -1;

        /* Per-agg dense value buffers, gathered contiguously from the records. */
        char* gv[16] = {0}; char* gy[16] = {0};
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (c->val_data[a]) {
                uint8_t ez = c->val_esz[a];
                gv[a] = malloc((size_t)total * (ez ? ez : 1));
                if (!gv[a]) goto oom;
            }
            if (c->val2_data[a]) {
                uint8_t ez2 = c->val2_esz[a];
                gy[a] = malloc((size_t)total * (ez2 ? ez2 : 1));
                if (!gy[a]) goto oom;
            }
        }

        int64_t ng = 0, ri = 0;
        for (uint32_t w = 0; w < c->nw; w++) {
            agg_pay_buf_t* b = &c->bufs[(size_t)w * AGG_RADIX_P + p];
            const char* rec = b->buf;
            for (uint32_t i = 0; i < b->n; i++, rec += c->rec) {
                const int64_t* keys = (const int64_t*)rec;
                int64_t r = *(const int64_t*)(rec + c->row_off);
                /* Gather this row's agg values into the dense per-agg buffers
                 * (sequential record read → sequential dense write). */
                for (uint8_t a = 0; a < n_aggs; a++) {
                    if (gv[a]) { uint8_t ez = c->val_esz[a];
                        memcpy(gv[a] + (size_t)ri * ez, rec + c->val_off[a], ez); }
                    if (gy[a]) { uint8_t ez2 = c->val2_esz[a];
                        memcpy(gy[a] + (size_t)ri * ez2, rec + c->val2_off[a], ez2); }
                }
                /* Hash the contiguous packed keys (same FNV-1a as the scatter). */
                uint64_t h = 1469598103934665603ULL;
                for (uint8_t k = 0; k < n_keys; k++) { h ^= (uint64_t)keys[k]; h *= 1099511628211ULL; }
                uint64_t slot = h & htmask;
                int32_t gg;
                for (;;) {
                    int32_t gp = ht[slot];
                    if (gp < 0) {                       /* new group */
                        gg = (int32_t)ng;
                        ht[slot] = gg;
                        first_row[gg] = r;
                        keyp[gg] = keys;
                        for (uint8_t a = 0; a < n_aggs; a++)
                            c->vts[a]->init(states + (size_t)gg * c->block + c->off[a]);
                        ng++;
                        break;
                    }
                    if (memcmp(keys, keyp[gp], key_bytes) == 0) {  /* contiguous key compare */
                        gg = gp;
                        if (r < first_row[gg]) first_row[gg] = r;   /* MIN */
                        break;
                    }
                    slot = (slot + 1) & htmask;
                }
                gid[ri] = (uint32_t)gg;
                ri++;
            }
        }
        free(ht); free(keyp);

        /* Batch-accumulate each agg over the partition's dense value buffers. */
        for (uint8_t a = 0; a < n_aggs; a++) {
            if (c->vts[a]->update_batch2) {             /* binary agg (pearson) */
                ray_valid_t vx = { gv[a], c->val_types[a],  c->val_hasnull[a] };
                ray_valid_t vy = { gy[a], c->val2_types[a], c->val2_hasnull[a] };
                c->vts[a]->update_batch2(states + c->off[a], c->block, gid,
                                         gv[a], gy[a], &vx, &vy, total, NULL);
            } else if (c->val_data[a]) {                /* unary agg over a column */
                ray_valid_t valid = { gv[a], c->val_types[a], c->val_hasnull[a] };
                c->vts[a]->update_batch(states + c->off[a], c->block, gid, gv[a], &valid, total, NULL);
            } else {                                    /* COUNT (no value column) */
                ray_valid_t valid = { NULL, c->val_types[a], false };
                c->vts[a]->update_batch(states + c->off[a], c->block, gid, NULL, &valid, total, NULL);
            }
        }

        for (uint8_t a = 0; a < n_aggs; a++) { free(gv[a]); free(gy[a]); }
        free(gid);
        pr->states = states; pr->first_row = first_row; pr->ng = ng;
        continue;
    oom:
        free(ht); free(first_row); free(states); free(keyp); free(gid);
        for (uint8_t a = 0; a < n_aggs; a++) { free(gv[a]); free(gy[a]); }
        pr->oom = 1; return;
    }
}

/* Parallel radix path.  Precondition: all aggs ACC_STREAMING, keys int/SYM and
 * non-null (selector-gated).  vts/off/block resolved by caller. */
static ray_t* exec_group_v2_parallel_radix(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
        ray_t** key_cols, int64_t* key_syms,
        const agg_vtable_t** vts, const size_t* off, size_t block) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    uint8_t n_keys = ext->n_keys, n_aggs = ext->n_aggs;
    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = ray_pool_total_workers(pool);

    const void* key_data[16];
    for (uint8_t k = 0; k < n_keys; k++) key_data[k] = ray_data(key_cols[k]);

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
        ray_t* vc2 = (ext->agg_ins2 && ext->agg_ins2[a])
            ? ray_table_get_col(tbl, find_ext(g, ext->agg_ins2[a]->id)->sym) : NULL;
        val2_data[a]    = vc2 ? ray_data(vc2) : NULL;
        val2_types[a]   = vc2 ? vc2->type : RAY_I64;
        val2_hasnull[a] = vc2 ? ((vc2->attrs & RAY_ATTR_HAS_NULLS) != 0) : false;
        val2_esz[a]     = vc2 ? col_esz(vc2) : 0;
    }

    /* Per-row payload record layout: [packed keys][agg values][row_idx]. */
    size_t val_off[16] = {0}, val2_off[16] = {0};
    size_t rec_cur = (size_t)n_keys * 8;
    for (uint8_t a = 0; a < n_aggs; a++) {
        if (val_data[a])  { val_off[a]  = rec_cur; rec_cur += val_esz[a]; }
        if (val2_data[a]) { val2_off[a] = rec_cur; rec_cur += val2_esz[a]; }
    }
    size_t row_off = rec_cur; rec_cur += 8;
    size_t rec = (rec_cur + 7u) & ~(size_t)7u;   /* 8-align records */

    size_t nbuf = (size_t)nw * AGG_RADIX_P;
    agg_pay_buf_t*    bufs  = calloc(nbuf, sizeof(agg_pay_buf_t));
    agg_radix_part_t* parts = calloc(AGG_RADIX_P, sizeof(agg_radix_part_t));
    if (!bufs || !parts) { free(bufs); free(parts); return ray_error("oom", NULL); }

    agg_radix_ctx_t ctx = {
        .key_cols = key_cols, .key_data = key_data, .n_keys = n_keys,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs,
        .val_data = val_data, .val_types = val_types, .val_hasnull = val_hasnull, .val_esz = val_esz,
        .val2_data = val2_data, .val2_types = val2_types, .val2_hasnull = val2_hasnull, .val2_esz = val2_esz,
        .nw = nw, .bufs = bufs, .parts = parts, .phase1_oom = 0,
        .rec = rec, .row_off = row_off,
    };
    memcpy(ctx.val_off, val_off, sizeof(val_off));
    memcpy(ctx.val2_off, val2_off, sizeof(val2_off));

    /* Phase 1: scatter. */
    ray_pool_dispatch(pool, agg_radix_scatter_fn, &ctx, nrows);

    /* Phase 2: per-partition group+accumulate. */
    int oom = ctx.phase1_oom;
    if (!oom)
        ray_pool_dispatch_n(pool, agg_radix_group_fn, &ctx, AGG_RADIX_P);
    if (!oom)
        for (uint32_t p = 0; p < AGG_RADIX_P; p++)
            if (parts[p].oom) { oom = 1; break; }

    if (oom) {
        for (uint32_t p = 0; p < AGG_RADIX_P; p++) { free(parts[p].states); free(parts[p].first_row); }
        for (size_t i = 0; i < nbuf; i++) free(bufs[i].buf);
        free(bufs); free(parts);
        return ray_error("oom", NULL);
    }

    /* Phase 3: global first-occurrence order, emit. */
    int64_t ng = 0;
    for (uint32_t p = 0; p < AGG_RADIX_P; p++) ng += parts[p].ng;

    /* (first_row, part, local gid) collected globally and sorted by first_row.
     * Reuse agg_fr_pair_t with idx encoding (part << 32 | local gid) so a single
     * qsort recovers both the partition and the in-partition state offset. */
    agg_fr_pair_t* pairs = malloc((size_t)(ng > 0 ? ng : 1) * sizeof(agg_fr_pair_t));
    int64_t* first_row_ordered = malloc((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    if (!pairs || !first_row_ordered) {
        free(pairs); free(first_row_ordered);
        for (uint32_t p = 0; p < AGG_RADIX_P; p++) { free(parts[p].states); free(parts[p].first_row); }
        for (size_t i = 0; i < nbuf; i++) free(bufs[i].buf);
        free(bufs); free(parts);
        return ray_error("oom", NULL);
    }
    { int64_t i = 0;
      for (uint32_t p = 0; p < AGG_RADIX_P; p++)
          for (int64_t gg = 0; gg < parts[p].ng; gg++) {
              pairs[i].fr  = parts[p].first_row[gg];
              pairs[i].idx = ((int64_t)p << 32) | (uint32_t)gg;   /* part | gid */
              i++;
          }
    }
    agg_sort_pairs_by_fr(pairs, ng);
    for (int64_t i = 0; i < ng; i++) first_row_ordered[i] = pairs[i].fr;

    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        free(pairs); free(first_row_ordered);
        for (uint32_t p = 0; p < AGG_RADIX_P; p++) { free(parts[p].states); free(parts[p].first_row); }
        for (size_t i = 0; i < nbuf; i++) free(bufs[i].buf);
        free(bufs); free(parts);
        return result ? result : ray_error("oom", NULL);
    }

    for (uint8_t k = 0; k < n_keys; k++) {
        ray_t* kc = agg_gather_key_col(key_cols[k], first_row_ordered, ng);
        if (!kc || RAY_IS_ERR(kc)) {
            free(pairs); free(first_row_ordered);
            for (uint32_t p = 0; p < AGG_RADIX_P; p++) { free(parts[p].states); free(parts[p].first_row); }
            for (size_t i = 0; i < nbuf; i++) free(bufs[i].buf);
            free(bufs); free(parts);
            ray_release(result); return kc ? kc : ray_error("oom", NULL);
        }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }

    for (uint8_t a = 0; a < n_aggs; a++) {
        ray_t* out = ray_vec_new(vts[a]->out_type, ng);   /* streaming → scalar out_type */
        if (!out || RAY_IS_ERR(out)) {
            free(pairs); free(first_row_ordered);
            for (uint32_t p = 0; p < AGG_RADIX_P; p++) { free(parts[p].states); free(parts[p].first_row); }
            for (size_t i = 0; i < nbuf; i++) free(bufs[i].buf);
            free(bufs); free(parts);
            ray_release(result); return out ? out : ray_error("oom", NULL);
        }
        out->len = ng;
        int64_t kparam = (ext->agg_k ? ext->agg_k[a] : 0);
        for (int64_t i = 0; i < ng; i++) {
            uint32_t p  = (uint32_t)(pairs[i].idx >> 32);
            uint32_t gg = (uint32_t)(pairs[i].idx & 0xffffffffu);
            ray_t* cell = vts[a]->finalize(parts[p].states + (size_t)gg * block + off[a], NULL, kparam);
            agg_put_cell(out, i, cell);
            ray_release(cell);
        }
        int64_t agg_name = agg_result_col_name(agg_syms[a], ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, out);
        ray_release(out);
    }

    free(pairs); free(first_row_ordered);
    for (uint32_t p = 0; p < AGG_RADIX_P; p++) { free(parts[p].states); free(parts[p].first_row); }
    for (size_t i = 0; i < nbuf; i++) free(bufs[i].buf);
    free(bufs); free(parts);
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

    /* Dense plan (low-card int/SYM keys + streaming aggs → direct index, no
     * hash).  Computed up front so both the parallel and serial dispatch can use
     * it.  One min/max prescan per key column. */
    dense_plan_t dp;
    bool dense = agg_dense_plan(key_cols, ext->n_keys, vts, ext->n_aggs, nrows, &dp);

    ray_pool_t* pool = ray_pool_get();
    if (pool && nrows >= RAY_PARALLEL_THRESHOLD) {
        /* Per-worker memory budget gate: dense parallel allocates a full
         * total_slots*(block + first_row) slab per worker.  Low-card (the perf
         * target) has tiny total_slots → always within budget → dense parallel.
         * Mid-card-over-budget dense plans fall back to hash parallel (correct). */
        int64_t per_worker_bytes = dp.total_slots * (int64_t)(block + 8 /*first_row*/);
        bool dense_par_ok = dp.ok && per_worker_bytes <= (8LL << 20);  /* 8 MB/worker cap */

        /* RADIX eligibility: every agg streaming + every key an int/SYM type with
         * no nulls (same type-set check as agg_dense_plan).  Radix takes the
         * high-card remainder of streaming int-key queries (dense handles the
         * low-card head); hash handles buffered aggs / F64 / STR keys. */
        bool all_streaming = true;
        for (uint8_t a = 0; a < ext->n_aggs && all_streaming; a++)
            if (vts[a]->kind != ACC_STREAMING) all_streaming = false;
        bool keys_intsym = true;
        for (uint8_t k = 0; k < ext->n_keys && keys_intsym; k++) {
            ray_t* kc = key_cols[k];
            switch (kc->type) {
                case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
                case RAY_BOOL: case RAY_DATE: case RAY_TIME:
                case RAY_TIMESTAMP: case RAY_SYM: break;
                default: keys_intsym = false;
            }
            if (kc->attrs & RAY_ATTR_HAS_NULLS) keys_intsym = false;
        }

        if (dense_par_ok)
            return exec_group_v2_parallel_dense(g, op, tbl, key_cols, key_syms, ext, nrows, pool, &dp);
        if (all_streaming && keys_intsym)   /* high-card streaming int/sym keys */
            return exec_group_v2_parallel_radix(g, op, tbl, nrows, key_cols, key_syms, vts, off, block);
        return exec_group_v2_parallel(g, op, tbl, nrows, key_cols, key_syms, vts, off, block);
    }


    agg_groups_t groups = {0};
    int grp_rc = dense ? agg_group_keys_dense(key_cols, nrows, &dp, &groups)
                       : agg_group_keys(key_cols, ext->n_keys, nrows, &groups);
    if (grp_rc != 0) return ray_error("oom", NULL);

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
        int64_t kparam = (ext->agg_k ? ext->agg_k[a] : 0);
        ray_t* col;
        if (ext->agg_ins2 && ext->agg_ins2[a]) {       /* binary agg (pearson) */
            ray_op_ext_t* ye = find_ext(g, ext->agg_ins2[a]->id);
            ray_t* x_col = ray_table_get_col(tbl, ie->sym);
            ray_t* y_col = ray_table_get_col(tbl, ye->sym);
            const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], x_col->type);
            col = agg_run_one_bin(vt, x_col, y_col, groups.gids, nrows, groups.ngroups, kparam);
        } else {
            ray_t* val_col = (ext->agg_ops[a] != OP_COUNT) ? ray_table_get_col(tbl, ie->sym) : NULL;
            int8_t in_type = val_col ? val_col->type : RAY_I64;
            const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], in_type);
            col = agg_run_one(vt, val_col, groups.gids, nrows, groups.ngroups, kparam);
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
                   const uint32_t* gids, int64_t nrows, int64_t ngroups,
                   int64_t kparam) {
    char* states = calloc((size_t)(ngroups > 0 ? ngroups : 1), vt->state_size);
    if (!states) return ray_error("oom", NULL);
    for (int64_t gi = 0; gi < ngroups; gi++)
        vt->init(states + (size_t)gi * vt->state_size);

    ray_valid_t valid = { val_col ? ray_data(val_col) : NULL,
                          val_col ? val_col->type : RAY_I64,
                          val_col ? ((val_col->attrs & RAY_ATTR_HAS_NULLS) != 0) : false };
    const void* vals = val_col ? ray_data(val_col) : NULL;
    vt->update_batch(states, vt->state_size, gids, vals, &valid, nrows, NULL);

    bool is_list = (vt->out_type == RAY_LIST);
    ray_t* out = is_list ? ray_list_new(ngroups) : ray_vec_new(vt->out_type, ngroups);
    if (!out || RAY_IS_ERR(out)) {
        if (vt->destroy)
            for (int64_t gi = 0; gi < ngroups; gi++)
                vt->destroy(states + (size_t)gi * vt->state_size);
        free(states); return ray_error("oom", NULL);
    }
    out->len = ngroups;
    for (int64_t gi = 0; gi < ngroups; gi++) {
        ray_t* cell = vt->finalize(states + (size_t)gi * vt->state_size, NULL, kparam);
        if (is_list) {
            out = ray_list_set(out, gi, cell);   /* retains cell */
            ray_release(cell);                    /* drop our local ref */
        } else {
            agg_put_cell(out, gi, cell);
            ray_release(cell);
        }
        if (vt->destroy) vt->destroy(states + (size_t)gi * vt->state_size);
    }
    free(states);
    return out;
}

/* Binary-aggregate (pearson) serial driver: mirrors agg_run_one but feeds two
 * value columns through vt->update_batch2.  A row contributes only when both x
 * and y are valid (the accumulator enforces this via valid_x/valid_y). */
ray_t* agg_run_one_bin(const agg_vtable_t* vt, ray_t* x_col, ray_t* y_col,
                       const uint32_t* gids, int64_t nrows, int64_t ngroups,
                       int64_t kparam) {
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
    if (!out || RAY_IS_ERR(out)) {
        if (vt->destroy)
            for (int64_t gi = 0; gi < ngroups; gi++)
                vt->destroy(states + (size_t)gi * vt->state_size);
        free(states); return ray_error("oom", NULL);
    }
    out->len = ngroups;
    for (int64_t gi = 0; gi < ngroups; gi++) {
        ray_t* cell = vt->finalize(states + (size_t)gi * vt->state_size, NULL, kparam);
        agg_put_cell(out, gi, cell);
        ray_release(cell);
        if (vt->destroy) vt->destroy(states + (size_t)gi * vt->state_size);
    }
    free(states);
    return out;
}

/* Direct-index grouping: gid = slot2gid[packed slot], assigned first-occurrence.
 * O(1) per row, no hashing. Precondition: dp->ok. Returns 0 / -1 (OOM).
 * Caller frees out->gids and out->first_row.  Fills the SAME agg_groups_t
 * contract as agg_group_keys (gids per row, first_row per group, ngroups in
 * first-occurrence order) so the downstream emit/assembler is unchanged. */
static int agg_group_keys_dense(ray_t** key_cols, int64_t nrows,
                                const dense_plan_t* dp, agg_groups_t* out) {
    const void* data[16];
    for (uint8_t k = 0; k < dp->n_keys; k++) data[k] = ray_data(key_cols[k]);
    int32_t* slot2gid = malloc((size_t)dp->total_slots * sizeof(int32_t));
    out->gids      = malloc((size_t)(nrows > 0 ? nrows : 1) * sizeof(uint32_t));
    out->first_row = malloc((size_t)(nrows > 0 ? nrows : 1) * sizeof(int64_t));
    if (!slot2gid || !out->gids || !out->first_row) {
        free(slot2gid); free(out->gids); free(out->first_row);
        out->gids = NULL; out->first_row = NULL; return -1;
    }
    for (int64_t s = 0; s < dp->total_slots; s++) slot2gid[s] = -1;
    int64_t ngroups = 0;
    for (int64_t r = 0; r < nrows; r++) {
        int64_t slot = 0;
        for (uint8_t k = 0; k < dp->n_keys; k++)
            slot += (agg_read_key_i64(key_cols[k], data[k], r) - dp->mins[k]) * dp->strides[k];
        /* slot is provably in [0,total_slots): each key in [min_k,max_k] so
         * (key-min) in [0,range_k), and the composite is a mixed-radix index
         * < total_slots (dp->ok from the same prescan). */
        int32_t gp = slot2gid[slot];
        if (gp < 0) {
            gp = (int32_t)ngroups;
            slot2gid[slot] = gp;
            out->first_row[ngroups] = r;
            ngroups++;
        }
        out->gids[r] = (uint32_t)gp;
    }
    out->ngroups = ngroups;
    free(slot2gid);
    return 0;
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
