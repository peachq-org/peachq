/* ops/q_table.c — table verbs: flip/keys/xkey/xgroup/ungroup, insert/upsert,
 * key, set, set-ops (distinct/union/inter/except/cross),
 * and the shared right-to-left context builder (list/table literals)
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#define Q_OPS_ENV_GRANDFATHER /* the .ipc.on.* six-slot callback seam: ray_env_set on hook syms only */
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_err.h"
#include "qlang/q_type.h"       /* q_type_is_keyed / q_type_qname / q_type_iatom_val */
#include "qlang/q_builtins.h"   /* q_ty_char — meta column letters */
#include "qlang/ops/q_bang.h"  /* q_bang_dispatch — the `-N!` internal-fn manifest */
#include "qlang/q_dotz.h"  /* q_dotz_ipc_hook_index, q_dotz_zts_set — .z.* handler arms */
#include "qlang/net/q_wirefile.h" /* q_wirefile_write — the `:file set y file form */
#include "qlang/q_env.h"   /* the q K-tree — every name read/bind below */
#include "lang/env.h"      /* ray_env_set + ray_sym_ipc_hook — the .ipc.on.* seam ONLY */
#include "lang/eval.h"     /* ray_eval; ray_list_fn, ray_except_fn, ray_sect_fn, ray_take_fn */
#include "lang/internal.h" /* ray_concat_fn, ray_typed_null, ray_error, ray_group_fn */
#include "qlang/ops/q_index.h" /* q_index_at / q_index_elem_at — group's key gathers */
#include "ops/agg_engine.h"    /* agg_group_keys — the one dense group core */
#include "table/sym.h"     /* ray_sym_intern_runtime, ray_sym_vec_cell, RAY_SYM_W64 */
#include <stdio.h>         /* snprintf, rename */
#include <string.h>
#include <stdlib.h>        /* malloc/calloc/free */

/* ===== table verbs (feat/q-table-verbs) ====================================
 * flip/keys/xkey/xasc/xdesc/xgroup/ungroup/insert/upsert + the
 * table arms of distinct/union/inter/except.  All built over the wave-4
 * keyed primitives (q_type_is_keyed in q_type.c, q_table_flatten below,
 * q_bang_enkey in q_bang.c) — NEVER duplicated (the #56 failure mode).  Row-equality is boxed q-match
 * compares: O(n^2) wrapper-tier code at test scale by design (single-home
 * principle; SIMD paths belong to the engine).                              */

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
static int64_t col_index(ray_t* t, int64_t nm) {
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
        int64_t c = col_index(t, names[i]);
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
            ray_t* ia = ray_i64(row);
            cell = ray_at_fn(col, ia);
            ray_release(ia);
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

/* Whole-value equality of two boxed cells (q match semantics). */
static int cell_eq(ray_t* a, ray_t* b) {
    return q_match_rec(a, b);
}

/* Row equality over the FIRST ncmp columns of two tables (boxed compare). */
static int row_eq(ray_t* ta, int64_t ra, ray_t* tb, int64_t rb, int64_t ncmp) {
    for (int64_t c = 0; c < ncmp; c++) {
        ray_t* ia = ray_i64(ra);
        ray_t* av = ray_at_fn(ray_table_get_col_idx(ta, c), ia);
        ray_release(ia);
        ray_t* ib = ray_i64(rb);
        ray_t* bv = ray_at_fn(ray_table_get_col_idx(tb, c), ib);
        ray_release(ib);
        int eq = (av && bv && !RAY_IS_ERR(av) && !RAY_IS_ERR(bv)) ? cell_eq(av, bv) : 0;
        if (av) ray_release(av);
        if (bv) ray_release(bv);
        if (!eq) return 0;
    }
    return 1;
}

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

/* n copies of atom `a` as a collapsed column (broadcast helper). */
static ray_t* bcast_col(ray_t* a, int64_t n) {
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
static ray_t* table_operand(ray_t* y, int64_t* sym_out) {
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
            ray_t* ia = ray_i64(c); ray_t* col = ray_at_fn(v, ia); ray_release(ia);
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
            ray_t* ia = ray_i64(c); ray_t* col = ray_at_fn(v, ia); ray_release(ia);
            if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : q_err(QE_OOM); }
            if (ray_is_atom(col)) {
                ray_t* b = bcast_col(col, L);
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
                    ray_t* ia = ray_i64(r); cell = ray_at_fn(it, ia); ray_release(ia);
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

/* q `keys x` — key column names (empty sym vector if unkeyed; table by value
 * or by name). */
ray_t* q_keys_wrap(ray_t* x) {
    int64_t sym;
    ray_t* t = table_operand(x, &sym);
    if (!t) return q_err(QE_TYPE);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    if (q_type_is_keyed(t)) {
        ray_t* kt = ray_dict_keys(t);                     /* borrowed */
        int64_t knc = ray_table_ncols(kt);
        for (int64_t c = 0; c < knc; c++) {
            int64_t nm = ray_table_col_name(kt, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        }
    }
    return out;
}

/* q `x xkey y` — set key columns: reorder x-first, enkey count x (reuses
 * q_bang_enkey).  By-reference (y a name): rebind and return the name. */
ray_t* q_xkey_wrap(ray_t* x, ray_t* y) {
    int64_t names[64];
    int64_t n = sym_ids(x, names, 64);
    if (n < 0) return q_err(QE_TYPE);
    int64_t sym;
    ray_t* t = table_operand(y, &sym);
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
    ray_t* t = table_operand(y, &sym);
    if (!t) return q_err(QE_TYPE);
    ray_t* flat = q_table_flatten(t);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    ray_t* reord = table_reorder(flat, names, nk);      /* key cols first */
    ray_release(flat);
    if (!reord || RAY_IS_ERR(reord)) return reord;
    int64_t nc = ray_table_ncols(reord);
    int64_t nr = ray_table_nrows(reord);
    /* group ids by first occurrence (boxed compare, test-scale O(n*g)) */
    int64_t* gid = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    int64_t* rep = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    if (!gid || !rep) { free(gid); free(rep); ray_release(reord); return q_err(QE_WSFULL); }
    int64_t ng = 0;
    for (int64_t r = 0; r < nr; r++) {
        int64_t g = -1;
        for (int64_t j = 0; j < ng && g < 0; j++)
            if (row_eq(reord, r, reord, rep[j], nk)) g = j;
        if (g < 0) { rep[ng] = r; g = ng++; }
        gid[r] = g;
    }
    /* key table: first-occurrence cells of the key columns */
    ray_t* kt = ray_table_new(nk);
    for (int64_t c = 0; c < nk && !RAY_IS_ERR(kt); c++) {
        ray_t* acc = ray_list_new(ng > 0 ? ng : 1);
        for (int64_t j = 0; j < ng && !RAY_IS_ERR(acc); j++) {
            ray_t* ia = ray_i64(rep[j]);
            ray_t* cell = ray_at_fn(ray_table_get_col_idx(reord, c), ia);
            ray_release(ia);
            if (!cell || RAY_IS_ERR(cell)) { ray_release(acc); acc = cell ? cell : q_err(QE_OOM); break; }
            acc = ray_list_append(acc, cell);
            ray_release(cell);
        }
        if (RAY_IS_ERR(acc)) { ray_release(kt); kt = acc; break; }
        ray_t* cc = q_list_collapse(acc);
        ray_release(acc);
        if (!cc || RAY_IS_ERR(cc)) { ray_release(kt); kt = cc ? cc : q_err(QE_OOM); break; }
        kt = ray_table_add_col(kt, ray_table_col_name(reord, c), cc);
        ray_release(cc);
    }
    /* value table: per group, gather each value column by row indices */
    ray_t* vt = RAY_IS_ERR(kt) ? (ray_retain(kt), kt) : ray_table_new(nc - nk > 0 ? nc - nk : 1);
    for (int64_t c = nk; c < nc && !RAY_IS_ERR(vt); c++) {
        ray_t* acc = ray_list_new(ng > 0 ? ng : 1);
        for (int64_t j = 0; j < ng && !RAY_IS_ERR(acc); j++) {
            int64_t cnt = 0;
            for (int64_t r = 0; r < nr; r++) if (gid[r] == j) cnt++;
            ray_t* idx = ray_vec_new(RAY_I64, cnt > 0 ? cnt : 1);
            if (RAY_IS_ERR(idx)) { ray_release(acc); acc = idx; break; }
            idx->len = 0;
            for (int64_t r = 0; r < nr && !RAY_IS_ERR(idx); r++)
                if (gid[r] == j) idx = ray_vec_append(idx, &r);
            if (RAY_IS_ERR(idx)) { ray_release(acc); acc = idx; break; }
            ray_t* grp = ray_at_fn(ray_table_get_col_idx(reord, c), idx);
            ray_release(idx);
            if (!grp || RAY_IS_ERR(grp)) { ray_release(acc); acc = grp ? grp : q_err(QE_OOM); break; }
            ray_t* gc;
            if (grp->type == RAY_LIST) { gc = q_list_collapse(grp); ray_release(grp); }
            else gc = grp;
            if (!gc || RAY_IS_ERR(gc)) { ray_release(acc); acc = gc ? gc : q_err(QE_OOM); break; }
            acc = ray_list_append(acc, gc);
            ray_release(gc);
        }
        if (RAY_IS_ERR(acc)) { ray_release(vt); vt = acc; break; }
        vt = ray_table_add_col(vt, ray_table_col_name(reord, c), acc);
        ray_release(acc);
    }
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
static ray_t* table_gather(ray_t* t, ray_t* idx);      /* fwd (set-ops block) */

static ray_t* group_table(ray_t* t) {
    int64_t nc = ray_table_ncols(t);
    int64_t nr = ray_table_nrows(t);
    int64_t* gid = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    int64_t* rep = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    if (!gid || !rep) { free(gid); free(rep); return q_err(QE_WSFULL); }
    int64_t ng = 0;
    int dense = nc >= 1 && nc <= 16 && nr > 0;
    for (int64_t c = 0; c < nc && dense; c++)
        dense = q_type_is_dense_group_col(ray_table_get_col_idx(t, c));
    if (dense) {
        ray_t* kcols[16];
        for (int64_t c = 0; c < nc; c++) kcols[c] = ray_table_get_col_idx(t, c);
        agg_groups_t g;
        if (agg_group_keys(kcols, (uint8_t)nc, nr, &g) != 0) {
            free(gid); free(rep);
            return q_err(QE_WSFULL);
        }
        for (int64_t r = 0; r < nr; r++) gid[r] = (int64_t)g.gids[r];
        ng = g.ngroups;
        for (int64_t j = 0; j < ng; j++) rep[j] = g.first_row[j];
        agg_groups_free(&g);
    } else {
        for (int64_t r = 0; r < nr; r++) {
            int64_t g = -1;
            for (int64_t j = 0; j < ng && g < 0; j++)
                if (row_eq(t, r, t, rep[j], nc)) g = j;
            if (g < 0) { rep[ng] = r; g = ng++; }
            gid[r] = g;
        }
    }
    ray_t* repv = ray_vec_new(RAY_I64, ng > 0 ? ng : 1);
    if (RAY_IS_ERR(repv)) { free(gid); free(rep); return repv; }
    repv->len = ng;
    memcpy(ray_data(repv), rep, sizeof(int64_t) * (size_t)(ng > 0 ? ng : 1));
    ray_t* kt = table_gather(t, repv);                    /* ≡ distinct t */
    ray_release(repv);
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
    ray_t* t = table_operand(x, &sym);
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
                ray_t* ia = ray_i64(r);
                ray_t* cell = ray_at_fn(col, ia);
                ray_release(ia);
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
            ray_t* ia = ray_i64(r);
            ray_t* cell = ray_at_fn(col, ia);
            ray_release(ia);
            if (!cell || RAY_IS_ERR(cell)) { err = cell ? cell : q_err(QE_OOM); break; }
            if (col && col->type == RAY_LIST) {           /* explode nested cell */
                for (int64_t i = 0; i < cnt && !err; i++) {
                    ray_t* ib = ray_i64(i);
                    ray_t* e = ray_at_fn(cell, ib);
                    ray_release(ib);
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
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* cc = q_list_collapse(acc[c]);
        ray_release(acc[c]);
        if (!RAY_IS_ERR(out)) {
            if (cc && !RAY_IS_ERR(cc)) {
                out = ray_table_add_col(out, ray_table_col_name(flat, c), cc);
            } else {
                ray_release(out);
                out = cc ? cc : q_err(QE_OOM);
                cc = NULL;
            }
        }
        if (cc && !RAY_IS_ERR(cc)) ray_release(cc);
        else if (cc && RAY_IS_ERR(cc) && out != cc) ray_release(cc);
    }
    ray_release(flat);
    return out;
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
            if (col_index(flat, ray_table_col_name(src, c)) < 0) {
                ray_release(src);
                return q_err(QE_MISMATCH);
            }
        }
        int64_t nr = ray_table_nrows(src);
        ray_t* out = ray_table_new(nc);
        for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
            int64_t nm = ray_table_col_name(flat, c);
            int64_t sc = col_index(src, nm);
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
                if (col_index(flat, id) < 0) return q_err(QE_MISMATCH);
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
            ray_t* col = bcast_col(cellv, 1);
            ray_release(cellv);
            if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : q_err(QE_OOM); }
            out = ray_table_add_col(out, nm, col);
            ray_release(col);
        }
        return out;
    }

    if (nc == 1 && y->type != RAY_LIST) {
        ray_t* col;
        if (ray_is_atom(y)) col = bcast_col(y, 1);
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
            ray_t* ia = ray_i64(c);
            ray_t* it = ray_at_fn(y, ia);
            ray_release(ia);
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
            ray_t* ia = ray_i64(c);
            ray_t* it = ray_at_fn(y, ia);
            ray_release(ia);
            if (!it || RAY_IS_ERR(it)) { ray_release(out); return it ? it : q_err(QE_OOM); }
            ray_t* col;
            if (ray_is_atom(it)) { col = bcast_col(it, L); ray_release(it); }
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
            ray_t* ia = ray_i64(r);
            ray_t* rec = ray_at_fn(y, ia);
            ray_release(ia);
            if (!rec || RAY_IS_ERR(rec) ||
                !(ray_is_vec(rec) || rec->type == RAY_LIST) || ray_len(rec) != nc) {
                if (rec && RAY_IS_ERR(rec)) err = rec;
                else { if (rec) ray_release(rec); err = q_err(QE_LENGTH); }
                break;
            }
            for (int64_t c = 0; c < nc && !err; c++) {
                ray_t* ib = ray_i64(c);
                ray_t* cell = ray_at_fn(rec, ib);
                ray_release(ib);
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
        ray_t* out = ray_table_new(nc);
        for (int64_t c = 0; c < nc; c++) {
            ray_t* cc = q_list_collapse(accs[c]);
            ray_release(accs[c]);
            if (!RAY_IS_ERR(out)) {
                if (cc && !RAY_IS_ERR(cc)) {
                    out = ray_table_add_col(out, ray_table_col_name(flat, c), cc);
                } else {
                    ray_release(out);
                    out = cc ? cc : q_err(QE_OOM);
                    cc = NULL;
                }
            }
            if (cc && !RAY_IS_ERR(cc)) ray_release(cc);
        }
        return out;
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
                if (row_eq(rows, r, kt, e, nkey)) {
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
            ray_t* ia = ray_i64(r);
            ray_t* cell = ray_at_fn(ray_table_get_col_idx(flat, c), ia);
            ray_release(ia);
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
                ray_t* ia = ray_i64(r);
                ray_t* nv = ray_at_fn(ray_table_get_col_idx(rows, c), ia);
                ray_release(ia);
                if (!nv || RAY_IS_ERR(nv)) { err = nv ? nv : q_err(QE_OOM); break; }
                ray_t** cells = (ray_t**)ray_data(colv[c]);
                eq = cell_eq(cells[e], nv);
                ray_release(nv);
            }
            if (!err && eq) hit = e;
        }
        for (int64_t c = 0; c < nc && !err; c++) {
            ray_t* ia = ray_i64(r);
            ray_t* nv = ray_at_fn(ray_table_get_col_idx(rows, c), ia);
            ray_release(ia);
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
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* cc = q_list_collapse(colv[c]);
        ray_release(colv[c]);
        if (!RAY_IS_ERR(out)) {
            if (cc && !RAY_IS_ERR(cc)) {
                out = ray_table_add_col(out, ray_table_col_name(flat, c), cc);
            } else {
                ray_release(out);
                out = cc ? cc : q_err(QE_OOM);
                cc = NULL;
            }
        }
        if (cc && !RAY_IS_ERR(cc)) ray_release(cc);
    }
    return out;
}

/* q `x upsert y` — plain table appends; keyed table updates-or-appends by
 * key.  Value target returns the new table; a NAMED target rebinds the
 * global and returns the name (unbound name + table payload creates it). */
ray_t* q_upsert_wrap(ray_t* x, ray_t* y) {
    int64_t sym;
    ray_t* t = table_operand(x, &sym);
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

/* ---- generic item access (joins wave) -------------------------------------
 * One boxed item of any sequence: strings iterate CHARS (1-char -RAY_STR
 * cells, string-model shim), atoms behave as 1-item lists.  Owned result. */
ray_t* qj_item(ray_t* x, int64_t i) {
    ray_t* ia = ray_i64(i);
    ray_t* e = ray_at_fn(x, ia);
    ray_release(ia);
    return e;
}
ray_t* qj_gen_item(ray_t* x, int64_t i) {
    if (x->type == -RAY_STR) return ray_str(ray_str_ptr(x) + i, 1);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    return qj_item(x, i);
}
/* generic item count matching qj_gen_item (atoms 1, strings char count);
 * -1 for non-sequences (dict). */
static int64_t qj_gen_len(ray_t* x) {
    if (!x) return -1;
    if (x->type == -RAY_STR) return (int64_t)ray_str_len(x);
    if (ray_is_atom(x)) return 1;
    if (x->type == RAY_TABLE) return ray_table_nrows(x);
    if (ray_is_vec(x) || x->type == RAY_LIST) return ray_len(x);
    return -1;
}

ray_t* qj_ktbl_merge(ray_t* x, ray_t* y, int mode);       /* fwd */
int    qj_same_schema(ray_t* a, ray_t* b);                /* fwd */

/* q `x,y` join — table , record-dict appends the record (ref/join.md +
 * ref/upsert.md: a simple table's Join of a matching record is the same
 * append upsert performs).  Joins wave: non-conforming table,table is
 * 'mismatch (ref/uj.md pins `s,t` -> 'mismatch; uj is the column-union
 * generalization); keyed,keyed is the uj upsert merge (ref/coalesce.md
 * `kt1,kt3`); and a base-concat 'type on list-joinable operands falls back
 * to a GENERIC boxed list (kdb `,` never type-errors on a list join —
 * ref/join.md `1 2,"a"`).  Every other operand pair delegates to base concat
 * (register_binary("concat") == ray_concat_fn) byte-identically — dict,dict
 * upsert-union and conforming table,table row-join already live there. */
ray_t* q_join_wrap(ray_t* x, ray_t* y) {
    if (x && y && x->type == RAY_TABLE && y->type == RAY_TABLE &&
        !qj_same_schema(x, y))
        return q_err(QE_MISMATCH);
    if (q_type_is_keyed(x) && q_type_is_keyed(y))
        return qj_ktbl_merge(x, y, 0);     /* upsert: y records win wholesale */
    if (x && x->type == RAY_TABLE && y && y->type == RAY_DICT && !q_type_is_keyed(y))
        return q_upsert_wrap(x, y);
    /* A bare dict joins ONLY with a dict (ref/join.md: `10,d` -> 'type; base
     * concat would wrongly DISTRIBUTE the scalar over the dict's values). */
    {
        int xd = x && x->type == RAY_DICT && !q_type_is_keyed(x);
        int yd = y && y->type == RAY_DICT && !q_type_is_keyed(y);
        if (xd != yd)
            return q_err(QE_TYPE);
    }
    ray_t* r = ray_concat_fn(x, y);
    if (r && !RAY_IS_ERR(r)) {
        /* dict upsert-union: the merged VALUES unify like any join result
         * (`~` is type-strict, so `(update c:3 from `a`b!1 2)~`a`b`c!1 2 3`
         * needs vector values) */
        if (q_type_is_dict(r) && !q_type_is_keyed(r)) {
            ray_t* v = ray_dict_vals(r);                       /* borrowed */
            ray_t* cv = v ? q_list_collapse(v) : NULL;         /* no-op off-list */
            if (cv && !RAY_IS_ERR(cv) && cv != v) {
                ray_t* k = ray_dict_keys(r);
                ray_retain(k);                        /* dict_new consumes both */
                ray_t* nd = ray_dict_new(k, cv);
                ray_release(r);
                return nd ? nd : q_err(QE_TYPE);
            }
            if (cv) ray_release(cv);
            return r;
        }
        /* `()` is Join's IDENTITY and identity must not retype: base concat
         * boxes the untyped empty into the result, so `(),2` came back 0h
         * where kdb says 7h.  That is the seed the `,` accumulator starts from
         * (ref/accumulators.md:264), so every partial inherited the boxing.
         * The collapse home already leaves mixed lists (`1 2,"a"`) alone. */
        ray_t* c = q_list_collapse(r);
        ray_release(r);
        return c;
    }
    if (!x || !y) return r;
    /* boxed-list fallback — ONLY when a char/string operand is involved
     * (ref/join.md "otherwise a mixed list"; ref/cross.md needs `2 10,"a"`
     * -> (2;10;"a")).  Deliberately NARROW: banked ledgers pin `,:` appends
     * of incompatible non-char items as 'type (assign/identity `x,:`a`,
     * list/join `s,:5f`), and the wrapper cannot tell plain `,` from the
     * in-place `,:` amend — so non-char incompatibles keep the base error
     * (error beats a wrong answer; the wider kdb mixed-list rule is a
     * deferred cell). */
    int64_t nx = qj_gen_len(x), ny = qj_gen_len(y);
    int x_chr = x->type == -RAY_STR || x->type == RAY_CHARV || x->type == -RAY_CHARV;
    int y_chr = y->type == -RAY_STR || y->type == RAY_CHARV || y->type == -RAY_CHARV;
    if (nx < 0 || ny < 0 || (!x_chr && !y_chr) ||
        x->type == RAY_DICT || y->type == RAY_DICT ||
        x->type == RAY_TABLE || y->type == RAY_TABLE)
        return r;                          /* keep the base error */
    ray_t* out = ray_list_new(nx + ny > 0 ? nx + ny : 1);
    if (RAY_IS_ERR(out)) { if (r) ray_release(r); return out; }
    for (int64_t i = 0; i < nx + ny; i++) {
        ray_t* e = (i < nx) ? qj_gen_item(x, i) : qj_gen_item(y, i - nx);
        if (!e || RAY_IS_ERR(e)) {
            ray_release(out);
            if (e) { if (r) ray_release(r); return e; }
            return r;
        }
        out = ray_list_append(out, e);
        ray_release(e);
        if (RAY_IS_ERR(out)) { if (r) ray_release(r); return out; }
    }
    if (r) ray_release(r);
    return out;
}

/* ---- table set-ops core (distinct/union/except/inter arms) --------------- */

/* Indices of x-rows [not] present in y (whole-row membership). */
static ray_t* table_member_idx(ray_t* x, ray_t* y, int keep_present) {
    int64_t nrx = ray_table_nrows(x), nry = ray_table_nrows(y);
    int64_t ncx = ray_table_ncols(x);
    if (ncx != ray_table_ncols(y)) return q_err(QE_MISMATCH);
    ray_t* idx = ray_vec_new(RAY_I64, nrx > 0 ? nrx : 1);
    if (RAY_IS_ERR(idx)) return idx;
    idx->len = 0;
    for (int64_t r = 0; r < nrx; r++) {
        int found = 0;
        for (int64_t e = 0; e < nry && !found; e++)
            found = row_eq(x, r, y, e, ncx);
        if (found == keep_present) {
            idx = ray_vec_append(idx, &r);
            if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_OOM);
        }
    }
    return idx;
}

/* Gather table rows by index vector (columns via ray_at_fn + collapse). */
static ray_t* table_gather(ray_t* t, ray_t* idx) {
    int64_t nc = ray_table_ncols(t);
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
        ray_t* col = ray_at_fn(ray_table_get_col_idx(t, c), idx);
        if (col && col->type == RAY_LIST) {
            ray_t* cc = q_list_collapse(col);
            ray_release(col);
            col = cc;
        }
        if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : q_err(QE_OOM); }
        out = ray_table_add_col(out, ray_table_col_name(t, c), col);
        ray_release(col);
    }
    return out;
}

/* q `distinct t` — FIRST-OCCURRENCE row dedup.  The base DAG table-distinct
 * (ray_table_distinct_fn) sorts, so it is NOT reused — the same reason the q
 * vector distinct is a wrapper. */
static ray_t* table_distinct(ray_t* t) {
    int64_t nr = ray_table_nrows(t);
    int64_t nc = ray_table_ncols(t);
    ray_t* idx = ray_vec_new(RAY_I64, nr > 0 ? nr : 1);
    if (RAY_IS_ERR(idx)) return idx;
    idx->len = 0;
    for (int64_t r = 0; r < nr; r++) {
        int dup = 0;
        int64_t* kept = (int64_t*)ray_data(idx);
        for (int64_t j = 0; j < idx->len && !dup; j++)
            dup = row_eq(t, r, t, kept[j], nc);
        if (!dup) {
            idx = ray_vec_append(idx, &r);
            if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_OOM);
        }
    }
    ray_t* out = table_gather(t, idx);
    ray_release(idx);
    return out;
}

/* q `x except y` — table pair: rows of x not in y (x order and duplicates
 * kept, then per kdb the RESULT is over distinct rows of x — ref/except.md
 * operates on items; for tables kdb dedups via distinct semantics of the
 * underlying find, so keep it simple: rows of x not in y, x-dups kept).
 * Non-table operands delegate to base ray_except_fn (pre-wave behaviour). */
ray_t* q_except_wrap(ray_t* x, ray_t* y) {
    /* keyed tables / dicts are deferred cells — the base list kernel would
     * mangle the dict structure (mirror of the inter guard). */
    if ((x && x->type == RAY_DICT) || (y && y->type == RAY_DICT))
        return q_err(QE_NYI);
    if (x && x->type == RAY_TABLE && y && y->type == RAY_TABLE) {
        ray_t* idx = table_member_idx(x, y, 0);
        if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_OOM);
        ray_t* r = table_gather(x, idx);
        ray_release(idx);
        return r;
    }
    return ray_except_fn(x, y);
}

/* q `key x` (ref/key.md) — dict keys, plus the name/namespace overloads:
 *   `` ` ``      -> root context roster (namespaces other than .z)
 *   `` `. ``     -> objects in the root (user variable names)
 *   `` `.foo ``  -> the context's keys (leading `` ` `` placeholder + members)
 *   `` `name ``  -> keys of the named dict; the sym itself if the name is
 *                   bound to a non-dict; `()` if unbound (context-aware)
 * File handles (`` `:path ``) are the file-I/O wave: 'nyi.  Everything else
 * non-dict stays a deferred 'type cell. */
ray_t* q_key_wrap(ray_t* x) {
    /* type of a vector (ref/key.md): `key 0#5` -> `long; a native string
     * atom IS the provisional char vector -> `char; `key 10` -> til 10. */
    if (x && ray_is_vec(x)) {
        const char* nm = q_type_qname(x->type);
        if (nm) return ray_sym(ray_sym_intern_runtime(nm, strlen(nm)));
        /* unnamed vector types keep the deferred 'type tail below */
    }
    if (x && x->type == -RAY_STR)
        return ray_sym(ray_sym_intern_runtime("char", 4));
    if (q_type_is_int_atom(x) && !RAY_ATOM_IS_NULL(x) && q_type_iatom_val(x) >= 0)
        return q_til_wrap(x);                       /* key n == til n */
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        if (!s) return q_err(QE_TYPE);
        const char* nm = ray_str_ptr(s);
        size_t l = ray_str_len(s);
        if (l == 0) {
            ray_release(s);
            ray_t* r = q_env_ns_roster();
            return r ? r : q_err(QE_WSFULL);
        }
        if (nm[0] == ':') {
            ray_release(s);
            return q_err(QE_NYI);
        }
        if (l == 1 && nm[0] == '.') {           /* `. — root objects, marker-free
                                                 * (ref/key.md: `key `.` lists names
                                                 * only; namespaces keep the marker) */
            ray_release(s);
            ray_t* root = q_env_resolve(x->i64);
            if (!root || root->type != RAY_DICT) {
                if (root) ray_release(root);
                return q_err(QE_NYI);
            }
            ray_t* marker = ray_sym(ray_sym_intern_runtime("", 0));
            if (!marker || RAY_IS_ERR(marker)) {
                ray_release(root);
                return marker ? marker : q_err(QE_WSFULL);
            }
            ray_t* bare = ray_dict_remove(root, marker);   /* consumes root */
            ray_release(marker);
            if (!bare || RAY_IS_ERR(bare)) return bare ? bare : q_err(QE_WSFULL);
            ray_t* rk = ray_dict_keys(bare);
            if (!rk) { ray_release(bare); return q_err(QE_TYPE); }
            ray_retain(rk);
            ray_release(bare);
            return rk;
        }
        ray_release(s);
        /* named variable / namespace: dict (incl. a context's dict) -> keys;
         * bound -> the sym itself; unbound -> () (ref/key.md). */
        ray_t* v = q_env_resolve(x->i64);
        if (!v) return ray_list_new(1);         /* () — empty general list */
        if (RAY_IS_ERR(v)) return v;
        if (v->type == RAY_DICT) {
            ray_t* k = ray_dict_keys(v);
            if (!k) { ray_release(v); return q_err(QE_TYPE); }
            ray_retain(k);
            ray_release(v);
            return k;
        }
        ray_release(v);
        ray_retain(x);
        return x;
    }
    if (!x || x->type != RAY_DICT)
        return q_err(QE_TYPE);
    ray_t* k = ray_dict_keys(x);                /* borrowed */
    if (!k) return q_err(QE_TYPE);
    ray_retain(k);
    return k;
}

/* q `nam set y` (ref/get.md) — assign a global through a symbol handle:
 *   `a set 42        -> bind the global (dotted names create contexts)
 *   `.foo set d      -> ordinary rebind, `:`-identical — the old namespace
 *                       dict goes with the name (no splat: dict model)
 *   `. set d         -> restore root variables from dict d
 *   `:f set y        -> write a kdb+ flat file (q_wirefile_write)
 * The splayed form and the compressed (file;lbs;alg;lvl) left-arguments are a
 * later wave: 'nyi.  Returns the handle (kdb returns nam). */
ray_t* q_setg_wrap(ray_t* x, ray_t* y) {
    if (!x || x->type != -RAY_SYM)
        return q_err(QE_NYI);
    ray_t* s = ray_sym_str(x->i64);
    if (!s) return q_err(QE_TYPE);
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    if (l == 0) {
        ray_release(s);
        return q_err(QE_TYPE);
    }
    if (nm[0] == ':') {                         /* file handle: q_wirefile writes it */
        ray_release(s);
        ray_t* r = q_wirefile_write(x, y);
        return r ? r : q_err(QE_TYPE);
    }
    int is_root = (l == 1 && nm[0] == '.');
    if (is_root && y && y->type == RAY_DICT) {
        /* root restore: upsert each member as a plain global */
        ray_t* dk = ray_dict_keys(y);           /* borrowed */
        ray_t* dv = ray_dict_vals(y);           /* borrowed */
        int64_t n = ray_dict_len(y);
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* k = ray_at_fn(dk, ia);       /* owned */
            ray_t* v = ray_at_fn(dv, ia);       /* owned */
            ray_release(ia);
            if (!k || RAY_IS_ERR(k) || k->type != -RAY_SYM || !v || RAY_IS_ERR(v)) {
                if (k && !RAY_IS_ERR(k)) ray_release(k);
                if (v && !RAY_IS_ERR(v)) ray_release(v);
                ray_release(s);
                return q_err(QE_TYPE);
            }
            ray_t* ks = ray_sym_str(k->i64);
            int skip = !ks || ray_str_len(ks) == 0;   /* :: placeholder */
            if (ks) ray_release(ks);
            ray_err_t err = skip ? RAY_OK : q_env_set(k->i64, v);
            ray_release(k);
            ray_release(v);
            if (err != RAY_OK) {
                ray_release(s);
                return ray_error(ray_err_code_str(err), "set: assign failed");
            }
        }
        ray_release(s);
        ray_retain(x);
        return x;
    }
    if (is_root) {                              /* `. set non-dict: no reading */
        ray_release(s);
        return q_err(QE_TYPE);
    }
    /* Settable `.z.*` handler slots (`.z.ts`/`.z.exit`/`.z.p*`/`.z.w*`/`.z.ac`) —
     * NOT `.ipc.on.*` hooks.  dotz.c owns the name->slot dispatch AND the
     * {…}-carrier unwrap (call_fn1 fires a bare lambda); q_dotz_set declines any
     * non-handler name so it falls through to the `.ipc.on.*`/plain-env path. */
    if (q_dotz_write_is_nyi(nm, l)) {
        ray_release(s);
        return q_err(QE_NYI);
    }
    if (q_dotz_set(nm, l, y)) {
        ray_release(s);
        ray_retain(x);
        return x;
    }
    /* kdb `.z.p*` connection-handler aliases write the `.ipc.on.*` slot that
     * ipc.c's hook_lookup reads — the six-slot callback table stays in
     * RAYFALL's env (the one deliberate seam; a direct `.ipc.on.*` set no
     * longer reaches it, closing the write-side leak).  A q `{…}` binds
     * AS-IS: RAY_QFN carriers fire through the value-apply seam (ipc.c
     * hook_fire).  (hk computed BEFORE ray_release(s): `nm` points into `s`.) */
    int hk = q_dotz_ipc_hook_index(nm, l);
    ray_release(s);
    ray_err_t err = hk >= 0 ? ray_env_set(ray_sym_ipc_hook(hk), y)
                            : q_env_set(x->i64, y);
    if (err != RAY_OK)
        return ray_error(ray_err_code_str(err), "set: assign failed");
    ray_retain(x);
    return x;
}


/* q `distinct x` / monadic `?` — unique items in FIRST-OCCURRENCE order
 * (kdb).  rayfall's distinct routes typed vectors through the DAG group
 * path, which SORTS — a rename would pin wrong answers, so this is a
 * match-based dedup (type-strict, nulls equal — the ~ semantics kdb's
 * distinct uses), collapsed back to a typed vector.  String operands are
 * a deferred cell (string model); atoms are kdb 'type. */
ray_t* q_distinct_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == RAY_TABLE) return table_distinct(x);   /* row dedup */
    if (x->type == -RAY_STR)
        return q_err(QE_NYI);
    if (!ray_is_vec(x) && x->type != RAY_LIST)
        return q_err(QE_TYPE);
    int64_t n = ray_len(x);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(x, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
        int dup = 0;
        int64_t m = ray_len(out);
        ray_t** oe = (ray_t**)ray_data(out);
        for (int64_t j = 0; j < m && !dup; j++) dup = q_match_rec(oe[j], e);
        if (!dup) {
            out = ray_list_append(out, e);
            if (RAY_IS_ERR(out)) { ray_release(e); return out; }
        }
        ray_release(e);
    }
    ray_t* c = q_list_collapse(out);
    ray_release(out);
    return c;
}

/* q `x union y` — `distinct x,y` (ref/union.md).  A wrapper because rayfall's
 * ray_union_fn KEEPS x-duplicates (it only filters y against x); kdb dedups
 * the whole join in first-occurrence order.  Reuses q join (`,` == rayfall
 * concat) + the q distinct wrapper above — no new set logic.  Operands the
 * distinct wrapper defers (strings, tables) defer here too: error, never a
 * wrong answer. */
ray_t* q_union_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    /* keyed tables / dicts are deferred cells (mirror of the inter guard). */
    if (x->type == RAY_DICT || y->type == RAY_DICT)
        return q_err(QE_NYI);
    if (x->type == RAY_TABLE && y->type == RAY_TABLE) {   /* distinct of t,u */
        ray_t* j = ray_concat_fn(x, y);
        if (!j || RAY_IS_ERR(j)) return j ? j : q_err(QE_OOM);
        ray_t* r = table_distinct(j);
        ray_release(j);
        return r;
    }
    ray_t* j = ray_concat_fn(x, y);
    if (!j || RAY_IS_ERR(j)) return j;
    ray_t* r = q_distinct_wrap(j);
    ray_release(j);
    return r;
}

/* q `x inter y` — items of x that are in y, x-duplicates and order kept
 * (ref/inter.md).  rayfall `sect` (ray_sect_fn) IS this for lists, but on
 * DICT operands it returns a wrong-shaped dict where kdb returns the common
 * VALUES as a list — so dict/table operands are guarded 'nyi (error, never a
 * wrong answer); everything else delegates to ray_sect_fn. */
ray_t* q_inter_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    if (x->type == RAY_TABLE && y->type == RAY_TABLE) {   /* rows of x in y */
        ray_t* idx = table_member_idx(x, y, 1);
        if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_OOM);
        ray_t* r = table_gather(x, idx);
        ray_release(idx);
        return r;
    }
    /* dict arm (ref/inter.md): the common VALUE items of two dicts, as a
     * list — recurse on the value lists (boxed whole-item membership). */
    if (x->type == RAY_DICT && y->type == RAY_DICT &&
        !q_type_is_keyed(x) && !q_type_is_keyed(y)) {
        ray_t* vx = ray_dict_vals(x);                      /* borrowed */
        ray_t* vy = ray_dict_vals(y);                      /* borrowed */
        if (!vx || !vy) return q_err(QE_TYPE);
        return q_inter_wrap(vx, vy);
    }
    if (x->type == RAY_DICT || x->type == RAY_TABLE ||
        y->type == RAY_DICT || y->type == RAY_TABLE)
        return q_err(QE_NYI);
    /* generic-list operands: whole-ITEM membership scan (base ray_sect_fn
     * flattens/mangles boxed items) — kdb keeps x items (dups kept) in y. */
    if (x->type == RAY_LIST || y->type == RAY_LIST) {
        int64_t nx = ray_len(x);
        int64_t ny = (ray_is_vec(y) || y->type == RAY_LIST) ? ray_len(y) : -1;
        if (ny < 0) return ray_sect_fn(x, y);
        ray_t* out = ray_list_new(nx > 0 ? nx : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < nx; i++) {
            ray_t* xi = qj_item(x, i);
            if (!xi || RAY_IS_ERR(xi)) { ray_release(out); return xi ? xi : q_err(QE_TYPE); }
            int found = 0;
            for (int64_t j = 0; j < ny && !found; j++) {
                ray_t* yj = qj_item(y, j);
                if (!yj || RAY_IS_ERR(yj)) { ray_release(xi); ray_release(out); return yj ? yj : q_err(QE_TYPE); }
                found = q_match_rec(xi, yj);
                ray_release(yj);
            }
            if (found) {
                out = ray_list_append(out, xi);
                if (RAY_IS_ERR(out)) { ray_release(xi); return out; }
            }
            ray_release(xi);
        }
        ray_t* c = q_list_collapse(out);
        ray_release(out);
        return c;
    }
    return ray_sect_fn(x, y);
}

/* q `x cross y` — Cartesian product, `{raze x,/:\:y}` (ref/cross.md): for
 * each item a of x (in order), for each item b of y, the JOIN `a,b`.
 * Composes existing primitives (ray_at_fn item access + q join == rayfall
 * concat) — rayfall has no cartesian primitive.  Atom operands behave as
 * one-item lists (each-left/right over an atom).  Deferred cells ('nyi,
 * never a wrong answer): string operands (kdb iterates a string's CHARS;
 * openq strings are -RAY_STR atoms — string model) and dict/table cross
 * (kdb cross-joins tables). */
ray_t* q_cross_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    if (x->type == RAY_DICT || y->type == RAY_DICT)
        return q_err(QE_NYI);
    /* table cross table: the cartesian-product table (ref/cross.md) */
    if (x->type == RAY_TABLE && y->type == RAY_TABLE) {
        for (int64_t c = 0; c < ray_table_ncols(y); c++)
            if (ray_table_get_col(x, ray_table_col_name(y, c)))
                return q_err(QE_TYPE);
        int64_t nxr = ray_table_nrows(x), nyr = ray_table_nrows(y);
        int64_t n = nxr * nyr;
        int64_t* xi = (int64_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
        int64_t* yi = (int64_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
        if (!xi || !yi) { free(xi); free(yi); return q_err(QE_WSFULL); }
        for (int64_t i = 0; i < n; i++) { xi[i] = i / nyr; yi[i] = i % nyr; }
        ray_t* xt = qj_table_gather_idx(x, xi, n);
        free(xi);
        if (!xt || RAY_IS_ERR(xt)) { free(yi); return xt ? xt : q_err(QE_TYPE); }
        ray_t* yt = qj_table_gather_idx(y, yi, n);
        free(yi);
        if (!yt || RAY_IS_ERR(yt)) { ray_release(xt); return yt ? yt : q_err(QE_TYPE); }
        for (int64_t c = 0; c < ray_table_ncols(yt); c++) {
            ray_t* col = ray_table_get_col_idx(yt, c);     /* borrowed */
            xt = ray_table_add_col(xt, ray_table_col_name(yt, c), col);
            if (RAY_IS_ERR(xt)) { ray_release(yt); return xt; }
        }
        ray_release(yt);
        return xt;
    }
    if (x->type == RAY_TABLE || y->type == RAY_TABLE)
        return q_err(QE_TYPE);
    /* strings iterate their CHARS; atoms act as 1-item lists (qj_gen_*) */
    int64_t nx = qj_gen_len(x), ny = qj_gen_len(y);
    if (nx < 0 || ny < 0)
        return q_err(QE_TYPE);
    ray_t* out = ray_list_new(nx * ny > 0 ? nx * ny : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < nx; i++) {
        ray_t* a = qj_gen_item(x, i);
        if (!a || RAY_IS_ERR(a)) { ray_release(out); return a ? a : q_err(QE_TYPE); }
        for (int64_t j = 0; j < ny; j++) {
            ray_t* b = qj_gen_item(y, j);
            if (!b || RAY_IS_ERR(b)) { ray_release(a); ray_release(out); return b ? b : q_err(QE_TYPE); }
            /* pair items joined with q `,` (boxed fallback for mixed types) */
            ray_t* p = q_join_wrap(a, b);
            ray_release(b);
            if (!p || RAY_IS_ERR(p)) { ray_release(a); ray_release(out); return p ? p : q_err(QE_TYPE); }
            out = ray_list_append(out, p);
            ray_release(p);
            if (RAY_IS_ERR(out)) { ray_release(a); return out; }
        }
        ray_release(a);
    }
    ray_t* c = q_list_collapse(out);
    ray_release(out);
    return c;
}

/* ===== table introspection: cols / meta (evicted from q_builtins.c) ===== */
/* Column name ids of a table as a RAY_SYM vector.  A keyed table (RAY_DICT of
 * key-table -> value-table) yields key cols ++ value cols. */
static ray_t* table_colnames(ray_t* x) {
    if (x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, nc > 0 ? nc : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        for (int64_t c = 0; c < nc; c++) {
            int64_t nm = ray_table_col_name(x, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        }
        return out;
    }
    if (x->type == RAY_DICT) {
        ray_t* kt = ray_dict_keys(x);      /* borrowed */
        ray_t* vt = ray_dict_vals(x);      /* borrowed */
        if (!kt || !vt || kt->type != RAY_TABLE || vt->type != RAY_TABLE)
            return q_err(QE_TYPE);
        int64_t knc = ray_table_ncols(kt), vnc = ray_table_ncols(vt);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, knc + vnc > 0 ? knc + vnc : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        for (int64_t c = 0; c < knc; c++) {
            int64_t nm = ray_table_col_name(kt, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        }
        for (int64_t c = 0; c < vnc; c++) {
            int64_t nm = ray_table_col_name(vt, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        }
        return out;
    }
    return q_err(QE_TYPE);
}

/* Resolve a by-name table operand (cols`t / meta`t): a -RAY_SYM naming a
 * global plain/keyed table resolves to it (borrowed); else x unchanged. */
static ray_t* table_bi_deref(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* g = q_env_get(x->i64);                   /* borrowed */
        if (g && (g->type == RAY_TABLE || q_type_is_keyed(g))) return g;
    }
    return x;
}

/* (cols x) — column names of a table as a symbol vector. */
ray_t* q_cols_fn(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    return table_colnames(table_bi_deref(x));
}

/* Flatten a plain-or-keyed table to a single plain RAY_TABLE (key cols first).
 * Returns owned (retained for a plain table). */
static ray_t* table_meta_flatten(ray_t* x) {
    if (x->type == RAY_TABLE) { ray_retain(x); return x; }
    if (x->type != RAY_DICT) return q_err(QE_TYPE);
    ray_t* kt = ray_dict_keys(x);          /* borrowed */
    ray_t* vt = ray_dict_vals(x);          /* borrowed */
    if (!kt || !vt || kt->type != RAY_TABLE || vt->type != RAY_TABLE)
        return q_err(QE_TYPE);
    int64_t knc = ray_table_ncols(kt), vnc = ray_table_ncols(vt);
    ray_t* out = ray_table_new(knc + vnc > 0 ? knc + vnc : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    for (int64_t c = 0; c < knc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(kt, c), ray_table_get_col_idx(kt, c));
    for (int64_t c = 0; c < vnc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(vt, c), ray_table_get_col_idx(vt, c));
    return out;
}

/* (meta x) — table metadata keyed by column name.  Builds the keyed table
 * (c) -> (t; f; a): `c` column names, `t` per-column type char (via the
 * single-home map), `f`/`a` blank (foreign-keys/attributes are out of scope).
 * The result is a RAY_DICT from a 1-col key table to a 3-col value table —
 * "a keyed table is just a dictionary from one table to another" (q_fmt
 * renders it `k| v`). */
ray_t* q_meta_fn(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    ray_t* flat = table_meta_flatten(table_bi_deref(x));
    if (!flat || RAY_IS_ERR(flat)) return flat;
    int64_t nc = ray_table_ncols(flat);
    int64_t cap = nc > 0 ? nc : 1;
    ray_t* cvec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* c: names          */
    ray_t* fvec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* f: blank per col  */
    ray_t* avec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* a: blank per col  */
    char stackt[64];
    char* tbuf = (cap <= (int64_t)sizeof stackt) ? stackt : (char*)malloc((size_t)cap);
    if (!cvec || RAY_IS_ERR(cvec) || !fvec || RAY_IS_ERR(fvec) ||
        !avec || RAY_IS_ERR(avec) || !tbuf) {
        if (cvec && !RAY_IS_ERR(cvec)) ray_release(cvec);
        if (fvec && !RAY_IS_ERR(fvec)) ray_release(fvec);
        if (avec && !RAY_IS_ERR(avec)) ray_release(avec);
        if (tbuf && tbuf != stackt) free(tbuf);
        ray_release(flat);
        return q_err(QE_WSFULL);
    }
    int64_t blank = ray_sym_intern_runtime("", 0);
    int ok = 1;
    for (int64_t c = 0; c < nc && ok; c++) {
        int64_t nm = ray_table_col_name(flat, c);
        ray_t* col = ray_table_get_col_idx(flat, c);   /* borrowed */
        tbuf[c] = q_ty_char(col);
        cvec = ray_vec_append(cvec, &nm);
        fvec = ray_vec_append(fvec, &blank);
        avec = ray_vec_append(avec, &blank);
        if (!cvec || RAY_IS_ERR(cvec) || !fvec || RAY_IS_ERR(fvec) ||
            !avec || RAY_IS_ERR(avec)) ok = 0;
    }
    ray_release(flat);
    ray_t* tstr = ok ? ray_str(tbuf, (size_t)nc) : NULL;
    if (tbuf != stackt) free(tbuf);
    if (!ok || !tstr || RAY_IS_ERR(tstr)) {
        if (cvec && !RAY_IS_ERR(cvec)) ray_release(cvec);
        if (fvec && !RAY_IS_ERR(fvec)) ray_release(fvec);
        if (avec && !RAY_IS_ERR(avec)) ray_release(avec);
        if (tstr && !RAY_IS_ERR(tstr)) ray_release(tstr);
        return q_err(QE_WSFULL);
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
