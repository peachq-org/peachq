/* q_zip — the zip archive (see q_zip.h).  Record layouts are APPNOTE.TXT 6.3.10 § 4.3: the end-of-central-
 * directory record (and its zip64 twin) locates the central directory, each central record names an entry and
 * its local header, and the entry's bytes follow that header. */
#include "qlang/q_count.h"
#include "qlang/io/q_zip.h"
#include "qlang/io/q_io.h"          /* q_io_stored_* — the archive's bytes over any transport; q_io_path_operand */
#include "qlang/net/q_gz.h"         /* q_gz_raw_* — method 8; q_gz_crc32 */
#include "qlang/base/q_err.h"
#include "qlang/base/q_calendar.h"  /* the DOS stamp as a timestamp */
#include "qlang/ops/q_table.h"      /* q_table_cols_from_accs — the .zip.i.info table */
#include "qlang/q_prim.h"           /* q_list_collapse, q_str_in */
#include "qlang/q_env.h"            /* q_env_bind_native */
#include "lang/env.h"               /* ray_fn_unary */
#include "lang/eval.h"              /* RAY_FN_NONE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIG_LOCAL 0x04034b50u
#define SIG_CENTRAL 0x02014b50u
#define SIG_EOCD 0x06054b50u
#define SIG_EOCD64 0x06064b50u
#define SIG_LOC64 0x07064b50u
#define EOCD_LEN 22
#define CENTRAL_LEN 46
#define LOCAL_LEN 30
#define ZIP_INBUF ((size_t)1 << 20)

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static ray_t* zip_fail(q_err_e cls, const char* fmt, const char* a, size_t an, const char* b, size_t bn) {
    ray_t* e = q_err(cls);
    char buf[700];
    int m = snprintf(buf, sizeof buf, fmt, q_err_class(e), (int)an, a, (int)bn, b);
    ray_t* t = m > 0 ? ray_charv(buf, m < (int)sizeof buf ? m : (int)sizeof buf - 1) : NULL;
    if (t && !RAY_IS_ERR(t)) q_err_put(t);
    return e;
}

typedef struct {
    ray_t*   path;               /* borrowed */
    int64_t  fsize;
    uint8_t* cd;
    size_t   cdlen;
    int64_t  count;
} zip_arch_t;

typedef struct {
    const uint8_t* name;
    size_t   nlen;
    uint16_t flags, method, time, date;
    uint32_t crc;
    int64_t  usize, csize, lho;
} zip_ent_t;

static ray_t* corrupt(void) { return q_err(QE_CORRUPT); }

/* `n` bytes at `off` of the archive, OWNED: the transport's error, or 'corrupt when it holds fewer */
static ray_t* zip_bytes(ray_t* path, int64_t off, size_t n) {
    ray_t* b = q_io_stored_read(path, off, (int64_t)n);
    if (!RAY_IS_ERR(b) && q_count(b) != (int64_t)n) { ray_release(b); return corrupt(); }
    return b;
}

/* a zip64 field: the 64-bit value stands where the 32-bit one is saturated; past INT64_MAX is no file */
static int z64(int64_t* v, uint64_t wide) {
    if (wide > (uint64_t)INT64_MAX) return 0;
    *v = (int64_t)wide;
    return 1;
}

/* The central directory, located from the end records.  `fsize` < 0 asks the transport for it. */
static ray_t* arch_open(ray_t* path, int64_t fsize, zip_arch_t* a) {
    memset(a, 0, sizeof *a);
    a->path = path;
    if (fsize < 0) {
        ray_t* sz = q_io_stored_size(path);
        if (RAY_IS_ERR(sz)) return sz;
        fsize = sz->i64;
        ray_release(sz);
    }
    a->fsize = fsize;
    if (a->fsize < EOCD_LEN) return corrupt();
    size_t tl = a->fsize < EOCD_LEN + 65535 ? (size_t)a->fsize : EOCD_LEN + 65535;
    int64_t tail_at = a->fsize - (int64_t)tl;
    ray_t* tb = zip_bytes(path, tail_at, tl);
    if (RAY_IS_ERR(tb)) return tb;
    const uint8_t* tail = (const uint8_t*)ray_data(tb);
    int64_t e = -1;
    for (int64_t i = (int64_t)tl - EOCD_LEN; i >= 0 && e < 0; i--)
        if (rd32(tail + i) == SIG_EOCD && i + EOCD_LEN + rd16(tail + i + 20) == (int64_t)tl) e = i;
    if (e < 0) { ray_release(tb); return corrupt(); }
    const uint8_t* r = tail + e;
    int multi = rd16(r + 4) != 0 || rd16(r + 6) != 0;
    uint64_t count = rd16(r + 10), cdsize = rd32(r + 12), cdoff = rd32(r + 16);
    int64_t eocd_at = tail_at + e, cd_end = eocd_at;
    ray_release(tb);
    if (count == 0xffff || cdsize == 0xffffffffu || cdoff == 0xffffffffu) {
        if (eocd_at < 20) return corrupt();
        ray_t* lb = zip_bytes(path, eocd_at - 20, 20);
        if (RAY_IS_ERR(lb)) return lb;
        const uint8_t* loc = (const uint8_t*)ray_data(lb);
        uint64_t zat = rd64(loc + 8);
        int bad = rd32(loc) != SIG_LOC64 || zat > (uint64_t)(eocd_at - 20 - 56);
        multi |= rd32(loc + 4) != 0;
        ray_release(lb);
        if (bad) return corrupt();
        ray_t* zb = zip_bytes(path, (int64_t)zat, 56);
        if (RAY_IS_ERR(zb)) return zb;
        const uint8_t* z = (const uint8_t*)ray_data(zb);
        bad = rd32(z) != SIG_EOCD64;
        multi |= rd32(z + 16) != 0 || rd32(z + 20) != 0;
        count = rd64(z + 32);
        cdsize = rd64(z + 40);
        cdoff = rd64(z + 48);
        ray_release(zb);
        if (bad) return corrupt();
        cd_end = (int64_t)zat;
    }
    if (multi) return q_err(QE_NYI);
    if (cdoff > (uint64_t)cd_end || cdsize > (uint64_t)cd_end - cdoff || count > cdsize / CENTRAL_LEN) return corrupt();
    a->cdlen = (size_t)cdsize;
    a->count = (int64_t)count;
    a->cd = (uint8_t*)malloc(a->cdlen ? a->cdlen : 1);
    if (!a->cd) return q_err(QE_OOM);
    if (!a->cdlen) return NULL;
    ray_t* cb = zip_bytes(path, (int64_t)cdoff, a->cdlen);
    if (RAY_IS_ERR(cb)) return cb;
    memcpy(a->cd, ray_data(cb), a->cdlen);
    ray_release(cb);
    return NULL;
}

/* the central record at *at, which moves past it */
static ray_t* arch_next(const zip_arch_t* a, size_t* at, zip_ent_t* e) {
    const uint8_t* p = a->cd + *at;
    size_t left = a->cdlen - *at;
    if (left < CENTRAL_LEN || rd32(p) != SIG_CENTRAL) return corrupt();
    size_t nl = rd16(p + 28), el = rd16(p + 30), cl = rd16(p + 32);
    if (CENTRAL_LEN + nl + el + cl > left) return corrupt();
    e->flags = rd16(p + 8);
    e->method = rd16(p + 10);
    e->time = rd16(p + 12);
    e->date = rd16(p + 14);
    e->crc = rd32(p + 16);
    e->csize = rd32(p + 20);
    e->usize = rd32(p + 24);
    e->lho = rd32(p + 42);
    e->name = p + CENTRAL_LEN;
    e->nlen = nl;
    int need_u = e->usize == 0xffffffff, need_c = e->csize == 0xffffffff, need_o = e->lho == 0xffffffff;
    const uint8_t* x = p + CENTRAL_LEN + nl;
    const uint8_t* end = x + el;
    while (x + 4 <= end) {
        const uint8_t* f = x + 4;
        const uint8_t* fe = f + rd16(x + 2);
        if (fe > end) return corrupt();
        if (rd16(x) == 1) {
            if (need_u) { if (f + 8 > fe || !z64(&e->usize, rd64(f))) return corrupt(); f += 8; need_u = 0; }
            if (need_c) { if (f + 8 > fe || !z64(&e->csize, rd64(f))) return corrupt(); f += 8; need_c = 0; }
            if (need_o) { if (f + 8 > fe || !z64(&e->lho, rd64(f))) return corrupt(); need_o = 0; }
        }
        x = fe;
    }
    if (need_u || need_c || need_o) return corrupt();
    if (rd16(p + 34) != 0) return q_err(QE_NYI);
    *at += CENTRAL_LEN + nl + el + cl;
    return NULL;
}

static const char* method_name(uint16_t m) {
    switch (m) {
        case 0: return "stored";   case 8: return "deflate";
        case 1: return "shrink";   case 6: return "implode"; case 9: return "deflate64";
        case 12: return "bzip2";   case 14: return "lzma";   case 93: return "zstd";
        case 95: return "xz";      case 98: return "ppmd";
        default: return NULL;
    }
}

/* The entry `ent` of the open archive `a`, with *data the offset of its first byte. */
static ray_t* zip_entry(zip_arch_t* a, const char* ent, size_t elen, zip_ent_t* e, int64_t* data) {
    size_t at = 0;
    int64_t k = 0;
    for (; k < a->count; k++) {
        ray_t* bad = arch_next(a, &at, e);
        if (bad) return bad;
        if (e->nlen == elen && memcmp(e->name, ent, elen) == 0) break;
    }
    if (k == a->count) return q_io_no_member(a->path, ent, elen);
    if (e->flags & 1) return zip_fail(QE_NYI, "%s. zip entry %.*s is encrypted%.*s", ent, elen, "", 0);
    if (e->method != 0 && e->method != 8) {
        const char* mn = method_name(e->method);
        char m[40];
        int ml = snprintf(m, sizeof m, "%u (%s)", e->method, mn ? mn : "unknown");
        return zip_fail(QE_NYI, "%s. zip method %.*s of %.*s", m, (size_t)ml, ent, elen);
    }
    if (e->method == 0 && e->csize != e->usize) return corrupt();
    if (e->lho > a->fsize - LOCAL_LEN - (int64_t)elen) return corrupt();
    ray_t* hb = zip_bytes(a->path, e->lho, LOCAL_LEN + elen);
    if (RAY_IS_ERR(hb)) return hb;
    const uint8_t* h = (const uint8_t*)ray_data(hb);
    int same = rd32(h) == SIG_LOCAL && rd16(h + 8) == e->method && rd16(h + 26) == elen
            && memcmp(h + LOCAL_LEN, ent, elen) == 0;
    *data = e->lho + LOCAL_LEN + (int64_t)elen + rd16(h + 28);
    ray_release(hb);
    if (!same || e->csize > a->fsize - *data) return corrupt();
    return NULL;
}

/* ---- the forward stream ---- */

enum { ZS_LIVE, ZS_DONE, ZS_FAILED };

typedef struct {
    q_io_stream_t io;
    ray_t*       path;                      /* owned */
    uint16_t     method;
    uint32_t     crc_want, crc;
    int64_t      data, csize, usize;
    int64_t      in_pos;                    /* compressed bytes fetched */
    ray_t*       in;
    size_t       in_at;
    q_gz_raw_t*  z;
    int          ended, state;
} zip_stream_t;

static ray_t* zs_fail(zip_stream_t* s, ray_t* why) {
    s->state = ZS_FAILED;
    return why ? why : corrupt();
}

static size_t zs_avail(const zip_stream_t* s) { return s->in ? (size_t)q_count(s->in) - s->in_at : 0; }

/* one inflate step into out[0..cap), fetching input when the buffer is spent: the transport's error, or 'corrupt
 * for bad or truncated data */
static ray_t* zs_inflate(zip_stream_t* s, uint8_t* out, size_t cap, size_t* made) {
    if (!zs_avail(s) && s->in_pos < s->csize) {
        int64_t k = s->csize - s->in_pos < (int64_t)ZIP_INBUF ? s->csize - s->in_pos : (int64_t)ZIP_INBUF;
        if (s->in) ray_release(s->in);
        s->in = zip_bytes(s->path, s->data + s->in_pos, (size_t)k);
        if (RAY_IS_ERR(s->in)) {
            ray_t* e = s->in;
            s->in = NULL;
            return e;
        }
        s->in_pos += k;
        s->in_at = 0;
    }
    size_t used = 0, avail = zs_avail(s);
    int rc = q_gz_raw_step(s->z, avail ? (const uint8_t*)ray_data(s->in) + s->in_at : (const uint8_t*)"", avail,
                           &used, out, cap, made);
    s->in_at += used;
    if (rc == 1) s->ended = 1;
    return rc < 0 || (rc == 0 && !used && !*made) ? corrupt() : NULL;
}

/* the entry's end: the stream closes exactly at usize, on exactly csize bytes, with the central CRC */
static ray_t* zs_finish(zip_stream_t* s) {
    if (s->method == 8) {
        while (!s->ended) {
            uint8_t extra;
            size_t made = 0;
            ray_t* bad = zs_inflate(s, &extra, 1, &made);
            if (bad || made) return zs_fail(s, bad);
        }
        if (s->in_pos - (int64_t)zs_avail(s) != s->csize) return zs_fail(s, NULL);
    }
    if (s->crc != s->crc_want) return zs_fail(s, NULL);
    s->state = ZS_DONE;
    return NULL;
}

/* up to `want` bytes into out; the entry's last bytes are only handed over once checked */
static ray_t* zs_next(q_io_stream_t* io, uint8_t* out, size_t want, size_t* got) {
    zip_stream_t* s = (zip_stream_t*)io;
    *got = 0;
    if (s->state == ZS_FAILED) return corrupt();
    int64_t at = io->pos;
    while (s->state == ZS_LIVE && *got < want && at < s->usize) {
        size_t room = want - *got;
        if ((int64_t)room > s->usize - at) room = (size_t)(s->usize - at);
        size_t made = 0;
        if (s->method == 0) {
            ray_t* b = zip_bytes(s->path, s->data + at, room);
            if (RAY_IS_ERR(b)) return zs_fail(s, b);
            memcpy(out + *got, ray_data(b), room);
            ray_release(b);
            made = room;
        } else {
            ray_t* bad = zs_inflate(s, out + *got, room, &made);
            if (bad || (s->ended && at + (int64_t)made < s->usize)) return zs_fail(s, bad);
        }
        s->crc = q_gz_crc32(s->crc, out + *got, made);
        at += (int64_t)made;
        *got += made;
    }
    return s->state == ZS_LIVE && at == s->usize ? zs_finish(s) : NULL;
}

static void zs_free(q_io_stream_t* io) {
    zip_stream_t* s = (zip_stream_t*)io;
    if (s->in) ray_release(s->in);
    ray_release(s->path);
    q_gz_raw_free(s->z);
    free(s);
}

ray_t* q_zip_member(ray_t* arch, int64_t asize, const char* m, size_t mn, q_io_stream_t** out, int64_t* size) {
    *out = NULL;
    zip_arch_t a;
    zip_ent_t e;
    int64_t data = 0;
    ray_t* bad = arch_open(arch, asize, &a);
    if (!bad) bad = zip_entry(&a, m, mn, &e, &data);
    free(a.cd);
    if (bad) return bad;
    zip_stream_t* s = (zip_stream_t*)calloc(1, sizeof *s);
    q_gz_raw_t* z = e.method == 8 ? q_gz_raw_new() : NULL;
    if (!s || (e.method == 8 && !z)) {
        free(s);
        q_gz_raw_free(z);
        return q_err(QE_OOM);
    }
    ray_retain(arch);
    *s = (zip_stream_t){ .io = { zs_next, zs_free, 0, 0 }, .path = arch, .method = e.method, .crc_want = e.crc,
                         .data = data, .csize = e.csize, .usize = e.usize, .z = z };
    *out = &s->io;
    *size = e.usize;
    return NULL;
}

ray_t* q_zip_names(ray_t* arch, int64_t asize) {
    zip_arch_t a;
    ray_t* bad = arch_open(arch, asize, &a);
    ray_t* out = bad ? NULL : ray_list_new(a.count);
    size_t at = 0;
    for (int64_t i = 0; !bad && i < a.count; i++) {
        zip_ent_t e;
        if ((bad = arch_next(&a, &at, &e))) break;
        if (e.nlen && e.name[e.nlen - 1] == '/') continue;
        ray_t* nm = ray_charv((const char*)e.name, (int64_t)e.nlen);
        out = ray_list_append(out, nm);
        ray_release(nm);
    }
    free(a.cd);
    if (!bad) return out;
    if (out) ray_release(out);
    return bad;
}

/* ---- .zip.i.info: the central directory as a table ---- */

static int64_t dos_stamp(uint16_t date, uint16_t time) {
    int64_t y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (!q_calendar_date_valid(y, m, d)) return INT64_MIN;
    int64_t tod = ((int64_t)(time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2) * 1000000000LL;
    return q_calendar_ts_compose(q_calendar_days_from_civil(y, m, d), tod);
}

/* an archive with no entries still answers the columns' types */
static ray_t* info_empty_col(int k) {
    return k == 0 ? ray_list_new(0) : k == 3 ? ray_sym_vec_new(RAY_SYM_W64, 0)
         : ray_vec_new(k == 5 ? RAY_TIMESTAMP : RAY_I64, 0);
}

static ray_t* info_keep(ray_t* v) {
    ray_retain(v);
    return v;
}

static ray_t* zip_info_fn(ray_t* x) {
    ray_t* xs = q_str_in(x);
    ray_t* path = q_io_path_operand(xs);
    ray_release(xs);
    if (!path) return q_err(QE_TYPE);
    zip_arch_t a;
    ray_t* bad = arch_open(path, -1, &a);
    static const char* nm[] = { "name", "size", "csize", "method", "crc", "modified" };
    int64_t names[6];
    ray_t* accs[6];
    int empty = !bad && a.count == 0;
    for (int k = 0; k < 6; k++) {
        names[k] = ray_sym_intern_runtime(nm[k], strlen(nm[k]));
        accs[k] = empty ? info_empty_col(k) : ray_list_new(bad ? 0 : a.count);
    }
    size_t at = 0;
    for (int64_t i = 0; !bad && i < a.count; i++) {
        zip_ent_t e;
        if ((bad = arch_next(&a, &at, &e))) break;
        const char* m = method_name(e.method);
        char mn[16];
        if (!m) { snprintf(mn, sizeof mn, "method%u", e.method); m = mn; }
        ray_t* cell[6] = { ray_charv((const char*)e.name, (int64_t)e.nlen), ray_i64(e.usize), ray_i64(e.csize),
                           ray_sym(ray_sym_intern_runtime(m, strlen(m))), ray_i64(e.crc),
                           ray_timestamp(dos_stamp(e.date, e.time)) };
        for (int k = 0; k < 6; k++) {
            accs[k] = ray_list_append(accs[k], cell[k]);
            ray_release(cell[k]);
        }
    }
    free(a.cd);
    ray_release(path);
    if (bad) {
        for (int k = 0; k < 6; k++) ray_release(accs[k]);
        return bad;
    }
    return q_table_cols_from_accs(names, accs, 6, empty ? info_keep : q_list_collapse);
}

void q_zip_register(void) {
    ray_t* f = ray_fn_unary(".zip.i.info", RAY_FN_NONE, zip_info_fn);
    q_env_bind_native(".zip.i.info", f, 1);
    ray_release(f);
}
