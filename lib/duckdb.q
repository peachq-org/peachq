/ duckdb.q — DuckDB as a virtual-table PROVIDER (`:pq:duckdb:alias:/path/db`,
/ actionable-plans/2026-08-07-plugin-data-sources-tables.md).  These hooks ARE
/ the DuckDB surface: the bespoke .duckdb.connect/.sql/.select API was replaced
/ 2026-08-07, not wrapped.  Written over the internal natives .duckdb.i.* (the
/ connection, the sidecar-lossless round-trip and one raw exec), which bind at
/ this same \l pq gate.  CONNID is the native int handle — no q-side state.
/ ANY-ORDER LAW: definitions only at top level.
/ NO .duckdb.qsql: pushdown is a later stage; the host's materialize fallback
/ is the correct phase-1 behaviour (always right by the residual invariant).

/ open[rest;timeout;config]: rest = the db path (`` `:default: `` = shared
/ in-memory); timeout is meaningless to an in-process engine and ignored;
/ config rides the native's (path;configDict) form verbatim.
.duckdb.open:{[rest;tmo;cfg]
  p:`$$[0=count rest;":default:";":",rest];
  $[99h=type cfg; .duckdb.i.open (p;cfg); .duckdb.i.open p]}

.duckdb.close:{[c] .duckdb.i.close c}
/ the sync flag is ignored: an in-process engine has no async lane (the same
/ treatment as open's timeout)
/ lastsql records the SQL last handed to the engine — the B2 pushdown-snapshot
/ slot (test/observed/taq-sql); inert until a .duckdb.qsql translator emits SQL.
.duckdb.call:{[c;q;sync] .duckdb.lastsql::q; .duckdb.i.exec[c;q]}
/ bind reads the column names off meta — ONE catalog lookup owns the query
/ and the column ORDER; a second duckdb_columns() spelling here would fork it.
.duckdb.bind:{[c;t] (key .duckdb.i.meta[c;t])`c}
.duckdb.get:{[c;t] .duckdb.i.get[c;t]}
.duckdb.set:{[c;t;d] .duckdb.i.set[c;t;d]; t}
.duckdb.upsert:{[c;t;d] .duckdb.i.append[c;t;d]; t}
.duckdb.meta:{[c;t] .duckdb.i.meta[c;t]}

/ count/meta answer from cheap SQL, never a materialize: COUNT(*) is the
/ provider's own aggregate, and the host's fallback would have read every row.
.duckdb.count:{[c;t] first .duckdb.i.exec[c; "SELECT COUNT(*) AS n FROM ",.duckdb.i.q t]`n}

/ identifiers double-quoted, a doubled inner quote escaping — the same law the
/ C side applies when it builds SQL.
.duckdb.i.q:{[t] "\"",ssr[string t;"\"";"\"\""],"\""}
