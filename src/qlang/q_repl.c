/* q_repl — see q_repl.h.  Shared by the `q` binary and the qcmd test runner. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/q_repl.h"
#include "qlang/q_parse.h"
#include "qlang/q_fmt.h"
#include "lang/eval.h"      /* ray_eval */
#include <rayforce.h>
#include <string.h>

void q_repl_run(FILE* in, FILE* out, FILE* err, int echo) {
    char line[4096];

    for (;;) {
        fputs("q)", out);
        fflush(out);

        if (!fgets(line, sizeof line, in)) { fputc('\n', out); break; }

        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';

        if (echo) { fputs(line, out); fputc('\n', out); }

        if (n == 0) continue;
        if (!strcmp(line, "\\\\") || !strcmp(line, "exit")) break;

        ray_t* ast = q_parse(line);
        if (RAY_IS_ERR(ast)) { fprintf(err, "parse error\n"); continue; }

        ray_t* r = ray_eval(ast);
        ray_release(ast);

        if (RAY_IS_ERR(r)) {
            const char* code = (const char*)r->sdata;
            fprintf(err, "error: %s\n", (code && *code) ? code : "eval");
            continue;
        }
        if (!RAY_IS_NULL(r)) {
            char buf[8192];
            q_fmt(r, buf, sizeof buf);
            fputs(buf, out);
            fputc('\n', out);
        }
        ray_release(r);
        fflush(out);
    }
}
