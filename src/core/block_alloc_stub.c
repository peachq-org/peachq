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

/* Weak fallback for ray_alloc — replaced by the buddy allocator at link
 * time (src/mem/heap.c).  Lives in its own TU so block.c can be measured
 * for coverage without this dead-by-link path inflating the missed-line
 * count: in any normal build the buddy allocator wins the symbol
 * resolution and this stub is never called.
 *
 * The mmap-backed fallback is used only when building rayforce without
 * the buddy allocator (e.g. a minimal embedding test harness that links
 * just block.o + core helpers).  Keep it standalone so removing or
 * stubbing the buddy allocator yields a still-linkable binary. */

#include "block.h"
#include "core/platform.h"

__attribute__((weak))
ray_t* ray_alloc(size_t size) {
    if (size < 32) size = 32;
    size = (size + 4095) & ~(size_t)4095;
    void* p = ray_vm_alloc(size);
    if (!p) return ray_error("oom", NULL);
    return (ray_t*)p;
}
