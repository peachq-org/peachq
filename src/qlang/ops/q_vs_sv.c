/* ops/q_vs_sv.c — the q `vs` / `sv` split-join / base-encode family (ref/vs.md, ref/sv.md), a graduated family home
 * like q_bang.c / q_dollar.c.  Partition and join are lane-uniform over chars and bytes with the result lane taken
 * from x ("type inferred from left hand side"); base-x encode/decode take integer-kinded y — byte and boolean
 * included — and the matrix forms are the per-item / per-column composition of the atom forms. */
#include "qlang/q_count.h"
#include "qlang/q_registry_internal.h" /* wrap decls + q_prim.h (q_str_text_bytes, q_str_split_lines, q_str_charv_out) */
#include "qlang/base/q_err.h"
#include "table/sym.h"     /* ray_sym_str, ray_sym_vec_cell, ray_sym_intern_runtime */
#include <string.h>        /* memcmp, memcpy */
#include <stdlib.h>        /* malloc, free */

/* ---- the byte lanes: RAY_BYTE_ONLY for the byte type, RAY_CHARV for text, 0 for neither ---- */
static int8_t lane_of(ray_t* x) {
    if (!x) return 0;
    if (x->type == RAY_BYTE_ONLY || x->type == -RAY_BYTE_ONLY) return RAY_BYTE_ONLY;
    if (x->type == RAY_CHARV || x->type == -RAY_CHARV || x->type == -RAY_STR) return RAY_CHARV;
    return 0;
}

static ray_t* lane_new(int8_t lane, const char* p, int64_t n) {
    return lane == RAY_BYTE_ONLY ? ray_vec_from_raw(RAY_BYTE_ONLY, p, n) : ray_charv(p, n);
}

/* the bytes behind a lane value; a lane atom is one byte */
static bool lane_bytes(ray_t* v, const char** p, int64_t* n) {
    if (!lane_of(v)) return false;
    if (v->type == -RAY_BYTE_ONLY) { *p = (const char*)&v->u8; *n = 1; return true; }
    if (v->type == RAY_BYTE_ONLY)  { *p = (const char*)ray_data(v); *n = q_count(v); return true; }
    return q_str_text_bytes(v, p, n);
}

/* a lane VECTOR (an internal -RAY_STR atom is a string) */
static bool lane_vec_bytes(ray_t* v, const char** p, int64_t* n) {
    return lane_bytes(v, p, n) && (v->type > 0 || v->type == -RAY_STR);
}

/* ---- integer-kinded y for base-x: the int lanes plus byte and boolean (tracked aoc programs ran `2 vs 0x05`,
 * `256 sv 0x...`, `2 sv` over booleans on kdb); local so bytes never leak into til/#/indexing ---- */
static bool ikind_atom(ray_t* v, int64_t* out) {
    if (q_type_is_int_atom(v)) { *out = q_type_iatom_val(v); return true; }
    if (!v) return false;
    switch (v->type) {
    case -RAY_BOOL: *out = v->b8 ? 1 : 0; return true;
    RAY_BYTE_ATOM_CASES: *out = v->u8; return true;
    default: return false;
    }
}

static bool ikind_vec(ray_t* v) {
    return q_type_is_int_vec(v) || (v && (v->type == RAY_BOOL || ray_is_bytelike(v->type)));
}

static int64_t ikind_get(ray_t* v, int64_t i) {
    return q_type_is_int_vec(v) ? q_type_ivec_get(v, i) : ((const uint8_t*)ray_data(v))[i];
}

/* split y on the byte string sep -> list of lane pieces (keeps empties; an empty sep is one piece) */
static ray_t* str_split(const char* y, int64_t yl, const char* sep, int64_t sl, int8_t lane) {
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    int64_t seg = 0;
    for (int64_t i = 0; sl > 0 && i + sl <= yl; ) {
        if (memcmp(y + i, sep, (size_t)sl) == 0) {
            ray_t* s = lane_new(lane, y + seg, i - seg);
            out = ray_list_append(out, s); ray_release(s);
            if (RAY_IS_ERR(out)) return out;
            i += sl; seg = i;
        } else i++;
    }
    ray_t* last = lane_new(lane, y + seg, yl - seg);
    out = ray_list_append(out, last); ray_release(last);
    return out;
}

/* ` vs `sym — split a symbol: a file handle (leading ':') into (dir; file) at
 * the LAST '/', `:. the dir when there is none (owner ruling 2026-09-17);
 * otherwise on every '.'.  -> RAY_SYM vector. */
static ray_t* sym_split(ray_t* y) {
    ray_t* s = ray_sym_str(y->i64);
    if (!s) return q_err(QE_TYPE);
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, 4);
    if (n > 0 && p[0] == ':') {                    /* file handle: last '/' */
        size_t cut = n;
        for (size_t i = n; i-- > 0; ) if (p[i] == '/') { cut = i; break; }
        int64_t a = cut == n ? ray_sym_intern_runtime(":.", 2) : ray_sym_intern_runtime(p, cut);
        int64_t b = cut == n ? ray_sym_intern_runtime(p + 1, n - 1)
                             : ray_sym_intern_runtime(p + cut + 1, n - cut - 1);
        out = ray_vec_append(out, &a);
        out = ray_vec_append(out, &b);
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
    RAY_BYTE_ATOM_CASES: w = 8;  bits = (uint8_t)y->u8; break;
    case -RAY_I16:  w = 16; bits = (uint16_t)y->i16; break;
    case -RAY_I32:  w = 32; bits = (uint32_t)y->i32; break;
    case -RAY_I64:  w = 64; bits = (uint64_t)y->i64; break;
    default: return q_err(QE_TYPE);
    }
    uint8_t stackb[64];
    for (int i = 0; i < w; i++) stackb[i] = (uint8_t)((bits >> (w - 1 - i)) & 1);
    return ray_vec_from_raw(RAY_BOOL, stackb, w);
}

/* ---- base-x encode: the widest leaf's digit count is fixed FIRST, so every leaf decomposes to the same width and
 * the rows of a list need no structural padding ---- */
static int64_t width_of(int64_t base, int64_t v) {
    int64_t n = (v == 0);
    for (uint64_t u = (uint64_t)v; u > 0; u /= (uint64_t)base) n++;
    return n;
}

/* an atom base needs v's digit count; a vector base is one digit per radix */
static int64_t leaf_width(ray_t* x, int64_t v) {
    return ray_is_atom(x) ? width_of(q_type_iatom_val(x), v) : q_count(x);
}

/* the width base x needs for y (ints, byte/bool, or any nesting of them); -1 when y is not integer-kinded */
static int64_t max_width(ray_t* x, ray_t* y) {
    int64_t v, w = 0;
    if (ikind_atom(y, &v)) return leaf_width(x, v);
    if (!y || (!ikind_vec(y) && y->type != RAY_LIST)) return -1;
    for (int64_t j = 0; j < q_count(y); j++) {
        int64_t wj = y->type == RAY_LIST ? max_width(x, ((ray_t**)ray_data(y))[j]) : leaf_width(x, ikind_get(y, j));
        if (wj < 0) return -1;
        if (wj > w) w = wj;
    }
    return w;
}

/* digits of v, MSB first: w digits of an atom base, or one digit per item of a vector base (mixed radix; a
 * non-positive radix takes the whole remainder, so `0 24 60 60` leaves the days unbounded) */
static ray_t* digits_of(ray_t* x, int64_t v, int64_t w) {
    int64_t n = ray_is_atom(x) ? w : q_count(x);
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    uint64_t u = (uint64_t)v;
    for (int64_t i = n - 1; i >= 0; i--) {
        int64_t bi = ray_is_atom(x) ? q_type_iatom_val(x) : q_type_ivec_get(x, i);
        if (bi <= 0) { d[i] = (int64_t)u; u = 0; }
        else { d[i] = (int64_t)(u % (uint64_t)bi); u /= (uint64_t)bi; }
    }
    return out;
}

/* x vs y at width w: an atom's digit vector; a list's per-item encodings stacked as w rows shaped like y
 * (ref/vs.md: "each item of the result is identical to y in structure") */
static ray_t* base_encode(ray_t* x, ray_t* y, int64_t w) {
    int64_t v;
    if (ikind_atom(y, &v)) return digits_of(x, v, w);
    bool gen = y->type == RAY_LIST;
    int64_t m = q_count(y);
    ray_t** e = gen ? (ray_t**)ray_data(y) : NULL;
    ray_t* cols = ray_list_new(m > 0 ? m : 1);
    if (RAY_IS_ERR(cols)) return cols;
    for (int64_t j = 0; j < m; j++) {
        ray_t* c = gen ? base_encode(x, e[j], w) : digits_of(x, ikind_get(y, j), w);
        if (RAY_IS_ERR(c)) { ray_release(cols); return c; }
        cols = ray_list_append(cols, c); ray_release(c);
        if (RAY_IS_ERR(cols)) return cols;
    }
    ray_t** cv = (ray_t**)ray_data(cols);
    ray_t* rows = ray_list_new(w > 0 ? w : 1);
    if (RAY_IS_ERR(rows)) { ray_release(cols); return rows; }
    for (int64_t r = 0; r < w; r++) {
        ray_t* row = gen ? ray_list_new(m > 0 ? m : 1) : ray_vec_new(RAY_I64, m > 0 ? m : 1);
        if (RAY_IS_ERR(row)) { ray_release(cols); ray_release(rows); return row; }
        if (!gen) row->len = m;
        for (int64_t j = 0; j < m && !RAY_IS_ERR(row); j++) {
            if (!gen) { ((int64_t*)ray_data(row))[j] = ((const int64_t*)ray_data(cv[j]))[r]; continue; }
            if (cv[j]->type == RAY_I64) {                 /* an atom item's digit */
                ray_t* d = ray_i64(((const int64_t*)ray_data(cv[j]))[r]);
                row = ray_list_append(row, d); ray_release(d);
            } else row = ray_list_append(row, ((ray_t**)ray_data(cv[j]))[r]);   /* a list item's row; append retains */
        }
        if (RAY_IS_ERR(row)) { ray_release(cols); ray_release(rows); return row; }
        rows = ray_list_append(rows, row); ray_release(row);
        if (RAY_IS_ERR(rows)) { ray_release(cols); return rows; }
    }
    ray_release(cols);
    return rows;
}

ray_t* q_vs_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    const char *xp, *yp; int64_t xl, yl;
    int8_t lane = lane_of(x);
    /* --- partition: x names the result lane; a text x splits a char atom as a 1-char string, a byte x leaves
     * atoms to the encode arm (vs_charv.qcmd pins `0x0 vs "f"` as 'type) --- */
    if (lane && (lane == RAY_CHARV ? lane_bytes(y, &yp, &yl) : lane_vec_bytes(y, &yp, &yl))) {
        lane_bytes(x, &xp, &xl);
        return str_split(yp, yl, xp, xl, lane);
    }
    /* --- line split / sym split (` vs) --- */
    if (q_type_is_null_sym(x)) {
        if (y->type == -RAY_SYM) return sym_split(y);
        if (!lane_bytes(y, &yp, &yl)) return q_err(QE_TYPE);
        return q_str_charv_out(q_str_split_lines(yp, (size_t)yl));
    }
    /* --- byte encode (0x0 vs scalar) / bit decompose (0b vs scalar) --- */
    if (x->type == -RAY_BYTE_ONLY) return ray_is_atom(y) ? byte_encode(y) : q_err(QE_NYI);
    if (x->type == -RAY_BOOL) return ray_is_atom(y) ? bit_decompose(y) : q_err(QE_TYPE);
    /* --- base-x --- */
    if (q_type_is_int_atom(x) && q_type_iatom_val(x) < 2) return q_err(QE_DOMAIN);
    if (q_type_is_int_atom(x) || q_type_is_int_vec(x)) {
        int64_t w = max_width(x, y);
        return w < 0 ? q_err(QE_TYPE) : base_encode(x, y, w);
    }
    return q_err(QE_TYPE);
}

/* join the lane vectors of list y with separator sep -> one lane vector (host==1 appends '\n' to every item, the
 * ` sv host-lines form) */
static ray_t* str_join(ray_t* y, const char* sep, int64_t sl, int host, int8_t lane) {
    if (!y || y->type != RAY_LIST) return q_err(QE_TYPE);
    int64_t n = q_count(y);
    ray_t** ev = (ray_t**)ray_data(y);
    const char* ep; int64_t el;
    int64_t total = host ? n : (n > 0 ? (n - 1) * sl : 0);
    for (int64_t i = 0; i < n; i++) {
        if (!lane_vec_bytes(ev[i], &ep, &el)) return q_err(QE_TYPE);
        total += el;
    }
    char* buf = malloc(total ? (size_t)total : 1);
    if (!buf) return q_err(QE_WSFULL);
    int64_t w = 0;
    for (int64_t i = 0; i < n; i++) {
        lane_vec_bytes(ev[i], &ep, &el);
        memcpy(buf + w, ep, (size_t)el); w += el;
        if (host) buf[w++] = '\n';
        else if (i + 1 < n) { memcpy(buf + w, sep, (size_t)sl); w += sl; }
    }
    ray_t* r = lane_new(lane, buf, w);
    free(buf);
    return r;
}

/* ` sv `syms — join symbols: leading ':' (file handle) joins with '/', else
 * with '.'  -> single -RAY_SYM atom. */
static ray_t* sym_join(ray_t* y) {
    int64_t n = q_count(y);
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
    int64_t n = q_count(y);
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
    int64_t n = q_count(y);
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

/* Horner over the digits, MSB first, radix per digit from an atom or vector base; col < 0 reads y as the digit
 * vector, else column col of y's rows */
static int64_t horner(ray_t* x, ray_t* y, int64_t col) {
    uint64_t acc = 0;                                 /* wraps by design: `2 sv 64#1` is -1 */
    for (int64_t i = 0, n = q_count(y); i < n; i++) {
        int64_t b = ray_is_atom(x) ? q_type_iatom_val(x) : q_type_ivec_get(x, i);
        int64_t d = col < 0 ? ikind_get(y, i) : ikind_get(((ray_t**)ray_data(y))[i], col);
        acc = acc * (uint64_t)b + (uint64_t)d;
    }
    return (int64_t)acc;
}

/* x sv y (ref/sv.md): a digit vector answers one number; a list of conforming items answers per column — the
 * inverse of the matrix encode, and Horner being atomic, a list of matrices answers a matrix (aoc/2021/day20.q) */
static ray_t* base_decode(ray_t* x, ray_t* y) {
    int64_t n = q_count(y);
    if (!ray_is_atom(x) && q_count(x) != n) return q_err(QE_LENGTH);
    if (y->type != RAY_LIST && !ikind_vec(y)) return q_err(QE_TYPE);
    if (y->type != RAY_LIST || n == 0) return ray_i64(horner(x, y, -1));   /* no digits: the fold's identity */
    ray_t** e = (ray_t**)ray_data(y);
    int64_t m = n > 0 ? q_count(e[0]) : 0;
    bool nested = n > 0 && e[0]->type == RAY_LIST;
    for (int64_t i = 0; i < n; i++) {
        if (nested ? e[i]->type != RAY_LIST : !ikind_vec(e[i])) return q_err(QE_TYPE);
        if (q_count(e[i]) != m) return q_err(QE_LENGTH);
    }
    ray_t* out = nested ? ray_list_new(m > 0 ? m : 1) : ray_vec_new(RAY_I64, m > 0 ? m : 1);
    if (RAY_IS_ERR(out)) return out;
    if (!nested) out->len = m;
    for (int64_t c = 0; c < m; c++) {
        if (!nested) { ((int64_t*)ray_data(out))[c] = horner(x, y, c); continue; }
        ray_t* col = ray_list_new(n);                 /* column c: every item's row c */
        for (int64_t i = 0; i < n && !RAY_IS_ERR(col); i++) col = ray_list_append(col, ((ray_t**)ray_data(e[i]))[c]);
        ray_t* r = RAY_IS_ERR(col) ? col : base_decode(x, col);
        if (r != col) ray_release(col);
        if (RAY_IS_ERR(r)) { ray_release(out); return r; }
        out = ray_list_append(out, r); ray_release(r);
        if (RAY_IS_ERR(out)) return out;
    }
    return out;
}

ray_t* q_sv_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    const char* xp; int64_t xl;
    int8_t lane = lane_of(x);
    if (lane && y->type == RAY_LIST && lane_bytes(x, &xp, &xl)) return str_join(y, xp, xl, 0, lane);
    if (q_type_is_null_sym(x)) return y->type == RAY_SYM ? sym_join(y) : str_join(y, "\n", 1, 1, RAY_CHARV);
    if (x->type == -RAY_BYTE_ONLY) return y->type == RAY_BYTE_ONLY ? byte_decode(y) : q_err(QE_TYPE);
    if (x->type == -RAY_BOOL) return y->type == RAY_BOOL ? bit_compose(y) : q_err(QE_TYPE);
    if (q_type_is_int_atom(x) || q_type_is_int_vec(x)) return base_decode(x, y);
    return q_err(QE_TYPE);
}
