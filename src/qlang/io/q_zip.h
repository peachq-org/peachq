/* q_zip — the zip:// source: one entry of a PKWARE zip archive (APPNOTE.TXT), read-only, a peachq extension.
 * `zip://<archive>/<entry>`: the archive is the path through the first `.zip/` (any case), the entry the rest,
 * matched exactly against the central directory (the first record of a repeated name wins).  An entry is
 * stored or DEFLATE; whatever reaches an entry's end has its size, compressed size and CRC-32 checked
 * ('corrupt).  A missing archive is 'io, a missing entry 'domain, encryption, another method or a multi-disk
 * archive 'nyi. */
#ifndef QLANG_Q_ZIP_H
#define QLANG_Q_ZIP_H
#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>

int q_zip_is(const char* p, size_t n);

/* The byte core's zip arm: `want` bytes (-1 = to the end) from `off` of the entry, inflated from the start. */
ray_t* q_zip_read(ray_t* pathstr, int64_t off, int64_t want);

/* hcount: the entry's uncompressed size, from the central directory. */
ray_t* q_zip_size(ray_t* pathstr);

/* The forward stream behind `hopen`: the fd it reserves keys it; read gives the next `want` bytes (want < 0 = to
 * the end), the empty vector at the end, NULL for an fd that is not a zip stream; close answers 0 for one. */
ray_t* q_zip_handle_open(const char* path, size_t n, int64_t* fd);
ray_t* q_zip_handle_read(int64_t fd, int64_t want);
int    q_zip_handle_close(int64_t fd);
void   q_zip_forget(void);                    /* a forked child drops every stream, closing nothing */

void q_zip_register(void);                    /* `.pq.load_natives`zip`: .zip.i.info */

#endif /* QLANG_Q_ZIP_H */
