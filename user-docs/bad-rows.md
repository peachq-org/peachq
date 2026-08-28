# Bad rows

Your load failed. This page says why, and what to do about it. It covers both readers — [`.csv.read`](csv.md) and
[`.j.read`](json.md) — because they answer the same way.

The default posture is strict: **a bad row aborts the load**. Tolerance is opt-in, one option at a time, and the
levers carry DuckDB's own names and defaults.

## The one law

> A lever governs whether the load **survives** a bad row, never whether it is **counted**.

Whichever lever let a bad row through, it still leaves a reject record and still bumps the summary's `rejected`.
No lever quietly stops counting, and none silently repairs a row without saying so.

With no lever set the load aborts instead, so there is no summary and no stored reject table — the error is the
whole report. The reject record is what you get when you have asked the load to continue.

## The error classes

| Class | Reader | Means |
|---|---|---|
| `'csv` | CSV | a malformed file, a frozen-type miss, a zero-byte file, or a ragged row with no lever set |
| `'parse` | JSON | a malformed document, a source carrying no document, or a framing the file does not have |
| `'type` | both | an argument of the wrong kind, and in JSON also a document that is not a table shape (see below) |
| `'option` | both | an unknown option key, a recognised-but-unimplemented one, or an unknown format specifier |
| `'domain` | both | an option value out of range, or a name that addresses nothing (see below) |
| `'length` | both | a `types` **string** that does not cover every column |
| `'rank` | CSV | a lambda target that is not rank 3 |
| `'dup` | JSON | a key repeated within one record |
| `'nyi` | JSON | a `target` other than `::` |

An error leaves you in q's error handler, at the `q))` prompt; `\` returns to `q)`.

`'type` covers a source that is neither a resource symbol nor text, an option value of the wrong type, a `types`
char outside the roster, and a CSV target that is not `::`, a symbol or a lambda. In JSON it also covers a document
that is not a table shape, a key beyond the frozen schema, a frozen-type miss, and a `path` step of the wrong kind
— a symbol meeting an array, or a long meeting an object.

`'domain` covers a two-character `comment`, a delimiter colliding with the quote, a negative `skip`, a `types` dict
key naming no column, and a `path` step this document cannot take.

## The frozen-type miss

This is the common one, so it is worth recognising on sight. The reader infers types from the first `sample_size`
records and then **freezes** them. A later cell that does not fit its column's frozen type is an error — never a
silent null, and never a mid-load promotion of the column to something wider, because a type that can change
halfway through a file is not a schema.

```q
q)`:f.csv 1: "a\n1\n2\nx\n";
q).csv.read[`:f.csv;::;::;(enlist `sample_size)!enlist 2]
'csv
```

The sample saw `1`, decided `long`, and then met `x`. There are two fixes, and which one is right depends on
whether the sample was unrepresentative or the file is genuinely mixed.

**Read further before deciding.** If the file really is one type and the sample just missed it, raise `sample_size`:

```q
q).csv.read[`:f.csv;::;::;(enlist `sample_size)!enlist 100]
| a   |
|     |
|-----|
| "1" |
| "2" |
| "x" |
```

**Or stop guessing and say what you want.** Naming the column in `types` replaces the inference for it outright, and
does so without reading any further:

```q
q).csv.read[`:f.csv;::;(enlist `a)!enlist "*";()!()]
| a   |
|     |
|-----|
| "1" |
| "2" |
| "x" |
```

`"*"` is the useful answer when a column is genuinely heterogeneous: it keeps the raw text and lets you sort it out
in q. Remember that an explicit type is a promise, not a hint — a cell that will not parse under it is a frozen-type
miss in its own right.

In JSON the same law also covers **keys**: the columns are the union of the keys the *sampled* records carry, so a
key first seen after the sample is not a column, and signals `'type`.

## The tolerance levers

All default off. Each one lets a specific kind of bad row through; none of them suppresses the record.

| Option | Reader | Survives |
|---|---|---|
| `ignore_errors` | both | skip the bad row (CSV), or drop the record (JSON — a record lands whole or not at all) |
| `null_padding` | CSV | pad a **short** row with nulls and keep it. It never pads a long row: too many fields stays an error |
| `strict_mode:0b` | CSV | read a field whose quotes will not parse as literal bytes, up to the next delimiter |
| `store_rejects` | both | continue, **and** keep the reject records in a global table |
| `rejects_table` | both | name that table (default `` `reject_errors ``), and imply `store_rejects` |

`null_padding` and `strict_mode` are **salvage** levers, not row-skip levers: they repair one specific defect and
keep the row. A cast miss under padding alone still aborts.

Accepted for parity and doing nothing: `null_padding` on the JSON side, where the key-union law already null-fills
an omitted key.

## The reject record

A count alone does not make a bad file diagnosable, so every rejected row leaves a row of its own — line, column,
class and the raw text. It is an audit trail, not an error count.

```q
q).csv.read[`:f.csv;::;::;`sample_size`ignore_errors`store_rejects!(2;1b;1b)]
| a    |
| long |
|------|
| 1    |
| 2    |
q)reject_errors
| line | column | error  | csvLine |
| long | symbol | symbol |         |
|------|--------|--------|---------|
| 4    | a      | cast   | "x"     |
```

The JSON reader fills the same table, with `record` — the offending record, re-serialized — in place of `csvLine`:

```q
q).j.read["[{\"a\":1},{\"a\":\"x\"}]";::;::;`sample_size`ignore_errors`store_rejects!(1;1b;1b)]
| a    |
| long |
|------|
| 1    |
q)reject_errors
| line | column | error  | record          |
| long | symbol | symbol |                 |
|------|--------|--------|-----------------|
| 2    | a      | cast   | "{\"a\":\"x\"}" |
```

The columns:

- `line` — where the row sits. In CSV that is the **physical** line, comments and blank lines included: `skip`
  renumbers nothing. In JSON it is the physical line under newline-delimited framing, and the record's 1-based
  ordinal under array framing.
- `column` — the column to blame, where one column is to blame.
- `error` — the class, from the table below.
- `csvLine` / `record` — the raw text of what was rejected.

The stored table is **one load's audit**: it is replaced on every load, and is empty after a clean one.

| Reject class | Reader | Raised by |
|---|---|---|
| `cast` | both | a cell that will not parse as its frozen type |
| `toofewcolumns` | CSV | a short row |
| `toomanycolumns` | CSV | a long row |
| `unquotedvalue` | CSV | a quote appearing where the dialect does not allow one |
| `unterminatedquote` | CSV | a quoted region the file never closes |
| `padded` | CSV | a short row that `null_padding` repaired — kept, and still distinguishable from a skipped one |
| `unknownkey` | JSON | a key beyond the frozen schema |
| `dupkey` | JSON | a key repeated within one record |
| `parse` | JSON | a line that will not parse as one whole in-line value |

## Rejects while streaming

With a lambda target, `.csv.read` hands each batch its own reject records as the second argument, so a streaming
load can react to them as they arrive rather than waiting for a table at the end:

```q
q).csv.read[`:trades.csv;{[tblData;errData;misc] if[count errData; show errData]};::;()!()];
```

`errData` is a table with the same four columns, and is the empty table — schema intact — on a clean batch. See
[Reading CSV](csv.md#a-lambda-called-once-per-batch).
