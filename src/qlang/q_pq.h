/* q_pq — the `\l pq` standard-library gate.  Nothing here runs at
 * q_runtime_create; q_pq_load() fires ONLY from the `\l pq` syscmd gate
 * (q_sys.c) and evals the embedded lib/ bundle (lib_gen.h). */
#ifndef PEACHQ_Q_PQ_H
#define PEACHQ_Q_PQ_H

#include <rayforce.h>

/* Eval the embedded lib/ bundle through the script seam.  Idempotent:
 * re-loads re-eval (setters are silent, assignments overwrite).  Returns NULL
 * on a full load, else the abort's owned re-signal (the script seam's law). */
ray_t* q_pq_load(void);

#endif /* PEACHQ_Q_PQ_H */
