/* q_eval_apply — the apply module, THE one dispatch home (see q_eval.h).
 * Owns valence/rank, projections, composition, RAY_QFN carriers, the L1-L4
 * family lifts (manifest family column), kernel invocation and result
 * construction; the native adverb/accumulator engine it dispatches into lives
 * in q_adverb.c (shared seam: q_eval_internal.h).  The atomic vector maps are
 * transcribed from the carve-eval quarry (base eval.c atomic_map_*): kept
 * empty-input probe, typed-output probe, int-width promotion,
 * boxed fallback, error-trim.  Kernel-library extraction of the base
 * atomic_map internals is pending; this q-side copy is the recorded choice.
 * Refcount contract: args borrowed, results owned; born rc=1, append
 * retains, release local. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/eval/q_eval.h"
#include "qlang/eval/q_dbg.h"  /* frame list + the error seams (basics/debug.md) */
#include "qlang/q_prim.h"
#include "qlang/q_ctx.h"     /* q_ctx_lang_tree — the char-atom language arm */
#include "qlang/eval/q_eval_internal.h"
#include "qlang/eval/q_view.h"     /* view carriers apply as their value */
#include "qlang/eval/q_funsql.h"  /* the `?`/`!` matrix entry points (fnv_matrix_value) */
#include "qlang/base/q_err.h"
#include <ctype.h>            /* isalpha — the char-atom language arm */
#include "qlang/q_ops.h"
#include "qlang/q_builtins.h"  /* q_count_long — q `count` for C callers, hot lane */
#include "qlang/q_registry.h"
#include "qlang/base/q_type.h"     /* q_type_is_keyed — the type axis home */
#include "qlang/parse/q_parse_internal.h"
#include "qlang/io/q_handles.h"
#include "qlang/io/q_splay.h"  /* colrefs force at the apply/gather seam */
#include "qlang/ops/q_bang.h"
#include "qlang/ops/q_dollar.h"
#include "qlang/ops/q_index.h"
#include "qlang/q_env.h"
#include "lang/eval.h"
#include "lang/internal.h"   /* call_lambda — bare engine-lambda application */
#include "ops/ops.h"
#include "table/dict.h"
#include "table/sym.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

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

/* lambda carrier: [params symvec, body list, src string, defining `\d` context]
 * — the context is captured HERE, at parse time, because a lambda's unqualified
 * globals belong to the namespace it was written in, not the one it is called
 * from (ref/value.md: the globals list of `.test.f` reads `` `test`d`e ``).
 * NULL slot = root. */
ray_t* q_eval_apply_lambda_new(ray_t* params, ray_t** body, int64_t nbody,
                               ray_t* src) {
    ray_t* b = ray_list_new(nbody > 0 ? nbody : 1);
    for (int64_t i = 0; i < nbody; i++)
        b = ray_list_append(b, body[i]);
    if (!b || RAY_IS_ERR(b)) return b ? b : q_err(QE_OOM);
    ray_t* c = car_new(Q_EVAL_CAR_LAMBDA, 4);
    if (RAY_IS_ERR(c)) { ray_release(b); return c; }
    ray_t** s = car_slots(c);
    if (params) ray_retain(params);
    s[0] = params;
    s[1] = b;
    if (src) ray_retain(src);
    s[2] = src;
    int64_t ctx = q_env_ctx();
    return ctx ? car_put(c, 3, ray_sym(ctx)) : c;
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

/* iterator carrier: [adv atom] — the operand-less iterator value (103h) */
ray_t* q_eval_apply_iter_new(int adv) {
    ray_t* c = car_new(Q_EVAL_CAR_ITER, 1);
    if (RAY_IS_ERR(c)) return c;
    return car_put(c, 0, ray_i64(adv));
}

int q_eval_apply_iter_id(ray_t* v) {
    return q_eval_apply_carrier_kind(v) == Q_EVAL_CAR_ITER
               ? (int)car_slots(v)[0]->i64 : -1;
}

/* projection carrier: [fv, fv-row box, slot0..slotR-1]; holes = C NULL */
ray_t* q_eval_apply_proj_new(ray_t* fv, const q_op_t* row, ray_t** args,
                             int64_t n, int64_t rank) {
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

/* view carrier: [body tree, deps symvec, text charv, cached|NULL]; the
 * pending/in-recalc flags live in aux[1], owned by q_view.c */
ray_t* q_eval_apply_view_new(ray_t* body, ray_t* deps, ray_t* text) {
    ray_t* c = car_new(Q_EVAL_CAR_VIEW, 4);
    if (RAY_IS_ERR(c)) return c;
    c->aux[1] = 0;
    ray_retain(body); car_slots(c)[0] = body;
    ray_retain(deps); car_slots(c)[1] = deps;
    ray_retain(text); car_slots(c)[2] = text;
    return c;
}

ray_t** q_eval_apply_view_slots(ray_t* v) {
    return q_eval_apply_carrier_kind(v) == Q_EVAL_CAR_VIEW
               ? car_slots(v) : NULL;
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

int q_eval_apply_is_fnval(ray_t* v) {
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

/* the FNV overload matrices (`?` `!` `@` `.`): QR_FNV-recipe VARY values
 * (FNV = fn-at-vary-arity, not the hash) whose classic reading is the DYAD —
 * they project below rank 2 and fold/scan as dyads */
int q_eval_apply_fnv_matrix_row(const struct q_op* r) {
    return r && r->dyad.kind == QK_FN && r->dyad.arity == 0;
}

/* value-side twin, by entry-point identity — O(1) where a registry row_of
 * scan on every monadic VARY apply (enlist!) would tax the hot path */
static int fnv_matrix_value(const ray_t* fv) {
    uintptr_t f = (uintptr_t)fv->i64;
    return f == (uintptr_t)q_funsql_ques_wrap ||
           f == (uintptr_t)q_funsql_bang_wrap ||
           f == (uintptr_t)q_eval_at_wrap ||
           f == (uintptr_t)q_eval_dot_wrap;
}

ray_t* q_eval_apply_collapse(ray_t* l) {          /* consumes l, returns owned */
    ray_t* c = q_list_collapse(l);
    ray_release(l);
    return c;
}

/* A lazily-bound splay column forces through the @[column;idx] gather seam
 * the moment anything consumes it — a phrase name nobody uses never touches
 * its file, a zip column inflates only idx's blocks.  Consumes r (the
 * q_eval_apply_concrete contract). */
static ray_t* colref_force(ray_t* r) {
    ray_t* car; int64_t name; ray_t* idx;
    q_splay_colref_parts(r, &car, &name, &idx);
    ray_t* g = q_splay_gather(car, name, RAY_IS_NULL(idx) ? NULL : idx);
    ray_release(r);
    return g ? g : q_err(QE_TYPE);
}

/* THE force home (materialization phase 1): the ONLY site that turns a lazy
 * DAG handle into a concrete value (ray_lazy_materialize CONSUMES its input and
 * is a no-op on a concrete value).  Errors/NULL pass through. */
ray_t* q_eval_apply_concrete(ray_t* r) {
    if (r && ray_is_lazy(r)) return ray_lazy_materialize(r);
    if (r && q_splay_colref_is(r)) return colref_force(r);
    return r;
}

#ifdef DEBUG
void q_eval_apply_assert_concrete(ray_t* v) {
    assert(!ray_is_lazy(v) && !q_splay_colref_is(v));
}
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

static int probe_ok(ray_t* a) { return a && !RAY_IS_ERR(a) && ray_is_atom(a); }

static void probe_drop(ray_t* a) {
    if (a && RAY_IS_ERR(a)) ray_error_free(a);
    else if (a) ray_release(a);
}

/* empty-input output-type probe: the stand-in carries the argument's OWN type,
 * so the kernel takes its normal path for that type instead of answering for a
 * fabricated long.  It must be a ZERO, not a typed null: `neg` shortcuts a null
 * to itself ("a null has no sign", ref/neg.md) and would skip the b->i
 * promotion its domain/range table sets.  q_dollar_cast is the one conversion
 * home; the types it cannot reach from a long (sym, guid) have no zero but do
 * have a canonical empty VALUE, which is what ray_typed_null returns for them.
 * A general list has no type to read and falls back to the long. */
static ray_t* probe_atom(ray_t* coll) {
    ray_t* z = ray_i64(0);
    if (!coll || !ray_is_vec(coll) || coll->type == RAY_STR) return z;
    ray_t* a = q_dollar_cast(coll->type, z);
    if (!probe_ok(a)) { probe_drop(a); a = ray_typed_null((int8_t)-coll->type); }
    if (probe_ok(a)) { ray_release(z); return a; }
    probe_drop(a);
    return z;
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
        ray_t* z = probe_atom(arg);
        ray_t* probe = (z && !RAY_IS_ERR(z)) ? fn(z) : NULL;
        if (z) ray_release(z);
        /* A kernel narrower than its published domain rejects the argument's
         * own type while still admitting a long (sqrt/exp/log take p m d n u v
         * t per their domain/range tables; ours take only the numerics).  Ask
         * the long before giving up, so the empty keeps the documented type. */
        if (arg->type != RAY_LIST && (!probe || RAY_IS_ERR(probe))) {
            probe_drop(probe);
            ray_t* zl = ray_i64(0);
            probe = probe_ok(zl) ? fn(zl) : NULL;
            probe_drop(zl);
        }
        if (probe && !RAY_IS_ERR(probe) && probe->type < 0) {
            int8_t t = (int8_t)(-probe->type);
            ray_release(probe);
            return ray_vec_new(t, 0);
        }
        /* A probe answering a LIST per element (`string `a` is ,"a") makes the
         * empty result the empty GENERAL list — no typed vector can hold lists,
         * so `type string `symbol$()` is 0h, not 7h (ref/string.md). */
        if (probe && !RAY_IS_ERR(probe)) {
            ray_release(probe);
            return ray_list_new(0);
        }
        /* The probe FAILED, so it named no type — and an untyped () carries
         * none either, which leaves () the only honest answer (the sibling of
         * q_index.c:miss_null).  A TYPED empty keeps the long: whether a
         * rejecting kernel should propagate its error is a separate ruling. */
        if (probe) ray_error_free(probe);
        if (arg->type == RAY_LIST) return ray_list_new(0);
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
    return q_eval_apply_collapse(out);
}

static ray_t* binary_elem(ray_binary_fn fn, ray_t* a, ray_t* b) {
    if (RAY_IS_ERR(a)) return a;
    if (RAY_IS_ERR(b)) return b;
    if (is_coll(a) || is_coll(b) || is_container(a) || is_container(b))
        return atomic2(fn, a, b);
    return fn(a, b);
}

/* eval.c's atomic_map_binary_op owns a DAG-executor fast path keyed on the
 * opcode; this carve kept only its boxed loop, so `v+v` paid three heap atoms
 * per element.  The gate below is a STRICT SUBSET of eval.c's can_dag, so the
 * executor always takes it and base's boxed fallback — a second
 * result-construction home — stays unreachable from here. */
static uint16_t dag_op_of(ray_binary_fn f) {
    uintptr_t p = (uintptr_t)f;
    if (p == (uintptr_t)ray_add_fn) return OP_ADD;
    if (p == (uintptr_t)ray_sub_fn) return OP_SUB;
    if (p == (uintptr_t)ray_mul_fn) return OP_MUL;
    if (p == (uintptr_t)ray_div_fn) return OP_DIV;
    return 0;
}

/* F64 vectors only.  eval.c's gate also admits the int and temporal widths,
 * but the executor computes in the machine width where the element kernels
 * carry q's sentinel rules — int infinity (`0W+til 3`) and byte wraparound
 * (`1-0x02` is 0xff) both diverge.  Float has no sentinel but NaN, which the
 * HAS_NULLS bail already covers.  Scalars may be any plain numeric: they
 * reach the executor as a promoted constant, exactly as the kernel reads them. */
#define DAG_ATOMT(t) ((t)==RAY_I64||(t)==RAY_F64||(t)==RAY_I32||(t)==RAY_I16)

static int dag_pair_ok(ray_t* l, ray_t* r, int lc, int rc) {
    int lv = lc && ray_is_vec(l), rv = rc && ray_is_vec(r);
    if ((lc && !lv) || (rc && !rv)) return 0;
    if (!lv && !rv) return 0;
    if (lv && (l->type != RAY_F64 || (l->attrs & RAY_ATTR_HAS_NULLS))) return 0;
    if (rv && (r->type != RAY_F64 || (r->attrs & RAY_ATTR_HAS_NULLS))) return 0;
    if (!lc && (!DAG_ATOMT((int8_t)-l->type) || RAY_ATOM_IS_NULL(l))) return 0;
    if (!rc && (!DAG_ATOMT((int8_t)-r->type) || RAY_ATOM_IS_NULL(r))) return 0;
    return 1;
}
#undef DAG_ATOMT

static ray_t* map_binary(ray_binary_fn fn, ray_t* l, ray_t* r) {
    int lc = is_coll(l), rc = is_coll(r);
    if (!lc && !rc) return fn(l, r);
    uint16_t dop = dag_op_of(fn);
    if (dop && dag_pair_ok(l, r, lc, rc) &&
        !(lc && rc && ray_len(l) != ray_len(r)))
        return atomic_map_binary_op(fn, dop, l, r);
    int64_t len;
    if (lc && rc) {
        if (ray_len(l) != ray_len(r)) return q_err(QE_LENGTH);
        len = ray_len(l);
    } else {
        len = lc ? ray_len(l) : ray_len(r);
    }

    if (len == 0) {
        ray_t* la = lc ? probe_atom(l) : l;
        ray_t* ra = rc ? probe_atom(r) : r;
        ray_t* probe = (la && ra) ? fn(la, ra) : NULL;
        if (lc && la) ray_release(la);
        if (rc && ra) ray_release(ra);
        /* same narrow-kernel retry as map_unary: `reciprocal` is 1%x, so the
         * temporal domains ref/reciprocal.md publishes are reached through here */
        if (((lc && l->type != RAY_LIST) || (rc && r->type != RAY_LIST)) &&
            (!probe || RAY_IS_ERR(probe))) {
            probe_drop(probe);
            ray_t* lz = lc ? ray_i64(0) : l;
            ray_t* rz = rc ? ray_i64(0) : r;
            probe = (lz && rz) ? fn(lz, rz) : NULL;
            if (lc) probe_drop(lz);
            if (rc) probe_drop(rz);
        }
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
    return q_eval_apply_collapse(out);
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
    return ray_dict_new(uk, q_eval_apply_collapse(out));
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

/* a manifest row's registry value at one valence (borrowed) + its row */
ray_t* q_eval_apply_manifest_value(const q_op_t* r, q_valence_t v,
                                   const q_op_t** out) {
    ray_t* val = q_registry_row_value(r, v);
    if (out) *out = val ? r : NULL;
    return val;
}

/* `flip x` through the manifest value — the transpose the rank-2 laws lean on */
static ray_t* flip_of(ray_t* x) {
    const q_op_t* frow = NULL;
    ray_t* fl = q_eval_apply_manifest_value(q_ops_find("flip", 4), Q_MONADIC, &frow);
    if (!fl) return q_err(QE_TYPE);
    return q_eval_apply(fl, frow, &x, 1);       /* ragged input 'length here */
}

/* `f each flip x` — literally: the 4.1t traverse-columns law (ref/dev.md,
 * ref/var.md), spelled with the same two primitives the docs spell it with; an
 * optional held LEFT makes it `l f' flip x` for the dyadic m-window verbs */
static ray_t* each_flip(ray_t* fv, const q_op_t* row, ray_t* l, ray_t* x) {
    ray_t* f = flip_of(x);
    if (!f || RAY_IS_ERR(f)) return f ? f : q_err(QE_TYPE);
    ray_t* a[2] = { l, f };
    ray_t* r = l ? q_adverb_apply(0 /* `'` each */, fv, row, a, 2)
                 : q_adverb_apply(0 /* `'` each */, fv, row, &f, 1);
    ray_release(f);
    return r;
}

/* THE rank-2 arm.  The fold group reduces the OUTER axis with its atomic dyad,
 * so null propagation is the DYAD's (basics/math.md "Aggregating nulls");
 * QNEST_MEAN scales that fold by the outer count (ref/avg.md). */
static ray_t* agg_nested(ray_t* fv, const q_op_t* row, ray_t* x) {
    if (row->nested == QNEST_COLUMNS) return each_flip(fv, row, NULL, x);
    const q_op_t* drow = NULL;
    ray_t* dv = q_eval_apply_manifest_value(q_ops_nested_dyad(row), Q_DYADIC, &drow);
    if (!dv) return q_err(QE_TYPE);
    int64_t n = ray_len(x);
    /* an additive fold over an all-boolean domain narrows b -> i (ref/sum.md
     * domain table): `sum (01b;10b)` is 1 1i, `sum enlist 01011b` int.  Row
     * identity, the flip_of/QNEST_MEAN lookup precedent below. */
    int allbool = drow && drow == q_ops_find("+", 1);
    for (int64_t i = 0; allbool && i < n; i++) {
        ray_t* e = q_index_elem_at(x, i);
        allbool = e && (e->type == RAY_BOOL || e->type == -RAY_BOOL);
        if (e) ray_release(e);
    }
    ray_t* r = q_index_elem_at(x, 0);
    for (int64_t i = 1; i < n && r && !RAY_IS_ERR(r); i++) {
        ray_t* e = q_index_elem_at(x, i);
        ray_t* a[2] = { r, e };
        ray_t* t = q_eval_apply(dv, drow, a, 2);
        ray_release(r);
        ray_release(e);
        r = t;
    }
    if (allbool && row->nested == QNEST_FOLD) r = q_agg_bool_narrow(r);
    if (row->nested != QNEST_MEAN || !r || RAY_IS_ERR(r))
        return r ? r : q_err(QE_TYPE);
    const q_op_t* prow = NULL;
    ray_t* pv = q_eval_apply_manifest_value(q_ops_find("%", 1), Q_DYADIC, &prow);
    ray_t* cnt = ray_i64(n);
    ray_t* a[2] = { r, cnt };
    ray_t* m = pv ? q_eval_apply(pv, prow, a, 2) : q_err(QE_TYPE);
    ray_release(r);
    ray_release(cnt);
    return m;
}

/* a dyadic aggregate lifts over a dict (by its VALUES) or a nested list, and
 * never over a table — basics/math.md's "Exceptions to the above" lists
 * `wavg (tables)` / `wsum (tables)` while ref/{sum,avg}.md say each "applies
 * to dictionaries"; a keyed table is a dict wearing a table's law. */
static int agg2_lifts(ray_t* v) {
    return (v->type == RAY_DICT && !q_type_is_keyed(v)) ||
           q_index_any_nested_item(v);
}

/* THE dyadic rank-2 arm, spelled by ref/avg.md's own annotation on
 * `(1 2;3 4) wavg (500 400;300 200)`: "this is (1 3 wavg 500 300; 2 4 wavg
 * 400 200)" — the two args' COLUMNS zipped, a flat arg riding whole into
 * every column (`1 2 wavg d`, same page). */
static ray_t* agg2(ray_t* fv, const q_op_t* row, ray_t* x, ray_t* y) {
    ray_t* vx = agg2_lifts(x) && x->type == RAY_DICT ? ray_dict_vals(x) : x;
    ray_t* vy = agg2_lifts(y) && y->type == RAY_DICT ? ray_dict_vals(y) : y;
    ray_t* fx = q_index_any_nested_item(vx) ? flip_of(vx) : NULL;
    if (fx && RAY_IS_ERR(fx)) return fx;
    ray_t* fy = q_index_any_nested_item(vy) ? flip_of(vy) : NULL;
    if (fy && RAY_IS_ERR(fy)) { ray_release(fx); return fy; }
    if (!fx && !fy) { ray_t* a[2] = { vx, vy }; return q_eval_apply(fv, row, a, 2); }
    if (fx && fy && ray_len(fx) != ray_len(fy)) {
        ray_release(fx); ray_release(fy);
        return q_err(QE_LENGTH);
    }
    int64_t n = ray_len(fx ? fx : fy);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    for (int64_t i = 0; i < n; i++) {
        ray_t* xi = fx ? q_index_elem_at(fx, i) : vx;
        ray_t* yi = fy ? q_index_elem_at(fy, i) : vy;
        ray_t* a[2] = { xi, yi };
        ray_t* r = q_eval_apply(fv, row, a, 2);
        if (fx) ray_release(xi);
        if (fy) ray_release(yi);
        if (RAY_IS_ERR(r)) {
            ray_release(out); ray_release(fx); ray_release(fy);
            return r;
        }
        out = ray_list_append(out, r);
        ray_release(r);
    }
    ray_release(fx);
    ray_release(fy);
    return q_eval_apply_collapse(out);
}

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
        return ray_dict_new(ks, q_eval_apply_collapse(vs));
    }
    if (row && row->nested && q_index_any_nested_item(x)) return agg_nested(fv, row, x);
    return q_err(QE_TYPE);
}

/* ===== L2 map lift ======================================================= */

static ray_t* map1(ray_t* fv, const q_op_t* row, ray_t* l, ray_t* x) {
    if (x && x->type == RAY_DICT) {
        ray_t* v = ray_dict_vals(x);
        ray_t* a[2] = { l, v };                /* l, when held, rides along */
        ray_t* nv;
        if (q_type_is_keyed(x)) nv = map1(fv, row, l, v);
        else                     nv = q_eval_apply_concrete(
                                     q_eval_apply(fv, row, l ? a : &v, l ? 2 : 1));
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
            ray_t* a[2] = { l, col };
            ray_t* r = q_eval_apply_concrete(
                q_eval_apply(fv, row, l ? a : &col, l ? 2 : 1));
            if (RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_table_add_col(out, ray_table_col_name(x, c), r);
            ray_release(r);
        }
        return out;
    }
    /* THE map-family rank-2 law: `flip f each flip x` — scan the OUTER axis
     * (ref/sum.md sums list-of-lists, ref/max.md maxs over a dict); single and
     * uniform across the roster, so no manifest column carries it */
    if (q_index_any_nested_item(x)) {
        ray_t* r = each_flip(fv, row, l, x);
        if (!r || RAY_IS_ERR(r)) return r ? r : q_err(QE_TYPE);
        ray_t* out = flip_of(r);
        ray_release(r);
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

/* ===== lambda application: frames over the engine scope stack ============ */

static _Thread_local int g_frame_depth;
static _Thread_local int g_frame_floor;

/* Effective depth = frames above the FLOOR.  A script load raises the floor to
 * the live depth (q_ctx.c ctx_run_script) so its statements bind globally, while
 * push/pop stay on the REAL depth — a lambda the script calls still gets locals.
 * Zeroing g_frame_depth instead would push that call into the caller's slot. */
int q_eval_apply_frame_depth(void) {
    int d = g_frame_depth - g_frame_floor;
    return d > 0 ? d : 0;
}

int q_eval_apply_frame_floor(int floor) {
    int prev = g_frame_floor;
    g_frame_floor = floor < 0 ? g_frame_depth : floor;
    return prev;
}

ray_t* q_eval_apply_lambda_src(ray_t* v) {
    if (q_eval_apply_carrier_kind(v) != Q_EVAL_CAR_LAMBDA) return NULL;
    ray_t* src = car_slots(v)[2];
    return (src && src->type == -RAY_STR) ? src : NULL;
}

/* `value` on a lambda reads the same slots (ref/value.md `## Lambda`); the
 * layout stays opaque, the scope analysis over body belongs to q_eval. */
int q_eval_apply_lambda_parts(ray_t* v, ray_t** params, ray_t** body,
                              ray_t** ctx) {
    if (q_eval_apply_carrier_kind(v) != Q_EVAL_CAR_LAMBDA) return 0;
    ray_t** c = car_slots(v);
    if (params) *params = c[0];
    if (body)   *body   = c[1];
    if (ctx)    *ctx    = c[3];
    return 1;
}

static ray_t* lambda_call(ray_t* lam, ray_t** args, int64_t n) {
    ray_t** c = car_slots(lam);
    ray_t* params = c[0];
    ray_t* body = c[1];
    /* barrier frame: strictly-local resolution (function-notation.md) —
     * the body sees its params/locals and globals, never a caller frame */
    if (q_env_frame_push(1) != RAY_OK) return q_err(QE_STACK);
    for (int64_t i = 0; i < n; i++) {
        ray_t* p = q_index_elem_at(params, i);          /* sym atom */
        if (p && !RAY_IS_ERR(p)) {
            q_env_local_set(p->i64, args[i]);              /* retains */
            ray_release(p);
        }
    }
    g_frame_depth++;
    q_dbg_frame_push(lam);
    /* the body runs in the namespace the lambda was DEFINED in, so a callee
     * resolves its own globals, not its caller's (see q_eval_apply_lambda_new) */
    int64_t caller_ctx = q_env_ctx(), lam_ctx = c[3] ? c[3]->i64 : 0;
    q_env_ctx_set(lam_ctx);
    ray_t* r = RAY_NULL_OBJ;
    int64_t nb = ray_len(body);
    ray_t** bs = (ray_t**)ray_data(body);
    for (int64_t i = 0; i < nb; i++) {
        ray_t* nr = q_eval(bs[i]);
        /* the lambda-boundary error seam: locals still live; a `:r` resume
         * substitutes the statement's result and the body continues */
        if (RAY_IS_ERR(nr)) nr = q_dbg_lambda_filter(nr);
        if (RAY_IS_ERR(nr)) { r = nr; break; }
        ray_release(r);
        r = nr;
    }
    /* explicit return `:x`: the QE_RETURN sentinel unwinds to HERE, the
     * lambda boundary, and becomes the ordinary successful result */
    if (RAY_IS_ERR(r) && q_err_is(r, QE_RETURN)) {
        ray_error_free(r);
        r = q_err_take();
        if (!r) { ray_retain(RAY_NULL_OBJ); r = RAY_NULL_OBJ; }
    }
    /* an explicit `\d` in the body is a SESSION directive and outlives the call
     * — only the implicit definition-context is unwound (namespace/switch.qcmd) */
    if (q_env_ctx() == lam_ctx) q_env_ctx_set(caller_ctx);
    q_dbg_frame_pop();
    g_frame_depth--;
    q_env_frame_pop();
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
    if (holes > 0) return q_eval_apply_proj_new(fv, row, merged, rank, rank);
    return q_eval_apply(fv, row, merged, rank);
}

/* ===== noun-head application ============================================
 * APPLICATION concerns only (handles, sym heads, elided lists, the string
 * boundary) — all DATA indexing is ops/q_index.c, the one index/amend home;
 * results cross into q-space through q_str_charv_out. */

/* Symbol-handle application: a `` `:… `` communication handle delegates
 * wholly to q_handles_sym_apply (the sole handle authority); otherwise
 * q4m3 §12 symbol-as-global indexing. */
static ray_t* sym_head_apply(ray_t* head, ray_t** args, int64_t n) {
    ray_t* s = ray_sym_str(head->i64);               /* borrowed */
    if (s && ray_str_len(s) > 0 && ray_str_ptr(s)[0] == ':')
        return q_handles_sym_apply(head, args, n);
    ray_t* v = q_env_resolve(head->i64);             /* owned or NULL */
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
    /* char-atom application IS the language dispatch: `"q" "til 3"` -> value,
     * `"g" "4"` -> `.g.e "4"` (owner-ruled 2026-08-07; one tree with the
     * statement seam, q_ctx_lang_tree) */
    if (v->type == -RAY_CHARV && n == 1 && args[0] &&
        isalpha((unsigned char)v->u8)) {
        const char* tp; int64_t tn;
        if (!q_str_text_bytes(args[0], &tp, &tn)) return q_err(QE_TYPE);
        ray_t* t = q_ctx_lang_tree((char)v->u8, tp, tn);
        ray_t* r = q_eval(t);
        ray_release(t);
        return r;
    }
    if (!(v->type == RAY_DICT || v->type == RAY_TABLE ||
          ray_is_vec(v) || v->type == RAY_LIST))
        return q_err(QE_TYPE);
    /* a lazy splay column thunk (io/q_splay.h colref) applies by BINDING the
     * index into its idx slot; the gather seam forces it later — the same
     * carrier-to-authority dispatch the handle arm above rides */
    if (q_splay_colref_is(v) && n == 1 && args[0]) {
        ray_t* car; int64_t nm; ray_t* idx;
        q_splay_colref_parts(v, &car, &nm, &idx);
        if (RAY_IS_NULL(idx)) return q_splay_colref(car, nm, args[0]);
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
    if (kind == Q_EVAL_CAR_ITER) return 1;  /* exactly one operand */
    return -1;                              /* vary / deriv: no fixed rank */
}

int64_t q_eval_apply_rank(ray_t* fv) { return fv ? rank_of(fv) : -1; }

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

/* An iterator value applied to its one operand IS the derived function —
 * `(/)[+]`, `+/` and the literal `(/;+)` are all this one call. */
static ray_t* iter_call(ray_t* it, ray_t** args, int64_t n) {
    if (n != 1) return q_err(QE_RANK);
    if (!args[0]) return q_eval_apply_proj_new(it, NULL, args, n, 1);
    const q_op_t* frow = q_eval_apply_is_fnval(args[0])
                             ? q_registry_operand_row(args[0]) : NULL;
    return q_eval_apply_deriv_new(q_eval_apply_iter_id(it), args[0], frow);
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
                                      q_eval_apply_is_fnval(args[0])
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
        return q_adverb_apply((int)c[2]->i64, c[0], row_unbox(c[1]),
                                   args, n);
    }
    if (kind == Q_EVAL_CAR_COMP) return comp_call(fv, args, n);
    if (kind == Q_EVAL_CAR_ITER) return iter_call(fv, args, n);
    if (!kind && !q_eval_apply_is_fnval(fv)) {
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
        return q_eval_apply_proj_new(fv, row, args, n, rank);
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
        ray_t* p = q_eval_apply_proj_new(fv, row, h, 2, 2);
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
            if (sib && q_eval_apply_is_fnval(sib)) g = sib;
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
    if (rank >= 0 && n < rank) return q_eval_apply_proj_new(fv, row, args, n, rank);
    if (rank >= 0 && n > rank) return q_err(QE_RANK);
    /* a manifest FNV overload matrix applied below its minimum rank PROJECTS
     * like any operator (kdb `?[x]` is `?[x;]`) */
    if (fv->type == RAY_VARY && n < 2 && fnv_matrix_value(fv)) {
        const q_op_t* vrow = row ? row : q_registry_row_of(fv, Q_DYADIC);
        return q_eval_apply_proj_new(fv, vrow, args, n, 2);
    }

    if (kind == Q_EVAL_CAR_LAMBDA) return lambda_call(fv, args, n);

    /* keyword-HOF rows route to the native adverb arms (finding 3) */
    if (row && row->adverb_hof && row->lex == QLEX_KW_INFIX && n == 2) {
        int adv = q_adverb_hof_id(row->adverb_hof);
        if (adv >= 0) {
            const q_op_t* frow = NULL;
            if (q_eval_apply_is_fnval(args[0]))
                frow = q_registry_operand_row(args[0]);
            return q_adverb_apply(adv, args[0], frow, args + 1, 1);
        }
    }

    /* on a TWO-VALENCE row `family` describes the DYAD (q_ops.c FAMILY AUDIT,
     * the `+` row note), so a monad that is atomic in its own right (`-:` neg,
     * `~:` not under Match's aggregate row) says so on ITS recipe (QR_FN1A)
     * and lifts whatever family the dyad claims. */
    if (row && n == 1 && row->mon.atomic && fv->type == RAY_UNARY)
        return atomic1((ray_unary_fn)(uintptr_t)fv->i64, args[0]);

    const char* fam = row ? row->family : NULL;
    if (fam) {
        if (strcmp(fam, "atomic") == 0) {
            /* the monadic sibling of an atomic dyad (`+:` flip, `<:` iasc)
             * reaches the kernel whole — only a one-valence row lifts here */
            if (n == 1 && fv->type == RAY_UNARY && row->dyad.kind == QK_NONE)
                return atomic1((ray_unary_fn)(uintptr_t)fv->i64, args[0]);
            if (n == 2 && fv->type == RAY_BINARY)
                return atomic2((ray_binary_fn)(uintptr_t)fv->i64,
                               args[0], args[1]);
        } else if (strcmp(fam, "aggregate") == 0) {
            if (n == 1 && (is_container(args[0]) ||
                           (row->nested && q_index_any_nested_item(args[0]))))
                return agg1(fv, row, args[0]);
            if (n == 2 && row->nested &&
                (agg2_lifts(args[0]) || agg2_lifts(args[1])))
                return agg2(fv, row, args[0], args[1]);
        } else if (strcmp(fam, "map") == 0) {
            if (n == 1 && (is_container(args[0]) ||
                           q_index_any_nested_item(args[0])))
                return map1(fv, row, NULL, args[0]);
            /* the dyadic m-window rows are window-then-data: lift on the DATA
             * with the window/decay ATOM held (`2 mmax d`, ref/max.md) */
            if (n == 2 && args[0]->type < 0 &&
                (is_container(args[1]) || q_index_any_nested_item(args[1])))
                return map1(fv, row, args[0], args[1]);
        } else if (strcmp(fam, "index") == 0) {
            /* as for `atomic`, a two-valence row's family describes the DYAD —
             * the monadic sibling (`#:` count, `_:` floor) derives no index */
            if (n == 1 && row->dyad.kind == QK_NONE && is_container(args[0]))
                return index_lift(fv, args, 1);
            if (n == 2 && is_container(args[1]) && q_type_is_int_atom(args[0]))
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
#define Q_APPLY_MAX_ARGS 60   /* q_eval.c's EVAL_MAX_ARGS — the one arg cap */

/* idxproj — `first`/`last` AS INDEX CONVERSION, the one home: an end head
 * over a whole-column colref or a carrier becomes a ONE-ROW gather (element
 * 0 / n-1; result rank follows index rank, so a carrier answers the row
 * dict).  Head identities are registry VALUES cached at init — #359: an
 * identity you hold is never re-spelled through lookup_name on the hot path
 * (`*`-monadic aliases `first`'s value, so it matches for free).  NULL = no
 * conversion; a returned value (or error) IS the application's answer. */
static ray_t* g_end_heads[2];              /* [0] `first` -> 0, [1] `last` -> n-1 */

void q_eval_apply_init(void) {
    g_end_heads[0] = q_registry_lookup_name("first", 5, Q_MONADIC);
    g_end_heads[1] = q_registry_lookup_name("last", 4, Q_MONADIC);
}

static ray_t* idxproj(ray_t* head, ray_t* arg) {
    int lastp;
    if (head == g_end_heads[0] && head) lastp = 0;
    else if (head == g_end_heads[1] && head) lastp = 1;
    else return NULL;
    ray_t* car = NULL; int64_t name = 0; ray_t* cidx = NULL;
    if (q_splay_colref_is(arg)) {
        q_splay_colref_parts(arg, &car, &name, &cidx);
        if (!RAY_IS_NULL(cidx)) return NULL;          /* partial: generic path */
    } else if (q_splay_is(arg)) {
        car = arg;
    } else {
        return NULL;
    }
    int64_t i = 0;
    if (lastp) {
        ray_t* cnt = q_splay_count(car);
        if (!cnt || RAY_IS_ERR(cnt)) return cnt ? cnt : q_err(QE_TYPE);
        i = cnt->i64 - 1;
        ray_release(cnt);
        if (i < 0) return NULL;                       /* empty: the whole path */
    }
    ray_t* ia = ray_i64(i);
    if (!ia || RAY_IS_ERR(ia)) return ia ? ia : q_err(QE_OOM);
    ray_t* g = (car == arg) ? q_splay_rows(car, ia)
                            : q_splay_gather(car, name, ia);
    ray_release(ia);
    return g ? g : q_err(QE_TYPE);
}

static ray_t* apply_dispatch(ray_t* fv, const q_op_t* row, ray_t** args,
                             int64_t n) {
    if (n == 1 && args[0]) {
        ray_t* one = idxproj(fv, args[0]);
        if (one) return one;
    }
    /* a splay colref arg forces HERE, the one dispatch entry — kernels,
     * adverbs, lambdas and projections all see the gathered column */
    int64_t i = 0;
    while (i < n && !q_splay_colref_is(args[i])) i++;
    if (i < n) {
        if (n > Q_APPLY_MAX_ARGS) return q_err(QE_RANK);
        ray_t* fa[Q_APPLY_MAX_ARGS];
        for (int64_t j = 0; j < n; j++) {
            if (q_splay_colref_is(args[j])) {
                ray_retain(args[j]);                  /* the frame's ref survives */
                fa[j] = q_eval_apply_concrete(args[j]);
                if (!fa[j] || RAY_IS_ERR(fa[j])) {
                    for (int64_t k = 0; k < j; k++)
                        if (fa[k] != args[k]) ray_release(fa[k]);
                    return fa[j] ? fa[j] : q_err(QE_TYPE);
                }
            } else
                fa[j] = args[j];
        }
        ray_t* r = q_eval_apply_concrete(apply_inner(fv, row, fa, n));
        for (int64_t j = 0; j < n; j++)
            if (fa[j] != args[j]) ray_release(fa[j]);
        return r;
    }
    return q_eval_apply_concrete(apply_inner(fv, row, args, n));
}

/* the ONE dispatch exit: the deepest apply to produce an error snapshots the
 * live frames here (and, under `\e 1`, suspends into the debugger) */
ray_t* q_eval_apply(ray_t* fv, const q_op_t* row, ray_t** args, int64_t n) {
    ray_t* r = apply_dispatch(fv, row, args, n);
    if (r && RAY_IS_ERR(r)) return q_dbg_filter(r, fv, args, n);
    return r;
}

/* ===== the public value-apply seam ======================================= */

int q_eval_apply_is_fn(ray_t* v) {
    int kind = q_eval_apply_carrier_kind(v);
    return q_eval_apply_is_fnval(v) || v->type == RAY_LAMBDA ||
           (kind != 0 && kind != Q_EVAL_CAR_VIEW);
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
            if (sib && q_eval_apply_is_fnval(sib)) return sib;
        }
    }
    *row = q_registry_row_of(head, n == 1 ? Q_MONADIC : Q_DYADIC);
    return head;
}

ray_t* q_eval_apply_value(ray_t* head, ray_t** args, int64_t n) {
    if (!head || RAY_IS_ERR(head)) return q_err(QE_TYPE);
    if (q_splay_colref_is(head)) {            /* `ask[i]` in a phrase */
        ray_t* car; int64_t name; ray_t* cidx;
        q_splay_colref_parts(head, &car, &name, &cidx);
        if (n == 1 && RAY_IS_NULL(cidx) &&
            (q_type_is_int_atom(args[0]) || q_type_is_int_vec(args[0]))) {
            /* int index straight into the gather — only the covering blocks;
             * the index home already answers atom = element, vector = gather */
            ray_t* g = q_splay_gather(car, name, args[0]);
            return g ? g : q_err(QE_TYPE);
        }
        ray_retain(head);
        ray_t* col = q_eval_apply_concrete(head);
        if (!col || RAY_IS_ERR(col)) return col ? col : q_err(QE_TYPE);
        ray_t* r = q_eval_apply_value(col, args, n);
        ray_release(col);
        return r;
    }
    if (q_view_is(head)) {                    /* applying a view applies its value */
        ray_t* dv = q_view_deref_borrowed(head);
        if (RAY_IS_ERR(dv)) return dv;
        ray_t* r = q_eval_apply_value(dv, args, n);
        ray_release(dv);
        return r;
    }
    if (q_eval_apply_is_fn(head)) {
        const q_op_t* row = NULL;
        if (q_eval_apply_is_fnval(head)) head = apply_valence_sibling(head, n, &row);
        return q_eval_apply(head, row, args, n);
    }
    return noun_index(head, args, n);
}

/* Trap (ref/apply.md): on error, a callable/null catch applies to the error
 * text, a noun catch is returned as the value.  Consumes r; e borrowed.
 * QE_RETURN passes through UNTOUCHED — an explicit return is a successful
 * return, never trappable (@[{:42};0;{x}] is 42), and its payload must
 * survive to the lambda boundary that unwraps it. */
static ray_t* trap_catch(ray_t* r, ray_t* e) {
    if (!r || !RAY_IS_ERR(r)) return r;
    if (q_err_is(r, QE_RETURN)) return r;
    q_dbg_snapshot_clear();             /* a caught error's trace is spent */
    if (!q_eval_apply_is_fn(e) && !RAY_IS_NULL(e)) {
        q_err_drop();                   /* swallow the payload with the error */
        ray_error_free(r);
        ray_retain(e);
        return e;
    }
    ray_t* msg = q_err_take();          /* signal/name text, already a charv */
    if (!msg) {
        int64_t tn = 0;
        const char* tp = q_err_text(r, &tn);
        msg = ray_charv(tp ? tp : "", tn);
    }
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
    ray_t* cur = q_env_get(id);                      /* borrowed flat global */
    int stole = 0;
    if (cur && !RAY_IS_ERR(cur)) {
        ray_retain(cur);                             /* the consumable ref */
        stole = q_env_take(id, cur);                 /* env drops its ref */
    } else if (q_env_ns_exists(id)) {
        cur = q_env_ns_view(id);       /* owned namespace dict: amend, rebind */
        if (!cur) return q_err(QE_DOMAIN);
    } else {
        return q_err(QE_DOMAIN);                     /* not a handle */
    }
    ray_t* y = n == 4 ? args[3] : NULL;
    ray_t* r = dot ? q_index_amend_dot(cur, args[1], args[2], y)
                   : q_index_amend_at(cur, args[1], args[2], y);
    if (!r || RAY_IS_ERR(r)) {
        if (stole) q_env_bind(id, cur);              /* restore the binding */
        ray_release(cur);
        return r ? r : q_err(QE_TYPE);
    }
    ray_err_t e = q_env_set(id, r);                  /* retains */
    ray_release(r);
    if (e != RAY_OK) return q_env_err(e);
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
        q_dbg_trap_enter();             /* error-trap mode 0 inside the trap */
        ray_t* r = q_eval_apply_concrete(q_eval_apply_value(args[0], &args[1], 1));
        q_dbg_trap_exit();
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
        q_dbg_trap_enter();
        ray_t* r = q_eval_dot_wrap(args, 2);
        q_dbg_trap_exit();
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

/* ===== carrier read-out (display renders in q_fmt; slots stay opaque) ==== */

int q_eval_apply_deriv_adv(ray_t* v) {
    return q_eval_apply_carrier_kind(v) == Q_EVAL_CAR_DERIV
               ? (int)car_slots(v)[2]->i64 : -1;
}

const char* q_eval_apply_car_head_name(ray_t* v) {
    int kind = q_eval_apply_carrier_kind(v);
    if (kind != Q_EVAL_CAR_DERIV && kind != Q_EVAL_CAR_PROJ) return NULL;
    const q_op_t* row = row_unbox(car_slots(v)[1]);
    return row ? row->name : NULL;
}

ray_t* q_eval_apply_car_head(ray_t* v) {
    int kind = q_eval_apply_carrier_kind(v);
    if (kind == Q_EVAL_CAR_DERIV || kind == Q_EVAL_CAR_PROJ ||
        kind == Q_EVAL_CAR_COMP)
        return car_slots(v)[0];
    return NULL;
}

ray_t* q_eval_apply_comp_inner(ray_t* v) {
    return q_eval_apply_carrier_kind(v) == Q_EVAL_CAR_COMP
               ? car_slots(v)[1] : NULL;
}

int64_t q_eval_apply_proj_nslots(ray_t* v) {
    return q_eval_apply_carrier_kind(v) == Q_EVAL_CAR_PROJ ? ray_len(v) - 2
                                                           : 0;
}

ray_t* q_eval_apply_proj_arg(ray_t* v, int64_t i) {
    return q_eval_apply_carrier_kind(v) == Q_EVAL_CAR_PROJ
               ? car_slots(v)[2 + i] : NULL;
}
