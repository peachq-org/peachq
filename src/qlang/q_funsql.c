/* q_funsql.c — the functional qSQL executor (?[t;c;b;a] / ![t;c;b;a]) and the
 * string select/exec/delete statement executors. Slated for retirement:
 * eval-unification stage 3 deletes this file once its seam loses its last
 * caller.  The WHOLE exported seam is the five declarations in
 * q_registry_internal.h block 1 (q_select_exec, q_funsql_select,
 * q_funsql_bang, q_delete_exec, q_exec_exec) — keep it from growing.
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_ns.h"    /* q_ns_is_context — context tables excluded from `tables` */
#include "qlang/q_apply.h" /* q_apply_noun — applying column/aggregate values */
#include "qlang/q_deriv.h" /* q_deriv_kind_of/base — lambda/projection aggregates */
#include "lang/env.h"      /* ray_env_get/set, ray_fn_name */
#include "lang/eval.h"     /* ray_eval; ray_and_fn/ray_not_fn/ray_take_fn */
#include "lang/internal.h" /* ray_where_fn, ray_group_fn, call_fn1/2, ray_typed_null, ray_error */
#include "ops/ops.h"       /* ray_sort_indices — order-by */
#include "table/sym.h"     /* ray_sym_vec_cell, ray_sym_intern_runtime — column names */
#include <stdint.h>        /* uintptr_t */
#include <stdio.h>         /* snprintf */
#include <string.h>
#include <stdlib.h>

static void q_select_rename_temps(ray_t* tbl, ray_t* tempnames, ray_t* realnames) {
    if (!tbl || tbl->type != RAY_TABLE ||
        !tempnames || tempnames->type != RAY_SYM ||
        !realnames || realnames->type != RAY_SYM)
        return;
    int64_t nt = ray_len(tempnames);
    int64_t nc = ray_table_ncols(tbl);
    int64_t used_stack[64];
    int64_t* used = (nc <= 64) ? used_stack : (int64_t*)malloc((size_t)nc * sizeof(int64_t));
    if (!used) return;
    for (int64_t c = 0; c < nc; c++) used[c] = ray_table_col_name(tbl, c);
    for (int64_t i = 0; i < nt; i++) {
        ray_t* ti = ray_i64(i);
        ray_t* ta = ray_at_fn(tempnames, ti);
        ray_t* ra = ray_at_fn(realnames, ti);
        ray_release(ti);
        int64_t tid = (ta && ta->type == -RAY_SYM) ? ta->i64 : -1;
        int64_t rid = (ra && ra->type == -RAY_SYM) ? ra->i64 : -1;
        if (ta) ray_release(ta);
        if (ra) ray_release(ra);
        if (tid < 0 || rid < 0) continue;
        for (int64_t c = 0; c < nc; c++) {
            if (ray_table_col_name(tbl, c) != tid) continue;
            int64_t nm = q_name_dedup(rid, used, c, 1);
            used[c] = nm;
            ray_table_set_col_name(tbl, c, nm);
            break;
        }
    }
    if (used != used_stack) free(used);
}

/* q qSQL SELECT adapter — lowers the functional 5-list (?;`t;c;b;a) (emitted by
 * BOTH the string form `select…from t` AND `?[t;c;b;a]`) onto the base
 * ray_select engine.  q_lower hands it a fully-built rayfall query dict plus the
 * by-group key column names; this special form calls ray_select and, for a
 * by-group query, splits the flat result into a KEYED table — a dict from the
 * key-columns table to the value-columns table (the mandate: "a keyed table is
 * just a dictionary from one table to another"), which q_fmt renders `k| v`.
 *   args[0] = query dict (unevaluated — ray_select owns clause-in-scope eval)
 *   args[1] = by-key column-name sym vector (empty => unkeyed passthrough) */
static ray_t* funsql_sort_keyed(ray_t* dict);   /* fwd: by-key ascending sort (consumes dict) */

ray_t* q_select_exec(ray_t** args, int64_t n) {
    ray_t* dict = args[0];
    ray_t* res  = ray_select(&dict, 1);
    if (!res || RAY_IS_ERR(res)) return res;

    ray_t* keys = (n >= 2) ? args[1] : NULL;
    int64_t nk = (keys && keys->type == RAY_SYM) ? ray_len(keys) : 0;
    if (nk == 0 || res->type != RAY_TABLE) {
        if (n >= 4) q_select_rename_temps(res, args[2], args[3]);
        return res;
    }

    int64_t kids[64];
    if (nk > 64) nk = 64;
    for (int64_t k = 0; k < nk; k++) {
        ray_t* ia = ray_i64(k);
        ray_t* ka = ray_at_fn(keys, ia);
        ray_release(ia);
        kids[k] = (ka && ka->type == -RAY_SYM) ? ka->i64 : -1;
        if (ka) ray_release(ka);
    }

    int64_t nc = ray_table_ncols(res);
    ray_t* kt = ray_table_new(nk);
    ray_t* vt = ray_table_new(nc - nk > 0 ? nc - nk : 1);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(kt) && !RAY_IS_ERR(vt); c++) {
        int64_t nm  = ray_table_col_name(res, c);
        ray_t*  col = ray_table_get_col_idx(res, c);
        int iskey = 0;
        for (int64_t k = 0; k < nk; k++) if (kids[k] == nm) { iskey = 1; break; }
        if (iskey) kt = ray_table_add_col(kt, nm, col);
        else       vt = ray_table_add_col(vt, nm, col);
    }
    ray_release(res);
    if (RAY_IS_ERR(kt)) { ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    if (n >= 4) q_select_rename_temps(vt, args[2], args[3]);
    ray_t* keyed = ray_dict_new(kt, vt);   /* consumes kt, vt */
    return funsql_sort_keyed(keyed);       /* kdb `by`: groups ascending by key (consumes) */
}

/* ===== functional qSQL executor (piece 3) =================================
 * Runtime evaluators for the by-VALUE functional forms `?[t;c;b;a]` (select)
 * and `![t;c;b;a]` (update/delete).  q_lower embeds g_funsql_select_value /
 * g_funsql_bang_value at the head of a rank-4 `?`/`!` application; ray_eval
 * evaluates the four operands (t→table, c/b/a→list/dict/sym VALUES) and calls
 * the executor below.  Reuse-first: WHERE is evaluated against the live table
 * columns (handles `in`, `=`, comparisons uniformly, incl. the enlist-a-symbol
 * -constant convention); BY + aggregates + column projection are delegated to
 * the base engine (q_select_exec → ray_select; ray_update for update-by).  See
 * qdocs/.../basics/funsql.md for the observable contract. */

/* Append one interned symbol into a RAY_SYM (W64) vector — local twin of the
 * q_parse helper (that one is file-static there). */
static ray_t* rsymvec_append(ray_t* vec, const char* s, int len) {
    int64_t id = ray_sym_intern_runtime(s, (size_t)len);
    return ray_vec_append(vec, &id);
}

/* A verb usable at a constraint/expression head: a plain fn value, OR a 104h
 * derived-verb carrier (glyph verbs like `>` `*` lower to a projection carrier
 * in non-head list positions — see q_deriv.h / ql_deriv_value). */
/* Multi-column ascending grade (permutation) — defined in src/ops/sort.c; no
 * public header exposes it, so forward-declare for the by-group key sort. */
ray_t* ray_sort_indices(ray_t** cols, uint8_t* descs, uint8_t* nulls_first,
                        uint8_t n_cols, int64_t nrows);

static int funsql_is_fn(const ray_t* x) {
    if (!x) return 0;
    if (x->type == RAY_UNARY || x->type == RAY_BINARY || x->type == RAY_VARY) return 1;
    if (x->type == RAY_LAMBDA) return 1;   /* user lambda / named fn in a phrase head */
    if (x->type == RAY_LIST && q_deriv_kind_of(x) != Q_DERIV_NONE) return 1;
    return 0;
}

/* The rayfall routing name of a verb head (plain value or carrier), or NULL. */
static const char* funsql_head_name(ray_t* fn) {
    if (fn->type == RAY_LIST && q_deriv_kind_of(fn) != Q_DERIV_NONE) {
        ray_t* base = q_deriv_base(fn);
        return base ? ray_fn_name(base) : NULL;
    }
    return ray_fn_name(fn);
}

/* Call a verb head on already-evaluated args (no re-eval, so a resolved
 * -RAY_SYM constant is NOT looked up as a variable).  Routes through the same
 * call_fn1/call_fn2 dispatch the evaluator uses — this applies the ATOMIC
 * broadcast (a scalar `>`/`=` RHS against a column vector) and lazy
 * materialisation that a raw kernel-pointer call would skip.  Carriers dispatch
 * through the shared q applicator. */
static ray_t* funsql_call(ray_t* fn, ray_t** a, int64_t n) {
    if (fn->type == RAY_LIST && q_deriv_kind_of(fn) != Q_DERIV_NONE)
        return q_apply_noun(fn, a, n);
    if (n == 1) return call_fn1(fn, a[0]);
    if (n == 2) return call_fn2(fn, a[0], a[1]);
    if (fn->type == RAY_VARY) return ((ray_vary_fn)(uintptr_t)fn->i64)(a, n);
    return ray_error("rank", "funsql: verb applied to %lld args", (long long)n);
}

/* Is x an empty operand — the general null `::`, C-NULL, or a 0-length vec/list?
 * The functional degenerate forms use `()` (→ `::`) for "no constraints" / "all
 * columns", and `0b` for "no grouping". */
static int funsql_empty(const ray_t* x) {
    if (!x) return 1;
    if (x->type == RAY_NULL) return 1;
    if (x->type == RAY_LIST || ray_is_vec(x)) return ray_len((ray_t*)x) == 0;
    return 0;
}

/* Evaluate a where-constraint parse-tree (a VALUE) against the table columns.
 *   -RAY_SYM atom        -> the named column (kdb: a bare symbol IS a column ref)
 *   (fn; arg; …)         -> resolve each arg, apply fn (yields a boolean vector)
 *   enlist X (1-list/vec)-> the symbol CONSTANT X (un-enlisted)
 *   literal              -> itself
 * Returns an OWNED value (or owned error). */
static ray_t* funsql_eval(ray_t* x, ray_t* tbl) {
    if (!x) return ray_error("domain", "funsql: null expression");
    if (x->type == -RAY_SYM) {
        ray_t* col = ray_table_get_col(tbl, x->i64);
        if (col) { ray_retain(col); return col; }
        ray_retain(x); return x;                  /* names no column -> literal sym */
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (n >= 1 && funsql_is_fn(e[0])) {
            int64_t na = n - 1;
            if (na < 1 || na > 7)
                return ray_error("rank", "funsql: constraint arity %lld", (long long)na);
            ray_t* av[7];
            for (int64_t i = 0; i < na; i++) {
                av[i] = funsql_eval(e[i + 1], tbl);
                if (!av[i] || RAY_IS_ERR(av[i])) {
                    for (int64_t j = 0; j < i; j++) ray_release(av[j]);
                    return av[i] ? av[i] : ray_error("domain", "funsql: constraint arg");
                }
            }
            ray_t* r = funsql_call(e[0], av, na);
            for (int64_t i = 0; i < na; i++) ray_release(av[i]);
            return r;
        }
        if (n == 1) { ray_retain(e[0]); return e[0]; }   /* enlist X -> X */
        ray_retain(x); return x;
    }
    if (x->type == RAY_SYM && ray_len(x) == 1) {         /* enlist `sym -> `sym */
        ray_t* i0 = ray_i64(0);
        ray_t* r = ray_at_fn(x, i0);
        ray_release(i0);
        return r;
    }
    ray_retain(x); return x;
}

/* Combine the where-constraints into one boolean mask (left-to-right AND), or
 * NULL when there are no constraints.  `c` is a list of constraints; a single
 * unwrapped constraint (function head) is accepted too. */
static ray_t* funsql_build_mask(ray_t* tbl, ray_t* c) {
    if (funsql_empty(c)) return NULL;
    ray_t* one[1];
    ray_t** cons; int64_t nc;
    if (c->type == RAY_LIST) {
        ray_t** e = (ray_t**)ray_data(c);
        int64_t n = ray_len(c);
        if (n >= 1 && funsql_is_fn(e[0])) { one[0] = c; cons = one; nc = 1; }
        else { cons = e; nc = n; }
    } else { one[0] = c; cons = one; nc = 1; }
    ray_t* mask = NULL;
    for (int64_t i = 0; i < nc; i++) {
        ray_t* m = funsql_eval(cons[i], tbl);
        if (!m || RAY_IS_ERR(m)) { if (mask) ray_release(mask); return m ? m : ray_error("domain", "funsql where"); }
        if (!mask) mask = m;
        else {
            ray_t* a = ray_and_fn(mask, m);
            ray_release(mask); ray_release(m);
            mask = a;
            if (!mask || RAY_IS_ERR(mask)) return mask ? mask : ray_error("oom", NULL);
        }
    }
    return mask;
}

/* Filter table rows by the where-constraints.  keep=1 selects matching rows
 * (select/exec/update); keep=0 selects the complement (delete rows).  Returns
 * a retained `tbl` unchanged when there are no constraints. */
static ray_t* funsql_filter(ray_t* tbl, ray_t* c, int keep) {
    ray_t* mask = funsql_build_mask(tbl, c);
    if (!mask) { ray_retain(tbl); return tbl; }
    if (RAY_IS_ERR(mask)) return mask;
    ray_t* use = mask;
    if (!keep) {
        use = ray_not_fn(mask);
        ray_release(mask);
        if (!use || RAY_IS_ERR(use)) return use;
    }
    ray_t* idx = ray_where_fn(use);
    ray_release(use);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("domain", "funsql where");
    ray_t* ft = ray_at_fn(tbl, idx);
    ray_release(idx);
    return ft;
}

/* Lower a by/select expression VALUE into the AST shape the base query engine
 * consumes: a function-VALUE head becomes a bare -RAY_SYM (ray_select /
 * ray_update key their agg/predicate dispatch on the rayfall routing name), a
 * column-ref symbol becomes a bare name-ref, an enlisted constant is passed
 * through.  Returns a fresh OWNED tree. */
static ray_t* funsql_lower_expr(ray_t* x) {
    if (!x) return NULL;
    if (x->type == -RAY_SYM) return ray_sym(x->i64);        /* bare name-ref */
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (!(n >= 1 && funsql_is_fn(e[0]))) { ray_retain(x); return x; }  /* constant */
        ray_t* out = ray_list_new(n);
        for (int64_t i = 0; i < n; i++) {
            ray_t* c;
            if (i == 0) {
                /* Stage-1 general eval (2026-07-11): convert the head to a
                 * name sym ONLY when the base env can resolve that name (the
                 * engine's name-dispatch fast paths).  An unresolvable head
                 * (q wrapper like sums/prds, spelling-less value) keeps the
                 * VALUE — ray_select's general-eval recognizer routes it.  A
                 * lambda/derived-verb CARRIER (a runtime LIST value) or bare
                 * RAY_LAMBDA is embedded as (quote <carrier>): ray_eval's
                 * head-eval returns it intact and the noun-head arm applies
                 * it through the q apply hook. */
                const char* nm = funsql_head_name(e[0]);
                int64_t id = (nm && nm[0])
                           ? ray_sym_intern_runtime(nm, strlen(nm)) : -1;
                if (id >= 0 && ray_env_get(id)) {
                    c = ray_sym(id);
                } else if (e[0]->type == RAY_UNARY || e[0]->type == RAY_BINARY ||
                           e[0]->type == RAY_VARY) {
                    ray_retain(e[0]); c = e[0];    /* value head: eval-safe */
                } else {                            /* carrier / RAY_LAMBDA */
                    ray_t* qs = ray_sym(ray_sym_intern_runtime("quote", 5));
                    ray_t* wrap = ray_list_new(2);
                    wrap = ray_list_append(wrap, qs);   ray_release(qs);
                    wrap = ray_list_append(wrap, e[0]); /* retains carrier */
                    c = wrap;
                }
            } else {
                c = funsql_lower_expr(e[i]);
            }
            if (!c || RAY_IS_ERR(c)) { ray_release(out); return c ? c : ray_error("domain", "funsql expr"); }
            out = ray_list_append(out, c);
            ray_release(c);
        }
        return out;
    }
    ray_retain(x); return x;
}

/* Resolve the `t` operand to a live table (a bare symbol names a table var). */
static ray_t* funsql_resolve_table(ray_t* t) {
    ray_t* tbl = t;
    if (t && t->type == -RAY_SYM) {
        tbl = ray_env_get(t->i64);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", "funsql: table symbol is unbound");
    }
    if (!tbl || tbl->type != RAY_TABLE)
        return ray_error("type", "funsql: `t` must be a table");
    ray_retain(tbl);
    return tbl;
}

/* Append a `by` clause + its key-column names for a `name!col` By-DICT (the
 * only By-phrase Select supports here — symbol By-phrases are grouped-exec and
 * refused upstream).  Values may be a SYM vector (plain column names) or a
 * general list (parse-tree expressions).  Fills the base-query by-dict into
 * (*keyvec,*vallist) and records the key names into *keycols. */
static void funsql_add_by(ray_t* b, ray_t** keyvec, ray_t** vallist, ray_t** keycols) {
    if (!b || b->type != RAY_DICT) return;
    ray_t* bk = ray_dict_keys(b);
    ray_t* bv = ray_dict_vals(b);
    int64_t nb = ray_len(bk);
    int bv_sym = (bv && bv->type == RAY_SYM);
    ray_t* bkeys = ray_sym_vec_new(RAY_SYM_W64, nb);
    ray_t* bvals = ray_list_new(nb);
    for (int64_t i = 0; i < nb; i++) {
        ray_t* kn = ray_sym_vec_cell(bk, i);
        bkeys    = rsymvec_append(bkeys, ray_str_ptr(kn), (int)ray_str_len(kn));
        *keycols = rsymvec_append(*keycols, ray_str_ptr(kn), (int)ray_str_len(kn));
        ray_t* ex;
        if (bv_sym) {
            ray_t* vn = ray_sym_vec_cell(bv, i);
            ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
        } else {
            ex = funsql_lower_expr(((ray_t**)ray_data(bv))[i]);
        }
        bvals = ray_list_append(bvals, ex);
        ray_release(ex);
    }
    ray_t* bydict = ray_dict_new(bkeys, bvals);
    *keyvec  = rsymvec_append(*keyvec, "by", 2);
    *vallist = ray_list_append(*vallist, bydict);
    ray_release(bydict);
}

/* Evaluate one functional operand.  These are SPECIAL FORMs (operands arrive
 * unevaluated) so the empty-list marker `()` — which the parser lowers to the
 * `::` name-ref and which ray_eval would reject as an unbound name — maps to
 * the generic null here; everything else (name-refs, list/dict literals, `0b`,
 * `` `sym$() ``) evaluates normally. */
static ray_t* funsql_operand(ray_t* x) {
    if (!x) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        int is_null = s && ray_str_len(s) == 2 && memcmp(ray_str_ptr(s), "::", 2) == 0;
        if (s) ray_release(s);
        if (is_null) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    }
    return ray_eval(x);
}

/* Sort a by-group keyed table ascending by its key column(s) — kdb's `by`
 * orders groups by key, whereas the base group-by emits in first-appearance
 * order.  `dict` is a keyed table (RAY_DICT whose keys/vals are tables).
 * Consumes `dict`; returns the sorted keyed table (or `dict` unchanged on any
 * non-keyed shape / sort failure). */
static ray_t* funsql_sort_keyed(ray_t* dict) {
    if (!dict || dict->type != RAY_DICT) return dict;
    ray_t* kt = ray_dict_keys(dict);
    ray_t* vt = ray_dict_vals(dict);
    if (!kt || kt->type != RAY_TABLE || !vt || vt->type != RAY_TABLE) return dict;
    int64_t nrows = ray_table_nrows(kt);
    int64_t nk = ray_table_ncols(kt);
    if (nrows <= 1 || nk < 1 || nk > 16) return dict;
    ray_t* cols[16];
    uint8_t descs[16] = {0};
    for (int64_t j = 0; j < nk; j++) cols[j] = ray_table_get_col_idx(kt, j);
    ray_t* perm = ray_sort_indices(cols, descs, NULL, (uint8_t)nk, nrows);
    if (!perm || RAY_IS_ERR(perm)) { if (perm) ray_release(perm); return dict; }
    ray_t* kt2 = ray_at_fn(kt, perm);
    ray_t* vt2 = ray_at_fn(vt, perm);
    ray_release(perm);
    if (!kt2 || RAY_IS_ERR(kt2) || !vt2 || RAY_IS_ERR(vt2)) {
        if (kt2) ray_release(kt2);
        if (vt2) ray_release(vt2);
        return dict;
    }
    ray_t* nd = ray_dict_new(kt2, vt2);   /* consumes kt2, vt2 */
    if (!nd || RAY_IS_ERR(nd)) return dict;
    ray_release(dict);
    return nd;
}

/* Grouped-EXEC helpers: a bare/vector By-symbol groups exactly like a Select
 * name!col by-dict, so build that Select query and reshape the keyed table into
 * exec's group->value dictionary (funsql.md "By phrase"). */

/* Build a name!col by-DICT from a bare or vector By-symbol (values a SYM vec so
 * funsql_add_by emits bare column-refs). */
static ray_t* funsql_sym_to_bydict(ray_t* b) {
    int64_t cnt = (b->type == -RAY_SYM) ? 1 : ray_len(b);
    int64_t cap = cnt > 0 ? cnt : 1;
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, cap);
    for (int64_t i = 0; i < cnt; i++) {
        ray_t* s = (b->type == -RAY_SYM) ? ray_sym_str(b->i64) : ray_sym_vec_cell(b, i);
        if (s) {
            k = rsymvec_append(k, ray_str_ptr(s), (int)ray_str_len(s));
            v = rsymvec_append(v, ray_str_ptr(s), (int)ray_str_len(s));
            if (b->type == -RAY_SYM) ray_release(s);   /* ray_sym_str owns; cell borrows */
        }
    }
    return ray_dict_new(k, v);   /* consumes k, v */
}

/* Wrap a single unnamed select-phrase (`a` = bare col -RAY_SYM or fn expr) into
 * a 1-entry name!expr out-dict so the shared has_out path materialises it. */
static ray_t* funsql_exec_out_wrap(ray_t* a) {
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, 1);
    k = rsymvec_append(k, "Qexec0", 6);
    ray_t* v;
    if (a->type == -RAY_SYM) {
        v = ray_sym_vec_new(RAY_SYM_W64, 1);
        ray_t* s = ray_sym_str(a->i64);
        if (s) { v = rsymvec_append(v, ray_str_ptr(s), (int)ray_str_len(s)); ray_release(s); }
    } else {
        v = ray_list_new(1);
        ray_retain(a);
        v = ray_list_append(v, a);
        ray_release(a);
    }
    return ray_dict_new(k, v);   /* consumes k, v */
}

/* Reshape a sorted keyed table (RAY_DICT{keytab,valtab}) into exec's group->value
 * dict.  keys = the single group column (a list) or the key table (multi col);
 * vals = the whole value table (named phrase -> list!table) or its single column
 * (unnamed phrase -> group->aggregate/collected list).  Consumes `res`. */
static ray_t* funsql_exec_reshape(ray_t* res, int named) {
    if (!res || res->type != RAY_DICT) return res;   /* error / non-keyed passthrough */
    ray_t* kt = ray_dict_keys(res);
    ray_t* vt = ray_dict_vals(res);
    if (!kt || kt->type != RAY_TABLE || !vt || vt->type != RAY_TABLE) return res;
    ray_t* keys = (ray_table_ncols(kt) == 1) ? ray_table_get_col_idx(kt, 0) : kt;
    ray_t* vals = named ? vt : ray_table_get_col_idx(vt, 0);
    if (!keys || !vals) return res;
    ray_retain(keys);
    ray_retain(vals);
    ray_release(res);
    return ray_dict_new(keys, vals);   /* consumes keys, vals */
}

/* Is symbol id `q` one of the grouping columns named by By-symbol `b`? */
static int funsql_sym_is_key(ray_t* b, int64_t q) {
    int64_t cnt = (b->type == -RAY_SYM) ? 1 : ray_len(b);
    for (int64_t i = 0; i < cnt; i++) {
        ray_t* s = (b->type == -RAY_SYM) ? ray_sym_str(b->i64) : ray_sym_vec_cell(b, i);
        if (!s) continue;
        int64_t id = ray_sym_intern_runtime(ray_str_ptr(s), ray_str_len(s));
        if (b->type == -RAY_SYM) ray_release(s);   /* ray_sym_str owns; cell borrows */
        if (id == q) return 1;
    }
    return 0;
}

/* Grouped exec whose select-phrase BARE-references a grouping column (`exec g by g`,
 * `exec v:g by g`) hits the base engine's key/value name collision (the string
 * select path wraps such refs in `reverse reverse`; the funsql path does not).
 * Detect that case so the caller can reject it cleanly rather than emit wrong
 * values.  Only a bare col-ref output collides — an aggregate over a key column
 * (`exec first g by g`) is a computed column and is fine. */
static int funsql_exec_key_collision(ray_t* b, ray_t* a) {
    if (!a) return 0;
    if (a->type == -RAY_SYM) return funsql_sym_is_key(b, a->i64);
    if (a->type == RAY_DICT) {
        ray_t* av = ray_dict_vals(a);
        if (!av) return 0;
        if (av->type == RAY_SYM) {
            int64_t na = ray_len(av);
            for (int64_t i = 0; i < na; i++) {
                ray_t* vn = ray_sym_vec_cell(av, i);
                if (vn && funsql_sym_is_key(b, ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)))) return 1;
            }
        } else if (av->type == RAY_LIST) {
            int64_t na = ray_len(av);
            ray_t** e = (ray_t**)ray_data(av);
            for (int64_t i = 0; i < na; i++)
                if (e[i] && e[i]->type == -RAY_SYM && funsql_sym_is_key(b, e[i]->i64)) return 1;
        }
    }
    return 0;
}

/* `?[t;c;b;a]` select — returns a table (or a keyed table for a by-group). */
static ray_t* q_funsql_select_impl(ray_t* t, ray_t* c, ray_t* b, ray_t* a) {
    ray_t* tbl = funsql_resolve_table(t);
    if (RAY_IS_ERR(tbl)) return tbl;

    ray_t* ft = funsql_filter(tbl, c, 1);
    ray_release(tbl);
    if (!ft || RAY_IS_ERR(ft)) return ft;

    /* EXEC forms — signalled by the By-phrase being EMPTY: the general empty
     * list `()` OR `::` (both are `funsql_empty`; `()` now parses to the empty
     * list, not `::`), as opposed to `0b` (RAY_BOOL) which is Select's "no
     * grouping".  Result is a vector (a is a column/parse-tree), a dictionary
     * (a is a dict), or the last row as a dictionary (a is `()`).  See
     * funsql.md "No grouping". */
    if (funsql_empty(b)) {
        if (a && (a->type == -RAY_SYM ||
                  (a->type == RAY_LIST && ray_len(a) > 0 &&        /* guard `()` (empty list): no head to read */
                   funsql_is_fn(((ray_t**)ray_data(a))[0])))) {
            ray_t* r = funsql_eval(a, ft);          /* exec col / parse-tree -> vector/atom */
            ray_release(ft);
            return r;
        }
        if (a && a->type == RAY_DICT) {             /* exec -> dict {name: eval} */
            ray_t* ak = ray_dict_keys(a);
            ray_t* av = ray_dict_vals(a);
            int64_t na = ray_len(ak);
            int av_sym = (av && av->type == RAY_SYM);
            ray_t* dk = ray_sym_vec_new(RAY_SYM_W64, na > 0 ? na : 1);
            ray_t* dv = ray_list_new(na > 0 ? na : 1);
            for (int64_t i = 0; i < na; i++) {
                ray_t* nm = ray_sym_vec_cell(ak, i);
                dk = rsymvec_append(dk, ray_str_ptr(nm), (int)ray_str_len(nm));
                ray_t* built = NULL, *expr;
                if (av_sym) {
                    ray_t* vn = ray_sym_vec_cell(av, i);
                    built = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
                    expr = built;
                } else {
                    expr = ((ray_t**)ray_data(av))[i];
                }
                ray_t* val = funsql_eval(expr, ft);
                if (built) ray_release(built);
                if (!val || RAY_IS_ERR(val)) {
                    ray_release(dk); ray_release(dv); ray_release(ft);
                    return val ? val : ray_error("domain", "exec");
                }
                dv = ray_list_append(dv, val);
                ray_release(val);
            }
            ray_release(ft);
            return ray_dict_new(dk, dv);
        }
        if (funsql_empty(a)) {                       /* exec last row -> dict */
            int64_t nr = ray_table_nrows(ft);
            ray_t* iv = ray_i64(nr > 0 ? nr - 1 : 0);
            ray_t* r = ray_at_fn(ft, iv);
            ray_release(iv);
            ray_release(ft);
            return r;
        }
    }

    /* A symbol / symbol-vector By-phrase is grouped EXEC: the base engine groups
     * on it exactly like a Select name!col by-dict, so build that Select query
     * (b -> name!col dict; a single unnamed phrase wrapped as an out-dict) and
     * reshape the keyed table into exec's group->value dict below. */
    ray_t* gx_by = NULL, *gx_a = NULL;
    int gx = 0, gx_named = 0;
    if (b && (b->type == -RAY_SYM || b->type == RAY_SYM)) {
        if (funsql_empty(a)) {                     /* `exec by g` (all cols) — not built */
            ray_release(ft);
            return ray_error("nyi", "?: by-group exec without a select-phrase");
        }
        if (funsql_exec_key_collision(b, a)) {     /* bare key-col output — needs the
                                                    * reverse-reverse identity wrap the
                                                    * string path has; deferred (would be
                                                    * a wrong answer otherwise). */
            ray_release(ft);
            return ray_error("nyi", "?: grouped exec of a grouping column is not supported");
        }
        gx = 1;
        gx_by = funsql_sym_to_bydict(b);
        if (!gx_by || RAY_IS_ERR(gx_by)) { ray_release(ft); return gx_by ? gx_by : ray_error("oom", NULL); }
        b = gx_by;
        if (a->type == RAY_DICT) { gx_named = 1; }
        else {
            gx_a = funsql_exec_out_wrap(a);
            if (!gx_a || RAY_IS_ERR(gx_a)) { ray_release(ft); ray_release(gx_by); return gx_a ? gx_a : ray_error("oom", NULL); }
            a = gx_a; gx_named = 0;
        }
    }

    int has_by  = b && b->type == RAY_DICT;
    int has_out = a && a->type == RAY_DICT;

    /* select from t [where] — no grouping, all columns */
    if (!has_by && !has_out) return ft;

    /* by-group with an implicit column set (a is `()`) is the keyed
     * all-columns form — deferred (not CORE).  Refuse cleanly rather than build
     * a zero-output query (which trips the base null-invariant check). */
    if (has_by && !has_out) {
        ray_release(ft);
        return ray_error("nyi", "?: by-group without an aggregate dict is not supported");
    }

    ray_t* keyvec   = ray_sym_vec_new(RAY_SYM_W64, 4);
    ray_t* vallist  = ray_list_new(4);
    ray_t* keycols  = ray_sym_vec_new(RAY_SYM_W64, 1);
    ray_t* tempnames = ray_sym_vec_new(RAY_SYM_W64, 1);
    ray_t* outnames  = ray_sym_vec_new(RAY_SYM_W64, 1);

    if (has_by) funsql_add_by(b, &keyvec, &vallist, &keycols);

    if (has_out) {
        ray_t* ak = ray_dict_keys(a);
        ray_t* av = ray_dict_vals(a);
        int64_t na = ray_len(ak);
        int av_sym = (av && av->type == RAY_SYM);
        for (int64_t i = 0; i < na; i++) {
            char tmp[24];
            int tl = snprintf(tmp, sizeof tmp, "Qqc%lld", (long long)i);
            keyvec    = rsymvec_append(keyvec, tmp, tl);
            tempnames = rsymvec_append(tempnames, tmp, tl);
            ray_t* nm = ray_sym_vec_cell(ak, i);
            outnames  = rsymvec_append(outnames, ray_str_ptr(nm), (int)ray_str_len(nm));
            ray_t* ex;
            if (av_sym) {
                ray_t* vn = ray_sym_vec_cell(av, i);
                ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
            } else {
                ex = funsql_lower_expr(((ray_t**)ray_data(av))[i]);
            }
            vallist = ray_list_append(vallist, ex);
            ray_release(ex);
        }
    }

    /* from: the (already-filtered) table VALUE — appended LAST (see ql_qsql). */
    keyvec  = rsymvec_append(keyvec, "from", 4);
    vallist = ray_list_append(vallist, ft);
    ray_release(ft);

    ray_t* dict = ray_dict_new(keyvec, vallist);   /* consumes keyvec, vallist */
    if (!dict || RAY_IS_ERR(dict)) {
        ray_release(keycols); ray_release(tempnames); ray_release(outnames);
        return dict ? dict : ray_error("oom", NULL);
    }
    ray_t* sargs[4] = { dict, keycols, tempnames, outnames };
    ray_t* res = q_select_exec(sargs, 4);   /* sorts the keyed result ascending by key */
    ray_release(dict); ray_release(keycols); ray_release(tempnames); ray_release(outnames);
    if (gx) res = funsql_exec_reshape(res, gx_named);   /* keyed table -> exec dict */
    if (gx_by) ray_release(gx_by);
    if (gx_a) ray_release(gx_a);
    return res;
}

/* Generic scatter: overlay `upd` (length M, aligned to the M rows named by the
 * i64 index vector `idx`) onto `base` (length N), leaving unindexed rows.  Used
 * to write a grouped-update result back onto the original table (where+by
 * update, which the base ray_update refuses to compose in one pass). */
static ray_t* funsql_scatter(ray_t* base, ray_t* idx, ray_t* upd) {
    int64_t N = ray_len(base);
    int64_t M = ray_len(idx);
    int64_t* pos = (int64_t*)malloc((N > 0 ? N : 1) * sizeof(int64_t));
    if (!pos) return ray_error("oom", NULL);
    for (int64_t r = 0; r < N; r++) pos[r] = -1;
    int64_t* ii = (int64_t*)ray_data(idx);
    for (int64_t k = 0; k < M; k++)
        if (ii[k] >= 0 && ii[k] < N) pos[ii[k]] = k;
    ray_t* out = ray_list_new(N > 0 ? N : 1);
    for (int64_t r = 0; r < N; r++) {
        ray_t* src = pos[r] >= 0 ? upd : base;
        ray_t* iv  = ray_i64(pos[r] >= 0 ? pos[r] : r);
        ray_t* v   = ray_at_fn(src, iv);
        ray_release(iv);
        if (!v || RAY_IS_ERR(v)) { free(pos); ray_release(out); return v ? v : ray_error("oom", NULL); }
        out = ray_list_append(out, v);
        ray_release(v);
    }
    free(pos);
    ray_t* col = q_collapse_list(out);
    ray_release(out);
    return col;
}

/* Scatter for a BRAND-NEW column (absent from the base table): `upd` values land
 * at the `idx` rows, the unselected rows get the type-correct null (kdb:
 * `update newcol:… where …` nulls the rows the where didn't match).  `N` is the
 * full row count. */
static ray_t* funsql_scatter_new(ray_t* idx, ray_t* upd, int64_t N) {
    int64_t M = ray_len(idx);
    /* ray_typed_null wants the ATOM (negative) type: a vector upd of type T
     * nulls as -T; a scalar upd already carries its negative atom type. */
    int8_t et = ray_is_vec(upd) ? (int8_t)(-upd->type)
                                : (upd->type < 0 ? upd->type : (int8_t)(-RAY_I64));
    int64_t* pos = (int64_t*)malloc((N > 0 ? N : 1) * sizeof(int64_t));
    if (!pos) return ray_error("oom", NULL);
    for (int64_t r = 0; r < N; r++) pos[r] = -1;
    int64_t* ii = (int64_t*)ray_data(idx);
    for (int64_t k = 0; k < M; k++)
        if (ii[k] >= 0 && ii[k] < N) pos[ii[k]] = k;
    ray_t* out = ray_list_new(N > 0 ? N : 1);
    for (int64_t r = 0; r < N; r++) {
        ray_t* v;
        if (pos[r] >= 0) { ray_t* iv = ray_i64(pos[r]); v = ray_at_fn(upd, iv); ray_release(iv); }
        else             { v = ray_typed_null(et); }
        if (!v || RAY_IS_ERR(v)) { free(pos); ray_release(out); return v ? v : ray_error("oom", NULL); }
        out = ray_list_append(out, v);
        ray_release(v);
    }
    free(pos);
    ray_t* col = q_collapse_list(out);
    ray_release(out);
    return col;
}

/* Build an update query dict {from: TBL [by: KEY] OUT:EXPR …} from the a-dict
 * of computed columns.  `bykey` (borrowed, may be NULL) is a single grouping
 * column name-ref for by-update. */
static ray_t* funsql_update_dict(ray_t* tbl, ray_t* bykey, ray_t* a) {
    ray_t* kv = ray_sym_vec_new(RAY_SYM_W64, 4);
    ray_t* vl = ray_list_new(4);
    if (bykey) {
        kv = rsymvec_append(kv, "by", 2);
        ray_retain(bykey);
        vl = ray_list_append(vl, bykey);
        ray_release(bykey);
    }
    ray_t* ak = ray_dict_keys(a);
    ray_t* av = ray_dict_vals(a);
    int64_t na = ray_len(ak);
    int av_sym = (av && av->type == RAY_SYM);
    for (int64_t i = 0; i < na; i++) {
        ray_t* nm = ray_sym_vec_cell(ak, i);
        kv = rsymvec_append(kv, ray_str_ptr(nm), (int)ray_str_len(nm));
        ray_t* ex;
        if (av_sym) {
            ray_t* vn = ray_sym_vec_cell(av, i);
            ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
        } else {
            ex = funsql_lower_expr(((ray_t**)ray_data(av))[i]);
        }
        /* A length-1 literal vector is kdb's scalar-broadcast form: a single-sym
         * assignment `col:`x` parses to (,`col)!,,`x — value ,`x, a 1-vector —
         * which must fill EVERY selected row.  It cannot be reduced to a `x atom
         * (funsql reads a bare symbol as a column/name reference, not a literal —
         * that is why kdb keeps the enlist), and the base ray_update null-fills
         * rather than broadcasts a short literal, so replicate the 1-vector to
         * the target row count (`tbl` is the full table, or the where-filtered
         * subtable) with take.  Excludes RAY_STR (ray_len garbage on the SSO
         * string atom) and general lists; multi-element literals keep length.
         * Skipped under a by-clause (grouped assignment realigns per group; the
         * doc by-forms use aggregate/uniform exprs, never bare literals). */
        if (!bykey && ex && ray_is_vec(ex) && ex->type != RAY_STR &&
            ray_len(ex) == 1) {
            int64_t nrow = ray_table_nrows(tbl);
            if (nrow > 1) {
                ray_t* nobj = ray_i64(nrow);
                ray_t* rep = ray_take_fn(ex, nobj);
                ray_release(nobj);
                if (rep && !RAY_IS_ERR(rep)) { ray_release(ex); ex = rep; }
                else if (rep) ray_release(rep);
            }
        }
        vl = ray_list_append(vl, ex);
        ray_release(ex);
    }
    kv = rsymvec_append(kv, "from", 4);
    ray_retain(tbl);
    vl = ray_list_append(vl, tbl);
    ray_release(tbl);
    return ray_dict_new(kv, vl);   /* consumes kv, vl */
}

/* Collapse duplicate columns produced by the base update-by path (which
 * appends a fresh `p` column rather than replacing the existing one): keep each
 * name at its first-occurrence position but take the value from its LAST
 * occurrence (kdb: `update col:…` replaces in place).  Returns a fresh table if
 * duplicates were found, else `tbl` retained. */
static ray_t* funsql_dedup_cols(ray_t* tbl) {
    if (!tbl || tbl->type != RAY_TABLE) { if (tbl) ray_retain(tbl); return tbl; }
    int64_t nc = ray_table_ncols(tbl);
    int has_dup = 0;
    for (int64_t c = 0; c < nc && !has_dup; c++)
        for (int64_t d = c + 1; d < nc; d++)
            if (ray_table_col_name(tbl, c) == ray_table_col_name(tbl, d)) { has_dup = 1; break; }
    if (!has_dup) { ray_retain(tbl); return tbl; }
    ray_t* out = ray_table_new((int32_t)(nc > 0 ? nc : 1));
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
        int64_t nm = ray_table_col_name(tbl, c);
        int seen = 0;
        for (int64_t e = 0; e < c; e++) if (ray_table_col_name(tbl, e) == nm) { seen = 1; break; }
        if (seen) continue;
        int64_t last = c;
        for (int64_t d = c + 1; d < nc; d++) if (ray_table_col_name(tbl, d) == nm) last = d;
        ray_t* col = ray_table_get_col_idx(tbl, last);
        out = ray_table_add_col(out, nm, col);
    }
    return out;
}

/* Single grouping column name-ref for update-by, or NULL.  b may be a dict
 * {name:col} (use the col), a -RAY_SYM, or a 1-vector. */
static ray_t* funsql_by_key(ray_t* b) {
    if (!b) return NULL;
    if (b->type == -RAY_SYM) return ray_sym(b->i64);
    if (b->type == RAY_SYM && ray_len(b) >= 1) {
        ray_t* cn = ray_sym_vec_cell(b, 0);
        return ray_sym(ray_sym_intern_runtime(ray_str_ptr(cn), ray_str_len(cn)));
    }
    if (b->type == RAY_DICT) {
        ray_t* bv = ray_dict_vals(b);
        if (bv && bv->type == RAY_SYM && ray_len(bv) >= 1) {
            ray_t* vn = ray_sym_vec_cell(bv, 0);
            return ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
        }
        if (bv && bv->type == RAY_LIST && ray_len(bv) >= 1) {
            ray_t* lowered = funsql_lower_expr(((ray_t**)ray_data(bv))[0]);
            if (lowered && lowered->type == -RAY_SYM) return lowered;
            if (lowered) ray_release(lowered);
        }
    }
    return NULL;
}

/* `![t;c;b;a]` — update (a is a dict) or delete (a is a symbol vector). */
static ray_t* q_funsql_bang_impl(ray_t* t, ray_t* c, ray_t* b, ray_t* a) {
    /* Namespace EXPUNGE (q4m3 §12.5): `delete x from `.` / `delete wrong
     * from `.jab` — the "table" is a context handle and `a` names the
     * bindings to remove.  Engine env_set_dotted(NULL) does the dict
     * delete + empty-context cascade; result is the handle (kdb returns
     * `.).  Claimed BEFORE table resolution — `.` is not a table. */
    if (t && t->type == -RAY_SYM && a && a->type == RAY_SYM && ray_len(a) > 0) {
        ray_t* hs = ray_sym_str(t->i64);
        if (hs) {
            const char* hn = ray_str_ptr(hs);
            size_t hl = ray_str_len(hs);
            int is_root = (hl == 1 && hn[0] == '.');
            int is_ctx  = (hl >= 2 && hn[0] == '.' && hn[1] != ':' &&
                           hn[1] != '.' &&
                           (!memchr(hn + 1, '.', hl - 1) ||
                            q_ns_is_context(hn, hl)));
            if (is_root || is_ctx) {
                int64_t nn = ray_len(a);
                for (int64_t i = 0; i < nn; i++) {
                    ray_t* cn = ray_sym_vec_cell(a, i);
                    char full[192];
                    full[0] = '\0';           /* error paths print `full` */
                    int fl = is_root
                        ? snprintf(full, sizeof full, "%.*s",
                                   (int)ray_str_len(cn), ray_str_ptr(cn))
                        : snprintf(full, sizeof full, "%.*s.%.*s", (int)hl, hn,
                                   (int)ray_str_len(cn), ray_str_ptr(cn));
                    ray_err_t err = (fl > 0 && (size_t)fl < sizeof full)
                        ? ray_env_set(ray_sym_intern(full, (size_t)fl), NULL)
                        : RAY_ERR_TYPE;
                    if (err != RAY_OK) {
                        ray_release(hs);
                        return err == RAY_ERR_RESERVED
                            ? ray_error("reserve", "delete: '%s' is reserved", full)
                            : ray_error(ray_err_code_str(err),
                                        "delete: '%s' failed", full);
                    }
                }
                ray_release(hs);
                ray_retain(t);
                return t;
            }
            ray_release(hs);
        }
    }

    /* Dict-entry delete (delete.md "Dictionary entries"): a plain symbol-keyed
     * dictionary that is neither a namespace handle nor a table.  `a` names the
     * keys to drop (same shape as the del-cols column list); `c` is empty.
     *   by-name  (`delete b from `d`, t = `d):   amend the env binding IN PLACE
     *            and return the name (mirrors the expunge path's handle return).
     *   by-value (![d;();0b;enlist`b], t = a dict value): return the pruned dict.
     * Claimed BEFORE funsql_resolve_table, which rejects non-tables.  Reuses the
     * engine's ray_dict_remove (single-home drop) per dropped key.  Excludes
     * keyed tables (RAY_DICT whose keys are a table, not a RAY_SYM vector). */
    if (a && a->type == RAY_SYM && ray_len(a) > 0) {
        int by_name = (t && t->type == -RAY_SYM);
        ray_t* dv = by_name ? ray_env_get(t->i64) : t;      /* borrowed */
        if (dv && !RAY_IS_ERR(dv) && dv->type == RAY_DICT) {
            ray_t* dk = ray_dict_keys(dv);
            if (dk && dk->type == RAY_SYM) {
                ray_retain(dv);                             /* ray_dict_remove consumes */
                int64_t nd = ray_len(a);
                for (int64_t i = 0; i < nd && dv && !RAY_IS_ERR(dv); i++) {
                    ray_t* cn = ray_sym_vec_cell(a, i);
                    ray_t* katom = ray_sym(ray_sym_intern_runtime(
                        ray_str_ptr(cn), ray_str_len(cn)));
                    dv = ray_dict_remove(dv, katom);        /* consumes dv; katom borrowed */
                    ray_release(katom);
                }
                if (!dv || RAY_IS_ERR(dv)) return dv ? dv : ray_error("oom", NULL);
                if (by_name) {
                    ray_err_t err = ray_env_set(t->i64, dv);   /* retains dv */
                    ray_release(dv);
                    if (err != RAY_OK)
                        return ray_error(ray_err_code_str(err),
                                         "delete: dict update failed");
                    ray_retain(t);
                    return t;
                }
                return dv;
            }
        }
    }

    ray_t* tbl = funsql_resolve_table(t);
    if (RAY_IS_ERR(tbl)) return tbl;

    int is_update = a && a->type == RAY_DICT;

    if (!is_update) {
        /* DELETE.  a non-empty (symbol vector) => drop those columns; else the
         * where-constraints select rows to remove. */
        int del_cols = a && a->type == RAY_SYM && ray_len(a) > 0;
        if (del_cols) {
            int64_t ndrop = ray_len(a);
            int64_t drop_ids[64];
            if (ndrop > 64) ndrop = 64;
            for (int64_t i = 0; i < ndrop; i++) {
                ray_t* cn = ray_sym_vec_cell(a, i);
                drop_ids[i] = ray_sym_intern_runtime(ray_str_ptr(cn), ray_str_len(cn));
            }
            int64_t nc = ray_table_ncols(tbl);
            ray_t* out = ray_table_new((int32_t)(nc > 0 ? nc : 1));
            for (int64_t col = 0; col < nc && !RAY_IS_ERR(out); col++) {
                int64_t nm = ray_table_col_name(tbl, col);
                int drop = 0;
                for (int64_t i = 0; i < ndrop; i++) if (drop_ids[i] == nm) { drop = 1; break; }
                if (drop) continue;
                ray_t* cv = ray_table_get_col_idx(tbl, col);
                out = ray_table_add_col(out, nm, cv);
            }
            ray_release(tbl);
            /* Deleting EVERY column wipes the table's schema — kdb signals
             * (owner-confirmed behaviour; the error code/text here are ours,
             * not doc-derived).  Dropping a non-existent column is a no-op and
             * must NOT error, so key on the RESULT having zero columns while the
             * source had >=1 (not on the drop-set size). */
            if (!RAY_IS_ERR(out) && nc > 0 && ray_table_ncols(out) == 0) {
                ray_release(out);
                return ray_error("domain",
                                 "delete: cannot delete all columns from a table");
            }
            return out;
        }
        /* delete rows matching c: keep the complement.  Empty c => delete ALL
         * rows (kdb: `delete from t` with no where empties the table) — index
         * the table at an empty row-index so the columns/schema survive.
         * funsql_filter returns the RETAINED full table when there are no
         * constraints (right for select keep=1, wrong for delete keep=0). */
        ray_t* res;
        if (funsql_empty(c)) {
            ray_t* idx = ray_vec_new(RAY_I64, 0);
            if (!idx) { ray_release(tbl); return ray_error("oom", NULL); }
            res = ray_at_fn(tbl, idx);
            ray_release(idx);
        } else {
            res = funsql_filter(tbl, c, 0);
        }
        ray_release(tbl);
        return res;
    }

    /* UPDATE.  Multi-column `by a,b` is not yet supported: funsql_by_key
     * collapses the by-dict to its FIRST grouping column, so a two-key by
     * would silently group by `a` alone and return WRONG aggregates.  Reject
     * it here (single-home — both the string `update … by a,b` and functional
     * ![t;c;(`a`b)!…;a] forms hit this) rather than mis-evaluate; single-column
     * by (the doc-covered form) is unaffected.  See PLAN.md Known defects. */
    if (b && b->type == RAY_DICT) {
        ray_t* bkeys = ray_dict_keys(b);
        if (bkeys && ray_len(bkeys) > 1) {
            ray_release(tbl);
            return ray_error("nyi", "update: multi-column by is not supported");
        }
    }

    /* where (if any) is applied by filtering, running the update on the
     * subtable, then scattering the changed columns back — this composes
     * where+by, which the base ray_update declines to do in one pass. */
    int has_where = !funsql_empty(c);
    ray_t* bykey = funsql_by_key(b);

    if (!has_where) {
        ray_t* dict = funsql_update_dict(tbl, bykey, a);
        if (bykey) ray_release(bykey);
        ray_release(tbl);
        if (!dict || RAY_IS_ERR(dict)) return dict ? dict : ray_error("oom", NULL);
        ray_t* res = ray_update(&dict, 1);
        ray_release(dict);
        if (!res || RAY_IS_ERR(res)) return res;
        ray_t* dd = funsql_dedup_cols(res);
        ray_release(res);
        return dd;
    }

    ray_t* mask = funsql_build_mask(tbl, c);
    if (!mask || RAY_IS_ERR(mask)) { if (bykey) ray_release(bykey); ray_release(tbl); return mask ? mask : ray_error("domain", "update where"); }
    ray_t* idx = ray_where_fn(mask);
    ray_release(mask);
    if (!idx || RAY_IS_ERR(idx)) { if (bykey) ray_release(bykey); ray_release(tbl); return idx ? idx : ray_error("domain", "update where"); }
    ray_t* ft = ray_at_fn(tbl, idx);
    if (!ft || RAY_IS_ERR(ft)) { if (bykey) ray_release(bykey); ray_release(idx); ray_release(tbl); return ft ? ft : ray_error("domain", "update where"); }

    ray_t* udict = funsql_update_dict(ft, bykey, a);
    if (bykey) ray_release(bykey);
    ray_release(ft);
    if (!udict || RAY_IS_ERR(udict)) { ray_release(idx); ray_release(tbl); return udict ? udict : ray_error("oom", NULL); }
    ray_t* uft0 = ray_update(&udict, 1);
    ray_release(udict);
    if (!uft0 || RAY_IS_ERR(uft0)) { ray_release(idx); ray_release(tbl); return uft0 ? uft0 : ray_error("domain", "update"); }
    ray_t* uft = funsql_dedup_cols(uft0);
    ray_release(uft0);

    /* Rebuild the table, scattering each updated column back at `idx`. */
    ray_t* ak = ray_dict_keys(a);
    int64_t na = ray_len(ak);
    int64_t upd_ids[64];
    if (na > 64) na = 64;
    for (int64_t i = 0; i < na; i++) {
        ray_t* nm = ray_sym_vec_cell(ak, i);
        upd_ids[i] = ray_sym_intern_runtime(ray_str_ptr(nm), ray_str_len(nm));
    }
    int64_t nc = ray_table_ncols(tbl);
    ray_t* out = ray_table_new((int32_t)(nc > 0 ? nc : 1));
    for (int64_t col = 0; col < nc && !RAY_IS_ERR(out); col++) {
        int64_t nm = ray_table_col_name(tbl, col);
        int updated = 0;
        for (int64_t i = 0; i < na; i++) if (upd_ids[i] == nm) { updated = 1; break; }
        ray_t* base = ray_table_get_col_idx(tbl, col);
        if (!updated) { out = ray_table_add_col(out, nm, base); continue; }
        ray_t* newvals = ray_table_get_col(uft, nm);
        if (!newvals) { out = ray_table_add_col(out, nm, base); continue; }
        ray_t* merged = funsql_scatter(base, idx, newvals);
        if (!merged || RAY_IS_ERR(merged)) { ray_release(out); ray_release(idx); ray_release(uft); ray_release(tbl); return merged ? merged : ray_error("oom", NULL); }
        out = ray_table_add_col(out, nm, merged);
        ray_release(merged);
    }
    /* Append columns that `a` CREATED (absent from the base table): the where
     * path filters to a subtable, so these live only in `uft` — scatter them
     * across the full row count with nulls on the unselected rows (parity with
     * the no-where update path, which adds them directly). */
    int64_t ntbl = ray_table_nrows(tbl);
    for (int64_t i = 0; i < na && !RAY_IS_ERR(out); i++) {
        if (ray_table_get_col(tbl, upd_ids[i])) continue;      /* already emitted */
        ray_t* newvals = ray_table_get_col(uft, upd_ids[i]);
        if (!newvals) continue;
        ray_t* merged = funsql_scatter_new(idx, newvals, ntbl);
        if (!merged || RAY_IS_ERR(merged)) { ray_release(out); ray_release(idx); ray_release(uft); ray_release(tbl); return merged ? merged : ray_error("oom", NULL); }
        out = ray_table_add_col(out, upd_ids[i], merged);
        ray_release(merged);
    }
    ray_release(idx);
    ray_release(uft);
    ray_release(tbl);
    return out;
}

/* SPECIAL-FORM entry points: evaluate the four operands (mapping `()`→null),
 * dispatch, then release the evaluated operands. */
static ray_t* q_funsql_dispatch(ray_t** args, int64_t n,
                                ray_t* (*impl)(ray_t*, ray_t*, ray_t*, ray_t*),
                                const char* glyph) {
    if (n != 4) return ray_error("rank", "%s[t;c;b;a]: expects 4 args, got %lld", glyph, (long long)n);
    ray_t* ev[4] = {0};
    for (int i = 0; i < 4; i++) {
        ev[i] = funsql_operand(args[i]);
        if (!ev[i] || RAY_IS_ERR(ev[i])) {
            ray_t* err = ev[i] ? ev[i] : ray_error("domain", "funsql: operand eval");
            for (int j = 0; j < i; j++) ray_release(ev[j]);
            return err;
        }
    }
    ray_t* res = impl(ev[0], ev[1], ev[2], ev[3]);
    for (int i = 0; i < 4; i++) ray_release(ev[i]);
    return res;
}

ray_t* q_funsql_select(ray_t** args, int64_t n) {
    return q_funsql_dispatch(args, n, q_funsql_select_impl, "?");
}
ray_t* q_funsql_bang(ray_t** args, int64_t n) {
    return q_funsql_dispatch(args, n, q_funsql_bang_impl, "!");
}

/* String `delete` statement executor.  Lowered from the symbolic (!;`t;c;0b;a)
 * tree by ql_qsql_bang: a SPECIAL FORM receiving the four operands UNEVALUATED.
 *   t  the from-table: a `t name-sym (by-name), a `. context handle, or an
 *      already-lowered expression subtree — funsql_operand resolves all three.
 *   c  where-constraints: `enlist(constraint-list)` (row form) or `()`; the
 *      parse-trees stay UNevaluated — funsql_build_mask (via q_funsql_bang_impl)
 *      interprets them against the table columns.
 *   b  0b (unused by delete).
 *   a  empty symbol vector (row form) or the column-name symbol vector (col form).
 * Reuses q_funsql_bang_impl wholesale (delete-rows / delete-cols / expunge).
 * Refcount: q_funsql_bang_impl BORROWS its four operands (it never consumes
 * them — it retains what it keeps), so `cc`/args[2]/args[3] are passed without
 * a transfer; only the OWNED `tv` from funsql_operand is released here. */
ray_t* q_delete_exec(ray_t** args, int64_t n) {
    if (n != 4)
        return ray_error("rank", "delete[t;c;b;a]: expects 4 args, got %lld", (long long)n);
    ray_t* tv = funsql_operand(args[0]);
    if (!tv || RAY_IS_ERR(tv)) return tv;
    /* statement c = enlist(constraint-list); the engine wants the inner list */
    ray_t* c = args[1];
    ray_t* cc = (c && c->type == RAY_LIST && ray_len(c) == 1)
                    ? ((ray_t**)ray_data(c))[0]
                    : c;
    ray_t* res = q_funsql_bang_impl(tv, cc, args[2], args[3]);
    ray_release(tv);
    return res;
}

/* String `exec` statement executor.  Lowered from the symbolic (?;`t;c;b;a)
 * statement tree by ql_qsql_exec: a SPECIAL FORM receiving the four operands
 * UNEVALUATED.  Reuses q_funsql_select_impl wholesale so the STRING `exec …`
 * form and the FUNCTIONAL `?[t;c;b;a]` exec form share ONE result-shaping engine
 * (single-home) and stay automatically equivalent.
 *   t  the from-table: a `t name-sym (by-name) or an already-lowered expression
 *      subtree — funsql_operand resolves both (evaluates it to the table value).
 *   c  where-constraints: `enlist(constraint-list)` (statement form) or `()`;
 *      unwrapped to the inner constraint-list, then interpreted UNEVALUATED
 *      against the table columns by q_funsql_select_impl (funsql_build_mask).
 *   b  the By-phrase: `()` (no grouping, exec shaping) or a bare/vector symbol
 *      (grouped exec — deferred by the impl).  Passed RAW.
 *   a  the Select-phrase: `()` (last record), a bare column symbol (the column
 *      value), or a name!expr dict (dict result).  Passed RAW (its parse-trees
 *      stay unevaluated — funsql_eval interprets them against the columns).
 * Refcount: q_funsql_select_impl BORROWS its four operands (retains what it
 * keeps), so c/b/a pass without a transfer; only the OWNED `tv` is released. */
ray_t* q_exec_exec(ray_t** args, int64_t n) {
    if (n != 4)
        return ray_error("rank", "exec[t;c;b;a]: expects 4 args, got %lld", (long long)n);
    ray_t* tv = funsql_operand(args[0]);
    if (!tv || RAY_IS_ERR(tv)) return tv;
    /* statement c = enlist(constraint-list); the engine wants the inner list */
    ray_t* c = args[1];
    ray_t* cc = (c && c->type == RAY_LIST && ray_len(c) == 1)
                    ? ((ray_t**)ray_data(c))[0]
                    : c;
    ray_t* res = q_funsql_select_impl(tv, cc, args[2], args[3]);
    ray_release(tv);
    return res;
}
