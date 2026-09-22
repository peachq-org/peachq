/* q_vecop — THE typed elementwise path for the compare / boolean / null /
 * fill verbs: the decision their kernels make for ONE atom pair, written once
 * per numeric lane over ray_data() instead of through a heap atom per element.
 *
 * A probe, not a second dispatch home.  The caller hands over the kernel it
 * was about to map; NULL comes back for every shape or type this file does not
 * own, and the caller's own element loop stays the sole authority on those.
 * What it does own it must answer byte-identically — see q_vecop.c for which
 * lane each pair lands on and why that is the kernels' own choice. */
#ifndef QLANG_OPS_Q_VECOP_H
#define QLANG_OPS_Q_VECOP_H

#include <rayforce.h>

/* owned result, or NULL = not this file's lane (the caller's loop owns it) */
ray_t* q_vecop_binary(ray_t* (*f)(ray_t*, ray_t*), ray_t* x, ray_t* y);
ray_t* q_vecop_unary(ray_t* (*f)(ray_t*), ray_t* x);

#endif /* QLANG_OPS_Q_VECOP_H */
