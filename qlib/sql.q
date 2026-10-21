/ A SQL SELECT as a q tree, slot for slot the shape q's own select (?) takes: (`sql;source;where;by;columns;limit;order).
/ .sql.render writes a tree as SQL text; .sql.parse reads SQL text into a tree.
/ .
/ source   the table: `t, a dotted `db.s.t, `t`u for the table t under the alias u, or a (`sql;...) tree for a
/          derived table (SELECT ... FROM (...); .sql.render only, .sql.parse reads none)
/ where    a list of the terms SQL ANDs together, () for none: one term is still a one-item list
/ by       0b, 1b for SELECT DISTINCT, or a dict of the GROUP BY expressions
/ columns  a dict of name to expression, ()!() for SELECT *.  An unaliased column's name is its expression's SQL
/          text, so a name that differs from that text is the column's AS (and SELECT a AS a cannot be carried)
/ limit    :: for none, n for LIMIT n, m n for LIMIT n OFFSET m (n 0W when there is no LIMIT); SQL applies it after
/          grouping, wherever q would
/ order    a list of (direction;expression) or (direction;expression;nulls).  direction is `$"<:" for ASC, `$">:" for
/          DESC, or ` when the query names neither; nulls is `$"nulls first" or `$"nulls last"
/ .
/ An expression is a column `price (`t.price qualified), a parameter `$"$name" or `$"$1", a literal (a long, a float,
/ a boolean, :: for NULL, an enlisted symbol for a string: 'x' is ,`x) or an application (head;arg;...).  The heads:
/ `= `<> `< `> `<= `>= `+ `- `* `/ `% `// `|| `like `not like `ilike `not ilike `glob `is distinct from and
/ `is not distinct from take two arguments, `- also one; `and `or take any number, `not `is null `is not null one;
/ (`between;x;lo;hi), (`in;x;item;...), (`cast;`TYPE;x) and `count`* for count(*).  Any other head is a call of the
/ function it names.  Spell a head that is not a q name with `$: `$"<=", `$"is null".  Two forms .sql.render alone
/ writes: (`in;x;tree) for x IN (SELECT ...) and a last argument (`$"order by";e;...) for f(x ORDER BY e, ...).
/ @eg .sql.render (`sql;`t;enlist (`$">";`price;100);0b;`sym`p!(`sym;`max`price);5;enlist (`$">:";`p))
/ @eg .sql.parse "SELECT sym, max(price) AS p FROM t WHERE price > 100 GROUP BY sym ORDER BY p DESC LIMIT 5"

/ The SQL text of a tree, written by q alone.  A tree the grammar above cannot read signals 'type, a head with the
/ wrong number of arguments 'length.
/ @return string
.sql.render:{[tree]
  if[not (7=count tree) and `sql~first tree; '`type];
  src:tree 1; wh:tree 2; by:tree 3; cl:tree 4; lim:tree 5; ord:tree 6;
  if[not (99h=type cl) and (-11h=type src) or ((11h=type src) and 2=count src) or .sql.i.derived src; '`type];
  column:{[k;e] r:.sql.i.expr e; $[k~`$r; r; r," AS ",.sql.i.quote string k]};
  order:{[o]
    if[not (count o) in 2 3; '`length];
    d:(`;`$"<:";`$">:")?o 0; n:`$("nulls first";"nulls last");
    if[(d>2) or (3=count o) and not o[2] in n; '`type];
    .sql.i.expr[o 1],(("";" ASC";" DESC") d),$[3=count o;" ",upper string o 2;""]};
  s:"SELECT ",$[1b~by;"DISTINCT ";""],$[count cl; ", " sv column'[key cl;value cl]; "*"];
  s,:" FROM ",$[-11h=type src; .sql.i.name src; .sql.i.derived src; "(",.sql.render[src],")";
    (.sql.i.name src 0)," AS ",.sql.i.quote string src 1];
  if[count wh; s,:" WHERE "," AND " sv .sql.i.sub[2] each wh];
  if[99h=type by; s,:" GROUP BY ",", " sv .sql.i.expr each value by];
  if[count ord; s,:" ORDER BY ",", " sv order each ord];
  if[lim~(::); :s];
  if[not (type[lim] in -5 -6 -7h) or (type[lim] in 5 6 7h) and 2=count lim; '`type];
  s,$[0>type lim; " LIMIT ",string lim; ($[0W=lim 1; ""; " LIMIT ",string lim 1])," OFFSET ",string lim 0]};

/ The tree of one SELECT, read by DuckDB's own parser, so it needs DuckDB: a tool for building and checking trees.
/ What the tree cannot carry signals 'sql: unsupported, naming it (a join, a subquery, HAVING, a window, UNION ...);
/ text DuckDB cannot parse signals 'sql: with DuckDB's reason.
/ @param sql (string)
/ @eg .sql.parse "SELECT a FROM t WHERE a > 1 AND b IS NULL"
.sql.parse:{[sql]
  j:.duckdb.exec[.duckdb.main[];
    ("SELECT json_serialize_sql($1, skip_default := true, skip_empty := true, skip_null := true) AS j";enlist sql)];
  d:.j.k first j`j;
  if[d`error; '`$"sql: ",d`error_message];
  s:d`statements;
  if[1<count s; .sql.i.no "multiple statements"];
  .sql.i.only[s 0;`node`named_param_map];
  .sql.i.select s[0;`node]};

.sql.i.INFIX:(`$("=";"<>";"<";">";"<=";">=";"+";"-";"*";"/";"%";"//";"||";"like";"not like";"ilike";"not ilike";"glob";
  "is distinct from";"is not distinct from"))!5 5 5 5 5 5 8 8 9 9 9 0N 0N 6 6 6 6 6 4 4;
.sql.i.LEFT:`$'"+-*/%";
/ duckdb_keywords(): the reserved and type_function words, which an unquoted column name cannot be.
.sql.i.RESERVED:`all`analyse`analyze`and`any`array`as`asc`asymmetric`both`case`cast`check`collate`column`constraint,
  `create`default`deferrable`desc`describe`distinct`do`else`end`except`false`fetch`for`foreign`from`group`having`in,
  `initially`intersect`into`lambda`lateral`leading`limit`not`null`offset`on`only`or`order`pivot`pivot_longer,
  `pivot_wider`placing`primary`qualify`references`returning`select`show`some`summarize`symmetric`table`then`to,
  `trailing`true`union`unique`unpivot`using`variadic`when`where`window`with`anti`asof`at`authorization`binary`by,
  `collation`columns`concurrently`cross`freeze`full`generated`glob`ilike`inner`is`isnull`join`left`like`map,
  `natural`notnull`outer`overlaps`positional`right`semi`similar`struct`tablesample`try_cast`tuple`unpack`verbose;
.sql.i.TYPES:(`COMPARE_EQUAL`COMPARE_NOTEQUAL`COMPARE_LESSTHAN`COMPARE_GREATERTHAN`COMPARE_LESSTHANOREQUALTO,
  `COMPARE_GREATERTHANOREQUALTO`COMPARE_DISTINCT_FROM`COMPARE_NOT_DISTINCT_FROM`CONJUNCTION_AND`CONJUNCTION_OR,
  `OPERATOR_NOT`OPERATOR_IS_NULL`OPERATOR_IS_NOT_NULL`COMPARE_IN`OPERATOR_COALESCE`COMPARE_BETWEEN)!
  `$("=";"<>";"<";">";"<=";">=";"is distinct from";"is not distinct from";"and";"or";"not";"is null";"is not null";"in";
  "coalesce";"between");
.sql.i.OPERATORS:(`$("+";"-";"*";"/";"%";"//";"||";"~~";"!~~";"~~*";"!~~*";"~~~"))!
  `$("+";"-";"*";"/";"%";"//";"||";"like";"not like";"ilike";"not ilike";"glob");
.sql.i.HEADS:(key .sql.i.INFIX),`and`or`not`in`between`cast,`$("is null";"is not null");
.sql.i.BASE:`class`type`query_location`query_location_length;

.sql.i.derived:{[src] (0h=type src) and (7=count src) and `sql~first src};
.sql.i.no:{[what] '`$"sql: unsupported ",what};
.sql.i.only:{[d;ks] if[count b:(key d) except ks; .sql.i.no string first b]};
.sql.i.sym:{[s] $[s~string v:`$s; v; .sql.i.no "blanks around ",s]};
.sql.i.app:{[e] $[0h=type e; 1b; 11h=type e; 1<count e; 0b]};
.sql.i.quote:{[s]
  $[(not (`$lower s) in .sql.i.RESERVED) and (all s in .Q.an) and (first s) in .Q.a,.Q.A,"_"; s; "\"",ssr[s;"\"";"\"\""],"\""]};
.sql.i.name:{[n] "." sv .sql.i.quote each "." vs string n};
/ The shortest text that reads back as the same float, with a point or an exponent so DuckDB keeps it a NUMERIC.
.sql.i.float:{[f]
  if[(null f) or f in -0w 0w; '`domain];
  c:{$["." in s:-27!(x;y); s; s,".0"]}[;f] each til 18;
  if[count s:first c where f={"F"$x} each c; if[20>count s; :s]];
  e:floor 10 xlog abs f;
  c:({-27!(x;y)}[;f%10 xexp e] each til 18),\:"e",string e;
  $[count s:first c where f={"F"$x} each c; s; '`domain]};

/ How tightly an expression binds, as DuckDB's grammar ranks it; an operator of unsettled rank is null, so it and
/ every compound argument it takes are parenthesised.
.sql.i.prec:{[e]
  if[not .sql.i.app e; :$[type[e] in -5 -6 -7 -9h; 10+0<=e; 11]];
  h:first e;
  $[h in `and`or; 1+h=`and; h=`not; 3; h in `$("is null";"is not null"); 4; h in `between`in; 6;
    (h in `$'"+-") and 2=count e; 10; h in key .sql.i.INFIX; .sql.i.INFIX h; 11]};
.sql.i.sub:{[p;e] s:.sql.i.expr e; $[p<.sql.i.prec e; s; "(",s,")"]};
.sql.i.expr:{[e]
  t:type e;
  if[e~(::); :"NULL"];
  if[-1h=t; :$[e;"true";"false"]];
  if[t in -5 -6 -7h; :$[null e; '`domain; string e]];
  if[-9h=t; :.sql.i.float e];
  if[-11h=t; :$["$"=first s:string e; s; .sql.i.name e]];
  if[11h=t; if[1=count e; :"'",ssr[string first e;"'";"''"],"'"]];
  if[not .sql.i.app[e] and -11h=type first e; '`type];
  .sql.i.apply e};
.sql.i.apply:{[e]
  h:first e; a:1_e; n:count a; p:.sql.i.prec e;
  if[$[h in `and`or; n<2; h in `not,`$("is null";"is not null"); n<>1; h=`between; n<>3; h=`cast; n<>2; h=`in; n<2;
      h in `$'"+-"; not n in 1 2; h in key .sql.i.INFIX; n<>2; 0b]; '`length];
  if[h in `and`or; :(" ",upper[string h]," ") sv .sql.i.sub[p] each a];
  if[h=`not; :"NOT ",.sql.i.sub[p;a 0]];
  if[h in `$("is null";"is not null"); :.sql.i.sub[p;a 0]," ",upper string h];
  if[h=`between; :.sql.i.sub[p;a 0]," BETWEEN ",.sql.i.sub[p;a 1]," AND ",.sql.i.sub[p;a 2]];
  if[h=`in; :.sql.i.sub[p;a 0]," IN (",$[(2=n) and .sql.i.derived a 1; .sql.render a 1; ", " sv .sql.i.expr each 1_a],")"];
  if[h=`cast; :"CAST(",.sql.i.expr[a 1]," AS ",string[a 0],")"];
  if[10=p; :(string h),.sql.i.sub[p;a 0]];
  if[h in key .sql.i.INFIX; :.sql.i.sub[$[null p;10;h in .sql.i.LEFT;p-1;p];a 0]," ",upper[string h]," ",.sql.i.sub[$[null p;10;p];a 1]];
  if[(h=`count) and a~enlist `$"*"; :"count(*)"];
  o:$[(1<n) and .sql.i.app[last a] and (`$"order by")~first last a; [a:-1_a; " ORDER BY ",", " sv .sql.i.expr each 1_last e]; ""];
  (.sql.i.quote string h),"(",(", " sv .sql.i.expr each a),o,")"};

.sql.i.select:{[n]
  .sql.i.only[n;`type`modifiers`cte_map`select_list`from_table`where_clause`group_expressions`group_sets`aggregate_handling];
  if[not "SELECT_NODE"~n`type; .sql.i.no n`type];
  if[count n`cte_map; .sql.i.no "WITH"];
  if[not "STANDARD_HANDLING"~n`aggregate_handling; .sql.i.no n`aggregate_handling];
  source:{[f]
    if[not "BASE_TABLE"~f`type; .sql.i.no f`type];
    .sql.i.only[f;`type`query_location`query_location_length`table_name`schema_name`catalog_name`alias`qualified_name];
    p:f `catalog_name`schema_name`table_name inter key f;
    if[(not p~f[`qualified_name]`path) or any "." in raze p; .sql.i.no "table name ","." sv p];
    $[`alias in key f; .sql.i.sym each ("." sv p;f`alias); .sql.i.sym "." sv p]};
  by:{[n]
    g:.sql.i.node each n`group_expressions;
    if[not (enlist til count g)~"j"$n`group_sets; .sql.i.no "GROUPING SETS"];
    (`$.sql.i.expr each g)!g};
  m:.sql.i.modifiers $[`modifiers in key n; n`modifiers; ()];
  w:$[`where_clause in key n; [w:.sql.i.node n`where_clause; $[.sql.i.app[w] and `and~first w; 1_w; enlist w]]; ()];
  b:$[`group_expressions in key n; by n; m 0];
  if[m[0] and 99h=type b; .sql.i.no "DISTINCT with GROUP BY"];
  (`sql;source n`from_table;w;b;.sql.i.columns n`select_list;m 1;m 2)};
.sql.i.columns:{[sl]
  if[any "STAR"~/:{x`class} each sl; if[1<count sl; .sql.i.no "* beside other columns"]; .sql.i.only[first sl;.sql.i.BASE]; :()!()];
  e:{.sql.i.node (enlist `alias) _ x} each sl;
  a:{$[`alias in key x; x`alias; ::]} each sl;
  r:.sql.i.expr each e;
  if[any b:a~'r; .sql.i.no "alias equal to its expression: ",first r where b];
  (.sql.i.sym each {$[x~(::); y; x]}'[a;r])!e};
.sql.i.modifiers:{[ms]
  if[0=count ms; :(0b;::;())];
  t:{x`type} each ms;
  if[count u:t except ("DISTINCT_MODIFIER";"ORDER_MODIFIER";"LIMIT_MODIFIER"); .sql.i.no first u];
  .sql.i.only[;`type] each ms where t~\:"DISTINCT_MODIFIER";
  order:{[o]
    .sql.i.only[o;`type`null_order`expression];
    if[3=i:(("ORDER_DEFAULT";"ASCENDING";"DESCENDING")~\:o`type)?1b; .sql.i.no o`type];
    d:(`;`$"<:";`$">:") i;
    $["ORDER_DEFAULT"~o`null_order; (d;.sql.i.node o`expression); (d;.sql.i.node o`expression;`$lower o`null_order)]};
  num:{[e] if[not -7h=type v:.sql.i.node e; .sql.i.no "LIMIT expression"]; v};
  limit:{[num;m] .sql.i.only[m;`type`limit`offset]; n:$[`limit in key m; num m`limit; 0W]; $[`offset in key m; (num m`offset;n); n]};
  o:raze {[order;m] .sql.i.only[m;`type`orders]; order each m`orders}[order] each ms where t~\:"ORDER_MODIFIER";
  l:limit[num] each ms where t~\:"LIMIT_MODIFIER";
  (any t~\:"DISTINCT_MODIFIER";$[count l; first l; ::];o)};

.sql.i.node:{[e]
  c:e`class;
  if[c~"COLUMN_REF"; .sql.i.only[e;.sql.i.BASE,`column_names]; n:e`column_names;
    if[any ("."in/:n),(enlist "*")~/:n; .sql.i.no "column name ","." sv n];
    if["$"=first first n; .sql.i.no "column name ",first n];
    :.sql.i.sym "." sv n];
  if[c~"CONSTANT"; .sql.i.only[e;.sql.i.BASE,`literal]; :.sql.i.literal e`literal];
  if[c~"PARAMETER"; .sql.i.only[e;.sql.i.BASE,`identifier]; :.sql.i.sym "$",e`identifier];
  if[c~"FUNCTION"; :.sql.i.function e];
  if[c~"CAST"; .sql.i.only[e;.sql.i.BASE,`child`type_expr]; y:e`type_expr; .sql.i.only[y;.sql.i.BASE,`type_name`qualified_name];
    if[not (enlist y`type_name)~y[`qualified_name]`path; .sql.i.no "type ",y`type_name];
    :(`cast;.sql.i.sym y`type_name;.sql.i.node e`child)];
  if[not c in ("COMPARISON";"CONJUNCTION";"OPERATOR";"BETWEEN"); .sql.i.no $[10h=type c; c; "node"]];
  if[null h:.sql.i.TYPES `$e`type; .sql.i.no e`type];
  ks:$[c~"COMPARISON"; `left`right; c~"BETWEEN"; `input`lower`upper; `children];
  .sql.i.only[e;.sql.i.BASE,ks];
  (enlist h),.sql.i.node each $[`children~ks; e`children; e ks]};
.sql.i.literal:{[l]
  k:l`kind; t:l`text;
  if[k~"NULL_LITERAL"; :(::)];
  if[(k~"BOOLEAN") and any t~/:("true";"false"); :t~"true"];
  if[k~"STRING"; :enlist .sql.i.sym t];
  if[k~"INTEGER"; :$[t~string v:"J"$t; v; .sql.i.no "integer ",t]];
  if[k~"NUMERIC"; :$[t~@[.sql.i.float;v:"F"$t;""]; v; .sql.i.no "numeric ",t]];
  .sql.i.no "literal ",k};
.sql.i.function:{[e]
  .sql.i.only[e;.sql.i.BASE,`function_name`order_bys`arguments`qualified_name`is_operator];
  f:e`function_name;
  if[not (enlist f)~e[`qualified_name]`path; .sql.i.no "function ","." sv e[`qualified_name]`path];
  if[not ((enlist `type)!enlist "ORDER_MODIFIER")~e`order_bys; .sql.i.no "ORDER BY in ",f];
  if[not `arguments in key e; :$[f~"count_star"; `count,`$"*"; .sql.i.no "function without arguments ",f]];
  h:$[`is_operator in key e; .sql.i.OPERATORS `$f; .sql.i.sym f];
  if[null[h] or (not `is_operator in key e) and h in .sql.i.HEADS; .sql.i.no "operator ",f];
  .sql.i.only[;enlist `expression] each e`arguments;
  (enlist h),.sql.i.node each {x`expression} each e`arguments};

/ The where terms, limit and order of a select that DuckDB can run as SQL, the rest left to q: (sqlTree;params;schema;
/ qTree) for a ? parse tree over a table whose schema is the envelope .duckdb.getx answers (col dtype logical iskey),
/ the row index i among its rows on a table that stores one.  sqlTree selects the columns qTree names (every column
/ when its select phrase is empty; i never, unless the phrase names it bare, then as i AS its q output name) under the
/ moved terms, params binds their literals as $pq1 $pq2 ... in the order the SQL text reads them, schema is the rows of
/ those columns, and qTree is the tree minus what moved with the table name still at slot 1, :: when nothing is left
/ for q.  A select phrase of aggregates, or a by, whose every item .sql.i.whole can spell moves whole with the entire
/ where: sqlTree groups, its schema rows carry q's result types with the by keys marked, and qTree is ::.
/ Five stages, each reading the tree and what the stages before it decided: the select phrase (.sql.i.phrase), the
/ where (.sql.i.where), the whole push of aggregates and by (.sql.i.whole), the limit and order (.sql.i.limit) and the
/ assembly of the answer (.sql.i.assemble), which numbers the literals each stage left as placeholders (.sql.i.number).
/ @eg .sql.translate[parse "select max price by sym from t where qty>100, f price";
/   ([] col:`sym`qty`price; dtype:("VARCHAR";"BIGINT";"DOUBLE"); logical:`symbol`int64`float64; iskey:000b)]
.sql.translate:{[tree;schema]
  p:.sql.i.phrase tree 4;
  w:.sql.i.where[tree;schema;p];
  g:.sql.i.whole[tree;schema;w];
  l:.sql.i.limit[tree;w;g];
  .sql.i.assemble[tree;schema;p;w;g;l]};

/ The select phrase stage, a dict: barrier (i sits inside an expression of the phrase; count i reads only the group's
/ size, so it is no barrier) and renames (the names the phrase gives a bare i: `x for exec i, as q names a bare one;
/ parse enlists that one column, eval bares it)
.sql.i.phrase:{[a]
  none:(0#`)!0#`;
  if[()~a; :`barrier`renames!(0b;none)];
  if[(11h=type a) and 1=count a; a:first a];
  if[-11h=type a; :`barrier`renames!(0b;$[a=`i; (enlist `x)!enlist `i; none])];
  if[99h<>type a; :`barrier`renames!(`i in .sql.i.names[0b] a;none)];
  b:`i~/:value a; v:(value a) where not b;
  `barrier`renames!(`i in raze .sql.i.names[0b] each v where not ((count);`i)~/:v; (key[a] where b)!(sum b)#`i)};
/ The where stage, a dict: cls (column to logical class), ix (the table stores i), stop (no term may move), terms (the
/ moved terms as SQL) and rest (the terms left for q, in order).  q runs term k+1 on the rows passing terms 1..k, so a
/ term that reads only its own row commutes with the others: every such term before the first that does not
/ (.sql.i.pure) moves when .sql.i.VERBS can spell it and stays for q, in order, when it cannot.  A term on i left for
/ q would read positions the pull no longer has, so it stops every term; on a table without i, i anywhere does (the
/ virtual index).  So do the phrase's barrier, a rename onto a column, an empty schema, and a table or column name
/ holding a dot (the SQL tree would read it as a path).
.sql.i.where:{[tree;schema;phrase]
  cl:exec col from schema; cls:cl!exec logical from schema; ix:`i in cl;
  wh:$[count tree 2; tree[2;0]; ()];
  stop:(phrase`barrier) or (0=count cl) or (any (key phrase`renames) in cl) or (any "." in/: string (tree 1),cl) or
    `i in raze .sql.i.names[0b] each $[ix; enlist tree 3; 2_tree];
  x:$[stop; (); .sql.i.term[cls;0b] each ((.sql.i.pure[0b] each wh)?0b)#wh];
  m:where not (::)~/:x; rest:wh (til count wh) except m;
  stop:stop or `i in raze .sql.i.names[0b] each rest;
  `cls`ix`stop`terms`rest!(cls;ix;stop;$[stop; (); x m];$[stop; wh; rest])};
/ The whole-push stage, a dict: ok (the select phrase and by move whole, so q is left nothing), byrow (the select by g
/ form: the last row of each group, so the SQL keeps every column that is not a key, as q does), by (the GROUP BY
/ expressions, 0b for none), cols (the SQL columns, keys first), rows (their schema rows: q's result type of each
/ aggregate, the keys marked) and order (the keys ascending, nulls first, as q sorts a by).  All or nothing: ok needs
/ the whole where moved, no limit or order, a phrase that is a dict of aggregates .sql.i.agg spells (or empty, over a
/ by, on a table with i) and by keys .sql.i.key spells, under distinct output names.  Anything else leaves the phrase
/ and by to q over the where-only push.
.sql.i.whole:{[tree;schema;w]
  off:`ok`byrow`by`cols`rows`order!(0b;0b;0b;()!();0#schema;());
  a:tree 4; by:tree 3; grouped:99h=type by; byrow:()~a;
  shape:$[99h=type a; grouped or 0b~by; byrow and grouped and w`ix];
  if[(5<count tree) or w[`stop] or (0<count w`rest) or not shape; :off];
  x:$[grouped; .sql.i.key[schema]'[key by;value by]; ()],$[byrow; (); .sql.i.agg[w`ix;schema] each value a];
  names:$[grouped; key by; `$()],$[byrow; `$(); key a]; nk:$[grouped; count by; 0];
  if[(any (::)~/:x) or count[names]<>count distinct names; :off];
  vals:$[byrow; (exec col from schema) except `i,names; `$()]; e:x[;0];
  rows:([] col:names; dtype:x[;1]; logical:x[;2]; iskey:(nk#1b),(count[names]-nk)#0b),
    update iskey:0b from select from schema where col in vals;
  `ok`byrow`by`cols`rows`order!(1b;byrow;$[grouped; names[til nk]!e til nk; 0b];(names!e),vals!vals;rows;
    {[e;lg] (`;e),$[.sql.i.class[lg]`nullable; enlist `$"nulls first"; ()]}'[e til nk;x[til nk;2]])};
/ the .sql.i.CLASS row of a logical name whose values q and DuckDB order alike, :: otherwise: an unknown class, or
/ char, whose null q orders below every byte
.sql.i.sortable:{[lg] $[(null k`class) or `char=(k:.sql.i.class lg)`class; (::); k]};
/ one by key named n as (expr;dtype;logical), or :: for one the SQL cannot group and sort as q does: a bare column of a
/ sortable class, or m xbar of an int column with m a positive int, as the long q answers, floored as q floors
/ (DuckDB's // truncates): c - (c % m + m) % m, m one parameter wherever the key is written
.sql.i.key:{[schema;n;e]
  xb:$[0h=type e; (3=count e) and (xbar)~e 0; 0b];
  c:$[xb; e 2; e]; if[not -11h=type c; :(::)]; if[(c=`i) or not c in schema`col; :(::)];
  row:schema (schema`col)?c; if[(::)~.sql.i.sortable row`logical; :(::)];
  if[not xb; :(c;row`dtype;row`logical)];
  m:e 1; if[not ((type m) in -5 -6 -7h) and (not null m) and (m>0) and (row`logical) in `int16`int32`int64; :(::)];
  p:.sql.i.lit[m],n;
  ((`cast;`BIGINT;(`$"-";c;(`$"%";(`$"+";(`$"%";c;p);p);p)));"BIGINT";`int64)};
/ The q aggregates the select phrase may push, one row each: sql, its SQL over the columns x and y (X and Y cast to
/ DOUBLE when real, as q converts both sides to floats; T the result's type; i the row index, needing a table with
/ it); arity; the column classes it is exact for, none meaning any column or i; result, a logical or the law naming
/ one: column (the first column's), int32up (short and boolean sum to int), wsum (a real where a real meets an int or
/ a real, as q types x*y, multiplied as stored); identity, the value a NULL answer becomes, coalesced inside any cast
/ (0N: none, DuckDB's NULL being q's null, but a symbol's NULL is the empty string); nulls, NaN where DuckDB computes
/ a float NaN (0/0, opposite infinities) that q calls null.  cov's averages each skip their own null rows, as q's do.
/ Left to q: med (q ranks a null lowest, which no SQL quantile does), sum of temporals, prd of ints (DuckDB's product
/ is a double), char and INTERVAL columns.
.sql.i.AGGS:([] verb:((count);(sum);(avg);(dev);(var);(sdev);(svar);(max);(min);(first);(last);(prd);(cor);(cov);(wsum);(wavg));
  sql:(`count,`$"*";(`cast;`T;`sum`x);`avg`x;`stddev_pop`x;`var_pop`x;`stddev_samp`x;`var_samp`x;`max`x;`min`x;
    (`first;`x;(`$"order by";`i));(`last;`x;(`$"order by";`i));(`cast;`T;`product`x);`corr`x`y;
    (`$"-";(`avg;(`$"*";`X;`Y));(`$"*";`avg`x;`avg`y));(`cast;`T;(`sum;(`$"*";`x;`y)));
    (`$"/";(`sum;(`$"*";`X;`Y));(`sum;(`$"*";`x;(`cast;`INTEGER;(`$"is not null";`y))))));
  arity:1 1 1 1 1 1 1 1 1 1 1 1 2 2 2 2;
  classes:(enlist `$()),(enlist `int`float`bool),(5#enlist `int`float),(4#enlist `int`float`symbol`temporal),
    (enlist enlist `float),4#enlist `int`float;
  result:`int64`int32up`float64`float64`float64`float64`float64`column`column`column`column`column`float64`float64`wsum`float64;
  identity:0N 0 0N 0N 0N 0N 0N -0W 0W 0N 0N 1 0N 0N 0 0N;
  nulls:`null`NaN`NaN`NaN`NaN`NaN`NaN`null`null`null`null`NaN`NaN`NaN`NaN`NaN);
/ the aggregate identity v (0, 1, -0W, 0W) in the type of the logical lg, as a parameter binds it: the null symbol is
/ the empty string, and a float type takes the float infinities (cast carries a long infinity to the ints and
/ temporals only); the type map is .duckdb.types[], the one owner of logical names
.sql.i.identity:{[lg;v]
  $[`symbol=lg; ""; (exec logical!ktype from .duckdb.types[])[lg]$$[lg in `float32`float64; (0 1 -0W 0W!0 1 -0w 0w) v; v]]};
/ the DuckDB type a logical name stores as, as the schema spells it
.sql.i.dtype:{[lg] string (exec logical!dtype from .duckdb.types[] where canon) lg};
/ one aggregate of the phrase as (expr;dtype;logical) under its .sql.i.AGGS row, or :: for none
.sql.i.agg:{[ix;schema;e]
  if[0h<>type e; :(::)]; if[null i:first where .sql.i.AGGS[`verb]~\:e 0; :(::)]; r:.sql.i.AGGS i; a:1_e; f:r`result;
  if[((count a)<>r`arity) or not all -11h=type each a; :(::)]; s:schema (schema`col)?a; L:s`logical;
  if[$[count r`classes; not all (.sql.i.class each L)[`class] in r`classes; not all a in `i,schema`col] or
    (`i in raze/[r`sql]) and not ix; :(::)];
  t:$[f=`column; L 0; f=`int32up; $[(L 0) in `int16`bool; `int32; L 0]; f=`wsum; `float64`float32(`float32 in L)>`float64 in L; f];
  dt:$[f=`column; first s`dtype; .sql.i.dtype t]; d:{[c;lg] $[`float32=lg; (`cast;`DOUBLE;c); c]}'[a;L];
  x:{[m;e] $[-11h=type e; $[e in key m; m e; e]; (type e) in 0 11h; .z.s[m] each e; e]}[(`T,raze (count a)#/:(`x`y;`X`Y))!(`$dt),a,d;r`sql];
  if[(not null r`identity) or `symbol=t; c:{(`coalesce;x;y)}[;.sql.i.lit .sql.i.identity[t;r`identity]]; x:$[`cast~first x; @[x;2;c]; c x]];
  ($[(`NaN=r`nulls) and t in `float32`float64; (`nullif;x;(`cast;`$dt;enlist `NaN)); x];dt;t)};
/ The limit and order stage, a dict: limit and order (SQL's; :: and at most the i order where they stay with q), inner,
/ moved (anything moved) and clear (the limit and order moved).  select[n] / [m n] / [-n] / [order] move only over a
/ table with i, the whole where moved and no by (the limit counts the rows the where kept, before the select phrase,
/ select.md); the order needs a column of a class q and DuckDB sort alike (never char, whose null q orders below every
/ byte), nulls first ascending and last descending as q sorts them, and i after it since q's sort is stable; a limit q
/ refuses (a null, a negative count in a pair) stays with q, which raises.  On a table with i, a statement that moves
/ anything orders its rows by i, except a whole push, which has no limit and orders by its keys.  select[-n] is the
/ LAST n rows: inner is the reversed order an inner query cuts them by, () for any other limit.
.sql.i.limit:{[tree;w;g]
  if[g`ok; :`limit`order`inner`moved`clear!(::;g`order;();1b;0b)];
  pick:{[cls;tree]
    none:(::;()); n:tree 5; o:$[6<count tree; tree 6; ()];
    if[(0h=type o) and (1=count o) and 0h=type first o; o:first o];
    if[count o; if[(2<>count o) or -11h<>type c:o 1; :none]; d:((<:);(>:))?o 0;
      if[(d=2) or (::)~k:.sql.i.sortable cls c; :none];
      o:enlist ((`$("<:";">:") d);c),$[k`nullable; enlist `$("nulls first";"nulls last") d; ()]];
    if[(type n) in -5 -6 -7h; :$[null n; none; n=0W; $[count o; (::;o); none]; (n;o)]];
    if[((type n) in 5 6 7h) and 2=count n; if[(not any null n) and (n[0]>=0) and n[1]>0; :(n;o)]];
    none};
  ok:w[`ix] and (not w`stop) and (0=count w`rest) and (0b~tree 3) and 5<count tree;
  lim:$[ok; pick[w`cls;tree]; (::;())];
  clear:not lim~(::;()); moved:clear or 0<count w`terms;
  o:lim[1],$[moved and w`ix; enlist (`;`i); ()];
  rev:(`;`$"<:";`$">:";`$"nulls first";`$"nulls last")!(`$">:";`$">:";`$"<:";`$"nulls last";`$"nulls first");
  inner:$[$[(type lim 0) in -5 -6 -7h; (lim 0)<0; 0b]; {[f;o] @[o;$[3=count o; 0 2; 0];f]}[rev] each o; ()];
  `limit`order`inner`moved`clear!(lim 0;o;inner;moved;clear)};
/ The assembly.  A whole push groups under the where (the select by g form keeps the rows holding each group's last i,
/ WHERE i IN a grouped subquery) and leaves q nothing.  Otherwise q's tree keeps the terms left, a bare i under the
/ name the phrase gives it, and the limit and order where they stay (:: when nothing is left); the SQL selects the
/ columns that tree reads, the key columns with them.
.sql.i.assemble:{[tree;schema;phrase;w;g;l]
  if[g`ok; last_:(`sql;tree 1;w`terms;g`by;(enlist `i)!enlist (`max;`i);::;());
    :.sql.i.number[$[g`byrow; (`sql;tree 1;enlist (`in;`i;last_);0b;g`cols;::;l`order); (`sql;tree 1;w`terms;g`by;g`cols;::;l`order)]],
      (g`rows;::)];
  rn:phrase`renames; rest:w`rest; q:tree;
  if[l`moved; q:@[tree;2;:;$[count rest; enlist rest; ()]];
    if[count rn; q[4]:$[-11h=type q 4; first key rn; 11h=type q 4; key rn; @[q 4;key rn;:;key rn]]];
    if[l`clear; q:5#q];
    if[(5>=count q) and (0=count rest) and (0b~q 3) and ()~q 4; q:(::)]];
  cl:(exec col from schema) except `i;
  base:{[n] `$first "." vs string n} each raze .sql.i.names[0b] each 2_$[q~(::); tree; q];
  need:$[()~tree 4; cl; cl where cl in base,exec col from schema where iskey];
  c:$[count c:(need!need),$[l`moved; rn; ()!()]; c; ()!()];
  s:(`sql;tree 1;w`terms;0b;c;l`limit;l`order);
  if[count l`inner; s:(`sql;(`sql;tree 1;w`terms;0b;u!u:distinct need,l[`order][;1];neg l`limit;l`inner);();0b;c;::;l`order)];
  (.sql.i.number s),(select from schema where col in need;q)};
/ the sql tree with each placeholder (::;v) numbered $pq1 $pq2 ... in the order .sql.render writes it, and the params they
/ bind; a placeholder with a third item is one parameter wherever it is written
.sql.i.number:{[tree]
  place:{[acc;e]
    if[99h=type e; r:.z.s[acc;value e]; :(r 0;key[e]!r 1)];
    if[0h<>type e; :(acc;e)];
    if[(1<count e) and (::)~first e; k:$[3=count e; (acc~\:e)?1b; count acc]; :($[k=count acc; acc,enlist e; acc];`$"$pq",string k+1)];
    {[f;x;i] y:f[x 0;x[1] i]; (y 0;@[x 1;i;:;y 1])}[.z.s]/[(acc;e);o where (type each e o:$[.sql.i.derived e; 4 1 2 3 6; til count e]) in 0 99h]};
  r:place[();tree]; (r 1;(`$"pq",/:string 1+til count r 0)!$[count r 0; r[0][;1]; ()])};
.sql.i.lit:{[v] (::;v)};

/ The q verbs a where term may push: the head .sql.render spells it with (in is list_contains over ONE list
/ parameter, which is what DuckDB rewrites IN over a list to), the head the term takes with its literal on the left,
/ whether q keeps a null row under it (q nulls sort lowest, so the SQL adds OR col IS NULL; in keeps the null row
/ exactly when its literal has a null member), and the column classes it is exact for: q compares a float with
/ tolerance (basics/precision.md) and DuckDB does not, so only in and null push a float column.
.sql.i.VERBS:([] verb:((=);(<>);(<);(>);(<=);(>=);(within);(in);(null);(not);(and);(&);(or);(|));
  sql:`$("=";"<>";"<";">";"<=";">=";"between";"list_contains";"is null";"not";"and";"and";"or";"or");
  shape:`scalar`scalar`scalar`scalar`scalar`scalar`range`list`isnull`not`logic`logic`logic`logic;
  mirror:`$("=";"<>";">";"<";">=";"<=";"";"";"";"";"";"";"";"");
  nulllit:`$("is null";"is not null";"";"";"";"";"";"";"";"";"";"";"";"");
  keepsnull:01101000000000b;
  classes:(2#enlist `int`bool`symbol`temporal`char),(5#enlist `int`bool`symbol`temporal),
    (enlist `int`float`bool`symbol`temporal`char),(enlist `int`float`bool`symbol`temporal),5#enlist `$());
/ The column classes a term or an aggregate may push: the .duckdb.types[] ktype letters of the logicals in each, the
/ q types of a literal vector that compares with it exactly (an atom is the negative; none is the column's own type;
/ an int class takes no float, which q compares with tolerance), and whether it holds a SQL NULL for a q null (a null
/ symbol is the empty string and a null char the space, both values; a boolean has none).  A char is a one-character
/ VARCHAR: equal exactly, but q orders the space below every char and DuckDB by byte, so only =, <> and in push it;
/ INTERVAL (minute, second, time) is lossy and has no class.
.sql.i.CLASS:([] class:`int`float`bool`symbol`temporal`char; ktype:("hij";"ef";"b";"s";"dmpn";"c");
  literal:(5 6 7h;5 6 7 8 9h;0#0h;0#0h;0#0h;0#0h); nullable:110010b);
/ the class row of each q logical, derived from .duckdb.types[] on the first lookup after a load
.sql.i.CLASSES:(::);
/ the row (logical class literal nullable) of a q logical in .sql.i.CLASS, a null row for any other
.sql.i.class:{[lg]
  if[(::)~.sql.i.CLASSES;
    t:select logical, ktype from .duckdb.types[] where needs<>`dtype, ktype in raze .sql.i.CLASS`ktype;
    c:.sql.i.CLASS {[k] first where k in/: .sql.i.CLASS`ktype} each t`ktype;
    .sql.i.CLASSES::([] logical:t`logical; class:c`class; literal:{[l;k] $[count l; l; enlist type k$()]}'[c`literal;t`ktype];
      nullable:c`nullable)];
  .sql.i.CLASSES .sql.i.CLASSES[`logical]?lg};

/ the names a tree fragment refers to: a bare symbol, or a symbol vector outside an application (inside one it is
/ a literal, as parse enlists a symbol constant); a projection's elided argument (101h) is skipped, never applied
.sql.i.names:{[inapp;node]
  $[99h=type node; raze .z.s[0b] each value node; 0h=type node; raze .z.s[1b] each node where 101h<>type each node;
    -11h=type node; enlist node; (11h=type node) and not inapp; node; `$()]};
/ the literal a term compares with, as q evaluates the node: a one-item list is its constant, a symbol rides
/ enlisted, a bare symbol is a name; list asks for a vector of the class, else an atom
.sql.i.operand:{[k;list;v]
  if[(0h=type v) and 1=count v; v:first v];
  if[-11h=type v; :(::)];
  if[list; :$[(type v) in k`literal; v; (::)]];
  if[(11h=type v) and 1=count v; v:first v];
  $[(neg type v) in k`literal; v; (::)]};
/ whether a where term reads only its own row: every head is a .sql.i.VERBS verb, int arithmetic, like, $ by a type
/ name or enlist of a literal list (any other head - an aggregate, fby, prev, a lambda - may read other rows), and no
/ vector but a constant (one item, or anywhere in the last argument of in, within or like, lit), since q pairs any
/ other vector with the rows by position; a name there reads a whole column.  $ casts row by row only by a type name:
/ ` and `symbol, or a char (tok), turn a char column into ONE value
.sql.i.pure:{[lit;e]
  t:type e; if[-11h=t; :not lit]; if[t<0; :1b]; if[t within 1 19h; :lit or 1=count e]; if[0h<>t; :0b];
  if[1=count e; :.z.s[lit] first e];
  if[not any (e 0)~/:.sql.i.VERBS[`verb],((+);(-);(*);(neg);(abs);(like);($);(enlist)); :0b];
  if[$[($)~e 0; not $[(11h=type e 1) and 1=count e 1; not (first e 1) in `,`symbol; 0b]; 0b]; :0b];
  a:1_e; all .z.s'[$[lit; (count a)#1b; (-1_(count a)#0b),any (e 0)~/:((in);(within);(like))]; a]};
.sql.i.bind:{[v] $[-11h=type v; $[null v; ""; v]; -10h=type v; enlist v; 10h=type v; enlist each v; v]};
/ the SQL that is true exactly where the column holds a q null; :: for a class without one
.sql.i.nullpred:{[k;c] $[k`nullable; (`$"is null";c); `symbol=k`logical; (`$"=";c;.sql.i.lit ""); (::)]};
/ e over a nullable column reads NULL on its null row: keeps adds the rows q keeps there, strict makes the expression
/ false there instead (under NOT, where NULL would not flip)
.sql.i.arm:{[k;strict;keeps;c;e]
  if[keeps; q:.sql.i.nullpred[k;c]; :$[q~(::); e; (`or;e;q)]];
  $[k[`nullable] and strict; (`and;e;(`$"is not null";c)); e]};

/ one where term as SQL, or :: for a term the map cannot push
.sql.i.term:{[cls;strict;e]
  if[0h<>type e; :(::)]; if[(any 101h=type each 1_e) or not (type e 0) within 100 112h; :(::)];
  if[null i:first where .sql.i.VERBS[`verb]~\:e 0; :(::)]; r:.sql.i.VERBS i; a:1_e;
  if[r[`shape]=`logic; if[2<>count a; :(::)]; l:.z.s[cls;strict] each a; :$[any (::)~/:l; (::); (r`sql),l]];
  if[r[`shape]=`not; :$[(::)~t:.z.s[cls;1b;a 0]; (::); (r`sql;t)]];
  .sql.i.compare[cls;strict;r;a]};
/ the compared side, a column or an .sql.i.arith expression, is c; an expression compares only by scalar verbs with a
/ non-null literal inside its type's range
.sql.i.compare:{[cls;strict;r;a]
  c:a 0; v:a 1; side:{[x] (-11h=type x) or (0h=type x) and any (first x)~/:.sql.i.ARITH};
  if[(not side c) and (side v) and not null r`mirror; c:v; v:a 0; r:.sql.i.VERBS first where .sql.i.VERBS[`sql]=r`mirror];
  x:$[-11h=type c; (c;.sql.i.class cls c); `scalar=r`shape; .sql.i.arith[cls;c]; (::)];
  if[x~(::); :(::)]; c:x 0; k:x 1; s:r`sql; h:r`shape;
  if[not k[`class] in r`classes; :(::)];
  if[h=`isnull; :.sql.i.nullpred[k;c]];
  w:.sql.i.operand[k;h in `range`list;v]; if[w~(::); :(::)];
  if[$[-11h=type c; 0b; (null w) or (abs w)>(`int16`int32`int64!(0Wh;0Wi;0W)) k`logical]; :(::)];
  if[h=`range; if[(2<>count w) or k[`nullable] and any null w; :(::)];
    :.sql.i.arm[k;strict;0b;c;(s;c),.sql.i.lit each .sql.i.bind each w]];
  if[h=`list; :.sql.i.arm[k;strict;any null w;c;(s;.sql.i.lit .sql.i.bind w;c)]];
  if[k[`nullable] and null w; :$[null r`nulllit; (::); (r`nulllit;c)]];
  .sql.i.arm[k;strict;k[`nullable] and r`keepsnull;c;(s;c;.sql.i.lit .sql.i.bind w)]};
/ the q heads of int arithmetic, spelt by .sql.i.arith as + - * and unary -
.sql.i.ARITH:((+);(-);(*);(neg));
/ an int expression as (sql;class row): ONE +, - or * over int columns and non-null int literals, or neg of one,
/ naming a column; :: otherwise.  The class is the row of its widest operand, the type DuckDB computes it in.  q nulls
/ a result landing on the null bit pattern that DuckDB keeps as the type's lowest value: alike under a compare with a
/ literal inside the type's range (q nulls sort lowest), but not under a further operation, so one operation only; an
/ overflow DuckDB refuses where q wraps is answered by q (.duckdb.i.push).
.sql.i.arith:{[cls;e]
  if[0h<>type e; :(::)]; i:.sql.i.ARITH?e 0; if[(i=4) or (count e)<>3-i=3; :(::)];
  a:1_e; col:-11h=type each a; ints:`int16`int32`int64;
  ok:{[ints;cls;x] $[-11h=type x; cls[x] in ints; (type x) in -5 -6 -7h; not null x; 0b]}[ints;cls];
  if[(not any col) or not all ok each a; :(::)];
  wide:max {[ints;cls;x] $[-11h=type x; ints?cls x; -5h-type x]}[ints;cls] each a;
  ((`$("+";"-";"*";"-") i),@[a;where not col;.sql.i.lit]; .sql.i.class ints wide)};
