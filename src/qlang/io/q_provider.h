/* q_provider — the virtual-table HOST (actionable-plans/
 * 2026-08-07-plugin-data-sources-tables.md).  Grammar: connection form
 * `:pq:ds:alias:config` (hopen only) vs table form `:pq:ds:alias[:config]:t/`
 * (every table position — the trailing slash IS the table marker).  ONE
 * registry maps the returned int handle <-> alias <-> (ds; open triad;
 * CONNID); providers are plain q namespaces (`.vtmock.open` / ...) reached by
 * NAME-GENERIC hook dispatch, and secrets die at hopen (only ":pq:ds:alias"
 * is ever stored for introspection).  Carriers are the splay shape — n sym
 * keys against ONE `:pq:ds:alias:t/ hsym atom — with ADVISORY columns: every
 * query and write goes back through the provider. */
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

/* hopen `:pq:ds:alias[:config]` (CONNECTION form only — a table form is
 * 'domain) — an int handle in the one q_handles space (provider-kind), a live
 * alias re-pointed in place.  EVERY open form (bare sym, 2-list, 3-list,
 * one-shot sym-apply) normalizes to the FROZEN triad
 * .ds.open[config; timeout; opts] — timeout 0N when absent, opts :: when
 * absent (both borrowed here, may be NULL); any future open-time need rides
 * the opts dict, never a fourth argument. */
ray_t* q_provider_hopen(const char* s, size_t n, ray_t* timeout, ray_t* config);

/* `h y` on a provider-kind handle (qh < 0 = async call).  Text -> .X.call,
 * sym atom -> .X.bind + carrier, list/sym-vector -> the named hook. */
ray_t* q_provider_apply(int64_t qh, ray_t* y);

/* `` `:pq:... `` sym apply: a LIVE alias resolves to its connection, else
 * transient one-shot (open -> dispatch -> close, alias never registered). */
ray_t* q_provider_sym_apply(ray_t* head, ray_t** args, int64_t n);

/* hclose arm: .X.close[connid] (no-op if undefined), then the record and its
 * reserved fd go away.  Owned :: like q_handles_close. */
ray_t* q_provider_close(int64_t qh);

int    q_provider_carrier_is(ray_t* x);
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
 * when the from-slot is a provider table and .X.qsql is defined, call it with the
 * functional args as a tree whose slot 0 is the BARE underlying name
 * (name-normalization law); NULL = not a provider or no qsql hook. */
ray_t* q_provider_from_table(ray_t* t);
ray_t* q_provider_qsql_push(ray_t** args, int64_t n);

/* hsym-target write door: `` `:pq:ds:alias:t/ set y `` -> .X.set (upsert=1
 * -> .X.upsert).  NULL = not a provider target (caller keeps its path). */
ray_t* q_provider_write(ray_t* x, ray_t* y, int upsert);

#endif
