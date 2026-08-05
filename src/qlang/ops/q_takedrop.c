/* ops/q_takedrop.c — `#` and `_`: take, reshape, drop, cut, rotate.  One arm per
 * section of ref/take.md / ref/drop.md / ref/cut.md ("count x gives the number
 * of dimensions"); every arm GENERATES INDICES and lets q_index gather. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_builtins.h"          /* q_count_long — the one count home */
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"
#include "lang/eval.h"     /* ray_take_fn — see take_kernel */
#include "lang/internal.h" /* ray_enlist_fn — reshape's atom arm */
#include "qlang/ops/q_index.h"
#include "qlang/eval/q_funsql.h" /* the entries-axis selection home */
#include <stdint.h>
#include <stdlib.h>

static int64_t len_of(ray_t* x) {          /* -1 = atom or uncountable */
    return (x && !ray_is_atom(x)) ? q_count_long(x) : -1;
}

/* y[start .. start+w) through the index home; `recycle` wraps modulo total
 * (ref/take.md "circular").  An empty slice keeps y's element type. */
static ray_t* slice_at(ray_t* y, int64_t start, int64_t w, int64_t total,
                       int recycle) {
    if (w < 0) w = 0;
    ray_t* idx = ray_vec_new(RAY_I64, w > 0 ? w : 1);
    if (RAY_IS_ERR(idx)) return idx;
    idx->len = w;
    int64_t* p = (int64_t*)ray_data(idx);
    for (int64_t i = 0; i < w; i++)
        p[i] = recycle ? (start + i) % total : start + i;
    ray_t* r = q_typed_empty_like(q_index_at(y, &idx, 1), y);
    ray_release(idx);
    return r ? r : q_err(QE_TYPE);
}

/* `n#y` is a contiguous or CYCLIC run — a COPY, not a selection: no q_ home owns
 * it, and gathering |n| materialised indices instead costs a million i64s for
 * `1000000#v`.  Also the BOOLEAN count q_type_strict_i64 refuses (phrases). */
static ray_t* take_kernel(ray_t* y, ray_t* n) {
    ray_t* r = ray_take_fn(y, n);
    return r ? r : q_err(QE_TYPE);
}

static ray_t* take_run(ray_t* y, int64_t n) {
    int64_t cnt = len_of(y);
    if (cnt >= 0 && (n == cnt || (n == -cnt && cnt > 0))) {
        ray_t* nil = NULL;        /* `y[::]` — all of y, no index materialised */
        return q_index_at(y, &nil, 1);
    }
    ray_t* nv = ray_i64(n);
    if (RAY_IS_ERR(nv)) return nv;
    ray_t* r = take_kernel(y, nv);
    ray_release(nv);
    return r;
}

static ray_t* push(ray_t* out, ray_t* s) {   /* consumes both on failure */
    if (!s || RAY_IS_ERR(s)) { ray_release(out); return s ? s : q_err(QE_TYPE); }
    out = ray_list_append(out, s);
    ray_release(s);
    return out;
}

/* nested index of a RECYCLING reshape; `*k` = the running source position */
static ray_t* reshape_rec(ray_t* y, const int64_t* d, int64_t r, int64_t total,
                          int64_t* k) {
    if (r == 1) {
        ray_t* row = slice_at(y, *k, d[0], total, 1);
        *k += d[0];
        return row;
    }
    ray_t* out = ray_list_new(d[0] > 0 ? d[0] : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < d[0] && !RAY_IS_ERR(out); i++)
        out = push(out, reshape_rec(y, d + 1, r - 1, total, k));
    return out;
}

/* rank-2 null dimensions (ref/take.md "Changes since V3.3"): `0N n` chunks by n
 * ragged; `n 0N` is BALANCED — the LAST (total mod n) rows one longer */
static ray_t* reshape_split(ray_t* y, int64_t rows, int64_t cols, int64_t total,
                            int balanced) {
    int64_t base = balanced ? (rows ? total / rows : 0) : cols;
    int64_t big  = balanced ? (rows ? total % rows : 0) : 0;
    ray_t* out = ray_list_new(rows > 0 ? rows : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0, k = 0; i < rows && !RAY_IS_ERR(out); i++) {
        int64_t w = base + (balanced && i >= rows - big ? 1 : 0);
        if (!balanced && k + w > total) w = total - k;
        out = push(out, slice_at(y, k, w, total, 0));
        k += w;
    }
    return out;
}

static ray_t* reshape_law(ray_t* y, const int64_t* d, int64_t r) {
    int64_t total = len_of(y);
    if (total < 0) return q_err(QE_TYPE);
    if (r > 2) {
        /* V3.4: above rank 2, no null, no negative, and a zero only in the
         * innermost dimension (ref/take.md) */
        for (int64_t i = 0; i < r; i++)
            if (d[i] == NULL_I64 || d[i] < 0 || (d[i] == 0 && i != r - 1))
                return q_err(QE_DOMAIN);
    } else {
        if (d[0] == NULL_I64 && d[1] == NULL_I64) return q_err(QE_LENGTH);
        if (d[0] == NULL_I64) {
            if (d[1] <= 0) return q_err(QE_LENGTH);
            return reshape_split(y, (total + d[1] - 1) / d[1], d[1], total, 0);
        }
        if (d[1] == NULL_I64) {
            if (d[0] <= 0) return q_err(QE_LENGTH);
            return reshape_split(y, d[0], 0, total, 1);
        }
        if (d[0] < 0 || d[1] < 0) return q_err(QE_LENGTH);
    }
    int64_t prod = 1;
    for (int64_t i = 0; i < r; i++) prod *= d[i];
    if (prod > 0 && total <= 0) return q_err(QE_LENGTH);
    int64_t k = 0;
    return reshape_rec(y, d, r, total, &k);
}

static ray_t* reshape(ray_t* shape, ray_t* y) {
    int64_t r = ray_len(shape);
    int64_t sbuf[8];
    int64_t* d = (r <= 8) ? sbuf : malloc((size_t)r * sizeof(int64_t));
    if (!d) return q_err(QE_WSFULL);
    for (int64_t i = 0; i < r; i++) d[i] = q_type_ivec_get(shape, i);
    ray_t* enl = (y && ray_is_atom(y)) ? ray_enlist_fn(&y, 1) : NULL;
    ray_t* res = (enl && RAY_IS_ERR(enl)) ? enl
                                          : reshape_law(enl ? enl : y, d, r);
    if (enl && !RAY_IS_ERR(enl)) ray_release(enl);
    if (d != sbuf) free(d);
    return res;
}

/* int-VECTOR x: y sliced at the cut positions — index ranges, so a TABLE cuts
 * like a list (ref/cut.md) */
static ray_t* cut_at(ray_t* pos, ray_t* y) {
    if (!y || (!ray_is_vec(y) && y->type != RAY_LIST && !q_type_is_table(y)))
        return q_err(QE_TYPE);
    int64_t len = len_of(y), np = ray_len(pos);
    if (len < 0) return q_err(QE_TYPE);
    ray_t* out = ray_list_new(np > 0 ? np : 1);
    if (RAY_IS_ERR(out)) return out;
    int64_t prev = 0;
    for (int64_t i = 0; i < np && !RAY_IS_ERR(out); i++) {
        int64_t p = q_type_ivec_get(pos, i);
        int64_t nxt = (i + 1 < np) ? q_type_ivec_get(pos, i + 1) : len;
        if (p < 0 || p > len || nxt < p || nxt > len || (i > 0 && p < prev)) {
            ray_release(out);
            return q_err(QE_DOMAIN);
        }
        prev = p;
        out = push(out, slice_at(y, p, nxt - p, len, 0));
    }
    return out;
}

/* kdb unifies a homogeneous take-of-mixed result to a typed vector (`~` is
 * type-strict per ref/match.md, yet misc/aoc/2016/day17.q's `3 3 in 2#'q`
 * terminates — so 2#(3;3;"s") must be 7h, not 0h) */
static ray_t* take_unify(ray_t* r) {
    if (!r || RAY_IS_ERR(r) || r->type != RAY_LIST) return r;
    ray_t* c = q_list_collapse(r);
    ray_release(r);
    return c;
}

/* q `x # y` — Take (ref/take.md); borrows both args */
ray_t* q_take_wrap(ray_t* x, ray_t* y) {
    if (x && x->type == -RAY_SYM && y && ray_is_vec(y))
        return q_attr_set_dispatch(x, y);              /* `s#v — set attribute */
    if (RAY_IS_NULL(y)) {   /* :: is an atom take fills from, but names no element lane */
        ray_t* e = ray_list_new(1);
        if (RAY_IS_ERR(e)) return e;
        e = ray_list_append(e, y);
        ray_t* r = RAY_IS_ERR(e) ? e : q_take_wrap(x, e);
        if (!RAY_IS_ERR(e)) ray_release(e);
        return r;
    }
    if (q_type_is_int_vec(x) && ray_len(x) >= 2) return reshape(x, y);
    ray_t* es = q_funsql_entries_take(x, y);  /* dict keys / table cols / keyed */
    if (es) return es;
    int64_t n;
    if (q_type_is_int_vec(x) && ray_len(x) == 1)
        n = q_type_ivec_get(x, 0);            /* V3.4: a rank-1 shape is n#y */
    else if (!q_type_strict_i64(x, &n))
        return take_unify(take_kernel(y, x));
    return take_unify(take_run(y, n));
}

/* q `x _ y` — Drop (ref/drop.md); borrows both args */
ray_t* q_drop_wrap(ray_t* x, ray_t* y) {
    if (y && q_type_is_plain_dict(y)) {
        int64_t cnt;
        if (q_type_strict_i64(x, &cnt)) {     /* n _ d ≡ (n _ key d)!(n _ value d) */
            ray_t* nk = q_drop_wrap(x, ray_dict_keys(y));
            if (!nk || RAY_IS_ERR(nk)) return nk ? nk : q_err(QE_TYPE);
            ray_t* nv = q_drop_wrap(x, ray_dict_vals(y));
            if (!nv || RAY_IS_ERR(nv)) { ray_release(nk); return nv ? nv : q_err(QE_TYPE); }
            nv = q_typed_empty_like(nv, ray_dict_vals(y));   /* dropping every entry keeps the value type */
            return ray_dict_new(nk, nv);
        }
        return q_funsql_dict_drop_keys(x, y);
    }
    if (x && q_type_is_plain_dict(x)) {       /* dict LEFT drops an atom key only */
        if (!y || !ray_is_atom(y)) return q_err(QE_TYPE);
        return q_funsql_dict_drop_keys(y, x);
    }
    ray_t* es = q_funsql_entries_drop(x, y);  /* keyed tables + table columns */
    if (es) return es;
    int64_t i;
    if (x && (ray_is_vec(x) || x->type == RAY_LIST) &&
        q_type_strict_i64(y, &i)) {           /* x _ i — delete the item at i */
        int64_t len = ray_len(x);
        if (i < 0 || i >= len) { ray_retain(x); return x; }   /* Drop is tolerant */
        ray_t* idx = ray_vec_new(RAY_I64, len > 1 ? len - 1 : 1);
        if (RAY_IS_ERR(idx)) return idx;
        idx->len = len - 1;
        int64_t* p = (int64_t*)ray_data(idx);
        for (int64_t j = 0, c = 0; j < len; j++)
            if (j != i) p[c++] = j;
        ray_t* r = q_typed_empty_like(q_index_at(x, &idx, 1), x);
        ray_release(idx);
        return r ? r : q_err(QE_TYPE);
    }
    if (q_type_is_int_vec(x)) return cut_at(x, y);
    int64_t k;
    ray_t* e = q_type_i64_or_err(x, &k, "_: n");
    if (e) return e;
    int64_t cnt = len_of(y);
    if (cnt < 0) return q_err(QE_TYPE);
    /* drop IS a take of what is left; the clamp is REQUIRED — drop is tolerant
     * past the ends where take wraps round (`5_0 1 2`) */
    int64_t keep = cnt - (k < 0 ? -k : k);
    if (keep < 0) keep = 0;
    return take_run(y, k >= 0 ? -keep : keep);
}

/* q `n rotate x` — x read from item n, wrapping (ref/rotate.md) */
ray_t* q_rotate_wrap(ray_t* n, ray_t* x) {
    int64_t k, len = len_of(x);
    if (!q_type_strict_i64(n, &k) || len < 0) return q_err(QE_TYPE);
    if (len == 0) { ray_retain(x); return x; }
    return slice_at(x, ((k % len) + len) % len, len, len, 1);
}

ray_t* q_cut_wrap(ray_t* n, ray_t* x) {
    int64_t sz;
    if (!q_type_strict_i64(n, &sz)) return q_drop_wrap(n, x);
    if (sz <= 0) return q_err(QE_DOMAIN);
    int64_t total = len_of(x);
    if (total < 0) return q_err(QE_TYPE);
    int64_t rows = (total + sz - 1) / sz;
    ray_t* out = ray_list_new(rows > 0 ? rows : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t r = 0; r < rows && !RAY_IS_ERR(out); r++) {
        int64_t start = r * sz, w = (start + sz > total) ? total - start : sz;
        out = push(out, slice_at(x, start, w, total, 0));
    }
    return out;
}
