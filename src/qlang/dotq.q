/ dotq.q — openq's embedded q bootstrap (the ".Q" utility namespace and the
/ q-faithful stdlib), authored from the PUBLISHED qdocs (qdocs/, CC BY 4.0).
/
/ BIGDECISION (ARCHITECTURE.md): this is a REAL .q file, embedded in the binary
/ via build-time codegen (tools/gen-bootstrap.sh -> src/qlang/dotq_gen.h) and
/ loaded post-registry at the tail of q_runtime_create — mirroring how kx runs
/ q.k at kdb+ startup. It is directly runnable: `./q src/qlang/dotq.q`.
/
/ Loader is line-at-a-time: each definition MUST be ONE line. Blank lines and
/ `/` full-line comments are skipped.
/
/ Wave 0 seeds exactly ONE entry to prove the pipeline end-to-end. The rest of
/ .Q (constants, one-liner utils, DB/partition family) lands in later waves.

/ .Q.b6 — bicameral-alphanums (ref/dotq.md): the base64 alphabet.
.Q.b6:"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

/ ---- Wave-A constants (ref/dotq.md) ----
/ .Q.A / .Q.a — upper / lower Roman alphabet.
.Q.A:"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
.Q.a:"abcdefghijklmnopqrstuvwxyz"
/ .Q.an — all alphanumerics (docs order: lower, upper, underscore, digits).
.Q.an:"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789"
/ .Q.n / .Q.nA — numerics / base-36 alphabet (.Q.nA drives j12/x12; define before them).
.Q.n:"0123456789"
.Q.nA:"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
/ .Q.K / .Q.k — version date + number of the q.k spec openq targets. openq's OWN
/ release stamp is .z.K/.z.k; these are the (static) "which q spec we implement" values.
.Q.K:2020.10.02
.Q.k:4f

/ ---- Wave-A encoders (ref/dotq.md): pure-q against .Q.b6 / .Q.nA ----
/ .Q.x10 / .Q.j10 — binhex (base-64) encode / decode vs .Q.b6; fixed width 10.
.Q.x10:{raze {1#x _ .Q.b6} each (10#64) vs x}
.Q.j10:{64 sv {[c] first where {x~1#y _ .Q.b6}[c;] each til count .Q.b6} each {[s;i]1#i _ s}[x;] each til count x}
/ .Q.x12 / .Q.j12 — base-36 encode / decode vs .Q.nA; fixed width 12.
.Q.x12:{raze {1#x _ .Q.nA} each (12#36) vs x}
.Q.j12:{36 sv {[c] first where {x~1#y _ .Q.nA}[c;] each til count .Q.nA} each {[s;i]1#i _ s}[x;] each til count x}

/ ---- Wave-D command-line / environment (ref/dotq.md) ----
/ .Q.x — non-command parameters; initialised empty, (re)set by .Q.opt on each call.
.Q.x:()
/ .Q.opt — command-line args -> dict (ref/dotq.md, "opt"). Command params start "-";
/ each flag's value is the tokens up to the next flag (0 -> (), 1 -> the string, many ->
/ a list). Empty argv is guarded (where errors on empty; inner lambda runs only when
/ argv is non-empty). Also sets .Q.x to the leading non-command tokens ("Set by .Q.opt").
.Q.opt:{[a] a:$[-10h=type a;enlist a;a]; .Q.x:(); $[count a;{[a] i:where {"-"~1#x} each a; e:(1_i),count a; .Q.x:$[count i;(first i)#a;a]; k:`$1_'a i; j:til count i; k!{[a;i;e;j] s:(1+i j)_(e j)#a; $[1=count s;first s;s]}[a;i;e;]each j}[a];(`$())!()]}
/ .Q.def — defaults + tok-typed coercion over .Q.opt output (ref/dotq.md, "def"). For each
/ default key: if present in opt, coerce its string to the default atom's type (typed null
/ on failure); else keep the default. DEPENDS ON .Q.opt above (loader is top-to-bottom).
.Q.def:{[d;o] key[d]!{[d;o;k] $[k in key o;$[-10h=type o k;(type d k)$o k;first 0#d k];d k]}[d;o;]each key d}

/ ---- Wave-B general-purpose utils (ref/dotq.md) ----
/ .Q.dd — join symbols (ref/dotq.md, "dd"): shorthand for ` sv x,`$string y. Builds
/ dotted names / filepaths / suffixed syms. `:handle joins with "/", plain sym with ".".
.Q.dd:{` sv x,`$string y}
/ .Q.addmonths — x (date) + y (int) months (ref/dotq.md, "addmonths"). Cast to month, add
/ y, take first-of-month back to a date, then re-add x's 0-based day offset; a day offset
/ past the shorter target month spills forward naturally (2006.10.29+4 -> 2007.03.01).
.Q.addmonths:{[x;y] (`date$(`month$x)+y)+x-`date$`month$x}
/ .Q.fu — apply-unique (ref/dotq.md, "fu"): eval unary f on the DISTINCT items of list y,
/ then reindex to full length (u?y). Atom y short-circuits to f y. Inner lambda takes u
/ explicitly (openq lambdas do not close over outer locals).
.Q.fu:{[f;y] $[0>type y;f y;{[f;y;u]f[u]u?y}[f;y]distinct y]}
/ .Q.ft — apply-simple to a keyed table (ref/dotq.md, "ft"): unkey (0!), apply f to the
/ simple table, re-key on the original key cols (k xkey). Non-keyed t just applies f.
.Q.ft:{[f;t] k:keys t; $[count k;k xkey f 0!t;f t]}
/ .Q.ff — append null-filled columns (ref/dotq.md, "ff"): add cols of table y absent from
/ table x, each a count[x] null of that col's type; common cols keep x's values. Works via
/ column dicts (flip) so multi-row y is handled (index the col-dict dy by the missing keys,
/ NOT the table by nc). Contract: y is a TABLE (uses flip y / key); not a general dict util.
.Q.ff:{[x;y] dx:flip x; dy:flip y; nc:key[dy] except key dx; flip dx,nc!{[n;v](type v)$n#0N}[count x]each dy nc}
/ .Q.s1 — single-line string representation (ref/dotq.md, "s1"): the -3! internal. -3! is
/ currently nyi in openq, so this is red-below-floor until it lands (then auto-greens).
.Q.s1:{-3!x}
/ .Q.s — console plain-text of x (ref/dotq.md, "s"): multi-line, console-size aware. The correct
/ backend is a console-format-to-string primitive that openq does not yet expose (-3! is the
/ single-line form, not this). Bound as an honest -3! placeholder; stays red until that lands.
.Q.s:{-3!x}
