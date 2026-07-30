/* ops/q_insert.c — q `insert` / `upsert`: the payload-normalization law and
 * the append / keyed update-or-append cores behind them.  Both verbs are
 * by-reference in kdb, so the name arms (create, rebind) live here too. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h"
#include "qlang/q_err.h"
#include "qlang/q_type.h"
#include "qlang/q_env.h"
#include "qlang/ops/q_table.h"
#include "qlang/ops/q_bang.h"  /* q_bang_enkey — the keying primitive */
#include "lang/internal.h"   /* ray_concat_fn, ray_typed_null */
#include "table/sym.h"       /* ray_sym_intern_runtime, ray_sym_vec_cell */
#include <string.h>
#include <stdlib.h>

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

/* Typed null cell matching a column's element type (missing-column fill). */
static ray_t* null_cell_like(ray_t* col) {
    if (!col) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    int8_t t = col->type;
    if (t == RAY_SYM || t == -RAY_SYM) return ray_sym(ray_sym_intern_runtime("", 0));
    if (t == -RAY_STR || t == RAY_STR) return ray_str("", 0);
    if (ray_is_vec(col)) return ray_typed_null((int8_t)-t);
    if (ray_is_atom(col)) return ray_typed_null(t);
    ray_retain(RAY_NULL_OBJ);                             /* list/empty column */
    return RAY_NULL_OBJ;
}

/* Normalize an insert/upsert payload y against the FLAT target schema.
 * Returns an OWNED plain table with the target's column names holding the
 * new rows.  Forms (ref/insert.md, ref/upsert.md; ambiguity rules per the
 * plan's review addendum):
 *   - TABLE (plain/keyed): columns matched BY NAME.  Payload columns unknown
 *     to the target -> 'mismatch (silent drop is never OK).  Target columns
 *     absent from the payload: null-filled when `partial` (upsert), else
 *     'mismatch (insert).
 *   - LIST with count == ncols: columns-form — item i is column i, atoms
 *     broadcast to the longest item (all-atom == the single-record form).
 *   - other LIST: records-form — every item a list/vector of ncols cells.
 *   - DICT: one row, name-matched (strict: key set must be a subset AND
 *     cover; partial: unknown keys ignored, missing columns null-filled).
 *   - 1-column target: an atom/vector payload IS the column. */
static ray_t* rows_normalize(ray_t* flat, ray_t* y, int partial) {
    if (!y) return q_err(QE_TYPE);
    int64_t nc = ray_table_ncols(flat);
    if (nc <= 0) return q_err(QE_TYPE);
    if (nc > 64) return q_err(QE_LIMIT);

    if (y->type == RAY_TABLE || q_type_is_keyed(y)) {
        ray_t* src = q_table_flatten(y);
        if (!src || RAY_IS_ERR(src)) return src;
        int64_t snc = ray_table_ncols(src);
        for (int64_t c = 0; c < snc; c++) {
            if (q_table_col_index(flat, ray_table_col_name(src, c)) < 0) {
                ray_release(src);
                return q_err(QE_MISMATCH);
            }
        }
        int64_t nr = ray_table_nrows(src);
        ray_t* out = ray_table_new(nc);
        for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
            int64_t nm = ray_table_col_name(flat, c);
            int64_t sc = q_table_col_index(src, nm);
            if (sc >= 0) {
                out = ray_table_add_col(out, nm, ray_table_get_col_idx(src, sc));
                continue;
            }
            if (!partial) { ray_release(out); ray_release(src); return q_err(QE_MISMATCH); }
            ray_t* acc = ray_list_new(nr > 0 ? nr : 1);
            for (int64_t r = 0; r < nr && !RAY_IS_ERR(acc); r++) {
                ray_t* nl = null_cell_like(ray_table_get_col_idx(flat, c));
                acc = ray_list_append(acc, nl);
                ray_release(nl);
            }
            if (RAY_IS_ERR(acc)) { ray_release(out); ray_release(src); return acc; }
            ray_t* cc = q_list_collapse(acc);
            ray_release(acc);
            if (!cc || RAY_IS_ERR(cc)) { ray_release(out); ray_release(src); return cc ? cc : q_err(QE_OOM); }
            out = ray_table_add_col(out, nm, cc);
            ray_release(cc);
        }
        ray_release(src);
        return out;
    }

    if (y->type == RAY_DICT) {
        ray_t* dk = ray_dict_keys(y);                     /* borrowed */
        if (!dk || dk->type != RAY_SYM)
            return q_err(QE_TYPE);
        if (!partial) {
            int64_t dn = ray_len(dk);
            for (int64_t i = 0; i < dn; i++) {
                /* borrowed domain atom — never released (table/sym.h) */
                ray_t* s = ray_sym_vec_cell(dk, i);
                int64_t id = s ? ray_sym_intern_runtime(ray_str_ptr(s), ray_str_len(s)) : 0;
                if (q_table_col_index(flat, id) < 0) return q_err(QE_MISMATCH);
            }
        }
        ray_t* out = ray_table_new(nc);
        for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
            int64_t nm = ray_table_col_name(flat, c);
            ray_t* ka = ray_sym(nm);
            ray_t* cellv = ray_dict_get(y, ka);           /* owned or NULL */
            ray_release(ka);
            if (!cellv) {
                if (!partial) { ray_release(out); return q_err(QE_MISMATCH); }
                cellv = null_cell_like(ray_table_get_col_idx(flat, c));
            }
            if (RAY_IS_ERR(cellv)) { ray_release(out); return cellv; }
            ray_t* col = q_table_bcast_col(cellv, 1);
            ray_release(cellv);
            if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : q_err(QE_OOM); }
            out = ray_table_add_col(out, nm, col);
            ray_release(col);
        }
        return out;
    }

    if (nc == 1 && y->type != RAY_LIST) {
        ray_t* col;
        if (ray_is_atom(y)) col = q_table_bcast_col(y, 1);
        else { ray_retain(y); col = y; }
        if (!col || RAY_IS_ERR(col)) return col ? col : q_err(QE_OOM);
        ray_t* out = ray_table_new(1);
        if (!RAY_IS_ERR(out)) out = ray_table_add_col(out, ray_table_col_name(flat, 0), col);
        ray_release(col);
        return out;
    }

    if (y->type != RAY_LIST && !ray_is_vec(y))
        return q_err(QE_TYPE);

    int64_t ny = ray_len(y);

    if (ny == nc) {                                       /* columns-form */
        int64_t L = -1;
        for (int64_t c = 0; c < nc; c++) {
            ray_t* it = q_join_item(y, c);
            if (!it || RAY_IS_ERR(it)) return it ? it : q_err(QE_OOM);
            if (!ray_is_atom(it)) {
                int64_t l = ray_len(it);
                if (L < 0) L = l;
                else if (l != L) { ray_release(it); return q_err(QE_LENGTH); }
            }
            ray_release(it);
        }
        if (L < 0) L = 1;
        ray_t* out = ray_table_new(nc);
        for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
            ray_t* it = q_join_item(y, c);
            if (!it || RAY_IS_ERR(it)) { ray_release(out); return it ? it : q_err(QE_OOM); }
            ray_t* col;
            if (ray_is_atom(it)) { col = q_table_bcast_col(it, L); ray_release(it); }
            else col = it;
            if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : q_err(QE_OOM); }
            out = ray_table_add_col(out, ray_table_col_name(flat, c), col);
            ray_release(col);
        }
        return out;
    }

    /* records-form */
    {
        ray_t* accs[64];
        for (int64_t c = 0; c < nc; c++) {
            accs[c] = ray_list_new(ny > 0 ? ny : 1);
            if (RAY_IS_ERR(accs[c])) {
                ray_t* e = accs[c];
                for (int64_t j = 0; j < c; j++) ray_release(accs[j]);
                return e;
            }
        }
        ray_t* err = NULL;
        for (int64_t r = 0; r < ny && !err; r++) {
            ray_t* rec = q_join_item(y, r);
            if (!rec || RAY_IS_ERR(rec) ||
                !(ray_is_vec(rec) || rec->type == RAY_LIST) || ray_len(rec) != nc) {
                if (rec && RAY_IS_ERR(rec)) err = rec;
                else { if (rec) ray_release(rec); err = q_err(QE_LENGTH); }
                break;
            }
            for (int64_t c = 0; c < nc && !err; c++) {
                ray_t* cell = q_join_item(rec, c);
                if (!cell || RAY_IS_ERR(cell)) { err = cell ? cell : q_err(QE_OOM); break; }
                accs[c] = ray_list_append(accs[c], cell);
                ray_release(cell);
                if (RAY_IS_ERR(accs[c])) { err = accs[c]; accs[c] = NULL; }
            }
            ray_release(rec);
        }
        if (err) {
            for (int64_t c = 0; c < nc; c++)
                if (accs[c] && !RAY_IS_ERR(accs[c])) ray_release(accs[c]);
            return err;
        }
        return q_table_cols_from_accs(flat, 0, accs, nc);
    }
}

/* Append normalized rows to a flat table.  An EMPTY target (0 rows — e.g.
 * `([]name:();age:())`) adopts the payload columns wholesale: that is how the
 * first insert types an untyped empty schema (insert.qcmd `meta u`).  Column
 * name set is the target's either way. */
static ray_t* table_append(ray_t* flat, ray_t* rows) {
    int64_t nc = ray_table_ncols(flat);
    if (ray_table_nrows(flat) == 0) {
        /* untyped empty columns (RAY_LIST) adopt the payload type; a TYPED
         * 0-row column keeps kdb type-strictness. */
        if (ray_table_nrows(rows) > 0) {
            for (int64_t c = 0; c < nc; c++) {
                ray_t* oc = ray_table_get_col_idx(flat, c);
                ray_t* pc = ray_table_get_col_idx(rows, c);
                if (oc && pc && ray_is_vec(oc) && pc->type != oc->type)
                    return q_err(QE_TYPE);
            }
        }
        ray_t* out = ray_table_new(nc > 0 ? nc : 1);
        for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++)
            out = ray_table_add_col(out, ray_table_col_name(flat, c),
                                    ray_table_get_col_idx(rows, c));
        return out;
    }
    /* kdb type-strictness: appending into a simple typed column requires the
     * SAME element type — `insert[`t;(`ferrari;8.22)]` into a long column is
     * 'type, never a silent float promotion.  List (nested) target columns
     * accept anything; 0-row payloads have nothing to check. */
    if (ray_table_nrows(rows) > 0) {
        for (int64_t c = 0; c < nc; c++) {
            ray_t* oc = ray_table_get_col_idx(flat, c);
            ray_t* pc = ray_table_get_col_idx(rows, c);
            if (oc && pc && ray_is_vec(oc) && pc->type != oc->type)
                return q_err(QE_TYPE);
        }
    }
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
        ray_t* joined = ray_concat_fn(ray_table_get_col_idx(flat, c),
                                             ray_table_get_col_idx(rows, c));
        if (!joined || RAY_IS_ERR(joined)) { ray_release(out); return joined ? joined : q_err(QE_OOM); }
        out = ray_table_add_col(out, ray_table_col_name(flat, c), joined);
        ray_release(joined);
    }
    return out;
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
    ray_t* rows = rows_normalize(flat, y, 0);
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
    ray_t* nf = table_append(flat, rows);
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

/* Keyed-table upsert core: payload rows whose key matches an existing key
 * UPDATE the value cells in place; the rest append (ref/upsert.md).  Both
 * operands flat (key cols first); returns the new FLAT table. */
static ray_t* keyed_upsert_flat(ray_t* flat, int64_t nkey, ray_t* rows) {
    int64_t nc = ray_table_ncols(flat);
    int64_t n0 = ray_table_nrows(flat);
    int64_t na = ray_table_nrows(rows);
    if (nc > 64) return q_err(QE_LIMIT);
    ray_t* colv[64];
    for (int64_t c = 0; c < nc; c++) colv[c] = NULL;
    ray_t* err = NULL;
    for (int64_t c = 0; c < nc && !err; c++) {
        colv[c] = ray_list_new(n0 + na > 0 ? n0 + na : 1);
        if (RAY_IS_ERR(colv[c])) { err = colv[c]; colv[c] = NULL; break; }
        for (int64_t r = 0; r < n0 && !err; r++) {
            ray_t* cell = q_join_item(ray_table_get_col_idx(flat, c), r);
            if (!cell || RAY_IS_ERR(cell)) { err = cell ? cell : q_err(QE_OOM); break; }
            colv[c] = ray_list_append(colv[c], cell);
            ray_release(cell);
            if (RAY_IS_ERR(colv[c])) { err = colv[c]; colv[c] = NULL; }
        }
    }
    int64_t nrows = n0;
    for (int64_t r = 0; r < na && !err; r++) {
        int64_t hit = -1;
        for (int64_t e = 0; e < nrows && hit < 0 && !err; e++) {
            int eq = 1;
            for (int64_t c = 0; c < nkey && eq && !err; c++) {
                ray_t* nv = q_join_item(ray_table_get_col_idx(rows, c), r);
                if (!nv || RAY_IS_ERR(nv)) { err = nv ? nv : q_err(QE_OOM); break; }
                ray_t** cells = (ray_t**)ray_data(colv[c]);
                eq = q_match_rec(cells[e], nv);
                ray_release(nv);
            }
            if (!err && eq) hit = e;
        }
        for (int64_t c = 0; c < nc && !err; c++) {
            ray_t* nv = q_join_item(ray_table_get_col_idx(rows, c), r);
            if (!nv || RAY_IS_ERR(nv)) { err = nv ? nv : q_err(QE_OOM); break; }
            if (hit >= 0) {
                if (c >= nkey) {                          /* update value cells */
                    ray_t** cells = (ray_t**)ray_data(colv[c]);
                    ray_t* old = cells[hit];
                    ray_retain(nv);
                    cells[hit] = nv;
                    ray_release(old);
                }
            } else {
                colv[c] = ray_list_append(colv[c], nv);
                if (RAY_IS_ERR(colv[c])) { err = colv[c]; colv[c] = NULL; }
            }
            ray_release(nv);
        }
        if (hit < 0 && !err) nrows++;
    }
    if (err) {
        for (int64_t c = 0; c < nc; c++)
            if (colv[c] && !RAY_IS_ERR(colv[c])) ray_release(colv[c]);
        return err;
    }
    return q_table_cols_from_accs(flat, 0, colv, nc);
}

/* q `x upsert y` — plain table appends; keyed table updates-or-appends by
 * key.  Value target returns the new table; a NAMED target rebinds the
 * global and returns the name (unbound name + table payload creates it). */
ray_t* q_upsert_wrap(ray_t* x, ray_t* y) {
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
    int keyed = q_type_is_keyed(t);
    int64_t nkey = keyed ? ray_table_ncols(ray_dict_keys(t)) : 0;
    ray_t* flat = q_table_flatten(t);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    ray_t* rows = rows_normalize(flat, y, 1);
    if (!rows || RAY_IS_ERR(rows)) { ray_release(flat); return rows ? rows : q_err(QE_OOM); }
    ray_t* nf = keyed ? keyed_upsert_flat(flat, nkey, rows)
                      : table_append(flat, rows);
    ray_release(flat); ray_release(rows);
    if (!nf || RAY_IS_ERR(nf)) return nf;
    ray_t* nt;
    if (keyed) { nt = q_bang_enkey(nkey, nf); ray_release(nf); }
    else nt = nf;
    if (!nt || RAY_IS_ERR(nt)) return nt;
    if (sym >= 0) {
        q_env_set(sym, nt);                               /* retains */
        ray_release(nt);
        ray_retain(x);
        return x;
    }
    return nt;
}
