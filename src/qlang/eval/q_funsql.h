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

#endif /* Q_FUNSQL_H */
