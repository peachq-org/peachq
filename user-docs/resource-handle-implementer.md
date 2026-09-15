# Resource handles — the provider implementer's page

The user-facing model is `user-docs/handles.md`; this page is for whoever writes a `:pq:` PROVIDER — a q namespace
`.X` whose hooks the host (`src/qlang/io/q_provider.c`) calls by NAME. A user names your provider in a coordinate,
`` `:pq:X:alias:… ``, and never calls a hook directly. The contract is v3 (`actionable-plans/2026-09-15-duckdb-sql-parquet-adr.md`
§ Handles and § Main instance); `lib/qpc.q` (q IPC), `lib/duckdb.q` and the test fixture `test/q/provider/vtmock.q`
are the three implementations to read beside it.

## The model in one paragraph

A resolver decides whether a `:...` specification is an existing q resource, an inferred format, a transport/container
composition, or an explicit `:pq:` provider. For a provider, `hopen` and `hclose` are the only lifecycle doors: the host
calls your open hook, keeps the ONE registry (alias → the private token your open hook returned) and answers the alias
symbol `` `:pq:X:alias ``; every other hook is token-keyed. A bound table is a POINTER (the flip of
`` cols!`:pq:X:alias:t/ ``) whose columns are advisory: every query and write goes back through you. Decoder-backed
resources (CSV, JSON, parquet) are not providers — they produce a table for the host's qSQL; transport and container
resolution (HTTP → CSV, ZIP → CSV) belong to PeachQ, not the decoder.

```text
resource specification
    -> resolve transport/container/provider
    -> obtain or expose table
    -> optional provider qsql
    -> host qSQL fallback
```

## The hooks

`token` is whatever `.X.i.open` returned — an int, a symbol, anything; the host stores it and hands it back, never
reads it. Names obey `[a-zA-Z][a-zA-Z0-9_]*`. A hook's error propagates to the caller unmodified, except where marked
best-effort.

| hook | called when | required? |
|---|---|---|
| `.X.i.open[alias;rest;timeout;config]` | `hopen`, the one-shot apply (alias `` ` ``), a temporary connection for an aliasless coordinate. `rest` is the coordinate text after the alias, verbatim; `timeout` 0N when absent; `config` `::` or the dict of the 3-list form. Answers the token. | required |
| `.X.i.close[token]` | `hclose`, the end of a one-shot, a re-pointed alias | optional (no-op) |
| `.X.call[token;text;sync]` | the handle applied to a string (`h "…"`) | required for that door |
| `.X.async[token;msg]` | `h (`async;msg)` | required for that door (`'.X.async` otherwise) |
| `.X.bind[token;name]` | a table coordinate is bound (`get`, `h`name`): answers the column names (a symbol list) | required |
| `.X.get[token;name]` | a bound table is materialised (`select`, `count`/`meta` fallback) | required |
| `.X.set[token;name;data]` / `.X.upsert[token;name;data]` | `` `:pq:X:alias:t/ set x `` / `upsert x`; `insert`/`upsert` on a global bound to your table (through `upsert`) | required for that door |
| `.X.count[token;name]` / `.X.meta[token;name]` | `count`/`meta` of a bound table | optional (host materialises through `get`) |
| `.X.qsql[token;cols;tree]` | a qSQL statement over a bound table: `cols` the live column list, `tree` the functional form with slot 0 the BARE table name; answer the result, or `::` to decline to the host's materialise-and-evaluate fallback | optional |
| `.X.i.link[token;qname;table]` | a carrier of yours is bound to a q global — at the one global-set seam, so `myt:get …`, `myt:h`t`, `.ns.t:…` and a `\d`-scoped assignment all reach it; `qname` is the global's FULL name, `table` the bare table name | optional |
| `.X.i.unlink[token;qname]` | that global is replaced or unbound (`myt:1`, `delete myt from `.`); `token` is `::` once the alias is closed | optional |

Any other name reaches you through the list door, `h (`name; args…)` → `.X.name[token; args…]`, except `i`, `open`
and `close`, which the host keeps to itself.

**The link hooks** are best-effort: the global is already bound when they run, so an error is dropped (the same shape
as `.z.vs`), a hook that itself binds a carrier does not re-enter, and an aliasless (self-contained) carrier never
links — it is served by a connection that dies with the call, so there is nothing durable to point at. `.X.i.link` is
called only while the alias is live; `.X.i.unlink` always. The DuckDB provider uses them to keep a same-named VIEW of
the table in its main instance (`docs/duckdb-api.md` § The instance); q IPC defines neither.

`qsql` is the table-query seam: a provider may execute the query itself, materialise and evaluate locally, or return
`::`. `.pq.i.resolveTree[cols;tree]` (`lib/pq.q`) is the shared resolver every implementation rides — free variables
inlined, columns left symbolic, an unresolvable name `'unpush`. DuckDB uses the seam for provider-side query handling;
q IPC forwards the resolved functional query to the peer.

## What the host guarantees

- `hopen` validates the open tuple before you see it (`timeout` an int atom or absent, `config` a dict or `::`); the
  alias is required for a registered open and reserved names are yours to refuse (DuckDB refuses `main`).
- Only `:pq:X:alias` is ever stored: the config (credentials, paths) dies at `hopen`.
- A live alias re-opened is re-pointed in place: your close hook, then your open hook, the same symbol answers.
- `.pq.conns[]` renders every registered row — the alias symbol as `handle`, the legacy fd as `h` — and a provider's
  own row (registered from C through `q_provider_register_internal`) renders like any other but refuses `hopen`
  and `hclose`.
- A hook is resolved from the live q env at call time: redefining one takes effect on the next call.
