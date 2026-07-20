/* q_http_client — blocking outbound HTTP client for `.Q.hg` (GET) / `.Q.hp`
 * (POST).  Behaviour pinned from qdocs ref/dotq.md (clean room); doc-unpinned
 * details (timeouts, size cap, redirect + https-error policy) are openq-authored
 * and recorded in the PR.  Synchronous/blocking is kdb-true for these verbs.
 *
 * The connect + send + response-read seams are shaped for REUSE: the queued
 * WebSocket client rides q_http_client_connect + q_http_client_send_all for its
 * Upgrade handshake (see the PR Continue-here). */
#ifndef Q_HTTP_CLIENT_H
#define Q_HTTP_CLIENT_H

#include <rayforce.h>
#include "core/sock.h"

/* Decoded body/response size cap — mirrors q_http.c's 32 MiB served-file cap. */
#define Q_HTTP_CLIENT_MAX (32ll * 1024 * 1024)

/* Parsed URL pieces.  host/path/userinfo are NUL-terminated copies. */
typedef struct {
    int      scheme;        /* 0 = http, 1 = https */
    char     host[256];
    uint16_t port;          /* default 80 (http) / 443 (https) */
    char     path[1024];    /* request target incl. leading '/' + query; '/' when empty */
    char     userinfo[256]; /* "user:pass" or "" */
} q_http_url_t;

/* Parse a `.Q.hg`/`.Q.hp` URL (an optional leading kdb ':' is stripped).
 * Requires an explicit http://|https:// scheme.  Rejects unknown scheme, empty
 * host, over-long components, control/CR/LF bytes (request-injection guard),
 * bracketed IPv6 literals (deferred), and port overflow.  Fragment (`#...`) is
 * dropped; query (`?...`) is kept in path.  Returns 0 ok, -1 malformed. */
int q_http_url_parse(const char* url, size_t n, q_http_url_t* out);

/* Extract the body from a COMPLETE response buffer (headers + framed body).
 * On success sets status and body/body_len (body points into buf; a chunked
 * body is de-framed IN PLACE, so buf is mutated).  When `gzip` is non-NULL it is
 * set to 1 iff the response carried `Content-Encoding: gzip` (caller inflates the
 * extracted body), else 0.  Returns 0 ok, -1 malformed, -2 decoded body exceeds
 * Q_HTTP_CLIENT_MAX. */
int q_http_client_extract(char* buf, size_t len, int* status,
                          const char** body, size_t* body_len, int* gzip);

/* --- reusable blocking pipeline seams (also the WS-client hook) --- */

/* Connect a blocking socket to host:port within timeout_ms.  Returns fd >= 0,
 * or RAY_INVALID_SOCK with *err set to a bare error-class word (never NULL). */
ray_sock_t q_http_client_connect(const char* host, uint16_t port,
                                 int timeout_ms, const char** err);

/* Send the whole buffer within an absolute deadline (EAGAIN/EINTR-aware; does
 * NOT rely on ray_sock_send's unbounded EAGAIN wait).  Returns 0 ok / -1. */
int q_http_client_send_all(ray_sock_t fd, const void* buf, size_t len,
                           int64_t deadline_ms);

/* Read a whole HTTP response into a grown heap buffer, framing-aware (stops at
 * the Content-Length / chunked terminator; reads to EOF only when close-framed)
 * bounded by an absolute deadline + Q_HTTP_CLIENT_MAX.  Returns malloc'd buf
 * (caller frees) with *len set, or NULL with *err set (bare class word). */
char* q_http_client_read_response(ray_sock_t fd, size_t* len,
                                  int64_t deadline_ms, const char** err);

/* `.Q.c.hg` — GET; x is a string or symbol URL atom, returns the body string. */
ray_t* q_dotq_hg_fn(ray_t* x);
/* `.Q.c.hp` — POST [url; mimeType; body], returns the body string (vary/3). */
ray_t* q_dotq_hp_fn(ray_t** args, int64_t nargs);

/* Low-level raw HTTP client (kb/http.md §"low level HTTP request mechanism"):
 * apply a raw request STRING to a `:http://host[:port]` hsym.  Sends the request
 * VERBATIM (no header injection — the caller supplies the full request line +
 * headers + terminating blank line) and returns the ENTIRE response (status line
 * + headers + blank line + body) as a string atom.  Chunked bodies are
 * reassembled (V3.3); the response headers are returned unchanged.  `https://`
 * -> 'nyi (TLS tier).  Reuses the #223 connect (30s) + send/read (30s) budgets
 * and the 32 MiB cap.  PROVISIONAL pre-C3: the return is a string ATOM.  hsym +
 * request are BORROWED; returns owned (or an owned bare-class ray_error). */
ray_t* q_http_raw_client(ray_t* hsym, ray_t* request);

#endif /* Q_HTTP_CLIENT_H */
