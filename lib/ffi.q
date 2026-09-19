/ Call C from q: .ffi.bind turns a symbol in any shared library into a q function, .ffi.callFunction is the
/ one-shot form.  The surface and type letters are KX ffikdb's, so its published examples run unchanged.
/ Adapted from KX ffikdb (Apache-2.0; licence at docs/licenses/kx-ffi-LICENSE).
/ .
/ @eg
/ pow:.ffi.bind[`libm.so.6`pow;"ff";"f"]
/ pow (2f;10f;::)

/ Resolve a C function once and answer a q function that calls it.  The result is unary: it takes ONE list of
/ arguments ending in (::) - the trailing :: stops q collapsing same-type arguments into a vector.
/ @param funcname `fn resolvable in this process, or `lib`fn to load a library
/ @param argtypes one type letter per argument (i j f C ... uppercase for a pointer, "k" a callback), "" for none
/ @param returntype one type letter as a char atom, " " for void
/ @throws os funcname did not resolve
/ @eg .ffi.bind[`libm.so.6`sqrt;"f";"f"] (16f;::)
.ffi.bind:{[funcname;argtypes;returntype]
    {[binding;arglist] .ffi.i.call[binding;arglist]} .ffi.i.bind[funcname;argtypes;returntype]}

/ Call a C function, resolving it every time, with argument types inferred from the values.
/ @param returnfunc `fn, `lib`fn, or (returnletter;fn) - the letter defaults to "i"
/ @param arglist the arguments, ending in (::)
/ @eg .ffi.callFunction[("f";`libm.so.6`sqrt)] (16f;::)
.ffi.callFunction:{[returnfunc;arglist] .ffi.i.callfn[returnfunc;arglist]}

/ Read a C global variable.
/ @param returnvar `var, `lib`var, or (typeletter;var)
.ffi.cvar:{[returnvar] .ffi.i.cvar returnvar}

/ Set errno and answer its previous value; anything but an int atom reads without setting.
/ @return the previous errno
/ @eg .ffi.setErrno[]
.ffi.setErrno:{[errnum] .ffi.i.errno errnum}

/ This platform's shared-library extension.
/ @return `so, `dll or `dylib
/ @eg ` sv `libmylib,.ffi.extension[]
.ffi.extension:{[] ("wlm"!`dll`so`dylib) first string .z.o}

/ The width of a pointer on this platform, in bytes.
/ @return 8i, or 4i on a 32-bit host
.ffi.ptrsize:{[] $[.z.o like "?32"; 4i; 8i]}

/ This platform's OS letter, as ffikdb examples branch on it.
/ @return "l", "w" or "m"
/ @eg "l"=.ffi.os[]
.ffi.os:{[] first string .z.o}
