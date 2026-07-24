/* q_math.c — atomic unary/dyadic math (libm family, xexp/xlog), comparison
 * wrappers (= <> & ~), and neg/null/within
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_err.h"
#include "qlang/ops/q_dollar.h" /* q_dollar_cast — THE conversion home */
#include "lang/eval.h"     /* ray_eq_fn/ray_neq_fn, ray_neg_fn */
#include "lang/internal.h" /* atomic_map_unary, as_f64, is_numeric, is_temporal, make_f64 */
#include "qlang/q_type.h"  /* q_type_as_i64 / q_type_is_numeric_or_temporal / q_type_is_bool */
#include <math.h>          /* sin/cos/tan/asin/acos/atan, exp/log, floor/floorf, ceil/ceilf */
#include <string.h>        /* memcmp, memcpy */
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
    if (q_type_is_numeric_or_temporal(x)) {
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
 * caught first. */
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
    return ray_neg_fn(x);
}

/* ---- dyadic atomic math (feat/q-math-parse-display) ----------------------
 * q `x xexp y` — x to the power y as a FLOAT (ref/exp.md).  The doc pins the
 * COMPUTATION, not just the value: "The calculation is performed as
 * exp y * log x" (so `2 xexp 3` is 7.999…, NOT C pow's exact 8 — codex r1).
 * All the doc's edge rules fall out of the identity: x null or NEGATIVE ->
 * log NaN -> 0n; y null -> 0n; x=0,y>0 -> 0f; overflow +inf -> 0n via
 * make_f64 (single-null model; kdb shows 0w — documented divergence).
 * Domain is numeric-only: ref/exp.md's table rejects char args. */
ray_t* q_xexp_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    if (!is_numeric(x) && !RAY_ATOM_IS_NULL(x))
        return q_err(QE_TYPE);
    if (!is_numeric(y) && !RAY_ATOM_IS_NULL(y))
        return q_err(QE_TYPE);
    if (RAY_ATOM_IS_NULL(x) || RAY_ATOM_IS_NULL(y))
        return ray_typed_null(-RAY_F64);
    return make_f64(exp(as_f64(y) * log(as_f64(x))));
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
 * -> type; the doc says "float").  Each entry is ray_inner_prod_fn over a row of x
 * and a column of q_flip_wrap y (reusing both kernels).  A vector is an f64 vec
 * whose axis drops from the result; a matrix is a rectangular list of f64 vecs.
 * Shape validated up front: ragged / count-y != count-first-x -> length. */
int q_mmu_class(ray_t* v, int64_t* first) {  /* 0=vector, 1=matrix, else QMMU_* */
    if (v && v->type == RAY_F64) { *first = ray_len(v); return 0; }   /* count x */
    if (v && v->type == RAY_LIST && ray_len(v) > 0) {
        ray_t** e = (ray_t**)ray_data(v);
        int64_t w = -1;
        for (int64_t i = 0; i < ray_len(v); i++) {
            if (!e[i] || e[i]->type != RAY_F64) return QMMU_BAD;
            int64_t l = ray_len(e[i]);
            if (w < 0) w = l; else if (l != w) return QMMU_RAGGED;
        }
        *first = w;                                                   /* count first x */
        return 1;
    }
    return QMMU_BAD;
}

ray_t* q_mmu_wrap(ray_t* x, ray_t* y) {
    int64_t kx, ky;                                     /* count-first (matrix) / count (vec) */
    int xc = q_mmu_class(x, &kx), yc = q_mmu_class(y, &ky);
    if (xc == QMMU_BAD || yc == QMMU_BAD) return q_err(QE_TYPE);
    if (xc == QMMU_RAGGED || yc == QMMU_RAGGED) return q_err(QE_LENGTH);
    if (kx != ray_len(y)) return q_err(QE_LENGTH);          /* count y must match */

    ray_t* ycols = yc ? q_flip_wrap(y) : NULL;          /* owned: cols of y as f64 vecs */
    if (yc && (!ycols || RAY_IS_ERR(ycols))) return ycols ? ycols : q_err(QE_OOM);
    ray_t** rowv = xc ? (ray_t**)ray_data(x) : NULL;
    ray_t** colv = yc ? (ray_t**)ray_data(ycols) : NULL;
    int64_t R = xc ? ray_len(x) : 1;                    /* result rows (dropped if x is a vec) */
    int64_t C = yc ? ky : 1;                            /* result cols (dropped if y is a vec) */

    /* scalar: vector . vector -> float atom (or propagated kernel error) */
    if (!xc && !yc) return ray_inner_prod_fn(x, y);

    /* matrix . matrix -> list of R f64 vecs, each length C */
    if (xc && yc) {
        ray_t* out = ray_list_new(R > 0 ? R : 1);
        if (!out || RAY_IS_ERR(out)) { ray_release(ycols); return out ? out : q_err(QE_OOM); }
        for (int64_t i = 0; i < R; i++) {
            ray_t* row = ray_vec_new(RAY_F64, C > 0 ? C : 1);
            if (!row || RAY_IS_ERR(row)) { ray_release(out); ray_release(ycols); return row ? row : q_err(QE_OOM); }
            row->len = C;
            double* od = (double*)ray_data(row);
            for (int64_t j = 0; j < C; j++) {
                ray_t* d = ray_inner_prod_fn(rowv[i], colv[j]);
                if (!d || RAY_IS_ERR(d)) { ray_release(row); ray_release(out); ray_release(ycols); return d ? d : q_err(QE_OOM); }
                od[j] = as_f64(d); ray_release(d);
            }
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
    for (int64_t k = 0; k < n; k++) {
        ray_t* d = ray_inner_prod_fn(xc ? rowv[k] : x, yc ? colv[k] : y);
        if (!d || RAY_IS_ERR(d)) { ray_release(out); if (ycols) ray_release(ycols); return d ? d : q_err(QE_OOM); }
        od[k] = as_f64(d); ray_release(d);
    }
    if (ycols) ray_release(ycols);
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

/* q dyadic `&` — min / boolean-and.  The wrapper is registered ATOMIC, so
 * vector/scalar and vector/vector cases are mapped by eval over atom pairs. */
ray_t* q_min2_wrap(ray_t* a, ray_t* b) {
    if (!a || !b || !ray_is_atom(a) || !ray_is_atom(b))
        return q_err(QE_TYPE);
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return ray_bool(a->b8 && b->b8);
    if (a->type == -RAY_F64 || b->type == -RAY_F64 ||
        a->type == -RAY_F32 || b->type == -RAY_F32) {
        double av = as_f64(a);
        double bv = as_f64(b);
        return ray_f64(av <= bv ? av : bv);
    }
    if ((a->type == -RAY_I64 || a->type == -RAY_I32 || a->type == -RAY_I16 ||
         a->type == -RAY_BYTE_ONLY || a->type == -RAY_BOOL) &&
        (b->type == -RAY_I64 || b->type == -RAY_I32 || b->type == -RAY_I16 ||
         b->type == -RAY_BYTE_ONLY || b->type == -RAY_BOOL)) {
        int64_t av = (a->type == -RAY_BOOL) ? a->b8 : as_i64(a);
        int64_t bv = (b->type == -RAY_BOOL) ? b->b8 : as_i64(b);
        return ray_i64(av <= bv ? av : bv);
    }
    return q_err(QE_TYPE);
}

/* q `x~y` — recursive whole-value equivalence (kdb match): TYPE-strict
 * (`1~1f` is 0b), attribute-blind (`1 2 3~\`s#1 2 3` is 1b), sentinel nulls
 * compare equal (`0n~0n` is 1b — non-finites canonicalize to one payload).
 * Unhandled types conservatively mismatch (kdb ~ never errors). */
int q_match_rec(ray_t* a, ray_t* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->type != b->type) return 0;
    if (a->type == -RAY_SYM) return a->i64 == b->i64;
    if (a->type == -RAY_STR)
        return ray_str_len(a) == ray_str_len(b) &&
               memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) == 0;
    if (a->type == -RAY_GUID) {
        /* payload lives in a 16-byte U8 buffer behind the obj pointer —
         * an 8-byte union memcmp would compare POINTERS (codex P2). */
        return a->obj && b->obj &&
               memcmp(ray_data(a->obj), ray_data(b->obj), 16) == 0;
    }
    if (ray_is_atom(a)) {
        /* inline-payload scalars ONLY (ray_is_atom also covers LAMBDA and
         * fn values, whose state is NOT in the union slot — those fall to
         * the conservative-mismatch tail below). */
        switch (-a->type) {
        case RAY_BOOL: RAY_BYTE_CASES: case RAY_I16: case RAY_I32: case RAY_I64:
        case RAY_F32: case RAY_F64:
        RAY_TEMPORAL32_CASES: RAY_TEMPORAL64_CASES: RAY_TEMPORALF_CASES:
            return memcmp(&a->i64, &b->i64, 8) == 0;   /* payload union */
        default:
            return 0;
        }
    }
    if (a->type == RAY_DICT || a->type == RAY_TABLE) {
        ray_t** ea = (ray_t**)ray_data(a);
        ray_t** eb = (ray_t**)ray_data(b);
        return q_match_rec(ea[0], eb[0]) && q_match_rec(ea[1], eb[1]);
    }
    if (a->type == RAY_LIST || ray_is_vec(a)) {
        int64_t la = ray_len(a);
        if (la != ray_len(b)) return 0;
        /* same-type numeric vectors: payload memcmp (nulls are in-payload
         * sentinels; attrs deliberately not compared).  SYM vecs vary in
         * index width -> per-element below. */
        if (ray_is_vec(a) && a->type != RAY_SYM && a->type != RAY_STR) {
            size_t esz = (a->type == RAY_I64 || a->type == RAY_F64 ||
                          RAY_IS_TEMPORALF(a->type)) ? 8
                       : (a->type == RAY_I32 || a->type == RAY_F32) ? 4
                       : (a->type == RAY_I16) ? 2
                       : (a->type == RAY_BOOL || ray_is_bytelike(a->type)) ? 1 : 0;
            if (esz)
                return memcmp(ray_data(a), ray_data(b), (size_t)la * esz) == 0;
        }
        for (int64_t i = 0; i < la; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* xa = ray_at_fn(a, ia);
            ray_t* xb = ray_at_fn(b, ia);
            ray_release(ia);
            int r = (xa && xb && !RAY_IS_ERR(xa) && !RAY_IS_ERR(xb))
                        ? q_match_rec(xa, xb) : 0;
            if (xa) ray_release(xa);
            if (xb) ray_release(xb);
            if (!r) return 0;
        }
        return 1;
    }
    return 0;
}

ray_t* q_match_wrap(ray_t* a, ray_t* b) {
    return ray_bool(q_match_rec(a, b));
}

/* q `x within y` — bounds check (ref/within.md: 1 3 10 6 4 within 2 6 ->
 * 01011b; inclusive).  Base ray_within_fn takes VECTOR vals only and reads
 * the range buffer at the vals' element width, so: an atom x is enlisted
 * (via list+collapse) and the answer unwrapped back to a bool atom, and the
 * two element widths must agree ('type — a silent misread otherwise).  The
 * flip-of-pairs range form and mixed-width operands are deferred cells. */
ray_t* q_within_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    if (!ray_is_vec(y) || ray_len(y) != 2)
        return q_err(QE_TYPE);
    ray_t* vals = x;
    ray_t* vals_owned = NULL;
    if (ray_is_atom(x)) {
        ray_t* l = ray_list_new(1);
        if (RAY_IS_ERR(l)) return l;
        l = ray_list_append(l, x);
        if (RAY_IS_ERR(l)) return l;
        vals_owned = q_list_collapse(l);
        ray_release(l);
        if (!vals_owned || RAY_IS_ERR(vals_owned))
            return vals_owned ? vals_owned : q_err(QE_TYPE);
        if (!ray_is_vec(vals_owned)) {           /* strings & friends: deferred */
            ray_release(vals_owned);
            return q_err(QE_TYPE);
        }
        vals = vals_owned;
    }
    if (!ray_is_vec(vals)) {
        if (vals_owned) ray_release(vals_owned);
        return q_err(QE_TYPE);
    }
    /* Base ray_within_fn dispatches on vals->type ONLY and reads the range
     * buffer as that element type, so ANY type mismatch — not just a width
     * mismatch — would silently reinterpret raw bits (codex: 1 2 within
     * 1.5 2.5 read the doubles as int64 -> 00b).  Same-type operands only;
     * mixed-type coercion is a deferred cell (error, never a wrong answer). */
    if (vals->type != y->type) {
        if (vals_owned) ray_release(vals_owned);
        return q_err(QE_TYPE);
    }
    ray_t* r;
    if (vals->type == RAY_TIMESTAMP) {
        /* base ray_within_fn has no i64-temporal arm; the payload is i64, so
         * relabel both sides through the one cast home and delegate (the
         * same-byte-rep TIMESTAMP<->I64 relabel, builtins.c). */
        ray_t* vi = q_dollar_cast(RAY_I64, vals);
        if (!vi || RAY_IS_ERR(vi)) { if (vals_owned) ray_release(vals_owned); return vi; }
        ray_t* yi = q_dollar_cast(RAY_I64, y);
        if (!yi || RAY_IS_ERR(yi)) {
            ray_release(vi);
            if (vals_owned) ray_release(vals_owned);
            return yi;
        }
        r = ray_within_fn(vi, yi);
        ray_release(vi);
        ray_release(yi);
    } else {
        r = ray_within_fn(vals, y);
    }
    if (!vals_owned) return r;                    /* vector x: pass through */
    ray_release(vals_owned);
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* idx = ray_i64(0);                      /* atom x: unwrap 1-vec */
    ray_t* a = ray_at_fn(r, idx);
    ray_release(idx);
    ray_release(r);
    return a;
}
