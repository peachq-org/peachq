/* q_tar — the tar archive (see q_tar.h).  Header layout: POSIX.1-2001 ustar (pax) — name 0..100, size 124..136,
 * checksum 148..156, typeflag 156, magic 257, prefix 345..500 — with GNU tar's 'L' long-name record and its
 * base-256 size. */
#include "qlang/q_count.h"
#include "qlang/io/q_tar.h"
#include "qlang/base/q_err.h"
#include <stdlib.h>
#include <string.h>

#define TAR_BLOCK 512

typedef struct {
    char    name[4096];
    size_t  nlen;
    int64_t size, data;
    char    type;
} tar_hdr_t;

static size_t tar_field_len(const uint8_t* f, size_t max) {
    const uint8_t* z = (const uint8_t*)memchr(f, 0, max);
    return z ? (size_t)(z - f) : max;
}

/* a numeric field: octal digits (leading spaces, ending at a space or NUL), or base-256 when the top bit is set */
static int64_t tar_num(const uint8_t* f, size_t n) {
    int64_t v = 0;
    size_t i = 0;
    if (f[0] & 0x80) {
        for (v = f[0] & 0x7f, i = 1; i < n; i++) {
            if (v > (INT64_MAX >> 8)) return -1;
            v = v << 8 | f[i];
        }
        return v;
    }
    while (i < n && f[i] == ' ') i++;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
        if (v > (INT64_MAX >> 3)) return -1;
        v = v * 8 + (f[i] - '0');
    }
    return i < n && f[i] != ' ' && f[i] != 0 ? -1 : v;
}

static void tar_name(tar_hdr_t* h, const char* p, size_t n) {
    while (n && p[n - 1] == 0) n--;
    if (n >= 2 && p[0] == '.' && p[1] == '/') p += 2, n -= 2;
    if (n >= sizeof h->name) n = sizeof h->name - 1;
    memcpy(h->name, p, n);
    h->nlen = n;
}

/* the pax record `path=` of an extended header, as the next member's name */
static int tar_pax_path(tar_hdr_t* h, const char* p, size_t n) {
    for (size_t at = 0; at < n; ) {
        size_t len = 0, k = at;
        while (k < n && p[k] >= '0' && p[k] <= '9' && len <= n) len = len * 10 + (size_t)(p[k++] - '0');
        if (len > n - at || k >= at + len || p[k] != ' ' || p[at + len - 1] != '\n') return 0;
        const char* kv = p + k + 1;
        size_t kvn = at + len - 1 - (k + 1);
        if (kvn >= 5 && memcmp(kv, "path=", 5) == 0) {
            tar_name(h, kv + 5, kvn - 5);
            return 1;
        }
        at += len;
    }
    return 0;
}

/* the bytes of one record of `n` (a long name, a pax header), the stream left past its padding */
static ray_t* tar_record(q_io_stream_t* in, int64_t n) {
    ray_t* b = q_io_stream_slice(in, in->pos, n);
    if (RAY_IS_ERR(b)) return b;
    if (q_count(b) != n) { ray_release(b); return q_err(QE_CORRUPT); }
    ray_t* pad = q_io_stream_slice(in, in->pos + (TAR_BLOCK - n % TAR_BLOCK) % TAR_BLOCK, 0);
    if (RAY_IS_ERR(pad)) { ray_release(b); return pad; }
    ray_release(pad);
    return b;
}

/* The next member's header, the stream left at its data; *more 0 at the archive's end (a zero block, or none). */
static ray_t* tar_next(q_io_stream_t* in, tar_hdr_t* h, int* more) {
    int named = 0;
    for (;;) {
        ray_t* b = q_io_stream_slice(in, in->pos, TAR_BLOCK);
        if (RAY_IS_ERR(b)) return b;
        const uint8_t* p = (const uint8_t*)ray_data(b);
        int64_t got = q_count(b), sum = 0;
        int zero = 1;
        for (int64_t i = 0; i < got; i++) {
            sum += i >= 148 && i < 156 ? ' ' : p[i];
            zero &= p[i] == 0;
        }
        if (!got || (got == TAR_BLOCK && zero)) { ray_release(b); *more = 0; return NULL; }
        int64_t size = got == TAR_BLOCK ? tar_num(p + 124, 12) : -1;
        if (size < 0 || tar_num(p + 148, 8) != sum) { ray_release(b); return q_err(QE_CORRUPT); }
        h->type = (char)p[156];
        h->size = size;
        if (h->type == 'L' || h->type == 'x') {
            ray_t* r = tar_record(in, size);
            if (!RAY_IS_ERR(r)) {
                if (h->type == 'x') named |= tar_pax_path(h, (const char*)ray_data(r), (size_t)size);
                else tar_name(h, (const char*)ray_data(r), (size_t)size), named = 1;
            }
            ray_release(b);
            if (RAY_IS_ERR(r)) return r;
            ray_release(r);
            continue;
        }
        if (!named) {
            char full[TAR_BLOCK];
            size_t pn = 0, nn = tar_field_len(p, 100);
            if (memcmp(p + 257, "ustar\0", 6) == 0 && p[345]) {
                pn = tar_field_len(p + 345, 155);
                memcpy(full, p + 345, pn);
                full[pn++] = '/';
            }
            memcpy(full + pn, p, nn);
            tar_name(h, full, pn + nn);
        }
        ray_release(b);
        h->data = in->pos;
        *more = 1;
        return NULL;
    }
}

static int tar_regular(const tar_hdr_t* h) { return h->type == '0' || h->type == 0 || h->type == '7'; }

/* past the current member's data and padding */
static ray_t* tar_skip(q_io_stream_t* in, const tar_hdr_t* h) {
    ray_t* b = q_io_stream_slice(in, h->data + (h->size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK, 0);
    if (RAY_IS_ERR(b)) return b;
    ray_release(b);
    return NULL;
}

static ray_t* tar_open(ray_t* arch, int gz, q_io_stream_t** in) {
    q_io_stream_t* r = q_io_range_stream(arch, 0, -1);
    if (!r) return q_err(QE_OOM);
    if (!gz) { *in = r; return NULL; }
    return q_io_gunzip_stream(r, in);
}

ray_t* q_tar_member(ray_t* arch, int gz, const char* m, size_t mn, q_io_stream_t** s, int64_t* size) {
    *s = NULL;
    q_io_stream_t* in;
    ray_t* bad = tar_open(arch, gz, &in);
    if (bad) return bad;
    tar_hdr_t h;
    for (int more = 1; !bad; ) {
        if ((bad = tar_next(in, &h, &more)) || !more) break;
        if (tar_regular(&h) && h.nlen == mn && memcmp(h.name, m, mn) == 0) {
            *size = h.size;
            if (!gz) {
                q_io_stream_close(in);
                *s = q_io_range_stream(arch, h.data, h.size);
                return *s ? NULL : q_err(QE_OOM);
            }
            *s = q_io_window_stream(in, h.size);
            return *s ? NULL : q_err(QE_OOM);
        }
        bad = tar_skip(in, &h);
    }
    q_io_stream_close(in);
    return bad ? bad : q_io_no_member(arch, m, mn);
}

ray_t* q_tar_names(ray_t* arch, int gz) {
    q_io_stream_t* in;
    ray_t* bad = tar_open(arch, gz, &in);
    if (bad) return bad;
    ray_t* out = ray_list_new(4);
    tar_hdr_t h;
    for (int more = 1; !bad; ) {
        if ((bad = tar_next(in, &h, &more)) || !more) break;
        if (tar_regular(&h)) {
            ray_t* nm = ray_charv(h.name, (int64_t)h.nlen);
            out = ray_list_append(out, nm);
            ray_release(nm);
        }
        bad = tar_skip(in, &h);
    }
    q_io_stream_close(in);
    if (!bad) return out;
    ray_release(out);
    return bad;
}
