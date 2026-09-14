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
 * by pure delegation to q_table_at.  A table's index TYPE picks its axis at
 * every level — a sym names a COLUMN (the column dict's level ops, since a
 * table is `flip` of that dict), an int a ROW — so `` t[0;`b] `` and
 * `` t[`b;0] `` reach the same cell.  A DICT's selector is read by Find, whole
 * (`d[x] ~ v[k?x]`, basics/dictsandtables.md): Find's rank law says whether
 * the selector is one key or a run of them, so the indexer never splits it;
 * a hit reads/rewrites the value and an absent key EXTENDS both halves (the
 * dictionary upsert law, ref/assign.md).  A keyed table is that same dict
 * with a TABLE domain — the row-seeking Find — and an absent ROW of a table
 * does not extend ('index, the list law).
 *
 * Ownership: reads borrow.  Amend entries CONSUME d on success (rc==1 nodes
 * mutate in place at store time — persistent path-copying otherwise) and
 * leave d the caller's on error; ix/f/y are always borrowed.  Value-form
 * callers retain d first (purity by refcount); the name-lift steal passes its
 * sole ref to reach the in-place path. */
#ifndef QLANG_OPS_Q_INDEX_H
#define QLANG_OPS_Q_INDEX_H

#include <rayforce.h>

/* v[i] as an OWNED atom/element (borrows v): the scalar-int fast path over
 * vectors/lists (direct payload read, no index-atom allocation, no collapse);
 * a TABLE's item is its ROW DICT (ref/count.md, q_table_row_at); generic ray_at
 * indexing for every other shape.  The ONE element-read home shared across the
 * q layer (the apply module, wrappers, codecs). */
ray_t* q_index_elem_at(ray_t* v, int64_t i);

/* The rank axis, homed here because both read items through q_index_elem_at.
 * is_nested: are v's ITEMS collections (a STR atom counts — kdb strings are
 * char LISTS)?  any_nested_item: the same axis read for CONFORMABILITY — does
 * ANY item make v rank-2 under flip, which is_nested (item 0 only) misses. */
int q_index_is_nested(ray_t* v);
int q_index_any_nested_item(ray_t* v);
int q_index_rank(ray_t* v);     /* ref/join.md:192: the recursive depth of the first element (`,:` enlists at +1) */

/* read at depth: x . ix[0..k) (k==0 -> x).  Borrows all; owned result. */
ray_t* q_index_at(ray_t* x, ray_t* const* ix, int64_t k);

/* amend at path ix[0..k) (k==0 -> Amend Entire); f NULL replaces with y,
 * else u[S] / v[S;y].  x consumed on success, the caller's on error. */
ray_t* q_index_amend(ray_t* x, ray_t* const* ix, int64_t k, ray_t* f, ray_t* y);

/* Amend Entire with `,` IN PLACE — a vector the caller owns outright (it
 * parked the name; rc is the caller's question, not asked here) takes vector
 * y of its own type where it stands.  growable: may it (type, letter, sym
 * domain and width).  grow: do it — NULL on success with *px the vector,
 * which may have MOVED; else an owned error with *px valid but partly grown.
 * ungrow: back to length nx and the attrs bits `was` carried. */
int    q_index_growable(ray_t* x, ray_t* y);
ray_t* q_index_grow(ray_t** px, ray_t* y);
void   q_index_ungrow(ray_t* x, int64_t nx, uint8_t was);

/* `@[d;i;u]` / `@[d;i;v;vy]` — depth-1 (i is `enlist i` of the path,
 * ref/amend.md); `.[d;i;…]` — i IS the path list ('type otherwise). */
ray_t* q_index_amend_at(ray_t* x, ray_t* i, ray_t* f, ray_t* y);
ray_t* q_index_amend_dot(ray_t* x, ray_t* i, ray_t* f, ray_t* y);

/* the `:` registry row: replacement IS the dyad returning its rhs, so
 * `@[x;i;:;v]` needs no special case (ref/amend.md "If v is Assign (:)"). */
ray_t* q_index_assign_wrap(ray_t* x, ray_t* y);

#endif /* QLANG_OPS_Q_INDEX_H */
