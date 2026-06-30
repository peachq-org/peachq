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

#ifndef RAY_CSV_H
#define RAY_CSV_H

#include <rayforce.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Column type tokens used during CSV parse and schema resolution.
 * CSV_TYPE_AUTO is a parse-time marker only — never a stored width and never
 * returned by csv_resolve_int_width.
 * -------------------------------------------------------------------------- */
typedef enum {
    CSV_TYPE_UNKNOWN = 0,
    CSV_TYPE_BOOL,
    CSV_TYPE_I64,
    CSV_TYPE_F64,
    CSV_TYPE_STR,
    CSV_TYPE_DATE,
    CSV_TYPE_TIME,
    CSV_TYPE_TIMESTAMP,
    CSV_TYPE_GUID,
    /* Narrow-int parse types — selected only via explicit schema (never from
     * inference, so they do not appear in promote_csv_type). Each parses the
     * field as int64 and narrows at write time to match the column width. */
    CSV_TYPE_U8,
    CSV_TYPE_I16,
    CSV_TYPE_I32,
    /* Marker for "pick the narrowest integer width automatically" — used only
     * during schema processing, never returned by the resolver. */
    CSV_TYPE_AUTO
} csv_type_t;

/* Resolve the narrowest integer column type that can hold all values in
 * [min, max].  BOOL and U8 have no null sentinel, so a nullable column
 * always resolves to a signed type (I16/I32/I64) that excludes INT_MIN.
 * When min > max (empty or all-null column) returns CSV_TYPE_I64. */
csv_type_t csv_resolve_int_width(int64_t min, int64_t max, bool has_null);

ray_t* ray_read_csv(const char* path);
ray_t* ray_read_csv_opts(const char* path, char delimiter, bool header,
                        const int8_t* col_types, int32_t n_types);
ray_t* ray_read_csv_named_opts(const char* path, char delimiter, bool header,
                               const int8_t* col_types, int32_t n_types,
                               const int64_t* col_names, int32_t n_names);
ray_err_t ray_csv_save_parted_named_opts(const char* path, char delimiter, bool header,
                                         const int8_t* col_types, int32_t n_types,
                                         const int64_t* col_names, int32_t n_names,
                                         const char* root, const char* table_name,
                                         int64_t rows_per_part);
ray_err_t ray_csv_save_splayed_named_opts(const char* path, char delimiter, bool header,
                                          const int8_t* col_types, int32_t n_types,
                                          const int64_t* col_names, int32_t n_names,
                                          const char* dir, int64_t rows_per_chunk);
ray_err_t ray_write_csv(ray_t* table, const char* path);

#endif /* RAY_CSV_H */
