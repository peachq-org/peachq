/* q derived-verb / projection carriers — kdb `104h` (ADR 0003 Decision 2).
 *
 * Projections (`+[2]`, `f[;2]`), adverb-derivatives (`+/ +\ each`) and
 * monadic-marked verbs (`+:`) are first-class `ray_t` VALUES — a bare
 * arity-typed builtin cannot be stored (`g:+[2]`) nor applied at the other
 * rank.  rayfall has no native partial-application / composition type (audited),
 * so openq carries them in a small marker-headed boxed representation.
 *
 * STAGE 2a builds the carriers and their inspectors; the PARSER DOES NOT EMIT
 * them yet (2b wires them) and NOTHING evaluates them this stage — they are
 * inert value objects with q-surface provenance {base verb, adverb / hole /
 * marker kind, effective valence}.  2b replaces the inert box with real
 * application semantics on the DAG (reusing this shape / provenance).
 *
 * Representation: a boxed RAY_LIST whose element[0] is a QUOTED marker sym
 * (`.q.proj` / `.q.adverb` / `.q.monad`) so it is self-identifying and is never
 * mistaken for an ordinary call head; element[1] is the base verb value; the
 * remaining elements are the kind-specific fields. */
#ifndef Q_DERIV_H
#define Q_DERIV_H

#include <rayforce.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    Q_DERIV_NONE = 0,
    Q_DERIV_PROJ,     /* +[2], f[;2] — bound-arg projection with hole mask */
    Q_DERIV_ADVERB,   /* +/ +\ ' — adverb-derivative                       */
    Q_DERIV_MONAD     /* +: — monadic-marked verb                          */
} q_deriv_kind;

/* Adverb code carried by a Q_DERIV_ADVERB carrier (kdb glyphs). */
typedef enum {
    Q_ADV_EACH = 1,   /* '  */
    Q_ADV_OVER,       /* /  */
    Q_ADV_SCAN,       /* \  */
    Q_ADV_EACH_PRIOR, /* ': */
    Q_ADV_EACH_RIGHT, /* /: */
    Q_ADV_EACH_LEFT   /* \: */
} q_adverb_t;

/* Projection `base[...]` with `argc` bound args (a hole is a NULL slot; the
 * `hole_mask` bit i set => arg i is an unbound hole `;`).  `eff_valence` is the
 * remaining rank (number of holes).  Retains `base` and every non-NULL arg.
 * Returns an owned carrier, or a RAY_ERROR. */
ray_t* q_proj_new(ray_t* base, ray_t** args, int64_t argc, uint64_t hole_mask,
                  int eff_valence);

/* Adverb-derivative `base<adverb>` (e.g. `+/`).  Retains `base`. */
ray_t* q_adverb_new(ray_t* base, q_adverb_t adverb);

/* Monadic-marked verb `base:` (e.g. `+:`).  Retains `base`. */
ray_t* q_monadic_mark(ray_t* base);

/* Inspectors — return Q_DERIV_NONE / NULL / defaults for a non-carrier. */
q_deriv_kind q_deriv_kind_of(const ray_t* v);
ray_t*       q_deriv_base(const ray_t* v);        /* borrowed */
q_adverb_t   q_deriv_adverb(const ray_t* v);      /* Q_DERIV_ADVERB only */
uint64_t     q_deriv_hole_mask(const ray_t* v);   /* Q_DERIV_PROJ only    */
int          q_deriv_valence(const ray_t* v);     /* effective valence    */

#endif /* Q_DERIV_H */
