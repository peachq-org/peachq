/* q_apply — the noun-head application dispatcher (spec 2c-1, per-head
 * semantic table).  Registered into eval's apply hook by q_builtins_register;
 * receives the EVALUATED head and EVALUATED args (borrowed).  Returns an
 * OWNED result, or NULL to DECLINE — the caller then raises its original
 * "not callable" error byte-identical.  String-atom heads always decline
 * (the string-as-sequence model is a deferred, separate decision). */
#ifndef Q_APPLY_H
#define Q_APPLY_H

#include <rayforce.h>
#include <stdint.h>

ray_t* q_apply_noun(ray_t* head, ray_t** args, int64_t n);

/* The q `@` / `.` VARY registry entrypoints (manifest rows in q_ops.c):
 * 2 args Apply/Index; 3 args Trap (callable head) or Amend (data head);
 * 4 args quaternary Amend.  Bodies live here with the noun dispatcher —
 * ref/apply.md + ref/amend.md. */
ray_t* q_at_wrap(ray_t** args, int64_t n);
ray_t* q_dot_wrap(ray_t** args, int64_t n);

#endif /* Q_APPLY_H */
