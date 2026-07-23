/* q op registry — see q_registry.h for the contract.
 *
 * The roster is now DRIVEN BY THE SINGLE-SOURCE MANIFEST (src/qlang/q_ops.c
 * Q_OPS[]).  q_registry_init iterates the manifest and, for every (row,
 * valence) whose build-kind is not QK_NONE, builds the owned rayfall function
 * VALUE and records its q-surface provenance.  The lexer derives its keyword
 * classification from the SAME manifest (q_lex_is_kw_infix), so the parser's
 * verb set and eval's verb set can never drift.
 *
 * Ground truth for the roster + the deferred glyphs lives in q_ops.c.  The
 * non-obvious mechanics the wrappers depend on:
 *   - rayfall `%` is MODULO and `/` is FLOAT-DIVIDE, so q `%` renames `/`.
 *   - q `#`/`_` cannot reuse rayfall `take`/`remove` verbatim (opposite arg
 *     order / dict-key semantics), so they are wrappers with the arg-swap /
 *     range-drop baked in.  q monadic `-` is negate (aliased to `neg`).
 *
 * LOWERING METADATA: each wrapper's rayfall aux-name is set to the CANONICAL
 * rayfall verb it lowers as (== != take drop) and it carries RAY_FN_Q_LOWER
 * (RAY_FN_Q_LOWER, lang/eval.h), so the compiler + query DAG dispatch on that name
 * (q `=`/`<>` hit ray_eq/ray_ne) instead of declining every non-canonical
 * value head to the eval fallback.  Pass-through/rename values ARE the env
 * builtin object, so they name-route via the existing canonical-identity path.
 *
 * REGISTRY-INIT PRECONDITION (codex #1): value embedding requires an
 * initialised registry, and a registry builder MUST NEVER call q_parse during
 * bootstrap (it would recurse into a not-yet-ready registry).  Builders here
 * only touch ray_env_get / ray_fn_* — never the parser.  A debug re-entry
 * guard (g_building) marks the bootstrap window and is the seam 2b's
 * parser-flip enforcement extends. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_deriv.h" /* q_deriv_kind_of — carrier guard in q_charv_out */
#include "ops/ops.h"       /* ray_is_lazy — DAG guard in q_charv_out */
#include "lang/env.h"      /* ray_env_get; ray_fn_unary/binary/vary — building the fn-values */
#include "lang/eval.h"     /* RAY_FN_ATOMIC/SPECIAL_FORM/Q_LOWER — attrs stamped on built values */
#include "lang/internal.h" /* ray_error, ray_sym_str, ray_vec_set_null */
#include "table/sym.h"     /* ray_sym_intern_runtime, RAY_SYM_W64 — name interning */
#include "store/serde.h"   /* ray_serde_set_fn_hooks — fn-value serde round-trip */
#include <assert.h>
#include <stdint.h>        /* INT64_MAX */
#include <stdio.h>         /* snprintf */
#include <string.h>
#include <stdlib.h>

typedef struct {
    int64_t     sym_id;
    q_valence_t valence;
    ray_t*      value;       /* owned (rc>=1) */
    const q_op_t* row;       /* the manifest row this entry was built from */
    /* provenance */
    const char* spelling;    /* q surface name (static, from the manifest) */
    const char* lower_name;  /* canonical rayfall routing name             */
    int         is_wrapper;
} entry_t;

/* Upper bound: every manifest row can contribute at most two entries. */
static entry_t g_entries[2 * 256];  /* 2 slots per manifest row; grown 96->128 (list-verb 2026-07-06)->256 (2026-07-07: set-ops+sort+control-flow+atomic-math pushed the row count past 128) */
static int     g_count    = 0;
static bool    g_inited   = false;
static bool    g_building = false;   /* debug re-entry guard (see header note) */

/* ===== registry SPECIALS — internal (spelling-less) fn-values ==============
 * ONE plain data table drives four things: the g_specials[] slots, the init
 * build loop (its error path written once), the teardown release loop, and
 * the 22 borrowed-ref accessors below.  A new special is ONE row.  A plain
 * table + loop is deliberate (owner ruling 2026-07-22, over a token-pasting
 * X-macro): the accessors and bodies stay greppable, ctags-visible and
 * debugger-legible.  Every value also carries RAY_FN_Q_LOWER (stamped by the
 * build loop); accessors return BORROWED refs, NULL before init. */
enum {
    SPEC_scan, SPEC_over, SPEC_eachboth, SPEC_prior, SPEC_mkderiv2, SPEC_mkopproj,
    SPEC_list, SPEC_table, SPEC_keyed_table, SPEC_select, SPEC_delete, SPEC_exec,
    SPEC_compose, SPEC_funsql_select, SPEC_funsql_bang, SPEC_lambda, SPEC_ret,
    SPEC_sig, SPEC_seq, SPEC_if, SPEC_do, SPEC_while, SPEC_N
};

enum spec_kind { SK_UNARY, SK_BINARY, SK_VARY };   /* -> ray_fn_unary/binary/vary */

static ray_t* sig_fn(ray_t* x);   /* body homed with the signal channel below */

/* qSQL execution stub — the q_funsql.c executor was demolished (eval rebuild,
 * spec 2026-07-23); select/exec/delete and ?[t;c;b;a]/![t;c;b;a] re-land as a
 * logical plan + backend router in the qSQL wave. */
static ray_t* qsql_nyi(ray_t** args, int64_t n) {
    (void)args; (void)n;
    return ray_error("nyi", NULL);
}

typedef struct { const char* wire; uint8_t kind; uint32_t flags; void* fn; } q_special_t;

static const q_special_t SPECIALS[SPEC_N] = {
    [SPEC_scan]          = { "scan",            SK_VARY,   RAY_FN_NONE,         (void*)q_scan_wrap },
    [SPEC_over]          = { "over",            SK_VARY,   RAY_FN_NONE,         (void*)q_over_wrap },
    [SPEC_eachboth]      = { "each-both",       SK_VARY,   RAY_FN_NONE,         (void*)q_eachboth_wrap },
    [SPEC_prior]         = { "each-prior",      SK_VARY,   RAY_FN_NONE,         (void*)q_prior_wrap },
    [SPEC_mkderiv2]      = { "q.mkderiv2",      SK_BINARY, RAY_FN_NONE,         (void*)q_deriv_mkderiv2 },
    [SPEC_mkopproj]      = { "q.mkopproj",      SK_VARY,   RAY_FN_NONE,         (void*)q_deriv_mkopproj },
    /* ctx constructor heads: SPECIAL_FORM so q_ctx_build gets the raw element
     * trees and evaluates them right-to-left inside a pushed scope */
    [SPEC_list]          = { "list",            SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)q_list_build },
    [SPEC_table]         = { "table",           SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)q_table_build },
    [SPEC_keyed_table]   = { "keyed-table",     SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)q_keyed_table_build },
    /* qSQL executors: 'nyi stubs until the plan-router wave (spec 2026-07-23) */
    [SPEC_select]        = { "q.select",        SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)qsql_nyi },
    [SPEC_delete]        = { "q.delete",        SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)qsql_nyi },
    [SPEC_exec]          = { "q.exec",          SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)qsql_nyi },
    /* compose builder — a NORMAL vary (args are resolved function values) */
    [SPEC_compose]       = { "q.compose",       SK_VARY,   RAY_FN_NONE,         (void*)q_compose_fn },
    [SPEC_funsql_select] = { "q.funsql.select", SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)qsql_nyi },
    [SPEC_funsql_bang]   = { "q.funsql.bang",   SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)qsql_nyi },
    [SPEC_lambda]        = { "q.fn",            SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)q_deriv_fn_make },
    [SPEC_ret]           = { "q.ret",           SK_UNARY,  RAY_FN_NONE,         (void*)q_ret_fn },
    [SPEC_sig]           = { "q.sig",           SK_UNARY,  RAY_FN_NONE,         (void*)sig_fn },
    /* imperative control — SPECIAL_FORM: args arrive unevaluated, the body
     * drives its own lazy left-to-right evaluation (basics/control.md) */
    [SPEC_seq]           = { "q.seq",           SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)q_seq_fn },
    [SPEC_if]            = { "q.if",            SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)q_if_fn },
    [SPEC_do]            = { "q.do",            SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)q_do_fn },
    [SPEC_while]         = { "q.while",         SK_VARY,   RAY_FN_SPECIAL_FORM, (void*)q_while_fn },
};
_Static_assert(sizeof SPECIALS / sizeof SPECIALS[0] == SPEC_N, "SPECIALS row count must match SPEC_* enum");

static ray_t* g_specials[SPEC_N];

ray_t* q_registry_scan_value(void)          { return g_specials[SPEC_scan]; }          /* borrowed */
ray_t* q_registry_over_value(void)          { return g_specials[SPEC_over]; }
ray_t* q_registry_eachboth_value(void)      { return g_specials[SPEC_eachboth]; }
ray_t* q_registry_prior_value(void)         { return g_specials[SPEC_prior]; }
ray_t* q_registry_mkderiv2_value(void)      { return g_specials[SPEC_mkderiv2]; }
ray_t* q_registry_mkopproj_value(void)      { return g_specials[SPEC_mkopproj]; }
ray_t* q_registry_list_value(void)          { return g_specials[SPEC_list]; }
ray_t* q_registry_table_value(void)         { return g_specials[SPEC_table]; }
ray_t* q_registry_keyed_table_value(void)   { return g_specials[SPEC_keyed_table]; }
ray_t* q_registry_select_value(void)        { return g_specials[SPEC_select]; }
ray_t* q_registry_delete_value(void)        { return g_specials[SPEC_delete]; }
ray_t* q_registry_exec_value(void)          { return g_specials[SPEC_exec]; }
ray_t* q_registry_compose_value(void)       { return g_specials[SPEC_compose]; }
ray_t* q_registry_funsql_select_value(void) { return g_specials[SPEC_funsql_select]; }
ray_t* q_registry_funsql_bang_value(void)   { return g_specials[SPEC_funsql_bang]; }
ray_t* q_registry_lambda_value(void)        { return g_specials[SPEC_lambda]; }
ray_t* q_registry_ret_value(void)           { return g_specials[SPEC_ret]; }
ray_t* q_registry_sig_value(void)           { return g_specials[SPEC_sig]; }
ray_t* q_registry_seq_value(void)           { return g_specials[SPEC_seq]; }
ray_t* q_registry_if_value(void)            { return g_specials[SPEC_if]; }
ray_t* q_registry_do_value(void)            { return g_specials[SPEC_do]; }
ray_t* q_registry_while_value(void)         { return g_specials[SPEC_while]; }

/* ---- the `'x` signal channel (registry-lifecycle thread-local state) ------
 * The <=7-char error class in err->sdata truncates, but kdb Trap hands the
 * handler the WHOLE message, so sig_fn stashes the untruncated text here
 * (mirroring the q.ret payload in q_deriv.c).  take returns OWNED or NULL. */
static _Thread_local ray_t* qsig_payload = NULL;

ray_t* q_registry_sig_take(void) {
    ray_t* v = qsig_payload;
    qsig_payload = NULL;
    return v;
}

void q_registry_sig_clear(void) {
    if (qsig_payload) { ray_release(qsig_payload); qsig_payload = NULL; }
}

/* `'x` Signal (ref/signal.md): abort with error class = the sym spelling /
 * string text (ray_error copies, 7-char sdata cap — kdb's own classes are
 * short for the same reason).  Full untruncated text stashed for Trap. */
static ray_t* sig_fn(ray_t* x) {
    char cls[8] = "signal";
    ray_t* full = NULL;                 /* owned full-text string, or NULL */
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        if (s) {
            size_t l = ray_str_len(s); size_t c = l > 7 ? 7 : l;
            memcpy(cls, ray_str_ptr(s), c); cls[c] = '\0';
            full = ray_str(ray_str_ptr(s), l);
            ray_release(s);
        }
    } else if (x) {
        const char* p; int64_t l;                     /* string / charv / char atom */
        if (q_text_bytes(x, &p, &l)) {
            size_t c = (size_t)l > 7 ? 7 : (size_t)l;
            memcpy(cls, p, c); cls[c] = '\0';
            full = ray_str(p, (size_t)l);
        }
    }
    if (qsig_payload) ray_release(qsig_payload);
    qsig_payload = full;                /* owned (or NULL) */
    return ray_error(cls, NULL);
}

/* ---- shared q-name sanitization (.Q.id + construction clash repair) ------ */

static int name_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int name_is_keyword_reserved(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return 0;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    int nop = 0;
    const q_op_t* ops = q_ops_table(&nop);
    int hit = 0;
    for (int i = 0; i < nop && !hit; i++) {
        if (ops[i].lex == QLEX_GLYPH || ops[i].lex == QLEX_ADVERB) continue;
        size_t m = strlen(ops[i].name);
        hit = (m == n && memcmp(ops[i].name, p, n) == 0);
    }
    ray_release(s);
    return hit;
}

static int name_prev_contains(const int64_t* previous, int64_t n_previous,
                                int64_t sym_id) {
    for (int64_t i = 0; i < n_previous; i++)
        if (previous[i] == sym_id) return 1;
    return 0;
}

static int64_t name_append_suffix(int64_t sym_id, int64_t suffix) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return sym_id;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    char stack[128];
    char* buf = stack;
    int need = snprintf(NULL, 0, "%.*s%lld", (int)n, p, (long long)suffix);
    if (need < 0) { ray_release(s); return sym_id; }
    if ((size_t)need + 1 > sizeof stack) {
        buf = (char*)malloc((size_t)need + 1);
        if (!buf) { ray_release(s); return sym_id; }
    }
    snprintf(buf, (size_t)need + 1, "%.*s%lld", (int)n, p, (long long)suffix);
    int64_t out = ray_sym_intern_runtime(buf, (size_t)need);
    if (buf != stack) free(buf);
    ray_release(s);
    return out;
}

int64_t q_registry_name_sanitize(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return ray_sym_intern_runtime("a", 1);
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    char stack[128];
    char* buf = (n + 2 <= sizeof stack) ? stack : (char*)malloc(n + 2);
    if (!buf) { ray_release(s); return ray_sym_intern_runtime("a", 1); }
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (name_char_ok(p[i])) buf[w++] = p[i];
    if (w == 0) buf[w++] = 'a';
    if (buf[0] == '_' || (buf[0] >= '0' && buf[0] <= '9')) {
        memmove(buf + 1, buf, w);
        buf[0] = 'a';
        w++;
    }
    int64_t out = ray_sym_intern_runtime(buf, w);
    if (buf != stack) free(buf);
    ray_release(s);
    return out;
}

int64_t q_name_dedup(int64_t sym_id, const int64_t* previous, int64_t n_previous,
                     int check_reserved) {
    int64_t base = sym_id;
    if (check_reserved && name_is_keyword_reserved(base))
        base = name_append_suffix(base, 1);
    if (!name_prev_contains(previous, n_previous, base)) return base;
    for (int64_t i = 1; i < INT64_MAX; i++) {
        int64_t cand = name_append_suffix(base, i);
        if (!name_prev_contains(previous, n_previous, cand)) return cand;
    }
    return base;
}

ray_t* q_registry_name_reserved_words(void) {
    int nop = 0;
    const q_op_t* ops = q_ops_table(&nop);
    int n = 0;
    for (int i = 0; i < nop; i++)
        if (ops[i].lex != QLEX_GLYPH && ops[i].lex != QLEX_ADVERB) n++;
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int i = 0; i < nop; i++) {
        if (ops[i].lex == QLEX_GLYPH || ops[i].lex == QLEX_ADVERB) continue;
        int64_t id = ray_sym_intern_runtime(ops[i].name, strlen(ops[i].name));
        out = ray_vec_append(out, &id);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    }
    return out;
}

/* ---- collapse: homogeneous atom list -> typed vector (see q_registry.h) ---- */

ray_t* q_collapse_list(ray_t* l) {
    if (!l || RAY_IS_ERR(l) || l->type != RAY_LIST || ray_len(l) == 0) {
        if (l) ray_retain(l);
        return l;
    }
    int64_t n = ray_len(l);
    ray_t** e = (ray_t**)ray_data(l);
    int8_t t = e[0] ? e[0]->type : 0;
    if (t >= 0 || t == -RAY_STR) { ray_retain(l); return l; }   /* not a scalar-atom run */
    for (int64_t i = 1; i < n; i++)
        if (!e[i] || e[i]->type != t) { ray_retain(l); return l; }

    if (t == -RAY_SYM) {
        ray_t* vec = ray_sym_vec_new(RAY_SYM_W64, n);
        if (RAY_IS_ERR(vec)) return vec;
        for (int64_t i = 0; i < n; i++) vec = ray_vec_append(vec, &e[i]->i64);
        return vec;
    }

    ray_t* vec = ray_vec_new(-t, n);
    if (RAY_IS_ERR(vec)) return vec;
    int64_t nulls = 0;
    for (int64_t i = 0; i < n; i++) {
        /* Switch the recovered POSITIVE tag, exhaustive over ray_type_e (no
         * default) so a future member demands a lane (#209 guard).  Only the
         * non-i64 reads live here; i64/temporal tags AND any out-of-enum tag
         * fall to the shared i64 append below — byte-identical to the old
         * default (U8 LE-aliased).  LIST/STR/SYM are dead arms (filtered above). */
        bool appended = false;
        switch ((ray_type_e)-t) {
        case RAY_BOOL: vec = ray_vec_append(vec, &e[i]->b8);  appended = true; break;
        case RAY_I16:  vec = ray_vec_append(vec, &e[i]->i16); appended = true; break;
        case RAY_I32:  vec = ray_vec_append(vec, &e[i]->i32); appended = true; break;
        case RAY_F32: { float f = (float)e[i]->f64;           /* F32 atom stores f64 */
                        vec = ray_vec_append(vec, &f); appended = true; } break;
        case RAY_F64:
        case RAY_DATETIME:
                       vec = ray_vec_append(vec, &e[i]->f64); appended = true; break;
        case RAY_GUID: {                                      /* 16-byte payload, not i64 */
            const void* g = e[i]->obj ? ray_data(e[i]->obj) : ray_data(e[i]);
            vec = ray_vec_append(vec, g); appended = true;
        } break;
        RAY_BYTE_CASES: /* explicit u8 read — byte + char atoms store the payload
                         * in u8; no LE-aliasing through the shared i64 append */
                       vec = ray_vec_append(vec, &e[i]->u8); appended = true; break;
        case RAY_I64:  case RAY_TIMESTAMP: case RAY_MONTH:
        case RAY_DATE: case RAY_TIMESPAN: case RAY_MINUTE:    case RAY_SECOND:
        case RAY_TIME: case RAY_LIST: case RAY_STR: case RAY_SYM:
                       break;
        }
        if (!appended) vec = ray_vec_append(vec, &e[i]->i64);   /* i64/temporal + out-of-enum */
        if (RAY_IS_ERR(vec)) return vec;
        if (RAY_ATOM_IS_NULL(e[i])) { ray_vec_set_null(vec, i, true); nulls++; }
    }
    (void)nulls;
    return vec;
}

/* v[i] as an owned atom/element (borrowed v): direct payload read for
 * vectors/lists (collection_elem — no index atom, no ray_at_fn dispatch);
 * generic indexing for every other shape.  alloc==0 results are BORROWED
 * list slots — retain, never release (r0 review). */
ray_t* q_registry_elem_at(ray_t* v, int64_t i) {
    if (v && (ray_is_vec(v) || v->type == RAY_LIST)) {
        int alloc = 0;
        ray_t* e = collection_elem(v, i, &alloc);
        if (e && !RAY_IS_ERR(e)) { if (!alloc) ray_retain(e); return e; }
        if (e && alloc) ray_release(e);   /* allocated error: generic fallback */
    }
    ray_t* ia = ray_i64(i);
    ray_t* e  = ray_at_fn(v, ia);   /* owned */
    ray_release(ia);
    return e;
}

/* ---- string-C3 boundary conversion (single home, q_registry.h) ---- */

/* MATERIALIZES (one O(len) memcpy), never a view: engine amend writes through
 * slices and SSO atoms (<=6 bytes, header-inline) cannot be slice parents
 * (stage-0 audit §2) — zero-copy stays a later constructor-internal option. */
ray_t* q_charv_of_str(ray_t* s) {
    if (!s || RAY_IS_ERR(s)) return s;               /* errors pass through (no-op rc) */
    if (s->type != -RAY_STR) return ray_error("type", "charv: expects a string atom");
    return ray_charv(ray_str_ptr(s), (int64_t)ray_str_len(s));
}

ray_t* q_str_of_charv(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;               /* errors pass through (no-op rc) */
    if (x->type == -RAY_CHARV) { char c = (char)x->u8; return ray_str(&c, 1); }
    if (x->type != RAY_CHARV) return ray_error("type", "charv: expects char text");
    return ray_str((const char*)ray_data(x), (size_t)ray_len(x));
}

bool q_text_bytes(ray_t* x, const char** p, int64_t* n) {
    if (!x || RAY_IS_ERR(x)) return false;
    if (x->type == -RAY_STR)  { *p = ray_str_ptr(x); *n = (int64_t)ray_str_len(x); return true; }
    if (x->type == RAY_CHARV) { *p = (const char*)ray_data(x); *n = ray_len(x); return true; }
    if (x->type == -RAY_CHARV){ *p = (const char*)&x->u8; *n = 1; return true; }
    return false;
}

/* Inverse adapter for legacy string-verb bodies (vs/sv/like/ss/ssr...):
 * BORROWS x, returns OWNED legacy form — charv/char atom -> -RAY_STR atom;
 * LIST elements converted recursively; everything else retained as-is. */
ray_t* q_str_in(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) { if (x) ray_retain(x); return x; }
    if (x->type == RAY_CHARV || x->type == -RAY_CHARV) return q_str_of_charv(x);
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        bool any = false;
        for (int64_t i = 0; i < n && !any; i++)
            any = e[i] && (e[i]->type == RAY_CHARV || e[i]->type == -RAY_CHARV ||
                           e[i]->type == RAY_LIST);
        if (!any) { ray_retain(x); return x; }
        ray_t* out = ray_list_new(n);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        for (int64_t i = 0; i < n; i++) {
            ray_t* c = q_str_in(e[i]);
            if (RAY_IS_ERR(c)) { ray_release(out); return c; }
            out = ray_list_append(out, c);
            if (c) ray_release(c);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    ray_retain(x);
    return x;
}

/* Boundary-out walk (consumes r, returns owned): -RAY_STR atom -> charv;
 * RAY_STR vector -> 0h list of charv; LIST -> elements converted (in place at
 * rc==1, else a fresh list); DICT -> values converted (fresh dict unless
 * nothing converts, or values are a TABLE = keyed table -> untouched); TABLE
 * and everything else pass through (columns stay pooled below the boundary). */
static bool charv_out_needed(ray_t* r) {
    if (!r) return false;
    if (ray_is_lazy(r)) return false;    /* deferred DAG values: never walk */
    if (r->type == -RAY_STR || r->type == RAY_STR) return true;
    if (r->type == RAY_LIST) {
        if (q_deriv_kind_of(r) != Q_DERIV_NONE) return false;  /* fn carriers:
            * their -RAY_STR source is an internal carrier, never a value */
        ray_t** e = (ray_t**)ray_data(r);
        for (int64_t i = 0; i < ray_len(r); i++)
            if (charv_out_needed(e[i])) return true;
    }
    if (r->type == RAY_DICT) {
        ray_t* vals = ray_dict_vals(r);
        return vals && vals->type != RAY_TABLE && charv_out_needed(vals);
    }
    return false;
}

ray_t* q_charv_out(ray_t* r) {
    if (!r || RAY_IS_ERR(r)) return r;
    if (ray_is_lazy(r)) return r;        /* deferred DAG values: never walk */
    if (r->type == -RAY_STR) {
        ray_t* v = q_charv_of_str(r);
        ray_release(r);
        return v;
    }
    if (r->type == RAY_STR) {                    /* extracted column -> 0h list */
        int64_t n = ray_len(r);
        ray_t* out = ray_list_new(n);
        if (!out || RAY_IS_ERR(out)) { ray_release(r); return out ? out : ray_error("oom", NULL); }
        for (int64_t i = 0; i < n; i++) {
            size_t sl = 0;
            const char* sp = ray_str_vec_get(r, i, &sl);
            ray_t* cv = ray_charv(sp ? sp : "", (int64_t)sl);
            if (RAY_IS_ERR(cv)) { ray_release(out); ray_release(r); return cv; }
            out = ray_list_append(out, cv);
            ray_release(cv);
            if (RAY_IS_ERR(out)) { ray_release(r); return out; }
        }
        ray_release(r);
        return out;
    }
    if (r->type == RAY_LIST && charv_out_needed(r)) {
        int64_t n = ray_len(r);
        ray_t** e = (ray_t**)ray_data(r);
        if (r->rc == 1) {                        /* sole owner: rewrite in place */
            for (int64_t i = 0; i < n; i++) {
                ray_t* c = q_charv_out(e[i]);    /* consumes the slot's ref */
                if (RAY_IS_ERR(c)) { e[i] = RAY_NULL_OBJ; ray_release(r); return c; }
                e[i] = c;
            }
            return r;
        }
        ray_t* out = ray_list_new(n);
        if (!out || RAY_IS_ERR(out)) { ray_release(r); return out ? out : ray_error("oom", NULL); }
        for (int64_t i = 0; i < n; i++) {
            ray_retain(e[i]);
            ray_t* c = q_charv_out(e[i]);
            if (RAY_IS_ERR(c)) { ray_release(out); ray_release(r); return c; }
            out = ray_list_append(out, c);
            ray_release(c);
            if (RAY_IS_ERR(out)) { ray_release(r); return out; }
        }
        ray_release(r);
        return out;
    }
    if (r->type == RAY_DICT) {
        ray_t* vals = ray_dict_vals(r);          /* borrowed */
        if (vals && vals->type != RAY_TABLE && charv_out_needed(vals)) {
            ray_retain(vals);
            ray_t* nv = q_charv_out(vals);       /* consumes our retain */
            if (RAY_IS_ERR(nv)) { ray_release(r); return nv; }
            ray_t* keys = ray_dict_keys(r);      /* borrowed */
            ray_retain(keys);                    /* dict_new consumes both */
            ray_t* nd = ray_dict_new(keys, nv);
            ray_release(r);
            return nd;
        }
        return r;
    }
    return r;
}

/* ---- value builders keyed by manifest build-kind ---- */

/* Identity/rename-reuse: snapshot an existing rayfall builtin value by name and
 * retain it (the registry owns one ref).  Returns NULL if the audited source is
 * absent — a real bug, so q_registry_init fails fast. */
static ray_t* build_env(const char* env_name) {
    ray_t* e = ray_env_get(ray_sym_intern(env_name, strlen(env_name)));
    if (!e) return NULL;
    ray_retain(e);
    return e;
}

/* Bespoke wrapper: the manifest row carries the wrapper FUNCTION POINTER and
 * its binding shape (q_recipe_t arity/atomic) — no per-verb enum dispatch.
 * aux-name = the canonical rayfall lowering name; flagged RAY_FN_Q_LOWER so
 * the compiler/query DAG name-route it.
 *
 * SERDE LIMITATION (codex P1, deferred to 2b): generic function serialization
 * (src/store/serde.c) writes ray_fn_name and deserializes via ray_env_get, so a
 * serialized wrapper would come back as the plain like-named builtin (losing the
 * q string/arg-swap semantics), and `_`->"drop" has no env binding at all.  This
 * is NOT reachable in stage 2a: with no parser flip, wrapper VALUES never become
 * user-visible or serializable — they exist only inside the registry and the
 * transient AST that q_lower feeds straight to eval.  (The pre-2a wrappers, named
 * "="/"#"/"_", were equally non-round-trippable — env has no such names.)  2b,
 * which makes value heads user-visible, must teach serde to recognise
 * RAY_FN_Q_LOWER and re-derive the wrapper from the registry; that fix has a
 * testable surface only once the parser embeds these values. */
static ray_t* build_wrapper(const q_recipe_t* r) {
    uint8_t attrs = (uint8_t)((r->atomic ? RAY_FN_ATOMIC : RAY_FN_NONE) | RAY_FN_Q_LOWER);
    switch (r->arity) {
    case 1:  return ray_fn_unary (r->target, attrs, (ray_unary_fn)r->fn);
    case 2:  return ray_fn_binary(r->target, attrs, (ray_binary_fn)r->fn);
    default: return ray_fn_vary  (r->target, attrs, (ray_vary_fn)r->fn);
    }
}

/* Record one (name, valence) entry.  Returns RAY_OK, or RAY_ERR_DOMAIN if the
 * builder produced NULL/err for a non-QR_NONE recipe (audited source missing). */
static ray_err_t add_entry(const q_op_t* op, q_valence_t valence,
                           const q_recipe_t* r) {
    /* Bootstrap invariant (codex #1): entries are only ever built inside
     * q_registry_init's build window, and a builder must never re-enter the
     * parser.  The builders below touch only ray_env_get / ray_fn_* — never
     * q_parse — so this holds by construction; the assert pins it. */
    assert(g_building);
    if (r->kind == QK_NONE) return RAY_OK;              /* nothing at this valence */
    if (r->kind == QK_QSRC) return RAY_OK;              /* value comes from q.q —
                                                         * installed post-bootstrap
                                                         * by q_registry_bind_qsrc */
    ray_t* val = (r->kind == QK_ENV) ? build_env(r->target) : build_wrapper(r);
    if (!val || RAY_IS_ERR(val)) return RAY_ERR_DOMAIN; /* fail-fast: audited bug */
    entry_t* e    = &g_entries[g_count++];
    e->sym_id     = ray_sym_intern(op->name, strlen(op->name));
    e->valence    = valence;
    e->value      = val;
    e->row        = op;
    e->spelling   = op->name;
    e->lower_name = r->target;                          /* rayfall routing name */
    e->is_wrapper = (r->kind != QK_ENV);
    return RAY_OK;
}

/* ---- serde hooks: q wrappers round-trip through the registry -------------
 * A RAY_FN_Q_LOWER wrapper serialized by aux-name would deserialize as the
 * like-named env builtin (wrong arg order for `#`, no binding at all for
 * `_`->"drop"), silently losing q semantics.  The writer claims registry
 * wrappers with a `q!<spelling>!<valence>` wire name (fits the standard
 * 15-byte slot); the reader decodes it back to THE registry value.  Internal
 * spelling-less values (scan/list) have no provenance and fall through to
 * the env path — documented, not silently wrong (env scan/list exist). */

static int serde_fn_writer(ray_t* fn, char out[16]) {
    if (!fn || !(fn->attrs & RAY_FN_Q_LOWER)) return 0;
    q_provenance_t pv;
    if (!q_registry_provenance(fn, &pv) || !pv.is_wrapper) return 0;
    int n = snprintf(out, 16, "q!%s!%d", pv.spelling, (int)pv.valence);
    return n > 0 && n < 16;
}

static ray_t* serde_fn_reader(const char* name) {
    if (!name || name[0] != 'q' || name[1] != '!') return NULL;
    const char* sp = name + 2;
    const char* bang = strrchr(sp, '!');
    if (!bang || bang == sp) return NULL;
    q_valence_t v = (q_valence_t)atoi(bang + 1);
    if (v != Q_MONADIC && v != Q_DYADIC) return NULL;
    ray_t* hit = q_registry_lookup_name(sp, (size_t)(bang - sp), v);
    if (!hit) return NULL;      /* falls through to the env path -> name error */
    ray_retain(hit);
    return hit;                 /* owned, per the hook contract */
}

/* ---- API ---- */

ray_err_t q_registry_init(void) {
    if (g_inited) return RAY_OK;   /* idempotent */
    g_count    = 0;
    g_building = true;
    int n = 0;
    const q_op_t* ops = q_ops_table(&n);
    /* Cap check: g_entries sized for 2 per row.  Static roster, so this is a
     * build-time invariant, asserted for future growth. */
    assert(2 * n <= (int)(sizeof g_entries / sizeof g_entries[0]));
    for (int i = 0; i < n; i++) {
        const q_op_t* op = &ops[i];
        if (add_entry(op, Q_MONADIC, &op->mon)  != RAY_OK) {
            g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
        }
        if (add_entry(op, Q_DYADIC,  &op->dyad) != RAY_OK) {
            g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
        }
    }
    /* internal (spelling-less) specials — ONE build loop over SPECIALS[]; the
     * failed-build error path is written once (RAY_FN_Q_LOWER stamped here). */
    for (int s = 0; s < SPEC_N; s++) {
        const q_special_t* sp = &SPECIALS[s];
        uint8_t attrs = (uint8_t)(sp->flags | RAY_FN_Q_LOWER);
        ray_t* v;
        switch ((enum spec_kind)sp->kind) {
        case SK_UNARY:  v = ray_fn_unary (sp->wire, attrs, (ray_unary_fn)sp->fn);  break;
        case SK_BINARY: v = ray_fn_binary(sp->wire, attrs, (ray_binary_fn)sp->fn); break;
        default:        v = ray_fn_vary  (sp->wire, attrs, (ray_vary_fn)sp->fn);   break;
        }
        if (!v || RAY_IS_ERR(v)) {
            g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
        }
        g_specials[s] = v;
    }
    g_building = false;
    g_inited   = true;
    ray_serde_set_fn_hooks(serde_fn_writer, serde_fn_reader);
    return RAY_OK;
}

bool q_registry_ready(void) {
    return g_inited;
}

/* Install the QK_QSRC cells from the loaded q.q definitions (contract in
 * q_registry.h).  Same entry shape as add_entry, but the value is a SNAPSHOT
 * of the `.q.<target>` env binding rather than a built one — immutable like
 * every other cell (`.q` is a reserved root only the bootstrap writes).
 * is_wrapper=1: the value is unique per cell, so pointer-identity provenance
 * is exact (q_registry_provenance).  Serde is UNTOUCHED by that flag: the
 * `q!…` fn-hook fires only for RAY_FN_Q_LOWER function values; carriers keep
 * riding whatever generic carrier serde exists. */
static ray_err_t bind_qsrc_one(const q_op_t* op, q_valence_t valence,
                               const q_recipe_t* r) {
    if (r->kind != QK_QSRC) return RAY_OK;
    char full[64];
    size_t tl = strlen(r->target);
    if (tl + 3 >= sizeof full) return RAY_ERR_DOMAIN;
    memcpy(full, ".q.", 3);
    memcpy(full + 3, r->target, tl);
    ray_t* val = ray_env_get(ray_sym_intern(full, tl + 3));   /* borrowed */
    if (!val || RAY_IS_ERR(val)) return RAY_ERR_DOMAIN;       /* q.q drift bug */
    ray_retain(val);
    entry_t* e    = &g_entries[g_count++];
    e->sym_id     = ray_sym_intern(op->name, strlen(op->name));
    e->valence    = valence;
    e->value      = val;
    e->row        = op;
    e->spelling   = op->name;
    e->lower_name = r->target;
    e->is_wrapper = 1;
    return RAY_OK;
}

ray_err_t q_registry_bind_qsrc(void) {
    if (!g_inited) return RAY_ERR_DOMAIN;
    int n = 0;
    const q_op_t* ops = q_ops_table(&n);
    for (int i = 0; i < n; i++) {
        /* idempotent per runtime: a cell already present was bound earlier */
        if (ops[i].mon.kind == QK_QSRC &&
            !q_registry_lookup_name(ops[i].name, strlen(ops[i].name), Q_MONADIC) &&
            bind_qsrc_one(&ops[i], Q_MONADIC, &ops[i].mon) != RAY_OK)
            return RAY_ERR_DOMAIN;
        if (ops[i].dyad.kind == QK_QSRC &&
            !q_registry_lookup_name(ops[i].name, strlen(ops[i].name), Q_DYADIC) &&
            bind_qsrc_one(&ops[i], Q_DYADIC, &ops[i].dyad) != RAY_OK)
            return RAY_ERR_DOMAIN;
    }
    return RAY_OK;
}

ray_t* q_registry_lookup(int64_t sym_id, q_valence_t valence) {
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].sym_id == sym_id && g_entries[i].valence == valence)
            return g_entries[i].value;   /* borrowed */
    return NULL;
}

ray_t* q_registry_lookup_name(const char* s, size_t n, q_valence_t valence) {
    return q_registry_lookup(ray_sym_intern(s, n), valence);
}

ray_t* q_registry_lookup_row(int64_t sym_id, q_valence_t valence,
                             const q_op_t** row_out) {
    if (row_out) *row_out = NULL;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].sym_id == sym_id && g_entries[i].valence == valence) {
            if (row_out) *row_out = g_entries[i].row;
            return g_entries[i].value;   /* borrowed */
        }
    }
    return NULL;
}

/* Exact for unique values; NULL when several rows alias one env object at
 * this valence (see q_registry.h).  Linear scan, same cost class as lookup. */
const q_op_t* q_registry_row_of(const ray_t* value, q_valence_t valence) {
    const q_op_t* row = NULL;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].value != value || g_entries[i].valence != valence)
            continue;
        if (row && row != g_entries[i].row) return NULL;   /* aliased: ambiguous */
        row = g_entries[i].row;
    }
    return row;
}

bool q_registry_provenance(const ray_t* value, q_provenance_t* out) {
    /* Wrapper values are UNIQUE objects (born rc=1 per row) — pointer identity
     * is exact for them.  Pass-through/rename values ARE the shared env builtin
     * object, and several q spellings can alias one env object (e.g. `#`-monadic
     * and `count` both embed env `count`; `-`-monadic and `neg` both embed env
     * `neg`).  For those, pointer identity alone cannot recover THE q spelling —
     * an inherent limitation of the reuse-the-env-object design.  So: prefer the
     * unique WRAPPER entry (always correct); for an aliased pass-through, return
     * the first-registered spelling (2b's formatter disambiguates aliased
     * pass-throughs from the parse-site glyph, not from this value-keyed API). */
    int first = -1;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].value != value) continue;
        if (g_entries[i].is_wrapper) { first = i; break; }   /* unique — exact */
        if (first < 0) first = i;                            /* remember first */
    }
    if (first < 0) return false;
    if (out) {
        out->spelling   = g_entries[first].spelling;
        out->valence    = g_entries[first].valence;
        out->lower_name = g_entries[first].lower_name;
        out->is_wrapper = g_entries[first].is_wrapper;
    }
    return true;
}

/* Idempotent; also serves as partial-cleanup on a failed init (guards on
 * g_count, not g_inited, so a half-built table is fully released). */
void q_registry_destroy(void) {
    ray_serde_set_fn_hooks(NULL, NULL);   /* hooks read g_entries — detach first */
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].value) ray_release(g_entries[i].value);
    for (int s = 0; s < SPEC_N; s++)
        if (g_specials[s]) { ray_release(g_specials[s]); g_specials[s] = NULL; }
    { ray_t* stale = q_deriv_ret_take(); if (stale) ray_release(stale); }
    q_registry_sig_clear();
    g_count  = 0;
    g_inited = false;
}
