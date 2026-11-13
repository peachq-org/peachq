/* q_zip — the zip:// source (see q_zip.h).  Record layouts are APPNOTE.TXT 6.3.10 § 4.3: the end-of-central-
 * directory record (and its zip64 twin) locates the central directory, each central record names an entry and
 * its local header, and the entry's bytes follow that header. */
#include "qlang/io/q_zip.h"
#include "qlang/io/q_io.h"          /* q_io_pread_all, q_io_path_operand */
#include "qlang/io/q_handles.h"     /* q_handles_reserve_fd — a stream's handle number */
#include "qlang/net/q_gz.h"         /* q_gz_raw_* — method 8; q_gz_crc32 */
#include "qlang/base/q_err.h"
#include "qlang/base/q_calendar.h"  /* the DOS stamp as a timestamp */
#include "qlang/ops/q_table.h"      /* q_table_cols_from_accs — the .zip.i.info table */
#include "qlang/q_prim.h"           /* q_list_collapse, q_str_in */
#include "qlang/q_env.h"            /* q_env_bind_native */
#include "lang/env.h"               /* ray_fn_unary */
#include "lang/eval.h"              /* RAY_FN_NONE, ray_eval_get_restricted */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif

#define SIG_LOCAL 0x04034b50u
#define SIG_CENTRAL 0x02014b50u
#define SIG_EOCD 0x06054b50u
#define SIG_EOCD64 0x06064b50u
#define SIG_LOC64 0x07064b50u
#define EOCD_LEN 22
#define CENTRAL_LEN 46
#define LOCAL_LEN 30
#define ZIP_INBUF ((size_t)64 << 10)

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
    int      fd;
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

static void arch_close(zip_arch_t* a) {
    if (a->fd >= 0) close(a->fd);
    free(a->cd);
    a->fd = -1;
    a->cd = NULL;
}

static ray_t* corrupt(void) { return q_err(QE_CORRUPT); }

/* a zip64 field: the 64-bit value stands where the 32-bit one is saturated; past INT64_MAX is no file */
static int z64(int64_t* v, uint64_t wide) {
    if (wide > (uint64_t)INT64_MAX) return 0;
    *v = (int64_t)wide;
    return 1;
}

static ray_t* arch_open(const char* path, zip_arch_t* a) {
    memset(a, 0, sizeof *a);
    a->fd = open(path, O_RDONLY | O_BINARY);
    if (a->fd < 0) return q_err(QE_IO);
    struct stat st;
    if (fstat(a->fd, &st) != 0 || !S_ISREG(st.st_mode)) return q_err(QE_IO);
    a->fsize = (int64_t)st.st_size;
    if (a->fsize < EOCD_LEN) return corrupt();
    size_t tl = a->fsize < EOCD_LEN + 65535 ? (size_t)a->fsize : EOCD_LEN + 65535;
    uint8_t* tail = (uint8_t*)malloc(tl);
    if (!tail) return q_err(QE_OOM);
    int64_t tail_at = a->fsize - (int64_t)tl;
    if (q_io_pread_all(a->fd, tail, tl, tail_at)) { free(tail); return q_err(QE_IO); }
    int64_t e = -1;
    for (int64_t i = (int64_t)tl - EOCD_LEN; i >= 0 && e < 0; i--)
        if (rd32(tail + i) == SIG_EOCD && i + EOCD_LEN + rd16(tail + i + 20) == (int64_t)tl) e = i;
    if (e < 0) { free(tail); return corrupt(); }
    const uint8_t* r = tail + e;
    int multi = rd16(r + 4) != 0 || rd16(r + 6) != 0;
    uint64_t count = rd16(r + 10), cdsize = rd32(r + 12), cdoff = rd32(r + 16);
    int64_t eocd_at = tail_at + e, cd_end = eocd_at;
    if (count == 0xffff || cdsize == 0xffffffffu || cdoff == 0xffffffffu) {
        uint8_t loc[20], z[56];
        if (eocd_at < 20 || q_io_pread_all(a->fd, loc, 20, eocd_at - 20)) { free(tail); return corrupt(); }
        uint64_t zat = rd64(loc + 8);
        if (rd32(loc) != SIG_LOC64 || zat > (uint64_t)(eocd_at - 20 - 56) || q_io_pread_all(a->fd, z, 56, (int64_t)zat)
            || rd32(z) != SIG_EOCD64) { free(tail); return corrupt(); }
        multi |= rd32(z + 16) != 0 || rd32(z + 20) != 0 || rd32(loc + 4) != 0;
        count = rd64(z + 32);
        cdsize = rd64(z + 40);
        cdoff = rd64(z + 48);
        cd_end = (int64_t)zat;
    }
    free(tail);
    if (multi) return q_err(QE_NYI);
    if (cdoff > (uint64_t)cd_end || cdsize > (uint64_t)cd_end - cdoff || count > cdsize / CENTRAL_LEN) return corrupt();
    a->cdlen = (size_t)cdsize;
    a->count = (int64_t)count;
    a->cd = (uint8_t*)malloc(a->cdlen ? a->cdlen : 1);
    if (!a->cd) return q_err(QE_OOM);
    if (q_io_pread_all(a->fd, a->cd, a->cdlen, (int64_t)cdoff)) return q_err(QE_IO);
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

int q_zip_is(const char* p, size_t n) { return p && n >= 6 && memcmp(p, "zip://", 6) == 0; }

/* zip://<archive>/<entry>: *arch an owned C string, the entry a span of p */
static ray_t* zip_split(const char* p, size_t n, char** arch, const char** ent, size_t* elen) {
    for (size_t i = 6; i + 5 <= n; i++) {
        if (p[i] != '.' || (p[i + 1] | 0x20) != 'z' || (p[i + 2] | 0x20) != 'i' || (p[i + 3] | 0x20) != 'p'
            || p[i + 4] != '/') continue;
        if (i + 5 == n) return q_err(QE_DOMAIN);
        size_t al = i + 4 - 6;
        if (!(*arch = (char*)malloc(al + 1))) return q_err(QE_OOM);
        memcpy(*arch, p + 6, al);
        (*arch)[al] = '\0';
        *ent = p + i + 5;
        *elen = n - i - 5;
        return NULL;
    }
    return q_err(QE_DOMAIN);
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

/* The entry `ent` of the open archive `a` (named `arch` in errors), with *data the offset of its first byte. */
static ray_t* zip_entry(zip_arch_t* a, const char* arch, const char* ent, size_t elen, zip_ent_t* e, int64_t* data) {
    size_t at = 0;
    int64_t k = 0;
    for (; k < a->count; k++) {
        ray_t* bad = arch_next(a, &at, e);
        if (bad) return bad;
        if (e->nlen == elen && memcmp(e->name, ent, elen) == 0) break;
    }
    if (k == a->count) return zip_fail(QE_DOMAIN, "%s. %.*s has no entry %.*s", arch, strlen(arch), ent, elen);
    if (e->flags & 1) return zip_fail(QE_NYI, "%s. zip entry %.*s is encrypted%.*s", ent, elen, "", 0);
    if (e->method != 0 && e->method != 8) {
        const char* mn = method_name(e->method);
        char m[40];
        int ml = snprintf(m, sizeof m, "%u (%s)", e->method, mn ? mn : "unknown");
        return zip_fail(QE_NYI, "%s. zip method %.*s of %.*s", m, (size_t)ml, ent, elen);
    }
    if (e->method == 0 && e->csize != e->usize) return corrupt();
    uint8_t h[LOCAL_LEN];
    if (e->lho > a->fsize - LOCAL_LEN || q_io_pread_all(a->fd, h, LOCAL_LEN, e->lho) || rd32(h) != SIG_LOCAL
        || rd16(h + 8) != e->method || rd16(h + 26) != elen) return corrupt();
    char* nm = (char*)malloc(elen ? elen : 1);
    if (!nm) return q_err(QE_OOM);
    int same = q_io_pread_all(a->fd, nm, elen, e->lho + LOCAL_LEN) == 0 && memcmp(nm, ent, elen) == 0;
    free(nm);
    *data = e->lho + LOCAL_LEN + (int64_t)elen + rd16(h + 28);
    if (!same || e->csize > a->fsize - *data) return corrupt();
    return NULL;
}

/* ---- the forward stream ---- */

enum { ZS_LIVE, ZS_DONE, ZS_FAILED };

typedef struct {
    int          fd;
    uint16_t     method;
    uint32_t     crc_want, crc;
    int64_t      data, csize, usize;
    int64_t      in_pos, out_pos;           /* compressed bytes fetched; bytes produced */
    uint8_t*     in;
    size_t       in_len, in_at;
    q_gz_raw_t*  z;
    int          ended, state;
} zip_stream_t;

static void zs_close(zip_stream_t* s) {
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    free(s->in);
    s->in = NULL;
    q_gz_raw_free(s->z);
    s->z = NULL;
}

static ray_t* zs_open(const char* p, size_t n, zip_stream_t* s) {
    memset(s, 0, sizeof *s);
    s->fd = -1;
    char* arch = NULL;
    const char* ent = NULL;
    size_t elen = 0;
    ray_t* bad = zip_split(p, n, &arch, &ent, &elen);
    if (bad) return bad;
    zip_arch_t a;
    zip_ent_t e;
    bad = arch_open(arch, &a);
    if (!bad) bad = zip_entry(&a, arch, ent, elen, &e, &s->data);
    free(arch);
    free(a.cd);
    s->fd = a.fd;
    if (bad) { zs_close(s); return bad; }
    s->method = e.method;
    s->crc_want = e.crc;
    s->csize = e.csize;
    s->usize = e.usize;
    if (s->method == 8 && (!(s->in = (uint8_t*)malloc(ZIP_INBUF)) || !(s->z = q_gz_raw_new()))) {
        zs_close(s);
        return q_err(QE_OOM);
    }
    return NULL;
}

static ray_t* zs_fail(zip_stream_t* s) {
    s->state = ZS_FAILED;
    return corrupt();
}

/* one inflate step into out[0..cap), fetching input when the buffer is spent; -1 = bad or truncated data */
static int zs_inflate(zip_stream_t* s, uint8_t* out, size_t cap, size_t* made) {
    if (s->in_at == s->in_len && s->in_pos < s->csize) {
        int64_t k = s->csize - s->in_pos < (int64_t)ZIP_INBUF ? s->csize - s->in_pos : (int64_t)ZIP_INBUF;
        if (q_io_pread_all(s->fd, s->in, (size_t)k, s->data + s->in_pos)) return -1;
        s->in_pos += k;
        s->in_len = (size_t)k;
        s->in_at = 0;
    }
    size_t used = 0;
    int rc = q_gz_raw_step(s->z, s->in + s->in_at, s->in_len - s->in_at, &used, out, cap, made);
    s->in_at += used;
    if (rc < 0) return -1;
    if (rc == 1) s->ended = 1;
    else if (!used && !*made) return -1;
    return 0;
}

/* the entry's end: the stream closes exactly at usize, on exactly csize bytes, with the central CRC */
static ray_t* zs_finish(zip_stream_t* s) {
    if (s->method == 8) {
        while (!s->ended) {
            uint8_t extra;
            size_t made = 0;
            if (zs_inflate(s, &extra, 1, &made) || made) return zs_fail(s);
        }
        if (s->in_pos - (int64_t)(s->in_len - s->in_at) != s->csize) return zs_fail(s);
    }
    if (s->crc != s->crc_want) return zs_fail(s);
    s->state = ZS_DONE;
    return NULL;
}

/* up to `want` bytes into out; *got what came; the entry's last bytes are only handed over once checked */
static ray_t* zs_read(zip_stream_t* s, uint8_t* out, size_t want, size_t* got) {
    *got = 0;
    if (s->state == ZS_FAILED) return corrupt();
    while (s->state == ZS_LIVE && *got < want && s->out_pos < s->usize) {
        size_t room = want - *got;
        if ((int64_t)room > s->usize - s->out_pos) room = (size_t)(s->usize - s->out_pos);
        size_t made = 0;
        if (s->method == 0) {
            if (q_io_pread_all(s->fd, out + *got, room, s->data + s->out_pos)) return zs_fail(s);
            made = room;
        } else if (zs_inflate(s, out + *got, room, &made) || (s->ended && s->out_pos + (int64_t)made < s->usize)) {
            return zs_fail(s);
        }
        s->crc = q_gz_crc32(s->crc, out + *got, made);
        s->out_pos += (int64_t)made;
        *got += made;
    }
    return s->state == ZS_LIVE && s->out_pos == s->usize ? zs_finish(s) : NULL;
}

/* Bytes [off, off+want) of the stream from where it stands, want < 0 = to the end: a buffer grown as the entry
 * inflates, never sized from the central record alone. */
static ray_t* zs_slice(zip_stream_t* s, int64_t off, int64_t want) {
    uint8_t* buf = NULL;
    size_t len = 0, cap = 0;
    ray_t* bad = NULL;
    for (;;) {
        int skip = s->out_pos < off;
        int64_t left = skip ? off - s->out_pos : want < 0 ? INT64_MAX : want - (int64_t)len;
        if (!left) break;
        size_t step = left < ((int64_t)1 << 20) ? (size_t)left : (size_t)1 << 20;
        size_t at = skip ? 0 : len;
        if (at + step > cap) {
            size_t nc = cap ? cap * 2 : step;
            while (nc < at + step) nc *= 2;
            uint8_t* nb = (uint8_t*)realloc(buf, nc);
            if (!nb) { bad = q_err(QE_OOM); break; }
            buf = nb;
            cap = nc;
        }
        size_t got = 0;
        if ((bad = zs_read(s, buf + at, step, &got)) || !got) break;
        if (!skip) len += got;
    }
    ray_t* out = bad ? bad : ray_vec_from_raw(RAY_BYTE_ONLY, buf ? buf : (const uint8_t*)"", (int64_t)len);
    free(buf);
    return out;
}

ray_t* q_zip_read(ray_t* pathstr, int64_t off, int64_t want) {
    zip_stream_t s;
    ray_t* bad = zs_open(ray_str_ptr(pathstr), ray_str_len(pathstr), &s);
    if (bad) return bad;
    ray_t* out = zs_slice(&s, off < 0 ? 0 : off, want);
    zs_close(&s);
    return out;
}

ray_t* q_zip_size(ray_t* pathstr) {
    zip_stream_t s;
    ray_t* bad = zs_open(ray_str_ptr(pathstr), ray_str_len(pathstr), &s);
    if (bad) return bad;
    zs_close(&s);
    return ray_i64(s.usize);
}

/* ---- the handles: a reserved fd keys each open stream ---- */

typedef struct { int64_t fd; zip_stream_t s; } zip_handle_t;
static zip_handle_t* g_zh;
static int64_t g_zh_n, g_zh_cap;

ray_t* q_zip_handle_open(const char* path, size_t n, int64_t* fd) {
    if (g_zh_n == g_zh_cap) {
        int64_t nc = g_zh_cap ? g_zh_cap * 2 : 4;
        zip_handle_t* nz = (zip_handle_t*)realloc(g_zh, (size_t)nc * sizeof *nz);
        if (!nz) return q_err(QE_OOM);
        g_zh = nz;
        g_zh_cap = nc;
    }
    zip_stream_t s;
    ray_t* bad = zs_open(path, n, &s);
    if (bad) return bad;
    *fd = q_handles_reserve_fd();
    if (*fd < 0) { zs_close(&s); return q_err(QE_IO); }
    g_zh[g_zh_n++] = (zip_handle_t){ *fd, s };
    return NULL;
}

ray_t* q_zip_handle_read(int64_t fd, int64_t want) {
    for (int64_t i = 0; i < g_zh_n; i++)
        if (g_zh[i].fd == fd) return zs_slice(&g_zh[i].s, g_zh[i].s.out_pos, want);
    return NULL;
}

int q_zip_handle_close(int64_t fd) {
    for (int64_t i = 0; i < g_zh_n; i++) {
        if (g_zh[i].fd != fd) continue;
        zs_close(&g_zh[i].s);
        close((int)fd);
        g_zh[i] = g_zh[--g_zh_n];
        return 1;
    }
    return 0;
}

void q_zip_forget(void) {
    for (int64_t i = 0; i < g_zh_n; i++) {
        free(g_zh[i].s.in);
        q_gz_raw_free(g_zh[i].s.z);
    }
    g_zh_n = 0;
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
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    ray_t* xs = q_str_in(x);
    ray_t* path = q_io_path_operand(xs);
    ray_release(xs);
    if (!path) return q_err(QE_TYPE);
    zip_arch_t a;
    ray_t* bad = arch_open(ray_str_ptr(path), &a);
    ray_release(path);
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
    arch_close(&a);
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
