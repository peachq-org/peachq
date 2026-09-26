/ This process, copied: h:hopen `:pq:forkq:w1 forks a worker that starts with every global this process has at that
/ moment and answers the alias symbol `:pq:forkq:w1.  h "1+1" is a sync call and h (`f;x) a sync message, exactly as
/ on a kdb handle; the async send is neg of the alias int (the h of its .pq.conns[] row, looked up by a handle not
/ named h: the query's h column shadows a global h); hclose h kills the worker, as does this process's exit.  The
/ worker only computes: its timer is off, it listens on no port, its message handlers are the ones a fresh q starts
/ with, and the connections this process had are not open in it.  It may open its own port (h "system \"p 0W\""),
/ serving IPC and HTTP from the snapshot as of the fork.  If it dies, the next use of h forks a fresh worker from this
/ process as it is then, with no port.  hopen (`:pq:forkq:w1;tmo) bounds the fork's handshake in ms (null, 0 or negative = 5000).
/ POSIX only.  Every .forkq member is a hook the handle machinery calls; none is a user door.
/ .
/ @eg
/ a:42
/ w:hopen `:pq:forkq:w1
/ w "a"
/ w ({x*2};21)
/ neg[first exec h from .pq.conns[] where handle~\:w] "a:0"
/ hclose w

.pq.load_natives`forkq;

/ @ignore
.forkq.i.peer:1b
/ @ignore
.forkq.i.open:{[alias;cfg;tmo;opt] if[count cfg;'domain]; if[not(opt~(::))or 99h=type opt;'`type]; .forkq.i.fork tmo}
/ @ignore
.forkq.i.close:{[c] hclose c}
/ @ignore
.forkq.call:{[c;q;sync] $[sync;c q;neg[c] q]}
