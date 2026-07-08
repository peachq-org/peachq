/* q_dotz — see q_dotz.h.  The eval-time `.z.*` command-line resolver. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* clock_gettime / gmtime_r for the clock producers */
#endif
#include "qlang/q_dotz.h"
#include "lang/cal.h"          /* ymd_to_date — build-date -> q date for .z.k */
#include <rayforce.h>
#include <stdio.h>             /* snprintf / sscanf for the version producers */
#include <stdlib.h>            /* strtod */
#include <string.h>
#include <time.h>             /* clock_gettime / gmtime_r / mktime — .z clock family */

/* argv is process-lifetime (owned by main), so we cache only the pointers and
 * the script's position and MINT each `.z.*` value on demand.  These values
 * are immutable argv snapshots, cheap to build, and read rarely — caching them
 * as owned `ray_t*` would add lifecycle (init/destroy/retain) without benefit.
 * Adding a `.z.*` name = one Z_TAB row + a small producer; init/destroy and the
 * resolver are untouched. */
static int    g_argc       = 0;
static char** g_argv       = NULL;
static int    g_script_idx = -1;   /* argv index of the `*.q` script, or -1 */

static bool ends_with_dot_q(const char* s) {
    size_t n = strlen(s);
    return n >= 2 && s[n - 2] == '.' && s[n - 1] == 'q';
}

/* A value-consuming q flag: its FOLLOWING argv token is a value, not the
 * script.  WARNING — hand-duplicated from qmain.c's flag parse (-p/--port/
 * -u/-U): if you add a value-taking flag to qmain.c you MUST add it here too,
 * or its value will be mis-detected as the `*.q` script path (wrong .z.f/.z.x). */
static bool flag_takes_value(const char* s) {
    return strcmp(s, "-p") == 0 || strcmp(s, "--port") == 0 ||
           strcmp(s, "-u") == 0 || strcmp(s, "-U") == 0;
}

/* argv[lo..hi) as a q list of strings (empty list, not null, when lo==hi). */
static ray_t* strings_list(int lo, int hi) {
    int    n   = hi - lo;
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    for (int i = lo; i < hi; i++) {
        ray_t* s = ray_str(g_argv[i], strlen(g_argv[i]));
        out = ray_list_append(out, s);   /* append RETAINS */
        ray_release(s);
    }
    return out;
}

/* `.z.*` producers — each mints a FRESH owned ref (rc>=1), matching the
 * name-hook contract (the resolver returns an owned value or NULL). */
static ray_t* z_f(void) {   /* script file symbol; null sym when no script */
    const char* s = g_script_idx < 0 ? "" : g_argv[g_script_idx];
    return ray_sym(ray_sym_intern(s, strlen(s)));
}
static ray_t* z_x(void) {   /* args positioned AFTER the script (empty if none) */
    int lo = g_script_idx < 0 ? g_argc : g_script_idx + 1;
    return strings_list(lo, g_argc);
}
static ray_t* z_X(void) {   /* full raw argv, including the binary */
    return strings_list(0, g_argc);
}

/* peachq version surface — reads the SAME compile-time macros the Makefile
 * injects for .sys.build (RAY_VERSION_MAJOR/MINOR, RAYFORCE_BUILD_DATE). */
static ray_t* z_K(void) {   /* `.z.K` — version as a major.minor float (kdb .z.K) */
    char buf[32];
    snprintf(buf, sizeof buf, "%d.%d", RAY_VERSION_MAJOR, RAY_VERSION_MINOR);
    return ray_f64(strtod(buf, NULL));
}
static ray_t* z_k(void) {   /* `.z.k` — build/release date (kdb .z.k) */
    int y = RAY_DATE_EPOCH, m = 1, d = 1;
#ifdef RAYFORCE_BUILD_DATE
    if (sscanf(RAYFORCE_BUILD_DATE, "%d-%d-%d", &y, &m, &d) != 3) {
        y = RAY_DATE_EPOCH; m = 1; d = 1;
    }
#endif
    return ray_date(ymd_to_date(y, m, d));
}

/* ---- .z clock family (kdb .z.p/.z.P … lowercase=UTC, uppercase=local) -------
 * Each producer RE-READS the system clock on every reference — q's timing idiom
 * `a:.z.t; costly[]; .z.t-a` requires re-sampling, never a per-block cache — and
 * at NANOSECOND resolution (clock_gettime), so sub-second deltas are visible
 * (the engine's second-granularity (date)/(time)/(timestamp) clock fns can't do
 * that).  All ten derive from one `z_now_ns` read. */
#define RAY_EPOCH_UNIX_SECS 946684800LL          /* 2000.01.01 00:00:00 UTC, unix secs */
#define RAY_NS_PER_DAY      86400000000000LL

/* Current time as nanoseconds since the rayforce epoch (2000.01.01), in UTC
 * (local=0) or local wall-clock (local=1).  Local offset is derived portably
 * (no tm_gmtoff): mktime of the UTC fields interprets them as local, so the
 * signed difference from the real instant IS the east-of-UTC offset. */
static int64_t z_now_ns(int local) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t unix_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (local) {
        time_t s = ts.tv_sec;
        struct tm g;
        gmtime_r(&s, &g);
        g.tm_isdst = -1;
        int64_t off = (int64_t)(s - mktime(&g));   /* seconds east of UTC (incl DST) */
        unix_ns += off * 1000000000LL;
    }
    return unix_ns - RAY_EPOCH_UNIX_SECS * 1000000000LL;
}

static ray_t* z_p(void) { return ray_timestamp(z_now_ns(0)); }                 /* .z.p UTC timestamp */
static ray_t* z_P(void) { return ray_timestamp(z_now_ns(1)); }                 /* .z.P local timestamp */
static ray_t* z_d(void) { return ray_date((int64_t)(z_now_ns(0) / RAY_NS_PER_DAY)); } /* .z.d UTC date */
static ray_t* z_D(void) { return ray_date((int64_t)(z_now_ns(1) / RAY_NS_PER_DAY)); } /* .z.D local date */
static ray_t* z_t(void) { return ray_time((z_now_ns(0) % RAY_NS_PER_DAY) / 1000000LL); } /* .z.t UTC time (ms) */
static ray_t* z_T(void) { return ray_time((z_now_ns(1) % RAY_NS_PER_DAY) / 1000000LL); } /* .z.T local time (ms) */
static ray_t* z_n(void) { return ray_timespan(z_now_ns(0) % RAY_NS_PER_DAY); } /* .z.n UTC timespan since midnight */
static ray_t* z_N(void) { return ray_timespan(z_now_ns(1) % RAY_NS_PER_DAY); } /* .z.N local timespan since midnight */
static ray_t* z_z(void) { return ray_datetime((double)z_now_ns(0) / (double)RAY_NS_PER_DAY); } /* .z.z UTC datetime */
static ray_t* z_Z(void) { return ray_datetime((double)z_now_ns(1) / (double)RAY_NS_PER_DAY); } /* .z.Z local datetime */

static const struct { const char* name; uint8_t len; ray_t* (*make)(void); }
Z_TAB[] = {
    { ".z.f", 4, z_f },
    { ".z.x", 4, z_x },
    { ".z.X", 4, z_X },
    { ".z.K", 4, z_K },
    { ".z.k", 4, z_k },
    { ".z.p", 4, z_p },
    { ".z.P", 4, z_P },
    { ".z.d", 4, z_d },
    { ".z.D", 4, z_D },
    { ".z.t", 4, z_t },
    { ".z.T", 4, z_T },
    { ".z.n", 4, z_n },
    { ".z.N", 4, z_N },
    { ".z.z", 4, z_z },
    { ".z.Z", 4, z_Z },
};

void q_dotz_init(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;

    /* Locate the positional `*.q` script: the first token ending in ".q" that
     * is not the value of a value-consuming flag.  (`.z.x` = every token AFTER
     * it; kdb also drops its own recognized options — position-based is
     * kdb-true for the non-flag args the increment-1 tests pass.) */
    g_script_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && flag_takes_value(argv[i - 1])) continue;  /* skip flag value */
        if (ends_with_dot_q(argv[i])) { g_script_idx = i; break; }
    }
}

const char* q_dotz_script_path(void) {
    return g_script_idx < 0 ? NULL : g_argv[g_script_idx];
}

ray_t* q_dotz_resolve(int64_t sym_id) {
    ray_t* name = ray_sym_str(sym_id);   /* BORROWED: cached arena string atom
                                          * (RAY_ATTR_ARENA); the ray_release
                                          * below is a no-op — do not rely on it
                                          * or "fix a leak" here */
    if (!name) return NULL;
    const char* p = ray_str_ptr(name);
    size_t      n = ray_str_len(name);

    ray_t* out = NULL;
    for (size_t i = 0; i < sizeof Z_TAB / sizeof *Z_TAB; i++)
        if (n == Z_TAB[i].len && memcmp(p, Z_TAB[i].name, n) == 0) {
            out = Z_TAB[i].make();   /* already owned (rc>=1) */
            break;
        }

    ray_release(name);
    return out;
}

void q_dotz_destroy(void) {
    g_argc       = 0;
    g_argv       = NULL;
    g_script_idx = -1;
}
