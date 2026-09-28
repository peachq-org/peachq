/ Query and write DuckDB databases from q: a database is a handle, its tables are q tables.
/ .pq.hopen_duckdb[`alias;"/path/db"] opens or creates a database and answers the alias symbol `:pq:duckdb:alias, the
/ handle every .duckdb verb takes.  h "SELECT ..." runs SQL and answers a table; hclose h closes it.
/ `:pq:duckdb:alias:table/ names a table for get, set, upsert and qsql, with select/where/by pushed down to DuckDB.
/ .
/ One DuckDB database lives in the process, `:pq:duckdb:main (.duckdb.main[]; q -duckdb path puts it in a file):
/ every alias is a catalog attached to it, and a q global bound to a DuckDB table is a same-named view in it, so
/ s)SELECT ... runs SQL over q names, live.  DuckDB's httpfs carries s3:// gcs:// hf:// URLs for read0, read1,
/ select from and set, with credentials as DuckDB secrets.  .duckdb.getx[h;`t] reads a table with its schema as the
/ pair (data;schema), which set and upsert accept back.  \?duckdb has more examples.
/ .
/ @eg
/ h:.pq.hopen_duckdb[`demo;":memory:"]
/ `:pq:duckdb:demo:trade/ set ([] sym:`a`b; px:1.5 2.5)
/ `:pq:duckdb:demo:trade/ upsert ([] sym:enlist `c; px:enlist 3.5)
/ select from `:pq:duckdb:demo:trade/ where px>2
/ h "SELECT sym, px*2 AS px2 FROM trade"
/ trade:get `:pq:duckdb:demo:trade/
/ s)SELECT sym, count(*) FROM trade GROUP BY sym
/ `:pq:duckdb:demo:dow/ set select from `:https://www.timestored.com/data/sample/dowjones.csv
/ select max Price by 10 xbar `year$Date from `:pq:duckdb:demo:dow/
/ h "SELECT year(Date) AS yr, max(Price) FROM dow GROUP BY yr ORDER BY yr DESC LIMIT 3"
/ hclose h

.pq.load_natives`duckdb;

/ @ignore
.duckdb.call:{[c;q;sync] if[not sync; '`domain]; .duckdb.lastsql::q; .duckdb.i.exec[c;q]}
/ @ignore
.duckdb.bind:{[c;t] (key .duckdb.i.meta[c;t])`c}
/ A table of the handle's catalog as a q table.
.duckdb.get:{[c;t] .duckdb.i.get[c;t]}
/ Create or replace a table of the handle's catalog from a q table, or from the pair .duckdb.getx answers.
.duckdb.set:{[c;t;d] .duckdb.i.set[c;t;d]; t}
/ @ignore
.duckdb.append:{[c;t;d] .duckdb.i.append[c;t;d]; t}
/ @ignore
.duckdb.upsert:.duckdb.append
/ The meta of a table of the handle's catalog, read from DuckDB without fetching a row.
.duckdb.meta:{[c;t] .duckdb.i.meta[c;t]}

/ Run SQL on a handle and answer the result as a table; a statement without a result answers ::.
/ @eg .duckdb.exec[.duckdb.main[];"SELECT 1 AS one, 'a' AS s"]
.duckdb.exec:{[handle;sql] .duckdb.i.exec[handle;sql]}

/ A table with its DuckDB schema, as the pair (data;schema) that set and upsert accept back.
.duckdb.getx:{[c;t] .duckdb.i.getx[c;t]}
/ SQL's answer with its DuckDB schema, as the pair (data;schema).
.duckdb.execx:{[c;sql] .duckdb.i.execx[c;sql]}

/ The reason behind the last 'duckdb signal, first line the cause, cleared by the next call.
/ err[] is any connection's, err[handle] that connection's.
/ @return string
/ @eg @[.duckdb.exec[.duckdb.main[]];"SELECT * FROM no_such_table";{.duckdb.err[]}]
.duckdb.err:{[handle] .duckdb.i.err handle}

/ The DuckDB-to-q type map as a table, one row per contract entry.
/ @col dtype the DuckDB DDL spelling
/ @col ktype the .duckdb.meta type char
/ @col logical the logical type name
/ @col canon whether a bare read of that DuckDB type produces this row
/ @col needs what a schema row must carry to round-trip: none, logical, dtype or both
/ @eg select from .duckdb.types[] where canon
.duckdb.types:{[] .duckdb.i.types[]}

/ @ignore
.duckdb.i.lit:{[v]
  $[-1h=type v;$[v;"true";"false"];
    (type v) in -5 -6 -7 -8 -9h;string v;
    -11h=type v;.duckdb.i.lit string v;
    10h=type v;"'",ssr[v;"'";"''"],"'";
    11h=type v;"[",(", " sv .duckdb.i.lit each v),"]";
    '`type]}
/ @ignore
.duckdb.i.ident:{[k] s:string k; if[not (0<count s)&all s in .Q.an; '`type]; s}
/ @ignore
.duckdb.i.none:{[v] any v~/:(();()!();(::))}
/ @ignore
.duckdb.i.opts:{[f;opts] $[.duckdb.i.none opts;();(99h=type opts)and 11h=type key opts;f'[key opts;value opts];'`type]}
/ @ignore
.duckdb.i.file:{[f] .duckdb.i.lit $[-11h=type f;$[":"=first s:string f;1_s;s];10h=type f;f;'`type]}

/ @ignore
.duckdb.i.read1:{[url]
  r:.duckdb.exec[.duckdb.main[];"SELECT content FROM read_blob(",(.duckdb.i.file url),")"];
  if[0=count r; '`io]; if[1<count r; '`domain]; first r`content}
/ @ignore
.duckdb.i.staging:`$"_q_staging"
/ @ignore
.duckdb.i.write0:{[url;lines]
  if[not all 10h=type each lines; '`type];
  c:.duckdb.main[]; .duckdb.set[c;.duckdb.i.staging;([] line:lines)];
  .duckdb.exec[c;"COPY (SELECT line FROM ",(.duckdb.i.qname .duckdb.i.staging),") TO ",(.duckdb.i.file url)," (FORMAT CSV, HEADER false, QUOTE '', ESCAPE '')"];
  .duckdb.hdel[c;.duckdb.i.staging]; url}

/ Create or replace a DuckDB secret (the credentials httpfs uses for s3:// gcs:// hf://), or drop one.
/ opts ride verbatim as KEY value clauses of CREATE SECRET, so an unknown key is DuckDB's own refusal; the key
/ `persistent (boolean) picks the persistent form, which outlives the process in DuckDB's own store.
/ @param typ the secret type: `s3, `gcs, `r2, `huggingface ...
/ @param opts a symbol-keyed dict of KEY value clauses; () or (::) drops the secret instead
/ @return name
/ @eg .duckdb.secret[`bucket;`s3;`key_id`secret`region!("AKIA...";"...";"eu-west-1")]
/ @eg .duckdb.secret[`bucket;`s3;()]
.duckdb.secret:{[name;typ;opts]
  c:.duckdb.main[]; n:.duckdb.i.ident name;
  if[.duckdb.i.none opts;
    p:first exec persistent from .duckdb.secrets[] where name~\:n;
    .duckdb.exec[c;"DROP ",$[p;"PERSISTENT ";""],"SECRET ",n]; :name];
  if[not (99h=type opts)and 11h=type key opts; '`type];
  per:$[`persistent in key opts;opts`persistent;0b]; opts:(enlist `persistent) _ opts;
  cl:(enlist "TYPE ",.duckdb.i.ident typ),.duckdb.i.opts[{[k;v] .duckdb.i.ident[k]," ",.duckdb.i.lit v};opts];
  .duckdb.exec[c;"CREATE OR REPLACE ",$[per;"PERSISTENT ";""],"SECRET ",n," (",(", " sv cl),")"]; name}
/ The secrets defined, without their values.
/ @col scope the URL prefixes the secret covers
/ @eg .duckdb.secrets[]
.duckdb.secrets:{[] .duckdb.exec[.duckdb.main[];"SELECT name, type, provider, persistent, storage, scope FROM duckdb_secrets()"]}

/ Run any statement and answer DuckDB's own rendering as text, bypassing the type map: rows as DuckDB prints them,
/ or the error message as data when the statement fails.  A diagnostic, not a query door.
/ @return string
/ @eg .duckdb.unsafeExecText[.duckdb.main[];"SELECT [1,2,3] AS arr, {'a': 1} AS st"]
.duckdb.unsafeExecText:{[handle;sql] .duckdb.i.unsafeExecText[handle;sql]}

/ A table as DuckDB renders it, every column cast to VARCHAR (so nested, UUID, ENUM and BIT cells show), NULL as
/ the text NULL, rows in insertion order.  A text cell holding a newline or " | " misaligns its row.
/ @return a table of one string column per column of tableName
/ @eg
/ .duckdb.exec[.duckdb.main[];"CREATE OR REPLACE TABLE u AS SELECT [1,2] AS arr, 1 AS a"]
/ .duckdb.unsafeText[.duckdb.main[];`u]
.duckdb.unsafeText:{[handle;tableName]
  names:1_"\n" vs .duckdb.i.unsafeExecText[handle;"SELECT column_name FROM (DESCRIBE ",(.duckdb.i.qname tableName),")"];
  quoted:{"\"",ssr[x;"\"";"\"\""],"\""} each names;
  r:"\n" vs .duckdb.i.unsafeExecText[handle;"SELECT ",(", " sv {x,"::VARCHAR AS ",x} each quoted)," FROM ",(.duckdb.i.qname tableName)," ORDER BY rowid"];
  flip (`$" | " vs r 0)!$[count 1_r; flip " | " vs/: 1_r; (count names)#enlist ()]}

/ The statement log: one row per statement the bridge issued, newest last.
/ @eg select time, dur, sql from .duckdb.sqllog
.duckdb.sqllog:([] time:0#0Np; dur:0#0Nn; ok:0#0b; conn:0#0Ni; rows:0#0N; sql:(); err:())

/ How many rows .duckdb.sqllog keeps: 0 keeps nothing; at the cap the older half is dropped.
.duckdb.sqllogmax:4096

/ Called once per statement with its log row as a dict.  Rebind it to route the log elsewhere, or set it to (::)
/ for none.  Do not close the connection or issue transaction control from a handler: it runs mid-statement.
/ @eg .duckdb.onsql:{[row] -1 row`sql;}
.duckdb.onsql:{[row]
  m:$[(type .duckdb.sqllogmax) in -7 -6h; .duckdb.sqllogmax; 0];
  if[0>=m; :.duckdb.sqllog::0#.duckdb.sqllog];
  insert[`.duckdb.sqllog; enlist row];
  if[m<count .duckdb.sqllog; .duckdb.sqllog::(neg m div 2)#.duckdb.sqllog]}

/ The row count of a table of the handle's catalog, counted by DuckDB.
.duckdb.count:{[c;t] first .duckdb.i.exec[c; "SELECT COUNT(*) AS n FROM ",.duckdb.i.qname t]`n}

/ @ignore
.duckdb.i.push:{[c;rt] t:$[10h=type rt 0;.duckdb.exec[c;"SELECT * FROM ",rt 0];.duckdb.get[c;rt 0]]; (?) . (enlist t),1_rt}
/ @ignore
.duckdb.qsql:{[c;cl;tree] rt:@[.pq.i.resolveTree[cl];tree;{[e] ::}]; $[rt~(::);::;.duckdb.i.push[c;rt]]}

/ The handle of the process's own DuckDB database, `:pq:duckdb:main: in-memory, or the file q -duckdb path names.
/ Opened on the first call; every alias is a catalog attached to it, and .parquet and s) run here.
/ @return `:pq:duckdb:main
/ @eg .duckdb.exec[.duckdb.main[];"SELECT current_database()"]
.duckdb.main:{[] .duckdb.i.main[]}

/ Bind the handle's tables and views as q globals at the root, later-wins.  \l `:pq:duckdb:alias is the same door.
/ @param names () for none, :: for all, else the table names
/ @return the names bound
/ @eg .duckdb.load[.duckdb.main[];::]
.duckdb.load:{[handle;names] .pq.i.load[handle;names]}
/ @ignore
.duckdb.i.load:{[c;tbls] $[tbls~(::); .duckdb.i.tables c; (),tbls]}

/ Drop a table (or view) of the handle's catalog.  hdel `:pq:duckdb:alias:table/ is the same verb.
/ A q global bound to it stays bound and errors on use.
/ @return table
/ @eg
/ .duckdb.exec[.duckdb.main[];"CREATE OR REPLACE TABLE u AS SELECT 1 AS a"]
/ .duckdb.hdel[.duckdb.main[];`u]
.duckdb.hdel:{[handle;table] .duckdb.i.hdel[handle;table]}

/ @ignore
.s.e:{[x] r:.duckdb.main[] x; if[(.s.i.head x) in ("CREATE";"ALTER"); .s.i.sync[]]; r}
/ @ignore
.s.i.sync:{[] h:.duckdb.main[]; .pq.i.load[h;.duckdb.i.tables[h] except key `.]}
/ @ignore
.s.i.head:{[s]
  s:((s in " \t\r\n")?0b)_s;
  if["--"~2#s; :.s.i.head (1+s?"\n")_s];
  if["/*"~2#s; k:first where (s="*")&next s="/"; :$[null k;"";.s.i.head (2+k)_s]];
  upper ((s in .Q.a,.Q.A)?0b)#s}
