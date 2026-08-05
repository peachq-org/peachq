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
#include "qlang/q_prim.h"         /* q_str_text_bytes — the remote value-apply head */
#include "qlang/ops/q_sys.h"      /* q_sys_is_cmd / q_sys_line / q_system_fn — the `\`-command door */
#include "qlang/ops/q_index.h"    /* q_index_elem_at — the element-read home */
#include "lang/eval.h"            /* ray_eval_is_interrupted, ray_eval_set_remote_*_fn */
#include "mem/heap.h"             /* ray_heap_gc — the `\g 1` statement-end collect */
#include "mem/sys.h"              /* ray_sys_alloc — remote-eval scratch */
#include "ops/ops.h"              /* ray_is_lazy, ray_lazy_materialize */
#include "app/term.h"             /* ray_term_interrupted */
#include <rayforce.h>
#include <stdlib.h>
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

/* GC policy has ONE home: this statement seam, covering all three doors
 * (run_line, remote_eval_str, remote_apply).  `\g 0` (deferred, the kdb
 * default — basics/syscmds.md#g-garbage-collection-mode) collects NOTHING
 * here, so the default path is byte-for-byte the pre-`\g` behaviour; `\g 1`
 * (immediate) collects after every statement.  ray_heap_gc self-gates its
 * cross-heap passes on ray_parallel_flag == 0, and a statement end is that
 * state by construction. */
static void ctx_statement_end(void) {
    if (q_sys_gc_mode()) ray_heap_gc();
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
int q_ctx_run_line(const char* s, size_t n, FILE* out, FILE* err,
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
        return 0;

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
        ctx_statement_end();   /* a `\g 1` line collects at its own end (kdb: set runs gc) */
        return 0;
    }

    ray_t* ast = q_parse(s);
    if (RAY_IS_ERR(ast)) {
        int64_t tn = 0;
        const char* text = q_err_text(ast, &tn);   /* 'dup dies at parse (qsql.md:168) */
        fprintf(err, "error: %.*s\n",
                (text && tn) ? (int)tn : 5, (text && tn) ? text : "parse");
        q_err_drop();
        int code = ast->aux[0] ? (int)ast->aux[0] : (int)QE_PARSE + 1;
        ray_error_free(ast);
        return code;
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
        ctx_statement_end();
        return 0;
    }

    if (RAY_IS_ERR(r)) {
        int64_t tn = 0;
        const char* text = q_err_text(r, &tn);
        fprintf(err, "error: %.*s\n",
                (text && tn) ? (int)tn : 4, (text && tn) ? text : "eval");
        q_err_drop();
        ray_error_free(r);
        ctx_statement_end();
        return 0;
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
    ctx_statement_end();
    return 0;
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

    /* a statement that fails to PARSE aborts the LOAD (kdb stops a script at
     * the error; qsql.md:168's 'dup fires here, so a bad file never runs the
     * statements after it).  Eval errors keep the historic continue. */
    int lrc = 0;
    #define FLUSH() do { if (alen) { lrc = q_ctx_run_line(acc, alen, out, err, 0); alen = 0; acc[0] = '\0'; } } while (0)

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
        if (!is_cont) { FLUSH(); if (lrc) break; }  /* fresh logical line: eval the prior one */

        /* append this physical line (join continuation fragments with '\n') */
        if (alen && alen + 1 < sizeof acc) acc[alen++] = '\n';
        size_t room = (alen < sizeof acc) ? sizeof acc - 1 - alen : 0;
        if (n > room) n = room;                   /* truncate pathological long line, never overflow */
        memcpy(acc + alen, line, n);
        alen += n;
        acc[alen] = '\0';
    }
    if (!lrc) FLUSH();                             /* eval any pending logical line (incl. before a lone \) */
    #undef FLUSH

    fclose(f);
    return lrc ? 1 + lrc : 0;
}

/* ===== The remote doors (see q_ctx.h) =====
 *
 * q_ctx_run_line's pipeline, disposing of the result over the wire instead of
 * to `out`; errors propagate as owned values the IPC layer serializes as -128h.
 * Console output drains to the SERVER's stdout, then resets so it cannot bleed
 * into the next request. */
static ray_t* remote_eval_str(const char* src, size_t len) {
    /* A leading `\` is a system command, not q source (kdb runs a solo `\l`/`\p`
     * received on the wire).  `system"X"` is exactly `\X`, so strip and reuse
     * q_system_fn — one home for q_sys_run, the restricted gate, `\`-shell capture. */
    if (len > 0 && src[0] == '\\') {
        ray_t* arg = ray_str(src + 1, len - 1);
        if (!arg) return q_err(QE_OOM);
        ray_t* r = q_system_fn(arg);
        ray_release(arg);
        { const char* con = q_console_str();
          if (con && *con) fputs(con, stdout);
          q_console_reset(); }
        ctx_statement_end();
        return r;
    }
    char* tmp = (char*)ray_sys_alloc(len + 1);
    if (!tmp) return q_err(QE_OOM);
    memcpy(tmp, src, len);
    tmp[len] = '\0';
    ray_t* ast = q_parse(tmp);
    if (RAY_IS_ERR(ast)) { ray_sys_free(tmp); return ast; }
    /* A trailing assignment answers with the generic null (basics/ipc.md:
     * `h"fn:{2+x}"` displays nothing).  Remote source text is a STATEMENT, so a
     * view definition is intercepted here exactly as a typed line would be. */
    ray_t* r;
    int is_assign = 1;
    if (!q_view_intercept(ast, tmp, &r)) {
        is_assign = q_parse_is_assign(ast);
        r = q_eval(ast);
    }
    ray_sys_free(tmp);
    ray_release(ast);
    if (ray_is_lazy(r))
        r = ray_lazy_materialize(r);
    { const char* con = q_console_str();
      if (con && *con) fputs(con, stdout);
      q_console_reset(); }
    ctx_statement_end();
    if (is_assign && !RAY_IS_ERR(r)) {   /* an error still propagates (-128h) */
        ray_release(r);
        ray_retain(RAY_NULL_OBJ);
        return RAY_NULL_OBJ;
    }
    return r;
}

/* The kdb value/apply wire shape — NOT a statement: ONE list-apply of the head to
 * already-evaluated tail args, never re-evaluating them (ADR-0004: value, not
 * eval), through the one public apply entry.  A sym/string head resolves/parses to
 * its value first.  `list` is BORROWED (the ipc layer releases it); the result is
 * OWNED.  Restricted mode needs no re-assert: ipc_dispatch sets it around the
 * whole dispatch. */
static ray_t* remote_apply(ray_t* list) {
    if (!list || (list->type != RAY_LIST && !ray_is_vec(list)) ||
        ray_len(list) < 1)
        return q_err(QE_TYPE);
    int64_t n = ray_len(list);
    ray_t* head = q_index_elem_at(list, 0);            /* owned */
    if (!head || RAY_IS_ERR(head)) return head ? head : q_err(QE_TYPE);
    if (head->type == -RAY_STR || head->type == RAY_CHARV) {   /* "+" / "{x*2}" */
        const char* sp; int64_t sn;
        if (q_str_text_bytes(head, &sp, &sn)) {
            char* z = malloc((size_t)sn + 1);
            if (!z) { ray_release(head); return q_err(QE_WSFULL); }
            memcpy(z, sp, (size_t)sn);
            z[sn] = '\0';
            ray_t* ast = q_parse(z);
            free(z);
            ray_release(head);
            if (RAY_IS_ERR(ast)) return ast;
            head = q_eval(ast);
            ray_release(ast);
            if (RAY_IS_ERR(head)) return head;
        }
    } else if (head->type == -RAY_SYM) {   /* `sum -> its value, registry-first */
        ray_t* v = q_eval_value_wrap(head);
        if (RAY_IS_ERR(v)) { ray_release(head); return v; }
        ray_release(head);
        head = v;
    }
    ray_t* argv[8];
    int64_t argc = n - 1;
    if (argc > 8) { ray_release(head); return q_err(QE_RANK); }
    for (int64_t i = 0; i < argc; i++) {
        argv[i] = q_index_elem_at(list, i + 1);        /* owned */
        if (!argv[i] || RAY_IS_ERR(argv[i])) {
            ray_t* err = argv[i];
            for (int64_t j = 0; j < i; j++) ray_release(argv[j]);
            ray_release(head);
            return err ? err : q_err(QE_TYPE);
        }
    }
    ray_t* r;
    if (argc == 0) {                                      /* (f) -> f[] = f@:: */
        ray_t* nil = RAY_NULL_OBJ;
        r = q_eval_apply_value(head, &nil, 1);
    } else {
        r = q_eval_apply_value(head, argv, argc);
    }
    if (r && ray_is_lazy(r)) r = ray_lazy_materialize(r);
    for (int64_t j = 0; j < argc; j++) ray_release(argv[j]);
    ray_release(head);
    ctx_statement_end();
    return r;
}

void q_ctx_install_remote_hooks(void) {
    ray_eval_set_remote_str_fn(remote_eval_str);
    ray_eval_set_remote_apply_fn(remote_apply);
}

/* A `\p N` (or startup `-p`) listener makes this process a server even if it
 * began as a client: like rayforce/kdb, once it has a listener it keeps serving
 * past stdin EOF instead of exiting.  Set by the `\p` handler (ops/q_sys.c).
 * Platform-neutral (a plain flag), declared BEFORE the poll-only guard so the
 * POSIX event loop, the Windows serial path, and common code all see it. */
static int g_listener_active = 0;
void q_ctx_mark_listener_active(void) { g_listener_active = 1; }
int  q_ctx_listener_active(void)      { return g_listener_active; }
