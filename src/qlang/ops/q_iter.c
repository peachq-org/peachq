/* ops/q_iter.c — the q iterators (each, each-both ', each-prior ':, over /,
 * scan \) and the imperative control constructs (q.seq/if/do/while + the one
 * shared truthiness home), the tail of the applyiter dissolution (owner
 * ruling 2026-07-22, PR #277 seam map).  Bucket A moved the amend/trap/@-.
 * bodies to q_apply.c; bucket B the fn-value machinery + lambda carrier/ret
 * to q_deriv.c; bucket C the registry specials' accessors + the `'x` signal
 * channel to q_registry.c.  These wrapper bodies ARE the build recipes the
 * registry SPECIALS[] table binds — declared in q_registry_internal.h.
 *
 * Lazy-step discipline (every apply site here): a step result may be a LAZY
 * DAG node BORROWING an operand the site is about to release —
 * ray_lazy_materialize before the release/store (it passes non-lazy, NULL and
 * error inputs straight through; D3). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* wrapper decls + q_registry.h (elem_at, collapse, provenance) + q_ops.h */
#include "qlang/ops/q_dollar.h" /* q_dollar_cast — truthiness via ONE type judgment */
#include "qlang/q_apply.h" /* q_apply_noun; q_apply_dict_union — each-both's dict key alignment (D7) */
#include "qlang/q_deriv.h" /* q_deriv_fn_rank / q_deriv_is_fn_value / q_deriv_call_n */
#include "lang/eval.h"     /* ray_eval; ray_fold_fn/ray_map_fn/ray_scan_fn HOFs */
#include "lang/internal.h" /* call_fn1/2, atom_eq, as_i64, ray_error */
#include "ops/ops.h"       /* ray_is_lazy, ray_lazy_materialize */
#include <stdint.h>        /* uintptr_t */

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
            ray_t* r = ray_lazy_materialize(call_fn1(f, row));
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
 * The fn-value utilities (rank / callable-test / generic apply) live with
 * the carriers in q_deriv.c (bucket B). */

/* ---- each-both  x f'y ------------------------------------------------------ */
static ray_t* eachboth_apply(ray_t* f, ray_t** ops, int64_t k);

static int op_is_dict(ray_t* v) { return v && v->type == RAY_DICT; }

/* each-both's per-key combiner: the general fn apply, lazy-materialized
 * inside the walk (the operands are released right after). */
static ray_t* eb_combine(ray_t* f, ray_t* va, ray_t* vb) {
    return ray_lazy_materialize(call_fn2(f, va, vb));
}

/* dict each-both (binary).  TWO DICTS KEY-ALIGN exactly like the atomic
 * dyadics (D7 fix, 2026-07-22; ref/add.md "Implicit iteration" upsert
 * semantics, ref/maps.md "corresponding items"): matching keys combine via f,
 * absentees pass through — composed on q_apply.c's ONE key-union walk, never a
 * positional zip.  A mixed dict/non-dict pair conforms the non-dict operand to
 * the dict's VALUES (kdb: d+'10 20 pairs value-wise, keys kept).  Mixed
 * operands previously dispatched ray_dict_vals(non-dict)=NULL straight into a
 * crash (codex round-2 P1). */
static ray_t* eachboth_dict(ray_t* f, ray_t* x, ray_t* y) {
    if (op_is_dict(x) && op_is_dict(y))
        return q_apply_dict_union(f, x, y, eb_combine);
    ray_t* kd = op_is_dict(x) ? x : y;     /* key donor */
    ray_t* xk = ray_dict_keys(kd);           /* borrowed */
    if (!xk) return ray_error("type", "each-both: malformed dictionary");
    ray_t* ops[2] = { op_is_dict(x) ? ray_dict_vals(x) : x,
                      op_is_dict(y) ? ray_dict_vals(y) : y };
    ray_t* rv = eachboth_apply(f, ops, 2);
    if (!rv || RAY_IS_ERR(rv)) return rv;
    ray_retain(xk);
    return ray_dict_new(xk, rv);             /* consumes keys + vals */
}
/* An each-both operand "conforms as an atom" (broadcast, not zipped) when it
 * is a true atom OR a callable FUNCTION VALUE.  A function has no depth (kdb
 * `count` on a lambda is 'type), so `.'` / `f'` broadcast the function operand
 * against the list of argument-lists — `{x+y} .' (1 2;3 4)` == 3 7 — instead
 * of mis-zipping a lambda/projection CARRIER (a RAY_LIST) against the data. */
static int op_is_atom(ray_t* v) {
    if (!v || op_is_dict(v)) return 0;
    return ray_is_atom(v) || q_deriv_is_fn_value(v);
}

static ray_t* eachboth_apply(ray_t* f, ray_t** ops, int64_t k) {
    int any_dict = 0, all_atom = 1;
    for (int64_t j = 0; j < k; j++) {
        if (op_is_dict(ops[j])) any_dict = 1;
        if (!op_is_atom(ops[j])) all_atom = 0;
    }
    if (any_dict && k == 2) return eachboth_dict(f, ops[0], ops[1]);
    if (all_atom) return q_deriv_call_n(f, ops, k);      /* all atoms -> one result */

    int64_t L = -1;
    for (int64_t j = 0; j < k; j++) {
        if (!op_is_atom(ops[j])) {
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
            if (!op_is_atom(ops[j])) { a[j] = q_registry_elem_at(ops[j], i); owned |= (1u << j); }
            else                       { a[j] = ops[j]; }   /* atom broadcast */
        }
        /* Force BEFORE releasing the operands the lazy may borrow (r2 review;
         * the ray_map_fn discipline). */
        ray_t* r = ray_lazy_materialize(q_deriv_call_n(f, a, kk));
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
    return eachboth_apply(args[0], args + 1, n - 1);
}

/* ---- each-prior  (f':)x  /  s f':x ---------------------------------------- */
/* Seed for the UNARY form: operator identity if known to q, else `first 0#x`
 * (typed null of the argument's element type) — ref/maps.md 259-279. */
static ray_t* prior_seed(ray_t* f, ray_t* x) {
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
static ray_t* prior_over_vec(ray_t* f, ray_t* seed, ray_t* x) {
    if (!x || (!ray_is_vec(x) && x->type != RAY_LIST)) {
        if (x && ray_is_atom(x)) return ray_lazy_materialize(call_fn2(f, x, seed));
        return ray_error("type", "each-prior: expected a list");
    }
    int64_t L = ray_len(x);
    ray_t* out = ray_list_new(L > 0 ? L : 1);
    ray_t* prev = seed; int prev_owned = 0;
    for (int64_t i = 0; i < L; i++) {
        ray_t* cur = q_registry_elem_at(x, i);            /* owned */
        ray_t* r   = ray_lazy_materialize(call_fn2(f, cur, prev));
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
    else { seed = prior_seed(f, x); seed_owned = 1; }
    ray_t* r;
    if (x && x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);
        ray_t* rv = prior_over_vec(f, seed, ray_dict_vals(x));
        if (!rv || RAY_IS_ERR(rv)) r = rv;
        else { ray_retain(k); r = ray_dict_new(k, rv); }
    } else {
        r = prior_over_vec(f, seed, x);
    }
    if (seed_owned) ray_release(seed);
    return r;
}

/* ---- over / scan  (converge, do, while, and reduce) ----------------------- */

/* The ONE truthiness home: the `if`/`while` test and the `f/`/`f\` while-adverb
 * condition (`do` takes a COUNT, not a truthiness, and keeps its own gate).
 * Owner ruling 2026-07-15, the authority where the docs are silent on the error
 * codes: materialize -> exclude float/real -> cast with the SAME fn `"b"$` uses
 * -> boolean ATOM = 1b, else 'type.  Deciding via q_dollar_cast keeps ONE type
 * judgment; the ATOM check subsumes an arity gate ("b"$1 2 -> 11b, a vector).
 * float/real go BEFORE the cast: the cast accepts them ("b"$1.5 -> 1b) but
 * ref/if.md:20 / ref/while.md:21 require "an atom of integral type".
 * CONSUMES v (ray_lazy_materialize releases its arg and passes a non-lazy one
 * through, so one release covers both); an owned error lands in *err. */
static int truth(ray_t* v, ray_t** err) {
    *err = NULL;
    if (!v) { *err = ray_error("type", NULL); return 0; }
    v = ray_lazy_materialize(v);
    if (RAY_IS_ERR(v)) { *err = v; return 0; }
    int8_t t = v->type < 0 ? (int8_t)-v->type : v->type;
    if (t == RAY_F64 || t == RAY_F32) { ray_release(v); *err = ray_error("type", NULL); return 0; }
    ray_t* b = q_dollar_cast(RAY_BOOL, v);
    ray_release(v);
    if (!b || RAY_IS_ERR(b)) { *err = b ? b : ray_error("type", NULL); return 0; }
    if (b->type != -RAY_BOOL) { ray_release(b); *err = ray_error("type", NULL); return 0; }
    int go = b->b8 != 0;
    ray_release(b);
    return go;
}

/* Converge: apply f until the result matches the previous OR the initial x.
 * collect=1 keeps every step (scan), else returns the last (over). */
static ray_t* converge(ray_t* f, ray_t* x, int collect) {
    ray_t* first = x; ray_retain(first);
    ray_t* cur   = x; ray_retain(cur);
    ray_t* acc   = collect ? ray_list_new(0) : NULL;
    if (collect) acc = ray_list_append(acc, cur);
    int64_t guard = 0;
    for (;;) {
        ray_t* nxt = ray_lazy_materialize(call_fn1(f, cur));
        if (!nxt || RAY_IS_ERR(nxt)) {
            ray_release(first); ray_release(cur); if (acc) ray_release(acc);
            return nxt ? nxt : ray_error("type", NULL);
        }
        if (nxt == cur || nxt == first || atom_eq(nxt, cur) || atom_eq(nxt, first)) { ray_release(nxt); break; }
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
static ray_t* ntimes(ray_t* f, int64_t cnt, ray_t* x, int collect) {
    if (cnt < 0) cnt = 0;
    ray_t* cur = x; ray_retain(cur);
    ray_t* acc = NULL;
    if (collect) { acc = ray_list_new(cnt + 1); acc = ray_list_append(acc, cur); }
    for (int64_t i = 0; i < cnt; i++) {
        ray_t* nxt = ray_lazy_materialize(call_fn1(f, cur));
        ray_release(cur);
        if (!nxt || RAY_IS_ERR(nxt)) { if (acc) ray_release(acc); return nxt ? nxt : ray_error("type", NULL); }
        cur = nxt;
        if (collect) acc = ray_list_append(acc, cur);
    }
    if (collect) { ray_release(cur); ray_t* c = q_collapse_list(acc); ray_release(acc); return c; }
    return cur;
}

/* While: apply f while test(cur) holds (test f/x).  collect keeps each step. */
static ray_t* i_while(ray_t* f, ray_t* test, ray_t* x, int collect) {
    ray_t* cur = x; ray_retain(cur);
    ray_t* acc = NULL;
    if (collect) { acc = ray_list_new(0); acc = ray_list_append(acc, cur); }
    int64_t guard = 0;
    for (;;) {
        ray_t* terr = NULL;
        int go = truth(call_fn1(test, cur), &terr);   /* consumes the test result */
        if (terr) { ray_release(cur); if (acc) ray_release(acc); return terr; }
        if (!go) break;
        ray_t* nxt = ray_lazy_materialize(call_fn1(f, cur));
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
static ray_t* seeded_scan(ray_t* f, ray_t* seed, ray_t* x, int collapse_steps) {
    if (!x || (!ray_is_vec(x) && x->type != RAY_LIST)) return ray_error("type", "scan: expected a list");
    int64_t L = ray_len(x);
    ray_t* out = ray_list_new(L > 0 ? L : 1);
    ray_t* acc = seed; ray_retain(acc);
    for (int64_t i = 0; i < L; i++) {
        ray_t* cur = q_registry_elem_at(x, i);
        ray_t* nxt = ray_lazy_materialize(call_fn2(f, acc, cur));
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
 * (Each-prior keeps its OWN maps.md-cited seed set in prior_seed.) */
static ray_t* acc_identity(ray_t* f) {
    q_provenance_t pv;
    if (!q_registry_provenance(f, &pv)) return NULL;
    return q_ops_acc_identity(pv.spelling);
}

/* Over/Scan on an EMPTY right argument (ref/accumulators.md:396-399 Scan,
 * 408-437 Over): known identity -> I; a LIST/vector VALUE (pinned for Over
 * only: 1 0 3h/[til 0] -> 5h) -> empty of the value's type; else (). */
static ray_t* acc_empty(ray_t* f, int scan) {
    ray_t* ident = acc_identity(f);
    if (ident) return ident;
    if (!scan && f) {
        if (f->type == RAY_LIST) return ray_list_new(0);
        if (ray_is_vec(f))
            return (f->type == RAY_SYM) ? ray_sym_vec_new(RAY_SYM_W64, 0)
                                        : ray_vec_new(f->type, 0);
    }
    return ray_list_new(0);
}

static int acc_is_coll(ray_t* x) {
    return x && (ray_is_vec(x) || x->type == RAY_LIST);
}

/* `/` over — reduce / converge / do / while by operand shape and f rank. */
ray_t* q_over_wrap(ray_t** args, int64_t n) {
    ray_t* f = args[0];
    int rank = q_deriv_fn_rank(f);
    if (n == 2) {
        ray_t* x = args[1];
        if (rank == 1) return converge(f, x, 0);
        if (acc_is_coll(x) && ray_len(x) == 0) return acc_empty(f, 0);
        if (x && ray_is_atom(x) && x->type != RAY_LIST) {
            /* atom right argument with known I: one evaluation f(I, x) —
             * (,/)42 -> ,42 (raze == ,/ and raze 42 -> ,42, ref/raze.md) */
            ray_t* ident = acc_identity(f);
            if (ident) {
                ray_t* r = ray_lazy_materialize(call_fn2(f, ident, x));
                ray_release(ident);
                return r;
            }
        }
        ray_t* fa[2] = { f, x };
        return ray_lazy_materialize(ray_fold_fn(fa, 2));              /* reduce */
    }
    if (n == 3) {
        ray_t* a = args[1], *x = args[2];
        if (q_deriv_is_fn_value(a))  return i_while(f, a, x, 0);
        if (rank == 1)         return ntimes(f, as_i64(a), x, 0);
        ray_t* fa[3] = { f, a, x };
        return ray_lazy_materialize(ray_fold_fn(fa, 3));              /* seeded reduce */
    }
    return ray_error("rank", "over: bad arity");
}

/* `\` scan — like over but every step is retained. */
ray_t* q_scan_wrap(ray_t** args, int64_t n) {
    ray_t* f = args[0];
    int rank = q_deriv_fn_rank(f);
    if (n == 2) {
        ray_t* x = args[1];
        if (rank == 1) return converge(f, x, 1);
        if (acc_is_coll(x) && ray_len(x) == 0) return acc_empty(f, 1);
        {   /* unary-seed rule (ref/accumulators.md:261-267): a known I is the
             * left argument of the FIRST evaluation — (,\)2 3 4 -> ,2 / 2 3 /
             * 2 3 4.  Applied for `,` only: seeding + and * is
             * value-identical but would promote the first step's type. */
            ray_t* ident = acc_identity(f);
            if (ident) {
                if (ident->type == RAY_LIST && acc_is_coll(x)) {
                    ray_t* r = seeded_scan(f, ident, x, 1);
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
        if (q_deriv_is_fn_value(a))  return i_while(f, a, x, 1);
        if (rank == 1)         return ntimes(f, as_i64(a), x, 1);
        return seeded_scan(f, a, x, 0);
    }
    return ray_error("rank", "scan: bad arity");
}

/* over/scan/prior keyword dyadic wrappers `f over x` / `f scan x` /
 * `f prior x` — delegate to the n==2 core (peach reuses q_each_wrap). */
ray_t* q_over_kw(ray_t* f, ray_t* x)  { ray_t* a[2] = { f, x }; return q_over_wrap(a, 2); }
ray_t* q_scan_kw(ray_t* f, ray_t* x)  { ray_t* a[2] = { f, x }; return q_scan_wrap(a, 2); }
ray_t* q_prior_kw(ray_t* f, ray_t* x) { ray_t* a[2] = { f, x }; return q_prior_wrap(a, 2); }

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

/* Evaluate an if/while test arg and decide it at the one truthiness home. */
static int ctl_truth(ray_t* arg, ray_t** err) {
    return truth(ray_eval(arg), err);   /* truth consumes the eval result */
}

/* Evaluate args[from..n) in order for their side effects, releasing each
 * result.  Returns an owned RAY_ERROR on the first failure, else NULL. */
static ray_t* ctl_run_body(ray_t** args, int64_t from, int64_t n) {
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
    int truthy = ctl_truth(args[0], &err);
    if (err) return err;
    if (truthy) { err = ctl_run_body(args, 1, n); if (err) return err; }
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
        ray_t* err = ctl_run_body(args, 1, n);
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
        int truthy = ctl_truth(args[0], &err);
        if (err) return err;
        if (!truthy) break;
        err = ctl_run_body(args, 1, n);
        if (err) return err;
    }
    return RAY_NULL_OBJ;
}
