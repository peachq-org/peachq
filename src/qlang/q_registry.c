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

/* ---- shared q-name sanitization (.Q.id + construction clash repair) ------ */

static int q_name_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int q_name_is_keyword_reserved(int64_t sym_id) {
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

static int q_name_prev_contains(const int64_t* previous, int64_t n_previous,
                                int64_t sym_id) {
    for (int64_t i = 0; i < n_previous; i++)
        if (previous[i] == sym_id) return 1;
    return 0;
}

static int64_t q_name_append_suffix(int64_t sym_id, int64_t suffix) {
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

int64_t q_name_sanitize(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return ray_sym_intern_runtime("a", 1);
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    char stack[128];
    char* buf = (n + 2 <= sizeof stack) ? stack : (char*)malloc(n + 2);
    if (!buf) { ray_release(s); return ray_sym_intern_runtime("a", 1); }
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (q_name_char_ok(p[i])) buf[w++] = p[i];
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
    if (check_reserved && q_name_is_keyword_reserved(base))
        base = q_name_append_suffix(base, 1);
    if (!q_name_prev_contains(previous, n_previous, base)) return base;
    for (int64_t i = 1; i < INT64_MAX; i++) {
        int64_t cand = q_name_append_suffix(base, i);
        if (!q_name_prev_contains(previous, n_previous, cand)) return cand;
    }
    return base;
}

ray_t* q_name_reserved_words(void) {
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
        switch (t) {
        case -RAY_BOOL: vec = ray_vec_append(vec, &e[i]->b8);  break;
        case -RAY_I16:  vec = ray_vec_append(vec, &e[i]->i16); break;
        case -RAY_I32:  vec = ray_vec_append(vec, &e[i]->i32); break;
        case -RAY_F32: { float f = (float)e[i]->f64;            /* F32 atom stores f64 */
                         vec = ray_vec_append(vec, &f); }       break;
        case -RAY_F64:
        case -RAY_DATETIME:
                        vec = ray_vec_append(vec, &e[i]->f64); break;
        case -RAY_GUID: {                                      /* 16-byte payload, not i64 */
            const void* g = e[i]->obj ? ray_data(e[i]->obj) : ray_data(e[i]);
            vec = ray_vec_append(vec, g);
        } break;
        default:        vec = ray_vec_append(vec, &e[i]->i64); break; /* i64 + temporals */
        }
        if (RAY_IS_ERR(vec)) return vec;
        if (RAY_ATOM_IS_NULL(e[i])) { ray_vec_set_null(vec, i, true); nulls++; }
    }
    (void)nulls;
    return vec;
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

/* Bespoke wrapper: aux-name = the canonical rayfall lowering name; flagged
 * RAY_FN_Q_LOWER so the compiler/query DAG name-route it.
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
static ray_t* build_wrapper(q_build_kind kind, const char* lower_name) {
    switch (kind) {
    case QK_EQ:   return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_eq_wrap);
    case QK_NE:   return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_ne_wrap);
    case QK_TAKE: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_take_wrap);
    case QK_DROP: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_drop_wrap);
    case QK_CUT:  return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_cut_wrap);
    case QK_EACH: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_each_wrap);
    case QK_MATCH:return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_match_wrap);
    case QK_FLOOR:return ray_fn_unary (lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_floor_wrap);
    case QK_BANG: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_bang_wrap);
    case QK_KEY:  return ray_fn_unary (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_key_wrap);
    case QK_VALUE:return ray_fn_unary (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_value_wrap);
    case QK_DISTINCT:
                  return ray_fn_unary (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_distinct_wrap);
    case QK_ROLL: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_roll_wrap);
    case QK_RAND: return ray_fn_unary (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_rand_wrap);
    case QK_CAST: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_cast_wrap);
    case QK_AT:   return ray_fn_vary  (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_at_wrap);
    case QK_DOT:  return ray_fn_vary  (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_dot_wrap);
    case QK_MIN2: return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_min2_wrap);
    case QK_NEG:  return ray_fn_unary (lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_neg_wrap);
    case QK_WITHIN: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_within_wrap);
    case QK_OVER:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_over_kw);
    case QK_SCANKW: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_scan_kw);
    case QK_PRIORKW:return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_prior_kw);
    case QK_DELTAS: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_deltas_wrap);
    case QK_DIFFER: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_differ_wrap);
    case QK_TIL:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_til_wrap);
    case QK_WHERE:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_where_wrap);
    case QK_REV:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_reverse_wrap);
    case QK_JOIN:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_join_wrap);
    case QK_VS:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_vs_wrap);
    case QK_SV:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_sv_wrap);
    case QK_SUMS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_sums_wrap);
    case QK_PRDS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_prds_wrap);
    case QK_PRD:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_prd_wrap);
    case QK_MAXS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_maxs_wrap);
    case QK_MINS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mins_wrap);
    case QK_AVGS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_avgs_wrap);
    case QK_RATIOS: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ratios_wrap);
    case QK_WSUM:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_wsum_wrap);
    case QK_WAVG:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_wavg_wrap);
    case QK_COV:    return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_cov_wrap);
    case QK_SCOV:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_scov_wrap);
    case QK_MSUM:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_msum_wrap);
    case QK_MAVG:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mavg_wrap);
    case QK_MMAX:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mmax_wrap);
    case QK_MMIN:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mmin_wrap);
    case QK_MCOUNT: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mcount_wrap);
    case QK_MDEV:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mdev_wrap);
    case QK_EMA:    return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ema_wrap);
    case QK_FILL:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_fill_wrap);
    case QK_ROTATE: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_rotate_wrap);
    case QK_SUBLIST:return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_sublist_wrap);
    case QK_NEXT:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_next_wrap);
    case QK_PREV:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_prev_wrap);
    case QK_XPREV:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xprev_wrap);
    case QK_IN:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_in_wrap);
    case QK_FILLS:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_fills_wrap);
    case QK_HOPEN:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_hopen_wrap);
    case QK_HCLOSE: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_hclose_wrap);
    case QK_FILETEXT: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_filetext_wrap);
    case QK_HSYM:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_hsym_wrap);
    case QK_READ0:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_read0_wrap);
    /* NB: deliberately NOT RAY_FN_AGGR — the eval aggregate fast path claims
     * AGGR fns before the wrapper runs and 'types on a boxed list-of-vectors;
     * name-routing (RAY_FN_Q_LOWER + aux "sum") keeps query/DAG behaviour. */
    case QK_SUM:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_sum_wrap);
    case QK_ATTR:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_attr_wrap);
    case QK_NULL:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_null_wrap);
    case QK_RAZE:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_raze_wrap);
    case QK_LIKE:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_like_wrap);
    case QK_SS:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ss_wrap);
    case QK_SSR:    return ray_fn_vary  (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ssr_wrap);
    case QK_UNION:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_union_wrap);
    case QK_INTER:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_inter_wrap);
    case QK_CROSS:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_cross_wrap);
    case QK_SIN:        return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_sin_wrap);
    case QK_COS:        return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_cos_wrap);
    case QK_TAN:        return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_tan_wrap);
    case QK_ASIN:       return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_asin_wrap);
    case QK_ACOS:       return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_acos_wrap);
    case QK_ATAN:       return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_atan_wrap);
    case QK_RECIPROCAL: return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_reciprocal_wrap);
    /* signum drives its own broadcast (q_null_wrap pattern) so a top-level
     * boxed-list result collapses to an int vector — see q_signum_wrap. */
    case QK_SIGNUM:     return ray_fn_unary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_signum_wrap);
    case QK_CEILING:    return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_ceiling_wrap);
    case QK_XEXP:       return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_xexp_wrap);
    case QK_XLOG:       return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_xlog_wrap);
    case QK_XBAR:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xbar_wrap);
    case QK_ASC:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_asc_wrap);
    case QK_DESC:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_desc_wrap);
    case QK_IASC:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_iasc_wrap);
    case QK_IDESC:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_idesc_wrap);
    /* ---- table verbs (feat/q-table-verbs) ---- */
    case QK_FLIP:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_flip_wrap);
    case QK_KEYS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_keys_wrap);
    case QK_UNGROUP:return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ungroup_wrap);
    case QK_XKEY:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xkey_wrap);
    case QK_XCOL:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xcol_wrap);
    case QK_XCOLS:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xcols_wrap);
    case QK_XASC:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xasc_wrap);
    case QK_XDESC:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xdesc_wrap);
    case QK_XGROUP: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xgroup_wrap);
    case QK_INSERT: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_insert_wrap);
    case QK_UPSERT: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_upsert_wrap);
    case QK_EXCEPT: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_except_wrap);
    case QK_LJ:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_lj_wrap);
    case QK_LJF:    return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ljf_wrap);
    case QK_IJ:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ij_wrap);
    case QK_IJF:    return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ijf_wrap);
    case QK_UJ:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_uj_wrap);
    case QK_UJF:    return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ujf_wrap);
    case QK_PJ:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_pj_wrap);
    case QK_ASOF:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_asof_wrap);
    case QK_SETG:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_setg_wrap);
    /* ---- environment variables (feat/q-getenv-setenv) ---- */
    case QK_GETENV: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_getenv_wrap);
    case QK_SETENV: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_setenv_wrap);
    default:      return NULL;
    }
}

/* Build the owned value for one (kind, target). */
static ray_t* build_value(q_build_kind kind, const char* target) {
    if (kind == QK_NONE) return NULL;
    if (kind == QK_ENV)  return build_env(target);
    return build_wrapper(kind, target);
}

/* Record one (name, valence) entry.  Returns RAY_OK, or RAY_ERR_DOMAIN if the
 * builder produced NULL/err for a non-QK_NONE kind (audited source missing). */
static ray_err_t add_entry(const char* name, q_valence_t valence,
                           q_build_kind kind, const char* target) {
    /* Bootstrap invariant (codex #1): entries are only ever built inside
     * q_registry_init's build window, and a builder must never re-enter the
     * parser.  build_value below touches only ray_env_get / ray_fn_* — never
     * q_parse — so this holds by construction; the assert pins it. */
    assert(g_building);
    if (kind == QK_NONE) return RAY_OK;                 /* nothing at this valence */
    ray_t* val = build_value(kind, target);
    if (!val || RAY_IS_ERR(val)) return RAY_ERR_DOMAIN; /* fail-fast: audited bug */
    entry_t* e    = &g_entries[g_count++];
    e->sym_id     = ray_sym_intern(name, strlen(name));
    e->valence    = valence;
    e->value      = val;
    e->spelling   = name;
    e->lower_name = target;                             /* rayfall routing name */
    e->is_wrapper = (kind != QK_ENV);
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

static int q_serde_fn_writer(ray_t* fn, char out[16]) {
    if (!fn || !(fn->attrs & RAY_FN_Q_LOWER)) return 0;
    q_provenance_t pv;
    if (!q_registry_provenance(fn, &pv) || !pv.is_wrapper) return 0;
    int n = snprintf(out, 16, "q!%s!%d", pv.spelling, (int)pv.valence);
    return n > 0 && n < 16;
}

static ray_t* q_serde_fn_reader(const char* name) {
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
        if (add_entry(op->name, Q_MONADIC, op->mon_kind,  op->mon_target)  != RAY_OK) {
            g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
        }
        if (add_entry(op->name, Q_DYADIC,  op->dyad_kind, op->dyad_target) != RAY_OK) {
            g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
        }
    }
    /* internal (spelling-less) values consumed by q_lower / the parser */
    g_scan_value = ray_fn_vary("scan", RAY_FN_NONE | RAY_FN_Q_LOWER, q_scan_wrap);
    if (!g_scan_value || RAY_IS_ERR(g_scan_value)) {
        g_scan_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_over_value = ray_fn_vary("over", RAY_FN_NONE | RAY_FN_Q_LOWER, q_over_wrap);
    if (!g_over_value || RAY_IS_ERR(g_over_value)) {
        g_over_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_eachboth_value = ray_fn_vary("each-both", RAY_FN_NONE | RAY_FN_Q_LOWER, q_eachboth_wrap);
    if (!g_eachboth_value || RAY_IS_ERR(g_eachboth_value)) {
        g_eachboth_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_prior_value = ray_fn_vary("each-prior", RAY_FN_NONE | RAY_FN_Q_LOWER, q_prior_wrap);
    if (!g_prior_value || RAY_IS_ERR(g_prior_value)) {
        g_prior_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_mkderiv2_value = ray_fn_binary("q.mkderiv2", RAY_FN_NONE | RAY_FN_Q_LOWER, q_mkderiv2);
    if (!g_mkderiv2_value || RAY_IS_ERR(g_mkderiv2_value)) {
        g_mkderiv2_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_mkopproj_value = ray_fn_vary("q.mkopproj", RAY_FN_NONE | RAY_FN_Q_LOWER, q_mkopproj);
    if (!g_mkopproj_value || RAY_IS_ERR(g_mkopproj_value)) {
        g_mkopproj_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    /* Both ctx constructor heads are SPECIAL_FORM: q_ctx_build must receive the
     * raw element trees to evaluate them right-to-left inside a pushed scope. */
    g_list_value = ray_fn_vary("list",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_list_build);
    if (!g_list_value || RAY_IS_ERR(g_list_value)) {
        g_list_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_table_value = ray_fn_vary("table",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_table_build);
    if (!g_table_value || RAY_IS_ERR(g_table_value)) {
        g_table_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_keyed_table_value = ray_fn_vary("keyed-table",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_keyed_table_build);
    if (!g_keyed_table_value || RAY_IS_ERR(g_keyed_table_value)) {
        g_keyed_table_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_select_value = ray_fn_vary("q.select",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_select_exec);
    if (!g_select_value || RAY_IS_ERR(g_select_value)) {
        g_select_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_delete_value = ray_fn_vary("q.delete",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_delete_exec);
    if (!g_delete_value || RAY_IS_ERR(g_delete_value)) {
        g_delete_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_exec_value = ray_fn_vary("q.exec",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_exec_exec);
    if (!g_exec_value || RAY_IS_ERR(g_exec_value)) {
        g_exec_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    /* compose builder — a NORMAL vary (args are the resolved function values). */
    g_compose_value = ray_fn_vary("q.compose", RAY_FN_Q_LOWER, q_compose_fn);
    if (!g_compose_value || RAY_IS_ERR(g_compose_value)) {
        g_compose_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    /* functional qSQL executors — SPECIAL FORMs: they receive t/c/b/a
     * UNEVALUATED and evaluate them internally (funsql_operand), so the `()`
     * empty marker (parser `::` name-ref) does not trip ray_eval's name check. */
    g_funsql_select_value = ray_fn_vary("q.funsql.select",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_funsql_select);
    if (!g_funsql_select_value || RAY_IS_ERR(g_funsql_select_value)) {
        g_funsql_select_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_funsql_bang_value = ray_fn_vary("q.funsql.bang",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_funsql_bang);
    if (!g_funsql_bang_value || RAY_IS_ERR(g_funsql_bang_value)) {
        g_funsql_bang_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_lambda_value = ray_fn_vary("q.fn",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_fn_make);
    if (!g_lambda_value || RAY_IS_ERR(g_lambda_value)) {
        g_lambda_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_ret_value = ray_fn_unary("q.ret", RAY_FN_Q_LOWER, q_ret_fn);
    if (!g_ret_value || RAY_IS_ERR(g_ret_value)) {
        g_ret_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_sig_value = ray_fn_unary("q.sig", RAY_FN_Q_LOWER, q_sig_fn);
    if (!g_sig_value || RAY_IS_ERR(g_sig_value)) {
        g_sig_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_seq_value = ray_fn_vary("q.seq",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_seq_fn);
    if (!g_seq_value || RAY_IS_ERR(g_seq_value)) {
        g_seq_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_if_value = ray_fn_vary("q.if",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_if_fn);
    if (!g_if_value || RAY_IS_ERR(g_if_value)) {
        g_if_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_do_value = ray_fn_vary("q.do",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_do_fn);
    if (!g_do_value || RAY_IS_ERR(g_do_value)) {
        g_do_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_while_value = ray_fn_vary("q.while",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_while_fn);
    if (!g_while_value || RAY_IS_ERR(g_while_value)) {
        g_while_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_building = false;
    g_inited   = true;
    ray_serde_set_fn_hooks(q_serde_fn_writer, q_serde_fn_reader);
    return RAY_OK;
}

bool q_registry_ready(void) {
    return g_inited;
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
    if (g_scan_value)  { ray_release(g_scan_value);  g_scan_value  = NULL; }
    if (g_over_value)     { ray_release(g_over_value);     g_over_value     = NULL; }
    if (g_eachboth_value) { ray_release(g_eachboth_value); g_eachboth_value = NULL; }
    if (g_prior_value)    { ray_release(g_prior_value);    g_prior_value    = NULL; }
    if (g_mkderiv2_value) { ray_release(g_mkderiv2_value); g_mkderiv2_value = NULL; }
    if (g_mkopproj_value) { ray_release(g_mkopproj_value); g_mkopproj_value = NULL; }
    if (g_list_value)   { ray_release(g_list_value);   g_list_value   = NULL; }
    if (g_table_value)  { ray_release(g_table_value);  g_table_value  = NULL; }
    if (g_keyed_table_value) { ray_release(g_keyed_table_value); g_keyed_table_value = NULL; }
    if (g_select_value) { ray_release(g_select_value); g_select_value = NULL; }
    if (g_delete_value) { ray_release(g_delete_value); g_delete_value = NULL; }
    if (g_exec_value)   { ray_release(g_exec_value);   g_exec_value   = NULL; }
    if (g_compose_value) { ray_release(g_compose_value); g_compose_value = NULL; }
    if (g_funsql_select_value) { ray_release(g_funsql_select_value); g_funsql_select_value = NULL; }
    if (g_funsql_bang_value)   { ray_release(g_funsql_bang_value);   g_funsql_bang_value   = NULL; }
    if (g_lambda_value)        { ray_release(g_lambda_value);        g_lambda_value        = NULL; }
    if (g_ret_value)           { ray_release(g_ret_value);           g_ret_value           = NULL; }
    if (g_sig_value)           { ray_release(g_sig_value);           g_sig_value           = NULL; }
    if (g_seq_value)           { ray_release(g_seq_value);           g_seq_value           = NULL; }
    if (g_if_value)            { ray_release(g_if_value);            g_if_value            = NULL; }
    if (g_do_value)            { ray_release(g_do_value);            g_do_value            = NULL; }
    if (g_while_value)         { ray_release(g_while_value);         g_while_value         = NULL; }
    if (g_qret_payload)        { ray_release(g_qret_payload);        g_qret_payload        = NULL; }
    if (g_qsig_payload)        { ray_release(g_qsig_payload);        g_qsig_payload        = NULL; }
    g_count  = 0;
    g_inited = false;
}
