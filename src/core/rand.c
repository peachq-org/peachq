/* PCG64-DXSM (see rand.h): 128-bit LCG stepped by the CHEAP 64-bit multiplier —
 * which is what makes it DXSM rather than pcg64 XSL-RR — then the DXSM output
 * permutation.  Drawn by the q `?` surface (qlang/ops/q_rand.c), the roll kernel
 * (ops/collection.c) and the guid kernel (ops/system.c).
 *
 * State is per-thread, so no draw contends and the guid kernel keeps the
 * thread-locality it owned before the streams merged.  The epoch is what lets
 * `\S` REACH a stream the seeding thread does not own: a thread with a stale
 * epoch reseeds from the current seed on its next draw, which the old
 * srand()/xorshift split could not do (PLAN.md, "`\S` does not reach the guid
 * stream").  REPRODUCIBILITY is the seeding thread's alone — it always takes
 * stream 0, while other threads take monotonically increasing ids, because two
 * threads handed the SAME id would emit identical sequences.  Nothing is lost:
 * parallel work assignment is not reproducible either, and no q-observable draw
 * happens off the seeding thread. */
#include "core/rand.h"
#include "core/platform.h" /* RAY_UNLIKELY */

#define PCG_MUL 15750249268501108917ULL /* 0xda942042e4dd58b5 — PCG's cheap multiplier */

static __thread __uint128_t rng_state, rng_inc;
static __thread uint32_t rng_epoch; /* 0 = this thread has no stream yet */

static _Atomic(int64_t) seed_word;    /* 0 until seeded: the rayforce binary never calls `\S` */
static _Atomic(uint32_t) seed_epoch = 1;
static _Atomic(uint32_t) seed_stream = 1; /* stream 0 is the seeding thread's */

static uint64_t pcg_step(void) {
    __uint128_t s = rng_state;
    rng_state = s * (__uint128_t)PCG_MUL + rng_inc;
    uint64_t hi = (uint64_t)(s >> 64), lo = (uint64_t)s | 1u;
    hi ^= hi >> 32;
    hi *= PCG_MUL;
    hi ^= hi >> 48;
    return hi * lo;
}

static void stream_seed(int64_t seed, uint64_t stream, uint32_t epoch) {
    rng_inc = (((__uint128_t)stream) << 1) | 1u;
    rng_state = (__uint128_t)(uint64_t)seed + rng_inc;
    rng_epoch = epoch;
    (void)pcg_step();
}

void ray_rand_seed(int64_t n) {
    seed_word = n;
    stream_seed(n, 0, ++seed_epoch);
}

uint64_t ray_rand_u64(void) {
    uint32_t e = seed_epoch;
    if (RAY_UNLIKELY(rng_epoch != e))
        stream_seed(seed_word, seed_stream++, e);
    return pcg_step();
}

/* Lemire: the 128-bit product's high word IS the draw; only a low word under
 * the (-m)%m threshold can bias it, so those alone are rejected. */
uint64_t ray_rand_below(uint64_t m) {
    __uint128_t w = (__uint128_t)ray_rand_u64() * m;
    uint64_t lo = (uint64_t)w;
    if (RAY_UNLIKELY(lo < m)) {
        uint64_t t = (0 - m) % m;
        while (lo < t) {
            w = (__uint128_t)ray_rand_u64() * m;
            lo = (uint64_t)w;
        }
    }
    return (uint64_t)(w >> 64);
}
