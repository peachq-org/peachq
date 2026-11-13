/* q_gz — the one DEFLATE seam: RFC 1952 gzip and the kxzip zlib stream, over
 * vendored zlib (third_party/zlib/README.peachq.md). */
#ifndef Q_GZ_H
#define Q_GZ_H

#include <stddef.h>
#include <stdint.h>

/* Decompression-bomb cap on inflate output (repo's 32 MiB precedent). */
#define Q_GZ_MAX_OUTPUT (32u * 1024u * 1024u)

/* Both return a malloc'd buffer the caller frees (sets *out_len), or NULL on
 * failure with *err pointing at a bare kdb error class:
 *   "type" | "domain" | "limit" | "wsfull".
 * level is 1..9. */
uint8_t* q_gz_deflate(const uint8_t* src, size_t src_len, int level,
                      size_t* out_len, const char** err);
uint8_t* q_gz_inflate(const uint8_t* src, size_t src_len,
                      size_t* out_len, const char** err);

/* Deflate src into ONE RFC 1950 zlib stream, kdb's kxzip block form, byte-equal
 * to kdb's at the same level.  malloc'd buffer or NULL with *err as above.
 * level 0..9. */
uint8_t* q_gz_deflate_zlib(const uint8_t* src, size_t src_len, int level,
                           size_t* out_len, const char** err);

/* Inflate ONE RFC 1950 zlib stream from src into out[0..out_cap), verifying
 * its adler32.  A zlib stream is self-terminating, so *consumed
 * reports the input bytes it used and anything after it is left untouched —
 * which is how the kxzip reader (io/q_io.c) walks its blocks.  Returns 0, or -1 with *err
 * a bare class as above. */
int q_gz_inflate_zlib(const uint8_t* src, size_t src_len,
                      uint8_t* out, size_t out_cap,
                      size_t* out_len, size_t* consumed, const char** err);

/* The same, with zlib's own state taken from `work` (Q_GZ_INFLATE_WORK bytes cover any stream) instead of the heap:
 * allocation-free, so the splay fault handler can inflate a block.  work NULL = the heap. */
#define Q_GZ_INFLATE_WORK ((size_t)64 << 10)
int q_gz_inflate_zlib_in(const uint8_t* src, size_t src_len,
                         uint8_t* out, size_t out_cap,
                         size_t* out_len, size_t* consumed, const char** err,
                         void* work, size_t work_len);

#endif /* Q_GZ_H */
