# Compatibility with kx q

peachq aims to be a drop-in q replacement, measured by q-observable behaviour: `parse` display, `type`, error
text. This page records the places where it deliberately differs, in **both** directions: things kx does that we
have chosen not to replicate, and things peachq does that kx does not. Either way the reason is written down.
This page is for choices we have made on purpose. Bugs and unfinished work are not divergences — they live in
`PLAN.md`.

## Pattern matching (kdb+ 4.1) — not replicated

kdb+ 4.1 added pattern matching to assignment and to lambda parameters:

```q
(1):1                          / assert the right side matches the literal
(1 2):1 3                      / 'match
f:{[x;]x}                      / trailing empty parameter
c2f:{[x:tempCheck]32+1.8*x}    / run tempCheck against the argument on entry
```

**peachq does not implement this and does not intend to.** The forms above signal `'parse`.

The reason is not effort, it is who can read it. Pattern matching puts arbitrary *runtime* behaviour into a
signature: `{[x:tempCheck] … }` means "call `tempCheck` on entry", so the only way to know what that function
accepts is to execute q. Every tool outside the interpreter — an editor, an IDE's completion and hover, a linter,
a doc generator, a code reviewer reading a diff — is then either blind or forced to embed a q runtime.

peachq takes the other route: a **static** signature that says what it means in its own text, and that anything
can read cheaply without running code. That is [typed parameters](typed-parameters.md) — declared types,
optional arguments, defaults, varargs — where a type is a name (``x:`j``), not a function call, so a signature
can be checked by reading it.

We would rather have one description of a function's interface that every tool can use than a more expressive
one that only the interpreter can evaluate.

### What this means in practice

- The assertion forms (`(1):1`, `(1 2):1 2`) have no peachq equivalent. Use `~` and a signal, or `.qunit`
  asserts in tests.
- A trailing empty parameter (`{[x;]x}`) is not accepted; write the arity you mean.
- Argument validation on entry is expressed as a **type** in the signature where a type suffices, and as an
  ordinary check in the body where it does not.
- peachq's pattern-matching suite is extracted from the kx documentation, so it covers the whole feature.
  The rows for the unsupported forms are commented out rather than left red — a permanent red row asserts we
  intend to fix something, and here we do not. The remaining rows still run.

## Reading data straight from a URL — an extension

In kx q, a `` `: `` symbol that names a file is something you can read, and a `` `: `` symbol that names a
host and port is something you can talk to. peachq keeps both of those meanings and adds a third: a `` `: ``
symbol can name a resource **anywhere**, and the ordinary reading verbs will fetch it.

```q
q)2#read0 `:https://www.timestored.com/data/sample/iris.csv
"sepal.length,sepal.width,petal.length,petal.width,variety"
"5.1,3.5,1.4,0.2,Setosa"
```

`read0` and `read1` mean exactly what they always meant — text and bytes — and their **ranged** forms work too,
so you can read the middle of a remote file without downloading the rest:

```q
q)"c"$read1 (`:https://www.timestored.com/data/sample/iris.csv;0;41)
"sepal.length,sepal.width,petal.length,pet"
```

Behind that, peachq asks the server for exactly those bytes with an HTTP range request. A server that supports
ranges answers with just them; one that does not sends the whole file and peachq takes the slice itself. Either
way you get the same answer, and the same clamping rules a local file has always had — a short read at the end
of the file is short, past the end is empty, and a negative offset is `'domain`.

### Formats resolve on top of it

Because the transport is a separate idea from the format, everything that decodes a file works on a URL without
knowing what a URL is:

```q
q)3#.csv.read[`:https://www.timestored.com/data/sample/iris.csv;::;::;()!()]
| sepal.length | sepal.width | petal.length | petal.width | variety  |
| float        | float       | float        | float       |          |
|--------------|-------------|--------------|-------------|----------|
| 5.1          | 3.5         | 1.4          | 0.2         | "Setosa" |
| 4.9          | 3           | 1.4          | 0.2         | "Setosa" |
| 4.7          | 3.2         | 1.3          | 0.2         | "Setosa" |
```

And qSQL resolves a CSV resource as a table, so a query can name the file itself — local or remote:

```q
q)select cnt:count i, avgLen:avg petal.length by variety from `:https://www.timestored.com/data/sample/iris.csv
| variety      | cnt  | avgLen |
|              | long | float  |
|==============|------|--------|
| "Setosa"     | 50   | 1.462  |
| "Versicolor" | 50   | 4.26   |
| "Virginica"  | 50   | 5.552  |

q)select from `:trades.csv where sym=`AAPL
```

The trailing-slash convention is unchanged: `` `:dir/ `` is still a splayed table, and `` select from `:dir/ ``
still means what it has always meant.

### The rules we chose

- **A symbol is a resource. A string is content.** `` `:x.csv `` names something to fetch; `"a,b\n1,2"` *is*
  CSV, and `.csv.read` will decode it directly — handy when the bytes came from an HTTP body, a websocket, or
  anywhere else. A plain string is never quietly treated as a filename.
- **A recognised ending picks the decoder, and nothing else does.** `.csv` and `.tsv` resolve as tables. An
  unrecognised ending signals rather than guessing — we do not sniff content types or magic bytes to decide what
  your file is.
- **`get` does not become format-aware.** Reading a resource and interpreting one are different questions, and
  only qSQL and the explicit `.csv` API ask the second.
- **Transport errors keep their own class.** A host that will not answer is `'conn`, not `'csv` — a network
  problem never arrives disguised as a malformed file.

### What is not here yet

`s3://` and `zip://` are not supported. Parquet and S3 are reached through the DuckDB provider rather than
implemented natively — see the storage notes — so `.parquet` is not a resolvable ending here.
