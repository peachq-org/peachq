/* q_splay — the mapped splayed-table authority (kb/splayed-tables.md:254: loading maps, it does not read).
 * The lifetime law (a deferred table, a map per use, the lanes) and each entry's contract are stated in q_splay.c.
 * The per-runtime registry here backs only what the files must be probed for: validated headers and the domain,
 * each entry re-checked against its files' stamps at every dereference. */
#ifndef QLANG_Q_SPLAY_H
#define QLANG_Q_SPLAY_H
#include <rayforce.h>
#include <stdint.h>

void q_splay_init(void);
void q_splay_destroy(void);

ray_t* q_splay_get(ray_t* x);

ray_t* q_splay_flip(ray_t* cols, int64_t dirsym);

/* The sym a mapped or deferred table was flipped from (0 = a plain table), the deferred test, and its uses. */
int64_t q_splay_table_path(ray_t* t);
int     q_splay_table_deferred(ray_t* t);
ray_t*  q_splay_deferred_map(ray_t* t, ray_t* want);
ray_t*  q_splay_deferred_col(ray_t* t, int64_t name);
ray_t*  q_splay_deferred_rows(ray_t* t, ray_t* want, int64_t lo, int64_t n);
ray_t*  q_splay_contained_table(ray_t* t);
static inline ray_t* q_splay_contained(ray_t* v) { return v && v->type == RAY_TABLE ? q_splay_contained_table(v) : v; }

/* A partitioned table is the reference flipped from a plain name (kb/partition.md; ref/flip-splayed.md:21).  What the
 * loaded database holds is q's (.Q.pf, .Q.pv, .Q.pd), set by _mount when `\l` meets a database root (NULL, or an
 * owned error).  _values is the owned one-column table of the partition values under the partition field's name
 * (NULL: none); _map the owned in-memory table of t's columns named in `want` over the partitions indexed by the
 * long vector `parts` (NULL = all), the partition column first — NULL when t is no such reference or the database
 * holds no table of its name; _latest that of the most recent partition alone. */
ray_t*  q_splay_named_mount(void);
int     q_splay_table_named(ray_t* t);
ray_t*  q_splay_named_values(void);
ray_t*  q_splay_named_map(ray_t* t, ray_t* want, ray_t* parts);
ray_t*  q_splay_named_latest(ray_t* t);
/* `.Q.c.part[dir;names]`: the named columns of the splayed table in directory `dir`, mapped — one partition's. */
ray_t*  q_splay_part_fn(ray_t** args, int64_t n);

/* Cumulative inflated-block count — the compressed lane's laziness witness; and the deferred tests asked. */
int64_t q_splay_zblocks(void);
int64_t q_splay_asks(void);

/* The fault handler cannot raise: a torn block reads back zero-filled, this answers 1 ONCE for it (the apply
 * answers 'corrupt) and re-arms the block; a trusted p# index its column's cells denied answers the same way. */
int q_splay_fault_pending(void);

/* Live mapped bytes (region totals) — `.Q.w[]`mmap`'s source. */
int64_t q_splay_mapped_bytes(void);

#endif
