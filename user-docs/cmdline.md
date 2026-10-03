# The command line

This page is the one home for how you launch peachq: every option the `q` binary takes, how each compares with kx q,
the peachq-only flags for running q text at startup, and the `-conn` mode that runs it on another process. The system commands you type once you are inside a
session have their own page: [System commands](syscmds.md).

The general shape is kx's:

```bash
q [file] [-option [parameters] ...]
```

**The file is the first argument**, when it does not start with `-`, and nothing else ever is: `q -P 3 t.q` loads
nothing and hands `t.q` to your code. It is loaded exactly as `\l file` would load it — a script, `\l trade` finding
`trade.q`, a directory — and `.z.f` is the name as you typed it (`` `trade ``).

**Every option takes the parameter count kx documents** (`-c r c` takes two, `-q` none, `-e 0|1|2` one) and is
consumed with them; everything else is yours to read back as `.z.x`, with or without a file (`q -abc 123` gives
`("-abc";"123")`; `.z.X` keeps the raw command line). An option whose `\` command exists is **applied through that
command** at startup, before the file loads — `-P 9` is `\P 9` — so it behaves exactly as the command does.

**A missing or invalid option value stops q before it starts**, on stderr with exit code 2:
`q: -c needs 2 parameters`, `q: invalid -e value '7' (expected 0|1|2)`, `q: invalid -P value 'x'`. The options whose
syntax lists their values (`-e -E -g -z`) accept only those; the others accept whatever their `\` command accepts.

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
a path that does not exist is a `cannot open script` error. Unset or empty,
nothing happens — peachq ships its own `q.q` as part of the bootstrap, so there is no `$QHOME/q.q` default to fall
back to. `.z.v` reports the value.

## Every option

The **status** column is the point of this table: **applied** = q consumes the option and acts on it (through the
`\` command in the last column, or in the launcher where there is none), **consumed only** = q consumes it with its
parameters, so it never reaches `.z.x`, but it has no effect yet, **peachq only** = does not exist in kx q. The
**applied as** column names the system command the option is applied through, which also reads it back at runtime —
and where a `\`-command merely shares the letter, it says so instead.

| Flag | What it does | Status | Applied as |
|---|---|---|---|
| `-b` | block client write-access | **consumed only** | none — the reader is `\_`, which is not implemented |
| `-c r c` | console size (rows, columns) | **applied** | `\c r c` |
| `-C r c` | HTTP display size | **applied** | `\C r c` |
| `-e 0\|1\|2` | error-trap mode for client evals | **applied** | `\e` |
| `-E 0\|1\|2` | TLS server mode | **applied** (launcher, before any listener) | none — `\E` only reads it |
| `-g 0\|1` | garbage-collection mode | **applied** | `\g` |
| `-l` | log updates to a file | **consumed only** | none — `\l` **loads a file**, same letter only |
| `-L` | as `-l`, synchronous | **consumed only** | none |
| `-m path` | memory domain | **consumed only** | none |
| `-o N` | offset from UTC (hours) | **applied** | `\o` |
| `-p N` | listen on port N; `0W` binds an OS-chosen free port | **applied** (launcher) | none — `\p` changes it at runtime |
| `-P N` | float display precision | **applied** | `\P` |
| `-q` | quiet mode: no startup banner; `.z.q` reads it back | **applied** (launcher) | none |
| `-r :h:p` | replicate from a primary | **consumed only** | none |
| `-s N` | secondary threads | **applied** | `\s` |
| `-S N` | random seed | **applied** | `\S` |
| `-t N` | timer period (ms) | **applied** | `\t` |
| `-T N` | client query timeout (s) | **consumed only** | none — `\T` is not implemented |
| `-u 1` or `-u file` | restrict client evals; with a file, also the password file | **applied** (launcher; see the `-u`/`-U` note) | `\u` **reloads** the file at runtime |
| `-U file` | password file | **applied** (launcher) | none |
| `-w N` | workspace memory **limit** (MB) | **consumed only** | none — `\w` reports memory **usage**, same letter only |
| `-W N` | start-of-week offset | **applied** | `\W` |
| `-z 0\|1` | date parse order (`"D"$`): 0 = mdy, 1 = dmy | **applied** | `\z` |
| `--port N` | the long spelling of `-p` | **peachq only** | none |
| `-classic` | legacy kx table display, kdb-clean environment | **peachq only** | `\classic` |
| `-eval "src"` | run q text **after** the startup script | **peachq only** | none |
| `-eval-before "src"` | run q text **before** the startup script | **peachq only** | none |
| `-duckdb path` | the main DuckDB database is this file, not memory; its tables load at startup (implies `\l pq`) | **peachq only** | none |
| `-conn target` | run the file, `-eval` texts and stdin on a RUNNING q at any `hopen` target — a mode of its own, see below | **peachq only** | none |
| `-save file` | with `-conn`: `set` the last result to this file, its ending choosing the format | **peachq only** | none |
| `-ls` | with `-conn`: list the server's variables, tables first | **peachq only** | none |

`-h` / `--help` prints this table and exits when no file is named; after a file it is the file's own argument, in
`.z.x`. Options are applied in the order they appear, after the display defaults (so `-c` wins over them) and before
`-eval-before`, the file and `-eval`.

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
q s.q -eval-before "opts:`fast"    # opts is bound before s.q loads
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
non-terminal stdin is read as console input to its end and then exits 0, and a live listener serves. `-eval` does
not imply "exit after" — end the text with `exit 0` if that is what you want. So `echo 'a+1' | q s.q` evaluates
`a+1` after s.q; `q s.q </dev/null` runs s.q and exits at once.

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
q setup.q -conn :localhost:5000                   # the file's text is one call, then each -eval
echo "count trade" | q -conn :localhost:5000       # stdin is read only with no file and no -eval
q -conn :localhost:5000 -save out.parquet -eval 'trade'
q -conn :localhost:5000 -ls
```

- **Target.** Anything `hopen` takes, passed through as written: all digits is a port (`-conn 5000`), anything
  else the symbol — `:localhost:5000`, `::5000`, `:host:port:user:pass`, `:unix://…`, `:tcps://…` — with or
  without the leading backtick. Nothing is checked or added here: a target that opens but is
  not a q process fails at the first call, with that call's error. The connection uses a 5-second timeout; a
  refusal is `q: cannot connect to <target>: <hopen's error>`, exit 2.
- **Ordering** is the local process's: the file's whole text as ONE call, then each `-eval` text as one call.
  Piped stdin is read, one call per line (blank lines run nothing), only when there is neither a file nor an
  `-eval`: with either, stdin is left alone, so a run from cron or a pipeline never waits on it. Multi-line text —
  a file, or an `-eval` with newlines — runs as a script on the server: statement by statement, the last
  statement's value comes back.
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
- **`-b -T -w -l -L -r -m` are consumed but not applied** — none has a working `\` command to apply it through
  (`\_` and `\T` answer `'nyi`), so the cmdline/options rows for `system "_"` and `system "T"` stay red. `\e \g \W`
  answer int by owner ruling (2026-09-24).
- **`\l` text with a space in the name** — the file loads through the loader `\l` uses, handed the argv token whole,
  so `q 'my file.q'` works; how `\l my file.q` itself should be spelled is unpinned (defects.tsv Dqzvnk, a ruling).
  On a terminal the file is still primed as a `\l` line, so a spaced name there waits on the same ruling.
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
