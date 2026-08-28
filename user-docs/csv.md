# Reading CSV

`.csv.read` loads delimited text into q — from a file, a URL, or a char vector you already hold. Its option names,
defaults and behaviour follow DuckDB's `read_csv`; the types you get back are q's.

It is an **incremental** reader: the source is pulled through a fixed read buffer, so a huge file costs no more
memory than a small one when you send the rows somewhere as they arrive.

Read [Loading a file](loading.md) first — the source law, the sample, the type freeze, `types` and what a cell means
are shared with the JSON reader and are not repeated here. This page is CSV's own half: delimiters, quoting
dialects, headers, `skip`, and where the rows go.

Standard q reads CSV with `0:`, which needs you to state every column type up front and gives no control over
quoting, comments or bad rows. `0:` still works exactly as it always did — use it when you already know the schema
and the file is clean, and `.csv.read` when you do not. See [Compatibility with kx q](compatibility.md).

## The two functions

The `.csv` namespace is part of the standard library:

```q
q)\l pq
q).csv.read
{[file;target;types;opts] .csv.i.read[file;target;types;opts]}
q).csv.info
{[file;opts] .csv.i.info[file;opts]}
```

`.csv.read` always takes four arguments. Pass `::` for `target` and `types` when you have nothing to say about them,
and `()!()` for an empty options dict:

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

Note `sym`: sniffed text is always a string column, never a symbol. Ask with `types` when you want symbols.

## Text sources

A symbol names a resource; text is content. Both go in the first argument, and
[Loading a file](loading.md#1-pick-a-source) states the law. What is CSV's own is what happens to a **list** of char
vectors, which is defined as **join with `"\n"`, then parse** — not one element per record:

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

The second is the discriminating case: three list elements, one header and **one** data row, whose first cell is the
two-line string `"x\ny"`.

Because `csv 0:` already answers a list of lines, a table round-trips through CSV without touching a file:

```q
q)t:([]a:1 2;b:2024.01.15 2024.01.16)
q).csv.read[csv 0: t;::;::;()!()]
| a    | b          |
| long | date       |
|------|------------|
| 1    | 2024.01.15 |
| 2    | 2024.01.16 |
```

Everything below the source is the same either way. Text is chunked at the same `buffer_size` a file is read at, so
`chunks` in the summary and the `` `chunk`rows `` dict a lambda target receives are identical for the same bytes. A
leading UTF-8 BOM is stripped. `skip`, `comment`, `delim`, the type freeze and the reject channel are unchanged, and
a reject record's `line` is the physical line within the payload.

An empty payload — `""` or `()` — signals `'csv`, exactly as a zero-byte file does: a source that states no schema
does not get one invented for it.

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
- `rejected` — rows that left a reject record (see [Bad rows](bad-rows.md))
- `chunks` — how many read batches the file took
- `ignored` — CSV columns dropped because the existing target table has no such column
- `types` — the per-column type chars actually used, `*` meaning "left as a string"

### A named table

A symbol target names a global table. If it does not exist it is created; if it does, the rows are **inserted**, and
the existing table's schema outranks the sniff — that is how you load ten files into one table and get the same
types every time. A **keyed** table upserts instead of inserting, so re-loading a file updates rows rather than
duplicating them.

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

An explicit `types` that disagrees with the target signals `'mismatch` early rather than part way through the load.
A headerless file maps to an existing target **positionally**: complete map, no projection. Everything else about
conformity — a missing column, a type that will not convert — is `insert`'s own contract and its own error.

The table exists from the moment the schema is decided, not from the first row: a header-only file gives you a
zero-row table with the right columns. And a load that fails part way through leaves what it had already inserted —
`insert` is not transactional, and the reader does not pretend otherwise.

When the file's column names are not your table's, rename them on the way in with the `xcol` option, which happens
**before** the target is consulted:

```q
q)t:([]ticker:0#`;px:0#0f)
q).csv.read["sym,price\nAAPL,1.5\nMSFT,2.5\n";`t;::;(enlist `xcol)!enlist `sym`price!`ticker`px]
rows    | 2
rejected| 0
chunks  | 1
ignored | `symbol$()
types   | `ticker`px!"sf"
```

[Loading a file](loading.md#5-you-rename-with-xcol) has the rest: what `xcol` takes, and why `types` still keys on
the file's own names.

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

## Stating types

The third argument overrides the sniff. The type chars and the rules that govern them are on
[Loading a file](loading.md#4-you-override-with-types); the two shapes they come in are CSV's business here.

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

One difference from `.csv.read`: `.csv.info` is **advisory** about text. Where a text column has low cardinality it
says `"s"`, suggesting a symbol, even though `.csv.read` itself never syms a sniffed column — feed the dict back to
adopt the advice. Because it reads only the sample, `.csv.info` does not validate the rest of the file.

## Options

The fourth argument is a dict of options. **An option is never silently ignored**: an unknown key — or one that is
recognised but not yet implemented — signals `'option`. An empty-symbol key is padding and is skipped, so the
short-dict idiom `` ``delim!(::;";") `` works.

| Option | Type | Default | What it does |
|---|---|---|---|
| `xcol` | symbol vector or dict | off | renames columns on the way in, exactly as the `xcol` verb does |
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

`nullstr` and `all_varchar` are recognised names that are **not implemented** — passing either signals `'option`,
like an unknown key. There is no `encoding` option; the reader is byte-transparent, and
[Loading a file](loading.md#encoding) says what that means.

Degenerate combinations are refused up front rather than producing nonsense: the quote character cannot be the
delimiter, a newline cannot be the delimiter, the comment character cannot collide with an explicit delimiter, and a
multi-character `comment` is `'domain`.

The last five options are the tolerance levers, and [Bad rows](bad-rows.md) is their page.

### Formats for dates and timestamps

`dateformat` and `timestampformat` take a strptime subset — `%Y %y %m %d %H %M %S %f %z` — and an explicit format
**replaces** the built-in grammar for its type outright, `\z`'s day-order reading included. `%Y` matches exactly
four digits; clock specifiers belong to `timestampformat` and are refused in `dateformat`; a format with no year is
refused; anything outside the subset signals `'option`.

`%z` is the one door onto a timezone offset, and it applies what it reads: the stored timestamp is UTC. It accepts
`Z` and `+-HH`, `+-HHMM`, `+-HH:MM`.

```q
q)`:tz.csv 0: ("ts";"2026-08-22T12:34:56+02:00";"2026-08-23T00:30:00Z");
q).csv.read[`:tz.csv;::;::;(enlist `timestampformat)!enlist "%Y-%m-%dT%H:%M:%S%z"]
| ts                            |
| timestamp                     |
|-------------------------------|
| 2026.08.22D10:34:56.000000000 |
| 2026.08.23D00:30:00.000000000 |
```

Without that option a tz-suffixed cell stays text, bytes intact, and zone **names** are not readable at all —
[Loading a file](loading.md#timezone-suffixes-stay-text) covers the posture and the workaround.

## The delimiter is sniffed; the dialect is not

Without an explicit `delim` the reader tries `,` `;` tab `|`, in that order. A candidate qualifies only if **every**
sample row parses to the same field count, above one column; most columns wins, and ties keep candidate order, so
comma wins a tie.

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

If no delimiter candidate qualifies, a **one-column-shaped** file — a strict majority of sample rows with one field
under every candidate, none malformed, no NUL bytes — loads whole lines as a single column:

```q
q)`:s.csv 1: "hello, world\nfoo\nbar baz\n";
q).csv.read[`:s.csv;::;::;()!()]
| hello, world |
|              |
|--------------|
| "foo"        |
| "bar baz"    |
```

Anything else is refused with `'csv`, and an explicit `delim` never falls back at all.

## `skip` drops a preamble before anything else looks at the file

Plenty of real files open with a title, a note or a run stamp before the data starts. Such a file is unloadable at
any other option setting: the sniffer freezes a schema off the junk and every real row then rejects as
`toomanycolumns`. `skip` drops that many records off the **front**, and everything downstream — the delimiter sniff,
the header sniff, the type sniff — runs on what remains.

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

- **`skip` counts records, not physical lines.** A quoted multi-line record in the skipped prefix counts **once**,
  so counting the file's lines by eye will mislead you wherever such a record sits in the preamble. This is the one
  place the reader parts company with DuckDB's `read_csv`, whose `skip` slices physical lines.
- A **blank line counts** toward `skip`. Blank lines are file structure, so a preamble that ends in one needs a
  `skip` that includes it.
- A **comment line does not count**. `comment` is a dialect you asked for, and it is transparent everywhere: the
  reader drops those lines before `skip` ever sees them, so the two options compose instead of fighting.

```q
q)`:mix.csv 1: "# generated\nmonthly report\na,b\n1,2\n";
q).csv.read[`:mix.csv;::;::;`comment`skip!("#";1)]
| a    | b    |
| long | long |
|------|------|
| 1    | 2    |
```

Line numbers in reject records stay **physical**: `skip` renumbers nothing.

A negative `skip` is `'domain` — `0` is the default, not a degenerate value. Skipping past the last record leaves a
stream that states no schema, and that signals `'csv`, exactly as a zero-byte file does.

## Headers

Row 0 is the header when it reads like one, and data when it does not: an all-typed first row such as
`10,hello,20` is data, and the columns take generated names `x`, `x1`, `x2`. Force the question either way with
`header`.

Duplicate header names dedupe with `_1`, `_2`; a blank header cell takes a generated name; a UTF-8 BOM is stripped.
A file with a header and no data rows loads as a **zero-row table with the header's columns** — a schema, correctly.
A **zero-byte** file signals `'csv`: a file that states no schema does not get one invented.

## Rows, lines and empty fields

`CR`, `LF` and `CRLF` all end a row, and may be mixed within one file. A quoted field may contain the delimiter, the
quote — doubled, or escaped — and newlines: an RFC-4180 quoted newline is one row, including when the quoted region
straddles a read-chunk boundary. A quote the dialect cannot account for is an error rather than a truncated value,
unless `strict_mode:0b` says to salvage it.

A **blank physical line is skipped**. Comment lines are skipped whole and never counted or rejected, though they
still advance the physical line numbers you see in reject records.

An **empty field is the column's null**: `0N` under a long column, `""` under a text one. There is no null-string
vocabulary — `NA`, `NULL` and `\N` are ordinary text — and no `nullstr` option to declare one yet.

```q
q)`:e.csv 0: ("a,b,c";"1,,x";"2,,y");
q).csv.read[`:e.csv;::;::;()!()]
| a    | b  | c   |
| long |    |     |
|------|----|-----|
| 1    | "" | "x" |
| 2    | "" | "y" |
```

## When a load fails

The error classes, the frozen-type miss and its two fixes, and the tolerance levers are on
[Bad rows](bad-rows.md) — one page, shared with the JSON reader.

## Undefined in this release

Deliberately unspecified — do not build on today's behaviour here:

- **Encodings beyond byte-transparent.** No encoding option; UTF-16 input is not decoded.
- **Compressed input.** `.csv.gz` and friends are not fed through the reader.
- **Zone names.** `%Z`, and any timezone-data-backed conversion.
- **`nullstr`, `all_varchar`.** Named and refused, not implemented.
- **Non-comma delimiter files with junk lines.** Whether such a file refuses or falls back to a single column is not
  guaranteed either way today.
- **Files carrying NUL bytes, and single-quote or backslash-escape dialects at defaults.** Some load, some refuse;
  the line between them is not a promise yet.
