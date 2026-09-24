/* q_wasm_buf — the growable byte buffer the wasm host adapters build their answers in:
 * always NUL-terminated, and an allocation failure latches `oom` instead of failing each put. */
#ifndef Q_WASM_BUF_H
#define Q_WASM_BUF_H

#include <stdlib.h>
#include <string.h>

typedef struct { char* p; size_t n, cap; int oom; } wbuf_t;

static inline void wbuf_put(wbuf_t* b, const void* s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->n + n + 1 > cap) cap *= 2;
        char* p = realloc(b->p, cap);
        if (!p) { b->oom = 1; return; }
        b->p = p;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static inline void wbuf_str(wbuf_t* b, const char* s) { wbuf_put(b, s, strlen(s)); }

#endif
