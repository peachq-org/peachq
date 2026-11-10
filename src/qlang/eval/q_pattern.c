/* q_pattern — see q_pattern.h. */
#include "qlang/q_count.h"
#include "qlang/eval/q_pattern.h"
#include "qlang/eval/q_eval.h"      /* q_eval — a constant's value; q_eval_apply_is_fn */
#include "qlang/parse/q_parse.h"    /* q_parse_assign_head — the `p:`c` head */
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"      /* q_type_of_char, q_type_is_iter, the hole */
#include "qlang/q_builtins.h"       /* q_builtins_type_num */
#include "qlang/q_prim.h"           /* q_match_rec */
#include "qlang/q_registry.h"       /* q_registry_list_value */
#include "qlang/ops/q_index.h"      /* q_index_elem_at */
#include "table/sym.h"              /* ray_read_sym */

typedef enum { PAT_NONE, PAT_NULL, PAT_NAME, PAT_CONST, PAT_LIST, PAT_TYPED } pat_kind_t;

int64_t q_pattern_type_num(int64_t sym) {
    ray_t* s = ray_sym_str(sym);
    char c = s && ray_str_len(s) == 1 ? ray_str_ptr(s)[0] : 0;
    ray_release(s);
    int list = c >= 'A' && c <= 'Z';
    int8_t tag = q_type_of_char(list ? (char)(c - 'A' + 'a') : c);
    return list ? tag : -tag;
}

int q_pattern_type_ok(ray_t* v, int64_t tnum) {
    return q_builtins_type_num(v) == (int8_t)tnum;
}

int64_t q_pattern_type_check(ray_t* x) {
    int one = x && x->type == RAY_SYM && q_count(x) == 1;
    return one ? q_pattern_type_num(ray_read_sym(ray_data(x), 0, RAY_SYM, x->attrs)) : 0;
}

/* The parser's target trees: an elided item is the hole, a name a sym atom, a list pattern wears the list
 * constructor, `p:`c` is (:;p;,`c) and `:`c` is (::;,`c); every other leaf the evaluator reads as itself is a
 * constant — so not a function, and not a bare symbol list, which is an application. */
static pat_kind_t pat_kind(ray_t* p, int64_t* tnum, ray_t** sub) {
    if (!p) return PAT_NONE;
    if (Q_IS_HOLE(p)) return PAT_NULL;
    if (p->type == -RAY_SYM) return PAT_NAME;
    if (p->type == RAY_SYM) return q_count(p) == 1 ? PAT_CONST : PAT_NONE;
    if (p->type != RAY_LIST) return q_eval_apply_is_fn(p) ? PAT_NONE : PAT_CONST;
    int64_t n = q_count(p);
    ray_t** e = (ray_t**)ray_data(p);
    if (n == 0) return PAT_CONST;
    if (!e[0]) return PAT_NONE;
    if (n == 1) return PAT_CONST;
    if (e[0] == q_registry_list_value()) return PAT_LIST;
    if (n == 3 && q_parse_assign_head(e[0], NULL, NULL) == Q_HEAD_ASSIGN) {
        *sub = e[1];
        *tnum = q_pattern_type_check(e[2]);
    } else if (n == 2 && Q_IS_GENERIC_NULL(e[0])) {
        *sub = Q_HOLE_OBJ;
        *tnum = q_pattern_type_check(e[1]);
    } else {
        return PAT_NONE;
    }
    return *tnum ? PAT_TYPED : PAT_NONE;
}

int q_pattern_supported(ray_t* pat) {
    int64_t tnum = 0;
    ray_t* sub = NULL;
    pat_kind_t k = pat_kind(pat, &tnum, &sub);
    if (k == PAT_TYPED) return q_pattern_supported(sub);
    if (k != PAT_LIST) return k != PAT_NONE;
    ray_t** e = (ray_t**)ray_data(pat);
    for (int64_t i = 1, n = q_count(pat); i < n; i++)
        if (!q_pattern_supported(e[i])) return 0;
    return 1;
}

void q_pattern_names(ray_t* pat, q_pattern_name_fn visit, void* ctx) {
    int64_t tnum = 0;
    ray_t* sub = NULL;
    pat_kind_t k = pat_kind(pat, &tnum, &sub);
    if (k == PAT_NAME) visit(pat->i64, ctx);
    if (k == PAT_TYPED) q_pattern_names(sub, visit, ctx);
    if (k != PAT_LIST) return;
    ray_t** e = (ray_t**)ray_data(pat);
    for (int64_t i = 1, n = q_count(pat); i < n; i++)
        q_pattern_names(e[i], visit, ctx);
}

static ray_t* pat_bind(ray_t** binds, ray_t* x) {
    ray_t* b = ray_list_append(*binds, x);
    if (!b || RAY_IS_ERR(b)) return b ? b : q_err(QE_OOM);
    *binds = b;
    return NULL;
}

ray_t* q_pattern_collect(ray_t* pat, ray_t* v, ray_t** binds) {
    int64_t tnum = 0;
    ray_t* sub = NULL;
    switch (pat_kind(pat, &tnum, &sub)) {
    case PAT_NONE: return q_err(QE_NYI);
    case PAT_NULL: return NULL;
    case PAT_NAME: {
        ray_t* err = pat_bind(binds, pat);
        return err ? err : pat_bind(binds, v);
    }
    case PAT_CONST: {
        ray_t* c = q_eval(pat);
        if (!c || RAY_IS_ERR(c)) return c ? c : q_err(QE_OOM);
        int same = q_match_rec(c, v);
        ray_release(c);
        return same ? NULL : q_err(QE_MATCH);
    }
    case PAT_TYPED:
        return q_pattern_type_ok(v, tnum) ? q_pattern_collect(sub, v, binds) : q_err(QE_TYPE);
    case PAT_LIST: break;
    }
    /* a table is a list of its rows (basics/pattern.md "List"); an atom or a dictionary has no items to pair */
    if (!q_type_is_iter(v)) return q_err(QE_TYPE);
    int64_t n = q_count(pat) - 1;
    if (q_count(v) != n) return q_err(QE_LENGTH);
    ray_t** e = (ray_t**)ray_data(pat);
    for (int64_t i = 0; i < n; i++) {
        ray_t* item = q_index_elem_at(v, i);
        if (!item || RAY_IS_ERR(item)) return item ? item : q_err(QE_OOM);
        ray_t* err = q_pattern_collect(e[i + 1], item, binds);
        ray_release(item);
        if (err) return err;
    }
    return NULL;
}
