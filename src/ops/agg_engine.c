/* src/ops/agg_engine.c — v2 aggregation engine. */
#include "ops/agg_engine.h"
#include "ops/agg_registry.h"
#include "ops/ops.h"

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

ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    (void)g; (void)op; (void)tbl;
    return ray_error("nyi", "agg v2: not reachable until gate admits");
}
