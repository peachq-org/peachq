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
#include "qlang/q_fmt_pipe.h" /* q_pipe_disable — reset the `\nonlegacy` display global per runtime */
#include "qlang/q_parse.h"    /* q_parse, q_lower — embedded-bootstrap loader */
#include "qlang/dotq_gen.h"   /* OPENQ_BOOTSTRAP — codegen'd from src/qlang/{q,dotq}.q */
#include "lang/env.h"         /* ray_env_bind — `.q.*` keyword bindings */
#include "lang/eval.h"        /* ray_eval_set_apply_hook / _name_hook / ray_eval */
#include <rayforce.h>
#include <stdio.h>            /* fprintf, stderr — bootstrap diagnostics */
#include <string.h>           /* memcpy, strchr, strlen, strncmp — line split */

/* One q line -> owned value; NULL after a stderr report (never fatal).
 * Refcounts mirror q_repl.c run_one_line (q_lower consumes its input AST). */
static ray_t* q_bootstrap_eval(const char* src) {
    ray_t* ast = q_parse(src);
    if (RAY_IS_ERR(ast)) {
        fprintf(stderr, "q bootstrap: parse error: %s\n", src);
        ray_error_free(ast);
        return NULL;
    }
    ast = q_lower(ast);
    if (RAY_IS_ERR(ast)) {
        fprintf(stderr, "q bootstrap: lower error: %s\n", src);
        ray_release(ast);
        return NULL;
    }
    ray_t* r = ray_eval(ast);
    ray_release(ast);
    if (RAY_IS_ERR(r)) {
        fprintf(stderr, "q bootstrap: eval error: %s\n", src);
        ray_error_free(r);
        return NULL;
    }
    return r;
}

/* `.q.NAME:RHS` (q.q keyword): eval RHS, bind via the BUILTIN binder — `.q`
 * is a reserved root (env.c) user `set` refuses; the bootstrap is the one
 * privileged writer (kdb: q.k populates .q).  1 = handled, 0 = fall through.
 *
 * Shadow-rebind: bare-name resolution reaches `.q` only on an env MISS, so a
 * same-named rayfall builtin at root (env `med` unary, env `rand` dyadic)
 * would win over the q.q keyword.  When root NAME is already bound, rebind it
 * to the q.q value too — in the q runtime the q keyword IS the name's meaning
 * (engine code calls its kernels by function, not by env name). */
static int q_bootstrap_dotq_line(const char* s) {
    if (strncmp(s, ".q.", 3) != 0) return 0;
    const char* p = s + 3;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '_')
        p++;
    if (p == s + 3 || *p != ':') return 0;
    ray_t* v = q_bootstrap_eval(p + 1);
    if (!v) return 1;                       /* reported by q_bootstrap_eval */
    if (ray_env_bind(ray_sym_intern(s, (size_t)(p - s)), v) != RAY_OK)
        fprintf(stderr, "q bootstrap: bind error: %s\n", s);
    int64_t bare = ray_sym_intern(s + 3, (size_t)(p - s - 3));
    if (ray_env_get(bare) && ray_env_bind(bare, v) != RAY_OK)
        fprintf(stderr, "q bootstrap: shadow-rebind error: %s\n", s);
    ray_release(v);                         /* bind retains */
    return 1;
}

/* Load the embedded bootstrap (q.q then dotq.q -> OPENQ_BOOTSTRAP) line at a
 * time, post-registry (rule 6), silently; blank/`/`-comment lines skipped,
 * `.q.name:` lines take the privileged binder, errors reported, never fatal. */
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

        if (q_bootstrap_dotq_line(s))  /* q.q keyword -> privileged binder */
            continue;
        ray_t* r = q_bootstrap_eval(line);
        if (r) ray_release(r);
    }
}

/* Name-load resolver: `.z.*` computed names first, then — stage-1 general
 * eval (2026-07-11) — a REGISTRY fallback so a bare q keyword verb stands as
 * a noun exactly as kdb's does (`sums` alone is the unary value; `(sums;`x)`
 * functional-form literals need the element to evaluate).  Fires only on an
 * env MISS, so every env-bound name keeps its meaning; registry handouts are
 * BORROWED — retain before returning the owned ref the hook contract wants. */
static ray_t* q_name_resolve(int64_t sym_id) {
    ray_t* v = q_dotz_resolve(sym_id);
    if (v) return v;
    ray_t* name = ray_sym_str(sym_id);   /* borrowed arena string */
    if (!name) return NULL;
    const char* p = ray_str_ptr(name);
    size_t      n = ray_str_len(name);
    /* KEYWORD verbs only: kdb lets `sums` stand bare as a noun, but a bare
     * GLYPH (`#`, `,`, `+`) is k-syntax and must stay an error (`',`) — see
     * the q-verb-forms rule.  Glyph values are reachable as `(#)` etc. */
    if (n == 0 || !((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')))
        return NULL;
    ray_t* hit = q_registry_lookup_name(p, n, Q_MONADIC);
    if (!hit) hit = q_registry_lookup_name(p, n, Q_DYADIC);
    if (!hit) {
        /* unbound bare name -> `.q.<name>` (kdb: keywords ARE .q entries) */
        hit = q_ns_dotq_get(p, n);               /* borrowed or NULL */
        if (!hit) return NULL;
    }
    ray_retain(hit);
    return hit;
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
        ray_eval_set_name_hook(q_name_resolve);
        q_bootstrap_load();    /* embedded .q stdlib, post-registry (rule 6) */
        /* QK_QSRC manifest cells (infix q.q keywords) snapshot their `.q`
         * definitions now that the bootstrap has bound them. */
        if (q_registry_bind_qsrc() != RAY_OK)
            fprintf(stderr, "q bootstrap: qsrc registry bind failed\n");
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
    q_pipe_disable();          /* reset the `\nonlegacy` display global — the
                                * process-wide pipe mode never leaks into the next
                                * runtime (fresh-per-file doctest, wasm re-init) */
    ray_runtime_destroy(rt);
}
