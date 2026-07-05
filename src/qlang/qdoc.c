/* qdoc — see qdoc.h. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/qdoc.h"
#include "qlang/q_parse.h"
#include "qlang/q_fmt.h"
#include "lang/eval.h"      /* ray_eval */
#include "ops/ops.h"        /* ray_is_lazy, ray_lazy_materialize */
#include <rayforce.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

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

/* Error-expectation rows: an expected output whose FIRST line starts with `'`
 * (kdb error display: 'type) asserts the example ERRORS.  Only the first
 * line is consulted — newer kdb appends stack-trace lines we deliberately do
 * not support yet, so trailing lines are ignored by construction.  Default
 * is LENIENT (any eval/lower error matches); QDOC_STRICT_ERRORS=1 also
 * requires the error CLASS to equal the word after the quote ('type ->
 * code "type").  Returns 1 and fills cls[] (may be empty) when the row is an
 * error expectation. */
static int expect_is_error(const char* expect, char* cls, size_t csz) {
    const char* p = expect;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\'') return 0;
    p++;
    size_t i = 0;
    while (p[i] && p[i] != '\n' && p[i] != ' ' && i + 1 < csz) {
        cls[i] = p[i];
        i++;
    }
    cls[i] = '\0';
    return 1;
}

static int strict_errors(void) {
    const char* e = getenv("QDOC_STRICT_ERRORS");
    return e && *e && *e != '0';
}

/* Match an actual error object against an error-expectation row.  The error
 * class lives in the RAY_ERROR's sdata (same field q_repl prints). */
static int error_row_matches(ray_t* err, const char* cls) {
    if (!strict_errors() || !cls[0]) return 1;          /* lenient: any error */
    const char* code = (const char*)err->sdata;
    return code && strncmp(code, cls, 7) == 0;
}

/* Run one example; update result; report on failure when verbose. */
/* Route an example's pass/fail verdict into the right bucket given whether it is
 * annotated expected-fail.  Non-xfail: matched->passed, mismatched->failed (the
 * only state that breaks the gate).  xfail: matched->ready (un-defer me),
 * mismatched->deferred (known-not-implemented, gate-neutral). */
static void classify(qdoc_result_t* r, int ok, int xfail) {
    if (xfail) { if (ok) r->ready++;  else r->deferred++; }
    else       { if (ok) r->passed++; else r->failed++;   }
}

static void run_example(const char* input, const char* expect, qdoc_mode_t mode,
                        int verbose, FILE* out, const char* path, int xfail,
                        qdoc_result_t* r) {
    r->examples++;

    ray_t* ast = q_parse(input);
    if (RAY_IS_ERR(ast)) {
        classify(r, 0, xfail);
        if (verbose) fprintf(out, "  q)%.200s\n    FAIL(parse)\n", input);
        return;
    }
    r->parsed++;   /* parsed OK — count before eval, regardless of outcome */
    if (mode == QDOC_PARSE_ONLY) {
        ray_release(ast);
        classify(r, 1, xfail);
        return;
    }

    if (getenv("QDOC_TRACE")) { char tb[256]; int tn = snprintf(tb, sizeof tb, "INPUT: %.200s\n", input); if (tn > 0) { ssize_t _w = write(2, tb, (size_t)tn); (void)_w; } }
    char errcls[8];
    int want_error = expect_is_error(expect, errcls, sizeof errcls);
    int is_assign = q_ast_is_assign(ast);   /* pre-lower shape */
    ast = q_lower(ast);
    if (RAY_IS_ERR(ast)) {
        if (want_error && error_row_matches(ast, errcls)) {
            ray_release(ast);
            classify(r, 1, xfail);
            return;
        }
        char ne[QD_OUT];
        normalize(expect, ne, sizeof ne);
        ray_release(ast);
        classify(r, 0, xfail);
        if (verbose)
            fprintf(out, "  q)%.200s\n    FAIL(lower) got \"<error>\" want \"%.200s\"\n",
                    input, ne);
        return;
    }
    ray_t* res = ray_eval(ast);
    ray_release(ast);
    if (ray_is_lazy(res)) res = ray_lazy_materialize(res);

    /* An eval error is a failure — NOT empty output that could match an empty
     * expected (otherwise every no-output example we can't run would falsely
     * "match") — UNLESS the row is an error EXPECTATION ('type). */
    if (RAY_IS_ERR(res)) {
        if (want_error && error_row_matches(res, errcls)) {
            ray_release(res);
            classify(r, 1, xfail);
            return;
        }
        char ne[QD_OUT];
        normalize(expect, ne, sizeof ne);
        ray_release(res);
        classify(r, 0, xfail);
        if (verbose)
            fprintf(out, "  q)%.200s\n    FAIL(eval) got \"<error>\" want \"%.200s\"\n",
                    input, ne);
        return;
    }

    /* an error-expectation row that did NOT error is a failure */
    if (want_error) {
        char got_ok[QD_OUT];
        got_ok[0] = '\0';
        if (!RAY_IS_NULL(res) && !is_assign) q_fmt(res, got_ok, sizeof got_ok);
        ray_release(res);
        classify(r, 0, xfail);
        if (verbose)
            fprintf(out, "  q)%.200s\n    FAIL(eval) got \"%.200s\" want error '%s\n",
                    input, got_ok, errcls);
        return;
    }

    char got[QD_OUT];
    got[0] = '\0';
    /* q console silence: a (last-statement) assignment prints nothing. */
    if (!RAY_IS_NULL(res) && !is_assign) q_fmt(res, got, sizeof got);
    ray_release(res);

    char ng[QD_OUT], ne[QD_OUT];
    normalize(got, ng, sizeof ng);
    normalize(expect, ne, sizeof ne);
    if (strcmp(ng, ne) == 0) {
        classify(r, 1, xfail);
    } else {
        classify(r, 0, xfail);
        if (verbose)
            fprintf(out, "  q)%.200s\n    FAIL(eval) got \"%.200s\" want \"%.200s\"\n",
                    input, ng, ne);
    }
}

qdoc_result_t qdoc_run_file(const char* path, qdoc_mode_t mode,
                            int verbose, FILE* out) {
    qdoc_result_t r = {0};

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
    int  cur_xfail = 0;     /* the in-flight example is annotated xfail */
    int  pend_xfail = 0;    /* an xfail directive seen before its q) input */
    int  file_deferred = 0; /* `;; STATUS: deferred` header — every example xfail */

#define FLUSH() do { if (have) { run_example(input, expect, mode, verbose, out, path, \
                                 (cur_xfail || file_deferred), &r); \
                                 have = 0; expect[0] = '\0'; cur_xfail = 0; } } while (0)

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

        /* `;;` annotation directives — NEVER expected output.  `;; STATUS:
         * deferred` marks the whole file xfail; `;; xfail` marks the current
         * example (or the next one, if seen before its q)); any other `;;`
         * line is a plain comment.  No q program emits `;;`, so this cannot
         * collide with real expected output. */
        {
            const char* t = line;
            while (*t == ' ' || *t == '\t') t++;
            if (t[0] == ';' && t[1] == ';') {
                const char* d = t + 2;
                while (*d == ' ' || *d == '\t') d++;
                if (strncmp(d, "STATUS:", 7) == 0) {
                    if (strstr(d, "deferred")) file_deferred = 1;
                } else if (strncmp(d, "xfail", 5) == 0) {
                    if (have) cur_xfail = 1; else pend_xfail = 1;
                }
                continue;
            }
        }

        if (strncmp(line, "q)", 2) == 0) {
            FLUSH();
            cur_xfail = pend_xfail; pend_xfail = 0;
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
