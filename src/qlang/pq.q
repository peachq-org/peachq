/ pq.q — the PeachQ stdlib. Loaded ONLY by the pq load-gate (never at
/ q_runtime_create). The single visible PeachQ manifest; .pq.c.* natives are
/ internal/unstable and bound in C at the gate (q_pq.c) before this text
/ evaluates. Each member is ONE line — the loader (q_pq_load) evals
/ line-at-a-time, skipping blank / comment lines. (Codegen: a backslash is
/ emitted VERBATIM into the C string literal, so keep NONE here — only a valid
/ C escape survives; an invalid one breaks the build.)
system "nonlegacy 1";
.pq.version:.z.K;
/ .pq.ray: the generic rayfall (lisp) escape hatch. Results are RAW engine values
/ — exotic rayfall shapes may display oddly through q_fmt (accepted; internal).
.pq.ray:.pq.c.ray;
/ .pq.parse / .pq.tree: rayforce-native tree introspection via ray_fmt (unlike
/ `parse`, which renders q notation). .pq.parse = raw pre-lower AST; .pq.tree =
/ post-lower tree (what ray_eval receives). Both take/return a q char-vector.
.pq.parse:.pq.c.parse;
.pq.tree:.pq.c.tree;
/ .pq.dr — domain and range in the layout the ref pages publish, so ours diffs
/ by eye against theirs (after qdocs/docs/docs/tools.q, kx, CC BY 4.0).
/ Samples are rebuilt by CASTING each call, so a cast regression shows here.
/ A named dyad is probed as infix: `parse "*"` yields the MONADIC value, whose
/ 2-arg application 'ranks — which would read as an empty domain.
.pq.drsamples:{[k] c:"bgxhijefcspmdznuvt"; v:@[.'[$;;`$]c,'1;c?"gs";:;(0Ng;`abc)]; $[k;{3#x}each v;v]}
.pq.drtype:{[x] c:"bgxhijefcspmdznuvt"; $[x~`err;".";$[0>type x;c;upper c]("h"$(1+til 19)except 3)?abs type x]}
.pq.drform:{[f;m] c:"bgxhijefcspmdznuvt"; v:.pq.drsamples m~`monadic; n:$[-11h=type f;string f;-10h=type f;enlist f;f]; g:$[not (type f)in -11 -10 10h;f;m~`dyadic;eval parse "{x ",n," y}";eval parse n]; $[m~`dyadic;-1 {x," | ",1_raze " ",'y}'[c;{[g;v;x]{[g;x;y].pq.drtype .[g;(x;y);{`err}]}[g;x]each v}[g;v]each v];-1 ("domain:";"range: "),'{raze " ",'x}each($[m~`monadic;upper c;c];.pq.drtype each{[g;x].[g;enlist x;{`err}]}[g]each v)];}
.pq.dr:{[x] t:.Q.ops[]; i:(t`name)?$[-11h=type x;x;`$$[-10h=type x;enlist x;x]]; $[i>=count t`name;"not in .Q.ops[]";t[`dyadic]i;.pq.drform[x;`dyadic];t[`monadic]i;.pq.drform[x;`monadic];"no valence in .Q.ops[]"]}
