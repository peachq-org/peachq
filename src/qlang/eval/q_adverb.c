/* q_adverb — the native adverb engine (split from the apply module 2026-07-30):
 * the Scan/Over accumulators (ref/accumulators.md) and the maps — Each, Each
 * Left/Right, Each Prior, Case (ref/maps.md) — plus manifest monomorphization
 * off the Q_OPS[] mono column.  q_adverb_apply is the one entry (the walker's
 * adverb nodes and the apply module's derived-value/keyword-HOF arms both land
 * here); derived evaluations call back out through q_eval_apply.  Same
 * refcount contract: args borrowed, result owned. */
#include "qlang/eval/q_eval.h"
#include "qlang/eval/q_eval_internal.h"
#include "qlang/q_err.h"
#include "qlang/q_ops.h"       /* q_ops_find / acc_identity + the manifest columns */
#include "qlang/q_registry.h"  /* q_registry_row_of, q_match_rec, q_typed_empty_like */
#include "qlang/q_builtins.h"  /* q_count_long — q `count` for C callers, hot lane */
#include "qlang/q_type.h"
#include "qlang/ops/q_index.h" /* q_index_elem_at — the one element accessor */
#include "table/dict.h"
#include <string.h>

/* An iterated dict argument IS its values — for the maps, and for the
 * accumulators by ref/accumulators.md:414 ("Over ... reduces lists AND
 * DICTIONARIES to atoms").  A keyed table is a dict too: its values are a
 * table, whose own items are rows, so the two laws compose and it needs no
 * arm of its own. */
static ray_t* iter_dict_vals(ray_t* x) {
    return (x && x->type == RAY_DICT) ? ray_dict_vals(x) : NULL;
}

/* re-key a UNIFORM result (each, scan, prior) onto x's keys; consumes r.  An
 * aggregate (over) does NOT re-key — it has reduced to an atom. */
static ray_t* dict_rekey(ray_t* x, ray_t* r) {
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* k = ray_dict_keys(x);
    ray_retain(k);
    return ray_dict_new(k, r);
}

/* ----- accumulators: Scan and Over (ref/accumulators.md) -----------------
 * "They have the same syntax and perform the same computation.  But where the
 * Scan-derived functions return the result of each evaluation, those of Over
 * return only the last" (:37) — so ONE engine carries a `keep` flag and every
 * arm below is shared.  `acc_t` is that running result. */
typedef struct { int keep; ray_t* list; ray_t* last; } acc_t;

static void acc_init(acc_t* a, int keep) {
    a->keep = keep;
    a->list = keep ? ray_list_new(0) : NULL;
    a->last = NULL;
}

/* borrows v; returns an owned error when the append failed, else NULL */
static ray_t* acc_push(acc_t* a, ray_t* v) {
    if (a->last) ray_release(a->last);
    ray_retain(v);
    a->last = v;
    if (!a->keep) return NULL;
    a->list = ray_list_append(a->list, v);
    if (!RAY_IS_ERR(a->list)) return NULL;
    ray_t* e = a->list;
    a->list = NULL;
    return e;
}

/* the owned result; an owned `err` wins and the partials are dropped */
static ray_t* acc_finish(acc_t* a, ray_t* err) {
    ray_t* r = err;
    if (!r && a->keep) { r = q_eval_apply_collapse(a->list); a->list = NULL; }
    if (!r) { r = a->last; a->last = NULL; }
    if (a->list) ray_release(a->list);
    if (a->last) ray_release(a->last);
    return r ? r : q_err(QE_TYPE);
}

/* Converge: evaluate until two successive results match, or one matches x
 * (:97).  `~` is the match home, so tolerance is decided in one place. */
static ray_t* acc_converge(ray_t* fv, const q_op_t* frow, ray_t* x, int keep) {
    acc_t a;
    acc_init(&a, keep);
    ray_retain(x);
    ray_t* x0 = q_eval_apply_concrete(x);
    ray_t* cur = x0;
    ray_retain(cur);
    ray_t* err = acc_push(&a, cur);
    while (!err) {
        ray_t* nx = q_eval_apply(fv, frow, &cur, 1);
        if (RAY_IS_ERR(nx)) { err = nx; break; }
        int stop = q_match_rec(nx, cur) || q_match_rec(nx, x0);
        ray_release(cur);
        cur = nx;
        if (stop) break;
        err = acc_push(&a, cur);
    }
    ray_release(cur);
    ray_release(x0);
    return acc_finish(&a, err);
}

/* Do (:134) and While (:193): x is the first result, then `n` more, or one per
 * pass while the unary truth map `t` holds on the running result. */
static ray_t* acc_iterate(ray_t* fv, const q_op_t* frow, ray_t* t, int64_t n,
                          ray_t* x, int keep) {
    const q_op_t* trow = (t && q_eval_apply_is_fnval(t))
                             ? q_registry_row_of(t, Q_MONADIC) : NULL;
    acc_t a;
    acc_init(&a, keep);
    ray_retain(x);
    ray_t* cur = q_eval_apply_concrete(x);
    ray_t* err = acc_push(&a, cur);
    for (int64_t i = 0; !err && (t || i < n); i++) {
        if (t) {
            int go = q_eval_apply_truthy(q_eval_apply(t, trow, &cur, 1), &err);
            if (err || !go) break;
        }
        ray_t* nx = q_eval_apply(fv, frow, &cur, 1);
        ray_release(cur);
        if (RAY_IS_ERR(nx)) { cur = NULL; err = nx; break; }
        cur = nx;
        err = acc_push(&a, cur);
    }
    if (cur) ray_release(cur);
    return acc_finish(&a, err);
}

/* Empty right argument (:385), the value never evaluated.  With a seed Scan
 * and Over diverge: Over reduces to the left argument (:445) where Scan, being
 * uniform, returns `()` (:405).  Applied as a unary they agree and the answer
 * comes from the VALUE — its identity element, else (for a list value) an
 * empty of its own type, else `()`.  NULL means "let the registered aggregate
 * answer": `|`/`&` monomorphize but have no doc-published identity. */
static ray_t* acc_empty(ray_t* fv, const q_op_t* frow, ray_t* seed, int keep,
                        int has_mono) {
    if (seed) {
        if (keep) return ray_list_new(0);
        ray_retain(seed);
        return seed;
    }
    ray_t* id = frow ? q_ops_acc_identity(frow->name) : NULL;
    if (id) return id;
    return has_mono ? NULL : q_typed_empty_like(ray_list_new(0), fv);
}

/* item i of an iterated argument; a non-collection is held whole and
 * broadcast, as for the maps (`{x+y*z}\[1000 2000;5 10 15 20;3]`, :355) */
static ray_t* acc_item(ray_t* v, int64_t i) {
    if (!q_type_is_iter(v)) { ray_retain(v); return v; }
    return q_index_elem_at(v, i);
}

/* Binary and higher-rank values (:217, :322): the running left argument is
 * `seed`, or — applied as a unary with no known identity — the first item,
 * which is then the first result (:272).  it[0..nit) are the iterated
 * arguments; the count is the longest of them. */
static ray_t* acc_reduce(ray_t* fv, const q_op_t* frow, ray_t* seed,
                         ray_t** it, int64_t nit, int keep) {
    if (nit < 1 || nit + 1 > APPLY_MAX_ARGS) return q_err(QE_RANK);
    ray_t* iv[APPLY_MAX_ARGS];
    ray_t* av[APPLY_MAX_ARGS];
    ray_t* dk = NULL;                 /* first dict argument: the result's keys */
    int64_t len = -1;
    for (int64_t p = 0; p < nit; p++) {
        ray_retain(it[p]);
        iv[p] = q_eval_apply_concrete(it[p]);   /* an iterated DAG has no items */
        ray_t* dv = iter_dict_vals(iv[p]);
        if (dv) {                     /* iterate the VALUES, not the entries */
            ray_retain(dv);
            if (!dk) { dk = iv[p]; ray_retain(dk); }
            ray_release(iv[p]);
            iv[p] = dv;
        }
        if (!q_type_is_iter(iv[p])) continue;
        int64_t c = q_count_long(iv[p]);
        if (len < 0 || c > len) len = c;
    }
    ray_t* r;
    if (len == 0) {
        r = acc_empty(fv, frow, seed, keep, 0);      /* :385, seeded branch */
    } else if (len < 0) {
        /* nothing to iterate over: one evaluation, and the result stays an
         * atom — the derived function is uniform */
        if (seed) {
            av[0] = seed;
            for (int64_t p = 0; p < nit; p++) av[p + 1] = iv[p];
            r = q_eval_apply(fv, frow, av, nit + 1);
        } else {
            ray_retain(iv[0]);
            r = iv[0];
        }
    } else {
        acc_t a;
        acc_init(&a, keep);
        ray_t* err = NULL;
        ray_t* cur;
        int64_t i0 = 0;
        if (seed) { ray_retain(seed); cur = seed; }
        else      { cur = acc_item(iv[0], 0); i0 = 1; err = acc_push(&a, cur); }
        for (int64_t i = i0; !err && i < len; i++) {
            av[0] = cur;
            for (int64_t p = 0; p < nit; p++) av[p + 1] = acc_item(iv[p], i);
            ray_t* nx = q_eval_apply(fv, frow, av, nit + 1);
            for (int64_t p = 0; p < nit; p++) ray_release(av[p + 1]);
            ray_release(cur);
            if (RAY_IS_ERR(nx)) { cur = NULL; err = nx; break; }
            cur = nx;
            err = acc_push(&a, cur);
        }
        if (cur) ray_release(cur);
        r = acc_finish(&a, err);
    }
    for (int64_t p = 0; p < nit; p++) ray_release(iv[p]);
    if (dk) {
        if (keep) r = dict_rekey(dk, r);
        ray_release(dk);
    }
    return r;
}

/* An accumulator's value may be a function, a list or a dictionary — the last
 * two being finite-state machines (:59) whose rank is their depth: a simple
 * vector maps one index, a list of lists two (`7 m\c`, :252). */
static int64_t acc_rank(ray_t* fv) {
    if (fv->type == RAY_LIST) {
        ray_t* it = ray_len(fv) ? ((ray_t**)ray_data(fv))[0] : NULL;
        return (q_type_is_iter(it) && it->type != RAY_TABLE) ? 2 : 1;
    }
    if (fv->type == RAY_DICT || fv->type == RAY_TABLE || ray_is_vec(fv))
        return 1;
    return q_eval_apply_rank(fv);
}

/* The manifest mono column: `f/` of a registered dyad IS its aggregate kernel
 * — `+/` joins the DAG as OP_SUM where a naive fold would materialize and go
 * scalar.  `\` reads its OWN column: `,` reduces to raze and scans to nothing,
 * so no spelling rule derives one from the other. */
static const char* acc_mono_name(const q_op_t* frow, int keep) {
    return !frow ? NULL : (keep ? frow->mono_scan : frow->mono);
}

static ray_t* acc_mono(const q_op_t* frow, int keep, const q_op_t** mrow) {
    const char* nm = acc_mono_name(frow, keep);
    if (!nm) return NULL;
    ray_t* mv = q_eval_apply_manifest_value(q_ops_find(nm, (int)strlen(nm)),
                                            Q_MONADIC, mrow);
    return (mv && q_eval_apply_is_fnval(mv)) ? mv : NULL;
}

/* Unary application of a non-unary value (:259): the seed is the value's
 * identity element when q knows one, else the first item — which is then the
 * first result. */
static ray_t* acc_unary(ray_t* fv, const q_op_t* frow, ray_t* x, int keep) {
    ray_retain(x);
    x = q_eval_apply_concrete(x);              /* materialize at the boundary */
    const q_op_t* mrow = NULL;
    /* boxed lists take the native fold — the aggregate wrappers' boxed arms
     * ride base call_fn plumbing (finding 5) */
    ray_t* dv = iter_dict_vals(x);
    if (dv) {                    /* the values are the domain; Scan re-keys */
        ray_t* dr = acc_unary(fv, frow, dv, keep);
        if (keep) dr = dict_rekey(x, dr);
        ray_release(x);
        return dr;
    }
    ray_t* mv = (x->type == RAY_LIST) ? NULL : acc_mono(frow, keep, &mrow);
    ray_t* r;
    if (q_type_is_iter(x) && q_count_long(x) == 0) {
        r = acc_empty(fv, frow, NULL, keep, mv != NULL);
        if (!r) r = q_eval_apply(mv, mrow, &x, 1);
    } else if (mv)
        r = q_eval_apply(mv, mrow, &x, 1);
    else {
        ray_t* id = frow ? q_ops_acc_identity(frow->name) : NULL;
        r = acc_reduce(fv, frow, id, &x, 1, keep);
        if (id) ray_release(id);
    }
    ray_release(x);
    return r;
}

/* Scan `\` and Over `/`: one classifier over the doc's own taxonomy — the
 * value's rank picks unary-value control flow (Converge / Do / While, :81)
 * or the reduction of a binary-or-higher value (:217, :322). */
static ray_t* acc_apply(ray_t* fv, const q_op_t* frow, ray_t** args,
                        int64_t n, int keep) {
    if (n < 1) return q_err(QE_RANK);
    /* the mono column is the ROW's property and is read before the value:
     * `|`'s dyad is a deferred cell, so its operand value is monadic `reverse`
     * and a rank test would read Converge where the manifest says max */
    if (n == 1 && acc_mono_name(frow, keep))
        return acc_unary(fv, frow, args[0], keep);
    /* a LIST value applied as a unary to an empty argument yields an empty of
     * the VALUE's own type, unevaluated (:436) — this outranks the rank test,
     * which would otherwise read a simple vector as a Converge machine */
    if (n == 1 && !q_eval_apply_is_fn(fv) && q_type_is_iter(args[0]) &&
        args[0]->type != RAY_TABLE && ray_len(args[0]) == 0)
        return q_typed_empty_like(ray_list_new(0), fv);
    int64_t rank = acc_rank(fv);
    if (rank < 0 && q_eval_apply_fnv_matrix_row(frow)) rank = 2;   /* `(!/)x` reduces */
    /* a variadic value reads as the UNARY form in both binary shapes:
     * `5 enlist\1` (:151) is Do-5 of unary enlist, not enlist[5;1].  Past two
     * arguments there is no unary reading left and a variadic ternary
     * (`ssr\[s;x;y]`) reduces */
    if (rank == 1 || (rank < 0 && n <= 2)) {
        if (n == 1) return acc_converge(fv, frow, args[0], keep);
        if (n != 2) return q_err(QE_RANK);
        /* the left argument separates the two binary forms: an integer counts
         * the passes, anything else is the unary truth map (:95) */
        if (!q_type_is_int_atom(args[0]))
            return acc_iterate(fv, frow, args[0], 0, args[1], keep);
        int64_t k = q_type_as_i64(args[0]);
        if (k < 0) return q_err(QE_DOMAIN);
        return acc_iterate(fv, frow, NULL, k, args[1], keep);
    }
    if (n == 1) return acc_unary(fv, frow, args[0], keep);
    if (rank >= 2 && n != rank) return q_err(QE_RANK);
    return acc_reduce(fv, frow, args[0], args + 1, n - 1, keep);
}

/* Each, Each Left and Each Right are ONE law (ref/maps.md: `x f\:y` is
 * `f[;y] each x`).  `mask` names the ITERATED positions — fixed by the
 * operator, never auto-detected — read at i; every other position is held
 * whole, and a non-collection in a masked slot leaves nothing to iterate. */
static ray_t* map_zip(ray_t* fv, const q_op_t* frow, ray_t** args, int64_t n,
                      uint64_t mask) {
    if (n < 1 || n > APPLY_MAX_ARGS) return q_err(QE_RANK);
    ray_t* av[APPLY_MAX_ARGS];
    uint64_t iter = 0;
    int64_t len = -1;
    ray_t* r = NULL;
    for (int64_t p = 0; p < n; p++) {
        ray_retain(args[p]);
        av[p] = q_eval_apply_concrete(args[p]);   /* an iterated DAG has no items */
        if (!(mask >> p & 1) || !q_type_is_iter(av[p])) continue;
        iter |= (uint64_t)1 << p;
        /* the scan must run to the end so every av[p] is populated for the
         * release below — so record the FIRST mismatch only (an error is an
         * allocation the refcount system does not track: overwriting leaks) */
        if (len < 0) len = q_count_long(av[p]);
        else if (len != q_count_long(av[p]) && !r) r = q_err(QE_LENGTH);
    }
    if (!r && len < 0) r = q_eval_apply(fv, frow, av, n);
    /* a map is uniform: an empty iterated side returns the GENERIC empty list
     * without an evaluation — `type (2*')til 0` is 0h, not 7h (ref/maps.md) */
    if (!r && len == 0) r = ray_list_new(0);
    if (!r) {
        ray_t* l = ray_list_new(len);
        ray_t* ev[APPLY_MAX_ARGS];
        for (int64_t i = 0; i < len; i++) {
            for (int64_t p = 0; p < n; p++)
                ev[p] = (iter >> p & 1) ? q_index_elem_at(av[p], i) : av[p];
            ray_t* e = q_eval_apply_concrete(q_eval_apply(fv, frow, ev, n));
            for (int64_t p = 0; p < n; p++)
                if (iter >> p & 1) ray_release(ev[p]);
            if (RAY_IS_ERR(e)) { ray_release(l); l = e; break; }
            l = ray_list_append(l, e);
            ray_release(e);
        }
        r = RAY_IS_ERR(l) ? l : q_eval_apply_collapse(l);
    }
    for (int64_t p = 0; p < n; p++) ray_release(av[p]);
    return r;
}

static ray_t* each1(ray_t* fv, const q_op_t* frow, ray_t* x) {
    ray_t* dv = iter_dict_vals(x);
    if (dv) return dict_rekey(x, each1(fv, frow, dv));
    return map_zip(fv, frow, &x, 1, 1);
}

/* Case (ref/maps.md "Case"): an INTEGER VECTOR at the `'` head is not a value
 * to apply — per index it picks WHICH argument to read, r[i] = args[sel[i]][i].
 * Atom arguments are infinitely repeated and extra arguments are not a rank
 * error, so the count comes from the selector alone. */
static ray_t* case_apply(ray_t* sel, ray_t** args, int64_t n) {
    if (n < 1 || n > APPLY_MAX_ARGS) return q_err(QE_RANK);
    ray_t* av[APPLY_MAX_ARGS];
    for (int64_t p = 0; p < n; p++) {
        ray_retain(args[p]);
        av[p] = q_eval_apply_concrete(args[p]);   /* a picked-from DAG has no items */
    }
    int64_t len = ray_len(sel);
    ray_t* l = ray_list_new(len);
    for (int64_t i = 0; i < len && !RAY_IS_ERR(l); i++) {
        ray_t* si = q_index_elem_at(sel, i);
        int64_t k = q_type_is_int_atom(si) ? q_type_as_i64(si) : -1;
        ray_release(si);
        ray_t* e;
        if (k < 0 || k >= n || !av[k]) e = q_err(QE_INDEX);
        else if (q_type_is_iter(av[k]) && av[k]->type != RAY_TABLE)
            e = q_index_elem_at(av[k], i);
        else { ray_retain(av[k]); e = av[k]; }
        if (RAY_IS_ERR(e)) { ray_release(l); l = e; break; }
        l = ray_list_append(l, e);
        ray_release(e);
    }
    for (int64_t p = 0; p < n; p++) ray_release(av[p]);
    return RAY_IS_ERR(l) ? l : q_eval_apply_collapse(l);
}

/* Each Prior: fv between each item and the one before it.  Applied as a
 * binary the left argument IS the seed; applied as a unary the seed is the
 * value's identity element when q knows one, else `first 0#x` (ref/maps.md
 * Each Prior). */
static ray_t* prior_each(ray_t* fv, const q_op_t* frow, ray_t* seed, ray_t* x) {
    ray_t* dv = iter_dict_vals(x);               /* prior is uniform: re-keys */
    if (dv) return dict_rekey(x, prior_each(fv, frow, seed, dv));
    ray_retain(x);
    x = q_eval_apply_concrete(x);
    ray_t* prev = seed;
    if (prev) ray_retain(prev);
    else {
        prev = frow ? q_ops_acc_identity(frow->name) : NULL;
        if (!prev && ray_is_vec(x)) prev = ray_typed_null((int8_t)-x->type);
        /* `first 0#x` for a table is its all-null row — the out-of-range read */
        if (!prev && x->type == RAY_TABLE) prev = q_index_elem_at(x, q_count_long(x));
        if (!prev) { prev = RAY_NULL_OBJ; ray_retain(prev); }
    }
    ray_t* r;
    if (!q_type_is_iter(x)) {
        ray_t* av[2] = { x, prev };
        r = q_eval_apply(fv, frow, av, 2);
    } else {
        int64_t len = q_count_long(x);
        ray_t* l = ray_list_new(len);
        for (int64_t i = 0; i < len; i++) {
            ray_t* cur = q_index_elem_at(x, i);
            ray_t* av[2] = { cur, prev };
            ray_t* e = q_eval_apply_concrete(q_eval_apply(fv, frow, av, 2));
            ray_release(prev);
            prev = cur;
            if (RAY_IS_ERR(e)) { ray_release(l); l = e; break; }
            l = ray_list_append(l, e);
            ray_release(e);
        }
        r = RAY_IS_ERR(l) ? l : q_eval_apply_collapse(l);
    }
    ray_release(prev);
    ray_release(x);
    return r;
}

ray_t* q_adverb_apply(int adv, ray_t* fv, const q_op_t* frow,
                           ray_t** args, int64_t n) {
    if (!fv) return q_err(QE_TYPE);
    /* an elided argument projects the DERIVED value, not the underlying one:
     * `2*'` is Each on a projection of `*`, and only the empty case tells them
     * apart (`type (2*')til 0` is 0h where `type (2*)til 0` is 7h) */
    for (int64_t i = 0; i < n; i++) {
        if (args[i]) continue;
        ray_t* d = q_eval_apply_deriv_new(adv, fv, frow);
        if (RAY_IS_ERR(d)) return d;
        ray_t* p = q_eval_apply_proj_new(d, NULL, args, n, n);
        ray_release(d);
        return p;
    }
    if (adv == 1 || adv == 2) return acc_apply(fv, frow, args, n, adv == 2);
    if (adv == 0) {                                        /* `'` each */
        if (q_type_is_int_vec(fv)) return case_apply(fv, args, n);
        if (n == 1) return each1(fv, frow, args[0]);
        return map_zip(fv, frow, args, n, ~(uint64_t)0);
    }
    if (adv == 3) {                                        /* `':` */
        /* a rank-1 value makes `':` Each Parallel, a rank-2 one Each Prior
         * (ref/maps.md); we have no secondary tasks, so parallel IS each */
        if (n == 1 && q_eval_apply_rank(fv) == 1)
            return each1(fv, frow, args[0]);
        if (n == 1) return prior_each(fv, frow, NULL, args[0]);
        if (n == 2) return prior_each(fv, frow, args[0], args[1]);
        return q_err(QE_RANK);
    }
    if (adv == 4 && n == 2) return map_zip(fv, frow, args, 2, 2);   /* /: right */
    if (adv == 5 && n == 2) return map_zip(fv, frow, args, 2, 1);   /* \: left  */
    return q_err(QE_NYI);                    /* the vs/sv unary forms: later */
}

/* keyword-HOF row (each/peach/over/scan/prior): adverb_hof -> adv id */
int q_adverb_hof_id(const char* hof) {
    if (strcmp(hof, "map") == 0)   return 0;
    if (strcmp(hof, "fold") == 0)  return 1;
    if (strcmp(hof, "scan") == 0)  return 2;
    if (strcmp(hof, "prior") == 0) return 3;
    return -1;
}
