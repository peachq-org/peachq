/* q_wrap_math.c — atomic unary/dyadic math (libm family, xexp/xlog), comparison
 * wrappers (= <> & ~), til/where/reverse, and the vs/sv family
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "lang/eval.h"     /* ray_eq_fn/ray_neq_fn, ray_til_fn, ray_reverse_fn */
#include "lang/internal.h" /* atomic_map_unary, as_f64/as_i64, is_numeric, RAY_IS_TEMPORAL64, ray_error */
#include "lang/format.h"   /* ray_type_name — error messages */
#include "ops/ops.h"       /* ray_is_lazy, ray_lazy_materialize */
#include "table/sym.h"     /* ray_sym_vec_cell, ray_sym_intern_runtime — vs/sv sym arms */
#include <math.h>          /* sin/cos/tan/asin/acos/atan, exp/log, floor/floorf, ceil/ceilf */
#include <string.h>        /* memcmp, memcpy */
#include <stdlib.h>        /* malloc, free */


/* q monadic `_` — floor to LONG (kdb `_ 3.7` is 3j; rayfall floor keeps f64).
 * Ints/bools pass through; f64 null -> long null.  RAY_FN_ATOMIC maps it
 * element-wise over float vectors. */
ray_t* q_floor_wrap(ray_t* x) {
    if (!x) return ray_error("type", "_ (floor): nil");
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
    return ray_error("type", "_ (floor): expects a numeric, got %s",
                     ray_type_name(x->type));
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
        if (!x) return ray_error("type", GLYPH ": nil");                       \
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);              \
        if (is_numeric(x)) return make_f64(FN(as_f64(x)));                     \
        return ray_error("type", GLYPH ": expects a numeric argument, got %s", \
                         ray_type_name(x->type));                              \
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
 * TEMPORAL atoms sign their underlying payload (ref/signum.md pins
 * `signum 1999.12.31` -> -1i — a pre-epoch date is negative), and every typed
 * null is null (-1i). */
static ray_t* q_signum_atom(ray_t* x) {
    if (!x) return ray_error("type", "signum: nil");
    if (RAY_ATOM_IS_NULL(x)) return ray_i32(-1);
    if (x->type < 0 && RAY_IS_TEMPORAL32(-x->type)) {
        int32_t v = x->i32;
        return ray_i32(v < 0 ? -1 : (v > 0 ? 1 : 0));
    }
    if (x->type < 0 && RAY_IS_TEMPORAL64(-x->type)) {
        int64_t v = x->i64;
        return ray_i32(v < 0 ? -1 : (v > 0 ? 1 : 0));
    }
    if (x->type == -RAY_DATETIME) {                    /* f64-payload temporal */
        double v = x->f64;
        return ray_i32(v < 0 ? -1 : (v > 0 ? 1 : 0));
    }
    if (is_numeric(x)) {
        /* as_f64 handles BOOL + every int/float width and preserves the sign
         * exactly (only magnitude precision is lost, irrelevant to sign), so
         * one branch covers all numeric atoms — avoids as_i64's missing BOOL
         * case (codex review). */
        double v = as_f64(x);
        return ray_i32(v < 0 ? -1 : (v > 0 ? 1 : 0));
    }
    return ray_error("type", "signum: expects a numeric argument, got %s",
                     ray_type_name(x->type));
}

/* Broadcast + collapse carrier (the q_null_wrap pattern): registered
 * RAY_FN_NONE so THIS wrapper drives the broadcast, letting a top-level boxed
 * list of i32 atoms collapse to an int vector — kdb shows `signum (0n;0N;0Nt)`
 * as ONE `-1 -1 -1i` line, not one atom per line. */
ray_t* q_signum_wrap(ray_t* x) {
    ray_t* r = is_collection(x) ? atomic_map_unary(q_signum_atom, x)
                                : q_signum_atom(x);
    if (!r || RAY_IS_ERR(r) || r->type != RAY_LIST) return r;
    ray_t* c = q_collapse_list(r);   /* owned: retains-or-builds */
    ray_release(r);
    return c;
}

/* q `ceiling x` — least integer >= x, returned as a LONG (kdb `ceiling 2.1` is
 * 3j).  The q_floor_wrap twin: rayfall's `ceil` keeps f64, so this wrapper rounds
 * to i64 exactly like q_floor_wrap.  Ints/bools pass through; f64 null -> long
 * null. */
ray_t* q_ceiling_wrap(ray_t* x) {
    if (!x) return ray_error("type", "ceiling: nil");
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
    return ray_error("type", "ceiling: expects a numeric argument, got %s",
                     ray_type_name(x->type));
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
    if (!x || !y) return ray_error("type", "xexp: nil operand");
    if (!is_numeric(x) && !RAY_ATOM_IS_NULL(x))
        return ray_error("type", "xexp: expects numeric arguments, got %s",
                         ray_type_name(x->type));
    if (!is_numeric(y) && !RAY_ATOM_IS_NULL(y))
        return ray_error("type", "xexp: expects numeric arguments, got %s",
                         ray_type_name(y->type));
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
static int q_xlog_operand(ray_t* v, double* out) {
    if (!v) return 0;
    if (v->type == -RAY_STR && ray_str_len(v) == 1) {   /* char atom */
        *out = (double)(unsigned char)ray_str_ptr(v)[0];
        return 1;
    }
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
    if (!q_xlog_operand(x, &xf) || !q_xlog_operand(y, &yf))
        return ray_error("type", "xlog: expects numeric or char arguments");
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
enum { QMMU_BAD = -1, QMMU_RAGGED = -2 };
static int q_mmu_class(ray_t* v, int64_t* first) {  /* 0=vector, 1=matrix, else QMMU_* */
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
    if (xc == QMMU_BAD || yc == QMMU_BAD) return ray_error("type", NULL);
    if (xc == QMMU_RAGGED || yc == QMMU_RAGGED) return ray_error("length", NULL);
    if (kx != ray_len(y)) return ray_error("length", NULL);          /* count y must match */

    ray_t* ycols = yc ? q_flip_wrap(y) : NULL;          /* owned: cols of y as f64 vecs */
    if (yc && (!ycols || RAY_IS_ERR(ycols))) return ycols ? ycols : ray_error("oom", NULL);
    ray_t** rowv = xc ? (ray_t**)ray_data(x) : NULL;
    ray_t** colv = yc ? (ray_t**)ray_data(ycols) : NULL;
    int64_t R = xc ? ray_len(x) : 1;                    /* result rows (dropped if x is a vec) */
    int64_t C = yc ? ky : 1;                            /* result cols (dropped if y is a vec) */

    /* scalar: vector . vector -> float atom (or propagated kernel error) */
    if (!xc && !yc) return ray_inner_prod_fn(x, y);

    /* matrix . matrix -> list of R f64 vecs, each length C */
    if (xc && yc) {
        ray_t* out = ray_list_new(R > 0 ? R : 1);
        if (!out || RAY_IS_ERR(out)) { ray_release(ycols); return out ? out : ray_error("oom", NULL); }
        for (int64_t i = 0; i < R; i++) {
            ray_t* row = ray_vec_new(RAY_F64, C > 0 ? C : 1);
            if (!row || RAY_IS_ERR(row)) { ray_release(out); ray_release(ycols); return row ? row : ray_error("oom", NULL); }
            row->len = C;
            double* od = (double*)ray_data(row);
            for (int64_t j = 0; j < C; j++) {
                ray_t* d = ray_inner_prod_fn(rowv[i], colv[j]);
                if (!d || RAY_IS_ERR(d)) { ray_release(row); ray_release(out); ray_release(ycols); return d ? d : ray_error("oom", NULL); }
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
    if (!out || RAY_IS_ERR(out)) { if (ycols) ray_release(ycols); return out ? out : ray_error("oom", NULL); }
    out->len = n;
    double* od = (double*)ray_data(out);
    for (int64_t k = 0; k < n; k++) {
        ray_t* d = ray_inner_prod_fn(xc ? rowv[k] : x, yc ? colv[k] : y);
        if (!d || RAY_IS_ERR(d)) { ray_release(out); if (ycols) ray_release(ycols); return d ? d : ray_error("oom", NULL); }
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
static int q_is_str_atom(ray_t* x) { return x && x->type == -RAY_STR; }

static ray_t* q_str_cmp_vec(ray_t* a, ray_t* b, int eq) {
    const char* pa = ray_str_ptr(a); size_t la = ray_str_len(a);
    const char* pb = ray_str_ptr(b); size_t lb = ray_str_len(b);
    if (la != lb)
        return ray_error("length", "%s: string lengths must match, got %zu and %zu",
                         eq ? "=" : "<>", la, lb);
    uint8_t stack[128];
    uint8_t* bits = (la <= sizeof stack) ? stack : (uint8_t*)malloc(la ? la : 1);
    if (!bits) return ray_error("wsfull", "=: out of memory");
    for (size_t i = 0; i < la; i++)
        bits[i] = (uint8_t)(eq ? (pa[i] == pb[i]) : (pa[i] != pb[i]));
    ray_t* r = ray_vec_from_raw(RAY_BOOL, bits, (int64_t)la);
    if (bits != stack) free(bits);
    return r;
}

/* q `=` — element-wise over char strings, else rayfall `==`.  RAY_FN_ATOMIC so
 * eval broadcasts it over numeric vectors (each element pair hits ray_eq_fn). */
ray_t* q_eq_wrap(ray_t* a, ray_t* b) {
    if (q_is_str_atom(a) && q_is_str_atom(b)) return q_str_cmp_vec(a, b, 1);
    return ray_eq_fn(a, b);
}

/* q `<>` — element-wise over char strings, else rayfall `!=`. */
ray_t* q_ne_wrap(ray_t* a, ray_t* b) {
    if (q_is_str_atom(a) && q_is_str_atom(b)) return q_str_cmp_vec(a, b, 0);
    return ray_neq_fn(a, b);
}

/* q dyadic `&` — min / boolean-and.  The wrapper is registered ATOMIC, so
 * vector/scalar and vector/vector cases are mapped by eval over atom pairs. */
ray_t* q_min2_wrap(ray_t* a, ray_t* b) {
    if (!a || !b || !ray_is_atom(a) || !ray_is_atom(b))
        return ray_error("type", "&: expects numeric atoms");
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return ray_bool(a->b8 && b->b8);
    if (a->type == -RAY_F64 || b->type == -RAY_F64 ||
        a->type == -RAY_F32 || b->type == -RAY_F32) {
        double av = as_f64(a);
        double bv = as_f64(b);
        return ray_f64(av <= bv ? av : bv);
    }
    if ((a->type == -RAY_I64 || a->type == -RAY_I32 || a->type == -RAY_I16 ||
         a->type == -RAY_U8  || a->type == -RAY_BOOL) &&
        (b->type == -RAY_I64 || b->type == -RAY_I32 || b->type == -RAY_I16 ||
         b->type == -RAY_U8  || b->type == -RAY_BOOL)) {
        int64_t av = (a->type == -RAY_BOOL) ? a->b8 : as_i64(a);
        int64_t bv = (b->type == -RAY_BOOL) ? b->b8 : as_i64(b);
        return ray_i64(av <= bv ? av : bv);
    }
    return ray_error("type", "&: unsupported operands");
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
        case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
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
                       : (a->type == RAY_BOOL || a->type == RAY_U8) ? 1 : 0;
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

/* q `til` — kdb accepts a boolean (`til 1b` -> ,0); base ray_til_fn is
 * int-only.  Everything else (int atoms, the error paths) delegates. */
ray_t* q_til_wrap(ray_t* x) {
    if (x && x->type == -RAY_BOOL) {
        ray_t* n = ray_i64(x->b8 ? 1 : 0);
        ray_t* r = ray_til_fn(n);
        ray_release(n);
        return r;
    }
    return ray_til_fn(x);
}

/* Borrow-or-collapse a dict's VALUES for a kernel call: a typed vector passes
 * through BORROWED (*owned=0); a boxed list collapses (q_collapse_list, owned
 * result, *owned=1) so homogeneous literal dicts hit the typed kernels.
 * Caller releases iff *owned.  NULL on a malformed dict. */
ray_t* q_dict_vals_vec(ray_t* d, int* owned) {
    *owned = 0;
    ray_t* vals = ray_dict_vals(d);              /* borrowed accessor */
    if (!vals) return NULL;
    if (vals->type == RAY_LIST) {
        ray_t* c = q_collapse_list(vals);        /* owned */
        if (c && !RAY_IS_ERR(c)) { *owned = 1; return c; }
        if (c) ray_release(c);
        return vals;                             /* uncollapsible: borrowed */
    }
    return vals;
}

/* q `where` / monadic `&` — an INTEGER vector repeats each index i, x[i] times
 * (`where 2 3 1` -> 0 0 1 1 1 2; `where 0 1 0 1 0 1` -> 1 3 5).  Base
 * ray_where_fn handles the boolean-mask form, so delegate for it and anything
 * else.  Result is a long vector (kdb).  Negative counts are 'domain. */
ray_t* q_where_wrap(ray_t* x) {
    /* where d — keys replicated by the (int/bool) values (ref/where.md):
     * `where `a`b`c!1 0 2` -> `a`c`c; a bool-valued dict (comparison result)
     * gives the keys where true.  Key-indexing shape keys[where vals], NOT
     * value distribution. */
    if (x && x->type == RAY_DICT && !q_is_keyed_table(x)) {
        ray_t* keys = ray_dict_keys(x);            /* borrowed */
        if (!keys) return ray_error("type", "where: malformed dictionary");
        int vo = 0;
        ray_t* vv = q_dict_vals_vec(x, &vo);
        if (!vv) return ray_error("type", "where: malformed dictionary");
        ray_t* w = q_where_wrap(vv);               /* bool mask or int counts */
        if (vo) ray_release(vv);
        if (!w || RAY_IS_ERR(w)) return w;
        ray_t* r = ray_at_fn(keys, w);
        ray_release(w);
        if (r && r->type == RAY_LIST) { ray_t* c = q_typed_empty_like(q_collapse_list(r), keys); ray_release(r); return c; }
        return r;
    }
    if (x && (x->type == RAY_I64 || x->type == RAY_I32 || x->type == RAY_I16)) {
        int64_t n = ray_len(x);
        int64_t total = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t c = (x->type == RAY_I64) ? ((const int64_t*)ray_data(x))[i]
                      : (x->type == RAY_I32) ? (int64_t)((const int32_t*)ray_data(x))[i]
                      : (int64_t)((const int16_t*)ray_data(x))[i];
            if (c < 0) return ray_error("domain", "where: negative count");
            total += c;
        }
        ray_t* out = ray_vec_new(RAY_I64, total);
        if (RAY_IS_ERR(out)) return out;
        out->len = total;
        int64_t* d = (int64_t*)ray_data(out);
        int64_t w = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t c = (x->type == RAY_I64) ? ((const int64_t*)ray_data(x))[i]
                      : (x->type == RAY_I32) ? (int64_t)((const int32_t*)ray_data(x))[i]
                      : (int64_t)((const int16_t*)ray_data(x))[i];
            for (int64_t k = 0; k < c; k++) d[w++] = i;
        }
        return out;
    }
    return ray_where_fn(x);
}

/* q `reverse x` / monadic `|` — a dict reverses ENTRIES (keys and values
 * together, ref/reverse.md: "on dictionaries, reverses the keys"); everything
 * else delegates to base reverse unchanged. */
ray_t* q_reverse_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT && !q_is_keyed_table(x)) {
        ray_t* k = ray_dict_keys(x);                 /* borrowed */
        ray_t* v = ray_dict_vals(x);                 /* borrowed */
        if (!k || !v) return ray_error("type", "reverse: malformed dictionary");
        ray_t* rk = ray_reverse_fn(k);
        if (rk && ray_is_lazy(rk)) rk = ray_lazy_materialize(rk);   /* dict slots must be concrete */
        if (!rk || RAY_IS_ERR(rk)) return rk;
        ray_t* rv = ray_reverse_fn(v);
        if (rv && ray_is_lazy(rv)) rv = ray_lazy_materialize(rv);
        if (!rv || RAY_IS_ERR(rv)) { ray_release(rk); return rv; }
        return ray_dict_new(rk, rv);                 /* consumes both */
    }
    return ray_reverse_fn(x);
}

/* ===== q `vs` / `sv` — split-join / base-encode family ===================
 * kdb reference vs.md / sv.md.  Both are strictly dyadic.  Native -RAY_STR
 * strings (split -> boxed list of string atoms, join -> one string atom),
 * symbol split/join, integer base decompose/compose (atom + vector base),
 * and big-endian byte/bit encode/decode.  Genuinely out-of-scope forms
 * (128-bit GUID compose, `1:` reparse, byte-vector base) return 'nyi. */

int q_is_null_sym(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return 0;
    ray_t* s = ray_sym_str(x->i64);
    int z = s && ray_str_len(s) == 0;
    return z;
}

/* split string y on substring sep -> boxed list of -RAY_STR (keeps empties) */
static ray_t* q_str_split(const char* y, size_t yl, const char* sep, size_t sl) {
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    if (sl == 0) {                                 /* empty sep -> one piece */
        ray_t* s = ray_str(y, yl);
        out = ray_list_append(out, s); ray_release(s);
        return out;
    }
    size_t seg = 0;
    for (size_t i = 0; i + sl <= yl; ) {
        if (memcmp(y + i, sep, sl) == 0) {
            ray_t* s = ray_str(y + seg, i - seg);
            out = ray_list_append(out, s); ray_release(s);
            if (RAY_IS_ERR(out)) return out;
            i += sl; seg = i;
        } else i++;
    }
    ray_t* last = ray_str(y + seg, yl - seg);
    out = ray_list_append(out, last); ray_release(last);
    return out;
}

/* newline / host-line-separator split: split on '\n', strip a trailing '\r'
 * from each line, drop a single trailing empty line (kdb ` vs read-lines). */
ray_t* q_str_split_lines(const char* y, size_t yl) {
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    size_t seg = 0;
    for (size_t i = 0; i <= yl; i++) {
        if (i == yl || y[i] == '\n') {
            size_t end = i;
            if (end > seg && y[end - 1] == '\r') end--;   /* strip CR */
            ray_t* s = ray_str(y + seg, end - seg);
            out = ray_list_append(out, s); ray_release(s);
            if (RAY_IS_ERR(out)) return out;
            seg = i + 1;
            if (i == yl) break;
        }
    }
    /* drop a single trailing empty produced by a terminal '\n' */
    int64_t n = ray_len(out);
    if (n >= 1) {
        ray_t** e = (ray_t**)ray_data(out);
        if (e[n - 1]->type == -RAY_STR && ray_str_len(e[n - 1]) == 0) {
            ray_release(e[n - 1]);
            out->len = n - 1;
        }
    }
    return out;
}

/* ` vs `sym — split a symbol: leading ':' (file handle) splits at the LAST
 * '/' into (dir; file); otherwise split on every '.'.  -> RAY_SYM vector. */
static ray_t* q_sym_split(ray_t* y) {
    ray_t* s = ray_sym_str(y->i64);
    if (!s) return ray_error("type", "vs: bad symbol");
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, 4);
    if (n > 0 && p[0] == ':') {                    /* file handle: last '/' */
        size_t cut = n;
        for (size_t i = n; i-- > 0; ) if (p[i] == '/') { cut = i; break; }
        if (cut == n) {                            /* no '/', single element */
            int64_t id = ray_sym_intern_runtime(p, n);
            out = ray_vec_append(out, &id);
        } else {
            int64_t a = ray_sym_intern_runtime(p, cut);
            int64_t b = ray_sym_intern_runtime(p + cut + 1, n - cut - 1);
            out = ray_vec_append(out, &a);
            out = ray_vec_append(out, &b);
        }
    } else {                                        /* split all '.' */
        size_t seg = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i == n || p[i] == '.') {
                int64_t id = ray_sym_intern_runtime(p + seg, i - seg);
                out = ray_vec_append(out, &id);
                seg = i + 1;
            }
        }
    }
    return out;
}

/* big-endian byte encode of a numeric scalar (0x0 vs y) -> U8 vector */
static ray_t* q_byte_encode(ray_t* y) {
    uint8_t b[8]; int w = 0; uint64_t bits = 0;
    switch (y->type) {
    case -RAY_I16: w = 2; bits = (uint16_t)y->i16; break;
    case -RAY_I32: w = 4; bits = (uint32_t)y->i32; break;
    case -RAY_I64: w = 8; bits = (uint64_t)y->i64; break;
    case -RAY_F32: { float f = (float)y->f64; uint32_t u; memcpy(&u, &f, 4);
                     w = 4; bits = u; break; }
    case -RAY_F64: { double d = y->f64; uint64_t u; memcpy(&u, &d, 8);
                     w = 8; bits = u; break; }
    default: return ray_error("type", "vs: unsupported byte-encode operand");
    }
    for (int i = 0; i < w; i++) b[i] = (uint8_t)(bits >> (8 * (w - 1 - i)));
    return ray_vec_from_raw(RAY_U8, b, w);
}

/* big-endian bit decompose of an integer scalar (0b vs y) -> BOOL vector */
static ray_t* q_bit_decompose(ray_t* y) {
    int w = 0; uint64_t bits = 0;
    switch (y->type) {
    case -RAY_BOOL: w = 1;  bits = y->b8 ? 1 : 0; break;
    case -RAY_U8:   w = 8;  bits = (uint8_t)y->u8; break;
    case -RAY_I16:  w = 16; bits = (uint16_t)y->i16; break;
    case -RAY_I32:  w = 32; bits = (uint32_t)y->i32; break;
    case -RAY_I64:  w = 64; bits = (uint64_t)y->i64; break;
    default: return ray_error("type", "vs: unsupported bit-decompose operand");
    }
    uint8_t stackb[64];
    for (int i = 0; i < w; i++) stackb[i] = (uint8_t)((bits >> (w - 1 - i)) & 1);
    return ray_vec_from_raw(RAY_BOOL, stackb, w);
}

/* decompose scalar v into minimal base-`base` digits (>=1) -> long vector */
static ray_t* q_base_decompose_atom(int64_t base, int64_t v) {
    if (base <= 0) return ray_error("domain", "vs: base must be positive");
    int64_t buf[64]; int n = 0;
    uint64_t u = (uint64_t)v;
    if (u == 0) buf[n++] = 0;
    while (u > 0 && n < 64) { buf[n++] = (int64_t)(u % (uint64_t)base); u /= (uint64_t)base; }
    ray_t* out = ray_vec_new(RAY_I64, n);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    for (int i = 0; i < n; i++) d[i] = buf[n - 1 - i];   /* MSB first */
    return out;
}

/* mixed-radix decompose scalar v by vector base -> long vector len(base) */
static ray_t* q_base_decompose_vec(ray_t* base, int64_t v) {
    int64_t n = ray_len(base);
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    uint64_t u = (uint64_t)v;
    for (int64_t i = n - 1; i >= 0; i--) {
        int64_t bi = q_ivec_get(base, i);
        if (bi <= 0) { d[i] = (int64_t)u; u = 0; }
        else { d[i] = (int64_t)(u % (uint64_t)bi); u /= (uint64_t)bi; }
    }
    return out;
}

ray_t* q_vs_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "vs: nil operand");
    /* --- string / newline split --- */
    if (x->type == -RAY_STR) {
        if (y->type != -RAY_STR)
            return ray_error("nyi", "vs: string split needs a string rhs (byte-string deferred)");
        return q_str_split(ray_str_ptr(y), ray_str_len(y),
                           ray_str_ptr(x), ray_str_len(x));
    }
    if (q_is_null_sym(x)) {
        if (y->type == -RAY_STR)
            return q_str_split_lines(ray_str_ptr(y), ray_str_len(y));
        if (y->type == -RAY_SYM) return q_sym_split(y);
        return ray_error("type", "vs: ` split expects a string or symbol");
    }
    /* --- byte encode (0x0 vs scalar) --- */
    if (x->type == -RAY_U8) {
        if (ray_is_atom(y) && y->type != -RAY_STR) return q_byte_encode(y);
        return ray_error("nyi", "vs: byte-vector base decompose deferred");
    }
    /* --- bit decompose (0b vs scalar) --- */
    if (x->type == -RAY_BOOL) {
        if (ray_is_atom(y)) return q_bit_decompose(y);
        return ray_error("type", "vs: 0b decompose expects a scalar");
    }
    /* --- integer base decompose --- */
    if (q_is_int_atom(x)) {
        int64_t base = q_iatom_val(x);
        if (q_is_int_atom(y)) return q_base_decompose_atom(base, q_iatom_val(y));
        if (q_is_int_vec(y)) {                     /* matrix: pad to max width */
            int64_t m = ray_len(y);
            ray_t* cols = ray_list_new(m > 0 ? m : 1);
            int64_t maxw = 1;
            for (int64_t j = 0; j < m; j++) {
                ray_t* c = q_base_decompose_atom(base, q_ivec_get(y, j));
                if (RAY_IS_ERR(c)) { ray_release(cols); return c; }
                if (ray_len(c) > maxw) maxw = ray_len(c);
                cols = ray_list_append(cols, c); ray_release(c);
            }
            ray_t* rows = ray_list_new(maxw);
            ray_t** cv = (ray_t**)ray_data(cols);
            for (int64_t r = 0; r < maxw; r++) {
                ray_t* row = ray_vec_new(RAY_I64, m); row->len = m;
                int64_t* rd = (int64_t*)ray_data(row);
                for (int64_t j = 0; j < m; j++) {
                    int64_t cw = ray_len(cv[j]);
                    int64_t pad = maxw - cw;         /* left-pad with 0 */
                    rd[j] = (r < pad) ? 0 : ((const int64_t*)ray_data(cv[j]))[r - pad];
                }
                rows = ray_list_append(rows, row); ray_release(row);
            }
            ray_release(cols);
            return rows;
        }
        return ray_error("type", "vs: integer decompose expects an integer rhs");
    }
    if (q_is_int_vec(x)) {
        if (q_is_int_atom(y)) return q_base_decompose_vec(x, q_iatom_val(y));
        return ray_error("nyi", "vs: vector-base matrix decompose deferred");
    }
    return ray_error("type", "vs: unsupported operand types");
}

/* join a boxed list / vector of strings with separator sep (append trailing
 * when host==1, the ` sv newline form). */
static ray_t* q_str_join(ray_t* y, const char* sep, size_t sl, int host) {
    if (!y || y->type != RAY_LIST)
        return ray_error("type", "sv: join expects a list of strings");
    int64_t n = ray_len(y);
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = (y->type == RAY_LIST) ? ((ray_t**)ray_data(y))[i] : NULL;
        if (!e || e->type != -RAY_STR)
            return ray_error("type", "sv: join expects string elements");
        total += ray_str_len(e);
        if (i + 1 < n) total += sl;
    }
    if (host) total += 1;
    char* buf = malloc(total ? total : 1);
    if (!buf) return ray_error("wsfull", "sv: out of memory");
    size_t w = 0;
    ray_t** ev = (ray_t**)ray_data(y);
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = ev[i];
        size_t el = ray_str_len(e);
        memcpy(buf + w, ray_str_ptr(e), el); w += el;
        if (host) { buf[w++] = '\n'; }
        else if (i + 1 < n) { memcpy(buf + w, sep, sl); w += sl; }
    }
    ray_t* r = ray_str(buf, w);
    free(buf);
    return r;
}

/* ` sv `syms — join symbols: leading ':' (file handle) joins with '/', else
 * with '.'  -> single -RAY_SYM atom. */
static ray_t* q_sym_join(ray_t* y) {
    int64_t n = ray_len(y);
    if (n == 0) return ray_sym(ray_sym_intern_runtime("", 0));
    ray_t* first = ray_sym_vec_cell(y, 0);
    const char* fp = first ? ray_str_ptr(first) : "";
    char joiner = (ray_str_len(first) > 0 && fp[0] == ':') ? '/' : '.';
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* c = ray_sym_vec_cell(y, i);
        total += ray_str_len(c);
        if (i + 1 < n) total += 1;
    }
    char* buf = malloc(total ? total : 1);
    if (!buf) return ray_error("wsfull", "sv: out of memory");
    size_t w = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* c = ray_sym_vec_cell(y, i);
        size_t cl = ray_str_len(c);
        memcpy(buf + w, ray_str_ptr(c), cl); w += cl;
        if (i + 1 < n) buf[w++] = joiner;
    }
    int64_t id = ray_sym_intern_runtime(buf, w);
    free(buf);
    return ray_sym(id);
}

/* big-endian byte decode: interpret a U8 vector as a signed integer of the
 * matching width (2->short, 4->int, 8->long). */
static ray_t* q_byte_decode(ray_t* y) {
    int64_t n = ray_len(y);
    const uint8_t* p = (const uint8_t*)ray_data(y);
    uint64_t v = 0;
    for (int64_t i = 0; i < n; i++) v = (v << 8) | p[i];
    if (n == 2) return ray_i16((int16_t)(uint16_t)v);
    if (n == 4) return ray_i32((int32_t)(uint32_t)v);
    if (n == 8) return ray_i64((int64_t)v);
    if (n == 1) return ray_i16((int16_t)(uint8_t)v);
    return ray_error("nyi", "sv: byte decode width %lld deferred", (long long)n);
}

/* bits -> integer (8->byte, 16->short, 32->int, 64->long; 128->guid deferred) */
static ray_t* q_bit_compose(ray_t* y) {
    int64_t n = ray_len(y);
    const uint8_t* p = (const uint8_t*)ray_data(y);
    if (n == 128) return ray_error("nyi", "sv: 128-bit GUID compose deferred");
    if (n != 8 && n != 16 && n != 32 && n != 64)
        return ray_error("nyi", "sv: bit compose width %lld deferred", (long long)n);
    uint64_t v = 0;
    for (int64_t i = 0; i < n; i++) v = (v << 1) | (p[i] & 1);
    if (n == 8)  return ray_u8((uint8_t)v);
    if (n == 16) return ray_i16((int16_t)(uint16_t)v);
    if (n == 32) return ray_i32((int32_t)(uint32_t)v);
    return ray_i64((int64_t)v);
}

ray_t* q_sv_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "sv: nil operand");
    /* --- string join --- */
    if (x->type == -RAY_STR)
        return q_str_join(y, ray_str_ptr(x), ray_str_len(x), 0);
    if (q_is_null_sym(x)) {
        if (y->type == RAY_SYM) return q_sym_join(y);            /* sym join */
        return q_str_join(y, "\n", 1, 1);                        /* host lines */
    }
    /* --- byte decode (0x0 sv bytes) --- */
    if (x->type == -RAY_U8) {
        if (y->type == RAY_U8) return q_byte_decode(y);
        return ray_error("nyi", "sv: byte-vector base compose deferred");
    }
    /* --- bit compose (0b sv bits) --- */
    if (x->type == -RAY_BOOL) {
        if (y->type == RAY_BOOL) return q_bit_compose(y);
        return ray_error("type", "sv: 0b compose expects a bool vector");
    }
    /* --- integer base compose (Horner) --- */
    if (q_is_int_atom(x)) {
        int64_t base = q_iatom_val(x);
        if (!q_is_int_vec(y) && y->type != RAY_BOOL)
            return ray_error("type", "sv: integer compose expects an integer vector");
        int64_t n = ray_len(y);
        int64_t acc = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t d = (y->type == RAY_BOOL) ? ((const uint8_t*)ray_data(y))[i]
                                              : q_ivec_get(y, i);
            acc = acc * base + d;
        }
        return ray_i64(acc);
    }
    /* --- mixed-radix compose (vector base) --- */
    if (q_is_int_vec(x)) {
        if (!q_is_int_vec(y)) return ray_error("type", "sv: mixed-radix expects an integer vector rhs");
        int64_t n = ray_len(y), bn = ray_len(x);
        if (n != bn) return ray_error("length", "sv: base and value lengths must match");
        int64_t acc = 0;
        for (int64_t i = 0; i < n; i++)
            acc = acc * q_ivec_get(x, i) + q_ivec_get(y, i);
        return ray_i64(acc);
    }
    return ray_error("type", "sv: unsupported operand types");
}
