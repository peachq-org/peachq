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

/* True iff y is a keyed table: a RAY_DICT whose keys AND values are both
 * tables (the wave-4 shape shared by the table verbs and q_builtins). */
int q_is_keyed_table(ray_t* y);

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

/* q `value` — the full form matrix (ref/value.md): dict->vals, symbol->variable,
 * string->eval, list->single apply/index, projection->(fn;args), derived->iterator
 * arg, operator->opcode, lambda->structure.  Exposed so q_builtins can ALSO env-
 * bind it (like string/meta/type): the parser embeds the registry copy at `value`
 * application heads, but a bare `value` used as a HOF operand (`value each …`) is
 * env-resolved, and without this override it would hit rayfall's dict-only native
 * `value`.  Both bindings call this one function (single home, rule 4). */
ray_t* q_value_wrap(ray_t* x);

/* Context-aware symbol resolution (q namespaces): `. / `.foo synthesize the
 * root/context dict views; `..name root-qualifies; a plain name resolves in
 * the current `\d` context first.  Returns an OWNED ref or NULL when the
 * name doesn't resolve.  Used by q_apply's symbol-handle arm. */
ray_t* q_value_resolve_owned(ray_t* symv);

/* Release every retained entry and reset.  Idempotent; also serves as
 * partial-cleanup on a failed init.  Must run before ray_env_destroy. */
void q_registry_destroy(void);

/* The internal scan value (rayfall scan + collapse) q_lower embeds for the
 * `\` adverb.  Borrowed; NULL before q_registry_init.  Not a roster row —
 * it has no q spelling of its own. */
ray_t* q_registry_scan_value(void);

/* Internal (spelling-less) HOF values q_lower embeds at adverb heads (wave-2):
 *   over      — `/` : reduce / converge / do / while
 *   each-both — `'` dyadic: zip a binary over conforming operands
 *   each-prior— `':`: apply a binary between each item and its predecessor
 * Borrowed; NULL before q_registry_init. */
ray_t* q_registry_over_value(void);
ray_t* q_registry_eachboth_value(void);
ray_t* q_registry_prior_value(void);

/* Binary helper (map-right/map-left-value, f) -> a 2-hole derived-verb carrier,
 * built at EVAL time so a lambda operand is a real value.  q_lower embeds it to
 * lower `(f/:)` / `(f\:)` as VALUES (stacked-adverb `f/:\:`).  Borrowed. */
ray_t* q_registry_mkderiv2_value(void);

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

/* The internal keyed-table constructor `([k:…] v:…)` head the PARSER embeds.
 * args[0] is the key-column count; the columns follow (keys then values).
 * Builds a RAY_DICT (key-cols table -> value-cols table).  Borrowed; NULL
 * before q_registry_init. */
ray_t* q_registry_keyed_table_value(void);

/* The q.fn special-form value behind every lambda literal (borrowed; NULL
 * before init).  q_lower embeds it at the head of lowered `{...}` nodes. */
ray_t* q_registry_lambda_value(void);

/* `:x` early return and `'x` signal (borrowed; NULL before init).  q_lower
 * embeds them at the head of the parser's .q.ret / .q.sig statement nodes. */
ray_t* q_registry_ret_value(void);
ray_t* q_registry_sig_value(void);

/* q imperative control constructs (borrowed; NULL before init).  q_lower
 * embeds these special-form values at the head of the parser's `;` statement
 * sequence and its `if` / `do` / `while` control-word bracket applications.
 * They receive their statement args UNEVALUATED (lazy, left-to-right, side
 * effects persist, no lexical scope). */
ray_t* q_registry_seq_value(void);
ray_t* q_registry_if_value(void);
ray_t* q_registry_do_value(void);
ray_t* q_registry_while_value(void);

/* Take (and clear) the thread-local early-return payload stashed by a `:x`
 * statement — called by q_lambda_apply when call_lambda comes back with the
 * reserved "q.ret" error class.  Returns an OWNED value or NULL. */
ray_t* q_lambda_ret_take(void);

/* Take (and clear) / clear the thread-local full-text payload stashed by a
 * `'x` signal — Trap hands the handler the whole message, not the ≤7-char
 * error class.  q_registry_sig_take returns an OWNED string or NULL. */
ray_t* q_registry_sig_take(void);
void   q_registry_sig_clear(void);

/* The qSQL SELECT adapter value q_lower embeds when it lowers the functional
 * 5-list (?;`t;c;b;a) onto the base ray_select engine.  Special form; its two
 * operands are the rayfall query dict and the by-key column-name sym vector.
 * Borrowed; NULL before q_registry_init. */
ray_t* q_registry_select_value(void);

/* The `'[f;g;…]` compose builder the PARSER embeds at the head of a compose
 * bracket form.  A regular (arg-evaluating) vary fn: ray_eval resolves the
 * function operands, then it boxes them into a Q_DERIV_COMPOSE carrier.
 * Borrowed; NULL before q_registry_init. */
ray_t* q_registry_compose_value(void);

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

/* q `ssr[s;p;r]` — string search-and-replace (feat/q-string-fns).  Exposed so
 * q_builtins_register can env-bind it (a triadic prefix keyword: the parser
 * name-refs `ssr[a;b;c]`, so it resolves through the env, not the registry). */
ray_t* q_ssr_wrap(ray_t** args, int64_t n);

/* ===== q cast home ==========================================================
 * THE q-layer conversion entry points (reuse mandate): future q-semantics
 * work that needs a conversion (bool-widening in arithmetic, promotion in
 * mixed-type ops, ...) MUST call q_cast_to with a rayfall type tag rather
 * than growing its own conversion — the only sanctioned exception is a
 * profiled hot path with a specialized SIMD/vectorized kernel (base arith.c's
 * as_i64/as_f64 atom coercions are that carve-out).
 *
 * q_cast_designator: resolve a q cast/Tok designator value to a rayfall type
 *   tag.  Accepts a sym name (`long`float`int`short`boolean`real`symbol), a
 *   single-char string ("j" "f" "i" "h" "b" "e" "s"; upper-case = Tok), or a
 *   short atom holding the kdb type number (positive = cast, negative = Tok
 *   per ref/tok.md — the rayfall tags already equal the kdb numbers).  The
 *   null symbol ` is Tok "S".  *is_tok is set to 1 for Tok (string-parse)
 *   designators.  Returns 0 (RAY_LIST — never a valid target) for unknown or
 *   deferred designators (byte/guid/char/temporals, and the 0h/"*" identity).
 * q_cast_to: convert x to the tag type with q semantics.  Exactly: RAY_LIST
 *   distributes per element (collapsed); float ATOMS and float VECTORS
 *   pre-round via rint (kdb ties-even) for integer targets; symbol target is
 *   identity on syms and 'nyi otherwise; every other input delegates to base
 *   ray_cast_fn (typed vectors included).  Returns owned; 'nyi error for
 *   deferred targets (F32/char/byte/guid/temporals).
 * q_tok_to: parse string atom(s) to the tag type (kdb Tok, ref/tok.md).
 *   Leading/trailing blanks are trimmed; unparseable or out-of-range parses
 *   yield the TYPED NULL (never an error); lists and string vectors
 *   distribute (implicit recursion stops at strings, not atoms). */
int8_t q_cast_designator(ray_t* t, int* is_tok);
ray_t* q_cast_to(int8_t tag, ray_t* x);
ray_t* q_tok_to(int8_t tag, ray_t* x);

/* ===== q calendar home (date) ==============================================
 * kdb date == base RAY_DATE payload: i32 days since 2000.01.01, proleptic
 * Gregorian (qdocs basics/datatypes.md).  q_days_from_civil is Hinnant's
 * days_from_civil (public domain) rebased from the unix epoch by -10957;
 * q_date_valid pins the kdb literal domain 0001.01.01..9999.12.31 with real
 * (leap-aware) month lengths.  Shared by the literal scanner (q_parse) and
 * "D"$ Tok (q_tok_to) — ONE conversion home, per the reuse mandate above. */
int64_t q_days_from_civil(int64_t y, int64_t m, int64_t d);
int     q_date_valid(int64_t y, int64_t m, int64_t d);

/* Timestamp payload composition: days*NS_PER_DAY + tod_ns, computed EXACTLY
 * (__int128).  q_ts_compose_checked returns 1 and fills *out when the exact
 * value lies in [-INT64_MAX, INT64_MAX] ("P"$ Tok maps failure to 0Np, the
 * ref/tok.md out-of-domain contract); q_ts_compose is the saturating form
 * for the literal scanner and `timestamp$ casts (out-of-range clamps to the
 * kdb inf sentinels +-INT64_MAX == +-0Wp: datatypes.md pins
 * `timestamp$1666.09.02 -> -0Wp).  The mul must NOT saturate before the add:
 * the doc-pinned minimum 1707.09.22D00:12:43.145224194 has a day component
 * that alone underflows i64 and re-enters range via the time of day. */
int     q_ts_compose_checked(int64_t days, int64_t tod_ns, int64_t* out);
int64_t q_ts_compose(int64_t days, int64_t tod_ns);

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
