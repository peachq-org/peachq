/* q_pq — PeachQ stdlib gate: `.pq.c.*` natives + the `\l pq` embedded loader.
 * See q_pq.h. Mirrors q_runtime.c's q_bootstrap loader, minus the `.q.`
 * privileged binder — pq members live in the USER `.pq` namespace. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_pq.h"
#include "qlang/q_parse.h"     /* q_parse, q_lower — the q eval pipeline */
#include "qlang/pq_gen.h"      /* OPENQ_PQ_BOOTSTRAP — codegen'd from src/qlang/pq.q */
#include "lang/env.h"          /* ray_fn_unary, ray_env_bind, ray_sym_intern */
#include "lang/eval.h"         /* ray_eval, ray_eval_str, RAY_FN_NONE */
#include <rayforce.h>
#include <stdio.h>             /* fprintf, stderr — gate diagnostics */
#include <string.h>            /* strchr, memcpy, strlen */
#include <stdlib.h>            /* malloc, free */

/* (.pq.c.ray str) — the generic rayfall (lisp) escape hatch. Parse+eval the
 * SOURCE string through the engine's own rayfall dialect (ray_eval_str) and
 * return the value as-is. Results are RAW engine values (exotic rayfall shapes
 * may display oddly through q_fmt — accepted; internal hatch). ray_eval_str
 * needs a NUL-terminated C string; a RAY_STR atom's bytes are not guaranteed
 * terminated, so copy through a bounded scratch buffer. Errors propagate as
 * ordinary q errors. */
static ray_t* q_pq_ray_fn(ray_t* x) {
    if (!x || x->type != -RAY_STR) return ray_error("type", "ray expects a string");
    const char* sp = ray_str_ptr(x);
    size_t sl = ray_str_len(x);
    if (!sp) return ray_error("domain", "ray: bad source string");
    char* src = malloc(sl + 1);
    if (!src) return ray_error("wsfull", "ray: out of memory");
    memcpy(src, sp, sl);
    src[sl] = '\0';
    ray_t* r = ray_eval_str(src);
    free(src);
    return r;   /* owned value / owned error / NULL void — all propagate as-is
                 * (the q apply layer treats NULL as a legitimate void result) */
}

/* Eval one q source line via the q pipeline (q_parse -> q_lower -> ray_eval),
 * mirroring q_runtime.c q_bootstrap_eval. OWNED value, or NULL on error
 * (reported to stderr — non-fatal per line, like the embedded bootstrap). */
static ray_t* q_pq_eval(const char* src) {
    ray_t* ast = q_parse(src);
    if (RAY_IS_ERR(ast)) { fprintf(stderr, "pq load: parse error: %s\n", src); ray_error_free(ast); return NULL; }
    ast = q_lower(ast);
    if (RAY_IS_ERR(ast)) { fprintf(stderr, "pq load: lower error: %s\n", src); ray_release(ast); return NULL; }
    ray_t* r = ray_eval(ast);
    ray_release(ast);
    if (RAY_IS_ERR(r)) { fprintf(stderr, "pq load: eval error: %s\n", src); ray_error_free(r); return NULL; }
    return r;
}

/* Bind the `.pq.c.*` natives into the env. A dotted bind routes through
 * env_set_dotted → builds the `.pq`/`.pq.c` namespace dicts; the name resolves
 * via the dotted-namespace walk (not a flat slot). Called before evaluating
 * pq.q so the `.pq.ray:.pq.c.ray` alias resolves. Returns RAY_OK, else the
 * bind error (e.g. `.pq` pre-exists as a non-dict → 'type) so the loader can
 * fail fast without a partial install. */
static ray_err_t q_pq_bind_natives(void) {
    ray_t* rayfn = ray_fn_unary(".pq.c.ray", RAY_FN_NONE, q_pq_ray_fn);
    if (RAY_IS_ERR(rayfn)) { ray_error_free(rayfn); return RAY_ERR_OOM; }  /* ray_fn_unary → owned 'oom on failure */
    ray_err_t rc = ray_env_bind(ray_sym_intern(".pq.c.ray", 9), rayfn);
    ray_release(rayfn);   /* bind retains */
    return rc;
}

void q_pq_load(void) {
    if (q_pq_bind_natives() != RAY_OK) {   /* fail fast: no partial install */
        fprintf(stderr, "pq load: native bind failed (.pq occupied?)\n");
        return;
    }
    const char* p = OPENQ_PQ_BOOTSTRAP;
    char line[4096];
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        const char* next = nl ? nl + 1 : p + n;
        if (n >= sizeof(line)) { fprintf(stderr, "pq load: line too long (%zu bytes), skipped\n", n); p = next; continue; }
        memcpy(line, p, n); line[n] = '\0'; p = next;
        const char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '/') continue;   /* blank / `/` comment */
        ray_t* r = q_pq_eval(line);
        if (r) ray_release(r);
    }
}
