/* q_env — q's OWN name surface: the K-tree as NESTED DICTS
 * (basics/namespaces.md: "Namespaces are implemented as dictionaries").
 * Three directory dicts — root variables (`.`), top-level namespaces (``),
 * and a hidden bootstrap-builtin shed — each carrying the ` -> :: marker
 * whose job is to keep the value slot a general RAY_LIST: a collapsed
 * (typed-value) dict is NOT traversable, so `.foo:`p`q!7 8` makes `.foo.q`
 * a 'name — dotted access is value traversal, never name splitting.
 * Rayfall's env (src/lang/env.c) is a bootstrap-only kernel catalogue; q's
 * ONLY live seam into it is the six `.ipc.on.*` hook syms. */
#ifndef PEACHQ_Q_ENV_H
#define PEACHQ_Q_ENV_H

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

/* THE write home every q assignment form reaches — `:`/`::`, indexed and
 * modified assign, `@`/`.` name-amend, `set` — so the name policy is stated
 * once: `` `. `` restores root variables from a dict, `.z.p*`/`.z.bm` alias the
 * `.ipc.on.*` hook slots, `.z.zd` is refused, and every other name is a
 * path-copy amend of the nested dicts.  Missing ancestors conjure marked dicts
 * (`.fee.fi.fo:42` creates `.fee`, `.fee.fi`); `.ns:d` is an ordinary rebind of
 * the one name — no subtree side effects. */
ray_err_t q_env_set(int64_t sym, ray_t* val);

/* The q error class a failed q_env_set means: 'nyi and 'type where the env
 * raised them deliberately, else the assignment itself failed ('assign). */
ray_t* q_env_err(ray_err_t e);

/* Same amend, but a NEW plain name binds into the hidden builtin shed
 * (bootstrap writers stay out of the `key `.` roster). */
ray_err_t q_env_bind(int64_t sym, ray_t* val);

/* Remove a binding (dict minus one key); no-op when absent.  An emptied
 * namespace survives as its marker dict (kdb keeps it in `key `). */
ray_err_t q_env_unbind(int64_t sym);

/* Drop the binding's OWN ref to `cur` — the slot parks at `::` — so a writer
 * already holding `cur` becomes its sole owner and its amend mutates in place
 * (ref/amend.md handle d).  Correctness never depends on it: rc decides at
 * store time, so an alias still forces the copy.  Refuses unless the name is
 * bound to exactly `cur` — a shadowed or walked read is not the slot the
 * write rebinds.  1 = parked, and the caller MUST put a value back:
 * q_env_set on success, q_env_bind to restore on error. */
int q_env_take(int64_t sym, ray_t* cur);

int    q_env_ns_exists(int64_t path_sym);
/* The stored namespace dict itself (marker included), retained for the
 * caller.  OWNED; NULL if the name is not bound to a dict. */
ray_t* q_env_ns_view(int64_t path_sym);

/* `<ns>.<member>` as one sym; ns 0 or `` `. `` leaves the member bare.  The
 * K-tree's name-composition rule, for callers holding a namespace by name. */
int64_t q_env_qualify(int64_t ns_sym, int64_t member_sym);

/* A namespace's own members by value KIND (`\v` `\f` `\a`, `tables`) — sorted,
 * never recursive; ns_sym 0 or `` `. `` is the root.  OWNED sym vector, or NULL
 * when ns_sym names no namespace. */
typedef enum { Q_ENV_NS_VARS, Q_ENV_NS_FNS, Q_ENV_NS_TABLES } q_env_ns_kind_t;
ray_t* q_env_ns_names(int64_t ns_sym, q_env_ns_kind_t kind);

/* qsort comparator over int64 sym ids by NAME bytes (the `\v`/`\f`/`\a`
 * ordering) — name ordering is env knowledge, shared with `\b`. */
int q_env_name_cmp(const void* a, const void* b);

/* The symbol that marks a dictionary as a NAMESPACE.  env's representation, so
 * env owns it: `key` strips it from the root (ops/q_key.c) and kdb prints it
 * for every other namespace (ref/key.md `key `.` beside `key `.q`). */
int64_t q_env_marker_sym(void);

/* `key ``: the root's namespaces in creation order, `.z` excluded
 * (basics/syscmds.md §`\d`).  OWNED sym vector. */
ray_t* q_env_ns_roster(void);

/* `\d` current context (0 = root).  A RELATIVE name resolves in the context
 * first and falls back to the root; an assignment lands IN the context, which
 * is what CREATES it — `\d .s` alone does not (syscmds.md §`\d`: "a new
 * namespace is created when an object is defined in it"). */
void    q_env_ctx_set(int64_t ns_sym);
int64_t q_env_ctx(void);

/* Local frames: one flat frame per call, no parent chain (q has no closures —
 * function-notation.md).  A BARRIER frame hides everything below it; frames
 * pushed above a barrier (query column scopes) still see it. */
ray_err_t q_env_frame_push(int barrier);
void      q_env_frame_pop(void);
ray_err_t q_env_local_set(int64_t sym, ray_t* val);
ray_t*    q_env_local_get(int64_t sym);   /* TOP frame only; borrowed */
int       q_env_local_take(int64_t sym, ray_t* cur);  /* q_env_take, TOP frame;
                                                       * q_env_local_set puts back */
int32_t   q_env_frame_depth(void);

/* Debugger frame cursor (basics/debug.md navigation): ROOT the resolving walk at
 * frame `depth`; _OFF = the top frame, _NONE = no frame at all (console [0]).  A
 * frame push saves and clears it, so a call made from the debug prompt resolves
 * in its own scope.  READS only — q_env_local_set stays on the top frame. */
#define Q_ENV_FRAME_VIEW_OFF  (-1)
#define Q_ENV_FRAME_VIEW_NONE (-2)
int32_t   q_env_frame_view(int32_t depth);

#endif /* PEACHQ_Q_ENV_H */
