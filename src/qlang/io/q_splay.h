/* q_splay — the lazy splayed-table authority (kb/splayed-tables.md:254: loading
 * maps, it does not read).  `get `:dir/` answers a CARRIER — the q-data dict
 * `col1`col2!`:dir/ (n sym keys against ONE hsym atom, a shape `!` refuses).
 * MARKER FIRST: what the value carries (cols = its keys, type = its shape) is
 * answered from the value; this per-runtime registry backs only what it cannot
 * — validated headers, the decode-lane cache, the loaded domain, and the live
 * mmap regions (rc-driven: a mapped column munmaps with its last holder,
 * through ONE choke point).  A cold carrier re-opens from its own path. */
#ifndef QLANG_Q_SPLAY_H
#define QLANG_Q_SPLAY_H
#include "qlang/net/q_wirefile.h"   /* q_wf_colhdr — the probed column header */
#include <rayforce.h>
#include <stdint.h>

void q_splay_init(void);
void q_splay_destroy(void);

/* Drop the entry behind a `:dir/ handle sym — the splay WRITER's hook, so an
 * in-process overwrite never serves stale headers or cached columns. */
void q_splay_invalidate(int64_t sym);

/* `get `:dir/` — NULL unless x is a `:path/ sym atom naming a directory with a
 * .d (the caller falls through to the flat reader); else an owned carrier or a
 * RAY_ERROR from load-time HEADER validation ('type/'corrupt/'nyi/'io). */
ray_t* q_splay_get(ray_t* x);

/* Carrier recognition — the VALUE's shape alone, no registry consult. */
int q_splay_is(ray_t* x);

/* Owned row count — the FIRST .d column's header count, never a data read
 * (a first column whose header hides its count decodes once) — or error. */
ray_t* q_splay_count(ray_t* car);

/* THE @[column;idx] gather seam (idx NULL = the whole column): an owned
 * vector — freshly mapped, a zip region with ONLY idx's blocks inflated, or
 * decoded+cached — an owned error on a corrupt body, NULL when `sym`/`name`
 * names no column (caller falls through).  q_splay_col is the whole-column
 * spelling. */
ray_t* q_splay_gather(ray_t* car, int64_t name, ray_t* idx);
ray_t* q_splay_col(ray_t* car, int64_t sym);

/* First-k-rows table through the gather (k < 0 = all rows; q_splay_table is
 * that spelling — display's row budget and set-of-carrier ride these). */
ray_t* q_splay_prefix(ray_t* car, int64_t k);
ray_t* q_splay_table(ray_t* car);

/* Cumulative inflated-block count — the compressed lane's laziness witness. */
int64_t q_splay_zblocks(void);

/* The header facts meta reads: column count (-1 when car is no carrier), a
 * column's name sym, and its probed header (BORROWED; NULL out of range) —
 * the meta owner translates them to display letters. */
int64_t q_splay_ncols(ray_t* car);
int64_t q_splay_col_sym(ray_t* car, int64_t i);
const q_wf_colhdr* q_splay_col_hdr(ray_t* car, int64_t i);

/* Live mapped bytes (region totals) — `.Q.w[]`mmap`'s source. */
int64_t q_splay_mapped_bytes(void);

/* The lazy column reference funsql binds into phrase scope: (carrier; name;
 * idx — generic null = ALL).  The apply module forces it through the gather
 * seam the moment any application consumes it. */
ray_t* q_splay_colref(ray_t* car, int64_t name, ray_t* idx);
int    q_splay_colref_is(ray_t* v);
void   q_splay_colref_parts(ray_t* v, ray_t** car, int64_t* name, ray_t** idx);

#endif
