/* q_ctx — see q_ctx.h.  The engine context: the statement seam every door
 * shares, and the process state that outlives whichever door set it.  Split out
 * of q_repl.c (2026-08-02) — ops/ and parse/ both needed `\l`, and reaching
 * into repl/ for it made a verb depend on the front end. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/q_ctx.h"
#include "qlang/base/q_err.h"     /* q_err_text / q_err_drop — the statement-entry backstop */
#include "qlang/parse/q_parse.h"
#include "qlang/eval/q_eval.h"
#include "qlang/eval/q_view.h"    /* q_view_intercept — `x::e` at the line seam */
#include "qlang/q_fmt.h"
#include "qlang/q_console.h"
#include "qlang/ops/q_sys.h"      /* q_sys_is_cmd / q_sys_line — the `\`-command door */
#include "lang/eval.h"            /* ray_eval_is_interrupted */
#include "ops/ops.h"              /* ray_is_lazy, ray_lazy_materialize */
#include "app/term.h"             /* ray_term_interrupted */
#include <rayforce.h>
#include <string.h>

/* The front end's terminal teardown, if one registered.  See q_ctx.h. */
static void (*g_console_close)(void);

void q_ctx_set_console_close(void (*fn)(void)) { g_console_close = fn; }
void q_ctx_console_close(void) { if (g_console_close) g_console_close(); }

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
const char* q_ctx_strip_prompt(const char* s) {
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
void q_ctx_run_line(const char* s, size_t n, FILE* out, FILE* err,
                    int print_result) {
    /* Drop any pasted `q)` console prompt(s) before anything else sees the
     * line — covers REPL (piped + interactive) and the `q file.q` loader, all
     * of which funnel through here.  Adjust n by the bytes we advanced past. */
    {
        const char* stripped = q_ctx_strip_prompt(s);
        n -= (size_t)(stripped - s);
        s = stripped;
    }
    if (n == 0)
        return;

    q_err_drop();   /* statement-entry error-payload backstop (q_err.c head) */

    /* `\`-system-command line: the shared q_sys glue renders console side
     * effects + value into buf (value-or-throw; `\\`/exit act inside q_sys).
     * Console lines arrive '\n'-terminated, a rendered value does not — the
     * append-if-missing keeps this byte-identical to the historic output. */
    if (q_sys_is_cmd(s, n)) {
        char buf[8192];
        ray_t* sr = q_sys_line(s, n, print_result, buf, sizeof buf);
        if (buf[0]) {
            fputs(buf, out);
            if (buf[strlen(buf) - 1] != '\n') fputc('\n', out);
        }
        if (sr) {
            int64_t tn = 0;
            const char* text = q_err_text(sr, &tn);
            fprintf(err, "error: %.*s\n",
                    (text && tn) ? (int)tn : 6, (text && tn) ? text : "syscmd");
            q_err_drop();
            ray_error_free(sr);
        }
        fflush(out);
        return;
    }

    ray_t* ast = q_parse(s);
    if (RAY_IS_ERR(ast)) {
        fprintf(err, "parse error\n");
        ray_error_free(ast);
        return;
    }

    ray_t* r;
    int is_assign = 1;                     /* a view definition prints nothing */
    if (!q_view_intercept(ast, s, &r)) {
        is_assign = q_parse_is_assign(ast);
        r = q_eval(ast);
    }
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
        int64_t tn = 0;
        const char* text = q_err_text(r, &tn);
        fprintf(err, "error: %.*s\n",
                (text && tn) ? (int)tn : 4, (text && tn) ? text : "eval");
        q_err_drop();
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

int q_ctx_run_file(const char* path, FILE* out, FILE* err) {
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
     *    the script (load-time syntax); `\\` / `exit x` evaluate normally and
     *    terminate the PROCESS via q_sys_exit (kdb-true).
     * Continuation fragments are joined with '\n' (now whitespace to the
     * scanner), so each fragment's trailing `/ comment` ends at its own newline. */
    static char acc[1 << 16];               /* one logical line (joined) */
    size_t      alen = 0;
    int         in_block = 0;
    char        line[4096];

    #define FLUSH() do { if (alen) { q_ctx_run_line(acc, alen, out, err, 0); alen = 0; acc[0] = '\0'; } } while (0)

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

/* A `\p N` (or startup `-p`) listener makes this process a server even if it
 * began as a client: like rayforce/kdb, once it has a listener it keeps serving
 * past stdin EOF instead of exiting.  Set by the `\p` handler (ops/q_sys.c).
 * Platform-neutral (a plain flag), declared BEFORE the poll-only guard so the
 * POSIX event loop, the Windows serial path, and common code all see it. */
static int g_listener_active = 0;
void q_ctx_mark_listener_active(void) { g_listener_active = 1; }
int  q_ctx_listener_active(void)      { return g_listener_active; }
