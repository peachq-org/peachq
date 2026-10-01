/* q_wasm — the C ABI of peachq's WebAssembly build: create the runtime, run q text.
 * The host reads each answer from the module's print/printErr (stdout/stderr).
 * wasm/engine.js is the one caller, and wasm/smoke.js runs its .qcmd ledger through q_wasm_qcmd. */
#include "qlang/q_runtime.h"
#include "qlang/q_ctx.h"
#include "qlang/q_console.h"      /* pipe-table display + the console clip */
#include "qlang/q_pq.h"           /* q_pq_load — the embedded stdlib bundle */
#include <rayforce.h>
#include <stdio.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE      /* the gate syntax-checks this file with the native compiler */
#endif

static ray_runtime_t* g_rt = NULL;

/* Idempotent.  0 ok, 1 failure.  The pool has no workers, as q without -s: a
 * wasm module is single-threaded (platform.c answers ray_thread_create with 'nyi). */
EMSCRIPTEN_KEEPALIVE
int q_wasm_init(void) {
    if (g_rt)
        return 0;
    g_rt = q_runtime_create(0, NULL);
    if (!g_rt)
        return 1;
    /* an argv-less ./q: modern display and the stdlib; no terminal to size, so a fixed \c */
    q_console_pipe_enable();
    q_console_clip_set(25, 120);
    q_pq_load();
    return 0;
}

/* Text as `value`/an IPC request reads it, in the console's session (`\d` sticks), answered as the console
 * displays it: the web never reaches the console's one-line door. */
EMSCRIPTEN_KEEPALIVE
void q_wasm_eval(const char* src) {
    if (!src || q_wasm_init() != 0)
        return;
    q_ctx_run_value_src(src, strlen(src), stdout, stderr);
    fflush(stderr);
}

/* A .qcmd transcript replayed in this session through the console's transcript door —
 * what `q f.qcmd` runs — answered on stdout/stderr like q_wasm_eval; the host scores
 * the rows (wasm/engine.js).  0 on a full run, 1 when the load aborted. */
EMSCRIPTEN_KEEPALIVE
int q_wasm_qcmd(const char* path) {
    if (!path || q_wasm_init() != 0)
        return 1;
    ray_t* esig = NULL;
    int    rc   = q_ctx_run_file(path, stdout, stderr, &esig);
    if (esig)
        ray_release(esig);
    fflush(stdout);
    fflush(stderr);
    return rc != 0;
}
