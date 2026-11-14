/* q_tar — a tar archive (POSIX ustar, with GNU long names and pax `path` records), read-only over the byte reader;
 * `gz` reads a `.tar.gz`/`.tgz` through the gzip stream.  A tar has no index, so a member is found by walking the
 * 512-byte headers from the start.  A header whose checksum fails, or data cut short, is 'corrupt; a missing
 * member 'domain. */
#ifndef QLANG_Q_TAR_H
#define QLANG_Q_TAR_H
#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>
#include "qlang/io/q_io.h"

/* The member's bytes forward from its start, and its size. */
ray_t* q_tar_member(ray_t* arch, int gz, const char* m, size_t mn, q_io_stream_t** s, int64_t* size);

/* OWNED list of the regular members' names (strings), in archive order. */
ray_t* q_tar_names(ray_t* arch, int gz);

#endif /* QLANG_Q_TAR_H */
