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

static ray_t* dict_lookup(ray_t* d, ray_t* idx) {
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

ray_t* q_apply_noun(ray_t* head, ray_t** args, int64_t n) {
    if (!head) return NULL;

    /* 100h lambda carriers — q rank/projection semantics.  Claimed before
     * the n guard: a rank-0 call shape must not fall through. */
    if (head->type == RAY_LIST && q_deriv_kind_of(head) == Q_DERIV_LAMBDA)
        return q_lambda_apply(head, args, n);

    if (n < 1) return NULL;

    if (head->type == -RAY_STR) return NULL;        /* deferred: string model */

    /* 104h carriers are RAY_LISTs — claim them BEFORE the gather arm. */
    if (head->type == RAY_LIST && q_deriv_kind_of(head) != Q_DERIV_NONE)
        return q_deriv_apply(head, args, n);

    if (head->type == RAY_DICT) {
        if (n != 1) return NULL;                    /* d[k;..] deferred */
        return dict_lookup(head, args[0]);
    }

    if (head->type == RAY_TABLE) {
        /* t[`col] -> column, t[0] -> row dict — base ray_at special-cases
         * tables and both forms audited kdb-sane. */
        if (n != 1) return NULL;
        return ray_at_fn(head, args[0]);
    }

    if (ray_is_vec(head) || head->type == RAY_LIST) {
        /* v[i] / v[1 3] / v i; v[i;j] = depth indexing, one gather per arg */
        ray_t* cur = head;
        ray_retain(cur);
        for (int64_t i = 0; i < n; i++) {
            ray_t* next = gather(cur, args[i]);
            ray_release(cur);
            if (!next || RAY_IS_ERR(next)) return next;
            cur = next;
        }
        return cur;
    }

    return NULL;
}
