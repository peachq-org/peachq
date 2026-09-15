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

/* `x,y` for two plain dicts — THE dict write (ref/join.md:94 upsert): `keys?key y` once, then the hits store into
 * the values and the misses grow both slots; a repeated key appends once and its last occurrence wins.  strict is
 * Append's law (`,:`, ref/join.md:173: a value the slot cannot hold is 'type, nothing written); else Join's
 * (ref/join.md:33: a mismatched slot boxes to a general list — the keys box either way).  A keyed table on either
 * side is 'type here (its rows join elsewhere).  x consumed on success (written where it stands when it is the
 * caller's only ref, its slots included; a shared level copies once), the caller's on error; y borrowed. */
ray_t* q_index_dict_join(ray_t* x, ray_t* y, int strict);

/* THE keyed-table write — the dict write with table slots (ref/upsert.md §Keyed table; ref/join.md:140,274;
 * ref/insert.md:56).  y is a keyed table of x's schema (the door normalised it: q_table_rows_normalize then
 * `nkey!rows`).  `(key x)?key y` once — run_positions' law: a repeated key appends once, its last occurrence wins —
 * then the misses grow every column of both slots and the hits store per VALUE column `hit` names (a boolean per
 * value column; NULL: every column — replace-row, upsert: an omitted column was null-filled at the door,
 * course/keyed-tables:82; the payload's own columns is merge-columns: `,:` with a keyed payload retains the rest,
 * ref/join.md:274).
 * Q_KEYED_INSERT refuses any hit ('insert) and appends every row in payload order.  The type gate is insert's
 * (q_table_rows_typed) over both parts before any write: a foreign type is 'type, int<->long casts to the column.
 * `exclusive` is the caller's word that x is its only ref: the write lands where x stands — its slots, their
 * columns — and the result is x retained; else x is untouched and a new dict answers, every level it shares copied
 * once.  Past the type check not transactional (an OOM mid-store leaves earlier stores standing), but consistent:
 * both slots always one length.  x/y/hit borrowed. */
enum { Q_KEYED_UPSERT, Q_KEYED_INSERT };
ray_t* q_index_keyed_put(ray_t* x, ray_t* y, ray_t* hit, int mode, int exclusive);

/* Amend Entire with `,` IN PLACE — a vector the caller owns outright (it
 * parked the name; rc is the caller's question, not asked here) takes vector
 * y of its own type where it stands.  growable: may it (type, letter, sym
 * domain and width).  grow: do it — NULL on success with *px the vector,
 * which may have MOVED; else an owned error with *px valid but partly grown.
 * ungrow: back to length nx and the attrs bits `was` carried; a general list only releases the items above nx. */
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
