/* q_funsql — functional qSQL (basics/funsql.md, basics/qsql.md).  The
 * implementation IS the equivalence laws of the 2026-07-28 wave plan: phrases
 * run through q_eval under one NON-barrier column scope; Where is successive
 * refinement of an index vector; sel is `flip names!phrases`; limit composes
 * on take/drop; by rides the group core; update is dict update over Amend At;
 * delete is drop / complement select.  Order: From-Where-By-Select-Limit. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/eval/q_funsql.h"
#include "qlang/eval/q_eval.h"
#include "qlang/base/q_err.h"
#include "qlang/q_builtins.h"          /* q_count_long */
#include "qlang/q_prim.h"              /* q_cols_fn */
#include "qlang/q_registry_internal.h" /* q_take_wrap, q_where_wrap, q_flip_wrap, ... */
#include "qlang/ops/q_bang.h"
#include "qlang/ops/q_index.h"
#include "qlang/io/q_splay.h"          /* mapped splays: from materializes, writes 'splay */
#include "qlang/io/q_provider.h"         /* provider carriers: qsql push / materialize */
#include "qlang/q_env.h"
#include "lang/internal.h"             /* ray_til_fn, ray_typed_null, ray_except_fn */
#include "table/dict.h"                /* ray_dict_slots — keyed-table halves */
#include <stdlib.h>


static int is_empty_gen(ray_t* v) {
    return v && ((v->type == RAY_LIST && ray_len(v) == 0) || RAY_IS_NULL(v));
}

static int is_bool_atom(ray_t* v, int truth) {
    return v && q_type_is_bool(v) && (v->b8 != 0) == truth;
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
    ray_t* pm = q_provider_from_table(t);   /* carrier / `:pq: hsym: provider truth */
    if (pm) return pm;
    if (t->type == -RAY_SYM) {
        ray_t* v = q_env_resolve(t->i64);
        if (!v) return q_err(QE_NAME);
        if (RAY_IS_ERR(v)) return v;
        ray_t* r = ques_from(v);
        ray_release(v);
        return r;
    }
    if (q_splay_is(t)) { ray_retain(t); return t; }    /* lazy: columns gather on use */
    if (q_type_is_keyed(t)) return q_bang_enkey(0, t);
    if (q_type_is_dict(t)) return q_flip_wrap(t);
    if (q_type_is_table(t)) { ray_retain(t); return t; }
    return q_err(QE_TYPE);
}

/* one column of the from-value, OWNED (NULL = no such column) */
static ray_t* from_col_owned(ray_t* t, int64_t nm) {
    if (q_splay_is(t)) return q_splay_col(t, nm);
    ray_t* c = ray_table_get_col(t, nm);
    if (c) ray_retain(c);
    return c;
}

/* funsql's idx starts as til n and only ever REFINES, so an identity idx is
 * common — but `?[t;i;p]` hands USER i straight in, so identity is verified,
 * never inferred from the length alone */
static int idx_is_identity(ray_t* idx, int64_t n) {
    if (!idx || idx->type != RAY_I64 || ray_len(idx) != n) return 0;
    const int64_t* p = (const int64_t*)ray_data(idx);
    for (int64_t j = 0; j < n; j++)
        if (p[j] != j) return 0;
    return 1;
}

/* rows of the from-value at idx: a mapped splay rides the authority's row
 * gather; an identity idx takes the value itself without a copy */
static ray_t* from_rows(ray_t* t, ray_t* idx) {
    ray_t* use = idx_is_identity(idx, q_count_long(t)) ? NULL : idx;
    if (q_splay_is(t)) return q_splay_rows(t, use);
    if (!use) { ray_retain(t); return t; }
    return gather(t, use);
}

/* flip of the from-value: a mapped splay answers `flip` of its full row
 * gather (an update's merge holds every column by definition) */
static ray_t* from_flip(ray_t* t) {
    if (!q_splay_is(t)) return q_flip_wrap(t);
    ray_t* tbl = q_splay_rows(t, NULL);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_TYPE);
    ray_t* d = q_flip_wrap(tbl);
    ray_release(tbl);
    return d;
}

/* Law 4: THE evaluator runs the phrase inside one NON-barrier scope (qsql.md
 * name order: column -> enclosing locals -> global); virtual `i` = idx. */
static ray_t* phrase_eval(ray_t* tree, ray_t* t, ray_t* idx) {
    if (q_env_frame_push(0) != RAY_OK) return q_err(QE_STACK);
    ray_t* err = NULL;
    int ident = idx_is_identity(idx, q_count_long(t));
    if (q_splay_is(t)) {
        /* a mapped splay binds LAZY column refs — a name the phrase never
         * uses never touches its file; identity idx = gather with :: (no copy) */
        ray_t* keys = ray_dict_keys(t);
        int64_t nc = ray_len(keys);
        ray_t* use = ident ? RAY_NULL_OBJ : idx;
        for (int64_t c = 0; c < nc && !err; c++) {
            int64_t id = ray_vec_get_sym_id(keys, c);
            ray_t* ref = q_splay_colref(t, id, use);
            if (!ref || RAY_IS_ERR(ref)) { err = ref ? ref : q_err(QE_OOM); break; }
            q_env_local_set(id, ref);                        /* retains */
            ray_release(ref);
        }
    } else {
        /* the same narrowing law in-memory: an identity idx binds the column
         * ITSELF — the per-element gather here was the projection cliff that
         * priced `select c1 from m` by the table's WIDTH (PLAN.md 2026-08-04) */
        int64_t nc = ray_table_ncols(t);
        for (int64_t c = 0; c < nc && !err; c++) {
            ray_t* col = ray_table_get_col_idx(t, c);
            ray_t* g = ident ? (ray_retain(col), col) : gather(col, idx);
            if (!g || RAY_IS_ERR(g)) { err = g ? g : q_err(QE_TYPE); break; }
            q_env_local_set(ray_table_col_name(t, c), g);    /* retains */
            ray_release(g);
        }
    }
    if (!err) q_env_local_set(ray_sym_intern_runtime("i", 1), idx);
    ray_t* r = err ? err : q_eval_apply_concrete(q_eval(tree));
    q_env_frame_pop();
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

/* OWNER RULING 2026-08-05: qSQL must NEVER silently answer with duplicate
 * output column names — `select res,pass,total,tim,path,gg:t.path,...` once
 * produced two `tim` columns and the defect surfaced far downstream; default
 * to 'dup.  STRICTER than kdb here, deliberately: basics/qsql.md:168 says kdb
 * AUTO-ALIASES a collision WITHIN one phrase list (`select a,a`, `by c,c`)
 * and rejects only cols-vs-groups (that half lives at the PARSE seam,
 * q_parse.c, per the same doc line's "during parse").  These eval seams also
 * cover the functional door (`?[t;();0b;`a`a!...]`).  Raw construction stays
 * permissive — `` `p`o`p!1 2 3 `` and `flip` of such a dict are legal values,
 * so `!`/`flip` must not learn this rule. */
static int names_collide(ray_t* names) {
    if (!names || names->type != RAY_SYM) return 0;
    int64_t n = ray_len(names);
    const int64_t* s = (const int64_t*)ray_data(names);
    for (int64_t i = 1; i < n; i++)
        for (int64_t j = 0; j < i; j++)
            if (s[i] == s[j]) return 1;
    return 0;
}

/* Law 8: a phrase-columns result table IS `flip names!cols` — and `flip` IS
 * the conform law, so a Select hands it the RAW phrase values: an atom beside
 * a column rides that column's length, and phrases that ALL gave atoms make
 * the one-row aggregate table (`select sum a from ([]a:1 2 3)` is a single 6,
 * basics/qsql.md).  A By key instead spans the rows, so it conforms first. */
static ray_t* cols_table(ray_t* names, ray_t* rng, ray_t* t, ray_t* idx, int conform) {
    if (names_collide(names)) return q_err(QE_DUP);
    ray_t* vals = sel_cols(rng, t, idx, conform);
    if (RAY_IS_ERR(vals)) return vals;
    ray_t* d = q_bang(names, vals);
    ray_release(vals);
    if (!d || RAY_IS_ERR(d)) return d ? d : q_err(QE_TYPE);
    ray_t* r = q_flip_wrap(d);
    ray_release(d);
    return r;
}

static ray_t* sel_table(ray_t* a, ray_t* t, ray_t* idx) {
    return cols_table(ray_dict_keys(a), ray_dict_vals(a), t, idx, 0);
}

/* Exec at b=() (law 11): () -> last row; sym/tree -> value; dict unconformed */
static ray_t* exec_shape(ray_t* a, ray_t* t, ray_t* idx) {
    if (is_empty_gen(a)) {
        ray_t* rows = from_rows(t, idx);
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
    if (names_collide(names)) return q_err(QE_DUP);
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

/* per group: the ORIGINAL row numbers, idx@positions (law 14) — the groups
 * taken in `ord` order, so every by-consumer is handed law 15 already done */
static ray_t* by_orig_idx(ray_t* idx, ray_t* gv, ray_t* ord) {
    int64_t ng = ray_len(gv);
    ray_t* out = ray_list_new(ng > 0 ? ng : 1);
    for (int64_t j = 0; j < ng && !RAY_IS_ERR(out); j++) {
        ray_t* pos = q_index_elem_at(gv, q_type_ivec_get(ord, j));
        ray_t* gi = (pos && !RAY_IS_ERR(pos)) ? gather(idx, pos) : pos;
        if (pos && gi != pos) ray_release(pos);
        if (!gi || RAY_IS_ERR(gi)) { ray_release(out); return gi ? gi : q_err(QE_TYPE); }
        out = ray_list_append(out, gi);
        ray_release(gi);
    }
    return out;
}

/* Zero groups evaluate no phrase, so nothing carries the result column's type.
 * The phrase's answer for a group of NO rows is that same evaluation at width
 * zero — the one thing that can say what the column would have held. */
static ray_t* by_empty_probe(ray_t* tree, ray_t* t) {
    ray_t* gi = q_type_empty(RAY_I64);
    ray_t* v = phrase_eval(tree, t, gi);
    ray_release(gi);
    return v ? v : q_err(QE_TYPE);
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
    if (ng) return c;
    /* enlisted, the probe IS the proto the empty column inherits from — an
     * aggregate's atom gives a typed vector, a uniform's vector a nested one */
    ray_t* v = by_empty_probe(tree, t);
    if (RAY_IS_ERR(v)) { ray_release(c); return v; }
    ray_t* proto = q_enlist_wrap(&v, 1);
    ray_release(v);
    if (RAY_IS_ERR(proto)) { ray_release(c); return proto; }
    c = q_typed_empty_like(c, proto);
    ray_release(proto);
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

/* grouped state shared by every by-consumer (laws 2+14): the by-table's
 * distinct rows as the owned key TABLE, and the original rows per group as
 * *gidxs_out.  Both come back ASCENDING BY KEY — law 15 is a By law, so it
 * belongs here, once, and not in each consumer; `group` itself keeps its
 * definitional first-occurrence order (ref/group.md). */
static ray_t* by_group(ray_t* names, ray_t* trees, ray_t* t, ray_t* idx,
                       ray_t** gidxs_out) {
    *gidxs_out = NULL;
    ray_t* bt = cols_table(names, trees, t, idx, 1);
    if (!bt || RAY_IS_ERR(bt)) return bt ? bt : q_err(QE_TYPE);
    ray_t* g = q_group_wrap(bt);
    ray_release(bt);
    if (!g || RAY_IS_ERR(g)) return g ? g : q_err(QE_TYPE);
    ray_t* ord = q_iasc_wrap(ray_dict_keys(g));
    if (!ord || RAY_IS_ERR(ord)) { ray_release(g); return ord ? ord : q_err(QE_TYPE); }
    ray_t* gidxs = by_orig_idx(idx, ray_dict_vals(g), ord);
    ray_t* kt = RAY_IS_ERR(gidxs) ? gidxs : gather(ray_dict_keys(g), ord);
    ray_release(ord);
    ray_release(g);
    if (RAY_IS_ERR(gidxs)) return gidxs;
    if (!kt || RAY_IS_ERR(kt)) { ray_release(gidxs); return kt ? kt : q_err(QE_TYPE); }
    *gidxs_out = gidxs;
    return kt;
}

/* Select-by (b a dict — the keyword By lowering): result keyed by the group
 * key (already ascending — law 15 lives in by_group), built by the `!`
 * table!table law; a=() takes the last row per group minus the by-named
 * columns (select.md); a sym gives the dict shape (funsql.md "Group by a
 * dictionary"). */
static ray_t* by_select(ray_t* names, ray_t* trees, ray_t* a, ray_t* t,
                        ray_t* idx) {
    ray_t* gidxs = NULL;
    ray_t* kt = by_group(names, trees, t, idx, &gidxs);
    if (RAY_IS_ERR(kt)) return kt;
    ray_t* r;
    if (is_empty_gen(a)) {                              /* last row per group */
        ray_t* li = by_last_idx(gidxs);
        ray_t* rows = RAY_IS_ERR(li) ? li : from_rows(t, li);
        if (!RAY_IS_ERR(li) && rows != li) ray_release(li);
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
            else r = q_bang(kt, vt), ray_release(vt);
        }
    } else if (q_type_is_dict(a)) {
        ray_t* vcols = by_cols(ray_dict_vals(a), t, gidxs);
        ray_t* vt = RAY_IS_ERR(vcols) ? vcols
                    : named_table(ray_dict_keys(a), vcols);
        if (!RAY_IS_ERR(vcols) && vt != vcols) ray_release(vcols);
        if (!vt || RAY_IS_ERR(vt)) r = vt ? vt : q_err(QE_TYPE);
        else r = q_bang(kt, vt), ray_release(vt);
    } else {                                            /* a sym/tree -> dict */
        ray_t* vl = by_col(a, t, gidxs);
        if (!vl || RAY_IS_ERR(vl)) r = vl ? vl : q_err(QE_TYPE);
        else r = q_bang(kt, vl), ray_release(vl);
    }
    ray_release(kt);
    ray_release(gidxs);
    return r;
}

/* Exec-by, b a sym ATOM: plain dict keyed by the column's distinct values,
 * ascending like Select-by (law 15 — the hand-curated qsql/exec ledger decides
 * what the docs leave open); a=dict keys by a one-ANONYMOUS-column table
 * (funsql.md "Group by column"). */
static ray_t* by_exec_atom(ray_t* names, ray_t* trees, ray_t* a, ray_t* t,
                           ray_t* idx) {
    if (is_empty_gen(a)) return q_err(QE_NYI);          /* matrix "-" */
    ray_t* gidxs = NULL;
    ray_t* kt = by_group(names, trees, t, idx, &gidxs); /* 1-col distinct */
    if (RAY_IS_ERR(kt)) return kt;
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
    ray_release(kt);
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
    ray_t* pushed = q_provider_qsql_push(args, n); /* NULL: not provider / no qsql hook */
    if (pushed) return pushed;
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
        if (is_empty_gen(a))            r = from_rows(t, idx);         /* law 5/7 */
        else if (q_type_is_dict(a))     r = sel_table(a, t, idx);
        else                            r = q_err(QE_NYI);          /* matrix "-" */
    } else if (is_empty_gen(b)) {
        r = exec_shape(a, t, idx);
    } else if (is_bool_atom(b, 1)) {    /* b=1b: distinct of the rank-4 result */
        ray_t* sub;
        if (is_empty_gen(a))            sub = from_rows(t, idx);
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


/* ===== `#` / `_` on the entries axis ======================================
 * One index derived from the DOMAIN drives both halves and `!` rebuilds it —
 * the same law for a dict (vector domain), a keyed table (table domain) and,
 * through `flip`, a table's columns.
 *
 * The two verbs read the index differently.  Take answers in PROBE order and
 * so may repeat or reorder entries (ref/take.md "returns matching rows");
 * Drop answers in DOMAIN order, each unselected entry once.  Both ignore a key
 * the search missed, which is why `` `b`x _ d `` drops d of `b` alone
 * (basics/dictsandtables.md). */

/* one search position, whether Find answered with an atom or a run; -1 for
 * anything that is not an int lane, which entries_index reads as "no entry" */
static int64_t search_pos_at(ray_t* pos, int64_t j) {
    if (ray_is_atom(pos)) return q_type_is_int_atom(pos) ? q_type_iatom_val(pos) : -1;
    return q_type_is_int_vec(pos) ? q_type_ivec_get(pos, j) : -1;
}

/* The found positions in probe order (take), or the unfound domain positions in
 * domain order (drop).  Find answers a miss with the count, so "found" is just
 * "in range".  Consumes nothing; owned i64 vector. */
static ray_t* entries_index(ray_t* pos, int64_t n, int drop) {
    int64_t m = ray_is_atom(pos) ? 1 : ray_len(pos);
    ray_t* out = ray_vec_new(RAY_I64, (drop ? n : m) > 0 ? (drop ? n : m) : 1);
    if (RAY_IS_ERR(out)) return out;
    int64_t* o = (int64_t*)ray_data(out);
    int64_t c = 0;
    if (!drop) {
        for (int64_t j = 0; j < m; j++) {
            int64_t p = search_pos_at(pos, j);
            if (p >= 0 && p < n) o[c++] = p;
        }
    } else {
        char* hit = calloc((size_t)(n > 0 ? n : 1), 1);
        if (!hit) { ray_release(out); return q_err(QE_OOM); }
        for (int64_t j = 0; j < m; j++) {
            int64_t p = search_pos_at(pos, j);
            if (p >= 0 && p < n) hit[p] = 1;
        }
        for (int64_t p = 0; p < n; p++)
            if (!hit[p]) o[c++] = p;
        free(hit);
    }
    out->len = c;
    return out;
}

/* an empty selection keeps the source's element type (`` type key `a _ `a!1 ``
 * is 11h, not 0h) */
static ray_t* entries_gather(ray_t* x, ray_t* idx) {
    return q_typed_empty_like(q_index_at(x, &idx, 1), x);
}

static ray_t* entries_select(ray_t* dom, ray_t* rng, ray_t* x, int drop) {
    ray_t* pos = q_search_find(dom, x);
    if (!pos || RAY_IS_ERR(pos)) return pos ? pos : q_err(QE_TYPE);
    ray_t* idx = entries_index(pos, q_count_long(dom), drop);
    ray_release(pos);
    if (RAY_IS_ERR(idx)) return idx;
    ray_t* nk = entries_gather(dom, idx);
    ray_t* nv = (nk && !RAY_IS_ERR(nk)) ? entries_gather(rng, idx) : NULL;
    ray_release(idx);
    ray_t* r = (nv && !RAY_IS_ERR(nv)) ? q_bang(nk, nv) : (nv ? nv : nk);
    if (nk && r != nk) ray_release(nk);
    if (nv && r != nv) ray_release(nv);
    return r ? r : q_err(QE_TYPE);
}

/* a table IS `flip` of its column dict, so column take/drop is the entries law
 * with the flip on either side */
static ray_t* cols_select(ray_t* x, ray_t* t, int drop) {
    ray_t* fd = q_flip_wrap(t);                              /* owned */
    if (!fd || RAY_IS_ERR(fd)) return fd ? fd : q_err(QE_TYPE);
    ray_t* nd = q_type_is_plain_dict(fd)
                    ? entries_select(ray_dict_keys(fd), ray_dict_vals(fd), x, drop)
                    : q_err(QE_TYPE);
    ray_release(fd);
    if (!nd || RAY_IS_ERR(nd)) return nd ? nd : q_err(QE_TYPE);
    ray_t* r = q_flip_wrap(nd);
    ray_release(nd);
    return r;
}

/* NULL = not an entries selection, the wrapper's fall-through signal. */
static ray_t* entries_verb(ray_t* x, ray_t* y, int drop) {
    if (!x || !y || ray_is_atom(x)) return NULL;
    if (q_type_is_keyed(y))                                  /* ([]k:…)#kt */
        return entries_select(ray_dict_slots(y)[0], ray_dict_slots(y)[1], x, drop);
    if (x->type != RAY_SYM) return NULL;
    if (y->type == RAY_TABLE) return cols_select(x, y, drop);
    if (y->type == RAY_DICT) return entries_select(ray_dict_keys(y), ray_dict_vals(y), x, drop);
    return NULL;
}

/* drop named entries: the doc's sub-dictionary extraction `(key d) except keys`
 * re-indexed as d[remaining] (ref/drop.md) */
static ray_t* dict_drop_keys(ray_t* keys, ray_t* d) {
    ray_t* rem = ray_except_fn(ray_dict_keys(d), keys);
    if (!rem || RAY_IS_ERR(rem)) return rem ? rem : q_err(QE_TYPE);
    ray_t* nv = q_index_at(d, &rem, 1);
    if (!nv || RAY_IS_ERR(nv)) { ray_release(rem); return nv ? nv : q_err(QE_TYPE); }
    nv = q_typed_empty_like(nv, ray_dict_vals(d));   /* dropping every key keeps the value type */
    return ray_dict_new(rem, nv);
}

ray_t* q_funsql_entries_take(ray_t* x, ray_t* y) { return entries_verb(x, y, 0); }
ray_t* q_funsql_entries_drop(ray_t* x, ray_t* y) { return entries_verb(x, y, 1); }
ray_t* q_funsql_dict_drop_keys(ray_t* keys, ray_t* d) { return dict_drop_keys(keys, d); }


/* ===== `!` rank 4: Update / Delete (wave 4) ============================== */

/* a fresh column of v's null, length n (update.md: a created column has
 * nulls wherever the Where phrase did not reach) */
static ray_t* null_col_like(ray_t* v, int64_t n) {
    int8_t ty = q_type_elem_tag(v);
    ray_t* na = ty ? ray_typed_null(ty) : NULL;
    if (!na) { na = RAY_NULL_OBJ; ray_retain(na); }
    ray_t* nn = ray_i64(n);
    ray_t* col = q_take_wrap(nn, na);
    ray_release(nn);
    ray_release(na);
    return col;
}

/* law 20's per-column step: the existing column (or a fresh null column)
 * amended at rows through Amend At — an atom broadcasts, a uniform result
 * scatters pairwise to the ORIGINAL positions. */
static ray_t* upd_amend(ray_t* cur, ray_t* rows, ray_t* v) {
    /* cur owned; consumed on success, released here on error */
    ray_t* r = q_index_amend_at(cur, rows, NULL, v);
    if (!r || RAY_IS_ERR(r)) {
        ray_release(cur);
        return r ? r : q_err(QE_TYPE);
    }
    return r;
}

/* What the amend starts from.  `full` = the update reaches EVERY row, so no
 * value of the old column survives and its type has no claim — a fresh null
 * column of the PHRASE's type is why `update avg weight by city from p` turns
 * an int column float (ref/update.md By phrase: the groups partition the whole
 * table).  Under a Where phrase the live column stands and "new values must
 * have the type of the column being amended"; a result naming no element type
 * (a nested column) claims nothing either way. */
static ray_t* upd_col_start(ray_t* t, int64_t nm, ray_t* v, int full) {
    ray_t* cur = from_col_owned(t, nm);                 /* owned or NULL */
    if (RAY_IS_ERR(cur)) return cur;
    if (cur && !(full && q_type_elem_tag(v))) return cur;
    if (cur) ray_release(cur);
    return null_col_like(v, q_count_long(t));
}

/* the updated columns as names!cols; grouped when b names groups */
static ray_t* upd_cols(ray_t* a, ray_t* t, ray_t* idx, ray_t* gidxs) {
    ray_t* keys = ray_dict_keys(a);
    ray_t* rng = ray_dict_vals(a);
    int64_t nc = q_count_long(rng);
    int full = q_count_long(idx) == q_count_long(t);
    ray_t* cols = ray_list_new(nc > 0 ? nc : 1);
    for (int64_t j = 0; j < nc && !RAY_IS_ERR(cols); j++) {
        ray_t* nm = q_index_elem_at(keys, j);
        ray_t* tree = q_index_elem_at(rng, j);
        ray_t* cur = NULL;
        ray_t* err = NULL;
        if (!nm || RAY_IS_ERR(nm) || !tree || RAY_IS_ERR(tree)) {
            err = q_err(QE_TYPE);
        } else if (!gidxs) {                             /* ungrouped */
            ray_t* v = phrase_eval(tree, t, idx);
            if (!v || RAY_IS_ERR(v)) err = v ? v : q_err(QE_TYPE);
            else {
                cur = upd_amend(upd_col_start(t, nm->i64, v, full), idx, v);
                if (RAY_IS_ERR(cur)) { err = cur; cur = NULL; }
                ray_release(v);
            }
        } else {                                         /* grouped (law 20) */
            int64_t ng = ray_len(gidxs);
            ray_t** gi = (ray_t**)ray_data(gidxs);
            for (int64_t k = 0; k < ng && !err; k++) {
                ray_t* v = phrase_eval(tree, t, gi[k]);
                if (!v || RAY_IS_ERR(v)) { err = v ? v : q_err(QE_TYPE); break; }
                if (!cur) cur = upd_col_start(t, nm->i64, v, full);
                cur = upd_amend(cur, gi[k], v);
                if (RAY_IS_ERR(cur)) { err = cur; cur = NULL; }
                ray_release(v);
            }
            if (!err && !cur) {                          /* zero groups */
                ray_t* v = by_empty_probe(tree, t);
                if (RAY_IS_ERR(v)) err = v;
                else {
                    cur = upd_col_start(t, nm->i64, v, full);
                    ray_release(v);
                    if (RAY_IS_ERR(cur)) { err = cur; cur = NULL; }
                }
            }
        }
        if (nm && !RAY_IS_ERR(nm)) ray_release(nm);
        if (tree && !RAY_IS_ERR(tree)) ray_release(tree);
        if (err) { ray_release(cols); return err; }
        cols = ray_list_append(cols, cur);
        ray_release(cur);
    }
    if (RAY_IS_ERR(cols)) return cols;
    ray_t* d = q_bang(keys, cols);
    ray_release(cols);
    return d;
}

/* Update on a table (law 20): amended columns then `flip (flip t),newcols` —
 * the dict-join right-override creates absent sel-key columns. */
static ray_t* update_table(ray_t* a, ray_t* b, ray_t* t, ray_t* idx) {
    ray_t* gidxs = NULL;
    ray_t* kt = NULL;
    int grouped = !(is_bool_atom(b, 0) || is_empty_gen(b));
    if (grouped) {
        ray_t* trees = NULL;
        ray_t* names = by_specs(b, &trees);
        if (!names || RAY_IS_ERR(names)) return names ? names : q_err(QE_TYPE);
        kt = by_group(names, trees, t, idx, &gidxs);
        ray_release(names);
        ray_release(trees);
        if (RAY_IS_ERR(kt)) return kt;
    }
    ray_t* nd = upd_cols(a, t, idx, gidxs);
    if (kt) { ray_release(gidxs); ray_release(kt); }
    if (!nd || RAY_IS_ERR(nd)) return nd ? nd : q_err(QE_TYPE);
    ray_t* cd = from_flip(t);
    ray_t* merged = (cd && !RAY_IS_ERR(cd)) ? q_join_wrap(cd, nd) : cd;
    if (cd && !RAY_IS_ERR(cd) && merged != cd) ray_release(cd);
    ray_release(nd);
    if (!merged || RAY_IS_ERR(merged)) return merged ? merged : q_err(QE_TYPE);
    ray_t* out = q_flip_wrap(merged);
    ray_release(merged);
    return out;
}

/* update on a plain dict: entries bind as the namespace; result is the
 * dict-join right-override d,a-evaluated (ref/update.md; by -> 'type 4.1) */
static ray_t* update_dict(ray_t* a, ray_t* b, ray_t* c, ray_t* d) {
    if (!(is_bool_atom(b, 0) || is_empty_gen(b))) return q_err(QE_TYPE);
    if (!is_empty_gen(c)) return q_err(QE_NYI);
    if (q_env_frame_push(0) != RAY_OK) return q_err(QE_STACK);
    ray_t* dk = ray_dict_keys(d);
    ray_t* dv = ray_dict_vals(d);
    int64_t n = q_count_long(dk);
    for (int64_t j = 0; j < n; j++) {
        ray_t* k = q_index_elem_at(dk, j);
        ray_t* v = q_index_elem_at(dv, j);
        if (k && !RAY_IS_ERR(k) && k->type == -RAY_SYM && v && !RAY_IS_ERR(v))
            q_env_local_set(k->i64, v);
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
    }
    ray_t* rng = ray_dict_vals(a);
    int64_t nc = q_count_long(rng);
    ray_t* vals = ray_list_new(nc > 0 ? nc : 1);
    for (int64_t j = 0; j < nc && !RAY_IS_ERR(vals); j++) {
        ray_t* tree = q_index_elem_at(rng, j);
        ray_t* v = (tree && !RAY_IS_ERR(tree))
                       ? q_eval_apply_concrete(q_eval(tree)) : tree;
        if (tree && v != tree) ray_release(tree);
        if (!v || RAY_IS_ERR(v)) { ray_release(vals); vals = v ? v : q_err(QE_TYPE); break; }
        vals = ray_list_append(vals, v);
        ray_release(v);
    }
    q_env_frame_pop();
    if (RAY_IS_ERR(vals)) return vals;
    ray_t* nd = q_bang(ray_dict_keys(a), vals);
    ray_release(vals);
    if (!nd || RAY_IS_ERR(nd)) return nd ? nd : q_err(QE_TYPE);
    ray_t* r = q_join_wrap(d, nd);
    ray_release(nd);
    return r;
}

/* is a the delete-columns shape: a non-empty symbol vector */
static int is_symvec(ray_t* a) { return a && a->type == RAY_SYM; }

/* `delete a from `.` / `![`.ns;();0b;`a`b]` — EXPUNGE (q4m3 §12.5).  A namespace
 * is not a dict to rewrite: each name is unbound from the K-tree, so an emptied
 * namespace survives (kdb keeps it in `key ``). */
static ray_t* expunge_ns(int64_t ns, ray_t* names) {
    int64_t n = ray_len(names);
    for (int64_t i = 0; i < n; i++) {
        int64_t member = ray_read_sym(ray_data(names), i, RAY_SYM, names->attrs);
        int64_t full = q_env_qualify(ns, member);
        if (full < 0 || q_env_unbind(full) != RAY_OK) return q_err(QE_ASSIGN);
    }
    return NULL;
}

static ray_t* bang_qsql(ray_t** args) {
    ray_t* tslot = args[0];
    ray_t* c = args[1];
    ray_t* b = args[2];
    ray_t* a = args[3];
    int64_t name = -1;
    ray_t* src;
    {   /* by-NAME mutation of a provider coordinate: table form is phase-2
         * ('nyi); a connection form in a table position is 'domain */
        int form = q_provider_coord_sym_form(tslot);
        if (form == 2) return q_err(QE_NYI);
        if (form == 1) return q_err(QE_DOMAIN);
    }
    if (tslot && tslot->type == -RAY_SYM) {
        name = tslot->i64;
        if (is_symvec(a) && ray_len(a) > 0 && is_empty_gen(c) &&
            q_env_ns_exists(name)) {
            ray_t* e = expunge_ns(name, a);
            if (e) return e;
            ray_retain(tslot);
            return tslot;
        }
        src = q_env_resolve(name);
        if (!src) return q_err(QE_NAME);
        if (RAY_IS_ERR(src)) return src;
    } else if (tslot) {
        ray_retain(src = tslot);
    } else {
        return q_err(QE_TYPE);
    }
    /* A mapped splay refuses schema changes and every write-back — BOTH delete
     * forms and the name form (kb/splayed-tables.md:350-354: `delete volume
     * from `trade` and `trade: delete volume from trade` are each 'splay).  A
     * value-form UPDATE is a plain query: the carrier flows into the table
     * lane below, its columns gathering on use. */
    if (q_splay_is(src)) {
        if (name >= 0 || !q_type_is_dict(a)) { ray_release(src); return q_err(QE_SPLAY); }
    }
    /* a provider carrier: by-NAME mutation is phase-2 ('nyi); by VALUE the
     * carrier materializes through the provider and updates locally */
    if (q_provider_carrier_is(src)) {
        if (name >= 0) { ray_release(src); return q_err(QE_NYI); }
        ray_t* m = q_provider_carrier_table(src);
        ray_release(src);
        if (!m || RAY_IS_ERR(m)) return m ? m : q_err(QE_TYPE);
        src = m;
    }
    ray_t* r;
    int64_t nk = 0;                 /* keyed source: re-key the result */
    if (q_type_is_dict(src) && !q_type_is_keyed(src) && !q_splay_is(src)) {
        if (q_type_is_dict(a))      r = update_dict(a, b, c, src);
        else if (is_symvec(a) && is_empty_gen(c)) r = dict_drop_keys(a, src);
        else                        r = q_err(QE_NYI);
    } else {
        ray_t* t = src;
        if (q_type_is_keyed(src)) {
            nk = ray_table_ncols(ray_dict_keys(src));
            t = q_bang_enkey(0, src);               /* law 24: unkey */
            if (!t || RAY_IS_ERR(t)) { ray_release(src); return t ? t : q_err(QE_TYPE); }
        } else if (q_type_is_table(src) || q_splay_is(src)) {
            ray_retain(t);
        } else {
            ray_release(src);
            return q_err(QE_TYPE);
        }
        if (is_symvec(a) && ray_len(a) > 0 && is_empty_gen(c)) {
            r = entries_verb(a, t, 1);              /* law 18: cols ≡ a _ t */
            /* a table stripped of EVERY column has no domain (an emptied dict does) */
            if (r && !RAY_IS_ERR(r) && ray_table_ncols(r) == 0) {
                ray_release(r);
                r = q_err(QE_DOMAIN);
            }
            nk = 0;                                 /* dropping may hit keys */
        } else {
            ray_t* idx0 = til_count(t);
            ray_t* idx = RAY_IS_ERR(idx0) ? idx0 : where_fold(c, t, idx0);
            if (idx != idx0 && !RAY_IS_ERR(idx0)) ray_release(idx0);
            if (RAY_IS_ERR(idx)) r = idx;
            else if (q_type_is_dict(a)) {           /* UPDATE */
                r = update_table(a, b, t, idx);
                ray_release(idx);
            } else if (is_empty_gen(a) || (is_symvec(a) && ray_len(a) == 0)) {
                /* law 19: delete rows ≡ the complement select */
                ray_t* full = til_count(t);
                ray_t* keep = RAY_IS_ERR(full) ? full : q_except_wrap(full, idx);
                if (full != keep && !RAY_IS_ERR(full)) ray_release(full);
                r = (keep && !RAY_IS_ERR(keep)) ? gather(t, keep) : keep;
                if (keep && !RAY_IS_ERR(keep) && r != keep) ray_release(keep);
                ray_release(idx);
            } else {
                ray_release(idx);
                r = q_err(QE_TYPE);
            }
        }
        if (nk > 0 && r && !RAY_IS_ERR(r)) {
            ray_t* rk = q_bang_enkey(nk, r);
            ray_release(r);
            r = rk;
        }
        ray_release(t);
    }
    ray_release(src);
    if (name >= 0 && r && !RAY_IS_ERR(r)) {
        /* name form (law 21): amend in place, hand back the name */
        ray_err_t e = q_env_set(name, r);
        if (e != RAY_OK) { ray_release(r); return q_env_err(e); }
        ray_release(r);
        ray_retain(tslot);
        return tslot;
    }
    return r;
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
    if (n == 4) return bang_qsql(args);
    return q_err(QE_RANK);
}
