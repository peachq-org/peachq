/* q_builtins — register q's own builtins into the rayforce env.
 *
 * q reuses rayfall's eval/value/show as-is (they operate on ray_t), but needs
 * its own q-syntax `parse` (rayfall's `parse` reads lisp).  We bind names via
 * the public ray_fn_* / ray_env_bind API — never touching the frozen builtin
 * registration site — so `parse` overrides rayfall's lisp parse in the env. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_builtins.h"
#include "qlang/q_apply.h"    /* q_apply_noun — the noun-head dispatcher */
#include "qlang/q_deriv.h"    /* carrier inspectors — fn-value introspection */
#include "qlang/q_parse.h"
#include "qlang/q_registry.h" /* q_registry_init */
#include "qlang/q_fmt.h"      /* q_console_show — show's display sink */
#include "lang/env.h"       /* ray_fn_unary, ray_env_bind */
#include "lang/eval.h"      /* RAY_FN_NONE */
#include "lang/format.h"    /* ray_fmt — q string cast */
#include "table/sym.h"      /* ray_sym_vec_cell */
#include <rayforce.h>
#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* (parse str) — parse q SOURCE into a ray_t AST (overrides rayfall's lisp
 * parse).  q_parse needs a NUL-terminated C string; a RAY_STR atom's bytes are
 * not guaranteed terminated, so copy through a bounded scratch buffer. */
static ray_t* q_parse_builtin_fn(ray_t* x) {
    if (!x || x->type != -RAY_STR) return ray_error("type", "parse expects a string");
    const char* sp = ray_str_ptr(x);
    size_t sl = ray_str_len(x);
    if (!sp) return ray_error("domain", "parse: bad source string");
    char* src = malloc(sl + 1);
    if (!src) return ray_error("wsfull", "parse: out of memory");
    memcpy(src, sp, sl);
    src[sl] = '\0';
    ray_t* ast = q_parse(src);
    free(src);
    return ast ? ast : ray_error("parse", NULL);
}

/* (string x) — q cast-to-string.  ATOM: a sym renders bare (`ibm -> "ibm"),
 * a string passes through, other atoms reuse rayfall's formatter (string 42
 * -> "42").  VECTOR / LIST: q maps string over each item, yielding a LIST of
 * strings (`string 192 168 1 23` -> ("192";"168";"1";"23")) — the base
 * formatter would instead render the whole vector as one bracketed string. */
static ray_t* q_string_fn(ray_t* x) {
    if (!x) return ray_error("type", "string: nil");
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);        /* borrowed */
        if (!s) return ray_error("type", "string: bad symbol");
        ray_retain(s);
        return s;
    }
    if (x->type == -RAY_STR) { ray_retain(x); return x; }
    /* Only a simple VECTOR or a boxed LIST maps element-wise; atoms and
     * whole-value containers (tables, dicts, and anything else) keep the base
     * formatter — indexing a table by 0..ncols would fabricate junk rows. */
    if (!ray_is_vec(x) && x->type != RAY_LIST) return ray_fmt(x, 0);
    /* vector or boxed list: per-element string */
    int64_t n = ray_len(x);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(x, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
        ray_t* s = q_string_fn(e);
        ray_release(e);
        if (!s || RAY_IS_ERR(s)) { ray_release(out); return s; }
        out = ray_list_append(out, s);
        ray_release(s);
        if (RAY_IS_ERR(out)) return out;
    }
    return out;
}

/* upper / lower — whole-string per-char ASCII transforms (spec piece 2's
 * in-scope string verbs; char-VECTOR semantics wait on the string model). */
static ray_t* q_case_fn(ray_t* x, int up) {
    if (!x || x->type != -RAY_STR)
        return ray_error("type", "%s: expects a string", up ? "upper" : "lower");
    const char* p = ray_str_ptr(x);
    size_t n = ray_str_len(x);
    char stack[256];
    char* b = (n < sizeof stack) ? stack : malloc(n + 1);
    if (!b) return ray_error("wsfull", "%s: out of memory", up ? "upper" : "lower");
    for (size_t i = 0; i < n; i++)
        b[i] = (char)(up ? toupper((unsigned char)p[i]) : tolower((unsigned char)p[i]));
    ray_t* r = ray_str(b, n);
    if (b != stack) free(b);
    return r;
}
static ray_t* q_upper_fn(ray_t* x) { return q_case_fn(x, 1); }
static ray_t* q_lower_fn(ray_t* x) { return q_case_fn(x, 0); }

/* (show x) — print x's q console display as a SIDE EFFECT (buffered in the q
 * console sink; the host drains it), then return generic null.  Overrides
 * rayfall's `show`, which uses the rayfall formatter (`[1 2 3]`) rather than
 * q_fmt.  qdoc/repl print nothing for the null result, so the row shows only
 * the buffered display. */
static ray_t* q_show_fn(ray_t* x) {
    q_console_show(x);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

static ray_t* q_id_table(ray_t* x) {
    int64_t nc = ray_table_ncols(x);
    ray_t* out = ray_table_new(nc);
    int64_t stack[64];
    int64_t* used = (nc <= 64) ? stack : (int64_t*)malloc((size_t)nc * sizeof(int64_t));
    if (!used) { ray_release(out); return ray_error("wsfull", ".Q.id: out of memory"); }
    for (int64_t c = 0; c < nc; c++) {
        int64_t nm = q_name_sanitize(ray_table_col_name(x, c));
        nm = q_name_dedup(nm, used, c, 1);
        used[c] = nm;
        out = ray_table_add_col(out, nm, ray_table_get_col_idx(x, c));
        if (!out || RAY_IS_ERR(out)) break;
    }
    if (used != stack) free(used);
    return out;
}

static ray_t* q_id_dict(ray_t* x) {
    ray_t* k = ray_dict_keys(x);
    ray_t* v = ray_dict_vals(x);
    if (!k || k->type != RAY_SYM)
        return ray_error("type", ".Q.id: dictionary keys must be symbols");
    int64_t n = ray_len(k);
    ray_t* nk = ray_sym_vec_new(RAY_SYM_W64, n);
    int64_t stack[64];
    int64_t* used = (n <= 64) ? stack : (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!used) { ray_release(nk); return ray_error("wsfull", ".Q.id: out of memory"); }
    for (int64_t i = 0; i < n; i++) {
        ray_t* ks = ray_sym_vec_cell(k, i);
        int64_t id = ray_sym_intern_runtime(ray_str_ptr(ks), ray_str_len(ks));
        id = q_name_sanitize(id);
        id = q_name_dedup(id, used, i, 1);
        used[i] = id;
        nk = ray_vec_append(nk, &id);
        if (!nk || RAY_IS_ERR(nk)) break;
    }
    if (used != stack) free(used);
    if (!nk || RAY_IS_ERR(nk)) return nk ? nk : ray_error("oom", NULL);
    ray_retain(v);
    return ray_dict_new(nk, v);   /* consumes nk, retained v */
}

static ray_t* q_id_fn(ray_t* x) {
    if (!x) return ray_error("type", ".Q.id: nil");
    if (x->type == -RAY_SYM) return ray_sym(q_name_sanitize(x->i64));
    if (x->type == RAY_TABLE) return q_id_table(x);
    if (x->type == RAY_DICT)  return q_id_dict(x);
    return ray_error("type", ".Q.id: expects a symbol, table, or dictionary");
}

/* ---- function-value introspection (stage 2): type / count / value ------- */

static ray_unary_fn g_base_type  = NULL;
static ray_unary_fn g_base_count = NULL;

static int q_is_fn_value(ray_t* x) {
    if (!x) return 0;
    if (q_deriv_kind_of(x) != Q_DERIV_NONE) return 1;
    return x->type == RAY_LAMBDA || x->type == RAY_UNARY ||
           x->type == RAY_BINARY || x->type == RAY_VARY;
}

/* q `type` on FUNCTION values only — 100h lambda, 104h projection, 102h
 * operator (ref/datatypes.md).  Everything else stays on the base verb; the
 * full q type map is cast/type.qcmd territory. */
static ray_t* q_type_fn(ray_t* x) {
    if (x) {
        switch (q_deriv_kind_of(x)) {
        case Q_DERIV_LAMBDA: return ray_i16(100);
        case Q_DERIV_PROJ:   return ray_i16(104);
        case Q_DERIV_MONAD:  return ray_i16(102);
        default: break;
        }
        if (x->type == RAY_LAMBDA) return ray_i16(100);
        if (x->type == RAY_UNARY || x->type == RAY_BINARY || x->type == RAY_VARY)
            return ray_i16(102);
    }
    return g_base_type ? g_base_type(x)
                       : ray_error("type", "type: base verb missing");
}

/* q `count` of any function value is 1 (kdb: functions are atoms) — the
 * carrier is a RAY_LIST, so the base count would leak its slot count. */
static ray_t* q_count_fn(ray_t* x) {
    if (q_is_fn_value(x)) return ray_i64(1);
    return g_base_count ? g_base_count(x)
                        : ray_error("type", "count: base verb missing");
}

/* Capture a base unary's fn pointer AND attrs — a wrapper must be bound with
 * the base flags (count is RAY_FN_AGGR | RAY_FN_LAZY_AWARE; dropping them
 * would de-aggregate count in the planner and force lazy materialization —
 * codex stage-2 finding). */
static void capture_base(const char* name, ray_unary_fn* out, uint8_t* attrs) {
    ray_t* v = ray_env_get(ray_sym_intern(name, strlen(name)));
    if (v && v->type == RAY_UNARY) {
        *out = (ray_unary_fn)(uintptr_t)v->i64;
        if (attrs) *attrs = v->attrs;
    }
}

static void bind_unary(const char* name, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(name, RAY_FN_NONE, fn);
    ray_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

static void bind_value(const char* name, ray_t* val) {
    ray_env_bind(ray_sym_intern(name, strlen(name)), val);
    ray_release(val);
}

void q_builtins_register(void) {
    /* Noun-head application (indexing, dict/table lookup, 104h carriers):
     * register the q dispatcher into eval's apply hook.  q_runtime_destroy
     * clears it before the runtime dies. */
    ray_eval_set_apply_hook(q_apply_noun);
    bind_unary("parse", q_parse_builtin_fn);
    /* q keywords with no rayfall counterpart — q-owned env bindings (same
     * mechanism as `parse`), snapshotted by the registry as QK_ENV rows. */
    bind_unary("string", q_string_fn);
    bind_unary("upper",  q_upper_fn);
    bind_unary("lower",  q_lower_fn);
    bind_unary("show",   q_show_fn);
    bind_value(".Q.an",  ray_str("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", 63));
    /* `::` — the generic-null VALUE.  Elided call args parse as unquoted `::`
     * name-refs (`f[]` is (`f;::), cases.tsv:43); binding the value makes them
     * evaluate to RAY_NULL_OBJ instead of a 'name error — which is exactly
     * what lambda application detects as projection holes, and gives a typed
     * `::` its kdb value.  The funsql special forms are unaffected (their `::`
     * markers are never evaluated). */
    ray_retain(RAY_NULL_OBJ);
    bind_value("::", RAY_NULL_OBJ);
    /* Function-value introspection wrappers.  Bound BEFORE q_registry_init so
     * the registry's QK_ENV rows (`#` monadic = count) snapshot the WRAPPED
     * values — one home for the carrier special-cases. */
    uint8_t type_attrs = RAY_FN_NONE, count_attrs = RAY_FN_NONE;
    capture_base("type",  &g_base_type,  &type_attrs);
    capture_base("count", &g_base_count, &count_attrs);
    {
        ray_t* tv = ray_fn_unary("type", type_attrs, q_type_fn);
        ray_env_bind(ray_sym_intern("type", 4), tv);
        ray_release(tv);
        ray_t* cv = ray_fn_unary("count", count_attrs, q_count_fn);
        ray_env_bind(ray_sym_intern("count", 5), cv);
        ray_release(cv);
    }
    /* NB `value` is a QK_VALUE manifest row — its lambda/string arms live in
     * the registry wrapper (q_value_wrap), the single home the parser embeds. */
    /* Build q's verb table over the now-populated g_env (ray_lang_init has run).
     * The registry is the authoritative, immutable verb source; it snapshots
     * builtin values and must be torn down via q_runtime_destroy before the
     * runtime.  Fail fast: a missing audited builtin is a bug. */
    assert(q_registry_init() == RAY_OK);
    bind_unary(".Q.id", q_id_fn);
    bind_value(".Q.res", q_name_reserved_words());
}
