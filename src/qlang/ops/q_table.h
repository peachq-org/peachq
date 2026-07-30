/* q_table — the table family's own seam.  The public table surface (verbs,
 * q_table_flatten/row_at/at/map_cols) is declared in q_registry.h and
 * q_registry_internal.h; what follows is the shared machinery the family's
 * three files (q_table.c, q_insert.c, q_setops.c) build their verbs from.
 *
 * Container knowledge lives in the apply module's family lifts and here — a
 * verb body below the seam never re-derives row identity or column assembly. */
#ifndef QLANG_OPS_Q_TABLE_H
#define QLANG_OPS_Q_TABLE_H

#include "rayforce.h"

/* Row equality over the FIRST ncmp columns of two tables (boxed q-match). */
int q_table_row_eq(ray_t* ta, int64_t ra, ray_t* tb, int64_t rb, int64_t ncmp);

/* THE grouping home: first-occurrence row grouping over the FIRST ncmp
 * columns.  Fills gid[nrows] with each row's group id and rep[ngroups] with
 * each group's first row; returns the group count, -1 on allocation failure.
 * Dense-hashed through agg_group_keys when every compared column qualifies,
 * boxed row-compare otherwise — `group`, `xgroup` and row dedup share it. */
int64_t q_table_row_groups(ray_t* t, int64_t ncmp, int64_t* gid, int64_t* rep);

/* Collapse per-column boxed accumulators into a table, taking column names
 * from `names` at offset c0.  CONSUMES every accs[c] — an entry that IS an
 * error propagates as the result — so a caller never unwinds by hand. */
ray_t* q_table_cols_from_accs(ray_t* names, int64_t c0, ray_t** accs, int64_t nc);

/* n copies of atom `a` as a collapsed column (broadcast). */
ray_t* q_table_bcast_col(ray_t* a, int64_t n);

/* Column index of name id in t, or -1. */
int64_t q_table_col_index(ray_t* t, int64_t nm);

/* Resolve a table operand that may be BY NAME (-RAY_SYM naming a global):
 * returns the borrowed target and sets *sym_out to the name id, or -1 for
 * by-value.  NULL => not a table. */
ray_t* q_table_operand(ray_t* y, int64_t* sym_out);

/* Most columns any one table verb will assemble at once (fixed accumulator
 * arrays); past it the verb returns 'limit. */
#define Q_TABLE_MAX_COLS 64

#endif
