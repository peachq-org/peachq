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
