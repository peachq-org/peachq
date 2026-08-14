/ path.q — THE .path surface: path structure as text, and never storage.  Nothing here opens, stats, expands `~`,
/ resolves a symlink or makes a path absolute.  Portable q — it must run on kx q as well as peachq, so it calls
/ nothing from the peachq C surface.  Names, and the stem/suffix rule, are pathlib's.
/ Three families live in `: symbols and only the FIRST is path-shaped: paths, IPC handles (`:host:port,
/ `:tcps://…) and provider handles (`:pq:…).  The last two are 'domain, because lexical rules corrupt them
/ silently — .path.parent would strip the trailing slash that makes `:pq:ds:al:cfg:t/ a TABLE handle rather than
/ a connection.  host:port is deliberately NOT detected and must never be: `:localhost:5000 is indistinguishable
/ from a folder named "localhost:5000", and a Windows drive puts a colon at position 1, so any host:port
/ heuristic would break every `:c:/temp/x.csv.  ANY-ORDER LAW: definitions only at top level.

/ a LIST of paths, as against one path — a string is 10h and stays one path.
.path.i.many:{[p] type[p] in 0 11h};

/ THE conversion and THE gate: one path-like value to its text, without the leading ":".
.path.i.body:{[p]
    t:$[-11h=type p; string p; -10h=type p; enlist p; 10h=type p; p; '`type];
    t:$[":"~first t; 1_t; t];
    / "pq:" and not a bare "pq": a provider handle always names a provider after the scheme, and `pq alone is
    / an ordinary relative path this gate must not eat.
    if["pq:"~3 sublist t; '`domain];
    w:(t?":") sublist t;
    / 1<count w is what keeps a Windows drive out: "c" is one character, every real scheme is two or more.
    scheme:(1<count w) and (all w in .Q.a,.Q.A,.Q.n,"+-.") and (first w) in .Q.a,.Q.A;
    if[scheme and "//"~2 sublist (1+count w)_t; '`domain];
    t};

/ the "/" components, trailing slashes dropped so `:foo/bar/ names bar.  One component always survives.
.path.i.parts:{[body]
    c:"/" vs body;
    (last 1,1+where 0<count each c) sublist c};

/ pathlib's rule: the last "." splits a name, but only where it is neither the first character (.bashrc is
/ all stem) nor the last (x. has no suffix).  Answers (stem;suffix).
.path.i.split:{[nm]
    i:last -1,where nm=".";
    $[(i>0) and i<-1+count nm; (i sublist nm; i _ nm); (nm;"")]};

/ the canonical path value.  A `:pq:… provider handle or a scheme:// URL is 'domain — see the header.
/ @param p (any) a string, a symbol or a path — or a list of them
.path.path:{[p] $[.path.i.many p; .z.s each p; `$":",.path.i.body p]};

/ the path as a string, WITHOUT the leading ":".
/ @param p (any) a string, a symbol or a path — or a list of them
.path.string:{[p] $[.path.i.many p; .z.s each p; .path.i.body p]};

/ the lexical parent.  A bare name parents to `:. and the root to itself, as pathlib does.  The empty path is
/ RELATIVE, so it parents to `:. too — only a leading "/" in the text tells it apart from the root here.
/ @param p (any) a string, a symbol or a path — or a list of them
.path.parent:{[p]
    if[.path.i.many p; :.z.s each p];
    t:.path.i.body p;
    c:.path.i.parts t;
    if[1=count c; :`$":",$[("/"~first t) and 0=count first c; "/"; "."]];
    c:-1_c;
    `$":",$[(1=count c) and 0=count first c; "/"; "/" sv c]};

/ the final component.
/ @param p (any) a string, a symbol or a path — or a list of them
.path.name:{[p] $[.path.i.many p; .z.s each p; `$last .path.i.parts .path.i.body p]};

/ the final component without its last suffix.
/ @param p (any) a string, a symbol or a path — or a list of them
.path.stem:{[p] $[.path.i.many p; .z.s each p; `$first .path.i.split last .path.i.parts .path.i.body p]};

/ the final suffix, leading "." included as pathlib spells it, or ` where there is none.
/ @param p (any) a string, a symbol or a path — or a list of them
.path.suffix:{[p] $[.path.i.many p; .z.s each p; `$last .path.i.split last .path.i.parts .path.i.body p]};

/ the components joined into ONE path, the first supplying any leading "/" or drive.  The one function here that
/ is NOT elementwise: join is variadic, so its list argument IS the component sequence, never paths to map over.
/ @param parts (list) the components, each a string, a symbol or a path
.path.join:{[parts]
    if[not .path.i.many parts; '`type];
    if[0=count parts; '`length];
    / one component is just conversion — trimming its trailing slash would lose the root `:/ and the drive `:c:/.
    if[1=count parts; :.path.path first parts];
    rs:{$[(0<count x) and "/"~last x; .z.s -1_x; x]};
    b:rs each .path.i.body each parts;
    `$":","/" sv b where (0=til count b) or 0<count each b};
