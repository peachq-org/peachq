/* q — a minimal q REPL: read a line, parse it to a rayforce ray_t via
 * q_parse, evaluate through the unmodified rayforce engine, print the result.
 *
 * Deliberately thin: no error traces, progress bar, remote session, or piped
 * bracket accumulation — those live in the (frozen) rayforce repl.c and are
 * reused later.  See ARCHITECTURE.md and the MVP design doc. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/q_parse.h"
#include "lang/eval.h"   /* ray_eval, ray_lang_print */
#include <rayforce.h>
#include <stdio.h>
#include <string.h>

static void eval_line(const char *s) {
    ray_t *ast = q_parse(s);
    if (RAY_IS_ERR(ast)) {
        fprintf(stderr, "parse error\n");
        return;
    }

    ray_t *r = ray_eval(ast);
    ray_release(ast);

    if (RAY_IS_ERR(r)) {
        const char *code = (const char *)r->sdata;
        fprintf(stderr, "error: %s\n", (code && *code) ? code : "eval");
        return;
    }
    if (!RAY_IS_NULL(r)) {
        ray_lang_print(stdout, r);
        fputc('\n', stdout);
    }
    ray_release(r);
}

int main(int argc, char **argv) {
    ray_runtime_t *rt = ray_runtime_create(argc, argv);
    if (!rt) { fprintf(stderr, "runtime init failed\n"); return 1; }

    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (!n) continue;
        if (!strcmp(line, "\\\\") || !strcmp(line, "exit")) break;
        eval_line(line);
        fflush(stdout);
    }

    ray_runtime_destroy(rt);
    return 0;
}
