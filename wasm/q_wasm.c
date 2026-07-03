/* q_wasm — the browser/JS entry points for openq's WebAssembly build.
 *
 * Exposes a tiny, stable C ABI that drives openq's real q pipeline
 * (q_parse -> q_resolve_verbs -> ray_eval -> materialize -> q_fmt), the
 * same sequence src/qlang/q_repl.c:run_one_line uses for the native REPL.
 * Compiled only by Makefile.wasm with emcc; never part of the native build. */
#define _POSIX_C_SOURCE 200809L   /* expose strdup (string.h) + setenv (stdlib.h) */
#include "qlang/q_runtime.h"
#include "qlang/q_parse.h"
#include "qlang/q_fmt.h"
#include "lang/eval.h"    /* ray_eval */
#include "ops/ops.h"      /* ray_is_lazy, ray_lazy_materialize */
#include <rayforce.h>
#include <stdio.h>        /* snprintf */
#include <stdlib.h>       /* malloc, free, setenv */
#include <string.h>       /* strdup */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

static ray_runtime_t* g_rt = NULL;

/* Create the q runtime once (idempotent). Returns 0 on success, 1 on failure.
 *
 * WASM is single-threaded by construction (src/core/platform.c stubs
 * ray_thread_create -> RAY_ERR_NYI). We force RAYFORCE_CORES=0 before creating
 * the runtime so no code path ever asks the pool to spawn workers — the default
 * (unset) already auto-sizes to zero workers on WASM, but pinning it makes the
 * single-threaded contract explicit and robust against a stray env value. */
EMSCRIPTEN_KEEPALIVE
int q_wasm_init(void) {
    if (g_rt)
        return 0;
    setenv("RAYFORCE_CORES", "0", 1);
    g_rt = q_runtime_create(0, NULL);
    return g_rt ? 0 : 1;
}

/* Evaluate one line of q source; return a freshly malloc'd, NUL-terminated
 * formatted result the caller must release with q_wasm_free. Errors are
 * returned as human-readable strings ("parse error", "error: <code>"), never
 * as a null pointer, so the JS side always has something to print. */
EMSCRIPTEN_KEEPALIVE
char* q_wasm_eval(const char* src) {
    if (!g_rt && q_wasm_init() != 0)
        return strdup("error: runtime init failed");
    if (!src)
        return strdup("");

    ray_t* ast = q_parse(src);
    if (RAY_IS_ERR(ast))
        return strdup("parse error");

    ast = q_resolve_verbs(ast);
    ray_t* r = ray_eval(ast);
    ray_release(ast);
    if (ray_is_lazy(r))
        r = ray_lazy_materialize(r);

    if (RAY_IS_ERR(r)) {
        const char* code = (const char*)r->sdata;
        char buf[128];
        snprintf(buf, sizeof buf, "error: %s", (code && *code) ? code : "eval");
        ray_release(r);
        return strdup(buf);
    }

    char buf[8192];
    if (!RAY_IS_NULL(r))
        q_fmt(r, buf, sizeof buf);
    else
        buf[0] = '\0';
    ray_release(r);
    return strdup(buf);
}

/* Release a string returned by q_wasm_eval. */
EMSCRIPTEN_KEEPALIVE
void q_wasm_free(char* p) {
    free(p);
}
