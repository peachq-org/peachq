/* q_wrap_join.c — the q join family (feat/q-joins-rebuild): lj/ljf ij/ijf ej uj/ujf
 * pj aj-family asof wj/wj1 + keyed-table lookup
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "lang/env.h"      /* ray_env_get */
#include "lang/eval.h"     /* ray_eval; ray_left_join_fn, ray_window_join*_fn */
#include "lang/internal.h" /* ray_asof_join_fn, ray_concat_fn, ray_vec_set_null, ray_error */
#include "table/sym.h"     /* ray_sym_intern_runtime, RAY_SYM_W64 */
#include <stdio.h>         /* snprintf */
#include <string.h>        /* strlen */
#include <stdlib.h>        /* malloc/free, qsort */

/* ===== q join family (feat/q-joins-rebuild) =================================
 * lj/ljf, ij/ijf, ej, uj/ujf, pj, aj/aj0/ajf/ajf0, asof, wj/wj1 — ported from
 * PR #56's q_join.c (algorithms twice-reviewed there; the parallel-file
 * placement was the failure mode).  THE core rule: the hash/asof MATCHING is
 * ALWAYS the engine's (ray_left_join_fn / ray_asof_join_fn / window-join,
 * src/ops/join.c machinery); this section only prepares rowid-augmented key
 * tables, reorders the engine's match relation to kdb row order, and
 * assembles result columns by vector gather.  Keyed-table primitives are the
 * single-home helpers in q_wrap_list.c (q_is_keyed_table / q_enkey /
 * q_table_flatten) — never redefined.  Refcounts: wrapper args BORROWED, results OWNED;
 * ray_list_append/ray_table_add_col RETAIN (release local after);
 * ray_table_get_col* return BORROWED columns. */

/* q-level null test for one atom cell: symbol null is the interned empty
 * symbol (id 0); strings are never null; else RAY_ATOM_IS_NULL. */
static int qj_atom_is_qnull(ray_t* a) {
    if (!a || !ray_is_atom(a)) return 0;
    if (a->type == -RAY_SYM) return a->i64 == 0;
    if (a->type == -RAY_STR) return ray_str_len(a) == 1 && ray_str_ptr(a)[0] == ' ';
    return RAY_ATOM_IS_NULL(a);
}

/* Same column-name schema, same order? */
int qj_same_schema(ray_t* a, ray_t* b) {
    int64_t nc = ray_table_ncols(a);
    if (nc != ray_table_ncols(b)) return 0;
    for (int64_t c = 0; c < nc; c++)
        if (ray_table_col_name(a, c) != ray_table_col_name(b, c)) return 0;
    return 1;
}

/* Gather one column by an index map (idx[i] < 0 => null cell).  A char
 * column is a -RAY_STR atom (string model) — gathered char-by-char with a
 * blank for null; everything else goes through ray_at + collapse.  Owned. */
static ray_t* qj_col_gather(ray_t* col, const int64_t* idx, int64_t n) {
    if (col && col->type == -RAY_STR) {
        const char* sp = ray_str_ptr(col);
        int64_t sl = (int64_t)ray_str_len(col);
        char stackb[1024];
        char* b = (n < (int64_t)sizeof stackb) ? stackb : malloc((size_t)n + 1);
        if (!b) return ray_error("wsfull", "join: out of memory");
        for (int64_t i = 0; i < n; i++)
            b[i] = (idx[i] < 0 || idx[i] >= sl) ? ' ' : sp[idx[i]];
        ray_t* r = ray_str(b, (size_t)n);
        if (b != stackb) free(b);
        return r;
    }
    ray_t* iv = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(iv)) return iv;
    iv->len = n;
    for (int64_t i = 0; i < n; i++) {
        ((int64_t*)ray_data(iv))[i] = (idx[i] < 0) ? 0 : idx[i];
        if (idx[i] < 0) ray_vec_set_null(iv, i, true);
    }
    ray_t* g = ray_at_fn(col, iv);
    ray_release(iv);
    if (!g || RAY_IS_ERR(g)) return g ? g : ray_error("type", NULL);
    if (g->type == RAY_LIST) {
        ray_t* cg = q_collapse_list(g);
        ray_release(g);
        return cg;
    }
    return g;
}

/* Gather rows of table `t` by index map (idx[i] < 0 => null row cells). */
ray_t* qj_table_gather_idx(ray_t* t, const int64_t* idx, int64_t n) {
    int64_t nc = ray_table_ncols(t);
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
        ray_t* col = ray_table_get_col_idx(t, c);          /* borrowed */
        ray_t* cg = qj_col_gather(col, idx, n);
        if (!cg || RAY_IS_ERR(cg)) { ray_release(out); return cg ? cg : ray_error("type", NULL); }
        out = ray_table_add_col(out, ray_table_col_name(t, c), cg);
        ray_release(cg);
    }
    return out;
}

/* ---- pair-relation core (lj / ij / ej / uj / pj) ---------------------------
 * The MATCHING is the engine's hash left-join (ray_left_join_fn): both sides
 * are reduced to their key columns plus a fresh rowid column, joined, and the
 * two rowid columns read back as the match relation.  Pairs are then sorted
 * (left row asc, right row asc) because kdb pins left-row-major order with
 * right-table order within a key, while the engine's within-key order is
 * unspecified.  A miss is encoded r = -1 (engine null rowid). */

typedef struct { int64_t l, r; } qj_pair_t;

static int qj_pair_cmp(const void* a, const void* b) {
    const qj_pair_t* pa = (const qj_pair_t*)a;
    const qj_pair_t* pb = (const qj_pair_t*)b;
    if (pa->l != pb->l) return pa->l < pb->l ? -1 : 1;
    if (pa->r != pb->r) return pa->r < pb->r ? -1 : 1;
    return 0;
}

/* Collision-free temp column name: base + counter until the interned sym
 * names no column of either table. */
static int64_t qj_tmp_colname(const char* base, ray_t* t1, ray_t* t2) {
    char buf[32];
    for (int64_t k = 0;; k++) {
        snprintf(buf, sizeof buf, "%s%lld", base, (long long)k);
        int64_t id = ray_sym_intern_runtime(buf, strlen(buf));
        if (!ray_table_get_col(t1, id) && (!t2 || !ray_table_get_col(t2, id)))
            return id;
    }
}

/* 0..n-1 rowid column. */
static ray_t* qj_rowid_col(int64_t n) {
    ray_t* v = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(v)) return v;
    v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = i;
    return v;
}

/* Minimal join input: `keys` columns of t (borrowed, re-added under the same
 * names) plus a rowid column named rid_sym.  keys = RAY_LIST of sym atoms
 * (kept BOXED end-to-end: the engine unboxes itself and the helpers here
 * index ray_t** — never collapse to a sym vector). */
static ray_t* qj_keyid_table(ray_t* t, ray_t* keys, int64_t rid_sym) {
    int64_t nk = ray_len(keys);
    ray_t** ke = (ray_t**)ray_data(keys);
    ray_t* out = ray_table_new(nk + 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < nk; i++) {
        ray_t* col = ray_table_get_col(t, ke[i]->i64);     /* borrowed */
        if (!col) {
            ray_release(out);
            return ray_error("type", "join: key column not found in table");
        }
        out = ray_table_add_col(out, ke[i]->i64, col);
        if (RAY_IS_ERR(out)) return out;
    }
    ray_t* rid = qj_rowid_col(ray_table_nrows(t));
    if (RAY_IS_ERR(rid)) { ray_release(out); return rid; }
    out = ray_table_add_col(out, rid_sym, rid);
    ray_release(rid);
    return out;
}

/* Full sorted match relation between x rows and y rows on `keys` (RAY_LIST of
 * sym atoms; columns of both).  Returns pair count (>=0) with *out malloc'd
 * (caller frees), or -1 with *err set. */
static int64_t qj_join_pairs(ray_t* x, ray_t* y, ray_t* keys,
                             qj_pair_t** out, ray_t** err) {
    *out = NULL; *err = NULL;
    int64_t lrid = qj_tmp_colname("qjl", x, y);
    int64_t rrid = qj_tmp_colname("qjr", x, y);
    ray_t* lk = qj_keyid_table(x, keys, lrid);
    if (!lk || RAY_IS_ERR(lk)) { *err = lk ? lk : ray_error("type", NULL); return -1; }
    ray_t* rk = qj_keyid_table(y, keys, rrid);
    if (!rk || RAY_IS_ERR(rk)) { ray_release(lk); *err = rk ? rk : ray_error("type", NULL); return -1; }
    ray_t* args[3] = { keys, lk, rk };
    ray_t* res = ray_left_join_fn(args, 3);
    ray_release(lk);
    ray_release(rk);
    if (!res || RAY_IS_ERR(res)) { *err = res ? res : ray_error("type", NULL); return -1; }
    ray_t* lc = ray_table_get_col(res, lrid);              /* borrowed */
    ray_t* rc = ray_table_get_col(res, rrid);              /* borrowed */
    if (!lc || !rc || lc->type != RAY_I64 || rc->type != RAY_I64) {
        ray_release(res);
        *err = ray_error("type", "join: engine relation missing rowid columns");
        return -1;
    }
    int64_t np = ray_len(lc);
    qj_pair_t* pairs = (qj_pair_t*)malloc((size_t)(np > 0 ? np : 1) * sizeof(qj_pair_t));
    if (!pairs) { ray_release(res); *err = ray_error("wsfull", "join: out of memory"); return -1; }
    const int64_t* lv = (const int64_t*)ray_data(lc);
    const int64_t* rv = (const int64_t*)ray_data(rc);
    for (int64_t i = 0; i < np; i++) {
        pairs[i].l = lv[i];
        pairs[i].r = (rv[i] == NULL_I64) ? -1 : rv[i];
    }
    ray_release(res);
    qsort(pairs, (size_t)np, sizeof(qj_pair_t), qj_pair_cmp);
    *out = pairs;
    return np;
}

/* First-match map over sorted pairs: fmap[i] = first rrid for left row i or
 * -1.  Owned malloc'd array of nx entries (caller frees), NULL on OOM. */
static int64_t* qj_first_map(const qj_pair_t* pairs, int64_t np, int64_t nx) {
    int64_t* fmap = (int64_t*)malloc((size_t)(nx > 0 ? nx : 1) * sizeof(int64_t));
    if (!fmap) return NULL;
    for (int64_t i = 0; i < nx; i++) fmap[i] = -1;
    for (int64_t p = 0; p < np; p++)
        if (pairs[p].r >= 0 && pairs[p].l >= 0 && pairs[p].l < nx &&
            fmap[pairs[p].l] < 0)
            fmap[pairs[p].l] = pairs[p].r;
    return fmap;
}

/* TYPE-PRESERVING column fill (codex plan-review F2): result[i] = gy[i]
 * unless gy[i] is a q-null, else xc[i].  A pure gather over concat(xc, gy)
 * keeps the column type exact (no I64/F64 widening); the per-cell null test
 * reads boxed items. */
static ray_t* qj_fill_col(ray_t* xc, ray_t* gy, int64_t nx) {
    ray_t* cat = ray_concat_fn(xc, gy);
    if (!cat || RAY_IS_ERR(cat)) return cat ? cat : ray_error("type", NULL);
    int64_t* idx2 = (int64_t*)malloc((size_t)(nx > 0 ? nx : 1) * sizeof(int64_t));
    if (!idx2) { ray_release(cat); return ray_error("wsfull", "join: out of memory"); }
    for (int64_t i = 0; i < nx; i++) {
        ray_t* yi = qj_gen_item(gy, i);
        if (!yi || RAY_IS_ERR(yi)) { free(idx2); ray_release(cat); return yi ? yi : ray_error("type", NULL); }
        idx2[i] = qj_atom_is_qnull(yi) ? i : nx + i;
        ray_release(yi);
    }
    ray_t* r = qj_col_gather(cat, idx2, nx);
    free(idx2);
    ray_release(cat);
    return r;
}

/* Merge one shared value column (x's cell vs the matched y cell):
 *   mode 0 (lj/ij, q3.0): matched -> y cell, nulls included; miss -> x cell
 *   mode 1 (ljf/ijf/coalesce): matched && y cell non-null -> y cell, else x
 *   mode 2 (pj): x cell + (matched ? y cell : 0)
 * Mode 0 is a single gather over concat(xc,yc); mode 1 gathers then
 * type-preserving-fills; mode 2 zero-fills then adds through env `+`. */
static ray_t* qj_merge_col(ray_t* xc, ray_t* yc, const int64_t* fmap,
                           int64_t nx, int mode) {
    if (mode == 0) {
        ray_t* cat = ray_concat_fn(xc, yc);
        if (!cat || RAY_IS_ERR(cat)) return cat ? cat : ray_error("type", NULL);
        int64_t* idx2 = (int64_t*)malloc((size_t)(nx > 0 ? nx : 1) * sizeof(int64_t));
        if (!idx2) { ray_release(cat); return ray_error("wsfull", "join: out of memory"); }
        for (int64_t i = 0; i < nx; i++)
            idx2[i] = (fmap[i] >= 0) ? nx + fmap[i] : i;
        ray_t* r = qj_col_gather(cat, idx2, nx);
        free(idx2);
        ray_release(cat);
        return r;
    }
    ray_t* gy = qj_col_gather(yc, fmap, nx);       /* miss -> null cell */
    if (!gy || RAY_IS_ERR(gy)) return gy ? gy : ray_error("type", NULL);
    ray_t* r;
    if (mode == 1) {
        r = qj_fill_col(xc, gy, nx);               /* y nulls keep x, type-safe */
    } else {
        ray_t* zero = ray_i64(0);
        ray_t* zf = q_fill_wrap(zero, gy);         /* 0^gathered (pj is numeric) */
        ray_release(zero);
        if (!zf || RAY_IS_ERR(zf)) { ray_release(gy); return zf ? zf : ray_error("type", NULL); }
        /* env `+` is ATOMIC — the raw fn pointer wants eval's broadcast, so
         * apply (+; xc; zf) through ray_eval (vectors self-evaluate). */
        ray_t* plus = ray_env_get(ray_sym_intern("+", 1));  /* borrowed */
        if (!plus) { ray_release(zf); ray_release(gy); return ray_error("type", "pj: + builtin missing"); }
        ray_t* app = ray_list_new(3);
        if (RAY_IS_ERR(app)) { ray_release(zf); ray_release(gy); return app; }
        app = ray_list_append(app, plus);
        if (!RAY_IS_ERR(app)) app = ray_list_append(app, xc);
        if (!RAY_IS_ERR(app)) app = ray_list_append(app, zf);
        ray_release(zf);
        if (RAY_IS_ERR(app)) { ray_release(gy); return app; }
        r = ray_eval(app);
        ray_release(app);
    }
    ray_release(gy);
    return r;
}

/* keys of a keyed table's key-side as an owned RAY_LIST of sym atoms. */
static ray_t* qj_key_syms(ray_t* ykeys_tbl) {
    int64_t nk = ray_table_ncols(ykeys_tbl);
    ray_t* keys = ray_list_new(nk > 0 ? nk : 1);
    if (RAY_IS_ERR(keys)) return keys;
    for (int64_t i = 0; i < nk; i++) {
        ray_t* s = ray_sym(ray_table_col_name(ykeys_tbl, i));
        if (!s || RAY_IS_ERR(s)) { ray_release(keys); return s ? s : ray_error("type", NULL); }
        keys = ray_list_append(keys, s);
        ray_release(s);
        if (RAY_IS_ERR(keys)) return keys;
    }
    return keys;
}

/* True iff sym id is one of the keys RAY_LIST. */
static int qj_sym_in_list(ray_t* keys, int64_t sym_id) {
    int64_t n = ray_len(keys);
    ray_t** e = (ray_t**)ray_data(keys);
    for (int64_t i = 0; i < n; i++)
        if (e[i] && e[i]->type == -RAY_SYM && e[i]->i64 == sym_id) return 1;
    return 0;
}

/* normalize a column spec (sym atom / sym vector / list of sym atoms) to an
 * owned RAY_LIST of sym atoms; NULL on bad input. */
static ray_t* qj_norm_keys(ray_t* c) {
    if (!c) return NULL;
    if (c->type == -RAY_SYM) {
        ray_t* l = ray_list_new(1);
        if (RAY_IS_ERR(l)) return NULL;
        l = ray_list_append(l, c);
        return RAY_IS_ERR(l) ? NULL : l;
    }
    if (c->type == RAY_SYM || c->type == RAY_LIST) {
        int64_t n = ray_len(c);
        ray_t* l = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(l)) return NULL;
        for (int64_t i = 0; i < n; i++) {
            ray_t* e = qj_item(c, i);
            if (!e || RAY_IS_ERR(e) || e->type != -RAY_SYM) {
                if (e && !RAY_IS_ERR(e)) ray_release(e);
                else if (e) ray_release(e);
                ray_release(l);
                return NULL;
            }
            l = ray_list_append(l, e);
            ray_release(e);
            if (RAY_IS_ERR(l)) return NULL;
        }
        return l;
    }
    return NULL;
}

/* ---- lj / ljf / ij / ijf / pj ----------------------------------------------
 * ref/lj.md: keyed rhs lookup, one result row per x row; matched rows take
 * y's value columns wholesale (q3.0 — nulls included); ljf keeps x where the
 * y cell is null; unmatched rows keep x and null-fill new columns.  V4.0:
 * unkeyed rhs is 'type; rhs `()` returns x.  inner=1 keeps matched rows only
 * (ref/ij.md); mode 2 is pj (ref/pj.md — adds numeric columns, zero-fill). */
static ray_t* qj_lj_core(ray_t* x, ray_t* y, int mode, int inner) {
    if (y && y->type == RAY_LIST && ray_len(y) == 0) {     /* x lj () -> x */
        if (x) ray_retain(x);
        return x ? x : ray_error("type", "lj: nil lhs");
    }
    if (q_is_keyed_table(x)) {             /* keyed lhs: unkey, join, re-key */
        ray_t* xk = ray_dict_keys(x);                      /* borrowed */
        int64_t nkeys = ray_table_ncols(xk);
        ray_t* xp = q_table_flatten(x);
        if (!xp || RAY_IS_ERR(xp)) return xp;
        ray_t* r = qj_lj_core(xp, y, mode, inner);
        ray_release(xp);
        if (!r || RAY_IS_ERR(r)) return r;
        ray_t* kr = q_enkey(r, nkeys);
        ray_release(r);
        return kr;
    }
    if (!x || x->type != RAY_TABLE)
        return ray_error("type", "lj: left operand must be a table");
    if (!q_is_keyed_table(y))
        return ray_error("type", "lj: right operand must be a keyed table");
    ray_t* yk = ray_dict_keys(y);                          /* borrowed */
    ray_t* yv = ray_dict_vals(y);                          /* borrowed */
    ray_t* keys = qj_key_syms(yk);
    if (!keys || RAY_IS_ERR(keys)) return keys ? keys : ray_error("type", NULL);
    ray_t* yflat = q_table_flatten(y);
    if (!yflat || RAY_IS_ERR(yflat)) { ray_release(keys); return yflat; }
    qj_pair_t* pairs; ray_t* err;
    int64_t np = qj_join_pairs(x, yflat, keys, &pairs, &err);
    if (np < 0) { ray_release(keys); ray_release(yflat); return err; }
    int64_t nx = ray_table_nrows(x);
    int64_t* fmap = qj_first_map(pairs, np, nx);
    free(pairs);
    if (!fmap) { ray_release(keys); ray_release(yflat); return ray_error("wsfull", "lj: out of memory"); }
    int64_t nxc = ray_table_ncols(x);
    int64_t nyv = ray_table_ncols(yv);
    ray_t* out = ray_table_new(nxc + nyv);
    if (RAY_IS_ERR(out)) { free(fmap); ray_release(keys); ray_release(yflat); return out; }
    /* x columns; shared value columns merge from y */
    for (int64_t c = 0; c < nxc && !RAY_IS_ERR(out); c++) {
        int64_t nm = ray_table_col_name(x, c);
        ray_t* xc = ray_table_get_col_idx(x, c);           /* borrowed */
        ray_t* yc = qj_sym_in_list(keys, nm) ? NULL
                                             : ray_table_get_col(yv, nm);  /* borrowed */
        if (yc) {
            ray_t* m = qj_merge_col(xc, ray_table_get_col(yflat, nm), fmap, nx,
                                    mode);
            if (!m || RAY_IS_ERR(m)) { ray_release(out); out = m ? m : ray_error("type", NULL); break; }
            out = ray_table_add_col(out, nm, m);
            ray_release(m);
        } else {
            out = ray_table_add_col(out, nm, xc);
        }
    }
    /* y value columns not in x: gather by first-match (miss -> null; pj
     * zero-fills — ref/pj.md "new columns are zero") */
    for (int64_t c = 0; c < nyv && !RAY_IS_ERR(out); c++) {
        int64_t nm = ray_table_col_name(yv, c);
        if (ray_table_get_col(x, nm)) continue;            /* shared: merged above */
        ray_t* g = qj_col_gather(ray_table_get_col(yflat, nm), fmap, nx);
        if (!g || RAY_IS_ERR(g)) { ray_release(out); out = g ? g : ray_error("type", NULL); break; }
        if (mode == 2) {
            ray_t* zero = ray_i64(0);
            ray_t* zf = q_fill_wrap(zero, g);
            ray_release(zero);
            ray_release(g);
            if (!zf || RAY_IS_ERR(zf)) { ray_release(out); out = zf ? zf : ray_error("type", NULL); break; }
            g = zf;
        }
        out = ray_table_add_col(out, nm, g);
        ray_release(g);
    }
    /* inner join: keep only matched x rows (ref/ij.md — one combined record
     * for each row in x that matches) */
    if (inner && !RAY_IS_ERR(out)) {
        int64_t m = 0;
        for (int64_t i = 0; i < nx; i++) if (fmap[i] >= 0) m++;
        int64_t* idx = (int64_t*)malloc((size_t)(m > 0 ? m : 1) * sizeof(int64_t));
        if (!idx) { ray_release(out); out = ray_error("wsfull", "ij: out of memory"); }
        else {
            int64_t w = 0;
            for (int64_t i = 0; i < nx; i++) if (fmap[i] >= 0) idx[w++] = i;
            ray_t* g = qj_table_gather_idx(out, idx, m);
            free(idx);
            ray_release(out);
            out = g;
        }
    }
    free(fmap);
    ray_release(keys);
    ray_release(yflat);
    return out;
}

ray_t* q_lj_wrap(ray_t* x, ray_t* y)  { return qj_lj_core(x, y, 0, 0); }
ray_t* q_ljf_wrap(ray_t* x, ray_t* y) { return qj_lj_core(x, y, 1, 0); }
ray_t* q_ij_wrap(ray_t* x, ray_t* y)  { return qj_lj_core(x, y, 0, 1); }
ray_t* q_ijf_wrap(ray_t* x, ray_t* y) { return qj_lj_core(x, y, 1, 1); }
ray_t* q_pj_wrap(ray_t* x, ray_t* y)  { return qj_lj_core(x, y, 2, 0); }

/* ---- ej ---------------------------------------------------------------------
 * ref/ej.md: `ej[c;t1;t2]` — one combined record for EACH matching pair
 * (duplicates kept, t1-row major / t2 order within a key); duplicate column
 * values fill from t2.  Both tables plain. */
static ray_t* qj_ej_impl(ray_t* c, ray_t* t1, ray_t* t2) {
    if (!t1 || t1->type != RAY_TABLE || !t2 || t2->type != RAY_TABLE)
        return ray_error("type", "ej: both operands must be tables");
    ray_t* keys = qj_norm_keys(c);
    if (!keys) return ray_error("type", "ej: column spec must be symbol name(s)");
    qj_pair_t* pairs; ray_t* err;
    int64_t np = qj_join_pairs(t1, t2, keys, &pairs, &err);
    if (np < 0) { ray_release(keys); return err; }
    /* matched pairs only, already (l, r)-sorted */
    int64_t m = 0;
    for (int64_t p = 0; p < np; p++) if (pairs[p].r >= 0) m++;
    int64_t* li = (int64_t*)malloc((size_t)(m > 0 ? m : 1) * sizeof(int64_t));
    int64_t* ri = (int64_t*)malloc((size_t)(m > 0 ? m : 1) * sizeof(int64_t));
    if (!li || !ri) {
        free(li); free(ri); free(pairs); ray_release(keys);
        return ray_error("wsfull", "ej: out of memory");
    }
    int64_t w = 0;
    for (int64_t p = 0; p < np; p++)
        if (pairs[p].r >= 0) { li[w] = pairs[p].l; ri[w] = pairs[p].r; w++; }
    free(pairs);
    int64_t n1 = ray_table_ncols(t1), n2 = ray_table_ncols(t2);
    ray_t* out = ray_table_new(n1 + n2);
    if (RAY_IS_ERR(out)) { free(li); free(ri); ray_release(keys); return out; }
    for (int64_t col = 0; col < n1 && !RAY_IS_ERR(out); col++) {
        int64_t nm = ray_table_col_name(t1, col);
        /* duplicate value columns fill from t2 */
        ray_t* t2c = qj_sym_in_list(keys, nm) ? NULL : ray_table_get_col(t2, nm);
        ray_t* src = t2c ? t2c : ray_table_get_col_idx(t1, col);   /* borrowed */
        const int64_t* map = t2c ? ri : li;
        ray_t* g = qj_col_gather(src, map, m);
        if (!g || RAY_IS_ERR(g)) { ray_release(out); out = g ? g : ray_error("type", NULL); break; }
        out = ray_table_add_col(out, nm, g);
        ray_release(g);
    }
    for (int64_t col = 0; col < n2 && !RAY_IS_ERR(out); col++) {
        int64_t nm = ray_table_col_name(t2, col);
        if (ray_table_get_col(t1, nm)) continue;           /* shared: above */
        ray_t* g = qj_col_gather(ray_table_get_col_idx(t2, col), ri, m);
        if (!g || RAY_IS_ERR(g)) { ray_release(out); out = g ? g : ray_error("type", NULL); break; }
        out = ray_table_add_col(out, nm, g);
        ray_release(g);
    }
    free(li); free(ri);
    ray_release(keys);
    return out;
}

ray_t* q_ej_wrap(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("rank", "ej: expects [c;t1;t2], got %lld args", (long long)n);
    return qj_ej_impl(args[0], args[1], args[2]);
}

/* ---- uj / ujf + the keyed merge core ---------------------------------------
 * ref/uj.md: column union.  Unkeyed: x rows then y rows, absent columns
 * null-filled.  Keyed: y records UPDATE matching x records (q3.0 wholesale,
 * ujf = fill) and unmatched y records are appended.  ref/coalesce.md: keyed
 * `,` is the same merge with mode 0, `^` with mode 1 (nulls don't overwrite). */

/* column union of two plain tables: x rows stacked over y rows */
static ray_t* qj_uj_unkeyed(ray_t* x, ray_t* y) {
    int64_t nx = ray_table_nrows(x), ny = ray_table_nrows(y);
    int64_t nxc = ray_table_ncols(x), nyc = ray_table_ncols(y);
    int64_t* xi = (int64_t*)malloc((size_t)(nx + ny > 0 ? nx + ny : 1) * sizeof(int64_t));
    if (!xi) return ray_error("wsfull", "uj: out of memory");
    ray_t* out = ray_table_new(nxc + nyc);
    if (RAY_IS_ERR(out)) { free(xi); return out; }
    /* x-side columns (shared ones concat with y's; x-only null-pad below y) */
    for (int64_t c = 0; c < nxc && !RAY_IS_ERR(out); c++) {
        int64_t nm = ray_table_col_name(x, c);
        ray_t* xc = ray_table_get_col_idx(x, c);           /* borrowed */
        ray_t* yc = ray_table_get_col(y, nm);              /* borrowed or NULL */
        ray_t* col;
        if (yc) {
            col = ray_concat_fn(xc, yc);
        } else {
            for (int64_t i = 0; i < nx + ny; i++) xi[i] = (i < nx) ? i : -1;
            col = qj_col_gather(xc, xi, nx + ny);
        }
        if (!col || RAY_IS_ERR(col)) { ray_release(out); out = col ? col : ray_error("type", NULL); break; }
        out = ray_table_add_col(out, nm, col);
        ray_release(col);
    }
    /* y-only columns: nulls above, y values below */
    for (int64_t c = 0; c < nyc && !RAY_IS_ERR(out); c++) {
        int64_t nm = ray_table_col_name(y, c);
        if (ray_table_get_col(x, nm)) continue;
        for (int64_t i = 0; i < nx + ny; i++) xi[i] = (i < nx) ? -1 : i - nx;
        ray_t* col = qj_col_gather(ray_table_get_col_idx(y, c), xi, nx + ny);
        if (!col || RAY_IS_ERR(col)) { ray_release(out); out = col ? col : ray_error("type", NULL); break; }
        out = ray_table_add_col(out, nm, col);
        ray_release(col);
    }
    free(xi);
    return out;
}

/* keyed merge: x updated by y (mode 0 wholesale / 1 fill), unmatched y rows
 * appended.  THE single home for keyed uj / ujf / `,` / `^`. */
ray_t* qj_ktbl_merge(ray_t* x, ray_t* y, int mode) {
    ray_t* xk = ray_dict_keys(x);                          /* borrowed */
    ray_t* yk = ray_dict_keys(y);                          /* borrowed */
    if (!qj_same_schema(xk, yk))
        return ray_error("type", "uj: key columns must match");
    int64_t nkeys = ray_table_ncols(xk);
    ray_t* xf = q_table_flatten(x);
    if (!xf || RAY_IS_ERR(xf)) return xf;
    /* part 1: x rows merged/extended with y's columns (full lj shape) */
    ray_t* part1 = qj_lj_core(xf, y, mode, 0);
    if (!part1 || RAY_IS_ERR(part1)) { ray_release(xf); return part1 ? part1 : ray_error("type", NULL); }
    /* part 2: y rows with no x key match, aligned to part1's schema */
    ray_t* yf = q_table_flatten(y);
    if (!yf || RAY_IS_ERR(yf)) { ray_release(xf); ray_release(part1); return yf; }
    ray_t* keys = qj_key_syms(yk);
    if (!keys || RAY_IS_ERR(keys)) { ray_release(xf); ray_release(part1); ray_release(yf); return keys ? keys : ray_error("type", NULL); }
    qj_pair_t* pairs; ray_t* err;
    int64_t np = qj_join_pairs(yf, xf, keys, &pairs, &err);
    ray_release(keys);
    ray_release(xf);
    if (np < 0) { ray_release(part1); ray_release(yf); return err; }
    int64_t ny = ray_table_nrows(yf);
    int64_t* rmap = qj_first_map(pairs, np, ny);
    free(pairs);
    if (!rmap) { ray_release(part1); ray_release(yf); return ray_error("wsfull", "uj: out of memory"); }
    int64_t nu = 0;
    for (int64_t i = 0; i < ny; i++) if (rmap[i] < 0) nu++;
    ray_t* out = part1;
    if (nu > 0) {
        int64_t* uidx = (int64_t*)malloc((size_t)nu * sizeof(int64_t));
        if (!uidx) { free(rmap); ray_release(part1); ray_release(yf); return ray_error("wsfull", "uj: out of memory"); }
        int64_t w = 0;
        for (int64_t i = 0; i < ny; i++) if (rmap[i] < 0) uidx[w++] = i;
        int64_t npc = ray_table_ncols(part1);
        ray_t* part2 = ray_table_new(npc);
        for (int64_t c = 0; c < npc && !RAY_IS_ERR(part2); c++) {
            int64_t nm = ray_table_col_name(part1, c);
            ray_t* yc = ray_table_get_col(yf, nm);         /* borrowed or NULL */
            ray_t* col;
            if (yc) {
                col = qj_col_gather(yc, uidx, nu);
            } else {
                /* x-only column: nu nulls typed by part1's column */
                int64_t* nid = (int64_t*)malloc((size_t)nu * sizeof(int64_t));
                if (!nid) { col = ray_error("wsfull", "uj: out of memory"); }
                else {
                    for (int64_t i = 0; i < nu; i++) nid[i] = -1;
                    col = qj_col_gather(ray_table_get_col_idx(part1, c), nid, nu);
                    free(nid);
                }
            }
            if (!col || RAY_IS_ERR(col)) { ray_release(part2); part2 = col ? col : ray_error("type", NULL); break; }
            part2 = ray_table_add_col(part2, nm, col);
            ray_release(col);
        }
        free(uidx);
        if (!part2 || RAY_IS_ERR(part2)) { free(rmap); ray_release(part1); ray_release(yf); return part2 ? part2 : ray_error("type", NULL); }
        out = ray_concat_fn(part1, part2);
        ray_release(part1);
        ray_release(part2);
    }
    free(rmap);
    ray_release(yf);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("type", NULL);
    ray_t* kr = q_enkey(out, nkeys);
    ray_release(out);
    return kr;
}

static ray_t* qj_uj_core(ray_t* x, ray_t* y, int mode) {
    if (!x || !y) return ray_error("type", "uj: nil operand");
    if (q_is_keyed_table(x) && q_is_keyed_table(y))
        return qj_ktbl_merge(x, y, mode);
    if (x->type == RAY_TABLE && y->type == RAY_TABLE)
        return qj_uj_unkeyed(x, y);
    return ray_error("type", "uj: operands must be both keyed or both unkeyed tables");
}

ray_t* q_uj_wrap(ray_t* x, ray_t* y)  { return qj_uj_core(x, y, 0); }
ray_t* q_ujf_wrap(ray_t* x, ray_t* y) { return qj_uj_core(x, y, 1); }

/* ---- aj / aj0 / ajf / ajf0 + asof -------------------------------------------
 * The asof MATCHING is the engine's (ray_asof_join_fn: equality on
 * c0..cn-1, last value <= on cn, ties broken by last-in-row-order).  The
 * right side is reduced to its key columns plus a rowid, so the engine
 * returns a per-t1-row match INDEX into t2 (null=miss) and all column
 * assembly is the same gather/merge machinery as lj. */

/* per-t1-row match index into t2 (or -1), via engine asof-join.  Returns
 * owned malloc'd array of n1 entries or NULL with *err set. */
static int64_t* qj_aj_map(ray_t* keys, ray_t* t1, ray_t* t2, ray_t** err) {
    *err = NULL;
    int64_t rid = qj_tmp_colname("ajr", t1, t2);
    ray_t* rk = qj_keyid_table(t2, keys, rid);
    if (!rk || RAY_IS_ERR(rk)) { *err = rk ? rk : ray_error("type", NULL); return NULL; }
    ray_t* args[3] = { keys, t1, rk };
    ray_t* res = ray_asof_join_fn(args, 3);
    ray_release(rk);
    if (!res || RAY_IS_ERR(res)) { *err = res ? res : ray_error("type", NULL); return NULL; }
    ray_t* rc = ray_table_get_col(res, rid);               /* borrowed */
    int64_t n1 = ray_table_nrows(t1);
    if (!rc || rc->type != RAY_I64 || ray_len(rc) != n1) {
        ray_release(res);
        *err = ray_error("type", "aj: engine relation missing rowid column");
        return NULL;
    }
    int64_t* map = (int64_t*)malloc((size_t)(n1 > 0 ? n1 : 1) * sizeof(int64_t));
    if (!map) { ray_release(res); *err = ray_error("wsfull", "aj: out of memory"); return NULL; }
    const int64_t* rv = (const int64_t*)ray_data(rc);
    for (int64_t i = 0; i < n1; i++) map[i] = (rv[i] == NULL_I64) ? -1 : rv[i];
    ray_release(res);
    return map;
}

/* aj family core.  t0mode: result time column (the LAST key) is t2's actual
 * matched time (aj0/ajf0) instead of t1's boundary time.  fill: t2 nulls /
 * misses keep t1's value on SHARED columns (ajf/ajf0). */
static ray_t* qj_aj_core(ray_t* c, ray_t* t1, ray_t* t2, int t0mode, int fill) {
    if (!t1 || t1->type != RAY_TABLE || !t2 || t2->type != RAY_TABLE)
        return ray_error("type", "aj: both operands must be tables");
    ray_t* keys = qj_norm_keys(c);
    if (!keys || ray_len(keys) < 1) {
        if (keys) ray_release(keys);
        return ray_error("type", "aj: column spec must be symbol name(s)");
    }
    ray_t** ke = (ray_t**)ray_data(keys);
    int64_t time_sym = ke[ray_len(keys) - 1]->i64;
    ray_t* err;
    int64_t* map = qj_aj_map(keys, t1, t2, &err);
    if (!map) { ray_release(keys); return err; }
    int64_t n1 = ray_table_nrows(t1);
    int64_t nc1 = ray_table_ncols(t1), nc2 = ray_table_ncols(t2);
    ray_t* out = ray_table_new(nc1 + nc2);
    if (RAY_IS_ERR(out)) { free(map); ray_release(keys); return out; }
    for (int64_t col = 0; col < nc1 && !RAY_IS_ERR(out); col++) {
        int64_t nm = ray_table_col_name(t1, col);
        ray_t* xc = ray_table_get_col_idx(t1, col);        /* borrowed */
        ray_t* g = NULL;
        if (qj_sym_in_list(keys, nm)) {
            if (t0mode && nm == time_sym) {
                /* aj0: actual t2 time; ajf0 fills the misses from t1 */
                ray_t* tt = ray_table_get_col(t2, nm);     /* borrowed */
                ray_t* gt = tt ? qj_col_gather(tt, map, n1)
                               : ray_error("type", "aj: time column missing in t2");
                if (gt && !RAY_IS_ERR(gt) && fill) {
                    ray_t* f = qj_fill_col(xc, gt, n1);
                    ray_release(gt);
                    gt = f;
                }
                g = gt;
            } else {
                ray_retain(xc);
                g = xc;
            }
        } else {
            ray_t* yc = ray_table_get_col(t2, nm);         /* borrowed or NULL */
            if (yc) {
                /* shared value column: t2's matched value (miss -> null);
                 * ajf keeps t1 where t2 is null / missed */
                ray_t* gy = qj_col_gather(yc, map, n1);
                if (gy && !RAY_IS_ERR(gy) && fill) {
                    ray_t* f = qj_fill_col(xc, gy, n1);
                    ray_release(gy);
                    gy = f;
                }
                g = gy;
            } else {
                ray_retain(xc);
                g = xc;
            }
        }
        if (!g || RAY_IS_ERR(g)) { ray_release(out); out = g ? g : ray_error("type", NULL); break; }
        out = ray_table_add_col(out, nm, g);
        ray_release(g);
    }
    for (int64_t col = 0; col < nc2 && !RAY_IS_ERR(out); col++) {
        int64_t nm = ray_table_col_name(t2, col);
        if (qj_sym_in_list(keys, nm) || ray_table_get_col(t1, nm)) continue;
        ray_t* g = qj_col_gather(ray_table_get_col_idx(t2, col), map, n1);
        if (!g || RAY_IS_ERR(g)) { ray_release(out); out = g ? g : ray_error("type", NULL); break; }
        out = ray_table_add_col(out, nm, g);
        ray_release(g);
    }
    free(map);
    ray_release(keys);
    return out;
}

ray_t* q_aj_wrap(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("rank", "aj: expects [c;t1;t2], got %lld args", (long long)n);
    return qj_aj_core(args[0], args[1], args[2], 0, 0);
}
ray_t* q_aj0_wrap(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("rank", "aj0: expects [c;t1;t2], got %lld args", (long long)n);
    return qj_aj_core(args[0], args[1], args[2], 1, 0);
}
ray_t* q_ajf_wrap(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("rank", "ajf: expects [c;t1;t2], got %lld args", (long long)n);
    return qj_aj_core(args[0], args[1], args[2], 0, 1);
}
ray_t* q_ajf0_wrap(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("rank", "ajf0: expects [c;t1;t2], got %lld args", (long long)n);
    return qj_aj_core(args[0], args[1], args[2], 1, 1);
}

/* ---- asof -------------------------------------------------------------------
 * ref/asof.md: `t asof d` — d is a dict (one lookup -> dict result) or table
 * (per-row lookups -> table result) whose LAST key/column is the time; the
 * result carries t's remaining columns, a null row where nothing matches.
 * Direction: LEFT = d, RIGHT = t under the same engine asof matching. */
ray_t* q_asof_wrap(ray_t* t, ray_t* d) {
    if (!t || t->type != RAY_TABLE)
        return ray_error("type", "asof: left operand must be a table");
    if (d && d->type == RAY_DICT && !q_is_keyed_table(d)) {
        /* dict -> 1-row table, then unwrap the single result row to a dict */
        ray_t* dk = ray_dict_keys(d);                      /* borrowed */
        ray_t* dv = ray_dict_vals(d);                      /* borrowed */
        if (!dk || !dv || (dk->type != RAY_SYM && dk->type != RAY_LIST))
            return ray_error("type", "asof: dict keys must be symbols");
        int64_t nk = ray_len(dk);
        ray_t* dt = ray_table_new(nk > 0 ? nk : 1);
        if (RAY_IS_ERR(dt)) return dt;
        for (int64_t i = 0; i < nk; i++) {
            ray_t* ks = qj_item(dk, i);
            if (!ks || RAY_IS_ERR(ks) || ks->type != -RAY_SYM) {
                if (ks) ray_release(ks);
                ray_release(dt);
                return ray_error("type", "asof: dict keys must be symbols");
            }
            ray_t* val = qj_item(dv, i);
            if (!val || RAY_IS_ERR(val)) { ray_release(ks); ray_release(dt); return val ? val : ray_error("type", NULL); }
            ray_t* l = ray_list_new(1);
            if (RAY_IS_ERR(l)) { ray_release(val); ray_release(ks); ray_release(dt); return l; }
            l = ray_list_append(l, val);
            ray_release(val);
            if (RAY_IS_ERR(l)) { ray_release(ks); ray_release(dt); return l; }
            ray_t* colv = q_collapse_list(l);
            ray_release(l);
            if (!colv || RAY_IS_ERR(colv)) { ray_release(ks); ray_release(dt); return colv ? colv : ray_error("type", NULL); }
            dt = ray_table_add_col(dt, ks->i64, colv);
            ray_release(ks);
            ray_release(colv);
            if (RAY_IS_ERR(dt)) return dt;
        }
        ray_t* rows = q_asof_wrap(t, dt);
        ray_release(dt);
        if (!rows || RAY_IS_ERR(rows) || rows->type != RAY_TABLE) return rows;
        /* single row -> col!value dict */
        int64_t nc = ray_table_ncols(rows);
        ray_t* okeys = ray_sym_vec_new(RAY_SYM_W64, nc > 0 ? nc : 1);
        if (!okeys || RAY_IS_ERR(okeys)) { ray_release(rows); return okeys ? okeys : ray_error("oom", NULL); }
        ray_t* ovals = ray_list_new(nc > 0 ? nc : 1);
        if (RAY_IS_ERR(ovals)) { ray_release(okeys); ray_release(rows); return ovals; }
        for (int64_t ci = 0; ci < nc; ci++) {
            int64_t nm = ray_table_col_name(rows, ci);
            okeys = ray_vec_append(okeys, &nm);
            if (!okeys || RAY_IS_ERR(okeys)) { ray_release(ovals); ray_release(rows); return okeys ? okeys : ray_error("oom", NULL); }
            ray_t* col = ray_table_get_col_idx(rows, ci);  /* borrowed */
            ray_t* cell = qj_item(col, 0);
            if (!cell || RAY_IS_ERR(cell)) { ray_release(okeys); ray_release(ovals); ray_release(rows); return cell ? cell : ray_error("type", NULL); }
            ovals = ray_list_append(ovals, cell);
            ray_release(cell);
            if (RAY_IS_ERR(ovals)) { ray_release(okeys); ray_release(rows); return ovals; }
        }
        ray_release(rows);
        ray_t* cvals = q_collapse_list(ovals);
        ray_release(ovals);
        if (!cvals || RAY_IS_ERR(cvals)) { ray_release(okeys); return cvals ? cvals : ray_error("type", NULL); }
        /* construct through the audited env dict builder (the same one `!`
         * uses) so the result is structurally identical to a `!`-made dict */
        ray_t* r = q_env_call2("dict", okeys, cvals);
        ray_release(okeys);
        ray_release(cvals);
        return r;
    }
    if (!d || d->type != RAY_TABLE)
        return ray_error("type", "asof: right operand must be a dict or table");
    ray_t* keys = qj_key_syms(d);
    if (!keys || RAY_IS_ERR(keys)) return keys ? keys : ray_error("type", NULL);
    ray_t* err;
    int64_t* map = qj_aj_map(keys, d, t, &err);            /* LEFT = d, RIGHT = t */
    if (!map) { ray_release(keys); return err; }
    int64_t nd = ray_table_nrows(d);
    int64_t nct = ray_table_ncols(t);
    ray_t* out = ray_table_new(nct > 0 ? nct : 1);
    if (RAY_IS_ERR(out)) { free(map); ray_release(keys); return out; }
    for (int64_t col = 0; col < nct && !RAY_IS_ERR(out); col++) {
        int64_t nm = ray_table_col_name(t, col);
        if (qj_sym_in_list(keys, nm)) continue;            /* remaining cols only */
        ray_t* g = qj_col_gather(ray_table_get_col_idx(t, col), map, nd);
        if (!g || RAY_IS_ERR(g)) { ray_release(out); out = g ? g : ray_error("type", NULL); break; }
        out = ray_table_add_col(out, nm, g);
        ray_release(g);
    }
    free(map);
    ray_release(keys);
    return out;
}

/* ---- wj / wj1 ---------------------------------------------------------------
 * ref/wj.md: `wj[w;f;t;(q;(f0;c0);(f1;c1)...)]` — per-t-row time windows
 * w = (lo-vec;hi-vec), aggregating q's columns over each window (wj seeds
 * the window with the prevailing quote; wj1 is strict).  The engine special
 * form IS this join — (window-join [keys] intervals L R {aggdict}) — so the
 * wrapper only reshapes the q spec:
 *   - aggdict: keys = output col names (= source col names), vals =
 *     (opname-sym; col-sym) lists; op names recovered from the embedded fn
 *     VALUES via registry provenance.
 *   - the intervals arg reaches the special form UNevaluated-shaped: a bare
 *     RAY_LIST would be APPLIED by its arg-eval, so it is wrapped as a
 *     (list-constructor; lo; hi) application (the paren-literal head).
 * The `(::;`c)` raw-window form is 'nyi: the engine's bare-column arm emits
 * null placeholders — an error beats a wrong answer. */
static ray_t* qj_wj_core(ray_t** args, int64_t n, int mode) {
    const char* nm = mode ? "wj1" : "wj";
    if (n != 4) return ray_error("rank", "%s: expects [w;f;t;(q;aggs..)], got %lld args", nm, (long long)n);
    ray_t* w = args[0];
    ray_t* f = args[1];
    ray_t* t = args[2];
    ray_t* spec = args[3];
    if (!w || w->type != RAY_LIST || ray_len(w) != 2)
        return ray_error("type", "%s: w must be a (lo;hi) pair of time vectors", nm);
    if (!t || t->type != RAY_TABLE)
        return ray_error("type", "%s: t must be a table", nm);
    if (!spec || spec->type != RAY_LIST || ray_len(spec) < 2)
        return ray_error("type", "%s: last arg must be (q;(f;c)..)", nm);
    ray_t* keys = qj_norm_keys(f);
    if (!keys || ray_len(keys) < 2) {
        if (keys) ray_release(keys);
        return ray_error("type", "%s: f must name equality key(s) plus the time key", nm);
    }
    ray_t** se = (ray_t**)ray_data(spec);
    ray_t* qt = se[0];
    if (!qt || qt->type != RAY_TABLE) {
        ray_release(keys);
        return ray_error("type", "%s: aggregation source must be a table", nm);
    }
    /* keys as a plain SYM vector (engine reads cells by width helpers) */
    int64_t nk = ray_len(keys);
    ray_t** ke = (ray_t**)ray_data(keys);
    ray_t* kv = ray_sym_vec_new(RAY_SYM_W64, nk);
    if (!kv || RAY_IS_ERR(kv)) { ray_release(keys); return kv ? kv : ray_error("oom", NULL); }
    for (int64_t i = 0; i < nk; i++) {
        int64_t id = ke[i]->i64;
        kv = ray_vec_append(kv, &id);
        if (!kv || RAY_IS_ERR(kv)) { ray_release(keys); return kv ? kv : ray_error("oom", NULL); }
    }
    ray_release(keys);
    /* agg dict from the (fnvalue; `col) pairs */
    int64_t na = ray_len(spec) - 1;
    ray_t* dk = ray_sym_vec_new(RAY_SYM_W64, na > 0 ? na : 1);
    if (!dk || RAY_IS_ERR(dk)) { ray_release(kv); return dk ? dk : ray_error("oom", NULL); }
    ray_t* dv = ray_list_new(na > 0 ? na : 1);
    if (RAY_IS_ERR(dv)) { ray_release(dk); ray_release(kv); return dv; }
    for (int64_t a = 0; a < na; a++) {
        ray_t* pr = se[a + 1];
        ray_t** pe = (pr && pr->type == RAY_LIST && ray_len(pr) == 2)
                         ? (ray_t**)ray_data(pr) : NULL;
        if (!pe || !pe[1] || pe[1]->type != -RAY_SYM) {
            ray_release(dk); ray_release(dv); ray_release(kv);
            return ray_error("type", "%s: aggregations must be (fn;`col) pairs", nm);
        }
        q_provenance_t p;
        if (!pe[0] || !q_registry_provenance(pe[0], &p) || p.valence != Q_MONADIC) {
            ray_release(dk); ray_release(dv); ray_release(kv);
            return ray_error("nyi", "%s: aggregation must be a named builtin (raw `::` windows deferred)", nm);
        }
        int64_t col_id = pe[1]->i64;
        dk = ray_vec_append(dk, &col_id);
        if (!dk || RAY_IS_ERR(dk)) { ray_release(dv); ray_release(kv); return dk ? dk : ray_error("oom", NULL); }
        ray_t* op = ray_sym(ray_sym_intern_runtime(p.spelling, strlen(p.spelling)));
        if (!op || RAY_IS_ERR(op)) { ray_release(dk); ray_release(dv); ray_release(kv); return op ? op : ray_error("oom", NULL); }
        ray_t* cs = ray_sym(col_id);
        if (!cs || RAY_IS_ERR(cs)) { ray_release(op); ray_release(dk); ray_release(dv); ray_release(kv); return cs ? cs : ray_error("oom", NULL); }
        ray_t* pair = ray_list_new(2);
        if (RAY_IS_ERR(pair)) { ray_release(op); ray_release(cs); ray_release(dk); ray_release(dv); ray_release(kv); return pair; }
        pair = ray_list_append(pair, op);
        if (!RAY_IS_ERR(pair)) pair = ray_list_append(pair, cs);
        ray_release(op);
        ray_release(cs);
        if (RAY_IS_ERR(pair)) { ray_release(dk); ray_release(dv); ray_release(kv); return pair; }
        dv = ray_list_append(dv, pair);
        ray_release(pair);
        if (RAY_IS_ERR(dv)) { ray_release(dk); ray_release(kv); return dv; }
    }
    ray_t* aggd = ray_dict_new(dk, dv);                    /* consumes both */
    if (!aggd || RAY_IS_ERR(aggd)) { ray_release(kv); return aggd ? aggd : ray_error("type", NULL); }
    /* intervals: wrap (lo;hi) as a list-constructor application so the
     * special form's arg-eval rebuilds (not applies) it */
    ray_t** we = (ray_t**)ray_data(w);
    ray_t* lv = q_registry_list_value();                   /* borrowed */
    ray_t* iv = ray_list_new(3);
    if (RAY_IS_ERR(iv)) { ray_release(aggd); ray_release(kv); return iv; }
    iv = ray_list_append(iv, lv);
    if (!RAY_IS_ERR(iv)) iv = ray_list_append(iv, we[0]);
    if (!RAY_IS_ERR(iv)) iv = ray_list_append(iv, we[1]);
    if (RAY_IS_ERR(iv)) { ray_release(aggd); ray_release(kv); return iv; }
    ray_t* wargs[5] = { kv, iv, t, qt, aggd };
    ray_t* r = mode ? ray_window_join1_fn(wargs, 5)
                    : ray_window_join_fn(wargs, 5);
    ray_release(iv);
    ray_release(aggd);
    ray_release(kv);
    return r;
}

ray_t* q_wj_wrap(ray_t** args, int64_t n)  { return qj_wj_core(args, n, 0); }
ray_t* q_wj1_wrap(ray_t** args, int64_t n) { return qj_wj_core(args, n, 1); }

/* ---- keyed-table lookup by key-table (q_apply: `y[select a,b from x]`) -----
 * Contract: keytbl must contain ALL of kt's key columns by name ('type
 * otherwise); extra keytbl columns are ignored; result preserves keytbl row
 * order (first match per row); a miss yields a null row. */
ray_t* q_keyed_lookup_rows(ray_t* kt, ray_t* keytbl) {
    if (!q_is_keyed_table(kt) || !keytbl || keytbl->type != RAY_TABLE)
        return ray_error("type", "index: keyed-table lookup expects a key table");
    ray_t* kk = ray_dict_keys(kt);                         /* borrowed */
    ray_t* kv = ray_dict_vals(kt);                         /* borrowed */
    ray_t* keys = qj_key_syms(kk);
    if (!keys || RAY_IS_ERR(keys)) return keys ? keys : ray_error("type", NULL);
    /* all key columns must exist in keytbl (by name) */
    {
        int64_t nk = ray_len(keys);
        ray_t** ke = (ray_t**)ray_data(keys);
        for (int64_t i = 0; i < nk; i++)
            if (!ray_table_get_col(keytbl, ke[i]->i64)) {
                ray_release(keys);
                return ray_error("type", "index: key column missing from lookup table");
            }
    }
    ray_t* kflat = q_table_flatten(kt);
    if (!kflat || RAY_IS_ERR(kflat)) { ray_release(keys); return kflat; }
    qj_pair_t* pairs; ray_t* err;
    int64_t np = qj_join_pairs(keytbl, kflat, keys, &pairs, &err);
    ray_release(kflat);
    if (np < 0) { ray_release(keys); return err; }
    int64_t nx = ray_table_nrows(keytbl);
    int64_t* fmap = qj_first_map(pairs, np, nx);
    free(pairs);
    ray_release(keys);
    if (!fmap) return ray_error("wsfull", "index: out of memory");
    ray_t* out = qj_table_gather_idx(kv, fmap, nx);
    free(fmap);
    return out;
}
