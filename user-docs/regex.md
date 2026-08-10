# Regular expressions

> **Status: proposed.** This document is the agreed design, written before the code. Every
> example here is pinned as a test row in `test/q/aigenerated/regex_rlike.qcmd`, and those rows
> are **red** until the engine lands. If an example here and the code ever disagree, this
> document and that ledger are the specification.

peachq matches with [RE2](https://github.com/google/re2), the same engine DuckDB uses. RE2
guarantees linear-time matching, so no pattern — however hostile or badly written — can hang a
query the way a backtracking engine can. It is also what makes a peachq predicate and the
equivalent DuckDB SQL predicate mean the same thing, which matters once a query pushes down to
DuckDB storage.

Regex is a peachq extension: standard q has no regular expressions, only glob-style `like` and
the `ss`/`ssr` search verbs. Nothing here changes those.

## Loading

The `.regex` namespace is part of the standard library:

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

Being infix, `rlike` takes exactly two arguments and so has no options argument. Put flags in
the pattern instead — see [Options](#options).

## Function reference

| function | returns |
|---|---|
| `.regex.matches[s;p]` `[s;p;o]` | boolean — match anywhere |
| `.regex.full_match[s;p]` `[s;p;o]` | boolean — the *entire* subject must match |
| `.regex.extract[s;p]` `[s;p;o]` | the matched text, `""` if no match |
| `.regex.groups[s;p]` `[s;p;o]` | the capture groups, `()` if no match |
| `.regex.extract_all[s;p]` `[s;p;o]` | every non-overlapping match |
| `.regex.replace[s;p;r]` `[s;p;r;o]` | first match replaced; `"g"` for all |
| `.regex.split[s;p]` `[s;p;o]` | subject split on the pattern |

`s` is the subject, `p` the pattern, `r` a replacement, `o` an options string.

### Matching

```q
q).regex.matches["abc";"b"]
1b
q).regex.full_match["abc";"b"]
0b
q).regex.full_match["abc";"a.c"]
1b
```

`.regex.matches` is `rlike` with an options argument available.

### Extracting

`.regex.extract` gives the matched text; `.regex.groups` gives the capture groups:

```q
q).regex.extract["2026-08-10";"[0-9]{4}"]
"2026"
q).regex.groups["2026-08-10";"([0-9]{4})-([0-9]{2})"]
"2026"
"08"
```

There is no group-number argument, because q can index a result. Take the group you want, or
name them all at once with a plain dict construction:

```q
q)first .regex.groups["2026-08-10";"([0-9]{4})-([0-9]{2})"]
"2026"
q)`year`month!.regex.groups["2026-08-10";"([0-9]{4})-([0-9]{2})"]
year | "2026"
month| "08"
```

`.regex.extract_all` returns every match rather than the first:

```q
q).regex.extract_all["a1b22c";"[0-9]+"]
"1"
"22"
```

RE2 supports up to nine capture groups.

### Replacing and splitting

`.regex.replace` replaces the first match; add `"g"` to replace all. `\1` to `\9` refer to
capture groups:

```q
q).regex.replace["a-b-c";"-";"+"]
"a+b-c"
q).regex.replace["a-b-c";"-";"+";"g"]
"a+b+c"
q).regex.replace["John Smith";"(\\w+) (\\w+)";"\\2 \\1"]
"Smith John"
```

```q
q).regex.split["a,b,,c";","]
"a"
"b"
""
"c"
```

A subject containing no match splits into itself: `.regex.split["abc";","]` is `enlist "abc"`.

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

```q
q)"word" rlike "^w"
1b
q)("words";"other") rlike "^w"
10b
q)`AAPL`MSFT`IBM rlike "^[AM]"
110b
```

**The one thing to watch:** a string is a list of characters, so `"word"` looks like a
collection but counts as a single subject. A symbol *list* counts as many. This is why
`"word"` gives one boolean and `` `a`list `` gives two. Mixed lists work elementwise, so
``("ab";`c)`` matches both entries.

An empty string is one empty subject, not zero subjects — `"" rlike "^$"` is `1b`. A null
symbol behaves as the empty string, so a predicate always answers with a boolean and never
injects a null into a `where` clause.

## Patterns

Patterns are ordinary q strings, which means **a backslash must be doubled**:

```q
q).regex.matches["a1";"\\d"]
1b
```

Write `"\\d"`, `"\\s"`, `"\\w"`, `"\\."` — the q string `"\\d"` is the two characters `\` and
`d`, which is what the regex engine needs to see.

peachq uses RE2 syntax, which covers the usual ground: character classes, `+ * ? {n,m}`,
alternation, groups, anchors, Perl classes (`\d \s \w`), and Unicode classes. RE2
deliberately omits backreferences within a pattern and lookaround assertions — the price of
its linear-time guarantee. Full syntax reference: <https://github.com/google/re2/wiki/Syntax>.

## Options

The options argument is a string of single-character flags, matching DuckDB's spelling:

| flag | meaning |
|---|---|
| `c` | case-sensitive (the default) |
| `i` | case-insensitive |
| `l` | treat the pattern as literal text, not as a regex |
| `m` `n` `p` | newline-sensitive matching |
| `s` | non-newline-sensitive matching |
| `g` | replace every match (`.regex.replace` only) |

```q
q).regex.matches["ABC";"b"]
0b
q).regex.matches["ABC";"b";"i"]
1b
q).regex.matches["abc";"."]
1b
q).regex.matches["abc";".";"l"]
0b
q).regex.matches["a.c";".";"l"]
1b
```

An unrecognised flag is an error rather than being quietly ignored, and so is `"g"` anywhere
but `.regex.replace`.

### Flags inside the pattern

Most flags have an in-pattern form, which is how you get case-insensitivity out of `rlike`:

```q
q)"ABC" rlike "(?i)b"
1b
```

`(?i)` `(?s)` `(?m)` apply from that point on, and `(?i:...)` scopes to a group. Because a
pattern can carry its own flags, `s rlike p` and `.regex.matches[s;p]` always agree for the
same `p`.

Two flags have no in-pattern form and are only available as options: **`l`** (literal) and
**`g`** (global replace).

## Errors

An invalid pattern, an unknown flag, or `"g"` outside `.regex.replace` all signal `'regex`.

```q
q).regex.matches["abc";"("]
'regex
```

## How this relates to `like`, `ss` and `ssr`

They are unchanged and unrelated. `like` remains glob-style (`*` and `?`) and matches the
whole subject; `ss` and `ssr` keep their own limited pattern grammar. Reach for `like` when a
glob says what you mean, and `.regex`/`rlike` when it does not. There is deliberately no
overlap in behaviour between `like` and `rlike` beyond both returning booleans — notably
`like` is whole-subject while `rlike` searches.

## For DuckDB SQL users

| DuckDB SQL | peachq |
|---|---|
| `regexp_matches(s, p)` | `.regex.matches[s;p]` or `s rlike p` |
| `regexp_full_match(s, p)` | `.regex.full_match[s;p]` |
| `regexp_extract(s, p)` | `.regex.extract[s;p]` |
| `regexp_extract(s, p, n)` | `.regex.groups[s;p][n-1]` |
| `regexp_extract(s, p, name_list)` | `` names!.regex.groups[s;p] `` |
| `regexp_extract_all(s, p)` | `.regex.extract_all[s;p]` |
| `regexp_replace(s, p, r)` | `.regex.replace[s;p;r]` |
| `regexp_split_to_array(s, p)` | `.regex.split[s;p]` |
| `regexp_split_to_table(s, p)` | `flip enlist .regex.split[s;p]` |

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
`.regex.version` reports it.
