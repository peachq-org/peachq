/* q_provider — the virtual-table HOST (actionable-plans/
 * 2026-08-07-plugin-data-sources-tables.md; contract v3 = the 2026-09-15 ADR
 * § Handles).  Grammar: connection form `:pq:ds:alias:config` (hopen only)
 * vs table form `:pq:ds:alias[:config]:t/` (every table position — the
 * trailing slash IS the table marker).  hopen answers the ALIAS SYMBOL
 * `:pq:ds:alias`, and hopen/hclose are the only lifecycle doors: the host
 * calls .ds.i.open[alias;rest;timeout;config] / .ds.i.close[token] and keeps
 * the ONE registry alias <-> (ds; TOKEN; the legacy reserved fd);
 * every other hook is token-keyed, reached by NAME-GENERIC dispatch off a
 * plain q namespace, and secrets die at hopen (only ":pq:ds:alias" is ever
 * stored).  A bound table is the splay POINTER — the flip of
 * `cols!`:pq:ds:alias:t/`, carried as that dict with the aux mark
 * (base/q_type.h) — and its columns are ADVISORY: every query and write goes
 * back through the provider. */
#ifndef QLANG_Q_PROVIDER_H
#define QLANG_Q_PROVIDER_H
#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>

void q_provider_init(void);
void q_provider_destroy(void);

/* Does the descriptor text spell the provider marker `:pq:...` (case-
 * insensitive on the marker)? */
int q_provider_spec_is(const char* s, size_t n);

/* Is the descriptor inside the pq FILE namespace — path `pq` or `pq:...`
 * after the optional hsym colon, case-insensitive?  hopen never opens these
 * as files: they parse as a coordinate or error 'domain (the `./pq...`
 * relative spelling stays the escape for real files). */
int q_provider_ns_is(const char* s, size_t n);

/* hopen `:pq:ds:alias:config` (CONNECTION form only — a table form is
 * 'domain, and the alias is REQUIRED: a sym handle needs a name) — the alias
 * sym `:pq:ds:alias`, a live alias re-pointed in place (the same sym).  EVERY
 * open form (bare sym, 2-list, 3-list, one-shot sym-apply) normalizes to the
 * FROZEN tuple .ds.i.open[alias; rest; timeout; config] — alias ` for the
 * one-shot, timeout 0N when absent, opts :: when absent (both borrowed here,
 * may be NULL); any future open-time need rides the opts dict. */
ray_t* q_provider_hopen(const char* s, size_t n, ray_t* timeout, ray_t* config);

/* A provider's OWN connection as a registered row (DuckDB's `:pq:ds:main`):
 * listed by .pq.conns[], resolved like any alias, but hopen of that alias and
 * hclose of the handle are 'domain.  Answers the handle sym id, 0 on failure;
 * token is retained. */
int64_t q_provider_register_internal(const char* ds, const char* alias, ray_t* token);

/* The link seam (q_env_set / q_env_unbind): a carrier bound to a global is
 * .X.i.link[token; qname; table], one displaced or unbound is
 * .X.i.unlink[token; qname] — both OPTIONAL hooks, best-effort (the global is
 * already bound; a hook's error is dropped).  qname = the global's full name. */
void q_provider_link(int64_t qname, ray_t* car);
void q_provider_unlink(int64_t qname, ray_t* car);

/* `h y` on the LEGACY int handle (qh < 0 = async call).  Text -> .X.call,
 * sym atom -> .X.bind + carrier, list/sym-vector -> the named hook. */
ray_t* q_provider_apply(int64_t qh, ray_t* y);

/* The token behind a handle of THIS provider — the alias sym or the legacy
 * int — BORROWED; NULL when it is neither (dead, another provider's, junk).
 * A token means something only to its own provider (qpc's is an IPC fd), so
 * a native never decodes another's. */
ray_t* q_provider_token(ray_t* handle, const char* provider);

/* `` `:pq:... `` sym apply: a LIVE alias resolves to its connection, else
 * transient one-shot (open -> dispatch -> close, alias never registered). */
ray_t* q_provider_sym_apply(ray_t* head, ray_t** args, int64_t n);

/* hclose arms: .X.i.close[token] (no-op if undefined), then the record and
 * its reserved fd go away.  Owned :: like q_handles_close; a dead alias sym
 * is the same tolerated no-op as a dead int, a sym that is no connection
 * REFERENCE (config, a table, no alias) 'domain, one that is not `:pq:` 'type. */
ray_t* q_provider_close(int64_t qh);
ray_t* q_provider_close_sym(ray_t* x);

/* provider/alias/handle sym ids of a registered provider fd; 0 = no such fd */
int    q_provider_info(int64_t fd, int64_t* provider, int64_t* alias, int64_t* handle);

int    q_provider_carrier_is(ray_t* x);      /* a bound table = the MARKED dict; never a shape test */

/* The flip law both ways (q_splay_flip's twin), each a FRESH block, NULL = not that shape: flip flip x ~ x. */
ray_t* q_provider_flip(ray_t* cols, int64_t sym);     /* `cols!`:pq:…:t/` -> the pointer */
ray_t* q_provider_unflip(ray_t* car);                 /* the pointer     -> that pair, plain */

int    q_provider_coord_sym_is(ray_t* x);   /* -RAY_SYM spelling :pq:... */
int    q_provider_coord_sym_form(ray_t* x); /* 0 none, 1 connection, 2 table (/) */

/* `get` of a table-form coordinate — the carrier (splay symmetry), cols via
 * the bind hook on the live or temporary connection.  NULL = not `:pq:. */
ray_t* q_provider_get_carrier(ray_t* x);

/* Provider truth for a bound carrier: .X.get materialization, .X.count /
 * .X.meta with the host fallback (materialize) when undefined. */
ray_t* q_provider_carrier_table(ray_t* car);
ray_t* q_provider_carrier_count(ray_t* car);
ray_t* q_provider_carrier_meta(ray_t* car);

/* funsql seams.  from_table: carrier value or table-form hsym -> owned
 * materialized table (phase-1 residual law), NULL = not a provider.  qsql_push:
 * when the from-slot is a provider table and .X.qsql is defined, call
 * .X.qsql[connid; cols; tree] — cols the provider-truth column list (one
 * LIVE bind round-trip; embedded carrier keys are advisory and can be
 * stale), tree the functional args with slot 0 the BARE underlying name
 * (name-normalization law).  A `::` result means the hook DECLINED; that,
 * a missing hook, or unavailable cols -> NULL (the materialize fallback). */
ray_t* q_provider_from_table(ray_t* t);
ray_t* q_provider_qsql_push(ray_t** args, int64_t n);

/* hsym-target write door: `` `:pq:ds:alias:t/ set y `` -> .X.set (upsert=1
 * -> .X.upsert).  NULL = not a provider target (caller keeps its path). */
ray_t* q_provider_write(ray_t* x, ray_t* y, int upsert);

#endif
