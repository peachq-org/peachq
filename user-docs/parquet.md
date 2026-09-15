# Reading parquet

`.parquet.read` loads a parquet file as a table. It is a thin shim over DuckDB: every verb is one SQL string run
through the `.duckdb` bridge, and every value crosses the same type codec a DuckDB table does. The `.parquet`
namespace arrives with `\l pq`; without the DuckDB library every verb signals the bare `'duckdb` and
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

Every `.parquet` verb runs on `.duckdb.main[]`, one connection opened on first use — today a connection to the
shared in-memory database (`` `:default: ``).

Writing parquet (`.parquet.write`, `` save `t.parquet ``) is not here yet.
