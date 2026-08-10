# Regular expressions

> **Status: implemented.** Every example here is pinned as a test row in
> `test/q/aigenerated/regex_rlike.qcmd` (and the version pin in `regex_version.qcmd`), both
> green. If an example here and the code ever disagree, this document and those ledgers are
> the specification.

peachq matches with [RE2](https://github.com/google/re2), the same engine DuckDB uses. RE2
guarantees linear-time matching, so no pattern — however hostile or badly written — can hang a
query the way a backtracking engine can. It is also what makes a peachq predicate and the
equivalent DuckDB SQL predicate mean the same thing, which matters once a query pushes down to
DuckDB storage.

Regex is a peachq extension: standard q has no regular expressions, only glob-style `like` and
the `ss`/`ssr` search verbs. Nothing here changes those.

## Loading

The `.regexp` namespace is part of the standard library:

```q
q)\l pq
```

The `rlike` operator is a language keyword and needs no load.

## `rlike` — the predicate

`rlike` is an infix operator that answers "does this subject contain a match?".

```q
q)"abc" rlike "b"
1b
q)"abc" rlike "^b"
0b
q)`AAPL`MSFT`IBM rlike "^[AM]"
110b
```

It reads naturally in a `where` clause, which is where regex matching mostly lives:

```q
q)select from trade where sym rlike "^[A-C]"
```

`rlike` searches anywhere in the subject unless you anchor with `^` or `$`. That is the
universal meaning of the name (MySQL, Hive, Spark and SQLite all use `RLIKE`/`REGEXP` this
way).

## Function reference

Every function takes a fixed number of arguments, and the names below are literally the
parameter names — typing a function's name prints its signature.

| function | returns |
|---|---|
| `.regexp.matches[subject;pattern]` | boolean — match anywhere |
| `.regexp.full_match[subject;pattern]` | boolean — the *entire* subject must match |
| `.regexp.extract[subject;pattern]` | the matched text, `""` if no match |
| `.regexp.groups[subject;pattern]` | the first match's capture groups, `()` if no match |
| `.regexp.groups_all[subject;pattern]` | one group-list per match |
| `.regexp.extract_all[subject;pattern]` | every non-overlapping match |
| `.regexp.replace[subject;pattern;replacement]` | the first match replaced |
| `.regexp.replace_all[subject;pattern;replacement]` | every match replaced |
| `.regexp.split[subject;pattern]` | subject split on the pattern |
| `.regexp.escape[text]` | the pattern that matches `text` literally |
| `.regexp.version` | the vendored RE2's version pin |

```q
q).regexp.matches
{[subject;pattern] .regexp.i.test[subject;pattern;0b]}
```

Coming from DuckDB SQL, swap the first `_` for a `.`: `regexp_matches` is `.regexp.matches`,
`regexp_full_match` is `.regexp.full_match`, `regexp_extract_all` is `.regexp.extract_all`.

### Matching

```q
q).regexp.matches["abc";"b"]
1b
q).regexp.full_match["abc";"b"]
0b
q).regexp.full_match["abc";"a.c"]
1b
```

`.regexp.matches` is `rlike` spelled as a function.

### Extracting

`.regexp.extract` gives the matched text; `.regexp.groups` gives the capture groups:

```q
q).regexp.extract["2026-08-10";"[0-9]{4}"]
"2026"
q).regexp.groups["2026-08-10";"([0-9]{4})-([0-9]{2})"]
"2026"
"08"
```

There is no group-number argument, because q can index a result. Take the group you want, or
name them all at once with a plain dict construction:

```q
q)first .regexp.groups["2026-08-10";"([0-9]{4})-([0-9]{2})"]
"2026"
q)`year`month!.regexp.groups["2026-08-10";"([0-9]{4})-([0-9]{2})"]
year | "2026"
month| "08"
```

A capture group that did not participate in the match reads as `""`. There is no limit on how
many groups a pattern may have — the `\1`-`\9` limit below is about the *replacement* string,
not the pattern.

`.regexp.extract_all` returns every match rather than the first:

```q
q).regexp.extract_all["a1b22c";"[0-9]+"]
,"1"
"22"
```

A one-character match displays as `,"1"` because it is a one-element *string*, not a character
— the same display `"," vs "a1b"` gives. Every result here is a string, never a char atom.

`.regexp.groups_all` gives one group-list per match, so selecting a group across every match is
ordinary indexing:

```q
q).regexp.groups_all["a1 b22";"([a-z])([0-9]+)"]
,"a" ,"1"
,"b" "22"
q).regexp.groups_all["a1 b22";"([a-z])([0-9]+)"][;1]
,"1"
"22"
```

That last line is DuckDB's `regexp_extract_all(subject, pattern, group)` — the second group of
every match — without needing a group argument.

### Replacing and splitting

`.regexp.replace` replaces the first match, `.regexp.replace_all` every match. `\1` to `\9`
refer to capture groups:

```q
q).regexp.replace["a-b-c";"-";"+"]
"a+b-c"
q).regexp.replace_all["a-b-c";"-";"+"]
"a+b+c"
q).regexp.replace["John Smith";"(\\w+) (\\w+)";"\\2 \\1"]
"Smith John"
```

```q
q).regexp.split["a,b,,c";","]
,"a"
,"b"
""
,"c"
```

A subject containing no match splits into itself: `.regexp.split["abc";","]` is `enlist "abc"`.

`split` follows DuckDB's `regexp_split_to_array` exactly, including its two non-obvious rules
for patterns that can match nothing: a zero-width match at the *start of the remaining text* is
not treated as a delimiter — it yields one character and the scan moves on — and once the
remaining text is empty no further match is considered.

```q
q).regexp.split["abc";""]
,"a"
,"b"
,"c"
q).regexp.split["ab";"a*"]
""
,"b"
```

`extract_all`, by contrast, keeps every zero-width match — also DuckDB's answer
(`regexp_extract_all('ab','a*')` is `[a, '', '']`). The two functions genuinely differ here.

### Matching literal text

`.regexp.escape` turns text into the pattern that matches it exactly:

```q
q).regexp.escape "1.5-2.0?"
"1\\.5\\-2\\.0\\?"
q).regexp.matches["a.c";.regexp.escape "."]
1b
q).regexp.matches["abc";.regexp.escape "."]
0b
```

It composes with every function, and it is visible at the call site — which is why peachq has
this rather than DuckDB's `l` (literal) option flag.

## Subjects, and how many answers you get back

Symbols and character data are treated alike — you can match against either. What decides
whether you get one answer or many is whether the subject is *one* piece of text or a
collection of them:

| subject | example | result |
|---|---|---|
| symbol | `` `one `` | one answer |
| character | `"c"` | one answer |
| **string** | `"word"` | **one answer** |
| symbol list | `` `a`list `` | one per symbol |
| list of strings | `("words";"other")` | one per string |
| dictionary | `` `a`b!("x1";"yy") `` | one per value, keys kept |

**Every** function distributes, not just the predicates — there is never a reason to write
`each`:

```q
q).regexp.matches[`AAPL`MSFT`IBM;"^[AM]"]
110b
q).regexp.extract[("a1";"b22");"[0-9]+"]
,"1"
"22"
q).regexp.replace_all[`x`y!("a-b";"c-d");"-";"+"]
x| "a+b"
y| "c+d"
```

Distributing is also the fast path: the pattern is compiled once and the whole collection is
handled in a single call, where `each` would pay a q-level dispatch per element.

An empty collection answers a **typed** empty, so a predicate over an empty column is still
boolean:

```q
q).regexp.matches[0#`;"x"]
`boolean$()
```

**The one thing to watch:** a string is a list of characters, so `"word"` looks like a
collection but counts as a single subject. A symbol *list* counts as many. This is why
`"word"` gives one boolean and `` `a`list `` gives two. Mixed lists work elementwise, so
``("ab";`c)`` matches both entries.

An empty string is one empty subject, not zero subjects — `"" rlike "^$"` is `1b`. A null
symbol behaves as the empty string, so a predicate always answers with a boolean and never
injects a null into a `where` clause.

### The collapse trap

This one is q, not regex, and no amount of checking can catch it: **equal-length character
atoms collapse into one string when they are written.**

```q
q)type ("k";"p")
10h
q)count ("k";"p")
2
```

`("k";"p")` is *already* `"kp"` — a single two-character subject — before any function sees it.
Unequal lengths stay two subjects:

```q
q).regexp.matches[("k";"p");"k"]
1b
q).regexp.matches[("k";"pp");"k"]
10b
```

If a collection of one-character subjects matters, build it with `enlist each` or as symbols.

### List results over a collection

`extract_all`, `groups`, `groups_all` and `split` already return a *list* for a single subject,
so over a collection they return a list of lists — and the type alone cannot tell you which
you have:

```q
q).regexp.extract_all[("a1";"b22c33");"[0-9]+"]
,,"1"
("22";"33")
```

`each` has exactly the same ambiguity; distribution just makes it easier to reach. Know the
shape of your subject.

### What is rejected

Anything not certainly text is `'type`: nesting deeper than one level, a collection with a
non-text element, a dictionary with non-text values, a table.

```q
q).regexp.matches[(("a";"bb");("c";"dd"));"a"]
'type
q).regexp.matches[("ab";1);"a"]
'type
q).regexp.matches[([] a:1 2);"a"]
'type
```

Accepting a shape is a one-way door; rejecting one is reversible. If a shape here should work,
it can be added.

## Patterns

Patterns are ordinary q strings, which means **a backslash must be doubled**:

```q
q).regexp.matches["a1";"\\d"]
1b
```

Write `"\\d"`, `"\\s"`, `"\\w"`, `"\\."` — the q string `"\\d"` is the two characters `\` and
`d`, which is what the regex engine needs to see.

peachq uses RE2 syntax, which covers the usual ground: character classes, `+ * ? {n,m}`,
alternation, groups, anchors, Perl classes (`\d \s \w`), and Unicode classes. RE2
deliberately omits backreferences within a pattern and lookaround assertions — the price of
its linear-time guarantee. Full syntax reference: <https://github.com/google/re2/wiki/Syntax>.

Matching is UTF-8 aware. A pattern that can match *nothing* (`""`, `"x*"`) advances by a whole
character rather than a byte, so `.regexp.split["aéb";""]` keeps `"é"` intact.

## Flags

**There is no options argument.** Flags go inside the pattern, where RE2 reads them itself:

```q
q).regexp.matches["ABC";"b"]
0b
q).regexp.matches["ABC";"(?i)b"]
1b
q)"ABC" rlike "(?i)b"
1b
```

| flag | meaning |
|---|---|
| `(?i)` | case-insensitive |
| `(?s)` | `.` matches newline |
| `(?m)` | `^`/`$` match at line breaks |

They apply from that point on, and `(?i:...)` scopes to a group. Because a pattern carries its
own flags, `subject rlike pattern` and `.regexp.matches[subject;pattern]` always agree for the
same pattern — and an infix operator, which can only take two arguments, is as capable as the
function.

DuckDB spells these as a third string argument (`regexp_matches('ABC','b','i')`). It accepts
the in-pattern form identically, so translation is a no-op: the flags travel inside the pattern
string, which a query translator passes through verbatim. Of DuckDB's own 231 regexp calls
across its test corpus, 27 use the options argument and only 2 — the `l` (literal) uses — could
not be written in-pattern. Those get `.regexp.escape` instead, and `g` gets
`.regexp.replace_all`, so nothing is lost and every function keeps a fixed arity.

## Errors

`'regex` means one thing: a pattern RE2 will not compile, or a replacement string that does not
fit its pattern.

```q
q).regexp.matches["abc";"("]
'regex
q).regexp.replace["a-b";"-";"\\1"]
'regex
```

Regex matching in a build with no RE2 module also signals `'regex` — the matcher is a separate
shared object, so that `./q` itself needs no C++ runtime.

## How this relates to `like`, `ss` and `ssr`

They are unchanged and unrelated. `like` remains glob-style (`*` and `?`) and matches the
whole subject; `ss` and `ssr` keep their own limited pattern grammar. Reach for `like` when a
glob says what you mean, and `.regexp`/`rlike` when it does not. There is deliberately no
overlap in behaviour between `like` and `rlike` beyond both returning booleans — notably
`like` is whole-subject while `rlike` searches.

## For DuckDB SQL users

| DuckDB SQL | peachq |
|---|---|
| `regexp_matches(s, p)` | `.regexp.matches[subject;pattern]` or `subject rlike pattern` |
| `regexp_full_match(s, p)` | `.regexp.full_match[subject;pattern]` |
| `regexp_extract(s, p)` | `.regexp.extract[subject;pattern]` |
| `regexp_extract(s, p, n)` | `.regexp.groups[subject;pattern][n-1]` |
| `regexp_extract(s, p, name_list)` | `` names!.regexp.groups[subject;pattern] `` |
| `regexp_extract_all(s, p)` | `.regexp.extract_all[subject;pattern]` |
| `regexp_extract_all(s, p, n)` | `.regexp.groups_all[subject;pattern][;n-1]` |
| `regexp_replace(s, p, r)` | `.regexp.replace[subject;pattern;replacement]` |
| `regexp_replace(s, p, r, 'g')` | `.regexp.replace_all[subject;pattern;replacement]` |
| `regexp_escape(s)` | `.regexp.escape[text]` |
| `regexp_split_to_array(s, p)` | `.regexp.split[subject;pattern]` |
| `regexp_split_to_table(s, p)` | `flip enlist .regexp.split[subject;pattern]` |

Two differences worth knowing. DuckDB's `~` operator is `regexp_full_match`, whereas peachq's
`rlike` searches — so `~` and `rlike` are **not** equivalents. And DuckDB has no
case-insensitive regex operator at all (`~*` is unsupported there), which is why peachq
documents `(?i)` rather than adding a second keyword.

`regexp_split_to_table` has no peachq twin because a q list already *is* the result; make a
table from it if you want one.

## Version pinning

peachq vendors RE2 from the same DuckDB release it is built to work with, so that a predicate
evaluated in peachq and the same predicate pushed down to DuckDB match identically. A RE2
version bump can change matching at the edges, which is exactly the agreement being protected.

RE2 as DuckDB vendors it carries no version string of its own — it is an unversioned snapshot
of a de-abseil'd fork — so the pin is expressed as the DuckDB release it came from, and
`.regexp.version` reports it:

```q
q).regexp.version
"v1.4.5"
```

Because there is no version string, drift in the vendored source is caught by a digest of the
tree instead, checked at build time (`third_party/re2/README.openq.md`). The DuckDB you connect
to is only ever *compared*, never enforced: DuckDB is optional and you may legitimately point
`PEACHQ_DUCKDB_LIB` at another release, so `.regexp.duckdb_match[handle]` answers whether an
open connection agrees with our pin and leaves the judgement to you.
