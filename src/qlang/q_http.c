/* q_http — see q_http.h.  Behaviour pinned from qdocs kb/http.md + ref/dotz.md
 * (clean room); doc-unpinned details are openq-authored and recorded in the PR. */
#ifndef RAY_OS_WINDOWS
  #define _GNU_SOURCE            /* openat / O_NOFOLLOW / O_DIRECTORY */
#endif
#include <rayforce.h>
#include "qlang/q_http.h"
#include "qlang/q_gz.h"        /* q_gz_deflate — `form?` response gzip (#6) */
#include "qlang/q_dotz.h"      /* q_dotz_zph — the `.z.ph` handler slot */
#include "qlang/q_ws.h"        /* q_ws_handshake — the Upgrade hand-off */
#include "qlang/html_assets_gen.h" /* q_html_assets[] — codegen'd from src/qlang/html/ */
#include "qlang/q_fmt.h"       /* q_console_str/_reset — drain handler show output */
#include "lang/env.h"          /* ray_env_get / ray_sym_intern — `.h.HOME` / `.h.ty` */
#include "lang/internal.h"     /* call_fn1 */
#include "core/runtime.h"      /* __VM — env lookups require a bound per-thread VM */
#include "mem/sys.h"
#include "table/dict.h"        /* ray_dict_find_sym — `.h.ty` override probe */
#include "picohttpparser.h"
#include <string.h>
#include <stdio.h>

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/socket.h>
  #include <poll.h>
  #include <errno.h>
#endif
#include <time.h>

#define Q_HTTP_MAX_HEADERS 64
#define Q_HTTP_MAX_PATH    1024
#define Q_HTTP_MAX_FILE    (32ll * 1024 * 1024)   /* served-file / POST-body cap (slice) */
#define Q_HTTP_SEND_SECS   30                     /* whole-response send deadline */
#define Q_HTTP_RECV_SECS   30                     /* whole POST-body read deadline */
#define Q_HTTP_GZIP_MIN    2000                   /* body chars before gzip (kb/http.md L54, V4.0) */
#define Q_HTTP_GZIP_LEVEL  6                      /* deflate level (doc-neutral; balanced) */

/* Bounded-deadline send: ray_sock_send waits FOREVER, so a stalled reader on a
 * 32 MiB body would wedge the poll loop — sock.c's loop + a deadline.  0/-1.
 * Exported (q_http.h): q_ws.c frames ride the same bounded-send policy. */
int q_http_send_all(ray_sock_t fd, const void* buf, size_t len, int secs) {
    const uint8_t* p   = (const uint8_t*)buf;
    size_t         rem = len;
    time_t         end = time(NULL) + secs;
    while (rem > 0) {
#ifdef RAY_OS_WINDOWS
        int n = send(fd, (const char*)p, (int)rem, 0);
        int e = (n < 0) ? WSAGetLastError() : 0;
        bool again = (e == WSAEWOULDBLOCK);
        bool intr  = (e == WSAEINTR);
#else
        ssize_t n = send(fd, p, rem, MSG_NOSIGNAL);
        bool again = (n < 0) && (errno == EAGAIN || errno == EWOULDBLOCK);
        bool intr  = (n < 0) && (errno == EINTR);
#endif
        if (n < 0) {
            if (intr) continue;
            if (!again) return -1;
            time_t now = time(NULL);
            if (now >= end) return -1;
#ifdef RAY_OS_WINDOWS
            WSAPOLLFD pfd = { fd, POLLOUT, 0 };
            if (WSAPoll(&pfd, 1, (INT)((end - now) * 1000)) <= 0) return -1;
#else
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            if (poll(&pfd, 1, (int)((end - now) * 1000)) <= 0) return -1;
#endif
            continue;
        }
        p   += n;
        rem -= (size_t)n;
    }
    return 0;
}

/* Bounded-deadline body read: the frozen ipc.c accumulator stops at the header
 * terminator (\r\n\r\n), so a POST body is still unread in the socket.  fd is
 * non-blocking in the poll loop — poll for readability like http_send does for
 * writability.  Synchronous in-poll read is a temporary degradation (a slow
 * POST stalls the loop); the async body state machine belongs in frozen ipc.c.
 * Returns 0 (want bytes read) / -1 (error/timeout/early close). */
static int http_recv_n(ray_sock_t fd, uint8_t* buf, size_t want) {
    size_t got = 0;
    time_t end = time(NULL) + Q_HTTP_RECV_SECS;
    while (got < want) {
#ifdef RAY_OS_WINDOWS
        int n = recv(fd, (char*)buf + got, (int)(want - got), 0);
        int e = (n < 0) ? WSAGetLastError() : 0;
        bool again = (e == WSAEWOULDBLOCK);
        bool intr  = (e == WSAEINTR);
#else
        ssize_t n = recv(fd, buf + got, want - got, 0);
        bool again = (n < 0) && (errno == EAGAIN || errno == EWOULDBLOCK);
        bool intr  = (n < 0) && (errno == EINTR);
#endif
        if (n == 0) return -1;                    /* peer closed before full body */
        if (n < 0) {
            if (intr) continue;
            if (!again) return -1;
            time_t now = time(NULL);
            if (now >= end) return -1;
#ifdef RAY_OS_WINDOWS
            WSAPOLLFD pfd = { fd, POLLIN, 0 };
            if (WSAPoll(&pfd, 1, (INT)((end - now) * 1000)) <= 0) return -1;
#else
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            if (poll(&pfd, 1, (int)((end - now) * 1000)) <= 0) return -1;
#endif
            continue;
        }
        got += (size_t)n;
    }
    return 0;
}

static int hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int q_http_decode_path(const char* in, size_t n, char* out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        unsigned c = (unsigned char)in[i];
        if (c == '%') {
            if (i + 2 >= n) return -1;
            int h = hexval((unsigned char)in[i + 1]);
            int l = hexval((unsigned char)in[i + 2]);
            if (h < 0 || l < 0) return -1;
            c = (unsigned)(h * 16 + l);
            i += 3;
        } else {
            i++;
        }
        if (c < 0x20 || c == 0x7f) return -1;          /* NUL / control bytes */
        if (o + 1 >= outsz) return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return (int)o;
}

bool q_http_path_ok(const char* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && p[j] != '/') {
            if (p[j] == '\\' || p[j] == ':') return false;
            j++;
        }
        size_t sl = j - i;
        if (sl == 1 && p[i] == '.') return false;
        if (sl == 2 && p[i] == '.' && p[i + 1] == '.') return false;
        i = j + 1;
    }
    return true;
}

static bool str_ieq(const char* s, size_t n, const char* lower_tok) {
    size_t tn = strlen(lower_tok);
    if (n != tn) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c != (unsigned char)lower_tok[i]) return false;
    }
    return true;
}

const char* q_http_mime_type(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot || !dot[1]) return "application/octet-stream";
    /* Covers kdb .h.ty's extensions (ref/doth.md: htm/html/csv/txt/xml/xls/gif...)
     * plus the web essentials a static server needs; VALUES are modern-correct
     * (kdb's .h.ty is dated, e.g. xml->text/plain) since this serves live browsers. */
    static const struct { const char* ext; const char* ty; } M[] = {
        { "html", "text/html" },   { "htm",  "text/html" },
        { "css",  "text/css" },    { "js",   "application/javascript" },
        { "mjs",  "application/javascript" },
        { "png",  "image/png" },   { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },  { "gif",  "image/gif" },
        { "svg",  "image/svg+xml" }, { "ico", "image/x-icon" },
        { "webp", "image/webp" },
        { "json", "application/json" }, { "txt", "text/plain" },
        { "csv",  "text/csv" },     { "xml",  "application/xml" },
        { "pdf",  "application/pdf" }, { "wasm", "application/wasm" },
        { "woff", "font/woff" },    { "woff2", "font/woff2" },
        { "xls",  "application/vnd.ms-excel" },
    };
    const char* ext = dot + 1;
    for (size_t i = 0; i < sizeof M / sizeof *M; i++)
        if (str_ieq(ext, strlen(ext), M[i].ext)) return M[i].ty;
    return "application/octet-stream";
}

/* Read a name from the global q env, but ONLY when a per-thread VM is bound —
 * ray_env_get walks the scope stack via __VM and derefs it unconditionally.  The
 * live server always serves on the runtime's main thread (VM bound), so `.h.*`
 * resolves; a caller with no bound VM (the pure-C-seam unit tests) degrades to
 * the fallback instead of crashing.  Borrowed ref (never release). */
static ray_t* http_env_get(int64_t sym_id) {
    return __VM ? ray_env_get(sym_id) : NULL;
}

/* Copy a RAY_STR ATOM's bytes into out[] (NUL-terminated) iff every byte is a
 * safe, non-control character (>= 0x20 and != 0x7f).  Rejects CR/LF (HTTP
 * header injection) and embedded NUL (silent truncation).  false = wrong type /
 * empty / oversize / control byte -> caller uses its own fallback.  SSO: accepts
 * ONLY a string atom (-RAY_STR), never a RAY_STR column, and reads it through
 * ray_str_ptr/_len (never ray_len). */
static bool http_str_atom_safe(ray_t* v, char* out, size_t outsz) {
    if (!v || RAY_IS_ERR(v) || v->type != -RAY_STR) return false;
    size_t n = ray_str_len(v);
    if (n == 0 || n >= outsz) return false;
    const char* s = ray_str_ptr(v);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

/* `.z.ac` return `(status; payload)` parser — see q_http.h.  Accepts any signed
 * integer atom for status; payload must be a RAY_STR atom.  Structural only. */
bool q_http_zac_parse(const ray_t* r, int64_t* status_out,
                      const char** pay_out, size_t* paylen_out) {
    if (!r || RAY_IS_ERR(r) || r->type != RAY_LIST || ray_len(r) != 2)
        return false;
    ray_t** it = (ray_t**)ray_data((ray_t*)r);
    ray_t*  s  = it[0];
    ray_t*  pv = it[1];
    if (!s || !pv) return false;
    int64_t st;
    switch (s->type) {
        case -RAY_U8:  st = (int64_t)s->u8;  break;
        case -RAY_I16: st = (int64_t)s->i16; break;
        case -RAY_I32: st = (int64_t)s->i32; break;
        case -RAY_I64: st = s->i64;          break;
        default: return false;
    }
    if (pv->type != -RAY_STR) return false;
    *status_out = st;
    *pay_out    = ray_str_ptr(pv);
    *paylen_out = ray_str_len(pv);
    return true;
}

const char* q_http_docroot(char* buf, size_t bufsz) {
    ray_t* home = http_env_get(ray_sym_intern(".h.HOME", 7));  /* borrowed, NULL if unset */
    if (http_str_atom_safe(home, buf, bufsz)) return buf;
    if (bufsz > 4) { memcpy(buf, "html", 5); return buf; }     /* fallback (incl. NUL) */
    return "html";
}

const char* q_http_mime_lookup(const char* path, char* scratch, size_t scratchsz) {
    const char* dot = strrchr(path, '.');
    if (dot && dot[1]) {
        char   low[24];
        size_t el = strlen(dot + 1);
        if (el > 0 && el < sizeof low) {
            for (size_t i = 0; i < el; i++) {         /* `.h.ty` keys are lowercase syms */
                unsigned char c = (unsigned char)dot[1 + i];
                low[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
            }
            ray_t* ty = http_env_get(ray_sym_intern(".h.ty", 5));  /* borrowed */
            if (ty && !RAY_IS_ERR(ty) && ty->type == RAY_DICT) {
                ray_t* key = ray_sym(ray_sym_intern(low, el));     /* owned atom */
                ray_t* v   = key ? ray_dict_get(ty, key) : NULL;   /* owned or NULL (miss) */
                if (key) ray_release(key);
                bool ok = http_str_atom_safe(v, scratch, scratchsz);
                if (v) { if (RAY_IS_ERR(v)) ray_error_free(v); else ray_release(v); }
                if (ok) return scratch;
            }
        }
    }
    return q_http_mime_type(path);
}

int q_http_open_doc(const char* rel, size_t n, int64_t* size_out) {
    if (!q_http_path_ok(rel, n)) return -1;             /* defense in depth */

    char rootbuf[Q_HTTP_MAX_PATH];
    const char* root = q_http_docroot(rootbuf, sizeof rootbuf);   /* `.h.HOME` or "html" */
#ifndef RAY_OS_WINDOWS
    int dir = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) return -1;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && rel[j] != '/') j++;
        size_t sl = j - i;
        if (sl == 0) { i = j + 1; continue; }
        char seg[256];
        if (sl >= sizeof seg) { close(dir); return -1; }
        memcpy(seg, rel + i, sl);
        seg[sl] = '\0';
        bool more = false;                              /* intermediate seg must be a dir */
        for (size_t k = j; k < n; k++)
            if (rel[k] != '/') { more = true; break; }
        int fd = openat(dir, seg,
                        O_RDONLY | O_NOFOLLOW | (more ? O_DIRECTORY : 0));
        close(dir);
        if (fd < 0) return -1;                          /* missing or symlink (ELOOP) */
        dir = fd;
        i = j + 1;
    }
    struct stat st;
    if (fstat(dir, &st) != 0 || !S_ISREG(st.st_mode)) { close(dir); return -1; }
    if (size_out) *size_out = (int64_t)st.st_size;
    return dir;
#else
    char path[2 * Q_HTTP_MAX_PATH];
    int  pl = snprintf(path, sizeof path, "%s/%.*s", root, (int)n, rel);
    if (pl < 0 || (size_t)pl >= sizeof path) return -1;
    int fd = _open(path, _O_RDONLY | _O_BINARY);
    if (fd < 0) return -1;
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0 || (st.st_mode & _S_IFMT) != _S_IFREG) {
        _close(fd);
        return -1;
    }
    if (size_out) *size_out = (int64_t)st.st_size;
    return fd;
#endif
}

void q_http_send_simple(ray_sock_t fd, int code, const char* reason) {
    char body[128];
    char hdr[256];
    int bl = snprintf(body, sizeof body, "%d %s\n", code, reason);
    int hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n",
                      code, reason, bl);
    if (bl < 0 || hl < 0) return;
    if (q_http_send_all(fd, hdr, (size_t)hl, Q_HTTP_SEND_SECS) == 0)
        q_http_send_all(fd, body, (size_t)bl, Q_HTTP_SEND_SECS);
}

/* True when there is NO docroot at all (the `.h.HOME`/"html" dir is absent, not a
 * missing file inside one) — drives the built-in landing page below. */
static bool http_docroot_absent(void) {
    char rootbuf[Q_HTTP_MAX_PATH];
    const char* root = q_http_docroot(rootbuf, sizeof rootbuf);
#ifndef RAY_OS_WINDOWS
    int d = open(root, O_RDONLY | O_DIRECTORY);
    if (d >= 0) { close(d); return false; }
    return errno == ENOENT;
#else
    struct _stat64 st;
    return !(_stat64(root, &st) == 0 && (st.st_mode & _S_IFMT) == _S_IFDIR);
#endif
}

const unsigned char* q_http_asset_lookup(const char* path, size_t* len_out) {
    if (!path) return NULL;
    while (*path == '/') path++;                 /* strip leading slash(es) */
    if (*path == '\0') path = "index.html";      /* "/" or "" -> index */
    for (size_t i = 0; i < q_html_assets_count; i++)
        if (strcmp(path, q_html_assets[i].path) == 0) {
            if (len_out) *len_out = q_html_assets[i].len;
            return q_html_assets[i].bytes;
        }
    return NULL;
}

size_t q_http_asset_count(void) { return q_html_assets_count; }

bool q_http_asset_at(size_t i, const char** path_out,
                     const unsigned char** bytes_out, size_t* len_out) {
    if (i >= q_html_assets_count) return false;
    if (path_out)  *path_out  = q_html_assets[i].path;
    if (bytes_out) *bytes_out = q_html_assets[i].bytes;
    if (len_out)   *len_out   = q_html_assets[i].len;
    return true;
}

/* Serve one request from the embedded table (no on-disk docroot). `rel` is the
 * respond()-normalized path (leading '/' stripped; "/" -> "index.html"). Hit ->
 * 200 with Content-Length + Content-Type (`.h.ty` override, else the C table);
 * miss -> 404. Fixed-table match: no filesystem, no traversal surface. */
static void serve_embedded(ray_sock_t fd, const char* rel) {
    size_t len = 0;
    const unsigned char* body = q_http_asset_lookup(rel, &len);
    if (!body) { q_http_send_simple(fd, 404, "Not Found"); return; }
    char hdr[256], mimebuf[64];
    int hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      q_http_mime_lookup(rel, mimebuf, sizeof mimebuf), len);
    if (hl > 0 && q_http_send_all(fd, hdr, (size_t)hl, Q_HTTP_SEND_SECS) == 0 && len)
        q_http_send_all(fd, body, len, Q_HTTP_SEND_SECS);
}

static void serve_file(ray_sock_t fd, const char* rel, size_t rl) {
    if (http_docroot_absent()) {         /* ALL-OR-NOTHING: no ./html -> embedded site */
        serve_embedded(fd, rel);
        return;
    }
    int64_t size = 0;
    int doc = q_http_open_doc(rel, rl, &size);
    if (doc < 0) {                        /* docroot present but file missing -> 404 */
        q_http_send_simple(fd, 404, "Not Found");
        return;
    }
    if (size > Q_HTTP_MAX_FILE) {
#ifndef RAY_OS_WINDOWS
        close(doc);
#else
        _close(doc);
#endif
        q_http_send_simple(fd, 500, "Internal Server Error");
        return;
    }
    uint8_t* buf = size ? (uint8_t*)ray_sys_alloc((size_t)size) : NULL;
    int64_t  got = 0;
    if (size && !buf) got = -1;
    while (got >= 0 && got < size) {
#ifndef RAY_OS_WINDOWS
        int64_t r = (int64_t)read(doc, buf + got, (size_t)(size - got));
#else
        int64_t r = (int64_t)_read(doc, (char*)buf + got, (unsigned)(size - got));
#endif
        if (r <= 0) { got = -1; break; }
        got += r;
    }
#ifndef RAY_OS_WINDOWS
    close(doc);
#else
    _close(doc);
#endif
    if (got < 0) {
        if (buf) ray_sys_free(buf);
        q_http_send_simple(fd, 500, "Internal Server Error");
        return;
    }
    char hdr[256];
    char mimebuf[64];
    int hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                      "Content-Length: %lld\r\nConnection: close\r\n\r\n",
                      q_http_mime_lookup(rel, mimebuf, sizeof mimebuf),
                      (long long)size);
    if (hl > 0) {
        if (q_http_send_all(fd, hdr, (size_t)hl, Q_HTTP_SEND_SECS) == 0 && size)
            q_http_send_all(fd, buf, (size_t)size, Q_HTTP_SEND_SECS);
    }
    if (buf) ray_sys_free(buf);
}

/* Build a handler arg (ref/dotz.md; PROVISIONAL pre-C3: string ATOMS).
 *   method != NULL -> 3-item (methodSym; text; sym-keyed hdrDict) for `.z.pm`;
 *   method == NULL -> 2-item (text; hdrDict) for `.z.ph`/`.z.pp`/`.z.ac`.
 * Returns an OWNED list (rc=1; caller releases) or NULL on ANY failure — every
 * intermediate (incl. error objects from the constructors/append) is freed
 * internally, so the caller only tests NULL and never receives a RAY_IS_ERR. */
static ray_t* zh_build_arg(const char* method, size_t mlen,
                           const char* text_p, size_t text_len,
                           const struct phr_header* hdrs, size_t nh)
{
    /* picohttpparser sets name==NULL for an obsolete folded header — skip it. */
    size_t nk = 0;
    for (size_t i = 0; i < nh; i++) if (hdrs[i].name) nk++;

    ray_t* msym = method ? ray_sym(ray_sym_intern(method, mlen)) : NULL;
    ray_t* text = ray_str(text_p, text_len);
    ray_t* keys = ray_vec_new(RAY_SYM, nk > 0 ? (int64_t)nk : 1);
    ray_t* vals = ray_list_new(nk > 0 ? (int64_t)nk : 1);
    ray_t* arg  = ray_list_new(method ? 3 : 2);
    bool bad = (method && (!msym || RAY_IS_ERR(msym))) ||
               !text || !keys || !vals || !arg ||
               RAY_IS_ERR(text) || RAY_IS_ERR(keys) ||
               RAY_IS_ERR(vals) || RAY_IS_ERR(arg);
    if (!bad) {
        keys->len = (int64_t)nk;
        int64_t* kd = (int64_t*)ray_data(keys);
        size_t k = 0;
        for (size_t i = 0; i < nh && !bad; i++) {
            if (!hdrs[i].name) continue;
            kd[k++] = ray_sym_intern(hdrs[i].name, hdrs[i].name_len);
            ray_t* v = ray_str(hdrs[i].value, hdrs[i].value_len);
            if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); bad = true; break; }
            ray_t* nv = ray_list_append(vals, v);   /* append RETAINS */
            ray_release(v);
            if (RAY_IS_ERR(nv)) { ray_error_free(nv); bad = true; break; }
            vals = nv;
        }
    }
    ray_t* hdrd = NULL;
    if (!bad) {
        hdrd = ray_dict_new(keys, vals);       /* consumes keys+vals */
        keys = vals = NULL;
        if (!hdrd || RAY_IS_ERR(hdrd)) bad = true;
    }
    /* append RETAINS; on error it returns a fresh error object (the input list
     * stays owned by us) — free that error, keep `arg`, and clean up below. */
    if (!bad && method) {
        ray_t* a0 = ray_list_append(arg, msym);      /* method symbol first */
        if (RAY_IS_ERR(a0)) { ray_error_free(a0); bad = true; } else arg = a0;
    }
    if (!bad) {
        ray_t* a1 = ray_list_append(arg, text);
        if (RAY_IS_ERR(a1)) { ray_error_free(a1); bad = true; } else arg = a1;
    }
    if (!bad) {
        ray_t* a2 = ray_list_append(arg, hdrd);
        if (RAY_IS_ERR(a2)) { ray_error_free(a2); bad = true; } else arg = a2;
    }
    /* free both plain and error objects (constructors may return either). */
    #define ZH_DROP(o) do { if (o) { if (RAY_IS_ERR(o)) ray_error_free(o); \
                                     else ray_release(o); } } while (0)
    if (bad) {
        ZH_DROP(msym); ZH_DROP(text); ZH_DROP(keys); ZH_DROP(vals);
        ZH_DROP(hdrd); ZH_DROP(arg);
        #undef ZH_DROP
        return NULL;
    }
    if (msym) ray_release(msym);   /* appended into arg (retained) — drop local */
    ray_release(text);
    ray_release(hdrd);
    return arg;
}

/* Request offers `Accept-Encoding: gzip` with a non-zero q-value (kb/http.md L54).
 * Comma-token scan; a `gzip;q=0`/`q=0.0` qualifier means NOT acceptable. */
static bool accept_encoding_has_gzip(const struct phr_header* hdrs, size_t nh) {
    for (size_t i = 0; i < nh; i++) {
        if (!hdrs[i].name || !str_ieq(hdrs[i].name, hdrs[i].name_len, "accept-encoding"))
            continue;
        const char* v = hdrs[i].value; size_t n = hdrs[i].value_len;
        for (size_t s = 0; s < n; ) {
            size_t e = s; while (e < n && v[e] != ',') e++;          /* one token */
            size_t ts = s, te = e;
            while (ts < te && (v[ts] == ' ' || v[ts] == '\t')) ts++;
            size_t ce = ts; while (ce < te && v[ce] != ';') ce++;    /* coding vs params */
            size_t ce2 = ce; while (ce2 > ts && (v[ce2-1] == ' ' || v[ce2-1] == '\t')) ce2--;
            if (str_ieq(v + ts, ce2 - ts, "gzip")) {
                bool q0 = false;                                     /* ;q=0 (not 0.x>0) */
                for (size_t p = ce; p + 1 < te; p++)
                    if ((v[p] == 'q' || v[p] == 'Q') && v[p+1] == '=') {
                        size_t d = p + 2; while (d < te && (v[d] == ' ' || v[d] == '\t')) d++;
                        if (d < te && v[d] == '0') {
                            size_t f = d + 1; bool nz = false;
                            if (f < te && v[f] == '.')
                                for (size_t g = f + 1; g < te; g++) if (v[g] >= '1' && v[g] <= '9') nz = true;
                            q0 = !nz;
                        }
                        break;
                    }
                if (!q0) return true;
            }
            s = (e < n) ? e + 1 : e;
        }
        return false;   /* header present but no acceptable gzip token */
    }
    return false;
}

/* Compress the handler's complete HTTP response (`form?` path, #6) when it is a
 * parseable response whose plaintext body is >= Q_HTTP_GZIP_MIN and carries no
 * Transfer-Encoding / existing Content-Encoding.  Rebuilds: status line + every
 * original header except Content-Length/Content-Encoding, then a recomputed
 * Content-Length, `Content-Encoding: gzip`, and the gzipped body.  Returns a
 * malloc'd buffer (caller frees, *out set) or NULL to send the original verbatim. */
static uint8_t* http_gzip_response(const char* resp, size_t len, size_t* out) {
    int minor, st; const char* msg; size_t mlen2, nh = Q_HTTP_MAX_HEADERS;
    struct phr_header h[Q_HTTP_MAX_HEADERS];
    int hl = phr_parse_response(resp, len, &minor, &st, &msg, &mlen2, h, &nh, 0);
    if (hl <= 0) return NULL;                          /* not a full response */
    for (size_t i = 0; i < nh; i++) {
        if (!h[i].name) continue;
        if (str_ieq(h[i].name, h[i].name_len, "transfer-encoding") ||
            str_ieq(h[i].name, h[i].name_len, "content-encoding"))
            return NULL;                               /* chunked / already-coded: skip */
    }
    const char* body = resp + hl; size_t blen = len - (size_t)hl;
    if (blen < Q_HTTP_GZIP_MIN) return NULL;           /* threshold (kb/http.md L54) */

    size_t gzlen = 0; const char* gerr = NULL;
    uint8_t* gz = q_gz_deflate((const uint8_t*)body, blen, Q_HTTP_GZIP_LEVEL, &gzlen, &gerr);
    if (!gz) return NULL;                              /* deflate failed -> verbatim */

    /* status line = resp up to and including the first CRLF (within the header block). */
    size_t sl = 0; while (sl + 1 < (size_t)hl && !(resp[sl] == '\r' && resp[sl+1] == '\n')) sl++;
    sl += 2;                                           /* include CRLF */

    /* upper bound: kept status+headers re-serialized as "name: value\r\n" can add up
     * to 1 byte/header over the original block (phr strips OWS) — bound that by
     * Q_HTTP_MAX_HEADERS, plus the two appended lines + final CRLF + gz body.  The
     * header-copy loop below is unchecked memcpy, so cap MUST cover it. */
    size_t cap = (size_t)hl + (size_t)Q_HTTP_MAX_HEADERS + 128 + gzlen;
    uint8_t* o = (uint8_t*)malloc(cap);
    if (!o) { free(gz); return NULL; }
    size_t p = 0;
    memcpy(o + p, resp, sl); p += sl;                  /* status line */
    for (size_t i = 0; i < nh; i++) {                  /* kept headers, verbatim casing */
        if (!h[i].name) continue;
        if (str_ieq(h[i].name, h[i].name_len, "content-length")) continue;
        memcpy(o + p, h[i].name, h[i].name_len);  p += h[i].name_len;
        o[p++] = ':'; o[p++] = ' ';
        memcpy(o + p, h[i].value, h[i].value_len); p += h[i].value_len;
        o[p++] = '\r'; o[p++] = '\n';
    }
    int nl = snprintf((char*)o + p, cap - p,
                      "Content-Length: %zu\r\nContent-Encoding: gzip\r\n\r\n", gzlen);
    if (nl < 0 || (size_t)nl >= cap - p) { free(o); free(gz); return NULL; }
    p += (size_t)nl;
    memcpy(o + p, gz, gzlen); p += gzlen;
    free(gz);
    *out = p;
    return o;
}

/* Shared `.z.ph`/`.z.pp`/`.z.pm` dispatch core (ref/dotz.md): call fn (BORROWED,
 * caller-retained) with the built arg, write its returned response string
 * VERBATIM.  Error/non-string -> 500.  Always returns 0 (a response was sent).
 * `method` (NULL for .z.ph/.z.pp) selects the 2- vs 3-item arg; `which` names
 * the handler for the log.  `may_gzip` (only the `.z.ph` GET path) enables the
 * `form?`-response gzip when the client offered Accept-Encoding + body >= 2000. */
static int zh_dispatch_call(ray_sock_t fd, const char* method, size_t mlen,
                            const char* text_p, size_t text_len,
                            const struct phr_header* hdrs, size_t nh,
                            ray_t* fn, const char* which, bool may_gzip)
{
    ray_t* arg = zh_build_arg(method, mlen, text_p, text_len, hdrs, nh);
    if (!arg) { q_http_send_simple(fd, 500, "Internal Server Error"); return 0; }

    ray_t* r = call_fn1(fn, arg);
    ray_release(arg);
    /* drain handler show/0N! to the server console (q_zts_tick pattern) */
    { const char* con = q_console_str();
      if (con && *con) { fputs(con, stdout); fflush(stdout); }
      q_console_reset(); }

    if (r && !RAY_IS_ERR(r) && r->type == -RAY_STR) {
        const char* rp = ray_str_ptr(r); size_t rn = ray_str_len(r);
        uint8_t* gz = NULL; size_t gn = 0;
        if (may_gzip && accept_encoding_has_gzip(hdrs, nh))
            gz = http_gzip_response(rp, rn, &gn);      /* NULL -> send verbatim */
        if (gz) { q_http_send_all(fd, gz, gn, Q_HTTP_SEND_SECS); free(gz); }
        else      q_http_send_all(fd, rp, rn, Q_HTTP_SEND_SECS);
        ray_release(r);
        return 0;
    }
    fprintf(stderr, "http: %s returned %s — sending 500\n", which,
            (r && RAY_IS_ERR(r)) ? "an error" : "a non-string");
    if (r) {
        if (RAY_IS_ERR(r)) ray_error_free(r);
        else if (r != RAY_NULL_OBJ) ray_release(r);
    }
    q_http_send_simple(fd, 500, "Internal Server Error");
    return 0;
}

/* `.z.ph` (HTTP GET) — requestText is the request target (leading '/' stripped).
 * 0 handled / -1 no handler set. */
static int zph_dispatch(ray_sock_t fd, const char* target, size_t tlen,
                        const struct phr_header* hdrs, size_t nh)
{
    ray_t* fn = q_dotz_zph();                  /* borrowed, NULL = unset */
    if (!fn) return -1;
    ray_retain(fn);                            /* handler may reassign .z.ph */
    if (tlen && target[0] == '/') { target++; tlen--; }
    int r = zh_dispatch_call(fd, NULL, 0, target, tlen, hdrs, nh, fn, ".z.ph", true);
    ray_release(fn);
    return r;
}

/* `.z.pp` (HTTP POST) — the request body (read from the socket; the frozen
 * ipc.c accumulator stops at the header terminator) is requestText.  No `.z.pp`
 * set -> 501 (openq policy; ref/dotz.md pins no default).  Transfer-Encoding on
 * the request -> 400 (chunked request bodies deferred).  Sends a response in
 * every branch. */
static void zpp_dispatch(ray_sock_t fd, const struct phr_header* hdrs, size_t nh)
{
    ray_t* fn = q_dotz_zpp();                  /* borrowed, NULL = unset */
    if (!fn) { q_http_send_simple(fd, 501, "Not Implemented"); return; }

    int64_t cl = 0; bool have_cl = false;
    for (size_t i = 0; i < nh; i++) {
        if (!hdrs[i].name) continue;
        if (str_ieq(hdrs[i].name, hdrs[i].name_len, "transfer-encoding")) {
            q_http_send_simple(fd, 400, "Bad Request"); return;
        }
        if (str_ieq(hdrs[i].name, hdrs[i].name_len, "content-length")) {
            /* strict: leading/trailing OWS only, all-digit body (an embedded
             * space would let a client under-declare and stall the read). */
            const char* vp = hdrs[i].value; size_t vn = hdrs[i].value_len;
            size_t s = 0; while (s < vn && (vp[s] == ' ' || vp[s] == '\t')) s++;
            size_t e = vn; while (e > s && (vp[e-1] == ' ' || vp[e-1] == '\t')) e--;
            if (s == e) { q_http_send_simple(fd, 400, "Bad Request"); return; }
            int64_t v = 0; bool over = false;
            for (size_t k = s; k < e; k++) {
                if (vp[k] < '0' || vp[k] > '9') { q_http_send_simple(fd, 400, "Bad Request"); return; }
                v = v * 10 + (vp[k] - '0');
                if (v > Q_HTTP_MAX_FILE) { over = true; break; }
            }
            if (over) { q_http_send_simple(fd, 413, "Payload Too Large"); return; }
            if (have_cl && v != cl) { q_http_send_simple(fd, 400, "Bad Request"); return; }
            have_cl = true; cl = v;
        }
    }

    ray_retain(fn);                            /* handler may reassign .z.pp */
    uint8_t* body = NULL;
    if (have_cl && cl > 0) {
        body = (uint8_t*)ray_sys_alloc((size_t)cl);
        if (!body) { ray_release(fn); q_http_send_simple(fd, 500, "Internal Server Error"); return; }
        if (http_recv_n(fd, body, (size_t)cl) != 0) {
            ray_sys_free(body); ray_release(fn);
            q_http_send_simple(fd, 400, "Bad Request"); return;
        }
    }
    zh_dispatch_call(fd, NULL, 0, body ? (const char*)body : "",
                     have_cl ? (size_t)cl : 0, hdrs, nh, fn, ".z.pp", false);
    if (body) ray_sys_free(body);
    ray_release(fn);
}

/* `.z.pm` (HTTP OPTIONS/PUT/DELETE/PATCH — ref/dotz.md) — 3-item
 * (methodSym; target; hdrDict) arg.  No `.z.pm` set -> 501 (openq policy;
 * dotz.md pins no default).  requestText = the request-target with leading '/'
 * stripped (`.z.ph` normalization); PUT/PATCH request bodies are not read
 * (deferred — the connection is close-per-response, so unread bytes are dropped
 * on close). */
static void zpm_dispatch(ray_sock_t fd, const char* method, size_t mlen,
                         const char* target, size_t tlen,
                         const struct phr_header* hdrs, size_t nh)
{
    ray_t* fn = q_dotz_zpm();                  /* borrowed, NULL = unset */
    if (!fn) { q_http_send_simple(fd, 501, "Not Implemented"); return; }
    ray_retain(fn);                            /* handler may reassign .z.pm */
    if (tlen && target[0] == '/') { target++; tlen--; }
    zh_dispatch_call(fd, method, mlen, target, tlen, hdrs, nh, fn, ".z.pm", false);
    ray_release(fn);
}

/* Default 401 with a Basic challenge — the `.z.ac` reject arm (dotz.md L137)
 * and the basic-auth-fallback not-permitted case.  Distinct from
 * q_http_send_simple (which the legacy -u/-U listener path keeps unchanged, so
 * an undefined `.z.ac` stays byte-identical). */
static void zac_send_401(ray_sock_t fd) {
    static const char body[] = "401 Unauthorized\n";
    char hdr[192];
    int hl = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"openq\"\r\n"
        "Content-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        sizeof body - 1);
    if (hl > 0 && q_http_send_all(fd, hdr, (size_t)hl, Q_HTTP_SEND_SECS) == 0)
        q_http_send_all(fd, body, sizeof body - 1, Q_HTTP_SEND_SECS);
}

/* `.z.ac` auth gate (ref/dotz.md).  fn is BORROWED (caller-retained).  Runs
 * `.z.ac` with (target; hdrDict) — target = request-target, leading '/' stripped
 * (header inspection is the documented job; the body is NOT read to feed auth).
 * Return protocol (dotz.md L129-166): 0 -> default 401 (reject); 1 -> proceed
 * (accept; the username -> `.z.u` is DEFERRED — openq `.z.u` is a computed
 * producer); 2 -> send the custom response VERBATIM; 4 -> basic-auth fallback
 * (caller re-applies the -u/-U policy).  A malformed/error/unknown-status return
 * fails CLOSED (500, no handler).  Never logs the arg or the return payload. */
enum { ZAC_PROCEED = 0, ZAC_DONE = 1, ZAC_FALLBACK = 2 };
static int zac_gate(ray_sock_t fd, ray_t* fn, const char* target, size_t tlen,
                    const struct phr_header* hdrs, size_t nh)
{
    if (tlen && target[0] == '/') { target++; tlen--; }
    ray_t* arg = zh_build_arg(NULL, 0, target, tlen, hdrs, nh);
    if (!arg) { q_http_send_simple(fd, 500, "Internal Server Error"); return ZAC_DONE; }

    ray_t* r = call_fn1(fn, arg);
    ray_release(arg);
    { const char* con = q_console_str();     /* drain handler show/0N! */
      if (con && *con) { fputs(con, stdout); fflush(stdout); }
      q_console_reset(); }

    int result = ZAC_DONE;
    int64_t st; const char* pay; size_t paylen;
    if (r && !RAY_IS_ERR(r) && q_http_zac_parse(r, &st, &pay, &paylen)) {
        switch (st) {
            case 1: result = ZAC_PROCEED;  break;   /* accept -> reach handler */
            case 4: result = ZAC_FALLBACK; break;   /* basic-auth fallback */
            case 0: zac_send_401(fd);      break;   /* reject */
            case 2: q_http_send_all(fd, pay, paylen, Q_HTTP_SEND_SECS); break; /* custom */
            default:
                fprintf(stderr, "http: .z.ac returned an unknown status — 500\n");
                q_http_send_simple(fd, 500, "Internal Server Error");
        }
    } else {
        fprintf(stderr, "http: .z.ac returned %s — 500\n",
                (r && RAY_IS_ERR(r)) ? "an error" : "a malformed value");
        q_http_send_simple(fd, 500, "Internal Server Error");
    }
    if (r) { if (RAY_IS_ERR(r)) ray_error_free(r);
             else if (r != RAY_NULL_OBJ) ray_release(r); }
    return result;
}

int q_http_respond(ray_sock_t fd, const uint8_t* req, size_t len,
                   bool auth_required)
{
    /* Undefined `.z.ac` on an authed (-u/-U) listener: today's behaviour,
     * BYTE-IDENTICAL — 401 BEFORE parse (so a malformed authed request stays
     * 401, not 400).  A defined `.z.ac` owns the auth decision (its gate runs
     * after parse, below), so the pre-parse 401 is skipped when it is set. */
    ray_t* ac = q_dotz_zac();                 /* borrowed; NULL = unset */
    if (!ac && auth_required) {
        q_http_send_simple(fd, 401, "Unauthorized");
        return 0;
    }

    const char* method; const char* target;
    size_t mlen, tlen, nh = Q_HTTP_MAX_HEADERS;
    int minor;
    struct phr_header hdrs[Q_HTTP_MAX_HEADERS];
    int pret = phr_parse_request((const char*)req, len, &method, &mlen,
                                 &target, &tlen, &minor, hdrs, &nh, 0);
    if (pret <= 0) {          /* -1 malformed; -2 still-incomplete after \r\n\r\n */
        q_http_send_simple(fd, 400, "Bad Request");
        return 0;
    }

    /* `.z.ac` GATE — the single auth choke point.  Runs ONCE, after parse and
     * before EVERY method branch (POST/.z.pm/GET/WS-upgrade/static), so no
     * request path can reach a handler ahead of it (openq policy; dotz.md is
     * silent on ordering + WS).  Accept -> fall through (bypassing the -u/-U
     * 401); reject/custom/error -> a response was sent; fallback -> re-apply the
     * listener's basic-auth policy (deferred to today's authed-401). */
    if (ac) {
        ray_retain(ac);                        /* handler may reassign .z.ac */
        int g = zac_gate(fd, ac, target, tlen, hdrs, nh);
        ray_release(ac);
        if (g == ZAC_DONE) return 0;
        if (g == ZAC_FALLBACK && auth_required) {
            q_http_send_simple(fd, 401, "Unauthorized");
            return 0;
        }
        /* ZAC_PROCEED, or ZAC_FALLBACK on a non-authed listener -> continue. */
    }

    if (mlen == 4 && memcmp(method, "POST", 4) == 0) {
        zpp_dispatch(fd, hdrs, nh);   /* `.z.pp` override, else 501 */
        return 0;
    }
    if (!(mlen == 3 && memcmp(method, "GET", 3) == 0)) {
        /* `.z.pm` methods (ref/dotz.md L739-744): OPTIONS/PUT/DELETE/PATCH.
         * HEAD is NOT among them and is neither GET nor POST -> 501, but a HEAD
         * response must carry NO body (RFC 7231 §4.3.2), so it gets a
         * headers-only 501.  Any other unknown method keeps the textual 501. */
        bool pm = (mlen == 7 && memcmp(method, "OPTIONS", 7) == 0) ||
                  (mlen == 3 && memcmp(method, "PUT",     3) == 0) ||
                  (mlen == 6 && memcmp(method, "DELETE",  6) == 0) ||
                  (mlen == 5 && memcmp(method, "PATCH",   5) == 0);
        if (pm) {
            zpm_dispatch(fd, method, mlen, target, tlen, hdrs, nh);
        } else if (mlen == 4 && memcmp(method, "HEAD", 4) == 0) {
            static const char h[] = "HTTP/1.1 501 Not Implemented\r\n"
                "Content-Type: text/plain\r\nContent-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            q_http_send_all(fd, h, sizeof h - 1, Q_HTTP_SEND_SECS);
        } else {
            q_http_send_simple(fd, 501, "Not Implemented");
        }
        return 0;
    }
    /* WebSocket upgrade (kb/websockets.md: same-port WS server): a GET whose
     * Upgrade header names the websocket protocol hands off to q_ws.c.  Any
     * OTHER Upgrade target keeps #217's 501. */
    for (size_t i = 0; i < nh; i++)
        if (hdrs[i].name && str_ieq(hdrs[i].name, hdrs[i].name_len, "upgrade")) {
            if (str_ieq(hdrs[i].value, hdrs[i].value_len, "websocket"))
                return q_ws_handshake(fd, hdrs, nh);
            q_http_send_simple(fd, 501, "Not Implemented");
            return 0;
        }

    /* `.z.ph` override owns the whole GET surface (kdb shape); else static. */
    if (zph_dispatch(fd, target, tlen, hdrs, nh) == 0) return 0;

    /* raw target cut at `?`/`#` BEFORE decoding (encoded %3f/%23 stay in path) */
    size_t pl = 0;
    while (pl < tlen && target[pl] != '?' && target[pl] != '#') pl++;
    const char* t = target;
    size_t      tl = pl;
    while (tl && t[0] == '/') { t++; tl--; }
    char dec[Q_HTTP_MAX_PATH];
    int  dl = q_http_decode_path(t, tl, dec, sizeof dec);
    if (dl < 0 || !q_http_path_ok(dec, (size_t)dl)) {
        q_http_send_simple(fd, 404, "Not Found");
        return 0;
    }
    char rel[Q_HTTP_MAX_PATH + 16];
    int  rl = (dl == 0 || dec[dl - 1] == '/')
        ? snprintf(rel, sizeof rel, "%.*sindex.html", dl, dec)
        : snprintf(rel, sizeof rel, "%.*s", dl, dec);
    if (rl < 0 || (size_t)rl >= sizeof rel) {
        q_http_send_simple(fd, 404, "Not Found");
        return 0;
    }
    serve_file(fd, rel, (size_t)rl);
    return 0;
}
