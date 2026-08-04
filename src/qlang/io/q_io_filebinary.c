/* io/q_io_filebinary.c — the q `1:` File Binary surface: Read Binary and the
 * plain Save Binary.  Oracle: ref/file-binary.md (CLEAN ROOM).  A second front
 * door on the byte core (io/q_io.h), not a second file layer: every file form
 * of `y` goes through q_io_file_triple + q_io_read_slice, inheriting offset
 * clamping, decompression and the restricted gate.  The type alphabet is
 * `0:`'s through the cast home and nothing else is — `0:` TOKENIZES text
 * (upper case), `1:` REINTERPRETS bytes (lower case).  Deferred 'nyi: the
 * anymap write and the 4-item compressed write.  Inputs borrowed. */
#include "qlang/q_registry_internal.h"  /* the wrapper's own declaration */
#include "qlang/base/q_err.h"
#include "qlang/io/q_io.h"
#include "qlang/ops/q_dollar.h"  /* q_cast_designator, q_dollar_cast */
#include "lang/eval.h"           /* ray_eval_get_restricted — compressed save */
#include <stdlib.h>
#include <string.h>

/* A column recipe: where its field sits in the record and how to read it. */
enum { FB_TYPED, FB_TEXT, FB_SYM, FB_SKIP };
typedef struct { int8_t tag; int kind; int64_t width, off; } fb_col_t;

/* One type char + width -> a recipe (NULL ok, else an owned error).  The doc's
 * "Column types and widths" table PAIRS each type with its width, so a typed
 * column disagreeing with its datatype's size is out of domain. */
static ray_t* fb_recipe(char c, int64_t w, fb_col_t* out) {
    out->tag = 0;
    out->width = w;
    if (w <= 0) return q_err(QE_DOMAIN);
    if (c == ' ') { out->kind = FB_SKIP; return NULL; }
    if (c == '*') { out->kind = FB_TEXT; return NULL; }
    if (c == 's') { out->kind = FB_SYM;  return NULL; }
    if (c < 'a' || c > 'z') return q_err(QE_TYPE);          /* doc: lower case */
    ray_t* d = ray_str(&c, 1);
    if (!d || RAY_IS_ERR(d)) return q_err(QE_OOM);
    int is_tok = 0;
    int8_t tag = q_cast_designator(d, &is_tok, NULL);
    ray_release(d);
    if (!tag) return q_err(QE_TYPE);
    if (ray_type_sizes[(uint8_t)tag] != w) return q_err(QE_DOMAIN);
    out->tag = tag;
    out->kind = FB_TYPED;
    return NULL;
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define FB_HOST_BE 1
#else
#define FB_HOST_BE 0
#endif

/* A typed column: the field at c->off of every record.  `x` names the DATA's
 * byte order, so the swap is data-vs-HOST (net/q_wire.c's frame-vs-host rule),
 * and a guid is 16 raw bytes rather than a scalar, so order never reaches it. */
static ray_t* fb_gather(const uint8_t* base, int64_t nrec, int64_t reclen,
                        const fb_col_t* c, int big) {
    uint8_t esz = ray_type_sizes[(uint8_t)c->tag];
    int swap = big != FB_HOST_BE && esz > 1 && c->tag != RAY_GUID;
    uint8_t* buf = (uint8_t*)malloc((size_t)(nrec > 0 ? nrec : 1) * esz);
    if (!buf) return q_err(QE_OOM);
    for (int64_t i = 0; i < nrec; i++) {
        const uint8_t* src = base + i * reclen + c->off;
        uint8_t* dst = buf + i * esz;
        if (swap) for (uint8_t k = 0; k < esz; k++) dst[k] = src[esz - 1 - k];
        else memcpy(dst, src, esz);
    }
    ray_t* v = ray_vec_from_raw(c->tag, buf, nrec);
    free(buf);
    return v;
}

/* The two text columns cut the same field: '*' keeps it verbatim, 's' trims
 * the NUL/space padding and hands the strings to the cast home. */
static ray_t* fb_text_col(const uint8_t* base, int64_t nrec, int64_t reclen,
                          const fb_col_t* c) {
    int sym = c->kind == FB_SYM;
    ray_t* out = ray_list_new(nrec > 0 ? nrec : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < nrec; i++) {
        const char* p = (const char*)base + i * reclen + c->off;
        int64_t n = c->width;
        if (sym) while (n && (p[n - 1] == '\0' || p[n - 1] == ' ')) n--;
        ray_t* s = ray_charv(p, n);
        if (!s || RAY_IS_ERR(s)) { ray_release(out); return s ? s : q_err(QE_OOM); }
        out = ray_list_append(out, s);
        ray_release(s);
        if (RAY_IS_ERR(out)) return out;
    }
    if (!sym) return out;
    ray_t* v = q_dollar_cast(RAY_SYM, out);
    ray_release(out);
    return v;
}

/* THE read core: `nb` bytes of fixed-length records cut into a matrix by the
 * recipes.  Every `y` door lands here. */
static ray_t* fb_decode(const char* ts, int64_t nt, ray_t* widths,
                        const uint8_t* base, int64_t nb, int big) {
    if (nt == 0 || nt != ray_len(widths)) return q_err(QE_LENGTH);
    fb_col_t* c = (fb_col_t*)malloc((size_t)nt * sizeof *c);
    if (!c) return q_err(QE_OOM);
    int64_t reclen = 0, nout = 0;
    for (int64_t j = 0; j < nt; j++) {
        ray_t* bad = fb_recipe(ts[j], q_type_ivec_get(widths, j), &c[j]);
        if (bad) { free(c); return bad; }
        c[j].off = reclen;
        reclen += c[j].width;
        if (c[j].kind != FB_SKIP) nout++;
    }
    int64_t nrec = nb / reclen;
    ray_t* out = ray_list_new(nout > 0 ? nout : 1);
    for (int64_t j = 0; j < nt && !RAY_IS_ERR(out); j++) {
        if (c[j].kind == FB_SKIP) continue;
        ray_t* col = c[j].kind == FB_TYPED ? fb_gather(base, nrec, reclen, &c[j], big)
                                           : fb_text_col(base, nrec, reclen, &c[j]);
        if (!col || RAY_IS_ERR(col)) {
            ray_release(out);
            out = col ? col : q_err(QE_OOM);
            break;
        }
        out = ray_list_append(out, col);
        ray_release(col);
    }
    free(c);
    return out;
}

/* The four `y` doors as one 1-byte-lane vector. */
static ray_t* fb_payload(ray_t* y) {
    if (!y) return q_err(QE_TYPE);
    if (y->type == -RAY_SYM) {
        ray_t* path = q_io_file_path(y);
        if (!path) return q_err(QE_TYPE);
        ray_t* b = q_io_read_slice(path, 0, -1, NULL);
        ray_release(path);
        return b;
    }
    if (y->type == RAY_LIST) {
        int64_t n = ray_len(y);
        ray_t** e = (ray_t**)ray_data(y);
        if (n >= 2 && n <= 3 && e[0] && e[0]->type == -RAY_SYM) {
            ray_t* path;
            int64_t off, want;
            ray_t* bad = q_io_file_triple(e[0], e[1], n == 3 ? e[2] : NULL, 1,
                                          &path, &off, &want);
            if (bad) return bad;
            ray_t* b = q_io_read_slice(path, off, want, NULL);
            ray_release(path);
            return b;
        }
    }
    const char* p;
    int64_t n;
    if (q_str_text_bytes(y, &p, &n)) return ray_vec_from_raw(RAY_BYTE_ONLY, p, n);
    if (y->type == RAY_BYTE_ONLY) { ray_retain(y); return y; }
    return q_err(QE_TYPE);
}

/* Read Binary.  `x` is (types;widths) or (widths;types) and the ORDER is the
 * byte order: types first is little-endian, widths first is big-endian. */
static ray_t* fb_read(ray_t* x0, ray_t* x1, ray_t* y) {
    const char* ts = NULL;
    int64_t nt = 0;
    ray_t* widths;
    int big;
    /* "a string of types" (ref/file-binary.md): a char ATOM is not one, and q
     * does not promote it — the doc spells every 1-column case enlist"i". */
    if (q_type_is_char_atom(x0) || q_type_is_char_atom(x1)) return q_err(QE_TYPE);
    if (q_type_is_int_vec(x0) && q_str_text_bytes(x1, &ts, &nt))      { widths = x0; big = 1; }
    else if (q_str_text_bytes(x0, &ts, &nt) && q_type_is_int_vec(x1)) { widths = x1; big = 0; }
    else return q_err(QE_TYPE);
    ray_t* pl = fb_payload(y);
    if (!pl || RAY_IS_ERR(pl)) return pl ? pl : q_err(QE_OOM);
    ray_t* r = fb_decode(ts, nt, widths, (const uint8_t*)ray_data(pl), ray_len(pl), big);
    ray_release(pl);
    return r;
}

/* Save Binary: the raw bytes of `y`, the file symbol back.  An atom is its own
 * one-element vector, so `enlist` supplies the payload; anything with no fixed
 * element layout is the deferred anymap form. */
static ray_t* fb_save(ray_t* fsym, ray_t* y) {
    if (!y) return q_err(QE_TYPE);
    ray_t* path = q_io_file_path(fsym);
    if (!path) return q_err(QE_TYPE);
    ray_t* bad;
    const char* p;
    int64_t n;
    if (q_str_text_bytes(y, &p, &n)) {
        bad = q_io_write_all(path, p, (size_t)n);
    } else {
        ray_t* v = y;
        if (ray_is_atom(y)) v = q_enlist_wrap(&y, 1);
        else ray_retain(v);
        if (!v || RAY_IS_ERR(v)) { ray_release(path); return v ? v : q_err(QE_OOM); }
        uint8_t esz = v->type > 0 ? ray_type_sizes[(uint8_t)v->type] : 0;
        if (!esz || v->type == RAY_STR || v->type == RAY_SYM) {
            ray_release(v);
            ray_release(path);
            return q_err(QE_NYI);
        }
        bad = q_io_write_all(path, ray_data(v), (size_t)(ray_len(v) * esz));
        ray_release(v);
    }
    ray_release(path);
    if (bad) return bad;
    ray_retain(fsym);
    return fsym;
}

ray_t* q_io_filebinary_wrap(ray_t* x, ray_t* y) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == -RAY_SYM) return fb_save(x, y);
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (n == 2) return fb_read(e[0], e[1], y);
        /* (file;blockSize;algorithm;level) — ref/file-binary.md § Compression:
         * Save Binary's raw bytes through the container. */
        if (n == 4 && e[0] && e[0]->type == -RAY_SYM) {
            int64_t lbs, alg, lvl;
            if (!q_type_strict_i64(e[1], &lbs) || !q_type_strict_i64(e[2], &alg) ||
                !q_type_strict_i64(e[3], &lvl))
                return q_err(QE_TYPE);
            if (alg == 1 || alg == 3 || alg == 4 || alg == 5) return q_err(QE_NYI);
            if (alg == 0) return fb_save(e[0], y);
            if (!y || ray_is_atom(y) || y->type != RAY_BYTE_ONLY)
                return q_err(QE_NYI);            /* raw-byte payloads only */
            ray_t* path = q_io_file_path(e[0]);
            if (!path) return q_err(QE_TYPE);
            ray_t* bad = ray_eval_get_restricted() ? q_err(QE_ACCESS)
                       : q_io_zip_write(path, (const uint8_t*)ray_data(y),
                                        (size_t)ray_len(y), (int)lbs, (int)alg, (int)lvl);
            ray_release(path);
            if (bad) return bad;
            ray_retain(e[0]);
            return e[0];
        }
    }
    return q_err(QE_TYPE);
}
