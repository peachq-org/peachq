# Compatibility with kx q

peachq aims to be a drop-in q replacement, measured by q-observable behaviour: `parse` display, `type`, error text.
This page is the index for someone moving code from kx q — what will not work as it did, and what is here that was
not there before. One or two lines each, then a link to the page that covers it.

Everything here is deliberate. A behaviour that is simply broken is a bug, not a divergence, and is not listed.

## Not supported, or different

Ordered by how likely each is to stop a real migration.

| What | Consequence | More |
|---|---|---|
| **Partitioned and segmented databases** | Do not load. Splayed tables do. | below |
| **Splayed and partitioned writing** | Reading kx on-disk format is in scope; writing it is not. | below |
| **Pattern matching (kdb+ 4.1)** | The 4.1 assignment and parameter forms signal `'parse`. | [typed-parameters.md](typed-parameters.md) |
| **Reserved words in name positions** | Refused wherever a name is bound, not only at `name:`. | below |
| **Load CSV (`0:`)** | Unchanged, and still needs the full type string and a clean file. | [csv.md](csv.md) |

**Partitioned and segmented databases.** This is the largest single gap for an existing kdb+ installation: a
partitioned or segmented HDB does not load. Splayed tables do, including nested columns, attributes and kx
compression.

**Splayed and partitioned writing.** `.Q.dpft`, `dsave`, partitioned `set`, `save`'s binary arm, `rsave` and `-24!`
are unavailable. Flat `set`, `` `:dir/ set `` and `.z.zd` all work. For large local storage the route is the
`.duckdb` provider — see [Handles and resources](handles.md).

**Pattern matching.** It will not be implemented; [typed parameters](typed-parameters.md) are the replacement for
the part of it that declares what a function accepts. Since there is no equivalent form to link to:

```q
(1 2):1 3                      / 'match in kx 4.1; 'parse here
c2f:{[x:tempCheck]32+1.8*x}    / runs tempCheck on entry in kx 4.1; 'parse here
```

Use `~` and a signal in place of the assertion forms, a declared type in place of an entry check where a type
suffices, and an ordinary check in the body where it does not.

**Reserved words in name positions.** `([] null)`, `select null from t`, `{[null] null}` and `w[1] div:3` all signal
`'assign` where kx accepts them. Each was silently producing a broken table, or resolving to the keyword's own
function instead of the binding.

## Additions

| What | One line | More |
|---|---|---|
| **Reading CSV** — `.csv.read`, `.csv.info` | Type inference, quoting dialects, streaming targets, a reject channel. | [csv.md](csv.md) |
| **Reading JSON** — `.j.read`, `.j.info` | A reader beside kdb's `.j.k` converter: written forms, a table, a schema. | [json.md](json.md) |
| **The shared loader laws** | What a cell means, the freeze, the error classes, the tolerance levers. | [loading.md](loading.md), [bad-rows.md](bad-rows.md) |
| **Resources at a URL** | A `` `: `` symbol can name a resource anywhere; `read0`, `read1` and qSQL resolve it. | [handles.md](handles.md) |
| **Regular expressions** — `.regexp`, `rlike` | RE2-backed matching, extraction, replacement and splitting. | [regexp.md](regexp.md) |
| **Typed parameters** | Declared types, optional arguments, defaults and varargs, read statically. | [typed-parameters.md](typed-parameters.md) |
| **Foreign functions** — `.ffi` | Call into a shared library from q. | [ffi.md](ffi.md) |
| **String helpers** — `.str` | `printf`/`format`, strip, prefix and suffix tests, character-class predicates. | at the REPL |
| **DuckDB-backed storage** — `.duckdb` | Query it from q, and reach Parquet and S3 through it. | at the REPL |

Everything above arrives with `\l pq`, except URL resources and `rlike`, which are always there. The two rows with
no page of their own are documented by their own doc comments — type `.str.printf` or `.duckdb.open` at the prompt.
