/* q_sys — the unified q `\`-command dispatcher.  A single Q_SYS[] manifest
 * (mirroring q_dotz.c's static-table idiom) maps each `\`-command token to its
 * handler + flags.  Stage 2 enumerates EVERY kdb `\`-command as a row:
 *   - the five working commands (\d \v \f \a \S) keep their exact behaviour;
 *   - config getter/setter commands (\P \c \o \z) accept their SETTER form as a
 *     silent no-op (kdb-true empty output) and return 'nyi for the GETTER form;
 *   - every other known-but-unimplemented command returns 'nyi;
 *   - an unknown `\`-token shells out via system() (REPL only) or falls through
 *     to the parser (doctest/script — never executes, the runner-safe path);
 *   - \\ (quit) and bare \ (terminate / toggle) are flag-gated so they never
 *     exit() the process from the non-interactive (doctest) path.
 * Frozen-clean: no src/lang or src/ops edits. */
#ifndef Q_SYS_H
#define Q_SYS_H

#include <rayforce.h>
#include <stddef.h>

/* Re-initialize the rng to kdb's constant startup seed (-314159i,
 * basics/syscmds.md \S) and record it as the last-initialized seed.  Called by
 * q_runtime_create; `\S n` re-initializes thereafter. */
void q_sys_seed_init(void);

/* Dispatch a console line that may begin with `\`.  Sets *handled and returns an
 * OWNED value to display (NULL = handled silently), or an OWNED RAY_ERROR.
 *
 * `is_repl` distinguishes the entry point:
 *   1 = interactive / piped REPL (q_repl.c) — `\\` and bare `\` may exit(); an
 *       unknown `\foo` shells out via system() (kdb-true).
 *   0 = doctest / script / unit (qdoc.c, tests) — exit/terminate commands are
 *       benign no-ops (NEVER exit — the runner-kill hazard); an unknown `\foo`
 *       is NOT executed but left to the parser (*handled == 0, byte-identical to
 *       the historic fall-through — keeps the corpus's real \ls/\cat/\curl rows
 *       from mutating the filesystem or hitting the network under CI).
 *
 * A known command token always sets *handled == 1.  A line that does not begin
 * with `\` returns NULL with *handled == 0 (not a syscmd; caller parses it). */
ray_t* q_sys_dispatch(const char* line, size_t n, int* handled, int is_repl);

#endif /* Q_SYS_H */
