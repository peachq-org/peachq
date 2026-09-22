/* ops/q_setops.c — q `distinct` `union` `inter` `except` `cross`.  `except`/`inter` are `x where [not] x in y`
 * (ref/except.md:30, ref/inter.md:18) and `union` is `distinct x,y`, so the set ops own no membership law: the type,
 * rank, null and index laws all arrive from Find.  Table arms compose the family's row primitives; dict/keyed
 * operands stay 'nyi — an error beats a wrong answer. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_count.h"
#include "qlang/q_registry_internal.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"
#include "qlang/ops/q_table.h"
#include "qlang/ops/q_index.h" /* q_index_at — the gather */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_value — `not` through the registry */
#include "lang/internal.h"   /* ray_group_fn */
#include "ops/idxop.h"       /* the key-set fronts: distinct/group read the attribute index */
#include "table/sym.h"       /* ray_read_sym */
#include <stdlib.h>
#include <string.h>

/* Indices of x-rows [not] present in y (whole-row membership). */
static ray_t* table_member_idx(ray_t* x, ray_t* y, int keep_present) {
    int64_t nrx = q_count(x), nry = q_count(y);
    int64_t ncx = ray_table_ncols(x);
    if (ncx != ray_table_ncols(y)) return q_err(QE_MISMATCH);
    ray_t* idx = ray_vec_new(RAY_I64, nrx > 0 ? nrx : 1);
    if (RAY_IS_ERR(idx)) return idx;
    idx->len = 0;
    for (int64_t r = 0; r < nrx; r++) {
        int found = 0;
        for (int64_t e = 0; e < nry && !found; e++)
            found = q_table_row_eq(x, r, y, e, ncx);
        if (found == keep_present) {
            idx = ray_vec_append(idx, &r);
            if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_OOM);
        }
    }
    return idx;
}

/* q `distinct t` — FIRST-OCCURRENCE row dedup.  The base DAG table-distinct
 * (ray_table_distinct_fn) sorts, so it is NOT reused — the same reason the q
 * vector distinct is a wrapper. */
static ray_t* table_distinct(ray_t* t) {
    int64_t nr = q_count(t);
    int64_t* gid = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    int64_t* rep = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    if (!gid || !rep) { free(gid); free(rep); return q_err(QE_WSFULL); }
    int64_t ng = q_table_row_groups(t, ray_table_ncols(t), gid, rep);
    ray_t* out = ng < 0 ? q_err(QE_WSFULL) : qj_table_gather_idx(t, rep, ng);
    free(gid); free(rep);
    return out;
}

/* `x where [not] x in y` for every non-table shape (ref/except.md:30, ref/inter.md:18).  `in` and `not` go through
 * the registry door so an enum operand meets the same arm the q expression does.  Borrowed in, owned out. */
static ray_t* setop_compose(ray_t* x, ray_t* y, int negate) {
    ray_t* av[2] = { x, y };
    ray_t* f = q_registry_lookup_name("in", 2, Q_DYADIC);  /* borrowed */
    ray_t* m = f ? q_eval_apply_value(f, av, 2) : NULL;
    if (!m || RAY_IS_ERR(m)) return m ? m : q_err(QE_TYPE);
    if (negate) {
        f = q_registry_lookup_name("not", 3, Q_MONADIC);
        ray_t* nm = f ? q_eval_apply_value(f, &m, 1) : NULL;
        ray_release(m);
        if (!nm || RAY_IS_ERR(nm)) return nm ? nm : q_err(QE_TYPE);
        m = nm;
    }
    ray_t* w = q_where_wrap(m);
    ray_release(m);
    if (!w || RAY_IS_ERR(w)) return w ? w : q_err(QE_TYPE);
    ray_t* r = q_typed_empty_like(q_index_at(x, &w, 1), x);
    ray_release(w);
    return r ? r : q_err(QE_TYPE);
}

/* q `x except y` (ref/except.md); a lone dict/table is a deferred cell — a wrapper builds no container. */
ray_t* q_except_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    if (q_type_is_table(x) && q_type_is_table(y)) {
        ray_t* idx = table_member_idx(x, y, 0);
        if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_OOM);
        ray_t* r = qj_table_gather_idx(x, (int64_t*)ray_data(idx), q_count(idx));
        ray_release(idx);
        return r;
    }
    if (q_type_is_dict(x) || q_type_is_dict(y) || q_type_is_table(x) || q_type_is_table(y))
        return q_err(QE_NYI);
    return setop_compose(x, y, 1);
}

/* q `distinct x` / monadic `?` — unique items in FIRST-OCCURRENCE order
 * (kdb), item equality being `~`: type-strict, nulls equal.
 * A TYPED VECTOR's items share one type, so `~` and the group kernel's atom
 * equality cannot disagree: its uniques are the KEYS OF ITS GROUPING, one hash
 * pass, where the scan below is O(n*distinct).  A LIST is not safe there —
 * group equates `1` with `1f`, `0n` with `0N`; `~` does not.  rayfall's own
 * ray_distinct_fn is no use either: it SORTS.  String operands are a deferred
 * cell (string model); atoms are kdb 'type.
 * An ATTRIBUTED vector reads its index (set-attribute.md:128): `u#` is a copy
 * without the marker (distinct.md:15 — the result carries no attribute), `g#`/`p#`
 * gather the key set; a null-bearing column keeps the scan, which owns nulls. */
ray_t* q_distinct_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == RAY_TABLE) return table_distinct(x);   /* row dedup */
    if (x->type == -RAY_STR)
        return q_err(QE_NYI);
    if (ray_is_vec(x)) {
        if (ray_index_has(x) && q_attr_letter(x) == 'u' && !(x->attrs & RAY_ATTR_HAS_NULLS) &&
            ray_index_payload(x->index)->built_for_len == q_count(x))
            return ray_attr_drop_fn(x);
        ray_t* ik = q_attr_index_keys(x, NULL);
        if (ik) return ik;
        ray_t* g = ray_group_fn(x);
        if (!g || RAY_IS_ERR(g)) return g ? g : q_err(QE_TYPE);
        ray_t* k = ray_dict_keys(g);
        ray_retain(k);
        ray_release(g);
        return k;
    }
    if (x->type != RAY_LIST)
        return q_err(QE_TYPE);
    int64_t n = q_count(x);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = q_join_item(x, i);
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
        int dup = 0;
        int64_t m = q_count(out);
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

/* q `x union y` — literally `distinct x,y` (ref/union.md), composed from THE two
 * q wrappers so it owns no set logic: join carries the mixed-list law
 * (ref/join.md:33), distinct the first-occurrence order rayfall's ray_union_fn
 * lacks (it keeps x-duplicates).  Whatever either wrapper defers, union defers. */
ray_t* q_union_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    /* keyed tables / dicts are deferred cells (mirror of the inter guard). */
    if (x->type == RAY_DICT || y->type == RAY_DICT)
        return q_err(QE_NYI);
    ray_t* j = q_join_wrap(x, y);
    if (!j || RAY_IS_ERR(j)) return j;
    ray_t* r = q_distinct_wrap(j);
    ray_release(j);
    return r;
}

/* q `x inter y` (ref/inter.md); two dicts answer their common VALUES as a list, so the dict arm recurses on them. */
ray_t* q_inter_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    if (q_type_is_table(x) && q_type_is_table(y)) {
        ray_t* idx = table_member_idx(x, y, 1);
        if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_OOM);
        ray_t* r = qj_table_gather_idx(x, (int64_t*)ray_data(idx), q_count(idx));
        ray_release(idx);
        return r;
    }
    if (q_type_is_plain_dict(x) && q_type_is_plain_dict(y))
        return q_inter_wrap(ray_dict_vals(x), ray_dict_vals(y));
    if (q_type_is_dict(x) || q_type_is_dict(y) || q_type_is_table(x) || q_type_is_table(y))
        return q_err(QE_NYI);
    return setop_compose(x, y, 0);
}

/* q `x cross y` — Cartesian product, `{raze x,/:\:y}` (ref/cross.md): for
 * each item a of x (in order), for each item b of y, the JOIN `a,b`.
 * Composes existing primitives (ray_at_fn item access + q join == rayfall
 * concat) — rayfall has no cartesian primitive.  Atom operands behave as
 * one-item lists (each-left/right over an atom).  Deferred cells ('nyi,
 * never a wrong answer): string operands (kdb iterates a string's CHARS;
 * peachq strings are -RAY_STR atoms — string model) and dict/table cross
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
        int64_t nxr = q_count(x), nyr = q_count(y);
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
    /* strings iterate their CHARS; atoms act as 1-item lists (q_join_gen_*) */
    int64_t nx = q_join_gen_len(x), ny = q_join_gen_len(y);
    if (nx < 0 || ny < 0)
        return q_err(QE_TYPE);
    ray_t* out = ray_list_new(nx * ny > 0 ? nx * ny : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < nx; i++) {
        ray_t* a = q_join_gen_item(x, i);
        if (!a || RAY_IS_ERR(a)) { ray_release(out); return a ? a : q_err(QE_TYPE); }
        for (int64_t j = 0; j < ny; j++) {
            ray_t* b = q_join_gen_item(y, j);
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
