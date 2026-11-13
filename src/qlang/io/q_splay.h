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
ray_t*  q_splay_contained_table(ray_t* t);
static inline ray_t* q_splay_contained(ray_t* v) { return v && v->type == RAY_TABLE ? q_splay_contained_table(v) : v; }

/* Cumulative inflated-block count — the compressed lane's laziness witness; and the deferred tests asked. */
int64_t q_splay_zblocks(void);
int64_t q_splay_asks(void);

/* The fault handler cannot raise: a torn block reads back zero-filled, this answers 1 ONCE for it (the apply
 * answers 'corrupt) and re-arms the block so the next touch answers again. */
int q_splay_fault_pending(void);

/* Live mapped bytes (region totals) — `.Q.w[]`mmap`'s source. */
int64_t q_splay_mapped_bytes(void);

#endif
