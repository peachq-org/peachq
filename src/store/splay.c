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

#include "splay.h"
#include "store/col.h"
#include "store/fileio.h"
#include "table/sym.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Splayed table: directory of column files + .d schema file
 *
 * Format:
 *   dir/.d        — RAY_STR vector of column names (self-describing)
 *   dir/<colname> — column file per column
 *
 * No symlink check: local-trust file format; path traversal checks
 * (rejecting '/', '\\', '..', leading '.') cover main attack vector.
 * -------------------------------------------------------------------------- */

/* True when `v` is or contains symbol data (sym vec, sym atom, or a list
 * nesting either) — anything that serializes raw sym ids and therefore
 * needs the dictionary persisted. */
static bool vec_has_syms(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return false;
    if (v->type == RAY_SYM || v->type == -RAY_SYM) return true;
    if (v->type == RAY_LIST) {
        ray_t** items = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < v->len; i++)
            if (vec_has_syms(items[i])) return true;
    }
    return false;
}

/* True when the table has at least one column carrying symbol data. */
static bool table_has_sym_cols(ray_t* tbl) {
    int64_t nc = ray_table_ncols(tbl);
    for (int64_t c = 0; c < nc; c++)
        if (vec_has_syms(ray_table_get_col_idx(tbl, c))) return true;
    return false;
}

/* Post-load validation: reject if sym table is empty but table has symbol
 * columns (incl. symbols nested in lists), or if schema expected columns
 * but none could be loaded. */
static ray_err_t validate_sym_columns(ray_t* tbl, int64_t schema_ncols,
                                      uint32_t sym_count_at_entry) {
    /* Sym table always has the empty string at ID 0 after init, so the
     * baseline "no real symbols loaded" state is count == 1, not 0.
     * The count is snapshotted BEFORE the load loop: loading interns the
     * column names from .d, which must not mask an unpopulated table. */
    if (sym_count_at_entry > 1) return RAY_OK;

    int64_t nc = ray_table_ncols(tbl);
    if (schema_ncols > 0 && nc == 0) return RAY_ERR_CORRUPT;

    for (int64_t c = 0; c < nc; c++)
        if (vec_has_syms(ray_table_get_col_idx(tbl, c))) return RAY_ERR_CORRUPT;
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * ray_splay_save — save a table to a splayed table directory
 * -------------------------------------------------------------------------- */

/* True when `name` (len bytes) names a column of `tbl` that passes the
 * on-disk name-safety filter. */
static bool table_has_col_named(ray_t* tbl, const char* name, size_t len) {
    int64_t nc = ray_table_ncols(tbl);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* na = ray_sym_str(ray_table_col_name(tbl, c));
        if (!na) continue;
        if (ray_str_len(na) == len && memcmp(ray_str_ptr(na), name, len) == 0)
            return true;
    }
    return false;
}

/* Remove regular files in `dir` that are not part of the just-written
 * table: not ".d", not "sym"/"sym.lk", not a current column.  Runs after
 * the .d commit so a stale wider-schema file can never shadow a column
 * (the historical "error: corrupt on re-set" bug). */
static void splay_sweep_stale(ray_t* tbl, const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        const char* n = ent->d_name;
        if (n[0] == '.') continue; /* ".", "..", ".d" */
        if (strcmp(n, "sym") == 0 || strcmp(n, "sym.lk") == 0) continue;
        size_t nlen = strlen(n);
        if (table_has_col_named(tbl, n, nlen)) continue;
        /* `<col>.link` sidecars (store/col.c) belong to their column: keep
         * them while the column is current; ray_col_save already removes a
         * stale sidecar when the column itself is rewritten without a link. */
        if (nlen > 5 && memcmp(n + nlen - 5, ".link", 5) == 0 &&
            table_has_col_named(tbl, n, nlen - 5))
            continue;
        char p[1024];
        int pl = snprintf(p, sizeof(p), "%s/%s", dir, n);
        if (pl <= 0 || (size_t)pl >= sizeof(p)) continue;
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        unlink(p);
    }
    closedir(d);
}

static ray_err_t splay_save_impl(ray_t* tbl, const char* dir, const char* sym_path,
                                 bool durable) {
    if (!tbl || RAY_IS_ERR(tbl)) return RAY_ERR_TYPE;
    if (!dir) return RAY_ERR_IO;

    /* Create directory and any missing parents (mkdir -p semantics).
     * Required for partitioned layouts like "/db/2024.01.01/t/" where the
     * caller hasn't pre-created the date partition. */
    ray_err_t mkdir_err = ray_mkdir_p(dir);
    if (mkdir_err != RAY_OK) return mkdir_err;

    /* 1. Symfile FIRST — column data must never reference symbols that are
     *    not persisted.  Skipped entirely for symbol-free tables: nothing
     *    to enumerate (spec: no-symbol-columns exemption). */
    if (sym_path && table_has_sym_cols(tbl)) {
        ray_err_t sym_err = durable ? ray_sym_save(sym_path)
                                    : ray_sym_save_bulk(sym_path);
        if (sym_err != RAY_OK) return sym_err;
    }

    int64_t ncols = ray_table_ncols(tbl);

    /* 2. Column files (and the schema names they correspond to).
     * NOTE: overwriting an existing dir rewrites columns in place; a crash
     * mid-loop leaves old .d + a mix of old/new column files.  Ragged
     * lengths are caught at load (column-length check); equal-length
     * mixed-generation rows are inherent to in-place overwrite and would
     * need staged writes — out of scope (fresh-dir crashes degrade to a
     * visibly missing table via the .d-last commit marker). */
    ray_t* schema = ray_vec_new(RAY_STR, ncols > 0 ? ncols : 1);
    if (!schema || RAY_IS_ERR(schema)) {
        if (schema) ray_release(schema);
        return RAY_ERR_OOM;
    }
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (!col) continue;
        ray_t* name_atom = ray_sym_str(ray_table_col_name(tbl, c));
        if (!name_atom) continue;
        const char* name = ray_str_ptr(name_atom);
        size_t name_len  = ray_str_len(name_atom);
        if (name_len == 0 || name[0] == '.' ||
            memchr(name, '/', name_len) || memchr(name, '\\', name_len) ||
            memchr(name, '\0', name_len))
            continue; /* unsafe name: no file, no .d entry */

        char path[1024];
        int path_len = snprintf(path, sizeof(path), "%s/%.*s", dir,
                                (int)name_len, name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            ray_release(schema);
            return RAY_ERR_RANGE;
        }
        ray_err_t err = durable ? ray_col_save(col, path)
                                : ray_col_save_bulk(col, path);
        if (err != RAY_OK) {
            /* No .d written yet: the dir stays in its previous committed
             * state (old .d) or uncommitted state (no .d) — never torn. */
            ray_release(schema);
            return err;
        }
        schema = ray_str_vec_append(schema, name, name_len);
        if (!schema || RAY_IS_ERR(schema)) {
            if (schema) ray_release(schema);
            return RAY_ERR_OOM;
        }
    }

    /* 3. .d LAST — the commit marker. */
    {
        char path[1024];
        int path_len = snprintf(path, sizeof(path), "%s/.d", dir);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            ray_release(schema);
            return RAY_ERR_RANGE;
        }
        ray_err_t err = durable ? ray_col_save(schema, path)
                                : ray_col_save_bulk(schema, path);
        ray_release(schema);
        if (err != RAY_OK) return err;
    }

    /* 4. Sweep files that are no longer part of the table. */
    splay_sweep_stale(tbl, dir);

    return RAY_OK;
}

ray_err_t ray_splay_save(ray_t* tbl, const char* dir, const char* sym_path) {
    return splay_save_impl(tbl, dir, sym_path, true);
}

ray_err_t ray_splay_save_bulk(ray_t* tbl, const char* dir, const char* sym_path) {
    return splay_save_impl(tbl, dir, sym_path, false);
}

/* --------------------------------------------------------------------------
 * splay_load_impl — shared implementation for ray_splay_load / ray_read_splayed
 *
 * When use_mmap is false, columns are loaded via ray_col_load (buddy copy).
 * When use_mmap is true, columns are loaded via ray_col_mmap (zero-copy).
 * The .d schema is always loaded via ray_col_load (small, buddy copy).
 * -------------------------------------------------------------------------- */

static ray_t* splay_load_impl(const char* dir, const char* sym_path, bool use_mmap) {
    if (!dir) return ray_error("io", NULL);
    bool trace = getenv("RAY_CSV_TRACE") != NULL;
    if (trace)
        fprintf(stderr, "splayed.get: dir=%s mmap=%d\n", dir, use_mmap ? 1 : 0);

    /* Load symbol table if sym_path provided */
    if (sym_path) {
        ray_err_t sym_err = ray_sym_load(sym_path);
        if (sym_err != RAY_OK) return ray_error(ray_err_code_str(sym_err), NULL);
    }

    /* Load .d schema */
    char path[1024];
    int path_len = snprintf(path, sizeof(path), "%s/.d", dir);
    if (path_len < 0 || (size_t)path_len >= sizeof(path))
        return ray_error("range", NULL);
    ray_t* schema = ray_col_load(path);
    if (!schema || RAY_IS_ERR(schema)) {
        if (trace)
            fprintf(stderr, "splayed.get: schema load failed path=%s err=%s\n",
                    path, schema && RAY_IS_ERR(schema) ? ray_err_code(schema) : "io");
        return schema;
    }

    if (schema->type != RAY_STR) {
        ray_release(schema);
        return ray_error("corrupt",
            "splayed %s: .d is not a string vector (pre-cleanup format?)", dir);
    }

    int64_t ncols = schema->len;

    ray_t* tbl = ray_table_new(ncols);
    if (!tbl || RAY_IS_ERR(tbl)) {
        ray_release(schema);
        return tbl;
    }

    uint32_t sym_count_at_entry = ray_sym_count();

    /* Load each column */
    for (int64_t c = 0; c < ncols; c++) {
        size_t name_len = 0;
        const char* name = ray_str_vec_get(schema, c, &name_len);
        if (!name) {
            ray_release(schema);
            ray_release(tbl);
            return ray_error("corrupt", "splayed %s: unreadable .d entry %lld",
                             dir, (long long)c);
        }

        /* Reject names with path separators, traversal, or starting with '.'
         * — these indicate a corrupt/hand-tampered .d. */
        if (name_len == 0 || name[0] == '.' ||
            memchr(name, '/', name_len) || memchr(name, '\\', name_len) ||
            memchr(name, '\0', name_len)) {
            ray_release(schema);
            ray_release(tbl);
            return ray_error("corrupt",
                "splayed %s: invalid column name in .d entry %lld",
                dir, (long long)c);
        }

        int64_t name_id = ray_sym_intern(name, name_len);

        path_len = snprintf(path, sizeof(path), "%s/%.*s", dir, (int)name_len, name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            ray_release(schema);
            ray_release(tbl);
            return ray_error("range", NULL);
        }

        ray_t* col = use_mmap ? ray_col_mmap_splayed(path) : ray_col_load(path);
        if (use_mmap && col && RAY_IS_ERR(col) &&
            strcmp(ray_err_code(col), "nyi") == 0) {
            /* ray_release on an error object is a no-op (rayforce.h:180);
             * must use ray_error_free to actually reclaim the error
             * before retrying with the non-mmap loader. */
            ray_error_free(col);
            col = ray_col_load(path);
        }
        if (!col || RAY_IS_ERR(col)) {
            if (trace)
                fprintf(stderr, "splayed.get: col load failed path=%s err=%s\n",
                        path, col && RAY_IS_ERR(col) ? ray_err_code(col) : "io");
            ray_release(schema);
            ray_release(tbl);
            return col ? col : ray_error("io", NULL);
        }

        if (c > 0 && col->len != ray_table_nrows(tbl)) {
            ray_t* err = ray_error("corrupt",
                "splayed %s: column '%.*s' has %lld rows, expected %lld "
                "(torn overwrite?)", dir, (int)name_len, name,
                (long long)col->len, (long long)ray_table_nrows(tbl));
            ray_release(col);
            ray_release(schema);
            ray_release(tbl);
            return err;
        }

        ray_t* new_df = ray_table_add_col(tbl, name_id, col);
        if (!new_df || RAY_IS_ERR(new_df)) {
            ray_release(col);
            ray_release(schema);
            ray_release(tbl);
            return new_df ? new_df : ray_error("oom", NULL);
        }
        ray_release(col); /* table_add_col retains; drop our ref */
        tbl = new_df;
    }

    ray_release(schema);

    ray_err_t sym_check = validate_sym_columns(tbl, ncols, sym_count_at_entry);
    if (sym_check != RAY_OK) {
        ray_release(tbl);
        return ray_error(ray_err_code_str(sym_check), NULL);
    }

    return tbl;
}

ray_t* ray_splay_load(const char* dir, const char* sym_path) {
    return splay_load_impl(dir, sym_path, false);
}

ray_t* ray_read_splayed(const char* dir, const char* sym_path) {
    return splay_load_impl(dir, sym_path, true);
}
