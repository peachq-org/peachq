/* q_http_client — see q_http_client.h.  Blocking `.Q.hg`/`.Q.hp` client.
 * Behaviour pinned from qdocs ref/dotq.md (clean room); the timeout, size-cap,
 * https-error and redirect policies are doc-unpinned openq choices (see PR). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L    /* clock_gettime / CLOCK_MONOTONIC */
#endif
#include <rayforce.h>
#include "qlang/q_http_client.h"
#include "picohttpparser.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <windows.h>
#else
  #include <poll.h>
  #include <errno.h>
  #include <time.h>
  #include <sys/socket.h>
#endif

#define Q_HTTP_CONNECT_MS 30000        /* connect budget */
#define Q_HTTP_TOTAL_MS   30000        /* whole send+read budget */
#define Q_HTTP_MAX_HDRS   64
#define Q_HTTP_HDR_CAP    (64 * 1024)  /* response header block cap (pre-body) */

/* ---- monotonic milliseconds (absolute-deadline arithmetic) ---- */
static int64_t now_ms(void) {
#ifdef RAY_OS_WINDOWS
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* ---- URL parse ---- */
static int scan_ok(const char* p, size_t n) {   /* no control / CR / LF bytes */
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)p[i] < 0x20 || p[i] == 0x7f) return 0;
    return 1;
}

int q_http_url_parse(const char* url, size_t n, q_http_url_t* out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof *out);
    if (n && url[0] == ':') { url++; n--; }        /* strip kdb handle ':' */
    if (!scan_ok(url, n)) return -1;               /* injection guard */

    if (n >= 7 && memcmp(url, "http://", 7) == 0)       { out->scheme = 0; url += 7; n -= 7; out->port = 80; }
    else if (n >= 8 && memcmp(url, "https://", 8) == 0) { out->scheme = 1; url += 8; n -= 8; out->port = 443; }
    else return -1;

    /* authority ends at the first '/', '?' or '#' */
    size_t a = 0;
    while (a < n && url[a] != '/' && url[a] != '?' && url[a] != '#') a++;
    const char* auth = url; size_t alen = a;
    const char* rest = url + a; size_t rlen = n - a;

    /* optional user:pass@ */
    size_t at = alen;
    for (size_t i = 0; i < alen; i++) if (auth[i] == '@') { at = i; break; }
    const char* hostport = auth; size_t hplen = alen;
    if (at < alen) {
        if (at >= sizeof out->userinfo) return -1;
        memcpy(out->userinfo, auth, at); out->userinfo[at] = '\0';
        hostport = auth + at + 1; hplen = alen - (at + 1);
    }
    if (hplen && hostport[0] == '[') return -1;    /* bracketed IPv6 deferred */

    /* host[:port] */
    size_t colon = hplen;
    for (size_t i = 0; i < hplen; i++) if (hostport[i] == ':') { colon = i; break; }
    size_t hlen = colon;
    if (hlen == 0 || hlen >= sizeof out->host) return -1;
    memcpy(out->host, hostport, hlen); out->host[hlen] = '\0';
    if (colon < hplen) {
        long port = 0; size_t pi = colon + 1;
        if (pi >= hplen) return -1;                /* trailing ':' */
        for (; pi < hplen; pi++) {
            if (hostport[pi] < '0' || hostport[pi] > '9') return -1;
            port = port * 10 + (hostport[pi] - '0');
            if (port > 65535) return -1;            /* overflow */
        }
        out->port = (uint16_t)port;
    }

    /* path = everything up to '#' (fragment dropped; query kept).  A query-only
     * target (rest starts with '?') gets a leading '/' for a valid origin-form. */
    size_t plen = 0;
    while (plen < rlen && rest[plen] != '#') plen++;
    if (plen == 0) { out->path[0] = '/'; out->path[1] = '\0'; }
    else {
        size_t off = (rest[0] != '/') ? 1 : 0;          /* prepend '/' for '?...' */
        if (plen + off >= sizeof out->path) return -1;
        if (off) out->path[0] = '/';
        memcpy(out->path + off, rest, plen); out->path[plen + off] = '\0';
    }
    return 0;
}

/* ---- header lookup helpers (case-insensitive) ---- */
static int hdr_ieq(const struct phr_header* h, const char* lower) {
    size_t tn = strlen(lower);
    if (h->name_len != tn) return 0;
    for (size_t i = 0; i < tn; i++) {
        unsigned char c = (unsigned char)h->name[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c != (unsigned char)lower[i]) return 0;
    }
    return 1;
}
/* Terminal transfer coding is exactly `chunked` (case-insensitive).  A TE header
 * whose last comma-separated token is not `chunked` (e.g. gzip) is unsupported. */
static int te_terminal_chunked(const char* v, size_t n) {
    size_t e = n; while (e > 0 && (v[e-1] == ' ' || v[e-1] == '\t' || v[e-1] == ',')) e--;
    size_t s = e; while (s > 0 && v[s-1] != ',') s--;               /* last token */
    while (s < e && (v[s] == ' ' || v[s] == '\t')) s++;
    if (e - s != 7) return 0;
    static const char* t = "chunked";
    for (size_t k = 0; k < 7; k++) {
        unsigned char c = (unsigned char)v[s + k];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c != (unsigned char)t[k]) return 0;
    }
    return 1;
}

/* Strict Content-Length: leading/trailing OWS only, all-digit body, within cap.
 * 0 ok / -1 malformed. */
static int parse_content_length(const char* v, size_t n, int64_t cap, int64_t* out) {
    size_t s = 0; while (s < n && (v[s] == ' ' || v[s] == '\t')) s++;
    size_t e = n; while (e > s && (v[e-1] == ' ' || v[e-1] == '\t')) e--;
    if (s == e) return -1;
    int64_t val = 0;
    for (size_t k = s; k < e; k++) {
        if (v[k] < '0' || v[k] > '9') return -1;                   /* embedded space / sign / junk */
        val = val * 10 + (v[k] - '0');
        if (val > cap) return -1;
    }
    *out = val;
    return 0;
}

/* Body framing of a parsed response.  -1 malformed; else 0.
 * *chunked / *have_cl / *cl / *bodyless describe the framing. */
static int response_framing(int status, const struct phr_header* h, size_t nh,
                            int* chunked, int* have_cl, int64_t* cl, int* bodyless)
{
    *chunked = 0; *have_cl = 0; *cl = 0;
    *bodyless = (status >= 100 && status < 200) || status == 204 || status == 304;
    int te = 0; int cl_seen = 0; int64_t clv = 0;
    for (size_t i = 0; i < nh; i++) {
        if (!h[i].name) continue;
        if (hdr_ieq(&h[i], "transfer-encoding")) {
            if (!te_terminal_chunked(h[i].value, h[i].value_len)) return -1;  /* unsupported coding */
            te = 1;
        } else if (hdr_ieq(&h[i], "content-length")) {
            int64_t v = 0;
            if (parse_content_length(h[i].value, h[i].value_len, Q_HTTP_CLIENT_MAX, &v) != 0)
                return -1;
            if (cl_seen && v != clv) return -1;         /* conflicting duplicate */
            cl_seen = 1; clv = v;
        }
    }
    if (te && cl_seen) return -1;                        /* TE+CL smuggling */
    if (te) { *chunked = 1; return 0; }
    if (cl_seen) { *have_cl = 1; *cl = clv; return 0; }
    return 0;                                            /* close-framed (or bodyless) */
}

/* Non-mutating chunked-completion probe over a COPY of the body region.
 * 1 complete, 0 incomplete, -1 malformed. */
static int chunked_complete(const char* body, size_t blen) {
    if (blen == 0) return 0;
    char* tmp = (char*)malloc(blen);
    if (!tmp) return -1;
    memcpy(tmp, body, blen);
    struct phr_chunked_decoder dec; memset(&dec, 0, sizeof dec);
    size_t sz = blen;
    ssize_t r = phr_decode_chunked(&dec, tmp, &sz);
    free(tmp);
    if (r == -1) return -1;
    if (r == -2) return 0;
    return 1;                                            /* >= 0: terminal chunk seen */
}

int q_http_client_extract(char* buf, size_t len, int* status,
                          const char** body, size_t* body_len)
{
    int minor, st; const char* msg; size_t msg_len, nh = Q_HTTP_MAX_HDRS;
    struct phr_header h[Q_HTTP_MAX_HDRS];
    int hl = phr_parse_response(buf, len, &minor, &st, &msg, &msg_len, h, &nh, 0);
    if (hl <= 0) return -1;
    if (status) *status = st;

    int chunked, have_cl, bodyless; int64_t cl;
    if (response_framing(st, h, nh, &chunked, &have_cl, &cl, &bodyless) != 0) return -1;

    char*  b   = buf + hl;
    size_t blen = len - (size_t)hl;
    if (bodyless) { *body = b; *body_len = 0; return 0; }
    if (chunked) {
        size_t sz = blen;
        struct phr_chunked_decoder dec; memset(&dec, 0, sizeof dec);
        ssize_t r = phr_decode_chunked(&dec, b, &sz);   /* de-frames IN PLACE */
        if (r == -1 || r == -2) return -1;
        if (sz > (size_t)Q_HTTP_CLIENT_MAX) return -2;
        *body = b; *body_len = sz; return 0;
    }
    if (have_cl) {
        if (cl > (int64_t)Q_HTTP_CLIENT_MAX) return -2;
        if ((size_t)cl > blen) return -1;               /* truncated */
        *body = b; *body_len = (size_t)cl; return 0;
    }
    if (blen > (size_t)Q_HTTP_CLIENT_MAX) return -2;
    *body = b; *body_len = blen; return 0;               /* close-framed */
}

/* ---- blocking pipeline seams ---- */
ray_sock_t q_http_client_connect(const char* host, uint16_t port,
                                 int timeout_ms, const char** err)
{
    ray_sock_t fd = ray_sock_connect(host, port, timeout_ms);
    if (fd == RAY_INVALID_SOCK) { if (err) *err = "conn"; return RAY_INVALID_SOCK; }
    return fd;
}

int q_http_client_send_all(ray_sock_t fd, const void* buf, size_t len,
                           int64_t deadline_ms)
{
    const uint8_t* p = (const uint8_t*)buf; size_t rem = len;
    while (rem > 0) {
        int64_t budget = deadline_ms - now_ms();
        if (budget <= 0) return -1;
        int wait = budget > 2000000000LL ? 2000000000 : (int)budget;
#ifdef RAY_OS_WINDOWS
        WSAPOLLFD pfd = { fd, POLLOUT, 0 };
        int pr = WSAPoll(&pfd, 1, wait);
        if (pr <= 0) return -1;
        int n = send(fd, (const char*)p, (int)rem, 0);
        if (n <= 0) { if (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINTR) continue; return -1; }
#else
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, wait);
        if (pr <= 0) { if (pr < 0 && errno == EINTR) continue; return -1; }
        ssize_t n = send(fd, p, rem,
  #ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
  #else
                         0
  #endif
        );
        if (n < 0) { if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; return -1; }
#endif
        p += n; rem -= (size_t)n;
    }
    return 0;
}

/* Grow *buf to at least need bytes.  0 ok / -1 OOM. */
static int grow(char** buf, size_t* cap, size_t need) {
    if (need <= *cap) return 0;
    size_t nc = *cap ? *cap : 4096;
    while (nc < need) { if (nc > (size_t)Q_HTTP_CLIENT_MAX + Q_HTTP_HDR_CAP) return -1; nc *= 2; }
    char* nb = (char*)realloc(*buf, nc);
    if (!nb) return -1;
    *buf = nb; *cap = nc; return 0;
}

char* q_http_client_read_response(ray_sock_t fd, size_t* out_len,
                                  int64_t deadline_ms, const char** err)
{
    char*  buf = NULL; size_t cap = 0, len = 0;
    int    hdr_len = 0;          /* >0 once headers parsed */
    int    chunked = 0, have_cl = 0, bodyless = 0; int64_t cl = 0;
    const char* e = "conn";

    for (;;) {
        int64_t budget = deadline_ms - now_ms();
        if (budget <= 0) { e = "conn"; goto fail; }
        int wait = budget > 2000000000LL ? 2000000000 : (int)budget;
        int rr = ray_sock_wait_readable(fd, wait);
        if (rr == 0) { e = "conn"; goto fail; }          /* timeout */
        if (rr < 0)  { e = "conn"; goto fail; }
        if (grow(&buf, &cap, len + 8192) != 0) { e = "wsfull"; goto fail; }
        int64_t n = ray_sock_recv(fd, buf + len, cap - len);
        if (n < 0) { e = "conn"; goto fail; }
        if (n == 0) {                                    /* peer closed */
            if (hdr_len == 0) { e = "conn"; goto fail; } /* no complete headers */
            if (chunked || have_cl) { e = "conn"; goto fail; } /* framed but short */
            break;                                       /* close-framed: EOF = done */
        }
        len += (size_t)n;

        while (hdr_len == 0) {                           /* re-parse buffered bytes */
            int minor, st; const char* msg; size_t ml, nh = Q_HTTP_MAX_HDRS;
            struct phr_header h[Q_HTTP_MAX_HDRS];
            int hl = phr_parse_response(buf, len, &minor, &st, &msg, &ml, h, &nh, 0);
            if (hl == -1) { e = "conn"; goto fail; }
            if (hl == -2) {                              /* need more headers */
                if (len > (size_t)Q_HTTP_HDR_CAP) { e = "wsfull"; goto fail; }
                break;                                   /* -> wait for more bytes */
            }
            if (st >= 100 && st < 200) {                 /* 1xx interim: drop it, the
                                                          * final response follows */
                memmove(buf, buf + hl, len - (size_t)hl);
                len -= (size_t)hl;
                continue;                                /* re-parse the next response */
            }
            if (response_framing(st, h, nh, &chunked, &have_cl, &cl, &bodyless) != 0) {
                e = "conn"; goto fail;
            }
            hdr_len = hl;
        }
        /* completion checks */
        if (hdr_len > 0) {
            char*  body = buf + hdr_len; size_t blen = len - (size_t)hdr_len;
            if (bodyless) break;
            if (have_cl) {
                if (cl > (int64_t)Q_HTTP_CLIENT_MAX) { e = "wsfull"; goto fail; }
                if (blen >= (size_t)cl) break;
            } else if (chunked) {
                if (blen > (size_t)Q_HTTP_CLIENT_MAX) { e = "wsfull"; goto fail; }
                int c = chunked_complete(body, blen);
                if (c == -1) { e = "conn"; goto fail; }
                if (c == 1) break;
            } else if (blen > (size_t)Q_HTTP_CLIENT_MAX) { e = "wsfull"; goto fail; }
        }
    }
    *out_len = len;
    return buf;
fail:
    free(buf);
    if (err) *err = e;
    return NULL;
}

/* ---- request builders + verb bodies ---- */
static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
/* base64 of userinfo into out (NUL-terminated); returns length or -1 on overflow. */
static int b64(const char* s, size_t n, char* out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint8_t)s[i] << 16;
        if (i + 1 < n) v |= (uint8_t)s[i + 1] << 8;
        if (i + 2 < n) v |= (uint8_t)s[i + 2];
        char q[4] = { b64tab[(v >> 18) & 63], b64tab[(v >> 12) & 63],
                      i + 1 < n ? b64tab[(v >> 6) & 63] : '=',
                      i + 2 < n ? b64tab[v & 63] : '=' };
        if (o + 4 >= outsz) return -1;
        memcpy(out + o, q, 4); o += 4;
    }
    out[o] = '\0';
    return (int)o;
}

/* URL bytes from a q value (string or symbol atom) into a bounded buffer. */
static int url_of(ray_t* x, char* out, size_t outsz, size_t* n) {
    const char* p; size_t l;
    if (x && x->type == -RAY_STR) { p = ray_str_ptr(x); l = ray_str_len(x); }
    else if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);   /* borrowed */
        if (!s) return -1;
        p = ray_str_ptr(s); l = ray_str_len(s);
    } else return -1;
    if (!p || l >= outsz) return -1;
    memcpy(out, p, l); out[l] = '\0'; *n = l;
    return 0;
}

/* Shared GET/POST driver.  mime/body non-NULL => POST.  Returns the body string
 * or a bare-class ray_error. */
static ray_t* http_do(ray_t* urlv, const char* mime, size_t mime_len,
                      const char* body, size_t body_len)
{
    char urlbuf[1280]; size_t un;
    if (url_of(urlv, urlbuf, sizeof urlbuf, &un) != 0)
        return ray_error("type", NULL);
    q_http_url_t u;
    if (q_http_url_parse(urlbuf, un, &u) != 0) return ray_error("domain", NULL);
    if (u.scheme == 1) return ray_error("nyi", NULL);        /* https: TLS tier */

    /* Authorization header (optional) */
    char authhdr[512]; authhdr[0] = '\0';
    if (u.userinfo[0]) {
        char enc[400];
        if (b64(u.userinfo, strlen(u.userinfo), enc, sizeof enc) < 0)
            return ray_error("limit", NULL);
        snprintf(authhdr, sizeof authhdr, "Authorization: Basic %s\r\n", enc);
    }
    /* Host: include non-default port */
    char hosthdr[300];
    int default_port = (u.scheme == 0 && u.port == 80) || (u.scheme == 1 && u.port == 443);
    if (default_port) snprintf(hosthdr, sizeof hosthdr, "%s", u.host);
    else              snprintf(hosthdr, sizeof hosthdr, "%s:%u", u.host, u.port);

    /* request head */
    char req[2048];
    int rl;
    if (mime) {
        if (!scan_ok(mime, mime_len)) return ray_error("domain", NULL);   /* injection guard */
        rl = snprintf(req, sizeof req,
            "POST %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
            "Content-Type: %.*s\r\nContent-Length: %zu\r\n%s\r\n",
            u.path, hosthdr, (int)mime_len, mime, body_len, authhdr);
    } else {
        rl = snprintf(req, sizeof req,
            "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n%s\r\n",
            u.path, hosthdr, authhdr);
    }
    if (rl < 0 || (size_t)rl >= sizeof req) return ray_error("limit", NULL);

    const char* err = "conn";
    ray_sock_t fd = q_http_client_connect(u.host, u.port, Q_HTTP_CONNECT_MS, &err);
    if (fd == RAY_INVALID_SOCK) return ray_error(err, NULL);

    int64_t deadline = now_ms() + Q_HTTP_TOTAL_MS;
    ray_t* result = NULL;
    if (q_http_client_send_all(fd, req, (size_t)rl, deadline) != 0) { err = "conn"; goto done; }
    if (mime && body_len &&
        q_http_client_send_all(fd, body, body_len, deadline) != 0) { err = "conn"; goto done; }

    size_t rlen = 0;
    char* resp = q_http_client_read_response(fd, &rlen, deadline, &err);
    if (!resp) goto done;
    int st; const char* rbody; size_t rbl;
    int ex = q_http_client_extract(resp, rlen, &st, &rbody, &rbl);
    if (ex == 0) result = ray_str(rbody, rbl);
    else if (ex == -2) err = "wsfull";
    else err = "conn";
    free(resp);
done:
    ray_sock_close(fd);
    if (result) return result;
    return ray_error(err, NULL);
}

ray_t* q_dotq_hg_fn(ray_t* x) {
    return http_do(x, NULL, 0, NULL, 0);
}

ray_t* q_dotq_hp_fn(ray_t** args, int64_t nargs) {
    if (nargs != 3) return ray_error("rank", NULL);
    ray_t* mimev = args[1];
    ray_t* bodyv = args[2];
    if (!mimev || mimev->type != -RAY_STR) return ray_error("type", NULL);
    if (!bodyv || bodyv->type != -RAY_STR) return ray_error("type", NULL);
    return http_do(args[0], ray_str_ptr(mimev), ray_str_len(mimev),
                   ray_str_ptr(bodyv), ray_str_len(bodyv));
}
