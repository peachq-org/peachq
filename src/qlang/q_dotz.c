/* q_dotz — see q_dotz.h.  The eval-time `.z.*` command-line resolver. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* clock_gettime / gmtime_r for the clock producers */
#endif
#include "qlang/q_dotz.h"
#include "qlang/q_sys.h"       /* q_sys_timer_active — stopped-timer no-op guard */
#include "qlang/q_fmt.h"       /* q_console_str/_reset — drain .z.ts show/0N! output */
#include "lang/cal.h"          /* ymd_to_date — build-date -> q date for .z.k */
#include "lang/env.h"          /* ray_sym_ipc_hook / ray_env_get / ray_fn_unary */
#include "lang/eval.h"         /* RAY_FN_NONE — .z.ts timer thunk attrs */
#include "lang/internal.h"     /* call_fn1 — .z.ts timer dispatch */
#include "core/ipc.h"          /* ray_ipc_current_handle / ray_ipc_fd_of_handle — .z.w */
#include <rayforce.h>
#include <stdio.h>             /* snprintf / sscanf for the version producers */
#include <stdlib.h>            /* strtod / getenv */
#include <string.h>
#include <time.h>             /* clock_gettime / gmtime_r / mktime — .z clock family */
#include <unistd.h>          /* getpid / gethostname / getuid — .z.i/.z.h/.z.u */
#ifndef RAY_OS_WINDOWS
  #include <pwd.h>             /* getpwuid — .z.u OS username */
  #include <netdb.h>          /* getaddrinfo — .z.a local IPv4 */
  #include <netinet/in.h>     /* struct sockaddr_in */
  #include <arpa/inet.h>      /* ntohl */
#else
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>        /* gethostname/getaddrinfo/ntohl — .z.h/.z.a (winsock, needs WSAStartup) */
  #include <ws2tcpip.h>        /* struct addrinfo / getaddrinfo on Windows */
  #include <windows.h>         /* GetSystemTimePreciseAsFileTime — CLOCK_REALTIME shim */
#endif

#ifdef RAY_OS_WINDOWS
/* On Windows gethostname()/getaddrinfo() are winsock calls that fail with
 * WSANOTINITIALISED until WSAStartup() has run.  Nothing in the engine starts
 * winsock (the socket layer relies on the OS lazy-init on its own paths), so
 * .z.h returned EMPTY and .z.a fell through to 0i (0.0.0.0).  Ensure winsock is
 * up here with a one-time guarded WSAStartup.  We deliberately never WSACleanup:
 * winsock is a process-lifetime resource the OS reclaims at exit, and a matched
 * teardown could pull it out from under a concurrent socket.  WSAStartup itself
 * is refcounted/idempotent, so even a benign flag race just calls it twice. */
static void win_wsa_ensure(void) {
    static bool started = false;
    if (started) return;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) started = true;
}
#endif

/* argv is process-lifetime (owned by main), so we cache only the pointers and
 * the script's position and MINT each `.z.*` value on demand.  These values
 * are immutable argv snapshots, cheap to build, and read rarely — caching them
 * as owned `ray_t*` would add lifecycle (init/destroy/retain) without benefit.
 * Adding a `.z.*` name = one Z_TAB row + a small producer; init/destroy and the
 * resolver are untouched. */
static int    g_argc       = 0;
static char** g_argv       = NULL;
static int    g_script_idx = -1;   /* argv index of the `*.q` script, or -1 */
static bool   g_quiet      = false; /* `-q` on the command line (kdb .z.q) */
static ray_t* g_zts        = NULL;  /* current `.z.ts` timer handler (owned), or NULL */
static ray_t* g_zexit      = NULL;  /* current `.z.exit` process-exit handler */

int q_dotz_ipc_hook_index(const char* name, size_t len) {
    if (len != 5 || name[0] != '.' || name[1] != 'z' || name[2] != '.' ||
        name[3] != 'p')
        return -1;
    switch (name[4]) {
        case 'o': return 0;   /* .z.po -> .ipc.on.open  */
        case 'c': return 1;   /* .z.pc -> .ipc.on.close */
        case 'g': return 2;   /* .z.pg -> .ipc.on.sync  */
        case 's': return 3;   /* .z.ps -> .ipc.on.async */
        case 'w': return 4;   /* .z.pw -> .ipc.on.auth  */
        default:  return -1;
    }
}

static bool ends_with_dot_q(const char* s) {
    size_t n = strlen(s);
    return n >= 2 && s[n - 2] == '.' && s[n - 1] == 'q';
}

/* Launcher-flag classification — THE single home for which argv tokens the q
 * launcher (qmain.c) recognizes and consumes.  kdb consumes its recognized
 * options, so the `.z.x` view (args after the script) OMITS them while `.z.X`
 * (raw argv) keeps everything.  This same table drives the `*.q` script
 * locator (a value-flag's following token is a value, not the script) and the
 * `-q`/`.z.q` quiet-mode probe.  Returns:
 *   Q_FLAG_NONE   not a recognized launcher flag (positional / script)
 *   Q_FLAG_BOOL   recognized flag consuming NO following token: -q
 *   Q_FLAG_VALUE  recognized flag consuming its FOLLOWING token: -p/--port/-u/-U
 * KEEP IN SYNC with qmain.c's arg-parse switch (it acts on -p/-u/-U and reads
 * `.z.q` back via q_dotz_quiet()) — this classifier is the canonical list. */
enum { Q_FLAG_NONE = 0, Q_FLAG_BOOL = 1, Q_FLAG_VALUE = 2 };
static int flag_kind(const char* s) {
    if (strcmp(s, "-q") == 0 || strcmp(s, "--nonlegacy") == 0) return Q_FLAG_BOOL;
    if (strcmp(s, "-p") == 0 || strcmp(s, "--port") == 0 ||
        strcmp(s, "-u") == 0 || strcmp(s, "-U") == 0) return Q_FLAG_VALUE;
    return Q_FLAG_NONE;
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
static ray_t* z_x(void) {   /* args AFTER the script, MINUS launcher-consumed flags */
    int    lo  = g_script_idx < 0 ? g_argc : g_script_idx + 1;
    int    cap = g_argc - lo;
    ray_t* out = ray_list_new(cap > 0 ? cap : 1);   /* over-cap ok — length grows on append */
    for (int i = lo; i < g_argc; i++) {
        int k = flag_kind(g_argv[i]);
        if (k == Q_FLAG_VALUE) { i++; continue; }   /* drop flag AND its value token */
        if (k == Q_FLAG_BOOL)  { continue; }        /* drop the bare flag (e.g. -q) */
        ray_t* s = ray_str(g_argv[i], strlen(g_argv[i]));
        out = ray_list_append(out, s);   /* append RETAINS */
        ray_release(s);
    }
    return out;
}
static ray_t* z_X(void) {   /* full raw argv, including the binary (UNFILTERED) */
    return strings_list(0, g_argc);
}
static ray_t* z_Xs(void) { /* raw command line as ONE space-joined string (kdb .z.Xs) */
    size_t tot = 0;
    for (int i = 0; i < g_argc; i++) tot += strlen(g_argv[i]) + 1;   /* +1 for space/NUL */
    char*  buf = tot ? (char*)malloc(tot) : NULL;
    size_t off = 0;
    for (int i = 0; i < g_argc; i++) {
        size_t l = strlen(g_argv[i]);
        memcpy(buf + off, g_argv[i], l);
        off += l;
        if (i + 1 < g_argc) buf[off++] = ' ';
    }
    ray_t* s = ray_str(buf ? buf : "", off);
    free(buf);
    return s;
}

/* ---- system / host / process producers (kdb .z.o/.z.i/.z.h/.z.u/.z.a) -------
 * Read-only, minted fresh per reference like the rest of Z_TAB.  POSIX-first
 * (this file is already POSIX for the clock family); Windows fidelity is
 * deferred with the clock family (see dotz-status.md). */
static ray_t* z_q(void) { return ray_bool(g_quiet); }   /* .z.q — quiet mode */

static ray_t* z_o(void) {   /* .z.o — OS/build symbol (l64/m64/w64 …), kdb-token set */
    const char* os;
#if defined(__linux__)
  #if defined(__aarch64__) || defined(__arm__)
    os = (sizeof(void*) == 8) ? "l64arm" : "l32arm";  /* l64arm since kdb 4.1t */
  #else
    os = (sizeof(void*) == 8) ? "l64" : "l32";
  #endif
#elif defined(__APPLE__)
    os = (sizeof(void*) == 8) ? "m64" : "m32";
#elif defined(_WIN32) || defined(RAY_OS_WINDOWS)
    os = (sizeof(void*) == 8) ? "w64" : "w32";
#else
    os = (sizeof(void*) == 8) ? "l64" : "l32";
#endif
    return ray_sym(ray_sym_intern(os, strlen(os)));
}

/* .z.i — PID.  The published .z reference displays it with NO `i` suffix (a
 * long/-7h), unlike .z.a which shows the `i` suffix (int/-6h). */
static ray_t* z_i(void) { return ray_i64((int64_t)getpid()); }

static ray_t* z_h(void) {   /* .z.h — host name as a symbol (gethostname) */
    char host[256];
#ifdef RAY_OS_WINDOWS
    win_wsa_ensure();        /* gethostname is a winsock call on Windows */
#endif
    if (gethostname(host, sizeof host) != 0) host[0] = '\0';
    host[sizeof host - 1] = '\0';
    return ray_sym(ray_sym_intern(host, strlen(host)));
}

/* .z.u — the OS username the process runs under (console handle-0 semantics:
 * "the userid under which the process is running").  NOT the -u/-U auth flag
 * (those are the access-control file, unrelated).  getpwuid then $USER/$LOGNAME. */
static ray_t* z_u(void) {
    const char* name = NULL;
#ifndef RAY_OS_WINDOWS
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_name) name = pw->pw_name;
#endif
    if (!name || !*name) name = getenv("USER");
    if (!name || !*name) name = getenv("LOGNAME");
    if (!name) name = "";
    return ray_sym(ray_sym_intern(name, strlen(name)));
}

/* .z.a — local IPv4 as a 32-bit int (kdb: `0x0 vs .z.a` yields the octets,
 * most-significant first — i.e. host-order ntohl of the network address).
 * Resolve the primary IPv4 of the hostname; 0i if it cannot be determined. */
static ray_t* z_a(void) {
    int32_t addr = 0;
    char host[256];
#ifdef RAY_OS_WINDOWS
    win_wsa_ensure();        /* gethostname/getaddrinfo are winsock calls on Windows */
#endif
    /* Identical selection on both platforms: the FIRST AF_INET result of
     * getaddrinfo(hostname) — winsock exposes the same getaddrinfo/addrinfo API
     * as POSIX (ws2tcpip.h), so this is a single shared path, not a fork. */
    if (gethostname(host, sizeof host) == 0) {
        host[sizeof host - 1] = '\0';
        struct addrinfo hints;
        struct addrinfo* res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
            struct sockaddr_in* sin = (struct sockaddr_in*)(void*)res->ai_addr;
            addr = (int32_t)ntohl(sin->sin_addr.s_addr);
            freeaddrinfo(res);
        }
    }
    return ray_i32(addr);
}

/* .z.w — the current IPC connection handle, as the kdb int handle.  Inside a
 * server-side hook (.z.pg/.z.ps/.z.po/…) this is the CALLER's handle; OUTSIDE
 * any hook kdb yields 0i.  ray_ipc_current_handle() returns the internal
 * SELECTOR ID (-1 when no connection is on this stack); the q-visible handle
 * namespace is the socket FD (fix/q-hopen-fd — a q handle IS the fd), so
 * convert selector->fd via ray_ipc_fd_of_handle so `.z.w` matches what
 * hopen/hclose/handle-apply see.  -1 or an unresolvable selector -> 0i.
 * (.ipc.handle keeps the raw selector-id/-1 surface; unchanged.) */
static ray_t* z_w(void) {
    int64_t sel = ray_ipc_current_handle();   /* selector id, or -1 outside a hook */
    if (sel < 0) return ray_i32(0);
    int64_t fd = ray_ipc_fd_of_handle(sel);   /* q handle == socket fd */
    return ray_i32(fd < 0 ? 0 : (int32_t)fd);
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
#ifdef RAY_OS_WINDOWS
    /* Windows has no clock_gettime(CLOCK_REALTIME).  FILETIME counts 100ns
     * ticks since 1601-01-01 UTC — rebase to the unix epoch (11644473600s in
     * 100ns units) and split into sec/nsec.  Precise variant needs Win8+, which
     * the build's _WIN32_WINNT=0x0A00 (Win10) guarantees. */
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    ts.tv_sec  = (time_t)(t / 10000000ULL);
    ts.tv_nsec = (long)((t % 10000000ULL) * 100);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
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

/* ---- `.z.ts` timer handler ------------------------------------------------
 * `.z.ts` is a SETTABLE handler fired on each `\t N` tick (server-initiated
 * periodic push).  It is NOT an `.ipc.on.*` connection hook, so it does not use
 * env.c's frozen ipc-hook carve-out — it lives in this q-layer slot, set via
 * q_setg_wrap and read back via q_dotz_resolve.  The single forwarding thunk
 * (q_zts_tick) is registered ONCE per `\t N` and resolves the CURRENT `.z.ts`
 * each fire, so re-assigning `.z.ts` takes effect with no re-registration. */
static ray_t* q_zts_tick(ray_t* tick) {
    (void)tick;                                  /* fire_expired's monotonic ms — kdb passes local ts */
    if (!q_sys_timer_active()) return NULL;      /* stopped (incl. reentrant \t 0) → no-op */
    ray_t* fn = g_zts;
    if (!fn) return NULL;                         /* .z.ts unset → no-op */
    ray_retain(fn);                               /* survive a re-assign mid-call */
    ray_t* ts = ray_timestamp(z_now_ns(1));       /* .z.P local timestamp arg */
    ray_t* r  = call_fn1(fn, ts);
    ray_release(ts);
    ray_release(fn);
    /* Drain any show/0N! console output the handler produced to the SERVER
     * stdout — the timer fires outside run_one_line / remote-eval / an IPC hook,
     * so nothing else drains q_console_str here; without this the output is
     * delayed until the next eval or (in an idle timer server) accumulates
     * unbounded.  fflush so an otherwise-idle server surfaces it promptly. */
    { const char* con = q_console_str();
      if (con && *con) { fputs(con, stdout); fflush(stdout); }
      q_console_reset(); }
    return r;                                      /* fire_expired frees/prints it */
}

void q_dotz_zts_set(ray_t* fn) {
    if (fn) ray_retain(fn);          /* retain-new before release-old (robust slot order) */
    if (g_zts) ray_release(g_zts);
    g_zts = fn;
}

/* `.z.exit` slot — same shape as `.z.ts` above: set via q_setg_wrap, read back
 * via q_dotz_resolve, released (never FIRED) by q_dotz_destroy — a per-runtime
 * teardown is not a process exit.  Firing is q_exit's job (q_sys.c). */
void q_dotz_zexit_set(ray_t* fn) {
    if (fn) ray_retain(fn);
    if (g_zexit) ray_release(g_zexit);
    g_zexit = fn;
}

/* Call `.z.exit` (if set) with the exit code (ref/dotz.md: unary, arg = the
 * exit parameter; default = do nothing), then drain its show/0N! console
 * output to stdout — this runs moments before exit(), nothing else drains. */
void q_dotz_exit_fire(int code) {
    ray_t* fn = g_zexit;
    if (!fn) return;
    ray_retain(fn);                      /* survive a re-assign mid-call */
    ray_t* arg = ray_i64(code);
    ray_t* r = call_fn1(fn, arg);
    ray_release(arg);
    ray_release(fn);
    if (r) ray_release(r);
    { const char* con = q_console_str();
      if (con && *con) { fputs(con, stdout); fflush(stdout); }
      q_console_reset(); }
}

ray_t* q_dotz_timer_thunk(void) {
    return ray_fn_unary(".z.ts", RAY_FN_NONE, q_zts_tick);
}

static const struct { const char* name; uint8_t len; ray_t* (*make)(void); }
Z_TAB[] = {
    { ".z.f", 4, z_f },
    { ".z.x", 4, z_x },
    { ".z.X", 4, z_X },
    { ".z.Xs", 5, z_Xs },
    { ".z.q", 4, z_q },
    { ".z.o", 4, z_o },
    { ".z.i", 4, z_i },
    { ".z.h", 4, z_h },
    { ".z.u", 4, z_u },
    { ".z.a", 4, z_a },
    { ".z.w", 4, z_w },
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
        if (i > 1 && flag_kind(argv[i - 1]) == Q_FLAG_VALUE) continue;  /* skip flag value */
        if (ends_with_dot_q(argv[i])) { g_script_idx = i; break; }
    }

    /* Quiet mode (`-q`, kdb .z.q): scan argv, skipping value-flag values so a
     * `-p -q` (where `-q` is a port value, not the flag) doesn't false-trigger. */
    g_quiet = false;
    for (int i = 1; i < argc; i++) {
        int k = flag_kind(argv[i]);
        if (k == Q_FLAG_VALUE) { i++; continue; }
        if (k == Q_FLAG_BOOL && strcmp(argv[i], "-q") == 0) g_quiet = true;
    }
}

bool q_dotz_quiet(void) { return g_quiet; }

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

    /* `.z.ts` timer handler read-back: return the stored handler (retained),
     * or decline when unset (→ eval raises 'name, like the `.z.p*` aliases). */
    if (!out && n == 5 && memcmp(p, ".z.ts", 5) == 0 && g_zts) {
        ray_retain(g_zts);
        out = g_zts;
    }
    if (!out && n == 7 && memcmp(p, ".z.exit", 7) == 0 && g_zexit) {
        ray_retain(g_zexit);
        out = g_zexit;
    }

    /* kdb `.z.p*` handler-alias READ-BACK: resolve to the SAME `.ipc.on.*` env
     * slot the write path (q_setg_wrap) installs into — so `.z.pg` reflects a
     * `.ipc.on.sync:{…}` assignment and vice-versa.  An UNSET alias declines
     * (NULL -> eval raises 'name, matching an unset name); kdb's default-handler
     * exposure is deferred (see the plan's accepted decisions). */
    if (!out) {
        int hk = q_dotz_ipc_hook_index(p, n);
        if (hk >= 0) {
            ray_t* fn = ray_env_get(ray_sym_ipc_hook(hk));   /* borrowed */
            if (fn) { ray_retain(fn); out = fn; }
        }
    }

    ray_release(name);
    return out;
}

void q_dotz_destroy(void) {
    if (g_zts) { ray_release(g_zts); g_zts = NULL; }   /* release the `.z.ts` handler */
    if (g_zexit) { ray_release(g_zexit); g_zexit = NULL; }
    g_argc       = 0;
    g_argv       = NULL;
    g_script_idx = -1;
    g_quiet      = false;
}
