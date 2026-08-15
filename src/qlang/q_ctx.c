/* q_ctx — see q_ctx.h.  The engine context: the statement seam every door
 * shares, and the process state that outlives whichever door set it.  Split out
 * of q_repl.c (2026-08-02) — ops/ and parse/ both needed `\l`, and reaching
 * into repl/ for it made a verb depend on the front end. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/q_ctx.h"
#include <ctype.h>           /* isalpha — the language-prefix scan */
#include "qlang/base/q_err.h"     /* q_err / q_err_drop — the statement-entry backstop */
#include "qlang/q_comment.h"          /* doc headers: the run above a definition */
#include "qlang/parse/q_parse.h"
#include "qlang/eval/q_eval.h"
#include "qlang/eval/q_dbg.h"     /* statement stash + `\e` trace display */
#include "qlang/eval/q_view.h"    /* q_view_intercept — `x::e` at the line seam */
#include "qlang/q_env.h"          /* q_env_ctx / _set — the load's `\d` save+restore */
#include "qlang/q_fmt.h"
#include "qlang/q_console.h"
#include "qlang/q_prim.h"         /* q_str_text_bytes — the remote value-apply head */
#include "qlang/ops/q_sys.h"      /* q_sys_is_cmd / q_sys_line / q_system_fn — the `\`-command door */
#include "qlang/ops/q_index.h"    /* q_index_elem_at — the element-read home */
#include "qlang/io/q_io.h"        /* q_io_read_slice — THE byte core a load reads through */
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

/* language-handler tree `(.X.e; "rest")`; `q`'s builtin handler is `value`.
 * The head is a NAME ref, so an undefined handler fails as ordinary
 * resolution (`'.g.e`) and a defined one applies through the one seam. */
char q_ctx_lang_scan(const char** s, size_t* n) {
    char lang = 0;
    const char* p = *s;
    size_t m = *n;
    while (m >= 2 && isalpha((unsigned char)p[0]) && p[1] == ')' &&
           !(m >= 3 && p[2] == ')')) {
        lang = p[0];
        p += 2;
        m -= 2;
    }
    *s = p;
    *n = m;
    return lang;
}

ray_t* q_ctx_lang_tree(char letter, const char* p, int64_t n) {
    char nm[5] = { '.', letter, '.', 'e', '\0' };
    ray_t* hd = letter == 'q' ? ray_sym(ray_sym_intern_runtime("value", 5))
                              : ray_sym(ray_sym_intern_runtime(nm, 4));
    ray_t* arg = ray_charv(p, n);
    ray_t* t = ray_list_new(2);
    t = ray_list_append(t, hd);
    ray_release(hd);
    t = ray_list_append(t, arg);
    ray_release(arg);
    return t;
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

/* THE error display: ONE home, ONE form for every statement error — kdb's
 * `'class` line plus the numbered frames (q_dbg_print_trace).  `\e` selects
 * CONTROL FLOW (abort / suspend / collect — basics/debug.md), never rendering,
 * so `\e 0` and `\e 1` show the identical text.  A debugger-reported error is
 * silent (kdb: after `\` the console just returns to its prompt).  Consumes
 * the error. */
static void ctx_show_err(FILE* out, FILE* err, ray_t* e) {
    if (!q_dbg_reported(e)) {
        fflush(out);               /* echo/prompt land before the trace */
        q_dbg_print_trace(err, e);
    }
    q_err_drop();
    ray_error_free(e);
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

/* A load line that ABORTS re-signals to whoever asked for the load: display
 * once at this line, then DETACH the error's identity (else display's drop and
 * q_dbg_statement_end's payload restore would destroy the text); the caller
 * re-signals only after the statement seam has closed. */
static int ctx_load_abort(ray_t* e, FILE* out, FILE* err, q_err_sig_t* t) {
    if (!q_dbg_reported(e)) {
        fflush(out);
        q_dbg_print_trace(err, e);
    }
    q_err_detach(e, t);
    ray_error_free(e);
    return t->aux ? (int)t->aux : (int)QE_VALUE + 1;
}

static void ctx_load_esig(ray_t** esig, q_err_sig_t* t) {
    if (esig) *esig = q_err_resignal(t);
    else if (t->pv) { ray_release(t->pv); t->pv = NULL; }
}

/* in_load: this is a script-runner line — an eval error (or interrupt, or a
 * failing `\`-command such as a nested `\l`) ABORTS the load: non-zero return
 * plus the re-signal in *esig.  Non-load callers keep the historic contract
 * (eval errors report and return 0). */
static int ctx_line(const char* s, size_t n, FILE* out, FILE* err,
                    int print_result, int in_load, ray_t** esig) {
    /* Language-handler prefixes (kdb `x)` lines, owner-ruled 2026-08-07):
     * strip every leading `<letter>)` — pasted prompts included — and the
     * RIGHTMOST letter is the language.  `q` (or none) evaluates as q; any
     * other letter routes the rest VERBATIM to `.<letter>.e`; a bare prefix
     * line is silent.  `q))` stays untouched (the debug prompt). */
    char lang = q_ctx_lang_scan(&s, &n);
    if (n == 0)
        return 0;

    /* `?…` — the help doors (peachq): at the statement head, `?[` alone stays
     * CODE (funsql / vector conditional); ANY other `?` line rewrites to
     * `.help.show"topic"` — `??` to `.help.full"topic"` (the full page) — so
     * glyph topics (`?$`, `?!`) work too.  Both PRINT: `.help.text` is the
     * value ladder and no door returns it.  Topic = the rest of the line minus
     * trailing blanks (a multi-word search is an AND — see .help.find), `"`/`\`
     * escaped into the rewrite; bare `?` sends "", which q answers with the
     * index page.  Intercepted lines are kdb parse-error space,
     * so classic fidelity costs nothing; without `\l pq` the call answers
     * '.help.show, which names the fix. */
    if ((!lang || lang == 'q') && n >= 1 && s[0] == '?') {
        size_t t0 = 1;
        int    full = (t0 < n && s[t0] == '?');
        t0 += full;
        while (t0 < n && (s[t0] == ' ' || s[t0] == '\t')) t0++;
        if (!(t0 < n && s[t0] == '[')) {
            size_t t1 = n;
            while (t1 > t0 && isspace((unsigned char)s[t1 - 1])) t1--;
            char hb[512];
            int  hn = snprintf(hb, sizeof hb, ".help.%s\"", full ? "full" : "show");
            for (size_t i = t0; i < t1 && hn < (int)sizeof hb - 4; i++) {
                if (s[i] == '"' || s[i] == '\\') hb[hn++] = '\\';
                hb[hn++] = s[i];
            }
            hb[hn++] = '"';
            hb[hn] = '\0';
            return ctx_line(hb, (size_t)hn, out, err, print_result, in_load, esig);
        }
    }

    /* The statement seam: frame-[0] text + suspendability (IPC never suspends;
     * a load line INHERITS it from the statement that asked for the load), and
     * the per-statement save/restore — the error-payload backstop (q_err.c
     * head) rides it.  Every exit path ends it, so a NESTED statement gives
     * this level its state back. */
    int dbg_prev = q_dbg_statement_begin(s, n, in_load ? -1 : print_result);

    /* `\`-system-command line: the shared q_sys glue renders console side
     * effects + value into buf (value-or-throw; `\\`/exit act inside q_sys).
     * Console lines arrive '\n'-terminated, a rendered value does not — the
     * append-if-missing keeps this byte-identical to the historic output. */
    if ((!lang || lang == 'q') && q_sys_is_cmd(s, n)) {
        char buf[8192];
        ray_t* sr = q_sys_line(s, n, print_result, buf, sizeof buf);
        if (buf[0]) {
            fputs(buf, out);
            if (buf[strlen(buf) - 1] != '\n') fputc('\n', out);
        }
        if (sr && in_load) {
            q_err_sig_t t;
            int code = ctx_load_abort(sr, out, err, &t);
            fflush(out);
            ctx_statement_end();
            q_dbg_statement_end(dbg_prev);
            ctx_load_esig(esig, &t);
            return code;
        }
        if (sr) ctx_show_err(out, err, sr);
        fflush(out);
        ctx_statement_end();   /* a `\g 1` line collects at its own end (kdb: set runs gc) */
        q_dbg_statement_end(dbg_prev);
        return 0;
    }

    ray_t* ast = (lang && lang != 'q') ? q_ctx_lang_tree(lang, s, (int64_t)n)
                                       : q_parse(s);
    if (RAY_IS_ERR(ast)) {                     /* 'dup dies at parse (qsql.md:168) */
        int code = ast->aux[0] ? (int)ast->aux[0] : (int)QE_PARSE + 1;
        ctx_show_err(out, err, ast);
        q_dbg_statement_end(dbg_prev);
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

    /* THE doc-capture fire point: a header claimed during this statement's eval
     * reaches its `.help` hook here — after the eval (never mid-eval) and BEFORE the
     * drain below, so anything the q hook shows leaves with this statement. */
    q_comment_stmt_end();

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
        q_err_drop();              /* the discarded error's text never re-renders */
        ctx_show_err(out, err, q_err(QE_STOP));
        ctx_statement_end();
        q_dbg_statement_end(dbg_prev);
        if (in_load) {             /* a Ctrl-C aborts the load — never suspends */
            if (esig) *esig = q_err(QE_STOP);
            return (int)QE_STOP + 1;
        }
        return 0;
    }

    if (RAY_IS_ERR(r)) {
        if (in_load) {
            ray_t* rep = q_dbg_load_filter(r);
            if (rep && !RAY_IS_ERR(rep)) {     /* :r — discard, the load continues */
                ray_error_free(r);
                ray_release(rep);
                fflush(out);
                ctx_statement_end();
                q_dbg_statement_end(dbg_prev);
                return 0;
            }
            if (rep) { ray_error_free(r); r = rep; }   /* 'e — abort with the re-signal */
            q_err_sig_t t;
            int code = ctx_load_abort(r, out, err, &t);
            ctx_statement_end();
            q_dbg_statement_end(dbg_prev);
            ctx_load_esig(esig, &t);
            return code;
        }
        ctx_show_err(out, err, r);
        ctx_statement_end();
        q_dbg_statement_end(dbg_prev);
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
    q_dbg_statement_end(dbg_prev);
    return 0;
}

int q_ctx_run_line(const char* s, size_t n, FILE* out, FILE* err,
                   int print_result) {
    return ctx_line(s, n, out, err, print_result, 0, NULL);
}

/* The script runner reads BYTES — `\l file` arrives through the q_io byte core
 * and the embedded stdlib bundle (`\l pq`) is already a string, so one
 * multiline law serves both with no second file-reading stack. */
static int ctx_run_script(const char* src, size_t len, int64_t file_sym,
                          FILE* out, FILE* err, ray_t** esig) {
    if (!src) { src = ""; len = 0; }     /* an empty read owns no buffer; `src + len` must stay defined */

    /* ONE logical line, joined.  Bounded by the SOURCE: a physical line adds
     * its own trimmed bytes plus at most the '\n' its own newline already paid
     * for.  Heap, not static — a loaded script may `\l` another while this one
     * still holds the line that invoked it.  Allocated before any global state
     * is touched, so the failure exit has nothing to restore. */
    char* acc = (char*)ray_sys_alloc(len + 2);
    if (!acc) return 2 + (int)QE_OOM;

    /* OWNER RULING 2026-08-06: a load SAVES the caller's `\d` context and
     * RESTORES it when the file runs to completion; a load that ABORTS leaves
     * the context where the error left it (deliberate — that is what makes the
     * failing namespace inspectable).  Every door rides this seam, so `\l`,
     * `system "l …"` and `-f` all obey it from here. */
    int64_t saved_ctx = q_env_ctx();

    /* A load runs at TOP LEVEL (owner ruling 2026-08-11): suspend the caller's
     * frames (env read floor + apply write floor), restored on EVERY exit,
     * aborts included — unlike `\d`, lost locals are never inspectable state. */
    int     saved_floor  = q_eval_apply_frame_floor(-1);
    int32_t saved_ffloor = q_env_frame_floor(-1);

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
    size_t      alen = 0;
    int         in_block = 0;
    const char* p = src;
    const char* pend = src + len;
    int64_t     lineno = 0;

    /* Doc headers ride this same classification — see q_comment.h.  Every arm
     * below states what it means for the run: a blank line, a `/`..`\` block
     * and any CONTINUATION line all end it, so a mid-body comment can never
     * leak onto the next definition. */
    q_comment_script_t dsaved = q_comment_script_begin(file_sym);

    /* ANY failing statement aborts the LOAD (kdb stops a script at the error;
     * qsql.md:168's parse-time 'dup and a mid-file eval error both fire here,
     * so a bad file never runs the statements after it).  At an interactive
     * console the eval error may first SUSPEND into the debugger — ctx_line's
     * load seam — and a `:r` resume continues the load instead of aborting. */
    int lrc = 0;
    #define FLUSH() do { if (alen) { lrc = ctx_line(acc, alen, out, err, 0, 1, esig); alen = 0; } } while (0)

    while (p < pend) {
        const char* line = p;
        const char* nl   = (const char*)memchr(p, '\n', (size_t)(pend - p));
        size_t      n    = nl ? (size_t)(nl - line) : (size_t)(pend - line);
        p = nl ? nl + 1 : pend;
        lineno++;
        /* The span is BORROWED (it IS the source's bytes), so every trim below
         * moves the LENGTH, never writes a NUL — and nothing caps it.  The
         * split already ate the '\n'; only a CRLF's '\r' can be left. */
        while (n && line[n - 1] == '\r') n--;
        /* Strip TRAILING whitespace too, so a block delimiter with superfluous
         * blanks (`/   ` / `\   `) still classifies as a singleton and a code
         * line's insignificant trailing spaces don't skew anything (kdb ignores
         * superfluous blanks — language.md).  Trailing spaces inside a string
         * literal are safe: such a line ends with `"`, not whitespace. */
        while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) n--;

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
        if (tlen == 0) { q_comment_break(); continue; }  /* blank/whitespace-only: ignored, no flush */
        if (tlen == 1 && trim[0] == '/') { in_block = 1; q_comment_break(); continue; }  /* open block; no flush */
        if (tlen == 1 && trim[0] == '\\') break; /* singleton \ exits (post-loop FLUSH runs)  */
        if (trim[0] == '/') { q_comment_line(trim, tlen); continue; }  /* comment-only line: ignored, no flush */

        int is_cont = indented && alen > 0;
        if (is_cont) q_comment_break();                 /* a continuation cannot be documented */
        else { FLUSH(); if (lrc) break; q_comment_fresh_line(lineno); }  /* eval the prior line, arm this one */

        /* append this physical line (join continuation fragments with '\n') */
        if (alen) acc[alen++] = '\n';
        memcpy(acc + alen, line, n);
        alen += n;
        acc[alen] = '\0';                          /* q_ctx_run_line's q_parse reads it as a C string */
    }
    if (!lrc) FLUSH();                             /* eval any pending logical line (incl. before a lone \) */
    #undef FLUSH
    q_comment_script_end(dsaved);

    q_env_frame_floor(saved_ffloor);
    q_eval_apply_frame_floor(saved_floor);
    if (!lrc) q_env_ctx_set(saved_ctx);            /* completed: caller's `\d` back */
    if (lrc && esig && *esig) q_dbg_mark_reported(*esig);
    ray_sys_free(acc);
    return lrc ? 1 + lrc : 0;
}

int q_ctx_run_file(const char* path, FILE* out, FILE* err, ray_t** esig) {
    size_t plen  = strlen(path);
    ray_t* pathv = ray_str(path, plen);
    ray_t* bytes = pathv ? q_io_read_slice(pathv, 0, -1, NULL) : NULL;
    if (pathv) ray_release(pathv);
    if (!bytes || RAY_IS_ERR(bytes)) {
        if (bytes) { q_err_drop(); ray_error_free(bytes); }
        fprintf(err, "q: cannot open script '%s'\n", path);
        return 1;
    }
    int rc = ctx_run_script((const char*)ray_data(bytes), (size_t)ray_len(bytes),
                            ray_sym_intern_runtime(path, plen), out, err, esig);
    ray_release(bytes);
    return rc;
}

int q_ctx_run_src(const char* s, FILE* out, FILE* err, ray_t** esig) {
    /* No path to attribute: the embedded bundles are ONE concatenated string,
     * so their docs record the empty file. */
    return ctx_run_script(s, strlen(s), 0, out, err, esig);
}

/* ===== The remote doors (see q_ctx.h) =====
 *
 * q_ctx_run_line's pipeline, disposing of the result over the wire instead of
 * to `out`; errors propagate as owned values the IPC layer serializes as -128h.
 * Console output drains to the SERVER's stdout, then resets so it cannot bleed
 * into the next request. */

/* `\e` error-trap-CLIENTS mode (syscmds.md#e-error-trap-clients) applied to a
 * request: 1 = the statement is console-marked, so an error suspends on the
 * server's console (the parked event loop is what blocks other requests);
 * 2 = dump the stack to stderr for an untrapped error, still answer (V3.5). */
static int remote_console(void)      { return q_sys_err_trap_mode() == 1 ? 1 : 0; }
static void remote_err_dump(ray_t* r) {
    if (r && RAY_IS_ERR(r) && q_sys_err_trap_mode() == 2)
        q_dbg_print_trace(stderr, r);
}

static ray_t* remote_eval_str(const char* src, size_t len) {
    /* OWNER RULING 2026-08-10: a request obeys ctx_run_script's law — restore the `\d`
     * context on success (no client parks a shared server), leave it where an abort left it. */
    int64_t saved_ctx = q_env_ctx();
    /* A leading `\` is a system command, not q source (kdb runs a solo `\l`/`\p`
     * received on the wire).  `system"X"` is exactly `\X`, so strip and reuse
     * q_system_fn — one home for q_sys_run, the restricted gate, `\`-shell capture. */
    if (len > 0 && src[0] == '\\') {
        ray_t* arg = ray_str(src + 1, len - 1);
        if (!arg) return q_err(QE_OOM);
        int dbg_prev = q_dbg_statement_begin(src, len, remote_console());
        ray_t* r = q_system_fn(arg);
        ray_release(arg);
        { const char* con = q_console_str();
          if (con && *con) fputs(con, stdout);
          q_console_reset(); }
        ctx_statement_end();
        remote_err_dump(r);
        q_dbg_statement_end(dbg_prev);
        if (!RAY_IS_ERR(r)) q_env_ctx_set(saved_ctx);
        return r;
    }
    char* tmp = (char*)ray_sys_alloc(len + 1);
    if (!tmp) return q_err(QE_OOM);
    memcpy(tmp, src, len);
    tmp[len] = '\0';
    /* remote statements suspend only under `\e 1`; the seam always gives a
     * remote .Q.trp its `[0]` frame */
    int dbg_prev = q_dbg_statement_begin(tmp, len, remote_console());
    ray_t* ast = q_parse(tmp);
    if (RAY_IS_ERR(ast)) {
        ray_sys_free(tmp);
        remote_err_dump(ast);            /* `\e 2` covers parse errors too (no
                                          * suspension: parse never suspends) */
        q_dbg_statement_end(dbg_prev);
        return ast;
    }
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
    remote_err_dump(r);                  /* `\e 2`: trace before the seam closes */
    q_dbg_statement_end(dbg_prev);
    if (!RAY_IS_ERR(r)) q_env_ctx_set(saved_ctx);
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
static ray_t* remote_apply_body(ray_t* list) {
    if (!list || (list->type != RAY_LIST && !ray_is_vec(list)) ||
        ray_len(list) < 1)
        return q_err(QE_TYPE);
    int64_t saved_ctx = q_env_ctx();   /* same request law as remote_eval_str: an applied lambda may `system"d …"` */
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
    if (!RAY_IS_ERR(r)) q_env_ctx_set(saved_ctx);
    return r;
}

/* The value-apply request under the same `\e` law; no source text for [0]. */
static ray_t* remote_apply(ray_t* list) {
    int dbg_prev = q_dbg_statement_begin(NULL, 0, remote_console());
    ray_t* r = remote_apply_body(list);
    remote_err_dump(r);
    q_dbg_statement_end(dbg_prev);
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
