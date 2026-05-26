/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_OPS_HLL_H
#define RAY_OPS_HLL_H

/**
 * Probabilistic cardinality sketch (HyperLogLog).
 *
 * Each sketch holds 2^P registers; each register stores the maximum
 * leading-zero count (rho) seen for any hash whose top P bits index
 * that register.  Cardinality is then read off the harmonic mean of
 * 2^reg over all registers, with bias correction for both ends of
 * the range.  Standard error ≈ 1.04 / sqrt(2^P).  P=14 → ≈ 0.8 %.
 *
 * Memory: 1 byte per register (8-bit reg holds rho up to 64+P, way
 * over the 6 bits a packed implementation would need; the extra few
 * KB buys a tighter hot loop).  At P=14 a sketch is 16 KB and lives
 * in L2 for the duration of one query.
 *
 * Sparse representation:
 *   Per-group HLL at high group counts wants to amortise the 16 KB
 *   sketch across groups that may only see a handful of hashes each
 *   (q13 SearchPhrase × UserID: many groups with < 50 uniques).  In
 *   sparse mode the sketch stores only the registers that have been
 *   written, as 32-bit `(reg_idx << 8) | rho` entries in a small
 *   caller-provided buffer.  The estimate / merge paths transparently
 *   support both modes; sparse converts to dense when the entry count
 *   exceeds the cap (caller-supplied; the per-group kernel uses 256).
 *
 * The sketch is mergeable element-wise (max), which is the property
 * the per-group / per-worker aggregation paths rely on: each worker
 * builds a local sketch and the planner merges them at finalisation.
 */

#include "rayforce.h"
#include "core/platform.h"
#include "ops/hash.h"

/* Default precision: 14 (16384 registers, ~0.81 % std error, 16 KB). */
#define RAY_HLL_DEFAULT_P  14

/* Sparse cap for per-group sketches.  Each entry is 4 bytes, so the
 * sparse buffer is 1 KB at this cap — well inside L1 and 16× smaller
 * than the dense register array.  Above the cap, sparse is converted
 * to dense in place (caller supplies both buffers on the stack). */
#define RAY_HLL_SPARSE_CAP 256

typedef struct {
    uint8_t   p;             /* precision: register count = 1 << p */
    uint32_t  m;             /* register count */
    uint8_t*  regs;          /* dense: [m] register array (NULL in sparse mode) */
    /* Sparse mode (active when sparse_keys != NULL && regs == NULL):
     * sparse_keys[i] = (reg_idx << 8) | rho — unsorted linear-probe set
     * over reg_idx (rho updated in-place on duplicate idx). */
    uint32_t* sparse_keys;
    uint32_t  sparse_count;
    uint32_t  sparse_cap;
    ray_t*    _hdr;          /* scratch handle for regs (sparse uses caller buf) */
} ray_hll_t;

/* Initialise an empty *dense* sketch with `p` precision bits.  Allocates
 * regs via scratch_alloc; the caller frees with ray_hll_free.  Returns
 * 0 on success, -1 on OOM. */
int  ray_hll_init(ray_hll_t* h, uint8_t p);

/* Initialise an empty *sparse* sketch with caller-provided buffers.
 *   sparse_buf — buffer of size sparse_cap entries, used as the sparse
 *                set until conversion to dense.
 *   dense_buf  — buffer of size 1<<p bytes, populated on conversion.
 * Both buffers are typically stack-allocated by the worker task.  The
 * sketch starts sparse (regs == NULL).  No allocation occurs; this
 * never fails.  Caller does not need to call ray_hll_free. */
void ray_hll_init_sparse(ray_hll_t* h, uint8_t p,
                          uint32_t* sparse_buf, uint32_t sparse_cap,
                          uint8_t* dense_buf);

/* Free the regs allocation.  Safe on a zeroed (uninitialised) sketch.
 * Sparse sketches with caller-provided buffers have _hdr == NULL and
 * are a no-op here — they're freed implicitly when the stack frame
 * unwinds. */
void ray_hll_free(ray_hll_t* h);

/* Zero all registers (clears the sketch — same effect as init with the
 * same p, but in-place; useful when reusing a sketch across calls).
 * Resets to sparse mode if a sparse buffer is attached. */
void ray_hll_reset(ray_hll_t* h);

/* Sparse → dense conversion.  Replays sparse_keys into the (already-
 * attached) dense buffer, zeros remaining registers, clears sparse_count.
 * Out-of-line: only called when the sparse cap is hit. */
void ray_hll_promote_to_dense(ray_hll_t* h);

/* Add a 64-bit hash to the sketch.  Caller is responsible for hashing
 * its value type before invoking — see ray_hash_i64 / ray_hash_bytes
 * in ops/hash.h.  Hot path; kept fully inline.  Dense fast path is
 * marked likely; the sparse arm is the fallback for per-group sketches
 * that haven't yet exceeded RAY_HLL_SPARSE_CAP. */
static inline void ray_hll_add(ray_hll_t* h, uint64_t hash) {
    uint32_t idx = (uint32_t)(hash >> (64u - h->p));
    /* The low (64-p) bits hold the value we scan for the leading-zero
     * run.  Sentinel-bit at position (64-p-1) keeps the rho value in
     * [1, 64-p+1] without a branch on all-zero. */
    uint64_t rest = (hash << h->p) | (1ULL << (h->p - 1));
    uint8_t  rho  = (uint8_t)(__builtin_clzll(rest) + 1u);

    if (RAY_LIKELY(h->regs != NULL)) {
        if (rho > h->regs[idx]) h->regs[idx] = rho;
        return;
    }
    /* Sparse path — linear scan over up to RAY_HLL_SPARSE_CAP entries.
     * Cap is small (256) so the inner loop is L1-resident; the compiler
     * folds it into a SIMD-friendly compare-and-mask sequence. */
    uint32_t* sk = h->sparse_keys;
    uint32_t  n  = h->sparse_count;
    uint32_t  enc = (idx << 8) | rho;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cur = sk[i];
        if ((cur >> 8) == idx) {
            /* Same register — keep max rho. */
            if (rho > (cur & 0xFF)) sk[i] = enc;
            return;
        }
    }
    if (n < h->sparse_cap) {
        sk[n] = enc;
        h->sparse_count = n + 1;
        return;
    }
    /* Cap hit — promote and re-insert. */
    ray_hll_promote_to_dense(h);
    if (rho > h->regs[idx]) h->regs[idx] = rho;
}

/* Merge src into dst (element-wise max).  src and dst must share the
 * same precision p.  Handles all four (dense/sparse)×(dense/sparse)
 * combinations; sparse+sparse promotes dst to dense first so the
 * merged sketch remains a valid dense register array. */
void ray_hll_merge(ray_hll_t* dst, const ray_hll_t* src);

/* Estimate the unique-value count of all hashes added so far.  Uses
 * the standard HyperLogLog estimator with bias-corrected raw-mean for
 * the mid-range and linear counting (m * ln(m/V)) when many registers
 * are still zero (V = unused register count).  Branches on mode:
 * dense scans the register array; sparse iterates the entry set and
 * accounts for (m - sparse_count) unset registers analytically. */
int64_t ray_hll_estimate(const ray_hll_t* h);

/* Scalar approximate `count(distinct …)` over a vec, ~0.8 % standard
 * error.  Handles I64/I32/I16/I8/U8/BOOL/F64/DATE/TIME/TIMESTAMP/SYM/
 * STR.  Nulls are skipped (matches the SQL `count distinct` semantics).
 * Parallelised: each worker builds a private sketch over its row range
 * and the main thread merges them before extracting the estimate.
 * Wired into `exec_count_distinct` above an input-row threshold. */
ray_t* ray_count_distinct_approx(ray_t* x);

/* Per-group approximate `count(distinct …)` over a buffered row-index
 * layout: group g owns the row indices
 *   idx_buf[offsets[g] .. offsets[g] + counts[g]).
 * Parallelised across groups — each task uses a private stack-resident
 * HLL that starts in sparse mode (1 KB) and converts to dense (16 KB)
 * on overflow.  Sparse mode keeps the memset / estimate cost bounded
 * by `min(unique_in_group, sparse_cap)` instead of m, which is the
 * decisive win at high group counts where the average group has few
 * unique values.
 *
 * Callers holding a row_gid layout instead build idx_buf+offsets+counts
 * once and call this; there's a single per-group kernel.  Writes the
 * estimate to out[gid].  Returns 0 on success, -1 on unsupported type
 * (caller falls back to exact). */
int ray_count_distinct_approx_pg_buf(ray_t* src,
                                      const int64_t* idx_buf,
                                      const int64_t* offsets,
                                      const int64_t* counts,
                                      int64_t n_groups,
                                      uint8_t p, int64_t* out);

#endif /* RAY_OPS_HLL_H */
