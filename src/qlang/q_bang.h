/* q_bang — the q internal-function `-N!` dispatcher.  A `switch` on the negative
 * bang id (the q_dotz.c switch-dispatch idiom, #237) maps each id to its C home.
 * The `!` verb wrapper (q_bang_wrap, q_registry.c) routes its negative-id branch
 * through q_bang_dispatch; the non-negative `N!` band (enkey/unkey/dict-make)
 * and the `0N!` show predicate stay in the wrapper.
 *
 * The BANG is the single C home: each wired id calls the stable C impl directly,
 * and the q-visible name DELEGATES to the bang in q (`.j.k:-29!`, `.Q.btoa:-32!`
 * in j.q/dotq.q), so a user rebind of the q name never touches the bang — kdb
 * keeps `-N!` the primitive UNDERNEATH the name.  Root keyword twins (`-1!`hsym,
 * `-5!`parse, ...) route to the same C fn the keyword uses (the keyword stays the
 * registry/builtin home).  Ids without an impl (or whose subsystem openq lacks —
 * IPC/TLS/codec/enums/DARE) are EXPLICIT placeholder cases returning 'nyi; the
 * switch is the first-class inventory. */
#ifndef Q_BANG_H
#define Q_BANG_H

#include <rayforce.h>
#include <stdint.h>

/* Dispatch a q internal function `-N!`.  `id` is the negative internal-fn id
 * (e.g. -8); `y` is the `!` RIGHT operand.  `-N!` is uniformly monadic — `y` is
 * the single argument (`-8!x`); `-27!` takes ONE arg that is a 2-element list
 * `(places;y)` and destructures it in its handler.  Borrowed `y`; returns an
 * OWNED value or error.  Unknown or unimplemented ids return 'nyi. */
ray_t* q_bang_dispatch(int64_t id, ray_t* y);

#endif /* Q_BANG_H */
