/* q_pq — the `\l pq` standard-library gate.  Nothing here runs at
 * q_runtime_create; q_pq_load() fires ONLY from the `\l pq` syscmd gate
 * (q_sys.c) and evals the embedded lib/ bundle (lib_gen.h). */
#ifndef PEACHQ_Q_PQ_H
#define PEACHQ_Q_PQ_H

/* Eval the embedded lib/ bundle through the script seam.  Idempotent:
 * re-loads re-eval (setters are silent, assignments overwrite). */
void q_pq_load(void);

#endif /* PEACHQ_Q_PQ_H */
