/* q_bang — the single C home for the q `!` verb.  Three entry points, widest
 * first: q_bang is the registry row (any operands); q_bang_dispatch owns every
 * INT left operand — 0N (show), negative (internal fns, a `switch` per the
 * q_dotz.c idiom, #237), N>=0 (enkey/unkey, else dict-make); q_bang_enkey is the
 * keying primitive.  Callers that already hold an int should call the most
 * specific of the three; callers holding only a ray_t* call q_bang.
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

/* The `!` verb.  Type-checks x: an int atom routes to q_bang_dispatch, anything
 * else dict-makes.  Borrowed operands; returns an OWNED value or error. */
ray_t* q_bang(ray_t* x, ray_t* y);

/* Dispatch an INT `!` left operand.  `id` is the left operand's value with every
 * typed null canonicalized to NULL_I64 (`0N!x` — print + pass through); `y` is
 * the RIGHT operand.  A negative id is an internal function, uniformly monadic —
 * `y` is the single argument (`-8!x`); `-27!` takes ONE arg that is a 2-element
 * list `(places;y)` and destructures it in its handler.  Unknown or unimplemented
 * negative ids return 'nyi.  id >= 0 keys a table (or a name bound to one), else
 * dict-makes.  Borrowed `y`; returns an OWNED value or error. */
ray_t* q_bang_dispatch(int64_t id, ray_t* y);

/* q enkey/unkey `N!table`: nkey 0 -> plain table (unkey), N>0 -> key the first N
 * columns into a keyed table.  Accepts a plain OR already-keyed table (re-keys);
 * 'length if nkey >= column count.  Borrowed `y`; returns an OWNED value. */
ray_t* q_bang_enkey(int64_t nkey, ray_t* y);

#endif /* Q_BANG_H */
