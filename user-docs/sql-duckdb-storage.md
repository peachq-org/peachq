# Storing tables in DuckDB

peachq can keep your q tables in a DuckDB database file and query them in place, with qSQL or with SQL. A select
on a stored table runs as much of the query as it can inside DuckDB, so only the rows you asked for come back into q.

You need the `-duckdb` download, which carries the DuckDB library and its extensions beside `q`. With the standard
download, any DuckDB call fails with `'duckdb`, and `.duckdb.err[]` says why.

## Store a table

Open a database file under a name of your choosing (here `mkt`). The file is created if it does not exist. Every
table in it is then reachable as `` `:pq:duckdb:mkt:<table>/ ``:

```q
q)h:.pq.hopen_duckdb[`mkt;"mkt.duckdb"]
q)h
`:pq:duckdb:mkt
q)trade:([] sym:`IBM`MSFT`IBM`AAPL`MSFT; price:101.5 250.2 102.0 180.1 251.0; size:100 200 150 300 50)
q)`:pq:duckdb:mkt:trade/ set trade
`trade
q)new:([] sym:enlist `AAPL; price:enlist 181.5; size:enlist 75)
q)`:pq:duckdb:mkt:trade/ upsert new
`trade
q)trade,:new
q)count get `:pq:duckdb:mkt:trade/
6
q)hclose h
q)h:.pq.hopen_duckdb[`mkt;"mkt.duckdb"]
```

`set` writes the whole table, replacing any table of that name, and `upsert` appends rows. The data
is in the file once the call returns. Close the handle, open the same path again, and the table is still there.
A keyed table stays keyed, and every column keeps its q type, nulls included.

## Query it

A select on the coordinate answers exactly as the same select on the in-memory table does:

```q
q)select from `:pq:duckdb:mkt:trade/ where sym=`IBM
| sym    | price | size |
| symbol | float | long |
|--------|-------|------|
| IBM    | 101.5 | 100  |
| IBM    | 102   | 150  |
q)select hi:max price, vol:sum size by sym from `:pq:duckdb:mkt:trade/
| sym    | hi    | vol  |
| symbol | float | long |
|========|-------|------|
| AAPL   | 181.5 | 375  |
| IBM    | 102   | 250  |
| MSFT   | 251   | 250  |
q)select[2] from `:pq:duckdb:mkt:trade/ where size>60
| sym    | price | size |
| symbol | float | long |
|--------|-------|------|
| IBM    | 101.5 | 100  |
| MSFT   | 250.2 | 200  |
q)(select hi:max price, vol:sum size by sym from trade)~select hi:max price, vol:sum size by sym from `:pq:duckdb:mkt:trade/
1b
```

The table is also an ordinary DuckDB table, so you can query it with SQL, either on the handle or as an `s)` line,
where it is `mkt.trade`:

```q
q)h "SELECT sym, avg(price) AS px FROM trade GROUP BY sym ORDER BY sym"
| sym    | px     |
|        | float  |
|--------|--------|
| "AAPL" | 180.8  |
| "IBM"  | 101.75 |
| "MSFT" | 250.6  |
q)s)SELECT count(*) AS n FROM mkt.trade
| n    |
| long |
|------|
| 6    |
```

SQL answers in DuckDB's types (text comes back as strings). qSQL answers in your table's own q types.

## Pass q values into SQL

Inside a `` ``sql … `` `` literal, `$name` stands for the q variable `name`. peachq binds the variable's value as
a parameter when the line runs, so you never paste values into the SQL text. The variable is looked up as any
name in that expression would be: a function argument, a local, or a global. This works the same at the REPL and
inside a function:

```q
q)lo:120
q)h ``sql SELECT sym, size FROM trade WHERE size > $lo``
| sym    | size |
|        | long |
|--------|------|
| "MSFT" | 200  |
| "IBM"  | 150  |
| "AAPL" | 300  |
q)bigger:{[n] h ``sql SELECT sym, size FROM trade WHERE size > $n ORDER BY size``}
q)bigger 150
| sym    | size |
|        | long |
|--------|------|
| "MSFT" | 200  |
| "AAPL" | 300  |
```

The function `bigger` uses `$n`, so `n` is its argument. A text with no `$name` markers is just a string. A marker
that is not a q name, such as `$_y`, is a parse error. So are two names that differ only in case (`$x` and `$X`),
because DuckDB treats them as one.

## See what is stored, and what ran

`.pq.info` on the handle lists the tables it holds, with their row counts. `.duckdb.sqllog` records every statement
peachq sent to DuckDB:

```q
q)(.pq.info h)`tables
| table  | origin | bound                         | rows |
| symbol | symbol | timestamp                     | long |
|--------|--------|-------------------------------|------|
| trade  | q      | 2026.10.21D19:58:34.699001848 | 6    |
q).duckdb.sqllog::0#.duckdb.sqllog
q)select avg price by sym from `:pq:duckdb:mkt:trade/ where size>60
| sym    | price  |
| symbol | float  |
|========|--------|
| AAPL   | 180.8  |
| IBM    | 101.75 |
| MSFT   | 250.2  |
q)exec sql from .duckdb.sqllog
"SELECT sym, nullif(avg(price), CAST('NaN' AS DOUBLE)) AS price FROM trade WHERE size > $pq1 GROUP BY sym ORDER BY sym"
```

That select ran entirely in DuckDB: the filter, the grouping and the averages, with one row per group coming back.

## Parquet and deleting a table

A file ending in `.parquet` is written and read through the same DuckDB library (see [parquet.md](lib/parquet.md)).
`hdel` drops a table from the database:

```q
q)`:trade.parquet set trade
`:trade.parquet
q)select from `:trade.parquet where size>100
| sym    | price | size |
| symbol | float | long |
|--------|-------|------|
| MSFT   | 250.2 | 200  |
| IBM    | 102   | 150  |
| AAPL   | 180.1 | 300  |
q)hdel `:pq:duckdb:mkt:trade/
`:pq:duckdb:mkt:trade/
q)(.pq.info h)`tables
| table  | origin | bound     | rows |
| symbol | symbol | timestamp | long |
|--------|--------|-----------|------|
q)hclose h
```

<details><summary>Pushdown: what runs in DuckDB, what runs in q</summary>

A select on a stored table sends DuckDB what it can answer exactly as q would, and q does the rest on the rows that
come back. The answer is always q's answer. What moves is only where the work happens:

- **Runs in DuckDB:** comparisons on ints, symbols, dates and timestamps, `in`, `within`, `null`, `not`, `and`/`or`;
  `by` on columns; `count`, `sum`, `avg`, `min`, `max`, `first`, `last`, `dev`, `var`, `wavg` and the like;
  `select[n]`, `select[m n]`, `select[-n]` and `select[>col]`; the row index `i`.
- **Float comparisons run in q.** q compares floats with a tolerance and DuckDB does not, so `where price>200`
  brings the table back and q filters it. Only `in` and `null` on a float column run in DuckDB.
- **Casts of a column (`time.minute`, `date.month`), `xbar` on a time, and minute or second columns run in q.**
- **`fby`, `prior`, `mavg`, `med`, a function you wrote, `aj` and the other joins run in q.** A where term after
  one of them runs in q too, because it may depend on the rows before it.
- **Any part that cannot run in DuckDB keeps `by` and the select phrase in q.** The where terms DuckDB can run still
  move to it.
- **If DuckDB fails** on something q answers (an int overflow that q wraps, say), q answers the whole select. You
  get q's result, never a different one.
- **`.duckdb.maxrows`** (default `0W`, no limit) refuses a select on a table larger than it unless a where term can
  run in DuckDB. You get `'limit` before anything is fetched, never a partial answer. `get` is never limited.
- **`.duckdb.pushdown:0b`** turns pushdown off: every select fetches the whole table and runs in q.
- **To see what ran,** read `.duckdb.sqllog`. Below, a float filter fetches the whole table, and with `.duckdb.maxrows`
  set, a select with nothing to push is refused while a symbol filter still runs:

```q
q).duckdb.sqllog::0#.duckdb.sqllog
q)select from `:pq:duckdb:mkt:trade/ where price>200
| sym    | price | size |
| symbol | float | long |
|--------|-------|------|
| MSFT   | 250.2 | 200  |
| MSFT   | 251   | 50   |
q)exec sql from .duckdb.sqllog
"SELECT \"sym\", \"price\", \"size\" FROM \"trade\" ORDER BY i"
q).duckdb.maxrows:3
q)@[{select from x}; `:pq:duckdb:mkt:trade/; {"error: ",x}]
"error: limit"
q).duckdb.err[]
"trade: 6 rows exceed .duckdb.maxrows 3; a where term the translator can push is required"
q)select from `:pq:duckdb:mkt:trade/ where sym=`MSFT
| sym    | price | size |
| symbol | float | long |
|--------|-------|------|
| MSFT   | 250.2 | 200  |
| MSFT   | 251   | 50   |
q).duckdb.maxrows:0W
```

- **Tables created by SQL** (not written from q) carry none of the q type information, so a select on one fetches
  the whole table and runs in q, and its row order is DuckDB's unless the select sorts. `.duckdb.maxrows` does not
  apply to them.

</details>
