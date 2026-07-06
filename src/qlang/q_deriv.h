/* q derived-verb / projection carriers — kdb `104h` (ADR 0003 Decision 2).
 *
 * Projections (`+[2]`, `f[;2]`) and monadic-marked verbs (`+:`) are first-class
 * `ray_t` VALUES — a bare arity-typed builtin cannot be stored (`g:+[2]`) nor
 * applied at the other rank.  rayfall has no native partial-application type
 * (audited), so openq carries them in a small marker-headed boxed representation.
 *
 * ADVERBS ARE NOT CARRIERS.  q's `'`(each) `/`(over) `\`(scan) ARE rayfall's
 * higher-order functions map/fold/scan (src/lang/eval.c), so no bespoke adverb
 * object exists: the op manifest (q_ops.c) maps each adverb glyph to its HOF,
 * and in 2b `+/` lowers to a PROJECTION of `fold` over `+` — i.e. a Q_DERIV_PROJ
 * of the HOF, reusing the projection carrier below.
 *
 * STAGE 2a builds the carriers and their inspectors; the PARSER DOES NOT EMIT
 * them yet (2b wires them) and NOTHING evaluates them this stage — they are
 * inert value objects with q-surface provenance {base verb, hole mask,
 * effective valence}.  2b replaces the inert box with real application
 * semantics on the DAG (reusing this shape / provenance).
 *
 * Representation: a boxed RAY_LIST whose element[0] is a QUOTED marker sym
 * (`.q.proj` / `.q.monad`) so it is self-identifying and is never mistaken for
 * an ordinary call head; element[1] is the base verb value; the remaining
 * elements are the kind-specific fields. */
#ifndef Q_DERIV_H
#define Q_DERIV_H

#include <rayforce.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    Q_DERIV_NONE = 0,
    Q_DERIV_PROJ,     /* +[2], f[;2] — bound-arg projection with hole mask */
    Q_DERIV_MONAD,    /* +: — monadic-marked verb                          */
    Q_DERIV_LAMBDA    /* {x*x} — 100h lambda: base RAY_LAMBDA + q source   */
} q_deriv_kind;

/* Projection `base[...]` with `argc` bound args (a hole is a NULL slot; the
 * `hole_mask` bit i set => arg i is an unbound hole `;`).  `eff_valence` is the
 * remaining rank (number of holes).  Retains `base` and every non-NULL arg.
 * Returns an owned carrier, or a RAY_ERROR. */
ray_t* q_proj_new(ray_t* base, ray_t** args, int64_t argc, uint64_t hole_mask,
                  int eff_valence);

/* Monadic-marked verb `base:` (e.g. `+:`).  Retains `base`. */
ray_t* q_monadic_mark(ray_t* base);

/* q lambda value `{...}`: base is the rayfall RAY_LAMBDA, `rank` its q valence,
 * `src` the verbatim `{...}` source text (the kdb display form).  Retains
 * `base` and `src`. */
ray_t* q_lambda_carrier_new(ray_t* base, int rank, ray_t* src);

/* Inspectors — return Q_DERIV_NONE / NULL / defaults for a non-carrier. */
q_deriv_kind q_deriv_kind_of(const ray_t* v);

/* Drop the lazily-interned marker sym-id cache.  Called by q_runtime_destroy:
 * the ids belong to the runtime's sym table, so a NEW runtime in the same
 * process (tests, embedders) must re-intern — otherwise values rebuilt by
 * name (serde v5 structural carrier decode) get fresh ids that can never
 * match a stale cache. */
void q_deriv_reset_markers(void);
ray_t*       q_deriv_base(const ray_t* v);        /* borrowed */
uint64_t     q_deriv_hole_mask(const ray_t* v);   /* Q_DERIV_PROJ only    */
int          q_deriv_valence(const ray_t* v);     /* effective valence    */
ray_t*       q_lambda_src(const ray_t* v);        /* Q_DERIV_LAMBDA only; borrowed */

#endif /* Q_DERIV_H */
