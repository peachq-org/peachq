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

#ifndef RAY_VEC_H
#define RAY_VEC_H

/*
 * vec.h -- Vector operations.
 *
 * Vectors are ray_t blocks with positive type tags. Data follows the 32-byte
 * header. Supports append, get, set, slice (zero-copy), concat. Null state
 * is encoded in-band via type-correct NULL_* sentinels (see vec.c).
 */

#include <rayforce.h>

/* Copy null bits from src to dst (sentinel-based). dst and src must have
 * the same length. Internal helper. */
ray_err_t ray_vec_copy_nulls(ray_t* dst, const ray_t* src);

/* ===== Null-model invariant validator (review item §2.1, invariant 16.4) =====
 *
 * Invariant 16.4: if a typed fixed-width column's payload contains a
 * type-correct *reserved* null sentinel (NULL_I64 = INT64_MIN, NULL_F64 =
 * any NaN, etc.), RAY_ATTR_HAS_NULLS MUST be set.  Downstream fast paths
 * gate null handling on that bit (ray_vec_is_null returns false outright
 * when it is clear), so a sentinel without the bit is a latent correctness
 * bug: null-aware ops silently treat the sentinel as an ordinary value.
 *
 * This is a DEBUG/ASAN-only enforcing mechanism.  In a release build the
 * function is compiled out entirely (the prototype expands to nothing) so
 * there is zero cost on the hot path.  It is wired into the ray_eval result
 * boundary (src/lang/eval.c) so the whole test suite exercises it across
 * every tested producer.
 *
 * Returns true (invariant holds / no-op) for: types with no reserved
 * sentinel (BOOL/U8); SYM (no-null by design — see ray_vec_is_null);
 * STR/GUID (empty string / zero-guid is a legitimate ordinary value, not a
 * reserved sentinel); slices (don't own the payload or carry HAS_NULLS);
 * zero-length vectors; LIST/TABLE/DICT are recursed into (cheaply).  On a
 * genuine violation it emits a diagnostic and aborts so it surfaces as a
 * test failure with a usable message. */
#ifdef DEBUG
bool ray_check_null_invariant(const ray_t* v);
#else
#define ray_check_null_invariant(v) ((void)0)
#endif

#endif /* RAY_VEC_H */
