/ ffi.q — the .ffi namespace: KX ffikdb-compatible FFI surface, verbatim
/ spellings (.ffi.bind / .ffi.callFunction / .ffi.cvar / .ffi.setErrno /
/ .ffi.extension / .ffi.ptrsize / .ffi.os) so published ffikdb examples run
/ unchanged.  Standard-library tier: loaded by the `\l pq` gate over the
/ .ffi.i.* natives (src/qlang/io/q_ffi.c, vendored libffi engine).
/ Provenance: surface adapted from KX ffikdb (Apache-2.0, adopted per the
/ ksql precedent; license tracked at docs/licenses/kx-ffi-LICENSE).
/ ANY-ORDER LAW: definitions only at top level.
.ffi.bind:{[f;a;r] {[h;args] .ffi.i.call[h;args]} .ffi.i.bind[f;a;r]}
.ffi.callFunction:{[rtf;args] .ffi.i.callfn[rtf;args]}
.ffi.cvar:{[rtv] .ffi.i.cvar rtv}
.ffi.setErrno:{[n] .ffi.i.errno n}
.ffi.extension:{[] ("wlm"!`dll`so`dylib) first string .z.o}
.ffi.ptrsize:{$[.z.o like "?32"; 4i; 8i]}
.ffi.os:{[] first string .z.o}
