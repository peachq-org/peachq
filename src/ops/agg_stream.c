/* src/ops/agg_stream.c — streaming accumulators (design §4.5).
 * Inner loops mirror group.c REDUCE_LOOP_I/F but carry ONLY this aggregate's
 * state, recovering the rowform density win generically. */
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include "lang/internal.h"  /* ray_median_dbl_inplace */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>         /* realloc/free for the buffered median accumulator */

/* ---- sum, I64 -------------------------------------------------------- */
typedef struct { int64_t sum; } sum_i64_state;

static void sum_i64_init(void* s) { ((sum_i64_state*)s)->sum = 0; }

static void sum_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* arena) {
    (void)arena;
    const int64_t* d = (const int64_t*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        sum_i64_state* st = (sum_i64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum = (int64_t)((uint64_t)st->sum + (uint64_t)d[i]); /* unsigned wrap: group.c:185 */
    }
}

static void sum_i64_merge(void* dst, const void* src, acc_arena_t* a) {
    (void)a;
    ((sum_i64_state*)dst)->sum = (int64_t)((uint64_t)((sum_i64_state*)dst)->sum
                                         + (uint64_t)((const sum_i64_state*)src)->sum);
}

static ray_t* sum_i64_final(const void* s, acc_arena_t* a) {
    (void)a; return ray_i64(((const sum_i64_state*)s)->sum);
}

static const agg_vtable_t SUM_I64 = {
    .state_size = sizeof(sum_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = sum_i64_init, .update_batch = sum_i64_update,
    .merge = sum_i64_merge, .finalize = sum_i64_final,
};

/* ---- count (type-agnostic over live rows) ---------------------------- */
typedef struct { int64_t n; } count_state;
static void count_init(void* s) { ((count_state*)s)->n = 0; }
static void count_update(void* base, size_t stride, const uint32_t* gids,
                         const void* vals, const ray_valid_t* valid,
                         int64_t n, acc_arena_t* a) {
    (void)vals; (void)a;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        ((count_state*)((char*)base + (size_t)gids[i]*stride))->n++;
    }
}
static void count_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; ((count_state*)d)->n += ((const count_state*)s)->n;
}
static ray_t* count_final(const void* s, acc_arena_t* a) {
    (void)a; return ray_i64(((const count_state*)s)->n);
}
static const agg_vtable_t COUNT_ANY = {
    .state_size = sizeof(count_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = count_init, .update_batch = count_update,
    .merge = count_merge, .finalize = count_final,
};

/* ---- min / max I64 (empty group → typed null) ------------------------ */
typedef struct { int64_t v; int64_t cnt; } ext_i64_state;
static void min_i64_init(void* s) { ((ext_i64_state*)s)->v = INT64_MAX; ((ext_i64_state*)s)->cnt = 0; }
static void max_i64_init(void* s) { ((ext_i64_state*)s)->v = INT64_MIN; ((ext_i64_state*)s)->cnt = 0; }
static void min_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        ext_i64_state* st = (ext_i64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->cnt++;
    }
}
static void max_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        ext_i64_state* st = (ext_i64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->cnt++;
    }
}
static void min_i64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_i64_state* src = s; ext_i64_state* dst = d;
    if (src->cnt && src->v < dst->v) { dst->v = src->v; }
    dst->cnt += src->cnt;
}
static void max_i64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_i64_state* src = s; ext_i64_state* dst = d;
    if (src->cnt && src->v > dst->v) { dst->v = src->v; }
    dst->cnt += src->cnt;
}
static ray_t* ext_i64_final(const void* s, acc_arena_t* a) {
    (void)a; const ext_i64_state* st = s;
    return st->cnt ? ray_i64(st->v) : ray_typed_null(-RAY_I64);
}
static const agg_vtable_t MIN_I64 = {
    .state_size = sizeof(ext_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = min_i64_init, .update_batch = min_i64_update,
    .merge = min_i64_merge, .finalize = ext_i64_final,
};
static const agg_vtable_t MAX_I64 = {
    .state_size = sizeof(ext_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = max_i64_init, .update_batch = max_i64_update,
    .merge = max_i64_merge, .finalize = ext_i64_final,
};

/* ---- sum, F64 -------------------------------------------------------- */
typedef struct { double sum; } sum_f64_state;
static void sum_f64_init(void* s) { ((sum_f64_state*)s)->sum = 0.0; }
static void sum_f64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const double* d = (const double*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        ((sum_f64_state*)((char*)base + (size_t)gids[i]*stride))->sum += d[i];
    }
}
static void sum_f64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; ((sum_f64_state*)d)->sum += ((const sum_f64_state*)s)->sum;
}
static ray_t* sum_f64_final(const void* s, acc_arena_t* a) {
    (void)a; return ray_f64(((const sum_f64_state*)s)->sum);
}
static const agg_vtable_t SUM_F64 = {
    .state_size = sizeof(sum_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = sum_f64_init, .update_batch = sum_f64_update,
    .merge = sum_f64_merge, .finalize = sum_f64_final,
};

/* ---- min / max F64 (empty group → typed null) ------------------------ */
typedef struct { double v; int64_t cnt; } ext_f64_state;
static void min_f64_init(void* s) { ((ext_f64_state*)s)->v = INFINITY;  ((ext_f64_state*)s)->cnt = 0; }
static void max_f64_init(void* s) { ((ext_f64_state*)s)->v = -INFINITY; ((ext_f64_state*)s)->cnt = 0; }
static void min_f64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const double* d = (const double*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        ext_f64_state* st = (ext_f64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->cnt++;
    }
}
static void max_f64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const double* d = (const double*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        ext_f64_state* st = (ext_f64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->cnt++;
    }
}
static void min_f64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_f64_state* src = s; ext_f64_state* dst = d;
    if (src->cnt && src->v < dst->v) { dst->v = src->v; }
    dst->cnt += src->cnt;
}
static void max_f64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_f64_state* src = s; ext_f64_state* dst = d;
    if (src->cnt && src->v > dst->v) { dst->v = src->v; }
    dst->cnt += src->cnt;
}
static ray_t* ext_f64_final(const void* s, acc_arena_t* a) {
    (void)a; const ext_f64_state* st = s;
    return st->cnt ? ray_f64(st->v) : ray_typed_null(-RAY_F64);
}
static const agg_vtable_t MIN_F64 = {
    .state_size = sizeof(ext_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = min_f64_init, .update_batch = min_f64_update,
    .merge = min_f64_merge, .finalize = ext_f64_final,
};
static const agg_vtable_t MAX_F64 = {
    .state_size = sizeof(ext_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = max_f64_init, .update_batch = max_f64_update,
    .merge = max_f64_merge, .finalize = ext_f64_final,
};

/* ---- avg, F64 -------------------------------------------------------- */
typedef struct { double sum; int64_t cnt; } avg_f64_state;
static void avg_f64_init(void* s) { ((avg_f64_state*)s)->sum = 0.0; ((avg_f64_state*)s)->cnt = 0; }
static void avg_f64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const double* d = (const double*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        avg_f64_state* st = (avg_f64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum += d[i]; st->cnt++;
    }
}
static void avg_f64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; ((avg_f64_state*)d)->sum += ((const avg_f64_state*)s)->sum;
    ((avg_f64_state*)d)->cnt += ((const avg_f64_state*)s)->cnt;
}
static ray_t* avg_f64_final(const void* s, acc_arena_t* a) {
    (void)a; const avg_f64_state* st = s;
    return st->cnt ? ray_f64(st->sum / (double)st->cnt) : ray_typed_null(-RAY_F64);
}
static const agg_vtable_t AVG_F64 = {
    .state_size = sizeof(avg_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = avg_f64_init, .update_batch = avg_f64_update,
    .merge = avg_f64_merge, .finalize = avg_f64_final,
};

/* ---- variance family, I64 (sumsq as int64 unsigned-wrap; formula group.c:2190) -- */
typedef struct { double sum; int64_t sumsq; int64_t cnt; } var_i64_state;
static void var_i64_init(void* s) { var_i64_state* st = s; st->sum = 0; st->sumsq = 0; st->cnt = 0; }
static void var_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        var_i64_state* st = (var_i64_state*)((char*)base + (size_t)gids[i]*stride);
        int64_t v = d[i]; st->sum += (double)v;
        st->sumsq = (int64_t)((uint64_t)st->sumsq + (uint64_t)v*(uint64_t)v); /* wrap: group.c:185 */
        st->cnt++;
    }
}
static void var_i64_merge(void* dd, const void* ss, acc_arena_t* a) {
    (void)a; var_i64_state* d = dd; const var_i64_state* s = ss;
    d->sum += s->sum; d->sumsq = (int64_t)((uint64_t)d->sumsq + (uint64_t)s->sumsq); d->cnt += s->cnt;
}
static inline double var_i64_varpop(const var_i64_state* st) {
    double mean = st->sum / (double)st->cnt;
    double vp = (double)st->sumsq / (double)st->cnt - mean*mean;
    return vp < 0 ? 0 : vp;
}
static ray_t* fin_var_pop_i64(const void* s, acc_arena_t* a) {
    (void)a; const var_i64_state* st = s;
    if (st->cnt <= 0) return ray_typed_null(-RAY_F64);
    return ray_f64(var_i64_varpop(st));
}
static ray_t* fin_var_i64(const void* s, acc_arena_t* a) {
    (void)a; const var_i64_state* st = s;
    if (st->cnt <= 1) return ray_typed_null(-RAY_F64);
    return ray_f64(var_i64_varpop(st) * (double)st->cnt / ((double)st->cnt - 1.0));
}
static ray_t* fin_stddev_pop_i64(const void* s, acc_arena_t* a) {
    (void)a; const var_i64_state* st = s;
    if (st->cnt <= 0) return ray_typed_null(-RAY_F64);
    return ray_f64(sqrt(var_i64_varpop(st)));
}
static ray_t* fin_stddev_i64(const void* s, acc_arena_t* a) {
    (void)a; const var_i64_state* st = s;
    if (st->cnt <= 1) return ray_typed_null(-RAY_F64);
    return ray_f64(sqrt(var_i64_varpop(st) * (double)st->cnt / ((double)st->cnt - 1.0)));
}
static const agg_vtable_t VAR_I64 = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i64_update,
    .merge = var_i64_merge, .finalize = fin_var_i64,
};
static const agg_vtable_t VAR_POP_I64 = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i64_update,
    .merge = var_i64_merge, .finalize = fin_var_pop_i64,
};
static const agg_vtable_t STDDEV_I64 = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i64_update,
    .merge = var_i64_merge, .finalize = fin_stddev_i64,
};
static const agg_vtable_t STDDEV_POP_I64 = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i64_update,
    .merge = var_i64_merge, .finalize = fin_stddev_pop_i64,
};

/* ---- variance family, F64 (sumsq as double) -------------------------- */
typedef struct { double sum; double sumsq; int64_t cnt; } var_f64_state;
static void var_f64_init(void* s) { var_f64_state* st = s; st->sum = 0; st->sumsq = 0; st->cnt = 0; }
static void var_f64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const double* d = (const double*)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        var_f64_state* st = (var_f64_state*)((char*)base + (size_t)gids[i]*stride);
        double v = d[i]; st->sum += v; st->sumsq += v*v; st->cnt++;
    }
}
static void var_f64_merge(void* dd, const void* ss, acc_arena_t* a) {
    (void)a; var_f64_state* d = dd; const var_f64_state* s = ss;
    d->sum += s->sum; d->sumsq += s->sumsq; d->cnt += s->cnt;
}
static inline double var_f64_varpop(const var_f64_state* st) {
    double mean = st->sum / (double)st->cnt;
    double vp = st->sumsq / (double)st->cnt - mean*mean;
    return vp < 0 ? 0 : vp;
}
static ray_t* fin_var_pop_f64(const void* s, acc_arena_t* a) {
    (void)a; const var_f64_state* st = s;
    if (st->cnt <= 0) return ray_typed_null(-RAY_F64);
    return ray_f64(var_f64_varpop(st));
}
static ray_t* fin_var_f64(const void* s, acc_arena_t* a) {
    (void)a; const var_f64_state* st = s;
    if (st->cnt <= 1) return ray_typed_null(-RAY_F64);
    return ray_f64(var_f64_varpop(st) * (double)st->cnt / ((double)st->cnt - 1.0));
}
static ray_t* fin_stddev_pop_f64(const void* s, acc_arena_t* a) {
    (void)a; const var_f64_state* st = s;
    if (st->cnt <= 0) return ray_typed_null(-RAY_F64);
    return ray_f64(sqrt(var_f64_varpop(st)));
}
static ray_t* fin_stddev_f64(const void* s, acc_arena_t* a) {
    (void)a; const var_f64_state* st = s;
    if (st->cnt <= 1) return ray_typed_null(-RAY_F64);
    return ray_f64(sqrt(var_f64_varpop(st) * (double)st->cnt / ((double)st->cnt - 1.0)));
}
static const agg_vtable_t VAR_F64 = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f64_update,
    .merge = var_f64_merge, .finalize = fin_var_f64,
};
static const agg_vtable_t VAR_POP_F64 = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f64_update,
    .merge = var_f64_merge, .finalize = fin_var_pop_f64,
};
static const agg_vtable_t STDDEV_F64 = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f64_update,
    .merge = var_f64_merge, .finalize = fin_stddev_f64,
};
static const agg_vtable_t STDDEV_POP_F64 = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f64_update,
    .merge = var_f64_merge, .finalize = fin_stddev_pop_f64,
};

/* ---- pearson correlation, F64 (first BINARY aggregate: x,y) ----------- */
typedef struct { double sx, sy, sxx, syy, sxy; int64_t n; } pearson_state;
static void pearson_init(void* s) {
    pearson_state* st = s; st->sx = st->sy = st->sxx = st->syy = st->sxy = 0; st->n = 0;
}
static void pearson_update2(void* base, size_t stride, const uint32_t* gids,
                            const void* vx, const void* vy,
                            const ray_valid_t* valx, const ray_valid_t* valy,
                            int64_t n, acc_arena_t* a) {
    (void)a; const double* x = vx; const double* y = vy;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valx, i) || !ray_valid_at(valy, i)) continue;
        pearson_state* st = (pearson_state*)((char*)base + (size_t)gids[i]*stride);
        double xi = x[i], yi = y[i];
        st->sx += xi; st->sy += yi; st->sxx += xi*xi; st->syy += yi*yi; st->sxy += xi*yi; st->n++;
    }
}
static void pearson_merge(void* dd, const void* ss, acc_arena_t* a) {
    (void)a; pearson_state* d = dd; const pearson_state* s = ss;
    d->sx += s->sx; d->sy += s->sy; d->sxx += s->sxx; d->syy += s->syy; d->sxy += s->sxy; d->n += s->n;
}
static ray_t* pearson_final(const void* s, acc_arena_t* a) {
    (void)a; const pearson_state* st = s;
    double dn = (double)st->n;
    double num = dn*st->sxy - st->sx*st->sy,
           dx  = dn*st->sxx - st->sx*st->sx,
           dy  = dn*st->syy - st->sy*st->sy;
    return ray_f64(num / sqrt(dx*dy));
}
static const agg_vtable_t PEARSON_F64 = {
    .state_size = sizeof(pearson_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = pearson_init, .update_batch2 = pearson_update2,
    .merge = pearson_merge, .finalize = pearson_final,
};

/* ---- median, F64 output (first ACC_BUFFERED: growable per-group buffer) ---- */
typedef struct { double* buf; int64_t len; int64_t cap; } median_state;
static void median_init(void* s){ median_state* st=s; st->buf=NULL; st->len=0; st->cap=0; }
static inline void median_push(median_state* st, double v){
    if (st->len == st->cap){ int64_t nc = st->cap ? st->cap*2 : 8;
        double* nb = realloc(st->buf, (size_t)nc*sizeof(double)); st->buf=nb; st->cap=nc; }
    st->buf[st->len++] = v;
}
static void median_update_i64(void* base, size_t stride, const uint32_t* gids,
                              const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a){
    (void)a; const int64_t* d=vals;
    for (int64_t i=0;i<n;i++){ if(!ray_valid_at(valid,i))continue;
        median_push((median_state*)((char*)base+(size_t)gids[i]*stride),(double)d[i]); }
}
static void median_update_f64(void* base, size_t stride, const uint32_t* gids,
                              const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a){
    (void)a; const double* d=vals;
    for (int64_t i=0;i<n;i++){ if(!ray_valid_at(valid,i))continue;
        median_push((median_state*)((char*)base+(size_t)gids[i]*stride), d[i]); }
}
static void median_merge(void* dd, const void* ss, acc_arena_t* a){ (void)a;
    median_state* d=dd; const median_state* s=ss;
    for (int64_t i=0;i<s->len;i++) median_push(d, s->buf[i]); }
static ray_t* median_final(const void* s, acc_arena_t* a){ (void)a; median_state* st=(median_state*)s;
    if (st->len==0) return ray_typed_null(-RAY_F64);
    return ray_f64(ray_median_dbl_inplace(st->buf, st->len)); }
static void median_destroy(void* s){ median_state* st=s; free(st->buf); st->buf=NULL; st->len=st->cap=0; }
static const agg_vtable_t MEDIAN_I64 = { .state_size=sizeof(median_state), .kind=ACC_BUFFERED, .out_type=RAY_F64,
    .init=median_init, .update_batch=median_update_i64, .merge=median_merge, .finalize=median_final, .destroy=median_destroy };
static const agg_vtable_t MEDIAN_F64 = { .state_size=sizeof(median_state), .kind=ACC_BUFFERED, .out_type=RAY_F64,
    .init=median_init, .update_batch=median_update_f64, .merge=median_merge, .finalize=median_final, .destroy=median_destroy };

const agg_vtable_t* agg_resolve(uint16_t agg_kind, int8_t in_type) {
    if (agg_kind == OP_MEDIAN && in_type == RAY_I64) return &MEDIAN_I64;
    if (agg_kind == OP_MEDIAN && in_type == RAY_F64) return &MEDIAN_F64;
    if (agg_kind == OP_PEARSON_CORR && in_type == RAY_F64) return &PEARSON_F64;
    if (agg_kind == OP_SUM && in_type == RAY_I64) return &SUM_I64;
    if (agg_kind == OP_COUNT)                     return &COUNT_ANY;
    if (agg_kind == OP_MIN && in_type == RAY_I64) return &MIN_I64;
    if (agg_kind == OP_MAX && in_type == RAY_I64) return &MAX_I64;
    if (agg_kind == OP_SUM && in_type == RAY_F64) return &SUM_F64;
    if (agg_kind == OP_MIN && in_type == RAY_F64) return &MIN_F64;
    if (agg_kind == OP_MAX && in_type == RAY_F64) return &MAX_F64;
    if (agg_kind == OP_AVG && in_type == RAY_F64) return &AVG_F64;
    if (in_type == RAY_I64) {
        if (agg_kind == OP_VAR) return &VAR_I64;
        if (agg_kind == OP_VAR_POP) return &VAR_POP_I64;
        if (agg_kind == OP_STDDEV) return &STDDEV_I64;
        if (agg_kind == OP_STDDEV_POP) return &STDDEV_POP_I64;
    } else if (in_type == RAY_F64) {
        if (agg_kind == OP_VAR) return &VAR_F64;
        if (agg_kind == OP_VAR_POP) return &VAR_POP_F64;
        if (agg_kind == OP_STDDEV) return &STDDEV_F64;
        if (agg_kind == OP_STDDEV_POP) return &STDDEV_POP_F64;
    }
    return NULL;
}
