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
q).parquet.read[`:data/a.parquet`:data/b.parquet;();()]                  / an explicit list, read in the order given
q).parquet.read[`$":data/depth/**/*.parquet"; (enlist `hive_partitioning)!enlist 1b;
    parse "select last px by sym from t where sym=`A, date within 2024.01.01 2024.01.31"]
```

peachq keeps what it learns about a local file (its columns and types) while the file is unchanged, so a repeated
query sends DuckDB one statement. To give a set of files a name, make a DuckDB view once:

```q
q)c:.duckdb.main[]
q).duckdb.exec[c;"CREATE VIEW depth AS SELECT * FROM read_parquet('data/depth/**/*.parquet', hive_partitioning=true)"]
q)depth:get `:pq:duckdb:main:depth/        / a q global bound to the view
q)select max px by sym from depth where date=2024.01.02
```

A bound global like `depth` is a pointer to the DuckDB table, not its rows. qSQL runs on it, but q verbs that need
the rows themselves (`` `sym xkey depth ``, `1!depth`, `t lj depth`) answer `'type`. That is deliberate: say which
rows you want first, `ref:select from depth where ...`, then key or join the q table that select returns.

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
| TIMESTAMP, `_S`, `_MS`, `_NS` | `p` | outside q's range reads as `0Wp` / `-0Wp` |
| TIMESTAMP WITH TIME ZONE | `p` | the UTC instant; the offset is not kept |
| TIME, TIME_NS | `n` | a timespan since midnight, so microseconds are kept |
| UUID | `g` | |
| ENUM | `s` | |
| BLOB | `X` | |
| BIT | `B` | |
| LIST of any of these | a list column | |
| HUGEINT, UBIGINT, UHUGEINT, BIGNUM, INTERVAL, TIME WITH TIME ZONE, STRUCT, MAP, UNION, JSON, GEOMETRY, untyped NULL | hidden | not in `cols`, `meta` or `select from` |

`.duckdb.hidden[h;`t]` and `.parquet.hidden[file;opts]` list the hidden columns. `h` is the handle of the database
that holds the table: `.duckdb.main[]` for a table in the main database, or the handle `.pq.hopen_duckdb` gave you
for an attached file (`h:.pq.hopen_duckdb[`ref;"ref.duckdb"]; .duckdb.hidden[h;`instruments]`). SQL still reaches them, so cast
one into a type q holds when you need it:

```q
q).duckdb.exec[c;"SELECT sym, CAST(notional AS DOUBLE) AS notional FROM trades"]
```

To see everything that does not read exactly, ask the inspector. `.duckdb.inspect[h;`t]` and
`.parquet.inspect[file;opts]` answer one row per column and issue, counted by DuckDB in one statement, so no row
comes back to q:

```q
q).duckdb.inspect[c;`inst]
col      dtype       qtype issue   rows
---------------------------------------
notional "HUGEINT"         hidden
sym      "VARCHAR"   C     merged  2
qty      "INTEGER"   i     minimum 1
valid_to "TIMESTAMP" p     range   2
```

The issues are `hidden` (no q type), `range` (a timestamp outside q's range, read as `0Wp` or `-0Wp`), `minimum`
(an integer type's lowest value, read as null), `merged` (NULL beside `''`, NaN, the zero UUID or an empty BLOB,
which read as one value), `zero` (a NULL boolean or byte, read as `0b` or `0x00`), `digits` (a DECIMAL wider than
15 digits whose value a float does not hold) and `offset` (a TIMESTAMP WITH TIME ZONE). `rows` counts the rows each
issue touches. A table or file q wrote answers an empty table.

## The rules

1. **One q type per DuckDB type**, as in the table above, applied the same way on every path.
2. **Nulls merge.** A SQL NULL and a value that is a q null in that type (the integer minimum, NaN) both read as the
   q null. A NULL string reads as `""`, a NULL boolean as `0b`, a NULL byte as `0x00`, a NULL list as an empty list.
   There are no extra columns. When you need to tell a NULL from a real minimum, ask DuckDB in SQL, or count where
   it happens with the inspector.
3. **Columns q cannot hold are hidden**, so `cols`, `meta` and `select from` always work. A timestamp q cannot hold
   reads as the infinity on its side, so a select over it answers, and a filter DuckDB runs keeps the rows q would.
   `.duckdb.inspect` and `.parquet.inspect` say where any of this happens.
4. **Text is a string that also answers to symbols.** On a foreign table, `` sym=`A ``, `` sym in `A`B `` and
   `sym like "A*"` work on a string column, where q itself would signal `'type` for the symbol, and `` `$sym ``
   gives you symbols in the result. A string literal means what it means in q: `sym="A"` compares character by
   character, so write `` sym=`A ``, `sym like "A"` or `sym~\:"A"` for the whole string.
5. **Row order is not promised.** `i`, `first`, `last` and `select[n]` answer from whatever order the rows arrive
   in, as a partitioned kdb database does. peachq uses a row number when DuckDB has one (a table's `rowid`, a file's
   row number, the order of an explicit file list), but do not rely on it; a view has none.
6. **A query answers as if the columns it reads were read into q first and the qSQL run there.** peachq runs as much
   of it inside DuckDB as gives the same answer, so only the columns and rows you need come back. A part DuckDB cannot
   run the same way is run by q, on the rows DuckDB returned.
7. **Round trips are exact through the pair.** `.duckdb.getx` answers `(data;schema)`, and `.duckdb.set` of that
   pair writes every column and value back exactly, hidden types and decimals included. The copy is a q table, so it
   also carries q's row index, a column `i`.

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
- **Out-of-range timestamps are infinities.** q timestamps read from DuckDB run from 1707 to 2262. A later one, such
  as the `9999-12-31` that marks a current row, reads as `0Wp`, an earlier one as `-0Wp`, so
  `select from inst where valid_to>.z.p` finds the current rows and `max valid_to` is `0Wp`. The order is kept,
  so a filter DuckDB runs keeps exactly the rows q would. q's timestamp infinities are not IEEE infinities: on such
  a row `valid_to-valid_from` is a huge, meaningless timespan, not an infinity, so check a table with
  `.duckdb.inspect` before doing arithmetic on its timestamps. A comparison with `0Wp` itself is run by q.
- **Out-of-range dates raise an error.** Only DuckDB's very earliest dates are past what a q date holds. A select
  that reads one fails with `'duckdb` (`.duckdb.err[]` names the column); a select that does not read that column
  answers.
- **Time zones are dropped.** TIMESTAMP WITH TIME ZONE reads as its UTC instant, whatever the DuckDB session's
  `TimeZone` setting is.

Columns

- **Hidden columns are silent.** A HUGEINT or STRUCT column simply is not there. Check `.duckdb.hidden` or
  `.parquet.hidden` when a column you expected is missing. A table whose every column is hidden reads as a table with
  no columns. DuckDB itself writes a HUGEINT to parquet as a DOUBLE, so in a file it reads as a float.
- **Hive partition columns come last** in `cols` and `meta`, after the file's own columns, and DuckDB guesses their
  type from the directory names (`date=2024-01-02` is a DATE, `region=EU` a VARCHAR).
- **A column named `i`** cannot be written to DuckDB: `i` is q's row index.

Queries

- **`count` of a handle is 1.** `` count `:pq:duckdb:main:t/ `` counts the symbol. Use
  ``count select from `:pq:duckdb:main:t/`` or bind the table to a global first.
- **Order-dependent answers can change.** `first`, `last`, `i`, `deltas` and `select[n]` depend on row order, which a
  foreign table does not promise. Sort or group on a real column when the order matters.
- **Filter on columns DuckDB can use first.** Equality, `in` and `within` on dates, timestamps, symbols/strings and
  integers run inside DuckDB and let it skip whole files and row groups. A float comparison (`px>100`) narrows the
  rows in DuckDB and is finished by q, because q compares floats with a tolerance; `px within 1 2` is run by q.
- **`like` patterns.** Plain characters with a leading and/or trailing `*` run in DuckDB. Any other pattern (`?`,
  `[..]`, a `*` in the middle) is narrowed in DuckDB by its literal prefix and finished by q.
- **Grouping by an expression** (`by a mod 2`) is run by q on the two columns DuckDB returns, so it reads every row;
  grouping by a column runs in DuckDB.
- **The string leniency covers symbols in comparisons, nothing else.** `=`, `<>`, `<`, `>`, `<=`, `>=`, `in`,
  `within` and `like` accept a symbol on a string column: a literal, a variable holding one, or (in a `where`) an
  expression that reads no column. A string literal is never reinterpreted: `sym="AB"` is q's character-wise compare,
  an error or a list per row exactly as on the in-memory table. `update` and `delete` have no leniency. On a q table,
  `` sym=`A `` against a string column is a `'type` error, as in kdb.
- **A value is computed up front only as a `where` operand.** In `` where qty>cfg`lim ``, ``where sym in key d`` or
  ``where sym in exec sym from ref`` the part that reads no column is evaluated once, where the select is written
  (so a function's locals count), and DuckDB is handed its value. The same kind of expression in a `by` or in the
  select phrase (`` select big:px>2*cfg`lim from t ``) is left to q: the answer is the same, but that select may run
  in q on the columns DuckDB returns instead of inside DuckDB. Assign the value to a variable first when you want it
  pushed.
- **`.duckdb.maxrows`** guards only q tables stored in DuckDB. A foreign table or a parquet file is never refused for
  its size, so a select with no filter DuckDB can run reads every row it names.
- **See what ran.** `.duckdb.sqllog` lists every statement sent to DuckDB, and `.duckdb.lastpush` the translation of
  the last select (`::` when nothing moved into DuckDB, so q did the work).
- **Session settings change answers.** `SET default_collation='nocase'` on the DuckDB connection makes some pushed
  text comparisons and groupings ignore case where q does not. Leave the collation alone.

SQL and writing

- **SQL answers keep DuckDB's own view.** `.duckdb.exec` and `s)` lines can show extra columns such as
  `qty_q_notnull` or `name_q_isnull`, which mark exactly where a NULL and a q null differ. qSQL never shows them.
- **A DECIMAL in a SQL answer is its unscaled integer.** `SELECT price FROM t` on a DECIMAL(10,2) column answers
  `10037` for 100.37, and `sum(price*size)` comes back 100 times too large. qSQL reads the same column as the
  float 100.37. In SQL, cast it yourself: `SELECT CAST(price AS DOUBLE) AS price FROM t`. `.duckdb.getx` answers the
  unscaled values together with the schema that holds their scale, which is how the exact round trip works.
- **Keyed tables.** `.parquet.write` refuses a keyed table with `'type`; write `0!t`. Remember a `select … by` result
  is keyed, so `0!` it before writing. `upsert` into a keyed table stored in
  DuckDB does not replace rows by key: a keyed table q stored gains duplicate keys, and a table with a DuckDB
  PRIMARY KEY refuses the upsert.
- **An empty table with a general-list column** (`0#([] s:("ab";"cd"))`) cannot be written, because it has no type.
  q has no empty string column to give it one, so create the table with SQL (`CREATE TABLE t (s VARCHAR)`) instead.
- **A parquet file peachq wrote and another tool rewrote** keeps the `q_schema` entry that says how to read it. peachq
  uses it for column types only, never trusts its values, and reads a column the entry does not describe, or an entry
  it cannot understand, as it would read a foreign file.

See also [Storing tables in DuckDB](sql-duckdb-storage.md) and [Parquet](lib/parquet.md).
