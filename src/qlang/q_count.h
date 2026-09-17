/* q_count — kdb `count` as a C int64, the ONE count accessor of the q layer
 * (q_count_fn is the same law boxed as a q value).
 *
 * ray_len is the block's SLOT count: 2 for a dict (keys, vals) and for a table
 * (names, cols), the slot count of a carrier, garbage on a STR atom.  It is
 * poisoned below with the per-type dodges it bred; a read that truly wants the
 * slots is ray_block_len.  Every src/qlang C file makes this its FIRST include
 * (tools/lint-q-count.sh), so the poison sees every header that follows.
 *
 * vec/list/enum -> elements; STR atom -> bytes; table -> rows; dict / keyed
 * table -> its key domain's count; provider carrier -> the provider's count;
 * atom / function value -> 1; lazy -> the materialized count; NULL pointer,
 * error or a non-q block (index, selection) -> -1.  Allocation-free except
 * the carrier/lazy tail: the iterators call this once per application. */
#ifndef Q_COUNT_H
#define Q_COUNT_H

#include <rayforce.h>
#include "qlang/base/q_type.h"

int64_t q_builtins_count_boxed(ray_t* x);

static inline int64_t q_count(const ray_t* v) {
    ray_t* x = (ray_t*)v;                        /* the base accessors are not const-correct */
    if (!x || RAY_IS_ERR(x)) return -1;
    if (x->type == RAY_LIST || ray_is_vec(x) || x->type == RAY_ENUM) return x->len;
    if (x->type < 0) return x->type == -RAY_STR ? (int64_t)ray_str_len(x) : 1;
    if (x->type == RAY_TABLE) return ray_table_nrows(x);
    if (x->type == RAY_DICT && q_type_coord_kind(x) != Q_COORD_PROVIDER) return q_count(ray_dict_keys(x));
    return q_builtins_count_boxed(x);
}

#undef ray_len
#pragma GCC poison ray_len ray_dict_len ray_table_nrows

#endif
