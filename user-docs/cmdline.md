# The command line

This page is the one home for how you launch peachq: every option the `q` binary takes, how each compares with kx q,
and the two peachq-only flags for running q text at startup. The system commands you type once you are inside a
session have their own page: [System commands](syscmds.md).

The general shape is kx's:

```bash
q [file.q] [-option [parameters] ...]
```

The first token ending in `.q` is the startup script; everything after it that q does not consume for itself is
yours to read back as `.z.x` (`.z.X` keeps the raw command line). The flags that work exactly as kx documents them
today are `-e -E -p -q -z`; `-c` is applied but not yet removed from `.z.x`, and `-u`/`-U` are applied but with
their meanings swapped against kdb (a known defect). Everything else is in the table below.

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

`-q`, `-L`, `-m`, `-eval` and `-eval-before` have no system-command equivalent.

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
- **An error aborts.** The text stops at the first erroring statement, everything after it is skipped — the startup
  script and the session loop included — and the process exits non-zero, exactly like a failing startup script.

**Order is named by the flag, not by argv position.** Every `-eval-before` runs before the startup script and every
`-eval` after it; repeats of the same flag run left-to-right. So `q s.q -eval "A" -eval-before "B"` runs B, then
s.q, then A.

After the texts run, the session does what it always does: an interactive terminal drops to the `q)` prompt, a
non-terminal stdin exits 0, and a live listener serves. `-eval` does not imply "exit after" — end the text with
`exit 0` if that is what you want.

Both flags are consumed by the launcher, so neither the flag nor its text appears in `.z.x`.

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
