/* q_outline — see q_outline.h.  Each statement `\l` would run is parsed by the strict parser and never evaluated.  Its
 * assignments are walked in the order evaluation makes them, so the first global write, the one the load's doc law
 * hands the statement's header to (q_comment.c), is known without a run. */
#include "qlang/q_count.h"
#include "qlang/parse/q_outline.h"
#include "qlang/parse/q_parse.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"   /* Q_IS_HOLE */
#include "qlang/q_comment.h"     /* q_comment_dry — the load's walk and its comment law, dry */
#include "qlang/q_env.h"         /* q_env_bind_native, q_env_fullname_in */
#include "qlang/q_prim.h"        /* q_str_text_bytes */
#include "qlang/q_registry.h"
#include "qlang/eval/q_eval.h"
#include "qlang/eval/q_eval_internal.h"   /* q_eval_ctl_sym — the control words evaluate left to right */
#include "qlang/ops/q_sys.h"     /* q_sys_ctx_named — what `\d name` switches to */
#include "lang/env.h"            /* ray_fn_unary */
#include "lang/eval.h"           /* RAY_FN_NONE */
#include <stdint.h>
#include <string.h>

typedef struct {
    const char *src, *stmt;
    const int64_t *ls;
    int64_t n, nls, line, ctx;   /* ls: src's line starts; line: where the statement starts; ctx: `\d` as text sets it */
    ray_t *pos;                  /* the statement's caret table, borrowed */
    ray_t *name, *ns, *kind, *params, *at, *doc;
    ray_t *system, *flip, *bang, *cond; /* borrowed registry values */
} outline_t;

/* A caret offset is into the statement's text, which keeps every byte of its lines but a CR before LF, and every line
 * but a `/`..`\` block's: a cut moves the lines after it down in src, so the name is found again on its column. */
static int64_t outline_at(const outline_t *o, const ray_t *node) {
    const int64_t *p = o->pos ? (const int64_t *)ray_data(o->pos) : NULL;
    ray_t *s = ray_sym_str(node->i64);
    size_t len = s ? (size_t)ray_str_len(s) : 0;
    for (int64_t i = 0; p && s && i + 1 < q_count(o->pos); i += 2) {
        if (p[i] != (int64_t)(uintptr_t)node) continue;
        int64_t off = p[i + 1], k = 0, bol = 0;
        for (int64_t j = 0; j < off && o->stmt[j]; j++) if (o->stmt[j] == '\n') { k++; bol = j + 1; }
        for (int64_t l = o->line - 1 + k, b; l < o->nls; l++)
            if ((b = o->ls[l] + off - bol) + (int64_t)len <= o->n && !memcmp(o->src + b, ray_str_ptr(s), len)) return b;
        return -1;
    }
    return -1;
}

static int outline_flipped(const outline_t *o, ray_t *v) {
    ray_t **e = v && v->type == RAY_LIST && q_count(v) == 2 ? (ray_t **)ray_data(v) : NULL;
    return e && e[0] == o->flip && e[1] && e[1]->type == RAY_LIST && q_count(e[1]) && ((ray_t **)ray_data(e[1]))[0] == o->bang;
}

/* Decided from the tree alone: a name applied with a hole may be a list indexed at depth, so it stays a value. */
static const char *outline_kind(const outline_t *o, ray_t *v, ray_t **lambda) {
    while (v && v->type == RAY_LIST && q_count(v) == 3 && q_parse_assign_head(((ray_t **)ray_data(v))[0], NULL, NULL))
        v = ((ray_t **)ray_data(v))[2];
    if (q_eval_apply_carrier_kind(v) == Q_EVAL_CAR_LAMBDA) { *lambda = v; return "function"; }
    if (!v || v->type != RAY_LIST || !q_count(v)) return "value";
    ray_t **e = (ray_t **)ray_data(v);
    int64_t n = q_count(v), rank;
    if (outline_flipped(o, v) || (n == 3 && e[0] == o->bang && outline_flipped(o, e[1]) && outline_flipped(o, e[2])))
        return "table";
    if (!e[0] || !q_eval_apply_is_fn(e[0])) return "value";
    for (int64_t i = 1; i < n; i++) if (Q_IS_HOLE(e[i])) return "projection";
    rank = q_eval_apply_rank(e[0]);
    return rank > 0 && n - 1 < rank ? "projection" : "value";
}

static void outline_put(ray_t **col, ray_t *item) {
    *col = ray_list_append(*col, item);
    ray_release(item);
}

static void outline_sym(ray_t **col, const char *s) {
    int64_t k = ray_sym_intern_runtime(s, strlen(s));
    *col = ray_vec_append(*col, &k);
}

static void outline_assign(outline_t *o, ray_t **e, q_assign_head_t h, int view) {
    ray_t *header = q_comment_dry_claim();
    int64_t at = (h == Q_HEAD_ASSIGN || h == Q_HEAD_GLOBAL) && q_parse_assign_target(e[1]) == Q_TARGET_NAME
               ? outline_at(o, e[1]) : -1, ns, full;
    if (at < 0) {
        if (header) ray_release(header);
        return;
    }
    ray_t *lambda = NULL, *params = NULL;
    const char *kind = view ? "view" : outline_kind(o, e[2], &lambda);
    full = q_env_fullname_in(view ? 0 : o->ctx, e[1]->i64, &ns);   /* a view is the root's under any `\d` */
    o->name = ray_vec_append(o->name, &full);
    o->ns = ray_vec_append(o->ns, &ns);
    outline_sym(&o->kind, kind);
    if (lambda && q_eval_apply_lambda_parts(lambda, &params, NULL, NULL) && params) ray_retain(params);
    outline_put(&o->params, params ? params : ray_sym_vec_new(RAY_SYM_W64, 0));
    o->at = ray_vec_append(o->at, &at);
    outline_put(&o->doc, header ? header : ray_charv("", 0));
}

/* `\d name` parses to `system "d name"`; the dispatcher's first word is the name. */
static int outline_cd(outline_t *o, ray_t *arg) {
    const char *p;
    int64_t n, i = 1, a;
    if (!q_str_text_bytes(arg, &p, &n) || !n || p[0] != 'd' || (n > 1 && p[1] != ' ' && p[1] != '\t')) return 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
    for (a = i; i < n && p[i] != ' ' && p[i] != '\t'; ) i++;
    int64_t ctx = i > a ? q_sys_ctx_named(p + a, (size_t)(i - a)) : -1;
    if (ctx >= 0) o->ctx = ctx;
    return 1;
}

/* q_view_intercept's shape: the statement's own `name::e`, name undotted, whose body (the rest of a `;` sequence too)
 * is the view's and never runs. */
static int outline_view(ray_t **e, q_assign_head_t h) {
    ray_t *s = h == Q_HEAD_GLOBAL && e[1] && e[1]->type == -RAY_SYM ? ray_sym_str(e[1]->i64) : NULL;
    return s && !memchr(ray_str_ptr(s), '.', (size_t)ray_str_len(s));
}

/* q_eval's order: right to left, an assignment after its right side and then its index; a `;` sequence, a control
 * word and Cond (`$` with three arguments or more) left to right; a one-item list is a constant.  Answers whether a
 * view took the rest of the statement. */
static int outline_walk(outline_t *o, ray_t *x, int root) {
    if (!x || x->type != RAY_LIST || q_count(x) < 2) return 0;
    ray_t **e = (ray_t **)ray_data(x);
    int64_t n = q_count(x);
    int seq = q_parse_is_seq_head(e[0]);
    if (seq || (e[0] && e[0]->type == -RAY_SYM && q_eval_ctl_sym(e[0]->i64)) || (n >= 4 && e[0] == o->cond)) {
        for (int64_t i = 1; i < n; i++) if (outline_walk(o, e[i], seq && root && i == 1)) break;
        return 0;
    }
    q_assign_head_t h = n == 3 ? q_parse_assign_head(e[0], NULL, NULL) : Q_HEAD_NONE;
    if (h && root && outline_view(e, h)) {
        outline_assign(o, e, h, 1);
        return 1;
    }
    if (h) {
        outline_walk(o, e[2], 0);
        if (q_parse_assign_target(e[1]) == Q_TARGET_INDEX)
            for (int64_t i = q_count(e[1]) - 1; i > 0; i--) outline_walk(o, ((ray_t **)ray_data(e[1]))[i], 0);
        outline_assign(o, e, h, 0);
        return 0;
    }
    if (n == 2 && e[0] == o->system && outline_cd(o, e[1])) return 0;
    for (int64_t i = n - 1; i >= 0; i--) outline_walk(o, e[i], 0);
    return 0;
}

static int outline_stmt(const char *s, size_t n, int64_t line, void *u) {
    (void)n;
    outline_t *o = u;
    ray_t *pos = NULL, *t = q_parse_at(s, &pos);
    if (t && !RAY_IS_ERR(t)) {
        o->line = line;
        o->stmt = s;
        o->pos = pos;
        outline_walk(o, t, 1);
    }
    if (t && RAY_IS_ERR(t)) {
        q_err_drop();
        ray_error_free(t);
    } else if (t) ray_release(t);
    if (pos) ray_release(pos);
    return 0;
}

static ray_t *outline_put_col(ray_t *tb, const char *name, ray_t *col) {
    tb = ray_table_add_col(tb, ray_sym_intern_runtime(name, strlen(name)), col);
    ray_release(col);
    return tb;
}

static ray_t *outline_fn(ray_t *x) {
    const char *p;
    int64_t n;
    if (!q_str_text_bytes(x, &p, &n)) return q_err(QE_TYPE);
    ray_t *ls = ray_vec_new(RAY_I64, 64), *held = q_err_take(), *header = NULL;
    int64_t z = 0;
    ls = ray_vec_append(ls, &z);
    for (int64_t i = 0; i < n; i++) if (p[i] == '\n') { z = i + 1; ls = ray_vec_append(ls, &z); }
    outline_t o = { .src = p, .n = n, .ls = (const int64_t *)ray_data(ls), .nls = q_count(ls),
                    .name = ray_sym_vec_new(RAY_SYM_W64, 8), .ns = ray_sym_vec_new(RAY_SYM_W64, 8),
                    .kind = ray_sym_vec_new(RAY_SYM_W64, 8), .params = ray_list_new(8), .at = ray_vec_new(RAY_I64, 8),
                    .doc = ray_list_new(8), .system = q_registry_lookup_name("system", 6, Q_MONADIC),
                    .flip = q_registry_lookup_name("+", 1, Q_MONADIC), .bang = q_registry_lookup_name("!", 1, Q_DYADIC),
                    .cond = q_registry_lookup_name("$", 1, Q_DYADIC) };
    q_comment_dry(p, (size_t)n, outline_stmt, &o, &header);
    ray_release(ls);
    ray_t *tb = ray_table_new(6);
    tb = outline_put_col(tb, "name", o.name);
    tb = outline_put_col(tb, "ns", o.ns);
    tb = outline_put_col(tb, "kind", o.kind);
    tb = outline_put_col(tb, "params", o.params);
    tb = outline_put_col(tb, "at", o.at);
    tb = outline_put_col(tb, "doc", o.doc);
    ray_t *keys = ray_sym_vec_new(RAY_SYM_W64, 2), *vals = ray_list_new(2);
    int64_t k[2] = { ray_sym_intern_runtime("file", 4), ray_sym_intern_runtime("defs", 4) };
    keys = ray_vec_append(ray_vec_append(keys, &k[0]), &k[1]);
    outline_put(&vals, header ? header : ray_charv("", 0));
    outline_put(&vals, tb);
    q_err_put(held);
    return ray_dict_new(keys, vals);
}

void q_outline_pq_register(void) {
    ray_t *f = ray_fn_unary(".pq.i.outline", RAY_FN_NONE, outline_fn);
    q_env_bind_native(".pq.i.outline", f, 1);
    ray_release(f);
}
