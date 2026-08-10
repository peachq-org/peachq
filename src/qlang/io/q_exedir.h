/* q_exedir — where THIS executable lives: the anchor for anything that ships
 * beside `q`.  Today that is the optional dlopen'd modules (io/q_re2.c,
 * io/q_duckdb.c), which both need the same answer on all three hosts. */
#ifndef QLANG_IO_Q_EXEDIR_H
#define QLANG_IO_Q_EXEDIR_H

#include <stddef.h>

/* The directory holding the running executable, with no trailing separator.
 * 1 = dst filled; 0 = unknown, dst is "" (a caller must then skip the
 * candidate, never build a path out of it). */
int q_exedir(char* dst, size_t cap);

#endif /* QLANG_IO_Q_EXEDIR_H */
