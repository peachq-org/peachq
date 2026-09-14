/ help.q - the .help doc store: every doc comment the engine saw, queryable.
/ PURE q by owner ruling - no C implements the STORE; C only classifies comment
/ runs, calls the two hooks, and delivers the deferred builtin bundle behind the
/ .help.i.loaddb native.  Re-pointing any of them is NOT a supported contract.
/ Schema after qstudio's man.q.  Headers are parsed ON INGEST - only the
/ structured rows are stored, so a changed parse rule means "reload the file".
/ ALWAYS-ON: the core bootstrap loads this file (q_runtime.c), last of the
/ ordered list, so the hooks are bound before the first file a user loads -
/ capture is a listener, and a late listener has already missed its events.
/ The bootstrap itself is NOT listened to: q_comment.c is inert over the core
/ list, so nothing in this file (or q.q/.h/.j) is captured and the store is
/ EMPTY after boot - this file's own public names are hand-written rows in
/ lib/help-db.q, file-less and line-less like every other builtin.  That whole
/ file, page entries included, is deferred: FIRST HELP ACCESS is the one thing
/ that loads it (owner 2026-09-03) - not boot, not `\l pq`.  Every READER
/ calls .help.i.loaddb (get/text/find/i.index/show/full); .help.oneline does
/ NOT, because it fires on every line typed and must never pull in a load as
/ a side effect of typing.  So the REPL hint for a builtin is dark until the
/ session's first real lookup, and live after it.
/ THE C CONTRACT - renaming one of these five BREAKS C:
/   .help.show / .help.full                q_sys.c      `\?topic` / `\??topic`
/   .help.oneline                          q_repl.c     prompt hint, every line
/   .help.register_file / _definition      q_comment.c  the two capture hooks
.help.funcs:([fullname:`$()] ns:`$(); file:`$(); line:`long$())
.help.files:([file:`$()] ns:`$())
.help.filetags:([] file:`$(); tag:`$(); val:())

/ .help.args is DERIVED, never written: register_definition assigns one name's
/ whole sub-table into .help.i.argsd and bumps the counter, and .help.args[]
/ re-razes only when the counter has moved.  Delete-then-insert into a flat
/ table rebuilt all four columns per call - 1694ms over 459 registrations
/ against 170ms here.  argsp is the TYPED empty seed: `raze value ()!()` is a
/ general list, which every `select from .help.args` would signal 'type on.
.help.i.argsp:([] fullname:`$(); tag:`$(); param:`$(); description:())
.help.i.argsd:()!()
.help.i.counter:0
.help.i.argsg:0
.help.i.argsc:.help.i.argsp
.help.args:{[] if[not .help.i.argsg=.help.i.counter;
    .help.i.argsc::.help.i.argsp,raze value .help.i.argsd;
    .help.i.argsg::.help.i.counter];
  .help.i.argsc}

.help.i.str:{$[10h=type x;x;-10h=type x;enlist x;string x]}

/ NOT built-in trim/rtrim, deliberately: trim strips the char null (" ") only,
/ and @tag parsing wants " \t\n\r" so tab-indented tags still parse.
.help.i.rstrip:{$[count i:where not x in " \t\n\r";(1+last i)#x;""]}
.help.i.strip:{.help.i.rstrip $[count i:where not x in " \t\n\r";(first i)_x;""]}

/ the slice is spelled #/_ , not index-by-til: `til` costs a flat ~0.6ms a call
/ here (PLAN.md defect), and this runs several times per captured doc comment.
.help.i.join:{[lines] $[count i:where 0<count each lines;"\n" sv (first i) _ (1+last i)#lines;""]}

/ header -> rows (tag;param;description;val), the lead description first under
/ a null tag: the lead is EVERY line before the first tag, a non-tag line
/ after a tag continues THAT tag, and a bare @ is text, never a tag - so it
/ cannot collide with the lead row's null tag.  A NAME is split off for @param
/ and @exception only; every other tag, known or invented, is kept under its
/ own name with its text unread - no whitelist, so no tag is ever dropped.
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

/ begin this file - UPSERT-ONLY: the store mirrors the SESSION, and `\l` never
/ removes a definition, so `file` is provenance, never a key to clear over
/ (definition rows replace per-fullname in register_definition).  This call
/ replaces only what it re-inserts itself; with an empty header (every script
/ begin) it is a pure no-op.
.help.register_file:{[file;ns;header]
  if[count header;
    fl:file;
    delete from `.help.filetags where file=fl;
    `.help.files upsert (file;ns);
    `.help.filetags insert .help.i.filetags[file;header]]; }

/ one documented definition; re-registering replaces its own rows only - the
/ dict assign IS that replacement, and it keeps the name's original position
/ (delete-then-insert moved a re-registered name to the end).
.help.register_definition:{[fullname;ns;file;line;header]
  `.help.funcs upsert (fullname;ns;file;line);
  .help.i.argsd,:enlist[fullname]!enlist flip `fullname`tag`param`description!.help.i.args[fullname;header];
  .help.i.counter::1+.help.i.counter; }

/ one name's documentation as text - its description, then its tags.
.help.get:{[name]
  .help.i.loaddb[];
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
  r:select tag,param,description from .help.args[] where fullname=n;
  $[count r;"\n" sv render[n;r];"no documentation for ",string n]}

/ the online docs base; "" disables the whole web tier (offline / tests).
.help.url:"https://peachq.org/"
.help.i.ix:()

/ GET one path off .help.url - the ONE trapped door to the network the web
/ tier uses; "" on any failure or when the tier is disabled.
.help.i.get:{[path] $[count .help.url;@[{.Q.hg x};.help.url,path;{""}];""]}

/ the online topic index (help.csv: pagepath,qname,kind), fetched on FIRST
/ use and cached for the session; offline caches as the empty table - reset
/ with .help.i.ix:() to retry after gaining net access.
.help.index:{[]
  if[()~.help.i.ix;
    r:.help.i.get"help.csv";
    .help.i.ix::$[count r;("**S";enlist",")0:"\n"vs .help.i.rstrip r;([]pagepath:();qname:();kind:`$())]];
  .help.i.ix}

/ the reference page (markdown) for one online-index topic; "" when offline,
/ disabled or not an index qname.
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
/ exact captured name - with a page's blurb and member table appended - joined
/ with the online page for an exact index topic; when both miss, .help.find -
/ exactly one documented match answers ITS page, else the matching rows come
/ back as a table to narrow by (empty = no match).
.help.text:{[pattern]
  .help.i.loaddb[];
  if[.help.i.blank pattern;:.help.i.index[]];
  lw:.help.i.ladder .help.i.str pattern;
  t:lw where 0<count each lw;
  if[count t;:"\n" sv t];
  r:.help.find pattern;
  $[1=count m:distinct r`fullname;.help.get first m;r]}

/ the effective console (rows;cols): `\c`, its auto (`0N`) axes filled from
/ the live terminal (.help.i.termsize, a C native bound at boot).
.help.i.csize:{[] (2#.help.i.termsize[])^system"c"}

/ lines clipped to the console width with the console's own `..` mark, so no
/ printed help line ever wraps.
.help.i.clip:{[ls] w:0|.help.i.csize[][1]-3; {[w;l]$[w<count l;((0|w-2)#l),"..";l]}[w] each ls}

/ render a fetched page for the console behind a `│ ` gutter, obeying the
/ effective `\c` - its rows bound the preview, its cols clip each line
/ (console `..` rule) - ending in a `.. N more` pointer (\??topic / the
/ website show everything).  ```q/```syntax block bodies get one tint, fence
/ lines the gutter grey.  .pq.termsize fills auto (`0N`) `\c` axes and
/ .pq.cancolor gates ALL the ANSI - both soft by-name calls, so a host
/ without .pq degrades to a plain 25x80 preview.
.help.i.page:{[topic;md]
  ls:"\n" vs .help.i.rstrip md;
  c:.help.i.csize[];
  n:count ls;
  ls:.help.i.clip (n&c 0)#ls;
  fen:ls like\:"```*";
  qf:{(x like "```q*")or x like "```syntax*"}each ls;
  st:{[s;f]$[f 0;$[s 0;00b;1b,f 1];s]}\[00b;fen,'qf];
  cc:@[{value[x][]};`.pq.cancolor;0b];
  if[cc;ls:?[fen;{"\033[90m",x,"\033[0m"}each ls;
              ?[st[;0]&st[;1]&not fen;{"\033[36m",x,"\033[0m"}each ls;ls]]];
  out:$[cc;"\033[90m│ \033[0m";"│ "],/:ls;
  if[n>c 0;
    x:".. ",string[n-c 0]," more lines: \\??",topic,"  or  ",.help.url,"help?q=",.h.hu topic;
    out,:enlist $[cc;"\033[90m",x,"\033[0m";x]];
  "\n" sv out}

/ print text clipped to the console width; a table prints as itself.
.help.i.out:{[r] $[10h=type r;-1 "\n" sv .help.i.clip "\n" vs r;show r];}

.help.i.show:{[pattern]
  .help.i.loaddb[];
  if[.help.i.blank pattern;:.help.i.out .help.i.index[]];
  s:.help.i.str pattern;
  lw:.help.i.ladder s;
  if[count first lw;.help.i.out first lw];
  if[count last lw;-1 .help.i.page[s;last lw]];
  if[0=sum count each lw;
    r:.help.find pattern;
    $[1=count m:distinct r`fullname;.help.i.out .help.get first m;show r]];}

/ THE printing door (`?`), and it returns null: the local page prints plainly, a
/ fetched page as the gutter preview (.help.i.page), the find fallback as its
/ page or table.  `.help.text` is the VALUE ladder - one contract per name, so
/ a caller never has to guess whether it printed or answered.  HELP NEVER
/ ERRORS: every line prints clipped to the console width, and a failure
/ anywhere prints one line and answers null.
.help.show:{[pattern] @[.help.i.show;pattern;{[e] -1 "help: ",e;}];}

/ the OTHER printing door (`??`): the full ladder, plain text, no gutter, no
/ preview.  Kept separate from .help.show because the two spellings mean
/ different things at the prompt; both print, neither returns.
.help.full:{[pattern] @[{[p] .help.i.loaddb[]; .help.i.out .help.text p};pattern;{[e] -1 "help: ",e;}];}

/ the datatype reference, transcribed from basics/datatypes.md: a VALUE, not a
/ function, because it is a constant - which pins help.q after dotq.q for .Q.t,
/ and the bootstrap order already guarantees that.
/ literal/pinf/ninf hold the VALUES, so a cell can never drift from what the
/ display prints - 0Wh shows 32767h, doc-true - and `::` marks a type that has
/ no such value, rendering blank, which is the right display for none.  `nul` is
/ the one RENDERING column: q prints a typed null as a blank cell, and the typed
/ null is anyway reconstructible from `n` by cast where its rendering is not.
/ It is spelled `nul` because `null` is reserved - ([]null:..) signals 'assign.
/ Rows 97-112 are the compound types; the RANGES (20-76 enums, 78-96 nested)
/ are page prose, never rows.
.help.types:([]
  n:0 1 2 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 97 98 99 100 101 102 103 104 105 106 107 108 109 110 111 112h;
  c:("*",.Q.t 1 2 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19),16#" ";
  name:`list`boolean`guid`byte`short`int`long`real`float`char`symbol`timestamp`month`date`datetime`timespan`minute`second`time,
    `$("nested sym enum";"table";"dictionary";"lambda";"unary primitive";"operator";"iterator";"projection";"composition";
       "f'";"f/";"f\\";"f':";"f/:";"f\\:";"dynamic load");
  sz:0N 1 16 1 2 4 8 4 8 1 0N 8 4 4 8 8 4 4 4,16#0N;
  literal:(();0b;0Ng;0x00;0h;0i;0j;0e;0.0;" ";`;2000.01.01D00:00:00.000000000;2000.01m;2000.01.01;
           2000.01.01T00:00:00.000;00:00:00.000000000;00:00;00:00:00;00:00:00.000;
           ::;([]a:1 2);`a`b!1 2;{x};til;+;';+[2];::;(+');(+/);(+\);(-':);(+/:);(+\:);::);
  nul:(enlist[""],-3!'(0b;0Ng;0x00;0Nh;0Ni;0N;0Ne;0n;" ";`;0Np;0Nm;0Nd;0Nz;0Nn;0Nu;0Nv;0Nt)),16#enlist"";
  pinf:(::;::;::;::;0Wh;0Wi;0W;0We;0w;::;::;0Wp;0Wm;0Wd;0Wz;0Wn;0Wu;0Wv;0Wt),16#enlist(::);
  ninf:(::;::;::;::;-0Wh;-0Wi;-0W;-0We;-0w;::;::;-0Wp;-0Wm;-0Wd;-0Wz;-0Wn;-0Wu;-0Wv;-0Wt),16#enlist(::);
  sql:("";"";"";"";"smallint";"int";"bigint";"real";"float";"";"varchar";"";"";"date";"timestamp";"";"";"";"time"),16#enlist"")

/ the command line, one row per flag (user-docs/cmdline.md is its prose):
/ option is the flag with its parameter shape, syscmd the `\` command that
/ reads or sets the same thing (` when none), supported whether peachq honours
/ it, new whether kx q has no such flag.  A VALUE like .help.types, so
/ `select from .help.cmdline where not supported` is the backlog.
.help.cmdline:([]
  option:("-b";"-c r c";"-C r c";"-e 0|1|2";"-E 0|1|2";"-g 0|1";"-l";"-L";"-m path";"-o N";"-p N";"-P N";"-q";
          "-r :h:p";"-s N";"-S N";"-t N";"-T N";"-u file";"-U file";"-w N";"-W N";"-z 0|1";
          "-classic";"-eval \"src\"";"-eval-before \"src\"";"-h | --help";"--port N");
  syscmd:`$("\\_";"\\c";"\\C";"\\e";"\\E";"\\g";"";"";"";"\\o";"\\p";"\\P";"";"\\r";"\\s";"\\S";"\\t";"\\T";"\\u";
            "";"";"\\W";"\\z";"\\classic";"";"";"\\?cmdline";"\\p");
  supported:0101100000101000001100111111b;
  new:      0000000000000000000000011111b;
  what:("block client write-access";"console size: rows and columns";"HTTP display size";
        "error-trap mode for client evals";"TLS server mode: 0 plain, 1 plain and TLS, 2 TLS only";"garbage-collection mode";
        "log updates to a file";"as -l, synchronous";"memory domain";"offset from UTC in hours";
        "listen on a port for IPC and HTTP clients; 0W picks a free port";"float display precision";
        "quiet: no banner, and .z.q is 1b";"replicate from a primary";"secondary threads";"random seed";
        "timer period in ms";"client query timeout in seconds";"password file, and restrict client evals";
        "password file";"workspace memory limit in MB";"start-of-week offset";"date parse order: 0 mdy, 1 dmy";
        "kx-classic mode: legacy table display, kdb-clean environment";
        "run q text after the startup script";"run q text before the startup script";
        "print this table and exit";"the long spelling of -p"))

/ .help.cmdline as page lines: `q -flag`, then the marker (- not supported,
/ * peachq only) and the \ command, then the meaning - padded as a table.
.help.i.cmdlines:{[]
  t:.help.cmdline;
  calls:"q ",/:t`option;
  mk:{[s;n] $[not s;"-";n;"*";" "]}'[t`supported;t`new];
  {[w;c;m;y;o] (w$c)," / ",(11$m,string y),o}[max count each calls]'[calls;mk;t`syscmd;t`what]}

/ `q -h`: the command-line page from this file alone - the builtin help db is
/ never loaded for it, so it answers at boot speed.
.help.usage:{[]
  -1 "\n" sv (enlist "usage: q [file.q] [-option [parameters] ...]"),("  ",/:.help.i.pages[`cmdline;`blurb]),(enlist ""),"  ",/:.help.i.clip .help.i.cmdlines[];}

/ the one-line summary for a name - the first line of its lead description,
/ else of the header of the file documenting it as a NAMESPACE (so a bare
/ `.massive` hints `\?.massive`), "" when neither.  The REPL hint renders
/ `\?name / <this>` and owns the width clipping, so the line comes back untrimmed.
.help.oneline:{[name]
  n:`$.help.i.str name;
  r:select description from .help.args[] where fullname=n,null tag;
  if[count r;:first "\n" vs r[0;`description]];
  s:exec summary from .help.namespaces[] where ns=n;
  $[count s;first s;""]}

/ the namespace a file documents: the one most of its definitions bind into,
/ else its stem (`lib/str.q` is `.str`) - register_file's ns is only the `\d`
/ context, which every lib file leaves at `.`.
.help.i.filens:{[f]
  d:exec ns from .help.funcs where file=f;
  $[count d;first key desc count each group d;`$".",first "." vs last "/" vs string f]}

/ a header's one-liner: its first line, less the `name.q - ` lead the file
/ convention opens with - the row it lands on already names the namespace.
.help.i.nssum:{[f;v] l:first "\n" vs v; s:(last "/" vs string f)," - "; $[s~(count s)#l;(count s)_l;l]}

/ every documented namespace: one row per file header the capture saw - the
/ `\l pq` files and a user's own alike - with the header's one-liner.
.help.namespaces:{[]
  t:select file,val from .help.filetags where null tag;
  select ns:.help.i.filens each file,file,summary:.help.i.nssum'[file;val] from t}

/ a namespace page's blurb: the header comment of every file documenting it.
.help.i.nsheader:{[p] fs:exec file from .help.namespaces[] where ns=p;
  raze {"\n" vs x} each exec val from .help.filetags where file in fs,null tag}

/ register one builtin's one-liner - every row of lib/help-db.q, the page entries
/ included, calls it, so keep the call short.  It NEVER overwrites a
/ CAPTURED definition: the db loads at FIRST HELP ACCESS, by which time a user's own
/ docs can already be in the store, and theirs win.  A null `line` is what marks a
/ registration rather than a capture (.help.i.memlines reads the same column), so the
/ db still replaces the page entry it is meant to.
.help.i.r:{[fullname;description]
  if[not null .help.funcs[fullname;`line];:(::)];
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

/ the doc rows whose fullname, tag, param or description match - globs,
/ matched anywhere and case-insensitively, so a bare word is a substring
/ search.  MANY WORDS ARE AN AND: a two-word query wants the rows answering to
/ both, which no one contiguous phrase finds.  NAMES FIRST: a name hit is what
/ the user asked for where a description hit is a guess, and each matched name
/ collapses to the ONE row that stands for it (its lead line, which is how
/ .help.args[] orders a name's rows) so ?str lists the .str functions, not their
/ tags.  The NAME half wants every term in the name and skips `.i.` privates,
/ as .help.i.nsmembers does; the other half takes each term in ANY field, the
/ name included, so one term can name a row the other describes and prose
/ naming a private still finds it.
.help.find:{[pattern]
  .help.i.loaddb[];
  ps:.help.i.globs lower .help.i.str pattern;
  t:.help.args[];
  if[0=count t;:t];
  nm:exec fullname from .help.funcs where all lower[string fullname] like/:ps, not fullname like "*.i.*";
  n:0!select first tag,first param,first description by fullname from t where fullname in nm;
  fs:(lower each string t`fullname;lower each string t`tag;lower each string t`param;lower each t`description);
  d:t where all {[fs;p] any fs like\:p}[fs] each ps;
  n,d where not d[`fullname] in nm}

/ ---- pages -----------------------------------------------------------------
/ A PAGE is a curated blurb over a member filter, and an ENTRY like any other
/ name: .help.find matches it and .help.oneline answers it.  NAMES BEAT PAGES -
/ an exact documented name always renders its own stack and the page's member
/ table is APPENDED below it.  A PAGE NAME NEVER COLLIDES with a documented name
/ (owner ruling, 2026-08-14): `table` is the page, `tables` stays the keyword.
/ .help.i.pr enforces it defensively - it will not overwrite a documented name -
/ but the spelling, not the guard, is what keeps the two apart.
/ Empty members means "filter by this page's own name as a namespace prefix",
/ which is also what any undeclared namespace with documented members gets.
/ Empty summary means the page's BODY is its entry (`started`: its rows already
/ lead with the page line), so the entry, not this table, is that summary's home.
/ webtopic is the NEAREST ONLINE topic, and ` where none is close enough.  A URL
/ is NEVER built from a page name: our names are ours (`types` is the site's
/ `datatypes`, and `?types` used to point at a 404).  No mapping, no pointer.
.help.i.pages:([page:`started`.z`.Q`.h`.j`adverbs`syscmds`cmdline`types`math`joins`strings`temporal`table`ffi`duckdb`handles`loaders]
 summary:(
  "";
  "session and environment callbacks";
  "database and utility toolkit";
  "HTTP, markup and text helpers";
  "JSON serialize and deserialize";
  "iterators: ' /: \\: ': / \\";
  "the \\ system commands";
  "q command-line flags: .help.cmdline is the table";
  "the datatype reference table";
  "arithmetic, statistics and rounding";
  "as-of, equi, left and union joins";
  "text: search, case, trim, split";
  "dates, times and calendar arithmetic";
  "tables and the qsql verbs: \\?tables is the keyword";
  "";"";"";"");
 webtopic:``dotz`dotq`doth`dotj`iterators`syscmds`cmdline`datatypes`math`joins``datatypes`qsql````;
 blurb:(
  ();();();();();
  ("an iterator modifies a verb: each item, each pair, each left, each right";"/ folds to one value, \\ keeps every step");
  ("a \\ line is a command, not an expression; system \"c 25 200\" is its q form");
  ("the first token ending in .q is the startup script; what follows it and q does not consume is the script's .z.x";
   "after / : the \\ command that reads or sets the same thing, * a peachq-only flag, - not implemented yet");
  ("n is the type number and c the .Q.t character; a vector is n, an atom -n";"sz is bytes per item; sql is the nearest ANSI SQL type";
   "0w and -0w are real infinities; the integer 0W and -0W are the type's bounds, not infinities";
   "20-76 are enums and 78-96 are 77+t, a mapped list of lists of type t: ranges, so neither is a row";
   "a guid's zero IS its null, so 0Ng is both its literal and its nul");
  ("atomic verbs spread over a whole list; aggregates collapse one to a value";"an m- prefix is a moving window, an s- prefix a sample statistic");
  ("aj is the as-of join: the last y row at or before each x time";"lj ij uj pj match on the RIGHT table's key columns");
  ("a string is a char vector, so every list verb works on it";"peachq adds a python-shaped text namespace: \\?.str");
  ("temporal types are numbers: add a long to a date, subtract two timestamps";"\\?types has the literals, the nulls and the infinities");
  ("a table is a flipped dictionary of equal-length named columns";"the functional forms of select and update are ?[t;..] and ![t;..]");
  ();();();());
 members:(
  `$();`$();`$();`$();`$();
  (`each`peach`over`scan`prior),`$("'";"':";"/:";"\\:";"/";"\\");
  `$"\\",/:("a";"b";"B";"c";"C";"cd";"d";"e";"E";"f";"g";"l";"o";"p";"P";"r";"s";"S";"t";"T";"ts";"u";"v";"w";"W";"x";"z";"1";"2";"_";"\\");
  `$();
  `$();
  `abs`neg`signum`sqrt`exp`log`xexp`xlog`floor`ceiling`div`mod`sum`sums`prd`prds`avg`avgs`max`min`maxs`mins`med`dev`var`sdev`svar`cor`cov`deltas`ratios`within`rand`mmu;
  (`aj`aj0`ajf`ajf0`asof`ej`ij`ijf`lj`ljf`pj`uj`ujf`wj`wj1),`$(",";"^");
  (`like`lower`upper`trim`ltrim`rtrim`ss`ssr`string`vs`sv`md5),`$("$";"0:");
  (`gtime`ltime`xbar`.Q.addmonths`.z.p`.z.P`.z.d`.z.D`.z.t`.z.T`.z.z`.z.Z),`$("\\W";"\\z");
  `select`exec`update`delete`from`fby`cols`keys`xcol`xcols`xkey`xasc`xdesc`xgroup`ungroup`meta`tables`insert`upsert`csv`fkeys`flip`key`.Q.en`.Q.id;
  `$();`$();`$();`$()))

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

/ THE column rhythm, byte-identical to tools/gen-help-builtins.py's layout:
/ call, the witnessed result, the meaning.
.help.i.exline:{[call;result;meaning] $[36>count call;36$call;call," "],"/ ",(24$result),meaning}
.help.i.line:{[call;meaning] .help.i.exline[call;"";meaning]}

.help.i.pagemem:{[p] m:$[p in .help.i.pagenames;.help.i.pages[p;`members];`$()];
  $[count m;m;"."=first string p;.help.i.nsmembers p;`$()]}

/ a name's argument list as `[a;b]`, "" when it is not a lambda: what a
/ reader most wants to know about a function is what to pass it.
.help.i.sig:{[n] v:@[value;n;::]; $[100h=type v;"[",(";" sv string (value v)1),"]";""]}

/ the member rows of a page.  A builtin one-liner already names itself in its
/ call column (its .help.funcs line is null) and keeps the tutorial rhythm; a
/ CAPTURED doc comment is prose, so its rows are a two-column table - the `\?`
/ call with the argument list, then ` / ` and the description - padded only as
/ wide as the widest call, since there is no result column to line up with.
.help.i.memlines:{[ns]
  c:{null .help.funcs[x;`line]} each ns;
  b:.help.oneline each ns where c;
  d:ns where not c;
  calls:{"\\?",(string x),.help.i.sig x} each d;
  w:max 0,count each calls;
  b,{[w;c;o] (w$c)," / ",o}[w]'[calls;.help.oneline each d]}

/ what an exact page match ADDS below the name's own entry line: the curated
/ blurb (a namespace's is the header comment of the file documenting it), the
/ member table (each member's dominant meaning), and at most ONE static pointer
/ at the page's mapped online topic - the website only, since `\??` shows a
/ page nothing `\?` did not.  A page NEVER fetches - the pointer is
/ string-built, and the index is only CONSULTED when a previous lookup already
/ cached it, never fetched to check.
.help.i.pagetext:{[p]
  b:($[p in .help.i.pagenames;.help.i.pages[p;`blurb];()]),.help.i.nsheader p;
  / the types page renders the two blocks basics/datatypes.md itself prints: one
  / 35-row table would clip against \c and would carry six blank compound columns.
  m:$[p~`types;
    1_raze {(enlist""),"\n" vs .help.i.rstrip .Q.s x}each
      (select from .help.types where n<20;select n,name,literal from .help.types where n>19);
    p~`cmdline;.help.i.cmdlines[];
    .help.i.memlines .help.i.pagemem p];
  r:("  ",/:b),($[(count b)and count m;enlist"";()]),.help.i.rstrip each"  ",/:m;
  w:$[p in .help.i.pagenames;.help.i.pages[p;`webtopic];`];
  if[null w;:r];
  if[not ()~.help.i.ix;if[not any (string w)~/:.help.i.ix`qname;:r]];   / `and` would index the uncached ()
  $[count .help.url;r,enlist"  more: ",.help.url,"help?q=",.h.hu string w;r]}

/ the index (bare `?`): the tutorial first, every row pasteable, `· page`
/ marking a directory.  Every listed page HAS an entry, so its row is that
/ entry's own line - one home per summary.  Then every documented NAMESPACE
/ the session holds (.help.namespaces: the `\l pq` files and the user's own),
/ or the `\l pq` line while none is loaded.  Layout is prose: order lives here.
.help.i.index:{[]
  .help.i.loaddb[];
  row:{[p] "  ",.help.oneline p};
  ns:`ns xasc select from .help.namespaces[] where not ns in `.z`.Q`.h`.j;
  ns:{[n;s] "  ",.help.i.line["\\?",string n;s]}'[ns`ns;ns`summary];
  "\n" sv (enlist "peachq help · one line per meaning · \\?name shows it · \\??name shows it in full"),
   (enlist row`started),
   (enlist "  ",.help.i.line["\\?til";"try any name: \\?max  \\?.Q.en  \\?$  \\?'type  \\?-p"]),
   (row each `.z`.Q`.h`.j),
   ($[count ns;ns;enlist "  ",.help.i.line["\\l pq";"load the standard library, then \\? lists its namespaces"]]),
   (row each `adverbs`syscmds`cmdline`types),
   (enlist "  ",(count[.help.i.line["";""]]$"\\?math  \\?joins  \\?strings  \\?temporal  \\?table"),"topic pages"),
   enlist "  ",(count[.help.i.line["";""]]$"\\?ffi  \\?duckdb  \\?handles  \\?loaders"),"extensions: what peachq adds to q"}

/ page ENTRY rows, rendered from the registry summary.  NEVER overwrites an
/ already-documented name - the defensive half of the no-collision rule.
/ lib/help-db.q calls it AFTER its generated block, so a page whose body IS its
/ entry (`started`) already stands and keeps its generated line.
.help.i.pr:{[name;page] if[not name in exec fullname from .help.funcs;
  .help.i.r[name;.help.i.line["\\?",string name;.help.i.pages[page;`summary]," · page"]]];}
