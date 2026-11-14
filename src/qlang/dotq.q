/ dotq.q - peachq's embedded q bootstrap: the ".Q" utility namespace, authored from the
/ PUBLISHED qdocs (qdocs/, CC BY 4.0). A REAL .q file, embedded via build-time codegen
/ (tools/gen-bootstrap.sh -> src/qlang/dotq_gen.h) and loaded at the tail of q_runtime_create.
/ Loaded through the script seam (q_ctx_run_src): full kdb script syntax, though most definitions stay one-liners.

/ ---- C-backed members: each delegates to its raw `.Q.c.*` primitive (bound in C before this loads; `.Q.c.*` is internal/unstable) ----
.Q.id:.Q.c.id;
.Q.ty:.Q.c.ty;
/ .Q.c.lty x (internal, no .Q alias): type x, with a mapped list (77h anymap, 77+t nested) answered as the general list
/ it holds, 0h.  Library code that asks "is this a general list" asks through it, so a column read from a kdb file
/ takes the same branch as its in-memory copy.
.Q.qt:.Q.c.qt;
/ wp/permissions .h.jx, kdb-common util.q:118-120 (a modified .Q.s): x's console lines within (rows;cols), from row j
.Q.S:{[b;j;x].Q.c.S[b;$[(j>0)&type[x]within 0 99;j _ x;x]]};
/ kdb-q-ext-vscode src/format/console.ts:66-84 (a port of q's .Q.s2): uncut, a simple vector's elements as cells;
/ an atom answers the cell it gets as an element (owner 2026-11-09)
.Q.s2:{$[type[x]within 1 19;.Q.c.cells x;neg[type x]within 1 19;first .Q.c.cells enlist x;.Q.S[0W 0W;0]x]};
.Q.s:.Q.c.s;
.Q.w:.Q.c.w;
.Q.gc:.Q.c.gc;
.Q.trp:.Q.c.trp;
.Q.trpd:.Q.c.trpd;
.Q.sbt:.Q.c.sbt;
.Q.bt:.Q.c.bt;
.Q.Ll:.Q.c.Ll;
.Q.btx:.Q.c.btx;
/ kb/profiler.md: true for a file name that holds system and built-in functions - peachq's embedded library members
.Q.fqk:{x like "qlib/*"};
/ .Q.btoa/.Q.sha1: the bang is the single C home (`-32!`/`-33!`); delegate here so a user rebind never touches the bang.
.Q.btoa:-32!;
.Q.atob:.Q.c.atob;
.Q.sha1:-33!;
/ .Q.host/.Q.addr: ref/dotq.md - the bang is the single C home (`-12!`/`-13!`, q.k spells them exactly so).
.Q.host:-12!;
.Q.addr:-13!;
.Q.ops:.Q.c.ops;
.Q.res:.Q.c.res;
.Q.hg:.Q.c.hg;
.Q.hp:.Q.c.hp;
.Q.ld:{r:.Q.c.ld "\n" sv x;r[;iasc r 0]};
/ .Q.gz/.Q.ts: the bang is the single C home (`-35!`/`-34!`, the q.k spellings); .Q.ts packs Apply's two args.
.Q.gz:-35!;
.Q.ts:{-34!(x;y)};

/ ---- Enumeration (ref/dotq.md en/ens; KX 2025.01.17 enxs): `?` grows the domain, then f (only `$`) enumerates.
.Q.enxs:{[f;d;t;n]
  if[not f~($);'"nyi"];
  if[not 98h=type t;'"type"];
  c:where 11h=type each flip t;
  h:$[d~(::);n;` sv d,n];
  h?/:(enlist`symbol$()),t c;
  @[t;c;f n]};
.Q.en:{[d;t] .Q.enxs[$;d;t;`sym]};
.Q.ens:{[d;t;n] .Q.enxs[$;d;t;n]};

/ ---- Constants (ref/dotq.md) ----
.Q.b6:"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
.Q.A:"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
.Q.a:"abcdefghijklmnopqrstuvwxyz";
.Q.an:"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789";
.Q.n:"0123456789";
.Q.nA:"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
/ .Q.k (version number) / .Q.K (version date): .Q.k follows .z.K (the kdb+ compatibility level, 5.0); .Q.K is our build date.
.Q.k:.z.K;
.Q.K:.z.k;
/ .Q.M: chunk size for .Q.dsftg (load-process-save); defaults to long infinity.
.Q.M:0W;
/ .Q.t: type letters indexed by datatype number.
.Q.t:" bg xhijefcspmdznuvts";

/ ---- Encoders (ref/dotq.md): pure-q against .Q.b6 (base-64, width 10) / .Q.nA (base-36, width 12) ----
.Q.x10:{raze {1#x _ .Q.b6} each (10#64) vs x};
.Q.j10:{64 sv {[c] first where {x~1#y _ .Q.b6}[c;] each til count .Q.b6} each {[s;i]1#i _ s}[x;] each til count x};
.Q.x12:{raze {1#x _ .Q.nA} each (12#36) vs x};
.Q.j12:{36 sv {[c] first where {x~1#y _ .Q.nA}[c;] each til count .Q.nA} each {[s;i]1#i _ s}[x;] each til count x};

/ ---- Command-line / environment (ref/dotq.md) ----
.Q.x:();
/ .Q.opt: argv -> dict; flags start "-", value = ALWAYS the list of tokens up to the next flag
/ (never collapsed to a bare string at count 1 - `first each .Q.opt .z.x` is the documented idiom). Sets .Q.x.
.Q.opt:{[a] a:$[10h=type a;enlist a;a]; .Q.x:(); $[count a;{[a] i:where {"-"~first x} each a; e:(1_i),count a; .Q.x:$[count i;(first i)#a;a]; k:`$1_'a i; j:til count i; k!{[a;i;e;j] (1+i j)_(e j)#a}[a;i;e;]each j}[a];(`$())!()]};
/ .Q.def: defaults + tok-typed coercion over .Q.opt output (typed null on absent value / bad coerce).
.Q.def:{[d;o] key[d]!{[d;o;k] $[k in key o;$[count o k;(type d k)$first o k;first 0#d k];d k]}[d;o;]each key d};

/ ---- Precision format (ref/dotq.md): thin wrappers over -27! (IEEE754 precision format) ----
/ .Q.f[x;y]: y to x decimals as a string. DECIMAL rounding (not -27!'s IEEE754): nudge y by a
/ tiny relative epsilon so decimal ties cross the boundary (.045->"0.05", cf -27!(2i;.045)->"0.04").
.Q.f:{-27!(x;(1+1e-15)*"f"$y)};
/ .Q.fmt[x;y;z]: z as a string of LENGTH x, formatted to y decimal places (left-padded via `$`).
.Q.fmt:{[x;y;z] neg[x]$.Q.f[y;z]};

/ ---- Value / table-dict (ref/dotq.md) ----
/ .Q.v: filepath -> splayed table (unsupported in-memory); other symbol -> global named x; else -> x.
.Q.v:{$[-11h=type x;value x;x]};
/ .Q.V: `flip x` keeps same-named columns' values apart; a mapped splay's flip is `cols!`:dir/` (ref/flip-splayed.md).
.Q.V:{$[98h<>type x;flip x;0b~.Q.qp x;c!x c:cols x;flip x]};

/ ---- General-purpose utils (ref/dotq.md) ----
.Q.dd:{` sv x,`$string y};
/ .Q.addmonths: x(date) + y months; a day offset past the shorter target month spills forward.
.Q.addmonths:{[x;y] (`date$(`month$x)+y)+x-`date$`month$x};
/ .Q.fu: eval unary f on the DISTINCT items of y then reindex to full length (atom y -> f y).
.Q.fu:{[f;y] $[0>type y;f y;{[f;y;u]f[u]u?y}[f;y]distinct y]};
/ .Q.ft: apply f to a keyed table's simple (0!) form, re-key on the original key cols.
.Q.ft:{[f;t] k:keys t; $[count k;k xkey f 0!t;f t]};
/ .Q.ff: append y's missing columns to table x as count[x] typed nulls (via column dicts).
.Q.ff:{[x;y] dx:.Q.V x; dy:.Q.V y; nc:key[dy] except key dx; flip dx,nc!{[n;v](type v)$n#0N}[count x]each dy nc};
/ .Q.s1: single-line string repr, a thin wrapper over the `-3!` internal fn.
.Q.s1:{-3!x};

/ ---- Partitioned and segmented databases (kb/partition.md, database/segment.md, ref/dotq.md) ----
/ Before a database is loaded the surface is a well-typed EMPTY date-partitioned one (`type .Q.pv` -> 14h), not a
/ 'name error. `.Q.par` is a plain dir/partition/table join: it does not consult par.txt.
.Q.ind:{[x;y] x y};
.Q.par:{[d;p;t] .Q.dd[.Q.dd[d;p];t]};
.Q.pf:`date;
.Q.PV:0#0Nd;
.Q.pv:0#0Nd;
.Q.P:();
.Q.D:();
.Q.PD:0#`;
.Q.pd:0#`;
.Q.pt:0#`;
.Q.pn:()!();
.Q.vp:()!();
.Q.i.absent:()!();
/ .Q.qp: a table's flip names where it lives - a plain symbol for a partitioned table, a directory for a splayed one.
.Q.qp:{$[98h<>type x;0;-11h<>type v:value flip x;0;not":"=first string v]};
/ The partitions of segments: their type letter (" " for none), the names and sorted values in each segment, and the
/ segment and value of every partition in partition order. An entry's type is the letter of the date, month or long
/ its name spells, " " for any other name and for a file (kb/partition.md:11).
.Q.i.partitions:{[segments]
  part_type:{[segment;name]
    letter:first("DMJ"where{[name;letter]name~string letter$name}[name;]each"DMJ")," ";
    $[null letter;letter;0>type key .Q.dd[segment;`$name];" ";letter]};
  found:{[part_type;segment]
    names:string key segment;
    (names;part_type[segment;]each names)}[part_type;]each segments;
  letter:first("DMJ"inter raze found[;1])," ";
  if[null letter;:`letter`dirs`parts!(letter;0#`;())];
  names:{[letter;found]found[0]where letter=found 1}[letter;]each found;
  values:{[letter;names]{x iasc x}letter$names}[letter;]each names;
  order:iasc raze values;
  `letter`names`values`dirs`parts!(letter;names;values;(raze(count each values)#'segments)order;(raze values)order)};
.Q.i.tabled:{[dir;name]
  s:string name;
  (first[s]in .Q.a,.Q.A)&(all s in .Q.an)&(not name in .Q.res,key`.q)&`.d in key .Q.dd[dir;name]};
/ `\l` of a database root, the current directory (basics/syscmds.md:441): the partition directories of each segment
/ (par.txt's rows, else the root itself) and the tables of the most recent one. It answers what the mount binds - the
/ partition field!its values, then each table!its columns - and reads names and that partition's .d files alone.
/ A mount forgets what .Q.bv built (ref/dotq.md .Q.bv: "After loading/re-loading ... invoke .Q.bv[]").
.Q.i.mount:{[]
  .Q.vp:()!();
  .Q.i.absent:()!();
  segmented:`par.txt in key`:.;
  segments:$[segmented;hsym`$read0`:par.txt;enlist`:.];
  found:.Q.i.partitions segments;
  if[null found`letter;:(::)];
  .Q.pf:(`date`month,$[all 4=count each raze found`names;`year;`int])"DMJ"?found`letter;
  .Q.PV:.Q.pv:found`parts;
  .Q.PD:.Q.pd:found`dirs;
  .Q.P:$[segmented;segments;()];
  .Q.D:$[segmented;found`values;()];
  latest:.Q.dd[last .Q.PD;last .Q.PV];
  dirs:key latest;
  .Q.pt:(`symbol$()),dirs where .Q.i.tabled[latest;]each dirs;
  .Q.pn:.Q.pt!count[.Q.pt]#enlist`long$();
  (.Q.pf,.Q.pt)!enlist[.Q.PV],{[dir;name]get .Q.par[dir;name;`.d]}[latest;]each .Q.pt};
/ The partition values as a table of the partition column: what a query's partition constraints are answered over.
.Q.i.values:{[] flip(1#.Q.pf)!enlist .Q.pv};
/ Partition i of partitioned table `name: its columns c, mapped from that partition's directory with at most n rows
/ read, or the empty schema .Q.bv recorded where the table is missing from it.
.Q.i.part:{[name;c;n;i]
  if[i in .Q.i.absent name;:c#.Q.vp name];
  .Q.c.part[.Q.par[.Q.pd i;.Q.pv i;name];c;n]};
/ Partition i of `name with its partition column first: columns c (the field not among them), at most n rows.
.Q.i.dated:{[name;c;n;i]
  t:.Q.i.part[name;c;n;i];
  if[n<count t;t:n#t];
  flip(.Q.pf,c)!enlist[count[t]#.Q.pv i],t c};
/ The row count of each partition idx of `name, read from its column c.
.Q.i.counts:{[name;c;idx]{[name;c;i]count .Q.i.part[name;c;0W;i]}[name;c;]each idx};
/ The table a query over partitioned table `name reads: columns c of the partitions idx (:: is every one), the
/ partition column first, razed in partition order; :: for a name the database does not hold.
.Q.i.table:{[name;c;idx]
  if[not name in .Q.pt;:(::)];
  dated:.Q.i.dated[name;c except .Q.pf;0W;];
  $[idx~(::);raze dated each til count .Q.pv;count idx;raze dated each idx;0#dated -1+count .Q.pv]};
/ Each row's number within its partition (basics/qsql.md:250-252), for the partitions idx (:: is every one) .Q.i.table
/ razes: counted per partition, as one date may be a partition of several segments (database/segment.md:121).
.Q.i.rows:{[name;idx]
  if[idx~(::);idx:til count .Q.pv];
  (0#0),raze til each .Q.i.counts[name;1#key flip get name;idx]};
/ The most recent partition of `name alone: what meta describes.
.Q.i.latest:{[name;c] .Q.i.table[name;c;enlist -1+count .Q.pv]};
/ At most n rows of the first partition of `name: what the console shows; :: when they cannot be read, so the display
/ falls back to the reference (ref/dotq.md, the .Q.bv example).
.Q.i.earliest:{[name;c;n] $[(name in .Q.pt)&0<count .Q.pv;@[.Q.i.dated[name;c except .Q.pf;n];0;::];::]};
/ .Q.cn: the row count of each partition of a partitioned table, kept in .Q.pn; one column of each is mapped.
.Q.cn:{[t]
  if[not 1b~.Q.qp t;:count t];
  ref:flip t;
  .Q.pn[value ref]:.Q.i.counts[value ref;1#key ref;til count .Q.pv];
  .Q.pn value ref};
/ ref/dotq.md .Q.bv: each mounted table missing from some partition gets its empty schema, the partition field first,
/ from the last partition holding it, or the first for .Q.bv[`]
.Q.bv:{[from]
  dirs:.Q.dd'[.Q.pd;.Q.pv];
  held:{[dirs;name]where .Q.i.tabled[;name]each dirs}[dirs;]each .Q.pt;
  gaps:where count[dirs]>count each held;
  schema:{[pick;dirs;name;held]
    dir:.Q.dd[dirs pick held;name];
    t:0#.Q.c.part[dir;get .Q.dd[dir;`.d];0W];
    flip(.Q.pf,cols t)!enlist[0#.Q.pv],value flip t}[$[from~(::);last;first];dirs;;];
  .Q.vp:.Q.pt[gaps]!schema'[.Q.pt gaps;held gaps];
  .Q.i.absent:.Q.pt[gaps]!(til count dirs)except/:held gaps;};
/ ref/dotq.md:328-342: per partition, () or the partition it filled; a table a partition lacks is written there as
/ an empty splay of the most recent partition holding it. A name already present, table or not, is never written.
.Q.chk:{[db]
  absolute:{[path]("/"=first path)|":"=path 1};
  text:{$[(1<count x)&"/"=last x;-1_x;x]}1_string db;
  text:$["./"~2#text;2_text;text];
  root:hsym`$$[absolute text;text;text~(),".";raze system"cd";(raze system"cd"),"/",text];
  rows:$[`par.txt in key root;{x where 0<count each x}read0 .Q.dd[root;`par.txt];()];
  resolve:{[absolute;root;row]$[absolute row;hsym`$row;.Q.dd[root;`$row]]}[absolute;root;];
  segments:$[count rows;resolve each rows;enlist root];
  found:.Q.i.partitions segments;
  dirs:.Q.dd'[found`dirs;found`parts];
  entries:key each dirs;
  held:{[dir;names]names where .Q.i.tabled[dir;]each names}'[dirs;entries];
  names:distinct raze held;
  template:{[dirs;held;name]last dirs where name in/:held}[dirs;held;]each names;
  fill:{[names;template;dir;entries]
    missing:names except entries;
    {[dir;source;name]
      .Q.dd[dir;`$string[name],"/"]set 0#get .Q.dd[source;`$string[name],"/"]}[dir;;]'[template names?missing;missing];
    $[count missing;enlist dir;()]}[names;template;;];
  fill'[dirs;entries]};
/ .Q.bvi/.Q.MAP: nullary no-ops yielding `()` - kdb's generic null is neither `type`-able nor `~`-able here (PLAN.md defect).
.Q.bvi:{[] ()};
.Q.MAP:{[] ()};
/ ref/dotq.md:452-540: enumerated before the sort (its .Q.dpfts mysym is arrival order), as kb/loading-from-large-files.md
/ sketches; a field of :: saves unsorted with no attribute; a table without a symbol column writes no sym file
.Q.i.save_partition:{[dir;part;field;name;domain]
  table:value name;
  sorted:not field~(::);
  if[98h<>type table;'"type"];
  if[sorted;if[not field in cols table;'field]];
  table:@[table;cols table;`#];
  if[any 11h=type each flip table;table:.Q.ens[dir;table;domain]];
  mappable:{[column]
    if[type[column]|not count column;:1b];
    types:type each column;
    (0h<>first types)&all types=first types};
  if[not all mappable each flip table;'"unmappable"];
  if[sorted;table:@[field xcols field xasc table;field;`p#]];
  .Q.par[dir;part;`$string[name],"/"]set table;
  name};
.Q.dpfts:{[d;p;f;t;s] .Q.i.save_partition[d;p;f;t;s]};
.Q.dpft:{[d;p;f;t] .Q.i.save_partition[d;p;f;t;`sym]};
.Q.dpts:{[d;p;t;s] .Q.i.save_partition[d;p;::;t;s]};
.Q.dpt:{[d;p;t] .Q.i.save_partition[d;p;::;t;`sym]};
/ ref/dotq.md:1155-1175
.Q.hdpf:{[port;dir;part;field]
  names:tables`.;
  .Q.dpft[dir;part;field;]each names;
  @[`.;names;0#];
  handle:hopen port;
  handle"\\l .";
  hclose handle;};

/ ---- File / pipe streaming (ref/dotq.md #fs-file-streaming/#fps-pipe-streaming; ref/read1.md ranged form) ----
/ .Q.fsn: loop file y in z-byte lumps of complete "\n" records - the partial tail record carries into the
/ next lump (no record split, no byte lost; a record longer than z grows the carry until its "\n" arrives),
/ apply unary x to each lump's record list, return hcount y. An unterminated final tail is still delivered.
/ A zip:// entry cannot seek, so it streams through .Q.fpn's forward loop.
.Q.fsn:{[x;y;z] if[z<=0;'`domain]; n:hcount y; if[(string y) like ":zip://*"; .Q.fpn[x;y;z]; :n]; o:0; c:""; while[count b:read1(y;o;z); o+:count b; c,:"c"$b; i:last where c="\n"; if[not null i; x "\n" vs i#c; c:(i+1)_c]]; if[count c; x enlist c]; n};
/ .Q.fs/.Q.fps are DOCUMENTED as projections `.Q.fsn[;;131000]`, but currying a projection one arg
/ at a time rank-errors here (the doc's own `.Q.fs[f]`:file shape; PLAN.md defect) - lambda until then.
.Q.fs:{[x;y] .Q.fsn[x;y;131000]};
/ .Q.fpn: the same lump/carry loop over a fifo path - read1(h;z) blocks for data, empty read = writer
/ closed = done; a zip:// entry opens as itself.  The handle is closed however the loop ends.
.Q.fpn:{[x;y;z] if[z<=0;'`domain]; h:hopen $[(s:string y) like ":zip://*"; y; `$":fifo://",1_s];
  .[{[x;h;z] c:""; b:read1(h;z); while[count b; c,:"c"$b; i:last where c="\n"; if[not null i; x "\n" vs i#c; c:(i+1)_c]; b:read1(h;z)]; if[count c; x enlist c]};
    (x;h;z); {[h;e] hclose h; 'e}h]; hclose h;};
.Q.fps:{[x;y] .Q.fpn[x;y;131000]};
