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

#ifndef RAY_DOMAIN_H
#define RAY_DOMAIN_H

/* The dictionary a RAY_SYM vector's cells resolve over.  The only domain is
 * the immortal RUNTIME singleton wrapping the global intern table (sym.c);
 * the API keeps its domain argument so a cell still resolves through the
 * vector that holds it. */

#include <rayforce.h>
#include <stdbool.h>

typedef struct ray_sym_domain_s ray_sym_domain_t;

ray_sym_domain_t* ray_sym_runtime_domain(void);
void ray_sym_domain_retain(ray_sym_domain_t* dom);
void ray_sym_domain_release(ray_sym_domain_t* dom);
ray_t* ray_sym_domain_str(ray_sym_domain_t* dom, int64_t pos);
int64_t ray_sym_domain_find(ray_sym_domain_t* dom, const char* str, size_t len);
int64_t ray_sym_domain_count(ray_sym_domain_t* dom);

/* Always NULL: the runtime domain's positions ARE runtime ids. */
const int64_t* ray_sym_domain_runtime_lut(ray_sym_domain_t* dom);

/* RAY_SYM_AUDIT=1 (cached at ray_sym_init): ray_sym_vec_cell cross-checks
 * every resolution and aborts with full context on a violation. */
extern uint8_t ray_g_sym_audit;
void ray_sym_audit_cell(ray_t* vec, int64_t row, int64_t pos, ray_t* resolved);

#endif /* RAY_DOMAIN_H */
