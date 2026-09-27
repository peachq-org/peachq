/* q_provider — the `:pq:<kind>:<alias>` aliases (q_handles_pq names the kinds).  hopen answers the ALIAS SYMBOL and
 * hopen/hclose are the only lifecycle doors over ONE registry alias <-> (kind; token; hopen arg; reserved fd).  The
 * three q kinds are one thing, an int IPC socket from connect, fork or spawn: every message is the peer's own and
 * close is hclose.  DuckDB's token is its own, reached through the fixed `.duckdb` hooks; a q peer's table hooks are
 * `.pq.i.q.*`.  The arg stays private to the host for the ONE re-dial a use of a dead peer makes.  A bound table is
 * the flip of `cols!`:pq:duckdb:db:t/`, the aux-marked dict (base/q_type.h); its columns are advisory. */
#ifndef QLANG_Q_PROVIDER_H
#define QLANG_Q_PROVIDER_H
#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>
#include "qlang/io/q_handles.h"

void q_provider_init(void);
void q_provider_destroy(void);

/* hopen (`:pq:<kind>:alias; arg) — arg borrowed, NULL = `::`; answers the alias sym, re-pointing a live one in place */
ray_t* q_provider_hopen(const char* s, size_t n, ray_t* arg);

/* A kind's OWN connection (`:pq:duckdb:main`): listed and resolved like any alias, but hopen/hclose of it are 'domain.
 * Answers the handle sym id, 0 on failure; token is retained. */
int64_t q_provider_register_internal(q_pq_kind kind, const char* alias, ray_t* token);

/* The link seam (q_env_set / q_env_unbind): .X.i.link[token; qname; table] / .X.i.unlink[token; qname], best-effort */
ray_err_t q_provider_link(int64_t qname, ray_t* car);
ray_err_t q_provider_unlink(int64_t qname, ray_t* car);

/* `\l` on a coordinate / .pq.i.load[h;tables]: .X.i.load names the tables, the host binds them at the root */
ray_t* q_provider_load(const char* s, size_t n, ray_t* tables);
void   q_provider_pq_register(void);

ray_t* q_provider_hdel(ray_t* x);                     /* NULL = not a `:pq:` sym */
ray_t* q_provider_apply(int64_t qh, ray_t* y);        /* the legacy int handle; qh < 0 = async */
ray_t* q_provider_token(ray_t* handle, q_pq_kind kind); /* BORROWED; NULL = not a live alias of that kind */
ray_t* q_provider_sym_apply(ray_t* head, ray_t** args, int64_t n);
ray_t* q_provider_close(int64_t qh);
ray_t* q_provider_close_sym(ray_t* x);

/* kind/alias/handle sym ids of an alias fd, whether it is up, and a q peer's socket fd (-1 otherwise); 0 = no such */
int    q_provider_info(int64_t fd, int64_t* provider, int64_t* alias, int64_t* handle, int* open, int64_t* link);

void   q_provider_forget(void);                       /* a forked child drops every alias, calling nothing */

int    q_provider_carrier_is(ray_t* x);               /* a bound table = the MARKED dict; never a shape test */
ray_t* q_provider_flip(ray_t* cols, int64_t sym);     /* `cols!`:pq:duckdb:db:t/` -> the pointer, NULL = not that */
ray_t* q_provider_unflip(ray_t* car);                 /* the pointer -> that pair, plain */
int    q_provider_coord_sym_form(ray_t* x);           /* 0 none, 1 connection, 2 table coordinate */

ray_t* q_provider_get_carrier(ray_t* x);              /* `get` of a coordinate; NULL = not a `:pq:` sym */
ray_t* q_provider_carrier_table(ray_t* car);
ray_t* q_provider_carrier_count(ray_t* car);
ray_t* q_provider_carrier_meta(ray_t* car);

/* funsql seams: the materialized from-slot (NULL = not a provider), and .X.qsql[token; cols; tree] pushdown — NULL
 * when there is no hook or it answers `::` (declined), so the caller materializes */
ray_t* q_provider_from_table(ray_t* t);
ray_t* q_provider_qsql_push(ray_t** args, int64_t n);

ray_t* q_provider_write(ray_t* x, ray_t* y, int upsert);   /* `:pq:duckdb:db:t/ set|upsert y`; NULL = not ours */

#endif
