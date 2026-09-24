# Markdown

The `.md` namespace reads Markdown as q data. `.md.parse` answers a table with one row per block, in document
order, so ordinary q-sql can filter a document, split it by section, or pull out its code blocks and tables.
`.md.html` renders the same document as XHTML.

Every function, parameter and column is listed in the [.md API](https://peachq.org/docs/api/md.q.html).

## Dialect

`.md` reads [CommonMark](https://spec.commonmark.org/0.31.2/) with three extensions: tables, `~~strikethrough~~`,
and task lists (`- [x] done`). Footnotes, admonitions, wiki links and maths are not recognised, so their text reads
as ordinary paragraphs. `.md.html` renders every example in the CommonMark specification as the specification
does.

## Loading

`.md` is part of the standard library:

```q
q)\l pq
```

## Sources

Both functions take the document the way `.csv.read` and `.j.read` take theirs:

- a symbol is a resource to read: a file such as `` `:notes.md ``, or an `http(s)` URL;
- a string is the Markdown itself;
- a list of strings is joined with newlines.

Anything else signals `'type`.

A symbol literal ends at a `-`, in kdb as well as here, so `` `:user-docs/c-extensions.md `` is not one symbol.
Build a hyphenated path from a string instead:

```q
q)t:.md.parse hsym `$"user-docs/c-extensions.md"
```

## Blocks, sections and code

Every block records the row of its enclosing block (`parent`) and the latest heading at or above it (`head`), so
the section under a heading is a `where head=` away:

```q
q)t:.md.parse "# Intro\n\nSome *text*.\n\n```q\n1+1\n```\n\n## Data\n\nnumbers below\n"
q)select kind, val from t where head=first exec i from t where kind=`h, val~\:"Data"
```

Text is plain. Emphasis, links and code spans keep their text and lose their markup, a soft line break becomes a
space, and a hard break becomes `"\n"`. A code block keeps its text exactly as written, and its `info` column holds
the fence's info string, so pulling out the q code blocks is one query:

```q
q)exec val from t where kind=`code, info like "q*"
```

## Tables

A table block's `val` is a q table: `.csv.read` of its cells. The header row names the columns and each column is
typed exactly as `.csv.read` types it. Numbers, dates and times become typed columns, text stays strings, and an
empty cell in a typed column is null.

```q
q)first exec val from .md.parse "|sym|px|\n|-|-|\n|a|1.5|\n|b|2|\n" where kind=`table
q)c:.md.parse hsym `$"user-docs/c-extensions.md"
q)first exec val from c where kind=`table        / the guide's Platform/Status table
```

## `.md.html`

`.md.html` answers the document's XHTML as a string: the contents of `<body>`, with `<br />`, `<hr />` and
self-closed `<img />`.

```q
q).md.html "Hello *world*"
"<p>Hello <em>world</em></p>\n"
```
