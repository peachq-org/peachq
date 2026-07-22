/* q_dollar — the single C home for the `$` verb (contract: q_dollar.h).
 * PURE value semantics: values -> values, no env/runtime state.  q_dollar is
 * the generic registry row; q_dollar_pad / q_dollar_cast / q_dollar_tok /
 * q_dollar_enum / q_dollar_mmu are the per-operation homes, exposed with types
 * for reuse.  The per-target q_cast_* matrix and the general int-atom helpers
 * (q_is_int_atom, q_iatom_val, ...) live here too (q_registry_internal.h). */
#include "qlang/q_dollar.h"
#include "qlang/q_scan.h"   /* the Tok string scanners (q_date_scan, q_ts_scan, ...) */
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "lang/eval.h"      /* ray_cast_fn */
#include "lang/internal.h"  /* ray_typed_null, ray_guid, ray_str_vec_get, ray_error */
#include "core/numparse.h"  /* ray_parse_i64/f64 — Tok string parses */
#include <math.h>           /* isnan */
#include <stdint.h>         /* INT16/32/64 MIN/MAX — Tok out-of-domain bounds */
#include <string.h>
#include <stdlib.h>

/* Designator resolution is separate from conversion so C callers (future
 * bool-widening / promotion work) can invoke q_dollar_cast(tag, x) directly. */

/* Is vector tag `t` legal as a ROW INDEX (uniform-structure-dispatch §2.2:
 * any int-backed index indexes)?  Returns the element width in bytes so
 * consumers key on WIDTH, never enumerate tags: 8/4/2 read signed, 1 reads
 * unsigned (U8); 0 = not an int index.  SYM is i64-backed but EXCLUDED —
 * a sym index means column access, never a row.  BOOL is excluded pending
 * a doc citation for boolean indexing.  Atom callers pass -atom->type;
 * every accepted ATOM stores its payload in .i64 (vec/atom.c), so as_i64's
 * fallback reads it correctly. */
int q_int_index_width(int8_t t) {
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
 * call site.  Accepted set = the q_int_index_width law: I64/I32/I16/U8 +
 * int-backed temporal ATOMS; never sym/bool/float/structures.
 * Returns 1 + *out, or 0 on refusal. */
int q_strict_i64(ray_t* x, int64_t* out) {
    if (!x || x->type >= 0) return 0;
    switch (q_int_index_width((int8_t)-x->type)) {
    case 8: *out = x->i64;          return 1;
    case 4: *out = (int64_t)x->i32; return 1;
    case 2: *out = (int64_t)x->i16; return 1;
    case 1: *out = (int64_t)x->u8;  return 1;
    default: return 0;
    }
}

/* Float twin: F64/F32/DATETIME (f64-slot payloads) + the q_strict_i64 set. */
int q_strict_f64(ray_t* x, double* out) {
    if (!x || x->type >= 0) return 0;
    if (x->type == -RAY_F64 || RAY_IS_TEMPORALF(-x->type)) { *out = x->f64; return 1; }
    if (x->type == -RAY_F32) { *out = (double)(float)x->f64; return 1; }
    int64_t v;
    if (!q_strict_i64(x, &v)) return 0;
    *out = (double)v;
    return 1;
}

/* Throwing gates for TERMINAL sites (failure = error): NULL on success, else
 * an owned 'type error carrying `what` — short site context, "verb: role".
 * The silent probe form above is for dispatch sites (failure = next arm). */
ray_t* q_i64_or_err(ray_t* x, int64_t* out, const char* what) {
    return q_strict_i64(x, out) ? NULL : ray_error("type", what);
}
ray_t* q_f64_or_err(ray_t* x, double* out, const char* what) {
    return q_strict_f64(x, out) ? NULL : ray_error("type", what);
}

/* Type-facts helpers (I64/I32/I16 only — vs/sv base-encode domain). */
int q_is_int_atom(ray_t* x) {
    return x && (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16);
}
int q_is_int_vec(ray_t* x) {
    return x && (x->type == RAY_I64 || x->type == RAY_I32 || x->type == RAY_I16);
}
int64_t q_ivec_get(ray_t* v, int64_t i) {
    const void* d = ray_data(v);
    return v->type == RAY_I64 ? ((const int64_t*)d)[i]
         : v->type == RAY_I32 ? (int64_t)((const int32_t*)d)[i]
                              : (int64_t)((const int16_t*)d)[i];
}
int64_t q_iatom_val(ray_t* x) {
    return x->type == -RAY_I64 ? x->i64
         : x->type == -RAY_I32 ? (int64_t)x->i32 : (int64_t)x->i16;
}

int8_t q_cast_designator(ray_t* t, int* is_tok) {
    *is_tok = 0;
    if (!t) return 0;
    if (t->type == -RAY_I16) {          /* kdb type number == rayfall tag */
        if (RAY_ATOM_IS_NULL(t)) return 0;
        int16_t n = t->i16;
        if (n <= 0) { *is_tok = 1; n = (int16_t)-n; }
        /* Numeric designator == rayfall tag.  No `default:` — exhaustive over
         * the value band (#209): a new datatype must name its designator here.
         * An out-of-band n (98h table, sparse gap 3) falls to the trailing
         * `return 0` = "not a designator", exactly as the old default did. */
        switch ((ray_type_e)n) {
        case RAY_BOOL: case RAY_BYTE_ONLY: case RAY_I16: case RAY_I32:
        case RAY_I64:  case RAY_F32: case RAY_F64: case RAY_SYM: case RAY_CHARV:
        case RAY_GUID:      /* cast.md:20 `2h "g" `guid` are one designator row;
                             * `-2h$"uuid"` = guid Tok (q_dollar_tok parses it). The
                             * `2h$` CAST stays deferred at q_dollar_cast. */
        RAY_TEMPORAL32_CASES: RAY_TEMPORAL64_CASES: RAY_TEMPORALF_CASES:
            return (int8_t)n;
        case RAY_LIST:      /* 0h is Identity (cast.md:40), not a cast tag — deferred */
            return 0;
        case RAY_STR:       /* 21h: physical storage tag, never a q designator */
            return 0;
        }
        return 0;   /* unreachable for in-band n; out-of-band handled here */
    }
    if ((t->type == -RAY_STR && ray_str_len(t) == 1) ||
        t->type == -RAY_CHARV ||
        (t->type == RAY_CHARV && ray_len(t) == 1)) {
        char c = t->type == -RAY_CHARV ? (char)t->u8
               : t->type == RAY_CHARV  ? ((const char*)ray_data(t))[0]
                                       : ray_str_ptr(t)[0];
        if (c >= 'A' && c <= 'Z') { *is_tok = 1; c = (char)(c - 'A' + 'a'); }
        switch (c) {
        case 'b': return RAY_BOOL; case 'x': return RAY_BYTE_ONLY;
        case 'h': return RAY_I16;  case 'i': return RAY_I32;
        case 'j': return RAY_I64;  case 'e': return RAY_F32;
        case 'f': return RAY_F64;  case 's': return RAY_SYM;
        case 'd': return RAY_DATE; case 'g': return RAY_GUID;
        case 't': return RAY_TIME; case 'p': return RAY_TIMESTAMP;
        case 'm': return RAY_MONTH;
        case 'u': return RAY_MINUTE;
        case 'v': return RAY_SECOND;
        case 'n': return RAY_TIMESPAN;
        case 'z': return RAY_DATETIME;
        case 'c': return RAY_CHARV; /* char cast: "c"$x reinterprets as chars */
        default:  return 0;       /* "*" identity: deferred */
        }
    }
    if (t->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(t->i64);
        if (!s) return 0;
        const char* nm = ray_str_ptr(s);
        size_t l = ray_str_len(s);
        int8_t r = 0;
        if      (l == 0)                              { *is_tok = 1; r = RAY_SYM; }
        else if (l == 4 && !memcmp(nm, "char",    4)) r = RAY_CHARV;
        else if (l == 4 && !memcmp(nm, "long",    4)) r = RAY_I64;
        else if (l == 5 && !memcmp(nm, "float",   5)) r = RAY_F64;
        else if (l == 3 && !memcmp(nm, "int",     3)) r = RAY_I32;
        else if (l == 5 && !memcmp(nm, "short",   5)) r = RAY_I16;
        else if (l == 7 && !memcmp(nm, "boolean", 7)) r = RAY_BOOL;
        else if (l == 4 && !memcmp(nm, "byte",    4)) r = RAY_BYTE_ONLY;
        else if (l == 4 && !memcmp(nm, "real",    4)) r = RAY_F32;
        else if (l == 6 && !memcmp(nm, "symbol",  6)) r = RAY_SYM;
        else if (l == 4 && !memcmp(nm, "date",    4)) r = RAY_DATE;
        else if (l == 5 && !memcmp(nm, "month",   5)) r = RAY_MONTH;
        else if (l == 6 && !memcmp(nm, "minute",  6)) r = RAY_MINUTE;
        else if (l == 6 && !memcmp(nm, "second",  6)) r = RAY_SECOND;
        else if (l == 8 && !memcmp(nm, "timespan",8)) r = RAY_TIMESPAN;
        else if (l == 4 && !memcmp(nm, "time",    4)) r = RAY_TIME;
        else if (l == 9 && !memcmp(nm, "timestamp", 9)) r = RAY_TIMESTAMP;
        else if (l == 8 && !memcmp(nm, "datetime", 8)) r = RAY_DATETIME;
        ray_release(s);
        return r;
    }
    return 0;
}

/* RAY vector type -> q type-name (ref/key.md "type of a vector"). Mirrors the
 * cast-designator name map above for every castable tag; `guid` is display-only
 * (no `$`guid designator, so it has no entry there). No `default:` — total over
 * the value band (#209): a new datatype refuses to build until it names its
 * q-spelling here, the single home for empty-vec display (q_fmt) + `meta`/`key`
 * type rows. LIST/STR have no scalar vector-type name (a STR vector is a list of
 * strings in the provisional model); the -RAY_STR atom shim names `char` at the
 * call sites (q_fmt_pipe/q_key_wrap). */
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
static const char* q_tag_rayname(int8_t tag) {
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

/* tag -> base `as` spelling, then delegate; 'nyi when the tag has no spelling
 * (LIST/GUID/F32 targets). */
static ray_t* q_cast_delegate(int8_t tag, ray_t* x) {
    const char* nm = q_tag_rayname(tag);
    if (!nm) return ray_error("nyi", "$: unsupported cast designator (deferred)");
    ray_t* ts = ray_sym(ray_sym_intern(nm, strlen(nm)));
    if (!ts || RAY_IS_ERR(ts)) return ts;
    ray_t* r = ray_cast_fn(ts, x);
    ray_release(ts);
    return r;
}

/* `$`-to-boolean: "only 0 is false" (owner ruling 2026-07-15; test/q/cast/
 * boolean.qcmd:2 cites it).  A null's payload is a NONZERO sentinel (INT_MIN,
 * NaN) so a null is TRUE — base's atom path null-propagates to 0b
 * (builtins.c:1334), contradicting its OWN vector arm `_v != 0`
 * (builtins.c:1004): "b"$0N -> 0b vs "b"$enlist 0N -> ,1b.  Intercepted here
 * because builtins.c is frozen.  Sources ride the #187 strict-cast home, not a
 * new ladder: q_strict_i64 = ints + int-backed temporals, q_strict_f64 adds
 * F64/F32/DATETIME. */
static ray_t* q_cast_bool(ray_t* x) {
    if (!x) return ray_error("type", "$: boolean");
    if (x->type == -RAY_BOOL || x->type == RAY_BOOL) { ray_retain(x); return x; }
    int64_t i;
    if (q_strict_i64(x, &i)) return ray_bool(i != 0);
    double d;
    if (q_strict_f64(x, &d)) return ray_bool(d != 0.0);   /* 0n is NaN; NaN != 0 -> 1b */
    /* string atom: emptiness, not a numeric law — the provisional string model
     * (ARCHITECTURE.md) owns it; preserved verbatim from base's arm. */
    if (x->type == -RAY_STR) return ray_bool(ray_str_len(x) > 0);
    if (x->type == RAY_CHARV) return ray_bool(ray_len(x) > 0);  /* same emptiness law */
    if (x->type == -RAY_CHARV) return ray_bool(1);              /* a char is non-empty */
    /* An atom reaching here (guid, sym) has no boolean law.  Atoms never
     * delegate: that re-enters the null-propagation being intercepted, which is
     * how 0Ng returned 0b while a non-null guid errored. */
    if (x->type < 0) return ray_error("type", "$: boolean");
    /* base cast_vec_numeric has no F32 source arm for ANY target, so the q
     * layer supplies it — exactly as the integer targets already do. */
    if (x->type == RAY_F32) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_BOOL, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        const float* f = (const float*)ray_data(x);
        for (int64_t k = 0; k < n; k++) ((uint8_t*)ray_data(out))[k] = (f[k] != 0.0f);
        return out;
    }
    return q_cast_delegate(RAY_BOOL, x);   /* other vectors: base's arm IS this law */
}

/* char cast (`10h$`/`` `char$``/`"c"$`): reinterpret an integer/byte value as
 * chars, producing a native string (openq has no char-atom type distinct from
 * a 1-char string).  Runs BEFORE the generic RAY_LIST distribution so a boxed
 * integer list packs into ONE string rather than a list of 1-char strings
 * (q_collapse_list refuses to pack -RAY_STR atoms). */
static ray_t* q_cast_str(ray_t* x) {
    if (x && x->type == -RAY_STR) { ray_retain(x); return x; }   /* identity */
    if (x && (x->type == RAY_CHARV || x->type == -RAY_CHARV)) {  /* charv identity */
        ray_retain(x); return x;
    }
    if (x && x->type == RAY_BYTE_ONLY)                                  /* byte vec */
        return ray_str((const char*)ray_data(x), (size_t)ray_len(x));
    if (x && x->type == -RAY_BYTE_ONLY) return ray_char(x->u8);  /* byte atom -> char atom */
    if (q_is_int_atom(x)) {
        return ray_char((uint8_t)q_iatom_val(x));   /* `char$65 -> "A" (atom) */
    }
    if (q_is_int_vec(x)) {
        int64_t n = ray_len(x);
        char* buf = (char*)malloc(n ? (size_t)n : 1);
        if (!buf) return ray_error("wsfull", "$: char cast");
        for (int64_t i = 0; i < n; i++)
            buf[i] = (char)q_ivec_get(x, i);
        ray_t* r = ray_str(buf, (size_t)n);
        free(buf);
        return r;
    }
    if (x && x->type == RAY_LIST) {          /* boxed list of int/byte -> string */
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        char* buf = (char*)malloc(n ? (size_t)n : 1);
        if (!buf) return ray_error("wsfull", "$: out of memory");
        for (int64_t i = 0; i < n; i++) {
            ray_t* ei = e[i];
            int64_t v;
            if      (ei && ei->type == -RAY_I64) v = ei->i64;
            else if (ei && ei->type == -RAY_I32) v = ei->i32;
            else if (ei && ei->type == -RAY_I16) v = ei->i16;
            else if (ei && ei->type == -RAY_BYTE_ONLY)  v = ei->u8;
            else if (ei && ei->type == -RAY_STR && ray_str_len(ei) == 1)
                v = (unsigned char)ray_str_ptr(ei)[0];
            else if (ei && ei->type == -RAY_CHARV) v = ei->u8;
            else { free(buf); return ray_error("type", "$: cannot cast list element to char"); }
            buf[i] = (char)v;
        }
        ray_t* r = ray_str(buf, (size_t)n);
        free(buf);
        return r;
    }
    return ray_error("type", "$: cannot cast to char");
}

/* Boxed list: cast per element, then collapse a homogeneous run to a vector. */
static ray_t* q_cast_distribute(int8_t tag, ray_t* x) {
    int64_t n = ray_len(x);
    ray_t** e = (ray_t**)ray_data(x);
    ray_t* out = ray_list_new(n);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        ray_t* r = q_dollar_cast(tag, e[i]);
        if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
        out = ray_list_append(out, r);   /* append retains */
        ray_release(r);
    }
    ray_t* c = q_collapse_list(out);
    ray_release(out);
    return c;
}

/* Integer targets (I64/I32/I16): kdb ROUNDS floats (rint = IEEE nearest/
 * ties-even: `long$3.7 -> 4, "j"$2.5 -> 2, `int$6.6 -> 7 — KX ref pins) where
 * rayfall `as` truncates, so pre-round here; the rest is base's. */
/* Real (float32) target.  Base rayfall has an `as float` (F64) arm but no
 * `real`/F32 one, so `"e"$` used to 'nyi. Reuse the base F64 cast (handles every
 * numeric input + string parse, exactly like `"f"$`), then narrow F64 -> F32. */
static ray_t* q_cast_real(ray_t* x) {
    ray_t* f = q_cast_delegate(RAY_F64, x);
    if (RAY_IS_ERR(f)) return f;
    if (f->type == -RAY_F64) {                              /* atom */
        ray_t* r = RAY_ATOM_IS_NULL(f) ? ray_typed_null(-RAY_F32)
                                       : ray_f32((float)f->f64);
        ray_release(f);
        return r;
    }
    if (f->type == RAY_F64) {                               /* vector */
        int64_t n = ray_len(f);
        ray_t* out = ray_vec_new(RAY_F32, n);
        if (RAY_IS_ERR(out)) { ray_release(f); return out; }
        out->len = n;
        const double* src = (const double*)ray_data(f);
        for (int64_t i = 0; i < n; i++) {
            double v = src[i];
            ((float*)ray_data(out))[i] = (float)v;
            if (isnan(v)) ray_vec_set_null(out, i, true);
        }
        ray_release(f);
        return out;
    }
    return f;
}

static ray_t* q_cast_int(int8_t tag, ray_t* x) {
    if (x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null((int8_t)-tag);
        double r = rint(x->f64);              /* F32 atoms store f64 payload */
        if (tag == RAY_I64) return ray_i64((int64_t)r);
        if (tag == RAY_I32) return ray_i32((int32_t)r);
        return ray_i16((int16_t)r);
    }
    if (x && (x->type == RAY_F64 || x->type == RAY_F32)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(tag, n);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        int is64 = (x->type == RAY_F64);
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(x))[i]
                            : (double)((const float*)ray_data(x))[i];
            int isnull = isnan(v);
            int64_t iv = isnull ? 0 : (int64_t)rint(v);
            if      (tag == RAY_I64) ((int64_t*)ray_data(out))[i] = iv;
            else if (tag == RAY_I32) ((int32_t*)ray_data(out))[i] = (int32_t)iv;
            else                     ((int16_t*)ray_data(out))[i] = (int16_t)iv;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    return q_cast_delegate(tag, x);
}

/* Byte target.  kdb `"x"$str` maps CHARS to bytes ("x"$"abc" -> 0x616263,
 * ref/cast.md #byte); base's U8 STR arm parses decimal text instead — pre-empt
 * it.  One char = char atom -> byte ATOM; else a byte vector of the raw chars
 * (empty string -> empty byte vector).  Byte joins the integer family for
 * float rounding (derived — byte float-cast is unpinned); float null -> 0x00:
 * byte has no null (basics/datatypes.md blank column). */
static ray_t* q_cast_u8(ray_t* x) {
    if (x && x->type == -RAY_STR) {
        const char* sp = ray_str_ptr(x);
        size_t sl = ray_str_len(x);
        if (sl == 1) return ray_u8((uint8_t)sp[0]);
        return ray_vec_from_raw(RAY_BYTE_ONLY, sp, (int64_t)sl);
    }
    if (x && x->type == -RAY_CHARV) return ray_u8(x->u8);
    if (x && x->type == RAY_CHARV)
        return ray_vec_from_raw(RAY_BYTE_ONLY, ray_data(x), ray_len(x));
    if (x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_u8(0);
        return ray_u8((uint8_t)(int64_t)rint(x->f64));  /* F32 stores f64 */
    }
    if (x && (x->type == RAY_F64 || x->type == RAY_F32)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_BYTE_ONLY, n);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        int is64 = (x->type == RAY_F64);
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(x))[i]
                            : (double)((const float*)ray_data(x))[i];
            ((uint8_t*)ray_data(out))[i] = isnan(v) ? 0 : (uint8_t)(int64_t)rint(v);
        }
        return out;
    }
    return q_cast_delegate(RAY_BYTE_ONLY, x);
}

/* Timestamp target.  `timestamp$date: days -> ns, SATURATING outside the
 * timestamp year range (`timestamp$1666.09.02 -> -0Wp, datatypes.md:149) —
 * base's arm multiplies unchecked (i64 overflow, UBSan, builtins.c:1616) — and
 * mapping the date sentinels to the i64 sentinels (0Nd -> 0Np, +-0Wd -> +-0Wp,
 * which the saturation clamp yields for free). */
static ray_t* q_cast_timestamp(ray_t* x) {
    if (x && x->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(q_ts_compose((int64_t)x->i32, 0));
    }
    if (x && x->type == RAY_DATE) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_TIMESTAMP, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        const int32_t* d = (const int32_t*)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            int isnull = (d[i] == INT32_MIN);
            ((int64_t*)ray_data(out))[i] =
                isnull ? 0 : q_ts_compose((int64_t)d[i], 0);
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* kdb `timestamp$time keeps the TIME OF DAY (ms -> ns on day 0, derived:
     * time is ms-of-day, timestamp ns; base's same-width path relabels the
     * raw ms payload as ns — a wrong answer, caught by the designator audit).
     * Sentinels map across (0Nt -> 0Np, +-0Wt -> +-0Wp). */
    if (x && x->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_TIMESTAMP);
        if (x->i32 == INT32_MAX)  return ray_timestamp(INT64_MAX);
        if (x->i32 == -INT32_MAX) return ray_timestamp(-INT64_MAX);
        return ray_timestamp((int64_t)x->i32 * 1000000LL);
    }
    if (x && x->type == RAY_TIME) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_TIMESTAMP, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        const int32_t* d = (const int32_t*)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            int isnull = (d[i] == INT32_MIN);
            ((int64_t*)ray_data(out))[i] =
                isnull ? 0
                : (d[i] == INT32_MAX)  ? INT64_MAX
                : (d[i] == -INT32_MAX) ? -INT64_MAX
                : (int64_t)d[i] * 1000000LL;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* float -> timestamp: base truncates the float to a raw ns count, but
     * kdb's unit semantics here (rint-ns vs datetime-style fractional DAYS)
     * is unpinned in the docs corpus — error beats a wrong answer, so the
     * shape is a deferred cell (designator-audit decision, plan 2026-07-07). */
    if (x && (x->type == -RAY_F64 || x->type == -RAY_F32 ||
              x->type == RAY_F64  || x->type == RAY_F32))
        return ray_error("nyi", "$: float->timestamp cast is deferred");
    return q_cast_delegate(RAY_TIMESTAMP, x);
}

/* Symbol target: `symbol$sym is identity; every other source is deferred. */
static ray_t* q_cast_sym(ray_t* x) {
    if (x && (x->type == -RAY_SYM || x->type == RAY_SYM)) {
        ray_retain(x);
        return x;
    }
    return ray_error("nyi", "$: cast to symbol is deferred (use `$ / \"S\"$ on strings)");
}

/* The ONE cast home (contract: q_dollar.h).  Dispatch is on the TARGET tag:
 * every target gets an arm naming the helper that owns it (the SOURCE types
 * live in that helper), or delegates to base ray_cast_fn where kdb and rayfall
 * already agree.  The switch has NO `default:`, so -Wall (=> -Wswitch) +
 * -Werror refuse to build a target no arm states. */
ray_t* q_dollar_cast(int8_t tag, ray_t* x) {
    /* Both precede the switch by ORDER, not preference: "c"$ packs a boxed list
     * into ONE string, so it must beat the generic per-element distribution. */
    if (tag == RAY_CHARV) return q_charv_out(q_cast_str(x));
    /* numeric cast of char text = code points (`int$"ABC" -> 65 66 67i;
     * `float$"AC" -> 65 67f, ref/log.md:101) — via the byte cast, then cast. */
    if (x && (x->type == RAY_CHARV || x->type == -RAY_CHARV) &&
        (tag == RAY_I16 || tag == RAY_I32 || tag == RAY_I64 ||
         tag == RAY_F32 || tag == RAY_F64)) {
        ray_t* b = q_cast_u8(x);
        if (!b || RAY_IS_ERR(b)) return b;
        ray_t* r = q_dollar_cast(tag, b);
        ray_release(b);
        return r;
    }
    if (x && x->type == RAY_LIST) return q_cast_distribute(tag, x);

    switch ((ray_type_e)tag) {
    case RAY_CHARV: break;                   /* hoisted above: packs boxed lists */
    case RAY_LIST: break;                    /* tag 0 is not a cast designator */
    case RAY_GUID: break;                    /* guid target: no base arm — deferred */
    case RAY_STR:  break;                    /* physical tag: never a cast target */
    case RAY_F32:  return q_cast_real(x);    /* real: narrow base F64 cast to F32 */
    case RAY_BOOL: return q_cast_bool(x);
    case RAY_BYTE_ONLY: return q_cast_u8(x);
    case RAY_I16: case RAY_I32: case RAY_I64:
        return q_cast_int(tag, x);
    case RAY_TIMESTAMP: return q_cast_timestamp(x);
    case RAY_SYM:  return q_cast_sym(x);
    case RAY_F64: case RAY_MONTH: case RAY_DATE: case RAY_DATETIME:
    case RAY_TIMESPAN: case RAY_MINUTE: case RAY_SECOND: case RAY_TIME:
        return q_cast_delegate(tag, x);
    }
    /* the `break` arms above + any out-of-band tag (the band is sparse: 3 is
     * kdb's short-of-3).  q_cast_delegate has no spelling for them -> 'nyi,
     * which is what each returned before — no bespoke error strings needed. */
    return q_cast_delegate(tag, x);
}

/* kdb Tok (ref/tok.md): parse a string as a value of the tag type.  Leading/
 * trailing blanks are trimmed; unparseable or out-of-range -> typed null.
 * Implicit recursion stops at STRINGS, not atoms: lists / string vectors
 * distribute. */
ray_t* q_dollar_tok(int8_t tag, ray_t* x) {
    if (x && (x->type == RAY_LIST || x->type == RAY_STR)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* xi;
            if (x->type == RAY_LIST) {
                xi = ((ray_t**)ray_data(x))[i];
                ray_retain(xi);
            } else {
                size_t sl = 0;
                const char* sp = ray_str_vec_get(x, i, &sl);
                xi = ray_str(sp ? sp : "", sp ? sl : 0);
            }
            ray_t* r = q_dollar_tok(tag, xi);
            ray_release(xi);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    const char* tp; int64_t tn;
    if (!q_text_bytes(x, &tp, &tn))
        return ray_error("type", "$: Tok right operand must be a string");
    const char* p = tp;
    size_t len = p ? (size_t)tn : 0;
    while (len && *p == ' ') { p++; len--; }            /* trim outer blanks */
    while (len && p[len - 1] == ' ') len--;
    switch (tag) {
    case RAY_SYM:
        return ray_sym(ray_sym_intern(len ? p : "", len));
    case RAY_BOOL:   /* truthy set pinned by ref/tok.md: "txyTXY1" */
        return ray_bool(len == 1 && strchr("1TtXxYy", p[0]) != NULL);
    case RAY_F64: case RAY_F32: {
        double v = 0;
        size_t used = len ? ray_parse_f64(p, len, &v) : 0;
        if (used != len || len == 0) return ray_typed_null((int8_t)-tag);
        return tag == RAY_F64 ? ray_f64(v) : ray_f32((float)v);
    }
    case RAY_I64: case RAY_I32: case RAY_I16: {
        int64_t v = 0;
        size_t used = len ? ray_parse_i64(p, len, &v) : 0;
        if (used != len || len == 0) return ray_typed_null((int8_t)-tag);
        /* Out-of-domain -> typed null (tok.md).  The bounds are ±INT*_MAX,
         * NOT INT*_MIN: the exact minimum IS the null sentinel (0N/0Ni/0Nh)
         * and must never round-trip as an accepted value. */
        if (tag == RAY_I64)
            return (v == INT64_MIN)
                 ? ray_typed_null(-RAY_I64) : ray_i64(v);
        if (tag == RAY_I32)
            return (v > INT32_MAX || v < -INT32_MAX)
                 ? ray_typed_null(-RAY_I32) : ray_i32((int32_t)v);
        return (v > INT16_MAX || v < -INT16_MAX)
             ? ray_typed_null(-RAY_I16) : ray_i16((int16_t)v);
    }
    case RAY_DATE: {
        /* Unparseable / invalid civil date / out-of-domain -> 0Nd, never an
         * error (tok.md pins "D"$"2147483648" -> 0Nd). */
        int64_t y, mo, d;
        if (!q_date_scan(p, len, &y, &mo, &d) || !q_date_valid(y, mo, d))
            return ray_typed_null(-RAY_DATE);
        return ray_date(q_days_from_civil(y, mo, d));
    }
    case RAY_MONTH: {
        /* "M"$str -> month (ref/tok.md).  Unparseable / out-of-domain -> 0Nm,
         * never an error (tok contract, mirrors "D"$). */
        int64_t mo;
        if (!q_month_scan(p, len, &mo))
            return ray_typed_null(-RAY_MONTH);
        return ray_month(mo);
    }
    case RAY_TIME: {
        /* "T"$str -> time (ref/tok.md).  Unparseable / out-of-domain -> 0Nt,
         * never an error (base ray_cast_fn errors on a bad string). */
        int32_t ms;
        if (!q_time_scan(p, len, &ms))
            return ray_typed_null(-RAY_TIME);
        return ray_time(ms);
    }
    case RAY_TIMESTAMP: {
        /* "P"$str -> timestamp (ref/tok.md Â§Timestamps).  Unparseable /
         * out-of-range -> 0Np, never an error (tok contract). */
        int64_t ns;
        if (!q_ts_scan(p, len, &ns))
            return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(ns);
    }
    case RAY_DATETIME: {
        /* "Z"$str -> datetime.  tok.md:222-227 pins "PZ"$\: over ONE input
         * ("20191122-11:11:11.123" -> 2019.11.22T11:11:11.123): Z shares P's
         * accepted shapes at ms display precision, so reuse q_ts_scan (the
         * single P parser) and convert ns -> fractional days.  Unparseable /
         * invalid -> 0Nz, never an error (tok contract). */
        int64_t ns;
        if (!q_ts_scan(p, len, &ns))
            return ray_typed_null(-RAY_DATETIME);
        return ray_datetime((double)ns / 86400000000000.0);
    }
    case RAY_BYTE_ONLY: {
        /* "X"$ reads the string as HEX ("X"$"42" -> 0x42, ref/tok.md).
         * Unparseable or > 0xff -> 0x00 (derived): tok.md pins out-of-
         * domain -> typed null, and byte HAS no null (basics/datatypes.md),
         * so its zero value stands in. */
        uint64_t v = 0;
        size_t i = 0;
        for (; i < len; i++) {
            char c = p[i];
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) break;
            v = (v << 4) | (uint64_t)d;
            if (v > 0xff) break;
        }
        if (len == 0 || i != len || v > 0xff) return ray_u8(0);
        return ray_u8((uint8_t)v);
    }
    case RAY_GUID: {
        /* "G"$str -> guid (basics/datatypes.md §Guid).  Tok contract:
         * unparseable / wrong-shape -> typed null 0Ng, never an error (base
         * ray_cast_fn "GUID" ERRORS on bad input, so parse here).  Canonical
         * 36-char UUID only; IPv4/IPv6 forms deferred (see q_parse_uuid). */
        uint8_t bytes[16];
        if (!q_parse_uuid(p, len, bytes)) return ray_typed_null(-RAY_GUID);
        return ray_guid(bytes);
    }
    case RAY_MINUTE: {
        /* "U"$str -> minute, FLOOR to the minute we are in (ref/tok.md:61
         * "U"$"12:13:14" -> 12:13; cast.md:168-170 truncation rule). */
        int64_t ns;
        if (!q_clock_scan_ns(p, len, &ns))
            return ray_typed_null(-RAY_MINUTE);
        return ray_minute(ns / 60000000000LL);
    }
    case RAY_SECOND: {
        /* "V"$str -> second, floor (derived — mirrors "U"$). */
        int64_t ns;
        if (!q_clock_scan_ns(p, len, &ns))
            return ray_typed_null(-RAY_SECOND);
        return ray_second(ns / 1000000000LL);
    }
    case RAY_TIMESPAN: {
        /* "N"$str -> timespan; grammar in q_timespan_scan_ns.  Unparseable /
         * out-of-range -> 0Nn (tok contract). */
        int64_t ns;
        if (!q_timespan_scan_ns(p, len, &ns))
            return ray_typed_null(-RAY_TIMESPAN);
        return ray_timespan(ns);
    }
    default:
        return ray_error("nyi", "$: char Tok is deferred");
    }
}

/* q `w$s` PAD (ref/pad.md): a LONG width w left-justifies the string s in a
 * field of |w| spaces (w<0 right-justifies); longer strings truncate to |w|.
 * Atomic through the container types (a LIST of strings pads each; DICT over
 * values; TABLE over columns).  Non-string leaves are a 'type error. */
ray_t* q_dollar_pad(int64_t w, ray_t* x) {
    if (!x) return ray_error("type", "$: pad nil");
    if (x->type == RAY_CHARV || x->type == -RAY_CHARV) {   /* char text -> charv */
        const char* p; int64_t pn;
        (void)q_text_bytes(x, &p, &pn);
        int64_t width = w < 0 ? -w : w;
        int right = w < 0;
        int64_t copy = pn < width ? pn : width;
        char stack[256];
        char* b = (width < (int64_t)sizeof stack) ? stack : malloc((size_t)width + 1);
        if (!b) return ray_error("wsfull", "$: out of memory");
        memset(b, ' ', (size_t)width);
        if (right) memcpy(b + (width - copy), p, (size_t)copy);
        else       memcpy(b, p, (size_t)copy);
        ray_t* r = ray_charv(b, width);
        if (b != stack) free(b);
        return r;
    }
    if (x->type == -RAY_STR) {
        int64_t width = w < 0 ? -w : w;
        int right = w < 0;                 /* w<0 -> right-justify */
        const char* p = ray_str_ptr(x);
        int64_t n = (int64_t)ray_str_len(x);
        int64_t copy = n < width ? n : width;
        char stack[256];
        char* b = (width < (int64_t)sizeof stack) ? stack : malloc((size_t)width + 1);
        if (!b) return ray_error("wsfull", "$: out of memory");
        memset(b, ' ', (size_t)width);
        if (right) memcpy(b + (width - copy), p, (size_t)copy);   /* text at right */
        else       memcpy(b, p, (size_t)copy);                    /* text at left  */
        ray_t* r = ray_str(b, (size_t)width);
        if (b != stack) free(b);
        return r;
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* e = ray_at_fn(x, ia);
            ray_release(ia);
            if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
            ray_t* r = q_dollar_pad(w, e);
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);
        ray_t* v = ray_dict_vals(x);
        if (!k || !v) return ray_error("type", "$: bad dict");
        ray_t* nv = q_dollar_pad(w, v);
        if (!nv || RAY_IS_ERR(nv)) return nv;
        ray_retain(k);
        return ray_dict_new(k, nv);
    }
    if (x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* out = ray_table_new(nc);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(x, c);
            ray_t* ncol = q_dollar_pad(w, col);
            if (!ncol || RAY_IS_ERR(ncol)) { ray_release(out); return ncol; }
            out = ray_table_add_col(out, ray_table_col_name(x, c), ncol);
            ray_release(ncol);
            if (!out || RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_STR) {            /* string vector -> pad each element */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            size_t sn; const char* p = ray_str_vec_get(x, i, &sn);
            ray_t* s = ray_str(p ? p : "", p ? sn : 0);
            ray_t* r = q_dollar_pad(w, s);
            ray_release(s);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return ray_error("type", "$: pad expects a string");
}

/* Enumerate `x$y` (ref/enumerate.md: sym lhs naming a domain list) — openq has
 * no enum domains, so the whole form is a 'nyi stub awaiting them. */
ray_t* q_dollar_enum(ray_t* x, ray_t* y) {
    (void)x; (void)y;
    return ray_error("nyi", "$: enumerate (no enum domains)");
}

/* `$` as matrix multiply / dot product (ref/mmu.md: `$` is mmu's glyph form).
 * Same home as the `mmu` keyword; q_dollar dispatches here only when BOTH
 * operands mmu-classify, so every non-mmu shape keeps its cast-path behavior. */
ray_t* q_dollar_mmu(ray_t* x, ray_t* y) {
    return q_mmu_wrap(x, y);
}

/* q `t$x` — the `$` verb (contract: q_dollar.h).  LEFT-operand dispatch:
 * a LONG width is PAD; mmu-shaped float operands (both sides) are matrix
 * multiply; a multi-designator LHS ("fiij", `int`float, 5 6h, (`int;"i";6h))
 * zips elementwise over x (ref/cast.md pins (`int;"i";6h)$10 -> 10 10 10i: an
 * ATOM rhs is broadcast); a single designator resolves via q_cast_designator
 * into q_dollar_cast / q_dollar_tok; a non-designator sym is Enumerate. */
ray_t* q_dollar(ray_t* t, ray_t* x) {
    if (t && t->type == -RAY_I64) return q_dollar_pad(t->i64, x);
    int64_t k;
    if (q_mmu_class(t, &k) != QMMU_BAD && q_mmu_class(x, &k) != QMMU_BAD)
        return q_dollar_mmu(t, x);   /* ragged included: mmu owns its 'length */
    int multi = t && ((t->type == -RAY_STR && ray_str_len(t) > 1) ||
                      (t->type == RAY_CHARV && ray_len(t) > 1) ||
                      t->type == RAY_SYM || t->type == RAY_I16 ||
                      t->type == RAY_LIST);
    if (multi) {
        int64_t n = (t->type == -RAY_STR) ? (int64_t)ray_str_len(t) : ray_len(t);
        int x_is_list = x && (ray_is_vec(x) || x->type == RAY_LIST);
        if (x_is_list && ray_len(x) != n)
            return ray_error("length", "$: designator/operand length mismatch");
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ti;
            if (t->type == -RAY_STR) ti = ray_str(ray_str_ptr(t) + i, 1);
            else if (t->type == RAY_CHARV) ti = ray_char(((const uint8_t*)ray_data(t))[i]);
            else {
                ray_t* idx = ray_i64(i);
                ti = ray_at_fn(t, idx);         /* sym/short vec, list */
                ray_release(idx);
            }
            if (!ti || RAY_IS_ERR(ti)) { ray_release(out); return ti; }
            ray_t* xi;
            if (x_is_list) {
                ray_t* idx = ray_i64(i);
                xi = ray_at_fn(x, idx);
                ray_release(idx);
            } else { xi = x; ray_retain(xi); }  /* atom rhs broadcasts */
            if (!xi || RAY_IS_ERR(xi)) { ray_release(ti); ray_release(out); return xi; }
            ray_t* r = q_dollar(ti, xi);
            ray_release(ti);
            ray_release(xi);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    int is_tok = 0;
    int8_t tag = q_cast_designator(t, &is_tok);
    if (!tag) {
        if (t && t->type == -RAY_SYM) return q_dollar_enum(t, x);
        return ray_error("nyi", "$: unsupported cast designator (deferred)");
    }
    /* `10h$`/`` `char$``/`"c"$` all land here with is_tok=0 and reinterpret via
     * q_dollar_cast; only the UPPERCASE char token `"C"$` carries is_tok=1 and stays
     * a deferred char-Tok — q_dollar_tok's default errors 'nyi (pinned by the
     * cast_tok_deferred unit test), so no RAY_STR special-case is needed here. */
    return is_tok ? q_dollar_tok(tag, x) : q_dollar_cast(tag, x);
}
