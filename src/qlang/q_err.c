/* q-side error classes — see q_err.h.
 *
 * The bare-class ruling (no embedded message strings): q code raises errors
 * by CLASS only via q_err(QE_x).  The class travels in the error's 7-byte
 * sdata (base display unchanged) with the enum stamped in aux[0]; q-side
 * display (q_repl) and the kdb wire (net/q_wire.c) recover the FULL class
 * name from the enum, so 'mismatch' is no longer truncated to 'mismatc'
 * q-side.  Base's own error path is untouched. */
#include "qlang/q_err.h"

static const char* const q_err_names[QE__COUNT] = {
    [QE_TYPE]="type", [QE_LENGTH]="length", [QE_RANK]="rank",
    [QE_DOMAIN]="domain", [QE_PARSE]="parse", [QE_LIMIT]="limit",
    [QE_NYI]="nyi", [QE_ASSIGN]="assign", [QE_CAST]="cast",
    [QE_VALUE]="value", [QE_FROM]="from", [QE_COND]="cond",
    [QE_MATCH]="match", [QE_MISMATCH]="mismatch", [QE_INSERT]="insert",
    [QE_LOOP]="loop", [QE_STACK]="stack", [QE_STOP]="stop",
    [QE_STYPE]="stype", [QE_NOAMEND]="noamend", [QE_SPLAY]="splay",
    [QE_PAR]="par", [QE_PART]="part", [QE_STEP]="step", [QE_DUP]="dup",
    [QE_RESTRICTED]="restricted", [QE_ACCESS]="access", [QE_OS]="os",
    [QE_CONN]="conn", [QE_WSFULL]="wsfull",
    [QE_OOM]="oom", [QE_IO]="io", [QE_NAME]="name", [QE_INDEX]="index",
    [QE_RESERVE]="reserve", [QE_INIT]="init", [QE_RANGE]="range",
    [QE_SCHEMA]="schema", [QE_CORRUPT]="corrupt", [QE_CANCEL]="cancel",
    [QE_VERSION]="version",
};

ray_t* q_err(q_err_e e) {
    const char* cls = (unsigned)e < QE__COUNT ? q_err_names[e] : "error";
    ray_t* err = ray_error(cls, NULL);
    if (RAY_IS_ERR(err) && err != RAY_OOM_OBJ)
        err->aux[0] = (uint8_t)(e + 1);
    return err;
}

const char* q_err_class(ray_t* err) {
    if (!RAY_IS_ERR(err)) return NULL;
    uint8_t s = err->aux[0];
    if (s && s <= QE__COUNT) return q_err_names[s - 1];
    return ray_err_code(err);
}
