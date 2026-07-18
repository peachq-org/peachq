/* q_http — the single-port HTTP static-serving slice (kb/http.md: kdb's
 * webserver shares the IPC listening port).  ipc.c's protocol sniff hands a
 * complete request-header block to q_http_respond; everything HTTP-semantic
 * (parse via vendored picohttpparser, traversal guard, MIME, the `.z.ph`
 * override hook, response bytes) lives here, outside the frozen base.
 * Scope: GET + `.z.ph` only — no TLS, no WebSockets (Upgrade -> 501), no
 * `.h` namespace; docroot is `./html` under the process cwd at request time. */
#ifndef Q_HTTP_H
#define Q_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/sock.h"

/* Percent-decode a request path exactly once into out (NUL-terminated).
 * Returns decoded length, or -1 on bad hex / %00 / control bytes / overflow. */
int q_http_decode_path(const char* in, size_t n, char* out, size_t outsz);

/* Post-decode traversal guard: rejects `.`/`..` segments, `\` and `:`.
 * Empty segments are allowed (skipped by the opener). */
bool q_http_path_ok(const char* p, size_t n);

/* ext -> Content-Type (case-insensitive, last extension wins);
 * application/octet-stream fallback. */
const char* q_http_mime_type(const char* path);

/* Open `./html/<rel>` for reading with NO pathname re-resolution: a dirfd +
 * per-segment openat(O_NOFOLLOW) walk (symlinks are never followed — policy).
 * Returns an fd positioned at 0 with *size_out set, or -1 (missing, symlink,
 * non-regular, traversal).  Windows: lexical guard + plain open (no
 * O_NOFOLLOW; reparse-point escape is a documented platform divergence). */
int q_http_open_doc(const char* rel, size_t n, int64_t* size_out);

/* Compose + send one minimal text/plain response (used for all error codes). */
void q_http_send_simple(ray_sock_t fd, int code, const char* reason);

/* Serve one complete request-header block: 401 on authed listeners, `.z.ph`
 * override for GET when set, else the static file server; non-GET/Upgrade ->
 * 501; unparseable -> 400.  Never closes fd (the caller owns the socket). */
void q_http_respond(ray_sock_t fd, const uint8_t* req, size_t len,
                    bool auth_required);

#endif /* Q_HTTP_H */
