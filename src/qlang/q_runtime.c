/* q_runtime — the single q environment factory: a rayforce runtime plus q's
 * own builtin bindings.  Every q entry point (binary, doctest, tests) creates
 * its runtime through here so `parse` (and future q verbs) resolve uniformly.
 *
 * The factory owns the q op registry lifecycle symmetrically: q_runtime_create
 * builds it (via q_builtins_register); q_runtime_destroy releases it BEFORE
 * tearing down the runtime, because ray_runtime_destroy -> ray_lang_destroy ->
 * ray_env_destroy releases the builtin values the registry snapshotted.  Every
 * q consumer MUST tear down through q_runtime_destroy, never ray_runtime_destroy
 * directly, or the registry's retained verb snapshots would outlive the env. */
#include "qlang/q_runtime.h"
#include "qlang/q_builtins.h"
#include "qlang/q_registry.h"
#include "qlang/q_deriv.h"    /* q_deriv_reset_markers — per-runtime sym-id cache */
#include "lang/eval.h"        /* ray_eval_set_apply_hook */
#include <rayforce.h>

ray_runtime_t* q_runtime_create(int argc, char** argv) {
    ray_runtime_t* rt = ray_runtime_create(argc, argv);
    if (rt) q_builtins_register();
    return rt;
}

void q_runtime_destroy(ray_runtime_t* rt) {
    ray_eval_set_apply_hook(NULL);   /* detach the q dispatcher first */
    q_registry_destroy();      /* free verb snapshots before the env goes away */
    q_deriv_reset_markers();   /* marker sym-ids die with this runtime's table */
    ray_runtime_destroy(rt);
}
