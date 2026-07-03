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

typedef enum { Q_MONADIC = 1, Q_DYADIC = 2 } q_valence_t;

/* Build the table once, AFTER ray_lang_init has populated g_env.  Idempotent:
 * a second call while already initialised is a no-op returning RAY_OK.  Fails
 * fast (RAY_ERR_DOMAIN) if an audited builtin source is absent or a wrapper
 * allocation fails — the roster is bound against verified registrations, so a
 * miss is a bug, not a soft skip. */
ray_err_t q_registry_init(void);

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

/* Release every retained entry and reset.  Idempotent; also serves as
 * partial-cleanup on a failed init.  Must run before ray_env_destroy. */
void q_registry_destroy(void);

#endif /* Q_REGISTRY_H */
