/ A SQL SELECT as a q tree, slot for slot the shape q's own select (?) takes: (`sql;source;where;by;columns;limit;order).
/ .sql.render writes a tree as SQL text; .sql.parse reads SQL text into a tree.
/ .
/ source   the table: `t, a dotted `db.s.t, or `t`u for the table t under the alias u
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
/ function it names.  Spell a head that is not a q name with `$: `$"<=", `$"is null".
/ @eg .sql.render (`sql;`t;enlist (`$">";`price;100);0b;`sym`p!(`sym;`max`price);5;enlist (`$">:";`p))
/ @eg .sql.parse "SELECT sym, max(price) AS p FROM t WHERE price > 100 GROUP BY sym ORDER BY p DESC LIMIT 5"

/ The SQL text of a tree, written by q alone.  A tree the grammar above cannot read signals 'type, a head with the
/ wrong number of arguments 'length.
/ @return string
.sql.render:{[tree]
  if[not (7=count tree) and `sql~first tree; '`type];
  src:tree 1; wh:tree 2; by:tree 3; cl:tree 4; lim:tree 5; ord:tree 6;
  if[not (99h=type cl) and (-11h=type src) or (11h=type src) and 2=count src; '`type];
  column:{[k;e] r:.sql.i.expr e; $[k~`$r; r; r," AS ",.sql.i.quote string k]};
  order:{[o]
    if[not (count o) in 2 3; '`length];
    d:(`;`$"<:";`$">:")?o 0; n:`$("nulls first";"nulls last");
    if[(d>2) or (3=count o) and not o[2] in n; '`type];
    .sql.i.expr[o 1],(("";" ASC";" DESC") d),$[3=count o;" ",upper string o 2;""]};
  s:"SELECT ",$[1b~by;"DISTINCT ";""],$[count cl; ", " sv column'[key cl;value cl]; "*"];
  s,:" FROM ",$[-11h=type src; .sql.i.name src; (.sql.i.name src 0)," AS ",.sql.i.quote string src 1];
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
  if[h=`in; :.sql.i.sub[p;a 0]," IN (",(", " sv .sql.i.expr each 1_a),")"];
  if[h=`cast; :"CAST(",.sql.i.expr[a 1]," AS ",string[a 0],")"];
  if[10=p; :(string h),.sql.i.sub[p;a 0]];
  if[h in key .sql.i.INFIX; :.sql.i.sub[$[null p;10;h in .sql.i.LEFT;p-1;p];a 0]," ",upper[string h]," ",.sql.i.sub[$[null p;10;p];a 1]];
  if[(h=`count) and a~enlist `$"*"; :"count(*)"];
  (.sql.i.quote string h),"(",(", " sv .sql.i.expr each a),")"};

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
