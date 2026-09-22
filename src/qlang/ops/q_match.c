/* q_match.c — the single C home for the `~` verb (match).  Split from q_math.c
 * (2026-09-22): the glyph's monadic `not` stays there.  Entry points
 * q_match_rec (the C predicate) and q_match_wrap (the verb) are declared in q_prim.h. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_count.h"
#include "qlang/q_prim.h"      /* q_match_rec/_wrap, q_enum_domain, q_enum_val_image */
#include "qlang/base/q_err.h"
#include "lang/eval.h"     /* ray_cmp_tol_eq, ray_at_fn */
#include "qlang/eval/q_eval.h" /* carrier read-out: `~` decomposes function values */
#include <math.h>          /* isnan */
#include <string.h>        /* memcmp */

/* `~` on FUNCTION values (owner ruling 2026-07-31 — ref/match.md pins only
 * data).  A function's identity is its STRUCTURE, never its object address or
 * its bytes: a lambda IS its source plus its defining context, a derived
 * function its iterator plus its operand, a projection its function plus its
 * bound arguments, a composition its parts, an iterator its adverb id.
 * Native fn values are registry snapshots (one per (row,valence)), so the
 * pointer test at the head of q_match_rec already decides those. */
static int match_carrier(ray_t* a, ray_t* b) {
    q_car_kind_t kind = q_eval_apply_carrier_kind(a);
    if (kind != q_eval_apply_carrier_kind(b)) return 0;
    switch (kind) {
    case Q_EVAL_CAR_LAMBDA: {
        ray_t *pa = NULL, *ba = NULL, *ca = NULL, *pb = NULL, *bb = NULL, *cb = NULL;
        q_eval_apply_lambda_parts(a, &pa, &ba, &ca);
        q_eval_apply_lambda_parts(b, &pb, &bb, &cb);
        if (!q_match_rec(ca, cb)) return 0;
        ray_t* sa = q_eval_apply_lambda_src(a);
        ray_t* sb = q_eval_apply_lambda_src(b);
        /* a source-less carrier (built by serde, not parsed) falls back to
         * the parts the source would have produced */
        if (sa && sb) return q_match_rec(sa, sb);
        return q_match_rec(pa, pb) && q_match_rec(ba, bb);
    }
    case Q_EVAL_CAR_DERIV:
        return q_eval_apply_deriv_adv(a) == q_eval_apply_deriv_adv(b) &&
               q_match_rec(q_eval_apply_car_head(a), q_eval_apply_car_head(b));
    case Q_EVAL_CAR_PROJ: {
        int64_t n = q_eval_apply_proj_nslots(a);
        if (n != q_eval_apply_proj_nslots(b)) return 0;
        if (!q_match_rec(q_eval_apply_car_head(a), q_eval_apply_car_head(b)))
            return 0;
        for (int64_t i = 0; i < n; i++)      /* a hole is C NULL, and NULL~NULL */
            if (!q_match_rec(q_eval_apply_proj_arg(a, i),
                             q_eval_apply_proj_arg(b, i)))
                return 0;
        return 1;
    }
    case Q_EVAL_CAR_COMP:
        return q_match_rec(q_eval_apply_car_head(a), q_eval_apply_car_head(b)) &&
               q_match_rec(q_eval_apply_comp_inner(a), q_eval_apply_comp_inner(b));
    case Q_EVAL_CAR_ITER:
        return q_eval_apply_iter_id(a) == q_eval_apply_iter_id(b);
    case Q_EVAL_CAR_KFN: {
        void *fa = NULL, *fb = NULL;
        int64_t ra = 0, rb = 0;
        q_eval_apply_kfn_parts(a, &fa, &ra, NULL, NULL);
        q_eval_apply_kfn_parts(b, &fb, &rb, NULL, NULL);
        return fa == fb && ra == rb;
    }
    case Q_EVAL_CAR_NONE: case Q_EVAL_CAR_VIEW: break;
    }
    return 0;
}

/* ~'s float-leaf test: nulls of any type are equal, else the shared tolerance */
static int match_tol_f64(double x, double y) {
    return (isnan(x) && isnan(y)) || ray_cmp_tol_eq(x, y);
}

/* q `x~y` — recursive whole-value equivalence (kdb match): TYPE-strict
 * (`1~1f` is 0b), attribute-blind (`1 2 3~\`s#1 2 3` is 1b), sentinel nulls
 * compare equal (`0n~0n` is 1b — non-finites canonicalize to one payload).
 * Unhandled types conservatively mismatch (kdb ~ never errors). */
static int match_rec(ray_t* a, ray_t* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    /* one side enum, the other not: the enum compares as its VALUE IMAGE
     * (resolved values, or bare positions for a reference domain — the decay
     * law; the apply-level `~` already decays and the container walk must
     * agree, e.g. a read-back splay table against its plain source).  An
     * allocation error answers mismatch. */
    int ea = a->type == RAY_ENUM || a->type == -RAY_ENUM;
    int eb = b->type == RAY_ENUM || b->type == -RAY_ENUM;
    if (ea != eb) {
        ray_t* r = q_enum_val_image(ea ? a : b);
        int m = r && !RAY_IS_ERR(r) && q_match_rec(ea ? r : a, ea ? b : r);
        if (r && RAY_IS_ERR(r)) ray_error_free(r);
        else if (r) ray_release(r);
        return m;
    }
    if (a->type != b->type) return 0;
    if (a->type == -RAY_SYM) return a->i64 == b->i64;
    if (a->type == -RAY_STR)
        return ray_str_len(a) == ray_str_len(b) &&
               memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) == 0;
    if (a->type == -RAY_GUID) return memcmp(ray_guid_bytes(a), ray_guid_bytes(b), 16) == 0;
    if (a->type == RAY_ENUM || a->type == -RAY_ENUM) {
        /* 20h sits outside the ray_is_vec band, so the container walk lands
         * here (the apply-level `~` decays before the kernel; children of a
         * table/list do not).  Same domain: positions decide.  Different
         * domains: the resolved values decide (the decay law's answer). */
        if (q_enum_domain(a) == q_enum_domain(b)) {
            if (a->type == -RAY_ENUM) return a->i64 == b->i64;
            int64_t la = q_count(a);
            return la == q_count(b) &&
                   memcmp(ray_data(a), ray_data(b), (size_t)la * 8) == 0;
        }
        ray_t* ra = q_enum_val_image(a);
        ray_t* rb = q_enum_val_image(b);
        int r = ra && rb && !RAY_IS_ERR(ra) && !RAY_IS_ERR(rb) && q_match_rec(ra, rb);
        if (ra) { if (RAY_IS_ERR(ra)) ray_error_free(ra); else ray_release(ra); }
        if (rb) { if (RAY_IS_ERR(rb)) ray_error_free(rb); else ray_release(rb); }
        return r;
    }
    if (q_eval_apply_carrier_kind(a)) return match_carrier(a, b);
    if (ray_is_atom(a)) {
        /* inline-payload scalars ONLY (ray_is_atom also covers LAMBDA and
         * fn values, whose state is NOT in the union slot — those fall to
         * the conservative-mismatch tail below). */
        switch (-a->type) {
        case RAY_F32: case RAY_F64: RAY_TEMPORALF_CASES:
            /* ~ is tolerant on real/float/datetime (precision.md "Use"); payload
             * memcmp first so canonical nulls match without arithmetic */
            if (memcmp(&a->i64, &b->i64, 8) == 0) return 1;
            return match_tol_f64(a->f64, b->f64);
        case RAY_BOOL: RAY_BYTE_CASES: case RAY_I16: case RAY_I32: case RAY_I64:
        RAY_TEMPORAL32_CASES: RAY_TEMPORAL64_CASES:
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
        int64_t la = q_count(a);
        if (la != q_count(b)) return 0;
        /* same-type numeric vectors: payload memcmp (nulls are in-payload
         * sentinels; attrs deliberately not compared).  SYM vecs vary in
         * index width -> per-element below. */
        if (ray_is_vec(a) && a->type != RAY_SYM && a->type != RAY_STR) {
            size_t esz = (a->type == RAY_I64 || a->type == RAY_F64 ||
                          RAY_IS_TEMPORALF(a->type)) ? 8
                       : (a->type == RAY_I32 || a->type == RAY_F32) ? 4
                       : (a->type == RAY_I16) ? 2
                       : (a->type == RAY_BOOL || ray_is_bytelike(a->type)) ? 1 : 0;
            if (esz) {
                if (memcmp(ray_data(a), ray_data(b), (size_t)la * esz) == 0) return 1;
                /* float lanes fall to a tolerant element loop; exact lanes are decided */
                if (a->type == RAY_F64 || RAY_IS_TEMPORALF(a->type)) {
                    const double *xa = ray_data(a), *xb = ray_data(b);
                    for (int64_t i = 0; i < la; i++)
                        if (!match_tol_f64(xa[i], xb[i])) return 0;
                    return 1;
                }
                if (a->type == RAY_F32) {
                    const float *xa = ray_data(a), *xb = ray_data(b);
                    for (int64_t i = 0; i < la; i++)
                        if (!match_tol_f64((double)xa[i], (double)xb[i])) return 0;
                    return 1;
                }
                return 0;
            }
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

/* the walk is bounded where the value is not (IDX_MAX_DEPTH's ceiling, sized for the 8MB stack): the predicate
 * answers mismatch, its conservative side; the verb reports the trip as 'stack (basics/errors.md) */
#define MATCH_MAX_DEPTH 2048
static _Thread_local int match_depth, match_deep;

int q_match_rec(ray_t* a, ray_t* b) {
    if (match_depth >= MATCH_MAX_DEPTH) { match_deep = 1; return 0; }
    match_depth++;
    int r = match_rec(a, b);
    match_depth--;
    return r;
}

ray_t* q_match_wrap(ray_t* a, ray_t* b) {
    match_deep = 0;
    int r = q_match_rec(a, b);
    return match_deep ? q_err(QE_STACK) : ray_bool(r);
}
