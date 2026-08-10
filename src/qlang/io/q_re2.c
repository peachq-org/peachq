/* q_re2 — how openq reaches RE2, and the one place a pattern gets compiled.
 *
 * RE2 is C++, and the owner's ruling is that `./q` stays pure C17 with no
 * libstdc++ dependency.  So RE2 ships as a SEPARATE shared object (libpqre2,
 * built from third_party/re2 + q_re2_shim.cc) that this file dlopens on first
 * use, exactly the way q_duckdb.c reaches libduckdb: PEACHQ_RE2_LIB set =
 * EXCLUSIVE, else $QHOME -> exe dir -> the system search path.  A missing
 * module is not a crash and not a special build: every call simply reports
 * unavailable and ops/q_regex.c signals the bare 'regex.
 *
 * The version pin is checked at load.  RE2 as DuckDB vendors it has no version
 * string, so the pin is the DuckDB release it came from (q_re2_pin.h) and it is
 * compiled into BOTH sides — a module built from a different tree is refused
 * rather than silently changing what a predicate means. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/io/q_re2.h"
#include "qlang/io/q_re2_abi.h"
#include "qlang/io/q_re2_pin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
/* no dynamic loading in the wasm build — the loader is a constant failure */
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

struct q_re2_prog {
    char*    key;      /* the pattern bytes — the whole cache identity, since a
                        * compiled pattern carries no options */
    int64_t  keyn;
    void*    prog;     /* the module's compiled RE2 */
    int      ngroups;
    uint64_t used;     /* LRU stamp; 0 = free slot */
};

#define RE2_CACHE 64

static struct {
    void*       dl;
    int         state;   /* 0 = untried, 1 = loaded, 2 = unavailable */
    q_re2_api_t api;
} g_re2;

static q_re2_prog g_cache[RE2_CACHE];
static uint64_t   g_clock;

/* Member order of q_re2_api_t — the table is filled by walking both in step. */
static const char* const RE2_SYMS[] = {
    "pqre2_pin", "pqre2_compile", "pqre2_release", "pqre2_ngroups",
    "pqre2_match", "pqre2_replace", "pqre2_escape", "pqre2_freestr",
};
#define RE2_NSYMS (sizeof RE2_SYMS / sizeof *RE2_SYMS)

#if defined(_WIN32)
#define RE2_LIB_BASENAME "pqre2.dll"
#elif defined(__APPLE__)
#define RE2_LIB_BASENAME "libpqre2.dylib"
#else
#define RE2_LIB_BASENAME "libpqre2.so"
#endif

#if !defined(__EMSCRIPTEN__)
static void* re2_dlopen(const char* path) {
#if defined(_WIN32)
    return (void*)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void* re2_dlsym(void* dl, const char* name) {
#if defined(_WIN32)
    return (void*)(uintptr_t)GetProcAddress((HMODULE)dl, name);
#else
    return dlsym(dl, name);
#endif
}

/* The module ships beside the binary, so the exe dir is a first-class
 * candidate (Windows searches it itself; Linux needs /proc).
 * PORTING NOTE: macOS has no /proc, so this step is inert there and the ladder
 * falls through to $QHOME / $PEACHQ_RE2_LIB / the system path — the SAME gap
 * q_duckdb.c has, which is why the fix is one shared exe-dir home for both
 * loaders (_NSGetExecutablePath under __APPLE__) rather than a copy here. */
static void re2_origin_candidate(char* dst, size_t cap) {
    dst[0] = '\0';
#if defined(_WIN32)
    (void)cap;
#else
    char    exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n]      = '\0';
    char* slash = strrchr(exe, '/');
    if (!slash) return;
    *slash = '\0';
    snprintf(dst, cap, "%s/%s", exe, RE2_LIB_BASENAME);
#endif
}
#endif /* !__EMSCRIPTEN__ */

static void re2_load(void) {
    if (g_re2.state) return;
#if defined(__EMSCRIPTEN__)
    g_re2.state = 2;
#else
    void*       dl   = NULL;
    const char* envp = getenv("PEACHQ_RE2_LIB");
    if (envp && *envp) {
        dl = re2_dlopen(envp);
        if (!dl) { g_re2.state = 2; return; }
    }
    if (!dl) {
        const char* qh = getenv("QHOME");
        if (qh && *qh) {
            char cand[600];
            snprintf(cand, sizeof cand, "%s/%s", qh, RE2_LIB_BASENAME);
            dl = re2_dlopen(cand);
        }
    }
    if (!dl) {
        char origin[600];
        re2_origin_candidate(origin, sizeof origin);
        if (origin[0]) dl = re2_dlopen(origin);
    }
    if (!dl) dl = re2_dlopen(RE2_LIB_BASENAME);
    if (!dl) { g_re2.state = 2; return; }
    void** slots = (void**)&g_re2.api;
    for (size_t i = 0; i < RE2_NSYMS; i++) {
        slots[i] = re2_dlsym(dl, RE2_SYMS[i]);
        if (!slots[i]) {
            g_re2.state = 2;
            memset(&g_re2.api, 0, sizeof g_re2.api);
            return;
        }
    }
    const char* pin = g_re2.api.pin();
    if (!pin || strcmp(pin, PQRE2_DUCKDB_PIN) != 0) {
        g_re2.state = 2;
        memset(&g_re2.api, 0, sizeof g_re2.api);
        return;
    }
    g_re2.dl    = dl;
    g_re2.state = 1;
#endif
}

bool q_re2_available(void) {
    re2_load();
    return g_re2.state == 1;
}

const char* q_re2_pin(void) { return PQRE2_DUCKDB_PIN; }

static void re2_evict(q_re2_prog* e) {
    if (e->prog) g_re2.api.release(e->prog);
    free(e->key);
    memset(e, 0, sizeof *e);
}

q_re2_prog* q_re2_get(const char* p, int64_t pn) {
    if (!q_re2_available()) return NULL;
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
    void* prog = g_re2.api.compile(p, pn, &err);
    if (!prog) return NULL;
    char* key = malloc((size_t)pn + 1);
    if (!key) { g_re2.api.release(prog); return NULL; }
    memcpy(key, p, (size_t)pn);
    re2_evict(victim);
    victim->key     = key;
    victim->keyn    = pn;
    victim->prog    = prog;
    victim->ngroups = g_re2.api.ngroups(prog);
    victim->used    = ++g_clock;
    return victim;
}

int q_re2_ngroups(const q_re2_prog* prog) { return prog->ngroups; }

int q_re2_match(const q_re2_prog* prog, const char* s, int64_t sn, int64_t start,
                int anchor, int64_t* out, int maxg) {
    return g_re2.api.match(prog->prog, s, sn, start, anchor, out, maxg);
}

int q_re2_replace(const q_re2_prog* prog, const char* s, int64_t sn, const char* r,
                  int64_t rn, bool global, char** out, int64_t* outn) {
    return g_re2.api.replace(prog->prog, s, sn, r, rn, global ? 1 : 0, out, outn);
}

int q_re2_escape(const char* s, int64_t sn, char** out, int64_t* outn) {
    if (!q_re2_available()) return -1;
    return g_re2.api.escape(s, sn, out, outn);
}

void q_re2_freestr(char* p) { g_re2.api.freestr(p); }

void q_re2_reset(void) {
    if (g_re2.state != 1) return;
    for (int i = 0; i < RE2_CACHE; i++)
        if (g_cache[i].used) re2_evict(&g_cache[i]);
    g_clock = 0;
}
