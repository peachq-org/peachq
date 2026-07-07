/* q_json — openq's `.j` JSON namespace (ref/dotj.md).
 *
 *   .j.j  x        serialize a q value -> JSON string (RAY_STR)   [hand-rolled]
 *   .j.k  s        deserialize a JSON string -> q value           [yyjson + glue]
 *   .j.jd (x;d)    serialize; thin wrapper over .j.j (0w->null moot in openq)
 *
 * Pure q-layer: bound as env unaries (the `.Q.*` precedent) by q_json_register,
 * called from q_builtins_register.  Serialize is hand-rolled type-dispatch over
 * ray_t; deserialize delegates raw tokenize/validate/unescape/number-parse to the
 * vendored yyjson (third_party/yyjson, MIT) and owns only the node->ray_t mapping. */
#ifndef QLANG_Q_JSON_H
#define QLANG_Q_JSON_H

#include <rayforce.h>

/* Bind .j.j / .j.k / .j.jd into the env.  Call from q_builtins_register. */
void q_json_register(void);

/* Exposed for unit tests (test/q_json.c).  Both return OWNED values (rc=1);
 * errors are RAY_ERROR objects. */
ray_t* q_json_serialize(ray_t* x);    /* .j.j  — value  -> RAY_STR (JSON)     */
ray_t* q_json_deserialize(ray_t* x);  /* .j.k  — RAY_STR (JSON) -> q value    */

#endif /* QLANG_Q_JSON_H */
