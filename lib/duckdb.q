/ Query and write DuckDB databases from q: a database is a handle, its tables are q tables.
/ h:hopen `:pq:duckdb:alias:/path/db opens (or creates) one; h "SELECT ..." runs SQL and answers a table;
/ `:pq:duckdb:alias:table/ names a table for get, set, upsert and qsql, with select/where/by pushed down to
/ DuckDB.  .duckdb.types[] is the type map, .duckdb.err[] the last error.  \?duckdb has examples.
/ @implNote DuckDB as a virtual-table PROVIDER (`:pq:duckdb:alias:/path/db`,
/ actionable-plans/2026-08-07-plugin-data-sources-tables.md).  These hooks ARE
/ the DuckDB surface: the bespoke .duckdb.connect/.sql/.select API was replaced
/ 2026-08-07, not wrapped.  Written over the internal natives .duckdb.i.* (the
/ connection, the sidecar-lossless round-trip and one raw exec), which bind at
/ this same \l pq gate.  CONNID is the native int handle - no q-side state.
/ ANY-ORDER LAW: definitions only at top level.

/ open[rest;timeout;config]: rest = the db path (`` `:default: `` = shared
/ in-memory); timeout is meaningless to an in-process engine and ignored;
/ config rides the native's (path;configDict) form verbatim.
.duckdb.open:{[rest;tmo;cfg]
  p:`$$[0=count rest;":default:";":",rest];
  $[99h=type cfg; .duckdb.i.open (p;cfg); .duckdb.i.open p]}

/ closing the main connection forgets it, so the next .duckdb.main[] opens afresh
.duckdb.close:{[c] if[c~.duckdb.i.mainc; .duckdb.i.mainc::0Ni]; .duckdb.i.close c}
/ the sync flag is ignored: an in-process engine has no async lane (the same
/ treatment as open's timeout)
/ lastsql records the SQL last handed to the engine - the B2 pushdown-snapshot
/ slot (test/observed/taq-sql); inert until a .duckdb.qsql translator emits SQL.
.duckdb.call:{[c;q;sync] .duckdb.lastsql::q; .duckdb.i.exec[c;q]}
/ bind reads the column names off meta - ONE catalog lookup owns the query
/ and the column ORDER; a second duckdb_columns() spelling here would fork it.
.duckdb.bind:{[c;t] (key .duckdb.i.meta[c;t])`c}
.duckdb.get:{[c;t] .duckdb.i.get[c;t]}
.duckdb.set:{[c;t;d] .duckdb.i.set[c;t;d]; t}
/ a missing table is created by this call, so the first batch declares the schema
.duckdb.append:{[c;t;d] .duckdb.i.append[c;t;d]; t}
/ upsert is the name the provider host calls its write hook (`:pq:duckdb:a:t/ upsert x)
.duckdb.upsert:.duckdb.append
.duckdb.meta:{[c;t] .duckdb.i.meta[c;t]}
/ run SQL and answer the bare table - the door beside get, and the sibling of execx, which answers the pair
/ @return (table)
.duckdb.exec:{[c;sql] .duckdb.i.exec[c;sql]}

/ the envelope doors beside get and exec: the pair (data;schema), schema one row per column whose DuckDB type the q
/ carrier alone cannot spell (TINYINT is also h); set and append take the pair back.  h (`getx;`t) is the handle form
.duckdb.getx:{[c;t] .duckdb.i.getx[c;t]}
.duckdb.execx:{[c;sql] .duckdb.i.execx[c;sql]}

/ the message channel behind the bare 'duckdb: the reason(s) behind the last call's refusal, first line the cause
/ (DuckDB's own text, or the bridge's), cleared by the next call.  err[] is any connection's, err[c] that connection's.
/ @return (string)
.duckdb.err:{.duckdb.i.err x}

/ the DuckDB<->q type contract (the C QD_TYPES[] table) as data: one row per contract entry, columns dtype (the
/ DDL spelling), ktype (the .duckdb.meta char), logical (the hub name), canon (whether a bare read of that DuckDB
/ type produces this row), needs (what an envelope row must carry: none, logical, dtype, both).  Derive from it.
/ @return (table) `dtype`ktype`logical`canon`needs
.duckdb.types:{[] .duckdb.i.types[]}

/ ---- the diagnostics ----

/ run ANY statement and answer DuckDB's own text, bypassing the type contract entirely: rows rendered cell by cell by
/ DuckDB itself, or, when the statement fails, its message AS DATA rather than a signal.  `unsafe` is the marker - a
/ hit for `.duckdb.unsafe` outside tests and interactive use is a review finding.
/ @return (string)
.duckdb.unsafeExecText:{[c;sql] .duckdb.i.unsafeExecText[c;sql]}

/ DuckDB's own account of a table, bypassing the type contract: every column cast to VARCHAR (so nested, UUID, ENUM
/ and BIT cells render), NULL kept as the text NULL, rows in insertion order.  Lines are rows and " | " splits a
/ line, so a text cell holding a newline or that sequence misaligns its row - accepted, not guarded.
/ @return (table) one string column per column of tableName
.duckdb.unsafeText:{[c;tableName]
  names:1_"\n" vs .duckdb.i.unsafeExecText[c;"SELECT column_name FROM (DESCRIBE ",(.duckdb.i.qname tableName),")"];
  quoted:{"\"",ssr[x;"\"";"\"\""],"\""} each names;
  r:"\n" vs .duckdb.i.unsafeExecText[c;"SELECT ",(", " sv {x,"::VARCHAR AS ",x} each quoted)," FROM ",(.duckdb.i.qname tableName)," ORDER BY rowid"];
  flip (`$" | " vs r 0)!$[count 1_r; flip " | " vs/: 1_r; (count names)#enlist ()]}

/ the statement log, an ordinary table: one row per statement the bridge issued - the stage-then-cast CREATE/INSERT,
/ the exactness check and the _q_schema writes included, plus an appender line for the non-SQL batch.
.duckdb.sqllog:([] time:0#0Np; dur:0#0Nn; ok:0#0b; conn:0#0Ni; rows:0#0N; sql:(); err:())

/ the ONE knob, cap AND switch: 0 keeps nothing at all, and at the cap the older half goes.
.duckdb.sqllogmax:4096

/ called once per statement with its row as a dict; rebind to route the log elsewhere, or unbind with (::) for none.
/ do NOT close the connection or issue transaction control from a handler: it runs mid-statement, before the bridge
/ has consumed the result it is about to read.
.duckdb.onsql:{[r]
  m:$[(type .duckdb.sqllogmax) in -7 -6h; .duckdb.sqllogmax; 0];   / anything but an int/long atom is OFF
  if[0>=m; :.duckdb.sqllog::0#.duckdb.sqllog];
  insert[`.duckdb.sqllog; enlist r];
  if[m<count .duckdb.sqllog; .duckdb.sqllog::(neg m div 2)#.duckdb.sqllog]}

/ count/meta answer from cheap SQL, never a materialize: COUNT(*) is the
/ provider's own aggregate, and the host's fallback would have read every row.
.duckdb.count:{[c;t] first .duckdb.i.exec[c; "SELECT COUNT(*) AS n FROM ",.duckdb.i.qname t]`n}

/ .duckdb.qsql - THE B2 SEAM, deliberately naive: it exercises the .X.qsql
/ contract (shared resolver, decline-on-unpush) but materializes and evaluates
/ locally - behaviorally the host fallback.  The B2 SQL translator replaces
/ ONLY .duckdb.i.push's body (emit SQL, run it, fill .duckdb.lastsql - inert
/ until then).  Only the RESOLVER is trapped (unpushable -> :: -> host
/ fallback); evaluation errors propagate - the query never runs twice.
/ rt 0 is the RELATION: a sym names a table of c, a string is FROM-clause SQL (.parquet.read's read_parquet(...)).
.duckdb.i.push:{[c;rt] t:$[10h=type rt 0;.duckdb.exec[c;"SELECT * FROM ",rt 0];.duckdb.get[c;rt 0]]; (?) . (enlist t),1_rt}
.duckdb.qsql:{[c;cl;tree] rt:@[.pq.i.resolveTree[cl];tree;{[e] ::}]; $[rt~(::);::;.duckdb.i.push[c;rt]]}

/ the one connection the standard library's own doors (.parquet) run on: opened on first call, the same CONNID
/ until closed.  Today a connection to the shared :default: db; a reserved instance is the ADR's A1 follow-up.
.duckdb.i.mainc:0Ni
.duckdb.main:{[] if[null .duckdb.i.mainc; .duckdb.i.mainc::.duckdb.open["default:";0N;::]]; .duckdb.i.mainc}
