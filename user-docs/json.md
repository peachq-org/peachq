# Reading JSON

`.j.read` loads a JSON document as a table. Its option names, defaults and behaviour follow DuckDB's `read_json`;
the types you get back are q's.

Everything on [Loading a file](loading.md) applies here — the source law, the sample, the freeze, `types`, and what
a cell means. This page is JSON's own half: how records are framed, what each root shape becomes, what happens to
nesting, and how to reach inside an envelope.

## Four functions, two of them always there

```q
q).j.k "{\"a\":1}"          / deserialize a string  — always available
a| 1
q).j.j ([]a:1 2)            / serialize             — always available
"[{\"a\":1},{\"a\":2}]"
q)\l pq                     / .j.read and .j.info arrive with the standard library
```

`.j.k` is q's documented JSON converter and is deliberately lossy: **every** number comes back a float, and a ragged
array of objects comes back as a list of dictionaries rather than a table.

`.j.read` is a *reader*, so it reads the **written form** — `1` is a long, `1.0` a float, `true` a boolean — at every
depth, and it owes you a table. That is the whole difference between them:

```q
q).j.k "[{\"a\":1},{\"a\":2}]"
| a     |
| float |
|-------|
| 1     |
| 2     |
q).j.read["[{\"a\":1},{\"a\":2}]";::;::;()!()]
| a    |
| long |
|------|
| 1    |
| 2    |
```

`.j.read` takes four arguments — source, target, types, options. `target` is reserved for a future in-place load and
signals `'nyi` for anything but `::`.

Because the reader reads the form q writes, `.j.j` output round-trips unaided, temporals included:

```q
q).j.j ([]a:1 2;d:2024.01.15 2024.01.16)
"[{\"a\":1,\"d\":\"2024-01-15\"},{\"a\":2,\"d\":\"2024-01-16\"}]"
q).j.read[.j.j ([]a:1 2;d:2024.01.15 2024.01.16);::;::;()!()]
| a    | d          |
| long | date       |
|------|------------|
| 1    | 2024.01.15 |
| 2    | 2024.01.16 |
```

Symbol, char, guid and byte are the exceptions: JSON writes them indistinguishably from strings, so guid and byte
come back by the [written-form grammar](loading.md#the-grammar-is-the-form-q-writes) and a symbol or char column
comes back as text unless `types` says otherwise.

## Framing: how records are delimited

`format` says how records are delimited **in the byte stream**, and it is a separate question from what the document
contains:

| `format` | Means |
|---|---|
| `` `array `` | the file is ONE JSON document |
| `` `newline_delimited `` | the file is a stream of whole values, none of them spanning a line break |
| `` `auto `` (default) | ask whether the whole file is one document, and read it that way if it is |

```q
q).j.read["[{\"a\":1},{\"a\":2}]";::;::;()!()]        / one document
| a    |
| long |
|------|
| 1    |
| 2    |
q).j.read["{\"a\":1}\n{\"a\":2}\n";::;::;()!()]       / JSON Lines — the same table
| a    |
| long |
|------|
| 1    |
| 2    |
```

**Stating a framing the file does not have fails**, and that is the point of stating one:

```q
q).j.read["[{\"a\":1},{\"a\":2}]";::;::;(enlist `format)!enlist `newline_delimited]
'parse
```

An array *is* a frame, so an array at the top of a newline-delimited stream is the array framing mis-stated. In the
other direction, `` `array `` over JSON Lines is `'parse` too. If you never say which framing you have, `` `auto ``
answers both, and a pretty-printed object — one document spanning many lines — as well; `` `auto `` asks whether the
whole file parses as one document, never whether it starts with `[`.

A source carrying **no** document at all is `'parse`, not an empty table. `[]` states a shape and reads as zero
records; whitespace states none, and we do not invent one.

## Root shapes: what the document becomes

Framing having decided where a document ends, its root kind decides what the rows are:

| Root | Result |
|---|---|
| an object | a one-row table of its keys |
| an array of objects | one row per object |
| any other array | a single column named `json`, one row per element (`[]` gives it zero rows) |
| a scalar or `null` | `'type` — neither is a table |
| an object with no keys, alone or in an array (`{}`, `[{}]`) | `'type` — it states no columns, so it is not a table |

```q
q).j.read["{\"a\":1,\"b\":\"x\"}";::;::;()!()]
| a    | b   |
| long |     |
|------|-----|
| 1    | "x" |
q).j.read["[1,2,3]";::;::;()!()]
| json |
| long |
|------|
| 1    |
| 2    |
| 3    |
```

`records` overrides that reading. The default `` `auto `` is the table above; `1b` reads **every** value as a record
and refuses one that is not an object; `0b` never expands and always gives the single `json` column — and never
recurses, so each value arrives exactly as `.j.k` would have built it.

```q
q).j.read["[{\"a\":1},{\"a\":2}]";::;::;(enlist `records)!enlist 0b]
| json     |
|          |
|----------|
| (,`a)!,1 |
| (,`a)!,2 |
```

## Records need not carry the same keys

Records match by **name**, so key order is free, and a record may omit a key another record has. The columns are the
**union** of the keys the sampled records carry, in the order they first appear, and a record that omits one gets
that column's null:

```q
q).j.read["[{\"a\":1},{\"b\":2}]";::;::;()!()]
| a    | b    |
| long | long |
|------|------|
| 1    |      |
|      | 2    |
```

**A missing key and a written `null` are the same thing**, and that is the whole null law rather than a convenience.
Nulls take no part in typing: a column is typed by the values that are *not* null, wherever they sit, and each null
is then written as that type's q null. So an early null in a column whose later values are longs is `0N`, not a
float — no special case needed. A column of nothing but nulls is a float column of `0n`, and a boolean column's null
is `0b`, because q booleans have no null.

A **boolean does not promote into the numbers**. JSON states its types, so `true` beside `1` is a genuine mix rather
than a text ambiguity, and the column keeps the values as written.

The union is over the **sample**, so the schema still freezes: a key first seen beyond `sample_size` is not a column
and signals `'type`. A key repeated *within one record* is `'dup`. See [Bad rows](bad-rows.md) for both, and for the
lever that drops such a record instead of signalling.

## Nesting

A nested object or array loads **recursively**, with the reader's own column inference, so a nested column carries
real types rather than everything arriving as a float. A column of records is a nested **table**:

```q
q)t:.j.read["[{\"a\":1,\"o\":{\"p\":1,\"q\":2}},{\"a\":2,\"o\":{\"p\":3,\"q\":4}}]";::;::;()!()]
q)t
| a    | o        |
| long |          |
|------|----------|
| 1    | `p`q!1 2 |
| 2    | `p`q!3 4 |
q)type t`o
98h
q)t`o
| p    | q    |
| long | long |
|------|------|
| 1    | 2    |
| 3    | 4    |
```

An array of records inside one cell is a sub-table in that cell; a genuine mix of kinds keeps the values as written.
A record that omits a nested key null-fills as that nested column's typed null, exactly as at the top level.

`types` does **not** reach inside a nested column, and `path` addresses the document root rather than each record.
Pull a nested table apart with q instead — `meta`, `key`, `type` and `first` walk a loaded value better than any
schema notation would.

## `path`: reading inside an envelope

The rows you want are often one level down inside an API envelope, where no argument reaches them: in
`{"status":"OK","results":[…]}`, `results` is a *cell*, not a column, so naming it in `types` is `'type`.

`path` selects a value out of **each document, before schema inference**. The selected value *becomes* the top-level
one, so the root-shape law, the key union, the sniff and `types` all then speak about it, and the answer is what
loading that value on its own would give:

```q
q).j.read["{\"status\":\"OK\",\"results\":[{\"t\":1},{\"t\":2}]}";::;::;(enlist `path)!enlist `results]
| t    |
| long |
|------|
| 1    |
| 2    |
```

A path is a **q path vector**, indexed exactly as `.` indexes: symbols pick object keys, longs pick array elements.
So `` `results ``, `` `data`items `` and `` (`data;`tables;0) `` are all paths, and `::` or an empty list is the
whole document. Because a path is a *value*, a path assembled at runtime is just `,`.

Three refusals bound it:

- a step this document **cannot take** — a missing key, an index past the end — is `'domain`. A typo does not
  quietly read as "no schema" and resurface one step later as a shape error.
- a step of the wrong **kind** — a symbol meeting an array, a long meeting an object — is `'type`.
- a path onto a non-table falls through to the ordinary root-shape law above.

Note that the path'd read is not the same value as the nested cell: a cell is built per record, while a table gets
column **inference** across records. A field written `311` in one record and `303.42` in the others is a mixed list
in the cell and a float column in the path'd read.

### The `#` URL fragment

On an `http` or `https` URL you can spell the path in the URL itself, which keeps the URL copy-pastable into curl or
a browser — a fragment is client-side by RFC and the transport drops it before the request:

```q
q).j.read[`$":https://api.example.com/v2/aggs#`results";::;::;()!()]
```

That is sugar for `` path:`results `` and reaches the same mechanism; the two must give the identical table. Stating
both signals `'domain` — two spellings of one thing, disagreeing.

The fragment is **read, never evaluated**, so a URL assembled from untrusted text cannot smuggle q into the call. It
spells symbol steps only (`` #`data`items ``); an array index is spelled by the option, which is a value and needs
no grammar.

It is **URL-only**. `#` is a legal character in a POSIX filename, so in a file path it stays literal:
`` read1 `$":od#d.json" `` reads a file called `od#d.json`, as it always did.

## `.j.info`: the schema without the load

`.j.info` runs the same sample the read runs and answers a dict of column name to type char — in exactly the shape
`.j.read` takes as `types`, so you can look at it, edit it, and feed it straight back:

```q
q).j.info["[{\"a\":1,\"b\":\"x\",\"o\":{\"p\":1}}]";()!()]
a| j
b| *
o| *
```

It is **flat**, one entry per top-level column, because its whole job is to emit something `types` accepts. A nested
column — or any column whose values do not share one q type — answers `"*"`, meaning "as the untyped read builds
it". `path` is honoured here too, so the sniff always describes the document the read would build.

Unlike `.csv.info` it carries no advisory `"s"`, and because it reads only the first `sample_size` records it does
not validate the rest of the document. Everything it *does* read obeys the read's rules, so a document `.j.read`
would refuse is refused here.

## Options

The fourth argument is a dict. **An option is never silently ignored**: an unknown key signals `'option`.

| Option | Type | Default | What it does |
|---|---|---|---|
| `sample_size` | long | 20480 | records read to infer types |
| `format` | symbol | `` `auto `` | how records are framed: `` `array ``, `` `newline_delimited ``, `` `auto `` |
| `records` | boolean or symbol | `` `auto `` | `1b` every value is a record; `0b` never expand, never recurse |
| `path` | symbol / long / list | `::` | the value to read out of each document, before inference |
| `dateformat` | string | off | strptime-subset format that REPLACES the built-in date grammar |
| `timestampformat` | string | off | the same for timestamps, and the only place `%z` is accepted |
| `ignore_errors` | boolean | `0b` | drop a record that cannot be built instead of signalling |
| `store_rejects` | boolean | `0b` | continue AND keep the reject records in a global table |
| `rejects_table` | symbol | `` `reject_errors `` | names that table, and implies `store_rejects` |
| `null_padding` | boolean | `0b` | accepted for `.csv.read` parity; changes nothing, since the key union already null-fills |

The format subset is the one [`.csv.read` validates](csv.md#formats-for-dates-and-timestamps).

Three option names you may have seen elsewhere signal `'option` here and always will: `maximum_depth`,
`map_inference_threshold` and `field_appearance_threshold`. They tune how deeply a nested value is flattened into
columns, and q needs no such setting — a q cell holds a table, so a nested value is kept as one.

## When a load fails

The error classes, the frozen-type miss and its two fixes, and the tolerance levers are on
[Bad rows](bad-rows.md) — one page, shared with the CSV reader.
