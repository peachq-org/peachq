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
void q_console_write(const char* s, size_t n);  /* raw bytes (kdb 1/-1 handles) */

/* `--nonlegacy` pipe-table display: a deliberate kdb divergence, OFF by
 * default (spec: docs/superpowers/specs/2026-07-16-nonlegacy-display-design.md).
 * Gated at ONE branch in q_fmt_console — the console seam — never q_fmt_body,
 * so round-trip surfaces (`string`, `-3!`, CSV, cells) keep the legacy text.
 * Arm/disarm: qmain `--nonlegacy`, the `\nonlegacy` syscmd; q_runtime_destroy
 * resets (no cross-runtime leak). */
void q_console_pipe_enable(void);
void q_console_pipe_disable(void);
bool q_console_pipe_on(void);




#endif /* Q_CONSOLE_H */
