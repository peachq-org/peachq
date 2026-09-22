/* q_kapi — the kdb C-API (`k.h`) seam and `2:` Dynamic Load.
 *
 * An extension built against kdb's published `k.h` compiles `kI(x)`, `x->n` and friends as macros over
 * kdb's `struct k0` INSIDE ITS OWN object code, so whatever we hand it must BE those bytes.  It is:
 * `struct k0{m,a,t,u,r,{n,G0[]}}` is `ray_t{mmod,attrs,type,order,rc,{len,data[]}}` shifted 16 bytes,
 * and rayfall's type tags ARE kdb's numbers, so `K = (K)((char*)v + 16)` is the whole marshal for flat
 * typed vectors and most atoms — no copy, in either direction.  `test/test_types.c types/header_layout`
 * pins the offsets that make it true.
 *
 * Copies survive at the seams where the two models genuinely differ: symbols (kdb wants a process-stable
 * `char*`, ray has narrow domain ids), real ATOMS (ray keeps an f32 atom widened in the f64 slot),
 * errors (kdb -128h with `x->s`, ray 127 with a packed class), the list/dict/table SPINES (kdb's are
 * `K*`/`x->k`, ray's are `ray_t*` 2-slot blocks) — leaves inside a spine stay overlays — physical
 * RAY_STR, and slices (`kG(x)` must be inline, so a slice materialises).
 *
 * ZERO-COPY IS DANGEROUS BY DESIGN, and we say so rather than defend against it.  The overlay hands the
 * extension the LIVE ray header: writing `x->t` retypes a live q value, writing `x->a`/`x->u` scuffs the
 * attribute/block-order bytes.  Reads are safe.  Two reads diverge and both are benign: `x->r` is ray's
 * rc, which is 1 where kx would say 0 for a sole owner (so an extension's in-place-mutation check takes
 * the conservative copy branch), and `x->a` is ray's attrs, whose bit values are not kdb's `s`/`u`/`p`/`g`
 * (a kdb attribute never changes what `kG(x)` points at, so the read is advisory either way). */
#ifndef Q_KAPI_H
#define Q_KAPI_H

#include <rayforce.h>

/* `2:` — Dynamic Load (ref/dynamic-load.md).  x is the library (a `` `:path `` file symbol or a char
 * vector), y is `` (`symbol;rank) ``.  Answers a Q_EVAL_CAR_KFN carrier: a 112h function value that
 * projects, iterates and `value`s like any other. */
ray_t* q_dl_wrap(ray_t* x, ray_t* y);

/* Apply a KFN carrier: marshal n args in, call the C function, marshal the result back.  THE apply
 * module's arm for 112h; args are BORROWED, the result is owned (CLAUDE.md rule 5 — which is also
 * capiref.md:103's law that a dynamically-linked module never takes ownership of its parameters). */
ray_t* q_kapi_invoke(ray_t* carrier, ray_t** args, int64_t n);

/* Per-runtime teardown: deregister every `sd1` fd and drop the symbol mirror (its ids die with the
 * runtime's intern table).  dlopen handles are NOT closed — RTLD_NODELETE, as q_ffi.c does. */
void q_kapi_reset(void);

#endif /* Q_KAPI_H */
