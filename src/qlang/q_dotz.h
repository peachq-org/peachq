/* q_dotz — the eval-time `.z.*` command-line resolver.
 *
 * `.z` is NOT a namespace or a `.Q`-style dict: there is no `.z` object to
 * enumerate.  Each `.z.<name>` is a name the EVALUATOR fills with a computed
 * value when a tree references it — the same way q fills `.z.p`.  This module
 * computes the process-constant argv values once at boot (`.z.f` script file,
 * `.z.x` args after the script, `.z.X` full raw argv) and exposes a resolver
 * installed as the engine's name-load hook (ray_eval_set_name_hook); it fires
 * only on an env-resolution MISS, before `'name`.
 *
 * Lifecycle is owned by q_runtime_create / q_runtime_destroy. */
#ifndef Q_DOTZ_H
#define Q_DOTZ_H

#include <rayforce.h>

/* Parse argv once and cache the `.z.*` values.  Requires an initialised sym
 * table (call after ray_runtime_create).  Idempotent per runtime; a second
 * call frees the previous values first. */
void q_dotz_init(int argc, char** argv);

/* The name-load hook: given a sym_id, return an OWNED value for `.z.f`/`.z.x`/
 * `.z.X`, or NULL to decline.  Installed via ray_eval_set_name_hook. */
ray_t* q_dotz_resolve(int64_t sym_id);

/* The positional `*.q` startup script from argv, or NULL if none — qmain uses
 * this to decide whether to run a script.  Points into argv (process-lifetime). */
const char* q_dotz_script_path(void);

/* Release the cached `.z.*` values. */
void q_dotz_destroy(void);

#endif /* Q_DOTZ_H */
