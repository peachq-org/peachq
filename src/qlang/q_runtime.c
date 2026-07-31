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
#include "qlang/q_dotz.h"     /* q_dotz_init/destroy — `.z.*` resolver */
#include "qlang/q_sys.h"      /* q_sys_seed_init / q_sys_ctx_reset */
#include "qlang/q_handles.h"  /* q_handles_init/destroy — the handle registry lifecycle */
#include "qlang/q_console.h"  /* q_console_pipe_disable — reset the `\nonlegacy` display global per runtime */
#include "qlang/q_parse.h"    /* q_parse — embedded-bootstrap loader */
#include "qlang/eval/q_eval.h" /* q_eval — THE eval pipeline */
#include "qlang/eval/q_view.h" /* q_view_reset — per-runtime view state */
#include "qlang/dotq_gen.h"   /* OPENQ_BOOTSTRAP — codegen'd from src/qlang/{q,dotq}.q */
#include "qlang/h_gen.h"      /* OPENQ_H_BOOTSTRAP — codegen'd from src/qlang/h.q (`.h` constants) */
#include "qlang/j_gen.h"      /* OPENQ_J_BOOTSTRAP — codegen'd from src/qlang/j.q (`.j` JSON ns) */
#include "qlang/q_env.h"      /* q_env_init/destroy — the q K-tree lifecycle */
#include "lang/eval.h"        /* ray_eval_set_remote_* teardown */
#include <rayforce.h>
#include <stdio.h>            /* fprintf, stderr — bootstrap diagnostics */
#include <string.h>           /* memcpy, strchr, strlen — line split */

/* One q line -> owned value; NULL after a stderr report (never fatal). */
static ray_t* bootstrap_eval(const char* src) {
    ray_t* ast = q_parse(src);
    if (RAY_IS_ERR(ast)) {
        fprintf(stderr, "q bootstrap: parse error: %s\n", src);
        ray_error_free(ast);
        return NULL;
    }
    ray_t* r = q_eval(ast);
    ray_release(ast);
    if (RAY_IS_ERR(r)) {
        fprintf(stderr, "q bootstrap: eval error: %s\n", src);
        ray_error_free(r);
        return NULL;
    }
    return r;
}

/* Load one embedded bootstrap source (`src`) line at a time, post-registry
 * (rule 6), silently; blank/`/`-comment lines skipped, errors reported, never
 * fatal.  `.q.name:` lines are ORDINARY q assignments — q owns its env.
 * Called for the q.q+dotq.q bundle, then `.h` (h.q) and `.j` (j.q). */
static void bootstrap_load_src(const char* p) {
    char line[4096];
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        const char* next = nl ? nl + 1 : p + n;

        if (n >= sizeof(line)) {   /* overlong: fail LOUD, never silently split */
            fprintf(stderr, "q bootstrap: line too long (%zu bytes), skipped\n", n);
            p = next;
            continue;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        p = next;

        const char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '/')   /* blank line or leading `/` comment */
            continue;

        ray_t* r = bootstrap_eval(line);
        if (r) ray_release(r);
    }
}

ray_runtime_t* q_runtime_create(int argc, char** argv) {
    /* q owns dotted namespaces for user code, and real q has no .sys/.os/.ipc:
     * suppress the rayfall system-plumbing namespaces for this runtime, then
     * restore the default so a later ray_runtime_create (same process) is
     * unaffected — the flag governs only the next builtin registration. */
    ray_set_load_rayfall_ns(false);
    ray_runtime_t* rt = ray_runtime_create(argc, argv);
    ray_set_load_rayfall_ns(true);
    if (rt) {
        if (q_env_init() != RAY_OK) {              /* q's K-tree, before any binding */
            fprintf(stderr, "q bootstrap: q_env init failed\n");
            ray_runtime_destroy(rt);               /* nothing q-side bound yet */
            return NULL;
        }
        q_sys_ctx_reset();     /* fresh runtime starts in the root context */
        q_sys_seed_init();     /* kdb constant-seed-at-startup contract (\S) */
        q_sys_cfg_init();      /* \P/\c/\C/\g/\o/\W/\e/\s defaults per runtime */
        q_handles_init();      /* fd-keyed handle registry (file/fifo/socket open-time) */
        q_builtins_register();
        /* `.z.*` is an eval-time resolver, NOT a namespace: compute the
         * process-constant argv values once; q_eval resolves them directly
         * (no base name hook — one pipeline, cutover 2026-07-23). */
        q_dotz_init(argc, argv);
        bootstrap_load_src(OPENQ_BOOTSTRAP);  /* embedded .q stdlib, post-registry (rule 6) */
        bootstrap_load_src(OPENQ_H_BOOTSTRAP); /* `.h` constants (h.q), always-on beside dotq */
        bootstrap_load_src(OPENQ_J_BOOTSTRAP); /* `.j` JSON ns (j.q), delegates to -29!/-31! bangs */
        /* QK_QSRC manifest cells (infix q.q keywords) snapshot their `.q`
         * definitions now that the bootstrap has bound them. */
        if (q_registry_bind_qsrc() != RAY_OK)
            fprintf(stderr, "q bootstrap: qsrc registry bind failed\n");
    }
    return rt;
}

void q_runtime_destroy(ray_runtime_t* rt) {
    ray_eval_set_remote_str_fn(NULL);  /* remote strings fall back to rayfall */
    ray_eval_set_remote_apply_fn(NULL);/* (func;args) value-apply -> 'nyi w/o q runtime */
    q_handles_destroy();       /* drop handle records (open_args refs) before the env */
    q_dotz_destroy();          /* free the `.z.*` argv snapshots */
    q_registry_destroy();      /* free verb snapshots before the env goes away */
    q_eval_syms_reset();       /* cached sym ids die with this runtime's sym table */
    q_view_reset();            /* view roster + cached sym ids likewise */
    q_env_destroy();           /* release q's K-tree before the heap dies */
    q_sys_ctx_reset();         /* drop the `\d` context with its runtime */
    q_console_pipe_disable();          /* reset the `\nonlegacy` display global — the
                                * process-wide pipe mode never leaks into the next
                                * runtime (fresh-per-file doctest, wasm re-init) */
    ray_runtime_destroy(rt);
}
