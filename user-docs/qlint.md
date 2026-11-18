# qlint: linting q with peachq

> DRAFT 2026-11-17, the target outcome for the qlint programme (`feat/qlint`). It describes where we are going, not
> what ships today; each section says which stage delivers it. Use it to check that what we build is heading here.

peachq's linter reads q with the same parser peachq runs, so it never misreads code that peachq would run. It judges
kdb's grammar: peachq's own friendly syntax (a trailing comma in a qSQL phrase, `select a, from t`) is a SYNTAX
finding, whether or not your session has `\classic` on. **It never runs your code.** The only thing it evaluates is literal-only pieces
built from pure built-ins (like `6+"hi"`), under `reval`, which blocks writes. One engine serves the command line,
the q session, qStudio, VS Code and any editor that speaks LSP.

## Quick start

```
$ q -lint code/
code/feed.q:12:5: error COND_EVENARGS $[] with 4 arguments has no catch-all else: when no test is true there is no defined result
code/feed.q:40:1: error SYNTAX missing }
2 findings (2 errors) in 14 files
$ echo $?
1
```

`q -lint` takes files and directories (every `.q` file below a directory; a directory with none is `0 findings in 0
files`, exit 0). Findings go to stdout; a notice, a path it cannot read and a usage error, which names the flag or
value that was wrong, go to stderr. `q -lint -help` (or `--help`, `-h`) prints the usage to stdout and exits 0.

Exit code: 0 clean, 1 an `error` finding, 2 an input it cannot read. 2 wins over 1: a run that could not read
everything is incomplete, whatever it found. A missing path, or a file it cannot open, is named on stderr and the rest
are still linted; a `lint.config` it cannot open stops the run: `q -lint: cannot read <path>/lint.config`, no findings, exit 2.

`-format json` prints one JSON array, an object per finding (`file`, then the `.lint.text` columns), `[]` when
clean. `-format sarif` prints SARIF 2.1.0, what GitHub code scanning reads; its columns count UTF-16 code units, where
the text and JSON columns count bytes.

`q -lint` runs none of your code: no `QINIT`, no startup script, no `-eval`, and only the standard library built into
peachq (a `pq/` folder in the tree or under `QHOME` is never loaded). Any other flag is a usage error: there are no
rule switches on the command line, so to turn on an opt-in rule such as `UNUSED_PARAM`, or turn one off, write a
`lint.config` (see Configuration).

In a q session (stage 4):

```q
q).lint.text "f:{[a;b] a+1}\ng:$[1b;2;0b;3]"
line col endline endcol rule          severity msg
-----------------------------------------------------------------------------------------------------
2    3   2       14     COND_EVENARGS error    "$[] with 4 arguments has no catch-all else: when no ..
q).lint.path `:src
q).lint.rules[]
q).lint.explain `COND_EVENARGS
```

**A string is always source and a symbol is always a path.** `.lint.text "code/a.q"` lints the eight characters
`code/a.q` as q, not the file; write `` .lint.path `:code/a.q ``. Lint never guesses which you meant from the text, so
`.lint.text` refuses a symbol and `.lint.path` a string, each with a message saying which to use, and
`.lint.run[target;opts]` takes either, by type. A path that does not exist is an error naming it.
`.lint.explain` of a rule peachq does not have says whether it is a KX QLint rule not implemented yet or not a rule.

## Editors

- **qStudio:** built in; qStudio bundles peachq and shows findings in the Problems panel and the gutter.
- **VS Code:** the peachq extension shows findings as you type.
- **Anything else:** `q -lsp` is a language server over stdin and stdout. Point the editor's LSP client at the
  command `q -lsp` for q files. It runs none of your code.

The language server is the same linter: each open document is linted whole on every change, with the
`lint.config` files above the document's folder (a document that is not a file uses the defaults), and the findings
appear as diagnostics with the rule ID as their code and `qlint` as their source.

It also knows your project. Every `.q` file under the editor's workspace folders is read when the editor starts (in
the background: the editor answers at once), and every open document counts as it stands, saved or not. From that it
offers:

- **Outline**: every definition, grouped by `\d` namespace, with its signature.
- **Go to definition** and **find references**, across files: a name in a function's own parameters and locals stays in
  that function, since q has no closures.
- **Hover**: the signature, with any type checks (``add[x:`j; y]``), and the comment above the definition; for a
  built-in, its help.
- **Signature help** inside `f[…]`, naming the parameter you are on.
- **Completion**: the function's parameters and locals, your definitions, keywords, the members of a namespace after
  `.ns.` (yours, and `.q .Q .z .h .j` and peachq's), and a table's columns inside `select … from t` when the file
  defines the table. The nearest scope comes first: the function's own names, then this file's, then the rest of the
  workspace's, then the built-ins. Nothing is offered inside a comment or a string.

Nothing you write is ever run to answer these. A name built from a string (`value "f 1"`) is invisible to them.
Rename and quick fixes are not offered yet. Folders and files whose name starts with `.` are skipped; the editor can
exclude more (`initializationOptions.exclude`, see the integration guide). Positions use the first encoding the editor lists that peachq speaks,
UTF-8 bytes or UTF-16 code units, else UTF-16, the protocol's default. An editor built on VS Code's
`vscode-languageclient` accepts UTF-16 alone, so its client must list only `utf-16` (or none).

Changes that arrive while a lint runs are not linted one by one: the server lints each document at the newest version
waiting, so findings never lag behind a burst of typing. A `lint.config` it cannot read, or one that is a directory, is
skipped with a notice on stderr naming it, and the other files of the chain still apply; a config's notices print
once, and again only when the file changes. Memory stays level over a long session; only a name no earlier edit used
adds a few hundred bytes.

## Rules

Rule IDs are KX QLint's, so existing suppressions keep working; PARAM_TYPE_MISMATCH and UNDEFINED_REFERENCE are qStudio's, and SYNTAX,
LITERAL_ERROR, TYPED_PARAM_ERROR and the hazard rules are peachq's own. On by default: errors, correctness and hazards. Style rules are opt-in.

| rule | severity | default | finds |
|---|---|---|---|
| SYNTAX | error | on | code q cannot parse, at the exact position; a named error such as `'params` is said in words |
| ASSIGN_RESERVED_WORD | error | on | an assignment to a reserved word, `cos:12`, which q refuses with `'assign` |
| COND_EVENARGS | error | on | cond with 4, 6 or more arguments, `$[a;b;c;d]`: no catch-all else, so no defined result when no test is true |
| LITERAL_ERROR | error | on | literal-only code that is sure to fail, e.g. `` `a+1 ``, or a literal dict or table whose lengths differ, `` `a`b!1 2 3 `` |
| TOO_MANY_ARGUMENTS | error | on | a call with more arguments than the function takes, `count[x;y]`: `'rank` when it runs |
| UNUSED_VAR | warning | on | a local assigned in a function and never read there; when a later statement assigns it again, that line |
| DEPRECATED_DATETIME | warning | on | a datetime literal (`0Nz`, `2001.01.01T12:00`), `.z.z` or `.z.Z`, or a cast to datetime (`` `datetime$ ``, `"z"$`) |
| UNUSED_PARAM | info | off | a parameter the body never reads (`b.c` is a global, not `b`) |
| NOT_FILE_SYMBOL | warning | on | hazard: `` not `:f ``, which deletes the file `f` |
| TIME_OF_DAY_COMPARE | warning | on | hazard: a timestamp or datetime compared with `.z.t`, which compares only the time of day |
| COND_TWO_ARG_IF | warning | on | hazard: `$[b;'err]` or `$[x>0;y]` written as an if; with two arguments `$` is cast or tok |
| NESTED_LAMBDA_OUTER_READ | warning | on | hazard: a nested function reading a local of the one around it, `{[a] {x+a} each 1 2}`; q has no closures, so `a` there is a global |
| WHERE_SELF_COMPARE | warning | on | hazard: `where c=c` (or `~`, `in`) on a local `c`: the column shadows the local, so every row matches |
| UNDECLARED_VAR | warning | off | a name a function reads that is not its local and that the file never defines, so it is a global |
| DUPLICATE_COLUMN | warning | on | hazard: a column named twice in one phrase, `select a,b,a from t` |
| TRAP_EAGER_CALL | warning | on | hazard: `.Q.trp[f x;y;h]` or `@[f x;y;h]`: `f x` runs before the trap, so its error is not caught |
| COND_SAME_BRANCHES | warning | on | hazard: `$[c;a;a]`, every branch the same expression |
| EMPTY_IF | warning | on | `if[test]` with nothing to run, usually a missing `;` (`if[x :1]` reads the return as part of the test) |
| ENGINE_UNSUPPORTED | info | on | valid q peachq's parser cannot read yet (`'nyi`), such as `{:[;x]}`: not your bug, but its statement is not checked |
| MALFORMED_SUPPRESSION | warning | on | an `@qlintsuppress` that suppresses nothing or less than it says: no such rule, lower case, `A B`, `A, B`, a bad count |
| PARAM_TYPE_MISMATCH | error | on | a literal that fails a type-checked parameter, ``f:{[x:`j] x}; f[1i]``, or a pattern assignment, `` (a:`j;b):(1.5;2) ``: `'type` when it runs |
| TYPED_PARAM_ERROR | error | on | code over typed parameters that fails whatever their values, ``{[x:`j] x+"a"}``: `'type` when it runs |
| UNDEFINED_REFERENCE | error | off | a member of a known namespace that nothing defines, `.Q.gg[]` or `.my.sned` beside `.my.send:{…}`: it signals when it runs |

The hazard rules are peachq's own: valid q that does something you probably did not mean. Each fires only on a
shape it is sure of, so it can stay on.

ASSIGN_RESERVED_WORD is reported instead of SYNTAX for the same statement, never as well; switched off, the statement
is reported as SYNTAX, because it still cannot load. TOO_MANY_ARGUMENTS judges only a function whose ranks it knows
for certain: a built-in, or a lambda the file assigns once, at top level, to a name nothing else in the file assigns
or names as a symbol, read in its `\d` context; a file that may change context in a way lint cannot follow
(`system` of a computed string, `value "\\d .a"`) gets no lambda check. A local of the same name, cond `$[…]` and
`sum`, `prd`, `max` and `min` are never judged. Fewer arguments is a projection and never reported. UNUSED_VAR counts
a compound assignment (`c,:1`), an amend (`c[0]:1`), a symbol naming the local, a read before the assignment, and
returning it (`c:1` as the last expression) as reads; qSQL and table columns (`select c:a from t`, `([] c:1)`) are not
locals.

PARAM_TYPE_MISMATCH and TYPED_PARAM_ERROR check q's type patterns (`` x:`j `` takes a long atom, `` x:`J `` a long
list, and the check is exact: `1i` is not a long) before the code runs. PARAM_TYPE_MISMATCH judges a literal
argument, `f[1i]` or `f 1i`, of a function it is certain of, the same functions TOO_MANY_ARGUMENTS judges or a lambda
written in place, called with all its arguments (`f[1i;]` is a projection and never judged); a list pattern of the
wrong length is reported too. The literal is checked by q's own pattern match: the function never runs.
TYPED_PARAM_ERROR checks each piece of a typed function's body made of its typed parameters, literals and operators
whose result type follows from their arguments' types (`+`, `<`, `neg`, ...), with each parameter replaced by an
empty value of its type, a null atom or, for a list, both an empty and a one-item list, and reports a piece only when
every one signals `'type`; an error such as `'length` depends on the values and is never reported. A parameter the
function assigns or its signature binds twice, and names in a qSQL query, are not judged.

COND_EVENARGS keeps KX's meaning: "Conditional $ should not be used with an even number of arguments". Two arguments,
`$["i";2.3]`, is the dyadic `$` (cast or tok), and `$[;x]` or `$[c;]` is a projection of `$`; neither is cond, so
neither is reported. Cond itself cannot be projected, so `$[a;b;c;]` is cond with four arguments and is reported.

UNUSED_PARAM is off by default, and `info` when on, where KX QLint makes it a warning: callbacks, trap handlers, stubs
and protocol signatures legitimately take parameters they ignore, and removing one changes the arity its caller relies
on. Turn it on in `lint.config` to hunt bugs (a body that reads the wrong name), and silence a parameter a caller
requires with `/ @qlintsuppress UNUSED_PARAM(0)` on its line.

UNDEFINED_REFERENCE is off by default for now; turn it on in `lint.config` (`UNDEFINED_REFERENCE : true`) or opts, and
it reports at error. It judges only a known namespace: one q provides (`.q .Q .z .h .j`, peachq's library, `.lint`), whose
members the linter lists itself, or one your project assigns a function into at top level (`.my.send:{…}`, or `\d .my`
and a definition), in any file linted with it. A member assigned anywhere is defined, as is a `.z` callback you may
define (`.z.pg`, `.z.ts`), a member named by a symbol (`` `.my.x set 1 ``, sent in a message), a member a test asks for
(`` `x in key `.my ``) and a member written in a string. It leaves alone a namespace you only give values (another
library's settings), one assigned whole or whose names you build (`` ` sv `.my,x ``), one more files read missing
members of than define functions in (a library you extend, such as a test framework's), and a name in the `globals`
option. Lint a folder, not one file, or the members other files define are missing. Under `\d .a` an undotted name is
`.a.name`, as q reads it, never the root's (only a `.q` function is seen from every namespace), and every rule reads
names that way.

NESTED_LAMBDA_OUTER_READ and UNDECLARED_VAR leave alone a name in a qSQL query (it may be a column, and a translator
may read the lambda as text), a dotted name for the first, and a name defined as a global (by the file for the first,
by any file of the project for the second): assigned at top level, with `::` or as a dotted name, or a symbol that `set`, `upsert` or `insert` names, that a name follows (as an
infix `.q` function such as qspec's `mock` takes it: lint assumes any name there may be one), or that `@[` may amend.
UNDECLARED_VAR is off by default: most code reads globals that other files define. Lint a folder (`q -lint src`,
`` .lint.path `:src ``) and every file in it counts as one project, a name any of them defines being defined for all;
in a session the `globals` option of `.lint.run` adds names defined anywhere else.
It skips KX's one-letter namespaces (`.z`, `.Q`), a namespace the file never defines into, and a file whose `\d`
context it cannot follow. TRAP_EAGER_CALL is reported only when `f` is a function of the file
called with all its arguments whose result is surely not a function (it ends with `;` and returns early only
constants), since a call that returns a function is a fine first argument.

For an unclosed bracket, SYNTAX points at the bracket left open. The parser matches each closer to the innermost open
bracket; when a closer alone on its line (a `;` or a comment may follow) lines up with an outer bracket's line rather
than with its own opener's, the inner bracket is the one reported:

```q
f:{[c;h;exc]
    $[`gwReqId in key c;
        h[`asend;(`gw.result;c)];
        h[`asend;(`EXCEPTION;exc)      / reported here: missing ]
    ];
 };
```

Planned next, by evidence from real code: the rest of the KX QLint set that applies outside KX Developer
(`DECLARED_AFTER_USE`, `INVALID_ESCAPE`, `STATEMENT_IN_EXPR`, …) and more hazard rules.

## Suppressing a finding

```q
/ @qlintsuppress UNUSED_PARAM          / this whole file
/ @qlintsuppress COND_EVENARGS(1)      / the next line only
/ @qlintsuppress all                   / every rule in this file
```

The same comments KX QLint and qStudio read. `/` or `//`, with or without a space. One rule per comment: write a
comment for each rule, since `A B` and `A, B` are a MALFORMED_SUPPRESSION finding, as is a rule that does not exist.
Words after the rule that are not rule IDs, a reason, are fine.

## Configuration

KX's `lint.config`: one rule per line.

```
COND_EVENARGS        : false
UNUSED_PARAM         : true, info
UNDECLARED_VAR       : true, warning { globals : [ "abc", "def" ] }
```

- `true` runs the rule, `false` does not; a severity after the comma (`error`, `warning`, `info`) runs it at that
  severity. Only an `error` finding makes `q -lint` exit 1.
- A file applies to its folder and every folder below it. For each rule the nearest file that sets it wins, so a
  child folder's `lint.config` overrides one rule and keeps its parent's other settings.
- The search climbs no higher than your repository: it stops at the nearest folder holding `.git` (a folder, or the
  file a worktree has), and a `lint.config` in that folder still applies. Outside a repository it climbs to the
  filesystem root.
- A `{ … }` parameter object is not applied (the text after `{` is skipped unchecked), with a notice on stderr naming
  the file and line; the rest of the line still applies. A line it cannot read gets the same kind of notice and is
  skipped. A rule peachq does not have is ignored, so a config written for KX QLint works here.
- There are no rule switches on the command line; a config file is how you choose.

In a session `.lint.path` reads the same files, and `.lint.run[target;opts]` takes the same settings as a dictionary,
`rule!1b`, `0b` or a severity, applied over the files. A rule it does not have is ignored with a notice. Two more keys:
`positions`, `` `utf16 `` for columns in UTF-16 code units rather than bytes, and `globals`, a symbol list of names
defined elsewhere.

## Not yet

- A call to a function defined in another file (TOO_MANY_ARGUMENTS and PARAM_TYPE_MISMATCH across files): v2.
- The pattern conditional `:[v;r:`f;…;d]`, once peachq runs it.
- Autofix.
- Linting a loaded namespace (KX's `.qlint.lintNS`).

## Open

- Do we provide KX's `.qlint.*` q API for code that calls it (e.g. reQ's `lint.q`)?
- Where are the remaining KX QLint rules worth it, and in which order?
