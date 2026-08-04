/* ops/q_table.c — the table primitives (flatten / row-at / map-cols / the
 * q_table.h family seam) and the shape verbs built on them:
 * flip, keys, xkey, xgroup, group, ungroup, cols, meta.
 *
 * The rest of the family lives alongside: q_insert.c (insert/upsert),
 * q_setops.c (distinct/union/inter/except/cross), q_join.c (`,`), and the
 * name-facing `key`/`set` in q_env.c.
 *
 * Keyed tables are built over the keyed primitives (q_type_is_keyed,
 * q_table_flatten, q_bang_enkey) — NEVER duplicated (the #56 failure mode).
 * Row equality is boxed q-match compare: O(n^2) at test scale by design
 * (single-home principle; SIMD paths belong to the engine). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"       /* q_type_is_keyed / q_type_is_dense_group_col */
#include "qlang/q_builtins.h"   /* q_ty_char — meta column letters */
#include "qlang/q_env.h"
#include "qlang/ops/q_table.h"
#include "qlang/ops/q_bang.h"   /* q_bang_enkey — xkey's keying primitive */
#include "qlang/ops/q_index.h"  /* q_index_at / q_index_elem_at — group's key gathers */
#include "qlang/io/q_splay.h"   /* mapped splays: cols/meta answer from headers */
#include "lang/internal.h"      /* ray_group_fn */
#include "ops/agg_engine.h"     /* agg_group_keys — the one dense group core */
#include "table/sym.h"          /* ray_sym_intern_runtime, ray_sym_vec_cell, RAY_SYM_W64 */
#include <string.h>
#include <stdlib.h>

/* THE per-column table walk: apply `colfn` to each column (borrowed), add the
 * owned result under the same name; first error aborts + propagates.  rc-careful
 * (colfn result released after add); colfn owns any lazy-materialization. */
ray_t* q_table_map_cols(ray_t* (*colfn)(void* ctx, ray_t* col), void* ctx, ray_t* t) {
    int64_t nc = ray_table_ncols(t);
    ray_t* out = ray_table_new(nc);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(t, c);          /* borrowed */
        ray_t* r = colfn(ctx, col);
        if (!r || RAY_IS_ERR(r)) {
            ray_release(out);
            return r ? r : q_err(QE_TYPE);
        }
        out = ray_table_add_col(out, ray_table_col_name(t, c), r);
        ray_release(r);
    }
    return out;
}

/* Flatten a plain-or-keyed table to a single plain table (key cols then value
 * cols).  Returns owned. */
ray_t* q_table_flatten(ray_t* y) {
    if (y->type == RAY_TABLE) { ray_retain(y); return y; }
    ray_t* kt = ray_dict_keys(y);          /* borrowed */
    ray_t* vt = ray_dict_vals(y);          /* borrowed */
    int64_t knc = ray_table_ncols(kt), vnc = ray_table_ncols(vt);
    ray_t* out = ray_table_new(knc + vnc > 0 ? knc + vnc : 1);
    for (int64_t c = 0; c < knc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(kt, c), ray_table_get_col_idx(kt, c));
    for (int64_t c = 0; c < vnc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(vt, c), ray_table_get_col_idx(vt, c));
    return out;
}

/* Borrow-or-collapse a dict's VALUES for a kernel call: a typed vector passes
 * through BORROWED (*owned=0); a boxed list collapses (q_list_collapse, owned
 * result, *owned=1) so homogeneous literal dicts hit the typed kernels.
 * Caller releases iff *owned.  NULL on a malformed dict. */
ray_t* q_table_dict_vals(ray_t* d, int* owned) {
    *owned = 0;
    ray_t* vals = ray_dict_vals(d);              /* borrowed accessor */
    if (!vals) return NULL;
    if (vals->type == RAY_LIST) {
        ray_t* c = q_list_collapse(vals);        /* owned */
        if (c && !RAY_IS_ERR(c)) { *owned = 1; return c; }
        if (c) ray_release(c);
        return vals;                             /* uncollapsible: borrowed */
    }
    return vals;
}

/* Extract symbol ids from a -RAY_SYM atom / RAY_SYM vector / LIST of sym
 * atoms.  Returns count, or -1 on a non-symbol operand.  cap-bounded. */
static int64_t sym_ids(ray_t* x, int64_t* out, int64_t cap) {
    if (!x) return -1;
    if (x->type == -RAY_SYM) { if (cap < 1) return -1; out[0] = x->i64; return 1; }
    if (x->type == RAY_SYM) {
        int64_t n = ray_len(x);
        if (n > cap) return -1;
        for (int64_t i = 0; i < n; i++) {
            /* borrowed domain atom — never released (table/sym.h) */
            ray_t* s = ray_sym_vec_cell(x, i);
            out[i] = s ? ray_sym_intern_runtime(ray_str_ptr(s), ray_str_len(s)) : 0;
        }
        return n;
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        if (n > cap) return -1;
        ray_t** e = (ray_t**)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            if (!e[i] || e[i]->type != -RAY_SYM) return -1;
            out[i] = e[i]->i64;
        }
        return n;
    }
    return -1;
}

/* Column index of name id in table t, or -1. */
int64_t q_table_col_index(ray_t* t, int64_t nm) {
    int64_t nc = ray_table_ncols(t);
    for (int64_t c = 0; c < nc; c++)
        if (ray_table_col_name(t, c) == nm) return c;
    return -1;
}

/* Reorder a plain table: the named columns first (in given order), the rest
 * in original order.  'length when a name is missing.  Returns owned. */
static ray_t* table_reorder(ray_t* t, const int64_t* names, int64_t n) {
    int64_t nc = ray_table_ncols(t);
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t i = 0; i < n && !RAY_IS_ERR(out); i++) {
        int64_t c = q_table_col_index(t, names[i]);
        if (c < 0) { ray_release(out); return q_err(QE_LENGTH); }
        out = ray_table_add_col(out, names[i], ray_table_get_col_idx(t, c));
    }
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
        int64_t nm = ray_table_col_name(t, c);
        int used = 0;
        for (int64_t i = 0; i < n; i++) if (names[i] == nm) { used = 1; break; }
        if (!used) out = ray_table_add_col(out, nm, ray_table_get_col_idx(t, c));
    }
    return out;
}

/* ===== universal table row indexing (uniform-structure-dispatch stage 0) ===
 * THE row-access primitive behind t[i] / t[indexvector] / each-over-rows /
 * count-drop and sublist row slices.  Base ray_at's table arms error on char
 * columns ('type) and out-of-range rows ('domain); the q law
 * (basics/application.md "Indexing out of bounds") is null-fill: a miss
 * yields the typed null of each column (char -> the blank " "; LIST -> the
 * null of the first item's type).  The vector arm is qj_table_gather_idx
 * (ops/q_join.c) — ONE gather home for joins, funsql scatters and row
 * indexing alike. */

/* t[row] -> the ROW DICT.  An out-of-range/negative/null row (miss) yields
 * the typed all-null row.  Values collapse like kdb row dicts do (a uniform
 * table's row has a typed vector value, not a boxed list). */
ray_t* q_table_row_at(ray_t* t, int64_t row) {
    int64_t nc = ray_table_ncols(t);
    int64_t nr = ray_table_nrows(t);
    int hit = row >= 0 && row < nr;
    ray_t* names = ray_vec_new(RAY_SYM, nc > 0 ? nc : 1);
    if (!names || RAY_IS_ERR(names)) return names ? names : q_err(QE_OOM);
    names->len = nc;
    int64_t* nd = (int64_t*)ray_data(names);
    ray_t* vals = ray_list_new(nc > 0 ? nc : 1);
    if (!vals || RAY_IS_ERR(vals)) { ray_release(names); return vals ? vals : q_err(QE_OOM); }
    for (int64_t c = 0; c < nc; c++) {
        nd[c] = ray_table_col_name(t, c);
        ray_t* col = ray_table_get_col_idx(t, c);            /* borrowed */
        ray_t* cell;
        if (col && col->type == -RAY_STR) {                  /* char column: one byte = one row */
            cell = (hit && row < (int64_t)ray_str_len(col))
                 ? ray_str(ray_str_ptr(col) + row, 1)
                 : ray_str(" ", 1);                          /* the char null is the blank */
        } else if (col && col->type == RAY_LIST) {
            if (hit && row < ray_len(col)) {
                cell = ((ray_t**)ray_data(col))[row];
                ray_retain(cell);
            } else {
                /* miss: the null of the first item's type (doc law); a string
                 * item nulls to the empty string, non-atom items to :: */
                ray_t** e = (ray_t**)ray_data(col);
                if (ray_len(col) > 0 && e[0] && e[0]->type == -RAY_STR)
                    cell = ray_str("", 0);
                else if (ray_len(col) > 0 && e[0] && e[0]->type < 0)
                    cell = ray_typed_null(e[0]->type);
                else { ray_retain(RAY_NULL_OBJ); cell = RAY_NULL_OBJ; }
            }
        } else if (col) {                                    /* typed vector: ray_at null-fills a miss */
            cell = q_join_item(col, row);
        } else {
            ray_release(names); ray_release(vals);
            return q_err(QE_TYPE);
        }
        if (!cell || RAY_IS_ERR(cell)) { ray_release(names); ray_release(vals); return cell ? cell : q_err(QE_TYPE); }
        vals = ray_list_append(vals, cell);
        ray_release(cell);
        if (RAY_IS_ERR(vals)) { ray_release(names); return vals; }
    }
    ray_t* cv = q_list_collapse(vals);
    ray_release(vals);
    if (!cv || RAY_IS_ERR(cv)) { ray_release(names); return cv ? cv : q_err(QE_TYPE); }
    return ray_dict_new(names, cv);                          /* consumes both */
}

/* t[idx] dispatcher over the single-home gather: an integer ATOM is the row
 * dict, an integer VECTOR is a row gather (misses null-filled).  Returns
 * NULL to DECLINE any other index shape — the caller keeps its historic
 * path (sym -> column access, boxed lists, errors). */
ray_t* q_table_at(ray_t* t, ray_t* idx) {
    if (!t || t->type != RAY_TABLE || !idx) return NULL;
    if (ray_is_atom(idx)) {
        if (!q_type_int_index_width((int8_t)-idx->type)) return NULL;
        return q_table_row_at(t, as_i64(idx));               /* int/temporal nulls land <0 -> miss */
    }
    int w = ray_is_vec(idx) ? q_type_int_index_width(idx->type) : 0;
    if (!w) return NULL;
    int64_t n = ray_len(idx);
    int64_t nr = ray_table_nrows(t);
    int64_t* ids = (int64_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
    if (!ids) return q_err(QE_WSFULL);
    for (int64_t i = 0; i < n; i++) {
        int64_t v;
        switch (w) {                                         /* width, never tag (spec §2.2) */
        case 8: v = ((int64_t*)ray_data(idx))[i]; break;
        case 4: v = ((int32_t*)ray_data(idx))[i]; break;
        case 2: v = ((int16_t*)ray_data(idx))[i]; break;
        default: v = ((uint8_t*)ray_data(idx))[i]; break;
        }
        /* normalize EVERY miss (bitmap null, sentinel null <0, out-of-range)
         * to the gather's documented miss encoding idx[i] < 0 — never lean
         * on the boxed path's incidental out-of-range handling */
        if (v < 0 || v >= nr || ray_vec_is_null(idx, i)) v = -1;
        ids[i] = v;
    }
    ray_t* r = qj_table_gather_idx(t, ids, n);
    free(ids);
    return r;
}

/* Row equality over the FIRST ncmp columns of two tables (boxed compare). */
int q_table_row_eq(ray_t* ta, int64_t ra, ray_t* tb, int64_t rb, int64_t ncmp) {
    for (int64_t c = 0; c < ncmp; c++) {
        ray_t* av = q_join_item(ray_table_get_col_idx(ta, c), ra);
        ray_t* bv = q_join_item(ray_table_get_col_idx(tb, c), rb);
        int eq = (av && bv && !RAY_IS_ERR(av) && !RAY_IS_ERR(bv)) ? q_match_rec(av, bv) : 0;
        if (av) ray_release(av);
        if (bv) ray_release(bv);
        if (!eq) return 0;
    }
    return 1;
}

/* Collapse per-column boxed accumulators into a table taking column names from
 * `names` at offset c0.  CONSUMES every accs[c] — an entry that IS an error
 * propagates as the result — so a caller never unwinds them by hand.  Owned. */
ray_t* q_table_cols_from_accs(ray_t* names, int64_t c0, ray_t** accs, int64_t nc) {
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* a = accs[c];
        ray_t* cc;
        if (!a) cc = q_err(QE_OOM);
        else if (RAY_IS_ERR(a)) cc = a;
        else { cc = q_list_collapse(a); ray_release(a); }
        if (RAY_IS_ERR(out)) {
            if (cc && cc != out) ray_release(cc);
        } else if (!cc || RAY_IS_ERR(cc)) {
            ray_release(out);
            out = cc ? cc : q_err(QE_OOM);
        } else {
            out = ray_table_add_col(out, ray_table_col_name(names, c0 + c), cc);
            ray_release(cc);
        }
    }
    return out;
}

/* First-occurrence row grouping over the FIRST ncmp columns: fills gid[nr] with
 * each row's group id, rep[ng] with each group's first row; returns the group
 * count, -1 on allocation failure.  Dense-hashed through the one group core
 * (agg_group_keys) when every compared column qualifies, boxed row-compare
 * otherwise — THE grouping home for `group`, `xgroup` and row dedup alike. */
int64_t q_table_row_groups(ray_t* t, int64_t ncmp, int64_t* gid, int64_t* rep) {
    int64_t nr = ray_table_nrows(t);
    int64_t ng = 0;
    int dense = ncmp >= 1 && ncmp <= 16 && nr > 0;
    for (int64_t c = 0; c < ncmp && dense; c++)
        dense = q_type_is_dense_group_col(ray_table_get_col_idx(t, c));
    if (dense) {
        ray_t* kcols[16];
        for (int64_t c = 0; c < ncmp; c++) kcols[c] = ray_table_get_col_idx(t, c);
        agg_groups_t g;
        if (agg_group_keys(kcols, (uint8_t)ncmp, nr, &g) != 0) return -1;
        for (int64_t r = 0; r < nr; r++) gid[r] = (int64_t)g.gids[r];
        ng = g.ngroups;
        for (int64_t j = 0; j < ng; j++) rep[j] = g.first_row[j];
        agg_groups_free(&g);
        return ng;
    }
    for (int64_t r = 0; r < nr; r++) {
        int64_t g = -1;
        for (int64_t j = 0; j < ng && g < 0; j++)
            if (q_table_row_eq(t, r, t, rep[j], ncmp)) g = j;
        if (g < 0) { rep[ng] = r; g = ng++; }
        gid[r] = g;
    }
    return ng;
}

/* n copies of atom `a` as a collapsed column (broadcast helper). */
ray_t* q_table_bcast_col(ray_t* a, int64_t n) {
    ray_t* l = ray_list_new(n > 0 ? n : 1);
    if (RAY_IS_ERR(l)) return l;
    for (int64_t i = 0; i < n; i++) {
        l = ray_list_append(l, a);
        if (RAY_IS_ERR(l)) return l;
    }
    ray_t* c = q_list_collapse(l);
    ray_release(l);
    return c;
}

/* Resolve a table operand that may be BY NAME (-RAY_SYM naming a global).
 * Returns the borrowed target (env-owned, or the operand itself) and sets
 * *sym_out to the name id (or -1 for by-value).  NULL => not a table. */
ray_t* q_table_operand(ray_t* y, int64_t* sym_out) {
    *sym_out = -1;
    if (!y) return NULL;
    if (y->type == -RAY_SYM) {
        ray_t* g = q_env_get(y->i64);
        if (g && (g->type == RAY_TABLE || q_type_is_keyed(g))) { *sym_out = y->i64; return g; }
        return NULL;
    }
    if (y->type == RAY_TABLE || q_type_is_keyed(y)) return y;
    return NULL;
}

/* q `flip x` / monadic `+` — transpose.
 *   table         -> dict (colnames ! list-of-columns)      [flip flip t ~ t]
 *   dict          -> table (sym keys; vector vals share one length L, atoms
 *                    broadcast to L; mismatched vector length -> 'length)
 *   list of lists -> transposed list (atom items broadcast)
 * Keyed tables, atoms, and an all-atom list are 'rank DEFERRED cells (the
 * all-atom arm is a choice, not verified kdb behaviour). */
ray_t* q_flip_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* k = ray_sym_vec_new(RAY_SYM_W64, nc > 0 ? nc : 1);
        if (!k || RAY_IS_ERR(k)) return k ? k : q_err(QE_OOM);
        ray_t* v = ray_list_new(nc > 0 ? nc : 1);
        if (RAY_IS_ERR(v)) { ray_release(k); return v; }
        for (int64_t c = 0; c < nc; c++) {
            int64_t nm = ray_table_col_name(x, c);
            k = ray_vec_append(k, &nm);
            if (!k || RAY_IS_ERR(k)) { ray_release(v); return k ? k : q_err(QE_OOM); }
            v = ray_list_append(v, ray_table_get_col_idx(x, c));   /* retains */
            if (RAY_IS_ERR(v)) { ray_release(k); return v; }
        }
        return ray_dict_new(k, v);                        /* consumes both */
    }
    if (q_type_is_keyed(x)) return q_err(QE_RANK);
    if (x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);                      /* borrowed */
        ray_t* v = ray_dict_vals(x);                      /* borrowed */
        if (!k || k->type != RAY_SYM || !v)
            return q_err(QE_TYPE);
        int64_t nc = ray_len(k);
        if (!(v->type == RAY_LIST || ray_is_vec(v)) || ray_len(v) != nc)
            return q_err(QE_LENGTH);
        /* pass 1: L = shared vector length (atoms broadcast; all-atom -> 1) */
        int64_t L = -1;
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = q_join_item(v, c);
            if (!col || RAY_IS_ERR(col)) return col ? col : q_err(QE_OOM);
            if (!ray_is_atom(col)) {
                int64_t l = ray_len(col);
                if (L < 0) L = l;
                else if (l != L) { ray_release(col); return q_err(QE_LENGTH); }
            }
            ray_release(col);
        }
        if (L < 0) L = 1;
        /* pass 2: build the table (atoms broadcast to L) */
        ray_t* out = ray_table_new(nc > 0 ? nc : 1);
        for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
            /* borrowed domain atom — never released (table/sym.h) */
            ray_t* s = ray_sym_vec_cell(k, c);
            int64_t nm = s ? ray_sym_intern_runtime(ray_str_ptr(s), ray_str_len(s)) : 0;
            ray_t* col = q_join_item(v, c);
            if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : q_err(QE_OOM); }
            if (ray_is_atom(col)) {
                ray_t* b = q_table_bcast_col(col, L);
                ray_release(col);
                col = b;
                if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : q_err(QE_OOM); }
            }
            out = ray_table_add_col(out, nm, col);
            ray_release(col);
        }
        return out;
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        if (n == 0) { ray_retain(x); return x; }
        ray_t** e = (ray_t**)ray_data(x);
        int64_t L = -1;
        for (int64_t i = 0; i < n; i++) {
            ray_t* it = e[i];
            if (it && (ray_is_vec(it) || it->type == RAY_LIST)) {
                int64_t l = ray_len(it);
                if (L < 0) L = l;
                else if (l != L) return q_err(QE_LENGTH);
            }
        }
        if (L < 0) return q_err(QE_RANK);
        ray_t* out = ray_list_new(L > 0 ? L : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t r = 0; r < L; r++) {
            ray_t* rowl = ray_list_new(n);
            if (RAY_IS_ERR(rowl)) { ray_release(out); return rowl; }
            for (int64_t i = 0; i < n; i++) {
                ray_t* it = e[i];
                ray_t* cell;
                if (it && (ray_is_vec(it) || it->type == RAY_LIST)) {
                    cell = q_join_item(it, r);
                } else { cell = it; if (cell) ray_retain(cell); }
                if (!cell || RAY_IS_ERR(cell)) { ray_release(rowl); ray_release(out); return cell ? cell : q_err(QE_OOM); }
                rowl = ray_list_append(rowl, cell);
                ray_release(cell);
                if (RAY_IS_ERR(rowl)) { ray_release(out); return rowl; }
            }
            ray_t* rowc = q_list_collapse(rowl);
            ray_release(rowl);
            if (!rowc || RAY_IS_ERR(rowc)) { ray_release(out); return rowc ? rowc : q_err(QE_OOM); }
            out = ray_list_append(out, rowc);
            ray_release(rowc);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return q_err(QE_RANK);
}

/* q `x xkey y` — set key columns: reorder x-first, enkey count x (reuses
 * q_bang_enkey).  By-reference (y a name): rebind and return the name. */
ray_t* q_xkey_wrap(ray_t* x, ray_t* y) {
    int64_t names[64];
    int64_t n = sym_ids(x, names, 64);
    if (n < 0) return q_err(QE_TYPE);
    int64_t sym;
    ray_t* t = q_table_operand(y, &sym);
    if (!t) return q_err(QE_TYPE);
    ray_t* flat = q_table_flatten(t);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    ray_t* keyed;
    if (n == 0) keyed = flat;                             /* () xkey t -> unkey */
    else {
        ray_t* reord = table_reorder(flat, names, n);
        ray_release(flat);
        if (!reord || RAY_IS_ERR(reord)) return reord;
        keyed = q_bang_enkey(n, reord);
        ray_release(reord);
        if (!keyed || RAY_IS_ERR(keyed)) return keyed;
    }
    if (sym >= 0) {
        q_env_set(sym, keyed);                            /* retains */
        ray_release(keyed);
        ray_retain(y);
        return y;
    }
    return keyed;
}

/* q `x xgroup y` — key by x, remaining columns become per-group nested lists
 * (first-occurrence group order, ref/xgroup.md). */
ray_t* q_xgroup_wrap(ray_t* x, ray_t* y) {
    int64_t names[64];
    int64_t nk = sym_ids(x, names, 64);
    if (nk <= 0) return q_err(QE_TYPE);
    int64_t sym;
    ray_t* t = q_table_operand(y, &sym);
    if (!t) return q_err(QE_TYPE);
    ray_t* flat = q_table_flatten(t);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    ray_t* reord = table_reorder(flat, names, nk);      /* key cols first */
    ray_release(flat);
    if (!reord || RAY_IS_ERR(reord)) return reord;
    int64_t nc = ray_table_ncols(reord);
    int64_t nr = ray_table_nrows(reord);
    int64_t* gid = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    int64_t* rep = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    if (!gid || !rep) { free(gid); free(rep); ray_release(reord); return q_err(QE_WSFULL); }
    int64_t ng = q_table_row_groups(reord, nk, gid, rep);
    if (ng < 0) { free(gid); free(rep); ray_release(reord); return q_err(QE_WSFULL); }
    /* key table: the first-occurrence rows of the key columns */
    ray_t* keycols = ray_table_new(nk);
    for (int64_t c = 0; c < nk && !RAY_IS_ERR(keycols); c++)
        keycols = ray_table_add_col(keycols, ray_table_col_name(reord, c),
                                    ray_table_get_col_idx(reord, c));
    ray_t* kt = RAY_IS_ERR(keycols) ? keycols : qj_table_gather_idx(keycols, rep, ng);
    if (!RAY_IS_ERR(keycols)) ray_release(keycols);
    /* the per-group row-index vectors, built ONCE — not per value column.
     * Sized from a single counting pass: `nr` slots per group would be
     * O(nr*ng), i.e. quadratic on unique keys. */
    int64_t* cnt = rep;                       /* rep is done — reuse as counts */
    memset(cnt, 0, sizeof(int64_t) * (size_t)(ng > 0 ? ng : 1));
    for (int64_t r = 0; r < nr; r++) cnt[gid[r]]++;
    ray_t* gidx = ray_list_new(ng > 0 ? ng : 1);
    for (int64_t j = 0; j < ng && !RAY_IS_ERR(gidx); j++) {
        ray_t* iv = ray_vec_new(RAY_I64, cnt[j] > 0 ? cnt[j] : 1);
        if (RAY_IS_ERR(iv)) { ray_release(gidx); gidx = iv; break; }
        iv->len = 0;
        gidx = ray_list_append(gidx, iv);
        ray_release(iv);
    }
    if (!RAY_IS_ERR(gidx)) {
        ray_t** ivs = (ray_t**)ray_data(gidx);
        for (int64_t r = 0; r < nr; r++) {
            ray_t* iv = ivs[gid[r]];
            ((int64_t*)ray_data(iv))[iv->len++] = r;
        }
    }
    /* value table: each remaining column gathered per group into nested cells */
    ray_t* vt = RAY_IS_ERR(gidx) ? gidx : ray_table_new(nc - nk > 0 ? nc - nk : 1);
    for (int64_t c = nk; c < nc && !RAY_IS_ERR(vt); c++) {
        ray_t* acc = ray_list_new(ng > 0 ? ng : 1);
        ray_t** ivs = (ray_t**)ray_data(gidx);
        for (int64_t j = 0; j < ng && !RAY_IS_ERR(acc); j++) {
            ray_t* grp = ray_at_fn(ray_table_get_col_idx(reord, c), ivs[j]);
            if (!grp || RAY_IS_ERR(grp)) { ray_release(acc); acc = grp ? grp : q_err(QE_OOM); break; }
            if (grp->type == RAY_LIST) {
                ray_t* gc = q_list_collapse(grp);
                ray_release(grp);
                grp = gc;
                if (!grp || RAY_IS_ERR(grp)) { ray_release(acc); acc = grp ? grp : q_err(QE_OOM); break; }
            }
            acc = ray_list_append(acc, grp);
            ray_release(grp);
        }
        if (RAY_IS_ERR(acc)) { ray_release(vt); vt = acc; break; }
        vt = ray_table_add_col(vt, ray_table_col_name(reord, c), acc);
        ray_release(acc);
    }
    if (!RAY_IS_ERR(gidx)) ray_release(gidx);
    free(gid); free(rep);
    ray_release(reord);
    if (RAY_IS_ERR(kt)) { if (vt && !RAY_IS_ERR(vt)) ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);                          /* consumes both */
}

/* q `group x` — one law for every shape (ref/group.md; funsql waves 1-2):
 * vector/list -> base hash kernel; dict -> `(key d) group value d` (each index
 * list maps through the keys); table -> `(distinct t)!(row-index lists)`,
 * first-occurrence order — dense-hashed via agg_group_keys when every column
 * is int64/sym/str-readable, boxed row-compare fallback otherwise. */
static ray_t* group_table(ray_t* t) {
    int64_t nc = ray_table_ncols(t);
    int64_t nr = ray_table_nrows(t);
    int64_t* gid = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    int64_t* rep = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    if (!gid || !rep) { free(gid); free(rep); return q_err(QE_WSFULL); }
    int64_t ng = q_table_row_groups(t, nc, gid, rep);
    if (ng < 0) { free(gid); free(rep); return q_err(QE_WSFULL); }
    ray_t* kt = qj_table_gather_idx(t, rep, ng);          /* ≡ distinct t */
    if (!kt || RAY_IS_ERR(kt)) { free(gid); free(rep); return kt ? kt : q_err(QE_OOM); }
    int64_t* cnt = rep;                       /* rep is done — reuse as counts */
    memset(cnt, 0, sizeof(int64_t) * (size_t)(ng > 0 ? ng : 1));
    for (int64_t r = 0; r < nr; r++) cnt[gid[r]]++;
    ray_t* vals = ray_list_new(ng > 0 ? ng : 1);
    ray_t** vs = (ray_t**)ray_data(vals);
    for (int64_t j = 0; j < ng && !RAY_IS_ERR(vals); j++) {
        ray_t* iv = ray_vec_new(RAY_I64, cnt[j] > 0 ? cnt[j] : 1);
        if (RAY_IS_ERR(iv)) { ray_release(vals); vals = iv; break; }
        iv->len = 0;
        vals = ray_list_append(vals, iv);
        ray_release(iv);
    }
    if (RAY_IS_ERR(vals)) { free(gid); free(rep); ray_release(kt); return vals; }
    vs = (ray_t**)ray_data(vals);
    for (int64_t r = 0; r < nr; r++) {
        ray_t* iv = vs[gid[r]];
        ((int64_t*)ray_data(iv))[iv->len++] = r;
    }
    free(gid); free(rep);
    return ray_dict_new(kt, vals);
}

ray_t* q_group_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (q_type_is_table(x)) return group_table(x);
    if (q_type_is_dict(x)) {
        ray_t* g = q_group_wrap(ray_dict_vals(x));        /* group the range */
        if (!g || RAY_IS_ERR(g)) return g ? g : q_err(QE_TYPE);
        ray_t* keys = ray_dict_keys(x);
        ray_t* gv = ray_dict_vals(g);
        int64_t ng = ray_len(gv);
        ray_t* nv = ray_list_new(ng > 0 ? ng : 1);
        for (int64_t j = 0; j < ng && !RAY_IS_ERR(nv); j++) {
            ray_t* iv = q_index_elem_at(gv, j);
            ray_t* m = (iv && !RAY_IS_ERR(iv)) ? q_index_at(keys, &iv, 1) : iv;
            if (m != iv && iv) ray_release(iv);
            if (m) { ray_t* c = q_list_collapse(m); ray_release(m); m = c; }
            if (!m || RAY_IS_ERR(m)) { ray_release(nv); nv = m ? m : q_err(QE_OOM); break; }
            nv = ray_list_append(nv, m);
            ray_release(m);
        }
        if (RAY_IS_ERR(nv)) { ray_release(g); return nv; }
        ray_t* gk = ray_dict_keys(g);
        ray_retain(gk);
        ray_release(g);
        return ray_dict_new(gk, nv);
    }
    return ray_group_fn(x);
}

/* q `ungroup x` — inverse of xgroup: explode nested list columns, repeating
 * simple cells; ragged nested rows -> 'length.  Keyed tables flatten first. */
ray_t* q_ungroup_wrap(ray_t* x) {
    int64_t sym;
    ray_t* t = q_table_operand(x, &sym);
    if (!t) return q_err(QE_TYPE);
    ray_t* flat = q_table_flatten(t);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    int64_t nc = ray_table_ncols(flat);
    int64_t nr = ray_table_nrows(flat);
    if (nc > 64) { ray_release(flat); return q_err(QE_LIMIT); }
    ray_t* acc[64];
    for (int64_t c = 0; c < nc; c++) {
        acc[c] = ray_list_new(nr > 0 ? nr : 1);
        if (RAY_IS_ERR(acc[c])) {
            ray_t* err = acc[c];
            for (int64_t j = 0; j < c; j++) ray_release(acc[j]);
            ray_release(flat);
            return err;
        }
    }
    ray_t* err = NULL;
    for (int64_t r = 0; r < nr && !err; r++) {
        int64_t cnt = -1;
        for (int64_t c = 0; c < nc && !err; c++) {
            ray_t* col = ray_table_get_col_idx(flat, c);
            if (col && col->type == RAY_LIST) {
                ray_t* cell = q_join_item(col, r);
                if (!cell || RAY_IS_ERR(cell)) { err = cell ? cell : q_err(QE_OOM); break; }
                int64_t l = cell->type == -RAY_STR ? (int64_t)ray_str_len(cell)
                          : (ray_is_vec(cell) || cell->type == RAY_LIST) ? ray_len(cell) : 1;
                ray_release(cell);
                if (cnt < 0) cnt = l;
                else if (l != cnt) err = q_err(QE_LENGTH);
            }
        }
        if (err) break;
        if (cnt < 0) cnt = 1;                             /* no nested column */
        for (int64_t c = 0; c < nc && !err; c++) {
            ray_t* col = ray_table_get_col_idx(flat, c);
            ray_t* cell = q_join_item(col, r);
            if (!cell || RAY_IS_ERR(cell)) { err = cell ? cell : q_err(QE_OOM); break; }
            if (col && col->type == RAY_LIST) {           /* explode nested cell */
                for (int64_t i = 0; i < cnt && !err; i++) {
                    ray_t* e = q_join_item(cell, i);
                    if (!e || RAY_IS_ERR(e)) { err = e ? e : q_err(QE_OOM); break; }
                    acc[c] = ray_list_append(acc[c], e);
                    ray_release(e);
                    if (RAY_IS_ERR(acc[c])) err = acc[c];
                }
            } else {                                      /* repeat simple cell */
                for (int64_t i = 0; i < cnt && !err; i++) {
                    acc[c] = ray_list_append(acc[c], cell);
                    if (RAY_IS_ERR(acc[c])) err = acc[c];
                }
            }
            ray_release(cell);
        }
    }
    if (err) {
        for (int64_t c = 0; c < nc; c++)
            if (acc[c] && !RAY_IS_ERR(acc[c])) ray_release(acc[c]);
        ray_release(flat);
        return err;
    }
    ray_t* out = q_table_cols_from_accs(flat, 0, acc, nc);
    ray_release(flat);
    return out;
}

/* Column name ids of a table as a RAY_SYM vector.  A keyed table yields key
 * cols ++ value cols — which is exactly the flatten order. */
static ray_t* table_colnames(ray_t* x) {
    if (!q_type_is_table(x) && !q_type_is_keyed(x)) return q_err(QE_TYPE);
    ray_t* flat = q_table_flatten(x);
    if (!flat || RAY_IS_ERR(flat)) return flat ? flat : q_err(QE_OOM);
    int64_t nc = ray_table_ncols(flat);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc && out && !RAY_IS_ERR(out); c++) {
        int64_t nm = ray_table_col_name(flat, c);
        out = ray_vec_append(out, &nm);
    }
    ray_release(flat);
    return out ? out : q_err(QE_OOM);
}

/* Resolve a by-name table operand (cols`t / meta`t): a -RAY_SYM naming a
 * global plain/keyed/mapped-splay table resolves to it (borrowed); else x. */
static ray_t* table_bi_deref(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* g = q_env_get(x->i64);                   /* borrowed */
        if (g && (g->type == RAY_TABLE || q_type_is_keyed(g) || q_splay_is(g)))
            return g;
    }
    return x;
}

/* (cols x) — column names of a table as a symbol vector. */
ray_t* q_cols_fn(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    ray_t* t = table_bi_deref(x);
    if (q_splay_is(t)) {                    /* marker-first: the keys ARE the cols */
        ray_t* k = ray_dict_keys(t);
        ray_retain(k);
        return k;
    }
    return table_colnames(t);
}

/* (meta x) — table metadata keyed by column name: (c) -> (t; f; a).  ONE
 * builder over two per-column fact sources: an in-memory table's columns
 * (t via q_ty_char, f/a blank) or a mapped splay's probed HEADERS — type from
 * the type byte (enum `s`, nested upper-cased), attr `s` when the disk byte
 * says sorted, never a data read; only a column whose header cannot name its
 * type (a kxzip container, a shape-A non-vector) decodes once to answer.
 * The result is a RAY_DICT from a 1-col key table to a 3-col value table —
 * "a keyed table is just a dictionary from one table to another". */
ray_t* q_meta_fn(ray_t* x) {
    ray_t* t = x ? table_bi_deref(x) : NULL;
    int64_t nc = q_splay_ncols(t);
    int splay = nc >= 0;
    ray_t* flat = NULL;
    if (!splay) {
        if (!q_type_is_table(t) && !q_type_is_keyed(t)) return q_err(QE_TYPE);
        flat = q_table_flatten(t);
        if (!flat || RAY_IS_ERR(flat)) return flat ? flat : q_err(QE_OOM);
        nc = ray_table_ncols(flat);
    }
    int64_t cap = nc > 0 ? nc : 1;
    ray_t* cvec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* c: names          */
    ray_t* fvec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* f: blank per col  */
    ray_t* avec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* a: `s or blank    */
    char stackt[64];
    char* tbuf = (cap <= (int64_t)sizeof stackt) ? stackt : (char*)malloc((size_t)cap);
    int ok = cvec && !RAY_IS_ERR(cvec) && fvec && !RAY_IS_ERR(fvec) &&
             avec && !RAY_IS_ERR(avec) && tbuf;
    ray_t* bad = NULL;
    int64_t blank = ray_sym_intern_runtime("", 0);
    int64_t ssym  = ray_sym_intern_runtime("s", 1);
    for (int64_t c = 0; c < nc && ok; c++) {
        int64_t nm, a = blank;
        char tc;
        if (!splay) {
            nm = ray_table_col_name(flat, c);
            tc = q_ty_char(ray_table_get_col_idx(flat, c));   /* borrowed */
        } else {
            nm = q_splay_col_sym(t, c);
            const q_wf_colhdr* h = q_splay_col_hdr(t, c);
            tc = h->is_enum ? 's' : h->tag ? q_type_char(h->tag) : 0;
            if (h->nested && tc) tc = (char)(tc - 'a' + 'A');
            if (h->disk_attr == 1) a = ssym;
            if (!tc) {                        /* opaque header: the decode answers */
                ray_t* col = q_splay_col(t, nm);
                if (!col || RAY_IS_ERR(col)) { bad = col; ok = 0; break; }
                tc = q_ty_char(col);
                ray_release(col);
            }
        }
        tbuf[c] = tc ? tc : ' ';
        cvec = ray_vec_append(cvec, &nm);
        fvec = ray_vec_append(fvec, &blank);
        avec = ray_vec_append(avec, &a);
        if (!cvec || RAY_IS_ERR(cvec) || !fvec || RAY_IS_ERR(fvec) ||
            !avec || RAY_IS_ERR(avec)) ok = 0;
    }
    if (flat) ray_release(flat);
    ray_t* tstr = ok ? ray_str(tbuf, (size_t)nc) : NULL;
    if (tbuf && tbuf != stackt) free(tbuf);
    if (!ok || !tstr || RAY_IS_ERR(tstr)) {
        if (cvec && !RAY_IS_ERR(cvec)) ray_release(cvec);
        if (fvec && !RAY_IS_ERR(fvec)) ray_release(fvec);
        if (avec && !RAY_IS_ERR(avec)) ray_release(avec);
        if (tstr && !RAY_IS_ERR(tstr)) ray_release(tstr);
        return bad ? bad : q_err(QE_WSFULL);
    }
    /* key table: c ; value table: t f a  -> keyed table dict */
    ray_t* kt = ray_table_new(1);
    kt = ray_table_add_col(kt, ray_sym_intern("c", 1), cvec);
    ray_release(cvec);
    ray_t* vt = ray_table_new(3);
    vt = ray_table_add_col(vt, ray_sym_intern("t", 1), tstr); ray_release(tstr);
    if (!RAY_IS_ERR(vt)) { vt = ray_table_add_col(vt, ray_sym_intern("f", 1), fvec); }
    ray_release(fvec);
    if (!RAY_IS_ERR(vt)) { vt = ray_table_add_col(vt, ray_sym_intern("a", 1), avec); }
    ray_release(avec);
    if (RAY_IS_ERR(kt)) { if (!RAY_IS_ERR(vt)) ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);   /* consumes kt, vt */
}
