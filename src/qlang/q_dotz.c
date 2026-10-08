/* q_dotz — see q_dotz.h.  The eval-time `.z.*` command-line resolver. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* clock_gettime / gmtime_r / localtime_r for the clock producers */
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
  #define _GNU_SOURCE             /* sched_getaffinity / CPU_COUNT — .z.c */
#endif
#include "qlang/q_count.h"
#include "qlang/q_dotz.h"
#include "qlang/q_env.h"       /* q_env_get — the settable handlers are globals */
#include "qlang/eval/q_eval.h" /* q_eval_apply_call_sym — handler firing */
#include "qlang/eval/q_view.h" /* q_view_zb — `.z.b` dependency dict */
#include "qlang/eval/q_dbg.h"  /* q_dbg_zex/_zey — `.z.ex`/`.z.ey` (basics/debug.md) */
#include "qlang/net/q_tls.h"   /* q_tls_dotz_e — `.z.e` TLS connection status */
#include "qlang/ops/q_sys.h"       /* q_sys_timer_active / q_sys_utc_offset_secs — timer guard, `\o` */
#include "qlang/q_console.h"   /* q_console_door — .z.ts/.z.exit output is the server console's */
#include "qlang/io/q_conn.h"   /* q_conn_zW/_zH — `.z.W`/`.z.H` collector views */
#include "lang/cal.h"          /* ymd_to_date — build-date -> q date for .z.k */
#include "lang/env.h"          /* ray_env_get / ray_fn_unary */
#include "lang/eval.h"         /* RAY_FN_NONE — .z.ts timer thunk attrs */
#include "core/ipc.h"          /* ray_ipc_current_handle / ray_ipc_fd_of_handle — .z.w */
#include "core/platform.h"     /* ray_thread_count — .z.c off Linux */
#include <rayforce.h>
#include <stdio.h>             /* snprintf / sscanf for the version producers */
#include <stdlib.h>            /* getenv — .z.v environment */
#include <string.h>
#include <time.h>             /* clock_gettime / gmtime_r / localtime_r — .z clock family, ltime/gtime offset */
#include <unistd.h>          /* getpid / gethostname / getuid — .z.i/.z.h/.z.u */
#ifndef RAY_OS_WINDOWS
  #include <pwd.h>             /* getpwuid — .z.u OS username */
  #include <netdb.h>          /* getaddrinfo — .z.a local IPv4 */
  #include <netinet/in.h>     /* struct sockaddr_in */
  #include <arpa/inet.h>      /* ntohl */
  #if defined(__linux__)
    #include <sched.h>        /* sched_getaffinity / cpu_set_t — .z.c */
  #endif
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
 * MINT each `.z.*` value on demand.  These values
 * are immutable argv snapshots, cheap to build, and read rarely — caching them
 * as owned `ray_t*` would add lifecycle (init/destroy/retain) without benefit.
 * Adding a computed `.z.*` name = one switch case in q_dotz_resolve + a small
 * producer; init/destroy are untouched. */
static int    g_argc  = 0;
static char** g_argv  = NULL;
static bool   g_file  = false;   /* argv[1] is the file */
static bool   g_quiet = false;   /* `-q` on the command line (kdb .z.q) */

/* door: a connection or message handler, which a forked worker resets to its startup value */
static const struct { const char* name; bool door; } HOOKS[] = {
    { ".z.pg", true },  { ".z.ps", true },  { ".z.po", true },  { ".z.pc", true },  { ".z.pw", true },
    { ".z.bm", true },  { ".z.pq", true },  { ".z.ph", true },  { ".z.pp", true },  { ".z.pm", true },
    { ".z.ac", true },  { ".z.wo", true },  { ".z.wc", true },  { ".z.ws", true },  { ".z.pi", false },
    { ".z.pd", false }, { ".z.ts", false }, { ".z.exit", false }, { ".z.vs", false }, { ".z.zd", false },
};
#define N_HOOKS (sizeof HOOKS / sizeof *HOOKS)
static ray_t* g_door_boot[N_HOOKS];

bool q_dotz_expungeable(const char* name, size_t len) {
    for (size_t i = 0; i < N_HOOKS; i++)
        if (strlen(HOOKS[i].name) == len && memcmp(HOOKS[i].name, name, len) == 0) return true;
    return false;
}

static int64_t hook_sym(size_t i) {
    return ray_sym_intern_runtime(HOOKS[i].name, strlen(HOOKS[i].name));
}

void q_dotz_doors_snapshot(void) {
    for (size_t i = 0; i < N_HOOKS; i++) {
        if (!HOOKS[i].door) continue;
        if (g_door_boot[i]) ray_release(g_door_boot[i]);
        g_door_boot[i] = q_env_get(hook_sym(i));
        if (g_door_boot[i]) ray_retain(g_door_boot[i]);
    }
}

void q_dotz_doors_restore(void) {
    for (size_t i = 0; i < N_HOOKS; i++) {
        if (!HOOKS[i].door) continue;
        if (g_door_boot[i]) (void)q_env_bind(hook_sym(i), g_door_boot[i]);
        else (void)q_env_unbind(hook_sym(i));
    }
}

/* basics/cmdline.md's options, then peachq's own launcher flags: what q consumes from argv. */
static const q_dotz_opt_t Q_OPTS[] = {
    { "-b", 0, NULL, NULL }, { "-c", 2, "c", NULL }, { "-C", 2, "C", NULL }, { "-e", 1, "e", "012" },
    { "-E", 1, NULL, "012" }, { "-g", 1, "g", "01" }, { "-l", 0, NULL, NULL }, { "-L", 0, NULL, NULL },
    { "-m", 1, NULL, NULL }, { "-o", 1, "o", NULL }, { "-p", 1, NULL, NULL }, { "-P", 1, "P", NULL },
    { "-q", 0, NULL, NULL }, { "-r", 1, NULL, NULL }, { "-s", 1, "s", NULL }, { "-S", 1, "S", NULL },
    { "-t", 1, "t", NULL }, { "-T", 1, NULL, NULL }, { "-u", 1, NULL, NULL }, { "-U", 1, NULL, NULL },
    { "-w", 1, NULL, NULL }, { "-W", 1, "W", NULL }, { "-z", 1, "z", "01" },
    { "--port", 1, NULL, NULL }, { "-classic", 0, NULL, NULL }, { "-eval", 1, NULL, NULL },
    { "-eval-before", 1, NULL, NULL }, { "-duckdb", 1, NULL, NULL }, { "-conn", 1, NULL, NULL },
    { "-save", 1, NULL, NULL }, { "-ls", 0, NULL, NULL },
};

const q_dotz_opt_t* q_dotz_opt(const char* token) {
    for (size_t i = 0; i < sizeof Q_OPTS / sizeof *Q_OPTS; i++)
        if (strcmp(Q_OPTS[i].name, token) == 0) return &Q_OPTS[i];
    return NULL;
}

const char* q_dotz_file_arg(int argc, char** argv) {
    return argc > 1 && argv[1][0] != '-' ? argv[1] : NULL;
}

/* argv[lo..hi) as a q list of strings, skipping every option and its parameters when `args` is set. */
static ray_t* strings_list(int lo, int hi, bool args) {
    ray_t* out = ray_list_new(hi > lo ? hi - lo : 1);
    for (int i = lo; i < hi; i++) {
        const q_dotz_opt_t* o = args ? q_dotz_opt(g_argv[i]) : NULL;
        if (o) { i += o->nparam; continue; }
        ray_t* s = ray_charv(g_argv[i], (int64_t)strlen(g_argv[i]));
        out = ray_list_append(out, s);   /* append RETAINS */
        ray_release(s);
    }
    return out;
}

/* `.z.*` producers — each mints a FRESH owned ref (rc>=1), matching the
 * name-hook contract (the resolver returns an owned value or NULL). */
static ray_t* z_f(void) {   /* the file as given; null sym when none */
    const char* s = g_file ? g_argv[1] : "";
    return ray_sym(ray_sym_intern(s, strlen(s)));
}
static ray_t* z_x(void) { return strings_list(g_file ? 2 : 1, g_argc, true); }
static ray_t* z_X(void) { return strings_list(0, g_argc, false); }

/* ---- system / host / process producers (kdb .z.o/.z.i/.z.h/.z.u/.z.a) -------
 * Read-only, minted fresh per reference like the other computed producers.  POSIX-first
 * (this file is already POSIX for the clock family); Windows fidelity is
 * deferred with the clock family (see dotz-status.md). */

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

/* .z.i — PID, an INT (ref/dotz.md:403 says so in prose).  The doc's bare `23219`
 * display is a pre-3.0 artifact, not evidence of a long. */

static ray_t* z_h(void) {   /* .z.h — host name as a symbol (gethostname) */
    char host[256];
#ifdef RAY_OS_WINDOWS
    win_wsa_ensure();        /* gethostname is a winsock call on Windows */
#endif
    if (gethostname(host, sizeof host) != 0) host[0] = '\0';
    host[sizeof host - 1] = '\0';
    return ray_sym(ray_sym_intern(host, strlen(host)));
}

/* .z.u — connection-context userid (ref/dotz.md): console/handle 0 = the OS
 * username the process runs under (NOT the -u/-U auth flag — that's the
 * access-control file); in a callback, server end (inbound) = the userid the
 * client's hopen passed, client end (outbound) = the null sym.  Correct
 * already inside .z.pw (user stamped at handshake parse, before the hook). */
static ray_t* z_u(void) {
    int64_t sel = ray_ipc_current_handle();
    ray_ipc_conn_ident_t id;
    if (sel >= 0 && ray_ipc_conn_identity(sel, &id)) {
        int64_t s = (id.inbound && id.user_sym >= 0)
                        ? id.user_sym : ray_sym_intern_runtime("", 0);
        return ray_sym(s);
    }
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

/* .z.a — IPv4 as a 32-bit int (kdb: `0x0 vs .z.a` yields the octets,
 * most-significant first — i.e. host-order ntohl of the network address).
 * In a callback: the PEER's IP (the client session, ref/dotz.md), 0i over a
 * unix domain socket.  Otherwise the primary IPv4 of the hostname
 * (== .Q.addr .z.h); 0i if it cannot be determined. */
static ray_t* z_a(void) {
    int64_t sel = ray_ipc_current_handle();
    ray_ipc_conn_ident_t id;
    if (sel >= 0 && ray_ipc_conn_identity(sel, &id))
        return ray_i32(id.is_unix ? 0 : id.peer_addr);
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

/* .z.c — the cores THIS PROCESS may use (learn/licensing.md:100), not the machine's: the affinity
 * mask on Linux/Windows (`taskset -c 0 q` answers 1i); the online count elsewhere.  An int atom (owner ruling). */
static ray_t* z_c(void) {
    long n = 0;
#if defined(__linux__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) n = CPU_COUNT(&set);
#elif defined(_WIN32)
    DWORD_PTR pm, sm;
    if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) for (; pm; pm &= pm - 1) n++;
#endif
    if (n <= 0) n = (long)ray_thread_count();
    return ray_i32((int32_t)n);
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

/* `.z.e` — TLS status of the CURRENT connection handle (the same `.z.w` one),
 * so a `.z.po` hook sees the client that just connected.  Outside any hook
 * there is no connection, hence the empty dict (ref/dotz.md:216). */
static ray_t* z_e(void) {
    int64_t fd = ray_ipc_current_fd();
    return q_tls_conn_info(fd < 0 ? RAY_INVALID_SOCK : (ray_sock_t)fd);
}

/* peachq version surface — reads the SAME compile-time macros the Makefile
 * injects for .sys.build (RAYFORCE_VERSION = the VERSION file, RAYFORCE_BUILD_DATE). */
static void z_build_ymd(int* y, int* m, int* d) {
    *y = RAY_DATE_EPOCH; *m = 1; *d = 1;
#ifdef RAYFORCE_BUILD_DATE
    if (sscanf(RAYFORCE_BUILD_DATE, "%d-%d-%d", y, m, d) != 3) { *y = RAY_DATE_EPOCH; *m = 1; *d = 1; }
#endif
}
/* `.z.K` is the kdb+ generation peachq claims compatibility with (ref/dotz.md:412), FIXED so framework gates
 * (`.z.K>=4`) take their modern branches; it never encodes our release number — that is `.z.v`version`. */
static ray_t* z_K(void) { return ray_f64(5.0); }
static ray_t* z_k(void) {   /* `.z.k` — build/release date (kdb .z.k) */
    int y, m, d;
    z_build_ymd(&y, &m, &d);
    return ray_date(ymd_to_date(y, m, d));
}
static ray_t* z_v(void) {   /* `.z.v` — peachq's own version + environment, kdb-X's dict shape key-for-key */
    static const char* const key[] = { "version", "QCFG", "QHOME", "QLIC", "QINIT", "QPATH" };
    int  y, m, d;
    char ver[64];
    z_build_ymd(&y, &m, &d);
    snprintf(ver, sizeof ver, "%s.%04d%02d%02d", RAYFORCE_VERSION, y, m, d);   /* the VERSION file verbatim */
    const char* val[] = { ver, getenv("QCFG"), getenv("QHOME"), "", getenv("QINIT"), getenv("QPATH") };
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, 6);
    ray_t* v = ray_list_new(6);
    for (int i = 0; i < 6; i++) {
        int64_t     id = ray_sym_intern(key[i], strlen(key[i]));
        const char* s  = val[i] ? val[i] : "";
        ray_t*      c  = ray_charv(s, (int64_t)strlen(s));
        k = ray_vec_append(k, &id);
        v = ray_list_append(v, c);   /* append RETAINS */
        ray_release(c);
    }
    return ray_dict_new(k, v);       /* consumes both */
}

/* ---- .z clock family (kdb .z.p/.z.P … lowercase=UTC, uppercase=local) -------
 * Each producer RE-READS the system clock on every reference — q's timing idiom
 * `a:.z.t; costly[]; .z.t-a` requires re-sampling, never a per-block cache — and
 * at NANOSECOND resolution (clock_gettime), so sub-second deltas are visible
 * (the engine's second-granularity (date)/(time)/(timestamp) clock fns can't do
 * that).  All ten derive from one `q_dotz_now_ns` read. */
#define RAY_EPOCH_UNIX_SECS 946684800LL          /* 2000.01.01 00:00:00 UTC, unix secs */
#define RAY_NS_PER_DAY      86400000000000LL

/* No portable tm_gmtoff: the offset is the local fields' distance from the UTC fields of the same instant. */
int64_t q_dotz_utc_offset_at(int64_t secs) {
    int64_t off;
    if (q_sys_utc_offset_secs(&off)) return off;
    time_t    t = (time_t)(secs + RAY_EPOCH_UNIX_SECS);
    struct tm l, g;
    if (!localtime_r(&t, &l) || !gmtime_r(&t, &g)) return 0;
    int64_t days = l.tm_year == g.tm_year ? l.tm_yday - g.tm_yday : l.tm_year > g.tm_year ? 1 : -1;
    return ((days * 24 + l.tm_hour - g.tm_hour) * 60 + l.tm_min - g.tm_min) * 60 + l.tm_sec - g.tm_sec;
}

/* Current time as nanoseconds since the rayforce epoch (2000.01.01), in UTC
 * (local=0) or local wall-clock (local=1). */
int64_t q_dotz_now_ns(int local) {
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
    int64_t ns = ((int64_t)ts.tv_sec - RAY_EPOCH_UNIX_SECS) * 1000000000LL + ts.tv_nsec;
    if (local) {
        int64_t off = q_dotz_utc_offset_at((int64_t)ts.tv_sec - RAY_EPOCH_UNIX_SECS);
        ns = (int64_t)((uint64_t)ns + (uint64_t)off * 1000000000ULL);   /* a wild `\o` wraps, never UB */
    }
    return ns;
}


/* ---- `.z.ts` timer handler ------------------------------------------------
 * `.z.ts` is a SETTABLE handler fired on each `\t N` tick (server-initiated
 * periodic push) — an ordinary global, resolved by name at fire time, so any
 * write form reaches it.  The single forwarding thunk (zts_tick) is registered
 * ONCE per `\t N`, so re-assigning `.z.ts` needs no re-registration. */
static ray_t* zts_tick(ray_t* tick) {
    (void)tick;                                  /* fire_expired's monotonic ms — kdb passes local ts */
    if (!q_sys_timer_active()) return NULL;      /* stopped (incl. reentrant \t 0) → no-op */
    int64_t zts = ray_sym_intern_runtime(".z.ts", 5);
    if (!q_env_get(zts)) return NULL;             /* .z.ts unset → no-op */
    ray_t* ts = ray_timestamp(q_dotz_now_ns(1));       /* .z.P local timestamp arg */
    FILE*  door = q_console_door(stdout);          /* the timer's output is the SERVER console's */
    ray_t* r    = q_eval_apply_call_sym(zts, &ts, 1);
    q_console_door(door);
    ray_release(ts);
    return r;                                      /* fire_expired frees/prints it */
}

/* Call `.z.exit` (if set) with the exit code (ref/dotz.md: unary, arg = the
 * exit parameter; default = do nothing); its show/0N! goes to stdout. */
void q_dotz_exit_fire(int code) {
    int64_t zexit = ray_sym_intern_runtime(".z.exit", 7);
    if (!q_env_get(zexit)) return;
    ray_t* arg  = ray_i64(code);
    FILE*  door = q_console_door(stdout);
    ray_t* r    = q_eval_apply_call_sym(zexit, &arg, 1);
    q_console_door(door);
    ray_release(arg);
    if (r) ray_release(r);
}

ray_t* q_dotz_timer_thunk(void) {
    return ray_fn_unary(".z.ts", RAY_FN_NONE, zts_tick);
}

void q_dotz_init(int argc, char** argv) {
    g_argc  = argc;
    g_argv  = argv;
    g_file  = q_dotz_file_arg(argc, argv) != NULL;
    g_quiet = q_dotz_has_flag(argc, argv, "-q");
}

bool q_dotz_quiet(void) { return g_quiet; }

bool q_dotz_has_flag(int argc, char** argv, const char* flag) {
    for (int i = q_dotz_file_arg(argc, argv) ? 2 : 1; i < argc; i++) {
        const q_dotz_opt_t* o = q_dotz_opt(argv[i]);
        if (o && strcmp(o->name, flag) == 0) return true;
        if (o) i += o->nparam;
    }
    return false;
}

ray_t* q_dotz_resolve(int64_t sym_id) {
    ray_t* name = ray_sym_str(sym_id);   /* BORROWED: cached arena string atom
                                          * (RAY_ATTR_ARENA); the ray_release
                                          * below is a no-op — do not rely on it
                                          * or "fix a leak" here */
    if (!name) return NULL;
    const char* p = ray_str_ptr(name);
    size_t      n = ray_str_len(name);

    /* Everything this resolver knows is `.z.*` — reject other names up front
     * (they fall to env/registry in name_resolve). */
    if (n < 4 || p[0] != '.' || p[1] != 'z' || p[2] != '.') return NULL;

    ray_t* out = NULL;
    /* Computed read-only `.z.*` — all 4-char names, dispatched on the char
     * after ".z.".  The per-name producers (z_*) are the single home and each
     * returns an OWNED ref (rc>=1).  The settable handlers are ordinary
     * globals, so they never reach here: q_env_resolve already found them. */
    if (n == 4) {
        switch (p[3]) {
            /* multi-line producers keep their z_* home (argv/host/version logic) */
            case 'f': out = z_f(); break;
            case 'x': out = z_x(); break;
            case 'X': out = z_X(); break;
            case 'o': out = z_o(); break;
            case 'h': out = z_h(); break;
            case 'u': out = z_u(); break;
            case 'a': out = z_a(); break;
            case 'c': out = z_c(); break;
            case 'w': out = z_w(); break;
            case 'W': out = q_conn_zW(); break;
            case 'H': out = q_conn_zH(); break;
            case 'K': out = z_K(); break;
            case 'k': out = z_k(); break;
            case 'v': out = z_v(); break;
            /* one-line producers inlined; q_dotz_now_ns(0)=UTC, (1)=local */
            case 'b': out = q_view_zb(); break;                                  /* .z.b view deps */
            case 'e': out = z_e(); break;                                        /* .z.e TLS status */
            case 'q': out = ray_bool(g_quiet); break;                            /* .z.q quiet */
            case 's': out = q_dbg_self(); break;                                 /* .z.s self */
            case 'i': out = ray_i32((int32_t)getpid()); break;                   /* .z.i pid   */
            case 'p': out = ray_timestamp(q_dotz_now_ns(0)); break;                   /* .z.p / .z.P */
            case 'P': out = ray_timestamp(q_dotz_now_ns(1)); break;
            case 'd': out = ray_date((int64_t)(q_dotz_now_ns(0) / RAY_NS_PER_DAY)); break;    /* .z.d / .z.D */
            case 'D': out = ray_date((int64_t)(q_dotz_now_ns(1) / RAY_NS_PER_DAY)); break;
            case 't': out = ray_time((q_dotz_now_ns(0) % RAY_NS_PER_DAY) / 1000000LL); break; /* .z.t / .z.T */
            case 'T': out = ray_time((q_dotz_now_ns(1) % RAY_NS_PER_DAY) / 1000000LL); break;
            case 'n': out = ray_timespan(q_dotz_now_ns(0) % RAY_NS_PER_DAY); break;           /* .z.n / .z.N */
            case 'N': out = ray_timespan(q_dotz_now_ns(1) % RAY_NS_PER_DAY); break;
            case 'z': out = ray_datetime((double)q_dotz_now_ns(0) / (double)RAY_NS_PER_DAY); break; /* .z.z / .z.Z */
            case 'Z': out = ray_datetime((double)q_dotz_now_ns(1) / (double)RAY_NS_PER_DAY); break;
        }
    } else if (n == 5 && p[3] == 'e') {
        /* .z.ex / .z.ey — failed primitive + its args (basics/debug.md);
         * unset (no snapshot) declines -> 'name, matching an unset callback */
        if (p[4] == 'x') out = q_dbg_zex();
        else if (p[4] == 'y') out = q_dbg_zey();
    }

    ray_release(name);
    return out;
}

void q_dotz_destroy(void) {
    for (size_t i = 0; i < N_HOOKS; i++)
        if (g_door_boot[i]) { ray_release(g_door_boot[i]); g_door_boot[i] = NULL; }
    g_argc       = 0;
    g_argv       = NULL;
    g_file       = false;
    g_quiet      = false;
}
