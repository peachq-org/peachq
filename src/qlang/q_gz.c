/* q_gz — RFC 1952 gzip framing over miniz raw deflate/inflate (see q_gz.h). */
#include "qlang/q_gz.h"
#include "miniz.h"
#include <stdlib.h>
#include <string.h>

/* gzip FLG bits (RFC 1952 §2.3.1). */
#define GZ_FTEXT    0x01
#define GZ_FHCRC    0x02
#define GZ_FEXTRA   0x04
#define GZ_FNAME    0x08
#define GZ_FCOMMENT 0x10
#define GZ_FRESERVED 0xE0   /* bits 5-7 must be zero */

static uint32_t gz_rd32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void gz_wr32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

uint8_t* q_gz_deflate(const uint8_t* src, size_t src_len, int level,
                      size_t* out_len, const char** err) {
    if (level < 1 || level > 9) { *err = "domain"; return NULL; }
    mz_uint flags =
        tdefl_create_comp_flags_from_zip_params(level, -15, MZ_DEFAULT_STRATEGY);
    size_t defl_len = 0;
    /* raw deflate stream (no zlib header) on the heap. */
    void* defl = tdefl_compress_mem_to_heap(src, src_len, &defl_len, flags);
    if (!defl) { *err = "wsfull"; return NULL; }
    if (defl_len > SIZE_MAX - 18) { mz_free(defl); *err = "limit"; return NULL; }
    size_t total = 10 + defl_len + 8;
    uint8_t* out = (uint8_t*)malloc(total);
    if (!out) { mz_free(defl); *err = "wsfull"; return NULL; }
    /* 10-byte header: magic, CM=deflate, no flags, mtime=0, XFL=0, OS=255. */
    out[0] = 0x1f; out[1] = 0x8b; out[2] = 8; out[3] = 0;
    out[4] = out[5] = out[6] = out[7] = 0;   /* MTIME */
    out[8] = 0;                              /* XFL */
    out[9] = 0xff;                           /* OS = unknown */
    memcpy(out + 10, defl, defl_len);
    mz_free(defl);
    /* CRC-32 of the uncompressed data, then ISIZE (input length mod 2^32). */
    uint32_t crc = (uint32_t)mz_crc32(mz_crc32(0L, NULL, 0), src, src_len);
    gz_wr32le(out + 10 + defl_len, crc);
    gz_wr32le(out + 10 + defl_len + 4, (uint32_t)(src_len & 0xffffffffu));
    *out_len = total;
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
        uint32_t hcrc = (uint32_t)mz_crc32(mz_crc32(0L, NULL, 0), src, pos);
        if ((hcrc & 0xffff) != ((uint32_t)src[pos] | ((uint32_t)src[pos + 1] << 8))) {
            *err = "domain"; return NULL;
        }
        pos += 2;
    }
    if (pos > foot) { *err = "domain"; return NULL; }                   /* no deflate room */
    uint32_t crc_expect = gz_rd32le(src + foot);
    uint32_t isize = gz_rd32le(src + foot + 4);
    if (isize > Q_GZ_MAX_OUTPUT) { *err = "limit"; return NULL; }       /* bomb guard */
    size_t cap = isize ? isize : 1;                                     /* malloc(0) guard */
    uint8_t* out = (uint8_t*)malloc(cap);
    if (!out) { *err = "wsfull"; return NULL; }
    tinfl_decompressor d;
    tinfl_init(&d);
    size_t in_sz = foot - pos;                                          /* deflate payload */
    size_t out_sz = isize;
    tinfl_status st = tinfl_decompress(&d, src + pos, &in_sz, out, out, &out_sz,
                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    /* Require: clean finish, exact declared output, and full input consumption
     * (no junk / hidden second member). The fixed buffer bounds a lying ISIZE. */
    if (st != TINFL_STATUS_DONE || out_sz != isize || in_sz != foot - pos) {
        free(out); *err = "domain"; return NULL;
    }
    if ((uint32_t)mz_crc32(mz_crc32(0L, NULL, 0), out, out_sz) != crc_expect) {
        free(out); *err = "domain"; return NULL;
    }
    *out_len = out_sz;
    *err = NULL;
    return out;
}
