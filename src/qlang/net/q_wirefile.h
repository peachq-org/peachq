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

/* `` `:file set y `` / `` `:dir/ set t `` — write a kdb+ data file.  NULL when
 * x is not a `:path symbol atom, so the caller falls through.  Else the owned
 * `:path handle (kdb returns nam) or a RAY_ERROR.  A flat write is shape B for
 * fixed-width simple vectors, shape A otherwise — kdb's own choice, and what
 * makes it byte-identical to a file kdb wrote.  A trailing slash splays: per-
 * column flat files, sym columns AUTO-ENUMERATED against `dom` (NULL = the
 * table dir's parent, `.Q.en`'s geography; naming it is the openq 2-item-`set`
 * API extension), nested char via the `#` companion, `.d` written last.
 * `.z.zd` (or the explicit zip triple, lbs >= 0) compresses data files; the
 * domain file NEVER compresses (KX's own concurrency retreat, kb page). */
ray_t* q_wirefile_write(ray_t* x, ray_t* y);
ray_t* q_wirefile_write_zip(ray_t* x, ray_t* y, int lbs, int alg, int lvl);
ray_t* q_wirefile_write_splay(ray_t* dirsym, ray_t* domsym, ray_t* y,
                              int lbs, int alg, int lvl);

/* The domain-append primitive `.Q.en` and the splay writer share: intern
 * symv's symbols into the domain FILE (append-only, new names only, positions
 * never rewritten, never compressed).  *positions (may be NULL) receives the
 * owned i64 domain positions of symv's cells.  NULL on success. */
ray_t* q_wirefile_domain_extend(ray_t* dompathstr, ray_t* symv, ray_t** positions);

/* `.Q.en[dom;t]` — the compat shim over domain_extend (body here, the format
 * home; ops/q_dotq.c only delegates).  Owned t back unchanged, or an error. */
ray_t* q_wirefile_en(ray_t* dom, ray_t* t);

/* ---- the splay authority's window on the format (io/q_splay.h) ---------- */

/* One column file's HEADER, no payload read.  `tag` is the element tag (0 when
 * only a decode can tell: a kxzip container, a shape-A non-vector); `count` is
 * the header count (-1 unknown); `mappable` marks a fixed-width uncompressed
 * simple vector whose payload starts at byte 16. */
typedef struct {
    int8_t  tag;
    uint8_t disk_attr;
    uint8_t nested;
    uint8_t is_enum;
    uint8_t zipped;
    uint8_t mappable;
    int64_t count;
    char    domain[256];   /* enum domain name, else "" */
} q_wf_colhdr;

/* Probe `pathstr` (owned by caller, RAY_STR).  NULL on success with *out
 * filled; else an owned error: 'io unreadable, 'type unrecognized magic or
 * tag, 'corrupt inconsistent lengths, 'nyi recognized-but-deferred layouts. */
ray_t* q_wirefile_probe(ray_t* pathstr, q_wf_colhdr* out);

/* Read ONE column file (enums/nesting/compression resolve away).  When
 * `domname`/`dom` name a preloaded enum domain, a column enumerated against
 * that name uses it instead of the directory walk-up. */
ray_t* q_wirefile_read_column(ray_t* pathstr, const char* domname, ray_t* dom);

/* The enum-domain walk-up from a column's directory (owned sym vector or
 * error) — the registry loads a splay's domain once through this. */
ray_t* q_wirefile_domain(ray_t* colpath, const char* name);

#endif /* Q_WIREFILE_H */
