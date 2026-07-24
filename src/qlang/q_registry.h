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

/* Install the QK_QSRC manifest cells: for every row whose recipe is marked
 * value-from-q.q, snapshot the (now-loaded) `.q.<target>` binding as the
 * cell's immutable value.  Called once by q_runtime_create AFTER the q.q
 * bootstrap load (the definitions cannot exist at q_registry_init time).
 * Fails fast (RAY_ERR_DOMAIN) on a missing `.q` binding — a q.q/manifest
 * drift bug, mirroring q_registry_init's missing-builtin policy. */
ray_err_t q_registry_bind_qsrc(void);

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

/* (value, manifest-row) pair lookup for the fresh evaluator (eval-rebuild
 * finding 2: family/mono attach at registration, keyed per entry — never
 * recovered post-hoc through q_registry_provenance's first-spelling rule).
 * Same borrowed-value contract as q_registry_lookup; *row_out gets the
 * entry's Q_OPS[] row (NULL only for values with no manifest row). */
struct q_op;
ray_t* q_registry_lookup_row(int64_t sym_id, q_valence_t valence,
                             const struct q_op** row_out);

/* Manifest row for a PARSER-EMBEDDED head value at the given valence.
 * Exact for wrapper/QSRC cells (unique objects).  A pass-through env value
 * aliased by several rows at that valence returns NULL — ambiguous by
 * construction (the `#`-monadic vs `count` class); the fresh apply module
 * then dispatches on the value's own attrs (RAY_FN_ATOMIC) alone. */
const struct q_op* q_registry_row_of(const ray_t* value, q_valence_t valence);

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

/* q `hsym x` / `attr x` verb implementations — exported so the q_bang.c
 * internal-fn aliases (`-1!` -> hsym, `-2!` -> attr) route to the SAME single
 * home the registry verb uses (Direction B, bang-ops-internal-status.md). */
ray_t* q_hsym_wrap(ray_t* x);
ray_t* q_attr_wrap(ray_t* x);

/* q `enlist` vary wrapper (base ray_enlist_fn + dict -> 1-row table arm) —
 * env-bound by q_builtins_register before registry init so both `enlist`
 * and monadic `,` share it. */
ray_t* q_enlist_wrap(ray_t** args, int64_t n);

/* Release every retained entry and reset.  Idempotent; also serves as
 * partial-cleanup on a failed init.  Must run before ray_env_destroy. */
void q_registry_destroy(void);

/* PARSER-EMBEDDED marker heads (borrowed; NULL before q_registry_init).
 * q_eval intercepts the literal-ctor values by pointer identity and builds
 * natively; q_fmt hides them in display.
 *   list        — paren-list literal `(1;2;3)` (the head value is what
 *                 distinguishes a literal from the shape-identical (v;i))
 *   table       — table literal `([] a:…; b:…)`
 *   keyed-table — keyed-table literal `([k:…] v:…)` (args[0] = key count)
 *   compose     — `'[f;g;…]` bracket form ('nyi until the adverb wave) */
ray_t* q_registry_list_value(void);
ray_t* q_registry_table_value(void);
ray_t* q_registry_keyed_table_value(void);
ray_t* q_registry_compose_value(void);

/* Collapse a boxed RAY_LIST of homogeneous scalar atoms into the matching
 * typed vector (kdb semantics: map/each/scan results and paren-lists of atoms
 * are simple vectors, not general lists).  Mixed types, non-atom elements,
 * string atoms (a list of strings IS kdb 0h) and non-lists are returned
 * unchanged.  Borrows `l`; always returns an OWNED value (a fresh vector, or
 * `l` retained).  DEFINED in ops/q_list.c; declared here so env-using callers
 * (net/, apply) reach it without the poisoned q_registry_internal.h. */
ray_t* q_list_collapse(ray_t* l);

/* ---- string-C3 boundary conversion (spec Design §3: physical RAY_STR never
 * appears in q-space; values in flight are charv; columns stay pooled).
 * DEFINED in ops/q_str.c; declared here (env-safe public reach). ---- */

/* THE single-home STR->charv constructor.  MATERIALIZES (one O(len) memcpy);
 * borrows s, returns an owned fresh charv vector (always a vector). */
ray_t* q_str_charv_of_str(ray_t* s);

/* charv vector or char atom -> owned -RAY_STR atom (for engine internals that
 * need pooled/NUL-terminated physical form).  Borrows x. */
ray_t* q_str_of_charv(ray_t* x);

/* Text-bytes accessor over all q text forms: -RAY_STR atom, charv vector,
 * char atom.  Borrowed pointer valid while x lives; false = not text. */
bool q_str_text_bytes(ray_t* x, const char** p, int64_t* n);

/* Boundary-out walk: CONSUMES r, returns owned.  -RAY_STR atom -> charv;
 * RAY_STR vector -> 0h list of charv; LIST/DICT values converted (in place
 * only at rc==1); TABLE (incl. keyed-table value side) passes untouched. */
ray_t* q_str_charv_out(ray_t* r);

/* Inverse adapter for legacy string-verb bodies: BORROWS x, returns OWNED
 * legacy form (charv/char atom -> -RAY_STR atom, LIST recursed). */
ray_t* q_str_in(ray_t* x);

/* Column attribute as kdb's single letter: 's'/'u'/'g'/'p', or 0 for none.
 * Reads the block markers/kind DIRECTLY (the kdb u#/p# policy is composed in the
 * q layer, not the rayfall-native engine `.attr.get`, so a hash-backed u#/p#
 * must be labelled by its marker, not the RAY_IDX_HASH kind — see q_registry.c).
 * Shared by the `attr` verb wrapper AND q_fmt's `` `s#``/`u#``/`g#``/`p# ``
 * display prefix so both agree on one mapping.  Borrows v. */
char q_attr_letter(ray_t* v);

/* Set a column attribute via the q `#` surface (`s`/`u`/`g`/`p`, or 0 to clear).
 * The kdb u#/p# accelerator (find-hash + marker) is COMPOSED in the q layer here,
 * not baked into the frozen engine.  Borrows vec; returns an owned attributed
 * column (or a remapped q error).  Exposed for the attribute-acceleration
 * C-unit (test/q_attr_accel.c) to exercise the real q set-attribute path. */
ray_t* q_attr_set_letter(char letter, ray_t* vec);

/* q `ssr[s;p;r]` — string search-and-replace (feat/q-string-fns).  Exposed so
 * q_builtins_register can env-bind it (a triadic prefix keyword: the parser
 * name-refs `ssr[a;b;c]`, so it resolves through the env, not the registry). */
ray_t* q_ssr_wrap(ray_t** args, int64_t n);

/* q join family bracket forms (feat/q-joins-rebuild) — triadic/quaternary
 * prefix keywords env-bound by q_builtins_register (the ssr precedent):
 * ej[c;t1;t2], aj[c;t1;t2] + variants, wj/wj1[w;f;t;(q;aggs..)]. */
ray_t* q_ej_wrap(ray_t** args, int64_t n);
ray_t* q_aj_wrap(ray_t** args, int64_t n);
ray_t* q_aj0_wrap(ray_t** args, int64_t n);
ray_t* q_ajf_wrap(ray_t** args, int64_t n);
ray_t* q_ajf0_wrap(ray_t** args, int64_t n);
ray_t* q_wj_wrap(ray_t** args, int64_t n);
ray_t* q_wj1_wrap(ray_t** args, int64_t n);

/* Keyed-table lookup by a table of key rows (`y[select a,b from x]`) — the
 * keyed-table row-set lookup arm.  keytbl must carry ALL key columns by name;
 * extra columns ignored; keytbl row order preserved; miss => null row. */
ray_t* q_join_keyed_lookup_rows(ray_t* kt, ray_t* keytbl);

/* Universal table row indexing (uniform-structure-dispatch stage 0; defined
 * in ops/q_table.c).  q_table_at: t[idx] for an integer atom (-> the row
 * dict) or an integer vector (-> row gather, misses null-filled per the
 * basics/application.md out-of-bounds law); returns NULL to DECLINE any
 * other index shape.  q_table_row_at: the row-dict arm — row < 0 (incl.
 * int nulls) or >= count t yields the typed all-null row. */
ray_t* q_table_at(ray_t* t, ray_t* idx);
ray_t* q_table_row_at(ray_t* t, int64_t row);

/* q `read0 x` (feat/q-file-text) — exposed so q_builtins can ALSO env-bind it
 * for the bracket-call form `read0[(f;o)]` (the ssr/value precedent: two fn
 * objects, one implementation). */
ray_t* q_read0_wrap(ray_t* x);

/* q `hopen`/`hclose` IPC client wrappers (feat/q-ipc-client; hsym Bundle 2b).
 * Exposed so the apply module's one-shot sync request (`` `:host:port "query" ``) can
 * REUSE the exact hopen normalization + restricted gate + fd/selector handle
 * translation rather than duplicating them.  q_hopen_wrap: descriptor/port ->
 * owned int fd handle (or error, incl. clean 'nyi for deferred transports).
 * q_hclose_wrap: fd handle -> closed (no-op on a dead/console handle). */
ray_t* q_hopen_wrap(ray_t* x);
ray_t* q_hclose_wrap(ray_t* x);

/* q-name sanitization shared by .Q.id and openq construction paths that must
 * repair name clashes.  q_registry_name_sanitize returns an interned symbol id for the
 * `.Q.id` atom rule.  q_name_dedup takes an already-sanitized/generated symbol
 * and resolves reserved-word and previous-name clashes by appending 1,2,...
 * using the same table/dict column-name rule. */
int64_t q_registry_name_sanitize(int64_t sym_id);
int64_t q_name_dedup(int64_t sym_id, const int64_t* previous, int64_t n_previous,
                     int check_reserved);
ray_t* q_registry_name_reserved_words(void);

#endif /* Q_REGISTRY_H */
