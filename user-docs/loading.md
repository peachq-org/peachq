# Loading a file

The option names, defaults and behaviour follow DuckDB's `read_csv` and `read_json`; the types you get back are q's.

peachq ships two readers, both part of the standard library:

```q
q)\l pq
```

| Reader | Reads | Guide |
|---|---|---|
| `.csv.read`, `.csv.info` | delimited text | [Reading CSV](csv.md) |
| `.j.read`, `.j.info` | JSON | [Reading JSON](json.md) |

The two readers take the same four arguments in the same order — source, target, types, options — and the two `info`
functions take the source and the options. Beyond the file format itself they answer the same way, and that shared
half is this page. Per-option detail lives on the two format pages; what to do when a load fails lives on
[Bad rows](bad-rows.md).

## The mental model

Five beats, in order.

### 1. Pick a source

The first argument is the thing to read, and its **type** decides what it is. There is no sniffing:

> **A symbol is a resource. Text is content, never a path.**

So `` `:trades.csv `` always names something to open, and `"trades.csv"` is always three fields of CSV text that
happen to spell a filename. A heuristic here would break every `` `:c:/temp/x.csv ``.

- **a symbol** — a local file, or a URL: `` `:trades.csv ``, `` `$":https://example.com/data.csv" ``. See
  [Handles and resources](handles.md) for what a `` `: `` symbol can name.
- **one char vector** — the whole payload, embedded newlines and all. This is what you want when the bytes never
  touched a disk: an HTTP or WebSocket body, an IPC payload, something you built in q.
- **a list of char vectors** — defined as **join with `"\n"`, then parse**, so `read0` output goes straight in. It is
  *not* one element per record, and it must not be: only the join reading survives a quoted field that got split
  across two elements.

Byte vectors are not a source. `"c"$` them first — `` "c"$read1 `:f `` is that conversion, and it is byte-exact.
Prefer it over `read0` when you want the bytes exactly as they sit on disk, line endings included. A leading UTF-8
byte-order mark is stripped either way.

### 2. A schema is inferred from a sample

The reader looks at the first `sample_size` records — 20480 by default — and decides one type per column.

Two things it will never decide for you. **Sniffed text is always a string column, never a symbol**: interning has a
cost and the reader will not pay it on your behalf. And **a written `1` or `0` is a long, not a boolean**. Both are
available by asking; neither is guessed.

`.csv.info` and `.j.info` run that sample and answer the schema, without loading the rest of the file:

```q
q).csv.info["a,b\n1,2\n";()!()]
a| j
b| j
```

### 3. The schema freezes

After the sample, the types are fixed. A later cell that does not fit its column's type is an **error, never a silent
null and never a mid-load re-promotion** — a type that can change halfway through a file is not a schema.

That is the single most common way a load fails, and it has two fixes: read further before deciding (`sample_size`),
or stop guessing and say what you want (`types`). [Bad rows](bad-rows.md) works through it.

### 4. You override with `types`

The third argument states types by hand and outranks the sniff. Two shapes:

- **a dict** — names the columns it cares about and leaves the rest to the sniff: `` (enlist `sym)!enlist "s" ``.
- **a string** — the complete schema, one char per column, covering every column or `'length`.

The type chars are the **18 basic types**, `"bgxhijefcspmdznuvt"` — boolean, guid, byte, short, int, long, real,
float, char, symbol, timestamp, month, date, datetime, timespan, minute, second, time — plus two specials: `"*"`
keeps the value as the untyped read builds it, and `" "` **drops that column entirely**. Anything else is `'type`.

Five of those chars are reachable only by declaring them, because the sniffer never claims them: `h`, `i` and `e` are
narrower widths of a written number, `c` is a char, and `z` is the legacy datetime. Three carry a rule worth knowing:

- **`c` is exactly one character.** One cell is one atom, so `a` reads as the char `"a"` and `ab` is a miss — two
  characters would be a char *vector* in a single cell. An empty cell is the char null `" "`. A column of text is
  what `*` is for.
- **`z` is `p`'s cell, read out as a datetime.** It accepts what `p` accepts and refuses what `p` refuses. The
  *sniffer* still types a `T`-separated token as `p`: a declared type beats a sniffed one.
- **`h` and `i` inherit the sentinel rule** (below) at their own width.

An explicit type is a promise, not a hint. A cell that will not parse under it is a frozen-type miss.

### 5. A row that does not fit stops the load, or goes to the reject channel

The default posture is strict: a bad row aborts. Tolerance is opt-in, per option, and **always counted** — a lever
governs whether the load survives a bad row, never whether it is recorded. See [Bad rows](bad-rows.md).

## What a cell means is decided once

The two readers share one parser for cell meaning, so **a JSON string and a CSV cell holding the same bytes read the
same**:

```q
q).csv.read["c\n2011-01-01\n";::;::;()!()]
| c          |
| date       |
|------------|
| 2011.01.01 |
q).j.read["{\"c\":\"2011-01-01\"}";::;::;()!()]
| c          |
| date       |
|------------|
| 2011.01.01 |
```

Everything in this section holds for both. The one thing JSON adds is that quoting is type information a CSV file
cannot carry, and it is respected: a quoted number or boolean — `"1"`, `"true"` — stays text, because the document
said it was a string. [Reading JSON](json.md) has the detail.

### The grammar is the form q writes

The reader reads back what q prints, and never guesses beyond a written form:

| Written | Type |
|---|---|
| `2011-01-01` | date |
| `2011-01m` | month |
| `2011-01-01D10:57:42.000000000`, and the ISO `T`- and space-separated alternates | timestamp |
| `10:57`, `10:57:42`, `10:57:42.123` | minute, second, time |
| `0D10:57:42.000000000` | timespan |
| a canonical 36-character uuid | guid |
| `0x` followed by exactly two hex digits | byte |

Beyond that it stops. `20260822` stays a long. `2026.08` stays a float. A leading-zero number like `030151360` stays
text, because the zeros are data. A comma decimal (`1,53`) stays text, because the comma is a delimiter. Longer hex
than one byte stays text, because `0xFF0000` is a byte *vector* and one cell holds one atom; **bare** hex stays text
too, because `0a` is far likelier an identifier than a number.

Because the grammar is what q writes, a table you wrote out loads back unaided — `.j.j`'s guids, bytes and temporals
included.

### Day-order forms follow `\z`

`03/10/2024` and `12-13-2004` are ambiguous as *forms*, but a global `\z` is a **declaration** of the field order —
`\z 0` month-first, `\z 1` day-first, the same setting `"D"$` obeys. So the slash and dash four-digit-year pairs, with
or without a seconds clock, sniff under whichever order you have declared:

```q
q)`:d.csv 0: ("a";"2024-03-08";"03/10/2024");
q).csv.read[`:d.csv;::;::;()!()]
| a          |
| date       |
|------------|
| 2024.03.08 |
| 2024.03.10 |
```

The same file answers differently under `\z 1`, which reads that second row as October 3. A sniffed day-order column
is therefore environment-dependent, exactly as `"D"$` already is.

A value the declared order cannot read (`30-04-2024` under `\z 0`) sniffs **text**, which is what keeps a mixed column
safe. A shape no declaration disambiguates stays text under either order: two-digit years (`12/17/23`), single-digit
fields (`1/1/2020`), and the dotted pair (`10.20.2024`). A stated `dateformat` replaces the grammar outright.

### A digit run that lands on a sentinel is refused

`0W`, `-0W` and `0N` are q's infinity and null. A digit run spelling one of them exactly would arrive as infinity or
null rather than as the number it wrote, which is a silently wrong value — so it is refused instead. In the sample it
demotes the column to text; after the freeze it is an ordinary miss.

The rule is per **width**, so `h` and `i` refuse their own sentinel runs the way `j` refuses the long's: `32767`
under `h` is a refusal and `32766` loads. `e` and `f` are on the float lane, where a *written* `0w` is a legitimate
form and reads as infinity — the rule is about a digit run that lands on a sentinel, not about a value that is one.

### Timezone suffixes stay text

q parses no timezone. At defaults a tz-suffixed cell — `...+02:00`, `...Z`, `... UTC` — is **text, bytes intact**.
There is no silent shift to local time and no silent discard of the offset:

```q
q)`:tz.csv 0: ("ts";"2026-08-22T12:34:56+02:00";"2026-08-23T00:30:00Z");
q).csv.read[`:tz.csv;::;::;()!()]
| ts                          |
|                             |
|-----------------------------|
| "2026-08-22T12:34:56+02:00" |
| "2026-08-23T00:30:00Z"      |
```

The one door onto an offset is `%z` in `timestampformat`. Ask for it and the offset is **applied**: the stored
timestamp is UTC. Both readers take that option; [Reading CSV](csv.md) works the example.

Under an explicit `"p"` with no format, a **zero**-offset tail — `Z`, `z`, `+00:00` — parses, because the digits
already are the UTC instant and nothing shifts, and so does minute resolution (`2025-10-06T10:57`). A **non-zero**
offset is a frozen-type miss, and so are `-00:00` (RFC 3339 spells that "offset unknown", which is not the same
claim as UTC) and compact ISO.

Zone **names** — `America/New_York`, or the `EST` in `2021-05-25 04:55:03 EST` — are not read at all: peachq carries
no timezone database, so `%Z` is not a recognised format specifier and signals `'option`. When you need a zone, load
the column as text and do the arithmetic in q, where it is visible:

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

### The loaders and the cast are different vocabularies

`"D"$`, `"P"$` and the rest are q's cast, whose domain is what the cast is documented to accept. The loaders read
what q **writes**. The two overlap but neither contains the other, and the differences are deliberate:

| | The loaders | `"…"$` |
|---|---|---|
| `2025-10-06T10:57:42Z` under `p` | a timestamp | `0Np` |
| `0x0a` under `x` | the byte `0x0a` | `0x00` — the cast reads *bare* hex, so the prefix consumes an invalid pair |
| `T` under `b` | text | `1b` |

### Encoding

Bytes are preserved. q strings are byte vectors, so UTF-8, Latin-1, Shift-JIS and even invalid-UTF-8 payloads all
round-trip through the readers unchanged, in the data and in the column names alike. Nothing is transcoded and
nothing is validated; there is no encoding option. A UTF-16 file is not decoded — it reads as bytes, which is
almost certainly not what you want.

## Where to go next

- [Reading CSV](csv.md) — delimiters, quoting dialects, headers, `skip`, streaming targets.
- [Reading JSON](json.md) — framing, root shapes, nesting, `path`.
- [Bad rows](bad-rows.md) — the error classes, the frozen-type miss, and the tolerance levers.
