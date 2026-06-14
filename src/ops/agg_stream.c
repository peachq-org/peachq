/* src/ops/agg_stream.c — streaming accumulators (design §4.5).
 * Inner loops mirror group.c REDUCE_LOOP_I/F but carry ONLY this aggregate's
 * state, recovering the rowform density win generically. */
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include <math.h>

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

const agg_vtable_t* agg_resolve(uint16_t agg_kind, int8_t in_type) {
    if (agg_kind == OP_SUM && in_type == RAY_I64) return &SUM_I64;
    return NULL;
}
