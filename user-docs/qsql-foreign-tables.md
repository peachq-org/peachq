# qSQL on DuckDB tables and parquet files

peachq runs qSQL on data it did not write: a table created in DuckDB by SQL, a DuckDB view, a parquet file written by
Spark, pandas, Polars or DuckDB, or a whole hive-partitioned directory of them. This page calls those **foreign**
tables. A table q wrote, to DuckDB with `set` or to parquet with `.parquet.write`, is a **q table**: it carries its q
types and reads back exactly as written, and nothing on this page changes that.

A foreign table has no q types of its own, so peachq gives every column one fixed q type by its DuckDB type. The
same table always reads in the same shape, whatever its rows hold.

## Point at the data

```q
q)select from `:pq:duckdb:main:trades/ where date=2024.01.02            / a DuckDB table or view
q)select px, sym from `:data/day.parquet where sz>100                    / one parquet file
q)select from `$":data/depth/**/*.parquet" where region like "EU"       / a glob; DuckDB finds hive partitions
q).parquet.read[`$":data/depth/**/*.parquet"; (enlist `hive_partitioning)!enlist 1b;
    parse "select last px by sym from t where sym=`A, date within 2024.01.01 2024.01.31"]
```

To query the same files many times, make a DuckDB view once. Its schema is read once and kept:

```q
q)c:.duckdb.main[]
q).duckdb.exec[c;"CREATE VIEW depth AS SELECT * FROM read_parquet('data/depth/**/*.parquet', hive_partitioning=true)"]
q)depth:get `:pq:duckdb:main:depth/        / a q global bound to the view
q)select max px by sym from depth where date=2024.01.02
```

## How each column reads

| DuckDB type | q type | notes |
|---|---|---|
| BIGINT, INTEGER, SMALLINT | `j` `i` `h` | |
| TINYINT, USMALLINT, UINTEGER | `h` `i` `j` | widened, so exact |
| DOUBLE, FLOAT | `f` `e` | `±inf` reads as `0w`/`-0w` |
| BOOLEAN | `b` | |
| UTINYINT | `x` | |
| VARCHAR | string (`C`) | never a symbol; see below |
| DECIMAL | `f` | a float: up to about 15 significant digits are exact |
| DATE | `d` | |
| TIMESTAMP, `_S`, `_MS`, `_NS` | `p` | |
| TIMESTAMP WITH TIME ZONE | `p` | the UTC instant; the offset is not kept |
| TIME, TIME_NS | `n` | a timespan since midnight, so microseconds are kept |
| UUID | `g` | |
| ENUM | `s` | |
| BLOB | `X` | |
| BIT | `B` | |
| LIST of any of these | a list column | |
| HUGEINT, UBIGINT, UHUGEINT, BIGNUM, INTERVAL, TIME WITH TIME ZONE, STRUCT, MAP, UNION, JSON, GEOMETRY | hidden | not in `cols`, `meta` or `select from` |

`.duckdb.hidden[c;`t]` and `.parquet.hidden[file;opts]` list the hidden columns. SQL still reaches them, so cast
one into a type q holds when you need it:

```q
q).duckdb.exec[c;"SELECT sym, CAST(notional AS DOUBLE) AS notional FROM trades"]
```

## The rules

1. **One q type per DuckDB type**, as in the table above, applied the same way on every path.
2. **Nulls merge.** A SQL NULL and a value that is a q null in that type (the integer minimum, NaN) both read as the
   q null. A NULL string reads as `""`, a NULL boolean as `0b`, a NULL byte as `0x00`, a NULL list as an empty list.
   There are no extra columns. When you need to tell a NULL from a real minimum, ask DuckDB in SQL.
3. **Columns q cannot hold are hidden**, so `cols`, `meta` and `select from` always work.
4. **Text is a string that also answers to symbols.** On a foreign table, `sym=`A`, `sym in `A`B`, `sym="A"` and
   `sym like "A*"` all work on a string column, and `` `$sym `` gives you symbols in the result.
5. **Row order is not promised.** `i`, `first`, `last` and `select[n]` answer from whatever order the rows arrive
   in, as a partitioned kdb database does. peachq uses a row number when DuckDB has one, but do not rely on it.
6. **A query answers as if the whole table were read into q first and the qSQL run there.** peachq runs as much of
   it inside DuckDB as gives the same answer, so only the rows you need come back. A part DuckDB cannot run the same
   way is run by q, on the rows DuckDB returned.
7. **Round trips are exact through the pair.** `.duckdb.getx` answers `(data;schema)`, and `.duckdb.set` of that
   pair writes every column back exactly, hidden types and decimals included.

## Gotchas

Types and values

- **Strings, not symbols.** A text column is a string column because peachq cannot know in advance how many distinct
  values it holds, and interning a column of order ids would fill the symbol table. Cast in the select when you want
  symbols: ``select `$sym, px from t``.
- **`""` and NULL are the same string.** `where sym like ""` matches both. So are the integer minimum and NULL, and
  NaN and NULL. Use SQL to separate them: `.duckdb.exec[c;"SELECT count(*) FROM t WHERE qty = -2147483648"]`.
- **Decimals are floats.** A DECIMAL(38,10) notional loses digits past about 15 significant figures, and a `sum` can
  differ from DuckDB's exact decimal sum in the last bits. Writing the result back makes a DOUBLE column, not a
  DECIMAL; use the `.duckdb.getx` pair for an exact copy.
- **TIME is a timespan, not a `t`.** It shows as `0D09:30:00.123456000`; `date+time` gives a timestamp.
- **A NULL boolean is `0b`.** q booleans have no null, so `where not flag` also matches rows whose flag was NULL.
- **Out-of-range dates and timestamps raise an error.** q timestamps run from 1707 to 2262. A row outside that range
  makes the read fail with `'duckdb` (`.duckdb.err[]` names the column) rather than come back as a null.
- **Time zones are dropped.** TIMESTAMP WITH TIME ZONE reads as its UTC instant, whatever the DuckDB session's
  `TimeZone` setting is.

Columns

- **Hidden columns are silent.** A HUGEINT or STRUCT column simply is not there. Check `.duckdb.hidden` or
  `.parquet.hidden` when a column you expected is missing.
- **Hive partition columns come last** in `cols` and `meta`, after the file's own columns, and DuckDB guesses their
  type from the directory names (`date=2024-01-02` is a DATE, `region=EU` a VARCHAR).
- **A column named `i`** cannot be written to DuckDB: `i` is q's row index.
- **A foreign column named like an internal companion** (`v_q_isnull` next to a column `v`) is refused.

Queries

- **`count` of a handle is 1.** `` count `:pq:duckdb:main:t/ `` counts the symbol. Use
  ``count select from `:pq:duckdb:main:t/`` or bind the table to a global first.
- **Order-dependent answers can change.** `first`, `last`, `i`, `deltas` and `select[n]` depend on row order, which a
  foreign table does not promise. Sort or group on a real column when the order matters.
- **Filter on columns DuckDB can use first.** Equality, `in` and `within` on dates, symbols/strings and integers run
  inside DuckDB and let it skip whole files and row groups. Float comparisons (`px>100`, `px within 1 2`) are run by
  q after the read, because q compares floats with a tolerance.
- **`like` patterns with `?` or `[..]`** may run in q rather than DuckDB. Patterns made of plain characters and `*`
  always run in DuckDB.
- **The string leniency is for foreign tables only.** On a q table, `sym=`A` against a string column is a `'type`
  error, as in kdb.
- **Large tables need a filter.** Above `.duckdb.maxrows` rows, a select with no filter DuckDB can run is `'limit`
  before anything is read.
- **See what ran.** `.duckdb.sqllog` lists every statement sent to DuckDB, and `.duckdb.lastpush` the translation of
  the last select. A `SELECT *` there means q did the work.

SQL and writing

- **SQL answers keep DuckDB's own view.** `.duckdb.exec` and `s)` lines can show extra columns such as
  `qty_q_notnull` or `name_q_isnull`, which mark exactly where a NULL and a q null differ. qSQL never shows them.
- **Keyed tables.** `.parquet.write` refuses a keyed table; write `0!t`. An `upsert` into a keyed DuckDB table
  appends rows rather than replacing them.
- **An empty table with a general-list column** (`0#([] s:("ab";"cd"))`) cannot be written, because it has no type.
  q has no empty string column to give it one, so create the table with SQL (`CREATE TABLE t (s VARCHAR)`) instead.
- **A parquet file peachq wrote and another tool rewrote** keeps the `q_schema` entry that says how to read it, and
  that entry may no longer match the file's columns.

See also [Storing tables in DuckDB](sql-duckdb-storage.md) and [Parquet](lib/parquet.md).
