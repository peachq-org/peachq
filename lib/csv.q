/ csv.q — THE public .csv surface: the incremental CSV reader (src/qlang/io/q_csv.c).
/ Lambdas, not natives, so the surface is DISCOVERABLE: typing `.csv.read` prints the signature and names its
/ arguments.  The engine is a bytes-in / typed-rows-out core behind a chunked file driver: an RFC-4180 quoted
/ field may span read chunks (or hold newlines) and still lands as one cell of one row.
/ Column types FREEZE after the sniff sample; a later cell that fails its frozen type signals 'csv — never a
/ silent null.  An option is NEVER silently ignored: unknown (or not-yet-implemented) keys signal 'option.
/ ANY-ORDER LAW: definitions only at top level.

/ load a CSV file through the incremental core.
/ A symbol or lambda target answers the summary dict `rows`rejected`chunks`ignored`types; otherwise the
/ table itself.  Parse types resolve as: explicit types > the existing target table's schema > the sniff —
/ an EXISTING symbol target fixes the per-column types from its meta (keyed targets upsert, unkeyed insert),
/ CSV columns it lacks are dropped and reported under `ignored`, and a headerless file maps to a target
/ positionally — complete map, no projection.  All other conformity (missing columns, types) is insert's
/ own contract.
/ @param file (symbol) file symbol, e.g. `:trades.csv
/ @param target (symbol) a global table name each batch is inserted into (created when absent), or a rank-3
/ lambda {[tblData;errData;misc] ...} called once per batch — errData is () and misc generic null until the
/ rejects work lands
/ @param types (dict) column syms to type chars, e.g. `a`b!"sj", overriding the sniff per column (a " "
/ value drops that column by name); or a type-char string "sj f" as the complete schema, one char per
/ column, " " dropping a column, "*" keeping strings
/ @param opts (dict) DuckDB option vocabulary: delim (char), header (boolean), sample_size (long, sniff
/ rows), buffer_size (long, read-chunk bytes)
/ @throws option for an unknown or unimplemented option key; csv for a malformed file or a frozen-type miss
.csv.read:{[file;target;types;opts] .csv.i.read[file;target;types;opts]};

/ sniff-only introspection: the inferred schema as `col1`col2!"jf" — usable directly as .csv.read's types.
/ ADVISORY on text (csvguess-style): where low cardinality suggests a symbol column the dict says "s",
/ but .csv.read itself never syms a sniffed column — feed this dict back as types to adopt the advice.
/ Reads only the sniff sample (sample_size rows), so it does not validate the rest of the file.
/ @param file (symbol) file symbol
/ @param opts (dict) the same options dict .csv.read takes
.csv.info:{[file;opts] .csv.i.info[file;opts]};
