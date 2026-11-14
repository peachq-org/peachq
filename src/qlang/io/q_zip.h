/* q_zip — a PKWARE zip archive (APPNOTE.TXT), read-only, over the byte reader, so over any transport.  The byte
 * core detects it (q_io.h) and hands over the archive's path, its stored size (< 0 = ask) and the entry's name,
 * matched exactly against the central directory (the first record of a repeated name wins).  An entry is stored or
 * DEFLATE; whatever reaches an entry's end has its size, compressed size and CRC-32 checked ('corrupt).  A missing
 * entry is 'domain, encryption, another method or a multi-disk archive 'nyi. */
#ifndef QLANG_Q_ZIP_H
#define QLANG_Q_ZIP_H
#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>
#include "qlang/io/q_io.h"

/* The entry's bytes forward from its start, and its uncompressed size. */
ray_t* q_zip_member(ray_t* arch, int64_t asize, const char* m, size_t mn, q_io_stream_t** s, int64_t* size);

/* OWNED list of the file entries' names (strings, a directory's left out), in central-directory order. */
ray_t* q_zip_names(ray_t* arch, int64_t asize);

void q_zip_register(void);                    /* `.pq.load_natives`zip`: .zip.i.info */

#endif /* QLANG_Q_ZIP_H */
