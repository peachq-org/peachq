/* src/ops/agg_engine.c — v2 aggregation engine. */
#include "ops/agg_engine.h"
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include "table/sym.h"    /* ray_read_sym */
#include <stdlib.h>
#include <string.h>

bool ray_agg_engine_v2 = false;   /* knob; default off */

bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return false;
    if (ext->n_keys != 1) return false;        /* 1a: single key only */
    if (ext->n_aggs == 0) return false;        /* need >=1 aggregate  */
    if (!tbl) return false;
    if (g->selection) return false;            /* no active/pushed filter */

    /* key must be a plain column scan of a supported type */
    ray_op_t* key = ext->keys[0];
    if (!key || key->opcode != OP_SCAN) return false;
    ray_op_ext_t* kext = find_ext(g, key->id);
    ray_t* kc = (kext) ? ray_table_get_col(tbl, kext->sym) : NULL;
    if (!kc) return false;
    switch (kc->type) {
        case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
        case RAY_BOOL: case RAY_DATE: case RAY_TIME:
        case RAY_TIMESTAMP: case RAY_SYM: break;
        default: return false;
    }

    /* every aggregate must be a registry-resolvable plain-column scan */
    for (uint8_t a = 0; a < ext->n_aggs; a++) {
        if (ext->agg_ins2 && ext->agg_ins2[a]) return false;   /* binary agg (pearson) — defer */
        if (ext->agg_k   && ext->agg_k[a])    return false;    /* holistic K (top/bot) — defer */
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

ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    (void)g; (void)op; (void)tbl;
    return ray_error("nyi", "agg v2: not reachable until gate admits");
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

/* Write a finalized scalar cell into output column slot gi, marking nulls. */
static void agg_out_put(ray_t* out, int64_t gi, ray_t* cell, bool* any_null) {
    switch (out->type) {
        case RAY_F64:
            ((double*)ray_data(out))[gi] = cell->f64; break;
        default: /* RAY_I64 (and temporal widths if they arise) */
            ((int64_t*)ray_data(out))[gi] = cell->i64; break;
    }
    if (RAY_ATOM_IS_NULL(cell)) {
        ray_vec_set_null(out, gi, true);   /* also sets RAY_ATTR_HAS_NULLS */
        *any_null = true;
    }
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
    bool any_null = false;
    for (int64_t gi = 0; gi < ngroups; gi++) {
        ray_t* cell = vt->finalize(states + (size_t)gi * vt->state_size, NULL);
        agg_out_put(out, gi, cell, &any_null);
        ray_release(cell);
    }
    free(states);
    return out;
}

int agg_group_keys_i(ray_t* key_col, agg_groups_t* out) {
    int64_t nrows = key_col->len;
    const void* data = ray_data(key_col);

    /* hash table capacity: next pow2 >= 2*nrows, min 16 */
    int64_t cap = 16;
    while (cap < nrows * 2) cap <<= 1;
    uint64_t mask = (uint64_t)cap - 1;

    int64_t* ht_key = malloc((size_t)cap * sizeof(int64_t));
    int32_t* ht_gid = malloc((size_t)cap * sizeof(int32_t));
    out->gids = malloc((size_t)(nrows > 0 ? nrows : 1) * sizeof(uint32_t));
    out->keys = malloc((size_t)(nrows > 0 ? nrows : 1) * sizeof(int64_t)); /* <= nrows groups */
    if (!ht_key || !ht_gid || !out->gids || !out->keys) {
        free(ht_key); free(ht_gid); free(out->gids); free(out->keys);
        out->gids = NULL; out->keys = NULL; return -1;
    }
    for (int64_t i = 0; i < cap; i++) ht_gid[i] = -1;

    int64_t ngroups = 0;
    for (int64_t r = 0; r < nrows; r++) {
        int64_t k = agg_read_key_i64(key_col, data, r);
        uint64_t h = ((uint64_t)k * 0x9E3779B97F4A7C15ULL) >> 29;
        h &= mask;
        for (;;) {
            int32_t g = ht_gid[h];
            if (g < 0) {                         /* empty slot → new group */
                ht_gid[h] = (int32_t)ngroups;
                ht_key[h] = k;
                out->keys[ngroups] = k;
                out->gids[r] = (uint32_t)ngroups;
                ngroups++;
                break;
            }
            if (ht_key[h] == k) {                /* existing group */
                out->gids[r] = (uint32_t)g;
                break;
            }
            h = (h + 1) & mask;                  /* linear probe */
        }
    }
    out->ngroups = ngroups;
    free(ht_key); free(ht_gid);
    return 0;
}
