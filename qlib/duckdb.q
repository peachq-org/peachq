/ Query and write DuckDB databases from q: a database is a handle, its tables are q tables.
/ .pq.hopen_duckdb[`alias;"/path/db"] opens or creates a database and answers the alias symbol `:pq:duckdb:alias, the
/ handle every .duckdb verb takes.  h "SELECT ..." runs SQL and answers a table; hclose h closes it.
/ `:pq:duckdb:alias:table/ names a table for get, set, upsert and qsql.
/ .
/ One DuckDB database lives in the process, `:pq:duckdb:main (.duckdb.main[]; q -duckdb path puts it in a file):
/ every alias is a catalog attached to it, and a q global bound to a DuckDB table is a same-named view in it, so
/ s)SELECT ... runs SQL over q names, live.  DuckDB's httpfs carries s3:// gcs:// hf:// URLs for read0, read1,
/ select from and set, with credentials as DuckDB secrets.  .duckdb.getx[h;`t] reads a table with its schema as the
/ pair (data;schema), which set and upsert accept back.  \?duckdb has more examples.
/ .
/ @eg
/ h:.pq.hopen_duckdb[`demo;":memory:"]
/ `:pq:duckdb:demo:trade/ set ([] sym:`a`b; px:1.5 2.5)
/ `:pq:duckdb:demo:trade/ upsert ([] sym:enlist `c; px:enlist 3.5)
/ select from `:pq:duckdb:demo:trade/ where px>2
/ h "SELECT sym, px*2 AS px2 FROM trade"
/ trade:get `:pq:duckdb:demo:trade/
/ s)SELECT sym, count(*) FROM trade GROUP BY sym
/ `:pq:duckdb:demo:dow/ set select from `:https://www.timestored.com/data/sample/dowjones.csv
/ select max Price by 10 xbar `year$Date from `:pq:duckdb:demo:dow/
/ h "SELECT year(Date) AS yr, max(Price) FROM dow GROUP BY yr ORDER BY yr DESC LIMIT 3"
/ hclose h

.pq.load_natives`duckdb;

/ @ignore
.duckdb.call:{[c;q;sync] if[not sync; '`domain]; .duckdb.lastsql::q; .duckdb.i.exec[c;q]}
/ @ignore
.duckdb.bind:{[c;t] $[.duckdb.i.foreign[c;t];.duckdb.i.ftable[c;t]`cols;(key .duckdb.i.meta[c;t])`c]}
/ @ignore
.duckdb.rebind:{[c;t] .duckdb.i.unbind[c;t]; .duckdb.bind[c;t]}
/ @ignore
.duckdb.info:{[c;t] .duckdb.i.info[c;t]}
/ A table of the handle's catalog as a q table.  A table q wrote reads back as written.  Any other table reads by one q type
/ per DuckDB type, with no companion columns: NULL, INT_MIN and NaN are the q null, a NULL VARCHAR is "", a NULL BOOLEAN 0b,
/ a NULL list empty; VARCHAR is a string, DECIMAL a float, TIME a timespan, TIMESTAMPTZ its UTC instant; a timestamp past
/ q's range is 0Wp, one before it -0Wp, a date outside it 'duckdb; .duckdb.inspect counts all of these; a column whose type q does not hold is left out (.duckdb.hidden names them).  Rows come in
/ rowid order where the table has one.  .duckdb.getx and SQL read every column exactly.
.duckdb.get:{[c;t] $[.duckdb.i.foreign[c;t];.duckdb.i.fread[c;t;.duckdb.i.fpull];.duckdb.i.get[c;t]]}
/ Create or replace a table of the handle's catalog from a q table, or from the pair .duckdb.getx answers.
.duckdb.set:{[c;t;d] .duckdb.i.set[c;t;d]; t}
/ @ignore
.duckdb.append:{[c;t;d] .duckdb.i.append[c;t;d]; t}
/ @ignore
.duckdb.upsert:.duckdb.append
/ The meta of a table of the handle's catalog, read from DuckDB without fetching a row.
.duckdb.meta:{[c;t]
  if[not .duckdb.i.foreign[c;t]; :.duckdb.i.meta[c;t]];
  v:.duckdb.i.ftable[c;t]; n:count v`cols; ([c:v`cols] t:v`t; f:n#`; a:n#`)}
/ The columns of a table that q does not show because it has no type for theirs: absent from cols, meta, get and select,
/ read by SQL and .duckdb.getx.  A table q wrote hides none.
/ @return a table ([] col; dtype), dtype as DuckDB declares it
/ @eg
/ .duckdb.exec[.duckdb.main[];"CREATE OR REPLACE TABLE u AS SELECT 1 AS a, 1::HUGEINT AS h"]
/ .duckdb.hidden[.duckdb.main[];`u]
.duckdb.hidden:{[c;t] $[.duckdb.i.foreign[c;t];.duckdb.i.ftable[c;t]`hidden;([] col:0#`; dtype:())]}
/ Where a table of the handle's catalog does not read into q exactly: one row per column and issue, counted by DuckDB in
/ one statement over the table, so no row of it reaches q.  A table q wrote answers no row.  The issues: hidden (no q
/ type; qtype " ", rows null), range (a timestamp outside q's range, read as 0Wp or -0Wp, or a date, which fails the
/ read), minimum (an integer type's lowest value, read as null), merged (NULL beside NaN, the zero UUID, '' or an empty
/ BLOB, or an ENUM's '', which read as one value),
/ zero (a NULL BOOLEAN or UTINYINT, read as 0b or 0x00), digits (a DECIMAL wider than 15 digits whose value a float does
/ not hold) and offset (a TIMESTAMPTZ, read as its UTC instant).  A list column's elements are not inspected.
/ @param c the handle whose catalog holds the table, as .duckdb.hidden takes it
/ @param t the table's name, a symbol
/ @return a table ([] col; dtype; qtype; issue; rows): dtype as DuckDB declares it, qtype the meta char q reads it as,
/ rows how many rows the issue touches
/ @eg
/ .duckdb.exec[.duckdb.main[];"CREATE OR REPLACE TABLE u AS SELECT * FROM (VALUES (1, TIMESTAMP '9999-12-31'), (NULL, NULL)) t(a, p)"]
/ .duckdb.inspect[.duckdb.main[];`u]
.duckdb.inspect:{[c;t] $[.duckdb.i.foreign[c;t];.duckdb.i.fread[c;t;.duckdb.i.finspect];.duckdb.i.NOISSUE]}
/ @ignore
.duckdb.i.NOISSUE:([] col:0#`; dtype:(); qtype:""; issue:0#`; rows:0#0N)

/ Run SQL on a handle and answer the result as a table; a statement without a result answers ::.  sql is a string, or the
/ pair (string;dict) binding each $name by name or (string;list) binding $1, $2 ... by position; values are atoms or strings
/ (a null is SQL NULL), DuckDB's refusal is 'duckdb with the reason in .duckdb.err[], anything else 'type.  The triple
/ (string;params;schema), params () for none and schema the table getx answers, reads the result as .duckdb.get reads a table.
/ A fourth item 1b reads a result q did not write: a column named like a companion (x_q_isnull beside x) is a column.
/ @eg .duckdb.exec[.duckdb.main[];("SELECT 'a' AS s";();([] col:enlist `s; dtype:enlist "VARCHAR"; logical:enlist `symbol))]
/ @eg .duckdb.exec[.duckdb.main[];"SELECT 1 AS one, 'a' AS s"]
/ @eg .duckdb.exec[.duckdb.main[];("SELECT $a + $b AS n";`a`b!1 2)]
.duckdb.exec:{[handle;sql] .duckdb.i.exec[handle;sql]}

/ A table with its DuckDB schema, as the pair (data;schema) that set and upsert accept back.
.duckdb.getx:{[c;t] .duckdb.i.getx[c;t]}
/ SQL's answer with its DuckDB schema, as the pair (data;schema); sql as .duckdb.exec takes it, parameters included.
.duckdb.execx:{[c;sql] .duckdb.i.execx[c;sql]}

/ The $name parameters a statement (string) declares, distinct and in first-occurrence order; $1 and ? are named by number.
/ @eg .duckdb.params "SELECT $x FROM t WHERE s='$y' AND n>$z"
.duckdb.params:{[sql] .duckdb.i.params sql}

/ The reason behind the last 'duckdb signal, first line the cause, cleared by the next call.
/ err[] is any connection's, err[handle] that connection's.
/ @return string
/ @eg @[.duckdb.exec[.duckdb.main[]];"SELECT * FROM no_such_table";{.duckdb.err[]}]
.duckdb.err:{[handle] .duckdb.i.err handle}

/ The DuckDB-to-q type map as a table, one row per contract entry.
/ @col dtype the DuckDB DDL spelling
/ @col ktype the .duckdb.meta type char
/ @col logical the logical type name
/ @col canon whether a bare read of that DuckDB type produces this row
/ @col needs what a schema row must carry to round-trip: none, logical, dtype or both
/ @eg select from .duckdb.types[] where canon
.duckdb.types:{[] .duckdb.i.types[]}

/ @ignore
.duckdb.i.lit:{[v]
  $[-1h=type v;$[v;"true";"false"];
    (type v) in -5 -6 -7 -8 -9h;string v;
    -11h=type v;.duckdb.i.lit string v;
    10h=type v;"'",ssr[v;"'";"''"],"'";
    11h=type v;"[",(", " sv .duckdb.i.lit each v),"]";
    '`type]}
/ @ignore
.duckdb.i.ident:{[k] s:string k; if[not (0<count s)&all s in .Q.an; '`type]; s}
/ @ignore
.duckdb.i.none:{[v] any v~/:(();()!();(::))}
/ @ignore
.duckdb.i.opts:{[f;opts] $[.duckdb.i.none opts;();(99h=type opts)and 11h=type key opts;f'[key opts;value opts];'`type]}
/ @ignore
.duckdb.i.file:{[f] .duckdb.i.lit $[-11h=type f;$[":"=first s:string f;1_s;s];10h=type f;f;'`type]}

/ The foreign view, how a table without q's schema reads: a rule per logical name of .duckdb.types[], no rule = hidden.
/ @ignore
.duckdb.i.FMIN:`int16`int32`int64!("-32768";"-2147483648";"-9223372036854775808")
/ @ignore
.duckdb.i.FNULL:`float32`float64`guid!("'NaN'";"'NaN'";"'00000000-0000-0000-0000-000000000000'")
/ @ignore
.duckdb.i.FZERO:`utf8`bool`uint8`bytes!("''";"false";"0";"''::BLOB")
/ @ignore
.duckdb.i.FCAST:`decimal`timeus!("DOUBLE";"TIME_NS")
/ the first and last instant of each grain that reads as a q timestamp
/ @ignore
.duckdb.i.FSPAN:`timestampsec`timestampms`timestampus`timestamp!(("1707-09-22 00:12:44";"2262-04-11 23:47:16");
  ("1707-09-22 00:12:43.146";"2262-04-11 23:47:16.854");("1707-09-22 00:12:43.145225";"2262-04-11 23:47:16.854775");
  ("1707-09-22 00:12:43.145224194";"2262-04-11 23:47:16.854775806"))
/ a VARCHAR compares, groups and sorts by its bytes, as q's strings do, whatever collation its column or the session declares
/ @ignore
.duckdb.i.FBYTES:" COLLATE \"binary\""
/ @ignore
.duckdb.i.quote:{[s] "\"",ssr[s;"\"";"\"\""],"\""}
/ A timestamp outside the range a DuckDB timestamp reads into q (.duckdb.i.FSPAN) reads as the infinity on its side, so q
/ orders it as DuckDB orders the stored value against any literal inside that range.
/ @ignore
.duckdb.i.fclamp:{[x;dt;b]
  e:{[dt;v] "'",v,"'::",dt}[dt];
  "CASE WHEN ",x," > ",e[b 1]," THEN ",e["infinity"]," WHEN ",x," < ",e[b 0]," THEN ",e["-infinity"]," ELSE ",x," END"}
/ whether a finite date x lands above -0Wd, the q dates
/ @ignore
.duckdb.i.fdate:{[x] "date_diff('day', DATE '2000-01-01', ",x,") >= -2147483646"}
/ @ignore
.duckdb.i.fguard:{[nm;x;ok]
  "CASE WHEN ",x," IS NULL OR NOT isfinite(",x,") OR ",ok," THEN ",x,
    " ELSE error(",(.duckdb.i.lit "column ",nm,": a value outside q's range"),") END"}
/ One value x of logical lg and DuckDB type dt as the SQL that reads it, () for a logical with no rule; nm names the column
/ in an error.  TINYINT, USMALLINT, UINTEGER and TIME_NS widen into a carrier whose null no value of theirs reaches.
/ @ignore
.duckdb.i.fwrap:{[nm;lg;dt;x]
  $[lg in key .duckdb.i.FMIN;"nullif(",x,", (",.duckdb.i.FMIN[lg],")::",dt,")";
    lg in key .duckdb.i.FNULL;"nullif(",x,", ",.duckdb.i.FNULL[lg],"::",dt,")";
    lg in key .duckdb.i.FZERO;"coalesce(",x,$[lg=`utf8;.duckdb.i.FBYTES;""],", ",.duckdb.i.FZERO[lg],")";
    lg=`categorical;"coalesce(CAST(",x," AS VARCHAR), '')";
    lg=`bitstring;"coalesce(CAST(string_split(CAST(",x," AS VARCHAR), '') AS BOOLEAN[]), [])";
    lg in key .duckdb.i.FCAST;"CAST(",x," AS ",.duckdb.i.FCAST[lg],")";
    lg=`date;.duckdb.i.fguard[nm;x;.duckdb.i.fdate x];
    lg=`timestamptz;.z.s[nm;`timestampus;"TIMESTAMP";"timezone('UTC', ",x,")"];
    lg in key .duckdb.i.FSPAN;.duckdb.i.fclamp[x;dt;.duckdb.i.FSPAN lg];
    lg in `int8`uint16`uint32`timens;x;
    ()]}
/ One column, or one level of a list, x the SQL naming it: (sql;meta char;dtype;logical), the last two the schema row the read
/ needs ("" for none); () when the type is hidden.  ty is .duckdb.types[]'s canon rows, dtype!(logical;ktype).  A column that
/ is no list adds the DuckDB type the SQL answers, the canon logical, and whether q compares a literal with it as DuckDB
/ does (a timespan is a BIGINT to q's parameters and a TIME to DuckDB).
/ @ignore
.duckdb.i.fcol:{[ty;nm;dt;x;depth]
  if["]"=last dt;
    el:(i:last where dt="[")#dt; v:"x",string depth;
    if[()~e:.z.s[ty;nm;el;v;depth+1]; :()];
    src:$["[]"~i _ dt;x;"CAST(",x," AS ",el,"[])"];
    :("coalesce(",$[v~e 0;src;"list_transform(",src,", lambda ",v,": ",e[0],")"],", [])";$[e[1] in .Q.a;upper e 1;" "];
      $[count e 2;e[2],"[]";""];$[count e 2;`$"list(",string[e 3],")";`])];
  if[not (b:`$first "(" vs dt) in key ty; :()];
  if[()~w:.duckdb.i.fwrap[nm;lg:ty[b] 0;string b;x]; :()];
  sym:lg=`categorical; cast:lg in key .duckdb.i.FCAST;
  (w;$[cast;ty[`$.duckdb.i.FCAST lg] 1;ty[b] 1];$[sym;"VARCHAR";""];$[sym;`symbol;`];
    $[cast;.duckdb.i.FCAST lg;sym;"VARCHAR";lg=`timestamptz;"TIMESTAMP";string b];lg;not lg in `timeus`timens)}
/ One value x of a column q wrote as logical lg over the DuckDB type dt, as the SQL in which a value q would not have
/ written there reads as the q null: the foreign rule of dt for a NULL or a null pattern, level by level through a list.
/ The file's metadata says what the column is, not that q wrote its values, and on values q did write this changes none.
/ @ignore
.duckdb.i.fmerge:{[ty;dt;lg;x;depth]
  if["]"=last dt;
    v:"y",string depth; e:.z.s[ty;(last where dt="[")#dt;`$5_-1_string lg;v;depth+1];
    :"coalesce(",$[v~e;x;"list_transform(",x,", lambda ",v,": ",e,")"],", [])"];
  cl:$[(b:`$first "(" vs dt) in key ty;ty[b] 0;`];
  $[lg=`char;"coalesce(",x,.duckdb.i.FBYTES,", ' ')";
    cl in (key .duckdb.i.FMIN),(key .duckdb.i.FNULL),key .duckdb.i.FZERO;.duckdb.i.fwrap["";cl;string b;x];
    x]}
/ Whether logical lg over the DuckDB type dt is a pair q writes, and its meta char, level by level through a list: (0b;" ")
/ for no such pair (tt is .duckdb.types[]), and the column then reads by its DuckDB type alone.  A kind q holds as a
/ general list (a struct, a map) is q's own with the meta char " ", so the codec decodes it as q wrote it.
/ @ignore
.duckdb.i.fown:{[tt;dt;lg]
  if[null lg; :(0b;" ")];
  if["]"=last dt; :$[(s:string lg) like "list(*)";@[.z.s[tt;(last where dt="[")#dt;`$5_-1_s];1;upper];(0b;" ")]];
  k:exec ktype from tt where dtype=`$first "(" vs dt, logical=lg; $[count k;(1b;first k);(0b;" ")]}
/ THE foreign view of a source: rel its FROM text, sch its columns ([] col; dtype) as DuckDB declares
/ them, order the expressions its rows sort by (() for none).  Answers sel, the SELECT list; cols and t, what q shows and its
/ meta chars; schema, the rows .duckdb.exec's triple needs beside the statement; hidden, the columns left out; text, the
/ string columns a literal compares with as text; shown, each shown column's DuckDB type and foreign logical (` for one q
/ wrote, or a list).
/ sch may say what q wrote (a parquet file's q_schema) or what was declared (a table's .duckdb.i.decl) in one more
/ column, logical: a column whose logical q writes over
/ its DuckDB type is q's own and reads as that q type, its nulls merged as a foreign column's are (.duckdb.i.fmerge);
/ any other column reads as foreign.
/ @ignore
.duckdb.i.fview:{[rel;sch;order]
  tt:.duckdb.types[]; ty:exec dtype!flip (logical;ktype) from tt where canon; n:count sch;
  qn:.duckdb.i.quote each string sch`col; lg:$[`logical in cols sch;sch`logical;n#`];
  ok:.duckdb.i.fown[tt]'[sch`dtype;lg]; own:ok[;0];
  r:{[ty;nm;dt;x;o;k;l] $[o;(.duckdb.i.fmerge[ty;dt;l;x;0];k;dt;l;dt;`;1b);.duckdb.i.fcol[ty;nm;dt;x;0]]}[ty]'[
    string sch`col;sch`dtype;qn;own;ok[;1];lg];
  j:where 0=count each r; i:where 0<count each r; s:i where 0<count each r[i;2];
  sel:{[e;q] e[0]," AS ",q}'[r i;qn i];
  (`rel`order`sel`cols`t`schema`hidden`text`shown!(rel;order;sel;sch[`col] i;"c"$r[i;1];
    ([] col:sch[`col] s; dtype:r[s;2]; logical:r[s;3]);([] col:sch[`col] j; dtype:sch[`dtype] j);
    sch[`col] i where ("C"=r[i;1]) and not own i;
    ([] col:sch[`col] i; dtype:sch[`dtype] i; logical:{[x;o] $[o or 7>count x;`;x 5]}'[r i;own i])))}
/ a table is q's when its _q_schema rows carry the table row .duckdb.set writes; column rows alone declare columns
/ of a foreign table (.duckdb.i.decl), which its view reads as declared
/ @ignore
.duckdb.i.foreign:{[c;t] 0=count .duckdb.i.schema[c;t]}
/ @ignore
/ the kept foreign views, each (handle and table;stamp;view) in a place of its own, and how many have taken one: a table
/ past the last place takes the oldest's, so what closed handles and dropped tables leave behind is bounded
/ @ignore
.duckdb.i.fkept:64#enlist (`;();::)
/ @ignore
.duckdb.i.fturn:0
/ THE foreign view of a table of a handle, kept while the table stays bound as it was when the view was built (told by
/ when it was bound; by the handle's whole list of bound tables for a name the list spells otherwise): whatever lets a bound
/ table go (a write of ours, a non-SELECT through the SQL door, get, a failed reference) binds it anew, and the view is
/ then built anew.  Whether the relation has a row number to order by is asked once, as the view is built: a
/ DuckDB view has none, and its rows then come in no promised order.
/ @ignore
.duckdb.i.ftable:{[c;t]
  .duckdb.i.rows[c;t]; k:`$(string c),"/",string t; st:.duckdb.i.info[c;::]`tables;
  if[t in st`table; st:st[`bound] st[`table]?t];
  i:.duckdb.i.fkept[;0]?k; if[i<count .duckdb.i.fkept; if[st~.duckdb.i.fkept[i;1]; :.duckdb.i.fkept[i;2]]];
  sch:select col, dtype from .duckdb.i.info[c;t]`schema; rel:.duckdb.i.qname t;
  if[count d:.duckdb.i.decl[c;t]; sch:update logical:(d[`col]!d`logical) col from sch];
  ord:$[`rowid in sch`col;();@[{[c;rel] .duckdb.exec[c;"SELECT rowid FROM ",rel," LIMIT 0"]; enlist "rowid"}[c];rel;{[e] ()}]];
  if[i=count .duckdb.i.fkept; i:.duckdb.i.fturn mod count .duckdb.i.fkept; .duckdb.i.fturn+:1];
  v:.duckdb.i.fview[rel;sch;ord]; .duckdb.i.fkept[i]:(k;st;v); v}
/ The rows of a foreign view.  A view whose every column is hidden shows q nothing: the table of no columns and no rows.
/ @ignore
.duckdb.i.fpull:{[c;v]
  if[0=count v`cols; :flip (0#`)!()];
  .duckdb.exec[c;("SELECT ",(", " sv v`sel)," FROM ",v[`rel],$[count v`order;" ORDER BY ",", " sv v`order;""];();v`schema;1b)]}
/ A foreign view with the columns need alone, for a read of them: no other column is converted, so a value q cannot hold
/ in a column the read does not name fails nothing.  A read that names none still counts the rows.
/ @ignore
.duckdb.i.fnarrow:{[v;need]
  i:where v[`cols] in need;
  if[(0=count i) and 0<count v`cols; :@[v;`sel`cols`t`schema;:;(enlist "1 AS \"_q_1\"";1#`$"_q_1";1#"i";0#v`schema)]];
  @[v;`sel`cols`t`schema;:;(v[`sel] i;v[`cols] i;v[`t] i;select from v[`schema] where col in need)]}
/ whether DuckDB refused a statement at its names or types, before running it: what a bound table or a kept view that the
/ catalog has since left behind answers
/ @ignore
.duckdb.i.unbound:{[c] any .duckdb.err[c] like/: ("Binder Error*";"Catalog Error*")}
/ f[c;t] for a table of a handle.  The catalog no longer having what the bound entry lists (DDL from another
/ connection) binds it afresh, and f is run once more.
/ @ignore
.duckdb.i.rebound:{[c;t;f] .[f;(c;t);{[c;t;f;e] if[not (e~"duckdb") and .duckdb.i.unbound c; 'e]; .duckdb.i.unbind[c;t]; f[c;t]}[c;t;f]]}
/ f[c;view] over the foreign view of a table of a handle, run once more over the view bound afresh (.duckdb.i.rebound).
/ @ignore
.duckdb.i.fread:{[c;t;f] .duckdb.i.rebound[c;t;{[f;c;t] f[c;.duckdb.i.ftable[c;t]]}[f]]}
/ A select over a foreign view v, run by q on the rows pulled of the columns it reads (.pq.i.reads).
/ @ignore
.duckdb.i.fresidue:{[c;v;rt] q:.duckdb.i.ftext[v;rt]; (?) . (enlist .duckdb.i.fpull[c;.duckdb.i.fnarrow[v;.pq.i.reads[v`cols;q]]]),1_q}

/ The checks of one foreign column, quoted x, of logical lg and DuckDB type dt: each (issue;tests), the issue counted where
/ its one test holds, or, for two tests, on the rows of both once each holds somewhere (two values that read as one).
/ @ignore
.duckdb.i.fchecks:{[x;lg;dt]
  two:{[x;v] (x," IS NULL";x," = ",v)};
  $[lg in key .duckdb.i.FMIN;enlist (`minimum;enlist x," = (",.duckdb.i.FMIN[lg],")::",dt);
    lg in key .duckdb.i.FNULL;enlist (`merged;two[x;.duckdb.i.FNULL[lg],"::",dt]);
    lg in `utf8`bytes;enlist (`merged;two[x;.duckdb.i.FZERO lg]);
    lg in `bool`uint8;enlist (`zero;enlist x," IS NULL");
    lg=`categorical;enlist (`merged;two[x;"''"]);
    lg=`date;enlist (`range;enlist "isfinite(",x,") AND NOT ",.duckdb.i.fdate x);
    lg in key .duckdb.i.FSPAN;enlist (`range;enlist "isfinite(",x,") AND NOT ",x," BETWEEN ",
      " AND " sv {[dt;b] "'",b,"'::",dt}[dt] each .duckdb.i.FSPAN lg);
    lg=`timestamptz;.z.s["timezone('UTC', ",x,")";`timestampus;"TIMESTAMP"],enlist (`offset;enlist x," IS NOT NULL");
    (lg=`decimal) and 15<"J"$first "," vs -1_8_dt;enlist (`digits;enlist "TRY_CAST(CAST(",x," AS DOUBLE) AS ",dt,") IS DISTINCT FROM ",x);
    ()]}
/ What does not read exactly in a foreign view v, by one statement over its relation that answers counts alone.
/ @ignore
.duckdb.i.finspect:{[c;v]
  h:v`hidden; n:count h;
  r:.duckdb.i.NOISSUE,([] col:h`col; dtype:h`dtype; qtype:n#" "; issue:n#`hidden; rows:n#0N);
  s:select from v[`shown] where not null logical;
  ck:raze {[x;cl;dt;lg] {[cl;dt;k] (cl;dt;k 0;k 1)}[cl;dt] each .duckdb.i.fchecks[x;lg;dt]}'[
    .duckdb.i.quote each string s`col;s`col;s`dtype;s`logical];
  if[0=count ck; :r];
  ts:raze ck[;3]; nt:count each ck[;3]; at:sums 0,-1_nt;
  q:"SELECT ",(", " sv {[i;t] "count(*) FILTER (WHERE ",t,") AS \"_q_",string[i],"\""}'[til count ts;ts])," FROM ",v`rel;
  m:"j"$value first .duckdb.exec[c;q];
  k:{[m;a;n] x:m a+til n; $[1=n;first x;all 0<x;sum x;0]}[m]'[at;nt];
  if[0=count w:where 0<k; :r]; qt:(v[`cols]!v`t) ck[w;0];
  r,([] col:ck[w;0]; dtype:ck[w;1]; qtype:qt; issue:ck[w;2]; rows:k w)}
/ Leniency answers only what q refuses.  Against a string column (sc names them) of a table q did not write a SYMBOL
/ compares as its text, where q signals 'type: c=`a, c<>`a and c in `a`b compare whole strings, a symbol pattern of like is
/ its text, and an order (c<`a, c within `a`b, either way round) compares the column as the symbols it holds, `$c.  A string
/ literal keeps q's meaning (c="ab" is char-wise), and string c is c.  rt is a select as (table;where terms;by;phrase;...),
/ its names resolved (.pq.i.resolve); a rewritten compare is in over strings, which q runs.
.duckdb.i.either:{[sc;rt]
  f:{[sc;e]
    if[(0h<>.Q.c.lty e) or 2>count e; :e];
    e:@[e;where 101h<>type each e;.z.s[sc]]; h:e 0; a:1_e; col:{[sc;x] $[-11h=type x; x in sc; 0b]}[sc];
    if[(1=count a) and (string)~h; :$[col a 0; a 0; e]];
    if[2<>count a; :e];
    sy:{[x] $[11h=type x; $[1=count x; enlist string first x; ()]; (0h=type x) and 1=count x; $[11h=type first x; string first x; ()]; ()]};
    if[any h~/:((=);(<>)); if[col a 1; a:reverse a];
      if[col[a 0] and 1=count s:sy a 1; r:(in;a 0;(enlist),s); :$[(=)~h; r; (not;r)]]];
    if[(in)~h; s:sy a 1; if[col[a 0] and 0<count s; :(in;a 0;(enlist),s)]];
    if[(like)~h; if[col[a 0] and 1=count s:sy a 1; :(like;a 0;first s)]];
    o:((<);(>);(<=);(>=)); i:(o~\:h)?1b;
    if[i<4; if[col a 1; a:reverse a; i:1 0 3 2 i];
      if[col[a 0] and 1=count s:sy a 1; :(o i;($;enlist `;a 0);enlist `$first s)]];
    if[(within)~h; if[col[a 0] and 2=count s:sy a 1; :(within;($;enlist `;a 0);enlist `$s)]];
    e};
  rt:@[rt;1;{[f;w] f each w}[f sc]];
  @[rt;2 3 inter til count rt;{[f;x] $[99h=type x; f each x; f x]}[f sc]]}
/ @ignore
/ A select tree over a foreign view, its string columns compared as text (.duckdb.i.either).
/ @ignore
.duckdb.i.ftext:{[v;rt] $[count v`text;.duckdb.i.either[v`text;rt];rt]}

/ @ignore
.duckdb.i.read1:{[url]
  r:.duckdb.exec[.duckdb.main[];"SELECT content FROM read_blob(",(.duckdb.i.file url),")"];
  if[0=count r; '`io]; if[1<count r; '`domain]; first r`content}
/ @ignore
.duckdb.i.staging:`$"_q_staging"
/ @ignore
.duckdb.i.write0:{[url;lines]
  if[not all 10h=type each lines; '`type];
  c:.duckdb.main[]; .duckdb.set[c;.duckdb.i.staging;([] line:lines)];
  .duckdb.exec[c;"COPY (SELECT line FROM ",(.duckdb.i.qname .duckdb.i.staging),") TO ",(.duckdb.i.file url)," (FORMAT CSV, HEADER false, QUOTE '', ESCAPE '')"];
  .duckdb.hdel[c;.duckdb.i.staging]; url}

/ Create or replace a DuckDB secret (the credentials httpfs uses for s3:// gcs:// hf://), or drop one.
/ opts ride verbatim as KEY value clauses of CREATE SECRET, so an unknown key is DuckDB's own refusal; the key
/ `persistent (boolean) picks the persistent form, which outlives the process in DuckDB's own store.
/ @param typ the secret type: `s3, `gcs, `r2, `huggingface ...
/ @param opts a symbol-keyed dict of KEY value clauses; () or (::) drops the secret instead
/ @return name
/ @eg .duckdb.secret[`bucket;`s3;`key_id`secret`region!("AKIA...";"...";"eu-west-1")]
/ @eg .duckdb.secret[`bucket;`s3;()]
.duckdb.secret:{[name;typ;opts]
  c:.duckdb.main[]; n:.duckdb.i.ident name;
  if[.duckdb.i.none opts;
    p:first exec persistent from .duckdb.secrets[] where name~\:n;
    .duckdb.exec[c;"DROP ",$[p;"PERSISTENT ";""],"SECRET ",n]; :name];
  if[not (99h=type opts)and 11h=type key opts; '`type];
  per:$[`persistent in key opts;opts`persistent;0b]; opts:(enlist `persistent) _ opts;
  cl:(enlist "TYPE ",.duckdb.i.ident typ),.duckdb.i.opts[{[k;v] .duckdb.i.ident[k]," ",.duckdb.i.lit v};opts];
  .duckdb.exec[c;"CREATE OR REPLACE ",$[per;"PERSISTENT ";""],"SECRET ",n," (",(", " sv cl),")"]; name}
/ The secrets defined, without their values.
/ @col scope the URL prefixes the secret covers
/ @eg .duckdb.secrets[]
.duckdb.secrets:{[] .duckdb.exec[.duckdb.main[];"SELECT name, type, provider, persistent, storage, scope FROM duckdb_secrets()"]}

/ Run any statement and answer DuckDB's own rendering as text, bypassing the type map: rows as DuckDB prints them,
/ or the error message as data when the statement fails.  A diagnostic, not a query door.
/ @return string
/ @eg .duckdb.unsafeExecText[.duckdb.main[];"SELECT [1,2,3] AS arr, {'a': 1} AS st"]
.duckdb.unsafeExecText:{[handle;sql] .duckdb.i.unsafeExecText[handle;sql]}

/ A table as DuckDB renders it, every column cast to VARCHAR (so nested, UUID, ENUM and BIT cells show), NULL as
/ the text NULL, rows in insertion order.  A text cell holding a newline or " | " misaligns its row.
/ @return a table of one string column per column of tableName
/ @eg
/ .duckdb.exec[.duckdb.main[];"CREATE OR REPLACE TABLE u AS SELECT [1,2] AS arr, 1 AS a"]
/ .duckdb.unsafeText[.duckdb.main[];`u]
.duckdb.unsafeText:{[handle;tableName]
  names:1_"\n" vs .duckdb.i.unsafeExecText[handle;"SELECT column_name FROM (DESCRIBE ",(.duckdb.i.qname tableName),")"];
  quoted:.duckdb.i.quote each names;
  r:"\n" vs .duckdb.i.unsafeExecText[handle;"SELECT ",(", " sv {x,"::VARCHAR AS ",x} each quoted)," FROM ",(.duckdb.i.qname tableName)," ORDER BY rowid"];
  flip (`$" | " vs r 0)!$[count 1_r; flip " | " vs/: 1_r; (count names)#enlist ()]}

/ The statement log: one row per statement the bridge issued, newest last.
/ @eg select time, dur, sql from .duckdb.sqllog
.duckdb.sqllog:([] time:0#0Np; dur:0#0Nn; ok:0#0b; conn:0#0Ni; rows:0#0N; ncols:0#0N; sql:(); err:())

/ How many rows .duckdb.sqllog keeps: 0 keeps nothing; at the cap the older half is dropped.  Set it with .duckdb.set_sqllogmax.
.duckdb.sqllogmax:4096
/ Set .duckdb.sqllogmax, the rows .duckdb.sqllog keeps.
.duckdb.set_sqllogmax:{[n] .duckdb.sqllogmax::n;}

/ Called once per statement with its log row as a dict.  Replace it with .duckdb.set_onsql to route the log elsewhere,
/ or with (::) for none.  Do not close the connection or issue transaction control from a handler: it runs mid-statement.
/ @eg .duckdb.set_onsql {[row] -1 row`sql;}
.duckdb.onsql:{[row]
  m:$[(type .duckdb.sqllogmax) in -7 -6h; .duckdb.sqllogmax; 0];
  if[0>=m; :.duckdb.sqllog::0#.duckdb.sqllog];
  insert[`.duckdb.sqllog; enlist row];
  if[m<count .duckdb.sqllog; .duckdb.sqllog::(neg m div 2)#.duckdb.sqllog]}
/ Set .duckdb.onsql, the function each statement's log row is handed to, or (::) for none.
.duckdb.set_onsql:{[f] .duckdb.onsql::f;}

/ The row count of a table of the handle's catalog, counted by DuckDB.
.duckdb.count:{[c;t] first .duckdb.i.exec[c; "SELECT COUNT(*) AS n FROM ",.duckdb.i.qname t]`n}

/ The row count above which a qSQL select on a q-origin table of a handle is 'limit before any query runs, the table,
/ its count and this threshold in .duckdb.err[]. The count is the one taken when the
/ table was bound, read afresh after the bridge's own writes; get stays unlimited. Set it with .duckdb.set_maxrows.
.duckdb.maxrows:0W
/ Set .duckdb.maxrows, the row count above which a select is refused; 0W for no limit.
.duckdb.set_maxrows:{[n] .duckdb.maxrows::n;}

/ @ignore
.duckdb.i.big:{[c;t] .duckdb.maxrows<.[.duckdb.i.rows;(c;t);0N]}
/ A select over a foreign view v that is no bound table (a parquet file), run by q on the rows pulled.
/ @ignore
.duckdb.i.fselect:{[c;v;rt] .duckdb.i.fresidue[c;v;rt]}
/ @ignore
.duckdb.i.push:{[c;rt]
  if[.duckdb.i.big[c;rt 0]; .duckdb.i.maxrows[c;rt 0;"j"$.duckdb.maxrows]];
  if[not .duckdb.i.foreign[c;rt 0]; :(?) . (enlist .duckdb.i.rebound[c;rt 0;.duckdb.i.qread[;;rt]]),1_rt];
  .duckdb.i.fread[c;rt 0;.duckdb.i.fresidue[;;rt]]}
/ The rows of a table q wrote that the select rt reads, of the columns it names (.pq.i.reads) in q's row order, as
/ .duckdb.i.fresidue pulls a foreign view's.  A keyed table is read whole: a select keeps its key.
/ @ignore
.duckdb.i.qread:{[c;t;rt]
  sc:.duckdb.i.schema[c;t]; k:select from sc where col<>`i;
  if[any k`iskey; :.duckdb.get[c;t]];
  k:select from k where col in .pq.i.reads[k`col;rt];
  sel:$[count k;.duckdb.i.quote each string k`col;enlist "1 AS \"_q_1\""];
  .duckdb.exec[c;("SELECT ",(", " sv sel)," FROM ",(.duckdb.i.qname t),$[`i in sc`col;" ORDER BY i";""];();k)]}
/ @ignore
.duckdb.qsql:{[c;cl;tree] rt:@[.pq.i.resolveSeam[cl];tree;{[e] ::}]; $[rt~(::);::;.duckdb.i.push[c;rt]]}

/ The handle of the process's own DuckDB database, `:pq:duckdb:main: in-memory, or the file q -duckdb path names.
/ Opened on the first call; every alias is a catalog attached to it, and .parquet and s) run here.
/ @return `:pq:duckdb:main
/ @eg .duckdb.exec[.duckdb.main[];"SELECT current_database()"]
.duckdb.main:{[] .duckdb.i.main[]}

/ Bind the handle's tables and views as q globals at the root, later-wins.  \l `:pq:duckdb:alias is the same door.
/ @param names () for none, :: for all, else the table names
/ @return the names bound
/ @eg .duckdb.load[.duckdb.main[];::]
.duckdb.load:{[handle;names] .pq.i.load[handle;names]}
/ @ignore
.duckdb.i.load:{[c;tbls] $[tbls~(::); .duckdb.i.tables c; (),tbls]}

/ Drop a table (or view) of the handle's catalog.  hdel `:pq:duckdb:alias:table/ is the same verb.
/ A q global bound to it stays bound and errors on use.
/ @return table
/ @eg
/ .duckdb.exec[.duckdb.main[];"CREATE OR REPLACE TABLE u AS SELECT 1 AS a"]
/ .duckdb.hdel[.duckdb.main[];`u]
.duckdb.hdel:{[handle;table] .duckdb.i.hdel[handle;table]}
