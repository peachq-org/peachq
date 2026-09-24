/ The session itself: version, open connections, terminal size and colour support, and the standard-library load
/ sequence.  Nothing here is loaded at start-up: the first reference to any .pq name loads this file, whose first
/ line - .pq.load_natives`pq, the one .pq member that exists before it runs - binds the C functions it calls.
/ .
/ The console's syntax colours are .pq.hl.
/ .
/ @eg .pq.version
/ @eg select handle, alias from .pq.conns[]

.pq.load_natives`pq;

/ The peachq version string.
.pq.version:.z.v`version;

/ Every open connection, one row each.  The single-letter columns p f z n m are the -38! names so kdb code ports.
/ @col handle what hclose takes: the alias symbol of a :pq: provider, else the fd int
/ @col alias the :pq: alias, when the connection opened with one
/ @eg .pq.conns[]
.pq.conns:{[] .pq.i.conns[]}

/ The live terminal size, with the 25/80 fallback and [10,2000] clamp that \c 0N uses.
/ @return (rows;cols)
/ @eg .pq.termsize[]
.pq.termsize:{[] .pq.i.termsize[]}

/ Whether stdout should carry ANSI colour: PEACHQ_COLORS=0/1 overrides everything, then NO_COLOR (off), FORCE_COLOR
/ (on), else a tty whose TERM is not dumb.
/ @return boolean
/ @eg .pq.cancolor[]
.pq.cancolor:{[] .pq.i.cancolor[]}

/ The console's syntax colours, role to colour, read by the highlighter at each redraw.  Roles: kw keywords, str
/ strings, esc string escapes, cmt comments, sym symbols, num numbers, tmp temporals and typed nulls, op verbs and
/ adverbs, sys system names, cmd \ commands, match the bracket or quote pair at the cursor.  A colour is an int 0-255
/ (a 256-colour foreground) or a string of digits and ; used as the SGR parameters.  A role left out, or given
/ anything else, keeps its default; with colour off (.pq.cancolor) nothing is coloured.  Amend in place
/ (.pq.hl[`kw]:141) or set it in QINIT; loading this file again resets it.
/ The defaults mirror the C table in src/qlang/repl/q_highlight.c - change both together.
.pq.hl:`kw`str`esc`cmt`sym`num`tmp`op`sys`cmd`match!(33;114;168;244;37;166;136;141;127;160;"7");

/ @ignore
.pq.drsamples:{[k] c:"bgxhijefcspmdznuvt"; v:@[.'[$;;`$]c,'1;c?"gs";:;(0Ng;`abc)]; $[k;{3#x}each v;v]}
/ @ignore
.pq.drtype:{[x] c:"bgxhijefcspmdznuvt"; $[x~`err;".";$[0>type x;c;upper c]("h"$(1+til 19)except 3)?abs type x]}
/ @ignore
.pq.drform:{[f;m] c:"bgxhijefcspmdznuvt"; v:.pq.drsamples m~`monadic; n:$[-11h=type f;string f;-10h=type f;enlist f;f]; g:$[not (type f)in -11 -10 10h;f;m~`dyadic;eval parse "{x ",n," y}";eval parse n]; $[m~`dyadic;-1 {x," | ",1_raze " ",'y}'[c;{[g;v;x]{[g;x;y].pq.drtype .[g;(x;y);{`err}]}[g;x]each v}[g;v]each v];-1 ("domain:";"range: "),'{raze " ",'x}each($[m~`monadic;upper c;c];.pq.drtype each{[g;x].[g;enlist x;{`err}]}[g]each v)];}
/ Print a verb's domain and range grid in the layout the KX reference pages use, probed by casting a sample of every
/ type: a lower-case letter is an atom result, upper-case a list, "." an error.  After KX's tools.q
/ (qdocs/docs/docs/tools.q, CC BY 4.0).
/ @param verb a symbol or char naming a verb in .Q.ops[], or a function
/ @eg .pq.dr `neg
/ @eg .pq.dr "+"
.pq.dr:{[verb] t:.Q.ops[]; i:(t`name)?$[-11h=type verb;verb;`$$[-10h=type verb;enlist verb;verb]]; $[i>=count t`name;"not in .Q.ops[]";t[`dyadic]i;.pq.drform[verb;`dyadic];t[`monadic]i;.pq.drform[verb;`monadic];"no valence in .Q.ops[]"]}

/ @ignore
.pq.i.rval:{[x] v:@[value;x;{[e] 'unpush}]; $[100h<=type v;'unpush;((11h=abs type v)or(type v) in 0 98 99h);enlist v;v]}
/ @ignore
.pq.i.rnode:{[cl;x] $[(type x) in 0 99h;.z.s[cl] each x;-11h=type x;$[x in cl;x;.pq.i.rval x];x]}
/ @ignore
.pq.i.resolveTree:{[cl;tree] (enlist tree 0),.pq.i.rnode[cl,`i] each 1_tree}

/ One line of facts about a table, one per column in column order: numeric columns as min-max, booleans as the
/ share true, everything else as a distinct count (`all distinct` when every row differs; a temporal column that
/ repeats shows its range), with a null count where there are nulls.  The same facts the console digest prints
/ under a clipped table, so the two never disagree.
/ @param t (table) a table or keyed table
/ @return string, `-1`-ready
/ @eg .pq.summary ([]a:1 2 3;b:`x`y`x)
.pq.summary:{[t] " " sv .pq.i.facts t};

/ The qStudio evaluation wrapper, sent byte-identical (firms detect its prefix); 10485760 is qStudio's default size
/ gate.  Answers (sizeOk; $[sizeOk; ((runOk;`);value or (error;backtrace)); 0b]; consoleText).
.pq.i.WRAPPER:"{v:$[`trp in key .Q; .Q.trp[{( (1b;`) ;value x)};x;{((0b;`);x;$[4<count y; .Q.sbt -4 _ y; \"\"])}]; ",
  "((1b;`);value x)]; a:10485760j>@[-22!;v;{0}]; (a;$[a;v;0b];.Q.s v 1)} ";

/ qStudio's server-tree query, sent byte-identical (its comment line and CRLF line ends included).
.pq.i.TREE_QUERY:"/ qstudio - get server tree \r\n",
  "{   nsl:\".\",/:string `,key `;    \r\n",
  "    nsf:{[ns] \r\n        ff:{ [viewset; v; fullname; sname]\r\n",
  "            findColArgs:{$[.Q.qt x; cols x; 100h~type x; (value x)1; `$()]};\r\n",
  "            safeCount: {$[.Q.qp x; $[`pn in key `.Q; {$[count x;sum x;-1]} .Q.pn y; -1]; count x]};\r\n",
  "            (@[type;v;0h]; .[safeCount;(v;fullname);-2]; @[.Q.qt;v;0b]; @[.Q.qp;v;0b]; @[findColArgs;v;()]; ",
  ".[in;(sname;viewset);0b])};\r\n",
  "        vws: system \"b \",ns;\r\n        n: asc key[`$ns] except `;\r\n",
  "        fn: $[ns~enlist \".\"; n; ns,/:\".\",/:string n];\r\n",
  "        n!.'[ ff[vws;;;]; flip ( @[`$ns; n]; fn; n)]};\r\n",
  "    (`$nsl)!@[nsf;;()!()] each nsl}[]";

/ Namespaces `-ls` hides: the language's own and the standard library's.
.pq.i.LS_HIDDEN:`.Q`.q`.h`.j`.help`.pq;

/ The text as a q string literal's body (qStudio's KdbHelper.escape, in its order): backslash, tab, CR, LF, quote.
.pq.i.remote_escape:{[text]
  text:"",text;
  special:"\\\t\r\n\"";
  escaped:("\\\\";"\\t";"\\r";"\\n";"\\\"");
  at:where text in special;
  $[count text; raze @[enlist each text;at;:;escaped special?text at]; text]};

/ Evaluate q text on a server, the way qStudio does: the wrapper round the escaped text, one sync call.
/ @param handle (int) an open handle
/ @param text (string) the q text; multi-line text runs as a script and the last statement answers
/ @return the raw triple (sizeOk; $[sizeOk; ((runOk;`);value or (error;backtrace)); 0b]; consoleText)
.pq.i.remote_call:{[handle;text] handle .pq.i.WRAPPER,"\"",.pq.i.remote_escape[text],"\""};

/ Whether a result is the size-gate form: the console text alone, as .pq.i.remote_eval answers it.
.pq.i.remote_is_console:{[result] $[99h=type result; (key result)~enlist `.pq.console; 0b]};

/ The value of q text evaluated on a server.  A remote error is signalled here as its class (`'type`); a result
/ over qStudio's 10 MB gate comes back as its console text only, flagged as the dict (enlist `.pq.console)!enlist
/ text - .pq.i.remote_is_console tells the two apart.
/ @param handle (int) an open handle
/ @param text (string) the q text
.pq.i.remote_eval:{[handle;text] .pq.i.remote_unwrap .pq.i.remote_call[handle;text]};

/ The wrapper's triple, or a raw value from a server that answers by deferred reply (a TorQ gateway) rather than
/ evaluating the wrapper: anything but a 3-list of (boolean atom; ...; string) IS the value, made the triple here.
.pq.i.remote_as_triple:{[reply]
  triple:$[(0h=type reply) and 3=count reply; (-1h=type reply 0) and 10h=type reply 2; 0b];
  $[triple; reply; (1b;((1b;`);reply);"")]};

.pq.i.remote_unwrap:{[reply]
  reply:.pq.i.remote_as_triple reply;
  if[not reply 0; :(enlist `.pq.console)!enlist reply 2];
  if[not reply[1;0;0]; 'reply[1;1]];
  reply[1;1]};

/ The recorded evaluation every launcher call is: the wrapper triple goes to the history before it is unwrapped,
/ so a failing or oversize call is in the index too.  A remote error is signalled as its class.
/ @return (sizeOk; the value, or the console text when sizeOk is 0b; the .bin path or "")
.pq.i.remote_query:{[handle;text]
  reply:.pq.i.remote_as_triple .pq.i.remote_call[handle;text];
  path:@[.pq.i.qhist_record[handle;text];reply;{[err] -2 "history not written: ",err; ""}];
  val:.pq.i.remote_unwrap reply;
  (reply 0; $[reply 0; val; val`.pq.console]; path)};

/ Every variable on a server as one flat table, tables first: ns name type count cols isview.  Asks the qStudio
/ server-tree query (byte-identical); the language's own namespaces and the standard library's are left out.
/ @param handle (int) an open handle
/ @return table
.pq.i.remote_ls:{[handle]
  tree:handle .pq.i.TREE_QUERY;
  tree:(key[tree] except .pq.i.LS_HIDDEN)#tree;
  facts:raze value each value tree;
  vars:flip `ns`name`type`count`cols`isview!$[count facts;
    (raze {[ns;d] (count d)#ns}'[key tree;value tree]; raze key each value tree; facts[;0]; facts[;1]; facts[;4]; facts[;5]);
    (`symbol$();`symbol$();`short$();`long$();();`boolean$())];
  vars:`name xasc vars;                            / stable sorts, the last applied is the primary key
  `ns xasc vars iasc not (vars[`type]) in 98 99h};

/ The -ls listing as lines, every variable regardless of the console size: a namespace's name (root first, then
/ sorted), then its variables indented - tables first - as name, type, count, columns, and "view" where it is one.
.pq.i.conn_ls_lines:{[handle]
  vars:.pq.i.remote_ls handle;
  raze {[grp]
    width:max count each string grp`name;
    columns:{$[count x; " " sv string x; ""]} each grp`cols;
    rows:(width$string grp`name),'"  ",/:(5$string[grp`type],'"h"),'(9$string grp`count),'columns,'{$[x;" view";""]} each grp`isview;
    (enlist string first grp`ns),"  ",/:rows} each {[vars;n] select from vars where ns=n}[vars] each distinct vars`ns};

.pq.i.conn_ls:{[handle] -1 .pq.i.conn_ls_lines handle;};

/ The console display of a remote result: the value as the console shows it, and under the classic display a
/ table's summary line - the modern display digests a clipped table itself and shows a small one whole.
/ @param val (any) the unwrapped value
.pq.i.remote_show:{[val]
  if[(::)~val; :(::)];
  1 .Q.s val;
  if[.Q.qt[val] and system "classic"; -1 .pq.summary val];
  };

/ Evaluate q text on a server and display the result, recording the query and its value under ~/.qhist.d.  A remote
/ error is signalled as its class; over the 10 MB gate only the console text arrives, and nothing is saved.
/ @param handle (int) an open handle
/ @param text (string) the q text
/ @return the saved kdb binary's path when the display hid part of a table, for the caller to announce once the
/ display has left; else ""
.pq.i.remote_run:{[handle;text]
  reply:.pq.i.remote_query[handle;text];
  if[not reply 0; 1 reply 1; -2 "result over the 10 MB gate: console text only, nothing saved"; :""];
  .pq.i.remote_show reply 1;
  $[.Q.qt[reply 1] and .pq.i.remote_clipped reply 1; reply 2; ""]};

/ Whether the console display of a value hides any of it: it differs from the display at the widest console.
.pq.i.remote_clipped:{[val]
  size:"c "," " sv {$[null x; "0N"; string x]} each system "c";
  system "c 2000 2000";
  whole:@[.Q.s;val;{[size;err] system size; 'err}[size]];
  system size;
  not whole~.Q.s val};

/ Evaluate q text on a server and `set` the value to a file, its ending choosing the format (.csv .json .parquet, else
/ kdb binary).  A result over the 10 MB gate is refused - only its display text came back - with `'oversize`.
/ @param handle (int) an open handle
/ @param file (string) the path to write
/ @param text (string) the q text
.pq.i.remote_save:{[handle;file;text]
  reply:.pq.i.remote_query[handle;text];
  if[not reply 0; -2 "result over the 10 MB gate: display text only, not saved to ",file; 'oversize];
  (hsym `$file) set reply 1;
  };

/ The launcher's connection under -conn: any hopen target as written - all digits a port, anything else the symbol -
/ with the connect timeout; the handle, or hopen's own reason on stderr and exit 2.
.pq.i.conn_open:{[target]
  where_to:$[(count target) and all target in .Q.n; "J"$target; `$target];
  @[hopen;(where_to;5000);{[target;err] -2 "q: cannot connect to ",target,": ",err; exit 2}[target]]};

/ One launcher call under -conn: a display, or with a file the last text's value saved and the earlier ones run
/ silently, as a script's non-final statements.  A remote error is its class on stderr and exit 1, as a failing
/ script; the caller announces the answered path once the display has left the console.
.pq.i.conn_call:{[handle;text;file;final]
  to_file:{[h;f;t] .pq.i.remote_save[h;f;t]; ""};
  silently:{[h;t] .pq.i.remote_query[h;t]; ""};
  run:$[""~file; .pq.i.remote_run[handle]; final; to_file[handle;file]; silently[handle]];
  @[run;text;{[err] -2 "'",err; exit 1}]};

.pq.i.conn_notice:{[path] if[count path; -2 "Full result saved locally to kdb binary: ",path]};

/ The query history root: PEACHQ_QHIST when set, else ~/.qhist.d (HOME, or USERPROFILE on Windows; the console's line
/ history already owns the ~/.qhist FILE).  The launcher turns the history off by binding .pq.i.QHIST to the empty
/ string; unbound, the environment decides.
.pq.i.qhist_root:{[]
  if[10h=type .pq.i.QHIST; :.pq.i.QHIST];
  root:getenv `PEACHQ_QHIST;
  if[count root; :root];
  home:getenv `HOME;
  if[not count home; home:getenv `USERPROFILE];
  home,"/.qhist.d"};
.pq.i.QHIST:(::);

/ A handle's history directory name: host_port, credentials dropped, anything outside [A-Za-z0-9_.-] made `_`.
.pq.i.qhist_dirname:{[target]
  parts:":" vs $[-11h=type target; string target; target];
  parts:$[(count parts)>1; 1_parts; parts];
  name:"_" sv 2#parts,enlist "";
  @[name; where not name in .Q.an,".-"; :; "_"]};

/ One line of index text: tabs and newlines flattened, cut to 200 characters.
.pq.i.qhist_line:{[text] 200 sublist ssr[ssr[ssr["",text;"\t";" "];"\r";" "];"\n";" "]};

/ The timestamp a result file is named by: yyyymmdd_hhmmss_mmm, local time.
.pq.i.qhist_stamp:{[ts] (except[string `date$ts;"."]),"_",ssr[except[string `time$ts;":"];".";"_"]};

/ Record one call: an index row (the last 200 kept) and, for a value under the gate, its `-8!` bytes as
/ <stamp>.bin (the last 10 kept, 100 MB in all, oldest evicted first).  Answers the .bin path, "" when none.
.pq.i.qhist_record:{[handle;text;reply]
  root:.pq.i.qhist_root[];
  if[not count root; :""];
  dir:root,"/",.pq.i.qhist_dirname .pq.i.qhist_target handle;
  reply:.pq.i.remote_as_triple reply;
  ok:$[not reply 0; `oversize; reply[1;0;0]; `ok; `fail];
  val:$[ok=`ok; reply[1;1]; ::];
  err:$[ok=`fail; reply[1;1]; ""];
  bytes:$[ok=`ok; -22!val; 0N];
  rows:$[(ok=`ok) and .Q.qt val; count val; 0N];
  path:$[ok=`ok; .pq.i.qhist_save[dir;val]; ""];
  .pq.i.qhist_index[dir; "\t" sv (string .z.P; string ok; .pq.i.qhist_line err; string bytes; string rows; .pq.i.qhist_line text)];
  path};

/ The :host:port a handle was opened to, from the connection table; the handle number when it is not there.
.pq.i.qhist_target:{[handle]
  addr:exec addr from .pq.conns[] where h=handle;
  $[count addr; ":",first addr; ":",string handle]};

.pq.i.qhist_index:{[dir;line]
  file:hsym `$dir,"/index.tsv";
  rows:$[()~key file; (); 1_read0 file];
  file 0: (enlist "ts\tok\terror\tbytes\trows\tquery"),-200 sublist rows,enlist line;
  };

.pq.i.qhist_save:{[dir;val]
  stamp:.pq.i.qhist_stamp .z.P;
  taken:string key hsym `$dir;
  n:0;
  name:stamp,".bin";
  while[any name~/:taken; n+:1; name:stamp,"_",string[n],".bin"];
  file:hsym `$dir,"/",name;
  file 1: -8!val;
  .pq.i.qhist_evict dir;
  1_string file};

/ Keep the newest 10 .bin files and no more than 100 MB of them; names sort by their stamp.
.pq.i.qhist_evict:{[dir]
  bins:asc {x where x like "*.bin"} string key hsym `$dir;
  paths:hsym each `$dir,/:"/",/:bins;
  sizes:hcount each paths;
  drop:0|(count bins)-10;
  while[(drop<count bins) and (sum (drop _ sizes))>100*1024*1024; drop+:1];
  hdel each paths til drop;
  };

/ Load the standard library: \l pq runs this.  Every file loads as \l pq/<file>.q - a real file when a pq/ directory
/ (the working directory's, then QHOME's) has it, else the copy built into peachq - so the list here IS the library.
/ @return (symbol list) the file names, in load order
/ @eg .pq.load[]
.pq.load:{[]
  files:`csv`duckdb`ffi`j`massive`md`parquet`pq`qpc`regexp`str`termbox`fs`path`pkg`qunit`yml;
  {system "l pq/",string[x],".q"} each files;
  files};
