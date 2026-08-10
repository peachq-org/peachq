/* q_re2 — the one place a q pattern becomes a compiled RE2, and the cache over
 * it.  RE2 is linked in on every platform (ARCHITECTURE.md, 2026-08-10), so the
 * shim behind q_re2_abi.h is CALLED, never loaded: no availability state, and a
 * NULL program means the pattern was bad. */
#include "qlang/io/q_re2.h"
#include "qlang/io/q_re2_abi.h"
#include "qlang/io/q_re2_pin.h"
#include <stdlib.h>
#include <string.h>

struct q_re2_prog {
    char*    key;      /* the pattern bytes — the whole cache identity, since a
                        * compiled pattern carries no options */
    int64_t  keyn;
    void*    prog;     /* the shim's compiled RE2 */
    int      ngroups;
    uint64_t used;     /* LRU stamp; 0 = free slot */
};

#define RE2_CACHE 64

static q_re2_prog g_cache[RE2_CACHE];
static uint64_t   g_clock;

const char* q_re2_pin(void) { return PQRE2_DUCKDB_PIN; }

static void re2_evict(q_re2_prog* e) {
    if (e->prog) pqre2_release(e->prog);
    free(e->key);
    memset(e, 0, sizeof *e);
}

q_re2_prog* q_re2_get(const char* p, int64_t pn) {
    q_re2_prog* victim = &g_cache[0];   /* free slot, else least recently used */
    for (int i = 0; i < RE2_CACHE; i++) {
        q_re2_prog* e = &g_cache[i];
        if (e->used && e->keyn == pn && memcmp(e->key, p, (size_t)pn) == 0) {
            e->used = ++g_clock;
            return e;
        }
        if (e->used < victim->used) victim = e;
    }
    int   err  = PQRE2_OK;
    void* prog = pqre2_compile(p, pn, &err);
    if (!prog) return NULL;
    char* key = malloc((size_t)pn + 1);
    if (!key) { pqre2_release(prog); return NULL; }
    memcpy(key, p, (size_t)pn);
    re2_evict(victim);
    victim->key     = key;
    victim->keyn    = pn;
    victim->prog    = prog;
    victim->ngroups = pqre2_ngroups(prog);
    victim->used    = ++g_clock;
    return victim;
}

int q_re2_ngroups(const q_re2_prog* prog) { return prog->ngroups; }

int q_re2_match(const q_re2_prog* prog, const char* s, int64_t sn, int64_t start,
                int anchor, int64_t* out, int maxg) {
    return pqre2_match(prog->prog, s, sn, start, anchor, out, maxg);
}

int q_re2_replace(const q_re2_prog* prog, const char* s, int64_t sn, const char* r,
                  int64_t rn, bool global, char** out, int64_t* outn) {
    return pqre2_replace(prog->prog, s, sn, r, rn, global ? 1 : 0, out, outn);
}

int q_re2_escape(const char* s, int64_t sn, char** out, int64_t* outn) {
    return pqre2_escape(s, sn, out, outn);
}

void q_re2_freestr(char* p) { pqre2_freestr(p); }

void q_re2_reset(void) {
    for (int i = 0; i < RE2_CACHE; i++)
        if (g_cache[i].used) re2_evict(&g_cache[i]);
    g_clock = 0;
}
