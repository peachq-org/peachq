/* q_fmt — format an evaluated q value as the q console would print it.
 *
 * For now this is a thin seam that delegates to rayforce's formatter; it is the
 * single place where q-correct output (`1b`, `42h`, `` `sym ``, typed nulls,
 * …) will be implemented, replacing the fallback case by case. */
#ifndef Q_FMT_H
#define Q_FMT_H

#include <rayforce.h>
#include <stddef.h>

/* Render val into buf (NUL-terminated, bounded by bufsz). */
void q_fmt(ray_t* val, char* buf, size_t bufsz);

/* ---- q console sink -------------------------------------------------------
 * `show` (and, in principle, `0N!`) print a value's q display as a SIDE EFFECT
 * and return generic null.  Since qdoc compares only the row's rendered output
 * and the REPL prints per line, the side-effect text is buffered here: the
 * host (qdoc / repl) drains it before/instead of the result and resets it once
 * per example / line. */
void        q_console_show(ray_t* val);   /* append q_fmt(val) + '\n' */
const char* q_console_str(void);          /* buffered text ("" if empty) */
void        q_console_reset(void);        /* clear the buffer */

#endif /* Q_FMT_H */
