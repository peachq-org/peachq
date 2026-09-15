/* ops/q_insert.c — the two by-reference verbs over the table family's shape
 * law (q_table_rows_normalize / q_table_append, q_table.c): `insert`
 * (column-major, name-only, index-returning) and `upsert`, which is a
 * SPELLING of Join — only the storage-provider path and the name/create/
 * rebind arms live here; the VALUE work is q_join_table_upsert. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"
#include "qlang/q_env.h"
#include "qlang/ops/q_table.h"
#include "qlang/ops/q_bang.h"  /* q_bang_enkey — the keying primitive */
#include "qlang/ops/q_index.h" /* q_index_keyed_put — THE keyed write, insert's no-hit mode */
#include "qlang/io/q_provider.h" /* upsert: `:pq: targets route to .X.upsert */
#include "qlang/io/q_io.h"       /* q_io_is_fsym — upsert's file-target classifier */
#include "qlang/net/q_wirefile.h"
#include "qlang/io/q_splay.h"    /* a mapped global refuses by-name rows */

/* Row count of a plain OR keyed table (keyed via its key table — never trust
 * ray_len on a string-atom column). */
static int64_t any_nrows(ray_t* t) {
    if (q_type_is_keyed(t)) return ray_table_nrows(ray_dict_keys(t));
    return ray_table_nrows(t);
}

/* 0-based long index vector [start, start+n). */
static ray_t* idx_range(int64_t start, int64_t n) {
    ray_t* v = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(v)) return v;
    v->len = n;
    int64_t* d = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < n; i++) d[i] = start + i;
    return v;
}

/* The keyed arm: the rows keyed to g's schema go through THE keyed write with no hit allowed — an existing key is
 * 'insert (ref/insert.md:56), the misses grow both slots where the parked g stands. */
static ray_t* insert_keyed(int64_t sym, ray_t* g, ray_t* y) {
    ray_t* flat = q_table_flatten(g);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    ray_t* rows = q_table_rows_normalize(flat, y, Q_ROWS_INSERT);
    ray_release(flat);
    if (!rows || RAY_IS_ERR(rows)) return rows ? rows : q_err(QE_OOM);
    int64_t before = any_nrows(g), added = ray_table_nrows(rows);
    ray_t* ky = q_bang_enkey(ray_table_ncols(ray_dict_keys(g)), rows);
    ray_release(rows);
    if (!ky || RAY_IS_ERR(ky)) return ky;
    ray_retain(g);                                        /* ours across the park */
    int stole = q_env_take(sym, g);
    ray_t* nt = q_index_keyed_put(g, ky, UINT64_MAX, Q_KEYED_INSERT, stole);
    ray_release(ky);
    if (!nt || RAY_IS_ERR(nt)) {
        if (stole) q_env_bind(sym, g);                    /* restore the binding */
        ray_release(g);
        return nt ? nt : q_err(QE_OOM);
    }
    ray_err_t e = q_env_settle(sym, stole, nt);           /* retains */
    ray_release(g);
    ray_release(nt);
    return e == RAY_OK ? idx_range(before, added) : q_env_err(e);
}

/* The rows of a global bound to a provider table go back through the provider (the host contract: every write
 * does), by the carrier's own coordinate; the indices are the provider's count before, then the batch. */
static ray_t* carrier_insert(ray_t* car, ray_t* y) {
    if (!y || y->type != RAY_TABLE) return q_err(QE_TYPE);
    ray_t* before = q_provider_carrier_count(car);
    if (!before || RAY_IS_ERR(before)) return before ? before : q_err(QE_TYPE);
    int64_t b0 = before->type == -RAY_I64 ? before->i64 : before->type == -RAY_I32 ? before->i32 : -1;
    ray_release(before);
    if (b0 < 0) return q_err(QE_TYPE);
    ray_t* r = q_provider_write(ray_dict_vals(car), y, 1);
    if (!r || RAY_IS_ERR(r)) return r ? r : q_err(QE_TYPE);
    ray_release(r);
    return idx_range(b0, ray_table_nrows(y));
}

/* q `x insert y` / insert[x;y] — x MUST name a global (kdb insert is always
 * by reference).  Unbound name + table payload CREATES the global.  Keyed
 * target: key collision -> 'insert.  Returns inserted row indices. */
ray_t* q_insert_wrap(ray_t* x, ray_t* y) {
    if (!x || x->type != -RAY_SYM)
        return q_err(QE_TYPE);
    ray_t* g = q_env_get(x->i64);                         /* borrowed */
    if (!g) {                                             /* create */
        if (y && (y->type == RAY_TABLE || q_type_is_keyed(y))) {
            q_env_set(x->i64, y);                         /* retains */
            return idx_range(0, any_nrows(y));
        }
        return q_err(QE_TYPE);
    }
    if (q_provider_carrier_is(g)) return carrier_insert(g, y);
    if (!(g->type == RAY_TABLE || q_type_is_keyed(g)))
        return q_err(QE_TYPE);
    if (q_splay_table_path(g)) return q_err(QE_SPLAY);   /* a mapped global takes no rows (kb/splayed-tables.md:350) */
    if (q_type_is_keyed(g)) return insert_keyed(x->i64, g, y);
    /* the binding this insert REPLACES double-counts the table, so every
     * column would copy: park it (q_env.h q_env_take) behind our own ref, which
     * keeps g alive for the restore. */
    ray_retain(g);
    int stole = q_env_take(x->i64, g);
    ray_t* rows = q_table_rows_normalize(g, y, Q_ROWS_INSERT);
    ray_t* nt = rows && !RAY_IS_ERR(rows) ? NULL : rows ? rows : q_err(QE_OOM);
    int64_t before = ray_table_nrows(g), added = 0;
    if (!nt) {
        added = ray_table_nrows(rows);
        nt = q_table_append(g, rows, stole);
        ray_release(rows);
    }
    if (!nt || RAY_IS_ERR(nt)) {
        if (stole) q_env_bind(x->i64, g);                 /* restore the binding */
        ray_release(g);
        return nt ? nt : q_err(QE_OOM);
    }
    ray_release(g);
    ray_err_t e = q_env_settle(x->i64, stole, nt);        /* retains */
    ray_release(nt);
    return e == RAY_OK ? idx_range(before, added) : q_env_err(e);
}

/* q `x upsert y` — a SPELLING of Join (`x upsert y <=> .[x;();,;y] <=> x,y`):
 * this wrapper owns only what the spelling adds — the storage-provider path
 * and name-vs-value resolution (a NAMED target rebinds the global and returns
 * the name; unbound name + table payload creates it).  The VALUE work is
 * Join's one row-append home. */
ray_t* q_upsert_wrap(ray_t* x, ray_t* y) {
    ray_t* pr = q_provider_write(x, y, 1);
    if (pr) return pr;
    if (q_io_is_fsym(x)) return q_wirefile_append(x, y);  /* file target: the one
                                                           * flat-append kernel */
    ray_t* car = x && x->type == -RAY_SYM ? q_env_get(x->i64) : x;   /* a carrier, named or as the value */
    if (q_provider_carrier_is(car)) {
        pr = q_provider_write(ray_dict_vals(car), y, 1);
        if (!pr || RAY_IS_ERR(pr)) return pr ? pr : q_err(QE_TYPE);
        ray_release(pr);
        ray_retain(x);
        return x;
    }
    int64_t sym;
    ray_t* t = q_table_operand(x, &sym);
    if (!t) {
        if (x && x->type == -RAY_SYM && !q_env_get(x->i64) &&
            y && (y->type == RAY_TABLE || q_type_is_keyed(y))) {
            q_env_set(x->i64, y);                         /* create, like insert */
            ray_retain(x);
            return x;
        }
        return q_err(QE_TYPE);
    }
    if (sym >= 0 && q_splay_table_path(t)) return q_err(QE_SPLAY);
    if (sym < 0) return q_join_table_upsert(t, y, 0);
    /* the binding this upsert REPLACES double-counts the table, so every
     * column would copy: park it (q_env.h q_env_take) behind our own ref */
    ray_retain(t);
    int stole = q_env_take(sym, t);
    ray_t* nt = q_join_table_upsert(t, y, stole);
    if (!nt || RAY_IS_ERR(nt)) {
        if (stole) q_env_bind(sym, t);                    /* restore the binding */
        ray_release(t);
        return nt ? nt : q_err(QE_OOM);
    }
    ray_err_t e = q_env_settle(sym, stole, nt);           /* retains */
    ray_release(t);
    ray_release(nt);
    if (e != RAY_OK) return q_env_err(e);
    ray_retain(x);
    return x;
}
