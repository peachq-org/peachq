/* ops/q_attr.c — column attributes (`` `s#`u#`g#`p# ``): the q surface over the
 * engine's built accelerator-index family (.attr.*).
 *
 * Split from q_list.c (2026-07-27) — pure function moves, no behaviour change.
 * Three consumers reach in here: the `attr` verb, q_fmt's display prefix, and
 * `#`'s set-attribute arm in ops/q_takedrop.c. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_err.h"
#include "qlang/ops/q_type.h"  /* q_type_* guards */
#include "ops/idxop.h"     /* .attr.* engine calls: ray_attr_*, RAY_IDX_*, RAY_MARK_* */
#include "mem/heap.h"      /* RAY_ATTR_HAS_NULLS — sorted-vector null probe */
#include "table/sym.h"     /* ray_sym_intern_runtime */
#include <string.h>        /* strcmp — the error remap */

/* ── Column attributes (`` `s#`u#`g#`p# ``) — the q surface over the engine's
 * built accelerator-index family (.attr.*).  See actionable-plans/
 * 2026-07-09-column-attributes.md.  Two entry points: q_attr_letter (shared by
 * the `attr` verb AND q_fmt's display prefix) and the `#` set-attribute arm. ── */

/* Read a vector's attribute as kdb's SINGLE letter: 's'/'u'/'g'/'p', or 0 for
 * none.  Reads the block markers/kind DIRECTLY rather than delegating to the
 * engine's `.attr.get` (ray_attr_get_fn is now rayfall-native and would mislabel
 * q's hash-backed `u#`/`p#` — which carry RAY_IDX_HASH + a marker — as `g`).
 * The kdb u#/p# policy is composed in q (attr_compose): the marker bit is the
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
static bool attr_no_dup_nulls(ray_t* v) {
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
static ray_t* attr_compose(ray_t* base, char letter) {
    int cls = ray_attr_numeric_class(base->type);
    if (cls < 0) return q_err(QE_TYPE);
    bool ok = (letter == 'u') ? (ray_attr_verify_distinct(base) && attr_no_dup_nulls(base))
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
 * match kdb); `u`/`p` are composed in q (attr_compose) so the kdb accelerator
 * policy stays out of the frozen engine.  Borrows both args. */
ray_t* q_attr_set_dispatch(ray_t* n, ray_t* vec) {
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
        /* kdb accelerator policy composed in q (see attr_compose).  `` `x# ``
         * REPLACES any prior attribute, so clear the base first. */
        ray_t* base = ray_attr_drop_fn(vec);         /* clear prior attr, owned */
        if (!base || RAY_IS_ERR(base)) return base ? base : q_err(QE_OOM);
        ray_t* r = attr_compose(base, letter);     /* borrows base */
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

