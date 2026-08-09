/* qdoc — see qdoc.h. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/repl/qdoc.h"
#include "qlang/parse/q_parse.h"
#include "qlang/q_ctx.h"    /* q_ctx_lang_scan/_tree — x) transcript lines */
#include "qlang/eval/q_eval.h"   /* q_eval — THE eval pipeline */
#include "qlang/eval/q_dbg.h"    /* statement stash — .Q.trp/.Q.bt frame [0] */
#include "qlang/eval/q_view.h"   /* q_view_intercept — `x::e` at the row seam */
#include "qlang/q_fmt.h"
#include "qlang/q_console.h"
#include "qlang/base/q_err.h"    /* q_err_text / q_err_drop — error text + backstop */
#include "qlang/ops/q_sys.h"    /* q_sys_is_cmd / q_sys_line / q_sys_prompt */
#include "ops/ops.h"        /* ray_is_lazy, ray_lazy_materialize */
#include "store/fileio.h"   /* ray_mkdir_p — --emit mirrors the source tree */
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

/* --emit sink: the ACTUAL output of every MISMATCHING example, written as a
 * .qcmd-shaped transcript at <dir>/<path>.  Opened lazily on the first
 * mismatch, so a file that matches everywhere writes nothing and simply
 * DISAPPEARS from the emit tree — an empty diff means nothing moved.
 * Partial by construction (mismatches only): a diff register, not a runnable
 * transcript. */
typedef struct { const char* dir; const char* path; FILE* f; int dead; } qd_emit_t;

static void emit_row(qd_emit_t* em, const char* prompt, const char* input,
                     const char* actual) {
    if (!em || !em->dir || em->dead) return;
    if (!em->f) {
        char full[PATH_MAX];
        if (snprintf(full, sizeof full, "%s/%s", em->dir, em->path) >= (int)sizeof full) {
            em->dead = 1;
            return;
        }
        char* slash = strrchr(full, '/');
        if (slash) { *slash = '\0'; (void)ray_mkdir_p(full); *slash = '/'; }
        if (!(em->f = fopen(full, "w"))) { em->dead = 1; return; }
    }
    fprintf(em->f, "%s%s\n", (prompt && *prompt) ? prompt : "q)", input);
    if (actual && *actual) {
        char na[QD_OUT];
        normalize(actual, na, sizeof na);   /* the form the gate compares */
        if (*na) fprintf(em->f, "%s\n", na);
    }
}

/* Error rows emit kdb's own display (`'type`) so the transcript stays q. */
static void emit_err(qd_emit_t* em, const char* prompt, const char* input,
                     ray_t* err) {
    char buf[256];
    int64_t tn = 0;
    const char* text = err ? q_err_text(err, &tn) : NULL;
    snprintf(buf, sizeof buf, "'%.*s",
             (text && tn) ? (int)tn : 5, (text && tn) ? text : "error");
    emit_row(em, prompt, input, buf);
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
 * not support yet, so trailing lines are ignored by construction.
 *
 * Matching contract (see error_row_matches):
 *   - DEFAULT is STRICT: the error TEXT (payload or class, q_err_text) must
 *     equal the word after the quote.  This is the project thesis ("error
 *     text match kdb"); a row expecting 'type no longer passes on 'name.
 *   - `'error` is the sanctioned ANY-ERROR wildcard: kdb has no class named
 *     `error`, so a row whose first line is exactly `'error` matches ANY error.
 *     Use it for honest "this errors, class not doc-determinable" claims.
 *   - Bare `'` (no class word) also matches any error — it asserts nothing to
 *     check against.
 *   - QDOC_LENIENT_ERRORS=1 restores the old any-error behaviour for debugging.
 *
 * Returns 1 and fills cls[] (may be empty) when the row is an error
 * expectation. */
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

static int lenient_errors(void) {
    const char* e = getenv("QDOC_LENIENT_ERRORS");
    return e && *e && *e != '0';
}

/* Match an actual error object against an error-expectation row: the FULL
 * error text (q_err_text: payload / class), exactly — 'missingName rows can
 * only pass when the name itself is signalled. */
static int error_row_matches(ray_t* err, const char* cls) {
    if (lenient_errors()) return 1;                     /* debug escape: any error */
    if (!cls[0]) return 1;                              /* bare `'`: any error */
    if (strcmp(cls, "error") == 0) return 1;            /* `'error` wildcard */
    int64_t tn = 0;
    const char* text = q_err_text(err, &tn);
    return text && (size_t)tn == strlen(cls) && memcmp(text, cls, (size_t)tn) == 0;
}

/* Run one example; update result; report on failure when verbose. */
static void classify(qdoc_result_t* r, int ok) {
    if (ok) r->passed++; else r->failed++;
}

static void run_example(const char* input, const char* expect,
                        const char* tprompt, qdoc_mode_t mode,
                        int verbose, FILE* out, const char* path,
                        qdoc_result_t* r, qd_emit_t* em) {
    r->examples++;
    q_console_reset();   /* drop any show/0N! output from a prior example */

    /* Prompt pin: the transcript's prompt (`q)` / `q.foo)`) must match the
     * LIVE context prompt at this point — that is what tests the `\d` prompt
     * without a pty.  Mismatch fails the row; the input still executes so
     * the rest of the transcript stays in sync. */
    int prompt_ok = 1;
    if (tprompt && *tprompt) {
        char live[80];
        q_sys_prompt(live, sizeof live);
        prompt_ok = (strcmp(tprompt, live) == 0);
    }

    /* `\`-system-command rows bypass the parser, like the REPL — same shared
     * q_sys_line glue, no runner-side policy: this runtime never enables the
     * process capability, so `\\`/`exit` rows are silent no-ops (the runner
     * survives) and an unknown `\ls`/`\curl` row is silent, never a real shell. */
    if (q_sys_is_cmd(input, strlen(input))) {
        char got[QD_OUT];
        ray_t* sr = q_sys_line(input, strlen(input), 1, got, sizeof got);
        r->parsed++;
        if (mode == QDOC_PARSE_ONLY) {
            if (sr) { q_err_drop(); ray_error_free(sr); }
            classify(r, prompt_ok);
            return;
        }
        char errcls[64];
        int want_error = expect_is_error(expect, errcls, sizeof errcls);
        char gotcls[72];
        int  was_err = 0;
        int ok;
        if (sr) {
            ok = want_error && error_row_matches(sr, errcls);
            int64_t tn = 0;
            const char* text = q_err_text(sr, &tn);
            snprintf(gotcls, sizeof gotcls, "'%.*s",
                     (text && tn) ? (int)tn : 5, (text && tn) ? text : "error");
            was_err = 1;
            snprintf(got, sizeof got, "<error>");
            ray_error_free(sr);
        } else if (want_error) {
            ok = 0;
        } else {
            char ng[QD_OUT], ne[QD_OUT];
            normalize(got, ng, sizeof ng);
            normalize(expect, ne, sizeof ne);
            ok = (strcmp(ng, ne) == 0);
        }
        ok = ok && prompt_ok;
        classify(r, ok);
        if (!ok && prompt_ok) emit_row(em, tprompt, input, was_err ? gotcls : got);
        if (!ok && verbose)
            fprintf(out, "  q)%.200s\n    FAIL(syscmd%s) got \"%.200s\" want \"%.200s\"\n",
                    input, prompt_ok ? "" : ":prompt", got, expect);
        return;
    }

    /* Transcript prompt out of sync with the live context: fail the row but
     * still execute the input so later rows see the intended state. */
    if (!prompt_ok) {
        const char* xs = input; size_t xn = strlen(input);
        char xl = q_ctx_lang_scan(&xs, &xn);
        ray_t* past = xn == 0 ? NULL
                    : (xl && xl != 'q') ? q_ctx_lang_tree(xl, xs, (int64_t)xn)
                                        : q_parse(xs);
        if (past && !RAY_IS_ERR(past)) {
            r->parsed++;
            ray_t* pres;
            if (!q_view_intercept(past, input, &pres)) pres = q_eval(past);
            ray_release(pres);
            ray_release(past);
        }
        classify(r, 0);
        if (verbose)
            fprintf(out, "  %s%.200s\n    FAIL(prompt) transcript prompt \"%s\" != live context\n",
                    tprompt, input, tprompt);
        return;
    }

    const char* ls = input; size_t ln = strlen(input);
    char lang = q_ctx_lang_scan(&ls, &ln);
    if (lang && ln == 0) {                 /* bare `g)` line: silent no-op */
        r->parsed++;
        int lok = expect[0] == '\0';
        classify(r, lok);
        if (!lok) {
            emit_row(em, tprompt, input, "");
            if (verbose) fprintf(out, "  q)%.200s\n    FAIL(eval) got \"\" want \"%.200s\"\n",
                                 input, expect);
        }
        return;
    }
    ray_t* ast = (lang && lang != 'q') ? q_ctx_lang_tree(lang, ls, (int64_t)ln)
                                       : q_parse(ls);
    if (RAY_IS_ERR(ast)) {
        /* an error-expectation row a PARSE-time error satisfies passes WHOLE —
         * 'dup dies during parse (qsql.md:168), and the transcript's
         * observable IS the error */
        char pcls[64];
        if (mode != QDOC_PARSE_ONLY && expect_is_error(expect, pcls, sizeof pcls) &&
            error_row_matches(ast, pcls)) {
            ray_error_free(ast);
            r->parsed++;
            classify(r, 1);
            return;
        }
        classify(r, 0);
        emit_err(em, tprompt, input, ast);
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
    char errcls[64];
    int want_error = expect_is_error(expect, errcls, sizeof errcls);
    ray_t* res;
    int is_assign = 1;                     /* a view definition prints nothing */
    if (!q_view_intercept(ast, input, &res)) {
        is_assign = q_parse_is_assign(ast);
        res = q_eval(ast);
    }
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
        emit_err(em, tprompt, input, res);
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
        emit_row(em, tprompt, input, got_ok);
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
        emit_row(em, tprompt, input, got);
        if (verbose)
            fprintf(out, "  q)%.200s\n    FAIL(eval) got \"%.200s\" want \"%.200s\"\n",
                    input, ng, ne);
    }
}

static qdoc_result_t run_path(const char* path, qdoc_mode_t mode,
                              int verbose, FILE* out, qd_emit_t* em) {
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

/* console 0: qdoc examples must never suspend (the runner has no reader) */
#define FLUSH() do { if (have) { int dtok = q_dbg_statement_begin(input, strlen(input), 0); \
                                 run_example(input, expect, tprompt, mode, verbose, out, path, &r, em); \
                                 q_dbg_statement_end(dtok); \
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
            /* whole-line q comments (`/ …`) are transcript prose, never
             * expected output — q output never emits them (observed mirror:
             * zero `/ ` lines; a BARE `/` is real output — iterator display) */
            if (line[0] == '/' && (line[1] == ' ' || line[1] == '\t'))
                continue;
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

/* Suites whose output IS a clock or an allocation counter: they differ every
 * run, so emitting them would leave the committed tree permanently dirty and
 * train readers to ignore it.  Nothing diffable was ever in them. */
static const char* const emit_volatile[] = {
    "test/q/dotq/bv.qcmd",      "test/q/dotq/dpts.qcmd",
    "test/q/dotq/fc.qcmd",      "test/q/phrases/temp.qcmd",
    "test/q/syscmd/o.qcmd",
};

static int is_volatile(const char* path) {
    for (size_t i = 0; i < sizeof emit_volatile / sizeof *emit_volatile; i++)
        if (ends_with(path, emit_volatile[i])) return 1;
    return 0;
}

/* Mirror relative to the corpus root, so test/q/list/take.qcmd emits at
 * <dir>/list/take.qcmd.  Non-corpus targets (docs) keep their path. */
static const char* emit_rel(const char* path) {
    const char* p = strstr(path, "test/q/");
    return p ? p + 7 : path;
}

qdoc_result_t q_qdoc_run_file_emit(const char* path, qdoc_mode_t mode,
                                 int verbose, FILE* out, const char* emit_dir) {
    if (!emit_dir || !*emit_dir || is_volatile(path))
        return run_path(path, mode, verbose, out, NULL);
    qd_emit_t em = { .dir = emit_dir, .path = emit_rel(path) };
    qdoc_result_t r = run_path(path, mode, verbose, out, &em);
    if (em.f) fclose(em.f);
    return r;
}
