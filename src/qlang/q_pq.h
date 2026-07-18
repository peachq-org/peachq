/* q_pq — PeachQ stdlib gate. `.pq.c.*` native primitives + the `\l pq` loader.
 * Nothing here runs at q_runtime_create; q_pq_load() fires ONLY from the
 * `\l pq` syscmd gate (q_sys.c). The embedded pq.q (pq_gen.h) is the single
 * visible manifest; `.pq.c.*` are internal/unstable natives bound at the gate. */
#ifndef OPENQ_Q_PQ_H
#define OPENQ_Q_PQ_H

/* Bind the `.pq.c.*` natives, then eval the embedded pq.q line-at-a-time.
 * Idempotent: re-loads simply re-bind and re-eval (nonlegacy setter is silent,
 * assignments overwrite). Fails fast (no pq.q eval) if the native bind fails.
 * Called only from the `\l pq` gate. */
void q_pq_load(void);

#endif /* OPENQ_Q_PQ_H */
