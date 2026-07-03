#ifndef Q_RUNTIME_H
#define Q_RUNTIME_H
#include <rayforce.h>
/* Create a rayforce runtime with q's builtins registered.  Use everywhere a q
 * environment is needed (binary, REPL, doctest, tests) so `parse` etc. resolve. */
ray_runtime_t* q_runtime_create(int argc, char** argv);

/* Destroy a q runtime.  INVARIANT: every q consumer MUST tear down through
 * here (never ray_runtime_destroy directly) — this releases the q op registry's
 * immutable verb snapshots BEFORE ray_env_destroy frees the underlying builtin
 * values.  Calling ray_runtime_destroy on a q runtime would leave the registry
 * holding refs to freed builtins.  (A CI grep-guard enforcing this is a
 * deferred follow-up; today nothing in the live pipeline consults the registry,
 * so the blast radius is nil.) */
void q_runtime_destroy(ray_runtime_t* rt);
#endif /* Q_RUNTIME_H */
