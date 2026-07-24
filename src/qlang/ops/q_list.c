/* ops/q_list.c — list verbs: reshape/take/drop/cut, column attributes,
 * raze/enlist, til/where/reverse, rotate/sublist/next/prev/fills/fill/in,
 * sort/grade/bucket, find
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#define Q_OPS_ENV_GRANDFATHER /* grandfathered 2026-07-23: 2 env uses — q-index PR audit */
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_err.h"
#include "lang/env.h"      /* ray_env_get — q_env_call2 */
#include "lang/eval.h"     /* ray_take_fn, ray_in_fn, ray_find_fn, ray_xbar_fn */
#include "lang/internal.h" /* ray_iasc_fn/ray_idesc_fn, RAY_IS_TEMPORAL64, ray_error */
#include "ops/ops.h"       /* ray_is_lazy, ray_lazy_materialize */
#include "ops/idxop.h"     /* .attr.* engine calls: ray_attr_*, RAY_IDX_*, RAY_MARK_* (column attributes) */
#include "mem/heap.h"      /* RAY_ATTR_HAS_NULLS — ? find miss remap */
#include "table/sym.h"     /* ray_sym_intern_runtime, RAY_SYM_W64 */
#include <stdint.h>        /* uintptr_t */
#include <string.h>
#include <stdlib.h>        /* malloc/free */

/* ---- collapse: homogeneous atom list -> typed vector (q_registry_internal.h) ---- */

ray_t* q_list_collapse(ray_t* l) {
    if (!l || RAY_IS_ERR(l) || l->type != RAY_LIST || ray_len(l) == 0) {
        if (l) ray_retain(l);
        return l;
    }
    int64_t n = ray_len(l);
    ray_t** e = (ray_t**)ray_data(l);
    int8_t t = e[0] ? e[0]->type : 0;
    if (t >= 0 || t == -RAY_STR) { ray_retain(l); return l; }   /* not a scalar-atom run */
    for (int64_t i = 1; i < n; i++)
        if (!e[i] || e[i]->type != t) { ray_retain(l); return l; }

    if (t == -RAY_SYM) {
        ray_t* vec = ray_sym_vec_new(RAY_SYM_W64, n);
        if (RAY_IS_ERR(vec)) return vec;
        for (int64_t i = 0; i < n; i++) vec = ray_vec_append(vec, &e[i]->i64);
        return vec;
    }

    ray_t* vec = ray_vec_new(-t, n);
    if (RAY_IS_ERR(vec)) return vec;
    int64_t nulls = 0;
    for (int64_t i = 0; i < n; i++) {
        /* Switch the recovered POSITIVE tag, exhaustive over ray_type_e (no
         * default) so a future member demands a lane (#209 guard).  Only the
         * non-i64 reads live here; i64/temporal tags AND any out-of-enum tag
         * fall to the shared i64 append below — byte-identical to the old
         * default (U8 LE-aliased).  LIST/STR/SYM are dead arms (filtered above). */
        bool appended = false;
        switch ((ray_type_e)-t) {
        case RAY_BOOL: vec = ray_vec_append(vec, &e[i]->b8);  appended = true; break;
        case RAY_I16:  vec = ray_vec_append(vec, &e[i]->i16); appended = true; break;
        case RAY_I32:  vec = ray_vec_append(vec, &e[i]->i32); appended = true; break;
        case RAY_F32: { float f = (float)e[i]->f64;           /* F32 atom stores f64 */
                        vec = ray_vec_append(vec, &f); appended = true; } break;
        case RAY_F64:
        case RAY_DATETIME:
                       vec = ray_vec_append(vec, &e[i]->f64); appended = true; break;
        case RAY_GUID: {                                      /* 16-byte payload, not i64 */
            const void* g = e[i]->obj ? ray_data(e[i]->obj) : ray_data(e[i]);
            vec = ray_vec_append(vec, g); appended = true;
        } break;
        RAY_BYTE_CASES: /* explicit u8 read — byte + char atoms store the payload
                         * in u8; no LE-aliasing through the shared i64 append */
                       vec = ray_vec_append(vec, &e[i]->u8); appended = true; break;
        case RAY_I64:  case RAY_TIMESTAMP: case RAY_MONTH:
        case RAY_DATE: case RAY_TIMESPAN: case RAY_MINUTE:    case RAY_SECOND:
        case RAY_TIME: case RAY_LIST: case RAY_STR: case RAY_SYM:
                       break;
        }
        if (!appended) vec = ray_vec_append(vec, &e[i]->i64);   /* i64/temporal + out-of-enum */
        if (RAY_IS_ERR(vec)) return vec;
        if (RAY_ATOM_IS_NULL(e[i])) { ray_vec_set_null(vec, i, true); nulls++; }
    }
    (void)nulls;
    return vec;
}

/* ---- wrappers (bespoke q semantics over a rayfall primitive) ---- */

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

/* ── Column attributes (`` `s#`u#`g#`p# ``) — the q surface over the engine's
 * built accelerator-index family (.attr.*).  See actionable-plans/
 * 2026-07-09-column-attributes.md.  Two entry points: q_attr_letter (shared by
 * the `attr` verb AND q_fmt's display prefix) and the `#` set-attribute arm. ── */

/* Read a vector's attribute as kdb's SINGLE letter: 's'/'u'/'g'/'p', or 0 for
 * none.  Reads the block markers/kind DIRECTLY rather than delegating to the
 * engine's `.attr.get` (ray_attr_get_fn is now rayfall-native and would mislabel
 * q's hash-backed `u#`/`p#` — which carry RAY_IDX_HASH + a marker — as `g`).
 * The kdb u#/p# policy is composed in q (q_attr_compose): the marker bit is the
 * attribute identity, winning over the hash kind; a bare hash is `g`, a native
 * RAY_IDX_PART directory is `p`, and the attrs sorted bit is `s`.  Borrows v;
 * never releases it. */
char q_attr_letter(ray_t* v) {
    if (!v || RAY_IS_ERR(v) || !ray_is_vec(v)) return 0;
    if (ray_index_has(v)) {
        ray_index_t* ix = ray_index_payload(v->index);
        if (ix->markers & RAY_MARK_UNIQUE) return 'u';
        if (ix->markers & RAY_MARK_PARTED) return 'p';
        if (ix->kind == RAY_IDX_HASH) return 'g';
        if (ix->kind == RAY_IDX_PART) return 'p';
    }
    if (ray_attr_is_sorted(v)) return 's';
    return 0;
}

/* q `attr x` — the column attribute as a symbol atom (`` ` ``/`s`/`u`/`g`/`p`).
 * Atoms and unattributed vectors return the empty symbol.  Borrows x.
 * Exported (q_registry.h) so the `-2!` internal-fn alias single-homes here. */
ray_t* q_attr_wrap(ray_t* x) {
    char c = q_attr_letter(x);
    char s1[1]; s1[0] = c;
    int64_t id = ray_sym_intern_runtime(c ? s1 : "", c ? 1 : 0);
    return ray_sym(id);
}

/* Remap the engine's set-attribute failure codes to kdb's error text.  The
 * verification failures use "domain"; the numeric-only gate uses "nyi"; type
 * guards use "type".  kdb signals: `'s-fail` (sorted not ascending), `'u-fail`
 * (unique not distinct OR parted not contiguous — SHARED), `'type` (wrong type
 * / non-numeric).  There is deliberately NO `p-fail` (ref set-attribute.md).
 * Consumes err, returns a fresh error; passes oom/unexpected codes through. */
static ray_t* attr_remap_err(ray_t* err, char letter) {
    const char* code = ray_err_code(err);
    const char* sig = NULL;
    if (code) {
        if (strcmp(code, "domain") == 0)
            sig = (letter == 's') ? "s-fail"
                : (letter == 'u' || letter == 'p') ? "u-fail"
                : "type";
        else if (strcmp(code, "nyi") == 0 || strcmp(code, "type") == 0)
            sig = "type";
    }
    if (!sig) return err;                            /* oom / unexpected */
    ray_error_free(err);
    return ray_error(sig, NULL);
}

/* kdb `u#`: a null is a value, so a column with TWO OR MORE nulls is not unique
 * (`` `u#0N 0N `` -> `'u-fail`).  The neutral engine ray_attr_verify_distinct is
 * rayfall-native and only checks NON-null distinctness (it skips nulls); this
 * q-side pass adds the kdb null policy so the engine stays untouched.  A single
 * null is fine.  Only scans when the column actually carries nulls. */
static bool q_no_dup_nulls(ray_t* v) {
    if (!(v->attrs & RAY_ATTR_HAS_NULLS)) return true;
    int64_t nulls = 0, n = ray_len(v);
    for (int64_t i = 0; i < n; i++)
        if (ray_vec_is_null(v, i) && ++nulls > 1) return false;
    return true;
}

/* Compose the kdb `u#`/`p#` accelerator on a cleared base column.  This is the
 * kdb POLICY that used to live in the frozen engine (idxop.c commit 27a8700a):
 * verify the layout, then attach a find-hash (integer-family) or a marker-only
 * assertion (float), stamping the identity marker.  Built from neutral engine
 * primitives (ray_attr_numeric_class / verify / ray_idx_hash_fn /
 * ray_attr_stamp_marker) so rayfall's native `.attr.*` is untouched.  Borrows
 * base (stays owned by the caller); returns an owned result carrying RAW engine
 * error codes (caller remaps via attr_remap_err). */
static ray_t* q_attr_compose(ray_t* base, char letter) {
    int cls = ray_attr_numeric_class(base->type);
    if (cls < 0) return q_err(QE_TYPE);
    bool ok = (letter == 'u') ? (ray_attr_verify_distinct(base) && q_no_dup_nulls(base))
                              : ray_attr_verify_contiguous(base);
    if (!ok) return q_err(QE_DOMAIN);
    uint8_t mark = (letter == 'u') ? RAY_MARK_UNIQUE : RAY_MARK_PARTED;
    if (cls == 1) {                              /* integer-family: find-hash + marker */
        ray_t* hv = ray_idx_hash_fn(base);       /* borrows base, owned out */
        if (!hv || RAY_IS_ERR(hv)) return hv ? hv : q_err(QE_OOM);
        ray_t* w = ray_attr_stamp_marker(hv, mark);  /* borrows hv, owned out */
        ray_release(hv);
        return w;
    }
    return ray_attr_stamp_marker(base, mark);    /* float: marker only */
}

/* q `sym # vec` — set / clear a column attribute.  sym is a symbol ATOM: the
 * empty symbol clears (`.attr.drop`), `s`/`u`/`g`/`p` set the matching
 * attribute, any other letter is `'type` (the 5-symbol allow-list IS the guard —
 * a non-attribute symbol against a flat vector has no take meaning).  `s`/`g` go
 * through the rayfall-native engine setter (sorted marker / grouped hash already
 * match kdb); `u`/`p` are composed in q (q_attr_compose) so the kdb accelerator
 * policy stays out of the frozen engine.  Borrows both args. */
static ray_t* attr_set_dispatch(ray_t* n, ray_t* vec) {
    char letter = '?';                               /* unknown -> 'type */
    ray_t* s = ray_sym_str(n->i64);                  /* owned -RAY_STR */
    if (s) {
        size_t len = ray_str_len(s);
        if (len == 0) letter = 0;                    /* `#x -> clear */
        else if (len == 1) letter = ray_str_ptr(s)[0];
        ray_release(s);
    }
    const char* attr_name = NULL;
    switch (letter) {
    case 0:   return ray_attr_drop_fn(vec);          /* `#vec -> drop all */
    case 'u': case 'p': {
        /* kdb accelerator policy composed in q (see q_attr_compose).  `` `x# ``
         * REPLACES any prior attribute, so clear the base first. */
        ray_t* base = ray_attr_drop_fn(vec);         /* clear prior attr, owned */
        if (!base || RAY_IS_ERR(base)) return base ? base : q_err(QE_OOM);
        ray_t* r = q_attr_compose(base, letter);     /* borrows base */
        ray_release(base);
        if (r && RAY_IS_ERR(r)) return attr_remap_err(r, letter);
        return r;
    }
    case 's': attr_name = "sorted";  break;
    case 'g': attr_name = "grouped"; break;
    default:  return q_err(QE_TYPE);        /* `z#vec etc. */
    }
    int64_t aid = ray_sym_intern_runtime(attr_name, strlen(attr_name));
    ray_t* nm = ray_sym(aid);                         /* owned -RAY_SYM */
    /* kdb: a vector carries at most ONE attribute, and `` `x# `` REPLACES any
     * prior one (`` attr `g#`s#1 2 3 `` -> `` `g ``).  The rayfall-native setter
     * preserves existing markers/index (sorted survives attach), so drop first,
     * then set on the cleared base.  ray_attr_drop_fn borrows vec and returns an
     * owned (possibly COW'd) result; ray_attr_set_fn borrows that base. */
    ray_t* base = ray_attr_drop_fn(vec);              /* owned */
    if (!base || RAY_IS_ERR(base)) { ray_release(nm); return base ? base : q_err(QE_OOM); }
    ray_t* r = ray_attr_set_fn(nm, base);             /* borrows nm, base */
    ray_release(nm);
    ray_release(base);
    if (r && RAY_IS_ERR(r)) return attr_remap_err(r, letter);
    return r;
}

/* Public (test-facing) entry over attr_set_dispatch: build the single-char
 * symbol the `#` set-attribute arm expects and dispatch.  letter 0 clears.
 * Borrows vec; returns owned.  Lets the acceleration C-unit exercise the real q
 * u#/p# compose path (find-hash + marker) rather than the reverted engine call. */
ray_t* q_attr_set_letter(char letter, ray_t* vec) {
    char s1[1]; s1[0] = letter;
    int64_t id = ray_sym_intern_runtime(letter ? s1 : "", letter ? 1 : 0);
    ray_t* n = ray_sym(id);                           /* owned -RAY_SYM */
    ray_t* r = attr_set_dispatch(n, vec);           /* borrows n, vec */
    ray_release(n);
    return r;
}

/* q `n # list` — take.  A SYMBOL ATOM left arg against a simple vector is
 * set-attribute (attr_set_dispatch) — the only meaning, since you cannot
 * key-take from a flat vector; a symbol atom against a dict/table stays
 * key/column take (falls through).  An int-VECTOR left arg (len>=2) is RESHAPE
 * (matrix); an atom is take.  rayfall ray_take_fn(vec, n) has the opposite arg
 * order, so swap.  Borrows both args (does not release them). */
ray_t* q_take_wrap(ray_t* n, ray_t* list) {
    if (n && n->type == -RAY_SYM && list && ray_is_vec(list))
        return attr_set_dispatch(n, list);
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
    return ray_take_fn(list, n);
}

/* q `n _ list` — count-drop (NOT rayfall's dict key-remove).  n>=0 drops the
 * first n elements; n<0 drops the last |n|.  Implemented as a range-take
 * ray_take_fn(list, (start; amount)), which clamps at the ends.  Borrows args.
 *
 * Length is derived string-aware: a q string is a -RAY_STR atom whose char
 * count lives in ray_str_len, NOT the ->len union field (which aliases the SSO
 * {slen,sdata} bytes), so ray_len would be garbage for strings. */
/* q_list_collapse leaves a ZERO-length boxed list untyped (no element to infer
 * from); key-indexing selections must instead inherit the PROTO vector's type
 * so an empty result keeps its domain (codex r3: `` type key `a _ `a!1 `` must
 * be 11h / `` `symbol$() ``, not 0h / `()`).  Consumes `collapsed`, borrows
 * `proto`; passes errors and non-empty results through untouched. */
ray_t* q_typed_empty_like(ray_t* collapsed, ray_t* proto) {
    if (!collapsed || RAY_IS_ERR(collapsed)) return collapsed;
    if (collapsed->type != RAY_LIST || ray_len(collapsed) != 0) return collapsed;
    if (!proto || !ray_is_vec(proto) || proto->type == RAY_LIST) return collapsed;
    ray_t* tv = (proto->type == RAY_SYM) ? ray_sym_vec_new(RAY_SYM_W64, 0)
                                         : ray_vec_new(proto->type, 0);
    if (!tv || RAY_IS_ERR(tv)) { if (tv) ray_release(tv); return collapsed; }
    ray_release(collapsed);
    return tv;
}

ray_t* q_drop_wrap(ray_t* n, ray_t* list) {
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
    if (!list) return q_err(QE_TYPE);
    int64_t len;
    if (list->type == -RAY_STR)
        len = (int64_t)ray_str_len(list);           /* SSO-safe string length */
    else if (ray_is_vec(list) || list->type == RAY_LIST)
        len = ray_len(list);                         /* typed vector / boxed list */
    else
        return q_err(QE_TYPE);
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

/* q `n cut x` — cut into pieces (kdb ref/cut).  An int ATOM chunks x into
 * groups of n (ragged last group: `4 cut til 10` -> (0 1 2 3;4 5 6 7;8 9)).
 * An int VECTOR is a positional cut — identical to `_`, so delegate.  Borrows. */
ray_t* q_cut_wrap(ray_t* n, ray_t* x) {
    int64_t sz;
    if (q_type_strict_i64(n, &sz)) {
        if (sz <= 0) return q_err(QE_DOMAIN);
        int is_str = (x && x->type == -RAY_STR);
        int64_t total = is_str ? (int64_t)ray_str_len(x) : (x ? ray_len(x) : 0);
        if (!x || (!is_str && !ray_is_vec(x) && x->type != RAY_LIST))
            return q_err(QE_TYPE);
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
/* q `raze x` — base ray_raze_fn plus the kdb atom arm: an atom comes back as
 * a 1-item list (ref/raze.md `raze 42` -> ,42).  Everything else delegates. */
ray_t* q_raze_wrap(ray_t* x) {
    /* strings are kdb char LISTS (rank 1) — never the atom arm */
    if (x && ray_is_atom(x) && x->type != -RAY_STR) {
        ray_t* l = ray_list_new(1);
        if (RAY_IS_ERR(l)) return l;
        l = ray_list_append(l, x);                   /* retains x */
        if (RAY_IS_ERR(l)) return l;
        ray_t* c = q_list_collapse(l);               /* owned */
        ray_release(l);
        return c;
    }
    return ray_raze_fn(x);
}

/* q `enlist` — base ray_enlist_fn plus the kdb dict arm: enlist of a bare
 * dict is a 1-ROW TABLE (ref/enlist.md: `` enlist `a`b`c!(1;2 3; 4) ``
 * displays a table whose b cell is 2 3).  Construction: the dict with each
 * value ENLISTED (1-item column; atoms collapse to typed 1-vecs, vector
 * cells stay boxed) flipped through the one flip home.  Env-bound by
 * q_builtins_register BEFORE registry init, so the `,` monadic QK_ENV
 * snapshot picks this wrapper up too. */
ray_t* q_enlist_wrap(ray_t** args, int64_t n) {
    if (n == 1 && args[0] && args[0]->type == RAY_DICT && !q_type_is_keyed(args[0])) {
        ray_t* d = args[0];
        ray_t* k = ray_dict_keys(d);                 /* borrowed */
        ray_t* v = ray_dict_vals(d);                 /* borrowed */
        if (!k || !v) return q_err(QE_TYPE);
        int64_t nd = ray_dict_len(d);
        ray_t* ev = ray_list_new(nd > 0 ? nd : 1);
        if (RAY_IS_ERR(ev)) return ev;
        for (int64_t i = 0; i < nd; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* cell = ray_at_fn(v, ia);          /* owned */
            ray_release(ia);
            if (!cell || RAY_IS_ERR(cell)) { ray_release(ev); return cell ? cell : q_err(QE_TYPE); }
            ray_t* col = ray_list_new(1);
            if (RAY_IS_ERR(col)) { ray_release(cell); ray_release(ev); return col; }
            col = ray_list_append(col, cell);        /* retains */
            ray_release(cell);
            if (RAY_IS_ERR(col)) { ray_release(ev); return col; }
            ray_t* cc = q_list_collapse(col);        /* atoms -> typed 1-vec */
            ray_release(col);
            if (!cc || RAY_IS_ERR(cc)) { ray_release(ev); return cc ? cc : q_err(QE_TYPE); }
            ev = ray_list_append(ev, cc);            /* retains */
            ray_release(cc);
            if (RAY_IS_ERR(ev)) return ev;
        }
        ray_retain(k);                               /* dict_new consumes */
        ray_t* ed = ray_dict_new(k, ev);             /* consumes k + ev */
        if (!ed || RAY_IS_ERR(ed)) return ed ? ed : q_err(QE_TYPE);
        ray_t* t = q_flip_wrap(ed);                  /* owned table */
        ray_release(ed);
        return t;
    }
    return ray_enlist_fn(args, n);
}

/* q `til` — kdb accepts a boolean (`til 1b` -> ,0); base ray_til_fn is
 * int-only.  Everything else (int atoms, the error paths) delegates. */
ray_t* q_til_wrap(ray_t* x) {
    if (x && x->type == -RAY_BOOL) {
        ray_t* n = ray_i64(x->b8 ? 1 : 0);
        ray_t* r = ray_til_fn(n);
        ray_release(n);
        return r;
    }
    return ray_til_fn(x);
}

/* q `where` / monadic `&` — an INTEGER vector repeats each index i, x[i] times
 * (`where 2 3 1` -> 0 0 1 1 1 2; `where 0 1 0 1 0 1` -> 1 3 5).  Base
 * ray_where_fn handles the boolean-mask form, so delegate for it and anything
 * else.  Result is a long vector (kdb).  Negative counts are 'domain. */
ray_t* q_where_wrap(ray_t* x) {
    if (x && (x->type == RAY_I64 || x->type == RAY_I32 || x->type == RAY_I16)) {
        int64_t n = ray_len(x);
        int64_t total = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t c = (x->type == RAY_I64) ? ((const int64_t*)ray_data(x))[i]
                      : (x->type == RAY_I32) ? (int64_t)((const int32_t*)ray_data(x))[i]
                      : (int64_t)((const int16_t*)ray_data(x))[i];
            if (c < 0) return q_err(QE_DOMAIN);
            total += c;
        }
        ray_t* out = ray_vec_new(RAY_I64, total);
        if (RAY_IS_ERR(out)) return out;
        out->len = total;
        int64_t* d = (int64_t*)ray_data(out);
        int64_t w = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t c = (x->type == RAY_I64) ? ((const int64_t*)ray_data(x))[i]
                      : (x->type == RAY_I32) ? (int64_t)((const int32_t*)ray_data(x))[i]
                      : (int64_t)((const int16_t*)ray_data(x))[i];
            for (int64_t k = 0; k < c; k++) d[w++] = i;
        }
        return out;
    }
    return ray_where_fn(x);
}


/* ===== q list verbs: xprev / fill (`^`) ====================================
 * ref next.md, fill.md; deferred forms error, never wrong-answer
 * (*-deferred.qcmd companions).  `rotate` and `sublist` are self-hosted in
 * q.q over `#`/`_`. */

/* q `n xprev x` — n-item shift, null-filling the vacated end (ref/next.md:
 * +n is prev-by-n, -n is next); `next`/`prev` are its q.q unit shifts, so
 * every arm here is theirs too.  Strings shift CHARS with ' ' fill
 * (`1 xprev "abcde"` -> " abcd"); a generic LIST fills each vacated slot
 * with `0#first` of the ORIGINAL (`prev (1 2;"abc";`ibm)` -> (`long$();1 2;"abc")). */
ray_t* q_xprev_wrap(ray_t* nx, ray_t* x) {
    int64_t k;   /* strict cast owns the type axis; 0N rejected here */
    if (!q_type_strict_i64(nx, &k) || RAY_ATOM_IS_NULL(nx))
        return q_err(QE_TYPE);
    if (x && x->type == RAY_LIST) {
        /* fill = 0#first (a take that cannot empty degrades to ()) */
        int64_t len = ray_len(x);
        if (len == 0) { ray_retain(x); return x; }
        int64_t sh = k >= 0 ? k : -k;
        if (sh > len) sh = len;
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* zero = ray_i64(0);
        ray_t* fill = e[0] ? ray_take_fn(e[0], zero) : NULL;   /* owned 0#first */
        ray_release(zero);
        if (!fill || RAY_IS_ERR(fill)) {
            if (fill) ray_release(fill);
            fill = ray_list_new(1);                  /* empty () fallback */
            if (RAY_IS_ERR(fill)) return fill;
        }
        ray_t* out = ray_list_new(len);
        if (RAY_IS_ERR(out)) { ray_release(fill); return out; }
        int64_t keep = len - sh;
        for (int64_t i = 0; i < len; i++) {
            /* prev (k>=0): fills lead; next: fills trail */
            ray_t* item = (k >= 0) ? (i < sh ? fill : e[i - sh])
                                   : (i < keep ? e[i + sh] : fill);
            out = ray_list_append(out, item);        /* retains */
            if (RAY_IS_ERR(out)) { ray_release(fill); return out; }
        }
        ray_release(fill);
        return out;
    }
    if (x && (x->type == -RAY_STR || x->type == RAY_CHARV)) {
        const char* s; int64_t len;
        (void)q_str_text_bytes(x, &s, &len);
        if (x->type == -RAY_STR) { s = ray_str_ptr(x); len = (int64_t)ray_str_len(x); }
        char stackb[256];
        char* b = (len <= (int64_t)sizeof stackb) ? stackb : malloc((size_t)(len > 0 ? len : 1));
        if (!b) return q_err(QE_OOM);
        for (int64_t i = 0; i < len; i++) {
            int64_t j = i - k;
            b[i] = (j >= 0 && j < len) ? s[j] : ' ';   /* char null is the blank */
        }
        ray_t* r = (x->type == RAY_CHARV) ? ray_charv(b, len) : ray_str(b, (size_t)len);
        if (b != stackb) free(b);
        return r;
    }
    if (!x || !ray_is_vec(x))
        return q_err(QE_NYI);
    int8_t t = x->type;
    if (!(t == RAY_I16 || t == RAY_I32 || t == RAY_I64 || t == RAY_F32 || t == RAY_F64 ||
          RAY_IS_TEMPORAL32(t) || RAY_IS_TEMPORAL64(t) || RAY_IS_TEMPORALF(t)))
        return q_err(QE_NYI);
    int64_t len = ray_len(x);
    size_t esz = ray_type_sizes[(uint8_t)t];
    ray_t* out = ray_vec_new(t, len > 0 ? len : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = len;
    char* o = (char*)ray_data(out);
    const char* in = (const char*)ray_data(x);
    int64_t sh = k >= 0 ? k : -k;
    if (sh > len) sh = len;
    int64_t keep = len - sh;
    if (k >= 0) {                                    /* prev-by-n: head nulls */
        if (keep > 0) memcpy(o + (size_t)sh * esz, in, (size_t)keep * esz);
        for (int64_t i = 0; i < sh; i++) ray_vec_set_null(out, i, true);
    } else {                                         /* next-by-n: tail nulls */
        if (keep > 0) memcpy(o, in + (size_t)sh * esz, (size_t)keep * esz);
        for (int64_t i = keep; i < len; i++) ray_vec_set_null(out, i, true);
    }
    return out;
}

/* q `fills x` — forward-fill: each null takes the last preceding non-null
 * (ref/fill.md; `fills` is the `^\` fill-scan).  Leading nulls stay null.
 * Numeric vectors keep q_fill_wrap's I64/F64 result split; SYM vectors carry
 * the last non-null sym id (id 0 IS q's null sym, same test as q_fill_wrap).
 * Atoms pass through; other shapes are deferred cells. */
ray_t* q_fills_wrap(ray_t* x) {
    if (x && ray_is_atom(x)) { ray_retain(x); return x; }
    if (x && x->type == RAY_SYM) {
        int64_t n = ray_len(x);
        ray_t* outl = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(outl)) return outl;
        int64_t carry = 0;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* se = ray_at_fn(x, ia);            /* owned sym atom */
            ray_release(ia);
            if (!se || RAY_IS_ERR(se)) { ray_release(outl); return se; }
            int64_t id = se->i64;
            ray_release(se);
            if (id != 0) carry = id;                 /* 0 == null sym */
            ray_t* oe = ray_sym(carry);
            outl = ray_list_append(outl, oe);
            ray_release(oe);
            if (RAY_IS_ERR(outl)) return outl;
        }
        ray_t* c = q_list_collapse(outl);
        ray_release(outl);
        return c;
    }
    if (!x || !q_vec_is_num(x))
        return q_err(QE_NYI);
    int isf = q_vec_is_float(x);
    int64_t n = ray_len(x);
    ray_t* out = ray_vec_new(isf ? RAY_F64 : RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    void* o = ray_data(out);
    double carry = 0; int have = 0;
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        if (!nu) { carry = v; have = 1; }
        int isnull = nu && !have;
        double use = nu ? carry : v;
        if (isf) ((double*)o)[i]  = isnull ? NULL_F64 : use;
        else     ((int64_t*)o)[i] = isnull ? NULL_I64 : (int64_t)use;
        if (isnull) ray_vec_set_null(out, i, true);
    }
    return out;
}

/* Whole-item scan: does any ITEM of container y match v (kdb `~`)?  Indexes
 * via ray_at_fn so typed vectors (STR lists-of-strings included) and boxed
 * lists share one home.  Borrows both. */
static int seq_has_item(ray_t* y, ray_t* v) {
    int64_t n = ray_len(y);
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* ye = ray_at_fn(y, ia);                /* owned item */
        ray_release(ia);
        if (!ye || RAY_IS_ERR(ye)) { if (ye) ray_release(ye); continue; }
        int hit = v && (ye == v || atom_eq(ye, v));
        ray_release(ye);
        if (hit) return 1;
    }
    return 0;
}

/* q `x^y` — fill: coalesce nulls in y with x (kdb `^`).  x may be an atom
 * (broadcast) or a same-length vector (element-wise).  Numeric result type:
 * F64 if EITHER operand is float, else I64 (the narrower-int-preserving lattice
 * is a deferred refinement — the `type` ledger rows that need it are blocked by
 * a separate `0n 2 3i` parse bug and split out).  Symbol fill is a distinct
 * path.  Dict / `fills` forward-fill / table / fill-scan forms are deferred. */
static int is_float_t(int8_t t) {
    return t == RAY_F64 || t == RAY_F32 || t == -RAY_F64 || t == -RAY_F32;
}
static int is_num_t(int8_t t) {
    return t == RAY_BOOL || t == RAY_BYTE_ONLY || t == RAY_I16 || t == RAY_I32 || t == RAY_I64 ||
           t == RAY_F32 || t == RAY_F64 || t == -RAY_BOOL || t == -RAY_BYTE_ONLY || t == -RAY_I16 ||
           t == -RAY_I32 || t == -RAY_I64 || t == -RAY_F32 || t == -RAY_F64;
}
static int is_sym_t(int8_t t) { return t == RAY_SYM || t == -RAY_SYM; }

ray_t* qj_ktbl_merge(ray_t* x, ray_t* y, int mode);   /* joins wave */

ray_t* q_fill_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    /* keyed^keyed is the uj merge with fill semantics (ref/coalesce.md:
     * y records update x's, but y NULLS don't overwrite). */
    if (q_type_is_keyed(x) && q_type_is_keyed(y))
        return qj_ktbl_merge(x, y, 1);
    int xatom = ray_is_atom(x), yatom = ray_is_atom(y);

    /* ---- symbol fill ---- */
    if (is_sym_t(x->type) || is_sym_t(y->type)) {
        if (!is_sym_t(x->type) || !is_sym_t(y->type))
            return q_err(QE_TYPE);
        /* length follows y when it is a vector; a scalar y broadcasts to the
         * length of a vector x (`` `a`b`c^` `` -> 3 items), matching the
         * numeric branch below. */
        int64_t len = yatom ? (xatom ? 1 : ray_len(x)) : ray_len(y);
        if (!xatom && !yatom && ray_len(x) != len)
            return q_err(QE_LENGTH);
        ray_t* outl = ray_list_new(len > 0 ? len : 1);
        if (RAY_IS_ERR(outl)) return outl;
        for (int64_t i = 0; i < len; i++) {
            int64_t yid;
            if (yatom) yid = y->i64;
            else { ray_t* ia = ray_i64(i); ray_t* ye = ray_at_fn(y, ia); ray_release(ia);
                   if (!ye || RAY_IS_ERR(ye)) { ray_release(outl); return ye; }
                   yid = ye->i64; ray_release(ye); }
            int64_t use = yid;
            if (yid == 0) {                      /* empty/null sym -> fill */
                if (xatom) use = x->i64;
                else { ray_t* ia = ray_i64(i); ray_t* xe = ray_at_fn(x, ia); ray_release(ia);
                       if (!xe || RAY_IS_ERR(xe)) { ray_release(outl); return xe; }
                       use = xe->i64; ray_release(xe); }
            }
            ray_t* se = ray_sym(use);
            outl = ray_list_append(outl, se); ray_release(se);
            if (RAY_IS_ERR(outl)) return outl;
        }
        ray_t* c = q_list_collapse(outl); ray_release(outl);
        if (yatom && xatom) {                    /* scalar^scalar -> atom */
            ray_t* ia = ray_i64(0); ray_t* a = ray_at_fn(c, ia); ray_release(ia); ray_release(c);
            return a;
        }
        return c;
    }

    /* ---- numeric fill ---- */
    if (!is_num_t(x->type) || !is_num_t(y->type))
        return q_err(QE_TYPE);
    int is_float = is_float_t(x->type) || is_float_t(y->type);
    int64_t len = yatom ? (xatom ? 1 : ray_len(x)) : ray_len(y);
    if (!xatom && !yatom && ray_len(x) != ray_len(y))
        return q_err(QE_LENGTH);
    if (xatom && yatom) {                        /* scalar^scalar -> atom */
        int yn; double yv = q_velem_f(y, 0, &yn);
        if (!yn) return is_float ? ray_f64(yv) : ray_i64((int64_t)yv);
        int xn; double xv = q_velem_f(x, 0, &xn);
        if (xn) return ray_typed_null(is_float ? -RAY_F64 : -RAY_I64);
        return is_float ? ray_f64(xv) : ray_i64((int64_t)xv);
    }
    ray_t* out = ray_vec_new(is_float ? RAY_F64 : RAY_I64, len > 0 ? len : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = len;
    void* o = ray_data(out);
    for (int64_t i = 0; i < len; i++) {
        int yn; double yv = yatom ? q_velem_f(y, 0, &yn) : q_velem_f(y, i, &yn);
        double v; int isnull = 0;
        if (!yn) v = yv;
        else {
            int xn; double xv = xatom ? q_velem_f(x, 0, &xn) : q_velem_f(x, i, &xn);
            if (xn) { v = 0; isnull = 1; } else v = xv;
        }
        if (is_float) ((double*)o)[i] = isnull ? NULL_F64 : v;
        else          ((int64_t*)o)[i] = isnull ? NULL_I64 : (int64_t)v;
        if (isnull) ray_vec_set_null(out, i, true);
    }
    return out;
}

/* q `x in y` — membership (ref/in.md).  Where y is a TYPED vector the test is
 * left-atomic (delegates to base ray_in_fn); where y is a generic LIST there
 * is NO iteration through x — x is tested WHOLE against the ITEMS of y, and
 * the search is rank-sensitive via y's FIRST item (find.md: a rank-n haystack
 * looks for rank n-1 objects): first item non-atom -> whole-x match (a rank-0
 * x is 0b: `3 in (1 2;3)` -> 0b); first item atom (or empty y — undocumented
 * edge, conservative) -> left-atomic over x against y's items.  Mixed numeric
 * families (float x vs int y) are allowed only against an ATOM or 1-item y
 * (elementwise equality); longer/empty mixed vectors are 'type.  A 1-char
 * string x against string y unwraps the base char row to an ATOM bool. */
ray_t* q_in_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    if (y->type == RAY_LIST) {
        int64_t ny = ray_len(y);
        ray_t** e = (ray_t**)ray_data(y);
        int rank1_seek = ny > 0 && e[0] && !ray_is_atom(e[0]);
        if (rank1_seek) {
            if (ray_is_atom(x)) return ray_bool(false);
            /* whole-x seek when x IS one item shape: a simple vector, or a
             * boxed list while y's items are boxed too ((1 2;3 4) in (...;9)).
             * Per-item only when x is boxed OVER y's simple-vector items —
             * e.g. list-of-strings in list-of-strings. */
            if (x->type != RAY_LIST || e[0]->type == RAY_LIST || e[0]->type == RAY_TABLE)
                return ray_bool(seq_has_item(y, x) != 0);
        } else if (ray_is_atom(x)) return ray_bool(seq_has_item(y, x) != 0);
        int64_t nx = ray_len(x);                     /* left-atomic over x */
        ray_t* outl = ray_list_new(nx > 0 ? nx : 1);
        if (RAY_IS_ERR(outl)) return outl;
        for (int64_t i = 0; i < nx; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* xe = ray_at_fn(x, ia);            /* owned */
            ray_release(ia);
            if (!xe || RAY_IS_ERR(xe)) { ray_release(outl); return xe; }
            ray_t* r = q_in_wrap(xe, y);
            ray_release(xe);
            if (!r || RAY_IS_ERR(r)) { ray_release(outl); return r; }
            outl = ray_list_append(outl, r);         /* retains */
            ray_release(r);
            if (RAY_IS_ERR(outl)) return outl;
        }
        ray_t* c = q_list_collapse(outl);
        ray_release(outl);
        return c;
    }
    /* STR-vector y (openq list-of-strings): whole-item membership -> atom */
    if (y->type == RAY_STR && x->type == -RAY_STR)
        return ray_bool(seq_has_item(y, x) != 0);
    /* mixed numeric families (ref/in.md Mixed argument types): allowed only
     * against an ATOM or 1-item y — elementwise numeric equality (q_velem_f
     * reads both families; nulls never match). */
    if (is_num_t(x->type) && is_num_t(y->type)) {
        int xf = is_float_t(x->type), yf = is_float_t(y->type);
        if (xf != yf) {
            if (!ray_is_atom(y) && ray_len(y) != 1)
                return q_err(QE_TYPE);
            int yn; double yv = q_velem_f(y, 0, &yn);
            if (ray_is_atom(x)) {
                int nu; double v = q_velem_f(x, 0, &nu);
                return ray_bool(!nu && !yn && v == yv);
            }
            int64_t n = ray_len(x);
            ray_t* out = ray_vec_new(RAY_BOOL, n > 0 ? n : 1);
            if (RAY_IS_ERR(out)) return out;
            out->len = n;
            bool* o = (bool*)ray_data(out);
            for (int64_t i = 0; i < n; i++) {
                int nu; double v = q_velem_f(x, i, &nu);
                o[i] = !nu && !yn && v == yv;
            }
            return out;
        }
    }
    ray_t* r = ray_in_fn(x, y);
    /* 1-char string x: base char membership returns a 1-vec; kdb wants an
     * ATOM (`"x" in "a"` -> 0b). */
    if (r && !RAY_IS_ERR(r) && x->type == -RAY_STR && ray_str_len(x) == 1 &&
        r->type == RAY_BOOL && ray_len(r) == 1) {
        int b = ((const bool*)ray_data(r))[0] != 0;
        ray_release(r);
        return ray_bool(b != 0);
    }
    return r;
}

/* Call the env-bound BINARY builtin `nm` (the wrapper-over-env pattern:
 * some base fns — dict — are declared only in internal base headers, so the
 * wrapper routes through the audited env value instead of a frozen-header
 * include).  Borrowed args; returns owned. */
ray_t* q_env_call2(const char* nm, ray_t* a, ray_t* b) {
    ray_t* f = ray_env_get(ray_sym_intern(nm, strlen(nm)));
    if (!f || f->type != RAY_BINARY)
        return q_err(QE_TYPE);
    return ((ray_binary_fn)(uintptr_t)f->i64)(a, b);
}

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

/* ===== q `?` find arms =====================================================
 * list ? y -> find.  kdb miss semantics: the smallest index NOT in the list,
 * i.e. `count x` — rayfall find returns 0N on a miss (atom result) or
 * per-element 0N (vector needle), so both shapes are remapped to count here.
 * The dict reverse lookup keys[vals?y] composes on find, so it lives here
 * WITH find.  The roll/deal/generate arms of `?` live in ops/q_rand.c
 * (q_roll_wrap, the `?` shape dispatcher — it routes find shapes here). */
/* First index of x (a boxed list) whose ITEM whole-matches v, else cnt
 * (the kdb miss).  Borrows both. */
static int64_t list_find_item(ray_t* x, ray_t* v, int64_t cnt) {
    ray_t** ex = (ray_t**)ray_data(x);
    for (int64_t i = 0; i < cnt; i++)
        if (ex[i] && v && (ex[i] == v || atom_eq(ex[i], v))) return i;
    return cnt;
}

ray_t* q_list_find(ray_t* x, ray_t* y) {
    /* d?y — reverse dictionary lookup (basics/dictsandtables.md): the key of
     * the FIRST value matching y, i.e. keys[vals?y].  A find miss lands at
     * count vals, and ray_at_fn null-fills that out-of-range key index — the
     * typed null of the key domain, kdb's miss result.  Keyed tables keep
     * their own (deferred) path. */
    if (x && x->type == RAY_DICT && !q_type_is_keyed(x)) {
        ray_t* keys = ray_dict_keys(x);              /* borrowed */
        if (!keys) return q_err(QE_TYPE);
        int vo = 0;
        ray_t* vv = q_table_dict_vals(x, &vo);
        if (!vv) return q_err(QE_TYPE);
        ray_t* i = q_list_find(vv, y);               /* find arm: miss -> count */
        if (vo) ray_release(vv);
        if (!i || RAY_IS_ERR(i)) return i;
        ray_t* r = ray_at_fn(keys, i);
        ray_release(i);
        if (r && r->type == RAY_LIST) { ray_t* c = q_typed_empty_like(q_list_collapse(r), keys); ray_release(r); return c; }
        return r;
    }
    if (x && (ray_is_vec(x) || x->type == RAY_LIST)) {          /* find */
        int64_t cnt = ray_len(x);
        /* ---- rank-aware arms (ref/find.md): a rank-n haystack looks for
         * rank n-1 objects.  A -RAY_STR atom counts as rank>=1 (kdb strings
         * are char LISTS). ---- */
        int x_ranked = 0;                    /* x is a "list of lists" */
        if (x->type == RAY_LIST && cnt > 0) {
            ray_t* x0 = ((ray_t**)ray_data(x))[0];
            x_ranked = x0 && (!ray_is_atom(x0) || x0->type == -RAY_STR);
        }
        if (x_ranked && y && y->type == RAY_LIST) {
            /* list-of-lists x, MIXED y: items of x matched with ITEMS of y
             * (`u?(2 3;\`ab)` -> 3 3 — never with the whole of y). */
            int64_t ny = ray_len(y);
            ray_t** e = (ray_t**)ray_data(y);
            ray_t* out = ray_vec_new(RAY_I64, ny > 0 ? ny : 1);
            if (RAY_IS_ERR(out)) return out;
            out->len = ny;
            int64_t* o = (int64_t*)ray_data(out);
            for (int64_t j = 0; j < ny; j++)
                o[j] = e[j] ? list_find_item(x, e[j], cnt) : cnt;
            return out;
        }
        if (x_ranked && y && !ray_is_atom(y) && ray_is_vec(y) && y->type != RAY_LIST) {
            /* list-of-lists x, SIMPLE vector y: whole-y match (`u?10 2 -6`
             * -> 1). */
            return ray_i64(list_find_item(x, y, cnt));
        }
        if (x->type != RAY_LIST && ray_is_vec(x) && y && y->type == RAY_LIST) {
            /* simple-vector x, list y whose first item is a list: RIGHT-
             * ATOMIC item-by-item; an ATOM item in this mode is a rank
             * mismatch and MISSES (w?rt: (10 5 -1;-8;3 17) -> (0 3 4;7;2 7),
             * the doc's own transcript). */
            int64_t ny = ray_len(y);
            ray_t** e = (ray_t**)ray_data(y);
            int y0_ranked = ny > 0 && e[0] &&
                            (!ray_is_atom(e[0]) || e[0]->type == -RAY_STR);
            if (y0_ranked) {
                ray_t* out = ray_list_new(ny > 0 ? ny : 1);
                if (RAY_IS_ERR(out)) return out;
                for (int64_t j = 0; j < ny; j++) {
                    ray_t* rr;
                    if (!e[j] || (ray_is_atom(e[j]) && e[j]->type != -RAY_STR))
                        rr = ray_i64(cnt);           /* rank-0 item: miss */
                    else
                        rr = q_list_find(x, e[j]);
                    if (!rr || RAY_IS_ERR(rr)) { ray_release(out); return rr; }
                    out = ray_list_append(out, rr);  /* retains */
                    ray_release(rr);
                    if (RAY_IS_ERR(out)) return out;
                }
                return out;                          /* mixed shapes stay boxed */
            }
        }
        ray_t* i = ray_find_fn(x, y);
        if (!i || RAY_IS_ERR(i)) return i;
        if (ray_is_atom(i) && i->type == -RAY_I64 && RAY_ATOM_IS_NULL(i)) {
            ray_release(i);
            return ray_i64(cnt);                    /* kdb: miss -> count x */
        }
        if (i->type == RAY_I64) {                   /* vector needle: per-elem */
            int64_t n = ray_len(i);
            int64_t* d = (int64_t*)ray_data(i);     /* fresh rc=1 from find */
            for (int64_t j = 0; j < n; j++)
                if (d[j] == NULL_I64) d[j] = cnt;
            i->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
        }
        return i;
    }
    return q_err(QE_TYPE);   /* unreachable: q_roll_wrap routes only find shapes here */
}
