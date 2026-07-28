/* q_dollar — the single C home for the `$` verb (contract: q_dollar.h).
 * PURE value semantics: values -> values, no env/runtime state.  q_dollar is
 * the generic registry row; q_dollar_pad / q_dollar_cast / q_dollar_tok /
 * q_dollar_enum / q_dollar_mmu are the per-operation homes, exposed with types
 * for reuse.  The per-target q_cast_* matrix lives here too; the int-atom
 * admission helpers and tag<->name vocabulary moved to q_type.c (q_type.h). */
#include "qlang/ops/q_dollar.h"
#include "qlang/q_type.h"  /* int/float admission + q_type_rayname vocabulary */
#include "qlang/q_err.h"
#include "qlang/q_tok.h"   /* q_tok — THE Tok entry */
#include "qlang/q_calendar.h" /* q_calendar_ts_compose — date->timestamp cast */
#include "ops/temporal.h"  /* ray_temporal_extract — base calendar decomposition */
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "lang/eval.h"      /* ray_cast_fn */
#include "lang/internal.h"  /* ray_typed_null, ray_guid, ray_str_vec_get, ray_error */
#include <math.h>           /* isnan */
#include <stdint.h>         /* INT32/64 MAX — temporal infinity mapping */
#include <string.h>
#include <stdlib.h>

/* Designator resolution is separate from conversion so C callers (future
 * bool-widening / promotion work) can invoke q_dollar_cast(tag, x) directly. */

int8_t q_cast_designator(ray_t* t, int* is_tok, int* is_identity) {
    *is_tok = 0;
    if (is_identity) *is_identity = 0;
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
        case RAY_LIST:      /* 0h is Identity (cast.md:40): returns y unchanged */
            if (is_identity) *is_identity = 1;
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
        case '*': if (is_identity) *is_identity = 1;  /* Identity (cast.md:40) */
                  return 0;
        default:  return 0;
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

/* Cast/Tok of the empty general list -> q_type_empty(tag) (`long$() / `$()).
 * A general list carries no element to infer from, so the target tag names the
 * empty result's domain. */
static int is_empty_list(ray_t* x) {
    return x && x->type == RAY_LIST && ray_len(x) == 0;
}

static ray_t* cast_u8(ray_t* x);

/* tag -> base `as` spelling, then delegate; 'nyi when the tag has no spelling
 * (LIST/GUID/F32 targets). */
static ray_t* cast_delegate(int8_t tag, ray_t* x) {
    const char* nm = q_type_rayname(tag);
    if (!nm) return q_err(QE_NYI);
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
    return cast_delegate(RAY_BOOL, x);   /* other vectors: base's arm IS this law */
}

/* char cast (`10h$`/`` `char$``/`"c"$`): reinterpret an integer/byte value as
 * chars, producing a native string (openq has no char-atom type distinct from
 * a 1-char string).  The boxed-list arm packs into ONE string, so "c"$ must
 * beat q_dollar_cast's RAY_LIST distribution (which would build a list of
 * 1-char strings — q_list_collapse refuses to pack them). */
static ray_t* cast_str(ray_t* x) {
    if (x && x->type == -RAY_STR) { ray_retain(x); return x; }   /* identity */
    if (x && (x->type == RAY_CHARV || x->type == -RAY_CHARV)) {  /* charv identity */
        ray_retain(x); return x;
    }
    if (x && x->type == RAY_BYTE_ONLY)                                  /* byte vec */
        return ray_str((const char*)ray_data(x), (size_t)ray_len(x));
    if (x && x->type == -RAY_BYTE_ONLY) return ray_char(x->u8);  /* byte atom -> char atom */
    if (q_type_is_int_atom(x)) {
        return ray_char((uint8_t)q_type_iatom_val(x));   /* `char$65 -> "A" (atom) */
    }
    if (q_type_is_int_vec(x)) {
        int64_t n = ray_len(x);
        char* buf = (char*)malloc(n ? (size_t)n : 1);
        if (!buf) return q_err(QE_WSFULL);
        for (int64_t i = 0; i < n; i++)
            buf[i] = (char)q_type_ivec_get(x, i);
        ray_t* r = ray_str(buf, (size_t)n);
        free(buf);
        return r;
    }
    if (x && x->type == RAY_LIST) {          /* boxed list of int/byte -> string */
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        char* buf = (char*)malloc(n ? (size_t)n : 1);
        if (!buf) return q_err(QE_WSFULL);
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

static ray_t* cast_int(int8_t tag, ray_t* x) {
    if (x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null((int8_t)-tag);
        /* ±inf saturates to the target's ±0W (`long$0w` -> 0W; the infinity
         * corresponding to numeric x is min 0#x, ref/cast.md). */
        if (isinf(x->f64)) {
            int64_t w = 0;
            ray_type_inf(tag, x->f64 > 0, &w);
            if (tag == RAY_I64) return ray_i64(w);
            if (tag == RAY_I32) return ray_i32((int32_t)w);
            return ray_i16((int16_t)w);
        }
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
            int64_t iv;
            if (isnull) iv = 0;
            else if (isinf(v)) ray_type_inf(tag, v > 0, &iv);
            else iv = (int64_t)rint(v);
            if      (tag == RAY_I64) ((int64_t*)ray_data(out))[i] = iv;
            else if (tag == RAY_I32) ((int32_t*)ray_data(out))[i] = (int32_t)iv;
            else                     ((int16_t*)ray_data(out))[i] = (int16_t)iv;
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
    if (q_type_is_int_atom(x)) return ray_u8(cast_u8_scalar(q_type_iatom_val(x)));
    if (q_type_is_int_vec(x)) {
        int64_t n = ray_len(x);
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
        int64_t n = ray_len(x);
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
        int64_t n = ray_len(x);
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
    /* float -> timestamp: base truncates the float to a raw ns count, but
     * kdb's unit semantics here (rint-ns vs datetime-style fractional DAYS)
     * is unpinned in the docs corpus — error beats a wrong answer, so the
     * shape is a deferred cell (designator-audit decision, plan 2026-07-07). */
    if (x && (x->type == -RAY_F64 || x->type == -RAY_F32 ||
              x->type == RAY_F64  || x->type == RAY_F32))
        return q_err(QE_NYI);
    return cast_delegate(RAY_TIMESTAMP, x);
}

/* Symbol target: `symbol$sym is identity; strings follow the Tok law
 * (ref/tok.md Symbols: `$"hello" -> `hello, blanks trimmed, `$"" -> `) —
 * the cast/tok distinction has no doc-visible difference for sym targets.
 * Every other source is deferred. */
static ray_t* cast_sym(ray_t* x) {
    if (x && (x->type == -RAY_SYM || x->type == RAY_SYM)) {
        ray_retain(x);
        return x;
    }
    const char* p; int64_t n;
    if (x && q_str_text_bytes(x, &p, &n)) return q_tok(RAY_SYM, p, (size_t)n);
    return q_err(QE_NYI);
}

/* The ONE cast home (contract: q_dollar.h).  Dispatch is on the TARGET tag:
 * every target gets an arm naming the helper that owns it (the SOURCE types
 * live in that helper), or delegates to base ray_cast_fn where kdb and rayfall
 * already agree.  The switch has NO `default:`, so -Wall (=> -Wswitch) +
 * -Werror refuse to build a target no arm states. */
ray_t* q_dollar_cast(int8_t tag, ray_t* x) {
    if (is_empty_list(x)) return q_type_empty(tag);
    /* Precedes the switch by ORDER, not preference: "c"$ packs a boxed list
     * into ONE string, so it must beat the per-tag arms AND the RAY_LIST
     * distribution below. */
    if (tag == RAY_CHARV)
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
        int64_t n = ray_len(x);
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
    case RAY_CHARV: break;                   /* hoisted above: packs boxed lists */
    case RAY_LIST: break;                    /* tag 0 is not a cast designator */
    case RAY_GUID: break;                    /* guid target: no base arm — deferred */
    case RAY_STR:  break;                    /* physical tag: never a cast target */
    case RAY_F32:  return q_cast_real(x);    /* real: narrow base F64 cast to F32 */
    case RAY_BOOL: return cast_bool(x);
    case RAY_BYTE_ONLY: return cast_u8(x);
    case RAY_I16: case RAY_I32: case RAY_I64:
        return cast_int(tag, x);
    case RAY_TIMESTAMP: return cast_timestamp(x);
    case RAY_SYM:  return cast_sym(x);
    case RAY_F64: case RAY_MONTH: case RAY_DATE: case RAY_DATETIME:
    case RAY_TIMESPAN: case RAY_MINUTE: case RAY_SECOND: case RAY_TIME:
        return cast_delegate(tag, x);
    }
    /* the `break` arms above + any out-of-band tag (the band is sparse: 3 is
     * kdb's short-of-3).  cast_delegate has no spelling for them -> 'nyi,
     * which is what each returned before — no bespoke error strings needed. */
    return cast_delegate(tag, x);
}

/* kdb Tok (ref/tok.md): parse a string as a value of the tag type.  Leading/
 * trailing blanks are trimmed; unparseable or out-of-range -> typed null.
 * Recursion stops at STRINGS, not atoms: boxed lists and physical string
 * columns distribute per element; a non-string leaf is a 'type error. */
static ray_t* tok_leaf(int8_t tag, ray_t* x) {
    if (is_empty_list(x)) return q_type_empty(tag);
    if (x->type == RAY_LIST) {           /* boxed list: tok each element */
        int64_t n = ray_len(x);
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
        int64_t n = ray_len(x);
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
        int64_t n = ray_len(x);
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
        int64_t n = ray_len(x);
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

/* Enumerate `x$y` (ref/enumerate.md: sym lhs naming a domain list) — openq has
 * no enum domains, so the whole form is a 'nyi stub awaiting them. */
ray_t* q_dollar_enum(ray_t* x, ray_t* y) {
    (void)x; (void)y;
    return q_err(QE_NYI);
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
 * year/mm/dd/hh/uu/ss -> int, week -> date. */
typedef enum {
    QCOMP_YEAR, QCOMP_MM, QCOMP_DD, QCOMP_HH, QCOMP_UU, QCOMP_SS, QCOMP_WEEK
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
    const int64_t NSDAY = 86400000000000LL;
    switch ((ray_type_e)t) {
    case RAY_TIMESTAMP: { int64_t d = raw / NSDAY, r = raw % NSDAY;
        if (r < 0) { r += NSDAY; d--; } *days = d; *tod_ns = r; break; }
    case RAY_DATETIME: { int64_t d = (int64_t)floor(rawf);   /* floor, not round:
        ref/cast.md:168 narrowing truncates */
        int64_t r = (int64_t)((rawf - (double)d) * (double)NSDAY);  /* [0,NSDAY) */
        *days = d; *tod_ns = r; break; }
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

/* days/tod -> the extracted scalar; *rtag is the RESULT tag (RAY_I32/RAY_DATE).
 * Calendar fields (year/mm/dd) reuse the frozen base decomposition — the same
 * ray_temporal_extract the dot accessor uses — via a throwaway RAY_DATE mirror,
 * so the Hinnant civil_from_days lives in ONE place.  Clock fields stay a
 * SIGNED inline division: timespan is an unbounded signed duration and the base
 * HOUR/MINUTE/SECOND wrap+cap it at 24h (0D25:00:00 -> 25, never 1). */
static int64_t component_value(q_comp_e c, int64_t days, int64_t tod, int8_t* rtag) {
    if (c == QCOMP_WEEK) { *rtag = RAY_DATE; return q_calendar_week_start(days); }
    *rtag = RAY_I32;
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
        int64_t n = ray_len(x);
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
        int8_t rtag = (c == QCOMP_WEEK) ? RAY_DATE : RAY_I32;
        if (RAY_ATOM_IS_NULL(x) || (datetimef && !isfinite(x->f64)))
            return ray_typed_null((int8_t)-rtag);
        int64_t days, tod;
        double rawf = datetimef ? x->f64 : 0.0;
        temporal_parts(at, temporal_raw_atom(at, x), rawf, &days, &tod);
        int64_t v = component_value(c, days, tod, &rtag);
        return rtag == RAY_DATE ? ray_date(v) : ray_i32((int32_t)v);
    }
    int8_t rtag = (c == QCOMP_WEEK) ? RAY_DATE : RAY_I32;
    int64_t n = ray_len(x);
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
        ((int32_t*)ray_data(out))[i] = (int32_t)v;   /* date + int both i32-stored */
    }
    return out;
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
                      (t->type == RAY_CHARV && ray_len(t) > 1) ||
                      t->type == RAY_SYM || t->type == RAY_I16 ||
                      t->type == RAY_LIST);
    if (multi) {
        int64_t n = (t->type == -RAY_STR) ? (int64_t)ray_str_len(t) : ray_len(t);
        int x_is_list = x && (ray_is_vec(x) || x->type == RAY_LIST);
        if (x_is_list && ray_len(x) != n)
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
     * q_dollar_cast; only the UPPERCASE char token `"C"$` carries is_tok=1 and
     * stays a deferred char-Tok — q_tok's default errors 'nyi (pinned by the
     * cast_tok_deferred unit test). */
    return is_tok ? q_dollar_tok(tag, x) : q_dollar_cast(tag, x);
}
