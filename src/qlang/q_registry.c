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
#define Q_OPS_ENV_GRANDFATHER /* legitimate owner: registry QR_ENV recipes snapshot env values */
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_err.h"
#include "lang/env.h"      /* ray_env_get; ray_fn_unary/binary/vary — building the fn-values */
#include "lang/eval.h"     /* RAY_FN_ATOMIC/SPECIAL_FORM/Q_LOWER — attrs stamped on built values */
#include "lang/internal.h" /* ray_sym_str — name display */
#include "table/sym.h"     /* ray_sym_intern_runtime, RAY_SYM_W64 — name interning */
#include "store/serde.h"   /* ray_serde_set_fn_hooks — fn-value serde round-trip */
#include "qlang/eval/q_eval.h" /* q_eval_apply_iter_new — the iterator carriers */
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
 * ONE plain data table drives the g_specials[] slots, the init build loop,
 * the teardown release loop, and the borrowed-ref accessors below.  Since the
 * eval-rebuild cutover the survivors are PARSER-EMBEDDED MARKER heads that
 * the evaluator intercepts by pointer identity (the literal ctor in q_eval,
 * compose in the apply module) — the bodies never run, so all share spec_nyi.
 * Accessors return BORROWED refs, NULL before init. */
enum { SPEC_list, SPEC_compose, SPEC_N };

enum spec_kind { SK_UNARY, SK_BINARY, SK_VARY };   /* -> ray_fn_unary/binary/vary */

static ray_t* spec_nyi(ray_t** args, int64_t n) {
    (void)args; (void)n;
    return q_err(QE_NYI);
}

typedef struct { const char* wire; uint8_t kind; uint32_t flags; void* fn; } q_special_t;

static const q_special_t SPECIALS[SPEC_N] = {
    /* ctx constructor head: SPECIAL_FORM marker — q_eval builds the literal
     * natively after intercepting it by pointer */
    [SPEC_list]        = { "list",        SK_VARY, RAY_FN_SPECIAL_FORM, (void*)spec_nyi },
    /* compose `'[f;g;…]` head — the apply module's compose_apply claims it */
    [SPEC_compose]     = { "q.compose",   SK_VARY, RAY_FN_NONE,         (void*)spec_nyi },
};
_Static_assert(sizeof SPECIALS / sizeof SPECIALS[0] == SPEC_N, "SPECIALS row count must match SPEC_* enum");

static ray_t* g_specials[SPEC_N];

/* the six iterator values (adv 0=' 1=/ 2=\ 3=': 4=/: 5=\:) — immutable
 * singletons like the specials, so `~` settles them on pointer identity */
static ray_t* g_iters[6];

ray_t* q_registry_list_value(void)          { return g_specials[SPEC_list]; }          /* borrowed */
ray_t* q_registry_compose_value(void)       { return g_specials[SPEC_compose]; }
ray_t* q_registry_iter_value(int adv) {
    return (adv >= 0 && adv < 6) ? g_iters[adv] : NULL;
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
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    for (int i = 0; i < nop; i++) {
        if (ops[i].lex == QLEX_GLYPH || ops[i].lex == QLEX_ADVERB) continue;
        int64_t id = ray_sym_intern_runtime(ops[i].name, strlen(ops[i].name));
        out = ray_vec_append(out, &id);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    }
    return out;
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
 * transient AST the evaluator consumes.  (The pre-2a wrappers, named
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
    for (int a = 0; a < 6; a++) {
        ray_t* v = q_eval_apply_iter_new(a);
        if (!v || RAY_IS_ERR(v)) {
            if (v) ray_release(v);
            g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
        }
        g_iters[a] = v;
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

ray_t* q_registry_qsrc_ns(void) {
    ray_t* v = ray_env_get(ray_sym_intern(".q", 2));      /* borrowed */
    return (v && !RAY_IS_ERR(v) && v->type == RAY_DICT) ? v : NULL;
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
    for (int a = 0; a < 6; a++)
        if (g_iters[a]) { ray_release(g_iters[a]); g_iters[a] = NULL; }
    g_count  = 0;
    g_inited = false;
}
