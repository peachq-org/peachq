/* q_handles — see q_handles.h.  A compact grow-on-demand array of open-time
 * handle records, fd-keyed by linear scan (a process holds few handles; kdb's
 * own connection tables are small).  Register on hopen-file/fifo/socket,
 * deregister on hclose.  Redaction runs at CAPTURE so a password never enters a
 * record.  The `hopen`/`hclose` verb bodies sit at the tail, on top of the
 * transport open / kind dispatch / close they delegate to. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/io/q_handles.h"
#include "qlang/base/q_err.h"
#include "qlang/q_registry_internal.h" /* q_str_text_bytes, q_type_strict_i64 */
#include "qlang/q_console.h" /* q_console_write — 1/-1/2/-2 console handles */
#include "qlang/q_dotz.h"   /* q_dotz_now_ns — the portable wall clock */
#include "qlang/io/q_io.h"   /* q_io_mkdir_parents — hopen creates missing directories */
#include "qlang/net/q_ws.h"          /* q_ws_client_open — `:ws:// sym handles */
#include "qlang/net/q_http_client.h" /* q_http_client_raw — `:http:// sym handles */
#include "lang/eval.h"       /* ray_eval_get_restricted, ray_at_fn */
#include "lang/internal.h"   /* make_i64, ray_hopen_fn/ray_hsend_fn/ray_hpost_fn/ray_hclose_fn */
#include "table/sym.h"       /* ray_sym_intern_runtime, ray_sym_str */
#include "core/ipc.h"        /* ray_ipc_handle_of_fd/fd_of_handle — q true-fd handle <-> selector id */
#include <rayforce.h>
#include <stdio.h>           /* snprintf — hopen descriptor normalization */
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>           /* open — file/fifo transport handles */
#include <unistd.h>          /* read/write/close — raw handle IO */
#include <errno.h>           /* EINTR — short-write retry loop */
#ifdef RAY_OS_WINDOWS
#include <io.h>              /* _dup/_close — msvcrt has no fcntl/F_DUPFD */
#endif

typedef struct {
    int64_t       fd;
    q_handle_kind kind;
    int           initiated_out;
    int64_t       user_sym;
    int64_t       open_time_ns;
    ray_t*        open_args;   /* owned redacted charv */
} q_handle_rec;

static q_handle_rec* g_recs = NULL;
static int64_t       g_cap  = 0;
static int64_t       g_n    = 0;

void q_handles_init(void) { g_recs = NULL; g_cap = 0; g_n = 0; }

void q_handles_destroy(void) {
    for (int64_t i = 0; i < g_n; i++) {
        /* file/fifo fds were opened directly by this registry — no other owner
         * closes them (sockets are the IPC layer's).  Close on teardown so
         * repeated runtime create/destroy cannot leak descriptors. */
        if (g_recs[i].kind == Q_HANDLE_FILE || g_recs[i].kind == Q_HANDLE_FIFO)
            close((int)g_recs[i].fd);
        if (g_recs[i].open_args) ray_release(g_recs[i].open_args);
    }
    free(g_recs);
    g_recs = NULL; g_cap = 0; g_n = 0;
}

static int64_t find_slot(int64_t fd) {
    for (int64_t i = 0; i < g_n; i++) if (g_recs[i].fd == fd) return i;
    return -1;
}

/* Redact a "host:port:user:password" descriptor: everything after the 3rd colon
 * becomes "***", and the user (between the 2nd and 3rd colon) is interned.  A
 * descriptor with fewer than 3 colons (a bare file/fifo path, or host:port) is
 * stored verbatim with an empty user. */
static ray_t* redact(const char* s, size_t n, int64_t* user_sym) {
    *user_sym = ray_sym_intern_runtime("", 0);
    int64_t c2 = -1, c3 = -1, seen = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] == ':') { seen++; if (seen == 2) c2 = (int64_t)i; else if (seen == 3) { c3 = (int64_t)i; break; } }
    if (c3 < 0) return ray_charv(s, (int64_t)n);
    *user_sym = ray_sym_intern_runtime(s + c2 + 1, (size_t)(c3 - c2 - 1));
    size_t head = (size_t)c3 + 1;
    char* buf = (char*)malloc(head + 3);
    if (!buf) return ray_charv(s, (int64_t)head);   /* head omits the password — never store cleartext */
    memcpy(buf, s, head);
    memcpy(buf + head, "***", 3);
    ray_t* r = ray_charv(buf, (int64_t)(head + 3));
    free(buf);
    return r;
}

int q_handles_register(int64_t fd, q_handle_kind kind, int initiated_out,
                       const char* args, size_t args_len) {
    int64_t existing = find_slot(fd);   /* an fd reused after close: overwrite */
    if (existing >= 0 && g_recs[existing].open_args) {
        ray_release(g_recs[existing].open_args);
        g_recs[existing].open_args = NULL;
    }
    if (existing < 0) {
        if (g_n == g_cap) {
            int64_t nc = g_cap ? g_cap * 2 : 16;
            q_handle_rec* nr = (q_handle_rec*)realloc(g_recs, (size_t)nc * sizeof *nr);
            if (!nr) return 0;   /* caller closes the fd — an unregistered raw handle is unusable */
            g_recs = nr; g_cap = nc;
        }
        existing = g_n++;
    }
    /* Credential redaction is IPC-specific (host:port:user:password).  A file /
     * fifo PATH may legitimately hold colons (`:logs/a:b:c`) and must be stored
     * verbatim with an empty user — only a socket descriptor is parsed. */
    int64_t user_sym;
    ray_t* red;
    if (kind == Q_HANDLE_SOCKET) {
        red = redact(args ? args : "", args ? args_len : 0, &user_sym);
    } else {
        user_sym = ray_sym_intern_runtime("", 0);
        red = ray_charv(args ? args : "", args ? (int64_t)args_len : 0);
    }
    g_recs[existing].fd            = fd;
    g_recs[existing].kind          = kind;
    g_recs[existing].initiated_out = initiated_out;
    g_recs[existing].user_sym      = user_sym;
    g_recs[existing].open_time_ns  = q_dotz_now_ns(0);
    g_recs[existing].open_args     = red;
    return 1;
}

void q_handles_deregister(int64_t fd) {
    int64_t i = find_slot(fd);
    if (i < 0) return;
    if (g_recs[i].open_args) ray_release(g_recs[i].open_args);
    g_recs[i] = g_recs[--g_n];   /* swap-remove */
}

int q_handles_kind(int64_t fd) {
    int64_t i = find_slot(fd);
    return i < 0 ? -1 : (int)g_recs[i].kind;
}

ray_t* q_handles_open_args(int64_t fd) {
    int64_t i = find_slot(fd);
    return i < 0 ? NULL : g_recs[i].open_args;
}

int64_t q_handles_user_sym(int64_t fd) {
    int64_t i = find_slot(fd);
    return i < 0 ? -1 : g_recs[i].user_sym;
}

/* ---- open: file/fifo transport (hopen `:path` / `:fifo://path`) ---------- */

#ifndef O_BINARY
#define O_BINARY 0           /* only Windows has a text mode to opt out of */
#endif

/* fcntl(fd, F_DUPFD, 3) — a duplicate >= 3, fd left open for the caller. */
#ifdef RAY_OS_WINDOWS
static int dup_above_std(int fd) {
    int low[3], n = 0, hi;
    while ((hi = _dup(fd)) >= 0 && hi < 3) low[n++] = hi;
    while (n--) _close(low[n]);
    return hi;
}
#else
static int dup_above_std(int fd) { return fcntl(fd, F_DUPFD, 3); }
#endif

/* POSIX open, NOT .ipc.open (IPC-only).  Handle = the real fd; registered so
 * apply/hclose/read1 recognise it.  Restricted mode refuses (writes the fs). */
ray_t* q_handles_open(const char* path, size_t plen, int is_fifo) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    char* p = (char*)malloc(plen + 1);
    if (!p) return q_err(QE_OOM);
    memcpy(p, path, plen); p[plen] = '\0';
    if (!is_fifo)                    /* hopen.md: a missing filepath "is created,
                                      * including directories" (fifos must exist) */
        q_io_mkdir_parents(p, plen);
    int flags = O_BINARY |           /* a handle writes/reads bytes VERBATIM */
                (is_fifo ? O_RDONLY : (O_WRONLY | O_CREAT | O_APPEND));
    int fd = open(p, flags, 0666);
    if (fd < 0) { free(p); return q_err(QE_IO); }
    if (fd < 3) {                    /* std fds closed: 0 is rejected by dispatch,
                                      * 1/2 are console handles — force fd >= 3 */
        int hi = dup_above_std(fd);
        close(fd);
        if (hi < 0) { free(p); return q_err(QE_IO); }
        fd = hi;
    }
    if (!q_handles_register((int64_t)fd, is_fifo ? Q_HANDLE_FIFO : Q_HANDLE_FILE, 1, p, plen)) {
        close(fd);                   /* an unregistered raw handle is unusable — fail cleanly */
        free(p);
        return q_err(QE_OOM);
    }
    free(p);
    return make_i64((int64_t)fd);
}

/* ---- apply: `h x` ------------------------------------------------------- */

/* write() the WHOLE buffer, retrying short writes and EINTR — a FIFO or large
 * payload can consume fewer bytes than asked without erroring.  0 ok, -1 error. */
static int write_all(int fd, const char* p, int64_t n) {
    int64_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, (size_t)(n - off));
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += w;
    }
    return 0;
}

/* Raw file write.  A POSITIVE handle writes the payload bytes verbatim — NO
 * newline framing (the primitive the streaming PR lacked); a NEGATIVE handle
 * appends '\n' after the string / after each list item (basics/handles.md:
 * `neg[h] x` appends x,"\n" / x,'"\n").  Returns the handle as applied. */
static ray_t* raw_write(int64_t qh, ray_t* y) {
    int fd = (int)(qh < 0 ? -qh : qh);
    int nl = qh < 0;
    const char* yp; int64_t yn;
    if (y && y->type == RAY_BYTE_ONLY) { yp = (const char*)ray_data(y); yn = ray_len(y); }
    else if (!(y && q_str_text_bytes(y, &yp, &yn))) yp = NULL;
    if (yp) {
        if (yn > 0 && write_all(fd, yp, yn) < 0) return q_err(QE_IO);
        if (nl && write_all(fd, "\n", 1) < 0) return q_err(QE_IO);
        return make_i64(qh);
    }
    if (y && (y->type == RAY_LIST || y->type == RAY_STR)) {
        int64_t m = ray_len(y);
        for (int64_t i = 0; i < m; i++) {
            ray_t* ia = make_i64(i);
            ray_t* it = ray_at_fn(y, ia);
            ray_release(ia);
            if (!it || RAY_IS_ERR(it)) return it ? it : q_err(QE_OOM);
            const char* ip; int64_t in_;
            if (!q_str_text_bytes(it, &ip, &in_)) { ray_release(it); return q_err(QE_TYPE); }
            if (in_ > 0 && write_all(fd, ip, in_) < 0) { ray_release(it); return q_err(QE_IO); }
            ray_release(it);
            if (nl && write_all(fd, "\n", 1) < 0) return q_err(QE_IO);
        }
        return make_i64(qh);
    }
    return q_err(QE_TYPE);
}

/* Console handles (kdb basics/handles.md): 1/-1 stdout, 2/-2 stderr, routed
 * to the q console sink; a NEGATIVE handle appends '\n' after each string. */
static ray_t* console_write_h(int64_t qh, ray_t* y) {
    int nl = qh < 0;
    const char* yp; int64_t yn;
    if (y && q_str_text_bytes(y, &yp, &yn)) {
        q_console_write(yp, (size_t)yn);
        if (nl) q_console_write("\n", 1);
    } else if (y && (y->type == RAY_LIST || y->type == RAY_STR)) {
        int64_t m = ray_len(y);
        for (int64_t i = 0; i < m; i++) {
            ray_t* ia = make_i64(i);
            ray_t* it = ray_at_fn(y, ia);
            ray_release(ia);
            if (!it || RAY_IS_ERR(it)) return it ? it : q_err(QE_OOM);
            const char* ip; int64_t in_;
            if (!q_str_text_bytes(it, &ip, &in_)) {
                ray_release(it);
                return q_err(QE_TYPE);
            }
            q_console_write(ip, (size_t)in_);
            if (nl) q_console_write("\n", 1);
            ray_release(it);
        }
    } else
        return q_err(QE_TYPE);
    return make_i64(qh);
}

/* The kind dispatch (q_handles.h contract).  File/fifo before IPC so a
 * filesystem fd never routes to `.ipc.*`; a Phase-1 fifo is a READER, so
 * writing it is a clean 'nyi, not an 'io.  A q handle IS the socket fd
 * (kdb-faithful, >= 3); translate to the poll selector id `.ipc.*` expect.
 * Those primitives are RAY_FN_RESTRICTED and called directly, so re-assert
 * restricted per arm (console handles stay usable under it). */
ray_t* q_handles_apply(int64_t qh, ray_t* y) {
    if (qh == 1 || qh == -1 || qh == 2 || qh == -2) return console_write_h(qh, y);
    int64_t afd = (qh == INT64_MIN) ? 0 : (qh < 0 ? -qh : qh);   /* neg h = same fd */
    if (afd >= 3) {
        int hk = q_handles_kind(afd);
        if (hk == Q_HANDLE_FILE) {
            if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
            return raw_write(qh, y);
        }
        if (hk == Q_HANDLE_FIFO) {
            if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
            return q_err(QE_NYI);
        }
    }
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    if (qh == 0 || qh == INT64_MIN || qh == INT32_MIN)   /* console-0 / int nulls */
        return q_err(QE_TYPE);
    int64_t fd  = (qh > 0) ? qh : -qh;         /* q handle is the socket fd */
    int64_t raw = ray_ipc_handle_of_fd(fd);    /* fd -> poll selector id */
    if (raw < 0)
        return q_err(QE_TYPE);
    ray_t* rawh = make_i64(raw);
    ray_t* r    = (qh > 0) ? ray_hsend_fn(rawh, y)   /* sync  */
                           : ray_hpost_fn(rawh, y);  /* async */
    ray_release(rawh);
    return r;
}

static int is_text_atom(ray_t* v) {
    return v && (v->type == -RAY_STR || v->type == RAY_CHARV ||
                 v->type == -RAY_CHARV);
}

/* The `:`-prefixed SYM arm of the same abstraction: protocol dispatch on the
 * descriptor text — ws/wss and http/https clients, else one-shot sync IPC
 * (ref/hopen.md): connect -> send -> close.  A file handle has no apply. */
ray_t* q_handles_sym_apply(ray_t* head, ray_t** args, int64_t n) {
    ray_t* s = ray_sym_str(head->i64);               /* borrowed */
    const char* sp = ray_str_ptr(s);
    size_t sl = ray_str_len(s);
    if ((sl >= 6 && memcmp(sp, ":ws://", 6) == 0) ||
        (sl >= 7 && memcmp(sp, ":wss://", 7) == 0)) {
        if (n == 1 && is_text_atom(args[0]))
            return q_ws_client_open(head, args[0]);
        return q_err(QE_TYPE);
    }
    if ((sl >= 8 && memcmp(sp, ":http://", 8) == 0) ||
        (sl >= 9 && memcmp(sp, ":https://", 9) == 0)) {
        if (n == 1 && is_text_atom(args[0]))
            return q_http_client_raw(head, args[0]);
        return q_err(QE_TYPE);
    }
    if (n == 1 && args[0] &&
        (args[0]->type == -RAY_STR || args[0]->type == RAY_CHARV)) {
        ray_t* h = q_hopen_wrap(head);           /* owned fd handle or error */
        if (!h || RAY_IS_ERR(h)) return h;
        ray_t* r;
        if (h->type == -RAY_I64 || h->type == -RAY_I32) {
            int64_t qh = (h->type == -RAY_I64) ? h->i64 : (int64_t)h->i32;
            r = q_handles_apply(qh, args[0]);    /* SYNC send */
        } else {
            r = q_err(QE_TYPE);
        }
        ray_t* c = q_hclose_wrap(h);             /* close regardless of r */
        if (c) ray_release(c);
        ray_release(h);
        return r;
    }
    return q_err(QE_TYPE);              /* file handle: no apply */
}

/* ---- close -------------------------------------------------------------- */

ray_t* q_handles_close(int64_t qh) {
    int k = q_handles_kind(qh);
    if (k == Q_HANDLE_FILE || k == Q_HANDLE_FIFO) {
        close((int)qh);
        q_handles_deregister(qh);
        return RAY_NULL_OBJ;
    }
    q_handles_deregister(qh);                 /* drop any socket record (no-op if absent) */
    int64_t id = ray_ipc_handle_of_fd(qh);
    if (id < 0) return RAY_NULL_OBJ;          /* not a live handle — no-op */
    ray_t* raw = make_i64(id);
    ray_t* r = ray_hclose_fn(raw);
    ray_release(raw);
    return r;
}

/* ---- read1 on a fifo handle --------------------------------------------- */

/* One blocking read of up to `count` bytes.  A fifo blocks for data; an empty
 * read is EOF (all writers closed) — the `.Q.fpn`/`.Q.fps` `while[count b]`
 * loop terminates on it.  Restricted mode refuses (reads the pipe). */
ray_t* q_handles_read1(int64_t fd, ray_t* count) {
    if (q_handles_kind(fd) != Q_HANDLE_FIFO) return NULL;
    int64_t want;
    if (!q_type_strict_i64(count, &want)) return q_err(QE_TYPE);
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    if (want <= 0) want = 65536;
    uint8_t* buf = (uint8_t*)malloc((size_t)want);
    if (!buf) return q_err(QE_OOM);
    ssize_t got = read((int)fd, buf, (size_t)want);
    if (got < 0) { free(buf); return q_err(QE_IO); }
    ray_t* out = ray_vec_from_raw(RAY_BYTE_ONLY, buf, (int64_t)got);
    free(buf);
    return out;
}

/* ---- the q verbs: hopen / hclose ---------------------------------------
 * (feat/q-ipc-client Phase D; hsym Bundle 2b; moved off ops/q_io.c 2026-07-31.)
 * hopen is a thin wrapper over `.ipc.open` (ray_hopen_fn), which takes a
 * "host:port[:user:password]" string + optional connect-timeout.  q `hopen`
 * accepts an int PORT (localhost), a "host:port[:user:pass]" STRING or the
 * equivalent hsym SYMBOL (both with the kdb "::PORT"/":host:port" leading-colon
 * conventions — the leading-`:` scanner lexes `` `::5000 ``/`` `:host:port `` as
 * one colon-bearing symbol, so both surfaces reach hopen_norm_descriptor and
 * share ONE parser), or a 2-list (conn; timeout-ms).  A `:path` / `:fifo://path`
 * descriptor is NOT IPC — hopen_transport routes it to q_handles_open above.
 * DEFERRED (clean 'nyi, not a silent TCP attempt): the transport schemes
 * `unix://` / `tcps://` / `unixs://`, which need a transport layer openq lacks,
 * and single-colon `` `fifo:path `` (kdb opens it non-blocking; see
 * hopen_transport). */

/* Normalize a "host:port[:user:password]" descriptor string (raw name text of a
 * string atom or an hsym symbol) into the form .ipc.open expects.  Applies the
 * kdb leading-colon conventions ("::rest" = localhost, ":rest" = strip one ':')
 * and rejects the not-yet-supported transport schemes with a clean 'nyi (so
 * BOTH the string and symbol surfaces decline them the same way, rather than
 * feeding e.g. "unix://5000" into a TCP connect).  Owned RAY_STR or error. */
static ray_t* hopen_norm_descriptor(const char* s, size_t n) {
    if (n > 512) return q_err(QE_DOMAIN);
    /* Strip the leading-colon marker: "::rest" localhost, single ":rest" strip 1. */
    const char* rest = s;
    size_t      rn   = n;
    bool        localhost = false;
    if (rn >= 2 && rest[0] == ':' && rest[1] == ':') { rest += 2; rn -= 2; localhost = true; }
    else if (rn >= 1 && rest[0] == ':')              { rest += 1; rn -= 1; }
    /* Deferred transports: a "scheme://" prefix after the leading colon(s). */
    static const char* const schemes[] = { "unixs://", "unix://", "tcps://", "fifo://" };
    for (size_t i = 0; i < sizeof schemes / sizeof *schemes; i++) {
        size_t sl = strlen(schemes[i]);
        if (rn >= sl && memcmp(rest, schemes[i], sl) == 0)
            return q_err(QE_NYI);   /* scheme sans "://" */
    }
    if (localhost) {
        char buf[600];
        int m = snprintf(buf, sizeof buf, "127.0.0.1:%.*s", (int)rn, rest);
        if (m <= 0 || m >= (int)sizeof buf)
            return q_err(QE_DOMAIN);
        return ray_str(buf, (size_t)m);
    }
    return ray_str(rest, rn);   /* "host:port[:user:pass]" (host omitted = as-is) */
}

/* Normalize a connection descriptor (int atom, string atom, or hsym symbol)
 * into the "host:port[:user:password]" form .ipc.open expects.  Owned RAY_STR
 * or error. */
static ray_t* hopen_connstr(ray_t* c) {
    if (q_type_is_int_atom(c)) {
        int64_t p = q_type_iatom_val(c);
        if (p <= 0 || p > 65535)
            return q_err(QE_DOMAIN);
        char buf[32];
        int m = snprintf(buf, sizeof buf, "127.0.0.1:%lld", (long long)p);
        if (m <= 0 || m >= (int)sizeof buf) return q_err(QE_DOMAIN);
        return ray_str(buf, (size_t)m);
    }
    if (c && c->type == -RAY_STR)
        return hopen_norm_descriptor(ray_str_ptr(c), ray_str_len(c));
    if (c && c->type == -RAY_SYM) {
        /* ray_sym_str returns a BORROWED interned string (table/sym.c) — do NOT
         * release it (that over-releases the sym table's own ref; the
         * q_io_file_path precedent).  hopen_norm_descriptor copies a new atom. */
        ray_t* nm = ray_sym_str(c->i64);
        if (!nm) return q_err(QE_TYPE);
        return hopen_norm_descriptor(ray_str_ptr(nm), ray_str_len(nm));
    }
    return q_err(QE_TYPE);
}

/* Classify a hopen descriptor into a transport (handle-registry Phase 1).  `s`/`n`
 * are the raw descriptor bytes (leading colon conventions intact).  On FILE/FIFO,
 * path and plen receive the filesystem path inside `s`.  IPC covers `::port`,
 * `:host:port[:user:pass]`, a bare `:port`, and the deferred `scheme://` forms
 * (the existing hopen_norm_descriptor rejects those with 'nyi); a `:fifo://path`
 * is a FIFO and any other `:path` is a FILE. */
enum { HT_IPC = 0, HT_FILE = 1, HT_FIFO = 2, HT_FIFO_NYI = 3 };
static int hopen_transport(const char* s, size_t n, const char** path, size_t* plen) {
    *path = s; *plen = n;
    if (n >= 2 && s[0] == ':' && s[1] == ':') return HT_IPC;   /* ::port localhost */
    size_t off = (n >= 1 && s[0] == ':') ? 1 : 0;
    const char* r = s + off;
    size_t      rn = n - off;
    if (rn >= 5 && memcmp(r, "fifo:", 5) == 0) {
        if (rn >= 7 && r[5] == '/' && r[6] == '/') { *path = r + 7; *plen = rn - 7; return HT_FIFO; }
        /* single-colon `fifo:path (read1.md): kdb's non-blocking fifo open is
         * NYI here — Phase-1 O_RDONLY would hang without a writer, and falling
         * through to FILE would create a junk file named "fifo:path". */
        return HT_FIFO_NYI;
    }
    static const char* const sch[] = { "unixs://", "unix://", "tcps://" };
    for (size_t i = 0; i < sizeof sch / sizeof *sch; i++)
        if (rn >= strlen(sch[i]) && memcmp(r, sch[i], strlen(sch[i])) == 0) return HT_IPC;
    int alldig = rn > 0;
    for (size_t i = 0; i < rn; i++) if (r[i] < '0' || r[i] > '9') { alldig = 0; break; }
    if (alldig) return HT_IPC;                                 /* bare :port */
    int64_t c1 = -1;
    for (size_t i = 0; i < rn; i++) if (r[i] == ':') { c1 = (int64_t)i; break; }
    if (c1 >= 0) {                                             /* host:PORT[...] iff field2 numeric */
        size_t fs = (size_t)c1 + 1, fe = fs;
        while (fe < rn && r[fe] != ':') fe++;
        int port = fe > fs;
        for (size_t i = fs; i < fe; i++) if (r[i] < '0' || r[i] > '9') { port = 0; break; }
        if (port) return HT_IPC;
    }
    *path = r; *plen = rn;
    return HT_FILE;
}

/* q `hopen y` — connect, return an int handle.  Restricted connections must not
 * open outbound sockets (the `.ipc.open` primitive is RAY_FN_RESTRICTED; calling
 * ray_hopen_fn directly bypasses the eval-layer check, so re-assert it here). */
static ray_t* hopen_wrap_impl(ray_t* x);
ray_t* q_hopen_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = hopen_wrap_impl(xs);
    ray_release(xs);
    return r;
}
static ray_t* hopen_wrap_impl(ray_t* x) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    ray_t* conn      = x;
    ray_t* timeout   = NULL;
    ray_t* pair_conn = NULL;   /* owned when a pair was a typed int VECTOR */
    ray_t* pair_to   = NULL;
    if (x && x->type == RAY_LIST && ray_len(x) == 2) {   /* (conn; timeout-ms) */
        ray_t** e = (ray_t**)ray_data(x);
        conn = e[0]; timeout = e[1];                     /* borrowed */
    } else if (q_type_is_int_vec(x) && ray_len(x) == 2) {
        /* an all-int (port; timeout-ms) pair collapses to a homogeneous int
         * VECTOR (not a general list) — recover the two atoms.  (A symbol/string
         * conn keeps the pair a RAY_LIST, handled above.) */
        pair_conn = ray_i64(q_type_ivec_get(x, 0));
        pair_to   = ray_i64(q_type_ivec_get(x, 1));
        conn = pair_conn; timeout = pair_to;
    }
    /* File/FIFO transport (Phase 1): a `:path` / `:fifo://path` descriptor opens a
     * filesystem fd directly, never an IPC socket.  A bare int port / `::port` /
     * `:host:port` classifies IPC and falls through. */
    if (conn && (conn->type == -RAY_STR || conn->type == -RAY_SYM)) {
        const char* ds; size_t dn;
        if (conn->type == -RAY_STR) { ds = ray_str_ptr(conn); dn = ray_str_len(conn); }
        else { ray_t* s = ray_sym_str(conn->i64);              /* borrowed — do not release */
               ds = s ? ray_str_ptr(s) : NULL; dn = s ? ray_str_len(s) : 0; }
        const char* path; size_t plen;
        int ht = ds ? hopen_transport(ds, dn, &path, &plen) : HT_IPC;
        if (ht != HT_IPC) {
            if (pair_conn) ray_release(pair_conn);
            if (pair_to)   ray_release(pair_to);
            if (ht == HT_FIFO_NYI)
                return q_err(QE_NYI);
            return q_handles_open(path, plen, ht == HT_FIFO);
        }
    }
    ray_t* cs = hopen_connstr(conn);                   /* owned or error */
    if (!cs || RAY_IS_ERR(cs)) {
        if (pair_conn) ray_release(pair_conn);
        if (pair_to)   ray_release(pair_to);
        return cs;
    }
    ray_t* args[2] = { cs, NULL };
    int64_t nargs = 1;
    ray_t* tv = NULL;
    int64_t tmo = 0;
    ray_t* terr = timeout ? q_type_i64_or_err(timeout, &tmo, "hopen: timeout") : NULL;
    if (terr) {
        ray_release(cs);
        if (pair_conn) ray_release(pair_conn);
        if (pair_to)   ray_release(pair_to);
        return terr;
    }
    if (timeout) {
        tv = make_i64(tmo);
        args[1] = tv; nargs = 2;
    }
    /* Snapshot the descriptor for the socket registry BEFORE cs is released (the
     * fd it keys off is only known after the connect + fd translation below). */
    char descbuf[640];
    size_t desclen = ray_str_len(cs);
    if (desclen >= sizeof descbuf) desclen = sizeof descbuf - 1;
    memcpy(descbuf, ray_str_ptr(cs), desclen);
    ray_t* h = ray_hopen_fn(args, nargs);                /* owned handle or error */
    ray_release(cs);
    if (tv)        ray_release(tv);
    if (pair_conn) ray_release(pair_conn);
    if (pair_to)   ray_release(pair_to);
    if (!h || RAY_IS_ERR(h)) return h;
    /* kdb-faithful "true fd" handle model: a q connection handle IS the socket
     * fd (qdocs basics/handles.md — 0 console, 1 stdout, 2 stderr, connections
     * at 3+).  ray_hopen_fn returns the rayfall poll SELECTOR ID (dense, starts
     * at 0); translate it to the connection's socket fd, which is always >= 3
     * (0/1/2 held by the std streams) and thus disjoint from the console-write
     * handles.  handle-apply / hclose translate the fd back to the selector id
     * the .ipc.* primitives expect. */
    int64_t raw = (h->type == -RAY_I64) ? h->i64 : (int64_t)h->i32;
    ray_release(h);
    int64_t fd = ray_ipc_fd_of_handle(raw);
    if (fd < 0) {
        /* Connection vanished between connect and fd lookup — close the raw
         * selector and surface, rather than hand back a bogus handle. */
        ray_t* rid = make_i64(raw);
        ray_t* cr = ray_hclose_fn(rid);
        ray_release(rid);
        if (cr) ray_release(cr);
        return q_err(QE_IO);
    }
    /* Outbound socket: capture the open-time metadata (redacted descriptor,
     * user).  The Phase-2 byte/msg counters + last-activity live in the frozen
     * read/write path and are NOT captured here. */
    (void)q_handles_register(fd, Q_HANDLE_SOCKET, 1, descbuf, desclen);  /* best-effort: the socket works via the IPC path regardless */
    return make_i64(fd);
}

/* q `hclose h` — validate the handle, then delegate the file/fifo-vs-IPC
 * dispatch to this file's q_handles_close.  Restricted connections are
 * refused, matching hopen / the handle-apply path. */
ray_t* q_hclose_wrap(ray_t* x) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    int64_t qh;
    if (!q_type_strict_i64(x, &qh) || RAY_ATOM_IS_NULL(x) || qh <= 0)
        return q_err(QE_TYPE);
    return q_handles_close(qh);
}
