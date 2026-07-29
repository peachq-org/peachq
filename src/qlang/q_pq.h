/* q_pq — PeachQ stdlib gate: the `\l pq` loader.  Nothing here runs at
 * q_runtime_create; q_pq_load() fires ONLY from the `\l pq` syscmd gate
 * (q_sys.c).  The embedded pq.q (pq_gen.h) is the single visible manifest —
 * pure q since the `.pq.c.*` rayfall natives were deleted (2026-07-29). */
#ifndef OPENQ_Q_PQ_H
#define OPENQ_Q_PQ_H

/* Eval the embedded pq.q line-at-a-time.  Idempotent: re-loads re-eval
 * (nonlegacy setter is silent, assignments overwrite). */
void q_pq_load(void);

#endif /* OPENQ_Q_PQ_H */
