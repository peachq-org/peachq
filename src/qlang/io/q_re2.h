/* q_re2 — the compile cache over the linked-in RE2 shim, and the raw match
 * calls.  Everything q-shaped (verb surface, shapes, q values, the 'regex
 * class) lives in ops/q_regex.c; nothing here builds a ray_t. */
#ifndef QLANG_IO_Q_RE2_H
#define QLANG_IO_Q_RE2_H

#include <stdbool.h>
#include <stdint.h>

typedef struct q_re2_prog q_re2_prog;   /* opaque: one cached compiled pattern */

/* The DuckDB release the vendored RE2 came from (see q_re2_pin.h). */
const char* q_re2_pin(void);

/* pattern -> compiled program, from the cache when already there.  NULL when
 * the pattern is bad.  RE2 compilation costs orders of magnitude more than a
 * match, so distributing one pattern over a million-row column must compile it
 * once. */
q_re2_prog* q_re2_get(const char* p, int64_t pn);

int         q_re2_ngroups(const q_re2_prog* prog);

/* First match at/after `start`, as (offset,length) group pairs in `out`;
 * anchor 1 = the whole subject must match.  See q_re2_abi.h for the contract. */
int         q_re2_match(const q_re2_prog* prog, const char* s, int64_t sn,
                        int64_t start, int anchor, int64_t* out, int maxg);

/* 0 = ok, *out shim-owned (release with q_re2_freestr); -1 = bad rewrite. */
int         q_re2_replace(const q_re2_prog* prog, const char* s, int64_t sn,
                          const char* r, int64_t rn, bool global,
                          char** out, int64_t* outn);
/* RE2::QuoteMeta: the pattern matching `s` literally.  Needs no program — this
 * is what replaced DuckDB's `l` (literal) option, which was an RE2::Options
 * property and so could never have an in-pattern spelling.  0 = ok. */
int         q_re2_escape(const char* s, int64_t sn, char** out, int64_t* outn);

void        q_re2_freestr(char* p);

/* Drop every cached program — q_runtime_destroy, so a compiled pattern never
 * outlives the runtime that compiled it (and never reads as a leak). */
void        q_re2_reset(void);

#endif
