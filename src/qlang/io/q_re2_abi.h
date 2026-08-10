/* q_re2_abi — the extern "C" ABI of libpqre2, openq's dlopen'd RE2 module.
 *
 * RE2 is C++ and `./q` stays pure C17 with no libstdc++ dependency, so RE2
 * lives in a separate shared object (built from third_party/re2 plus
 * q_re2_shim.cc) that the engine loads at first use.  Both sides include this
 * header, so the seam cannot drift: the shim's definitions are checked against
 * the prototypes, and the loader's fn-pointer table mirrors them member for
 * member, in the order the symbol-name array in q_re2.c lists.
 *
 * A compiled pattern carries NO options: q has no options argument, flags ride
 * inside the pattern as (?i)/(?s)/(?m), and the two RE2 Options with no inline
 * form are functions of their own instead — `escape` (QuoteMeta, replacing the
 * `l` literal option) and replace's `global` argument (replacing `g`).
 *
 * Matches come back as byte OFFSETS into the caller's own subject — only
 * `replace` and `escape` produce new text, and only they allocate. */
#ifndef Q_RE2_ABI_H
#define Q_RE2_ABI_H

#include <stdint.h>

enum { PQRE2_OK = 0, PQRE2_BAD_PATTERN = 1 };

/* The module is built -fvisibility=hidden so RE2's own symbols stay private;
 * these eight are the whole of its public face and say so. */
#if defined(_WIN32)
#define PQRE2_API __declspec(dllexport)
#else
#define PQRE2_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The DuckDB release third_party/re2 was taken from.  RE2 as DuckDB vendors it
 * carries no version string of its own, so this pin IS the version, and the
 * loader refuses a module whose pin differs from its own. */
PQRE2_API const char* pqre2_pin(void);

/* NULL on failure, *err then PQRE2_BAD_PATTERN. */
PQRE2_API void* pqre2_compile(const char* pat, int64_t patn, int* err);
PQRE2_API void  pqre2_release(void* prog);
PQRE2_API int   pqre2_ngroups(void* prog);

/* First match at/after `start`; anchor 1 = the WHOLE subject must match.
 * Fills out[2*i], out[2*i+1] with group i's (offset, length) — group 0 is the
 * whole match, a group that did not participate gets offset -1.  Returns the
 * slots filled (>= 1) on a match, 0 on no match; maxg 0 is a pure test and is
 * clamped up from, maxg above the pattern's group count clamped down to it. */
PQRE2_API int   pqre2_match(void* prog, const char* s, int64_t sn, int64_t start,
                            int anchor, int64_t* out, int maxg);

/* Rewrite `r` (\1..\9 name capture groups).  0 = ok, *out module-owned (free
 * with pqre2_freestr); -1 = the rewrite string does not fit the pattern. */
PQRE2_API int   pqre2_replace(void* prog, const char* s, int64_t sn, const char* r,
                              int64_t rn, int global, char** out, int64_t* outn);

/* RE2::QuoteMeta — the pattern that matches `s` literally.  0 = ok, *out
 * module-owned.  Needs no compiled program: it is pure text escaping. */
PQRE2_API int   pqre2_escape(const char* s, int64_t sn, char** out, int64_t* outn);

PQRE2_API void  pqre2_freestr(char* p);

#ifdef __cplusplus
}
#endif

/* The dlsym'd table — member order IS the order of RE2_SYMS in q_re2.c. */
typedef struct {
    const char* (*pin)(void);
    void* (*compile)(const char* pat, int64_t patn, int* err);
    void  (*release)(void* prog);
    int   (*ngroups)(void* prog);
    int   (*match)(void* prog, const char* s, int64_t sn, int64_t start, int anchor,
                   int64_t* out, int maxg);
    int   (*replace)(void* prog, const char* s, int64_t sn, const char* r, int64_t rn,
                     int global, char** out, int64_t* outn);
    int   (*escape)(const char* s, int64_t sn, char** out, int64_t* outn);
    void  (*freestr)(char* p);
} q_re2_api_t;

#endif
