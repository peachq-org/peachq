/* q_bang — the q internal-function `-N!` dispatcher.  A single Q_BANG[] static
 * manifest ({id, name, valence, flags, handler}, in the q_sys.c / q_dotz.c
 * idiom) maps each NEGATIVE-integer-left `!` id to its handler + flags.  The
 * `!` verb wrapper (q_bang_wrap, q_registry.c) routes its negative-id branch
 * through q_bang_dispatch; the non-negative `N!` band (enkey/unkey/dict-make)
 * and the `0N!` show predicate stay in the wrapper.
 *
 * Direction B (bang-ops-internal-status.md, resolved 2026-07-08): for every id
 * that has a keyword/utility twin (`-1!`hsym, `-5!`parse, ...) the KEYWORD is
 * the single home and the `-N!` row is a THIN ALIAS routing to it — never a
 * re-implementation.  Ids without a twin, or whose subsystem openq lacks
 * (IPC/TLS/codec/enums/DARE), are BLOCKED rows (NULL handler) that return
 * 'nyi — the table is the first-class inventory. */
#ifndef Q_BANG_H
#define Q_BANG_H

#include <rayforce.h>
#include <stdint.h>

/* Dispatch a q internal function `-N!`.  `id` is the negative internal-fn id
 * (e.g. -8); `y` is the `!` RIGHT operand — the single argument for valence-1
 * ids (`-8!x`), or a 2-element list `(x;y)` for valence-2 ids (`-27!(x;y)`).
 * Borrowed `y`; returns an OWNED value or error.  Unknown or blocked ids
 * return 'nyi. */
ray_t* q_bang_dispatch(int64_t id, ray_t* y);

#endif /* Q_BANG_H */
