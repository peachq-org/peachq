/* q_wrap_applyiter.c — amend/apply/trap (@ and . ternary+), iterators (each-both,
 * each-prior, over/scan), lambda carrier (q.fn, ret/sig), and
 * the imperative control constructs (q.seq/if/do/while)
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_apply.h" /* q_apply_noun — @/. noun arms */
#include "qlang/q_deriv.h" /* q_proj_new, q_compose_new, q_lambda_carrier_new — 104h carriers */
#include "lang/env.h"      /* ray_env_get */
#include "lang/eval.h"     /* ray_eval; ray_fold_fn/ray_map_fn/ray_scan_fn HOFs; RAY_FN_SPECIAL_FORM, LAMBDA_PARAMS */
#include "lang/internal.h" /* call_fn1/2, atom_eq, as_i64, ray_error */
#include "lang/format.h"   /* ray_type_name — error messages */
#include "ops/ops.h"       /* ray_is_lazy, ray_lazy_materialize — control forms */
#include "table/sym.h"     /* ray_sym_intern_runtime */
#include <stdint.h>        /* uintptr_t */
#include <string.h>        /* memcpy, strlen */

/* ===== Amend / Apply / Trap — @ and . ternary+ forms (wave-3) ==============
 * ref/amend.md + ref/apply.md.  q_at_wrap / q_dot_wrap are VARY wrappers now;
 * the rayfall VARY dispatch (eval.c) evaluates every bracket arg and
 * short-circuits an arg-eval error BEFORE calling us — exactly kdb Trap's
 * "errors in fx are not caught" rule.  Dispatch on arg count and first-arg
 * kind: a callable first arg -> Trap; a data first arg -> Amend; n==2 keeps
 * the historic Apply/Index path.  Amend is copy-on-write: q_explode to a
 * fresh rc==1 boxed list, run a sequential single-path engine (repeated
 * indices and cross-sections both decompose into it), collapse back. */

/* fwd decls (mutual recursion / define-before-use) */
static ray_t* q_amend_at(ray_t* v, ray_t* idx, ray_t* f, ray_t* y);
static ray_t* q_amend_dot(ray_t* d, ray_t* idx, ray_t* f, ray_t* y);
static ray_t* q_at_apply2(ray_t* f, ray_t* x);
static ray_t* q_dot_apply(ray_t* f, ray_t* a);
static ray_t* q_elem_at(ray_t* v, int64_t i);       /* defined below */

/* The ray_map_fn discipline (collection.c:414), for every apply/iterate site
 * in this file: a step result may be a LAZY DAG node BORROWING an operand the
 * site is about to release — force it before the release/store. */
static ray_t* q_force(ray_t* r) {
    return (r && ray_is_lazy(r)) ? ray_lazy_materialize(r) : r;
}

/* The `:` (assign / replace) function slot of Amend.  Arrives either as a
 * symbol atom spelled ":" (quoted verb-sym) or, defensively, a registry value
 * whose provenance spelling is ":". */
static int q_is_assign(ray_t* f) {
    if (!f) return 0;
    if (f->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(f->i64);
        int yes = s && ray_str_len(s) == 1 && ray_str_ptr(s)[0] == ':';
        if (s) ray_release(s);
        return yes;
    }
    q_provenance_t pv;
    return q_registry_provenance(f, &pv) && pv.spelling
        && pv.spelling[0] == ':' && pv.spelling[1] == '\0';
}

/* Index element read: bool arm + the strict-cast law (callers value-check). */
static int q_idx_int(ray_t* e, int64_t* out) {
    if (e && e->type == -RAY_BOOL) { *out = e->b8; return 1; }
    return q_strict_i64(e, out);
}

/* Copy v's items into a fresh rc==1 boxed list (amend in place with
 * ray_list_set, collapse back afterwards).  Borrowed v; owned result. */
static ray_t* q_explode(ray_t* v) {
    int64_t n = ray_len(v);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (!out || RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = q_elem_at(v, i);                 /* owned v[i] */
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
        out = ray_list_append(out, e);              /* RETAINS */
        ray_release(e);
    }
    return out;
}

/* u each v, collapsed to a typed vector (kdb whole-value @[d;::;u] == u'[d]). */
static ray_t* q_each_over(ray_t* f, ray_t* v) {
    ray_t* mapargs[2] = { f, v };
    ray_t* r = ray_map_fn(mapargs, 2);              /* base map == each */
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* c = q_collapse_list(r);
    ray_release(r);
    return c;
}

/* One amend update at an already-selected leaf `cur` (borrowed):
 *   unary   f(cur)          (y == NULL)
 *   binary  f(cur, ry)      (y != NULL)
 *   assign  ry              (f is `:`)
 * ry is y whole (yi < 0) or y[yi] when y is a per-step vector.  Owned leaf. */
static ray_t* q_amend_step(ray_t* f, ray_t* cur, ray_t* y, int64_t yi) {
    if (q_is_assign(f) && !y)
        return ray_error("type", "@: assign (:) needs a replacement value");
    ray_t* ry = NULL;                                /* borrowed-ish (owned) */
    if (y) {
        if (yi >= 0 && (ray_is_vec(y) || y->type == RAY_LIST)) {
            ry = q_elem_at(y, yi);                    /* owned */
            if (!ry || RAY_IS_ERR(ry)) return ry;
        } else { ry = y; ray_retain(ry); }           /* whole y */
    }
    ray_t* r;
    if (q_is_assign(f)) { r = ry; ray_retain(r); }   /* replace: new = ry */
    else if (y)         r = q_force(call_fn2(f, cur, ry)); /* binary f(cur, ry) */
    else                r = q_force(call_fn1(f, cur));     /* unary  f(cur)     */
    if (ry) ray_release(ry);
    return r;
}

/* @[d;key(s);f;y] — dict amend.  For each key: if present, amend the value at
 * its position; if ABSENT, extend the dict with that key (kdb inserts on a
 * missing-key amend, e.g. @[`a`b!1 2;`c;:;3] -> a|1 b|2 c|3).  Works on
 * exploded keys/vals boxed lists, collapses back, rebuilds. */
static ray_t* q_amend_dict(ray_t* d, ray_t* key, ray_t* f, ray_t* y) {
    ray_t* keys0 = ray_dict_keys(d);                /* borrowed */
    ray_t* vals0 = ray_dict_vals(d);                /* borrowed */
    if (!keys0 || !vals0) return ray_error("type", "@: malformed dictionary");
    ray_t* keys = q_explode(keys0);
    if (!keys || RAY_IS_ERR(keys)) return keys;
    ray_t* vals = q_explode(vals0);
    if (!vals || RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    int key_atom = ray_is_atom(key);
    int64_t steps = key_atom ? 1 : ray_len(key);
    if (!key_atom && y && (ray_is_vec(y) || y->type == RAY_LIST) && ray_len(y) != steps) {
        ray_release(keys); ray_release(vals); return ray_error("length", NULL);
    }
    for (int64_t k = 0; k < steps; k++) {
        ray_t* kk = key_atom ? (ray_retain(key), key) : q_elem_at(key, k);  /* owned */
        if (!kk || RAY_IS_ERR(kk)) { ray_release(keys); ray_release(vals); return kk; }
        /* locate kk in the working keys (linear; dicts are small) */
        int64_t pos = -1, kn = ray_len(keys);
        for (int64_t j = 0; j < kn; j++) {
            ray_t* kj = ray_list_get(keys, j);       /* borrowed */
            if (q_values_match(kj, kk)) { pos = j; break; }
        }
        int64_t yi = key_atom ? -1 : k;
        if (pos >= 0) {
            ray_t* cur = ray_list_get(vals, pos);    /* borrowed */
            ray_t* nv = q_amend_step(f, cur, y, yi);
            if (!nv || RAY_IS_ERR(nv)) { ray_release(kk); ray_release(keys); ray_release(vals); return nv; }
            vals = ray_list_set(vals, pos, nv);
            ray_release(nv);
        } else {                                     /* insert: apply to generic null */
            ray_t* nv = q_amend_step(f, RAY_NULL_OBJ, y, yi);
            if (!nv || RAY_IS_ERR(nv)) { ray_release(kk); ray_release(keys); ray_release(vals); return nv; }
            keys = ray_list_append(keys, kk);        /* RETAINS kk */
            vals = ray_list_append(vals, nv);
            ray_release(nv);
        }
        ray_release(kk);
    }
    ray_t* ck = q_collapse_list(keys);  ray_release(keys);
    ray_t* cv = q_collapse_list(vals);  ray_release(vals);
    if (!ck || RAY_IS_ERR(ck)) { if (cv) ray_release(cv); return ck; }
    if (!cv || RAY_IS_ERR(cv)) { ray_release(ck); return cv; }
    return ray_dict_new(ck, cv);                     /* consumes ck + cv */
}

/* Amend At — @[v;i;f] / @[v;i;f;y] / @[v;::;f...] (whole) / dict amend.
 * v borrowed; returns an owned copy-on-write value or owned error. */
static ray_t* q_amend_at(ray_t* v, ray_t* idx, ray_t* f, ray_t* y) {
    if (RAY_IS_NULL(idx)) {                          /* whole value */
        if (q_is_assign(f)) {
            if (!y) return ray_error("type", "@: assign (:) needs a value");
            ray_retain(y); return y;
        }
        if (y) return ray_error("nyi", "@[d;::;v;vy] deferred");  /* v'[d;vy] */
        ray_t* r = q_each_over(f, v);                 /* u'[d] */
        /* amend-entire conform rule (ref/amend.md: @[1 2;::;3 4*] -> 'type):
         * boxed per-item results cannot fit back into a typed-vector d.
         * Checked here, not in q_each_over — `each` itself may box freely. */
        if (r && !RAY_IS_ERR(r) && ray_is_vec(v) && r->type == RAY_LIST) {
            ray_release(r);
            return ray_error("type", "@: amend result does not conform to a %s vector",
                             ray_type_name(v->type));
        }
        return r;
    }
    if (v->type == RAY_DICT) return q_amend_dict(v, idx, f, y);
    if (!ray_is_vec(v) && v->type != RAY_LIST)
        return ray_error("type", "@: cannot amend a %s", ray_type_name(v->type));
    int64_t n = ray_len(v);
    int idx_atom = ray_is_atom(idx);
    int64_t steps = idx_atom ? 1 : ray_len(idx);
    /* vector index + vector y must conform (kdb 'length) */
    if (!idx_atom && y && (ray_is_vec(y) || y->type == RAY_LIST) && ray_len(y) != steps)
        return ray_error("length", NULL);
    ray_t* work = q_explode(v);
    if (!work || RAY_IS_ERR(work)) return work;
    for (int64_t k = 0; k < steps; k++) {
        int64_t pos;
        if (idx_atom) { if (!q_idx_int(idx, &pos)) { ray_release(work); return ray_error("index", NULL); } }
        else { ray_t* p = q_elem_at(idx, k); int ok = q_idx_int(p, &pos); ray_release(p);
               if (!ok) { ray_release(work); return ray_error("index", NULL); } }
        if (pos < 0 || pos >= n) { ray_release(work); return ray_error("index", NULL); }
        ray_t* cur = ray_list_get(work, pos);        /* borrowed slot */
        ray_t* nv  = q_amend_step(f, cur, y, idx_atom ? -1 : k);
        if (!nv || RAY_IS_ERR(nv)) { ray_release(work); return nv; }
        work = ray_list_set(work, pos, nv);          /* retains nv */
        ray_release(nv);
    }
    ray_t* c = q_collapse_list(work);
    ray_release(work);
    return c;
}

/* Deep single-path amend: descend path[0..plen) into d (copy-on-write), apply
 * the update at the leaf.  d borrowed; owned result. */
static ray_t* q_amend_path(ray_t* d, const int64_t* path, int64_t plen,
                           ray_t* f, ray_t* y, int64_t yi) {
    if (plen == 0) return q_amend_step(f, d, y, yi);
    int64_t pos = path[0];
    if (!(ray_is_vec(d) || d->type == RAY_LIST) || pos < 0 || pos >= ray_len(d))
        return ray_error("index", NULL);
    ray_t* work = q_explode(d);
    if (!work || RAY_IS_ERR(work)) return work;
    ray_t* child = ray_list_get(work, pos);          /* borrowed */
    ray_t* nc = q_amend_path(child, path + 1, plen - 1, f, y, yi);
    if (!nc || RAY_IS_ERR(nc)) { ray_release(work); return nc; }
    work = ray_list_set(work, pos, nc);
    ray_release(nc);
    ray_t* c = q_collapse_list(work);
    ray_release(work);
    return c;
}

/* Cross-sectional deep amend: idx items are vectors; amend every path in the
 * cartesian product idx[0] x idx[1] x ... sequentially (repeats accumulate).
 * Core rows use scalar/unary updates (y broadcast whole). */
static ray_t* q_amend_cross(ray_t* d, ray_t* idx, ray_t* f, ray_t* y) {
    int64_t rank = ray_len(idx);
    if (rank < 1 || rank > 8) return ray_error("rank", ".: cross-section 1..8 deep");
    ray_t* dims[8]; int64_t dn[8];
    for (int64_t a = 0; a < rank; a++) {
        ray_t* col = q_elem_at(idx, a);              /* owned; vector or atom */
        if (!col || RAY_IS_ERR(col)) { for (int64_t b=0;b<a;b++) ray_release(dims[b]); return col; }
        dims[a] = col; dn[a] = ray_is_atom(col) ? 1 : ray_len(col);
    }
    int64_t total = 1; for (int64_t a = 0; a < rank; a++) total *= dn[a];
    ray_t* acc = d; ray_retain(acc);
    for (int64_t t = 0; t < total; t++) {
        int64_t path[8], rem = t; int bad = 0;
        for (int64_t a = rank - 1; a >= 0; a--) {
            int64_t ix = dn[a] ? rem % dn[a] : 0; if (dn[a]) rem /= dn[a];
            if (ray_is_atom(dims[a])) { if (!q_idx_int(dims[a], &path[a])) bad = 1; }
            else { ray_t* p = q_elem_at(dims[a], ix); if (!q_idx_int(p, &path[a])) bad = 1; ray_release(p); }
        }
        ray_t* na = bad ? ray_error("index", NULL)
                        : q_amend_path(acc, path, rank, f, y, -1);
        ray_release(acc);
        if (!na || RAY_IS_ERR(na)) { for (int64_t a=0;a<rank;a++) ray_release(dims[a]); return na; }
        acc = na;
    }
    for (int64_t a = 0; a < rank; a++) ray_release(dims[a]);
    return acc;
}

/* Amend (deep) — .[d;i;f] / .[d;i;v;vy] / .[d;();f...] (whole). */
static ray_t* q_amend_dot(ray_t* d, ray_t* idx, ray_t* f, ray_t* y) {
    if (RAY_IS_NULL(idx)) {                          /* .[d;();u] == u[d] */
        if (q_is_assign(f)) {
            if (!y) return ray_error("type", ".: assign (:) needs a value");
            ray_retain(y); return y;
        }
        if (y) return call_fn2(f, d, y);
        return call_fn1(f, d);
    }
    int64_t ilen = ray_is_atom(idx) ? 1 : ray_len(idx);
    int all_atom = 1;
    if (!ray_is_atom(idx) && idx->type == RAY_LIST) {
        ray_t** e = (ray_t**)ray_data(idx);
        for (int64_t k = 0; k < ilen; k++)
            if (e[k] && !ray_is_atom(e[k])) { all_atom = 0; break; }
    }
    if (all_atom) {
        if (ilen > 8) return ray_error("rank", ".: index path too deep");
        int64_t path[8];
        for (int64_t k = 0; k < ilen; k++) {
            int ok;
            if (ray_is_atom(idx)) ok = q_idx_int(idx, &path[k]);
            else { ray_t* p = q_elem_at(idx, k); ok = q_idx_int(p, &path[k]); ray_release(p); }
            if (!ok) return ray_error("index", NULL);
        }
        return q_amend_path(d, path, ilen, f, y, -1);
    }
    return q_amend_cross(d, idx, f, y);              /* items are vectors */
}

/* Trap tail: r is g's (owned) result; on error run/return the handler e. */
static ray_t* q_trap_finish(ray_t* r, ray_t* e) {
    if (!r || !RAY_IS_ERR(r)) return r;              /* success passes through */
    ray_t* text = q_registry_sig_take();             /* full signal text, owned */
    if (!text) { const char* c = ray_err_code(r); text = ray_str(c ? c : "", c ? strlen(c) : 0); }
    text = q_charv_out(text);                        /* handler receives a charv */
    ray_error_free(r);
    if (!q_is_fn_value(e)) { ray_release(text); ray_retain(e); return e; }
    ray_t* hr = call_fn1(e, text);
    ray_release(text);
    return hr;
}

/* Trap At — @[f;fx;e] == .[f;enlist fx;e]. */
static ray_t* q_trap(ray_t* f, ray_t* x, ray_t* e) {
    q_registry_sig_clear();                          /* drop stale payload */
    ray_t* args[1] = { x };
    ray_t* r = q_call_n(f, args, 1);
    return q_trap_finish(r, e);
}

/* Trap — .[g;gx;e]: gx is the argument LIST; spread-apply g over it. */
static ray_t* q_trap_dot(ray_t* g, ray_t* gx, ray_t* e) {
    q_registry_sig_clear();
    if (!gx || (!ray_is_vec(gx) && gx->type != RAY_LIST))
        return ray_error("type", ".: trap args must be a list");
    int64_t k = ray_len(gx);
    if (k < 1 || k > 8) return ray_error("rank", ".: 1..8 trap args");
    ray_t* a[8];
    for (int64_t i = 0; i < k; i++) a[i] = q_elem_at(gx, i);   /* owned */
    ray_t* r = q_force(q_call_n(g, a, k));
    for (int64_t i = 0; i < k; i++) ray_release(a[i]);
    return q_trap_finish(r, e);
}

/* q `f@x` — Apply At / Index At (ref/apply.md).  A callable f invokes with
 * the single argument; everything else (vector, list, dict, table, 104h
 * carrier) delegates to q_apply_noun — identical semantics to `f[x]`/`f x`. */
static ray_t* q_at_apply2(ray_t* f, ray_t* x) {
    if (f && (f->type == RAY_UNARY || f->type == RAY_BINARY || f->type == RAY_VARY)
          && (f->attrs & RAY_FN_SPECIAL_FORM))
        return ray_error("type", "@: special forms cannot be applied");
    if (f && f->type == RAY_UNARY)
        return ((ray_unary_fn)(uintptr_t)f->i64)(x);
    if (f && f->type == RAY_VARY) {
        ray_t* one[1] = { x };
        return ((ray_vary_fn)(uintptr_t)f->i64)(one, 1);
    }
    if (f && f->type == RAY_BINARY)
        return ray_error("rank", "@: unary application of a binary verb");
    ray_t* args[1] = { x };
    ray_t* r = q_apply_noun(f, args, 1);
    return r ? r : ray_error("type", "@: not applicable");
}

/* @ VARY: 2 args Apply/Index; 3 args Trap-At (callable) or Amend-At (data);
 * 4 args Amend-At (quaternary).  An ELIDED argument (`@[count;;-1]`) is turned
 * into a projection at lower time (q.mkopproj, q_parse.c) — so every call that
 * reaches here is a fully-bound apply/trap/amend, never a projection. */
ray_t* q_at_wrap(ray_t** args, int64_t n) {
    if (n == 2) return q_at_apply2(args[0], args[1]);
    if (n == 3) {
        if (q_is_fn_value(args[0])) return q_trap(args[0], args[1], args[2]);
        return q_amend_at(args[0], args[1], args[2], NULL);
    }
    if (n == 4) return q_amend_at(args[0], args[1], args[2], args[3]);
    return ray_error("rank", "@: got %lld args", (long long)n);
}

/* q `v . vx` — Apply / Index (ref/apply.md): the rhs is the ARGUMENT LIST —
 * a rank-n callable spread-calls over vx's n items; a noun depth-indexes
 * (m . 1 2 is m[1;2]).  Atom rhs is not a list -> 'type (kdb wants a list). */
static ray_t* q_dot_apply(ray_t* f, ray_t* a) {
    if (!a || (!ray_is_vec(a) && a->type != RAY_LIST))
        return ray_error("type", ".: rhs must be an argument list");
    if (f && (f->type == RAY_UNARY || f->type == RAY_BINARY || f->type == RAY_VARY)
          && (f->attrs & RAY_FN_SPECIAL_FORM))
        return ray_error("type", ".: special forms cannot be applied");
    int64_t n = ray_len(a);
    if (n < 1 || n > 8) return ray_error("rank", ".: 1..8 arguments");
    ray_t* args[8];
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        args[i] = ray_at_fn(a, ia);
        ray_release(ia);
        if (!args[i] || RAY_IS_ERR(args[i])) {
            ray_t* err = args[i];
            for (int64_t j = 0; j < i; j++) ray_release(args[j]);
            return err ? err : ray_error("type", ".: bad argument list");
        }
    }
    ray_t* r;
    if (f && f->type == RAY_UNARY && n == 1)
        r = ((ray_unary_fn)(uintptr_t)f->i64)(args[0]);
    else if (f && f->type == RAY_BINARY && n == 2)
        r = ((ray_binary_fn)(uintptr_t)f->i64)(args[0], args[1]);
    else if (f && f->type == RAY_VARY)
        r = ((ray_vary_fn)(uintptr_t)f->i64)(args, n);
    else if (f && (f->type == RAY_UNARY || f->type == RAY_BINARY))
        r = ray_error("rank", ".: argument count does not match the verb's rank");
    else {
        r = q_apply_noun(f, args, n);
        if (!r) r = ray_error("type", ".: not applicable");
    }
    r = q_force(r);
    for (int64_t j = 0; j < n; j++) ray_release(args[j]);
    return r;
}

/* . VARY: 2 args Apply/Index; 3 args Trap (callable) or Amend deep (data);
 * 4 args Amend deep (quaternary).  An elided argument projects at lower time
 * (q.mkopproj) — reached here only fully bound. */
ray_t* q_dot_wrap(ray_t** args, int64_t n) {
    if (n == 2) return q_dot_apply(args[0], args[1]);
    if (n == 3) {
        if (q_is_fn_value(args[0])) return q_trap_dot(args[0], args[1], args[2]);
        return q_amend_dot(args[0], args[1], args[2], NULL);
    }
    if (n == 4) return q_amend_dot(args[0], args[1], args[2], args[3]);
    return ray_error("rank", ".: got %lld args", (long long)n);
}

/* q `f each x` — rayfall map, then collapse the boxed result to a simple
 * vector (kdb: `neg each 1 2 3` is -1 -2 -3, type 7h, not a general list). */
ray_t* q_each_wrap(ray_t* f, ray_t* x) {
    /* table arm: q iterates a table's ROWS (`count each t` -> 2 2 2) — each
     * row is the t[i] row dict; keyed tables are a deferred cell. */
    if (x && x->type == RAY_TABLE) {
        int64_t n = ray_table_nrows(x);
        ray_t* outl = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(outl)) return outl;
        for (int64_t i = 0; i < n; i++) {
            ray_t* row = q_table_row_at(x, i);       /* owned row dict (char-column safe) */
            if (!row || RAY_IS_ERR(row)) { ray_release(outl); return row; }
            ray_t* r = q_force(call_fn1(f, row));
            ray_release(row);
            if (!r || RAY_IS_ERR(r)) { ray_release(outl); return r; }
            outl = ray_list_append(outl, r);         /* retains */
            ray_release(r);
            if (RAY_IS_ERR(outl)) return outl;
        }
        ray_t* c = q_collapse_list(outl);
        ray_release(outl);
        return c;
    }
    ray_t* args[2] = { f, x };
    ray_t* r = ray_map_fn(args, 2);
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* c = q_collapse_list(r);
    ray_release(r);
    return c;
}

/* ===== q iterators: each-both ' , each-prior ': , over / , scan \ ==========
 * (wave-2 adverb completion — docs/superpowers/plans/2026-07-06-q-adverbs.md).
 * These are internal (spelling-less) HOF VALUES that q_lower embeds at adverb
 * heads, plus the runtime cores the over/scan/prior/peach/deltas/differ
 * keyword wrappers delegate to.  Every function operand is applied through
 * call_fn1/call_fn2, which fall through to q_apply_noun for 100h lambda and
 * 104h projection carriers — so lambdas, native ops and projections all work.
 *
 * Rank of a q value: 1 monadic, 2 dyadic, -1 ambiguous (native vary). */
static int q_fn_rank(ray_t* f) {
    if (!f) return -1;
    switch (f->type) {
    case RAY_UNARY:  return 1;
    case RAY_BINARY: return 2;
    case RAY_VARY:   return -1;
    case RAY_LAMBDA: return (int)ray_len(LAMBDA_PARAMS(f));
    default: break;
    }
    q_deriv_kind k = q_deriv_kind_of(f);
    if (k == Q_DERIV_LAMBDA) return q_deriv_valence(f);
    if (k == Q_DERIV_MONAD)  return 1;
    if (k == Q_DERIV_PROJ) {
        uint64_t m = q_deriv_hole_mask(f);
        int c = 0; while (m) { c += (int)(m & 1u); m >>= 1; }
        return c;              /* effective rank = open holes */
    }
    return -1;
}

/* True iff x is a callable q value (native fn or carrier) — distinguishes the
 * `while` test-function argument of `/` `\` from a numeric do-count. */
int q_is_fn_value(ray_t* x) {
    if (!x) return 0;
    if (x->type == RAY_UNARY || x->type == RAY_BINARY ||
        x->type == RAY_VARY  || x->type == RAY_LAMBDA) return 1;
    return q_deriv_kind_of(x) != Q_DERIV_NONE;
}

/* Whole-value equivalence (converge stop test) — atom_eq handles atoms,
 * vectors and structural lists. */
int q_values_match(ray_t* a, ray_t* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    return atom_eq(a, b);
}

/* v[i] as an owned atom/element (borrowed v): direct payload read for
 * vectors/lists (collection_elem — no index atom, no ray_at_fn dispatch);
 * generic indexing for every other shape.  alloc==0 results are BORROWED
 * list slots — retain, never release (r0 review). */
static ray_t* q_elem_at(ray_t* v, int64_t i) {
    if (v && (ray_is_vec(v) || v->type == RAY_LIST)) {
        int alloc = 0;
        ray_t* e = collection_elem(v, i, &alloc);
        if (e && !RAY_IS_ERR(e)) { if (!alloc) ray_retain(e); return e; }
        if (e && alloc) ray_release(e);   /* allocated error: generic fallback */
    }
    ray_t* ia = ray_i64(i);
    ray_t* e  = ray_at_fn(v, ia);   /* owned */
    ray_release(ia);
    return e;
}

/* Apply f to k args (borrowed).  1/2 route via call_fn1/2 (carrier-aware);
 * k>=3 via the noun dispatcher (lambda/proj carriers) or a native vary. */
ray_t* q_call_n(ray_t* f, ray_t** a, int64_t k) {
    if (k == 1) return call_fn1(f, a[0]);
    if (k == 2) return call_fn2(f, a[0], a[1]);
    if (f && f->type == RAY_VARY) return ((ray_vary_fn)(uintptr_t)f->i64)(a, k);
    ray_t* r = q_apply_noun(f, a, k);
    if (r) return r;
    return ray_error("rank", "each-both: cannot apply to %lld args", (long long)k);
}

/* ---- each-both  x f'y ------------------------------------------------------ */
static ray_t* q_eachboth_apply(ray_t* f, ray_t** ops, int64_t k);

static int q_op_is_dict(ray_t* v) { return v && v->type == RAY_DICT; }

/* dict each-both (binary): keys come from the dict side; a non-dict operand
 * pairs with the dict's VALUES (kdb: d+'10 20 conforms values, keys kept).
 * Mixed operands previously dispatched ray_dict_vals(non-dict)=NULL straight
 * into a crash (codex round-2 P1). */
static ray_t* q_eachboth_dict(ray_t* f, ray_t* x, ray_t* y) {
    ray_t* kd = q_op_is_dict(x) ? x : y;     /* key donor */
    ray_t* xk = ray_dict_keys(kd);           /* borrowed */
    if (!xk) return ray_error("type", "each-both: malformed dictionary");
    ray_t* ops[2] = { q_op_is_dict(x) ? ray_dict_vals(x) : x,
                      q_op_is_dict(y) ? ray_dict_vals(y) : y };
    ray_t* rv = q_eachboth_apply(f, ops, 2);
    if (!rv || RAY_IS_ERR(rv)) return rv;
    ray_retain(xk);
    return ray_dict_new(xk, rv);             /* consumes keys + vals */
}
/* An each-both operand "conforms as an atom" (broadcast, not zipped) when it
 * is a true atom OR a callable FUNCTION VALUE.  A function has no depth (kdb
 * `count` on a lambda is 'type), so `.'` / `f'` broadcast the function operand
 * against the list of argument-lists — `{x+y} .' (1 2;3 4)` == 3 7 — instead
 * of mis-zipping a lambda/projection CARRIER (a RAY_LIST) against the data. */
static int q_op_is_atom(ray_t* v) {
    if (!v || q_op_is_dict(v)) return 0;
    return ray_is_atom(v) || q_is_fn_value(v);
}

static ray_t* q_eachboth_apply(ray_t* f, ray_t** ops, int64_t k) {
    int any_dict = 0, all_atom = 1;
    for (int64_t j = 0; j < k; j++) {
        if (q_op_is_dict(ops[j])) any_dict = 1;
        if (!q_op_is_atom(ops[j])) all_atom = 0;
    }
    if (any_dict && k == 2) return q_eachboth_dict(f, ops[0], ops[1]);
    if (all_atom) return q_call_n(f, ops, k);      /* all atoms -> one result */

    int64_t L = -1;
    for (int64_t j = 0; j < k; j++) {
        if (!q_op_is_atom(ops[j])) {
            int64_t lj = ray_len(ops[j]);
            if (L < 0) L = lj;
            else if (L != lj) return ray_error("length", "each-both: length mismatch");
        }
    }
    if (L < 0) L = 1;
    ray_t* out = ray_list_new(L > 0 ? L : 1);
    for (int64_t i = 0; i < L; i++) {
        ray_t* a[16]; uint32_t owned = 0;
        int64_t kk = k < 16 ? k : 16;
        for (int64_t j = 0; j < kk; j++) {
            if (!q_op_is_atom(ops[j])) { a[j] = q_elem_at(ops[j], i); owned |= (1u << j); }
            else                       { a[j] = ops[j]; }   /* atom broadcast */
        }
        /* Force BEFORE releasing the operands the lazy may borrow (r2 review;
         * the ray_map_fn discipline). */
        ray_t* r = q_force(q_call_n(f, a, kk));
        for (int64_t j = 0; j < kk; j++) if (owned & (1u << j)) ray_release(a[j]);
        if (!r || RAY_IS_ERR(r)) { ray_release(out); return r ? r : ray_error("type", NULL); }
        out = ray_list_append(out, r);
        ray_release(r);
    }
    ray_t* c = q_collapse_list(out);
    ray_release(out);
    return c;
}

/* internal each-both value: args[0]=f, args[1..] operands. */
ray_t* q_eachboth_wrap(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("rank", "each-both: needs a function and operand");
    return q_eachboth_apply(args[0], args + 1, n - 1);
}

/* ---- each-prior  (f':)x  /  s f':x ---------------------------------------- */
/* Seed for the UNARY form: operator identity if known to q, else `first 0#x`
 * (typed null of the argument's element type) — ref/maps.md 259-279. */
static ray_t* q_prior_seed(ray_t* f, ray_t* x) {
    q_provenance_t pv;
    if (q_registry_provenance(f, &pv) && pv.spelling && pv.spelling[0] &&
        pv.spelling[1] == '\0') {
        char g = pv.spelling[0];
        if (g == '+' || g == '-') return ray_i64(0);       /* I(+) = I(-) = 0 */
        if (g == '*' || g == '%') return ray_i64(1);       /* I(*) = I(%) = 1 */
        if (g == ',')             return ray_list_new(0);  /* I(,) = ()       */
    }
    if (ray_is_vec(x))    return ray_typed_null((int8_t)(-x->type));
    if (x && ray_is_atom(x) && x->type != RAY_LIST)
        return ray_typed_null(x->type);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* result[0]=f(x0,seed); result[i]=f(xi,x[i-1]).  Borrows f/seed/x. */
static ray_t* q_prior_over_vec(ray_t* f, ray_t* seed, ray_t* x) {
    if (!x || (!ray_is_vec(x) && x->type != RAY_LIST)) {
        if (x && ray_is_atom(x)) return q_force(call_fn2(f, x, seed));
        return ray_error("type", "each-prior: expected a list");
    }
    int64_t L = ray_len(x);
    ray_t* out = ray_list_new(L > 0 ? L : 1);
    ray_t* prev = seed; int prev_owned = 0;
    for (int64_t i = 0; i < L; i++) {
        ray_t* cur = q_elem_at(x, i);            /* owned */
        ray_t* r   = q_force(call_fn2(f, cur, prev));
        if (prev_owned) ray_release(prev);
        if (!r || RAY_IS_ERR(r)) { ray_release(cur); ray_release(out); return r ? r : ray_error("type", NULL); }
        out = ray_list_append(out, r);
        ray_release(r);
        prev = cur; prev_owned = 1;               /* current becomes next prior */
    }
    if (prev_owned) ray_release(prev);
    ray_t* c = q_collapse_list(out);
    ray_release(out);
    return c;
}

/* internal each-prior value: n==2 (f':)x unary; n==3 s f':x seeded. */
ray_t* q_prior_wrap(ray_t** args, int64_t n) {
    if (n < 2 || n > 3) return ray_error("rank", "each-prior: bad arity");
    ray_t* f = args[0];
    ray_t* x = args[n - 1];
    ray_t* seed; int seed_owned = 0;
    if (n == 3) seed = args[1];
    else { seed = q_prior_seed(f, x); seed_owned = 1; }
    ray_t* r;
    if (x && x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);
        ray_t* rv = q_prior_over_vec(f, seed, ray_dict_vals(x));
        if (!rv || RAY_IS_ERR(rv)) r = rv;
        else { ray_retain(k); r = ray_dict_new(k, rv); }
    } else {
        r = q_prior_over_vec(f, seed, x);
    }
    if (seed_owned) ray_release(seed);
    return r;
}

/* ---- over / scan  (converge, do, while, and reduce) ----------------------- */

/* The ONE truthiness home: the `if`/`while` test and the `f/`/`f\` while-adverb
 * condition (`do` takes a COUNT, not a truthiness, and keeps its own gate).
 * Owner ruling 2026-07-15, the authority where the docs are silent on the error
 * codes: materialize -> exclude float/real -> cast with the SAME fn `"b"$` uses
 * -> boolean ATOM = 1b, else 'type.  Deciding via q_cast_to keeps ONE type
 * judgment; the ATOM check subsumes an arity gate ("b"$1 2 -> 11b, a vector).
 * float/real go BEFORE the cast: the cast accepts them ("b"$1.5 -> 1b) but
 * ref/if.md:20 / ref/while.md:21 require "an atom of integral type".
 * CONSUMES v (ray_lazy_materialize releases its arg and passes a non-lazy one
 * through, so one release covers both); an owned error lands in *err. */
static int q_truth(ray_t* v, ray_t** err) {
    *err = NULL;
    if (!v) { *err = ray_error("type", NULL); return 0; }
    v = ray_lazy_materialize(v);
    if (RAY_IS_ERR(v)) { *err = v; return 0; }
    int8_t t = v->type < 0 ? (int8_t)-v->type : v->type;
    if (t == RAY_F64 || t == RAY_F32) { ray_release(v); *err = ray_error("type", NULL); return 0; }
    ray_t* b = q_cast_to(RAY_BOOL, v);
    ray_release(v);
    if (!b || RAY_IS_ERR(b)) { *err = b ? b : ray_error("type", NULL); return 0; }
    if (b->type != -RAY_BOOL) { ray_release(b); *err = ray_error("type", NULL); return 0; }
    int go = b->b8 != 0;
    ray_release(b);
    return go;
}

/* Converge: apply f until the result matches the previous OR the initial x.
 * collect=1 keeps every step (scan), else returns the last (over). */
static ray_t* q_converge(ray_t* f, ray_t* x, int collect) {
    ray_t* first = x; ray_retain(first);
    ray_t* cur   = x; ray_retain(cur);
    ray_t* acc   = collect ? ray_list_new(0) : NULL;
    if (collect) acc = ray_list_append(acc, cur);
    int64_t guard = 0;
    for (;;) {
        ray_t* nxt = q_force(call_fn1(f, cur));
        if (!nxt || RAY_IS_ERR(nxt)) {
            ray_release(first); ray_release(cur); if (acc) ray_release(acc);
            return nxt ? nxt : ray_error("type", NULL);
        }
        if (q_values_match(nxt, cur) || q_values_match(nxt, first)) { ray_release(nxt); break; }
        if (collect) acc = ray_list_append(acc, nxt);
        ray_release(cur);
        cur = nxt;
        /* kdb has NO cap here (`(not/) 42` hangs until interrupt — doc-pinned
         * "never returns!"); the cap is openq's deliberate divergence.  1e6 keeps
         * ~4 orders of magnitude of headroom over any real fixpoint (tens of
         * iterations) while making the pathological oscillators cheap: at 1e8 the
         * accumulators suite burned ~50s CPU under ASan to produce this same
         * 'limit (2026-07-09). */
        if (++guard > 1000000) {
            ray_release(first); ray_release(cur); if (acc) ray_release(acc);
            return ray_error("limit", "converge: no fixed point");
        }
    }
    ray_release(first);
    if (collect) { ray_release(cur); ray_t* c = q_collapse_list(acc); ray_release(acc); return c; }
    return cur;
}

/* Do: apply f exactly cnt times to x (n f/x).  collect keeps each step. */
static ray_t* q_ntimes(ray_t* f, int64_t cnt, ray_t* x, int collect) {
    if (cnt < 0) cnt = 0;
    ray_t* cur = x; ray_retain(cur);
    ray_t* acc = NULL;
    if (collect) { acc = ray_list_new(cnt + 1); acc = ray_list_append(acc, cur); }
    for (int64_t i = 0; i < cnt; i++) {
        ray_t* nxt = q_force(call_fn1(f, cur));
        ray_release(cur);
        if (!nxt || RAY_IS_ERR(nxt)) { if (acc) ray_release(acc); return nxt ? nxt : ray_error("type", NULL); }
        cur = nxt;
        if (collect) acc = ray_list_append(acc, cur);
    }
    if (collect) { ray_release(cur); ray_t* c = q_collapse_list(acc); ray_release(acc); return c; }
    return cur;
}

/* While: apply f while test(cur) holds (test f/x).  collect keeps each step. */
static ray_t* q_while(ray_t* f, ray_t* test, ray_t* x, int collect) {
    ray_t* cur = x; ray_retain(cur);
    ray_t* acc = NULL;
    if (collect) { acc = ray_list_new(0); acc = ray_list_append(acc, cur); }
    int64_t guard = 0;
    for (;;) {
        ray_t* terr = NULL;
        int go = q_truth(call_fn1(test, cur), &terr);   /* consumes the test result */
        if (terr) { ray_release(cur); if (acc) ray_release(acc); return terr; }
        if (!go) break;
        ray_t* nxt = q_force(call_fn1(f, cur));
        ray_release(cur);
        if (!nxt || RAY_IS_ERR(nxt)) { if (acc) ray_release(acc); return nxt ? nxt : ray_error("type", NULL); }
        cur = nxt;
        if (collect) acc = ray_list_append(acc, cur);
        if (++guard > 100000000) { ray_release(cur); if (acc) ray_release(acc); return ray_error("limit", "while: no termination"); }
    }
    if (collect) { ray_release(cur); ray_t* c = q_collapse_list(acc); ray_release(acc); return c; }
    return cur;
}

/* Seeded scan  x f\y  (kept minimal — ray_scan_fn has no seed slot).
 * collapse_steps: the identity-seeded (f\)x path collapses EACH step —
 * openq's `(),atom` join stays a boxed list where kdb promotes to a typed
 * vector, so the running acc must re-collapse to keep (,\)2 3 4 -> ,2 / 2 3 /
 * 2 3 4 (ref/accumulators.md:263-267).  The user-seeded x f\y path keeps its
 * uncollapsed steps (banked behavior). */
static ray_t* q_seeded_scan(ray_t* f, ray_t* seed, ray_t* x, int collapse_steps) {
    if (!x || (!ray_is_vec(x) && x->type != RAY_LIST)) return ray_error("type", "scan: expected a list");
    int64_t L = ray_len(x);
    ray_t* out = ray_list_new(L > 0 ? L : 1);
    ray_t* acc = seed; ray_retain(acc);
    for (int64_t i = 0; i < L; i++) {
        ray_t* cur = q_elem_at(x, i);
        ray_t* nxt = q_force(call_fn2(f, acc, cur));
        ray_release(acc); ray_release(cur);
        if (!nxt || RAY_IS_ERR(nxt)) { ray_release(out); return nxt ? nxt : ray_error("type", NULL); }
        if (collapse_steps && nxt->type == RAY_LIST) {
            ray_t* cn = q_collapse_list(nxt);
            ray_release(nxt);
            if (!cn || RAY_IS_ERR(cn)) { ray_release(out); return cn ? cn : ray_error("type", NULL); }
            nxt = cn;
        }
        out = ray_list_append(out, nxt);
        acc = nxt; ray_retain(acc);
    }
    ray_release(acc);
    ray_t* c = q_collapse_list(out); ray_release(out); return c;
}

/* Identity element I for the ACCUMULATOR context — resolved from the
 * MANIFEST (q_ops_acc_identity; rule 3: the manifest owns per-verb facts).
 * (Each-prior keeps its OWN maps.md-cited seed set in q_prior_seed.) */
static ray_t* q_acc_identity(ray_t* f) {
    q_provenance_t pv;
    if (!q_registry_provenance(f, &pv)) return NULL;
    return q_ops_acc_identity(pv.spelling);
}

/* Over/Scan on an EMPTY right argument (ref/accumulators.md:396-399 Scan,
 * 408-437 Over): known identity -> I; a LIST/vector VALUE (pinned for Over
 * only: 1 0 3h/[til 0] -> 5h) -> empty of the value's type; else (). */
static ray_t* q_acc_empty(ray_t* f, int scan) {
    ray_t* ident = q_acc_identity(f);
    if (ident) return ident;
    if (!scan && f) {
        if (f->type == RAY_LIST) return ray_list_new(0);
        if (ray_is_vec(f))
            return (f->type == RAY_SYM) ? ray_sym_vec_new(RAY_SYM_W64, 0)
                                        : ray_vec_new(f->type, 0);
    }
    return ray_list_new(0);
}

static int q_acc_is_coll(ray_t* x) {
    return x && (ray_is_vec(x) || x->type == RAY_LIST);
}

/* `/` over — reduce / converge / do / while by operand shape and f rank. */
ray_t* q_over_wrap(ray_t** args, int64_t n) {
    ray_t* f = args[0];
    int rank = q_fn_rank(f);
    if (n == 2) {
        ray_t* x = args[1];
        if (rank == 1) return q_converge(f, x, 0);
        if (q_acc_is_coll(x) && ray_len(x) == 0) return q_acc_empty(f, 0);
        if (x && ray_is_atom(x) && x->type != RAY_LIST) {
            /* atom right argument with known I: one evaluation f(I, x) —
             * (,/)42 -> ,42 (raze == ,/ and raze 42 -> ,42, ref/raze.md) */
            ray_t* ident = q_acc_identity(f);
            if (ident) {
                ray_t* r = q_force(call_fn2(f, ident, x));
                ray_release(ident);
                return r;
            }
        }
        ray_t* fa[2] = { f, x };
        return q_force(ray_fold_fn(fa, 2));              /* reduce */
    }
    if (n == 3) {
        ray_t* a = args[1], *x = args[2];
        if (q_is_fn_value(a))  return q_while(f, a, x, 0);
        if (rank == 1)         return q_ntimes(f, as_i64(a), x, 0);
        ray_t* fa[3] = { f, a, x };
        return q_force(ray_fold_fn(fa, 3));              /* seeded reduce */
    }
    return ray_error("rank", "over: bad arity");
}

/* `\` scan — like over but every step is retained. */
ray_t* q_scan_wrap(ray_t** args, int64_t n) {
    ray_t* f = args[0];
    int rank = q_fn_rank(f);
    if (n == 2) {
        ray_t* x = args[1];
        if (rank == 1) return q_converge(f, x, 1);
        if (q_acc_is_coll(x) && ray_len(x) == 0) return q_acc_empty(f, 1);
        {   /* unary-seed rule (ref/accumulators.md:261-267): a known I is the
             * left argument of the FIRST evaluation — (,\)2 3 4 -> ,2 / 2 3 /
             * 2 3 4.  Applied for `,` only: seeding + and * is
             * value-identical but would promote the first step's type. */
            ray_t* ident = q_acc_identity(f);
            if (ident) {
                if (ident->type == RAY_LIST && q_acc_is_coll(x)) {
                    ray_t* r = q_seeded_scan(f, ident, x, 1);
                    ray_release(ident);
                    return r;
                }
                ray_release(ident);
            }
        }
        ray_t* fa[2] = { f, x };
        ray_t* r = ray_scan_fn(fa, 2);
        if (!r || RAY_IS_ERR(r)) return r;
        if (r->type == RAY_LIST) {
            /* engine scan has no lazy discipline — force each retained step
             * in place (rc==1 result); propagate a materialize error with the
             * atomic_map trim pattern (free exactly the initialized prefix) */
            ray_t** e = (ray_t**)ray_data(r);
            for (int64_t i = 0; i < ray_len(r); i++) {
                if (!e[i] || !ray_is_lazy(e[i])) continue;
                e[i] = ray_lazy_materialize(e[i]);
                if (!e[i] || RAY_IS_ERR(e[i])) {
                    ray_t* err = e[i] ? e[i] : ray_error("type", NULL);
                    r->len = i;
                    ray_release(r);
                    return err;
                }
            }
        }
        ray_t* c = q_collapse_list(r); ray_release(r); return c;
    }
    if (n == 3) {
        ray_t* a = args[1], *x = args[2];
        if (q_is_fn_value(a))  return q_while(f, a, x, 1);
        if (rank == 1)         return q_ntimes(f, as_i64(a), x, 1);
        return q_seeded_scan(f, a, x, 0);
    }
    return ray_error("rank", "scan: bad arity");
}

/* over/scan/prior keyword dyadic wrappers `f over x` / `f scan x` /
 * `f prior x` — delegate to the n==2 core (peach reuses q_each_wrap). */
ray_t* q_over_kw(ray_t* f, ray_t* x)  { ray_t* a[2] = { f, x }; return q_over_wrap(a, 2); }
ray_t* q_scan_kw(ray_t* f, ray_t* x)  { ray_t* a[2] = { f, x }; return q_scan_wrap(a, 2); }
ray_t* q_prior_kw(ray_t* f, ray_t* x) { ray_t* a[2] = { f, x }; return q_prior_wrap(a, 2); }

/* Build a BINARY derived-verb carrier at EVAL time: `hof` with `f` bound in
 * slot 0 and two data operands open.  Used to lower `(f/:)` / `(f\:)` when the
 * operand f is an expression (a lambda) that must be EVALUATED to a value
 * first — a lower-time q_proj would capture the raw `(q.fn …)` tree.  `x f/: y`
 * then == map-right(f;x;y), which lets a stacked outer adverb (`f/:\:`) drive
 * it through map-left. */
ray_t* q_mkderiv2(ray_t* hof, ray_t* f) {
    ray_t* args[3] = { f, NULL, NULL };
    return q_proj_new(hof, args, 3, 0x6u, 2);
}

/* q.mkopproj — build a projection carrier over an `@`/`.` operator with an
 * ELIDED argument (`@[count;;-1]`, `type @[;;0h]`), at EVAL time so the bound
 * (non-hole) args are already VALUES: a name-ref `count` or a lambda literal
 * `{x+1}` has been evaluated before it is bound.  This is what distinguishes
 * an elision (project) from an explicit `::` (amend/trap data) — the parser
 * only lowers a genuine bracket elision here (q_parse.c ql_project).  Args:
 *   [0] base — the @/. VARY value (the carrier's base)
 *   [1] n    — total slot count
 *   [2] mask — hole bitmask over slots 0..n-1
 *   [3..k)   — the non-hole bound values, in slot order
 * Returns an owned .q.proj carrier (kdb 104h). */
ray_t* q_mkopproj(ray_t** args, int64_t k) {
    if (k < 3) return ray_error("rank", "q.mkopproj: need base, n, mask");
    ray_t* base = args[0];
    int64_t n; int64_t m;
    if (!q_idx_int(args[1], &n) || !q_idx_int(args[2], &m))
        return ray_error("type", "q.mkopproj: n/mask");
    if (n < 1 || n > 60) return ray_error("rank", "q.mkopproj: bad slot count");
    uint64_t mask = (uint64_t)m;
    ray_t* slots[64];
    int64_t j = 3; int holes = 0;
    for (int64_t i = 0; i < n; i++) {
        if (mask & (1ull << i)) { slots[i] = NULL; holes++; }
        else                    { slots[i] = (j < k) ? args[j++] : NULL; }
    }
    if (holes == 0) return ray_error("rank", "q.mkopproj: no holes");
    return q_proj_new(base, slots, n, mask, holes);   /* retains base + slots */
}

ray_t* g_scan_value     = NULL;
ray_t* g_over_value     = NULL;
ray_t* g_eachboth_value = NULL;
ray_t* g_prior_value    = NULL;
ray_t* g_mkderiv2_value = NULL;
ray_t* g_mkopproj_value = NULL;

ray_t* q_registry_scan_value(void)     { return g_scan_value;     }  /* borrowed */
ray_t* q_registry_over_value(void)     { return g_over_value;     }  /* borrowed */
ray_t* q_registry_eachboth_value(void) { return g_eachboth_value; }  /* borrowed */
ray_t* q_registry_prior_value(void)    { return g_prior_value;    }  /* borrowed */
ray_t* q_registry_mkderiv2_value(void) { return g_mkderiv2_value; }  /* borrowed */
ray_t* q_registry_mkopproj_value(void) { return g_mkopproj_value; }  /* borrowed */

ray_t* g_list_value   = NULL;
ray_t* g_table_value  = NULL;
ray_t* g_keyed_table_value = NULL;
ray_t* g_select_value = NULL;
ray_t* g_delete_value = NULL;   /* q.delete: string `delete` statement executor */
ray_t* g_exec_value   = NULL;   /* q.exec:   string `exec`   statement executor */
ray_t* g_compose_value = NULL;

/* q `'[f;g;…]` compose builder — a normal VARY (args are the resolved function
 * VALUES): boxes them into a Q_DERIV_COMPOSE carrier (q_deriv.c). */
ray_t* q_compose_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("rank", "': compose needs at least one function");
    return q_compose_new(args, n);
}
ray_t* g_funsql_select_value = NULL;
ray_t* g_funsql_bang_value   = NULL;
ray_t* g_lambda_value        = NULL;

/* q.fn — SPECIAL FORM behind every q lambda literal.  q_lower rewrites the
 * parser's `{`-marker node to (q.fn src params body...); at eval this
 * delegates lambda creation to the base env `fn` form (same params/body
 * calling convention) and wraps the resulting RAY_LAMBDA in the 100h
 * .q.lambda carrier that carries q valence + verbatim source for display.
 * kdb caps lambdas at 8 arguments -> 'params — signalled HERE (not at parse:
 * qdoc error rows only match lower/eval errors). */
ray_t* q_fn_make(ray_t** args, int64_t n) {
    if (n < 3 || !args[0] || args[0]->type != -RAY_STR ||
        !args[1] || args[1]->type != RAY_SYM)
        return ray_error("type", "q.fn: malformed lambda node");
    int64_t rank = ray_len(args[1]);
    if (rank > 8)
        return ray_error("params", "'params: lambdas take at most 8 arguments");
    ray_t* fnv = ray_env_get(ray_sym_intern("fn", 2));       /* borrowed */
    if (!fnv || fnv->type != RAY_VARY)
        return ray_error("type", "q.fn: base fn form unavailable");
    ray_t* lam = ((ray_vary_fn)(uintptr_t)fnv->i64)(args + 1, n - 1);
    if (!lam || RAY_IS_ERR(lam)) return lam;
    ray_t* c = q_lambda_carrier_new(lam, (int)rank, args[0]);
    ray_release(lam);                       /* carrier holds its own ref */
    return c;
}

ray_t* q_registry_lambda_value(void) { return g_lambda_value; }  /* borrowed */

ray_t* g_ret_value = NULL;
ray_t* g_sig_value = NULL;
_Thread_local ray_t* g_qret_payload = NULL;
_Thread_local ray_t* g_qsig_payload = NULL;

/* `:x` early return (basics/function-notation.md#explicit-return).  The body
 * must unwind NOW: eval aborts a lambda body on any RAY_ERROR, so we ride
 * the error path with the reserved class "q.ret" and stash the payload in a
 * thread-local for the innermost q_lambda_apply to take. */
ray_t* q_ret_fn(ray_t* x) {
    if (g_qret_payload) { ray_release(g_qret_payload); g_qret_payload = NULL; }
    if (x) ray_retain(x);
    g_qret_payload = x;
    return ray_error("q.ret", NULL);
}

ray_t* q_lambda_ret_take(void) {
    ray_t* v = g_qret_payload;      /* owned by the caller now */
    g_qret_payload = NULL;
    return v;
}

/* Full text of the most recent `'x` signal.  The ≤7-char error class in
 * err->sdata truncates, but kdb Trap hands the handler the WHOLE message, so
 * q_sig_fn stashes the untruncated text here (mirroring the q.ret payload).
 * Owned by the caller; NULL if the last error was not a q signal. */
ray_t* q_registry_sig_take(void) {
    ray_t* v = g_qsig_payload;
    g_qsig_payload = NULL;
    return v;
}

void q_registry_sig_clear(void) {
    if (g_qsig_payload) { ray_release(g_qsig_payload); g_qsig_payload = NULL; }
}

/* `'x` Signal (ref/signal.md): abort with error class = the sym spelling /
 * string text (ray_error copies, 7-char sdata cap — kdb's own classes are
 * short for the same reason).  The full untruncated text is stashed for Trap
 * (q_registry_sig_take). */
ray_t* q_sig_fn(ray_t* x) {
    char cls[8] = "signal";
    ray_t* full = NULL;                 /* owned full-text string, or NULL */
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        if (s) {
            size_t l = ray_str_len(s); size_t c = l > 7 ? 7 : l;
            memcpy(cls, ray_str_ptr(s), c); cls[c] = '\0';
            full = ray_str(ray_str_ptr(s), l);
            ray_release(s);
        }
    } else if (x) {
        const char* p; int64_t l;                     /* string / charv / char atom */
        if (q_text_bytes(x, &p, &l)) {
            size_t c = (size_t)l > 7 ? 7 : (size_t)l;
            memcpy(cls, p, c); cls[c] = '\0';
            full = ray_str(p, (size_t)l);
        }
    }
    if (g_qsig_payload) ray_release(g_qsig_payload);
    g_qsig_payload = full;              /* owned (or NULL) */
    return ray_error(cls, NULL);
}

ray_t* q_registry_ret_value(void) { return g_ret_value; }   /* borrowed */
ray_t* q_registry_sig_value(void) { return g_sig_value; }   /* borrowed */

/* ===== q imperative control constructs =====================================
 * The `;` statement sequence and the `if` / `do` / `while` control words are
 * SPECIAL FORMS (basics/control.md, ref/{if,do,while}.md): they receive their
 * statement args UNEVALUATED and drive evaluation themselves — lazy, strictly
 * left-to-right, with side effects PERSISTING.  Unlike a lambda body they do
 * NOT open a lexical scope: a `:` assignment inside amends the ENCLOSING frame
 * (q_lower already lowered it to set/let against that frame; "the brackets do
 * not create lexical scope").  `if`/`do`/`while` always return the generic
 * null; `;` returns its LAST statement's value.  This is the q-layer home for
 * the semantics (CLAUDE.md rule 4) — rayfall's own `if`/`do` differ (triadic
 * cond / scope-pushing progn) so they are NOT reused here. */
ray_t* g_seq_value   = NULL;
ray_t* g_if_value    = NULL;
ray_t* g_do_value    = NULL;
ray_t* g_while_value = NULL;

/* Evaluate an if/while test arg and decide it at the one truthiness home. */
static int q_ctl_truth(ray_t* arg, ray_t** err) {
    return q_truth(ray_eval(arg), err);   /* q_truth consumes the eval result */
}

/* Evaluate args[from..n) in order for their side effects, releasing each
 * result.  Returns an owned RAY_ERROR on the first failure, else NULL. */
static ray_t* q_ctl_run_body(ray_t** args, int64_t from, int64_t n) {
    for (int64_t i = from; i < n; i++) {
        ray_t* r = ray_eval(args[i]);
        if (RAY_IS_ERR(r)) return r;
        ray_release(r);
    }
    return NULL;
}

/* `s1; s2; …; sn` — evaluate each statement left-to-right (side effects
 * persist to the enclosing frame); the value is the LAST statement's. */
ray_t* q_seq_fn(ray_t** args, int64_t n) {
    ray_t* result = RAY_NULL_OBJ;
    for (int64_t i = 0; i < n; i++) {
        ray_release(result);                 /* RAY_NULL_OBJ release is a no-op */
        result = ray_eval(args[i]);
        if (RAY_IS_ERR(result)) return result;
    }
    return result;
}

/* `if[test; e1; …; en]` — evaluate the body once, in order, unless test is
 * zero; result is always the generic null (ref/if.md). */
ray_t* q_if_fn(ray_t** args, int64_t n) {
    if (n < 1) return RAY_NULL_OBJ;
    ray_t* err = NULL;
    int truthy = q_ctl_truth(args[0], &err);
    if (err) return err;
    if (truthy) { err = q_ctl_run_body(args, 1, n); if (err) return err; }
    return RAY_NULL_OBJ;
}

/* `do[count; e1; …; en]` — evaluate the body `count` times; result is always
 * the generic null (ref/do.md).  `count` is a non-negative integer atom. */
ray_t* q_do_fn(ray_t** args, int64_t n) {
    if (n < 1) return RAY_NULL_OBJ;
    ray_t* cnt = ray_eval(args[0]);
    if (RAY_IS_ERR(cnt)) return cnt;
    if (ray_is_lazy(cnt)) cnt = ray_lazy_materialize(cnt);
    if (RAY_IS_ERR(cnt)) return cnt;
    int64_t times;
    if (RAY_ATOM_IS_NULL(cnt) || !q_strict_i64(cnt, &times)) {
        ray_release(cnt);
        return ray_error("type", "do: n");
    }
    ray_release(cnt);
    if (times < 0) return ray_error("type", "do: n");
    for (int64_t k = 0; k < times; k++) {
        ray_t* err = q_ctl_run_body(args, 1, n);
        if (err) return err;
    }
    return RAY_NULL_OBJ;
}

/* `while[test; e1; …; en]` — re-evaluate test each pass; while non-zero run
 * the body in order; result is always the generic null (ref/while.md). */
ray_t* q_while_fn(ray_t** args, int64_t n) {
    if (n < 1) return RAY_NULL_OBJ;
    for (;;) {
        ray_t* err = NULL;
        int truthy = q_ctl_truth(args[0], &err);
        if (err) return err;
        if (!truthy) break;
        err = q_ctl_run_body(args, 1, n);
        if (err) return err;
    }
    return RAY_NULL_OBJ;
}

ray_t* q_registry_seq_value(void)   { return g_seq_value; }    /* borrowed */
ray_t* q_registry_if_value(void)    { return g_if_value; }     /* borrowed */
ray_t* q_registry_do_value(void)    { return g_do_value; }     /* borrowed */
ray_t* q_registry_while_value(void) { return g_while_value; }  /* borrowed */

ray_t* q_registry_funsql_select_value(void) { return g_funsql_select_value; }
ray_t* q_registry_funsql_bang_value(void)   { return g_funsql_bang_value; }

ray_t* q_registry_select_value(void) {
    return g_select_value;   /* borrowed; NULL before init */
}

ray_t* q_registry_delete_value(void) { return g_delete_value; }   /* borrowed; NULL before init */

ray_t* q_registry_exec_value(void) { return g_exec_value; }       /* borrowed; NULL before init */

ray_t* q_registry_compose_value(void) {
    return g_compose_value;  /* borrowed; NULL before init */
}

ray_t* q_registry_list_value(void) {
    return g_list_value;   /* borrowed; NULL before init */
}

ray_t* q_registry_keyed_table_value(void) {
    return g_keyed_table_value;  /* borrowed; NULL before init */
}

ray_t* q_registry_table_value(void) {
    return g_table_value;  /* borrowed; NULL before init */
}
