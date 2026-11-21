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

#ifndef RAY_COW_H
#define RAY_COW_H

/*
 * cow.h -- COW (Copy-on-Write) ref counting.
 *
 * ray_retain: increment reference count
 * ray_release: decrement reference count, free when it reaches zero
 * ray_cow: copy-on-write — return same pointer if sole owner, else copy
 */

#include <rayforce.h>
#include "core/platform.h"
#include "mem/heap.h"   /* RAY_ATTR_ARENA */

/* Thread-local flag: plain (false) vs atomic (true) refcount ops.
 * Default is false (fast single-threaded path).
 * The thread pool sets true before parallel dispatch. */
extern RAY_TLS bool ray_rc_sync;

/* sole owner — what ray_cow tests before it mutates in place, without the consume */
static inline bool ray_rc_sole(ray_t* v) {
    return (RAY_LIKELY(!ray_rc_sync) ? v->rc : ray_atomic_load(&v->rc)) == 1;
}

/* ray_retain and ray_release for a caller that runs them once per evaluated node: the single-threaded count is
 * written in line, and every other case — a count other threads share, the last reference, a null or error
 * pointer, the debug build's stale-pointer check — is the function's. */
static inline bool ray_rc_plain(const ray_t* v) {
#ifdef DEBUG
    (void)v;
    return false;
#else
    return (uintptr_t)v > 31 && v->type != RAY_ERROR && !(v->attrs & RAY_ATTR_ARENA) && !ray_rc_sync;
#endif
}

static inline void ray_retain_inline(ray_t* v) {
    if (RAY_LIKELY(ray_rc_plain(v))) v->rc++;
    else ray_retain(v);
}

static inline void ray_release_inline(ray_t* v) {
    if (RAY_LIKELY(ray_rc_plain(v) && v->rc > 1)) v->rc--;
    else ray_release(v);
}

#endif /* RAY_COW_H */
