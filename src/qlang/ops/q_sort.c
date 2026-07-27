/* ops/q_sort.c — the q grade family: iasc/idesc over vectors, dicts, tables and
 * keyed tables, plus xbar bucketing.
 *
 * Split from q_list.c (2026-07-27) — pure function moves, no behaviour change.
 * asc/desc/rank/xrank/xasc/xdesc are q.q derivations over these (QR_QSRC), not
 * C.  See q_registry.h for the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_err.h"
#include "lang/eval.h"     /* ray_at_fn, ray_xbar_fn */
#include "lang/internal.h" /* ray_iasc_fn / ray_idesc_fn */
#include <stdint.h>

/* ===== q grade / bucket family ============================================
 * GRADE IS THE PRIMITIVE: iasc/idesc own ordering for every structure (vector →
 * ray_iasc_fn; dict → keys by the value grade; table/keyed → grade_table), and
 * asc/desc/rank/xrank/xasc/xdesc are q.q derivations over them (index once, ONE
 * gather).  xbar is an arg-swap over ray_xbar_fn.  DEFERRED (error, never a
 * wrong answer): the `s#` attribute on asc results — the attr-take arm accepts
 * long vectors only, so setting it would regress symbol/nested sorts (PLAN.md)
 * — and the mixed-general-list-by-type-number sort (ray_iasc_fn 'types on 0h). */

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
        int64_t owned = 0;                           /* cols[0..owned) gathered, need release */
        ray_t* g = NULL;
        for (int64_t c = lo; c < hi; c++) {
            ray_t* col = ray_table_get_col_idx(t, c);         /* borrowed */
            if (col && perm) {
                ray_retain(perm);
                col = reindex_collapse(col, perm);          /* owned copy */
            }
            if (!col || RAY_IS_ERR(col)) {
                g = col ? col : q_err(QE_TYPE);
                break;
            }
            cols[c - lo] = col;
            descs[c - lo] = (uint8_t)desc;
            if (perm) owned++;
        }
        if (!g) {
            cols[k] = idx;             /* position: original on the first chunk, the
                                        * running minor order on later ones — either
                                        * way it is what must be preserved */
            descs[k] = 0;              /* ALWAYS ascending, even for idesc */
            g = ray_sort_indices(cols, descs, NULL, (uint8_t)(k + 1), nr);
        }
        for (int64_t c = 0; c < owned; c++) ray_release(cols[c]);
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

/* Grade a dict's VALUE vector.  Dict vals are stored as a boxed RAY_LIST, so
 * collapse to a typed vector before grading (a genuinely mixed value list can't
 * collapse and ray_iasc_fn errors → the by-type-number sort is DEFERRED). */
static ray_t* dict_value_grade(ray_t* vals, int desc) {
    ray_t* cv = (vals && vals->type == RAY_LIST) ? q_list_collapse(vals) : NULL;
    ray_t* use = cv ? cv : vals;
    /* A KEYED table's value is a table — grade its rows, not its columns (this is
     * what lets the dict law carry keyed tables: `asc kt` gathers key+value rows
     * by the grade of the value rows, per ref/asc.md's non-key-column rule). */
    if (use && use->type == RAY_TABLE) {
        ray_t* g = grade_table(use, desc);
        if (cv) ray_release(cv);
        return g;
    }
    /* Empty dict (e.g. `asc 0#d`): an empty value list can't be graded by
     * ray_iasc_fn (it needs a typed vector, and q_list_collapse leaves an empty
     * RAY_LIST as-is), so return an empty long grade — reindexing then yields an
     * empty dict / key list, matching kdb (codex review). */
    if (use && (ray_is_vec(use) || use->type == RAY_LIST) && ray_len(use) == 0) {
        if (cv) ray_release(cv);
        ray_t* g = ray_vec_new(RAY_I64, 0);
        if (g && !RAY_IS_ERR(g)) g->len = 0;
        return g;
    }
    ray_t* grade = desc ? ray_idesc_fn(use) : ray_iasc_fn(use);
    if (cv) ray_release(cv);
    return grade;
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
    if (x && x->type == RAY_DICT)  return grade_dict(x, 0);
    if (x && x->type == RAY_TABLE) return grade_table(x, 0);
    return ray_iasc_fn(x);
}
ray_t* q_idesc_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT)  return grade_dict(x, 1);
    if (x && x->type == RAY_TABLE) return grade_table(x, 1);
    return ray_idesc_fn(x);
}

/* q `width xbar list` — interval bucketing.  rayfall ray_xbar_fn is (col,
 * bucket); q spells it (bucket, col), so swap the arguments.  Everything else
 * (numeric int/float bucket, temporal cols, list zip) is handled by the base
 * kernel; dict/keyed-table/qSQL forms fall through to whatever the base does. */
ray_t* q_xbar_wrap(ray_t* bucket, ray_t* col) {
    return ray_xbar_fn(col, bucket);
}

