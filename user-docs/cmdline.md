# The command line

This page is the one home for how you launch peachq: every option the `q` binary takes, how each compares with kx q,
the peachq-only flags for running q text at startup, and the `-conn` mode that runs it on another process. The system commands you type once you are inside a
session have their own page: [System commands](syscmds.md).

The general shape is kx's:

```bash
q [file.q] [-option [parameters] ...]
```

The first token ending in `.q` is the startup script; everything after it that q does not consume for itself is
yours to read back as `.z.x` (`.z.X` keeps the raw command line). The flags that work exactly as kx documents them
today are `-e -E -p -q -z`; `-c` is applied but not yet removed from `.z.x`, and `-u`/`-U` are applied but with
their meanings swapped against kdb (a known defect). Everything else is in the table below.

**A failing startup script** follows kx: when stdin is a terminal the script is the console's first `\l`, so the
erroring statement suspends into the `q))` debugger with the session up — `:` resumes the load at the next
statement, `\` abandons it and returns to `q)`. When stdin is not a terminal (`q file.q </dev/null`, a pipe, a
supervisor) the load aborts at the error and the process exits non-zero; that batch contract is unchanged.

**`QINIT` names a file to load first.** When the environment variable is set, peachq loads that file (as given:
absolute, or relative to the directory you start in) after its own bootstrap and any `-duckdb` main database, before
the startup script and the prompt, and on a non-terminal stdin before every `-eval-before` text too — so anything it
defines is visible to all of them. (On a terminal the `-eval-before` texts stay batch and run before the console
takes over, so there they precede it.) It is an ordinary `\l`: definitions land in the root namespace, top-level values echo, and a failing one follows the
startup-script rule above (a terminal suspends into `q))`, a non-terminal exits non-zero and the script never runs);
a path that does not exist is the same `cannot open script` error a missing startup script gives. Unset or empty,
nothing happens — peachq ships its own `q.q` as part of the bootstrap, so there is no `$QHOME/q.q` default to fall
back to. `.z.v` reports the value.

## Every option

The **status** column is the point of this table: **same** = behaves as the kx documentation describes, **DIFFERS** =
recognized surface but not (or not fully) what kx does, **PEACHQ ONLY** = does not exist in kx q. The **also as**
column names the system command that reads or sets the same thing at runtime, where one exists — and where a
`\`-command merely shares the letter, it says so instead.

| Flag | What it does | Status | Also as |
|---|---|---|---|
| `-b` | block client write-access | **DIFFERS** — not yet wired, and left in `.z.x` | the reader is `\_`, not `\b` (`\b` is views) |
| `-c r c` | console size (rows, columns) | **same**, but left in `.z.x` | `\c`, `system "c"` |
| `-C r c` | HTTP display size | **DIFFERS** — not yet wired, and left in `.z.x` | `\C` |
| `-e 0\|1\|2` | error-trap mode for client evals | **same** | `\e` |
| `-E 0\|1\|2` | TLS server mode | **same** | `\E` (view only) |
| `-g 0\|1` | garbage-collection mode | **DIFFERS** — not yet wired, and left in `.z.x` | `\g` |
| `-l` | log updates to a file | **DIFFERS** — not yet wired | none — `\l` **loads a file**, same letter only |
| `-L` | as `-l`, synchronous | **DIFFERS** — not yet wired | none |
| `-m path` | memory domain | **DIFFERS** — not yet wired | none |
| `-o N` | offset from UTC (hours) | **DIFFERS** — not yet wired, and left in `.z.x` | `\o` |
| `-p N` | listen on port N; `0W` binds an OS-chosen free port; peachq also accepts the spelling `--port N` | **same** for a plain port or `0W` | `\p` |
| `-P N` | float display precision | **DIFFERS** — not yet wired, and left in `.z.x` | `\P` |
| `-q` | quiet mode: no startup banner; `.z.q` reads it back | **same** | none |
| `-r :h:p` | replicate from a primary | **DIFFERS** — not yet wired | `\r` |
| `-s N` | secondary threads | **DIFFERS** — not yet wired, and left in `.z.x` | `\s` |
| `-S N` | random seed | **DIFFERS** — not yet wired, and left in `.z.x` | `\S` |
| `-t N` | timer period (ms) | **DIFFERS** — not yet wired, and left in `.z.x` | `\t` — which is also `\t expr` expression timing |
| `-T N` | client query timeout (s) | **DIFFERS** — not yet wired, and left in `.z.x` | `\T` |
| `-u file` | password file, and restrict client evals | **DIFFERS** — swapped with `-U` (known defect, see below) | `\u` **reloads** the file at runtime |
| `-U file` | password file | **DIFFERS** — swapped with `-u` | none |
| `-w N` | workspace memory **limit** (MB) | **DIFFERS** — not yet wired, and left in `.z.x` | none — `\w` reports memory **usage**, same letter only |
| `-W N` | start-of-week offset | **DIFFERS** — not yet wired, and left in `.z.x` | `\W` |
| `-z 0\|1` | date parse order (`"D"$`): 0 = mdy, 1 = dmy | **same** | `\z` |
| `-classic` | legacy kx table display, kdb-clean environment | **PEACHQ ONLY** | `\classic` |
| `-eval "src"` | run q text **after** the startup script | **PEACHQ ONLY** | none |
| `-eval-before "src"` | run q text **before** the startup script | **PEACHQ ONLY** | none |
| `-duckdb path` | the main DuckDB database is this file, not memory; its tables load at startup (implies `\l pq`) | **PEACHQ ONLY** | none |
| `-conn target` | run the file, `-eval` texts and stdin on a RUNNING q at any `hopen` target — a mode of its own, see below | **PEACHQ ONLY** | none |
| `-save file` | with `-conn`: `set` the last result to this file, its ending choosing the format | **PEACHQ ONLY** | none |
| `-ls` | with `-conn`: list the server's variables, tables first | **PEACHQ ONLY** | none |

`-q`, `-L`, `-m`, `-eval`, `-eval-before`, `-duckdb`, `-conn`, `-save` and `-ls` have no system-command equivalent.

## `-duckdb`

The process has ONE DuckDB database — the main instance, `` `:pq:duckdb:main `` — opened on first need: by `s)`,
`.parquet.read`, `.duckdb.main[]` or the first `` hopen `:pq:duckdb:… ``. It is in-memory unless `-duckdb path`
names a file (the flag wins over the `PEACHQ_DUCKDB_MAIN` environment variable, which does the same). Everything the
bridge puts in main — the views that link q globals to DuckDB tables, `s)CREATE TABLE …` — then persists in that
file, so a second `q -duckdb path` reads it back. DuckDB locks a database file per process: two live processes cannot
share one. The flag is launcher-consumed (never in `.z.x`); `user-docs/handles.md` and `\?duckdb` have the instance.

**`-duckdb path` loads main's tables at startup**, like `q dir/` loads a database directory: before the startup script
runs, every table and view of the file becomes a q pointer under its own name (`tables[]` lists them, `s)` reads them),
which implies `\l pq` — the loader is standard library, so the process starts with it loaded. `PEACHQ_DUCKDB_MAIN`
does the same. A fresh in-memory main (no flag, no variable) loads nothing and stays untouched.

```bash
q -duckdb work.duckdb -eval 's)CREATE TABLE x AS SELECT 1 AS a'
q -duckdb work.duckdb -eval 'show tables[]' -eval 's)SELECT * FROM x'   # a second process: x is a q name already
```

## `-eval` and `-eval-before`

A cross-platform way to run q text at startup without piping stdin — piping is awkward on Windows and in CI:

```bash
q -eval "show 2+2"                 # prints 4
q startup.q -eval "show count t"   # startup.q loads first, then the text runs
q -eval-before "opts:`fast" s.q    # opts is bound before s.q loads
```

The text is **a script whose source came from argv**, not a console line, so script rules apply:

- **Silent.** Results are not echoed. `q -eval "2+2"` prints nothing; print with `show` or `-1`.
- **Multiline text and `\`-commands work**, under the same script semantics a `.q` file gets.
- **An error aborts.** The text stops at the first erroring statement and everything after it is skipped — exactly
  like a failing startup script: on a non-terminal stdin the process exits non-zero (a `-eval-before` error also
  skips the script); on a terminal a `-eval` text runs after the script as a console-initiated script, so its error
  suspends into `q))` and `\` returns to the prompt.

**Order is named by the flag, not by argv position.** Every `-eval-before` runs before the startup script and every
`-eval` after it; repeats of the same flag run left-to-right. So `q s.q -eval "A" -eval-before "B"` runs B, then
s.q, then A.

After the texts run, the session does what it always does: an interactive terminal drops to the `q)` prompt, a
non-terminal stdin exits 0, and a live listener serves. `-eval` does not imply "exit after" — end the text with
`exit 0` if that is what you want.

Both flags are consumed by the launcher, so neither the flag nor its text appears in `.z.x`.

## `-conn`: run q text on a running process

```bash
q [file.q] -conn target [-eval "src"]... [-save file] [-ls]
```

`-conn` sends q text to a q or peachq process that is already listening, the way an IDE does, and shows the answer
here — no PTY, no persistent client, no escaping of your own beyond the shell's. It is **a mode, not a flag**: the only
things it takes are the positional `file.q`, `-eval` (repeatable), piped stdin, `-save` and `-ls`. Every other `q`
flag is an error — `q: -p is not valid with -conn`, exit 2 — because nothing runs locally: the local process boots,
loads its standard library, connects, and everything you hand it is evaluated on the server.

```bash
q -conn :localhost:5000 -eval 'select from trade where sym=`AAPL'
q setup.q -conn :localhost:5000                   # the file's text is one call, then each -eval, then stdin
echo "count trade" | q -conn :localhost:5000
q -conn :localhost:5000 -save out.parquet -eval 'trade'
q -conn :localhost:5000 -ls
```

- **Target.** Anything `hopen` takes, passed through as written: all digits is a port (`-conn 5000`), anything
  else the symbol — `:localhost:5000`, `::5000`, `:host:port:user:pass`, `:unix://…`, `:tcps://…`, even a `:pq:`
  resource — with or without the leading backtick. Nothing is checked or added here: a target that opens but is
  not a q process fails at the first call, with that call's error. The connection uses a 5-second timeout; a
  refusal is `q: cannot connect to <target>: <hopen's error>`, exit 2.
- **Ordering** is the local process's: the file's whole text as ONE call, then each `-eval` text as one call, then
  piped stdin one call per line (blank lines run nothing). Multi-line text — a file, or an `-eval` with newlines —
  runs as a script on the server: statement by statement, the last statement's value comes back.
- **Display.** The value of each call is shown as the console would show it: a table as the pipe table, clipped to
  `\c` rows with the `... (showing first n of N rows)` line and the per-column summary (`px=1-3. sym=all distinct.`)
  under it; anything else in the usual form. On a terminal the console is the terminal's size; when stdout is a
  pipe or a file the rows stay 25 but the WIDTH is unbounded, so every column reaches the reader. stdout carries only
  values; notices and errors go to stderr. A server that answers by deferred reply (a TorQ gateway) hands back the
  value itself rather than the wrapper's reply; it is shown, recorded and saved like any value.
- **What is not captured.** Only the value of the last expression comes back. Anything the server itself prints —
  `show`, `0N!`, `-1` — lands on the server's console, not here.
- **`-save file`** evaluates as usual and `set`s the LAST call's value to the file, the ending choosing the format:
  `.csv`, `.json`, `.parquet`, anything else kdb binary. Earlier calls run silently, as a script's non-final
  statements do. A result over the 10 MB display gate (below) is refused rather than saved as display text.
- **`-ls`** lists every variable on the server, whole, regardless of the console size: one block per namespace
  (root `.` first, the rest sorted), each variable as `name type count columns` (and `view` where it is one), tables
  first within the block; the language's own namespaces (`.Q .q .h .j .help .pq`) are left out.
- **Exit codes.** 0 on success; 1 on a remote `'error`, which stops at the first erroring call as a failing script
  would, the error class on stderr (`'type`); 2 on bad arguments or no connection.
- **The wire.** Each call is the qStudio evaluation wrapper, sent byte-identical (some sites recognise its prefix),
  so any q server that accepts qStudio accepts this. The wrapper carries qStudio's 10 MB size gate: a larger result
  arrives as its console text only, with a notice on stderr, and is not saved.

### Query history: `~/.qhist.d/`

This is the `-conn` result store; the console's own line history, `~/.qhist`, is described in [the REPL](repl.md).

Every call is recorded under `~/.qhist.d/<host>_<port>/` (`HOME`, or `USERPROFILE` on Windows; credentials never
reach a path): `index.tsv` keeps the last 200 calls — timestamp, `ok`/`fail`/`oversize`, the error text, serialised
bytes, rows, and the query flattened to one line of at most 200 characters — and the last 10 results are kept as
`<yyyymmdd_hhmmss_mmm>.bin`, the `-8!` bytes of the VALUE, at most 100 MB in all, the oldest evicted first. When the
display hid part of a table — rows beyond the console's height, columns beyond its width — it ends with
`Full result saved locally to kdb binary: <path>` on stderr, so `-9!read1 `:<path>` gives the whole result back; a
table shown whole is the full result and gets no notice, though its `.bin` is written and indexed all the same. `PEACHQ_QHIST=/some/dir` relocates the root; `PEACHQ_QHIST=` (set and empty) turns the history off.

# Notes for dev

This section is temporary bookkeeping and is expected to disappear as the entries above go green.

- **The published kx pages contradict each other on `\_`** — the kx command-line page shows `q)\_` answering `1`
  where the kx system-commands page shows `0b`. The cmdline/options qscript suite pins `1b`, following the
  system-commands page as the shape authority its header cites. Nothing to fix in the engine until the corpus is
  reconciled; recorded so the next reader does not "correct" the golden to the other page.
- **The command-line options are TWO independent gaps, not one** — stripping from `.z.x` AND propagation to the
  setting. `-c` is the proof: `system "c"` answers the value (applied) while `any .z.x like "-c"` is `1b` (not
  stripped); `-e` is the mirror image (stripped AND applied, only its getter's type is off); `-o -P -S -t -W -g -s
  -C` are neither. Pinned since 2026-08-17 by the cmdline/options qscript suite — a PAIR of rows for every option
  with a getter, each red row naming its own gap. Two getters (`\_ \T`) and the bare `\u` reload answer `'nyi`
  (the `\z` getter, listed here before 2026-08-30, now works), and `\e \g \W \s` answer `i` where the un-printed
  doc reading says long.
- **`-u` and `-U` are swapped against kdb (2026-08-14, #487)** — kdb's `-u file` is "password file AND restricted"
  while `-U file` is the password file alone; peachq has it exactly the other way round. Unpinned: a pin needs a
  multi-process qscript case (a remote `read0`/`system` under each flag).
- **`\x name` does not expunge (2026-08-12)** — `\x .z.ts` is silently a no-op, so an assigned `.z` callback
  survives it. Pinned red-below-floor by the syscmd x.qcmd suite.
- **`count \a` throws `'parse`, not `'\` (2026-08-12)** — the kx system reference gives `'\` for a `\`-command used
  as an operand. Pinned red-below-floor by the syscmd system.qcmd suite.
- **`\E` has no setter (2026-08-04)** — the kx page documents `\E` as display-only, so `\E 2` is the silent no-op
  every other display-only `\`-command uses rather than a runtime mode change. If kdb turns out to accept a setter,
  it is a one-line change in the `\E` handler.
