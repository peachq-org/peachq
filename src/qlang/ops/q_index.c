/* q_index — the one-code-path index/amend family (contract in q_index.h). */
#include "qlang/ops/q_index.h"
#include "qlang/ops/q_bang.h"   /* q_bang — the `!` verb, which rebuilds a selection */
#include "qlang/eval/q_eval.h"
#include "qlang/q_builtins.h"   /* q_builtins_count_long — THE count owner */
#include "qlang/base/q_err.h"
#include "qlang/q_registry_internal.h"
#include "qlang/base/q_type.h"       /* the type/shape axes: keyed, nested, iter */
#include "lang/internal.h"   /* as_i64 — the int-atom payload accessor */
#include "table/dict.h"
#include "mem/heap.h"        /* ray_cow + the vec attr bits — scatter_store, the grow bits */
#include "table/sym.h"       /* ray_read_sym/ray_write_sym — a sym id in the target's own width */
#include <stdlib.h>
#include <string.h>

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

/* The rank axis (contract: q_index.h) — both probes read items through
 * q_index_elem_at, so they live at the element-read home rather than dragging
 * it into base/. */
int q_index_is_nested(ray_t* v) {
    if (!v || ray_is_atom(v) || ray_len(v) == 0) return 0;
    ray_t* e0 = q_index_elem_at(v, 0);
    int r = e0 && !RAY_IS_ERR(e0) && (!ray_is_atom(e0) || q_type_is_str_atom(e0));
    if (e0) ray_release(e0);
    return r;
}

/* ref/join.md:192's rank, the recursive depth of the first element.  An EMPTY container reads its type-shaped miss
 * (a typed vector's null atom, a table's null row): rank is a property of TYPE, never of count; `()` stays 1 (#688). */
int q_index_rank(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return 0;
    if (q_type_is_str_atom(v)) return 1;
    if (ray_is_atom(v)) return 0;
    ray_t* c = v->type == RAY_DICT ? ray_dict_vals(v) : v;
    ray_t* e = c && c->type == RAY_LIST && ray_len(c) == 0 ? NULL : q_index_elem_at(c, 0);
    int r = 1 + q_index_rank(e);
    if (e) ray_release(e);
    return r;
}

int q_index_any_nested_item(ray_t* v) {
    if (!v || v->type != RAY_LIST) return 0;
    int64_t n = ray_len(v);
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = q_index_elem_at(v, i);
        int c = e && !RAY_IS_ERR(e) && !ray_is_atom(e) && e->type != RAY_STR;
        if (e) ray_release(e);
        if (c) return 1;
    }
    return 0;
}

/* one join operand for a single item: atoms join natively, else boxed */
static ray_t* boxed1(ray_t* v) {
    if (ray_is_atom(v)) { ray_retain(v); return v; }
    ray_t* b = ray_list_new(1);
    return ray_list_append(b, v);
}

/* miss result for one absent/OOB element of c: the null SHAPED like the first
 * element (ref/apply.md Index; dict misses ride this via find).  An atom item
 * yields its typed atom null, a LIST-valued item the EMPTY of its own type —
 * the miss of ("aa";"bb") is "" and not " ".  With no item to read a shape
 * from, the empty general list answers the empty of its OWN type, which is
 * what makes `first ()` ~ `() 0` ~ `()`. */
static ray_t* miss_null(ray_t* c) {
    if (ray_is_vec(c)) return ray_typed_null((int8_t)-c->type);
    if (c && c->type == RAY_LIST) {
        if (ray_len(c) == 0) return ray_list_new(0);
        ray_t* e0 = ((ray_t**)ray_data(c))[0];
        if (e0 && !RAY_IS_ERR(e0) && !RAY_IS_NULL(e0)) {
            if (ray_is_atom(e0)) return ray_typed_null(e0->type);
            if (ray_is_vec(e0)) return q_type_empty(e0->type);
            if (e0->type == RAY_LIST) return ray_list_new(0);
        }
    }
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* ===== the level ops ====================================================== */

static ray_t* store_level(ray_t* x, ray_t* i, ray_t* v);
static ray_t* amend_seq(ray_t* x, ray_t* sel, ray_t* const* rest, int64_t k, ray_t* f, ray_t* y, int whole,
                        const int64_t* dst);
static ray_t* table_level(ray_t* t, ray_t* i, int write);
static ray_t* table_store(ray_t* t, ray_t* i, ray_t* v);

/* ----- the dict level: Find rules the selector -----------------------------
 * "Dictionary indexing uses Find to search the keys: d[x] ~ v[k?x]" (basics/dictsandtables.md:145) and Find is
 * rank-sensitive (ref/find.md), so the SHAPE of its answer is the rank ruling on a selector — an atom: the whole
 * selector is one key; a vector: one key per item; a boxed list: nested selectors — and the indexer never splits a
 * dict's selector itself.  A keyed table is a dict whose domain is a TABLE (kb/faq.md): the same law with the
 * row-seeking Find, whose record answers are an atom and whose runs (a table, a list of rows) are a vector. */

static ray_t* keyed_probe(ray_t* i) {                /* a table domain seeks rows: an atom is its one-column row */
    if (ray_is_atom(i)) return ray_enlist_fn(&i, 1);
    ray_retain(i);
    return i;
}

/* The engine's typed scan for the commonest probe — a same-type atom on a typed key vector — where Find boxes an
 * atom per element (280ns each under ASan: `d[k]` on a 20k-key dict went from 0.3ms to 5ms).  Only a HIT is
 * trusted: the scan answers -1 for the types it does not cover (temporals), and Find owns every miss and every
 * `u#` vector, whose hash it consults.  Owned, or NULL for Find to answer. */
static ray_t* atom_pos(ray_t* x, ray_t* probe) {
    ray_t* keys = ray_dict_slots(x)[0];
    if (!ray_is_atom(probe) || !ray_is_vec(keys) || probe->type != -keys->type || (keys->attrs & RAY_ATTR_HAS_INDEX))
        return NULL;
    int64_t p = ray_dict_find_idx(x, probe);
    return p >= 0 ? ray_i64(p) : NULL;
}

static ray_t* dict_pos(ray_t* x, ray_t* probe) {     /* Find over the domain; owned */
    ray_t* keys = ray_dict_slots(x)[0];
    ray_t* pos = atom_pos(x, probe);
    if (pos) return pos;
    ray_t* p = q_type_is_table(keys) ? keyed_probe(probe) : (ray_retain(probe), probe);
    if (!p || RAY_IS_ERR(p)) return p ? p : q_err(QE_OOM);
    pos = q_search_find(keys, p);
    ray_release(p);
    return pos;
}

/* where ONE key sits — an owned i64 atom (Find's miss, the count, when absent) or Find's own error.  A LIST key
 * on a list domain is boxed so Find matches it whole — which is what keeps it one key over `()` keys, where a bare
 * list probe would split (no key to read a rank off); the mirror image of the table domain, which boxes its ATOM. */
static ray_t* key_pos(ray_t* x, ray_t* key) {
    ray_t* keys = ray_dict_slots(x)[0];
    ray_t* pos = atom_pos(x, key);
    if (pos) return pos;
    ray_t* p = ray_is_atom(key) == q_type_is_table(keys) ? ray_enlist_fn(&key, 1) : (ray_retain(key), key);
    if (!p || RAY_IS_ERR(p)) return p ? p : q_err(QE_OOM);
    pos = q_search_find(keys, p);
    ray_release(p);
    if (!pos || RAY_IS_ERR(pos)) return pos ? pos : q_err(QE_TYPE);
    int64_t n = q_builtins_count_long(keys), r = n;
    if (q_type_is_int_atom(pos)) r = q_type_iatom_val(pos);
    else if (pos->type == RAY_I64 && ray_len(pos) == 1) r = ((const int64_t*)ray_data(pos))[0];
    ray_release(pos);
    return ray_i64(r >= 0 && r < n ? r : n);
}

/* A run's positions, from the ONE Find that ruled it (a re-find per item scans the domain each time: i060's
 * 100k-key dict timed the gate out).  The sequential law survives — a key missed twice is appended by its first
 * occurrence and hit by its repeats — which Find over the run itself settles.  pos consumed. */
static ray_t* run_positions(ray_t* x, ray_t* sel, ray_t* pos) {
    int64_t n0 = q_builtins_count_long(ray_dict_slots(x)[0]), m = ray_len(pos), added = 0;
    int64_t* d = (int64_t*)ray_data(pos);
    ray_t* first = q_search_find(sel, sel);
    if (!first || RAY_IS_ERR(first) || first->type != RAY_I64 || ray_len(first) != m) {
        ray_release(pos);
        if (first && !RAY_IS_ERR(first)) { ray_release(first); first = NULL; }
        return first ? first : q_err(QE_TYPE);
    }
    const int64_t* fj = (const int64_t*)ray_data(first);
    for (int64_t j = 0; j < m; j++)
        if (d[j] >= n0) d[j] = fj[j] == j ? n0 + added++ : d[fj[j]];
    ray_release(first);
    return pos;
}

/* A step dictionary's miss: its keys carry `s (ref/apply.md:308 "keys are a sorted vector") and a key outside the
 * domain takes the value of the highest key below — `bin` exactly, down to its -1 (still a miss) below the domain —
 * for "the items of i that are outside the domain", so a nested probe steps at its atoms.  pos consumed, probe
 * borrowed; a probe bin cannot order keeps the miss it already has. */
static ray_t* step_fill(ray_t* keys, ray_t* pos, ray_t* probe) {
    int64_t n = ray_len(keys);
    if (pos->type == RAY_LIST) {
        for (int64_t j = 0, m = ray_len(pos); j < m; j++) {
            ray_t* pj = q_index_elem_at(probe, j);
            ray_t* nj = pj && !RAY_IS_ERR(pj) ? step_fill(keys, q_index_elem_at(pos, j), pj) : NULL;
            if (pj) ray_release(pj);
            if (!nj) continue;
            ray_t* nl = ray_list_set(pos, j, nj);     /* cows; consumes pos on ok */
            ray_release(nj);
            if (!nl || RAY_IS_ERR(nl)) { ray_release(pos); return nl ? nl : q_err(QE_OOM); }
            pos = nl;
        }
        return pos;
    }
    int atom = pos->type == -RAY_I64;
    if (!atom && pos->type != RAY_I64) return pos;
    int64_t m = atom ? 1 : ray_len(pos), miss = 0;
    int64_t* d = atom ? &pos->i64 : (int64_t*)ray_data(pos);
    for (int64_t j = 0; j < m && !miss; j++) miss = d[j] >= n;
    if (!miss) return pos;
    ray_t* b = q_bin_wrap(keys, probe);
    if (b && !RAY_IS_ERR(b) && atom && b->type == -RAY_I64) { ray_release(pos); return b; }
    if (b && !RAY_IS_ERR(b) && !atom && b->type == RAY_I64 && ray_len(b) == m) {
        const int64_t* bd = (const int64_t*)ray_data(b);
        for (int64_t j = 0; j < m; j++) if (d[j] >= n) d[j] = bd[j];
    }
    if (b) ray_release(b);
    return pos;
}

/* (value d)@pos, pos being Find's answer: its miss, the count, gathers OUT OF RANGE into the typed null
 * (ref/apply.md Index) — the null row for a table range (`` kt `Jack`London ``).  Consumes pos. */
static ray_t* dict_read(ray_t* x, ray_t* pos, ray_t* probe) {
    ray_t* keys = ray_dict_slots(x)[0];
    if (ray_is_vec(keys) && (keys->attrs & RAY_ATTR_SORTED)) pos = step_fill(keys, pos, probe);
    if (RAY_IS_ERR(pos)) return pos;
    ray_t* r = q_index_at(ray_dict_slots(x)[1], &pos, 1);
    ray_release(pos);
    return r;
}

/* one KEY-index READ step: dict = key_pos then dict_read; vec/list = elem or miss.  In write mode a miss/OOB is
 * 'index (a path must exist to be amended). */
static ray_t* index_level(ray_t* x, ray_t* i, int write) {
    if (x->type == RAY_DICT) {
        ray_t* pos = key_pos(x, i);
        if (RAY_IS_ERR(pos)) return pos;
        if (!write) return dict_read(x, pos, i);
        int64_t p = pos->i64;
        ray_release(pos);
        return p < q_builtins_count_long(ray_dict_slots(x)[0]) ? q_index_elem_at(ray_dict_slots(x)[1], p) : q_err(QE_INDEX);
    }
    if (x->type == RAY_TABLE) return table_level(x, i, write);
    if (!is_coll(x)) return q_err(QE_TYPE);
    int64_t ix;
    if (!idx_i64(i, &ix)) return q_err(QE_TYPE);
    if (ix < 0 || ix >= ray_len(x)) return write ? q_err(QE_INDEX) : miss_null(x);
    return q_index_elem_at(x, ix);
}

/* (i#x),item,(i+1)_x — the store for a SAME-TYPE item the element writer cannot
 * reach in place (a symbol too wide for the vector).  Borrows c/v; owned. */
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

/* Can a SIMPLE list hold v as one item?  A general list holds anything; a typed
 * vector takes an atom of its own type (a typed null passes as the sentinel).
 * THE amend-write law, shared by the vector and dict-value stores. */
static int elem_fits(ray_t* x, ray_t* v) {
    if (!x || x->type == RAY_LIST) return 1;
    if (!ray_is_vec(x) || x->type == RAY_STR) return 0;
    return ray_is_atom(v) &&
           (RAY_ATOM_IS_NULL(v) || (int8_t)-v->type == x->type);
}

/* store v as item ix of a list/typed vector.  x consumed on success, the
 * caller's on error; v borrowed.  rc decides at store time (ray_list_set /
 * ray_cow mutate iff sole owner).  A mismatched leaf is 'type — ref/assign.md
 * pins `s:1 2 3; s[1]:5f` -> 'type, and a dict's values are a simple list, so
 * they inherit it (a differing item is refused, never re-generalized: only
 * Join itself boxes, ref/join.md:33). */
static ray_t* vec_store(ray_t* x, int64_t ix, ray_t* v) {
    if (x->type == RAY_LIST) {
        ray_t* nl = ray_list_set(x, ix, v);          /* cows; consumes on ok */
        return (nl && !RAY_IS_ERR(nl)) ? nl : q_err(QE_OOM);
    }
    if (!elem_fits(x, v)) return q_err(QE_TYPE);
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

/* store v at ONE key of dict x, p from key_pos: a hit rewrites the value — a row of a table range — and a miss
 * APPENDS the pair, "assignment has upsert semantics" (ref/amend.md; ref/assign.md for a keyed table, whose new
 * key is the row the probe names).  x consumed on success; key/v borrowed. */
static ray_t* dict_put(ray_t* x, ray_t* key, int64_t p, ray_t* v) {
    ray_t* keys = ray_dict_slots(x)[0];
    ray_t* vals = ray_dict_slots(x)[1];
    int rows = vals->type == RAY_TABLE;
    ray_t *nk, *nv;
    if (p < q_builtins_count_long(keys)) {
        ray_retain(vals);
        if (rows) { ray_t* pa = ray_i64(p); nv = table_store(vals, pa, v); ray_release(pa); }
        else nv = vec_store(vals, p, v);
        if (!nv || RAY_IS_ERR(nv)) { ray_release(vals); return nv ? nv : q_err(QE_TYPE); }
        ray_retain(keys);
        nk = keys;
    } else {
        ray_t* item;
        if (q_type_is_table(keys)) {
            ray_t* fk = q_flip_wrap(keys);
            if (!fk || RAY_IS_ERR(fk)) return fk ? fk : q_err(QE_TYPE);
            ray_t* probe = keyed_probe(key);
            item = probe && !RAY_IS_ERR(probe) ? q_bang(ray_dict_keys(fk), probe) : probe;
            ray_release(fk);
            if (probe != item) ray_release(probe);
        } else {
            if (!elem_fits(vals, v)) return q_err(QE_TYPE);   /* same law on INSERT */
            item = boxed1(key);
        }
        if (!item || RAY_IS_ERR(item)) return item ? item : q_err(QE_OOM);
        nk = q_join_wrap(keys, item);
        ray_release(item);
        if (!nk || RAY_IS_ERR(nk)) return nk ? nk : q_err(QE_TYPE);
        ray_t* ev = rows ? (ray_retain(v), v) : boxed1(v);
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

/* ===== the table level (the axis law: q_index.h) ==========================
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
        ray_t* fd = q_table_to_dict(t);
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
    ray_t* fd = q_table_to_dict(t);
    if (!fd || RAY_IS_ERR(fd)) return fd ? fd : q_err(QE_TYPE);
    if (i->type == -RAY_SYM) return table_of_cols(t, fd, store_level(fd, i, v));
    int64_t ix;
    if (!idx_i64(i, &ix)) { ray_release(fd); return q_err(QE_TYPE); }
    int rowdict = v && q_type_is_plain_dict(v);
    ray_t* sel = ray_dict_keys(rowdict ? v : fd);
    ray_retain(sel);                         /* outlives the dict rebuilds */
    ray_t* nd = amend_seq(fd, sel, &i, 1, NULL, rowdict ? ray_dict_vals(v) : v, 1, NULL);
    ray_release(sel);
    return table_of_cols(t, fd, nd);
}

/* one KEY-index WRITE step: dict_put / table_store / vec_store */
static ray_t* store_level(ray_t* x, ray_t* i, ray_t* v) {
    if (x->type == RAY_DICT) {
        ray_t* pos = key_pos(x, i);
        if (RAY_IS_ERR(pos)) return pos;
        ray_t* r = dict_put(x, i, pos->i64, v);
        ray_release(pos);
        return r;
    }
    if (x->type == RAY_TABLE) return table_store(x, i, v);
    int64_t ix;
    if (!idx_i64(i, &ix)) return q_err(QE_TYPE);
    if (ix < 0 || ix >= ray_len(x)) return q_err(QE_INDEX);
    return vec_store(x, ix, v);
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
    /* terminal vector-by-i64-vector step: when every index is IN RANGE, the
     * base typed-gather owner (gather_by_idx) does the run in one pass —
     * the boxed walk below costs ~3 allocations a row, which is what made
     * 1M-row link deref lose to lj (2026-08-23 perf round).  Any miss
     * (OOB / null index -> the lane's null, the total-index law) keeps the
     * boxed walk: the kernel gathers blind and owns no miss law. */
    if (i && k == 0 && x && ray_is_vec(x) && i->type == RAY_I64) {
        int64_t xn = ray_len(x), n = ray_len(i), j = 0;
        const int64_t* ix = (const int64_t*)ray_data(i);
        while (j < n && ix[j] >= 0 && ix[j] < xn) j++;
        if (j == n && n > 0) {
            ray_t* r = gather_by_idx(x, (int64_t*)ix, n);
            if (r && !RAY_IS_ERR(r)) return r;
            if (r) ray_release(r);              /* OOM etc: the boxed walk answers */
        }
    }
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
    /* at the terminal step the items ARE x's, so an empty gather keeps x's
     * element type (`2 4 4 9 _ til 10` cuts to `` `long$() ``, ref/cut.md) */
    return k == 0 ? q_typed_empty_like(collapse(out), x) : collapse(out);
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
    /* Index AT a dictionary: `x[d] ~ (key d)!x[value d]` (ref/fby.md prints it as `dat group grp`).  Reads off the
     * INDEX, so it outranks x's own shape — except on a TABLE domain, where the dict IS a key row (kb/faq.md). */
    if (q_type_is_plain_dict(i0) && !(x->type == RAY_DICT && q_type_is_table(ray_dict_slots(x)[0]))) {
        ray_t* v = index_r(x, ray_dict_slots(i0)[1], rest, k);
        if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_TYPE);
        ray_t* r = q_bang(ray_dict_slots(i0)[0], v);
        ray_release(v);
        return r;
    }
    if (x->type == RAY_DICT) {                       /* `d[x] ~ v[k?x]`, whatever shape x has */
        ray_t* pos = dict_pos(x, i0);
        if (!pos || RAY_IS_ERR(pos)) return pos ? pos : q_err(QE_TYPE);
        return elem_rest(dict_read(x, pos, i0), rest, k);
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
    /* indexing PRESERVES the enum (`e 0` -> -20h atom): index the positions,
     * re-stamp the domain — the structural preserve set's index arm. */
    if (x && x->type == RAY_ENUM) {
        ray_t* p = q_enum_positions(x);
        if (!p || RAY_IS_ERR(p)) return p ? p : q_err(QE_OOM);
        ray_t* r = q_index_at(p, ix, k);
        ray_release(p);
        return q_enum_stamp(r, q_enum_domain(x));
    }
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

/* amend_seq's plain-replace leaf with every fallible step hoisted ahead of the
 * first write, so the in-place path needs no error-restore guard; COW still
 * decides at store time, so a shared target (value form, alias) copies once
 * exactly as before.  NULL = shape not covered (caller loops); errors leave
 * x to the caller; x consumed on success. */
static ray_t* scatter_store(ray_t* x, ray_t* sel, ray_t* y) {
    if (!ray_is_vec(x) || x->type == RAY_SYM || x->type == RAY_STR) return NULL;
    if (x->attrs & (RAY_ATTR_SLICE | RAY_ATTR_ARENA | RAY_ATTR_HAS_INDEX)) return NULL;
    if (sel->type != RAY_I64 || (sel->attrs & RAY_ATTR_SLICE)) return NULL;
    size_t esz = ray_type_sizes[(uint8_t)x->type];
    if (!esz) return NULL;
    int bc = ray_is_atom(y);
    if (bc) {
        if (!q_eval_apply_store_elem_ok(x->type)) return NULL;
        if (!RAY_ATOM_IS_NULL(y) && (int8_t)-y->type != x->type) return NULL;
    } else {
        if (y->type != x->type || (y->attrs & RAY_ATTR_SLICE)) return NULL;
    }
    int64_t n = ray_len(sel), xn = ray_len(x);
    if (n == 0) return x;                       /* conformability held by amend_seq */
    if (sel == x || y == x) return NULL;        /* self-referential: keep the loop's snapshot */
    const int64_t* ix = (const int64_t*)ray_data(sel);
    for (int64_t j = 0; j < n; j++)
        if (ix[j] < 0 || ix[j] >= xn) return q_err(QE_INDEX);
    ray_t* nx = ray_cow(x);
    if (!nx || RAY_IS_ERR(nx)) return nx ? nx : q_err(QE_OOM);
    if (bc) {
        for (int64_t j = 0; j < n; j++) q_eval_apply_store_elem(nx, ix[j], y);
        return nx;
    }
    uint8_t* dst = (uint8_t*)ray_data(nx);
    const uint8_t* src = (const uint8_t*)ray_data(y);
    for (int64_t j = 0; j < n; j++)
        memcpy(dst + (size_t)ix[j] * esz, src + (size_t)j * esz, esz);
    /* the loop marks nulls per written ATOM (payload truth — validate.c law:
     * a reserved sentinel needs HAS_NULLS); probe the written lanes with the
     * gate open and keep the attr exactly when the loop would have set it */
    uint8_t had = nx->attrs & RAY_ATTR_HAS_NULLS;
    nx->attrs |= RAY_ATTR_HAS_NULLS;
    int any = 0;
    for (int64_t j = 0; j < n && !any; j++) any = ray_vec_is_null(nx, ix[j]);
    if (!any && !had) nx->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    return nx;
}

/* Amend Entire: the selection is x itself.  x consumed on success. */
static ray_t* amend_entire(ray_t* x, ray_t* f, ray_t* y) {
    ray_t* nv = leaf_apply(f, x, y);
    if (!nv || RAY_IS_ERR(nv)) return nv ? nv : q_err(QE_TYPE);
    ray_release(x);
    return nv;
}

/* leaf store at ONE index i0: read S, apply, store (a dict key miss reads the typed null and the store INSERTS —
 * ref/amend.md).  p >= 0 is a dict key's position the run's Find already settled; -1 looks it up here. */
static ray_t* leaf1(ray_t* x, ray_t* i0, int64_t p, ray_t* f, ray_t* y) {
    ray_t* nv;
    if (f) {
        ray_t* s = p >= 0 ? dict_read(x, ray_i64(p), i0) : index_level(x, i0, 0);
        if (!s || RAY_IS_ERR(s)) return s ? s : q_err(QE_TYPE);
        nv = leaf_apply(f, s, y);
        ray_release(s);
    } else {
        nv = leaf_apply(NULL, NULL, y);
    }
    if (!nv || RAY_IS_ERR(nv)) return nv ? nv : q_err(QE_TYPE);
    ray_t* r = p >= 0 ? dict_put(x, i0, p, nv) : store_level(x, i0, nv);
    ray_release(nv);
    return r;
}

/* A completed level owns the homogeneity invariant (owner ruling 2026-09-14: amends and removals collapse, appends
 * never do): a general list — or a dict's value list — that has become same-typed atoms is the typed vector.  Once
 * per level, when its whole amend is done: per item would type the second item of `x[0 1]:(`b;2)`.  Consumes x. */
static ray_t* level_collapse(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    ray_t* v = x->type == RAY_LIST ? x : x->type == RAY_DICT ? ray_dict_slots(x)[1] : NULL;
    if (!v || v->type != RAY_LIST) return x;
    ray_t* cv = q_list_collapse(v);
    if (!cv || RAY_IS_ERR(cv) || cv == v) {          /* the amend stood; uncollapsed is a legal value */
        if (cv && RAY_IS_ERR(cv)) ray_error_free(cv); else if (cv) ray_release(cv);
        return x;
    }
    ray_t* nd = cv;
    if (v != x) {
        ray_retain(ray_dict_slots(x)[0]);
        nd = ray_dict_new(ray_dict_slots(x)[0], cv);     /* consumes both, on failure too */
        if (!nd || RAY_IS_ERR(nd)) { if (nd) ray_error_free(nd); return x; }
    }
    ray_release(x);
    return nd;
}

/* The slot x's child at atom index i0 sits in, when the whole level is ours to write: x at rc 1 and the child at rc 1
 * in a slot array only x holds (a list's own slots, a dict's value list, a table's column list).  A shared level
 * anywhere answers NULL and the path-copying walk COWs it there, as ref/amend.md's other-references law says. */
static ray_t** own_slot(ray_t* x, ray_t* i0) {
    if (!x || !i0 || x->rc != 1 || (x->attrs & (RAY_ATTR_ARENA | RAY_ATTR_SLICE))) return NULL;
    ray_t* l = x;
    int64_t p = -1;
    if (x->type == RAY_DICT || x->type == RAY_TABLE) {
        l = ray_dict_slots(x)[1];
        if (!l || l->type != RAY_LIST || l->rc != 1 || (l->attrs & (RAY_ATTR_ARENA | RAY_ATTR_SLICE))) return NULL;
        if (x->type == RAY_TABLE) {
            if (i0->type != -RAY_SYM) return NULL;
            for (int64_t c = 0, n = ray_table_ncols(x); c < n && p < 0; c++)
                if (ray_table_col_name(x, c) == i0->i64) p = c;
        } else {
            ray_t* pos = key_pos(x, i0);
            if (!pos || RAY_IS_ERR(pos)) { if (pos) ray_release(pos); return NULL; }
            p = pos->i64;
            ray_release(pos);
        }
    } else if (x->type != RAY_LIST || !idx_i64(i0, &p)) return NULL;
    if (p < 0 || p >= ray_len(l)) return NULL;
    ray_t** slot = (ray_t**)ray_data(l) + p;
    ray_t* c = *slot;
    return c && !RAY_IS_ERR(c) && c->rc == 1 && !(c->attrs & (RAY_ATTR_ARENA | RAY_ATTR_SLICE)) ? slot : NULL;
}

/* one index item at this level: the leaf, or the path continued through the child it names.  An exclusive path
 * lifts the child OUT of its slot (the slot's ref becomes the walk's, so rc 1 reaches the store and it writes where
 * it stands) and puts the amended child back; a table names its column at either depth (the axis law). */
static ray_t* amend_one(ray_t* x, ray_t* i0, ray_t* const* rest, int64_t k, ray_t* f, ray_t* y) {
    if (k == 0) {
        ray_t** slot = y && f && q_registry_row_of(f, Q_DYADIC) == q_ops_find(",", 1) ? own_slot(x, i0) : NULL;
        if (!slot) return leaf1(x, i0, -1, f, y);
        ray_t* child = *slot;                        /* `x[i],:y` grows the child where it stands */
        *slot = NULL;
        ray_t* r = q_join_grow(&child, y);
        *slot = RAY_IS_ERR(r) ? child : r;
        return RAY_IS_ERR(r) ? r : x;
    }
    ray_t* ci = rest[0];
    ray_t** slot = own_slot(x, i0);
    if (!slot && x->type == RAY_TABLE && ci && ci->type == -RAY_SYM && (slot = own_slot(x, ci)) != NULL) ci = i0;
    if (slot) {
        ray_t* child = *slot;
        *slot = NULL;
        ray_t* nc = amend_r(child, ci, rest + 1, k - 1, f, y);
        if (RAY_IS_ERR(nc)) { *slot = child; return nc; }
        *slot = level_collapse(nc);
        return x;
    }
    ray_t* child = index_level(x, i0, 1);            /* absent path: 'index */
    if (!child || RAY_IS_ERR(child)) return child ? child : q_err(QE_TYPE);
    ray_t* nc = level_collapse(amend_r(child, rest[0], rest + 1, k - 1, f, y));
    if (RAY_IS_ERR(nc)) { ray_release(child); return nc; }
    ray_t* r = store_level(x, i0, nc);
    ray_release(nc);
    return r;
}

/* sequential left-to-right accumulation over a selection (sel NULL = all
 * indices: dict keys / 0..n-1), so repeat-accumulation falls out.  x consumed
 * on success.  The retained guard keeps x alive across a mid-loop error and
 * STANDS IN for the ref a successful early step consumed — count-neutral
 * because every amend caller releases its ref on error.  whole: the items ARE
 * the indices (a dict's keys, a run Find ruled) and never distribute again;
 * dst: a dict run's settled positions, one per item (run_positions). */
static ray_t* amend_seq(ray_t* x, ray_t* sel, ray_t* const* rest, int64_t k,
                        ray_t* f, ray_t* y, int whole, const int64_t* dst) {
    ray_t* keys = (!sel && x->type == RAY_DICT) ? ray_dict_slots(x)[0] : NULL;
    if (keys) ray_retain(keys);                      /* outlives dict rebuilds */
    int64_t n = q_builtins_count_long(sel ? sel : keys ? keys : x);
    /* THE length law, once per level (ref/amend.md errors; conformable.md: an
     * atom conforms to everything, lists only at equal counts — so it must
     * fire for n==0 too, which a per-iteration check never reaches).  Pairing
     * reads the ITERATION domain, so a table of replacement ROWS pairs like
     * any list.  Before the guard: the caller's on-error release stays right. */
    if (y && q_type_is_iter(y) && q_builtins_count_long(y) != n) {
        if (keys) ray_release(keys);
        return q_err(QE_LENGTH);
    }
    if (sel && k == 0 && y &&
        (!f || q_registry_row_of(f, Q_DYADIC) == q_ops_find(":", 1))) {
        ray_t* r = scatter_store(x, sel, y);   /* registry `:` IS plain replace */
        if (r) return r;
    }
    ray_retain(x);                                   /* the error-restore guard */
    ray_t* cur = x;
    ray_t* err = NULL;
    for (int64_t j = 0; j < n && !err; j++) {
        ray_t* yj = !y ? NULL                        /* ternary: no replacement */
                  : !q_type_is_iter(y) ? (ray_retain(y), y)
                  : q_index_elem_at(y, j);
        if (yj && RAY_IS_ERR(yj)) { err = yj; break; }
        ray_t* kj = sel  ? q_index_elem_at(sel, j)
                  : keys ? q_index_elem_at(keys, j)
                         : ray_i64(j);
        if (!kj || RAY_IS_ERR(kj)) err = kj ? kj : q_err(QE_OOM);
        else {
            ray_t* nd = dst ? leaf1(cur, kj, dst[j], f, yj)
                      : whole ? amend_one(cur, kj, rest, k, f, yj) : amend_r(cur, kj, rest, k, f, yj);
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
    if (!i0 || RAY_IS_NULL(i0)) return amend_seq(x, NULL, rest, k, f, y, 1, NULL);
    if (x->type == RAY_DICT && !ray_is_atom(i0)) {   /* Find's ruling: one key, a run of keys, nested selectors */
        ray_t* pos = dict_pos(x, i0);
        if (!pos || RAY_IS_ERR(pos)) return pos ? pos : q_err(QE_TYPE);
        int rank = pos->type == -RAY_I64 ? 0 : pos->type == RAY_I64 ? 1 : pos->type == RAY_LIST ? 2 : -1;
        if (rank == 1 && k == 0) {                   /* the run's leaf takes its positions from this one Find */
            pos = run_positions(x, i0, pos);
            if (RAY_IS_ERR(pos)) return pos;
            ray_t* r = amend_seq(x, i0, rest, k, f, y, 1, (const int64_t*)ray_data(pos));
            ray_release(pos);
            return r;
        }
        ray_release(pos);
        if (rank < 0) return q_err(QE_TYPE);
        return rank ? amend_seq(x, i0, rest, k, f, y, rank == 1, NULL) : amend_one(x, i0, rest, k, f, y);
    }
    if (is_coll(i0)) return amend_seq(x, i0, rest, k, f, y, 0, NULL);   /* a collection index distributes */
    return amend_one(x, i0, rest, k, f, y);
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
    /* enum target: index-assign enforces the domain — an in-domain sym coerces
     * to its position, out-of-domain is 'cast (owner training `` el[0]:`oo ``);
     * only plain `:` replacement is defined on 20h this phase. */
    if (x->type == RAY_ENUM) {
        if (f && q_registry_row_of(f, Q_DYADIC) != q_ops_find(":", 1))
            return q_err(QE_TYPE);           /* NULL f IS plain replace */
        ray_t* ny = q_enum_coerce(q_enum_domain(x), y);
        if (!ny || RAY_IS_ERR(ny)) return ny ? ny : q_err(QE_TYPE);
        ray_t* p = q_enum_positions(x);
        if (!p || RAY_IS_ERR(p)) { ray_release(ny); return p ? p : q_err(QE_OOM); }
        ray_t* r = q_index_amend(p, ix, k, f, ny);
        ray_release(ny);
        if (!r || RAY_IS_ERR(r)) { ray_release(p); return r ? r : q_err(QE_TYPE); }
        r = q_enum_stamp(r, q_enum_domain(x));
        if (r && !RAY_IS_ERR(r)) ray_release(x);         /* consumed on success */
        return r;
    }
    if (k <= 0 || (!is_coll(x) && x->type != RAY_DICT && x->type != RAY_TABLE))
        return amend_entire(x, f, y);
    return level_collapse(amend_r(x, ix[0], ix + 1, k - 1, f, y));
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

/* Amend Entire with `,` on a vector the writer owns outright: the cells go in
 * through ray_vec_append, inside the slack the buddy block already holds, where
 * base concat allocates na+nb and copies.  What base concat does and append
 * does NOT is why the guard exists: a sym id is translated to the column's
 * domain and widened (append memcpy's esz bytes — so the domain must match and
 * every id fit); HAS_NULLS is derived from the result (so it is OR'd in here);
 * a STR column merges pools; an index-backed u#/g#/p# letter is dropped by the
 * first write and re-stamped by an allocation, which a later failure could
 * not undo, so only `s` (re-derived by the retention law) is admitted. */
int q_index_growable(ray_t* x, ray_t* y) {
    if (!x || !y || !ray_is_vec(x) || x->type == RAY_STR || y->type != x->type || x->mmod ||
        (x->attrs & (RAY_ATTR_SLICE | RAY_ATTR_ARENA | RAY_ATTR_HAS_LINK)) || (y->attrs & RAY_ATTR_SLICE))
        return 0;
    char lx = q_attr_letter(x);
    if (lx && lx != 's') return 0;
    if (x->type == RAY_SYM) {
        if (ray_sym_vec_domain(x) != ray_sym_vec_domain(y)) return 0;
        for (int64_t i = 0, n = ray_len(y); i < n; i++)
            if (ray_sym_dict_width(ray_read_sym(ray_data(y), i, RAY_SYM, y->attrs) + 1) > (x->attrs & RAY_SYM_W_MASK))
                return 0;
    }
    return 1;
}

ray_t* q_index_grow(ray_t** px, ray_t* y) {
    ray_t* x = *px;
    int64_t nx = ray_len(x);
    char lx = q_attr_letter(x);
    uint8_t esz = ray_sym_elem_size(y->type, y->attrs), xesz = ray_sym_elem_size(x->type, x->attrs);
    int nulls = 0;
    x->attrs &= (uint8_t)~RAY_ATTR_SORTED;               /* append keeps attrs; the law re-derives s */
    for (int64_t i = 0, n = ray_len(y); i < n; i++) {
        const void* elem = (const char*)ray_data(y) + i * esz;
        int64_t id = 0;
        if (esz != xesz) {
            ray_write_sym(&id, 0, (uint64_t)ray_read_sym(ray_data(y), i, RAY_SYM, y->attrs), RAY_SYM, x->attrs);
            elem = &id;
        }
        nulls |= ray_vec_is_null(y, i);
        ray_t* nv = ray_vec_append(x, elem);
        if (!nv || RAY_IS_ERR(nv)) { *px = x; return nv ? nv : q_err(QE_OOM); }
        x = nv;
    }
    if (nulls) x->attrs |= RAY_ATTR_HAS_NULLS;
    *px = q_attr_append_keep(lx, nx, x);
    return NULL;
}

void q_index_ungrow(ray_t* x, int64_t nx, uint8_t was) {
    x->len = nx;
    x->attrs = (x->attrs & (uint8_t)~(RAY_ATTR_SORTED | RAY_ATTR_HAS_NULLS)) | (was & (RAY_ATTR_SORTED | RAY_ATTR_HAS_NULLS));
}
