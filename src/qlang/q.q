/ q.q — self-hosted q keywords (kdb: keywords are `.q` entries from q.k).
/ CONTRACT: one `.q.name:expr` per line; loads after registry init, before
/ dotq.q (which may use these names, never the reverse).  See q_runtime.c.

/ ---- wave 1 (ref/reciprocal.md, ref/next.md) ----
.q.reciprocal:%[1;]
.q.prev:xprev[1;]
.q.next:xprev[-1;]
