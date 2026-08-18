/* q_ctx — the global engine context.  q is single-threaded with one shared
 * context that any door may set, so the state and the seams below belong to the
 * ENGINE, not to whichever front end happens to be driving: `system "l f.q"`
 * and `\l f.q` are the same operation, and a `\p` listener outlives stdin
 * whether a REPL, a script or an IPC peer started it.
 *
 * Homing them here is what lets ops/ and parse/ reach them without including
 * repl/ (the layering rule: nothing may include repl/). */
#ifndef Q_CTX_H
#define Q_CTX_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <rayforce.h>       /* ray_t — q_ctx_lang_tree builds a q tree */

/* ---- the statement seam ---------------------------------------------------
 * ONE line of q source, executed the way every door executes it: strip a pasted
 * `q)` prompt, route a `\`-command through q_sys, else parse -> view-intercept
 * -> eval -> flush console side effects -> report.  print_result echoes a
 * non-null non-assignment value (REPL); zero discards it (script load — kdb
 * scripts are silent but for explicit side effects).  Returns 0 when the line
 * RAN (eval errors included — they were reported); else the PARSE error's
 * q_err_e + 1 — the statement never ran. */
int q_ctx_run_line(const char* s, size_t n, FILE* out, FILE* err, int print_result);

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

/* An in-memory STRING of q source under the SAME script semantics/returns as
 * q_ctx_run_file — the embedded stdlib bundle (`\l pq`) rides this: one
 * loader, one multiline law. */
int q_ctx_run_src(const char* s, FILE* out, FILE* err, ray_t** esig);

/* Scan leading `<letter>)` prefixes off the (s; n) pair (rightmost letter wins;
 * `q))` never matches).  Returns the language letter, 0 = none. */
char q_ctx_lang_scan(const char** s, size_t* n);

/* `(.X.e; "text")` application tree (`q` -> `value`) — the language-handler
 * dispatch shared by the statement seam and the char-atom apply arm. */
ray_t* q_ctx_lang_tree(char letter, const char* p, int64_t n);

/* Install the two callbacks the IPC layer evaluates a request through: source
 * text (the seam above, but answering with a value instead of printing — hence
 * a shared pipeline, not a shared function) and the kdb `(func;args)`
 * value-apply, which is not source at all.  q_runtime owns the paired teardown. */
void q_ctx_install_remote_hooks(void);

/* Console teardown before exit.  The context knows only that SOMETHING may need
 * restoring before `.z.exit` runs (its 0N! output must land on a cooked
 * terminal); the front end that owns a terminal registers the how.  Unset —
 * qdoctest, wasm, a bare pipe — q_ctx_console_close is a no-op. */
void q_ctx_set_console_close(void (*fn)(void));
void q_ctx_console_close(void);

#endif /* Q_CTX_H */
