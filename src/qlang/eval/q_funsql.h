/* q_funsql — functional qSQL: the rank>2 overload arms of `?` (Select/Exec/
 * Simple Exec/Vector Conditional) and `!` (Update/Delete), basics/funsql.md.
 * Lives in eval/ because every phrase evaluates through q_eval under a
 * column scope (qsql.md names-in-subphrases).  Both wrappers are the manifest
 * QR_FNV recipes for the dyad rows: n==2 delegates to the classic dyadic
 * bodies (roll/find, dict-make), n>2 is the qSQL matrix. */
#ifndef Q_FUNSQL_H
#define Q_FUNSQL_H

#include <rayforce.h>
#include <stdint.h>

ray_t* q_funsql_ques_wrap(ray_t** args, int64_t n);
ray_t* q_funsql_bang_wrap(ray_t** args, int64_t n);

/* `#` / `_` on the ENTRIES axis — the ONE selection home: one index off the
 * domain applied to both halves, take keeping what Find found and drop the
 * complement (dict keys, keyed tables, and a table's columns through flip).
 * NULL = not an entries selection, the take/drop wrapper's fall-through. */
ray_t* q_funsql_entries_take(ray_t* x, ray_t* y);
ray_t* q_funsql_entries_drop(ray_t* x, ray_t* y);

/* drop named dict entries: `(key d) except keys` re-indexed (ref/drop.md) */
ray_t* q_funsql_dict_drop_keys(ray_t* keys, ray_t* d);

#endif /* Q_FUNSQL_H */
