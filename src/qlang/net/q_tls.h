/* q_tls — TLS client sessions over dlopen'd system OpenSSL (never linked, so a
 * box without libssl still builds and runs).  Sessions hang off the
 * ray_sock_send/ray_sock_recv overlay, and config is re-read from the
 * environment on EVERY connection (KX_SSL_* wins, kb/ssl.md:75). */
#ifndef Q_TLS_H
#define Q_TLS_H

#include <rayforce.h>
#include "core/sock.h"

/* Client handshake on a connected fd, then attach.  `host` drives SNI and peer
 * name verification.  0 ok; -1 with *err set to a bare error class: `nyi` when
 * this box has no usable OpenSSL (the degradation ladder — https stays as
 * unimplemented as it was before the tier), `conn` for a real TLS failure. */
int q_tls_client_start(ray_sock_t fd, const char* host, const char** err);

/* `-E` TLS server mode: 0 plain, 1 plain and TLS, 2 TLS only (cmdline.md). */
int  q_tls_server_mode(void);
void q_tls_server_mode_set(int mode);

/* The accept-side handshake, driven NON-BLOCKINGLY from poll events: `fd` is
 * already non-blocking and registered, and each readable event calls this once.
 * `state` is an opaque per-connection cookie the CALLER stores (NULL to start);
 * keeping it caller-side means concurrent negotiations are limited only by the
 * connections the server already accepted, never by a table in here.
 *   1 = done, TLS attached to the send/recv overlay — serve the connection
 *   0 = in progress, call again on the next event
 *  -1 = refused/failed — the caller closes (this is why `-E 2` cannot serve
 *       plaintext at any depth)
 * Never waits: a peer that connects and says nothing costs one idle fd, not a
 * parked thread, so it cannot stall accepts or any established connection. */
int q_tls_server_handshake(ray_sock_t fd, void** state);

/* Drop the cookie for a connection that died mid-negotiation (no-op on NULL).
 * A COMPLETED session is owned by the overlay and freed by ray_sock_close. */
void q_tls_server_handshake_abort(void** state);

/* Both non-zero `-E` modes decide on the first byte (a TLS record header, RFC 8446).
 * Peeks without consuming: -1 = nothing readable yet (wait for another event),
 * 0 = not a ClientHello — `-E 1` speaks plain, `-E 2` refuses — 1 = speak TLS. */
int q_tls_server_sniff(ray_sock_t fd);

ray_t* q_tls_info(void);              /* `(-26!)[]`      — basics/internal.md:350 */
ray_t* q_tls_conn_info(ray_sock_t fd);/* `.z.e`/per-handle — ref/dotz.md:215      */

#endif /* Q_TLS_H */
