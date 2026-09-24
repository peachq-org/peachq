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

#include "domain.h"
#include <stdio.h>
#include <stdlib.h>

struct ray_sym_domain_s {
    uint8_t unused;
};

static struct ray_sym_domain_s g_runtime_domain;

ray_sym_domain_t* ray_sym_runtime_domain(void) {
    return &g_runtime_domain;
}

void ray_sym_domain_retain(ray_sym_domain_t* dom) {
    (void)dom;
}

void ray_sym_domain_release(ray_sym_domain_t* dom) {
    (void)dom;
}

ray_t* ray_sym_domain_str(ray_sym_domain_t* dom, int64_t pos) {
    return dom ? ray_sym_str(pos) : NULL;
}

int64_t ray_sym_domain_find(ray_sym_domain_t* dom, const char* str, size_t len) {
    return dom && str ? ray_sym_find(str, len) : -1;
}

const int64_t* ray_sym_domain_runtime_lut(ray_sym_domain_t* dom) {
    (void)dom;
    return NULL;
}

int64_t ray_sym_domain_count(ray_sym_domain_t* dom) {
    return dom ? (int64_t)ray_sym_count() : 0;
}

uint8_t ray_g_sym_audit = 0;

void ray_sym_audit_cell(ray_t* vec, int64_t row, int64_t pos, ray_t* resolved) {
    ray_sym_domain_t* dom = ray_sym_vec_domain(vec);
    int64_t count = ray_sym_domain_count(dom);
    if (resolved != NULL && pos >= 0 && pos < count) return;

    fprintf(stderr,
            "rayforce: RAY_SYM_AUDIT violation: vec=%p type=%d attrs=0x%02x "
            "len=%lld row=%lld pos=%lld count=%lld resolved=%p\n",
            (void*)vec, (int)vec->type, (unsigned)vec->attrs,
            (long long)vec->len, (long long)row, (long long)pos,
            (long long)count, (void*)resolved);
    abort();
}
