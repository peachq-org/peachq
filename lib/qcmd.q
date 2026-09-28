/ @ignore
/ Score .qcmd console transcripts: each file is replayed in a forked worker exactly as `q f.qcmd` replays it, the
/ replay lands under an out directory at the file's own relative path, and each replayed row is scored against the
/ transcript's by the row rule of tools/qcmd/run.sh.  The worker copies this session, so its flags set the replay's.
/ .qcmd.e is never defined: it is the whole-file load hook every .qcmd load looks up.
/ @eg exit .qcmd.run `test/q/ipc
/ @eg .qcmd.check[`tx.qcmd;`out`n!(`:replays;4)]

.qcmd.i.EMPTY:([] file:`symbol$(); line:`long$(); prompt:(); want:(); got:(); why:());

/ Absolute against the working directory and lexically normal; a leading "/" or a drive ("c:/") is already absolute.
/ Symlinks are not resolved.
.qcmd.i.abspath:{[path]
  path:.path.string path;
  drive:{[path] (":/"~path 1 2) and (first path) in .Q.a,.Q.A};
  if[not ("/"=first path) or drive path; path:(system "cd"),"/",path];
  root:$[drive path; 2#path; ""];
  parts:"/" vs (count root)_path;
  parts:parts where not (0=count each parts) or parts~\:1#".";
  root,"/","/" sv {[kept;part] $[part~".."; -1_kept; kept,enlist part]}/[();parts]};

.qcmd.i.opts:{[opts]
  opts:(`out`n!(::;10)),$[99h=type opts; opts; ()!()];
  if[not (type opts`n) in -5 -6 -7h; '`type];
  if[1>opts`n; '`domain];
  opts};

/ Replay and score every transcript under a path.
/ @param path (any) a .qcmd file, or a directory whose *.qcmd files at every depth are scored (*.win.qcmd excluded)
/ @param opts (dict) `out the replay directory (by default a temporary one, deleted after scoring), `n the forks run
/ at once (10)
/ @return (table) file line prompt want got why, one row per transcript row, line its prompt's 1-based line in the
/ file; why is "" for a pass, else output, prompt, no error, error, worker died or missing row
.qcmd.check:{[path;opts]
  / A source inside out is skipped: its replay target would be the source itself, deleted before the replay appends.
  plan:{[path;out]
    join:{[dir;rel] .path.string .path.join (dir;rel)};
    rel:.fs.walk[path;"*.qcmd"];
    rel:rel where not rel like "*.win.qcmd";
    src:$[.fs.isdir path; join[path] each rel; (count rel)#enlist .path.string path];
    absolute:.qcmd.i.abspath each src;
    keep:where not .str.startswith[absolute; out,"/"];
    ([] file:`$src keep; src:absolute keep; out:join[out] each rel keep; k:til count keep)};
  batch:{[jobs]
    / Answers (worker;pid).  The worker appends, so a stale replay goes first; it replays in a directory of its own.
    / No local may share a .pq.conns[] column name: `where spec=alias` would compare two columns.
    start:{[job]
      target:.path.path job`out;
      @[hdel;target;::];
      worker:@[.pq.hopen_qfork[`$"qcmdrun",string job`k];`stdout`stderr!2#target;{[err] `}];
      if[null worker; :(worker;0Ni)];
      conn:exec handle:first handle, pid:first pid from .pq.conns[] where spec=worker;
      replay:{[dir;file] system "cd ",dir; system "l ",file};
      @[neg conn`handle;(replay;job`dir;job`src);::];
      (worker;conn`pid)};
    / A sync call on a dead qfork alias forks a fresh worker, so a changed .z.i is a death too.
    finish:{[started]
      worker:first started;
      if[null worker; :1b];
      alive:@[{[worker;pid] pid~worker ".z.i"}[worker];last started;{[err] 0b}];
      @[hclose;worker;::];
      not alive};
    score:{[file;src;out;died]
      lines:{[file] @[read0;.path.path file;()] except\: "\r"};
      rows:{[lines;prose]
        at:where 0<.pq.i.qcmd_prompt each lines;
        blocks:1_'at _ lines;
        if[prose; blocks:{x where not x like "/[ \t]*"} each blocks];
        (lines at; {.str.strip "\n" sv x} each blocks; 1+at)};
      verdict:{[want_prompt;want;got_prompt;got]
        if[not want_prompt~got_prompt; :($[count got; got_prompt,"\n",got; got_prompt]; "prompt")];
        if[not "'"=first want; :(got; $[want~got; ""; "output"])];
        errs:{x where x like "'*"} "\n" vs got;
        if[0=count errs; :(got; "no error")];
        class:.str.rstrip first "\n" vs want;
        (first errs; $[any class~/:(1#"'";"'error"); ""; class~first errs; ""; "error"])};
      want:rows[lines src;1b];
      got:rows[lines out;0b];
      n:count want 0;
      if[0=n; :.qcmd.i.EMPTY];
      lost:(count got 0)_til n;
      res:verdict'[want 0;want 1;got[0] til n;got[1] til n];
      res:@[res;lost;:;(count lost)#enlist (""; $[died; "worker died"; "missing row"])];
      ([] file:n#file; line:want 2; prompt:want 0; want:want 1; got:res[;0]; why:res[;1])};
    jobs[`dir]:{.fs.mkdtemp[]} each jobs`k;
    started:start each jobs;
    died:finish each started;
    {@[.fs.rmtree;x;::]} each jobs`dir;
    raze score'[jobs`file;jobs`src;jobs`out;died]};
  opts:.qcmd.i.opts opts;
  own:(::)~opts`out;
  out:.qcmd.i.abspath $[own; .fs.mkdtemp[]; opts`out];
  score_all:{[plan;batch;path;out;n] jobs:plan[path;out]; .qcmd.i.EMPTY,$[count jobs; raze batch each n cut jobs; ()]};
  results:.[score_all;(plan;batch;path;out;opts`n);::];
  if[own; @[.fs.rmtree;out;::]];
  if[10h=type results; 'results];
  results};

/ Replay and score like .qcmd.check, print each failing row and a pass count per file, and answer the exit code:
/ 0 when every row passes, 1 when any fails or there were none.
/ @param input (any) a path, or (path;opts) as .qcmd.check takes them
/ @eg exit .qcmd.run `test/q/ipc
.qcmd.run:{[input]
  show_row:{[row]
    pad:ssr[;"\n";"\n        "];
    -1 string[row`file],":",string[row`line],": ",row`why;
    -1 "  ",row`prompt;
    -1 "  want: ",pad row`want;
    -1 "  got:  ",pad row`got;
    };
  args:$[0h=type input; input; (input;::)];
  opts:.qcmd.i.opts last args;
  results:.qcmd.check[first args;opts];
  ok:0=count each results`why;
  show_row each results where not ok;
  files:group results`file;
  {[file;ok] -1 string[file],": ",string[sum ok],"/",string count ok}'[key files;ok value files];
  if[not (::)~opts`out; -1 "replays: ",.qcmd.i.abspath opts`out];
  $[(0=count results) or not all ok; 1; 0]};
