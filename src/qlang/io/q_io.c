/* io/q_io.c — THE byte core (q_io.h) and the verbs sitting directly on it:
 * hsym, read0, read1, hdel.  Oracle: ref/read0.md, ref/read1.md, ref/hsym.md,
 * ref/hdel.md, kb/file-compression.md (CLEAN ROOM).  `0:` File Text builds on
 * this file from io/q_io_filetext.c; hopen/hclose live with the handle
 * registry (q_handles.c) and getenv/setenv with the process commands (q_sys.c).
 *
 * RAY_FN_RESTRICTED note: the base file primitives carry the flag on their ENV
 * fn objects; calling the C functions directly bypasses the eval-layer check,
 * so every file-touching arm re-asserts ray_eval_get_restricted(). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/io/q_io.h"
#include "qlang/q_registry_internal.h" /* q_str_split_lines, q_type_strict_i64 */
#include "qlang/base/q_err.h"
#include "qlang/io/q_handles.h" /* q_handles_read1 — the fifo-handle read form */
#include "qlang/net/q_gz.h"     /* q_gz_inflate_zlib — the kxzip block codec */
#include "lang/eval.h"      /* ray_eval_get_restricted */
#include "store/fileio.h"   /* ray_mkdir_p — the parent directories a write promises */
#include "table/sym.h"      /* ray_sym_intern_runtime, ray_sym_vec_cell */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ---- paths, size, write ------------------------------------------------- */

ray_t* q_io_file_path(ray_t* x) {
    const char* p;
    size_t n;
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);                       /* borrowed */
        if (!s) return NULL;
        p = ray_str_ptr(s);
        n = ray_str_len(s);
        if (n < 2 || p[0] != ':') return NULL;                /* not a file symbol */
    } else if (x && x->type == -RAY_STR) {
        p = ray_str_ptr(x);
        n = ray_str_len(x);
        if (!p) return NULL;
    } else {
        return NULL;
    }
    if (n && p[0] == ':') { p++; n--; }
    return ray_str(p, n);
}

int64_t q_io_file_size(ray_t* pathstr) {
    const char* p = ray_str_ptr(pathstr);
    if (!p) return -1;
#ifdef RAY_OS_WINDOWS
    struct _stat64 st;                  /* 64-bit twin: st_size past 2 GiB */
    return _stat64(p, &st) == 0 ? (int64_t)st.st_size : -1;
#else
    struct stat st;
    return stat(p, &st) == 0 ? (int64_t)st.st_size : -1;
#endif
}

void q_io_mkdir_parents(const char* path, size_t n) {
    while (n > 0 && path[n - 1] != '/') n--;
    if (n <= 1) return;                 /* no parent, or the root itself */
    char* dir = (char*)malloc(n);
    if (!dir) return;
    memcpy(dir, path, n - 1);
    dir[n - 1] = '\0';
    (void)ray_mkdir_p(dir);
    free(dir);
}

ray_t* q_io_write_all(ray_t* pathstr, const void* bytes, size_t n) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    const char* p = ray_str_ptr(pathstr);
    if (!p) return q_err(QE_DOMAIN);
    q_io_mkdir_parents(p, ray_str_len(pathstr));
    FILE* fp = fopen(p, "wb");
    if (!fp) return q_err(QE_IO);
    size_t w = n ? fwrite(bytes, 1, n, fp) : 0;
    fclose(fp);
    return w == n ? NULL : q_err(QE_IO);
}

/* ---- the reads ---------------------------------------------------------- */

#define ZIP_MAGIC     "kxzipped"
#define ZIP_MAGIC_LEN 8
#define ZIP_TRAIL_LEN 40
#define ZIP_ENTRY_LEN 8

/* An offset past EOF reads nothing, a length past EOF is a short read, and
 * want < 0 is to EOF — the one clamp every read here shares. */
static int64_t io_clamp(int64_t n, int64_t* off, int64_t want) {
    if (*off < 0) *off = 0;
    if (*off > n) *off = n;
    return (want < 0 || want > n - *off) ? n - *off : want;
}

/* Bytes exactly as they sit on disk.  One open answers the size and the
 * container magic too, so neither can change under the read (fstat on the open
 * fd, never a second stat). */
static ray_t* io_read_raw(ray_t* pathstr, int64_t off, int64_t want, int* wrapped) {
    if (wrapped) *wrapped = 0;
    const char* p = ray_str_ptr(pathstr);
    if (!p) return q_err(QE_DOMAIN);
    FILE* fp = fopen(p, "rb");
    if (!fp) return q_err(QE_IO);
    struct stat st;
    if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(fp);
        return q_err(QE_IO);
    }
    if (wrapped) {
        char magic[ZIP_MAGIC_LEN];      /* the magic alone: a truncated container
                                         * is still one, and must say 'corrupt */
        *wrapped = st.st_size >= ZIP_MAGIC_LEN &&
                   fread(magic, 1, ZIP_MAGIC_LEN, fp) == ZIP_MAGIC_LEN &&
                   memcmp(magic, ZIP_MAGIC, ZIP_MAGIC_LEN) == 0;
    }
    int64_t take = io_clamp((int64_t)st.st_size, &off, want);
    uint8_t* buf = take > 0 ? (uint8_t*)malloc((size_t)take) : NULL;
    size_t got = 0;
    ray_t* out;
    if (take > 0 && (!buf || fseek(fp, (long)off, SEEK_SET) != 0)) {
        out = q_err(buf ? QE_IO : QE_OOM);
    } else {
        if (buf) got = fread(buf, 1, (size_t)take, fp);
        out = ray_vec_from_raw(RAY_BYTE_ONLY, buf, (int64_t)got);
    }
    free(buf);
    fclose(fp);
    return out;
}

ray_t* q_io_read_slice(ray_t* pathstr, int64_t off, int64_t want, int* zipped) {
    if (zipped) *zipped = 0;
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    int wrapped = 0;
    ray_t* out = io_read_raw(pathstr, off, want, &wrapped);
    if (!wrapped || RAY_IS_ERR(out)) return out;
    /* A container cannot be inflated in part — its block index is not offsets —
     * so the whole file comes back and the offset addresses the plaintext. */
    ray_release(out);
    ray_t* all = io_read_raw(pathstr, 0, -1, NULL);
    if (RAY_IS_ERR(all)) return all;
    ray_t* plain = q_io_unzip((const uint8_t*)ray_data(all), (size_t)ray_len(all));
    ray_release(all);
    if (!plain) return q_err(QE_CORRUPT);       /* magic promised what unzip denies */
    if (RAY_IS_ERR(plain)) return plain;
    if (zipped) *zipped = 1;
    if (off == 0 && want < 0) return plain;
    int64_t take = io_clamp(ray_len(plain), &off, want);
    out = ray_vec_from_raw(RAY_BYTE_ONLY, (const uint8_t*)ray_data(plain) + off, take);
    ray_release(plain);
    return out;
}

ray_t* q_io_file_triple(ray_t* fsym, ray_t* offv, ray_t* wantv, int clamp,
                        ray_t** path, int64_t* off, int64_t* want) {
    *path = q_io_file_path(fsym);
    if (!*path) return q_err(QE_TYPE);
    *want = -1;
    q_err_e e = QE_TYPE;
    if (q_type_strict_i64(offv, off) && (!wantv || q_type_strict_i64(wantv, want))) {
        if (clamp || (*off >= 0 && (!wantv || *want >= 0))) return NULL;
        e = QE_DOMAIN;
    }
    ray_release(*path);
    *path = NULL;
    return q_err(e);
}

/* ---- the kxzip container (kb/file-compression.md) ----------------------- */

enum { ZIP_NONE = 0, ZIP_IPC = 1, ZIP_GZIP = 2, ZIP_SNAPPY = 3, ZIP_ENCRYPTED = 16 };

/* kb/file-compression.md: a power of 2 between 12 and 20. */
#define ZIP_LOG2_MIN 12
#define ZIP_LOG2_MAX 20

typedef struct {
    int64_t uncompressed;   /* plaintext bytes */
    int64_t block_size;     /* logicalBlockSize in BYTES; -21! reports its log2 */
    int64_t num_blocks;
    int32_t compressed;     /* block bytes, excluding magic, index and trailer */
    int32_t algorithm;
    int32_t level;
} zip_hdr_t;

static int64_t io_i64(const uint8_t* p) { int64_t v; memcpy(&v, p, 8); return v; }
static int32_t io_i32(const uint8_t* p) { int32_t v; memcpy(&v, p, 4); return v; }

static int32_t zip_log2(int64_t v) {
    int32_t k = 0;
    while (v > 1) { v >>= 1; k++; }
    return k;
}

/* Decode the fixed 40-byte trailer at `t`.  `file_len` is the whole file size,
 * which is what closes the layout arithmetic and makes a truncation
 * detectable.  Returns NULL on success, else an owned 'corrupt. */
static ray_t* zip_trailer(const uint8_t* t, size_t file_len, zip_hdr_t* z) {
    z->uncompressed = io_i64(t);
    z->block_size   = io_i64(t + 16);
    z->compressed   = io_i32(t + 24);
    z->algorithm    = io_i32(t + 28);
    z->num_blocks   = io_i64(t + 32);
    z->level        = 0;
    size_t room = file_len - ZIP_MAGIC_LEN - ZIP_TRAIL_LEN;
    if (z->uncompressed < 0 || z->compressed < 0 || z->num_blocks < 0 ||
        (uint64_t)z->num_blocks > room / ZIP_ENTRY_LEN)
        return q_err(QE_CORRUPT);
    if ((size_t)z->compressed + (size_t)z->num_blocks * ZIP_ENTRY_LEN != room ||
        io_i32(t + 8) != ZIP_MAGIC_LEN + z->compressed)
        return q_err(QE_CORRUPT);
    return NULL;
}

ray_t* q_io_unzip(const uint8_t* buf, size_t len) {
    if (!buf || len < ZIP_MAGIC_LEN || memcmp(buf, ZIP_MAGIC, ZIP_MAGIC_LEN) != 0) return NULL;
    if (len < ZIP_MAGIC_LEN + ZIP_TRAIL_LEN) return q_err(QE_CORRUPT);
    zip_hdr_t z;
    ray_t* bad = zip_trailer(buf + len - ZIP_TRAIL_LEN, len, &z);
    if (bad) return bad;
    if (z.algorithm != ZIP_NONE && z.algorithm != ZIP_GZIP) return q_err(QE_NYI);
    int32_t log2bs = zip_log2(z.block_size);
    if (z.block_size != (int64_t)1 << log2bs ||
        log2bs < ZIP_LOG2_MIN || log2bs > ZIP_LOG2_MAX)
        return q_err(QE_CORRUPT);
    /* Each block inflates to at most logicalBlockSize, so the trailer bounds its
     * own uncompressedLength: a lying header cannot drive the allocation.  The
     * ceiling is spelled out rather than (a+b-1)/b, which would overflow. */
    int64_t need = z.uncompressed / z.block_size + (z.uncompressed % z.block_size ? 1 : 0);
    if (need > z.num_blocks) return q_err(QE_CORRUPT);

    const size_t end = ZIP_MAGIC_LEN + (size_t)z.compressed;
    /* Algorithm 0 is the wrapper kdb still writes when compression did not pay;
     * no corpus sample, so accept it only when the two lengths agree exactly. */
    if (z.algorithm == ZIP_NONE) {
        if ((int64_t)z.compressed != z.uncompressed) return q_err(QE_CORRUPT);
        return ray_vec_from_raw(RAY_BYTE_ONLY, buf + ZIP_MAGIC_LEN, z.uncompressed);
    }

    uint8_t* dst = (uint8_t*)malloc(z.uncompressed > 0 ? (size_t)z.uncompressed : 1);
    if (!dst) return q_err(QE_OOM);
    size_t produced = 0, pos = ZIP_MAGIC_LEN;
    int64_t blocks = 0;
    while (produced < (size_t)z.uncompressed) {
        size_t got = 0, used = 0;
        const char* err = NULL;
        if (pos >= end ||
            q_gz_inflate_zlib(buf + pos, end - pos, dst + produced,
                              (size_t)z.uncompressed - produced,
                              &got, &used, &err) != 0 || used == 0 || got == 0) {
            free(dst);
            return q_err(QE_CORRUPT);
        }
        produced += got;
        pos += used;
        blocks++;
    }
    /* One zlib stream per block: fewer streams than numBlocks means the payload
     * and the trailer disagree, however plausible the plaintext looks. */
    ray_t* out = (produced == (size_t)z.uncompressed && pos == end &&
                  blocks == z.num_blocks)
                     ? ray_vec_from_raw(RAY_BYTE_ONLY, dst, z.uncompressed)
                     : q_err(QE_CORRUPT);
    free(dst);
    return out;
}

/* The index entries are NOT block offsets — they are byte-identical across
 * every corpus file regardless of size.  Their second i32 is level<<8|algorithm,
 * the only field -21! needs from the index. */
static int32_t zip_read_level(ray_t* path, int64_t file_len, const zip_hdr_t* z) {
    if (z->num_blocks <= 0) return 0;
    int64_t off = file_len - ZIP_TRAIL_LEN - z->num_blocks * ZIP_ENTRY_LEN;
    ray_t* e = io_read_raw(path, off + 4, 4, NULL);
    if (RAY_IS_ERR(e)) return 0;
    int32_t lvl = ray_len(e) == 4 ? (io_i32((const uint8_t*)ray_data(e)) >> 8) & 0xff : 0;
    ray_release(e);
    return lvl;
}

static ray_t* zip_stats_dict(const zip_hdr_t* z, int64_t file_len) {
    static const char* const names[] = { "compressedLength", "uncompressedLength",
                                         "algorithm", "logicalBlockSize", "zipLevel" };
    ray_t* vals[] = { ray_i64(file_len), ray_i64(z->uncompressed),
                      ray_i32(z->algorithm), ray_i32(zip_log2(z->block_size)),
                      ray_i32(z->level) };
    int64_t n = (int64_t)(sizeof names / sizeof names[0]);
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, n);
    ray_t* v = ray_list_new(n);
    for (int64_t i = 0; i < n; i++) {
        int64_t id = ray_sym_intern(names[i], strlen(names[i]));
        if (k && !RAY_IS_ERR(k)) k = ray_vec_append(k, &id);
        if (v && !RAY_IS_ERR(v) && vals[i]) v = ray_list_append(v, vals[i]);
        if (vals[i]) ray_release(vals[i]);
    }
    if (!k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v)) {
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
        return q_err(QE_OOM);
    }
    return ray_dict_new(k, v);
}

ray_t* q_io_zip_stats(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return q_err(QE_TYPE);
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    ray_t* path = q_io_file_path(x);
    if (!path) return q_err(QE_TYPE);
    int wrapped = 0;
    int64_t file_len = q_io_file_size(path);
    ray_t* tail = io_read_raw(path, file_len - ZIP_TRAIL_LEN, ZIP_TRAIL_LEN, &wrapped);
    ray_t* r = NULL;
    if (RAY_IS_ERR(tail)) r = tail;
    else if (!wrapped) r = ray_dict_new(ray_sym_vec_new(RAY_SYM_W64, 0), ray_list_new(0));
    else if (file_len < ZIP_MAGIC_LEN + ZIP_TRAIL_LEN ||
             ray_len(tail) != ZIP_TRAIL_LEN) r = q_err(QE_CORRUPT);
    if (!r) {
        zip_hdr_t z;
        r = zip_trailer((const uint8_t*)ray_data(tail), (size_t)file_len, &z);
        if (!r) {
            z.level = zip_read_level(path, file_len, &z);
            r = zip_stats_dict(&z, file_len);
        }
    }
    if (!RAY_IS_ERR(tail)) ray_release(tail);
    ray_release(path);
    return r;
}

/* ---- the verbs ---------------------------------------------------------- */

/* q `hsym x` — sym atom/vector -> file symbol: prefix ':' unless already
 * present (ref/hsym.md). */
static int64_t hsym_id(const char* p, size_t n) {
    if (n > 0 && p[0] == ':') return ray_sym_intern_runtime(p, n);
    char* buf = (char*)malloc(n + 1);
    if (!buf) return ray_sym_intern_runtime(":", 1);
    buf[0] = ':';
    memcpy(buf + 1, p, n);
    int64_t id = ray_sym_intern_runtime(buf, n + 1);
    free(buf);
    return id;
}
/* Exported (q_registry.h) so the `-1!` internal-fn alias single-homes here. */
static ray_t* hsym_wrap_impl(ray_t* x);
ray_t* q_hsym_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = hsym_wrap_impl(xs);
    ray_release(xs);
    return r;
}
static ray_t* hsym_wrap_impl(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);                   /* borrowed */
        if (!s) return q_err(QE_TYPE);
        const char* p = ray_str_ptr(s);
        size_t n = ray_str_len(s);
        if (n > 0 && p[0] == ':') { ray_retain(x); return x; }
        return ray_sym(hsym_id(p, n));
    }
    if (x && x->type == RAY_SYM) {
        int64_t n = ray_len(x);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        for (int64_t i = 0; i < n; i++) {
            ray_t* c = ray_sym_vec_cell(x, i);            /* borrowed domain atom */
            if (!c) { ray_release(out); return q_err(QE_TYPE); }
            int64_t id = hsym_id(ray_str_ptr(c), ray_str_len(c));
            out = ray_vec_append(out, &id);
            if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        }
        return out;
    }
    return q_err(QE_TYPE);
}

/* q `read1 x` — ref/read1.md.  File sym -> whole content as bytes; (f;o) ->
 * bytes from offset o to EOF; (f;o;n) -> up to n bytes from o (short read at
 * EOF, offsets clamped).  A kxzip container is resolved away by the byte core,
 * so what comes back is the uncompressed data the doc's Compression section
 * promises.  `read1(h;n)` on a fifo handle is q_handles_read1's; any other
 * handle is 'nyi. */
static ray_t* read1_wrap_impl(ray_t* x);
ray_t* q_read1_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = read1_wrap_impl(xs);
    ray_release(xs);
    return r;
}
static ray_t* read1_wrap_impl(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* path = q_io_file_path(x);
        if (!path) return q_err(QE_TYPE);
        ray_t* r = q_io_read_slice(path, 0, -1, NULL);
        ray_release(path);
        return r;
    }
    /* read1(handle;count): an all-int (h;count) pair collapses to a homogeneous
     * int VECTOR (not a RAY_LIST) — the fifo-handle streaming form `.Q.fpn` uses.
     * q_handles_read1 answers NULL unless fd is a registered fifo (a Phase-1
     * file handle is a write/append fd — read a file via `read1 `:path`). */
    if (x && q_type_is_int_vec(x) && ray_len(x) == 2) {
        ray_t* c = ray_i64(q_type_ivec_get(x, 1));
        ray_t* r = q_handles_read1(q_type_ivec_get(x, 0), c);
        ray_release(c);
        if (r) return r;
    }
    if (x && x->type == RAY_LIST && (ray_len(x) == 2 || ray_len(x) == 3)) {
        ray_t** e = (ray_t**)ray_data(x);
        int three = ray_len(x) == 3;
        if (e[0] && e[0]->type == -RAY_SYM) {
            ray_t* path;
            int64_t off, want;
            ray_t* bad = q_io_file_triple(e[0], e[1], three ? e[2] : NULL, 0, &path, &off, &want);
            if (bad) return bad;
            ray_t* r = q_io_read_slice(path, off, want, NULL);
            ray_release(path);
            return r;
        }
        if (!three && e[0] && q_type_is_int_atom(e[0])) {   /* read1(handle;count), fifo only */
            int64_t fd;
            if (!q_type_strict_i64(e[0], &fd)) return q_err(QE_TYPE);
            ray_t* r = q_handles_read1(fd, e[1]);
            if (r) return r;
        }
        return q_err(QE_NYI);
    }
    if (x && q_type_is_int_atom(x))
        return q_err(QE_NYI);
    return q_err(QE_TYPE);
}

/* q `read0 x` — ref/read0.md.  read0 IS read1's bytes decoded: a file symbol
 * splits into lines (LF/CRLF delimiters removed), `(f;o)` gives the chars to
 * EOF minus ONE trailing line break (the doc pins `read0(`:foo;6)` -> "world"
 * on a file ending \n), and `(f;o;n)`/`(fifo;n)` give exactly what was read.
 * Console (0) is deferred 'nyi.  Offsets accept 0 (superset of the doc). */
static ray_t* read0_wrap_impl(ray_t* x);
ray_t* q_read0_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = read0_wrap_impl(xs);
    ray_release(xs);
    return q_str_charv_out(r);              /* lines cross as char vectors */
}
static ray_t* read0_wrap_impl(ray_t* x) {
    ray_t* b = read1_wrap_impl(x);
    if (!b || RAY_IS_ERR(b)) return b;
    int64_t n = ray_len(b);
    const char* p = n ? (const char*)ray_data(b) : "";
    ray_t* r;
    if (x->type == -RAY_SYM) {
        r = q_str_split_lines(p, (size_t)n);
    } else {
        ray_t** e = x->type == RAY_LIST ? (ray_t**)ray_data(x) : NULL;
        if (e && ray_len(x) == 2 && e[0] && e[0]->type == -RAY_SYM) {   /* (f;o): to EOF */
            if (n && p[n - 1] == '\n') n--;
            if (n && p[n - 1] == '\r') n--;
        }
        r = ray_charv(p, n);
    }
    ray_release(b);
    return r;
}

/* q `hdel x` — ref/hdel.md.  Delete the file or (empty) folder named by the
 * file symbol `:path and return x.  POSIX remove() dispatches unlink/rmdir, so
 * a folder is removed only when empty (the doc's "folders only if empty").  A
 * missing path or non-empty folder surfaces 'io (the read0 ENOENT precedent).
 * WRITES the filesystem, so restricted mode refuses (the file-verb precedent). */
ray_t* q_hdel_wrap(ray_t* x) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    if (!x || x->type != -RAY_SYM) return q_err(QE_TYPE);  /* a DELETE takes only the
                                                            * file symbol it documents */
    ray_t* path = q_io_file_path(x);
    if (!path) return q_err(QE_TYPE);
    int rc = remove(ray_str_ptr(path));  /* ray_str path is NUL-terminated */
    ray_release(path);
    if (rc != 0) return q_err(QE_IO);
    ray_retain(x);
    return x;
}
