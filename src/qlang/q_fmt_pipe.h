/* q_fmt_pipe — the `--nonlegacy` console table render (q_fmt_pipe.c).
 *
 * A DELIBERATE DIVERGENCE from kdb display, OFF by default and armed only by
 * the launch flag: a markdown-style pipe table (`rows: N columns: M`, name row,
 * type row, divider, data rows, `... (showing first N of M rows)` footer) plus
 * an optional <=2-line digest.  Shape: docs/superpowers/specs/
 * 2026-07-16-nonlegacy-display-design.md + references/example-to-string.txt.
 *
 * Gated at ONE branch in q_fmt_console (q_fmt.c) — the CONSOLE seam.  NOT in
 * q_fmt_body, which q_fmt (`string`, `-3!`, CSV, every recursive cell) shares:
 * this mode is console display only, so the round-trip surfaces keep the legacy
 * text.  The legacy renderer q_fmt_table is not touched at all. */
#ifndef Q_FMT_PIPE_H
#define Q_FMT_PIPE_H

#include <rayforce.h>
#include <stdbool.h>

/* Arm the mode.  qmain's arg loop is the sole caller (launch-only: there is no
 * runtime toggle, so one process has exactly one display path). */
void q_pipe_enable(void);
bool q_pipe_on(void);

/* True iff val is a shape this mode renders: a table, or a keyed table
 * (dict of table!table).  Everything else falls through to the legacy path. */
bool q_pipe_is_table(ray_t* val);

/* Render val as the pipe table into buf (NUL-terminated, bounded by bufsz).
 * Honours the live `\c rows cols`; unarmed `\c` (a batch script load) means no
 * budget, so every row prints and neither footer nor digest appears. */
void q_pipe_console(ray_t* val, char* buf, size_t bufsz);

#endif /* Q_FMT_PIPE_H */
