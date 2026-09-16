# Handles and Resources

PeachQ keeps the familiar kdb+/q handle syntax while giving it a clearer mental model for new resource types.

In kdb+/q, symbols beginning with `:` are commonly described as **file handles**, **process handles**, or **communication handles** depending on how
they are used. PeachQ preserves those existing behaviours and syntax. For the extended model described here, we use the term **resource
specification** for a `:...` symbol that identifies something outside the immediate q value space.

For example:

```q
`:data/trades.csv
`:localhost:5000
`:https://www.timestored.com/data/sample/dowjones.csv
`:pq:duckdb:prod:/data/market.db
```

A resource specification says **what resource is being referred to**. The operation applied to it determines what PeachQ asks that resource to do.

Existing kdb+/q behaviour takes precedence where it is defined. PeachQ extends the model to additional transports, containers, formats, and providers.

## The mental model

A resource can have one or more capabilities. It may be readable, writable, openable, callable, queryable as a table, or expose other resources.

These capabilities overlap. They are not intended to form a rigid hierarchy of mutually exclusive handle types.

| Resource | Example | Typical behaviour |
|---|---|---|
| File | ```:data/x``` | `get`, `set`, `read0`, `read1`, file I/O |
| Splayed table | ```:data/trade/``` | `get`, `select`, `exec`, table operations |
| q IPC endpoint | ```:localhost:5000``` | open, synchronous call, asynchronous send |
| HTTP resource | ```:http://host/x``` | retrieve remote content; planned table resolution |
| WebSocket endpoint | ```:ws://host/x``` | connect, then persistent framed messaging |
| PeachQ provider | ```:pq:duckdb:prod:/data/db``` | provider-defined connection and table capabilities |

An **opened handle** is different from a resource specification. For example, `hopen` on an IPC resource returns an integer handle. The `:...`
value identifies the resource; the returned integer represents an opened connection to it. `hopen` on a `:pq:` provider resource returns the
**alias symbol** instead (see PeachQ providers below): the handle says what it is, and it is the same value the coordinate forms use.

## Resources as tables

PeachQ extends the existing q idea that some resources can be used directly as tables.

A trailing `/` is an important part of that model. In existing q, a splayed table is addressed by a directory-style resource ending in `/`. PeachQ
should preserve that signal and use it consistently:

```q
`:data/trade/
`:pq:duckdb:prod:trade/
```

A trailing `/` means that the resource is explicitly table- or collection-like. A resource without the trailing slash remains file-, object-,
endpoint-, or member-like unless another rule, such as a recognised tabular file format, gives it table semantics.

This gives PeachQ two clear routes into qSQL:

1. An explicit table resource, normally signalled by a trailing `/`, such as a splay or virtual provider table.
2. A file-like resource whose format is known to decode to a table, such as `.csv`, `.tsv`, `.json`, or `.parquet` (see [parquet.md](parquet.md)).

Existing q supports operations such as:

```q
select from `:data/trade/
```

PeachQ generalises the idea:

```q
select from `:trades.csv
select from `:https://www.timestored.com/data/sample/dowjones.csv
select from `:pq:duckdb:prod:trade/
```

The intended meaning in each case is:

> Resolve this resource as a table, then apply qSQL to it.

The mechanism used to obtain the table may be completely different for each resource.

A local CSV may be read and decoded into a table. An HTTP CSV may first be downloaded and then decoded. A DuckDB provider may translate and push the
qSQL query into DuckDB without materialising the full table.

The qSQL expression should not need to know which path was taken.

## Transport, container, format, and provider

When resolving data resources, it is useful to separate four concepts.

| Concept | Examples | Responsibility |
|---|---|---|
| **Transport** | file, HTTP, HTTPS, S3 | Obtain underlying bytes or content; where possible support ranged `read0` / `read1` |
| **Container** | ZIP | Expose a member or child resource |
| **Format / decoder** | CSV, TSV, JSON, Parquet | Convert content into a logical value such as a table |
| **Query provider** | DuckDB, q IPC provider | Expose table/query semantics directly, potentially with pushdown |

These concepts can compose.

For example, the proposed:

```q
select from `:https://www.timestored.com/data/sample/dowjones.csv
```

is conceptually:

```text
HTTP transport
    -> content
    -> CSV decoder
    -> table
    -> qSQL
```

A proposed ZIP example:

```q
select from `:zip://archive.zip/trades.csv
```

is conceptually:

```text
file transport
    -> ZIP container
    -> trades.csv member
    -> CSV decoder
    -> table
    -> qSQL
```

ZIP support and `zip://` syntax are **planned, not currently supported**.

This separation is important. HTTP is not a CSV provider: it is a transport. ZIP is not a table format: it is a container. CSV describes how the final
content is decoded.

## `get`, `read0`, `read1`, and `select`

These operations ask different questions of a resource.

```q
get `:some-resource
```

asks for the resource using its normal q `get` semantics. For ordinary file paths, `get` retains existing q object-loading semantics.

```q
read0 `:some-resource
read1 `:some-resource
```

ask for the textual or binary contents of a resource. PeachQ should preserve the existing q distinction: `read0` is the text-oriented read operation
and `read1` is the binary read operation.

Both operations should preserve q's ranged-read forms:

```q
read0 (`:some-resource;offset;length)
read1 (`:some-resource;offset;length)
```

Offsets are resource byte offsets. A transport that supports native range reads, such as HTTP or S3, may satisfy these operations without fetching the
whole object. A transport that does not support efficient ranges may fall back to fetching and slicing.

For HTTP, the intended PeachQ direction is therefore:

```q
read0 `:http://example.com/a.txt
read1 `:http://example.com/a.bin
read1 (`:http://example.com/large.bin;1048576;65536)
```

while:

```q
select from `:http://example.com/a.csv
```

will mean:

```text
HTTP transport
    -> resource content
    -> CSV decode
    -> table
    -> qSQL
```

HTTP table selection is a **proposal/planned extension**, not a statement of current support.

The important rule is that `get` does not become CSV-aware merely because a URL ends in `.csv`. `read0` and `read1` retrieve resource content, while
the **table resolver used by qSQL** performs format interpretation.

## Format inference

For table selection, PeachQ should prefer explicit interpretation over inference.

The initial rule is:

1. An explicit scheme or PeachQ provider wins.
2. Otherwise, a recognised file ending selects the format.
3. Otherwise, PeachQ does not invent a tabular interpretation.

For example:

```q
select from `:trades.csv
select from `:trades.tsv
select from `:trades.json
select from `:trades.jsonl
select from `:trades.ndjson
```

can select the CSV/TSV or JSON decoder from the file ending.

The ending is a **declaration**, not a hint, so it can carry more than the decoder's name. `.jsonl` and `.ndjson` say the
file is JSON Lines and it is read that way; `.json` says only JSON, and the reader decides whether the file is one
document or a stream. Nothing is inferred from the bytes: a `.jsonl` holding a single JSON array is an error rather than a
quiet re-reading. [Reading JSON](json.md) covers framing.

The same rule is intended to apply after transport resolution:

```q
select from `:https://www.timestored.com/data/sample/dowjones.csv
```

The final `.csv` identifies the decoder after HTTP has retrieved the content.

If the resource does not have a recognised table format:

```q
select from `:trades.dat
```

PeachQ should fail rather than guess.

The existing `get`, `read0`, and `read1` behaviours remain independent of table-format inference.

The same ending selects the WRITER. `` `:f.EXT set t `` writes the format `.h.tx` names for `EXT` — every `.h.tx`
key: `csv`, `txt`, `xml`, `xls` and `json` through peachq's own writers (the lines `save` would write, so
`` select from `:f.csv `` reads them back), `parquet` through `.parquet.write` (see [parquet.md](parquet.md)). `t`
must be a table (`'type` otherwise); no recognised ending is the binary form as in kx, and a dotfile such as
`` `:.json `` is not a format claim. This is a documented divergence from kx, where the same `set` writes the q
binary form under any name. To a remote scheme (`s3://`, `gcs://`, `az://`, `hf://`) only parquet writes today;
the other formats arrive with the transport law (B3).

Applications that know an ambiguously named file is CSV can use the explicit CSV API rather than relying on qSQL inference.

PeachQ should not initially make HTTP `Content-Type`, magic-byte sniffing, or other heuristics part of this contract. They can be considered later
without changing the basic model.

## CSV and other decoded formats

The explicit CSV API remains:

```q
.csv.read[file;target;types;opts]
```

`.csv.read` is a local CSV loader and decoder. It should not need to understand HTTP, ZIP, S3, or every future transport.

For direct qSQL over a local CSV:

```q
select from `:trades.csv
```

PeachQ can use the CSV decoder to obtain a table and execute qSQL locally.

For a future remote CSV:

```q
select from `:https://www.timestored.com/data/sample/dowjones.csv
```

PeachQ owns the composition:

```text
HTTP retrieves content
    -> CSV decodes content
    -> qSQL evaluates the table
```

The CSV implementation does not become an HTTP implementation. PeachQ's resource layer obtains the content first and then invokes the CSV decoding
path.

TSV and JSON follow the same general model where they can be interpreted as tables. Parquet table resources are planned and should follow the same
resource model, although their implementation may support more efficient projection or predicate pushdown than a simple materialising decoder.

## Ranged reads

Ranged reads are part of the resource contract because they are useful for both existing file I/O and remote object stores.

```q
read1 (`:large.bin;1048576;65536)
read1 (`:s3://bucket/large.parquet;footerOffset;footerLength)
```

The public operation remains `read0` or `read1`; transports should not require separate user-facing APIs such as `.s3.readRange`. Native range support
is an implementation optimisation, not a different user model.

This is particularly important for formats such as Parquet, where a decoder may read file metadata and selected row groups without materialising the
whole object.

## Containers

Containers introduce another resource-resolution step.

The planned ZIP notation is:

```q
`:zip://archive.zip/member.csv
```

and, when table selection is supported:

```q
select from `:zip://archive.zip/member.csv
```

`zip://` explicitly says that the outer resource is a ZIP container. This avoids relying on the outer filename ending in `.zip`.

The member can then be interpreted independently from the container:

```text
:zip://anything.dat/member.csv
             ^          ^
         ZIP container  CSV format
```

This is deliberate. A ZIP file does not need to be named `.zip`, and a CSV file does not always need to be named `.csv` when the caller uses an
explicit decoding API.

Future transports and containers should compose rather than require combined implementations such as "HTTP CSV" or "ZIP CSV".

## PeachQ providers

Some resources are not naturally modelled as transport + decoder. They already provide operations over structured data.

PeachQ reserves the explicit prefix:

```text
:pq:<provider>:<alias>:<resource>
```

For example:

```q
`:pq:duckdb:prod:/data/market.db
```

`:pq:` was chosen as a clean namespace for PeachQ resource providers and, in particular, to make named aliases unambiguous.

A provider connection can expose tables which participate directly in qSQL. Table resources should retain the trailing `/` convention:

```q
`:pq:duckdb:prod:trade/
```

A DuckDB-backed table can therefore resolve through the DuckDB provider rather than being downloaded and decoded as a file.

This also gives PeachQ an explicit escape hatch where inference from a normal path would be inappropriate.

### Opening, using and closing a provider connection

```q
h:hopen `:pq:duckdb:prod:/data/market.db     / answers the alias symbol `:pq:duckdb:prod
h "SELECT count(*) FROM trade"                / the handle applies like any q handle: text is a call
h (`get;`trade)                               / a list names a provider hook
hclose h                                      / the one close door
```

- `hopen` **answers the alias symbol** `` `:pq:<provider>:<alias> `` — not an int. The alias is **required**: a
  handle needs a name, so `` hopen `:pq:duckdb::/data/market.db `` is `'domain`. The aliasless form is the one-shot
  apply, `` `:pq:duckdb::/data/market.db "SELECT 1" `` (open, run, close, nothing registered).
- The symbol IS the live connection: `` `:pq:duckdb:prod "SELECT 1" `` and `h "SELECT 1"` are the same call, and
  the public verbs of a provider take it (`.duckdb.exec[h;sql]`). After `hclose h` the same symbol answers `'conn`.
  Opening the same alias again re-points it in place and answers the same symbol.
- `hclose h` is the only close door; a provider's own open/close are hooks the host calls, with no public spelling.
- `.pq.conns[]` lists every open connection: `handle` is what `hclose` takes (the alias symbol for a provider row,
  the int for a socket or file), `h` the fd, with `provider`, `alias` and `opened` beside them.
- **Async is a hook call**: `h (`async; msg)` reaches the provider's `.X.async` — the q IPC provider defines it as
  the async send; an in-process engine like DuckDB does not (`'.duckdb.async`). `neg` on the symbol stays q's `neg`.
- The int fd shown as `h` in `.pq.conns[]` is the legacy form, for code that expects `hopen` to answer an int: it is
  accepted wherever the symbol is, and goes once nothing uses it.

**The DuckDB link and `s)`.** The process has ONE DuckDB database — the main instance, whose own handle is
`` `:pq:duckdb:main `` (listed by `.pq.conns[]`, answered by `.duckdb.main[]`; `hopen`/`hclose` refuse the alias, and
`q -duckdb path` makes it a file). Every DuckDB alias is a catalog ATTACHed to it under the alias's name, shared by
path. A q global bound to a DuckDB table — `` myt:get `:pq:duckdb:al:dt/ `` — becomes a same-named VIEW in main at the
moment of assignment (a view, never a copy: `` `myt insert x `` writes through the provider and the next `s)` reads
it); re-pointing the global re-points the view, any other value drops it, `` `.ns.t `` links under its full name.
`s)SELECT count(*) FROM myt` is that view; `s)SELECT * FROM al.dt` reaches the alias's catalog qualified, and a bare
`dt` is NOT visible (a bare name in `s)` is a q name). After `hclose` of the alias the view errors on use, as the
pointer's own `get` does. `\?duckdb` and `docs/duckdb-api.md` have the rest; the q IPC provider defines no link.

**Loading and dropping — the sync law.** Anything done THROUGH q is in sync by construction: assigning, re-pointing
or unbinding a pointer, `insert`/`upsert` on one, `hdel` of a coordinate — one global-set seam, one provider hook per
verb. Anything done through the provider's own language is best effort, weighted to additions; nothing prints, and
nothing is ever unbound behind your back.

- **Provider → q is a LOAD, and `\l` is its verb.** `` \l `:pq:duckdb:al `` binds every table and view of the
  alias's catalog as a pointer under its own name, at the root (the `\l dir` law), later-wins on names — a global
  `dt` you had becomes the pointer; `` \l `:pq:duckdb:al:dt/ `` loads that one. `.duckdb.load[h;tables]` is the
  verb form: `()` none, `::` all, else the names; it answers the names bound. The alias must be live (`'conn`): a
  load never opens a connection. A loaded pointer links like any other, so its bare name is a q name in `s)`.
  `q -duckdb path` loads main's tables at startup like `q dir/` — and so implies `\l pq`.
- **`s)` after a `CREATE` or `ALTER` binds the NEW names** of main's own catalog (`CREATE TABLE … AS SELECT`
  included); an existing global keeps its value — `\l` is the later-wins door, this one is add-only. Row writes need
  nothing (a pointer reads live). `s)DROP TABLE x` leaves q's `x` bound: it errors on use with DuckDB's own text.
- **`hdel` on a coordinate drops the OBJECT**: `` hdel `:pq:duckdb:al:dt/ `` is `DROP TABLE` (or `DROP VIEW`, by what
  the name is); `` hdel `:pq:qpc:al:t/ `` deletes the table on the peer. A q name bound to it stays bound and errors
  on use. The connection form is `'domain`, a dead alias `'conn`, a provider without the hook `'nyi`. Deleting the q
  name (`delete dt from `.`) never drops the object — the splay precedent: deleting `` t:get `:db/t/ `` never removes
  the directory.

## Compatibility principle

PeachQ should preserve established kdb+/q meanings of handles wherever practical.

The extended resource model is intended to explain and extend those behaviours, not redefine ordinary q file and IPC operations.

- Existing file `get`/`set`, `read0`, and `read1` semantics remain existing file semantics.
- Existing q IPC resource and opened-handle behaviour remains compatible.
- Existing splayed-table qSQL behaviour remains valid, including the trailing `/` table-resource convention.
- New table-resource behaviour extends the same idea to additional sources.
- Explicit PeachQ providers use the `:pq:` namespace so they do not need to overload unrelated existing handle forms.

This gives existing q code the familiar compact syntax while allowing new code to treat external data sources consistently.

## For implementers: provider and qSQL contract

Writing a `:pq:` provider — the `.X` namespace, every hook the host calls (`.X.i.open`/`.X.i.close`, `call`/`async`,
`bind`/`get`/`set`/`upsert`/`count`/`meta`/`qsql`, the link hooks `.X.i.link`/`.X.i.unlink`), which are required and
which optional, and what the host guarantees — is its own page: `user-docs/resource-handle-implementer.md`.



# Dev Work
Your order is good. I’d tighten it to this:

1. Generalize read0 / read1 to resource identifiers
   Support :http://... first, including ranged reads; move .Q.hg and similar HTTP helpers onto the same underlying transport calls.
2. Refactor .csv.read around generic readable inputs
	.csv.read[fileOrString;...] should accept raw content or any readable resource and rely only on shared read0/read1 primitives, never implement HTTP itself.
3. Add qSQL over CSV resources
	Support select from :a.csv and select from :http://.../a.csv; resolve → read → .csv.i.read/C decoder → in-memory table → host qSQL, without requiring \l pq.
4. Add ZIP as a readable container resource
	Support read0/read1 :zip://file.zip/a.csv (or final chosen syntax), so CSV-over-ZIP works automatically through the same resource pipeline.
5. Only specialize local/ZIP CSV if profiling justifies it
	If .csv.i.read can consume the same low-level streaming/ranged reader used by read0/read1, avoid separate ZIP/HTTP code paths; optimize underneath that common seam.
6. Add .j.read later using the same source contract
	JSON should reuse the same resource layer from day one: raw string/bytes or any readable :... resource, with no transport-specific logic.
	
	
## Points


1. Keep one low-level resource-read seam beneath read0/read1; HTTP, ZIP, CSV, .Q.hg, S3, etc. must reuse it.
2. Treat strings/byte vectors as content and :... symbols as resource identifiers. Never make plain strings implicitly mean filenames.
3. Make ranged reads first-class: (resource;offset;length) should map to native seek/range where possible, fallback fetch+slice otherwise.
4. Define EOF/range semantics once: short reads, offset beyond EOF, zero length, invalid negative values.
5. Keep transport errors separate from decoder errors: HTTP/TLS failures are not 'csv; malformed CSV is not 'http.
6. Preserve streaming. .csv.read should consume chunks from the common reader without forcing full materialization.
7. Formats must not know transports. CSV/JSON/Parquet should depend only on readable-resource primitives.
8. Containers must not know formats. ZIP exposes member content; normal format resolution happens afterward.
9. Make suffix inference a resolver concern, not a decoder concern.
10. Resolution order: explicit provider/scheme > explicit format API > recognized final suffix > error.
11. Do not initially infer from HTTP Content-Type or magic bytes.
12. Preserve trailing / as the explicit table/collection signal for splays and provider-backed tables.
13. Keep provider resolution separate from file-format resolution: :pq:duckdb:.../ is a provider; .csv is a decoder.
14. .qsql pushdown is optional; materialize-then-host-qSQL is a valid implementation.
15. Avoid combined implementations like httpcsv, zipcsv, or s3parquet; those indicate abstraction leakage. The one documented exception: a `.parquet` URL is handed to DuckDB, whose httpfs does the range reads a footer needs (parquet.md).

