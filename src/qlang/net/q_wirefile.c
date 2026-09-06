/* q_wirefile — kdb+ on-disk reader (see q_wirefile.h). */
#define _POSIX_C_SOURCE 200809L   /* lstat */
#include "qlang/net/q_wirefile.h"
#include "qlang/io/q_io.h"      /* the byte core: paths, the slice read, the write */
#include "qlang/net/q_wire.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"  /* q_type_is_int_vec — the `.z.zd` triple */
#include "qlang/q_env.h"        /* q_env_get — `.z.zd` lives as a plain global */
#include "qlang/q_prim.h"       /* q_str_text_bytes — nested CHAR rows */
#include "qlang/q_builtins.h"   /* q_count_long — nested column length */
#include "qlang/ops/q_index.h"  /* q_index_elem_at — nested row reads */
#include "qlang/io/q_splay.h"   /* q_splay_invalidate(_under) — writes drop stale map entries */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_concrete, q_eval_apply_value */
#include "qlang/q_registry.h"   /* the `,` value the append fallback composes on */
#include "lang/eval.h"          /* ray_eval_get_restricted */
#include "store/fileio.h"     /* ray_file_rename — THE platform-armed replace-by-rename */
#include "mem/heap.h"           /* RAY_ATTR_SORTED */
#include <stdio.h>   /* fopen/fread — the header probe reads raw prefix bytes */
#include <stdlib.h>
#include <string.h>
#include <errno.h>   /* ENOENT — append's write-if-absent arm */
#include <sys/stat.h>

static int64_t wf_i64(const uint8_t* p) { int64_t v; memcpy(&v, p, 8); return v; }
static uint32_t wf_u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

static ray_t* wf_read_path(ray_t* path, int follow);
static ray_t* wf_write_a(ray_t* x);

/* ---- the on-disk shapes ------------------------------------------------- */

#define WF_A_OFF   8   /* shape A: ff 01 + the -9! payload */
#define WF_B_OFF  16   /* shape B: fe 20 type attr + 4 pad + count(8) */
#define WF_L_OFF  16   /* legacy: ff 20 00 00 + u32 + type(4) + count(4) — 2009 writer */
#define WF_C_MIN  16   /* shape C: fe + domain name; the count never lands before this */
#define WF_D_NAME 16   /* shape D: fd 20 + a 4096-byte page, domain name at +16 */
#define WF_D_DESC 4080 /* ...whose last 16 bytes are a shape B header */
#define WF_D_OFF  4096
#define WF_ENUM_TYPE 20  /* kdb's enum — read enum-NATIVE as RAY_ENUM (20h) */
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

/* An ALLOWLIST, not a cast: peachq added a type kdb never writes (RAY_STR 21)
 * and RAY_SYM's width is adaptive, so raw disk bytes cannot carry it; disk 20
 * is the enum SHAPE marker, never a simple tag.  Width stays ray_type_sizes'
 * business — one source of truth. */
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
 * bits — translate, never copy.  The byte is TRUSTED (kdb trusts its own
 * files; a lying byte is corrupt-class), so no O(n) verify and no index build:
 * the trusted stamp attaches the marker-only letter, and any kx index trailer
 * beyond count*width is simply never read. */
static const char wf_attr_letter[5] = { 0, 's', 'u', 'p', 'g' };

static ray_t* wf_apply_attr(ray_t* v, uint8_t disk_attr) {
    if (!v || RAY_IS_ERR(v) || disk_attr < 1 || disk_attr > 4) return v;
    return q_attr_stamp_trusted(v, wf_attr_letter[disk_attr]);
}

/* ---- the `.pqattr` sidecar: OUR on-disk carrier for u/p/g ----------------
 * kdb's own u/p/g index trailer is not clean-room reproducible (the format doc
 * records the dissection), and writing attr bytes 2/3/4 without it would hand
 * kdb a file it misreads — so the kx byte stays s-only and the letter rides a
 * VERSIONED sidecar dict `ver`attr`count`size`hash beside the data file (shape
 * A).  count+size+hash bind it to the data-file generation: a rewritten data
 * file stops validating and the letter is DROPPED, never fabricated.  Ordering
 * is drop-sidecar / write-data / write-sidecar, so a crash loses a letter but
 * cannot lie.  kdb reading our splay sees a plain attr-0 column. */
#define WF_SIDECAR_EXT ".pqattr"
#define WF_SIDECAR_EXTLEN 7

static int wf_sidecar_applicable(ray_t* path) {   /* never a sidecar's sidecar */
    size_t n = ray_str_len(path);
    return n < WF_SIDECAR_EXTLEN ||
           memcmp(ray_str_ptr(path) + n - WF_SIDECAR_EXTLEN, WF_SIDECAR_EXT,
                  WF_SIDECAR_EXTLEN) != 0;
}

static ray_t* wf_sidecar_path(ray_t* path) {
    return wf_join(ray_str_ptr(path), ray_str_len(path), WF_SIDECAR_EXT,
                   WF_SIDECAR_EXTLEN);
}

static void wf_sidecar_drop(ray_t* path) {
    if (!wf_sidecar_applicable(path)) return;
    ray_t* sp = wf_sidecar_path(path);
    if (!sp) return;
    remove(ray_str_ptr(sp));                       /* ENOENT is the common case */
    ray_release(sp);
}

/* FNV-1a over the data file's first and last (up to) 4096 bytes — the
 * generation FINGERPRINT beside count+size: an external same-size rewrite
 * flips it and the letter is dropped.  A middle-window-only change on a file
 * beyond 8K stays theoretically invisible — the same trust class as kdb's own
 * unverified attr byte.  -1 = unreadable (never validates). */
static int64_t wf_sidecar_file_hash(const char* p, int64_t size) {
    uint8_t buf[4096];
    FILE* fp = fopen(p, "rb");
    if (!fp) return -1;
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t want = size < 4096 ? (size_t)size : 4096;
    size_t got = fread(buf, 1, want, fp);
    for (size_t i = 0; i < got; i++) { h ^= buf[i]; h *= 0x100000001b3ULL; }
    int bad = got != want;
    if (!bad && size > 4096) {
        int64_t tail = size - 4096;
        bad = fseek(fp, (long)tail, SEEK_SET) != 0 ||
              (got = fread(buf, 1, 4096, fp)) != 4096;
        for (size_t i = 0; i < got && !bad; i++) { h ^= buf[i]; h *= 0x100000001b3ULL; }
    }
    fclose(fp);
    return bad ? -1 : (int64_t)(h >> 1);            /* non-negative */
}

static void wf_sidecar_write(ray_t* path, char letter, int64_t count) {
    if (!wf_sidecar_applicable(path)) return;
    struct stat st;
    if (stat(ray_str_ptr(path), &st) != 0) return;
    int64_t size = (int64_t)st.st_size;
    int64_t hash = wf_sidecar_file_hash(ray_str_ptr(path), size);
    if (hash < 0) return;                            /* letter lost, never lied */
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, 5);
    ray_t* v = ray_list_new(5);
    if (!k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v)) goto out;
    static const char* nm[5] = { "ver", "attr", "count", "size", "hash" };
    for (int i = 0; i < 5; i++) {
        int64_t id = ray_sym_intern_runtime(nm[i], strlen(nm[i]));
        k = ray_vec_append(k, &id);
        if (!k || RAY_IS_ERR(k)) goto out;
    }
    char l1[1] = { letter };
    ray_t* cells[5] = { ray_i64(1), ray_sym(ray_sym_intern_runtime(l1, 1)),
                        ray_i64(count), ray_i64(size), ray_i64(hash) };
    for (int i = 0; i < 5; i++) {
        v = v && !RAY_IS_ERR(v) ? ray_list_append(v, cells[i]) : v;
        if (cells[i] && !RAY_IS_ERR(cells[i])) ray_release(cells[i]);
    }
    if (!v || RAY_IS_ERR(v)) goto out;
    {
        ray_t* d = ray_dict_new(k, v);             /* consumes both */
        k = NULL; v = NULL;
        if (!d || RAY_IS_ERR(d)) { if (d) ray_error_free(d); goto out; }
        ray_t* img = wf_write_a(d);
        ray_release(d);
        if (RAY_IS_ERR(img)) { ray_error_free(img); goto out; }
        ray_t* sp = wf_sidecar_path(path);
        if (sp) {
            ray_t* bad = q_io_write_all(sp, ray_str_ptr(img), ray_str_len(img));
            if (bad) ray_error_free(bad);          /* best-effort: letter lost, never lied */
            ray_release(sp);
        }
        ray_release(img);
        return;
    }
out:
    if (k && !RAY_IS_ERR(k)) ray_release(k);
    if (v && !RAY_IS_ERR(v)) ray_release(v);
}

/* The validated sidecar letter for a data file of `count` elements and `size`
 * bytes — 0 unless every field matches (unknown versions/letters ignored). */
static char wf_sidecar_letter(ray_t* path, int64_t count, int64_t size) {
    if (count < 0 || !wf_sidecar_applicable(path)) return 0;
    ray_t* sp = wf_sidecar_path(path);
    if (!sp) return 0;
    struct stat st;
    if (stat(ray_str_ptr(sp), &st) != 0 || !S_ISREG(st.st_mode)) {
        ray_release(sp);
        return 0;
    }
    ray_t* d = wf_read_path(sp, 0);
    ray_release(sp);
    if (!d || RAY_IS_ERR(d)) { if (d) ray_error_free(d); return 0; }
    char letter = 0;
    int64_t ver = 0, scount = -1, ssize = -1, shash = -1;
    if (d->type == RAY_DICT) {
        ray_t* dk = ray_dict_keys(d);
        ray_t* dv = ray_dict_vals(d);
        int64_t n = dk && dk->type == RAY_SYM ? ray_len(dk) : 0;
        for (int64_t i = 0; i < n; i++) {
            ray_t* nm = ray_sym_str(ray_vec_get_sym_id(dk, i));   /* borrowed */
            ray_t* cell = dv && dv->type == RAY_LIST && i < ray_len(dv)
                        ? ((ray_t**)ray_data(dv))[i] : NULL;
            if (!nm || !cell) continue;
            const char* s = ray_str_ptr(nm);
            size_t sl = ray_str_len(nm);
            if (sl == 3 && !memcmp(s, "ver", 3) && cell->type == -RAY_I64)
                ver = cell->i64;
            else if (sl == 4 && !memcmp(s, "attr", 4) && cell->type == -RAY_SYM) {
                ray_t* ls = ray_sym_str(cell->i64);
                if (ls && ray_str_len(ls) == 1) letter = ray_str_ptr(ls)[0];
            } else if (sl == 5 && !memcmp(s, "count", 5) && cell->type == -RAY_I64)
                scount = cell->i64;
            else if (sl == 4 && !memcmp(s, "size", 4) && cell->type == -RAY_I64)
                ssize = cell->i64;
            else if (sl == 4 && !memcmp(s, "hash", 4) && cell->type == -RAY_I64)
                shash = cell->i64;
        }
    }
    ray_release(d);
    if (ver != 1 || scount != count || ssize != size || shash < 0 ||
        (letter != 'u' && letter != 'p' && letter != 'g'))
        return 0;
    return shash == wf_sidecar_file_hash(ray_str_ptr(path), size) ? letter : 0;
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
    if (buf[2] == RAY_SYM)
        return wf_apply_attr(wf_syms_to_eof(buf + WF_A_OFF, len - WF_A_OFF), buf[3]);
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

/* N cumulative END offsets in companion BYTES, element i spanning
 * [off[i-1], off[i]).  The format doc pins bytes for char (the last offset
 * equals `#`'s size); byte offsets for wider elements are the PROJECT RULING
 * of 2026-08-25 (splay plan, PR 1) — no published artifact carries one, so we
 * write and read our own generalization.  A span must land on an element
 * boundary. */
static ray_t* wf_read_nested(int8_t elem, ray_t* path, const uint8_t* off, int64_t count) {
    if (!path) return q_err(QE_TYPE);
    uint8_t w = ray_type_sizes[(uint8_t)elem];
    ray_t* out = ray_list_new(count);
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    ray_t* comp = wf_companion(path);
    if (RAY_IS_ERR(comp)) { ray_release(out); return comp; }
    const uint8_t* data = (const uint8_t*)ray_data(comp);
    int64_t size = ray_len(comp), prev = 0;
    for (int64_t i = 0; i < count; i++) {
        int64_t end = wf_i64(off + i * 8);
        if (end < prev || end > size || (end - prev) % w) {
            ray_release(out); out = q_err(QE_CORRUPT); break;
        }
        ray_t* e = q_wire_fixed_vec(elem, data + prev, (end - prev) / w, 0);
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

/* The 2009 32-bit layout (the format doc's `legacy` row; qspec fixtures are the
 * witnesses): 4-byte type and count where B has 1-byte type and count(8), no attr
 * byte, payload at 16 exactly like B — so the mmap lane serves it unchanged.
 * The u32 at offset 4 is the same opaque stamp in every exemplar; not validated. */
static ray_t* wf_legacy_hdr(const uint8_t* buf, size_t got, size_t fsz,
                            int8_t* tag, int64_t* count) {
    if (got < WF_L_OFF || fsz < WF_L_OFF || buf[2] || buf[3]) return q_err(QE_CORRUPT);
    uint32_t dt = wf_u32(buf + 8);
    *tag = dt <= 0xff ? wf_simple_tag((uint8_t)dt) : 0;
    if (!*tag) return q_err(QE_TYPE);
    *count = (int64_t)wf_u32(buf + 12);
    if ((uint64_t)*count > (fsz - WF_L_OFF) / ray_type_sizes[(uint8_t)*tag])
        return q_err(QE_CORRUPT);
    return NULL;
}

static ray_t* wf_read_legacy(const uint8_t* buf, size_t len) {
    int8_t tag = 0;
    int64_t count = 0;
    ray_t* e = wf_legacy_hdr(buf, len, len, &tag, &count);
    return e ? e : q_wire_fixed_vec(tag, buf + WF_L_OFF, count, 0);
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
    return wf_apply_attr(q_wire_fixed_vec(tag, buf + WF_B_OFF, count, 0), buf[3]);
}

static ray_t* wf_read_path(ray_t* path, int follow);

/* C and D agree from the count on: count(8) then `w`-byte indices.  The read
 * is ENUM-NATIVE (2026-08-22 plan): widen the indices to i64, stamp the
 * domain-NAME sym, return a 20h vector — the domain FILE is never touched
 * here; resolution is lazy through the env (the root `sym` binds at splay
 * registration, io/q_splay.c).  Sorted-by-domain-position is not sorted-by-
 * symbol, so the attribute cannot survive. */
static ray_t* wf_read_enum(const char* domain, const uint8_t* p, size_t room,
                           uint8_t w, uint8_t attr, int derive) {
    size_t nn = strlen(domain);
    if (!wf_leaf_name(domain, nn)) return q_err(QE_CORRUPT);
    int64_t count = wf_i64(p);
    if (derive && count == 0) {
        count = wf_derive_count(room, w, attr);
        if (count < 0) return q_err(QE_NYI);
    }
    if (count < 0 || (uint64_t)count > room / w) return q_err(QE_CORRUPT);
    return wf_apply_attr(q_enum_from_indices(ray_sym_intern_runtime(domain, nn),
                                             p + 8, count, w), attr);
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

static ray_t* wf_read_c(const uint8_t* buf, size_t len, int derive) {
    size_t coff = 0;
    if (!wf_c_count_off(buf, len, len, derive, &coff)) return q_err(QE_CORRUPT);
    return wf_read_enum((const char*)buf + 1, buf + coff, len - coff - 8, 4, 0, derive);
}

static ray_t* wf_read_d(const uint8_t* buf, size_t len, ray_t* path, int derive) {
    if (len < WF_D_OFF) return q_err(QE_CORRUPT);
    const uint8_t* desc = buf + WF_D_DESC;
    if (desc[2] != WF_ENUM_TYPE) return wf_read_b(desc, len - WF_D_DESC, derive, path);
    const uint8_t* nul = (const uint8_t*)memchr(buf + WF_D_NAME, 0, WF_D_DESC - WF_D_NAME);
    if (!nul || nul == buf + WF_D_NAME) return q_err(QE_CORRUPT);
    return wf_read_enum((const char*)buf + WF_D_NAME,
                        desc + 8, len - WF_D_OFF, 8, desc[3], derive);
}

/* An fd page that is not 20 stays deferred. */
typedef enum { WF_UNKNOWN, WF_A, WF_B, WF_C, WF_D, WF_LEGACY, WF_DEFER } wf_shape_t;

static wf_shape_t wf_sniff(const uint8_t* p, size_t n) {
    if (n < 2) return WF_UNKNOWN;
    if (p[0] == 0xff) return p[1] == 0x01 ? WF_A : p[1] == 0x20 ? WF_LEGACY : WF_DEFER;
    if (p[0] == 0xfe) return p[1] == 0x20 ? WF_B : WF_C;
    if (p[0] == 0xfd) return p[1] == 0x20 ? WF_D : WF_DEFER;
    return WF_UNKNOWN;
}

static ray_t* wf_read_image(const uint8_t* buf, size_t len, int unzipped, ray_t* path) {
    switch (wf_sniff(buf, len)) {
    case WF_A:      return wf_read_a(buf, len);
    case WF_B:      return wf_read_b(buf, len, unzipped, path);
    case WF_C:      return wf_read_c(buf, len, unzipped);
    case WF_D:      return wf_read_d(buf, len, path, unzipped);
    case WF_LEGACY: return wf_read_legacy(buf, len);
    case WF_DEFER:  return q_err(QE_NYI);
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

/* `follow` = may this read resolve REFERENCES to other files (the `#`
 * companion, nested arenas)?  A referenced file's own read says no and reaches
 * the readers with a NULL path, which is what keeps the walk flat. */
static ray_t* wf_read_path(ray_t* path, int follow) {
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
                             zipped, follow ? path : NULL);
    ray_release(all);
    /* flat sidecar consult: only an unattributed vector/enum can take the letter */
    if (r && !RAY_IS_ERR(r) && !zipped && follow &&
        (ray_is_vec(r) || r->type == RAY_ENUM) && !q_attr_letter(r)) {
        char sl = wf_sidecar_letter(path, ray_len(r), (int64_t)st.st_size);
        if (sl) r = q_attr_stamp_trusted(r, sl);
    }
    return r;
}

ray_t* q_wirefile_read(ray_t* x) {
    ray_t* path = q_io_file_path(x);
    if (!path) return NULL;
    ray_t* r = ray_eval_get_restricted() ? q_err(QE_ACCESS) : wf_read_path(path, 1);
    ray_release(path);
    return r;
}

ray_t* q_wirefile_read_column(ray_t* pathstr) {
    return wf_read_path(pathstr, 1);
}

/* ---- the header probe (no payload read) --------------------------------- */

/* Shape B / embedded-B tag classification shared by the probe's two arms. */
static ray_t* wf_probe_b(const uint8_t* hdr, q_wf_colhdr* out) {
    uint8_t disk = hdr[2];
    if (disk == WF_NEST_BIAS) return q_err(QE_NYI);            /* anymap */
    int nested = disk > WF_NEST_BIAS && disk <= WF_NEST_HI;
    int8_t tag = wf_simple_tag(nested ? (uint8_t)(disk - WF_NEST_BIAS) : disk);
    if (!tag) return q_err(QE_TYPE);
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
        out->disk_attr = desc[3];
        out->count = wf_i64(desc + 8);
        if (derive && out->count == 0) {
            out->count = wf_derive_count(fsz - WF_D_OFF, 8, desc[3]);
            if (out->count < 0) return q_err(QE_NYI);
        }
        if (out->count < 0 ||
            (uint64_t)out->count > (fsz - WF_D_OFF) / 8) return q_err(QE_CORRUPT);
        return NULL;
    }
    case WF_LEGACY: {                       /* payload at 16 like B: mmap serves it */
        ray_t* e = wf_legacy_hdr(buf, got, fsz, &out->tag, &out->count);
        if (e) return e;
        out->mappable = 1;
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
    ray_t* e = wf_probe_classify(buf, got, fsz, 0, out);
    if (!e && !out->disk_attr && !out->nested)
        out->side_attr = wf_sidecar_letter(pathstr, out->count, (int64_t)fsz);
    return e;
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
static ray_t* wf_write_enum_img(ray_t* pos, const char* dn, size_t dnl);

/* Image for a REFERENCE-shaped column — a 20h enum whose domain is anything
 * but a bound SYMLIST (kb/linking-columns.md; FK enums, links, unbound alike):
 * i64 positions + target-NAME header, no domain file.  The reader stamps the
 * name back and deref/resolution stay lazy.  NULL = symlist-domain (the splay
 * writer decays those into its auto-enumerate arm) or not an enum. */
static ray_t* wf_ref_image(ray_t* x) {
    if (x->type != RAY_ENUM) return NULL;
    int64_t dom = q_enum_domain(x);
    if (q_enum_domain_kind(dom, NULL) == Q_EDOM_SYMLIST) return NULL;
    ray_t* nm = ray_sym_str(dom);
    if (!nm || RAY_IS_ERR(nm)) return nm ? nm : q_err(QE_TYPE);
    ray_t* pos = q_enum_positions(x);
    if (!pos || RAY_IS_ERR(pos)) { ray_release(nm); return pos ? pos : q_err(QE_OOM); }
    ray_t* img = wf_write_enum_img(pos, ray_str_ptr(nm), ray_str_len(nm));
    ray_release(pos);
    ray_release(nm);
    return img;
}

static ray_t* wf_write_image(ray_t* x) {
    ray_t* ref = wf_ref_image(x);
    if (ref) return ref;
    if (x->type == RAY_ENUM) {
        /* a FLAT write of a symlist-domain 20h keeps the enum SHAPE (positions
         * + domain name) — kx's own on-disk-attribute idiom rewrites a splay
         * column in place ({x set `g#get x}`:t/s, owner speed-trick guide) and
         * a decay to shape-A syms would break the splay it belongs to.  Only
         * the splay DIRECTORY writer re-enumerates (the copy-a-splay idiom). */
        ray_t* nm = ray_sym_str(q_enum_domain(x));           /* borrowed */
        ray_t* pos = q_enum_positions(x);
        if (!pos || RAY_IS_ERR(pos)) return pos ? pos : q_err(QE_OOM);
        ray_t* img = nm ? wf_write_enum_img(pos, ray_str_ptr(nm), ray_str_len(nm))
                        : q_err(QE_TYPE);
        ray_release(pos);
        return img;
    }
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

#ifdef RAY_OS_WINDOWS
#define wf_lstat stat     /* no symlink to tell apart */
#else
#define wf_lstat lstat
#endif

/* One data file: the image bytes, plain or through the container.  `hdr` says
 * img carries a file-level count field — compression ZEROES it (the kdbfile
 * trap rows pin this; the reader derives it back from the plain length), so a
 * headerless companion passes 0 and stays untouched.  An EXISTING plain file
 * (never a link) with BYTES IN IT is replaced by `ray_file_rename` beside it: a
 * live mapping keeps its inode where a truncate-in-place would SIGBUS it, and a
 * failed move leaves the old file whole.  An EMPTY file has no mapped page to
 * protect, so it keeps the direct write with every non-regular target — which is
 * what lets `hopen`'s pre-created 0-byte file take its first append while the
 * handle still holds it.  Windows refuses the move while the target is MAPPED
 * (ref/hdel.md:44); that `'io` is the platform's limit, not ours. */
static ray_t* wf_put(ray_t* path, ray_t* img, int lbs, int alg, int lvl, int hdr) {
    struct stat st;
    int over = wf_lstat(ray_str_ptr(path), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
    ray_t* tmp = over ? wf_join(ray_str_ptr(path), ray_str_len(path), ".tmp", 4) : NULL;
    if (over && !tmp) return q_err(QE_OOM);
    ray_t* dst = over ? tmp : path;
    ray_t* bad;
    if (alg > 0) {
        uint8_t* p = (uint8_t*)ray_str_ptr(img);     /* fresh image: safe to patch */
        size_t n = ray_str_len(img);
        if (hdr && n >= WF_B_OFF && p[0] == 0xfe && p[1] == 0x20)
            memset(p + 8, 0, 8);
        if (hdr && n >= WF_D_OFF && p[0] == 0xfd && p[1] == 0x20)
            memset(p + WF_D_DESC + 8, 0, 8);
        bad = q_io_zip_write(dst, p, n, lbs, alg, lvl);
    } else
        bad = q_io_write_all(dst, ray_str_ptr(img), ray_str_len(img));
    if (over) {
        if (!bad && ray_file_rename(ray_str_ptr(tmp), ray_str_ptr(path)) != RAY_OK) bad = q_err(QE_IO);
        if (bad) remove(ray_str_ptr(tmp));
        ray_release(tmp);
    }
    return bad;
}

static ray_t* wf_write_flat_path(ray_t* path, ray_t* y, int lbs, int alg, int lvl) {
    ray_retain(y);
    y = q_eval_apply_concrete(y);              /* storage boundary: no lazy on disk */
    char yl = q_attr_letter(y);
    int64_t yn = ray_is_vec(y) || y->type == RAY_ENUM || y->type == RAY_LIST
               ? ray_len(y) : -1;
    ray_t* img = wf_write_image(y);
    ray_release(y);
    if (RAY_IS_ERR(img)) return img;
    if (alg < 0 && !wf_zd(&lbs, &alg, &lvl)) alg = 0;
    wf_sidecar_drop(path);                     /* drop / write-data / write-sidecar */
    ray_t* bad = wf_put(path, img, lbs, alg, lvl, 1);
    if (!bad && alg <= 0 && yn >= 0 && (yl == 'u' || yl == 'p' || yl == 'g'))
        wf_sidecar_write(path, yl, yn);
    ray_release(img);
    return bad;
}

static ray_t* wf_write_flat(ray_t* x, ray_t* y, int lbs, int alg, int lvl) {
    ray_t* path = q_io_file_path(x);
    if (!path) return NULL;
    if (ray_eval_get_restricted()) { ray_release(path); return q_err(QE_ACCESS); }
    ray_t* bad = wf_write_flat_path(path, y, lbs, alg, lvl);
    if (!bad)   /* a write inside a mapped dir stales its entry (link workflow) */
        q_splay_invalidate_under(ray_str_ptr(path), ray_str_len(path));
    ray_release(path);
    if (bad) return bad;
    ray_retain(x);
    return x;
}

/* ---- the flat-append primitive (one kernel, three doors) ----------------
 * `:f upsert y, .[`:f;();,;y] and file-handle apply all land here (the
 * ref/amend.md Amend Entire law: .[d;();v;y] <=> v[d;y], so append is read
 * -> `,` -> write, with the shape-B in-place arm as its optimization —
 * kb/performance-tips.md:151).  A typed target takes only its own element
 * type; only an untyped shape-A list/atom file delegates to the join home. */

/* Elements at EOF, then the count: a tear leaves the old count (the reader
 * ignores the tail) and the next append's exact-size check routes to the
 * rewrite fallback.  Best-effort ordering, like every writer here (no fsync). */
static ray_t* wf_append_inplace(ray_t* path, ray_t* v, int64_t count) {
    size_t esz = ray_type_sizes[(uint8_t)v->type];
    int64_t n = ray_len(v);
    if (count > INT64_MAX - n) return q_err(QE_LIMIT);
    FILE* fp = fopen(ray_str_ptr(path), "r+b");
    if (!fp) return q_err(QE_IO);
    int64_t nc = count + n;
    int bad = fseek(fp, 0, SEEK_END) != 0 ||
              q_io_fwrite(fp, ray_data(v), esz * (size_t)n) != 0 ||
              fflush(fp) != 0 || fseek(fp, 8, SEEK_SET) != 0 ||
              fwrite(&nc, 8, 1, fp) != 1;
    if (fclose(fp) != 0) bad = 1;
    return bad ? q_err(QE_IO) : NULL;
}

/* Shape-A sym file: NUL-terminated names at EOF, ONE buffer/ONE write (no torn
 * half-symbol), header count untouched — advisory, the reader scans to EOF. */
static ray_t* wf_append_syms(ray_t* path, ray_t* y) {
    int64_t n = y->type == -RAY_SYM ? 1 : ray_len(y);
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t id = y->type == -RAY_SYM ? y->i64 : ray_vec_get_sym_id(y, i);
        ray_t* nm = ray_sym_str(id);                     /* borrowed */
        total += (nm ? ray_str_len(nm) : 0) + 1;
    }
    uint8_t* buf = (uint8_t*)malloc(total ? total : 1);
    if (!buf) return q_err(QE_OOM);
    uint8_t* w = buf;
    for (int64_t i = 0; i < n; i++) {
        int64_t id = y->type == -RAY_SYM ? y->i64 : ray_vec_get_sym_id(y, i);
        ray_t* nm = ray_sym_str(id);
        size_t l = nm ? ray_str_len(nm) : 0;
        memcpy(w, nm ? ray_str_ptr(nm) : "", l);
        w += l;
        *w++ = 0;
    }
    FILE* fp = fopen(ray_str_ptr(path), "ab");
    int bad = !fp || fwrite(buf, 1, total, fp) != total;
    if (fp && fclose(fp) != 0) bad = 1;
    free(buf);
    return bad ? q_err(QE_IO) : NULL;
}

/* read -> `,` -> rewrite; a container is rewritten with its own parameters
 * (alg 0 goes through (2;0), whose never-paying deflate re-emits the alg-0
 * wrapper), a plain file stays plain — `.z.zd` never applies to a rewrite. */
static ray_t* wf_append_rewrite(ray_t* path, ray_t* y, int zipped) {
    ray_t* old = wf_read_path(path, 1);
    if (!old || RAY_IS_ERR(old)) return old ? old : q_err(QE_IO);
    if (old->type == RAY_TABLE || old->type == RAY_DICT || q_type_is_keyed(old)) {
        ray_release(old);
        return q_err(QE_TYPE);       /* serialized-table upsert: its own queued PR */
    }
    ray_t* join = q_registry_lookup_name(",", 1, Q_DYADIC);   /* borrowed */
    if (!join) { ray_release(old); return q_err(QE_TYPE); }
    ray_t* args[2] = { old, y };
    ray_t* joined = q_eval_apply_concrete(q_eval_apply_value(join, args, 2));
    ray_release(old);
    if (!joined || RAY_IS_ERR(joined)) return joined ? joined : q_err(QE_TYPE);
    char jl = q_attr_letter(joined);   /* the in-memory law may keep u/g — disk keeps only s */
    if (jl && jl != 's') {
        ray_t* stripped = q_attr_set_letter(0, joined);   /* `#x — borrows, owned out */
        ray_release(joined);
        if (!stripped || RAY_IS_ERR(stripped)) return stripped ? stripped : q_err(QE_OOM);
        joined = stripped;
    }
    int lbs = 0, alg = 0, lvl = 0;
    if (zipped) {
        q_io_zipmap_t zm;
        ray_t* e = q_io_zip_open(path, &zm);
        if (e) { ray_release(joined); return e; }
        alg = zm.algorithm ? (int)zm.algorithm : 2;
        lvl = zm.algorithm ? (int)zm.level : 0;
        while ((1LL << lbs) < zm.block_size) lbs++;
        q_io_zipmap_free(&zm);
    }
    ray_t* bad = wf_write_flat_path(path, joined, lbs, alg, lvl);
    ray_release(joined);
    return bad;
}

static ray_t* wf_append_path(ray_t* path, ray_t* y) {
    if (!y) return q_err(QE_TYPE);
    ray_retain(y);
    y = q_eval_apply_concrete(y);
    if (!y || RAY_IS_ERR(y)) return y ? y : q_err(QE_TYPE);
    struct stat st;
    errno = 0;
    int have = stat(ray_str_ptr(path), &st) == 0;
    if (!have && errno != ENOENT) { ray_release(y); return q_err(QE_IO); }
    if (have && !S_ISREG(st.st_mode)) { ray_release(y); return q_err(QE_TYPE); }
    ray_t* bad;
    if (!have || st.st_size == 0) {   /* write-if-absent, ref/upsert.md:32 (hopen
                                       * pre-creates a 0-byte file); atoms enlist */
        ray_t* v = y->type < 0 ? q_enlist_wrap(&y, 1) : (ray_retain(y), y);
        bad = !v || RAY_IS_ERR(v) ? (v ? v : q_err(QE_OOM))
                                  : wf_write_flat_path(path, v, -1, -1, -1);
        if (v && !RAY_IS_ERR(v) && v != bad) ray_release(v);
        ray_release(y);
        return bad;
    }
    q_wf_colhdr h;
    ray_t* e = q_wirefile_probe(path, &h);
    if (e) { ray_release(y); return e; }
    if (h.is_enum) { ray_release(y); return q_err(QE_NYI); }   /* enum: won't-do */
    if (h.tag && y->type != h.tag && y->type != -h.tag) {
        ray_release(y);
        return q_err(QE_TYPE);        /* a typed file keeps its element type */
    }
    wf_sidecar_drop(path);   /* only `s` survives an append to disk (set-attribute.md:32);
                              * dropped only once the append is really going to run */
    if (!h.zipped && h.tag == RAY_SYM) {
        uint8_t hd[4] = {0};
        FILE* fp = fopen(ray_str_ptr(path), "rb");
        int plain = fp && fread(hd, 1, 4, fp) == 4 && hd[3] == 0;
        if (fp) fclose(fp);
        bad = plain ? wf_append_syms(path, y) : wf_append_rewrite(path, y, 0);
    } else if (!h.zipped && h.mappable && !h.nested && h.disk_attr == 0 &&
               (int64_t)st.st_size ==
                   WF_B_OFF + h.count * (int64_t)ray_type_sizes[(uint8_t)h.tag]) {
        ray_t* v = y->type < 0 ? q_enlist_wrap(&y, 1) : (ray_retain(y), y);
        bad = !v || RAY_IS_ERR(v) ? (v ? v : q_err(QE_OOM))
                                  : wf_append_inplace(path, v, h.count);
        if (v && !RAY_IS_ERR(v) && v != bad) ray_release(v);
    } else {
        bad = wf_append_rewrite(path, y, h.zipped);
    }
    ray_release(y);
    return bad;
}

ray_t* q_wirefile_append(ray_t* x, ray_t* y) {
    ray_t* path = q_io_file_path(x);
    if (!path) return q_err(QE_TYPE);
    if (ray_eval_get_restricted()) { ray_release(path); return q_err(QE_ACCESS); }
    ray_t* bad = wf_append_path(path, y);
    ray_release(path);
    if (bad) return bad;
    ray_retain(x);
    return x;
}

ray_t* q_wirefile_append_path(ray_t* pathstr, ray_t* y) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    return wf_append_path(pathstr, y);
}

/* ---- the domain-append primitive (enumeration's only trace) ------------- */

/* Positions of symv's cells in the domain FILE at dompathstr, extending it
 * append-only with the new names: existing bytes (the advisory header count
 * included — kdb's own appends leave it stale, torq-hdb/sym proves it) are
 * never rewritten, and the file never compresses. */
ray_t* q_wirefile_domain_extend(ray_t* dompathstr, ray_t* symv, ray_t** positions) {
    if (positions) *positions = NULL;
    if (!symv || symv->type != RAY_SYM) return q_err(QE_TYPE);
    ray_t* dom = wf_is_file(dompathstr) ? wf_read_path(dompathstr, 0) : NULL;
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
    if (!wf_leaf_name(dn, dnl) || dnl > 255)   /* the probe's q_wf_colhdr.domain
                                                * cap — never write a name the
                                                * reader must refuse (codex r3) */
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

/* One row's raw bytes for a nested write: char rows through the text accessor
 * (charv/-STR/-charv alike), any other element type as its vector's payload. */
static int wf_row_bytes(ray_t* e, int8_t elem, const char** p, int64_t* nbytes) {
    if (!e || RAY_IS_ERR(e)) return 0;
    if (elem == RAY_CHARV) return q_str_text_bytes(e, p, nbytes) ? 1 : 0;
    if (e->type != elem) return 0;
    *p = (const char*)ray_data(e);
    *nbytes = ray_len(e) * (int64_t)ray_type_sizes[(uint8_t)elem];
    return 1;
}

/* Nested column pair: `<col>` = fe20 (77+elem) + cumulative END offsets,
 * `<col>#` = the raw element bytes (headerless) — the read format, reversed. */
static ray_t* wf_write_nested(ray_t* path, ray_t* col, int8_t elem,
                              int lbs, int alg, int lvl) {
    int64_t n = q_count_long(col);
    ray_t* offs = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    size_t total = 0;
    ray_t* bad = NULL;
    for (int64_t i = 0; i < n && offs && !RAY_IS_ERR(offs) && !bad; i++) {
        ray_t* e = q_index_elem_at(col, i);
        const char* ep; int64_t el;
        if (!wf_row_bytes(e, elem, &ep, &el)) bad = q_err(QE_TYPE);
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
        if (wf_row_bytes(e, elem, &ep, &el)) {
            memcpy(body + w, ep, (size_t)el);
            w += (size_t)el;
        }
        if (e && !RAY_IS_ERR(e)) ray_release(e);
    }
    ray_t* img = wf_write_b(offs, RAY_I64);      /* i64 END offsets... */
    ray_release(offs);
    if (RAY_IS_ERR(img)) { free(body); return img; }
    ((char*)ray_str_ptr(img))[2] = (char)(WF_NEST_BIAS + elem);  /* ...tagged 77+elem */
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

/* The compound-form classifier (kb/splayed-tables.md:80 — EVERY row a simple
 * vector of ONE element type; never first-item, whose law belongs to meta and
 * would corrupt a later divergent row).  Returns the element tag, 0 for not
 * compound: sym rows and mixed rows are kx's anymap lane, deferred to 'type. */
static int8_t wf_col_nested_tag(ray_t* col) {
    if (!col) return 0;
    if (col->type == RAY_STR) return (int8_t)RAY_CHARV;
    if (col->type != RAY_LIST) return 0;
    int64_t n = ray_len(col);
    ray_t** e = (ray_t**)ray_data(col);
    int8_t t0 = (int8_t)RAY_CHARV;               /* empty column: char, as before */
    for (int64_t i = 0; i < n; i++) {
        ray_t* ei = e[i];
        int8_t ti;
        if (!ei) return 0;
        if (ei->type == RAY_CHARV || ei->type == -RAY_STR || ei->type == -RAY_CHARV)
            ti = (int8_t)RAY_CHARV;
        else if (ei->type > 0 && wf_simple_tag((uint8_t)ei->type))
            ti = (int8_t)ei->type;
        else return 0;
        if (i == 0) t0 = ti; else if (ti != t0) return 0;
    }
    return t0;
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
        int8_t nt;
        ray_t* col = ray_table_get_col_idx(y, c);    /* borrowed */
        ray_t* nm = ray_sym_str(id);
        if (!col || !nm || !wf_leaf_name(ray_str_ptr(nm), ray_str_len(nm))) {
            bad = q_err(QE_TYPE);
            break;
        }
        ray_t* cp = wf_join(ray_str_ptr(dirstr), ray_str_len(dirstr),
                            ray_str_ptr(nm), ray_str_len(nm));
        if (!cp) { bad = q_err(QE_OOM); break; }
        char cl = q_attr_letter(col);                /* before decay strips it */
        int64_t cn = ray_is_vec(col) || col->type == RAY_ENUM ? ray_len(col) : -1;
        wf_sidecar_drop(cp);
        ray_t* dec = NULL;                           /* a SYMLIST-domain 20h column
                                                      * writes as its resolved syms
                                                      * and re-enumerates against
                                                      * THIS dir's domain file (the
                                                      * copy-a-splay idiom) */
        ray_t* rimg = wf_ref_image(col);             /* FK/link: positions + name */
        if (rimg && RAY_IS_ERR(rimg)) { bad = rimg; ray_release(cp); break; }
        if (!rimg && col->type == RAY_ENUM) {
            dec = q_enum_decay(col);
            if (!dec || RAY_IS_ERR(dec)) { bad = dec ? dec : q_err(QE_TYPE); ray_release(cp); break; }
            col = dec;
        }
        if (rimg) {
            bad = wf_put(cp, rimg, lbs, alg, lvl, 1);
            if (rimg != bad) ray_release(rimg);
        } else if (col->type == RAY_SYM) {           /* auto-enumerate (fused) */
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
        } else if ((nt = wf_col_nested_tag(col))) {
            bad = wf_write_nested(cp, col, nt, lbs, alg, lvl);
        } else if (col->type > 0 && wf_simple_tag((uint8_t)col->type)) {
            ray_t* img = wf_write_b(col, (uint8_t)col->type);
            bad = RAY_IS_ERR(img) ? img : wf_put(cp, img, lbs, alg, lvl, 1);
            if (!RAY_IS_ERR(img) && img != bad) ray_release(img);
        } else {
            bad = q_err(QE_TYPE);                    /* kb: vectors/compound only */
        }
        if (dec) ray_release(dec);
        if (!bad && alg <= 0 && cn >= 0 && (cl == 'u' || cl == 'p' || cl == 'g'))
            wf_sidecar_write(cp, cl, cn);
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
        ray_t* full = q_wirefile_read_column(dompath);
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
