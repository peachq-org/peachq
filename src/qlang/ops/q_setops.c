/* ops/q_setops.c — q `distinct` `union` `inter` `except` `cross`.  Each is a
 * wrapper rather than a rayfall rename because kdb's law differs at the
 * dedup: first-occurrence order, x-duplicates kept, whole-ITEM (not
 * flattened) membership.  Table arms compose the family's row primitives;
 * dict/keyed operands the base kernels would mangle stay 'nyi — an error
 * beats a wrong answer. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"
#include "qlang/ops/q_table.h"
#include "lang/eval.h"       /* ray_except_fn, ray_sect_fn */
#include "lang/internal.h"   /* ray_concat_fn */
#include <stdlib.h>

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
    int64_t nr = ray_table_nrows(t);
    int64_t* gid = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    int64_t* rep = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    if (!gid || !rep) { free(gid); free(rep); return q_err(QE_WSFULL); }
    int64_t ng = q_table_row_groups(t, ray_table_ncols(t), gid, rep);
    ray_t* out = ng < 0 ? q_err(QE_WSFULL) : qj_table_gather_idx(t, rep, ng);
    free(gid); free(rep);
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
        ray_t* r = qj_table_gather_idx(x, (int64_t*)ray_data(idx), ray_len(idx));
        ray_release(idx);
        return r;
    }
    return ray_except_fn(x, y);
}

/* q `distinct x` / monadic `?` — unique items in FIRST-OCCURRENCE order
 * (kdb), item equality being `~`: type-strict, nulls equal.
 * A TYPED VECTOR's items share one type, so `~` and the group kernel's atom
 * equality cannot disagree: its uniques are the KEYS OF ITS GROUPING, one hash
 * pass, where the scan below is O(n*distinct).  A LIST is not safe there —
 * group equates `1` with `1f`, `0n` with `0N`; `~` does not.  rayfall's own
 * ray_distinct_fn is no use either: it SORTS.  String operands are a deferred
 * cell (string model); atoms are kdb 'type. */
ray_t* q_distinct_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == RAY_TABLE) return table_distinct(x);   /* row dedup */
    if (x->type == -RAY_STR)
        return q_err(QE_NYI);
    if (ray_is_vec(x)) {
        ray_t* g = ray_group_fn(x);
        if (!g || RAY_IS_ERR(g)) return g ? g : q_err(QE_TYPE);
        ray_t* k = ray_dict_keys(g);
        ray_retain(k);
        ray_release(g);
        return k;
    }
    if (x->type != RAY_LIST)
        return q_err(QE_TYPE);
    int64_t n = ray_len(x);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = q_join_item(x, i);
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
        ray_t* r = qj_table_gather_idx(x, (int64_t*)ray_data(idx), ray_len(idx));
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
            ray_t* xi = q_join_item(x, i);
            if (!xi || RAY_IS_ERR(xi)) { ray_release(out); return xi ? xi : q_err(QE_TYPE); }
            int found = 0;
            for (int64_t j = 0; j < ny && !found; j++) {
                ray_t* yj = q_join_item(y, j);
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
