/* q op registry — see q_registry.h for the contract.
 *
 * Roster source = a divergence audit of the ACTUAL builtin registrations
 * (src/lang/eval.c:ray_register_builtins + the fn bodies under src/ops), not
 * stale notes.  The non-obvious ground truth the roster depends on:
 *   - rayfall `%` is MODULO and `/` is FLOAT-DIVIDE (7/2 -> 3.5) — so q's `%`
 *     (float divide) rename-reuses rayfall `/`, and q's `#`/`_` cannot reuse
 *     `remove` (dict-key deletion) or `take` (opposite arg order): they need
 *     wrappers.  q monadic `-` is negate, aliased to `neg`.
 * Deferred (not bound in the MVP — the parser emits these untagged and their
 * monad/dyad valence design is unsettled, so a clean miss->name-ref beats a
 * guess): monadic + * % ; glyphs & | < > = ~ , ! @ ? $ . ; adverbs / \ ' ;
 * monadic # _.  The roster itself is the SPECS[] table below.
 */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry.h"
#include "lang/env.h"    /* ray_env_get, ray_fn_binary */
#include "lang/eval.h"   /* builtin fn decls, RAY_FN_* */
#include <string.h>
#include <stdlib.h>

/* A roster row: a q (name, valence) key and a builder that produces its owned
 * function VALUE (rc>=1).  A per-row builder (instead of a kind enum + switch)
 * keeps each flavour's construction self-contained and makes a new wrapper a
 * one-line addition to SPECS[]. */
typedef struct spec_s spec_t;
typedef ray_t* (*build_fn)(const spec_t* s);
struct spec_s {
    const char* q_name;    /* registry key name */
    q_valence_t valence;
    build_fn    build;     /* produces the owned fn value for this row */
    const char* env_name;  /* env source for build_env; NULL for wrappers */
};

/* builders — defined below, near the wrappers they reference */
static ray_t* build_env(const spec_t* s);
static ray_t* build_take(const spec_t* s);
static ray_t* build_drop(const spec_t* s);
static ray_t* build_eq(const spec_t* s);
static ray_t* build_ne(const spec_t* s);

static const spec_t SPECS[] = {
    /* identity-reuse (dyadic glyphs) */
    { "+",       Q_DYADIC,  build_env,  "+" },
    { "-",       Q_DYADIC,  build_env,  "-" },
    { "*",       Q_DYADIC,  build_env,  "*" },
    /* rename-reuse */
    { "-",       Q_MONADIC, build_env,  "neg" },   /* q monadic minus = negate  */
    { "%",       Q_DYADIC,  build_env,  "/" },     /* q float-divide = rayfall / */
    /* comparison renames — q `=`/`<>` map to rayfall `==`/`!=`.  The ordering
     * glyphs `< > <= >=` and the `div` keyword are NOT listed: their q spelling
     * equals the rayfall builtin name, so a registry miss falls through to the
     * identically-named env binding.  Sources verified present in
     * ray_register_builtins (eval.c); a missing one trips the init fail-fast. */
    { "=",       Q_DYADIC,  build_eq,   NULL },    /* q equal      = rayfall == */
    { "<>",      Q_DYADIC,  build_ne,   NULL },    /* q not-equal  = rayfall != */
    /* identity-reuse (monadic keywords) */
    { "neg",     Q_MONADIC, build_env,  "neg" },
    { "til",     Q_MONADIC, build_env,  "til" },
    { "count",   Q_MONADIC, build_env,  "count" },
    { "first",   Q_MONADIC, build_env,  "first" },
    { "last",    Q_MONADIC, build_env,  "last" },
    { "where",   Q_MONADIC, build_env,  "where" },
    { "reverse", Q_MONADIC, build_env,  "reverse" },
    { "sum",     Q_MONADIC, build_env,  "sum" },
    /* wrappers (new q-semantics values over a rayfall primitive) */
    { "#",       Q_DYADIC,  build_take, NULL },    /* q n#list (arg-swap take)  */
    { "_",       Q_DYADIC,  build_drop, NULL },    /* q n_list (count-drop)     */
};
#define N_SPECS ((int)(sizeof SPECS / sizeof SPECS[0]))

typedef struct { int64_t sym_id; q_valence_t valence; ray_t* value; } entry_t;
static entry_t g_entries[N_SPECS];
static int     g_count  = 0;
static bool    g_inited = false;

/* ---- wrappers ---- */

/* q `n # list` — take.  rayfall ray_take_fn(vec, n) has the opposite arg
 * order, so swap.  Borrows both args (does not release them). */
static ray_t* q_take_wrap(ray_t* n, ray_t* list) {
    return ray_take_fn(list, n);
}

/* q `n _ list` — count-drop (NOT rayfall's dict key-remove).  n>=0 drops the
 * first n elements; n<0 drops the last |n|.  Implemented as a range-take
 * ray_take_fn(list, (start; amount)), which clamps at the ends.  Borrows args.
 *
 * Length is derived string-aware: a q string is a -RAY_STR atom whose char
 * count lives in ray_str_len, NOT the ->len union field (which aliases the SSO
 * {slen,sdata} bytes), so ray_len would be garbage for strings. */
static ray_t* q_drop_wrap(ray_t* n, ray_t* list) {
    if (!n || n->type != -RAY_I64)
        return ray_error("type", "_ (drop): count must be an integer");
    if (!list) return ray_error("type", "_ (drop): nil list");
    int64_t len;
    if (list->type == -RAY_STR)
        len = (int64_t)ray_str_len(list);           /* SSO-safe string length */
    else if (ray_is_vec(list) || list->type == RAY_LIST)
        len = ray_len(list);                         /* typed vector / boxed list */
    else
        return ray_error("type", "_ (drop): expects a list, vector, or string");
    int64_t k = n->i64;
    int64_t start, amount;
    if (k >= 0) { start = (k < len) ? k : len; amount = len - start; }
    else        { start = 0; amount = len + k; if (amount < 0) amount = 0; }
    int64_t rng[2] = { start, amount };
    ray_t* range = ray_vec_from_raw(RAY_I64, rng, 2);
    if (RAY_IS_ERR(range)) return range;
    ray_t* r = ray_take_fn(list, range);
    ray_release(range);
    return r;
}

/* q char-string comparison — q treats a string as a char vector, so `=`/`<>`
 * compare element-wise and yield a boolean vector (`"abc"="abd"` -> 110b).
 * rayfall's `==`/`!=` (ray_eq_fn/ray_neq_fn) compare two -RAY_STR atoms as
 * whole values (a single 0b/1b), so the q verbs wrap them: two equal-length
 * string atoms take the element-wise path here, everything else (ints, floats,
 * vectors, symbols, mismatched-length strings) delegates unchanged to the
 * rayfall builtin.  Returns NULL to signal "not the string case, delegate". */
static ray_t* q_str_cmp_vec(ray_t* a, ray_t* b, int eq) {
    if (!a || !b || a->type != -RAY_STR || b->type != -RAY_STR) return NULL;
    const char* pa = ray_str_ptr(a); size_t la = ray_str_len(a);
    const char* pb = ray_str_ptr(b); size_t lb = ray_str_len(b);
    if (la != lb) return NULL;                 /* length mismatch -> delegate */
    uint8_t stack[128];
    uint8_t* bits = (la <= sizeof stack) ? stack : (uint8_t*)malloc(la ? la : 1);
    if (!bits) return ray_error("wsfull", "=: out of memory");
    for (size_t i = 0; i < la; i++)
        bits[i] = (uint8_t)(eq ? (pa[i] == pb[i]) : (pa[i] != pb[i]));
    ray_t* r = ray_vec_from_raw(RAY_BOOL, bits, (int64_t)la);
    if (bits != stack) free(bits);
    return r;
}

/* q `=` — element-wise over char strings, else rayfall `==`.  RAY_FN_ATOMIC so
 * eval broadcasts it over numeric vectors (each element pair hits ray_eq_fn). */
static ray_t* q_eq_wrap(ray_t* a, ray_t* b) {
    ray_t* r = q_str_cmp_vec(a, b, 1);
    return r ? r : ray_eq_fn(a, b);
}

/* q `<>` — element-wise over char strings, else rayfall `!=`. */
static ray_t* q_ne_wrap(ray_t* a, ray_t* b) {
    ray_t* r = q_str_cmp_vec(a, b, 0);
    return r ? r : ray_neq_fn(a, b);
}

/* ---- builders ---- */

/* Identity/rename-reuse: snapshot an existing rayfall builtin value by name and
 * retain it (the registry owns one ref).  Returns NULL if the audited source is
 * absent — a real bug, so q_registry_init fails fast. */
static ray_t* build_env(const spec_t* s) {
    ray_t* e = ray_env_get(ray_sym_intern(s->env_name, strlen(s->env_name)));
    if (!e) return NULL;
    ray_retain(e);
    return e;
}
static ray_t* build_take(const spec_t* s) {
    return ray_fn_binary(s->q_name, RAY_FN_NONE, q_take_wrap);   /* born rc=1 */
}
static ray_t* build_drop(const spec_t* s) {
    return ray_fn_binary(s->q_name, RAY_FN_NONE, q_drop_wrap);   /* born rc=1 */
}
static ray_t* build_eq(const spec_t* s) {
    return ray_fn_binary(s->q_name, RAY_FN_ATOMIC, q_eq_wrap);   /* born rc=1 */
}
static ray_t* build_ne(const spec_t* s) {
    return ray_fn_binary(s->q_name, RAY_FN_ATOMIC, q_ne_wrap);   /* born rc=1 */
}

/* ---- API ---- */

ray_err_t q_registry_init(void) {
    if (g_inited) return RAY_OK;   /* idempotent */
    g_count = 0;
    for (int i = 0; i < N_SPECS; i++) {
        const spec_t* s = &SPECS[i];
        ray_t* val = s->build(s);
        if (!val || RAY_IS_ERR(val)) { q_registry_destroy(); return RAY_ERR_DOMAIN; }
        g_entries[g_count].sym_id  = ray_sym_intern(s->q_name, strlen(s->q_name));
        g_entries[g_count].valence = s->valence;
        g_entries[g_count].value   = val;
        g_count++;
    }
    g_inited = true;
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

/* Idempotent; also serves as partial-cleanup on a failed init (guards on
 * g_count, not g_inited, so a half-built table is fully released). */
void q_registry_destroy(void) {
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].value) ray_release(g_entries[i].value);
    g_count  = 0;
    g_inited = false;
}
