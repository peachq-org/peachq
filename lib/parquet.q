/ Read parquet files as tables: .parquet.read[file;opts;query] answers a table, and .parquet.schema, .metadata,
/ .file_metadata, .kv_metadata and .bloom_probe answer DuckDB's own tables verbatim.  `select from `:x.parquet` is
/ the shortest spelling.  \?.parquet lists the verbs; user-docs/parquet.md has the detail.
/ @implNote THE SHIM LAW: every verb is ONE SQL string on .duckdb.main[] through .duckdb.exec, every value crosses
/ the .duckdb codec untouched (a foreign file comes back as DuckDB reads it: text is text, never symbols), and there
/ is no parquet C.  DuckDB's httpfs is the transport for a URL, the one exception to handles.md rule 15.  A missing
/ library or file is the bare 'duckdb, .duckdb.err[] the reason.  ANY-ORDER LAW: definitions only at top level.

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

/ key=value: a key is an identifier (the one unquoted text in the SQL, so anything else is 'type, never spliced)
.parquet.i.opt:{[k;v] s:string k; if[not (0<count s)&all s in .Q.an; '`type]; s,"=",.parquet.i.lit v}

/ read_parquet(file, key=value, ...): every opts key rides verbatim, so an unknown one is DuckDB's binder error
.parquet.i.rel:{[file;opts]
  o:$[.parquet.i.none opts;();(99h=type opts)and 11h=type key opts;.parquet.i.opt'[key opts;value opts];'`type];
  "read_parquet(",(", " sv (enlist .parquet.i.file file),o),")"}

/ query is a parsed select tree whose table position is ignored.  It rides the one qSQL seam every provider does
/ (.pq.i.resolveTree over the relation's columns, then .duckdb.i.push) - never an evaluator of its own.  eval of
/ each slot lands the functional-form values the host hands that seam (a parsed where slot is ,,(>;`a;`lim))
.parquet.read:{[file;opts;query]
  rel:.parquet.i.rel[file;opts]; c:.duckdb.main[];
  if[.parquet.i.none query; :.duckdb.exec[c;"SELECT * FROM ",rel]];
  if[not (?)~query 0; '`type];
  cl:cols .duckdb.exec[c;"SELECT * FROM ",rel," LIMIT 0"];
  .duckdb.i.push[c;.pq.i.resolveTree[cl;(enlist rel),eval each 2_query]]}

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
