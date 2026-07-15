/ q.q — self-hosted q keywords (kdb: keywords are `.q` entries from q.k).
/ CONTRACT: one `.q.name:expr` per line; loads after registry init, before
/ dotq.q (which may use these names, never the reverse).  See q_runtime.c.

/ ---- wave 1 (ref/reciprocal.md, ref/next.md) ----
.q.reciprocal:%[1;]
.q.prev:xprev[1;]
.q.next:xprev[-1;]

/ ---- wave 2 ----
/ prefix keywords (no manifest row; bare name resolves via .q)
.q.rand:{first 1?x}
.q.deltas:(-':)
.q.differ:{not(~':)x}
/ ref/med.md "equivalent to {avg x (iasc x)@floor .5*-1 0+count x,:()}";
/ the atom-listify is spelled 1#x (`,:()` amend unsupported; `(),x` boxes)
.q.med:{x:$[0h>type x;1#x;x];avg x (iasc x)@floor .5*-1 0+count x}
/ infix keywords (lexer row stays; registry cell rebound from these post-load)
/ `max 0,`/`max 1,` stand in for k's `0|`/`1|` (dyadic | lands in wave 3)
.q.sublist:{$[2=count x;x[0]_((sum x)&count y)#y;0<=x;(x&count y)#y;(max 0,x+count y)_y]}
.q.rotate:{(k _ y),(k:x mod max 1,count y)#y}
.q.wsum:{sum x*"f"$y}
.q.cov:{avg[x*y]-avg[x]*avg y}
.q.scov:{cov[x;y]*count[x]%-1+count x}
.q.mavg:{(x msum y)%x mcount y}

/ ---- wave 3 (ref/cols.md) ----
/ lifted via .Q.ft (defined later, in dotq.q — a lambda resolves it at call time);
/ names pair with the unchanged column values, so a bad rename/reorder goes ragged
/ at `!` and throws its own 'length (ref/cols.md: nonexistent key x -> 'length)
.q.xcol:{[x;y] .Q.ft[{[x;t] c:cols t; flip ($[99h=type x;{[c;m](c^m c),(key m)except c}[c;$[98h=type key x;first each flip key x;x]];x,(count x)_c])!value flip t}[x;];y]}
.q.xcols:{[x;y] .Q.ft[{[x;t] c:cols t; n:x,c except x; flip n!(flip t) n inter c}[x;];y]}
