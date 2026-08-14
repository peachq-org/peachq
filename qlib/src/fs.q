/ fs.q — THE .fs surface: the read-safe view of the filesystem, plus deletion bounded to the objects NAMED — one hdel
/ per element, so a vector argument deletes exactly its own elements and never a level below any of them.  Every
/ path argument converts through .path.path, so .fs takes a string, a symbol or a path and inherits .path's refusal of
/ `:pq: provider handles and scheme:// URLs.  Portable q — key, hcount and hdel are all standard kdb, so nothing here
/ calls the peachq C surface; the names are Python's os/os.path spellings.  NOTHING HERE RECURSES: hdel refuses a
/ populated directory itself, and that refusal is the whole guard — inherited, never re-worded.  Definitions only.

/ THE SHAPE LAW, and the one place the three kinds are told apart: `key` answers a symbol ATOM for a file, a symbol
/ VECTOR for a directory and a GENERAL empty for a missing path.  So an EMPTY directory is `symbol$() — 11h, a
/ directory still — and only a missing path is 0h.  Reading a COUNT here would call an empty directory missing.
.fs.i.kind:{[path] type key path};

/ THE conversion and THE vectorisation in one place: .path.path answers a symbol ATOM for one path and a symbol
/ VECTOR for a list of them, so the shape of its result IS the elementwise decision.
.fs.i.map:{[f;p]
    c:.path.path p;
    $[-11h=type c; f c; f each c]};

/ THE shape gate: a verb names the kinds it owns and anything else is 'domain, so remove can never take a directory
/ and rmdir can never take a file.  Accepting 0h — the missing path — leaves the answer for it to hdel.
.fs.i.accept:{[kinds;path] $[.fs.i.kind[path] in kinds; path; '`domain]};

/ whether the path names anything at all, file or directory.
/ @param p (any) a path, a string or a symbol — or a list of them
.fs.exists:{[p] .fs.i.map[{0h<>.fs.i.kind x};p]};

/ whether the path names a regular file.
/ @param p (any) a path, a string or a symbol — or a list of them
.fs.isfile:{[p] .fs.i.map[{-11h=.fs.i.kind x};p]};

/ whether the path names a directory — an EMPTY one still is one.
/ @param p (any) a path, a string or a symbol — or a list of them
.fs.isdir:{[p] .fs.i.map[{11h=.fs.i.kind x};p]};

/ the file's size in bytes.  A directory is NOT refused and does NOT answer the size of its contents — hcount hands
/ back the OS's own directory-entry size, exactly as Python's os.path.getsize does.  A missing path is 'io.
/ @param p (any) a path, a string or a symbol — or a list of them
.fs.size:{[p] .fs.i.map[hcount;p]};

/ delete ONE file.  A directory is 'domain whether it is empty or not: remove is the file verb, and letting hdel
/ answer would quietly delete an empty directory.  A missing path is hdel's own 'io.
/ @param p (any) a path, a string or a symbol — or a list of them
.fs.remove:{[p] .fs.i.map[{hdel .fs.i.accept[0 -11h;x]};p]};

/ delete ONE EMPTY directory.  A file is 'domain: rmdir is the directory verb.  A POPULATED directory is hdel's own
/ 'io and survives intact — the engine's refusal to recurse is the guard, and it is passed through untouched.
/ @param p (any) a path, a string or a symbol — or a list of them
.fs.rmdir:{[p] .fs.i.map[{hdel .fs.i.accept[0 11h;x]};p]};
