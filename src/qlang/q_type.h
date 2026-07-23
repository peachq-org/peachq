/* q_type — THE type-axis home: q-side lane accessors, lane predicates, and
 * (future) type-signature tables. The base accessors (as_i64/as_f64 in
 * lang/internal.h) stay in the quarry; q's temporal-lane extensions live HERE,
 * so src/lang/internal.h remains byte-identical to its fork point. This is the
 * edit-boundary rule made physical: base is touched only for type enrollment,
 * every other extension wraps in a q_ home. */
#ifndef QLANG_Q_TYPE_H
#define QLANG_Q_TYPE_H

#include <rayforce.h>
#include <stdint.h>

/* Integer lane read, temporal-complete: the int-backed temporals read
 * explicitly here (TEMPORAL32 from the i32 payload, TEMPORAL64 from i64); every
 * other atom delegates to the base as_i64. DATETIME is f64-backed
 * (rayforce.h:129) — read it via as_f64, never here. Nulls are the caller's to
 * test (RAY_ATOM_IS_NULL) before the read. */
int64_t q_type_as_i64(ray_t* x);

/* Admission for a verb reading any numeric OR temporal lane via as_f64
 * (signum): exactly the atoms as_f64 has a lane for (is_temporal omits
 * DATETIME). cmp.c open-codes this same union — a future drain consumer. */
int q_type_is_numeric_or_temporal(ray_t* x);

/* Bool atom — the lane predicate, so bool's b->i promotion is a named lane
 * test, not an inline type probe in a verb wrapper. */
int q_type_is_bool(ray_t* x);

#endif /* QLANG_Q_TYPE_H */
