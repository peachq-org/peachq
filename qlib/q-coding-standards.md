# q coding standards

**Scope: every `.q` file we ship — `qlib/` (portable q) and `lib/` (q that calls the peachq C
surface) alike.** The two tiers differ in what they may call, not in how they are written: the
naming, namespace, semicolon and qdoc rules below apply identically to both.

Adapted from the repo owner's published guide,
[timestored.com/kdb-guides/q-coding-standards](https://www.timestored.com/kdb-guides/q-coding-standards).
Four rules below deliberately DEPART from that source; each says so and says why.

## Naming

**snake_case, lowercase, for functions and locals** — `.regexp.find_matches`, `match_count`.
**DEPARTURE 1** (source says camelCase, and bans underscores as "also a kdb operator"). We match
Python and DuckDB, the two languages our users arrive from. The operator objection is void:
verified on peachq, `find_matches:{x+1}` scans as ONE identifier and `2_til 5` still drops — the
only cost is that Drop between two *names* needs spacing (`a_b` is an identifier; `a _ b` drops),
which is how you would space a binary operator anyway.

| Scope | Entity | Convention | Example |
|---|---|---|---|
| Namespace | — | short, lowercase, no underscore | `.regexp` |
| Global | variable | leading uppercase | `Counter` |
| Global | constant | ALL CAPS | `EMPTYAR` |
| Any | function | snake_case, verb-first | `find_matches`, `replace_all` |
| Local | variable | snake_case; single letters only where the scope is a line or two | `c`, `row_count` |

- Periods are namespace separators. Never put one inside a name.
- `x`, `y`, `z` are for implicit arguments only. Name the parameters of anything else.
- Column names: no reserved words, no spaces.

## Namespaces

**Prefer fully-qualified `.a.b:` assignment over `\d .a` (or `system "d .a"`). Both are allowed** —
**DEPARTURE 2** from the source, and a marginal preference, but the reason is functional rather
than stylistic: **`\d` does not survive IPC.** Binding over a handle ignores the `\d` context and
`\l` is not processed remotely, so a file organised with `\d` cannot be pushed down a connection.
A fully-qualified file loads either way.

**Private functions live in an `.i.` sub-namespace** — `.duckdb.i.open`, `.regexp.i.extract`.
Established practice, not a new invention: `lib/duckdb.q` and `lib/regexp.q` both do it. Everything
NOT under `.i.` is public API and carries the compatibility promise that implies.

## Functions

- **One statement per line.** Indent each statement block and control sequence.
- **20 lines is the ceiling.** Longer means refactor.
- **`};` after the closing brace of a definition** — `.ns.f:{[x] a:x*2; a};` — so a multi-line
  definition can be loaded over IPC. **DEPARTURE 4** in its precision, not its intent: the source
  says "always place semicolon after function definition" and its own examples show exactly this
  shape, so we spell the boundary out. NOT after every line, and NEVER after the final expression
  inside the braces: `{a;b;c;}` returns generic null, which would silently break a qunit test,
  whose last line returns the value being asserted on.
- **Name the parameters** whenever there is more than one, or the meaning is not obvious.
- **Projections carry every semicolon**: `f[;2;]`, not `f[;2]`.
- Assignments that are not alone on their line get their own line.

## Doc comments

**Every public function gets a doc comment, and that comment IS its documentation.** peachq renders
a lambda from its verbatim source, so the lines above a definition and its parameter names are the
first thing a user ever reads.

**DEPARTURE 3:** the source requires a tag per parameter and per return. We do not. Document what a
caller cannot guess — a trap, a constraint, a surprising return type — and skip the obvious rather
than pad to a template. One honest line beats five generated ones.

- A plain `/` comment line above the definition is the whole form. **No `###` or `#####` headings**,
  no banner rules.
- **Only tags the reference parser knows**, in field order: `@param <name> (<type>) <description>`,
  then `@returns`, `@throws`, `@see`. Never invent a tag — a tool cannot read it.
- **Types are the kdb type name, all lowercase**: `(int)`, `(long)`, `(symbol)`, `(symbol list)`,
  `(char)`, `(string)`, `(list)`, `(any)`. `(string)` for a char vector, never `(char list)`.
  **No CamelCase**: `(Integer)`, `(SymbolList)`, `(Dict)` are all out.
- **`(any)` unless the type is TRULY limited.** A narrower type that is really "any" is a lie a
  caller will trust. Limited means the code genuinely rejects everything else — go read it before
  you enumerate. Worked example: `.ffi.bind`'s `returntype` is `(char)`, not `(string)`, because
  `enlist "f"` is `'type`; but its `arglist` is `(any)`, because it really does take anything.

## Comments in general

- Write a comment for what the code cannot say: a non-obvious *why*, a constraint, a bug it must
  not regress into. Never restate what a line does.
- **No numeric budget** — no percentage, no maximum line count. The rule is simply that a file
  never carries more comment lines than code lines.
- Avoid standalone `/` comment blocks mid-body; `//` and end-of-line `/` are free.

## Formatting

- **Line width 120 columns, up to 140 where it reads better.** Applies to `.q` and `.md` alike.
  Never reflow to 80 — that is a 1980s terminal constraint, and it shreds tables and long `@param`
  lines. Existing 80-column files are not a standard to match; widen them when you touch them.
- Prefer q's own idiom: adverbs over loops, and right-to-left evaluation to shed brackets — but
  only while it stays readable. Clarity outranks brevity.

## Errors

Signal a bare error class and let it propagate — `'type`, `'length`, `'domain`. No per-function
message strings, no wrapping, no re-classification.

## Loading

Every file must be **inert on load** (definitions only at top level) and **safe to reload**. In
`qlib/src/` this is enforced by the auto-loader; in `lib/` it is the ANY-ORDER LAW, gate-enforced.
A file that does work at load time cannot be reloaded and cannot be reordered.
