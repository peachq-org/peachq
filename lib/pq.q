/ The session itself: version, open connections, terminal size and colour support.  Loaded by \l pq.
/ .
/ @eg .pq.version
/ @eg select handle, alias from .pq.conns[]

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
.pq.termsize:{2#.pq.i.termsize[]}

/ Whether stdout should carry ANSI colour: PEACHQ_COLORS=0/1 overrides everything, then NO_COLOR (off), FORCE_COLOR
/ (on), else a tty whose TERM is not dumb.
/ @return boolean
/ @eg .pq.cancolor[]
.pq.cancolor:{
  e:getenv`PEACHQ_COLORS;
  $[e~enlist"1";1b;e~enlist"0";0b;
    count getenv`NO_COLOR;0b;
    count getenv`FORCE_COLOR;1b;
    (0<last .pq.i.termsize[])and not "dumb"~getenv`TERM]}

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
