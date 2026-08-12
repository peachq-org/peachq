/ .ffi — calling C from q: the KX ffikdb surface verbatim over a vendored libffi (src/qlang/io/q_ffi.c), so published
/ ffikdb examples run unchanged.  Loaded by the `\l pq` gate.  Full doc: user-docs/ffi.md.
/ Lambdas, not natives, so the surface is DISCOVERABLE: a bound function prints the word `arglist`, which is the answer
/ to the question that sends people here.
/ Provenance: adapted from KX ffikdb (Apache-2.0, per the ksql precedent; license at docs/licenses/kx-ffi-LICENSE).
/ ANY-ORDER LAW: definitions only.

/ resolve a C function ONCE and hand back a callable.
/ The returned function is UNARY: it takes ONE list of arguments, and the trailing (::) is what stops q collapsing
/ same-type arguments into a vector — (2f;10f) is ALREADY a float vector, so pow (2f;10f) is 'rank while
/ pow (2f;10f;::) is 1024f.  KX's convention, not ours.
/ @param funcname (symbol|symbol list) `fn resolvable in this process, or `lib`fn to dlopen a library
/ @param argtypes (string|char) one letter per argument, "" for none; uppercase is a pointer, "k" a callback
/ @param returntype (char) one letter, " " for void — a char ATOM, so enlist "f" is 'type
/ @throws os funcname did not resolve
.ffi.bind:{[funcname;argtypes;returntype]
    {[binding;arglist] .ffi.i.call[binding;arglist]} .ffi.i.bind[funcname;argtypes;returntype]}

/ call a C function, resolving it EVERY time — the one-shot twin of bind, with argument types inferred from the values.
/ @param returnfunc (symbol|symbol list|list) `fn, `lib`fn, or (returnletter;fn) — the letter defaults to "i"
/ @param arglist (any) the arguments, ending in (::)
.ffi.callFunction:{[returnfunc;arglist] .ffi.i.callfn[returnfunc;arglist]}

/ read a C global variable.
/ @param returnvar (symbol|symbol list|list) the spelling callFunction takes, naming a variable
.ffi.cvar:{[returnvar] .ffi.i.cvar returnvar}

/ set errno, and answer what it was BEFORE the set.  Anything but an int atom reads without setting.
/ @param errnum (int) the new errno; omit to read without setting
/ @return (int) the PREVIOUS errno
.ffi.setErrno:{[errnum] .ffi.i.errno errnum}

/ this platform's shared-library extension, as a SYMBOL.
/ @return (symbol) `so, `dll or `dylib — a symbol, so ` sv `qr,.ffi.extension[] is `qr.so
.ffi.extension:{[] ("wlm"!`dll`so`dylib) first string .z.o}

/ the width of a pointer on this platform, in bytes.
/ @return (int) 8i, or 4i on a 32-bit host
.ffi.ptrsize:{$[.z.o like "?32"; 4i; 8i]}

/ this platform's OS letter, as a CHAR — KX's `{[] first string .z.o}` verbatim, and what published examples branch on
/ with "l"=.ffi.os[].  ` sv `qr,.ffi.os[] is therefore 'type where the extension spelling works; use `$"qr",.ffi.os[].
/ @return (char) "l", "w" or "m"
.ffi.os:{[] first string .z.o}
