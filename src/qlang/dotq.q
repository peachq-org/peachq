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
