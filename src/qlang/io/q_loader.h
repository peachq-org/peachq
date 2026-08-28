/* q_loader — the TARGET seam .csv.read and .j.read share.
 *
 * A target is TWO things, and a caller uses both.  As a SCHEMA ORACLE an existing symbol target
 * answers, before a cell parses, what type a column is and whether it exists at all
 * (has_schema / names / chars).  As a SINK it receives whole batches after parsing; only that
 * leg is lambda-shaped.  Why they stay apart: ADR 0007 §9. */
#ifndef QLANG_Q_LOADER_H
#define QLANG_Q_LOADER_H

#include <rayforce.h>
#include <stdint.h>

typedef struct q_loader_sink {
    int      kind;        /* 0 return the table, 1 a global name, 2 a rank-3 lambda */
    ray_t*   target;      /* borrowed: the caller's argument */
    int      upsert;      /* a KEYED global target upserts where an unkeyed one inserts */
    int      has_schema;  /* the named global exists as a table: its meta is the oracle */
    int64_t* names;
    char*    chars;       /* per target column: its type char; '*' for a list/string column */
    int64_t  n;
} q_loader_sink;

/* `::`/elided is kind 0; a lambda of any rank but 3 is 'rank; anything else 'type. */
ray_t*  q_loader_sink_open(q_loader_sink* s, ray_t* target);
void    q_loader_sink_free(q_loader_sink* s);
int64_t q_loader_sink_find(const q_loader_sink* s, int64_t name);   /* the target's column index, or -1 */

/* ONE batch.  tbl/errdata stay the caller's; errdata is read only by a lambda target. */
ray_t* q_loader_sink_emit(q_loader_sink* s, ray_t* tbl, ray_t* errdata, int64_t chunk, int64_t rows);

/* THE RENAME, rewriting `names` in place: `.q.xcol` CALLED, not copied — so a key naming
 * no column is its own 'length (ref/cols.md). */
ray_t* q_loader_rename(ray_t* spec, int64_t* names, int64_t n);

/* The summary both readers answer; q_loader_summary CONSUMES `ignored` and `types`. */
ray_t* q_loader_types_dict(const int64_t* names, const char* chars, int64_t n);
ray_t* q_loader_syms(const int64_t* names, int64_t n);
ray_t* q_loader_summary(int64_t rows, int64_t rejected, int64_t chunks, ray_t* ignored, ray_t* types);

#endif /* QLANG_Q_LOADER_H */
