/* q_console — the host-facing console SINK + its routing config (q_console.c).
 * Base doctrine (src/lang/format.h): pure string core, print veneers, config
 * beside the sink.  q_fmt.c is the pure formatter; this layer buffers the
 * side-effect text `show`/`0N!`/the 1/-1 handles emit — qdoc compares only a
 * row's rendered output and the REPL prints per line, so the host drains the
 * buffer before/instead of the result and resets it once per example / line. */
#ifndef Q_CONSOLE_H
#define Q_CONSOLE_H

#include <rayforce.h>
#include <stdbool.h>
#include <stddef.h>

void        q_console_show(ray_t* val);   /* append q_fmt_console(val) + '\n' */
const char* q_console_str(void);          /* buffered text ("" if empty) */
void        q_console_reset(void);        /* clear the buffer */
void        q_console_flush(void);        /* write + clear: the exit-path drain */
void q_console_write(const char* s, size_t n);  /* raw bytes (kdb 1/-1 handles) */

/* Modern pipe-table display: a deliberate kdb divergence, the `./q`/wasm
 * DEFAULT (`-classic` at launch / `\classic 1` at runtime opt out; spec:
 * docs/superpowers/specs/2026-07-16-nonlegacy-display-design.md).  The global
 * itself starts OFF — each front end arms it — so qdoctest and embedders stay
 * kdb-true.  Gated at ONE branch in q_fmt_console — the console seam — never
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

/* Bind the `.pq.i.termsize` native (live terminal rows/cols) — called from
 * the `\l pq` gate, beside the other .pq.i.* registrars. */
void q_console_pq_register(void);

#endif /* Q_CONSOLE_H */
