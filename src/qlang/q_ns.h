/* q_ns — q namespaces/contexts: the `\d` current-context state, the console
 * prompt, `\d`/`\v`/`\f`/`\a` system commands, and the directory<->dictionary
 * view synthesis (`get `.` / `get `.foo` / `key `` ` ``).
 *
 * Storage is the ENGINE's: env_set_dotted (src/lang/env.c) already keeps a
 * user context as a nested RAY_DICT under the root env key `.foo`; this module
 * only reads those dicts and owns the q-level presentation (the `::`
 * placeholder entry, the root roster, the prompt) plus the `\d` state that the
 * q_lower qualification pass consults.  See the 2026-07-08 namespaces spec. */
#ifndef Q_NS_H
#define Q_NS_H

#include <rayforce.h>
#include <stddef.h>

/* Reset to the root context (called by q_runtime_create/destroy). */
void q_ns_reset(void);

/* Current context: "" at root, else the dotted name (".jab").  Never NULL. */
const char* q_ns_current(void);

/* Switch context: name is "." (root) or ".ident" (ONE level below root —
 * kdb's limitation; deeper paths error with the offending name as the error
 * class, e.g. '.jab.util).  Returns NULL on success or an OWNED RAY_ERROR. */
ray_t* q_ns_switch(const char* name, size_t len);

/* Write the console prompt ("q)" at root, "q.foo)" in a context) into buf;
 * returns its length. */
int q_ns_prompt(char* buf, size_t cap);

/* True iff `s` must NOT be context-qualified by the q_lower pass: q manifest
 * reserved words (q_ops_is_reserved), `.q`-hosted keywords (q.q bootstrap),
 * control/qSQL words, or a BUILTIN env binding (q keywords implemented as env
 * fns — parse/type/not/…).  User globals never match. */
int q_ns_is_unqualifiable(const char* s, size_t len);

/* `.q.<name>` binding for a BARE identifier (kdb: keywords are `.q` entries).
 * BORROWED, or NULL when dotted/oversized/unbound; non-NULL = q.q keyword. */
ray_t* q_ns_dotq_get(const char* name, size_t len);

/* ---- directory <-> dictionary views (all results OWNED) ---- */

/* Root context as a dict: user, non-dotted env bindings in insertion order.
 * NO `::` placeholder (key.md / q4m3 pin root without it). */
ray_t* q_ns_root_dict(void);

/* Context `.foo` (or NESTED `.fee.fi`) as a dict: leading `` ` ``->`::`
 * placeholder entry, then the members.  Seed contexts (q/Q/o/h) that have no
 * bindings yet synthesize as placeholder-only.  Returns NULL if `dotname`
 * names no context. */
ray_t* q_ns_ctx_dict(const char* dotname, size_t len);

/* True iff `dotname` CURRENTLY names a context: a leading-dot (non-`..`)
 * path resolving to a sym-keyed namespace dict, any depth. */
int q_ns_is_context(const char* dotname, size_t len);

/* `key `` ` `` roster: the seeds `q`Q`o`h then user-created root contexts in
 * env insertion order (RAY_SYM vector). */
ray_t* q_ns_key_roster(void);

/* List members for the \v/\f/\a `\`-commands (dispatched by q_sys.c).  cmd is
 * 'v' (vars) | 'f' (functions) | 'a' (tables); arg/alen is the tokenized
 * namespace argument (empty = current context).  Returns an OWNED RAY_SYM
 * vector, or an OWNED RAY_ERROR naming a missing namespace. */
ray_t* q_ns_list(char cmd, const char* arg, size_t alen);

#endif /* Q_NS_H */
