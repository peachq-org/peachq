/* q_wrap_agg.c — running/weighted/covariance/moving-window aggregates (wave 5),
 * neg/raze/enlist/null/within, and the list-arm sum wrapper
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "lang/eval.h"     /* ray_sum_fn, ray_mul_fn, ray_neg_fn, ray_enlist_fn — engine arms */
#include "lang/internal.h" /* atomic_map_unary/binary, make_f64, is_collection, ray_error */
#include "lang/format.h"   /* ray_type_name — error messages */
#include "table/sym.h"     /* RAY_SYM_W64 */
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
        case -RAY_BYTE_ONLY: return (double)x->u8;
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
    case RAY_BYTE_ONLY: return (double)((const uint8_t*)d)[i];
    case RAY_F64: { double v = ((const double*)d)[i]; if (isnan(v)){*isnull=1;} return v; }
    case RAY_F32: { float  v = ((const float*)d)[i];  if (isnan(v)){*isnull=1;} return (double)v; }
    default: *isnull = 1; return 0;
    }
}
int q_vec_is_float(ray_t* x) { return x->type == RAY_F64 || x->type == RAY_F32; }
int q_vec_is_num(ray_t* x) {
    return x && (x->type==RAY_I64||x->type==RAY_I32||x->type==RAY_I16||
                 x->type==RAY_BOOL||x->type==RAY_BYTE_ONLY||x->type==RAY_F64||x->type==RAY_F32);
}

typedef enum { RS_SUMS, RS_PRDS, RS_MAXS, RS_MINS, RS_AVGS } q_rs_kind;

/* running max/min over the bytes of a q string (kdb `maxs "genie"`). */
static ray_t* q_runscan_str(ray_t* x, q_rs_kind k) {
    const char* p = ray_str_ptr(x);
    size_t n = ray_str_len(x);
    char stackb[256];
    char* b = (n <= sizeof stackb) ? stackb : malloc(n ? n : 1);
    if (!b) return ray_error("wsfull", "maxs: out of memory");
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

static ray_t* q_runscan(ray_t* x, q_rs_kind k) {
    if (!x) return ray_error("type", "running scan: nil");
    if (x->type == -RAY_STR) {
        if (k==RS_MAXS || k==RS_MINS) return q_runscan_str(x, k);
        return ray_error("type", "running scan: non-numeric");
    }
    if (x->type == RAY_CHARV) {                  /* char vector rides the STR body */
        if (k==RS_MAXS || k==RS_MINS) {
            ray_t* s = q_str_of_charv(x);
            if (!s || RAY_IS_ERR(s)) return s ? s : ray_error("oom", NULL);
            ray_t* r = q_runscan_str(s, k);
            ray_release(s);
            return q_charv_out(r);
        }
        return ray_error("type", "running scan: non-numeric");
    }
    if (ray_is_atom(x)) {                 /* atom returned unchanged (avgs->float) */
        if (k == RS_AVGS) { int nu; double v = q_velem_f(x, 0, &nu);
                            return nu ? ray_typed_null(-RAY_F64) : ray_f64(v); }
        ray_retain(x); return x;
    }
    if (!q_vec_is_num(x)) return ray_error("type", "running scan: non-numeric list");
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
ray_t* q_sums_wrap(ray_t* x){ return q_runscan(x, RS_SUMS); }
ray_t* q_prds_wrap(ray_t* x){ return q_runscan(x, RS_PRDS); }
ray_t* q_maxs_wrap(ray_t* x){ return q_runscan(x, RS_MAXS); }
ray_t* q_mins_wrap(ray_t* x){ return q_runscan(x, RS_MINS); }
ray_t* q_avgs_wrap(ray_t* x){ return q_runscan(x, RS_AVGS); }

/* q `ratios x` — pairwise ratio: r[0]=x[0], r[i]=x[i] % x[i-1] (float). */
ray_t* q_ratios_wrap(ray_t* x) {
    if (!x) return ray_error("type", "ratios: nil");
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (!q_vec_is_num(x)) return ray_error("type", "ratios: non-numeric list");
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
 * 2 3 5 7)` -> 2 6 15 28 — the fold over the registered atomic multiply);
 * a dict folds over its value list (`prd d` -> 40 105 18); a table returns a
 * per-column dict (`prd t` -> a| 630 …), a keyed table folds its VALUE table
 * (`prd k` — implicit-iteration section).  Non-numeric -> 'type. */
ray_t* q_prd_wrap(ray_t* x) {
    if (!x) return ray_error("type", "prd: nil");
    if (x->type == -RAY_STR || x->type == RAY_SYM || x->type == RAY_CHARV)
        return ray_error("type", "prd: expects numeric values, got %s",
                         ray_type_name(x->type));
    if (ray_is_atom(x)) { ray_retain(x); return x; }   /* doc: atom unchanged */
    if (q_is_keyed_table(x))                           /* prd k == prd value t */
        return q_prd_wrap(ray_dict_vals(x));           /* vals borrowed — fine */
    if (x->type == RAY_TABLE) {                        /* per-column dict */
        int64_t nc = ray_table_ncols(x);
        ray_t* k = ray_sym_vec_new(RAY_SYM_W64, nc > 0 ? nc : 1);
        if (!k || RAY_IS_ERR(k)) return k ? k : ray_error("oom", NULL);
        ray_t* v = ray_list_new(nc > 0 ? nc : 1);
        if (RAY_IS_ERR(v)) { ray_release(k); return v; }
        for (int64_t c = 0; c < nc; c++) {
            int64_t nm = ray_table_col_name(x, c);
            k = ray_vec_append(k, &nm);
            if (!k || RAY_IS_ERR(k)) { ray_release(v); return k ? k : ray_error("oom", NULL); }
            ray_t* p = q_prd_wrap(ray_table_get_col_idx(x, c));
            if (!p || RAY_IS_ERR(p)) { ray_release(k); ray_release(v);
                                       return p ? p : ray_error("type", NULL); }
            v = ray_list_append(v, p);                 /* retains */
            ray_release(p);
            if (RAY_IS_ERR(v)) { ray_release(k); return v; }
        }
        ray_t* cv = q_collapse_list(v);                /* owned */
        ray_release(v);
        if (!cv || RAY_IS_ERR(cv)) { ray_release(k); return cv; }
        return ray_dict_new(k, cv);                    /* consumes both */
    }
    if (x->type == RAY_DICT)                           /* fold the value list */
        return q_prd_wrap(ray_dict_vals(x));           /* vals borrowed — fine */
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
                                       return acc ? acc : ray_error("type", NULL); }
        for (int64_t i = 1; i < n; i++) {
            ray_t* fi = q_fill_wrap(one, e[i]);
            if (!fi || RAY_IS_ERR(fi)) { ray_release(acc); ray_release(one);
                                         return fi ? fi : ray_error("type", NULL); }
            /* ray_mul_fn is the ATOM kernel; atomic_map_binary is eval's
             * broadcast (vector*vector, atom*vector, nested) around it. */
            ray_t* nx = atomic_map_binary(ray_mul_fn, acc, fi);
            ray_release(fi);
            ray_release(acc);
            if (!nx || RAY_IS_ERR(nx)) { ray_release(one);
                                         return nx ? nx : ray_error("type", NULL); }
            acc = nx;
        }
        ray_release(one);
        return acc;
    }
    if (!q_vec_is_num(x))
        return ray_error("type", "prd: expects numeric values, got %s",
                         ray_type_name(x->type));
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
    if (!x || !y) return ray_error("type", "wsum/wavg: nil operand");
    int xatom = ray_is_atom(x);
    if (!q_vec_is_num(y) && !ray_is_atom(y)) return ray_error("type", "wsum/wavg: numeric args only");
    if (!xatom && !q_vec_is_num(x)) return ray_error("type", "wsum/wavg: numeric args only");
    int64_t n = ray_is_atom(y) ? (xatom ? 1 : ray_len(x)) : ray_len(y);
    if (!xatom && !ray_is_atom(y) && ray_len(x) != ray_len(y))
        return ray_error("length", "wsum/wavg: length mismatch");
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

static ray_t* q_mwin(ray_t* nx, ray_t* x, q_mw_kind k) {
    int64_t N;
    ray_t* err = q_i64_or_err(nx, &N, "m-window: n");
    if (err) return err;
    if (!x || !q_vec_is_num(x)) {
        if (x && ray_is_atom(x)) { ray_retain(x); return x; }
        return ray_error("type", "m-window: x");
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
ray_t* q_msum_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_SUM); }
ray_t* q_mmax_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_MAX); }
ray_t* q_mmin_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_MIN); }
ray_t* q_mcount_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_COUNT); }
ray_t* q_mdev_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_DEV); }

/* q `a ema x` — exponential moving average: e[0]=x[0], e[i]=a*x[i]+(1-a)*e[i-1].
 * `a` is a float atom (the smoothing factor). */
ray_t* q_ema_wrap(ray_t* a, ray_t* x) {
    if (!a || !ray_is_atom(a)) return ray_error("type", "ema: smoothing factor must be an atom");
    int an; double alpha = q_velem_f(a, 0, &an);
    if (!x || !q_vec_is_num(x)) {
        if (x && ray_is_atom(x)) { ray_retain(x); return x; }
        return ray_error("type", "ema: numeric vector rhs");
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

/* q `neg` / monadic `-` — negate.  kdb negates a date's underlying day count
 * PRESERVING the type (function_neg.qcmd: neg 2000.01.01 2012.01.01 ->
 * 2000.01.01 1988.01.01; 0Wd <-> -0Wd; 0Nd passes through), where base
 * ray_neg_fn rejects temporals — so the date arm lives here and every other
 * input delegates.  Registered ATOMIC: eval's atomic_map_unary maps vectors
 * element-wise and its typed-out path already carries RAY_DATE, so a date
 * vector comes back a date vector.  time/timestamp arms arrive with their
 * datatypes (deferred). */
ray_t* q_neg_wrap(ray_t* x) {
    if (x && x->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_date(-(int64_t)x->i32);
    }
    if (x && x->type == -RAY_MINUTE) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_minute(-(int64_t)x->i32);
    }
    if (x && x->type == -RAY_SECOND) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_second(-(int64_t)x->i32);
    }
    if (x && x->type == -RAY_TIMESPAN) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_timespan(-x->i64);
    }
    if (x && x->type == -RAY_MONTH) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_month(-(int64_t)x->i32);
    }
    if (x && x->type == -RAY_DATETIME) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_datetime(-x->f64);
    }
    /* kdb `neg` promotes a boolean to INT and negates (`neg 1b` -> -1i);
     * base ray_neg_fn rejects bools.  Registered ATOMIC, so a bool vector
     * arrives here element-wise and the i32 atoms collapse to an i32 vector. */
    if (x && x->type == -RAY_BOOL)
        return ray_i32(-(int32_t)(x->b8 ? 1 : 0));
    return ray_neg_fn(x);
}

/* q `null x` — elementwise null test.  Drives the engine's atomic `nil?`
 * (ray_nil_fn) through atomic_map_unary so it broadcasts over typed vectors
 * AND nested general lists at every depth; collection_elem reconstructs
 * typed-null atoms, so nulls are SEEN (unlike other atomics, which stay
 * null-avoiding via the dispatch guards).  Registered RAY_FN_NONE — NOT
 * ATOMIC — so it receives the whole argument here and owns the collapse: a
 * heterogeneous input list yields a homogeneous bool-atom run that
 * q_collapse_list folds to a bool vector (`null (1;\`a;2.5;"x")` -> 0000b),
 * while a nested list yields a list of bool VECTORS that q_collapse_list
 * leaves intact (multi-line, `null (0N 1;2 0N)` -> 10b / 01b). */
/* q-layer null test: the base engine's `ray_nil_fn` treats sym id 0 as the
 * EMPTY symbol (a value, include/rayforce.h SYM case), but q treats the null
 * symbol `` ` `` AS null (`null \`` -> 1b).  This wrapper special-cases the
 * null symbol here in the q layer so the divergence stays out of base rayfall,
 * whose own paths rely on sym-0-as-empty.  Drives `q_null_wrap` for both the
 * atom path and the per-element `atomic_map_unary` recursion (nested lists /
 * symbol vectors reconstruct null-sym atoms via collection_elem). */
static ray_t* q_nil_fn(ray_t* x) {
    if (q_is_null_sym(x)) return ray_bool(true);
    return ray_nil_fn(x);
}

/* q `raze x` — base ray_raze_fn plus the kdb atom arm: an atom comes back as
 * a 1-item list (ref/raze.md `raze 42` -> ,42).  Everything else delegates. */
ray_t* ray_raze_fn(ray_t* x);                        /* base (ops/builtins.c) */
ray_t* q_raze_wrap(ray_t* x) {
    /* strings are kdb char LISTS (rank 1) — never the atom arm */
    if (x && ray_is_atom(x) && x->type != -RAY_STR) {
        ray_t* l = ray_list_new(1);
        if (RAY_IS_ERR(l)) return l;
        l = ray_list_append(l, x);                   /* retains x */
        if (RAY_IS_ERR(l)) return l;
        ray_t* c = q_collapse_list(l);               /* owned */
        ray_release(l);
        return c;
    }
    return ray_raze_fn(x);
}

/* q `enlist` — base ray_enlist_fn plus the kdb dict arm: enlist of a bare
 * dict is a 1-ROW TABLE (ref/enlist.md: `` enlist `a`b`c!(1;2 3; 4) ``
 * displays a table whose b cell is 2 3).  Construction: the dict with each
 * value ENLISTED (1-item column; atoms collapse to typed 1-vecs, vector
 * cells stay boxed) flipped through the one flip home.  Env-bound by
 * q_builtins_register BEFORE registry init, so the `,` monadic QK_ENV
 * snapshot picks this wrapper up too. */
ray_t* ray_enlist_fn(ray_t** args, int64_t n);       /* base (ops/builtins.c) */
ray_t* q_enlist_wrap_vary(ray_t** args, int64_t n) {
    if (n == 1 && args[0] && args[0]->type == RAY_DICT && !q_is_keyed_table(args[0])) {
        ray_t* d = args[0];
        ray_t* k = ray_dict_keys(d);                 /* borrowed */
        ray_t* v = ray_dict_vals(d);                 /* borrowed */
        if (!k || !v) return ray_error("type", "enlist: malformed dictionary");
        int64_t nd = ray_dict_len(d);
        ray_t* ev = ray_list_new(nd > 0 ? nd : 1);
        if (RAY_IS_ERR(ev)) return ev;
        for (int64_t i = 0; i < nd; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* cell = ray_at_fn(v, ia);          /* owned */
            ray_release(ia);
            if (!cell || RAY_IS_ERR(cell)) { ray_release(ev); return cell ? cell : ray_error("type", NULL); }
            ray_t* col = ray_list_new(1);
            if (RAY_IS_ERR(col)) { ray_release(cell); ray_release(ev); return col; }
            col = ray_list_append(col, cell);        /* retains */
            ray_release(cell);
            if (RAY_IS_ERR(col)) { ray_release(ev); return col; }
            ray_t* cc = q_collapse_list(col);        /* atoms -> typed 1-vec */
            ray_release(col);
            if (!cc || RAY_IS_ERR(cc)) { ray_release(ev); return cc ? cc : ray_error("type", NULL); }
            ev = ray_list_append(ev, cc);            /* retains */
            ray_release(cc);
            if (RAY_IS_ERR(ev)) return ev;
        }
        ray_retain(k);                               /* dict_new consumes */
        ray_t* ed = ray_dict_new(k, ev);             /* consumes k + ev */
        if (!ed || RAY_IS_ERR(ed)) return ed ? ed : ray_error("type", NULL);
        ray_t* t = q_flip_wrap(ed);                  /* owned table */
        ray_release(ed);
        return t;
    }
    return ray_enlist_fn(args, n);
}

ray_t* q_null_wrap(ray_t* x) {
    ray_t* r = is_collection(x) ? atomic_map_unary(q_nil_fn, x) : q_nil_fn(x);
    if (!r || RAY_IS_ERR(r) || r->type != RAY_LIST) return r;
    ray_t* c = q_collapse_list(r);   /* owned: retains-or-builds */
    ray_release(r);
    return c;
}

/* q `x within y` — bounds check (ref/within.md: 1 3 10 6 4 within 2 6 ->
 * 01011b; inclusive).  Base ray_within_fn takes VECTOR vals only and reads
 * the range buffer at the vals' element width, so: an atom x is enlisted
 * (via list+collapse) and the answer unwrapped back to a bool atom, and the
 * two element widths must agree ('type — a silent misread otherwise).  The
 * flip-of-pairs range form and mixed-width operands are deferred cells. */
ray_t* q_within_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "within: nil operand");
    if (!ray_is_vec(y) || ray_len(y) != 2)
        return ray_error("type", "within: range must be a 2-item vector");
    ray_t* vals = x;
    ray_t* vals_owned = NULL;
    if (ray_is_atom(x)) {
        ray_t* l = ray_list_new(1);
        if (RAY_IS_ERR(l)) return l;
        l = ray_list_append(l, x);
        if (RAY_IS_ERR(l)) return l;
        vals_owned = q_collapse_list(l);
        ray_release(l);
        if (!vals_owned || RAY_IS_ERR(vals_owned))
            return vals_owned ? vals_owned : ray_error("type", NULL);
        if (!ray_is_vec(vals_owned)) {           /* strings & friends: deferred */
            ray_release(vals_owned);
            return ray_error("type", "within: unsupported value type (deferred)");
        }
        vals = vals_owned;
    }
    if (!ray_is_vec(vals)) {
        if (vals_owned) ray_release(vals_owned);
        return ray_error("type", "within: unsupported value type (deferred)");
    }
    /* Base ray_within_fn dispatches on vals->type ONLY and reads the range
     * buffer as that element type, so ANY type mismatch — not just a width
     * mismatch — would silently reinterpret raw bits (codex: 1 2 within
     * 1.5 2.5 read the doubles as int64 -> 00b).  Same-type operands only;
     * mixed-type coercion is a deferred cell (error, never a wrong answer). */
    if (vals->type != y->type) {
        if (vals_owned) ray_release(vals_owned);
        return ray_error("type", "within: value/range types must match (mixed-type deferred)");
    }
    ray_t* r;
    if (vals->type == RAY_TIMESTAMP) {
        /* base ray_within_fn has no i64-temporal arm; the payload is i64, so
         * relabel both sides through the one cast home and delegate (the
         * same-byte-rep TIMESTAMP<->I64 relabel, builtins.c). */
        ray_t* vi = q_cast_to(RAY_I64, vals);
        if (!vi || RAY_IS_ERR(vi)) { if (vals_owned) ray_release(vals_owned); return vi; }
        ray_t* yi = q_cast_to(RAY_I64, y);
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

/* q `sum x` — LIST arm sums the items (kdb: `sum(2013.03.15;18:55:40.686)`
 * is a timestamp; Load Fixed pins `sum("DT";8 9)0:enlist"…"`).  Non-lists
 * keep the base vector aggregate. */
ray_t* q_sum_wrap(ray_t* x) {
    if (x && x->type == RAY_LIST && ray_len(x) > 0) {
        /* fold q `+` over the items via call_fn2 (the ATOMIC dispatch —
         * ray_add_fn alone is the atom kernel; vectors broadcast in eval). */
        ray_t* plus = q_registry_lookup_name("+", 1, Q_DYADIC);   /* borrowed */
        if (!plus) return ray_error("type", "sum: + unavailable");
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* acc = e[0];
        ray_retain(acc);
        for (int64_t i = 1; i < ray_len(x); i++) {
            ray_t* nx = call_fn2(plus, acc, e[i]);
            ray_release(acc);
            if (!nx || RAY_IS_ERR(nx)) return nx ? nx : ray_error("oom", NULL);
            acc = nx;
        }
        return acc;
    }
    return ray_sum_fn(x);
}
