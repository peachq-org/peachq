/* q_dl — a native library a q program asked for by path (`2:`, the .ffi binder), opened NOW and never
 * unmapped: pointers into it (a bound function, a foreign object's destructor) must stay callable for
 * the life of the process.  The optional modules peachq itself loads (duckdb, re2, TLS) keep their own. */
#ifndef QLANG_IO_Q_DL_H
#define QLANG_IO_Q_DL_H

/* NULL when the library will not load; `/` separators are accepted on Windows too */
void* q_dl_open(const char* path);

/* NULL when the library does not export `name` */
void* q_dl_sym(void* lib, const char* name);

#endif /* QLANG_IO_Q_DL_H */
