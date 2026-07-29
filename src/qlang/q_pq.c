/* q_pq — PeachQ stdlib gate: the `\l pq` embedded loader.  Mirrors
 * q_runtime.c's bootstrap loader; pq members live in the USER `.pq` namespace.
 * The `.pq.c.*` natives (ray/parse/tree — the rayfall escape hatch) were
 * deleted 2026-07-29 with the q-owned env: q no longer exposes rayfall. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_pq.h"
#include "qlang/q_parse.h"     /* q_parse — the q eval pipeline */
#include "qlang/eval/q_eval.h" /* q_eval — THE eval pipeline */
#include "qlang/pq_gen.h"      /* OPENQ_PQ_BOOTSTRAP — codegen'd from src/qlang/pq.q */
#include <rayforce.h>
#include <stdio.h>             /* fprintf, stderr — gate diagnostics */
#include <string.h>            /* strchr, memcpy, strlen */

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

void q_pq_load(void) {
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
