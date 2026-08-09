/ qpc.q — the `qpc` virtual-table provider: plain q IPC as a data source
/ (`:pq:qpc:alias:host:port`, actionable-plans/2026-08-07-plugin-data-
/ sources-tables.md).  Standard-library tier (lib/*.q): loaded by the `\l pq`
/ gate, NOT on the always-on bootstrap — the engine reserves `.ipc.*`
/ (restricted natives + `.ipc.on.*` event hooks) and real q has no such
/ namespace, so out of the box neither exists.  ANY-ORDER LAW: definitions
/ only at top level.  CONNID is the plain int handle hopen returned; qsql
/ pushes the resolved functional tree to the remote (below).
/ opt (the config dict) is ignored: plain q IPC has no open-time options yet
.qpc.open:{[cfg;tmo;opt] $[null tmo; hopen `$":",cfg; hopen (`$":",cfg;tmo)]}
.qpc.close:{[c] hclose c}
.qpc.call:{[c;q;sync] $[sync;c q;neg[c] q]}
.qpc.bind:{[c;t] c "cols ",string t}
.qpc.get:{[c;t] c string t}
/ writes ride the sym-head value form (`set — the set VALUE itself does not
/ cross the wire); sync sends so the remote's error propagates
.qpc.set:{[c;t;d] c (`set;t;d); t}
.qpc.upsert:{[c;t;d] c (`upsert;t;d); t}
/ qsql pushdown: resolve free vars via the shared walk (columns win; enclosing
/ locals are INVISIBLE to a pushed query — the value-of-string law), send the
/ functional message (?;name;...) — the remote q evaluates it natively.  qpc
/ NEVER materializes a query: an unpushable tree errors ('unpush from the
/ resolver; wire/remote errors verbatim) — .qpc.get is an explicit primitive,
/ never an implicit fallback.  .qpc.i.lastq records the last pushed tree.
.qpc.qsql:{[c;cl;tree] rt:.pq.i.resolveTree[cl;tree]; r:c (?),rt; .qpc.i.lastq::rt; r}
