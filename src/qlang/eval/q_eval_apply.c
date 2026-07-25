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
#include "qlang/q_builtins.h"  /* q_count_long — q `count` for C callers, hot lane */
#include "qlang/q_registry.h"
#include "qlang/q_type.h"     /* q_type_is_keyed — the type axis home */
#include "qlang/q_parse_internal.h"
#include "qlang/q_fmt.h"
#include "qlang/q_handles.h"
#include "qlang/net/q_ws.h"
#include "qlang/net/q_http_client.h"
#include "qlang/ops/q_bang.h"
#include "qlang/ops/q_dollar.h"
#include "qlang/ops/q_index.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "lang/internal.h"   /* call_lambda — bare engine-lambda application */
#include "ops/ops.h"
#include "table/dict.h"
#include "table/sym.h"
#include <assert.h>
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

/* An iterated dict argument IS its values — for the maps, and for the
 * accumulators by ref/accumulators.md:414 ("Over ... reduces lists AND
 * DICTIONARIES to atoms").  A keyed table is a dict too: its values are a
 * table, whose own items are rows, so the two laws compose and it needs no
 * arm of its own. */
static ray_t* iter_dict_vals(ray_t* x) {
    return (x && x->type == RAY_DICT) ? ray_dict_vals(x) : NULL;
}

/* re-key a UNIFORM result (each, scan, prior) onto x's keys; consumes r.  An
 * aggregate (over) does NOT re-key — it has reduced to an atom. */
static ray_t* dict_rekey(ray_t* x, ray_t* r) {
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* k = ray_dict_keys(x);
    ray_retain(k);
    return ray_dict_new(k, r);
}

static ray_t* collapse(ray_t* l) {          /* consumes l, returns owned */
    ray_t* c = q_list_collapse(l);
    ray_release(l);
    return c;
}

/* THE force home (materialization phase 1): the ONLY site that turns a lazy
 * DAG handle into a concrete value (ray_lazy_materialize CONSUMES its input and
 * is a no-op on a concrete value).  Errors/NULL pass through. */
ray_t* q_eval_apply_concrete(ray_t* r) {
    if (r && ray_is_lazy(r)) return ray_lazy_materialize(r);
    return r;
}

#ifdef DEBUG
void q_eval_apply_assert_concrete(ray_t* v) { assert(!ray_is_lazy(v)); }
#endif

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

int q_eval_apply_store_elem(ray_t* vec, int64_t i, ray_t* e) {
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

    ray_t* e0_in = q_index_elem_at(arg, 0);
    ray_t* e0 = unary_elem(fn, e0_in);
    if (e0 != e0_in) ray_release(e0_in);
    if (RAY_IS_ERR(e0)) return e0;

    int8_t out_type = (int8_t)(-e0->type);
    if (!is_boxed && ray_is_atom(e0) && typed_out_ok(out_type)) {
        ray_t* vec = ray_vec_new(out_type, len);
        if (RAY_IS_ERR(vec)) { ray_release(e0); return vec; }
        vec->len = len;
        q_eval_apply_store_elem(vec, 0, e0);
        ray_release(e0);
        for (int64_t i = 1; i < len; i++) {
            if ((i & 0xFFF) == 0 && ray_eval_is_interrupted()) {
                ray_release(vec);
                return q_err(QE_STOP);
            }
            ray_t* e = q_index_elem_at(arg, i);
            ray_t* r = RAY_IS_ERR(e) ? e : fn(e);
            if (r != e) ray_release(e);
            if (RAY_IS_ERR(r)) { ray_release(vec); return r; }
            if (q_eval_apply_store_elem(vec, i, r) != 0) {
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
        ray_t* e = q_index_elem_at(arg, i);
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

    ray_t* a0 = lc ? q_index_elem_at(l, 0) : l;
    ray_t* b0 = rc ? q_index_elem_at(r, 0) : r;
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
        q_eval_apply_store_elem(vec, 0, e0);
        ray_release(e0);
        for (int64_t i = 1; i < len; i++) {
            if ((i & 0xFFF) == 0 && ray_eval_is_interrupted()) {
                ray_release(vec);
                return q_err(QE_STOP);
            }
            ray_t* a = lc ? q_index_elem_at(l, i) : l;
            ray_t* b = rc ? q_index_elem_at(r, i) : r;
            ray_t* e = (RAY_IS_ERR(a) || RAY_IS_ERR(b)) ? q_err(QE_TYPE)
                                                        : fn(a, b);
            if (lc && a != e) ray_release(a);
            if (rc && b != e) ray_release(b);
            if (RAY_IS_ERR(e)) { ray_release(vec); return e; }
            if (q_eval_apply_store_elem(vec, i, e) != 0) {
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
        ray_t* a = lc ? q_index_elem_at(l, i) : l;
        ray_t* b = rc ? q_index_elem_at(r, i) : r;
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

/* the per-column walk lives in the table home (q_table_map_cols); these colfns
 * force each column at the container-append boundary (q_eval_apply_concrete). */
static ray_t* atomic1_col(void* ctx, ray_t* col) {
    return q_eval_apply_concrete(atomic1((ray_unary_fn)(uintptr_t)ctx, col));
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
        return q_table_map_cols(atomic1_col, (void*)(uintptr_t)f, x);
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
        ray_t* k = q_index_elem_at(uk, j);
        int64_t ix = ray_dict_find_idx(x, k);
        int64_t iy = ray_dict_find_idx(y, k);
        ray_release(k);
        ray_t* r;
        if (ix >= 0 && iy >= 0) {
            ray_t* ex = q_index_elem_at(vx, ix);
            ray_t* ey = q_index_elem_at(vy, iy);
            r = binary_elem(f, ex, ey);
            if (r != ex) ray_release(ex);
            if (r != ey) ray_release(ey);
        } else if (ix >= 0) {
            r = q_index_elem_at(vx, ix);
        } else {
            r = q_index_elem_at(vy, iy);
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
    return q_eval_apply_concrete(c->other_left ? atomic2(c->f, c->other, col)
                                   : atomic2(c->f, col, c->other));
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
        return q_table_map_cols(atomic2_col, &ctx, t);
    }
    return map_binary(f, x, y);
}

/* ===== L3 aggregate lift ================================================= */

static ray_t* agg1(ray_t* fv, const q_op_t* row, ray_t* x) {
    if (x && x->type == RAY_DICT) {
        if (q_type_is_keyed(x)) return agg1(fv, row, ray_dict_vals(x));
        ray_t* v = ray_dict_vals(x);
        return q_eval_apply(fv, row, &v, 1);
    }
    if (x && x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* ks = ray_sym_vec_new(RAY_SYM_W64, nc > 0 ? nc : 1);
        ray_t* vs = ray_list_new(nc > 0 ? nc : 1);
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(x, c);      /* borrowed */
            ray_t* r = q_eval_apply_concrete(q_eval_apply(fv, row, &col, 1));
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
        if (q_type_is_keyed(x)) nv = map1(fv, row, v);
        else                     nv = q_eval_apply_concrete(q_eval_apply(fv, row, &v, 1));
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
            ray_t* r = q_eval_apply_concrete(q_eval_apply(fv, row, &col, 1));
            if (RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_table_add_col(out, ray_table_col_name(x, c), r);
            ray_release(r);
        }
        return out;
    }
    return q_err(QE_TYPE);
}

/* ===== kernel invocation ================================================= */

/* boundary seam + wrapper-entry tripwire (materialization phase 1): a wrapper
 * body receives ONLY concrete args, so force here and assert the postcondition
 * (ASan-enforced — a phase-2 leak trips it).  A lazy-AWARE kernel (sum et al.)
 * is the exception: it takes the handle to chain the DAG, and is the one
 * RAY_FN_LAZY_AWARE reader. */
static ray_t* force_arg(ray_t* fv, ray_t* a, int* owned) {
    *owned = 0;
    if (fv->attrs & RAY_FN_LAZY_AWARE) return a;
    if (a && ray_is_lazy(a)) {
        ray_retain(a);
        a = q_eval_apply_concrete(a);
        *owned = 1;
    }
    Q_ASSERT_CONCRETE(a);
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

/* ===== L4 index lift: op x = x @ op til count x (ONE gather) =============
 * A dictionary is ENTRIES — domain!range — and a keyed table is the same law
 * with a TABLE in each slot (basics/dictsandtables.md), so one index derived
 * from the DOMAIN's count drives both halves and the container is rebuilt
 * from them. */

/* gather x at idx through the one index home; an empty selection keeps x's
 * element type (`` type key `a _ `a!1 `` is 11h, not 0h) */
static ray_t* gather_at(ray_t* x, ray_t* idx) {
    ray_t* g = q_eval_apply_concrete(q_index_at(x, &idx, 1));
    return q_typed_empty_like(g, x);
}

static ray_t* index_lift(ray_t* fv, ray_t** args, int64_t n) {
    ray_t* x = (n == 2) ? args[1] : args[0];
    ray_t* dom = (x->type == RAY_DICT) ? ray_dict_keys(x) : NULL;
    int64_t n_ent = q_count_long(dom ? dom : x);   /* q count, one home */
    if (n_ent < 0) return q_err(QE_TYPE);
    ray_t* nrv = ray_i64(n_ent);
    ray_t* til = ray_til_fn(nrv);
    ray_release(nrv);
    if (!til || RAY_IS_ERR(til)) return til ? til : q_err(QE_TYPE);
    ray_t* iargs[2];
    if (n == 2) { iargs[0] = args[0]; iargs[1] = til; }
    else        { iargs[0] = til; }
    /* the index is a VALUE the gather consumes, never a chain link */
    ray_t* idx = q_eval_apply_concrete(call_kernel(fv, iargs, n));
    ray_release(til);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_TYPE);
    ray_t* r = gather_at(dom ? dom : x, idx);
    if (dom && r && !RAY_IS_ERR(r)) {
        ray_t* nv = gather_at(ray_dict_vals(x), idx);
        /* rebuild through the `!` home, so a lifted slice is representationally
         * the dict `(op key d)!(op value d)` builds — the L4 law is `~`-true */
        ray_t* nd = (nv && !RAY_IS_ERR(nv)) ? q_bang(r, nv) : nv;
        ray_release(r);
        if (nv) ray_release(nv);
        r = nd;
    }
    ray_release(idx);
    return r ? r : q_err(QE_TYPE);
}

/* ===== adverbs: native application + manifest monomorphization =========== */

static int64_t rank_of(ray_t* fv);

/* ----- accumulators: Scan and Over (ref/accumulators.md) -----------------
 * "They have the same syntax and perform the same computation.  But where the
 * Scan-derived functions return the result of each evaluation, those of Over
 * return only the last" (:37) — so ONE engine carries a `keep` flag and every
 * arm below is shared.  `acc_t` is that running result. */
typedef struct { int keep; ray_t* list; ray_t* last; } acc_t;

static void acc_init(acc_t* a, int keep) {
    a->keep = keep;
    a->list = keep ? ray_list_new(0) : NULL;
    a->last = NULL;
}

/* borrows v; returns an owned error when the append failed, else NULL */
static ray_t* acc_push(acc_t* a, ray_t* v) {
    if (a->last) ray_release(a->last);
    ray_retain(v);
    a->last = v;
    if (!a->keep) return NULL;
    a->list = ray_list_append(a->list, v);
    if (!RAY_IS_ERR(a->list)) return NULL;
    ray_t* e = a->list;
    a->list = NULL;
    return e;
}

/* the owned result; an owned `err` wins and the partials are dropped */
static ray_t* acc_finish(acc_t* a, ray_t* err) {
    ray_t* r = err;
    if (!r && a->keep) { r = collapse(a->list); a->list = NULL; }
    if (!r) { r = a->last; a->last = NULL; }
    if (a->list) ray_release(a->list);
    if (a->last) ray_release(a->last);
    return r ? r : q_err(QE_TYPE);
}

/* Converge: evaluate until two successive results match, or one matches x
 * (:97).  `~` is the match home, so tolerance is decided in one place. */
static ray_t* acc_converge(ray_t* fv, const q_op_t* frow, ray_t* x, int keep) {
    acc_t a;
    acc_init(&a, keep);
    ray_retain(x);
    ray_t* x0 = q_eval_apply_concrete(x);
    ray_t* cur = x0;
    ray_retain(cur);
    ray_t* err = acc_push(&a, cur);
    while (!err) {
        ray_t* nx = q_eval_apply(fv, frow, &cur, 1);
        if (RAY_IS_ERR(nx)) { err = nx; break; }
        int stop = q_match_rec(nx, cur) || q_match_rec(nx, x0);
        ray_release(cur);
        cur = nx;
        if (stop) break;
        err = acc_push(&a, cur);
    }
    ray_release(cur);
    ray_release(x0);
    return acc_finish(&a, err);
}

/* Do (:134) and While (:193): x is the first result, then `n` more, or one per
 * pass while the unary truth map `t` holds on the running result. */
static ray_t* acc_iterate(ray_t* fv, const q_op_t* frow, ray_t* t, int64_t n,
                          ray_t* x, int keep) {
    const q_op_t* trow = (t && is_fnval(t)) ? q_registry_row_of(t, Q_MONADIC)
                                           : NULL;
    acc_t a;
    acc_init(&a, keep);
    ray_retain(x);
    ray_t* cur = q_eval_apply_concrete(x);
    ray_t* err = acc_push(&a, cur);
    for (int64_t i = 0; !err && (t || i < n); i++) {
        if (t) {
            int go = q_eval_apply_truthy(q_eval_apply(t, trow, &cur, 1), &err);
            if (err || !go) break;
        }
        ray_t* nx = q_eval_apply(fv, frow, &cur, 1);
        ray_release(cur);
        if (RAY_IS_ERR(nx)) { cur = NULL; err = nx; break; }
        cur = nx;
        err = acc_push(&a, cur);
    }
    if (cur) ray_release(cur);
    return acc_finish(&a, err);
}

/* Empty right argument (:385), the value never evaluated.  With a seed Scan
 * and Over diverge: Over reduces to the left argument (:445) where Scan, being
 * uniform, returns `()` (:405).  Applied as a unary they agree and the answer
 * comes from the VALUE — its identity element, else (for a list value) an
 * empty of its own type, else `()`.  NULL means "let the registered aggregate
 * answer": `|`/`&` monomorphize but have no doc-published identity. */
static ray_t* acc_empty(ray_t* fv, const q_op_t* frow, ray_t* seed, int keep,
                        int has_mono) {
    if (seed) {
        if (keep) return ray_list_new(0);
        ray_retain(seed);
        return seed;
    }
    ray_t* id = frow ? q_ops_acc_identity(frow->name) : NULL;
    if (id) return id;
    return has_mono ? NULL : q_typed_empty_like(ray_list_new(0), fv);
}

/* item i of an iterated argument; a non-collection is held whole and
 * broadcast, as for the maps (`{x+y*z}\[1000 2000;5 10 15 20;3]`, :355) */
static ray_t* acc_item(ray_t* v, int64_t i) {
    if (!q_type_is_iter(v)) { ray_retain(v); return v; }
    return q_index_elem_at(v, i);
}

/* Binary and higher-rank values (:217, :322): the running left argument is
 * `seed`, or — applied as a unary with no known identity — the first item,
 * which is then the first result (:272).  it[0..nit) are the iterated
 * arguments; the count is the longest of them. */
static ray_t* acc_reduce(ray_t* fv, const q_op_t* frow, ray_t* seed,
                         ray_t** it, int64_t nit, int keep) {
    if (nit < 1 || nit + 1 > APPLY_MAX_ARGS) return q_err(QE_RANK);
    ray_t* iv[APPLY_MAX_ARGS];
    ray_t* av[APPLY_MAX_ARGS];
    ray_t* dk = NULL;                 /* first dict argument: the result's keys */
    int64_t len = -1;
    for (int64_t p = 0; p < nit; p++) {
        ray_retain(it[p]);
        iv[p] = q_eval_apply_concrete(it[p]);   /* an iterated DAG has no items */
        ray_t* dv = iter_dict_vals(iv[p]);
        if (dv) {                     /* iterate the VALUES, not the entries */
            ray_retain(dv);
            if (!dk) { dk = iv[p]; ray_retain(dk); }
            ray_release(iv[p]);
            iv[p] = dv;
        }
        if (!q_type_is_iter(iv[p])) continue;
        int64_t c = q_count_long(iv[p]);
        if (len < 0 || c > len) len = c;
    }
    ray_t* r;
    if (len == 0) {
        r = acc_empty(fv, frow, seed, keep, 0);      /* :385, seeded branch */
    } else if (len < 0) {
        /* nothing to iterate over: one evaluation, and the result stays an
         * atom — the derived function is uniform */
        if (seed) {
            av[0] = seed;
            for (int64_t p = 0; p < nit; p++) av[p + 1] = iv[p];
            r = q_eval_apply(fv, frow, av, nit + 1);
        } else {
            ray_retain(iv[0]);
            r = iv[0];
        }
    } else {
        acc_t a;
        acc_init(&a, keep);
        ray_t* err = NULL;
        ray_t* cur;
        int64_t i0 = 0;
        if (seed) { ray_retain(seed); cur = seed; }
        else      { cur = acc_item(iv[0], 0); i0 = 1; err = acc_push(&a, cur); }
        for (int64_t i = i0; !err && i < len; i++) {
            av[0] = cur;
            for (int64_t p = 0; p < nit; p++) av[p + 1] = acc_item(iv[p], i);
            ray_t* nx = q_eval_apply(fv, frow, av, nit + 1);
            for (int64_t p = 0; p < nit; p++) ray_release(av[p + 1]);
            ray_release(cur);
            if (RAY_IS_ERR(nx)) { cur = NULL; err = nx; break; }
            cur = nx;
            err = acc_push(&a, cur);
        }
        if (cur) ray_release(cur);
        r = acc_finish(&a, err);
    }
    for (int64_t p = 0; p < nit; p++) ray_release(iv[p]);
    if (dk) {
        if (keep) r = dict_rekey(dk, r);
        ray_release(dk);
    }
    return r;
}

/* An accumulator's value may be a function, a list or a dictionary — the last
 * two being finite-state machines (:59) whose rank is their depth: a simple
 * vector maps one index, a list of lists two (`7 m\c`, :252). */
static int64_t acc_rank(ray_t* fv) {
    if (fv->type == RAY_LIST)
        return (ray_len(fv) && is_coll(((ray_t**)ray_data(fv))[0])) ? 2 : 1;
    if (is_container(fv) || ray_is_vec(fv)) return 1;
    return rank_of(fv);
}

/* The manifest mono column: `f/` of a registered dyad IS its aggregate kernel
 * — `+/` joins the DAG as OP_SUM where a naive fold would materialize and go
 * scalar.  `\` reads its OWN column: `,` reduces to raze and scans to nothing,
 * so no spelling rule derives one from the other. */
static const char* acc_mono_name(const q_op_t* frow, int keep) {
    return !frow ? NULL : (keep ? frow->mono_scan : frow->mono);
}

static ray_t* acc_mono(const q_op_t* frow, int keep, const q_op_t** mrow) {
    const char* nm = acc_mono_name(frow, keep);
    if (!nm) return NULL;
    ray_t* mv = q_registry_lookup_row(
        ray_sym_intern_runtime(nm, strlen(nm)), Q_MONADIC, mrow);
    return (mv && is_fnval(mv)) ? mv : NULL;
}

/* Unary application of a non-unary value (:259): the seed is the value's
 * identity element when q knows one, else the first item — which is then the
 * first result. */
static ray_t* acc_unary(ray_t* fv, const q_op_t* frow, ray_t* x, int keep) {
    ray_retain(x);
    x = q_eval_apply_concrete(x);              /* materialize at the boundary */
    const q_op_t* mrow = NULL;
    /* boxed lists take the native fold — the aggregate wrappers' boxed arms
     * ride base call_fn plumbing (finding 5) */
    ray_t* dv = iter_dict_vals(x);
    if (dv) {                    /* the values are the domain; Scan re-keys */
        ray_t* dr = acc_unary(fv, frow, dv, keep);
        if (keep) dr = dict_rekey(x, dr);
        ray_release(x);
        return dr;
    }
    ray_t* mv = (x->type == RAY_LIST) ? NULL : acc_mono(frow, keep, &mrow);
    ray_t* r;
    if (q_type_is_iter(x) && q_count_long(x) == 0) {
        r = acc_empty(fv, frow, NULL, keep, mv != NULL);
        if (!r) r = q_eval_apply(mv, mrow, &x, 1);
    } else if (mv)
        r = q_eval_apply(mv, mrow, &x, 1);
    else {
        ray_t* id = frow ? q_ops_acc_identity(frow->name) : NULL;
        r = acc_reduce(fv, frow, id, &x, 1, keep);
        if (id) ray_release(id);
    }
    ray_release(x);
    return r;
}

/* Scan `\` and Over `/`: one classifier over the doc's own taxonomy — the
 * value's rank picks unary-value control flow (Converge / Do / While, :81)
 * or the reduction of a binary-or-higher value (:217, :322). */
static ray_t* acc_apply(ray_t* fv, const q_op_t* frow, ray_t** args,
                        int64_t n, int keep) {
    if (n < 1) return q_err(QE_RANK);
    /* the mono column is the ROW's property and is read before the value:
     * `|`'s dyad is a deferred cell, so its operand value is monadic `reverse`
     * and a rank test would read Converge where the manifest says max */
    if (n == 1 && acc_mono_name(frow, keep))
        return acc_unary(fv, frow, args[0], keep);
    /* a LIST value applied as a unary to an empty argument yields an empty of
     * the VALUE's own type, unevaluated (:436) — this outranks the rank test,
     * which would otherwise read a simple vector as a Converge machine */
    if (n == 1 && !q_eval_apply_is_fn(fv) && is_coll(args[0]) &&
        ray_len(args[0]) == 0)
        return q_typed_empty_like(ray_list_new(0), fv);
    int64_t rank = acc_rank(fv);
    /* a variadic value reads as the UNARY form in both binary shapes:
     * `5 enlist\1` (:151) is Do-5 of unary enlist, not enlist[5;1].  Past two
     * arguments there is no unary reading left and a variadic ternary
     * (`ssr\[s;x;y]`) reduces */
    if (rank == 1 || (rank < 0 && n <= 2)) {
        if (n == 1) return acc_converge(fv, frow, args[0], keep);
        if (n != 2) return q_err(QE_RANK);
        /* the left argument separates the two binary forms: an integer counts
         * the passes, anything else is the unary truth map (:95) */
        if (!int_atom(args[0]))
            return acc_iterate(fv, frow, args[0], 0, args[1], keep);
        int64_t k = q_type_as_i64(args[0]);
        if (k < 0) return q_err(QE_DOMAIN);
        return acc_iterate(fv, frow, NULL, k, args[1], keep);
    }
    if (n == 1) return acc_unary(fv, frow, args[0], keep);
    if (rank >= 2 && n != rank) return q_err(QE_RANK);
    return acc_reduce(fv, frow, args[0], args + 1, n - 1, keep);
}

/* Each, Each Left and Each Right are ONE law (ref/maps.md: `x f\:y` is
 * `f[;y] each x`).  `mask` names the ITERATED positions — fixed by the
 * operator, never auto-detected — read at i; every other position is held
 * whole, and a non-collection in a masked slot leaves nothing to iterate. */
static ray_t* map_zip(ray_t* fv, const q_op_t* frow, ray_t** args, int64_t n,
                      uint64_t mask) {
    if (n < 1 || n > APPLY_MAX_ARGS) return q_err(QE_RANK);
    ray_t* av[APPLY_MAX_ARGS];
    uint64_t iter = 0;
    int64_t len = -1;
    ray_t* r = NULL;
    for (int64_t p = 0; p < n; p++) {
        ray_retain(args[p]);
        av[p] = q_eval_apply_concrete(args[p]);   /* an iterated DAG has no items */
        if (!(mask >> p & 1) || !q_type_is_iter(av[p])) continue;
        iter |= (uint64_t)1 << p;
        /* the scan must run to the end so every av[p] is populated for the
         * release below — so record the FIRST mismatch only (an error is an
         * allocation the refcount system does not track: overwriting leaks) */
        if (len < 0) len = q_count_long(av[p]);
        else if (len != q_count_long(av[p]) && !r) r = q_err(QE_LENGTH);
    }
    if (!r && len < 0) r = q_eval_apply(fv, frow, av, n);
    /* a map is uniform: an empty iterated side returns the GENERIC empty list
     * without an evaluation — `type (2*')til 0` is 0h, not 7h (ref/maps.md) */
    if (!r && len == 0) r = ray_list_new(0);
    if (!r) {
        ray_t* l = ray_list_new(len);
        ray_t* ev[APPLY_MAX_ARGS];
        for (int64_t i = 0; i < len; i++) {
            for (int64_t p = 0; p < n; p++)
                ev[p] = (iter >> p & 1) ? q_index_elem_at(av[p], i) : av[p];
            ray_t* e = q_eval_apply_concrete(q_eval_apply(fv, frow, ev, n));
            for (int64_t p = 0; p < n; p++)
                if (iter >> p & 1) ray_release(ev[p]);
            if (RAY_IS_ERR(e)) { ray_release(l); l = e; break; }
            l = ray_list_append(l, e);
            ray_release(e);
        }
        r = RAY_IS_ERR(l) ? l : collapse(l);
    }
    for (int64_t p = 0; p < n; p++) ray_release(av[p]);
    return r;
}

static ray_t* each1(ray_t* fv, const q_op_t* frow, ray_t* x) {
    ray_t* dv = iter_dict_vals(x);
    if (dv) return dict_rekey(x, each1(fv, frow, dv));
    return map_zip(fv, frow, &x, 1, 1);
}

/* Case (ref/maps.md "Case"): an INTEGER VECTOR at the `'` head is not a value
 * to apply — per index it picks WHICH argument to read, r[i] = args[sel[i]][i].
 * Atom arguments are infinitely repeated and extra arguments are not a rank
 * error, so the count comes from the selector alone. */
static int int_vec(ray_t* v) {
    return v && (v->type == RAY_I64 || v->type == RAY_I32 ||
                 v->type == RAY_I16);
}

static ray_t* case_apply(ray_t* sel, ray_t** args, int64_t n) {
    if (n < 1 || n > APPLY_MAX_ARGS) return q_err(QE_RANK);
    ray_t* av[APPLY_MAX_ARGS];
    for (int64_t p = 0; p < n; p++) {
        ray_retain(args[p]);
        av[p] = q_eval_apply_concrete(args[p]);   /* a picked-from DAG has no items */
    }
    int64_t len = ray_len(sel);
    ray_t* l = ray_list_new(len);
    for (int64_t i = 0; i < len && !RAY_IS_ERR(l); i++) {
        ray_t* si = q_index_elem_at(sel, i);
        int64_t k = int_atom(si) ? q_type_as_i64(si) : -1;
        ray_release(si);
        ray_t* e;
        if (k < 0 || k >= n || !av[k]) e = q_err(QE_INDEX);
        else if (is_coll(av[k])) e = q_index_elem_at(av[k], i);
        else { ray_retain(av[k]); e = av[k]; }
        if (RAY_IS_ERR(e)) { ray_release(l); l = e; break; }
        l = ray_list_append(l, e);
        ray_release(e);
    }
    for (int64_t p = 0; p < n; p++) ray_release(av[p]);
    return RAY_IS_ERR(l) ? l : collapse(l);
}

/* Each Prior: fv between each item and the one before it.  Applied as a
 * binary the left argument IS the seed; applied as a unary the seed is the
 * value's identity element when q knows one, else `first 0#x` (ref/maps.md
 * Each Prior). */
static ray_t* prior_each(ray_t* fv, const q_op_t* frow, ray_t* seed, ray_t* x) {
    ray_t* dv = iter_dict_vals(x);               /* prior is uniform: re-keys */
    if (dv) return dict_rekey(x, prior_each(fv, frow, seed, dv));
    ray_retain(x);
    x = q_eval_apply_concrete(x);
    ray_t* prev = seed;
    if (prev) ray_retain(prev);
    else {
        prev = frow ? q_ops_acc_identity(frow->name) : NULL;
        if (!prev && ray_is_vec(x)) prev = ray_typed_null((int8_t)-x->type);
        /* `first 0#x` for a table is its all-null row — the out-of-range read */
        if (!prev && x->type == RAY_TABLE) prev = q_index_elem_at(x, q_count_long(x));
        if (!prev) { prev = RAY_NULL_OBJ; ray_retain(prev); }
    }
    ray_t* r;
    if (!q_type_is_iter(x)) {
        ray_t* av[2] = { x, prev };
        r = q_eval_apply(fv, frow, av, 2);
    } else {
        int64_t len = q_count_long(x);
        ray_t* l = ray_list_new(len);
        for (int64_t i = 0; i < len; i++) {
            ray_t* cur = q_index_elem_at(x, i);
            ray_t* av[2] = { cur, prev };
            ray_t* e = q_eval_apply_concrete(q_eval_apply(fv, frow, av, 2));
            ray_release(prev);
            prev = cur;
            if (RAY_IS_ERR(e)) { ray_release(l); l = e; break; }
            l = ray_list_append(l, e);
            ray_release(e);
        }
        r = RAY_IS_ERR(l) ? l : collapse(l);
    }
    ray_release(prev);
    ray_release(x);
    return r;
}

ray_t* q_eval_apply_adverb(int adv, ray_t* fv, const q_op_t* frow,
                           ray_t** args, int64_t n) {
    if (!fv) return q_err(QE_TYPE);
    /* an elided argument projects the DERIVED value, not the underlying one:
     * `2*'` is Each on a projection of `*`, and only the empty case tells them
     * apart (`type (2*')til 0` is 0h where `type (2*)til 0` is 7h) */
    for (int64_t i = 0; i < n; i++) {
        if (args[i]) continue;
        ray_t* d = q_eval_apply_deriv_new(adv, fv, frow);
        if (RAY_IS_ERR(d)) return d;
        ray_t* p = proj_new(d, NULL, args, n, n);
        ray_release(d);
        return p;
    }
    if (adv == 1 || adv == 2) return acc_apply(fv, frow, args, n, adv == 2);
    if (adv == 0) {                                        /* `'` each */
        if (int_vec(fv)) return case_apply(fv, args, n);
        if (n == 1) return each1(fv, frow, args[0]);
        return map_zip(fv, frow, args, n, ~(uint64_t)0);
    }
    if (adv == 3) {                                        /* `':` */
        /* a rank-1 value makes `':` Each Parallel, a rank-2 one Each Prior
         * (ref/maps.md); we have no secondary tasks, so parallel IS each */
        if (n == 1 && rank_of(fv) == 1) return each1(fv, frow, args[0]);
        if (n == 1) return prior_each(fv, frow, NULL, args[0]);
        if (n == 2) return prior_each(fv, frow, args[0], args[1]);
        return q_err(QE_RANK);
    }
    if (adv == 4 && n == 2) return map_zip(fv, frow, args, 2, 2);   /* /: right */
    if (adv == 5 && n == 2) return map_zip(fv, frow, args, 2, 1);   /* \: left  */
    return q_err(QE_NYI);                    /* the vs/sv unary forms: later */
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
        ray_t* p = q_index_elem_at(params, i);          /* sym atom */
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

/* ===== noun-head application ============================================
 * APPLICATION concerns only (handles, sym heads, elided lists, the string
 * boundary) — all DATA indexing is ops/q_index.c, the one index/amend home;
 * results cross into q-space through q_str_charv_out. */

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
        ray_t* cv = q_str_charv_of_str(v);
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
                e = q_eval_apply_concrete(e);
                if (RAY_IS_ERR(e)) { ray_release(out); return e; }
                out = ray_list_append(out, e);
                ray_release(e);
            }
            return out;
        }
    }
    if (n > APPLY_MAX_ARGS) return q_err(QE_RANK);
    ray_t* r = q_index_at(v, args, n);               /* the ONE index home */
    if (!r || RAY_IS_ERR(r)) return r ? r : q_err(QE_TYPE);
    return q_str_charv_out(r);     /* boundary-out: charv, never physical STR */
}

/* ===== q_eval_apply: the single entry ==================================== */

static int64_t rank_of(ray_t* fv) {
    if (fv->type == RAY_UNARY) return 1;
    if (fv->type == RAY_BINARY) return 2;
    int kind = q_eval_apply_carrier_kind(fv);
    if (kind == Q_EVAL_CAR_LAMBDA)
        return car_slots(fv)[0] ? ray_len(car_slots(fv)[0]) : 0;
    /* a projection's rank is the slots it still wants; a composition's is its
     * inner value's (`mmu[;b]` is unary, so `':` reads it as Each Parallel) */
    if (kind == Q_EVAL_CAR_PROJ) {
        int64_t slots = ray_len(fv) - 2, holes = 0;
        for (int64_t i = 0; i < slots; i++)
            if (!car_slots(fv)[2 + i]) holes++;
        return holes;
    }
    if (kind == Q_EVAL_CAR_COMP) return rank_of(car_slots(fv)[1]);
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

/* `'` in BRACKET form with VALUES (ref/compose.md): one value derives Each
 * (`'[count]` is `count'`), two or more COMPOSE — right to left, each outer
 * value unary, the derived rank the innermost value's. */
static ray_t* apply_valence_sibling(ray_t* head, int64_t n, const q_op_t** row);

static ray_t* compose_apply(ray_t** args, int64_t n) {
    if (n < 1) return q_err(QE_RANK);
    for (int64_t i = 0; i < n; i++)
        if (!q_eval_apply_is_fn(args[i])) return q_err(QE_TYPE);
    if (n == 1)
        return q_eval_apply_deriv_new(0, args[0],
                                      is_fnval(args[0])
                                          ? q_registry_row_of(args[0], Q_MONADIC)
                                          : NULL);
    ray_t* r = args[n - 1];
    ray_retain(r);
    for (int64_t i = n - 2; i >= 0; i--) {
        int64_t k = rank_of(args[i]);
        ray_t* c = (k >= 0 && k != 1) ? q_err(QE_RANK) : comp_new(args[i], r);
        ray_release(r);
        if (RAY_IS_ERR(c)) return c;
        r = c;
    }
    return r;
}

static ray_t* apply_inner(ray_t* fv, const q_op_t* row, ray_t** args, int64_t n) {
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
    if (fv == q_registry_compose_value()) return compose_apply(args, n);
    /* ref/apply.md Composition, the glyph form: `(0|+)` is the projection
     * `0|` ON `+`, not max of a function value.  Only single-glyph rows, and
     * not the ones that legitimately CONSUME a function (`@` `.` apply, `,`
     * `!` build structure from it, `~` `?` compare/search it). */
    if (row && !row->adverb_hof && n == 2 && row->name[1] == '\0' &&
        !strchr("@.,!~?", row->name[0]) && q_eval_apply_is_fn(args[1]) &&
        !q_eval_apply_is_fn(args[0])) {
        ray_t* h[2] = { args[0], NULL };
        ray_t* p = proj_new(fv, row, h, 2, 2);
        if (RAY_IS_ERR(p)) return p;
        /* a bare glyph operand arrives as the MONADIC sibling (name
         * resolution prefers it) but q spells a bare glyph dyadic — `(0|+)`
         * is `0|` on Add, rank 2.  The glyph and its keyword monad share one
         * value, so `(0|neg)` reads dyadic too: unpinned by any doc row */
        const q_op_t* grow = q_registry_row_of(args[1], Q_MONADIC);
        ray_t* g = args[1];
        if (grow && grow->name[1] == '\0' && rank_of(g) == 1) {
            const q_op_t* drow = NULL;
            ray_t* sib = q_registry_lookup_row(
                ray_sym_intern_runtime(grow->name, 1), Q_DYADIC, &drow);
            if (sib && is_fnval(sib)) g = sib;
        }
        ray_t* c = comp_new(p, g);
        ray_release(p);
        return c;
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
            /* on a TWO-VALENCE row `family` describes the DYAD (q_ops.c
             * FAMILY AUDIT, the `+` row note): its monadic sibling (`+:`
             * flip, `<:` iasc) reaches the kernel whole unless the recipe
             * marks it atomic itself (`-:` neg) */
            if (n == 1 && fv->type == RAY_UNARY &&
                (row->dyad.kind == QK_NONE || row->mon.atomic))
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
            /* as for `atomic`, a two-valence row's family describes the DYAD —
             * the monadic sibling (`#:` count, `_:` floor) derives no index */
            if (n == 1 && row->dyad.kind == QK_NONE && is_container(args[0]))
                return index_lift(fv, args, 1);
            if (n == 2 && is_container(args[1]) && int_atom(args[0]))
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

/* THE materialization dial (Future A default, materialization phase 1): every
 * verb/kernel application funnels through here, so forcing the result is what
 * makes phase 1 FULL materialization — no lazy handle ever becomes a q value.
 * Deleting the q_eval_apply_concrete call is exactly the phase-2 / Future B (partial
 * fusion) flip; the boundary seams then catch lazy at each observable edge. */
ray_t* q_eval_apply(ray_t* fv, const q_op_t* row, ray_t** args, int64_t n) {
    return q_eval_apply_concrete(apply_inner(fv, row, args, n));
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
    ray_t* c = q_eval_apply_concrete(q_eval_apply(e, NULL, &msg, 1));
    ray_release(msg);
    return c;
}

/* Name-form amend, gated by the manifest name_lift flag — NEVER by
 * sym-sniffing in verb bodies (sym atoms are legal data).  The env's ref is
 * STOLEN for the call so a sole global amends IN PLACE (ref/amend.md handle
 * d: "modifies the item/s of its reference, and returns the handle");
 * correctness never depends on the steal — rc decides at store time.  A
 * mid-amend error restores the binding.  NULL = the form is not lifted. */
static ray_t* name_lift(const q_op_t* row, ray_t** args, int64_t n, int dot) {
    if (!row || !row->name_lift || !args[0] || args[0]->type != -RAY_SYM)
        return NULL;
    ray_t* s = ray_sym_str(args[0]->i64);
    int isfile = s && ray_str_len(s) > 0 && ray_str_ptr(s)[0] == ':';
    if (s) ray_release(s);
    if (isfile) return q_err(QE_NYI);       /* `:path on-disk amend: file wave */
    int64_t id = args[0]->i64;
    ray_t* cur = ray_env_get(id);                    /* borrowed global */
    if (!cur || RAY_IS_ERR(cur)) return q_err(QE_DOMAIN);   /* not a handle */
    ray_retain(cur);                                 /* the consumable ref */
    int stole = ray_env_set(id, RAY_NULL_OBJ) == RAY_OK;    /* env drops its ref */
    ray_t* y = n == 4 ? args[3] : NULL;
    ray_t* r = dot ? q_index_amend_dot(cur, args[1], args[2], y)
                   : q_index_amend_at(cur, args[1], args[2], y);
    if (!r || RAY_IS_ERR(r)) {
        if (stole) ray_env_set(id, cur);             /* restore the binding */
        ray_release(cur);
        return r ? r : q_err(QE_TYPE);
    }
    ray_err_t e = ray_env_set(id, r);                /* retains */
    ray_release(r);
    if (e != RAY_OK) return q_err(QE_ASSIGN);
    ray_retain(args[0]);
    return args[0];                                  /* the handle comes back */
}

/* value-form amend: PURE — the retain forces the store to path-copy, so the
 * caller's d is atomically intact even on error */
static ray_t* amend_value(ray_t** args, int64_t n, int dot) {
    ray_t* l = name_lift(q_ops_find(dot ? "." : "@", 1), args, n, dot);
    if (l) return l;
    ray_t* y = n == 4 ? args[3] : NULL;
    ray_retain(args[0]);
    ray_t* r = dot ? q_index_amend_dot(args[0], args[1], args[2], y)
                   : q_index_amend_at(args[0], args[1], args[2], y);
    if (!r || RAY_IS_ERR(r)) ray_release(args[0]);   /* amend kept our ref */
    return r ? r : q_err(QE_TYPE);
}

/* `@` — the overload matrix (ref/apply.md + ref/amend.md): 2 args Apply At /
 * Index At; 3 args on a callable head Trap At; 3-4 args on a data head
 * Amend At (machinery: ops/q_index.c; a sym-atom d name-lifts). */
ray_t* q_eval_at_wrap(ray_t** args, int64_t n) {
    if (n == 2) return q_eval_apply_concrete(q_eval_apply_value(args[0], &args[1], 1));
    if (n == 3 && q_eval_apply_is_fn(args[0])) {
        ray_t* r = q_eval_apply_concrete(q_eval_apply_value(args[0], &args[1], 1));
        return trap_catch(r, args[2]);
    }
    if (n == 3 || n == 4) return amend_value(args, n, 0);
    return q_err(QE_RANK);
}

/* `.` — 2 args: a callable spread-applies over the rhs list, a noun
 * depth-indexes (m . 1 2 is m[1;2]); 3 args callable Trap; 3-4 args data
 * head Amend (i is the path list). */
ray_t* q_eval_dot_wrap(ray_t** args, int64_t n) {
    if (n == 3 && q_eval_apply_is_fn(args[0])) {
        ray_t* r = q_eval_dot_wrap(args, 2);
        return trap_catch(r, args[2]);
    }
    if (n == 3 || n == 4) return amend_value(args, n, 1);
    if (n != 2) return q_err(QE_RANK);
    ray_t* a = args[1];
    if (!a || (!ray_is_vec(a) && a->type != RAY_LIST))
        return q_err(QE_TYPE);
    int64_t k = ray_len(a);
    if (k < 1 || k > 8) return q_err(QE_RANK);
    ray_t* av[8];
    for (int64_t i = 0; i < k; i++) {
        av[i] = q_index_elem_at(a, i);            /* owned */
        if (!av[i] || RAY_IS_ERR(av[i])) {
            ray_t* err = av[i];
            for (int64_t j = 0; j < i; j++) ray_release(av[j]);
            return err ? err : q_err(QE_TYPE);
        }
    }
    ray_t* r = q_eval_apply_concrete(q_eval_apply_value(args[0], av, k));
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
    v = q_eval_apply_concrete(v);                     /* boundary seam: truthiness */
    if (RAY_IS_ERR(v)) { *err = v; return 0; }
    int8_t t = v->type < 0 ? (int8_t)-v->type : v->type;
    if (t == RAY_F64 || t == RAY_F32) {
        ray_release(v);
        *err = q_err(QE_TYPE);
        return 0;
    }
    /* strings decide by EMPTINESS — the provisional string model's divergence
     * (kdb 'types on char vectors); the "b"$ cast is per-char and can't. */
    {
        const char* p; int64_t n;
        if (q_str_text_bytes(v, &p, &n)) {
            ray_release(v);
            return n > 0;
        }
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
