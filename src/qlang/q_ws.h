/* q_ws — RFC 6455 WebSocket server on the single listening port
 * (kb/websockets.md).  All WS semantics (handshake, framing, `.z.w*`
 * dispatch) live here, outside the frozen base; ipc.c only pumps bytes.
 * Scope: server only — no TLS/wss, no WS client, deflate declined. */
#ifndef Q_WS_H
#define Q_WS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <rayforce.h>
#include "core/sock.h"

/* ---- pure frame codec (unit-testable, no state) ---- */
typedef struct {
    uint8_t  fin, rsv, opcode, masked;
    uint64_t len;          /* payload length */
    uint8_t  mask[4];      /* zeroed when unmasked */
    size_t   hdr_len;      /* total header bytes (2..14) */
} q_ws_hdr_t;

/* Parse a header from b[0..n): hdr_len (>0) complete, 0 need more,
 * -1 protocol violation (MSB set / non-minimal encoding, RFC §5.2). */
int q_ws_hdr_parse(const uint8_t* b, size_t n, q_ws_hdr_t* h);

/* Encode an UNMASKED server frame header into out[10]; returns 2/4/10. */
size_t q_ws_hdr_encode(uint8_t* out, int fin, int opcode, uint64_t len);

/* XOR p[0..n) with key, starting at absolute payload offset off (RFC §5.3). */
void q_ws_mask_apply(uint8_t* p, size_t n, const uint8_t key[4], uint64_t off);

bool q_ws_utf8_ok(const uint8_t* p, size_t n);

/* ---- handshake ---- */
struct phr_header;   /* picohttpparser */
/* Answer an Upgrade request on fd: 101 -> 1, else 400/426 sent -> 0.
 * Accept = base64(SHA1(key+GUID)) via .Q.sha1/.Q.btoa (needs a q runtime). */
int q_ws_handshake(ray_sock_t fd, const struct phr_header* hdrs, size_t nh);

/* ---- per-connection state machine (driven by ipc.c's read pump) ---- */
typedef struct q_ws_conn q_ws_conn_t;
q_ws_conn_t* q_ws_conn_new(void);
void         q_ws_conn_free(q_ws_conn_t* c);
int64_t      q_ws_want(const q_ws_conn_t* c);   /* next rx size to request */

/* Consume exactly the wanted bytes (a DETACHED buffer; unmasked in place);
 * may write pong/close/echo frames and dispatch `.z.ws`.  Next want (>0) or
 * 0 = close.  RE-ENTRANCY: a handler may close this very conn, freeing `c` —
 * all state mutation precedes dispatch; `c` is never touched after it. */
int64_t      q_ws_feed(q_ws_conn_t* c, ray_sock_t fd,
                       uint8_t* data, size_t len);

/* neg[h] write path: string -> text frame, byte vector -> binary frame
 * (ref/dotz.md).  0 ok; -1 io failure; -2 bad payload type. */
int q_ws_send(ray_sock_t fd, ray_t* msg);

/* ---- lifecycle (called from ipc.c) ---- */
void q_ws_on_open(int64_t qhandle);    /* fires `.z.wo` if set */
void q_ws_on_close(int64_t qhandle);   /* fires `.z.wc` if set */

#endif /* Q_WS_H */
