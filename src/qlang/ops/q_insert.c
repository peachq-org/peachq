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
#include "qlang/io/q_provider.h" /* upsert: `:pq: targets route to .X.upsert */

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
    if (!(g->type == RAY_TABLE || q_type_is_keyed(g)))
        return q_err(QE_TYPE);
    int keyed = q_type_is_keyed(g);
    int64_t nkey = keyed ? ray_table_ncols(ray_dict_keys(g)) : 0;
    ray_t* flat = q_table_flatten(g);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    ray_t* rows = q_table_rows_normalize(flat, y, Q_ROWS_INSERT);
    if (!rows || RAY_IS_ERR(rows)) { ray_release(flat); return rows ? rows : q_err(QE_OOM); }
    int64_t before = ray_table_nrows(flat);
    int64_t added  = ray_table_nrows(rows);
    if (keyed) {                                          /* collision -> 'insert */
        ray_t* kt = ray_dict_keys(g);                     /* borrowed */
        int64_t kn = ray_table_nrows(kt);
        for (int64_t r = 0; r < added; r++)
            for (int64_t e = 0; e < kn; e++)
                if (q_table_row_eq(rows, r, kt, e, nkey)) {
                    ray_release(rows); ray_release(flat);
                    return q_err(QE_INSERT);
                }
    }
    ray_t* nf = q_table_append(flat, rows);
    ray_release(flat); ray_release(rows);
    if (!nf || RAY_IS_ERR(nf)) return nf;
    ray_t* nt;
    if (keyed) { nt = q_bang_enkey(nkey, nf); ray_release(nf); }
    else nt = nf;
    if (!nt || RAY_IS_ERR(nt)) return nt;
    q_env_set(x->i64, nt);                                /* retains */
    ray_release(nt);
    return idx_range(before, added);
}

/* q `x upsert y` — a SPELLING of Join (`x upsert y <=> .[x;();,;y] <=> x,y`):
 * this wrapper owns only what the spelling adds — the storage-provider path
 * and name-vs-value resolution (a NAMED target rebinds the global and returns
 * the name; unbound name + table payload creates it).  The VALUE work is
 * Join's one row-append home. */
ray_t* q_upsert_wrap(ray_t* x, ray_t* y) {
    ray_t* pr = q_provider_write(x, y, 1);
    if (pr) return pr;
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
    ray_t* nt = q_join_table_upsert(t, y);
    if (!nt || RAY_IS_ERR(nt)) return nt;
    if (sym >= 0) {
        q_env_set(sym, nt);                               /* retains */
        ray_release(nt);
        ray_retain(x);
        return x;
    }
    return nt;
}
