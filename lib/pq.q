/ pq.q — the PeachQ stdlib namespace (.pq). lib/*.q is the standard-library
/ tier: bundled sorted at build, loaded ONLY by the \l pq gate (never at
/ q_runtime_create) through the multiline statement seam. THE ANY-ORDER LAW:
/ top level is definitions only — no cross-lib calls execute at load time.
/ Pure q since the .pq.c.* rayfall escape hatch was deleted (2026-07-29).
.pq.version:.z.K;
/ .pq.conns — every open connection, 13 cols; the single-letter p/f/z/n/m are
/ DELIBERATELY the -38! names so kdb code ports, the readable columns are
/ peachq additions (the kdb verbs -38!/.z.W/.z.H stay socket-only).
.pq.conns:{.pq.i.conns[]}

/ the live terminal size — the same query, 25/80 fallback and [10,2000]
/ coercion that resolve the `\c 0N` auto axes
/ @return (long list) (rows;cols)
.pq.termsize:{2#.pq.i.termsize[]}

/ should stdout carry ANSI color?  PEACHQ_COLORS=0/1 overrides everything,
/ then NO_COLOR (off), FORCE_COLOR (on), else a tty whose TERM is not dumb
/ @return (boolean) 1b when color is safe
.pq.cancolor:{
  e:getenv`PEACHQ_COLORS;
  $[e~enlist"1";1b;e~enlist"0";0b;
    count getenv`NO_COLOR;0b;
    count getenv`FORCE_COLOR;1b;
    (0<last .pq.i.termsize[])and not "dumb"~getenv`TERM]}
/ .pq.dr — domain and range in the layout the ref pages publish, so ours diffs
/ by eye against theirs (after qdocs/docs/docs/tools.q, kx, CC BY 4.0).
/ Samples are rebuilt by CASTING each call, so a cast regression shows here.
/ A named dyad is probed as infix: `parse "*"` yields the MONADIC value, whose
/ 2-arg application 'ranks — which would read as an empty domain.
.pq.drsamples:{[k] c:"bgxhijefcspmdznuvt"; v:@[.'[$;;`$]c,'1;c?"gs";:;(0Ng;`abc)]; $[k;{3#x}each v;v]}
.pq.drtype:{[x] c:"bgxhijefcspmdznuvt"; $[x~`err;".";$[0>type x;c;upper c]("h"$(1+til 19)except 3)?abs type x]}
.pq.drform:{[f;m] c:"bgxhijefcspmdznuvt"; v:.pq.drsamples m~`monadic; n:$[-11h=type f;string f;-10h=type f;enlist f;f]; g:$[not (type f)in -11 -10 10h;f;m~`dyadic;eval parse "{x ",n," y}";eval parse n]; $[m~`dyadic;-1 {x," | ",1_raze " ",'y}'[c;{[g;v;x]{[g;x;y].pq.drtype .[g;(x;y);{`err}]}[g;x]each v}[g;v]each v];-1 ("domain:";"range: "),'{raze " ",'x}each($[m~`monadic;upper c;c];.pq.drtype each{[g;x].[g;enlist x;{`err}]}[g]each v)];}
.pq.dr:{[x] t:.Q.ops[]; i:(t`name)?$[-11h=type x;x;`$$[-10h=type x;enlist x;x]]; $[i>=count t`name;"not in .Q.ops[]";t[`dyadic]i;.pq.drform[x;`dyadic];t[`monadic]i;.pq.drform[x;`monadic];"no valence in .Q.ops[]"]}
/ .pq.i.resolveTree — THE shared pushdown resolver every .X.qsql hook rides (qpc now, B2
/ DuckDB next): free vars inline (sym-kind/containers enlist-wrapped, the parser's literal
/ law), columns stay symbolic (columns shadow variables), slot 0 rides verbatim; walks 0h/99h
/ nodes only (typed literals are leaves); virtual `i` counts as a column; an unresolvable
/ or function-valued name signals 'unpush — qpc PROPAGATES it (never a fallback pull),
/ duckdb declines to the host.  Free names resolve like value-of-string: globals, never locals.
.pq.i.rval:{[x] v:@[value;x;{[e] 'unpush}]; $[100h<=type v;'unpush;((11h=abs type v)or(type v) in 0 98 99h);enlist v;v]}
.pq.i.rnode:{[cl;x] $[(type x) in 0 99h;.z.s[cl] each x;-11h=type x;$[x in cl;x;.pq.i.rval x];x]}
.pq.i.resolveTree:{[cl;tree] (tree 0),.pq.i.rnode[cl,`i] each 1_tree}
