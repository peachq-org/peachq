/* q-side error classes — see q_err.h.
 *
 * The bare-class ruling (no embedded message strings): q code raises errors
 * by CLASS via q_err(QE_x), optionally with payload TEXT (below) — never
 * per-verb prose.  The class travels in the error's 7-byte
 * sdata (base display unchanged) with the enum stamped in aux[0]; q-side
 * display (q_repl) and the kdb wire (net/q_wire.c) recover the FULL class
 * name from the enum, so 'mismatch' is no longer truncated to 'mismatc'
 * q-side.  Base's own error path is untouched.
 *
 * Error TEXT rides beside the class, never inside it (2026-07-30): Signal /
 * explicit return / undefined-name errors stash their payload in
 * __VM->raise_val (only other writer: base ray_raise_fn, rayfall-only).
 * Consume discipline: trap takes or drops it, statement entry drops as
 * backstop — a stale payload would caption the NEXT error. */
#include "qlang/q_err.h"
#include "core/runtime.h"   /* __VM->raise_val — the pending-payload slot */
#include <string.h>

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
    [QE_VERSION]="version", [QE_SIGNAL]="signal", [QE_RETURN]="return",
};

/* the single call into the base builder (the bare-class ruling made physical) */
ray_t* q_err(q_err_e e) {
    const char* cls = (unsigned)e < QE__COUNT ? q_err_names[e] : "error";
    ray_t* err = ray_error(cls, NULL);
    if (RAY_IS_ERR(err) && err != RAY_OOM_OBJ)
        err->aux[0] = (uint8_t)(e + 1);
    return err;
}

ray_t* q_err_signal(q_err_e e, ray_t* payload) {
    if (payload && !RAY_IS_ERR(payload) && __VM) {
        ray_retain(payload);
        if (__VM->raise_val) ray_release(__VM->raise_val);
        __VM->raise_val = payload;
    }
    return q_err(e);
}

ray_t* q_err_name(const char* p, size_t n) {
    ray_t* t = ray_charv(p, (int64_t)n);
    if (!t || RAY_IS_ERR(t)) return q_err(QE_NAME);
    ray_t* e = q_err_signal(QE_NAME, t);
    ray_release(t);
    return e;
}

/* q_err_text's read-side twin (wire decode): a known class name regains its
 * enum stamp; any other text is the name-signal semantic — payload-carried,
 * surviving the 7-byte sdata.  QE_SIGNAL/QE_RETURN are internal sentinels,
 * never reconstructed from text. */
ray_t* q_err_from_text(const char* p, size_t n) {
    for (unsigned i = 0; i < QE_SIGNAL; i++)
        if (strlen(q_err_names[i]) == n && memcmp(q_err_names[i], p, n) == 0)
            return q_err((q_err_e)i);
    return q_err_name(p, n);
}

const char* q_err_text(ray_t* err, int64_t* len) {
    if (!RAY_IS_ERR(err)) return NULL;
    ray_t* pv = __VM ? __VM->raise_val : NULL;
    if (pv && pv->type == RAY_CHARV) {
        if (len) *len = ray_len(pv);
        return (const char*)ray_data(pv);
    }
    const char* c = q_err_class(err);
    if (len) *len = c ? (int64_t)strlen(c) : 0;
    return c ? c : "";
}

ray_t* q_err_take(void) {
    if (!__VM || !__VM->raise_val) return NULL;
    ray_t* v = __VM->raise_val;
    __VM->raise_val = NULL;
    return v;
}

void q_err_drop(void) {
    if (__VM && __VM->raise_val) {
        ray_release(__VM->raise_val);
        __VM->raise_val = NULL;
    }
}

int q_err_is(ray_t* err, q_err_e e) {
    return RAY_IS_ERR(err) && err->aux[0] == (uint8_t)(e + 1);
}

const char* q_err_class(ray_t* err) {
    if (!RAY_IS_ERR(err)) return NULL;
    uint8_t s = err->aux[0];
    if (s && s <= QE__COUNT) return q_err_names[s - 1];
    return ray_err_code(err);
}
