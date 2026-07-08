/* q_sys — the unified q `\`-command dispatcher.  A single Q_SYS[] manifest
 * (mirroring q_dotz.c's static-table idiom) maps each `\`-glyph to its handler.
 * Stage 1 folds the five working commands (\d \v \f \a \S) behind this seam
 * with no behaviour change; later stages add enumerate-all + 'nyi + shell
 * fallback + flag enforcement.  Frozen-clean: no src/lang or src/ops edits. */
#ifndef Q_SYS_H
#define Q_SYS_H

#include <rayforce.h>
#include <stddef.h>

/* Re-initialize the rng to kdb's constant startup seed (-314159i,
 * basics/syscmds.md \S) and record it as the last-initialized seed.  Called by
 * q_runtime_create; `\S n` re-initializes thereafter. */
void q_sys_seed_init(void);

/* Dispatch a console line that may begin with `\`.  Handles \d [ns], \v [ns],
 * \f [ns], \a [ns], \S [n]; sets *handled and returns an OWNED value to display
 * (NULL = handled silently), or an OWNED RAY_ERROR.  An unrecognized `\cmd`
 * leaves *handled == 0 (the caller falls through to the parser). */
ray_t* q_sys_dispatch(const char* line, size_t n, int* handled);

#endif /* Q_SYS_H */
