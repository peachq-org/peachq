/ Query and write DuckDB databases from q: a database is a handle, its tables are q tables.
/ .pq.hopen_duckdb[`alias;"/path/db"] opens or creates a database and answers the alias symbol `:pq:duckdb:alias, the
/ handle every .duckdb verb takes.  h "SELECT ..." runs SQL and answers a table; hclose h closes it.
/ `:pq:duckdb:alias:table/ names a table for get, set, upsert and qsql, with select/where/by pushed down to DuckDB.
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
/ a NULL list empty; VARCHAR is a string, DECIMAL a float, TIME a timespan, TIMESTAMPTZ its UTC instant; a date or timestamp
/ outside q's range is 'duckdb; a column whose type q does not hold is left out (.duckdb.hidden names them).  Rows come in
/ rowid order where the table has one.  .duckdb.getx and SQL read every column exactly.
.duckdb.get:{[c;t]
  if[not .duckdb.i.foreign[c;t]; :.duckdb.i.get[c;t]];
  / the catalog no longer has what the bound entry lists (DDL from another connection): bind afresh and read once more
  .[.duckdb.i.fpull;(c;.duckdb.i.ftable[c;t]);{[c;t;e]
    if[not any .duckdb.err[c] like/: ("Binder Error*";"Catalog Error*"); 'e];
    .duckdb.i.unbind[c;t]; .duckdb.i.fpull[c;.duckdb.i.ftable[c;t]]}[c;t]]}
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

/ Run SQL on a handle and answer the result as a table; a statement without a result answers ::.  sql is a string, or the
/ pair (string;dict) binding each $name by name or (string;list) binding $1, $2 ... by position; values are atoms or strings
/ (a null is SQL NULL), DuckDB's refusal is 'duckdb with the reason in .duckdb.err[], anything else 'type.  The triple
/ (string;params;schema), params () for none and schema the table getx answers, reads the result as .duckdb.get reads a table.
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
/ the logicals whose stored column a pushed where term compares as it stands: DuckDB then answers as q does on the shown
/ value once the term has its null arms (.sql.i.compare).  Any other compares the converted column.
/ a VARCHAR compares, groups and sorts by its bytes, as q's strings do, whatever collation its column or the session declares
/ @ignore
.duckdb.i.FBYTES:" COLLATE \"binary\""
/ @ignore
.duckdb.i.FBARE:`int8`int16`int32`int64`uint16`uint32`float32`float64`utf8`date`timestamp`timestampsec`timestampms`timestampus
/ @ignore
.duckdb.i.quote:{[s] "\"",ssr[s;"\"";"\"\""],"\""}
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
    lg=`date;.duckdb.i.fguard[nm;x;"date_diff('day', DATE '2000-01-01', ",x,") >= -2147483646"];
    lg=`timestamptz;.z.s[nm;`timestampus;"TIMESTAMP";"timezone('UTC', ",x,")"];
    lg in key .duckdb.i.FSPAN;
      .duckdb.i.fguard[nm;x;x," BETWEEN ",(" AND " sv {[dt;b] "'",b,"'::",dt}[dt] each .duckdb.i.FSPAN lg)];
    lg in `int8`uint16`uint32`timens;x;
    ()]}
/ One column, or one level of a list, x the SQL naming it: (sql;meta char;dtype;logical), the last two the schema row the read
/ needs ("" for none); () when the type is hidden.  ty is .duckdb.types[]'s canon rows, dtype!(logical;ktype).  A column that
/ is no list adds what a push needs: the DuckDB type the SQL answers, the canon logical, and whether q compares a literal
/ with it as DuckDB does (a timespan is a BIGINT to q's parameters and a TIME to DuckDB, so no term or aggregate reads one).
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
/ The meta char of a column q wrote as logical lg over the DuckDB type dt, level by level through a list; " " when q writes
/ no such pair (tt is .duckdb.types[]), and the column then reads by its DuckDB type alone.
/ @ignore
.duckdb.i.fown:{[tt;dt;lg]
  if[null lg; :" "];
  if["]"=last dt; :$[(s:string lg) like "list(*)";upper .z.s[tt;(last where dt="[")#dt;`$5_-1_s];" "]];
  k:exec ktype from tt where dtype=`$first "(" vs dt, logical=lg; $[count k;first k;" "]}
/ THE foreign view of a source, for a pull and for a push: rel its FROM text, sch its columns ([] col; dtype) as DuckDB declares
/ them, order the expressions its rows sort by (() for none).  Answers sel, the SELECT list; cols and t, what q shows and its
/ meta chars; schema, the rows .duckdb.exec's triple needs beside the statement; hidden, the columns left out; text, the
/ string columns a literal compares with as text; push, the schema .sql.translate reads (empty when a shown name is one the
/ pushed statement uses itself) and src, the SELECT items of the relation that statement reads, by name: each shown column,
/ and each stored column a where term compares as it stands under its raw name.
/ sch may say what q wrote (a parquet file's q_schema) in one more column, logical: a column whose logical q writes over
/ its DuckDB type is q's own, read as stored with NULL its only null; any other column reads as foreign.
/ @ignore
.duckdb.i.fview:{[rel;sch;order]
  tt:.duckdb.types[]; ty:exec dtype!flip (logical;ktype) from tt where canon; n:count sch;
  qn:.duckdb.i.quote each string sch`col; lg:$[`logical in cols sch;sch`logical;n#`];
  kt:.duckdb.i.fown[tt]'[sch`dtype;lg]; own:kt<>" ";
  r:{[ty;nm;dt;x;k;l] $[k=" ";.duckdb.i.fcol[ty;nm;dt;x;0];(x;k;dt;l;dt;`;1b)]}[ty]'[string sch`col;sch`dtype;qn;kt;lg];
  j:where 0=count each r; i:where 0<count each r; s:i where 0<count each r[i;2];
  sel:{[e;q] e[0]," AS ",q}'[r i;qn i];
  (`rel`order`sel`cols`t`schema`hidden`text!(rel;order;", " sv sel;sch[`col] i;"c"$r[i;1];
    ([] col:sch[`col] s; dtype:r[s;2]; logical:r[s;3]);([] col:sch[`col] j; dtype:sch[`dtype] j);
    sch[`col] i where ("C"=r[i;1]) and not own i)),
    .duckdb.i.fpush[exec ktype!logical from tt where needs<>`dtype;sel;sch[`col] i;qn i;r i;order]}
/ The push half of a foreign view (.duckdb.i.fview): a column's logical is the one q writes for the type q shows (ql), so the
/ translator classes it as q's own column of that type; its sentinel is the stored value that reads as the q null.
/ @ignore
.duckdb.i.fpush:{[ql;sel;cl;qn;r;order]
  one:7=count each r; lg:{[x;y] $[y;x 5;`]}'[r;one];
  bare:lg in .duckdb.i.FBARE; al:`$"_q_raw_",/:string cl;
  p:([] col:cl; dtype:{[x;y] $[y;x 4;""]}'[r;one]; logical:{[ql;x;y] $[count x 2;x 3;not y;`;x 6;ql x 1;`]}[ql]'[r;one]; iskey:0b;
    raw:?[bare;al;cl]; sentinel:{[x] $[x in key .duckdb.i.FMIN;.duckdb.i.FMIN x;x in `float32`float64;"NaN";""]} each lg);
  if[count order; p,:([] col:1#`i; dtype:1#enlist "BIGINT"; logical:1#`int64; iskey:1#0b; raw:1#`; sentinel:1#enlist "")];
  if[any (lower `i,al where bare) in lower cl; :`push`src!(0#p;()!())];
  w:where bare;
  `push`src!(p;(cl,al w)!sel,{[q;l;a] q,$[l=`utf8;.duckdb.i.FBYTES;""]," AS ",.duckdb.i.quote string a}'[qn w;lg w;al w])}
/ @ignore
.duckdb.i.foreign:{[c;t] 0=count .duckdb.i.schema[c;t]}
/ @ignore
.duckdb.i.ftable:{[c;t]
  sch:select col, dtype from .duckdb.i.info[c;t]`schema;
  .duckdb.i.fview[.duckdb.i.qname t;sch;$[`rowid in sch`col;();enlist "rowid"]]}
/ A view has no rowid: DuckDB's refusal of the row number, on a read or a push, is answered by the same without it.
/ @ignore
.duckdb.i.norowid:{[c;v] ((enlist "rowid")~v`order) and .duckdb.err[c] like "Binder Error: Referenced column \"rowid\"*"}
/ The rows of a foreign view.
/ @ignore
.duckdb.i.fpull:{[c;v]
  if[0=count v`cols; '`type];
  q:{[v;o] s:"SELECT ",v[`sel]," FROM ",v[`rel],$[count o;" ORDER BY ",", " sv o;""]; $[count v`schema;(s;();v`schema);s]}[v];
  .[.duckdb.exec;(c;q v`order);{[c;v;q;e] if[.duckdb.i.norowid[c;v]; :.duckdb.exec[c;q ()]]; 'e}[c;v;q]]}

/ A select tree over a foreign view, its string columns compared as text (.sql.i.either).
/ @ignore
.duckdb.i.ftext:{[v;rt] $[count v`text;.sql.i.either[v`text;rt];rt]}

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

/ Whether a qSQL select on a table of the handle runs what .sql.translate can move (where terms, limit, order; the select
/ phrase and by whole when every item translates) inside DuckDB before q runs the rest, read on every select; off, every select pulls
/ the whole table.  A statement DuckDB refuses stays in .duckdb.sqllog and the select pulls instead.  Set it with .duckdb.set_pushdown.
.duckdb.pushdown:1b
/ Set .duckdb.pushdown: 1b moves what a select can into DuckDB, 0b pulls every select whole.
.duckdb.set_pushdown:{[on] .duckdb.pushdown::on;}

/ The row count above which a qSQL select on a q-origin table of a handle needs a where term .sql.translate moves into DuckDB: without
/ one it is 'limit before any query runs, the table, its count and this threshold in .duckdb.err[]. The count is the one taken when the
/ table was bound, read afresh after the bridge's own writes; get stays unlimited. Set it with .duckdb.set_maxrows.
.duckdb.maxrows:0W
/ Set .duckdb.maxrows, the row count above which a select needs a where term DuckDB runs; 0W for no limit.
.duckdb.set_maxrows:{[n] .duckdb.maxrows::n;}

/ The .sql.translate answer (sqlTree;params;schema;qTree) of the last linked select that ran a pushed statement, qTree ::
/ when DuckDB answered it whole; :: when the last select pushed nothing (pushdown off, nothing to move, or a refusal).
.duckdb.lastpush:(::)

/ @ignore
.duckdb.i.stale:{[c] any (first "\n" vs .duckdb.err c) like/: ("Binder Error: Referenced column*";"Catalog Error: Table with name*")}
/ @ignore
.duckdb.i.big:{[c;t] .duckdb.maxrows<.[.duckdb.i.rows;(c;t);0N]}
/ The part of the select rt that .sql.translate moves, run over the source rt 0 names with the schema sc, q running the
/ rest; :: when nothing moves.  with answers the text ahead of the statement that defines that source, given its SQL tree.
/ @ignore
.duckdb.i.run:{[c;rt;sc;with;big]
  r:.sql.translate[((?);rt 0;$[count rt 1;enlist rt 1;()]),2_rt;sc];
  if[(0=count r[0;2]) and big; :(::)];
  if[(not (::)~r 3) and all ((0=count r[0;2]);(::)~r[0;5];0=count r[0;6]); :(::)];
  t:.duckdb.exec[c;((with r 0),.sql.render r 0;r 1;select from r[2] where not null logical)]; .duckdb.lastpush::r;
  / a by with no sort order answers a key marked s#, as q's own select does; a key s# cannot verify stays unmarked
  if[(::)~r 3; :$[(99h=type rt 2) and (6>count rt) and 99h=type t; @[`s#;t;t]; t]];
  (?) . (t;$[count r[3;2]; r[3;2;0]; ()]),3_r 3}
/ A select over a foreign view v, pushed: its statement reads the view as one relation, defined ahead of it, in which a
/ where term finds each stored column, the select list each converted one, and i the row number the pull orders by.  The
/ relation lists only what the statement names (a symbol anywhere in its tree), so no other column's conversion is bound.
/ The tree is the one the pull runs (.duckdb.i.ftext).
/ @ignore
.duckdb.i.fpushed:{[c;v;rt;big]
  if[0=count v`push; :(::)];
  go:{[c;v;rt;big;o]
    src:v[`src],$[count o;(enlist `i)!enlist $[1=count o;first o;"ROW(",(", " sv o),")"]," AS i";()!()];
    w:{[v;src;tree]
      n:{$[99h=type x;raze .z.s each (key x;value x);0h=type x;raze .z.s each x;11h=abs type x;(),x;`$()]} tree;
      "WITH \"_q_src\" AS NOT MATERIALIZED (SELECT ",(", " sv $[count s:src (key src) inter n;s;enlist "1 AS \"_q_1\""])," FROM ",v[`rel],") "}[v;src];
    .duckdb.i.run[c;@[.duckdb.i.ftext[v;rt];0;:;`$"_q_src"];$[count o;v`push;select from v[`push] where col<>`i];w;big]};
  .[go;(c;v;rt;big;v`order);{[go;c;v;rt;big;e] if[not .duckdb.i.norowid[c;v]; 'e]; go[c;v;rt;big;()]}[go;c;v;rt;big]]}
/ A select over a foreign view v that is no bound table (a parquet file): pushed, or run by q on the rows pulled.
/ @ignore
.duckdb.i.fselect:{[c;v;rt]
  .duckdb.lastpush::(::);
  if[.duckdb.pushdown; r:.[.duckdb.i.fpushed;(c;v;rt;0b);{[e] ::}]; if[not (::)~r; :r]];
  (?) . (enlist .duckdb.i.fpull[c;v]),1_.duckdb.i.ftext[v;rt]}
/ @ignore
.duckdb.i.pushed:{[c;rt]
  big:.duckdb.i.big[c;rt 0];
  $[count sc:.duckdb.i.schema[c;rt 0];.duckdb.i.run[c;rt;sc;{[tree] ""};big];.duckdb.i.fpushed[c;.duckdb.i.ftable[c;rt 0];rt;big]]}
/ @ignore
.duckdb.i.push:{[c;rt]
  .duckdb.lastpush::(::);
  if[.duckdb.pushdown;
    r:@[.duckdb.i.pushed[c];rt;{[c;rt;e] if[not .duckdb.i.stale c; :(::)]; .duckdb.i.unbind[c;rt 0];
      @[.duckdb.i.pushed[c];rt;{[e] ::}]}[c;rt]];
    if[not (::)~r; :r]];
  if[.duckdb.i.big[c;rt 0]; .duckdb.i.maxrows[c;rt 0;"j"$.duckdb.maxrows]];
  t:.duckdb.get[c;rt 0];
  (?) . (enlist t),1_$[.duckdb.i.foreign[c;rt 0];.duckdb.i.ftext[.duckdb.i.ftable[c;rt 0];rt];rt]}
/ @ignore
.duckdb.qsql:{[c;cl;tree]
  node:{[cl;nd]
    if[99h=type nd; :key[nd]!.z.s[cl] value nd];
    if[0h=.Q.c.lty nd; i:where 101h<>type each nd; :@[nd;i;:;.z.s[cl] each nd i]];
    $[-11h<>type nd; nd; (`$first "." vs string nd) in cl; nd; 100h<=type v:@[value;nd;{[e] 'unpush}]; nd; (11h=abs type v) or (type v) in 0 98h; enlist v; v]};
  rt:.[{[node;cl;t] (enlist t 0),node[cl,`i] each 1_t}[node];(cl;tree);{[e] ::}];
  $[rt~(::);::;.duckdb.i.push[c;rt]]}

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
