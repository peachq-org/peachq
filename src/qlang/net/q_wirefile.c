/* q_wirefile — kdb+ on-disk reader (see q_wirefile.h). */
#include "qlang/net/q_wirefile.h"
#include "qlang/io/q_io.h"      /* the byte core: paths, the slice read, the write */
#include "qlang/net/q_wire.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"  /* q_type_is_int_vec — the `.z.zd` triple */
#include "qlang/q_env.h"        /* q_env_get — `.z.zd` lives as a plain global */
#include "qlang/q_prim.h"       /* q_str_text_bytes — nested char elements */
#include "qlang/q_builtins.h"   /* q_count_long — nested column length */
#include "qlang/ops/q_index.h"  /* q_index_elem_at — nested char elements */
#include "qlang/io/q_splay.h"   /* q_splay_invalidate — an overwrite drops the entry */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_concrete */
#include "lang/eval.h"          /* ray_eval_get_restricted */
#include "mem/heap.h"           /* RAY_ATTR_SORTED */
#include <stdio.h>   /* fopen/fread — the header probe reads raw prefix bytes */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int64_t wf_i64(const uint8_t* p) { int64_t v; memcpy(&v, p, 8); return v; }
static int32_t wf_i32(const uint8_t* p) { int32_t v; memcpy(&v, p, 4); return v; }

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
    ray_t* b = q_io_read_slice(cp, 0, -1, NULL);
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

/* A preloaded enum domain (the splay registry's): a column enumerated against
 * `name` resolves on it instead of the directory walk-up. */
typedef struct { const char* name; ray_t* dom; } wf_dom_hint;

/* `follow` = may this read resolve REFERENCES to other files (the enum domain,
 * the `#` companion)?  A referenced file's own read says no and reaches the
 * readers with a NULL path, which is what keeps the walk flat. */
static ray_t* wf_read_path(ray_t* path, int follow, const wf_dom_hint* hint);

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
        ray_t* d = wf_is_file(cand) ? wf_read_path(cand, 0, NULL) : NULL;
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
                           uint8_t attr, int derive, const wf_dom_hint* hint) {
    int64_t count = wf_i64(p);
    if (derive && count == 0) {
        count = wf_derive_count(room, w, attr);
        if (count < 0) return q_err(QE_NYI);
    }
    if (count < 0 || (uint64_t)count > room / w) return q_err(QE_CORRUPT);
    if (!path) return q_err(QE_TYPE);
    ray_t* dom;
    if (hint && hint->dom && strcmp(domain, hint->name) == 0) {
        dom = hint->dom;
        ray_retain(dom);                       /* released below like a loaded one */
    } else
        dom = wf_domain(path, domain);
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
static int wf_c_count_off(const uint8_t* buf, size_t avail, size_t len,
                          int derive, size_t* coff) {
    const uint8_t* nul = avail > 1 ? (const uint8_t*)memchr(buf + 1, 0, avail - 1) : NULL;
    if (!nul || nul == buf + 1) return 0;
    int64_t idx = nul - buf;
    size_t rule = (size_t)((idx + 8) & ~(int64_t)7);
    if (rule < WF_C_MIN) rule = WF_C_MIN;
    const size_t cand[2] = { rule, (size_t)(WF_C_MIN + 8 * ((idx - 3) / 8)) };
    int hits = 0;
    for (int k = 0; k < 2; k++) {
        size_t off = cand[k];
        if (k && off == cand[0]) continue;             /* one candidate, not two */
        if (len < 8 || off > len - 8 || off + 8 > avail) continue;   /* bounds first */
        size_t room = len - off - 8;
        if (room % 4) continue;
        int64_t c = wf_i64(buf + off);
        /* A decompressed image's count is ZEROED (the file-level count is not
         * rewritten), so there the width alone is the whole test. */
        if (c == (int64_t)(room / 4) || (derive && c == 0)) { *coff = off; hits++; }
    }
    return hits == 1;
}

static ray_t* wf_read_c(const uint8_t* buf, size_t len, ray_t* path, int derive,
                        const wf_dom_hint* hint) {
    size_t coff = 0;
    if (!wf_c_count_off(buf, len, len, derive, &coff)) return q_err(QE_CORRUPT);
    return wf_read_enum(path, (const char*)buf + 1,
                        buf + coff, len - coff - 8, 4, 0, derive, hint);
}

static ray_t* wf_read_d(const uint8_t* buf, size_t len, ray_t* path, int derive,
                        const wf_dom_hint* hint) {
    if (len < WF_D_OFF) return q_err(QE_CORRUPT);
    const uint8_t* desc = buf + WF_D_DESC;
    if (desc[2] != WF_ENUM_TYPE) return wf_read_b(desc, len - WF_D_DESC, derive, path);
    const uint8_t* nul = (const uint8_t*)memchr(buf + WF_D_NAME, 0, WF_D_DESC - WF_D_NAME);
    if (!nul || nul == buf + WF_D_NAME) return q_err(QE_CORRUPT);
    return wf_read_enum(path, (const char*)buf + WF_D_NAME,
                        desc + 8, len - WF_D_OFF, 8, desc[3], derive, hint);
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

static ray_t* wf_read_image(const uint8_t* buf, size_t len, int unzipped, ray_t* path,
                            const wf_dom_hint* hint) {
    switch (wf_sniff(buf, len)) {
    case WF_A:     return wf_read_a(buf, len);
    case WF_B:     return wf_read_b(buf, len, unzipped, path);
    case WF_C:     return wf_read_c(buf, len, path, unzipped, hint);
    case WF_D:     return wf_read_d(buf, len, path, unzipped, hint);
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
        ray_t* col = wf_is_file(cp) ? wf_read_path(cp, 1, NULL) : q_err(QE_CORRUPT);
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
    ray_t* dotd = wf_is_file(dp) ? wf_read_path(dp, 0, NULL) : q_err(QE_TYPE);
    ray_release(dp);
    if (RAY_IS_ERR(dotd)) return dotd;
    ray_t* r = dotd->type == RAY_SYM ? wf_read_splay(dir, n, dotd) : q_err(QE_CORRUPT);
    ray_release(dotd);
    return r;
}

static ray_t* wf_read_path(ray_t* path, int follow, const wf_dom_hint* hint) {
    const char* p = ray_str_ptr(path);
    size_t n = ray_str_len(path);
    struct stat st;
    if (stat(p, &st) != 0) return q_err(QE_IO);
    /* The trailing slash asks for the folder form; without it, an unopenable file. */
    if (S_ISDIR(st.st_mode))
        return n && p[n - 1] == '/' ? wf_read_folder(p, n) : q_err(QE_IO);
    if (!S_ISREG(st.st_mode)) return q_err(QE_IO);

    /* A value spans its whole file, so the read is never partial; the byte core
     * resolves a kxzip container away and reports that it did, which is the one
     * thing the image cannot say for itself (compression zeroes its count). */
    int zipped = 0;
    ray_t* all = q_io_read_slice(path, 0, -1, &zipped);
    if (!all || RAY_IS_ERR(all)) return all ? all : q_err(QE_IO);
    ray_t* r = wf_read_image((const uint8_t*)ray_data(all), (size_t)ray_len(all),
                             zipped, follow ? path : NULL, hint);
    ray_release(all);
    return r;
}

ray_t* q_wirefile_read(ray_t* x) {
    ray_t* path = q_io_file_path(x);
    if (!path) return NULL;
    ray_t* r = ray_eval_get_restricted() ? q_err(QE_ACCESS) : wf_read_path(path, 1, NULL);
    ray_release(path);
    return r;
}

ray_t* q_wirefile_read_column(ray_t* pathstr, const char* domname, ray_t* dom) {
    wf_dom_hint h = { domname, dom };
    return wf_read_path(pathstr, 1, (domname && dom) ? &h : NULL);
}

ray_t* q_wirefile_domain(ray_t* colpath, const char* name) {
    return wf_domain(colpath, name);
}

/* ---- the header probe (no payload read) --------------------------------- */

/* Shape B / embedded-B tag classification shared by the probe's two arms. */
static ray_t* wf_probe_b(const uint8_t* hdr, q_wf_colhdr* out) {
    uint8_t disk = hdr[2];
    if (disk == WF_NEST_BIAS) return q_err(QE_NYI);            /* anymap */
    int nested = disk > WF_NEST_BIAS && disk <= WF_NEST_HI;
    int8_t tag = wf_simple_tag(nested ? (uint8_t)(disk - WF_NEST_BIAS) : disk);
    if (!tag) return q_err(QE_TYPE);
    if (nested && tag != RAY_CHARV) return q_err(QE_NYI);      /* wf_read_nested's law */
    out->tag = tag;
    out->nested = (uint8_t)nested;
    out->disk_attr = hdr[3];
    out->count = wf_i64(hdr + 8);
    return NULL;
}

/* Classify header bytes (raw file prefix, or a container's inflated block 0
 * with fsz = the plain length and derive set — counts are zeroed there). */
static ray_t* wf_probe_classify(const uint8_t* buf, size_t got, size_t fsz,
                                int derive, q_wf_colhdr* out) {
    switch (wf_sniff(buf, got)) {
    case WF_A:                       /* full serialized value; count advisory */
        out->tag = wf_simple_tag(buf[2]) ? (int8_t)buf[2]
                 : buf[2] == RAY_SYM ? (int8_t)RAY_SYM : 0;
        return NULL;
    case WF_B: {
        if (got < WF_B_OFF || fsz < WF_B_OFF) return q_err(QE_CORRUPT);
        ray_t* e = wf_probe_b(buf, out);
        if (e) return e;
        uint8_t esz = out->nested ? 8 : ray_type_sizes[(uint8_t)out->tag];
        if (derive && out->count == 0) {
            out->count = wf_derive_count(fsz - WF_B_OFF, esz, out->disk_attr);
            if (out->count < 0) return q_err(QE_NYI);
        }
        if (out->count < 0 ||
            (uint64_t)out->count > (fsz - WF_B_OFF) / esz) return q_err(QE_CORRUPT);
        out->mappable = !out->nested;
        return NULL;
    }
    case WF_C: {
        size_t coff = 0;
        if (!wf_c_count_off(buf, got, fsz, derive, &coff)) return q_err(QE_CORRUPT);
        size_t nn = strlen((const char*)buf + 1);
        if (nn >= sizeof out->domain) return q_err(QE_CORRUPT);
        memcpy(out->domain, buf + 1, nn);
        out->is_enum = 1;
        out->tag = RAY_SYM;
        out->count = wf_i64(buf + coff);
        if (derive && out->count == 0) {
            out->count = wf_derive_count(fsz - coff - 8, 4, 0);
            if (out->count < 0) return q_err(QE_NYI);
        }
        return NULL;
    }
    case WF_D: {
        if (fsz < WF_D_OFF || got < WF_D_OFF) return q_err(QE_CORRUPT);
        const uint8_t* desc = buf + WF_D_DESC;
        if (desc[2] != WF_ENUM_TYPE) {
            ray_t* e = wf_probe_b(desc, out);           /* B header on a D page */
            if (e) return e;
            if (derive && out->count == 0 && !out->nested) {
                uint8_t esz = ray_type_sizes[(uint8_t)out->tag];
                out->count = wf_derive_count(fsz - WF_D_OFF, esz, out->disk_attr);
                if (out->count < 0) return q_err(QE_NYI);
            }
            return NULL;
        }
        const uint8_t* nul = (const uint8_t*)memchr(buf + WF_D_NAME, 0,
                                                    WF_D_DESC - WF_D_NAME);
        if (!nul || nul == buf + WF_D_NAME) return q_err(QE_CORRUPT);
        size_t nn = (size_t)(nul - (buf + WF_D_NAME));
        if (nn >= sizeof out->domain) return q_err(QE_CORRUPT);
        memcpy(out->domain, buf + WF_D_NAME, nn);
        out->is_enum = 1;
        out->tag = RAY_SYM;
        out->count = wf_i64(desc + 8);
        if (derive && out->count == 0) {
            out->count = wf_derive_count(fsz - WF_D_OFF, 8, desc[3]);
            if (out->count < 0) return q_err(QE_NYI);
        }
        if (out->count < 0 ||
            (uint64_t)out->count > (fsz - WF_D_OFF) / 8) return q_err(QE_CORRUPT);
        return NULL;
    }
    case WF_DEFER: return q_err(QE_NYI);
    case WF_UNKNOWN: break;
    }
    return q_err(QE_TYPE);
}

ray_t* q_wirefile_probe(ray_t* pathstr, q_wf_colhdr* out) {
    memset(out, 0, sizeof *out);
    out->count = -1;
    const char* p = ray_str_ptr(pathstr);
    struct stat st;
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) return q_err(QE_CORRUPT);
    size_t fsz = (size_t)st.st_size;
    uint8_t buf[WF_D_OFF];
    FILE* fp = fopen(p, "rb");
    if (!fp) return q_err(QE_IO);
    size_t got = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    if (got < 2) return q_err(QE_CORRUPT);
    if (got >= 8 && memcmp(buf, "kxzipped", 8) == 0) {
        /* container: inflate BLOCK 0 alone — the inner header (its zeroed
         * counts derived back from the plain length) costs one block */
        out->zipped = 1;
        q_io_zipmap_t zm;
        ray_t* e = q_io_zip_open(pathstr, &zm);
        if (e) return e;
        size_t b0 = (size_t)(zm.uncompressed < zm.block_size ? zm.uncompressed
                                                             : zm.block_size);
        uint8_t* plain = (uint8_t*)malloc(b0 ? b0 : 1);
        if (!plain) { q_io_zipmap_free(&zm); return q_err(QE_OOM); }
        e = q_io_zip_block(pathstr, &zm, 0, plain, b0);
        if (!e)
            e = wf_probe_classify(plain, b0, (size_t)zm.uncompressed, 1, out);
        free(plain);
        q_io_zipmap_free(&zm);
        return e;
    }
    return wf_probe_classify(buf, got, fsz, 0, out);
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

/* `.z.zd` (ref/dotz.md): the (lbs;alg;lvl) default `set` applies when no
 * explicit triple names one.  1 when set and 3 int cells, else 0. */
static int wf_zd(int* lbs, int* alg, int* lvl) {
    ray_t* zd = q_env_get(ray_sym_intern_runtime(".z.zd", 5));   /* borrowed */
    if (!zd || !q_type_is_int_vec(zd) || ray_len(zd) != 3) return 0;
    *lbs = (int)q_type_ivec_get(zd, 0);
    *alg = (int)q_type_ivec_get(zd, 1);
    *lvl = (int)q_type_ivec_get(zd, 2);
    return *alg != 0;
}

/* One data file: the image bytes, plain or through the container.  `hdr` says
 * img carries a file-level count field — compression ZEROES it (the kdbfile
 * trap rows pin this; the reader derives it back from the plain length), so a
 * headerless companion passes 0 and stays untouched. */
static ray_t* wf_put(ray_t* path, ray_t* img, int lbs, int alg, int lvl, int hdr) {
    if (alg > 0) {
        uint8_t* p = (uint8_t*)ray_str_ptr(img);     /* fresh image: safe to patch */
        size_t n = ray_str_len(img);
        if (hdr && n >= WF_B_OFF && p[0] == 0xfe && p[1] == 0x20)
            memset(p + 8, 0, 8);
        if (hdr && n >= WF_D_OFF && p[0] == 0xfd && p[1] == 0x20)
            memset(p + WF_D_DESC + 8, 0, 8);
        return q_io_zip_write(path, p, n, lbs, alg, lvl);
    }
    return q_io_write_all(path, ray_str_ptr(img), ray_str_len(img));
}

static ray_t* wf_write_flat(ray_t* x, ray_t* y, int lbs, int alg, int lvl) {
    ray_t* path = q_io_file_path(x);
    if (!path) return NULL;
    if (ray_eval_get_restricted()) { ray_release(path); return q_err(QE_ACCESS); }
    ray_retain(y);
    y = q_eval_apply_concrete(y);              /* storage boundary: no lazy on disk */
    ray_t* img = wf_write_image(y);
    ray_release(y);
    if (RAY_IS_ERR(img)) { ray_release(path); return img; }
    if (alg < 0 && !wf_zd(&lbs, &alg, &lvl)) alg = 0;
    ray_t* bad = wf_put(path, img, lbs, alg, lvl, 1);
    ray_release(img);
    ray_release(path);
    if (bad) return bad;
    ray_retain(x);
    return x;
}

/* ---- the domain-append primitive (enumeration's only trace) ------------- */

/* Positions of symv's cells in the domain FILE at dompathstr, extending it
 * append-only with the new names: existing bytes (the advisory header count
 * included — kdb's own appends leave it stale, torq-hdb/sym proves it) are
 * never rewritten, and the file never compresses. */
ray_t* q_wirefile_domain_extend(ray_t* dompathstr, ray_t* symv, ray_t** positions) {
    if (positions) *positions = NULL;
    if (!symv || symv->type != RAY_SYM) return q_err(QE_TYPE);
    ray_t* dom = wf_is_file(dompathstr) ? wf_read_path(dompathstr, 0, NULL) : NULL;
    if (dom && RAY_IS_ERR(dom)) return dom;
    if (dom && dom->type != RAY_SYM) { ray_release(dom); return q_err(QE_TYPE); }
    int64_t nold = dom ? ray_len(dom) : 0;
    int64_t n = ray_len(symv);
    /* open-addressed id->position map over the runtime sym ids both sides carry */
    int64_t hcap = 16;
    while (hcap < (nold + n) * 2) hcap <<= 1;
    int64_t* hid = (int64_t*)malloc((size_t)hcap * sizeof(int64_t));
    int64_t* hpos = (int64_t*)malloc((size_t)hcap * sizeof(int64_t));
    ray_t* fresh = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    ray_t* pos = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (!hid || !hpos || !fresh || RAY_IS_ERR(fresh) || !pos || RAY_IS_ERR(pos)) {
        free(hid); free(hpos);
        if (fresh && !RAY_IS_ERR(fresh)) ray_release(fresh);
        if (pos && !RAY_IS_ERR(pos)) ray_release(pos);
        if (dom) ray_release(dom);
        return q_err(QE_OOM);
    }
    memset(hid, 0xff, (size_t)hcap * sizeof(int64_t));   /* -1 = empty slot */
    for (int64_t i = 0; i < nold; i++) {
        int64_t id = ray_vec_get_sym_id(dom, i);
        int64_t h = (int64_t)((uint64_t)id * 0x9e3779b97f4a7c15ULL >> 1) & (hcap - 1);
        while (hid[h] != -1 && hid[h] != id) h = (h + 1) & (hcap - 1);
        if (hid[h] == -1) { hid[h] = id; hpos[h] = i; }  /* first occurrence wins */
    }
    int64_t next = nold;
    for (int64_t i = 0; i < n && pos && !RAY_IS_ERR(pos); i++) {
        int64_t id = ray_vec_get_sym_id(symv, i);
        int64_t h = (int64_t)((uint64_t)id * 0x9e3779b97f4a7c15ULL >> 1) & (hcap - 1);
        while (hid[h] != -1 && hid[h] != id) h = (h + 1) & (hcap - 1);
        if (hid[h] == -1) {
            hid[h] = id; hpos[h] = next++;
            fresh = ray_vec_append(fresh, &id);
            if (!fresh || RAY_IS_ERR(fresh)) { ray_release(pos); pos = NULL; break; }
        }
        pos = ray_vec_append(pos, &hpos[h]);
    }
    free(hid); free(hpos);
    if (dom) ray_release(dom);
    if (!pos || RAY_IS_ERR(pos) || !fresh || RAY_IS_ERR(fresh)) {
        if (fresh && !RAY_IS_ERR(fresh)) ray_release(fresh);
        if (pos && !RAY_IS_ERR(pos)) ray_release(pos);
        return q_err(QE_OOM);
    }
    ray_t* bad = NULL;
    if (nold == 0 && ray_len(fresh) >= 0 && !wf_is_file(dompathstr)) {
        ray_t* img = wf_write_image(fresh);              /* fresh: true count */
        bad = RAY_IS_ERR(img) ? img
            : q_io_write_all(dompathstr, ray_str_ptr(img), ray_str_len(img));
        if (!RAY_IS_ERR(img) && img != bad) ray_release(img);
    } else if (ray_len(fresh) > 0) {
        ray_t* old = q_io_read_slice(dompathstr, 0, -1, NULL);
        if (!old || RAY_IS_ERR(old)) bad = old ? old : q_err(QE_IO);
        else {
            size_t add = 0;
            for (int64_t i = 0; i < ray_len(fresh); i++) {
                ray_t* nm = ray_sym_str(ray_vec_get_sym_id(fresh, i));
                add += (nm ? ray_str_len(nm) : 0) + 1;
            }
            uint8_t* buf = (uint8_t*)malloc((size_t)ray_len(old) + add);
            if (!buf) bad = q_err(QE_OOM);
            else {
                memcpy(buf, ray_data(old), (size_t)ray_len(old));
                uint8_t* w = buf + ray_len(old);
                for (int64_t i = 0; i < ray_len(fresh); i++) {
                    ray_t* nm = ray_sym_str(ray_vec_get_sym_id(fresh, i));
                    size_t l = nm ? ray_str_len(nm) : 0;
                    memcpy(w, nm ? ray_str_ptr(nm) : "", l);
                    w += l;
                    *w++ = 0;
                }
                bad = q_io_write_all(dompathstr, buf, (size_t)ray_len(old) + add);
                free(buf);
            }
            ray_release(old);
        }
    }
    ray_release(fresh);
    if (bad) { ray_release(pos); return bad; }
    if (positions) *positions = pos;
    else ray_release(pos);
    return NULL;
}

/* ---- the splay writer (kb/splayed-tables.md guards) --------------------- */

/* Shape D enum column against domain basename `dn`: the vendored quote/sym
 * template byte-for-byte — fd20 page, name at +16, descriptor at 4080
 * (fd 00 14 attr, count i64), i64 positions from 4096. */
static ray_t* wf_write_enum_img(ray_t* pos, const char* dn, size_t dnl) {
    int64_t n = ray_len(pos);
    if (!wf_leaf_name(dn, dnl) || dnl > WF_D_DESC - WF_D_NAME - 1)
        return q_err(QE_DOMAIN);
    size_t total = WF_D_OFF + (size_t)n * 8;
    uint8_t* buf = (uint8_t*)calloc(1, total);
    if (!buf) return q_err(QE_OOM);
    buf[0] = 0xfd;
    buf[1] = 0x20;
    memcpy(buf + WF_D_NAME, dn, dnl);
    uint8_t* desc = buf + WF_D_DESC;
    desc[0] = 0xfd;
    desc[2] = WF_ENUM_TYPE;
    memcpy(desc + 8, &n, 8);
    if (n) memcpy(buf + WF_D_OFF, ray_data(pos), (size_t)n * 8);
    ray_t* s = ray_str((const char*)buf, total);
    free(buf);
    return s ? s : q_err(QE_OOM);
}

/* Nested char column pair: `<col>` = fe20 (77+10) + cumulative END offsets,
 * `<col>#` = the raw element bytes (headerless) — the read format, reversed. */
static ray_t* wf_write_nested(ray_t* path, ray_t* col, int lbs, int alg, int lvl) {
    int64_t n = q_count_long(col);
    ray_t* offs = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    size_t total = 0;
    ray_t* bad = NULL;
    for (int64_t i = 0; i < n && offs && !RAY_IS_ERR(offs) && !bad; i++) {
        ray_t* e = q_index_elem_at(col, i);
        const char* ep; int64_t el;
        if (!e || RAY_IS_ERR(e) || !q_str_text_bytes(e, &ep, &el)) bad = q_err(QE_TYPE);
        else {
            total += (size_t)el;
            int64_t end = (int64_t)total;
            offs = ray_vec_append(offs, &end);
        }
        if (e && !RAY_IS_ERR(e)) ray_release(e);
    }
    if (!offs || RAY_IS_ERR(offs)) bad = offs ? offs : q_err(QE_OOM);
    if (bad) { if (offs && !RAY_IS_ERR(offs) && offs != bad) ray_release(offs); return bad; }
    uint8_t* body = (uint8_t*)malloc(total ? total : 1);
    if (!body) { ray_release(offs); return q_err(QE_OOM); }
    size_t w = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = q_index_elem_at(col, i);
        const char* ep; int64_t el;
        if (e && !RAY_IS_ERR(e) && q_str_text_bytes(e, &ep, &el)) {
            memcpy(body + w, ep, (size_t)el);
            w += (size_t)el;
        }
        if (e && !RAY_IS_ERR(e)) ray_release(e);
    }
    ray_t* img = wf_write_b(offs, RAY_I64);      /* i64 END offsets... */
    ray_release(offs);
    if (RAY_IS_ERR(img)) { free(body); return img; }
    ((char*)ray_str_ptr(img))[2] = (char)(WF_NEST_BIAS + RAY_CHARV);  /* ...tagged 87 */
    bad = wf_put(path, img, lbs, alg, lvl, 1);
    ray_release(img);
    if (!bad) {
        ray_t* cp = wf_join(ray_str_ptr(path), ray_str_len(path), "#", 1);
        if (!cp) bad = q_err(QE_OOM);
        else {
            ray_t* bimg = ray_str((const char*)body, total);
            if (!bimg) bad = q_err(QE_OOM);
            else {
                bad = wf_put(cp, bimg, lbs, alg, lvl, 0);
                ray_release(bimg);
            }
            ray_release(cp);
        }
    }
    free(body);
    return bad;
}

static int wf_col_is_nested(ray_t* col) {
    if (!col) return 0;
    if (col->type == RAY_STR) return 1;
    if (col->type != RAY_LIST) return 0;
    int64_t n = ray_len(col);
    ray_t** e = (ray_t**)ray_data(col);
    for (int64_t i = 0; i < n; i++)
        if (!e[i] || (e[i]->type != RAY_CHARV && e[i]->type != -RAY_STR &&
                      e[i]->type != -RAY_CHARV))
            return 0;
    return 1;
}

/* Domain path: named (2-item set), else `<parent-of-dir>/sym` — `.Q.en`'s
 * geography (`` `:db/tr/ set t `` enumerates against db/sym). */
static ray_t* wf_domain_path(ray_t* dirstr, ray_t* domsym) {
    if (domsym) {
        ray_t* p = q_io_file_path(domsym);
        return p ? p : q_err(QE_TYPE);
    }
    const char* p = ray_str_ptr(dirstr);
    size_t n = ray_str_len(dirstr);
    size_t cut = n > 1 ? n - 1 : n;              /* drop the trailing slash */
    while (cut && p[cut - 1] != '/') cut--;
    return wf_join(p, cut, "sym", 3);
}

static ray_t* wf_write_splay_dir(ray_t* dirstr, ray_t* domsym, ray_t* y,
                                 int lbs, int alg, int lvl) {
    if (!y || y->type != RAY_TABLE)
        return q_err(QE_TYPE);                   /* keyed/dict/atom: kb guard */
    int64_t nc = ray_table_ncols(y);
    if (nc <= 0) return q_err(QE_TYPE);
    if (alg < 0 && !wf_zd(&lbs, &alg, &lvl)) alg = 0;
    ray_t* dompath = NULL;
    ray_t* names = ray_sym_vec_new(RAY_SYM_W64, nc);
    ray_t* bad = !names || RAY_IS_ERR(names) ? q_err(QE_OOM) : NULL;
    for (int64_t c = 0; c < nc && !bad; c++) {
        int64_t id = ray_table_col_name(y, c);
        ray_t* col = ray_table_get_col_idx(y, c);    /* borrowed */
        ray_t* nm = ray_sym_str(id);
        if (!col || !nm || !wf_leaf_name(ray_str_ptr(nm), ray_str_len(nm))) {
            bad = q_err(QE_TYPE);
            break;
        }
        ray_t* cp = wf_join(ray_str_ptr(dirstr), ray_str_len(dirstr),
                            ray_str_ptr(nm), ray_str_len(nm));
        if (!cp) { bad = q_err(QE_OOM); break; }
        if (col->type == RAY_SYM) {                  /* auto-enumerate (fused) */
            if (!dompath) {
                dompath = wf_domain_path(dirstr, domsym);
                if (RAY_IS_ERR(dompath)) { bad = dompath; dompath = NULL; }
            }
            ray_t* pos = NULL;
            if (!bad) bad = q_wirefile_domain_extend(dompath, col, &pos);
            if (!bad) {
                const char* dp = ray_str_ptr(dompath);
                size_t dn = ray_str_len(dompath), base = dn;
                while (base && dp[base - 1] != '/') base--;
                ray_t* img = wf_write_enum_img(pos, dp + base, dn - base);
                bad = RAY_IS_ERR(img) ? img : wf_put(cp, img, lbs, alg, lvl, 1);
                if (!RAY_IS_ERR(img) && img != bad) ray_release(img);
            }
            if (pos) ray_release(pos);
        } else if (wf_col_is_nested(col)) {
            bad = wf_write_nested(cp, col, lbs, alg, lvl);
        } else if (col->type > 0 && wf_simple_tag((uint8_t)col->type)) {
            ray_t* img = wf_write_b(col, (uint8_t)col->type);
            bad = RAY_IS_ERR(img) ? img : wf_put(cp, img, lbs, alg, lvl, 1);
            if (!RAY_IS_ERR(img) && img != bad) ray_release(img);
        } else {
            bad = q_err(QE_TYPE);                    /* kb: vectors/compound only */
        }
        ray_release(cp);
        if (!bad) {
            names = ray_vec_append(names, &id);
            if (!names || RAY_IS_ERR(names)) bad = q_err(QE_OOM);
        }
    }
    if (dompath) ray_release(dompath);
    if (!bad) {                                      /* .d LAST: a torn write is
                                                      * detectably incomplete */
        ray_t* dp = wf_join(ray_str_ptr(dirstr), ray_str_len(dirstr), ".d", 2);
        ray_t* img = dp ? wf_write_image(names) : NULL;
        if (!dp || !img) bad = q_err(QE_OOM);
        else bad = RAY_IS_ERR(img) ? img
                 : q_io_write_all(dp, ray_str_ptr(img), ray_str_len(img));
        if (img && !RAY_IS_ERR(img) && img != bad) ray_release(img);
        if (dp) ray_release(dp);
    }
    if (names && !RAY_IS_ERR(names)) ray_release(names);
    return bad;
}

ray_t* q_wirefile_write_splay(ray_t* dirsym, ray_t* domsym, ray_t* y,
                              int lbs, int alg, int lvl) {
    ray_t* dir = q_io_file_path(dirsym);
    if (!dir) return q_err(QE_TYPE);
    if (ray_eval_get_restricted()) { ray_release(dir); return q_err(QE_ACCESS); }
    size_t n = ray_str_len(dir);
    if (!n || ray_str_ptr(dir)[n - 1] != '/') { ray_release(dir); return q_err(QE_TYPE); }
    ray_retain(y);
    y = q_eval_apply_concrete(y);
    ray_t* bad = wf_write_splay_dir(dir, domsym, y, lbs, alg, lvl);
    ray_release(y);
    ray_release(dir);
    if (bad) return bad;
    q_splay_invalidate(dirsym->i64);   /* a warm entry is now a lie */
    ray_retain(dirsym);
    return dirsym;
}

ray_t* q_wirefile_write_zip(ray_t* x, ray_t* y, int lbs, int alg, int lvl) {
    if (!y) return q_err(QE_TYPE);
    if (alg == 1 || alg == 3 || alg == 4 || alg == 5)
        return q_err(QE_NYI);                        /* the request site */
    ray_t* path = q_io_file_path(x);
    if (!path) return q_err(QE_TYPE);
    size_t n = ray_str_len(path);
    int splay = n && ray_str_ptr(path)[n - 1] == '/';
    ray_release(path);
    if (splay) return q_wirefile_write_splay(x, NULL, y, lbs, alg, lvl);
    ray_t* r = wf_write_flat(x, y, lbs, alg, lvl);
    return r ? r : q_err(QE_TYPE);
}

ray_t* q_wirefile_write(ray_t* x, ray_t* y) {
    if (!y) return q_err(QE_TYPE);
    ray_t* path = q_io_file_path(x);
    if (!path) return NULL;
    size_t n = ray_str_len(path);
    int splay = n && ray_str_ptr(path)[n - 1] == '/';
    ray_release(path);
    if (splay) return q_wirefile_write_splay(x, NULL, y, -1, -1, -1);
    return wf_write_flat(x, y, -1, -1, -1);          /* .z.zd may compress */
}

/* `.Q.en[dom;t]` (ref/dotq.md): extend `dom/sym` with every sym column's
 * symbols, bind the domain list as the `sym` global, hand t back UNCHANGED —
 * columns stay 11h (the standing no-enum-type divergence). */
ray_t* q_wirefile_en(ray_t* dom, ray_t* t) {
    if (!t || t->type != RAY_TABLE || !dom || dom->type != -RAY_SYM)
        return q_err(QE_TYPE);
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    ray_t* dp = q_io_file_path(dom);
    if (!dp) return q_err(QE_TYPE);
    size_t dn = ray_str_len(dp);
    int slashed = dn && ray_str_ptr(dp)[dn - 1] == '/';
    ray_t* dompath = wf_join(ray_str_ptr(dp), dn, slashed ? "sym" : "/sym",
                             slashed ? 3 : 4);
    ray_release(dp);
    if (!dompath) return q_err(QE_OOM);
    ray_t* bad = NULL;
    int64_t nc = ray_table_ncols(t);
    for (int64_t c = 0; c < nc && !bad; c++) {
        ray_t* col = ray_table_get_col_idx(t, c);        /* borrowed */
        if (col && col->type == RAY_SYM)
            bad = q_wirefile_domain_extend(dompath, col, NULL);
    }
    if (!bad) {
        ray_t* full = q_wirefile_read_column(dompath, NULL, NULL);
        if (full && !RAY_IS_ERR(full) && full->type == RAY_SYM) {
            (void)q_env_set(ray_sym_intern_runtime("sym", 3), full);  /* retains */
            ray_release(full);
        } else if (full && RAY_IS_ERR(full)) {
            bad = full;
        } else if (full) {
            ray_release(full);
        }
    }
    ray_release(dompath);
    if (bad) return bad;
    ray_retain(t);
    return t;
}
