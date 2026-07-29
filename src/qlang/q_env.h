/* q_env — q's OWN name surface: the K-tree as NESTED DICTS
 * (basics/namespaces.md: "Namespaces are implemented as dictionaries").
 * Three directory dicts — root variables (`.`), top-level namespaces (``),
 * and a hidden bootstrap-builtin shed — each carrying the ` -> :: marker
 * whose job is to keep the value slot a general RAY_LIST: a collapsed
 * (typed-value) dict is NOT traversable, so `.foo:`p`q!7 8` makes `.foo.q`
 * a 'name — dotted access is value traversal, never name splitting.
 * Rayfall's env (src/lang/env.c) is a bootstrap-only kernel catalogue; q's
 * ONLY live seam into it is the six `.ipc.on.*` hook syms. */
#ifndef OPENQ_Q_ENV_H
#define OPENQ_Q_ENV_H

#include <rayforce.h>

ray_err_t q_env_init(void);
void      q_env_destroy(void);

/* Borrowed ref or NULL: the pure dict chain (`.a.b.c` = successive general-
 * dict probes; a namespace name yields its stored dict; `.` the root). */
ray_t* q_env_get(int64_t sym);

/* Full resolution — locals (frame stack, barrier-aware), then the dict walk
 * plus the non-dict segment steps: table/link deref and the temporal
 * accessor roster.  OWNED result (or an owned error), NULL when undefined. */
ray_t* q_env_resolve(int64_t sym);

/* User assign: path-copy amend of the nested dicts.  Missing ancestors
 * conjure marked dicts (`.fee.fi.fo:42` creates `.fee`, `.fee.fi`); `.ns:d`
 * is an ordinary rebind of the one name — no subtree side effects. */
ray_err_t q_env_set(int64_t sym, ray_t* val);

/* Same amend, but a NEW plain name binds into the hidden builtin shed
 * (bootstrap writers stay out of the `key `.` roster). */
ray_err_t q_env_bind(int64_t sym, ray_t* val);

/* Remove a binding (dict minus one key); no-op when absent.  An emptied
 * namespace survives as its marker dict (kdb keeps it in `key `). */
ray_err_t q_env_unbind(int64_t sym);

int    q_env_ns_exists(int64_t path_sym);
/* The stored namespace dict itself (marker included), retained for the
 * caller.  OWNED; NULL if the name is not bound to a dict. */
ray_t* q_env_ns_view(int64_t path_sym);

/* `\d` current-context pointer — STORED only; stage C wires it into
 * resolution.  0 = root. */
void    q_env_ctx_set(int64_t ns_sym);
int64_t q_env_ctx(void);

/* Local frames: one flat frame per call, no parent chain (q has no closures —
 * function-notation.md).  A BARRIER frame hides everything below it; frames
 * pushed above a barrier (query column scopes) still see it. */
ray_err_t q_env_frame_push(int barrier);
void      q_env_frame_pop(void);
ray_err_t q_env_local_set(int64_t sym, ray_t* val);
ray_t*    q_env_local_get(int64_t sym);   /* TOP frame only; borrowed */

#endif /* OPENQ_Q_ENV_H */
