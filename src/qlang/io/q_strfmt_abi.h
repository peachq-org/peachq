/* q_strfmt_abi — the flat extern "C" seam between peachq's C and the vendored
 * DuckDB fmt.  The re2 boundary's twin (q_re2_abi.h): fmt is C++ and links
 * straight into the executable, so this is a COMPILE-TIME seam, not a module.
 *
 * NOTHING q-shaped crosses it.  An argument has already collapsed to one of
 * four lanes, and the format string has already been rewritten by the scanner
 * in ops/q_strfmt.c — every replacement field carries an explicit index, so
 * fmt does no argument stepping of its own and one q value may appear at
 * several occurrences in different lanes. */
#ifndef Q_STRFMT_ABI_H
#define Q_STRFMT_ABI_H

#include <stdint.h>

enum { PQFMT_LANE_BOOL = 0, PQFMT_LANE_I64 = 1, PQFMT_LANE_F64 = 2, PQFMT_LANE_STR = 3 };
enum { PQFMT_OK = 0, PQFMT_BAD = 1, PQFMT_NOMEM = 2 };

typedef struct {
    int32_t lane;
    union {
        int64_t i; /* PQFMT_LANE_BOOL rides here too: 0 or 1 */
        double  f;
        struct { const char* p; int64_t n; } s;
    } v;
} pqfmt_arg;

#ifdef __cplusplus
extern "C" {
#endif

/* Both return PQFMT_OK with *out shim-owned (free with pqfmt_freestr), or
 * PQFMT_BAD for any grammar/argument error, or PQFMT_NOMEM.  Every C++
 * exception — fmt's own, std::bad_alloc, anything — is caught before the
 * boundary: nothing throws into C. */
int  pqfmt_printf(const char* fmt, int64_t fmtn, const pqfmt_arg* args, int argn, char** out, int64_t* outn);
int  pqfmt_format(const char* fmt, int64_t fmtn, const pqfmt_arg* args, int argn, char** out, int64_t* outn);
void pqfmt_freestr(char* p);

#ifdef __cplusplus
}
#endif

#endif
