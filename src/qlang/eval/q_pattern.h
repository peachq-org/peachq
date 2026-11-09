/* q_pattern — THE pattern matcher (basics/pattern.md), the list slice: names, the null pattern, constants, list
 * patterns and `p:`c` type checks, read off the parser's own target trees.  Assignment (q_eval.c) and lambda entry
 * (q_eval_apply.c) both collect here and commit only a whole match. */
#ifndef Q_PATTERN_H
#define Q_PATTERN_H

#include <rayforce.h>
#include <stdint.h>

/* the kdb type number a type-check symbol names — `j is -7, `J is 7 — or 0 when it names none */
int64_t q_pattern_type_num(int64_t sym);
/* the same for a tree node: the type number when x is a type check's enlisted symbol ,`c, else 0 */
int64_t q_pattern_type_check(ray_t* x);
/* exact against q `type`, no widening (owner ruling 2026-08-12) */
int q_pattern_type_ok(ray_t* v, int64_t tnum);

/* is every node of this target tree in the slice?  A filter, dictionary, table or operator pattern is not. */
int q_pattern_supported(ray_t* pat);

typedef void (*q_pattern_name_fn)(int64_t sym, void* ctx);
void q_pattern_names(ray_t* pat, q_pattern_name_fn visit, void* ctx);

/* Match v against a supported pattern, appending one (name sym atom; value) pair per name to the general list
 * *binds and writing nothing: NULL on a match, else the owned 'match / 'length / 'type error.  pat, v borrowed. */
ray_t* q_pattern_collect(ray_t* pat, ray_t* v, ray_t** binds);

#endif /* Q_PATTERN_H */
