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

/* ---- the statement seam ---------------------------------------------------
 * ONE line of q source, executed the way every door executes it: strip a pasted
 * `q)` prompt, route a `\`-command through q_sys, else parse -> view-intercept
 * -> eval -> flush console side effects -> report.  print_result echoes a
 * non-null non-assignment value (REPL); zero discards it (script load — kdb
 * scripts are silent but for explicit side effects). */
void q_ctx_run_line(const char* s, size_t n, FILE* out, FILE* err, int print_result);

/* A file of q source under kdb script semantics: an INDENTED line continues the
 * previous logical one, blank/comment lines do not flush, `/`..`\` blocks skip,
 * and a trimmed singleton `\` exits the script.  Returns 1 if the file could
 * not be opened (error already printed), else 0. */
int q_ctx_run_file(const char* path, FILE* out, FILE* err);

/* Skip any pasted `q)` prompt(s): returns a pointer INTO s. */
const char* q_ctx_strip_prompt(const char* s);

/* ---- process lifecycle ----------------------------------------------------
 * A `\p N` (or startup `-p`) listener makes this process a server even if it
 * began as a client: like kdb, once it has a listener it keeps serving past
 * stdin EOF instead of exiting. */
void q_ctx_mark_listener_active(void);
int  q_ctx_listener_active(void);

/* Console teardown before exit.  The context knows only that SOMETHING may need
 * restoring before `.z.exit` runs (its 0N! output must land on a cooked
 * terminal); the front end that owns a terminal registers the how.  Unset —
 * qdoctest, wasm, a bare pipe — q_ctx_console_close is a no-op. */
void q_ctx_set_console_close(void (*fn)(void));
void q_ctx_console_close(void);

#endif /* Q_CTX_H */
