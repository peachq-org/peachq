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

/* How big the file IS, or -1 when the path will not stat: a kxzip container
 * answers with its ORIGINAL file's length (ref/hcount.md), one that will not
 * decode with its own. */
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

/* Is x the file-symbol shape (a nonempty `:…` sym atom)?  The ONE home for
 * the name-vs-path spelling — `set`'s env half asks it too. */
int q_io_is_fsym(ray_t* x);

/* `set`'s file half — every path-shaped target (q_env.c's q_setg_wrap keeps
 * only name-vs-path).  Owns the on-disk-format classification: flat file,
 * the 4-item compression form, the (dir;sympath) domain overload; a
 * trailing-slash dir routes inside the writer.  Owned result. */
ray_t* q_io_set(ray_t* x, ray_t* y);

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

/* ---- container block access + write (PR #393 stage-1 dissection note) ---- */

typedef struct {
    int64_t  uncompressed;
    int64_t  block_size;
    int64_t  num_blocks;
    int64_t  payload;      /* compressed bytes, magic excluded */
    int32_t  algorithm;
    int32_t  level;
    int64_t* ends;         /* cumulative compressed END of each block; malloc'd */
} q_io_zipmap_t;

/* Parse a container's tail header + block boundaries into *zm (free the
 * ends with q_io_zipmap_free).  NULL on success; 'type when the file is no
 * container; 'corrupt/'nyi otherwise.  nb==1: the data area is the block;
 * nb>1: the per-block length prefixes are skip-chained, bounds-checked. */
ray_t* q_io_zip_open(ray_t* pathstr, q_io_zipmap_t* zm);
void   q_io_zipmap_free(q_io_zipmap_t* zm);

/* Inflate block k into dst.  The stream must terminate exactly at its recorded
 * end and yield exactly the block's plain size — anything else is 'corrupt. */
ray_t* q_io_zip_block(ray_t* pathstr, const q_io_zipmap_t* zm, int64_t k,
                      uint8_t* dst, size_t cap);

/* Write `img` as a container: 2^lbs blocks, each an independent zlib stream
 * (gzip alg 2 only — others 'nyi; a deflate that does not pay writes the
 * alg-0 wrapper).  NULL on success or an owned error. */
ray_t* q_io_zip_write(ray_t* pathstr, const uint8_t* img, size_t n,
                      int lbs, int alg, int lvl);

#endif /* Q_IO_H */
