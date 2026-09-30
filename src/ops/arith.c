/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "lang/internal.h"
#include "ops/ops.h"
#include "core/pool.h"   /* ray_pool_dispatch — xbar's parallel morsel loop */

/* Arithmetic builtins (atom-only).
 * Vector dispatch goes through the DAG executor. */

/* kdb integer arithmetic "does no checks for infinities, just treats them
 * as a signed integer" (basics/datatypes.md:236-240 — 0N!0W+til 3 ->
 * 0W 0N -0W — and :157-166, the 0Wp+1 -1 limits walk): two's-complement
 * WRAP is the pinned contract for sentinel/infinity payload arithmetic.
 * C signed overflow is UB, so every temporal payload op goes through
 * unsigned space (same bits, defined behaviour). */
static inline int64_t wrap_add64(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a + (uint64_t)b);
}
static inline int64_t wrap_sub64(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a - (uint64_t)b);
}
static inline int64_t wrap_mul64(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a * (uint64_t)b);
}

/* ns per unit for the DURATION temporals (u/v/t/n); 0 = not a duration.
 * The duration/absolute split mirrors ref/add.md / ref/subtract.md:
 * durations combine to the FINER unit, absolute (p/d/m) ± duration -> p. */
static int64_t duration_unit_ns(int8_t t) {
    switch (-t) {
        case RAY_MINUTE:   return 60000000000LL;
        case RAY_SECOND:   return 1000000000LL;
        case RAY_TIME:     return 1000000LL;
        case RAY_TIMESPAN: return 1LL;
        default:           return 0;
    }
}
static int is_absolute_temporal(ray_t* x) {
    return x->type == -RAY_DATE || x->type == -RAY_MONTH ||
           x->type == -RAY_TIMESTAMP;
}
/* duration (+|-) duration -> the finer unit (every cell of the add/subtract
 * domain-table duration block: u+v->v, u+t->t, n+x->n, t-u->t, ...). */
static ray_t* duration_pair(ray_t* a, ray_t* b, int sub) {
    int64_t ua = duration_unit_ns(a->type), ub = duration_unit_ns(b->type);
    int64_t ur = ua < ub ? ua : ub;
    int8_t  rt = ua < ub ? a->type : b->type;
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(rt);
    int64_t r = sub ? wrap_sub64(temporal_as_ns(a), temporal_as_ns(b))
                    : wrap_add64(temporal_as_ns(a), temporal_as_ns(b));
    r /= ur;
    switch (-rt) {
        case RAY_MINUTE:   return ray_minute(r);
        case RAY_SECOND:   return ray_second(r);
        case RAY_TIME:     return ray_time(r);
        default:           return ray_timespan(r);
    }
}

ray_t* ray_add_fn(ray_t* a, ray_t* b) {
    if ((a && RAY_IS_PARTED(a->type)) || (b && RAY_IS_PARTED(b->type)))
        return atomic_map_binary_op(ray_add_fn, OP_ADD, a, b);
    /* Vector fast path — only when at least one operand is a typed vector */

    /* DATETIME (f64-backed, excluded from is_temporal) — BEFORE the int-
     * temporal blocks, whose payload readers assume int slots.
     * z + num -> z, both orders (float add on days; a NaN payload rides the
     * add and the ctor canonicalizes non-finite results — decision 1).
     * Int-null offsets need the explicit guard (INT_MIN is not NaN).
     * z + z falls through to 'type (two absolute datetimes). */
    if (a->type == -RAY_DATETIME && is_numeric(b)) {
        if (RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_DATETIME);
        return ray_datetime(a->f64 + as_f64(b));
    }
    if (is_numeric(a) && b->type == -RAY_DATETIME) {
        if (RAY_ATOM_IS_NULL(a)) return ray_typed_null(-RAY_DATETIME);
        return ray_datetime(as_f64(a) + b->f64);
    }
    /* date + float -> DATETIME (basics/precision.md:264-271: 2000.01.02 +
     * sum 1000#1%86400 is 2000.01.02T00:16:40.000), both orders.  Must sit
     * before the float+temporal reject below. */
    if (a->type == -RAY_DATE && b->type == -RAY_F64) {
        if (RAY_ATOM_IS_NULL(a)) return ray_typed_null(-RAY_DATETIME);
        return ray_datetime((double)a->i32 + b->f64);
    }
    if (a->type == -RAY_F64 && b->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_DATETIME);
        return ray_datetime(a->f64 + (double)b->i32);
    }

    /* Temporal + integer arithmetic (only int types, not float) */
    if (is_temporal(a) && is_numeric(b) && b->type != -RAY_F64) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(a->type);

        int64_t v = as_i64(b);
        if (a->type == -RAY_DATE)      return ray_date(wrap_add64(a->i64, v));
        if (a->type == -RAY_TIME)      return ray_time(wrap_add64(a->i64, v));
        if (a->type == -RAY_MONTH)     return ray_month(wrap_add64(a->i64, v));
        if (a->type == -RAY_MINUTE)    return ray_minute(wrap_add64(a->i64, v));
        if (a->type == -RAY_SECOND)    return ray_second(wrap_add64(a->i64, v));
        if (a->type == -RAY_TIMESPAN)  return ray_timespan(wrap_add64(a->i64, v));
        if (a->type == -RAY_TIMESTAMP) return ray_timestamp(wrap_add64(a->i64, v));
    }
    if (is_numeric(a) && a->type != -RAY_F64 && is_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(b->type);

        int64_t v = as_i64(a);
        if (b->type == -RAY_DATE)      return ray_date(wrap_add64(b->i64, v));
        if (b->type == -RAY_TIME)      return ray_time(wrap_add64(b->i64, v));
        if (b->type == -RAY_MONTH)     return ray_month(wrap_add64(b->i64, v));
        if (b->type == -RAY_MINUTE)    return ray_minute(wrap_add64(b->i64, v));
        if (b->type == -RAY_SECOND)    return ray_second(wrap_add64(b->i64, v));
        if (b->type == -RAY_TIMESPAN)  return ray_timespan(wrap_add64(b->i64, v));
        if (b->type == -RAY_TIMESTAMP) return ray_timestamp(wrap_add64(b->i64, v));
    }
    /* Reject float + temporal */
    if ((a->type == -RAY_F64 && is_temporal(b)) || (is_temporal(a) && b->type == -RAY_F64))
        return ray_error("type", "add: temporal arithmetic requires integer offsets, got %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* Reject null_numeric + temporal (for null floats etc) */
    if (is_numeric(a) && RAY_ATOM_IS_NULL(a) && is_temporal(b))
        return ray_error("type", "add: unsupported temporal operand combination, got %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    if (is_temporal(a) && is_numeric(b) && RAY_ATOM_IS_NULL(b))
        return ray_error("type", "add: unsupported temporal operand combination, got %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* DATE + TIME → TIMESTAMP */
    if (a->type == -RAY_DATE && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(wrap_add64(wrap_mul64(a->i64, 86400000000000LL), b->i64 * 1000000LL));
    }
    if (a->type == -RAY_TIME && b->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(wrap_add64(wrap_mul64(b->i64, 86400000000000LL), a->i64 * 1000000LL));
    }
    /* TIME + TIME → TIME */
    if (a->type == -RAY_TIME && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIME);
        return ray_time(a->i64 + b->i64);
    }
    /* TIME + TIMESTAMP → TIMESTAMP (add ms as ns) */
    if (a->type == -RAY_TIME && b->type == -RAY_TIMESTAMP) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(wrap_add64(b->i64, a->i64 * 1000000LL));
    }
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(wrap_add64(a->i64, b->i64 * 1000000LL));
    }
    /* duration + duration -> the finer unit (ref/add.md temporal block).
     * TIME+TIME kept its dedicated arm above; every pair involving u/v/n
     * (and t with them) lands here. */
    if (duration_unit_ns(a->type) && duration_unit_ns(b->type))
        return duration_pair(a, b, 0);
    /* absolute (p/d/m) + duration -> timestamp, both orders (ref/add.md
     * rows/cols p,d,m x n,u,v).  DATE/TIMESTAMP + TIME pairs returned via
     * their dedicated arms above; month converts through civil days. */
    if (is_absolute_temporal(a) && duration_unit_ns(b->type)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(wrap_add64(temporal_as_ns(a), temporal_as_ns(b)));
    }
    if (duration_unit_ns(a->type) && is_absolute_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(wrap_add64(temporal_as_ns(a), temporal_as_ns(b)));
    }
    /* p + p -> n (ref/add.md row p col p; EXAMPLE-pinned by
     * basics/datatypes.md:163-166 `0p+ -0W 0Wp+1 -1` -> timespan). */
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_TIMESTAMP) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESPAN);
        return ray_timespan(wrap_add64(a->i64, b->i64));
    }

    if (!is_numeric(a) || !is_numeric(b) || arith_char_refused(a->type, b->type))
        return ray_error("type", "cannot add %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* Null propagation */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return null_for_promoted(a, b);
    if (is_float_op(a, b))
        return make_typed_float(promote_float_type(a->type, b->type),
                                as_f64(a) + as_f64(b));
    int8_t rt = promote_int_type(a, b);
    return make_typed_int(rt, wrap_add64(as_i64(a), as_i64(b)));
}

ray_t* ray_sub_fn(ray_t* a, ray_t* b) {
    if ((a && RAY_IS_PARTED(a->type)) || (b && RAY_IS_PARTED(b->type)))
        return atomic_map_binary_op(ray_sub_fn, OP_SUB, a, b);
    if (b->type == -RAY_CHARV) {   /* x-y is x+neg y, and ref/neg.md takes c to i */
        ray_t* bi = make_i32((int32_t)b->u8);
        ray_t* r = ray_sub_fn(a, bi);
        ray_release(bi);
        return r;
    }

    /* DATETIME (f64-backed, excluded from is_temporal) — see ray_add_fn.
     * z - z -> f64 days difference (basics/precision.md:274 `0=a-b`);
     * z - num -> z.  num - z: unpinned, falls through (mirrors date). */
    if (a->type == -RAY_DATETIME && b->type == -RAY_DATETIME)
        return make_f64(a->f64 - b->f64);          /* NaN nulls propagate */
    if (a->type == -RAY_DATETIME && is_numeric(b)) {
        if (RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_DATETIME);
        return ray_datetime(a->f64 - as_f64(b));
    }

    /* Temporal - int null propagation (both operands) */
    if (is_temporal(a) && is_numeric(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(a->type);
    }
    if (is_numeric(a) && is_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(b->type);
    }
    /* DATE - int → DATE */
    if (a->type == -RAY_DATE && is_numeric(b)) {
        return ray_date(wrap_sub64(a->i64, as_i64(b)));
    }
    /* DATE - DATE → i32 (days difference) */
    if (a->type == -RAY_DATE && b->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_I32);
        return ray_i32((int32_t)(a->i64 - b->i64));
    }
    /* MONTH - int → MONTH (basics/math.md: 2012.05 2012.06m-2 → 2012.03 2012.04m) */
    if (a->type == -RAY_MONTH && is_numeric(b)) {
        return ray_month(wrap_sub64(a->i64, as_i64(b)));
    }
    /* MONTH - MONTH → i32 (months difference, mirrors DATE-DATE) */
    if (a->type == -RAY_MONTH && b->type == -RAY_MONTH) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_I32);
        return ray_i32((int32_t)(a->i64 - b->i64));
    }
    /* DATE - TIME → TIMESTAMP */
    if (a->type == -RAY_DATE && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(wrap_sub64(wrap_mul64(a->i64, 86400000000000LL), b->i64 * 1000000LL));
    }
    /* TIME - int → TIME */
    if (a->type == -RAY_TIME && is_numeric(b)) {
        return ray_time(wrap_sub64(a->i64, as_i64(b)));
    }
    /* int - TIME → TIME (negative) */
    if (is_numeric(a) && b->type == -RAY_TIME) {
        return ray_time(wrap_sub64(as_i64(a), b->i64));
    }
    /* duration - int / int - duration → duration (ref/subtract.md rows
     * b..j × cols n,u,v — durations mirror the TIME arms above). */
    if (a->type == -RAY_MINUTE && is_numeric(b))   return ray_minute(wrap_sub64(a->i64, as_i64(b)));
    if (is_numeric(a) && b->type == -RAY_MINUTE)   return ray_minute(wrap_sub64(as_i64(a), b->i64));
    if (a->type == -RAY_SECOND && is_numeric(b))   return ray_second(wrap_sub64(a->i64, as_i64(b)));
    if (is_numeric(a) && b->type == -RAY_SECOND)   return ray_second(wrap_sub64(as_i64(a), b->i64));
    if (a->type == -RAY_TIMESPAN && is_numeric(b)) return ray_timespan(wrap_sub64(a->i64, as_i64(b)));
    if (is_numeric(a) && b->type == -RAY_TIMESPAN) return ray_timespan(wrap_sub64(as_i64(a), b->i64));
    /* TIME - TIME → TIME */
    if (a->type == -RAY_TIME && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIME);
        return ray_time(a->i64 - b->i64);
    }
    /* TIMESTAMP - int → TIMESTAMP */
    if (a->type == -RAY_TIMESTAMP && is_numeric(b)) {
        return ray_timestamp(wrap_sub64(a->i64, as_i64(b)));
    }
    /* TIMESTAMP - TIME → TIMESTAMP */
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(wrap_sub64(a->i64, b->i64 * 1000000LL));
    }
    /* TIMESTAMP - TIMESTAMP → TIMESPAN (kdb p-p->n, ref/subtract.md row p
     * col p; same ns payload as the old i64 result, only the tag changed —
     * the base rfl pin (test/rfl/arith/sub.rfl) is re-recorded with it). */
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_TIMESTAMP) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESPAN);
        return ray_timespan(wrap_sub64(a->i64, b->i64));
    }
    /* duration - duration -> the finer unit (TIME-TIME kept its arm). */
    if (duration_unit_ns(a->type) && duration_unit_ns(b->type))
        return duration_pair(a, b, 1);
    /* absolute - duration -> timestamp; duration - absolute -> timestamp
     * (ref/subtract.md rows p,d,m x cols n,u,v and rows n,u,v x cols p,m,d). */
    if (is_absolute_temporal(a) && duration_unit_ns(b->type)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(wrap_sub64(temporal_as_ns(a), temporal_as_ns(b)));
    }
    if (duration_unit_ns(a->type) && is_absolute_temporal(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(wrap_sub64(temporal_as_ns(a), temporal_as_ns(b)));
    }
    /* int - absolute keeps the absolute (ref/subtract.md rows b..e, cols p m d z); a float row is D672a's */
    if (is_numeric(a) && a->type != -RAY_F64) {
        if (is_absolute_temporal(b)) return make_typed_int(b->type, wrap_sub64(as_i64(a), as_i64(b)));
        if (b->type == -RAY_DATETIME)
            return RAY_ATOM_IS_NULL(a) ? ray_typed_null(-RAY_DATETIME) : ray_datetime(as_f64(a) - b->f64);
    }
    /* TIMESTAMP - DATE → error */
    if (a->type == -RAY_TIMESTAMP && b->type == -RAY_DATE)
        return ray_error("type", "subtract: cannot subtract %s from %s",
                         ray_type_name(b->type), ray_type_name(a->type));

    if (!is_numeric(a) || !is_numeric(b) || arith_char_refused(a->type, b->type))
        return ray_error("type", "cannot subtract %s and %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    /* Null propagation */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return null_for_promoted(a, b);
    if (is_float_op(a, b)) {
        double r = as_f64(a) - as_f64(b);
        if (r == 0.0) r = 0.0; /* normalize -0.0 to +0.0 */
        return make_typed_float(promote_float_type(a->type, b->type), r);
    }
    int8_t rt = promote_int_type(a, b);
    return make_typed_int(rt, wrap_sub64(as_i64(a), as_i64(b)));
}

/* ref/multiply.md:94-111, the temporal rows and columns (t temporal, n numeric; `*` commutes, so
 * one arm serves both orders).  An int-family or real scale keeps the temporal (n*j -> n, e*p -> p);
 * a float answers f, except that d*f and every z pair answer z.  Temporal x temporal is 'type. */
static ray_t* mul_temporal(ray_t* t, ray_t* n) {
    int f = n->type == -RAY_F64;
    int8_t rt = t->type == -RAY_DATETIME || (f && t->type == -RAY_DATE) ? -RAY_DATETIME
              : f ? -RAY_F64 : t->type;
    if (RAY_ATOM_IS_NULL(t) || RAY_ATOM_IS_NULL(n)) return ray_typed_null(rt);
    if (rt == -RAY_DATETIME) return ray_datetime(as_f64(t) * as_f64(n));
    if (rt == -RAY_F64)      return make_f64(as_f64(t) * as_f64(n));
    if (n->type == -RAY_F32) {   /* a real scales the count in its own lane, then narrows (as_i64's range guard) */
        double v = as_f64(t) * as_f64(n);
        return v >= -9223372036854775808.0 && v < 9223372036854775808.0 ? make_typed_int(rt, (int64_t)v)
                                                                         : ray_typed_null(rt);
    }
    return make_typed_int(rt, wrap_mul64(t->i64, as_i64(n)));
}

ray_t* ray_mul_fn(ray_t* a, ray_t* b) {
    if ((a && RAY_IS_PARTED(a->type)) || (b && RAY_IS_PARTED(b->type)))
        return atomic_map_binary_op(ray_mul_fn, OP_MUL, a, b);

    if (!is_numeric(a) || !is_numeric(b)) {
        if (is_numeric(a) && is_numeric_or_temporal(b)) return mul_temporal(b, a);
        if (is_numeric_or_temporal(a) && is_numeric(b)) return mul_temporal(a, b);
    }
    if (!is_numeric(a) || !is_numeric(b) || arith_char_refused(a->type, b->type))
        return ray_error("type", "cannot multiply %s and %s", ray_type_name(a->type), ray_type_name(b->type));
    /* Null propagation */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) return null_for_promoted(a, b);
    if (is_float_op(a, b))
        return make_typed_float(promote_float_type(a->type, b->type),
                                as_f64(a) * as_f64(b));
    int8_t rt = promote_int_type(a, b);
    return make_typed_int(rt, wrap_mul64(as_i64(a), as_i64(b)));
}

/* ref/divide.md:13 and :91-111: the ratio of the UNDERLYING values as a float, for every numeric or
 * temporal pair — mixed units divide their raw counts (a minute is its minute count, a timespan its ns). */
ray_t* ray_div_fn(ray_t* a, ray_t* b) {
    if ((a && RAY_IS_PARTED(a->type)) || (b && RAY_IS_PARTED(b->type)))
        return atomic_map_binary_op(ray_div_fn, OP_DIV, a, b);
    if (!is_numeric_or_temporal(a) || !is_numeric_or_temporal(b))
        return ray_error("type", "cannot divide %s by %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
        return ray_typed_null(-RAY_F64);
    /* IEEE divide: 1%0 -> 0w, -1%0 -> -0w, 0%0 -> 0n (live-infinity model). */
    return make_f64(as_f64(a) / as_f64(b));
}

ray_t* ray_idiv_fn(ray_t* a, ray_t* b) {
    if (!is_numeric(a) || !is_numeric(b))
        return ray_error("type", "cannot div %s by %s",
                         ray_type_name(a->type), ray_type_name(b->type));
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
        return ray_typed_null(-RAY_I64);
    double bv = as_f64(b);
    if (bv == 0.0)
        return ray_typed_null(-RAY_I64);
    double q = floor(as_f64(a) / bv);
    if (q < (double)INT64_MIN || q > (double)INT64_MAX)
        return ray_typed_null(-RAY_I64);
    return make_i64((int64_t)q);
}

ray_t* ray_mod_fn(ray_t* a, ray_t* b) {
    if ((a && RAY_IS_PARTED(a->type)) || (b && RAY_IS_PARTED(b->type)))
        return atomic_map_binary_op(ray_mod_fn, OP_MOD, a, b);
    /* Temporal % numeric → temporal (same type as left operand) */
    if (is_temporal(a) && is_numeric(b)) {
        if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b))
            return ray_typed_null(a->type);
        int64_t bv;
        if (b->type == -RAY_F64) {
            double bvf = b->f64;
            if (bvf == 0.0)
                return ray_typed_null(a->type);
            bv = (int64_t)bvf;
        } else {
            bv = as_i64(b);
        }
        if (bv == 0)
            return ray_typed_null(a->type);

        int64_t av = a->i64;
        int64_t q = av / bv;
        if ((av ^ bv) < 0 && q * bv != av) q--;
        int64_t result = wrap_sub64(av, wrap_mul64(bv, q));
        if (a->type == -RAY_TIME)      return ray_time(result);
        if (a->type == -RAY_DATE)      return ray_date(result);
        if (a->type == -RAY_MONTH)     return ray_month(result);
        if (a->type == -RAY_MINUTE)    return ray_minute(result);
        if (a->type == -RAY_SECOND)    return ray_second(result);
        if (a->type == -RAY_TIMESPAN)  return ray_timespan(result);
        return ray_timestamp(result);
    }
    if (!is_numeric(a) || !is_numeric(b) || arith_char_refused(a->type == -RAY_F32 ? -RAY_F64 : a->type, b->type))
        return ray_error("type", "cannot mod %s by %s",
                         ray_type_name(a->type), ray_type_name(b->type));

    /* u8: unsigned byte modulo, no null sentinel — mod by 0 returns 0 */
    if (b->type == -RAY_BYTE_ONLY) {
        uint8_t bv = b->u8;
        if (bv == 0) return make_u8(0);
        return make_u8((uint8_t)((uint8_t)as_i64(a) % bv));
    }
    if (a->type == -RAY_BYTE_ONLY) {
        /* a is u8 but b is not u8 — treat as integer, result follows b's type */
    }

    /* Null propagation and division by zero: null type follows RIGHT operand.
     * ref/mod.md's grid is asymmetric, so mod cannot share promote_float_type:
     * row `e` is f (a real LEFT operand yields float), while an int row with
     * col `e` is e — which "follows RIGHT" already gives. */
    if (RAY_ATOM_IS_NULL(a) || RAY_ATOM_IS_NULL(b)) {
        int8_t rt = (b->type == -RAY_F64 || a->type == -RAY_F64 ||
                     a->type == -RAY_F32) ? -RAY_F64 : b->type;
        return ray_typed_null(rt);
    }

    /* Float modulo: result = a - b * floor(a/b), type follows RIGHT or f64 */
    if (is_float_op(a, b)) {
        double av = as_f64(a), bv = as_f64(b);
        if (bv == 0.0) {
            int8_t rt = (b->type == -RAY_F64 || a->type == -RAY_F64 ||
                         a->type == -RAY_F32) ? -RAY_F64 : b->type;
            return ray_typed_null(rt);
        }
        double result = av - bv * floor(av / bv);
        /* Snap tiny residual to 0 */
        if (fabs(result) < 1e-12 || fabs(result - fabs(bv)) < 1e-12) result = bv > 0 ? 0.0 : -0.0;
        if (b->type == -RAY_F64 || a->type == -RAY_F64 || a->type == -RAY_F32)
            return make_f64(result);
        if (b->type == -RAY_F32) return make_typed_float(-RAY_F32, result);
        if (b->type == -RAY_I32) return make_i32((int32_t)(int64_t)result);
        if (b->type == -RAY_I16) return make_i16((int16_t)(int64_t)result);
        return make_i64((int64_t)result);
    }

    /* Integer modulo: result = a - b * floor(a/b), sign follows b (divisor) */
    int64_t av = as_i64(a), bv = as_i64(b);
    if (bv == 0)
        return ray_typed_null(b->type);

    int64_t q = av / bv;
    if ((av ^ bv) < 0 && q * bv != av) q--;  /* floor division */
    int64_t result = wrap_sub64(av, wrap_mul64(bv, q));
    /* Result type follows RIGHT operand */
    if (b->type == -RAY_I32) return make_i32((int32_t)result);
    if (b->type == -RAY_I16) return make_i16((int16_t)result);
    if (b->type == -RAY_BYTE_ONLY) return make_u8((uint8_t)result);
    return make_i64(result);
}

ray_t* ray_neg_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    /* ref/neg.md domain `e` -> range `e`; f64 and f32 atoms share the f64 slot. */
    if (x->type == -RAY_F64 || x->type == -RAY_F32)
        return make_typed_float(x->type, -x->f64);
    /* INT_MIN is the lone overflow case for signed negation: -INT_MIN
     * doesn't fit in the same width.  By convention, surface this
     * as a typed null of the same width — preserving type, avoiding UB,
     * and giving the caller a `nil?`-detectable signal that overflow
     * happened.  Consistent with how `(neg 0Ni) → 0Ni` propagates. */
    if (x->type == -RAY_I64) {
        if (RAY_UNLIKELY(x->i64 == INT64_MIN)) return ray_typed_null(-RAY_I64);
        return make_i64(-x->i64);
    }
    if (x->type == -RAY_I32) {
        if (RAY_UNLIKELY(x->i32 == INT32_MIN)) return ray_typed_null(-RAY_I32);
        return make_i32(-x->i32);
    }
    if (x->type == -RAY_I16) {
        if (RAY_UNLIKELY(x->i16 == INT16_MIN)) return ray_typed_null(-RAY_I16);
        return make_i16(-x->i16);
    }
    return ray_error("type", "negate: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* round: round to nearest integer (ties go away from zero), returns f64 */
ray_t* ray_round_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (x->type == -RAY_F64) return make_f64(round(x->f64));
    if (is_numeric(x)) return make_f64(round(as_f64(x)));
    return ray_error("type", "round: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* floor: round toward -inf, returns f64 for f64, identity for int */
ray_t* ray_floor_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    if (x->type == -RAY_F64) return make_f64(floor(x->f64));
    if (is_numeric(x)) { ray_retain(x); return x; }
    return ray_error("type", "floor: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* ceil: round toward +inf, returns f64 for f64, identity for int */
ray_t* ray_ceil_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    if (x->type == -RAY_F64) return make_f64(ceil(x->f64));
    if (is_numeric(x)) { ray_retain(x); return x; }
    return ray_error("type", "ceil: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* abs: absolute value, preserves type.  INT_MIN has no representable
 * positive in the same width — return a typed null instead (same
 * convention as `neg`).  Stops `(abs -32768h) → -32768h` (negative
 * result from abs!) and `(abs INT_MIN)` UB simultaneously. */
ray_t* ray_abs_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    /* ref/abs.md domain `e` -> range `e`. */
    if (x->type == -RAY_F64 || x->type == -RAY_F32)
        return make_typed_float(x->type, fabs(x->f64));
    if (x->type == -RAY_I64) {
        if (RAY_UNLIKELY(x->i64 == INT64_MIN)) return ray_typed_null(-RAY_I64);
        return make_i64(x->i64 < 0 ? -x->i64 : x->i64);
    }
    if (x->type == -RAY_I32) {
        if (RAY_UNLIKELY(x->i32 == INT32_MIN)) return ray_typed_null(-RAY_I32);
        return make_i32(x->i32 < 0 ? -x->i32 : x->i32);
    }
    if (x->type == -RAY_I16) {
        if (RAY_UNLIKELY(x->i16 == INT16_MIN)) return ray_typed_null(-RAY_I16);
        return make_i16(x->i16 < 0 ? -x->i16 : x->i16);
    }
    if (x->type == -RAY_CHARV) return make_i32(x->u8);   /* ref/abs.md range c -> i */
    return ray_error("type", "abs: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* sqrt: square root, returns f64 */
ray_t* ray_sqrt_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (x->type == -RAY_F64) return make_f64(sqrt(x->f64));
    if (is_numeric(x)) return make_f64(sqrt(as_f64(x)));
    return ray_error("type", "sqrt: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* log: natural logarithm, returns f64 */
ray_t* ray_log_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (x->type == -RAY_F64) return make_f64(log(x->f64));
    if (is_numeric(x)) return make_f64(log(as_f64(x)));
    return ray_error("type", "log: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* exp: e^x, returns f64 */
ray_t* ray_exp_fn(ray_t* x) {
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (x->type == -RAY_F64) return make_f64(exp(x->f64));
    if (is_numeric(x)) return make_f64(exp(as_f64(x)));
    return ray_error("type", "exp: expects a numeric argument, got %s", ray_type_name(x->type));
}

/* pow: x raised to y, returns f64.
 *
 * Atomic binary — broadcasts over numeric vectors via the same
 * RAY_FN_ATOMIC dispatch the other binary atomic ops use.  Result is
 * always F64; integer bases with integer exponents still go through
 * libm pow() so semantics match polars/numpy for fractional exponents
 * (e.g. (pow 2 0.5) → 1.41…).
 *
 * Null propagation: either operand null → typed F64 null. */
ray_t* ray_pow_fn(ray_t* x, ray_t* y) {
    if (RAY_ATOM_IS_NULL(x) || RAY_ATOM_IS_NULL(y))
        return ray_typed_null(-RAY_F64);
    if (!is_numeric(x) || !is_numeric(y))
        return ray_error("type", "pow: expects numeric base and exponent, got %s and %s",
                         ray_type_name(x->type), ray_type_name(y->type));
    return make_f64(pow(as_f64(x), as_f64(y)));
}

/* (xbar col bucket) — time/value bucketing: floor(col/bucket)*bucket */
/* Parallel inner loops for ray_xbar_fn fast path.  Dispatch one task per
 * morsel range so 5M-row temporal columns scale across the worker pool
 * (Q43's xbar was ~6ms serial, ~0.5ms with 28 workers). */
typedef struct {
    int8_t out_type;
    const void* in;
    void* out;
    int64_t b;
    int pow2;
} xbar_par_ctx_t;

static void xbar_par_fn(void* vctx, uint32_t worker_id,
                        int64_t start, int64_t end) {
    (void)worker_id;
    xbar_par_ctx_t* c = (xbar_par_ctx_t*)vctx;
    int64_t b = c->b;
    if (c->out_type == RAY_I64 || RAY_IS_TEMPORAL64(c->out_type)) {
        const int64_t* in = (const int64_t*)c->in;
        int64_t* o = (int64_t*)c->out;
        if (c->pow2) {
            int64_t mask = ~(b - 1);
            for (int64_t i = start; i < end; i++) o[i] = in[i] & mask;
        } else {
            for (int64_t i = start; i < end; i++) {
                int64_t a = in[i];
                int64_t q = a / b;
                if ((a ^ b) < 0 && q * b != a) q--;
                o[i] = q * b;
            }
        }
    } else if (c->out_type == RAY_I32 || RAY_IS_TEMPORAL32(c->out_type)) {
        const int32_t* in = (const int32_t*)c->in;
        int32_t* o = (int32_t*)c->out;
        int32_t b32 = (int32_t)b;
        if (c->pow2) {
            int32_t mask = (int32_t)~((uint32_t)b32 - 1);
            for (int64_t i = start; i < end; i++) o[i] = in[i] & mask;
        } else {
            for (int64_t i = start; i < end; i++) {
                int32_t a = in[i];
                int64_t q = (int64_t)a / b32;
                if ((a ^ b32) < 0 && q * b32 != a) q--;
                o[i] = (int32_t)(q * b32);
            }
        }
    } else { /* RAY_I16 */
        const int16_t* in = (const int16_t*)c->in;
        int16_t* o = (int16_t*)c->out;
        int16_t b16 = (int16_t)b;
        for (int64_t i = start; i < end; i++) {
            int16_t a = in[i];
            int16_t q = a / b16;
            if ((a ^ b16) < 0 && q * b16 != a) q--;
            o[i] = q * b16;
        }
    }
}

ray_t* ray_xbar_fn(ray_t* col, ray_t* bucket) {
    /* Vectorised fast path for `(xbar VEC scalar_int)` on integer or
     * temporal columns.  The generic atomic_map_binary path was
     * allocating one ray_t* atom per row and calling ray_xbar_fn
     * recursively — at 5M rows this dominates (≥100 ms).  A direct
     * tight loop computes floor-div + multiply per element with no
     * allocations.  When the bucket is a power of two we lower the
     * divide further to mask + arithmetic.  Parallelised across the
     * worker pool for large columns.
     *
     * Short-circuited only when both bucket and col are well-typed;
     * everything else falls through to the recursive
     * atomic_map_binary path. */
    if (col && ray_is_vec(col) && bucket && ray_is_atom(bucket) &&
        (bucket->type == -RAY_I64 || bucket->type == -RAY_I32 ||
         bucket->type == -RAY_I16) &&
        (col->type == RAY_I64 || col->type == RAY_I32 ||
         col->type == RAY_I16 || col->type == RAY_TIMESTAMP ||
         RAY_IS_TEMPORAL32(col->type)) &&
        !RAY_ATOM_IS_NULL(bucket)) {
        int64_t b = bucket->i64;
        if (b == 0) return ray_error("domain", "xbar: bucket size must be non-zero");
        int64_t n = col->len;
        ray_t* out = ray_vec_new(col->type, n);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        out->len = n;

        int8_t out_type = col->type;
        int pow2 = 0;
        if (out_type == RAY_I64 || RAY_IS_TEMPORAL64(out_type)) {
            pow2 = (b > 0 && (b & (b - 1)) == 0);
        } else if (out_type == RAY_I32 || RAY_IS_TEMPORAL32(out_type)) {
            int32_t b32 = (int32_t)b;
            pow2 = (b32 > 0 && ((uint32_t)b32 & ((uint32_t)b32 - 1)) == 0);
        }

        ray_pool_t* pool = ray_pool_get();
        if (pool && n >= 200000 && ray_pool_total_workers(pool) >= 2) {
            xbar_par_ctx_t ctx = {
                .out_type = out_type,
                .in       = ray_data(col),
                .out      = ray_data(out),
                .b        = b,
                .pow2     = pow2,
            };
            ray_pool_dispatch(pool, xbar_par_fn, &ctx, n);
        } else {
            xbar_par_ctx_t ctx = {
                .out_type = out_type,
                .in       = ray_data(col),
                .out      = ray_data(out),
                .b        = b,
                .pow2     = pow2,
            };
            xbar_par_fn(&ctx, 0, 0, n);
        }

        /* Propagate nulls if present.  Walk per-element via
         * ray_vec_is_null (sentinel-based). */
        if (col->attrs & RAY_ATTR_HAS_NULLS) {
            for (int64_t i = 0; i < n; i++)
                if (ray_vec_is_null(col, i))
                    ray_vec_set_null(out, i, true);
        }
        return out;
    }

    /* Recursive unwrap for nested collections (list of vectors) */
    if (is_collection(col) || is_collection(bucket))
        return atomic_map_binary(ray_xbar_fn, col, bucket);
    /* Both are integer types (i64, i32, i16) → integer xbar */
    if (is_numeric(col) && is_numeric(bucket) && !is_float_op(col, bucket)) {
        int64_t a = as_i64(col), b = as_i64(bucket);
        if (b == 0 || RAY_ATOM_IS_NULL(col) || RAY_ATOM_IS_NULL(bucket))
            return ray_error("domain", "xbar: bucket size must be non-zero and operands non-null");
        int64_t q = a / b;
        if ((a ^ b) < 0 && q * b != a) q--;
        int64_t result = q * b;
        /* Result type follows the wider of the two operands */
        if (col->type == -RAY_I32 && bucket->type == -RAY_I32) return make_i32((int32_t)result);
        if (col->type == -RAY_I16 && bucket->type == -RAY_I16) return make_i16((int16_t)result);
        return make_i64(result);
    }
    /* Float path: either operand is f64 */
    if (is_numeric(col) && is_numeric(bucket)) {
        if (RAY_ATOM_IS_NULL(col) || RAY_ATOM_IS_NULL(bucket))
            return ray_error("domain", "xbar: operands must be non-null");
        double c = as_f64(col), b = as_f64(bucket);
        if (b == 0.0) return ray_error("domain", "xbar: bucket size must be non-zero");
        double fq = floor(c / b);
        return make_f64(fq * b);
    }
    /* Temporal xbar: col is temporal, bucket is integer or temporal (not float) */
    if (is_temporal(col) && (is_temporal(bucket) ||
        (is_numeric(bucket) && bucket->type != -RAY_F64))) {
        int64_t a = col->i64, b;
        if (is_temporal(bucket)) {
            b = bucket->i64;
            /* Cross-temporal conversion: TIME(ms) bucket on TIMESTAMP(ns) col */
            if (col->type == -RAY_TIMESTAMP && bucket->type == -RAY_TIME)
                b *= 1000000LL;
        } else {
            b = as_i64(bucket);
        }
        if (b == 0 || RAY_ATOM_IS_NULL(bucket)) return ray_error("domain", "xbar: bucket size must be non-zero and non-null");
        int64_t q = a / b;
        if ((a ^ b) < 0 && q * b != a) q--;
        int64_t result = q * b;
        if (col->type == -RAY_TIME) return ray_time(result);
        if (col->type == -RAY_DATE) return ray_date(result);
        if (col->type == -RAY_MONTH) return ray_month(result);
        if (col->type == -RAY_MINUTE) return ray_minute(result);
        if (col->type == -RAY_SECOND) return ray_second(result);
        if (col->type == -RAY_TIMESPAN) return ray_timespan(result);
        return ray_timestamp(result);
    }
    return ray_error("type", "xbar: unsupported operand types, got %s and %s",
                     ray_type_name(col->type), ray_type_name(bucket->type));
}
