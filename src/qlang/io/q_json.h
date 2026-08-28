/* q_json — `.j` JSON namespace (ref/dotj.md).  C homes reached via `-31!`/`-29!`
 * (q_bang.c); the q names are bound in src/qlang/j.q.  Deserialize uses yyjson (MIT). */
#ifndef QLANG_Q_JSON_H
#define QLANG_Q_JSON_H

#include <rayforce.h>

ray_t* q_json_serialize(ray_t* x);    /* .j.j: value -> JSON string */
ray_t* q_json_deserialize(ray_t* x);  /* .j.k: JSON string -> value */

/* Bind the `.j.i.read` / `.j.i.info` natives lib/j.q's `.j.read` / `.j.info`
 * call.  At the `\l pq` GATE, not q_runtime_create: `.j` pre-gate is kdb's three. */
void q_json_register(void);

#endif
