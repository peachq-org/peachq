/* q_http — the single-port HTTP static-serving slice (kb/http.md: kdb's
 * webserver shares the IPC listening port).  ipc.c's protocol sniff hands a
 * complete request-header block to q_http_respond; everything HTTP-semantic
 * (parse via vendored picohttpparser, traversal guard, MIME, the `.z.ph`
 * override hook, response bytes) lives here, outside the frozen base.
 * Scope: GET (`.z.ph`), POST (`.z.pp`), the other HTTP methods (`.z.pm`:
 * OPTIONS/PUT/DELETE/PATCH), the `.z.ac` auth gate that precedes every
 * handler, and the WebSocket upgrade hand-off (q_ws.c) — no TLS.
 * The docroot and MIME map are single-homed on the `.h` constants (`.h.HOME` /
 * `.h.ty`, defined in src/qlang/h.q) at request time, with the built-in "html"
 * docroot and MIME table as defensive fallbacks; both are user-overridable. */
#ifndef Q_HTTP_H
#define Q_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <rayforce.h>          /* ray_t — the `.z.ac` return-value parser prototype */
#include "core/sock.h"

/* Percent-decode a request path exactly once into out (NUL-terminated).
 * Returns decoded length, or -1 on bad hex / %00 / control bytes / overflow. */
int q_http_decode_path(const char* in, size_t n, char* out, size_t outsz);

/* Post-decode traversal guard: rejects `.`/`..` segments, `\` and `:`.
 * Empty segments are allowed (skipped by the opener). */
bool q_http_path_ok(const char* p, size_t n);

/* ext -> Content-Type (case-insensitive, last extension wins);
 * application/octet-stream fallback.  The built-in table (no `.h.ty` consult). */
const char* q_http_mime_type(const char* path);

/* ext(path) -> Content-Type honoring a user `.h.ty` override (env dict), copying
 * an override hit into scratch[]; falls back to q_http_mime_type on any miss /
 * wrong-shape / unsafe (control-byte) value.  Returns scratch or a static literal. */
const char* q_http_mime_lookup(const char* path, char* scratch, size_t scratchsz);

/* Resolve the static docroot dir: `.h.HOME` (env string) when usable, else the
 * built-in "html".  Rejects control bytes (NUL/CR/LF).  Fills buf; returns it. */
const char* q_http_docroot(char* buf, size_t bufsz);

/* Open `./html/<rel>` for reading with NO pathname re-resolution: a dirfd +
 * per-segment openat(O_NOFOLLOW) walk (symlinks are never followed — policy).
 * Returns an fd positioned at 0 with *size_out set, or -1 (missing, symlink,
 * non-regular, traversal).  Windows: lexical guard + plain open (no
 * O_NOFOLLOW; reparse-point escape is a documented platform divergence). */
int q_http_open_doc(const char* rel, size_t n, int64_t* size_out);

/* Embedded default site (src/qlang/html/, baked in via html_assets_gen.h): the
 * fallback served when no on-disk ./html docroot exists. Pure table match (no
 * filesystem, no traversal). q_http_asset_lookup normalizes `path` (strips
 * leading '/', empty/"/" -> "index.html") then exact-matches the table; returns
 * the bytes + *len_out (Content-Length; the array's trailing NUL is excluded),
 * or NULL on a miss. The count/at accessors expose the table for tests. */
const unsigned char* q_http_asset_lookup(const char* path, size_t* len_out);
size_t               q_http_asset_count(void);
bool                 q_http_asset_at(size_t i, const char** path_out,
                                     const unsigned char** bytes_out, size_t* len_out);

/* Compose + send one minimal text/plain response (used for all error codes). */
void q_http_send_simple(ray_sock_t fd, int code, const char* reason);

/* Bounded-deadline whole-buffer send (the #217 anti-wedge policy); shared
 * with q_ws.c's frame writes.  0 ok / -1 failed-or-expired. */
int q_http_send_all(ray_sock_t fd, const void* buf, size_t len, int secs);

/* Serve one complete request-header block: 401 on authed listeners, `.z.ph`
 * override for GET when set, else the static file server; a GET with
 * `Upgrade: websocket` routes to q_ws_handshake — returns 1 when the
 * connection upgraded to WebSocket (101 sent; caller switches protocol),
 * else 0 (response fully handled: file/404/400/426/501...).  Non-GET -> 501;
 * unparseable -> 400.  Never closes fd (the caller owns the socket). */
int q_http_respond(ray_sock_t fd, const uint8_t* req, size_t len,
                   bool auth_required);

/* Structural parse of a `.z.ac` return value `(status; payload)` (ref/dotz.md):
 * item0 an integer atom (byte/short/int/long), item1 a RAY_STR atom.  On a
 * well-formed 2-item list writes *status_out and the payload ptr/len (aliasing
 * r's storage — valid only while r is retained) and returns true; false on any
 * other shape.  Pure (no socket/send) so it is unit-tested directly; the caller
 * (the gate) maps the status to reject / proceed / custom / fallback. */
bool q_http_zac_parse(const ray_t* r, int64_t* status_out,
                      const char** pay_out, size_t* paylen_out);

#endif /* Q_HTTP_H */
