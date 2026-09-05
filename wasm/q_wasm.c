/* q_wasm — the browser/JS entry points for peachq's WebAssembly build.
 *
 * Exposes a tiny, stable C ABI that drives peachq's real q pipeline
 * (q_eval_statement -> materialize -> q_fmt), the same sequence
 * src/qlang/q_ctx.c:ctx_line uses for every native door.
 * Compiled only by Makefile.wasm with emcc; never part of the native build. */
#define _POSIX_C_SOURCE 200809L   /* expose strdup (string.h) + setenv (stdlib.h) */
#include "qlang/q_runtime.h"
#include "qlang/q_fmt.h"
#include "qlang/q_console.h"  /* pipe-table display, clip, and the side-effect drain */
#include "qlang/q_pq.h"       /* q_pq_load — the embedded stdlib bundle */
#include "qlang/eval/q_eval.h"   /* q_eval_statement — THE statement home */
#include "ops/ops.h"      /* ray_is_lazy, ray_lazy_materialize */
#include <rayforce.h>
#include <stdio.h>        /* snprintf */
#include <stdlib.h>       /* free, setenv */
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
    if (g_rt) {
        /* Modern mode, same as an argv-less `./q`: pipe-table display + the
         * embedded stdlib.  No argv in a browser, so no `-classic` opt-out;
         * `\classic 1` re-toggles the display at runtime.  No terminal to
         * query either, so no `\c 25 0N` auto width — a fixed 25 120. */
        q_console_pipe_enable();
        q_console_clip_set(25, 120);
        q_pq_load();
    }
    return g_rt ? 0 : 1;
}

/* Evaluate one line of q source; return a freshly malloc'd, NUL-terminated
 * formatted result the caller must release with q_wasm_free. Errors are
 * returned as human-readable strings ("error: <class>", a parse failure being
 * "error: parse"), never as a null pointer, so the JS side always has something
 * to print. */
EMSCRIPTEN_KEEPALIVE
char* q_wasm_eval(const char* src) {
    if (!g_rt && q_wasm_init() != 0)
        return strdup("error: runtime init failed");
    if (!src)
        return strdup("");

    /* `\`-commands (\c, \classic, \h, ...) need no door of their own: q_parse
     * turns a leading `\X` into the `system "X"` tree, so they ride the same
     * pipeline as any other line.  `\\` stays a silent no-op (nothing to quit
     * in a browser — the process capability is never enabled), and an unknown
     * `\token` now answers whatever `system "token"` answers in a tab, which
     * is what the owner ruling asked for: one path, not two. */
    ray_t* r = q_eval_statement(src, NULL);   /* THE statement home ctx_line uses */
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
    buf[0] = '\0';
    /* side effects (show / 0N! / `\h`'s doc lines) come first, then the value —
     * q_ctx.c ctx_line's display order */
    { const char* con = q_console_str();
      if (con && *con) snprintf(buf, sizeof buf, "%s", con);
      q_console_reset(); }
    /* q console silence: the generic null prints nothing, and that is what an
     * assignment statement answers — mirrors q_ctx.c ctx_line. */
    size_t used = strlen(buf);
    if (!RAY_IS_NULL(r))
        q_fmt_console(r, buf + used, sizeof buf - used);   /* obey \c / \classic on auto-echo */
    ray_release(r);
    return strdup(buf);
}

/* Release a string returned by q_wasm_eval. */
EMSCRIPTEN_KEEPALIVE
void q_wasm_free(char* p) {
    free(p);
}
