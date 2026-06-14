/* src/ops/agg_stream.c — streaming accumulators (design §4.5).
 * Inner loops mirror group.c REDUCE_LOOP_I/F but carry ONLY this aggregate's
 * state, recovering the rowform density win generically. */
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include <math.h>
#include <stdint.h>

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

const agg_vtable_t* agg_resolve(uint16_t agg_kind, int8_t in_type) {
    if (agg_kind == OP_SUM && in_type == RAY_I64) return &SUM_I64;
    if (agg_kind == OP_COUNT)                     return &COUNT_ANY;
    if (agg_kind == OP_MIN && in_type == RAY_I64) return &MIN_I64;
    if (agg_kind == OP_MAX && in_type == RAY_I64) return &MAX_I64;
    if (agg_kind == OP_SUM && in_type == RAY_F64) return &SUM_F64;
    if (agg_kind == OP_MIN && in_type == RAY_F64) return &MIN_F64;
    if (agg_kind == OP_MAX && in_type == RAY_F64) return &MAX_F64;
    if (agg_kind == OP_AVG && in_type == RAY_F64) return &AVG_F64;
    return NULL;
}
