/* ops/q_sort.c — the q grade family: iasc/idesc over vectors, general lists,
 * dicts, tables and keyed tables, plus xbar bucketing.  Holds THE ordering law
 * (ord_cmp) for the values the radix kernel has no key for.
 *
 * Split from q_list.c (2026-07-27) — pure function moves, no behaviour change.
 * asc/desc/rank/xrank/xasc/xdesc are q.q derivations over these (QR_QSRC), not
 * C.  See q_registry.h for the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/base/q_err.h"
#include "qlang/q_builtins.h"  /* q_builtins_type_num — the q type-id key */
#include "qlang/eval/q_eval.h" /* carrier kind / iter id / lambda source */
#include "qlang/ops/q_index.h" /* q_index_elem_at — THE element read */
#include "lang/eval.h"     /* ray_at_fn, ray_xbar_fn */
#include "lang/internal.h" /* ray_iasc_fn / ray_idesc_fn, as_f64 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* ===== q grade / bucket family ============================================
 * GRADE IS THE PRIMITIVE: iasc/idesc own ordering for every structure (vector →
 * ray_iasc_fn; general list → ord_grade; dict → keys by the value grade;
 * table/keyed → grade_table), and asc/desc/rank/xrank/xasc/xdesc are q.q
 * derivations over them (index once, ONE
 * gather).  xbar is an arg-swap over ray_xbar_fn.  DEFERRED (error, never a
 * wrong answer): the `s#` attribute on asc results — the attr-take arm accepts
 * long vectors only, so setting it would regress symbol/nested sorts (PLAN.md). */

/* Reorder a keys-or-vals vector by a grade-index vector (owned grade), then
 * collapse the boxed result back to a typed vector.  Releases `grade`. */
static ray_t* reindex_collapse(ray_t* vec, ray_t* grade) {
    if (!grade || RAY_IS_ERR(grade)) return grade;
    ray_t* boxed = ray_at_fn(vec, grade);
    ray_release(grade);
    if (!boxed || RAY_IS_ERR(boxed)) return boxed;
    if (boxed->type == RAY_LIST) {
        ray_t* c = q_list_collapse(boxed);
        ray_release(boxed);
        return c;
    }
    return boxed;
}

/* ===== THE ordering law, for values the radix kernel has no key for ========
 * ref/asc.md: a mixed list sorts "within datatype" (:21), a nested list
 * "lexicographically" (:22).  So: TYPE NUMBER FIRST — the `type each` transcript
 * at :60-68 puts bool before long before char before date — then the published
 * intra-type order.  ONE comparison, reached only from the grade; every q door
 * (iasc/idesc, asc/desc, xasc, a By key) inherits it by being a derivation.
 *
 * NOT `bin`'s comparison (ops/q_search.c): bin.md's domain is "an atom of
 * exactly the same type (no type promotion)", so that one REFUSES a cross-type
 * pair where this law must ORDER it, and it spends a registry lookup + a full
 * apply of `<` per atom — affordable for a binary search's O(log n) probes, not
 * for a grade's O(n log n). */

/* Groups run in ascending |type number| — `test/q/sort/asc.qcmd`'s mixed-list
 * row is the anchor (`-1 -7 -7 -10 -10 -14h`), so this half is published, not
 * chosen.  The rest is ours: an atom sorts adjacent to, and BEFORE, the vector
 * of its own datatype, and the general list (0h) leads.  Injective over the tag
 * range, so equal keys mean equal types.  FUNCTION values key on their q type
 * id (100h..) — community law, test/q/community/sort_functions.qcmd — which
 * ascending |type| places after every data type (peachq's own placement). */
static int64_t ord_type_key(ray_t* x) {
    int64_t t = q_builtins_type_num(x);
    if (t >= 100) return 2 * t;
    return t < 0 ? -2 * t : 2 * t + 1;
}

/* The datatypes q publishes NO intra-type order for — an ENUMERATED catalogue,
 * never a default arm, because a default is what makes "no law here" and "law
 * not implemented yet" indistinguishable.  They tie, and the stable grade then
 * keeps input order: q is total, and the order among genuinely unspecified
 * elements is not something a caller can depend on. */
static int ord_unordered(int8_t t) {
    switch (t < 0 ? -t : t) {
    case RAY_TABLE: case RAY_DICT:
        return 1;
    default:
        return 0;
    }
}

static int ord_cmp(ray_t* a, ray_t* b);

static int ord_cmp_i64(int64_t x, int64_t y) { return x < y ? -1 : x > y; }

/* Within one function type: primitives compare by their manifest kdb_op
 * identity number (`::` IS unary primitive 0), iterators by adverb id,
 * lambdas by VERBATIM SOURCE (a recorded peachq choice — kdb recurses the
 * value structure, which pins bytecode); proj/comp/deriv carriers tie and
 * the stable grade keeps input order. */
static int64_t ord_fn_id(ray_t* x, int8_t qt) {
    if (RAY_IS_NULL(x)) return 0;
    if (qt == 103) return q_eval_apply_iter_id(x);
    if (qt == 101 || qt == 102) return q_registry_kdb_op_of(x, NULL);
    return 0;
}

static int ord_cmp_fn(ray_t* a, ray_t* b, int8_t qt) {
    if (qt == 100) {
        ray_t* sa = q_eval_apply_lambda_src(a);   /* borrowed; NULL on engine lambdas */
        ray_t* sb = q_eval_apply_lambda_src(b);
        if (!sa || !sb) return 0;
        int c = ray_str_cmp(sa, sb);
        return c < 0 ? -1 : c > 0;
    }
    return ord_cmp_i64(ord_fn_id(a, qt), ord_fn_id(b, qt));
}

/* Lexicographic over items, shorter-prefix-first.  Same-datatype byte vectors —
 * the string case — settle in one memcmp rather than n element reads. */
static int ord_cmp_items(ray_t* a, ray_t* b) {
    int64_t la = ray_len(a), lb = ray_len(b), n = la < lb ? la : lb;
    if (ray_is_bytelike(a->type)) {
        int c = n ? memcmp(ray_data(a), ray_data(b), (size_t)n) : 0;
        if (c) return c < 0 ? -1 : 1;
    } else {
        for (int64_t i = 0; i < n; i++) {
            ray_t* ea = q_index_elem_at(a, i);
            ray_t* eb = q_index_elem_at(b, i);
            int c = (ea && eb && !RAY_IS_ERR(ea) && !RAY_IS_ERR(eb)) ? ord_cmp(ea, eb) : 0;
            if (ea) ray_release(ea);
            if (eb) ray_release(eb);
            if (c) return c;
        }
    }
    return ord_cmp_i64(la, lb);
}

/* THE comparison: ascending |type number|, then the lane's own order (the key
 * is ord_type_key, NOT the signed tag).  Nulls sort FIRST —
 * NaN by the explicit test, the int sentinels by being their lane's minimum,
 * which is also the radix kernel's ascending convention. */
static int ord_cmp(ray_t* a, ray_t* b) {
    if (a == b) return 0;
    if (!a || !b) return a ? 1 : -1;
    int64_t ka = ord_type_key(a), kb = ord_type_key(b);
    if (ka != kb) return ka < kb ? -1 : 1;
    if (ka >= 200) return ord_cmp_fn(a, b, (int8_t)(ka / 2));
    if (a->type < 0) {
        switch (-a->type) {
        case RAY_BOOL:   /* the one lane as_i64 has no explicit read for */
            return ord_cmp_i64(a->b8, b->b8);
        RAY_BYTE_CASES: case RAY_I16: case RAY_I32: case RAY_I64:
        RAY_TEMPORAL32_CASES: RAY_TEMPORAL64_CASES:
            return ord_cmp_i64(q_type_as_i64(a), q_type_as_i64(b));
        case RAY_F32: case RAY_F64: RAY_TEMPORALF_CASES: {
            double x = as_f64(a), y = as_f64(b);
            if (isnan(x) || isnan(y)) return isnan(x) ? (isnan(y) ? 0 : -1) : 1;
            return x < y ? -1 : x > y;
        }
        case RAY_SYM: {
            ray_t* sa = ray_sym_str(a->i64);   /* symbols order by their STRING */
            ray_t* sb = ray_sym_str(b->i64);
            if (sa && sb) { int c = ray_str_cmp(sa, sb); return c < 0 ? -1 : c > 0; }
            return 0;
        }
        case RAY_STR: { int c = ray_str_cmp(a, b); return c < 0 ? -1 : c > 0; }
        case RAY_GUID:
            if (a->obj && b->obj) {
                int c = memcmp(ray_data(a->obj), ray_data(b->obj), 16);
                return c < 0 ? -1 : c > 0;
            }
            return 0;
        default: break;
        }
    } else if (a->type == RAY_LIST || ray_is_vec(a)) {
        return ord_cmp_items(a, b);
    }
    assert(ord_unordered(a->type) && "ordering law: type outside the catalogue");
    return 0;
}

/* One comparison between two ITEMS of v, by index. */
static int ord_cmp_at(ray_t* v, int64_t i, int64_t j) {
    ray_t* a = q_index_elem_at(v, i);
    ray_t* b = q_index_elem_at(v, j);
    int c = (a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b)) ? ord_cmp(a, b) : 0;
    if (a) ray_release(a);
    if (b) ray_release(b);
    return c;
}

/* THE comparison grade: a stable bottom-up merge sort of `til count x` under
 * ord_cmp.  Ties take the left run, so equals keep input order for idesc too. */
static ray_t* ord_grade(ray_t* x, int desc) {
    int64_t n = ray_len(x);
    ray_t* g = ray_vec_new(RAY_I64, n);
    if (!g || RAY_IS_ERR(g)) return g ? g : q_err(QE_OOM);
    g->len = n;
    int64_t* a = (int64_t*)ray_data(g);
    for (int64_t i = 0; i < n; i++) a[i] = i;
    if (n < 2) return g;
    ray_t* scratch = ray_vec_new(RAY_I64, n);
    if (!scratch || RAY_IS_ERR(scratch)) {
        ray_release(g);
        return scratch ? scratch : q_err(QE_OOM);
    }
    scratch->len = n;
    int64_t* b = (int64_t*)ray_data(scratch);
    for (int64_t w = 1; w < n; w *= 2) {
        for (int64_t lo = 0; lo < n; lo += 2 * w) {
            int64_t mid = lo + w < n ? lo + w : n;
            int64_t hi  = lo + 2 * w < n ? lo + 2 * w : n;
            int64_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                int c = ord_cmp_at(x, a[j], a[i]);
                b[k++] = (desc ? -c : c) < 0 ? a[j++] : a[i++];
            }
            while (i < mid) b[k++] = a[i++];
            while (j < hi)  b[k++] = a[j++];
        }
        int64_t* t = a; a = b; b = t;
    }
    if (a != (int64_t*)ray_data(g))
        memcpy(ray_data(g), a, (size_t)n * sizeof(int64_t));
    ray_release(scratch);
    return g;
}

/* A column the radix kernel cannot key must not TIE — a tie leaves the position
 * tiebreaker to win and hands the caller a plausible identity permutation, which
 * is how a missing ordering law became a wrong answer.  Order it by the law and
 * give the kernel the DENSE rank, which carries the order AND the ties. */
static ray_t* ord_dense_rank(ray_t* col) {
    int64_t n = ray_len(col);
    ray_t* g = ord_grade(col, 0);
    if (!g || RAY_IS_ERR(g)) return g ? g : q_err(QE_OOM);
    ray_t* r = ray_vec_new(RAY_I64, n);
    if (!r || RAY_IS_ERR(r)) { ray_release(g); return r ? r : q_err(QE_OOM); }
    r->len = n;
    int64_t* rp = (int64_t*)ray_data(r);
    const int64_t* gp = (const int64_t*)ray_data(g);
    for (int64_t i = 0, rank = 0; i < n; i++) {
        if (i && ord_cmp_at(col, gp[i - 1], gp[i])) rank++;
        rp[gp[i]] = rank;
    }
    ray_release(g);
    return r;
}

#define Q_GRADE_CHUNK 15    /* data keys per kernel call: 15 + the position
                             * tiebreaker = ray_sort_indices' 16-key cap */

/* q `iasc`/`idesc` on a TABLE — the lexicographic row grade.  Rides the engine's
 * multi-column composite-radix grade (ray_sort_indices), the entry qSQL order-by
 * already uses, so table order and query order cannot decohere.
 *
 * Stable by construction: the kernel leaves stability to its caller (sort.c:1212
 * "stability via row is handled by the caller"), so position rides as a final,
 * ALWAYS-ascending key — unique values make the composite key a total order, which
 * the kernel's unstable small-array path cannot then reorder.  Columns are graded
 * in right-to-left chunks and the chunk grades composed (minor-first, the LSD-radix
 * argument), lifting the kernel's 16-key cap. */
static ray_t* grade_table(ray_t* t, int desc) {
    int64_t nc = ray_table_ncols(t);
    int64_t nr = ray_table_nrows(t);
    if (nc <= 0) return q_err(QE_TYPE);
    if (nr == 0) {
        ray_t* g = ray_vec_new(RAY_I64, 0);
        if (g && !RAY_IS_ERR(g)) g->len = 0;
        return g;
    }
    ray_t* idx = ray_vec_new(RAY_I64, nr);           /* til nr — the tiebreaker key */
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_OOM);
    idx->len = nr;
    int64_t* ip = (int64_t*)ray_data(idx);
    for (int64_t i = 0; i < nr; i++) ip[i] = i;
    ray_t* perm = NULL;                              /* identity until the first chunk */
    for (int64_t hi = nc; hi > 0; hi -= Q_GRADE_CHUNK) {
        int64_t lo = hi > Q_GRADE_CHUNK ? hi - Q_GRADE_CHUNK : 0;
        int64_t k = hi - lo;
        ray_t* cols[Q_GRADE_CHUNK + 1];
        uint8_t descs[Q_GRADE_CHUNK + 1];
        uint8_t owns[Q_GRADE_CHUNK + 1];             /* per-column: needs release */
        int64_t filled = 0;
        ray_t* g = NULL;
        for (int64_t c = lo; c < hi; c++) {
            ray_t* col = ray_table_get_col_idx(t, c);         /* borrowed */
            int own = 0;
            if (col && perm) {
                ray_retain(perm);
                col = reindex_collapse(col, perm);          /* owned copy */
                own = 1;
            }
            if (col && !RAY_IS_ERR(col) && col->type == RAY_LIST) {
                ray_t* rk = ord_dense_rank(col);   /* the one type the kernel ignores */
                if (own) ray_release(col);
                col = rk;
                own = 1;
            }
            if (!col || RAY_IS_ERR(col)) {
                g = col ? col : q_err(QE_TYPE);
                break;
            }
            cols[filled] = col;
            descs[filled] = (uint8_t)desc;
            owns[filled++] = (uint8_t)own;
        }
        if (!g) {
            cols[k] = idx;             /* position: original on the first chunk, the
                                        * running minor order on later ones — either
                                        * way it is what must be preserved */
            descs[k] = 0;              /* ALWAYS ascending, even for idesc */
            g = ray_sort_indices(cols, descs, NULL, (uint8_t)(k + 1), nr);
        }
        for (int64_t c = 0; c < filled; c++) if (owns[c]) ray_release(cols[c]);
        if (!g || RAY_IS_ERR(g)) {
            if (perm) ray_release(perm);
            ray_release(idx);
            return g ? g : q_err(QE_OOM);
        }
        /* Fold the chunk grade into the running permutation: perm = perm[g] */
        if (perm) {
            ray_t* np = reindex_collapse(perm, g);          /* releases g */
            ray_release(perm);
            perm = np;
        } else {
            perm = g;
        }
        if (!perm || RAY_IS_ERR(perm)) {
            ray_release(idx);
            return perm ? perm : q_err(QE_OOM);
        }
    }
    ray_release(idx);
    return perm;
}

/* Grade a general (boxed) list.  Collapse first, so a list that IS one datatype
 * keeps the radix kernel's answer and its speed; only a genuinely mixed or
 * nested list reaches the comparison grade.  A run of like row-dicts collapses
 * to a TABLE, which grades by its rows. */
static ray_t* grade_general(ray_t* x, int desc) {
    ray_t* c = q_list_collapse(x);
    if (!c) return ord_grade(x, desc);
    if (RAY_IS_ERR(c)) return c;
    ray_t* g;
    if (c->type == RAY_LIST)     g = ord_grade(x, desc);
    else if (q_type_is_table(c)) g = grade_table(c, desc);
    else                         g = desc ? ray_idesc_fn(c) : ray_iasc_fn(c);
    ray_release(c);
    return g;
}

/* Grade a dict's VALUE vector.  An empty value list grades to an empty long
 * vector via ord_grade, so `asc 0#d` reindexes to an empty dict. */
static ray_t* dict_value_grade(ray_t* vals, int desc) {
    if (!vals) return q_err(QE_TYPE);
    /* A KEYED table's value is a table — grade its rows, not its columns (this is
     * what lets the dict law carry keyed tables: `asc kt` gathers key+value rows
     * by the grade of the value rows, per ref/asc.md's non-key-column rule). */
    if (q_type_is_table(vals))   return grade_table(vals, desc);
    if (vals->type == RAY_LIST)  return grade_general(vals, desc);
    return desc ? ray_idesc_fn(vals) : ray_iasc_fn(vals);
}

/* q `iasc`/`idesc` on a DICT — return the KEYS in ascending / descending VALUE
 * order (kdb ref/asc.md grade form: `iasc d` grades the values, indexes keys). */
static ray_t* grade_dict(ray_t* d, int desc) {
    ray_t* keys = ray_dict_keys(d);      /* borrowed */
    ray_t* vals = ray_dict_vals(d);      /* borrowed */
    if (!keys || !vals) return q_err(QE_TYPE);
    ray_t* grade = dict_value_grade(vals, desc);
    return reindex_collapse(keys, grade);             /* releases grade */
}

ray_t* q_iasc_wrap(ray_t* x) {
    if (q_type_is_dict(x))         return grade_dict(x, 0);
    if (q_type_is_table(x))        return grade_table(x, 0);
    if (x && x->type == RAY_LIST)  return grade_general(x, 0);
    return ray_iasc_fn(x);
}
ray_t* q_idesc_wrap(ray_t* x) {
    if (q_type_is_dict(x))         return grade_dict(x, 1);
    if (q_type_is_table(x))        return grade_table(x, 1);
    if (x && x->type == RAY_LIST)  return grade_general(x, 1);
    return ray_idesc_fn(x);
}

/* q `width xbar list` — interval bucketing.  rayfall ray_xbar_fn is (col,
 * bucket); q spells it (bucket, col), so swap the arguments.  Everything else
 * (numeric int/float bucket, temporal cols, list zip) is handled by the base
 * kernel; dict/keyed-table/qSQL forms fall through to whatever the base does. */
ray_t* q_xbar_wrap(ray_t* bucket, ray_t* col) {
    return ray_xbar_fn(col, bucket);
}

