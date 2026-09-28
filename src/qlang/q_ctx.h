/* q_ctx — the global engine context.  q is single-threaded with one shared
 * context that any door may set, so the state and the seams below belong to the
 * ENGINE, not to whichever front end happens to be driving: `system "l f.q"`
 * and `\l f.q` are the same operation, and a `\p` listener outlives stdin
 * whether a REPL, a script or an IPC peer started it.
 *
 * Homing them here is what lets ops/ reach them without including repl/ (the
 * layering rule: nothing may include repl/). */
#ifndef Q_CTX_H
#define Q_CTX_H

#include <stdio.h>
#include <stddef.h>
#include <rayforce.h>       /* ray_t — the remote doors answer with a value */

/* ---- the statement seam ---------------------------------------------------
 * ONE line of q source, executed the way every door executes it: parse (which
 * owns the `q)` prompt and the `\`-command — see q_parse.c's text intake) ->
 * view-intercept -> eval -> flush console side effects -> report.  print_result echoes a
 * non-null non-assignment value (REPL); zero discards it (script load — kdb
 * scripts are silent but for explicit side effects).  Returns 0 when the line
 * RAN (eval errors included — they were reported); else the PARSE error's
 * q_err_e + 1 — the statement never ran. */
int q_ctx_run_line(const char* s, size_t n, FILE* out, FILE* err, int print_result);

/* Called as each q_ctx_run_line line finishes, a q)) line inside the one it suspended included: its text, "ok" or
 * the error shown ('type), and its ms; "exit" for the line that ends the process.  The tty keeps its history here. */
void q_ctx_set_line_done(void (*fn)(const char* s, size_t n, const char* status, int64_t ms));

/* A file of q source under kdb script semantics: an INDENTED line continues the
 * previous logical one, blank/comment lines do not flush, `/`..`\` blocks skip,
 * and a trimmed singleton `\` exits the script.  Returns 1 if the file's bytes
 * could not be READ (error already printed); 0 on a full load; else 1 + the
 * failing statement's code — the load ABORTED at the first erroring statement,
 * parse or eval alike (kdb stops a script at the error; at an interactive
 * console an eval error may instead suspend into the `\e 1` debugger, and a
 * resume continues the load).  On an EVAL abort, non-NULL esig receives an
 * owned re-signal carrying the error's text, already displayed at the erroring
 * line — `\l` raises it so the abort propagates out of nested loads. */
int q_ctx_run_file(const char* path, FILE* out, FILE* err, ray_t** esig);

/* `.pq`'s native `.pq.i.console x` — the console line for a transcript's input x: the live prompt and x echoed
 * (neither under `-q`, as the piped console), then x run as the console runs a line, on the streams of the load in
 * progress (else stdout/stderr).  `.pq.i.qcmd` applies it per prompt line.  Bound by `.pq.load_natives`pq`. */
void q_ctx_pq_register(void);

/* The error a load ANSWERS with, from a run's return and esig: the eval abort's
 * owned re-signal, a parse abort's class, NULL for a full run — what `\l` raises. */
ray_t* q_ctx_run_abort(int rc, ray_t* esig);

/* The launcher's file as ONE batch statement: `name` reaches q_sys_load (the loader `\l` uses) as the argv token,
 * spaces kept, never re-parsed as `\l` text.  Returns 0 on a load, 1 after displaying its error once. */
int q_ctx_run_load(const char* name, FILE* out, FILE* err);

/* An in-memory STRING of q source under the SAME script semantics/returns as
 * q_ctx_run_file — the bootstrap and the launcher's argv text ride this: one
 * loader, one multiline law — except that a bare top-level value is NOT
 * echoed (a load echoes, #53; `-eval` was ratified silent 2026-08-30). */
int q_ctx_run_src(const char* s, FILE* out, FILE* err, ray_t** esig);

/* q_ctx_run_src for an embedded file that KEEPS its name: `\l pq` runs each
 * library file through this so the doc capture attributes the file's header to
 * `name` and a lambda's `l` names it, as a `\l` of the real
 * file would. */
int q_ctx_run_named_src(const char* name, const char* s, FILE* out, FILE* err, ray_t** esig);

/* Text under the SAME multiline law, ANSWERING instead of printing: the value
 * of the last statement (owned; an assignment's is `::`, as is an empty text),
 * or the first erroring statement's error, the statements after it never run.
 * `value` of a string and the IPC source-text door are this door (owner ruling
 * 2026-09-20: kdb's `value` is script-aware).  Nothing is echoed and no console
 * drain happens here — the caller's statement seam owns both, as it owns the
 * `\d` and frame-floor policy. */
ray_t* q_ctx_eval_src(const char* s, size_t n);

/* q_ctx_run_src as a CONSOLE-initiated load — the tty's startup texts (`\l file`,
 * the `-eval` texts): silent and multiline as any script, but its statements
 * suspend into the debugger the way a typed `\l`'s do, so `:`/`\` resume or
 * abort the load.  An abort is reported here; the return is q_ctx_run_src's. */
int q_ctx_run_console_src(const char* s, FILE* out, FILE* err);

/* Install the two callbacks the IPC layer evaluates a request through: source
 * text (the seam above, but answering with a value instead of printing — hence
 * a shared pipeline, not a shared function) and the kdb `(func;args)`
 * value-apply, which is not source at all.  q_runtime owns the paired teardown. */
void q_ctx_install_remote_hooks(void);

/* Console teardown before exit.  The context knows only that SOMETHING may need
 * restoring before `.z.exit` runs (its 0N! output must land on a cooked
 * terminal); the front end that owns a terminal registers the how.  Unset —
 * wasm, a bare pipe — q_ctx_console_close is a no-op. */
void q_ctx_set_console_close(void (*fn)(void));
void q_ctx_console_close(void);

/* A tty owner NESTED inside the front end's (.termbox): its restore fires on
 * every ERROR / 'stop statement end, before the trace prints, and FIRST in
 * q_ctx_console_close — LIFO, its snapshot was taken inside the REPL's eval
 * window.  A normal statement end never fires it: a script's init, loop and
 * shutdown are separate statements.  The fn must be a no-op when idle. */
void q_ctx_set_tty_restore(void (*fn)(void));

#endif /* Q_CTX_H */
