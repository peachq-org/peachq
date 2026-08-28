/* io/q_loader.c — the target seam both readers reach (contract in q_loader.h). */
#include "qlang/q_registry_internal.h" /* q_insert_wrap / q_upsert_wrap — the by-name row-append */
#include "qlang/base/q_err.h"
#include "qlang/eval/q_eval.h"        /* q_eval_apply_value/_is_fn/_rank — the lambda + .q.xcol seam */
#include "qlang/io/q_loader.h"
#include "qlang/q_env.h"              /* q_env_get — the global the symbol target names */
#include "qlang/q_prim.h"             /* q_list_collapse */
#include "table/sym.h"                /* ray_sym_intern_runtime */
#include <rayforce.h>
#include <stdlib.h>
#include <string.h>

static ray_t* loader_symvec(const int64_t* names, int64_t n) {
    ray_t* l = ray_list_new(n ? n : 1);
    if (RAY_IS_ERR(l)) return l;
    for (int64_t i = 0; i < n; i++) {
        ray_t* s = ray_sym(names[i]);
        l = ray_list_append(l, s);
        ray_release(s);
        if (RAY_IS_ERR(l)) return l;
    }
    ray_t* v = q_list_collapse(l);
    ray_release(l);
    return v ? v : q_err(QE_OOM);
}

ray_t* q_loader_syms(const int64_t* names, int64_t n) {
    return n ? loader_symvec(names, n) : ray_sym_vec_new(RAY_SYM_W64, 1);
}

ray_t* q_loader_types_dict(const int64_t* names, const char* chars, int64_t n) {
    ray_t* keys = loader_symvec(names, n);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_charv(chars, n);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    return ray_dict_new(keys, vals);               /* consumes both */
}

/* a dict of n syms over n OWNED values: every value is consumed, and a value that arrived
 * as an ERROR is the error propagated — unmodified, and never replaced by an OOM */
static ray_t* loader_dict(const char* const* kn, ray_t** vs, int n, int collapse) {
    ray_t* bad = NULL;
    for (int i = 0; i < n; i++)
        if (!bad && (!vs[i] || RAY_IS_ERR(vs[i]))) bad = vs[i] ? vs[i] : q_err(QE_OOM);
    if (bad) {
        for (int i = 0; i < n; i++)
            if (vs[i] && vs[i] != bad) ray_release(vs[i]);
        return bad;
    }
    ray_t* kl = ray_list_new(n);
    ray_t* vl = RAY_IS_ERR(kl) ? NULL : ray_list_new(n);
    for (int i = 0; i < n && vl && !RAY_IS_ERR(kl) && !RAY_IS_ERR(vl); i++) {
        ray_t* s = ray_sym(ray_sym_intern_runtime(kn[i], strlen(kn[i])));
        kl = ray_list_append(kl, s);
        ray_release(s);
        if (!RAY_IS_ERR(kl)) vl = ray_list_append(vl, vs[i]);
    }
    for (int i = 0; i < n; i++) ray_release(vs[i]);
    ray_t* e = RAY_IS_ERR(kl) ? kl : (!vl ? q_err(QE_OOM) : (RAY_IS_ERR(vl) ? vl : NULL));
    if (e) {
        if (kl != e) ray_release(kl);
        if (vl && vl != e) ray_release(vl);
        return e;
    }
    ray_t* keys = q_list_collapse(kl);
    ray_release(kl);
    if (!keys || RAY_IS_ERR(keys)) { ray_release(vl); return keys ? keys : q_err(QE_OOM); }
    ray_t* vals = vl;
    if (collapse) {
        vals = q_list_collapse(vl);
        ray_release(vl);
        if (!vals || RAY_IS_ERR(vals)) { ray_release(keys); return vals ? vals : q_err(QE_OOM); }
    }
    return ray_dict_new(keys, vals);               /* consumes both */
}

ray_t* q_loader_summary(int64_t rows, int64_t rejected, int64_t chunks, ray_t* ignored, ray_t* types) {
    static const char* const kn[5] = { "rows", "rejected", "chunks", "ignored", "types" };
    ray_t* vs[5] = { ray_i64(rows), ray_i64(rejected), ray_i64(chunks), ignored, types };
    return loader_dict(kn, vs, 5, 0);
}

int64_t q_loader_sink_find(const q_loader_sink* s, int64_t name) {
    for (int64_t k = 0; k < s->n; k++)
        if (s->names[k] == name) return k;
    return -1;
}

/* An EXISTING symbol target fixes the parse map: per-column chars from its meta (key table
 * first for a keyed target, which also routes emission to upsert).  A list/str column is the
 * string COLUMN -> '*'; the tag<->char owner serves every other tag. */
static ray_t* loader_schema(q_loader_sink* s, ray_t* g) {
    ray_t* parts[2] = { g, NULL };
    if (q_type_is_keyed(g)) {
        parts[0] = ray_dict_keys(g);
        parts[1] = ray_dict_vals(g);
        s->upsert = 1;
    }
    int64_t n = ray_table_ncols(parts[0]) + (parts[1] ? ray_table_ncols(parts[1]) : 0);
    s->names = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    s->chars = (char*)malloc((size_t)n);
    if (!s->names || !s->chars) return q_err(QE_WSFULL);
    int64_t k = 0;
    for (int p = 0; p < 2 && parts[p]; p++)
        for (int64_t c = 0; c < ray_table_ncols(parts[p]); c++, k++) {
            s->names[k] = ray_table_col_name(parts[p], c);
            ray_t* col = ray_table_get_col_idx(parts[p], c);   /* borrowed */
            s->chars[k] = (col->type == RAY_LIST || col->type == RAY_STR) ? '*' : q_type_char(col->type);
        }
    s->n = n;
    s->has_schema = 1;
    return NULL;
}

ray_t* q_loader_sink_open(q_loader_sink* s, ray_t* target) {
    memset(s, 0, sizeof *s);
    if (!target || target->type == RAY_NULL) return NULL;
    if (q_eval_apply_is_fn(target)) {
        if (q_eval_apply_rank(target) != 3) return q_err(QE_RANK);
        s->kind = 2;
        s->target = target;
        return NULL;
    }
    if (target->type != -RAY_SYM) return q_err(QE_TYPE);
    s->kind = 1;
    s->target = target;
    ray_t* g = q_env_get(target->i64);              /* the same resolution insert itself uses */
    return (g && (g->type == RAY_TABLE || q_type_is_keyed(g))) ? loader_schema(s, g) : NULL;
}

void q_loader_sink_free(q_loader_sink* s) {
    free(s->names);
    free(s->chars);
    s->names = NULL;
    s->chars = NULL;
}

ray_t* q_loader_sink_emit(q_loader_sink* s, ray_t* tbl, ray_t* errdata, int64_t chunk, int64_t rows) {
    ray_t* r;
    if (s->kind == 1) r = s->upsert ? q_upsert_wrap(s->target, tbl) : q_insert_wrap(s->target, tbl);
    else {
        static const char* const kn[2] = { "chunk", "rows" };   /* the extensible side channel */
        ray_t* vs[2] = { ray_i64(chunk), ray_i64(rows) };
        ray_t* misc = loader_dict(kn, vs, 2, 1);
        if (!misc || RAY_IS_ERR(misc)) return misc ? misc : q_err(QE_OOM);
        ray_t* args[3] = { tbl, errdata, misc };
        r = q_eval_apply_value(s->target, args, 3);
        ray_release(misc);
    }
    if (!r) return q_err(QE_OOM);
    if (RAY_IS_ERR(r)) return r;
    ray_release(r);
    return NULL;
}

ray_t* q_loader_rename(ray_t* spec, int64_t* names, int64_t n) {
    if (spec->type != -RAY_SYM && spec->type != RAY_SYM && spec->type != RAY_DICT) return q_err(QE_TYPE);
    ray_t* fn = q_env_get(ray_sym_intern(".q.xcol", 7));       /* borrowed */
    if (!fn) return q_err(QE_VALUE);
    ray_t* t = ray_table_new(n);                               /* names only: the verb's law decides the rest */
    for (int64_t j = 0; j < n && !RAY_IS_ERR(t); j++) {
        ray_t* c = ray_list_new(1);
        if (RAY_IS_ERR(c)) { ray_release(t); return c; }
        t = ray_table_add_col(t, names[j], c);
        ray_release(c);
    }
    if (RAY_IS_ERR(t)) return t;
    ray_t* args[2] = { spec, t };
    ray_t* r = q_eval_apply_value(fn, args, 2);
    ray_release(t);
    if (!r) return q_err(QE_OOM);
    if (RAY_IS_ERR(r)) return r;
    if (r->type != RAY_TABLE || ray_table_ncols(r) != n) {
        ray_release(r);
        return q_err(QE_LENGTH);
    }
    for (int64_t j = 0; j < n; j++) names[j] = ray_table_col_name(r, j);
    ray_release(r);
    return NULL;
}
