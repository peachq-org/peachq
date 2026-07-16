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

/ ---- wave 3: the sort wave — every sort verb derives from the grade ----
/ ref/asc.md: atom = already sorted (carries the RAY_STR atom too); 99h = dict AND keyed
/ table, entries gathered by the value grade (the non-key-column rule; .Q.ft would sort a
/ keyed table by its KEY cols).  No `s#: the attr-take arm takes longs only (PLAN.md).
.q.asc:{$[0h>type x;x;99h=type x;(key x)[g]!(value x)g:iasc value x;x iasc x]}
.q.desc:{$[0h>type x;x;99h=type x;(key x)[g]!(value x)g:idesc value x;x idesc x]}
/ ref/rank.md: "the same as calling iasc twice on the list"
.q.rank:{iasc iasc x}
/ ref/xrank.md prints no source; bucket = floor(x*rank y % count y) reproduces all 5
/ of its examples (4 xrank til 8/9, 7 xrank til 9, 3 xrank 1 37 5 4 0 3 / 1 7 5 4 0 3)
.q.xrank:{(x*rank y) div count y}
/ ref/asc.md: by the first column given, then the second within it = the grade of the named
/ columns, then ONE gather.  y a SYMBOL updates in place, returns the name (set returns its
/ target).  t@/:x throws 'domain on a bad column — (flip t)x misses silently and truncates.
.q.xasc:{[x;y]$[-11h=type y;y set .q.xasc[x;get y];.Q.ft[{[x;t]t iasc flip x!t@/:x:$[0h>type x;1#x;x]}[x;];y]]}
.q.xdesc:{[x;y]$[-11h=type y;y set .q.xdesc[x;get y];.Q.ft[{[x;t]t idesc flip x!t@/:x:$[0h>type x;1#x;x]}[x;];y]]}

/ ---- wave 3 (ref/cols.md) ----
/ lifted via .Q.ft (defined later, in dotq.q — a lambda resolves it at call time);
/ names pair with the unchanged column values, so a bad rename/reorder goes ragged
/ at `!` and throws its own 'length (ref/cols.md: nonexistent key x -> 'length)
.q.xcol:{[x;y] .Q.ft[{[x;t] c:cols t; flip ($[99h=type x;{[c;m](c^m c),(key m)except c}[c;$[98h=type key x;first each flip key x;x]];x,(count x)_c])!value flip t}[x;];y]}
.q.xcols:{[x;y] .Q.ft[{[x;t] c:cols t; n:x,c except x; flip n!(flip t) n inter c}[x;];y]}

/ ---- wave 4: verbs with no kernel to lose ----
/ ref/all-any.md: CAST then fold — (&/)1 2 3 is 1 (a raw min), but `all 1 2 3` is 1b.
/ Three arms the fold cannot serve: 98h table "iterates over its columns and returns a
/ dictionary" (flip gives the 99h dict; each gives the doc's shape); an atom cast is an
/ atom ("a nonzero atom" -> 1b) and folding an atom throws; the empty list is the fold's
/ identity ("this includes the empty list" -> 1b).  Sym/GUID exclusion is the cast's own
/ 'type, never a hand-written gate.  Bare all/any recurse via .q at call time.
.q.all:{$[98h=type x;all each flip x;0h>type b:"b"$x;b;0=count b;1b;(&/)b]}
.q.any:{$[98h=type x;any each flip x;0h>type b:"b"$x;b;0=count b;0b;(|/)b]}
/ ref/tables.md "default is root namespace" — and bare `system"a"` lists the CURRENT
/ context, so [] must pass `. itself.  f[] binds x to :: , but type/null/count/string
/ all THROW on :: and `(::)~x` elides to a projection (PLAN.md) — matching against a
/ constructed null is the one total test.  A bad namespace throws \a's own error.
.q.tables:{system"a ",string $[({x}[])~x;`.;x]}
/ ref/view.md: views[] is niladic, "views defined in the default namespace".
.q.views:{system"b"}
