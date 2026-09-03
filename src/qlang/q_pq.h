/* q_pq — the `\l pq` standard-library gate.  Nothing here runs at
 * q_runtime_create; q_pq_load() fires ONLY from the `\l pq` syscmd gate
 * (q_sys.c) and evals the embedded lib/ bundle (lib_gen.h).  The ONE
 * exception is the help-db seam below: bound at boot, loaded at first help
 * access — by neither of the two events this file otherwise serves. */
#ifndef PEACHQ_Q_PQ_H
#define PEACHQ_Q_PQ_H

#include <rayforce.h>

/* Eval the embedded lib/ bundle through the script seam.  Idempotent:
 * re-loads re-eval (setters are silent, assignments overwrite).  Returns NULL
 * on a full load, else the abort's owned re-signal (the script seam's law). */
ray_t* q_pq_load(void);

/* Bind `.help.i.loaddb` — the on-demand door onto the embedded lib/help-db.q
 * bundle (the generated builtin one-liners).  Called at runtime create so a bare
 * prompt has it.  FIRST HELP ACCESS is the only trigger (owner 2026-09-03): every
 * reader door in help.q calls it, `\l pq` does NOT, and the load is once per
 * runtime. */
void q_pq_helpdb_register(void);

/* Forget the once-per-runtime help-db flag (a fresh runtime reloads it). */
void q_pq_reset(void);

#endif /* PEACHQ_Q_PQ_H */
