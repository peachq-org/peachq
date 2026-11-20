/* q_http_client — see q_http_client.h.  Blocking `.Q.hg`/`.Q.hp` client.
 * Behaviour pinned from qdocs ref/dotq.md (clean room); the timeout, size-cap,
 * https-error and redirect policies are doc-unpinned peachq choices (see PR):
 * 3xx followed cross-domain and across schemes up to Q_HTTP_MAX_HOPS, credentials
 * dropped on a host change, the raw client never following. */
#include "qlang/q_count.h"
#include <rayforce.h>
#include "qlang/base/q_err.h"
#include "qlang/q_prim.h"
#include "qlang/net/q_http_client.h"
#include "qlang/net/q_gz.h"           /* transparent gzip/deflate inflate (Content-Encoding) */
#include "qlang/net/q_tls.h"          /* https — the TLS overlay on the socket */
#include "qlang/io/q_io.h"            /* q_io_clamp — ONE range/EOF law across transports */
#include "table/sym.h"           /* ray_sym_str — hsym text */
#include "picohttpparser.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
#else
  #include <errno.h>
  #include <sys/socket.h>
#endif

#define Q_HTTP_CONNECT_MS 30000        /* connect budget */
#define Q_HTTP_IDLE_MS    30000        /* silence allowed per send or read; no whole-transfer deadline, as curl */
#define Q_HTTP_SLICE_MS   100          /* a wait is cut this fine so Ctrl+C is seen during a stall */
#define Q_HTTP_MAX_HDRS   64
#define Q_HTTP_HDR_CAP    (64 * 1024)  /* response header block cap (pre-body) */
#define Q_HTTP_RECV       8192
#define Q_HTTP_MAX_HOPS   10           /* redirect follows (urllib's number): 11 transactions, then the 3xx stands */
#define Q_HTTP_USER_AGENT "peachq/" RAYFORCE_VERSION   /* some APIs (Coinbase) refuse a request without one */

/* ---- URL parse ---- */
static int scan_ok(const char* p, size_t n) {   /* no control / CR / LF bytes */
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)p[i] < 0x20 || p[i] == 0x7f) return 0;
    return 1;
}

/* n bytes of s equal `lower` ignoring ASCII case: schemes, header names and tokens are all case-insensitive. */
static int lower_eq(const char* s, const char* lower, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c != (unsigned char)lower[i]) return 0;
    }
    return 1;
}

/* The two schemes this client speaks, and how far in the authority starts.
 * -1 = neither; else 0 http / 1 https with *pfx set. */
static int scheme_of(const char* s, size_t n, size_t* pfx) {
    if (n >= 7 && lower_eq(s, "http://", 7))  { *pfx = 7; return 0; }
    if (n >= 8 && lower_eq(s, "https://", 8)) { *pfx = 8; return 1; }
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

    size_t pfx;
    int sch = scheme_of(url, n, &pfx);
    if (sch < 0) return -1;
    out->scheme = sch;
    out->port = sch ? 443 : 80;
    url += pfx; n -= pfx;

    /* authority ends at the first '/', '?' or '#' */
    size_t a = 0;
    while (a < n && url[a] != '/' && url[a] != '?' && url[a] != '#') a++;
    if (!scan_ok(url, a)) return -1;               /* injection guard: the target is encoded below instead */
    const char* auth = url; size_t alen = a;
    const char* rest = url + a; size_t rlen = n - a;

    /* optional user:pass@ — the LAST '@' ends it, so a pasted password may hold one */
    size_t at = alen;
    for (size_t i = alen; i-- > 0;) if (auth[i] == '@') { at = i; break; }
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
     * target (rest starts with '?') gets a leading '/' for a valid origin-form.
     * A space, control byte or byte >= 0x80 is percent-encoded, as browsers do; '%'
     * is not, so an existing %XX is never encoded twice. */
    static const char hex[] = "0123456789ABCDEF";
    size_t plen = 0, o = 0;
    while (plen < rlen && rest[plen] != '#') plen++;
    if (plen == 0 || rest[0] != '/') out->path[o++] = '/';
    for (size_t i = 0; i < plen; i++) {
        unsigned char c = (unsigned char)rest[i];
        int enc = c <= ' ' || c >= 0x7f;
        if (o + (enc ? 3 : 1) >= sizeof out->path) return -1;
        if (enc) { out->path[o++] = '%'; out->path[o++] = hex[c >> 4]; out->path[o++] = hex[c & 15]; }
        else out->path[o++] = (char)c;
    }
    out->path[o] = '\0';
    return 0;
}

/* ---- header lookup helpers (case-insensitive) ---- */
static int hdr_ieq(const struct phr_header* h, const char* lower) {
    return h->name_len == strlen(lower) && lower_eq(h->name, lower, h->name_len);
}
/* A header value's trimmed body is exactly one token (case-insensitive) — the
 * `Content-Encoding` shapes that matter: `gzip` is the only coding peachq
 * inflates, `identity` the only one that leaves the resource's own bytes. */
static int val_is(const char* v, size_t n, const char* tok) {
    size_t s = 0; while (s < n && (v[s] == ' ' || v[s] == '\t')) s++;
    size_t e = n; while (e > s && (v[e-1] == ' ' || v[e-1] == '\t')) e--;
    return e - s == strlen(tok) && lower_eq(v + s, tok, e - s);
}

/* Terminal transfer coding is exactly `chunked` (case-insensitive).  A TE header
 * whose last comma-separated token is not `chunked` (e.g. gzip) is unsupported. */
static int te_terminal_chunked(const char* v, size_t n) {
    size_t e = n; while (e > 0 && (v[e-1] == ' ' || v[e-1] == '\t' || v[e-1] == ',')) e--;
    size_t s = e; while (s > 0 && v[s-1] != ',') s--;               /* last token */
    while (s < e && (v[s] == ' ' || v[s] == '\t')) s++;
    return e - s == 7 && lower_eq(v + s, "chunked", 7);
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
    int te = 0, cl_seen = 0, cl_bad = 0; int64_t clv = 0;
    for (size_t i = 0; i < nh; i++) {
        if (!h[i].name) continue;
        if (hdr_ieq(&h[i], "transfer-encoding")) {
            if (!te_terminal_chunked(h[i].value, h[i].value_len)) return -1;  /* unsupported coding */
            te = 1;
        } else if (hdr_ieq(&h[i], "content-length")) {
            int64_t v = 0;
            if (parse_content_length(h[i].value, h[i].value_len, Q_HTTP_CLIENT_MAX, &v) != 0 ||
                (cl_seen && v != clv)) cl_bad = 1;      /* malformed, or a conflicting duplicate */
            cl_seen = 1; clv = v;
        }
    }
    if (te) { *chunked = 1; return 0; }                  /* Transfer-Encoding overrides it (RFC 9112 §6.3) */
    if (cl_bad) return -1;
    if (cl_seen) { *have_cl = 1; *cl = clv; return 0; }
    return 0;                                            /* close-framed (or bodyless) */
}

/* Walk a chunked body from *pos (the next chunk-size line, kept between reads), so each chunk is stepped over once:
 * re-decoding the whole body per recv was quadratic (182 s for a 294 MB answer).  With out set it also de-frames,
 * moving each chunk's data to out + *olen (in place when out is b).  As tolerant as RFC 9112 lets a recipient be: a
 * line may end in a bare LF, an extension after BWS or ';' is skipped, trailers are ignored.  picohttpparser's decoder is stricter on
 * the LF, and refuses part-way a body whose framing is mostly overhead (100k one-byte chunks).
 * 1 complete, 0 incomplete, -1 malformed, -2 a chunk past the cap. */
static int chunked_walk(char* b, size_t blen, size_t* pos, char* out, size_t* olen) {
    for (;;) {
        size_t p = *pos, n = 0, i = p;
        const char* nl = p < blen ? memchr(b + p, '\n', blen - p) : NULL;
        if (!nl) return 0;
        for (; isxdigit((unsigned char)b[i]); i++) {
            if (n >> 60) return -1;
            n = n * 16 + (size_t)(isdigit((unsigned char)b[i]) ? b[i] - '0' : tolower((unsigned char)b[i]) - 'a' + 10);
        }
        if (i == p || !memchr(" \t;\r\n", b[i], 5)) return -1;    /* `0x10` is junk, not the last chunk */
        if (n == 0) return 1;
        if (n > (size_t)Q_HTTP_CLIENT_MAX) return -2;
        size_t d = (size_t)(nl - b) + 1, end = d + n;
        if (end >= blen) return 0;
        size_t lf = end + (b[end] == '\r');
        if (lf >= blen) return 0;
        if (b[lf] != '\n') return -1;
        if (out) { memmove(out + *olen, b + d, n); *olen += n; }
        *pos = lf + 1;
    }
}

int q_http_client_extract(char* buf, size_t len, int* status,
                          const char** body, size_t* body_len, int* coding,
                          int no_body, int64_t* clen, q_http_span_t* location)
{
    if (coding) *coding = 0;
    if (clen) *clen = -1;
    if (location) { location->p = NULL; location->n = 0; }
    int minor, st; const char* msg; size_t msg_len, nh = Q_HTTP_MAX_HDRS;
    struct phr_header h[Q_HTTP_MAX_HDRS];
    int hl = phr_parse_response(buf, len, &minor, &st, &msg, &msg_len, h, &nh, 0);
    if (hl <= 0) return -1;
    if (status) *status = st;
    /* Content-Encoding gzip or deflate -> caller inflates (headers read intact, before the
     * chunked in-place de-frame below; TE is the outer coding, dechunked first). */
    int cod = 0, coded = 0;
    for (size_t i = 0; i < nh; i++)
        if (h[i].name && hdr_ieq(&h[i], "content-encoding") &&
            !val_is(h[i].value, h[i].value_len, "identity")) {
            coded = 1;                                     /* br/deflate/gzip alike */
            cod = val_is(h[i].value, h[i].value_len, "gzip") ? Q_GZ_GZIP
                : val_is(h[i].value, h[i].value_len, "deflate") ? Q_GZ_DEFLATE : 0;
            break;
        }
    if (coding) *coding = cod;
    for (size_t i = 0; location && i < nh; i++)
        if (h[i].name && hdr_ieq(&h[i], "location")) {
            location->p = h[i].value; location->n = h[i].value_len;
            break;
        }

    int chunked, have_cl, bodyless; int64_t cl;
    if (response_framing(st, h, nh, &chunked, &have_cl, &cl, &bodyless) != 0) return -1;
    /* A coded length measures the coding, not the resource, so it can never seat a range. */
    if (have_cl && clen && !coded) *clen = cl;
    if (no_body) bodyless = 1;        /* HEAD: the length describes a body never sent */

    char*  b   = buf + hl;
    size_t blen = len - (size_t)hl;
    if (bodyless) { *body = b; *body_len = 0; return 0; }
    if (chunked) {
        size_t pos = 0, sz = 0;
        int r = chunked_walk(b, blen, &pos, b, &sz);    /* de-frames IN PLACE */
        if (r != 1) return r == -2 ? -2 : -1;
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

/* Wait until fd can be written (`out`) or read, through idle_ms of silence cut into slices so Ctrl+C is seen while a
 * server stalls.  1 ready, 0 silent too long, -1 error, -2 interrupted. */
static int wait_io(ray_sock_t fd, int out, int idle_ms) {
    for (int left = idle_ms; left > 0; left -= Q_HTTP_SLICE_MS) {
        if (ray_interrupted()) return -2;
        int w = left < Q_HTTP_SLICE_MS ? left : Q_HTTP_SLICE_MS;
        int r = out ? ray_sock_wait_writable(fd, w) : ray_sock_wait_readable(fd, w);
        if (r != 0) return r;
    }
    return 0;
}

int q_http_client_send_all(ray_sock_t fd, const void* buf, size_t len, int idle_ms)
{
    /* A TLS record cannot be split by a raw send(): the overlay owns framing, so
     * the whole write goes through it — under THIS call's budget, which the
     * overlay reads back from ray_sock_set_timeout.  SO_SNDTIMEO alone does not
     * reach the overlay's own poll waits. */
    if (ray_sock_io_active(fd)) {
        ray_sock_set_timeout(fd, idle_ms);
        int64_t n = ray_sock_send(fd, buf, len);
        ray_sock_set_timeout(fd, 0);
        return n == (int64_t)len ? 0 : -1;
    }

    const uint8_t* p = (const uint8_t*)buf; size_t rem = len;
    while (rem > 0) {
        if (wait_io(fd, 1, idle_ms) <= 0) return -1;
#ifdef RAY_OS_WINDOWS
        int n = send(fd, (const char*)p, (int)rem, 0);
        if (n <= 0) { if (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINTR) continue; return -1; }
#else
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

/* Grow *buf to at least need bytes, never past what a capped response can fill.  0 ok / -1 too big or OOM. */
static int grow(char** buf, size_t* cap, size_t need) {
    const size_t lim = (size_t)Q_HTTP_CLIENT_MAX + Q_HTTP_HDR_CAP + Q_HTTP_RECV;
    if (need <= *cap) return 0;
    if (need > lim) return -1;
    size_t nc = *cap ? *cap : 4096;
    while (nc < need) nc *= 2;
    if (nc > lim) nc = lim;
    char* nb = (char*)realloc(*buf, nc);
    if (!nb) return -1;
    *buf = nb; *cap = nc; return 0;
}

char* q_http_client_read_response(ray_sock_t fd, size_t* out_len,
                                  int idle_ms, const char** err, int no_body)
{
    char*  buf = NULL; size_t cap = 0, len = 0;
    int    hdr_len = 0;          /* >0 once headers parsed */
    int    chunked = 0, have_cl = 0, bodyless = 0; int64_t cl = 0;
    size_t cpos = 0;             /* the chunked walk's place in the body */
    const char* e = "conn";

    for (;;) {
        /* Decrypted bytes already buffered in the overlay are invisible to
         * poll(), so a readability wait would stall on the last record. */
        int rr = ray_sock_io_pending(fd) ? 1 : wait_io(fd, 0, idle_ms);
        if (rr == -2) { e = "stop"; goto fail; }
        if (rr <= 0)  { e = "conn"; goto fail; }
        if (grow(&buf, &cap, len + Q_HTTP_RECV) != 0) { e = "wsfull"; goto fail; }
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
                int c = chunked_walk(body, blen, &cpos, NULL, NULL);
                if (c < 0) { e = c == -2 ? "wsfull" : "conn"; goto fail; }
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

/* One outbound request.  mime adds a body (POST unless `method` names another); `head` => HEAD.  roff >= 0 adds a
 * `Range:` (rlen < 0 = open-ended) — always sent, never negotiated by probing
 * `Accept-Ranges` first (the DuckDB policy measured in the 2026-08-24 brief). */
typedef struct {
    int         head;
    const char* method;      /* `.Q.hmb`'s verb; NULL = POST/HEAD/GET from mime and head */
    const char* mime; size_t mime_len;
    const char* body; size_t body_len;
    int64_t     roff, rlen;
    int         as_bytes;    /* result is RAY_BYTE_ONLY (read1's shape), not charv */
    int*        status;      /* out, optional */
    int64_t*    clen;        /* out, optional: the length a HEAD advertised, else -1 */
    ray_t**     head_text;   /* out, optional: the final hop's status line + headers */
} http_req_t;

/* The body in the shape the caller asked for: read1 wants bytes, `.Q.hg` chars. */
static ray_t* http_body(const http_req_t* r, const char* p, size_t n) {
    return r->as_bytes ? ray_vec_from_raw(RAY_BYTE_ONLY, (const uint8_t*)p, (int64_t)n)
                       : ray_charv(p, (int64_t)n);
}

static int host_ieq(const char* a, const char* b) {   /* DNS names are case-insensitive */
    for (; *a && *b; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* Resolve a `Location` against the URL that answered it (RFC 3986 §5.2 reference
 * forms: absolute, scheme-relative `//h/p`, path-absolute `/p`, relative `p`).  A
 * foreign scheme (`ftp:`, `file:` — a ':' before any '/', '?' or '#') is refused
 * rather than read as a path; dot segments are left to the server.  0 ok / -1 unresolvable
 * (the caller falls out). */
static int redirect_resolve(const q_http_url_t* cur, const char* loc, size_t n,
                            q_http_url_t* out)
{
    while (n && (loc[0] == ' ' || loc[0] == '\t')) { loc++; n--; }
    while (n && (loc[n-1] == ' ' || loc[n-1] == '\t')) n--;
    if (n == 0 || loc[0] == ':') return -1;
    size_t pfx;
    if (scheme_of(loc, n, &pfx) >= 0) return q_http_client_url_parse(loc, n, out);
    for (size_t i = 0; i < n && loc[i] != '/' && loc[i] != '?' && loc[i] != '#'; i++)
        if (loc[i] == ':') return -1;
    const char* sch = cur->scheme ? "https:" : "http:";
    char buf[2560]; int w;
    if (n >= 2 && loc[0] == '/' && loc[1] == '/')
        w = snprintf(buf, sizeof buf, "%s%.*s", sch, (int)n, loc);
    else if (loc[0] == '/')
        w = snprintf(buf, sizeof buf, "%s//%s:%u%.*s", sch, cur->host, cur->port, (int)n, loc);
    else {
        size_t d = 0, q = 0;                            /* how much of the current path the reference keeps */
        while (cur->path[q] && cur->path[q] != '?') q++;
        if (loc[0] == '?') d = q;                       /* query-only: the whole path, a new query */
        else if (loc[0] == '#') d = strlen(cur->path);  /* fragment-only: the same URL */
        else for (size_t i = 0; i < q; i++) if (cur->path[i] == '/') d = i + 1;   /* its directory */
        w = snprintf(buf, sizeof buf, "%s//%s:%u%.*s%.*s", sch, cur->host, cur->port,
                     (int)d, cur->path, (int)n, loc);
    }
    if (w < 0 || (size_t)w >= sizeof buf) return -1;
    return q_http_client_url_parse(buf, (size_t)w, out);
}

/* THE one network exchange: request head + body out, the whole raw response back
 * (q_http_client_read_response's contract).  http_do and the raw client both ride it. */
static char* http_exchange(const q_http_url_t* u, const char* req, size_t req_len,
                           const char* body, size_t body_len,
                           int no_body, size_t* len, const char** err)
{
#if defined(__EMSCRIPTEN__)
    return q_http_client_host_exchange(u, req, req_len, body, body_len, no_body, len, err);
#else
    ray_sock_t fd = q_http_client_connect(u->host, u->port, Q_HTTP_CONNECT_MS, err);
    if (fd == RAY_INVALID_SOCK) return NULL;
    char* resp = NULL;
    if (u->scheme == 1 && q_tls_client_start(fd, u->host, false, err) != 0) goto done;
    if (q_http_client_send_all(fd, req, req_len, Q_HTTP_IDLE_MS) != 0) { *err = "conn"; goto done; }
    if (body_len && q_http_client_send_all(fd, body, body_len, Q_HTTP_IDLE_MS) != 0) { *err = "conn"; goto done; }
    resp = q_http_client_read_response(fd, len, Q_HTTP_IDLE_MS, err, no_body);
done:
    ray_sock_close(fd);
    return resp;
#endif
}

void q_http_client_host(const q_http_url_t* u, char* out, size_t cap) {
    int default_port = (u->scheme == 0 && u->port == 80) || (u->scheme == 1 && u->port == 443);
    if (default_port) snprintf(out, cap, "%s", u->host);
    else              snprintf(out, cap, "%s:%u", u->host, u->port);
}

ray_t* q_http_client_fetch(const q_http_url_t* u, const char* method, const char* headers, q_http_resp_t* r) {
    memset(r, 0, sizeof *r);
    if (!scan_ok(method, strlen(method))) return q_err(QE_DOMAIN);
    char req[4096];
    int rl = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nConnection: close\r\n%s\r\n", method, u->path, headers);
    if (rl < 0 || (size_t)rl >= sizeof req) return q_err(QE_LIMIT);
    const char* err = "conn";
    int head = strcmp(method, "HEAD") == 0;
    r->buf = http_exchange(u, req, (size_t)rl, NULL, 0, head, &r->len, &err);
    if (!r->buf) return q_err_from_text(err, strlen(err));
    int ex = q_http_client_extract(r->buf, r->len, &r->status, &r->body, &r->body_len, NULL, head, NULL, NULL);
    if (ex == 0) return NULL;
    q_http_client_resp_free(r);
    return q_err(ex == -2 ? QE_WSFULL : QE_CONN);
}

q_http_span_t q_http_client_resp_header(const q_http_resp_t* r, const char* name) {
    q_http_span_t v = { NULL, 0 };
    int minor, st; const char* msg; size_t ml, nh = Q_HTTP_MAX_HDRS;
    struct phr_header h[Q_HTTP_MAX_HDRS];
    if (!r->buf || phr_parse_response(r->buf, r->len, &minor, &st, &msg, &ml, h, &nh, 0) <= 0) return v;
    size_t n = strlen(name);
    for (size_t i = 0; i < nh; i++) {
        if (!h[i].name || h[i].name_len != n) continue;
        size_t k = 0;
        while (k < n && (h[i].name[k] | 0x20) == (name[k] | 0x20)) k++;
        if (k == n) { v.p = h[i].value; v.n = h[i].value_len; return v; }
    }
    return v;
}

void q_http_client_resp_free(q_http_resp_t* r) {
    free(r->buf);
    memset(r, 0, sizeof *r);
}

/* Shared request driver — THE one place a 3xx is followed (`.Q.hg`, `.Q.hp`, `.Q.hmb`
 * and the read0/read1 slice all ride it; q_http_client_raw does not).  Returns the
 * body (charv, or bytes when the caller asked) or a bare-class ray_error. */
static ray_t* http_do(ray_t* urlv, const http_req_t* r)
{
    char urlbuf[1280]; size_t un;
    if (url_of(urlv, urlbuf, sizeof urlbuf, &un) != 0)
        return q_err(QE_TYPE);
    q_http_url_t u;
    if (q_http_client_url_parse(urlbuf, un, &u) != 0) return q_err(QE_DOMAIN);
    const char* mime = r->mime; size_t mime_len = r->mime_len;
    const char* body = r->body; size_t body_len = r->body_len;
    if (mime && !scan_ok(mime, mime_len)) return q_err(QE_DOMAIN);   /* injection guard */
    const char* method = r->method ? r->method : mime ? "POST" : r->head ? "HEAD" : "GET";

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

    for (int hops = 0;; hops++) {      /* each hop is a fresh exchange with its own budgets; the count bounds the chain */
        /* Authorization header (optional) — rebuilt per hop from the hop's own userinfo */
        char authhdr[512]; authhdr[0] = '\0';
        if (u.userinfo[0]) {
            char enc[400];
            if (b64(u.userinfo, strlen(u.userinfo), enc, sizeof enc) < 0)
                return q_err(QE_LIMIT);
            snprintf(authhdr, sizeof authhdr, "Authorization: Basic %s\r\n", enc);
        }
        char hosthdr[300];
        q_http_client_host(&u, hosthdr, sizeof hosthdr);

        char ctype[600]; ctype[0] = '\0';
        if (mime) {
            int cl = snprintf(ctype, sizeof ctype, "Content-Type: %.*s\r\nContent-Length: %zu\r\n",
                              (int)mime_len, mime, body_len);
            if (cl < 0 || (size_t)cl >= sizeof ctype) return q_err(QE_LIMIT);
        }
        char req[2048];
        int rl = snprintf(req, sizeof req,
            "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: " Q_HTTP_USER_AGENT "\r\n"
            "%s%s%s%s\r\n",
            method, u.path, hosthdr, accept_enc, ctype, rangehdr, authhdr);
        if (rl < 0 || (size_t)rl >= sizeof req) return q_err(QE_LIMIT);

        const char* err = "conn";
        ray_t* result = NULL;
        size_t rlen = 0;
        char* resp = http_exchange(&u, req, (size_t)rl, body, mime ? body_len : 0, r->head, &rlen, &err);
        if (!resp) return q_err_from_text(err, strlen(err));
        int st; const char* rbody; size_t rbl; int coding = 0; q_http_span_t loc;
        int ex = q_http_client_extract(resp, rlen, &st, &rbody, &rbl, &coding, r->head, r->clen, &loc);
        if (ex == 0 && r->status) *r->status = st;
        if (ex == 0 && loc.p && hops < Q_HTTP_MAX_HOPS &&
            (st == 301 || st == 302 || st == 303 || st == 307 || st == 308)) {
            q_http_url_t nu;
            if (redirect_resolve(&u, loc.p, loc.n, &nu) == 0) {
                /* the requests/browser law: a 303 turns any method but HEAD into a GET, a 301/302 only
                 * a POST; the body goes with it.  Credentials follow the host, never the redirect: a
                 * target that names its own userinfo wins, the same host keeps the original, a new host gets none. */
                int is_post = strcmp(method, "POST") == 0;
                if ((st == 303 && !r->head) || ((st == 301 || st == 302) && is_post)) {
                    method = "GET"; mime = NULL; body = NULL; body_len = 0;
                }
                if (!nu.userinfo[0] && host_ieq(nu.host, u.host))
                    memcpy(nu.userinfo, u.userinfo, sizeof nu.userinfo);
                u = nu;
                free(resp);
                continue;
            }
        }
        if (ex == 0 && coding && rbl) {
            /* transparent inflate, to the same cap a plain body has.
             * An empty body (HEAD, or a coded 0-length answer) codes nothing to inflate. */
            size_t ilen = 0; const char* ierr = NULL;
            uint8_t* infl = q_gz_inflate_coding((const uint8_t*)rbody, rbl, coding, (size_t)Q_HTTP_CLIENT_MAX,
                                                &ilen, &ierr);
            if (infl) { result = http_body(r, (const char*)infl, ilen); free(infl); }
            else err = ierr ? ierr : "domain";
        }
        else if (ex == 0) result = http_body(r, rbody, rbl);
        else if (ex == -2) err = "wsfull";
        else err = "conn";
        if (result && r->head_text) {
            size_t hn = (size_t)(rbody - resp);
            while (hn && (resp[hn - 1] == '\r' || resp[hn - 1] == '\n')) hn--;
            *r->head_text = ray_charv(resp, (int64_t)hn);
        }
        free(resp);
        if (result) return result;
        return ray_error(err, NULL);
    }
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

static ray_t* enlist_owned(ray_t** items, int n) {   /* consumes items */
    ray_t* out = q_enlist_wrap(items, n);
    for (int i = 0; i < n; i++) ray_release(items[i]);
    return out;
}

ray_t* q_dotq_hmb_fn(ray_t** args, int64_t nargs) {
    if (nargs != 3) return q_err(QE_RANK);
    ray_t* mv = args[1];
    ray_t* bv = args[2];
    if (!mv || mv->type != -RAY_SYM || !bv) return q_err(QE_TYPE);
    ray_t* ms = ray_sym_str(mv->i64);   /* borrowed */
    if (!ms) return q_err(QE_TYPE);
    char method[16];
    size_t mn = ray_str_len(ms);
    if (mn == 0 || mn >= sizeof method) return q_err(QE_DOMAIN);
    memcpy(method, ray_str_ptr(ms), mn); method[mn] = '\0';
    for (size_t i = 0; i < mn; i++)
        if (method[i] < 'A' || method[i] > 'Z') return q_err(QE_DOMAIN);   /* the request line's first token */
    http_req_t r = { .method = method, .head = strcmp(method, "HEAD") == 0, .roff = -1 };
    int64_t bn = q_count(bv);
    if (bn == 2 && bv->type == RAY_LIST) {
        const char* mp; int64_t ml; const char* bp; int64_t bl;
        if (!q_str_text_bytes(ray_list_get(bv, 0), &mp, &ml) || !q_str_text_bytes(ray_list_get(bv, 1), &bp, &bl))
            return q_err(QE_TYPE);
        r.mime = mp; r.mime_len = (size_t)ml; r.body = bp; r.body_len = (size_t)bl;
    } else if (bn != 0) return q_err(QE_TYPE);
    ray_t* head = NULL;
    r.head_text = &head;
    ray_t* content = http_do(args[0], &r);
    if (RAY_IS_ERR(content)) return content;
    if (!head || RAY_IS_ERR(head)) {
        ray_release(content);
        if (head) ray_error_free(head);
        return ray_error("oom", NULL);
    }
    ray_t* parts[] = { head, content };
    return enlist_owned(parts, 2);
}

ray_t* q_dotq_hap_fn(ray_t* x) {
    char urlbuf[1280]; size_t un;
    if (url_of(x, urlbuf, sizeof urlbuf, &un) != 0) return q_err(QE_TYPE);
    q_http_url_t u;
    if (q_http_client_url_parse(urlbuf, un, &u) != 0) return q_err(QE_DOMAIN);
    char hostport[300];
    q_http_client_host(&u, hostport, sizeof hostport);
    const char* text[] = { u.scheme ? "https://" : "http://", u.userinfo, hostport, u.path };
    ray_t* parts[4];
    for (int i = 0; i < 4; i++) parts[i] = ray_charv(text[i], (int64_t)strlen(text[i]));
    return enlist_owned(parts, 4);
}

/* A resource read answers a remote object that is not there the way it answers a
 * missing file — 'io.  Resource policy, not client policy: `.Q.hg` above keeps
 * handing back whatever the server said. */
static ray_t* http_status_body(ray_t* b, int status) {
    if (RAY_IS_ERR(b) || (status >= 200 && status < 300)) return b;
    ray_release(b);
    return q_err(QE_IO);
}

ray_t* q_http_client_size(ray_t* url) {
    int st = 0;
    int64_t total = -1;
    http_req_t hr = { .head = 1, .roff = -1, .status = &st, .clen = &total };
    ray_t* h = http_do(url, &hr);
    if (RAY_IS_ERR(h)) return h;
    ray_release(h);
    return st >= 200 && st < 300 && total >= 0 ? ray_i64(total) : q_err(QE_IO);
}

/* ---- the http transport under the resource-read seam (q_io_resource_read) ----
 * read0/read1 reach HTTP through the SAME seam
 * the file transport sits behind, and a ranged call really does send `Range:`.
 * One q-level read is one GET; a ranged one first asks HEAD for the length so the
 * clamp is q_io_clamp's law rather than a server's 416 discipline.  That HEAD is a
 * BEST-EFFORT probe and can only ever improve the clamp: refused, failed outright,
 * or answering with no length at all (a CDN-fronted API answers chunked with no
 * Content-Length), the read falls back to clamping the
 * GET's own answer — which is the whole body when the server ignores `Range:`. */
ray_t* q_http_client_read_slice(ray_t* url, int64_t off, int64_t want) {
    int st = 0;
    if (off <= 0 && want < 0) {                            /* whole object: one GET */
        http_req_t r = { .roff = -1, .as_bytes = 1, .status = &st };
        ray_t* b = http_do(url, &r);
        return http_status_body(b, st);
    }
    if (off < 0) off = 0;
    ray_t* total = q_http_client_size(url);
    if (RAY_IS_ERR(total)) ray_error_free(total);
    else { want = q_io_clamp(total->i64, &off, want); ray_release(total); }
    if (want == 0) return ray_vec_from_raw(RAY_BYTE_ONLY, NULL, 0);

    http_req_t r = { .roff = off, .rlen = want, .as_bytes = 1, .status = &st };
    ray_t* b = http_do(url, &r);
    if (RAY_IS_ERR(b)) return b;
    if (st == 416) { ray_release(b); return ray_vec_from_raw(RAY_BYTE_ONLY, NULL, 0); }
    b = http_status_body(b, st);
    if (RAY_IS_ERR(b) || st == 206) return b;              /* 206: the body IS the slice */
    int64_t at = off, n = q_count(b);                      /* 200: Range ignored — slice here */
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
    size_t rlen = 0;
    char* resp = http_exchange(&u, reqp, (size_t)reqn, NULL, 0, 0, &rlen, &err);
    if (!resp) return q_err_from_text(err, strlen(err));
    ray_t* result = NULL;
    int st; const char* body; size_t body_len;
    /* raw client returns the response verbatim — no transparent gzip inflate (NULL) */
    int ex = q_http_client_extract(resp, rlen, &st, &body, &body_len, NULL, 0, NULL, NULL);
    if (ex == 0) {
        size_t total = (size_t)(body - resp) + body_len;   /* headers + framed body */
        result = ray_charv(resp, (int64_t)total);
        if (!result) err = "oom";                          /* NULL: OOM, not 'conn */
    } else if (ex == -2) err = "wsfull";
    else err = "conn";
    free(resp);
    if (result) return result;
    return ray_error(err, NULL);
}
