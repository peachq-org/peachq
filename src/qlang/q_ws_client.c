/* q_ws_client — WebSocket CLIENT open (kb/websockets.md lines 111-185).
 * Surface: (`$":ws://[user:pass@]host:port")"<raw HTTP request>" — a raw HTTP
 * request string applied to a `ws://` file-symbol hsym.  Rides the #223 blocking
 * HTTP-client seams (connect/send) + the #220 accept primitives (.Q.sha1/.Q.btoa)
 * and q_ws.c's client-role codec.  wss:// -> 'nyi (TLS tier).  Returns the 2-list
 * (handle;response) on 101, (0Ni;response) on a clean non-101 upgrade failure, or
 * a signalled 'conn/'nyi/'type error.  Doc-unpinned choices: PR Decisions. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L    /* clock_gettime / CLOCK_MONOTONIC */
#endif
#include <rayforce.h>
#include "qlang/q_ws.h"
#include "qlang/q_http_client.h"   /* q_http_url_t + connect/send seams (#223) */
#include "qlang/q_builtins.h"      /* q_dotq_sha1_fn / q_dotq_btoa_fn */
#include "lang/eval.h"             /* ray_eval_get_restricted */
#include "core/ipc.h"              /* ray_ws_client_register / _fd_of_handle / _close */
#include "core/sock.h"
#include "table/sym.h"             /* ray_sym_str */
#include "picohttpparser.h"
#include "qlang/q_registry.h"   /* q_text_bytes — charv/legacy text accessor */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <fcntl.h>     /* open — /dev/urandom Sec-WebSocket-Key nonce */
  #include <unistd.h>    /* read, close */
#endif

#define Q_WS_CONNECT_MS 30000
#define Q_WS_HS_MS      30000
#define Q_WS_HS_MAX     (64 * 1024)     /* handshake response header cap */

static int64_t ws_now_ms(void) {
#ifdef RAY_OS_WINDOWS
    return (int64_t)GetTickCount64();
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Read the HTTP handshake response until the CRLFCRLF header terminator (a 101
 * carries no body).  Byte-at-a-time so it STOPS exactly at the terminator and
 * never swallows the server's first WS frame into the response.  Blocking,
 * bounded by deadline + cap.  malloc'd buffer (caller frees) + *len, or NULL. */
static char* ws_read_response(ray_sock_t fd, int64_t deadline, size_t* len,
                              const char** err) {
    size_t cap = 512, n = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { *err = "wsfull"; return NULL; }
    for (;;) {
        int64_t budget = deadline - ws_now_ms();
        if (budget <= 0) { *err = "conn"; free(buf); return NULL; }
        int wait = budget > 2000000000LL ? 2000000000 : (int)budget;
        int rr = ray_sock_wait_readable(fd, wait);
        if (rr <= 0) { *err = "conn"; free(buf); return NULL; }
        uint8_t b;
        int64_t got = ray_sock_recv(fd, &b, 1);
        if (got <= 0) { *err = "conn"; free(buf); return NULL; }
        if (n + 1 > cap) {
            if (cap >= (size_t)Q_WS_HS_MAX) { *err = "wsfull"; free(buf); return NULL; }
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { *err = "wsfull"; free(buf); return NULL; }
            buf = nb;
        }
        buf[n++] = (char)b;
        if (n >= 4 && memcmp(buf + n - 4, "\r\n\r\n", 4) == 0) break;
    }
    *len = n;
    return buf;
}

/* After a NON-101 status, append the error body to *buf so the returned
 * response string is the COMPLETE HTTP response (kb/websockets.md shows a failed
 * upgrade carrying its response body).  A non-101 tears the connection down —
 * no WS frames follow — so reading is safe.  content_len >= 0 (from the response
 * Content-Length): read EXACTLY that many body bytes on the full deadline (no
 * stall, no truncation).  content_len < 0 (no CL, incl. chunked): close-framed
 * drain to EOF, bounded to a short 2s window since such a peer closes promptly.
 * buf was malloc'd with *len bytes used; realloc grows it.  Best-effort: on
 * OOM/timeout it returns what it has (headers at least). */
static char* ws_drain_body(ray_sock_t fd, char* buf, size_t* len,
                           int64_t deadline, int64_t content_len) {
    size_t cap = *len, n = *len;
    size_t target = content_len >= 0
        ? (*len + (size_t)content_len > (size_t)Q_WS_HS_MAX
               ? (size_t)Q_WS_HS_MAX : *len + (size_t)content_len)
        : (size_t)Q_WS_HS_MAX;
    int64_t dl = deadline;
    if (content_len < 0) {                            /* close-framed: cap at 2s */
        int64_t soft = ws_now_ms() + 2000;
        if (soft < dl) dl = soft;
    }
    while (n < target) {
        int64_t budget = dl - ws_now_ms();
        if (budget <= 0) break;
        int wait = budget > 2000000000LL ? 2000000000 : (int)budget;
        if (ray_sock_wait_readable(fd, wait) <= 0) break;
        if (n == cap) {
            size_t nc = cap < 4096 ? 4096 : cap * 2;
            if (nc > (size_t)Q_WS_HS_MAX) nc = (size_t)Q_WS_HS_MAX;
            char* nb = (char*)realloc(buf, nc);
            if (!nb) break;
            buf = nb; cap = nc;
        }
        int64_t got = ray_sock_recv(fd, buf + n, cap - n);
        if (got <= 0) break;                          /* EOF/error -> body done */
        n += (size_t)got;
    }
    *len = n;
    return buf;
}

static int ws_tok_ieq(const char* s, size_t n, const char* lower) {
    size_t tn = strlen(lower);
    if (n != tn) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c != (unsigned char)lower[i]) return 0;
    }
    return 1;
}

static const struct phr_header* ws_hdr(const struct phr_header* h, size_t nh,
                                       const char* lower) {
    for (size_t i = 0; i < nh; i++)
        if (h[i].name && ws_tok_ieq(h[i].name, h[i].name_len, lower)) return &h[i];
    return NULL;
}

/* Case-insensitive comma-separated token membership (RFC 7230 list syntax). */
static int ws_list_has(const char* v, size_t n, const char* lower) {
    size_t i = 0;
    while (i < n) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t j = i;
        while (j < n && v[j] != ',' && v[j] != ' ' && v[j] != '\t') j++;
        if (j > i && ws_tok_ieq(v + i, j - i, lower)) return 1;
        i = j;
    }
    return 0;
}

/* Parse a Content-Length header value (leading/trailing OWS, all-digit, capped).
 * Returns the length, or -1 when absent/malformed. */
static int64_t ws_content_length(const struct phr_header* h, size_t nh) {
    const struct phr_header* clh = ws_hdr(h, nh, "content-length");
    if (!clh) return -1;
    const char* v = clh->value; size_t vn = clh->value_len;
    size_t i = 0, j = vn;
    while (i < j && (v[i] == ' ' || v[i] == '\t')) i++;
    while (j > i && (v[j-1] == ' ' || v[j-1] == '\t')) j--;
    if (i == j) return -1;
    int64_t cl = 0;
    for (; i < j; i++) {
        if (v[i] < '0' || v[i] > '9') return -1;
        cl = cl * 10 + (v[i] - '0');
        if (cl > (int64_t)Q_WS_HS_MAX) return (int64_t)Q_WS_HS_MAX;
    }
    return cl;
}

/* Build (handle;response) — handle OWNED, response copied.  Capacity-2 list
 * never reallocs, so the two appends cannot fail. */
static ray_t* ws_pair(ray_t* handle, const char* resp, size_t rlen) {
    ray_t* rs = ray_charv(resp, (int64_t)rlen);
    if (!rs || RAY_IS_ERR(rs)) { ray_release(handle); return rs ? rs : ray_error("oom", NULL); }
    ray_t* out = ray_list_new(2);
    if (!out || RAY_IS_ERR(out)) { ray_release(handle); ray_release(rs); return out ? out : ray_error("oom", NULL); }
    out = ray_list_append(out, handle);   /* retains */
    out = ray_list_append(out, rs);
    ray_release(handle);
    ray_release(rs);
    return out;
}

ray_t* q_ws_client_open(ray_t* hsym, ray_t* request) {
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    const char* reqp; int64_t reqn;                 /* charv or legacy STR text */
    if (!request || !q_text_bytes(request, &reqp, &reqn)) return ray_error("type", NULL);
    if (!hsym || hsym->type != -RAY_SYM) return ray_error("type", NULL);

    /* hsym text (BORROWED interned string) -> ":ws://..." */
    ray_t* nm = ray_sym_str(hsym->i64);
    if (!nm) return ray_error("type", NULL);
    const char* s = ray_str_ptr(nm);
    size_t sn = ray_str_len(nm);
    if (sn < 1 || s[0] != ':') return ray_error("domain", NULL);
    const char* rest = s + 1; size_t rn = sn - 1;         /* strip leading ':' */

    size_t schlen;
    if      (rn >= 5 && memcmp(rest, "ws://", 5) == 0)  schlen = 5;
    else if (rn >= 6 && memcmp(rest, "wss://", 6) == 0) return ray_error("nyi", NULL); /* D-A */
    else return ray_error("domain", NULL);

    /* Reuse the #223 URL parser by mapping ws://->http:// (same authority +
     * user:pass@ + default port 80 grammar). */
    char urlbuf[600];
    int m = snprintf(urlbuf, sizeof urlbuf, "http://%.*s",
                     (int)(rn - schlen), rest + schlen);
    if (m <= 0 || (size_t)m >= sizeof urlbuf) return ray_error("domain", NULL);
    q_http_url_t u;
    if (q_http_url_parse(urlbuf, (size_t)m, &u) != 0) return ray_error("domain", NULL);

    /* 16 random key bytes -> base64 Sec-WebSocket-Key (a fresh nonce, RFC §4.1).
     * From /dev/urandom on POSIX; xorshift(clock+addr) fallback.  The expected
     * accept is base64(sha1(key + GUID)) via the SAME .Q primitives the server
     * uses, so the round-trip is self-consistent. */
    uint8_t rawkey[16];
    int have_rand = 0;
#ifndef RAY_OS_WINDOWS
    { int rfd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
      if (rfd >= 0) { have_rand = read(rfd, rawkey, 16) == 16; close(rfd); } }
#endif
    if (!have_rand) {                     /* fallback (Windows / urandom absent) */
        uint64_t x = (uint64_t)ws_now_ms()
                   ^ ((uint64_t)(uintptr_t)&u << 16) ^ 0x9E3779B97F4A7C15ULL;
        for (int i = 0; i < 16; i++) { x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
            rawkey[i] = (uint8_t)((x * 0x2545F4914F6CDD1DULL) >> 56); }
    }
    ray_t* keyin = ray_str((const char*)rawkey, 16);
    if (!keyin || RAY_IS_ERR(keyin)) return keyin ? keyin : ray_error("oom", NULL);
    ray_t* keyb64 = q_dotq_btoa_fn(keyin);
    ray_release(keyin);
    if (!keyb64 || RAY_IS_ERR(keyb64)) return keyb64 ? keyb64 : ray_error("oom", NULL);

    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    size_t kl = ray_str_len(keyb64);
    if (kl > 64) { ray_release(keyb64); return ray_error("limit", NULL); }
    char cat[64 + sizeof guid];
    memcpy(cat, ray_str_ptr(keyb64), kl);
    memcpy(cat + kl, guid, sizeof guid - 1);
    ray_t* catin = ray_str(cat, kl + sizeof guid - 1);
    if (!catin || RAY_IS_ERR(catin)) { ray_release(keyb64); return catin ? catin : ray_error("oom", NULL); }
    ray_t* dig = q_dotq_sha1_fn(catin);
    ray_release(catin);
    if (!dig || RAY_IS_ERR(dig)) { ray_release(keyb64); return dig ? dig : ray_error("oom", NULL); }
    ray_t* accept = q_dotq_btoa_fn(dig);
    ray_release(dig);
    if (!accept || RAY_IS_ERR(accept)) { ray_release(keyb64); return accept ? accept : ray_error("oom", NULL); }

    /* Optional Basic-auth header from user:pass@ (in-tree base64 via .Q.btoa). */
    char authhdr[512]; authhdr[0] = '\0';
    if (u.userinfo[0]) {
        ray_t* uin = ray_str(u.userinfo, strlen(u.userinfo));
        ray_t* enc = (uin && !RAY_IS_ERR(uin)) ? q_dotq_btoa_fn(uin) : uin;
        if (uin && !RAY_IS_ERR(uin)) ray_release(uin);
        if (!enc || RAY_IS_ERR(enc)) { ray_release(keyb64); ray_release(accept); return enc ? enc : ray_error("oom", NULL); }
        snprintf(authhdr, sizeof authhdr, "Authorization: Basic %.*s\r\n",
                 (int)ray_str_len(enc), ray_str_ptr(enc));
        ray_release(enc);
    }

    /* Compose the request: splice our headers before the user's blank line.
     * The user string ends "...\r\n\r\n"; keep one CRLF, then append the WS
     * upgrade headers + the blank line (D-D: unconditional injection). */
    const char* uq = reqp;
    size_t uqn = (size_t)reqn;
    size_t head = uqn;
    if (uqn >= 4 && memcmp(uq + uqn - 4, "\r\n\r\n", 4) == 0) head = uqn - 2;
    else if (uqn >= 2 && memcmp(uq + uqn - 2, "\r\n", 2) == 0) head = uqn;
    size_t reqcap = head + 640 + kl;
    char* req = (char*)malloc(reqcap);
    if (!req) { ray_release(keyb64); ray_release(accept); return ray_error("oom", NULL); }
    memcpy(req, uq, head);
    int extra = snprintf(req + head, reqcap - head,
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %.*s\r\nSec-WebSocket-Version: 13\r\n%s\r\n",
        (int)kl, ray_str_ptr(keyb64), authhdr);
    ray_release(keyb64);
    if (extra <= 0 || (size_t)(head + extra) >= reqcap) {
        free(req); ray_release(accept); return ray_error("limit", NULL);
    }
    size_t reqlen = head + (size_t)extra;

    /* Connect + send + read the handshake response (blocking, pre-poll). */
    const char* err = "conn";
    ray_sock_t fd = q_http_client_connect(u.host, u.port, Q_WS_CONNECT_MS, &err);
    if (fd == RAY_INVALID_SOCK) { free(req); ray_release(accept); return ray_error(err, NULL); }
    int64_t deadline = ws_now_ms() + Q_WS_HS_MS;
    if (q_http_client_send_all(fd, req, reqlen, deadline) != 0) {
        free(req); ray_release(accept); ray_sock_close(fd); return ray_error("conn", NULL);
    }
    free(req);
    size_t rlen = 0;
    char* resp = ws_read_response(fd, deadline, &rlen, &err);
    if (!resp) { ray_release(accept); ray_sock_close(fd); return ray_error(err, NULL); }

    /* Parse status + validate the upgrade. */
    int minor, status; const char* msg; size_t mlen, nh = 32;
    struct phr_header h[32];
    int pr = phr_parse_response(resp, rlen, &minor, &status, &msg, &mlen, h, &nh, 0);
    if (pr <= 0) { ray_release(accept); free(resp); ray_sock_close(fd); return ray_error("conn", NULL); }

    if (status != 101) {                                  /* clean upgrade failure */
        int64_t cl = ws_content_length(h, nh);            /* -1 = close-framed */
        resp = ws_drain_body(fd, resp, &rlen, deadline, cl);  /* complete the body */
        ray_release(accept);
        ray_sock_close(fd);
        ray_t* r = ws_pair(ray_typed_null(-RAY_I32), resp, rlen);   /* (0Ni;resp) */
        free(resp);
        return r;
    }

    /* RFC 6455 §4.1: 101 must carry Upgrade: websocket, Connection contains the
     * `upgrade` token, and a matching Sec-WebSocket-Accept.  Any miss is a
     * broken handshake -> refused-class 'conn (D-C), socket closed, no fd leak. */
    const struct phr_header* up = ws_hdr(h, nh, "upgrade");
    const struct phr_header* co = ws_hdr(h, nh, "connection");
    const struct phr_header* ah = ws_hdr(h, nh, "sec-websocket-accept");
    int ok = up && ws_tok_ieq(up->value, up->value_len, "websocket") &&
             co && ws_list_has(co->value, co->value_len, "upgrade") &&
             ah && ah->value_len == ray_str_len(accept) &&
             memcmp(ah->value, ray_str_ptr(accept), ah->value_len) == 0;
    ray_release(accept);
    if (!ok) { free(resp); ray_sock_close(fd); return ray_error("conn", NULL); }

    /* Success: register the fd as a client WS poll handle. */
    q_ws_conn_t* c = q_ws_conn_new_role(1);
    if (!c) { free(resp); ray_sock_close(fd); return ray_error("oom", NULL); }
    int64_t id = ray_ws_client_register(fd, c);
    if (id < 0) { q_ws_conn_free(c); free(resp); ray_sock_close(fd); return ray_error("conn", NULL); }
    int64_t sockfd = ray_ipc_fd_of_handle(id);            /* kdb handle == the fd */
    ray_t* handle = ray_i32((int32_t)(sockfd >= 0 ? sockfd : id));
    if (!handle || RAY_IS_ERR(handle)) {                  /* unwind the registration */
        ray_ipc_close(id); free(resp);
        return handle ? handle : ray_error("oom", NULL);
    }
    ray_t* r = ws_pair(handle, resp, rlen);               /* (handle;resp) */
    free(resp);
    if (!r || RAY_IS_ERR(r)) { ray_ipc_close(id); return r ? r : ray_error("oom", NULL); }
    return r;
}
