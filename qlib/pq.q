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

/ Every open connection, one row each; -38! keeps kdb's letters, these are the same facts in words.
/ @col handle the int: what .z.pc gets and neg works on, for an alias too
/ @col spec the alias symbol (`:pq:qfork:w1) hopen answered, null for a plain handle
/ @col protocol `q or `ws on a socket; family `tcp or `unix
/ @col pid the worker an owned alias kills on hclose; owned says it has one
/ @eg .pq.conns[]
/ @eg exec handle from .pq.conns[] where spec=`:pq:qfork:w1
.pq.conns:{[] .pq.i.conns[]}

/ What peachq itself holds behind a `:pq: handle, a dict of tables: a table coordinate answers `schema (col dtype
/ logical iskey t, as bound on first touch); an alias answers `tables (table origin bound: what is bound, `q or `duckdb
/ by the sidecar, when) and `stats (binds hits rebinds dropped).  Connection facts are .pq.conns[]; the catalog is SQL.
/ @param x the `:pq: symbol; anything else is 'type, an alias that is not open 'conn
/ @eg .pq.info hopen (`:pq:duckdb:demo;":memory:")
.pq.info:{[x] .pq.i.info x}

/ Declare which text columns of a DuckDB table or view q did not write are symbols, as the template table's types say.
/ The declaration is written into the database's _q_schema, so it stays with the table for every session, and replaces
/ any earlier one; the next select reads each declared column as a symbol and every other column as before.  For now
/ only symbols are declared: a template column of type s over a VARCHAR column.  Any other template column, and one
/ the table lacks, is reported but not declared.
/ @param coordinate the table, `:pq:duckdb:alias:table/
/ @param template a q table whose column types are the declaration, rows or none; () or a table of no columns drops it
/ @return ([] col; type; applied): each template column, its meta char, and whether it was declared
/ @throws type coordinate is not a DuckDB table coordinate, or template is not a table
/ @throws domain the table is one q wrote, whose own schema stands
/ @eg h:hopen (`:pq:duckdb:d;":memory:"); h "CREATE TABLE t AS SELECT 'A' AS s, 1.5 AS px"; .pq.setschema[`:pq:duckdb:d:t/;([] s:`symbol$())]
.pq.setschema:{[coordinate;template]
  if[-11h<>type coordinate; '`type];
  s:string coordinate; if[not (s like ":pq:duckdb:*/") and 3<sum s=":"; '`type];
  h:`$(i:(where s=":") 3)#s; t:`$-1_(1+i)_s;
  if[99h=type template; template:0!template];
  if[not (template~()) or 98h=type template; '`type];
  if[not .duckdb.i.foreign[h;t]; '`domain];
  tc:$[template~();0#`;cols template]; ty:$[count tc;exec t from meta template;""];
  sc:.duckdb.i.info[h;t]`schema; j:(lower sc`col)?lower tc;
  ok:(0#0b),(ty="s") and {[sc;j] $[j<count sc;"VARCHAR"~sc[`dtype] j;0b]}[sc] each j;
  d:distinct sc[`col] j where ok; .duckdb.i.declare[h;t;([] col:d; logical:(count d)#`symbol)];
  flip `col`type`applied!(tc;ty;ok)}

/ The syntax tree of q source text, for tools: `nodes`errors!(nodes;errors).  It never stops at an error: every
/ statement (cut as \l cuts them) and every closing bracket starts afresh, and a statement it finds well-formed is
/ judged by the parser itself, whose refusal becomes an error row.  Offsets are bytes into src, end-exclusive.
/ @param src (string) q source text, a whole file or a line
/ @return a dict of two tables: nodes ([] id; parent; kind; start; end; scope; text), parent -1 at top level and
/ always below id, scope the nearest enclosing lambda or params (-1 for none), text always a string; and errors
/ ([] start; end; msg).  A kind is one of `stmt`cmd`comment`expr`lambda`params`args`block`paren`name`num`str`sym`verb
/ `adverb: an expr is each ;-separated slot, a cmd an opaque \ or <letter>) statement, args a [ after a term
/ @eg .pq.syntax "f:{[a;b] a+1}"
/ @eg (.pq.syntax "f:{x+1\ng:2")`errors
.pq.syntax:{[src] .pq.i.syntax src}

/ Every top-level definition in q source text, documented or not, for editors and tools.  The text is cut and its
/ comments read as \l reads them, and never run: a \l in it is not followed, \d is followed as text from the root, and
/ the doc store is untouched.  A definition is a name a statement assigns outside any lambda (an amend, a,:1 or a[i]:1,
/ is none), and its doc is the comment run \l would register for it: a chain a:b:3 documents b, assigned first.
/ Positions are 1-based lines and byte columns, end inclusive: line to endcol spans the ;-separated slot that holds the
/ definition, nameline and namecol its name.
/ @param src (string) q source text, a whole file or a line
/ @return a dict: file `doc`tags!(doc;tags) from the file's header run, and defs ([] name; ns; kind; params; line; col;
/ endline; endcol; nameline; namecol; doc; tags; types) in source order.  name is the full name, kind one of `function
/ `projection`table`view`value, params a function's (x y z by use when undeclared), doc "" when undocumented, tags
/ ([] tag; param; description), param set for @param and @exception, and types one per param, the type its signature
/ checks (x:`j is `j) or `
/ @eg .pq.outline "/ Adds one.\n/ @param x (long)\nf:{x+1}"
/ @eg exec name from (.pq.outline "\\d .ns\nf:{x}\ng:2")`defs
/ @eg exec types from (.pq.outline "f:{[x:`j;y] x+y}")`defs
.pq.outline:{[src] .pq.i.outline_of[src;(.pq.syntax src)`nodes]};

/ .pq.outline of src from its .pq.syntax nodes, for a caller that has them.
.pq.i.outline_of:{[src;nodes]
    outline:.pq.i.outline src;
    slots:`start xasc select start, end from nodes where kind=`expr, parent in exec id from nodes where parent=-1;
    tokens:`start xasc select start, end from nodes where not kind in `stmt`cmd`comment`expr;
    found:`at xasc outline`defs;
    slot:0|slots[`start] bin found`at;
    head:tokens[`start] binr slots[`start] slot;
    tail:-1+"j"$max each tokens[`end] head+til each 1+(tokens[`start] bin slots[`end][slot]-1)-head;
    line_col:{[starts;b] (1+starts bin b; 1+b-starts starts bin b)}[0,1+where src="\n"];
    pos:line_col each (tokens[`start] head; tail; found`at);
    docs:.pq.i.docs[(enlist `),found`name;enlist[outline`file],found`doc];
    defs:([] name:found`name; ns:found`ns; kind:found`kind; params:{x where not null x} each found`params;
        line:pos[0;0]; col:pos[0;1]; endline:pos[1;0]; endcol:pos[1;1]; nameline:pos[2;0]; namecol:pos[2;1];
        doc:1_docs[;0]; tags:1_docs[;1]; types:.pq.i.types[nodes;found;slots[`end] slot]);
    `file`defs!(`doc`tags!first docs; defs)};

/ Per doc of docs, its lead text and its tags table (.help.i.parse), the file's header named `.  The answers of the
/ last call are kept, so a name whose doc is unchanged since then is not parsed again.
.pq.i.docs:{[names;docs]
    split:{[header]
        rows:.help.i.parse header;
        tagged:rows where not null rows[;0];
        (first ((rows where null rows[;0])[;2]),enlist "";
            flip `tag`param`description!(`symbol$tagged[;0]; `symbol$tagged[;1]; tagged[;2]))};
    held:@[get;`.pq.i.Docs;{[e] (`symbol$();();())}];
    parsed:{[held;split;i;doc] $[(i<count held 0) and doc~held[1] i; held[2] i; split doc]}[held;split]'[held[0]?names;docs];
    `.pq.i.Docs set (names;docs;parsed);
    parsed};

/ Per definition of found (.pq.i.outline's rows, by at), the type each declared parameter's signature slot checks
/ (.pq.i.slot_type), ` where none, from the params of the first top-level lambda after its name within its slot,
/ which ends at ends.
.pq.i.types:{[nodes;found;ends]
    lambdas:`start xasc select id, start from nodes where kind=`lambda, scope=-1;
    after:lambdas[`start] binr found`at;
    lambda:?[(after<count lambdas) and lambdas[`start][after]<ends; lambdas[`id] after; 0N];
    kids:.pq.i.kids nodes;
    slot:{[k;t;kids;e] c:kids e; .pq.i.slot_type[k c;t c]}[nodes`kind;nodes`text;kids];
    typed:{[slot;kids;params;raw]
        t:slot each kids params;
        `symbol$$[count[raw]=count t; t where not null raw; sum[not null raw]#`]}[slot;kids];
    typed'[(exec parent!id from nodes where kind=`params) lambda;found`params]};

/ Per syntax node, its children by position, comments left out.
.pq.i.kids:{[nodes]
    order:(iasc nodes`start) except where nodes[`kind]=`comment;
    order@:where nodes[`parent][order]>=0;
    by_parent:order group nodes[`parent] order;
    @[count[nodes`kind]#enlist `long$();key by_parent;:;value by_parent]};

/ The type a signature slot's tokens check, kinds k and texts t (basics/pattern.md:225): `j for x:`j and (a;b):`j, else `.
.pq.i.slot_type:{[k;t] $[2>count k; `; (`verb`sym~-2#k) and (enlist[":"]~t count[k]-2) and .pq.i.type_check last t; `$1_last t; `]};

/ Whether sym is a type check's type, a backtick and one letter (basics/pattern.md:225).
.pq.i.type_check:{[sym] (2=count sym) and ("`"=first sym) and last[sym] in .Q.a,.Q.A};

/ The ranks x accepts (basics/glossary.md "Rank": of a function, the number of arguments it takes), sorted: a
/ variadic answers each, a list every depth it indexes to, and a value that is not applicable `long$().
/ @param x (any)
/ @return (long list)
/ @eg .pq.valence (-)
/ @eg .pq.valence {x+y}[1]
/ @eg .pq.valence (@)
.pq.valence:{[x] .pq.i.valence x}

/ Open (or re-point) `:pq:q:alias on another q process, the same call as hopen (`:pq:q:alias;arg); answers that symbol.
/ @param arg (any) what hopen takes (port, `::5000, `:host:port:user:pass, (conn;timeout) ...), or a `conn`timeout dict
.pq.hopen_q:{[alias;arg] hopen (`$":pq:q:",string alias;arg)};

/ Fork this process as the worker `:pq:qfork:alias, the same call as hopen (`:pq:qfork:alias;arg).
/ @param arg (any) :: for the defaults, a timeout in ms, or a dict of `timeout`stdout`stderr: `stdout`stderr!(`:w1.out;`:w1.err)
/ sends the worker's output to those files, as \1/\2 run by the worker before anything else
.pq.hopen_qfork:{[alias;arg] hopen (`$":pq:qfork:",string alias;arg)};

/ Launch a fresh peachq as the worker `:pq:qspawn:alias, the same call as hopen (`:pq:qspawn:alias;arg).
/ @param arg (any) its argv, one string per argument ("" or :: for none), or a dict of `argv`timeout`stdout`stderr:
/ `argv`stdout`stderr!(("init.q";"-s";"2");`:logs/w2.log;`:logs/w2.log) logs its startup script's output too
.pq.hopen_qspawn:{[alias;arg] hopen (`$":pq:qspawn:",string alias;arg)};

/ Attach a DuckDB database as `:pq:duckdb:alias, the same call as hopen (`:pq:duckdb:alias;arg).
/ @param arg (any) the path or ":memory:", or a dict of `path beside DuckDB's own ATTACH and SET options
.pq.hopen_duckdb:{[alias;arg] hopen (`$":pq:duckdb:",string alias;arg)};

/ Open a Kafka client as the alias `:pq:kafka:alias; arg is librdkafka's config as a dict of its keys, metadata.broker.list
/ at least and group.id to subscribe.  The alias consumes and publishes.
/ @eg .pq.hopen_kafka[`c1;`metadata.broker.list`group.id!`localhost:9092`myapp]
.pq.hopen_kafka:{[alias;arg] hopen (`$":pq:kafka:",string alias;arg)};

/ How each HTTPS client in this process checks a server's certificate, one row per transport: `peachq for .Q.hg, read0,
/ select from an https URL and hopen of tcps:// or wss://, and `duckdb once DuckDB's httpfs extension is loaded.
/ With no SSL_CA_CERT_FILE or SSL_CA_CERT_PATH set, peachq trusts the operating system's roots: Windows' root store, or
/ OpenSSL's default bundle elsewhere.  SSL_VERIFY_SERVER=NO turns checking off.
/ @col verify whether the server's certificate is checked
/ @col ca_source `env when SSL_CA_CERT_FILE or SSL_CA_CERT_PATH names the roots, `os for Windows' root store, `default
/ for the library's own default locations, `setting for DuckDB's ca_cert_file
/ @col ca_file the trusted roots' bundle file, "" where none applies; ca_path the same for a directory of roots
/ @col openssl_version the OpenSSL peachq loaded, "" when there is none
/ @col overridden_by the environment variables (or DuckDB settings) that move this row off its default
/ @col last_error why peachq's last TLS connection failed, "" after one that worked; the error it signalled is 'conn
/ @eg .pq.tlsinfo[]
/ @eg first exec last_error from .pq.tlsinfo[] where transport=`peachq
.pq.tlsinfo:{[]
  t:enlist(enlist[`transport]!enlist`peachq),.pq.i.tlsinfo[];
  if[not`:pq:duckdb:main in exec spec from .pq.conns[];:t];
  q:"SELECT name, value FROM duckdb_settings() WHERE name IN ('ca_cert_file','enable_curl_server_cert_verification')";
  r:.duckdb.exec[.duckdb.main[];q];
  s:(`$r`name)!r`value;
  if[2>count s;:t];
  f:s`ca_cert_file;
  v:"true"~s`enable_curl_server_cert_verification;
  t,enlist`transport`verify`ca_source`ca_file`ca_path`openssl_version`overridden_by`last_error!
    (`duckdb;v;$[count f;`setting;`default];f;"";"";`ca_cert_file`enable_curl_server_cert_verification where(0<count f),not v;"")};

/ The live terminal size, with the 25/80 fallback and [10,2000] clamp that \c 0N uses.
/ @return (rows;cols)
/ @eg .pq.termsize[]
.pq.termsize:{[] .pq.i.termsize[]}

/ Whether stdout should carry ANSI colour: PEACHQ_COLORS=0/1 overrides everything, then NO_COLOR (off), FORCE_COLOR
/ (on), else a tty whose TERM is not dumb.
/ @return boolean
/ @eg .pq.cancolor[]
.pq.cancolor:{[] .pq.i.cancolor[]}

/ The modern display of x as a string: exactly what the console prints when it is not in \classic mode, whatever the
/ session's mode, clipped by \c like .Q.s.  A table or keyed table is the pipe table; anything else is the text .Q.s
/ returns, which is always the classic display.
/ @param x (any) the value to render
/ @return (string) the display, ending in a newline
/ @eg .pq.render_modern ([]a:1 2;b:`x`y)
.pq.render_modern:{[x] .pq.i.render_modern x};

/ The console's syntax colours, role to colour, read by the highlighter at each redraw.  Roles: kw keywords, str
/ strings, esc string escapes, cmt comments, sym symbols, num numbers, tmp temporals and typed nulls, op verbs and
/ adverbs, sys system names, cmd \ commands, match the bracket or quote pair at the cursor.  A colour is an int 0-255
/ (a 256-colour foreground) or a string of digits and ; used as the SGR parameters.  A role left out, or given
/ anything else, keeps its default; with colour off (.pq.cancolor) nothing is coloured.  Amend in place
/ (.pq.hl[`kw]:141) or set it in QINIT; loading this file again resets it.
/ The defaults mirror the C table in src/qlang/repl/q_highlight.c - change both together.
.pq.hl:`kw`str`esc`cmt`sym`num`tmp`op`sys`cmd`match!(33;114;168;244;37;166;136;141;127;160;"1;31");

/ @ignore
.pq.drsamples:{[k] c:"bgxhijefcspmdznuvt"; if[not k; :@[.'[$;;`$]c,'1;c?"gs";:;(0Ng;`abc)]];
  g:"G"$("8c680a01-5a49-5aab-5a65-d4bfddb6a661";"5ae7962d-49f2-404d-5aec-f7c8abbae288";"5a580fb6-656b-5e69-d445-417ebfe71994");
  (101b;g),("xhijef"$\:3 1 2),("bca";`b`ca`abc),(10_c)$\:2022.06.04D10:20:30.4 2023.01.02D03:04:05.006 2021.12.31D23:59:59.999}
/ @ignore
.pq.drtype:{[x] c:"bgxhijefcspmdznuvt"; $[x~`err;".";$[0>type x;c;upper c]("h"$(1+til 19)except 3)?abs type x]}
/ @ignore
.pq.drform:{[f;m] c:"bgxhijefcspmdznuvt"; v:.pq.drsamples not m~`dyadic;
  n:$[-11h=type f;string f;-10h=type f;enlist f;f];
  g:$[not (type f)in -11 -10 10h;f;m~`dyadic;eval parse "{x ",n," y}";eval parse n];
  if[m~`dyadic; -1 {x," | ",1_raze " ",'y}'[c;{[g;v;x]{[g;x;y].pq.drtype .[g;(x;y);{`err}]}[g;x]each v}[g;v]each v]; :(::)];
  r:$[m~`monadic;::;lower].pq.drtype each{[g;x].[g;enlist x;{`err}]}[g]each v;
  -1 ("domain:";"range: "),'{raze " ",'x}each($[m~`monadic;upper c;c];r);}
/ Print a verb's domain and range grid in the layout the KX reference pages use, probed with a sample of every type
/ (an atom each side of a dyad, a three-item vector for a monad): a lower-case letter is an atom result, upper-case
/ a list, "." an error.  After KX's tools.q (qdocs/docs/docs/tools.q, CC BY 4.0).
/ @param verb a symbol or char naming a verb in .Q.ops[], or a function
/ @eg .pq.dr `neg
/ @eg .pq.dr "+"
.pq.dr:{[verb] t:.Q.ops[]; i:(t`name)?$[-11h=type verb;verb;`$$[-10h=type verb;enlist verb;verb]]; $[i>=count t`name;"not in .Q.ops[]";t[`dyadic]i;.pq.drform[verb;`dyadic];.pq.drform[verb;`monadic]]}

/ THE name resolver of every door that answers a select somewhere else (a DuckDB table, a parquet file, a q peer's table,
/ a kafka topic): one walk, so a name means the same through each.  .pq.i.resolve[cl;env;tree] answers the select's
/ arguments as ? takes them (table;where terms;by;phrase;...) with every name that is no column (cl) replaced by its
/ value, written as parse writes a constant (.pq.i.lit).  env is what each name means where the select was written, a
/ local included (the select seam hands it over, q_funsql.c ques_env); :: when the tree came as data and its names are
/ globals.  A one-item list is a constant already and is left alone.
/ 'unpush leaves the select to q, in the frame it was written in: a name with no value, an assignment, a nested select
/ whose table cannot be read here (it reads a column, or is no table) or that names a column of the enclosing one.
/ @ignore
.pq.i.lit:{[v] $[(type v) in -11 11 0h;enlist v;v]}
/ a name: a column, or a dotted name under one, stays; any other is its value.  dep are the columns of the enclosing
/ selects that a nested one's table does not have.
/ @ignore
.pq.i.rname:{[cl;dep;env;x]
  if[null x; :x];
  s:"." vs string x; h:`$first s;
  if[(x in cl) or (h in cl) and not null h; :x];
  if[any (x,h) in dep; '`unpush];
  .pq.i.lit $[(null h) or env~(::);@[get;x;{[e] '`unpush}];
    not h in key env;'`unpush;
    1=count s;env h;
    .[{[d;k] d . k};(env h;`$1_s);{[e] '`unpush}]]}
/ a select inside the select, (?;table;where;by;phrase;...) as parse writes its slots
/ @ignore
.pq.i.sel:{[x] (any (x 0)~/:(?;!)) and (count x) in 5 6 7}
/ one expression; a projection's elided argument is no expression
/ @ignore
.pq.i.rs:{[cl;dep;env;x]
  t:.Q.c.lty x;
  if[-11h=t; :.pq.i.rname[cl;dep;env;x]];
  if[(not t in 0 11h) or 2>count x; :x];
  if[any (x 0)~/:(:;::); '`unpush];
  if[.pq.i.sel x; :.pq.i.rsub[cl;dep;env;x]];
  n:$[11h=t;.z.s[cl;dep;env] each x;@[x;where 101h<>type each x;.z.s[cl;dep;env]]];
  if[not (.Q.c.lty n) in 0 11h; '`unpush];
  n}
/ A nested select's table is read first, once, so its names are resolved against its own columns.
/ @ignore
.pq.i.rsub:{[cl;dep;env;x]
  src:.pq.i.rs[cl;dep;env] x 1; if[not .pq.i.free src; '`unpush];
  t:@[eval;src;{[e] '`unpush}]; if[not .Q.qt t; '`unpush];
  ic:(cols t),`i; f:.pq.i.rs[ic;(cl,dep) except ic;env]; g:{[f;p] $[99h=type p;(key p)!f each value p;(0h=type p) and 1=count p;enlist f first p;p]};
  w:x 2; w:$[(0h=type w) and (1=count w) and 0h=type first w;enlist f each first w;w];
  (x 0;t;w;g[f] x 3;g[f] x 4),.pq.i.rs[cl;dep;env] each 5_x}
/ Whether a resolved part reads no column.  value, eval, get and parse of text or of a name read the select's columns
/ where q runs them, so they count as reading one.
/ @ignore
.pq.i.free:{[x]
  t:.Q.c.lty x; if[-11h=t; :null x]; if[(not t in 0 11h) or 2>count x; :1b]; if[.pq.i.sel x; :1b];
  a:x 1; a:$[(0h=type a) and 1=count a;first a;a];
  $[(any (x 0)~/:(value;eval;get;parse)) and (type a) in -11 -10 0 10 11h;0b;all .z.s each x]}
/ A resolved where term with each operand that reads no column, beside one that reads one, run here and its value in
/ its place (col<verb>expr, either way round); an operand that fails is left for q.  The branches of a cond are run
/ only with the whole of it.
/ @ignore
.pq.i.rfold:{[x]
  if[(0h<>.Q.c.lty x) or 2>count x; :x]; if[.pq.i.sel[x] or (($)~x 0) and 3<count x; :x];
  x:@[x;where 101h<>type each x;.z.s]; f:.pq.i.free each x;
  if[all f; :x];
  @[x;where f and {[y] ((.Q.c.lty y) in 0 11h) and 1<count y} each x;{[y] @[{[y] .pq.i.lit eval y};y;{[y;e] y}[y]]}]}
/ @ignore
.pq.i.resolve:{[cl;env;tree]
  f:.pq.i.rs[cl,`i;();env]; g:{[f;x] $[99h=type x;(key x)!f each value x;(0h=type x) and 0<count x;f x;x]};
  (tree 0;{[f;x] .pq.i.rfold f x}[f] each tree 1;g[f] tree 2;g[f] tree 3),4_tree}
/ The tree a provider's qsql hook is handed ends with the dict of what its names mean to the caller (the select seam,
/ q_funsql.c ques_pushed): (env;the tree without it), env :: for a tree that carries none.
/ @ignore
.pq.i.seam:{[tree] e:(4<count tree) and 99h=type last tree; ($[e;last tree;::];$[e;-1_tree;tree])}
/ the same over a seam tree, for a door's hook
/ @ignore
.pq.i.resolveSeam:{[cl;tree] .pq.i.resolve[cl] . .pq.i.seam tree}
/ Every symbol in a tree but a dict's keys, the names a select gives its outputs; and by them the columns of cl a resolved
/ select rt reads, in cl's order: all under an empty phrase, and p for p.date.  A literal symbol counts: one column too many is harmless.
/ @ignore
.pq.i.syms:{[x] $[99h=type x;raze .z.s each value x;0h=type x;raze .z.s each x;11h=abs type x;(),x;`$()]}
/ @ignore
.pq.i.reads:{[cl;rt]
  if[()~rt 3; :cl];
  n:.pq.i.syms 1_rt; cl where cl in n,`$first each "." vs/: string n}

.pq.i.q.bind:{[c;t] c ({[t] cols get t};t)};
.pq.i.q.get:{[c;t] c (get;t)};
.pq.i.q.set:{[c;t;d] c (set;t;d); t};
.pq.i.q.upsert:{[c;t;d] c (upsert;t;d); t};
.pq.i.q.hdel:{[c;t] c (!;`.;();0b;enlist t); t};
.pq.i.q.qsql:{[c;cl;tree] c (?),.pq.i.resolveSeam[cl;tree]};

/ One line of facts about a table, one per column in column order: numeric columns as a range, booleans as the
/ share true, everything else as a distinct count (`all distinct` when every row differs; a temporal column that
/ repeats shows its range), with a null count where there are nulls.  The same facts the console digest prints
/ under a clipped table, so the two never disagree.
/ @param t (table) a table or keyed table
/ @return string, `-1`-ready
/ @eg .pq.summary ([]a:1 2 3;b:`x`y`x)
.pq.summary:{[t] "  " sv .pq.i.facts t};

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

/ @ignore
.pq.i.qcmd_prompt:{[line]
  if[line like "q)*"; :2];
  if[not line like "q.*"; :0];
  close:line?")";
  name:line 2_til close;
  named:all{(0<count x) and (first[x] in .Q.a,.Q.A) and all x in .Q.an}each "." vs name;
  $[named and close<count line; close+1; 0]};

/ @ignore
/ Internal only, not user-facing: do not document in docs, help or --help until the owner decides to expose it.
.pq.i.qcmd:{[lines]
  lines:$[10h=type lines; "\n" vs lines; lines];
  at:.pq.i.qcmd_prompt each lines;
  keep:where at>0;
  texts:at[keep] _' lines keep;
  heads:where not texts like ")*";   / a q)) line is input to the debugger the q) line before it suspends into
  texts:{x where maxs not x=")"} each texts;
  lead:texts til first heads,count texts;
  if[count lead; .pq.i.console[::;lead]];
  {.pq.i.console[first x;1_x]} each heads cut texts};

/ The text as a q string literal's body (qStudio's KdbHelper.escape, in its order): backslash, tab, CR, LF, quote.
.pq.i.remote_escape:{[text]
  text:"",text;
  special:"\\\t\r\n\"";
  escaped:("\\\\";"\\t";"\\r";"\\n";"\\\"");
  at:where text in special;
  $[count text; raze @[enlist each text;at;:;escaped special?text at]; text]};

/ docs/text-entry-law.md: -conn -eval is IPC text; -conn f.q is via file, on the client

/ Evaluate q text on a server, the way qStudio does: the wrapper round the escaped text, one sync call.
/ @param handle (int) an open handle
/ @param text (string) the q text, read by the IPC column: one expression, `;` the only statement separator
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
  triple:$[(0h=.Q.c.lty reply) and 3=count reply; (-1h=type reply 0) and 10h=type reply 2; 0b];
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

/ The text the console prints for x in the session's display mode.
.pq.i.console_text:{[x] $[system "classic"; .Q.s x; .pq.render_modern x]};

/ The console display of a remote result: the value as the console shows it, and under the classic display a
/ table's summary line - the modern display digests a clipped table itself and shows a small one whole.
/ @param val (any) the unwrapped value
.pq.i.remote_show:{[val]
  if[(::)~val; :(::)];
  1 .pq.i.console_text val;
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
  whole:@[.pq.i.console_text;val;{[size;err] system size; 'err}[size]];
  system size;
  not whole~.pq.i.console_text val};

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

/ The launcher's connection under -conn: any hopen target as written - all digits a local port, anything else the
/ symbol - with the connect timeout; the handle, or hopen's own reason on stderr and exit 2.  A port is spelled `::port:
/ (port;timeout) is a long vector, which hopen (it IS <:) grades.
.pq.i.conn_open:{[target]
  where_to:`$$[(count target) and all target in .Q.n; "::",target; target];
  @[hopen;(where_to;5000);{[target;err] -2 "q: cannot connect to ",target,": ",err; exit 2}[target]]};

/ One launcher call under -conn: a display, or with a file the last text's value saved and the earlier ones run
/ silently, as a script's non-final statements; a quiet text (a statement of the -conn script) is never displayed.
/ A remote error is its class on stderr and exit 1, as a failing script; the caller announces the answered path once
/ the display has left the console.
.pq.i.conn_call:{[handle;text;file;final;quiet]
  to_file:{[h;f;t] .pq.i.remote_save[h;f;t]; ""};
  silently:{[h;t] .pq.i.remote_query[h;t]; ""};
  run:$[final and count file; to_file[handle;file]; quiet or count file; silently[handle]; .pq.i.remote_run[handle]];
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
.pq.i.qhist_target:{[h]
  addr:exec address from .pq.conns[] where handle=h;
  $[count addr; ":",first addr; ":",string h]};

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

/ Every standard-library file, in load order.
.pq.i.files:`csv`duckdb`s`ffi`hash`crypto`j`kafka`massive`md`parquet`pq`regexp`str`termbox`fs`path`pkg`qunit`yml;

/ A file's one-line examples: the text after "/ @eg " on every line that carries one.
.pq.i.egs:{[file] lines:read0 file; 6_/:lines where lines like "/ @eg ?*"};

/ Run each file as \l pq/<file>.q: a pq/ directory's (the working directory's, then QHOME's), else the built-in copy.
.pq.i.run:{[files] {system "l pq/",string[x],".q"} each files; files};

/ Load standard-library files not already loaded; \l pq/<file>.q always reloads.
/ @param files (symbol|symbol list) the files to load; [] for every library file
/ @return (symbol list) the files this call loaded
/ @eg .pq.load`regexp
.pq.load:{[files] .pq.i.run $[files~(::);.pq.i.files;(),files] except .pq.i.loaded[]};
