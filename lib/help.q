/ help.q — the .help doc store: every doc comment the engine saw, queryable.
/ PURE q by owner ruling — no C implements .help; the script seam only calls
/ the two hooks below.  Re-pointing them is NOT a supported contract.
/ Schema after qstudio's man.q.  Headers are parsed ON INGEST — only the
/ structured rows are stored, so a changed parse rule means "reload the file".
/ Loaded FIRST in the bundle (Makefile LIB_Q_SRCS) — the one load order that
/ is not moot, since capture needs the hooks already bound.
.help.funcs:([fullname:`$()] ns:`$(); file:`$(); line:`long$())
.help.args:([] fullname:`$(); tag:`$(); param:`$(); description:())
.help.files:([file:`$()] ns:`$())
.help.filetags:([] file:`$(); tag:`$(); val:())

.help.i.str:{$[10h=abs type x;x;string x]}

/ NOT built-in trim/rtrim, deliberately: trim strips the char null (" ") only,
/ and @tag parsing wants " \t\n\r" so tab-indented tags still parse.
.help.i.rstrip:{$[count i:where not x in " \t\n\r";(1+last i)#x;""]}
.help.i.strip:{.help.i.rstrip $[count i:where not x in " \t\n\r";(first i)_x;""]}

.help.i.join:{[lines] $[count i:where 0<count each lines;"\n" sv lines (first i)+til 1+(last i)-first i;""]}

/ header -> rows (tag;param;description;val), the lead description first under
/ a null tag: the lead is EVERY line before the first tag, a non-tag line
/ after a tag continues THAT tag, and a bare @ is text, never a tag — so it
/ cannot collide with the lead row's null tag.  A NAME is split off for @param
/ and @exception only; every other tag, known or invented, is kept under its
/ own name with its text unread — no whitelist, so no tag is ever dropped.
.help.i.parse:{[header]
  text:{[l] l:.help.i.strip l; l:$[count i:where not l="/";(first i)_l;""]; $[(count l)and (first l)in " \t";1_l;l]};
  istag:{[l] l:.help.i.strip l;(1<count l)and"@"=first l};
  tag:{[blk]
    word:{[s] i:$[count j:where s in " \t";first j;count s];(i#s;.help.i.strip i _ s)};
    w:word 1_.help.i.strip first blk;
    v:.help.i.join (enlist w 1),1_blk;
    $[(`$w 0)in`param`exception;
      [p:word w 1;(`$w 0;`$p 0;.help.i.join (enlist p 1),1_blk;v)];
      (`$w 0;`;v;v)]};
  s:text each "\n" vs header;
  g:sums istag each s;
  d:.help.i.join s where g=0;
  n:$[count g;`long$last g;0];
  ($[count d;enlist(`;`;d;d);()]),tag each {[s;g;i] s where g=i}[s;g] each 1+til n}

.help.i.args:{[fullname;header] r:.help.i.parse header;$[count r;((count r)#fullname;r[;0];r[;1];r[;2]);(`$();`$();`$();())]}
.help.i.filetags:{[file;header] r:.help.i.parse header;$[count r;((count r)#file;r[;0];r[;3]);(`$();`$();())]}

/ begin this file — replace everything previously known about it.  C calls
/ this at every script begin with an empty header, and AGAIN with the real one
/ if the leading run resolves to a file header; the q_comment.h invariant (the
/ leading run resolves before any definition registers) makes the second
/ clear a no-op wipe of nothing.  An empty header records no file row.
/ .help.args rows carry no file, so the fullname set MUST be read out before
/ the funcs rows are deleted.
.help.register_file:{[file;ns;header]
  fl:file;
  fns:exec fullname from .help.funcs where file=fl;
  delete from `.help.args where fullname in fns;
  delete from `.help.funcs where file=fl;
  delete from `.help.filetags where file=fl;
  delete from `.help.files where file=fl;
  if[count header;
    `.help.files upsert (file;ns);
    `.help.filetags insert .help.i.filetags[file;header]]; }

/ one documented definition; re-registering replaces its own rows only.  The
/ derived args are unkeyed with many rows per name, so delete-then-insert is
/ the idiom there, not a workaround.
.help.register_definition:{[fullname;ns;file;line;header]
  nm:fullname;
  `.help.funcs upsert (fullname;ns;file;line);
  delete from `.help.args where fullname=nm;
  `.help.args insert .help.i.args[fullname;header]; }

/ one name's documentation as text — its description, then its tags.
/ @param name (symbol|string) the fullname a definition was captured under
/ @return (string) the rendered page, or a one-line "no documentation" note
.help.get:{[name]
  render:{[n;r]
    tagline:{[t;p;d]
      h:"  @",string[t],$[null p;"";" ",string p];
      ls:"\n" vs d;
      (enlist $[count first ls;h,"  ",first ls;h]),"    ",/:1_ls};
    d:$[count i:where null r`tag;r[first i;`description];""];
    t:select from r where not null tag;
    (enlist string n),($[count d;"  ",/:"\n" vs d;()]),($[count t;enlist"";()]),
     raze tagline'[t`tag;t`param;t`description]};
  n:`$.help.i.str name;
  r:select tag,param,description from .help.args where fullname=n;
  $[count r;"\n" sv render[n;r];"no documentation for ",string n]}

/ the online docs base; "" disables the whole web tier (offline / tests).
.help.url:"https://peachq.org/"
.help.i.ix:()

/ GET one path off .help.url — the ONE trapped door to the network the web
/ tier uses; "" on any failure or when the tier is disabled.
.help.i.get:{[path] $[count .help.url;@[{.Q.hg x};.help.url,path;{""}];""]}

/ the online topic index (help.csv: pagepath,qname,kind), fetched on FIRST
/ use and cached for the session; offline caches as the empty table — reset
/ with .help.i.ix:() to retry after gaining net access.
/ @return (table) pagepath (string), qname (string), kind (symbol)
.help.index:{[]
  if[()~.help.i.ix;
    r:.help.i.get"help.csv";
    .help.i.ix::$[count r;("**S";enlist",")0:"\n"vs .help.i.rstrip r;([]pagepath:();qname:();kind:`$())]];
  .help.i.ix}

/ the reference page (markdown) for one online-index topic; "" when offline,
/ disabled or not an index qname.
/ @param topic (symbol|string) the topic as the online index names it
/ @return (string) the page markdown, or ""
.help.webfetch:{[topic]
  t:.help.i.str topic;
  $[any t~/:.help.index[]`qname;.help.i.get"help.md?q=",.h.hu t;""]}

/ one exact-name pass of the ladder: (local page;online page), either "".
.help.i.ladder:{[s] n:`$s;
  ($[n in exec fullname from .help.funcs;.help.get n;""];.help.webfetch s)}

/ the help ladder as a VALUE (the IPC-friendly form): the local page for an
/ exact captured name joined with the online page for an exact index topic;
/ when both miss, .help.find — exactly one documented match answers ITS page,
/ else the matching rows come back as a table to narrow by (empty = no match).
/ @param pattern (symbol|string) a name, or a pattern as .help.find takes it
/ @return (string|table) the help text, or the matching doc rows
.help.text:{[pattern]
  lw:.help.i.ladder .help.i.str pattern;
  t:lw where 0<count each lw;
  if[count t;:"\n" sv t];
  r:.help.find pattern;
  $[1=count m:distinct r`fullname;.help.get first m;r]}

/ render a fetched page for the console behind a `│ ` gutter, obeying the
/ effective `\c` — its rows bound the preview, its cols clip each line
/ (console `..` rule) — ending in a `.. N more` pointer (??topic / the
/ website show everything).  ```q/```syntax block bodies get one tint, fence
/ lines the gutter grey.  .pq.termsize fills auto (`0N`) `\c` axes and
/ .pq.cancolor gates ALL the ANSI — both soft by-name calls, so a host
/ without .pq degrades to a plain 25x80 preview.
.help.i.page:{[topic;md]
  ls:"\n" vs .help.i.rstrip md;
  c:@[{value[x][]};`.pq.termsize;25 80]^system"c";
  n:count ls;
  ls:(n&c 0)#ls;
  ls:{[w;l]$[w<count l;((0|w-2)#l),"..";l]}[0|c[1]-3] each ls;
  fen:ls like\:"```*";
  qf:{(x like "```q*")or x like "```syntax*"}each ls;
  st:{[s;f]$[f 0;$[s 0;00b;1b,f 1];s]}\[00b;fen,'qf];
  cc:@[{value[x][]};`.pq.cancolor;0b];
  if[cc;ls:?[fen;{"\033[90m",x,"\033[0m"}each ls;
              ?[st[;0]&st[;1]&not fen;{"\033[36m",x,"\033[0m"}each ls;ls]]];
  out:$[cc;"\033[90m│ \033[0m";"│ "],/:ls;
  if[n>c 0;
    x:".. ",string[n-c 0]," more lines: ??",topic,"  or  ",.help.url,"help?q=",.h.hu topic;
    out,:enlist $[cc;"\033[90m",x,"\033[0m";x]];
  "\n" sv out}

/ the console door: the local page prints plainly; a fetched page renders as
/ the gutter preview (.help.i.page); the find fallback as its page or table.
/ @param pattern (symbol|string) a name, or a pattern as .help.find takes it
.help.help:{[pattern]
  s:.help.i.str pattern;
  lw:.help.i.ladder s;
  if[count first lw;-1 first lw];
  if[count last lw;-1 .help.i.page[s;last lw]];
  if[0=sum count each lw;
    r:.help.find pattern;
    $[1=count m:distinct r`fullname;-1 .help.get first m;show r]];}

/ the full unclipped ladder (`??topic`): plain text, no gutter, no preview.
.help.full:{[pattern] r:.help.text pattern; $[10h=type r;-1 r;show r];}

/ the basic datatype reference (ref/card.md shape), spelled by the engine:
/ the nulls and infinities are TYPED literals, n derives from their types,
/ c from .Q.t, the null/inf cells are their -3! renderings (so the table can
/ never drift from what the display prints — 0Wh shows 32767h, doc-true),
/ and the inf rows self-align by type number.
/ @return (table) n, c, name, sz (bytes), literal, null, inf, sql
.help.types:{[]
  nul:(0b;0Ng;0x00;0Nh;0Ni;0N;0Ne;0n;" ";`;0Np;0Nm;0Nd;0Nz;0Nn;0Nu;0Nv;0Nt);
  inf:(0Wh;0Wi;0W;0We;0w;0Wp;0Wm;0Wd;0Wz;0Wn;0Wu;0Wv;0Wt);
  n:0h,abs type each nul;
  ([]n;c:"*",.Q.t 1_n;
    name:`list`boolean`guid`byte`short`int`long`real`float`char`symbol`timestamp`month`date`datetime`timespan`minute`second`time;
    sz:0N 1 16 1 2 4 8 4 8 1 0N 8 4 4 8 8 4 4 4;
    literal:("";"0b";"";"0x00";"0h";"0i";"0j";"0e";"0.0";"\" \"";"`";"dateDtimespan";"2000.01m";"2000.01.01";"dateTtime";"00:00:00.000000000";"00:00";"00:00:00";"00:00:00.000");
    null:enlist[""],-3!'nul;
    inf:@[count[n]#enlist"";n?abs type each inf;:;-3!'inf];
    sql:("";"";"";"";"smallint";"int";"bigint";"real";"float";"";"varchar";"";"";"date";"timestamp";"";"";"";"time"))}

/ the one-line summary for a name — the first line of its lead description,
/ "" when undocumented.  The REPL hint renders `?name / <this>` and owns the
/ width clipping, so the line comes back untrimmed.
/ @param name (symbol|string) the fullname
.help.oneline:{[name]
  n:`$.help.i.str name;
  r:select description from .help.args where fullname=n,null tag;
  $[count r;first "\n" vs r[0;`description];""]}

/ register one builtin's one-liner (repeated a LOT below — keep calls short).
.help.i.r:{[fullname;description]
  .help.register_definition[fullname;`$"."sv -1_"."vs string fullname;`;0N;description];}

/ the doc rows whose tag, param or description match — a glob, matched
/ anywhere and case-insensitively, so a bare word is a substring search.
/ @param pattern (string|symbol) the pattern
/ @return (table) the matching rows of .help.args
.help.find:{[pattern]
  p:"*",lower[.help.i.str pattern],"*";
  p:p where not (p="*")and"*"=next p;   / a user's own leading * would make ** — 'nyi here
  t:.help.args;
  $[count t;t where((lower each string t`tag)like p)or((lower each string t`param)like p)or(lower each t`description)like p;t]}

/ >>> GENERATED from lib/help-builtins.tsv by `python3 tools/gen-help-builtins.py`
/ >>> edit the TSV, rerun (it splices this block in place), commit both.
.help.i.r[`abs;"Absolute value — Where `x` is a numeric, returns the absolute value of `x`.\n@syntax abs x    abs[x]\n@example q)abs -1.0\n1f\nq)abs 10 -43 0N\n10 43 0N\nq)abs 1999.01.01\n2000.12.31\n@see ref/abs/"]
.help.i.r[`aj;"As-of join. Returns boundary time from t1.\n@syntax aj[c1...cn;t1;t2]\n@example aj[`sym`time; trade; quote]\n@see ref/aj/#aj-aj0"]
.help.i.r[`aj0;"As-of join. Returns actual time from t2.\n@see ref/aj/#aj-aj0"]
.help.i.r[`ajf;"Since V3.6 2018.05.18 `ajf` and `ajf0` behave as V2.8 `aj` and `aj0`; they fill from `t1` if the corresponding value in `t2` is null.\n@example q)t0:([]time:2#00:00:01;sym:`a`b;p:1 1;n:`r`s)\nq)t1:([]time:2#00:00:01;sym:`a`b;p:0 1)\nq)t2:([]time:2#00:00:00;sym:`a`b;p:1 0N;n:`r`s)\nq)t0~ajf[`sym`time;t1;t2]\n1b\n@see ref/aj/#ajf-ajf0"]
.help.i.r[`ajf0;"Since V3.6 2018.05.18 `ajf` and `ajf0` behave as V2.8 `aj` and `aj0`; they fill from `t1` if the corresponding value in `t2` is null.\n@see ref/aj/#ajf-ajf0"]
.help.i.r[`all;"Is every item true? — Returns a boolean atom `1b` if `x` is\n@syntax all x    all[x]\n@example q)all 1 2 3 = 1 2 4\n0b\nq)all 1 2 3 = 1 2 3\n1b\nq)all \"YNYN\" / string casts to 1111b\n1b\n@see ref/all-any/#all"]
.help.i.r[`any;"Is there a true item? — Returns a boolean atom `1b` if `x` is\n@syntax any x    any[x]\n@example q)any 1 2 3 = 10 20 4\n0b\nq)any 1 2 3 = 1 20 30\n1b\nq)any \"YNYN\" / string casts to 1111b\n1b\n@see ref/all-any/#any"]
.help.i.r[`and;"Lesser of two values; logical AND — Returns the lesser of the underlying values of `x` and `y`.\n@syntax x & y         &[x;y]\nx and y       and[x;y]\n@example q)2&3\n2\nq)1010b and 1100b  /logical AND with booleans\n1000b\nq)\"sat\"&\"cow\"\n\"cat\"\n@see ref/lesser/"]
.help.i.r[`asc;"Ascending sort — Where `x` is a\n@syntax asc x     asc[x]\n@example q)asc 2 1 3 4 2 1 2\n`s#1 1 2 2 2 3 4\n\nq)a:0 1\nq)b:a\nq)asc b  / result has sorted attribute applied\n@see ref/asc/#asc"]
.help.i.r[`iasc;"Ascending grade — Where `x` is a list or dictionary, returns the indices needed to sort the list `x` in ascending order.\n@syntax iasc x    iasc[x]\n@example q)L:2 1 3 4 2 1 2\nq)iasc L\n1 5 0 4 6 2 3\nq)L iasc L\n1 1 2 2 2 3 4\nq)(asc L)~L iasc L\n@see ref/asc/#iasc"]
.help.i.r[`xasc;"Sort a table in ascending order of specified columns. — Where `x` is a symbol vector of column names defined in table `y`, which is passed by\n@syntax x xasc y     xasc[x;y]\n@see ref/asc/#xasc"]
.help.i.r[`asof;"As-of join — Where\n@syntax t asof d     asof[t;d]\n@see ref/asof/"]
.help.i.r[`attr;"Attribute of an object — Where `x` is any object, returns its attribute as a symbol atom.\n@syntax attr x     attr[x]\n@example q)attr 1 3 4\n`\nq)attr asc 1 3 4\n`s\n@see ref/attr/"]
.help.i.r[`avg;"Arithmetic mean — Where `x` is a numeric or temporal list, returns the arithmetic mean as a float.\n@syntax avg x     avg[x]\n@example q)avg 1 2 3\n2f\nq)avg 1 0w\n0w\nq)avg -0w 0w\n0n\n@see ref/avg/#avg"]
.help.i.r[`avgs;"Running averages — Where `x` is a numeric or temporal list, returns the running averages, i.e.\n@syntax avgs x     avgs[x]\n@example q)avgs 1 2 3 0n 4 -0w 0w\n1 1.5 2 2 2.5 -0w 0n\n@see ref/avg/#avgs"]
.help.i.r[`mavg;"Moving averages — Where\n@syntax x mavg y     mavg[x;y]\n@example q)2 mavg 1 2 3 5 7 10\n1 1.5 2.5 4 6 8.5\nq)5 mavg 1 2 3 5 7 10\n1 1.5 2 2.75 3.6 5.4\nq)5 mavg 0N 2 0N 5 7 0N       / first item of the result is null\n0n 2 2 3.5 4.666667 4.666667\n@see ref/avg/#mavg"]
.help.i.r[`wavg;"Weighted average — Where\n@syntax x wavg y     wavg[x;y]\n@example q)2 3 4 wavg 1 2 4\n2.666667\nq)2 0N 4 5 wavg 1 2 0N 8  / nulls in either argument are ignored\n6f\nq)0 wavg 2 3\n0n                        / since 4.1t 2021.09.03,4.0 2021.10.01, previously returned 2.5\n@see ref/avg/#wavg"]
.help.i.r[`bin;"Binary search — Where\n@syntax x bin  y    bin[x;y]\nx binr y    binr[x;y]\n@example q)0 2 4 6 8 10 bin 5\n2\nq)0 2 4 6 8 10 bin -10 0 4 5 6 20\n-1 0 2 2 3 5\n\nq)0 1 1 2 bin 0 1 2\n@see ref/bin/"]
.help.i.r[`binr;"Binary search — Where\n@syntax x bin  y    bin[x;y]\nx binr y    binr[x;y]\n@see ref/bin/"]
.help.i.r[`ceiling;"Round up — Returns the least integer greater than or equal to boolean or numeric `x`.\n@syntax ceiling x      ceiling[x]\n@example q)ceiling -2.1 0 2.1\n-2 0 3\nq)ceiling 01b\n0 1i\n@see ref/ceiling/"]
.help.i.r[`count;"Count the items of a list or dictionary.\n@syntax count x     count[x]\n@example q)count 0                            / atom\n1\nq)count \"zero\"                       / vector\n4\nq)count (2;3 5;\"eight\")              / mixed list\n3\n@see ref/count/#count"]
.help.i.r[`mcount;"Moving counts — Where\n@syntax x mcount y     mcount[x;y]\n@example q)3 mcount 0 1 2 3 4 5\n1 2 3 3 3 3\nq)3 mcount 0N 1 2 3 0N 5\n0 1 2 3 2 2\n@see ref/count/#mcount"]
.help.i.r[`cols;"Column names of a table — Where `x` is a\n@syntax cols x    cols[x]\n@example q)\\l trade.q\nq)cols trade            /value\n `time`sym`price`size\nq)cols`trade            /reference\n `time`sym`price`size\n@see ref/cols/#cols"]
.help.i.r[`xcol;"Rename table columns — Where `y` is a table passed by value, and `x` is\n@syntax x xcol y    xcol[x;y]\n@example q)t:([]a:3 4 5; b:6 7 8; c:`z`u`i)\nq)`d`e xcol t                               / rename first two columns\nd e c\n-----\n3 6 z\n4 7 u\n@see ref/cols/#xcol"]
.help.i.r[`xcols;"Reorder table columns — Where\n@syntax x xcols y    xcols[x;y]\n@example q)t:([]a:3 4 5; b:6 7 8; c:`z`u`i)\nq)`b xcols t\nb a c\n-----\n6 3 z\n7 4 u\n@see ref/cols/#xcols"]
.help.i.r[`cor;"Correlation — Where `x` and `y` are conforming numeric lists, returns their (Pearson) correlation as a float in the range `-1f` to `1f`.\n@syntax x cor y    cor[x;y]\n@example q)29 10 54 cor 1 3 9\n0.7727746\nq)10 29 54 cor 1 3 9\n0.9795734\nq)1 3 9 cor 1 3 9\n1f\n@see ref/cor/"]
.help.i.r[`cos;"Cosine, arccosine — Where `x` is a numeric, returns\n@syntax cos x     cos[x]\nacos x    acos[x]\n@example q)cos 0.2                       / cosine\n0.9800666\nq)min cos 10000?3.14159265\n-1f\nq)max cos 10000?3.14159265\n1f\n@see ref/cos/"]
.help.i.r[`acos;"Cosine, arccosine — Where `x` is a numeric, returns\n@syntax cos x     cos[x]\nacos x    acos[x]\n@see ref/cos/"]
.help.i.r[`cov;"Where `x` and `y` are conforming numeric lists, returns their covariance as a floating-point number.\n@syntax x cov y    cov[x;y]\n@example q)2 3 5 7 cov 3 3 5 9\n4.5\nq)t:([]a:2 3 5 7;b:4 3 0 2)\nq)exec a cov b from t\n-1.8125\n@see ref/cov/#cov"]
.help.i.r[`scov;"Sample covariance — Where `x` and `y` are conforming numeric lists, returns their sample covariance as a float atom.\n@syntax x scov y    scov[x;y]\n@example q)2 3 5 7 scov 3 3 5 9\n6f\nq)t:([]a:2 3 5 7;b:4 3 0 2)\nq)exec a scov b from t\n-2.416667\n@see ref/cov/#scov"]
.help.i.r[`cross;"Cross product — Returns the cross (or Cartesian) product (that is, all possible pairings) of lists `x` and `y`.\n@syntax x cross y    cross[x;y]\n@example q)1 2 3 cross 10 20\n1 10\n1 20\n2 10\n2 20\n3 10\n@see ref/cross/"]
.help.i.r[`csv;"CSV delimiter — A synonym for `\",\"` for use in preparing text for CSV files, or reading them.\n@syntax csv\n@see ref/csv/"]
.help.i.r[`cut;"Cut a list or table into a matrix of x columns or as _ Cut operator.\n@syntax x cut y     cut[x;y]\n@example q)4 cut til 10\n0 1 2 3\n4 5 6 7\n8 9\n@see ref/cut/"]
.help.i.r[`delete;"Delete rows or columns from a table, entries from a dictionary, or objects from a namespace — For the Delete operator `!`, see\n@syntax delete    from x\ndelete    from x where pw\ndelete ps from x\n@see ref/delete/"]
.help.i.r[`deltas;"Differences between adjacent list items — Where `x` is a numeric or temporal vector, returns differences between consecutive pairs of its items, with the first item of the result being the first item of `x`.\n@syntax deltas x    deltas[x]\n@example q)deltas 1 4 9 16\n1 3 5 7\nq)t:([]time:2020.01.01D09:00:00+1000*til 6; sym:`GOOG`AAPL`AAPL`GOOG`AAPL`GOOG; price:51 54 54 52 53 53)\nq)show t:update diff:deltas price by sym from t\ntime                          sym  price diff\n---------------------------------------------\n@see ref/deltas/"]
.help.i.r[`suite;"Descending sort.\n@see ref/suite/#suite"]
.help.i.r[`idesc;"Descending grade.\n@syntax r:idesc L\n@example idesc (2 1 3 4 2 1 2)\n@see ref/suite/#idesc"]
.help.i.r[`xdesc;"Sorts a table in descending order of specified columns.\n@syntax q)r:cols xdesc table\n@example q)\\l sp.q \nq)s \ns | name  status city \n--| ------------------- \ns1| smith 20     london \ns2| jones 10     paris \ns3| blake 30     paris \ns4| clark 20     london \ns5| adams 30     athens \nq)`city xdesc s                 / sort descending by city \ns | name  status city \n--| ------------------- \ns2| jones 10     paris \ns3| blake 30     paris \ns1| smith 20     london \ns4| clark 20     london \ns5| adams 30     athens\n@see ref/suite/#xdesc"]
.help.i.r[`dev;"Standard deviation — Where `x` is a numeric list, returns its standard deviation (the square root of the variance).\n@syntax dev x     dev[x]\n@example q)dev 10 343 232 55\n134.3484\n@see ref/dev/#dev"]
.help.i.r[`mdev;"Moving deviations — Where\n@syntax x mdev y     mdev[x;y]\n@example q)2 mdev 1 2 3 5 7 10\n0 0.5 0.5 1 1 1.5\nq)5 mdev 1 2 3 5 7 10\n0 0.5 0.8164966 1.47902 2.154066 2.87054\nq)5 mdev 0N 2 0N 5 7 0N      / the first item is null\n0n 0 0 1.5 2.054805 2.054805\n@see ref/dev/#mdev"]
.help.i.r[`sdev;"Sample standard deviation — Where `x` is a numeric list, returns its sample standard deviation, the square root of the sample variance.\n@syntax sdev x     sdev[x]\n@example q)sdev 10 343 232 55\n155.1322\n@see ref/dev/#sdev"]
.help.i.r[`differ;"Find where list items change value — Returns a boolean list indicating where consecutive pairs of items in `x` differ.\n@syntax differ x    differ[x]\n@example q)differ`IBM`IBM`MSFT`CSCO`CSCO\n10110b\nq)differ 1 3 3 4 5 6 6\n1101110b\nq)differ (7;`a;`a;09:34)\n1101b\n@see ref/differ/"]
.help.i.r[`distinct;"Unique items of a list — Where `x` is a list, returns the distinct (unique) items of `x` in the order of their first occurrence.\n@syntax distinct x    distinct[x]\n@example q)distinct 2 3 7 3 5 3\n2 3 7 5\n@see ref/distinct/"]
.help.i.r[`div;"Integer division — Returns the greatest whole number that does not exceed `x%y`.\n@syntax x div y    div[x;y]\n@example q)7 div 3\n2\n\nq)7 div 2 3 4\n3 2 1\n\n@see ref/div/"]
.help.i.r[`dsave;"Write global tables to disk as splayed, enumerated, indexed kdb+ tables. — Where\n@syntax x dsave y     dsave[x;y]\n@see ref/dsave/"]
.help.i.r[`each;"Iterate a unary — Where\n@syntax v1 each x   each[v1;x]       v1 peach x   peach[v1;x]\n(vv)each x   each[vv;x]      (vv)peach x   peach[vv;x]\n@example q)count each (\"the\";\"quick\";\" brown\";\"fox\")\n3 5 6 3\nq)(+\\)peach(2 3 4;(5 6;7 8);9 10 11 12)\n2 5 9\n(5 6;12 14)\n9 19 30 42\n@see ref/each/"]
.help.i.r[`peach;"And any system command which might cause a change of global state.\n@example {sum exp x?1.0}peach 2#1000000\n@see ref/each/"]
.help.i.r[`ej;"Equi join — Where\n@syntax ej[c;t1;t2]\n@see ref/ej/"]
.help.i.r[`ema;"Exponential moving average — Where\n@syntax x ema y    ema[x;y]\n@example q)ema[1%3;1,10#0]\n1 0.6666667 0.4444444 0.2962963 0.1975309 0.1316872 0.0877915 0.05852766 0.03901844 0.02601229 0.01734153\n@see ref/ema/"]
.help.i.r[`enlist;"Make a list — Returns a list with its argument/s as items.\n@syntax enlist x    enlist[x]    enlist[x;y;z;…]\n@example q)a:10\nq)b:enlist a\nq)c:enlist b\nq)type each (a;b;c)\n-7 7 0h\nq)a~b\n@see ref/enlist/"]
.help.i.r[`eval;"Evaluate a parse tree — Where `x` is a parse tree, returns the result of evaluating it.\n@syntax eval x     eval[x]\n@example q)parse \"2+3\"\n+\n2\n3\nq)eval parse \"2+3\"\n5\n@see ref/eval/#eval"]
.help.i.r[`reval;"Restricted evaluation of a parse tree — The `reval` function is similar to `eval`, and behaves as if the command-line option `-b` were active during evaluation.\n@syntax reval x     reval[x]\n@example q).z.pg:{reval(value;enlist x)} / define in process listening on port 5000\nq)h:hopen 5000 / from another process on same host\nq)h\"a:4\"\n'noupdate: `. `a\n@see ref/eval/#reval"]
.help.i.r[`except;"Exclude items from a list — Where\n@syntax x except y    except[x;y]\n@example q)1 2 3 except 2\n1 3\nq)1 2 3 4 1 3 except 2 3\n1 4 1\n@see ref/except/"]
.help.i.r[`exec;"Return selected rows and columns from a table — For the Exec operator `?`, see\n@example q)\\l sp.q\n\nq)exec from sp  / last record\ns  | `s!0\np  | `p$`p5\nqty| 400\n@see ref/exec/"]
.help.i.r[`exit;"Terminate kdb+ — Control word.\n@syntax exit x    exit[x]\n@example q)exit 0        / typical successful exit status\n..\n\nq)exit 42\n@see ref/exit/"]
.help.i.r[`exp;"Raise e to a power — Where\n@syntax exp x     exp[x]\n@example q)exp 1\n2.718282\n\nq)exp 0.5\n1.648721\n\n@see ref/exp/#exp"]
.help.i.r[`xexp;"Raise x to a power — Where `x` and `y` are numerics, returns as a float where `x` is\n@syntax x xexp y    xexp[x;y]\n@example q)2 xexp 8\n256f\n\nq)-2 2 xexp .5\n0n 1.414214\n\n@see ref/exp/#xexp"]
.help.i.r[`fby;"Apply an aggregate to groups — Where\n@syntax (aggr;d) fby g\n@example q)show dat:10?10\n4 9 2 7 0 1 9 2 1 8\nq)grp:`a`b`a`b`c`d`c`d`d`a\nq)(sum;dat) fby grp\n14 16 14 16 9 4 9 4 4 14\n@see ref/fby/"]
.help.i.r[`fills;"Replace nulls with preceding non-nulls.\n@syntax r:fills A\n@example fills (0N 2 3 0N 0N 7 0N)\n@see ref/fills/"]
.help.i.r[`first;"First item of a list — Where `x` is a list or dictionary, returns its first item, else `x`.\n@syntax first x    first[x]\n@example q)first 1 2 3 4 5\n1\nq)first 42\n42\nq)RaggedArray:(1 2 3;4 5;6 7 8 9;0)\nq)first each RaggedArray\n@see ref/first/#first"]
.help.i.r[`last;"Last item of a list — Where `x` is a list or dictionary, returns its last item; otherwise `x`.\n@syntax last x    last[x]\n@example q)last til 10\n9\nq)last `a`b`c!1 2 3\n3\nq)last 42\n42\n@see ref/first/#last"]
.help.i.r[`fkeys;"Foreign-key columns of a table — Where `x` is a table, returns a dictionary that maps foreign-key columns to their tables.\n@syntax fkeys x    fkeys[x]\n@see ref/fkeys/"]
.help.i.r[`flip;"Returns `x` transposed, where `x` may be a list of lists, a dictionary or a table.\n@syntax flip x     flip[x]\n@example q)flip (1 2 3;4 5 6)\n1 4\n2 5\n3 6\n@see ref/flip/"]
.help.i.r[`floor;"Round down — Returns the greatest integer less than or equal to numeric `x`.\n@syntax floor x    floor[x]\n@example q)floor -2.1 0 2.1\n-3 0 2\n@see ref/floor/"]
.help.i.r[`get;"Read or memory-map a variable or kdb+ data file — Where `x` is\n@syntax get x     get[x]\n@example q)a:42\nq)get `a\n42\n\nq)\\l trade.q\nq)`:NewTrade set trade                  / save trade data to file\n@see ref/get/#get"]
.help.i.r[`set;"Assign a value to a global variable. Persist an object as a file or directory.\n@example q)`a set 42                         / set global variable\n`a\nq)a\n42\n\nq)`:a set 42                        / serialize object to file\n@see ref/get/#set"]
.help.i.r[`getenv;"Get the value of an environment variable — where `x` is a symbol atom naming an environment variable, returns its value.\n@syntax getenv x     getenv[x]\n@example q)getenv `SHELL\n\"/bin/bash\"\nq)getenv `UNKNOWN      / returns empty if variable not defined\n\"\"\n@see ref/getenv/#getenv_1"]
.help.i.r[`setenv;"Set the value of an environment variable — where\n@syntax x setenv y     setenv[x;y]\n@example q)`RTMP setenv \"/home/user/temp\"\nq)getenv `RTMP\n\"/home/user/temp\"\nq)\\echo $RTMP\n\"/home/user/temp\"\n@see ref/getenv/#setenv"]
.help.i.r[`group;"Returns a dictionary in which the keys are the distinct items of `x`, and the values the indexes where the distinct items occur.\n@syntax group x     group[x]\n@example q)group \"mississippi\"\nm| ,0\ni| 1 4 7 10\ns| 2 3 5 6\np| 8 9\n@see ref/group/"]
.help.i.r[`gtime;"UTC equivalent of local timestamp — Where `ts` is a datetime/timestamp, returns the UTC datetime/timestamp.\n@syntax gtime ts    gtime[ts]\n@example q).z.p\n2009.10.20D10:52:17.782138000\nq)gtime .z.P                      / same timezone as .z.p\n2009.10.20D10:52:17.783660000\n@see ref/gtime/#gtime"]
.help.i.r[`ltime;"Local equivalent of UTC timestamp — Where `ts` is a datetime/timestamp, returns the local datetime/timestamp.\n@syntax ltime ts    ltime[ts]\n@example q).z.P\n2009.11.05D15:21:10.040666000\nq)ltime .z.p                  / same timezone as .z.P\n2009.11.05D15:21:10.043235000\n@see ref/gtime/#ltime"]
.help.i.r[`hcount;"Size of a file in bytes — Where `x` is a file symbol, returns as a long the size of the file.\n@syntax hcount x     hcount[x]\n@example q)hcount`:c:/q/test.txt\n42\n@see ref/hcount/"]
.help.i.r[`hdel;"Delete a file or folder — Where `x` is a file symbol atom, deletes the file or folder and returns `x`.\n@syntax hdel x     hdel[x]\n@example q)hdel`:test.txt   / delete test.txt in current working directory\n`:test.txt\nq)hdel`:test.txt   / should generate an error\n'test.txt: No such file or directory\n@see ref/hdel/"]
.help.i.r[`hopen;"Open a connection to a file or process — Where\n@syntax hopen filehandle\nhopen processhandle\nhopen (communicationhandle;timeout)\nhopen port\n@example hopen \":path/to/file.txt\"                   / filehandle\nhopen `:unix://5010                         / localhost, Unix domain socket\nhopen `:tcps://mydb.us.com:5010             / SSL/TLS with hostname\nhopen(\":10.43.23.198:5010\";10000)           / IP address and timeout\nhopen 5010                                  / local port number\n@see ref/hopen/#hopen"]
.help.i.r[`hclose;"Close a connection to a file or process — Where `x` is a connection handle, closes the connection, and destroys the handle.\n@syntax hclose x     hclose[x]\n@example q)show h:hopen `::5001\n3i\nq)h\"til 5\"\n0 1 2 3 4\nq)hclose h\nq)h\"til 5\"\n@see ref/hopen/#hclose"]
.help.i.r[`hsym;"Symbol/s to file or process symbol/s — Where `x` is a symbol atom or vector (since V3.1) returns the symbol/s prefixed with a colon if it does begin with one.\n@syntax hsym x     hsym[x]\n@example q)hsym`c:/q/test.txt                / file path to symbolic file handle\n`:c:/q/test.txt\nq)hsym`10.43.23.197                 / IP address to symbolic handle\n`:10.43.23.197\nq)hsym `host:port`localhost:8001    / hostname to symbolic handle\n`:host:port`:localhost:8001\n@see ref/hsym/"]
.help.i.r[`ij;"Inner join — Where\n@syntax x ij  y     ij [x;y]\nx ijf y     ijf[x;y]\n@see ref/ij/"]
.help.i.r[`ijf;"Inner join — Where\n@syntax x ij  y     ij [x;y]\nx ijf y     ijf[x;y]\n@see ref/ij/"]
.help.i.r[`in;"Whether x is an item of y — Where `y` is\n@syntax x in y    in[x;y]\n@example q)\"x\" in \"a\"                                    / atom in atom\n0b\nq)\"x\" in \"acdexyz\"                              / atom in vector\n1b\nq)\"wx\" in \"acdexyz\"                             / vector in vector\n01b\n@see ref/in/"]
.help.i.r[`insert;"Insert or append records to a table — Where\n@syntax x insert y    insert[x;y]\n@see ref/insert/"]
.help.i.r[`inter;"Intersection of two lists or dictionaries — Where `x` and `y` are lists or dictionaries, uses the result of `x in y` to return items or entries from `x`.\n@syntax x inter y    inter[x;y]\n@example q)1 3 4 2 inter 2 3 5 7 11\n3 2\nq)1 2 3 1 4 inter 4 1 4\n1 1 4\n@see ref/inter/"]
.help.i.r[`inv;"Matrix inverse — Returns the inverse of non-singular float matrix `x`.\n@syntax inv x     inv[x]\n@example q)a:3 3#2 4 8 3 5 6 0 7 1f\nq)inv a\n-0.4512195  0.6341463  -0.195122\n-0.03658537 0.02439024 0.1463415\n0.2560976   -0.1707317 -0.02439024\nq)a mmu inv a\n@see ref/inv/"]
.help.i.r[`key;"Where `x` is a dictionary (or the name of one), returns its key.\n@syntax key x     key[x]\n@example q)D:`q`w`e!(1 2;3 4;5 6)\nq)key D\n`q`w`e\nq)key `D\n`q`w`e\n@see ref/key/"]
.help.i.r[`keys;"Key column/s of a table — Where `x` is a table (by value or reference), returns as a symbol vector the primary key column/s of `x` – empty if none.\n@syntax keys x    keys[x]\n@example q)\\l trade.q        / no keys\nq)keys trade\n`symbol$()\nq)keys`trade\n`symbol$()\nq)`sym xkey`trade   / define a key\n@see ref/keys/#keys"]
.help.i.r[`xkey;"Set specified columns as primary keys of a table — Where symbol atom or vector `x` lists columns in table `y`, which is passed by\n@syntax x xkey y    xkey[x;y]\n@example q)\\l trade.q\nq)keys trade\n`symbol$()            / no primary key\nq)`sym xkey trade     / return table with primary key sym\nsym| time         price size\n---| -----------------------\n@see ref/keys/#xkey"]
.help.i.r[`like;"Whether text matches a pattern — Where\n@syntax x like y    like[x;y]\n@example q)`quick like \"qu?ck\"\n1b\nq)`brown like \"br[ao]wn\"\n1b\nq)`quickly like \"quick*\"\n1b\n@see ref/like/"]
.help.i.r[`lj;"Left join — Where\n@syntax x lj  y     lj [x;y]\nx ljf y     ljf[x;y]\n@see ref/lj/"]
.help.i.r[`ljf;"Left join — Where\n@syntax x lj  y     lj [x;y]\nx ljf y     ljf[x;y]\n@see ref/lj/"]
.help.i.r[`load;"Load binary data from a file — Where `x` is\n@syntax load x     load[x]\n@see ref/load/#load"]
.help.i.r[`rload;"Load a splayed table from a directory — Where `x` is the table name as a symbol, the table is read from a directory of the same name.\n@syntax rload x     rload[x]\n@see ref/load/#rload"]
.help.i.r[`log;"Natural logarithm — Where `x` is numeric and\n@syntax log x    log[x]\n@example q)log 1\n0f\nq)log 0.5\n-0.6931472\nq)log exp 42\n42f\n@see ref/log/#log"]
.help.i.r[`xlog;"Logarithm — Returns the base-`xf` logarithm of `yf`, where `xf` and `yf` are `x` and `y` cast to floats, i.e.\n@syntax x xlog y    xlog[x;y]\n@example q)2 xlog 8\n3f\n\nq)2 xlog 0.125\n-3f\n\n@see ref/log/#xlog"]
.help.i.r[`lower;"Shift case — Where `x` is a character or symbol atom or vector, returns it with any bicameral characters in the lower/upper case.\n@syntax lower x     lower[x]\nupper x     upper[x]\n@example q)lower\"IBM\"\n\"ibm\"\nq)lower`IBM\n`ibm\n\nq)upper\"ibm\"\n@see ref/lower/"]
.help.i.r[`upper;"Shift case — Where `x` is a character or symbol atom or vector, returns it with any bicameral characters in the lower/upper case.\n@syntax lower x     lower[x]\nupper x     upper[x]\n@example q)lower\"IBM\"\n\"ibm\"\nq)lower`IBM\n`ibm\n\nq)upper\"ibm\"\n@see ref/lower/"]
.help.i.r[`lsq;"Least squares matrix divide.\n@syntax x lsq y     lsq[x;y]\n@example d:x - (x lsq y) mmu y\n@see ref/lsq/"]
.help.i.r[`max;"Maximum — Where `x` is a non-symbol sortable list, returns the maximum of its items.\n@syntax max x    max[x]\n@example q)max 2 5 7 1 3\n7\nq)max \"genie\"\n\"n\"\nq)max 0N 5 0N 1 3                  / nulls are ignored\n5\n@see ref/max/#max"]
.help.i.r[`maxs;"Maximums — Where `x` is a non-symbol sortable list, returns the running maximums of its prefixes.\n@syntax maxs x    maxs[x]\n@example q)maxs 2 5 7 1 3\n2 5 7 7 7\nq)maxs \"genie\"\n\"ggnnn\"\nq)maxs 0N 5 0N 1 3         / initial nulls return negative infinity\n-0W 5 5 5 5\n@see ref/max/#maxs"]
.help.i.r[`mmax;"Moving maximums — Where\n@syntax x mmax y    mmax[x;y]\n@example q)3 mmax 2 7 1 3 5 2 8\n2 7 7 7 5 5 8\nq)3 mmax 0N -3 -2 0N 1 0  / initial null returns negative infinity\n-0W -3 -2 -2 1 1          / remaining nulls replaced by preceding max\n@see ref/max/#mmax"]
.help.i.r[`md5;"Message Digest hash — Where `x` is a string, returns as a bytestream its MD5 (Message-Digest algorithm 5) hash.\n@syntax md5 x    md5[x]\n@example q)md5 \"this is a not so secret message\"\n0x6cf192c1938b79012c323fa30e62787e\n@see ref/md5/"]
.help.i.r[`med;"Median — Where `x` is a numeric list returns its median.\n@syntax med x    med[x]\n@example q)med 10 34 23 123 5 56\n28.5\nq)select med price by sym from trade where date=2001.10.10,sym in`AAPL`LEH\n@see ref/med/"]
.help.i.r[`meta;"Metadata for a table — Where `x` is a\n@syntax meta x    meta[x]\n@example q)\\l trade.q\nq)show meta trade\nc    | t f a\n-----| -----\ntime | t\nsym  | s\n@see ref/meta/"]
.help.i.r[`min;"Minimum — Where `x` is a non-symbol sortable list, returns its minimum.\n@syntax min x     min[x]\n@example q)min 2 5 7 1 3\n1\nq)min \"genie\"\n\"e\"\nq)min 0N 5 0N 1 3                  / nulls are ignored\n1\n@see ref/min/#min"]
.help.i.r[`mins;"Minimums — Where `x` is a non-symbol sortable list, returns the running minimums of the prefixes.\n@syntax mins x     mins[x]\n@example q)mins 2 5 7 1 3\n2 2 2 1 1\nq)mins \"genie\"\n\"geeee\"\nq)mins 0N 5 0N 1 3         / initial nulls return infinity\n0W 5 5 1 1\n@see ref/min/#mins"]
.help.i.r[`mmin;"Moving minimums — Where `y` is a non-symbol sortable list and `x` is a\n@syntax x mmin y     mmin[x;y]\n@example q)3 mmin 0N -3 -2 1 -0W 0\n0N 0N 0N -3 -0W -0W\nq)3 mmin 0N -3 -2 1 0N -0W    / null is the minimum value\n0N 0N 0N -3 0N 0N\n@see ref/min/#mmin"]
.help.i.r[`mmu;"Matrix multiply, dot product — Where `x` and `y` are both float vectors or matrixes, returns their matrix- or dot-product.\n@syntax x mmu y    mmu[x;y]\nx$y        $[x;y]\n@example q)a:2 4#2 4 8 3 5 6 0 7f\nq)b:4 3#\"f\"$til 12\nq)a mmu b\n87 104 121\n81 99  117\n\n@see ref/mmu/"]
.help.i.r[`mod;"Modulus — Where `x` and `y` are numeric, returns the remainder of `x%y`.\n@syntax x mod y    mod[x;y]\n@example q)-3 -2 -1 0 1 2 3 4 mod 3\n0 1 2 0 1 2 0 1\n\nq)7 mod 2 3 4\n1 1 3\n\n@see ref/mod/"]
.help.i.r[`neg;"Negate — Returns the negation of boolean or numeric `x`.\n@syntax neg x    neg[x]\n@example q)neg -1 0 1 2\n1 0 -1 -2\n\nq)neg 01001b\n0 -1 0 0 -1i\n\n@see ref/neg/"]
.help.i.r[`next;"Next item/s in a list — Where `x` is a list, for each item in `x`, returns the next item.\n@syntax next x      next[x]\n@example q)next 2 3 5 7 11\n3 5 7 11 0N\nq)next (1 2;\"abc\";`ibm)\n\"abc\"\n`ibm\n`long$()\n@see ref/next/#next"]
.help.i.r[`prev;"Immediately preceding item/s in a list — Where `x` is a list, for each item, returns the previous item.\n@syntax prev x     prev[x]\n@example q)prev 2 3 5 7 11\n0N 2 3 5 7\nq)prev (1 2;\"abc\";`ibm)\n`long$()\n1 2\n\"abc\"\n@see ref/next/#prev"]
.help.i.r[`xprev;"Nearby items in a list — Where `x` is a long atom and `y` is a list, returns for each item of `y` the item `x` indices before it.\n@syntax x xprev y     xprev[x;y]\n@example q)2 xprev 2 7 5 3 11\n0N 0N 2 7 5\nq)-2 xprev 2 7 5 3 11\n5 3 11 0N 0N\nq)1 xprev \"abcde\"\n\" abcd\"\n@see ref/next/#xprev"]
.help.i.r[`not;"Not zero — Returns `0b` where `x` not equal to zero, and `1b` otherwise.\n@syntax not x    not[x]\n@example q)not -1 0 1 2\n0100b\n\nq)not \"abc\",\"c\"$0\n0001b\n\n@see ref/not/"]
.help.i.r[`null;"Is null — Returns `1b` where `x` is null.\n@syntax null x     null[x]\n@example q)null 0 0n 0w 1 0n\n01001b\n\nq)where all null ([] c1:`a`b`c; c2:0n 0n 0n; c3:10 0N 30)\n,`c2\n@see ref/null/"]
.help.i.r[`or;"Greater; logical OR — Returns the greater of the underlying values of `x` and `y`.\n@syntax x|y       |[x;y]\nx or y    or[x;y]\n@example q)2|3\n3\nq)1010b or 1100b  /logical OR with booleans\n1110b\nq)\"sat\"|\"cow\"\n\"sow\"\n@see ref/greater/"]
.help.i.r[`over;"The keywords `over` and `scan` are covers for the accumulating iterators, Over and Scan.\n@syntax v1 over x    over[v1;x]        v1 scan x    scan[v1;x]\n(vv)over x    over[vv;x]       (vv)scan x    scan[vv;x]\n@example q)n:(\"the \";(\"quick \";\"brown \";(\"fox \";\"jumps \";\"over \");\"the \");(\"lazy \";\"dog.\"))\nq)raze over n\n\"the quick brown fox jumps over the lazy dog.\"\nq)(,/)over n\n\"the quick brown fox jumps over the lazy dog.\"\nq){x*x} scan .01\n@see ref/over/"]
.help.i.r[`scan;"The keywords `over` and `scan` are covers for the accumulating iterators, Over and Scan.\n@syntax v1 over x    over[v1;x]        v1 scan x    scan[v1;x]\n(vv)over x    over[vv;x]       (vv)scan x    scan[vv;x]\n@example q)n:(\"the \";(\"quick \";\"brown \";(\"fox \";\"jumps \";\"over \");\"the \");(\"lazy \";\"dog.\"))\nq)raze over n\n\"the quick brown fox jumps over the lazy dog.\"\nq)(,/)over n\n\"the quick brown fox jumps over the lazy dog.\"\nq){x*x} scan .01\n@see ref/over/"]
.help.i.r[`parse;"Parse a string.\n@example parse \"{x+42} each til 10\"\n@see ref/parse/"]
.help.i.r[`pj;"Plus join — Where\n@syntax x pj y     pj[x;y]\n@see ref/pj/"]
.help.i.r[`prd;"Product — Where `x` is a numeric list, returns its product.\n@syntax prd x    prd[x]\n@example q)prd 7                    / product of atom (returned unchanged)\n7\nq)prd 2 3 5 7              / product of list\n210\nq)prd 2 3 0N 7             / 0N is treated as 1\n42\n@see ref/prd/#prd"]
.help.i.r[`prds;"Products — Where `x` is a numeric list, returns the cumulative products of its items.\n@syntax prds x    prds[x]\n@example q)prds 7                     / atom is returned unchanged\n7\nq)prds 2 3 5 7               / cumulative products of list\n2 6 30 210\nq)prds 2 3 0N 7              / 0N is treated as 1\n2 6 6 42\n@see ref/prd/#prds"]
.help.i.r[`prior;"Is a wrapper for the Each Prior iterator.\n@syntax v2 prior x      prior[v2;x]\n(vv)prior x      prior[vv;x]\n@example q)(+) prior til 10\n0 1 3 5 7 9 11 13 15 17\nq){x+y%10}prior til 10\n0n 1 2.1 3.2 4.3 5.4 6.5 7.6 8.7 9.8\n@see ref/prior/"]
.help.i.r[`rand;"Pick randomly — Where `x` is a list returns one item chosen randomly from `x`\n@syntax rand x   rand[x]\n@example q)rand 1 30 45 32\n32\nq)rand(\"abc\";\"def\";\"ghi\")  / list of lists\n\"ghi\"\n@see ref/rand/"]
.help.i.r[`rank;"Position in the sorted list — Where `x` is a list or dictionary, returns for each item in `x` the index of where it would occur in the sorted list or dictionary.\n@syntax rank x    rank[x]\n@example q)rank 2 7 3 2 5\n0 4 2 1 3\nq)iasc 2 7 3 2 5\n0 3 2 4 1\nq)iasc iasc 2 7 3 2 5            / same as rank\n0 4 2 1 3\n@see ref/rank/"]
.help.i.r[`ratios;"Ratios between items — Where `y` is a non-symbolic sortable list, returns the ratios of the underlying values of consecutive pairs of items of `y`.\n@syntax ratios y     ratios[y]\n@example update ret:ratios price by sym from trade\nselect log ratios price from trade\n@see ref/ratios/"]
.help.i.r[`raze;"Return the items of `x` joined, collapsing one level of nesting — To collapse all levels, use Converge i.e.\n@syntax raze x    raze[x]\n@example q)raze (1 2;3 4 5)\n1 2 3 4 5\nq)b:(1 2;(3 4;5 6);7;8)\nq)raze b                 / flatten one level\n1\n2\n@see ref/raze/"]
.help.i.r[`read0;"Read text from a file or process handle — where\n@syntax read0 f           read0[f]\nread0 (f;o)       read0[(f;o)]\nread0 (f;o;n)     read0[(f;o;n)]\nread0 h           read0[h]\nread0 (fifo;n)    read0[(fifo;n)]\n@example q)`:test.txt 0:(\"hello\";\"goodbye\")  / write some text to a file\nq)read0`:test.txt\n\"hello\"\n\"goodbye\"\n\nq)/ Read 500000 lines, chunks of (up to) 100000 at a time\n@see ref/read0/"]
.help.i.r[`read1;"Read bytes from a file or named pipe — Where\n@syntax read1 f           read1[f]\nread1 (f;o)       read1[(f;o)]\nread1 (f;o;n)     read1[(f;o;n)]\nread1 h           read1[h]\nread1 (fifo;n)    read1[(fifo;n)]\n@example q)`:test.txt 0:(\"hello\";\"goodbye\")      / write some text to a file\nq)read1`:test.txt                       / read in as bytes\n0x68656c6c6f0a676f6f646279650a\nq)\"c\"$read1`:test.txt                   / convert from bytes to char\n\"hello\ngoodbye\n\"\n\n@see ref/read1/"]
.help.i.r[`reciprocal;"Reciprocal of a number — Returns the reciprocal of numeric `x` as a float.\n@syntax reciprocal x    reciprocal[x]\n@example q)reciprocal 0 0w 0n 3 10\n0w 0 0n 0.3333333 0.1\nq)reciprocal 1b\n1f\n@see ref/reciprocal/"]
.help.i.r[`reverse;"Reverse the order of items of a list or dictionary — Returns the items of `x` in reverse order.\n@syntax reverse x    reverse[x]\n@example q)reverse 1 2 3 4\n4 3 2 1\n@see ref/reverse/"]
.help.i.r[`rotate;"Shift the items of a list to the left or right — Where\n@syntax x rotate y    rotate[x;y]\n@example q)2 rotate 2 3 5 7 11    / rotate a list\n5 7 11 2 3\nq)-2 rotate 2 3 5 7 11\n7 11 2 3 5\nq)t:([]a:1 2 3;b:\"xyz\")\nq)1 rotate t             / rotate a table\n@see ref/rotate/"]
.help.i.r[`save;"Write a global variable to file and optionally format data — Where `x` is a symbol atom or vector of the form `[path/to/]v[.ext]` in which\n@syntax save x     save[x]\n@example q)t:([]x:2 3 5; y:`ibm`amd`intel; z:\"npn\")\n\nq)save `t            / binary\n`:t\nq)read0 `:t\n\"\\377\\001b\\000c\\013\\000\\003\\000\\000\\000x\\000y\\000z\\000\\000\\..\n@see ref/save/#save"]
.help.i.r[`rsave;"Write a table splayed to a directory — Where `x` is a table name as a symbol atom, saves the table, in binary format, splayed to a directory of the same name.\n@syntax rsave x     rsave[x]\n@example q)\\l sp.q\nq)rsave `sp           / save splayed table\n`:sp/\nq)\\ls sp\n,\"p\"\n\"qty\"\n@see ref/save/#rsave"]
.help.i.r[`select;"Select all or part of a table, possibly with new columns — For the Select operator `?`, see\n@syntax select columns by groups from table where filters\n@example q)tbl:([] id:1 1 2 2 2;val:100 200 300 400 500)\nq)select from tbl\nid val\n------\n1  100\n1  200\n@see ref/select/"]
.help.i.r[`from;"Select all or part of a table.\n@see ref/../basics/qsql/#from-phrase"]
.help.i.r[`show;"Format and display at the console. — Formats `x` and writes it to the console; returns the identity function `(::)`.\n@syntax show x    show[x]\n@example q)a:show til 5\n0 1 2 3 4\nq)a~(::)\n1b\n@see ref/show/"]
.help.i.r[`signum;"Where `x` (or its underlying value for temporals) is\n@syntax signum x    signum[x]\n@example q)signum -2 0 1 3\n-1 0 1 1i\n\nq)signum (0n;0N;0Nt;0Nd;0Nz;0Nu;0Nv;0Nm;0Nh;0Nj;0Ne)\n-1 -1 -1 -1 -1 -1 -1 -1 -1 -1 -1i\n\n@see ref/signum/"]
.help.i.r[`sin;"Sine, arcsine — Where `x` is a numeric, returns\n@syntax sin x     sin[x]\nasin x    asin[x]\n@example q)sin 0.5       / sine\n0.4794255\nq)sin 1%0\n0n\n\nq)asin 0.8      / arcsine\n@see ref/sin/"]
.help.i.r[`asin;"Sine, arcsine — Where `x` is a numeric, returns\n@syntax sin x     sin[x]\nasin x    asin[x]\n@example q)sin 0.5       / sine\n0.4794255\nq)sin 1%0\n0n\n\nq)asin 0.8      / arcsine\n@see ref/sin/"]
.help.i.r[`sqrt;"Square root — Returns as a float where `x` is numeric and\n@syntax sqrt x    sqrt[x]\n@example q)sqrt -1 0n 0 25 50\n0n 0n 0 5 7.071068\n\nq)sqrt 12:00:00.000000000\n6572671f\n\n@see ref/sqrt/"]
.help.i.r[`ss;"String search; y is a pattern.\n@syntax x ss y     ss[x;y]\n@example q)\"We the people of the United States\" ss \"the\"\n3 17\n\nq)s:\"toronto ontario\"\nq)s ss \"ont\"\n3 8\n@see ref/ss/#ss"]
.help.i.r[`ssr;"String search and replace;y is a pattern;z is a string or a function.\n@syntax ssr[x;y;z]\n@example q)s:\"toronto ontario\"\nq)ssr[s;\"ont\";\"x\"]      / replace \"ont\" by \"x\"\n\"torxo xario\"\nq)ssr[s;\"t?r\";upper]    / replace matches by their uppercase\n\"TORonto onTARio\"\n@see ref/ss/#ssr"]
.help.i.r[`string;"Cast to string — Returns `x` as a string.\n@syntax string x    string[x]\n@example q)string `ibm\n\"ibm\"\nq)string 2\n,\"2\"\nq)string {x*x}\n\"{x*x}\"\n@see ref/string/"]
.help.i.r[`sublist;"Select a sublist of a list — Where\n@syntax x sublist y    sublist[x;y]\n@example q)p:2 3 5 7 11\nq)3 sublist p                           / 3 from the front\n2 3 5\nq)10 sublist p                          / only available values\n2 3 5 7 11\nq)2 sublist `a`b`c!(1 2 3;\"xyz\";2 3 5)  / 2 keys from a dictionary\n@see ref/sublist/"]
.help.i.r[`sum;"Total — Where `x` is\n@syntax sum x    sum[x]\n@example q)sum 7                         / sum atom (returned unchanged)\n7\nq)sum 2 3 5 7                   / sum list\n17\nq)sum 2 3 0N 7                  / 0N is treated as 0\n12\n@see ref/sum/#sum"]
.help.i.r[`sums;"Running totals — Where `x` is a numeric or temporal list, returns the cumulative sums of the items of `x`.\n@syntax sums x    sums[x]\n@example q)sums 7                        / cumulative sum atom (returned unchanged)\n7\nq)sums 2 3 5 7                  / cumulative sum list\n2 5 10 17\nq)sums 2 3 0N 7                 / 0N is treated as 0\n2 5 5 12\n@see ref/sum/#sums"]
.help.i.r[`msum;"Moving sums — Where\n@syntax x msum y    msum[x;y]\n@example q)3 msum 1 2 3 5 7 11\n1 3 6 10 15 23\nq)3 msum 0N 2 3 5 0N 11     / nulls treated as zero\n0 2 5 10 8 16\n@see ref/sum/#msum"]
.help.i.r[`wsum;"Weighted sum — Where `x` and `y` are numeric lists, returns the weighted sum of the products of `x` and `y`.\n@syntax x wsum y    wsum[x;y]\n@example q)2 3 4 wsum 1 2 4   / equivalent to sum 2 3 4 * 1 2 4f\n24f\n\nq)2 wsum 1 2 4       / equivalent to sum 2 * 1 2 4\n14\n\n@see ref/sum/#wsum"]
.help.i.r[`sv;"Scalar from vector;y is a list;x is a char atom.\n@syntax x sv y    sv[x;y]\n@example q)\",\" sv (\"one\";\"two\";\"three\")    / comma-separated\n\"one,two,three\"\nq)\"\\t\" sv (\"one\";\"two\";\"three\")   / tab-separated\n\"one\\ttwo\\tthree\"\nq)\", \" sv (\"one\";\"two\";\"three\")   / x may be a string\n\"one, two, three\"\n@see ref/sv/"]
.help.i.r[`system;"Execute a system command — Where `x` is a string representing a kdb+ system command or operating system shell command, and any parameters to it.\n@syntax system x     system[x]\n@see ref/system/"]
.help.i.r[`tables;"List of tables in a namespace — Where `x` is a reference to a namespace, returns as a symbol vector a sorted list of the tables in `x`\n@syntax tables x    tables[x]\n@example q)\\l sp.q\nq)tables `.       / tables in root namespace\n`p`s`sp\nq)tables[]        / default is root namespace\n`p`s`sp\nq).work.tab:sp    / assign table in work namespace\n@see ref/tables/"]
.help.i.r[`tan;"Tangent and arctangent — Where `x` is a numeric, returns\n@syntax tan x     tan[x]\natan x    atan[x]\n@example q)tan 0 0.5 1 1.5707963 2 0w                    / tangent\n0 0.5463025 1.557408 3.732054e+07 -2.18504 0n\n\nq)atan 0.5                                      / arctangent\n0.4636476\nq)atan 42\n@see ref/tan/"]
.help.i.r[`atan;"Tangent and arctangent — Where `x` is a numeric, returns\n@syntax tan x     tan[x]\natan x    atan[x]\n@example q)tan 0 0.5 1 1.5707963 2 0w                    / tangent\n0 0.5463025 1.557408 3.732054e+07 -2.18504 0n\n\nq)atan 0.5                                      / arctangent\n0.4636476\nq)atan 42\n@see ref/tan/"]
.help.i.r[`til;"First x natural numbers — Where `x` is a non-negative integer atom, returns a vector of the first `x` integers.\n@syntax til x    til[x]\n@example q)til 0\n`long$()\nq)til 1b\n,0\nq)til 5\n0 1 2 3 4\n@see ref/til/"]
.help.i.r[`trim;"Remove leading or trailing nulls from a list — Where `x` is a vector or non-null atom, returns `x` without leading (`ltrim`) or trailing (`rtrim`) nulls or without either (`trim`).\n@syntax trim x     trim[x]\nltrim x    ltrim[x]\nrtrim x    rtrim[x]\n@example q)trim \"   IBM   \"\n\"IBM\"\nq)trim 0N 0N 1 2 3 0N 0N  4 5 0N 0N\n1 2 3 0N 0N 4 5\n\nq)ltrim\"   IBM   \"\n@see ref/trim/"]
.help.i.r[`ltrim;"Remove leading or trailing nulls from a list — Where `x` is a vector or non-null atom, returns `x` without leading (`ltrim`) or trailing (`rtrim`) nulls or without either (`trim`).\n@syntax trim x     trim[x]\nltrim x    ltrim[x]\nrtrim x    rtrim[x]\n@example q)trim \"   IBM   \"\n\"IBM\"\nq)trim 0N 0N 1 2 3 0N 0N  4 5 0N 0N\n1 2 3 0N 0N 4 5\n\nq)ltrim\"   IBM   \"\n@see ref/trim/"]
.help.i.r[`rtrim;"Remove leading or trailing nulls from a list — Where `x` is a vector or non-null atom, returns `x` without leading (`ltrim`) or trailing (`rtrim`) nulls or without either (`trim`).\n@syntax trim x     trim[x]\nltrim x    ltrim[x]\nrtrim x    rtrim[x]\n@see ref/trim/"]
.help.i.r[`type;"Type of an object — Where `x` is any object, returns its type.\n@syntax type x    type[x]\n@example q)type 5                        / integer atom\n-7h\nq)type 2 3 5                    / integer vector\n7h\nq)type (2 3 5;\"hello\")          / general list\n0h\n@see ref/type/"]
.help.i.r[`uj;"Union join.\n@syntax x uj  y     uj [x;y]\nx ujf y     ujf[x;y]\n@see ref/uj/"]
.help.i.r[`ujf;"Union join.\n@syntax x uj  y     uj [x;y]\nx ujf y     ujf[x;y]\n@see ref/uj/"]
.help.i.r[`union;"Union of two lists — Where `x` and `y` are lists or atoms, returns a list of the distinct items of its combined arguments, i.e.\n@syntax x union y    union[x;y]\n@example q)1 2 3 3 6 union 2 4 6 8\n1 2 3 6 4 8\nq)distinct 1 2 3 3 6, 2 4 6 8      / same as distinct on join\n1 2 3 6 4 8\n\nq)t0:([]x:2 3 5;y:\"abc\")\n@see ref/union/"]
.help.i.r[`ungroup;"Where `x` is a table, in which some cells are lists, but for any row, all lists are of the same length, returns the normalized table, with one row for each item of a lists.\n@syntax ungroup x    ungroup[x]\n@see ref/ungroup/"]
.help.i.r[`update;"Add or amend rows or columns of a table or entries in a dictionary — For the Update operator `!`, see\n@syntax update col by c2 from t where filter\n@example q)update x:0 from get`:mysplay\n@see ref/update/"]
.help.i.r[`upsert;"Overwrite or append records to a table — Where\n@syntax x upsert y    upsert[x;y]\n@example q)t:([]name:`tom`dick`harry;age:28 29 30;sex:`M)\n\nq)t upsert (`dick;49;`M)\nname  age sex\n-------------\ntom   28  M\n@see ref/upsert/"]
.help.i.r[`value;"Recurse the interpreter.\n@syntax value x     value[x]\n@example q)value `q`w`e!(1 2;3 4;5 6)        / dictionary\n1 2\n3 4\n5 6\n\nq)a:1 2 3\n@see ref/value/"]
.help.i.r[`var;"Variance — Where `x` is a numeric list, returns its variance as a float atom.\n@syntax var x    var[x]\n@example q)var 2 3 5 7\n3.6875\nq)var 2 3 5 0n 7\n3.6875\nq)select var price by sym from trade where date=2010.10.10,sym in`IBM`MSFT\n@see ref/var/#var"]
.help.i.r[`svar;"Sample variance — Where `x` is a numeric list, returns its sample variance as a float atom.\n@syntax svar x    svar[x]\n@example q)var 2 3 5 7\n3.6875\nq)svar 2 3 5 7\n4.916667\nq)select svar price by sym from trade where date=2010.10.10,sym in`IBM`MSFT\n@see ref/var/#svar"]
.help.i.r[`view;"Expression defining a view — Where `x` is a view (by reference), returns the expression defining `x`.\n@syntax view x    view[x]\n@example q)v::2+a*3                        / define dependency v\nq)a:5\nq)v\n17\nq)view `v                         / view the dependency expression\n\"2+a*3\"\n@see ref/view/#view"]
.help.i.r[`views;"List views defined in the default namespace — Returns a sorted list of the views currently defined in the default namespace.\n@syntax views[]\n@example q)w::b*10\nq)v::2+a*3\nq)views[]\n`s#`v`w\n@see ref/view/#views"]
.help.i.r[`vs;"“Vector from scalar” — Where `x` is a char atom or string, and `y` is a string, returns a list of strings: `y` cut using `x` as the delimiter.\n@syntax x vs y    vs[x;y]\n@example q)\",\" vs \"one,two,three\"\n\"one\"\n\"two\"\n\"three\"\nq)\", \" vs \"spring, summer, autumn, winter\"\n\"spring\"\n@see ref/vs/"]
.help.i.r[`where;"Copies of indexes of a list or keys of a dictionary.\n@syntax where x    where[x]\n@example q)where 2 3 0 1\n0 0 1 1 1 3\nq)raze x #' til count x:2 3 0 1\n0 0 1 1 1 3\n@see ref/where/"]
.help.i.r[`within;"Check bounds — Where\n@syntax x within y    within[x;y]\n@example q)1 3 10 6 4 within 2 6\n01011b\nq)\"acyxmpu\" within \"br\"  / chars are ordered\n0100110b\nq)select sym from ([]sym:`dd`ccc`ccc) where sym within `c`d\nsym\n@see ref/within/"]
.help.i.r[`wj;"Window join — Where\n@syntax wj [w; c; t; (q; (f0;c0); (f1;c1))]\nwj1[w; c; t; (q; (f0;c0); (f1;c1))]\n@example wj[w;`sym`time;trade;(quote;(max;`ask);(min;`bid))]\n@see ref/wj/"]
.help.i.r[`wj1;"Window join — Where\n@syntax wj [w; c; t; (q; (f0;c0); (f1;c1))]\nwj1[w; c; t; (q; (f0;c0); (f1;c1))]\n@see ref/wj/"]
.help.i.r[`xbar;"Round down — Where\n@syntax x xbar y    xbar[x;y]\n@example q)3 xbar til 16\n0 0 0 3 3 3 6 6 6 9 9 9 12 12 12 15\nq)2.5 xbar til 16\n0 0 0 2.5 2.5 5 5 5 7.5 7.5 10 10 10 12.5 12.5 15\nq)5 xbar 11:00 + 0 2 3 5 7 11 13\n11:00 11:00 11:00 11:05 11:05 11:10 11:10\n@see ref/xbar/"]
.help.i.r[`xgroup;"Groups a table by values in selected columns — Where\n@syntax x xgroup y    xgroup[x;y]\n@example q)`a`b xgroup ([]a:0 0 1 1 2;b:`a`a`c`d`e;c:til 5)\na b| c\n---| ---\n0 a| 0 1\n1 c| ,2\n1 d| ,3\n@see ref/xgroup/"]
.help.i.r[`xrank;"Group by value — Where\n@syntax x xrank y     xrank[x;y]\n@example q)4 xrank til 8          / equal size buckets\n0 0 1 1 2 2 3 3\nq)4 xrank til 9          / 1 bucket size differs\n0 0 0 1 1 2 2 3 3\nq)7 xrank til 9          / multiple bucket sizes differ\n0 0 1 2 3 3 4 5 6\n@see ref/xrank/"]
.help.i.r[`do;"Evaluate expression/s some number of times — Control construct.\n@syntax do[count;e1;e2;e3;…;en]\n@example q)r:()\nq)t:2*asin 1\nq)do[7;r,:q:floor t;t:reciprocal t-q]\nq)r\n3 7 15 1 292 1 1\n@see ref/do/"]
.help.i.r[`if;"Evaluate expression/s under some condition — Control construct.\n@syntax if[test;e1;e2;e3;…;en]\n@example q)a:100\nq)r:\"\"\nq)if[a>10;a:20;r:\"true\"]\nq)a\n20\nq)r\n@see ref/if/"]
.help.i.r[`while;"Evaluate expression/s while some condition remains true — Control construct.\n@syntax while[test;e1;e2;e3;…;en]\n@example q)r:1 1\nq)x:10\nq)while[x-:1;r,:sum -2#r]\nq)r\n1 1 2 3 5 8 13 21 34 55 89\n@see ref/while/"]
.help.i.r[`$"\\a";"List tables — Lists tables in namespace `ns` – defaults to current namespace.\n@syntax \\a\n\\a ns\n@example q)\\a\n`symbol$()\nq)aa:bb:23\nq)\\a\n`symbol$()\nq)tt:([]dd:12 34)\n@see basics/syscmds/#a-tables"]
.help.i.r[`$"\\b";"List dependencies — Lists dependencies (views) in namespace `ns` – defaults to current namespace.\n@syntax \\b\n\\b ns\n@example q)a::x+y\nq)b::x+1\nq)\\b\n`s#`a`b\n@see basics/syscmds/#b-views"]
.help.i.r[`$"\\B";"List pending dependencies — Lists pending dependencies (views) in namespace `ns`, i.e.\n@syntax \\B\n\\B ns\n@example q)a::x+1          / a depends on x\nq)\\B              / the dependency is pending\n,`a\nq)x:10\nq)\\B              / still pending after x is defined\n,`a\n@see basics/syscmds/#b-pending-views"]
.help.i.r[`$"\\c";"Console maximum rows and columns — Where `size` is a pair of integers: rows and columns, these values determine when q truncates output with `..`.\n@syntax \\c\n\\c size\n@example q)\\c\n45 160\nq)\\c 5 5\nq)\\c\n10 10\nq)til each 20+til 10\n@see basics/syscmds/#c-console-size"]
.help.i.r[`$"\\C";"HTTP display maximum rows and columns — Where `size` is a pair of integers: rows and columns, the values determine when q truncates output with `..`.\n@syntax \\C\n\\C size\n@see basics/syscmds/#c-http-size"]
.help.i.r[`$"\\cd";"Current directory — Where `fp` is a filepath, sets the current directory.\n@syntax \\cd\n\\cd fp\n@example q)\\cd\n\"/home/guest/q\"\nq)\\cd /home/guest/dev\nq)\\cd\n\"/home/guest/dev\"\nq)\\pwd\n@see basics/syscmds/#cd-change-directory"]
.help.i.r[`$"\\d";"Current namespace — Where `ns` is the name of a namespace, shows or sets the current namespace, also known as directory or context.\n@syntax \\d\n\\d ns\n@example q)\\d                  / default namespace\n`.\nq)\\d .o               / change to .o\nq.o)\\f\n`Cols`Columns`FG`Fkey`Gkey`Key`Special..\nq.o)\\d .              / return to default\n@see basics/syscmds/#d-directory"]
.help.i.r[`$"\\e";"Error trapping — Governs error trapping for client requests.\n@syntax \\e\n\\e mode\n@see basics/syscmds/#e-error-trap-clients"]
.help.i.r[`$"\\E";"TLS server mode.\n@syntax \\E\n@see basics/syscmds/#e-tls-server-mode"]
.help.i.r[`$"\\f";"List functions — Where `ns` is the name of a namespace, lists functions in it; defaults to current namespace.\n@syntax \\f\n\\f ns\n@example q)f:g:h:{x+2*y}\nq)\\f\n`f`g`h\nq)\\f .h\n`cd`code`data`eb`ec`ed`es`estr`fram`ha`hb`hc`he`hn`hp`hr`ht`hta`htac`htc`html`http`hu`hu..\nq){x where x like\"ht??\"}system\"f .h\"\n@see basics/syscmds/#f-functions"]
.help.i.r[`$"\\g";"Show or set garbage-collection mode.\n@syntax \\g            / current garbage-collection mode\n\\g mode       / set garbage-collection mode\n@see basics/syscmds/#g-garbage-collection-mode"]
.help.i.r[`$"\\l";"Where `name` is the name of a\n@syntax \\l name\n\\l .\n@example q)\\l sp.q            / load sp.q script\n...\nq)\\a                 / tables defined in sp.q\n`p`s`sp\nq)\\l db/tickdata     / load the data found in db/tickdata\nq)\\a                 / with tables quote and trade\n@see basics/syscmds/#l-load-file-or-directory"]
.help.i.r[`$"\\o";"Show or set the local time offset, as integer `n` hours from UTC, or as minutes if `abs[n]>23`.\n@syntax \\o\n\\o n\n@example q)\\o\n0N\nq).z.p                        / UTC\n2010.05.31D23:45:52.086467000\nq).z.P                        / local time is UTC + 8\n2010.06.01D07:45:53.830469000\n@see basics/syscmds/#o-offset-from-utc"]
.help.i.r[`$"\\p";"][hostname:][portnumber|servicename] listening port.\n@syntax \\p [rp,][hostname:][portnumber|servicename]\n@see basics/syscmds/#p-listening-port"]
.help.i.r[`$"\\P";"Show or set display precision for floating-point numbers, i.e.\n@syntax \\P\n\\P n\n@example q)\\P                       / default\n7i\nq)reciprocal 7             / 7 digits shown\n0.1428571\nq)123456789                / integers shown in full\n123456789\n@see basics/syscmds/#p-precision"]
.help.i.r[`$"\\r";"This should not be executed manually otherwise it can disrupt replication.\n@syntax \\r\n@see basics/syscmds/#r-replication-primary"]
.help.i.r[`$"\\s";"Show or , where `N` is an integer, set the number of secondary threads available for parallel processing, within the limit set by the `-s` command-line option.\n@syntax \\s\n\\s N\n@see basics/syscmds/#s-number-of-secondary-threads"]
.help.i.r[`$"\\S";"random seed.\n@syntax \\S\n\\S n\n@example q)\\S                       / default\n-314159i\nq)5?10\n8 1 9 5 4\nq)5?10\n6 6 1 8 5\n@see basics/syscmds/#s-random-seed"]
.help.i.r[`$"\\t";"This command has two different uses, according to the parameter.\n@syntax \\t         / show timer interval\n\\t N       / set timer interval\n\\t exp     / time expression\n\\t:n exp   / time n repetitions of expression\n@example q)/Show or set timer ticks\nq)\\t                           / default off\n0\nq).z.ts:{show`second$.z.N}\nq)\\t 1000                      / tick each second\nq)13:12:52\n@see basics/syscmds/#t-timer"]
.help.i.r[`$"\\T";"Show or set the client execution timeout, as `n` (integer) number of seconds a client call will execute before timing out.\n@syntax \\T\n\\T n\n@see basics/syscmds/#t-timeout"]
.help.i.r[`$"\\ts";"Executes the expression `exp` and shows the execution time in milliseconds and the space used in bytes.\n@syntax \\ts exp\n\\ts:n exp\n@example q)\\ts log til 100000\n7 2621568\n\nq)\\ts:10000 log til 1000           /same as \\ts do[10000; log til 1000]\n329 24672\n@see basics/syscmds/#ts-time-and-space"]
.help.i.r[`$"\\u";"When q is invoked with the `-u` parameter specifying a user password file, then `\\u` will reload the password file.\n@syntax \\u\n@see basics/syscmds/#u-reload-user-password-file"]
.help.i.r[`$"\\v";"Lists the variables in namespace `ns`; defaults to current namespace.\n@syntax \\v\n\\v ns\n@example q)a:1+b:2\nq)\\v\n`a`b\nq)\\v .h\n`HOME`br`c0`c1`logo`sa`sb`sc`tx`ty\nq){x where x like\"????\"}system\"v .h\"\n@see basics/syscmds/#v-variables"]
.help.i.r[`$"\\w";"With no parameter, returns current memory usage, as a list of 6 long integers.\n@syntax \\w          / current memory usage\n\\w 0|1      / internalized symbols\n\\w n        / set workspace memory limit\n@example q)\\w\n168144 67108864 67108864 0 0 8589934592\n@see basics/syscmds/#w-workspace"]
.help.i.r[`$"\\W";"Show or set the start-of-week offset `n`, where 0 is Saturday.\n@syntax \\W\n\\W n\n@see basics/syscmds/#w-week-offset"]
.help.i.r[`$"\\x";"By default, callbacks like `.z.po` are not defined in the session.\n@syntax \\x .z.p*\n@example q).z.pi                       / default has no user defined function\n'.z.pi\nq).z.pi:{\">\",.Q.s value x}    / assign function\nq)2+3\n>5\nq)\\x .z.pi                    / restore default\n@see basics/syscmds/#x-expunge"]
.help.i.r[`$"\\z";"Show or set the format for `\"D\"$` date parsing.\n@syntax \\z\n\\z 0|1\n@example q)\\z\n0\nq)\"D\"$\"06/01/2010\"\n2010.06.01\nq)\\z 1\nq)\"D\"$\"06/01/2010\"\n@see basics/syscmds/#z-date-parsing"]
.help.i.r[`$"\\1";"`\\1` and `\\2` let you redirect stdout and stderr to files from within the q session.\n@syntax \\1 filename\n\\2 filename\n@example q)\\1 t1.txt              / stdout\nq)\\2 t2.txt              / stderr\ntil 10\n2 + \"hello\"\n\\\\\n@see basics/syscmds/#1-2-redirect"]
.help.i.r[`$"\\2";"`\\1` and `\\2` let you redirect stdout and stderr to files from within the q session.\n@syntax \\1 filename\n\\2 filename\n@example q)\\1 t1.txt              / stdout\nq)\\2 t2.txt              / stderr\ntil 10\n2 + \"hello\"\n\\\\\n@see basics/syscmds/#1-2-redirect"]
.help.i.r[`$"\\_";"This command has two different uses depending on whether a parameter is given.\n@syntax \\_               / show client write access\n\\_ scriptname    / make runtime script\n@example q)\\_\n0b\n@see basics/syscmds/#_-hide-q-code"]
.help.i.r[`$"\\\\";"The text following `\\\\` and white space is ignored by q.\n@syntax \\\\\n@see basics/syscmds/#quit"]
.help.i.r[`.Q.A;"uppercase alphabet.\n@see ref/dotq/#qa-upper-case-alphabet"]
.help.i.r[`.Q.a;"lowercase alphabet.\n@see ref/dotq/#qa-lower-case-alphabet"]
.help.i.r[`.Q.an;"Strings: upper-case Roman alphabet (`.Q.A`), lower-case Roman alphabet (`.Q.a`), and all alphanums (`.Q.an`).\n@syntax .Q.A       / upper-case alphabet\n.Q.a       / lower-case alphabet\n.Q.an      / all alphanumerics\n@example q).Q.A\n\"ABCDEFGHIJKLMNOPQRSTUVWXYZ\"\nq).Q.a\n\"abcdefghijklmnopqrstuvwxyz\"\nq).Q.an\n\"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789\"\n@see ref/dotq/#qan-all-alphanumerics"]
.help.i.r[`.Q.addmonths;"Where `x` is a date and `y` is an int, returns `x` plus `y` months.\n@syntax .Q.addmonths[x;y]\n@example q).Q.addmonths[2007.10.16;6 7]\n2008.04.16 2008.05.16\n@see ref/dotq/#qaddmonths"]
.help.i.r[`.Q.addr;"Where `x` is a hostname or IP address as a symbol atom, returns the IP address as an integer.\n@syntax .Q.addr x\n@example q).Q.addr`$\"127.0.0.1\"\n2130706433i\n@see ref/dotq/#qaddr-ip-address"]
.help.i.r[`.Q.b6;"Returns upper- and lower-case alphabet and numerics.\n@syntax .Q.b6\n@example q).Q.b6\n\"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/\"\n@see ref/dotq/#qb6-bicameral-alphanums"]
.help.i.r[`.Q.bt;"Dumps the backtrace to stdout at any point during execution or debug.\n@syntax .Q.bt[]\n@example q)f:{{.Q.bt[];x*2}x+1}\nq)f 4\n  [2]  f@:{.Q.bt[];x*2}\n           ^\n  [1]  f:{{.Q.bt[];x*2}x+1}\n          ^\n@see ref/dotq/#qbt-backtrace"]
.help.i.r[`.Q.btoa;"Encodes data in base64 format.\n@syntax .Q.btoa x\n@example q).Q.btoa\"Hello World!\"\n\"SGVsbG8gV29ybGQh\"\n@see ref/dotq/#qbtoa-b64-encode"]
.help.i.r[`.Q.bv;"In partitioned DBs, construct the dictionary `.Q.vp` of table schemas for tables with missing partitions.\n@syntax .Q.bv[]\n.Q.bv[`]\n@see ref/dotq/#qbv-build-vp"]
.help.i.r[`.Q.Cf;"Deprecated since 4.1t 2022.03.25.\n@syntax .Q.Cf x\n@see ref/dotq/#qcf-create-empty-nested-char-file"]
.help.i.r[`.Q.chk;"Where `x` is a HDB as a filepath, fills tables missing from partitions using the most recent partition containing the table as a template, and reports which partitions (but not which tables) it is fixing.\n@syntax .Q.chk x\n@example q).Q.chk[`:hdb]\n()\n()\n,`:/db/2009.01.04\n,`:/db/2009.01.03\n@see ref/dotq/#qchk-fill-hdb"]
.help.i.r[`.Q.cn;"Where `x` is a partitioned table, passed by value, returns its count.\n@syntax .Q.cn x\n@see ref/dotq/#qcn-count-partitioned-table"]
.help.i.r[`.Q.D;"In segmented DBs, contains a list of the partitions – conformant to `.Q.P` – that are present in each segment.\n@syntax .Q.D\n@example q).Q.P\n`:../segments/1`:../segments/2`:../segments/3`:../segments/4\nq).Q.D\n2010.05.26 2010.05.31\n,2010.05.27\n2010.05.28 2010.05.30\n@see ref/dotq/#qd-partitions"]
.help.i.r[`.Q.dd;"Shorthand for `` ` sv x,`$string y``.\n@syntax .Q.dd[x;y]\n@example q).Q.dd[`:dir]`file\n`:dir/file\nq){x .Q.dd'key x}`:dir\n`:dir/file1`:dir/file2\nq).Q.dd[`AAPL]\"O\"\n`AAPL.O\n@see ref/dotq/#qdd-join-symbols"]
.help.i.r[`.Q.def;"Default values and type checks for command-line arguments parsed with `.Q.opt` — Where `x` is a dictionary of default parameter names and values, and `y` is the output of `.Q.opt`.\n@syntax .Q.def[x;y]\n@example q).Q.def[`abc`xyz`efg!(1;2.;`a)].Q.opt .z.x\nabc| 123\nxyz| 321f\nefg| `a\n@see ref/dotq/#qdef-parse-options"]
.help.i.r[`.Q.dpft;"save table.\n@syntax .Q.dpft[directory;partition;`p#field;tablename]\n@see ref/dotq/#qdpft-save-table"]
.help.i.r[`.Q.dpfts;"save table with symtable.\n@see ref/dotq/#qdpft-save-table"]
.help.i.r[`.Q.dpt;"save table unsorted.\n@see ref/dotq/#qdpt-save-table-unsorted"]
.help.i.r[`.Q.dpts;"save table unsorted with symtable.\n@syntax .Q.dpft[d;p;f;t]\n.Q.dpfts[d;p;f;t;s]\n.Q.dpt[d;p;t]\n.Q.dpts[d;p;t;s]\n@see ref/dotq/#qdpts-save-table-unsorted-with-symtable"]
.help.i.r[`.Q.dsftg;"load process save.\n@syntax .Q.dsftg[d;s;f;t;g]\n@example q)d:(`:/dst/taq;2000.10.02;`trade)\nq)s:(`:/src/taq;19;0)  / nonpositive length from end\nq)f:`time`price`size`stop`corr`cond`ex\nq)t:(\"iiihhc c\";4 4 4 2 2 1 1 1)\nq)g:{x[`stop]=:240h;@[x;`price;%;1e4]}\nq).Q.dsftg[d;s;f;t;g]\n@see ref/dotq/#qdsftg-load-process-save"]
.help.i.r[`.Q.en;"enumerate varchar cols.\n@syntax .Q.en[`:db; table]\n@see ref/dotq/#qen-enumerate-varchar-cols"]
.help.i.r[`.Q.ens;"enumerate against domain).\n@syntax .Q.en[dir;table]\n.Q.ens[dir;table;name]\n@see ref/dotq/#qens-enumerate-against-domain"]
.help.i.r[`.Q.f;"y as a string formatted as a float to x decimal places.\n@syntax .Q.f[x;y]\n@example q)\\P 0\nq).Q.f[2;]each 9.996 34.3445 7817047037.90 781704703567.90 -.02 9.996 -0.0001\n\"10.00\"\n\"34.34\"\n\"7817047037.90\"\n\"781704703567.90\"\n@see ref/dotq/#qf-format"]
.help.i.r[`.Q.fc;"the result of evaluating f vec – using multiple threads if possible.\n@syntax .Q.fc[x;y]\n@see ref/dotq/#qfc-parallel-on-cut"]
.help.i.r[`.Q.ff;"append columns.\n@syntax .Q.ff[x;y]\n@see ref/dotq/#qff-append-columns"]
.help.i.r[`.Q.fk;"Where `x` is a table column, returns `` ` `` if the column is not a foreign key or `` `tab`` if the column is a foreign key into `tab`.\n@syntax .Q.fk x\n@see ref/dotq/#qfk-foreign-key"]
.help.i.r[`.Q.fmt;"z as a string of length x formatted to y decimal places.\n@syntax .Q.fmt[x;y;z]\n@example q).Q.fmt[6;2]each 1 234\n\"  1.00\"\n\"234.00\"\n@see ref/dotq/#qfmt-format"]
.help.i.r[`.Q.fpn;"streaming algorithm.\n@see ref/dotq/#qfpn-streaming-algorithm"]
.help.i.r[`.Q.fps;"`.Q.fs` for pipes — Where\n@syntax .Q.fps[x;y]\n.Q.fpn[x;y;z]\n@see ref/dotq/#qfps-streaming-algorithm"]
.help.i.r[`.Q.fs;"streaming algorithm.\n@see ref/dotq/#qfs-streaming-algorithm"]
.help.i.r[`.Q.fsn;"streaming algorithm.\n@syntax .Q.fs[x;y]\n.Q.fsn[x;y;z]\n@see ref/dotq/#qfsn-streaming-algorithm"]
.help.i.r[`.Q.ft;"apply simple.\n@syntax .Q.ft[x;y]\n@see ref/dotq/#qft-apply-simple"]
.help.i.r[`.Q.fu;"Where `x` is a unary function and `y` is\n@syntax .Q.fu[x;y]\n@example q)vec:100000 ? 30     / long vector with few different values\nq)f:{exp x*x}         / e raised to x*x\nq)\\t:1000 r1:f vec\n745\nq)\\t:1000 r2:.Q.fu[f;vec]\n271\n@see ref/dotq/#qfu-apply-unique"]
.help.i.r[`.Q.gc;"Run garbage-collection and returns the amount of memory that was returned to the OS.\n@syntax .Q.gc[]\n@see ref/dotq/#qgc-garbage-collect"]
.help.i.r[`.Q.gz;".Q.gz[::]- zlib loaded;.Q.gz cbv - unzipped; .Q.gz (cl;cbv) - zipped.\n@syntax .Q.gz[::]           / zlib loaded?\n.Q.gz cbv           / unzipped\n.Q.gz (cl;cbv)      / zipped\n@example q).Q.gz{0N!count x;x}[.Q.gz(9;10000#\"helloworld\")]\n66\n\"helloworldhelloworldhelloworldhelloworldhelloworldhelloworldhelloworldhellow..\n@see ref/dotq/#qgz-gzip"]
.help.i.r[`.Q.hdpf;"save tables.\n@syntax .Q.hdpf[historicalport;directory;partition;`p#field]\n@see ref/dotq/#qhdpf-save-tables"]
.help.i.r[`.Q.hg;"Where `x` is a URL as a symbol atom or (since V3.6 2018.02.10) a string, returns a string for the result of an HTTP[S] GET query.\n@syntax .Q.hg x\n@example q).Q.hg`:http://www.google.com\nq)count a:.Q.hg`:http:///www.google.com\n212\nq)show a\n\"<!DOCTYPE HTML PUBLIC \\\"-//IETF//DTD HTML 2.0//EN\\\">\n<html><head>\n<title>4..\nq).Q.hg \":http://username:password@www.google.com\"\n@see ref/dotq/#qhg-http-get"]
.help.i.r[`.Q.host;"Where `x` is an IP address as an int atom, returns its hostname as a symbol atom.\n@syntax .Q.host x\n@example q).Q.host 2130706433i\n`localhost\n@see ref/dotq/#qhost-hostname"]
.help.i.r[`.Q.hp;"HTTP post.\n@syntax .Q.hp[x;y;z]\n@example q).Q.hp[\"http://google.com\";.h.ty`json]\"my question\"\n\"<!DOCTYPE html>\n<html lang=en>\n  <meta charset=utf-8>\n  <meta name=viewpo..\n@see ref/dotq/#qhp-http-post"]
.help.i.r[`.Q.id;"sanitize.\n@syntax .Q.id x\n@example q).Q.id each `$(\"ab\";\"a/b\";\"two words\";\"2drifters\";\"2+2\")\n    `ab`ab`twowords`a2drifters`a22\n@see ref/dotq/#qid-sanitize"]
.help.i.r[`.Q.ind;"partitioned index.\n@syntax .Q.ind[x;y]\n@see ref/dotq/#qind-partitioned-index"]
.help.i.r[`.Q.j10;"encode binhex.\n@see ref/dotq/#qj10-encode-binhex"]
.help.i.r[`.Q.j12;"decode binhex.\n@see ref/dotq/#qj12-encode-base-36"]
.help.i.r[`.Q.x10;"encode base-36.\n@see ref/dotq/#qx10-decode-binhex"]
.help.i.r[`.Q.x12;"decode base-36.\n@syntax .Q.j10 s     .Q.j12 s\n.Q.x10 s     .Q.x12 s\n@see ref/dotq/#qx12-decode-base-36"]
.help.i.r[`.Q.K;"version date.\n@see ref/dotq/#qk-version-date"]
.help.i.r[`.Q.k;"Return the interpreter version date (`.Q.K`) and number (`.Q.k`) for which `q.k` has been written: checked against `.z.K` at startup.\n@syntax .Q.K      / version date\n.Q.k      / version\n@example q).Q.K\n2020.10.02\nq).Q.k\n4f\n@see ref/dotq/#qk-version"]
.help.i.r[`.Q.l;"Where `x` is a hsym or symbol atom naming a directory in the current directory, loads it recursively as in `load`, but into the default namespace.\n@syntax .Q.l x\n@see ref/dotq/#ql-load"]
.help.i.r[`.Q.M;"Chunk size for `dsftg` (load-process-save).\n@syntax .Q.M\n@example q)0W~.Q.M  / defaults to long infinity\n1b\n@see ref/dotq/#qm-long-infinity"]
.help.i.r[`.Q.MAP;"Keeps partitions mapped to avoid the overhead of repeated file system calls during a `select`.\n@syntax .Q.MAP[]\n@example q)\\l .\nq).Q.MAP[]\n@see ref/dotq/#qmap-maps-partitions"]
.help.i.r[`.Q.n;"nums.\n@see ref/dotq/#qn-nums"]
.help.i.r[`.Q.nA;"Strings: numerics (`.Q.n`) and upper-case alphabet and numerics (`.Q.nA`).\n@syntax .Q.n\n.Q.nA\n@example q).Q.n\n\"0123456789\"\nq).Q.nA\n\"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ\"\n@see ref/dotq/#qna-alphanums"]
.help.i.r[`.Q.opt;"Presents command-line arguments as a dictionary, using the output of `.z.x`.\n@syntax .Q.opt .z.x\n@example q)params:.Q.opt .z.x\nq)show params\nparam1| \"val1\"\nparam2| \"val2\"\nq)params`param1\n\"val1\"\n@see ref/dotq/#qopt-command-parameters"]
.help.i.r[`.Q.P;"In segmented DBs, returns a list of the segments (i.e.\n@syntax .Q.P\n@example q).Q.P\n`:../segments/1`:../segments/2`:../segments/3`:../segments/4\n@see ref/dotq/#qp-segments"]
.help.i.r[`.Q.par;"locate partition.\n@syntax .Q.par[dir;part;table]\n@example q).Q.par[`:.;2010.02.02;`quote]\n`:/data/taq/2010.02.02/quote\n@see ref/dotq/#qpar-locate-partition"]
.help.i.r[`.Q.PD;"In partitioned DBs, a list of partition locations – conformant to `.Q.PV` – which represents the partition location for each partition.\n@syntax .Q.PD\n@example q).Q.PV\n2010.05.26 2010.05.27 2010.05.28 2010.05.29 2010.05.30 2010.05.30 2010.05.31\nq).Q.PD\n`:../segments/1`:../segments/2`:../segments/3`:../segments/4`:../segments/3`:../segments/4`:../segments/1\nq).Q.PV!.Q.PD\n2010.05.26| :../segments/1\n@see ref/dotq/#qpd-partition-locations"]
.help.i.r[`.Q.pd;"In partitioned DBs, `.Q.PD` as modified by `.Q.view`.\n@syntax .Q.pd\n@see ref/dotq/#qpd-modified-partition-locations"]
.help.i.r[`.Q.pf;"In partitioned DBs, the partition field.\n@syntax .Q.pf\n@see ref/dotq/#qpf-partition-field"]
.help.i.r[`.Q.pn;"In partitioned DBs, returns a dictionary of cached partition counts – conformant to `.Q.pt`, each conformant to `.Q.pv` – as populated by `.Q.cn`.\n@syntax .Q.pn\n@example q)n:100\nq)t:([]time:.z.T+til n;sym:n?`2;num:n)\nq).Q.dpft[`:.;;`sym;`t]each 2010.01.01+til 5\n`t`t`t`t`t\nq)\\l .\nq).Q.pn\n@see ref/dotq/#qpn-partition-counts"]
.help.i.r[`.Q.prf0;"Where `pid` is a process ID, returns a table representing a snapshot of the call stack at the time of the call in another kdb+ process `pid`, with columns\n@syntax .Q.prf0 pid\n@see ref/dotq/#qprf0-code-profiler"]
.help.i.r[`.Q.pt;"Returns a list of partitioned tables.\n@syntax .Q.pt\n@see ref/dotq/#qpt-partitioned-tables"]
.help.i.r[`.Q.pv;"A list of the values of the partition domain: the values corresponding to the slice directories actually found in the root.\n@syntax .Q.pv\n@see ref/dotq/#qpv-modified-partition-values"]
.help.i.r[`.Q.PV;"In partitioned DBs, returns a list of partition values – conformant to `.Q.PD` – which represents the partition value for each partition.\n@syntax .Q.PV\n@example q).Q.PD\n`:../segments/1`:../segments/2`:../segments/3`:../segments/4`:../segments/3`:../segments/4`:../segments/1\nq).Q.PV\n2010.05.26 2010.05.27 2010.05.28 2010.05.29 2010.05.30 2010.05.30 2010.05.31\nq)date\n2010.05.26 2010.05.27 2010.05.28 2010.05.29 2010.05.30 2010.05.30 2010.05.31\n@see ref/dotq/#qpv-partition-values"]
.help.i.r[`.Q.qp;"is partitioned table.\n@syntax .Q.qp x\n@see ref/dotq/#qqp-is-partitioned"]
.help.i.r[`.Q.qt;"Where `x` is a table, returns `1b`, else `0b`.\n@syntax .Q.qt x\n@see ref/dotq/#qqt-is-table"]
.help.i.r[`.Q.res;"Returns the control words and keywords as a symbol vector.\n@syntax .Q.res\n@example q).Q.res,key`.q\n`abs`acos`asin`atan`avg`bin`binr`cor`cos`cov`delete`dev`div`do`enlist`exec`ex..\n@see ref/dotq/#qres-keywords"]
.help.i.r[`.Q.s;"Returns `x` formatted to plain text, as used by the console.\n@syntax .Q.s x\n@example q).Q.s ([h:1 2 3] m: 4 5 6)\n\"h| m\n-| -\n1| 4\n2| 5\n3| 6\n\"\n@see ref/dotq/#qs-plain-text"]
.help.i.r[`.Q.s1;"Returns a string representation of `x`.\n@syntax .Q.s1 x\n@see ref/dotq/#qs1-string-representation"]
.help.i.r[`.Q.sbt;"Where `x` is a backtrace object returns it as a string formatted for display.\n@syntax .Q.sbt x\n@see ref/dotq/#qsbt-string-backtrace"]
.help.i.r[`.Q.sha1;"Where `x` is a string, returns as a bytestream its SHA-1 hash.\n@syntax .Q.sha1 x\n@example q).Q.sha1\"Hello World!\"\n0x2ef7bde608ce5404e97d5f042f95f89f1c232871\n@see ref/dotq/#qsha1-sha-1-encode"]
.help.i.r[`.Q.t;"List of chars indexed by datatype numbers.\n@syntax .Q.t\n@example q).Q.t\n\" bg xhijefcspmdznuvts\"\nq).Q.t?\"j\"  / longs have datatype 7\n7\n@see ref/dotq/#qt-type-letters"]
.help.i.r[`.Q.trp;"extends Trap (@[f;x;g]) to collect backtrace.\n@syntax .Q.trp[f;x;g]\n@example q)f:{`hello+x}\nq)           / print the formatted backtrace and error string to stderr\nq).Q.trp[f;2;{2\"error: \",x,\"\nbacktrace:\n\",.Q.sbt y;-1}]\nerror: type\nbacktrace:\n  [2]  f:{`hello+x}\n@see ref/dotq/#qtrp-extend-trap"]
.help.i.r[`.Q.ts;"Apply with time and space.\n@syntax .Q.ts[x;y]\n@example q)\\ts .Q.hg `:http://www.google.com\n148 131760\nq).Q.ts[.Q.hg;enlist`:http://www.google.com]\n148 131760\n\"<!doctype html><html itemscope=\\\"\\\" itemtype=\\\"http://schema.org/WebPa\n\n@see ref/dotq/#qts-time-and-space"]
.help.i.r[`.Q.ty;"type.\n@syntax .Q.ty x\n@see ref/dotq/#qty-type"]
.help.i.r[`.Q.u;"date based.\n@see q/ref/dotq/#qu-date-based"]
.help.i.r[`.Q.V;"table to dict.\n@syntax .Q.V x\n@see ref/dotq/#qv-table-to-dict"]
.help.i.r[`.Q.v;"value.\n@syntax .Q.v x\n@see ref/dotq/#qv-value"]
.help.i.r[`.Q.view;"Where `x` is a list of partition values that serves as a filter for all queries against any partitioned table in the database, `x` is added as a constraint in the first sub-phrase of the where-clause of every query.\n@syntax .Q.view x\n@example .Q.view 2#date\n@see ref/dotq/#qview-subview"]
.help.i.r[`.Q.vp;"In partitioned DBs, returns a dictionary of table schemas for tables with missing partitions, as populated by `.Q.bv`.\n@syntax .Q.vp\n@see ref/dotq/#qvp-missing-partitions"]
.help.i.r[`.Q.w;"Returns the memory stats from `\\w` into a more readable dictionary.\n@syntax .Q.w[]\n@example q).Q.w[]\nused| 168304\nheap| 67108864\npeak| 67108864\nwmax| 0\nmmap| 0\n@see ref/dotq/#qw-memory-stats"]
.help.i.r[`.Q.Xf;"Deprecated since 4.1t 2022.03.25.\n@syntax .Q.Xf[x;y]\n@example q).Q.Xf[\"C\";`:emptyNestedCharVector];\nq)type get`:emptyNestedCharVector\n87h\n@see ref/dotq/#qxf-create-file"]
.help.i.r[`.Q.x;"Set by `.Q.opt`: a list of non-command parameters from the command line, where command parameters are prefixed by `-`.\n@syntax .Q.x\n@example q)cla:.Q.opt .z.X /command-line arguments\nq).Q.x\n\"/Users/me/q/m64/q\"\n\"path/to/source\"\n\"path/to/destn\"\n@see ref/dotq/#qx-non-command-parameters"]
.help.i.r[`.z.a;"The IP address as a 32-bit integer.\n@example q).z.a\n-1408172030i\n@see ref/dotz/#za-ip-address"]
.help.i.r[`.z.ac;"Lets you define custom code to authorize/authenticate an HTTP request.\n@syntax .z.ac:(requestText;requestHeaderAsDictionary)\n@see ref/dotz/#zac-http-auth-from-cookie"]
.help.i.r[`.z.b;"The dependency dictionary.\n@example q)a::x+y\nq)b::x+1\nq).z.b\nx| `a`b\ny| ,`a\n@see ref/dotz/#zb-dependencies"]
.help.i.r[`.z.c;"The number of physical cores.\n@see ref/dotz/#zc-cores"]
.help.i.r[`.z.d;"UTC date.\n@syntax .z.T  `time$.z.Z     .z.D  `date$.z.Z\n.z.t  `time$.z.z     .z.d  `date$.z.z\n@see ref/dotz/#zt-zt-zd-zd-timedate-shortcuts"]
.help.i.r[`.z.D;"local date.\n@syntax .z.T  `time$.z.Z     .z.D  `date$.z.Z\n.z.t  `time$.z.z     .z.d  `date$.z.z\n@see ref/dotz/#zt-zt-zd-zd-timedate-shortcuts"]
.help.i.r[`.z.e;"TLS details used with the current connection handle.\n@example q)h:hopen `:tcps://localhost:5000\nq)h\".z.e\"\nCIPHER  | `AES128-GCM-SHA256\nPROTOCOL| `TLSv1.2\nCERT    | `SUBJECT`ISSUER`SERIALNUMBER`NOTVALIDBEFORE`NOTVALIDAFTER`VERIFIED`VERIFYERROR!(\"/C=US/ST=New York/L=Brooklyn/O=Example Brooklyn Company/CN=myname.com\";\"/C=US/ST=New York/L=Brooklyn/O=Example Brooklyn Company/CN=examplebrooklyn.com\";,\"1\";\"Jul  6 10:08:57 2021 GMT\";\"May 15 10:08:57 2031 GMT\";1b;0)\n@see ref/dotz/#ze-tls-connection-status"]
.help.i.r[`.z.ex;"In a debugger session, `.z.ex` is set to the failed primitive.\n@see ref/dotz/#zex-failed-primitive"]
.help.i.r[`.z.ey;"In a debugger session, `.z.ey` is set to the argument to failed primitive.\n@see ref/dotz/#zey-argument-to-failed-primitive"]
.help.i.r[`.z.f;"Name of the q script as a symbol.\n@example $ q test.q\nq).z.f\n`test.q\n@see ref/dotz/#zf-file"]
.help.i.r[`.z.h;"The host name as a symbol.\n@example q).z.h\n`demo.kx.com\n@see ref/dotz/#zh-host"]
.help.i.r[`.z.i;"The process ID as an integer.\n@example q).z.i\n23219\n@see ref/dotz/#zi-pid"]
.help.i.r[`.z.K;"The major version number, as a float, of the version of kdb+ being used.\n@example q).z.K\n2.4\nq).z.k\n2006.10.30\n@see ref/dotz/#zk-version"]
.help.i.r[`.z.k;"Date on which the version of kdb+ being used was released.\n@example q).z.k\n2006.10.30\nq)\n@see ref/dotz/#zk-release-date"]
.help.i.r[`.z.l;"License information as a list of strings; `()` for non-commercial 32-bit versions.\n@example q)`maxCoresAllowed`expiryDate`updateDate`````bannerText`!.z.l\nmaxCoresAllowed| \"\"\nexpiryDate     | \"2021.05.27\"\nupdateDate     | \"2021.05.27\"\n               | ,\"1\"\n               | ,\"1\"\n@see ref/dotz/#zl-license"]
.help.i.r[`.z.N;"System local time as timespan in nanoseconds.\n@example q).z.N\n0D23:30:10.827156000\n@see ref/dotz/#zn-local-timespan"]
.help.i.r[`.z.n;"System UTC time as timespan in nanoseconds.\n@example q).z.n\n0D23:30:10.827156000\n@see ref/dotz/#zn-utc-timespan"]
.help.i.r[`.z.o;"kdb+ operating system version as a symbol.\n@example q).z.o\n`w32\n@see ref/dotz/#zo-os-version"]
.help.i.r[`.z.P;"System localtime timestamp in nanoseconds.\n@example q).z.P\n2018.04.30D10:18:31.932126000\n@see ref/dotz/#zp-local-timestamp"]
.help.i.r[`.z.p;"UTC timestamp in nanoseconds.\n@example q).z.p\n2018.04.30D09:18:38.117667000\n@see ref/dotz/#zp-utc-timestamp"]
.help.i.r[`.z.pm;"Where f is a unary function, .z.pm is evaluated when the following HTTP request methods are received in the kdb+ session.\n@syntax .z.pm:f\n@see ref/dotz/#zpm-http-options"]
.help.i.r[`.z.q;"`1b` if Quiet Mode is set, else `0b`.\n@see ref/dotz/#zq-quiet-mode"]
.help.i.r[`.z.s;"A reference to the current function.\n@example q){.z.s}[]\n{.z.s}\n@see ref/dotz/#zs-self"]
.help.i.r[`.z.T;"local time.\n@syntax .z.T  `time$.z.Z     .z.D  `date$.z.Z\n.z.t  `time$.z.z     .z.d  `date$.z.z\n@see ref/dotz/#zt-zt-zd-zd-timedate-shortcuts"]
.help.i.r[`.z.t;"UTC time.\n@syntax .z.T  `time$.z.Z     .z.D  `date$.z.Z\n.z.t  `time$.z.z     .z.d  `date$.z.z\n@see ref/dotz/#zt-zt-zd-zd-timedate-shortcuts"]
.help.i.r[`.z.u;"User ID, as a symbol, associated with the current handle.\n@example q).z.u\n`demo\n@see ref/dotz/#zu-user-id"]
.help.i.r[`.z.w;"Connection handle; 0 for current session console.\n@example q).z.w\n0i\n@see ref/dotz/#zw-handle"]
.help.i.r[`.z.W;"Dictionary of IPC handles with the number of bytes waiting in their output queues.\n@example q)h:hopen ...\nq)h\n3\nq)neg[h]({};til 1000000); neg[h]({};til 10); .z.W\n3| 8000030 110\nq)neg[h]({};til 1000000); neg[h]({};til 10); sum each .z.W\n@see ref/dotz/#zw-handles"]
.help.i.r[`.z.x;"Command-line arguments as a list of strings.\n@example $ q test.q -P 0 -abc 123\nq).z.x\n\"-abc\"\n\"123\"\n@see ref/dotz/#zx-argv"]
.help.i.r[`.z.X;"Returns a list of strings of the raw, unfiltered command line with which kdb+ was invoked, including the name under which q was invoked, as well as single-letter arguments.\n@syntax .z.X\n@example q).z.X\n,\"q\"\n\"somefile.q\"\n\"-customarg\"\n\"42\"\n\"-p\"\n@see ref/dotz/#zx-raw-command-line"]
.help.i.r[`.z.z;"UTC time as a datetime atom.\n@example q).z.z\n2006.11.13T21:16:14.601\n@see ref/dotz/#zz-utc-datetime"]
.help.i.r[`.z.Z;"Local time as a datetime atom.\n@example q).z.Z\n2006.11.13T21:16:14.601\n@see ref/dotz/#zz-local-datetime"]
.help.i.r[`.z.zd;"Integers `lbs`, `alg`, and `lvl` are compression parameters and/or encryption parameters.\n@syntax .z.zd:(lbs;alg;lvl)\n.z.zd:dict\n@example q).z.zd:17 2 4            / enable compression\nq)`:fileA set til 1000    / create file, now compressed as .z.zd set\n`:fileA\nq)`:fileB set til 1000    / create file, now compressed as .z.zd set\n`:fileB\nq)-21!`:fileA             / check that file is compressed\n@see ref/dotz/#zzd-zip-defaults"]
.help.i.r[`.z.bm;"Where `x` is a unary function.\n@syntax .z.bm:x\n@example q).z.bm:{`msg set (.z.p;x);}\n@see ref/dotz/#zbm-msg-validator"]
.help.i.r[`.z.exit;"Where `f` is a unary function, `f` is called with the exit parameter as the argument just before exiting the kdb+ session.\n@syntax .z.exit:f\n@example q).z.exit\n'.z.exit\nq).z.exit:{0N!x}\nq)\\\\\n0\nos>..\n@see ref/dotz/#zexit-action-on-exit"]
.help.i.r[`.z.pc;"Where `f` is a unary function, `.z.pc` is called after a connection has been closed.\n@syntax .z.pc:f\n@example q).z.pc\n'.z.pc\nq).z.pc:{0N!(.z.a;.z.u;.z.w;x);x}\nq)\\p 2021\nq)(2130706433i;`simon;0i;4i)\n\n@see ref/dotz/#zpc-close"]
.help.i.r[`.z.pd;"Where q has been started with secondary processes for use in parallel processing, `x` is\n@syntax .z.pd: x\n@example q)/ Open connections to 4 processes on the localhost\nq).z.pd:`u#hopen each 20000+til 4\n@see ref/dotz/#zpd-peach-handles"]
.help.i.r[`.z.pg;"Where `f` is a unary function, called with the object that is passed to the q session via a synchronous request.\n@syntax .z.pg:f\n@see ref/dotz/#zpg-get"]
.help.i.r[`.z.ph;"Where `f` is a unary function, it is evaluated when a synchronous HTTP request is received by the kdb+ session.\n@syntax .z.ph:f\n@see ref/dotz/#zph-http-get"]
.help.i.r[`.z.pi;"Where `f` is a unary function, it is evaluated as the default handler for input.\n@syntax .z.pi:f\n@see ref/dotz/#zpi-input"]
.help.i.r[`.z.po;"Where `f` is a unary function, `.z.po` is evaluated when a connection to a kdb+ session has been initialized, i.e.\n@syntax .z.po:f\n@see ref/dotz/#zpo-open"]
.help.i.r[`.z.pp;"Where `f` is a unary function, `.z.pp` is evaluated when an HTTP POST request is received in the kdb+ session.\n@syntax .z.pp:f\n@see ref/dotz/#zpp-http-post"]
.help.i.r[`.z.pq;"Remote connections using the ‘qcon’ text protocol are routed to `.z.pq`, which defaults to calling `.z.pi`.\n@syntax .z.pq:f\n@see ref/dotz/#zpq-qcon"]
.help.i.r[`.z.ps;"Where `f` is a unary function, `.z.ps` is evaluated with the object that is passed to this kdb+ session via an asynchronous request.\n@syntax .z.ps:f\n@example q).z.ps:{[x]0N!(`zps;x);value x}\nq).z.pg:{[x]0N!(`zpg;x);value x}\nq)0 \"2+2\"\n(`zps;\"2+2\")\n4\n@see ref/dotz/#zps-set"]
.help.i.r[`.z.pw;"Where `f` is a binary function, `.z.pw` is evaluated after the `-u`/`-U` checks, and before `.z.po` when opening a new connection to a kdb+ session.\n@syntax .z.pw:f\n@see ref/dotz/#zpw-validate-user"]
.help.i.r[`.z.ts;"Where `f` is a unary function, `.z.ts` is evaluated on intervals of the timer variable set by system command `\\t`.\n@syntax .z.ts:f\n@example q)/ Set the timer to 1000 milliseconds\nq)\\t 1000\nq)/ Argument x is the timestamp scheduled for the callback\nq)/ .z.ts is called once per second and returns the timestamp\nq).z.ts:{0N!x}\nq)2010.12.16D17:12:12.849442000\n@see ref/dotz/#zts-timer"]
.help.i.r[`.z.vs;"Where `f` is a binary function, `.z.vs` is evaluated after a value is set globally in the default namespace (e.g.\n@syntax .z.vs:f\n@example q).z.vs:{0N!(x;y;value x)}\nq)m:(1 2;3 4)\n(`m;();(1 2;3 4))\nq)m[1;1]:0\n(`m;1 1;(1 2;3 0))\n@see ref/dotz/#zvs-value-set"]
.help.i.r[`.z.wc;"WebSocket close.\n@syntax .z.wc:f\n@see ref/dotz/#zwc-websocket-close"]
.help.i.r[`.z.wo;"WebSocket open.\n@syntax .z.wo:f\n@see ref/dotz/#zwo-websocket-open"]
.help.i.r[`.z.ws;"Where `f` is a unary function, it is evaluated on a message arriving at a websocket.\n@syntax z.ws:f\n@see ref/dotz/#zws-websockets"]
.help.i.r[`.h.br;"HTML linebreak (string), defaults to `\"<br>\"`.\n@see ref/doth/#hbr-linebreak"]
.help.i.r[`.h.c0;"Color used by the web console (symbol), defaults to `` `024C7E``.\n@see ref/doth/#hc0-web-color"]
.help.i.r[`.h.c1;"Color used by the web console (symbol), defaults to `` `958600``.\n@see ref/doth/#hc1-web-color"]
.help.i.r[`.h.cd;"Where `x` is a table or a list of columns returns a matrix of comma-separated values.\n@syntax .h.cd x\n@example q).h.cd ([]a:1 2 3;b:`x`y`z)\n\"a,b\"\n\"1,x\"\n\"2,y\"\n\"3,z\"\n\n@see ref/doth/#hcd-csv-from-data"]
.help.i.r[`.h.code;"Where `x` is a string with embedded Tab characters, returns the string with alternating segments marked up as\n@syntax .h.code x\n@example q).h.code \"foo\\tbar\"\n\"foo <code><nobr>bar</nobr></code>\"\nq).h.code \"foo\\tbar\\tabc\\tdef\"\n\"foo <code><nobr>bar</nobr></code> abc <code><nobr>def</nobr></code>\"\nq).h.code \"foo\"\n\"foo\"\n@see ref/doth/#hcode-code-after-tab"]
.help.i.r[`.h.d;"Delimiter used by `.h.cd` to join subitems of nested lists.\n@see ref/doth/#hd-delimiter"]
.help.i.r[`.h.ed;"Where `x` is a table, returns as a list of strings the XML for an Excel workbook.\n@syntax .h.ed x\n@example q).h.ed ([]a:1 2 3;b:`x`y`z)\n\"<?xml version=\\\"1.0\\\"?><?mso-application progid=\\\"Excel.Sheet\\\"?>\"\n\"<Workbook xmlns=\\\"urn:schemas-microsoft-com:office:spreadsheet\\\" xmlns:o=\\\"u..\n@see ref/doth/#hed-excel-from-data"]
.help.i.r[`.h.edsn;"Excel from tables.\n@syntax .h.edsn x!y\n@see ref/doth/#hedsn-excel-from-tables"]
.help.i.r[`.h.fram;"HTML page with two frames — Where\n@syntax .h.fram[t;s;(l;r)]\n@see ref/doth/#hfram-frame"]
.help.i.r[`.h.ha;"Where `x` is the `href` attribute as a symbol atom or a string, and `y` is the link text as a string, returns as a string an HTML `A` element.\n@syntax .h.ha[x;y]\n@example q).h.ha[`http://www.example.com;\"Example.com Main Page\"]\n\"<a href=http://www.example.com>Example.com Main Page</a>\"\nq).h.ha[\"http://www.example.com\";\"Example.com Main Page\"]\n\"<a href=\\\"http://www.example.com\\\">Example.com Main Page</a>\"\n@see ref/doth/#hha-anchor"]
.help.i.r[`.h.hb;"Same as `.h.ha`, but adds a `target=v` attribute to the tag.\n@syntax .h.hb[x;y]\n@example q).h.hb[\"http://www.example.com\";\"Example.com Main Page\"]\n\"<a target=v href=\\\"http://www.example.com\\\">Example.com Main Page</a>\"\n@see ref/doth/#hhb-anchor-target"]
.help.i.r[`.h.hc;"Where `x` is a string, returns `x` with any `<` chars escaped.\n@syntax .h.hc x\n@example q).h.hc \"<foo>\"\n\"&lt;foo>\"\n@see ref/doth/#hhc-escape-lt"]
.help.i.r[`.h.he;"Where `x` is a string, escapes `\"<\"` characters, adds a `\"'\"` at the front, and returns an HTTP 400 error (Bad Request) with that content.\n@syntax .h.he x\n@example q).h.he \"<rubbish>\"\n\"HTTP/1.1 400 Bad Request\\r\nContent-Type: text/plain\\r\nConnection: close\\r\\..\n@see ref/doth/#hhe-http-400"]
.help.i.r[`.h.hn;"HTTP response.\n@syntax .h.hn[x;y;z]\n@example q).h.hn[\"404\";`txt;\"Not found: favicon.ico\"]\n\"HTTP/1.1 404\\r\nContent-Type: text/plain\\r\nConnection: close\\r\nContent-Length: 22\\r\n\\r\nNot found: favicon.ico\"\n@see ref/doth/#hhn-http-response"]
.help.i.r[`.h.hp;"Where `x` is a list of strings, returns as a string a valid HTTP response displaying them as a `pre` element in an HTML document.\n@syntax .h.hp x\n@example q)1 .h.hp\" \"sv'2#''string 5 10#50?100;\nHTTP/1.1 200 OK\nContent-Type: text/html\nConnection: close\nContent-Length: 257\n\n@see ref/doth/#hhp-http-response-pre"]
.help.i.r[`.h.hr;"Where `x` is a string, returns a string of the same length filled with `\"-\"`.\n@syntax .h.hr x\n@example q).h.hr \"foo\"\n\"---\"\n@see ref/doth/#hhr-horizontal-rule"]
.help.i.r[`.h.ht;"HTML documentation generator: <!-- for <https://kx.com/q/d/> --> where `x` is a symbol atom, reads file `:src/x.txt` and writes file `:x.htm`.\n@syntax .h.ht x\n@see ref/doth/#hht-marqdown-to-html"]
.help.i.r[`.h.hta;"Where `x` is the element as a symbol atom, and `y` is a dictionary of attributes and values, returns as a string an opening HTML tag for element `x`.\n@syntax .h.hta[x;y]\n@example q).h.hta[`a;(`href`target)!(\"http://www.example.com\";\"_blank\")]\n\"<a href=\\\"http://www.example.com\\\" target=\\\"_blank\\\">\"\n@see ref/doth/#hhta-start-tag"]
.help.i.r[`.h.htac;"Where `x` is the element as a symbol atom, `y` is a dictionary of attributes and their values, and `z` is the content of the node as a string, returns as a string the HTML element.\n@syntax .h.htac[x;y;z]\n@example q).h.htac[`a;(`href`target)!(\"http://www.example.com\";\"_blank\");\"Example.com Main Page\"]\n\"<a href=\\\"http://www.example.com\\\" target=\\\"_blank\\\">Example.com Main Page</..\n@see ref/doth/#hhtac-element"]
.help.i.r[`.h.htc;"Where `x` is the HTML element as a symbol atom, and `y` is the content of the node as a string, returns as a string the HTML node.\n@syntax .h.htc[x;y]\n@example q).h.htc[`tag;\"value\"]\n\"<tag>value</tag>\"\n@see ref/doth/#hhtac-element"]
.help.i.r[`.h.html;"Where `x` is the body of an HTML document as a string, returns as a string an HTML document with fixed style rules.\n@syntax .h.html x\n@example q).h.html \"<p>Hello world!</p>\"\n\"<html><head><style>a{text-decoration:none}a:link{color:024C7E}a:visited{colo..\n@see ref/doth/#hhtml-document"]
.help.i.r[`.h.http;"Where `x` is a string, returns `x` with embedded URLs beginning `\"http://\"` converted to HTML hyperlinks.\n@syntax .h.http x\n@example q).h.http \"The main page is http://www.example.com\"\n\"The main page is <a href=\\\"http://www.example.com\\\">http://www.example.com</..\n@see ref/doth/#hhttp-hyperlinks"]
.help.i.r[`.h.hu;"Where `x` is a string, returns `x` with URI-unsafe characters replaced with safe equivalents.\n@syntax .h.hu x\n@example q).h.hu \"http://www.kx.com\"\n\"http%3a%2f%2fwww.kx.com\"\n@see ref/doth/#hhu-uri-escape"]
.help.i.r[`.h.hug;"Where `x` is a char vector, returns a mapping from characters to `%`xx escape sequences except for the chars in `x`, which get mapped to themselves.\n@syntax .h.hug x\n@see ref/doth/#hhug-uri-map"]
.help.i.r[`.h.hy;"HTTP response content.\n@syntax .h.hy[x;y]\n@see ref/doth/#hhy-http-response-content"]
.help.i.r[`.h.HOME;"String: location of the webserver root.\n@see ref/doth/#hhome-webserver-root"]
.help.i.r[`.h.iso8601;"Where `x` is nanoseconds since 2000.01.01 as an int atom, returns as a string a timestamp in ISO-8601 format.\n@syntax .h.iso8601 x\n@example q).h.iso8601 100\n\"2000-01-01T00:00:00.000000100\"\n@see ref/doth/#hiso8601-iso-timestamp"]
.help.i.r[`.h.jx;"Where `x` is an int atom, and `y` is the name of a table, returns a list of strings representing the records of `y`, starting from row `x`.\n@syntax .h.jx[x;y]\n@example q)a:([] a:100*til 1000;b:1000?1000;c:1000?1000)\nq){(where x=\"<\")_x}first .h.jx[0;`a]\n\"<a href=\\\"?[0\\\">home\"\n\"</a> \"\n\"<a href=\\\"?[0\\\">up\"\n\"</a> \"\n@see ref/doth/#hjx-table"]
.help.i.r[`.h.logo;"String: defaults to the KX logo in HTML format.\n@see ref/doth/#hlogo-kx-logo"]
.help.i.r[`.h.nbr;"Where `x` is a string, returns `x` as the content of a `nobr` element.\n@syntax .h.nbr x\n@example q).h.nbr \"foo bar\"\n\"<nobr>foo bar</nobr>\"\n@see ref/doth/#hnbr-no-break"]
.help.i.r[`.h.pre;"Where `x` is a list of strings, returns `x` as a string with embedded newlines with a `pre` HTML element.\n@syntax .h.pre x\n@example q).h.pre(\"foo\";\"bar\")\n\"<pre>foo\nbar\n</pre>\"\n@see ref/doth/#hpre-pre"]
.help.i.r[`.h.sa;"String: CSS style rules used in the web console for anchor elements.\n@example q).h.sa\n\"a{text-decoration:none}a:link{color:024C7E}a:visited{color:024C7E}a:active{c..\n@see ref/doth/#hsa-anchor-style"]
.help.i.r[`.h.sb;"String: CSS style rules used in the web console for the HTML body.\n@example q).h.sb\n\"body{font:10pt verdana;text-align:justify}\"\n@see ref/doth/#hsb-body-style"]
.help.i.r[`.h.sc;"String: characters that do not need to be escaped in URIs.\n@example q).h.sc\n\"$-.+!*'(),abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789\"\n@see ref/doth/#hsc-uri-safe"]
.help.i.r[`.h.td;"Where `x` is a table, returns it as a list of tab-separated value strings\n@syntax .h.td x\n@example q).h.td ([]a:1 2 3;b:`x`y`z)\n\"a\\tb\"\n\"1\\tx\"\n\"2\\ty\"\n\"3\\tz\"\n@see ref/doth/#htd-tsv-from-data"]
.help.i.r[`.h.text;"Where `x` is a list of strings, returns as a string, `x` with each item as the content of a `p` element.\n@syntax .h.text x\n@example q).h.text(\"foo\";\"bar\")\n\"<p>foo</p>\n<p>bar</p>\n\"\n@see ref/doth/#htext-paragraphs"]
.help.i.r[`.h.tx;"Dictionary of file types and corresponding conversion functions (`.h.cd`, `.h.td`, `.h.xd`, `.h.ed`).\n@example q).h.tx\nraw | ,:\njson| k){.j.j'$[.Q.qt x;0!x;x]}\ncsv | k){.q.csv 0:$[.Q.qt x;![x;();0b;(!t)[c]!,:'.q.sv[d]@/:'$v c:&(~l=-10h)&0>l:.Q.tx'v:. t:+0!x];x]}\ntxt | k){\"\\t\"0:x}\nxml | k){g:{(#*y)#'(,,\"<\",x),y,,,\"</\",x:($x),\">\"};(,\"<R>\"),(,/'+g[`r]@,/(!x)g'{,xs'$[11h=@x;$x;t&77h>t:@x;$x;x]}'x:+0!x),,\"</R>\"}\n@see ref/doth/#htx-filetypes"]
.help.i.r[`.h.ty;"Dictionary of content types and corresponding media types.\n@example q).h.ty\nhtm | \"text/html\"\nhtml| \"text/html\"\ncsv | \"text/comma-separated-values\"\ntxt | \"text/plain\"\nxml | \"text/plain\"\n@see ref/doth/#hty-mime-types"]
.help.i.r[`.h.uh;"Where `x` is a string, returns `x` with `%`xx hex sequences replaced with character equivalents.\n@syntax .h.uh x\n@example q).h.uh \"http%3a%2f%2fwww.kx.com\"\n\"http://www.kx.com\"\n@see ref/doth/#huh-uri-unescape"]
.help.i.r[`.h.val;"`.h.val` is called by `.z.ph` to evaluate a request to the server.\n@syntax .h.val x\n@see ref/doth/#hval-value"]
.help.i.r[`.h.xd;"Where `x` is a table, returns as a list of strings, `x` as an XML table.\n@syntax .h.xd x\n@example q).h.xd ([]a:1 2 3;b:`x`y`z)\n\"<R>\"\n\"<r><a>1</a><b>x</b></r>\"\n\"<r><a>2</a><b>y</b></r>\"\n\"<r><a>3</a><b>z</b></r>\"\n\"</R>\"\n@see ref/doth/#hxd-xml"]
.help.i.r[`.h.xmp;"Where `x` is a list of strings, returns as a string `x` as the newline-separated content of an HTML `xmp` element.\n@syntax .h.xmp x\n@example q).h.xmp(\"foo\";\"bar\")\n\"<xmp>foo\nbar\n</xmp>\"\n@see ref/doth/#hxmp-xmp"]
.help.i.r[`.h.xs;"Where `x` is a string, returns `x` with characters XML-escaped where necessary.\n@syntax .h.xs x\n@example q).h.xs \"Arthur & Co.\"\n\"Arthur &amp; Co.\"\n@see ref/doth/#hxs-xml-escape"]
.help.i.r[`.h.xt;"Where `x` is `` `json`` and `y` is a list of JSON strings, returns `y` as a list of dictionaries.\n@syntax .h.xt[x;y]\n@example q).h.xt[`json;(\"{\\\"foo\\\":\\\"bar\\\"}\";\"{\\\"this\\\":\\\"that\\\"}\")]\n(,`foo)!,\"bar\"\n(,`this)!,\"that\"\nq)first .h.xt[`json;(\"{\\\"foo\\\":\\\"bar\\\"}\";\"{\\\"this\\\":\\\"that\\\"}\")]\nfoo| \"bar\"\n@see ref/doth/#hxt-json"]
.help.i.r[`.j.j;"Where `x` is a K object, returns a string representing it in JSON.\n@syntax .j.j x\n@see ref/dotj/#jj-serialize"]
.help.i.r[`.j.k;"Where `x` is a string containing JSON, returns a K object.\n@syntax .j.k x\n@example q).j.k 0N!.j.j `a`b!(0 1;(\"hello\";\"world\"))        / dictionary\n\"{\\\"a\\\":[0,1],\\\"b\\\":[\\\"hello\\\",\\\"world\\\"]}\"\na| 0       1\nb| \"hello\" \"world\"\nq).j.k 0N!.j.j ([]a:1 2;b:`Greetings`Earthlings)   / table\n\"[{\\\"a\\\":1,\\\"b\\\":\\\"Greetings\\\"},{\\\"a\\\":2,\\\"b\\\":\\\"Earthlings\\\"}]\"\n@see ref/dotj/#jk-deserialize"]
.help.i.r[`.j.jd;"serialize infinity.\n@syntax .j.jd (x;d)\n@example q).j.j -0w 0 1 2 3 0w\n\"[-inf,0,1,2,3,inf]\"\nq).j.jd(-0w 0 1 2 3 0w;()!())\n\"[-inf,0,1,2,3,inf]\"\nq).j.jd(-0w 0 1 2 3 0w;([null0w:1b]))\n\"[null,0,1,2,3,null]\"\n@see ref/dotj/#jjd-serialize-infinity"]
.help.i.r[`$"0:";"Prepare.\n@example show csv 0: ([]a:1 2 3;b:`x`y`z)"]
.help.i.r[`$"1";"Write to standard output\n@example 1 \"String vector here\""]
.help.i.r[`$"1:";"The 1: dyadic function is used to read fixed length data from a file or byte sequence.\n@example (\"ich\";4 1 2)1:0x00000000410000FF00000042FFFF"]
.help.i.r[`$"2";"write to standard error\n@example 2 \"String vector here\""]
.help.i.r[`$"2:";"The 2: function is a dyadic function used to dynamically load C functions into Kdb+."]
.help.i.r[`.Q.U;"In segmented dbs, true if each partition is uniquely found in one segment."]
.help.i.r[`.Q.atob;"Decodes base64 data to a byte vector."]
.help.i.r[`.Q.bvi;"Incremental version of .Q.bv scanning only new partitions."]
.help.i.r[`.Q.ld;"Load-and-group script lines for evaluation (used by \\l)."]
.help.i.r[`.Q.li;"Load additional partitions into the current HDB."]
.help.i.r[`.Q.lo;"Load database without changing directory or running scripts."]
.help.i.r[`.Q.trpd;"Trap function with backtrace support for general-rank f."]
.help.i.r[`.Q.st;"Timespace statistics helper (documented as ts/st)."]
.help.i.r[`.Q.vt;"Table types (set by .Q.bv), used during partitioned select."]
.help.i.r[`.Q.p1;"Partitioned select helper, used internally by .Q.ps."]
.help.i.r[`.Q.ps;"Partitioned select — applies a where-clause to each partition."]
.help.i.r[`.h.ka;"Coerce to atom."]
.help.i.r[`.z.1;"quiet mode"]
.help.i.r[`.z.H;"Active sockets as a low-cost list."]
.help.i.r[`.z.r;"Indicates whether updates in this context are blocked."]
.help.i.r[`.z.M;"Module namespace name."]
.help.i.r[`.z.m;"Module namespace."]
/ <<< end generated
