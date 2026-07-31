/* q_index — the one-code-path index/amend family (contract in q_index.h). */
#include "qlang/ops/q_index.h"
#include "qlang/ops/q_bang.h"   /* q_bang — the `!` verb, which rebuilds a selection */
#include "qlang/eval/q_eval.h"
#include "qlang/q_builtins.h"   /* q_builtins_count_long — THE count owner */
#include "qlang/q_err.h"
#include "qlang/q_registry_internal.h"
#include "qlang/q_type.h"       /* the type/shape axes: keyed, nested, iter */
#include "lang/internal.h"   /* as_i64 — the int-atom payload accessor */
#include "table/dict.h"
#include <stdlib.h>

#define IDX_MAX_DEPTH 2048

static _Thread_local int g_depth;

/* index admission: ANY int-backed atom indexes (bools/bytes/temporals
 * included; floats, chars and syms do not — ref/apply.md errors) */
static int idx_i64(ray_t* v, int64_t* out) {
    if (!v || !ray_is_atom(v)) return 0;
    if (v->type == -RAY_BOOL) { *out = v->b8 ? 1 : 0; return 1; }
    if (v->type == -RAY_CHARV) return 0;
    if (v->type == -RAY_I64 || v->type == -RAY_I32 || v->type == -RAY_I16 ||
        ray_is_bytelike(-v->type)) { *out = as_i64(v); return 1; }
    if (RAY_IS_TEMPORAL32(-v->type)) { *out = (int64_t)v->i32; return 1; }
    if (RAY_IS_TEMPORAL64(-v->type)) { *out = v->i64; return 1; }
    return 0;
}

static int is_coll(ray_t* v) {
    return v && !RAY_IS_ERR(v) && (ray_is_vec(v) || v->type == RAY_LIST);
}

static ray_t* collapse(ray_t* l) {                   /* consumes l */
    ray_t* c = q_list_collapse(l);
    ray_release(l);
    return c;
}

static ray_t* mat(ray_t* r) {                        /* consumes r */
    if (r && ray_is_lazy(r)) return ray_lazy_materialize(r);
    return r;
}

/* v[i] as an owned atom/element (borrowed v): direct payload read for
 * vectors/lists (collection_elem — no index atom, no ray_at_fn dispatch);
 * generic indexing for every other shape.  alloc==0 results are BORROWED
 * list slots — retain, never release (r0 review).  The one element-read home. */
ray_t* q_index_elem_at(ray_t* v, int64_t i) {
    if (v && v->type == RAY_TABLE)      /* the items of a table are its ROWS */
        return q_table_row_at(v, i);
    if (v && (ray_is_vec(v) || v->type == RAY_LIST)) {
        int alloc = 0;
        ray_t* e = collection_elem(v, i, &alloc);
        if (e && !RAY_IS_ERR(e)) { if (!alloc) ray_retain(e); return e; }
        if (e && alloc) ray_release(e);   /* allocated error: generic fallback */
    }
    ray_t* ia = ray_i64(i);
    ray_t* e  = ray_at_fn(v, ia);   /* owned */
    ray_release(ia);
    return e;
}

/* one join operand for a single item: atoms join natively, else boxed */
static ray_t* boxed1(ray_t* v) {
    if (ray_is_atom(v)) { ray_retain(v); return v; }
    ray_t* b = ray_list_new(1);
    return ray_list_append(b, v);
}

/* miss result for one absent/OOB element of c: the typed null of the FIRST
 * element's type (ref/apply.md Index; dict misses ride this via find) */
static ray_t* miss_null(ray_t* c) {
    if (ray_is_vec(c)) return ray_typed_null((int8_t)-c->type);
    if (c && c->type == RAY_LIST && ray_len(c) > 0) {
        ray_t* e0 = ((ray_t**)ray_data(c))[0];
        if (e0 && !RAY_IS_ERR(e0) && !RAY_IS_NULL(e0)) {
            if (ray_is_atom(e0)) return ray_typed_null(e0->type);
            if (ray_is_vec(e0)) return ray_typed_null((int8_t)-e0->type);
        }
    }
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* ===== the two level ops ================================================= */

static ray_t* store_level(ray_t* x, ray_t* i, ray_t* v);
static ray_t* amend_seq(ray_t* x, ray_t* sel, ray_t* const* rest, int64_t k,
                        ray_t* f, ray_t* y);
static ray_t* table_level(ray_t* t, ray_t* i, int write);
static ray_t* keyed_level(ray_t* x, ray_t* i, int write);
static ray_t* keyed_at(ray_t* x, ray_t* i);

/* one atom-index READ step.  Dict = find-then-index-values (a key miss is
 * the values' typed null, NEVER positional); vec/list = elem or miss.  In
 * write mode a miss/OOB is 'index (a path must exist to be amended). */
static ray_t* index_level(ray_t* x, ray_t* i, int write) {
    if (q_type_is_keyed(x)) return keyed_level(x, i, write);
    if (x->type == RAY_TABLE) return table_level(x, i, write);
    if (x->type == RAY_DICT) {
        int64_t ki = ray_dict_find_idx(x, i);
        ray_t* vals = ray_dict_slots(x)[1];
        if (ki < 0) return write ? q_err(QE_INDEX) : miss_null(vals);
        return q_index_elem_at(vals, ki);
    }
    if (!is_coll(x)) return q_err(QE_TYPE);
    int64_t ix;
    if (!idx_i64(i, &ix)) return q_err(QE_TYPE);
    if (ix < 0 || ix >= ray_len(x)) return write ? q_err(QE_INDEX) : miss_null(x);
    return q_index_elem_at(x, ix);
}

/* (i#x),item,(i+1)_x — the store for shapes the element writer can't reach
 * (sym widths, re-generalizing dict values).  Borrows c/v; owned result. */
static ray_t* splice(ray_t* c, int64_t ix, ray_t* v) {
    ray_t* n0 = ray_i64(ix);
    ray_t* left = q_take_wrap(n0, c);
    ray_release(n0);
    if (!left || RAY_IS_ERR(left)) return left ? left : q_err(QE_TYPE);
    ray_t* n1 = ray_i64(ix + 1);
    ray_t* right = q_drop_wrap(n1, c);
    ray_release(n1);
    if (!right || RAY_IS_ERR(right)) { ray_release(left); return right ? right : q_err(QE_TYPE); }
    ray_t* midv = boxed1(v);
    if (!midv || RAY_IS_ERR(midv)) {
        ray_release(left); ray_release(right);
        return midv ? midv : q_err(QE_OOM);
    }
    ray_t* lm = q_join_wrap(left, midv);
    ray_release(left); ray_release(midv);
    if (!lm || RAY_IS_ERR(lm)) { ray_release(right); return lm ? lm : q_err(QE_TYPE); }
    ray_t* r = q_join_wrap(lm, right);
    ray_release(lm); ray_release(right);
    return r ? r : q_err(QE_TYPE);
}

/* store v as item ix of a list/typed vector.  x consumed on success, the
 * caller's on error; v borrowed.  rc decides at store time (ray_list_set /
 * ray_cow mutate iff sole owner).  strict = kdb vector amend ('type on a
 * mismatched leaf, ref/amend.md errors); dict values re-generalize instead. */
static ray_t* vec_store(ray_t* x, int64_t ix, ray_t* v, int strict) {
    if (x->type == RAY_LIST) {
        ray_t* nl = ray_list_set(x, ix, v);          /* cows; consumes on ok */
        return (nl && !RAY_IS_ERR(nl)) ? nl : q_err(QE_OOM);
    }
    if (!ray_is_vec(x) || x->type == RAY_STR) return q_err(QE_TYPE);
    int fits = ray_is_atom(v) &&
               (RAY_ATOM_IS_NULL(v) || (int8_t)-v->type == x->type);
    if (!fits) {
        if (strict) return q_err(QE_TYPE);
        ray_t* r = splice(x, ix, v);
        if (r && !RAY_IS_ERR(r)) ray_release(x);
        return r;
    }
    ray_t* nx = ray_cow(x);                          /* rc==1 in place, else copy */
    if (!nx || RAY_IS_ERR(nx)) return nx ? nx : q_err(QE_OOM);
    if (q_eval_apply_store_elem(nx, ix, v) != 0) {
        ray_t* r = splice(nx, ix, v);                /* width unreachable: sym */
        if (r && !RAY_IS_ERR(r)) { ray_release(nx); return r; }
        if (nx != x) { ray_release(nx); ray_retain(x); }
        return r ? r : q_err(QE_TYPE);
    }
    if (!RAY_ATOM_IS_NULL(v)) ray_vec_set_null(nx, ix, false);
    return nx;
}

/* store at a dict key: a hit updates the value, a miss INSERTS the pair
 * (ref/amend.md).  x consumed on success; key/v borrowed. */
static ray_t* dict_store(ray_t* x, ray_t* key, ray_t* v) {
    ray_t* keys = ray_dict_slots(x)[0];
    ray_t* vals = ray_dict_slots(x)[1];
    int64_t ki = ray_dict_find_idx(x, key);
    ray_t *nk, *nv;
    if (ki >= 0) {
        ray_retain(vals);
        nv = vec_store(vals, ki, v, 0);
        if (!nv || RAY_IS_ERR(nv)) { ray_release(vals); return nv ? nv : q_err(QE_TYPE); }
        ray_retain(keys);
        nk = keys;
    } else {
        if (!ray_is_atom(key)) return q_err(QE_TYPE);
        nk = q_join_wrap(keys, key);
        if (!nk || RAY_IS_ERR(nk)) return nk ? nk : q_err(QE_TYPE);
        ray_t* ev = boxed1(v);
        if (!ev || RAY_IS_ERR(ev)) { ray_release(nk); return ev ? ev : q_err(QE_OOM); }
        nv = q_join_wrap(vals, ev);
        ray_release(ev);
        if (!nv || RAY_IS_ERR(nv)) { ray_release(nk); return nv ? nv : q_err(QE_TYPE); }
    }
    ray_t* nd = ray_dict_new(nk, nv);                /* consumes both */
    if (!nd || RAY_IS_ERR(nd)) return nd ? nd : q_err(QE_TYPE);
    ray_release(x);
    return nd;
}

/* ===== table and keyed-table levels (the axis law: q_index.h) =============
 * "Tables are indexed first by row; second by column" (basics/syntax.md) plus
 * the `` t[`age] `` column shorthand, so the index TYPE picks the axis. */

/* the amended column dict nd flipped back to a table, consuming nd and (on
 * success) t.  An error left the write's input fd ours to release. */
static ray_t* table_of_cols(ray_t* t, ray_t* fd, ray_t* nd) {
    if (!nd || RAY_IS_ERR(nd)) { ray_release(fd); return nd ? nd : q_err(QE_TYPE); }
    ray_t* r = q_flip_wrap(nd);
    ray_release(nd);
    if (!r || RAY_IS_ERR(r)) return r ? r : q_err(QE_TYPE);
    ray_release(t);
    return r;
}

static ray_t* table_level(ray_t* t, ray_t* i, int write) {
    if (i->type == -RAY_SYM) {
        ray_t* fd = q_flip_wrap(t);
        if (!fd || RAY_IS_ERR(fd)) return fd ? fd : q_err(QE_TYPE);
        ray_t* r = index_level(fd, i, 1);    /* a table has no absent column */
        ray_release(fd);
        return r;
    }
    int64_t ix;
    if (!idx_i64(i, &ix)) return q_err(QE_TYPE);
    if (write && (ix < 0 || ix >= ray_table_nrows(t))) return q_err(QE_INDEX);
    return q_index_elem_at(t, ix);           /* the item of a table is its row */
}

/* Both writes are ONE write to the column dict (a table is `flip` of it — the
 * entries-axis law): at a sym that dict's own store, so a new column
 * extends; at a row the same store run per column — `t[i]:d` is
 * `` t[key d; i]:value d ``, so a row dict addresses its OWN keys. */
static ray_t* table_store(ray_t* t, ray_t* i, ray_t* v) {
    ray_t* fd = q_flip_wrap(t);
    if (!fd || RAY_IS_ERR(fd)) return fd ? fd : q_err(QE_TYPE);
    if (i->type == -RAY_SYM) return table_of_cols(t, fd, store_level(fd, i, v));
    int64_t ix;
    if (!idx_i64(i, &ix)) { ray_release(fd); return q_err(QE_TYPE); }
    int rowdict = v && q_type_is_plain_dict(v);
    ray_t* sel = ray_dict_keys(rowdict ? v : fd);
    ray_retain(sel);                         /* outlives the dict rebuilds */
    ray_t* nd = amend_seq(fd, sel, &i, 1, NULL, rowdict ? ray_dict_vals(v) : v);
    ray_release(sel);
    return table_of_cols(t, fd, nd);
}

static ray_t* keyed_probe(ray_t* i) {
    if (ray_is_atom(i)) return ray_enlist_fn(&i, 1);
    ray_retain(i);
    return i;
}

static int64_t keyed_pos(ray_t* x, ray_t* probe) {
    ray_t* pos = q_search_find(ray_dict_slots(x)[0], probe);
    if (!pos || RAY_IS_ERR(pos)) { if (pos) ray_release(pos); return -1; }
    int64_t p = q_type_is_int_atom(pos) ? q_type_iatom_val(pos) : -1;
    ray_release(pos);
    return (p >= 0 && p < q_builtins_count_long(ray_dict_slots(x)[1])) ? p : -1;
}

static ray_t* keyed_level(ray_t* x, ray_t* i, int write) {
    if (!write) return keyed_at(x, i);
    ray_t* probe = keyed_probe(i);
    if (!probe || RAY_IS_ERR(probe)) return probe ? probe : q_err(QE_OOM);
    int64_t p = keyed_pos(x, probe);
    ray_release(probe);
    if (p < 0) return q_err(QE_INDEX);
    return q_index_elem_at(ray_dict_slots(x)[1], p);
}

/* A HIT rewrites that row of the value table; a MISS EXTENDS both halves — a
 * keyed table IS a dictionary (kb/faq.md) and "assignment has upsert
 * semantics" for one (ref/assign.md), which is dict_store's law verbatim. */
static ray_t* keyed_store(ray_t* x, ray_t* i, ray_t* v) {
    ray_t* keys = ray_dict_slots(x)[0];
    ray_t* vals = ray_dict_slots(x)[1];
    ray_t* probe = keyed_probe(i);
    if (!probe || RAY_IS_ERR(probe)) return probe ? probe : q_err(QE_OOM);
    int64_t p = keyed_pos(x, probe);
    ray_t *nk, *nv;
    if (p >= 0) {
        ray_t* pa = ray_i64(p);
        ray_retain(vals);
        nv = table_store(vals, pa, v);       /* consumes vals on success */
        ray_release(pa);
        if (!nv || RAY_IS_ERR(nv)) {
            ray_release(vals); ray_release(probe);
            return nv ? nv : q_err(QE_TYPE);
        }
        ray_retain(keys);
        nk = keys;
    } else {
        ray_t* fk = q_flip_wrap(keys);       /* the key row the probe names */
        if (!fk || RAY_IS_ERR(fk)) { ray_release(probe); return fk ? fk : q_err(QE_TYPE); }
        ray_t* krow = q_bang(ray_dict_keys(fk), probe);
        ray_release(fk);
        if (!krow || RAY_IS_ERR(krow)) { ray_release(probe); return krow ? krow : q_err(QE_TYPE); }
        nk = q_join_wrap(keys, krow);
        ray_release(krow);
        if (!nk || RAY_IS_ERR(nk)) { ray_release(probe); return nk ? nk : q_err(QE_TYPE); }
        nv = q_join_wrap(vals, v);
    }
    ray_release(probe);
    if (!nv || RAY_IS_ERR(nv)) { ray_release(nk); return nv ? nv : q_err(QE_TYPE); }
    ray_t* nd = ray_dict_new(nk, nv);        /* consumes both */
    if (!nd || RAY_IS_ERR(nd)) return nd ? nd : q_err(QE_TYPE);
    ray_release(x);
    return nd;
}

/* one atom-index WRITE step over the stores above */
static ray_t* store_level(ray_t* x, ray_t* i, ray_t* v) {
    if (q_type_is_keyed(x)) return keyed_store(x, i, v);
    if (x->type == RAY_TABLE) return table_store(x, i, v);
    if (x->type == RAY_DICT) return dict_store(x, i, v);
    int64_t ix;
    if (!idx_i64(i, &ix)) return q_err(QE_TYPE);
    if (ix < 0 || ix >= ray_len(x)) return q_err(QE_INDEX);
    return vec_store(x, ix, v, 1);
}

/* ===== the read recursion ================================================ */

static ray_t* index_r(ray_t* x, ray_t* i0, ray_t* const* rest, int64_t k);

/* one owned element continued through the rest of the path; a FUNCTION landed
 * on mid-path consumes ONE index as its argument — the ref/apply.md Index fold
 * `((d@i[0])@i[1])@i[2]` makes each step Apply At */
static ray_t* elem_rest(ray_t* e, ray_t* const* ix, int64_t k) {
    while (e && !RAY_IS_ERR(e) && k > 0 && ix[0] && q_eval_apply_is_fn(e)) {
        ray_t* a0 = ix[0];
        ray_t* r = q_eval_apply_value(e, &a0, 1);
        ray_release(e);
        e = r; ix++; k--;
    }
    if (!e || RAY_IS_ERR(e)) return e ? e : q_err(QE_TYPE);
    if (k == 0) return mat(e);
    ray_t* r = mat(index_r(e, ix[0], ix + 1, k - 1));
    ray_release(e);
    return r;
}

/* one result item per j — over the items of i (a collection index maps its
 * structure) or, when i is NULL (`::`), over the items of x themselves */
static ray_t* index_map(ray_t* x, ray_t* i, ray_t* const* rest, int64_t k) {
    ray_t* src = i ? i : x;
    int64_t n = ray_len(src);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    for (int64_t j = 0; j < n; j++) {
        ray_t* ej = q_index_elem_at(src, j);
        ray_t* r;
        if (!ej || RAY_IS_ERR(ej)) r = ej ? ej : q_err(QE_TYPE);
        else if (!i) r = elem_rest(ej, rest, k);     /* consumes ej */
        else { r = mat(index_r(x, ej, rest, k)); ray_release(ej); }
        if (!r || RAY_IS_ERR(r)) { ray_release(out); return r ? r : q_err(QE_TYPE); }
        out = ray_list_append(out, r);
        ray_release(r);
    }
    return collapse(out);
}

/* Keyed-table read.  A keyed table is a dict whose domain is a TABLE, so the
 * dict law reads through unchanged — `d[x] ~ v[k?x]`, "dictionary indexing uses
 * Find to search the keys" (basics/dictsandtables.md Indexing) — with the
 * table-domain Find doing the searching.
 *
 * kb/faq.md "Indexing a keyed table" gives the two forms and nothing else:
 * a SINGLE ROW of the key (`` ku `Tom`Lagos `` -> one dictionary, a flat list
 * carrying one element per key column) or a SUBLIST of it (a table, or a list
 * of key rows -> a table).  So a flat list is one COMPOUND key, never a run of
 * keys; `` s 2 3 `` is 'length because two elements do not make a one-column key
 * row (ref/dotq.md `.Q.ft`).  An atom is that same single row at width one.
 * Find's miss answer is the count, so an absent key gathers out of range and
 * yields the null row with no special case (`` kt `Jack`London ``). */
static ray_t* keyed_at(ray_t* x, ray_t* i) {
    ray_t* probe = keyed_probe(i);
    if (!probe || RAY_IS_ERR(probe)) return probe ? probe : q_err(QE_OOM);
    ray_t* pos = q_search_find(ray_dict_slots(x)[0], probe);
    ray_release(probe);
    if (!pos || RAY_IS_ERR(pos)) return pos ? pos : q_err(QE_TYPE);
    ray_t* r = q_index_at(ray_dict_slots(x)[1], &pos, 1);
    ray_release(pos);
    return r;
}

static ray_t* index_step(ray_t* x, ray_t* i0, ray_t* const* rest, int64_t k) {
    if (!i0 || RAY_IS_NULL(i0)) {                    /* `::`: identity / all */
        /* `x[::]` IS x for every structure (ref/identity.md; ref/apply.md:151
         * "selects the entire left argument"); an ATOM is not applicable, so it
         * keeps signalling 'type (identity.qcmd pins `3[::]`). */
        if (!is_coll(x) && x->type != RAY_DICT && x->type != RAY_TABLE)
            return q_err(QE_TYPE);
        if (k == 0) { ray_retain(x); return x; }
        if (x->type == RAY_DICT && !q_type_is_keyed(x))
            return index_map(ray_dict_slots(x)[1], NULL, rest, k);
        if (x->type == RAY_DICT || x->type == RAY_TABLE) return q_err(QE_NYI);
        return index_map(x, NULL, rest, k);
    }
    /* a TABLE domain probes by ROW whatever the range holds (`group t` gathered
     * into a plain column is still looked up by keyed_at's Find) */
    if (x->type == RAY_DICT && q_type_is_table(ray_dict_slots(x)[0]))
        return elem_rest(keyed_at(x, i0), rest, k);
    /* Index AT a dictionary: `x[d] ~ (key d)!x[value d]` (ref/fby.md prints it
     * as `dat group grp`).  Reads off the INDEX, so it outranks x's own shape. */
    if (q_type_is_plain_dict(i0)) {
        ray_t* v = index_r(x, ray_dict_slots(i0)[1], rest, k);
        if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_TYPE);
        ray_t* r = q_bang(ray_dict_slots(i0)[0], v);
        ray_release(v);
        return r;
    }
    if (x->type == RAY_TABLE) {                      /* pure delegation */
        ray_t* nx = q_table_at(x, i0);
        if (!nx) nx = ray_at_fn(x, i0);
        return elem_rest(nx, rest, k);
    }
    if (is_coll(i0)) return index_map(x, i0, rest, k);
    return elem_rest(index_level(x, i0, 0), rest, k);
}

static ray_t* index_r(ray_t* x, ray_t* i0, ray_t* const* rest, int64_t k) {
    if (!x || RAY_IS_ERR(x)) return x ? x : q_err(QE_TYPE);
    if (++g_depth > IDX_MAX_DEPTH) { g_depth--; return q_err(QE_STACK); }
    ray_t* r = index_step(x, i0, rest, k);
    g_depth--;
    return r;
}

ray_t* q_index_at(ray_t* x, ray_t* const* ix, int64_t k) {
    if (k <= 0) { ray_retain(x); return x; }
    return index_r(x, ix[0], ix + 1, k - 1);
}

/* ===== the amend recursion =============================================== */

static ray_t* amend_r(ray_t* x, ray_t* i0, ray_t* const* rest, int64_t k,
                      ray_t* f, ray_t* y);

/* new value for one selection S: f NULL replaces with y, y NULL is the
 * ternary u[S], else v[S;y] — f fully general through the apply seam */
static ray_t* leaf_apply(ray_t* f, ray_t* s, ray_t* y) {
    if (!f) { ray_retain(y); return mat(y); }
    ray_t* av[2] = { s, y };
    ray_t* r = q_eval_apply_value(f, av, y ? 2 : 1);
    return r ? mat(r) : q_err(QE_TYPE);
}

/* y against an n-item selection: an atom broadcasts, a collection pairs 1:1
 * ('length otherwise, ref/amend.md); NULL stays NULL (ternary).  Pairing reads
 * the ITERATION domain, so a table of replacement ROWS pairs like any list. */
static ray_t* conform(ray_t* y, int64_t n, int64_t j) {
    if (!y) return NULL;
    if (!q_type_is_iter(y)) { ray_retain(y); return y; }
    if (q_builtins_count_long(y) != n) return q_err(QE_LENGTH);
    return q_index_elem_at(y, j);
}

/* Amend Entire: the selection is x itself.  x consumed on success. */
static ray_t* amend_entire(ray_t* x, ray_t* f, ray_t* y) {
    ray_t* nv = leaf_apply(f, x, y);
    if (!nv || RAY_IS_ERR(nv)) return nv ? nv : q_err(QE_TYPE);
    ray_release(x);
    return nv;
}

/* leaf store at atom index i0: read S, apply, store (a dict key miss reads
 * the typed null and the store INSERTS — ref/amend.md) */
static ray_t* leaf1(ray_t* x, ray_t* i0, ray_t* f, ray_t* y) {
    ray_t* nv;
    if (f) {
        ray_t* s = index_level(x, i0, 0);
        if (!s || RAY_IS_ERR(s)) return s ? s : q_err(QE_TYPE);
        nv = leaf_apply(f, s, y);
        ray_release(s);
    } else {
        nv = leaf_apply(NULL, NULL, y);
    }
    if (!nv || RAY_IS_ERR(nv)) return nv ? nv : q_err(QE_TYPE);
    ray_t* r = store_level(x, i0, nv);
    ray_release(nv);
    return r;
}

/* sequential left-to-right accumulation over a selection (sel NULL = all
 * indices: dict keys / 0..n-1), so repeat-accumulation falls out.  x consumed
 * on success.  The retained guard keeps x alive across a mid-loop error and
 * STANDS IN for the ref a successful early step consumed — count-neutral
 * because every amend caller releases its ref on error. */
static ray_t* amend_seq(ray_t* x, ray_t* sel, ray_t* const* rest, int64_t k,
                        ray_t* f, ray_t* y) {
    ray_t* keys = (!sel && x->type == RAY_DICT) ? ray_dict_slots(x)[0] : NULL;
    if (keys) ray_retain(keys);                      /* outlives dict rebuilds */
    int64_t n = q_builtins_count_long(sel ? sel : keys ? keys : x);
    ray_retain(x);                                   /* the error-restore guard */
    ray_t* cur = x;
    ray_t* err = NULL;
    for (int64_t j = 0; j < n && !err; j++) {
        ray_t* yj = conform(y, n, j);
        if (yj && RAY_IS_ERR(yj)) { err = yj; break; }
        ray_t* kj = sel  ? q_index_elem_at(sel, j)
                  : keys ? q_index_elem_at(keys, j)
                         : ray_i64(j);
        if (!kj || RAY_IS_ERR(kj)) err = kj ? kj : q_err(QE_OOM);
        else {
            ray_t* nd = amend_r(cur, kj, rest, k, f, yj);
            ray_release(kj);
            if (RAY_IS_ERR(nd)) err = nd;
            else cur = nd;
        }
        if (yj) ray_release(yj);
    }
    if (keys) ray_release(keys);
    if (err) {
        ray_release(cur);            /* our copy chain, or the guard when cur==x */
        return err;
    }
    ray_release(x);                                  /* drop the guard */
    return cur;
}

static ray_t* amend_step(ray_t* x, ray_t* i0, ray_t* const* rest, int64_t k,
                         ray_t* f, ray_t* y) {
    if (!is_coll(x) && x->type != RAY_DICT && x->type != RAY_TABLE) return q_err(QE_TYPE);
    if (!i0 || RAY_IS_NULL(i0)) return amend_seq(x, NULL, rest, k, f, y);
    /* A collection index distributes — EXCEPT on a keyed table, where a flat one is
     * ONE compound key row, never a run of keys (#325): amend splits where reads do. */
    int keyed_row = q_type_is_keyed(x) && i0->type != RAY_TABLE && !q_type_is_nested(i0);
    if (is_coll(i0) && !keyed_row) return amend_seq(x, i0, rest, k, f, y);
    if (k == 0) return leaf1(x, i0, f, y);
    ray_t* child = index_level(x, i0, 1);            /* absent path: 'index */
    if (!child || RAY_IS_ERR(child)) return child ? child : q_err(QE_TYPE);
    ray_t* nc = amend_r(child, rest[0], rest + 1, k - 1, f, y);
    if (RAY_IS_ERR(nc)) { ray_release(child); return nc; }
    ray_t* r = store_level(x, i0, nc);
    ray_release(nc);
    return r;
}

static ray_t* amend_r(ray_t* x, ray_t* i0, ray_t* const* rest, int64_t k,
                      ray_t* f, ray_t* y) {
    if (!x || RAY_IS_ERR(x)) return q_err(QE_TYPE);
    if (++g_depth > IDX_MAX_DEPTH) { g_depth--; return q_err(QE_STACK); }
    ray_t* r = amend_step(x, i0, rest, k, f, y);
    g_depth--;
    return r;
}

/* entry guards (ref/amend.md): a sym atom d is 'domain (handles resolve at the
 * name-lift seam, never here); a non-handle ATOM d selects d itself — top
 * level only, mid-path atoms 'type */
ray_t* q_index_amend(ray_t* x, ray_t* const* ix, int64_t k, ray_t* f, ray_t* y) {
    if (!x || RAY_IS_ERR(x)) return q_err(QE_TYPE);
    if (x->type == -RAY_SYM) return q_err(QE_DOMAIN);
    if (k <= 0 || (!is_coll(x) && x->type != RAY_DICT && x->type != RAY_TABLE))
        return amend_entire(x, f, y);
    return amend_r(x, ix[0], ix + 1, k - 1, f, y);
}

ray_t* q_index_amend_at(ray_t* x, ray_t* i, ray_t* f, ray_t* y) {
    return q_index_amend(x, &i, 1, f, y);            /* @: path = enlist i */
}

ray_t* q_index_amend_dot(ray_t* x, ray_t* i, ray_t* f, ray_t* y) {
    if (!is_coll(i)) return q_err(QE_TYPE);          /* i must be a list for `.` */
    int64_t k = ray_len(i);
    if (k > IDX_MAX_DEPTH) return q_err(QE_STACK);
    ray_t* buf[16];
    ray_t** ix = k <= 16 ? buf : malloc((size_t)k * sizeof *ix);
    if (!ix) return q_err(QE_OOM);
    ray_t* r = NULL;
    int64_t got = 0;
    for (; got < k; got++) {
        ix[got] = q_index_elem_at(i, got);
        if (!ix[got] || RAY_IS_ERR(ix[got])) {
            r = ix[got] ? ix[got] : q_err(QE_TYPE);
            break;
        }
    }
    if (!r) r = q_index_amend(x, ix, k, f, y);
    for (int64_t j = 0; j < got; j++) ray_release(ix[j]);
    if (ix != buf) free(ix);
    return r;
}

ray_t* q_index_assign_wrap(ray_t* x, ray_t* y) {
    (void)x;
    ray_retain(y);
    return y;
}
