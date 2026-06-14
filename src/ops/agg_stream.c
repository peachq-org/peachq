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

const agg_vtable_t* agg_resolve(uint16_t agg_kind, int8_t in_type) {
    if (agg_kind == OP_SUM && in_type == RAY_I64) return &SUM_I64;
    if (agg_kind == OP_COUNT)                     return &COUNT_ANY;
    if (agg_kind == OP_MIN && in_type == RAY_I64) return &MIN_I64;
    if (agg_kind == OP_MAX && in_type == RAY_I64) return &MAX_I64;
    return NULL;
}
