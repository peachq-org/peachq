/ Another q process as a data source: h:hopen `:pq:qpc:alias:host:port connects and answers the alias symbol
/ `:pq:qpc:alias, which names it from then on - h "1+1" is a sync call, h (`async;"x::1") an async send,
/ `:pq:qpc:alias:table/ is that process's table for get, set, upsert and qsql, with the query pushed to the peer;
/ hclose h closes it.  \?handles has examples.
/ @implNote The `qpc` virtual-table provider: plain q IPC as a data source
/ (`:pq:qpc:alias:host:port`, contract v3: the 2026-09-15 ADR § Handles).
/ Standard-library tier (lib/*.q): loaded by the `\l pq` gate, NOT on the
/ always-on bootstrap - the engine reserves `.ipc.*` (restricted natives +
/ `.ipc.on.*` event hooks) and real q has no such namespace, so out of the box
/ neither exists.  ANY-ORDER LAW: definitions only at top level.  The token
/ is the plain int handle the inner hopen returned; qsql pushes the resolved
/ functional tree to the remote (below).

/ the host-called lifecycle hooks: alias unused (a peer needs no name), opt ignored (no open-time options yet)
.qpc.i.open:{[alias;cfg;tmo;opt] $[null tmo; hopen `$":",cfg; hopen (`$":",cfg;tmo)]}
.qpc.i.close:{[c] hclose c}
.qpc.call:{[c;q;sync] $[sync;c q;neg[c] q]}
.qpc.async:{[c;msg] .qpc.call[c;msg;0b]}
.qpc.bind:{[c;t] c "cols ",string t}
.qpc.get:{[c;t] c string t}
/ writes ride the sym-head value form (`set - the set VALUE itself does not
/ cross the wire); sync sends so the remote's error propagates
.qpc.set:{[c;t;d] c (`set;t;d); t}
.qpc.upsert:{[c;t;d] c (`upsert;t;d); t}
/ hdel `:pq:qpc:al:t/ drops the REMOTE table: the functional delete, so a parse-tree-only .z.pg accepts it
.qpc.i.hdel:{[c;t] c (!;`.;();0b;enlist t); t}
/ qsql pushdown: resolve free vars via the shared walk (columns win; enclosing
/ locals are INVISIBLE to a pushed query - the value-of-string law), send the
/ functional message (?;name;...) - the remote q evaluates it natively.  qpc
/ NEVER materializes a query: an unpushable tree errors ('unpush from the
/ resolver; wire/remote errors verbatim) - .qpc.get is an explicit primitive,
/ never an implicit fallback.  .qpc.i.lastq records the last pushed tree.
.qpc.qsql:{[c;cl;tree] rt:.pq.i.resolveTree[cl;tree]; r:c (?),rt; .qpc.i.lastq::rt; r}
