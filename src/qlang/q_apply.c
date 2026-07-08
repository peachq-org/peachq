/* q_apply — see q_apply.h.  The per-head semantic table (spec 2c, revised
 * after adversarial codex review):
 *
 *   104h carrier      -> q_deriv_apply (Task 5; claimed FIRST — a carrier IS
 *                        a RAY_LIST and must not fall into the gather arm)
 *   dict              -> direct ray_dict_get; on a miss, the typed null of
 *                        the dict's VALUE type (ray_at would hardcode 0Nl)
 *   table             -> ray_at row/column access (base special-cases tables)
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
#include "qlang/q_registry.h"   /* q_collapse_list */
#include "qlang/q_deriv.h"      /* q_deriv_kind_of (carrier arm, Task 5) */
#include "lang/eval.h"          /* ray_at_fn */
#include "lang/internal.h"      /* call_lambda — 100h lambda-carrier application */
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
    if (hit >= 0) {
        ray_t* ra = ray_i64(hit);
        ray_t* row = ray_at_fn(vtab, ra);            /* row dict, owned */
        ray_release(ra);
        return row;
    }
    /* miss -> null row: colname!typed-null per value column */
    int64_t nv = ray_table_ncols(vtab);
    ray_t* names = ray_vec_new(RAY_SYM, nv > 0 ? nv : 1);
    if (!names || RAY_IS_ERR(names)) return names ? names : ray_error("oom", NULL);
    names->len = nv;
    int64_t* nd = (int64_t*)ray_data(names);
    ray_t* nulls = ray_list_new(nv > 0 ? nv : 1);
    if (!nulls || RAY_IS_ERR(nulls)) { ray_release(names); return nulls ? nulls : ray_error("oom", NULL); }
    for (int64_t c = 0; c < nv; c++) {
        nd[c] = ray_table_col_name(vtab, c);
        ray_t* col = ray_table_get_col_idx(vtab, c);       /* borrowed */
        int8_t nt = (col && ray_is_vec(col)) ? (int8_t)-col->type : 0;
        ray_t* nu = nt ? ray_typed_null(nt)
                       : (ray_retain(RAY_NULL_OBJ), RAY_NULL_OBJ);
        nulls = ray_list_append(nulls, nu);
        ray_release(nu);
    }
    ray_t* cv = q_collapse_list(nulls);
    ray_release(nulls);
    if (!cv || RAY_IS_ERR(cv)) { ray_release(names); return cv ? cv : ray_error("type", NULL); }
    return ray_dict_new(names, cv);                  /* consumes both */
}

static ray_t* dict_lookup(ray_t* d, ray_t* idx) {
    if (q_is_keyed_table(d)) return keyed_table_lookup(d, idx);
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
 * The base HOF is called by its fn type.  Args/carrier borrowed; returns
 * owned (or an owned error). */
static ray_t* q_deriv_apply(ray_t* carrier, ray_t** args, int64_t n) {
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
        return ((ray_unary_fn)(uintptr_t)base->i64)(call[0]);
    case RAY_BINARY:
        if (cn != 2) return ray_error("rank", "derived verb: expected 2 args, got %lld", (long long)cn);
        return ((ray_binary_fn)(uintptr_t)base->i64)(call[0], call[1]);
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
static ray_t* q_call_lambda(ray_t* lam, ray_t** args, int64_t n) {
    /* Drop any stale payload first (a q.ret swallowed by a user trap), so
     * the class check below can never be spoofed into returning old data. */
    ray_t* stale = q_lambda_ret_take();
    if (stale) ray_release(stale);
    ray_t* r = call_lambda(lam, args, n);
    if (r && RAY_IS_ERR(r) && r->slen == 5 && memcmp(r->sdata, "q.ret", 5) == 0) {
        /* genuine `:x` always stashed a payload (a bare `:` stashes `::`);
         * a user-forged '"q.ret" signal has none and propagates as an error */
        ray_t* v = q_lambda_ret_take();               /* owned, or NULL */
        if (v) { ray_release(r); return v; }
    }
    return r;
}

static ray_t* q_lambda_apply(ray_t* carrier, ray_t** args, int64_t n) {
    int     r   = q_deriv_valence(carrier);
    ray_t*  lam = q_deriv_base(carrier);              /* borrowed */
    if (!lam) return ray_error("type", "lambda carrier: missing base");
    if (r == 0) {
        if (n <= 1) return q_call_lambda(lam, NULL, 0);
        return ray_error("rank", "lambda: rank 0 applied to %lld args", (long long)n);
    }
    if (n < 1) return ray_error("rank", "lambda: no arguments");
    if (n > r)
        return ray_error("rank", "lambda: rank %d applied to %lld args",
                         r, (long long)n);
    if (r == 1) return q_call_lambda(lam, args, 1);
    if (r > 64) return ray_error("limit", "lambda: rank too large");
    uint64_t mask  = 0;
    ray_t*   slots[64];
    int      holes = 0;
    for (int i = 0; i < r; i++) {
        ray_t* a = (i < (int)n) ? args[i] : NULL;
        if (!a || RAY_IS_NULL(a)) { mask |= 1ull << i; slots[i] = NULL; holes++; }
        else slots[i] = a;
    }
    if (holes == 0) return q_call_lambda(lam, args, n);
    return q_proj_new(carrier, slots, r, mask, holes);
}

/* Apply any function VALUE to args: bare builtins direct, everything else
 * (carriers, lambdas, VARY, projections) through the noun dispatcher. */
static ray_t* q_apply_fn(ray_t* fn, ray_t** args, int64_t n) {
    if (fn && fn->type == RAY_UNARY && n == 1)
        return ((ray_unary_fn)(uintptr_t)fn->i64)(args[0]);
    if (fn && fn->type == RAY_BINARY && n == 2)
        return ((ray_binary_fn)(uintptr_t)fn->i64)(args[0], args[1]);
    return q_apply_noun(fn, args, n);
}

/* Apply a composition carrier `'[f;g;…]`: the innermost (rightmost) function
 * consumes all supplied args, then each function to its left is applied
 * monadically to the running result.  `'[f;g][a;b]` == `f g[a;b]`. */
static ray_t* q_compose_apply(ray_t* carrier, ray_t** args, int64_t n) {
    int64_t nf = q_compose_count(carrier);
    if (nf < 1) return ray_error("rank", "compose: empty composition");
    ray_t* acc = q_apply_fn(q_compose_fn_at(carrier, nf - 1), args, n);
    if (!acc || RAY_IS_ERR(acc)) return acc;
    for (int64_t i = nf - 2; i >= 0; i--) {
        ray_t* a1[1] = { acc };
        ray_t* next = q_apply_fn(q_compose_fn_at(carrier, i), a1, 1);
        ray_release(acc);
        if (!next || RAY_IS_ERR(next)) return next;
        acc = next;
    }
    return acc;
}

/* ── dict function-distribution shim (openq) ─────────────────────────────
 * A builtin verb applied to a dictionary distributes over its VALUES, with
 * key order preserved.  Reached only via eval's `dict_retry` on the 'type-
 * error path, so verbs that already handle a dict natively (key/value/count/
 * type/`,`/`#`/`@`) never arrive here.  The verb is applied to the value
 * VECTOR (never a dict) exactly as the tree-walk kernel dispatch would, so
 * every rayfall kernel is reused with zero reimplementation. */

/* Apply a builtin UNARY head to one operand, matching the tree-walk arm:
 * atomic verbs map over a collection, others call the kernel directly. */
static ray_t* dict_apply1(ray_t* head, ray_t* a) {
    ray_unary_fn fn = (ray_unary_fn)(uintptr_t)head->i64;
    if ((head->attrs & RAY_FN_ATOMIC) && is_collection(a))
        return atomic_map_unary(fn, a);
    return fn(a);
}

/* Apply a builtin BINARY head to two operands, matching the tree-walk arm
 * (opcode carried so a length-mismatch error names the verb). */
static ray_t* dict_apply2(ray_t* head, ray_t* a, ray_t* b) {
    ray_binary_fn fn = (ray_binary_fn)(uintptr_t)head->i64;
    if ((head->attrs & RAY_FN_ATOMIC) && (is_collection(a) || is_collection(b)))
        return atomic_map_binary_op(fn, RAY_FN_OPCODE(head), a, b);
    return fn(a, b);
}

/* d op e — key-union / upsert.  Result keys = keys of `a` in order, then keys
 * of `b` absent from `a`; matching keys combine via `op`, others pass through.
 * Mirrors q_eachboth_dict's key-donor discipline (retain keys before
 * ray_dict_new). */
static ray_t* q_dict_union(ray_t* head, ray_t* a, ray_t* b) {
    ray_t* ka = ray_dict_keys(a);        /* borrowed */
    ray_t* kb = ray_dict_keys(b);        /* borrowed */
    if (!ka || !kb) return ray_error("type", "dict op: malformed dictionary");
    int64_t na = ray_dict_len(a);
    int64_t nb = ray_dict_len(b);
    int64_t cap = na + nb; if (cap < 1) cap = 1;
    ray_t* okeys = ray_list_new(cap);
    ray_t* ovals = ray_list_new(cap);
    if (!okeys || !ovals) {
        if (okeys) ray_release(okeys);
        if (ovals) ray_release(ovals);
        return ray_error("oom", NULL);
    }
    /* pass 1: every key of `a`, combined with b's matching value if present */
    for (int64_t i = 0; i < na; i++) {
        ray_t* ix = ray_i64(i);
        ray_t* k = ray_at_fn(ka, ix);            /* owned key atom */
        ray_release(ix);
        if (!k || RAY_IS_ERR(k)) { ray_release(okeys); ray_release(ovals); return k ? k : ray_error("type", NULL); }
        ray_t* va = ray_dict_get(a, k);          /* owned, present */
        ray_t* vb = ray_dict_get(b, k);          /* owned or NULL */
        ray_t* rv;
        if (vb) {
            rv = dict_apply2(head, va, vb);
            ray_release(va); ray_release(vb);
            if (!rv || RAY_IS_ERR(rv)) { ray_release(k); ray_release(okeys); ray_release(ovals); return rv ? rv : ray_error("type", NULL); }
        } else {
            rv = va;                             /* pass through, owned */
        }
        okeys = ray_list_append(okeys, k);
        ovals = ray_list_append(ovals, rv);
        ray_release(k); ray_release(rv);
    }
    /* pass 2: keys of `b` not present in `a`, values unchanged */
    for (int64_t j = 0; j < nb; j++) {
        ray_t* ix = ray_i64(j);
        ray_t* k = ray_at_fn(kb, ix);
        ray_release(ix);
        if (!k || RAY_IS_ERR(k)) { ray_release(okeys); ray_release(ovals); return k ? k : ray_error("type", NULL); }
        ray_t* pa = ray_dict_get(a, k);          /* present in a? */
        if (pa) { ray_release(pa); ray_release(k); continue; }
        ray_t* vb = ray_dict_get(b, k);          /* owned, present */
        okeys = ray_list_append(okeys, k);
        ovals = ray_list_append(ovals, vb);
        ray_release(k);
        if (vb) ray_release(vb);
    }
    ray_t* ck = q_collapse_list(okeys);
    ray_t* cv = q_collapse_list(ovals);
    ray_release(okeys); ray_release(ovals);
    if (!ck || RAY_IS_ERR(ck)) { if (cv && !RAY_IS_ERR(cv)) ray_release(cv); return ck ? ck : ray_error("type", NULL); }
    if (!cv || RAY_IS_ERR(cv)) { ray_release(ck); return cv ? cv : ray_error("type", NULL); }
    return ray_dict_new(ck, cv);                 /* consumes ck + cv */
}

/* The distribution dispatch: monadic map/aggregate, dyadic dict+atom (both
 * orders) and dyadic dict+dict union.  Returns owned, or NULL to decline. */
static ray_t* q_dict_distribute(ray_t* head, ray_t** args, int64_t n) {
    if (n == 1) {
        ray_t* d = args[0];
        ray_t* keys = ray_dict_keys(d);          /* borrowed */
        ray_t* vals = ray_dict_vals(d);          /* borrowed */
        if (!keys || !vals) return NULL;
        ray_t* r = dict_apply1(head, vals);
        if (!r || RAY_IS_ERR(r)) return r;
        /* map (result conforms to values) vs aggregate (scalar result) */
        if (is_collection(r) && ray_len(r) == ray_dict_len(d)) {
            ray_retain(keys);
            return ray_dict_new(keys, r);        /* consumes keys + r */
        }
        return r;                                /* aggregate scalar */
    }
    if (n == 2) {
        ray_t* a = args[0];
        ray_t* b = args[1];
        bool ad = a && a->type == RAY_DICT;
        bool bd = b && b->type == RAY_DICT;
        if (ad && bd) return q_dict_union(head, a, b);
        ray_t* d = ad ? a : b;
        ray_t* keys = ray_dict_keys(d);          /* borrowed */
        ray_t* vals = ray_dict_vals(d);          /* borrowed */
        if (!keys || !vals) return NULL;
        ray_t* r = ad ? dict_apply2(head, vals, b) : dict_apply2(head, a, vals);
        if (!r || RAY_IS_ERR(r)) return r;
        ray_retain(keys);
        return ray_dict_new(keys, r);            /* consumes keys + r */
    }
    return NULL;
}

ray_t* q_apply_noun(ray_t* head, ray_t** args, int64_t n) {
    if (!head) return NULL;

    /* openq dict function-distribution shim: a builtin verb whose argument is
     * a dictionary distributes over the values.  Reached from eval's dict_retry
     * (the 'type-error path) with matching arity, but ALSO reachable via the
     * call_fn1/call_fn2 hook tails for a wrong-arity builtin (e.g. `map[+;d]`
     * applies the BINARY `+` monadically) — so require exact arity here, else
     * q_dict_distribute would call the wrong kernel signature and crash. */
    if ((head->type == RAY_UNARY && n == 1) ||
        (head->type == RAY_BINARY && n == 2)) {
        for (int64_t i = 0; i < n; i++)
            if (args[i] && args[i]->type == RAY_DICT)
                return q_dict_distribute(head, args, n);
    }

    /* 100h lambda carriers — q rank/projection semantics.  Claimed before
     * the n guard: a rank-0 call shape must not fall through. */
    if (head->type == RAY_LIST && q_deriv_kind_of(head) == Q_DERIV_LAMBDA)
        return q_lambda_apply(head, args, n);

    if (n < 1) return NULL;

    /* IPC handle-as-verb application (feat/q-ipc-client, Phase D).  An int atom
     * applied to a payload is a kdb IPC send: a POSITIVE handle sends SYNC
     * (`.ipc.send` -> deserialized response); a NEGATIVE handle (from `neg h`)
     * sends ASYNC (`.ipc.post` -> `::`).  q handles are 1-BASED (hopen offsets the
     * raw poll id by +1 so 0 stays reserved and neg can sign it), so translate
     * `|q handle| - 1` back to the raw selector id the primitives expect.  Only
     * the single-payload form `h x` is handled (n==1); `h[a;b]` keeps its existing
     * decline.  The `.ipc.*` primitives are RAY_FN_RESTRICTED and we call them
     * directly, so re-assert the restricted check (a restricted remote connection
     * must not drive handles).  q handle 0 (console) and null/INT_MIN decline. */
    if ((head->type == -RAY_I64 || head->type == -RAY_I32) && n == 1) {
        if (ray_eval_get_restricted()) return ray_error("access", "restricted");
        int64_t qh = (head->type == -RAY_I64) ? head->i64 : (int64_t)head->i32;
        if (RAY_ATOM_IS_NULL(head) || qh == 0 || qh == INT64_MIN)
            return ray_error("type", "handle apply: invalid handle %lld", (long long)qh);
        int64_t raw   = (qh > 0 ? qh : -qh) - 1;   /* 1-based q handle -> raw id */
        ray_t*  rawh  = make_i64(raw);
        ray_t*  r     = (qh > 0) ? ray_hsend_fn(rawh, args[0])   /* sync  */
                                 : ray_hpost_fn(rawh, args[0]);  /* async */
        ray_release(rawh);
        return r;
    }

    /* Raw native VARY fn values (e.g. the @/. amend-trap wrappers) reach this
     * hook via call_fn1/call_fn2, whose fast paths only special-case
     * UNARY/BINARY/LAMBDA.  Apply it here so a VARY verb works as an adverb
     * operand (`. /:`, `@ each`) and as a trap/amend applicand.  Special forms
     * need UNEVALUATED args, which are already gone -> decline. */
    if (head->type == RAY_VARY && !(head->attrs & RAY_FN_SPECIAL_FORM))
        return ((ray_vary_fn)(uintptr_t)head->i64)(args, n);

    if (head->type == -RAY_STR) return NULL;        /* deferred: string model */

    /* Symbol-handle application (q namespaces, q4m3 §12): applying a symbol
     * indexes the GLOBAL it names — `` `.[`a] `` reads root `a`,
     * `` `.jab[`wrong] `` indexes the context dict.  Resolution goes through
     * the context-aware owned resolver (root/context views synthesized); an
     * unresolved name declines to the caller's historic error.  File handles
     * (`` `:path ``) stay declined (file-I/O wave). */
    if (head->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(head->i64);
        if (s && ray_str_len(s) > 0 && ray_str_ptr(s)[0] == ':') {
            ray_release(s);
            return NULL;                            /* file handle — decline */
        }
        if (s) ray_release(s);
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
        return q_compose_apply(head, args, n);

    /* 104h carriers are RAY_LISTs — claim them BEFORE the gather arm. */
    if (head->type == RAY_LIST && q_deriv_kind_of(head) != Q_DERIV_NONE)
        return q_deriv_apply(head, args, n);

    /* Noun indexing, one step per arg — d[k;i] / t[r;c] / m[i;j] drill through
     * whatever each step yields (dict -> dict_lookup with kdb miss semantics,
     * table -> ray_at row/column, vec/list -> gather).  Single-arg dict/table
     * behaviour is byte-identical to the old dedicated arms.  A `::` (null)
     * index at a dict/table step DECLINES to the caller's historic error —
     * elision ("all at this level") is a later wave; vec/list steps keep
     * their pre-existing gather path for it. */
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
                next = ray_at_fn(cur, args[i]);
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
        return cur;
    }

    return NULL;
}
