/ j.q - the .j JSON reader: .j.read loads a JSON document as a table, .j.info reports the schema it would use.
/ .j.j and .j.k (serialize / deserialize a string) are always available; these two arrive with \l pq.

/ The `#` fragment is SUGAR for the path option and reaches the same one mechanism.  It is read only on an
/ http(s) URL, where a fragment is client-side by RFC and the transport drops it anyway; a `#` in a file path
/ is a legal POSIX filename character and stays literal.  The tail is READ, never evaluated: a URL can be
/ assembled from anywhere, and `value` on it would run arbitrary q before the request is even made.  So it
/ spells symbol steps and nothing else; an array index is spelled by the option, which needs no grammar.
.j.i.frag:{[source;opts]
  if[not -11h=type source; :(source;opts)];
  u:string source;
  if[not any (u like ":http://*"; u like ":https://*"); :(source;opts)];
  if[(n:u?"#")=count u; :(source;opts)];
  o:$[99h=type opts;opts;()!()];
  if[`path in key o; '`domain];
  f:(n+1)_u;
  if[count[f] and not "`"=first f; '`type];
  ((`$n#u); o,(enlist `path)!enlist $[count f;`$1_'(where "`"=f) cut f;::])};

/ Load a JSON document as a table.
/ Numbers keep the form they were written in, at every depth: 1 reads as a long, 1.0 and 1e3 as floats, and an
/ integer too wide for 0Wj as a float.  true/false read as booleans.  A column mixing forms - a string beside a
/ number, or true beside 1 - keeps them as they are.  (.j.k differs: it reads EVERY number as a float.)
/ A string cell means what the same bytes mean in a CSV cell - one shared parser, so .j.j's own output round-trips
/ unaided.  QUOTING is type information JSON has and CSV does not, and it is respected: a quoted number or boolean
/ ("1", "true") stays TEXT, because the file said it was a string.  The written-form grammar, \z day order, the
/ sentinel rule, the timezone posture and the 18-type roster are the loaders' shared law - see the guide
/ user-docs/loading.md.  JSON has no symbol or char type, so those columns read back text; types is the door.
/ Records are framed two ways and format says which: `array is ONE JSON document, `newline_delimited a stream of
/ whole values none spanning a line break (what save `t.json writes), and `auto - the default - reads either, a
/ pretty-printed object included.  Stating a framing the file does not have signals 'parse.
/ Each JSON root shape returns, under the default records:`auto: an object gives a one-row table of its keys, an
/ array of objects one row per object, any other array a single column named `json with one row per element ([]
/ giving it zero rows), and a scalar, null or {} signals 'type - none of those is a table.
/ Nulls take no part in typing: a column is typed by its non-null values, each null written as that type's q null,
/ and a column of nothing but nulls is a float column of 0n.  Types are inferred from the first sample_size records
/ and then FIXED: a later value that does not fit signals 'type rather than nulling - raise sample_size, or use types.
/ Records match by NAME: key order is free, the columns are the union of the sampled keys in first-appearance order,
/ an omitted key is that column's null, a key first seen after the sample is 'type and a repeat in one record 'dup.
/ A nested object or array loads RECURSIVELY with the same column inference: a column of records is a nested TABLE,
/ an array of records is a sub-table cell, a genuine mix keeps the values as written, and types does not reach in.
/ Records the caller wants often sit inside an envelope - {"status":"OK","results":[...]} - so path selects a value
/ out of each document BEFORE any of the above, and the read is what loading that value on its own would give.
/ xcol RENAMES columns on the way in, taking what the q verb xcol takes and applying it BEFORE the target is
/ consulted; types keys on the FILE's key names whatever the rename does.  The full guide is user-docs/json.md;
/ bad rows and the tolerance levers are user-docs/bad-rows.md.
/ @param source (symbol or string or string list) `:trades.json, a URL, or the JSON text itself.  A symbol is
/ always a resource to open and text is always the content to parse; a list of strings is joined with newlines
/ first, so read0 output can be passed straight in.  A leading UTF-8 byte-order mark is ignored.  On an http(s)
/ URL a `#` fragment spells the path option - `$":https://api.x/v2/aggs#`results" - and only there, since `#` is a
/ legal character in a file name; it is read, not evaluated, so it spells symbol steps only
/ @param target (symbol) a global table name the rows are inserted into (created when absent, upserted when keyed),
/ or a rank-3 lambda {[tblData;errData;misc] ...} - one document is ONE batch, so it is called once.  :: returns
/ the table; anything else answers the summary dict `rows`rejected`chunks`ignored`types.  An existing table's
/ schema outranks the sniff and columns it lacks are dropped and reported under `ignored`; explicit types
/ disagreeing with it signals 'mismatch.  A partial load leaves a partial table: insert is not transactional
/ @param types (dict or string) column syms to type chars, e.g. `a`b!"sj", or a type-char string "sj f" giving the
/ whole schema, one char per column.  A named type from the 18-type roster .csv.read takes parses TEXT through the
/ shared cell parser, so "2026.08.25" under "d" is a date and text the type cannot parse signals 'type, never a
/ silent null; anything not text is CAST instead.  "*" leaves the column as the untyped read builds it, " " drops it
/ @param opts (dict) xcol (symbol vector renaming the first columns positionally, or a dict of key name to new
/ name); sample_size (long, records to infer from); format (symbol: `array, `newline_delimited or
/ `auto); records (boolean or `auto: 1b reads every value as a record and refuses one that is not an object, 0b
/ never expands and never recurses, giving the single `json column); dateformat / timestampformat (string, the
/ same strptime subset .csv.read validates, replacing the writer's-forms grammar for its type); path (symbol, long
/ or a list of them: the value to read out of each document, indexed as `.` indexes - so `results, `data`items and
/ (`data;`tables;0) are all paths, and :: or an empty list is the whole document).  THE REJECT CHANNEL, all default
/ off: ignore_errors, store_rejects, rejects_table (symbol, default `reject_errors) and null_padding, which is
/ accepted for .csv.read parity and changes nothing.  Any other key signals 'option
/ @throws option for an unknown option key; domain for a path step this document cannot take; rank for a lambda
/ target of any other rank; length for an xcol key naming no column, before a value is read - a bad option is
/ never a reject record; type for a document that is not a table shape, a target that is neither symbol nor
/ lambda, a path step of the wrong kind, a key beyond the frozen schema, or a value that does not fit its
/ column's type; dup for a repeated key in one record; parse for a malformed document, a source carrying no
/ document at all, or a framing the file does not have
/ @example .j.read["[{\"a\":1},{\"a\":2}]";::;::;()!()]  /  a table of one long column
/ @example .j.read["{\"a\":1}\n{\"a\":2}\n";::;::;()!()]  /  the same table, read as JSON Lines
/ @example .j.read[`:price.json;::;`price!enlist "f";(enlist `sample_size)!enlist 1000]
/ @example .j.read[`:aggs.json;::;(enlist `t)!enlist "p";(enlist `path)!enlist `results]  /  inside the envelope
.j.read:{[source;target;types;opts] r:.j.i.frag[source;opts]; .j.i.read[r 0;target;types;r 1]};

/ Report the schema .j.read would use, without loading the document: a dict of column name to type char, which
/ can be passed straight back to .j.read as its types argument.
/ One entry per top-level column, over the same key union .j.read builds.  A nested column - or any column whose
/ values do not share one q type - answers "*", meaning "as the untyped read builds it"; walk into such a column
/ with meta, key, type and first once it is loaded.  Only the first sample_size records are read, so the rest of
/ the document is not validated, but everything that IS read obeys the same rules .j.read obeys - a document
/ .j.read would refuse is refused here.  path is honoured too, so the sniff always describes the document the read
/ would build.  Unlike .csv.info it carries no advisory "s".  It describes the DOCUMENT, so an xcol in opts is
/ accepted and renames nothing here - which is what keeps the answer feedable back as types.
/ @param source (symbol or string or string list) the same source forms .j.read takes
/ @param opts (dict) the same options dict .j.read takes
/ @example .j.info["[{\"a\":1,\"b\":\"x\",\"o\":{\"p\":1}}]";()!()]  /  `a`b`o!"j**"
/ @example .j.info[`:aggs.json;(enlist `path)!enlist `results]  /  the schema inside the envelope
.j.info:{[source;opts] r:.j.i.frag[source;opts]; .j.i.info[r 0;r 1]};
