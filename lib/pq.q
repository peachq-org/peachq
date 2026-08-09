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
/ .pq.dr — domain and range in the layout the ref pages publish, so ours diffs
/ by eye against theirs (after qdocs/docs/docs/tools.q, kx, CC BY 4.0).
/ Samples are rebuilt by CASTING each call, so a cast regression shows here.
/ A named dyad is probed as infix: `parse "*"` yields the MONADIC value, whose
/ 2-arg application 'ranks — which would read as an empty domain.
.pq.drsamples:{[k] c:"bgxhijefcspmdznuvt"; v:@[.'[$;;`$]c,'1;c?"gs";:;(0Ng;`abc)]; $[k;{3#x}each v;v]}
.pq.drtype:{[x] c:"bgxhijefcspmdznuvt"; $[x~`err;".";$[0>type x;c;upper c]("h"$(1+til 19)except 3)?abs type x]}
.pq.drform:{[f;m] c:"bgxhijefcspmdznuvt"; v:.pq.drsamples m~`monadic; n:$[-11h=type f;string f;-10h=type f;enlist f;f]; g:$[not (type f)in -11 -10 10h;f;m~`dyadic;eval parse "{x ",n," y}";eval parse n]; $[m~`dyadic;-1 {x," | ",1_raze " ",'y}'[c;{[g;v;x]{[g;x;y].pq.drtype .[g;(x;y);{`err}]}[g;x]each v}[g;v]each v];-1 ("domain:";"range: "),'{raze " ",'x}each($[m~`monadic;upper c;c];.pq.drtype each{[g;x].[g;enlist x;{`err}]}[g]each v)];}
.pq.dr:{[x] t:.Q.ops[]; i:(t`name)?$[-11h=type x;x;`$$[-10h=type x;enlist x;x]]; $[i>=count t`name;"not in .Q.ops[]";t[`dyadic]i;.pq.drform[x;`dyadic];t[`monadic]i;.pq.drform[x;`monadic];"no valence in .Q.ops[]"]}
