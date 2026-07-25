/* q_handles — see q_handles.h.  A compact grow-on-demand array of open-time
 * handle records, fd-keyed by linear scan (a process holds few handles; kdb's
 * own connection tables are small).  Register on hopen-file/fifo/socket,
 * deregister on hclose.  Redaction runs at CAPTURE so a password never enters a
 * record. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_handles.h"
#include "qlang/q_err.h"
#include "qlang/q_registry_internal.h" /* q_str_text_bytes, q_type_strict_i64 */
#include "qlang/q_console.h" /* q_console_write — 1/-1/2/-2 console handles */
#include "qlang/q_dotz.h"   /* q_dotz_now_ns — the portable wall clock */
#include "lang/eval.h"       /* ray_eval_get_restricted, ray_at_fn */
#include "lang/internal.h"   /* make_i64, ray_hsend_fn/ray_hpost_fn/ray_hclose_fn */
#include "table/sym.h"       /* ray_sym_intern_runtime */
#include "store/fileio.h"    /* ray_mkdir_p — hopen creates missing directories */
#include "core/ipc.h"        /* ray_ipc_handle_of_fd — q true-fd handle -> selector id */
#include <rayforce.h>
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
    if (!is_fifo) {                  /* hopen.md: a missing filepath "is created,
                                      * including directories" (fifos must exist) */
        size_t cut = plen;
        while (cut > 0 && p[cut - 1] != '/') cut--;
        if (cut > 1) {
            p[cut - 1] = '\0';
            (void)ray_mkdir_p(p);
            p[cut - 1] = '/';
        }
    }
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
