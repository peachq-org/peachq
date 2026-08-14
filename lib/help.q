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
.help.i.r[`abs;"abs -5 3 -2                         / 5 3 2                   take the magnitude, dropping the sign"]
.help.i.r[`aj;"aj[`sym`time;trade;quote]           /                         join the last quote at or before each trade time"]
.help.i.r[`aj0;"aj0[`sym`time;trade;quote]          /                         as-of join that keeps the matched row's own time"]
.help.i.r[`ajf;"ajf[`sym`time;trade;quote]          /                         as-of join that fills nulls from the left table"]
.help.i.r[`ajf0;"ajf0[`sym`time;trade;quote]         /                         as-of join filling nulls, keeping the matched time"]
.help.i.r[`all;"all 1 2 3=1 2 4                     / 0b                      test whether every item is true"]
.help.i.r[`any;"any 1 2 3=1 20 30                   / 1b                      test whether at least one item is true"]
.help.i.r[`and;"1010b and 1100b                     / 1000b                   take the lesser of two values - logical AND on booleans"]
.help.i.r[`asc;"asc 2 1 3 2                         / 1 2 2 3                 sort a list, dictionary or table ascending"]
.help.i.r[`iasc;"iasc 2 1 3                          / 1 0 2                   give the indices that would sort a list ascending"]
.help.i.r[`xasc;"`price xasc trade                   /                         sort a table ascending by the named columns"]
.help.i.r[`asof;"trade asof `AAPL                    /                         take the last row of a table at or before a key value"]
.help.i.r[`attr;"attr `s#1 3 4                       / `s                      report an object's attribute: s, u, p or g"]
.help.i.r[`avg;"avg 1 2 3 4                         / 2.5                     take the arithmetic mean as a float"]
.help.i.r[`avgs;"avgs 1 2 3 4                        / 1 1.5 2 2.5             take the running mean at every position"]
.help.i.r[`mavg;"3 mavg 1 2 3 4 5                    / 1 1.5 2 3 4             take the mean over a sliding window of x items"]
.help.i.r[`wavg;"1 2 3 wavg 10 20 30                 / 23.33333                take the mean of y weighted by x"]
.help.i.r[`bin;"bin[1 3 5;4]                        / 1                       binary-search for the index of the last item up to y"]
.help.i.r[`binr;"binr[1 3 5;4]                       / 2                       binary-search for the index of the first item from y"]
.help.i.r[`ceiling;"ceiling 2.1 -2.1                    / 3 -2                    round up to the next whole number"]
.help.i.r[`count;"count ([]a:1 2;b:3 4)               / 2                       count the items of a list, dictionary or table"]
.help.i.r[`mcount;"3 mcount 1 2 3 4                    / 1 2 3 3                 count the non-null items in a sliding window"]
.help.i.r[`cols;"cols ([]a:1 2;b:3 4)                / `a`b                    list a table's column names"]
.help.i.r[`xcol;"`x`y xcol ([]a:1 2;b:3 4)           / +`x`y!(1 2;3 4)         rename a table's leading columns"]
.help.i.r[`xcols;"`b`a xcols ([]a:1 2;b:3 4)          / +`b`a!(3 4;1 2)         reorder a table's columns, the named ones first"]
.help.i.r[`cor;"cor[1 2 3;2 4 7]                    / 0.9933993               measure how two lists correlate, from -1 to 1"]
.help.i.r[`cos;"cos 0 1                             / 1 0.5403023             take the cosine of an angle in radians"]
.help.i.r[`acos;"acos 1                              / 0f                      take the arccosine, in radians"]
.help.i.r[`cov;"cov[1 2 3;2 4 7]                    / 1.666667                measure the covariance of two lists"]
.help.i.r[`scov;"scov[1 2 3;2 4 7]                   / 2.5                     measure the sample covariance of two lists"]
.help.i.r[`cross;"1 2 cross `a`b                      /                         pair every item of x with every item of y"]
.help.i.r[`csv;"csv vs \"a,b,c\"                      / (,\"a\";,\"b\";,\"c\")        name the comma delimiter, for csv text"]
.help.i.r[`cut;"2 cut til 6                         / (0 1;2 3;4 5)           cut a list into pieces of x items each\n1 3 cut til 6                       / (1 2;3 4 5)             cut a list at each of the indices in x"]
.help.i.r[`delete;"delete b from ([]a:1 2;b:3 4)       / +(,`a)!,1 2             drop rows or columns from a table"]
.help.i.r[`deltas;"deltas 1 4 9                        / 1 3 5                   take the difference between adjacent items"]
.help.i.r[`desc;"desc 2 1 3                          / 3 2 1                   sort descending"]
.help.i.r[`idesc;"idesc 2 1 3                         / 2 0 1                   give the indices that would sort a list descending"]
.help.i.r[`xdesc;"`price xdesc trade                  /                         sort a table descending by the named columns"]
.help.i.r[`dev;"dev 1 2 3 4                         / 1.118034                measure the standard deviation of a list"]
.help.i.r[`mdev;"3 mdev 1 2 3 4                      /                         measure the standard deviation over a sliding window"]
.help.i.r[`sdev;"sdev 1 2 3 4                        / 1.290994                measure the sample standard deviation"]
.help.i.r[`differ;"differ 1 1 2 2 3                    / 10101b                  flag each item that differs from the one before it"]
.help.i.r[`distinct;"distinct 1 2 1 3                    / 1 2 3                   keep the unique items, in first-seen order"]
.help.i.r[`div;"7 div 2                             / 3                       divide and round the quotient down to a whole number"]
.help.i.r[`dsave;"`:db dsave `trade                   /                         write global tables to disk splayed and enumerated"]
.help.i.r[`each;"count each (\"ab\";\"cde\")             / 2 3                     apply a function to every item separately"]
.help.i.r[`peach;"count peach (\"ab\";\"cde\")            / 2 3                     apply a function to every item on secondary threads"]
.help.i.r[`ej;"ej[`a;([]a:1 2);([]a:1 2;b:`x`y)]   / +`a`b!(1 2;`x`y)        join two tables on matching values of the named columns"]
.help.i.r[`ema;"0.5 ema 1 2 3                       / 1 1.5 2.25              take the exponentially weighted moving average"]
.help.i.r[`enlist;"enlist 1 2 3                        / ,1 2 3                  wrap the arguments as a single list"]
.help.i.r[`eval;"eval parse \"2+3\"                    / 5                       evaluate a parse tree"]
.help.i.r[`reval;"reval parse \"2+3\"                   /                         evaluate a parse tree under the remote-client restrictions"]
.help.i.r[`except;"1 2 3 4 except 2 4                  / 1 3                     drop from x every item that appears in y"]
.help.i.r[`exec;"exec a from ([]a:1 2)               / 1 2                     read columns out of a table as values, not as a table"]
.help.i.r[`exit;"exit 0                              /                         end the process with an exit code"]
.help.i.r[`exp;"exp 1                               / 2.718282                raise e to a power"]
.help.i.r[`xexp;"2 xexp 10                           / 1024f                   raise x to the power y"]
.help.i.r[`fby;"select from t where a=(max;a) fby b /                         compare each row against an aggregate of its group"]
.help.i.r[`fills;"fills 1 0N 3 0N                     / 1 1 3 3                 replace each null with the last non-null before it"]
.help.i.r[`first;"first 1 2 3                         / 1                       take the first item"]
.help.i.r[`last;"last 1 2 3                          / 3                       take the last item"]
.help.i.r[`fkeys;"fkeys t                             /                         map a table's foreign-key columns to the tables they cite"]
.help.i.r[`flip;"flip (1 2;3 4)                      / (1 3;2 4)               transpose a matrix, dictionary or table"]
.help.i.r[`floor;"floor 2.7 -2.7                      / 2 -3                    round down to the next whole number"]
.help.i.r[`get;"a:42;get `a                         / 42                      read the value bound to a name\nget `:trade                         /                         read or memory-map a kdb+ data file"]
.help.i.r[`set;"`a set 42                           / `a                      bind a value to a global name\n`:t.dat set til 3                   /                         write a value to a file"]
.help.i.r[`getenv;"getenv `HOME                        /                         read an environment variable"]
.help.i.r[`setenv;"`FOO setenv \"bar\"                   /                         set an environment variable for this process"]
.help.i.r[`group;"group \"mississippi\"                 /                         map each distinct item to the indices where it occurs"]
.help.i.r[`gtime;"gtime 2000.01.01D12:00:00           /                         convert a local timestamp to UTC"]
.help.i.r[`ltime;"ltime 2000.01.01D12:00:00           /                         convert a UTC timestamp to local time"]
.help.i.r[`hcount;"hcount `:trade.csv                  /                         report a file's size in bytes"]
.help.i.r[`hdel;"hdel `:trade.csv                    /                         delete a file, or an empty directory"]
.help.i.r[`hopen;"hopen `::5000                       /                         open a connection to a process, file or fifo"]
.help.i.r[`hclose;"hclose h                            /                         close a connection handle"]
.help.i.r[`hsym;"hsym `trade.csv                     / `:trade.csv             turn a symbol into a file or process handle symbol"]
.help.i.r[`ij;"ij[([]a:1 2);([a:1 2]b:`x`y)]       / +`a`b!(1 2;`x`y)        keep the rows of x whose key matches y, adding y's columns"]
.help.i.r[`ijf;"ijf[([]a:1 2);([a:1 2]b:`x`y)]      / +`a`b!(1 2;`x`y)        inner join that keeps x's values where y is null"]
.help.i.r[`in;"2 in 1 2 3                          / 1b                      test whether each item of x appears in y"]
.help.i.r[`insert;"t:([]a:1;b:`x);`t insert (2;`y)     / ,1                      append records to a table named by symbol"]
.help.i.r[`inter;"1 2 3 inter 2 3 4                   / 2 3                     keep the items that appear in both lists"]
.help.i.r[`inv;"inv 2 2#1 0 0 1f                    /                         invert a non-singular float matrix"]
.help.i.r[`key;"key `a`b!1 2                        / `a`b                    list a dictionary's keys\nkey ([a:1 2]b:3 4)                  / +(,`a)!,1 2             take a keyed table's key table\nkey 4                               / 0 1 2 3                 list the first x integers, as til does\nkey `:.                             /                         list the files in a directory"]
.help.i.r[`keys;"keys ([a:1 2]b:3 4)                 / ,`a                     name a table's primary-key columns"]
.help.i.r[`xkey;"`a xkey ([]a:1 2;b:3 4)             /                         make the named columns a table's primary key"]
.help.i.r[`like;"\"quick\" like \"qu*\"                  / 1b                      match text against a glob pattern"]
.help.i.r[`lj;"([]a:1 2)lj([a:1 2]b:`x`y)          / +`a`b!(1 2;`x`y)        add y's columns to x, matching on y's key"]
.help.i.r[`ljf;"([]a:1 2)ljf([a:1 2]b:`x`y)         / +`a`b!(1 2;`x`y)        left join that keeps x's values where y is null"]
.help.i.r[`load;"load `:trade                        /                         read a binary file or splayed table into a global"]
.help.i.r[`rload;"rload `trade                        /                         read a splayed table from a directory of that name"]
.help.i.r[`log;"log 1 2                             / 0 0.6931472             take the natural logarithm"]
.help.i.r[`xlog;"2 xlog 8                            / 3f                      take the logarithm of y in base x"]
.help.i.r[`lower;"lower \"ABC\"                         / \"abc\"                   fold text or symbols to lower case"]
.help.i.r[`upper;"upper \"abc\"                         / \"ABC\"                   fold text or symbols to upper case"]
.help.i.r[`lsq;"(1 2#1 2f)lsq 2 2#1 0 0 1f          /                         solve x=y by least squares matrix divide"]
.help.i.r[`max;"max 3 1 4                           / 4                       take the largest item"]
.help.i.r[`maxs;"maxs 3 1 4                          / 3 3 4                   take the running maximum"]
.help.i.r[`mmax;"2 mmax 3 1 4                        / 3 3 4                   take the maximum over a sliding window"]
.help.i.r[`md5;"md5 \"hello\"                         /                         hash text with MD5, as bytes"]
.help.i.r[`med;"med 1 2 3 100                       / 2.5                     take the median"]
.help.i.r[`meta;"meta ([]a:1 2;b:`x`y)               /                         describe a table's columns: type, attribute and foreign key"]
.help.i.r[`min;"min 3 1 4                           / 1                       take the smallest item"]
.help.i.r[`mins;"mins 3 1 4                          / 3 1 1                   take the running minimum"]
.help.i.r[`mmin;"2 mmin 3 1 4                        / 3 1 1                   take the minimum over a sliding window"]
.help.i.r[`mmu;"(2 2#1 0 0 1f)mmu 2 2#1 2 3 4f      / (1 2f;3 4f)             multiply two float matrices"]
.help.i.r[`mod;"7 mod 3                             / 1                       take the remainder left by whole division"]
.help.i.r[`neg;"neg 1 -2 3                          / -1 2 -3                 flip the sign"]
.help.i.r[`next;"next 1 2 3                          / 2 3 0N                  shift each item one place earlier"]
.help.i.r[`prev;"prev 1 2 3                          / 0N 1 2                  shift each item one place later"]
.help.i.r[`xprev;"2 xprev 1 2 3 4                     / 0N 0N 1 2               take the item x places earlier"]
.help.i.r[`not;"not 0 1 2                           / 100b                    flag the items that are zero"]
.help.i.r[`null;"null 0N 1                           / 10b                     flag the items that are null"]
.help.i.r[`or;"1010b or 1100b                      / 1110b                   take the greater of two values - logical OR on booleans"]
.help.i.r[`over;"(+) over 1 2 3                      / 6                       reduce a list with a function, keeping only the last result"]
.help.i.r[`scan;"(+) scan 1 2 3                      / 1 3 6                   accumulate a function along a list, keeping every step"]
.help.i.r[`parse;"parse \"2+3\"                         / (+;2;3)                 turn source text into a parse tree"]
.help.i.r[`pj;"pj[([]a:1 2;b:0 0);([a:1 2]b:1 2)]  / +`a`b!(1 2;1 2)         add y's matching numeric columns into x"]
.help.i.r[`prd;"prd 1 2 3 4                         / 24                      multiply the items together"]
.help.i.r[`prds;"prds 1 2 3 4                        / 1 2 6 24                take the running product"]
.help.i.r[`prior;"(-) prior 1 4 9                     / 1 3 5                   apply a function to each item and the one before it"]
.help.i.r[`rand;"rand 10                             /                         pick a random number below x, or an item from a list"]
.help.i.r[`rank;"rank 3 1 4                          / 1 0 2                   give each item its position in the sorted list"]
.help.i.r[`ratios;"ratios 1 4 8                        / 1 4 2f                  divide each item by the one before it"]
.help.i.r[`raze;"raze (1 2;3 4)                      / 1 2 3 4                 join the items, collapsing one level of nesting"]
.help.i.r[`read0;"read0 `:trade.csv                   /                         read a file, or a handle, as lines of text"]
.help.i.r[`read1;"read1 `:trade.csv                   /                         read a file, or a handle, as raw bytes"]
.help.i.r[`reciprocal;"reciprocal 0 2 4                    / 0w 0.5 0.25             divide one by each item"]
.help.i.r[`reverse;"reverse 1 2 3                       / 3 2 1                   reverse the order of the items"]
.help.i.r[`rotate;"2 rotate 1 2 3 4 5                  / 3 4 5 1 2               shift the items left, wrapping round; right if x is negative"]
.help.i.r[`save;"save `:trade.csv                    /                         write a global to a file, format chosen by the suffix"]
.help.i.r[`rsave;"rsave `trade                        /                         write a table splayed to a directory of that name"]
.help.i.r[`select;"select a from ([]a:1 2)             / +(,`a)!,1 2             read rows and columns from a table"]
.help.i.r[`from;"select a from trade                 /                         name the table a select, exec, update or delete reads"]
.help.i.r[`show;"show 1 2 3                          / ::                      format a value and write it to the console"]
.help.i.r[`signum;"signum -3 0 2                       / -1 0 1i                 report the sign as -1, 0 or 1"]
.help.i.r[`sin;"sin 0 1                             / 0 0.841471              take the sine of an angle in radians"]
.help.i.r[`asin;"asin 1                              / 1.570796                take the arcsine, in radians"]
.help.i.r[`sqrt;"sqrt 2 9                            / 1.414214 3              take the square root"]
.help.i.r[`ss;"\"a cat\" ss \"at\"                     / ,3                      find every position where a pattern occurs in text"]
.help.i.r[`ssr;"ssr[\"hello\";\"l\";\"L\"]                / \"heLLo\"                 replace every occurrence of a pattern in text"]
.help.i.r[`string;"string 42                           / \"42\"                    render a value as text"]
.help.i.r[`sublist;"2 sublist 1 2 3 4                   / 1 2                     take up to x items from the front, or a slice x[0] x[1]"]
.help.i.r[`sum;"sum 1 2 3                           / 6                       add the items together"]
.help.i.r[`sums;"sums 1 2 3                          / 1 3 6                   take the running total"]
.help.i.r[`msum;"2 msum 1 2 3 4                      / 1 3 5 7                 total a sliding window of x items"]
.help.i.r[`wsum;"1 2 3 wsum 10 20 30                 / 140f                    total y weighted by x"]
.help.i.r[`sv;"\",\" sv (\"ab\";\"cd\")                  / \"ab,cd\"                 join strings with a delimiter\n` sv `path`to`file                  / `path.to.file           join symbols into one dotted symbol\n2 sv 1 0 1 0b                       / 10                      evaluate digits y in base x\n` sv (`:/tmp;`a.txt)                /                         append a filename to a directory handle"]
.help.i.r[`system;"system \"t\"                          /                         run a system command and take its output as a value"]
.help.i.r[`tables;"tables `.                           / `symbol$()              list the tables of a namespace"]
.help.i.r[`tan;"tan 0 1                             / 0 1.557408              take the tangent of an angle in radians"]
.help.i.r[`atan;"atan 1                              / 0.7853982               take the arctangent, in radians"]
.help.i.r[`til;"til 5                               / 0 1 2 3 4               list the first x integers from zero"]
.help.i.r[`trim;"trim \"  ab  \"                       / \"ab\"                    drop blanks from both ends of text"]
.help.i.r[`ltrim;"ltrim \"  ab\"                        / \"ab\"                    drop blanks from the front of text"]
.help.i.r[`rtrim;"rtrim \"ab  \"                        / \"ab\"                    drop blanks from the end of text"]
.help.i.r[`type;"type 1 2 3                          / 7h                      report the type number of a value"]
.help.i.r[`uj;"([]a:1 2)uj([]b:`x`y)               / +`a`b!(1 2 0N 0N;```x`y)join two tables, unioning their columns"]
.help.i.r[`ujf;"([]a:1 2)ujf([]b:`x`y)              / +`a`b!(1 2 0N 0N;```x`y)union join that keeps x's values where y is null"]
.help.i.r[`union;"1 2 3 union 3 4                     / 1 2 3 4                 join both lists and keep the distinct items"]
.help.i.r[`ungroup;"ungroup ([]a:1 2;b:(1 2;3 4))       / +`a`b!(1 1 2 2;1 2 3 4) flatten a table whose cells hold lists"]
.help.i.r[`update;"update a:0 from ([]a:1 2)           / +(,`a)!,0 0             add or amend the columns of a table"]
.help.i.r[`upsert;"t:([a:1]b:`x);`t upsert (1;`z)      / `t                      append records, replacing any with a matching key"]
.help.i.r[`value;"value \"2+3\"                         / 5                       evaluate text, or unwrap a name, dictionary or function"]
.help.i.r[`var;"var 1 2 3 4                         / 1.25                    measure the variance"]
.help.i.r[`svar;"svar 1 2 3 4                        / 1.666667                measure the sample variance"]
.help.i.r[`view;"view `v                             /                         show the expression a view is defined by"]
.help.i.r[`views;"views[]                             / `symbol$()              list the views defined in a namespace"]
.help.i.r[`vs;"\",\" vs \"a,b,c\"                      / (,\"a\";,\"b\";,\"c\")        split a string on a delimiter\n` vs `path.to.file                  / `path`to`file           split a dotted symbol, or a file handle, into parts\n10 vs 1234                          / 1 2 3 4                 take the digits of y in base x"]
.help.i.r[`where;"where 1 0 1 1b                      / 0 2 3                   give the indices of the items that are true\nwhere 2 3                           / 0 0 1 1 1               repeat each index as many times as its count says"]
.help.i.r[`within;"5 within 1 10                       / 1b                      test whether items fall inside a range, ends included"]
.help.i.r[`wj;"wj[w;`sym`time;t;(q;(max;`bid))]    /                         aggregate y's rows over a window around each row of x"]
.help.i.r[`wj1;"wj1[w;`sym`time;t;(q;(max;`bid))]   /                         window join counting only rows strictly inside the window"]
.help.i.r[`xbar;"5 xbar 3 7 11 18                    / 0 5 10 15               round down to the nearest multiple of x"]
.help.i.r[`xgroup;"`a xgroup ([]a:1 1 2;b:1 2 3)       /                         key a table by the named columns, grouping the rest"]
.help.i.r[`xrank;"3 xrank til 6                       / 0 0 1 1 2 2             label each item with its bucket in an x-way split"]
.help.i.r[`do;"i:0;do[3;i+:1];i                    / 3                       repeat expressions a fixed number of times"]
.help.i.r[`if;"if[1<2;r:\"yes\"];r                   / \"yes\"                   evaluate expressions only when a condition holds"]
.help.i.r[`while;"i:0;while[i<3;i+:1];i               / 3                       repeat expressions while a condition holds"]
.help.i.r[`$"\\a";"\\a                                  /                         list the tables of a namespace"]
.help.i.r[`$"\\b";"\\b                                  /                         list the views defined in a namespace"]
.help.i.r[`$"\\B";"\\B                                  /                         list the views awaiting recalculation"]
.help.i.r[`$"\\c";"\\c 25 200                           /                         show or set the console page size, rows then columns"]
.help.i.r[`$"\\C";"\\C 36 2000                          /                         show or set the page size used for HTTP output"]
.help.i.r[`$"\\cd";"\\cd /tmp                            /                         show or set the working directory"]
.help.i.r[`$"\\d";"\\d .foo                             /                         show or set the namespace new definitions land in"]
.help.i.r[`$"\\e";"\\e 1                                /                         show or set error trapping for client requests"]
.help.i.r[`$"\\E";"\\E 1                                /                         show or set TLS server mode"]
.help.i.r[`$"\\f";"\\f .q                               /                         list the functions of a namespace"]
.help.i.r[`$"\\g";"\\g 1                                /                         show or set garbage collection to immediate or deferred"]
.help.i.r[`$"\\l";"\\l script.q                         /                         load a script, or a database directory"]
.help.i.r[`$"\\o";"\\o 0                                /                         show or set the local time offset in hours from UTC"]
.help.i.r[`$"\\p";"\\p 5000                             /                         show or set the port this process listens on"]
.help.i.r[`$"\\P";"\\P 7                                /                         show or set how many digits floats display"]
.help.i.r[`$"\\r";"\\r                                  /                         show or set the replication primary\n\\r a.txt b.txt                      /                         rename a file"]
.help.i.r[`$"\\s";"\\s 4                                /                         show or set how many secondary threads are available"]
.help.i.r[`$"\\S";"\\S 42                               /                         show or set the seed the random generator starts from"]
.help.i.r[`$"\\t";"\\t sum til 1000                     /                         time an expression in milliseconds\n\\t 1000                             /                         show or set the timer interval feeding .z.ts"]
.help.i.r[`$"\\T";"\\T 5                                /                         show or set the client execution timeout in seconds"]
.help.i.r[`$"\\ts";"\\ts sum til 1000                    /                         time an expression and report the bytes it used"]
.help.i.r[`$"\\u";"\\u                                  /                         reload the user-password file"]
.help.i.r[`$"\\v";"\\v                                  /                         list the variables of a namespace"]
.help.i.r[`$"\\w";"\\w                                  /                         report memory usage, or show and set the workspace limit"]
.help.i.r[`$"\\W";"\\W 2                                /                         show or set which day the week starts on"]
.help.i.r[`$"\\x";"\\x .z.pi                            /                         restore a callback such as .z.pi to its default"]
.help.i.r[`$"\\z";"\\z 1                                /                         show or set the date format \"D\"$ parses"]
.help.i.r[`$"\\1";"\\1 out.txt                          /                         redirect stdout to a file"]
.help.i.r[`$"\\2";"\\2 err.txt                          /                         redirect stderr to a file"]
.help.i.r[`$"\\_";"\\_ script.q                         /                         hide a script's source, or ask whether one is hidden"]
.help.i.r[`$"\\\\";"\\\\                                  /                         end the q process"]
.help.i.r[`.Q.A;".Q.A                                /                         name the upper-case alphabet"]
.help.i.r[`.Q.a;".Q.a                                /                         name the lower-case alphabet"]
.help.i.r[`.Q.an;".Q.an                               /                         name the letters and digits, both cases"]
.help.i.r[`.Q.b6;".Q.b6                               /                         name the base-64 alphabet"]
.help.i.r[`.Q.n;".Q.n                                / \"0123456789\"            name the decimal digits"]
.help.i.r[`.Q.nA;".Q.nA                               /                         name the digits with the upper-case alphabet"]
.help.i.r[`.Q.addmonths;".Q.addmonths[2000.01.15;1]          / 2000.02.15              add a number of months to a date"]
.help.i.r[`.Q.addr;".Q.addr `localhost                  /                         resolve a hostname to an IP address as an int"]
.help.i.r[`.Q.host;".Q.host .Q.addr `localhost          /                         resolve an IP address int back to a hostname"]
.help.i.r[`.Q.bt;".Q.bt[]                             /                         dump the backtrace of the current execution to stdout"]
.help.i.r[`.Q.sbt;".Q.sbt .Q.bt[]                      /                         render a backtrace object as display text"]
.help.i.r[`.Q.trp;".Q.trp[{x+1};`a;{y}]                /                         trap a unary and hand the handler a backtrace"]
.help.i.r[`.Q.trpd;".Q.trpd[+;(1;2);{y}]                /                         trap a function of any rank with a backtrace"]
.help.i.r[`.Q.btoa;".Q.btoa \"hello\"                     /                         encode text as base-64"]
.help.i.r[`.Q.atob;".Q.atob \"aGVsbG8=\"                  / 0x68656c6c6f            decode base-64 text back to bytes"]
.help.i.r[`.Q.sha1;".Q.sha1 \"hello\"                     /                         hash text with SHA-1"]
.help.i.r[`.Q.gz;".Q.gz(6;20#0x2a)                    /                         compress or decompress a byte vector with zlib"]
.help.i.r[`.Q.bv;".Q.bv[]                             /                         fill missing tables across partitions with empty schemas"]
.help.i.r[`.Q.bvi;".Q.bvi[]                            / ()                      extend .Q.bv over the partitions added since it last ran"]
.help.i.r[`.Q.chk;".Q.chk `:db                         /                         create any partition directories a table is missing"]
.help.i.r[`.Q.cn;".Q.cn trade                         /                         count the rows of a partitioned table"]
.help.i.r[`.Q.D;".Q.D                                / ()                      list each segment's partitions, in .Q.P order"]
.help.i.r[`.Q.P;".Q.P                                /                         list the segments of a segmented database"]
.help.i.r[`.Q.PD;".Q.PD                               / `symbol$()              list where each partition lives on disk"]
.help.i.r[`.Q.pd;".Q.pd                               / `symbol$()              list the partition locations .Q.view leaves in play"]
.help.i.r[`.Q.PV;".Q.PV                               / `date$()                list the partition value behind each partition location"]
.help.i.r[`.Q.pv;".Q.pv                               / `date$()                list the partition values .Q.view leaves in play"]
.help.i.r[`.Q.pf;".Q.pf                               / `date                   name the partition column: date, month, year or int"]
.help.i.r[`.Q.pn;".Q.pn                               / ()!()                   report the cached row counts per table per partition"]
.help.i.r[`.Q.pt;".Q.pt                               / `symbol$()              list the partitioned tables"]
.help.i.r[`.Q.par;".Q.par[`:db;2024.01.01;`trade]      /                         locate the directory holding one partition of a table"]
.help.i.r[`.Q.view;".Q.view 2024.01.01                  /                         restrict every partitioned query to these partitions"]
.help.i.r[`.Q.vp;".Q.vp                               / ()!()                   list the schemas standing in for tables missing partitions"]
.help.i.r[`.Q.vt;".Q.vt                               /                         record which tables .Q.bv filled in"]
.help.i.r[`.Q.U;".Q.U                                /                         tell whether each partition sits in exactly one segment"]
.help.i.r[`.Q.MAP;".Q.MAP[]                            / ()                      keep partitions mapped instead of remapping per query"]
.help.i.r[`.Q.M;".Q.M                                / 0W                      set the chunk size .Q.dsftg loads in"]
.help.i.r[`.Q.ind;".Q.ind[trade;0 1]                   /                         index a partitioned table by absolute row number"]
.help.i.r[`.Q.dpft;".Q.dpft[`:db;2024.01.01;`sym;`trade] /                         save a table to a partition, sorted and parted by a column"]
.help.i.r[`.Q.dpfts;".Q.dpfts[`:db;d;`sym;`trade;s]      /                         save a partition reusing an existing symbol file"]
.help.i.r[`.Q.dpt;".Q.dpt[`:db;2024.01.01;`trade]      /                         save a table to a partition without parting it"]
.help.i.r[`.Q.dpts;".Q.dpts[`:db;d;`trade;s]            /                         save an unparted partition reusing a symbol file"]
.help.i.r[`.Q.hdpf;".Q.hdpf[5000;`:db;d;`sym]           /                         save every in-memory table to a partition and reload"]
.help.i.r[`.Q.dsftg;".Q.dsftg[(0;10);h;4;3 1;`I`s;g]     /                         load, convert and save data in fixed-size chunks"]
.help.i.r[`.Q.en;".Q.en[`:db;trade]                   /                         enumerate a table's symbol columns against a sym file"]
.help.i.r[`.Q.ens;".Q.ens[`:db;trade;`mysym]           /                         enumerate against a named domain rather than sym"]
.help.i.r[`.Q.f;".Q.f[2;3.14159]                     / \"3.14\"                  format a number to x decimal places"]
.help.i.r[`.Q.fmt;".Q.fmt[8;2;3.14159]                 / \"    3.14\"              format a number to x characters and y decimals"]
.help.i.r[`.Q.fc;".Q.fc[sum;til 10]                   /                         apply a function to a vector split across threads"]
.help.i.r[`.Q.ff;".Q.ff[([]a:1 2);([]b:3 4)]          / +`a`b!(1 2;0N 0N)       append the columns of y that x lacks"]
.help.i.r[`.Q.fk;".Q.fk `trade.sym                    /                         name the table a foreign-key column points at"]
.help.i.r[`.Q.ft;".Q.ft[0!;([a:1 2]b:3 4)]            /                         apply a simple-table function to a keyed table"]
.help.i.r[`.Q.fu;".Q.fu[{x*2};1 1 2 2]                / 2 2 4 4                 apply a function to the distinct items only"]
.help.i.r[`.Q.fs;".Q.fs[{show x};`:trade.csv]         /                         feed a file to a function in chunks"]
.help.i.r[`.Q.fsn;".Q.fsn[{show x};`:t.csv;100000]     /                         feed a file to a function in chunks of a given size"]
.help.i.r[`.Q.fps;".Q.fps[{show x};`:pipe]             /                         feed a named pipe to a function in chunks"]
.help.i.r[`.Q.fpn;".Q.fpn[{show x};`:pipe;100000]      /                         feed a named pipe in chunks of a given size"]
.help.i.r[`.Q.gc;".Q.gc[]                             /                         return unused memory to the operating system"]
.help.i.r[`.Q.w;".Q.w[]                              /                         report memory statistics as a readable dictionary"]
.help.i.r[`.Q.ts;".Q.ts[sum;enlist til 1000]          /                         apply a function and report the time and space it took"]
.help.i.r[`.Q.st;".Q.st[sum;enlist til 1000]          /                         apply a function and report its time and space in detail"]
.help.i.r[`.Q.prf0;".Q.prf0 .z.i                        /                         snapshot another q process's call stack"]
.help.i.r[`.Q.hg;".Q.hg `$\":http://localhost\"         /                         fetch a URL with HTTP GET"]
.help.i.r[`.Q.hp;".Q.hp[`$\":http://x\";.h.ty`json;\"{}\"] /                         post a body to a URL with HTTP POST"]
.help.i.r[`.Q.id;".Q.id `$\"a b\"                       / `ab                     sanitize a symbol, or a table's columns, into valid names"]
.help.i.r[`.Q.dd;".Q.dd[`a;`b]                        / `a.b                    glue two symbols together with a dot"]
.help.i.r[`.Q.j10;".Q.j10 \"hello\"                      / 561666408               encode a string as a long in base 36 with punctuation"]
.help.i.r[`.Q.x10;".Q.x10 .Q.j10 \"hello\"               / \"AAAAAhello\"            decode a base-36 long back to a string"]
.help.i.r[`.Q.j12;".Q.j12 `hello                       /                         encode a symbol as a long in base 36"]
.help.i.r[`.Q.x12;".Q.x12 .Q.j12 `hello                /                         decode a base-36 long back to a symbol"]
.help.i.r[`.Q.k;".Q.k                                /                         report the interpreter version number"]
.help.i.r[`.Q.K;".Q.K                                /                         report the interpreter version date"]
.help.i.r[`.Q.opt;".Q.opt .z.x                         /                         read command-line arguments as a dictionary"]
.help.i.r[`.Q.def;".Q.def[`p!enlist 80;()!()]          / (,`p)!,80               apply defaults and types to parsed command-line arguments"]
.help.i.r[`.Q.x;".Q.x                                / ()                      list the command-line arguments that were not options"]
.help.i.r[`.Q.res;".Q.res                              /                         list the reserved words and control words"]
.help.i.r[`.Q.s;".Q.s til 3                          / \"0 1 2\\n\"               format a value as the console would print it"]
.help.i.r[`.Q.s1;".Q.s1 til 3                         / \"0 1 2\"                 format a value on one line, as 0N! does"]
.help.i.r[`.Q.t;".Q.t                                / \" bg xhijefcspmdznuvts\" list the type character of every datatype, indexed by type"]
.help.i.r[`.Q.ty;".Q.ty 1 2 3                         / \"j\"                     report the type character of a value"]
.help.i.r[`.Q.V;".Q.V ([]a:1 2)                      / (,`a)!,1 2              take a table's columns as a dictionary"]
.help.i.r[`.Q.v;".Q.v `:trade                        /                         read a value from disk, or pass a value through"]
.help.i.r[`.Q.l;".Q.l `db                            /                         load a database directory as \\l does"]
.help.i.r[`.Q.lo;".Q.lo[`db;0;0]                      /                         load a database without changing directory or running scripts"]
.help.i.r[`.Q.li;".Q.li 2024.01.02                    /                         add partitions to the loaded database"]
.help.i.r[`.Q.ld;".Q.ld read0 `:script.q              /                         group a script's lines into evaluable statements"]
.help.i.r[`.Q.qt;".Q.qt ([]a:1 2)                     / 1b                      tell whether a value is a table"]
.help.i.r[`.Q.qp;".Q.qp trade                         /                         tell whether a table is partitioned"]
.help.i.r[`.Q.ps;".Q.ps[`trade;();();()]              /                         run a select over each partition in turn"]
.help.i.r[`.Q.p1;".Q.p1[`trade;2024.01.01;()]         /                         select from one partition, for .Q.ps"]
.help.i.r[`.Q.u;".Q.u                                /                         tell whether the partitions are date based"]
.help.i.r[`.Q.Cf;".Q.Cf                               /                         create an empty nested-column file (deprecated)"]
.help.i.r[`.Q.Xf;".Q.Xf[`j;`:f]                       /                         create an empty mapped file of a type (deprecated)"]
.help.i.r[`.z.a;".z.a                                /                         report the current connection's IP address as an int"]
.help.i.r[`.z.ac;".z.ac:{[rh] (1;\"\")}                 /                         hook that authorizes or authenticates an HTTP request"]
.help.i.r[`.z.b;".z.b                                /                         map each name a view depends on to its dependents"]
.help.i.r[`.z.bm;".z.bm:{[x] x}                       /                         hook that validates an incoming IPC message"]
.help.i.r[`.z.c;".z.c                                /                         count the physical cores of this machine"]
.help.i.r[`.z.d;".z.d                                /                         take today's UTC date"]
.help.i.r[`.z.D;".z.D                                /                         take today's local date"]
.help.i.r[`.z.e;".z.e                                /                         report the TLS details of the current connection"]
.help.i.r[`.z.ex;".z.ex                               /                         name the primitive that failed, in the debugger"]
.help.i.r[`.z.ey;".z.ey                               /                         take the argument the failed primitive was given"]
.help.i.r[`.z.exit;".z.exit:{[x] show \"bye\"}            /                         hook run as the process exits, given the exit code"]
.help.i.r[`.z.f;".z.f                                /                         name the script this process was started with"]
.help.i.r[`.z.H;".z.H                                /                         list the active socket handles"]
.help.i.r[`.z.h;".z.h                                /                         name the host this process runs on"]
.help.i.r[`.z.i;".z.i                                /                         report this process id"]
.help.i.r[`.z.K;".z.K                                /                         report the interpreter major version as a float"]
.help.i.r[`.z.k;".z.k                                /                         report the release date of the interpreter"]
.help.i.r[`.z.l;".z.l                                /                         list the license details"]
.help.i.r[`.z.M;".z.M                                /                         name the module namespace being loaded"]
.help.i.r[`.z.m;".z.m                                /                         take the module namespace being loaded"]
.help.i.r[`.z.N;".z.N                                /                         take the local time as a timespan"]
.help.i.r[`.z.n;".z.n                                /                         take the UTC time as a timespan"]
.help.i.r[`.z.o;".z.o                                /                         name the operating system this build was made for"]
.help.i.r[`.z.P;".z.P                                /                         take the local timestamp now"]
.help.i.r[`.z.p;".z.p                                /                         take the UTC timestamp now"]
.help.i.r[`.z.pc;".z.pc:{[h] show h}                  /                         hook run after a connection closes, given its handle"]
.help.i.r[`.z.pd;".z.pd:`u#hopen each 20000+til 4     /                         supply the handles peach spreads work across"]
.help.i.r[`.z.pg;".z.pg:{value x}                     /                         hook that answers a synchronous IPC request"]
.help.i.r[`.z.ph;".z.ph:{[x] .h.hy[`html;\"hi\"]}       /                         hook that answers an HTTP GET"]
.help.i.r[`.z.pi;".z.pi:{show value x}                /                         hook that evaluates a line of console input"]
.help.i.r[`.z.pm;".z.pm:{[x] show x}                  /                         hook that answers HTTP methods other than GET and POST"]
.help.i.r[`.z.po;".z.po:{[h] show h}                  /                         hook run after a connection opens, given its handle"]
.help.i.r[`.z.pp;".z.pp:{[x] .h.hy[`txt;\"ok\"]}        /                         hook that answers an HTTP POST"]
.help.i.r[`.z.pq;".z.pq:{value x}                     /                         hook that answers a qcon text-protocol request"]
.help.i.r[`.z.ps;".z.ps:{value x}                     /                         hook that runs an asynchronous IPC message"]
.help.i.r[`.z.pw;".z.pw:{[u;p] 1b}                    /                         hook that accepts or rejects a user and password"]
.help.i.r[`.z.q;".z.q                                /                         tell whether quiet mode is on"]
.help.i.r[`.z.r;".z.r                                /                         tell whether updates are blocked in this context"]
.help.i.r[`.z.s;".z.s                                /                         refer to the function currently running, for recursion"]
.help.i.r[`.z.T;".z.T                                /                         take the local time"]
.help.i.r[`.z.t;".z.t                                /                         take the UTC time"]
.help.i.r[`.z.ts;".z.ts:{[x] show x}                  /                         hook run on every timer tick"]
.help.i.r[`.z.u;".z.u                                /                         name the user of the current handle"]
.help.i.r[`.z.vs;".z.vs:{[n;i] show n}                /                         hook run after a global is amended"]
.help.i.r[`.z.W;".z.W                                /                         report the bytes queued on each outbound handle"]
.help.i.r[`.z.w;".z.w                                /                         report the current handle, zero in the console"]
.help.i.r[`.z.wc;".z.wc:{[h] show h}                  /                         hook run after a websocket closes"]
.help.i.r[`.z.wo;".z.wo:{[h] show h}                  /                         hook run after a websocket opens"]
.help.i.r[`.z.ws;".z.ws:{[x] neg[.z.w] x}             /                         hook that answers a websocket message"]
.help.i.r[`.z.X;".z.X                                /                         list the raw command line, unfiltered"]
.help.i.r[`.z.x;".z.x                                /                         list the command-line arguments after the script"]
.help.i.r[`.z.Z;".z.Z                                /                         take the local datetime now"]
.help.i.r[`.z.z;".z.z                                /                         take the UTC datetime now"]
.help.i.r[`.z.zd;".z.zd:17 2 6                        /                         set the default compression: block size, algorithm, level"]
.help.i.r[`.h.br;".h.br                               / \"<br>\"                  name the HTML line break"]
.help.i.r[`.h.c0;".h.c0                               / `024C7E                 name the first colour the web console uses"]
.help.i.r[`.h.c1;".h.c1                               / `958600                 name the second colour the web console uses"]
.help.i.r[`.h.cd;".h.cd ([]a:1 2;b:3 4)               / (\"a,b\";\"1,3\";\"2,4\")     render a table as comma-separated lines"]
.help.i.r[`.h.code;".h.code \"a\\tb\"                      /                         render tab-separated text as an HTML table row"]
.help.i.r[`.h.d;".h.d                                / \" \"                     name the delimiter .h.cd joins nested items with"]
.help.i.r[`.h.ed;".h.ed ([]a:1 2)                     /                         render a table as an Excel XML workbook"]
.help.i.r[`.h.edsn;".h.edsn ([]a:1 2)                   /                         render an Excel workbook from several tables"]
.help.i.r[`.h.fram;".h.fram[`title;(`a;`b)]             /                         build an HTML page of two frames"]
.help.i.r[`.h.ha;".h.ha[`$\"/a\";\"link\"]                / \"<a href=/a>link</a>\"   build an HTML anchor"]
.help.i.r[`.h.hb;".h.hb[`$\"/a\";\"link\"]                /                         build an HTML anchor that opens in a named target"]
.help.i.r[`.h.hc;".h.hc \"a<b\"                         / \"a&lt;b\"                escape the angle brackets in text"]
.help.i.r[`.h.he;".h.he \"a<b\"                         /                         escape text and mark it as an XML entity"]
.help.i.r[`.h.hn;".h.hn[\"200 OK\";`txt;\"hi\"]           /                         build a complete HTTP response"]
.help.i.r[`.h.hp;".h.hp enlist \"hi\"                   /                         build an HTTP response holding pre-formatted text"]
.help.i.r[`.h.hr;".h.hr \"abcd\"                        / \"----\"                  build a rule of dashes as long as the text"]
.help.i.r[`.h.ht;".h.ht `index                        /                         generate HTML documentation from a q script"]
.help.i.r[`.h.hta;".h.hta[`a;(1#`href)!1#\"/\"]          / \"<a href=\\\"/\\\">\"        build an HTML start tag with attributes"]
.help.i.r[`.h.htac;".h.htac[`a;(1#`href)!1#\"/\";\"x\"]     / \"<a href=\\\"/\\\">x</a>\"   build an HTML element with attributes and content"]
.help.i.r[`.h.htc;".h.htc[`b;\"bold\"]                   / \"<b>bold</b>\"           build an HTML element around some content"]
.help.i.r[`.h.html;".h.html \"hi\"                        /                         wrap body text as a whole HTML page"]
.help.i.r[`.h.http;".h.http \"see http://x.com\"          /                         turn bare URLs in text into HTML links"]
.help.i.r[`.h.hu;".h.hu \"a b\"                         / \"a%20b\"                 escape text for use in a URL"]
.help.i.r[`.h.hug;".h.hug .h.sc                        /                         build the escape table .h.hu uses"]
.help.i.r[`.h.hy;".h.hy[`json;\"{}\"]                   /                         build an HTTP response of a given content type"]
.help.i.r[`.h.HOME;".h.HOME                             / \"html\"                  name the directory the web server serves files from"]
.help.i.r[`.h.iso8601;".h.iso8601 3600000000000            /                         render nanoseconds since 2000 as an ISO 8601 time"]
.help.i.r[`.h.jx;".h.jx[0;`t]                         /                         render one page of a table as HTML rows"]
.help.i.r[`.h.ka;".h.ka 5000i                         / \"keep-alive\"            give the HTTP keep-alive header for an idle timeout"]
.help.i.r[`.h.logo;".h.logo                             /                         name the HTML for the logo the web console shows"]
.help.i.r[`.h.nbr;".h.nbr \"a b\"                        / \"<nobr>a b</nobr>\"      wrap text so it will not break across lines"]
.help.i.r[`.h.pre;".h.pre (\"a\";\"b\")                    / \"<pre>a\\nb\\n</pre>\"     join lines as pre-formatted HTML text"]
.help.i.r[`.h.sa;".h.sa                               /                         name the CSS the web console styles anchors with"]
.help.i.r[`.h.sb;".h.sb                               /                         name the CSS the web console styles the body with"]
.help.i.r[`.h.sc;".h.sc                               /                         name the characters that need no escaping in a URL"]
.help.i.r[`.h.td;".h.td ([]a:1 2;b:3 4)               / (\"a\\tb\";\"1\\t3\";\"2\\t4\")  render a table as tab-separated lines"]
.help.i.r[`.h.text;".h.text (\"a\";\"b\")                   / \"<p>a</p>\\n<p>b</p>\\n\"  render lines as HTML text nodes"]
.help.i.r[`.h.tx;".h.tx                               /                         map each file type to the function that renders it"]
.help.i.r[`.h.ty;".h.ty                               /                         map each content type to its media type"]
.help.i.r[`.h.uh;".h.uh \"a%20b\"                       / \"a b\"                   decode the percent escapes in a URL"]
.help.i.r[`.h.val;".h.val \"2+3\"                        / 5                       evaluate a request, as .z.ph does"]
.help.i.r[`.h.xd;".h.xd ([]a:1 2)                     /                         render a table as an XML table"]
.help.i.r[`.h.xmp;".h.xmp (\"a\";\"b\")                    / \"<xmp>a\\nb\\n</xmp>\"     render lines inside an HTML xmp element"]
.help.i.r[`.h.xs;".h.xs \"a<b\"                         / \"a&lt;b\"                escape text for XML"]
.help.i.r[`.h.xt;".h.xt[`json;enlist\"{}\"]             / +(`symbol$())!()        parse text of a given type into a value"]
.help.i.r[`.j.j;".j.j `a`b!1 2                       / \"{\\\"a\\\":1,\\\"b\\\":2}\"     render a value as JSON text"]
.help.i.r[`.j.k;".j.k \"[1,2,3]\"                      / 1 2 3f                  parse JSON text into a value"]
.help.i.r[`.j.jd;".j.jd (1b;0w)                       /                         render JSON, choosing how to write infinities"]
.help.i.r[`$"0:";"(\"II\";\",\") 0: \"1,2\"                 / (1i;2i)                 parse delimited text into typed columns\n`:t.csv 0: csv 0: ([]a:1 2)         /                         write text lines to a file\n(\"SI\";4 3)0:\"abc 123\"               / (,`abc;,123i)           parse fixed-width fields into typed columns"]
.help.i.r[`$"1:";"(enlist\"j\";enlist 8)1:8#0x00        / ,,0                     parse fixed-width binary into typed values\n`:t.bin 1: 0x2a                     /                         write raw bytes to a file"]
.help.i.r[`$"2:";"`f 2:(`fname;1)                     /                         load a function from a shared library"]
.help.i.r[`$"1";"1 \"hello\\n\"                         / 1                       write text to stdout"]
.help.i.r[`$"2";"2 \"oops\\n\"                          / 2                       write text to stderr"]
.help.i.r[`$"+";"1 2 3+10                            / 11 12 13                add, item by item, spreading an atom over a list"]
.help.i.r[`$"-";"5-2                                 / 3                       subtract"]
.help.i.r[`$"*";"2*3                                 / 6                       multiply"]
.help.i.r[`$"%";"7%2                                 / 3.5                     divide, always giving a float"]
.help.i.r[`$"=";"1 2 3=2                             / 010b                    test equality, item by item"]
.help.i.r[`$"<>";"1 2 3<>2                            / 101b                    test inequality, item by item"]
.help.i.r[`$"<";"2<3                                 / 1b                      test whether x is less than y"]
.help.i.r[`$">";"3>2                                 / 1b                      test whether x is greater than y"]
.help.i.r[`$"<=";"2<=3                                / 1b                      test whether x is at most y"]
.help.i.r[`$">=";"3>=2                                / 1b                      test whether x is at least y"]
.help.i.r[`$"&";"2&3                                 / 2                       take the lesser of two values - logical AND on booleans"]
.help.i.r[`$"|";"2|3                                 / 3                       take the greater of two values - logical OR on booleans"]
.help.i.r[`$"^";"0^1 0N 3                            / 1 0 3                   fill the nulls in y with x\n([]a:1 0N)^([]a:0N 2)               /                         coalesce two tables, taking non-null values from y"]
.help.i.r[`$",";"1 2,3 4                             / 1 2 3 4                 join values into one list\n([]a:1 2),([]a:3 4)                 / +(,`a)!,1 2 3 4         append the rows of two matching tables"]
.help.i.r[`$"~";"1 2~1 2                             / 1b                      test whether two values match exactly, type included"]
.help.i.r[`$"#";"3#til 10                            / 0 1 2                   take the first x items, recycling the list if needed\n-2#til 10                           / 8 9                     take the last items, when x is negative\n2 3#til 6                           / (0 1 2;3 4 5)           reshape a list into x rows of y\n`a`c#`a`b`c!1 2 3                   / `a`c!1 3                keep only the named entries of a dictionary"]
.help.i.r[`$"_";"2_til 6                             / 2 3 4 5                 drop the first x items\n-2_til 6                            / 0 1 2 3                 drop the last items, when x is negative\n1 3_til 6                           / (1 2;3 4 5)             cut a list at each of the given indices\n`a _ `a`b!1 2                       / (,`b)!,2                drop the named entries from a dictionary\n(til 6)_2                           / 0 1 3 4 5               drop the item at one index"]
.help.i.r[`$"$";"\"J\"$\"42\"                            / 42                      cast a value, or parse text, to a type\n$[1b;\"yes\";\"no\"]                    / \"yes\"                   choose between expressions - the conditional\n10$\"abc\"                            / \"abc       \"            pad text to a fixed width, on the right\n`sym$`a                             /                         enumerate symbols against a domain\n(2 2#1 0 0 1f)$1 2f                 / 1 2f                    multiply a matrix by a matrix or vector"]
.help.i.r[`$"?";"1 2 3?2                             / 1                       find where a value first occurs, or the count if absent\n5?10                                /                         pick items at random, or deal x distinct ones\n?[([]a:1 2);();0b;(1#`a)!1#`a]      / +(,`a)!,1 2             query a table, as select does\n?[1 0 1b;`a;`b]                     / `a`b`a                  choose item by item - the vector conditional"]
.help.i.r[`$"!";"`a`b!1 2                            / `a`b!1 2                make a dictionary from keys and values\n![([]a:1 2;b:3 4);();0b;1#`a]       / +(,`b)!,3 4             update or delete on a table, as update does\n0N!til 3                            / 0 1 2                   print a value and return it, for tracing"]
.help.i.r[`$"@";"9 8 7@1 2                           / 8 7                     index a list, or apply a function\nv:1 2 3;@[v;1;:;9]                  / 1 9 3                   amend an item at an index\nr:@[{1+x};`a;{\"err\"}]               / \"err\"                   apply a function and catch any error"]
.help.i.r[`.;"(1 2;3 4) . 1 1                     / 4                       index at depth, one index per level\n.[+;(2;3)]                          / 5                       apply a function to a list of arguments\n.[(1 2;3 4);(0;1);:;9]              / (1 9;3 4)               amend at depth\n.[{x+y};(1;2);{\"err\"}]              / 3                       apply a function of any rank and catch any error"]
.help.i.r[`$":";"a:42                                / 42                      bind a value to a name\nf:{:x*2}                            / {:x*2}                  return from a function early"]
.help.i.r[`$"::";"a:1;v::a+1                          / 2                       define a view, recomputed when its inputs change\na::42                               / ::                      assign a global from inside a function\n(::)                                / ::                      name the identity function, and the generic null"]
.help.i.r[`$"'";",'[1 2;3 4]                         / (1 3;2 4)               pair up the arguments item by item - each"]
.help.i.r[`$"':";"(-':)1 4 9                          / 1 3 5                   apply to each item and the one before - each prior"]
.help.i.r[`$"/:";"1 2 3+/:10 20                       / (11 12 13;21 22 23)     apply to each item on the right - each right"]
.help.i.r[`$"\\:";"10 20+\\:1 2 3                       / (11 12 13;21 22 23)     apply to each item on the left - each left"]
.help.i.r[`$"/";"(+/)1 2 3                           / 6                       reduce a list to one value - over\n10+/1 2 3                           / 16                      reduce from a starting value\n{x*2}/[3;1]                         / 8                       apply a function x times, or until it converges"]
.help.i.r[`$"\\";"(+\\)1 2 3                           / 1 3 6                   accumulate along a list, keeping every step - scan\n10+\\1 2 3                           / 11 13 16                accumulate from a starting value"]
/ <<< end generated
