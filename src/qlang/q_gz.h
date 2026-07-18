/* q_gz — RFC 1952 gzip framing over miniz raw deflate/inflate.
 *
 * miniz's zlib-compat layer has no gzip (windowBits+16) mode, so openq writes
 * the 10-byte gzip header, CRC-32, and ISIZE footer itself around miniz's raw
 * deflate stream.  This q_gz_deflate / q_gz_inflate pair is the seam a future
 * dlopen-system-zlib preference would swap inside — keep the interface clean. */
#ifndef Q_GZ_H
#define Q_GZ_H

#include <stddef.h>
#include <stdint.h>

/* Decompression-bomb cap on inflate output (repo's 32 MiB precedent). */
#define Q_GZ_MAX_OUTPUT (32u * 1024u * 1024u)

/* Both return a malloc'd buffer the caller frees (sets *out_len), or NULL on
 * failure with *err pointing at a bare kdb error class:
 *   "type" | "domain" | "limit" | "wsfull".
 * level is 1..9 (mapped 1:1 to miniz). */
uint8_t* q_gz_deflate(const uint8_t* src, size_t src_len, int level,
                      size_t* out_len, const char** err);
uint8_t* q_gz_inflate(const uint8_t* src, size_t src_len,
                      size_t* out_len, const char** err);

#endif /* Q_GZ_H */
