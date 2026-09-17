/* ops/q_attr.c — column attributes (`` `s#`u#`g#`p# ``): the q surface over the
 * engine's built accelerator-index family (.attr.*).
 *
 * Consumers: the `attr` verb, q_fmt's display prefix, `#`'s set-attribute arm
 * (ops/q_takedrop.c), q.q's asc/xasc via `.Q.c.sorted`/`.Q.c.parted`, the three
 * append verbs via q_table.c, and the wire decoder via q_attr_stamp_byte.
 * The lifecycle rule (ARCHITECTURE.md 2026-09-16): a u/g/p letter exists only
 * with its index on the same block — attr_build is the ONE place that mints one;
 * `s` needs no index and is the one letter a caller may vouch for.
 * Design: docs/attributes-status.md. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_count.h"
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"  /* q_type_* guards */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_value — the retention scan composes on `<=` */
#include "ops/idxop.h"     /* .attr.* engine calls: ray_attr_*, RAY_IDX_*, RAY_MARK_* */
#include "mem/heap.h"      /* RAY_ATTR_HAS_NULLS / RAY_ATTR_SORTED, ray_cow */
#include "table/sym.h"     /* ray_sym_intern_runtime */
#include <string.h>        /* strcmp — the error remap */

/* Read a vector's attribute as kdb's SINGLE letter: 's'/'u'/'g'/'p', or 0 for
 * none.  Reads the block markers/kind DIRECTLY rather than delegating to the
 * engine's `.attr.get` (ray_attr_get_fn is now rayfall-native and would mislabel
 * q's hash-backed `u#`/`p#` — which carry RAY_IDX_HASH + a marker — as `g`).
 * The kdb u#/p# policy is composed in q (attr_build): the marker bit is the
 * attribute identity, winning over the hash kind; a bare hash is `g`, a native
 * RAY_IDX_PART directory is `p`, and the attrs sorted bit is `s`.  Borrows v;
 * never releases it. */
char q_attr_letter(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return 0;
    if (v->type == RAY_ENUM) return q_enum_attr(v);       /* letter rides the domain slot */
    if (v->type == RAY_TABLE || v->type == RAY_DICT ||    /* `s#t/`s#kt key table (set-attribute.md:35-37) */
        v->type == RAY_LIST)                              /* asc of a nested list carries s (asc.md:31-33) */
        return (v->attrs & RAY_ATTR_SORTED) ? 's' : 0;
    if (!ray_is_vec(v)) return 0;
    if (ray_index_has(v)) {
        ray_index_t* ix = ray_index_payload(v->index);
        if (ix->markers & RAY_MARK_UNIQUE) return 'u';
        if (ix->markers & RAY_MARK_PARTED) return 'p';
        if (ix->markers & RAY_MARK_GROUPED) return 'g';
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

/* PRODUCER `s#` (asc/xasc): no verify, no numeric gate — the caller just built
 * the order, and the gate would drop symbol sorts (asc.md:18-28, 189).  Borrows x. */
ray_t* q_attr_stamp_sorted(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x ? (ray_retain(x), x) : q_err(QE_TYPE);
    ray_retain(x);
    if ((ray_is_vec(x) || x->type == RAY_LIST) &&
        !(x->attrs & (RAY_ATTR_SLICE | RAY_ATTR_ARENA)))
        x->attrs |= RAY_ATTR_SORTED;   /* IN PLACE — asc.md:31-33: an already-ordered
                                        * argument (identity grade) is marked itself */
    return x;
}

/* True iff r[from..from+n) is non-descending.  Composes `<=`/`min`/truthiness
 * over two zero-copy slice VIEWS, so it owns no type knowledge (symbols included). */
static bool non_descending(ray_t* r, int64_t from, int64_t n) {
    int64_t m = n - 1;
    if (m < 1) return true;
    ray_t* a = ray_vec_slice(r, from, m);
    ray_t* b = ray_vec_slice(r, from + 1, m);
    ray_t* le = NULL;
    if (a && !RAY_IS_ERR(a) && b && !RAY_IS_ERR(b)) {
        ray_t* f = q_registry_lookup_name("<=", 2, Q_DYADIC);      /* borrowed */
        ray_t* av[2] = { a, b };
        le = f ? q_eval_apply_value(f, av, 2) : NULL;
    }
    if (a && !RAY_IS_ERR(a)) ray_release(a);
    if (b && !RAY_IS_ERR(b)) ray_release(b);
    if (!le || RAY_IS_ERR(le)) { if (le) ray_release(le); return false; }
    ray_t* g = q_registry_lookup_name("min", 3, Q_MONADIC);        /* borrowed */
    ray_t* all = g ? q_eval_apply_value(g, &le, 1) : NULL;
    ray_release(le);
    if (!all) return false;
    ray_t* err = NULL;
    int ok = q_eval_apply_truthy(all, &err);                       /* consumes all */
    if (err) { ray_release(err); return false; }
    return ok != 0;
}

/* The vouched `s` stamp (the wire 127 tag, the disk byte 1): sortedness needs no
 * index, so the caller's word is enough.  Any other letter is DROPPED here — a
 * u/g/p letter is minted only by attr_build.  Owned exclusive v. */
ray_t* q_attr_stamp_trusted(ray_t* v, char letter) {
    if (!v || RAY_IS_ERR(v) || letter != 's') return v;
    if (v->type == RAY_ENUM) {
        if (v->rc == 1) (void)q_enum_attr_set(v, letter);
        return v;
    }
    /* the carrier set MIRRORS q_attr_letter's sorted arm, or a stamp and a read disagree */
    if ((ray_is_vec(v) || v->type == RAY_LIST || v->type == RAY_TABLE || v->type == RAY_DICT) &&
        !(v->attrs & (RAY_ATTR_SLICE | RAY_ATTR_ARENA)) && v->rc == 1)
        v->attrs |= RAY_ATTR_SORTED;
    return v;
}

/* kdb spells an attribute as a BYTE — the same 0-4 on the wire (kb/serialization.md:39) and on disk —
 * which COLLIDES with rayforce's attrs bits, so both directions translate through the letter above
 * rather than copying the bitfield. */
static const char attr_byte_letters[5] = { 0, 's', 'u', 'p', 'g' };

uint8_t q_attr_byte(ray_t* v) {
    char c = q_attr_letter(v);
    for (uint8_t b = 1; b < 5; b++)
        if (attr_byte_letters[b] == c) return b;
    return 0;
}

ray_t* q_attr_stamp_byte(ray_t* v, uint8_t byte) {
    if (!v || RAY_IS_ERR(v) || byte < 1 || byte > 4) return v;
    return q_attr_rebuild(v, attr_byte_letters[byte]);
}

/* Remap the engine's set-attribute failure codes to kdb's error text.  The
 * verification failures use "domain"; the engine's numeric-only gate (reached
 * by `s#`) uses "nyi"; type guards use "type".  kdb signals: `'s-fail` (sorted not ascending), `'u-fail`
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
    int64_t nulls = 0, n = q_count(v);
    for (int64_t i = 0; i < n; i++)
        if (ray_vec_is_null(v, i) && ++nulls > 1) return false;
    return true;
}

/* A copying writer's block for the law: a clone, so the source keeps its own while the result's is extended.
 * NULL when no extension could keep the letter (not u/g, or a mapped block). */
ray_t* q_attr_index_clone(ray_t* x) {
    char l = q_attr_letter(x);
    if ((l != 'u' && l != 'g') || !ray_index_has(x) || x->index->mmod == 1 ||
        (ray_index_payload(x->index)->markers & RAY_MARK_MMAP))
        return NULL;
    ray_t* c = ray_index_clone(x->index);
    if (c && RAY_IS_ERR(c)) { ray_error_free(c); return NULL; }
    return c;
}

/* A kdb u# holds at most ONE null (attr_no_dup_nulls); the hash skips nulls, so a null among the new rows is judged
 * here: the letter survives iff the whole vector still holds at most one. */
static bool u_nulls_ok(ray_t* r, int64_t nx) {
    for (int64_t i = nx, n = q_count(r); i < n; i++)
        if (ray_vec_is_null(r, i)) return attr_no_dup_nulls(r);
    return true;
}

/* THE append-retention law (set-attribute.md:31-32, :94; the table in docs/attributes-status.md): s kept iff the
 * tail from x's LAST item is non-descending; u/g kept iff the index EXTENDS over the new rows (a u repeat drops
 * the letter, no error); p dropped.  Stated over x's letter, length and DETACHED block (idx, owned, consumed —
 * NULL when nothing could be kept), not x itself, so an append that grew x IN PLACE applies it too (x's own
 * letter is stale by then, its length the result's).  r CONSUMED (rc==1 fresh); slices/arena refused. */
ray_t* q_attr_append_keep(char lx, int64_t nx, ray_t* idx, ray_t* r) {
    bool vec = r && !RAY_IS_ERR(r) && ray_is_vec(r) && !q_attr_letter(r) && !(r->attrs & (RAY_ATTR_SLICE | RAY_ATTR_ARENA))
            && nx <= q_count(r);
    if (lx == 'u' || lx == 'g') {
        if (idx && vec && (lx == 'g' || u_nulls_ok(r, nx)) && ray_index_extend(idx, r, nx) == 1) {
            ray_t* e = ray_index_attach_built(&r, idx);
            if (e && !RAY_IS_ERR(e)) return e;
            if (e) ray_error_free(e);
        }
        if (idx) ray_release(idx);
        return r;
    }
    if (idx) ray_release(idx);
    if (lx != 's' || !vec) return r;
    if (!non_descending(r, nx > 0 ? nx - 1 : 0, q_count(r) - (nx > 0 ? nx - 1 : 0))) return r;
    r = ray_cow(r);                 /* rc==1 here (fresh append result): in place */
    if (r && !RAY_IS_ERR(r)) r->attrs |= RAY_ATTR_SORTED;
    return r;
}

/* The store twin of the append law: s survives a store at pos[0..m) iff every written cell still sits between its
 * neighbours (the same scan, one window per position).  An index-backed letter left with its index at the write
 * (ray_vec_set drops it — the table's amend cell).  r is the writer's, already written. */
void q_attr_store_keep(ray_t* r, const int64_t* pos, int64_t m) {
    if (!r || !ray_is_vec(r) || !(r->attrs & RAY_ATTR_SORTED)) return;
    int64_t n = q_count(r);
    for (int64_t j = 0; j < m; j++) {
        int64_t from = pos[j] > 0 ? pos[j] - 1 : 0;
        if (!non_descending(r, from, n - from < 3 ? n - from : 3)) { r->attrs &= (uint8_t)~RAY_ATTR_SORTED; return; }
    }
}

/* THE one place a u/g/p letter is minted: verify the layout (u distinct with
 * kdb's null policy, p contiguous, g nothing), build the index on the block,
 * set the identity marker.  A SYMBOL vector is already id-coded, so its index
 * is the direct-addressed CODES kind (its build is the verify; the domain
 * pointer stays in place beside it).  Every other numeric vector — the float
 * family included — takes the hash, probed by the builder's canonical word.
 * The engine attach contract: *vp is
 * the caller's owned live vector, cow'd when shared; NULL on success, else an
 * owned RAW engine error with *vp untouched (the `#` door remaps it). */
static ray_t* attr_build(ray_t** vp, char letter) {
    ray_t* v = *vp;
    uint8_t mark = letter == 'u' ? RAY_MARK_UNIQUE : letter == 'p' ? RAY_MARK_PARTED : RAY_MARK_GROUPED;
    if (ray_is_vec(v) && v->type == RAY_SYM) {
        ray_t* e = ray_index_attach_codes(vp, letter == 'u' ? RAY_CODES_UNIQUE
                                            : letter == 'p' ? RAY_CODES_PARTED : RAY_CODES_GROUPED);
        if (RAY_IS_ERR(e)) return e;
        ray_index_payload((*vp)->index)->markers |= mark;
        return NULL;
    }
    if (!ray_is_vec(v) || ray_attr_numeric_class(v->type) < 0) return q_err(QE_TYPE);
    bool ok = letter == 'u' ? (ray_attr_verify_distinct(v) && attr_no_dup_nulls(v))
            : letter == 'p' ? ray_attr_verify_contiguous(v) : true;
    if (!ok) return q_err(QE_DOMAIN);
    ray_t* e = letter == 'u' ? ray_index_attach_hash_unique(vp) : ray_index_attach_hash(vp);
    if (RAY_IS_ERR(e)) return e;
    if (letter != 'g') ray_index_payload((*vp)->index)->markers |= mark;   /* the attach's own block: no clone */
    return NULL;
}

/* The decode / producer door onto attr_build: IPC and `-9!` hand a letter with
 * no index, asc and `s#t` hand a column they just ordered.  A letter the
 * carrier cannot hold or the data contradicts is DROPPED, never an error (a
 * lying byte is corrupt-class); a resource failure is the caller's.  Owned v
 * in, owned out (consumed on that error). */
ray_t* q_attr_rebuild(ray_t* v, char letter) {
    if (!v || RAY_IS_ERR(v) || !letter) return v;
    if (letter == 's') return q_attr_stamp_trusted(v, 's');
    ray_t* e = attr_build(&v, letter);
    if (!e) return v;
    const char* code = ray_err_code(e);
    if (code && (strcmp(code, "type") == 0 || strcmp(code, "domain") == 0)) { ray_error_free(e); return v; }
    ray_release(v);
    return e;
}

/* `s#` on a TABLE: verify the FIRST column, stamp parted on it (sortedness
 * implies contiguity; set-attribute.md:65-73) and mark the table.  Borrows y. */
static ray_t* attr_set_table_s(ray_t* y) {
    int64_t nc = ray_table_ncols(y);
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(y, c);           /* borrowed */
        if (!col) { ray_release(out); return q_err(QE_TYPE); }
        if (c == 0) {
            if (!non_descending(col, 0, q_count(col))) { ray_release(out); return ray_error("s-fail", NULL); }
            ray_retain(col);
            ray_t* pc = ray_cow(col);                        /* copy-on-shared (:29) */
            if (!pc || RAY_IS_ERR(pc)) { ray_release(out); return pc ? pc : q_err(QE_OOM); }
            pc = q_attr_rebuild(pc, 'p');
            if (!pc || RAY_IS_ERR(pc)) { ray_release(out); return pc ? pc : q_err(QE_OOM); }
            out = ray_table_add_col(out, ray_table_col_name(y, c), pc);
            ray_release(pc);
        } else {
            out = ray_table_add_col(out, ray_table_col_name(y, c), col);
        }
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    }
    out->attrs |= RAY_ATTR_SORTED;
    return out;
}

/* `s#` on a KEYED table stamps the KEY table IN PLACE (set-attribute.md:63 — why the :35-37 pin sees
 * `s from an UNASSIGNED `s#t) and "copies the outer object" (:75), so the RESULT is a sorted dict
 * while the argument keeps its own attribute.  The key table's first column is NOT parted:
 * kb/serialization.md's keyed-table frame carries 00 there where the plain-table frame carries 03. */
static ray_t* attr_set_keyed_s(ray_t* y) {
    ray_t* kt = ray_dict_keys(y);                            /* borrowed key TABLE */
    if (!kt || kt->type != RAY_TABLE || ray_table_ncols(kt) < 1) return q_err(QE_TYPE);
    ray_t* c0 = ray_table_get_col_idx(kt, 0);
    if (!c0 || !non_descending(c0, 0, q_count(c0))) return ray_error("s-fail", NULL);
    kt->attrs |= RAY_ATTR_SORTED;
    ray_t* vals = ray_dict_vals(y);
    ray_retain(kt); ray_retain(vals);
    ray_t* d = ray_dict_new(kt, vals);                       /* consumes both */
    if (d && !RAY_IS_ERR(d)) d->attrs |= RAY_ATTR_SORTED;
    return d;
}

/* `s#` on a DICT: verify the key order (the step-function contract), stamp the
 * key vector and the copied outer dict. */
static ray_t* attr_set_dict_s(ray_t* y) {
    ray_t* k = ray_dict_keys(y);                             /* borrowed */
    if (!k || !ray_is_vec(k)) return q_err(QE_TYPE);
    if (!non_descending(k, 0, q_count(k))) return ray_error("s-fail", NULL);
    ray_retain(k);
    ray_t* nk = ray_cow(k);
    if (!nk || RAY_IS_ERR(nk)) return nk ? nk : q_err(QE_OOM);
    nk->attrs |= RAY_ATTR_SORTED;
    ray_t* vals = ray_dict_vals(y);
    ray_retain(vals);
    ray_t* d = ray_dict_new(nk, vals);                       /* consumes both */
    if (d && !RAY_IS_ERR(d)) d->attrs |= RAY_ATTR_SORTED;
    return d;
}

/* `` `# `` on a container: the undo of the arms above; values are left as data. */
static ray_t* attr_clear_container(ray_t* y) {
    if (y->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(y);
        ray_t* out = ray_table_new(nc > 0 ? nc : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        for (int64_t c = 0; c < nc; c++) {
            ray_t* dc = ray_attr_drop_fn(ray_table_get_col_idx(y, c));   /* owned */
            if (!dc || RAY_IS_ERR(dc)) { ray_release(out); return dc ? dc : q_err(QE_OOM); }
            out = ray_table_add_col(out, ray_table_col_name(y, c), dc);
            ray_release(dc);
            if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        }
        return out;
    }
    ray_t* k = ray_dict_keys(y);
    ray_t* nk = k && k->type == RAY_TABLE ? attr_clear_container(k)
              : k && ray_is_vec(k)        ? ray_attr_drop_fn(k)
              : NULL;
    if (!nk || RAY_IS_ERR(nk)) return nk ? nk : q_err(QE_TYPE);
    ray_t* vals = ray_dict_vals(y);
    ray_retain(vals);
    return ray_dict_new(nk, vals);                           /* consumes both */
}

/* A 20h vector takes no letter yet: its domain pointer sits where the index
 * would (deferred 3, #558), so u/g/p would be a letter without a hash — 'nyi;
 * `s#` orders by RESOLVED value — recorded deferral, 'type.  `#` clears. */
static ray_t* attr_set_enum(char letter, ray_t* y) {
    if (letter) return q_err(letter == 's' ? QE_TYPE : QE_NYI);
    ray_t* w;
    if (y->attrs & RAY_ATTR_SLICE) {                         /* a column VIEW: materialize */
        ray_t* pos = q_enum_positions(y);
        if (!pos || RAY_IS_ERR(pos)) return pos ? pos : q_err(QE_OOM);
        w = q_enum_stamp(pos, q_enum_domain(y));             /* consumes pos */
    } else {
        ray_retain(y);
        w = ray_cow(y);                                      /* always copies (:29) */
    }
    if (!w || RAY_IS_ERR(w)) return w ? w : q_err(QE_OOM);
    (void)q_enum_attr_set(w, 0);
    return w;
}

/* q `sym # vec` — set / clear a column attribute.  sym is a symbol ATOM: the
 * empty symbol clears (`.attr.drop`), `s`/`u`/`g`/`p` set the matching
 * attribute, any other letter is `'type` (the 5-symbol allow-list IS the guard —
 * a non-attribute symbol against a flat vector has no take meaning).  `s` goes
 * through the rayfall-native engine setter; `u`/`g`/`p` are built in q
 * (attr_build) so the kdb accelerator policy stays out of the frozen engine.
 * Borrows both args. */
ray_t* q_attr_set_dispatch(ray_t* n, ray_t* vec) {
    char letter = '?';                               /* unknown -> 'type */
    ray_t* s = ray_sym_str(n->i64);                  /* owned -RAY_STR */
    if (s) {
        size_t len = ray_str_len(s);
        if (len == 0) letter = 0;                    /* `#x -> clear */
        else if (len == 1) letter = ray_str_ptr(s)[0];
        ray_release(s);
    }
    if (vec && (vec->type == RAY_TABLE || vec->type == RAY_DICT)) {
        if (letter == 0)   return attr_clear_container(vec);
        if (letter == 's') return vec->type == RAY_TABLE ? attr_set_table_s(vec)
                                : q_type_is_keyed(vec)   ? attr_set_keyed_s(vec)
                                                         : attr_set_dict_s(vec);
        return q_err(QE_TYPE);
    }
    if (vec && vec->type == RAY_ENUM)
        return letter == '?' ? q_err(QE_TYPE) : attr_set_enum(letter, vec);
    switch (letter) {
    case 0:   return ray_attr_drop_fn(vec);          /* `#vec -> drop all */
    case 'u': case 'p': case 'g': {
        ray_t* r = ray_attr_drop_fn(vec);            /* `` `x# `` REPLACES any prior attribute: cleared base, owned */
        if (!r || RAY_IS_ERR(r)) return r ? r : q_err(QE_OOM);
        ray_t* e = attr_build(&r, letter);
        if (e) { ray_release(r); return attr_remap_err(e, letter); }
        return r;
    }
    case 's': break;
    default:  return q_err(QE_TYPE);        /* `z#vec etc. */
    }
    ray_t* nm = ray_sym(ray_sym_intern_runtime("sorted", 6));   /* owned -RAY_SYM */
    /* kdb: a vector carries at most ONE attribute, and `` `x# `` REPLACES any
     * prior one (`` attr `s#`g#1 2 3 `` -> `` `s ``).  The rayfall-native setter
     * preserves existing markers/index, so drop first, then set on the cleared
     * base.  ray_attr_drop_fn borrows vec and returns an owned (possibly COW'd)
     * result; ray_attr_set_fn borrows that base. */
    ray_t* base = ray_attr_drop_fn(vec);              /* owned */
    if (!base || RAY_IS_ERR(base)) { ray_release(nm); return base ? base : q_err(QE_OOM); }
    ray_t* r = ray_attr_set_fn(nm, base);             /* borrows nm, base */
    ray_release(nm);
    ray_release(base);
    if (r && RAY_IS_ERR(r)) return attr_remap_err(r, letter);
    return r;
}

/* `.Q.c.parted` — asc's multi-column producer twin: p built on a column asc
 * just ordered; a carrier-less column passes through unattributed.  Borrows x. */
ray_t* q_attr_stamp_parted(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x ? (ray_retain(x), x) : q_err(QE_TYPE);
    ray_retain(x);
    ray_t* w = ray_cow(x);
    if (!w || RAY_IS_ERR(w)) return w ? w : q_err(QE_OOM);
    return q_attr_rebuild(w, 'p');
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

