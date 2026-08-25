/ j.q - the .j JSON reader: .j.read loads a JSON document as a table, .j.info reports the schema it would use.
/ .j.j and .j.k (serialize / deserialize a string, ref/dotj.md) are always available; these two arrive with \l pq.

/ Load a JSON document as a table.
/ Numbers keep the form they were written in, at every depth: 1 reads as a long, 1.0 and 1e3 as floats, and an
/ integer too wide for 0Wj as a float.  true/false read as booleans, a string as a char-vector column, and an
/ object or array as the nested value .j.k would build for it.  A column mixing forms - a string beside a
/ number, or true beside 1 - keeps them as they are rather than picking one.  (.j.k differs: it reads EVERY
/ number as a float, which is what ref/dotj.md documents it to do.)
/ Each JSON root shape returns:
/   an object              a one-row table of its keys
/   an array of objects    one row per object
/   any other array        a single column named `json, one row per element; [] gives it zero rows
/   a scalar, null or {}   'type - none of these is a table
/ Nulls take no part in typing: a column is typed by the values that are not null, wherever they sit, and each
/ null is then written as that type's q null - 0N in a long column, 0n in a float one, 0b in a boolean one,
/ since q booleans have no null.  A column of nothing but nulls is a float column of 0n.
/ Column types are inferred from the first sample_size records and then fixed.  A later value that does not fit
/ signals 'type rather than arriving as a null - raise sample_size, or name the column in types, which replaces
/ the inference for that column entirely.
/ Date and time strings are never recognised as such: JSON has no temporal type, so a date-shaped string stays
/ a string.  Ask for it in types and it is parsed.
/ Every record must carry the same keys, in any order - records are matched by name.  A missing or extra key
/ signals 'type and a repeated key 'dup.
/ @param source (symbol or string or string list) `:trades.json, a URL, or the JSON text itself.  A symbol is
/ always a resource to open and text is always the content to parse; a list of strings is joined with newlines
/ first, so read0 output can be passed straight in.  A leading UTF-8 byte-order mark is ignored
/ @param target (any) reserved for a future in-place load; anything but :: signals 'nyi
/ @param types (dict or string) column syms to type chars, e.g. `a`b!"sj", or a type-char string "sj f" giving
/ the whole schema, one char per column.  A named column is converted with q's own `$` - text is parsed
/ ("2026.08.25" under "d" is a date, and text a type cannot parse becomes that type's null) and anything else
/ is cast.  "*" leaves the column as JSON wrote it, and " " drops it
/ @param opts (dict) sample_size (long, how many records to infer from); format (symbol, how records are framed
/ in the document: `array, one JSON document, is the only one implemented - `newline_delimited and `auto
/ signal 'nyi).  Any other key signals 'option
/ @throws nyi for a target, or a format that is not implemented; option for an unknown option key; type for a
/ document that is not a table shape, a record whose keys disagree, or a value that does not fit its column's
/ type; dup for a repeated key in one record; parse for a malformed document
/ @example .j.read["[{\"a\":1},{\"a\":2}]";::;::;()!()]  /  a table of one long column
/ @example .j.read[`:price.json;::;`price!enlist "f";(enlist `sample_size)!enlist 1000]
.j.read:{[source;target;types;opts] .j.i.read[source;target;types;opts]};

/ Report the schema .j.read would use, without loading the document: a dict of column name to type char, which
/ can be passed straight back to .j.read as its types argument.
/ One entry per top-level column.  A nested column - or any column whose values do not share one q type -
/ answers "*", meaning "as JSON wrote it"; walk into such a column with meta, key, type and first once it is
/ loaded.  Only the first sample_size records are read, so the rest of the document is not validated, but
/ everything that IS read obeys the same rules .j.read obeys - a document .j.read would refuse is refused here.
/ @param source (symbol or string or string list) the same source forms .j.read takes
/ @param opts (dict) the same options dict .j.read takes
/ @example .j.info["[{\"a\":1,\"b\":\"x\",\"o\":{\"p\":1}}]";()!()]  /  `a`b`o!"j**"
.j.info:{[source;opts] .j.i.info[source;opts]};
