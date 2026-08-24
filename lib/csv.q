/ csv.q - THE public .csv surface: the incremental CSV reader (src/qlang/io/q_csv.c).
/ Lambdas, not natives, so the surface is DISCOVERABLE: typing `.csv.read` prints the signature and names its
/ arguments.  The engine is a bytes-in / typed-rows-out core behind a chunked file driver: an RFC-4180 quoted
/ field may span read chunks (or hold newlines) and still lands as one cell of one row.
/ Column types FREEZE after the sniff sample; a later cell that fails its frozen type signals 'csv - never a
/ silent null.  An option is NEVER silently ignored: unknown (or not-yet-implemented) keys signal 'option.
/ The doc below is the NORMATIVE statement of the laws; user-docs/csv.md is the user-facing guide and restates
/ them with worked examples.  Where the two disagree this file wins, and the guide is the one that must change.
/ ANY-ORDER LAW: definitions only at top level.

/ load a CSV file through the incremental core.
/ A symbol or lambda target answers the summary dict `rows`rejected`chunks`ignored`types; otherwise the
/ table itself.  Parse types resolve as: explicit types > the existing target table's schema > the sniff -
/ an EXISTING symbol target fixes the per-column types from its meta (keyed targets upsert, unkeyed insert),
/ CSV columns it lacks are dropped and reported under `ignored`, and a headerless file maps to a target
/ positionally - complete map, no projection.  All other conformity (missing columns, types) is insert's
/ own contract.
/ @param file (symbol) file symbol, e.g. `:trades.csv
/ @param target (symbol) a global table name each batch is inserted into (created when absent), or a rank-3
/ lambda {[tblData;errData;misc] ...} called once per batch - errData is that batch's reject records as a
/ table (line, column, error, csvLine; empty on a clean batch), misc the dict `chunk`rows (0-based batch
/ index; rows landed so far)
/ The sniffer reads exactly what `csv 0:` writes (kdb-formats law, owner 2026-08-22): dashed dates,
/ dotted-D timestamps (plus the ISO T- and space-separated alternates), m-suffixed months, hh:mm minute,
/ hh:mm:ss second, 3-digit-fraction time and D-separated timespan - clock shapes promote per column up
/ the minute < second < time < timespan lattice.  It never guesses beyond a written form: bare 20260822
/ stays long, suffixless 2026.08 stays float, 1/0 stays long (type booleans and syms explicitly).
/ THE TZ POSTURE (owner 2026-08-23): kdb parses no timezone, so a tz-suffixed cell (...+02:00, ...Z) is TEXT
/ at defaults, bytes intact - never a silent shift.  An offset is read only where the caller asks with
/ timestampformat's %z, and then it is APPLIED: the stored timestamp is UTC.  Under an explicit p with
/ no format an offset cell is a frozen-type MISS ('csv, or a `cast reject under a continue-mode lever)
/ - DuckDB's forced-TIMESTAMP path drops the offset silently; we refuse instead.
/ A blank physical line is SKIPPED (DuckDB emits a row of nulls).  A file with a header row and no data
/ rows loads as a zero-row table with the header's columns; a zero-byte file signals 'csv - a file that
/ states no schema does not get one invented (DuckDB invents a VARCHAR column0).
/ @param types (dict) column syms to type chars from "bjfsdtpmuvn", e.g. `a`b!"sj", overriding the sniff
/ per column (a " " value drops that column by name); or a type-char string "sj f" as the complete
/ schema, one char per column, " " dropping a column, "*" keeping strings
/ Without an explicit delim the delimiter is sniffed among "," ";" tab "|": a candidate qualifies only
/ if every sample row parses to the same field count above one column; most columns wins, ties keep
/ candidate order (comma first).  None qualifying: a one-column-SHAPED file (a strict majority of sample
/ rows one field under every candidate, none malformed, no NUL bytes, no ragged-row lever on) loads
/ whole lines as ONE column; anything else - a polluted candidate, a stray junk line - keeps the loud
/ 'csv refusal.
/ @param opts (dict) DuckDB option vocabulary: delim (char), header (boolean), sample_size (long, sniff
/ rows), buffer_size (long, read-chunk bytes), dateformat / timestampformat (string, strptime subset
/ %Y %y %m %d %H %M %S %f %z - an explicit format REPLACES the built-in grammar for its type; day-first
/ and month-first forms are ambiguous as forms, so without one they stay text.  %z (timestampformat only)
/ reads Z or +-HH[[:]MM] and applies it, storing UTC, DuckDB's own semantics; zone NAMES need tz data
/ this build does not carry, so %Z is not in the subset and signals 'option like any unknown specifier)
/ THE DIALECT OPTIONS (explicit only, NEVER sniffed - a non-default quote or comment dialect is always
/ the caller's statement): quote (char; default "; "" disables quoting - quotes are ordinary bytes);
/ escape (char; default = the quote char, i.e. RFC "" doubling; inside a quoted field the escape
/ consumes its follower - esc+quote a literal quote, esc+esc a literal esc, esc+any that byte);
/ comment (char; default off) - a line whose FIRST byte is the comment char is skipped whole, an
/ unquoted comment char mid-row truncates the row there, a quoted field protects it; comment lines are
/ requested dialect: never counted, never rejected, though they still advance the physical line numbers
/ THE TOLERANCE LAW (all default off, DuckDB's own strict posture): options govern whether the load
/ SURVIVES a bad row, never whether it is counted - every bad row leaves a reject record (line; column;
/ error class cast/toofewcolumns/toomanycolumns/unquotedvalue/unterminatedquote/padded; raw csvLine) and
/ bumps the summary's `rejected`.  ignore_errors (boolean) skip bad rows; null_padding (boolean) pad
/ short rows with nulls, each padded row recorded; strict_mode (boolean, default 1b) - 0b reads a field
/ whose quotes cannot parse as literal bytes to the next delimiter, every salvaged row recorded;
/ store_rejects (boolean) continue AND store the reject records in a global table, replaced per load;
/ rejects_table (symbol, default `reject_errors, landing like any q assignment) names it and implies
/ store_rejects
/ @throws option for an unknown or unimplemented option key; csv for a malformed file or a frozen-type miss
.csv.read:{[file;target;types;opts] .csv.i.read[file;target;types;opts]};

/ sniff-only introspection: the inferred schema as `col1`col2!"jf" - usable directly as .csv.read's types.
/ ADVISORY on text (csvguess-style): where low cardinality suggests a symbol column the dict says "s",
/ but .csv.read itself never syms a sniffed column - feed this dict back as types to adopt the advice.
/ Reads only the sniff sample (sample_size rows), so it does not validate the rest of the file.
/ @param file (symbol) file symbol
/ @param opts (dict) the same options dict .csv.read takes
.csv.info:{[file;opts] .csv.i.info[file;opts]};
