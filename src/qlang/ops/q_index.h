/* q_index — THE one index/amend home (ref/apply.md Index, ref/amend.md;
 * owner ruling: one code path, structurally enforced).  Every container read
 * (`a[i]`, `a . p`, `@[a;i]`, `.[a;p]`) and every write (`a[i]:v`,
 * `@[a;i;f;y]`, `.[a;p;f;y]`) flows through TWO recursions over per-container
 * level ops — index_level(x,i) / store_level(x,i,v).  A collection index at a
 * level recurses per element (read: the structure of i maps over the result;
 * write: sequential left-to-right, so repeat-accumulation falls out); `::` at
 * a level reads as identity when last / all-items otherwise, and writes as
 * all indices.  Naive recursion is the sanctioned shape — speed returns later
 * as recognized cases INSIDE this path, never as a second path.  Tables read
 * by pure delegation to q_table_at; table/keyed writes are 'nyi (table wave).
 *
 * Ownership: reads borrow.  Amend entries CONSUME d on success (rc==1 nodes
 * mutate in place at store time — persistent path-copying otherwise) and
 * leave d the caller's on error; ix/f/y are always borrowed.  Value-form
 * callers retain d first (purity by refcount); the name-lift steal passes its
 * sole ref to reach the in-place path. */
#ifndef QLANG_OPS_Q_INDEX_H
#define QLANG_OPS_Q_INDEX_H

#include <rayforce.h>

/* read at depth: x . ix[0..k) (k==0 -> x).  Borrows all; owned result. */
ray_t* q_index_at(ray_t* x, ray_t* const* ix, int64_t k);

/* amend at path ix[0..k) (k==0 -> Amend Entire); f NULL replaces with y,
 * else u[S] / v[S;y].  x consumed on success, the caller's on error. */
ray_t* q_index_amend(ray_t* x, ray_t* const* ix, int64_t k, ray_t* f, ray_t* y);

/* `@[d;i;u]` / `@[d;i;v;vy]` — depth-1 (i is `enlist i` of the path,
 * ref/amend.md); `.[d;i;…]` — i IS the path list ('type otherwise). */
ray_t* q_index_amend_at(ray_t* x, ray_t* i, ray_t* f, ray_t* y);
ray_t* q_index_amend_dot(ray_t* x, ray_t* i, ray_t* f, ray_t* y);

/* the `:` registry row: replacement IS the dyad returning its rhs, so
 * `@[x;i;:;v]` needs no special case (ref/amend.md "If v is Assign (:)"). */
ray_t* q_index_assign_wrap(ray_t* x, ray_t* y);

#endif /* QLANG_OPS_Q_INDEX_H */
