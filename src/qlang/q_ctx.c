/* q_ctx — see q_ctx.h.  The engine context: the statement seam every door
 * shares, and the process state that outlives whichever door set it.  Split out
 * of q_repl.c (2026-08-02) — ops/ needed `\l`, and reaching into repl/ for it
 * made a verb depend on the front end. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/q_count.h"
#include "qlang/q_ctx.h"
#include "qlang/base/q_err.h"     /* q_err / q_err_drop — the statement-entry backstop */
#include "qlang/q_comment.h"          /* doc headers: the run above a definition */
#include "qlang/parse/q_parse.h"
#include "qlang/eval/q_eval.h"
#include "qlang/eval/q_dbg.h"     /* statement stash + `\e` trace display */
#include "qlang/eval/q_view.h"    /* q_view_intercept — `x::e` at the line seam */
#include "qlang/q_env.h"          /* q_env_ctx / _set — the load's `\d` save+restore; q_env_peek — `.<ext>.e` */
#include "qlang/q_fmt.h"
#include "qlang/q_console.h"      /* q_console_door — the running door names the console's stream */
#include "qlang/q_prim.h"        /* q_str_text_bytes — the remote value-apply head; q_ssr_wrap — a known file's CRLF;
                                   * q_str_split_lines / q_str_charv_out — a whole-text file as read0's lines */
#include "qlang/q_builtins.h"     /* q_dotq_sha1_fn — a known file's digest */
#include "qlang/q_dotz.h"         /* q_dotz_quiet — `-q` silences a transcript's prompt and echo, as the piped console's */
#include "qlang/ops/q_sys.h"      /* q_sys_gc_mode / q_sys_err_trap_mode — the statement-seam policy; q_sys_prompt */
#include "qlang/ops/q_index.h"    /* q_index_elem_at — the element-read home */
#include "qlang/io/q_io.h"        /* q_io_read_slice — THE byte core a load reads through */
#include "lang/env.h"             /* ray_fn_unary — the .pq.i.console native */
#include "lang/eval.h"            /* ray_eval_is_interrupted, ray_eval_set_remote_*_fn, RAY_FN_NONE */
#include "mem/heap.h"             /* ray_heap_gc — the `\g 1` statement-end collect */
#include "mem/sys.h"              /* ray_sys_alloc — remote-eval scratch */
#include "ops/ops.h"              /* ray_is_lazy, ray_lazy_materialize */
#include "app/term.h"             /* ray_term_interrupted */
#include "core/timer.h"           /* ray_time_now_ms — a finished line's ms */
#include "core/runtime.h"         /* __VM->ipc_handle — a transcript line runs in the console's handle context */
#include <rayforce.h>
#include <ctype.h>                /* isalnum — a file's extension */
#include <limits.h>               /* PATH_MAX — the load's resolved file symbol */
#include <stdlib.h>
#include <string.h>

/* The front end's terminal teardown, if one registered.  See q_ctx.h. */
static void (*g_console_close)(void);

static void (*g_tty_restore)(void);

void q_ctx_set_console_close(void (*fn)(void)) { g_console_close = fn; }
void q_ctx_set_tty_restore(void (*fn)(void))   { g_tty_restore = fn; }
static void ctx_tty_restore(void) { if (g_tty_restore) g_tty_restore(); }
/* The console line running now (innermost: a q)) line inside the q) line it suspended), for the line-done hook. */
typedef struct { const char* s; size_t n; int64_t t0; } ctx_line_t;
static ctx_line_t g_line;
static void (*g_line_done)(const char* s, size_t n, const char* status, int64_t ms);

void q_ctx_set_line_done(void (*fn)(const char*, size_t, const char*, int64_t)) { g_line_done = fn; }

static void ctx_line_done(ray_t* e, const char* status) {
    char        st[128];
    int64_t     n = 0;
    const char* t = e ? q_err_text(e, &n) : NULL;
    if (t) snprintf(st, sizeof st, "'%.*s", (int)n, t);
    else   snprintf(st, sizeof st, "%s", status);
    if (g_line_done && g_line.s) g_line_done(g_line.s, g_line.n, st, ray_time_now_ms() - g_line.t0);
    g_line.s = NULL;
}

void q_ctx_console_close(void) {
    if (g_line.s) ctx_line_done(NULL, "exit");
    ctx_tty_restore();
    if (g_console_close) g_console_close();
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
    ctx_tty_restore();
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
/* print_result: when non-zero a non-null, non-assignment result is q-formatted
 * to `out` (console auto-display).  A script LOAD prints too — kdb displays every
 * unterminated top-level statement of a file (ml/jupyterq/index.md: a `;` is
 * the off-switch; #53) — so zero is only for argv/bootstrap text. */

/* A load line that ABORTS re-signals to whoever asked for the load: display
 * once at this line — unless a trap will catch it, whose handler owns it (owner
 * 2026-09-25) — then DETACH the error's identity (else display's drop and
 * q_dbg_statement_end's payload restore would destroy the text); the caller
 * re-signals only after the statement seam has closed. */
static int ctx_load_abort(ray_t* e, FILE* out, FILE* err, q_err_sig_t* t) {
    ctx_tty_restore();
    if (!q_dbg_reported(e) && !q_dbg_trapped()) {
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
 * (eval errors report and return 0).  console: q_dbg_statement_begin's — 0
 * never suspends (a transcript's line), else a console statement or a load
 * line's inheritance. */
static int ctx_line_run(const char* s, size_t n, FILE* out, FILE* err,
                        int print_result, int in_load, int console, ray_t** esig) {
    if (n == 0)
        return 0;

    /* The statement seam: frame-[0] text + suspendability (IPC never suspends;
     * a load line INHERITS it from the statement that asked for the load), and
     * the per-statement save/restore — the error-payload backstop (q_err.c
     * head) rides it.  Every exit path ends it, so a NESTED statement gives
     * this level its state back. */
    int dbg_prev = q_dbg_statement_begin(s, n, console);

    /* `.z.pi` (ref/dotz.md:703, syscmds.md:937, owner 2026-09-18): a console
     * line is the handler's to evaluate and its result is the display.  Never a
     * load line, and never a `\`-command — both doc transcripts show `\x .z.pi`
     * printing nothing under a prefixing handler, so the console keeps its
     * commands (the text intake's byte-0 read).  The text is the line without
     * its newline (what kdb passes is unrecorded: flip it here).  Gated on the
     * binding only: a handler that never calls `value` swallows every
     * expression, as on kdb. */
    int     parsed = 1;
    ray_t*  r;
    int64_t zpi    = ray_sym_intern_runtime(".z.pi", 5);
    ray_t*  zfn    = print_result && !in_load && s[0] != '\\' ? q_env_get(zpi) : NULL;
    int     hooked = zfn && q_eval_apply_is_fn(zfn);
    if (hooked) {
        ray_t* txt = ray_charv(s, (int64_t)n);
        r = q_eval_apply_call_sym(zpi, &txt, 1);
        ray_release(txt);
    } else
        r = q_eval_statement(s, &parsed);
    if (!parsed) {                             /* 'dup dies at parse (qsql.md:168) */
        int code = r->aux[0] ? (int)r->aux[0] : (int)QE_PARSE + 1;
        if (in_load) {                         /* the load's trap sees the text too */
            q_err_sig_t t;
            code = ctx_load_abort(r, out, err, &t);
            q_dbg_statement_end(dbg_prev);
            ctx_load_esig(esig, &t);
            return code;
        }
        ctx_line_done(r, NULL);
        ctx_show_err(out, err, r);
        q_dbg_statement_end(dbg_prev);
        return code;
    }
    if (ray_is_lazy(r))
        r = ray_lazy_materialize(r);

    /* THE doc-capture fire point: a header claimed during this statement's eval
     * reaches its `.help` hook here — after the eval, never mid-eval. */
    q_comment_stmt_end();

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
        ray_t* stop = q_err(QE_STOP);
        if (!in_load) ctx_line_done(stop, NULL);
        ctx_show_err(out, err, stop);
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
        ctx_line_done(r, NULL);
        ctx_show_err(out, err, r);
        ctx_statement_end();
        q_dbg_statement_end(dbg_prev);
        return 0;
    }
    /* q console silence: the generic null prints nothing, which is already what
     * an assignment statement (q_eval_statement) or `x;` answers.  A handler's
     * TEXT is written as-is: the default handler is `.Q.s value x`. */
    const char* hp; int64_t hn;
    if (hooked && !RAY_IS_NULL(r) && q_str_text_bytes(r, &hp, &hn)) {
        fwrite(hp, 1, (size_t)hn, out);
    } else if (print_result && !RAY_IS_NULL(r)) {
        size_t n;
        char*  txt = q_fmt_console_alloc(r, &n);   /* obey \c on auto-echo display */
        if (txt) {
            fwrite(txt, 1, n, out);
            fputc('\n', out);
            free(txt);
        } else {
            ray_t* e = q_err(QE_WSFULL);   /* a display we cannot build is an error */
            if (!in_load) ctx_line_done(e, NULL);
            ctx_show_err(out, err, e);
        }
    }
    ray_release(r);
    fflush(out);
    if (!in_load && g_line.s) ctx_line_done(NULL, "ok");
    ctx_statement_end();
    q_dbg_statement_end(dbg_prev);
    return 0;
}

static int ctx_line(const char* s, size_t n, FILE* out, FILE* err,
                    int print_result, int in_load, int console, ray_t** esig) {
    FILE* door = q_console_door(out);
    int   rc   = ctx_line_run(s, n, out, err, print_result, in_load, console, esig);
    q_console_door(door);
    return rc;
}

int q_ctx_run_line(const char* s, size_t n, FILE* out, FILE* err,
                   int print_result) {
    ctx_line_t prev = g_line;
    g_line = (ctx_line_t){ s, n, ray_time_now_ms() };
    int rc = ctx_line(s, n, out, err, print_result, 0, print_result, NULL);
    g_line = prev;
    return rc;
}

/* THE multiline law as ONE walker every text door rides — the script runner
 * (prints) and the value door (answers).  Yields each logical line, NUL-
 * terminated, to `fn`; the first non-zero return stops the walk and is the
 * walker's own.  It reads BYTES — `\l file` arrives through the q_io byte core
 * and the embedded stdlib bundle (`\l pq`) is already a string, so one law
 * serves both with no second file-reading stack. */
typedef int (*ctx_stmt_fn)(const char* s, size_t n, void* u);

static int ctx_walk_script(const char* src, size_t len, int64_t file_sym, ctx_stmt_fn fn, void* u) {
    if (!src) { src = ""; len = 0; }     /* an empty read owns no buffer; `src + len` must stay defined */

    /* ONE logical line, joined.  Bounded by the SOURCE: a physical line adds
     * its own trimmed bytes plus at most the '\n' its own newline already paid
     * for.  Heap, not static — a loaded script may `\l` another while this one
     * still holds the line that invoked it. */
    char* acc = (char*)ray_sys_alloc(len + 2);
    if (!acc) return 1 + (int)QE_OOM;

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
    #define FLUSH() do { if (alen) { lrc = fn(acc, alen, u); alen = 0; } } while (0)

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
    ray_sys_free(acc);
    return lrc;
}

typedef struct { FILE* out; FILE* err; int print_result; char lang; ray_t** esig; } ctx_load_t;

/* A custom language handler lives in a SINGLE-LETTER namespace and is passed the statement text
 * (wp/query-interface.md:41), so a script whose extension is one such letter runs every statement
 * through `.X.e` — embedPy's `.t` suites are scored that way.  Prefixing is the whole routing: the
 * text seam already strips every leading `<letter>)` and keeps the RIGHTMOST, so a `p)` line inside
 * a `.t` file still reaches `.p.e`.  `q` and `k` keep their own doors. */
static int ctx_load_stmt(const char* s, size_t n, void* u) {
    ctx_load_t* ld = (ctx_load_t*)u;
    if (!ld->lang) return ctx_line(s, n, ld->out, ld->err, ld->print_result, 1, -1, ld->esig);
    char* t = (char*)ray_sys_alloc(n + 3);
    if (!t) return 1 + (int)QE_OOM;
    t[0] = ld->lang;
    t[1] = ')';
    memcpy(t + 2, s, n);
    t[n + 2] = '\0';
    int rc = ctx_line(t, n + 2, ld->out, ld->err, ld->print_result, 1, -1, ld->esig);
    ray_sys_free(t);
    return rc;
}

static char ctx_script_lang(const char* path) {
    const char* dot = path ? strrchr(path, '.') : NULL;
    if (!dot || strlen(dot) != 2) return 0;
    char c = dot[1];
    int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');   /* `.H.e` is the documented one */
    if (!alpha || c == 'q' || c == 'k') return 0;
    /* Gated on the HANDLER, not the spelling (owner ruling 2026-09-22): `.h` and `.j` are peachq's own
     * namespaces, so a file named for one must keep loading as q until someone defines its `.X.e`. */
    char    name[5] = { '.', c, '.', 'e', '\0' };
    ray_t*  fn      = q_env_get(ray_sym_intern_runtime(name, 4));
    return fn && q_eval_apply_is_fn(fn) ? c : 0;
}

/* THE load boundary, every door's (`\l`, `system "l …"`, `-f`, a whole-text file).  OWNER RULING 2026-08-06: a
 * load SAVES the caller's `\d` context and RESTORES it when the file runs to completion; a load that ABORTS leaves
 * the context where the error left it (deliberate — that is what makes the failing namespace inspectable; a trap
 * that catches the abort restores it, 2026-09-17).  And a load runs at TOP LEVEL (owner ruling 2026-08-11): the
 * caller's frames suspended, resumed on EVERY exit, aborts included — unlike `\d`, lost locals are never
 * inspectable state. */
typedef struct { int64_t ctx; q_eval_frames_t frames; } ctx_load_scope_t;

static ctx_load_scope_t ctx_load_enter(void) {
    ctx_load_scope_t s;
    s.ctx    = q_env_ctx();
    s.frames = q_eval_frames_suspend();
    return s;
}

static void ctx_load_leave(ctx_load_scope_t s, int completed) {
    q_eval_frames_resume(s.frames);
    if (completed) q_env_ctx_set(s.ctx);
}

static int ctx_run_script(const char* src, size_t len, int64_t file_sym, int print_result,
                          char lang, FILE* out, FILE* err, ray_t** esig) {
    ctx_load_scope_t scope = ctx_load_enter();
    ctx_load_t       ld    = { out, err, print_result, lang, esig };
    int              lrc   = ctx_walk_script(src, len, file_sym, ctx_load_stmt, &ld);
    ctx_load_leave(scope, !lrc);
    if (lrc && esig && *esig && !q_dbg_trapped()) q_dbg_mark_reported(*esig);
    return lrc ? 1 + lrc : 0;
}

/* The value door's line: keep the latest answer, stop at the first error. */
static int ctx_eval_stmt(const char* s, size_t n, void* u) {
    ray_t** last = (ray_t**)u;
    ray_t*  r    = q_eval_statement(s, NULL);
    if (ray_is_lazy(r)) r = ray_lazy_materialize(r);
    q_comment_stmt_end();                      /* a definition's header leaves with its statement, as in a load */
    if (*last) ray_release(*last);
    *last = r;
    return RAY_IS_ERR(r) ? 1 : 0;
}

ray_t* q_ctx_eval_src(const char* s, size_t n) {
    ray_t* last = NULL;
    int    rc   = ctx_walk_script(s, n, 0, ctx_eval_stmt, &last);
    if (last) return last;
    if (rc) return q_err(QE_OOM);          /* the one non-statement stop: the walker's own buffer */
    ray_retain(RAY_NULL_OBJ);              /* no statement ran: what an assignment answers */
    return RAY_NULL_OBJ;
}

/* Third-party files q cannot run but whose loss costs nothing, known by the SHA-1 of their text read with CRLF as LF
 * (both checkouts are one row); a load runs `q` in their place.  Never by name: every other byte runs as it is. */
static const struct { int64_t len; const char* sha1; const char* q; } g_known_files[] = {
    /* TorQ code/common/json.k (4aa4b90e…): KX's old k encoder, parked under `.jOLD` (unreferenced) on a modern .z.K */
    { 725, "\x4a\xa4\xb9\x0e\xef\x7b\x7c\x9b\x7c\xd0\x57\x2e\x6c\xf8\xee\xee\xd7\x41\x1f\x19", "" },
    /* Old TorQ code/common/json.k (1bace6a8…): an older KX k encoder loaded into `.j`; later TorQ kept native .j */
    { 514, "\x1b\xac\xe6\xa8\x4a\x61\x1d\xbf\xcf\xbe\x0e\x67\x5c\x4b\x2c\xcb\x5b\xb7\x6a\xcb", "" },
    /* KX e/json.k (32327ed1…): "json support for kdb+3.2 or before (superceded by .j namespace)", e/README.txt */
    { 831, "\x32\x32\x7e\xd1\x10\x4b\x65\xe0\xcd\xc1\x26\xdd\xa1\xbc\xcd\xb9\xc1\x71\xd0\x44", "" },
};

static const char* ctx_known_file(const char* p, int64_t n) {
    size_t k = 0, nk = sizeof g_known_files / sizeof *g_known_files;
    while (k < nk && !(g_known_files[k].len <= n && n <= 2 * g_known_files[k].len)) k++;   /* CRLF at most doubles */
    if (k == nk) return NULL;
    ray_t* a[3] = { ray_charv(p, n), ray_charv("\r\n", 2), ray_charv("\n", 1) };
    ray_t* lf   = q_ssr_wrap(a, 3);
    for (int i = 0; i < 3; i++) ray_release(a[i]);
    int64_t len = RAY_IS_ERR(lf) ? -1 : q_count(lf);
    ray_t*  dg  = RAY_IS_ERR(lf) ? lf : q_dotq_sha1_fn(lf);
    if (dg != lf) ray_release(lf);
    if (!dg || RAY_IS_ERR(dg)) { if (dg) { q_err_drop(); ray_error_free(dg); } return NULL; }
    const char* hit = NULL;
    for (; k < nk && !hit; k++)
        if (g_known_files[k].len == len && !memcmp(ray_data(dg), g_known_files[k].sha1, 20)) hit = g_known_files[k].q;
    ray_release(dg);
    return hit;
}

/* ===== Whole-text files: `.<ext>.e` and the .qcmd default =====
 *
 * A file whose extension is MORE than one letter is handed WHOLE, as read0's
 * lines, to `.<ext>.e` when that is a function, else to peachq's built-in
 * default for the extension — `qcmd`: `.pq.i.qcmd` (lib/pq.q), a console
 * transcript replayed — else it loads as q (owner ruling 2026-09-24).  Whole,
 * because the statement walker cannot read a transcript: an indented display
 * line continues the statement before it, a bare `/` output line opens a
 * comment block and a lone `\` ends the file.  kdb's single-letter handlers
 * (ctx_script_lang) keep their statement-by-statement routing. */

static FILE* g_console_out;   /* the load in progress's streams: what `.pq.i.console` prints to; NULL = stdout/stderr */
static FILE* g_console_err;

/* `.pq.i.console` — see q_ctx.h: prompt and input echo (none under `-q`, as
 * the piped console), then the line as the console runs it — `.z.pi`,
 * display, an error printed and the run continuing.  Never suspends (a
 * transcript's expected answer is often an error), never a history record
 * (nobody typed it), and at top level: a transcript's `a:1` is the session's. */
static ray_t* pq_console_fn(ray_t* x) {
    const char* p; int64_t n;
    if (!q_str_text_bytes(x, &p, &n)) return q_err(QE_TYPE);
    FILE* out = g_console_out ? g_console_out : stdout;
    FILE* err = g_console_err ? g_console_err : stderr;
    if (!q_dotz_quiet()) {
        char prompt[80];
        int  pl = q_sys_prompt(prompt, sizeof prompt);
        fwrite(prompt, 1, (size_t)pl, out);
        fwrite(p, 1, (size_t)n, out);
        fputc('\n', out);
        fflush(out);   /* a terminal shows the echo before anything the line writes to stderr */
    }
    if (n) {
        char* s = (char*)ray_sys_alloc((size_t)n + 1);   /* q_parse reads a C string */
        if (!s) return q_err(QE_OOM);
        memcpy(s, p, (size_t)n);
        s[n] = '\0';
        ctx_line_t      prev   = g_line;
        q_eval_frames_t frames = q_eval_frames_suspend();
        int             base   = q_dbg_frame_base(-1);
        int64_t         conn   = __VM ? __VM->ipc_handle : -1;
        if (__VM) __VM->ipc_handle = -1;           /* the console's handle context: .z.w 0i, as `0 x` sets it */
        g_line.s = NULL;
        ctx_line(s, (size_t)n, out, err, 1, 0, 0, NULL);
        if (__VM) __VM->ipc_handle = conn;
        q_dbg_frame_base(base);
        g_line = prev;
        q_eval_frames_resume(frames);
        ray_sys_free(s);
    }
    fflush(out);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

void q_ctx_pq_register(void) {
    ray_t* fn = ray_fn_unary(".pq.i.console", RAY_FN_NONE, pq_console_fn);
    q_env_bind_native(".pq.i.console", fn, 1);
    ray_release(fn);
}

static const char* ctx_ext(const char* path) {   /* an alphanumeric extension longer than a letter, else NULL */
    const char* dot = strrchr(path, '.');
    if (!dot || strlen(dot) < 3) return NULL;
    for (const char* c = dot + 1; *c; c++)
        if (!isalnum((unsigned char)*c)) return NULL;
    return dot + 1;
}

/* 1 with *handler = `.<ext>.e` when that is a function (borrowed), 1 with NULL for a built-in default, 0: q. */
static int ctx_whole_file(const char* path, ray_t** handler) {
    const char* ext = ctx_ext(path);
    char        name[64];
    *handler = NULL;
    if (!ext || snprintf(name, sizeof name, ".%s.e", ext) >= (int)sizeof name) return 0;
    ray_t* fn = q_env_peek(ray_sym_intern_runtime(name, strlen(name)));   /* peek: `f.pq` must not autoload .pq */
    if (fn && q_eval_apply_is_fn(fn)) { *handler = fn; return 1; }
    /* internal only, not user-facing: no user-docs, help or --help mention until the owner decides to expose it */
    return strcmp(ext, "qcmd") == 0;
}

/* The whole-text load, under the load boundary's law.  The handler's error is the load's answer (esig), displayed
 * here only when nobody asked for it. */
static int ctx_run_whole(ray_t* handler, ray_t* lines, FILE* out, FILE* err, ray_t** esig) {
    ctx_load_scope_t scope    = ctx_load_enter();
    FILE*            door     = q_console_door(out);
    FILE*            prev_out = g_console_out;
    FILE*            prev_err = g_console_err;
    g_console_out = out;
    g_console_err = err;
    ray_t* r  = handler ? q_eval_apply_value(handler, &lines, 1)
                        : q_eval_apply_call_name(".pq.i.qcmd", 10, &lines, 1);   /* the `.pq` autoload resolves it */
    int    ok = !r || !RAY_IS_ERR(r);
    g_console_out = prev_out;
    g_console_err = prev_err;
    q_console_door(door);
    ctx_load_leave(scope, ok);
    if (ok) {
        if (r) ray_release(r);
        return 0;
    }
    int code = r->aux[0] ? (int)r->aux[0] : (int)QE_VALUE + 1;
    if (esig) *esig = r;
    else ctx_show_err(out, err, r);
    return 1 + code;
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
    char abs[PATH_MAX];
    const char* fpath = q_io_abs_path(path, abs, sizeof abs) ? abs : path;   /* ref/value.md `f`: the FULL path */
    const char* src = (const char*)ray_data(bytes);
    const char* sub = ctx_known_file(src, q_count(bytes));
    ray_t*      handler;
    int rc;
    if (!sub && ctx_whole_file(path, &handler)) {
        ray_t* lines = q_str_charv_out(q_str_split_lines(src, (size_t)q_count(bytes)));
        if (RAY_IS_ERR(lines)) { ray_error_free(lines); rc = 1 + (int)QE_OOM; }
        else { rc = ctx_run_whole(handler, lines, out, err, esig); ray_release(lines); }
    } else
        rc = ctx_run_script(sub ? sub : src, sub ? strlen(sub) : (size_t) q_count(bytes),
                            ray_sym_intern_runtime(fpath, strlen(fpath)), 1,
                            ctx_script_lang(path), out, err, esig);
    ray_release(bytes);
    return rc;
}

ray_t* q_ctx_run_abort(int rc, ray_t* esig) {
    if (esig) return esig;
    return rc >= 2 ? q_err((q_err_e)(rc - 2)) : NULL;
}

int q_ctx_run_src(const char* s, FILE* out, FILE* err, ray_t** esig) {
    /* No path to attribute: the core bundles are ONE concatenated string, so
     * their docs record the empty file.  Silent: this is the bootstrap and the
     * launcher's argv text (`-eval`, ratified silent 2026-08-30), not a `\l`. */
    return ctx_run_script(s, strlen(s), 0, 0, 0, out, err, esig);
}

int q_ctx_run_named_src(const char* name, const char* s, FILE* out, FILE* err, ray_t** esig) {
    return ctx_run_script(s, strlen(s), ray_sym_intern_runtime(name, strlen(name)), 1, 0, out, err, esig);
}

int q_ctx_run_console_src(const char* s, FILE* out, FILE* err) {
    int tok = q_dbg_statement_begin(s, strlen(s), 1);   /* the statement the load's lines inherit console-ness from */
    int rc  = ctx_run_script(s, strlen(s), 0, 0, 0, out, err, NULL);   /* argv text: silent, as q_ctx_run_src */
    q_dbg_statement_end(tok);
    return rc;
}

int q_ctx_run_load(const char* name, FILE* out, FILE* err) {
    size_t n   = strlen(name);
    int    tok = q_dbg_statement_begin(name, n, 0);
    ray_t* r   = q_sys_load(name, n);
    int    rc  = r && RAY_IS_ERR(r);
    if (rc) ctx_show_err(out, err, r);
    else if (r) ray_release(r);
    fflush(out);
    ctx_statement_end();
    q_dbg_statement_end(tok);
    return rc;
}

/* ===== The remote doors (see q_ctx.h) =====
 *
 * q_ctx_run_line's pipeline, disposing of the result over the wire instead of
 * to `out`; errors propagate as owned values the IPC layer serializes as -128h.
 * Console output is written to the SERVER's stdout as it is issued. */

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
    /* remote statements suspend only under `\e 1`; the seam always gives a
     * remote .Q.trp its `[0]` frame */
    int dbg_prev = q_dbg_statement_begin(src, len, remote_console());
    /* Remote source text is a SCRIPT like any other text (owner ruling
     * 2026-09-20): the assignment law and the view intercept come from the one
     * home — basics/ipc.md's `h"fn:{2+x}"` displays nothing because `value`
     * answers nothing, not because the wire silences it.  A parse error needs
     * no arm of its own here (it propagates as the -128h answer); only the
     * `\e 2` dump is this door's. */
    FILE*  door = q_console_door(stdout);
    ray_t* r    = q_ctx_eval_src(src, len);
    q_console_door(door);
    ctx_statement_end();
    remote_err_dump(r);                  /* `\e 2`: trace before the seam closes */
    q_dbg_statement_end(dbg_prev);
    if (!RAY_IS_ERR(r)) q_env_ctx_set(saved_ctx);
    return r;                            /* an error propagates as the -128h answer */
}

/* The kdb value/apply wire shape — NOT a statement: ONE list-apply of the head to
 * already-evaluated tail args, never re-evaluating them (ADR-0004: value, not
 * eval), through the one public apply entry.  A sym/string head resolves/parses to
 * its value first.  `list` is BORROWED (the ipc layer releases it); the result is
 * OWNED.  Restricted mode needs no re-assert: ipc_dispatch sets it around the
 * whole dispatch. */
static ray_t* remote_apply_body(ray_t* list) {
    if (!list || (list->type != RAY_LIST && !ray_is_vec(list)) ||
        q_count(list) < 1)
        return q_err(QE_TYPE);
    int64_t saved_ctx = q_env_ctx();   /* same request law as remote_eval_str: an applied lambda may `system"d …"` */
    int64_t n = q_count(list);
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
