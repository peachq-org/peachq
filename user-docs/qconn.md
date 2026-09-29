# `q -conn`: query any running q from your shell

`q -conn` points peachq at a q process that is already running — kdb+ or peachq, local or remote — and runs your q
text **there**, showing the answer here. It is the shortest path from a shell prompt to a live database: no IDE, no
client library, no quoting q inside a `h"..."` string.

```bash
q -conn :prod-host:5000 -eval 'select from trade where sym=`AAPL'
```

```
| time         | sym    | px   | size |
| time         | symbol | long | long |
|--------------|--------|------|------|
| 09:30:00.000 | AAPL   | 100  | 100  |
| 09:30:00.002 | AAPL   | 102  | 300  |
| 09:30:00.004 | AAPL   | 104  | 500  |
```

## Why use it

- **Works against the server you already have.** Each call is sent exactly as qStudio sends it, so any kdb+ process
  that accepts qStudio accepts `q -conn` — no server-side install, no new port.
- **Shell-native.** Values go to stdout, errors to stderr, and the exit code tells a script what happened. Pipe q
  in, pipe results out, put it in cron or CI.
- **Tables you can read.** Results display as pipe tables with column types, clipped to the console with a summary of
  what was left out; when piped, every column is kept.
- **Results you can keep.** `-save` writes the answer straight to CSV, JSON, Parquet or kdb binary, and every call is
  recorded in a local query history.

## Recipes

Ask one question:

```bash
q -conn 5000 -eval 'count trade'
```

Pipe a batch — one call per line. stdin is read only when there is no script file and no `-eval`:

```bash
printf 'count trade\nf 21\n' | q -conn :localhost:5000
```

```
5
42
```

Run a script on the server, then a query after it — the file runs as one call, each `-eval` after it. With a script
file or `-eval`, stdin is not read, so this runs and exits even from cron or a pipeline:

```bash
q setup.q -conn :localhost:5000 -eval 'select sum size by sym from trade'
```

See what the server holds — every variable, tables first, with type, count and columns:

```bash
q -conn 5000 -ls
```

```
.
  quote  98h  2        sym bid
  trade  98h  5        time sym px size
  f      100h 1        x
```

Pull a table down to a file — the ending picks the format (`.csv`, `.json`, `.parquet`, anything else kdb binary):

```bash
q -conn :prod-host:5000 -save trades.parquet -eval 'select from trade where date=.z.d'
```

Fail a job when the query fails:

```bash
q -conn 5000 -eval '1+`a' || echo "query failed"
```

```
'type
query failed
```

## Targets

Anything `hopen` takes: a bare port (`5000`), `:host:port`, `:host:port:user:pass`, a unix socket (`:unix://5000`),
TLS (`:tcps://host:port`), with or without the leading backtick. A TorQ-style gateway that answers by deferred reply
works too — you get the value, not the wrapper.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | every call succeeded |
| 1 | the server signalled an error — the first failing call stops the run, its error on stderr |
| 2 | bad arguments (`q: -p is not valid with -conn`) or no connection (`q: cannot connect to <target>: …`) |

## Good to know

- Only the value of the last expression in each call comes back. `show`, `0N!` and `-1` print on the **server's**
  console, not yours.
- `-conn` is a mode, not a flag: it takes a script file, `-eval`, piped stdin, `-save` and `-ls`, and nothing else.
  Nothing runs locally. Piped stdin is read only when there is no file and no `-eval`.
- Results over 10 MB come back as console text only, with a notice on stderr, and are not saved.
- Every call is recorded under `~/.qhist.d/<host>_<port>/`; when the display clipped a table, the full result is kept
  as kdb binary and stderr tells you the path. `PEACHQ_QHIST=` (set and empty) turns this off.

The full reference — ordering, display width, the history format — is [the command line § `-conn`](cmdline.md).
