/* q_repl — the q REPL loop, factored out of the binary so tests can drive the
 * exact same console behaviour in-process (prompt, input echo, q-formatted
 * output) and diff the captured transcript. */
#ifndef Q_REPL_H
#define Q_REPL_H

#include <stdio.h>

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

/* Run a q startup script (`q file.q`): evaluate each line silently (no prompt,
 * no echo, no auto-display of results) — only explicit side-effects (show/0N!)
 * are written to `out`, matching kdb script-load semantics.  Returns 0 on
 * success, non-zero if the file could not be opened. */
int q_repl_run_file(const char* path, FILE* out, FILE* err);

#endif /* Q_REPL_H */
