/ A fresh process: h:hopen (`:pq:procq:w1;tmo;("init.q";"-s";"2")) launches this peachq with exactly that argv (no
/ shell, no splitting) and answers the alias symbol `:pq:procq:w1 once it is serving.  The argv is a list of strings,
/ one per argument (enlist "init.q" is one); "" and :: mean no arguments.  The
/ worker is an ordinary q: its script, options and .z.p* handlers apply, and .z.x is the argv less q's own options and
/ the script, as for any q command line.  h "1+1" is a sync call and h (`f;x) a sync message, exactly as on a kdb
/ handle; the async send is neg of the alias int (the h of its .pq.conns[] row, looked up by a handle not named h: the
/ query's h column shadows a global h).  hclose h kills it, as does this process's exit.  If it dies, the next use of
/ h launches it again from the same argv: its recipe is the argv, so anything pushed into it later is gone.  tmo
/ bounds the launch in ms (null, 0 or negative = 10000); a worker that exits or never answers within it is 'proc.
/ hopen answering means the process started and answered, not that its script succeeded: a script error stops the
/ script, not the process.  Every .procq member is a hook the handle machinery calls; none is a user door.
/ .
/ @eg
/ w:hopen (`:pq:procq:w1;5000;("-s";"2"))
/ w ".z.x"
/ w ({x*2};21)
/ neg[first exec h from .pq.conns[] where handle~\:w] "a:0"
/ hclose w

.pq.load_natives`procq;

/ @ignore
.procq.i.peer:1b
/ @ignore
.procq.i.open:{[alias;cfg;tmo;opt]
  if[count cfg;'domain];
  if[(opt~(::))or opt~"";opt:()];
  if[not(0h=type opt)&all 10h=abs type each opt;'`type];
  .procq.i.spawn[@[opt;where -10h=type each opt;enlist];tmo]}
/ @ignore
.procq.i.close:{[c] hclose c}
/ @ignore
.procq.call:{[c;q;sync] $[sync;c q;neg[c] q]}
