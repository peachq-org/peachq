/* q_wrap_castcal.c — q `value` form matrix, the calendar home (days-from-civil),
 * and the cast/tok home ($ casts, string scans, pad)
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_ns.h"     /* q_ns_current/ctx_dict/root_dict — value-of-sym name resolution */
#include "qlang/q_deriv.h"  /* q_deriv_kind_of/base, q_lambda_src — value on carriers */
#include "qlang/q_parse.h"  /* q_parse/q_lower — value-of-string (RUNTIME wrapper only; builders must never parse, rule 6) */
#include "lang/env.h"       /* ray_env_get */
#include "lang/eval.h"      /* ray_eval, ray_cast_fn, LAMBDA_PARAMS */
#include "lang/internal.h"  /* ray_typed_null, ray_guid, ray_str_vec_get, ray_error */
#include "table/sym.h"      /* RAY_SYM_W64 */
#include "core/numparse.h"  /* ray_parse_i64/f64 — Tok string parses */
#include <math.h>           /* isnan */
#include <stdint.h>         /* INT16/32/64 MIN/MAX — Tok out-of-domain bounds */
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

ray_t* q_call_n(ray_t* f, ray_t** a, int64_t k);   /* fwd; defined ~2726 */

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
 * borrowed q_value_resolve_sym contract stays untouched; review-1 decision):
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
        /* root OR nested context handle (codex round-3 P2) */
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
        t = ray_vec_new(RAY_U8, 0);           l = ray_list_append(l, t); ray_release(t);
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

    /* ---- string atom -> evaluate as q source ---- */
    if (x->type == -RAY_STR) return q_value_eval_str(x);

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
        if (head && head->type == -RAY_STR) {         /* form 3: string head */
            ray_t* hv = q_value_eval_str(head);       /* owned */
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
/* ===== q calendar home (see q_registry.h) ==================================
 * Hinnant days_from_civil (public domain, http://howardhinnant.github.io/
 * date_algorithms.html), rebased to the kdb/base date epoch: the algorithm
 * yields days since 1970-01-01, and 2000.01.01 is unix day 10957 (the same
 * constant base temporal.c uses in the inverse direction). */
int64_t q_days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                    /* [0,399] */
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   /* [0,365] */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* [0,146096] */
    return era * 146097 + doe - 719468 - 10957;  /* unix days -> 2000.01.01 epoch */
}

/* kdb literal/value domain: 0001.01.01 .. 9999.12.31 (datatypes.md), real
 * month lengths, proleptic-Gregorian leap rule. */
int q_date_valid(int64_t y, int64_t m, int64_t d) {
    static const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1) return 0;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    return d <= md[m - 1] + ((m == 2 && leap) ? 1 : 0);
}

/* Timestamp payload composition (see q_registry.h for the contract and the
 * boundary rationale — codex plan-review round 1). */
int q_ts_compose_checked(int64_t days, int64_t tod_ns, int64_t* out) {
    __int128 ns = (__int128)days * 86400000000000LL + tod_ns;
    if (ns > INT64_MAX || ns < -(__int128)INT64_MAX) return 0;
    *out = (int64_t)ns;
    return 1;
}
int64_t q_ts_compose(int64_t days, int64_t tod_ns) {
    int64_t ns;
    if (q_ts_compose_checked(days, tod_ns, &ns)) return ns;
    return ((__int128)days * 86400000000000LL + tod_ns) < 0 ? -INT64_MAX
                                                            : INT64_MAX;
}

/* ===== q cast home (see q_registry.h for the reuse contract) ===============
 * Designator resolution is separate from conversion so C callers (future
 * bool-widening / promotion work) can invoke q_cast_to(tag, x) directly. */

int8_t q_cast_designator(ray_t* t, int* is_tok) {
    *is_tok = 0;
    if (!t) return 0;
    if (t->type == -RAY_I16) {          /* kdb type number == rayfall tag */
        if (RAY_ATOM_IS_NULL(t)) return 0;
        int16_t n = t->i16;
        if (n <= 0) { *is_tok = 1; n = (int16_t)-n; }
        switch (n) {
        case RAY_BOOL: case RAY_U8:  case RAY_I16: case RAY_I32:
        case RAY_I64:  case RAY_F32: case RAY_F64: case RAY_SYM: case RAY_STR:
        RAY_TEMPORAL32_CASES: RAY_TEMPORAL64_CASES: RAY_TEMPORALF_CASES:
            return (int8_t)n;
        default: return 0;    /* guid: deferred */
        }
    }
    if (t->type == -RAY_STR && ray_str_len(t) == 1) {
        char c = ray_str_ptr(t)[0];
        if (c >= 'A' && c <= 'Z') { *is_tok = 1; c = (char)(c - 'A' + 'a'); }
        switch (c) {
        case 'b': return RAY_BOOL; case 'x': return RAY_U8;
        case 'h': return RAY_I16;  case 'i': return RAY_I32;
        case 'j': return RAY_I64;  case 'e': return RAY_F32;
        case 'f': return RAY_F64;  case 's': return RAY_SYM;
        case 'd': return RAY_DATE; case 'g': return RAY_GUID;
        case 't': return RAY_TIME; case 'p': return RAY_TIMESTAMP;
        case 'm': return RAY_MONTH;
        case 'u': return RAY_MINUTE;
        case 'v': return RAY_SECOND;
        case 'n': return RAY_TIMESPAN;
        case 'z': return RAY_DATETIME;
        case 'c': return RAY_STR;   /* char cast: "c"$x reinterprets as chars */
        default:  return 0;       /* "*" identity: deferred */
        }
    }
    if (t->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(t->i64);
        if (!s) return 0;
        const char* nm = ray_str_ptr(s);
        size_t l = ray_str_len(s);
        int8_t r = 0;
        if      (l == 0)                              { *is_tok = 1; r = RAY_SYM; }
        else if (l == 4 && !memcmp(nm, "char",    4)) r = RAY_STR;
        else if (l == 4 && !memcmp(nm, "long",    4)) r = RAY_I64;
        else if (l == 5 && !memcmp(nm, "float",   5)) r = RAY_F64;
        else if (l == 3 && !memcmp(nm, "int",     3)) r = RAY_I32;
        else if (l == 5 && !memcmp(nm, "short",   5)) r = RAY_I16;
        else if (l == 7 && !memcmp(nm, "boolean", 7)) r = RAY_BOOL;
        else if (l == 4 && !memcmp(nm, "byte",    4)) r = RAY_U8;
        else if (l == 4 && !memcmp(nm, "real",    4)) r = RAY_F32;
        else if (l == 6 && !memcmp(nm, "symbol",  6)) r = RAY_SYM;
        else if (l == 4 && !memcmp(nm, "date",    4)) r = RAY_DATE;
        else if (l == 5 && !memcmp(nm, "month",   5)) r = RAY_MONTH;
        else if (l == 6 && !memcmp(nm, "minute",  6)) r = RAY_MINUTE;
        else if (l == 6 && !memcmp(nm, "second",  6)) r = RAY_SECOND;
        else if (l == 8 && !memcmp(nm, "timespan",8)) r = RAY_TIMESPAN;
        else if (l == 4 && !memcmp(nm, "time",    4)) r = RAY_TIME;
        else if (l == 9 && !memcmp(nm, "timestamp", 9)) r = RAY_TIMESTAMP;
        else if (l == 8 && !memcmp(nm, "datetime", 8)) r = RAY_DATETIME;
        ray_release(s);
        return r;
    }
    return 0;
}

/* RAY vector type -> q type-name (ref/key.md "type of a vector"; the exact
 * REVERSE of the cast-designator name map above — keep the two in sync). */
const char* q_type_qname(int8_t t) {
    switch (t) {
    case RAY_BOOL:      return "boolean";
    case RAY_U8:        return "byte";
    case RAY_I16:       return "short";
    case RAY_I32:       return "int";
    case RAY_I64:       return "long";
    case RAY_F32:       return "real";
    case RAY_F64:       return "float";
    case RAY_SYM:       return "symbol";
    case RAY_DATE:      return "date";
    case RAY_MONTH:     return "month";
    case RAY_MINUTE:    return "minute";
    case RAY_SECOND:    return "second";
    case RAY_TIME:      return "time";
    case RAY_TIMESPAN:  return "timespan";
    case RAY_TIMESTAMP: return "timestamp";
    case RAY_DATETIME:  return "datetime";
    default:            return NULL;
    }
}

/* tag -> rayfall `as` type-sym spelling (cast delegation targets only) */
static const char* q_tag_rayname(int8_t tag) {
    switch (tag) {
    case RAY_BOOL: return "BOOL"; case RAY_U8:  return "U8";
    case RAY_I16:  return "I16";  case RAY_I32: return "I32";
    case RAY_I64:  return "I64";  case RAY_F64: return "F64";
    case RAY_DATE: return "DATE"; case RAY_TIME: return "TIME";
    case RAY_MONTH: return "MONTH";
    case RAY_MINUTE: return "MINUTE";
    case RAY_SECOND: return "SECOND";
    case RAY_TIMESPAN: return "TIMESPAN";
    case RAY_TIMESTAMP: return "TIMESTAMP";
    case RAY_DATETIME: return "DATETIME";
    default:       return NULL;
    }
}

/* kdb float->integer casts ROUND (rint = IEEE nearest/ties-even: `long$3.7
 * is 4, "j"$2.5 is 2, `int$6.6 is 7 — KX ref pins), where rayfall `as`
 * truncates — integer targets pre-round here; everything else delegates to
 * base ray_cast_fn.  RAY_LIST distributes per element and collapses. */
ray_t* q_cast_to(int8_t tag, ray_t* x) {
    /* char cast (`10h$`/`` `char$``/`"c"$`): reinterpret an integer/byte value as
     * chars, producing a native string (openq has no char-atom type distinct from
     * a 1-char string).  Handled BEFORE the generic RAY_LIST distribution so a
     * boxed integer list packs into ONE string rather than a list of 1-char
     * strings (q_collapse_list refuses to pack -RAY_STR atoms). */
    if (tag == RAY_STR) {
        if (x && x->type == -RAY_STR) { ray_retain(x); return x; }   /* identity */
        if (x && x->type == RAY_U8)                                  /* byte vec */
            return ray_str((const char*)ray_data(x), (size_t)ray_len(x));
        if (x && x->type == -RAY_U8) { char c = (char)x->u8; return ray_str(&c, 1); }
        if (x && (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16)) {
            int64_t v = (x->type == -RAY_I64) ? x->i64
                      : (x->type == -RAY_I32) ? (int64_t)x->i32 : (int64_t)x->i16;
            char c = (char)v; return ray_str(&c, 1);
        }
        if (x && (x->type == RAY_I64 || x->type == RAY_I32 || x->type == RAY_I16)) {
            int64_t n = ray_len(x);
            char* buf = (char*)malloc(n ? (size_t)n : 1);
            if (!buf) return ray_error("wsfull", "$: out of memory");
            for (int64_t i = 0; i < n; i++) {
                int64_t v = (x->type == RAY_I64) ? ((const int64_t*)ray_data(x))[i]
                          : (x->type == RAY_I32) ? (int64_t)((const int32_t*)ray_data(x))[i]
                          :                        (int64_t)((const int16_t*)ray_data(x))[i];
                buf[i] = (char)v;
            }
            ray_t* r = ray_str(buf, (size_t)n);
            free(buf);
            return r;
        }
        if (x && x->type == RAY_LIST) {          /* boxed list of int/byte -> string */
            int64_t n = ray_len(x);
            ray_t** e = (ray_t**)ray_data(x);
            char* buf = (char*)malloc(n ? (size_t)n : 1);
            if (!buf) return ray_error("wsfull", "$: out of memory");
            for (int64_t i = 0; i < n; i++) {
                ray_t* ei = e[i];
                int64_t v;
                if      (ei && ei->type == -RAY_I64) v = ei->i64;
                else if (ei && ei->type == -RAY_I32) v = ei->i32;
                else if (ei && ei->type == -RAY_I16) v = ei->i16;
                else if (ei && ei->type == -RAY_U8)  v = ei->u8;
                else if (ei && ei->type == -RAY_STR && ray_str_len(ei) == 1)
                    v = (unsigned char)ray_str_ptr(ei)[0];
                else { free(buf); return ray_error("type", "$: cannot cast list element to char"); }
                buf[i] = (char)v;
            }
            ray_t* r = ray_str(buf, (size_t)n);
            free(buf);
            return r;
        }
        return ray_error("type", "$: cannot cast to char");
    }
    if (x && x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* r = q_cast_to(tag, e[i]);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);   /* append retains */
            ray_release(r);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    int isint = (tag == RAY_I64 || tag == RAY_I32 || tag == RAY_I16);
    if (isint && x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null((int8_t)-tag);
        double r = rint(x->f64);              /* F32 atoms store f64 payload */
        if (tag == RAY_I64) return ray_i64((int64_t)r);
        if (tag == RAY_I32) return ray_i32((int32_t)r);
        return ray_i16((int16_t)r);
    }
    if (isint && x && (x->type == RAY_F64 || x->type == RAY_F32)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(tag, n);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        int is64 = (x->type == RAY_F64);
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(x))[i]
                            : (double)((const float*)ray_data(x))[i];
            int isnull = isnan(v);
            int64_t iv = isnull ? 0 : (int64_t)rint(v);
            if      (tag == RAY_I64) ((int64_t*)ray_data(out))[i] = iv;
            else if (tag == RAY_I32) ((int32_t*)ray_data(out))[i] = (int32_t)iv;
            else                     ((int16_t*)ray_data(out))[i] = (int16_t)iv;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* kdb `"x"$str` maps CHARS to bytes ("x"$"abc" -> 0x616263, ref/cast.md
     * #byte); base's U8 STR arm parses decimal text instead — pre-empt it.
     * One char = char atom -> byte ATOM; else a byte vector of the raw chars
     * (empty string -> empty byte vector). */
    if (tag == RAY_U8 && x && x->type == -RAY_STR) {
        const char* sp = ray_str_ptr(x);
        size_t sl = ray_str_len(x);
        if (sl == 1) return ray_u8((uint8_t)sp[0]);
        return ray_vec_from_raw(RAY_U8, sp, (int64_t)sl);
    }
    /* kdb integer-family casts ROUND floats (`int$6.6 -> 7, ref/cast.md);
     * byte joins that family (derived — byte float-cast is unpinned).  Float
     * null -> 0x00: byte has no null (basics/datatypes.md blank column). */
    if (tag == RAY_U8 && x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_u8(0);
        return ray_u8((uint8_t)(int64_t)rint(x->f64));  /* F32 stores f64 */
    }
    if (tag == RAY_U8 && x && (x->type == RAY_F64 || x->type == RAY_F32)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_U8, n);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        int is64 = (x->type == RAY_F64);
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(x))[i]
                            : (double)((const float*)ray_data(x))[i];
            ((uint8_t*)ray_data(out))[i] = isnan(v) ? 0 : (uint8_t)(int64_t)rint(v);
        }
        return out;
    }
    /* kdb `timestamp$date: days -> ns, SATURATING outside the timestamp year
     * range (`timestamp$1666.09.02 -> -0Wp, datatypes.md:149) — base's arm
     * multiplies unchecked (i64 overflow, UBSan, builtins.c:1616) — and
     * mapping the date sentinels to the i64 sentinels (0Nd -> 0Np,
     * +-0Wd -> +-0Wp, which the saturation clamp yields for free). */
    if (tag == RAY_TIMESTAMP && x && x->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(q_ts_compose((int64_t)x->i32, 0));
    }
    if (tag == RAY_TIMESTAMP && x && x->type == RAY_DATE) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_TIMESTAMP, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        const int32_t* d = (const int32_t*)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            int isnull = (d[i] == INT32_MIN);
            ((int64_t*)ray_data(out))[i] =
                isnull ? 0 : q_ts_compose((int64_t)d[i], 0);
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* kdb `timestamp$time keeps the TIME OF DAY (ms -> ns on day 0, derived:
     * time is ms-of-day, timestamp ns; base's same-width path relabels the
     * raw ms payload as ns — a wrong answer, caught by the designator audit).
     * Sentinels map across (0Nt -> 0Np, +-0Wt -> +-0Wp). */
    if (tag == RAY_TIMESTAMP && x && x->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_TIMESTAMP);
        if (x->i32 == INT32_MAX)  return ray_timestamp(INT64_MAX);
        if (x->i32 == -INT32_MAX) return ray_timestamp(-INT64_MAX);
        return ray_timestamp((int64_t)x->i32 * 1000000LL);
    }
    if (tag == RAY_TIMESTAMP && x && x->type == RAY_TIME) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_TIMESTAMP, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        const int32_t* d = (const int32_t*)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            int isnull = (d[i] == INT32_MIN);
            ((int64_t*)ray_data(out))[i] =
                isnull ? 0
                : (d[i] == INT32_MAX)  ? INT64_MAX
                : (d[i] == -INT32_MAX) ? -INT64_MAX
                : (int64_t)d[i] * 1000000LL;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* float -> timestamp: base truncates the float to a raw ns count, but
     * kdb's unit semantics here (rint-ns vs datetime-style fractional DAYS)
     * is unpinned in the docs corpus — error beats a wrong answer, so the
     * shape is a deferred cell (designator-audit decision, plan 2026-07-07). */
    if (tag == RAY_TIMESTAMP && x &&
        (x->type == -RAY_F64 || x->type == -RAY_F32 ||
         x->type == RAY_F64  || x->type == RAY_F32))
        return ray_error("nyi", "$: float->timestamp cast is deferred");
    if (tag == RAY_SYM) {                 /* `symbol$sym is identity; rest nyi */
        if (x && (x->type == -RAY_SYM || x->type == RAY_SYM)) {
            ray_retain(x);
            return x;
        }
        return ray_error("nyi", "$: cast to symbol is deferred (use `$ / \"S\"$ on strings)");
    }
    const char* nm = q_tag_rayname(tag);
    if (!nm)                              /* F32 cast: no base arm — deferred */
        return ray_error("nyi", "$: unsupported cast designator (deferred)");
    ray_t* ts = ray_sym(ray_sym_intern(nm, strlen(nm)));
    if (!ts || RAY_IS_ERR(ts)) return ts;
    ray_t* r = ray_cast_fn(ts, x);
    ray_release(ts);
    return r;
}

/* "D"$ date-string scan (ref/tok.md date formats).  Supported subset:
 * yyyymmdd (8 digits, the doc's [yy]yymmdd with an unambiguous 4-digit year)
 * and yyyy.mm.dd / yyyy-mm-dd / yyyy/mm/dd (the doc's separator variants;
 * "D"$"2000-12-12" is letter-pinned).  Two-digit years and MMM month names
 * are deferred.  Returns 1 and fills y/m/d on a shape match; civil validity
 * is the caller's q_date_valid check. */
static int q_date_scan(const char* p, size_t len,
                       int64_t* y, int64_t* m, int64_t* d) {
    if (len == 8) {
        for (int i = 0; i < 8; i++)
            if (p[i] < '0' || p[i] > '9') return 0;
        *y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        *m = (p[4]-'0')*10 + (p[5]-'0');
        *d = (p[6]-'0')*10 + (p[7]-'0');
        return 1;
    }
    if (len == 10 && (p[4] == '.' || p[4] == '-' || p[4] == '/') && p[7] == p[4]) {
        for (int i = 0; i < 10; i++) {
            if (i == 4 || i == 7) continue;
            if (p[i] < '0' || p[i] > '9') return 0;
        }
        *y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        *m = (p[5]-'0')*10 + (p[6]-'0');
        *d = (p[8]-'0')*10 + (p[9]-'0');
        return 1;
    }
    return 0;
}

static int q_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a CANONICAL 36-char UUID (8-4-4-4-12, hyphens at 8/13/18/23, hex
 * elsewhere, case-insensitive) into out[16].  Returns 1 on success, 0 on any
 * shape/char mismatch.  kdb "G"$ ALSO accepts IPv4/IPv6 address forms
 * (test/q/cast/tok.qcmd, skiplisted) — DEFERRED here (see PLAN.md); those
 * inputs fail this shape check and Tok returns 0Ng. */
static int q_parse_uuid(const char* p, size_t len, uint8_t out[16]) {
    if (len != 36) return 0;
    int bi = 0;
    for (size_t i = 0; i < 36; ) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (p[i] != '-') return 0;
            i++;
            continue;
        }
        int h = q_hexval(p[i]);
        int l = q_hexval(p[i + 1]);
        if (h < 0 || l < 0) return 0;
        out[bi++] = (uint8_t)((h << 4) | l);
        i += 2;
    }
    return bi == 16;
}

/* "T"$ time-string scan (ref/tok.md).  Two forms, both -> i32 ms of day:
 *   - PACKED digits HHMMSSmmm (doc-pinned): "T"$"123456789" -> 12:34:56.789,
 *     "T"$"123456123987654" -> 12:34:56.123 (>=6 digits: HH MM SS then up to 3
 *     fractional; extra fractional digits ignored).
 *   - COLON HH:MM:SS[.f…] (derived — the natural literal spelling): the `.`
 *     fractional is optional; only its first 3 digits (millis) are used.
 * mm/ss must be < 60, else out-of-domain.  Returns 1 and fills *ms on success,
 * 0 on any shape/range mismatch (caller -> typed null 0Nt). */
static int q_all_digits(const char* p, size_t len) {
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++)
        if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}
static int q_time_scan(const char* p, size_t len, int32_t* ms) {
    int64_t h, mi, s, frac = 0;
    int has_colon = 0;
    for (size_t i = 0; i < len; i++) if (p[i] == ':') { has_colon = 1; break; }
    /* colon form: H[H]:MM:SS[.f…] */
    if (has_colon) {
        size_t i = 0;
        int64_t hv = 0;
        while (i < len && p[i] >= '0' && p[i] <= '9') { hv = hv * 10 + (p[i] - '0'); i++; }
        if (i == 0 || i > 2 || i >= len || p[i] != ':') return 0;
        i++;
        if (i + 2 > len || !q_all_digits(p + i, 2) || i + 2 >= len || p[i + 2] != ':')
            return 0;
        mi = (p[i] - '0') * 10 + (p[i + 1] - '0');
        i += 3;
        if (i + 2 > len || !q_all_digits(p + i, 2)) return 0;
        s = (p[i] - '0') * 10 + (p[i + 1] - '0');
        i += 2;
        if (i < len) {                        /* optional .fractional */
            if (p[i] != '.') return 0;
            i++;
            int64_t scale = 100;
            size_t seen = 0;
            while (i < len && p[i] >= '0' && p[i] <= '9') {
                if (seen < 3) { frac += (p[i] - '0') * scale; scale /= 10; seen++; }
                i++;
            }
            if (i != len) return 0;           /* trailing junk */
        }
        h = hv;
        if (mi >= 60 || s >= 60) return 0;
        *ms = (int32_t)(h * 3600000 + mi * 60000 + s * 1000 + frac);
        return 1;
    }
    /* packed HHMMSSmmm: >=6 digits, first 6 = HHMMSS, next up to 3 = millis */
    if (len >= 6 && q_all_digits(p, len)) {
        h  = (p[0] - '0') * 10 + (p[1] - '0');
        mi = (p[2] - '0') * 10 + (p[3] - '0');
        s  = (p[4] - '0') * 10 + (p[5] - '0');
        int64_t scale = 100;
        for (size_t i = 6; i < len && i < 9; i++) { frac += (p[i] - '0') * scale; scale /= 10; }
        if (mi >= 60 || s >= 60) return 0;
        *ms = (int32_t)(h * 3600000 + mi * 60000 + s * 1000 + frac);
        return 1;
    }
    return 0;
}

/* Clock scan for the duration Toks "U"$/"V"$/"N"$ -> ns.  Two forms
 * (the q_time_scan scheme generalised to ns):
 *   - PACKED digits HHMMSS + up to 9 fractional digits right-padded
 *     (doc-pinned for "N": tok.md:200 "N"$"123456123987654" ->
 *     0D12:34:56.123987654); >=4 digits HHMM accepted with SS=0 (derived).
 *   - COLON H[H]:MM[:SS[.f{1..9}]] (derived — the literal spellings).
 * mm/ss must be < 60.  Returns 1 and fills *ns, else 0 (caller -> null). */
static int q_clock_scan_ns(const char* p, size_t len, int64_t* ns) {
    int64_t h = 0, mi = 0, s = 0, frac = 0;
    int has_colon = 0;
    for (size_t i = 0; i < len; i++) if (p[i] == ':') { has_colon = 1; break; }
    if (has_colon) {
        size_t i = 0;
        while (i < len && p[i] >= '0' && p[i] <= '9') { h = h * 10 + (p[i] - '0'); i++; }
        if (i == 0 || i > 2 || i >= len || p[i] != ':') return 0;
        i++;
        if (i + 2 > len || !q_all_digits(p + i, 2)) return 0;
        mi = (p[i] - '0') * 10 + (p[i + 1] - '0');
        i += 2;
        if (i < len) {                        /* optional :SS[.f…] */
            if (p[i] != ':') return 0;
            i++;
            if (i + 2 > len || !q_all_digits(p + i, 2)) return 0;
            s = (p[i] - '0') * 10 + (p[i + 1] - '0');
            i += 2;
            if (i < len) {
                if (p[i] != '.' || i + 1 == len) return 0;
                i++;
                size_t fd = len - i;
                if (fd > 9 || !q_all_digits(p + i, fd)) return 0;
                for (size_t k = 0; k < fd; k++) frac = frac * 10 + (p[i + k] - '0');
                for (size_t k = fd; k < 9; k++) frac *= 10;
            }
        }
    } else if (len >= 4 && q_all_digits(p, len)) {
        h  = (p[0] - '0') * 10 + (p[1] - '0');
        mi = (p[2] - '0') * 10 + (p[3] - '0');
        if (len >= 6) {
            s = (p[4] - '0') * 10 + (p[5] - '0');
            size_t fd = len - 6;
            if (fd > 9) return 0;
            for (size_t k = 0; k < fd; k++) frac = frac * 10 + (p[6 + k] - '0');
            for (size_t k = fd; k < 9; k++) frac *= 10;
        } else if (len != 4) return 0;
    } else return 0;
    if (mi >= 60 || s >= 60) return 0;
    *ns = (h * 3600 + mi * 60 + s) * 1000000000LL + frac;
    return 1;
}

/* tod scan for "P"$: HH:MM:SS[.f{1..9}] -> ns of day (colon form only; the
 * packed date form is split off by the caller).  Returns 1/0. */
static int q_tod_scan_ns(const char* p, size_t len, int64_t* ns) {
    if (len < 8 || !q_all_digits(p, 2) || p[2] != ':' ||
        !q_all_digits(p + 3, 2) || p[5] != ':' || !q_all_digits(p + 6, 2))
        return 0;
    int64_t h  = (p[0]-'0')*10 + (p[1]-'0');
    int64_t mi = (p[3]-'0')*10 + (p[4]-'0');
    int64_t s  = (p[6]-'0')*10 + (p[7]-'0');
    if (mi >= 60 || s >= 60) return 0;
    int64_t frac = 0;
    if (len > 8) {
        if (p[8] != '.' || len == 9) return 0;
        size_t fd = len - 9;
        if (fd > 9) return 0;
        for (size_t k = 0; k < fd; k++) {
            if (p[9 + k] < '0' || p[9 + k] > '9') return 0;
            frac = frac * 10 + (p[9 + k] - '0');
        }
        for (size_t k = fd; k < 9; k++) frac *= 10;
    }
    *ns = (h * 3600 + mi * 60 + s) * 1000000000LL + frac;
    return 1;
}

/* "P"$ timestamp-string scan (ref/tok.md Â§Timestamps).  Subset:
 *   - Unix seconds, 9..11 digits [+ . fraction] (doc-pinned:
 *     "P"$"10129708800" -> 2290.12.31D00:00:00.000000000,
 *     "P"$"10129708800.123456789" -> ...D00:00:00.123456789);
 *   - date part (q_date_scan separator forms or packed yyyymmdd) + one of
 *     "DT- " + colon tod (pins: "PZ"$\:"20191122-11:11:11.123");
 *   - date-only -> midnight (derived).
 * MMM months / 2-digit years / timezone forms deferred.  Returns 1 + payload
 * ns on success; 0 -> caller yields 0Np (tok.md out-of-domain contract —
 * CHECKED compose, never the cast path's saturating +-0Wp). */
static int q_ts_scan(const char* p, size_t len, int64_t* out) {
    /* unix-seconds: 9..11 digits, optionally . + 1..9 fraction digits */
    size_t dot = len;
    for (size_t i = 0; i < len; i++) if (p[i] == '.') { dot = i; break; }
    if (dot >= 9 && dot <= 11 && q_all_digits(p, dot) &&
        (dot == len || (len > dot + 1 && len <= dot + 10 &&
                        q_all_digits(p + dot + 1, len - dot - 1)))) {
        int64_t secs = 0;
        for (size_t i = 0; i < dot; i++) secs = secs * 10 + (p[i] - '0');
        secs -= 946684800LL;                  /* unix epoch -> 2000.01.01 */
        int64_t ns;
        if (__builtin_mul_overflow(secs, 1000000000LL, &ns)) return 0;
        int64_t frac = 0;
        size_t fd = (dot == len) ? 0 : len - dot - 1;
        for (size_t k = 0; k < fd; k++) frac = frac * 10 + (p[dot + 1 + k] - '0');
        for (size_t k = fd; k < 9; k++) frac *= 10;
        if (__builtin_add_overflow(ns, frac, &ns)) return 0;
        *out = ns;
        return 1;
    }
    /* date [sep tod] */
    size_t dl = 0;
    if (len >= 10 && (p[4] == '.' || p[4] == '-' || p[4] == '/')) dl = 10;
    else if (len >= 8 && q_all_digits(p, 8)) dl = 8;
    if (dl == 0 || len < dl) return 0;
    int64_t y, mo, d;
    if (!q_date_scan(p, dl, &y, &mo, &d) || !q_date_valid(y, mo, d)) return 0;
    int64_t tod = 0;
    if (len > dl) {
        char sep = p[dl];
        if (!(sep == 'D' || sep == 'T' || sep == '-' || sep == ' ')) return 0;
        if (!q_tod_scan_ns(p + dl + 1, len - dl - 1, &tod)) return 0;
    }
    return q_ts_compose_checked(q_days_from_civil(y, mo, d), tod, out);
}

/* kdb Tok (ref/tok.md): parse a string as a value of the tag type.  Leading/
 * trailing blanks are trimmed; unparseable or out-of-range -> typed null.
 * Implicit recursion stops at STRINGS, not atoms: lists / string vectors
 * distribute. */
ray_t* q_tok_to(int8_t tag, ray_t* x) {
    if (x && (x->type == RAY_LIST || x->type == RAY_STR)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* xi;
            if (x->type == RAY_LIST) {
                xi = ((ray_t**)ray_data(x))[i];
                ray_retain(xi);
            } else {
                size_t sl = 0;
                const char* sp = ray_str_vec_get(x, i, &sl);
                xi = ray_str(sp ? sp : "", sp ? sl : 0);
            }
            ray_t* r = q_tok_to(tag, xi);
            ray_release(xi);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    if (!x || x->type != -RAY_STR)
        return ray_error("type", "$: Tok right operand must be a string");
    const char* p = ray_str_ptr(x);
    size_t len = p ? ray_str_len(x) : 0;
    while (len && *p == ' ') { p++; len--; }            /* trim outer blanks */
    while (len && p[len - 1] == ' ') len--;
    switch (tag) {
    case RAY_SYM:
        return ray_sym(ray_sym_intern(len ? p : "", len));
    case RAY_BOOL:   /* truthy set pinned by ref/tok.md: "txyTXY1" */
        return ray_bool(len == 1 && strchr("1TtXxYy", p[0]) != NULL);
    case RAY_F64: case RAY_F32: {
        double v = 0;
        size_t used = len ? ray_parse_f64(p, len, &v) : 0;
        if (used != len || len == 0) return ray_typed_null((int8_t)-tag);
        return tag == RAY_F64 ? ray_f64(v) : ray_f32((float)v);
    }
    case RAY_I64: case RAY_I32: case RAY_I16: {
        int64_t v = 0;
        size_t used = len ? ray_parse_i64(p, len, &v) : 0;
        if (used != len || len == 0) return ray_typed_null((int8_t)-tag);
        /* Out-of-domain -> typed null (tok.md).  The bounds are ±INT*_MAX,
         * NOT INT*_MIN: the exact minimum IS the null sentinel (0N/0Ni/0Nh)
         * and must never round-trip as an accepted value. */
        if (tag == RAY_I64)
            return (v == INT64_MIN)
                 ? ray_typed_null(-RAY_I64) : ray_i64(v);
        if (tag == RAY_I32)
            return (v > INT32_MAX || v < -INT32_MAX)
                 ? ray_typed_null(-RAY_I32) : ray_i32((int32_t)v);
        return (v > INT16_MAX || v < -INT16_MAX)
             ? ray_typed_null(-RAY_I16) : ray_i16((int16_t)v);
    }
    case RAY_DATE: {
        /* Unparseable / invalid civil date / out-of-domain -> 0Nd, never an
         * error (tok.md pins "D"$"2147483648" -> 0Nd). */
        int64_t y, mo, d;
        if (!q_date_scan(p, len, &y, &mo, &d) || !q_date_valid(y, mo, d))
            return ray_typed_null(-RAY_DATE);
        return ray_date(q_days_from_civil(y, mo, d));
    }
    case RAY_MONTH: {
        /* "M"$str -> month (ref/tok.md designator table: month | -13 M).
         * Subset: "yyyy.mm" / "yyyy-mm" / "yyyy/mm" / packed yyyymm; the
         * civil month must be 01..12 and the year in the date domain
         * [1,9999].  Unparseable / out-of-domain -> 0Nm, never an error
         * (tok contract, mirrors "D"$). */
        int64_t y = 0, mo = 0;
        int ok = 0;
        if (len == 7 && q_all_digits(p, 4) &&
            (p[4] == '.' || p[4] == '-' || p[4] == '/') &&
            q_all_digits(p + 5, 2)) {
            y  = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
            mo = (p[5]-'0')*10 + (p[6]-'0');
            ok = 1;
        } else if (len == 6 && q_all_digits(p, 6)) {   /* packed yyyymm */
            y  = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
            mo = (p[4]-'0')*10 + (p[5]-'0');
            ok = 1;
        }
        if (!ok || mo < 1 || mo > 12 || y < 1 || y > 9999)
            return ray_typed_null(-RAY_MONTH);
        return ray_month((y - 2000) * 12 + (mo - 1));
    }
    case RAY_TIME: {
        /* "T"$str -> time (ref/tok.md).  Unparseable / out-of-domain -> 0Nt,
         * never an error (base ray_cast_fn errors on a bad string). */
        int32_t ms;
        if (!q_time_scan(p, len, &ms))
            return ray_typed_null(-RAY_TIME);
        return ray_time(ms);
    }
    case RAY_TIMESTAMP: {
        /* "P"$str -> timestamp (ref/tok.md Â§Timestamps).  Unparseable /
         * out-of-range -> 0Np, never an error (tok contract). */
        int64_t ns;
        if (!q_ts_scan(p, len, &ns))
            return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(ns);
    }
    case RAY_DATETIME: {
        /* "Z"$str -> datetime.  tok.md:222-227 pins "PZ"$\: over ONE input
         * ("20191122-11:11:11.123" -> 2019.11.22T11:11:11.123): Z shares P's
         * accepted shapes at ms display precision, so reuse q_ts_scan (the
         * single P parser) and convert ns -> fractional days.  Unparseable /
         * invalid -> 0Nz, never an error (tok contract). */
        int64_t ns;
        if (!q_ts_scan(p, len, &ns))
            return ray_typed_null(-RAY_DATETIME);
        return ray_datetime((double)ns / 86400000000000.0);
    }
    case RAY_U8: {
        /* "X"$ reads the string as HEX ("X"$"42" -> 0x42, ref/tok.md).
         * Unparseable or > 0xff -> 0x00 (derived): tok.md pins out-of-
         * domain -> typed null, and byte HAS no null (basics/datatypes.md),
         * so its zero value stands in. */
        uint64_t v = 0;
        size_t i = 0;
        for (; i < len; i++) {
            char c = p[i];
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) break;
            v = (v << 4) | (uint64_t)d;
            if (v > 0xff) break;
        }
        if (len == 0 || i != len || v > 0xff) return ray_u8(0);
        return ray_u8((uint8_t)v);
    }
    case RAY_GUID: {
        /* "G"$str -> guid (basics/datatypes.md §Guid).  Tok contract:
         * unparseable / wrong-shape -> typed null 0Ng, never an error (base
         * ray_cast_fn "GUID" ERRORS on bad input, so parse here).  Canonical
         * 36-char UUID only; IPv4/IPv6 forms deferred (see q_parse_uuid). */
        uint8_t bytes[16];
        if (!q_parse_uuid(p, len, bytes)) return ray_typed_null(-RAY_GUID);
        return ray_guid(bytes);
    }
    case RAY_MINUTE: {
        /* "U"$str -> minute, FLOOR to the minute we are in (ref/tok.md:61
         * "U"$"12:13:14" -> 12:13; cast.md:168-170 truncation rule). */
        int64_t ns;
        if (!q_clock_scan_ns(p, len, &ns))
            return ray_typed_null(-RAY_MINUTE);
        return ray_minute(ns / 60000000000LL);
    }
    case RAY_SECOND: {
        /* "V"$str -> second, floor (derived — mirrors "U"$). */
        int64_t ns;
        if (!q_clock_scan_ns(p, len, &ns))
            return ray_typed_null(-RAY_SECOND);
        return ray_second(ns / 1000000000LL);
    }
    case RAY_TIMESPAN: {
        /* "N"$str -> timespan (tok.md:200-201: the digit run is
         * HHMMSS + up to 9 fractional digits right-padded, NOT a raw ns
         * count).  dD… string forms deferred. */
        int64_t ns;
        if (!q_clock_scan_ns(p, len, &ns))
            return ray_typed_null(-RAY_TIMESPAN);
        return ray_timespan(ns);
    }
    default:
        return ray_error("nyi", "$: char Tok is deferred");
    }
}

/* q `w$s` PAD (ref/pad.md): a LONG width w left-justifies the string s in a
 * field of |w| spaces (w<0 right-justifies); longer strings truncate to |w|.
 * Atomic through the container types (a LIST of strings pads each; DICT over
 * values; TABLE over columns).  Non-string leaves are a 'type error. */
static ray_t* q_pad(int64_t w, ray_t* x) {
    if (!x) return ray_error("type", "$: pad nil");
    if (x->type == -RAY_STR) {
        int64_t width = w < 0 ? -w : w;
        int right = w < 0;                 /* w<0 -> right-justify */
        const char* p = ray_str_ptr(x);
        int64_t n = (int64_t)ray_str_len(x);
        int64_t copy = n < width ? n : width;
        char stack[256];
        char* b = (width < (int64_t)sizeof stack) ? stack : malloc((size_t)width + 1);
        if (!b) return ray_error("wsfull", "$: out of memory");
        memset(b, ' ', (size_t)width);
        if (right) memcpy(b + (width - copy), p, (size_t)copy);   /* text at right */
        else       memcpy(b, p, (size_t)copy);                    /* text at left  */
        ray_t* r = ray_str(b, (size_t)width);
        if (b != stack) free(b);
        return r;
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* e = ray_at_fn(x, ia);
            ray_release(ia);
            if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
            ray_t* r = q_pad(w, e);
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);
        ray_t* v = ray_dict_vals(x);
        if (!k || !v) return ray_error("type", "$: bad dict");
        ray_t* nv = q_pad(w, v);
        if (!nv || RAY_IS_ERR(nv)) return nv;
        ray_retain(k);
        return ray_dict_new(k, nv);
    }
    if (x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* out = ray_table_new(nc);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(x, c);
            ray_t* ncol = q_pad(w, col);
            if (!ncol || RAY_IS_ERR(ncol)) { ray_release(out); return ncol; }
            out = ray_table_add_col(out, ray_table_col_name(x, c), ncol);
            ray_release(ncol);
            if (!out || RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_STR) {            /* string vector -> pad each element */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            size_t sn; const char* p = ray_str_vec_get(x, i, &sn);
            ray_t* s = ray_str(p ? p : "", p ? sn : 0);
            ray_t* r = q_pad(w, s);
            ray_release(s);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return ray_error("type", "$: pad expects a string");
}

/* q `t$x` — Cast/Tok dispatcher.  Multi-designator LHS ("fiij", `int`float,
 * 5 6h, (`int;"i";6h)) zips elementwise over x (ref/cast.md pins
 * (`int;"i";6h)$10 -> 10 10 10i: an ATOM rhs is broadcast).  Single
 * designators resolve via q_cast_designator and dispatch to the shared
 * q_cast_to / q_tok_to (the ONE conversion home — see q_registry.h).  A LONG
 * width LHS (`9$"foo"`) is PAD, not a cast — intercepted before designators. */
ray_t* q_cast_wrap(ray_t* t, ray_t* x) {
    if (t && t->type == -RAY_I64) return q_pad(t->i64, x);
    int multi = t && ((t->type == -RAY_STR && ray_str_len(t) > 1) ||
                      t->type == RAY_SYM || t->type == RAY_I16 ||
                      t->type == RAY_LIST);
    if (multi) {
        int64_t n = (t->type == -RAY_STR) ? (int64_t)ray_str_len(t) : ray_len(t);
        int x_is_list = x && (ray_is_vec(x) || x->type == RAY_LIST);
        if (x_is_list && ray_len(x) != n)
            return ray_error("length", "$: designator/operand length mismatch");
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ti;
            if (t->type == -RAY_STR) ti = ray_str(ray_str_ptr(t) + i, 1);
            else {
                ray_t* idx = ray_i64(i);
                ti = ray_at_fn(t, idx);         /* sym/short vec, list */
                ray_release(idx);
            }
            if (!ti || RAY_IS_ERR(ti)) { ray_release(out); return ti; }
            ray_t* xi;
            if (x_is_list) {
                ray_t* idx = ray_i64(i);
                xi = ray_at_fn(x, idx);
                ray_release(idx);
            } else { xi = x; ray_retain(xi); }  /* atom rhs broadcasts */
            if (!xi || RAY_IS_ERR(xi)) { ray_release(ti); ray_release(out); return xi; }
            ray_t* r = q_cast_wrap(ti, xi);
            ray_release(ti);
            ray_release(xi);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    int is_tok = 0;
    int8_t tag = q_cast_designator(t, &is_tok);
    if (!tag)
        return ray_error("nyi", "$: unsupported cast designator (deferred)");
    /* `10h$`/`` `char$``/`"c"$` all land here with is_tok=0 and reinterpret via
     * q_cast_to; only the UPPERCASE char token `"C"$` carries is_tok=1 and stays
     * a deferred char-Tok — q_tok_to's default errors 'nyi (pinned by the
     * cast_tok_deferred unit test), so no RAY_STR special-case is needed here. */
    return is_tok ? q_tok_to(tag, x) : q_cast_to(tag, x);
}
