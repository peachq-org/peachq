# Reading CSV

`.csv.read` loads delimited text into q — from a file, or from a char vector you already hold. It is an
incremental reader: the source is pulled through a fixed read buffer, so a huge file costs no more memory than a
small one when you send the rows somewhere as they arrive. Column types are inferred from a sample of the first
rows and then **frozen** — a later cell that does not fit its column's type is an error, never a silent null.

This is a peachq extension. Standard q reads CSV with `0:`, which needs you to state every column type up front
and gives no control over quoting, comments or bad rows. `0:` still works exactly as it always did; nothing here
changes it. Use `0:` when you already know the schema and the file is clean, and `.csv.read` when you do not.

## Loading

The `.csv` namespace is part of the standard library:

```q
q)\l pq
```

## The two functions

```q
q).csv.read
{[file;target;types;opts] .csv.i.read[file;target;types;opts]}
q).csv.info
{[file;opts] .csv.i.info[file;opts]}
```

`.csv.read` always takes four arguments. The first is the source — a file symbol, or the CSV text itself (see
the next section). Pass `::` for `target` and `types` when you have nothing to say about them, and `()!()` for an
empty options dict:

```q
q)`:trades.csv 0: ("sym,px,qty,dt";"AAPL,171.4,100,2024-01-15";"MSFT,402.3,250,2024-01-16");
q)t:.csv.read[`:trades.csv;::;::;()!()]
q)t
| sym    | px    | qty  | dt         |
|        | float | long | date       |
|--------|-------|------|------------|
| "AAPL" | 171.4 | 100  | 2024.01.15 |
| "MSFT" | 402.3 | 250  | 2024.01.16 |
q)meta t
| c      | t    | f      | a      |
| symbol | char | symbol | symbol |
|========|------|--------|--------|
| sym    | C    |        |        |
| px     | f    |        |        |
| qty    | j    |        |        |
| dt     | d    |        |        |
```

Note `sym`: **sniffed text is always a string column, never a symbol.** Interning has a cost, and the reader will
not decide to pay it for you — ask with `types` (below) when you want symbols.

## The source: a file, or the text itself

The first argument is the source, and its **type** says which kind it is. There is no sniffing:

> **A symbol is a path. A string is content, never a path.**

So `` `:trades.csv `` always names a file, and `"trades.csv"` is always three fields of CSV text that happen to
spell a filename. The rule is the same one `read0` uses, and the same one `qlib/src/path.q` keeps: a heuristic
here would break every `` `:c:/temp/x.csv ``.

In-memory text is what you want when the CSV never touched the disk — an HTTP or WebSocket body, an IPC payload,
something you built in q. All of those arrive as char vectors, so they go straight in:

```q
q).csv.read["a,b\n1,2\n3,4\n";::;::;()!()]
| a    | b    |
| long | long |
|------|------|
| 1    | 2    |
| 3    | 4    |
q).csv.read["c"$read1 `:trades.csv;::;::;()!()]
| sym    | px    | qty  | dt         |
|        | float | long | date       |
|--------|-------|------|------------|
| "AAPL" | 171.4 | 100  | 2024.01.15 |
| "MSFT" | 402.3 | 250  | 2024.01.16 |
```

Note `"c"$read1` rather than `read0`: `read1` gives you the bytes exactly as they sit on disk, where `read0`
splits on line endings and normalises them. Anything the reader can do with a file it can do with those bytes.

### One blob, or a list of lines

Two text shapes are accepted:

- **one char vector** — the whole payload, embedded newlines and all. This is the honest carrier, because the
  reader's own unit is not a line: an RFC-4180 quoted field may contain newlines and span as many of them as it
  likes.
- **a list of char vectors** — defined as **join with `"\n"`, then parse**. It is *not* one element per record,
  and it must not be: only the join reading survives a quoted field that got split across two elements.

```q
q).csv.read[("a,b";"1,2";"3,4");::;::;()!()]
| a    | b    |
| long | long |
|------|------|
| 1    | 2    |
| 3    | 4    |
q).csv.read[("a,b";"\"x";"y\",2");::;::;()!()]
| a      | b    |
|        | long |
|--------|------|
| "x\ny" | 2    |
```

The second is the discriminating case: three list elements, one header and **one** data row, whose first cell is
the two-line string `"x\ny"`.

Because `csv 0:` already answers a list of lines, a table round-trips through CSV without a file:

```q
q)t:([]a:1 2;b:2024.01.15 2024.01.16)
q)csv 0: t
"a,b"
"1,2024-01-15"
"2,2024-01-16"
q).csv.read[csv 0: t;::;::;()!()]
| a    | b          |
| long | date       |
|------|------------|
| 1    | 2024.01.15 |
| 2    | 2024.01.16 |
```

### What stays the same

Everything below the source. Text is chunked at the same `buffer_size` a file is read at, so `chunks` in the
summary and the `` `chunk`rows `` dict a lambda target receives are identical for the same bytes either way. A
leading UTF-8 BOM is stripped. `skip`, `comment`, `delim`, the type freeze and the reject channel are unchanged,
and a reject record's `line` is the physical line within the payload.

An empty payload — `""` or `()` — signals `'csv`, exactly as a zero-byte file does: a source that states no
schema does not get one invented for it.

`.csv.info` takes the same source forms:

```q
q).csv.info["a,b\n1,2\n";()!()]
a| j
b| j
```

Byte vectors are not a source. `"c"$` them first — `` "c"$read1 `:f `` above is that conversion, and it is
byte-exact.

## Targets: where the rows go

The second argument decides what the rows do. With `::` you get the table back, as above. Anything else makes
`.csv.read` return a **summary dict** instead, because the rows have gone somewhere:

```q
q).csv.read[`:trades.csv;`dest;::;()!()]
rows    | 2
rejected| 0
chunks  | 1
ignored | `symbol$()
types   | `sym`px`qty`dt!"*fjd"
```

- `rows` — rows landed **by this call**, not the target's total
- `rejected` — rows that left a reject record (see [Bad rows](#bad-rows))
- `chunks` — how many read batches the file took
- `ignored` — CSV columns dropped because the existing target table has no such column
- `types` — the per-column type chars actually used, `*` meaning "left as a string"

### A named table

A symbol target names a global table. If it does not exist it is created; if it does, the rows are **inserted**,
and the existing table's schema outranks the sniff — that is how you load ten files into one table and get the
same types every time. A **keyed** table upserts instead of inserting, so re-loading a file updates rows rather
than duplicating them.

```q
q)kt:([sym:`symbol$()]px:`float$();qty:`long$();dt:`date$())
q).csv.read[`:trades.csv;`kt;(enlist `sym)!enlist "s";()!()];
q)kt
| sym    | px    | qty  | dt         |
| symbol | float | long | date       |
|========|-------|------|------------|
| AAPL   | 171.4 | 100  | 2024.01.15 |
| MSFT   | 402.3 | 250  | 2024.01.16 |
```

A headerless file maps to an existing target **positionally**: complete map, no projection. Everything else about
conformity — a missing column, a type that will not convert — is `insert`'s own contract and its own error.

### A lambda, called once per batch

A rank-3 lambda `{[tblData;errData;misc] ...}` is called once per read batch. This is the streaming door: the rows
never all exist at once, so it is how you aggregate, filter or forward a file larger than memory.

```q
q).csv.read[`:trades.csv;{[tblData;errData;misc] -1 "batch ",-3!misc; show tblData};::;()!()];
batch `chunk`rows!0 2
| sym    | px    | qty  | dt         |
|        | float | long | date       |
|--------|-------|------|------------|
| "AAPL" | 171.4 | 100  | 2024.01.15 |
| "MSFT" | 402.3 | 250  | 2024.01.16 |
```

- `tblData` — this batch's rows as a table
- `errData` — this batch's reject records, as a table with columns `line`, `column`, `error`, `csvLine`; the empty
  table, schema intact, on a clean batch
- `misc` — the dict `` `chunk`rows ``: the 0-based batch index, and rows landed so far

A lambda of any other rank is refused with `'rank`. A target that is neither `::`, a symbol, nor a lambda is
`'type`.

## Types: overriding the sniff

The third argument states types by hand. Type chars are drawn from `"bjfsdtpmuvn"` — boolean, long, float,
symbol, date, time, timestamp, month, minute, second, timespan — plus two specials: `"*"` keeps the raw string,
and `" "` **drops that column entirely**, so it is never parsed and never appears in the result.

A **dict** names the columns it cares about and leaves the rest to the sniff:

```q
q).csv.read[`:trades.csv;::;(enlist `sym)!enlist "s";()!()]
| sym    | px    | qty  | dt         |
| symbol | float | long | date       |
|--------|-------|------|------------|
| AAPL   | 171.4 | 100  | 2024.01.15 |
| MSFT   | 402.3 | 250  | 2024.01.16 |
```

A **string** is the complete schema, one char per column, and must cover every column or you get `'length`:

```q
q).csv.read[`:trades.csv;::;"s jd";()!()]
| sym    | qty  | dt         |
| symbol | long | date       |
|--------|------|------------|
| AAPL   | 100  | 2024.01.15 |
| MSFT   | 250  | 2024.01.16 |
```

The blank in position two drops `px`. A dict key naming no column in the file is `'domain`.

An explicit type is a promise, not a hint: a cell that will not parse under it is a frozen-type miss, which
signals `'csv` unless a tolerance option (below) says otherwise.

### `.csv.info`, the sniff on its own

`.csv.info` reads only the sample and answers the schema it inferred, in exactly the shape `.csv.read` takes as
`types` — so you can look, edit, and feed it back:

```q
q).csv.info[`:trades.csv;()!()]
sym| s
px | f
qty| j
dt | d
```

One difference from `.csv.read`: `.csv.info` is **advisory** about text. Where a text column has low cardinality
it says `"s"`, suggesting a symbol, even though `.csv.read` itself never syms a sniffed column — feed the dict
back to adopt the advice. Because it reads only the sample, `.csv.info` does not validate the rest of the file.

## Options

The fourth argument is a dict of options, following DuckDB's `read_csv` vocabulary and its defaults. **An option
is never silently ignored**: an unknown key — or one that is recognised but not yet implemented — signals
`'option`. An empty-symbol key is padding and is skipped, so the short-dict idiom `` ``delim!(::;";") `` works.

| Option | Type | Default | What it does |
|---|---|---|---|
| `delim` | char | sniffed | the field delimiter |
| `header` | boolean | sniffed | force row 0 to be, or not be, the header |
| `quote` | char | `"` | the quote character; `""` disables quoting, making quotes ordinary bytes |
| `escape` | char | the quote char | inside a quoted field the escape consumes its follower |
| `comment` | char | off | a line whose FIRST byte is this is skipped whole |
| `sample_size` | long | 20480 | rows read to infer types, the header included |
| `buffer_size` | long | 1048576 | read-chunk bytes |
| `skip` | long | `0` | records dropped off the front before anything else reads them |
| `dateformat` | string | off | strptime-subset format that REPLACES the built-in date grammar |
| `timestampformat` | string | off | the same for timestamps, and the only place `%z` is accepted |
| `ignore_errors` | boolean | `0b` | skip a bad row instead of aborting |
| `null_padding` | boolean | `0b` | pad a short row with nulls instead of aborting |
| `strict_mode` | boolean | `1b` | `0b` salvages a field whose quotes will not parse |
| `store_rejects` | boolean | `0b` | continue AND keep the reject records in a global table |
| `rejects_table` | symbol | `` `reject_errors `` | names that table, and implies `store_rejects` |

The format subset for `dateformat` / `timestampformat` is `%Y %y %m %d %H %M %S %f %z`. `%Y` matches exactly four
digits; clock specifiers belong to `timestampformat` and are refused in `dateformat`; a format with no year is
refused; anything outside the subset signals `'option`.

`nullstr` and `all_varchar` are recognised names that are **not implemented** — passing either signals
`'option`, like an unknown key. There is no `encoding` option (see [Encoding](#encoding)).

Degenerate combinations are refused up front rather than producing nonsense: the quote character cannot be the
delimiter, a newline cannot be the delimiter, the comment character cannot collide with an explicit delimiter, and
a multi-character `comment` is `'domain`.

## The laws you will actually feel

### `skip` drops a preamble before anything else looks at the file

Plenty of real files open with a title, a note or a run stamp before the data starts. Such a file is unloadable at
any other option setting: the sniffer freezes a schema off the junk and every real row then rejects as
`toomanycolumns`. `skip` drops that many records off the **front**, and everything downstream — the delimiter
sniff, the header sniff, the type sniff — runs on what remains.

```q
q)`:pre.csv 1: "monthly report\nrun 2026-08-24\na,b\n1,2\n3,4\n";
q).csv.read[`:pre.csv;::;::;(enlist `skip)!enlist 2]
| a    | b    |
| long | long |
|------|------|
| 1    | 2    |
| 3    | 4    |
```

Three laws follow from where the counter sits, and they are worth knowing before you count lines by eye:

- A **blank line counts** toward `skip`. Blank lines are file structure, so a preamble that ends in one needs a
  `skip` that includes it.
- A **comment line does not count**. `comment` is a dialect you asked for, and it is transparent everywhere: the
  reader drops those lines before `skip` ever sees them, so the two options compose instead of fighting.
- **Line numbers are physical.** A reject record's `line` is where the row sits in the file; `skip` renumbers
  nothing.

```q
q)`:mix.csv 1: "# generated\nmonthly report\na,b\n1,2\n";
q).csv.read[`:mix.csv;::;::;`comment`skip!("#";1)]
| a    | b    |
| long | long |
|------|------|
| 1    | 2    |
```

A negative `skip` is `'domain` — `0` is the default, not a degenerate value. Skipping past the last record leaves
a stream that states no schema, and that signals `'csv`, exactly as a zero-byte file does.

### The delimiter is sniffed; the dialect is not

Without an explicit `delim` the reader tries `,` `;` tab `|`, in that order. A candidate qualifies only if
**every** sample row parses to the same field count, above one column; most columns wins, and ties keep candidate
order, so comma wins a tie.

```q
q)`:t3.csv 0: ("a;b";"1;2");
q).csv.read[`:t3.csv;::;::;()!()]
| a    | b    |
| long | long |
|------|------|
| 1    | 2    |
```

`quote`, `escape` and `comment` are **never** sniffed. A non-default quote or comment dialect is always something
you state, because guessing it silently changes what a file means:

```q
q)`:c2.csv 1: "a,b\n1,'x,y'\n";
q).csv.read[`:c2.csv;::;::;(enlist `quote)!enlist "'"]
| a    | b     |
| long |       |
|------|-------|
| 1    | "x,y" |
```

If no delimiter candidate qualifies, a **one-column-shaped** file — a strict majority of sample rows with one
field under every candidate, none malformed, no NUL bytes — loads whole lines as a single column:

```q
q)`:s.csv 1: "hello, world\nfoo\nbar baz\n";
q).csv.read[`:s.csv;::;::;()!()]
| hello, world |
|              |
|--------------|
| "foo"        |
| "bar baz"    |
```

Anything else keeps the loud `'csv` refusal, and an explicit `delim` never falls back at all.

### The sniffer reads what `csv 0:` writes, and never guesses beyond it

The type grammar is exactly the set of forms q's own CSV writer produces: dashed dates, dotted-`D` timestamps
(plus the ISO `T`- and space-separated alternates), `m`-suffixed months, `hh:mm` minute, `hh:mm:ss[.f]` second,
and `D`-separated timespan. Clock shapes promote per column up the minute < second < time < timespan lattice.

It never guesses beyond a written form. `20260822` stays a long. `2026.08` stays a float. `1`/`0` stays a long —
ask for booleans and symbols explicitly. A leading-zero number like `030151360` stays text, because the zeros are
data. A comma decimal (`1,53`) stays text, because the comma is a delimiter, not a decimal point.

Most visibly, **day-order forms stay text**. `03/10/2024` and `12-13-2004` are ambiguous as *forms*, so the
sniffer adopts neither reading and the column arrives as strings. One such cell holds the whole column text, even
where every other cell is a clean ISO date:

```q
q)`:d.csv 0: ("a";"2024-03-08";"03/10/2024");
q).csv.read[`:d.csv;::;::;()!()]
| a            |
|              |
|--------------|
| "2024-03-08" |
| "03/10/2024" |
```

There are two doors out. Declare the column `"d"` and the reader parses the slash form using the field order `\z`
selects — `\z 0` month-first, `\z 1` day-first, the same system setting the rest of q obeys. Or state a
`dateformat` and replace the grammar outright.

### Timezone suffixes stay text

kdb parses no timezone, and neither do we. At defaults a tz-suffixed cell — `...+02:00`, `...Z`, `... UTC` — is
**text, bytes intact**. There is no silent shift to local time and no silent discard of the offset:

```q
q)`:tz.csv 0: ("ts";"2026-08-22T12:34:56+02:00";"2026-08-23T00:30:00Z");
q).csv.read[`:tz.csv;::;::;()!()]
| ts                          |
|                             |
|-----------------------------|
| "2026-08-22T12:34:56+02:00" |
| "2026-08-23T00:30:00Z"      |
```

The one door is `%z` in `timestampformat`. Ask for an offset and it is **applied**: the stored timestamp is UTC,
which is DuckDB's own semantics.

```q
q).csv.read[`:tz.csv;::;::;(enlist `timestampformat)!enlist "%Y-%m-%dT%H:%M:%S%z"]
| ts                            |
| timestamp                     |
|-------------------------------|
| 2026.08.22D10:34:56.000000000 |
| 2026.08.23D00:30:00.000000000 |
```

`%z` reads `Z` and `+-HH`, `+-HHMM`, `+-HH:MM`. Zone **names** — `America/New_York`, or the `EST` in
`2021-05-25 04:55:03 EST` — need tz data this build does not carry, so `%Z` is not in the format subset and
signals `'option`. When you need a zone, load the column as text and do the arithmetic in q, where it is visible:

```q
q)`:tzn.csv 0: ("id,ts";"1,2021-05-25 04:55:03.382494 UTC";"2,2021-05-25 04:55:03.382494 EST");
q)t:.csv.read[`:tzn.csv;::;::;()!()]
q)p:" " vs/: t`ts
q)t:update zone:`$last each p, stamp:"P"$" " sv/:2#/:p from t
q)off:`UTC`EST!0D00 -0D05
q)select id, utc:stamp-off zone from t
| id   | utc                           |
| long | timestamp                     |
|------|-------------------------------|
| 1    | 2021.05.25D04:55:03.382494000 |
| 2    | 2021.05.25D09:55:03.382494000 |
```

Under an explicit `"p"` with no format, an offset-bearing cell is a **frozen-type miss**: `'csv`, or a `cast`
reject under a tolerance lever. DuckDB's forced-TIMESTAMP path drops the offset without a word; we refuse instead.

### Headers

Row 0 is the header when it reads like one, and data when it does not: an all-typed first row such as
`10,hello,20` is data, and the columns take generated names `x`, `x1`, `x2`. Force the question either way with
`header`.

Duplicate header names dedupe with `_1`, `_2`; a blank header cell takes a generated name; a UTF-8 BOM is
stripped. A file with a header and no data rows loads as a **zero-row table with the header's columns** — a
schema, correctly. A **zero-byte** file signals `'csv`: a file that states no schema does not get one invented.
(DuckDB invents a one-column VARCHAR schema from nothing; we will not.)

### Rows, lines and empty fields

`CR`, `LF` and `CRLF` all end a row, and may be mixed within one file. A quoted field may contain the delimiter,
the quote — doubled, or escaped — and newlines: an RFC-4180 quoted newline is one row, including when the quoted
region straddles a read-chunk boundary. A **blank physical line is skipped**, where DuckDB emits a row of nulls.
Comment lines are skipped whole and never counted or rejected, though they still advance the physical line numbers
you see in reject records.

An **empty field is the column's null**: `0N` under a long column, `""` under a text one. There is no
null-string vocabulary — `NA`, `NULL` and `\N` are ordinary text — and no `nullstr` option to declare one yet.

```q
q)`:e.csv 0: ("a,b,c";"1,,x";"2,,y");
q).csv.read[`:e.csv;::;::;()!()]
| a    | b  | c   |
| long |    |     |
|------|----|-----|
| 1    | "" | "x" |
| 2    | "" | "y" |
```

### Encoding

Bytes are preserved. q strings are byte vectors, so UTF-8, Latin-1, Shift-JIS and even invalid-UTF-8 payloads all
round-trip through the reader unchanged, in the data and in header names alike. Nothing is transcoded and nothing
is validated — there is no encoding option, and DuckDB rejects some files this reader loads happily. A UTF-16 file
is not decoded; it reads as bytes, which is almost certainly not what you want.

## Bad rows

The default posture is strict, like DuckDB's: a ragged row, a malformed quote, or a cell that misses its frozen
type aborts the load with `'csv`. The tolerance options govern whether the load **survives** a bad row — never
whether it is counted. Every bad row leaves a reject record and bumps `rejected`, whichever lever let it through.

```q
q)`:t2.csv 0: ("sym,px,qty";"AAPL,171.4,100";"IBM,189.2");
q).csv.read[`:t2.csv;::;::;()!()]
'csv
  [1]  {[file;target;types;opts] .csv.i.read[file;target;types;opts]}
q)).csv.read[`:t2.csv;`h;::;`ignore_errors`rejects_table!(1b;`rej)]
rows    | 1
rejected| 1
chunks  | 1
ignored | `symbol$()
types   | `sym`px`qty!"*fj"
q))rej
| line | column | error         | csvLine     |
| long | symbol | symbol        |             |
|------|--------|---------------|-------------|
| 3    |        | toofewcolumns | "IBM,189.2" |
```

(The `q))` prompt is the error handler the failed load left you in; `\` returns to `q)`.)

The reject table has four columns: `line`, the physical line number with comments and blank lines included;
`column`, the column that failed where one column is to blame; `error`; and `csvLine`, the raw text. The error
classes are `cast`, `toofewcolumns`, `toomanycolumns`, `unquotedvalue`, `unterminatedquote` and `padded`.
`store_rejects` puts the same records in a global table — `` `reject_errors `` by default, or whatever
`rejects_table` names — replaced on each load. With a lambda target the records arrive per batch as `errData`.

The levers, and what each survives:

- `ignore_errors` — skip the bad row entirely
- `null_padding` — pad a **short** row with nulls and keep it; the padded row is still recorded. It never pads a
  long row: too many fields stays an error
- `strict_mode:0b` — read a field whose quotes will not parse as quotes as literal bytes, up to the next delimiter

## Divergences from kdb+ `0:` and from DuckDB

Against kdb+ `0:`:

- `0:` requires the full type string and column count. `.csv.read` infers them, and takes the same type chars when
  you do want to state them.
- `0:` reads the whole file into memory. `.csv.read` streams, and can hand you the rows batch by batch.
- `0:` has no quoting dialect, no comment handling, no reject records, and no options at all.
- `0:` gives you a symbol column when you ask for `S`. Sniffed text here is always a string; symbols are explicit.

Against DuckDB `read_csv`, with the same options set:

- The types are q's, not SQL's: minute, second and timespan are separate types where DuckDB has one `TIME`, and
  there is no `TIMESTAMPTZ` — which is why tz-suffixed cells stay text.
- We refuse where DuckDB is lenient: unbalanced quotes and mid-field quotes are `'csv` here, where DuckDB
  sometimes truncates the value and carries on.
- We load where DuckDB refuses: invalid UTF-8 and Latin-1 payloads pass through as bytes.
- A blank line is skipped here and is a row of nulls in DuckDB. A zero-byte file is `'csv` here and an invented
  one-column schema in DuckDB.
- `skip` counts **records**, so a quoted multi-line record inside the skipped prefix counts once. DuckDB's skip
  runs before its tokenizer and slices that record's physical lines, leaving a fragment as the first cell. One
  carry scan cannot hold two contradictory notions of a line — the whole point of that scan being that a quoted
  newline is not a row boundary — so we count records and DuckDB counts lines.
- Quote, escape and comment dialects are never sniffed here; DuckDB's sniffer will try them.

None of that is guesswork. `qlib/test/csvDiffTest.q` loads a corpus of some 500 files through both engines and
compares them cell by cell, and every difference it still carries is one of the named classes above. Files whose
divergence is not one of those classes are not compared through both engines: they are pinned against
peachq alone at the end of `qlib/test/csvTest.q`, or they are outside the corpus for the reasons in the next
section.

## Undefined in this release

Deliberately unspecified for a first release — do not build on today's behaviour here:

- **Encodings beyond byte-transparent.** No encoding option; UTF-16 input is not decoded.
- **Compressed input.** `.csv.gz` and friends are not fed through the reader.
- **Zone names.** `%Z`, and any tzdata-backed conversion.
- **`nullstr`, `all_varchar`.** Named and refused, not implemented.
- **Non-comma delimiter files with junk lines.** Whether such a file refuses or falls back to a single column is
  not guaranteed either way today.
- **Files carrying NUL bytes, and single-quote or backslash-escape dialects at defaults.** Some load, some refuse;
  the line between them is not a promise yet.

## Errors

- `'csv` — a malformed file, a frozen-type miss, a zero-byte file, or a ragged row with no tolerance lever set
- `'option` — an unknown option key, a recognised-but-unimplemented one, or an unknown format specifier
- `'domain` — an option value out of range (a two-character comment, a delimiter colliding with the quote), or a
  `types` dict key naming no column
- `'type` — a source that is neither a file symbol nor text, an option value of the wrong type, or a target that
  is neither `::`, a symbol, nor a lambda
- `'length` — a `types` string that does not cover every column
- `'rank` — a lambda target that is not rank 3
