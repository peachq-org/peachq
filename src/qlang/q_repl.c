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
#include "qlang/q_ns.h"       /* q_ns_prompt — namespaces */
#include "qlang/q_sys.h"      /* q_sys_dispatch — `\`-command dispatcher */
#include "app/term.h"       /* ray_term_* line editor + highlighter hook */
#include "core/poll.h"      /* ray_poll_* — concurrent REPL + IPC event loop */
#include "lang/eval.h"      /* ray_eval */
#include "lang/env.h"       /* ray_env_has_name — live env-derived name highlight */
#include "ops/ops.h"        /* ray_is_lazy, ray_lazy_materialize */
#include <rayforce.h>
#include <stdlib.h>         /* getenv */
#include <string.h>
#include <errno.h>
#include <unistd.h>         /* read, STDIN_FILENO — poll-driven stdin
                             * (mingw-w64 provides both; the CRT read() on fd 0
                             * covers pipes/files, and the tty flavour reads
                             * through ray_term_getc, never this fd) */

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

/* Strip pasted kdb `q)` REPL prompts from the front of an intake line.
 *
 * kdb lets you paste a transcript line that still carries its `q)` console
 * prompt and it "just works" — the line-reader drops a leading `q)` before
 * parsing.  We mirror that here, and ONLY here (the line-intake layer), never
 * in q_parse: the strip is a console/loader affordance, not a language feature,
 * so a `q)` inside a lambda body, a string literal, or an argument to
 * `parse`/`value` must survive untouched — and it does, because run_one_line
 * only ever sees whole top-level intake lines (openq is line-at-a-time).
 *
 * Rule: repeated exact `q)` only.  The `s[2] != ')'` guard is what tells a
 * repeated prompt (`q)q)…`, strip) apart from the debug prompt (`q))…`, leave
 * alone).  Namespace prompts (`q.foo)`) and `k)` mode fail the `s[1] == ')'` /
 * `s[0] == 'q'` tests and are likewise left alone.  No leading-whitespace trim:
 * an indented line is not a prompt.  Returns the advanced pointer.
 *
 * Reads are in-bounds on any NUL-terminated string: s[1] is only reached when
 * s[0]=='q' (so s[0] != '\0'), and s[2] only when s[1]==')' (so s[1] != '\0'). */
const char* q_strip_repl_prompt(const char* s) {
    while (s[0] == 'q' && s[1] == ')' && s[2] != ')')
        s += 2;
    return s;
}

/* ===== Shared line processing =====
 *
 * Parse + evaluate + print a single input line.  Used verbatim by both the
 * piped and the interactive loops so their observable behaviour is identical
 * (same parse/eval/materialize/format pipeline, same error text). */
/* print_result: when non-zero (REPL) a non-null, non-assignment result is
 * q-formatted to `out` (console auto-display).  When zero (script load) the
 * result is discarded — kdb scripts are silent except explicit side-effects
 * (show / 0N! / console writes), which still flush below. */
static void run_one_line(const char* s, size_t n, FILE* out, FILE* err,
                         int print_result) {
    /* Drop any pasted `q)` console prompt(s) before anything else sees the
     * line — covers REPL (piped + interactive) and the `q file.q` loader, all
     * of which funnel through here.  Adjust n by the bytes we advanced past. */
    {
        const char* stripped = q_strip_repl_prompt(s);
        n -= (size_t)(stripped - s);
        s = stripped;
    }
    if (n == 0)
        return;

    /* q system commands (\d \v \f \a — q_ns.c) run before the parser; an
     * unhandled \cmd falls through to the historic path.  This is the REPL
     * adapter over the mode-less core: it OWNS the interactive policy —
     * `\\` really quits, bare `\` is a not-yet-implemented q/k toggle, and an
     * unknown `\foo` shells out returning its raw system(3) status. */
    {
        q_sys_result d = q_sys_dispatch(s, n);
        int handled = (d.kind != QS_NOT_CMD);
        ray_t* sr = NULL;
        switch (d.kind) {
        case QS_QUIT:    exit(0);                             /* real quit — kdb-true */
        case QS_TOGGLE:  sr = ray_error("nyi", NULL); break;  /* q/k toggle NYI */
        case QS_UNKNOWN: sr = q_sys_shell(d.shell, d.shell_len); break;
        case QS_VALUE:   sr = d.val; break;
        case QS_NOT_CMD: break;
        }
        if (handled) {
            if (sr && RAY_IS_ERR(sr)) {
                const char* code = (const char*)sr->sdata;
                fprintf(err, "error: %s\n", (code && *code) ? code : "syscmd");
                ray_error_free(sr);
            } else if (sr) {
                if (print_result && !RAY_IS_NULL(sr)) {
                    char buf[8192];
                    q_fmt_console(sr, buf, sizeof buf);   /* obey \c on display */
                    fputs(buf, out);
                    fputc('\n', out);
                }
                ray_release(sr);
            }
            fflush(out);
            return;
        }
    }

    ray_t* ast = q_parse(s);
    if (RAY_IS_ERR(ast)) {
        fprintf(err, "parse error\n");
        ray_error_free(ast);
        return;
    }

    int is_assign = q_ast_is_assign(ast);   /* pre-lower shape */
    ast = q_lower(ast);
    if (RAY_IS_ERR(ast)) {
        const char* code = (const char*)ast->sdata;
        fprintf(err, "error: %s\n", (code && *code) ? code : "lower");
        ray_release(ast);
        return;
    }
    ray_t* r = ray_eval(ast);
    ray_release(ast);
    if (ray_is_lazy(r))
        r = ray_lazy_materialize(r);

    /* flush any show/0N! side-effect display captured during eval */
    { const char* con = q_console_str();
      if (con && *con) fputs(con, out);
      q_console_reset(); }

    /* Mirror repl.c's post-eval contract: a Ctrl-C that landed during eval
     * means "stop" even when a non-polling C kernel absorbed it and the
     * eval completed with a normal result — the result is discarded, never
     * printed (repl.c prints ^C there; q reports the qdocs 'stop error:
     * "Current operation stopped due to user interrupt (Ctrl-c)").  The
     * polling paths surface the same flag as a 'limit error, so this one
     * check covers both. */
    if (ray_eval_is_interrupted() || ray_term_interrupted()) {
        ray_eval_clear_interrupt();
        ray_term_clear_interrupt();
        if (RAY_IS_ERR(r)) ray_error_free(r); else ray_release(r);
        fprintf(err, "error: stop\n");
        return;
    }

    if (RAY_IS_ERR(r)) {
        const char* code = (const char*)r->sdata;
        fprintf(err, "error: %s\n", (code && *code) ? code : "eval");
        ray_error_free(r);
        return;
    }
    /* q console silence: a (last-statement) assignment prints nothing; a
     * script load (print_result == 0) prints no result at all. */
    if (print_result && !RAY_IS_NULL(r) && !is_assign) {
        char buf[8192];
        q_fmt_console(r, buf, sizeof buf);   /* obey \c on auto-echo display */
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
/* q REPL is line-at-a-time, exactly like kdb's `q)` console: every Return
 * submits.  This replaces rayforce's bracket-continuation counter, whose
 * `;`-as-line-comment rule (correct for rayfall/lisp) mis-flagged q's `;`
 * separator inside parens as an open expression — e.g. `(1 2 3;4 5)` dropped
 * into a `…` continuation prompt instead of evaluating.  Returning 0 means
 * "never incomplete". */
static int32_t q_no_continuation(const char* mbuf, int32_t mbuf_len,
                                 const char* buf, int32_t buf_len) {
    (void)mbuf; (void)mbuf_len; (void)buf; (void)buf_len;
    return 0;
}

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
    ray_term_set_continuation_fn(t, q_no_continuation);  /* kdb: line-at-a-time */

    /* SIGINT/console-ctrl plumbing (mirrors rayforce's repl.c contract):
     * at the prompt interrupts stay raw keypresses (0x03 clears the line);
     * ray_term_eval_begin below arms them only for the eval window. */
    ray_term_install_signals(t);

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

        /* Interrupt window: Ctrl-C becomes SIGINT (POSIX, ISIG) or
         * CTRL_C_EVENT (Windows, processed input) ONLY while eval runs;
         * both set the eval-interrupt flag run_one_line reports as 'stop.
         * Keep this bracket tight — no early exits between begin and end. */
        ray_term_clear_interrupt();
        ray_eval_clear_interrupt();
        ray_term_eval_begin(t);
        run_one_line(str, len, out, err, 1);
        ray_term_eval_end(t);
        ray_release(line);
        /* `\d` may have switched context: refresh the prompt (q.foo). */
        {
            char prompt[80];
            int pl = q_ns_prompt(prompt, sizeof prompt);
            ray_term_set_prompt(t, prompt, pl);
        }
        ray_term_begin(t);
    }

    ray_hist_save(&t->hist, hist_path);
    ray_term_destroy(t);
}

/* ===== Poll-driven REPL (concurrent console + IPC) =====
 *
 * Mirrors rayforce's own run_interactive (src/app/repl.c ~919): stdin is
 * registered as a selector on the SAME poll that carries the IPC listener,
 * so one single-threaded ray_poll_run services keystrokes AND client
 * sockets — a client round-trips while the console sits at its prompt.
 *
 * Two stdin flavours share one context:
 *   - tty:   ray_term_getc/feed per byte (read_fn) + line dispatch (data_fn),
 *            byte-for-byte the q_repl_interactive behaviour.
 *   - piped: a line accumulator over plain read(2); each complete line is
 *            processed with the same prompt/echo shape as the fgets loop so
 *            the transcript is unchanged.
 * `\\` / `exit` exit the poll loop (kdb: process exit).  EOF keeps the loop
 * serving IPC when a listener is live (the daemon shape), else exits. */

/* A `\p N` (or startup `-p`) listener makes this process a server even if it
 * began as a client: like rayforce/kdb, once it has a listener it keeps serving
 * past stdin EOF instead of exiting.  Set by the `\p` handler (q_sys.c).
 * Platform-neutral (a plain flag), declared BEFORE the poll-only guard so the
 * POSIX event loop, the Windows serial path, and common code all see it. */
static int g_listener_active = 0;
void q_repl_mark_listener_active(void) { g_listener_active = 1; }
int  q_repl_listener_active(void)      { return g_listener_active; }

typedef struct {
    ray_term_t* term;            /* tty console; NULL in piped mode / after teardown */
    FILE*       out;
    FILE*       err;
    int         have_listener;   /* EOF → keep serving instead of exiting */
    int         eof_done;        /* stdin EOF handled once (EPOLLIN and/or EPOLLHUP) */
    char        hist_path[4108];
    /* piped mode */
    int         echo;
    size_t      acc_len;         /* bytes accumulated toward the next line */
    char        acc[4096];       /* mirrors the fgets loop's 4096 line buffer */
} q_poll_repl_t;

static q_poll_repl_t g_q_poll_repl;

/* Restore the terminal + save history exactly once (idempotent). */
static void q_poll_close_term(q_poll_repl_t* c) {
    if (!c->term)
        return;
    ray_hist_save(&c->term->hist, c->hist_path);
    ray_term_destroy(c->term);
    c->term = NULL;
}

/* --- tty flavour: same callbacks shape as repl.c's repl_read/repl_on_data --- */

static ray_t* q_poll_tty_read(ray_poll_t* poll, ray_selector_t* sel) {
    q_poll_repl_t* c = (q_poll_repl_t*)sel->data;
    ray_term_t*    t = c->term;

    int64_t sz = ray_term_getc(t);
    if (sz <= 0) {
        if (sz == -2) {
            /* SIGINT at the prompt: clear the line, re-prompt (repl.c contract). */
            ray_term_clear_interrupt();
            ray_eval_clear_interrupt();
            t->comp_cycling = 0;
            t->esc_state = 0;
            t->buf_len = 0;
            t->buf_pos = 0;
            t->multiline_len = 0;
            fputs("^C\n", c->out);
            fflush(c->out);
            ray_term_prompt(t);
            return NULL;
        }
        goto eof;
    }

    {
        ray_t* line = ray_term_feed(t);
        if (line == RAY_TERM_EOF)
            goto eof;
        return line;   /* complete line (or NULL: keep accumulating) */
    }

eof:
    /* Ctrl-D: restore the terminal; with a live listener keep serving IPC
     * clients (the historic REPL-then-serve shape), else exit the loop. */
    q_poll_close_term(c);
    if (c->have_listener)
        ray_poll_deregister(poll, sel->id);
    else
        ray_poll_exit(poll, 0);
    return NULL;
}

static ray_t* q_poll_tty_data(ray_poll_t* poll, ray_selector_t* sel, void* data) {
    q_poll_repl_t* c = (q_poll_repl_t*)sel->data;
    ray_t* line = (ray_t*)data;

    const char* str = ray_str_ptr(line);
    size_t      len = ray_str_len(line);

    if (len == 0) {
        ray_release(line);
        ray_term_begin(c->term);
        return NULL;
    }

    if ((len == 2 && memcmp(str, "\\\\", 2) == 0) ||
        (len == 4 && memcmp(str, "exit", 4) == 0)) {
        ray_release(line);
        q_poll_close_term(c);
        ray_poll_exit(poll, 0);
        return NULL;
    }

    /* Interrupt window: identical bracket to q_repl_interactive — Ctrl-C is
     * SIGINT only while the eval runs; run_one_line reports it as 'stop. */
    ray_term_clear_interrupt();
    ray_eval_clear_interrupt();
    ray_term_eval_begin(c->term);
    run_one_line(str, len, c->out, c->err, 1);
    ray_term_eval_end(c->term);
    ray_release(line);

    /* `\d` may have switched context: refresh the prompt (q.foo). */
    {
        char prompt[80];
        int  pl = q_ns_prompt(prompt, sizeof prompt);
        ray_term_set_prompt(c->term, prompt, pl);
    }
    ray_term_begin(c->term);
    return NULL;
}

/* --- piped flavour: fgets-loop transcript over poll-driven read(2) --- */

static void q_pipe_prompt(q_poll_repl_t* c) {
    char prompt[80];
    q_ns_prompt(prompt, sizeof prompt);
    fputs(prompt, c->out);
    fflush(c->out);
}

/* Process one complete piped line (prompt already showing, mirrors the fgets
 * loop's prompt-then-read order).  Returns 1 when the line requests exit. */
static int q_pipe_line(q_poll_repl_t* c, char* line, size_t n) {
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    line[n] = '\0';

    if (c->echo) {
        fputs(line, c->out);
        fputc('\n', c->out);
    }

    if (n) {
        if (!strcmp(line, "\\\\") || !strcmp(line, "exit"))
            return 1;
        run_one_line(line, n, c->out, c->err, 1);
    }
    q_pipe_prompt(c);
    return 0;
}

/* Single-home stdin-EOF handling.  Reached from BOTH a draining read()==0
 * (EPOLLIN) and a bare EPOLLHUP (an empty pipe whose writer closed reports HUP
 * with NO EPOLLIN, so the read_fn never runs — see q_poll_stdin_hup).  Flush any
 * partial final line, then a CLIENT (no listener) exits the poll loop while a
 * SERVER deregisters stdin and keeps serving IPC.  Idempotent via eof_done so an
 * EPOLLIN|EPOLLHUP event can't double-process (double prompt / double-free). */
static void q_poll_stdin_eof(ray_poll_t* poll, ray_selector_t* sel, q_poll_repl_t* c) {
    if (c->eof_done) return;
    c->eof_done = 1;
    int quit = 0;
    if (c->acc_len) {   /* final line without a trailing newline */
        size_t n = c->acc_len;
        c->acc_len = 0;
        quit = q_pipe_line(c, c->acc, n);
    }
    if (!quit) {        /* fgets loop prints '\n' after the EOF prompt */
        fputc('\n', c->out);
        fflush(c->out);
    }
    if (quit || (!c->have_listener && !g_listener_active))
        ray_poll_exit(poll, 0);
    else
        ray_poll_deregister(poll, sel->id);   /* keep serving IPC (has a listener) */
}

/* stdin hangup handler (registered as the stdin selector's error_fn).  The
 * frozen epoll loop's default HUP action is a bare deregister — for a client
 * that strands ray_poll_run with poll->code still < 0 (an idle hang, the whole
 * point of Bundle 3).  Route HUP through the same EOF path instead. */
static void q_poll_stdin_hup(ray_poll_t* poll, ray_selector_t* sel) {
    if (sel && sel->data) q_poll_stdin_eof(poll, sel, (q_poll_repl_t*)sel->data);
}

static ray_t* q_poll_pipe_read(ray_poll_t* poll, ray_selector_t* sel) {
    q_poll_repl_t* c = (q_poll_repl_t*)sel->data;
    char tmp[1024];

    /* One read per readable event: the fd is blocking, and a single read
     * after EPOLLIN/EVFILT_READ never blocks; level-triggered polling
     * re-fires while more input is pending. */
    ssize_t rd = read((int)sel->fd, tmp, sizeof tmp);
    if (rd < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return NULL;
        rd = 0;   /* real error → treat as EOF */
    }

    if (rd == 0) {
        q_poll_stdin_eof(poll, sel, c);
        return NULL;
    }

    for (ssize_t i = 0; i < rd; i++) {
        /* Overlong line (no newline within the buffer): flush it as its own
         * line — the same split the fgets loop's 4096 buffer produced. */
        if (tmp[i] == '\n' || c->acc_len >= sizeof(c->acc) - 1) {
            if (tmp[i] != '\n')
                c->acc[c->acc_len++] = tmp[i];
            size_t n = c->acc_len;
            c->acc_len = 0;
            if (q_pipe_line(c, c->acc, n)) {
                ray_poll_exit(poll, 0);
                return NULL;
            }
        } else {
            c->acc[c->acc_len++] = tmp[i];
        }
    }
    return NULL;
}

int q_repl_run_poll(ray_poll_t* poll, FILE* out, FILE* err,
                    int stdin_tty, int have_listener) {
    q_poll_repl_t* c = &g_q_poll_repl;
    memset(c, 0, sizeof *c);
    c->out = out;
    c->err = err;
    c->have_listener = have_listener;

    ray_poll_reg_t reg = {0};
    reg.fd       = STDIN_FILENO;
    reg.type     = RAY_SEL_STDIN;
    reg.data     = c;
    reg.error_fn = q_poll_stdin_hup;   /* HUP → same EOF path (else a client hangs) */

    if (stdin_tty) {
        ray_term_t* t = ray_term_create();
        if (!t) {
            fprintf(err, "q: terminal init failed\n");
            return -1;
        }
        /* Same setup as q_repl_interactive: q history, q highlighter, kdb
         * `q)` prompt, line-at-a-time (no continuation), SIGINT plumbing. */
        char hist_buf[4096];
        const char* hp = q_hist_path(hist_buf, sizeof hist_buf);
        snprintf(c->hist_path, sizeof c->hist_path, "%s", hp);
        ray_hist_load(&t->hist, c->hist_path);
        ray_term_set_highlighter(t, q_highlight);
        ray_term_set_prompt(t, "q)", 2);
        ray_term_set_continuation_fn(t, q_no_continuation);
        ray_term_install_signals(t);
        c->term = t;
        reg.read_fn = q_poll_tty_read;
        reg.data_fn = q_poll_tty_data;
    } else {
        c->echo = 1;   /* piped transcript: echo input after the prompt */
        reg.read_fn = q_poll_pipe_read;
    }

    if (ray_poll_register(poll, &reg) < 0) {
        /* Backend can't watch this stdin (e.g. epoll + regular-file
         * redirect): restore the terminal and let the caller fall back. */
        if (c->term) {
            ray_term_destroy(c->term);
            c->term = NULL;
        }
        return -1;
    }

    if (c->term)
        ray_term_begin(c->term);   /* draw the first prompt */
    else
        q_pipe_prompt(c);

    ray_poll_run(poll);

    /* Loop exited with the console still live (e.g. a remote-initiated
     * exit): restore the terminal before returning. */
    q_poll_close_term(c);
    return 0;
}

void q_repl_run(FILE* in, FILE* out, FILE* err, int echo) {
    /* Interactive TTY (echo == 0): reuse rayforce's line editor. */
    if (echo == 0) {
        q_repl_interactive(out, err);
        return;
    }

    /* Piped / redirected stdin: original fgets loop, kept byte-for-byte
     * identical so the qcmd transcript tests stay stable.  The prompt is
     * context-derived: `q)` at root, `q.foo)` after `\d .foo`. */
    char line[4096];

    for (;;) {
        char prompt[80];
        q_ns_prompt(prompt, sizeof prompt);
        fputs(prompt, out);
        fflush(out);

        if (!fgets(line, sizeof line, in)) { fputc('\n', out); break; }

        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';

        if (echo) { fputs(line, out); fputc('\n', out); }

        if (n == 0) continue;
        if (!strcmp(line, "\\\\") || !strcmp(line, "exit")) break;

        run_one_line(line, n, out, err, 1);
    }
}

/* Run a q startup script (`q file.q`): evaluate each line with NO `q)` prompt,
 * NO input echo, and NO auto-display of top-level results — only explicit
 * console side-effects (show / 0N!) reach `out`, matching kdb script-load
 * semantics.  Line-at-a-time (multi-line constructs are a follow-on).  Returns
 * 0 on success, non-zero if the file could not be opened. */
int q_repl_run_file(const char* path, FILE* out, FILE* err) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(err, "q: cannot open script '%s'\n", path);
        return 1;
    }

    /* kdb script semantics (learn/startingkdb/language.md):
     *  - an INDENTED line CONTINUES the previous logical line;
     *  - blank lines, whitespace-only lines, and comment lines (trimmed first
     *    char '/') are IGNORED for continuation — they do NOT flush the
     *    accumulator (so `a:1 2` <blank> `/c` <blank> ` 3` ` + 4` => a:5 6 7);
     *  - a trimmed singleton `/` opens a `/`..`\` block comment (skip to a
     *    trimmed singleton `\`); a trimmed singleton `\` (outside a block) EXITS
     *    the script; `\\` / `exit` also stop the load.
     * Continuation fragments are joined with '\n' (now whitespace to the
     * scanner), so each fragment's trailing `/ comment` ends at its own newline. */
    static char acc[1 << 16];               /* one logical line (joined) */
    size_t      alen = 0;
    int         in_block = 0;
    char        line[4096];

    #define FLUSH() do { if (alen) { run_one_line(acc, alen, out, err, 0); alen = 0; acc[0] = '\0'; } } while (0)

    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        /* Strip TRAILING whitespace too, so a block delimiter with superfluous
         * blanks (`/   ` / `\   `) still classifies as a singleton and a code
         * line's insignificant trailing spaces don't skew anything (kdb ignores
         * superfluous blanks — language.md).  Trailing spaces inside a string
         * literal are safe: such a line ends with `"`, not whitespace. */
        while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';

        /* trimmed view (leading whitespace skipped) drives classification */
        size_t lead = 0;
        while (lead < n && (line[lead] == ' ' || line[lead] == '\t')) lead++;
        const char* trim = line + lead;
        size_t      tlen = n - lead;
        int indented = (lead > 0);

        if (in_block) {                          /* inside a /..\ block comment */
            if (tlen == 1 && trim[0] == '\\') in_block = 0;   /* singleton \ closes; no flush */
            continue;
        }
        if (tlen == 0) continue;                 /* blank/whitespace-only: ignored, no flush */
        if (tlen == 1 && trim[0] == '/') { in_block = 1; continue; }   /* open block; no flush */
        if (tlen == 1 && trim[0] == '\\') break; /* singleton \ exits (post-loop FLUSH runs)  */
        if (!strcmp(trim, "\\\\") || !strcmp(trim, "exit")) break;      /* \\ / exit           */
        if (trim[0] == '/') continue;            /* comment-only line: ignored, no flush        */

        int is_cont = indented && alen > 0;
        if (!is_cont) FLUSH();                    /* a fresh logical line: eval the prior one   */

        /* append this physical line (join continuation fragments with '\n') */
        if (alen && alen + 1 < sizeof acc) acc[alen++] = '\n';
        size_t room = (alen < sizeof acc) ? sizeof acc - 1 - alen : 0;
        if (n > room) n = room;                   /* truncate pathological long line, never overflow */
        memcpy(acc + alen, line, n);
        alen += n;
        acc[alen] = '\0';
    }
    FLUSH();                                       /* eval any pending logical line (incl. before a lone \) */
    #undef FLUSH

    fclose(f);
    return 0;
}
