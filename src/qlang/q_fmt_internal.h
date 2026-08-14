/* q_fmt internals shared with the pipe renderer (q_console.c).
 *
 * The ONE thing the modern render borrows from q_fmt.c: its table-CELL
 * renderer.  Cells must read identically in both modes (sym cells bare, bool
 * cells bare — the column carries the type, float cells without the `f`, null
 * cells blank), so the pipe table inherits it rather than forking a second
 * cell renderer.  Nothing else crosses; the legacy table/keyed renderers stay
 * private to q_fmt.c. */
#ifndef Q_FMT_INTERNAL_H
#define Q_FMT_INTERNAL_H

#include <rayforce.h>
#include <stddef.h>

/* One table cell of `col` at `row`, NUL-terminated into out[0..outsz). */
void q_fmt_cell(ray_t* col, int64_t row, int blank_null, char* out, size_t outsz);

#endif /* Q_FMT_INTERNAL_H */
