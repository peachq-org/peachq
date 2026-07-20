/* q_json — `.j` JSON namespace (ref/dotj.md).  C homes reached via `-31!`/`-29!`
 * (q_bang.c); the q names are bound in src/qlang/j.q.  Deserialize uses yyjson (MIT). */
#ifndef QLANG_Q_JSON_H
#define QLANG_Q_JSON_H

#include <rayforce.h>

ray_t* q_json_serialize(ray_t* x);    /* .j.j: value -> JSON string */
ray_t* q_json_deserialize(ray_t* x);  /* .j.k: JSON string -> value */

#endif
