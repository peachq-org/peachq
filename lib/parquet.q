/ Read and write parquet: .parquet.read[file;opts;query] answers a table, .parquet.write[file;table;opts] writes one
/ (COPY options verbatim: compression, row_group_size, partition_by, ...), and .parquet.schema, .metadata,
/ .file_metadata, .kv_metadata and .bloom_probe answer DuckDB's own tables verbatim.  `select from `:x.parquet` and
/ `:x.parquet set t are the shortest spellings; save `t.parquet rides .h.tx.  \?.parquet lists the verbs;
/ user-docs/parquet.md has the detail.
/ @implNote THE SHIM LAW: every verb is SQL through .duckdb.exec on .duckdb.main[] - a DuckDB-based table on its own
/ connection - every value crosses the .duckdb codec untouched, and there is no parquet C.  A file written from a
/ table with a sidecar carries q_schema in its key-value metadata: those sidecar rows in .duckdb.getx's vocabulary
/ (col dtype logical iskey), as JSON; a read finds the key and restores through the codec's declared-schema leg, a
/ file without it (foreign, or from a SQL-created table) comes back as DuckDB reads it (text is text, never symbols).
/ Staging is the reserved TEMP table _q_staging of main's connection (connection-scoped, so stage, fill, read and
/ drop share it): a q table is .duckdb.set there and .duckdb.hdel'd after the COPY.  A failed call leaves its staging
/ for the next call's CREATE OR REPLACE to reclaim, because every bridge door clears .duckdb.err[] and the reason
/ must survive the signal.  DuckDB's httpfs is the transport for a URL, the one exception to handles.md rule 15.  A
/ missing library or file is the bare 'duckdb, .duckdb.err[] the reason.
/ ANY-ORDER LAW: definitions only at top level.

/ a SQL literal: bool, int, float, sym, string and sym list have a spelling; anything else is 'type
.parquet.i.lit:{[v]
  $[-1h=type v;$[v;"true";"false"];
    (type v) in -5 -6 -7 -8 -9h;string v;
    -11h=type v;.parquet.i.lit string v;
    10h=type v;"'",ssr[v;"'";"''"],"'";
    11h=type v;"[",(", " sv .parquet.i.lit each v),"]";
    '`type]}

/ the file: a `:path symbol (leading colon dropped, a glob or URL passes verbatim) or a list of them
.parquet.i.file:{[f]
  $[-11h=type f;.parquet.i.lit $[":"=first s:string f;1_s;s];
    11h=type f;"[",(", " sv .parquet.i.file each f),"]";
    '`type]}

/ () or ()!() or (::) as opts or query = none
.parquet.i.none:{[x] any x~/:(();()!();(::))}

/ an identifier is the one unquoted text in the SQL, so anything else is 'type, never spliced
.parquet.i.ident:{[k] s:string k; if[not (0<count s)&all s in .Q.an; '`type]; s}
/ opts as (key;value) pairs through f, or none; a dict with symbol keys is the only shape
.parquet.i.opts:{[f;opts] $[.parquet.i.none opts;();(99h=type opts)and 11h=type key opts;f'[key opts;value opts];'`type]}
/ read_parquet's key=value
.parquet.i.opt:{[k;v] .parquet.i.ident[k],"=",.parquet.i.lit v}
/ COPY's KEY value: a sym list is an identifier list (PARTITION_BY (a, b)), a dict is a struct (FIELD_IDS {a: 1})
.parquet.i.copyval:{[v]
  $[11h=type v;"(",(", " sv .parquet.i.ident each v),")";
    99h=type v;"{",(", " sv {[k;x] .parquet.i.ident[k],": ",.parquet.i.copyval x}'[key v;value v]),"}";
    .parquet.i.lit v]}

/ read_parquet(file, key=value, ...): every opts key rides verbatim, so an unknown one is DuckDB's binder error
.parquet.i.rel:{[file;opts] "read_parquet(",(", " sv (enlist .parquet.i.file file),.parquet.i.opts[.parquet.i.opt;opts]),")"}

/ the q_schema text a file carries, "" when none (a foreign file, or one written from a table without a sidecar)
.parquet.i.kv:{[c;file]
  r:.duckdb.exec[c;"SELECT decode(value) AS v FROM parquet_kv_metadata(",(.parquet.i.file file),") WHERE decode(key) = 'q_schema' LIMIT 1"];
  $[count r;first r`v;""]}

/ the sidecar rows of table t on c, in the envelope's vocabulary: the physical type comes from the catalog (temp's
/ for the staging table) and the column name is matched under the codec's ASCII fold; () when the catalog has no sidecar
.parquet.i.schema:{[c;t]
  k:.parquet.i.lit lower string t;
  if[0=first (.duckdb.exec[c;"SELECT count(*) AS n FROM duckdb_tables() WHERE database_name = current_database() AND schema_name = 'main' AND table_name = '_q_schema'"])`n; :()];
  .duckdb.exec[c;"SELECT c.column_name AS col, c.data_type AS dtype, s.logical, s.iskey FROM main._q_schema s JOIN duckdb_columns() c ON c.database_name = ",$[t=`_q_staging;"'temp'";"current_database()"]," AND c.schema_name = 'main' AND c.table_name = ",(.parquet.i.lit string t)," AND translate(c.column_name, 'ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz') = s.col WHERE s.tbl = ",k," AND s.col <> '' ORDER BY c.column_index"]}

/ a provider-bound table is the marked dict whose flip holds the coordinate; a DuckDB one splits into (connection;
/ table) - the last segment is the table, the rest the handle; anything else answers ()
.parquet.i.coord:{[t]
  if[-11h<>type v:value flip t; :()];
  if[not (s:string v) like ":pq:duckdb:*"; :()];
  i:last where s=":";
  (`$i#s; `$-1_(1+i)_s)}

/ COPY rel TO file (FORMAT PARQUET, KV_METADATA {q_schema: '...'}, opts): no key when the schema is empty
.parquet.i.copy:{[c;rel;file;sc;opts]
  kv:$[count sc;enlist "KV_METADATA {q_schema: ",(.parquet.i.lit .j.j sc),"}";()];
  o:.parquet.i.opts[{[k;v] .parquet.i.ident[k]," ",.parquet.i.copyval v};opts];
  .duckdb.exec[c;"COPY ",rel," TO ",(.parquet.i.file file)," (",(", " sv (enlist "FORMAT PARQUET"),kv,o),")"]}

/ write table to file (a directory `:out/ for partition_by; an s3:// URL passes through) and answer file.  A DuckDB-
/ based table copies in place, no q round-trip; any other table (a qpc or splayed one materialised) stages through
/ .duckdb.set, so its sidecar exists, and is dropped after the COPY
.parquet.write:{[file;table;opts]
  if[-11h<>type file; '`type];
  if[98h<>type table; '`type];
  if[count ct:.parquet.i.coord table;
    .parquet.i.copy[ct 0;"(SELECT * FROM ",(.duckdb.i.qname ct 1),")";file;.parquet.i.schema . ct;opts]; :file];
  c:.duckdb.main[]; .duckdb.set[c;`_q_staging;$[-11h=type value flip table;select from table;table]];
  .parquet.i.copy[c;.duckdb.i.qname `_q_staging;file;.parquet.i.schema[c;`_q_staging];opts];
  .duckdb.hdel[c;`_q_staging]; file}

/ restore a file's q types: the staging table is .duckdb.set from a 0-row all-() table with the envelope (every
/ column's physical type from DESCRIBE, the logical from q_schema), so the codec lays the DDL and the sidecar rows
/ itself; the rows then move inside DuckDB and .duckdb.get's declared-schema leg reads them back
.parquet.i.restore:{[rel;kv;query]
  c:.duckdb.main[];
  ds:update col:`$col from .duckdb.exec[c;"SELECT column_name AS col, column_type AS dtype FROM (DESCRIBE SELECT * FROM ",rel,")"];
  env:ds lj `col xkey select col:`$col, logical:`$logical, iskey from .j.k kv;
  .duckdb.set[c;`_q_staging;(flip (env`col)!(count env)#enlist ();env)];
  .duckdb.exec[c;"INSERT INTO ",(.duckdb.i.qname `_q_staging)," SELECT * FROM ",rel];
  r:$[.parquet.i.none query;.duckdb.get[c;`_q_staging];.duckdb.i.push[c;.pq.i.resolveTree[env`col;(enlist `_q_staging),eval each 2_query]]];
  .duckdb.hdel[c;`_q_staging]; r}

/ query is a parsed select tree whose table position is ignored.  It rides the one qSQL seam every provider does
/ (.pq.i.resolveTree over the relation's columns, then .duckdb.i.push) - never an evaluator of its own.  eval of
/ each slot lands the functional-form values the host hands that seam (a parsed where slot is ,,(>;`a;`lim))
.parquet.read:{[file;opts;query]
  rel:.parquet.i.rel[file;opts]; c:.duckdb.main[];
  if[not .parquet.i.none query; if[not (?)~query 0; '`type]];
  if[count kv:.parquet.i.kv[c;file]; :.parquet.i.restore[rel;kv;query]];
  if[.parquet.i.none query; :.duckdb.exec[c;"SELECT * FROM ",rel]];
  cl:cols .duckdb.exec[c;"SELECT * FROM ",rel," LIMIT 0"];
  .duckdb.i.push[c;.pq.i.resolveTree[cl;(enlist rel),eval each 2_query]]}

/ the bytes of table t as a parquet file - .h.tx's `parquet entry, so save `t.parquet and the download door share it
.parquet.i.bytes:{[t]
  f:hsym `$$[count d:getenv `TMPDIR;d;count d:getenv `TEMP;d;"/tmp"],"/pq",(string .z.i),"_",(string "j"$.z.p),".parquet";
  .parquet.write[f;t;()]; b:read1 f; hdel f; b}

.parquet.i.fn:{[fn;file] .duckdb.exec[.duckdb.main[];"SELECT * FROM ",fn,"(",.parquet.i.file[file],")"]}
/ parquet_schema(file): one row per column of the file's schema, DuckDB's columns verbatim
.parquet.schema:{[file] .parquet.i.fn["parquet_schema";file]}
/ parquet_metadata(file): one row per column chunk per row group
.parquet.metadata:{[file] .parquet.i.fn["parquet_metadata";file]}
/ parquet_file_metadata(file): created_by, num_rows, num_row_groups, format_version, encryption_algorithm
.parquet.file_metadata:{[file] .parquet.i.fn["parquet_file_metadata";file]}
/ parquet_kv_metadata(file): the file's key-value metadata, one row per key
.parquet.kv_metadata:{[file] .parquet.i.fn["parquet_kv_metadata";file]}
/ parquet_bloom_probe(file, 'column', value): per row group, whether its bloom filter excludes the value
.parquet.bloom_probe:{[file;column;val]
  args:", " sv (.parquet.i.file file;.parquet.i.lit column;.parquet.i.lit val);
  .duckdb.exec[.duckdb.main[];"SELECT * FROM parquet_bloom_probe(",args,")"]}
