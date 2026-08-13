/* q_strns — bind the `.str.i.*` strip natives.  Called at the `\l pq` gate;
 * lib/str.q wraps the public `.str.*` spellings on top. */
#ifndef QLANG_OPS_Q_STRNS_H
#define QLANG_OPS_Q_STRNS_H

void q_strns_register(void);

#endif
