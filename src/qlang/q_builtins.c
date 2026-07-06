/* q_builtins — register q's own builtins into the rayforce env.
 *
 * q reuses rayfall's eval/value/show as-is (they operate on ray_t), but needs
 * its own q-syntax `parse` (rayfall's `parse` reads lisp).  We bind names via
 * the public ray_fn_* / ray_env_bind API — never touching the frozen builtin
 * registration site — so `parse` overrides rayfall's lisp parse in the env. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_builtins.h"
#include "qlang/q_apply.h"    /* q_apply_noun — the noun-head dispatcher */
#include "qlang/q_parse.h"
#include "qlang/q_registry.h" /* q_registry_init */
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

/* (string x) — q cast-to-string.  A sym renders bare (`ibm -> "ibm"); other
 * values reuse rayfall's formatter (string 42 -> "42"). */
static ray_t* q_string_fn(ray_t* x) {
    if (!x) return ray_error("type", "string: nil");
    if (x->type == -RAY_SYM) return ray_sym_str(x->i64);
    if (x->type == -RAY_STR) { ray_retain(x); return x; }
    return ray_fmt(x, 0);
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
    bind_value(".Q.an",  ray_str("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", 63));
    /* `::` — the generic-null VALUE.  Elided call args parse as unquoted `::`
     * name-refs (`f[]` is (`f;::), cases.tsv:43); binding the value makes them
     * evaluate to RAY_NULL_OBJ instead of a 'name error — which is exactly
     * what lambda application detects as projection holes, and gives a typed
     * `::` its kdb value.  The funsql special forms are unaffected (their `::`
     * markers are never evaluated). */
    ray_retain(RAY_NULL_OBJ);
    bind_value("::", RAY_NULL_OBJ);
    /* Build q's verb table over the now-populated g_env (ray_lang_init has run).
     * The registry is the authoritative, immutable verb source; it snapshots
     * builtin values and must be torn down via q_runtime_destroy before the
     * runtime.  Fail fast: a missing audited builtin is a bug. */
    assert(q_registry_init() == RAY_OK);
    bind_unary(".Q.id", q_id_fn);
    bind_value(".Q.res", q_name_reserved_words());
}
