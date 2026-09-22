/* q_vecop.c — the typed elementwise loops behind q_vecop.h.
 *
 * Three lanes carry every pair this file admits: BYTE (two 1-byte tags, which
 * have no null and no tolerance), INT (int64, nulls as NULL_I64) and DBL
 * (double, nulls as NaN).  Each operand is brought to its lane's width once —
 * an already-wide vector in place, a narrow one through a widening pass, an
 * atom as a zero-stride broadcast — and then ONE loop decides the whole
 * vector.  Which lane a pair lands on is not a preference: it reproduces the
 * kernels' own split, so the answer is the kernels' answer.
 *   ordering (< <= > >=)  ops/cmp.c int_cmp_lane: INT only when both operands
 *                         are int-lane tags, else the double tail with the
 *                         tolerance band; bool is NOT an int-lane tag there.
 *   equality (= <>)       ops/cmp.c ray_eq_fn's is_float_op test: the double
 *                         tail (tolerant) when either side is a float, else
 *                         the integer read.
 *   min/max/fill (& | ^)  the result tag is q_type_common, the published
 *                         mixed-pair law, and the lane follows it.  A bool
 *                         operand can therefore land on a lane the scalar
 *                         kernel would not have picked; 0 and 1 order the same
 *                         way on all three, which is why that is safe.
 * ops/cmp.c's cmp_pick compares with tolerance OFF, so & and | never tolerate.
 */
#include "qlang/q_count.h"
#include "qlang/ops/q_vecop.h"
#include "qlang/q_registry_internal.h" /* the compare/min2/max2/fill/null wrapper roster */
#include "qlang/base/q_type.h"         /* q_type_common + the numeric/float tag memberships */
#include "lang/eval.h"                 /* ray_lt_fn/ray_gt_fn/..., ray_cmp_tol_eq */
#include "lang/internal.h"             /* as_f64 / as_i64 — the atom lane reads */
#include <stdlib.h>
#include <string.h>

typedef enum { VO_NONE = 0, VO_LT, VO_LE, VO_GT, VO_GE, VO_EQ, VO_NE, VO_MIN, VO_MAX, VO_FILL } vop_t;
typedef enum { LN_BYTE = 1, LN_INT, LN_DBL } lane_t;

static vop_t op_of(ray_t* (*f)(ray_t*, ray_t*)) {
    uintptr_t p = (uintptr_t)f;
    if (p == (uintptr_t)ray_lt_fn) return VO_LT;
    if (p == (uintptr_t)ray_lte_fn) return VO_LE;
    if (p == (uintptr_t)ray_gt_fn) return VO_GT;
    if (p == (uintptr_t)ray_gte_fn) return VO_GE;
    if (p == (uintptr_t)q_eq_wrap) return VO_EQ;
    if (p == (uintptr_t)q_ne_wrap) return VO_NE;
    if (p == (uintptr_t)q_min2_wrap) return VO_MIN;
    if (p == (uintptr_t)q_max2_wrap) return VO_MAX;
    if (p == (uintptr_t)q_fill_wrap) return VO_FILL;
    return VO_NONE;
}

/* the two lane memberships ops/cmp.c turns on and the type axis does not name:
 * the 1-byte payloads, and int_cmp_lane's own set (which excludes bool) */
static int byte_tag(int8_t t) { return t == RAY_BOOL || t == RAY_BYTE_ONLY; }
static int int_tag(int8_t t) { return t == RAY_I64 || t == RAY_I32 || t == RAY_I16 || t == RAY_BYTE_ONLY; }
static int answers_bool(vop_t op) { return op >= VO_LT && op <= VO_NE; }

/* ===== operand widening ==================================================
 * The vectorized form of lang/internal.h's as_f64 / as_i64 — the LANE READ the
 * scalar kernels perform on each atom, with the source's null sentinel carried
 * to the lane's own.  `$` would ANSWER the same: ref/cast.md:195-203 rules a
 * narrowing infinity finite in the wider type (`float$0Wh` is 32767f), which
 * is as_f64's reading too, and the sweep pins the two together.  It is not
 * called because a hot elementwise loop has no business routing through the
 * user-visible cast verb — layering, not disagreement. */

static void widen_f(double* o, const void* s, int8_t t, int64_t n) {
    switch (t) {
        case RAY_BOOL: {
            const uint8_t* p = s;
            for (int64_t i = 0; i < n; i++) o[i] = p[i] ? 1.0 : 0.0;
            break;
        }
        case RAY_BYTE_ONLY: {
            const uint8_t* p = s;
            for (int64_t i = 0; i < n; i++) o[i] = (double)p[i];
            break;
        }
        case RAY_I16: {
            const int16_t* p = s;
            for (int64_t i = 0; i < n; i++) o[i] = p[i] == NULL_I16 ? NULL_F64 : (double)p[i];
            break;
        }
        case RAY_I32: {
            const int32_t* p = s;
            for (int64_t i = 0; i < n; i++) o[i] = p[i] == NULL_I32 ? NULL_F64 : (double)p[i];
            break;
        }
        case RAY_I64: {
            const int64_t* p = s;
            for (int64_t i = 0; i < n; i++) o[i] = p[i] == NULL_I64 ? NULL_F64 : (double)p[i];
            break;
        }
        default: {
            const float* p = s;
            for (int64_t i = 0; i < n; i++) o[i] = (double)p[i];
            break;
        }
    }
}

static void widen_j(int64_t* o, const void* s, int8_t t, int64_t n) {
    switch (t) {
        case RAY_BOOL: {
            const uint8_t* p = s;
            for (int64_t i = 0; i < n; i++) o[i] = p[i] ? 1 : 0;
            break;
        }
        case RAY_BYTE_ONLY: {
            const uint8_t* p = s;
            for (int64_t i = 0; i < n; i++) o[i] = (int64_t)p[i];
            break;
        }
        case RAY_I16: {
            const int16_t* p = s;
            for (int64_t i = 0; i < n; i++) o[i] = p[i] == NULL_I16 ? NULL_I64 : (int64_t)p[i];
            break;
        }
        default: {
            const int32_t* p = s;
            for (int64_t i = 0; i < n; i++) o[i] = p[i] == NULL_I32 ? NULL_I64 : (int64_t)p[i];
            break;
        }
    }
}

/* One operand as a lane-width array plus a stride (0 = an atom broadcasts).
 * `own` takes the widening buffer when one was needed, for the caller to free. */
typedef struct {
    const void* p;
    int64_t step;
    void* own;
    union { double d; int64_t j; uint8_t b; } scalar;
} band_t;

static int band_make(band_t* o, ray_t* v, int8_t tag, int vec, lane_t ln, int64_t n) {
    o->own = NULL;
    if (!vec) {
        o->step = 0;
        switch (ln) {
            case LN_BYTE: o->scalar.b = tag == RAY_BOOL ? (v->b8 ? 1 : 0) : v->u8; break;
            case LN_INT: o->scalar.j = RAY_ATOM_IS_NULL(v) ? NULL_I64 : as_i64(v); break;
            default: o->scalar.d = RAY_ATOM_IS_NULL(v) ? NULL_F64 : as_f64(v); break;
        }
        o->p = &o->scalar;
        return 1;
    }
    o->step = 1;
    const void* src = ray_data(v);
    if (ln == LN_BYTE || (ln == LN_INT && tag == RAY_I64) || (ln == LN_DBL && tag == RAY_F64)) {
        o->p = src;
        return 1;
    }
    void* buf = malloc((ln == LN_INT ? sizeof(int64_t) : sizeof(double)) * (size_t)n);
    if (!buf) return 0;
    if (ln == LN_INT) widen_j(buf, src, tag, n);
    else widen_f(buf, src, tag, n);
    o->own = buf;
    o->p = buf;
    return 1;
}

/* ===== the decision pass =================================================
 * One byte per element: a comparison's own answer, or for & | ^ the mask
 * "take the LEFT operand here". */

#define DEC_D(BOTH, ONLYX, ONLYY, EXPR)                                            \
    do {                                                                           \
        const double *px = xb.p, *py = yb.p;                                       \
        for (int64_t i = 0; i < n; i++, px += xb.step, py += yb.step) {            \
            double x = *px, y = *py;                                               \
            int nx = (x != x), ny = (y != y);                                      \
            m[i] = (uint8_t)((nx && ny) ? (BOTH) : nx ? (ONLYX) : ny ? (ONLYY) : (EXPR)); \
        }                                                                          \
    } while (0)

#define DEC_J(BOTH, ONLYX, ONLYY, EXPR)                                            \
    do {                                                                           \
        const int64_t *px = xb.p, *py = yb.p;                                      \
        for (int64_t i = 0; i < n; i++, px += xb.step, py += yb.step) {            \
            int64_t x = *px, y = *py;                                              \
            int nx = (x == NULL_I64), ny = (y == NULL_I64);                        \
            m[i] = (uint8_t)((nx && ny) ? (BOTH) : nx ? (ONLYX) : ny ? (ONLYY) : (EXPR)); \
        }                                                                          \
    } while (0)

#define DEC_B(EXPR)                                                                \
    do {                                                                           \
        const uint8_t *px = xb.p, *py = yb.p;                                      \
        for (int64_t i = 0; i < n; i++, px += xb.step, py += yb.step) {            \
            unsigned x = *px, y = *py;                                             \
            m[i] = (uint8_t)(EXPR);                                                \
        }                                                                          \
    } while (0)

/* `tol` is ops/cmp.c's tol_pair for this pair, and 0 for the & | picks. */
static void decide(uint8_t* m, vop_t op, lane_t ln, band_t xb, band_t yb, int tol, int64_t n) {
    if (ln == LN_DBL) {
        switch (op) {
            case VO_LT: case VO_MIN: DEC_D(0, 1, 0, x < y && !(tol && ray_cmp_tol_eq(x, y))); break;
            case VO_LE: DEC_D(1, 1, 0, x <= y || (tol && ray_cmp_tol_eq(x, y))); break;
            case VO_GT: case VO_MAX: DEC_D(0, 0, 1, x > y && !(tol && ray_cmp_tol_eq(x, y))); break;
            case VO_GE: DEC_D(1, 0, 1, x >= y || (tol && ray_cmp_tol_eq(x, y))); break;
            case VO_EQ: DEC_D(1, 0, 0, ray_cmp_tol_eq(x, y)); break;
            case VO_NE: DEC_D(0, 1, 1, !ray_cmp_tol_eq(x, y)); break;
            default: DEC_D(1, 0, 1, 0); break;      /* ^ takes x exactly where y is null */
        }
    } else if (ln == LN_INT) {
        switch (op) {
            case VO_LT: case VO_MIN: DEC_J(0, 1, 0, x < y); break;
            case VO_LE: DEC_J(1, 1, 0, x <= y); break;
            case VO_GT: case VO_MAX: DEC_J(0, 0, 1, x > y); break;
            case VO_GE: DEC_J(1, 0, 1, x >= y); break;
            case VO_EQ: DEC_J(1, 0, 0, x == y); break;
            case VO_NE: DEC_J(0, 1, 1, x != y); break;
            default: DEC_J(1, 0, 1, 0); break;
        }
    } else {
        switch (op) {
            case VO_LT: case VO_MIN: DEC_B(x < y); break;
            case VO_LE: DEC_B(x <= y); break;
            case VO_GT: case VO_MAX: DEC_B(x > y); break;
            case VO_GE: DEC_B(x >= y); break;
            case VO_EQ: DEC_B(x == y); break;
            case VO_NE: DEC_B(x != y); break;
            default: memset(m, 0, (size_t)n); break;  /* a byte is never null: ^ always takes y */
        }
    }
}

/* The picked operand's value, carried to the result tag.  Both operands are
 * already the lane's width and a lane null is the lane's own sentinel, so the
 * narrowing here is the whole of what the cast home would have done. */
static void pick_into(ray_t* out, int8_t rt, lane_t ln, band_t xb, band_t yb, const uint8_t* m, int64_t n) {
    void* o = ray_data(out);
    if (ln == LN_BYTE) {
        const uint8_t *px = xb.p, *py = yb.p;
        uint8_t* d = o;
        for (int64_t i = 0; i < n; i++, px += xb.step, py += yb.step) d[i] = m[i] ? *px : *py;
        return;
    }
    if (ln == LN_DBL) {
        const double *px = xb.p, *py = yb.p;
        for (int64_t i = 0; i < n; i++, px += xb.step, py += yb.step) {
            double v = m[i] ? *px : *py;
            if (rt == RAY_F32) ((float*)o)[i] = (float)v;
            else ((double*)o)[i] = v;
            if (v != v) ray_vec_set_null(out, i, true);
        }
        return;
    }
    const int64_t *px = xb.p, *py = yb.p;
    for (int64_t i = 0; i < n; i++, px += xb.step, py += yb.step) {
        int64_t v = m[i] ? *px : *py;
        int isnull = (v == NULL_I64);
        if (rt == RAY_I16) ((int16_t*)o)[i] = isnull ? NULL_I16 : (int16_t)v;
        else if (rt == RAY_I32) ((int32_t*)o)[i] = isnull ? NULL_I32 : (int32_t)v;
        else ((int64_t*)o)[i] = v;
        if (isnull) ray_vec_set_null(out, i, true);
    }
}

/* ===== the binary seam =================================================== */

ray_t* q_vecop_binary(ray_t* (*f)(ray_t*, ray_t*), ray_t* x, ray_t* y) {
    vop_t op = op_of(f);
    if (op == VO_NONE || !x || !y || RAY_IS_ERR(x) || RAY_IS_ERR(y)) return NULL;

    int xv = ray_is_vec(x), yv = ray_is_vec(y);
    if (!xv && !yv) return NULL;                       /* an atom pair IS the kernel's own call */
    if ((!xv && !ray_is_atom(x)) || (!yv && !ray_is_atom(y))) return NULL;
    int8_t xt = xv ? x->type : (int8_t)-x->type;
    int8_t yt = yv ? y->type : (int8_t)-y->type;
    if (!q_type_is_num_tag(xt) || !q_type_is_num_tag(yt)) return NULL;
    if (xv && yv && x->len != y->len) return NULL;     /* 'length stays the caller's to raise */
    int64_t n = xv ? x->len : y->len;
    if (n <= 0) return NULL;                           /* so does the empty's carried type */

    int8_t rt;
    lane_t ln;
    if (answers_bool(op)) {
        rt = RAY_BOOL;
        if (byte_tag(xt) && byte_tag(yt)) ln = LN_BYTE;
        else if (op == VO_EQ || op == VO_NE) ln = (q_type_is_float_tag(xt) || q_type_is_float_tag(yt)) ? LN_DBL : LN_INT;
        else ln = (int_tag(xt) && int_tag(yt)) ? LN_INT : LN_DBL;
    } else {
        rt = q_type_common(xt, yt);
        if (!rt || !q_type_is_num_tag(rt)) return NULL;
        ln = byte_tag(rt) ? LN_BYTE : q_type_is_float_tag(rt) ? LN_DBL : LN_INT;
    }

    band_t xb, yb;
    if (!band_make(&xb, x, xt, xv, ln, n)) return NULL;
    if (!band_make(&yb, y, yt, yv, ln, n)) { free(xb.own); return NULL; }

    ray_t* out = ray_vec_new(rt, n);
    uint8_t* m = NULL;
    if (out && !RAY_IS_ERR(out)) {
        out->len = n;
        /* a comparison's decision IS its result, so it lands in the output
         * itself; a pick needs the mask alongside the values it selects */
        m = answers_bool(op) ? ray_data(out) : malloc((size_t)n);
        if (!m) { ray_release(out); out = NULL; }
    }
    if (out && !RAY_IS_ERR(out)) {
        int tol = (op == VO_MIN || op == VO_MAX) ? 0 : (q_type_is_float_tag(xt) || q_type_is_float_tag(yt));
        decide(m, op, ln, xb, yb, tol, n);
        if (!answers_bool(op)) {
            pick_into(out, rt, ln, xb, yb, m, n);
            free(m);
        }
    }
    free(xb.own);
    free(yb.own);
    return out;
}

/* ===== the monadic seam: `null x` ======================================== */

#define NULL_SCAN(T, ISNULL)                                                       \
    do {                                                                           \
        const T* p = s;                                                            \
        for (int64_t i = 0; i < n; i++) o[i] = (uint8_t)(ISNULL);                  \
    } while (0)

ray_t* q_vecop_unary(ray_t* (*f)(ray_t*), ray_t* x) {
    if ((uintptr_t)f != (uintptr_t)q_null_wrap) return NULL;
    if (!x || RAY_IS_ERR(x) || !ray_is_vec(x)) return NULL;
    int64_t n = x->len;
    if (n <= 0) return NULL;
    int8_t t = x->type;
    switch (t) {
        case RAY_BOOL: case RAY_BYTE_ONLY: case RAY_CHARV:
        case RAY_I16: case RAY_I32: case RAY_I64: case RAY_F32: case RAY_F64:
        RAY_TEMPORAL32_CASES: RAY_TEMPORAL64_CASES: RAY_TEMPORALF_CASES: break;
        default: return NULL;             /* sym, guid, str, list: their own null readings */
    }
    ray_t* out = ray_vec_new(RAY_BOOL, n);
    if (!out || RAY_IS_ERR(out)) return out;
    out->len = n;
    uint8_t* o = ray_data(out);
    const void* s = ray_data(x);
    switch (t) {
        case RAY_CHARV: NULL_SCAN(uint8_t, p[i] == 0x20); break;
        case RAY_I16: NULL_SCAN(int16_t, p[i] == NULL_I16); break;
        case RAY_I32: RAY_TEMPORAL32_CASES: NULL_SCAN(int32_t, p[i] == NULL_I32); break;
        case RAY_I64: RAY_TEMPORAL64_CASES: NULL_SCAN(int64_t, p[i] == NULL_I64); break;
        case RAY_F32: NULL_SCAN(float, p[i] != p[i]); break;
        case RAY_F64: RAY_TEMPORALF_CASES: NULL_SCAN(double, p[i] != p[i]); break;
        default: memset(o, 0, (size_t)n); break;       /* bool and byte carry no null */
    }
    return out;
}
