/ q.q - self-hosted q keywords (kdb: keywords are `.q` entries from q.k).
/ CONTRACT: one `.q.name:expr` per line; loads after registry init, before
/ dotq.q (which may use these names, never the reverse).  See q_runtime.c.

/ ---- wave 1 (ref/reciprocal.md, ref/next.md) ----
.q.reciprocal:(%:)
.q.next:{-1 xprev x}

/ ---- wave 2 ----
/ prefix keywords (no manifest row; bare name resolves via .q)
.q.rand:{first 1?x}
/ a variadic keyword that covers an extension IS it, unary and binary-bracket alike (basics/exposed-infrastructure.md:72-113)
.q.deltas:(-':)
/ ref/value.md:89-92: value differ is $["b"] and ~~':
.q.differ:('["b"$;not;~':])
/ the covers with a QR_COVER manifest row: the value is kdb's, the row keeps the name reserved and names the C kernel
/ the apply module runs when the value is applied (ref/raze.md:50, exposed-infrastructure.md:72)
.q.sums:(+\)
.q.prds:(*\)
.q.maxs:(|\)
.q.mins:(&\)
.q.raze:(,/)
.q.ratios:(%':)
/ kdb-written bytes W-EXX-091
.q.prev:(:':)
/ ref/fill.md:136
.q.fills:(^\)
/ docs-v1 ref/logic.md:15,43; kdb-written bytes W-EXX-087
.q.any:('[max;"b"$])
.q.all:('[min;"b"$])
/ ref/union.md:18: distinct x,y
.q.union:('[distinct;,])
/ wp/permissions/index.md:387
.q.upsert:(.[;();,;])
/ basics/internal.md:15
.q.attr:(-2!)
/ the parts are inferred from the kdb-written 105h type byte: the least integer not below x
.q.ceiling:('[neg;floor;neg])
/ ref/md5.md: verbatim q.k spelling - the -15! internal fn (q_bang.c -> q_md5_fn)
.q.md5:(-15!)
/ ref/hcount.md: file size in bytes - the -7! internal fn (q_bang.c -> h_hcount)
.q.hcount:(-7!)
/ ref/eval.md over basics/internal.md: eval IS the -6! internal (q_bang.c -> q_eval)
.q.eval:(-6!)
/ ref/med.md "equivalent to {avg x (iasc x)@floor .5*-1 0+count x,:()}";
/ the atom-listify is spelled 1#x (`,:()` amend unsupported; `(),x` boxes)
.q.med:{x:$[0h>type x;1#x;x];avg x (iasc x)@floor .5*-1 0+count x}
/ infix keywords (lexer row stays; registry cell rebound from these post-load)
/ the iterator keywords: ref/parse.md:72 shows each as k){x'y}, learn/views.md:264 peach as k){x':y}; over scan prior
/ take the same shape undocumented.  q spells a derived function applied prefix bracketed (ref/iterators.md:94-100)
.q.each:{x'[y]}
.q.over:{x/[y]}
.q.scan:{x\[y]}
.q.prior:{x':[y]}
.q.peach:{x':[y]}
/ `max 0,`/`max 1,` stand in for k's `0|`/`1|` (dyadic | lands in wave 3)
.q.sublist:{$[2=count x;x[0]_((sum x)&count y)#y;0<=x;(x&count y)#y;(max 0,x+count y)_y]}
/ ref/sum.md "equivalent to {sum x*y}", float ONLY "when both x and y are
/ integer lists" - an atom operand is not a list, so it never promotes
.q.wsum:{sum x*$[(type x)in 5 6 7h;$[(type y)in 5 6 7h;"f"$y;y];y]}
.q.cov:{x:"f"$x; y:"f"$y; avg[x*y]-avg[x]*avg y}
.q.scov:{cov[x;y]*count[x]%-1+count x}
.q.mavg:{(x msum y)%x mcount y}
/ ref/fby.md prints the law: `(sum each dat group grp)grp`; x is the (aggr;d)
/ pair.  Since V2.7 an aggregate may answer a LIST per group - then the results
/ go back to their own rows, the raze un-permuted by iasc of the razed indices.
.q.fby:{[x;y] g:group y; r:(first x) each (last x) g; $[0h>type first value r; r y; (raze value r) iasc raze value g]}

/ ---- wave 3: the sort wave - every sort verb derives from the grade ----
/ ref/asc.md: atom = already sorted (carries the RAY_STR atom too); 99h = dict AND keyed
/ table, entries gathered by the value grade (the non-key-column rule; .Q.ft would sort a
/ keyed table by its KEY cols).  asc.md:18-28 - s stamps the attribute via the producer
/ seams .Q.c.sorted/.Q.c.parted: a lone table column takes `s#, several take `p# on the
/ FIRST (asc.md's meta rows); a keyed table stamps its VALUE table only (`::` covers the
/ plain-dict value).  desc.md:110 - desc sets NO attribute.
.q.asc:{s:{$[98h<>type x;.Q.c.sorted x;0=count c:cols x;x;1=count c;@[x;first c;.Q.c.sorted];@[x;first c;.Q.c.parted]]};$[0h>type x;x;99h=type x;(key x)[g]!$[98h=type value x;s;::](value x)g:iasc value x;s $[(g:iasc x)~til count x;x;x g]]}
.q.desc:{$[0h>type x;x;99h=type x;(key x)[g]!(value x)g:idesc value x;x idesc x]}
/ ref/rank.md: "the same as calling iasc twice on the list"
.q.rank:{iasc iasc x}
/ ref/xrank.md prints no source; bucket = floor(x*rank y % count y) reproduces all 5
/ of its examples (4 xrank til 8/9, 7 xrank til 9, 3 xrank 1 37 5 4 0 3 / 1 7 5 4 0 3)
.q.xrank:{(x*rank y) div count y}
/ ref/asc.md: by the first column given, then the second within it = the grade of the named
/ columns, then ONE gather.  y a SYMBOL updates in place, returns the name (set returns its
/ target).  A bad column is 'domain - t@/:x would answer the first column's empty and truncate.
/ asc.md:189 - the sorted attribute is set on the FIRST column given; xdesc sets none.  No columns: t unchanged.
.q.xasc:{[x;y]$[-11h=type y;y set .q.xasc[x;get y];.Q.ft[{[x;t]x:$[0h>type x;1#x;x];if[not all x in cols t;'domain];$[count x;@[t iasc flip x!t@/:x;first x;.Q.c.sorted];t]}[x;];y]]}
.q.xdesc:{[x;y]$[-11h=type y;y set .q.xdesc[x;get y];.Q.ft[{[x;t]x:$[0h>type x;1#x;x];if[not all x in cols t;'domain];$[count x;t idesc flip x!t@/:x;t]}[x;];y]]}

/ ---- wave 3 (ref/cols.md) ----
/ lifted via .Q.ft (defined later, in dotq.q - a lambda resolves it at call time);
/ names pair with the unchanged column values, so a bad rename/reorder goes ragged
/ at `!` and throws its own 'length (ref/cols.md: nonexistent key x -> 'length)
.q.xcol:{[x;y] .Q.ft[{[x;t] c:cols t; flip ($[99h=type x;{[c;m] w:where c in k:key m; @[c;w;:;m c w],k except c}[c;$[98h=type key x;first each flip key x;x]];x,(count x)_c])!value .Q.V t}[x;];y]}
/ a dictionary's keys stand for the columns (TorQ dataaccess/queryorder.q:41; owner 2026-11-09)
.q.xcols:{[x;y] $[(99h=type y)&not .Q.qt y;{[x;d] k:key d; n:x,k except x; n!d n inter k}[x;y];.Q.ft[{[x;t] flip .q.xcols[x;.Q.V t]}[x;];y]]}

/ ---- wave 4: verbs with no kernel to lose ----
/ ref/tables.md "default is root namespace" - and bare `system"a"` lists the CURRENT
/ context, so [] must pass `. itself.  f[] binds x to :: , and the kdb null-test
/ idiom `(::)~x` is the one total test.  A bad namespace throws \a's own error.
.q.tables:{system"a ",string $[(::)~x;`.;x]}
/ ref/view.md: views[] is niladic, "views defined in the default namespace".
.q.views:{system"b"}

/ ---- wave 5 (ref/save.md, ref/load.md) ----
/ `[path/to/]v.ext`: get the global v, dispatch .ext through .h.tx, write with Save Text
/ (which creates parent dirs, overwrites, and returns the filename), or with Save Binary
/ when the entry answers bytes (`parquet) - no formatting of its own, every byte comes from
/ .h.tx/0:/1:.  No .ext IS the binary arm, and ref/save.md states its equivalence outright:
/ `save `t` is `` `:t set t ``.  An .ext .h.tx does not key signals that ext (`xlsx).
.q.save:{f:{p:"." vs last "/" vs 1_string h:hsym x;$[2>count p;h set get `$"." sv p;not (e:`$last p) in key .h.tx;'e;4h=type r:.h.tx[e] get `$"." sv -1_p;h 1: r;h 0: r]};$[-11h=type x;f x;11h=type x;f each x;'`type]}
/ ref/load.md: `load `t` is `t:get `:t` - the file's name IS the global's, and the name is
/ returned; a filesymbol names the same global.  The directory-recursion arm is deferred.
.q.load:{(`$last "/" vs 1_string h) set get h:hsym x}'
/ ref/save.md rsave, ref/load.md rload: a table name and the splayed directory of the same name; ref/dsave.md: each
/ table enumerated against the root (the first item of x), `p# on its first column, splayed under the path x
.q.rsave:{(`$":",string[x],"/") set get x}'
.q.rload:{$[-11h=type x;x set get `$":",string[x],"/";'`type]}'
.q.dsave:{[x;y] t:.Q.en[first x] get y; (` sv x,y,`) set @[t;first cols t;`p#]; y}/:
/ basics/internal.md: reval is the -24! internal
.q.reval:(-24!)
