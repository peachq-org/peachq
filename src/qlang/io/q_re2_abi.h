/* q_re2_abi — the flat extern "C" seam between peachq's C and the vendored RE2.
 *
 * RE2 is C++ and its objects are linked straight into the executable, so this
 * is a COMPILE-TIME seam, not a module boundary: q_re2_shim.cc is the only C++
 * translation unit and everything RE2-shaped stops there.  Both sides include
 * this header, so the shim's definitions are checked against the prototypes its
 * one caller (q_re2.c) uses.
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

#ifdef __cplusplus
extern "C" {
#endif

/* NULL on failure, *err then PQRE2_BAD_PATTERN. */
void* pqre2_compile(const char* pat, int64_t patn, int* err);
void  pqre2_release(void* prog);
int   pqre2_ngroups(void* prog);

/* First match at/after `start`; anchor 1 = the WHOLE subject must match.
 * Fills out[2*i], out[2*i+1] with group i's (offset, length) — group 0 is the
 * whole match, a group that did not participate gets offset -1.  Returns the
 * slots filled (>= 1) on a match, 0 on no match; maxg 0 is a pure test and is
 * clamped up from, maxg above the pattern's group count clamped down to it. */
int   pqre2_match(void* prog, const char* s, int64_t sn, int64_t start,
                  int anchor, int64_t* out, int maxg);

/* Rewrite `r` (\1..\9 name capture groups).  0 = ok, *out shim-owned (free with
 * pqre2_freestr); -1 = the rewrite string does not fit the pattern. */
int   pqre2_replace(void* prog, const char* s, int64_t sn, const char* r,
                    int64_t rn, int global, char** out, int64_t* outn);

/* RE2::QuoteMeta — the pattern that matches `s` literally.  0 = ok, *out
 * shim-owned.  Needs no compiled program: it is pure text escaping. */
int   pqre2_escape(const char* s, int64_t sn, char** out, int64_t* outn);

void  pqre2_freestr(char* p);

#ifdef __cplusplus
}
#endif

#endif
