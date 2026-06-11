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

#ifndef RAY_COL_H
#define RAY_COL_H

#include <rayforce.h>

struct ray_sym_domain_s;

/* Column file I/O.
 *
 * The bare save/load/mmap entry points keep PROCESS-LOCAL semantics for
 * RAY_SYM columns: cells are written as runtime intern ids (re-expressed
 * through the vec's domain when it is a FILE domain) and loads attach
 * the runtime singleton, bounds-validated against the global table.
 *
 * Tables persisted as splayed dirs use the *_sym_encoded / *_dom
 * variants: on-disk SYM data is positions in the table's symfile and
 * loads attach that symfile's domain (sym-domain architecture spec). */
ray_err_t ray_col_save(ray_t* vec, const char* path);
ray_err_t ray_col_save_bulk(ray_t* vec, const char* path);
ray_t*    ray_col_load(const char* path);
ray_t*    ray_col_mmap(const char* path);

/* Write a RAY_SYM column re-encoded as positions in `target` (width =
 * ray_sym_dict_width(domain count); header rc = domain count at save).
 * Caller must have interned the column's distinct symbols into `target`
 * and FLUSHED the domain first (crash ordering: sym before columns) —
 * a cell whose symbol is absent from `target` is RAY_ERR_CORRUPT. */
ray_err_t ray_col_save_sym_encoded(ray_t* vec, const char* path,
                                   struct ray_sym_domain_s* target,
                                   bool durable);

/* Domain-attaching loaders for splayed/parted tables.  `dom` is the
 * table's symfile domain; RAY_SYM columns attach it (retained per
 * column) and bounds-validate against its count.  A RAY_SYM column with
 * dom == NULL is a loud "sym" error — a stored SYM column without a
 * resolvable symfile must never resolve against incidental state. */
ray_t*    ray_col_load_dom(const char* path, struct ray_sym_domain_s* dom);
ray_t*    ray_col_mmap_splayed_dom(const char* path, struct ray_sym_domain_s* dom);

#endif /* RAY_COL_H */
