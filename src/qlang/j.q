.j.k:-29!;
.j.j:-31!;
/ .j.jd (x;d): .j.j, except d[`null0w]~1b maps 0w/-0w to null (ref/dotj.md).
/ d[`prec] is peachq's per-call `\P` (syscmds.md:546); absent = the live `\P`.
/ ref/dotj.md reserves `.j` for KX and publishes exactly j/k/jd, so both helpers
/ are LOCALS.  `n` recurses by SELF-PASSING (`n[n;]`) — q lambdas do not see
/ enclosing locals and peachq has no `.z.s`.  Its float arm also takes real (8h),
/ substituting 0Ne so a real vector stays real; the atom takes `$` because `?`
/ wants a boolean VECTOR.  `jp` is trapped so a signalling .j.j cannot strand `\P`.
.j.jd:{a:$[(0h=type x)&2=count x;x;(x;()!())];d:a 1;n:{[s;x]t:type x;b:abs t;nu:$[8h=b;0Ne;0n];$[b in 8 9h;$[t>0;?[(x=0w)|x=-0w;nu;x];$[(x=0w)|x=-0w;nu;x]];0h=t;s[s;] each x;99h=t;(key x)!s[s;] value x;98h=t;flip s[s;] flip x;x]};jp:{[p;x]o:system"P";system"P ",string p;r:@[.j.j;x;enlist];system"P ",string o;$[0h=type r;'first r;r]};v:$[1b~d`null0w;n[n;] a 0;a 0];$[`prec in key d;jp[d`prec;v];.j.j v]};
