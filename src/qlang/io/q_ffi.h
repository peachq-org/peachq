/* q_ffi — the .ffi.i.* natives (KX ffikdb-compatible surface over vendored
 * libffi).  Bound by `.pq.load_natives`; `.ffi` wraps the public
 * .ffi.* spellings on top. */
#ifndef QLANG_IO_Q_FFI_H
#define QLANG_IO_Q_FFI_H

void q_ffi_register(void);

#endif
