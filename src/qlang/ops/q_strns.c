/* q_strns — the `.str.i.*` strip natives behind lib/str.q's Python-shaped
 * `.str` namespace.  In C because character scanning in q is slow and these
 * three are the hot ones; everything else in `.str` stays q.
 * The stripped set is Python's `str.strip()` default WHITESPACE, spelled
 * exactly " \t\n\r" — NOT q's `trim`, which strips the char null.
 * Each native is a LEAF: it answers for one piece of text and knows nothing of
 * lists, symbols or dicts.  Shape is the wrapper's job (lib/str.q), which is
 * also where the symbol coercion lives, so the discoverable q surface and the
 * dispatch have one home. */
#include "qlang/ops/q_strns.h"
#include "qlang/base/q_err.h"
#include "qlang/q_env.h" /* q_env_bind — the .str.i.* bindings */
#include "qlang/q_prim.h" /* q_str_text_bytes — THE text-bytes accessor */
#include "lang/env.h"    /* ray_fn_unary */
#include "lang/eval.h"   /* RAY_FN_NONE */
#include <rayforce.h>
#include <string.h>

enum { STRNS_BOTH = 0, STRNS_LEAD = 1, STRNS_TRAIL = 2 };

static int strns_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* Answers a fresh char VECTOR for every accepted input, char atoms included:
 * Python has no character type, so `.str.strip "a"` is the one-char string. */
static ray_t* strns_strip(ray_t* x, int mode) {
    const char* p;
    int64_t     n;
    if (!x) return q_err(QE_TYPE);
    if (RAY_IS_ERR(x)) { ray_retain(x); return x; }  /* errors propagate unmodified */
    if (!q_str_text_bytes(x, &p, &n)) return q_err(QE_TYPE);
    int64_t a = 0, b = n;
    if (mode != STRNS_TRAIL) while (a < b && strns_is_ws(p[a])) a++;
    if (mode != STRNS_LEAD) while (b > a && strns_is_ws(p[b - 1])) b--;
    return ray_charv(p + a, b - a);
}

static ray_t* strns_strip_fn(ray_t* x)  { return strns_strip(x, STRNS_BOTH); }
static ray_t* strns_lstrip_fn(ray_t* x) { return strns_strip(x, STRNS_LEAD); }
static ray_t* strns_rstrip_fn(ray_t* x) { return strns_strip(x, STRNS_TRAIL); }

static void strns_bind_fn(const char* name, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(name, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

void q_strns_register(void) {
    strns_bind_fn(".str.i.strip",  strns_strip_fn);
    strns_bind_fn(".str.i.lstrip", strns_lstrip_fn);
    strns_bind_fn(".str.i.rstrip", strns_rstrip_fn);
}
