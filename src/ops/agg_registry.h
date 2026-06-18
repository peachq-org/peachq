/* src/ops/agg_registry.h — resolve (agg_kind, in_type) → accumulator vtable.
 * agg_kind values are the existing OP_SUM/OP_MIN/... opcodes from ops/ops.h. */
#ifndef RAY_OPS_AGG_REGISTRY_H
#define RAY_OPS_AGG_REGISTRY_H

#include "ops/agg_acc.h"

/* Returns NULL when no specialized vtable is registered for (agg_kind, in_type).
 * In later phases NULL routes to the buffered-eval oracle accumulator. */
const agg_vtable_t* agg_resolve(uint16_t agg_kind, int8_t in_type);

#endif /* RAY_OPS_AGG_REGISTRY_H */
