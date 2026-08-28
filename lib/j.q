/ j.q - the .j JSON reader: .j.read loads a JSON document as a table, .j.info reports the schema it would use.
/ .j.j and .j.k (serialize / deserialize a string, ref/dotj.md) are always available; these two arrive with \l pq.

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
/ number, or true beside 1 - keeps them as they are rather than picking one.  (.j.k differs: it reads EVERY
/ number as a float, which is what ref/dotj.md documents it to do.)
/ WHAT A CELL MEANS IS THE LOADERS' ONE LAW: a string is read exactly as .csv.read reads the same bytes, so the
/ writer's temporal forms type on sight - 2011-01-01 a date, 2011-01m a month, 2011-01-01D10:57:42.000000000 a
/ timestamp, hh:mm / hh:mm:ss / .fff clocks, the D-separated timespan - and .j.j's own output round-trips
/ unaided.  Quoting is type information CSV does not have and .j.j uses it: a QUOTED number or boolean - "1",
/ "true" - stays text, because the file said it was a string (DuckDB's read_json answers VARCHAR there too).
/ A tz-SUFFIXED spelling (..Z, ..+02:00) stays text at defaults - never a silent shift; an explicit "p" in
/ types accepts the ZERO-offset tails Z / z / +00:00 and minute resolution, while "P"$"..Z" stays 0Np - the
/ loaders accept more than the kx cast, and less: T alone is a Tok boolean but loader text.
/ The \z DECLARATION reaches here too (owner 2026-08-27): a quoted "03/10/2024" or "12-13-2004" - with or
/ without a seconds clock - types under the field order \z states, exactly as the same bytes do in a CSV cell.
/ THE WRITER'S FORM IS THE GRAMMAR (owner 2026-08-27), which is what makes .j.j's guids and bytes round-trip:
/ a canonical 36-char uuid types g, and "0x" + EXACTLY two hex digits types x.  Longer hex stays text (a byte
/ VECTOR is not one cell) and so does BARE hex (0a is likelier an identifier).  "X"$"0x0a" is 0x00 and STAYS
/ 0x00 - Tok reads bare hex, so the prefix makes it consume an invalid pair - while the loader reads the
/ prefixed form, the one q writes.  DuckDB's read_json types a uuid UUID and a "0x0a" VARCHAR, where its own
/ read_csv does the opposite of both; it disagrees with itself, so loading OUR output decides it.
/ JSON has no symbol type, so a sym column writes exactly as a string column does and reads back text; types
/ "s" is the door.  A char column is the same: "a" is written as any one-character string is.
/ A nested object or array loads RECURSIVELY with the same column inference: a column of records is a nested
/ TABLE (a record's missing key null-fills as its column's typed null), an array of records is a sub-table
/ cell, and a genuine mix keeps the values as written.  types does not reach inside - pull a nested table
/ apart with q; path addresses the document root, not each record.
/ Records can be framed two ways and format says which.  `array means the file is ONE JSON document;
/ `newline_delimited means it is a stream of whole values, none of them spanning a line break, which is what
/ save `t.json writes.  `auto - the default - asks whether the whole file is one document and reads it that way
/ if it is, so it handles both, and a pretty-printed object too.  Stating the framing you do not have fails:
/ `array over JSON Lines and `newline_delimited over an array both signal 'parse.
/ Each JSON root shape returns, under the default records:`auto:
/   an object              a one-row table of its keys
/   an array of objects    one row per object
/   any other array        a single column named `json, one row per element; [] gives it zero rows
/   a scalar, null or {}   'type - none of these is a table
/ Nulls take no part in typing: a column is typed by the values that are not null, wherever they sit, and each
/ null is then written as that type's q null - 0N in a long column, 0Nd in a date one, 0b in a boolean one,
/ since q booleans have no null.  A column of nothing but nulls is a float column of 0n.
/ Column types are inferred from the first sample_size records and then fixed.  A later value that does not fit
/ signals 'type rather than arriving as a null - raise sample_size, or name the column in types, which replaces
/ the inference for that column entirely; under a continue-mode lever the record drops with a reject record
/ instead (THE REJECT CHANNEL below).
/ Records are matched by name, so key order is free, and they need not carry the same keys: the columns are the
/ union of the keys the sampled records carry, in the order they first appear, and a record that omits one gets
/ that column's null.  A key first seen after the sample is not a column and signals 'type; a key repeated
/ inside one record signals 'dup.  A missing key and a written null are the same thing.
/ Records the caller wants often sit inside an envelope - {"status":"OK","results":[...]} - so path selects a
/ value out of each document before any of the above happens.  The selected value becomes the top-level one,
/ which is why types then reaches its columns: the read is exactly what loading that value on its own gives.
/ @param source (symbol or string or string list) `:trades.json, a URL, or the JSON text itself.  A symbol is
/ always a resource to open and text is always the content to parse; a list of strings is joined with newlines
/ first, so read0 output can be passed straight in.  A leading UTF-8 byte-order mark is ignored.  On an http or
/ https URL a `#` fragment spells the path option - `$":https://api.x/v2/aggs#`results", `#`data`items` for
/ depth - which keeps the URL copy-pastable, since a fragment is client-side and never sent.  It is READ, not
/ evaluated, so it spells symbol steps only; an array index is spelled by the option.  Only URLs: `#` is a
/ legal character in a file name, so in a file path it is a literal `#`
/ @param target (any) reserved for a future in-place load; anything but :: signals 'nyi
/ @param types (dict or string) column syms to type chars, e.g. `a`b!"sj", or a type-char string "sj f" giving
/ the whole schema, one char per column.  A named LOADER type - the whole 18-type roster .csv.read takes -
/ parses text through the shared cell parser, so "2026.08.25" under "d" is a date and text the type cannot
/ parse signals 'type, never a silent null; anything that is not text is CAST instead (widening a long column
/ to float is a cast, not a parse).  "*" leaves the column as the untyped read builds it, and " " drops it
/ @param opts (dict) sample_size (long, how many records to infer from); format (symbol, how records are framed:
/ `array, `newline_delimited or `auto, the default); records (boolean or `auto, the default: 1b reads every value
/ as a record and refuses one that is not an object, 0b never expands and gives the single `json column - and
/ never recurses); dateformat / timestampformat (string, the same strptime subset .csv.read validates, replacing
/ the writer's-forms grammar for its type); path (symbol, long or a list of them: the value to read out of each
/ document, indexed as `.` indexes - symbols pick object keys, longs pick array elements - so `results,
/ `data`items and (`data;`tables;0) are all paths, and :: or an empty list is the whole document).
/ THE REJECT CHANNEL (all default off, .csv.read's law: a lever governs whether the load SURVIVES a bad record,
/ never whether it is counted): ignore_errors (boolean: 1b drops a record that cannot be built - an unknown or
/ duplicate key, a frozen-type miss, an nd line that will not parse as one whole in-line value - instead of
/ signalling; a record lands whole or not at all); store_rejects (boolean: continue AND store the reject records
/ in a global table, replaced per load - line, the physical line under newline_delimited and the record's
/ 1-based ordinal under array framing; column; error class cast/unknownkey/dupkey/parse; record, re-serialized);
/ rejects_table (symbol, default `reject_errors, landing like any q assignment) names it and implies
/ store_rejects; null_padding (boolean) is accepted for .csv.read parity and changes nothing - the key-union
/ law already null-fills an omitted key.  records and ignore_errors also decide the root shapes above.  Any
/ other key signals 'option
/ @throws nyi for a target; option for an unknown option key; domain for a path step this document cannot take;
/ type for a document that is not a table shape, a path step of the wrong kind - a symbol into an array, a long
/ into an object - a key beyond the frozen
/ schema, or a value that does not fit its column's type; dup for a repeated key in one record; parse for a
/ malformed document, a source carrying no document at all, or a framing the file does not have
/ @example .j.read["[{\"a\":1},{\"a\":2}]";::;::;()!()]  /  a table of one long column
/ @example .j.read["{\"a\":1}\n{\"a\":2}\n";::;::;()!()]  /  the same table, read as JSON Lines
/ @example .j.read[`:price.json;::;`price!enlist "f";(enlist `sample_size)!enlist 1000]
/ @example .j.read[`:aggs.json;::;(enlist `t)!enlist "p";(enlist `path)!enlist `results]  /  inside the envelope
.j.read:{[source;target;types;opts] r:.j.i.frag[source;opts]; .j.i.read[r 0;target;types;r 1]};

/ Report the schema .j.read would use, without loading the document: a dict of column name to type char, which
/ can be passed straight back to .j.read as its types argument.
/ One entry per top-level column, over the same key union .j.read builds.  A nested column - or any column
/ whose values do not share one q type - answers "*", meaning "as the untyped read builds it" (for a column of
/ records, the recursively loaded nested table); walk into such a column
/ with meta, key, type and first once it is loaded.  Only the first sample_size records are read, so the
/ rest of the document is not validated, but everything that IS read obeys the same rules .j.read obeys -
/ a document .j.read would refuse is refused here.
/ path is honoured here too, so the sniff always describes the document the read would build.
/ @param source (symbol or string or string list) the same source forms .j.read takes
/ @param opts (dict) the same options dict .j.read takes
/ @example .j.info["[{\"a\":1,\"b\":\"x\",\"o\":{\"p\":1}}]";()!()]  /  `a`b`o!"j**"
.j.info:{[source;opts] r:.j.i.frag[source;opts]; .j.i.info[r 0;r 1]};
