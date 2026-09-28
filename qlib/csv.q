/ Read delimited text (CSV, TSV) as a table, from a file, a URL or text in memory.
/ `select from `:trades.csv` is the shortest spelling; .csv.read adds a target table, explicit column types and
/ options; .csv.info shows the schema it would sniff.  What a cell means is shared with .j.read.
/ .
/ @eg
/ select from `:https://www.timestored.com/data/sample/dowjones.csv where Date>1960.01.01
/ select MIC, CITY from `:https://www.timestored.com/data/sample/iso10383_mic.csv where CITY like "LONDON"
/ `:trades.csv 0: csv 0: ([] sym:`a`b; px:1.5 2.5)
/ select from `:trades.csv where px>2
/ t:.csv.read[`:trades.csv;::;(enlist `sym)!enlist "s";()!()]

.pq.load_natives`csv;

/ Load delimited text as a table.  A symbol is a resource to open, text is the content itself.
/ Types resolve as: explicit types > an existing target table's schema > the sniff, and are fixed after the sniff
/ sample: a later cell that fails its type signals 'csv.  Sniffed text is a string column; ask for symbols with types.
/ The delimiter is sniffed among , ; tab |; quote, escape and comment are never sniffed.
/ @param file `:trades.csv, a URL, the CSV text as a string, or a list of strings joined with newlines
/ @param target :: answers the table; a symbol names a global table each batch is inserted into (created when absent,
/ upserted when keyed; its meta fixes the types, columns it lacks are dropped and reported under `ignored); a rank-3
/ lambda {[tblData;errData;misc] ...} is called per batch - errData the batch's reject records (line column error
/ csvLine), misc the dict `chunk`rows.  A symbol or lambda target answers the summary dict `rows`rejected`chunks`ignored`types
/ @param types a dict of column to type char from "bgxhijefcspmdznuvt", or a type-char string for the whole schema, one
/ char per column; " " drops a column, "*" keeps it as a string
/ @param opts a dict; any unknown key signals 'option.  xcol (symbol vector renaming the first columns, or a dict of
/ file name to new name, applied before the target is consulted); delim (char); header (boolean); sample_size (long,
/ sniff rows); buffer_size (long, read-chunk bytes); skip (long, records dropped off the front); dateformat /
/ timestampformat (strptime subset %Y %y %m %d %H %M %S %f %z, replacing the built-in grammar for that type); quote
/ (char, default "; "" disables quoting); escape (char, default the quote char); comment (char, default off).
/ Bad-row levers, all default off: ignore_errors (skip bad rows); null_padding (pad short rows); strict_mode (default
/ 1b; 0b reads unparseable quotes as bytes); store_rejects (keep the reject records in a global table);
/ rejects_table (symbol, default `reject_errors)
/ @throws option an unknown or unimplemented option key
/ @throws csv a malformed file, an empty payload, or a cell that fails its fixed type
/ @throws length an xcol key naming no column
/ @throws mismatch explicit types disagreeing with an existing target table
/ @eg .csv.read["sym,px\na,1.5\nb,2.5\n";::;(enlist `sym)!enlist "s";()!()]
/ @eg .csv.read["h\nx\n1\n2\n";::;::;(enlist `skip)!enlist 1]
/ @eg .csv.read["a\n1\n2\n";{[tblData;errData;misc] show count tblData};::;()!()]
.csv.read:{[file;target;types;opts] .csv.i.read[file;target;types;opts]};

/ The schema .csv.read would infer, as the dict its types argument takes, without loading the file.
/ Advisory "s" marks a low-cardinality text column .csv.read alone would keep as strings.  Only the sniff sample is read.
/ @param opts the options dict .csv.read takes
/ @return a dict of column name to type char
/ @eg .csv.info["a,b\n1,x\n";()!()]
/ @eg .csv.read["a,b\n1,x\n";::;.csv.info["a,b\n1,x\n";()!()];()!()]
.csv.info:{[file;opts] .csv.i.info[file;opts]};
