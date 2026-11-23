/* ops/q_agg.c — running/weighted/covariance/moving-window aggregates (wave 5),
 * the list-arm sum wrapper, the first/last index-miss adapters, all/any and med's middle items
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_count.h"
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/base/q_err.h"
#include "qlang/ops/q_index.h" /* q_index_at — first/last of an empty ride the miss law */
#include "qlang/ops/q_dollar.h" /* q_dollar_cast — all/any read their argument as boolean */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_value — all/any apply min/max */
#include "lang/eval.h"     /* ray_sum_fn, ray_avg_fn, ray_mul_fn, ray_first_fn, ray_last_fn — engine arms */
#include "lang/internal.h" /* atomic_map_binary, make_f64, is_list, is_numeric, as_f64 */
#include <math.h>          /* isnan, sqrt — sentinel-null discipline, mdev/cov */
#include <string.h>        /* memcpy — the width-generic scan store */

/* ===== Wave 5 — running / weighted / covariance aggregates ================
 * kdb ref/{sums,prds,maxs,mins,avgs,ratios,wsum,wavg,cov}.md.  Null discipline
 * per page: sum/sums treat null as 0, prd/prds as 1, avg/avgs EXCLUDE nulls.
 * maxs/mins skip nulls and lead with the type's ∓infinity identity — owned by
 * ray_type_inf, the same law as the src/ops/agg.c aggregates (maxs 0N 5 ->
 * -0W 5).  mmax/mmin are ASYMMETRIC by kdb design: mmax replaces nulls after
 * the first with the preceding max (ref/max.md), while for mmin a null IS the
 * window minimum (ref/min.md) — do not fold them into one law. */

/* Read element i of a numeric or temporal vector as a double; *isnull set for the typed
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
    case RAY_I64: RAY_TEMPORAL64_CASES: { int64_t v = ((const int64_t*)d)[i]; if (v==NULL_I64){*isnull=1;} return (double)v; }
    case RAY_I32: RAY_TEMPORAL32_CASES: { int32_t v = ((const int32_t*)d)[i]; if (v==NULL_I32){*isnull=1;} return (double)v; }
    case RAY_I16: { int16_t v = ((const int16_t*)d)[i]; if (v==NULL_I16){*isnull=1;} return (double)v; }
    case RAY_BOOL:return (double)((const uint8_t*)d)[i];
    case RAY_BYTE_ONLY: case RAY_CHARV: return (double)((const uint8_t*)d)[i];
    case RAY_F64: RAY_TEMPORALF_CASES: { double v = ((const double*)d)[i]; if (isnan(v)){*isnull=1;} return v; }
    case RAY_F32: { float  v = ((const float*)d)[i];  if (isnan(v)){*isnull=1;} return (double)v; }
    default: *isnull = 1; return 0;
    }
}
/* an n-long result vector, or ray_vec_new's owned error untouched: setting len on
 * that error would overwrite its code and the caller's writes would overrun it */
static ray_t* agg_vec_out(int8_t type, int64_t n) {
    ray_t* out = ray_vec_new(type, n > 0 ? n : 1);
    if (!RAY_IS_ERR(out)) out->len = n;
    return out;
}
int q_vec_is_float(ray_t* x) { return x->type == RAY_F64 || x->type == RAY_F32; }
int q_vec_is_num(ray_t* x) {
    return x && (x->type==RAY_I64||x->type==RAY_I32||x->type==RAY_I16||
                 x->type==RAY_BOOL||ray_is_bytelike(x->type)||x->type==RAY_F64||x->type==RAY_F32);
}

typedef enum { RS_SUMS, RS_PRDS, RS_MAXS, RS_MINS, RS_AVGS } q_rs_kind;

/* Width-generic float store: the scans accumulate in double but the result
 * keeps the input's width (ref/sums.md: domain e -> range e), so an f32 out
 * vector narrows on store — the float twin of rs_store. */
static void runscan_store_f(char* o, int esz, int64_t i, double v) {
    if (esz == 4) { float f = (float)v; memcpy(o + i * 4, &f, 4); }
    else memcpy(o + i * 8, &v, 8);
}

/* running max/min over a byte-uniform vector — charv (kdb `maxs "genie"` ->
 * "ggnnn") and u8 alike; bytes have no null, so no identity arm. */
static ray_t* runscan_bytes(ray_t* x, q_rs_kind k) {
    int64_t n = q_count(x);
    ray_t* out = agg_vec_out(x->type, n); if (RAY_IS_ERR(out)) return out;
    const uint8_t* p = (const uint8_t*)ray_data(x);
    uint8_t* o = (uint8_t*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (i && (k == RS_MAXS ? o[i-1] > c : o[i-1] < c)) c = o[i-1];
        o[i] = c;
    }
    return out;
}

/* exact int lanes: ±0W survive and a timespan passes 2^53, which a double read would round */
RAY_INLINE int64_t rs_load(const void* d, int esz, int64_t i, int* nu) {
    switch (esz) {
    case 8: { int64_t v = ((const int64_t*)d)[i]; *nu = v == NULL_I64; return v; }
    case 4: { int32_t v = ((const int32_t*)d)[i]; *nu = v == NULL_I32; return v; }
    case 2: { int16_t v = ((const int16_t*)d)[i]; *nu = v == NULL_I16; return v; }
    default: *nu = 0; return ((const uint8_t*)d)[i];
    }
}
RAY_INLINE void rs_store(void* o, int esz, int64_t i, int64_t v) {
    switch (esz) {
    case 8: ((int64_t*)o)[i] = v; break;
    case 4: ((int32_t*)o)[i] = (int32_t)v; break;
    case 2: ((int16_t*)o)[i] = (int16_t)v; break;
    default: ((uint8_t*)o)[i] = (uint8_t)v; break;
    }
}
/* sums/prds wrap in unsigned arithmetic (UB-free); a null adds 0 / multiplies by 1 */
RAY_INLINE void rs_int_loop(const void* d, int iesz, void* o, int oesz, int64_t n, q_rs_kind k, int64_t inf) {
    if (k == RS_SUMS || k == RS_PRDS) {
        uint64_t acc = k == RS_PRDS;
        for (int64_t i = 0; i < n; i++) {
            int nu; uint64_t v = (uint64_t)rs_load(d, iesz, i, &nu);
            if (k == RS_SUMS) acc += nu ? 0 : v; else acc *= nu ? 1 : v;
            rs_store(o, oesz, i, (int64_t)acc);
        }
        return;
    }
    int64_t m = inf; int started = 0;
    for (int64_t i = 0; i < n; i++) {
        int nu; int64_t v = rs_load(d, iesz, i, &nu);
        if (!nu && (!started || (k == RS_MAXS ? v > m : v < m))) { m = v; started = 1; }
        rs_store(o, oesz, i, m);
    }
}
/* one width-constant instance per lane pair, so the loads and stores inline to plain moves */
static void runscan_int(ray_t* x, ray_t* out, int64_t n, q_rs_kind k, int64_t inf) {
    const void* d = ray_data(x); void* o = ray_data(out);
    int oesz = ray_elem_size(out->type);
    switch (ray_elem_size(x->type)) {
    case 8:  rs_int_loop(d, 8, o, 8, n, k, inf); break;
    case 4:  rs_int_loop(d, 4, o, 4, n, k, inf); break;
    case 2:  if (oesz == 4) rs_int_loop(d, 2, o, 4, n, k, inf); else rs_int_loop(d, 2, o, 2, n, k, inf); break;
    default: if (oesz == 4) rs_int_loop(d, 1, o, 4, n, k, inf); else rs_int_loop(d, 1, o, 1, n, k, inf); break;
    }
}

static ray_t* runscan(ray_t* x, q_rs_kind k) {
    if (!x) return q_err(QE_TYPE);
    if (ray_is_atom(x)) {                 /* avgs -> float; the others keep the atom (sum.md:92, prd.md:50) */
        if (k == RS_AVGS) { int nu; double v = q_velem_f(x, 0, &nu);
                            return nu ? ray_typed_null(-RAY_F64) : ray_f64(v); }
        ray_retain(x); return x;
    }
    if ((k == RS_MAXS || k == RS_MINS) && ray_is_vec(x) && ray_is_bytelike(x->type))
        return runscan_bytes(x, k);
    /* a char is not a number in arithmetic: `sums "abc"` is 'type (sum.md:113, owner ruling 2026-10-09);
     * a temporal is ordered, so maxs/mins keep it (max.md:65-66), and sums takes the temporals sum takes */
    int ordered = (k == RS_MAXS || k == RS_MINS) && ray_is_vec(x) && temporal_tag(x->type);
    int summed = k == RS_SUMS && ray_is_vec(x) && RAY_IS_TEMPORAL(x->type) && agg_type_admitted(OP_SUM, x->type);
    if (!ray_is_vec(x) || !(q_type_is_num_tag(x->type) || ordered || summed)) return q_err(QE_TYPE);
    int64_t n = q_count(x);
    if (k == RS_AVGS) {
        ray_t* out = agg_vec_out(RAY_F64, n); if (RAY_IS_ERR(out)) return out;
        double* o = (double*)ray_data(out);
        double s = 0; int64_t c = 0;
        for (int64_t i = 0; i < n; i++) {
            int nu; double v = q_velem_f(x, i, &nu);
            if (!nu) { s += v; c++; }
            if (c == 0) { o[i] = 0; ray_vec_set_null(out, i, true); } else o[i] = s / (double)c;
        }
        return out;
    }
    int isf = q_vec_is_float(x) || RAY_IS_TEMPORALF(x->type);
    if (k == RS_MAXS || k == RS_MINS) {
        if (isf) {
            ray_t* out = agg_vec_out(x->type, n); if (RAY_IS_ERR(out)) return out;
            char* o = (char*)ray_data(out);
            int esz = ray_elem_size(x->type);
            double m = (k == RS_MAXS) ? -INFINITY : INFINITY;   /* leading-null identity */
            for (int64_t i = 0; i < n; i++) {
                int nu; double v = q_velem_f(x, i, &nu);
                if (!nu && (k == RS_MAXS ? v > m : v < m)) m = v;
                runscan_store_f(o, esz, i, m);
            }
            return out;
        }
        ray_t* out = agg_vec_out(x->type, n); if (RAY_IS_ERR(out)) return out;
        int64_t inf = 0;
        ray_type_inf(x->type, k == RS_MINS, &inf);   /* false only for BOOL (null-free) */
        runscan_int(x, out, n, k, inf);
        return out;
    }
    if (isf) {
        ray_t* out = agg_vec_out(x->type, n); if (RAY_IS_ERR(out)) return out;
        char* o = (char*)ray_data(out);
        int esz = ray_elem_size(x->type);
        double acc = (k==RS_PRDS) ? 1 : 0;
        for (int64_t i = 0; i < n; i++) {
            int nu; double v = q_velem_f(x, i, &nu);
            if (k==RS_SUMS) acc += nu?0:v; else acc *= nu?1:v;
            runscan_store_f(o, esz, i, acc);
        }
        return out;
    }
    ray_t* out = agg_vec_out(agg_sum_type(x->type), n); if (RAY_IS_ERR(out)) return out;
    runscan_int(x, out, n, k, 0);
    return out;
}
ray_t* q_sums_wrap(ray_t* x){ return runscan(x, RS_SUMS); }
ray_t* q_prds_wrap(ray_t* x){ return runscan(x, RS_PRDS); }
ray_t* q_maxs_wrap(ray_t* x){ return runscan(x, RS_MAXS); }
ray_t* q_mins_wrap(ray_t* x){ return runscan(x, RS_MINS); }
ray_t* q_avgs_wrap(ray_t* x){ return runscan(x, RS_AVGS); }

/* the `%':` kernel on a numeric vector (q_adverb.c prior_each): r[0]=x[0], r[i]=x[i]%x[i-1] (float). */
ray_t* q_ratios_wrap(ray_t* x) {
    if (!x || !ray_is_vec(x) || !q_type_is_num_tag(x->type)) return q_err(QE_TYPE);
    int64_t n = q_count(x);
    ray_t* out = agg_vec_out(RAY_F64, n); if (RAY_IS_ERR(out)) return out;
    double* o = (double*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        if (i == 0) {                       /* r[0] = x[0] (null stays null) */
            if (nu) { o[i] = 0; ray_vec_set_null(out, i, true); } else o[i] = v;
            continue;
        }
        int pnu; double pv = q_velem_f(x, i-1, &pnu);
        /* a null operand nulls; a zero divisor is live-infinity `%` (`1b%0b` is 0w, ref/divide.md:27) */
        if (nu || pnu) { o[i] = 0; ray_vec_set_null(out, i, true); }
        else { o[i] = v / pv; if (o[i] != o[i]) ray_vec_set_null(out, i, true); }
    }
    return out;
}

/* q `prd x` — product aggregate, the multiply-over fold twin of prds (ref/prd.md):
 * a numeric vector folds to its product with NULLS TREATED AS 1s (`prd 2 3 0N 7`
 * -> 42) in the dyad's range (`prd 101b` -> 0i); a general list is the multiply-over fold of its
 * items, as sum's list arm is the add-over fold (a one-item fold returns the item).
 * Non-numeric -> 'type. */
ray_t* q_prd_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == -RAY_STR || x->type == RAY_SYM)
        return q_err(QE_TYPE);
    if (ray_is_atom(x)) return agg_atom_result(x);
    if (x->type == RAY_LIST) {
        int64_t n = q_count(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (n == 0) return ray_i64(1);                 /* empty product (derived) */
        ray_t* acc = e[0];
        ray_retain(acc);
        for (int64_t i = 1; i < n; i++) {
            ray_t* nx = atomic_map_binary(ray_mul_fn, acc, e[i]);
            ray_release(acc);
            if (!nx || RAY_IS_ERR(nx)) return nx ? nx : q_err(QE_TYPE);
            acc = nx;
        }
        return acc;
    }
    if (!q_vec_is_num(x))
        return q_err(QE_TYPE);
    int64_t n = q_count(x);
    if (q_vec_is_float(x)) {
        double acc = 1;
        for (int64_t i = 0; i < n; i++) {
            int nu; double v = q_velem_f(x, i, &nu);
            if (!nu) acc *= v;                         /* nulls are 1s */
        }
        /* e in, e out (ref/prd.md) — width test, same axis as runscan_store_f */
        if (ray_elem_size(x->type) == 4) return ray_f32((float)acc);
        return make_f64(acc);
    }
    int64_t acc = 1;
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        if (!nu) acc = (int64_t)((uint64_t)acc * (uint64_t)(int64_t)v);
    }
    return make_typed_int((int8_t)-agg_sum_type(x->type), acc);
}

/* q `x wavg y` — weighted average (sum x*y) % sum x, pairs where EITHER side
 * is null excluded (kdb); an atom x broadcasts.  Kept in C: the q.q
 * composition's bool-multiply denominator measured 16x slower (2026-07-15).
 * wsum/cov/scov are q.q-hosted (their doc formulas ride engine kernels). */
/* avg.md:180-203: wavg takes the vectors avg takes, a temporal read as its counts */
static int wavg_operand(ray_t* v) { return ray_is_vec(v) && agg_type_admitted(OP_AVG, v->type); }

ray_t* q_wavg_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    int xatom = ray_is_atom(x);
    if (!wavg_operand(y) && !ray_is_atom(y)) return q_err(QE_TYPE);
    if (!xatom && !wavg_operand(x)) return q_err(QE_TYPE);
    int64_t n = ray_is_atom(y) ? q_count(x) : q_count(y);
    if (!xatom && !ray_is_atom(y) && q_count(x) != q_count(y))
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
 * msum treats null as 0; max/dev/count exclude nulls (an all-null mmax window
 * yields -0W, ref/max.md); for mmin a null IS the window minimum (ref/min.md
 * — the mmax/mmin asymmetry).  N<=0: mmin returns y (ref/min.md); the rest
 * keep the empty window (sum/count 0, others null).  mavg is q.q-hosted. */
typedef enum { MW_SUM, MW_MAX, MW_MIN, MW_COUNT, MW_DEV } q_mw_kind;

static inline __attribute__((always_inline)) int64_t mw_lo(int64_t N, int64_t i) {
    if (N <= 0) return i + 1;
    return i - N + 1 > 0 ? i - N + 1 : 0;
}
/* the exact lane value (an int lane never passes through a double) and its null */
static inline __attribute__((always_inline)) int64_t mw_lane(const void* d, int esz, int isf, int64_t i, int* nu) {
    if (!isf) return rs_load(d, esz, i, nu);
    double v = esz == 4 ? (double)((const float*)d)[i] : ((const double*)d)[i];
    *nu = isnan(v); return 0;
}
static inline __attribute__((always_inline)) double mw_load(const void* d, int esz, int isf, int64_t i, int* nu) {
    if (!isf) return (double)rs_load(d, esz, i, nu);
    double v = esz == 4 ? (double)((const float*)d)[i] : ((const double*)d)[i];
    *nu = isnan(v); return v;
}
static inline __attribute__((always_inline)) int mw_beats(const void* d, int esz, int isf, int max, int64_t a, int64_t b) {
    int nu;
    if (isf) { double va = mw_load(d, esz, 1, a, &nu), vb = mw_load(d, esz, 1, b, &nu); return max ? va > vb : va < vb; }
    int64_t va = rs_load(d, esz, a, &nu), vb = rs_load(d, esz, b, &nu);
    return max ? va > vb : va < vb;
}

/* Up to MW_SMALL an mmax/mmin window is rescanned (branch-free, MW_BLK windows at a time); past it the deque wins. */
enum { MW_BLK = 16, MW_SMALL = 32 };
typedef double mw_v4 __attribute__((vector_size(32)));
typedef int64_t mw_m4 __attribute__((vector_size(32)));
static inline __attribute__((always_inline)) mw_v4 mw_pick(mw_m4 keep, mw_v4 a, mw_v4 b) { return (mw_v4)(((mw_m4)a & keep) | ((mw_m4)b & ~keep)); }
static inline __attribute__((always_inline)) mw_m4 mw_ipick(mw_m4 keep, mw_m4 a, mw_m4 b) { return (a & keep) | (b & ~keep); }
static inline __attribute__((always_inline)) void mw_store_null(ray_t* out, int fout, int oesz, int64_t i) {
    if (fout) runscan_store_f((char*)ray_data(out), oesz, i, 0); else rs_store(ray_data(out), oesz, i, 0);
    ray_vec_set_null(out, i, true);
}

/* four consecutive items: the float view, the exact int view (int lanes only) and the non-null mask */
static inline __attribute__((always_inline)) void mw_vload(const void* d, int esz, int isf, int wantf, int64_t j, mw_v4* v, mw_m4* iv, mw_m4* ok) {
    if (isf && esz == 8) { memcpy(v, (const double*)d + j, sizeof *v); *ok = (mw_m4)(*v == *v); return; }
    if (!isf && esz == 8) {
        memcpy(iv, (const int64_t*)d + j, sizeof *iv);
        *ok = *iv != NULL_I64; if (wantf) *v = __builtin_convertvector(*iv, mw_v4); return;
    }
    for (int l = 0; l < 4; l++) {
        int nu;
        if (isf) (*v)[l] = mw_load(d, esz, 1, j + l, &nu); else { (*iv)[l] = rs_load(d, esz, j + l, &nu); if (wantf) (*v)[l] = (double)(*iv)[l]; }
        (*ok)[l] = nu ? 0 : -1;
    }
}

/* MW_BLK full windows ending at i..i+MW_BLK-1 side by side.  Each lane takes the scalar loop's steps in its order
 * (a null keeps the lane's state), so the bits are the scalar loop's while the lanes' divides overlap. */
static inline __attribute__((always_inline)) void mw_block(ray_t* out, const void* d, int iesz, int isf, int64_t i, int64_t N, q_mw_kind k, int64_t neg_inf) {
    int8_t otype = out->type;
    int fout = q_type_is_float_tag(otype) || RAY_IS_TEMPORALF(otype), oesz = ray_elem_size(otype);
    mw_v4 c[4] = {{0}}, a[4] = {{0}}, m2[4] = {{0}};
    mw_m4 im[4], nul[4] = {{0}};
    for (int h = 0; h < 4; h++)
        for (int l = 0; l < 4; l++) {
            a[h][l] = k == MW_MAX ? -INFINITY : k == MW_MIN ? INFINITY : 0;
            im[h][l] = k == MW_MAX ? neg_inf : INT64_MAX;
        }
    for (int64_t t = i - N + 1; t <= i; t++)
        for (int h = 0; h < 4; h++) {
            mw_v4 v; mw_m4 iv, ok;
            mw_vload(d, iesz, isf, k == MW_SUM || k == MW_DEV, t + 4 * h, &v, &iv, &ok);
            nul[h] |= ~ok;
            if (k == MW_SUM) a[h] = mw_pick(ok, t == i - N + 1 ? v : a[h] + v, a[h]);
            else if (k == MW_DEV) {
                mw_v4 cn = c[h] + 1, dv = v - a[h], nm = a[h] + dv / cn, nm2 = m2[h] + dv * (v - nm);
                c[h] = mw_pick(ok, cn, c[h]); a[h] = mw_pick(ok, nm, a[h]); m2[h] = mw_pick(ok, nm2, m2[h]);
            } else if (isf) a[h] = mw_pick(ok & (mw_m4)(k == MW_MAX ? v > a[h] : v < a[h]), v, a[h]);
            else im[h] = mw_ipick(ok & (mw_m4)(k == MW_MAX ? iv > im[h] : iv < im[h]), iv, im[h]);
        }
    mw_v4 var[4];
    for (int h = 0; h < 4; h++) var[h] = m2[h] / c[h];
    for (int l = 0; l < MW_BLK; l++) {
        int h = l / 4, e = l % 4; int64_t r = i + l;
        if (k == MW_DEV) {
            if (c[h][e]) runscan_store_f((char*)ray_data(out), 8, r, m2[h][e] > 0 ? sqrt(var[h][e]) : 0);
            else mw_store_null(out, 1, 8, r);
        } else if (k == MW_MIN && nul[h][e]) mw_store_null(out, fout, oesz, r);
        else if (fout) runscan_store_f((char*)ray_data(out), oesz, r, a[h][e]);
        else rs_store(ray_data(out), oesz, r, im[h][e]);
    }
}

/* one instance per input lane, so every window read inlines to a plain load */
static inline __attribute__((always_inline)) ray_t* mw_pass(ray_t* out, const void* d, int iesz, int isf, int64_t n, int64_t N, q_mw_kind k) {
    int8_t otype = out->type;
    int fout = q_type_is_float_tag(otype) || RAY_IS_TEMPORALF(otype);
    int oesz = ray_elem_size(otype);
    char* o = (char*)ray_data(out);
    int64_t neg_inf = 0;
    ray_type_inf(otype, false, &neg_inf);
    if ((k == MW_SUM && !fout) || k == MW_COUNT) {
        uint64_t acc = 0;
        for (int64_t i = 0; i < n; i++) {
            int nu;
            if (N > 0) {
                uint64_t v = (uint64_t)mw_lane(d, iesz, isf, i, &nu);
                if (!nu) acc += k == MW_COUNT ? 1 : v;
            }
            if (N > 0 && i - N >= 0) {
                uint64_t v = (uint64_t)mw_lane(d, iesz, isf, i - N, &nu);
                if (!nu) acc -= k == MW_COUNT ? 1 : v;
            }
            rs_store(o, oesz, i, (int64_t)acc);
        }
        return out;
    }
    if (k == MW_SUM || k == MW_DEV || N <= MW_SMALL) {
        for (int64_t i = 0; i < n; i++) {
            if (N > 0 && i >= N - 1 && i + MW_BLK <= n) { mw_block(out, d, iesz, isf, i, N, k, neg_inf); i += MW_BLK - 1; continue; }
            int64_t lo = mw_lo(N, i), c = 0, im = 0; int started = 0, sawnull = 0;
            double sum = 0, mean = 0, m2 = 0, m = 0;
            for (int64_t j = lo; j <= i; j++) {
                int nu; int64_t jv = isf ? 0 : rs_load(d, iesz, j, &nu);
                double v = isf ? mw_load(d, iesz, 1, j, &nu) : (double)jv;
                if (nu) { sawnull = 1; continue; }
                if (k == MW_SUM) { sum = j == lo ? v : sum + v; continue; }   /* the window opens on its first value: a lone -0f stays -0f */
                if (k == MW_DEV) { c++; double dv = v - mean; mean += dv / (double)c; m2 += dv * (v - mean); continue; }   /* Welford: no cancellation at 1e17 */
                if (!started || (isf ? (k == MW_MAX ? v > m : v < m) : (k == MW_MAX ? jv > im : jv < im))) { m = v; im = jv; }
                started = 1;
            }
            if (k == MW_SUM) runscan_store_f(o, oesz, i, sum);
            else if (k == MW_DEV) { if (c) runscan_store_f(o, oesz, i, m2 > 0 ? sqrt(m2 / (double)c) : 0); else mw_store_null(out, 1, oesz, i); }
            else if (k == MW_MIN && sawnull) mw_store_null(out, fout, oesz, i);
            else if (started) { if (fout) runscan_store_f(o, oesz, i, m); else rs_store(o, oesz, i, im); }
            else if (k == MW_MAX && lo <= i) { if (fout) runscan_store_f(o, oesz, i, -INFINITY); else rs_store(o, oesz, i, neg_inf); }   /* all-null window */
            else mw_store_null(out, fout, oesz, i);
        }
        return out;
    }
    /* a monotone deque of window positions whose front is the earliest extreme, as the rescan keeps */
    ray_t* dq = agg_vec_out(RAY_I64, n);
    if (RAY_IS_ERR(dq)) { ray_release(out); return dq; }
    int64_t* q = (int64_t*)ray_data(dq);
    int64_t h = 0, t = 0, lastnull = -1;
    for (int64_t i = 0; i < n; i++) {
        int64_t lo = mw_lo(N, i);
        int nu; mw_lane(d, iesz, isf, i, &nu);
        if (nu) lastnull = i;
        else {
            while (t > h && mw_beats(d, iesz, isf, k == MW_MAX, i, q[t-1])) t--;
            q[t++] = i;
        }
        while (t > h && q[h] < lo) h++;
        if (k == MW_MIN && lastnull >= lo) mw_store_null(out, fout, oesz, i);
        else if (t > h) {
            if (fout) runscan_store_f(o, oesz, i, mw_load(d, iesz, isf, q[h], &nu));
            else rs_store(o, oesz, i, rs_load(d, iesz, q[h], &nu));
        } else if (k == MW_MAX && lo <= i) {
            if (fout) runscan_store_f(o, oesz, i, -INFINITY); else rs_store(o, oesz, i, neg_inf);   /* all-null window */
        } else mw_store_null(out, fout, oesz, i);
    }
    ray_release(dq);
    return out;
}

static ray_t* mwin(ray_t* nx, ray_t* x, q_mw_kind k) {
    int64_t N;
    if (nx && !ray_is_atom(nx)) return q_err(QE_DOMAIN);
    if (!nx || RAY_IS_TEMPORAL(-nx->type) || !q_type_count_i64(nx, &N)) return q_err(QE_TYPE);   /* grid rows b x h i j */
    if (!x) return q_err(QE_TYPE);
    if (ray_is_atom(x)) {          /* the dyadic grids probe atoms (tools.q ddt2): an atom answers as its one-item window */
        if (!is_numeric_or_temporal(x)) {
            if (k != MW_MAX && k != MW_MIN) return q_err(QE_TYPE);
            ray_retain(x); return x;
        }
        ray_t* v = ray_enlist_fn(&x, 1);
        if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_TYPE);
        ray_t* r = mwin(nx, v, k);
        ray_release(v);
        if (!r || RAY_IS_ERR(r)) return r;
        ray_t* a = q_index_elem_at(r, 0);
        ray_release(r);
        return a;
    }
    if (!q_vec_is_num(x) && !RAY_IS_TEMPORAL(x->type) && !RAY_IS_TEMPORALF(x->type)) return q_err(QE_TYPE);
    if (k == MW_MIN && N <= 0) { ray_retain(x); return x; }
    int64_t n = q_count(x);
    int isf = q_vec_is_float(x) || RAY_IS_TEMPORALF(x->type);
    /* msum is a difference of running sums (sum.md:224), so it answers in the difference type: a char has none */
    int8_t otype = k == MW_SUM ? q_type_diff(x->type) : (k == MW_MAX || k == MW_MIN) ? x->type
                 : k == MW_COUNT ? RAY_I64 : RAY_F64;
    if (!otype) return q_err(QE_TYPE);
    ray_t* out = agg_vec_out(otype, n); if (RAY_IS_ERR(out)) return out;
    const void* d = ray_data(x);
    if (isf) return ray_elem_size(x->type) == 4 ? mw_pass(out, d, 4, 1, n, N, k) : mw_pass(out, d, 8, 1, n, N, k);
    switch (ray_elem_size(x->type)) {
    case 8:  return mw_pass(out, d, 8, 0, n, N, k);
    case 4:  return mw_pass(out, d, 4, 0, n, N, k);
    case 2:  return mw_pass(out, d, 2, 0, n, N, k);
    default: return mw_pass(out, d, 1, 0, n, N, k);
    }
}
ray_t* q_msum_wrap(ray_t* n, ray_t* x){ return mwin(n, x, MW_SUM); }
ray_t* q_mmax_wrap(ray_t* n, ray_t* x){ return mwin(n, x, MW_MAX); }
ray_t* q_mmin_wrap(ray_t* n, ray_t* x){ return mwin(n, x, MW_MIN); }
ray_t* q_mcount_wrap(ray_t* n, ray_t* x){ return mwin(n, x, MW_COUNT); }
ray_t* q_mdev_wrap(ray_t* n, ray_t* x){ return mwin(n, x, MW_DEV); }

/* q `a ema x` — exponential moving average: e[0]=x[0], e[i]=a[i]*x[i]+(1-a[i])*e[i-1].
 * `a` is an atom or a list of count x (ref/ema.md:19), so a[0] never weighs in. */
ray_t* q_ema_wrap(ray_t* a, ray_t* x) {
    int wv = a && !ray_is_atom(a);
    if (!a || !q_type_is_num_tag(a->type)) return q_err(QE_TYPE);
    if (!x || !q_vec_is_num(x)) {
        if (x && ray_is_atom(x) && !wv) { ray_retain(x); return x; }
        return q_err(QE_TYPE);
    }
    int64_t n = q_count(x);
    if (wv && q_count(a) != n) return q_err(QE_LENGTH);
    ray_t* out = agg_vec_out(RAY_F64, n); if (RAY_IS_ERR(out)) return out;
    double* o = (double*)ray_data(out);
    double e = 0;
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        double xv = nu ? 0 : v;
        if (i == 0) e = xv;
        else {
            double alpha = q_velem_f(a, wv ? i : 0, &nu);
            e = alpha * xv + (1.0 - alpha) * e;
        }
        o[i] = e;
    }
    return out;
}

/* q `first x` IS `x[0]` and `last x` IS `x[count[x]-1]`, so an EMPTY collection
 * answers from the index miss law rather than from a second copy of it: `() 0`
 * is `()`, a typed empty its own typed null.  Everything non-empty (and every
 * lazy/DAG input) keeps the base aggregate, whose answer the index home agrees
 * with by construction. */
static ray_t* end_item(ray_t* x, int64_t i, ray_t* (*base)(ray_t*)) {
    if (!x || RAY_IS_ERR(x) || ray_is_lazy(x) ||
        !(x->type == RAY_LIST || ray_is_vec(x)) || q_count(x) != 0)
        return base(x);
    ray_t* ia = ray_i64(i);
    ray_t* r  = q_index_at(x, (ray_t* const[]){ ia }, 1);
    ray_release(ia);
    return r;
}

ray_t* q_first_wrap(ray_t* x) { return end_item(x, 0, ray_first_fn); }
ray_t* q_last_wrap(ray_t* x)  { return end_item(x, -1, ray_last_fn); }

/* q `sum x` — LIST arm sums the items (kdb: `sum(2013.03.15;18:55:40.686)`
 * is a timestamp; Load Fixed pins `sum("DT";8 9)0:enlist"…"`).  Non-lists
 * keep the base vector aggregate. */
ray_t* q_sum_wrap(ray_t* x) {
    if (x && x->type == RAY_LIST && q_count(x) > 0) {
        /* fold q `+` over the items via call_fn2 (the ATOMIC dispatch —
         * ray_add_fn alone is the atom kernel; vectors broadcast in eval). */
        ray_t* plus = q_registry_lookup_name("+", 1, Q_DYADIC);   /* borrowed */
        if (!plus) return q_err(QE_TYPE);
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* acc = e[0];
        ray_retain(acc);
        for (int64_t i = 1; i < q_count(x); i++) {
            ray_t* nx = call_fn2(plus, acc, e[i]);
            ray_release(acc);
            if (!nx || RAY_IS_ERR(nx)) return nx ? nx : q_err(QE_OOM);
            acc = nx;
        }
        return acc;
    }
    return ray_sum_fn(x);
}

/* q `avg x` — a general LIST counts its nulls in the divisor ("If x is a mixed
 * list, null items are treated as zero", ref/avg.md).  The VECTOR arm EXCLUDES
 * them and stays the base kernel's: that divisor is rayfall's own tested
 * contract (test/rfl/null/agg.rfl) and the one group.c's SQL AVG shares. */
ray_t* q_avg_wrap(ray_t* x) {
    if (is_list(x) && q_count(x) > 0) {
        int64_t n = q_count(x);
        ray_t** e = (ray_t**)ray_data(x);
        double s = 0;
        for (int64_t i = 0; i < n; i++) {
            if (!is_numeric(e[i])) return q_err(QE_TYPE);
            if (!RAY_ATOM_IS_NULL(e[i])) s += as_f64(e[i]);
        }
        return make_f64(s / (double)n);
    }
    return ray_avg_fn(x);
}

/* sum and avg hand a vector to the base aggregate (their wrappers above); min and max ARE the base ones; first and
 * last of a run are the items at its two ends — one index.  A string column's items are not atoms: declined. */
ray_t* q_agg_runs(ray_t* f, ray_t* x, ray_t* lens) {
    if (f->type != RAY_UNARY) return NULL;
    ray_unary_fn k = (ray_unary_fn)(uintptr_t)f->i64;
    const int64_t* len = (const int64_t*)ray_data(lens);
    int64_t n = q_count(lens);
    if (k == q_first_wrap || k == q_last_wrap) {
        ray_t* at = !ray_is_vec(x) || x->type == RAY_STR ? NULL : agg_vec_out(RAY_I64, n);
        if (!at || RAY_IS_ERR(at)) return at;
        for (int64_t j = 0, lo = 0; j < n; lo += len[j], j++)
            ((int64_t*)ray_data(at))[j] = k == q_first_wrap ? lo : lo + len[j] - 1;
        ray_t* r = q_index_at(x, &at, 1);
        ray_release(at);
        return r;
    }
    if (k == q_sum_wrap) k = ray_sum_fn;
    else if (k == q_avg_wrap) k = ray_avg_fn;
    return ray_agg_runs(k, x, len, n);
}

/* `all x` is `min "b"$x` and `any x` is `max "b"$x` (docs-v1 ref/logic.md:15,43): every container, empty and type
 * rule is the one min, max and the boolean cast already have. */
static ray_t* bool_extreme(ray_t* x, const char* name) {
    ray_t* b = q_dollar_cast(RAY_BOOL, x);
    if (!b || RAY_IS_ERR(b)) return b ? b : q_err(QE_TYPE);
    ray_t* f = q_registry_lookup_name(name, 3, Q_MONADIC);   /* borrowed */
    ray_t* r = f ? q_eval_apply_value(f, &b, 1) : q_err(QE_TYPE);
    ray_release(b);
    return r;
}

ray_t* q_all_wrap(ray_t* x) { return bool_extreme(x, "min"); }
ray_t* q_any_wrap(ray_t* x) { return bool_extreme(x, "max"); }

/* iasc's order on a fixed-width vector as one signed key: a null (int MIN, a NaN) first, -0 and 0 equal */
static int mid_keys(ray_t* x, int64_t* k) {
    int64_t n = x->len;
    const void* d = ray_data(x);
    switch (x->type) {
    case RAY_I16: for (int64_t i = 0; i < n; i++) k[i] = ((const int16_t*)d)[i]; return 1;
    case RAY_I32: RAY_TEMPORAL32_CASES: for (int64_t i = 0; i < n; i++) k[i] = ((const int32_t*)d)[i]; return 1;
    case RAY_I64: RAY_TEMPORAL64_CASES: memcpy(k, d, (size_t)n * sizeof *k); return 1;
    case RAY_F32: case RAY_F64: RAY_TEMPORALF_CASES:
        for (int64_t i = 0; i < n; i++) {
            double v = x->type == RAY_F32 ? ((const float*)d)[i] : ((const double*)d)[i];
            int64_t b = 0;
            if (v != 0) memcpy(&b, &v, sizeof b);
            k[i] = isnan(v) ? INT64_MIN : b < 0 ? b ^ INT64_MAX : b;
        }
        return 1;
    default: return 0;
    }
}

static int i64_cmp(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

/* the k-th smallest of a[lo..hi] into a[k], smaller left and larger right; past 2*log2 n rounds a sort ends it */
static void i64_select(int64_t* a, int64_t lo, int64_t hi, int64_t k) {
    int depth = 0;
    for (int64_t r = hi - lo + 1; r > 0; r >>= 1) depth += 2;
    while (hi - lo > 16) {
        if (depth-- <= 0) break;
        int64_t mid = lo + ((hi - lo) >> 1), t;
        if (a[lo] > a[mid]) t = a[lo], a[lo] = a[mid], a[mid] = t;
        if (a[lo] > a[hi]) t = a[lo], a[lo] = a[hi], a[hi] = t;
        if (a[mid] > a[hi]) t = a[mid], a[mid] = a[hi], a[hi] = t;
        t = a[mid], a[mid] = a[hi - 1], a[hi - 1] = t;
        int64_t p = a[hi - 1], i = lo, j = hi - 1;
        for (;;) {
            while (a[++i] < p) {}
            while (a[--j] > p) {}
            if (i >= j) break;
            t = a[i], a[i] = a[j], a[j] = t;
        }
        t = a[i], a[i] = a[hi - 1], a[hi - 1] = t;
        if (k < i) hi = i - 1;
        else if (k > i) lo = i + 1;
        else return;
    }
    qsort(a + lo, (size_t)(hi - lo + 1), sizeof *a, i64_cmp);
}

/* `.Q.c.mid x` is med's `(iasc x)@floor .5*-1 0+count x` (ref/med.md): a fixed-width vector selects the two middle
 * keys instead of grading, then takes iasc's index of each — the r-th of its key's ties, counted in row order */
ray_t* q_agg_mid(ray_t* x) {
    int64_t n = q_count(x), r[2] = { (n - 1) >> 1, n >> 1 };
    ray_t* out = agg_vec_out(RAY_I64, 2);
    if (RAY_IS_ERR(out)) return out;
    int64_t* o = (int64_t*)ray_data(out);
    if (ray_is_vec(x) && n > 0 && q_attr_letter(x) == 's') { o[0] = r[0], o[1] = r[1]; return out; }
    ray_t* ks = ray_is_vec(x) && n > 0 ? ray_vec_new(RAY_I64, 2 * n) : NULL;
    if (ks && RAY_IS_ERR(ks)) { ray_release(out); return ks; }
    if (ks && !mid_keys(x, (int64_t*)ray_data(ks))) { ray_release(ks); ks = NULL; }
    if (!ks) {
        memcpy(o, r, sizeof r);
        ray_t* g = q_eval_apply_value(q_registry_lookup_name("iasc", 4, Q_MONADIC), &x, 1);
        ray_t* m = RAY_IS_ERR(g) ? g : q_index_at(g, &out, 1);
        if (!RAY_IS_ERR(g)) ray_release(g);
        ray_release(out);
        return m;
    }
    int64_t *k = (int64_t*)ray_data(ks), *s = k + n, v[2], lt[2] = { 0, 0 };
    memcpy(s, k, (size_t)n * sizeof *k);
    i64_select(s, 0, n - 1, r[0]);
    v[0] = v[1] = s[r[0]];
    if (r[1] > r[0]) v[1] = s[r[1]];
    for (int64_t i = r[1] + 1; r[1] > r[0] && i < n; i++) v[1] = s[i] < v[1] ? s[i] : v[1];
    for (int64_t i = 0; i < n; i++) lt[0] += k[i] < v[0], lt[1] += k[i] < v[1];
    for (int j = 0; j < 2; j++)
        for (int64_t i = 0, e = r[j] - lt[j]; i < n; i++)
            if (k[i] == v[j] && e-- == 0) { o[j] = i; break; }
    ray_release(ks);
    return out;
}
