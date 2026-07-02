/* qdoc — see qdoc.h. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/qdoc.h"
#include "qlang/q_parse.h"
#include "qlang/q_fmt.h"
#include "lang/eval.h"      /* ray_eval */
#include "ops/ops.h"        /* ray_is_lazy, ray_lazy_materialize */
#include <rayforce.h>
#include <string.h>

#define QD_IN   2048
#define QD_OUT  8192

/* Copy s into out without '\r' and with leading/trailing whitespace trimmed —
 * the spec's whitespace-insensitive compare (leading/trailing space, CRLF). */
static void normalize(const char* s, char* out, size_t osz) {
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 1 < osz; i++)
        if (s[i] != '\r') out[j++] = s[i];
    out[j] = '\0';
    while (j > 0 && (out[j-1] == ' ' || out[j-1] == '\t' || out[j-1] == '\n'))
        out[--j] = '\0';
    size_t k = 0;
    while (out[k] == ' ' || out[k] == '\t' || out[k] == '\n') k++;
    if (k) memmove(out, out + k, j - k + 1);
}

static int ends_with(const char* s, const char* suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

/* Run one example; update result; report on failure when verbose. */
static void run_example(const char* input, const char* expect, qdoc_mode_t mode,
                        int verbose, FILE* out, const char* path,
                        qdoc_result_t* r) {
    r->examples++;

    ray_t* ast = q_parse(input);
    if (RAY_IS_ERR(ast)) {
        r->failed++;
        if (verbose) fprintf(out, "  %.80s: FAIL (parse) q)%.120s\n", path, input);
        return;
    }
    if (mode == QDOC_PARSE_ONLY) {
        ray_release(ast);
        r->passed++;
        return;
    }

    ray_t* res = ray_eval(ast);
    ray_release(ast);
    if (ray_is_lazy(res)) res = ray_lazy_materialize(res);

    /* An eval error is a failure — NOT empty output that could match an empty
     * expected.  Otherwise every no-output example we can't run (assignments,
     * table defs, `select`) would falsely "match". */
    if (RAY_IS_ERR(res)) {
        r->failed++;
        if (verbose)
            fprintf(out, "  %.60s: FAIL (eval) q)%.110s\n", path, input);
        return;
    }

    char got[QD_OUT];
    got[0] = '\0';
    if (!RAY_IS_NULL(res)) q_fmt(res, got, sizeof got);
    ray_release(res);

    char ng[QD_OUT], ne[QD_OUT];
    normalize(got, ng, sizeof ng);
    normalize(expect, ne, sizeof ne);
    if (strcmp(ng, ne) == 0) {
        r->passed++;
    } else {
        r->failed++;
        if (verbose)
            fprintf(out, "  %.60s: FAIL q)%.100s -> \"%.120s\", want \"%.120s\"\n",
                    path, input, ng, ne);
    }
}

qdoc_result_t qdoc_run_file(const char* path, qdoc_mode_t mode,
                            int verbose, FILE* out) {
    qdoc_result_t r = {0, 0, 0, 0};

    FILE* f = fopen(path, "r");
    if (!f) {
        if (verbose) fprintf(out, "  %.80s: cannot open\n", path);
        return r;
    }

    int is_qcmd  = ends_with(path, ".qcmd");
    int in_block = is_qcmd;   /* .qcmd: whole file is one block */

    char line[QD_OUT];
    char input[QD_IN]  = {0};
    char expect[QD_OUT] = {0};
    int  have = 0;

#define FLUSH() do { if (have) { run_example(input, expect, mode, verbose, out, path, &r); \
                                 have = 0; expect[0] = '\0'; } } while (0)

    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';

        if (!is_qcmd) {
            if (!in_block) {
                /* open a ```q fence (exactly, optionally with trailing info) */
                if (strncmp(line, "```q", 4) == 0 && (line[4] == '\0' || line[4] == ' '))
                    in_block = 1;
                continue;
            }
            if (strncmp(line, "```", 3) == 0) {   /* fence close */
                FLUSH();
                in_block = 0;
                continue;
            }
        }

        if (strncmp(line, "q)", 2) == 0) {
            FLUSH();
            snprintf(input, sizeof input, "%.2047s", line + 2);
            expect[0] = '\0';
            have = 1;
        } else if (have) {
            size_t e = strlen(expect);
            if (e && e + 1 < QD_OUT) expect[e++] = '\n';
            size_t room = (e < QD_OUT) ? QD_OUT - 1 - e : 0;
            if (n > room) n = room;
            memcpy(expect + e, line, n);
            expect[e + n] = '\0';
        }
    }
    FLUSH();
#undef FLUSH

    fclose(f);
    return r;
}
