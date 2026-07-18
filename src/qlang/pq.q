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
