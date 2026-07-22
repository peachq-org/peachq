/* ops/q_codec.c — the byte-codec verb bodies: md5 (-15!), .Q base64
 * (btoa/atob), SHA-1, and the .Q.c.gz seam wrapper.  Evicted from
 * q_builtins.c; the shared text-bytes probe lives here with its users. */
#include "qlang/q_builtins.h"   /* the codec decls (q_md5_fn, q_dotq_*) */
#include "qlang/q_registry.h"   /* q_text_bytes */
#include "qlang/net/q_gz.h"         /* q_gz_deflate/inflate — .Q.c.gz seam */
#include "lang/internal.h"      /* ray_error, ray_vec_from_raw */
#include <string.h>
#include <stdlib.h>

/* ---- md5 (RFC 1321, public spec — CLEAN ROOM) -----------------------------
 * q `md5 s` hashes the string s to a 16-byte digest (a RAY_BYTE_ONLY byte vector,
 * displayed as `0x…`).  Self-contained implementation of the published
 * algorithm; no external dependency, no kdb reference.  Reached ONLY through
 * the `!` internal-fn arm (q_bang.c -15!); the q spelling is `.q.md5:(-15!)`
 * in q.q — no env binding, no manifest row. */
static int codec_md5_compute(const uint8_t* msg, size_t len, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const uint32_t S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    /* padded length: message + 0x80 + zeros to 56 mod 64 + 8-byte bit length */
    size_t nblk = ((len + 8) / 64) + 1;
    uint8_t* buf = calloc(nblk, 64);
    if (!buf) return 0;                 /* OOM -> caller signals 'wsfull */
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[nblk * 64 - 8 + i] = (uint8_t)(bits >> (8 * i));
    for (size_t blk = 0; blk < nblk; blk++) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) {
            const uint8_t* q = buf + blk * 64 + i * 4;
            M[i] = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D);        g = i; }
            else if (i < 32) { F = (D & B) | (~D & C);        g = (5 * i + 1) & 15; }
            else if (i < 48) { F = B ^ C ^ D;                 g = (3 * i + 5) & 15; }
            else             { F = C ^ (B | ~D);              g = (7 * i) & 15; }
            F += A + K[i] + M[g];
            A = D; D = C; C = B;
            B += (F << S[i]) | (F >> (32 - S[i]));
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    free(buf);
    const uint32_t words[4] = { a0, b0, c0, d0 };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(words[i] >> (8 * j));
    return 1;
}
/* Exported (q_builtins.h) so the `-15!` internal-fn alias single-homes here. */
ray_t* q_md5_fn(ray_t* x) {
    const char* sp; int64_t sn;
    if (!q_text_bytes(x, &sp, &sn)) return ray_error("type", "md5: expects a string");
    uint8_t digest[16];
    if (!codec_md5_compute((const uint8_t*)sp, (size_t)sn, digest))
        return ray_error("wsfull", "md5: out of memory");
    return ray_vec_from_raw(RAY_BYTE_ONLY, digest, 16);
}

/* ---- .Q base64 + SHA-1 encoding primitives (ref/dotq.md; clean-room: RFC 4648
 * base64 + FIPS 180 SHA-1, both public standards, implemented from scratch —
 * zero-dependency, portable (fixed-width stdint, no POSIX calls)) ----------- */

static const char Q_B64_ALPHA[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Borrow the raw bytes of a string atom (-RAY_STR; use ray_str_len — SSO union
 * aliasing makes ray_len garbage on a string atom) or a byte vector (RAY_BYTE_ONLY).
 * Returns 0 for any other type. */
static int codec_bytes_of(ray_t* x, const uint8_t** p, size_t* n) {
    const char* tp; int64_t tn;
    if (!x) return 0;
    if (q_text_bytes(x, &tp, &tn)) { *p = (const uint8_t*)tp; *n = (size_t)tn; return 1; }
    if (x->type == RAY_BYTE_ONLY)  { *p = (const uint8_t*)ray_data(x); *n = (size_t)ray_len(x); return 1; }
    return 0;
}

/* base64-encode n bytes of src into a fresh malloc'd buffer; *outlen set.
 * Returns NULL on OOM (caller frees the result). */
static char* codec_b64_encode(const uint8_t* src, size_t n, size_t* outlen) {
    size_t olen = ((n + 2) / 3) * 4;
    char* out = (char*)malloc(olen ? olen : 1);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i+1] << 8) | src[i+2];
        out[o++] = Q_B64_ALPHA[(v >> 18) & 63];
        out[o++] = Q_B64_ALPHA[(v >> 12) & 63];
        out[o++] = Q_B64_ALPHA[(v >> 6) & 63];
        out[o++] = Q_B64_ALPHA[v & 63];
    }
    size_t rem = n - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)src[i] << 16;
        out[o++] = Q_B64_ALPHA[(v >> 18) & 63];
        out[o++] = Q_B64_ALPHA[(v >> 12) & 63];
        out[o++] = '='; out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i+1] << 8);
        out[o++] = Q_B64_ALPHA[(v >> 18) & 63];
        out[o++] = Q_B64_ALPHA[(v >> 12) & 63];
        out[o++] = Q_B64_ALPHA[(v >> 6) & 63];
        out[o++] = '=';
    }
    *outlen = o;
    return out;
}

/* base64-decode n chars of src into a fresh malloc'd byte buffer; *outlen set.
 * Returns NULL on OOM, and sets *bad=1 on malformed / incorrectly-padded input
 * (caller raises 'domain, per ref/dotq.md).  Skips ASCII whitespace; rejects any
 * non-alphabet char, data after padding, a non-multiple-of-4 group length, and
 * (RFC 4648 canonical form) non-zero residual pad bits. */
static uint8_t* codec_b64_decode(const char* src, size_t n, size_t* outlen, int* bad) {
    static int8_t rev[256];
    static int inited = 0;
    if (!inited) {
        for (int i = 0; i < 256; i++) rev[i] = -1;
        for (int i = 0; i < 64; i++) rev[(unsigned char)Q_B64_ALPHA[i]] = (int8_t)i;
        inited = 1;
    }
    *bad = 0;
    uint8_t* out = (uint8_t*)malloc((n / 4) * 3 + 3);
    if (!out) return NULL;
    size_t o = 0;
    uint32_t acc = 0;
    int nbits = 0, pad = 0, ngroup = 0;
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') { pad++; ngroup++; continue; }
        if (pad) { *bad = 1; break; }                  /* data after padding */
        int8_t d = rev[(unsigned char)c];
        if (d < 0) { *bad = 1; break; }                /* non-alphabet char */
        acc = (acc << 6) | (uint32_t)d;
        nbits += 6; ngroup++;
        if (nbits >= 8) { nbits -= 8; out[o++] = (uint8_t)((acc >> nbits) & 0xFF); }
    }
    /* canonical form: whole groups of 4, at most 2 pad chars, and no stray
     * (non-zero) leftover bits in the final partial group. */
    if (!*bad && (ngroup % 4 != 0 || pad > 2 || (nbits && (acc & ((1u << nbits) - 1)))))
        *bad = 1;
    if (*bad) { free(out); return NULL; }
    *outlen = o;
    return out;
}

static uint32_t codec_sha1_rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

/* SHA-1 (FIPS 180) of n bytes -> 20-byte big-endian digest.  Returns 1 on
 * success, 0 on OOM (caller raises 'wsfull — never a silent all-zero digest). */
static int codec_sha1_compute(const uint8_t* msg, size_t n, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;
    uint64_t ml = (uint64_t)n * 8;
    size_t total = n + 1;
    while (total % 64 != 56) total++;
    total += 8;
    uint8_t* buf = (uint8_t*)calloc(total, 1);
    if (!buf) return 0;
    memcpy(buf, msg, n);
    buf[n] = 0x80;
    for (int i = 0; i < 8; i++) buf[total - 1 - i] = (uint8_t)(ml >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const uint8_t* p = buf + off + i * 4;
            w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        }
        for (int i = 16; i < 80; i++)
            w[i] = codec_sha1_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t tmp = codec_sha1_rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = codec_sha1_rol(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(buf);
    const uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(hs[i] >> (24 - 8 * j));
    return 1;
}

/* (.Q.btoa x) — base64-encode a string or byte vector to a char string.
 * Exported (q_builtins.h) so the `-32!` internal-fn alias single-homes here. */
ray_t* q_dotq_btoa_fn(ray_t* x) {
    const uint8_t* p; size_t n;
    if (!codec_bytes_of(x, &p, &n))
        return ray_error("type", ".Q.btoa: expects a string or byte vector");
    size_t olen = 0;
    char* enc = codec_b64_encode(p, n, &olen);
    if (!enc) return ray_error("wsfull", ".Q.btoa: out of memory");
    ray_t* r = ray_str(enc, olen);
    free(enc);
    return r;
}

/* (.Q.atob x) — base64-decode a char/byte input to a byte vector (ref/dotq.md:
 * throws 'domain if the data is not correctly padded). */
ray_t* q_dotq_atob_fn(ray_t* x) {
    const uint8_t* p; size_t n;
    if (!codec_bytes_of(x, &p, &n))
        return ray_error("type", ".Q.atob: expects a string or byte vector");
    size_t olen = 0; int bad = 0;
    uint8_t* dec = codec_b64_decode((const char*)p, n, &olen, &bad);
    if (bad) return ray_error("domain", ".Q.atob: invalid base64 padding");
    if (!dec) return ray_error("wsfull", ".Q.atob: out of memory");
    ray_t* r = ray_vec_from_raw(RAY_BYTE_ONLY, dec, (int64_t)olen);
    free(dec);
    return r;
}

/* (.Q.sha1 x) — SHA-1 digest of a string (or byte vector) as a 20-byte
 * bytestream (ref/dotq.md).  Exported (see q_builtins.h) so the `-33!` bang
 * alias routes to this single-home C impl. */
ray_t* q_dotq_sha1_fn(ray_t* x) {
    const uint8_t* p; size_t n;
    if (!codec_bytes_of(x, &p, &n))
        return ray_error("type", ".Q.sha1: expects a string or byte vector");
    uint8_t digest[20];
    if (!codec_sha1_compute(p, n, digest))
        return ray_error("wsfull", ".Q.sha1: out of memory");
    return ray_vec_from_raw(RAY_BYTE_ONLY, digest, 20);
}

/* (.Q.c.gz x) — GZip (ref/dotq.md). x is one of:
 *   general null (::)  → 1b   (capability is embedded; owner-ratified divergence
 *                              from the doc's "whether Zlib is loaded")
 *   char/byte vector   → the INFLATED vector (string→string atom, bytes→byte vec)
 *   2-list (cl;cbv)    → the DEFLATED BYTE vector (gzip bytes contain NULs; a
 *                        pre-C3 string atom can't carry them), cl a long 1..9.
 * A VARY (not unary): the VM's op_call1 null gate hard-whitelists only nil/type,
 * so a unary here could never see the general null; the VARY apply path has no
 * such gate, so `.Q.gz[::]` reaches this fn as `::` and answers the capability.
 * Framing lives in q_gz.c; list/atom refs here are BORROWED (never released) and
 * the constructors copy before the malloc'd buffer is freed. */
ray_t* q_dotq_gz_fn(ray_t** args, int64_t argc) {
    if (argc != 1) return ray_error("rank", NULL);       /* VARY is a unary facade */
    ray_t* x = args[0];
    if (!x) return ray_error("type", NULL);
    if (RAY_IS_NULL(x)) return ray_bool(1);              /* .Q.gz[::] — capability */
    if (x->type == RAY_LIST && ray_len(x) == 2) {        /* deflate */
        ray_t* cl  = ray_list_get(x, 0);                 /* borrowed */
        ray_t* cbv = ray_list_get(x, 1);                 /* borrowed */
        if (!cl || cl->type != -RAY_I64) return ray_error("type", NULL);
        if (cl->i64 < 1 || cl->i64 > 9) return ray_error("domain", NULL);  /* before the int cast */
        const uint8_t* p; size_t n;
        if (!codec_bytes_of(cbv, &p, &n)) return ray_error("type", NULL);
        size_t olen = 0; const char* err = NULL;
        uint8_t* out = q_gz_deflate(p, n, (int)cl->i64, &olen, &err);
        if (!out) return ray_error(err ? err : "wsfull", NULL);
        ray_t* r = ray_vec_from_raw(RAY_BYTE_ONLY, out, (int64_t)olen);
        free(out);
        return r ? r : ray_error("wsfull", NULL);
    }
    const uint8_t* p; size_t n;                          /* inflate */
    if (!codec_bytes_of(x, &p, &n)) return ray_error("type", NULL);
    size_t olen = 0; const char* err = NULL;
    uint8_t* out = q_gz_inflate(p, n, &olen, &err);
    if (!out) return ray_error(err ? err : "domain", NULL);
    int textin = x->type == -RAY_STR || x->type == RAY_CHARV || x->type == -RAY_CHARV;
    ray_t* r = textin ? ray_charv((const char*)out, (int64_t)olen)
                      : ray_vec_from_raw(RAY_BYTE_ONLY, out, (int64_t)olen);
    free(out);
    return r ? r : ray_error("wsfull", NULL);
}
