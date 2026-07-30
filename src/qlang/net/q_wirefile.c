/* q_wirefile — kdb+ on-disk reader (see q_wirefile.h). */
#include "qlang/net/q_wirefile.h"
#include "qlang/net/q_gz.h"
#include "qlang/net/q_wire.h"
#include "qlang/q_err.h"
#include "qlang/q_registry.h"   /* q_io_file_path, q_io_read_slice */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_concrete */
#include "lang/eval.h"          /* ray_eval_get_restricted, ray_write_file_fn */
#include "mem/heap.h"           /* RAY_ATTR_SORTED */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ZIP_MAGIC     "kxzipped"
#define ZIP_MAGIC_LEN 8
#define ZIP_TRAIL_LEN 40
#define ZIP_ENTRY_LEN 8

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

static int64_t wf_i64(const uint8_t* p) { int64_t v; memcpy(&v, p, 8); return v; }
static int32_t wf_i32(const uint8_t* p) { int32_t v; memcpy(&v, p, 4); return v; }

static int32_t zip_log2(int64_t v) {
    int32_t k = 0;
    while (v > 1) { v >>= 1; k++; }
    return k;
}

/* Decode the fixed 40-byte trailer at `t`.  `file_len` is the whole file size,
 * which is what closes the layout arithmetic and makes a truncation
 * detectable.  Returns NULL on success, else an owned 'corrupt. */
static ray_t* zip_trailer(const uint8_t* t, size_t file_len, zip_hdr_t* z) {
    z->uncompressed = wf_i64(t);
    z->block_size   = wf_i64(t + 16);
    z->compressed   = wf_i32(t + 24);
    z->algorithm    = wf_i32(t + 28);
    z->num_blocks   = wf_i64(t + 32);
    z->level        = 0;
    size_t room = file_len - ZIP_MAGIC_LEN - ZIP_TRAIL_LEN;
    if (z->uncompressed < 0 || z->compressed < 0 || z->num_blocks < 0 ||
        (uint64_t)z->num_blocks > room / ZIP_ENTRY_LEN)
        return q_err(QE_CORRUPT);
    if ((size_t)z->compressed + (size_t)z->num_blocks * ZIP_ENTRY_LEN != room ||
        wf_i32(t + 8) != ZIP_MAGIC_LEN + z->compressed)
        return q_err(QE_CORRUPT);
    return NULL;
}

static bool zip_is_wrapped(const uint8_t* buf, size_t len) {
    return buf && len >= ZIP_MAGIC_LEN && memcmp(buf, ZIP_MAGIC, ZIP_MAGIC_LEN) == 0;
}

ray_t* q_wirefile_unzip(const uint8_t* buf, size_t len) {
    if (!zip_is_wrapped(buf, len)) return NULL;
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

/* ---- the on-disk shapes ------------------------------------------------- */

#define WF_A_OFF   8   /* shape A: ff 01 + the -9! payload */
#define WF_B_OFF  16   /* shape B: fe 20 type attr + 4 pad + count(8) */
#define WF_C_MIN  16   /* shape C: fe + domain name; the count never lands before this */
#define WF_D_NAME 16   /* shape D: fd 20 + a 4096-byte page, domain name at +16 */
#define WF_D_DESC 4080 /* ...whose last 16 bytes are a shape B header */
#define WF_D_OFF  4096
#define WF_ENUM_TYPE 20  /* kdb's enum; openq has none, so it resolves away to 11h */
#define WF_DOMAIN_LEVELS 4  /* a column copied out of its database fails, never walks to / */
#define WF_NEST_BIAS 77  /* 77+n = mapped list of vectors of type n, elements in `<col>#` */
#define WF_NEST_HI   96

/* The first `n` bytes of `prefix` with `name` appended, as an owned RAY_STR —
 * NUL-terminated, so it is also the C path stat() and the readers want. */
static ray_t* wf_join(const char* prefix, size_t n, const char* name, size_t nn) {
    char* buf = (char*)malloc(n + nn + 1);
    if (!buf) return NULL;
    memcpy(buf, prefix, n);
    memcpy(buf + n, name, nn);
    buf[n + nn] = '\0';
    ray_t* s = ray_str(buf, n + nn);
    free(buf);
    return s;
}

static int wf_is_file(ray_t* path) {
    struct stat st;
    return stat(ray_str_ptr(path), &st) == 0 && S_ISREG(st.st_mode);
}

/* A `.d` entry and a shape C/D domain name are UNTRUSTED FILE CONTENT, so a
 * name is only ever a leaf: anything carrying a separator escapes the database
 * directory.  `\` is rejected alongside `/` on every platform — Windows takes
 * both, and the guard cannot depend on which host wrote the file. */
static int wf_leaf_name(const char* s, size_t n) {
    return n && !memchr(s, '/', n) && !memchr(s, '\\', n);
}

/* An ALLOWLIST, not a cast: openq added types kdb never writes (RAY_SEL 20,
 * RAY_STR 21) and RAY_SYM's width is adaptive, so raw disk bytes cannot carry
 * it.  Width stays ray_type_sizes' business — one source of truth. */
static int8_t wf_simple_tag(uint8_t disk) {
    switch (disk) {
    case RAY_BOOL: case RAY_GUID: case RAY_BYTE_ONLY: case RAY_I16:
    case RAY_I32: case RAY_I64: case RAY_F32: case RAY_F64: case RAY_CHARV:
    case RAY_TIMESTAMP: case RAY_MONTH: case RAY_DATE: case RAY_DATETIME:
    case RAY_TIMESPAN: case RAY_MINUTE: case RAY_SECOND: case RAY_TIME:
        return (int8_t)disk;
    default: return 0;
    }
}

/* kdb's disk attribute (0=none 1=s 2=u 3=p 4=g) COLLIDES with rayforce's attrs
 * bits — translate, never copy.  `u#/`p#/`g# drop undecoded (COL_DISK_ATTRS_MASK). */
static void wf_apply_attr(ray_t* v, uint8_t disk_attr) {
    if (v && !RAY_IS_ERR(v) && disk_attr == 1) v->attrs |= RAY_ATTR_SORTED;
}

static ray_t* wf_syms_to_eof(const uint8_t* p, size_t len) {
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, 0);
    if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_OOM);
    for (size_t i = 0; i < len; ) {
        const uint8_t* nul = (const uint8_t*)memchr(p + i, 0, len - i);
        if (!nul) { ray_release(v); return q_err(QE_CORRUPT); }
        int64_t id = ray_sym_intern_runtime((const char*)(p + i), (size_t)(nul - (p + i)));
        v = ray_vec_append(v, &id);
        if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_OOM);
        i = (size_t)(nul - p) + 1;
    }
    return v;
}

static ray_t* wf_read_a(const uint8_t* buf, size_t len) {
    if (len < WF_A_OFF) return q_err(QE_CORRUPT);
    /* The symbol count is advisory in BOTH directions (`.Q.en appends without
     * rewriting it: 10 declared for 16 names, 0 for 55), so type 11 scans
     * NUL-terminated names to EOF instead of reaching the wire decoder. */
    if (buf[2] == RAY_SYM) {
        ray_t* v = wf_syms_to_eof(buf + WF_A_OFF, len - WF_A_OFF);
        wf_apply_attr(v, buf[3]);
        return v;
    }
    size_t consumed = 0;
    ray_t* v = q_wire_read_obj(buf + 2, len - 2, &consumed, 0);
    if (v && !RAY_IS_ERR(v) && consumed != len - 2) { ray_release(v); return q_err(QE_CORRUPT); }
    return v;
}

/* `derive` marks a decompressed image: compression zeroes the file-level count,
 * leaving the plaintext length the only witness — and a true one only when
 * unattributed, since `u#/`p#/`g# append an index the length would eat.  -1 refuses. */
static int64_t wf_derive_count(size_t room, uint8_t esz, uint8_t attr) {
    if (attr > 1) return -1;
    return room % esz ? 0 : (int64_t)(room / esz);
}

/* The `#` companion carries no header at all — it is raw element bytes, and
 * its SIZE is what closes the parent's last offset. */
static ray_t* wf_companion(ray_t* path) {
    ray_t* cp = wf_join(ray_str_ptr(path), ray_str_len(path), "#", 1);
    if (!cp) return q_err(QE_OOM);
    ray_t* b = q_io_read_slice(cp, 0, -1);
    ray_release(cp);
    return b ? b : q_err(QE_IO);
}

/* N cumulative END offsets, element i spanning [off[i-1], off[i]).  Only 87
 * (char) is observed, and at width 1 an offset is indistinguishable from an
 * element index — every wider element type stays 'nyi rather than pick one. */
static ray_t* wf_read_nested(int8_t elem, ray_t* path, const uint8_t* off, int64_t count) {
    if (elem != RAY_CHARV) return q_err(QE_NYI);
    if (!path) return q_err(QE_TYPE);
    ray_t* out = ray_list_new(count);
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    ray_t* comp = wf_companion(path);
    if (RAY_IS_ERR(comp)) { ray_release(out); return comp; }
    const uint8_t* data = (const uint8_t*)ray_data(comp);
    int64_t size = ray_len(comp), prev = 0;
    for (int64_t i = 0; i < count; i++) {
        int64_t end = wf_i64(off + i * 8);
        if (end < prev || end > size) { ray_release(out); out = q_err(QE_CORRUPT); break; }
        ray_t* e = q_wire_fixed_vec(elem, data + prev, end - prev, 0);
        if (RAY_IS_ERR(e)) { ray_release(out); out = e; break; }
        out = ray_list_append(out, e);
        ray_release(e);
        if (!out || RAY_IS_ERR(out)) { out = out ? out : q_err(QE_OOM); break; }
        prev = end;
    }
    /* Every companion byte accounted for, empty column included — the size is
     * the only witness that the two files describe the same column. */
    if (!RAY_IS_ERR(out) && prev != size) { ray_release(out); out = q_err(QE_CORRUPT); }
    ray_release(comp);
    return out;
}

static ray_t* wf_read_b(const uint8_t* buf, size_t len, int derive, ray_t* path) {
    if (len < WF_B_OFF) return q_err(QE_CORRUPT);
    uint8_t disk = buf[2];
    /* The bias itself is anymap — an arena plus a `##` intern file, not this. */
    if (disk == WF_NEST_BIAS) return q_err(QE_NYI);
    int nested = disk > WF_NEST_BIAS && disk <= WF_NEST_HI;
    int8_t tag = wf_simple_tag(nested ? (uint8_t)(disk - WF_NEST_BIAS) : disk);
    if (!tag) return q_err(QE_TYPE);
    uint8_t esz = nested ? 8 : ray_type_sizes[(uint8_t)tag];   /* offsets, not elements */
    int64_t room = (int64_t)(len - WF_B_OFF) / esz;
    int64_t count = wf_i64(buf + 8);
    if (derive && count == 0) {
        count = wf_derive_count(len - WF_B_OFF, esz, buf[3]);
        if (count < 0) return q_err(QE_NYI);
    }
    if (count < 0 || count > room) return q_err(QE_CORRUPT);
    if (nested) return wf_read_nested(tag, path, buf + WF_B_OFF, count);
    ray_t* v = q_wire_fixed_vec(tag, buf + WF_B_OFF, count, 0);
    wf_apply_attr(v, buf[3]);
    return v;
}

/* `follow` = may this read resolve REFERENCES to other files (the enum domain,
 * the `#` companion)?  A referenced file's own read says no and reaches the
 * readers with a NULL path, which is what keeps the walk flat. */
static ray_t* wf_read_path(ray_t* path, int follow);

/* An enum names its domain, not its location.  Climb from the column's PARENT —
 * a table directory holds only columns, `quote/sym` among them. */
static ray_t* wf_domain(ray_t* path, const char* name) {
    size_t nn = strlen(name);
    if (!wf_leaf_name(name, nn)) return q_err(QE_CORRUPT);
    const char* p = ray_str_ptr(path);
    size_t n = ray_str_len(path);
    while (n && p[n - 1] != '/') n--;                  /* the column's own directory */
    for (int lvl = 0; lvl < WF_DOMAIN_LEVELS && n; lvl++) {
        n--;                                           /* climb one: the '/' ... */
        while (n && p[n - 1] != '/') n--;               /* ...and the name before it */
        if (!n) break;
        ray_t* cand = wf_join(p, n, name, nn);
        if (!cand) return q_err(QE_OOM);
        ray_t* d = wf_is_file(cand) ? wf_read_path(cand, 0) : NULL;
        ray_release(cand);
        if (!d) continue;
        if (!RAY_IS_ERR(d) && d->type != RAY_SYM) { ray_release(d); return q_err(QE_TYPE); }
        return d;
    }
    return q_err(QE_IO);
}

/* C and D agree from the count on: count(8) then `w`-byte indices.  Sorted-by-
 * domain-position is not sorted-by-symbol, so the attribute cannot survive. */
static ray_t* wf_read_enum(ray_t* path, const char* domain,
                           const uint8_t* p, size_t room, uint8_t w,
                           uint8_t attr, int derive) {
    int64_t count = wf_i64(p);
    if (derive && count == 0) {
        count = wf_derive_count(room, w, attr);
        if (count < 0) return q_err(QE_NYI);
    }
    if (count < 0 || (uint64_t)count > room / w) return q_err(QE_CORRUPT);
    if (!path) return q_err(QE_TYPE);
    ray_t* dom = wf_domain(path, domain);
    if (!dom || RAY_IS_ERR(dom)) return dom ? dom : q_err(QE_IO);
    int64_t dlen = ray_len(dom);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, count);
    const uint8_t* idx = p + 8;
    for (int64_t i = 0; out && !RAY_IS_ERR(out) && i < count; i++) {
        int64_t k = w == 4 ? wf_i32(idx + i * 4) : wf_i64(idx + i * 8);
        if (k < 0 || k >= dlen) { ray_release(out); out = q_err(QE_CORRUPT); break; }
        int64_t id = ray_vec_get_sym_id(dom, k);
        out = ray_vec_append(out, &id);
    }
    ray_release(dom);
    return out ? out : q_err(QE_OOM);
}

/* Two candidate count offsets: the terminator rounded up to the next 8-byte
 * boundary, and the format doc's single-sourced `16 + 8*((nul-3)/8)`.  They
 * disagree for domain names of 10 to 14 characters and no artifact holds one —
 * every name in the corpus is `sym`.  So try both and let the FILE decide: an
 * unattributed column's count accounts for the payload exactly, and 8 bytes of
 * separation put the two implied counts 2 apart, so at most one can fit.  A
 * `p#`/`g#` column's trailer fits neither, and errors rather than guesses. */
static ray_t* wf_read_c(const uint8_t* buf, size_t len, ray_t* path, int derive) {
    const uint8_t* nul = len > 1 ? (const uint8_t*)memchr(buf + 1, 0, len - 1) : NULL;
    if (!nul || nul == buf + 1) return q_err(QE_CORRUPT);
    int64_t idx = nul - buf;
    size_t rule = (size_t)((idx + 8) & ~(int64_t)7);
    if (rule < WF_C_MIN) rule = WF_C_MIN;
    const size_t cand[2] = { rule, (size_t)(WF_C_MIN + 8 * ((idx - 3) / 8)) };
    size_t coff = 0;
    int hits = 0;
    for (int k = 0; k < 2; k++) {
        size_t off = cand[k];
        if (k && off == cand[0]) continue;             /* one candidate, not two */
        if (len < 8 || off > len - 8) continue;        /* bounds first: no read yet */
        size_t room = len - off - 8;
        if (room % 4) continue;
        int64_t c = wf_i64(buf + off);
        /* A decompressed image's count is ZEROED (the file-level count is not
         * rewritten), so there the width alone is the whole test. */
        if (c == (int64_t)(room / 4) || (derive && c == 0)) { coff = off; hits++; }
    }
    if (hits != 1) return q_err(QE_CORRUPT);
    return wf_read_enum(path, (const char*)buf + 1,
                        buf + coff, len - coff - 8, 4, 0, derive);
}

static ray_t* wf_read_d(const uint8_t* buf, size_t len, ray_t* path, int derive) {
    if (len < WF_D_OFF) return q_err(QE_CORRUPT);
    const uint8_t* desc = buf + WF_D_DESC;
    if (desc[2] != WF_ENUM_TYPE) return wf_read_b(desc, len - WF_D_DESC, derive, path);
    const uint8_t* nul = (const uint8_t*)memchr(buf + WF_D_NAME, 0, WF_D_DESC - WF_D_NAME);
    if (!nul || nul == buf + WF_D_NAME) return q_err(QE_CORRUPT);
    return wf_read_enum(path, (const char*)buf + WF_D_NAME,
                        desc + 8, len - WF_D_OFF, 8, desc[3], derive);
}

/* The 2009 legacy ff 20 header and an fd page that is not 20 stay deferred. */
typedef enum { WF_UNKNOWN, WF_A, WF_B, WF_C, WF_D, WF_DEFER } wf_shape_t;

static wf_shape_t wf_sniff(const uint8_t* p, size_t n) {
    if (n < 2) return WF_UNKNOWN;
    if (p[0] == 0xff) return p[1] == 0x01 ? WF_A : WF_DEFER;
    if (p[0] == 0xfe) return p[1] == 0x20 ? WF_B : WF_C;
    if (p[0] == 0xfd) return p[1] == 0x20 ? WF_D : WF_DEFER;
    return WF_UNKNOWN;
}

static ray_t* wf_read_image(const uint8_t* buf, size_t len, int unzipped, ray_t* path) {
    switch (wf_sniff(buf, len)) {
    case WF_A:     return wf_read_a(buf, len);
    case WF_B:     return wf_read_b(buf, len, unzipped, path);
    case WF_C:     return wf_read_c(buf, len, path, unzipped);
    case WF_D:     return wf_read_d(buf, len, path, unzipped);
    case WF_DEFER: return q_err(QE_NYI);
    case WF_UNKNOWN: break;
    }
    return q_err(QE_TYPE);
}

/* `.d` is the manifest — it fixes both membership and column order, and a file
 * on disk it does not name is not part of the table.  Every column then goes
 * through the flat reader, so compression, enums and nesting come along free. */
static ray_t* wf_read_splay(const char* dir, size_t n, ray_t* dotd) {
    int64_t ncols = ray_len(dotd), rows = -1;
    ray_t* tbl = ray_table_new(ncols);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_OOM);
    ray_t* bad = NULL;
    for (int64_t i = 0; i < ncols; i++) {
        int64_t id = ray_vec_get_sym_id(dotd, i);
        ray_t* nm = ray_sym_str(id);                          /* borrowed */
        size_t cn = nm ? ray_str_len(nm) : 0;
        if (!nm || !wf_leaf_name(ray_str_ptr(nm), cn)) { bad = q_err(QE_CORRUPT); break; }
        ray_t* cp = wf_join(dir, n, ray_str_ptr(nm), cn);
        if (!cp) { bad = q_err(QE_OOM); break; }
        /* A name .d lists with no file behind it is a damaged table, not a
         * file someone asked for and missed. */
        ray_t* col = wf_is_file(cp) ? wf_read_path(cp, 1) : q_err(QE_CORRUPT);
        ray_release(cp);
        if (RAY_IS_ERR(col)) { bad = col; break; }
        if (rows < 0) rows = ray_len(col);
        /* No artifact settles what kdb does with ragged columns; failing beats
         * truncating to the shortest and returning a plausible short table. */
        if (ray_len(col) != rows) { ray_release(col); bad = q_err(QE_CORRUPT); break; }
        tbl = ray_table_add_col(tbl, id, col);
        ray_release(col);
        if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_OOM);
    }
    if (!bad) return tbl;
    ray_release(tbl);
    return bad;
}

/* Only a folder holding a .d is a kdb+ data folder (ref/get.md's `type rule). */
static ray_t* wf_read_folder(const char* dir, size_t n) {
    ray_t* dp = wf_join(dir, n, ".d", 2);
    if (!dp) return q_err(QE_OOM);
    ray_t* dotd = wf_is_file(dp) ? wf_read_path(dp, 0) : q_err(QE_TYPE);
    ray_release(dp);
    if (RAY_IS_ERR(dotd)) return dotd;
    ray_t* r = dotd->type == RAY_SYM ? wf_read_splay(dir, n, dotd) : q_err(QE_CORRUPT);
    ray_release(dotd);
    return r;
}

static ray_t* wf_read_path(ray_t* path, int follow) {
    const char* p = ray_str_ptr(path);
    size_t n = ray_str_len(path);
    struct stat st;
    if (stat(p, &st) != 0) return q_err(QE_IO);
    /* The trailing slash asks for the folder form; without it, an unopenable file. */
    if (S_ISDIR(st.st_mode))
        return n && p[n - 1] == '/' ? wf_read_folder(p, n) : q_err(QE_IO);
    if (!S_ISREG(st.st_mode)) return q_err(QE_IO);

    ray_t* head = q_io_read_slice(path, 0, ZIP_MAGIC_LEN);
    if (!head || RAY_IS_ERR(head)) return head ? head : q_err(QE_IO);
    const uint8_t* hp = (const uint8_t*)ray_data(head);
    size_t hn = (size_t)ray_len(head);
    int wrapped = zip_is_wrapped(hp, hn);
    int known = wrapped || wf_sniff(hp, hn) != WF_UNKNOWN;
    ray_release(head);
    /* Only the sniff is positional: a value spans its file, and a compressed one
     * cannot be inflated in part (the block index is not offsets). */
    if (!known) return q_err(QE_TYPE);

    ray_t* all = q_io_read_slice(path, 0, -1);
    if (!all || RAY_IS_ERR(all)) return all ? all : q_err(QE_IO);
    const uint8_t* buf = (const uint8_t*)ray_data(all);
    size_t len = (size_t)ray_len(all);
    ray_t* r;
    if (!wrapped) {
        r = wf_read_image(buf, len, 0, follow ? path : NULL);
    } else {
        ray_t* plain = q_wirefile_unzip(buf, len);
        if (!plain || RAY_IS_ERR(plain)) {
            r = plain ? plain : q_err(QE_CORRUPT);
        } else {
            r = wf_read_image((const uint8_t*)ray_data(plain), (size_t)ray_len(plain), 1,
                              follow ? path : NULL);
            ray_release(plain);
        }
    }
    ray_release(all);
    return r;
}

ray_t* q_wirefile_read(ray_t* x) {
    ray_t* path = q_io_file_path(x);
    if (!path) return NULL;
    ray_t* r = ray_eval_get_restricted() ? q_err(QE_ACCESS) : wf_read_path(path, 1);
    ray_release(path);
    return r;
}

/* ---- the writer --------------------------------------------------------- */

#define WF_A_MAGIC 2   /* the `ff 01` before the -8! payload */

/* Shape B header, then the elements verbatim.  The attribute byte is
 * TRANSLATED, never copied: rayforce's `attrs` packs the sym width, HAS_NULLS,
 * SLICE and the index flags into bits that collide with kdb's disk encoding
 * (0=none 1=s 2=u 3=p 4=g).  Only `s#` survives a rewrite — the other three
 * carry a side structure this writer does not build. */
static ray_t* wf_write_b(ray_t* x, uint8_t disk) {
    int64_t n = ray_len(x);
    size_t esz = ray_type_sizes[disk];
    if (n < 0 || (uint64_t)n > (SIZE_MAX - WF_B_OFF) / esz) return q_err(QE_LIMIT);
    size_t total = WF_B_OFF + (size_t)n * esz;
    uint8_t* buf = (uint8_t*)calloc(1, total);
    if (!buf) return q_err(QE_OOM);
    buf[0] = 0xfe;
    buf[1] = 0x20;
    buf[2] = disk;
    buf[3] = (x->attrs & RAY_ATTR_SORTED) ? 1 : 0;
    int64_t cnt = n;
    memcpy(buf + 8, &cnt, 8);
    if (n) memcpy(buf + WF_B_OFF, ray_data(x), (size_t)n * esz);
    ray_t* s = ray_str((const char*)buf, total);
    free(buf);
    return s ? s : q_err(QE_OOM);
}

/* `ff 01` then the -8! body — the same grammar q_wire.c puts on a socket. */
static ray_t* wf_write_a(ray_t* x) {
    q_wire_wbuf_t b = {0};
    if (q_wire_write_obj(&b, x)) {
        ray_t* e = b.err ? b.err : q_err(QE_TYPE);
        b.err = NULL;
        q_wire_wbuf_free(&b);
        return e;
    }
    uint8_t* buf = (uint8_t*)malloc(WF_A_MAGIC + b.len);
    if (!buf) { q_wire_wbuf_free(&b); return q_err(QE_OOM); }
    buf[0] = 0xff;
    buf[1] = 0x01;
    memcpy(buf + WF_A_MAGIC, b.p, b.len);
    ray_t* s = ray_str((const char*)buf, WF_A_MAGIC + b.len);
    free(buf);
    q_wire_wbuf_free(&b);
    return s ? s : q_err(QE_OOM);
}

/* Shape B is for FIXED-WIDTH SIMPLE VECTORS and nothing else, so the choice is
 * exactly wf_simple_tag's allowlist read backwards — one table, both
 * directions.  A tag it declines (RAY_SYM's adaptive width, RAY_SEL/RAY_STR
 * which kdb has no byte for, atoms, lists, dicts, tables) takes shape A. */
static ray_t* wf_write_image(ray_t* x) {
    uint8_t disk = x->type > 0 ? (uint8_t)wf_simple_tag((uint8_t)x->type) : 0;
    return disk ? wf_write_b(x, disk) : wf_write_a(x);
}

ray_t* q_wirefile_write(ray_t* x, ray_t* y) {
    if (!y) return q_err(QE_TYPE);
    ray_t* path = q_io_file_path(x);
    if (!path) return NULL;
    const char* p = ray_str_ptr(path);
    size_t n = ray_str_len(path);
    /* A trailing slash asks for the splayed form; `.Q.en`, compression and the
     * (file;lbs;alg;lvl) left-arguments are the same later wave. */
    if (ray_eval_get_restricted() || !n || p[n - 1] == '/') {
        ray_t* e = ray_eval_get_restricted() ? q_err(QE_ACCESS) : q_err(QE_NYI);
        ray_release(path);
        return e;
    }
    ray_retain(y);
    y = q_eval_apply_concrete(y);              /* storage boundary: no lazy on disk */
    ray_t* img = wf_write_image(y);
    ray_release(y);
    if (RAY_IS_ERR(img)) { ray_release(path); return img; }
    ray_t* r = ray_write_file_fn(path, img);
    ray_release(img);
    ray_release(path);
    if (!r) return q_err(QE_IO);
    if (RAY_IS_ERR(r)) return r;
    ray_release(r);
    ray_retain(x);
    return x;
}

/* ---- `-21!` ------------------------------------------------------------- */

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

/* The index entries are NOT block offsets — they are byte-identical across
 * every corpus file regardless of size.  Their second i32 is level<<8|algorithm,
 * the only field -21! needs from the index. */
static int32_t zip_read_level(ray_t* path, int64_t file_len, const zip_hdr_t* z) {
    if (z->num_blocks <= 0) return 0;
    int64_t off = file_len - ZIP_TRAIL_LEN - z->num_blocks * ZIP_ENTRY_LEN;
    ray_t* e = q_io_read_slice(path, off + 4, 4);
    if (!e || RAY_IS_ERR(e)) return 0;
    int32_t lvl = ray_len(e) == 4 ? (wf_i32((const uint8_t*)ray_data(e)) >> 8) & 0xff : 0;
    ray_release(e);
    return lvl;
}

ray_t* q_wirefile_stats(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return q_err(QE_TYPE);
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    ray_t* path = q_io_file_path(x);
    if (!path) return q_err(QE_TYPE);
    struct stat st;
    if (stat(ray_str_ptr(path), &st) != 0 || !S_ISREG(st.st_mode)) {
        ray_release(path);
        return q_err(QE_IO);
    }
    int64_t file_len = (int64_t)st.st_size;
    ray_t* head = q_io_read_slice(path, 0, ZIP_MAGIC_LEN);
    if (!head || RAY_IS_ERR(head)) { ray_release(path); return head ? head : q_err(QE_IO); }
    int wrapped = zip_is_wrapped((const uint8_t*)ray_data(head), (size_t)ray_len(head));
    ray_release(head);
    if (!wrapped) {
        ray_release(path);
        return ray_dict_new(ray_sym_vec_new(RAY_SYM_W64, 0), ray_list_new(0));
    }
    ray_t* r;
    if (file_len < ZIP_MAGIC_LEN + ZIP_TRAIL_LEN) {
        r = q_err(QE_CORRUPT);
    } else {
        ray_t* tail = q_io_read_slice(path, file_len - ZIP_TRAIL_LEN, ZIP_TRAIL_LEN);
        if (!tail || RAY_IS_ERR(tail)) r = tail ? tail : q_err(QE_IO);
        else if (ray_len(tail) != ZIP_TRAIL_LEN) { ray_release(tail); r = q_err(QE_CORRUPT); }
        else {
            zip_hdr_t z;
            ray_t* bad = zip_trailer((const uint8_t*)ray_data(tail), (size_t)file_len, &z);
            ray_release(tail);
            if (bad) r = bad;
            else {
                z.level = zip_read_level(path, file_len, &z);
                r = zip_stats_dict(&z, file_len);
            }
        }
    }
    ray_release(path);
    return r;
}
