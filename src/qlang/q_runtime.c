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
#include "qlang/eval/q_dbg.h" /* q_dbg_reset — snapshot refs die with the runtime */
#include "qlang/ops/q_sys.h"      /* q_sys_seed_init / q_sys_ctx_reset */
#include "qlang/io/q_handles.h"  /* q_handles_init/destroy — the handle registry lifecycle */
#include "qlang/io/q_provider.h"   /* q_provider_init/destroy — the provider registry lifecycle */
#include "qlang/io/q_splay.h"    /* q_splay_init/destroy — the splay registry lifecycle */
#include "qlang/io/q_duckdb.h"   /* q_duckdb_reset — close handles at teardown */
#include "qlang/io/q_re2.h"      /* q_re2_reset — drop compiled patterns at teardown */
#include "qlang/q_console.h"  /* q_console_pipe_disable — reset the `\classic` display global per runtime */
#include "qlang/q_ctx.h"      /* remote-door install + q_ctx_run_src (the bootstrap loader) */
#include "qlang/eval/q_eval.h" /* q_eval_syms_reset — per-runtime teardown */
#include "qlang/eval/q_view.h" /* q_view_reset — per-runtime view state */
#include "qlang/dotq_gen.h"   /* PEACHQ_BOOTSTRAP — codegen'd from src/qlang/{q,dotq}.q */
#include "qlang/h_gen.h"      /* PEACHQ_H_BOOTSTRAP — codegen'd from src/qlang/h.q (`.h` constants) */
#include "qlang/j_gen.h"      /* PEACHQ_J_BOOTSTRAP — codegen'd from src/qlang/j.q (`.j` JSON ns) */
#include "qlang/q_env.h"      /* q_env_init/destroy — the q K-tree lifecycle */
#include "lang/eval.h"        /* ray_eval_set_remote_* teardown */
#include <rayforce.h>
#include <stdio.h>            /* fprintf, stderr — bootstrap diagnostics */
#include <string.h>           /* memcpy, strchr, strlen — line split */

/* One embedded core source through THE script seam (q_ctx_run_src), post-
 * registry (rule 6).  An UNPARSEABLE statement aborts the load (the seam's
 * script law) and the CALLER fails q_runtime_create loudly — never a runtime
 * whose core stopped mid-file.  Eval errors keep the script law's report-and-
 * continue (loud on stderr; the seam serves `\l` too, whose banked semantics
 * continue).  Non-zero = aborted. */
static int bootstrap_run(const char* src, const char* what) {
    int rc = q_ctx_run_src(src, stdout, stderr);
    if (rc)
        fprintf(stderr, "q bootstrap: %s ABORTED at an unparseable statement — no runtime\n",
                what);
    return rc;
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
        q_provider_init();       /* virtual-table provider registry (int<->alias<->conn) */
        q_splay_init();        /* the lazy splayed-table registry (maps + column caches) */
        q_ctx_install_remote_hooks();  /* paired with the teardown in q_runtime_destroy */
        q_builtins_register();
        q_eval_apply_init();   /* head-identity cache — needs the built registry */
        /* `.z.*` is an eval-time resolver, NOT a namespace: compute the
         * process-constant argv values once; q_eval resolves them directly
         * (no base name hook — one pipeline, cutover 2026-07-23). */
        q_dotz_init(argc, argv);
        /* the ORDERED core list: q.q before dotq.q (semantic), then .h, .j */
        if (bootstrap_run(PEACHQ_BOOTSTRAP,   "q.q+dotq.q") ||
            bootstrap_run(PEACHQ_H_BOOTSTRAP, "h.q") ||
            bootstrap_run(PEACHQ_J_BOOTSTRAP, "j.q")) {
            q_runtime_destroy(rt);
            return NULL;
        }
        /* QK_QSRC manifest cells (infix q.q keywords) snapshot their `.q`
         * definitions now that the bootstrap has bound them. */
        if (q_registry_bind_qsrc() != RAY_OK)
            fprintf(stderr, "q bootstrap: qsrc registry bind failed\n");
    }
    return rt;
}

void q_runtime_destroy(ray_runtime_t* rt) {
    q_duckdb_reset();          /* close DuckDB handles/dbs (suite isolation) */
    q_re2_reset();             /* no compiled pattern outlives its runtime */
    ray_eval_set_remote_str_fn(NULL);  /* remote strings fall back to rayfall */
    ray_eval_set_remote_apply_fn(NULL);/* (func;args) value-apply -> 'nyi w/o q runtime */
    q_dbg_reset();             /* drop snapshot-retained lambdas before the env */
    q_handles_destroy();       /* drop handle records (open_args refs) before the env */
    q_provider_destroy();        /* drop provider records (connid refs) before the env */
    q_dotz_destroy();          /* free the `.z.*` argv snapshots */
    q_registry_destroy();      /* free verb snapshots before the env goes away */
    q_eval_syms_reset();       /* cached sym ids die with this runtime's sym table */
    q_view_reset();            /* view roster + cached sym ids likewise */
    q_env_destroy();           /* release q's K-tree before the heap dies */
    q_splay_destroy();         /* munmap column regions AFTER every holder released */
    q_sys_ctx_reset();         /* drop the `\d` context with its runtime */
    q_console_pipe_disable();          /* reset the `\classic` display global — the
                                * process-wide pipe mode never leaks into the next
                                * runtime (fresh-per-file doctest, wasm re-init) */
    ray_runtime_destroy(rt);
}
