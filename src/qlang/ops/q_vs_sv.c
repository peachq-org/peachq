/* ops/q_vs_sv.c — the q `vs` / `sv` split-join / base-encode family, a
 * graduated family home like q_bang.c / q_dollar.c.  Evicted from
 * ops/q_math.c (2026-07-22). */
#include "qlang/q_registry_internal.h" /* wrap decls + q_registry.h (q_str_in/q_str_charv_out) */
#include "qlang/base/q_err.h"
#include "lang/internal.h" /* ray_error */
#include "table/sym.h"     /* ray_sym_str, ray_sym_vec_cell, ray_sym_intern_runtime */
#include <string.h>        /* memcmp, memcpy */
#include <stdlib.h>        /* malloc, free */


/* ===== q `vs` / `sv` — split-join / base-encode family ===================
 * kdb reference vs.md / sv.md.  Both are strictly dyadic.  Native -RAY_STR
 * strings (split -> boxed list of string atoms, join -> one string atom),
 * symbol split/join, integer base decompose/compose (atom + vector base),
 * and big-endian byte/bit encode/decode.  Genuinely out-of-scope forms
 * (128-bit GUID compose, `1:` reparse, byte-vector base) return 'nyi. */

/* split string y on substring sep -> boxed list of -RAY_STR (keeps empties) */
static ray_t* str_split(const char* y, size_t yl, const char* sep, size_t sl) {
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    if (sl == 0) {                                 /* empty sep -> one piece */
        ray_t* s = ray_str(y, yl);
        out = ray_list_append(out, s); ray_release(s);
        return out;
    }
    size_t seg = 0;
    for (size_t i = 0; i + sl <= yl; ) {
        if (memcmp(y + i, sep, sl) == 0) {
            ray_t* s = ray_str(y + seg, i - seg);
            out = ray_list_append(out, s); ray_release(s);
            if (RAY_IS_ERR(out)) return out;
            i += sl; seg = i;
        } else i++;
    }
    ray_t* last = ray_str(y + seg, yl - seg);
    out = ray_list_append(out, last); ray_release(last);
    return out;
}

/* ` vs `sym — split a symbol: leading ':' (file handle) splits at the LAST
 * '/' into (dir; file); otherwise split on every '.'.  -> RAY_SYM vector. */
static ray_t* sym_split(ray_t* y) {
    ray_t* s = ray_sym_str(y->i64);
    if (!s) return q_err(QE_TYPE);
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, 4);
    if (n > 0 && p[0] == ':') {                    /* file handle: last '/' */
        size_t cut = n;
        for (size_t i = n; i-- > 0; ) if (p[i] == '/') { cut = i; break; }
        if (cut == n) {                            /* no '/', single element */
            int64_t id = ray_sym_intern_runtime(p, n);
            out = ray_vec_append(out, &id);
        } else {
            int64_t a = ray_sym_intern_runtime(p, cut);
            int64_t b = ray_sym_intern_runtime(p + cut + 1, n - cut - 1);
            out = ray_vec_append(out, &a);
            out = ray_vec_append(out, &b);
        }
    } else {                                        /* split all '.' */
        size_t seg = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i == n || p[i] == '.') {
                int64_t id = ray_sym_intern_runtime(p + seg, i - seg);
                out = ray_vec_append(out, &id);
                seg = i + 1;
            }
        }
    }
    return out;
}

/* big-endian byte encode of a numeric scalar (0x0 vs y) -> U8 vector */
static ray_t* byte_encode(ray_t* y) {
    uint8_t b[8]; int w = 0; uint64_t bits = 0;
    switch (y->type) {
    case -RAY_I16: w = 2; bits = (uint16_t)y->i16; break;
    case -RAY_I32: w = 4; bits = (uint32_t)y->i32; break;
    case -RAY_I64: w = 8; bits = (uint64_t)y->i64; break;
    case -RAY_F32: { float f = (float)y->f64; uint32_t u; memcpy(&u, &f, 4);
                     w = 4; bits = u; break; }
    case -RAY_F64: { double d = y->f64; uint64_t u; memcpy(&u, &d, 8);
                     w = 8; bits = u; break; }
    default: return q_err(QE_TYPE);
    }
    for (int i = 0; i < w; i++) b[i] = (uint8_t)(bits >> (8 * (w - 1 - i)));
    return ray_vec_from_raw(RAY_BYTE_ONLY, b, w);
}

/* big-endian bit decompose of an integer scalar (0b vs y) -> BOOL vector */
static ray_t* bit_decompose(ray_t* y) {
    int w = 0; uint64_t bits = 0;
    switch (y->type) {
    case -RAY_BOOL: w = 1;  bits = y->b8 ? 1 : 0; break;
    case -RAY_BYTE_ONLY: w = 8;  bits = (uint8_t)y->u8; break;
    case -RAY_I16:  w = 16; bits = (uint16_t)y->i16; break;
    case -RAY_I32:  w = 32; bits = (uint32_t)y->i32; break;
    case -RAY_I64:  w = 64; bits = (uint64_t)y->i64; break;
    default: return q_err(QE_TYPE);
    }
    uint8_t stackb[64];
    for (int i = 0; i < w; i++) stackb[i] = (uint8_t)((bits >> (w - 1 - i)) & 1);
    return ray_vec_from_raw(RAY_BOOL, stackb, w);
}

/* decompose scalar v into minimal base-`base` digits (>=1) -> long vector */
static ray_t* base_decompose_atom(int64_t base, int64_t v) {
    if (base <= 0) return q_err(QE_DOMAIN);
    int64_t buf[64]; int n = 0;
    uint64_t u = (uint64_t)v;
    if (u == 0) buf[n++] = 0;
    while (u > 0 && n < 64) { buf[n++] = (int64_t)(u % (uint64_t)base); u /= (uint64_t)base; }
    ray_t* out = ray_vec_new(RAY_I64, n);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    for (int i = 0; i < n; i++) d[i] = buf[n - 1 - i];   /* MSB first */
    return out;
}

/* mixed-radix decompose scalar v by vector base -> long vector len(base) */
static ray_t* base_decompose_vec(ray_t* base, int64_t v) {
    int64_t n = ray_len(base);
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    uint64_t u = (uint64_t)v;
    for (int64_t i = n - 1; i >= 0; i--) {
        int64_t bi = q_type_ivec_get(base, i);
        if (bi <= 0) { d[i] = (int64_t)u; u = 0; }
        else { d[i] = (int64_t)(u % (uint64_t)bi); u /= (uint64_t)bi; }
    }
    return out;
}

static ray_t* vs_impl(ray_t* x, ray_t* y);
ray_t* q_vs_wrap(ray_t* x, ray_t* y) {
    /* charv args ride the legacy -RAY_STR body; results cross back as charv */
    ray_t* xs = q_str_in(x); ray_t* ys = q_str_in(y);
    if (xs != x || ys != y) {
        ray_t* r = vs_impl(xs, ys);
        ray_release(xs); ray_release(ys);
        return q_str_charv_out(r);
    }
    ray_release(xs); ray_release(ys);
    return vs_impl(x, y);
}
static ray_t* vs_impl(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    /* --- string / newline split --- */
    if (x->type == -RAY_STR) {
        if (y->type != -RAY_STR)
            return q_err(QE_NYI);
        return str_split(ray_str_ptr(y), ray_str_len(y),
                           ray_str_ptr(x), ray_str_len(x));
    }
    if (q_type_is_null_sym(x)) {
        if (y->type == -RAY_STR)
            return q_str_split_lines(ray_str_ptr(y), ray_str_len(y));
        if (y->type == -RAY_SYM) return sym_split(y);
        return q_err(QE_TYPE);
    }
    /* --- byte encode (0x0 vs scalar) --- */
    if (x->type == -RAY_BYTE_ONLY) {
        if (ray_is_atom(y) && y->type != -RAY_STR) return byte_encode(y);
        return q_err(QE_NYI);
    }
    /* --- bit decompose (0b vs scalar) --- */
    if (x->type == -RAY_BOOL) {
        if (ray_is_atom(y)) return bit_decompose(y);
        return q_err(QE_TYPE);
    }
    /* --- integer base decompose --- */
    if (q_type_is_int_atom(x)) {
        int64_t base = q_type_iatom_val(x);
        if (q_type_is_int_atom(y)) return base_decompose_atom(base, q_type_iatom_val(y));
        if (q_type_is_int_vec(y)) {                     /* matrix: pad to max width */
            int64_t m = ray_len(y);
            ray_t* cols = ray_list_new(m > 0 ? m : 1);
            int64_t maxw = 1;
            for (int64_t j = 0; j < m; j++) {
                ray_t* c = base_decompose_atom(base, q_type_ivec_get(y, j));
                if (RAY_IS_ERR(c)) { ray_release(cols); return c; }
                if (ray_len(c) > maxw) maxw = ray_len(c);
                cols = ray_list_append(cols, c); ray_release(c);
            }
            ray_t* rows = ray_list_new(maxw);
            ray_t** cv = (ray_t**)ray_data(cols);
            for (int64_t r = 0; r < maxw; r++) {
                ray_t* row = ray_vec_new(RAY_I64, m); row->len = m;
                int64_t* rd = (int64_t*)ray_data(row);
                for (int64_t j = 0; j < m; j++) {
                    int64_t cw = ray_len(cv[j]);
                    int64_t pad = maxw - cw;         /* left-pad with 0 */
                    rd[j] = (r < pad) ? 0 : ((const int64_t*)ray_data(cv[j]))[r - pad];
                }
                rows = ray_list_append(rows, row); ray_release(row);
            }
            ray_release(cols);
            return rows;
        }
        return q_err(QE_TYPE);
    }
    if (q_type_is_int_vec(x)) {
        if (q_type_is_int_atom(y)) return base_decompose_vec(x, q_type_iatom_val(y));
        return q_err(QE_NYI);
    }
    return q_err(QE_TYPE);
}

/* join a boxed list / vector of strings with separator sep (append trailing
 * when host==1, the ` sv newline form). */
static ray_t* str_join(ray_t* y, const char* sep, size_t sl, int host) {
    if (!y || y->type != RAY_LIST)
        return q_err(QE_TYPE);
    int64_t n = ray_len(y);
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = (y->type == RAY_LIST) ? ((ray_t**)ray_data(y))[i] : NULL;
        if (!e || e->type != -RAY_STR)
            return q_err(QE_TYPE);
        total += ray_str_len(e);
        if (i + 1 < n) total += sl;
    }
    if (host) total += 1;
    char* buf = malloc(total ? total : 1);
    if (!buf) return q_err(QE_WSFULL);
    size_t w = 0;
    ray_t** ev = (ray_t**)ray_data(y);
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = ev[i];
        size_t el = ray_str_len(e);
        memcpy(buf + w, ray_str_ptr(e), el); w += el;
        if (host) { buf[w++] = '\n'; }
        else if (i + 1 < n) { memcpy(buf + w, sep, sl); w += sl; }
    }
    ray_t* r = ray_str(buf, w);
    free(buf);
    return r;
}

/* ` sv `syms — join symbols: leading ':' (file handle) joins with '/', else
 * with '.'  -> single -RAY_SYM atom. */
static ray_t* sym_join(ray_t* y) {
    int64_t n = ray_len(y);
    if (n == 0) return ray_sym(ray_sym_intern_runtime("", 0));
    ray_t* first = ray_sym_vec_cell(y, 0);
    const char* fp = first ? ray_str_ptr(first) : "";
    char joiner = (ray_str_len(first) > 0 && fp[0] == ':') ? '/' : '.';
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* c = ray_sym_vec_cell(y, i);
        total += ray_str_len(c);
        if (i + 1 < n) total += 1;
    }
    char* buf = malloc(total ? total : 1);
    if (!buf) return q_err(QE_WSFULL);
    size_t w = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* c = ray_sym_vec_cell(y, i);
        size_t cl = ray_str_len(c);
        memcpy(buf + w, ray_str_ptr(c), cl); w += cl;
        if (i + 1 < n) buf[w++] = joiner;
    }
    int64_t id = ray_sym_intern_runtime(buf, w);
    free(buf);
    return ray_sym(id);
}

/* big-endian byte decode: interpret a U8 vector as a signed integer of the
 * matching width (2->short, 4->int, 8->long). */
static ray_t* byte_decode(ray_t* y) {
    int64_t n = ray_len(y);
    const uint8_t* p = (const uint8_t*)ray_data(y);
    uint64_t v = 0;
    for (int64_t i = 0; i < n; i++) v = (v << 8) | p[i];
    if (n == 2) return ray_i16((int16_t)(uint16_t)v);
    if (n == 4) return ray_i32((int32_t)(uint32_t)v);
    if (n == 8) return ray_i64((int64_t)v);
    if (n == 1) return ray_i16((int16_t)(uint8_t)v);
    return q_err(QE_NYI);
}

/* bits -> integer (8->byte, 16->short, 32->int, 64->long; 128->guid deferred) */
static ray_t* bit_compose(ray_t* y) {
    int64_t n = ray_len(y);
    const uint8_t* p = (const uint8_t*)ray_data(y);
    if (n == 128) return q_err(QE_NYI);
    if (n != 8 && n != 16 && n != 32 && n != 64)
        return q_err(QE_NYI);
    uint64_t v = 0;
    for (int64_t i = 0; i < n; i++) v = (v << 1) | (p[i] & 1);
    if (n == 8)  return ray_u8((uint8_t)v);
    if (n == 16) return ray_i16((int16_t)(uint16_t)v);
    if (n == 32) return ray_i32((int32_t)(uint32_t)v);
    return ray_i64((int64_t)v);
}

static ray_t* sv_impl(ray_t* x, ray_t* y);
ray_t* q_sv_wrap(ray_t* x, ray_t* y) {
    ray_t* xs = q_str_in(x); ray_t* ys = q_str_in(y);
    if (xs != x || ys != y) {
        ray_t* r = sv_impl(xs, ys);
        ray_release(xs); ray_release(ys);
        return q_str_charv_out(r);
    }
    ray_release(xs); ray_release(ys);
    return sv_impl(x, y);
}
static ray_t* sv_impl(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    /* --- string join --- */
    if (x->type == -RAY_STR)
        return str_join(y, ray_str_ptr(x), ray_str_len(x), 0);
    if (q_type_is_null_sym(x)) {
        if (y->type == RAY_SYM) return sym_join(y);            /* sym join */
        return str_join(y, "\n", 1, 1);                        /* host lines */
    }
    /* --- byte decode (0x0 sv bytes) --- */
    if (x->type == -RAY_BYTE_ONLY) {
        if (y->type == RAY_BYTE_ONLY) return byte_decode(y);
        return q_err(QE_NYI);
    }
    /* --- bit compose (0b sv bits) --- */
    if (x->type == -RAY_BOOL) {
        if (y->type == RAY_BOOL) return bit_compose(y);
        return q_err(QE_TYPE);
    }
    /* --- integer base compose (Horner) --- */
    if (q_type_is_int_atom(x)) {
        int64_t base = q_type_iatom_val(x);
        if (!q_type_is_int_vec(y) && y->type != RAY_BOOL)
            return q_err(QE_TYPE);
        int64_t n = ray_len(y);
        int64_t acc = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t d = (y->type == RAY_BOOL) ? ((const uint8_t*)ray_data(y))[i]
                                              : q_type_ivec_get(y, i);
            acc = acc * base + d;
        }
        return ray_i64(acc);
    }
    /* --- mixed-radix compose (vector base) --- */
    if (q_type_is_int_vec(x)) {
        if (!q_type_is_int_vec(y)) return q_err(QE_TYPE);
        int64_t n = ray_len(y), bn = ray_len(x);
        if (n != bn) return q_err(QE_LENGTH);
        int64_t acc = 0;
        for (int64_t i = 0; i < n; i++)
            acc = acc * q_type_ivec_get(x, i) + q_type_ivec_get(y, i);
        return ray_i64(acc);
    }
    return q_err(QE_TYPE);
}
