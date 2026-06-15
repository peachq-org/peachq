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

ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    int64_t nrows = ray_table_nrows(tbl);

    ray_t* key_cols[16]; int64_t key_syms[16];
    for (uint8_t k = 0; k < ext->n_keys; k++) {
        ray_op_ext_t* kext = find_ext(g, ext->keys[k]->id);
        key_cols[k] = ray_table_get_col(tbl, kext->sym);
        key_syms[k] = kext->sym;
    }

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
        ray_t* val_col = (ext->agg_ops[a] != OP_COUNT) ? ray_table_get_col(tbl, ie->sym) : NULL;
        int8_t in_type = val_col ? val_col->type : RAY_I64;
        const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], in_type);
        ray_t* col = agg_run_one(vt, val_col, groups.gids, nrows, groups.ngroups);
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
