/* q_eval_apply — the apply module, THE one dispatch home (see q_eval.h).
 * Owns valence/rank, projections, composition, RAY_QFN carriers, native
 * adverbs + the manifest mono column, the L1-L4 family lifts (manifest
 * family column), kernel invocation and result construction.  The atomic vector maps are
 * transcribed from the carve-eval quarry (base eval.c atomic_map_*): kept
 * empty-input zero-atom probe, typed-output probe, int-width promotion,
 * boxed fallback, error-trim.  Kernel-library extraction of the base
 * atomic_map internals is pending; this q-side copy is the recorded choice.
 * Refcount contract: args borrowed, results owned; born rc=1, append
 * retains, release local. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/eval/q_eval.h"
#include "qlang/q_err.h"
#include "qlang/q_ops.h"
#include "qlang/q_registry.h"
#include "qlang/q_parse_internal.h"
#include "qlang/q_fmt.h"
#include "qlang/q_handles.h"
#include "qlang/net/q_ws.h"
#include "qlang/net/q_http_client.h"
#include "qlang/ops/q_dollar.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "lang/internal.h"   /* call_lambda — bare engine-lambda application */
#include "ops/ops.h"
#include "table/dict.h"
#include "table/sym.h"
#include <stdio.h>
#include <string.h>

#define APPLY_MAX_ARGS 60

/* ===== RAY_QFN carriers ================================================== */

/* kind lives in aux[0] — the i64 union slot IS len for the slot count */
static ray_t* car_new(int kind, int64_t nslots) {
    ray_t* c = ray_alloc((size_t)nslots * sizeof(ray_t*));
    if (!c) return q_err(QE_OOM);
    c->type = RAY_QFN;
    c->attrs = 0;
    c->len = nslots;
    c->aux[0] = (uint8_t)kind;
    memset(ray_data(c), 0, (size_t)nslots * sizeof(ray_t*));
    return c;
}

int q_eval_apply_carrier_kind(const ray_t* v) {
    return (v && v->type == RAY_QFN) ? (int)v->aux[0] : 0;
}

static ray_t** car_slots(ray_t* c) { return (ray_t**)ray_data(c); }

/* manifest rows are static-storage; boxed as an i64 atom slot */
static ray_t* row_box(const q_op_t* row) {
    return row ? ray_i64((int64_t)(uintptr_t)row) : NULL;
}

static const q_op_t* row_unbox(ray_t* slot) {
    return slot ? (const q_op_t*)(uintptr_t)slot->i64 : NULL;
}

/* store a freshly built child into slot i; on a NULL/error child, unwind
 * the carrier and hand back one owned error (errors must not sit in slots:
 * the heap release walk skips them, which would leak) */
static ray_t* car_put(ray_t* c, int64_t i, ray_t* child) {
    if (child && !RAY_IS_ERR(child)) { car_slots(c)[i] = child; return c; }
    ray_release(c);
    if (child) return child;
    return q_err(QE_OOM);
}

/* lambda carrier: [params symvec, body list, src string] */
ray_t* q_eval_apply_lambda_new(ray_t* params, ray_t** body, int64_t nbody,
                               ray_t* src) {
    ray_t* b = ray_list_new(nbody > 0 ? nbody : 1);
    for (int64_t i = 0; i < nbody; i++)
        b = ray_list_append(b, body[i]);
    if (!b || RAY_IS_ERR(b)) return b ? b : q_err(QE_OOM);
    ray_t* c = car_new(Q_EVAL_CAR_LAMBDA, 3);
    if (RAY_IS_ERR(c)) { ray_release(b); return c; }
    ray_t** s = car_slots(c);
    if (params) ray_retain(params);
    s[0] = params;
    s[1] = b;
    if (src) ray_retain(src);
    s[2] = src;
    return c;
}

/* deriv carrier: [F, F-row box, adv atom] */
ray_t* q_eval_apply_deriv_new(int adv, ray_t* fv, const q_op_t* frow) {
    ray_t* c = car_new(Q_EVAL_CAR_DERIV, 3);
    if (RAY_IS_ERR(c)) return c;
    if (fv) ray_retain(fv);
    car_slots(c)[0] = fv;
    if (frow) {
        c = car_put(c, 1, row_box(frow));
        if (RAY_IS_ERR(c)) return c;
    }
    return car_put(c, 2, ray_i64(adv));
}

/* projection carrier: [fv, fv-row box, slot0..slotR-1]; holes = C NULL */
static ray_t* proj_new(ray_t* fv, const q_op_t* row, ray_t** args, int64_t n,
                       int64_t rank) {
    int64_t slots = rank > n ? rank : n;
    ray_t* c = car_new(Q_EVAL_CAR_PROJ, slots + 2);
    if (RAY_IS_ERR(c)) return c;
    ray_t** s = car_slots(c);
    if (fv) ray_retain(fv);
    s[0] = fv;
    if (row) {
        c = car_put(c, 1, row_box(row));
        if (RAY_IS_ERR(c)) return c;
        s = car_slots(c);
    }
    for (int64_t i = 0; i < n; i++) {
        if (args[i]) ray_retain(args[i]);
        s[2 + i] = args[i];
    }
    return c;
}

/* composition carrier (ref/apply.md Composition): [u, g] — u unary */
static ray_t* comp_new(ray_t* u, ray_t* g) {
    ray_t* c = car_new(Q_EVAL_CAR_COMP, 2);
    if (RAY_IS_ERR(c)) return c;
    ray_retain(u);
    car_slots(c)[0] = u;
    ray_retain(g);
    car_slots(c)[1] = g;
    return c;
}

/* ===== small helpers ===================================================== */

static int is_fnval(ray_t* v) {
    return v && (v->type == RAY_UNARY || v->type == RAY_BINARY ||
                 v->type == RAY_VARY);
}

static int is_coll(ray_t* v) {
    return v && !RAY_IS_ERR(v) && (v->type == RAY_LIST || ray_is_vec(v)) &&
           v->type != RAY_STR;
}

static int is_container(ray_t* v) {
    return v && (v->type == RAY_DICT || v->type == RAY_TABLE);
}

static int int_atom(ray_t* v) {
    return v && (v->type == -RAY_I64 || v->type == -RAY_I32 ||
                 v->type == -RAY_I16);
}

static ray_t* collapse(ray_t* l) {          /* consumes l, returns owned */
    ray_t* c = q_collapse_list(l);
    ray_release(l);
    return c;
}

/* materialize boundary: values STORED INTO CONTAINERS leave the DAG here
 * (design-doc obligation 2; the graph-steal rule applies once, at this one
 * home).  Consumes r. */
static ray_t* store_mat(ray_t* r) {
    if (r && ray_is_lazy(r)) return ray_lazy_materialize(r);
    return r;
}

/* ===== result construction (carve-eval quarry: internal.h trims) ========= */

static int64_t qe_elem_i64(ray_t* e) {
    if (e->type == -RAY_I64 || RAY_IS_TEMPORAL64(-e->type) ||
        RAY_IS_TEMPORAL32(-e->type) || e->type == -RAY_SYM) return e->i64;
    if (e->type == -RAY_I32) return (int64_t)e->i32;
    if (e->type == -RAY_I16) return (int64_t)e->i16;
    if (ray_is_bytelike(-e->type)) return (int64_t)e->u8;
    if (e->type == -RAY_F64 || RAY_IS_TEMPORALF(-e->type)) return (int64_t)e->f64;
    return e->i64;
}

static int store_elem(ray_t* vec, int64_t i, ray_t* e) {
    if (vec->type == RAY_CHARV) {
        ((uint8_t*)ray_data(vec))[i] =
            RAY_ATOM_IS_NULL(e) ? 0x20 : (uint8_t)qe_elem_i64(e);
        return 0;
    }
    if (RAY_ATOM_IS_NULL(e)) {
        switch (vec->type) {
            case RAY_F64: RAY_TEMPORALF_CASES:
                ((double*)ray_data(vec))[i] = NULL_F64; break;
            case RAY_I64: RAY_TEMPORAL64_CASES:
                ((int64_t*)ray_data(vec))[i] = NULL_I64; break;
            case RAY_I32: case RAY_DATE: case RAY_TIME: case RAY_MONTH:
            case RAY_MINUTE: case RAY_SECOND:
                ((int32_t*)ray_data(vec))[i] = NULL_I32; break;
            case RAY_I16:
                ((int16_t*)ray_data(vec))[i] = NULL_I16; break;
            case RAY_BOOL:
                ((bool*)ray_data(vec))[i] = false; break;
            case RAY_BYTE_ONLY:
                ((uint8_t*)ray_data(vec))[i] = 0; break;
            default: return -1;
        }
        ray_vec_set_null(vec, i, true);
        return 0;
    }
    switch (vec->type) {
        case RAY_I64: RAY_TEMPORAL64_CASES:
            ((int64_t*)ray_data(vec))[i] = qe_elem_i64(e); return 0;
        case RAY_F64:
            ((double*)ray_data(vec))[i] =
                (e->type == -RAY_F64) ? e->f64 : (double)qe_elem_i64(e); return 0;
        case RAY_DATETIME:
            ((double*)ray_data(vec))[i] =
                (e->type == -RAY_F64 || e->type == -RAY_DATETIME)
                    ? e->f64 : (double)qe_elem_i64(e); return 0;
        case RAY_I32: case RAY_DATE: case RAY_TIME: case RAY_MONTH:
        case RAY_MINUTE: case RAY_SECOND:
            ((int32_t*)ray_data(vec))[i] = (int32_t)qe_elem_i64(e); return 0;
        case RAY_I16:
            ((int16_t*)ray_data(vec))[i] = (int16_t)qe_elem_i64(e); return 0;
        case RAY_BOOL:
            ((bool*)ray_data(vec))[i] = e->b8; return 0;
        case RAY_BYTE_ONLY:
            ((uint8_t*)ray_data(vec))[i] = (uint8_t)qe_elem_i64(e); return 0;
        default: return -1;
    }
}

static int typed_out_ok(int8_t t) {
    return t == RAY_I64 || t == RAY_F64 || t == RAY_I32 || t == RAY_I16 ||
           t == RAY_BOOL || t == RAY_BYTE_ONLY || t == RAY_CHARV ||
           RAY_IS_TEMPORAL32(t) || RAY_IS_TEMPORAL64(t) || RAY_IS_TEMPORALF(t);
}

/* empty-input output-type probe */
static ray_t* zero_atom(ray_t* coll) {
    switch (coll ? coll->type : RAY_LIST) {
        case RAY_F64:  return ray_f64(0.0);
        case RAY_BOOL: return ray_bool(0);
        case RAY_I32:  return ray_i32(0);
        case RAY_I16:  return ray_i16(0);
        case RAY_SYM:  return ray_sym(0);
        default:       return ray_i64(0);
    }
}

/* ===== atomic vector maps (carved from base atomic_map_unary/_binary) ==== */

static ray_t* atomic1(ray_unary_fn f, ray_t* x);
static ray_t* atomic2(ray_binary_fn f, ray_t* x, ray_t* y);

static ray_t* unary_elem(ray_unary_fn fn, ray_t* e) {
    if (RAY_IS_ERR(e)) return e;
    if (is_coll(e) || is_container(e)) return atomic1(fn, e);
    return fn(e);
}

static ray_t* map_unary(ray_unary_fn fn, ray_t* arg) {
    int64_t len = ray_len(arg);
    int is_boxed = (arg->type == RAY_LIST);

    if (len == 0) {
        ray_t* z = zero_atom(arg);
        ray_t* probe = (z && !RAY_IS_ERR(z)) ? fn(z) : NULL;
        if (z) ray_release(z);
        if (probe && !RAY_IS_ERR(probe) && probe->type < 0) {
            int8_t t = (int8_t)(-probe->type);
            ray_release(probe);
            return ray_vec_new(t, 0);
        }
        if (probe && RAY_IS_ERR(probe)) ray_error_free(probe);
        else if (probe) ray_release(probe);
        return ray_vec_new(RAY_I64, 0);
    }

    ray_t* e0_in = q_registry_elem_at(arg, 0);
    ray_t* e0 = unary_elem(fn, e0_in);
    if (e0 != e0_in) ray_release(e0_in);
    if (RAY_IS_ERR(e0)) return e0;

    int8_t out_type = (int8_t)(-e0->type);
    if (!is_boxed && ray_is_atom(e0) && typed_out_ok(out_type)) {
        ray_t* vec = ray_vec_new(out_type, len);
        if (RAY_IS_ERR(vec)) { ray_release(e0); return vec; }
        vec->len = len;
        store_elem(vec, 0, e0);
        ray_release(e0);
        for (int64_t i = 1; i < len; i++) {
            if ((i & 0xFFF) == 0 && ray_eval_is_interrupted()) {
                ray_release(vec);
                return q_err(QE_STOP);
            }
            ray_t* e = q_registry_elem_at(arg, i);
            ray_t* r = RAY_IS_ERR(e) ? e : fn(e);
            if (r != e) ray_release(e);
            if (RAY_IS_ERR(r)) { ray_release(vec); return r; }
            if (store_elem(vec, i, r) != 0) {
                ray_release(r); ray_release(vec);
                return q_err(QE_TYPE);
            }
            ray_release(r);
        }
        return vec;
    }

    ray_t* out = ray_list_new(len);
    out = ray_list_append(out, e0);
    ray_release(e0);
    for (int64_t i = 1; i < len; i++) {
        ray_t* e = q_registry_elem_at(arg, i);
        ray_t* r = unary_elem(fn, e);
        if (r != e) ray_release(e);
        if (RAY_IS_ERR(r)) { ray_release(out); return r; }
        out = ray_list_append(out, r);
        ray_release(r);
    }
    return collapse(out);
}

static ray_t* binary_elem(ray_binary_fn fn, ray_t* a, ray_t* b) {
    if (RAY_IS_ERR(a)) return a;
    if (RAY_IS_ERR(b)) return b;
    if (is_coll(a) || is_coll(b) || is_container(a) || is_container(b))
        return atomic2(fn, a, b);
    return fn(a, b);
}

static ray_t* map_binary(ray_binary_fn fn, ray_t* l, ray_t* r) {
    int lc = is_coll(l), rc = is_coll(r);
    if (!lc && !rc) return fn(l, r);
    int64_t len;
    if (lc && rc) {
        if (ray_len(l) != ray_len(r)) return q_err(QE_LENGTH);
        len = ray_len(l);
    } else {
        len = lc ? ray_len(l) : ray_len(r);
    }

    if (len == 0) {
        ray_t* la = lc ? zero_atom(l) : l;
        ray_t* ra = rc ? zero_atom(r) : r;
        ray_t* probe = (la && ra) ? fn(la, ra) : NULL;
        if (lc && la) ray_release(la);
        if (rc && ra) ray_release(ra);
        if (probe && !RAY_IS_ERR(probe) && probe->type < 0) {
            int8_t t = (int8_t)(-probe->type);
            ray_release(probe);
            return ray_vec_new(t, 0);
        }
        if (probe && RAY_IS_ERR(probe)) ray_error_free(probe);
        else if (probe) ray_release(probe);
        return ray_vec_new(RAY_I64, 0);
    }

    ray_t* a0 = lc ? q_registry_elem_at(l, 0) : l;
    ray_t* b0 = rc ? q_registry_elem_at(r, 0) : r;
    ray_t* e0 = binary_elem(fn, a0, b0);
    if (lc && a0 != e0) ray_release(a0);
    if (rc && b0 != e0) ray_release(b0);
    if (RAY_IS_ERR(e0)) return e0;

    int8_t out_type = (int8_t)(-e0->type);
    int force_boxed = (lc && l->type == RAY_LIST) || (rc && r->type == RAY_LIST);
    int e0_null = ray_is_atom(e0) && RAY_ATOM_IS_NULL(e0);
    int e0_bool = (e0->type == -RAY_BOOL);

    /* kept from the carve: integer width follows the wider vector operand */
    #define INTT(t) ((t)==RAY_I64||(t)==RAY_I32||(t)==RAY_I16||(t)==RAY_BYTE_ONLY)
    if (!e0_null && !e0_bool && INTT(out_type)) {
        int8_t w = out_type;
        if (lc && ray_is_vec(l) && INTT(l->type) && l->type > w) w = l->type;
        if (rc && ray_is_vec(r) && INTT(r->type) && r->type > w) w = r->type;
        if (!lc && ray_is_atom(l) && INTT((int8_t)-l->type) && (int8_t)-l->type > w) w = (int8_t)-l->type;
        if (!rc && ray_is_atom(r) && INTT((int8_t)-r->type) && (int8_t)-r->type > w) w = (int8_t)-r->type;
        out_type = w;
    }
    #undef INTT

    if (!force_boxed && ray_is_atom(e0) && typed_out_ok(out_type)) {
        ray_t* vec = ray_vec_new(out_type, len);
        if (RAY_IS_ERR(vec)) { ray_release(e0); return vec; }
        vec->len = len;
        store_elem(vec, 0, e0);
        ray_release(e0);
        for (int64_t i = 1; i < len; i++) {
            if ((i & 0xFFF) == 0 && ray_eval_is_interrupted()) {
                ray_release(vec);
                return q_err(QE_STOP);
            }
            ray_t* a = lc ? q_registry_elem_at(l, i) : l;
            ray_t* b = rc ? q_registry_elem_at(r, i) : r;
            ray_t* e = (RAY_IS_ERR(a) || RAY_IS_ERR(b)) ? q_err(QE_TYPE)
                                                        : fn(a, b);
            if (lc && a != e) ray_release(a);
            if (rc && b != e) ray_release(b);
            if (RAY_IS_ERR(e)) { ray_release(vec); return e; }
            if (store_elem(vec, i, e) != 0) {
                ray_release(e); ray_release(vec);
                return q_err(QE_TYPE);
            }
            ray_release(e);
        }
        return vec;
    }

    ray_t* out = ray_list_new(len);
    out = ray_list_append(out, e0);
    ray_release(e0);
    for (int64_t i = 1; i < len; i++) {
        ray_t* a = lc ? q_registry_elem_at(l, i) : l;
        ray_t* b = rc ? q_registry_elem_at(r, i) : r;
        ray_t* e = binary_elem(fn, a, b);
        if (lc && a != e) ray_release(a);
        if (rc && b != e) ray_release(b);
        if (RAY_IS_ERR(e)) { ray_release(out); return e; }
        out = ray_list_append(out, e);
        ray_release(e);
    }
    return collapse(out);
}

/* ===== L1/L2 atomic lifts over containers ================================ */

static ray_t* table_percol(ray_t* t, ray_t* (*colfn)(void* ctx, ray_t* col),
                           void* ctx) {
    int64_t nc = ray_table_ncols(t);
    ray_t* out = ray_table_new(nc);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(t, c);          /* borrowed */
        ray_t* r = store_mat(colfn(ctx, col));
        if (!r || RAY_IS_ERR(r)) {
            ray_release(out);
            return r ? r : q_err(QE_TYPE);
        }
        out = ray_table_add_col(out, ray_table_col_name(t, c), r);
        ray_release(r);
    }
    return out;
}

static ray_t* atomic1_col(void* ctx, ray_t* col) {
    return atomic1((ray_unary_fn)(uintptr_t)ctx, col);
}

static ray_t* atomic1(ray_unary_fn f, ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == RAY_DICT) {
        ray_t* nv = atomic1(f, ray_dict_vals(x));
        if (RAY_IS_ERR(nv)) return nv;
        ray_t* k = ray_dict_keys(x);
        ray_retain(k);
        return ray_dict_new(k, nv);                        /* consumes both */
    }
    if (x->type == RAY_TABLE)
        return table_percol(x, atomic1_col, (void*)(uintptr_t)f);
    if (is_coll(x)) return map_unary(f, x);
    return f(x);
}

/* dyadic dict-dict: key-union distribution (common keys combine,
 * singletons pass through) */
static ray_t* atomic2_dicts(ray_binary_fn f, ray_t* x, ray_t* y) {
    ray_t* uk = ray_union_fn(ray_dict_keys(x), ray_dict_keys(y));
    if (!uk || RAY_IS_ERR(uk)) return uk ? uk : q_err(QE_TYPE);
    int64_t n = ray_len(uk);
    ray_t* vx = ray_dict_vals(x);
    ray_t* vy = ray_dict_vals(y);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    for (int64_t j = 0; j < n; j++) {
        ray_t* k = q_registry_elem_at(uk, j);
        int64_t ix = ray_dict_find_idx(x, k);
        int64_t iy = ray_dict_find_idx(y, k);
        ray_release(k);
        ray_t* r;
        if (ix >= 0 && iy >= 0) {
            ray_t* ex = q_registry_elem_at(vx, ix);
            ray_t* ey = q_registry_elem_at(vy, iy);
            r = binary_elem(f, ex, ey);
            if (r != ex) ray_release(ex);
            if (r != ey) ray_release(ey);
        } else if (ix >= 0) {
            r = q_registry_elem_at(vx, ix);
        } else {
            r = q_registry_elem_at(vy, iy);
        }
        if (!r || RAY_IS_ERR(r)) {
            ray_release(out);
            ray_release(uk);
            return r ? r : q_err(QE_TYPE);
        }
        out = ray_list_append(out, r);
        ray_release(r);
    }
    return ray_dict_new(uk, collapse(out));
}

typedef struct { ray_binary_fn f; ray_t* other; int other_left; } a2ctx;

static ray_t* atomic2_col(void* vctx, ray_t* col) {
    a2ctx* c = (a2ctx*)vctx;
    return c->other_left ? atomic2(c->f, c->other, col)
                         : atomic2(c->f, col, c->other);
}

static ray_t* atomic2(ray_binary_fn f, ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    int xd = x->type == RAY_DICT, yd = y->type == RAY_DICT;
    if (xd && yd) return atomic2_dicts(f, x, y);
    if (xd || yd) {
        ray_t* d = xd ? x : y;
        ray_t* o = xd ? y : x;
        if (o->type == RAY_TABLE) return q_err(QE_NYI);
        ray_t* nv = xd ? atomic2(f, ray_dict_vals(d), o)
                       : atomic2(f, o, ray_dict_vals(d));
        if (RAY_IS_ERR(nv)) return nv;
        ray_t* k = ray_dict_keys(d);
        ray_retain(k);
        return ray_dict_new(k, nv);
    }
    if (x->type == RAY_TABLE || y->type == RAY_TABLE) {
        int xt = x->type == RAY_TABLE;
        ray_t* t = xt ? x : y;
        ray_t* o = xt ? y : x;
        if (o->type == RAY_TABLE || is_coll(o)) return q_err(QE_NYI);
        a2ctx ctx = { f, o, !xt };
        return table_percol(t, atomic2_col, &ctx);
    }
    return map_binary(f, x, y);
}

/* ===== L3 aggregate lift ================================================= */

static ray_t* agg1(ray_t* fv, const q_op_t* row, ray_t* x) {
    if (x && x->type == RAY_DICT) {
        if (q_table_is_keyed(x)) return agg1(fv, row, ray_dict_vals(x));
        ray_t* v = ray_dict_vals(x);
        return q_eval_apply(fv, row, &v, 1);
    }
    if (x && x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* ks = ray_sym_vec_new(RAY_SYM_W64, nc > 0 ? nc : 1);
        ray_t* vs = ray_list_new(nc > 0 ? nc : 1);
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(x, c);      /* borrowed */
            ray_t* r = store_mat(q_eval_apply(fv, row, &col, 1));
            if (RAY_IS_ERR(r)) { ray_release(ks); ray_release(vs); return r; }
            int64_t nm = ray_table_col_name(x, c);
            ks = ray_vec_append(ks, &nm);
            vs = ray_list_append(vs, r);
            ray_release(r);
        }
        return ray_dict_new(ks, collapse(vs));
    }
    return q_err(QE_TYPE);
}

/* ===== L2 map lift ======================================================= */

static ray_t* map1(ray_t* fv, const q_op_t* row, ray_t* x) {
    if (x && x->type == RAY_DICT) {
        ray_t* v = ray_dict_vals(x);
        ray_t* nv;
        if (q_table_is_keyed(x)) nv = map1(fv, row, v);
        else                     nv = store_mat(q_eval_apply(fv, row, &v, 1));
        if (RAY_IS_ERR(nv)) return nv;
        ray_t* k = ray_dict_keys(x);
        ray_retain(k);
        return ray_dict_new(k, nv);
    }
    if (x && x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* out = ray_table_new(nc);
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(x, c);
            ray_t* r = store_mat(q_eval_apply(fv, row, &col, 1));
            if (RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_table_add_col(out, ray_table_col_name(x, c), r);
            ray_release(r);
        }
        return out;
    }
    return q_err(QE_TYPE);
}

/* ===== kernel invocation ================================================= */

/* materialize a lazy arg for a non-lazy-aware kernel (call_fn idiom) */
static ray_t* force_arg(ray_t* fv, ray_t* a, int* owned) {
    *owned = 0;
    if (a && !(fv->attrs & RAY_FN_LAZY_AWARE) && ray_is_lazy(a)) {
        ray_retain(a);
        a = ray_lazy_materialize(a);
        *owned = 1;
    }
    return a;
}

static ray_t* call_kernel(ray_t* fv, ray_t** args, int64_t n) {
    if (fv->attrs & RAY_FN_SPECIAL_FORM) return q_err(QE_NYI);
    if (fv->type == RAY_UNARY) {
        if (n != 1) return q_err(QE_RANK);
        int o;
        ray_t* a = force_arg(fv, args[0], &o);
        if (!a || RAY_IS_ERR(a)) return a ? a : q_err(QE_TYPE);
        ray_t* r = ((ray_unary_fn)(uintptr_t)fv->i64)(a);
        if (o) ray_release(a);
        return r ? r : q_err(QE_TYPE);
    }
    if (fv->type == RAY_BINARY) {
        if (n != 2) return q_err(QE_RANK);
        int oa, ob;
        ray_t* a = force_arg(fv, args[0], &oa);
        if (!a || RAY_IS_ERR(a)) return a ? a : q_err(QE_TYPE);
        ray_t* b = force_arg(fv, args[1], &ob);
        if (!b || RAY_IS_ERR(b)) {
            if (oa) ray_release(a);
            return b ? b : q_err(QE_TYPE);
        }
        ray_t* r = ((ray_binary_fn)(uintptr_t)fv->i64)(a, b);
        if (oa) ray_release(a);
        if (ob) ray_release(b);
        return r ? r : q_err(QE_TYPE);
    }
    if (fv->type == RAY_VARY) {
        ray_t* r = ((ray_vary_fn)(uintptr_t)fv->i64)(args, n);
        return r ? r : q_err(QE_TYPE);
    }
    return q_err(QE_TYPE);
}

/* ===== L4 index lift: op t = t @ op til count t (ONE gather) ============= */

static ray_t* index_lift(ray_t* fv, ray_t** args, int64_t n) {
    ray_t* t = (n == 2) ? args[1] : args[0];
    ray_t* nrv = ray_i64(ray_table_nrows(t));
    ray_t* til = ray_til_fn(nrv);
    ray_release(nrv);
    if (!til || RAY_IS_ERR(til)) return til ? til : q_err(QE_TYPE);
    ray_t* iargs[2];
    if (n == 2) { iargs[0] = args[0]; iargs[1] = til; }
    else        { iargs[0] = til; }
    ray_t* idx = call_kernel(fv, iargs, n);
    ray_release(til);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_TYPE);
    int64_t nc = ray_table_ncols(t);
    ray_t* out = ray_table_new(nc);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* g = ray_at_fn(ray_table_get_col_idx(t, c), idx);
        if (!g || RAY_IS_ERR(g)) {
            ray_release(out);
            ray_release(idx);
            return g ? g : q_err(QE_TYPE);
        }
        out = ray_table_add_col(out, ray_table_col_name(t, c), g);
        ray_release(g);
    }
    ray_release(idx);
    return out;
}

/* ===== adverbs: native application + manifest monomorphization =========== */

static ray_t* fold_over(ray_t* fv, const q_op_t* frow, ray_t* seed, ray_t* x) {
    if (!is_coll(x)) return q_err(QE_NYI);   /* converge/do: punt */
    int64_t len = ray_len(x);
    if (len == 0) {
        if (seed) { ray_retain(seed); return seed; }
        ray_t* id = frow ? q_ops_acc_identity(frow->name) : NULL;
        return id ? id : q_err(QE_NYI);
    }
    ray_t* acc;
    int64_t i0;
    if (seed) { ray_retain(seed); acc = seed; i0 = 0; }
    else      { acc = q_registry_elem_at(x, 0); i0 = 1; }
    for (int64_t i = i0; i < len; i++) {
        ray_t* ei = q_registry_elem_at(x, i);
        ray_t* av[2] = { acc, ei };
        ray_t* nx = q_eval_apply(fv, frow, av, 2);
        ray_release(ei);
        ray_release(acc);
        if (RAY_IS_ERR(nx)) return nx;
        acc = nx;
    }
    return acc;
}

static ray_t* each1(ray_t* fv, const q_op_t* frow, ray_t* x) {
    if (x && x->type == RAY_DICT) {
        ray_t* nv = each1(fv, frow, ray_dict_vals(x));
        if (RAY_IS_ERR(nv)) return nv;
        ray_t* k = ray_dict_keys(x);
        ray_retain(k);
        return ray_dict_new(k, nv);
    }
    if (!is_coll(x)) return q_eval_apply(fv, frow, &x, 1);
    int64_t len = ray_len(x);
    ray_t* l = ray_list_new(len > 0 ? len : 1);
    for (int64_t i = 0; i < len; i++) {
        ray_t* ei = q_registry_elem_at(x, i);
        ray_t* r = store_mat(q_eval_apply(fv, frow, &ei, 1));
        ray_release(ei);
        if (RAY_IS_ERR(r)) { ray_release(l); return r; }
        l = ray_list_append(l, r);
        ray_release(r);
    }
    return collapse(l);
}

ray_t* q_eval_apply_adverb(int adv, ray_t* fv, const q_op_t* frow,
                           ray_t** args, int64_t n) {
    if (!fv) return q_err(QE_TYPE);
    if (adv == 1) {                                        /* `/` over */
        if (n == 1) {
            /* mono column: f/ of a registered dyad IS its aggregate kernel.
             * Boxed lists take the native fold instead — the aggregate
             * wrappers' boxed arms ride base call_fn plumbing (finding 5). */
            if (frow && frow->mono && args[0] && args[0]->type != RAY_LIST) {
                const q_op_t* mrow = NULL;
                ray_t* mv = q_registry_lookup_row(
                    ray_sym_intern_runtime(frow->mono, strlen(frow->mono)),
                    Q_MONADIC, &mrow);
                if (mv && is_fnval(mv))
                    return q_eval_apply(mv, mrow, args, 1);
            }
            return fold_over(fv, frow, NULL, args[0]);
        }
        if (n == 2) return fold_over(fv, frow, args[0], args[1]);
        return q_err(QE_RANK);
    }
    if (adv == 0 && n == 1) return each1(fv, frow, args[0]);
    return q_err(QE_NYI);          /* \ ': /: \: — native arms later */
}

/* keyword-HOF row (each/peach/over/scan/prior): adverb_hof -> adv id */
static int hof_adv_id(const char* hof) {
    if (strcmp(hof, "map") == 0)   return 0;
    if (strcmp(hof, "fold") == 0)  return 1;
    if (strcmp(hof, "scan") == 0)  return 2;
    if (strcmp(hof, "prior") == 0) return 3;
    return -1;
}

/* ===== lambda application: frames over the engine scope stack ============ */

static _Thread_local int g_frame_depth;
int q_eval_apply_frame_depth(void) { return g_frame_depth; }

ray_t* q_eval_apply_lambda_src(ray_t* v) {
    if (q_eval_apply_carrier_kind(v) != Q_EVAL_CAR_LAMBDA) return NULL;
    ray_t* src = car_slots(v)[2];
    return (src && src->type == -RAY_STR) ? src : NULL;
}

static ray_t* lambda_call(ray_t* lam, ray_t** args, int64_t n) {
    ray_t** c = car_slots(lam);
    ray_t* params = c[0];
    ray_t* body = c[1];
    /* barrier frame: strictly-local resolution (function-notation.md) —
     * the body sees its params/locals and globals, never a caller frame */
    if (ray_env_push_scope_barrier() != RAY_OK) return q_err(QE_STACK);
    for (int64_t i = 0; i < n; i++) {
        ray_t* p = q_registry_elem_at(params, i);          /* sym atom */
        if (p && !RAY_IS_ERR(p)) {
            ray_env_set_local(p->i64, args[i]);            /* retains */
            ray_release(p);
        }
    }
    g_frame_depth++;
    ray_t* r = RAY_NULL_OBJ;
    int64_t nb = ray_len(body);
    ray_t** bs = (ray_t**)ray_data(body);
    for (int64_t i = 0; i < nb; i++) {
        ray_t* nr = q_eval(bs[i]);
        if (RAY_IS_ERR(nr)) { r = nr; break; }
        ray_release(r);
        r = nr;
    }
    g_frame_depth--;
    ray_env_pop_scope();
    return r;
}

/* ===== projections ======================================================= */

static ray_t* proj_call(ray_t* proj, ray_t** args, int64_t n) {
    ray_t** c = car_slots(proj);
    ray_t* fv = c[0];
    const q_op_t* row = row_unbox(c[1]);
    int64_t rank = ray_len(proj) - 2;
    ray_t* merged[APPLY_MAX_ARGS];
    int64_t ai = 0, holes = 0;
    if (rank > APPLY_MAX_ARGS) return q_err(QE_RANK);
    for (int64_t i = 0; i < rank; i++) {
        merged[i] = (!c[2 + i] && ai < n) ? args[ai++] : c[2 + i];
        if (!merged[i]) holes++;
    }
    if (ai < n) return q_err(QE_RANK);
    if (holes > 0) return proj_new(fv, row, merged, rank, rank);
    return q_eval_apply(fv, row, merged, rank);
}

/* ===== noun-head indexing (re-homed from the retired q_apply_noun) =======
 * ONE indexing primitive per structure — dict miss-typed lookup, table row
 * gather (q_table_at), vector/list gather — drilled one step per arg;
 * results cross into q-space through q_charv_out.  Keyed-table-by-value
 * stays 'nyi (rebuild wave). */

/* one indexing step: v[idx].  ray_at null-fills out-of-range; a collection
 * index yields a boxed list -> collapse to the typed vector. */
static ray_t* gather(ray_t* v, ray_t* idx) {
    ray_t* r = ray_at_fn(v, idx);
    if (!r || RAY_IS_ERR(r)) return r;
    if (r->type == RAY_LIST) return collapse(r);
    return r;
}

/* the typed-null type for a dict's values: a typed vector gives its element
 * type; a homogeneous boxed list of atoms gives their shared type; anything
 * mixed -> 0 (generic null, kdb's mixed-value dict miss). */
static int8_t dict_val_null_type(ray_t* vals) {
    if (!vals) return 0;
    if (ray_is_vec(vals)) return (int8_t)-vals->type;
    if (vals->type == RAY_LIST && ray_len(vals) > 0) {
        ray_t** e = (ray_t**)ray_data(vals);
        if (!e[0] || e[0]->type >= 0) return 0;
        int8_t t = e[0]->type;
        for (int64_t i = 1; i < ray_len(vals); i++)
            if (!e[i] || e[i]->type != t) return 0;
        return t;
    }
    return 0;
}

/* dict arm — DIRECT lookup (not ray_at, whose miss path hardcodes 0Nl): a
 * hit returns the stored value; a miss the typed null of the dict's VALUE
 * type.  A vector of keys loops with those miss semantics, then collapses. */
static ray_t* dict_lookup(ray_t* d, ray_t* idx) {
    if (q_table_is_keyed(d)) return q_err(QE_NYI);
    ray_t* vals = ray_dict_vals(d);                  /* borrowed accessor */
    int8_t vt = dict_val_null_type(vals);
    if (ray_is_vec(idx) || idx->type == RAY_LIST) {
        int64_t kn = ray_len(idx);
        ray_t* out = ray_list_new(kn > 0 ? kn : 1);
        for (int64_t i = 0; i < kn; i++) {
            ray_t* k = q_registry_elem_at(idx, i);
            if (!k || RAY_IS_ERR(k)) { ray_release(out); return k; }
            ray_t* v = ray_dict_get(d, k);
            ray_release(k);
            if (!v) v = vt ? ray_typed_null(vt)
                           : (ray_retain(RAY_NULL_OBJ), RAY_NULL_OBJ);
            out = ray_list_append(out, v);
            ray_release(v);
        }
        return collapse(out);
    }
    ray_t* v = ray_dict_get(d, idx);                 /* owned, or C NULL */
    if (v) return v;
    return vt ? ray_typed_null(vt)
              : (ray_retain(RAY_NULL_OBJ), RAY_NULL_OBJ);
}

static int is_text_atom(ray_t* v) {
    return v && (v->type == -RAY_STR || v->type == RAY_CHARV ||
                 v->type == -RAY_CHARV);
}

static ray_t* noun_index(ray_t* v, ray_t** args, int64_t n);

/* Symbol-handle application: `` `:… `` communication handles (ws://, http://,
 * one-shot IPC) and q4m3 §12 symbol-as-global indexing. */
static ray_t* sym_head_apply(ray_t* head, ray_t** args, int64_t n) {
    ray_t* s = ray_sym_str(head->i64);               /* borrowed */
    if (s && ray_str_len(s) > 0 && ray_str_ptr(s)[0] == ':') {
        const char* sp = ray_str_ptr(s);
        size_t sl = ray_str_len(s);
        if ((sl >= 6 && memcmp(sp, ":ws://", 6) == 0) ||
            (sl >= 7 && memcmp(sp, ":wss://", 7) == 0)) {
            if (n == 1 && is_text_atom(args[0]))
                return q_ws_client_open(head, args[0]);
            return q_err(QE_TYPE);
        }
        if ((sl >= 8 && memcmp(sp, ":http://", 8) == 0) ||
            (sl >= 9 && memcmp(sp, ":https://", 9) == 0)) {
            if (n == 1 && is_text_atom(args[0]))
                return q_http_client_raw(head, args[0]);
            return q_err(QE_TYPE);
        }
        /* one-shot sync IPC (ref/hopen.md): connect -> send -> close */
        if (n == 1 && args[0] &&
            (args[0]->type == -RAY_STR || args[0]->type == RAY_CHARV)) {
            ray_t* h = q_hopen_wrap(head);           /* owned fd handle or error */
            if (!h || RAY_IS_ERR(h)) return h;
            ray_t* r = noun_index(h, args, 1);       /* int-head SYNC send */
            ray_t* c = q_hclose_wrap(h);             /* close regardless of r */
            if (c) ray_release(c);
            ray_release(h);
            return r;
        }
        return q_err(QE_TYPE);              /* file handle: no apply */
    }
    ray_t* v = ray_env_resolve(head->i64);           /* owned or NULL */
    if (!v) return q_err(QE_TYPE);
    if (RAY_IS_ERR(v)) return v;
    if (v->type == -RAY_SYM) { ray_release(v); return q_err(QE_TYPE); }
    ray_t* r = q_eval_apply_value(v, args, n);
    ray_release(v);
    return r;
}

/* one indexing step at the current depth (vector-capable per structure) */
static ray_t* step1(ray_t* cur, ray_t* idx) {
    if (cur->type == RAY_DICT) return dict_lookup(cur, idx);
    if (cur->type == RAY_TABLE) {
        ray_t* r = q_table_at(cur, idx);
        return r ? r : ray_at_fn(cur, idx);
    }
    if (!(ray_is_vec(cur) || cur->type == RAY_LIST))
        return q_err(QE_TYPE);
    return gather(cur, idx);
}

static ray_t* at_depth(ray_t* v, ray_t** ix, int64_t k);

/* null index = select ALL at this depth, continue each item with the rest */
static ray_t* all_items(ray_t* v, ray_t** ix, int64_t k) {
    int64_t len = ray_len(v);
    ray_t* out = ray_list_new(len > 0 ? len : 1);
    for (int64_t j = 0; j < len; j++) {
        ray_t* e = q_registry_elem_at(v, j);
        ray_t* r = (!e || RAY_IS_ERR(e)) ? e : store_mat(at_depth(e, ix, k));
        if (r != e && e && !RAY_IS_ERR(e)) ray_release(e);
        if (!r || RAY_IS_ERR(r)) {
            ray_release(out);
            return r ? r : q_err(QE_TYPE);
        }
        out = ray_list_append(out, r);
        ray_release(r);
    }
    return collapse(out);
}

/* Index at depth (ref/apply.md "Nulls in i" + the general case): a null/
 * elided index selects all at its depth; a list index distributes
 * (m[0 1;1] is (m[0;1];m[1;1])); an atom drills one level. */
static ray_t* at_depth(ray_t* v, ray_t** ix, int64_t k) {
    if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_TYPE);
    if (k <= 0) { ray_retain(v); return v; }
    ray_t* i0 = ix[0];
    if (!i0 || RAY_IS_NULL(i0)) {
        if (v->type == RAY_DICT && !q_table_is_keyed(v))
            return all_items(ray_dict_vals(v), ix + 1, k - 1);
        if (v->type == RAY_DICT || v->type == RAY_TABLE)
            return q_err(QE_NYI);
        if (k == 1) { ray_retain(v); return v; }
        return all_items(v, ix + 1, k - 1);
    }
    if (k == 1) return step1(v, i0);
    if (is_coll(i0)) {
        ray_t* tmp[APPLY_MAX_ARGS];
        memcpy(tmp + 1, ix + 1, (size_t)(k - 1) * sizeof *tmp);
        int64_t len = ray_len(i0);
        ray_t* out = ray_list_new(len > 0 ? len : 1);
        for (int64_t j = 0; j < len; j++) {
            ray_t* e = q_registry_elem_at(i0, j);
            if (!e || RAY_IS_ERR(e)) {
                ray_release(out);
                return e ? e : q_err(QE_TYPE);
            }
            tmp[0] = e;
            ray_t* r = store_mat(at_depth(v, tmp, k));
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) {
                ray_release(out);
                return r ? r : q_err(QE_TYPE);
            }
            out = ray_list_append(out, r);
            ray_release(r);
        }
        return collapse(out);
    }
    ray_t* nx = step1(v, i0);
    if (!nx || RAY_IS_ERR(nx)) return nx ? nx : q_err(QE_TYPE);
    ray_t* r = at_depth(nx, ix + 1, k - 1);
    ray_release(nx);
    return r;
}

static ray_t* noun_index(ray_t* v, ray_t** args, int64_t n) {
    if (n < 1) return q_err(QE_RANK);
    /* handle-as-verb (`h x`): console/file/fifo/socket dispatch lives wholly
     * in q_handles_apply (the sole handle authority) */
    if ((v->type == -RAY_I64 || v->type == -RAY_I32) && n == 1 && args[0]) {
        int64_t qh = (v->type == -RAY_I64) ? v->i64 : (int64_t)v->i32;
        return q_handles_apply(qh, args[0]);
    }
    if (v->type == -RAY_SYM) return sym_head_apply(v, args, n);
    if (v->type == -RAY_STR) {           /* stray physical string: convert, retry */
        ray_t* cv = q_charv_of_str(v);
        if (!cv || RAY_IS_ERR(cv)) return cv;
        ray_t* r = noun_index(cv, args, n);
        ray_release(cv);
        return r;
    }
    if (!(v->type == RAY_DICT || v->type == RAY_TABLE ||
          ray_is_vec(v) || v->type == RAY_LIST))
        return q_err(QE_TYPE);
    /* a list with elided items applies as a function of that rank — args
     * SUBSTITUTE into the null slots and the result stays a general list
     * (basics/application.md "Applying a list with elided items") */
    if (v->type == RAY_LIST) {
        ray_t** it = (ray_t**)ray_data(v);
        int64_t len = ray_len(v), nulls = 0;
        for (int64_t j = 0; j < len; j++)
            if (it[j] && RAY_IS_NULL(it[j])) nulls++;
        if (nulls > 0) {
            if (n != nulls) return q_err(QE_RANK);
            ray_t* out = ray_list_new(len > 0 ? len : 1);
            int64_t ai = 0;
            for (int64_t j = 0; j < len; j++) {
                ray_t* e = (it[j] && RAY_IS_NULL(it[j])) ? args[ai++] : it[j];
                if (!e) e = RAY_NULL_OBJ;
                ray_retain(e);
                e = store_mat(e);
                if (RAY_IS_ERR(e)) { ray_release(out); return e; }
                out = ray_list_append(out, e);
                ray_release(e);
            }
            return out;
        }
    }
    if (n > APPLY_MAX_ARGS) return q_err(QE_RANK);
    ray_t* r = at_depth(v, args, n);
    if (!r || RAY_IS_ERR(r)) return r ? r : q_err(QE_TYPE);
    return q_charv_out(r);     /* boundary-out: charv, never physical STR */
}

/* ===== indexed amend (ref/assign.md Indexed assign) ======================
 * Cow the spine, write the path leaf: lists via ray_list_set, typed vectors
 * via cow + the ONE element-store (kdb-strict: no silent narrowing).
 * cur/val borrowed; owned result. */

static ray_t* amend_at(ray_t* cur, ray_t** ix, int64_t k, ray_t* val) {
    if (k <= 0 || !cur || RAY_IS_ERR(cur)) return q_err(QE_TYPE);
    ray_t* i0 = ix[0];
    if (!i0 || RAY_IS_NULL(i0) || is_coll(i0) || is_container(i0))
        return q_err(QE_NYI);   /* `::`/vector-path amend: later wave */
    if (!int_atom(i0)) return q_err(QE_TYPE);
    int64_t idx = qe_elem_i64(i0);
    if (cur->type == RAY_LIST) {
        if (idx < 0 || idx >= ray_len(cur)) return q_err(QE_INDEX);
        ray_t* nv;
        if (k == 1) {
            ray_retain(val);
            nv = store_mat(val);
            if (RAY_IS_ERR(nv)) return nv;
        } else {
            nv = amend_at(((ray_t**)ray_data(cur))[idx], ix + 1, k - 1, val);
            if (!nv || RAY_IS_ERR(nv)) return nv ? nv : q_err(QE_TYPE);
        }
        ray_retain(cur);
        ray_t* nl = ray_list_set(cur, idx, nv);      /* cows; consumes on ok */
        ray_release(nv);
        if (!nl || RAY_IS_ERR(nl)) {
            ray_release(cur);                        /* cow-fail leaves cur */
            return nl ? nl : q_err(QE_OOM);
        }
        return nl;
    }
    if (ray_is_vec(cur) && cur->type != RAY_STR && cur->type != RAY_SYM) {
        if (k != 1) return q_err(QE_TYPE);
        if (idx < 0 || idx >= ray_len(cur)) return q_err(QE_INDEX);
        if (!ray_is_atom(val)) return q_err(QE_TYPE);
        if (!RAY_ATOM_IS_NULL(val) && (int8_t)-val->type != cur->type)
            return q_err(QE_TYPE);
        ray_retain(cur);
        ray_t* nv = ray_cow(cur);
        if (!nv || RAY_IS_ERR(nv)) {
            ray_release(cur);                        /* cow-fail leaves cur */
            return nv ? nv : q_err(QE_OOM);
        }
        if (store_elem(nv, idx, val) != 0) {
            ray_release(nv);
            return q_err(QE_TYPE);
        }
        if (!RAY_ATOM_IS_NULL(val)) ray_vec_set_null(nv, idx, false);
        return nv;
    }
    return q_err(QE_NYI);       /* sym-vec/dict/table amend: later */
}

ray_t* q_eval_apply_amend(ray_t* cur, ray_t** ix, int64_t k, ray_t* val) {
    if (k < 1 || k > APPLY_MAX_ARGS) return q_err(QE_RANK);
    return amend_at(cur, ix, k, val);
}

/* ===== q_eval_apply: the single entry ==================================== */

static int64_t rank_of(ray_t* fv) {
    if (fv->type == RAY_UNARY) return 1;
    if (fv->type == RAY_BINARY) return 2;
    if (q_eval_apply_carrier_kind(fv) == Q_EVAL_CAR_LAMBDA)
        return car_slots(fv)[0] ? ray_len(car_slots(fv)[0]) : 0;
    return -1;                              /* vary / deriv: no fixed rank */
}

/* a value that composes when a unary is juxtaposed onto it: an `@`/`.`
 * projection (`count@`, `(%).`) or an existing composition (`u v w@`) */
static int comp_tail(ray_t* x) {
    int k = q_eval_apply_carrier_kind(x);
    if (k == Q_EVAL_CAR_COMP) return 1;
    if (k != Q_EVAL_CAR_PROJ) return 0;
    const q_op_t* row = row_unbox(car_slots(x)[1]);
    return row && row->name[1] == '\0' &&
           (row->name[0] == '@' || row->name[0] == '.');
}

static ray_t* comp_call(ray_t* comp, ray_t** args, int64_t n) {
    ray_t** c = car_slots(comp);
    ray_t* inner = q_eval_apply(c[1], NULL, args, n);
    if (RAY_IS_ERR(inner)) return inner;
    ray_t* r = q_eval_apply(c[0], q_registry_row_of(c[0], Q_MONADIC),
                            &inner, 1);
    ray_release(inner);
    return r;
}

ray_t* q_eval_apply(ray_t* fv, const q_op_t* row, ray_t** args, int64_t n) {
    if (!fv || RAY_IS_ERR(fv)) return q_err(QE_TYPE);
    if (ray_eval_is_interrupted()) return q_err(QE_STOP);
    if (n < 0 || n > APPLY_MAX_ARGS) return q_err(QE_RANK);

    int kind = q_eval_apply_carrier_kind(fv);
    if (kind == Q_EVAL_CAR_PROJ) return proj_call(fv, args, n);
    if (kind == Q_EVAL_CAR_DERIV) {
        ray_t** c = car_slots(fv);
        return q_eval_apply_adverb((int)c[2]->i64, c[0], row_unbox(c[1]),
                                   args, n);
    }
    if (kind == Q_EVAL_CAR_COMP) return comp_call(fv, args, n);
    if (!kind && !is_fnval(fv)) {
        /* the generic null is Identity: `(::) x` / `::[x]` returns x
         * (ref/identity.md) — 101h is a unary primitive, not a noun */
        if (RAY_IS_NULL(fv) && n == 1 && args[0]) {
            ray_retain(args[0]);
            return args[0];
        }
        /* bare ENGINE lambda (rayfall-defined .rfl/serde values): base call */
        if (fv->type == RAY_LAMBDA) return call_lambda(fv, args, n);
        return noun_index(fv, args, n);
    }

    /* ref/apply.md Composition: `u v w@` — a UNARY on an `@`/`.` projection
     * (or a composition) COMPOSES rather than applying to the fn value */
    if (n == 1 && fv->type == RAY_UNARY && comp_tail(args[0]))
        return comp_new(fv, args[0]);

    int64_t holes = 0;
    for (int64_t i = 0; i < n; i++)
        if (!args[i]) holes++;
    int64_t rank = rank_of(fv);
    if (holes > 0) {
        if (rank < 0) rank = n;   /* vary/deriv: project at the called rank */
        return proj_new(fv, row, args, n, rank);
    }
    /* f[] on a niladic lambda: the lone `::` argument means "no arguments"
     * (learn/views.md `{[];}[]` evaluates) */
    if (rank == 0 && n == 1 && args[0] && RAY_IS_NULL(args[0]) &&
        kind == Q_EVAL_CAR_LAMBDA)
        return lambda_call(fv, args, 0);
    if (rank >= 0 && n < rank) return proj_new(fv, row, args, n, rank);
    if (rank >= 0 && n > rank) return q_err(QE_RANK);

    if (kind == Q_EVAL_CAR_LAMBDA) return lambda_call(fv, args, n);

    /* keyword-HOF rows route to the native adverb arms (finding 3) */
    if (row && row->adverb_hof && row->lex == QLEX_KW_INFIX && n == 2) {
        int adv = hof_adv_id(row->adverb_hof);
        if (adv >= 0) {
            const q_op_t* frow = NULL;
            if (is_fnval(args[0]))
                frow = q_registry_row_of(args[0], Q_DYADIC);
            return q_eval_apply_adverb(adv, args[0], frow, args + 1, 1);
        }
    }

    const char* fam = row ? row->family : NULL;
    if (fam) {
        if (strcmp(fam, "atomic") == 0) {
            if (n == 1 && fv->type == RAY_UNARY)
                return atomic1((ray_unary_fn)(uintptr_t)fv->i64, args[0]);
            if (n == 2 && fv->type == RAY_BINARY)
                return atomic2((ray_binary_fn)(uintptr_t)fv->i64,
                               args[0], args[1]);
        } else if (strcmp(fam, "aggregate") == 0 && n == 1 &&
                   is_container(args[0])) {
            return agg1(fv, row, args[0]);
        } else if (strcmp(fam, "map") == 0 && n == 1 &&
                   is_container(args[0])) {
            return map1(fv, row, args[0]);
        } else if (strcmp(fam, "index") == 0) {
            if (n == 1 && args[0] && args[0]->type == RAY_TABLE)
                return index_lift(fv, args, 1);
            if (n == 2 && args[1] && args[1]->type == RAY_TABLE &&
                int_atom(args[0]))
                return index_lift(fv, args, 2);
        }
    } else if (fv->attrs & RAY_FN_ATOMIC) {
        /* row-less atomic value (e.g. an each-operand): attr-driven lift */
        if (n == 1 && fv->type == RAY_UNARY &&
            (is_coll(args[0]) || is_container(args[0])))
            return atomic1((ray_unary_fn)(uintptr_t)fv->i64, args[0]);
        if (n == 2 && fv->type == RAY_BINARY &&
            (is_coll(args[0]) || is_coll(args[1]) ||
             is_container(args[0]) || is_container(args[1])))
            return atomic2((ray_binary_fn)(uintptr_t)fv->i64,
                           args[0], args[1]);
    }
    return call_kernel(fv, args, n);
}

/* ===== the public value-apply seam ======================================= */

int q_eval_apply_is_fn(ray_t* v) {
    return is_fnval(v) || v->type == RAY_LAMBDA ||
           q_eval_apply_carrier_kind(v) != 0;
}

/* Ambivalent-operator promotion for value-apply: a bare operator resolves to
 * its MONADIC registry value, so `("+";2;3)` hands the rank-1 head 2 args.
 * Swap to the same-spelling DYADIC sibling — but ONLY for wrapper values, whose
 * provenance is exact; a shared pass-through (monadic `#`==`count`) carries only
 * its first alias and could mis-route, so those (and monadic-only verbs) 'rank. */
static ray_t* apply_valence_sibling(ray_t* head, int64_t n, const q_op_t** row) {
    if (head->type == RAY_UNARY && n == 2) {
        q_provenance_t pv;
        if (q_registry_provenance(head, &pv) && pv.is_wrapper && pv.spelling) {
            ray_t* sib = q_registry_lookup_row(
                ray_sym_intern_runtime(pv.spelling, strlen(pv.spelling)),
                Q_DYADIC, row);
            if (sib && is_fnval(sib)) return sib;
        }
    }
    *row = q_registry_row_of(head, n == 1 ? Q_MONADIC : Q_DYADIC);
    return head;
}

ray_t* q_eval_apply_value(ray_t* head, ray_t** args, int64_t n) {
    if (!head || RAY_IS_ERR(head)) return q_err(QE_TYPE);
    if (q_eval_apply_is_fn(head)) {
        const q_op_t* row = NULL;
        if (is_fnval(head)) head = apply_valence_sibling(head, n, &row);
        return q_eval_apply(head, row, args, n);
    }
    return noun_index(head, args, n);
}

/* Trap (ref/apply.md): on error, a callable/null catch applies to the error
 * text, a noun catch is returned as the value.  Consumes r; e borrowed. */
static ray_t* trap_catch(ray_t* r, ray_t* e) {
    if (!r || !RAY_IS_ERR(r)) return r;
    if (!q_eval_apply_is_fn(e) && !RAY_IS_NULL(e)) {
        ray_error_free(r);
        ray_retain(e);
        return e;
    }
    const char* code = ray_err_code(r);
    ray_t* msg = ray_charv(code ? code : "",
                           code ? (int64_t)strlen(code) : 0);
    ray_error_free(r);
    if (!msg || RAY_IS_ERR(msg)) return msg ? msg : q_err(QE_OOM);
    ray_t* c = store_mat(q_eval_apply(e, NULL, &msg, 1));
    ray_release(msg);
    return c;
}

/* `f@x` — Apply At / Index At; `@[f;x;e]` traps a CALLABLE f (noun-first
 * amend forms stay 'nyi — later wave). */
ray_t* q_eval_at_wrap(ray_t** args, int64_t n) {
    if (n == 2) return store_mat(q_eval_apply_value(args[0], &args[1], 1));
    if (n == 3 && q_eval_apply_is_fn(args[0])) {
        ray_t* r = store_mat(q_eval_apply_value(args[0], &args[1], 1));
        return trap_catch(r, args[2]);
    }
    return q_err(QE_NYI);
}

/* `v . vx` — the rhs is the ARGUMENT LIST; a callable spread-applies over
 * its items, a noun depth-indexes (m . 1 2 is m[1;2]).  `.[f;vx;e]` traps
 * a callable f; noun-first amend forms stay 'nyi. */
ray_t* q_eval_dot_wrap(ray_t** args, int64_t n) {
    if (n == 3 && q_eval_apply_is_fn(args[0])) {
        ray_t* r = q_eval_dot_wrap(args, 2);
        return trap_catch(r, args[2]);
    }
    if (n != 2) return q_err(QE_NYI);       /* amend: later wave */
    ray_t* a = args[1];
    if (!a || (!ray_is_vec(a) && a->type != RAY_LIST))
        return q_err(QE_TYPE);
    int64_t k = ray_len(a);
    if (k < 1 || k > 8) return q_err(QE_RANK);
    ray_t* av[8];
    for (int64_t i = 0; i < k; i++) {
        av[i] = q_registry_elem_at(a, i);            /* owned */
        if (!av[i] || RAY_IS_ERR(av[i])) {
            ray_t* err = av[i];
            for (int64_t j = 0; j < i; j++) ray_release(av[j]);
            return err ? err : q_err(QE_TYPE);
        }
    }
    ray_t* r = store_mat(q_eval_apply_value(args[0], av, k));
    for (int64_t j = 0; j < k; j++) ray_release(av[j]);
    return r;
}

/* ===== truthiness (THE one home — owner ruling 2026-07-15) ===============
 * materialize -> exclude float/real (ref/if.md: "an atom of integral type")
 * -> cast with the SAME fn `"b"$` uses -> boolean ATOM = decided; else 'type.
 * Only 0 is false; nulls cast to 1b.  CONSUMES v. */
int q_eval_apply_truthy(ray_t* v, ray_t** err) {
    *err = NULL;
    if (!v) { *err = q_err(QE_TYPE); return 0; }
    v = ray_lazy_materialize(v);
    if (RAY_IS_ERR(v)) { *err = v; return 0; }
    int8_t t = v->type < 0 ? (int8_t)-v->type : v->type;
    if (t == RAY_F64 || t == RAY_F32) {
        ray_release(v);
        *err = q_err(QE_TYPE);
        return 0;
    }
    ray_t* b = q_dollar_cast(RAY_BOOL, v);
    ray_release(v);
    if (!b || RAY_IS_ERR(b)) { *err = b ? b : q_err(QE_TYPE); return 0; }
    if (b->type != -RAY_BOOL) { ray_release(b); *err = q_err(QE_TYPE); return 0; }
    int go = b->b8 != 0;
    ray_release(b);
    return go;
}

/* ===== carrier display (q_fmt hook) ====================================== */

int q_eval_apply_carrier_fmt(ray_t* v, char* buf, size_t bufsz) {
    int kind = q_eval_apply_carrier_kind(v);
    if (!kind || bufsz == 0) return 0;
    ray_t** c = car_slots(v);
    if (kind == Q_EVAL_CAR_LAMBDA) {
        ray_t* src = c[2];
        if (src && src->type == -RAY_STR)
            snprintf(buf, bufsz, "%.*s", (int)ray_str_len(src),
                     ray_str_ptr(src));
        else
            snprintf(buf, bufsz, "{..}");
        return 1;
    }
    if (kind == Q_EVAL_CAR_DERIV) {
        int adv = (int)c[2]->i64;
        const q_op_t* frow = row_unbox(c[1]);
        char fb[128] = "";
        if (frow) snprintf(fb, sizeof fb, "%s", frow->name);
        else if (c[0]) q_fmt(c[0], fb, sizeof fb);
        snprintf(buf, bufsz, "%s%s", fb,
                 (adv >= 0 && adv < 6) ? ADVERB_NAMES[adv] : "");
        return 1;
    }
    if (kind == Q_EVAL_CAR_COMP) {
        char ub[128] = "", gb[128] = "";
        if (c[0]) q_fmt(c[0], ub, sizeof ub);
        if (c[1]) q_fmt(c[1], gb, sizeof gb);
        snprintf(buf, bufsz, "%s %s", ub, gb);
        return 1;
    }
    /* projection: verbatim head + the bound-argument echo, holes empty —
     * {x+y}[1;], +[;2] (kdb shape, owner ruling 2026-07-23) */
    char fb[256] = "";
    const q_op_t* frow = row_unbox(c[1]);
    if (frow) snprintf(fb, sizeof fb, "%s", frow->name);
    else if (c[0]) q_fmt(c[0], fb, sizeof fb);
    size_t off = 0;
    int64_t slots = ray_len(v) - 2;
    #define PUT(...) do { \
        if (off < bufsz) { \
            int w = snprintf(buf + off, bufsz - off, __VA_ARGS__); \
            off += (w > 0) ? (size_t)w : 0; \
        } \
    } while (0)
    PUT("%s[", fb);
    for (int64_t i = 0; i < slots; i++) {
        if (i) PUT(";");
        if (c[2 + i]) {
            char ab[128] = "";
            q_fmt(c[2 + i], ab, sizeof ab);
            PUT("%s", ab);
        }
    }
    PUT("]");
    #undef PUT
    return 1;
}
