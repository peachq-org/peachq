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

ray_t* q_tls_info(void);     /* `(-26!)[]` — basics/internal.md:350 */
ray_t* q_tls_dotz_e(void);   /* `.z.e`     — ref/dotz.md:215        */

#endif /* Q_TLS_H */
