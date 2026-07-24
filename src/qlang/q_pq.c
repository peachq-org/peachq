/* q_pq — PeachQ stdlib gate: `.pq.c.*` natives + the `\l pq` embedded loader.
 * See q_pq.h. Mirrors q_runtime.c's q_bootstrap loader, minus the `.q.`
 * privileged binder — pq members live in the USER `.pq` namespace. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_pq.h"
#include "qlang/q_err.h"
#include "qlang/q_parse.h"     /* q_parse — the q eval pipeline */
#include "qlang/eval/q_eval.h" /* q_eval — THE eval pipeline */
#include "qlang/pq_gen.h"      /* OPENQ_PQ_BOOTSTRAP — codegen'd from src/qlang/pq.q */
#include "lang/env.h"          /* ray_fn_unary, ray_env_bind, ray_sym_intern */
#include "lang/eval.h"         /* ray_eval, ray_eval_str, RAY_FN_NONE */
#include <rayforce.h>
#include <stdio.h>             /* fprintf, stderr — gate diagnostics */
#include <string.h>            /* strchr, memcpy, strlen */
#include <stdlib.h>            /* malloc, free */
#include "qlang/q_registry.h"   /* q_str_text_bytes, q_str_charv_out — text accessors */
#include "lang/format.h"        /* ray_fmt — rayforce-native tree display */

/* Copy a q text arg (charv / char atom / -RAY_STR) into a malloc'd NUL-
 * terminated C string. q_parse / ray_eval_str need termination; a charv's
 * bytes are not guaranteed terminated. On failure returns NULL and sets *err
 * to an owned RAY_ERROR the caller returns as-is. Free the result with free(). */
static char* pq_src_dup(ray_t* x, ray_t** err) {
    const char* sp; int64_t sn;
    if (!q_str_text_bytes(x, &sp, &sn)) { *err = q_err(QE_TYPE); return NULL; }
    if (!sp) { *err = q_err(QE_DOMAIN); return NULL; }
    size_t sl = (size_t)sn;
    char* src = malloc(sl + 1);
    if (!src) { *err = q_err(QE_WSFULL); return NULL; }
    memcpy(src, sp, sl);
    src[sl] = '\0';
    return src;
}

/* (.pq.c.ray str) — the generic rayfall (lisp) escape hatch. Parse+eval the
 * SOURCE string through the engine's own rayfall dialect (ray_eval_str) and
 * return the value as-is. Results are RAW engine values (exotic rayfall shapes
 * may display oddly through q_fmt — accepted; internal hatch). Errors propagate
 * as ordinary q errors. */
static ray_t* pq_ray_fn(ray_t* x) {
    ray_t* err = NULL;
    char* src = pq_src_dup(x, &err);
    if (!src) return err;
    ray_t* r = ray_eval_str(src);
    free(src);
    return r;   /* owned value / owned error / NULL void — all propagate as-is
                 * (the q apply layer treats NULL as a legitimate void result) */
}

/* (.pq.c.parse str) — the raw parse tree rendered in RAYFORCE-native
 * notation via ray_fmt (unlike `parse`, which renders q notation). Returns a q
 * char-vector so the tree text is composable. */
static ray_t* pq_parse_fn(ray_t* x) {
    ray_t* err = NULL;
    char* src = pq_src_dup(x, &err);
    if (!src) return err;
    ray_t* ast = q_parse(src);
    free(src);
    if (RAY_IS_ERR(ast)) return ast;   /* q parse error -> q error, as-is */
    ray_t* repr = ray_fmt(ast, 0);
    ray_release(ast);
    return q_str_charv_out(repr);   /* consumes repr, returns owned q charv */
}

/* (.pq.c.tree str) — the EVAL tree (exactly what q_eval receives — the raw
 * parse tree, one pipeline) rendered via ray_fmt (rayforce-native).
 * Returns a q char-vector. */
static ray_t* pq_tree_fn(ray_t* x) {
    ray_t* err = NULL;
    char* src = pq_src_dup(x, &err);
    if (!src) return err;
    ray_t* ast = q_parse(src);
    free(src);
    if (RAY_IS_ERR(ast)) return ast;
    ray_t* repr = ray_fmt(ast, 0);
    ray_release(ast);
    return q_str_charv_out(repr);
}

/* Eval one q source line via the q pipeline (q_parse -> q_eval), mirroring
 * q_runtime.c bootstrap_eval. OWNED value, or NULL on error (reported to
 * stderr — non-fatal per line, like the embedded bootstrap). */
static ray_t* pq_eval(const char* src) {
    ray_t* ast = q_parse(src);
    if (RAY_IS_ERR(ast)) { fprintf(stderr, "pq load: parse error: %s\n", src); ray_error_free(ast); return NULL; }
    ray_t* r = q_eval(ast);
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
static ray_err_t pq_bind_one(const char* name, int nlen, ray_t* (*fn)(ray_t*)) {
    ray_t* v = ray_fn_unary(name, RAY_FN_NONE, fn);
    if (RAY_IS_ERR(v)) { ray_error_free(v); return RAY_ERR_OOM; }  /* ray_fn_unary → owned 'oom on failure */
    ray_err_t rc = ray_env_bind(ray_sym_intern(name, nlen), v);
    ray_release(v);   /* bind retains */
    return rc;
}

static ray_err_t pq_bind_natives(void) {
    ray_err_t rc = pq_bind_one(".pq.c.ray", 9, pq_ray_fn);
    if (rc != RAY_OK) return rc;
    rc = pq_bind_one(".pq.c.parse", 11, pq_parse_fn);
    if (rc != RAY_OK) return rc;                                   /* fail fast: no partial install */
    return pq_bind_one(".pq.c.tree", 10, pq_tree_fn);
}

void q_pq_load(void) {
    if (pq_bind_natives() != RAY_OK) {   /* fail fast: no partial install */
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
        ray_t* r = pq_eval(line);
        if (r) ray_release(r);
    }
}
