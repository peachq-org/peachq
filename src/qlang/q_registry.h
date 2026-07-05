/* q op registry — parse-time (q-name, valence) -> ray_t function value.
 *
 * A separate q-owned table layered over rayfall; it does NOT mutate rayfall's
 * global env (see ADR 0001, ADR 0002).  Each entry reuses a rayfall builtin
 * (identity- or rename-reuse) or is a thin q-semantics wrapper.  Verb values
 * are snapshotted immutably at init: a later user `set +` on g_env does NOT
 * change what the registry returns — the registry is the authoritative,
 * immutable verb source.
 *
 * Valence-split: lookup is keyed by (name, valence) because the parser emits
 * bare glyphs with no monad/dyad tag and most q glyphs mean different things
 * monadically vs dyadically.
 *
 * Ownership / lifetime:
 *   ray_lang_init -> q_registry_init -> (lookup/use) -> q_registry_destroy -> ray_env_destroy
 * The registry RETAINS one ref per stored value (both env snapshots and
 * wrappers).  q_registry_lookup* returns a BORROWED ref; a consumer that
 * embeds a value into an AST must retain it.  q_registry_destroy releases
 * every retained entry and MUST run before ray_env_destroy (the q runtime
 * factory pair q_runtime_create / q_runtime_destroy enforces this ordering). */
#ifndef Q_REGISTRY_H
#define Q_REGISTRY_H

#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum { Q_MONADIC = 1, Q_DYADIC = 2 } q_valence_t;

/* First-class q-surface provenance carried alongside each registry value.
 * The value's rayfall aux-name is its LOWERING name (canonical rayfall verb,
 * e.g. q `=` lowers as "=="); provenance records the ORIGINAL q spelling and
 * valence so the 2b formatter can render the q glyph without depending on the
 * (routing-purposed) aux-name.  `is_wrapper` distinguishes a bespoke
 * q-semantics op from a pass-through/rename. */
typedef struct {
    const char* spelling;    /* q surface name, e.g. "=", "#", "%"        */
    q_valence_t valence;     /* the valence this value serves             */
    const char* lower_name;  /* canonical rayfall routing name, e.g. "==" */
    int         is_wrapper;  /* 1 = bespoke q wrapper; 0 = pass-through    */
} q_provenance_t;

/* Build the table once, AFTER ray_lang_init has populated g_env.  Idempotent:
 * a second call while already initialised is a no-op returning RAY_OK.  Fails
 * fast (RAY_ERR_DOMAIN) if an audited builtin source is absent or a wrapper
 * allocation fails — the roster is bound against verified registrations, so a
 * miss is a bug, not a soft skip. */
ray_err_t q_registry_init(void);

/* True once q_registry_init has completed.  The PARSER embeds registry values
 * at verb heads, so q_parse fails fast when this is false (codex #1: value
 * embedding must never run against a cold registry). */
bool q_registry_ready(void);

/* Borrowed ref, or NULL on miss.  A miss is NOT an error — it means "not a
 * registry verb at this valence," and the caller keeps the token a
 * name-reference (ADR 0002's unknown->name-ref rule). */
ray_t* q_registry_lookup(int64_t sym_id, q_valence_t valence);

/* Convenience over q_registry_lookup: interns s[0..n) to a sym-id first.
 * Interning the probed name is the canonical key derivation — the parser
 * interns every verb via ray_sym_intern_runtime, and interning is idempotent
 * for any real verb.  A genuinely novel miss adds one sym-table entry, which
 * matches parser behaviour and is intentional. */
ray_t* q_registry_lookup_name(const char* s, size_t n, q_valence_t valence);

/* Recover the q-surface provenance of a registry value (by pointer identity).
 * Returns true and fills *out on a hit; false if `value` is not a registry
 * value.  Consumed by the 2b formatter to print the original q glyph.
 *
 * EXACT for WRAPPER values (unique objects).  Pass-through/rename values are the
 * shared env builtin object, and several q spellings may alias one env object
 * (e.g. `#`-monadic and `count`); for those, this returns the first-registered
 * spelling — 2b disambiguates aliased pass-throughs from the parse-site glyph,
 * not from this value-keyed lookup. */
bool q_registry_provenance(const ray_t* value, q_provenance_t* out);

/* Release every retained entry and reset.  Idempotent; also serves as
 * partial-cleanup on a failed init.  Must run before ray_env_destroy. */
void q_registry_destroy(void);

/* The internal scan value (rayfall scan + collapse) q_lower embeds for the
 * `\` adverb.  Borrowed; NULL before q_registry_init.  Not a roster row —
 * it has no q spelling of its own. */
ray_t* q_registry_scan_value(void);

/* The internal paren-list constructor (list + collapse) the PARSER embeds at
 * the head of every multi-element paren list `(1;2;3)` — the head value is
 * what distinguishes a literal from the shape-identical index call (v;i).
 * q_fmt hides this head, so the tree still displays (1;2;3).  Borrowed;
 * NULL before q_registry_init. */
ray_t* q_registry_list_value(void);

/* The internal table constructor `([] a:…; b:…)` the PARSER embeds at the head
 * of every table literal.  Shares the right-to-left context machinery with the
 * paren-list value (q_ctx_build) — list and table def are ONE mechanism.
 * Borrowed; NULL before q_registry_init. */
ray_t* q_registry_table_value(void);

/* The qSQL SELECT adapter value q_lower embeds when it lowers the functional
 * 5-list (?;`t;c;b;a) onto the base ray_select engine.  Special form; its two
 * operands are the rayfall query dict and the by-key column-name sym vector.
 * Borrowed; NULL before q_registry_init. */
ray_t* q_registry_select_value(void);

/* The functional-qSQL executor values q_lower embeds at the head of a rank-4
 * `?[t;c;b;a]` (select/exec) or `![t;c;b;a]` (update/delete) application.  They
 * are regular (arg-evaluating) vary fns — ray_eval evaluates the four operands
 * to VALUES first.  Borrowed; NULL before q_registry_init. */
ray_t* q_registry_funsql_select_value(void);
ray_t* q_registry_funsql_bang_value(void);

/* Collapse a boxed RAY_LIST of homogeneous scalar atoms into the matching
 * typed vector (kdb semantics: map/each/scan results and paren-lists of atoms
 * are simple vectors, not general lists).  Mixed types, non-atom elements,
 * string atoms (a list of strings IS kdb 0h) and non-lists are returned
 * unchanged.  Borrows `l`; always returns an OWNED value (a fresh vector, or
 * `l` retained). */
ray_t* q_collapse_list(ray_t* l);

/* q-name sanitization shared by .Q.id and openq construction paths that must
 * repair name clashes.  q_name_sanitize returns an interned symbol id for the
 * `.Q.id` atom rule.  q_name_dedup takes an already-sanitized/generated symbol
 * and resolves reserved-word and previous-name clashes by appending 1,2,...
 * using the same table/dict column-name rule. */
int64_t q_name_sanitize(int64_t sym_id);
int64_t q_name_dedup(int64_t sym_id, const int64_t* previous, int64_t n_previous,
                     int check_reserved);
ray_t* q_name_reserved_words(void);

#endif /* Q_REGISTRY_H */
