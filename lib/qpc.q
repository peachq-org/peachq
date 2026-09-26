/ Another q process as a data source: h:hopen `:pq:qpc:alias:host:port connects and answers the alias symbol
/ `:pq:qpc:alias; h "1+1" is a sync call and h (`f;x) a sync message, exactly as on a kdb handle; the async send is neg
/ of the alias int (the h of its .pq.conns[] row).  `:pq:qpc:alias:table/ is that process's table for get, set,
/ upsert and qsql (the query is pushed to the peer), hclose h closes it.  \?handles has examples.
/ Every .qpc member is a hook the handle machinery calls; none is a user door.
/ .
/ @eg
/ p:hopen `:pq:qpc:peer:localhost:5000
/ p "1+1"
/ p (+;1;1)
/ neg[first exec h from .pq.conns[] where handle~\:p] "x::1"
/ select from `:pq:qpc:peer:trade/ where px>2
/ hclose p

/ @ignore
.qpc.i.peer:1b
/ @ignore
.qpc.i.open:{[alias;cfg;tmo;opt] if[not(opt~(::))or 99h=type opt;'`type]; $[null tmo; hopen `$":",cfg; hopen (`$":",cfg;tmo)]}
/ @ignore
.qpc.i.close:{[c] hclose c}
/ @ignore
.qpc.call:{[c;q;sync] $[sync;c q;neg[c] q]}
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
