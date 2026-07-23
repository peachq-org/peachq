/* q_type — the type-axis home (contract: q_type.h). These are the q-side lane
 * extensions relocated out of the frozen base header so it stays quarry-pure. */
#include "qlang/q_type.h"
#include "lang/internal.h"   /* base as_i64 + is_numeric / is_temporal */

int64_t q_type_as_i64(ray_t* x) {
    if (RAY_IS_TEMPORAL32(-x->type)) return (int64_t)x->i32;
    if (RAY_IS_TEMPORAL64(-x->type)) return x->i64;
    return as_i64(x);
}

int q_type_is_numeric_or_temporal(ray_t* x) {
    return is_numeric(x) || is_temporal(x) || RAY_IS_TEMPORALF(-x->type);
}

int q_type_is_bool(ray_t* x) {
    return x->type == -RAY_BOOL;
}
