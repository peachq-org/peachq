/* q_http_client — see q_http_client.h.  Blocking `.Q.hg`/`.Q.hp` client.
 * Behaviour pinned from qdocs ref/dotq.md (clean room); the timeout, size-cap,
 * https-error and redirect policies are doc-unpinned peachq choices (see PR). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L    /* clock_gettime / CLOCK_MONOTONIC */
#endif
#include <rayforce.h>
#include "qlang/base/q_err.h"
#include "qlang/q_prim.h"
#include "qlang/net/q_http_client.h"
#include "qlang/net/q_gz.h"           /* transparent gzip inflate (Content-Encoding) */
#include "qlang/net/q_tls.h"          /* https — the TLS overlay on the socket */
#include "qlang/io/q_io.h"            /* q_io_clamp — ONE range/EOF law across transports */
#include "lang/eval.h"            /* ray_eval_get_restricted — outbound gate */
#include "table/sym.h"           /* ray_sym_str — hsym text */
#include "picohttpparser.h"
#include <limits.h>
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

/* The two schemes this client speaks, and how far in the authority starts.
 * -1 = neither; else 0 http / 1 https with *pfx set. */
static int scheme_of(const char* s, size_t n, size_t* pfx) {
    if (n >= 7 && memcmp(s, "http://", 7) == 0)  { *pfx = 7; return 0; }
    if (n >= 8 && memcmp(s, "https://", 8) == 0) { *pfx = 8; return 1; }
    return -1;
}

int q_http_client_scheme_is(const char* s, size_t n) {
    size_t pfx;
    return s && scheme_of(s, n, &pfx) >= 0;
}

int q_http_client_url_parse(const char* url, size_t n, q_http_url_t* out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof *out);
    if (n && url[0] == ':') { url++; n--; }        /* strip kdb handle ':' */
    if (!scan_ok(url, n)) return -1;               /* injection guard */

    size_t pfx;
    int sch = scheme_of(url, n, &pfx);
    if (sch < 0) return -1;
    out->scheme = sch;
    out->port = sch ? 443 : 80;
    url += pfx; n -= pfx;

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
/* A header value's trimmed body is exactly one token (case-insensitive) — the
 * `Content-Encoding` shapes that matter: `gzip` is the only coding peachq
 * inflates, `identity` the only one that leaves the resource's own bytes. */
static int val_is(const char* v, size_t n, const char* tok) {
    size_t s = 0; while (s < n && (v[s] == ' ' || v[s] == '\t')) s++;
    size_t e = n; while (e > s && (v[e-1] == ' ' || v[e-1] == '\t')) e--;
    size_t tn = strlen(tok);
    if (e - s != tn) return 0;
    for (size_t k = 0; k < tn; k++) {
        unsigned char c = (unsigned char)v[s + k];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c != (unsigned char)tok[k]) return 0;
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
                          const char** body, size_t* body_len, int* gzip,
                          int no_body, int64_t* clen)
{
    if (gzip) *gzip = 0;
    if (clen) *clen = -1;
    int minor, st; const char* msg; size_t msg_len, nh = Q_HTTP_MAX_HDRS;
    struct phr_header h[Q_HTTP_MAX_HDRS];
    int hl = phr_parse_response(buf, len, &minor, &st, &msg, &msg_len, h, &nh, 0);
    if (hl <= 0) return -1;
    if (status) *status = st;
    /* Content-Encoding: gzip -> caller inflates (headers read intact, before the
     * chunked in-place de-frame below; TE is the outer coding, dechunked first). */
    int gz = 0, coded = 0;
    for (size_t i = 0; i < nh; i++)
        if (h[i].name && hdr_ieq(&h[i], "content-encoding") &&
            !val_is(h[i].value, h[i].value_len, "identity")) {
            coded = 1;                                     /* br/deflate/gzip alike */
            gz = val_is(h[i].value, h[i].value_len, "gzip");
            break;
        }
    if (gzip) *gzip = gz;

    int chunked, have_cl, bodyless; int64_t cl;
    if (response_framing(st, h, nh, &chunked, &have_cl, &cl, &bodyless) != 0) return -1;
    /* A coded length measures the coding, not the resource, so it can never seat a range. */
    if (have_cl && clen && !coded) *clen = cl;
    if (no_body) bodyless = 1;        /* HEAD: the length describes a body never sent */

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
    /* A TLS record cannot be split by a raw send(): the overlay owns framing, so
     * the whole write goes through it — under THIS call's deadline, which the
     * overlay reads back from ray_sock_set_timeout.  SO_SNDTIMEO alone does not
     * reach the overlay's own poll waits. */
    if (ray_sock_io_active(fd)) {
        int64_t left = deadline_ms - now_ms();
        if (left <= 0) return -1;
        ray_sock_set_timeout(fd, left > INT_MAX ? INT_MAX : (int)left);
        int64_t n = ray_sock_send(fd, buf, len);
        ray_sock_set_timeout(fd, 0);
        return n == (int64_t)len ? 0 : -1;
    }

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
                                  int64_t deadline_ms, const char** err, int no_body)
{
    char*  buf = NULL; size_t cap = 0, len = 0;
    int    hdr_len = 0;          /* >0 once headers parsed */
    int    chunked = 0, have_cl = 0, bodyless = 0; int64_t cl = 0;
    const char* e = "conn";

    for (;;) {
        int64_t budget = deadline_ms - now_ms();
        if (budget <= 0) { e = "conn"; goto fail; }
        int wait = budget > 2000000000LL ? 2000000000 : (int)budget;
        /* Decrypted bytes already buffered in the overlay are invisible to
         * poll(), so a readability wait would stall on the last record. */
        int rr = ray_sock_io_pending(fd) ? 1 : ray_sock_wait_readable(fd, wait);
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
            if (no_body) bodyless = 1;   /* HEAD: Content-Length describes a body never sent */
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
    int64_t tl;
    if (x && q_str_text_bytes(x, &p, &tl)) { l = (size_t)tl; }
    else if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);   /* borrowed */
        if (!s) return -1;
        p = ray_str_ptr(s); l = ray_str_len(s);
    } else return -1;
    if (!p || l >= outsz) return -1;
    memcpy(out, p, l); out[l] = '\0'; *n = l;
    return 0;
}

/* One outbound request.  mime non-NULL => POST; `head` => HEAD.  roff >= 0 adds a
 * `Range:` (rlen < 0 = open-ended) — always sent, never negotiated by probing
 * `Accept-Ranges` first (the DuckDB policy measured in the 2026-08-24 brief). */
typedef struct {
    int         head;
    const char* mime; size_t mime_len;
    const char* body; size_t body_len;
    int64_t     roff, rlen;
    int         as_bytes;    /* result is RAY_BYTE_ONLY (read1's shape), not charv */
    int*        status;      /* out, optional */
    int64_t*    clen;        /* out, optional: the length a HEAD advertised, else -1 */
} http_req_t;

/* The body in the shape the caller asked for: read1 wants bytes, `.Q.hg` chars. */
static ray_t* http_body(const http_req_t* r, const char* p, size_t n) {
    return r->as_bytes ? ray_vec_from_raw(RAY_BYTE_ONLY, (const uint8_t*)p, (int64_t)n)
                       : ray_charv(p, (int64_t)n);
}

/* Shared GET/HEAD/POST driver.  Returns the body (charv, or bytes when the caller
 * asked) or a bare-class ray_error. */
static ray_t* http_do(ray_t* urlv, const http_req_t* r)
{
    char urlbuf[1280]; size_t un;
    if (url_of(urlv, urlbuf, sizeof urlbuf, &un) != 0)
        return q_err(QE_TYPE);
    q_http_url_t u;
    if (q_http_client_url_parse(urlbuf, un, &u) != 0) return q_err(QE_DOMAIN);
    const char* mime = r->mime; size_t mime_len = r->mime_len;
    const char* body = r->body; size_t body_len = r->body_len;

    /* Authorization header (optional) */
    char authhdr[512]; authhdr[0] = '\0';
    if (u.userinfo[0]) {
        char enc[400];
        if (b64(u.userinfo, strlen(u.userinfo), enc, sizeof enc) < 0)
            return q_err(QE_LIMIT);
        snprintf(authhdr, sizeof authhdr, "Authorization: Basic %s\r\n", enc);
    }
    /* Host: include non-default port */
    char hosthdr[300];
    int default_port = (u.scheme == 0 && u.port == 80) || (u.scheme == 1 && u.port == 443);
    if (default_port) snprintf(hosthdr, sizeof hosthdr, "%s", u.host);
    else              snprintf(hosthdr, sizeof hosthdr, "%s:%u", u.host, u.port);

    /* A range names bytes of the RESOURCE, so a content coding would move them —
     * a ranged request never offers gzip, nor does the HEAD that measures it. */
    char rangehdr[64]; rangehdr[0] = '\0';
    if (r->roff >= 0) {
        if (r->rlen < 0) snprintf(rangehdr, sizeof rangehdr, "Range: bytes=%lld-\r\n",
                                  (long long)r->roff);
        else snprintf(rangehdr, sizeof rangehdr, "Range: bytes=%lld-%lld\r\n",
                      (long long)r->roff, (long long)(r->roff + r->rlen - 1));
    }
    const char* accept_enc = (rangehdr[0] || r->head) ? "" : "Accept-Encoding: gzip\r\n";

    /* request head */
    char req[2048];
    int rl;
    if (mime) {
        if (!scan_ok(mime, mime_len)) return q_err(QE_DOMAIN);   /* injection guard */
        rl = snprintf(req, sizeof req,
            "POST %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
            "%s"
            "Content-Type: %.*s\r\nContent-Length: %zu\r\n%s\r\n",
            u.path, hosthdr, accept_enc, (int)mime_len, mime, body_len, authhdr);
    } else {
        rl = snprintf(req, sizeof req,
            "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
            "%s%s%s\r\n",
            r->head ? "HEAD" : "GET", u.path, hosthdr, accept_enc, rangehdr, authhdr);
    }
    if (rl < 0 || (size_t)rl >= sizeof req) return q_err(QE_LIMIT);

    const char* err = "conn";
    ray_sock_t fd = q_http_client_connect(u.host, u.port, Q_HTTP_CONNECT_MS, &err);
    if (fd == RAY_INVALID_SOCK) return ray_error(err, NULL);

    int64_t deadline = now_ms() + Q_HTTP_TOTAL_MS;
    ray_t* result = NULL;
    if (u.scheme == 1 && q_tls_client_start(fd, u.host, &err) != 0) goto done;
    if (q_http_client_send_all(fd, req, (size_t)rl, deadline) != 0) { err = "conn"; goto done; }
    if (mime && body_len &&
        q_http_client_send_all(fd, body, body_len, deadline) != 0) { err = "conn"; goto done; }

    size_t rlen = 0;
    char* resp = q_http_client_read_response(fd, &rlen, deadline, &err, r->head);
    if (!resp) goto done;
    int st; const char* rbody; size_t rbl; int gz = 0;
    int ex = q_http_client_extract(resp, rlen, &st, &rbody, &rbl, &gz, r->head, r->clen);
    if (ex == 0 && r->status) *r->status = st;
    if (ex == 0 && gz && rbl) {
        /* transparent inflate — q_gz_inflate bounds output at 32 MiB (bomb guard).
         * An empty body (HEAD, or a coded 0-length answer) codes nothing to inflate. */
        size_t ilen = 0; const char* ierr = NULL;
        uint8_t* infl = q_gz_inflate((const uint8_t*)rbody, rbl, &ilen, &ierr);
        if (infl) { result = http_body(r, (const char*)infl, ilen); free(infl); }
        else err = ierr ? ierr : "domain";
    }
    else if (ex == 0) result = http_body(r, rbody, rbl);
    else if (ex == -2) err = "wsfull";
    else err = "conn";
    free(resp);
done:
    ray_sock_close(fd);
    if (result) return result;
    return ray_error(err, NULL);
}

ray_t* q_dotq_hg_fn(ray_t* x) {
    http_req_t r = { .roff = -1 };
    return http_do(x, &r);
}

ray_t* q_dotq_hp_fn(ray_t** args, int64_t nargs) {
    if (nargs != 3) return q_err(QE_RANK);
    ray_t* mimev = args[1];
    ray_t* bodyv = args[2];
    const char* mp; int64_t ml; const char* bp; int64_t bl;
    if (!mimev || !q_str_text_bytes(mimev, &mp, &ml)) return q_err(QE_TYPE);
    if (!bodyv || !q_str_text_bytes(bodyv, &bp, &bl)) return q_err(QE_TYPE);
    http_req_t r = { .mime = mp, .mime_len = (size_t)ml,
                     .body = bp, .body_len = (size_t)bl, .roff = -1 };
    return http_do(args[0], &r);
}

/* A resource read answers a remote object that is not there the way it answers a
 * missing file — 'io.  Resource policy, not client policy: `.Q.hg` above keeps
 * handing back whatever the server said. */
static ray_t* http_status_body(ray_t* b, int status) {
    if (RAY_IS_ERR(b) || (status >= 200 && status < 300)) return b;
    ray_release(b);
    return q_err(QE_IO);
}

/* ---- the http transport under the resource-read seam (q_io_resource_read) ----
 * user-docs/handles.md points 1 + 3: read0/read1 reach HTTP through the SAME seam
 * the file transport sits behind, and a ranged call really does send `Range:`.
 * One q-level read is one GET; a ranged one first asks HEAD for the length so the
 * clamp is q_io_clamp's law rather than a server's 416 discipline.  That HEAD is a
 * BEST-EFFORT probe and can only ever improve the clamp: refused, failed outright,
 * or answering with no length at all (peachq's listener answers 501; a CDN-fronted
 * API answers chunked with no Content-Length), the read falls back to clamping the
 * GET's own answer — which is the whole body when the server ignores `Range:`. */
ray_t* q_http_client_read_slice(ray_t* url, int64_t off, int64_t want) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    int st = 0;
    if (off <= 0 && want < 0) {                            /* whole object: one GET */
        http_req_t r = { .roff = -1, .as_bytes = 1, .status = &st };
        ray_t* b = http_do(url, &r);
        return http_status_body(b, st);
    }
    if (off < 0) off = 0;
    int hst = 0; int64_t total = -1;
    http_req_t hr = { .head = 1, .roff = -1, .status = &hst, .clen = &total };
    ray_t* h = http_do(url, &hr);
    if (RAY_IS_ERR(h)) ray_error_free(h); else ray_release(h);
    if (hst >= 200 && hst < 300 && total >= 0) want = q_io_clamp(total, &off, want);
    if (want == 0) return ray_vec_from_raw(RAY_BYTE_ONLY, NULL, 0);

    http_req_t r = { .roff = off, .rlen = want, .as_bytes = 1, .status = &st };
    ray_t* b = http_do(url, &r);
    if (RAY_IS_ERR(b)) return b;
    if (st == 416) { ray_release(b); return ray_vec_from_raw(RAY_BYTE_ONLY, NULL, 0); }
    b = http_status_body(b, st);
    if (RAY_IS_ERR(b) || st == 206) return b;              /* 206: the body IS the slice */
    int64_t at = off, n = ray_len(b);                      /* 200: Range ignored — slice here */
    int64_t take = q_io_clamp(n, &at, want);
    ray_t* out = ray_vec_from_raw(RAY_BYTE_ONLY,
                                  take ? (const uint8_t*)ray_data(b) + at : NULL, take);
    ray_release(b);
    return out;
}

/* ---- low-level raw client (kb/http.md §low level HTTP request mechanism) ----
 * The ONLY divergence from http_do: the caller supplies the whole request (sent
 * verbatim, nothing injected) and we return the ENTIRE response — status line +
 * headers + framed body — not just the body.  q_http_client_extract normalizes
 * framing (Content-Length trim / chunked de-frame-in-place / close-to-EOF /
 * bodyless) and sets body = resp + header_len, so the full raw response is
 * exactly [resp, (body-resp)+body_len).  Chunked reassembly leaves the response
 * headers unchanged (doc: "constructed from the chunks").  A HEAD/CONNECT
 * response is framed by the reader as if it had a body (the reader is
 * method-agnostic, inherited from #223) — an accepted limitation for this
 * escape hatch. */
ray_t* q_http_client_raw(ray_t* hsym, ray_t* request) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    const char* reqp; int64_t reqn;                 /* charv or legacy STR text */
    if (!request || !q_str_text_bytes(request, &reqp, &reqn)) return q_err(QE_TYPE);
    if (!hsym || hsym->type != -RAY_SYM) return q_err(QE_TYPE);

    /* hsym text (BORROWED interned string) -> ":http://host[:port]" */
    ray_t* nm = ray_sym_str(hsym->i64);
    if (!nm) return q_err(QE_TYPE);
    const char* s = ray_str_ptr(nm);
    size_t sn = ray_str_len(nm);
    if (sn < 1 || s[0] != ':') return q_err(QE_DOMAIN);

    q_http_url_t u;                       /* q_http_client_url_parse strips the ':' */
    if (q_http_client_url_parse(s, sn, &u) != 0) return q_err(QE_DOMAIN);

    const char* err = "conn";
    ray_sock_t fd = q_http_client_connect(u.host, u.port, Q_HTTP_CONNECT_MS, &err);
    if (fd == RAY_INVALID_SOCK) return ray_error(err, NULL);

    int64_t deadline = now_ms() + Q_HTTP_TOTAL_MS;
    ray_t* result = NULL;
    if (u.scheme == 1 && q_tls_client_start(fd, u.host, &err) != 0) goto done;
    if (q_http_client_send_all(fd, reqp, (size_t)reqn,
                               deadline) != 0) { err = "conn"; goto done; }
    size_t rlen = 0;
    char* resp = q_http_client_read_response(fd, &rlen, deadline, &err, 0);
    if (!resp) goto done;
    int st; const char* body; size_t body_len;
    /* raw client returns the response verbatim — no transparent gzip inflate (NULL) */
    int ex = q_http_client_extract(resp, rlen, &st, &body, &body_len, NULL, 0, NULL);
    if (ex == 0) {
        size_t total = (size_t)(body - resp) + body_len;   /* headers + framed body */
        result = ray_charv(resp, (int64_t)total);
        if (!result) err = "oom";                          /* NULL: OOM, not 'conn */
    } else if (ex == -2) err = "wsfull";
    else err = "conn";
    free(resp);
done:
    ray_sock_close(fd);
    if (result) return result;
    return ray_error(err, NULL);
}
