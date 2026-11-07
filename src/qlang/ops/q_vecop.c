/* q_vecop.c — the typed elementwise loops behind q_vecop.h.
 *
 * Three lanes carry every pair this file admits: BYTE (the 1-byte tags, whose
 * only null is the char blank, and no tolerance), INT (int64, nulls as NULL_I64)
 * and DBL (double, nulls as NaN).  A same-type temporal rides INT on its own
 * PAYLOAD — a common unit would buy nothing and overflow i64 past year 2292.
 * Each operand is brought to its lane's width once — an already-wide vector in
 * place, a narrow one through a widening pass, an atom read once — and then one
 * loop per operand shape decides the whole vector.  Which lane a pair
 * lands on is not a preference: it reproduces the kernels' own split, so the
 * answer is the kernels' answer.
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
 *
 * + - * div mod ride INT on the integer tags (div and mod on every int-backed
 * count).  Their result tag is not the operands' — ops/arith.c promotes (`5h+3h`
 * is an int) — so the lane computes in i64 and CONSTRUCTS the promoted tag, read
 * off lang/internal.h's arith_int_type / div_type / mod_type, never a table.
 * Overflow is left alone: two's-complement wrap in unsigned space, whose
 * landing on a sentinel (`0W+1` is 0N) is incidental and stays so.
 *
 * + - * beside a float or real, and % on every plain number, ride DBL instead:
 * ops/arith.c answers a null for either null operand, else reads each through
 * as_f64 (`0W` a large finite), applies one IEEE op and answers
 * promote_float_type's tag (% always a float).  The lane reads nulls as NaN,
 * which every op propagates, narrows a real through `float` and writes every
 * NaN as the null.  A char, a temporal and a byte vector stay out.
 */
#include "qlang/q_count.h"
#include "qlang/ops/q_vecop.h"
#include "qlang/q_registry_internal.h" /* the compare/min2/max2/fill/null wrapper roster */
#include "qlang/base/q_type.h"         /* q_type_common + the numeric/float tag memberships */
#include "lang/eval.h"                 /* ray_lt_fn/ray_gt_fn/..., ray_cmp_tol_eq */
#include "lang/internal.h"             /* as_f64 / as_i64 / arith_int_type — the atom lane reads */
#include <stdlib.h>
#include <string.h>

typedef enum {
    VO_NONE = 0, VO_LT, VO_LE, VO_GT, VO_GE, VO_EQ, VO_NE, VO_MIN, VO_MAX, VO_FILL,
    VO_ADD, VO_SUB, VO_MUL, VO_IDIV, VO_MOD, VO_DIV
} vop_t;
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
    if (p == (uintptr_t)ray_add_fn) return VO_ADD;
    if (p == (uintptr_t)ray_sub_fn) return VO_SUB;
    if (p == (uintptr_t)ray_mul_fn) return VO_MUL;
    if (p == (uintptr_t)ray_idiv_fn) return VO_IDIV;
    if (p == (uintptr_t)ray_mod_fn) return VO_MOD;
    if (p == (uintptr_t)ray_div_fn) return VO_DIV;
    return VO_NONE;
}

static int is_arith(vop_t op) { return op >= VO_ADD; }

/* the lane memberships ops/cmp.c turns on and the type axis does not name: the
 * 1-byte payloads, and int_cmp_lane's own set (which excludes bool) */
static int byte_tag(int8_t t) { return t == RAY_BOOL || ray_is_bytelike(t); }
static int int_tag(int8_t t) {
    return t == RAY_I64 || t == RAY_I32 || t == RAY_I16 || ray_is_bytelike(t) || RAY_IS_TEMPORAL(t);
}
/* Every tag the lanes read; the datetime stays out, its tolerance band being
 * one q_type_is_float_tag does not name. */
static int lane_tag(int8_t t) { return q_type_is_num_tag(t) || t == RAY_CHARV || RAY_IS_TEMPORAL(t); }
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
        RAY_BYTE_CASES: {
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
        RAY_BYTE_CASES: {
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

static double atom_f(ray_t* v) { return RAY_ATOM_IS_NULL(v) ? NULL_F64 : as_f64(v); }

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
            case LN_INT: o->scalar.j = atom_is_oob_null(v) ? NULL_I64 : as_i64(v); break;
            default: o->scalar.d = atom_f(v); break;
        }
        o->p = &o->scalar;
        return 1;
    }
    o->step = 1;
    const void* src = ray_data(v);
    if (ln == LN_BYTE || (ln == LN_INT && (tag == RAY_I64 || RAY_IS_TEMPORAL64(tag))) || (ln == LN_DBL && tag == RAY_F64)) {
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

/* One loop per operand shape (vector-vector, vector-atom, atom-vector), so every
 * loop reads at a compile-time stride and vectorizes; BODY sees x, y and i. */
#define SHAPED(T, PX, PY, XV, YV, N, BODY)                                         \
    do {                                                                           \
        const T *px_ = (const T*)(PX), *py_ = (const T*)(PY);                      \
        if ((XV) && (YV)) {                                                        \
            for (int64_t i = 0; i < (N); i++) {                                    \
                T x = px_[i], y = py_[i];                                          \
                (void)x;                   /* ^ on the int lane reads y alone */   \
                BODY;                                                              \
            }                                                                      \
        } else if (XV) {                                                           \
            const T y = *py_;                                                      \
            for (int64_t i = 0; i < (N); i++) { T x = px_[i]; (void)x; BODY; }     \
        } else {                                                                   \
            const T x = *px_;                                                      \
            (void)x;                                                               \
            for (int64_t i = 0; i < (N); i++) { T y = py_[i]; BODY; }              \
        }                                                                          \
    } while (0)

/* The loops write the sentinels themselves, and a fresh vector has no index to
 * drop, so the attr is all ray_vec_set_null would add — once, not per null. */
static void mark_nulls(ray_t* out, int any) {
    if (any) out->attrs |= RAY_ATTR_HAS_NULLS;
}

/* The double lane's store at OFF: V is the answer, Z whether it is the null, which
 * lands as the result tag's own sentinel; a real result narrows through float. */
#define DBL_W(T, NUL, OFF, PX, PY, XV, YV, N, V, Z)                                \
    do {                                                                           \
        T* d = (T*)ray_data(out) + (OFF);                                          \
        SHAPED(double, PX, PY, XV, YV, N, {                                        \
            double v = (V);                                                        \
            int z = (Z);                                                           \
            d[i] = z ? (NUL) : (T)v;                                               \
            any |= z;                                                              \
        });                                                                        \
    } while (0)
#define DBL_STORE(OFF, PX, PY, XV, YV, N, V, Z)                                    \
    do {                                                                           \
        if (rt == RAY_F32) DBL_W(float, NULL_F32, OFF, PX, PY, XV, YV, N, V, Z);   \
        else DBL_W(double, NULL_F64, OFF, PX, PY, XV, YV, N, V, Z);                \
    } while (0)

/* ===== the decision pass =================================================
 * One byte per element: a comparison's own answer, or for & | ^ the mask
 * "take the LEFT operand here".  Branch-free, so the null cases select rather
 * than jump. */

/* NaN's roster (both, x-only, y-only null) as plain boolean algebra: GCC
 * if-converts a double compare's nested selects no further than one level. */
#define DEC_D(BOTH, ONLYX, ONLYY, EXPR)                                            \
    SHAPED(double, xb.p, yb.p, xb.step, yb.step, n, {                              \
        int nx = x != x;                                                           \
        int ny = y != y;                                                           \
        int e = (EXPR);                                                            \
        m[i] = (uint8_t)((nx & ny & (BOTH)) | (nx & (!ny) & (ONLYX)) |             \
                         ((!nx) & ny & (ONLYY)) | ((!(nx | ny)) & e));             \
    })

#define NUL_J(v) ((v) == NULL_I64)
#define NUL_B(v) ((int)(v) == bnul)
/* The exact lanes order by value alone: NULL_I64 is the least int64, so a null
 * already sits under each value and equals itself, and the byte lane's blank is
 * an in-band 32 (basics/comparison.md).  Only ^ asks for the null. */
#define DEC_V(T, E) SHAPED(T, xb.p, yb.p, xb.step, yb.step, n, m[i] = (uint8_t)(E))
#define DEC_EXACT(T, ISNUL)                                                        \
    switch (op) {                                                                  \
        case VO_LT: case VO_MIN: DEC_V(T, x < y); break;                           \
        case VO_LE: DEC_V(T, x <= y); break;                                       \
        case VO_GT: case VO_MAX: DEC_V(T, x > y); break;                           \
        case VO_GE: DEC_V(T, x >= y); break;                                       \
        case VO_EQ: DEC_V(T, x == y); break;                                       \
        case VO_NE: DEC_V(T, x != y); break;                                       \
        default: DEC_V(T, ISNUL(y)); break;                                        \
    }

/* `tol` is ops/cmp.c's tol_pair for this pair, which only the orderings consult:
 * = and <> tolerate on the double lane whatever the pair.  `bnul` is the null ^
 * fills: 0x20 for a char (kdb-true `null " "`), and a value no byte can hold for
 * the bool/byte tags, which have none. */
static void decide(uint8_t* m, vop_t op, lane_t ln, band_t xb, band_t yb, int tol, int bnul, int64_t n) {
    if (ln == LN_DBL) {
        /* `tol` is hoisted out of the loops: a run-time flag in the body stops GCC vectorizing it */
        switch (op) {
            case VO_LT:
                if (tol) DEC_D(0, 1, 0, (x < y) & !ray_cmp_tol_eq(x, y));
                else DEC_D(0, 1, 0, x < y);
                break;
            case VO_LE:
                if (tol) DEC_D(1, 1, 0, (x <= y) | ray_cmp_tol_eq(x, y));
                else DEC_D(1, 1, 0, x <= y);
                break;
            case VO_GT:
                if (tol) DEC_D(0, 0, 1, (x > y) & !ray_cmp_tol_eq(x, y));
                else DEC_D(0, 0, 1, x > y);
                break;
            case VO_GE:
                if (tol) DEC_D(1, 0, 1, (x >= y) | ray_cmp_tol_eq(x, y));
                else DEC_D(1, 0, 1, x >= y);
                break;
            case VO_EQ: DEC_D(1, 0, 0, ray_cmp_tol_eq(x, y)); break;
            case VO_NE: DEC_D(0, 1, 1, !ray_cmp_tol_eq(x, y)); break;
            default: break;                /* & | ^ decide as they pick: pick_dbl */
        }
    } else if (ln == LN_INT) {
        DEC_EXACT(int64_t, NUL_J);
    } else {
        DEC_EXACT(uint8_t, NUL_B);
    }
}

/* The picked operand's value, carried to the result tag.  Both operands are
 * already the lane's width and a lane null is the lane's own sentinel, so the
 * narrowing here is the whole of what the cast home would have done. */
static void pick_into(ray_t* out, int8_t rt, lane_t ln, band_t xb, band_t yb, const uint8_t* m, int64_t n) {
    void* o = ray_data(out);
    if (ln == LN_BYTE) {
        uint8_t* d = o;
        SHAPED(uint8_t, xb.p, yb.p, xb.step, yb.step, n, d[i] = m[i] ? x : y);
        return;
    }
    int w = ray_type_sizes[rt];          /* a temporal narrows by its payload width, not its tag */
    int any = 0;
#define PICK_J(T, NUL)                                                             \
    do {                                                                           \
        T* d = o;                                                                  \
        SHAPED(int64_t, xb.p, yb.p, xb.step, yb.step, n, {                         \
            int64_t v = m[i] ? x : y;                                              \
            int z = v == NULL_I64;                                                 \
            d[i] = z ? (NUL) : (T)v;                                               \
            any |= z;                                                              \
        });                                                                        \
    } while (0)
    if (w == 2) PICK_J(int16_t, NULL_I16);
    else if (w == 4) PICK_J(int32_t, NULL_I32);
    else PICK_J(int64_t, NULL_I64);
#undef PICK_J
    mark_nulls(out, any);
}

/* & | ^ on the double lane, decided and picked in one pass, each a plain select
 * GCC can vectorize.  Nulls are the least value: & is null where either side is,
 * | reads a null y as -0w and is null only where both are, ^ takes x exactly
 * where y is null.  Ties take y, as the decision pass's roster does. */
static void pick_dbl(ray_t* out, int8_t rt, vop_t op, band_t xb, band_t yb, int64_t n) {
    int any = 0;
#define NUL_LEAST(v) ((v) != (v) ? -INFINITY : (v))
#define PICK_D(V, Z) DBL_STORE(0, xb.p, yb.p, xb.step, yb.step, n, V, Z)
    switch (op) {
        case VO_MIN: PICK_D(x < y ? x : y, (x != x) | (y != y)); break;
        case VO_MAX: PICK_D(x > NUL_LEAST(y) ? x : NUL_LEAST(y), (x != x) & (y != y)); break;
        default: PICK_D(y != y ? x : y, (x != x) & (y != y)); break;
    }
#undef PICK_D
#undef NUL_LEAST
    mark_nulls(out, any);
}

/* ===== the arithmetic lane ===============================================
 * The tags + - * read on INT: plain integers only.  A char is out (its 0x20 null
 * is in-band, and the byte lane's own reading of it is not arithmetic's), a
 * temporal is out (it has a payload law of its own); a float goes to DBL.
 * ray_is_byte_only keeps a byte ATOM, which widens like any other operand.
 * div and mod read every int-backed count, a char's code point included. */
static int arith_tag(int8_t t) {
    return t == RAY_BOOL || t == RAY_I16 || t == RAY_I32 || t == RAY_I64 || ray_is_byte_only(t);
}
static int counts_tag(vop_t op, int8_t t) {
    return op == VO_IDIV || op == VO_MOD ? int_tag(t) || t == RAY_BOOL : arith_tag(t);
}

/* The tag map_binary would have built for this pair: the kernel's own atom
 * answer, raised to the widest int operand by its "integer width follows the
 * wider vector operand" rule (bool is not one of those widths).  Where the two
 * disagree the lane DECLINES, so the per-element road decides it.  div and mod
 * answer their published grid as it stands: the kernel's tag, never widened.
 * 0 = not this lane's. */
static int8_t arith_rt(vop_t op, int8_t xt, int8_t yt) {
    if (op == VO_IDIV || op == VO_MOD) {
        int8_t rt = op == VO_IDIV ? div_type(xt, yt) : mod_type(xt, yt);
        return rt == RAY_I16 || rt == RAY_I32 || rt == RAY_I64 || RAY_IS_TEMPORAL(rt) ? rt : 0;
    }
    int8_t w = 0;
    if (xt != RAY_BOOL && xt > w) w = xt;
    if (yt != RAY_BOOL && yt > w) w = yt;
    int8_t rt = arith_int_type(xt, yt);
    if (rt != RAY_I16 && rt != RAY_I32 && rt != RAY_I64) return 0;
    return rt >= w ? rt : 0;
}

/* One loop per (verb, result width).  The width is loop-invariant, so hoisting
 * it out is what leaves a plain typed loop behind.  A lane null narrows to the
 * result's own sentinel; a WRAPPED value that lands on that sentinel is a null
 * too, which is how `0Wi+1i` is 0Ni — so the test is on the narrowed value. */
#define ARI_W(T, NUL, EXPR)                                                        \
    do {                                                                           \
        T* d = o;                                                                  \
        SHAPED(int64_t, xb.p, yb.p, xb.step, yb.step, n, {                         \
            int64_t v = (x == NULL_I64 || y == NULL_I64) ? NULL_I64 : (EXPR);      \
            T r = v == NULL_I64 ? (NUL) : (T)v;                                    \
            d[i] = r;                                                              \
            any |= r == (NUL);                                                     \
        });                                                                        \
    } while (0)

#define ARI_OP(EXPR)                                                               \
    do {                                                                           \
        if (w == 8) ARI_W(int64_t, NULL_I64, EXPR);                                \
        else if (w == 4) ARI_W(int32_t, NULL_I32, EXPR);                           \
        else ARI_W(int16_t, NULL_I16, EXPR);                                       \
    } while (0)

static void arith_into(ray_t* out, int8_t rt, vop_t op, band_t xb, band_t yb, int64_t n) {
    void* o = ray_data(out);
    int w = ray_type_sizes[rt];
    int any = 0;
    switch (op) {
        case VO_ADD: ARI_OP((int64_t)((uint64_t)x + (uint64_t)y)); break;
        case VO_SUB: ARI_OP((int64_t)((uint64_t)x - (uint64_t)y)); break;
        case VO_MUL: ARI_OP((int64_t)((uint64_t)x * (uint64_t)y)); break;
        case VO_IDIV: ARI_OP(floor_div64(x, y)); break;
        default: ARI_OP(floor_mod64(x, y)); break;
    }
    mark_nulls(out, any);
}

/* The double lane's tag: % answers make_f64 for every pair, + - * need a float side.  0 = not this lane's. */
static int8_t float_rt(vop_t op, int8_t xt, int8_t yt) {
    if (op == VO_IDIV || op == VO_MOD || !q_type_is_num_tag(xt) || !q_type_is_num_tag(yt)) return 0;
    if (op == VO_DIV) return RAY_F64;
    if (!q_type_is_float_tag(xt) && !q_type_is_float_tag(yt)) return 0;
    return (int8_t)-promote_float_type((int8_t)-xt, (int8_t)-yt);
}

/* Widened in cache-sized runs, not whole: a real or int operand of a big vector would otherwise cost a heap buffer
 * the size of the answer, which outweighs the arithmetic. */
enum { FLT_RUN = 1024 };

static const double* flt_run(double* buf, ray_t* v, int8_t t, int vec, const double* atom, int64_t i, int64_t m) {
    if (!vec) return atom;
    if (t == RAY_F64) return (const double*)ray_data(v) + i;
    widen_f(buf, (const char*)ray_data(v) + i * ray_type_sizes[t], t, m);
    return buf;
}

#define FLT_OP(EXPR) DBL_STORE(i0, px, py, xv, yv, m, EXPR, v != v)

static ray_t* float_binary(vop_t op, int8_t rt, ray_t* x, ray_t* y, int8_t xt, int8_t yt, int xv, int yv, int64_t n) {
    ray_t* out = ray_vec_new(rt, n);
    if (!out || RAY_IS_ERR(out)) return out;
    out->len = n;
    double xa = xv ? 0 : atom_f(x), ya = yv ? 0 : atom_f(y), xbuf[FLT_RUN], ybuf[FLT_RUN];
    int any = 0;
    for (int64_t i0 = 0; i0 < n; i0 += FLT_RUN) {
        int64_t m = n - i0 < FLT_RUN ? n - i0 : FLT_RUN;
        const double* px = flt_run(xbuf, x, xt, xv, &xa, i0, m);
        const double* py = flt_run(ybuf, y, yt, yv, &ya, i0, m);
        switch (op) {
            case VO_ADD: FLT_OP(x + y); break;
            case VO_SUB: FLT_OP(x - y == 0.0 ? 0.0 : x - y); break;   /* ray_sub_fn's -0.0 normalization */
            case VO_MUL: FLT_OP(x * y); break;
            default: FLT_OP(x / y); break;
        }
    }
    mark_nulls(out, any);
    return out;
}

/* A byte VECTOR operand of + - * % stays on the element road: + - * answer 'type
 * there today (`(0x01 0x02)+0x03`), the element store having no byte arm, and
 * declining leaves that error where it is.  div and mod never answer a byte. */
static ray_t* arith_binary(vop_t op, ray_t* x, ray_t* y, int8_t xt, int8_t yt, int xv, int yv, int64_t n) {
    if (op != VO_IDIV && op != VO_MOD && ((xv && ray_is_byte_only(xt)) || (yv && ray_is_byte_only(yt)))) return NULL;
    int8_t rt = float_rt(op, xt, yt);
    if (rt) return float_binary(op, rt, x, y, xt, yt, xv, yv, n);
    if (op == VO_DIV || !counts_tag(op, xt) || !counts_tag(op, yt)) return NULL;
    rt = arith_rt(op, xt, yt);
    if (!rt) return NULL;

    band_t xb, yb;
    if (!band_make(&xb, x, xt, xv, LN_INT, n)) return NULL;
    if (!band_make(&yb, y, yt, yv, LN_INT, n)) { free(xb.own); return NULL; }
    ray_t* out = ray_vec_new(rt, n);
    if (out && !RAY_IS_ERR(out)) {
        out->len = n;
        arith_into(out, rt, op, xb, yb, n);
    }
    free(xb.own);
    free(yb.own);
    return out;
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
    if (xv && yv && x->len != y->len) return NULL;     /* 'length stays the caller's to raise */
    int64_t n = xv ? x->len : y->len;
    if (n <= 0) return NULL;                           /* so does the empty's carried type */
    if (is_arith(op)) return arith_binary(op, x, y, xt, yt, xv, yv, n);
    if (!lane_tag(xt) || !lane_tag(yt)) return NULL;
    /* Across a type difference the lanes take the plain numerics only.  A
     * cross-type temporal pair keeps the kernel's own reading (the pairs
     * q_type_cmp_as narrows arrive here already one type); a char beside a
     * byte or bool puts ONE in-band null (the blank) on a lane whose other side
     * has none. */
    if (xt != yt && !(q_type_is_num_tag(xt) && q_type_is_num_tag(yt))) return NULL;

    int8_t rt;
    lane_t ln;
    if (answers_bool(op)) {
        rt = RAY_BOOL;
        if (byte_tag(xt) && byte_tag(yt)) ln = LN_BYTE;
        else if (op == VO_EQ || op == VO_NE) ln = (q_type_is_float_tag(xt) || q_type_is_float_tag(yt)) ? LN_DBL : LN_INT;
        else ln = (int_tag(xt) && int_tag(yt)) ? LN_INT : LN_DBL;
    } else {
        rt = q_type_common(xt, yt);
        if (!lane_tag(rt)) return NULL;
        ln = byte_tag(rt) ? LN_BYTE : q_type_is_float_tag(rt) ? LN_DBL : LN_INT;
    }

    band_t xb, yb;
    if (!band_make(&xb, x, xt, xv, ln, n)) return NULL;
    if (!band_make(&yb, y, yt, yv, ln, n)) { free(xb.own); return NULL; }

    ray_t* out = ray_vec_new(rt, n);
    uint8_t* m = NULL;
    if (out && !RAY_IS_ERR(out) && ln == LN_DBL && !answers_bool(op)) {
        out->len = n;
        pick_dbl(out, rt, op, xb, yb, n);
    } else if (out && !RAY_IS_ERR(out)) {
        out->len = n;
        /* a comparison's decision IS its result, so it lands in the output
         * itself; a pick needs the mask alongside the values it selects */
        m = answers_bool(op) ? ray_data(out) : malloc((size_t)n);
        if (!m) { ray_release(out); out = NULL; }
    }
    if (m) {
        int tol = (op == VO_MIN || op == VO_MAX) ? 0 : (q_type_is_float_tag(xt) || q_type_is_float_tag(yt));
        decide(m, op, ln, xb, yb, tol, xt == RAY_CHARV ? 0x20 : 0x100, n);
        if (!answers_bool(op)) {
            pick_into(out, rt, ln, xb, yb, m, n);
            free(m);
        }
    }
    free(xb.own);
    free(yb.own);
    return out;
}

int q_vecop_is_compare(ray_t* (*f)(ray_t*, ray_t*)) { return answers_bool(op_of(f)); }

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
