/* q_dotz — see q_dotz.h.  The eval-time `.z.*` command-line resolver. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* clock_gettime / gmtime_r for the clock producers */
#endif
#include "qlang/q_dotz.h"
#include "qlang/q_deriv.h"     /* q_deriv_kind_of / q_deriv_base — unwrap {…} carrier */
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
 * Adding a computed `.z.*` name = one switch case in q_dotz_resolve + a small
 * producer; init/destroy are untouched. */
static int    g_argc       = 0;
static char** g_argv       = NULL;
static int    g_script_idx = -1;   /* argv index of the `*.q` script, or -1 */
static bool   g_quiet      = false; /* `-q` on the command line (kdb .z.q) */
static ray_t* g_zts        = NULL;  /* current `.z.ts` timer handler (owned), or NULL */
static ray_t* g_zexit      = NULL;  /* current `.z.exit` process-exit handler */
static ray_t* g_zph        = NULL;  /* current `.z.ph` HTTP-GET handler (owned) */
static ray_t* g_zws        = NULL;  /* current `.z.ws` WS-message handler (owned) */
static ray_t* g_zwo        = NULL;  /* current `.z.wo` WS-open handler (owned) */
static ray_t* g_zwc        = NULL;  /* current `.z.wc` WS-close handler (owned) */
static ray_t* g_zpp        = NULL;  /* current `.z.pp` HTTP-POST handler (owned) */
static ray_t* g_zac        = NULL;  /* current `.z.ac` HTTP-auth handler (owned) */
static ray_t* g_zpm        = NULL;  /* current `.z.pm` HTTP-methods handler (owned) */

int q_dotz_ipc_hook_index(const char* name, size_t len) {
    if (len == 5 && memcmp(name, ".z.bm", 5) == 0)
        return 5;             /* .z.bm -> .ipc.on.badmsg (dotz.md msg validator) */
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
        ray_t* s = ray_charv(g_argv[i], (int64_t)strlen(g_argv[i]));
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
        ray_t* s = ray_charv(g_argv[i], (int64_t)strlen(g_argv[i]));
        out = ray_list_append(out, s);   /* append RETAINS */
        ray_release(s);
    }
    return out;
}
static ray_t* z_X(void) {   /* full raw argv, including the binary (UNFILTERED) */
    return strings_list(0, g_argc);
}

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

/* .z.i — PID.  The published .z reference displays it with NO `i` suffix (a
 * long/-7h), unlike .z.a which shows the `i` suffix (int/-6h). */

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

/* The ONE home for the settable `.z.*` handler name->slot mapping: q_dotz_get
 * (the C fire consumers) and q_dotz_set (the write path) both route through it,
 * so nothing outside dotz.c needs per-slot functions.  Who FIRES each (never
 * from here): `.z.ts` the poll-loop timer, `.z.exit` q_exit (q_sys.c),
 * `.z.ph`/`.z.pp`/`.z.ac`/`.z.pm` q_http.c, `.z.ws`/`.z.wo`/`.z.wc` q_ws.c. */
static ray_t** z_slot_ptr(const char* nm, size_t l) {
    if (l == 7 && memcmp(nm, ".z.exit", 7) == 0) return &g_zexit;
    if (l != 5 || nm[0] != '.' || nm[1] != 'z' || nm[2] != '.') return NULL;
    switch (nm[3]) {
        case 'p': switch (nm[4]) { case 'h': return &g_zph;    /* .z.ph */
                                   case 'p': return &g_zpp;    /* .z.pp */
                                   case 'm': return &g_zpm; }  /* .z.pm */
                  break;
        case 'w': switch (nm[4]) { case 's': return &g_zws;    /* .z.ws */
                                   case 'o': return &g_zwo;    /* .z.wo */
                                   case 'c': return &g_zwc; }  /* .z.wc */
                  break;
        case 't': if (nm[4] == 's') return &g_zts; break;      /* .z.ts */
        case 'a': if (nm[4] == 'c') return &g_zac; break;      /* .z.ac */
    }
    return NULL;
}

/* Read-back a settable handler by name — BORROWED, NULL = unset/not-a-handler.
 * The C fire consumers (q_http.c / q_ws.c) call this on their request/event
 * path (named lookup, not a hot loop); q_dotz_resolve retains it for q. */
ray_t* q_dotz_get(const char* name, size_t len) {
    ray_t** slot = z_slot_ptr(name, len);
    return slot ? *slot : NULL;
}

/* Assign a settable handler by name — returns true iff `name` IS one (caller
 * then stops; else it falls to the `.ipc.on.*` / plain-env path).  Retains its
 * own ref; unwraps a q `{…}` lambda carrier to its base RAY_LAMBDA (call_fn1
 * fires a bare lambda, never the apply-hook carrier).  Passing NULL clears. */
bool q_dotz_set(const char* name, size_t len, ray_t* val) {
    ray_t** slot = z_slot_ptr(name, len);
    if (!slot) return false;
    if (val && val->type == RAY_LIST && q_deriv_kind_of(val) == Q_DERIV_LAMBDA) {
        ray_t* base = q_deriv_base(val);        /* borrowed bare RAY_LAMBDA */
        if (base) val = base;
    }
    if (val) ray_retain(val);
    if (*slot) ray_release(*slot);
    *slot = val;
    return true;
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

/* Borrow a settable-handler slot for read-back: retained (owned) or NULL when
 * unset (NULL -> eval raises 'name, matching an unset name). */
static ray_t* z_slot(ray_t* g) { if (g) ray_retain(g); return g; }

ray_t* q_dotz_resolve(int64_t sym_id) {
    ray_t* name = ray_sym_str(sym_id);   /* BORROWED: cached arena string atom
                                          * (RAY_ATTR_ARENA); the ray_release
                                          * below is a no-op — do not rely on it
                                          * or "fix a leak" here */
    if (!name) return NULL;
    const char* p = ray_str_ptr(name);
    size_t      n = ray_str_len(name);

    /* Everything this resolver knows is `.z.*` — reject other names up front
     * (they fall to env/registry in q_name_resolve). */
    if (n < 4 || p[0] != '.' || p[1] != 'z' || p[2] != '.') return NULL;

    ray_t* out = NULL;
    /* Computed read-only `.z.*` — all 4-char names, dispatched on the char
     * after ".z.".  The per-name producers (z_*) are the single home and each
     * returns an OWNED ref (rc>=1).  The len-5 settable-handler names
     * (.z.ts/.z.ph/.z.ws/.z.wo/.z.wc/.z.pp/.z.ac/.z.pm) miss here and fall
     * through to their g_z* read-backs below. */
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
            case 'w': out = z_w(); break;
            case 'K': out = z_K(); break;
            case 'k': out = z_k(); break;
            /* one-line producers inlined; z_now_ns(0)=UTC, (1)=local */
            case 'q': out = ray_bool(g_quiet); break;                            /* .z.q quiet */
            case 'i': out = ray_i64((int64_t)getpid()); break;                   /* .z.i pid   */
            case 'p': out = ray_timestamp(z_now_ns(0)); break;                   /* .z.p / .z.P */
            case 'P': out = ray_timestamp(z_now_ns(1)); break;
            case 'd': out = ray_date((int64_t)(z_now_ns(0) / RAY_NS_PER_DAY)); break;    /* .z.d / .z.D */
            case 'D': out = ray_date((int64_t)(z_now_ns(1) / RAY_NS_PER_DAY)); break;
            case 't': out = ray_time((z_now_ns(0) % RAY_NS_PER_DAY) / 1000000LL); break; /* .z.t / .z.T */
            case 'T': out = ray_time((z_now_ns(1) % RAY_NS_PER_DAY) / 1000000LL); break;
            case 'n': out = ray_timespan(z_now_ns(0) % RAY_NS_PER_DAY); break;           /* .z.n / .z.N */
            case 'N': out = ray_timespan(z_now_ns(1) % RAY_NS_PER_DAY); break;
            case 'z': out = ray_datetime((double)z_now_ns(0) / (double)RAY_NS_PER_DAY); break; /* .z.z / .z.Z */
            case 'Z': out = ray_datetime((double)z_now_ns(1) / (double)RAY_NS_PER_DAY); break;
        }
    }

    /* Settable-handler read-back via the shared name->slot map (owned copy for
     * q).  The `.z.p*` IPC hooks (.z.pg/.z.ps/.z.po/.z.pc/.z.pw) aren't handler
     * slots — z_slot_ptr declines them and they fall to the `.ipc.on.*` aliases
     * below. */
    if (!out) out = z_slot(q_dotz_get(p, n));

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
    if (g_zph) { ray_release(g_zph); g_zph = NULL; }
    if (g_zws) { ray_release(g_zws); g_zws = NULL; }
    if (g_zwo) { ray_release(g_zwo); g_zwo = NULL; }
    if (g_zwc) { ray_release(g_zwc); g_zwc = NULL; }
    if (g_zpp) { ray_release(g_zpp); g_zpp = NULL; }
    if (g_zac) { ray_release(g_zac); g_zac = NULL; }
    if (g_zpm) { ray_release(g_zpm); g_zpm = NULL; }
    g_argc       = 0;
    g_argv       = NULL;
    g_script_idx = -1;
    g_quiet      = false;
}
