/* ops/q_agg.c — running/weighted/covariance/moving-window aggregates (wave 5)
 * and the list-arm sum wrapper
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_err.h"
#include "lang/eval.h"     /* ray_sum_fn, ray_mul_fn — engine arms */
#include "lang/internal.h" /* atomic_map_unary/binary, make_f64, is_collection, ray_error */
#include <math.h>          /* isnan, sqrt — sentinel-null discipline, mdev/cov */
#include <stdlib.h>        /* malloc, free */

/* ===== Wave 5 — running / weighted / covariance aggregates ================
 * kdb ref/{sums,prds,maxs,mins,avgs,ratios,wsum,wavg,cov}.md.  Null discipline
 * per page: sum/sums treat null as 0, prd/prds as 1, avg/avgs EXCLUDE nulls,
 * max/min skip nulls (kdb shows -0W/0W for leading nulls — long ±infinity is
 * not representable in this engine's sentinel-null model, so those specific
 * rows are a documented lang-divergence). */

/* Read element i of a numeric vector as a double; *isnull set for the typed
 * null sentinel (int MIN / NaN). */
double q_velem_f(ray_t* x, int64_t i, int* isnull) {
    *isnull = 0;
    if (ray_is_atom(x)) {                 /* scalar (index ignored) */
        switch (x->type) {
        case -RAY_I64: if (x->i64==NULL_I64){*isnull=1;} return (double)x->i64;
        case -RAY_I32: if (x->i32==NULL_I32){*isnull=1;} return (double)x->i32;
        case -RAY_I16: if (x->i16==NULL_I16){*isnull=1;} return (double)x->i16;
        case -RAY_BOOL:return (double)x->b8;
        case -RAY_BYTE_ONLY: case -RAY_CHARV: return (double)x->u8;
        case -RAY_F64: case -RAY_F32: if (isnan(x->f64)){*isnull=1;} return x->f64;
        default: *isnull = 1; return 0;
        }
    }
    const void* d = ray_data(x);
    switch (x->type) {
    case RAY_I64: { int64_t v = ((const int64_t*)d)[i]; if (v==NULL_I64){*isnull=1;} return (double)v; }
    case RAY_I32: { int32_t v = ((const int32_t*)d)[i]; if (v==NULL_I32){*isnull=1;} return (double)v; }
    case RAY_I16: { int16_t v = ((const int16_t*)d)[i]; if (v==NULL_I16){*isnull=1;} return (double)v; }
    case RAY_BOOL:return (double)((const uint8_t*)d)[i];
    case RAY_BYTE_ONLY: case RAY_CHARV: return (double)((const uint8_t*)d)[i];
    case RAY_F64: { double v = ((const double*)d)[i]; if (isnan(v)){*isnull=1;} return v; }
    case RAY_F32: { float  v = ((const float*)d)[i];  if (isnan(v)){*isnull=1;} return (double)v; }
    default: *isnull = 1; return 0;
    }
}
int q_vec_is_float(ray_t* x) { return x->type == RAY_F64 || x->type == RAY_F32; }
int q_vec_is_num(ray_t* x) {
    return x && (x->type==RAY_I64||x->type==RAY_I32||x->type==RAY_I16||
                 x->type==RAY_BOOL||ray_is_bytelike(x->type)||x->type==RAY_F64||x->type==RAY_F32);
}

typedef enum { RS_SUMS, RS_PRDS, RS_MAXS, RS_MINS, RS_AVGS } q_rs_kind;

/* running max/min over the bytes of a q string (kdb `maxs "genie"`). */
static ray_t* runscan_str(ray_t* x, q_rs_kind k) {
    const char* p = ray_str_ptr(x);
    size_t n = ray_str_len(x);
    char stackb[256];
    char* b = (n <= sizeof stackb) ? stackb : malloc(n ? n : 1);
    if (!b) return q_err(QE_WSFULL);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (i == 0) b[i] = (char)c;
        else b[i] = (k==RS_MAXS) ? (char)((unsigned char)b[i-1] > c ? (unsigned char)b[i-1] : c)
                                 : (char)((unsigned char)b[i-1] < c ? (unsigned char)b[i-1] : c);
    }
    ray_t* r = ray_str(b, n);
    if (b != stackb) free(b);
    return r;
}

static ray_t* runscan(ray_t* x, q_rs_kind k) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == -RAY_STR) {
        if (k==RS_MAXS || k==RS_MINS) return runscan_str(x, k);
        return q_err(QE_TYPE);
    }
    if (ray_is_atom(x)) {                 /* atom returned unchanged (avgs->float) */
        if (k == RS_AVGS) { int nu; double v = q_velem_f(x, 0, &nu);
                            return nu ? ray_typed_null(-RAY_F64) : ray_f64(v); }
        ray_retain(x); return x;
    }
    if (!q_vec_is_num(x)) return q_err(QE_TYPE);
    int64_t n = ray_len(x);
    if (k == RS_AVGS) {
        ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
        double* o = (double*)ray_data(out);
        double s = 0; int64_t c = 0;
        for (int64_t i = 0; i < n; i++) {
            int nu; double v = q_velem_f(x, i, &nu);
            if (!nu) { s += v; c++; }
            if (c == 0) { o[i] = 0; ray_vec_set_null(out, i, true); } else o[i] = s / (double)c;
        }
        return out;
    }
    int isf = q_vec_is_float(x);
    if (isf || k == RS_AVGS) {
        ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
        double* o = (double*)ray_data(out);
        double acc = (k==RS_PRDS) ? 1 : 0; int started = 0; double m = 0;
        for (int64_t i = 0; i < n; i++) {
            int nu; double v = q_velem_f(x, i, &nu);
            if (k==RS_SUMS) { acc += nu?0:v; o[i]=acc; }
            else if (k==RS_PRDS) { acc *= nu?1:v; o[i]=acc; }
            else { if (!nu) { if (!started){m=v;started=1;} else if (k==RS_MAXS?v>m:v<m) m=v; }
                   if (started) o[i]=m; else { o[i]=0; ray_vec_set_null(out,i,true); } }
        }
        return out;
    }
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1); out->len = n;
    int64_t* o = (int64_t*)ray_data(out);
    int64_t acc = (k==RS_PRDS) ? 1 : 0; int started = 0; int64_t m = 0;
    for (int64_t i = 0; i < n; i++) {
        int nu; double vd = q_velem_f(x, i, &nu); int64_t v = (int64_t)vd;
        if (k==RS_SUMS) { acc += nu?0:v; o[i]=acc; }
        else if (k==RS_PRDS) { acc *= nu?1:v; o[i]=acc; }
        else { if (!nu) { if (!started){m=v;started=1;} else if (k==RS_MAXS?v>m:v<m) m=v; }
               if (started) o[i]=m; else { o[i]=NULL_I64; ray_vec_set_null(out,i,true); } }
    }
    return out;
}
ray_t* q_sums_wrap(ray_t* x){ return runscan(x, RS_SUMS); }
ray_t* q_prds_wrap(ray_t* x){ return runscan(x, RS_PRDS); }
ray_t* q_maxs_wrap(ray_t* x){ return runscan(x, RS_MAXS); }
ray_t* q_mins_wrap(ray_t* x){ return runscan(x, RS_MINS); }
ray_t* q_avgs_wrap(ray_t* x){ return runscan(x, RS_AVGS); }

/* q `ratios x` — pairwise ratio: r[0]=x[0], r[i]=x[i] % x[i-1] (float). */
ray_t* q_ratios_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (!q_vec_is_num(x)) return q_err(QE_TYPE);
    int64_t n = ray_len(x);
    ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
    double* o = (double*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        if (i == 0) {                       /* r[0] = x[0] (null stays null) */
            if (nu) { o[i] = 0; ray_vec_set_null(out, i, true); } else o[i] = v;
            continue;
        }
        int pnu; double pv = q_velem_f(x, i-1, &pnu);
        /* r[i] = x[i] % x[i-1] with q float-divide semantics: a null operand or
         * a zero divisor yields null (the engine's `%` canonicalizes ±inf/NaN
         * to 0n), not a fabricated value. */
        if (nu || pnu || pv == 0) { o[i] = 0; ray_vec_set_null(out, i, true); }
        else o[i] = v / pv;
    }
    return out;
}

ray_t* q_fill_wrap(ray_t* x, ray_t* y);   /* fwd — prd's nulls-as-1 fill */

/* q `prd x` — product aggregate, the multiply-over fold twin of prds (ref/prd.md):
 * an atom is returned unchanged; a numeric vector folds to its product with
 * NULLS TREATED AS 1s (`prd 2 3 0N 7` -> 42); a BOOL vector returns an int
 * (`prd 101b` -> 0i); a list of lists multiplies element-wise (`prd (1 2 3 4;
 * 2 3 5 7)` -> 2 6 15 28 — the fold over the registered atomic multiply).
 * Non-numeric -> 'type. */
ray_t* q_prd_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == -RAY_STR || x->type == RAY_SYM)
        return q_err(QE_TYPE);
    if (ray_is_atom(x)) { ray_retain(x); return x; }   /* doc: atom unchanged */
    if (x->type == RAY_LIST) {                         /* element-wise fold */
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (n == 0) return ray_i64(1);                 /* empty product (derived) */
        /* Nulls are 1s here too (ref/prd.md's unconditional rule — codex r2:
         * `prd (1 0N;2 3)` must be 2 3, not 2 0N), so every operand is
         * null-filled with 1 (q `1^`) before it enters the multiply. */
        ray_t* one = ray_i64(1);
        ray_t* acc = q_fill_wrap(one, e[0]);
        if (!acc || RAY_IS_ERR(acc)) { ray_release(one);
                                       return acc ? acc : q_err(QE_TYPE); }
        for (int64_t i = 1; i < n; i++) {
            ray_t* fi = q_fill_wrap(one, e[i]);
            if (!fi || RAY_IS_ERR(fi)) { ray_release(acc); ray_release(one);
                                         return fi ? fi : q_err(QE_TYPE); }
            /* ray_mul_fn is the ATOM kernel; atomic_map_binary is eval's
             * broadcast (vector*vector, atom*vector, nested) around it. */
            ray_t* nx = atomic_map_binary(ray_mul_fn, acc, fi);
            ray_release(fi);
            ray_release(acc);
            if (!nx || RAY_IS_ERR(nx)) { ray_release(one);
                                         return nx ? nx : q_err(QE_TYPE); }
            acc = nx;
        }
        ray_release(one);
        return acc;
    }
    if (!q_vec_is_num(x))
        return q_err(QE_TYPE);
    int64_t n = ray_len(x);
    if (q_vec_is_float(x)) {
        double acc = 1;
        for (int64_t i = 0; i < n; i++) {
            int nu; double v = q_velem_f(x, i, &nu);
            if (!nu) acc *= v;                         /* nulls are 1s */
        }
        return make_f64(acc);
    }
    int64_t acc = 1;
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        if (!nu) acc *= (int64_t)v;
    }
    if (x->type == RAY_BOOL) return ray_i32((int32_t)acc);   /* prd 101b -> 0i */
    return ray_i64(acc);
}

/* q `x wavg y` — weighted average (sum x*y) % sum x, pairs where EITHER side
 * is null excluded (kdb); an atom x broadcasts.  Kept in C: the q.q
 * composition's bool-multiply denominator measured 16x slower (2026-07-15).
 * wsum/cov/scov are q.q-hosted (their doc formulas ride engine kernels). */
ray_t* q_wavg_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    int xatom = ray_is_atom(x);
    if (!q_vec_is_num(y) && !ray_is_atom(y)) return q_err(QE_TYPE);
    if (!xatom && !q_vec_is_num(x)) return q_err(QE_TYPE);
    int64_t n = ray_is_atom(y) ? (xatom ? 1 : ray_len(x)) : ray_len(y);
    if (!xatom && !ray_is_atom(y) && ray_len(x) != ray_len(y))
        return q_err(QE_LENGTH);
    double sp = 0, sw = 0;
    for (int64_t i = 0; i < n; i++) {
        int xn, yn;
        double xv = xatom ? q_velem_f(x, 0, &xn) : q_velem_f(x, i, &xn);
        double yv = ray_is_atom(y) ? q_velem_f(y, 0, &yn) : q_velem_f(y, i, &yn);
        if (xn || yn) continue;                 /* exclude null pairs */
        sp += xv * yv; sw += xv;
    }
    if (sw == 0) return ray_typed_null(-RAY_F64);
    return ray_f64(sp / sw);
}

/* q sliding m-window family `N mf x` — window i covers x[max(0,i-N+1)..i].
 * msum treats null as 0; max/min/dev/count exclude nulls; N<=0 -> empty
 * window (sum/count 0, others null).  mavg is q.q-hosted (msum%mcount). */
typedef enum { MW_SUM, MW_MAX, MW_MIN, MW_COUNT, MW_DEV } q_mw_kind;

static ray_t* mwin(ray_t* nx, ray_t* x, q_mw_kind k) {
    int64_t N;
    ray_t* err = q_i64_or_err(nx, &N, "m-window: n");
    if (err) return err;
    if (!x || !q_vec_is_num(x)) {
        if (x && ray_is_atom(x)) { ray_retain(x); return x; }
        return q_err(QE_TYPE);
    }
    int64_t n = ray_len(x);
    int isf = q_vec_is_float(x);
    int8_t otype = (k==MW_SUM || k==MW_MAX || k==MW_MIN) ? (isf ? RAY_F64 : RAY_I64)
                 : (k==MW_COUNT) ? RAY_I64 : RAY_F64;
    ray_t* out = ray_vec_new(otype, n > 0 ? n : 1); out->len = n;
    void* o = ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int64_t lo = (N > 0 && i - N + 1 > 0) ? i - N + 1 : 0;
        if (N <= 0) lo = i + 1;                  /* empty window */
        double sum=0, sumsq=0, m=0; int64_t c=0; int started=0;
        for (int64_t j = lo; j <= i; j++) {
            int nu; double v = q_velem_f(x, j, &nu);
            if (k==MW_SUM) { if (!nu) sum += v; continue; }
            if (nu) continue;
            c++; sum += v; sumsq += v*v;
            if (!started) { m=v; started=1; }
            else if (k==MW_MAX ? v>m : v<m) m=v;
        }
        if (otype == RAY_I64) {
            if (k==MW_SUM)   ((int64_t*)o)[i] = (int64_t)sum;
            else if (k==MW_COUNT) ((int64_t*)o)[i] = c;
            else { if (started) ((int64_t*)o)[i] = (int64_t)m;   /* mmax/mmin */
                   else { ((int64_t*)o)[i] = NULL_I64; ray_vec_set_null(out, i, true); } }
        } else {
            double r; int isnull = 0;
            switch (k) {
            case MW_SUM: r = sum; break;
            case MW_MAX: case MW_MIN: if (started) r=m; else { r=0; isnull=1; } break;
            case MW_DEV: if (c) { double mean=sum/(double)c; double var=sumsq/(double)c - mean*mean;
                                  r = var>0 ? sqrt(var) : 0; } else { r=0; isnull=1; } break;
            default: r = 0; break;
            }
            ((double*)o)[i] = r;
            if (isnull) ray_vec_set_null(out, i, true);
        }
    }
    return out;
}
ray_t* q_msum_wrap(ray_t* n, ray_t* x){ return mwin(n, x, MW_SUM); }
ray_t* q_mmax_wrap(ray_t* n, ray_t* x){ return mwin(n, x, MW_MAX); }
ray_t* q_mmin_wrap(ray_t* n, ray_t* x){ return mwin(n, x, MW_MIN); }
ray_t* q_mcount_wrap(ray_t* n, ray_t* x){ return mwin(n, x, MW_COUNT); }
ray_t* q_mdev_wrap(ray_t* n, ray_t* x){ return mwin(n, x, MW_DEV); }

/* q `a ema x` — exponential moving average: e[0]=x[0], e[i]=a*x[i]+(1-a)*e[i-1].
 * `a` is a float atom (the smoothing factor). */
ray_t* q_ema_wrap(ray_t* a, ray_t* x) {
    if (!a || !ray_is_atom(a)) return q_err(QE_TYPE);
    int an; double alpha = q_velem_f(a, 0, &an);
    if (!x || !q_vec_is_num(x)) {
        if (x && ray_is_atom(x)) { ray_retain(x); return x; }
        return q_err(QE_TYPE);
    }
    int64_t n = ray_len(x);
    ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
    double* o = (double*)ray_data(out);
    double e = 0; int started = 0;
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        double xv = nu ? 0 : v;
        if (!started) { e = xv; started = 1; }
        else e = alpha * xv + (1.0 - alpha) * e;
        o[i] = e;
    }
    return out;
}

/* q `sum x` — LIST arm sums the items (kdb: `sum(2013.03.15;18:55:40.686)`
 * is a timestamp; Load Fixed pins `sum("DT";8 9)0:enlist"…"`).  Non-lists
 * keep the base vector aggregate. */
ray_t* q_sum_wrap(ray_t* x) {
    if (x && x->type == RAY_LIST && ray_len(x) > 0) {
        /* fold q `+` over the items via call_fn2 (the ATOMIC dispatch —
         * ray_add_fn alone is the atom kernel; vectors broadcast in eval). */
        ray_t* plus = q_registry_lookup_name("+", 1, Q_DYADIC);   /* borrowed */
        if (!plus) return q_err(QE_TYPE);
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* acc = e[0];
        ray_retain(acc);
        for (int64_t i = 1; i < ray_len(x); i++) {
            ray_t* nx = call_fn2(plus, acc, e[i]);
            ray_release(acc);
            if (!nx || RAY_IS_ERR(nx)) return nx ? nx : q_err(QE_OOM);
            acc = nx;
        }
        return acc;
    }
    return ray_sum_fn(x);
}
