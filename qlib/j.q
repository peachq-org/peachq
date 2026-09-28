/ Read JSON as a table: .j.read loads a document or JSON Lines from a file, a URL or a string, .j.info shows the
/ schema it would use.  `select from `:data.json` is the shortest spelling.  .j.j and .j.k serialize and parse a
/ value and are core q, always loaded.
/ .
/ @eg
/ select from `:https://www.timestored.com/data/sample/price.json where symbol like "*BTC"
/ select english, ineffective from `:https://www.timestored.com/data/sample/types.json where english like "F*"
/ `:t.json 0: enlist .j.j ([] a:1 2; b:("x";"y"))
/ select from `:t.json where a>1
/ .j.read["[{\"a\":1},{\"a\":2}]";::;::;()!()]

.pq.load_natives`j;

/ @ignore
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

/ Load a JSON document as a table.  An object gives a one-row table, an array of objects one row per object, any
/ other array a single `json column; a scalar or null signals 'type.  Numbers keep the form they were written in
/ (1 long, 1.0 float); a quoted number stays text.  Records match by name, key order is free, an omitted key is
/ null.  Types are inferred from the first sample_size records and then fixed.  Nested objects load recursively.
/ @param source `:data.json, a URL, the JSON text as a string, or a list of strings joined with newlines.  On an
/ http(s) URL a `#`results fragment spells the path option
/ @param target :: answers the table; a symbol names a global table the rows are inserted into (created when absent,
/ upserted when keyed; its schema outranks the sniff); a rank-3 lambda {[tblData;errData;misc] ...} is called once
/ with the whole document.  A symbol or lambda target answers the summary dict `rows`rejected`chunks`ignored`types
/ @param types a dict of column to type char from "bgxhijefcspmdznuvt", or a type-char string for the whole schema;
/ text parses through the CSV cell parser ("2026.08.25" under "d" is a date), anything else is cast; "*" leaves the
/ column as read, " " drops it
/ @param opts a dict; any unknown key signals 'option.  path (symbol, long or a list of them: the value read out of
/ each document, so `results, `data`items and (`data;`tables;0) are paths); format (`array, `newline_delimited or
/ `auto); records (1b every value is a record, 0b never expand, or `auto); sample_size (long); xcol (symbol vector
/ or dict renaming columns before the target is consulted); dateformat / timestampformat (the strptime subset
/ .csv.read takes); ignore_errors, store_rejects, rejects_table (symbol, default `reject_errors)
/ @throws option an unknown option key
/ @throws parse a malformed document, or a framing the file does not have
/ @throws type a document that is not a table shape, a value that does not fit its column, or a key first seen
/ after the sample
/ @throws domain a path step this document cannot take
/ @throws dup a repeated key in one record
/ @throws mismatch explicit types disagreeing with an existing target table
/ @eg .j.read["{\"a\":1}\n{\"a\":2}\n";::;::;()!()]
/ @eg .j.read["{\"status\":\"OK\",\"results\":[{\"t\":\"2026.01.01\"}]}";::;(enlist `t)!enlist "d";(enlist `path)!enlist `results]
/ @eg .j.read["[{\"a\":1}]";`t;::;()!()]
.j.read:{[source;target;types;opts] r:.j.i.frag[source;opts]; .j.i.read[r 0;target;types;r 1]};

/ The schema .j.read would use, as the dict its types argument takes, without loading the document.
/ A nested column, or one whose values do not share a q type, answers "*".  Only sample_size records are read.
/ @param opts the options dict .j.read takes
/ @return a dict of column name to type char
/ @eg .j.info["[{\"a\":1,\"b\":\"x\",\"o\":{\"p\":1}}]";()!()]
.j.info:{[source;opts] r:.j.i.frag[source;opts]; .j.i.info[r 0;r 1]};
