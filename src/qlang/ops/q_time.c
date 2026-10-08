/* q_time.c — ltime/gtime (ref/gtime.md): a timestamp or datetime moved between UTC and local time at its own instant */
#include "qlang/q_count.h"
#include "qlang/q_registry_internal.h"
#include "qlang/base/q_err.h"
#include "qlang/q_dotz.h"   /* q_dotz_utc_offset_at */
#include "lang/cal.h"       /* datetime_to_ns / ts_days_floor / ts_ns_in_day */

/* A local reading x names the UTC instant x - off, so gtime looks the offset up a second time, at that instant. */
static int64_t shift_secs(int64_t ns, int to_local) {
    int64_t secs = ts_days_floor(ns) * 86400 + ts_ns_in_day(ns) / 1000000000;
    int64_t off  = q_dotz_utc_offset_at(secs);
    if (to_local) return off;
    uint64_t back = (uint64_t)q_dotz_utc_offset_at((int64_t)((uint64_t)secs - (uint64_t)off));   /* a wild `\o` wraps */
    return (int64_t)(0 - back);
}

static ray_t* convert(ray_t* x, int to_local) {
    if (!x || (x->type != -RAY_TIMESTAMP && x->type != -RAY_DATETIME)) return q_err(QE_TYPE);
    if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
    if (x->type == -RAY_DATETIME)
        return ray_datetime(x->f64 + (double)shift_secs(datetime_to_ns(x->f64), to_local) / 86400);
    if (x->i64 == INT64_MAX || x->i64 == -INT64_MAX) { ray_retain(x); return x; }
    return ray_timestamp((int64_t)((uint64_t)x->i64 + (uint64_t)shift_secs(x->i64, to_local) * 1000000000));
}

ray_t* q_ltime_wrap(ray_t* x) { return convert(x, 1); }
ray_t* q_gtime_wrap(ray_t* x) { return convert(x, 0); }
