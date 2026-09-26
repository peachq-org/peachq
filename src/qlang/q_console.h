/* q_console — the console SINK + its routing config (q_console.c).
 * Base doctrine (src/lang/format.h): pure string core, print veneers, config
 * beside the sink.  q_fmt.c is the pure formatter; this layer writes the
 * side-effect text `show`/`0N!`/the 1 -1 2 -2 handles emit THROUGH to its
 * destination as it is issued: each call is one whole logical write (payload
 * plus its newline) under one lock; one thread's writes keep issue order across
 * stdout and fd 2, and writers outside the sink stay ordered because every sink
 * write flushes.  A door's stream must not itself write to the console. */
#ifndef Q_CONSOLE_H
#define Q_CONSOLE_H

#include <rayforce.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

int   q_console_show(ray_t* val);     /* the whole display + '\n'; -1 = could not build it */
int   q_console_write(const char* s, size_t n, bool nl);      /* the door's stdout; always 0 (errors not escalated) */
int   q_console_write_err(const char* s, size_t n, bool nl);  /* fd 2 (handles 2/-2); -1 = the write failed */
void  q_console_flush(void);          /* the exit path's fflush (issue #23) */
/* The running door names its thread's stdout stream (NULL = stdout) and restores the previous one when it returns. */
FILE* q_console_door(FILE* out);
/* The child of a fork writes the process stdout, and no thread of the parent can still hold the lock. */
void  q_console_forked(void);

/* Modern pipe-table display: a deliberate kdb divergence, the `./q`/wasm
 * DEFAULT (`-classic` at launch / `\classic 1` at runtime opt out; spec:
 * docs/superpowers/specs/2026-07-16-nonlegacy-display-design.md).  The global
 * itself starts OFF — each front end arms it — so embedders stay kdb-true.
 * Gated at ONE branch in q_fmt_console — the console seam — never
 * q_fmt_body, so round-trip surfaces (`string`, `-3!`, CSV, cells) keep the
 * legacy text.  q_runtime_destroy resets (no cross-runtime leak). */
void q_console_pipe_enable(void);
void q_console_pipe_disable(void);
bool q_console_pipe_on(void);

/* `\c` console DISPLAY clip (config beside the sink — base doctrine, format.h).
 * q_console_clip fills rows/cols with the live `\c` size and returns true iff
 * clipping is armed; q_fmt.c's console emitter reads it.  q_console_clip_set is
 * the ONE setter — the `\c` syscmd (q_sys.c) and startup call it; it coerces
 * each value to the documented [10,2000] range (basics/syscmds.md `\c`) and
 * ARMS clipping.  kdb has no off-switch — the range ceiling (2000) is the
 * batch idiom, so there is no disable.  peachq extension: an axis set to `0N`
 * (NULL_I64) is AUTO — resolved to the live terminal size at each render
 * (resize-following), same [10,2000] coercion, 25/80 fallback off-tty.
 * q_console_clip_setting reads back the raw setting (NULL_I64 for an auto
 * axis) for the `\c` getter's round-trip display. */
bool q_console_clip(int32_t* rows, int32_t* cols);
void q_console_clip_set(int64_t rows, int64_t cols);
void q_console_clip_setting(int64_t* rows, int64_t* cols);

/* THE colour on/off law every colour emitter consults: PEACHQ_COLORS=1/0 outright, then a non-empty NO_COLOR (off)
 * or FORCE_COLOR (on), else colour iff the emitter's output is a terminal (`tty`) whose TERM is not dumb. */
bool q_console_color(bool tty);

/* lib/pq.q's natives `.pq.i.termsize` and `.pq.i.cancolor`; and termsize again as `.help.i.termsize`, bound at boot. */
void q_console_pq_register(void);
void q_console_help_register(void);

#endif /* Q_CONSOLE_H */
