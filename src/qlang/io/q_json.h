/* q_json — `.j` JSON namespace (ref/dotj.md).  C homes reached via `-31!`/`-29!`
 * (q_bang.c); the q names are bound in src/qlang/j.q.  Deserialize uses yyjson (MIT). */
#ifndef QLANG_Q_JSON_H
#define QLANG_Q_JSON_H

#include <rayforce.h>

ray_t* q_json_serialize(ray_t* x);    /* .j.j: value -> JSON string */
ray_t* q_json_deserialize(ray_t* x);  /* .j.k: JSON string -> value */

/* How a stream's records are FRAMED: the `format` option's three values, and the
 * one thing a recognised suffix states (user-docs/json.md § Framing). */
typedef enum { Q_JSON_AUTO = 0, Q_JSON_ARRAY, Q_JSON_ND } q_json_frame_t;

/* THE C door on the reader: a JSON RESOURCE (any read0 identifier) or CONTENT
 * decoded into a table under `frame`, independent of the `.j` namespace's
 * binding.  Owned table, or an owned error. */
ray_t* q_json_read_table(ray_t* src, q_json_frame_t frame);

/* Bind the `.j.i.read` / `.j.i.info` natives lib/j.q's `.j.read` / `.j.info`
 * call.  At the `\l pq` GATE, not q_runtime_create: `.j` pre-gate is kdb's three. */
void q_json_register(void);

#endif
