/* q_runtime — the single q environment factory: a rayforce runtime plus q's
 * own builtin bindings.  Every q entry point (binary, doctest, tests) creates
 * its runtime through here so `parse` (and future q verbs) resolve uniformly. */
#include "qlang/q_runtime.h"
#include "qlang/q_builtins.h"
#include <rayforce.h>

ray_runtime_t* q_runtime_create(int argc, char** argv) {
    ray_runtime_t* rt = ray_runtime_create(argc, argv);
    if (rt) q_builtins_register();
    return rt;
}
