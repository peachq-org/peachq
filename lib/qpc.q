/ qpc.q — the `qpc` virtual-table provider: plain q IPC as a data source
/ (`:pq:qpc:alias:host:port`, actionable-plans/2026-08-07-plugin-data-
/ sources-tables.md).  Standard-library tier (lib/*.q): loaded by the `\l pq`
/ gate, NOT on the always-on bootstrap — the engine reserves `.ipc.*`
/ (restricted natives + `.ipc.on.*` event hooks) and real q has no such
/ namespace, so out of the box neither exists.  ANY-ORDER LAW: definitions
/ only at top level.  CONNID is the plain int handle hopen returned; qsql
/ rides the host fallback (phase-1 residual).
/ opt (the config dict) is ignored: plain q IPC has no open-time options yet
.qpc.open:{[cfg;tmo;opt] $[null tmo; hopen `$":",cfg; hopen (`$":",cfg;tmo)]}
.qpc.close:{[c] hclose c}
.qpc.call:{[c;q;sync] $[sync;c q;neg[c] q]}
.qpc.bind:{[c;t] c "cols ",string t}
.qpc.get:{[c;t] c string t}
