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
#include "qlang/q_dotz.h"     /* q_dotz_init/resolve/destroy — `.z.*` resolver */
#include "qlang/q_ns.h"       /* q_ns_reset — `\d` context state */
#include "lang/eval.h"        /* ray_eval_set_apply_hook / _name_hook */
#include <rayforce.h>

ray_runtime_t* q_runtime_create(int argc, char** argv) {
    ray_runtime_t* rt = ray_runtime_create(argc, argv);
    if (rt) {
        q_ns_reset();          /* fresh runtime starts in the root context */
        q_builtins_register();
        /* `.z.*` is an eval-time resolver, NOT a namespace: compute the
         * process-constant argv values once and install the name-load hook. */
        q_dotz_init(argc, argv);
        ray_eval_set_name_hook(q_dotz_resolve);
    }
    return rt;
}

void q_runtime_destroy(ray_runtime_t* rt) {
    ray_eval_set_apply_hook(NULL);   /* detach the q dispatcher first */
    ray_eval_set_remote_str_fn(NULL);  /* remote strings fall back to rayfall */
    ray_eval_set_name_hook(NULL);    /* detach `.z.*` before its values die */
    q_dotz_destroy();          /* free the `.z.*` argv snapshots */
    q_registry_destroy();      /* free verb snapshots before the env goes away */
    q_deriv_reset_markers();   /* marker sym-ids die with this runtime's table */
    q_ns_reset();              /* drop the `\d` context with its runtime */
    ray_runtime_destroy(rt);
}
