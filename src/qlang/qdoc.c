/* qdoc — see qdoc.h. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/qdoc.h"
#include "qlang/q_parse.h"
#include "qlang/q_fmt.h"
#include "qlang/q_ns.h"     /* q_ns_prompt — namespace transcripts */
#include "qlang/q_sys.h"    /* q_sys_dispatch — `\`-command dispatcher */
#include "lang/eval.h"      /* ray_eval */
#include "ops/ops.h"        /* ray_is_lazy, ray_lazy_materialize */
#include <rayforce.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>          /* open — per-file cwd containment (\cd / system"cd") */
#include <limits.h>         /* PATH_MAX — Windows cwd-restore buffer */
#include <sys/stat.h>       /* stat / S_ISDIR — QHOME fixtures-root probe */

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

/* Input-prompt prefix: `q)` or `q.<ident>)` (namespace transcripts after
 * `\d .foo` — basics/syscmds.md).  Strict: the ident must be
 * [A-Za-z][A-Za-z0-9_]* and the closing paren present, so ordinary output
 * lines starting with `q` never reclassify.  Returns the prefix length
 * (including `)`) or 0. */
static size_t prompt_prefix_len(const char* line) {
    if (line[0] != 'q') return 0;
    if (line[1] == ')') return 2;
    if (line[1] != '.') return 0;
    size_t i = 2;
    char c = line[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return 0;
    for (i = 3; line[i]; i++) {
        c = line[i];
        if (c == ')') return i + 1;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 0;
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
static void classify(qdoc_result_t* r, int ok) {
    if (ok) r->passed++; else r->failed++;
}

static void run_example(const char* input, const char* expect,
                        const char* tprompt, qdoc_mode_t mode,
                        int verbose, FILE* out, const char* path,
                        qdoc_result_t* r) {
    r->examples++;
    q_console_reset();   /* drop any show/0N! output from a prior example */

    /* Prompt pin: the transcript's prompt (`q)` / `q.foo)`) must match the
     * LIVE context prompt at this point — that is what tests the `\d` prompt
     * without a pty.  Mismatch fails the row; the input still executes so
     * the rest of the transcript stays in sync. */
    int prompt_ok = 1;
    if (tprompt && *tprompt) {
        char live[80];
        q_ns_prompt(live, sizeof live);
        prompt_ok = (strcmp(tprompt, live) == 0);
    }

    /* q system commands (\d \v \f \a) bypass the parser, like the REPL.  This is
     * the DOCTEST-runner adapter over the mode-less core, and it OWNS the
     * runner-safe policy (kept HERE, never baked into the shared dispatcher):
     * survive the exit/terminate commands (QS_QUIT/QS_TOGGLE — the runner-kill
     * hazard), and DECLINE to execute an unknown `\foo` (QS_UNKNOWN → leave it
     * to the parser, so \ls/\cat/\curl corpus rows never touch the FS / net). */
    {
        q_sys_result d = q_sys_dispatch(input, strlen(input));
        int handled = (d.kind == QS_VALUE || d.kind == QS_QUIT || d.kind == QS_TOGGLE);
        ray_t* sr = (d.kind == QS_VALUE) ? d.val : NULL;
        if (handled) {
            r->parsed++;
            if (mode == QDOC_PARSE_ONLY) {
                if (sr && RAY_IS_ERR(sr)) ray_error_free(sr);
                else if (sr) ray_release(sr);
                classify(r, prompt_ok);
                return;
            }
            char errcls[8];
            int want_error = expect_is_error(expect, errcls, sizeof errcls);
            char got[QD_OUT];
            got[0] = '\0';
            int ok;
            if (sr && RAY_IS_ERR(sr)) {
                ok = want_error && error_row_matches(sr, errcls);
                snprintf(got, sizeof got, "<error>");
                ray_error_free(sr);
            } else {
                if (sr && !RAY_IS_NULL(sr)) q_fmt_console(sr, got, sizeof got);
                if (sr) ray_release(sr);
                if (want_error) {
                    ok = 0;
                } else {
                    char ng[QD_OUT], ne[QD_OUT];
                    normalize(got, ng, sizeof ng);
                    normalize(expect, ne, sizeof ne);
                    ok = (strcmp(ng, ne) == 0);
                }
            }
            ok = ok && prompt_ok;
            classify(r, ok);
            if (!ok && verbose)
                fprintf(out, "  q)%.200s\n    FAIL(syscmd%s) got \"%.200s\" want \"%.200s\"\n",
                        input, prompt_ok ? "" : ":prompt", got, expect);
            return;
        }
    }

    /* Transcript prompt out of sync with the live context: fail the row but
     * still execute the input so later rows see the intended state. */
    if (!prompt_ok) {
        ray_t* past = q_parse(input);
        if (!RAY_IS_ERR(past)) {
            r->parsed++;
            past = q_lower(past);
            if (!RAY_IS_ERR(past)) {
                ray_t* pres = ray_eval(past);
                ray_release(pres);
            }
            ray_release(past);
        }
        classify(r, 0);
        if (verbose)
            fprintf(out, "  %s%.200s\n    FAIL(prompt) transcript prompt \"%s\" != live context\n",
                    tprompt, input, tprompt);
        return;
    }

    ray_t* ast = q_parse(input);
    if (RAY_IS_ERR(ast)) {
        classify(r, 0);
        if (verbose) fprintf(out, "  q)%.200s\n    FAIL(parse)\n", input);
        return;
    }
    r->parsed++;   /* parsed OK — count before eval, regardless of outcome */
    if (mode == QDOC_PARSE_ONLY) {
        ray_release(ast);
        classify(r, 1);
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
            classify(r, 1);
            return;
        }
        char ne[QD_OUT];
        normalize(expect, ne, sizeof ne);
        ray_release(ast);
        classify(r, 0);
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
            classify(r, 1);
            return;
        }
        char ne[QD_OUT];
        normalize(expect, ne, sizeof ne);
        ray_release(res);
        classify(r, 0);
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
        classify(r, 0);
        if (verbose)
            fprintf(out, "  q)%.200s\n    FAIL(eval) got \"%.200s\" want error '%s\n",
                    input, got_ok, errcls);
        return;
    }

    char got[QD_OUT];
    got[0] = '\0';
    size_t gpos = 0;
    /* show/0N! side-effect display comes FIRST (e.g. `f 2 3 5 7 3` prints the
     * `show a` line then the returned sum). */
    const char* con = q_console_str();
    if (con && *con) {
        gpos = strlen(con);
        if (gpos >= sizeof got) gpos = sizeof got - 1;
        memcpy(got, con, gpos);
        got[gpos] = '\0';
    }
    q_console_reset();
    /* q console silence: a (last-statement) assignment prints nothing.  Auto-
     * echo uses the DISPLAY seam so an in-transcript `\c` clips output (c.qcmd);
     * the fresh-per-file runtime arms `\c 25 80` by default (q_sys_cfg_init). */
    if (!RAY_IS_NULL(res) && !is_assign) q_fmt_console(res, got + gpos, sizeof got - gpos);
    ray_release(res);

    char ng[QD_OUT], ne[QD_OUT];
    normalize(got, ng, sizeof ng);
    normalize(expect, ne, sizeof ne);
    if (strcmp(ng, ne) == 0) {
        classify(r, 1);
    } else {
        classify(r, 0);
        if (verbose)
            fprintf(out, "  q)%.200s\n    FAIL(eval) got \"%.200s\" want \"%.200s\"\n",
                    input, ng, ne);
    }
}

qdoc_result_t qdoc_run_file(const char* path, qdoc_mode_t mode,
                            int verbose, FILE* out) {
    qdoc_result_t r = {0};

    /* Fixture resolution for `\l name` (h_l's QHOME search): point QHOME at the
     * committed fixtures root test/qscript so corpus rows like `\l sp.q` resolve
     * to test/qscript/sp.q.  Absolute (from cwd == repo root at entry, before any
     * in-file `\cd`); set once.  We OVERRIDE any inherited QHOME so a developer's
     * own kdb QHOME can't make the fixture-dependent suites non-deterministic.
     * A missing dir (odd cwd) leaves QHOME untouched -> those rows stay no-ops.
     * (Known limit: a standalone qdoctest run from a non-repo-root cwd won't set
     * it — consistent with the runner already requiring repo-root cwd.) */
    {
        static int qhome_set = 0;
        if (!qhome_set) {
            char qh[PATH_MAX];
            if (getcwd(qh, sizeof qh)) {
                size_t l = strlen(qh);
                const char* sub = "/test/qscript";
                if (l + strlen(sub) + 1 < sizeof qh) {
                    memcpy(qh + l, sub, strlen(sub) + 1);
                    struct stat st;
                    if (stat(qh, &st) == 0 && S_ISDIR(st.st_mode))
#if defined(RAY_OS_WINDOWS)
                        _putenv_s("QHOME", qh);        /* mingw has no setenv */
#else
                        setenv("QHOME", qh, /*overwrite=*/1);
#endif
                }
            }
            qhome_set = 1;
        }
    }

    FILE* f = fopen(path, "r");
    if (!f) {
        if (verbose) fprintf(out, "  %.80s: cannot open\n", path);
        return r;
    }

    /* Per-file cwd containment: `\cd` / `system "cd …"` really chdir() the
     * PROCESS now, so a transcript that changes directory (or fails to restore)
     * must NOT leak into the next file's relative paths (\l, save/load). Snapshot
     * cwd via a dir fd and fchdir() back after the file — handles a deleted cwd. */
#ifdef RAY_OS_WINDOWS
    /* mingw has neither O_CLOEXEC nor fchdir; snapshot the path and chdir() back. */
    char  cwd_buf[PATH_MAX];
    char* cwd_saved = getcwd(cwd_buf, sizeof cwd_buf);
#else
    int cwd_fd = open(".", O_RDONLY | O_CLOEXEC);
#endif

    int is_qcmd  = ends_with(path, ".qcmd");
    int in_block = is_qcmd;   /* .qcmd: whole file is one block */

    char line[QD_OUT];
    char input[QD_IN]  = {0};
    char expect[QD_OUT] = {0};
    char tprompt[80] = {0};
    int  have = 0;

#define FLUSH() do { if (have) { run_example(input, expect, tprompt, mode, verbose, out, path, &r); \
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

        size_t pl = prompt_prefix_len(line);
        if (pl) {
            FLUSH();
            snprintf(tprompt, sizeof tprompt, "%.*s", (int)(pl < 79 ? pl : 79), line);
            snprintf(input, sizeof input, "%.2047s", line + pl);
            expect[0] = '\0';
            /* Empty and comment-only inputs stay examples — they always
             * were (parse to nothing, pass), so the committed floors hold;
             * a trailing prompt-only `q.nn)` line just pins its prompt. */
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
#ifdef RAY_OS_WINDOWS
    if (cwd_saved) {                   /* restore cwd (contain any `\cd` in-file) */
        int rc = chdir(cwd_saved);
        (void)rc;                      /* best effort — nothing to recover to */
    }
#else
    if (cwd_fd >= 0) {                 /* restore cwd (contain any `\cd` in-file) */
        int rc = fchdir(cwd_fd);
        (void)rc;                      /* best effort — nothing to recover to */
        close(cwd_fd);
    }
#endif
    return r;
}
