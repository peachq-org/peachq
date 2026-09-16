/ Query and write DuckDB databases from q: a database is a handle, its tables are q tables.
/ h:hopen `:pq:duckdb:alias:/path/db opens (or creates) one and answers the alias symbol `:pq:duckdb:alias - the
/ handle every public verb takes (.duckdb.exec[h;sql]); h "SELECT ..." runs SQL and answers a table; hclose h
/ closes it.  `:pq:duckdb:alias:table/ names a table for get, set, upsert and qsql, with select/where/by pushed
/ down to DuckDB.  There is ONE DuckDB database per process, `:pq:duckdb:main (.duckdb.main[]; q -duckdb path makes
/ it a file): every alias is a catalog attached to it, and a q global bound to a DuckDB table is a same-named view
/ in it, so s)SELECT ... runs SQL over q names, live.  .duckdb.types[] is the type map, .duckdb.err[] the last
/ error.  DuckDB's httpfs is also the transport for s3:// gcs:// hf:// (read0, read1, select from and set on a URL
/ just work), with credentials as DuckDB secrets: .duckdb.secret[name;type;opts] and .duckdb.secrets[].  \?duckdb has
/ examples.
/ @implNote DuckDB as a virtual-table PROVIDER (contract v3: the 2026-09-15 ADR § Handles; the instance and the
/ link: § Main instance).  These hooks ARE the DuckDB surface, written over the natives .duckdb.i.*, which bind at
/ this same \l pq gate; the lifecycle hooks .duckdb.i.open[alias;rest;tmo;cfg] / .duckdb.i.close[token] and the link
/ hooks .duckdb.i.link[token;qname;table] / .duckdb.i.unlink[token;qname] ARE natives, called by the host alone
/ (hopen/hclose are the only doors; the link fires at the global-set seam).  c below is the HANDLE (alias sym, or the
/ legacy int), resolved to the native token in C - no q-side state.  ANY-ORDER LAW: definitions only at top level.

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

/ ---- SQL spelling: the one home for what a q value looks like inside a statement (.parquet builds on these) ----

/ a SQL literal: bool, int, float, sym, string and sym list have a spelling; anything else is 'type
.duckdb.i.lit:{[v]
  $[-1h=type v;$[v;"true";"false"];
    (type v) in -5 -6 -7 -8 -9h;string v;
    -11h=type v;.duckdb.i.lit string v;
    10h=type v;"'",ssr[v;"'";"''"],"'";
    11h=type v;"[",(", " sv .duckdb.i.lit each v),"]";
    '`type]}
/ an identifier is the one unquoted text in the SQL, so anything else is 'type, never spliced
.duckdb.i.ident:{[k] s:string k; if[not (0<count s)&all s in .Q.an; '`type]; s}
/ () or ()!() or (::) as an opts or query argument = none
.duckdb.i.none:{[v] any v~/:(();()!();(::))}
/ opts as (key;value) pairs through f, or none; a dict with symbol keys is the only shape
.duckdb.i.opts:{[f;opts] $[.duckdb.i.none opts;();(99h=type opts)and 11h=type key opts;f'[key opts;value opts];'`type]}
/ a resource as a literal: a `:path symbol (leading colon dropped, a URL verbatim) or a string
.duckdb.i.file:{[f] .duckdb.i.lit $[-11h=type f;$[":"=first s:string f;1_s;s];10h=type f;f;'`type]}

/ ---- the transport (ADR 2026-09-15 § B3): httpfs carries the bytes of a remote scheme, our decoders own their
/ meaning.  HOST-ONLY hooks, named for the q verb they serve (the A1.1 naming law): the core's read0/read1/select-from
/ doors call i.read1 by name for s3:// gcs:// hf://, `:url.EXT set t calls i.write0 - never a user's spelling ----

/ the whole object as bytes (read_blob; a ranged read1 slices in memory) - exactly what read1 answers for a file.
/ DuckDB globs the URL: no row is 'io (read1's class for a missing file; a remote miss is DuckDB's 'duckdb), several
/ rows are 'domain - the byte doors read ONE resource, globbing is the parquet door's
/ @return (bytes)
.duckdb.i.read1:{[url]
  r:.duckdb.exec[.duckdb.main[];"SELECT content FROM read_blob(",(.duckdb.i.file url),")"];
  if[0=count r; '`io]; if[1<count r; '`domain]; first r`content}
/ write lines (a format's .h.tx text) to url: staged as one VARCHAR column on _q_staging and COPYd unquoted and
/ unescaped, so the bytes are the lines joined by \n plus a trailing \n - what 0: writes.  Lines are UTF-8 (the
/ codec's law); the staging is dropped after
/ @return url
.duckdb.i.write0:{[url;lines]
  if[not all 10h=type each lines; '`type];
  c:.duckdb.main[]; .duckdb.set[c;`_q_staging;([] line:lines)];
  .duckdb.exec[c;"COPY (SELECT line FROM ",(.duckdb.i.qname `_q_staging),") TO ",(.duckdb.i.file url)," (FORMAT CSV, HEADER false, QUOTE '', ESCAPE '')"];
  .duckdb.hdel[c;`_q_staging]; url}

/ ---- credentials are DuckDB secrets (duckdb.org/docs/configuration/secrets_manager), on main ----

/ CREATE OR REPLACE [PERSISTENT] SECRET name (TYPE typ, KEY value, ...): opts ride verbatim as KEY value clauses (a
/ string or sym quoted, a bool or number bare - the COPY-option shape), so an unknown key is DuckDB's own refusal; the
/ key `persistent (a bool) picks the persistent form and is no clause.  opts (::) or () DROPs the secret, persistent
/ or not, and typ is not consulted.  A persistent secret lands in DuckDB's own store, outside q
/ @return name
.duckdb.secret:{[name;typ;opts]
  c:.duckdb.main[]; n:.duckdb.i.ident name;
  if[.duckdb.i.none opts;
    p:first exec persistent from .duckdb.secrets[] where name~\:n;
    .duckdb.exec[c;"DROP ",$[p;"PERSISTENT ";""],"SECRET ",n]; :name];
  if[not (99h=type opts)and 11h=type key opts; '`type];
  per:$[`persistent in key opts;opts`persistent;0b]; opts:(enlist `persistent) _ opts;
  cl:(enlist "TYPE ",.duckdb.i.ident typ),.duckdb.i.opts[{[k;v] .duckdb.i.ident[k]," ",.duckdb.i.lit v};opts];
  .duckdb.exec[c;"CREATE OR REPLACE ",$[per;"PERSISTENT ";""],"SECRET ",n," (",(", " sv cl),")"]; name}
/ what duckdb_secrets() shows, minus the secret values: scope is the list of URL prefixes the secret covers
/ @return (table) name type provider persistent storage scope
.duckdb.secrets:{[] .duckdb.exec[.duckdb.main[];"SELECT name, type, provider, persistent, storage, scope FROM duckdb_secrets()"]}

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

/ THE main instance's own handle, `:pq:duckdb:main: the one DuckDB database of the process (in-memory, or the file
/ q -duckdb path / PEACHQ_DUCKDB_MAIN names), opened and registered on the first call - .pq.conns[] lists it, every
/ alias is a catalog ATTACHed to it, and a q global bound to a DuckDB table is a same-named VIEW in it.  The
/ standard library's own doors (.parquet, s)) run here; hopen/hclose refuse the alias.
/ @return (symbol) `:pq:duckdb:main
.duckdb.main:{[] .duckdb.i.main[]}

/ load the handle's tables and views as q pointers, at the root, later-wins (\l `:pq:duckdb:al is the same door):
/ tbls () none, :: all, else the names
/ @return (symbol list) the names bound
.duckdb.load:{[c;tbls] .pq.i.load[c;tbls]}
/ the host's load hook answers the NAMES; the host binds them
.duckdb.i.load:{[c;tbls] $[tbls~(::); .duckdb.i.tables c; (),tbls]}

/ drop table t of the handle's catalog with its sidecar rows (native: the key law is the codec's) - a view when that
/ is what t names; hdel `:pq:duckdb:al:t/ is this verb's spelling.  A q name bound to it stays bound and errors on use
/ @return (symbol) t
.duckdb.hdel:{[c;t] .duckdb.i.hdel[c;t]}

/ s)SELECT ... - the custom-language handler kdb documents (.X.e receives the line after the prefix), on main: a
/ q global bound to a DuckDB table is a bare name here, an alias's table is alias.table, a result prints as a table
/ and a statement without one answers ::.  Errors are the bridge's 'duckdb, .duckdb.err[] the reason.  After a
/ CREATE or ALTER the NEW names of main's own catalog become q pointers (an existing global keeps its value; \l is
/ the later-wins door); a DROP unbinds nothing - the stale pointer errors on use.
.s.e:{[x] r:.duckdb.main[] x; if[(.s.i.head x) in ("CREATE";"ALTER"); .s.i.sync[]]; r}
.s.i.sync:{[] h:.duckdb.main[]; .pq.i.load[h;.duckdb.i.tables[h] except key `.]}
/ the statement's first word, past whitespace and -- or block comments, upper-cased: the classifier of the sync law
/ (DuckDB reports CREATE TABLE AS as a row write, so the result's kind cannot serve)
.s.i.head:{[s]
  s:((s in " \t\r\n")?0b)_s;
  if["--"~2#s; :.s.i.head (1+s?"\n")_s];
  if["/*"~2#s; k:first where (s="*")&next s="/"; :$[null k;"";.s.i.head (2+k)_s]];
  upper ((s in .Q.a,.Q.A)?0b)#s}
