/* q_wrap_list.c — list verbs: reshape/take/drop/cut, column attributes,
 * rotate/sublist/next/prev/fills/fill/in, sort/grade/bucket, deal/generate
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_apply.h" /* q_apply_noun */
#include "lang/env.h"      /* ray_env_get — q_env_call2 */
#include "lang/eval.h"     /* ray_take_fn, ray_in_fn, ray_find_fn, ray_xbar_fn */
#include "lang/internal.h" /* ray_iasc_fn/ray_idesc_fn, RAY_IS_TEMPORAL64, ray_error */
#include "lang/format.h"   /* ray_type_name — error messages */
#include "ops/ops.h"       /* ray_is_lazy, ray_lazy_materialize */
#include "ops/idxop.h"     /* .attr.* engine calls: ray_attr_*, RAY_IDX_*, RAY_MARK_* (column attributes) */
#include "mem/heap.h"      /* RAY_ATTR_HAS_NULLS — ? find miss remap */
#include "table/sym.h"     /* ray_sym_intern_runtime, RAY_SYM_W64 */
#include <stdint.h>        /* INT32/64 MIN/MAX, uintptr_t */
#include <string.h>
#include <stdlib.h>        /* malloc/free, rand */

/* ---- wrappers (bespoke q semantics over a rayfall primitive) ---- */

/* Gather `count` elements from x starting at logical index `start`.  recycle:
 * indices wrap modulo `total` (reshape recycling); else sequential in-range
 * (chunk / cut).  A string gathers chars into a new string; a vector/list
 * gathers via ray_at_fn over an i64 index vector.  Borrows x. */
static ray_t* q_gather(ray_t* x, int64_t start, int64_t count, int64_t total,
                       int recycle) {
    if (count < 0) count = 0;
    if (x && x->type == -RAY_STR) {
        const char* sp = ray_str_ptr(x);
        char stackb[1024];
        char* b = (count < (int64_t)sizeof stackb) ? stackb
                                                    : malloc((size_t)count + 1);
        if (!b) return ray_error("wsfull", "#: out of memory");
        for (int64_t i = 0; i < count; i++)
            b[i] = sp[recycle && total ? ((start + i) % total) : (start + i)];
        ray_t* row = ray_str(b, (size_t)count);
        if (b != stackb) free(b);
        return row;
    }
    int64_t stacki[1024];
    int64_t* ix = (count <= 1024) ? stacki
                                  : malloc((size_t)(count ? count : 1) * sizeof(int64_t));
    if (!ix) return ray_error("wsfull", "#: out of memory");
    for (int64_t i = 0; i < count; i++)
        ix[i] = recycle && total ? ((start + i) % total) : (start + i);
    ray_t* idx = ray_vec_from_raw(RAY_I64, ix, count);
    if (ix != stacki) free(ix);
    if (RAY_IS_ERR(idx)) return idx;
    ray_t* row = ray_at_fn(x, idx);       /* boxed list of atoms */
    ray_release(idx);
    if (!row || RAY_IS_ERR(row)) return row;
    ray_t* c = q_collapse_list(row);      /* -> typed vector when homogeneous */
    ray_release(row);
    return c;
}

/* q `shape # x` — reshape (kdb ref/take): an int-VECTOR left arg reshapes x
 * row-major into a matrix (2-D case).  A 0N dimension is INFERRED (chunk into
 * that stride, ragged last row); otherwise x is RECYCLED to fill.  Ranks other
 * than 2 fall back to rayfall range-take (unchanged behaviour).  Borrows both. */
static ray_t* q_reshape(ray_t* shape, ray_t* x) {
    int64_t nd = ray_len(shape);
    if (nd != 2) return ray_take_fn(x, shape);
    const int64_t* dv = (const int64_t*)ray_data(shape);
    int64_t d0 = dv[0], d1 = dv[1];
    int is_str = (x && x->type == -RAY_STR);
    int64_t total = is_str ? (int64_t)ray_str_len(x) : (x ? ray_len(x) : 0);
    if (total <= 0) return ray_error("length", "#: reshape of empty");
    int64_t rows, cols, chunk = 0;
    if (d0 == NULL_I64 && d1 != NULL_I64) {   /* 0N dimension -> inferred stride */
        if (d1 <= 0) return ray_error("length", "#: inferred reshape needs a positive stride");
        cols = d1; rows = (total + cols - 1) / cols; chunk = 1;
    } else if (d1 == NULL_I64 && d0 != NULL_I64) {
        if (d0 <= 0) return ray_error("length", "#: inferred reshape needs a positive stride");
        rows = d0; cols = (total + rows - 1) / rows; chunk = 1;
    } else if (d0 == NULL_I64 || d1 == NULL_I64) {
        return ray_error("length", "#: 0N 0N reshape");
    } else {
        rows = d0; cols = d1;
    }
    if (rows < 0 || cols < 0) return ray_error("length", "#: negative shape");
    ray_t* out = ray_list_new(rows > 0 ? rows : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t r = 0; r < rows; r++) {
        int64_t start = r * cols, rc = cols;
        if (chunk && start + rc > total) rc = total - start;   /* ragged last */
        ray_t* row = q_gather(x, start, rc, total, !chunk);
        if (!row || RAY_IS_ERR(row)) { ray_release(out); return row ? row : ray_error("domain", "#: reshape"); }
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
static ray_t* q_attr_remap_err(ray_t* err, char letter) {
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
 * error codes (caller remaps via q_attr_remap_err). */
static ray_t* q_attr_compose(ray_t* base, char letter) {
    int cls = ray_attr_numeric_class(base->type);
    if (cls < 0) return ray_error("type", NULL);
    bool ok = (letter == 'u') ? (ray_attr_verify_distinct(base) && q_no_dup_nulls(base))
                              : ray_attr_verify_contiguous(base);
    if (!ok) return ray_error("domain", NULL);
    uint8_t mark = (letter == 'u') ? RAY_MARK_UNIQUE : RAY_MARK_PARTED;
    if (cls == 1) {                              /* integer-family: find-hash + marker */
        ray_t* hv = ray_idx_hash_fn(base);       /* borrows base, owned out */
        if (!hv || RAY_IS_ERR(hv)) return hv ? hv : ray_error("oom", NULL);
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
static ray_t* q_attr_set_dispatch(ray_t* n, ray_t* vec) {
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
        if (!base || RAY_IS_ERR(base)) return base ? base : ray_error("oom", NULL);
        ray_t* r = q_attr_compose(base, letter);     /* borrows base */
        ray_release(base);
        if (r && RAY_IS_ERR(r)) return q_attr_remap_err(r, letter);
        return r;
    }
    case 's': attr_name = "sorted";  break;
    case 'g': attr_name = "grouped"; break;
    default:  return ray_error("type", NULL);        /* `z#vec etc. */
    }
    int64_t aid = ray_sym_intern_runtime(attr_name, strlen(attr_name));
    ray_t* nm = ray_sym(aid);                         /* owned -RAY_SYM */
    /* kdb: a vector carries at most ONE attribute, and `` `x# `` REPLACES any
     * prior one (`` attr `g#`s#1 2 3 `` -> `` `g ``).  The rayfall-native setter
     * preserves existing markers/index (sorted survives attach), so drop first,
     * then set on the cleared base.  ray_attr_drop_fn borrows vec and returns an
     * owned (possibly COW'd) result; ray_attr_set_fn borrows that base. */
    ray_t* base = ray_attr_drop_fn(vec);              /* owned */
    if (!base || RAY_IS_ERR(base)) { ray_release(nm); return base ? base : ray_error("oom", NULL); }
    ray_t* r = ray_attr_set_fn(nm, base);             /* borrows nm, base */
    ray_release(nm);
    ray_release(base);
    if (r && RAY_IS_ERR(r)) return q_attr_remap_err(r, letter);
    return r;
}

/* Public (test-facing) entry over q_attr_set_dispatch: build the single-char
 * symbol the `#` set-attribute arm expects and dispatch.  letter 0 clears.
 * Borrows vec; returns owned.  Lets the acceleration C-unit exercise the real q
 * u#/p# compose path (find-hash + marker) rather than the reverted engine call. */
ray_t* q_attr_set_letter(char letter, ray_t* vec) {
    char s1[1]; s1[0] = letter;
    int64_t id = ray_sym_intern_runtime(letter ? s1 : "", letter ? 1 : 0);
    ray_t* n = ray_sym(id);                           /* owned -RAY_SYM */
    ray_t* r = q_attr_set_dispatch(n, vec);           /* borrows n, vec */
    ray_release(n);
    return r;
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
    if (n && n->type == RAY_I64 && ray_len(n) >= 2) return q_reshape(n, list);
    return ray_take_fn(list, n);
}

/* q `n _ list` — count-drop (NOT rayfall's dict key-remove).  n>=0 drops the
 * first n elements; n<0 drops the last |n|.  Implemented as a range-take
 * ray_take_fn(list, (start; amount)), which clamps at the ends.  Borrows args.
 *
 * Length is derived string-aware: a q string is a -RAY_STR atom whose char
 * count lives in ray_str_len, NOT the ->len union field (which aliases the SSO
 * {slen,sdata} bytes), so ray_len would be garbage for strings. */
/* q_collapse_list leaves a ZERO-length boxed list untyped (no element to infer
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

/* Gather table rows [start, start+count) (recycle=1 wraps cyclically) into an
 * OWNED table via qj_table_gather_idx — the same single-home gather `t[0 2]`
 * reaches through q_apply_noun (char columns byte-permuted, misses
 * null-filled).  The result is never collapsed: tables stay tables. */
static ray_t* q_row_gather(ray_t* t, int64_t start, int64_t count, int recycle) {
    int64_t n = ray_table_nrows(t);
    int64_t* p = (int64_t*)malloc((size_t)(count > 0 ? count : 1) * sizeof(int64_t));
    if (!p) return ray_error("wsfull", "take: out of memory");
    for (int64_t i = 0; i < count; i++) {
        int64_t j = start + i;
        if (recycle && n > 0) j = ((j % n) + n) % n;
        p[i] = j;
    }
    ray_t* r = qj_table_gather_idx(t, p, count);     /* owned */
    free(p);
    return r;
}

/* d without the entries for keys ks (atom or vector) — kdb Drop is tolerant:
 * keys not present are ignored (ref/drop.md `` `a _ `a`b`c!1 2 3 ``).
 * Borrows both; returns an owned dict.  Same append/release discipline as
 * q_dict_union (q_apply.c). */
static ray_t* q_dict_drop_keys(ray_t* d, ray_t* ks) {
    ray_t* dk = ray_dict_keys(d);                    /* borrowed */
    ray_t* dv = ray_dict_vals(d);                    /* borrowed */
    if (!dk || !dv) return ray_error("type", "_ (drop): malformed dictionary");
    int64_t nd = ray_dict_len(d);
    int64_t nk = (ks && (ray_is_vec(ks) || ks->type == RAY_LIST)) ? ray_len(ks) : -1;
    ray_t* ok = ray_list_new(nd > 0 ? nd : 1);
    ray_t* ov = ray_list_new(nd > 0 ? nd : 1);
    if (!ok || !ov) {
        if (ok) ray_release(ok);
        if (ov) ray_release(ov);
        return ray_error("oom", NULL);
    }
    for (int64_t i = 0; i < nd; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* ke = ray_at_fn(dk, ia);               /* owned key */
        ray_release(ia);
        if (!ke || RAY_IS_ERR(ke)) { ray_release(ok); ray_release(ov); return ke ? ke : ray_error("type", NULL); }
        int hit = 0;
        if (nk < 0) {
            hit = q_values_match(ke, ks);
        } else {
            for (int64_t j = 0; j < nk && !hit; j++) {
                ray_t* ja = ray_i64(j);
                ray_t* kj = ray_at_fn(ks, ja);       /* owned */
                ray_release(ja);
                if (kj && !RAY_IS_ERR(kj)) {
                    hit = q_values_match(ke, kj);
                    ray_release(kj);
                } else if (kj) {
                    ray_release(ke); ray_release(ok); ray_release(ov);
                    return kj;
                }
            }
        }
        if (hit) { ray_release(ke); continue; }
        ray_t* ja2 = ray_i64(i);
        ray_t* ve = ray_at_fn(dv, ja2);              /* owned value */
        ray_release(ja2);
        if (!ve || RAY_IS_ERR(ve)) { ray_release(ke); ray_release(ok); ray_release(ov); return ve ? ve : ray_error("type", NULL); }
        ok = ray_list_append(ok, ke);
        ov = ray_list_append(ov, ve);
        ray_release(ke);
        ray_release(ve);
    }
    ray_t* ck = q_typed_empty_like(q_collapse_list(ok), dk);
    ray_t* cv = q_typed_empty_like(q_collapse_list(ov), dv);
    ray_release(ok);
    ray_release(ov);
    if (!ck || RAY_IS_ERR(ck)) { if (cv && !RAY_IS_ERR(cv)) ray_release(cv); return ck ? ck : ray_error("type", NULL); }
    if (!cv || RAY_IS_ERR(cv)) { ray_release(ck); return cv ? cv : ray_error("type", NULL); }
    return ray_dict_new(ck, cv);                     /* consumes both */
}

ray_t* q_drop_wrap(ray_t* n, ray_t* list) {
    /* ---- dict arms (ref/drop.md) — claimed BEFORE the int-vector cut arm so
     * `1 2 _ intkeyed_dict` is a key-drop, and before the int-atom count tail
     * so `1_d` drops ENTRIES (keys and values together), never values-only. */
    int64_t dk;
    if (list && list->type == RAY_DICT && !q_is_keyed_table(list)) {
        /* n _ d — int ATOM drops the first/last n entries */
        if (q_strict_i64(n, &dk) && !RAY_ATOM_IS_NULL(n)) {
            ray_t* k = ray_dict_keys(list);          /* borrowed */
            ray_t* v = ray_dict_vals(list);          /* borrowed */
            if (!k || !v) return ray_error("type", "_: dict");
            ray_t* rk = q_drop_wrap(n, k);
            if (rk && ray_is_lazy(rk)) rk = ray_lazy_materialize(rk);
            if (!rk || RAY_IS_ERR(rk)) return rk;
            ray_t* rv = q_drop_wrap(n, v);
            if (rv && ray_is_lazy(rv)) rv = ray_lazy_materialize(rv);
            if (!rv || RAY_IS_ERR(rv)) { ray_release(rk); return rv; }
            return ray_dict_new(rk, rv);             /* consumes both */
        }
        /* keys _ d — sym atom / sym vector / int vector lhs drops entries by
         * key (other lhs kinds keep today's error tail) */
        if (n && (n->type == -RAY_SYM || n->type == RAY_SYM || q_is_int_vec(n)))
            return q_dict_drop_keys(list, n);
    }
    /* d _ key — dict lhs drops the entry at an ATOM key; a vector rhs is
     * 'type (ref/drop.md pins `(`a`b`c!1 2 3) _ `a`b` -> 'type). */
    if (n && n->type == RAY_DICT && !q_is_keyed_table(n)) {
        if (list && ray_is_atom(list))
            return q_dict_drop_keys(n, list);
        return ray_error("type", "_: key");
    }
    /* syms _ t — table column-drop (ref/drop.md `` `a`b _ t ``): sym-VECTOR
     * lhs only (a sym ATOM lhs on a table stays 'type — pinned rows).  One
     * home: flip -> dict key-drop -> flip back (q_flip_wrap owns its results;
     * q_dict_drop_keys borrows both args). */
    if (n && n->type == RAY_SYM && list && list->type == RAY_TABLE) {
        ray_t* d = q_flip_wrap(list);                /* owned dict */
        if (!d || RAY_IS_ERR(d)) return d;
        ray_t* rd = q_dict_drop_keys(d, n);          /* owned dict */
        ray_release(d);
        if (!rd || RAY_IS_ERR(rd)) return rd;
        ray_t* rt = q_flip_wrap(rd);                 /* owned table */
        ray_release(rd);
        return rt;
    }
    /* x _ i — delete the item at index i (ref/drop.md `0 1 ... 8 _ 5`):
     * list/vector lhs, int-ATOM rhs.  Two clamped range-takes joined; an
     * out-of-range index returns x unchanged (Drop is tolerant). */
    int64_t i;
    if (n && (ray_is_vec(n) || n->type == RAY_LIST) && n->type != RAY_DICT &&
        q_strict_i64(list, &i)) {
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
    if (q_is_int_vec(n)) {
        if (!list || (!ray_is_vec(list) && list->type != RAY_LIST &&
                      list->type != RAY_TABLE))
            return ray_error("type", "_: x");
        int64_t len = (list->type == RAY_TABLE) ? ray_table_nrows(list)
                                                : ray_len(list);
        int64_t np = ray_len(n);
        ray_t* out = ray_list_new(np > 0 ? np : 1);
        int64_t prev = 0;
        for (int64_t i = 0; i < np; i++) {
            int64_t p = q_ivec_get(n, i);
            int64_t nxt = (i + 1 < np) ? q_ivec_get(n, i + 1) : len;
            if (p < 0 || p > len || nxt < p || nxt > len || (i > 0 && p < prev)) {
                ray_release(out);
                return ray_error("domain", "_: positions");
            }
            prev = p;
            ray_t* slice;
            if (list->type == RAY_TABLE) {           /* table cut: row slices */
                slice = q_row_gather(list, p, nxt - p, 0);
            } else {
                int64_t rng[2] = { p, nxt - p };
                ray_t* range = ray_vec_from_raw(RAY_I64, rng, 2);
                if (RAY_IS_ERR(range)) { ray_release(out); return range; }
                slice = ray_take_fn(list, range);
                ray_release(range);
            }
            if (!slice || RAY_IS_ERR(slice)) { ray_release(out); return slice; }
            out = ray_list_append(out, slice);
            ray_release(slice);
        }
        return out;
    }
    int64_t k;
    ray_t* err = q_i64_or_err(n, &k, "_: n");
    if (err) return err;
    if (!list) return ray_error("type", "_: x");
    int64_t len;
    if (list->type == -RAY_STR)
        len = (int64_t)ray_str_len(list);           /* SSO-safe string length */
    else if (list->type == RAY_TABLE)
        len = ray_table_nrows(list);                 /* count-drop is a row drop */
    else if (ray_is_vec(list) || list->type == RAY_LIST)
        len = ray_len(list);                         /* typed vector / boxed list */
    else
        return ray_error("type", "_: x");
    int64_t start, amount;
    if (k >= 0) { start = (k < len) ? k : len; amount = len - start; }
    else        { start = 0; amount = len + k; if (amount < 0) amount = 0; }
    if (list->type == RAY_TABLE)                     /* rows stay a table */
        return q_row_gather(list, start, amount, 0);
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
    if (q_strict_i64(n, &sz)) {
        if (sz <= 0) return ray_error("domain", "cut: n");
        int is_str = (x && x->type == -RAY_STR);
        int64_t total = is_str ? (int64_t)ray_str_len(x) : (x ? ray_len(x) : 0);
        if (!x || (!is_str && !ray_is_vec(x) && x->type != RAY_LIST))
            return ray_error("type", "cut: x");
        int64_t rows = (total + sz - 1) / sz;
        ray_t* out = ray_list_new(rows > 0 ? rows : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t r = 0; r < rows; r++) {
            int64_t start = r * sz, rc = sz;
            if (start + rc > total) rc = total - start;
            ray_t* row = q_gather(x, start, rc, total, 0);   /* chunk, no recycle */
            if (!row || RAY_IS_ERR(row)) { ray_release(out); return row ? row : ray_error("domain", "cut"); }
            out = ray_list_append(out, row);
            ray_release(row);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return q_drop_wrap(n, x);   /* int-vector positional cut == `_` */
}
/* ===== q list verbs: xprev / fill (`^`) ====================================
 * ref next.md, fill.md; deferred forms error, never wrong-answer
 * (*-deferred.qcmd companions).  `rotate` and `sublist` are self-hosted in
 * q.q over `#`/`_` — the table arms ride q_drop_wrap's row-drop tail. */

/* q `n xprev x` — n-item shift, null-filling the vacated end (ref/next.md:
 * +n is prev-by-n, -n is next); `next`/`prev` are its q.q unit shifts, so
 * every arm here is theirs too.  Strings shift CHARS with ' ' fill
 * (`1 xprev "abcde"` -> " abcd"); a generic LIST fills each vacated slot
 * with `0#first` of the ORIGINAL (`prev (1 2;"abc";`ibm)` -> (`long$();1 2;"abc")). */
ray_t* q_xprev_wrap(ray_t* nx, ray_t* x) {
    int64_t k;   /* strict cast owns the type axis; 0N rejected here */
    if (!q_strict_i64(nx, &k) || RAY_ATOM_IS_NULL(nx))
        return ray_error("type", "xprev: n");
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
    if (x && x->type == -RAY_STR) {
        int64_t len = (int64_t)ray_str_len(x);
        const char* s = ray_str_ptr(x);
        char stackb[256];
        char* b = (len <= (int64_t)sizeof stackb) ? stackb : malloc((size_t)(len > 0 ? len : 1));
        if (!b) return ray_error("oom", NULL);
        for (int64_t i = 0; i < len; i++) {
            int64_t j = i - k;
            b[i] = (j >= 0 && j < len) ? s[j] : ' ';
        }
        ray_t* r = ray_str(b, (size_t)len);
        if (b != stackb) free(b);
        return r;
    }
    if (!x || !ray_is_vec(x))
        return ray_error("nyi", "xprev: only simple vectors, strings and lists (dict/table deferred)");
    int8_t t = x->type;
    if (!(t == RAY_I16 || t == RAY_I32 || t == RAY_I64 || t == RAY_F32 || t == RAY_F64 ||
          RAY_IS_TEMPORAL32(t) || RAY_IS_TEMPORAL64(t) || RAY_IS_TEMPORALF(t)))
        return ray_error("nyi", "xprev: %s vectors are deferred", ray_type_name(t));
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
    if (x && ray_is_atom(x) && x->type != RAY_DICT) { ray_retain(x); return x; }
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
        ray_t* c = q_collapse_list(outl);
        ray_release(outl);
        return c;
    }
    if (!x || !q_vec_is_num(x))
        return ray_error("nyi", "fills: numeric or symbol vector (dict/table deferred)");
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
static int q_seq_has_item(ray_t* y, ray_t* v) {
    int64_t n = ray_len(y);
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* ye = ray_at_fn(y, ia);                /* owned item */
        ray_release(ia);
        if (!ye || RAY_IS_ERR(ye)) { if (ye) ray_release(ye); continue; }
        int hit = q_values_match(ye, v);
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
static int q_is_float_t(int8_t t) {
    return t == RAY_F64 || t == RAY_F32 || t == -RAY_F64 || t == -RAY_F32;
}
static int q_is_num_t(int8_t t) {
    return t == RAY_BOOL || t == RAY_U8 || t == RAY_I16 || t == RAY_I32 || t == RAY_I64 ||
           t == RAY_F32 || t == RAY_F64 || t == -RAY_BOOL || t == -RAY_U8 || t == -RAY_I16 ||
           t == -RAY_I32 || t == -RAY_I64 || t == -RAY_F32 || t == -RAY_F64;
}
static int q_is_sym_t(int8_t t) { return t == RAY_SYM || t == -RAY_SYM; }

ray_t* qj_ktbl_merge(ray_t* x, ray_t* y, int mode);   /* joins wave */

ray_t* q_fill_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "^: nil operand");
    /* keyed^keyed is the uj merge with fill semantics (ref/coalesce.md:
     * y records update x's, but y NULLS don't overwrite). */
    if (q_is_keyed_table(x) && q_is_keyed_table(y))
        return qj_ktbl_merge(x, y, 1);
    int xatom = ray_is_atom(x), yatom = ray_is_atom(y);

    /* ---- symbol fill ---- */
    if (q_is_sym_t(x->type) || q_is_sym_t(y->type)) {
        if (!q_is_sym_t(x->type) || !q_is_sym_t(y->type))
            return ray_error("type", "^: symbol fill needs symbol operands");
        /* length follows y when it is a vector; a scalar y broadcasts to the
         * length of a vector x (`` `a`b`c^` `` -> 3 items), matching the
         * numeric branch below. */
        int64_t len = yatom ? (xatom ? 1 : ray_len(x)) : ray_len(y);
        if (!xatom && !yatom && ray_len(x) != len)
            return ray_error("length", "^: operand lengths must match");
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
        ray_t* c = q_collapse_list(outl); ray_release(outl);
        if (yatom && xatom) {                    /* scalar^scalar -> atom */
            ray_t* ia = ray_i64(0); ray_t* a = ray_at_fn(c, ia); ray_release(ia); ray_release(c);
            return a;
        }
        return c;
    }

    /* ---- numeric fill ---- */
    if (!q_is_num_t(x->type) || !q_is_num_t(y->type))
        return ray_error("type", "^: unsupported operand types (dict/table/list fill deferred)");
    int is_float = q_is_float_t(x->type) || q_is_float_t(y->type);
    int64_t len = yatom ? (xatom ? 1 : ray_len(x)) : ray_len(y);
    if (!xatom && !yatom && ray_len(x) != ray_len(y))
        return ray_error("length", "^: operand lengths must match");
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
    if (!x || !y) return ray_error("type", "in: nil operand");
    if (y->type == RAY_LIST) {
        int64_t ny = ray_len(y);
        ray_t** e = (ray_t**)ray_data(y);
        int rank1_seek = ny > 0 && e[0] && !ray_is_atom(e[0]);
        if (rank1_seek) {
            if (ray_is_atom(x)) return ray_bool(false);
            return ray_bool(q_seq_has_item(y, x) != 0);
        }
        if (ray_is_atom(x)) return ray_bool(q_seq_has_item(y, x) != 0);
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
        ray_t* c = q_collapse_list(outl);
        ray_release(outl);
        return c;
    }
    /* STR-vector y (openq list-of-strings): whole-item membership -> atom */
    if (y->type == RAY_STR && x->type == -RAY_STR)
        return ray_bool(q_seq_has_item(y, x) != 0);
    /* mixed numeric families (ref/in.md Mixed argument types): allowed only
     * against an ATOM or 1-item y — elementwise numeric equality (q_velem_f
     * reads both families; nulls never match). */
    if (q_is_num_t(x->type) && q_is_num_t(y->type)) {
        int xf = q_is_float_t(x->type), yf = q_is_float_t(y->type);
        if (xf != yf) {
            if (!ray_is_atom(y) && ray_len(y) != 1)
                return ray_error("type", "in: mixed numeric types need an atom or 1-item right arg");
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
        return ray_error("type", "%s: env builtin missing", nm);
    return ((ray_binary_fn)(uintptr_t)f->i64)(a, b);
}

/* Unary sibling of q_env_call2 (guid roll/deal routes through the audited
 * env `guid` value).  Borrowed arg; returns owned. */
static ray_t* q_env_call1(const char* nm, ray_t* a) {
    ray_t* f = ray_env_get(ray_sym_intern(nm, strlen(nm)));
    if (!f || f->type != RAY_UNARY)
        return ray_error("type", "%s: env builtin missing", nm);
    return ((ray_unary_fn)(uintptr_t)f->i64)(a);
}

/* A keyed table is a RAY_DICT whose keys AND values are both tables.
 * Exported (q_registry.h) — q_builtins' by-name deref shares it. */
int q_is_keyed_table(ray_t* y) {
    if (!y || y->type != RAY_DICT) return 0;
    ray_t* k = ray_dict_keys(y);
    ray_t* v = ray_dict_vals(y);
    return k && v && k->type == RAY_TABLE && v->type == RAY_TABLE;
}

/* Flatten a plain-or-keyed table to a single plain table (key cols then value
 * cols).  Returns owned. */
ray_t* q_table_flatten(ray_t* y) {
    if (y->type == RAY_TABLE) { ray_retain(y); return y; }
    ray_t* kt = ray_dict_keys(y);          /* borrowed */
    ray_t* vt = ray_dict_vals(y);          /* borrowed */
    int64_t knc = ray_table_ncols(kt), vnc = ray_table_ncols(vt);
    ray_t* out = ray_table_new(knc + vnc > 0 ? knc + vnc : 1);
    for (int64_t c = 0; c < knc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(kt, c), ray_table_get_col_idx(kt, c));
    for (int64_t c = 0; c < vnc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(vt, c), ray_table_get_col_idx(vt, c));
    return out;
}

/* q enkey/unkey `N!table`: 0 -> plain table (unkey), N>0 -> key the first N
 * columns into a keyed table (RAY_DICT keycols-table -> valcols-table).
 * Accepts a plain OR already-keyed table (re-keys).  Consumes nothing. */
ray_t* q_enkey(ray_t* y, int64_t nkey) {
    ray_t* flat = q_table_flatten(y);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    int64_t nc = ray_table_ncols(flat);
    if (nkey <= 0) return flat;                 /* unkey */
    if (nkey >= nc) { ray_release(flat); return ray_error("length", "!: key count exceeds columns"); }
    ray_t* kt = ray_table_new(nkey);
    ray_t* vt = ray_table_new(nc - nkey);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(kt) && !RAY_IS_ERR(vt); c++) {
        int64_t nm = ray_table_col_name(flat, c);
        ray_t* col = ray_table_get_col_idx(flat, c);
        if (c < nkey) kt = ray_table_add_col(kt, nm, col);
        else          vt = ray_table_add_col(vt, nm, col);
    }
    ray_release(flat);
    if (RAY_IS_ERR(kt)) { ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);
}

/* ===== q grade / bucket family ============================================
 * GRADE IS THE PRIMITIVE: iasc/idesc own ordering for every structure (vector →
 * ray_iasc_fn; dict → keys by the value grade; table/keyed → q_grade_table), and
 * asc/desc/rank/xrank/xasc/xdesc are q.q derivations over them (index once, ONE
 * gather).  xbar is an arg-swap over ray_xbar_fn.  DEFERRED (error, never a
 * wrong answer): the `s#` attribute on asc results — the attr-take arm accepts
 * long vectors only, so setting it would regress symbol/nested sorts (PLAN.md)
 * — and the mixed-general-list-by-type-number sort (ray_iasc_fn 'types on 0h). */

/* Reorder a keys-or-vals vector by a grade-index vector (owned grade), then
 * collapse the boxed result back to a typed vector.  Releases `grade`. */
static ray_t* q_reindex_collapse(ray_t* vec, ray_t* grade) {
    if (!grade || RAY_IS_ERR(grade)) return grade;
    ray_t* boxed = ray_at_fn(vec, grade);
    ray_release(grade);
    if (!boxed || RAY_IS_ERR(boxed)) return boxed;
    if (boxed->type == RAY_LIST) {
        ray_t* c = q_collapse_list(boxed);
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
static ray_t* q_grade_table(ray_t* t, int desc) {
    int64_t nc = ray_table_ncols(t);
    int64_t nr = ray_table_nrows(t);
    if (nc <= 0) return ray_error("type", NULL);
    if (nr == 0) {
        ray_t* g = ray_vec_new(RAY_I64, 0);
        if (g && !RAY_IS_ERR(g)) g->len = 0;
        return g;
    }
    ray_t* idx = ray_vec_new(RAY_I64, nr);           /* til nr — the tiebreaker key */
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("oom", NULL);
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
                col = q_reindex_collapse(col, perm);          /* owned copy */
            }
            if (!col || RAY_IS_ERR(col)) {
                g = col ? col : ray_error("type", NULL);
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
            return g ? g : ray_error("oom", NULL);
        }
        /* Fold the chunk grade into the running permutation: perm = perm[g] */
        if (perm) {
            ray_t* np = q_reindex_collapse(perm, g);          /* releases g */
            ray_release(perm);
            perm = np;
        } else {
            perm = g;
        }
        if (!perm || RAY_IS_ERR(perm)) {
            ray_release(idx);
            return perm ? perm : ray_error("oom", NULL);
        }
    }
    ray_release(idx);
    return perm;
}

/* Grade a dict's VALUE vector.  Dict vals are stored as a boxed RAY_LIST, so
 * collapse to a typed vector before grading (a genuinely mixed value list can't
 * collapse and ray_iasc_fn errors → the by-type-number sort is DEFERRED). */
static ray_t* q_dict_value_grade(ray_t* vals, int desc) {
    ray_t* cv = (vals && vals->type == RAY_LIST) ? q_collapse_list(vals) : NULL;
    ray_t* use = cv ? cv : vals;
    /* A KEYED table's value is a table — grade its rows, not its columns (this is
     * what lets the dict law carry keyed tables: `asc kt` gathers key+value rows
     * by the grade of the value rows, per ref/asc.md's non-key-column rule). */
    if (use && use->type == RAY_TABLE) {
        ray_t* g = q_grade_table(use, desc);
        if (cv) ray_release(cv);
        return g;
    }
    /* Empty dict (e.g. `asc 0#d`): an empty value list can't be graded by
     * ray_iasc_fn (it needs a typed vector, and q_collapse_list leaves an empty
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
static ray_t* q_grade_dict(ray_t* d, int desc) {
    ray_t* keys = ray_dict_keys(d);      /* borrowed */
    ray_t* vals = ray_dict_vals(d);      /* borrowed */
    if (!keys || !vals) return ray_error("type", "iasc/idesc: malformed dict");
    ray_t* grade = q_dict_value_grade(vals, desc);
    return q_reindex_collapse(keys, grade);             /* releases grade */
}

ray_t* q_iasc_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT)  return q_grade_dict(x, 0);
    if (x && x->type == RAY_TABLE) return q_grade_table(x, 0);
    return ray_iasc_fn(x);
}
ray_t* q_idesc_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT)  return q_grade_dict(x, 1);
    if (x && x->type == RAY_TABLE) return q_grade_table(x, 1);
    return ray_idesc_fn(x);
}

/* q `width xbar list` — interval bucketing.  rayfall ray_xbar_fn is (col,
 * bucket); q spells it (bucket, col), so swap the arguments.  Everything else
 * (numeric int/float bucket, temporal cols, list zip) is handled by the base
 * kernel; dict/keyed-table/qSQL forms fall through to whatever the base does. */
ray_t* q_xbar_wrap(ray_t* bucket, ray_t* col) {
    return ray_xbar_fn(col, bucket);
}

/* ===== `?` GENERATE arms (ref/deal.md "Generate") ===========================
 * All arms draw from libc rand() (one stream — `\S n` re-seeds them all);
 * each right-operand form yields a result of y's type per the docs table. */

/* n?`m — n symbols of m chars each from "abcdefghijklmnop"; m is the numeric
 * symbol's NAME (`2 -> 2), 1<=m<=8 -> 'length otherwise (unpinned error
 * class; chosen to mirror the docs' n<=8 bound).  distinct (deal) draws by
 * generate-and-retry with a linear scan over accepted ids — fine at the
 * 16^m<=4.3e9 space for practical n; n>16^m is 'length. */
static ray_t* q_gen_syms(int64_t n, ray_t* ysym, int distinct) {
    static const char letters[] = "abcdefghijklmnop";
    ray_t* nm = ray_sym_str(ysym->i64);
    if (!nm || RAY_IS_ERR(nm))
        return nm ? nm : ray_error("type", "?: malformed symbol operand");
    const char* s = ray_str_ptr(nm);
    size_t sl = ray_str_len(nm);
    int64_t m = 0;
    int numeric = sl > 0 && sl <= 2;
    for (size_t i = 0; numeric && i < sl; i++) {
        if (s[i] < '0' || s[i] > '9') numeric = 0;
        else m = m * 10 + (s[i] - '0');
    }
    ray_release(nm);
    if (!numeric)
        return ray_error("type", "?: sym generate needs a numeric symbol (`1..`8)");
    if (m < 1 || m > 8)
        return ray_error("length", "?: sym generate length must be 1-8");
    if (distinct) {
        int64_t space = 1;
        for (int64_t j = 0; j < m; j++) space *= 16;
        if (n > space)
            return ray_error("length", "?: cannot deal %lld distinct %lld-char syms",
                             (long long)n, (long long)m);
    }
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int64_t i = 0; i < n; i++) {
        char buf[8];
        int64_t id;
        for (;;) {
            for (int64_t j = 0; j < m; j++) buf[j] = letters[rand() % 16];
            id = ray_sym_intern(buf, (size_t)m);
            if (!distinct) break;
            const int64_t* got = (const int64_t*)ray_data(out);
            int dup = 0;
            for (int64_t k = 0; k < i; k++) if (got[k] == id) { dup = 1; break; }
            if (!dup) break;
        }
        out = ray_vec_append(out, &id);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    }
    return out;
}

/* n?f — uniform floats in [0,y); result is y's type (F64 float / F32 real).
 * 62 random bits give the fraction; a rounding hit at the top is clamped
 * back below y so the [0,y) contract holds exactly. */
static ray_t* q_gen_floats(int64_t n, ray_t* y) {
    double fy;
    int f32 = (y->type == -RAY_F32);
    if (!q_strict_f64(y, &fy) || fy != fy || fy < 0)
        return ray_error("domain", "?: bound");
    ray_t* out = ray_vec_new(f32 ? RAY_F32 : RAY_F64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    for (int64_t i = 0; i < n; i++) {
        uint64_t u = ((uint64_t)rand() << 31) | (uint64_t)rand();
        double v = fy * ((double)u / 4611686018427387904.0);   /* / 2^62 */
        if (f32) {
            float fv = (float)v;
            if (fv >= (float)fy && fy > 0) fv = nextafterf((float)fy, 0.0f);
            ((float*)ray_data(out))[i] = fv;
        } else {
            if (v >= fy && fy > 0) v = nextafter(fy, 0.0);
            ((double*)ray_data(out))[i] = v;
        }
    }
    return out;
}

/* n?0b — random booleans (01b). */
static ray_t* q_gen_bits(int64_t n) {
    ray_t* out = ray_vec_new(RAY_BOOL, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    for (int64_t i = 0; i < n; i++) ((bool*)ray_data(out))[i] = rand() & 1;
    return out;
}

/* n?0x0 — random bytes 0x00-0xff. */
static ray_t* q_gen_bytes(int64_t n) {
    ray_t* out = ray_vec_new(RAY_U8, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    for (int64_t i = 0; i < n; i++)
        ((uint8_t*)ray_data(out))[i] = (uint8_t)(rand() & 0xFF);
    return out;
}

/* n?0 / n?0i — full-range longs/ints.  rand() yields 31 bits, so words are
 * composed from multiple calls.  The engine's sentinel values (0N=INT_MIN,
 * -0W=INT_MIN+1, 0W=INT_MAX) are never generated (rejection loop) — a roll
 * must not fabricate nulls/infinities (decision recorded in the plan). */
static ray_t* q_gen_longs(int64_t n) {
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int64_t v;
        do {
            uint64_t u = ((uint64_t)rand() << 33) |
                         ((uint64_t)rand() << 2)  |
                         ((uint64_t)rand() & 3);
            v = (int64_t)u;
        } while (v == INT64_MIN || v == INT64_MIN + 1 || v == INT64_MAX);
        d[i] = v;
    }
    return out;
}

static ray_t* q_gen_ints(int64_t n) {
    ray_t* out = ray_vec_new(RAY_I32, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int32_t* d = (int32_t*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int32_t v;
        do {
            uint32_t u = ((uint32_t)rand() << 1) | ((uint32_t)rand() & 1);
            v = (int32_t)u;
        } while (v == INT32_MIN || v == INT32_MIN + 1 || v == INT32_MAX);
        d[i] = v;
    }
    return out;
}

/* Uniform draw in [0,m), m>0 — a single rand()%m cannot reach past RAND_MAX
 * (31-bit glibc, 15-bit Windows CRT) and would fold a big roll (n?.z.p) onto
 * its start, so compose 63 bits from 15-bit chunks (the portable floor) and
 * reject draws past the last whole multiple of m (exact uniform, no modulo
 * bias; rejection odds < 1/2 per draw). */
static int64_t q_rand_below(int64_t m) {
    const uint64_t span = 1ULL << 63;
    const uint64_t limit = span - span % (uint64_t)m;
    uint64_t u;
    do {
        u = 0;
        for (int bits = 0; bits < 63; bits += 15)
            u = (u << 15) | ((uint64_t)rand() & 0x7FFF);
        u &= span - 1;
    } while (u >= limit);
    return (int64_t)(u % (uint64_t)m);
}

/* n?t — temporal roll, uniform on [0,y) over the backing payload (ref/deal.md:
 * "float, temporal >=0 -> 0 to y"; `4?2012.09m` is its transcript).  Draw an
 * i64 within the backing payload range, then re-tag through q_cast_to — THE
 * one conversion home (q_registry.h) — so the payload->temporal law stays
 * there.  y=0 degenerates
 * to all-zero items (the float row's y*0 behaviour); y<0 or null -> the
 * pinned float-bound class 'domain. */
static ray_t* q_gen_temporal(int64_t n, ray_t* y) {
    int8_t tag = (int8_t)-y->type;
    ray_t* v;
    if (RAY_IS_TEMPORALF(tag)) {              /* datetime rides the float roll
                                               * (q_strict_f64 reads its payload) */
        v = q_gen_floats(n, y);
    } else {
        int64_t m = RAY_IS_TEMPORAL64(tag) ? y->i64 : (int64_t)y->i32;
        if (m < 0) return ray_error("domain", "?: bound");   /* nulls are negative sentinels */
        v = ray_vec_new(RAY_I64, n > 0 ? n : 1);
        if (RAY_IS_ERR(v)) return v;
        v->len = n;
        int64_t* d = (int64_t*)ray_data(v);
        for (int64_t i = 0; i < n; i++) d[i] = m ? q_rand_below(m) : 0;
    }
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* r = q_cast_to(tag, v);
    ray_release(v);
    return r;
}

/* n?" " — chars drawn from .Q.a (ref/deal.md y-table row `" " -> .Q.a`).
 * Result is a RAY_STR atom = the string model's char list (provisional,
 * ARCHITECTURE.md). */
static ray_t* q_gen_chars(int64_t n) {
    char stackb[1024];
    char* b = (n < (int64_t)sizeof stackb) ? stackb : malloc((size_t)n + 1);
    if (!b) return ray_error("wsfull", "?: out of memory");
    for (int64_t i = 0; i < n; i++) b[i] = (char)('a' + rand() % 26);
    ray_t* s = ray_str(b, (size_t)n);
    if (b != stackb) free(b);
    return s;
}

/* Deal n distinct values from [0,total) — partial Fisher-Yates over `til total`,
 * take the first n (kdb deal / permute; uses the same libc rand() the roll path
 * does).  n<=total required.  Result is an owned I64 vector. */
static ray_t* q_deal_indices(int64_t n, int64_t total) {
    if (n < 0) return ray_error("domain", "?: deal count must be non-negative");
    if (n > total) return ray_error("length", "?: cannot deal %lld from %lld",
                                    (long long)n, (long long)total);
    ray_t* arr = ray_vec_new(RAY_I64, total > 0 ? total : 1);
    if (RAY_IS_ERR(arr)) return arr;
    arr->len = total;
    int64_t* a = (int64_t*)ray_data(arr);
    for (int64_t i = 0; i < total; i++) a[i] = i;
    for (int64_t i = 0; i < n; i++) {
        int64_t range = total - i;
        int64_t j = i + (int64_t)(rand() % range);
        int64_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
    arr->len = n;                          /* take first n (buffer already sized) */
    return arr;
}

/* Deal/permute n indices then gather them from the list y (collapsed). */
static ray_t* q_deal_pick(int64_t n, ray_t* y) {
    ray_t* idx = q_deal_indices(n, ray_len(y));
    if (!idx || RAY_IS_ERR(idx)) return idx;
    ray_t* out = ray_at_fn(y, idx);
    ray_release(idx);
    if (out && out->type == RAY_LIST) { ray_t* c = q_collapse_list(out); ray_release(out); return c; }
    return out;
}

/* q `x?y` — find / roll / pick (type-dispatch on the operands).
 *   list ? y   -> find.  kdb miss semantics: the smallest index NOT in the
 *                 list, i.e. `count x` — rayfall find returns 0N on a miss
 *                 (atom result) or per-element 0N (vector needle), so both
 *                 shapes are remapped to count here.
 *   n ? int    -> roll: n randoms in [0,int)  (rayfall rand)
 *   n ? list   -> pick: n random indices gathered from the list
 *   -n ? m / 0N ? m -> deal / permute (q_deal_indices)
 *   generate arms (`m sym, float, temporal, " ", 0b, 0x0, 0, 0i, 0Ng) ->
 *   q_gen_* above.  Deferred cells (error, never a wrong answer): short y,
 *   non-" " char pick (string model), deal of 0/0i. */
/* First index of x (a boxed list) whose ITEM whole-matches v, else cnt
 * (the kdb miss).  Borrows both. */
static int64_t q_list_find_item(ray_t* x, ray_t* v, int64_t cnt) {
    ray_t** ex = (ray_t**)ray_data(x);
    for (int64_t i = 0; i < cnt; i++)
        if (ex[i] && q_values_match(ex[i], v)) return i;
    return cnt;
}

ray_t* q_roll_wrap(ray_t* x, ray_t* y) {
    /* d?y — reverse dictionary lookup (basics/dictsandtables.md): the key of
     * the FIRST value matching y, i.e. keys[vals?y].  A find miss lands at
     * count vals, and ray_at_fn null-fills that out-of-range key index — the
     * typed null of the key domain, kdb's miss result.  Keyed tables keep
     * their own (deferred) path. */
    if (x && x->type == RAY_DICT && !q_is_keyed_table(x)) {
        ray_t* keys = ray_dict_keys(x);              /* borrowed */
        if (!keys) return ray_error("type", "?: dict");
        int vo = 0;
        ray_t* vv = q_dict_vals_vec(x, &vo);
        if (!vv) return ray_error("type", "?: dict");
        ray_t* i = q_roll_wrap(vv, y);               /* find arm: miss -> count */
        if (vo) ray_release(vv);
        if (!i || RAY_IS_ERR(i)) return i;
        ray_t* r = ray_at_fn(keys, i);
        ray_release(i);
        if (r && r->type == RAY_LIST) { ray_t* c = q_typed_empty_like(q_collapse_list(r), keys); ray_release(r); return c; }
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
                o[j] = e[j] ? q_list_find_item(x, e[j], cnt) : cnt;
            return out;
        }
        if (x_ranked && y && !ray_is_atom(y) && ray_is_vec(y) && y->type != RAY_LIST) {
            /* list-of-lists x, SIMPLE vector y: whole-y match (`u?10 2 -6`
             * -> 1). */
            return ray_i64(q_list_find_item(x, y, cnt));
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
                        rr = q_roll_wrap(x, e[j]);
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
    int64_t nx;
    if (!q_strict_i64(x, &nx)) return ray_error("type", "?: x");
    if (RAY_ATOM_IS_NULL(x)) {                  /* 0N ? y — permute all items */
        if (y && (y->type == -RAY_I64 || y->type == -RAY_I32)) {
            int64_t m = q_iatom_val(y);
            if (m < 0) return ray_error("type", "?: permute n");
            return q_deal_indices(m, m);
        }
        if (y && (ray_is_vec(y) || y->type == RAY_LIST)) return q_deal_pick(ray_len(y), y);
        return ray_error("nyi", "?: permute y");
    }
    int deal = nx < 0;
    int64_t n = deal ? -nx : nx;
    if (y && (ray_is_vec(y) || y->type == RAY_LIST)) {
        if (deal) return q_deal_pick(n, y);     /* -n?list — deal, no replacement */
        /* n?list — pick.  Indices draw via the engine KERNEL directly
         * (ray_rand_fn), not the env name — the bootstrap shadow-rebinds root
         * `rand` to `.q.rand`. */
        ray_t* cnt = ray_i64(n);
        ray_t* len = ray_i64(ray_len(y));
        ray_t* idx = ray_rand_fn(cnt, len);
        ray_release(cnt);
        ray_release(len);
        if (!idx || RAY_IS_ERR(idx)) return idx;
        ray_t* out = ray_at_fn(y, idx);
        ray_release(idx);
        if (out && out->type == RAY_LIST) {
            ray_t* c = q_collapse_list(out);
            ray_release(out);
            return c;
        }
        return out;
    }
    if (y && y->type < 0) {
        /* ---- generate arms: ONE switch over the value band, total by
         * -Wswitch + -Werror (no `default:` — a missing tag refuses to
         * build; structural leftovers return AFTER the switch). ---- */
        switch ((ray_type_e)-y->type) {
        case RAY_LIST:                          /* dead arm: an atom tag is strictly negative */
            break;
        case RAY_BOOL:
            if (deal) return ray_error("type", "?: deal y");
            if (y->b8) return ray_error("nyi", "?: y");   /* roll defined for 0b only */
            return q_gen_bits(n);               /* n?0b */
        case RAY_GUID: {                        /* n?0Ng / -n?0Ng — env guid.
             * Deal reuses the same generator: distinctness rests on the
             * 122-bit space (collisions negligible); kdb's process/time deal
             * seed nuance is NOT reproduced (recorded divergence). */
            if (!RAY_ATOM_IS_NULL(y))
                return ray_error("type", "?: y");   /* guid generate needs 0Ng */
            ray_t* cnt = ray_i64(n);
            ray_t* g = q_env_call1("guid", cnt);
            ray_release(cnt);
            return g;
        }
        case RAY_U8:
            if (deal) return ray_error("type", "?: deal y");
            if (y->u8) return ray_error("nyi", "?: y");   /* roll defined for 0x0 only */
            return q_gen_bytes(n);              /* n?0x0 */
        case RAY_I16:
            return ray_error("nyi", "?: y");    /* short roll/deal deferred */
        case RAY_I32: case RAY_I64: {
            if (!RAY_ATOM_IS_NULL(y) && q_iatom_val(y) == 0) {  /* n?0 / n?0i full-range */
                if (deal) return ray_error("nyi", "?: deal 0");
                return (y->type == -RAY_I64) ? q_gen_longs(n) : q_gen_ints(n);
            }
            if (deal) {                         /* -n?m — deal, no replacement */
                int64_t m = q_iatom_val(y);
                if (m <= 0) return ray_error("domain", "?: deal m");
                return q_deal_indices(n, m);
            }
            ray_t* cnt = ray_i64(n);            /* n?m — roll via the kernel */
            ray_t* r = ray_rand_fn(cnt, y);
            ray_release(cnt);
            return r;
        }
        case RAY_F32: case RAY_F64:
            if (deal) return ray_error("type", "?: deal y");
            return q_gen_floats(n, y);          /* n?f uniform [0,y) */
        case RAY_STR:                           /* char atom: only `" "` has a
             * roll law (-> .Q.a); other strings are pick-from-chars =
             * string-model territory, deferred. */
            if (deal) return ray_error("type", "?: deal y");
            if (ray_str_len(y) == 1 && ray_str_ptr(y)[0] == ' ')
                return q_gen_chars(n);
            return ray_error("nyi", "?: y");
        case RAY_SYM:
            return q_gen_syms(n, y, deal);      /* n?`m sym roll / deal */
        case RAY_TIMESTAMP: case RAY_MONTH: case RAY_DATE: case RAY_DATETIME:
        case RAY_TIMESPAN: case RAY_MINUTE: case RAY_SECOND: case RAY_TIME:
            /* the doc's "float, temporal" row is Roll-only -> deal is 'type
             * exactly as the float arm above */
            if (deal) return ray_error("type", "?: deal y");
            return q_gen_temporal(n, y);
        }
    }
    return ray_error("nyi", "?: y");   /* structural atom y (lambda, ...): no roll law */
}
