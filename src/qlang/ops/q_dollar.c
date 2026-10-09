/* q_dollar — the single C home for the `$` verb (contract: q_dollar.h).
 * Value semantics: values -> values; the one runtime read is the live `\W` day for `week$.  q_dollar is
 * the generic registry row; q_dollar_pad / q_dollar_cast / q_dollar_tok /
 * q_dollar_enum / q_dollar_mmu are the per-operation homes, exposed with types
 * for reuse.  The per-target q_cast_* matrix lives here too; the int-atom
 * admission helpers and tag<->name vocabulary moved to q_type.c (q_type.h). */
#include "qlang/q_count.h"
#include "qlang/ops/q_dollar.h"
#include "qlang/base/q_type.h"  /* int/float admission + the tag/letter/name vocabulary */
#include "qlang/base/q_err.h"
#include "qlang/parse/q_tok.h"   /* q_tok — THE Tok entry */
#include "qlang/base/q_calendar.h" /* q_calendar_ts_compose — date->timestamp cast */
#include "qlang/ops/q_index.h"  /* q_index_any_nested_item — the "c"$ pack/distribute boundary */
#include "qlang/ops/q_sys.h"    /* q_sys_week_offset — the live `\W` start day */
#include "ops/temporal.h"  /* ray_temporal_extract — base calendar decomposition */
#include "lang/cal.h"     /* THE datetime and timestamp splits */
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "lang/eval.h"      /* ray_cast_fn */
#include "lang/internal.h"  /* ray_typed_null, ray_guid, ray_str_vec_get, ray_error */
#include <math.h>           /* isnan */
#include <stdint.h>         /* INT32/64 MAX — temporal infinity mapping */
#include <string.h>
#include <stdlib.h>

/* Number, letter and name all resolve through q_type's maps, so the #209 guarantee (a new datatype must
 * name its designator) rides q_type_char's compile-guarded switch; ENUM and physical STR are never targets. */
int8_t q_cast_designator(ray_t* t, int* is_tok, int* is_identity) {
    *is_tok = 0;
    if (is_identity) *is_identity = 0;
    if (!t) return 0;
    if (t->type == -RAY_I16) {
        if (RAY_ATOM_IS_NULL(t)) return 0;
        int n = t->i16;
        if (n <= 0) { *is_tok = 1; n = -n; }
        if (n == RAY_LIST && is_identity) *is_identity = 1;   /* 0h is Identity (cast.md:40) */
        if (n >= RAY_TYPE_COUNT || n == RAY_ENUM || n == RAY_STR) return 0;
        return q_type_char((int8_t)n) ? (int8_t)n : 0;
    }
    if ((t->type == -RAY_STR && ray_str_len(t) == 1) ||
        t->type == -RAY_CHARV ||
        (t->type == RAY_CHARV && q_count(t) == 1)) {
        char c = t->type == -RAY_CHARV ? (char)t->u8
               : t->type == RAY_CHARV  ? ((const char*)ray_data(t))[0]
                                       : ray_str_ptr(t)[0];
        if (c >= 'A' && c <= 'Z') { *is_tok = 1; c = (char)(c - 'A' + 'a'); }
        if (c == '*' && is_identity) *is_identity = 1;
        return q_type_of_char(c);
    }
    if (t->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(t->i64);
        if (!s) return 0;
        size_t l = ray_str_len(s);
        int8_t r = l ? q_type_of_qname(ray_str_ptr(s), l) : RAY_SYM;
        if (!l) *is_tok = 1;
        ray_release(s);
        return r;
    }
    return 0;
}

static ray_t* cast_u8(ray_t* x);
static ray_t* cast_tod(int8_t tag, ray_t* x);

/* The one place the rayfall `as` vocabulary appears in the q layer; a tag base cannot cast to is 'type. */
static const char* cast_rayname(int8_t tag) {
    switch (tag) {
    case RAY_BOOL: return "BOOL";   case RAY_BYTE_ONLY: return "U8";
    case RAY_I16: return "I16";     case RAY_I32: return "I32";
    case RAY_I64: return "I64";     case RAY_F64: return "F64";
    case RAY_DATE: return "DATE";   case RAY_TIME: return "TIME";
    case RAY_MONTH: return "MONTH"; case RAY_MINUTE: return "MINUTE";
    case RAY_SECOND: return "SECOND";       case RAY_TIMESPAN: return "TIMESPAN";
    case RAY_TIMESTAMP: return "TIMESTAMP"; case RAY_DATETIME: return "DATETIME";
    default: return NULL;
    }
}

static ray_t* cast_delegate(int8_t tag, ray_t* x) {
    const char* nm = cast_rayname(tag);
    if (!nm) return q_err(QE_TYPE);
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
 * new ladder: q_type_strict_i64 = ints + int-backed temporals, q_type_strict_f64 adds
 * F64/F32/DATETIME. */
static ray_t* cast_bool(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == -RAY_BOOL || x->type == RAY_BOOL) { ray_retain(x); return x; }
    /* text: chars ARE bytes (string-C3), so "b"$ is per-char nonzero-code —
     * "b"$" ",.Q.an -> 64#1b (cast/cast golden).  Truthiness keeps its own
     * string-emptiness law at its one home (q_eval_apply_truthy). */
    if (x->type == RAY_CHARV || x->type == -RAY_CHARV || x->type == -RAY_STR) {
        ray_t* b = cast_u8(x);
        if (!b || RAY_IS_ERR(b)) return b;
        ray_t* r = cast_bool(b);
        ray_release(b);
        return r;
    }
    int64_t i;
    if (q_type_strict_i64(x, &i)) return ray_bool(i != 0);
    double d;
    if (q_type_strict_f64(x, &d)) return ray_bool(d != 0.0);   /* 0n is NaN; NaN != 0 -> 1b */
    /* An atom reaching here (guid, sym) has no boolean law.  Atoms never
     * delegate: that re-enters the null-propagation being intercepted, which is
     * how 0Ng returned 0b while a non-null guid errored. */
    if (x->type < 0) return q_err(QE_TYPE);
    return cast_delegate(RAY_BOOL, x);   /* vectors (F32 included): base's arm IS this law */
}

/* char cast (`10h$`/`` `char$``/`"c"$`): reinterpret an integer/byte value as
 * chars, producing a native string (peachq has no char-atom type distinct from
 * a 1-char string).  The boxed-list arm packs a list of ATOMS into ONE string,
 * so "c"$ must beat q_dollar_cast's RAY_LIST distribution (which would build a
 * list of 1-char strings — q_list_collapse refuses to pack them). */
static ray_t* cast_str(ray_t* x) {
    if (x && x->type == -RAY_STR) { ray_retain(x); return x; }   /* identity */
    if (x && (x->type == RAY_CHARV || x->type == -RAY_CHARV)) {  /* charv identity */
        ray_retain(x); return x;
    }
    /* a boolean is one byte (cast.md: the bit pattern is unchanged) */
    if (x && (x->type == RAY_BYTE_ONLY || x->type == RAY_BOOL))
        return ray_str((const char*)ray_data(x), (size_t) q_count(x));
    if (x && (x->type == -RAY_BYTE_ONLY || x->type == -RAY_BOOL)) return ray_char(x->u8);
    if (q_type_is_int_atom(x)) {
        return ray_char((uint8_t)q_type_iatom_val(x));   /* `char$65 -> "A" (atom) */
    }
    if (q_type_is_int_vec(x)) {
        int64_t n = q_count(x);
        char* buf = (char*)malloc(n > 0 ? (size_t)n : 1);
        if (!buf) return q_err(QE_WSFULL);
        for (int64_t i = 0; i < n; i++)
            buf[i] = (char)q_type_ivec_get(x, i);
        ray_t* r = ray_str(buf, (size_t)n);
        free(buf);
        return r;
    }
    /* float/real -> char rides the byte cast (`char$65e -> "A", cast.md:20
     * designator row): cast_u8 owns the float rounding law. */
    if (x && (x->type == -RAY_F64 || x->type == -RAY_F32 ||
              x->type == RAY_F64  || x->type == RAY_F32)) {
        ray_t* b = cast_u8(x);
        if (!b || RAY_IS_ERR(b)) return b;
        ray_t* r = cast_str(b);
        ray_release(b);
        return r;
    }
    if (x && x->type == RAY_LIST) {          /* boxed list of int/byte ATOMS -> string */
        int64_t n = q_count(x);
        ray_t** e = (ray_t**)ray_data(x);
        char* buf = (char*)malloc(n > 0 ? (size_t)n : 1);
        if (!buf) return q_err(QE_WSFULL);
        for (int64_t i = 0; i < n; i++) {
            ray_t* ei = e[i];
            int64_t v;
            if      (ei && ei->type == -RAY_I64) v = ei->i64;
            else if (ei && ei->type == -RAY_I32) v = ei->i32;
            else if (ei && ei->type == -RAY_I16) v = ei->i16;
            else if (ei && (ei->type == -RAY_BYTE_ONLY || ei->type == -RAY_BOOL)) v = ei->u8;
            else if (ei && ei->type == -RAY_STR && ray_str_len(ei) == 1)
                v = (unsigned char)ray_str_ptr(ei)[0];
            else if (ei && ei->type == -RAY_CHARV) v = ei->u8;
            else { free(buf); return q_err(QE_TYPE); }
            buf[i] = (char)v;
        }
        ray_t* r = ray_str(buf, (size_t)n);
        free(buf);
        return r;
    }
    return q_err(QE_TYPE);
}

/* Integer targets (I64/I32/I16): kdb ROUNDS floats HALF-TO-EVEN (banker's:
 * `long$3.7 -> 4, "j"$2.5 -> 2, `int$6.6 -> 7 — cast.md ex + the banked golden
 * cond/cast_cond_simple:9) where rayfall `as` truncates, so pre-round via rint()
 * here; the rest is base's.  (The timestored guide's `int$100.5 -> 101 is a
 * third-party error — real kdb gives 100 by banker's rounding.) */
/* Real (float32) target.  Base rayfall has an `as float` (F64) arm but no
 * `real`/F32 one, so `"e"$` used to 'nyi. Reuse the base F64 cast (handles every
 * numeric input + string parse, exactly like `"f"$`), then narrow F64 -> F32. */
static ray_t* q_cast_real(ray_t* x) {
    ray_t* f = cast_delegate(RAY_F64, x);
    if (RAY_IS_ERR(f)) return f;
    if (f->type == -RAY_F64) {                              /* atom */
        ray_t* r = RAY_ATOM_IS_NULL(f) ? ray_typed_null(-RAY_F32)
                                       : ray_f32((float)f->f64);
        ray_release(f);
        return r;
    }
    if (f->type == RAY_F64) {                               /* vector */
        int64_t n = q_count(f);
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

/* A float beyond the target's range, ±inf included, saturates to its ±0W (`long$0w` -> 0W; the infinity
 * corresponding to numeric x is min 0#x, ref/cast.md; qcheck t/review.q:3 needs "j"$1e300 -> 0W). */
static int64_t cast_float_lane(int8_t tag, double v) {
    int64_t w = 0;
    ray_type_inf(tag, v > 0, &w);
    double r = rint(v);
    return (v > 0 ? r >= (double)w : r <= (double)w) ? w : (int64_t)r;
}

static ray_t* cast_int(int8_t tag, ray_t* x) {
    if (x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null((int8_t)-tag);
        int64_t v = cast_float_lane(tag, x->f64);       /* F32 atoms store f64 payload */
        if (tag == RAY_I64) return ray_i64(v);
        if (tag == RAY_I32) return ray_i32((int32_t)v);
        return ray_i16((int16_t)v);
    }
    if (x && (x->type == RAY_F64 || x->type == RAY_F32)) {
        int64_t n = q_count(x);
        ray_t* out = ray_vec_new(tag, n);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        int is64 = (x->type == RAY_F64);
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(x))[i]
                            : (double)((const float*)ray_data(x))[i];
            int isnull = isnan(v);
            int64_t iv = isnull ? 0 : cast_float_lane(tag, v);
            if      (tag == RAY_I64) ((int64_t*)ray_data(out))[i] = iv;
            else if (tag == RAY_I32) ((int32_t*)ray_data(out))[i] = (int32_t)iv;
            else                     ((int16_t*)ray_data(out))[i] = (int16_t)iv;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* Owner ruling 2026-08-11: an integral infinity NARROWING to a smaller int
     * saturates to the target's ±0W (`int$0W -> 0Wi), extending the float-
     * source law above.  Widening keeps the bit pattern (ref/cast.md:193-204
     * pins `float$0Wh -> 32767f), nulls stay typed nulls, and finite out-of-
     * range values still truncate — all via base's arms below. */
    if (x && (x->type == -RAY_I64 || x->type == -RAY_I32) &&
        ray_elem_size((int8_t)-x->type) > ray_elem_size(tag) && q_type_is_inf(x)) {
        int64_t v = x->type == -RAY_I64 ? x->i64 : (int64_t)x->i32;
        int64_t inf = 0;
        ray_type_inf(tag, v > 0, &inf);
        return tag == RAY_I32 ? ray_i32((int32_t)inf) : ray_i16((int16_t)inf);
    }
    if (x && (x->type == RAY_I64 || x->type == RAY_I32) &&
        ray_elem_size(x->type) > ray_elem_size(tag)) {
        int64_t n = q_count(x);
        ray_t* out = ray_vec_new(tag, n);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        int is64 = (x->type == RAY_I64);
        int64_t src_inf = 0;
        ray_type_inf(x->type, 1, &src_inf);
        for (int64_t i = 0; i < n; i++) {
            int64_t v = is64 ? ((const int64_t*)ray_data(x))[i]
                             : (int64_t)((const int32_t*)ray_data(x))[i];
            int isnull = ray_vec_is_null(x, i);
            if (!isnull && (v == src_inf || v == -src_inf))
                ray_type_inf(tag, v > 0, &v);
            if (tag == RAY_I32) ((int32_t*)ray_data(out))[i] = (int32_t)v;
            else                ((int16_t*)ray_data(out))[i] = (int16_t)v;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    return cast_delegate(tag, x);
}

/* Byte target.  kdb `"x"$str` maps CHARS to bytes ("x"$"abc" -> 0x616263,
 * ref/cast.md #byte); base's U8 STR arm parses decimal text instead — pre-empt
 * it.  One char = char atom -> byte ATOM; else a byte vector of the raw chars
 * (empty string -> empty byte vector).  Integer sources take the low byte
 * (modular: "x"$3 4 5 -> 0x030405, cast.qcmd) — base's U8 arm passes int
 * VECTORS through untouched, so own them here.  Byte joins the integer family
 * for float rounding (derived — byte float-cast is unpinned); float null ->
 * 0x00: byte has no null (basics/datatypes.md blank column). */
/* ref/cast.md:120 — longs greater than 0wi cast to 0xff. */
static uint8_t cast_u8_scalar(int64_t v) {
    return v > INT32_MAX ? 0xff : (uint8_t)v;
}

static ray_t* cast_u8(ray_t* x) {
    if (x && x->type == -RAY_STR) {
        const char* sp = ray_str_ptr(x);
        size_t sl = ray_str_len(x);
        if (sl == 1) return ray_u8((uint8_t)sp[0]);
        return ray_vec_from_raw(RAY_BYTE_ONLY, sp, (int64_t)sl);
    }
    if (x && x->type == -RAY_CHARV) return ray_u8(x->u8);
    if (x && x->type == RAY_CHARV)
        return ray_vec_from_raw(RAY_BYTE_ONLY, ray_data(x), q_count(x));
    if (x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_u8(0);
        return ray_u8((uint8_t)(int64_t)rint(x->f64));  /* F32 stores f64 */
    }
    if (x && (x->type == RAY_F64 || x->type == RAY_F32)) {
        int64_t n = q_count(x);
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
    if (q_type_is_int_atom(x)) return ray_u8(cast_u8_scalar(q_type_iatom_val(x)));
    int64_t tv;
    /* the low byte, as base's temporal VECTOR arm gives the atom's own vector */
    if (x && x->type < 0 && RAY_IS_TEMPORAL(-x->type) && q_type_strict_i64(x, &tv))
        return ray_u8((uint8_t)tv);
    if (q_type_is_int_vec(x)) {
        int64_t n = q_count(x);
        ray_t* out = ray_vec_new(RAY_BYTE_ONLY, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        uint8_t* d = (uint8_t*)ray_data(out);
        for (int64_t i = 0; i < n; i++) d[i] = cast_u8_scalar(q_type_ivec_get(x, i));
        return out;
    }
    return cast_delegate(RAY_BYTE_ONLY, x);
}

/* Timestamp target.  `timestamp$date: days -> ns, SATURATING outside the
 * timestamp year range (`timestamp$1666.09.02 -> -0Wp, datatypes.md:149) —
 * base's arm multiplies unchecked (i64 overflow, UBSan, builtins.c:1616) — and
 * mapping the date sentinels to the i64 sentinels (0Nd -> 0Np, +-0Wd -> +-0Wp,
 * which the saturation clamp yields for free). */
static ray_t* cast_timestamp(ray_t* x) {
    if (x && x->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(q_calendar_ts_compose((int64_t)x->i32, 0));
    }
    if (x && x->type == RAY_DATE) {
        int64_t n = q_count(x);
        ray_t* out = ray_vec_new(RAY_TIMESTAMP, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        const int32_t* d = (const int32_t*)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            int isnull = (d[i] == INT32_MIN);
            ((int64_t*)ray_data(out))[i] =
                isnull ? 0 : q_calendar_ts_compose((int64_t)d[i], 0);
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
        int64_t src, dst;
        ray_type_inf(RAY_TIME, x->i32 > 0, &src);
        if (x->i32 == src && ray_type_inf(RAY_TIMESTAMP, x->i32 > 0, &dst))
            return ray_timestamp(dst);
        return ray_timestamp((int64_t)x->i32 * 1000000LL);
    }
    if (x && x->type == RAY_TIME) {
        int64_t n = q_count(x);
        ray_t* out = ray_vec_new(RAY_TIMESTAMP, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        const int32_t* d = (const int32_t*)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            int isnull = (d[i] == NULL_I32);
            int64_t src, dst = 0;
            ray_type_inf(RAY_TIME, d[i] > 0, &src);
            ((int64_t*)ray_data(out))[i] =
                isnull ? 0
                : (d[i] == src && ray_type_inf(RAY_TIMESTAMP, d[i] > 0, &dst)) ? dst
                : (int64_t)d[i] * 1000000LL;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* a float counts nanoseconds as the long it casts to, not datetime's days: qstudio-private's
     * KdbHelperDataTableImageTest golden renders `timestamp$110.11 on 2000.01.01 */
    if (x && (x->type == -RAY_F64 || x->type == -RAY_F32 || x->type == RAY_F64 || x->type == RAY_F32)) {
        ray_t* ns = cast_int(RAY_I64, x);
        if (!ns || RAY_IS_ERR(ns)) return ns;
        ray_t* r = cast_delegate(RAY_TIMESTAMP, ns);
        ray_release(ns);
        return r;
    }
    return cast_delegate(RAY_TIMESTAMP, x);
}

/* A long reaches a 32-bit temporal through the int lane, so its infinities saturate as `int$ does ("u"$0W -> 0Wu,
 * qcheck t/types.q:14); finite values truncate there exactly as base's own arm does. */
static ray_t* cast_temporal32(int8_t tag, ray_t* x) {
    if (q_type_elem_tag(x) != -RAY_I64) return cast_delegate(tag, x);
    ray_t* i = cast_int(RAY_I32, x);
    if (!i || RAY_IS_ERR(i)) return i;
    ray_t* r = cast_delegate(tag, i);
    ray_release(i);
    return r;
}

/* Symbol target: `symbol$sym is identity; strings follow the Tok law
 * (ref/tok.md Symbols: `$"hello" -> `hello, blanks trimmed, `$"" -> `) —
 * the cast/tok distinction has no doc-visible difference for sym targets.
 * Every other source has no conversion: 'type. */
static ray_t* cast_sym(ray_t* x) {
    if (x && (x->type == -RAY_SYM || x->type == RAY_SYM)) {
        ray_retain(x);
        return x;
    }
    const char* p; int64_t n;
    if (x && q_str_text_bytes(x, &p, &n)) return q_tok(RAY_SYM, p, (size_t)n);
    return q_err(QE_TYPE);
}

/* The ONE cast home (contract: q_dollar.h).  Dispatch is on the TARGET tag:
 * every target gets an arm naming the helper that owns it (the SOURCE types
 * live in that helper), or delegates to base ray_cast_fn where kdb and rayfall
 * already agree.  The switch has NO `default:`, so -Wall (=> -Wswitch) +
 * -Werror refuse to build a target no arm states. */
ray_t* q_dollar_cast(int8_t tag, ray_t* x) {
    if (q_type_is_empty_list(x)) return q_type_empty(tag);   /* () has no item to infer from: the tag names the domain */
    /* enum SOURCE: int-family targets read the POSITIONS ("i"$e is unchanged
     * by a domain edit — ref/enumerate.md); every other target sees the
     * resolved symlist (the decay law: `$e -> syms, "c"/string via sym). */
    if (q_enum_is(x)) {
        ray_t* v = (tag == RAY_I16 || tag == RAY_I32 || tag == RAY_I64)
                       ? q_enum_positions(x) : q_enum_decay(x);
        if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_TYPE);
        ray_t* r = q_dollar_cast(tag, v);
        ray_release(v);
        return r;
    }
    /* Precedes the switch by ORDER, not preference: "c"$ packs a boxed list of
     * ATOMS into ONE string (kdb's atomic cast collapses the char atoms the
     * same way), so it must beat the per-tag arms AND the RAY_LIST
     * distribution below — which a list holding a collection falls to, like
     * every other target (owner ruling 2026-09-17). */
    if (tag == RAY_CHARV && !q_index_any_nested_item(x))
        return q_str_charv_out(cast_str(x));
    /* numeric cast of char text = code points (`int$"ABC" -> 65 66 67i;
     * `float$"AC" -> 65 67f, ref/log.md:101) — via the byte cast, then cast. */
    if (x && (x->type == RAY_CHARV || x->type == -RAY_CHARV) &&
        (tag == RAY_I16 || tag == RAY_I32 || tag == RAY_I64 ||
         tag == RAY_F32 || tag == RAY_F64)) {
        ray_t* b = cast_u8(x);
        if (!b || RAY_IS_ERR(b)) return b;
        ray_t* r = q_dollar_cast(tag, b);
        ray_release(b);
        return r;
    }
    /* general (boxed) list: cast each element, then collapse — typed vectors
     * are leaves the switch below hits WHOLE (vectorized kernels). */
    if (x && x->type == RAY_LIST) {
        int64_t n = q_count(x);
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        ray_t** e = (ray_t**)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            ray_t* r = q_dollar_cast(tag, e[i]);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r ? r : q_err(QE_TYPE); }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        ray_t* c = q_list_collapse(out);
        ray_release(out);
        return c;
    }
    switch ((ray_type_e)tag) {
    case RAY_CHARV: break;                   /* hoisted above: the atom pack; nested lists distributed */
    case RAY_LIST: break;                    /* tag 0 is not a cast designator */
    case RAY_GUID:                           /* identity on guids; no base arm for any other source */
        if (q_type_elem_tag(x) == -RAY_GUID) { ray_retain(x); return x; }
        break;
    case RAY_ENUM: break;                    /* never a designator (q_cast_designator) */
    case RAY_STR:  break;                    /* physical tag: never a cast target */
    case RAY_F32:  return q_cast_real(x);    /* real: narrow base F64 cast to F32 */
    case RAY_BOOL: return cast_bool(x);
    case RAY_BYTE_ONLY: return cast_u8(x);
    case RAY_I16: case RAY_I32: case RAY_I64:
        return cast_int(tag, x);
    case RAY_TIMESTAMP: return cast_timestamp(x);
    case RAY_SYM:  return cast_sym(x);
    case RAY_TIMESPAN: case RAY_MINUTE: case RAY_SECOND: {
        int8_t st = (int8_t)-q_type_elem_tag(x);
        if (st == RAY_TIMESTAMP || st == RAY_DATETIME) return cast_tod(tag, x);
        return tag == RAY_TIMESPAN ? cast_delegate(tag, x) : cast_temporal32(tag, x);
    }
    case RAY_MONTH: case RAY_DATE: case RAY_TIME:
        return cast_temporal32(tag, x);
    case RAY_F64: case RAY_DATETIME:
        return cast_delegate(tag, x);
    }
    /* the `break` arms above + any out-of-band tag: no base spelling, so cast_delegate refuses 'type */
    return cast_delegate(tag, x);
}

/* kdb Tok (ref/tok.md): parse a string as a value of the tag type.  Leading/
 * trailing blanks are trimmed; unparseable or out-of-range -> typed null.
 * Recursion stops at STRINGS, not atoms: boxed lists and physical string
 * columns distribute per element; a non-string leaf is a 'type error. */
static ray_t* tok_leaf(int8_t tag, ray_t* x) {
    if (q_type_is_empty_list(x)) return q_type_empty(tag);
    if (x->type == RAY_LIST) {           /* boxed list: tok each element */
        int64_t n = q_count(x);
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        ray_t** e = (ray_t**)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            ray_t* r = tok_leaf(tag, e[i]);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r ? r : q_err(QE_TYPE); }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        ray_t* c = q_list_collapse(out);
        ray_release(out);
        return c;
    }
    if (x->type == RAY_STR) {            /* physical string column: tok each */
        int64_t n = q_count(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            size_t sl = 0;
            const char* sp = ray_str_vec_get(x, i, &sl);
            ray_t* r = q_tok(tag, sp ? sp : "", sp ? sl : 0);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        ray_t* c = q_list_collapse(out);
        ray_release(out);
        return c;
    }
    const char* tp; int64_t tn;
    if (!q_str_text_bytes(x, &tp, &tn))
        return q_err(QE_TYPE);
    return q_tok(tag, tp, tp ? (size_t)tn : 0);
}
ray_t* q_dollar_tok(int8_t tag, ray_t* x) {
    return tok_leaf(tag, x);
}

/* q `w$s` PAD (ref/pad.md): a LONG width w left-justifies the string s in a
 * field of |w| spaces (w<0 right-justifies); longer strings truncate to |w|.
 * Non-string operands are a 'type error.  Output mirrors the input's string
 * form (charv vs -RAY_STR). */
static ray_t* pad_leaf(int64_t w, ray_t* x) {
    if (x->type == RAY_LIST) {           /* boxed list -> pad each element */
        int64_t n = q_count(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        ray_t** e = (ray_t**)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            ray_t* r = pad_leaf(w, e[i]);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r ? r : q_err(QE_TYPE); }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_STR) {            /* physical string column -> pad each */
        int64_t n = q_count(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            size_t sn; const char* p = ray_str_vec_get(x, i, &sn);
            ray_t* s = ray_str(p ? p : "", p ? sn : 0);
            if (!s || RAY_IS_ERR(s)) { ray_release(out); return s; }
            ray_t* r = pad_leaf(w, s);
            ray_release(s);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    const char* p; int64_t pn;
    if (!q_str_text_bytes(x, &p, &pn)) return q_err(QE_TYPE);
    if (w == INT64_MIN) return q_err(QE_LIMIT);   /* -w is UB */
    int64_t width = w < 0 ? -w : w;
    int64_t copy = pn < width ? pn : width;
    char stack[256];
    char* b = (width < (int64_t)sizeof stack) ? stack : malloc((size_t)width + 1);
    if (!b) return q_err(QE_WSFULL);
    memset(b, ' ', (size_t)width);
    memcpy(w < 0 ? b + (width - copy) : b, p, (size_t)copy);   /* w<0: text at right */
    ray_t* r = (x->type == -RAY_STR) ? ray_str(b, (size_t)width)
                                     : ray_charv(b, width);
    if (b != stack) free(b);
    return r;
}
ray_t* q_dollar_pad(int64_t w, ray_t* x) {
    return pad_leaf(w, x);
}

/* Enumerate `x$y` (ref/enumerate.md: sym lhs naming a domain list). */
ray_t* q_dollar_enum(ray_t* x, ray_t* y) {
    return q_enum_dollar(x, y);
}

/* `$` as matrix multiply / dot product (ref/mmu.md: `$` is mmu's glyph form).
 * Same home as the `mmu` keyword; q_dollar dispatches here only when BOTH
 * operands mmu-classify, so every non-mmu shape keeps its cast-path behavior. */
ray_t* q_dollar_mmu(ray_t* x, ray_t* y) {
    return q_mmu_wrap(x, y);
}

/* `$` temporal-component extraction (ref/cast.md:133-142).  A symbol from
 * `year`mm`dd`hh`uu`ss`week names a field of a temporal value; `month` is NOT
 * here — it is a TYPE designator (q_cast_designator resolves it to RAY_MONTH,
 * so `month$ts` already yields the month datatype).  Return TYPES differ:
 * year/mm/dd/hh/uu/ss -> int, week -> date; the time of day (no symbol — cast_tod's
 * own component) -> long ns. */
typedef enum {
    QCOMP_YEAR, QCOMP_MM, QCOMP_DD, QCOMP_HH, QCOMP_UU, QCOMP_SS, QCOMP_WEEK, QCOMP_TOD
} q_comp_e;

static int component_of_sym(ray_t* t) {
    if (!t || t->type != -RAY_SYM) return -1;
    ray_t* s = ray_sym_str(t->i64);
    if (!s) return -1;
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int r = -1;
    if      (l == 4 && !memcmp(nm, "year", 4)) r = QCOMP_YEAR;
    else if (l == 2 && !memcmp(nm, "mm",   2)) r = QCOMP_MM;
    else if (l == 2 && !memcmp(nm, "dd",   2)) r = QCOMP_DD;
    else if (l == 2 && !memcmp(nm, "hh",   2)) r = QCOMP_HH;
    else if (l == 2 && !memcmp(nm, "uu",   2)) r = QCOMP_UU;
    else if (l == 2 && !memcmp(nm, "ss",   2)) r = QCOMP_SS;
    else if (l == 4 && !memcmp(nm, "week", 4)) r = QCOMP_WEEK;
    ray_release(s);
    return r;
}

/* ref/cast.md:155 validity matrix (`month` column omitted — type-cast path). */
static int component_valid(int8_t t, q_comp_e c) {
    int is_date  = (c == QCOMP_YEAR || c == QCOMP_MM);
    int is_wkdd  = (c == QCOMP_WEEK || c == QCOMP_DD);
    int is_clock = (c == QCOMP_HH || c == QCOMP_UU || c == QCOMP_SS);
    switch ((ray_type_e)t) {
    case RAY_TIMESTAMP: case RAY_DATETIME: return 1;
    case RAY_MONTH: return is_date;
    case RAY_DATE:  return is_date || is_wkdd;
    case RAY_TIMESPAN: case RAY_MINUTE: case RAY_SECOND: case RAY_TIME:
        return is_clock;
    default: return 0;
    }
}

/* One temporal value (native payload) -> (days since 2000.01.01, nanosecond of
 * day in [0,86400e9)).  time-of-day types reduce modulo their own unit first so
 * the ns multiply cannot overflow i64 at the inf sentinels. */
static void temporal_parts(int8_t t, int64_t raw, double rawf,
                             int64_t* days, int64_t* tod_ns) {
    switch ((ray_type_e)t) {
    case RAY_TIMESTAMP: *days = ts_days_floor(raw); *tod_ns = ts_ns_in_day(raw); break;
    case RAY_DATETIME:  datetime_to_day_ns(rawf, days, tod_ns); break;
    case RAY_DATE:  *days = raw; *tod_ns = 0; break;
    case RAY_MONTH: *days = month_payload_as_days(raw); *tod_ns = 0; break;
    case RAY_TIMESPAN: *days = 0; *tod_ns = raw; break;   /* signed duration ns */
    case RAY_MINUTE: *days = 0;
        *tod_ns = (((raw % 1440) + 1440) % 1440) * 60000000000LL; break;
    case RAY_SECOND: *days = 0;
        *tod_ns = (((raw % 86400) + 86400) % 86400) * 1000000000LL; break;
    case RAY_TIME: *days = 0;
        *tod_ns = (((raw % 86400000) + 86400000) % 86400000) * 1000000LL; break;
    default: *days = 0; *tod_ns = 0; break;
    }
}

static int8_t component_tag(q_comp_e c) {
    return c == QCOMP_WEEK ? RAY_DATE : c == QCOMP_TOD ? RAY_I64 : RAY_I32;
}

/* days/tod -> the extracted scalar; *rtag is the RESULT tag (component_tag).
 * Calendar fields (year/mm/dd) reuse the frozen base decomposition — the same
 * ray_temporal_extract the dot accessor uses — via a throwaway RAY_DATE mirror,
 * so the Hinnant civil_from_days lives in ONE place.  Clock fields stay a
 * SIGNED inline division: timespan is an unbounded signed duration and the base
 * HOUR/MINUTE/SECOND wrap+cap it at 24h (0D25:00:00 -> 25, never 1). */
static int64_t component_value(q_comp_e c, int64_t days, int64_t tod, int8_t* rtag) {
    *rtag = component_tag(c);
    if (c == QCOMP_WEEK) return q_calendar_week_start(days, q_sys_week_offset());
    if (c == QCOMP_TOD) return tod;
    switch (c) {
    case QCOMP_YEAR: case QCOMP_MM: case QCOMP_DD: {
        int field = c == QCOMP_YEAR ? RAY_EXTRACT_YEAR
                  : c == QCOMP_MM   ? RAY_EXTRACT_MONTH : RAY_EXTRACT_DAY;
        ray_t* mirror = ray_date(days);
        ray_t* got = ray_temporal_extract(mirror, field);
        int64_t v = got->i64;
        ray_release(mirror); ray_release(got);
        return v;
    }
    case QCOMP_HH: return tod / 3600000000000LL;
    case QCOMP_UU: return (tod / 60000000000LL) % 60;
    default:       return (tod / 1000000000LL) % 60;   /* SS */
    }
}

static int64_t temporal_raw_atom(int8_t at, ray_t* x) {
    return RAY_IS_TEMPORAL64(at) ? x->i64 : (int64_t)x->i32;
}
static int64_t temporal_raw_vec(int8_t at, const void* base, int64_t i) {
    return RAY_IS_TEMPORAL64(at) ? ((const int64_t*)base)[i]
                                 : (int64_t)((const int32_t*)base)[i];
}

/* Temporal component over an atom or simple vector; an
 * invalid (component, temporal-type) pair per the matrix is a 'type error
 * (the doc pins the valid set, not the invalid-pair result — honest refusal
 * beats a fabricated value). */
static ray_t* component_leaf(ray_t* x, int64_t comp) {
    q_comp_e c = (q_comp_e)comp;
    if (!x) return q_err(QE_TYPE);
    if (x->type == RAY_LIST) {           /* boxed list -> component each element */
        int64_t n = q_count(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        ray_t** e = (ray_t**)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            ray_t* r = component_leaf(e[i], comp);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r ? r : q_err(QE_TYPE); }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        ray_t* col = q_list_collapse(out);
        ray_release(out);
        return col;
    }
    int8_t at = x->type < 0 ? (int8_t)-x->type : x->type;
    int temporal = RAY_IS_TEMPORAL32(at) || RAY_IS_TEMPORAL64(at) ||
                   RAY_IS_TEMPORALF(at);
    if (!temporal) return q_err(QE_TYPE);
    if (!component_valid(at, c))
        return q_err(QE_TYPE);
    /* A non-finite DATETIME (canonically 0n) has no meaningful field AND would
     * make floor()/(int64_t) UB — treat it as null, like the sentinel. */
    int datetimef = RAY_IS_TEMPORALF(at);
    if (x->type < 0) {
        int8_t rtag = component_tag(c);
        if (RAY_ATOM_IS_NULL(x) || (datetimef && !isfinite(x->f64)))
            return ray_typed_null((int8_t)-rtag);
        int64_t days, tod;
        double rawf = datetimef ? x->f64 : 0.0;
        temporal_parts(at, temporal_raw_atom(at, x), rawf, &days, &tod);
        int64_t v = component_value(c, days, tod, &rtag);
        return rtag == RAY_DATE ? ray_date(v) : rtag == RAY_I64 ? ray_i64(v) : ray_i32((int32_t)v);
    }
    int8_t rtag = component_tag(c);
    int64_t n = q_count(x);
    ray_t* out = ray_vec_new(rtag, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    const void* base = ray_data(x);
    const double* fbase = (const double*)base;
    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(x, i) || (datetimef && !isfinite(fbase[i]))) {
            ray_vec_set_null(out, i, true); continue;
        }
        int64_t days, tod;
        double rawf = datetimef ? fbase[i] : 0.0;
        temporal_parts(at, temporal_raw_vec(at, base, i), rawf, &days, &tod);
        int8_t rt; int64_t v = component_value(c, days, tod, &rt);
        if (rt == RAY_I64) ((int64_t*)ray_data(out))[i] = v;
        else ((int32_t*)ray_data(out))[i] = (int32_t)v;   /* date + int both i32-stored */
    }
    return out;
}

/* timespan/minute/second of a timestamp or datetime is its time of day (ref/cast.md:168 "[) notions";
 * kdb-common rand.q:4): the ns of day as a timespan, then the target's own duration narrowing */
static ray_t* cast_tod(int8_t tag, ray_t* x) {
    ray_t* ns = component_leaf(x, QCOMP_TOD);
    if (!ns || RAY_IS_ERR(ns)) return ns ? ns : q_err(QE_TYPE);
    ray_t* span = cast_delegate(RAY_TIMESPAN, ns);
    ray_release(ns);
    if (tag == RAY_TIMESPAN || !span || RAY_IS_ERR(span)) return span ? span : q_err(QE_TYPE);
    ray_t* r = cast_delegate(tag, span);
    ray_release(span);
    return r;
}

/* `sym$temporal` component extraction; NULL if `sym` names no component. */
static ray_t* component_extract(ray_t* t, ray_t* x) {
    int c = component_of_sym(t);
    if (c < 0) return NULL;
    return component_leaf(x, c);
}

/* `$` over a table: q_table_map_cols walks the columns; this colfn re-enters
 * q_dollar so each column re-classifies against the designator carried in ctx. */
static ray_t* dollar_col(void* ctx, ray_t* col) {
    return q_dollar((ray_t*)ctx, col);
}

/* q `t$x` — the `$` verb (contract: q_dollar.h).  Family "none": receives
 * WHOLE args and self-distributes.  DICT/TABLE are UNIFORM structure — handled
 * once here by re-entering q_dollar per value/column (keys/colnames kept), so
 * every overload inherits them.  Then LEFT-operand dispatch: a LONG width is
 * PAD; mmu-shaped float operands (both sides) are matrix multiply; a
 * multi-designator LHS ("fiij", `int`float, 5 6h, (`int;"i";6h)) zips over x,
 * RE-ENTERING q_dollar per pair (ref/cast.md pins (`int;"i";6h)$10 -> 10 10
 * 10i: an ATOM rhs is broadcast); a single designator resolves via
 * q_cast_designator into q_dollar_cast / q_dollar_tok, each of which owns its
 * own string/list boundary; a non-designator sym is a temporal component or
 * Enumerate.  LIST stays inside the leaves so it cannot preempt the mmu form. */
ray_t* q_dollar(ray_t* t, ray_t* x) {
    if (x && x->type == RAY_DICT) {
        ray_t* nv = q_dollar(t, ray_dict_vals(x));
        if (!nv || RAY_IS_ERR(nv)) return nv ? nv : q_err(QE_TYPE);
        ray_t* keys = ray_dict_keys(x);
        ray_retain(keys);
        return ray_dict_new(keys, nv);                 /* consumes both */
    }
    if (x && x->type == RAY_TABLE) return q_table_map_cols(dollar_col, t, x);
    if (t && t->type == -RAY_I64) return q_dollar_pad(t->i64, x);
    int64_t k;
    if (q_mmu_class(t, &k) != QMMU_BAD && q_mmu_class(x, &k) != QMMU_BAD)
        return q_dollar_mmu(t, x);   /* ragged included: mmu owns its 'length */
    int multi = t && ((t->type == -RAY_STR && ray_str_len(t) > 1) ||
                      (t->type == RAY_CHARV && q_count(t) > 1) ||
                      t->type == RAY_SYM || t->type == RAY_I16 ||
                      t->type == RAY_LIST);
    if (multi) {
        int64_t n = q_count(t);
        int x_is_list = x && (ray_is_vec(x) || x->type == RAY_LIST);
        if (x_is_list && q_count(x) != n)
            return q_err(QE_LENGTH);
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
        ray_t* c = q_list_collapse(out);
        ray_release(out);
        return c;
    }
    int is_tok = 0, is_identity = 0;
    int8_t tag = q_cast_designator(t, &is_tok, &is_identity);
    if (!tag) {
        /* Identity (cast.md:40): `0h`/`"*"` returns y ("and y is not a
         * string" — the string-y Tok arm is a deferred divergence). */
        if (is_identity) { ray_retain(x); return x; }
        if (t && t->type == -RAY_SYM) {
            ray_t* comp = component_extract(t, x);   /* year/mm/dd/hh/uu/ss/week */
            if (comp) return comp;
            return q_dollar_enum(t, x);
        }
        return q_err(QE_NYI);
    }
    /* `10h$`/`` `char$``/`"c"$` all land here with is_tok=0 and reinterpret via
     * q_dollar_cast; the UPPERCASE char token `"C"$` carries is_tok=1 and Toks
     * one char per field — `0:`'s char column (ref/file-text.md:369). */
    return is_tok ? q_dollar_tok(tag, x) : q_dollar_cast(tag, x);
}
