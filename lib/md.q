/ Read Markdown as q data: .md.parse answers one row per block, .md.html renders XHTML.  The dialect is CommonMark
/ with tables, strikethrough and task lists.
/ .
/ @eg
/ t:.md.parse hsym `$"user-docs/c-extensions.md"
/ first exec val from t where kind=`table
/ .md.html "Hello *world*"

.pq.load_natives`md;

/ @ignore
.md.i.text:{[source]
  $[-11h=type source; "c"$read1 source;
    10h=abs type source; source;
    (0h=type source) and all 10h=abs type each source; "\n" sv "",/:source;
    '`type]};

/ @ignore
.md.i.csv:{[rows] .csv.read["\n" sv {"," sv {"\"",ssr[x;"\"";"\"\""],"\""} each x} each rows;::;::;`delim`header!(",";1b)]};

/ The document's blocks as a flat table, one row per block in document order, so a document filters, splits by
/ section and yields its code blocks and tables with ordinary q-sql.  Inline markup is dropped from text: emphasis,
/ links and code spans keep their text, a soft line break is a space and a hard one "\n".
/ @summary Markdown as a block table
/ @param source (symbol|string|list) `:notes.md, a URL, the Markdown text, or a list of lines joined with newlines
/ @return (table) one row per block in document order
/ @col kind (symbol) the block: `doc `quote `ul `ol `li `hr `h `code `html `p `table
/ @col depth (long) how many blocks enclose it; the doc row is 0
/ @col parent (long) the row number of the enclosing block, 0N for the doc row
/ @col head (long) the row number of the latest heading at or above it (a heading's own row), 0N before the first
/ @col level (long) a heading's level 1 to 6, 0N for every other kind
/ @col info (string) a code block's info string ("q title=x"), a task-list item's mark ("x", "X" or " "), else ""
/ @col val (any) plain text for h and p, the text as written for code and html, "" for doc quote ul ol li hr, and
/ for a table the q table .csv.read builds from its cells (names and types are .csv.read's)
/ @throws type a source that is none of those
/ @eg .md.parse "# Title\n\nSome *text*.\n\n- one\n- two\n"
/ @eg t:.md.parse "# A\n\nx\n\n# B\n\ny\n"; select kind, val from t where head=3
/ @eg exec val from .md.parse "```q\n1+1\n```\n\n```sh\nls\n```\n" where kind=`code, info like "q*"
/ @eg first exec val from .md.parse["|a|b|\n|-|-|\n|1|x|"] where kind=`table
.md.parse:{[source]
  t:.md.i.parse .md.i.text source;
  @[t;`val;@[;where t[`kind]=`table;.md.i.csv]]};

/ The document rendered as XHTML: the contents of <body>, following the CommonMark reference rendering.
/ @summary Markdown as XHTML
/ @param source (symbol|string|list) `:notes.md, a URL, the Markdown text, or a list of lines joined with newlines
/ @return (string) the XHTML
/ @throws type a source that is none of those
/ @eg .md.html "# Title\n\nSome *text*.\n\n---\n"
.md.html:{[source] .md.i.html .md.i.text source};
