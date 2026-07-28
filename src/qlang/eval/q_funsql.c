/* q_funsql — functional qSQL (basics/funsql.md, basics/qsql.md).  The
 * implementation IS the equivalence laws of the 2026-07-28 wave plan: phrases
 * run through q_eval under one NON-barrier column scope; Where is successive
 * refinement of an index vector; sel is `flip names!phrases`; limit composes
 * on take/drop; by rides the group core; update is dict update over Amend At;
 * delete is drop / complement select.  Order: From-Where-By-Select-Limit. */
#define _POSIX_C_SOURCE 200809L
#define Q_OPS_ENV_GRANDFATHER /* legitimate owner: From-resolve + phrase scopes are eval-side name resolution */

#include "qlang/eval/q_funsql.h"
#include "qlang/eval/q_eval.h"
#include "qlang/q_err.h"
#include "qlang/q_builtins.h"          /* q_count_long */
#include "qlang/q_registry_internal.h" /* q_take_wrap, q_where_wrap, q_flip_wrap, ... */
#include "qlang/ops/q_bang.h"
#include "qlang/ops/q_index.h"
#include "lang/env.h"
#include "lang/internal.h"             /* ray_til_fn, ray_typed_null */
#include <string.h>


static int is_empty_gen(ray_t* v) {
    return v && ((v->type == RAY_LIST && ray_len(v) == 0) || RAY_IS_NULL(v));
}

static int is_bool_atom(ray_t* v, int truth) {
    return v && v->type == -RAY_BOOL && (v->b8 != 0) == truth;
}

static ray_t* til_count(ray_t* t) {
    ray_t* n = ray_i64(q_count_long(t));
    ray_t* r = ray_til_fn(n);
    ray_release(n);
    return r ? r : q_err(QE_TYPE);
}

/* one gather home for everything: x@idx via value-apply (law 7) */
static ray_t* gather(ray_t* x, ray_t* idx) {
    return q_eval_apply_value(x, &idx, 1);
}

/* From-resolve (law 24 + column-dict superset): sym -> env; keyed -> 0!; dict -> flip */
static ray_t* ques_from(ray_t* t) {
    if (!t) return q_err(QE_TYPE);
    if (t->type == -RAY_SYM) {
        ray_t* v = ray_env_resolve(t->i64);
        if (!v) return q_err(QE_NAME);
        if (RAY_IS_ERR(v)) return v;
        ray_t* r = ques_from(v);
        ray_release(v);
        return r;
    }
    if (q_type_is_keyed(t)) return q_bang_enkey(0, t);
    if (q_type_is_dict(t)) return q_flip_wrap(t);
    if (q_type_is_table(t)) { ray_retain(t); return t; }
    return q_err(QE_TYPE);
}

/* Law 4: THE evaluator runs the phrase inside one NON-barrier scope (qsql.md
 * name order: column -> enclosing locals -> global); virtual `i` = idx. */
static ray_t* phrase_eval(ray_t* tree, ray_t* t, ray_t* idx) {
    if (ray_env_push_scope() != RAY_OK) return q_err(QE_STACK);
    ray_t* err = NULL;
    int64_t nc = ray_table_ncols(t);
    for (int64_t c = 0; c < nc && !err; c++) {
        ray_t* g = gather(ray_table_get_col_idx(t, c), idx);
        if (!g || RAY_IS_ERR(g)) { err = g ? g : q_err(QE_TYPE); break; }
        ray_env_set_local(ray_table_col_name(t, c), g);        /* retains */
        ray_release(g);
    }
    if (!err) ray_env_set_local(ray_sym_intern_runtime("i", 1), idx);
    ray_t* r = err ? err : q_eval_apply_concrete(q_eval(tree));
    ray_env_pop_scope();
    return r;
}

/* Law 6: SUCCESSIVE refinement — each constraint sees only the rows the
 * previous ones kept; idx:=idx[where result]. */
static ray_t* where_fold(ray_t* c, ray_t* t, ray_t* idx0) {
    ray_retain(idx0);
    if (!c || is_empty_gen(c) || (ray_is_vec(c) && ray_len(c) == 0))
        return idx0;
    if (c->type != RAY_LIST) { ray_release(idx0); return q_err(QE_TYPE); }
    ray_t* idx = idx0;
    int64_t n = ray_len(c);
    for (int64_t j = 0; j < n; j++) {
        ray_t* tree = q_index_elem_at(c, j);
        ray_t* r = (tree && !RAY_IS_ERR(tree)) ? phrase_eval(tree, t, idx) : tree;
        if (tree && r != tree) ray_release(tree);
        ray_t* w = (r && !RAY_IS_ERR(r)) ? q_where_wrap(r) : r;
        if (r && w != r) ray_release(r);
        ray_t* nidx = (w && !RAY_IS_ERR(w)) ? gather(idx, w) : w;
        if (w && nidx != w) ray_release(w);
        ray_release(idx);
        if (!nidx || RAY_IS_ERR(nidx)) return nidx ? nidx : q_err(QE_TYPE);
        idx = nidx;
    }
    return idx;
}

/* an ATOM phrase result conforms to the row count: n#atom (law 8) */
static ray_t* conform_col(ray_t* v, int64_t n) {
    if (!v || RAY_IS_ERR(v) || !ray_is_atom(v)) return v;
    ray_t* na = ray_i64(n);
    ray_t* c = q_take_wrap(na, v);
    ray_release(na);
    ray_release(v);
    return c;
}

/* every phrase of rng -> owned LIST of columns; atoms conform unless exec */
static ray_t* sel_cols(ray_t* rng, ray_t* t, ray_t* idx, int conform) {
    int64_t nc = q_count_long(rng);
    int64_t n = q_count_long(idx);
    ray_t* vals = ray_list_new(nc > 0 ? nc : 1);
    for (int64_t j = 0; j < nc && !RAY_IS_ERR(vals); j++) {
        ray_t* tree = q_index_elem_at(rng, j);
        ray_t* v = (tree && !RAY_IS_ERR(tree)) ? phrase_eval(tree, t, idx) : tree;
        if (tree && v != tree) ray_release(tree);
        if (conform) v = conform_col(v, n);
        if (!v || RAY_IS_ERR(v)) { ray_release(vals); return v ? v : q_err(QE_TYPE); }
        vals = ray_list_append(vals, v);
        ray_release(v);
    }
    return vals;
}

/* Law 8: a phrase-columns result table IS `flip names!cols`. */
static ray_t* cols_table(ray_t* names, ray_t* rng, ray_t* t, ray_t* idx) {
    ray_t* vals = sel_cols(rng, t, idx, 1);
    if (RAY_IS_ERR(vals)) return vals;
    ray_t* d = q_bang(names, vals);
    ray_release(vals);
    if (!d || RAY_IS_ERR(d)) return d ? d : q_err(QE_TYPE);
    ray_t* r = q_flip_wrap(d);
    ray_release(d);
    return r;
}

static ray_t* sel_table(ray_t* a, ray_t* t, ray_t* idx) {
    return cols_table(ray_dict_keys(a), ray_dict_vals(a), t, idx);
}

/* Exec at b=() (law 11): () -> last row; sym/tree -> value; dict unconformed */
static ray_t* exec_shape(ray_t* a, ray_t* t, ray_t* idx) {
    if (is_empty_gen(a)) {
        ray_t* rows = gather(t, idx);
        if (!rows || RAY_IS_ERR(rows)) return rows ? rows : q_err(QE_TYPE);
        int64_t n = q_count_long(rows);
        ray_t* r = q_index_elem_at(rows, n > 0 ? n - 1 : 0);
        ray_release(rows);
        return r;
    }
    if (q_type_is_dict(a)) {
        ray_t* vals = sel_cols(ray_dict_vals(a), t, idx, 0);
        if (RAY_IS_ERR(vals)) return vals;
        ray_t* c = q_list_collapse(vals);
        ray_release(vals);
        if (!c || RAY_IS_ERR(c)) return c ? c : q_err(QE_TYPE);
        ray_t* d = q_bang(ray_dict_keys(a), c);
        ray_release(c);
        return d;
    }
    return phrase_eval(a, t, idx);
}

/* Rank 5 (law 9): n#r, pair (i;j) is j#i _ r; "up to n" clamps (ref/select.md) */
static ray_t* limit_apply(ray_t* nspec, ray_t* r) {
    if (!r || RAY_IS_ERR(r)) return r;
    int64_t cnt = q_count_long(r);
    if (q_type_is_int_atom(nspec) && !RAY_ATOM_IS_NULL(nspec)) {
        int64_t n = q_type_iatom_val(nspec);
        if (n > cnt) n = cnt;
        if (n < -cnt) n = -cnt;
        ray_t* na = ray_i64(n);
        ray_t* out = q_take_wrap(na, r);
        ray_release(na);
        ray_release(r);
        return out;
    }
    if (q_type_is_int_vec(nspec) && ray_len(nspec) == 2) {
        ray_t* ia = q_index_elem_at(nspec, 0);
        ray_t* ja = q_index_elem_at(nspec, 1);
        int64_t i = q_type_iatom_val(ia), j = q_type_iatom_val(ja);
        ray_release(ia);
        ray_release(ja);
        if (i < 0 || j < 0) { ray_release(r); return q_err(QE_DOMAIN); }
        ray_t* na = ray_i64(i);
        ray_t* d = q_drop_wrap(na, r);
        ray_release(na);
        ray_release(r);
        if (!d || RAY_IS_ERR(d)) return d ? d : q_err(QE_TYPE);
        int64_t left = q_count_long(d);
        if (j > left) j = left;
        na = ray_i64(j);
        ray_t* out = q_take_wrap(na, d);
        ray_release(na);
        ray_release(d);
        return out;
    }
    ray_release(r);
    return q_err(QE_TYPE);
}


/* a value table from names + ready column list (no conform, no re-eval) */
static ray_t* named_table(ray_t* names, ray_t* cols) {
    ray_t* d = q_bang(names, cols);
    if (!d || RAY_IS_ERR(d)) return d ? d : q_err(QE_TYPE);
    ray_t* r = q_flip_wrap(d);
    ray_release(d);
    return r;
}

/* ===== by (wave 3): laws 14-16 on the wave-1 group core ================== */

/* b -> (names, trees): dict keys!range; a sym atom/vector IS both — the
 * name and the column phrase.  Owned result + owned *trees_out. */
static ray_t* by_specs(ray_t* b, ray_t** trees_out) {
    *trees_out = NULL;
    if (q_type_is_dict(b)) {
        ray_t* k = ray_dict_keys(b);
        ray_t* v = ray_dict_vals(b);
        ray_retain(k);
        ray_retain(v);
        *trees_out = v;
        return k;
    }
    ray_t* sv = (b->type == -RAY_SYM) ? q_enlist_wrap(&b, 1)
                                      : (ray_retain(b), b);
    if (!sv || RAY_IS_ERR(sv)) return sv;
    ray_retain(sv);
    *trees_out = sv;
    return sv;
}

/* per group: the ORIGINAL row numbers, idx@positions (law 14) */
static ray_t* by_orig_idx(ray_t* idx, ray_t* gv) {
    int64_t ng = ray_len(gv);
    ray_t* out = ray_list_new(ng > 0 ? ng : 1);
    for (int64_t j = 0; j < ng && !RAY_IS_ERR(out); j++) {
        ray_t* pos = q_index_elem_at(gv, j);
        ray_t* gi = (pos && !RAY_IS_ERR(pos)) ? gather(idx, pos) : pos;
        if (pos && gi != pos) ray_release(pos);
        if (!gi || RAY_IS_ERR(gi)) { ray_release(out); return gi ? gi : q_err(QE_TYPE); }
        out = ray_list_append(out, gi);
        ray_release(gi);
    }
    return out;
}

/* one a-phrase per group over its original rows; collapse (aggregates ->
 * vector, uniforms -> nested column) */
static ray_t* by_col(ray_t* tree, ray_t* t, ray_t* gidxs) {
    int64_t ng = ray_len(gidxs);
    ray_t** gi = (ray_t**)ray_data(gidxs);
    ray_t* out = ray_list_new(ng > 0 ? ng : 1);
    for (int64_t j = 0; j < ng && !RAY_IS_ERR(out); j++) {
        ray_t* v = phrase_eval(tree, t, gi[j]);
        if (!v || RAY_IS_ERR(v)) { ray_release(out); return v ? v : q_err(QE_TYPE); }
        out = ray_list_append(out, v);
        ray_release(v);
    }
    if (RAY_IS_ERR(out)) return out;
    ray_t* c = q_list_collapse(out);
    ray_release(out);
    return c;
}

/* every a-phrase through by_col -> owned LIST of value columns */
static ray_t* by_cols(ray_t* rng, ray_t* t, ray_t* gidxs) {
    int64_t nc = q_count_long(rng);
    ray_t* vals = ray_list_new(nc > 0 ? nc : 1);
    for (int64_t j = 0; j < nc && !RAY_IS_ERR(vals); j++) {
        ray_t* tree = q_index_elem_at(rng, j);
        ray_t* v = (tree && !RAY_IS_ERR(tree)) ? by_col(tree, t, gidxs) : tree;
        if (tree && v != tree) ray_release(tree);
        if (!v || RAY_IS_ERR(v)) { ray_release(vals); return v ? v : q_err(QE_TYPE); }
        vals = ray_list_append(vals, v);
        ray_release(v);
    }
    return vals;
}

/* per group LAST original row (select.md: a By with no Select phrase) */
static ray_t* by_last_idx(ray_t* gidxs) {
    int64_t ng = ray_len(gidxs);
    ray_t** gi = (ray_t**)ray_data(gidxs);
    ray_t* out = ray_vec_new(RAY_I64, ng > 0 ? ng : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = 0;
    for (int64_t j = 0; j < ng; j++) {
        int64_t n = q_count_long(gi[j]);
        ray_t* e = n > 0 ? q_index_elem_at(gi[j], n - 1) : NULL;
        int64_t v = (e && !RAY_IS_ERR(e)) ? q_type_iatom_val(e) : 0;
        if (e && !RAY_IS_ERR(e)) ray_release(e);
        else if (e) { ray_release(out); return e; }
        out = ray_vec_append(out, &v);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    }
    return out;
}

/* grouped state shared by every by-consumer: g = group(by-table over idx)
 * (laws 2+14), gidxs = original rows per group.  Owned out-params. */
static ray_t* by_group(ray_t* names, ray_t* trees, ray_t* t, ray_t* idx,
                       ray_t** gidxs_out) {
    *gidxs_out = NULL;
    ray_t* bt = cols_table(names, trees, t, idx);
    if (!bt || RAY_IS_ERR(bt)) return bt ? bt : q_err(QE_TYPE);
    ray_t* g = q_group_wrap(bt);
    ray_release(bt);
    if (!g || RAY_IS_ERR(g)) return g ? g : q_err(QE_TYPE);
    ray_t* gidxs = by_orig_idx(idx, ray_dict_vals(g));
    if (RAY_IS_ERR(gidxs)) { ray_release(g); return gidxs; }
    *gidxs_out = gidxs;
    return g;
}

/* Select-by (b a dict — the keyword By lowering): result keyed ascending by
 * its key (law 15), built by the `!` table!table law; a=() takes the last
 * row per group minus the by-named columns (select.md); a sym gives the
 * dict shape (funsql.md "Group by a dictionary"). */
static ray_t* by_select(ray_t* names, ray_t* trees, ray_t* a, ray_t* t,
                        ray_t* idx) {
    ray_t* gidxs = NULL;
    ray_t* g = by_group(names, trees, t, idx, &gidxs);
    if (RAY_IS_ERR(g)) return g;
    ray_t* kt = ray_dict_keys(g);                       /* borrowed */
    ray_t* ord = q_iasc_wrap(kt);                       /* law 15 */
    ray_t* kt2 = (ord && !RAY_IS_ERR(ord)) ? gather(kt, ord) : ord;
    if (ord && kt2 != ord && !RAY_IS_ERR(ord)) { /* keep ord */ } else ord = NULL;
    ray_t* r = NULL;
    if (!kt2 || RAY_IS_ERR(kt2)) {
        r = kt2 ? kt2 : q_err(QE_TYPE);
        kt2 = NULL;
    } else if (is_empty_gen(a)) {                       /* last row per group */
        ray_t* li = by_last_idx(gidxs);
        ray_t* li2 = RAY_IS_ERR(li) ? li : gather(li, ord);
        if (!RAY_IS_ERR(li) && li2 != li) ray_release(li);
        ray_t* rows = (li2 && !RAY_IS_ERR(li2)) ? gather(t, li2) : li2;
        if (li2 && !RAY_IS_ERR(li2) && rows != li2) ray_release(li2);
        if (!rows || RAY_IS_ERR(rows)) r = rows ? rows : q_err(QE_TYPE);
        else {
            ray_t* tcols = q_cols_fn(rows);
            ray_t* keep = (tcols && !RAY_IS_ERR(tcols))
                              ? q_except_wrap(tcols, names) : tcols;
            if (tcols && !RAY_IS_ERR(tcols) && keep != tcols) ray_release(tcols);
            ray_t* vt = (keep && !RAY_IS_ERR(keep)) ? q_take_wrap(keep, rows)
                                                    : keep;
            if (keep && !RAY_IS_ERR(keep) && vt != keep) ray_release(keep);
            ray_release(rows);
            if (!vt || RAY_IS_ERR(vt)) r = vt ? vt : q_err(QE_TYPE);
            else r = q_bang(kt2, vt), ray_release(vt);
        }
    } else if (q_type_is_dict(a)) {
        ray_t* vcols = by_cols(ray_dict_vals(a), t, gidxs);
        ray_t* v2;                        /* law-15 order: EACH column @ ord */
        if (RAY_IS_ERR(vcols)) v2 = vcols;
        else {
            int64_t nc = ray_len(vcols);
            v2 = ray_list_new(nc > 0 ? nc : 1);
            for (int64_t j = 0; j < nc && !RAY_IS_ERR(v2); j++) {
                ray_t* cv = q_index_elem_at(vcols, j);
                ray_t* cr = (cv && !RAY_IS_ERR(cv)) ? gather(cv, ord) : cv;
                if (cv && cr != cv) ray_release(cv);
                if (!cr || RAY_IS_ERR(cr)) { ray_release(v2); v2 = cr ? cr : q_err(QE_TYPE); break; }
                v2 = ray_list_append(v2, cr);
                ray_release(cr);
            }
            ray_release(vcols);
        }
        ray_t* vt = (v2 && !RAY_IS_ERR(v2)) ? named_table(ray_dict_keys(a), v2)
                                            : v2;
        if (v2 && !RAY_IS_ERR(v2) && vt != v2) ray_release(v2);
        if (!vt || RAY_IS_ERR(vt)) r = vt ? vt : q_err(QE_TYPE);
        else r = q_bang(kt2, vt), ray_release(vt);
    } else {                                            /* a sym/tree -> dict */
        ray_t* vl = by_col(a, t, gidxs);
        ray_t* v2 = RAY_IS_ERR(vl) ? vl : gather(vl, ord);
        if (!RAY_IS_ERR(vl) && v2 != vl) ray_release(vl);
        if (!v2 || RAY_IS_ERR(v2)) r = v2 ? v2 : q_err(QE_TYPE);
        else r = q_bang(kt2, v2), ray_release(v2);
    }
    if (kt2) ray_release(kt2);
    if (ord) ray_release(ord);
    ray_release(gidxs);
    ray_release(g);
    return r;
}

/* Exec-by, b a sym ATOM: plain dict keyed by the column's distinct values in
 * FIRST-OCCURRENCE order (law 16, the group law; ordering doc-undecidable —
 * gaps register item 4); a=dict keys by a one-ANONYMOUS-column table
 * (funsql.md "Group by column"). */
static ray_t* by_exec_atom(ray_t* names, ray_t* trees, ray_t* a, ray_t* t,
                           ray_t* idx) {
    if (is_empty_gen(a)) return q_err(QE_NYI);          /* matrix "-" */
    ray_t* gidxs = NULL;
    ray_t* g = by_group(names, trees, t, idx, &gidxs);
    if (RAY_IS_ERR(g)) return g;
    ray_t* kt = ray_dict_keys(g);                       /* 1-col distinct */
    ray_t* r;
    if (q_type_is_dict(a)) {
        ray_t* vcols = by_cols(ray_dict_vals(a), t, gidxs);
        ray_t* vt = RAY_IS_ERR(vcols) ? vcols
                    : named_table(ray_dict_keys(a), vcols);
        if (!RAY_IS_ERR(vcols) && vt != vcols) ray_release(vcols);
        if (!vt || RAY_IS_ERR(vt)) r = vt ? vt : q_err(QE_TYPE);
        else {
            ray_t* akt = ray_table_new(1);
            akt = ray_table_add_col(akt, ray_sym_intern_runtime("", 0),
                                    ray_table_get_col_idx(kt, 0));
            if (RAY_IS_ERR(akt)) r = akt;
            else r = q_bang(akt, vt), ray_release(akt);
            ray_release(vt);
        }
    } else {
        ray_t* vl = by_col(a, t, gidxs);
        if (!vl || RAY_IS_ERR(vl)) r = vl ? vl : q_err(QE_TYPE);
        else {
            r = q_bang(ray_table_get_col_idx(kt, 0), vl);
            ray_release(vl);
        }
    }
    ray_release(gidxs);
    ray_release(g);
    return r;
}

/* Exec-by, b a sym VECTOR (funsql.md "Group by columns", doc-literal): the
 * key is the empty symbol and the values run over the WHOLE filtered set. */
static ray_t* by_exec_vec(ray_t* a, ray_t* t, ray_t* idx) {
    ray_t* es = ray_sym(ray_sym_intern_runtime("", 0));
    ray_t* ek = (es && !RAY_IS_ERR(es)) ? q_enlist_wrap(&es, 1) : es;
    if (es && !RAY_IS_ERR(es) && ek != es) ray_release(es);
    if (!ek || RAY_IS_ERR(ek)) return ek ? ek : q_err(QE_TYPE);
    ray_t* r;
    if (q_type_is_dict(a)) {
        ray_t* vals = sel_cols(ray_dict_vals(a), t, idx, 0);
        if (RAY_IS_ERR(vals)) r = vals;
        else {
            int64_t nc = ray_len(vals);          /* enlist each -> 1-row table */
            ray_t* cols1 = ray_list_new(nc > 0 ? nc : 1);
            for (int64_t j = 0; j < nc && !RAY_IS_ERR(cols1); j++) {
                ray_t* v = q_index_elem_at(vals, j);
                ray_t* e = (v && !RAY_IS_ERR(v)) ? q_enlist_wrap(&v, 1) : v;
                if (v && !RAY_IS_ERR(v) && e != v) ray_release(v);
                if (!e || RAY_IS_ERR(e)) { ray_release(cols1); cols1 = e ? e : q_err(QE_TYPE); break; }
                cols1 = ray_list_append(cols1, e);
                ray_release(e);
            }
            ray_release(vals);
            ray_t* vt = RAY_IS_ERR(cols1) ? cols1
                        : named_table(ray_dict_keys(a), cols1);
            if (!RAY_IS_ERR(cols1) && vt != cols1) ray_release(cols1);
            if (!vt || RAY_IS_ERR(vt)) r = vt ? vt : q_err(QE_TYPE);
            else r = q_bang(ek, vt), ray_release(vt);
        }
    } else if (!is_empty_gen(a)) {
        ray_t* v = phrase_eval(a, t, idx);
        ray_t* vl = (v && !RAY_IS_ERR(v)) ? q_enlist_wrap(&v, 1) : v;
        if (v && !RAY_IS_ERR(v) && vl != v) ray_release(v);
        if (!vl || RAY_IS_ERR(vl)) r = vl ? vl : q_err(QE_TYPE);
        else r = q_bang(ek, vl), ray_release(vl);
    } else {
        r = q_err(QE_NYI);
    }
    ray_release(ek);
    return r;
}


static ray_t* ques_select(ray_t** args, int64_t n) {
    if (n == 6) return q_err(QE_NYI);            /* rank 6: the wave-5 sort */
    ray_t* t = ques_from(args[0]);
    if (!t || RAY_IS_ERR(t)) return t ? t : q_err(QE_TYPE);
    ray_t* idx0 = til_count(t);
    ray_t* idx = RAY_IS_ERR(idx0) ? idx0 : where_fold(args[1], t, idx0);
    if (idx != idx0 && !RAY_IS_ERR(idx0)) ray_release(idx0);
    if (RAY_IS_ERR(idx)) { ray_release(t); return idx; }
    ray_t* b = args[2];
    ray_t* a = args[3];
    ray_t* r;
    if (is_bool_atom(b, 0)) {
        if (is_empty_gen(a))            r = gather(t, idx);         /* law 5/7 */
        else if (q_type_is_dict(a))     r = sel_table(a, t, idx);
        else                            r = q_err(QE_NYI);          /* matrix "-" */
    } else if (is_empty_gen(b)) {
        r = exec_shape(a, t, idx);
    } else if (is_bool_atom(b, 1)) {    /* b=1b: distinct of the rank-4 result */
        ray_t* sub;
        if (is_empty_gen(a))            sub = gather(t, idx);
        else if (q_type_is_dict(a))     sub = sel_table(a, t, idx);
        else                            sub = q_err(QE_NYI);
        if (!sub || RAY_IS_ERR(sub)) r = sub ? sub : q_err(QE_TYPE);
        else { r = q_distinct_wrap(sub); ray_release(sub); }
    } else if (q_type_is_dict(b) || b->type == -RAY_SYM) {
        ray_t* trees = NULL;
        ray_t* names = by_specs(b, &trees);
        if (!names || RAY_IS_ERR(names)) r = names ? names : q_err(QE_TYPE);
        else {
            r = (b->type == -RAY_SYM) ? by_exec_atom(names, trees, a, t, idx)
                                      : by_select(names, trees, a, t, idx);
            ray_release(names);
            ray_release(trees);
        }
    } else if (b->type == RAY_SYM) {
        r = by_exec_vec(a, t, idx);
    } else {
        r = q_err(QE_TYPE);
    }
    ray_release(idx);
    ray_release(t);
    if (n >= 5) r = limit_apply(args[4], r);
    return r;
}


/* Simple Exec `?[t;i;p]` (law 10): the phrase over t[i] */
static ray_t* ques_simple_exec(ray_t* t, ray_t* i, ray_t* p) {
    ray_t* rt = ques_from(t);
    if (!rt || RAY_IS_ERR(rt)) return rt ? rt : q_err(QE_TYPE);
    ray_t* r = phrase_eval(p, rt, i);
    ray_release(rt);
    return r;
}

/* Vector Conditional (ref/vector-conditional.md, law 12): item-wise merge,
 * atoms repeat, an atom x picks a whole side */
static ray_t* vec_cond(ray_t* b, ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    if (b->type == -RAY_BOOL) {
        ray_t* pick = b->b8 ? x : y;
        ray_retain(pick);
        return pick;
    }
    int64_t n = ray_len(b);
    if ((q_type_is_iter(x) && q_count_long(x) != n) ||
        (q_type_is_iter(y) && q_count_long(y) != n))
        return q_err(QE_LENGTH);
    const bool* bp = (const bool*)ray_data(b);
    ray_t* l = ray_list_new(n > 0 ? n : 1);
    for (int64_t j = 0; j < n && !RAY_IS_ERR(l); j++) {
        ray_t* pick = bp[j] ? x : y;
        ray_t* e;
        if (q_type_is_iter(pick)) e = q_index_elem_at(pick, j);
        else { ray_retain(pick); e = pick; }
        if (!e || RAY_IS_ERR(e)) { ray_release(l); return e ? e : q_err(QE_TYPE); }
        l = ray_list_append(l, e);
        ray_release(e);
    }
    if (RAY_IS_ERR(l)) return l;
    ray_t* c = q_list_collapse(l);
    ray_release(l);
    return c;
}


ray_t* q_funsql_ques_wrap(ray_t** args, int64_t n) {
    if (n == 2) return q_roll_wrap(args[0], args[1]);
    if (n == 3) {
        if (args[0] && (args[0]->type == RAY_BOOL || args[0]->type == -RAY_BOOL))
            return vec_cond(args[0], args[1], args[2]);
        return ques_simple_exec(args[0], args[1], args[2]);
    }
    if (n >= 4 && n <= 6) return ques_select(args, n);
    return q_err(QE_RANK);
}

ray_t* q_funsql_bang_wrap(ray_t** args, int64_t n) {
    if (n == 2) return q_bang(args[0], args[1]);
    if (n == 4) return q_err(QE_NYI);            /* update/delete: wave 4 */
    return q_err(QE_RANK);
}
