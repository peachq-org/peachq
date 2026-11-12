/* q_gz — the DEFLATE seam over vendored zlib (see q_gz.h). */
#include "qlang/q_count.h"
#include "qlang/net/q_gz.h"
#define ZLIB_CONST
#include "zlib.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* gzip FLG bits (RFC 1952 §2.3.1). */
#define GZ_FTEXT    0x01
#define GZ_FHCRC    0x02
#define GZ_FEXTRA   0x04
#define GZ_FNAME    0x08
#define GZ_FCOMMENT 0x10
#define GZ_FRESERVED 0xE0   /* bits 5-7 must be zero */

/* zlib counts in uInt (and uLong, 32 bits on Win64), so lengths are fed to it in
 * chunks of at most this and every count is kept here as a size_t. */
#define GZ_CHUNK ((size_t)UINT_MAX)

static uint32_t gz_rd32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void gz_wr32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uInt gz_take(size_t* left) {
    size_t k = *left < GZ_CHUNK ? *left : GZ_CHUNK;
    *left -= k;
    return (uInt)k;
}

/* One deflate stream into a malloc'd buffer with `head` bytes free before it and
 * `tail` after; *zlen is the stream's width.  memLevel 8 and the default strategy
 * are deflateInit's, so wbits 15 is compress2 — the call kdb's blocks reproduce.
 * The buffer starts at half the input and grows; deflate's bytes do not depend on
 * how its input or output is split. */
static uint8_t* gz_deflate(const uint8_t* src, size_t n, int level, int wbits,
                           size_t head, size_t tail, size_t* zlen, const char** err) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (deflateInit2(&zs, level, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        *err = "wsfull"; return NULL;
    }
    size_t room = n / 2 + 64, done = 0, in_left = n;
    uint8_t* out = (uint8_t*)malloc(head + room + tail);
    zs.next_in = src;
    int rc = Z_OK;
    while (out && rc == Z_OK) {
        if (done == room) {
            size_t grown = room + room / 2;
            uint8_t* bigger = grown > room ? (uint8_t*)realloc(out, head + grown + tail) : NULL;
            if (!bigger) { free(out); out = NULL; break; }
            out = bigger;
            room = grown;
        }
        if (zs.avail_in == 0) zs.avail_in = gz_take(&in_left);
        size_t free_out = room - done;
        zs.next_out = out + head + done;
        zs.avail_out = gz_take(&free_out);
        uInt before = zs.avail_out;
        rc = deflate(&zs, in_left ? Z_NO_FLUSH : Z_FINISH);
        done += before - zs.avail_out;
    }
    deflateEnd(&zs);
    if (!out || rc != Z_STREAM_END) { free(out); *err = "wsfull"; return NULL; }
    *zlen = done;
    return out;
}

/* One stream from src into out[0..cap): 0 once it ends cleanly (zlib checks the
 * adler32 of a wbits-15 stream), with *out_len produced and *used consumed.  The
 * loop ends because zlib answers Z_BUF_ERROR once neither side can move. */
static int gz_inflate(const uint8_t* src, size_t n, int wbits, uint8_t* out, size_t cap,
                      size_t* out_len, size_t* used, const char** err) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, wbits) != Z_OK) { *err = "wsfull"; return -1; }
    size_t in_left = n, out_left = cap;
    zs.next_in = src;
    zs.next_out = out;
    int rc;
    do {
        if (zs.avail_in == 0) zs.avail_in = gz_take(&in_left);
        if (zs.avail_out == 0) zs.avail_out = gz_take(&out_left);
        rc = inflate(&zs, Z_NO_FLUSH);
    } while (rc == Z_OK);
    *out_len = cap - out_left - zs.avail_out;
    *used = n - in_left - zs.avail_in;
    inflateEnd(&zs);
    if (rc == Z_STREAM_END) return 0;
    *err = rc == Z_MEM_ERROR ? "wsfull" : "domain";
    return -1;
}

uint8_t* q_gz_deflate(const uint8_t* src, size_t src_len, int level,
                      size_t* out_len, const char** err) {
    if (level < 1 || level > 9) { *err = "domain"; return NULL; }
    size_t defl_len = 0;
    uint8_t* out = gz_deflate(src, src_len, level, -15, 10, 8, &defl_len, err);
    if (!out) return NULL;
    /* 10-byte header: magic, CM=deflate, no flags, mtime=0, XFL=0, OS=255 — by hand,
     * because zlib's windowBits+16 mode writes the build platform's OS byte. */
    out[0] = 0x1f; out[1] = 0x8b; out[2] = 8; out[3] = 0;
    out[4] = out[5] = out[6] = out[7] = 0;   /* MTIME */
    out[8] = 0;                              /* XFL */
    out[9] = 0xff;                           /* OS = unknown */
    /* CRC-32 of the uncompressed data, then ISIZE (input length mod 2^32). */
    gz_wr32le(out + 10 + defl_len, (uint32_t)crc32_z(0L, src, src_len));
    gz_wr32le(out + 10 + defl_len + 4, (uint32_t)(src_len & 0xffffffffu));
    *out_len = 10 + defl_len + 8;
    *err = NULL;
    return out;
}

uint8_t* q_gz_inflate(const uint8_t* src, size_t src_len,
                      size_t* out_len, const char** err) {
    /* Single-member only (concatenated members out of scope): the exact-input-
     * consumption check below rejects any trailing/second-member bytes. */
    if (src_len < 18) { *err = "domain"; return NULL; }                 /* 10 hdr + 2 min + 8 ftr */
    if (src[0] != 0x1f || src[1] != 0x8b || src[2] != 8) { *err = "domain"; return NULL; }
    uint8_t flg = src[3];
    if (flg & GZ_FRESERVED) { *err = "domain"; return NULL; }
    const size_t foot = src_len - 8;   /* first footer byte offset */
    size_t pos = 10;
    if (flg & GZ_FEXTRA) {
        if (pos + 2 > foot) { *err = "domain"; return NULL; }
        size_t xlen = (size_t)src[pos] | ((size_t)src[pos + 1] << 8);
        pos += 2;
        if (xlen > foot - pos) { *err = "domain"; return NULL; }
        pos += xlen;
    }
    if (flg & GZ_FNAME) {
        while (pos < foot && src[pos] != 0) pos++;
        if (pos >= foot) { *err = "domain"; return NULL; }              /* no NUL */
        pos++;                                                          /* skip NUL */
    }
    if (flg & GZ_FCOMMENT) {
        while (pos < foot && src[pos] != 0) pos++;
        if (pos >= foot) { *err = "domain"; return NULL; }
        pos++;
    }
    if (flg & GZ_FHCRC) {
        if (pos + 2 > foot) { *err = "domain"; return NULL; }
        uint32_t hcrc = (uint32_t)crc32_z(0L, src, pos);
        if ((hcrc & 0xffff) != ((uint32_t)src[pos] | ((uint32_t)src[pos + 1] << 8))) {
            *err = "domain"; return NULL;
        }
        pos += 2;
    }
    if (pos > foot) { *err = "domain"; return NULL; }                   /* no deflate room */
    uint32_t crc_expect = gz_rd32le(src + foot);
    uint32_t isize = gz_rd32le(src + foot + 4);
    if (isize > Q_GZ_MAX_OUTPUT) { *err = "limit"; return NULL; }       /* bomb guard */
    uint8_t* out = (uint8_t*)malloc(isize ? isize : 1);                 /* malloc(0) guard */
    if (!out) { *err = "wsfull"; return NULL; }
    size_t out_sz = 0, used = 0;
    /* Require: clean finish, exact declared output, and full input consumption
     * (no junk / hidden second member). The fixed buffer bounds a lying ISIZE. */
    if (gz_inflate(src + pos, foot - pos, -15, out, isize, &out_sz, &used, err) != 0) {
        free(out); return NULL;
    }
    if (out_sz != isize || used != foot - pos) { free(out); *err = "domain"; return NULL; }
    if ((uint32_t)crc32_z(0L, out, out_sz) != crc_expect) {
        free(out); *err = "domain"; return NULL;
    }
    *out_len = out_sz;
    *err = NULL;
    return out;
}

int q_gz_inflate_zlib(const uint8_t* src, size_t src_len,
                      uint8_t* out, size_t out_cap,
                      size_t* out_len, size_t* consumed, const char** err) {
    if (!src || src_len < 2 || !out || out_cap == 0) { *err = "domain"; return -1; }
    if (gz_inflate(src, src_len, 15, out, out_cap, out_len, consumed, err) != 0) return -1;
    *err = NULL;
    return 0;
}

uint8_t* q_gz_deflate_zlib(const uint8_t* src, size_t src_len, int level,
                           size_t* out_len, const char** err) {
    if (level < 0 || level > 9) { *err = "domain"; return NULL; }
    uint8_t* p = gz_deflate(src, src_len, level, 15, 0, 0, out_len, err);
    if (p) *err = NULL;
    return p;
}
