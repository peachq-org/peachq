/* q_io — THE byte core: what a path is, a file's size, its bytes, and how
 * bytes are written.  One clamp law, one mkdir law, one kxzip home. */
#ifndef Q_IO_H
#define Q_IO_H

#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>

/* Sym-or-string -> OWNED RAY_STR path; a SYM must carry the file symbol's ':'
 * (stripped), a string may.  NULL otherwise: 'type, or fall through.  A string
 * is taken as a RAW PATH, so a verb whose q surface is a file SYMBOL (hdel,
 * get, set) tests -RAY_SYM itself before asking. */
ray_t* q_io_file_path(ray_t* x);

/* Bytes on disk, or -1 when the path will not stat. */
int64_t q_io_file_size(ray_t* pathstr);

/* read0/read1/`0:`'s `(file;offset[;length])`.  NULL with *path OWNED and
 * *want -1 for the length-less form; else *path is released and an owned
 * 'type/'domain comes back.  `clamp` takes an out-of-range offset/length as a
 * request for what IS there — `0:` where the read verbs signal 'domain. */
ray_t* q_io_file_triple(ray_t* fsym, ray_t* offv, ray_t* wantv, int clamp,
                        ray_t** path, int64_t* off, int64_t* want);

/* OWNED RAY_BYTE_ONLY of `want` bytes from `off` (want < 0 = to EOF, both
 * clamped: a short read is not an error).  A kxzip container is resolved away
 * and `off` addresses the plaintext (ref/read1.md § Compression); *zipped,
 * when asked, says whether one was. */
ray_t* q_io_read_slice(ray_t* pathstr, int64_t off, int64_t want, int* zipped);

/* Create `path`'s missing parent directories — both file-creating doors promise
 * them (ref/file-text.md Save Text, ref/hopen.md).  q_io_write_all writes n
 * bytes over that law: NULL on success, else an owned 'access/'io. */
void q_io_mkdir_parents(const char* path, size_t n);
ray_t* q_io_write_all(ray_t* pathstr, const void* bytes, size_t n);

/* A kxzip container held whole in memory: NULL = none (buf IS the plaintext),
 * else owned plaintext bytes, or 'corrupt/'nyi.  q_io_zip_stats is `-21!x` —
 * an owned five-key dict, EMPTY when the file carries no container. */
ray_t* q_io_unzip(const uint8_t* buf, size_t len);
ray_t* q_io_zip_stats(ray_t* x);

#endif /* Q_IO_H */
