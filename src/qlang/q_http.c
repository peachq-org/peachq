/* q_http — see q_http.h.  Behaviour pinned from qdocs kb/http.md + ref/dotz.md
 * (clean room); doc-unpinned details are openq-authored and recorded in the PR. */
#ifndef RAY_OS_WINDOWS
  #define _GNU_SOURCE            /* openat / O_NOFOLLOW / O_DIRECTORY */
#endif
#include <rayforce.h>
#include "qlang/q_http.h"
#include "qlang/q_dotz.h"      /* q_dotz_zph — the `.z.ph` handler slot */
#include "qlang/q_ws.h"        /* q_ws_handshake — the Upgrade hand-off */
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
#define Q_HTTP_MAX_FILE    (32ll * 1024 * 1024)   /* served-file cap (slice) */
#define Q_HTTP_SEND_SECS   30                     /* whole-response send deadline */

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

/* Friendly 200 when no docroot exists yet: the server is up, tell the operator
 * how to serve content, rather than 404-ing every path.  `root` is the resolved
 * docroot dir (already validated safe by q_http_docroot). */
static void serve_welcome(ray_sock_t fd, const char* root) {
    static const char generic[] =
        "<!doctype html><meta charset=utf-8><title>openq</title>\n"
        "<h1>openq HTTP server is running</h1>\n"
        "<p>No docroot directory was found. Create one (with an "
        "<code>index.html</code>) beside the process to serve your own pages.</p>\n";
    char body[512];
    int bl = snprintf(body, sizeof body,
        "<!doctype html><meta charset=utf-8><title>openq</title>\n"
        "<h1>openq HTTP server is running</h1>\n"
        "<p>No <code>%s</code> directory was found. Create one (with an "
        "<code>index.html</code>) beside the process to serve your own pages.</p>\n",
        root);
    const char* b = body;
    size_t      n = (size_t)bl;
    if (bl < 0 || (size_t)bl >= sizeof body) {   /* over-long root -> generic body */
        b = generic;
        n = sizeof generic - 1;
    }
    char hdr[192];
    int hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n", n);
    if (hl > 0 && q_http_send_all(fd, hdr, (size_t)hl, Q_HTTP_SEND_SECS) == 0)
        q_http_send_all(fd, b, n, Q_HTTP_SEND_SECS);
}

static void serve_file(ray_sock_t fd, const char* rel, size_t rl) {
    int64_t size = 0;
    int doc = q_http_open_doc(rel, rl, &size);
    if (doc < 0) {
        if (http_docroot_absent()) {
            char rootbuf[Q_HTTP_MAX_PATH];
            serve_welcome(fd, q_http_docroot(rootbuf, sizeof rootbuf));
            return;
        }
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

/* `.z.ph` dispatch (ref/dotz.md; PROVISIONAL pre-C3: string ATOMS): call the
 * handler with (requestText; sym-keyed headerDict), write its returned response
 * string VERBATIM.  Error/non-string -> 500.  0 handled / -1 no handler. */
static int zph_dispatch(ray_sock_t fd, const char* target, size_t tlen,
                        const struct phr_header* hdrs, size_t nh)
{
    ray_t* fn = q_dotz_zph();                  /* borrowed, NULL = unset */
    if (!fn) return -1;
    ray_retain(fn);                            /* handler may reassign .z.ph */

    /* picohttpparser sets name==NULL for an obsolete folded header — skip it. */
    size_t nk = 0;
    for (size_t i = 0; i < nh; i++) if (hdrs[i].name) nk++;

    if (tlen && target[0] == '/') { target++; tlen--; }
    ray_t* text = ray_str(target, tlen);
    ray_t* keys = ray_vec_new(RAY_SYM, nk > 0 ? (int64_t)nk : 1);
    ray_t* vals = ray_list_new(nk > 0 ? (int64_t)nk : 1);
    ray_t* arg  = ray_list_new(2);
    bool bad = !text || !keys || !vals || !arg ||
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
            if (RAY_IS_ERR(nv)) { bad = true; break; }
            vals = nv;
        }
    }
    if (bad) {
        if (text && !RAY_IS_ERR(text)) ray_release(text);
        if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
        if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
        if (arg  && !RAY_IS_ERR(arg))  ray_release(arg);
        ray_release(fn);
        q_http_send_simple(fd, 500, "Internal Server Error");
        return 0;
    }
    ray_t* hdrd = ray_dict_new(keys, vals);    /* consumes both; dup keys kept as-is */
    arg = ray_list_append(arg, text);
    ray_release(text);
    arg = ray_list_append(arg, hdrd);
    ray_release(hdrd);

    ray_t* r = call_fn1(fn, arg);
    ray_release(arg);
    ray_release(fn);
    /* drain handler show/0N! to the server console (q_zts_tick pattern) */
    { const char* con = q_console_str();
      if (con && *con) { fputs(con, stdout); fflush(stdout); }
      q_console_reset(); }

    if (r && !RAY_IS_ERR(r) && r->type == -RAY_STR) {
        q_http_send_all(fd, ray_str_ptr(r), ray_str_len(r), Q_HTTP_SEND_SECS);
        ray_release(r);
        return 0;
    }
    fprintf(stderr, "http: .z.ph returned %s — sending 500\n",
            (r && RAY_IS_ERR(r)) ? "an error" : "a non-string");
    if (r) {
        if (RAY_IS_ERR(r)) ray_error_free(r);
        else if (r != RAY_NULL_OBJ) ray_release(r);
    }
    q_http_send_simple(fd, 500, "Internal Server Error");
    return 0;
}

int q_http_respond(ray_sock_t fd, const uint8_t* req, size_t len,
                   bool auth_required)
{
    /* authed listener (-u/-U) must not be an auth bypass — .z.ac deferred */
    if (auth_required) { q_http_send_simple(fd, 401, "Unauthorized"); return 0; }

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
    if (!(mlen == 3 && memcmp(method, "GET", 3) == 0)) {
        q_http_send_simple(fd, 501, "Not Implemented");   /* .z.pp/.z.pm deferred */
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
