/* q_repl — see q_repl.h.  Shared by the `q` binary and the qcmd test runner.
 *
 * Two console modes share one line-processing helper (run_one_line):
 *   - piped / redirected stdin (echo != 0): the original fgets loop, kept
 *     byte-for-byte identical so the qcmd transcript tests stay stable.
 *   - interactive TTY (echo == 0): real-reuses rayforce's line editor
 *     (ray_term_*) for history, inline editing, Ctrl-R search and bracket
 *     handling, plus a q-correct syntax highlighter installed through the
 *     pluggable ray_term_set_highlighter() hook. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/q_repl.h"
#include "qlang/q_parse.h"
#include "qlang/q_fmt.h"
#include "app/term.h"       /* ray_term_* line editor + highlighter hook */
#include "lang/eval.h"      /* ray_eval */
#include "lang/env.h"       /* ray_env_has_name — live env-derived name highlight */
#include "ops/ops.h"        /* ray_is_lazy, ray_lazy_materialize */
#include <rayforce.h>
#include <stdlib.h>         /* getenv */
#include <string.h>

/* ===== q syntax highlighter (matches ray_highlight_fn) =====
 *
 * Colours q — not rayfall — syntax: backtick symbols, `/` end-of-line
 * comments (q's comment char; `;` is a q separator, NOT a comment), "..."
 * strings, numeric literals and q verbs/keywords.  Every write is bounded
 * so it can never run past dst_cap. */

#define QHL_KEYWORD  "\033[1;32m"        /* green  — verbs/keywords          */
#define QHL_STRING   "\033[1;33m"        /* yellow — "..." string literals   */
#define QHL_COMMENT  "\033[1;38;5;8m"    /* gray   — / comment to EOL        */
#define QHL_SYMBOL   "\033[1;38;5;118m"  /* salad  — `sym backtick symbols   */
#define QHL_NUMBER   "\033[1;38;5;208m"  /* orange — numeric literals        */
#define QHL_OP       "\033[1;38;5;39m"   /* blue   — operators/adverbs       */
#define QHL_RESET    "\033[0m"

/* Verbs and builtins are NOT hardcoded: a word highlights green iff it is a
 * real bound name in the live eval env (ray_env_has_name), exactly as
 * rayforce's own term_highlight_into does.  This tracks reality with zero
 * maintenance — as q verbs get bound they light up automatically, and unbound
 * words stay uncoloured (an honest "won't resolve" signal).
 *
 * The one thing the env can't supply is q's pure SQL *statement* keywords,
 * which are syntax rather than functions.  Those are the only hardcoded set. */
static const char* const Q_SQL_WORDS[] = {
    "select", "exec", "update", "delete", "from", "by",
};

static int q_is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int q_is_digit(char c) { return c >= '0' && c <= '9'; }

static int q_is_op(char c) {
    return strchr(":+-*%!&|<>=~,^#_$?@.", c) != NULL && c != '\0';
}

static int q_is_keyword(const char* w, int32_t len) {
    /* qSQL statement keywords (pure syntax, not env functions) ... */
    for (size_t i = 0; i < sizeof(Q_SQL_WORDS) / sizeof(Q_SQL_WORDS[0]); i++) {
        if ((int32_t)strlen(Q_SQL_WORDS[i]) == len &&
            memcmp(Q_SQL_WORDS[i], w, (size_t)len) == 0)
            return 1;
    }
    /* ... everything else: green iff it is a real bound name in the eval env. */
    return ray_env_has_name(w, (int64_t)len);
}

int32_t q_highlight(char* dst, int32_t dst_cap, const char* buf, int32_t buf_len,
                    int32_t match_pos1, int32_t match_pos2) {
    (void)match_pos1;
    (void)match_pos2;
    int32_t n = 0;

#define QHL_PUT(s, slen) do {                                         \
        int32_t sl_ = (int32_t)(slen);                                \
        if (n + sl_ < dst_cap) { memcpy(dst + n, (s), (size_t)sl_); n += sl_; } \
    } while (0)
#define QHL_LIT(s) QHL_PUT((s), (int32_t)strlen(s))

    for (int32_t i = 0; i < buf_len; ) {
        char c = buf[i];

        /* `/` comment — q treats `/` as a comment to end of line when it
         * starts a line or is preceded by whitespace; otherwise it is an
         * operator/adverb (divide, over). */
        if (c == '/' && (i == 0 || buf[i - 1] == ' ' || buf[i - 1] == '\t')) {
            int32_t j = i;
            while (j < buf_len && buf[j] != '\n')
                j++;
            QHL_LIT(QHL_COMMENT);
            QHL_PUT(buf + i, j - i);
            QHL_LIT(QHL_RESET);
            i = j;
            continue;
        }

        /* Backtick symbol: `sym or ` on its own. */
        if (c == '`') {
            int32_t j = i + 1;
            if (j < buf_len && buf[j] == ':') /* `:handle */
                j++;
            while (j < buf_len && (q_is_word(buf[j]) || buf[j] == '.' || buf[j] == ':'))
                j++;
            QHL_LIT(QHL_SYMBOL);
            QHL_PUT(buf + i, j - i);
            QHL_LIT(QHL_RESET);
            i = j;
            continue;
        }

        /* "..." string literal (with backslash escapes). */
        if (c == '"') {
            int32_t j = i + 1;
            while (j < buf_len) {
                if (buf[j] == '\\' && j + 1 < buf_len) {
                    j += 2;
                    continue;
                }
                if (buf[j] == '"') {
                    j++;
                    break;
                }
                j++;
            }
            QHL_LIT(QHL_STRING);
            QHL_PUT(buf + i, j - i);
            QHL_LIT(QHL_RESET);
            i = j;
            continue;
        }

        /* Numeric literal: a digit, or a leading `.` before a digit.  The
         * scan pulls in the usual q numeric tails (dot, exponent, and the
         * type-suffix letters h/i/j/e/f/p/n/z/u/v/t/b) so 2019.01m, 1.5e3
         * and 42j read as one token. */
        int num_start = q_is_digit(c) ||
                        (c == '.' && i + 1 < buf_len && q_is_digit(buf[i + 1]));
        int prev_word = (i > 0 && q_is_word(buf[i - 1]));
        if (num_start && !prev_word) {
            int32_t j = i + 1;
            while (j < buf_len) {
                char d = buf[j];
                if (q_is_digit(d) || d == '.' || strchr("hijefpnzuvtb", d))
                    j++;
                else if ((d == 'e' || d == 'E') && j + 1 < buf_len &&
                         (buf[j + 1] == '+' || buf[j + 1] == '-'))
                    j += 2;
                else
                    break;
            }
            QHL_LIT(QHL_NUMBER);
            QHL_PUT(buf + i, j - i);
            QHL_LIT(QHL_RESET);
            i = j;
            continue;
        }

        /* Word: keyword/verb (green) or plain identifier. */
        if (q_is_word(c) && !q_is_digit(c)) {
            int32_t j = i + 1;
            while (j < buf_len && q_is_word(buf[j]))
                j++;
            int32_t wlen = j - i;
            if (q_is_keyword(buf + i, wlen)) {
                QHL_LIT(QHL_KEYWORD);
                QHL_PUT(buf + i, wlen);
                QHL_LIT(QHL_RESET);
            } else {
                QHL_PUT(buf + i, wlen);
            }
            i = j;
            continue;
        }

        /* Standalone operator / adverb char. */
        if (q_is_op(c)) {
            QHL_LIT(QHL_OP);
            QHL_PUT(&c, 1);
            QHL_LIT(QHL_RESET);
            i++;
            continue;
        }

        /* Anything else: pass through verbatim. */
        QHL_PUT(&c, 1);
        i++;
    }

#undef QHL_PUT
#undef QHL_LIT
    return n;
}

/* ===== Shared line processing =====
 *
 * Parse + evaluate + print a single input line.  Used verbatim by both the
 * piped and the interactive loops so their observable behaviour is identical
 * (same parse/eval/materialize/format pipeline, same error text). */
static void run_one_line(const char* s, size_t n, FILE* out, FILE* err) {
    if (n == 0)
        return;

    ray_t* ast = q_parse(s);
    if (RAY_IS_ERR(ast)) {
        fprintf(err, "parse error\n");
        return;
    }

    ray_t* r = ray_eval(ast);
    ray_release(ast);
    if (ray_is_lazy(r))
        r = ray_lazy_materialize(r);

    if (RAY_IS_ERR(r)) {
        const char* code = (const char*)r->sdata;
        fprintf(err, "error: %s\n", (code && *code) ? code : "eval");
        return;
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

/* Locate the q history file: $HOME/.qhist, or a bare ".qhist" in the CWD
 * when $HOME is unset.  Returns a pointer into the caller-supplied buffer. */
static const char* q_hist_path(char* buf, size_t cap) {
    const char* home = getenv("HOME");
    if (home && *home) {
        int len = snprintf(buf, cap, "%s/.qhist", home);
        if (len > 0 && (size_t)len < cap)
            return buf;
    }
    return ".qhist";
}

/* Interactive TTY loop — mirrors the proven fallback branch of rayforce's
 * run_interactive (src/app/repl.c) but drives the q pipeline. */
static void q_repl_interactive(FILE* out, FILE* err) {
    ray_term_t* t = ray_term_create();
    if (!t) {
        fprintf(err, "q: terminal init failed\n");
        return;
    }

    char hist_buf[4096];
    const char* hist_path = q_hist_path(hist_buf, sizeof hist_buf);
    ray_hist_load(&t->hist, hist_path);
    ray_term_set_highlighter(t, q_highlight);
    ray_term_set_prompt(t, "q)", 2);   /* exact kdb-style prompt, no glyph */

    ray_term_begin(t);
    for (;;) {
        int64_t sz = ray_term_getc(t);
        if (sz <= 0) {
            if (sz == -2) {
                /* SIGINT: clear interrupt, reset the line, re-prompt. */
                ray_term_clear_interrupt();
                ray_eval_clear_interrupt();
                t->comp_cycling = 0;
                t->esc_state = 0;
                t->buf_len = 0;
                t->buf_pos = 0;
                t->multiline_len = 0;
                fputs("^C\n", out);
                fflush(out);
                ray_term_prompt(t);
                continue;
            }
            break; /* EOF */
        }

        ray_t* line = ray_term_feed(t);
        if (line == RAY_TERM_EOF)
            break;
        if (!line)
            continue;

        const char* str = ray_str_ptr(line);
        size_t len = ray_str_len(line);

        if ((len == 2 && memcmp(str, "\\\\", 2) == 0) ||
            (len == 4 && memcmp(str, "exit", 4) == 0)) {
            ray_release(line);
            break;
        }

        run_one_line(str, len, out, err);
        ray_release(line);
        ray_term_begin(t);
    }

    ray_hist_save(&t->hist, hist_path);
    ray_term_destroy(t);
}

void q_repl_run(FILE* in, FILE* out, FILE* err, int echo) {
    /* Interactive TTY (echo == 0): reuse rayforce's line editor. */
    if (echo == 0) {
        q_repl_interactive(out, err);
        return;
    }

    /* Piped / redirected stdin: original fgets loop, kept byte-for-byte
     * identical so the qcmd transcript tests stay stable. */
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

        run_one_line(line, n, out, err);
    }
}
