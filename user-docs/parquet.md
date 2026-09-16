# Parquet

`.parquet.read` loads a parquet file as a table and `.parquet.write` writes one. Both are a thin shim over DuckDB:
every verb is SQL run through the `.duckdb` bridge, and every value crosses the same type codec a DuckDB table does.
The `.parquet` namespace arrives with `\l pq`; without the DuckDB library every verb signals the bare `'duckdb` and
`.duckdb.err[]` says why.

```q
q)\l pq
q)select from `:test/data/parquet/gold_vs_bitcoin.parquet          / the shortest spelling
q).parquet.read[`:test/data/parquet/gold_vs_bitcoin.parquet;();()]
q)meta `:test/data/parquet/gold_vs_bitcoin.parquet
c      | t f a
-------| -----
time   | p
gold   | f
bitcoin| f
```

A `.parquet` suffix makes a file symbol a table to qSQL, `cols` and `meta`, exactly as `.csv` and `.json` do; the
door is `.parquet.read[file;();()]`, so `select from` a parquet file before `\l pq` signals `'.parquet.read` — the name
it could not find.

## `.parquet.read[file;opts;query]`

**file** is a file symbol. The leading colon is dropped and the rest is handed to DuckDB's `read_parquet`
verbatim, so everything `read_parquet` accepts as a path works: a glob (`` `$":part/*/*.parquet" `` — `*` is not a
bare-symbol character), a hive-partitioned tree, an `http://` or `https://` URL, or a symbol LIST, which becomes
DuckDB's list form `['a.parquet','b.parquet']`. A non-symbol file is `'type`; a file DuckDB cannot find is `'duckdb`.

**opts** is a dictionary of `read_parquet` named parameters, or `()`, `()!()` or `(::)` for none. Keys and values
ride verbatim: a boolean is `true`/`false`, an int or float is the number, a symbol or string is `'quoted'`, a
symbol list is `['a','b']`; any other value type is `'type`. Nothing checks the keys here, so an unknown option is
DuckDB's own binder error behind `'duckdb` — never silently ignored:

```q
q).parquet.read[f;enlist[`file_row_number]!enlist 1b;()]      / one more column, file_row_number
q).parquet.read[f;`hive_partitioning`union_by_name!11b;()]
q).parquet.read[f;enlist[`bogus]!enlist 1b;()]
'duckdb
q).duckdb.err[]
"Binder Error: Invalid named parameter \"bogus\" for function read_parquet..."
```

**query** is `()`, `()!()` or `(::)` for the whole file, or a parsed select tree — the shape `parse` gives a
`select` statement — whose table position is ignored:

```q
q).parquet.read[f;();parse "select max gold by 0D01 xbar time from t"]
```

The query rides the one qSQL seam the DuckDB provider uses (`.duckdb.qsql`'s resolver and `.duckdb.i.push`), so it
answers exactly what the same `select` over `.parquet.read[f;();()]` answers, and it will be pushed down to DuckDB
the day that seam translates qSQL to SQL, with no change to `.parquet`.

## The transport is DuckDB's

A URL is handed to DuckDB as-is: reading a parquet footer needs range reads, and DuckDB's `httpfs` extension does
them (it autoloads on first use). This is the one documented exception to the handles rule against combined
transport-and-format implementations — peachq's own HTTP client never fetches the bytes.

## Foreign files come back as DuckDB types

A file peachq did not write carries no q schema, so you get what DuckDB reads: text is a string column (never
symbols), `TIMESTAMP_MICROS` is `p`, `DATE` is `d`, `INT64` is `j`, `DOUBLE` is `f`, and a null is a null. Column
names come through as-is, spaces and parentheses included:

```q
q)meta .parquet.read[`:test/data/parquet/bank_failures.parquet;();()]
c             | t f a
--------------| -----
c1            | j
Bank          | C
City          | C
State         | C
Date          | d
Acquired by   | C
Assets ($mil.)| f
```

## The metadata verbs

Each answers the DuckDB table function of the same name, verbatim — DuckDB's columns, DuckDB's rows:

| verb | DuckDB function |
|---|---|
| `.parquet.schema[file]` | `parquet_schema(file)` — one row per column of the file's schema |
| `.parquet.metadata[file]` | `parquet_metadata(file)` — one row per column chunk per row group |
| `.parquet.file_metadata[file]` | `parquet_file_metadata(file)` — `created_by`, `num_rows`, `num_row_groups`, ... |
| `.parquet.kv_metadata[file]` | `parquet_kv_metadata(file)` — the file's key-value metadata |
| `.parquet.bloom_probe[file;column;value]` | `parquet_bloom_probe(file, 'column', value)` — which row groups a bloom filter excludes |

```q
q)`name`type#.parquet.schema f      / a column is named type, which select reads as the verb
q).parquet.file_metadata[f]`num_rows
,517
```

Every verb runs on `.duckdb.main[]`, the one DuckDB database of the process; a write, and a read that restores q
types, stage on its reserved TEMP table `_q_staging` (below).

## Writing parquet

```q
q)t:([] s:`a`b; p:1.5 2.5; c:"xy")
q).parquet.write[`:x.parquet;t;()]
`:x.parquet
q)`:x.parquet set t                               / the same write, kdb spelling
q)save `t.parquet                                 / the global t to t.parquet in the current directory
q)t ~ select from `:x.parquet
1b
```

### `.parquet.write[file;table;opts]`

**file** is a file symbol, answered back. A directory (`` `:out/ ``) is the target of a partitioned write; an
`s3://` URL is handed to DuckDB's httpfs as a read URL is. A non-symbol file is `'type`.

**table** is a plain table; a keyed table or anything else is `'type`. A q table is staged through `.duckdb.set`
as `_q_staging` — a TEMP table of main's connection, reserved for this, that never enters a `-duckdb` file — and
`COPY`ed out, and the staging is dropped after the write. A table bound to a DuckDB pointer (`` get `:pq:duckdb:al:t/ ``) is copied
in place — `COPY (SELECT * FROM t) TO ...` on its own connection, no q round trip. A table bound to any other
provider is materialised through q first.

**opts** is a dictionary of `COPY ... (FORMAT PARQUET, ...)` options, or `()`, `()!()` or `(::)` for none. Values
ride verbatim: a boolean, number, symbol or string as the literal, a symbol LIST as an identifier list — the
spelling `PARTITION_BY (a, b)` wants — and a dictionary as DuckDB's struct spelling (`FIELD_IDS {sym: 42}`); any
other value is `'type`, and an unknown key is DuckDB's own error behind `'duckdb`:

```q
q).parquet.write[`:x.parquet;t;`compression`compression_level`row_group_size!(`zstd;6;100000)]
q).parquet.write[`:out/;t;`partition_by`overwrite!((enlist `s);1b)]      / out/s=a/data_0.parquet, out/s=b/...
q).parquet.read[`$":out/*/*.parquet";enlist[`hive_partitioning]!enlist 1b;()]
q).parquet.write[`:x.parquet;t;`parquet_version`field_ids!(`V2;`s`p!42 43)]
q).parquet.write[`:x.parquet;t;enlist[`bogus]!enlist 1b]
'duckdb
q).duckdb.err[]
"Not implemented Error: Unrecognized option \"bogus\" for parquet..."
```

The options DuckDB documents for a parquet `COPY`: `compression` (`uncompressed`, `snappy`, `gzip`, `zstd`, `brotli`,
`lz4`, `lz4_raw`), `compression_level`, `row_group_size`, `row_group_size_bytes`, `row_groups_per_file`,
`parquet_version` (`V1`, `V2`), `field_ids`, `partition_by`, `write_partition_columns`, `per_thread_output`,
`filename_pattern`, `file_size_bytes`, `overwrite`, `overwrite_or_ignore`, `append`, `use_tmp_file`.

A failed write leaves `_q_staging` in place until the next `.parquet` call reclaims it. That is deliberate: every
bridge call clears `.duckdb.err[]`, and the reason for the failure is worth more than a tidy catalog. The name is
reserved for the bridge: `.duckdb.load` and `s)` never bind it, `.duckdb.set` takes it (that is how `.parquet`
stages), and whatever you put there yourself the next `.parquet` call replaces.

### The `q_schema` law: what we write, we read back exactly

A parquet file has no symbol, char, month, minute, second, time, datetime or timespan: DuckDB writes them as
`VARCHAR`, `DATE`, `INTERVAL`, `TIMESTAMP` and `BIGINT`, and a bare read gives those back. So a file peachq writes
from a table with a sidecar (every q table has one once staged; a DuckDB table has one when `.duckdb.set` created
it) carries a key `q_schema` in its key-value metadata: those sidecar rows in the vocabulary `.duckdb.getx` hands
back — `col dtype logical iskey`, one row per column, as JSON (`.parquet.kv_metadata` shows it). On read, a file
with the key is staged under a temp name WITH those rows, so the codec's own declared-schema leg restores the q
types; a file without it (a foreign file, or one written from a SQL-created DuckDB table, which has no sidecar)
comes back as DuckDB reads it. There is no q-side cast table: the restore is the same leg `.duckdb.get` uses for any table, and
`t ~ .parquet.read[.parquet.write[f;t;()];();()]` holds for every basic type.

### `set`, `save` and the download door

`` `:f.parquet set t `` is `.parquet.write[`:f.parquet;t;()]` — the suffix names the format, as it does for every
`.h.tx` key ([handles.md](handles.md) § Format inference). `` save `t.parquet `` and the web server's
`/name.parquet?expr` door both go through `.h.tx[`parquet]`, which answers the file's BYTES (`.parquet.i.bytes`:
a write to a temp file, `read1`, `hdel`); `save` writes them with `1:`, the door sends them verbatim. Before `\l pq`
the entry signals `'.parquet.i.bytes`, the name it could not find, and `` `:f.parquet set t `` signals
`'.parquet.write`. `` get `:f.parquet `` is `.parquet.read[`:f.parquet;();()]` — the same table `select from` reads,
materialised (kx signals `'type` there).

### S3

`` `:s3://bucket/x.parquet set t `` and `.parquet.write[`$":s3://bucket/x.parquet";t;()]` `COPY` through httpfs
(credentials are DuckDB's: `CREATE SECRET` on `.duckdb.main[]`). The other formats to an `s3://`, `gcs://`, `az://`
or `hf://` target wait on a byte transport for writes (B3).
