/* q_ws — see q_ws.h.  Behaviour pinned from qdocs kb/websockets.md +
 * ref/dotz.md (clean room); doc-unpinned choices recorded in the PR Decisions. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L    /* clock_gettime / CLOCK_MONOTONIC */
#endif
#include <rayforce.h>
#include "qlang/q_ws.h"
#include "qlang/q_http.h"      /* q_http_send_all — bounded sends */
#include "qlang/q_dotz.h"      /* the `.z.ws`/`.z.wo`/`.z.wc` handler slots */
#include "qlang/q_builtins.h"  /* q_dotq_sha1_fn / q_dotq_btoa_fn — accept key */
#include "qlang/q_fmt.h"       /* q_console_str/_reset — drain handler output */
#include "lang/internal.h"     /* call_fn1 */
#include "mem/sys.h"
#include "picohttpparser.h"
#include <string.h>
#include <stdio.h>
#include "qlang/q_registry.h"   /* q_text_bytes — charv/string text accessor */
#include <time.h>
#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <fcntl.h>     /* open — /dev/urandom mask-key seed */
  #include <unistd.h>    /* read, close */
#endif

#define Q_WS_MAX_MSG    (32ll * 1024 * 1024)  /* per-message cap (#217 parity) */
#define Q_WS_MAX_FRAGS  65536u                /* fragment-count cap per message */
#define Q_WS_CTRL_SECS  5                     /* control-frame send deadline */
#define Q_WS_DATA_SECS  30                    /* data-frame send deadline */

#define Q_WS_OP_CONT  0x0
#define Q_WS_OP_TEXT  0x1
#define Q_WS_OP_BIN   0x2
#define Q_WS_OP_CLOSE 0x8
#define Q_WS_OP_PING  0x9
#define Q_WS_OP_PONG  0xA

/* Per-thread PRNG for the 4-byte per-frame mask key (client role, RFC §5.3).
 * Independent of the q `rand` RNG so masking never perturbs user-visible random
 * sequences.  xorshift64* (as the GUID generator, src/ops/system.c) SEEDED from
 * /dev/urandom (a strong entropy source, RFC §5.3) on POSIX — clock+address is
 * the fallback (and the Windows rot-guard path).  A 4-byte key is inherently
 * 32-bit, so a birthday repeat after ~2^16 frames is intrinsic to the WS mask
 * size, not this generator.  An openq choice (masking is transparent); PR. */
static __thread uint64_t ws_mask_rng = 0;
static uint64_t ws_mask_seed(void) {
    uint64_t x;
#ifdef RAY_OS_WINDOWS
    x = (uint64_t)GetTickCount64() ^ ((uint64_t)time(NULL) << 20)
        ^ (uint64_t)(uintptr_t)&ws_mask_rng ^ 0x9E3779B97F4A7C15ULL;
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t got = read(fd, &x, sizeof x);
        close(fd);
        if (got == (ssize_t)sizeof x && x != 0) return x;   /* strong entropy */
    }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    x = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 32)
        ^ (uint64_t)(uintptr_t)&ws_mask_rng ^ 0x9E3779B97F4A7C15ULL;
#endif
    return x ? x : 0x9E3779B97F4A7C15ULL;
}
static uint32_t ws_mask_next(void) {
    uint64_t x = ws_mask_rng;
    if (x == 0) x = ws_mask_seed();
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    ws_mask_rng = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

/* ---- pure codec ---- */

int q_ws_hdr_parse(const uint8_t* b, size_t n, q_ws_hdr_t* h) {
    if (n < 2) return 0;
    h->fin    = (uint8_t)((b[0] >> 7) & 1);
    h->rsv    = (uint8_t)((b[0] >> 4) & 7);
    h->opcode = (uint8_t)(b[0] & 0xF);
    h->masked = (uint8_t)((b[1] >> 7) & 1);
    uint64_t len = b[1] & 0x7F;
    size_t   pos = 2;
    if (len == 126) {
        if (n < 4) return 0;
        len = ((uint64_t)b[2] << 8) | b[3];
        if (len < 126) return -1;              /* non-minimal (RFC §5.2) */
        pos = 4;
    } else if (len == 127) {
        if (n < 10) return 0;
        if (b[2] & 0x80) return -1;            /* MSB must be 0 (RFC §5.2) */
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | b[2 + i];
        if (len < 65536) return -1;            /* non-minimal */
        pos = 10;
    }
    if (h->masked) {
        if (n < pos + 4) return 0;
        memcpy(h->mask, b + pos, 4);
        pos += 4;
    } else {
        memset(h->mask, 0, 4);
    }
    h->len     = len;
    h->hdr_len = pos;
    return (int)pos;
}

size_t q_ws_hdr_encode(uint8_t* out, int fin, int opcode, uint64_t len) {
    out[0] = (uint8_t)((fin ? 0x80 : 0x00) | (opcode & 0xF));
    if (len <= 125)   { out[1] = (uint8_t)len; return 2; }
    if (len <= 65535) { out[1] = 126; out[2] = (uint8_t)(len >> 8);
                        out[3] = (uint8_t)len; return 4; }
    out[1] = 127;
    for (int i = 0; i < 8; i++) out[2 + i] = (uint8_t)(len >> (8 * (7 - i)));
    return 10;
}

void q_ws_mask_apply(uint8_t* p, size_t n, const uint8_t key[4], uint64_t off) {
    for (size_t i = 0; i < n; i++) p[i] ^= key[(off + i) & 3];
}

/* UTF-8 validation — RFC 6455 §8.1 requires closing 1007 on invalid text. */
bool q_ws_utf8_ok(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }
        size_t   need; uint32_t cp, min;
        if      ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; min = 0x10000; }
        else return false;
        if (i + need >= n) return false;                          /* truncated */
        for (size_t k = 1; k <= need; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF) return false;              /* overlong / range */
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;           /* surrogate */
        i += need + 1;
    }
    return true;
}

/* ---- frame sends (server frames unmasked; bounded deadlines) ---- */

/* Emit one frame.  mask != 0 (client role) masks the payload with a fresh key
 * (RFC §5.3): the masked payload is built and the header keyed BEFORE any bytes
 * leave, so an allocation failure can never ship a truncated/dangling frame. */
static int ws_frame_send(ray_sock_t fd, int opcode, const void* payload,
                         size_t n, int secs, int mask) {
    uint8_t hdr[14];
    size_t  hl = q_ws_hdr_encode(hdr, 1, opcode, n);
    uint8_t  stackbuf[256];
    uint8_t* mp = NULL;
    uint8_t  key[4];
    if (mask) {
        uint32_t k = ws_mask_next();
        key[0] = (uint8_t)(k >> 24); key[1] = (uint8_t)(k >> 16);
        key[2] = (uint8_t)(k >> 8);  key[3] = (uint8_t)k;
        hdr[1] |= 0x80;                       /* set the mask bit */
        memcpy(hdr + hl, key, 4);
        hl += 4;
        if (n) {                              /* mask a private copy up-front */
            mp = n <= sizeof stackbuf ? stackbuf : (uint8_t*)ray_sys_alloc(n);
            if (!mp) return -1;
            memcpy(mp, payload, n);
            q_ws_mask_apply(mp, n, key, 0);
        }
    }
    int rc = q_http_send_all(fd, hdr, hl, secs);
    if (rc == 0 && n)
        rc = q_http_send_all(fd, mask ? (const void*)mp : payload, n, secs);
    if (mp && mp != stackbuf) ray_sys_free(mp);
    return rc == 0 ? 0 : -1;
}

static void ws_close_send(ray_sock_t fd, int code, int mask) {
    uint8_t body[2] = { (uint8_t)(code >> 8), (uint8_t)code };
    ws_frame_send(fd, Q_WS_OP_CLOSE, body, code ? 2 : 0, Q_WS_CTRL_SECS, mask);
}

/* ---- `.z.ws`/`.z.wo`/`.z.wc` dispatch ---- */

/* Drain handler show/0N! output — nothing else drains here (q_zts_tick). */
static void ws_console_drain(void) {
    const char* con = q_console_str();
    if (con && *con) { fputs(con, stdout); fflush(stdout); }
    q_console_reset();
}

static void ws_hook_fire(ray_t* fn, ray_t* arg, const char* name) {
    ray_retain(fn);                       /* survive a re-assign mid-call */
    ray_t* r = call_fn1(fn, arg);
    ray_release(fn);
    ws_console_drain();
    if (r) {
        if (RAY_IS_ERR(r)) {              /* D2: log, keep the connection */
            fprintf(stderr, "ws: %s handler raised an error\n", name);
            ray_error_free(r);
        } else if (r != RAY_NULL_OBJ) {
            ray_release(r);               /* result is NOT auto-sent (dotz.md) */
        }
    }
}

void q_ws_on_open(int64_t qhandle) {
    ray_t* fn = q_dotz_get(".z.wo", 5);             /* borrowed; NULL = unset (no default) */
    if (!fn) return;
    ray_t* h = ray_i32((int32_t)qhandle);
    ws_hook_fire(fn, h, ".z.wo");
    ray_release(h);
}

void q_ws_on_close(int64_t qhandle) {
    ray_t* fn = q_dotz_get(".z.wc", 5);
    if (!fn) return;
    ray_t* h = ray_i32((int32_t)qhandle);
    ws_hook_fire(fn, h, ".z.wc");
    ray_release(h);
}

/* ---- per-connection state machine (type visible to the dispatch below) ---- */

enum { WS_HDR = 0, WS_HDREXT = 1, WS_PAYLOAD = 2 };

struct q_ws_conn {
    int        st;
    int        role;           /* Q_WS_SERVER / Q_WS_CLIENT */
    uint8_t    hbuf[14];       /* header accumulator (max 2+8+4) */
    size_t     hlen;
    q_ws_hdr_t h;              /* current frame's parsed header */
    uint8_t*   msg;            /* fragmentation accumulator */
    size_t     msg_len, msg_cap;
    int        msg_op;         /* TEXT/BIN of the in-flight message; 0 = none */
    uint32_t   msg_frames;     /* fragment count (D12 cap) */
    int64_t    want;
};

/* One complete message: text -> string atom, binary -> byte vector (the
 * PROVISIONAL pre-C3 arg contract).  Unset handler: the SERVER default echoes
 * a binary message verbatim (kb/websockets.md); text drops (D1).  A CLIENT with
 * `.z.ws` unset drops BOTH — echoing a server's message back to it is not a
 * client default (D-B).  Called only with conn state already advanced (handler
 * may close us). */
static void ws_dispatch(q_ws_conn_t* c, ray_sock_t fd, int opcode,
                        const uint8_t* p, size_t n) {
    ray_t* fn = q_dotz_get(".z.ws", 5);             /* borrowed; NULL = unset */
    if (!fn) {
        if (opcode == Q_WS_OP_BIN && c->role == Q_WS_SERVER)
            ws_frame_send(fd, Q_WS_OP_BIN, p, n, Q_WS_DATA_SECS, 0);
        else
            fprintf(stderr, "ws: message dropped (.z.ws not set)\n");
        return;
    }
    ray_t* arg = (opcode == Q_WS_OP_TEXT)
        ? ray_charv((const char*)p, (int64_t)n)
        : ray_vec_from_raw(RAY_BYTE_ONLY, p, (int64_t)n);
    if (!arg || RAY_IS_ERR(arg)) {
        if (arg) ray_error_free(arg);
        fprintf(stderr, "ws: message arg allocation failed\n");
        return;
    }
    ws_hook_fire(fn, arg, ".z.ws");
    ray_release(arg);
}

q_ws_conn_t* q_ws_conn_new(void) { return q_ws_conn_new_role(0); }

q_ws_conn_t* q_ws_conn_new_role(int is_client) {
    q_ws_conn_t* c = (q_ws_conn_t*)ray_sys_alloc(sizeof *c);
    if (!c) return NULL;
    memset(c, 0, sizeof *c);
    c->st   = WS_HDR;
    c->role = is_client ? Q_WS_CLIENT : Q_WS_SERVER;
    c->want = 2;
    return c;
}

void q_ws_conn_free(q_ws_conn_t* c) {
    if (!c) return;
    if (c->msg) ray_sys_free(c->msg);
    ray_sys_free(c);
}

int64_t q_ws_want(const q_ws_conn_t* c) { return c->want; }

static int msg_append(q_ws_conn_t* c, const uint8_t* p, size_t n) {
    if (n == 0) return 0;
    if (c->msg_len + n > c->msg_cap) {
        size_t cap = c->msg_cap ? c->msg_cap * 2 : 4096;
        while (cap < c->msg_len + n) cap *= 2;
        uint8_t* nb = (uint8_t*)ray_sys_alloc(cap);
        if (!nb) return -1;
        if (c->msg) { memcpy(nb, c->msg, c->msg_len); ray_sys_free(c->msg); }
        c->msg     = nb;
        c->msg_cap = cap;
    }
    memcpy(c->msg + c->msg_len, p, n);
    c->msg_len += n;
    return 0;
}

/* Process one complete (unmasked) payload: next want, or 0 = close.  Every
 * dispatch is tail-positioned — state advances BEFORE q code runs (q_ws.h). */
static int64_t ws_process(q_ws_conn_t* c, ray_sock_t fd,
                          uint8_t* p, size_t n) {
    int op   = c->h.opcode;
    int fin  = c->h.fin;
    int mask = (c->role == Q_WS_CLIENT);         /* client emits masked frames */
    c->st   = WS_HDR;
    c->hlen = 0;
    c->want = 2;

    if (op == Q_WS_OP_PING) {
        if (ws_frame_send(fd, Q_WS_OP_PONG, p, n, Q_WS_CTRL_SECS, mask) != 0)
            return 0;              /* unsendable pong: broken/flooding peer */
        return c->want;
    }
    if (op == Q_WS_OP_PONG) return c->want;      /* unsolicited — ignored */
    if (op == Q_WS_OP_CLOSE) {
        if (n == 1) { ws_close_send(fd, 1002, mask); return 0; }  /* RFC §5.5.1 */
        int code = (n >= 2) ? ((p[0] << 8) | p[1]) : 0;
        ws_close_send(fd, code, mask);  /* echo the code (or empty close) — D10 */
        return 0;
    }

    if (op == Q_WS_OP_TEXT || op == Q_WS_OP_BIN) {
        if (!fin) {                              /* start of a fragmented msg */
            c->msg_op     = op;
            c->msg_frames = 1;
            if (msg_append(c, p, n) != 0) { ws_close_send(fd, 1009, mask); return 0; }
            return c->want;
        }
        int64_t ret = c->want;
        if (op == Q_WS_OP_TEXT && !q_ws_utf8_ok(p, n)) {
            ws_close_send(fd, 1007, mask);
            return 0;
        }
        ws_dispatch(c, fd, op, p, n);            /* tail — c not touched after */
        return ret;
    }

    if (msg_append(c, p, n) != 0) { ws_close_send(fd, 1009, mask); return 0; }
    if (!fin) return c->want;
    /* message complete: detach the accumulator, reset state, THEN dispatch */
    uint8_t* m    = c->msg;
    size_t   mlen = c->msg_len;
    int      mop  = c->msg_op;
    int64_t  ret  = c->want;
    c->msg = NULL; c->msg_len = 0; c->msg_cap = 0;
    c->msg_op = 0; c->msg_frames = 0;
    if (mop == Q_WS_OP_TEXT && !q_ws_utf8_ok(m, mlen)) {
        ray_sys_free(m);
        ws_close_send(fd, 1007, mask);
        return 0;
    }
    ws_dispatch(c, fd, mop, m ? m : (uint8_t*)"", mlen);
    if (m) ray_sys_free(m);
    return ret;
}

int64_t q_ws_feed(q_ws_conn_t* c, ray_sock_t fd, uint8_t* data, size_t len) {
    int m = (c->role == Q_WS_CLIENT);            /* our protocol-close frames */
    if (c->st == WS_PAYLOAD) {
        if (c->h.masked) q_ws_mask_apply(data, len, c->h.mask, 0);  /* server input only */
        return ws_process(c, fd, data, len);
    }

    if (c->hlen + len > sizeof c->hbuf) { ws_close_send(fd, 1002, m); return 0; }
    memcpy(c->hbuf + c->hlen, data, len);
    c->hlen += len;
    int pr = q_ws_hdr_parse(c->hbuf, c->hlen, &c->h);
    if (pr < 0) { ws_close_send(fd, 1002, m); return 0; }
    if (pr == 0) {
        uint8_t len7   = (uint8_t)(c->hbuf[1] & 0x7F);
        int     masked = (c->hbuf[1] & 0x80) != 0;
        size_t  full   = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0)
                           + (masked ? 4 : 0);
        c->st   = WS_HDREXT;
        c->want = (int64_t)(full - c->hlen);
        return c->want;
    }

    /* header complete — validate the mask per role (RFC §5.1): a SERVER
     * requires masked client frames; a CLIENT requires UNMASKED server frames
     * (a masked frame from the server is a protocol error -> 1002). */
    int want_masked = (c->role == Q_WS_SERVER);
    if (c->h.masked != want_masked || c->h.rsv != 0) { ws_close_send(fd, 1002, m); return 0; }
    if (c->h.opcode >= 0x8) {                    /* control frame */
        if (c->h.opcode > Q_WS_OP_PONG ||
            !c->h.fin || c->h.len > 125) { ws_close_send(fd, 1002, m); return 0; }
    } else if (c->h.opcode > Q_WS_OP_BIN) {      /* reserved data opcode */
        ws_close_send(fd, 1002, m); return 0;
    } else if (c->h.opcode == Q_WS_OP_CONT) {
        if (c->msg_op == 0) { ws_close_send(fd, 1002, m); return 0; }
        if (++c->msg_frames > Q_WS_MAX_FRAGS) { ws_close_send(fd, 1009, m); return 0; }
    } else if (c->msg_op != 0) {                 /* fresh data mid-message */
        ws_close_send(fd, 1002, m); return 0;
    }
    if (c->h.opcode < 0x8 &&
        (c->h.len > (uint64_t)Q_WS_MAX_MSG ||
         c->msg_len + c->h.len > (uint64_t)Q_WS_MAX_MSG)) {
        ws_close_send(fd, 1009, m); return 0;
    }

    if (c->h.len == 0)                           /* no payload to wait for */
        return ws_process(c, fd, NULL, 0);
    c->st   = WS_PAYLOAD;
    c->want = (int64_t)c->h.len;
    return c->want;
}

/* ---- outbound: the neg[h] framed-write arm ---- */

int q_ws_send(q_ws_conn_t* c, ray_sock_t fd, ray_t* msg) {
    int mask = c && c->role == Q_WS_CLIENT;      /* client masks; server plain */
    if (!msg || RAY_IS_NULL(msg)) return 0;      /* neg[h][] flush -> no-op */
    { const char* tp; int64_t tn;                /* charv OR legacy STR text */
      if (q_text_bytes(msg, &tp, &tn))
          return ws_frame_send(fd, Q_WS_OP_TEXT, tp, (size_t)tn,
                               Q_WS_DATA_SECS, mask); }
    if (msg->type == RAY_BYTE_ONLY)
        return ws_frame_send(fd, Q_WS_OP_BIN, ray_data(msg),
                             (size_t)msg->len, Q_WS_DATA_SECS, mask);
    return -2;
}

/* ---- handshake ---- */

static bool tok_ieq(const char* s, size_t n, const char* lower) {
    size_t tn = strlen(lower);
    if (n != tn) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch + 32);
        if (ch != (unsigned char)lower[i]) return false;
    }
    return true;
}

/* Comma-separated token-list membership, case-insens (RFC 7230 list syntax). */
static bool list_has_token(const char* v, size_t n, const char* lower) {
    size_t i = 0;
    while (i < n) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t j = i;
        while (j < n && v[j] != ',' && v[j] != ' ' && v[j] != '\t') j++;
        if (j > i && tok_ieq(v + i, j - i, lower)) return true;
        i = j;
    }
    return false;
}

/* First occurrence of a header (case-insens), or NULL (D14: first wins). */
static const struct phr_header* hdr_find(const struct phr_header* hdrs,
                                         size_t nh, const char* lower) {
    for (size_t i = 0; i < nh; i++)
        if (hdrs[i].name && tok_ieq(hdrs[i].name, hdrs[i].name_len, lower))
            return &hdrs[i];
    return NULL;
}

int q_ws_handshake(ray_sock_t fd, const struct phr_header* hdrs, size_t nh) {
    const struct phr_header* conn = hdr_find(hdrs, nh, "connection");
    const struct phr_header* key  = hdr_find(hdrs, nh, "sec-websocket-key");
    const struct phr_header* ver  = hdr_find(hdrs, nh, "sec-websocket-version");

    if (!ver || !tok_ieq(ver->value, ver->value_len, "13")) {
        static const char resp[] =
            "HTTP/1.1 426 Upgrade Required\r\nSec-WebSocket-Version: 13\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n";
        q_http_send_all(fd, resp, sizeof resp - 1, Q_WS_CTRL_SECS);
        return 0;
    }
    if (!conn || !list_has_token(conn->value, conn->value_len, "upgrade") ||
        !key || key->value_len == 0 || key->value_len > 64) {
        q_http_send_simple(fd, 400, "Bad Request");
        return 0;
    }

    /* accept = base64(SHA1(key+GUID)) — RFC §4.2.2, via the .Q primitives */
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char cat[64 + sizeof guid];
    memcpy(cat, key->value, key->value_len);
    memcpy(cat + key->value_len, guid, sizeof guid - 1);
    ray_t* s = ray_str(cat, key->value_len + sizeof guid - 1);
    if (!s || RAY_IS_ERR(s)) {
        if (s) ray_error_free(s);
        q_http_send_simple(fd, 500, "Internal Server Error");
        return 0;
    }
    ray_t* digest = q_dotq_sha1_fn(s);
    ray_release(s);
    if (!digest || RAY_IS_ERR(digest)) {
        if (digest) ray_error_free(digest);
        q_http_send_simple(fd, 500, "Internal Server Error");
        return 0;
    }
    ray_t* accept = q_dotq_btoa_fn(digest);
    ray_release(digest);
    if (!accept || RAY_IS_ERR(accept)) {
        if (accept) ray_error_free(accept);
        q_http_send_simple(fd, 500, "Internal Server Error");
        return 0;
    }

    char resp[256];
    int  rl = snprintf(resp, sizeof resp,
                       "HTTP/1.1 101 Switching Protocols\r\n"
                       "Connection: Upgrade\r\nUpgrade: websocket\r\n"
                       "Sec-WebSocket-Accept: %.*s\r\n\r\n",
                       (int)ray_str_len(accept), ray_str_ptr(accept));
    ray_release(accept);
    if (rl <= 0 || (size_t)rl >= sizeof resp ||
        q_http_send_all(fd, resp, (size_t)rl, Q_WS_CTRL_SECS) != 0)
        return 0;   /* peer gone before/while sending 101 — caller closes */
    return 1;
}
