/* q_value.c — the q `value` verb (moved out of q_wrap_castcal.c, castcal
 * split 2026-07-22; shared internal surface: q_registry_internal.h). */
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_ns.h"     /* q_ns_current/ctx_dict/root_dict — value-of-sym name resolution */
#include "qlang/q_deriv.h"  /* q_deriv_kind_of/base, q_lambda_src — value on carriers */
#include "qlang/q_parse.h"  /* q_parse/q_lower — value-of-string (RUNTIME wrapper only; builders must never parse, rule 6) */
#include "lang/env.h"       /* ray_env_get */
#include "lang/eval.h"      /* ray_eval, LAMBDA_PARAMS */
#include "table/sym.h"      /* RAY_SYM_W64 */
#include <stdio.h>          /* snprintf */
#include <string.h>
#include <stdlib.h>

/* ===== q `value` (ref/value.md) — the full form matrix =====================
 * value is monadic; its ONE argument selects the behaviour by shape:
 *   dict            -> vals (collapsed)
 *   symbol atom     -> value of the variable it names (env / .q namespace / kw)
 *   enumeration     -> [deferred: no enum type]
 *   string          -> parse+lower+eval in the current context
 *   list            -> SINGLE apply/index of the head over the rest (a value
 *                      OBJECT apply — NOT eval's recursive parse-tree walk; if the
 *                      head is a symbol/string it is resolved first)
 *   projection      -> (function; bound args…)
 *   derived function-> argument of the outer iterator
 *   operator        -> internal opcode integer
 *   lambda          -> the V3.5 structure list
 * eval-vs-value: `value(f;args)` is ONE apply of f to the args-as-data; `eval`
 * would descend recursively.  They coincide only for flat args (ARCHITECTURE.md).
 */

/* Parse+lower+eval a q source string to a value (runtime-only; the registry is
 * warm here — rule 6 forbids q_parse in BUILDERS, not runtime wrappers, per the
 * q_select_exec precedent).  Returns owned value or RAY_ERROR. */
static ray_t* q_value_eval_str(ray_t* strv) {
    const char* sp = ray_str_ptr(strv);
    size_t sl = ray_str_len(strv);
    char* s = malloc(sl + 1);
    if (!s) return ray_error("wsfull", "value: out of memory");
    memcpy(s, sp, sl);
    s[sl] = '\0';
    ray_t* ast = q_parse(s);
    free(s);
    if (!ast || RAY_IS_ERR(ast)) return ast ? ast : ray_error("parse", NULL);
    ast = q_lower(ast);
    if (!ast || RAY_IS_ERR(ast)) return ast ? ast : ray_error("parse", NULL);
    ray_t* r = ray_eval(ast);
    ray_release(ast);
    return r;
}

/* Resolve a q symbol atom to its value (ref/value.md: "symbol atom -> value of
 * the variable it names").  Order: env (user vars + builtins) for the full name;
 * else strip a leading `.q.` and retry env; else the registry keyword table
 * (monadic then dyadic).  Returns a BORROWED ref (env/registry own it) or NULL. */
static ray_t* q_value_resolve_sym(ray_t* symv) {
    if (!symv || symv->type != -RAY_SYM) return NULL;
    ray_t* s = ray_sym_str(symv->i64);
    if (!s) return NULL;
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    ray_t* r = ray_env_get(symv->i64);            /* borrowed; full name */
    const char* base = nm; size_t bl = l;
    if (!r && l > 3 && nm[0] == '.' && nm[1] == 'q' && nm[2] == '.') {
        base = nm + 3; bl = l - 3;
        int64_t bid = ray_sym_intern(base, bl);
        r = ray_env_get(bid);                     /* borrowed */
    }
    if (!r) {
        r = q_registry_lookup_name(base, bl, Q_MONADIC);   /* borrowed */
        if (!r) r = q_registry_lookup_name(base, bl, Q_DYADIC);
    }
    ray_release(s);
    return r;                                     /* borrowed */
}

/* Owned-resolution variant for `get`/`value`/`key` sym arms — the q-namespace
 * views SYNTHESIZE fresh dicts, so this returns OWNED refs on every path (the
 * borrowed q_value_resolve_sym contract stays untouched):
 *   `.        -> root context dict (user vars, no :: placeholder)
 *   `.foo     -> context dict with the leading :: placeholder
 *   `..name   -> the ROOT variable `name` (k-style root qualification)
 *   `name     -> under `\d .ctx`, `.ctx.name` first (kdb: unqualified names
 *                are context-relative; reserved words stay reserved), else
 *                the plain env/registry resolution.
 * Returns NULL when unresolved. */
ray_t* q_value_resolve_sym_owned(ray_t* symv) {
    if (!symv || symv->type != -RAY_SYM) return NULL;
    ray_t* s = ray_sym_str(symv->i64);
    if (!s) return NULL;
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    if (l == 1 && nm[0] == '.') {                 /* `. — root view */
        ray_release(s);
        return q_ns_root_dict();
    }
    if (l > 2 && nm[0] == '.' && nm[1] == '.') {  /* `..name — root variable */
        int64_t rid = ray_sym_intern(nm + 2, l - 2);
        ray_release(s);
        ray_t* v = ray_env_get(rid);
        if (!v) return NULL;
        ray_retain(v);
        return v;
    }
    if (l >= 2 && nm[0] == '.' && nm[1] != '.') {
        /* root OR nested context handle */
        ray_t* d = q_ns_ctx_dict(nm, l);          /* owned or NULL */
        if (d) { ray_release(s); return d; }
        /* not a context — fall through (member reads like `.jab.wrong,
         * flat dotted bindings like `.z.f, data dicts like `.foo.a) */
    }
    /* under \d .ctx an unqualified user name is context-relative */
    const char* ctx = q_ns_current();
    if (ctx[0] && l > 0 && nm[0] != '.' && !q_ns_is_unqualifiable(nm, l)) {
        char full[192];
        int fl = snprintf(full, sizeof full, "%s.%.*s", ctx, (int)l, nm);
        ray_release(s);
        if (fl <= 0 || (size_t)fl >= sizeof full) return NULL;
        ray_t* v = ray_env_get(ray_sym_intern(full, (size_t)fl));
        if (!v) return NULL;
        ray_retain(v);
        return v;
    }
    ray_release(s);
    ray_t* v = q_value_resolve_sym(symv);         /* borrowed */
    if (!v) return NULL;
    ray_retain(v);
    return v;                                     /* owned from here on */
}

/* Public alias — see q_registry.h (q_apply's symbol-handle arm). */
ray_t* q_value_resolve_owned(ray_t* symv) {
    return q_value_resolve_sym_owned(symv);
}

/* Single-apply a resolved head VALUE to argc data args (borrowed).  Value-object
 * apply: args are DATA — q_call_n routes callables (call_fn1/2, carrier/lambda-
 * aware) and INDEXES a noun head, never re-evaluating the args.  argc==0 => the
 * head itself (retained).  Returns owned result or RAY_ERROR. */
static ray_t* q_value_apply_head(ray_t* head, ray_t** args, int64_t argc) {
    if (!head) return ray_error("type", "value: nil head");
    if (argc == 0) { ray_retain(head); return head; }
    if (argc > 64) return ray_error("limit", "value: too many args");
    return q_call_n(head, args, argc);            /* borrowed args, owned result */
}

/* True iff `base` is one of the six adverb HOF values a q derived function
 * projects over (over/scan/each/prior/map-right/map-left).  A Q_DERIV_PROJ over
 * one of these is a DERIVED FUNCTION (value -> the iterator's argument); over
 * anything else it is a plain PROJECTION (value -> function + bound args).  The
 * env HOF ids are interned once (lazy), mirroring q_deriv's marker cache. */
static int q_value_is_adverb_hof(ray_t* base) {
    if (!base) return 0;
    /* Re-intern the env HOF ids per call: the sym table is recreated per runtime
     * (q_runtime_destroy tears it down), so a `static` id cache would go STALE for
     * a new runtime in the same process (the C unit suites do exactly that).
     * `value` is not hot, and ray_sym_intern is a cheap hash lookup when present. */
    return base == q_registry_over_value()
        || base == ray_env_get(ray_sym_intern("fold", 4))
        || base == q_registry_scan_value()
        || base == q_registry_lookup_name("each", 4, Q_DYADIC)
        || base == q_registry_prior_value()
        || base == ray_env_get(ray_sym_intern("map-right", 9))
        || base == ray_env_get(ray_sym_intern("map-left", 8));
}

ray_t* q_value_wrap(ray_t* x) {
    if (!x) return ray_error("type", "value: nil");

    /* ---- carriers (all RAY_LIST — matched BEFORE the general-list arm) ---- */
    q_deriv_kind dk = q_deriv_kind_of(x);

    /* lambda carrier -> the kdb lambda-structure list (ref/value.md#lambda,
     * V3.5 shape, best-effort fields):
     *   [0] bytecode (empty 0x)  [1] params  [2] locals  [3] globals
     *   [4] source map -1  [5] name ""  [6] file ""  [7] line -1  [8] source
     * The doc-pinned fields are [1] (params) and LAST (source text). */
    if (dk == Q_DERIV_LAMBDA) {
        ray_t* lam = q_deriv_base(x);                 /* borrowed */
        ray_t* src = q_lambda_src(x);                 /* borrowed */
        if (!lam || !src) return ray_error("type", "value: malformed lambda");
        ray_t* l = ray_list_new(9);
        ray_t* t;
        t = ray_vec_new(RAY_BYTE_ONLY, 0);           l = ray_list_append(l, t); ray_release(t);
        l = ray_list_append(l, LAMBDA_PARAMS(lam));   /* borrowed; append retains */
        t = ray_sym_vec_new(RAY_SYM_W64, 0);  l = ray_list_append(l, t); ray_release(t);
        t = ray_sym_vec_new(RAY_SYM_W64, 0);  l = ray_list_append(l, t); ray_release(t);
        t = ray_i64(-1);                      l = ray_list_append(l, t); ray_release(t);
        t = ray_str("", 0);                   l = ray_list_append(l, t); ray_release(t);
        t = ray_str("", 0);                   l = ray_list_append(l, t); ray_release(t);
        t = ray_i64(-1);                      l = ray_list_append(l, t); ray_release(t);
        l = ray_list_append(l, src);
        return l;
    }

    if (dk == Q_DERIV_PROJ) {
        ray_t* base   = q_deriv_base(x);              /* borrowed, idx 1 */
        int64_t n     = ray_len(x);
        ray_t** e     = (ray_t**)ray_data(x);
        if (q_value_is_adverb_hof(base)) {
            /* derived function -> the argument of the iterator (idx 4).  The
             * adverb-derived carrier always binds EXACTLY the inner value there
             * (q_lower builds it as `(hof; V, hole)`); assert the invariant so a
             * future multi-arg adverb projection can't silently drop args. */
            uint64_t mask = q_deriv_hole_mask(x);
            int64_t bound = 0;
            for (int64_t i = 0; i < n - 4; i++) if (!(mask & (1ull << i))) bound++;
            if (n < 5 || bound != 1 || !e[4])
                return ray_error("type", "value: unexpected derived-fn shape");
            ray_retain(e[4]);
            return e[4];
        }
        /* plain projection -> (base; bound-args…), holes omitted (ref/value.md:
         * "list: function followed by argument/s"). */
        uint64_t mask = q_deriv_hole_mask(x);
        int64_t slots = n - 4;
        ray_t* out = ray_list_new(slots + 1);
        out = ray_list_append(out, base);             /* borrowed; append retains */
        for (int64_t i = 0; i < slots; i++)
            if (!(mask & (1ull << i))) out = ray_list_append(out, e[4 + i]);
        return out;
    }

    if (dk == Q_DERIV_MONAD || dk == Q_DERIV_COMPOSE)
        return ray_error("nyi", "value: composition/monadic-mark deferred");

    /* ---- symbol atom -> variable/keyword/namespace value ---- */
    if (x->type == -RAY_SYM) {
        ray_t* v = q_value_resolve_sym_owned(x);      /* OWNED (or NULL) */
        if (!v) return ray_error("name", "value: unresolved symbol");
        return v;
    }

    /* ---- string / char vector -> evaluate as q source ---- */
    if (x->type == -RAY_STR) return q_value_eval_str(x);
    if (x->type == RAY_CHARV || x->type == -RAY_CHARV) {
        ray_t* s = q_str_of_charv(x);
        if (!s || RAY_IS_ERR(s)) return s ? s : ray_error("oom", NULL);
        ray_t* r = q_value_eval_str(s);
        ray_release(s);
        return r;
    }

    /* ---- general list -> single apply/index of the head over the rest ---- */
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (n < 1) return ray_error("type", "value: empty list");
        ray_t* head = e[0];                           /* borrowed */
        if (head && head->type == -RAY_SYM) {         /* form 2: symbol head */
            ray_t* hv = q_value_resolve_sym_owned(head);  /* OWNED (or NULL) */
            if (!hv) return ray_error("name", "value: unresolved symbol head");
            ray_t* r = q_value_apply_head(hv, e + 1, n - 1);
            ray_release(hv);
            return r;
        }
        if (head && (head->type == -RAY_STR || head->type == RAY_CHARV ||
                     head->type == -RAY_CHARV)) {
            ray_t* hs = head->type != -RAY_STR ? q_str_of_charv(head)
                                               : (ray_retain(head), head);
            ray_t* hv = q_value_eval_str(hs);         /* form 3: string head */
            ray_release(hs);
            if (!hv || RAY_IS_ERR(hv)) return hv ? hv : ray_error("parse", NULL);
            ray_t* r = q_value_apply_head(hv, e + 1, n - 1);
            ray_release(hv);
            return r;
        }
        return q_value_apply_head(head, e + 1, n - 1);  /* form 1: value head */
    }

    /* ---- dict -> vals (collapsed) ---- */
    if (x->type == RAY_DICT) {
        ray_t* v = ray_dict_vals(x);                /* borrowed */
        if (!v) return ray_error("type", "value: nil vals");
        if (v->type == RAY_LIST) return q_collapse_list(v);   /* returns owned */
        ray_retain(v);
        return v;
    }

    return ray_error("type", "value: unsupported operand (other forms deferred)");
}
