/* q_apply — see q_apply.h.  The per-head semantic table (spec 2c, revised
 * after adversarial codex review):
 *
 *   104h carrier      -> q_deriv_apply (Task 5; claimed FIRST — a carrier IS
 *                        a RAY_LIST and must not fall into the gather arm)
 *   dict              -> direct ray_dict_get; on a miss, the typed null of
 *                        the dict's VALUE type (ray_at would hardcode 0Nl)
 *   table             -> ray_at row/column access (base special-cases tables)
 *   typed vec / list  -> gather: ray_at (already kdb null-fill for
 *                        out-of-range/negative), then collapse a boxed
 *                        multi-index result to the typed vector
 *   string atom       -> DECLINE (ray_at implements char-indexing, but the
 *                        string-as-sequence model is deferred — routing
 *                        strings through would commit it by accident)
 *   everything else   -> DECLINE (caller's historic error, byte-identical)
 */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_apply.h"
#include "qlang/q_registry.h"   /* q_collapse_list */
#include "qlang/q_deriv.h"      /* q_deriv_kind_of (carrier arm, Task 5) */
#include "lang/eval.h"          /* ray_at_fn */
#include <string.h>

/* one indexing step: v[idx].  ray_at null-fills out-of-range; a
 * collection index yields a boxed list -> collapse to the typed vector. */
static ray_t* gather(ray_t* v, ray_t* idx) {
    ray_t* r = ray_at_fn(v, idx);
    if (!r || RAY_IS_ERR(r)) return r;
    if (r->type == RAY_LIST) {
        ray_t* c = q_collapse_list(r);
        ray_release(r);
        return c;
    }
    return r;
}

/* dict arm — DIRECT lookup (not ray_at, whose miss path hardcodes 0Nl): a
 * hit returns the stored value; a miss returns the typed null of the dict's
 * VALUE type (generic null for boxed/mixed values).  A vector of keys does a
 * per-key loop with those same miss semantics, then collapses. */
/* the typed-null type for a dict's values: a typed vector gives its element
 * type; a homogeneous boxed list of atoms gives their shared type (rayfall's
 * dict constructor stores values as a boxed list); anything mixed -> 0
 * (generic null, kdb's behaviour for mixed-value dict misses). */
static int8_t dict_val_null_type(ray_t* vals) {
    if (!vals) return 0;
    if (ray_is_vec(vals)) return (int8_t)-vals->type;
    if (vals->type == RAY_LIST && ray_len(vals) > 0) {
        ray_t** e = (ray_t**)ray_data(vals);
        if (!e[0] || e[0]->type >= 0) return 0;
        int8_t t = e[0]->type;
        for (int64_t i = 1; i < ray_len(vals); i++)
            if (!e[i] || e[i]->type != t) return 0;
        return t;
    }
    return 0;
}

static ray_t* dict_lookup(ray_t* d, ray_t* idx) {
    ray_t* vals = ray_dict_vals(d);                  /* borrowed accessor */
    int8_t vt = dict_val_null_type(vals);

    if (ray_is_vec(idx) || idx->type == RAY_LIST) {
        int64_t kn = ray_len(idx);
        ray_t* out = ray_list_new(kn > 0 ? kn : 1);
        for (int64_t i = 0; i < kn; i++) {
            ray_t* i_atom = ray_i64(i);
            ray_t* k = ray_at_fn(idx, i_atom);
            ray_release(i_atom);
            if (!k || RAY_IS_ERR(k)) { ray_release(out); return k; }
            ray_t* v = ray_dict_get(d, k);
            ray_release(k);
            if (!v) v = vt ? ray_typed_null(vt)
                           : (ray_retain(RAY_NULL_OBJ), RAY_NULL_OBJ);
            out = ray_list_append(out, v);
            ray_release(v);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }

    ray_t* v = ray_dict_get(d, idx);                 /* owned, or C NULL */
    if (v) return v;
    return vt ? ray_typed_null(vt)
              : (ray_retain(RAY_NULL_OBJ), RAY_NULL_OBJ);
}

ray_t* q_apply_noun(ray_t* head, ray_t** args, int64_t n) {
    if (!head || n < 1) return NULL;

    if (head->type == -RAY_STR) return NULL;        /* deferred: string model */

    if (head->type == RAY_DICT) {
        if (n != 1) return NULL;                    /* d[k;..] deferred */
        return dict_lookup(head, args[0]);
    }

    if (head->type == RAY_TABLE) {
        /* t[`col] -> column, t[0] -> row dict — base ray_at special-cases
         * tables and both forms audited kdb-sane. */
        if (n != 1) return NULL;
        return ray_at_fn(head, args[0]);
    }

    if (ray_is_vec(head) || head->type == RAY_LIST) {
        /* v[i] / v[1 3] / v i; v[i;j] = depth indexing, one gather per arg */
        ray_t* cur = head;
        ray_retain(cur);
        for (int64_t i = 0; i < n; i++) {
            ray_t* next = gather(cur, args[i]);
            ray_release(cur);
            if (!next || RAY_IS_ERR(next)) return next;
            cur = next;
        }
        return cur;
    }

    return NULL;
}
