/* q_splay — the mapped splayed-table authority (kb/splayed-tables.md:254: loading maps, it does not read).
 * `get `:dir/` answers a REAL TABLE (ref/flip-splayed.md: the flip of `cols!`:dir/`): one vector per .d column —
 * fixed-width uncompressed columns mmap, compressed ones reserve their plain size and inflate a block on first touch
 * (the fault handler), nested/string/enum columns decode — and the table block's aux bytes carry the directory sym,
 * which is what `flip`, the k-repr, `value`, the wire and the write refusals read.  Every lane dies by refcount with
 * its last holder through ONE unmap choke point; a global bound by `\l` keeps its maps for the session (kx's law).
 * The per-runtime registry here backs only what the files must be probed for: validated headers and the domain. */
#ifndef QLANG_Q_SPLAY_H
#define QLANG_Q_SPLAY_H
#include <rayforce.h>
#include <stdint.h>

void q_splay_init(void);
void q_splay_destroy(void);

/* Drop the entry behind a `:dir/ handle sym — the splay WRITER's hook, so an in-process overwrite never serves
 * stale headers. */
void q_splay_invalidate(int64_t sym);
void q_splay_invalidate_under(const char* path, size_t n);   /* flat write inside a mapped dir */

/* `get `:dir/` — NULL unless x is a `:path/ sym atom naming a directory with a .d (the caller falls through to the
 * flat reader); else the owned mapped table or a RAY_ERROR from load-time HEADER validation ('type/'corrupt/'nyi/'io). */
ray_t* q_splay_get(ray_t* x);

/* `flip cols!`:dir/` — the mapped table, or, when the directory does not resolve, the UNRESOLVED table (the dict
 * retagged: an empty general list per column, aux set) that displays `+(,`a)!`:./s/` and fails only when queried
 * (ref/flip-splayed.md).  NULL unless dirsym spells a splay directory (`:…/`, not a provider coordinate). */
ray_t* q_splay_flip(ray_t* cols, int64_t dirsym);

/* The directory sym a mapped or unresolved table was flipped from (0 = a plain table), and the unresolved test. */
int64_t q_splay_table_path(ray_t* t);
int     q_splay_table_unresolved(ray_t* t);

/* Cumulative inflated-block count — the compressed lane's laziness witness. */
int64_t q_splay_zblocks(void);

/* The fault handler cannot raise: a torn block reads back zero-filled, this answers 1 ONCE for it (the apply
 * answers 'corrupt) and re-arms the block so the next touch answers again. */
int q_splay_fault_pending(void);

/* Live mapped bytes (region totals) — `.Q.w[]`mmap`'s source. */
int64_t q_splay_mapped_bytes(void);

#endif
