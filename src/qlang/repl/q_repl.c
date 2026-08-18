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

#include "qlang/repl/q_repl.h"
#include "qlang/q_ctx.h"        /* the statement + console-teardown seams */
#include "qlang/base/q_err.h"   /* q_err_text — full error text for console display */
#include "qlang/parse/q_parse.h"
#include "qlang/eval/q_eval.h"   /* q_eval — THE eval pipeline */
#include "qlang/eval/q_dbg.h"    /* debug-loop line readers (basics/debug.md) */
#include "qlang/eval/q_view.h"   /* q_view_intercept — `x::e` at the line seam */
#include "qlang/q_fmt.h"
#include "qlang/q_console.h"
#include "qlang/ops/q_sys.h"      /* q_sys_is_cmd / q_sys_line / q_sys_prompt / q_sys_listen_port */
#include "app/term.h"       /* ray_term_* line editor + highlighter hook */
#include "core/poll.h"      /* ray_poll_* — concurrent REPL + IPC event loop */
#include "lang/eval.h"      /* ray_eval_is_interrupted */
#include "lang/env.h"       /* ray_env_has_name — live env-derived name highlight */
#include "ops/ops.h"        /* ray_is_lazy, ray_lazy_materialize */
#include <rayforce.h>
#include <ctype.h>          /* isalpha/isalnum — the hint's name-token scan */
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

static int is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_op(char c) {
    return strchr(":+-*%!&|<>=~,^#_$?@.", c) != NULL && c != '\0';
}

static int is_keyword(const char* w, int32_t len) {
    /* qSQL statement keywords (pure syntax, not env functions) ... */
    for (size_t i = 0; i < sizeof(Q_SQL_WORDS) / sizeof(Q_SQL_WORDS[0]); i++) {
        if ((int32_t)strlen(Q_SQL_WORDS[i]) == len &&
            memcmp(Q_SQL_WORDS[i], w, (size_t)len) == 0)
            return 1;
    }
    /* ... everything else: green iff it is a real bound name in the eval env. */
    return ray_env_has_name(w, (int64_t)len);
}

static int32_t repl_highlight(char* dst, int32_t dst_cap, const char* buf, int32_t buf_len,
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
            while (j < buf_len && (is_word(buf[j]) || buf[j] == '.' || buf[j] == ':'))
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
        int num_start = is_digit(c) ||
                        (c == '.' && i + 1 < buf_len && is_digit(buf[i + 1]));
        int prev_word = (i > 0 && is_word(buf[i - 1]));
        if (num_start && !prev_word) {
            int32_t j = i + 1;
            while (j < buf_len) {
                char d = buf[j];
                if (is_digit(d) || d == '.' || strchr("hijefpnzuvtb", d))
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
        if (is_word(c) && !is_digit(c)) {
            int32_t j = i + 1;
            while (j < buf_len && is_word(buf[j]))
                j++;
            int32_t wlen = j - i;
            if (is_keyword(buf + i, wlen)) {
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
        if (is_op(c)) {
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



/* Locate the q history file: $HOME/.qhist, or a bare ".qhist" in the CWD
 * when $HOME is unset.  Returns a pointer into the caller-supplied buffer. */
static const char* i_hist_path(char* buf, size_t cap) {
    const char* home = getenv("HOME");
    if (home && *home) {
        int len = snprintf(buf, cap, "%s/.qhist", home);
        if (len > 0 && (size_t)len < cap)
            return buf;
    }
    return ".qhist";
}

/* The live non-poll interactive terminal (repl_interactive), so q_sys_exit can
 * restore it + save history from inside an eval (q_ctx_console_close).  The
 * poll flavour's terminal lives in g_q_poll_repl and is closed there. */
static ray_term_t* g_live_term;
static char g_live_hist_path[4108];

/* Interactive TTY loop — mirrors the proven fallback branch of rayforce's
 * run_interactive (src/app/repl.c) but drives the q pipeline. */
/* q REPL is line-at-a-time, exactly like kdb's `q)` console: every Return
 * submits.  This replaces rayforce's bracket-continuation counter, whose
 * `;`-as-line-comment rule (correct for rayfall/lisp) mis-flagged q's `;`
 * separator inside parens as an open expression — e.g. `(1 2 3;4 5)` dropped
 * into a `…` continuation prompt instead of evaluating.  Returning 0 means
 * "never incomplete". */
static int32_t no_continuation(const char* mbuf, int32_t mbuf_len,
                                 const char* buf, int32_t buf_len) {
    (void)mbuf; (void)mbuf_len; (void)buf; (void)buf_len;
    return 0;
}

static int repl_tty_dbg_read(const char* prompt, char* buf, size_t cap);

/* Autosuggest: a submitted line that was exactly one name token with a
 * .help one-liner arms the next prompt's hint — `?name / <one-liner>`, Tab/→
 * accepting just the `?name` part.  The lookup is LOCAL (.help.oneline; no
 * network) and absent .help (classic, no `\l pq`) simply never answers, so
 * the prompt stays clean.  Anything else disarms. */
static void repl_update_hint(ray_term_t* t, const char* s, size_t n) {
    ray_term_set_hint(t, NULL, 0);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\n')) n--;
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    if (n == 0 || n > 64) return;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '.')) return;
    for (size_t i = 1; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '_')) return;
    char src[96];
    int  sl = snprintf(src, sizeof src, ".help.oneline\"%.*s\"", (int)n, s);
    ray_t* ast = q_parse(src);
    (void)sl;
    if (RAY_IS_ERR(ast)) { ray_release(ast); return; }
    ray_t* r = q_eval(ast);
    ray_release(ast);
    if (!RAY_IS_ERR(r) && r && r->type == RAY_CHARV && ray_len(r) > 0) {
        char hint[256];
        int  cmd = snprintf(hint, sizeof hint, "?%.*s", (int)n, s);
        snprintf(hint + cmd, sizeof hint - (size_t)cmd, " / %.*s",
                 (int)ray_len(r), (const char*)ray_data(r));
        ray_term_set_hint(t, hint, cmd);
    }
    ray_release(r);
}

/* First-session teach: an empty history means a fresh user — arm the empty-
 * prompt ghost with the help door (Tab/→ accepts `?`).  Modern only (classic
 * has no .help); the first evaluated line replaces or clears it. */
static void repl_teach_hint(ray_term_t* t) {
    if (t->hist.count == 0 && q_console_pipe_on())
        ray_term_set_hint(t, "? / help", 1);
}

static void repl_interactive(FILE* out, FILE* err) {
    ray_term_t* t = ray_term_create();
    if (!t) {
        fprintf(err, "q: terminal init failed\n");
        return;
    }

    char hist_buf[4096];
    const char* hist_path = i_hist_path(hist_buf, sizeof hist_buf);
    snprintf(g_live_hist_path, sizeof g_live_hist_path, "%s", hist_path);
    g_live_term = t;
    ray_hist_load(&t->hist, hist_path);
    ray_term_set_highlighter(t, repl_highlight);
    ray_term_set_prompt(t, "q)", 2);   /* exact kdb-style prompt, no glyph */
    ray_term_set_continuation_fn(t, no_continuation);  /* kdb: line-at-a-time */

    /* SIGINT/console-ctrl plumbing (mirrors rayforce's repl.c contract):
     * at the prompt interrupts stay raw keypresses (0x03 clears the line);
     * ray_term_eval_begin below arms them only for the eval window. */
    ray_term_install_signals(t);
    q_dbg_set_reader(repl_tty_dbg_read);   /* `\e 1` debugger over this editor */

    repl_teach_hint(t);
    ray_term_begin(t);
    if (t->hint_len > 0)
        ray_term_redraw(t);   /* paint the teach ghost before the first keypress */
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

        /* Interrupt window: Ctrl-C becomes SIGINT (POSIX, ISIG) or
         * CTRL_C_EVENT (Windows, processed input) ONLY while eval runs;
         * both set the eval-interrupt flag run_one_line reports as 'stop.
         * Keep this bracket tight — no early exits between begin and end. */
        ray_term_clear_interrupt();
        ray_eval_clear_interrupt();
        ray_term_eval_begin(t);
        q_ctx_run_line(str, len, out, err, 1);
        ray_term_eval_end(t);
        repl_update_hint(t, str, len);
        ray_release(line);
        /* `\d` may have switched context: refresh the prompt (q.foo). */
        {
            char prompt[80];
            int pl = q_sys_prompt(prompt, sizeof prompt);
            ray_term_set_prompt(t, prompt, pl);
        }
        ray_term_begin(t);
        if (t->hint_len > 0)
            ray_term_redraw(t);   /* paint the hint before the first keypress */
    }

    q_dbg_set_reader(NULL);
    ray_hist_save(&t->hist, hist_path);
    ray_term_destroy(t);
    g_live_term = NULL;
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
 *            byte-for-byte the repl_interactive behaviour.
 *   - piped: a line accumulator over plain read(2); each complete line is
 *            processed with the same prompt/echo shape as the fgets loop so
 *            the transcript is unchanged.
 * `\\` / `exit x` terminate inside the eval (q_sys_exit — kdb: process exit).
 * With a listener live, a tty ^D KEEPS the console (soft eof) and a real stdin
 * EOF keeps serving with stdin dropped; with none, either ends the session. */


typedef struct {
    ray_term_t* term;            /* tty console; NULL in piped mode / after teardown */
    FILE*       out;
    FILE*       err;
    int         eof_done;        /* stdin EOF handled once (EPOLLIN and/or EPOLLHUP) */
    char        hist_path[4108];
    /* piped mode */
    int         echo;
    size_t      acc_len;         /* bytes accumulated toward the next line */
    char        acc[4096];       /* mirrors the fgets loop's 4096 line buffer */
} q_poll_repl_t;

static q_poll_repl_t g_q_poll_repl;

/* Restore the terminal + save history exactly once (idempotent). */
static void poll_close_term(q_poll_repl_t* c) {
    if (!c->term)
        return;
    ray_hist_save(&c->term->hist, c->hist_path);
    ray_term_destroy(c->term);
    c->term = NULL;
}

/* The console teardown registered into q_ctx by the entry points below:
 * whichever REPL flavour holds a live terminal, restore it and save history
 * BEFORE `.z.exit` runs (its 0N! output must land on a cooked terminal).
 * Idempotent; no-op when piped. */
static void repl_console_close(void) {
    poll_close_term(&g_q_poll_repl);
    if (g_live_term) {
        ray_hist_save(&g_live_term->hist, g_live_hist_path);
        ray_term_destroy(g_live_term);
        g_live_term = NULL;
    }
}

/* A stdin that is really GONE (a closed pipe, a dead tty): a LIVE `\p` listener
 * keeps the process serving with stdin dropped from the poll, else the session
 * ends.  Must stay a QUERY — a snapshot or a latch regresses to #40/#41. */
static void poll_serve_or_exit(ray_poll_t* poll, int64_t sel_id) {
    if (q_sys_listen_port() > 0)
        ray_poll_deregister(poll, sel_id);
    else
        ray_poll_exit(poll, 0);
}

/* --- tty flavour: same callbacks shape as repl.c's repl_read/repl_on_data --- */

static ray_t* poll_tty_read(ray_poll_t* poll, ray_selector_t* sel) {
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
        goto stdin_gone;   /* the tty itself died — re-prompting would spin */
    }

    {
        ray_t* line = ray_term_feed(t);
        if (line != RAY_TERM_EOF)
            return line;   /* complete line (or NULL: keep accumulating) */
    }

    /* ^D is a SOFT eof — a byte, not a closed fd, so the tty stays readable and a
     * serving process keeps its console rather than being stranded with a live
     * listener and no reader (feed already echoed the newline; `\\`/`exit` stay the
     * real quit).  ^D ONLY: Windows idle-prompt ^C shares this sentinel to QUIT. */
    if (t->input[0] == KEYCODE_CTRL_D && q_sys_listen_port() > 0) {
        ray_term_begin(t);
        return NULL;
    }

stdin_gone:
    poll_close_term(c);
    poll_serve_or_exit(poll, sel->id);
    return NULL;
}

static ray_t* poll_tty_data(ray_poll_t* poll, ray_selector_t* sel, void* data) {
    q_poll_repl_t* c = (q_poll_repl_t*)sel->data;
    ray_t* line = (ray_t*)data;

    const char* str = ray_str_ptr(line);
    size_t      len = ray_str_len(line);

    if (len == 0) {
        ray_release(line);
        ray_term_set_hint(c->term, NULL, 0);   /* Enter dismisses the hint */
        ray_term_begin(c->term);
        return NULL;
    }

    /* Interrupt window: identical bracket to repl_interactive — Ctrl-C is
     * SIGINT only while the eval runs; run_one_line reports it as 'stop. */
    ray_term_clear_interrupt();
    ray_eval_clear_interrupt();
    ray_term_eval_begin(c->term);
    q_ctx_run_line(str, len, c->out, c->err, 1);
    ray_term_eval_end(c->term);
    repl_update_hint(c->term, str, len);
    ray_release(line);

    /* `\d` may have switched context: refresh the prompt (q.foo). */
    {
        char prompt[80];
        int  pl = q_sys_prompt(prompt, sizeof prompt);
        ray_term_set_prompt(c->term, prompt, pl);
    }
    ray_term_begin(c->term);
    if (c->term->hint_len > 0)
        ray_term_redraw(c->term);   /* paint the hint before the first keypress */
    return NULL;
}

/* --- piped flavour: fgets-loop transcript over poll-driven read(2) --- */

static void pipe_prompt(q_poll_repl_t* c) {
    char prompt[80];
    q_sys_prompt(prompt, sizeof prompt);
    fputs(prompt, c->out);
    fflush(c->out);
}

/* Process one complete piped line (prompt already showing, mirrors the fgets
 * loop's prompt-then-read order).  `\\`/`exit x` terminate inside q_sys_exit, so
 * there is no quit signal to propagate. */
static void pipe_line(q_poll_repl_t* c, char* line, size_t n) {
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        n--;
    line[n] = '\0';

    if (c->echo) {
        fputs(line, c->out);
        fputc('\n', c->out);
    }

    if (n)
        q_ctx_run_line(line, n, c->out, c->err, 1);
    pipe_prompt(c);
}

/* Single-home stdin-EOF handling.  Reached from BOTH a draining read()==0
 * (EPOLLIN) and a bare EPOLLHUP (an empty pipe whose writer closed reports HUP
 * with NO EPOLLIN, so the read_fn never runs — see poll_stdin_hup).  Flush any
 * partial final line, then hand the disposition to poll_serve_or_exit.  Idempotent
 * via eof_done so an EPOLLIN|EPOLLHUP event can't double-process (double prompt
 * / double-free). */
static void poll_stdin_eof(ray_poll_t* poll, ray_selector_t* sel, q_poll_repl_t* c) {
    if (c->eof_done) return;
    c->eof_done = 1;
    if (c->acc_len) {   /* final line without a trailing newline */
        size_t n = c->acc_len;
        c->acc_len = 0;
        pipe_line(c, c->acc, n);
    }
    fputc('\n', c->out);   /* fgets loop prints '\n' after the EOF prompt */
    fflush(c->out);
    poll_serve_or_exit(poll, sel->id);
}

/* stdin hangup handler (registered as the stdin selector's error_fn).  The
 * frozen epoll loop's default HUP action is a bare deregister — for a client
 * that strands ray_poll_run with poll->code still < 0 (an idle hang, the whole
 * point of Bundle 3).  Route HUP through the same EOF path instead. */
static void poll_stdin_hup(ray_poll_t* poll, ray_selector_t* sel) {
    if (sel && sel->data) poll_stdin_eof(poll, sel, (q_poll_repl_t*)sel->data);
}

/* Append raw bytes to the line accumulator; returns the count consumed. */
static size_t pipe_acc_push(q_poll_repl_t* c, const char* p, size_t n) {
    size_t room = sizeof(c->acc) - 1 - c->acc_len;
    if (n > room) n = room;
    memcpy(c->acc + c->acc_len, p, n);
    c->acc_len += n;
    return n;
}

/* Pop one complete line off the accumulator FRONT into dst (no trailing
 * newline), or -1 if none is buffered yet.  A full accumulator with no
 * newline flushes as its own line — the same split the fgets loop's 4096
 * buffer produced.  Lines stay IN the accumulator until popped, so a
 * debugger suspension mid-line leaves the rest of the input intact for the
 * nested read loop (repl_poll_dbg_read below). */
static int pipe_pop_line(q_poll_repl_t* c, char* dst, size_t cap) {
    size_t i = 0;
    while (i < c->acc_len && c->acc[i] != '\n') i++;
    int newline = (i < c->acc_len);
    if (!newline && c->acc_len < sizeof(c->acc) - 1) return -1;
    size_t n = i;
    if (n >= cap) n = cap - 1;
    memcpy(dst, c->acc, n);
    dst[n] = '\0';
    size_t consumed = newline ? i + 1 : c->acc_len;
    memmove(c->acc, c->acc + consumed, c->acc_len - consumed);
    c->acc_len -= consumed;
    return (int)n;
}

static ray_t* poll_pipe_read(ray_poll_t* poll, ray_selector_t* sel) {
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
        poll_stdin_eof(poll, sel, c);
        return NULL;
    }

    size_t off = 0;
    while (off < (size_t)rd) {
        off += pipe_acc_push(c, tmp + off, (size_t)rd - off);
        char line[4096];
        int n;
        while ((n = pipe_pop_line(c, line, sizeof line)) >= 0)
            pipe_line(c, line, (size_t)n);
    }
    return NULL;
}

/* piped debug readers own the transcript shape: prompt, then input echo */
static int pipe_dbg_echo(char* buf, int n) {
    if (n < 0) return n;
    fputs(buf, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return n;
}

/* Debug-loop reader for the poll-piped console: drain buffered lines first,
 * then block on stdin directly — the poll loop is parked beneath us on the C
 * stack, so a plain read(2) cannot race it. */
static int repl_poll_dbg_read(const char* prompt, char* buf, size_t cap) {
    fputs(prompt, stdout);
    fflush(stdout);
    q_poll_repl_t* c = &g_q_poll_repl;
    for (;;) {
        int n = pipe_pop_line(c, buf, cap);
        if (n >= 0) {
            while (n && buf[n - 1] == '\r') buf[--n] = '\0';
            return pipe_dbg_echo(buf, n);
        }
        char tmp[1024];
        ssize_t rd = read(STDIN_FILENO, tmp, sizeof tmp);
        if (rd <= 0) return -1;
        size_t off = 0;
        while (off < (size_t)rd)
            off += pipe_acc_push(c, tmp + off, (size_t)rd - off);
    }
}

/* Debug-loop reader for the interactive tty: run the SAME line editor the
 * outer loop uses, nested — flip the terminal back to edit state, draw the
 * debug prompt, feed bytes until a line, then re-arm the eval window before
 * returning (Ctrl-C during the nested EVAL is the interrupt again).  SIGINT
 * at the debug prompt clears the line and re-prompts (the repl.c prompt
 * contract) — it never exits the process and never pops a debug level. */
static int repl_tty_dbg_read(const char* prompt, char* buf, size_t cap) {
    ray_term_t* t = g_live_term ? g_live_term : g_q_poll_repl.term;
    if (!t) return -1;
    ray_term_eval_end(t);
    ray_term_set_prompt(t, prompt, (int32_t)strlen(prompt));
    ray_term_begin(t);
    int out = -1;
    for (;;) {
        int64_t sz = ray_term_getc(t);
        if (sz <= 0) {
            if (sz == -2) {
                ray_term_clear_interrupt();
                ray_eval_clear_interrupt();
                t->comp_cycling = 0;
                t->esc_state = 0;
                t->buf_len = 0;
                t->buf_pos = 0;
                t->multiline_len = 0;
                fputs("^C\n", stdout);
                fflush(stdout);
                ray_term_prompt(t);
                continue;
            }
            break;                                     /* EOF: abort */
        }
        ray_t* line = ray_term_feed(t);
        if (line == RAY_TERM_EOF)
            break;
        if (!line)
            continue;
        size_t n = ray_str_len(line);
        if (n >= cap) n = cap - 1;
        memcpy(buf, ray_str_ptr(line), n);
        buf[n] = '\0';
        ray_release(line);
        out = (int)n;
        break;
    }
    ray_term_clear_interrupt();
    ray_eval_clear_interrupt();
    ray_term_eval_begin(t);
    return out;
}

int q_repl_run_poll(ray_poll_t* poll, FILE* out, FILE* err, int stdin_tty) {
    q_ctx_set_console_close(repl_console_close);
    q_poll_repl_t* c = &g_q_poll_repl;
    memset(c, 0, sizeof *c);
    c->out = out;
    c->err = err;

    ray_poll_reg_t reg = {0};
    reg.fd       = STDIN_FILENO;
    reg.type     = RAY_SEL_STDIN;
    reg.data     = c;
    reg.error_fn = poll_stdin_hup;   /* HUP → same EOF path (else a client hangs) */

    if (stdin_tty) {
        ray_term_t* t = ray_term_create();
        if (!t) {
            fprintf(err, "q: terminal init failed\n");
            return -1;
        }
        /* Same setup as repl_interactive: q history, q highlighter, kdb
         * `q)` prompt, line-at-a-time (no continuation), SIGINT plumbing. */
        char hist_buf[4096];
        const char* hp = i_hist_path(hist_buf, sizeof hist_buf);
        snprintf(c->hist_path, sizeof c->hist_path, "%s", hp);
        ray_hist_load(&t->hist, c->hist_path);
        ray_term_set_highlighter(t, repl_highlight);
        ray_term_set_prompt(t, "q)", 2);
        ray_term_set_continuation_fn(t, no_continuation);
        ray_term_install_signals(t);
        c->term = t;
        reg.read_fn = poll_tty_read;
        q_dbg_set_reader(repl_tty_dbg_read);   /* `\e 1` debugger over this editor */
        reg.data_fn = poll_tty_data;
    } else {
        c->echo = 1;   /* piped transcript: echo input after the prompt */
        reg.read_fn = poll_pipe_read;
        /* piped console: arm the `\e 1` debugger's nested line reader */
        q_dbg_set_reader(repl_poll_dbg_read);
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

    if (c->term) {
        repl_teach_hint(c->term);
        ray_term_begin(c->term);   /* draw the first prompt */
        if (c->term->hint_len > 0)
            ray_term_redraw(c->term);
    } else
        pipe_prompt(c);

    ray_poll_run(poll);

    q_dbg_set_reader(NULL);
    /* Loop exited with the console still live (e.g. a remote-initiated
     * exit): restore the terminal before returning. */
    poll_close_term(c);
    return 0;
}

/* Debug-loop reader for the fgets-piped console: the same FILE* buffering as
 * the outer loop, so the nested read continues exactly where it stopped. */
static int repl_fgets_dbg_read(const char* prompt, char* buf, size_t cap) {
    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(buf, (int)cap, stdin)) return -1;
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return pipe_dbg_echo(buf, (int)n);
}

void q_repl_run(FILE* in, FILE* out, FILE* err, int echo) {
    q_ctx_set_console_close(repl_console_close);
    /* Interactive TTY (echo == 0): reuse rayforce's line editor. */
    if (echo == 0) {
        repl_interactive(out, err);
        return;
    }

    /* Piped / redirected stdin: original fgets loop, kept byte-for-byte
     * identical so the qcmd transcript tests stay stable.  The prompt is
     * context-derived: `q)` at root, `q.foo)` after `\d .foo`. */
    char line[4096];

    if (in == stdin)
        q_dbg_set_reader(repl_fgets_dbg_read);

    for (;;) {
        char prompt[80];
        q_sys_prompt(prompt, sizeof prompt);
        fputs(prompt, out);
        fflush(out);

        if (!fgets(line, sizeof line, in)) { fputc('\n', out); break; }

        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';

        if (echo) { fputs(line, out); fputc('\n', out); }

        if (n == 0) continue;
        q_ctx_run_line(line, n, out, err, 1);
    }
    q_dbg_set_reader(NULL);
}

/* Run a q startup script (`q file.q`): evaluate each line with NO `q)` prompt,
 * NO input echo, and NO auto-display of top-level results — only explicit
 * console side-effects (show / 0N!) reach `out`, matching kdb script-load
 * semantics.  Line-at-a-time (multi-line constructs are a follow-on).  Returns
 * 0 on success, non-zero if the file could not be opened. */
