# The REPL

What you see and can do when you run `q` at a terminal.

## Prompts

- **`q)`** — the normal prompt. After `\d .foo` it shows the context: `q.foo)`.
- **`q))`** — the debugger. An error inside a function suspends there instead of returning to `q)`, with the
  failing frame shown; each extra `)` is one more level of suspension. At `q))` you can look at the function's
  local variables, and:
  - `\` leaves one level (back to `q)` from `q))`);
  - `:` resumes, and `:value` resumes with that value as the result;
  - `'text` signals a new error from here;
  - `&` shows where you are again; `` ` `` and `.` move up and down the call stack;
  - `\\` quits q altogether.

## Keys

| Key | Does |
|---|---|
| Up / Down (or Ctrl-P / Ctrl-N) | step back and forward through history |
| Ctrl-R | search history backwards as you type; Ctrl-R again finds an older match, Enter runs it, Tab or Esc keeps it for editing, Ctrl-C cancels |
| Left / Right | move one character |
| Ctrl-Left / Ctrl-Right (or Alt-B / Alt-F) | move one word |
| Home / End (or Ctrl-A / Ctrl-E) | start / end of the line |
| Tab | complete the name under the cursor; press again to cycle through candidates |
| Tab or Right at the end of the line | accept the grey hint |
| Ctrl-W / Ctrl-U / Ctrl-K | delete the word before the cursor / to the start / to the end |
| Ctrl-L | clear the screen |
| Ctrl-C | while a line runs: interrupt it (`'stop`); at the prompt: clear the line (on Windows, at an empty prompt: quit) |
| Ctrl-D | at an empty prompt: end the session (a process serving a port keeps its console) |
| `\\` then Enter | quit |

## Help

- `\?` opens the help; `\?name` shows the entry for `name`.
- Type a single name, such as `til`, and the next prompt offers a grey hint: `\?til / <one-line summary>`. Tab or
  Right accepts the `\?til` part.
- In a first session with no history yet, the empty prompt shows `\? / help`.

## History

Every line you enter at a terminal is saved to `~/.qhist` (or `./.qhist` when `HOME` is unset), duplicates included.
Lines piped into `q` are not saved. Each record is one line: the time, the status (`ok`, the error, or `exit`), how
many milliseconds it ran, and the line itself, ending with `NSL888`. Lines typed at `q))` are saved too; the line
that opened `q))` is saved when it finishes, with its error:

```
2026.09.23D16:05:20.101	'type	0	1+`aNSL888
```

Up-arrow and Ctrl-R reach the newest 5000 lines.

To read it as a table:

```q
q)h:{flip`time`status`ms`query!("P"$x 0;`$x 1;"J"$x 2;-6_'x 3)}flip{(3#v),enlist"\t"sv 3_v:"\t"vs x}each 1_read0 hsym`$getenv[`HOME],"/.qhist"
```

If none of your lines contain a tab, this is shorter:

```q
q)h:update -6_'query from("PSJ*";enlist"\t")0:hsym`$getenv[`HOME],"/.qhist"
```

A history in the older format is converted the first time a new `q` starts; the original is kept once as
`~/.qhist.bak`, and its lines show an unknown time, status and duration.

History is best-effort: if the file can't be read or written (disk full, permissions, a slow mount), q carries on
without it.

Results of `q -conn` calls are kept separately, under `~/.qhist.d/` — see [the command line](cmdline.md).
