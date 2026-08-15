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
/ A PAGE (see the pages block at the foot of this file) never fetches: its
/ member table IS the answer and the website is one static pointer line, so
/ the whole page tier is offline by construction.
.help.i.ladder:{[s] n:`$s;
  loc:$[n in exec fullname from .help.funcs;.help.get n;""];
  p:.help.i.canon s;
  $[null p;(loc;.help.webfetch s);("\n" sv (enlist $[count loc;loc;s]),.help.i.pagetext p;"")]}

/ `""`, `(::)` and the niladic `.help.show[]` all mean "show me the index".
.help.i.blank:{[x] $[x~(::);1b;x~`;1b;10h=abs type x;0=count x;0b]}

/ the help ladder as a VALUE (the IPC-friendly form), returning EVERYTHING the
/ print form shows: the index for an empty pattern; else the local page for an
/ exact captured name — with a page's blurb and member table appended — joined
/ with the online page for an exact index topic; when both miss, .help.find —
/ exactly one documented match answers ITS page, else the matching rows come
/ back as a table to narrow by (empty = no match).
/ @param pattern (symbol|string) a name, or a pattern as .help.find takes it
/ @return (string|table) the help text, or the matching doc rows
.help.text:{[pattern]
  if[.help.i.blank pattern;:.help.i.index[]];
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

/ THE printing door (`?`), and it returns null: the local page prints plainly, a
/ fetched page as the gutter preview (.help.i.page), the find fallback as its
/ page or table.  `.help.text` is the VALUE ladder — one contract per name, so
/ a caller never has to guess whether it printed or answered.
/ @param pattern (symbol|string) a name, or a pattern as .help.find takes it
.help.show:{[pattern]
  if[.help.i.blank pattern;-1 .help.i.index[];:(::)];
  s:.help.i.str pattern;
  lw:.help.i.ladder s;
  if[count first lw;-1 first lw];
  if[count last lw;-1 .help.i.page[s;last lw]];
  if[0=sum count each lw;
    r:.help.find pattern;
    $[1=count m:distinct r`fullname;-1 .help.get first m;show r]];}

/ the OTHER printing door (`??`): the full unclipped ladder, plain text, no
/ gutter, no preview.  Kept separate from .help.show because the two spellings
/ mean different things at the prompt; both print, neither returns.
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
  / `null` is a reserved word, so a table LITERAL cannot name that column — built from data instead
  flip `n`c`name`sz`literal`null`inf`sql!(n;"*",.Q.t 1_n;
    `list`boolean`guid`byte`short`int`long`real`float`char`symbol`timestamp`month`date`datetime`timespan`minute`second`time;
    0N 1 16 1 2 4 8 4 8 1 0N 8 4 4 8 8 4 4 4;
    ("";"0b";"";"0x00";"0h";"0i";"0j";"0e";"0.0";"\" \"";"`";"dateDtimespan";"2000.01m";"2000.01.01";"dateTtime";"00:00:00.000000000";"00:00";"00:00:00";"00:00:00.000");
    enlist[""],-3!'nul;
    @[count[n]#enlist"";n?abs type each inf;:;-3!'inf];
    ("";"";"";"";"smallint";"int";"bigint";"real";"float";"";"varchar";"";"";"date";"timestamp";"";"";"";"time"))}

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

/ the search terms of a pattern: whitespace splits it, each word becoming a
/ `*word*` substring glob.  No word yields the one `*` an empty pattern has
/ always meant.
/ @param pattern (string) the lowercased pattern
/ @return (list) one glob per term
.help.i.globs:{[pattern]
  w:@[pattern;where pattern in "\t\r\n";:;" "];
  ts:ts where 0<count each ts:" " vs w;
  {p:"*",x,"*";p where not (p="*")and"*"=next p} each $[count ts;ts;enlist ""]}

/ the doc rows whose fullname, tag, param or description match — globs,
/ matched anywhere and case-insensitively, so a bare word is a substring
/ search.  MANY WORDS ARE AN AND: a two-word query wants the rows answering to
/ both, which no one contiguous phrase finds.  NAMES FIRST: a name hit is what
/ the user asked for where a description hit is a guess, and each matched name
/ collapses to the ONE row that stands for it (its lead line, which is how
/ .help.args orders a name's rows) so ?str lists the .str functions, not their
/ tags.  The NAME half wants every term in the name and skips `.i.` privates,
/ as .help.i.nsmembers does; the other half takes each term in ANY field, the
/ name included, so one term can name a row the other describes and prose
/ naming a private still finds it.
/ @param pattern (string|symbol) the pattern
/ @return (table) the matching rows of .help.args, name matches first
.help.find:{[pattern]
  ps:.help.i.globs lower .help.i.str pattern;
  t:.help.args;
  if[0=count t;:t];
  nm:exec fullname from .help.funcs where all lower[string fullname] like/:ps, not fullname like "*.i.*";
  n:0!select first tag,first param,first description by fullname from t where fullname in nm;
  fs:(lower each string t`fullname;lower each string t`tag;lower each string t`param;lower each t`description);
  d:t where all {[fs;p] any fs like\:p}[fs] each ps;
  n,d where not d[`fullname] in nm}

/ >>> GENERATED from lib/help-builtins.tsv by `python3 tools/gen-help-builtins.py`
/ >>> edit the TSV, rerun (it splices this block in place), commit both.
/ >>> block deliberately EMPTY (owner 2026-08-14): the 459 registrations cost ~6s of every `\l pq`; rerun the gen to restore.
/ <<< end generated

/ ---- pages -----------------------------------------------------------------
/ A PAGE is a curated blurb over a member filter, and an ENTRY like any other
/ name: .help.find matches it and .help.oneline answers it.  NAMES BEAT PAGES —
/ an exact documented name always renders its own stack and the page's member
/ table is APPENDED below it.  A PAGE NAME NEVER COLLIDES with a documented name
/ (owner ruling, 2026-08-14): `table` is the page, `tables` stays the keyword.
/ .help.i.pr enforces it defensively — it will not overwrite a documented name —
/ but the spelling, not the guard, is what keeps the two apart.
/ Empty members means "filter by this page's own name as a namespace prefix",
/ which is also what any undeclared namespace with documented members gets.
/ Empty summary means the page's BODY is its entry (`started`: its rows already
/ lead with the page line), so the entry, not this table, is that summary's home.
/ webtopic is the NEAREST ONLINE topic, and ` where none is close enough.  A URL
/ is NEVER built from a page name: our names are ours (`types` is the site's
/ `datatypes`, and `?types` used to point at a 404).  No mapping, no pointer.
.help.i.pages:([page:`started`.z`.Q`.h`.j`adverbs`syscmds`cmdline`types`math`joins`strings`temporal`table]
 summary:(
  "";
  "session and environment callbacks";
  "database and utility toolkit";
  "HTTP, markup and text helpers";
  "JSON serialize and deserialize";
  "iterators: ' /: \\: ': / \\";
  "the \\ system commands";
  "q command-line flags";
  "the datatype reference table";
  "arithmetic, statistics and rounding";
  "as-of, equi, left and union joins";
  "text: search, case, trim, split";
  "dates, times and calendar arithmetic";
  "tables and the qsql verbs: ?tables is the keyword");
 webtopic:``dotz`dotq`doth`dotj`iterators`syscmds`cmdline`datatypes`math`joins``datatypes`qsql;
 blurb:(
  ();();();();();
  ("an iterator modifies a verb: each item, each pair, each left, each right";"/ folds to one value, \\ keeps every step");
  ("a \\ line is a command, not an expression; system \"c 25 200\" is its q form");
  ("peachq honours the flags below; the other kdb+ flags are not implemented yet";"everything after the script name reaches the script as .z.x");
  ("n is the type number and c the .Q.t character; a vector is n, an atom -n";"sz is bytes per item; sql is the nearest ANSI SQL type");
  ("atomic verbs spread over a whole list; aggregates collapse one to a value";"an m- prefix is a moving window, an s- prefix a sample statistic");
  ("aj is the as-of join: the last y row at or before each x time";"lj ij uj pj match on the RIGHT table's key columns");
  ("a string is a char vector, so every list verb works on it";"peachq adds a python-shaped text namespace: ?.str");
  ("temporal types are numbers: add a long to a date, subtract two timestamps";"?types has the literals, the nulls and the infinities");
  ("a table is a flipped dictionary of equal-length named columns";"the functional forms of select and update are ?[t;..] and ![t;..]"));
 members:(
  `$();`$();`$();`$();`$();
  (`each`peach`over`scan`prior),`$("'";"':";"/:";"\\:";"/";"\\");
  `$"\\",/:("a";"b";"B";"c";"C";"cd";"d";"e";"E";"f";"g";"l";"o";"p";"P";"r";"s";"S";"t";"T";"ts";"u";"v";"w";"W";"x";"z";"1";"2";"_";"\\");
  `$("-p";"-q";"-u";"-U";"-E";"-classic");
  `$();
  `abs`neg`signum`sqrt`exp`log`xexp`xlog`floor`ceiling`div`mod`sum`sums`prd`prds`avg`avgs`max`min`maxs`mins`med`dev`var`sdev`svar`cor`cov`deltas`ratios`within`rand`mmu;
  (`aj`aj0`ajf`ajf0`asof`ej`ij`ijf`lj`ljf`pj`uj`ujf`wj`wj1),`$(",";"^");
  (`like`lower`upper`trim`ltrim`rtrim`ss`ssr`string`vs`sv`md5),`$("$";"0:");
  (`gtime`ltime`xbar`.Q.addmonths`.z.p`.z.P`.z.d`.z.D`.z.t`.z.T`.z.z`.z.Z),`$("\\W";"\\z");
  `select`exec`update`delete`from`fby`cols`keys`xcol`xcols`xkey`xasc`xdesc`xgroup`ungroup`meta`tables`insert`upsert`csv`fkeys`flip`key`.Q.en`.Q.id))

/ both spellings answer; one page renders.
.help.i.alias:`iterators`tutorial!`adverbs`started
.help.i.pagenames:key[.help.i.pages]`page

/ documented fullnames under a namespace; `.i.` privates are not public surface.
.help.i.nsmembers:{[ns] p:(string ns),".";
  exec fullname from .help.funcs where (fullname like p,"*"), not fullname like "*.i.*"}

/ a topic -> the page it names, or ` : curated page, alias, or ANY namespace
/ that has documented members (so .pq, .str and a user's own come for free).
.help.i.canon:{[s] n:`$s;
  n:$[n in key .help.i.alias;.help.i.alias n;n];
  $[n in .help.i.pagenames;n;(1<count s)and("."=first s)and count .help.i.nsmembers n;n;`]}

/ THE column rhythm, byte-identical to tools/gen-help-builtins.py's layout.
.help.i.line:{[call;meaning] $[36>count call;36$call;call," "],"/ ",(24$""),meaning}

.help.i.pagemem:{[p] m:$[p in .help.i.pagenames;.help.i.pages[p;`members];`$()];
  $[count m;m;"."=first string p;.help.i.nsmembers p;`$()]}

/ one member row.  A builtin one-liner already names itself in its call column
/ (its .help.funcs line is null); a CAPTURED doc comment is prose, so the name
/ becomes its call — otherwise a namespace page lists sentences with no names.
.help.i.memline:{[n] o:.help.oneline n;
  $[null .help.funcs[n;`line];o;.help.i.line[string n;o]]}

/ what an exact page match ADDS below the name's own entry line: the curated
/ blurb, the member table (each member's dominant meaning), and at most ONE
/ static pointer at the page's mapped online topic.  A page NEVER fetches — the
/ pointer is string-built, and the index is only CONSULTED when a previous
/ lookup already cached it, never fetched to check.
.help.i.pagetext:{[p]
  b:$[p in .help.i.pagenames;.help.i.pages[p;`blurb];()];
  m:$[p~`types;"\n" vs .help.i.rstrip .Q.s .help.types[];.help.i.memline each .help.i.pagemem p];
  r:("  ",/:b),($[(count b)and count m;enlist"";()]),"  ",/:m;
  w:$[p in .help.i.pagenames;.help.i.pages[p;`webtopic];`];
  if[null w;:r];
  if[not ()~.help.i.ix;if[not any (string w)~/:.help.i.ix`qname;:r]];   / `and` would index the uncached ()
  $[count .help.url;r,enlist"  more: ??",(string w)," or ",.help.url,"help?q=",.h.hu string w;r]}

/ the index (bare `?`): the tutorial first, every row pasteable, `· page`
/ marking a directory.  Every listed page HAS an entry, so its row is that
/ entry's own line — one home per summary.  Layout is prose: order lives here.
.help.i.index:{[]
  row:{[p] "  ",.help.oneline p};
  "\n" sv (enlist "peachq help · one line per meaning · ?name shows it · ??name shows it in full"),
   (enlist row`started),
   (enlist "  ",.help.i.line["?til";"try any name: ?max  ?.Q.en  ?$  ?'type  ?-p"]),
   (row each `.z`.Q`.h`.j`adverbs`syscmds`cmdline`types),
   enlist "  ",(count[.help.i.line["";""]]$"?math  ?joins  ?strings  ?temporal  ?table"),"topic pages"}

/ page ENTRY rows, rendered from the registry summary.  Runs AFTER the generated
/ block and NEVER overwrites an already-documented name — the defensive half of
/ the no-collision rule, and what lets a page whose body IS its entry (`started`)
/ be skipped here and carry its own summary.
.help.i.pr:{[name;page] if[not name in exec fullname from .help.funcs;
  .help.i.r[name;.help.i.line["?",string name;.help.i.pages[page;`summary]," · page"]]];}
.help.i.pr'[.help.i.pagenames;.help.i.pagenames];
.help.i.pr'[key .help.i.alias;value .help.i.alias];
