/* q-side error classes — see q_err.c. */
#ifndef QLANG_Q_ERR_H
#define QLANG_Q_ERR_H

#include <rayforce.h>

/* Kdb block: basics/errors.md classes reachable from the q surface.  OURS
 * block: admitted non-kdb classes (base-engine spellings + q-layer
 * divergences).  Values are runtime-only — stamped into the error's unused
 * aux[0] (as e+1, so 0 = "no q enum") — so the enum order is never persisted. */
typedef enum {
    /* ---- kdb classes (basics/errors.md) ---- */
    QE_TYPE, QE_LENGTH, QE_RANK, QE_DOMAIN, QE_PARSE, QE_LIMIT,
    QE_NYI, QE_ASSIGN, QE_CAST, QE_VALUE, QE_FROM, QE_COND,
    QE_MATCH, QE_MISMATCH, QE_INSERT, QE_LOOP, QE_STACK, QE_STOP,
    QE_STYPE, QE_NOAMEND, QE_SPLAY, QE_PAR, QE_PART, QE_STEP,
    QE_DUP, QE_RESTRICTED, QE_ACCESS, QE_OS, QE_CONN, QE_WSFULL,
    /* ---- OURS: admitted non-kdb classes ---- */
    QE_OOM, QE_IO, QE_NAME, QE_INDEX, QE_RESERVE, QE_INIT,
    QE_RANGE, QE_SCHEMA, QE_CORRUPT, QE_CANCEL, QE_VERSION,
    QE__COUNT
} q_err_e;

/* Construct a q error value (rc=1, owned).  sdata carries the base-truncated
 * 7-byte class so base display stays byte-identical; aux[0] carries the enum
 * so q-side display and the wire can render the full class name. */
ray_t* q_err(q_err_e e);

/* The NAME is the class — kdb signals the offending name itself for a bad or
 * missing namespace (`\d .a.b` -> '.a.b, `\a .n` -> '.n, `tables `.n`).  The
 * 7-byte error-code width truncates a longer name (by design). */
ray_t* q_err_name(const char* p, size_t n);

/* Full kdb class string for q-side rendering (display + wire): the stamped
 * enum's full name (dissolving the 7-byte 'mismatc' truncation), else the
 * stored 7-byte code for base-constructed errors.  NULL when not an error. */
const char* q_err_class(ray_t* err);

#endif
