/* q_type — the type-axis home (contract: q_type.h): the q-side lane accessors,
 * the type-number / null axes, and the tag<->name vocabulary. Relocated out of
 * the frozen base header (lane extensions) and out of q_builtins/q_dollar/
 * q_math/q_table (the type + null knowledge) so each has ONE obvious home. */
#include "qlang/q_type.h"
#include "qlang/q_err.h"          /* q_err / QE_TYPE — the throwing gates */
#include "qlang/eval/q_eval.h"    /* q_eval_apply_carrier_kind — fn-value type */
#include "lang/internal.h"        /* as_i64, is_numeric/is_temporal, is_collection,
                                   * atomic_map_unary, ray_nil_fn */
#include "table/sym.h"            /* ray_sym_str — the null-symbol divergence */
#include "qlang/q_registry.h"     /* q_list_collapse — q_null_wrap collapse */

int64_t q_type_as_i64(ray_t* x) {
    if (RAY_IS_TEMPORAL32(-x->type)) return (int64_t)x->i32;
    if (RAY_IS_TEMPORAL64(-x->type)) return x->i64;
    return as_i64(x);
}

int q_type_is_numeric_or_temporal(ray_t* x) {
    return is_numeric(x) || is_temporal(x) || RAY_IS_TEMPORALF(-x->type);
}

int q_type_is_bool(ray_t* x) {
    return x->type == -RAY_BOOL;
}

/* ---- the kdb type number of a value (the `type` verb's knowledge) --------- */

/* q `type` as the kdb short: FUNCTION values report 100h lambda / 104h
 * projection / 106+adv derived / 102h operator / 101h generic-null unary; every
 * other value's internal tag ALREADY IS its kdb type number (include/rayforce.h:
 * long 7, sym 11, list 0, table 98, dict 99), atoms stored NEGATIVE. A native
 * -RAY_STR string reports -10h (char ATOM) vs kdb's 10h char VECTOR — a recorded
 * string-model divergence (ARCHITECTURE.md; cast/type-deferred.qcmd). Callers on
 * a NULL x keep their own base-verb fallback (q_builtins). */
int8_t q_type_of(ray_t* x) {
    if (RAY_IS_NULL(x)) return 101;
    switch (q_eval_apply_carrier_kind(x)) {
    case Q_EVAL_CAR_LAMBDA: return 100;
    case Q_EVAL_CAR_PROJ:   return 104;
    case Q_EVAL_CAR_DERIV: {
        ray_t** c = (ray_t**)ray_data(x);
        int adv = c[2] ? (int)c[2]->i64 : 0;
        return (int8_t)(106 + (adv >= 0 && adv < 6 ? adv : 0));
    }
    default: break;
    }
    if (x->type == RAY_LAMBDA) return 100;
    if (x->type == RAY_UNARY || x->type == RAY_BINARY || x->type == RAY_VARY)
        return 102;
    return x->type;
}

/* ---- tag <-> name vocabulary --------------------------------------------- */

/* Vector tag -> kdb type name (`meta`/`key` type rows, empty-vec display).
 * No `default:` — total over the value band (#209): a new datatype refuses to
 * build until it names its q-spelling here. LIST/STR have no scalar name; the
 * -RAY_STR atom shim names `char` at the call sites. */
const char* q_type_qname(int8_t t) {
    switch ((ray_type_e)t) {
    case RAY_BOOL:      return "boolean";
    case RAY_BYTE_ONLY: return "byte";
    case RAY_I16:       return "short";
    case RAY_I32:       return "int";
    case RAY_I64:       return "long";
    case RAY_F32:       return "real";
    case RAY_F64:       return "float";
    case RAY_SYM:       return "symbol";
    case RAY_GUID:      return "guid";       /* `key 0#0Ng` -> `guid (was a gap) */
    case RAY_DATE:      return "date";
    case RAY_MONTH:     return "month";
    case RAY_MINUTE:    return "minute";
    case RAY_SECOND:    return "second";
    case RAY_TIME:      return "time";
    case RAY_TIMESPAN:  return "timespan";
    case RAY_TIMESTAMP: return "timestamp";
    case RAY_DATETIME:  return "datetime";
    case RAY_CHARV: return "char";
    case RAY_LIST: case RAY_STR: return NULL;   /* boxed / physical: unnamed */
    }
    return NULL;   /* unreachable: value band is exhausted above */
}

/* tag -> rayfall `as` type-sym spelling (cast delegation targets only) */
const char* q_type_rayname(int8_t tag) {
    switch (tag) {
    case RAY_BOOL: return "BOOL"; case RAY_BYTE_ONLY: return "U8";
    case RAY_I16:  return "I16";  case RAY_I32: return "I32";
    case RAY_I64:  return "I64";  case RAY_F64: return "F64";
    case RAY_DATE: return "DATE"; case RAY_TIME: return "TIME";
    case RAY_MONTH: return "MONTH";
    case RAY_MINUTE: return "MINUTE";
    case RAY_SECOND: return "SECOND";
    case RAY_TIMESPAN: return "TIMESPAN";
    case RAY_TIMESTAMP: return "TIMESTAMP";
    case RAY_DATETIME: return "DATETIME";
    default:       return NULL;
    }
}

/* ---- int/float admission (which numeric lane may a verb read) ------------- */

/* Is vector tag `t` legal as a ROW INDEX (uniform-structure-dispatch §2.2:
 * any int-backed index indexes)?  Returns the element width in bytes so
 * consumers key on WIDTH, never enumerate tags: 8/4/2 read signed, 1 reads
 * unsigned (U8); 0 = not an int index.  SYM is i64-backed but EXCLUDED —
 * a sym index means column access, never a row.  BOOL is excluded pending
 * a doc citation for boolean indexing.  Atom callers pass -atom->type;
 * every accepted ATOM stores its payload in .i64 (vec/atom.c), so as_i64's
 * fallback reads it correctly. */
int q_type_int_index_width(int8_t t) {
    switch (t) {
    case RAY_I64: return 8;
    case RAY_I32: return 4;
    case RAY_I16: return 2;
    case RAY_BYTE_ONLY: return 1;
    default:
        if (RAY_IS_TEMPORAL64(t)) return 8;   /* timestamp, timespan */
        if (RAY_IS_TEMPORAL32(t)) return 4;   /* month date minute second time */
        return 0;
    }
}

/* Strict cast: cast-or-fail, TYPE-strict (an integral-valued float refuses);
 * typed nulls pass through as sentinel payloads — value checks stay at the
 * call site.  Accepted set = the q_type_int_index_width law: I64/I32/I16/U8 +
 * int-backed temporal ATOMS; never sym/bool/float/structures.
 * Returns 1 + *out, or 0 on refusal. */
int q_type_strict_i64(ray_t* x, int64_t* out) {
    if (!x || x->type >= 0) return 0;
    switch (q_type_int_index_width((int8_t)-x->type)) {
    case 8: *out = x->i64;          return 1;
    case 4: *out = (int64_t)x->i32; return 1;
    case 2: *out = (int64_t)x->i16; return 1;
    case 1: *out = (int64_t)x->u8;  return 1;
    default: return 0;
    }
}

/* Float twin: F64/F32/DATETIME (f64-slot payloads) + the q_type_strict_i64 set. */
int q_type_strict_f64(ray_t* x, double* out) {
    if (!x || x->type >= 0) return 0;
    if (x->type == -RAY_F64 || RAY_IS_TEMPORALF(-x->type)) { *out = x->f64; return 1; }
    if (x->type == -RAY_F32) { *out = (double)(float)x->f64; return 1; }
    int64_t v;
    if (!q_type_strict_i64(x, &v)) return 0;
    *out = (double)v;
    return 1;
}

/* Throwing gate for TERMINAL sites (failure = error): NULL on success, else the
 * bare 'type class (q_err carries no string post-#305). `what` is the caller's
 * site context ("verb: role") reserved for a future error-context layer — held,
 * not embedded. The silent probe form above is for dispatch sites (next arm). */
ray_t* q_type_i64_or_err(ray_t* x, int64_t* out, const char* what) {
    return q_type_strict_i64(x, out) ? NULL : q_err(QE_TYPE);
}

/* Type-facts helpers (I64/I32/I16 only — vs/sv base-encode domain). */
int q_type_is_int_atom(ray_t* x) {
    return x && (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16);
}
int q_type_is_int_vec(ray_t* x) {
    return x && (x->type == RAY_I64 || x->type == RAY_I32 || x->type == RAY_I16);
}
int64_t q_type_ivec_get(ray_t* v, int64_t i) {
    const void* d = ray_data(v);
    return v->type == RAY_I64 ? ((const int64_t*)d)[i]
         : v->type == RAY_I32 ? (int64_t)((const int32_t*)d)[i]
                              : (int64_t)((const int16_t*)d)[i];
}
int64_t q_type_iatom_val(ray_t* x) {
    return x->type == -RAY_I64 ? x->i64
         : x->type == -RAY_I32 ? (int64_t)x->i32 : (int64_t)x->i16;
}

/* ---- keyed-table shape (a type question, owner-ruled) -------------------- */

/* A keyed table is a RAY_DICT whose keys AND values are both tables. */
int q_type_is_keyed(ray_t* y) {
    if (!y || y->type != RAY_DICT) return 0;
    ray_t* k = ray_dict_keys(y);
    ray_t* v = ray_dict_vals(y);
    return k && v && k->type == RAY_TABLE && v->type == RAY_TABLE;
}

/* ---- null axis (owner-ruled: null is not math) --------------------------- */

/* q treats the null symbol ` AS null — shared predicate (the `null` verb,
 * ops/q_vs_sv.c dispatch). */
int q_type_is_null_sym(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return 0;
    ray_t* s = ray_sym_str(x->i64);
    int z = s && ray_str_len(s) == 0;
    return z;
}

/* q-layer null test: the base engine's `ray_nil_fn` treats sym id 0 as the
 * EMPTY symbol (a value, include/rayforce.h SYM case), but q treats the null
 * symbol `` ` `` AS null (`null \`` -> 1b).  This wrapper special-cases the
 * null symbol here in the q layer so the divergence stays out of base rayfall,
 * whose own paths rely on sym-0-as-empty.  Drives `q_null_wrap` for both the
 * atom path and the per-element `atomic_map_unary` recursion (nested lists /
 * symbol vectors reconstruct null-sym atoms via collection_elem). */
static ray_t* nil_fn(ray_t* x) {
    if (q_type_is_null_sym(x)) return ray_bool(true);
    return ray_nil_fn(x);
}

/* q `null x` — elementwise null test.  Drives the engine's atomic `nil?`
 * (ray_nil_fn) through atomic_map_unary so it broadcasts over typed vectors
 * AND nested general lists at every depth; collection_elem reconstructs
 * typed-null atoms, so nulls are SEEN (unlike other atomics, which stay
 * null-avoiding via the dispatch guards).  Registered RAY_FN_NONE — NOT
 * ATOMIC — so it receives the whole argument here and owns the collapse: a
 * heterogeneous input list yields a homogeneous bool-atom run that
 * q_list_collapse folds to a bool vector (`null (1;\`a;2.5;"x")` -> 0000b),
 * while a nested list yields a list of bool VECTORS that q_list_collapse
 * leaves intact (multi-line, `null (0N 1;2 0N)` -> 10b / 01b). */
ray_t* q_null_wrap(ray_t* x) {
    ray_t* r = is_collection(x) ? atomic_map_unary(nil_fn, x) : nil_fn(x);
    if (!r || RAY_IS_ERR(r) || r->type != RAY_LIST) return r;
    ray_t* c = q_list_collapse(r);   /* owned: retains-or-builds */
    ray_release(r);
    return c;
}
