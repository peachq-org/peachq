/* q_repl — the q REPL loop, factored out of the binary so tests can drive the
 * exact same console behaviour in-process (prompt, input echo, q-formatted
 * output) and diff the captured transcript. */
#ifndef Q_REPL_H
#define Q_REPL_H

#include <stdio.h>

/* Restore the live terminal and save history, if any.  Idempotent. */
void q_repl_console_close(void);

typedef struct ray_poll ray_poll_t;   /* fwd — full API in core/poll.h */

/* Read q source lines from `in`, evaluating each and writing the console
 * transcript to `out`:
 *
 *     q)<input>
 *     <q_fmt(result)>
 *
 * A `q)` prompt is written before every read.  When `echo` is non-zero the
 * input line is written after the prompt (so a piped / captured session
 * reproduces what a terminal would show); pass 0 for an interactive tty where
 * the terminal already echoes.  Evaluation errors go to `err`, never `out`, so
 * a captured transcript shows no output line for an unsupported input.
 *
 * Requires an initialised rayforce runtime. */
void q_repl_run(FILE* in, FILE* out, FILE* err, int echo);

/* Poll-driven REPL: register stdin on `poll` (alongside any IPC listener
 * already on it) and run ONE event loop, so the console and IPC clients are
 * serviced concurrently — mirrors rayforce's own run_interactive (repl.c).
 *
 * stdin_tty != 0 drives the line-editor console (same behaviour as
 * q_repl_run's interactive mode); 0 drives the piped transcript loop (prompt
 * + echo, identical output shape to the fgets loop).  `\\` / `exit x`
 * terminate inside the eval (q_exit).  On stdin EOF: when have_listener != 0
 * the loop keeps running so IPC clients stay served (the daemon shape);
 * otherwise the loop exits.
 *
 * Returns 0 after the poll loop has run and exited; -1 when stdin cannot be
 * poll-driven on this platform (Windows IOCP has no stdin selector; epoll
 * rejects a regular-file redirect) — the caller falls back to the serial
 * REPL-then-serve shape. */
int q_repl_run_poll(ray_poll_t* poll, FILE* out, FILE* err,
                    int stdin_tty, int have_listener);

/* Run a q startup script (`q file.q`): evaluate each line silently (no prompt,
 * no echo, no auto-display of results) — only explicit side-effects (show/0N!)
 * are written to `out`, matching kdb script-load semantics.  Returns 0 on
 * success, non-zero if the file could not be opened. */
int q_repl_run_file(const char* path, FILE* out, FILE* err);

/* Mark this process as having an IPC listener (startup `-p` or a runtime `\p`).
 * The unified poll loop then keeps serving past stdin EOF instead of exiting —
 * a client that `\p`s a port becomes a long-lived server, like kdb/rayforce. */
void q_repl_mark_listener_active(void);

/* Strip pasted kdb `q)` console prompts from the front of an intake line and
 * return the advanced pointer.  Repeated exact `q)` only: `q)q)2+2` -> `2+2`,
 * but the debug prompt `q))…`, namespace prompts `q.foo)`, and `k)` mode are
 * left untouched (the `s[2] != ')'` guard is what excludes `q))`).  No
 * leading-whitespace trim — an indented line is not a prompt.  A console/loader
 * affordance, applied at line-intake (run_one_line), never in the parser.
 * Exposed for direct unit testing (test/q_repl_strip.c). */
const char* q_strip_repl_prompt(const char* s);

#endif /* Q_REPL_H */
