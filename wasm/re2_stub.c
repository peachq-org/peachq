/* re2_stub — inert RE2 shim symbols for the WebAssembly build.
 *
 * The browser build is one emcc invocation over a C-only source list, so the
 * C++ q_re2_shim.cc is not in it and q_re2.c would not link. These fail inertly:
 * a pattern never compiles, so every regex verb signals the bare 'regex, which
 * is what the wasm build has always done. Kept here on the ipc_stub.c precedent
 * so the native tree needs no #ifdef. */
#include "qlang/io/q_re2_abi.h"

void* pqre2_compile(const char* pat, int64_t patn, int* err) {
    (void)pat;
    (void)patn;
    *err = PQRE2_BAD_PATTERN;
    return 0;
}

void pqre2_release(void* prog) { (void)prog; }

int pqre2_ngroups(void* prog) {
    (void)prog;
    return 0;
}

int pqre2_match(void* prog, const char* s, int64_t sn, int64_t start, int anchor,
                int64_t* out, int maxg) {
    (void)prog;
    (void)s;
    (void)sn;
    (void)start;
    (void)anchor;
    (void)out;
    (void)maxg;
    return 0;
}

int pqre2_replace(void* prog, const char* s, int64_t sn, const char* r, int64_t rn,
                  int global, char** out, int64_t* outn) {
    (void)prog;
    (void)s;
    (void)sn;
    (void)r;
    (void)rn;
    (void)global;
    (void)out;
    (void)outn;
    return -1;
}

int pqre2_escape(const char* s, int64_t sn, char** out, int64_t* outn) {
    (void)s;
    (void)sn;
    (void)out;
    (void)outn;
    return -1;
}

void pqre2_freestr(char* p) { (void)p; }
