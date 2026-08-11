/* q_duckdb — peachq's `.duckdb` namespace (the DuckDB bridge).
 * See docs/duckdb-api.md and q_duckdb.c. */
#ifndef Q_DUCKDB_H
#define Q_DUCKDB_H

#include <stdbool.h>

/* Bind the `.duckdb.*` verbs into the q env (dotted env binds, the .Q.c.*
 * pattern).  Does NOT touch the DuckDB library — that is dlopen'd lazily on
 * first use so `q` starts fine without it. */
void q_duckdb_register(void);

/* Capability probe: true iff the DuckDB shared library resolves and passes
 * the version gate (>= 1.4).  Loads lazily on first call. */
bool q_duckdb_available(void);

/* Close every live connection and cached database (runtime teardown — keeps
 * .qcmd suites isolated).  No-op if never loaded. */
void q_duckdb_reset(void);

#endif /* Q_DUCKDB_H */
