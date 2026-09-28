/ The filesystem: .fs.exists, .fs.isfile, .fs.isdir, .fs.size answer for a path, a symbol or a list of them;
/ .fs.remove deletes the files named and .fs.rmdir the empty directories named - never anything below them;
/ .fs.rmtree deletes a directory tree.  .fs.walk lists a tree's files, .fs.gettempdir names the temporary directory
/ and .fs.mkdtemp makes a new one in it.
/ @implNote Every path argument converts through .path.path, so .fs takes a string, a symbol or a path and inherits
/ .path's refusal of `:pq: provider handles and scheme:// URLs.  The names are Python's os/shutil/tempfile spellings.
/ .fs.remove and .fs.rmdir are one hdel per element, and hdel's refusal of a populated directory is their whole guard;
/ .fs.rmtree is the ONE recursive delete, and native, because a q walk over `key` follows links.

.pq.load_natives`fs;

/ THE SHAPE LAW, and the one place the three kinds are told apart: `key` answers a symbol ATOM for a file, a symbol
/ VECTOR for a directory and a GENERAL empty for a missing path.  So an EMPTY directory is `symbol$() - 11h, a
/ directory still - and only a missing path is 0h.  Reading a COUNT here would call an empty directory missing.
.fs.i.kind:{[path] type key path};

/ THE conversion and THE vectorisation in one place: .path.path answers a symbol ATOM for one path and a symbol
/ VECTOR for a list of them, so the shape of its result IS the elementwise decision.
.fs.i.map:{[f;p]
    c:.path.path p;
    $[-11h=type c; f c; f each c]};

/ THE shape gate: a verb names the kinds it owns and anything else is 'domain, so remove can never take a directory
/ and rmdir can never take a file.  Accepting 0h - the missing path - leaves the answer for it to hdel.
.fs.i.accept:{[kinds;path] $[.fs.i.kind[path] in kinds; path; '`domain]};

/ whether the path names anything at all, file or directory.
/ @param p (any) a path, a string or a symbol - or a list of them
.fs.exists:{[p] .fs.i.map[{0h<>.fs.i.kind x};p]};

/ whether the path names a regular file.
/ @param p (any) a path, a string or a symbol - or a list of them
.fs.isfile:{[p] .fs.i.map[{-11h=.fs.i.kind x};p]};

/ whether the path names a directory - an EMPTY one still is one.
/ @param p (any) a path, a string or a symbol - or a list of them
.fs.isdir:{[p] .fs.i.map[{11h=.fs.i.kind x};p]};

/ the file's size in bytes.  A directory is NOT refused and does NOT answer the size of its contents - hcount hands
/ back the OS's own directory-entry size, exactly as Python's os.path.getsize does.  A missing path is 'io.
/ @param p (any) a path, a string or a symbol - or a list of them
.fs.size:{[p] .fs.i.map[hcount;p]};

/ delete ONE file.  A directory is 'domain whether it is empty or not: remove is the file verb, and letting hdel
/ answer would quietly delete an empty directory.  A missing path is hdel's own 'io.
/ @param p (any) a path, a string or a symbol - or a list of them
.fs.remove:{[p] .fs.i.map[{hdel .fs.i.accept[0 -11h;x]};p]};

/ delete ONE EMPTY directory.  A file is 'domain: rmdir is the directory verb.  A POPULATED directory is hdel's own
/ 'io and survives intact - the engine's refusal to recurse is the guard, and it is passed through untouched.
/ @param p (any) a path, a string or a symbol - or a list of them
.fs.rmdir:{[p] .fs.i.map[{hdel .fs.i.accept[0 11h;x]};p]};

/ delete a directory and everything in it, answering its path; a link inside it, or named, is removed and never
/ followed.  A file, a root, the working directory or any directory above it is 'domain; a missing path is 'io.
/ @param p (any) a path, a string or a symbol - or a list of them
.fs.rmtree:{[p] .fs.i.map[.fs.i.rmtree;p]};

/ the files at any depth under a directory whose names match a `like` pattern, as strings relative to it, in `key`
/ order.  A file answers its own name when it matches; an empty directory or a missing path answers ().
/ @param p (any) a path, a string or a symbol
/ @param pattern (string) a `like` pattern, matched against each file's name
.fs.walk:{[p;pattern]
    p:.path.path p;
    name:string .path.name p;
    if[.fs.isfile p; :$[name like pattern; enlist name; ()]];
    raze {[p;pattern;kid]
        sub:` sv p,kid;
        found:.fs.walk[sub;pattern];
        $[.fs.isdir sub; (string[kid],"/"),/:found; found]}[p;pattern] each key p};

/ the directory temporary files belong in: the first of the TMPDIR, TEMP and TMP environment variables that is set and
/ not empty, else "/tmp".
.fs.gettempdir:{[]
    dirs:getenv each `TMPDIR`TEMP`TMP;
    first (dirs where 0<count each dirs),enlist "/tmp"};

/ a new, empty directory under .fs.gettempdir[], its path as a string.
.fs.mkdtemp:{[]
    draw:{[] .fs.gettempdir[],"/tmp",string[.z.i],"_",string["j"$.z.p],"_",string first 1?1000000};
    dir:draw[];
    while[.fs.exists dir; dir:draw[]];
    hdel (` sv (.path.path dir),`.placeholder) set 0;
    dir};
