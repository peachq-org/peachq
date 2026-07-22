/* q derived-verb / projection carriers — see q_deriv.h.
 *
 * Inert boxed carriers (STAGE 2a): built + inspectable, never evaluated.  A
 * carrier is a RAY_LIST laid out as:
 *   PROJ   : [ `.q.proj , base, hole_mask(i64), eff_valence(i64), arg0, ... ]
 *   MONAD  : [ `.q.monad, base, eff_valence(i64) ]
 * element[0] is a QUOTED marker sym so the box self-identifies.  (Adverbs are
 * NOT carriers — they map to rayfall HOFs in the op manifest; see q_deriv.h.) */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_deriv.h"
#include "qlang/q_registry_internal.h" /* q_strict_i64 (mkopproj n/mask reads) */
#include "qlang/q_apply.h" /* q_apply_noun — q_deriv_call_n's n>=3 route */
#include "lang/env.h"      /* ray_env_get — q_deriv_fn_make's base `fn` form */
#include "lang/eval.h"     /* LAMBDA_PARAMS — q_deriv_fn_rank */
#include "lang/internal.h" /* call_fn1/2, ray_error */
#include <string.h>

#define Q_DERIV_QUOTED 0x20   /* ATTR_QUOTED (src/lang/eval.h) — literal sym */

/* Cached marker sym-ids (lazily interned; -1 = not yet). */
static int64_t g_sid_proj   = -1;
static int64_t g_sid_monad  = -1;
static int64_t g_sid_hole   = -1;  /* unbound-arg `;` sentinel in a projection */
static int64_t g_sid_lambda = -1;
static int64_t g_sid_compose = -1;

void q_deriv_reset_markers(void) {
    g_sid_proj = g_sid_monad = g_sid_hole = g_sid_lambda = -1;
}

static void ensure_markers(void) {
    if (g_sid_proj < 0) {
        g_sid_proj   = ray_sym_intern(".q.proj",   7);
        g_sid_monad  = ray_sym_intern(".q.monad",  8);
        g_sid_hole   = ray_sym_intern(".q.hole",   7);
        g_sid_lambda = ray_sym_intern(".q.lambda", 9);
        g_sid_compose = ray_sym_intern(".q.compose", 10);
    }
}

static ray_t* marker_atom(int64_t sid) {
    ray_t* m = ray_sym(sid);
    if (m && !RAY_IS_ERR(m)) m->attrs |= Q_DERIV_QUOTED;
    return m;
}

/* Append one owned child into `l`, releasing it (append retains).  Returns the
 * (possibly-reallocated) list. */
static ray_t* push_owned(ray_t* l, ray_t* child) {
    l = ray_list_append(l, child);
    ray_release(child);
    return l;
}

/* Append `base` (borrowed by caller) — append retains it, so no extra ref. */
static ray_t* push_borrowed(ray_t* l, ray_t* v) {
    return ray_list_append(l, v);
}

ray_t* q_proj_new(ray_t* base, ray_t** args, int64_t argc, uint64_t hole_mask,
                  int eff_valence) {
    if (!base) return ray_error("type", "q_proj_new: nil base");
    ensure_markers();
    ray_t* l = ray_list_new(4 + (argc > 0 ? argc : 0));
    l = push_owned(l, marker_atom(g_sid_proj));
    l = push_borrowed(l, base);
    l = push_owned(l, ray_i64((int64_t)hole_mask));
    l = push_owned(l, ray_i64((int64_t)eff_valence));
    for (int64_t i = 0; i < argc; i++) {
        if (args[i]) l = push_borrowed(l, args[i]);
        else {
            /* an unbound-arg hole — a dedicated quoted `.q.hole` sentinel so the
             * slot count matches argc; the authoritative hole record is
             * `hole_mask` (this filler is never inspected in 2a). */
            ray_t* hole = marker_atom(g_sid_hole);
            l = push_owned(l, hole);
        }
    }
    return l;
}

ray_t* q_lambda_carrier_new(ray_t* base, int rank, ray_t* src) {
    if (!base) return ray_error("type", "q_lambda_carrier_new: nil base");
    if (!src)  return ray_error("type", "q_lambda_carrier_new: nil src");
    ensure_markers();
    ray_t* l = ray_list_new(4);
    l = push_owned(l, marker_atom(g_sid_lambda));
    l = push_borrowed(l, base);
    l = push_owned(l, ray_i64((int64_t)rank));   /* valence at idx 2 = MONAD layout */
    l = push_borrowed(l, src);
    return l;
}

ray_t* q_compose_new(ray_t** fns, int64_t nf) {
    if (nf < 1) return ray_error("rank", "q_compose_new: needs >=1 function");
    ensure_markers();
    ray_t* l = ray_list_new(nf + 1);
    l = push_owned(l, marker_atom(g_sid_compose));
    for (int64_t i = 0; i < nf; i++) {
        if (!fns[i]) { ray_release(l); return ray_error("type", "compose: nil function"); }
        l = push_borrowed(l, fns[i]);   /* append retains */
    }
    return l;
}

ray_t* q_deriv_monadic_mark(ray_t* base) {
    if (!base) return ray_error("type", "q_deriv_monadic_mark: nil base");
    ensure_markers();
    ray_t* l = ray_list_new(3);
    l = push_owned(l, marker_atom(g_sid_monad));
    l = push_borrowed(l, base);
    l = push_owned(l, ray_i64(1));   /* monadic-marked: valence 1 */
    return l;
}

/* ---- inspectors ---- */

static int64_t head_sid(const ray_t* v) {
    if (!v || v->type != RAY_LIST || ray_len((ray_t*)v) < 2) return -1;
    ray_t* h = ((ray_t**)ray_data((ray_t*)v))[0];
    if (!h || h->type != -RAY_SYM) return -1;
    return h->i64;
}

/* Classify — AND validate the full fixed-field arity so every accessor below
 * can index its slots without bounds-checking (a forged/truncated list that
 * happens to carry a marker in slot 0 but lacks the fixed fields classifies as
 * NONE, never as a carrier whose fields would read past the body). */
q_deriv_kind q_deriv_kind_of(const ray_t* v) {
    ensure_markers();
    int64_t sid = head_sid(v);
    if (sid < 0) return Q_DERIV_NONE;
    int64_t n = ray_len((ray_t*)v);
    if (sid == g_sid_proj   && n >= 4) return Q_DERIV_PROJ;   /* marker,base,mask,val */
    if (sid == g_sid_monad  && n >= 3) return Q_DERIV_MONAD;  /* marker,base,val       */
    if (sid == g_sid_lambda && n >= 4) return Q_DERIV_LAMBDA; /* marker,base,val,src   */
    if (sid == g_sid_compose && n >= 2) return Q_DERIV_COMPOSE; /* marker,fn0,fn1,…    */
    return Q_DERIV_NONE;
}

int64_t q_deriv_compose_count(const ray_t* v) {
    if (q_deriv_kind_of(v) != Q_DERIV_COMPOSE) return 0;
    return ray_len((ray_t*)v) - 1;
}

ray_t* q_deriv_compose_fn_at(const ray_t* v, int64_t i) {
    if (q_deriv_kind_of(v) != Q_DERIV_COMPOSE) return NULL;
    int64_t nf = ray_len((ray_t*)v) - 1;
    if (i < 0 || i >= nf) return NULL;
    return ((ray_t**)ray_data((ray_t*)v))[1 + i];   /* borrowed */
}

ray_t* q_deriv_base(const ray_t* v) {
    if (q_deriv_kind_of(v) == Q_DERIV_NONE) return NULL;
    return ((ray_t**)ray_data((ray_t*)v))[1];   /* borrowed */
}

uint64_t q_deriv_hole_mask(const ray_t* v) {
    if (q_deriv_kind_of(v) != Q_DERIV_PROJ) return 0;
    ray_t* m = ((ray_t**)ray_data((ray_t*)v))[2];
    return (uint64_t)(m && m->type == -RAY_I64 ? m->i64 : 0);
}

ray_t* q_lambda_src(const ray_t* v) {
    if (q_deriv_kind_of(v) != Q_DERIV_LAMBDA) return NULL;
    return ((ray_t**)ray_data((ray_t*)v))[3];   /* borrowed */
}

int q_deriv_valence(const ray_t* v) {
    q_deriv_kind k = q_deriv_kind_of(v);
    if (k == Q_DERIV_NONE) return 0;
    int idx = (k == Q_DERIV_PROJ) ? 3 : 2;   /* PROJ: [.,.,mask,val]; MONAD: [.,.,val] */
    ray_t* ev = ((ray_t**)ray_data((ray_t*)v))[idx];
    return (int)(ev && ev->type == -RAY_I64 ? ev->i64 : 0);
}

/* ===== fn-value machinery (bucket B of the applyiter dissolution) ========
 * The callable-classification and generic-apply utilities over native fn
 * types + the carriers above — homed with the carrier representation so a
 * near-twin can never shadow them across a file boundary (the historic
 * q_is_fn_value bug class). */

/* Rank of a q value: 1 monadic, 2 dyadic, -1 ambiguous (native vary). */
int q_deriv_fn_rank(ray_t* f) {
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
int q_deriv_is_fn_value(ray_t* x) {
    if (!x) return 0;
    if (x->type == RAY_UNARY || x->type == RAY_BINARY ||
        x->type == RAY_VARY  || x->type == RAY_LAMBDA) return 1;
    return q_deriv_kind_of(x) != Q_DERIV_NONE;
}

/* Apply f to k args (borrowed).  1/2 route via call_fn1/2 (carrier-aware);
 * k>=3 via the noun dispatcher (lambda/proj carriers) or a native vary. */
ray_t* q_deriv_call_n(ray_t* f, ray_t** a, int64_t k) {
    if (k == 1) return call_fn1(f, a[0]);
    if (k == 2) return call_fn2(f, a[0], a[1]);
    if (f && f->type == RAY_VARY) return ((ray_vary_fn)(uintptr_t)f->i64)(a, k);
    ray_t* r = q_apply_noun(f, a, k);
    if (r) return r;
    return ray_error("rank", "each-both: cannot apply to %lld args", (long long)k);
}

/* Build a BINARY derived-verb carrier at EVAL time: `hof` with `f` bound in
 * slot 0 and two data operands open.  Used to lower `(f/:)` / `(f\:)` when the
 * operand f is an expression (a lambda) that must be EVALUATED to a value
 * first — a lower-time q_proj would capture the raw `(q.fn …)` tree.  `x f/: y`
 * then == map-right(f;x;y), which lets a stacked outer adverb (`f/:\:`) drive
 * it through map-left. */
ray_t* q_deriv_mkderiv2(ray_t* hof, ray_t* f) {
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
ray_t* q_deriv_mkopproj(ray_t** args, int64_t k) {
    if (k < 3) return ray_error("rank", "q.mkopproj: need base, n, mask");
    ray_t* base = args[0];
    int64_t n; int64_t m;
    if (!q_strict_i64(args[1], &n) || !q_strict_i64(args[2], &m))
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

/* q `'[f;g;…]` compose builder — a normal VARY (args are the resolved function
 * VALUES): boxes them into a Q_DERIV_COMPOSE carrier. */
ray_t* q_compose_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("rank", "': compose needs at least one function");
    return q_compose_new(args, n);
}

/* q.fn — SPECIAL FORM behind every q lambda literal.  q_lower rewrites the
 * parser's `{`-marker node to (q.fn src params body...); at eval this
 * delegates lambda creation to the base env `fn` form (same params/body
 * calling convention) and wraps the resulting RAY_LAMBDA in the 100h
 * .q.lambda carrier that carries q valence + verbatim source for display.
 * kdb caps lambdas at 8 arguments -> 'params — signalled HERE (not at parse:
 * qdoc error rows only match lower/eval errors). */
ray_t* q_deriv_fn_make(ray_t** args, int64_t n) {
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

static _Thread_local ray_t* qret_payload = NULL;

/* `:x` early return (basics/function-notation.md#explicit-return).  The body
 * must unwind NOW: eval aborts a lambda body on any RAY_ERROR, so we ride
 * the error path with the reserved class "q.ret" and stash the payload in a
 * thread-local for the innermost lambda application to take. */
ray_t* q_ret_fn(ray_t* x) {
    if (qret_payload) { ray_release(qret_payload); qret_payload = NULL; }
    if (x) ray_retain(x);
    qret_payload = x;
    return ray_error("q.ret", NULL);
}

ray_t* q_deriv_ret_take(void) {
    ray_t* v = qret_payload;        /* owned by the caller now */
    qret_payload = NULL;
    return v;
}
