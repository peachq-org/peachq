/ Another q process as a data source: h:hopen `:pq:qpc:alias:host:port connects and answers the alias symbol
/ `:pq:qpc:alias; h "1+1" is a sync call, h (`async;"x::1") an async send, `:pq:qpc:alias:table/ is that process's
/ table for get, set, upsert and qsql (the query is pushed to the peer), hclose h closes it.  \?handles has examples.
/ Every .qpc member is a hook the handle machinery calls; none is a user door.
/ .
/ @eg
/ h:hopen `:pq:qpc:peer:localhost:5000
/ h "1+1"
/ h (`async;"x::1")
/ select from `:pq:qpc:peer:trade/ where px>2
/ hclose h

/ @ignore
.qpc.i.open:{[alias;cfg;tmo;opt] $[null tmo; hopen `$":",cfg; hopen (`$":",cfg;tmo)]}
/ @ignore
.qpc.i.close:{[c] hclose c}
/ @ignore
.qpc.call:{[c;q;sync] $[sync;c q;neg[c] q]}
/ @ignore
.qpc.async:{[c;msg] .qpc.call[c;msg;0b]}
/ @ignore
.qpc.bind:{[c;t] c "cols ",string t}
/ @ignore
.qpc.get:{[c;t] c string t}
/ @ignore
.qpc.set:{[c;t;d] c (`set;t;d); t}
/ @ignore
.qpc.upsert:{[c;t;d] c (`upsert;t;d); t}
/ @ignore
.qpc.i.hdel:{[c;t] c (!;`.;();0b;enlist t); t}
/ @ignore
.qpc.qsql:{[c;cl;tree] rt:.pq.i.resolveTree[cl;tree]; r:c (?),rt; .qpc.i.lastq::rt; r}
