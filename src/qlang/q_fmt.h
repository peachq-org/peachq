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

#endif /* Q_FMT_H */
