# Compatibility with kx q

peachq aims to be a drop-in q replacement, measured by q-observable behaviour: `parse` display, `type`, error text.
This page is the index for someone moving code from kx q — what will not work as it did, and what is here that was
not there before. One or two lines each, then a link to the page that covers it.

Everything here is deliberate. A behaviour that is simply broken is a bug, not a divergence, and is not listed.
The errors peachq signals that kx q does not have are listed in [Errors](errors.md).

## Not supported, or different

Ordered by how likely each is to stop a real migration.

| What | Consequence | More |
|---|---|---|
| **Partitioned and segmented databases** | Do not load. Splayed tables do. | below |
| **Splayed and partitioned writing** | Reading kx on-disk format is in scope; writing it is not. | below |
| **Pattern matching (kdb+ 4.1)** | List patterns work; dictionary, table and filter-function patterns do not. | below |
| **Serializing functions (`-8!`, IPC)** | Primitives use kx's bytes; keywords kx defines in k travel as a q lambda. | below |
| **Reserved words in name positions** | Refused wherever a name is bound, not only at `name:`. | below |
| **Load CSV (`0:`)** | Unchanged, and still needs the full type string and a clean file. | [csv.md](lib/csv.md) |
| **`set` to a `.csv`/`.json`/`.txt`/`.xml`/`.xls`/`.parquet` path** | Writes that format, not the q binary form; `t` must be a table. | below |
| **System commands and launch flags** | Some are unwired or no-ops; both pages mark every item against kx. | [cmdline.md](cmdline.md), [syscmds.md](syscmds.md) |

**Partitioned and segmented databases.** This is the largest single gap for an existing kdb+ installation: a
partitioned or segmented HDB does not load. Splayed tables do, including nested columns, attributes and kx
compression.

**Splayed and partitioned writing.** `.Q.dpft`, `dsave`, partitioned `set`, `save`'s binary arm, `rsave` and `-24!`
are unavailable. Flat `set`, `` `:dir/ set `` and `.z.zd` all work. For large local storage the route is the
`.duckdb` provider — see [Storing tables in DuckDB](sql-duckdb-storage.md) and [Handles and resources](handles.md).

**`set` writes the format the suffix names.** `` `:f.csv set t `` writes CSV (the lines `save` writes),
`` `:f.parquet set t `` writes parquet, and so on for every `.h.tx` key, where kx writes the q binary form under any
name. The read side already worked that way (`` select from `:f.csv ``); the write side now matches it. A value that
is not a table signals `'type` under those endings; a path with no recognised ending is the binary form as in kx.
See [Handles and resources](handles.md) § Format inference and [parquet.md](lib/parquet.md).

**`get` reads the format the suffix names.** `` get `:f.csv `` (and `value`) answers the table `` select from `:f.csv ``
reads, for every ending `select from` speaks (`.csv .tsv .json .jsonl .ndjson .parquet`), where kx signals `'type`
for any file that is not a kdb+ data file. An error becomes a value, so no working kx program changes meaning. A path
with no recognised ending is the kdb+ file read as in kx; an ending with a writer and no reader (`.xml`, `.xls`,
`.txt`) is `'type` as in kx.

**Pattern matching.** The kdb+ 4.1 list patterns work in assignment and in a function's parameter list: names,
nested lists, the null pattern (an empty slot), constants and type checks. Names bound inside a lambda are local to it.

```q
q)(b;c):(2;3)
q)(h;g):(c;b)
q)(a;(d;e)):(1;(2;3))
q)(s:`s;n:`j):(`x;5)
q)(s:`s;n:`j):(`x;5.0)
'type
q)(1 2):1 3
'match
q)r:{[x;(m;n)] x*m+n}
q)r[2;3 4]
14
```

Dictionary and table patterns, such as `` (i:`a;j:`c):d ``, signal `'nyi`. Filter functions, such as
`{[x:tempCheck]32+1.8*x}`, signal `'parse` and are not planned; use an ordinary check in the body, or a declared
type where a type suffices (see [typed parameters](typed-parameters.md)).

**Serializing functions.** Every function value serializes, through `-8!`, `-9!` and IPC, and `-9!-8!f` answers
a value matching `f`. What goes on the wire depends on what the function is:

- **A primitive, or a composition of primitives**, uses the kx wire form: `-8!neg` is `0x010000000a0000006502`
  and `-8!ceiling` is the composition `0x0100000018000000690200000065026902000000650e6502`.
- **A keyword kx defines in k** (`ej`, `mod`, `xbar`, `lj` and others) travels as a q lambda that calls it, not as
  kx's k source: `-8!ej` carries `{ej[x;y;z]}`. peachq decodes that exact text back to the keyword itself; a kx
  process receives an ordinary lambda that calls its own `ej`.
- **A library function implemented in C** (`.regexp.i.*`, `.duckdb.i.*` and others) travels as its `2:` call, such
  as `` {[x;y;z] (`pq 2:(`.regexp.i.test;3))[x;y;z]} ``. A peachq process decodes it to its own native; a kx process
  cannot run it.

```q
q)-8!ej
0x010000001b00000064000a000b0000007b656a5b783b793b7a5d7d
q)(-9!-8!ej)~ej
1b
```

Two gaps remain. A parse tree holding a composition head, such as `-8!parse "sum abs::"`, signals `'nyi`. A
malformed message given to `-9!` signals `'domain` or `'badfunc` where kx signals `'badmsg`, and a lambda written in
k (`k){...}`) does not decode, since peachq does not run k.

**Reserved words in name positions.** `([] null)`, `select null from t`, `{[null] null}` and `w[1] div:3` all signal
`'assign` where kx accepts them. Each was silently producing a broken table, or resolving to the keyword's own
function instead of the binding.

## Storage

**Splayed tables map at `get` and `\l`, and stay mapped.** `` get `:dir/ `` is the flip of `` cols!`:dir/ `` as in
kx — a table whose fixed-width columns are memory-mapped and whose compressed columns inflate a block on first
touch — and the maps live as long as the value does, so a table bound by `\l` holds its maps for the session.
`-3!t` prints `` +`a`b!`:dir/ ``, `flip t` is the dictionary, `value t` is the path, `.Q.qp t` is `0b`, and a table
derived from it (`select`, `1#`, `,`, `update` by value) is plain.

**Nested, string and enumerated columns are read into memory at `get`.** kx maps those lazily; peachq decodes them
when the table is opened, so a splayed table with large string, nested or `sym` columns costs memory at load time,
not at first touch. Fixed-width columns are mapped as in kx. The enumerated column stays an enumeration (`20h`),
and its domain file binds under its own name at `get`.

## Additions

| What | One line | More |
|---|---|---|
| **Reading CSV** — `.csv.read`, `.csv.info` | Type inference, quoting dialects, streaming targets, a reject channel. | [csv.md](lib/csv.md) |
| **Reading JSON** — `.j.read`, `.j.info` | A reader beside kdb's `.j.k` converter: written forms, a table, a schema. | [json.md](lib/j.md) |
| **The shared loader laws** | What a cell means, the freeze, the error classes, the tolerance levers. | [loading.md](loading.md), [bad-rows.md](bad-rows.md) |
| **Tables stored in DuckDB** — `:pq:duckdb:` | Keep q tables in a DuckDB file; qSQL and SQL query them in place. | [sql-duckdb-storage.md](sql-duckdb-storage.md) |
| **Resources at a URL** | A `` `: `` symbol can name a resource anywhere; `read0`, `read1` and qSQL resolve it. | [handles.md](handles.md) |
| **`neg` of a `:pq:` handle symbol** | The async form, the alias with a leading `-`, where kx signals `'type`: `neg[h] msg` sends async. | [handles.md](handles.md) |
| **Regular expressions** — `.regexp`, `rlike` | RE2-backed matching, extraction, replacement and splitting. | [regexp.md](lib/regexp.md) |
| **Typed parameters** | Declared types, optional arguments, defaults and varargs, read statically. | [typed-parameters.md](typed-parameters.md) |
| **Foreign functions** — `.ffi` | Call into a shared library from q. | [ffi.md](lib/ffi.md) |
| **Startup evaluation** — `-eval`, `-eval-before` | Run q text from argv after / before the startup script; no stdin piping. | [cmdline.md](cmdline.md) |
| **String helpers** — `.str` | `printf`/`format`, strip, prefix and suffix tests, character-class predicates. | at the REPL |
| **DuckDB-backed storage** — `.duckdb` | Query it from q, and reach Parquet and S3 through it. | at the REPL |
| **Parquet** — `.parquet.read`, `.parquet.write` | Read and write parquet through DuckDB; q types survive the round trip. | [parquet.md](lib/parquet.md) |

Everything above arrives with `\l pq`, except URL resources, `rlike` and the startup flags, which are always there. The two rows with
no page of their own are documented by their own doc comments — type `.str.printf` or `.duckdb.exec` at the prompt.
