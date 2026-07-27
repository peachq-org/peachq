/* ops/q_takedrop.c — the `#` and `_` glyph homes: take, reshape, drop, cut,
 * rotate.
 *
 * Split from q_list.c (2026-07-27) — pure function moves, no behaviour change.
 * `#`'s set-attribute arm delegates to ops/q_attr.c; the grade family moved to
 * ops/q_sort.c.  See q_registry.h for the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_err.h"
#include "qlang/q_type.h"  /* q_type_strict_i64, q_type_is_int_vec, q_type_ivec_get */
#include "lang/eval.h"     /* ray_take_fn, ray_at_fn */
#include "lang/internal.h" /* ray_enlist_fn — reshape's atom arm; ray_concat_fn — drop's splice */
#include "qlang/ops/q_index.h" /* q_index_entries_take/_drop, q_index_drop_dict — the entry arms */
#include <stdint.h>
#include <stdlib.h>        /* malloc/free — gather's index buffer */

/* Item count of a countable shape.  A q string is a -RAY_STR atom whose count
 * lives in ray_str_len, NOT the ->len union field (which aliases the SSO
 * {slen,sdata} bytes), so ray_len would be garbage for it.  0 = not countable. */
static int seq_len(ray_t* x, int64_t* out) {
    if (!x) return 0;
    if (x->type == -RAY_STR) { *out = (int64_t)ray_str_len(x); return 1; }
    if (ray_is_vec(x) || x->type == RAY_LIST) { *out = ray_len(x); return 1; }
    return 0;
}

/* Gather `count` elements from x starting at logical index `start`.  recycle:
 * indices wrap modulo `total` (reshape recycling); else sequential in-range
 * (chunk / cut).  A string gathers chars into a new string; a vector/list
 * gathers via ray_at_fn over an i64 index vector.  Borrows x. */
static ray_t* gather(ray_t* x, int64_t start, int64_t count, int64_t total,
                       int recycle) {
    if (count < 0) count = 0;
    if (x && x->type == -RAY_STR) {
        const char* sp = ray_str_ptr(x);
        char stackb[1024];
        char* b = (count < (int64_t)sizeof stackb) ? stackb
                                                    : malloc((size_t)count + 1);
        if (!b) return q_err(QE_WSFULL);
        for (int64_t i = 0; i < count; i++)
            b[i] = sp[recycle && total ? ((start + i) % total) : (start + i)];
        ray_t* row = ray_str(b, (size_t)count);
        if (b != stackb) free(b);
        return row;
    }
    int64_t stacki[1024];
    int64_t* ix = (count <= 1024) ? stacki
                                  : malloc((size_t)(count ? count : 1) * sizeof(int64_t));
    if (!ix) return q_err(QE_WSFULL);
    for (int64_t i = 0; i < count; i++)
        ix[i] = recycle && total ? ((start + i) % total) : (start + i);
    ray_t* idx = ray_vec_from_raw(RAY_I64, ix, count);
    if (ix != stacki) free(ix);
    if (RAY_IS_ERR(idx)) return idx;
    ray_t* row = ray_at_fn(x, idx);       /* boxed list of atoms */
    ray_release(idx);
    if (!row || RAY_IS_ERR(row)) return row;
    ray_t* c = q_list_collapse(row);      /* -> typed vector when homogeneous */
    ray_release(row);
    return c;
}

/* q `shape # x` — reshape (kdb ref/take): an int-VECTOR left arg reshapes x
 * row-major into a matrix (2-D case).  A 0N dimension is INFERRED (chunk into
 * that stride, ragged last row); otherwise x is RECYCLED to fill.  Ranks other
 * than 2 fall back to rayfall range-take (unchanged behaviour).  Borrows both. */
static ray_t* i_reshape(ray_t* shape, ray_t* x) {
    int64_t nd = ray_len(shape);
    if (nd != 2) return ray_take_fn(x, shape);
    const int64_t* dv = (const int64_t*)ray_data(shape);
    int64_t d0 = dv[0], d1 = dv[1];
    int is_str = (x && x->type == -RAY_STR);
    int64_t total = is_str ? (int64_t)ray_str_len(x) : (x ? ray_len(x) : 0);
    if (total <= 0) return q_err(QE_LENGTH);
    int64_t rows, cols, chunk = 0;
    if (d0 == NULL_I64 && d1 != NULL_I64) {   /* 0N dimension -> inferred stride */
        if (d1 <= 0) return q_err(QE_LENGTH);
        cols = d1; rows = (total + cols - 1) / cols; chunk = 1;
    } else if (d1 == NULL_I64 && d0 != NULL_I64) {
        if (d0 <= 0) return q_err(QE_LENGTH);
        rows = d0; cols = (total + rows - 1) / rows; chunk = 1;
    } else if (d0 == NULL_I64 || d1 == NULL_I64) {
        return q_err(QE_LENGTH);
    } else {
        rows = d0; cols = d1;
    }
    if (rows < 0 || cols < 0) return q_err(QE_LENGTH);
    ray_t* out = ray_list_new(rows > 0 ? rows : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t r = 0; r < rows; r++) {
        int64_t start = r * cols, rc = cols;
        if (chunk && start + rc > total) rc = total - start;   /* ragged last */
        ray_t* row = gather(x, start, rc, total, !chunk);
        if (!row || RAY_IS_ERR(row)) { ray_release(out); return row ? row : q_err(QE_DOMAIN); }
        out = ray_list_append(out, row);
        ray_release(row);
        if (RAY_IS_ERR(out)) return out;
    }
    return out;
}

/* q `n # list` — take.  A SYMBOL ATOM left arg against a simple vector is
 * set-attribute (q_attr_set_dispatch) — the only meaning, since you cannot
 * key-take from a flat vector; a symbol atom against a dict/table stays
 * key/column take (falls through).  An int-VECTOR left arg (len>=2) is RESHAPE
 * (matrix); an atom is take.  rayfall ray_take_fn(vec, n) has the opposite arg
 * order, so swap.  Borrows both args (does not release them). */
ray_t* q_take_wrap(ray_t* n, ray_t* list) {
    if (n && n->type == -RAY_SYM && list && ray_is_vec(list))
        return q_attr_set_dispatch(n, list);
    if (n && n->type == RAY_I64 && ray_len(n) >= 2) {
        if (list && ray_is_atom(list)) {         /* n1 n2#atom — TYPE-BLIND: kdb
                                                  * reshapes any atom by cycling
                                                  * its enlist (ints/bytes/chars) */
            ray_t* v = ray_enlist_fn(&list, 1);
            if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_OOM);
            ray_t* r = i_reshape(n, v);
            ray_release(v);
            return r;
        }
        return i_reshape(n, list);
    }
    /* dict / table-column / keyed-table selection — the index home owns entry
     * structure (ref/take.md Keys, Columns, Keyed table) */
    ray_t* es = q_index_entries_take(n, list);
    if (es) return es;
    return ray_take_fn(list, n);
}

/* q `n _ list` — count-drop (NOT rayfall's dict key-remove).  n>=0 drops the
 * first n elements; n<0 drops the last |n|.  Implemented as a range-take
 * ray_take_fn(list, (start; amount)), which clamps at the ends.  Borrows args.
 *
 * Length comes from seq_len (string-aware). */
ray_t* q_drop_wrap(ray_t* n, ray_t* list) {
    /* dict arms (ref/drop.md) — the index home owns dict structure; NULL means
     * neither operand is a dict, so fall through to the list/vector arms. */
    ray_t* dd = q_index_drop_dict(n, list);
    if (dd) return dd;
    ray_t* es = q_index_entries_drop(n, list);   /* keyed tables + table columns */
    if (es) return es;
    /* x _ i — delete the item at index i (ref/drop.md `0 1 ... 8 _ 5`):
     * list/vector lhs, int-ATOM rhs.  Two clamped range-takes joined; an
     * out-of-range index returns x unchanged (Drop is tolerant). */
    int64_t i;
    if (n && (ray_is_vec(n) || n->type == RAY_LIST) && n->type != RAY_DICT &&
        q_type_strict_i64(list, &i)) {
        int64_t len = ray_len(n);
        if (i < 0 || i >= len) { ray_retain(n); return n; }
        int64_t r1[2] = { 0, i }, r2[2] = { i + 1, len - i - 1 };
        ray_t* rng1 = ray_vec_from_raw(RAY_I64, r1, 2);
        if (RAY_IS_ERR(rng1)) return rng1;
        ray_t* head = ray_take_fn(n, rng1);          /* owned */
        ray_release(rng1);
        if (!head || RAY_IS_ERR(head)) return head;
        ray_t* rng2 = ray_vec_from_raw(RAY_I64, r2, 2);
        if (RAY_IS_ERR(rng2)) { ray_release(head); return rng2; }
        ray_t* tail = ray_take_fn(n, rng2);          /* owned */
        ray_release(rng2);
        if (!tail || RAY_IS_ERR(tail)) { ray_release(head); return tail; }
        ray_t* r = ray_concat_fn(head, tail);        /* owned */
        ray_release(head);
        ray_release(tail);
        return r;
    }
    /* q cut: int-VECTOR lhs — `2 4_v` slices [p0,p1) then [p_last,end).
     * Positions non-decreasing within 0..len; result is a boxed list of
     * slices (kdb 0h). */
    if (q_type_is_int_vec(n)) {
        if (!list || (!ray_is_vec(list) && list->type != RAY_LIST))
            return q_err(QE_TYPE);
        int64_t len = ray_len(list);
        int64_t np = ray_len(n);
        ray_t* out = ray_list_new(np > 0 ? np : 1);
        int64_t prev = 0;
        for (int64_t i = 0; i < np; i++) {
            int64_t p = q_type_ivec_get(n, i);
            int64_t nxt = (i + 1 < np) ? q_type_ivec_get(n, i + 1) : len;
            if (p < 0 || p > len || nxt < p || nxt > len || (i > 0 && p < prev)) {
                ray_release(out);
                return q_err(QE_DOMAIN);
            }
            prev = p;
            int64_t rng[2] = { p, nxt - p };
            ray_t* range = ray_vec_from_raw(RAY_I64, rng, 2);
            if (RAY_IS_ERR(range)) { ray_release(out); return range; }
            ray_t* slice = ray_take_fn(list, range);
            ray_release(range);
            if (!slice || RAY_IS_ERR(slice)) { ray_release(out); return slice; }
            out = ray_list_append(out, slice);
            ray_release(slice);
        }
        return out;
    }
    int64_t k;
    ray_t* err = q_type_i64_or_err(n, &k, "_: n");
    if (err) return err;
    int64_t len;
    if (!seq_len(list, &len)) return q_err(QE_TYPE);
    int64_t start, amount;
    if (k >= 0) { start = (k < len) ? k : len; amount = len - start; }
    else        { start = 0; amount = len + k; if (amount < 0) amount = 0; }
    int64_t rng[2] = { start, amount };
    ray_t* range = ray_vec_from_raw(RAY_I64, rng, 2);
    if (RAY_IS_ERR(range)) return range;
    ray_t* r = ray_take_fn(list, range);
    ray_release(range);
    return r;
}

/* q `n rotate x` — x read from item n, wrapping (ref/rotate.md); a negative n
 * rotates right.  ONE cyclic gather, which is also what makes it an index
 * derivation: container structure is the family-"index" lift's, so x arrives
 * here a vector/list/string.  Borrows both args. */
ray_t* q_rotate_wrap(ray_t* n, ray_t* x) {
    int64_t k, len;
    if (q_type_strict_i64(n, &k) && seq_len(x, &len)) {
        if (len <= 0) { ray_retain(x); return x; }
        return gather(x, ((k % len) + len) % len, len, len, 1);
    }
    return q_err(QE_TYPE);
}

/* q `n cut x` — cut into pieces (kdb ref/cut).  An int ATOM chunks x into
 * groups of n (ragged last group: `4 cut til 10` -> (0 1 2 3;4 5 6 7;8 9)).
 * An int VECTOR is a positional cut — identical to `_`, so delegate.  Borrows. */
ray_t* q_cut_wrap(ray_t* n, ray_t* x) {
    int64_t sz;
    if (q_type_strict_i64(n, &sz)) {
        if (sz <= 0) return q_err(QE_DOMAIN);
        int64_t total;
        if (!seq_len(x, &total)) return q_err(QE_TYPE);
        int64_t rows = (total + sz - 1) / sz;
        ray_t* out = ray_list_new(rows > 0 ? rows : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t r = 0; r < rows; r++) {
            int64_t start = r * sz, rc = sz;
            if (start + rc > total) rc = total - start;
            ray_t* row = gather(x, start, rc, total, 0);   /* chunk, no recycle */
            if (!row || RAY_IS_ERR(row)) { ray_release(out); return row ? row : q_err(QE_DOMAIN); }
            out = ray_list_append(out, row);
            ray_release(row);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return q_drop_wrap(n, x);   /* int-vector positional cut == `_` */
}
