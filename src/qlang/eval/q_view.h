/* q views (learn/views.md, ref/view.md) — the `name::expr` dependency
 * mechanism and its introspection surface.  q_view.c carries the design. */
#ifndef OPENQ_Q_VIEW_H
#define OPENQ_Q_VIEW_H

#include <rayforce.h>

int    q_view_is(ray_t* v);                       /* view carrier? */
/* 1 = ast (BORROWED) was a top-level view definition and was handled;
 * *out owned (:: / error).  src = the statement's source text. */
int    q_view_intercept(ray_t* ast, const char* src, ray_t** out);
ray_t* q_view_deref(ray_t* v);                    /* CONSUMES v; recalc-or-cache */
ray_t* q_view_deref_borrowed(ray_t* v);           /* borrows v; owned result */
ray_t* q_view_value4(ray_t* v);                   /* (last|::; tree; deps; text) */
ray_t* q_view_text(ray_t* v);                     /* owned text charv, NULL if not a view */
ray_t* q_view_wrap(ray_t* x);                     /* `view` verb body */
/* \b (pending=0: sorted + `s#` when nonempty) / \B (pending=1: sorted, bare).
 * ns_sym 0 or `. = the default namespace (the only home of views); another
 * EXISTING namespace lists empty; unknown -> NULL (caller signals the name). */
ray_t* q_view_names(int64_t ns_sym, int pending);
ray_t* q_view_zb(void);                           /* .z.b dependency dict */
void   q_view_on_global_set(int64_t sym);         /* invalidate + fire .z.vs */
void   q_view_on_global_unbind(int64_t sym);      /* invalidate only */
void   q_view_set_index(ray_t** idxv, int64_t k); /* .z.vs y for an indexed assign */
void   q_view_reset(void);                        /* runtime teardown */

#endif /* OPENQ_Q_VIEW_H */
