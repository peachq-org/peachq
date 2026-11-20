/ Read and write parquet files through DuckDB: .parquet.read answers a table, .parquet.write writes one, and the
/ schema, metadata, file_metadata, kv_metadata and bloom_probe verbs answer DuckDB's own inspection tables.
/ `select from `:x.parquet` and `:x.parquet set t are the shortest spellings; save `t.parquet also works.
/ A file written from a q table carries its q types in the key-value metadata, so it reads back as written.  Any other
/ file reads as .duckdb.get reads a table q did not write: one q type per DuckDB type, nulls as q nulls, text as strings
/ (never symbols), no companion columns, and a column whose type q does not hold left out (.parquet.hidden names them).
/ A select on either kind, a glob included, runs what it can inside DuckDB, so a where or a by reads only the columns and
/ row groups it needs.  A URL (s3:// ...) passes straight through.
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
.parquet.i.rel:{[src;opts] "read_parquet(",(", " sv (enlist src),.duckdb.i.opts[.parquet.i.opt;opts]),")"}
/ @ignore
.parquet.i.kv:{[c;src]
  r:.duckdb.exec[c;"SELECT decode(value) AS v FROM parquet_kv_metadata(",src,") WHERE decode(key) = 'q_schema' LIMIT 1"];
  $[count r;first r`v;""]}
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
  if[count ct:.parquet.i.coord table; sc:.duckdb.i.schema[ct 0;ct 1]; if[count sc; sc:select from sc where col<>`i];
    .parquet.i.copy[ct 0;"(SELECT ",($[count sc; ", " sv .duckdb.i.qname each sc`col; "*"])," FROM ",(.duckdb.i.qname ct 1),")";file;sc;opts]; :file];
  c:.duckdb.main[]; .duckdb.set[c;.duckdb.i.staging;$[-11h=type value flip table;select from table;table]];
  .parquet.i.copy[c;.duckdb.i.qname .duckdb.i.staging;file;.duckdb.i.schema[c;.duckdb.i.staging];opts];
  .duckdb.hdel[c;.duckdb.i.staging]; file}

/ @ignore
.parquet.i.desc:{[c;rel]
  update col:`$col from .duckdb.exec[c;"SELECT column_name AS col, column_type AS dtype FROM (DESCRIBE SELECT * FROM ",rel,")"]}
/ What a q_schema entry says q wrote, ([col] logical): the two fields a read needs, as text.  A row without them is left
/ out, any other field is unread (a file has no key, so iskey says nothing), and an entry that is no JSON list of rows
/ says nothing at all.
/ @ignore
.parquet.i.qschema:{[kv]
  j:@[.j.k;kv;{[e] ()}]; if[not (type j) in 0 98h; j:()];
  f:{[k;x] $[99h<>type x;"";not k in key x;"";10h=type x k;x k;""]};
  n:f[`col] each j; l:f[`logical] each j; i:where (0<count each n) and 0<count each l;
  ([col:`$n i] logical:`$l i)}
/ The view of the files src names in SQL (.duckdb.i.fview): their columns as DuckDB declares them, beside what the q_schema
/ entry of head, the first of them, says q wrote.  Its rows are ordered by row number, after the file where src may name
/ more than one, through read_parquet's helper columns, unless the caller's options or the file's own columns already use
/ those names.
/ @ignore
.parquet.i.build:{[c;src;head;one;opts]
  qs:.parquet.i.qschema .parquet.i.kv[c;head];
  o:$[.duckdb.i.none opts;()!();opts]; h:$[one;1_;::] `filename`file_row_number!("_q_file";1b);
  rel:.parquet.i.rel[src;o,h];
  d:$[any `filename`file_row_number in key o;();@[.parquet.i.desc[c];rel;{[e] ()}]];
  if[count d;
    :.duckdb.i.fview[rel;(select from d where not col in `$$[one;1_;::] ("_q_file";"file_row_number")) lj qs;
      $[one;1_;::] ("\"_q_file\"";"file_row_number")]];
  rel:.parquet.i.rel[src;opts];
  .duckdb.i.fview[rel;.parquet.i.desc[c;rel] lj qs;()]}

/ the read_parquet options under which DuckDB reads a source's columns from its file names and its first file alone
/ @ignore
.parquet.i.FIRST:`hive_partitioning`filename`file_row_number
/ What a local source is made of, () for a URL or a file that will not stat: files, in read order (DuckDB expands a glob),
/ and stamp, which differs whenever the view of the source might: the file names with the size and time (.fs.i.stamp) of
/ each file the columns are read from, the first alone for one file or glob under .parquet.i.FIRST.  stamp is () while
/ such a file is under two seconds old: a file system may keep times in whole seconds, and a rewrite within the one a
/ stamp was taken in would not show.
/ @ignore
.parquet.i.stat:{[c;file;o]
  ps:{[x] $[":"=first x;1_x;x]} each string (),file;
  if[any ps like "*://*"; :()];
  fl:raze {[c;p] $[any p in "*?[{";.duckdb.exec[c;"SELECT file AS f FROM glob(",.duckdb.i.lit[p],")"]`f;enlist p]}[c] each ps;
  b:.fs.i.stamp each $[(1=count ps) and all key[o] in .parquet.i.FIRST;1 sublist fl;fl];
  if[(0=count b) or any null raze b; :()];
  `files`stamp!(fl;$[all b[;1]<("j"$.z.p)-2000000000;(fl;b);()])}
/ how many sources keep their view; one more takes the place of the oldest
/ @ignore
.parquet.i.SLOTS:16
/ the kept views, each (source;stamp;view), and how many sources have taken a place
/ @ignore
.parquet.i.cache:()
/ @ignore
.parquet.i.turn:0
/ THE view of a source, every parquet door's one way in.  A local source keeps its view while its stamp is unchanged
/ (.parquet.i.stat), so a later read asks DuckDB for the stamp alone.  Several files are read as the list the stamp was
/ taken of, held in a DuckDB variable: the statement neither expands the glob again nor reads a file the view was not
/ built from.  A URL has no stamp and is asked afresh on every read, its q_schema entry the first any of its files has.
/ @ignore
.parquet.i.view:{[c;file;opts]
  s:.parquet.i.file file; o:$[.duckdb.i.none opts;()!();opts];
  st:@[.parquet.i.stat[c;file];o;{[e] ()}];
  if[0=count st; :.parquet.i.build[c;s;s;0b;opts]];
  k:(file;o); i:first where k~/:{[x] x 0} each .parquet.i.cache;
  if[not null i; if[(count st`stamp) and st[`stamp]~.parquet.i.cache[i;1]; :.parquet.i.cache[i;2]]];
  if[null i; i:.parquet.i.turn mod .parquet.i.SLOTS; .parquet.i.turn+:1];
  fl:st`files; nm:"_q_pf",string i;
  if[1<count fl; .duckdb.exec[c;"SET VARIABLE ",nm," = [",(", " sv .duckdb.i.lit each fl),"]"]];
  v:.parquet.i.build[c;$[1<count fl;"getvariable('",nm,"')";.duckdb.i.lit first fl];.duckdb.i.lit first fl;1=count fl;opts];
  $[i<count .parquet.i.cache;.parquet.i.cache[i]:(k;st`stamp;v);.parquet.i.cache,:enlist (k;st`stamp;v)]; v}

/ The columns of a file that .parquet.read does not show because q has no type for theirs; DuckDB's SQL still reads them.
/ A file written from a q table hides none.
/ @param opts the read_parquet options, as .parquet.read takes them
/ @return a table ([] col; dtype), dtype as DuckDB declares it
/ @eg .parquet.hidden[`:t.parquet;()]
.parquet.hidden:{[file;opts] .parquet.i.view[.duckdb.main[];file;opts]`hidden}

/ Read a parquet file (or files) as a table, optionally through a select pushed down to DuckDB.
/ @param file `:x.parquet, a glob as `$":data/*.parquet", a URL, or a list of them
/ @param opts a dict of read_parquet options passed verbatim (hive_partitioning, union_by_name, filename ...); () for none
/ @param query () or :: for every row, else a parsed select tree (parse "select ... from t") whose table is ignored
/ @throws type query is not a select tree
/ @eg .parquet.read[`:t.parquet;();()]
/ @eg .parquet.read[`$":t/*/*.parquet";(enlist `hive_partitioning)!enlist 1b;()]
/ @eg .parquet.read[`:t.parquet;();parse "select sum a from t where a>1"]
.parquet.read:{[file;opts;query]
  c:.duckdb.main[];
  if[not .duckdb.i.none query; if[not (?)~query 0; '`type]];
  v:.parquet.i.view[c;file;opts];
  $[.duckdb.i.none query;.duckdb.i.fpull[c;v];.duckdb.i.fselect[c;v;.pq.i.resolveTree[v`cols;(enlist v`rel),eval each 2_query]]]}
/ The suffix door's select (select ... from `:f.parquet): tree is its arguments as ? takes them, env what each plain name
/ they mention means where the select was written, a local included.  A name that is no column takes that meaning; one
/ with none leaves the select to q (::), as does a restricted evaluation that may not load the translator.
/ @ignore
.parquet.i.select:{[file;tree;env]
  if[not @[{[u] .sql.translate; 1b};::;{[e] 0b}]; :(::)];
  c:.duckdb.main[]; v:.parquet.i.view[c;file;()];
  f:{[cl;env;x]
    if[(type x) in 0 99h; :.z.s[cl;env] each x];
    if[-11h<>type x; :x]; if[x in cl; :x];
    e:$[x in key env;env x;"."=first string x;value x;'`unpush];
    $[(11h=abs type e) or (type e) in 0 98 99h;enlist e;e]};
  rt:.[{[f;cl;env;t] (enlist t 0),f[cl;env] each 1_t}[f];(v[`cols],`i;env;tree);{[e] `unpush}];
  $[`unpush~rt;::;.duckdb.i.fselect[c;v;rt]]}

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
