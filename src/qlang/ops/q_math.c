/* q_math.c — atomic unary/dyadic math (libm family, xexp/xlog), comparison
 * wrappers (= <> & not), neg/null, and the float-matrix verbs (mmu/inv/lsq)
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_count.h"
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/base/q_err.h"
#include "qlang/ops/q_dollar.h" /* q_dollar_cast — THE conversion home */
#include "lang/eval.h"     /* ray_eq_fn/ray_neq_fn, ray_neg_fn */
#include "lang/internal.h" /* atomic_map_unary, as_f64, is_numeric_or_temporal, make_f64, ray_pow_fn */
#include "qlang/base/q_type.h"  /* q_type_as_i64 / q_type_is_bool / q_type_is_char_atom */
#include "qlang/io/q_handles.h" /* q_handles_pq_neg — a `:pq:` handle's async form */
#include <math.h>          /* sin/cos/tan/asin/acos/atan, exp/log, isfinite, floor/floorf, ceil/ceilf */
#include <string.h>        /* memcpy */
#include <stdlib.h>        /* malloc, free */


/* q monadic `_` — floor to LONG (kdb `_ 3.7` is 3j; rayfall floor keeps f64).
 * Ints/bools pass through; f64 null -> long null.  RAY_FN_ATOMIC maps it
 * element-wise over float vectors. */
ray_t* q_floor_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == -RAY_F64) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)floor(x->f64));
    }
    if (x->type == -RAY_F32) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)floorf((float)x->f64));
    }
    if (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16 ||
        x->type == -RAY_BOOL) {
        ray_retain(x);
        return x;
    }
    return q_err(QE_TYPE);
}

/* ---- atomic unary math (feat/q-math-atomic) — implement-via-libm ----
 * rayfall has exp/log/sqrt but no trig/signum, so these are q-layer wrappers,
 * one libm call per atom.  All are registered RAY_FN_ATOMIC, so the evaluator
 * (atomic_map_unary) broadcasts them over vectors and nested lists; each
 * wrapper handles the ATOM case only (mirroring ray_sqrt_fn/q_floor_wrap).
 * Float results go through make_f64 (internal.h), which canonicalizes every
 * non-finite (NaN OR ±Inf) to the single float null 0n — so `sin 1%0` -> 0n
 * (kdb's 0w is unrepresentable under this model, a deferred cell).  Null in
 * -> typed float null out (kdb: sin/cos/asin/... of a null is null). */
#define Q_LIBM_UNARY(NAME, FN, GLYPH)                                          \
    ray_t* NAME(ray_t* x) {                                                    \
        if (!x) return q_err(QE_TYPE);                       \
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);              \
        if (is_numeric(x)) return make_f64(FN(as_f64(x)));                     \
        return q_err(QE_TYPE);                              \
    }
Q_LIBM_UNARY(q_sin_wrap,  sin,  "sin")
Q_LIBM_UNARY(q_cos_wrap,  cos,  "cos")
Q_LIBM_UNARY(q_tan_wrap,  tan,  "tan")
Q_LIBM_UNARY(q_asin_wrap, asin, "asin")
Q_LIBM_UNARY(q_acos_wrap, acos, "acos")
Q_LIBM_UNARY(q_atan_wrap, atan, "atan")
#undef Q_LIBM_UNARY

/* q `signum x` — sign as INT (i32): null or negative -> -1i, zero -> 0i,
 * positive -> 1i (ref/signum.md).  Kdb ALWAYS returns int, whatever the input
 * width.  A float null (0n) tests as null -> -1i (kdb treats null as negative).
 * Every numeric OR temporal lane reads through as_f64 (which owns the payload
 * per type, incl. the three temporal lanes) — the sign is exact for all of
 * them, so the type-ladder collapses to one admission + one read
 * (`signum 1999.12.31` -> -1i, a pre-epoch date is negative). */
static ray_t* signum_atom(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (RAY_ATOM_IS_NULL(x)) return ray_i32(-1);
    if (is_numeric_or_temporal(x)) {
        double v = as_f64(x);
        return ray_i32(v < 0 ? -1 : (v > 0 ? 1 : 0));
    }
    return q_err(QE_TYPE);
}

/* Broadcast + collapse carrier: registered
 * RAY_FN_NONE so THIS wrapper drives the broadcast, letting a top-level boxed
 * list of i32 atoms collapse to an int vector — kdb shows `signum (0n;0N;0Nt)`
 * as ONE `-1 -1 -1i` line, not one atom per line. */
ray_t* q_signum_wrap(ray_t* x) {
    ray_t* r = is_collection(x) ? atomic_map_unary(signum_atom, x)
                                : signum_atom(x);
    if (!r || RAY_IS_ERR(r) || r->type != RAY_LIST) return r;
    ray_t* c = q_list_collapse(r);   /* owned: retains-or-builds */
    ray_release(r);
    return c;
}

/* q `ceiling x` — least integer >= x, returned as a LONG (kdb `ceiling 2.1` is
 * 3j).  The q_floor_wrap twin: rayfall's `ceil` keeps f64, so this wrapper rounds
 * to i64 exactly like q_floor_wrap.  Ints/bools pass through; f64 null -> long
 * null. */
ray_t* q_ceiling_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == -RAY_F64) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)ceil(x->f64));
    }
    if (x->type == -RAY_F32) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)ceilf((float)x->f64));
    }
    if (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16 ||
        x->type == -RAY_BOOL) {
        ray_retain(x);
        return x;
    }
    if (q_type_is_char_atom(x)) return ray_i32(x->u8);   /* ref/ceiling.md range c -> i */
    return q_err(QE_TYPE);
}

/* q `neg` / monadic `-` (ref/neg.md domain b g x h i j e f c s p m d z n u v t):
 * negate any temporal type-PRESERVING and a bool promoting to INT (range b->i),
 * by reading the lane, negating, and letting the ONE cast home rebuild the
 * range type (neg 2000.01.01 2012.01.01 -> 2000.01.01 1988.01.01; 0Wd <-> -0Wd;
 * neg 12:00:00.000 -> -12:00:00.000).  The whole temporal domain is admitted by
 * ONE predicate — the six-arm gate is gone: TIME/TIMESTAMP were OUR deferral,
 * not kdb's (ref/neg.md's range pins t->t, p->p).  ints/floats/nulls delegate
 * to the base kernel (its INT_MIN->null width guards).  Registered ATOMIC, so a
 * vector arrives element-wise.  Two lanes: f64-backed (DATETIME, bool) vs
 * i64-backed (the int temporals); a temporal's only INT64_MIN is its null,
 * caught first.  A `:pq:` handle sym negs to its async form (a peachq extension). */
ray_t* q_neg_wrap(ray_t* x) {
    if (x && (is_temporal(x) || RAY_IS_TEMPORALF(-x->type) || q_type_is_bool(x))) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        int8_t tag = -x->type;
        int8_t out = q_type_is_bool(x) ? RAY_I32 : tag;
        ray_t* p = (q_type_is_bool(x) || RAY_IS_TEMPORALF(tag)) ? ray_f64(-as_f64(x))
                                                                : ray_i64(-q_type_as_i64(x));
        if (RAY_IS_ERR(p)) return p;
        ray_t* r = q_dollar_cast(out, p);
        ray_release(p);
        return r;
    }
    if (q_type_is_char_atom(x)) return ray_i32(-(int32_t)x->u8);   /* ref/neg.md range c -> i */
    if (x && x->type == -RAY_SYM) {
        ray_t* r = q_handles_pq_neg(x);
        return r ? r : q_err(QE_TYPE);
    }
    return ray_neg_fn(x);
}

/* q `x xexp y` — C pow(), the kdb 4.0 2020.07.15 law (embedPy tests/curvefit.t), not ref/exp.md's older exp y * log x:
 * `2 xexp 3` is exactly 8f and `-1 xexp 2` is 1f.  A negative base with a fractional or infinite exponent is 0n,
 * which pow alone misses at pow(-2, inf) = inf and pow(-inf, .5) = inf.  The published domain is `b x h i j e f` on
 * both axes: char is refused although it shares the byte lane, and a null is refused by its TYPE before it is honoured
 * as a null (0Ng). */
ray_t* q_xexp_wrap(ray_t* x, ray_t* y) {
    if (!x || !y || !is_numeric(x) || !is_numeric(y) || q_type_is_char_atom(x) || q_type_is_char_atom(y))
        return q_err(QE_TYPE);
    if (RAY_ATOM_IS_NULL(x) || RAY_ATOM_IS_NULL(y))
        return ray_typed_null(-RAY_F64);
    double xf = as_f64(x), yf = as_f64(y);
    if (xf < 0 && (!isfinite(yf) || yf != floor(yf)))
        return ray_typed_null(-RAY_F64);
    return ray_pow_fn(x, y);
}

/* q `x xlog y` — base-x logarithm of y as a FLOAT: log(yf)/log(xf) with both
 * operands cast to float first (ref/log.md "the base-xf logarithm of yf").
 * y null -> 0n; y negative -> 0n (log NaN); y zero -> -inf -> 0n (kdb -0w —
 * the documented single-null divergence).  CHAR operands read as their code
 * points (ref/log.md pins `"A" xlog "C"` == `65 xlog 67` -> 1.00726); xexp
 * does NOT share the char arm (its domain table rejects chars). */
static int xlog_operand(ray_t* v, double* out) {
    if (!v) return 0;
    if (v->type == -RAY_STR && ray_str_len(v) == 1) {   /* legacy 1-char string */
        *out = (double)(unsigned char)ray_str_ptr(v)[0];
        return 1;
    }
    if (v->type == -RAY_CHARV) { *out = (double)v->u8; return 1; }  /* char atom */
    /* Temporal operands cast to float via their payload (ref/log.md domain
     * table: p m d n u v t all map to f; z and s are excluded — codex r2).
     * Temporal NULLS pass through here too; the wrap's null gate turns them
     * into 0n before the payload is used. */
    if (v->type < 0 && RAY_IS_TEMPORAL32(-v->type)) { *out = (double)v->i32; return 1; }
    if (v->type < 0 && RAY_IS_TEMPORAL64(-v->type)) { *out = (double)v->i64; return 1; }
    if (is_numeric(v) || RAY_ATOM_IS_NULL(v)) { *out = as_f64(v); return 1; }
    return 0;
}
ray_t* q_xlog_wrap(ray_t* x, ray_t* y) {
    double xf, yf;
    if (!xlog_operand(x, &xf) || !xlog_operand(y, &yf))
        return q_err(QE_TYPE);
    if ((x->type != -RAY_STR && RAY_ATOM_IS_NULL(x)) ||
        (y->type != -RAY_STR && RAY_ATOM_IS_NULL(y)))
        return ray_typed_null(-RAY_F64);
    return make_f64(log(yf) / log(xf));
}

/* q `x mmu y` — matrix multiply / dot product (ref/mmu.md).  f64-only (`real`/int
 * -> type; the doc says "float").  Each entry is mmu_dot over a row of x and a
 * column of q_flip_wrap y.  A vector is an f64 vec whose axis drops from the
 * result; a matrix is a rectangular list of f64 vecs.  Shape validated up front:
 * ragged / count-y != count-first-x / empty inner axis -> length. */
int q_mmu_class(ray_t* v, int64_t* first) {  /* 0=vector, 1=matrix, else QMMU_* */
    if (v && v->type == RAY_F64) { *first = q_count(v); return 0; }   /* count x */
    if (v && v->type == RAY_LIST && q_count(v) > 0) {
        ray_t** e = (ray_t**)ray_data(v);
        int64_t w = -1;
        for (int64_t i = 0; i < q_count(v); i++) {
            if (!e[i] || e[i]->type != RAY_F64) return QMMU_BAD;
            int64_t l = q_count(e[i]);
            if (w < 0) w = l; else if (l != w) return QMMU_RAGGED;
        }
        *first = w;                                                   /* count first x */
        return 1;
    }
    return QMMU_BAD;
}

/* The inner product mmu is built from — every operand reaching it has already been
 * proved a RAY_F64 vector of length n by q_mmu_class, so the read is direct.  NaN
 * folds to the one float null, as the make_f64 this replaced did; ±Inf survives. */
static double mmu_dot(const double* a, const double* b, int64_t n) {
    double acc = 0.0;
    for (int64_t i = 0; i < n; i++) acc += a[i] * b[i];
    return __builtin_isnan(acc) ? NULL_F64 : acc;
}

ray_t* q_mmu_wrap(ray_t* x, ray_t* y) {
    int64_t kx, ky;                                     /* count-first (matrix) / count (vec) */
    int xc = q_mmu_class(x, &kx), yc = q_mmu_class(y, &ky);
    if (xc == QMMU_BAD || yc == QMMU_BAD) return q_err(QE_TYPE);
    if (xc == QMMU_RAGGED || yc == QMMU_RAGGED) return q_err(QE_LENGTH);
    if (kx != q_count(y)) return q_err(QE_LENGTH);          /* count y must match */
    if (kx <= 0) return q_err(QE_LENGTH);                   /* no product over an empty axis */

    ray_t* ycols = yc ? q_flip_wrap(y) : NULL;          /* owned: cols of y as f64 vecs */
    if (yc && (!ycols || RAY_IS_ERR(ycols))) return ycols ? ycols : q_err(QE_OOM);
    if (yc && ky > 0) {                                 /* the flip is what mmu_dot reads */
        int64_t w;
        if (q_mmu_class(ycols, &w) != 1 || w != kx || q_count(ycols) != ky) { ray_release(ycols); return q_err(QE_TYPE); }
    }
    ray_t** rowv = xc ? (ray_t**)ray_data(x) : NULL;
    ray_t** colv = yc ? (ray_t**)ray_data(ycols) : NULL;
    int64_t R = xc ? q_count(x) : 1;                    /* result rows (dropped if x is a vec) */
    int64_t C = yc ? ky : 1;                            /* result cols (dropped if y is a vec) */

    /* scalar: vector . vector -> float atom */
    if (!xc && !yc) return make_f64(mmu_dot(ray_data(x), ray_data(y), kx));

    /* matrix . matrix -> list of R f64 vecs, each length C */
    if (xc && yc) {
        ray_t* out = ray_list_new(R > 0 ? R : 1);
        if (!out || RAY_IS_ERR(out)) { ray_release(ycols); return out ? out : q_err(QE_OOM); }
        for (int64_t i = 0; i < R; i++) {
            ray_t* row = ray_vec_new(RAY_F64, C > 0 ? C : 1);
            if (!row || RAY_IS_ERR(row)) { ray_release(out); ray_release(ycols); return row ? row : q_err(QE_OOM); }
            row->len = C;
            double* od = (double*)ray_data(row);
            for (int64_t j = 0; j < C; j++)
                od[j] = mmu_dot(ray_data(rowv[i]), ray_data(colv[j]), kx);
            out = ray_list_append(out, row); ray_release(row);   /* append RETAINS */
            if (RAY_IS_ERR(out)) { ray_release(ycols); return out; }
        }
        ray_release(ycols);
        return out;
    }

    /* exactly one matrix operand -> f64 vec (the other axis drops) */
    int64_t n = xc ? R : C;
    ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1);
    if (!out || RAY_IS_ERR(out)) { if (ycols) ray_release(ycols); return out ? out : q_err(QE_OOM); }
    out->len = n;
    double* od = (double*)ray_data(out);
    for (int64_t k = 0; k < n; k++)
        od[k] = mmu_dot(ray_data(xc ? rowv[k] : x), ray_data(yc ? colv[k] : y), kx);
    if (ycols) ray_release(ycols);
    return out;
}

/* inv/lsq factor in flat row-major scratch; results rebuild as the q_mmu_wrap shape. */
static double* mat_flat(ray_t* m, int64_t rows, int64_t cols) {
    double* a = (double*)malloc((size_t)(rows * cols) * sizeof(double));
    if (!a) return NULL;
    ray_t** rowv = (ray_t**)ray_data(m);
    for (int64_t i = 0; i < rows; i++)
        memcpy(a + i * cols, ray_data(rowv[i]), (size_t)cols * sizeof(double));
    return a;
}

static ray_t* mat_rows_new(const double* a, int64_t rows, int64_t cols) {
    ray_t* out = ray_list_new(rows > 0 ? rows : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    for (int64_t i = 0; i < rows; i++) {
        ray_t* row = ray_vec_from_raw(RAY_F64, a + i * cols, cols);
        if (!row || RAY_IS_ERR(row)) { ray_release(out); return row ? row : q_err(QE_OOM); }
        out = ray_list_append(out, row); ray_release(row);   /* append RETAINS */
        if (RAY_IS_ERR(out)) return out;
    }
    return out;
}

/* q `inv x` — matrix inverse of a non-singular float matrix by LU decomposition
 * with partial pivoting (ref/inv.md "Since V3.6 2017.09.26 inv uses LU
 * decomposition"; float-only per ref/matrixes.md).  Right-looking elimination,
 * then one permuted-identity solve per result column — this exact order
 * reproduces the doc goldens' rounding noise (`a mmu inv a` off-diagonals).
 * Singular pivot -> 'domain (undocumented; lsq's non-PD case matches). */
ray_t* q_inv_wrap(ray_t* x) {
    int64_t w;
    int xc = q_mmu_class(x, &w);
    if (xc == QMMU_BAD || xc == 0) return q_err(QE_TYPE);
    if (xc == QMMU_RAGGED) return q_err(QE_LENGTH);
    int64_t n = q_count(x);
    if (w != n) return q_err(QE_LENGTH);

    double* a = mat_flat(x, n, n);
    int64_t* piv = a ? (int64_t*)malloc((size_t)n * sizeof(int64_t)) : NULL;
    double* r = piv ? (double*)malloc((size_t)(n * n + n) * sizeof(double)) : NULL;
    if (!r) { free(piv); free(a); return q_err(QE_WSFULL); }
    double* b = r + n * n;
    for (int64_t i = 0; i < n; i++) piv[i] = i;

    for (int64_t k = 0; k < n; k++) {
        int64_t p = k;
        for (int64_t i = k + 1; i < n; i++)
            if (fabs(a[i * n + k]) > fabs(a[p * n + k])) p = i;
        if (a[p * n + k] == 0.0) { free(r); free(piv); free(a); return q_err(QE_DOMAIN); }
        if (p != k) {
            for (int64_t j = 0; j < n; j++) { double t = a[k * n + j]; a[k * n + j] = a[p * n + j]; a[p * n + j] = t; }
            int64_t t = piv[k]; piv[k] = piv[p]; piv[p] = t;
        }
        for (int64_t i = k + 1; i < n; i++) {
            a[i * n + k] /= a[k * n + k];
            for (int64_t j = k + 1; j < n; j++) a[i * n + j] -= a[i * n + k] * a[k * n + j];
        }
    }

    for (int64_t c = 0; c < n; c++) {
        for (int64_t i = 0; i < n; i++) b[i] = piv[i] == c ? 1.0 : 0.0;
        for (int64_t i = 0; i < n; i++) {                    /* L (unit diag) forward */
            double s = b[i];
            for (int64_t j = 0; j < i; j++) s -= a[i * n + j] * b[j];
            b[i] = s;
        }
        for (int64_t i = n - 1; i >= 0; i--) {               /* U backward */
            double s = b[i];
            for (int64_t j = i + 1; j < n; j++) s -= a[i * n + j] * b[j];
            b[i] = s / a[i * n + i];
        }
        for (int64_t i = 0; i < n; i++) r[i * n + c] = b[i];
    }

    ray_t* out = mat_rows_new(r, n, n);
    free(r); free(piv); free(a);
    return out;
}

/* q `x lsq y` — least squares: R such that R mmu y best fits x, both float
 * matrixes sharing a column count (ref/lsq.md: "lsq solves a normal equations
 * matrix via Cholesky decomposition"; float-only per ref/matrixes.md).  NEVER
 * `x mmu inv y` — that shortcut needs square y, and ref/lsq.md's own second
 * example divides a 3x4 by a 3x4.  G = y mmu flip y, C = x mmu flip y (the
 * existing kernels); G symmetric makes R G = C a per-row solve through the
 * Cholesky factor.  Non-positive-definite G -> 'domain, the inv singular class. */
ray_t* q_lsq_wrap(ray_t* x, ray_t* y) {
    int64_t px, py;
    int xc = q_mmu_class(x, &px), yc = q_mmu_class(y, &py);
    if (xc == QMMU_BAD || yc == QMMU_BAD || xc == 0 || yc == 0) return q_err(QE_TYPE);
    if (xc == QMMU_RAGGED || yc == QMMU_RAGGED) return q_err(QE_LENGTH);
    if (px != py) return q_err(QE_LENGTH);
    int64_t m = q_count(x), n = q_count(y);

    ray_t* yt = q_flip_wrap(y);
    if (!yt || RAY_IS_ERR(yt)) return yt ? yt : q_err(QE_OOM);
    ray_t* gm = q_mmu_wrap(y, yt);                          /* n x n normal matrix */
    if (!gm || RAY_IS_ERR(gm)) { ray_release(yt); return gm ? gm : q_err(QE_OOM); }
    ray_t* cm = q_mmu_wrap(x, yt);                          /* m x n right-hand sides */
    ray_release(yt);
    if (!cm || RAY_IS_ERR(cm)) { ray_release(gm); return cm ? cm : q_err(QE_OOM); }

    double* g = mat_flat(gm, n, n);
    double* c = g ? mat_flat(cm, m, n) : NULL;
    ray_release(gm); ray_release(cm);
    if (!c) { free(g); return q_err(QE_WSFULL); }

    for (int64_t i = 0; i < n; i++) {                        /* Cholesky G = L L' in place */
        for (int64_t j = 0; j <= i; j++) {
            double s = g[i * n + j];
            for (int64_t k = 0; k < j; k++) s -= g[i * n + k] * g[j * n + k];
            if (i == j) {
                if (s <= 0.0) { free(c); free(g); return q_err(QE_DOMAIN); }
                g[i * n + j] = sqrt(s);
            } else
                g[i * n + j] = s / g[j * n + j];
        }
    }

    for (int64_t row = 0; row < m; row++) {                  /* per row: L z = c, L' r = z */
        double* b = c + row * n;
        for (int64_t i = 0; i < n; i++) {
            double s = b[i];
            for (int64_t k = 0; k < i; k++) s -= g[i * n + k] * b[k];
            b[i] = s / g[i * n + i];
        }
        for (int64_t i = n - 1; i >= 0; i--) {
            double s = b[i];
            for (int64_t k = i + 1; k < n; k++) s -= g[k * n + i] * b[k];
            b[i] = s / g[i * n + i];
        }
    }

    ray_t* out = mat_rows_new(c, m, n);
    free(c); free(g);
    return out;
}

/* q char-string comparison — q treats a string as a char vector, so `=`/`<>`
 * compare element-wise and yield a boolean vector (`"abc"="abd"` -> 110b).
 * rayfall's `==`/`!=` (ray_eq_fn/ray_neq_fn) compare two -RAY_STR atoms as
 * whole values (a single 0b/1b), so the q verbs wrap them.  Two -RAY_STR
 * operands take the element-wise path here (equal length -> boolean vector,
 * unequal -> a q `length` error); everything else delegates to rayfall. */
static int is_str_atom(ray_t* x) { return x && x->type == -RAY_STR; }

static ray_t* str_cmp_vec(ray_t* a, ray_t* b, int eq) {
    const char* pa = ray_str_ptr(a); size_t la = ray_str_len(a);
    const char* pb = ray_str_ptr(b); size_t lb = ray_str_len(b);
    if (la != lb)
        return q_err(QE_LENGTH);
    uint8_t stack[128];
    uint8_t* bits = (la <= sizeof stack) ? stack : (uint8_t*)malloc(la ? la : 1);
    if (!bits) return q_err(QE_WSFULL);
    for (size_t i = 0; i < la; i++)
        bits[i] = (uint8_t)(eq ? (pa[i] == pb[i]) : (pa[i] != pb[i]));
    ray_t* r = ray_vec_from_raw(RAY_BOOL, bits, (int64_t)la);
    if (bits != stack) free(bits);
    return r;
}

/* q `=`/`<>` own their structure dispatch (Q_OPS rows are QR_FN2, NON-atomic):
 * legacy STR pairs keep str_cmp_vec; every other collection shape — charv
 * included, exactly as u8 — delegates to the SAME opcode-0 atomic broadcast
 * eval used before the rows dropped RAY_FN_ATOMIC (recursion re-enters this
 * wrapper per element and terminates at the two-atom scalar kernel). */
ray_t* q_eq_wrap(ray_t* a, ray_t* b) {
    if (is_str_atom(a) && is_str_atom(b)) return str_cmp_vec(a, b, 1);
    if (is_collection(a) || is_collection(b))
        return atomic_map_binary(q_eq_wrap, a, b);
    return ray_eq_fn(a, b);
}

ray_t* q_ne_wrap(ray_t* a, ray_t* b) {
    if (is_str_atom(a) && is_str_atom(b)) return str_cmp_vec(a, b, 0);
    if (is_collection(a) || is_collection(b))
        return atomic_map_binary(q_ne_wrap, a, b);
    return ray_neq_fn(a, b);
}

/* q dyadic `&`/`|` — Lesser/Greater: min/max, and on flags boolean and/or.
 * ref/lesser.md gives "the lesser of their underlying VALUES returned as the
 * higher of the two TYPES" — a compare-and-retag, never a promoted
 * computation, so the whole domain is one ordering probe (ray_min2_fn) plus
 * the printed result-type matrix (q_type_common) discharged through THE cast
 * home.  Registered ATOMIC: eval maps the vector/dict/table shapes. */
static ray_t* minmax2(ray_t* a, ray_t* b, int want_min) {
    if (!a || !b || !ray_is_atom(a) || !ray_is_atom(b))
        return q_err(QE_TYPE);
    int8_t ta = (int8_t)-a->type, tb = (int8_t)-b->type;
    int8_t t = q_type_common(ta, tb);
    if (!t) return q_err(QE_TYPE);
    ray_t* r = want_min ? ray_min2_fn(a, b) : ray_max2_fn(a, b);
    /* the matrix diagonal is the identity, so a same-tag pair needs no retag */
    if (ta == tb || !r || RAY_IS_ERR(r)) return r;
    /* min2/max2 PICK an operand, so the winner's tag is ta or tb by identity —
     * when it already is the result tag the retag is a no-op too. */
    if (((r == a) ? ta : tb) == t) return r;
    ray_t* c = q_dollar_cast(t, r);
    ray_release(r);
    return c;
}

ray_t* q_min2_wrap(ray_t* a, ray_t* b) { return minmax2(a, b, 1); }
ray_t* q_max2_wrap(ray_t* a, ray_t* b) { return minmax2(a, b, 0); }

/* q `not x` — "0b where x is not equal to zero, and 1b otherwise" (ref/not.md).
 * That IS `x=0`: ray_eq_fn already owns every lane's zero, the null rule
 * ("nulls and infinities never equal zero") and the symbol refusal. */
ray_t* q_not_wrap(ray_t* x) {
    ray_t* z = ray_i64(0);
    ray_t* r = ray_eq_fn(x, z);
    ray_release(z);
    return r;
}
