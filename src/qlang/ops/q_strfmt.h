/* q_strfmt — bind the `.str.i.printf` / `.str.i.format` natives.  Called at the
 * `\l pq` gate; lib/str.q wraps the public `.str.printf` / `.str.format`. */
#ifndef QLANG_OPS_Q_STRFMT_H
#define QLANG_OPS_Q_STRFMT_H

void q_strfmt_register(void);

#endif
