/* ops/q_search.c — the SEARCH family: query a value against a REFERENCE and
 * report where it stands, without building a new collection.  `within` (in
 * the bounds?), `in` (a member?), `?` find (at which position?), `bin`/`binr`
 * (where does it fall in a sorted domain?).  Set algebra
 * (except/inter/union/distinct) is deliberately absent: it BUILDS
 * collections, so it is a different concept.
 *
 * `?` is multi-concept, so only the find ARM lives here; the glyph's
 * roll/deal/generate classifier stays in ops/q_rand.c and routes find shapes
 * to q_search_find.
 *
 * Assembled 2026-07-24 from ops/q_math.c (within) and ops/q_list.c (in, find)
 * — pure moves except within, rewritten onto the comparison primitives.
 * bin/binr were written here 2026-07-25; they had no q-layer body before. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"  /* the type-axis home: the shape predicates and the int-lane reads */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_value — within composes on `>=`/`<=`/`&` */
#include "qlang/ops/q_index.h"  /* q_index_elem_at — THE element-read home */
#include "lang/eval.h"     /* ray_in_fn, ray_find_fn */
#include "lang/internal.h" /* atom_eq */
#include "mem/heap.h"      /* RAY_ATTR_HAS_NULLS — ? find miss remap */
#include <stdint.h>
#include <stdlib.h>

/* ===== q `x within y` — inclusive bounds =================================== */

/* `x >= y[k]` / `x <= y[k]` — the bound at k against the whole of x, taken
 * through the apply seam so the atomic lift and every shape law come with it
 * and this file owns no type knowledge. */
static ray_t* bound_cmp(ray_t* x, ray_t* y, int64_t k, const char* op) {
    ray_t* b = q_index_elem_at(y, k);
    if (!b || RAY_IS_ERR(b)) return b ? b : q_err(QE_TYPE);
    ray_t* f = q_registry_lookup_name(op, 2, Q_DYADIC);       /* borrowed */
    ray_t* av[2] = { x, b };
    ray_t* r = f ? q_eval_apply_value(f, av, 2) : NULL;
    ray_release(b);
    return r ? r : q_err(QE_TYPE);
}

/* ref/within.md: y is an ordered pair, or the flip of a list of ordered pairs.
 * BOTH forms are exactly `(x >= y 0) & (x <= y 1)`, so within owns no type
 * knowledge — chars, syms, temporals, nesting and the pair-flip all arrive via
 * the atomic lift on the comparison verbs.  y is BOUNDS, not an operand that
 * conforms to x, so the row is family `none` and this receives whole args. */
ray_t* q_within_wrap(ray_t* x, ray_t* y) {
    if (!x || !y || ray_is_atom(y)) return q_err(QE_TYPE);
    if (ray_len(y) != 2) return q_err(QE_LENGTH);
    ray_t* ge = bound_cmp(x, y, 0, ">=");
    if (RAY_IS_ERR(ge)) return ge;
    ray_t* le = bound_cmp(x, y, 1, "<=");
    if (RAY_IS_ERR(le)) { ray_release(ge); return le; }
    ray_t* f = q_registry_lookup_name("&", 1, Q_DYADIC);      /* borrowed */
    ray_t* av[2] = { ge, le };
    ray_t* r = f ? q_eval_apply_value(f, av, 2) : NULL;
    ray_release(ge);
    ray_release(le);
    return r ? r : q_err(QE_TYPE);
}

/* ===== q `x in y` — membership ============================================= */

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
    /* a TABLE domain is membership over ROWS, which is exactly "did the row
     * search find it": find answers a miss with `count y`, so the flag is
     * `(y?x) < count y` and the record-vs-run shape comes back with it. */
    if (q_type_is_table(y)) {
        ray_t* i = q_search_find(y, x);
        if (!i || RAY_IS_ERR(i)) return i ? i : q_err(QE_TYPE);
        ray_t* n = ray_i64(ray_table_nrows(y));
        ray_t* f = q_registry_lookup_name("<", 1, Q_DYADIC);  /* borrowed */
        ray_t* av[2] = { i, n };
        ray_t* r = f ? q_eval_apply_value(f, av, 2) : NULL;
        ray_release(i);
        ray_release(n);
        return r ? r : q_err(QE_TYPE);
    }
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
    /* STR-vector y (peachq list-of-strings): whole-item membership -> atom */
    if (y->type == RAY_STR && x->type == -RAY_STR)
        return ray_bool(seq_has_item(y, x) != 0);
    /* mixed numeric families (ref/in.md Mixed argument types): allowed only
     * against an ATOM or 1-item y — elementwise numeric equality (q_velem_f
     * reads both families; nulls never match). */
    if (q_type_is_num_tag(x->type) && q_type_is_num_tag(y->type)) {
        int xf = q_type_is_float_tag(x->type), yf = q_type_is_float_tag(y->type);
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
    /* a typed domain of two or more items is Find's (ref/find.md:102 "Find is implicit in ... in"), so its type
     * law rules here too; the atom and the 1-item y above keep in.md's "wider input type mix" */
    if (ray_is_vec(y) && y->type != RAY_STR && ray_len(y) != 1 && !q_search_admits(y, x)) return q_err(QE_TYPE);
    /* Against a non-list y the comparison is left-atomic (ref/in.md) — one
     * boolean per item of x, and NONE is still boolean, where the base kernel
     * answers the untyped `()` that no downstream `where` survives.  A STR y
     * is excluded: it is a LIST of strings, seeking whole-x above. */
    if (y->type != RAY_STR && !ray_is_atom(x) && ray_len(x) == 0)
        return q_type_empty(RAY_BOOL);
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

/* ===== row-wise search =====================================================
 * A table row is searched IN PLACE: the row kernel (ray_find_rows_fn) walks the domain comparing key columns cell
 * by cell under Find's own compare law, so `0N~0N` and the int/long widening are inherited and nothing proportional
 * to the domain is built per call.  The q layer owns the probe's SHAPE (probe_cols) and the type gate. */

/* A column as the row kernel reads it: an atom is its one-row column, an enumeration its values (q_enum owns the
 * 20h representation; the kernel knows base types only).  Consumes c; owned. */
static ray_t* row_col(ray_t* c) {
    if (c && ray_is_atom(c)) { ray_t* v = ray_enlist_fn(&c, 1); ray_release(c); c = v; }
    if (c && !RAY_IS_ERR(c) && q_enum_is(c)) { ray_t* v = q_enum_val_image(c); ray_release(c); c = v; }
    return c ? c : q_err(QE_OOM);
}

/* The domain's first k columns as the kernel reads them, each admitted against its probe column under Find's type
 * law (ref/find.md:42; owner ruling 2026-09-15: a foreign-typed cell is 'type, int and long widen).  Owned dc[0..k);
 * nonzero on a fault. */
static int row_domain(ray_t* x, ray_t** dc, ray_t* const* pc, int64_t k) {
    int bad = 0;
    for (int64_t j = 0; j < k; j++) {
        ray_t* c = ray_table_get_col_idx(x, j);
        ray_retain(c);
        dc[j] = row_col(c);
        if (RAY_IS_ERR(dc[j]) || (ray_is_vec(dc[j]) && dc[j]->type != RAY_STR && !q_search_admits(dc[j], pc[j]))) bad = 1;
    }
    return bad;
}

/* Find's miss is `count x` (find.md) where the kernel answers 0N: the atom, and every item of a run.  Consumes i. */
static ray_t* miss_is_count(ray_t* i, int64_t cnt) {
    if (!i || RAY_IS_ERR(i)) return i;
    if (ray_is_atom(i) && i->type == -RAY_I64 && RAY_ATOM_IS_NULL(i)) { ray_release(i); return ray_i64(cnt); }
    if (i->type == RAY_I64) {
        int64_t* d = (int64_t*)ray_data(i);          /* fresh rc=1 from find */
        for (int64_t j = 0, n = ray_len(i); j < n; j++) if (d[j] == NULL_I64) d[j] = cnt;
        i->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    }
    return i;
}

/* find.md's rank law one level up: a boxed list whose ITEMS are rows is a RUN
 * of records, not one record.  The DOMAIN settles the ambiguity — a collection
 * item can only be a FIELD where that column itself holds collections (a string
 * column), so a simple leading column means the probe is a list of rows.  `flip`
 * then hands it to the positional column path unchanged. */
static int probe_is_rowlist(ray_t* dom, ray_t* y) {
    if (!y || y->type != RAY_LIST || ray_len(y) == 0) return 0;
    ray_t* y0 = ((ray_t**)ray_data(y))[0];
    if (!y0 || ray_is_atom(y0)) return 0;
    return !q_index_is_nested(ray_table_get_col_idx(dom, 0));
}

/* probe_cols' fault code as the q error it means */
static ray_t* probe_err(int bad) { return q_err(bad == 2 ? QE_LENGTH : QE_TYPE); }

/* Split a probe into one value per DOMAIN column.  A table probe FLIPS to its
 * column dict, so a table and a dict record share ONE path: named entries are
 * matched to the domain's own column names through find — order-free, and a
 * name the probe lacks lands out of range and refuses.  A list is positional.
 * *m gets the probe's row count and *rec whether it is a single RECORD, whose
 * answer is an atom rather than a run (find.md "a compatible record
 * (dictionary or list) or table").  Owned pc[0..k), each as the kernel reads it (row_col); nothing owned on a
 * shape mismatch, reported as 1 for a TYPE fault and 2 for an ARITY one. */
static int probe_cols(ray_t* x, ray_t* y, ray_t** pc, int64_t k, int64_t* m, int* rec) {
    if (ray_is_atom(y)) return -1;               /* find.md: a rank-2 x seeks rank-1 records */
    int rows = probe_is_rowlist(x, y);
    ray_t* fy = q_type_is_table(y) ? q_table_to_dict(y) : rows ? q_flip_wrap(y) : NULL;   /* owned */
    ray_t* d = fy ? fy : y;
    ray_t* pn = q_type_is_plain_dict(d) ? ray_dict_keys(d) : NULL;  /* borrowed */
    ray_t* pv = pn ? ray_dict_vals(d) : d;
    ray_t* fx = pn ? q_table_to_dict(x) : NULL;                     /* owned */
    ray_t* pos = fx && q_type_is_plain_dict(fx)
                     ? q_search_find(pn, ray_dict_keys(fx)) : NULL;
    if (fx) ray_release(fx);
    int bad = ((pn && (!pos || RAY_IS_ERR(pos))) || !pv) ? 1 : 0;
    /* a POSITIONAL record carries one field per domain column, so a count that
     * disagrees is an arity fault, not a type one: `.Q.ft` (ref/dotq.md) pins
     * `s 2 3` — two elements against one key column — as 'length. */
    if (!bad && ray_len(pv) != k) bad = pn ? 1 : 2;
    int64_t got = 0;
    for (; got < k && !bad; got++) {
        int64_t at = got;
        if (pos) {
            ray_t* e = q_index_elem_at(pos, got);
            at = e && q_type_is_int_atom(e) ? q_type_iatom_val(e) : -1;
            if (e) ray_release(e);
        }
        ray_t* v = at >= 0 && at < k ? q_index_elem_at(pv, at) : NULL;
        if (!v || RAY_IS_ERR(v)) { if (v) ray_release(v); bad = 1; break; }
        pc[got] = v;
    }
    *rec = 1;
    *m = 1;
    for (int64_t j = 0; j < k && !bad; j++) {
        if (ray_is_atom(pc[j])) continue;            /* an atom column broadcasts */
        int64_t l = ray_len(pc[j]);
        if (!*rec && l != *m) bad = 2;                /* rows of unequal width */
        else { *rec = 0; *m = l; }
    }
    for (int64_t j = 0; j < k && !bad; j++) bad = RAY_IS_ERR(pc[j] = row_col(pc[j]));
    if (bad)
        for (int64_t j = 0; j < got; j++) ray_release(pc[j]);
    if (fy) ray_release(fy);
    if (pos) ray_release(pos);
    return bad;
}

/* The shape law of a row search: a RECORD probe answers with an atom, a run of
 * rows with the index vector itself.  Consumes r. */
static ray_t* row_answer(ray_t* r, int rec) {
    if (!rec || !r || RAY_IS_ERR(r) || ray_is_atom(r)) return r;
    int64_t v = ((const int64_t*)ray_data(r))[0];
    ray_release(r);
    return ray_i64(v);
}

/* `t ? row` — the smallest row index of table x matching the probe (find.md Searching tables); a miss is
 * `count x`. */
static ray_t* find_rows(ray_t* x, ray_t* y) {
    int64_t k = ray_table_ncols(x), n = ray_table_nrows(x);
    if (k <= 0 || !y) return q_err(QE_TYPE);
    /* a one-column table Finds as its column (owner 2026-09-16): an atom is its one-field record */
    if (k == 1 && ray_is_atom(y)) {
        ray_t* rec = ray_enlist_fn(&y, 1);
        if (!rec || RAY_IS_ERR(rec)) return rec ? rec : q_err(QE_OOM);
        ray_t* r = find_rows(x, rec);
        ray_release(rec);
        return r;
    }
    ray_t** pc = (ray_t**)calloc((size_t)k * 2, sizeof *pc);
    if (!pc) return q_err(QE_TYPE);
    ray_t** dc = pc + k;
    int64_t m;
    int rec;
    int bad = probe_cols(x, y, pc, k, &m, &rec);
    if (bad) { free(pc); return probe_err(bad); }
    bad = row_domain(x, dc, pc, k);
    ray_t* r = bad ? q_err(QE_TYPE) : ray_find_rows_fn(dc, pc, k, m);
    for (int64_t j = 0; j < 2 * k; j++) ray_release(pc[j]);
    free(pc);
    return row_answer(miss_is_count(r, n), rec);
}

/* ===== q `?` find ==========================================================
 * list ? y -> find.  kdb miss semantics: the smallest index NOT in the list,
 * i.e. `count x` — rayfall find returns 0N on a miss (atom result) or
 * per-element 0N (vector needle), so both shapes are remapped to count here.
 * The dict reverse lookup keys[vals?y] composes on find, so it lives here
 * WITH find.  The roll/deal/generate arms of `?` live in ops/q_rand.c
 * (q_roll_wrap, the `?` shape dispatcher — it routes find shapes here). */

/* A dict search REPORTS KEYS: read the key domain at the index (or indices)
 * the value search landed on.  The index home null-fills an out-of-range index
 * — find's miss (count) and bin's miss (-1/0N) all become the key null that way
 * — but the mixed nulls come back BOXED, so collapse to the key type.  A KEYED
 * table's keys are a TABLE, which only that home reads.  Consumes i, borrows
 * keys. */
static ray_t* keys_at(ray_t* keys, ray_t* i) {
    ray_t* r = q_index_at(keys, &i, 1);
    ray_release(i);
    if (!r || r->type != RAY_LIST) return r;
    ray_t* c = q_typed_empty_like(q_list_collapse(r), keys);
    ray_release(r);
    return c;
}

int64_t q_search_find_item(ray_t* x, ray_t* v, int64_t cnt) {
    ray_t** ex = (ray_t**)ray_data(x);
    for (int64_t i = 0; i < cnt; i++)
        if (ex[i] && v && (ex[i] == v || atom_eq(ex[i], v))) return i;
    return cnt;
}

/* Find is type-specific relative to x (ref/find.md:42; owner ruling 2026-09-15): a needle reaches a typed domain at
 * the domain's own type or across the int/long pair (learn/python/examples/list.md:91 finds a long in an int
 * vector), and is 'type otherwise; a general-list needle is a run under the rank law, so each item is asked, and
 * an empty needle has nothing to compare.  The pairs the owner has not ruled keep the kernel's coercion: short in
 * the integer family, real against float, byte against the integers, an enum on a sym domain.  x a typed vector,
 * never STR. */
int q_search_admits(ray_t* x, ray_t* y) {
    if (!y) return 1;
    if (y->type == RAY_LIST) {
        ray_t** e = (ray_t**)ray_data(y);
        for (int64_t i = 0, n = ray_len(y); i < n; i++) if (!q_search_admits(x, e[i])) return 0;
        return 1;
    }
    if (y->type == RAY_STR) return x->type == RAY_CHARV;       /* a list of strings: char-vector items */
    int8_t d = x->type, t = y->type == -RAY_STR ? RAY_CHARV : y->type < 0 ? (int8_t)-y->type : y->type;
    if (t == d || t == RAY_ENUM || (ray_is_vec(y) && ray_len(y) == 0)) return 1;
    int di = d == RAY_I16 || d == RAY_I32 || d == RAY_I64, ti = t == RAY_I16 || t == RAY_I32 || t == RAY_I64;
    return (di && ti) || (di && t == RAY_BYTE_ONLY) || (ti && d == RAY_BYTE_ONLY) ||
           (q_type_is_float_tag(d) && q_type_is_float_tag(t));
}

/* rank read down the first items: an atom 0, a list one more than its first item (an empty list 1; a string
 * atom is a char list) — the axis find.md's "rank-sensitive" law compares on */
static int find_depth(ray_t* v) {
    if (!v || (ray_is_atom(v) && !q_type_is_str_atom(v))) return 0;
    if (ray_len(v) == 0) return 1;
    ray_t* e0 = q_index_elem_at(v, 0);
    int d = 1 + find_depth(e0);
    if (e0) ray_release(e0);
    return d;
}

ray_t* q_search_find(ray_t* x, ray_t* y) {
    if (q_type_is_table(x)) return find_rows(x, y);
    /* kt?row — a keyed table IS keytable!valuetable, so the dict's reverse
     * lookup reads through unchanged: the KEY row at the value row found. */
    if (q_type_is_keyed(x)) {
        ray_t* i = find_rows(ray_dict_vals(x), y);
        if (!i || RAY_IS_ERR(i)) return i;
        return keys_at(ray_dict_keys(x), i);
    }
    /* d?y — reverse dictionary lookup (basics/dictsandtables.md): the key of
     * the FIRST value matching y, i.e. keys[vals?y]. */
    if (q_type_is_plain_dict(x)) {
        ray_t* keys = ray_dict_keys(x);              /* borrowed */
        if (!keys) return q_err(QE_TYPE);
        int vo = 0;
        ray_t* vv = q_table_dict_vals(x, &vo);
        if (!vv) return q_err(QE_TYPE);
        ray_t* i = q_search_find(vv, y);             /* find arm: miss -> count */
        if (vo) ray_release(vv);
        if (!i || RAY_IS_ERR(i)) return i;
        return keys_at(keys, i);
    }
    if (x && (ray_is_vec(x) || x->type == RAY_LIST)) {          /* find */
        if (ray_is_vec(x) && x->type != RAY_STR && !q_search_admits(x, y)) return q_err(QE_TYPE);
        int64_t cnt = ray_len(x);
        int xd = find_depth(x) - 1;                  /* the rank of x's items, read off the first (find.md) */
        if (x->type == RAY_LIST && y && y->type == RAY_LIST && cnt > 0 && find_depth(y) == xd)
            return ray_i64(q_search_find_item(x, y, cnt));   /* y IS one item's shape: whole, so x[x?x 0] round-trips */
        if (y && y->type == RAY_LIST) {
            /* Find is right-atomic to the rank of x's items ("x?y looks for objects of rank n-1", find.md): an
             * item deeper than that rank is a run of them, found item by item — an atom item over a simple x
             * HITS, as the older editions print `w?(10 5 -1;-8;3 17)` -> (0 3 4;1;2 7) (docs-v1 search.md:132,
             * q1.txt:1046; the current page's miss is the divergence list/find.qcmd records) — and one at that
             * rank is matched whole (`u?(2 3;\`ab)` -> 3 3, never the whole of y).  The answer keeps y's shape, a
             * run of atoms collapsing to the index vector.  Empty x has no rank to read, so every item is one
             * miss (D2: a list probe on `()` is item-wise). */
            int64_t ny = ray_len(y);
            ray_t** e = (ray_t**)ray_data(y);
            ray_t* out = ray_list_new(ny > 0 ? ny : 1);
            if (RAY_IS_ERR(out)) return out;
            for (int64_t j = 0; j < ny; j++) {
                ray_t* rr;
                if (!e[j] || cnt == 0)
                    rr = ray_i64(cnt);
                else if (xd > 0 && find_depth(e[j]) <= xd)
                    rr = ray_i64(q_search_find_item(x, e[j], cnt));
                else
                    rr = q_search_find(x, e[j]);
                if (!rr || RAY_IS_ERR(rr)) { ray_release(out); return rr; }
                out = ray_list_append(out, rr);      /* retains */
                ray_release(rr);
                if (RAY_IS_ERR(out)) return out;
            }
            ray_t* c = q_list_collapse(out);
            ray_release(out);
            return c;
        }
        if (xd > 0 && y && !ray_is_atom(y) && y->type != RAY_LIST) {
            /* list-of-lists x, SIMPLE vector y: whole-y match (`u?10 2 -6` -> 1). */
            return ray_i64(q_search_find_item(x, y, cnt));
        }
        return miss_is_count(ray_find_fn(x, y), cnt);
    }
    return q_err(QE_TYPE);   /* unreachable: q_roll_wrap routes only find shapes here */
}

/* ===== q `x bin y` / `x binr y` — binary search ============================
 * ref/bin.md: x sorted, y the same type (no promotion).  `bin` -> index of
 * the LAST item <= y (-1 below the domain); `binr` -> the FIRST item >= y.
 * The base kernels are i64-only, so the ordering here comes from the `<`
 * VERB — one type home, and every sorted type (sym, float, char, temporal,
 * short, …) falls out.  Row-wise domains are below (bin_rows). */

/* Total order on two values: -1/0/1, *err on an incomparable pair.  Atoms
 * defer to `<`; deeper ranks compare item-by-item then break ties on length
 * (bin.md "items are lexicographically sorted").
 *
 * Atoms must carry the SAME tag: bin.md's domain is "an atom of exactly the
 * same type (no type promotion)", and `<` WOULD promote (1 2 3 bin 2.5 would
 * answer 1 rather than refuse).  Delegating order must not silently widen the
 * verb, so the pair is rejected here instead. */
static int ord_cmp(ray_t* a, ray_t* b, int* err) {
    if (!a || !b) { *err = 1; return 0; }
    if (a == b || atom_eq(a, b)) return 0;
    if (ray_is_atom(a) != ray_is_atom(b)) { *err = 1; return 0; }
    if (ray_is_atom(a)) {
        if (a->type != b->type) { *err = 1; return 0; }
        ray_t* f = q_registry_lookup_name("<", 1, Q_DYADIC);   /* borrowed */
        ray_t* av[2] = { a, b };
        ray_t* r = f ? q_eval_apply_value(f, av, 2) : NULL;
        if (!r || RAY_IS_ERR(r) || r->type != -RAY_BOOL) {
            if (r) ray_release(r);
            *err = 1;
            return 0;
        }
        int lt = r->b8 != 0;
        ray_release(r);
        return lt ? -1 : 1;
    }
    int64_t na = ray_len(a), nb = ray_len(b), n = na < nb ? na : nb;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ea = q_index_elem_at(a, i);
        ray_t* eb = q_index_elem_at(b, i);
        int c = 0;
        if (!ea || !eb || RAY_IS_ERR(ea) || RAY_IS_ERR(eb)) *err = 1;
        else c = ord_cmp(ea, eb, err);
        if (ea) ray_release(ea);
        if (eb) ray_release(eb);
        if (*err || c) return c;
    }
    return na < nb ? -1 : na > nb ? 1 : 0;
}

/* One probe against the sorted run x[sel[0..n)] — sel NULL reads x straight,
 * and a row-wise bin passes the positions of one equivalence class.  `right`
 * selects binr: the leftmost item >= y.  The answer indexes the RUN, and its
 * out-of-run signals (-1 below, n above) are the caller's to interpret. */
static int64_t bin_probe(ray_t* x, const int64_t* sel, int64_t n, ray_t* y,
                         int right, int* err) {
    int64_t lo = 0, hi = n - 1, r = right ? n : -1;
    while (lo <= hi && !*err) {
        int64_t mid = lo + (hi - lo) / 2;
        ray_t* e = q_index_elem_at(x, sel ? sel[mid] : mid);
        int c = 0;
        if (!e || RAY_IS_ERR(e)) *err = 1;
        else c = ord_cmp(e, y, err);
        if (e) ray_release(e);
        if (*err) break;
        if (right ? c >= 0 : c <= 0) { r = mid; if (right) hi = mid - 1; else lo = mid + 1; }
        else { if (right) lo = mid + 1; else hi = mid - 1; }
    }
    return r;
}

/* A LIST domain's overrun: bin.md documents no answer for a binr past the end
 * and no row pins one, so the retired kernel's clamp is carried forward. */
static int64_t bin_clamp(int64_t r, int64_t n, int right) {
    return right && r >= n ? n - 1 : r;
}

/* `t bin row` — bin.md Tables: the LAST row of x whose leading k-1 values MATCH the probe's and whose last value
 * does not exceed it; `0N` when no row matches the leading columns, or none within them is low enough.  The
 * equivalence classes come from the row Find over the leading columns (one pass for a run of probes); ORDER on the
 * last column stays a binary search over each class's positions, within which bin.md requires that column sorted. */
static ray_t* bin_rows(ray_t* x, ray_t* y, int right) {
    int64_t k = ray_table_ncols(x), n = ray_table_nrows(x);
    if (k <= 0 || !y) return q_err(QE_TYPE);
    ray_t** pc = (ray_t**)calloc((size_t)k * 2, sizeof *pc);
    if (!pc) return q_err(QE_TYPE);
    ray_t** dc = pc + k;
    int64_t m;
    int rec;
    int err = probe_cols(x, y, pc, k, &m, &rec);
    if (err) { free(pc); return probe_err(err); }
    err = row_domain(x, dc, pc, k - 1);
    ray_t* last = ray_table_get_col_idx(x, k - 1);
    ray_t* cls = err || k == 1 ? NULL : ray_find_rows_class_fn(dc, pc, k - 1, m);   /* no leading columns: one class */
    ray_t* out = cls && RAY_IS_ERR(cls) ? cls : ray_vec_new(RAY_I64, m > 0 ? m : 1);
    if (RAY_IS_ERR(out)) {                            /* the kernel's own error, propagated unmodified */
        for (int64_t j = 0; j < 2 * k; j++) if (pc[j]) ray_release(pc[j]);
        free(pc);
        if (cls && cls != out) ray_release(cls);
        return out;
    }
    out->len = m;
    for (int64_t i = 0; i < m && !err; i++) {
        ray_t* sel = cls ? ((ray_t**)ray_data(cls))[i] : NULL;                       /* borrowed */
        int64_t cn = sel ? ray_len(sel) : n;
        const int64_t* sd = sel ? (const int64_t*)ray_data(sel) : NULL;
        ray_t* pv = q_index_elem_at(pc[k - 1], ray_len(pc[k - 1]) == 1 ? 0 : i);
        if (!pv || RAY_IS_ERR(pv)) err = 1;
        int64_t r = err || cn == 0 ? -1 : bin_probe(last, sd, cn, pv, right, &err);
        int64_t* od = (int64_t*)ray_data(out);
        od[i] = !err && r >= 0 && r < cn ? (sd ? sd[r] : r) : NULL_I64;
        if (od[i] == NULL_I64) out->attrs |= RAY_ATTR_HAS_NULLS;
        if (pv) ray_release(pv);
    }
    for (int64_t j = 0; j < 2 * k; j++) if (pc[j]) ray_release(pc[j]);
    free(pc);
    if (cls) ray_release(cls);
    if (err) { ray_release(out); return q_err(QE_TYPE); }
    return row_answer(out, rec);
}

static ray_t* bin_search(ray_t* x, ray_t* y, int right) {
    if (!x || !y) return q_err(QE_TYPE);
    if (q_type_is_table(x)) return bin_rows(x, y, right);
    /* keyed domain (bin.md "y needs to contain all value columns, and it is
     * the keys that are returned"): the same dict law as below, over rows. */
    if (q_type_is_keyed(x)) {
        ray_t* i = bin_rows(ray_dict_vals(x), y, right);
        if (!i || RAY_IS_ERR(i)) return i;
        return keys_at(ray_dict_keys(x), i);
    }
    /* dict domain (bin.md "x is a dictionary with its values sorted"):
     * keys[vals bin y] — the -1 miss reads back as the key null. */
    if (q_type_is_plain_dict(x)) {
        ray_t* keys = ray_dict_keys(x);              /* borrowed */
        if (!keys) return q_err(QE_TYPE);
        int vo = 0;
        ray_t* vv = q_table_dict_vals(x, &vo);
        if (!vv) return q_err(QE_TYPE);
        ray_t* i = bin_search(vv, y, right);
        if (vo) ray_release(vv);
        if (!i || RAY_IS_ERR(i)) return i;
        return keys_at(keys, i);
    }
    if (!ray_is_vec(x) && x->type != RAY_LIST) return q_err(QE_TYPE);
    int64_t n = ray_len(x);
    int err = 0;
    if (ray_is_atom(y) || q_index_is_nested(y) != q_index_is_nested(x)) {
        int64_t r = bin_clamp(bin_probe(x, NULL, n, y, right, &err), n, right);
        return err ? q_err(QE_TYPE) : ray_i64(r);
    }
    int64_t ny = ray_len(y);
    ray_t* out = ray_vec_new(RAY_I64, ny > 0 ? ny : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = ny;
    int64_t* o = (int64_t*)ray_data(out);
    for (int64_t j = 0; j < ny && !err; j++) {
        ray_t* e = q_index_elem_at(y, j);
        if (!e || RAY_IS_ERR(e)) err = 1;
        else o[j] = bin_clamp(bin_probe(x, NULL, n, e, right, &err), n, right);
        if (e) ray_release(e);
    }
    if (err) { ray_release(out); return q_err(QE_TYPE); }
    return out;
}

ray_t* q_bin_wrap(ray_t* x, ray_t* y)  { return bin_search(x, y, 0); }
ray_t* q_binr_wrap(ray_t* x, ray_t* y) { return bin_search(x, y, 1); }
