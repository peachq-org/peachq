/* ops/q_rand.c — the `?` roll / deal / permute / generate domain, drawing
 * from ONE libc rand() stream, plus q_rand_seed — the seed home (startup and
 * `\S` both funnel here).
 *
 * Split from ops/q_list.c (corridor pass, 2026-07-22) — pure function moves;
 * the shared internal surface lives in q_registry_internal.h. */
#define _POSIX_C_SOURCE 200809L
#define Q_OPS_ENV_GRANDFATHER /* grandfathered 2026-07-23: 2 env uses (seed state) — q-index PR audit */
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_err.h"
#include "qlang/ops/q_dollar.h" /* q_dollar_cast — THE conversion home */
#include "lang/env.h"      /* ray_env_get — env_call1 (guid roll/deal) */
#include "lang/internal.h" /* ray_rand_fn, RAY_IS_TEMPORAL64, ray_error */
#include "table/sym.h"     /* ray_sym_intern, RAY_SYM_W64 — sym generate */
#include <math.h>          /* nextafter/nextafterf — float roll clamp */
#include <stdint.h>        /* INT32/64 MIN/MAX */
#include <stdlib.h>        /* rand, srand, malloc, free */

/* The libc rand() stream contract (moved from q_sys.c with the seed home):
 * kdb re-initializes its rng to a CONSTANT seed at startup (-314159i,
 * basics/syscmds.md) so scripts using Roll/Deal/rand repeat.  ALL openq
 * randomness (`?` roll/deal/permute + generate arms, `rand`) funnels through
 * libc rand(), so srand IS the whole contract — except the guid generator
 * (src/ops/system.c xorshift64*), which seeds lazily from rand() per thread:
 * `\S` makes guid sequences reproducible only if set before the thread's
 * first guid use (recorded caveat, not fixed here). */
void q_rand_seed(int64_t n) {
    srand((unsigned)n);
}

/* ===== `?` GENERATE arms (ref/deal.md "Generate") ===========================
 * All arms draw from libc rand() (one stream — `\S n` re-seeds them all);
 * each right-operand form yields a result of y's type per the docs table. */

/* n?`m — n symbols of m chars each from "abcdefghijklmnop"; m is the numeric
 * symbol's NAME (`2 -> 2), 1<=m<=8 -> 'length otherwise (unpinned error
 * class; chosen to mirror the docs' n<=8 bound).  distinct (deal) draws by
 * generate-and-retry with a linear scan over accepted ids — fine at the
 * 16^m<=4.3e9 space for practical n; n>16^m is 'length. */
static ray_t* gen_syms(int64_t n, ray_t* ysym, int distinct) {
    static const char letters[] = "abcdefghijklmnop";
    ray_t* nm = ray_sym_str(ysym->i64);
    if (!nm || RAY_IS_ERR(nm))
        return nm ? nm : q_err(QE_TYPE);
    const char* s = ray_str_ptr(nm);
    size_t sl = ray_str_len(nm);
    int64_t m = 0;
    int numeric = sl > 0 && sl <= 2;
    for (size_t i = 0; numeric && i < sl; i++) {
        if (s[i] < '0' || s[i] > '9') numeric = 0;
        else m = m * 10 + (s[i] - '0');
    }
    ray_release(nm);
    if (!numeric)
        return q_err(QE_TYPE);
    if (m < 1 || m > 8)
        return q_err(QE_LENGTH);
    if (distinct) {
        int64_t space = 1;
        for (int64_t j = 0; j < m; j++) space *= 16;
        if (n > space)
            return q_err(QE_LENGTH);
    }
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    for (int64_t i = 0; i < n; i++) {
        char buf[8];
        int64_t id;
        for (;;) {
            for (int64_t j = 0; j < m; j++) buf[j] = letters[rand() % 16];
            id = ray_sym_intern(buf, (size_t)m);
            if (!distinct) break;
            const int64_t* got = (const int64_t*)ray_data(out);
            int dup = 0;
            for (int64_t k = 0; k < i; k++) if (got[k] == id) { dup = 1; break; }
            if (!dup) break;
        }
        out = ray_vec_append(out, &id);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    }
    return out;
}

/* n?f — uniform floats in [0,y); result is y's type (F64 float / F32 real).
 * 62 random bits give the fraction; a rounding hit at the top is clamped
 * back below y so the [0,y) contract holds exactly. */
static ray_t* gen_floats(int64_t n, ray_t* y) {
    double fy;
    int f32 = (y->type == -RAY_F32);
    if (!q_type_strict_f64(y, &fy) || fy != fy || fy < 0)
        return q_err(QE_DOMAIN);
    ray_t* out = ray_vec_new(f32 ? RAY_F32 : RAY_F64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    for (int64_t i = 0; i < n; i++) {
        uint64_t u = ((uint64_t)rand() << 31) | (uint64_t)rand();
        double v = fy * ((double)u / 4611686018427387904.0);   /* / 2^62 */
        if (f32) {
            float fv = (float)v;
            if (fv >= (float)fy && fy > 0) fv = nextafterf((float)fy, 0.0f);
            ((float*)ray_data(out))[i] = fv;
        } else {
            if (v >= fy && fy > 0) v = nextafter(fy, 0.0);
            ((double*)ray_data(out))[i] = v;
        }
    }
    return out;
}

/* n?0b — random booleans (01b). */
static ray_t* gen_bits(int64_t n) {
    ray_t* out = ray_vec_new(RAY_BOOL, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    for (int64_t i = 0; i < n; i++) ((bool*)ray_data(out))[i] = rand() & 1;
    return out;
}

/* n?0x0 — random bytes 0x00-0xff. */
static ray_t* gen_bytes(int64_t n) {
    ray_t* out = ray_vec_new(RAY_BYTE_ONLY, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    for (int64_t i = 0; i < n; i++)
        ((uint8_t*)ray_data(out))[i] = (uint8_t)(rand() & 0xFF);
    return out;
}

/* n?0 / n?0i — full-range longs/ints.  rand() yields 31 bits, so words are
 * composed from multiple calls.  The engine's sentinel values (0N=INT_MIN,
 * -0W=INT_MIN+1, 0W=INT_MAX) are never generated (rejection loop) — a roll
 * must not fabricate nulls/infinities (decision recorded in the plan). */
static ray_t* gen_longs(int64_t n) {
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int64_t v;
        do {
            uint64_t u = ((uint64_t)rand() << 33) |
                         ((uint64_t)rand() << 2)  |
                         ((uint64_t)rand() & 3);
            v = (int64_t)u;
        } while (v == INT64_MIN || v == INT64_MIN + 1 || v == INT64_MAX);
        d[i] = v;
    }
    return out;
}

static ray_t* gen_ints(int64_t n) {
    ray_t* out = ray_vec_new(RAY_I32, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int32_t* d = (int32_t*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int32_t v;
        do {
            uint32_t u = ((uint32_t)rand() << 1) | ((uint32_t)rand() & 1);
            v = (int32_t)u;
        } while (v == INT32_MIN || v == INT32_MIN + 1 || v == INT32_MAX);
        d[i] = v;
    }
    return out;
}

/* Uniform draw in [0,m), m>0 — a single rand()%m cannot reach past RAND_MAX
 * (31-bit glibc, 15-bit Windows CRT) and would fold a big roll (n?.z.p) onto
 * its start, so compose 63 bits from 15-bit chunks (the portable floor) and
 * reject draws past the last whole multiple of m (exact uniform, no modulo
 * bias; rejection odds < 1/2 per draw). */
static int64_t rand_below(int64_t m) {
    const uint64_t span = 1ULL << 63;
    const uint64_t limit = span - span % (uint64_t)m;
    uint64_t u;
    do {
        u = 0;
        for (int bits = 0; bits < 63; bits += 15)
            u = (u << 15) | ((uint64_t)rand() & 0x7FFF);
        u &= span - 1;
    } while (u >= limit);
    return (int64_t)(u % (uint64_t)m);
}

/* n?t — temporal roll, uniform on [0,y) over the backing payload (ref/deal.md:
 * "float, temporal >=0 -> 0 to y"; `4?2012.09m` is its transcript).  Draw an
 * i64 within the backing payload range, then re-tag through q_dollar_cast — THE
 * one conversion home (q_registry.h) — so the payload->temporal law stays
 * there.  y=0 degenerates
 * to all-zero items (the float row's y*0 behaviour); y<0 or null -> the
 * pinned float-bound class 'domain. */
static ray_t* gen_temporal(int64_t n, ray_t* y) {
    int8_t tag = (int8_t)-y->type;
    ray_t* v;
    if (RAY_IS_TEMPORALF(tag)) {              /* datetime rides the float roll
                                               * (q_type_strict_f64 reads its payload) */
        v = gen_floats(n, y);
    } else {
        int64_t m = RAY_IS_TEMPORAL64(tag) ? y->i64 : (int64_t)y->i32;
        if (m < 0) return q_err(QE_DOMAIN);   /* nulls are negative sentinels */
        v = ray_vec_new(RAY_I64, n > 0 ? n : 1);
        if (RAY_IS_ERR(v)) return v;
        v->len = n;
        int64_t* d = (int64_t*)ray_data(v);
        for (int64_t i = 0; i < n; i++) d[i] = m ? rand_below(m) : 0;
    }
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* r = q_dollar_cast(tag, v);
    ray_release(v);
    return r;
}

/* n?" " — chars drawn from .Q.a (ref/deal.md y-table row `" " -> .Q.a`).
 * Result is a RAY_STR atom = the string model's char list (provisional,
 * ARCHITECTURE.md). */
static ray_t* gen_chars(int64_t n) {
    char stackb[1024];
    char* b = (n < (int64_t)sizeof stackb) ? stackb : malloc((size_t)n + 1);
    if (!b) return q_err(QE_WSFULL);
    for (int64_t i = 0; i < n; i++) b[i] = (char)('a' + rand() % 26);
    ray_t* s = ray_str(b, (size_t)n);
    if (b != stackb) free(b);
    return s;
}

/* Deal n distinct values from [0,total) — partial Fisher-Yates over `til total`,
 * take the first n (kdb deal / permute; uses the same libc rand() the roll path
 * does).  n<=total required.  Result is an owned I64 vector. */
static ray_t* deal_indices(int64_t n, int64_t total) {
    if (n < 0) return q_err(QE_DOMAIN);
    if (n > total) return q_err(QE_LENGTH);
    ray_t* arr = ray_vec_new(RAY_I64, total > 0 ? total : 1);
    if (RAY_IS_ERR(arr)) return arr;
    arr->len = total;
    int64_t* a = (int64_t*)ray_data(arr);
    for (int64_t i = 0; i < total; i++) a[i] = i;
    for (int64_t i = 0; i < n; i++) {
        int64_t range = total - i;
        int64_t j = i + (int64_t)(rand() % range);
        int64_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
    arr->len = n;                          /* take first n (buffer already sized) */
    return arr;
}

/* Deal/permute n indices then gather them from the list y (collapsed). */
static ray_t* deal_pick(int64_t n, ray_t* y) {
    ray_t* idx = deal_indices(n, ray_len(y));
    if (!idx || RAY_IS_ERR(idx)) return idx;
    ray_t* out = ray_at_fn(y, idx);
    ray_release(idx);
    if (out && out->type == RAY_LIST) { ray_t* c = q_list_collapse(out); ray_release(out); return c; }
    return out;
}

/* Unary sibling of q_env_call2 (guid roll/deal routes through the audited
 * env `guid` value).  Borrowed arg; returns owned. */
static ray_t* env_call1(const char* nm, ray_t* a) {
    ray_t* f = ray_env_get(ray_sym_intern(nm, strlen(nm)));
    if (!f || f->type != RAY_UNARY)
        return q_err(QE_TYPE);
    return ((ray_unary_fn)(uintptr_t)f->i64)(a);
}

/* q `x?y` — roll / deal / pick / generate + the find dispatch (type-dispatch
 * on the operands).
 *   list ? y   -> find (q_list_find, ops/q_list.c — dict reverse lookup rides it)
 *   n ? int    -> roll: n randoms in [0,int)  (rayfall rand)
 *   n ? list   -> pick: n random indices gathered from the list
 *   -n ? m / 0N ? m -> deal / permute (deal_indices)
 *   generate arms (`m sym, float, temporal, " ", 0b, 0x0, 0, 0i, 0Ng) ->
 *   gen_* above.  Deferred cells (error, never a wrong answer): short y,
 *   non-" " char pick (string model), deal of 0/0i. */
ray_t* q_roll_wrap(ray_t* x, ray_t* y) {
    if (x && ((x->type == RAY_DICT && !q_type_is_keyed(x)) ||
              ray_is_vec(x) || x->type == RAY_LIST))
        return q_list_find(x, y);           /* find / dict reverse lookup */
    int64_t nx;
    if (!q_type_strict_i64(x, &nx)) return q_err(QE_TYPE);
    if (RAY_ATOM_IS_NULL(x)) {                  /* 0N ? y — permute all items */
        if (y && (y->type == -RAY_I64 || y->type == -RAY_I32)) {
            int64_t m = q_type_iatom_val(y);
            if (m < 0) return q_err(QE_TYPE);
            return deal_indices(m, m);
        }
        if (y && (ray_is_vec(y) || y->type == RAY_LIST)) return deal_pick(ray_len(y), y);
        return q_err(QE_NYI);
    }
    int deal = nx < 0;
    int64_t n = deal ? -nx : nx;
    if (y && (ray_is_vec(y) || y->type == RAY_LIST)) {
        if (deal) return deal_pick(n, y);     /* -n?list — deal, no replacement */
        /* n?list — pick.  Indices draw via the engine KERNEL directly
         * (ray_rand_fn), not the env name — the bootstrap shadow-rebinds root
         * `rand` to `.q.rand`. */
        ray_t* cnt = ray_i64(n);
        ray_t* len = ray_i64(ray_len(y));
        ray_t* idx = ray_rand_fn(cnt, len);
        ray_release(cnt);
        ray_release(len);
        if (!idx || RAY_IS_ERR(idx)) return idx;
        ray_t* out = ray_at_fn(y, idx);
        ray_release(idx);
        if (out && out->type == RAY_LIST) {
            ray_t* c = q_list_collapse(out);
            ray_release(out);
            return c;
        }
        return out;
    }
    if (y && y->type < 0) {
        /* ---- generate arms: ONE switch over the value band, total by
         * -Wswitch + -Werror (no `default:` — a missing tag refuses to
         * build; structural leftovers return AFTER the switch). ---- */
        switch ((ray_type_e)-y->type) {
        case RAY_LIST:                          /* dead arm: an atom tag is strictly negative */
            break;
        case RAY_BOOL:
            if (deal) return q_err(QE_TYPE);
            if (y->b8) return q_err(QE_NYI);   /* roll defined for 0b only */
            return gen_bits(n);               /* n?0b */
        case RAY_GUID: {                        /* n?0Ng / -n?0Ng — env guid.
             * Deal reuses the same generator: distinctness rests on the
             * 122-bit space (collisions negligible); kdb's process/time deal
             * seed nuance is NOT reproduced (recorded divergence). */
            if (!RAY_ATOM_IS_NULL(y))
                return q_err(QE_TYPE);   /* guid generate needs 0Ng */
            ray_t* cnt = ray_i64(n);
            ray_t* g = env_call1("guid", cnt);
            ray_release(cnt);
            return g;
        }
        case RAY_BYTE_ONLY:
            if (deal) return q_err(QE_TYPE);
            if (y->u8) return q_err(QE_NYI);   /* roll defined for 0x0 only */
            return gen_bytes(n);              /* n?0x0 */
        case RAY_I16:
            return q_err(QE_NYI);    /* short roll/deal deferred */
        case RAY_I32: case RAY_I64: {
            if (!RAY_ATOM_IS_NULL(y) && q_type_iatom_val(y) == 0) {  /* n?0 / n?0i full-range */
                if (deal) return q_err(QE_NYI);
                return (y->type == -RAY_I64) ? gen_longs(n) : gen_ints(n);
            }
            if (deal) {                         /* -n?m — deal, no replacement */
                int64_t m = q_type_iatom_val(y);
                if (m <= 0) return q_err(QE_DOMAIN);
                return deal_indices(n, m);
            }
            ray_t* cnt = ray_i64(n);            /* n?m — roll via the kernel */
            ray_t* r = ray_rand_fn(cnt, y);
            ray_release(cnt);
            return r;
        }
        case RAY_F32: case RAY_F64:
            if (deal) return q_err(QE_TYPE);
            return gen_floats(n, y);          /* n?f uniform [0,y) */
        case RAY_STR:                           /* legacy 1-char string blank */
            if (deal) return q_err(QE_TYPE);
            if (ray_str_len(y) == 1 && ray_str_ptr(y)[0] == ' ')
                return gen_chars(n);
            return q_err(QE_NYI);
        case RAY_CHARV:                         /* char atom: only `" "` has a
             * roll law (-> .Q.a); pick-from-string is a stage-2+ cell. */
            if (deal) return q_err(QE_TYPE);
            if (y->u8 == ' ') return q_str_charv_out(gen_chars(n));
            return q_err(QE_NYI);
        case RAY_SYM:
            return gen_syms(n, y, deal);      /* n?`m sym roll / deal */
        case RAY_TIMESTAMP: case RAY_MONTH: case RAY_DATE: case RAY_DATETIME:
        case RAY_TIMESPAN: case RAY_MINUTE: case RAY_SECOND: case RAY_TIME:
            /* the doc's "float, temporal" row is Roll-only -> deal is 'type
             * exactly as the float arm above */
            if (deal) return q_err(QE_TYPE);
            return gen_temporal(n, y);
        }
    }
    return q_err(QE_NYI);   /* structural atom y (lambda, ...): no roll law */
}
