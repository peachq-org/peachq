/* q_apply — see q_apply.h.  The per-head semantic table (spec 2c, revised
 * after adversarial codex review):
 *
 *   104h carrier      -> deriv_apply (Task 5; claimed FIRST — a carrier IS
 *                        a RAY_LIST and must not fall into the gather arm)
 *   dict              -> direct ray_dict_get; on a miss, the typed null of
 *                        the dict's VALUE type (ray_at would hardcode 0Nl)
 *   table             -> q_table_at row gather (int atom/vector; misses
 *                        null-filled, char columns byte-permuted), declining
 *                        to ray_at for column access and everything else
 *   typed vec / list  -> gather: ray_at (already kdb null-fill for
 *                        out-of-range/negative), then collapse a boxed
 *                        multi-index result to the typed vector
 *   string atom       -> DECLINE (ray_at implements char-indexing, but the
 *                        string-as-sequence model is deferred — routing
 *                        strings through would commit it by accident)
 *   everything else   -> DECLINE (caller's historic error, byte-identical)
 */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_apply.h"
#include "qlang/q_registry_internal.h" /* q_registry.h + the split's shared surface (q_deriv_is_fn_value, q_deriv_call_n) */
#include "qlang/q_deriv.h"      /* q_deriv_kind_of (carrier arm, Task 5) */
#include "lang/eval.h"          /* ray_at_fn */
#include "lang/format.h"        /* ray_type_name — amend error messages */
#include "lang/internal.h"      /* call_lambda — 100h lambda-carrier application */
#include "qlang/q_handles.h"    /* q_handles_apply — the sole handle authority */
#include "qlang/net/q_ws.h"         /* q_ws_client_open — ws:// hsym-apply */
#include "qlang/net/q_http_client.h" /* q_http_client_raw — http:// hsym-apply */
#include "ops/ops.h"            /* ray_is_lazy / ray_lazy_materialize — DAG agg results */
#include <string.h>

/* one indexing step: v[idx].  ray_at null-fills out-of-range; a
 * collection index yields a boxed list -> collapse to the typed vector. */
static ray_t* gather(ray_t* v, ray_t* idx) {
    ray_t* r = ray_at_fn(v, idx);
    if (!r || RAY_IS_ERR(r)) return r;
    if (r->type == RAY_LIST) {
        ray_t* c = q_collapse_list(r);
        ray_release(r);
        return c;
    }
    return r;
}

/* dict arm — DIRECT lookup (not ray_at, whose miss path hardcodes 0Nl): a
 * hit returns the stored value; a miss returns the typed null of the dict's
 * VALUE type (generic null for boxed/mixed values).  A vector of keys does a
 * per-key loop with those same miss semantics, then collapses. */
/* the typed-null type for a dict's values: a typed vector gives its element
 * type; a homogeneous boxed list of atoms gives their shared type (rayfall's
 * dict constructor stores values as a boxed list); anything mixed -> 0
 * (generic null, kdb's behaviour for mixed-value dict misses). */
static int8_t dict_val_null_type(ray_t* vals) {
    if (!vals) return 0;
    if (ray_is_vec(vals)) return (int8_t)-vals->type;
    if (vals->type == RAY_LIST && ray_len(vals) > 0) {
        ray_t** e = (ray_t**)ray_data(vals);
        if (!e[0] || e[0]->type >= 0) return 0;
        int8_t t = e[0]->type;
        for (int64_t i = 1; i < ray_len(vals); i++)
            if (!e[i] || e[i]->type != t) return 0;
        return t;
    }
    return 0;
}

/* Keyed-table lookup: kt[key] — probe the key TABLE row-wise, return the
 * matching row of the value table as a column dict (q4m3 "a keyed table is a
 * dictionary from key records to value records").  Accepted index shapes:
 *   1 key column : an ATOM key                       kt[`A]  -> age| 36
 *   k key columns: a k-item LIST/vector composite    kt[(`Jo;`LA)]
 * A miss returns the null row (typed null per value column) — kdb kt[`Z] ->
 * `age| 0N`, never a silent empty.  Every other shape (vector of keys on a
 * single-key table = table result) is an explicit 'nyi so nothing falls
 * through to the plain-dict path with silently-wrong results. */
static ray_t* keyed_table_lookup(ray_t* d, ray_t* idx) {
    ray_t* ktab = ray_dict_keys(d);                  /* borrowed key TABLE */
    ray_t* vtab = ray_dict_vals(d);                  /* borrowed val TABLE */
    int64_t nk = ray_table_ncols(ktab);
    int64_t nr = ray_table_nrows(ktab);
    /* row-set lookup: kt[keytable] — one value row per keytbl row, misses
     * null-filled (ref/lj.md `y[select a,b from x]`; joins wave). */
    if (idx && idx->type == RAY_TABLE)
        return q_join_keyed_lookup_rows(d, idx);
    int composite = idx && (ray_is_vec(idx) || idx->type == RAY_LIST) &&
                    nk > 1 && ray_len(idx) == nk;
    if (!composite && !(idx && ray_is_atom(idx) && nk == 1))
        return ray_error("nyi", "keyed table: unsupported key shape (row-set lookup deferred)");
    int64_t hit = -1;
    for (int64_t r = 0; r < nr && hit < 0; r++) {
        int all = 1;
        for (int64_t c = 0; c < nk && all; c++) {
            ray_t* col = ray_table_get_col_idx(ktab, c);   /* borrowed */
            if (!col) return ray_error("type", "keyed table: malformed key table");
            ray_t* ra = ray_i64(r);
            ray_t* cell = ray_at_fn(col, ra);              /* owned */
            ray_release(ra);
            if (!cell || RAY_IS_ERR(cell)) return cell ? cell : ray_error("type", NULL);
            ray_t* want = NULL;                            /* owned iff composite */
            if (composite) {
                ray_t* ca = ray_i64(c);
                want = ray_at_fn(idx, ca);
                ray_release(ca);
                if (!want || RAY_IS_ERR(want)) { ray_release(cell); return want ? want : ray_error("type", NULL); }
            }
            all = atom_eq(cell, composite ? want : idx);
            ray_release(cell);
            if (want) ray_release(want);
        }
        if (all) hit = r;
    }
    /* hit -> that row of the value table; miss -> the typed all-null row.
     * Both are q_table_row_at (the universal row-dict arm, ops/q_table.c):
     * a miss is just an out-of-range row. */
    return q_table_row_at(vtab, hit);
}

static ray_t* dict_lookup(ray_t* d, ray_t* idx) {
    if (q_table_is_keyed(d)) return keyed_table_lookup(d, idx);
    ray_t* vals = ray_dict_vals(d);                  /* borrowed accessor */
    int8_t vt = dict_val_null_type(vals);

    if (ray_is_vec(idx) || idx->type == RAY_LIST) {
        int64_t kn = ray_len(idx);
        ray_t* out = ray_list_new(kn > 0 ? kn : 1);
        for (int64_t i = 0; i < kn; i++) {
            ray_t* i_atom = ray_i64(i);
            ray_t* k = ray_at_fn(idx, i_atom);
            ray_release(i_atom);
            if (!k || RAY_IS_ERR(k)) { ray_release(out); return k; }
            ray_t* v = ray_dict_get(d, k);
            ray_release(k);
            if (!v) v = vt ? ray_typed_null(vt)
                           : (ray_retain(RAY_NULL_OBJ), RAY_NULL_OBJ);
            out = ray_list_append(out, v);
            ray_release(v);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }

    ray_t* v = ray_dict_get(d, idx);                 /* owned, or C NULL */
    if (v) return v;
    return vt ? ray_typed_null(vt)
              : (ray_retain(RAY_NULL_OBJ), RAY_NULL_OBJ);
}

/* Apply a 104h derived-verb carrier: positional assembly = the carrier's
 * bound slots with holes (hole_mask bits) filled left-to-right from `args`;
 * EXTRA supplied args append after the assembled list — that is what makes
 * seeded over work through a name: f:(+/); f[100;v] -> fold(F, 100, v).
 * UNARY/BINARY bases go through call_fn1/call_fn2 so a carrier call behaves
 * exactly like the tree-walk call of the same base (atomic broadcast).
 * Args/carrier borrowed; returns owned (or an owned error). */
static ray_t* deriv_apply(ray_t* carrier, ray_t* const* args, int64_t n) {
    q_deriv_kind k = q_deriv_kind_of(carrier);
    ray_t* base = q_deriv_base(carrier);            /* borrowed */
    if (!base) return NULL;
    ray_t* call[66];
    int64_t cn = 0;
    if (k == Q_DERIV_PROJ) {
        uint64_t mask = q_deriv_hole_mask(carrier);
        int64_t slots = ray_len(carrier) - 4;
        ray_t** e = (ray_t**)ray_data(carrier);
        int64_t supplied = 0;
        for (int64_t i = 0; i < slots && cn < 64; i++) {
            if (mask & (1ull << i)) {
                if (supplied >= n)
                    return ray_error("rank", "projection: missing argument %lld",
                                     (long long)(i + 1));
                call[cn++] = args[supplied++];
            } else {
                call[cn++] = e[4 + i];
            }
        }
        for (; supplied < n && cn < 64; supplied++) call[cn++] = args[supplied];
    } else if (k == Q_DERIV_MONAD) {
        if (n != 1)
            return ray_error("rank", "monadic verb applied to %lld args", (long long)n);
        call[cn++] = args[0];
    } else {
        return NULL;
    }
    switch (base->type) {
    case RAY_UNARY:
        if (cn != 1) return ray_error("rank", "derived verb: expected 1 arg, got %lld", (long long)cn);
        return call_fn1(base, call[0]);
    case RAY_BINARY:
        if (cn != 2) return ray_error("rank", "derived verb: expected 2 args, got %lld", (long long)cn);
        return call_fn2(base, call[0], call[1]);
    case RAY_VARY:
        return ((ray_vary_fn)(uintptr_t)base->i64)(call, cn);
    default:
        /* a carrier base (projected lambda, projection-of-projection):
         * route the assembled args back through the noun dispatcher */
        if (base->type == RAY_LIST && q_deriv_kind_of(base) != Q_DERIV_NONE)
            return q_apply_noun(base, call, cn);
        return NULL;
    }
}

/* Apply a 100h lambda carrier with q semantics.  Base call_lambda enforces
 * exact arity (class 'arity); q wants 'rank plus kdb's projection-on-elision:
 *   rank 0:  `f[]` arrives as ONE `::` arg (cases.tsv:43) — ignore it and
 *            call; kdb ignores even a real value there (q4m3: const42[98.6]).
 *   rank 1:  one arg always applies (kdb `f[]` == `f[::]` APPLIES a unary).
 *   rank>=2: `::` args are elision holes; underapplication pads trailing
 *            holes; any hole -> a .q.proj carrier over THIS carrier (display
 *            and re-application both read the base); full -> call_lambda. */
/* call_lambda + interception of the reserved "q.ret" class: a `:x` statement
 * unwinds the body via the error path with its payload stashed thread-local
 * (q_registry.c); the innermost lambda application turns it back into a
 * normal result here. */
static ray_t* i_call_lambda(ray_t* lam, ray_t** args, int64_t n) {
    /* Drop any stale payload first (a q.ret swallowed by a user trap), so
     * the class check below can never be spoofed into returning old data. */
    ray_t* stale = q_deriv_ret_take();
    if (stale) ray_release(stale);
    ray_t* r = call_lambda(lam, args, n);
    if (r && RAY_IS_ERR(r) && r->slen == 5 && memcmp(r->sdata, "q.ret", 5) == 0) {
        /* genuine `:x` always stashed a payload (a bare `:` stashes `::`);
         * a user-forged '"q.ret" signal has none and propagates as an error */
        ray_t* v = q_deriv_ret_take();               /* owned, or NULL */
        if (v) { ray_release(r); return v; }
    }
    return r;
}

static ray_t* lambda_apply(ray_t* carrier, ray_t** args, int64_t n) {
    int     r   = q_deriv_valence(carrier);
    ray_t*  lam = q_deriv_base(carrier);              /* borrowed */
    if (!lam) return ray_error("type", "lambda carrier: missing base");
    if (r == 0) {
        if (n <= 1) return i_call_lambda(lam, NULL, 0);
        return ray_error("rank", "lambda: rank 0 applied to %lld args", (long long)n);
    }
    if (n < 1) return ray_error("rank", "lambda: no arguments");
    if (n > r)
        return ray_error("rank", "lambda: rank %d applied to %lld args",
                         r, (long long)n);
    if (r == 1) return i_call_lambda(lam, args, 1);
    if (r > 64) return ray_error("limit", "lambda: rank too large");
    uint64_t mask  = 0;
    ray_t*   slots[64];
    int      holes = 0;
    for (int i = 0; i < r; i++) {
        ray_t* a = (i < (int)n) ? args[i] : NULL;
        if (!a || RAY_IS_NULL(a)) { mask |= 1ull << i; slots[i] = NULL; holes++; }
        else slots[i] = a;
    }
    if (holes == 0) return i_call_lambda(lam, args, n);
    return q_proj_new(carrier, slots, r, mask, holes);
}

/* Apply any function VALUE to args: bare builtins direct, everything else
 * (carriers, lambdas, VARY, projections) through the noun dispatcher. */
static ray_t* apply_fn(ray_t* fn, ray_t** args, int64_t n) {
    if (fn && fn->type == RAY_UNARY && n == 1)
        return ((ray_unary_fn)(uintptr_t)fn->i64)(args[0]);
    if (fn && fn->type == RAY_BINARY && n == 2)
        return ((ray_binary_fn)(uintptr_t)fn->i64)(args[0], args[1]);
    return q_apply_noun(fn, args, n);
}

/* Apply a composition carrier `'[f;g;…]`: the innermost (rightmost) function
 * consumes all supplied args, then each function to its left is applied
 * monadically to the running result.  `'[f;g][a;b]` == `f g[a;b]`. */
static ray_t* compose_apply(ray_t* carrier, ray_t** args, int64_t n) {
    int64_t nf = q_deriv_compose_count(carrier);
    if (nf < 1) return ray_error("rank", "compose: empty composition");
    ray_t* acc = apply_fn(q_deriv_compose_fn_at(carrier, nf - 1), args, n);
    if (!acc || RAY_IS_ERR(acc)) return acc;
    for (int64_t i = nf - 2; i >= 0; i--) {
        ray_t* a1[1] = { acc };
        ray_t* next = apply_fn(q_deriv_compose_fn_at(carrier, i), a1, 1);
        ray_release(acc);
        if (!next || RAY_IS_ERR(next)) return next;
        acc = next;
    }
    return acc;
}

/* ===== Amend / Apply / Trap — @ and . ternary+ forms (wave-3) ==============
 * ref/amend.md + ref/apply.md.  q_at_wrap / q_dot_wrap are VARY wrappers;
 * the rayfall VARY dispatch (eval.c) evaluates every bracket arg and
 * short-circuits an arg-eval error BEFORE calling us — exactly kdb Trap's
 * "errors in fx are not caught" rule.  Dispatch on arg count and first-arg
 * kind: a callable first arg -> Trap; a data first arg -> Amend; n==2 keeps
 * the historic Apply/Index path.  Amend is copy-on-write: explode to a
 * fresh rc==1 boxed list, run a sequential single-path engine (repeated
 * indices and cross-sections both decompose into it), collapse back.
 * The ray_map_fn discipline (collection.c) applies at every apply site: a
 * step result may be a LAZY DAG node BORROWING an operand the site is about
 * to release — ray_lazy_materialize before the release/store (it passes
 * non-lazy, NULL and error inputs straight through). */

/* fwd decls (mutual recursion / define-before-use) */
static ray_t* amend_at(ray_t* v, ray_t* idx, ray_t* f, ray_t* y);
static ray_t* amend_dot(ray_t* d, ray_t* idx, ray_t* f, ray_t* y);

/* The `:` (assign / replace) function slot of Amend.  Arrives either as a
 * symbol atom spelled ":" (quoted verb-sym) or, defensively, a registry value
 * whose provenance spelling is ":". */
static int is_assign(ray_t* f) {
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
static int idx_int(ray_t* e, int64_t* out) {
    if (e && e->type == -RAY_BOOL) { *out = e->b8; return 1; }
    return q_strict_i64(e, out);
}

/* Copy v's items into a fresh rc==1 boxed list (amend in place with
 * ray_list_set, collapse back afterwards).  Borrowed v; owned result. */
static ray_t* explode(ray_t* v) {
    int64_t n = ray_len(v);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (!out || RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = q_registry_elem_at(v, i);          /* owned v[i] */
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
        out = ray_list_append(out, e);              /* RETAINS */
        ray_release(e);
    }
    return out;
}

/* u each v, collapsed to a typed vector (kdb whole-value @[d;::;u] == u'[d]). */
static ray_t* each_over(ray_t* f, ray_t* v) {
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
static ray_t* amend_step(ray_t* f, ray_t* cur, ray_t* y, int64_t yi) {
    if (is_assign(f) && !y)
        return ray_error("type", "@: assign (:) needs a replacement value");
    ray_t* ry = NULL;                                /* borrowed-ish (owned) */
    if (y) {
        if (yi >= 0 && (ray_is_vec(y) || y->type == RAY_LIST)) {
            ry = q_registry_elem_at(y, yi);         /* owned */
            if (!ry || RAY_IS_ERR(ry)) return ry;
        } else { ry = y; ray_retain(ry); }           /* whole y */
    }
    ray_t* r;
    if (is_assign(f)) { r = ry; ray_retain(r); }   /* replace: new = ry */
    else if (y)         r = ray_lazy_materialize(call_fn2(f, cur, ry)); /* binary f(cur, ry) */
    else                r = ray_lazy_materialize(call_fn1(f, cur));     /* unary  f(cur)     */
    if (ry) ray_release(ry);
    return r;
}

/* @[d;key(s);f;y] — dict amend.  For each key: if present, amend the value at
 * its position; if ABSENT, extend the dict with that key (kdb inserts on a
 * missing-key amend, e.g. @[`a`b!1 2;`c;:;3] -> a|1 b|2 c|3).  Works on
 * exploded keys/vals boxed lists, collapses back, rebuilds. */
static ray_t* amend_dict(ray_t* d, ray_t* key, ray_t* f, ray_t* y) {
    ray_t* keys0 = ray_dict_keys(d);                /* borrowed */
    ray_t* vals0 = ray_dict_vals(d);                /* borrowed */
    if (!keys0 || !vals0) return ray_error("type", "@: malformed dictionary");
    ray_t* keys = explode(keys0);
    if (!keys || RAY_IS_ERR(keys)) return keys;
    ray_t* vals = explode(vals0);
    if (!vals || RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    int key_atom = ray_is_atom(key);
    int64_t steps = key_atom ? 1 : ray_len(key);
    if (!key_atom && y && (ray_is_vec(y) || y->type == RAY_LIST) && ray_len(y) != steps) {
        ray_release(keys); ray_release(vals); return ray_error("length", NULL);
    }
    for (int64_t k = 0; k < steps; k++) {
        ray_t* kk = key_atom ? (ray_retain(key), key) : q_registry_elem_at(key, k);  /* owned */
        if (!kk || RAY_IS_ERR(kk)) { ray_release(keys); ray_release(vals); return kk; }
        /* locate kk in the working keys (linear; dicts are small) */
        int64_t pos = -1, kn = ray_len(keys);
        for (int64_t j = 0; j < kn; j++) {
            ray_t* kj = ray_list_get(keys, j);       /* borrowed */
            if (kj && (kj == kk || atom_eq(kj, kk))) { pos = j; break; }  /* == : identity for non-comparable items */
        }
        int64_t yi = key_atom ? -1 : k;
        if (pos >= 0) {
            ray_t* cur = ray_list_get(vals, pos);    /* borrowed */
            ray_t* nv = amend_step(f, cur, y, yi);
            if (!nv || RAY_IS_ERR(nv)) { ray_release(kk); ray_release(keys); ray_release(vals); return nv; }
            vals = ray_list_set(vals, pos, nv);
            ray_release(nv);
        } else {                                     /* insert: apply to generic null */
            ray_t* nv = amend_step(f, RAY_NULL_OBJ, y, yi);
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
static ray_t* amend_at(ray_t* v, ray_t* idx, ray_t* f, ray_t* y) {
    if (RAY_IS_NULL(idx)) {                          /* whole value */
        if (is_assign(f)) {
            if (!y) return ray_error("type", "@: assign (:) needs a value");
            ray_retain(y); return y;
        }
        if (y) return ray_error("nyi", "@[d;::;v;vy] deferred");  /* v'[d;vy] */
        ray_t* r = each_over(f, v);                 /* u'[d] */
        /* amend-entire conform rule (ref/amend.md: @[1 2;::;3 4*] -> 'type):
         * boxed per-item results cannot fit back into a typed-vector d.
         * Checked here, not in each_over — `each` itself may box freely. */
        if (r && !RAY_IS_ERR(r) && ray_is_vec(v) && r->type == RAY_LIST) {
            ray_release(r);
            return ray_error("type", "@: amend result does not conform to a %s vector",
                             ray_type_name(v->type));
        }
        return r;
    }
    if (v->type == RAY_DICT) return amend_dict(v, idx, f, y);
    if (!ray_is_vec(v) && v->type != RAY_LIST)
        return ray_error("type", "@: cannot amend a %s", ray_type_name(v->type));
    int64_t n = ray_len(v);
    int idx_atom = ray_is_atom(idx);
    int64_t steps = idx_atom ? 1 : ray_len(idx);
    /* vector index + vector y must conform (kdb 'length) */
    if (!idx_atom && y && (ray_is_vec(y) || y->type == RAY_LIST) && ray_len(y) != steps)
        return ray_error("length", NULL);
    ray_t* work = explode(v);
    if (!work || RAY_IS_ERR(work)) return work;
    for (int64_t k = 0; k < steps; k++) {
        int64_t pos;
        if (idx_atom) { if (!idx_int(idx, &pos)) { ray_release(work); return ray_error("index", NULL); } }
        else { ray_t* p = q_registry_elem_at(idx, k); int ok = idx_int(p, &pos); ray_release(p);
               if (!ok) { ray_release(work); return ray_error("index", NULL); } }
        if (pos < 0 || pos >= n) { ray_release(work); return ray_error("index", NULL); }
        ray_t* cur = ray_list_get(work, pos);        /* borrowed slot */
        ray_t* nv  = amend_step(f, cur, y, idx_atom ? -1 : k);
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
static ray_t* amend_path(ray_t* d, const int64_t* path, int64_t plen,
                           ray_t* f, ray_t* y, int64_t yi) {
    if (plen == 0) return amend_step(f, d, y, yi);
    int64_t pos = path[0];
    if (!(ray_is_vec(d) || d->type == RAY_LIST) || pos < 0 || pos >= ray_len(d))
        return ray_error("index", NULL);
    ray_t* work = explode(d);
    if (!work || RAY_IS_ERR(work)) return work;
    ray_t* child = ray_list_get(work, pos);          /* borrowed */
    ray_t* nc = amend_path(child, path + 1, plen - 1, f, y, yi);
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
static ray_t* amend_cross(ray_t* d, ray_t* idx, ray_t* f, ray_t* y) {
    int64_t rank = ray_len(idx);
    if (rank < 1 || rank > 8) return ray_error("rank", ".: cross-section 1..8 deep");
    ray_t* dims[8]; int64_t dn[8];
    for (int64_t a = 0; a < rank; a++) {
        ray_t* col = q_registry_elem_at(idx, a);    /* owned; vector or atom */
        if (!col || RAY_IS_ERR(col)) { for (int64_t b=0;b<a;b++) ray_release(dims[b]); return col; }
        dims[a] = col; dn[a] = ray_is_atom(col) ? 1 : ray_len(col);
    }
    int64_t total = 1; for (int64_t a = 0; a < rank; a++) total *= dn[a];
    ray_t* acc = d; ray_retain(acc);
    for (int64_t t = 0; t < total; t++) {
        int64_t path[8], rem = t; int bad = 0;
        for (int64_t a = rank - 1; a >= 0; a--) {
            int64_t ix = dn[a] ? rem % dn[a] : 0; if (dn[a]) rem /= dn[a];
            if (ray_is_atom(dims[a])) { if (!idx_int(dims[a], &path[a])) bad = 1; }
            else { ray_t* p = q_registry_elem_at(dims[a], ix); if (!idx_int(p, &path[a])) bad = 1; ray_release(p); }
        }
        ray_t* na = bad ? ray_error("index", NULL)
                        : amend_path(acc, path, rank, f, y, -1);
        ray_release(acc);
        if (!na || RAY_IS_ERR(na)) { for (int64_t a=0;a<rank;a++) ray_release(dims[a]); return na; }
        acc = na;
    }
    for (int64_t a = 0; a < rank; a++) ray_release(dims[a]);
    return acc;
}

/* Amend (deep) — .[d;i;f] / .[d;i;v;vy] / .[d;();f...] (whole). */
static ray_t* amend_dot(ray_t* d, ray_t* idx, ray_t* f, ray_t* y) {
    if (RAY_IS_NULL(idx)) {                          /* .[d;();u] == u[d] */
        if (is_assign(f)) {
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
            if (ray_is_atom(idx)) ok = idx_int(idx, &path[k]);
            else { ray_t* p = q_registry_elem_at(idx, k); ok = idx_int(p, &path[k]); ray_release(p); }
            if (!ok) return ray_error("index", NULL);
        }
        return amend_path(d, path, ilen, f, y, -1);
    }
    return amend_cross(d, idx, f, y);              /* items are vectors */
}

/* Trap tail: r is g's (owned) result; on error run/return the handler e. */
static ray_t* trap_finish(ray_t* r, ray_t* e) {
    if (!r || !RAY_IS_ERR(r)) return r;              /* success passes through */
    ray_t* text = q_registry_sig_take();             /* full signal text, owned */
    if (!text) { const char* c = ray_err_code(r); text = ray_str(c ? c : "", c ? strlen(c) : 0); }
    text = q_charv_out(text);                        /* handler receives a charv */
    ray_error_free(r);
    if (!q_deriv_is_fn_value(e)) { ray_release(text); ray_retain(e); return e; }
    ray_t* hr = call_fn1(e, text);
    ray_release(text);
    return hr;
}

/* Trap At — @[f;fx;e] == .[f;enlist fx;e]. */
static ray_t* trap(ray_t* f, ray_t* x, ray_t* e) {
    q_registry_sig_clear();                          /* drop stale payload */
    ray_t* args[1] = { x };
    ray_t* r = q_deriv_call_n(f, args, 1);
    return trap_finish(r, e);
}

/* Trap — .[g;gx;e]: gx is the argument LIST; spread-apply g over it. */
static ray_t* trap_dot(ray_t* g, ray_t* gx, ray_t* e) {
    q_registry_sig_clear();
    if (!gx || (!ray_is_vec(gx) && gx->type != RAY_LIST))
        return ray_error("type", ".: trap args must be a list");
    int64_t k = ray_len(gx);
    if (k < 1 || k > 8) return ray_error("rank", ".: 1..8 trap args");
    ray_t* a[8];
    for (int64_t i = 0; i < k; i++) a[i] = q_registry_elem_at(gx, i);   /* owned */
    ray_t* r = ray_lazy_materialize(q_deriv_call_n(g, a, k));
    for (int64_t i = 0; i < k; i++) ray_release(a[i]);
    return trap_finish(r, e);
}

/* q `f@x` — Apply At / Index At (ref/apply.md).  A callable f invokes with
 * the single argument; everything else (vector, list, dict, table, 104h
 * carrier) delegates to q_apply_noun — identical semantics to `f[x]`/`f x`. */
static ray_t* at_apply2(ray_t* f, ray_t* x) {
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
    if (n == 2) return at_apply2(args[0], args[1]);
    if (n == 3) {
        if (q_deriv_is_fn_value(args[0])) return trap(args[0], args[1], args[2]);
        return amend_at(args[0], args[1], args[2], NULL);
    }
    if (n == 4) return amend_at(args[0], args[1], args[2], args[3]);
    return ray_error("rank", "@: got %lld args", (long long)n);
}

/* q `v . vx` — Apply / Index (ref/apply.md): the rhs is the ARGUMENT LIST —
 * a rank-n callable spread-calls over vx's n items; a noun depth-indexes
 * (m . 1 2 is m[1;2]).  Atom rhs is not a list -> 'type (kdb wants a list). */
static ray_t* dot_apply(ray_t* f, ray_t* a) {
    if (!a || (!ray_is_vec(a) && a->type != RAY_LIST))
        return ray_error("type", ".: rhs must be an argument list");
    if (f && (f->type == RAY_UNARY || f->type == RAY_BINARY || f->type == RAY_VARY)
          && (f->attrs & RAY_FN_SPECIAL_FORM))
        return ray_error("type", ".: special forms cannot be applied");
    int64_t n = ray_len(a);
    if (n < 1 || n > 8) return ray_error("rank", ".: 1..8 arguments");
    ray_t* args[8];
    for (int64_t i = 0; i < n; i++) {
        args[i] = q_registry_elem_at(a, i);          /* owned */
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
    r = ray_lazy_materialize(r);
    for (int64_t j = 0; j < n; j++) ray_release(args[j]);
    return r;
}

/* . VARY: 2 args Apply/Index; 3 args Trap (callable) or Amend deep (data);
 * 4 args Amend deep (quaternary).  An elided argument projects at lower time
 * (q.mkopproj) — reached here only fully bound. */
ray_t* q_dot_wrap(ray_t** args, int64_t n) {
    if (n == 2) return dot_apply(args[0], args[1]);
    if (n == 3) {
        if (q_deriv_is_fn_value(args[0])) return trap_dot(args[0], args[1], args[2]);
        return amend_dot(args[0], args[1], args[2], NULL);
    }
    if (n == 4) return amend_dot(args[0], args[1], args[2], args[3]);
    return ray_error("rank", ".: got %lld args", (long long)n);
}

ray_t* q_apply_noun(ray_t* head, ray_t** args, int64_t n) {
    if (!head) return NULL;

    /* wire pass 3: ray_eval's binary null gate offers applications with a
     * RAY_NULL_OBJ operand here before raising 'type.  Only registry-blessed
     * null-tolerant dyadics proceed (`-8!(::)` serialize, `x~(::)` match);
     * everything else declines -> historic 'type. */
    if (head->type == RAY_BINARY && n == 2 && args[0] && args[1] &&
        (RAY_IS_NULL(args[0]) || RAY_IS_NULL(args[1])) &&
        q_fn_null_ok(head))
        return ((ray_binary_fn)(uintptr_t)head->i64)(args[0], args[1]);

    /* 100h lambda carriers — q rank/projection semantics.  Claimed before
     * the n guard: a rank-0 call shape must not fall through. */
    if (head->type == RAY_LIST && q_deriv_kind_of(head) == Q_DERIV_LAMBDA)
        return lambda_apply(head, args, n);

    if (n < 1) return NULL;

    /* Handle-as-verb application (`h x`): an int atom head IS a handle apply —
     * console/file/fifo/socket dispatch lives wholly in q_handles_apply
     * (q_handles.c, the sole handle authority; this file knows no kinds).
     * Only the single-payload form is a handle apply (n==1); `h[a;b]` keeps
     * its existing decline.  An int-null head maps onto the int-null qh values
     * q_handles_apply rejects. */
    if ((head->type == -RAY_I64 || head->type == -RAY_I32) && n == 1) {
        int64_t qh = (head->type == -RAY_I64) ? head->i64 : (int64_t)head->i32;
        return q_handles_apply(qh, args[0]);
    }

    /* Raw native VARY fn values (e.g. the @/. amend-trap wrappers) reach this
     * hook via call_fn1/call_fn2, whose fast paths only special-case
     * UNARY/BINARY/LAMBDA.  Apply it here so a VARY verb works as an adverb
     * operand (`. /:`, `@ each`) and as a trap/amend applicand.  Special forms
     * need UNEVALUATED args, which are already gone -> decline. */
    if (head->type == RAY_VARY && !(head->attrs & RAY_FN_SPECIAL_FORM))
        return ((ray_vary_fn)(uintptr_t)head->i64)(args, n);

    if (head->type == -RAY_STR) {       /* stray physical string: convert, retry */
        ray_t* cv = q_charv_of_str(head);
        if (!cv || RAY_IS_ERR(cv)) return cv;
        ray_t* r = q_apply_noun(cv, args, n);
        ray_release(cv);
        return r;
    }

    /* Symbol-handle application (q namespaces, q4m3 §12): applying a symbol
     * indexes the GLOBAL it names — `` `.[`a] `` reads root `a`,
     * `` `.jab[`wrong] `` indexes the context dict.  Resolution goes through
     * the context-aware owned resolver (root/context views synthesized); an
     * unresolved name declines to the caller's historic error.  File handles
     * (`` `:path ``) stay declined (file-I/O wave). */
    if (head->type == -RAY_SYM) {
        /* ray_sym_str returns a BORROWED interned string (table/sym.c) — the
         * former ray_release here over-released the sym table's own ref
         * (latent until `:path symbols began lexing, feat/q-file-text). */
        ray_t* s = ray_sym_str(head->i64);
        if (s && ray_str_len(s) > 0 && ray_str_ptr(s)[0] == ':') {
            /* WebSocket client (kb/websockets.md): `:ws://[user:pass@]host:port`
             * applied to a raw HTTP request string opens a WS client ->
             * (handle;response).  Claimed BEFORE the one-shot IPC arm so a ws://
             * handle never TCP-connects as a kdb peer; `:wss://` -> 'nyi inside
             * q_ws_client_open.  A non-string arg declines to the historic error. */
            {
                const char* sp = ray_str_ptr(s);
                size_t sl = ray_str_len(s);
                if ((sl >= 6 && memcmp(sp, ":ws://", 6) == 0) ||
                    (sl >= 7 && memcmp(sp, ":wss://", 7) == 0)) {
                    if (n == 1 && args[0] && (args[0]->type == -RAY_STR ||
                                          args[0]->type == RAY_CHARV || args[0]->type == -RAY_CHARV))
                        return q_ws_client_open(head, args[0]);
                    return NULL;
                }
            }
            /* Low-level HTTP client (kb/http.md §low level HTTP request
             * mechanism): `:http://host:port` applied to a raw HTTP request
             * string sends it verbatim and returns the full response string.
             * Claimed BEFORE the one-shot IPC arm (as ws:// is) so an http://
             * handle never TCP-connects as a kdb peer; `:https://` -> 'nyi
             * inside q_http_client_raw.  A non-string arg declines. */
            {
                const char* hp = ray_str_ptr(s);
                size_t hl = ray_str_len(s);
                if ((hl >= 8 && memcmp(hp, ":http://", 8) == 0) ||
                    (hl >= 9 && memcmp(hp, ":https://", 9) == 0)) {
                    if (n == 1 && args[0] && (args[0]->type == -RAY_STR ||
                                          args[0]->type == RAY_CHARV || args[0]->type == -RAY_CHARV))
                        return q_http_client_raw(head, args[0]);
                    return NULL;
                }
            }
            /* One-shot synchronous IPC request (ref/hopen.md "One-shot
             * request"):  `` `:host:port "query" `` == connect -> send -> get
             * -> disconnect.  A leading-`:` COMMUNICATION handle applied to
             * exactly one STRING is the one-shot form; a `:path` file handle or
             * a non-string arg still declines to the caller's historic error.
             * Single-home reuse: q_hopen_wrap (descriptor normalize + restricted
             * gate + fd translation, incl. clean 'nyi for deferred transports),
             * the int-head handle-apply SYNC-send arm above (via a recursive
             * q_apply_noun on the fd handle), and q_hclose_wrap (fd -> selector
             * close).  The handle is hclosed on EVERY path — send success AND
             * error — so a one-shot never leaks a connection. */
            if (n == 1 && args[0] && (args[0]->type == -RAY_STR ||
                                      args[0]->type == RAY_CHARV)) {
                ray_t* h = q_hopen_wrap(head);       /* owned fd handle or error */
                if (!h || RAY_IS_ERR(h)) return h;
                ray_t* r = q_apply_noun(h, args, 1); /* int-head SYNC send */
                ray_t* c = q_hclose_wrap(h);         /* close regardless of r */
                if (c) ray_release(c);               /* swallow close status; r wins */
                ray_release(h);
                return r;
            }
            return NULL;                            /* file handle — decline */
        }
        ray_t* v = q_value_resolve_owned(head);   /* OWNED or NULL */
        if (!v) return NULL;
        if (RAY_IS_ERR(v)) return v;
        if (v->type == -RAY_SYM) { ray_release(v); return NULL; }  /* no handle chains */
        ray_t* r = q_apply_noun(v, args, n);
        ray_release(v);
        return r;
    }

    /* Composition `'[f;g;…]` — the rightmost function consumes all args, each
     * function to its left is then applied monadically to the running result. */
    if (head->type == RAY_LIST && q_deriv_kind_of(head) == Q_DERIV_COMPOSE)
        return compose_apply(head, args, n);

    /* 104h carriers are RAY_LISTs — claim them BEFORE the gather arm. */
    if (head->type == RAY_LIST && q_deriv_kind_of(head) != Q_DERIV_NONE)
        return deriv_apply(head, args, n);

    /* Noun indexing, one step per arg — d[k;i] / t[r;c] / m[i;j] drill through
     * whatever each step yields (dict -> dict_lookup with kdb miss semantics,
     * table -> q_table_at row gather then ray_at column access, vec/list ->
     * gather).  A `::` (null) index at a dict/table step DECLINES to the
     * caller's historic error — elision ("all at this level") is a later
     * wave; vec/list steps keep their pre-existing gather path for it. */
    if (head->type == RAY_DICT || head->type == RAY_TABLE ||
        ray_is_vec(head) || head->type == RAY_LIST) {
        ray_t* cur = head;
        ray_retain(cur);
        for (int64_t i = 0; i < n; i++) {
            ray_t* next;
            if (cur->type == RAY_DICT) {
                if (!args[i] || RAY_IS_NULL(args[i])) { ray_release(cur); return NULL; }
                next = dict_lookup(cur, args[i]);
            } else if (cur->type == RAY_TABLE) {
                if (!args[i] || RAY_IS_NULL(args[i])) { ray_release(cur); return NULL; }
                /* integer atom/vector -> the universal row gather (ops/q_table.c:
                 * char columns byte-permuted, misses null-filled); anything else
                 * (sym -> column, ...) declines to the historic ray_at arms. */
                next = q_table_at(cur, args[i]);
                if (!next) next = ray_at_fn(cur, args[i]);
            } else {
                /* vec/list — AND any mid-path atom, exactly as the old loop:
                 * gather -> ray_at_fn, which char-indexes string atoms
                 * ((5 2.14;"abc") . 1 2 -> "c", amend.qcmd) and errors on
                 * genuinely non-indexable atoms. */
                next = gather(cur, args[i]);
            }
            ray_release(cur);
            if (!next || RAY_IS_ERR(next)) return next;
            cur = next;
        }
        /* boundary-out: cell reads / row dicts / extracted columns cross into
         * q-space as charv, never physical STR (string-C3 rule 3). */
        return q_charv_out(cur);
    }

    return NULL;
}
