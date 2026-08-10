/* rand — THE owner of pseudo-random words: every random draw in the process
 * comes from here, so `\S` reaches all of them.  Streams are per-thread; the
 * thread that seeds owns the reproducible one. */
#ifndef RAY_CORE_RAND_H
#define RAY_CORE_RAND_H

#include <stdint.h>

/* Reseed the CALLING thread's stream and publish n as the seed other threads'
 * streams derive from; `\S n` is its only q surface (basics/syscmds.md). */
void ray_rand_seed(int64_t n);

uint64_t ray_rand_u64(void);
uint64_t ray_rand_below(uint64_t m); /* uniform [0,m), m>0 — unbiased */

#endif /* RAY_CORE_RAND_H */
