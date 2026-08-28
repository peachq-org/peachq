/ csv.q - THE public .csv surface: the incremental CSV reader (src/qlang/io/q_csv.c).
/ Lambdas, not natives, so the surface is DISCOVERABLE: typing `.csv.read` prints the signature and names its
/ arguments.  The engine is a bytes-in / typed-rows-out core behind two chunked drivers - a file and an
/ in-memory one (THE SOURCE LAW below) - so an RFC-4180 quoted field may span chunks (or hold newlines) and
/ still lands as one cell of one row.
/ Column types FREEZE after the sniff sample; a later cell that fails its frozen type signals 'csv - never a
/ silent null.  An option is NEVER silently ignored: unknown (or not-yet-implemented) keys signal 'option.
/ The doc below is the NORMATIVE statement of the laws; user-docs/csv.md is the user-facing guide and restates
/ them with worked examples.  Where the two disagree this file wins, and the guide is the one that must change.
/ ANY-ORDER LAW: definitions only at top level.

/ load CSV through the incremental core - from a file, or from text already in memory.
/ A symbol or lambda target answers the summary dict `rows`rejected`chunks`ignored`types; otherwise the
/ table itself.  Parse types resolve as: explicit types > the existing target table's schema > the sniff -
/ an EXISTING symbol target fixes the per-column types from its meta (keyed targets upsert, unkeyed insert),
/ CSV columns it lacks are dropped and reported under `ignored`, and a headerless file maps to a target
/ positionally - complete map, no projection.  All other conformity (missing columns, types) is insert's
/ own contract.
/ THE SOURCE LAW: a SYMBOL is a path, TEXT is content - never sniffed.  Text is one char vector (the
/ whole payload, embedded newlines and all) or a list of them, DEFINED as join-with-newline-then-parse
/ and NOT one element per record: only that reading rejoins a quoted field split across two elements.
/ Text chunks at the same buffer_size a file is read at, so `chunks and the target's misc match either
/ way; the BOM strip is the same, and an empty payload states no schema ('csv, the zero-byte law).
/ @param file (symbol or string or string list) `:trades.csv, or the CSV text itself
/ @param target (symbol) a global table name each batch is inserted into (created when absent), or a rank-3
/ lambda {[tblData;errData;misc] ...} called once per batch - errData is that batch's reject records as a
/ table (line, column, error, csvLine; empty on a clean batch), misc the dict `chunk`rows (0-based batch
/ index; rows landed so far)
/ The sniffer reads exactly what `csv 0:` writes (kdb-formats law, owner 2026-08-22): dashed dates,
/ dotted-D timestamps (plus the ISO T- and space-separated alternates), m-suffixed months, hh:mm minute,
/ hh:mm:ss second, 3-digit-fraction time and D-separated timespan - clock shapes promote per column up
/ the minute < second < time < timespan lattice.  It never guesses beyond a written form: bare 20260822
/ stays long, suffixless 2026.08 stays float, 1/0 stays long (type booleans and syms explicitly).
/ THE \z DECLARATION (owner 2026-08-27): the slash/dash 4-digit-year day-order pairs (03/10/2024,
/ 12-13-2004, and either with a seconds clock) SNIFF under the field order \z states - 0 month-first,
/ 1 day-first, "D"$'s own switch.  A global \z is a declaration, not a guess, which is why honouring it
/ keeps the never-guess law; a value the declared order cannot read sniffs text (the near-miss law), and
/ a shape no declaration disambiguates - 2-digit years, single-digit fields, the dotted pair - stays
/ text under either order.  A sniffed day-order column is therefore \z-dependent, as "D"$ already is.
/ THE WRITER'S FORM IS THE GRAMMAR (owner 2026-08-27): two more shapes join it because q PRINTS them and
/ so goal 1 - loading our own output - needs them.  A canonical 36-char uuid types g (the IPv4/IPv6 forms
/ "G"$ also reads are the CAST's, not a written form, so they stay text and refuse under an explicit g).
/ "0x" + EXACTLY two hex digits types x; longer stays text, because 0xFF0000 is a byte VECTOR and a cell
/ holds one atom, and BARE hex stays text because 0a is likelier an identifier - the same call 007 gets.
/ DuckDB's read_csv leaves a uuid VARCHAR and reads 0x0a as the decimal 10 while its OWN read_json types
/ that uuid UUID and that byte VARCHAR: it disagrees with itself, so no verdict is parity with both, and
/ goal 1 takes the CSV side.  Recorded as the guid / hexbyte grant classes in qlib/test/csvDiffTest.q.
/ THE SENTINEL RULE (owner 2026-08-27): a digit run landing EXACTLY on 0W, -0W or 0N would read as q's
/ infinity or null - a silently wrong meaning - so in the sample it demotes the column to text, and after
/ the freeze it is the miss law: 'csv, or a `cast reject under a lever.  It is a per-WIDTH rule, so h and i
/ refuse their own sentinel runs the way j refuses the long's.
/ THE DECLARED VOCABULARY (owner 2026-08-27) is all 18 basic types, five of them reachable ONLY here because
/ the sniff never claims them: h i e (narrower widths of a written number), c and z.  c is exactly ONE
/ character - one cell is one atom, so a longer cell is a miss rather than a char VECTOR in a single cell,
/ and text is what * is for.  z is p's cell read out as the legacy datetime: it accepts what p accepts and
/ refuses what p refuses, while the SNIFFER still types a T-separated token p - declared beats sniffed.
/ THE TZ POSTURE (owner 2026-08-23; explicit-p widened 2026-08-27): kdb parses no timezone, so a tz-suffixed
/ cell (...+02:00, ...Z) is TEXT at defaults, bytes intact - never a silent shift.  An offset is read only
/ where the caller asks with timestampformat's %z, and then it is APPLIED: the stored timestamp is UTC.
/ Under an explicit p with no format a ZERO-offset tail - Z, z, +00:00 - parses, because the digits already
/ ARE the UTC instant and nothing shifts, and so does minute resolution (2025-10-06T10:57); a NON-ZERO
/ offset stays a frozen-type MISS ('csv, or a `cast reject under a continue-mode lever), as do -00:00
/ (RFC 3339: offset UNKNOWN, not UTC) and compact ISO - DuckDB's forced-TIMESTAMP path drops the offset
/ silently; we refuse instead.  THE LOADER-VS-CAST ASYMMETRY: the loaders accept more than the kx cast, and
/ less - Z parses here while "P"$"...Z" stays 0Np (Tok is kdb-faithful and does not change), and T alone is
/ a Tok boolean but loader text.  "X"$"0x0a" is 0x00 and STAYS 0x00 - Tok reads BARE hex, so the prefix
/ makes it consume an invalid pair - while the loader reads the prefixed form, which is the one q writes.
/ The same cell law serves .j.read: what a cell means is decided once.
/ A blank physical line is SKIPPED (DuckDB emits a row of nulls).  A file with a header row and no data
/ rows loads as a zero-row table with the header's columns; a zero-byte file signals 'csv - a file that
/ states no schema does not get one invented (DuckDB invents a VARCHAR column0).
/ THE SKIP LAW: skip drops that many records off the FRONT before anything else reads them, so every sniff -
/ header, delimiter, types - runs on what remains.  A blank line COUNTS toward skip; a comment line does not
/ (comment is dialect, transparent everywhere).  A quoted multi-line record counts ONCE, where DuckDB's skip
/ runs before its tokenizer and slices the record's physical lines - a granted divergence, since one carry
/ scan cannot hold two contradictory notions of a line.  Reject records keep PHYSICAL line numbers: skip
/ renumbers nothing.  A negative skip signals 'domain (0 is the default, not a degenerate value), and
/ skipping past the last record leaves a stream that states no schema - 'csv, exactly as a zero-byte file.
/ @param types (dict) column syms to type chars from the 18 basic types "bgxhijefcspmdznuvt", e.g. `a`b!"sj",
/ overriding the sniff
/ per column (a " " value drops that column by name); or a type-char string "sj f" as the complete
/ schema, one char per column, " " dropping a column, "*" keeping strings
/ Without an explicit delim the delimiter is sniffed among "," ";" tab "|": a candidate qualifies only
/ if every sample row parses to the same field count above one column; most columns wins, ties keep
/ candidate order (comma first).  None qualifying: a one-column-SHAPED file (a strict majority of sample
/ rows one field under every candidate, none malformed, no NUL bytes, no ragged-row lever on) loads
/ whole lines as ONE column; anything else - a polluted candidate, a stray junk line - keeps the loud
/ 'csv refusal.
/ @param opts (dict) DuckDB option vocabulary: delim (char), header (boolean), sample_size (long, sniff
/ rows), buffer_size (long, read-chunk bytes), skip (long, default 0, see THE SKIP LAW above), dateformat /
/ timestampformat (string, strptime subset %Y %y %m %d %H %M %S %f %z - an explicit format REPLACES the
/ built-in grammar for its type, \z's day-order reading included.  %z (timestampformat only)
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
/ @param file (symbol or string or string list) the same source forms .csv.read takes
/ @param opts (dict) the same options dict .csv.read takes
.csv.info:{[file;opts] .csv.i.info[file;opts]};
