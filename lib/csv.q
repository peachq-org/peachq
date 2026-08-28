/ csv.q - the .csv surface: .csv.read loads delimited text as a table, .csv.info reports the schema it would use.

/ Load delimited text as a table - from a file, a URL, or text already in memory.
/ A symbol or lambda target answers the summary dict `rows`rejected`chunks`ignored`types; otherwise the table
/ itself.  Types resolve as: explicit types > the existing target table's schema > the sniff - an EXISTING symbol
/ target fixes the per-column types from its meta (keyed targets upsert, unkeyed insert), CSV columns it lacks are
/ dropped and reported under `ignored`, and a headerless file maps to a target positionally: complete map, no
/ projection.  All other conformity (missing columns, types that will not convert) is insert's own contract.
/ THE SOURCE LAW: a SYMBOL is a resource, TEXT is content - never sniffed.  Text is one char vector (the whole
/ payload, embedded newlines and all) or a list of them, DEFINED as join-with-newline-then-parse and NOT one
/ element per record: only that reading rejoins a quoted field split across two elements.  A quoted field may hold
/ newlines or straddle a read boundary and still lands as one cell of one row.  Text chunks at the same
/ buffer_size a file is read at, so `chunks and a lambda target's misc match either way; the BOM strip is the same,
/ and an empty payload signals 'csv, exactly as a zero-byte file does.
/ Column types FREEZE after the sniff sample: a later cell that fails its frozen type signals 'csv, never a silent
/ null and never a mid-load re-promotion.  Sniffed text is always a string column - ask for symbols with types.
/ An option is NEVER silently ignored: an unknown or unimplemented key signals 'option.
/ The DELIMITER is sniffed among "," ";" tab "|" (a candidate qualifies only if every sample row parses to the same
/ field count above one column; most columns wins, ties keep candidate order).  None qualifying, a one-column-SHAPED
/ file loads whole lines as ONE column and anything else is refused with 'csv.  The DIALECT - quote, escape,
/ comment - is never sniffed: guessing it would silently change what the file means.
/ What a cell MEANS is shared with .j.read and written up in the guide user-docs/loading.md - the written-form
/ grammar, \z day order, the sentinel rule, the timezone posture and the 18-type roster.  This reader's own
/ options and behaviour are in user-docs/csv.md, and bad rows in user-docs/bad-rows.md.
/ @param file (symbol or string or string list) `:trades.csv, a URL, or the CSV text itself
/ @param target (symbol) a global table name each batch is inserted into (created when absent), or a rank-3
/ lambda {[tblData;errData;misc] ...} called once per batch - errData is that batch's reject records as a table
/ (line, column, error, csvLine; empty on a clean batch), misc the dict `chunk`rows (0-based batch index; rows
/ landed so far).  :: returns the table.  An explicit types disagreeing with an existing target signals 'mismatch
/ @param types (dict or string) column syms to type chars from the 18 basic types "bgxhijefcspmdznuvt", e.g.
/ `a`b!"sj", overriding the sniff per column; or a type-char string "sj f" as the complete schema, one char per
/ column.  " " drops a column, "*" keeps it as a string.  An explicit type is a promise: a cell that will not
/ parse under it is a frozen-type miss
/ @param opts (dict) delim (char), header (boolean), sample_size (long, sniff rows), buffer_size (long,
/ read-chunk bytes), skip (long, default 0: records - not physical lines - dropped off the FRONT before anything
/ else reads them, so every sniff runs on what remains; a blank line counts, a comment line does not, reject
/ records keep physical line numbers), dateformat / timestampformat (string, strptime subset
/ %Y %y %m %d %H %M %S %f %z - an explicit format REPLACES the built-in grammar for its type, \z's day-order
/ reading included; %z is timestampformat-only, reads Z or +-HH[[:]MM] and APPLIES it, storing UTC).
/ THE DIALECT OPTIONS, explicit only: quote (char; default "; "" disables quoting - quotes are ordinary bytes);
/ escape (char; default = the quote char, i.e. RFC "" doubling; inside a quoted field the escape consumes its
/ follower); comment (char; default off) - a line whose FIRST byte is the comment char is skipped whole, an
/ unquoted comment char mid-row truncates the row there, a quoted field protects it; comment lines are never
/ counted and never rejected, though they still advance the physical line numbers.
/ THE TOLERANCE LAW (all default off): a lever governs whether the load SURVIVES a bad row, never whether it is
/ counted - every bad row leaves a reject record (line; column; error class
/ cast/toofewcolumns/toomanycolumns/unquotedvalue/unterminatedquote/padded; raw csvLine) and bumps the summary's
/ `rejected`.  ignore_errors (boolean) skip bad rows; null_padding (boolean) pad short rows with nulls, each
/ padded row recorded; strict_mode (boolean, default 1b) - 0b reads a field whose quotes cannot parse as literal
/ bytes to the next delimiter; store_rejects (boolean) continue AND store the reject records in a global table,
/ replaced per load; rejects_table (symbol, default `reject_errors) names it and implies store_rejects
/ @throws option for an unknown or unimplemented option key; csv for a malformed file or a frozen-type miss
/ @example .csv.read["a,b\n1,2\n3,4\n";::;::;()!()]  /  two long columns, from text in memory
/ @example .csv.read[`:trades.csv;::;(enlist `sym)!enlist "s";()!()]  /  sym as symbols, the rest sniffed
/ @example .csv.read[`:trades.csv;`dest;::;(enlist `skip)!enlist 2]  /  past a 2-record preamble into `dest
/ @example .csv.read[`:big.csv;{[tblData;errData;misc] show count tblData};::;()!()]  /  stream, batch by batch
.csv.read:{[file;target;types;opts] .csv.i.read[file;target;types;opts]};

/ Report the schema .csv.read would infer, without loading the file: a dict of column name to type char, in
/ exactly the shape .csv.read takes as its types argument, so it can be edited and fed straight back.
/ ADVISORY on text: where low cardinality suggests a symbol column the dict says "s", even though .csv.read
/ itself never syms a sniffed column - feeding the dict back is what adopts the advice.
/ Reads only the sniff sample (sample_size rows), so it does not validate the rest of the file.
/ @param file (symbol or string or string list) the same source forms .csv.read takes
/ @param opts (dict) the same options dict .csv.read takes
/ @example .csv.info["a,b\n1,2\n";()!()]  /  `a`b!"jj"
/ @example .csv.info[`:trades.csv;()!()]  /  the sniffed schema, ready to edit
/ @example .csv.read[`:trades.csv;::;.csv.info[`:trades.csv;()!()];()!()]  /  adopt the sniff, symbol advice included
/ @example .csv.info[`:trades.csv;(enlist `sample_size)!enlist 100000]  /  sniff deeper before deciding
.csv.info:{[file;opts] .csv.i.info[file;opts]};
