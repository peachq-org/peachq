/* q_wirefile — the kdb wire grammar as it is spelled ON DISK, beside q_wire.c
 * which speaks the same grammar over a socket.  Shape A is byte-for-byte the
 * -9! payload behind a two-byte header, so it routes through q_wire_read_obj:
 * one grammar, two transports.  The "kxzipped" container is NOT here — it wraps
 * bytes, not values, so io/q_io.h resolves it away before this file sees any.
 * Layout, provenance and the clean-room position: docs/2026-07-27-kdb-ondisk-format.md. */
#ifndef Q_WIREFILE_H
#define Q_WIREFILE_H

#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>

/* `get `:file` — read a kdb+ data file.  NULL when x is not a `:path symbol
 * atom, so the caller falls through.  Else an owned value or a RAY_ERROR:
 *   'io absent/unreadable/a directory opened as a file; 'type not a kdb+ data
 *   file or a folder without .d (ref/get.md); 'corrupt recognised magic with
 *   inconsistent lengths; 'nyi a layout a later stage reads. */
ray_t* q_wirefile_read(ray_t* x);

/* `` `:file set y `` — write a kdb+ data FLAT file.  NULL when x is not a
 * `:path symbol atom, so the caller falls through.  Else the owned `:path
 * handle (kdb returns nam) or a RAY_ERROR: 'nyi a trailing slash (the splayed
 * form) or a value no shape can carry, 'io the write itself.
 * Shape B for fixed-width simple vectors, shape A for everything else — kdb's
 * own choice, and what makes the write byte-identical to a file it wrote. */
ray_t* q_wirefile_write(ray_t* x, ray_t* y);

#endif /* Q_WIREFILE_H */
