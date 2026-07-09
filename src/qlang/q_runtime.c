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
#include "qlang/q_sys.h"      /* q_sys_seed_init — `\S` constant-seed contract */
#include "qlang/q_parse.h"    /* q_parse, q_lower — embedded-bootstrap loader */
#include "qlang/dotq_gen.h"   /* OPENQ_BOOTSTRAP — codegen'd from src/qlang/dotq.q */
#include "lang/eval.h"        /* ray_eval_set_apply_hook / _name_hook / ray_eval */
#include <rayforce.h>
#include <stdio.h>            /* fprintf, stderr — bootstrap diagnostics */
#include <string.h>           /* memcpy, strchr, strlen — bootstrap line split */

/* Load the embedded q bootstrap (src/qlang/dotq.q -> OPENQ_BOOTSTRAP) line at a
 * time, AFTER the registry and name hook are live (rule 6: registry builders
 * never q_parse; the bootstrap runs post-build).  Mirrors the REPL script-load
 * path (parse -> lower -> eval) but SILENT: results are discarded (assignments
 * print nothing anyway).  Blank lines and `/` full-line comments are skipped so
 * dotq.q reads as ordinary q source.  A malformed / overlong line is reported on
 * stderr and skipped — never fatal to runtime creation.
 *
 * Refcount discipline mirrors run_one_line exactly (q_repl.c): q_lower consumes
 * its input AST and returns a fresh value/error, so `ast` is reassigned to its
 * return; parse error -> ray_error_free; lower error -> ray_release (the
 * returned err); eval error -> ray_error_free; success -> release AST then r. */
static void q_bootstrap_load(void) {
    const char* p = OPENQ_BOOTSTRAP;
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

        ray_t* ast = q_parse(line);
        if (RAY_IS_ERR(ast)) {
            fprintf(stderr, "q bootstrap: parse error: %s\n", line);
            ray_error_free(ast);
            continue;
        }
        ast = q_lower(ast);
        if (RAY_IS_ERR(ast)) {
            fprintf(stderr, "q bootstrap: lower error: %s\n", line);
            ray_release(ast);
            continue;
        }
        ray_t* r = ray_eval(ast);
        ray_release(ast);
        if (RAY_IS_ERR(r)) {
            fprintf(stderr, "q bootstrap: eval error: %s\n", line);
            ray_error_free(r);
            continue;
        }
        ray_release(r);
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
        q_ns_reset();          /* fresh runtime starts in the root context */
        q_sys_seed_init();     /* kdb constant-seed-at-startup contract (\S) */
        q_sys_cfg_init();      /* \P/\c/\C/\g/\o/\W/\e/\s defaults per runtime */
        q_builtins_register();
        /* `.z.*` is an eval-time resolver, NOT a namespace: compute the
         * process-constant argv values once and install the name-load hook. */
        q_dotz_init(argc, argv);
        ray_eval_set_name_hook(q_dotz_resolve);
        q_bootstrap_load();    /* embedded .q stdlib, post-registry (rule 6) */
    }
    return rt;
}

void q_runtime_destroy(ray_runtime_t* rt) {
    ray_eval_set_apply_hook(NULL);   /* detach the q dispatcher first */
    ray_eval_set_remote_str_fn(NULL);  /* remote strings fall back to rayfall */
    ray_eval_set_remote_apply_fn(NULL);/* (func;args) value-apply -> 'nyi w/o q runtime */
    ray_eval_set_name_hook(NULL);    /* detach `.z.*` before its values die */
    q_dotz_destroy();          /* free the `.z.*` argv snapshots */
    q_registry_destroy();      /* free verb snapshots before the env goes away */
    q_deriv_reset_markers();   /* marker sym-ids die with this runtime's table */
    q_ns_reset();              /* drop the `\d` context with its runtime */
    ray_runtime_destroy(rt);
}
