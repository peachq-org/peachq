/* q_duckdb — peachq's `.duckdb` namespace (the DuckDB bridge).
 * See docs/duckdb-api.md and q_duckdb.c. */
#ifndef Q_DUCKDB_H
#define Q_DUCKDB_H

#include <rayforce.h>
#include <stddef.h>

/* The `$name` parameters of a SQL statement — distinct, first-occurrence order, an 11h vector (empty for none) — as
 * DuckDB's lexer reads them: `$1` and `?` are named by number.  Pure text, no library (q_duckdb_params.c). */
ray_t* q_duckdb_params(const char* s, size_t n);

/* Bind the `.duckdb.*` verbs into the q env (dotted env binds, the .Q.c.*
 * pattern).  Does NOT touch the DuckDB library — that is dlopen'd lazily on
 * first use so `q` starts fine without it. */
void q_duckdb_register(void);

/* Close every live connection and the main database (runtime teardown — keeps
 * .qcmd suites isolated).  No-op if never loaded. */
void q_duckdb_reset(void);

/* The file the main database opens on first need (`-duckdb path`); wins over
 * PEACHQ_DUCKDB_MAIN.  Read at that open, so it must precede any DuckDB use. */
void q_duckdb_main_path_set(const char* path);

#endif /* Q_DUCKDB_H */
