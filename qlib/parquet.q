/ Read and write parquet files through DuckDB: .parquet.read answers a table, .parquet.write writes one, and the
/ schema, metadata, file_metadata, kv_metadata and bloom_probe verbs answer DuckDB's own inspection tables.
/ `select from `:x.parquet` and `:x.parquet set t are the shortest spellings; save `t.parquet also works.
/ A file written from a q table carries its q types in the key-value metadata, so it reads back as written; a
/ foreign file comes back as DuckDB reads it (text is text, never symbols).  A URL (s3:// ...) passes straight through.
/ .
/ @eg
/ .parquet.write[`:trade.parquet;([] sym:`a`b; px:1.5 2.5);()]
/ .parquet.read[`:trade.parquet;();()]
/ select from `:trade.parquet where px>2

/ @ignore
.parquet.i.file:{[f] $[-11h=type f;.duckdb.i.file f;11h=type f;"[",(", " sv .parquet.i.file each f),"]";'`type]}
/ @ignore
.parquet.i.opt:{[k;v] .duckdb.i.ident[k],"=",.duckdb.i.lit v}
/ @ignore
.parquet.i.copyval:{[v]
  $[11h=type v;"(",(", " sv .duckdb.i.ident each v),")";
    99h=type v;"{",(", " sv {[k;x] .duckdb.i.ident[k],": ",.parquet.i.copyval x}'[key v;value v]),"}";
    .duckdb.i.lit v]}
/ @ignore
.parquet.i.rel:{[file;opts] "read_parquet(",(", " sv (enlist .parquet.i.file file),.duckdb.i.opts[.parquet.i.opt;opts]),")"}
/ @ignore
.parquet.i.kv:{[c;file]
  r:.duckdb.exec[c;"SELECT decode(value) AS v FROM parquet_kv_metadata(",(.parquet.i.file file),") WHERE decode(key) = 'q_schema' LIMIT 1"];
  $[count r;first r`v;""]}
/ @ignore
.parquet.i.schema:{[c;t]
  k:.duckdb.i.lit lower string t;
  if[0=first (.duckdb.exec[c;"SELECT count(*) AS n FROM duckdb_tables() WHERE database_name = current_database() AND schema_name = 'main' AND table_name = '_q_schema'"])`n; :()];
  .duckdb.exec[c;"SELECT c.column_name AS col, c.data_type AS dtype, s.logical, s.iskey FROM main._q_schema s JOIN duckdb_columns() c ON c.database_name = ",$[t=.duckdb.i.staging;"'temp'";"current_database()"]," AND c.schema_name = 'main' AND c.table_name = ",(.duckdb.i.lit string t)," AND translate(c.column_name, 'ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz') = s.col WHERE s.tbl = ",k," AND s.col <> '' ORDER BY c.column_index"]}
/ @ignore
.parquet.i.coord:{[t]
  if[-11h<>type v:value flip t; :()];
  if[not (s:string v) like ":pq:duckdb:*"; :()];
  i:last where s=":";
  (`$i#s; `$-1_(1+i)_s)}
/ @ignore
.parquet.i.copy:{[c;rel;file;sc;opts]
  kv:$[count sc;enlist "KV_METADATA {q_schema: ",(.duckdb.i.lit .j.j sc),"}";()];
  o:.duckdb.i.opts[{[k;v] .duckdb.i.ident[k]," ",.parquet.i.copyval v};opts];
  .duckdb.exec[c;"COPY ",rel," TO ",(.parquet.i.file file)," (",(", " sv (enlist "FORMAT PARQUET"),kv,o),")"]}

/ Write a table to a parquet file and answer the file.  A DuckDB-backed table copies in place; any other table is
/ staged through the main DuckDB database first.
/ @param file `:out.parquet, a directory `:out/ with partition_by, or a URL
/ @param opts a dict of COPY options passed verbatim (compression, row_group_size, partition_by, field_ids ...): a
/ symbol list is an identifier list, a dict a struct; () for none
/ @throws type file is not a symbol, or table is not a table
/ @eg .parquet.write[`:t.parquet;([] a:1 2);(enlist `compression)!enlist `zstd]
/ @eg .parquet.write[`:t/;([] d:2026.01.01 2026.01.02; a:1 2);(enlist `partition_by)!enlist `d]
.parquet.write:{[file;table;opts]
  if[-11h<>type file; '`type];
  if[98h<>type table; '`type];
  if[count ct:.parquet.i.coord table;
    .parquet.i.copy[ct 0;"(SELECT * FROM ",(.duckdb.i.qname ct 1),")";file;.parquet.i.schema . ct;opts]; :file];
  c:.duckdb.main[]; .duckdb.set[c;.duckdb.i.staging;$[-11h=type value flip table;select from table;table]];
  .parquet.i.copy[c;.duckdb.i.qname .duckdb.i.staging;file;.parquet.i.schema[c;.duckdb.i.staging];opts];
  .duckdb.hdel[c;.duckdb.i.staging]; file}

/ @ignore
.parquet.i.restore:{[rel;kv;query]
  c:.duckdb.main[];
  ds:update col:`$col from .duckdb.exec[c;"SELECT column_name AS col, column_type AS dtype FROM (DESCRIBE SELECT * FROM ",rel,")"];
  env:ds lj `col xkey select col:`$col, logical:`$logical, iskey from .j.k kv;
  .duckdb.set[c;.duckdb.i.staging;(flip (env`col)!(count env)#enlist ();env)];
  .duckdb.exec[c;"INSERT INTO ",(.duckdb.i.qname .duckdb.i.staging)," SELECT * FROM ",rel];
  r:$[.duckdb.i.none query;.duckdb.get[c;.duckdb.i.staging];.duckdb.i.push[c;.pq.i.resolveTree[env`col;(enlist .duckdb.i.staging),eval each 2_query]]];
  .duckdb.hdel[c;.duckdb.i.staging]; r}

/ Read a parquet file (or files) as a table, optionally through a select pushed down to DuckDB.
/ @param file `:x.parquet, a glob as `$":data/*.parquet", a URL, or a list of them
/ @param opts a dict of read_parquet options passed verbatim (hive_partitioning, union_by_name, filename ...); () for none
/ @param query () or :: for every row, else a parsed select tree (parse "select ... from t") whose table is ignored
/ @throws type query is not a select tree
/ @eg .parquet.read[`:t.parquet;();()]
/ @eg .parquet.read[`$":t/*/*.parquet";(enlist `hive_partitioning)!enlist 1b;()]
/ @eg .parquet.read[`:t.parquet;();parse "select sum a from t where a>1"]
.parquet.read:{[file;opts;query]
  rel:.parquet.i.rel[file;opts]; c:.duckdb.main[];
  if[not .duckdb.i.none query; if[not (?)~query 0; '`type]];
  if[count kv:.parquet.i.kv[c;file]; :.parquet.i.restore[rel;kv;query]];
  if[.duckdb.i.none query; :.duckdb.exec[c;"SELECT * FROM ",rel]];
  cl:cols .duckdb.exec[c;"SELECT * FROM ",rel," LIMIT 0"];
  .duckdb.i.push[c;.pq.i.resolveTree[cl;(enlist rel),eval each 2_query]]}

/ @ignore
.parquet.i.bytes:{[t]
  f:hsym `$$[count d:getenv `TMPDIR;d;count d:getenv `TEMP;d;"/tmp"],"/pq",(string .z.i),"_",(string "j"$.z.p),".parquet";
  .parquet.write[f;t;()]; b:read1 f; hdel f; b}

/ @ignore
.parquet.i.fn:{[fn;file] .duckdb.exec[.duckdb.main[];"SELECT * FROM ",fn,"(",.parquet.i.file[file],")"]}
/ The file's schema: one row per column, DuckDB's parquet_schema columns verbatim.
/ @eg .parquet.schema `:t.parquet
.parquet.schema:{[file] .parquet.i.fn["parquet_schema";file]}
/ One row per column chunk per row group: DuckDB's parquet_metadata.
/ @eg select path_in_schema, row_group_id, num_values from .parquet.metadata `:t.parquet
.parquet.metadata:{[file] .parquet.i.fn["parquet_metadata";file]}
/ The file-level metadata: created_by, num_rows, num_row_groups, format_version, encryption_algorithm.
/ @eg .parquet.file_metadata `:t.parquet
.parquet.file_metadata:{[file] .parquet.i.fn["parquet_file_metadata";file]}
/ The file's key-value metadata, one row per key (a q-written file carries q_schema).
/ @eg .parquet.kv_metadata `:t.parquet
.parquet.kv_metadata:{[file] .parquet.i.fn["parquet_kv_metadata";file]}
/ Per row group, whether the bloom filter on a column excludes a value: DuckDB's parquet_bloom_probe.
/ @eg .parquet.bloom_probe[`:t.parquet;`a;1]
.parquet.bloom_probe:{[file;column;val]
  args:", " sv (.parquet.i.file file;.duckdb.i.lit column;.duckdb.i.lit val);
  .duckdb.exec[.duckdb.main[];"SELECT * FROM parquet_bloom_probe(",args,")"]}
