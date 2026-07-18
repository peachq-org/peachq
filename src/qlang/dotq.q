/ dotq.q — openq's embedded q bootstrap: the ".Q" utility namespace, authored from the
/ PUBLISHED qdocs (qdocs/, CC BY 4.0). A REAL .q file, embedded via build-time codegen
/ (tools/gen-bootstrap.sh -> src/qlang/dotq_gen.h) and loaded at the tail of q_runtime_create.
/ Loader is line-at-a-time: each definition MUST be ONE line (some exceed 120 by necessity).

/ ---- C-backed members: each delegates to its raw `.Q.c.*` primitive (bound in C before this loads; `.Q.c.*` is internal/unstable) ----
.Q.id:.Q.c.id;
.Q.ty:.Q.c.ty;
.Q.qt:.Q.c.qt;
.Q.qp:.Q.c.qp;
.Q.s:.Q.c.s;
.Q.btoa:.Q.c.btoa;
.Q.atob:.Q.c.atob;
.Q.sha1:.Q.c.sha1;
.Q.ops:.Q.c.ops;
.Q.res:.Q.c.res;
.Q.hg:.Q.c.hg;
.Q.hp:.Q.c.hp;
.Q.gz:.Q.c.gz;

/ ---- Constants (ref/dotq.md) ----
.Q.b6:"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
.Q.A:"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
.Q.a:"abcdefghijklmnopqrstuvwxyz";
.Q.an:"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789";
.Q.n:"0123456789";
.Q.nA:"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
/ .Q.k (version number) / .Q.K (version date): openq's own, tracking .z.K/.z.k (from the VERSION file).
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
/ .Q.opt: argv -> dict; flags start "-", value = tokens up to next flag (0->();1->str;many->list); sets .Q.x.
.Q.opt:{[a] a:$[-10h=type a;enlist a;a]; .Q.x:(); $[count a;{[a] i:where {"-"~1#x} each a; e:(1_i),count a; .Q.x:$[count i;(first i)#a;a]; k:`$1_'a i; j:til count i; k!{[a;i;e;j] s:(1+i j)_(e j)#a; $[1=count s;first s;s]}[a;i;e;]each j}[a];(`$())!()]};
/ .Q.def: defaults + tok-typed coercion over .Q.opt output (typed null on absent value / bad coerce).
.Q.def:{[d;o] key[d]!{[d;o;k] $[k in key o;$[-10h=type o k;(type d k)$o k;first 0#d k];d k]}[d;o;]each key d};

/ ---- Precision format (ref/dotq.md): thin wrappers over -27! (IEEE754 precision format) ----
/ .Q.f[x;y]: y to x decimals as a string. DECIMAL rounding (not -27!'s IEEE754): nudge y by a
/ tiny relative epsilon so decimal ties cross the boundary (.045->"0.05", cf -27!(2i;.045)->"0.04").
.Q.f:{-27!(x;(1+1e-15)*"f"$y)};
/ .Q.fmt[x;y;z]: z as a string of LENGTH x, formatted to y decimal places (left-padded via `$`).
.Q.fmt:{[x;y;z] neg[x]$.Q.f[y;z]};

/ ---- Value / table-dict (ref/dotq.md) ----
/ .Q.v: filepath -> splayed table (unsupported in-memory); other symbol -> global named x; else -> x.
.Q.v:{$[-11h=type x;value x;x]};
/ .Q.V: table -> dictionary of its column values (a table is a flipped column-dict).
.Q.V:{flip x};

/ ---- General-purpose utils (ref/dotq.md) ----
.Q.dd:{` sv x,`$string y};
/ .Q.addmonths: x(date) + y months; a day offset past the shorter target month spills forward.
.Q.addmonths:{[x;y] (`date$(`month$x)+y)+x-`date$`month$x};
/ .Q.fu: eval unary f on the DISTINCT items of y then reindex to full length (atom y -> f y).
.Q.fu:{[f;y] $[0>type y;f y;{[f;y;u]f[u]u?y}[f;y]distinct y]};
/ .Q.ft: apply f to a keyed table's simple (0!) form, re-key on the original key cols.
.Q.ft:{[f;t] k:keys t; $[count k;k xkey f 0!t;f t]};
/ .Q.ff: append y's missing columns to table x as count[x] typed nulls (via column dicts).
.Q.ff:{[x;y] dx:flip x; dy:flip y; nc:key[dy] except key dx; flip dx,nc!{[n;v](type v)$n#0N}[count x]each dy nc};
/ .Q.s1: single-line string repr, a thin wrapper over the `-3!` internal fn.
.Q.s1:{-3!x};
