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
    /* STR-vector y (openq list-of-strings): whole-item membership -> atom */
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

/* ===== row-wise search — the composite row key =============================
 * A table row is searched by DICTIONARY-ENCODING every column to dense integer
 * ranks and FUSING a row's ranks into one i64 key, so a row search becomes an
 * ordinary vector search.  No row hash table: the column primitives' match
 * semantics (`0N~0N` is true) are inherited rather than re-derived.
 *
 * The radix is 1+distinct, NEVER distinct.  An absent probe value ranks at
 * `count distinct` — find's miss answer — and that digit has to be unreachable
 * by any domain row or a miss collides with a real one: (a:1 2 1;b:`x`y`y)
 * fuses row 2 to key 2 at radix 2, exactly what a probe (99;`x) would carry.
 * The reserved digit is what keeps a miss a miss. */

/* `group x` through the apply seam.  Its keys ARE `distinct x` and its values
 * the rank classes, so ONE hashed pass yields the dictionary, the ranks and the
 * radix; `distinct`+`?` is two passes, the second a linear scan per item. */
static ray_t* group_of(ray_t* x) {
    ray_t* f = q_registry_lookup_name("group", 5, Q_MONADIC);   /* borrowed */
    ray_t* av[1] = { x };
    ray_t* r = f ? q_eval_apply_value(f, av, 1) : NULL;
    return r ? r : q_err(QE_TYPE);
}

/* The running key accumulator: n zeroed i64 slots, owned. */
static ray_t* zero_keys(int64_t n) {
    ray_t* v = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(v)) return v;
    v->len = n;
    int64_t* d = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < n; i++) d[i] = 0;
    return v;
}

/* Rank a domain column and the probe's values for it in ONE dictionary: the
 * domain's ranks invert `group`'s classes, the probe's come from finding it in
 * `key group`, so an absent probe value lands on *d.  Owned *dr (n long) and
 * *pr (m long, atoms broadcast); nonzero on failure. */
static int rank_pair(ray_t* dcol, ray_t* pcol, int64_t n, int64_t m,
                     ray_t** dr, ray_t** pr, int64_t* d) {
    *dr = *pr = NULL;
    ray_t* g = group_of(dcol);
    if (!g || !q_type_is_plain_dict(g)) { if (g) ray_release(g); return -1; }
    ray_t* dict = ray_dict_keys(g);                  /* borrowed */
    ray_t* cls = ray_dict_vals(g);                   /* borrowed */
    *d = dict ? ray_len(dict) : 0;
    ray_t* rv = zero_keys(n);
    if (RAY_IS_ERR(rv)) { ray_release(g); return -1; }
    int64_t* rd = (int64_t*)ray_data(rv);
    for (int64_t gi = 0; gi < *d; gi++) {
        ray_t* c = q_index_elem_at(cls, gi);
        if (!c || RAY_IS_ERR(c)) { if (c) ray_release(c); ray_release(rv); ray_release(g); return -1; }
        int64_t cn = ray_is_atom(c) ? 1 : ray_len(c);
        const int64_t* cp = ray_is_atom(c) ? &c->i64 : (const int64_t*)ray_data(c);
        for (int64_t t = 0; t < cn; t++)
            if (cp[t] >= 0 && cp[t] < n) rd[cp[t]] = gi;
        ray_release(c);
    }
    ray_t* pf = q_search_find(dict, pcol);           /* miss -> *d, the reserved rank */
    ray_release(g);
    int one = pf && q_type_is_int_atom(pf);
    int ok = one || (pf && !RAY_IS_ERR(pf) && q_type_is_int_vec(pf));
    int64_t pn = ok && !one ? ray_len(pf) : 1;
    if (!ok || (pn != m && pn != 1)) {
        if (pf) ray_release(pf);
        ray_release(rv);
        return -1;
    }
    ray_t* pv = zero_keys(m);
    if (RAY_IS_ERR(pv)) { ray_release(pf); ray_release(rv); return -1; }
    int64_t* pd = (int64_t*)ray_data(pv);
    for (int64_t i = 0; i < m; i++)
        pd[i] = one ? q_type_iatom_val(pf) : q_type_ivec_get(pf, pn == 1 ? 0 : i);
    ray_release(pf);
    *dr = rv;
    *pr = pv;
    return 0;
}

/* Fold the first k columns of table x and the matching probe columns into ONE
 * i64 key each, in lockstep so both rank in the same dictionaries.  When the
 * next radix would overflow, the running key is re-densified through the same
 * ranking — which re-bounds the radix by the row count.  Owned *dk (n) and
 * *pk (m); nonzero on failure. */
static int fuse_rows(ray_t* x, ray_t* const* pc, int64_t k, int64_t n, int64_t m,
                     ray_t** dk, ray_t** pk) {
    *dk = zero_keys(n);
    *pk = zero_keys(m);
    int64_t radix = 1;
    int bad = RAY_IS_ERR(*dk) || RAY_IS_ERR(*pk);
    for (int64_t j = 0; j < k && !bad; j++) {
        ray_t *dr, *pr;
        int64_t d;
        if (rank_pair(ray_table_get_col_idx(x, j), pc[j], n, m, &dr, &pr, &d)) { bad = 1; break; }
        if (radix > INT64_MAX / (d + 1)) {
            ray_t *nd, *np;
            int64_t d2;
            if (rank_pair(*dk, *pk, n, m, &nd, &np, &d2)) {
                ray_release(dr); ray_release(pr); bad = 1; break;
            }
            ray_release(*dk); ray_release(*pk);
            *dk = nd; *pk = np; radix = d2 + 1;
        }
        int64_t* dd = (int64_t*)ray_data(*dk);
        const int64_t* sd = (const int64_t*)ray_data(dr);
        for (int64_t i = 0; i < n; i++) dd[i] += radix * sd[i];
        int64_t* pd = (int64_t*)ray_data(*pk);
        const int64_t* sp = (const int64_t*)ray_data(pr);
        for (int64_t i = 0; i < m; i++) pd[i] += radix * sp[i];
        radix *= d + 1;
        ray_release(dr);
        ray_release(pr);
    }
    if (bad) {
        if (*dk) ray_release(*dk);
        if (*pk) ray_release(*pk);
        *dk = *pk = NULL;
    }
    return bad;
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
 * (dictionary or list) or table").  Owned pc[0..k); nothing owned on a shape
 * mismatch, reported as 1 for a TYPE fault and 2 for an ARITY one. */
static int probe_cols(ray_t* x, ray_t* y, ray_t** pc, int64_t k, int64_t* m, int* rec) {
    if (ray_is_atom(y)) return -1;               /* find.md: a rank-2 x seeks rank-1 records */
    int rows = probe_is_rowlist(x, y);
    ray_t* fy = (q_type_is_table(y) || rows) ? q_flip_wrap(y) : NULL;   /* owned */
    ray_t* d = fy ? fy : y;
    ray_t* pn = q_type_is_plain_dict(d) ? ray_dict_keys(d) : NULL;  /* borrowed */
    ray_t* pv = pn ? ray_dict_vals(d) : d;
    ray_t* fx = pn ? q_flip_wrap(x) : NULL;                         /* owned */
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

/* `t ? row` — the smallest row index of table x matching the probe
 * (find.md Searching tables); a miss is `count x`, both inherited from the
 * ordinary find over the fused keys. */
static ray_t* find_rows(ray_t* x, ray_t* y) {
    int64_t k = ray_table_ncols(x), n = ray_table_nrows(x);
    if (k <= 0 || !y) return q_err(QE_TYPE);
    ray_t** pc = (ray_t**)malloc((size_t)k * sizeof *pc);
    if (!pc) return q_err(QE_TYPE);
    int64_t m;
    int rec;
    int bad_probe = probe_cols(x, y, pc, k, &m, &rec);
    if (bad_probe) { free(pc); return probe_err(bad_probe); }
    ray_t *dk, *pk;
    int bad = fuse_rows(x, pc, k, n, m, &dk, &pk);
    for (int64_t j = 0; j < k; j++) ray_release(pc[j]);
    free(pc);
    if (bad) return q_err(QE_TYPE);
    ray_t* r = q_search_find(dk, pk);
    ray_release(dk);
    ray_release(pk);
    return row_answer(r, rec);
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

/* First index of x (a boxed list) whose ITEM whole-matches v, else cnt
 * (the kdb miss).  Borrows both. */
static int64_t list_find_item(ray_t* x, ray_t* v, int64_t cnt) {
    ray_t** ex = (ray_t**)ray_data(x);
    for (int64_t i = 0; i < cnt; i++)
        if (ex[i] && v && (ex[i] == v || atom_eq(ex[i], v))) return i;
    return cnt;
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
        int64_t cnt = ray_len(x);
        int x_ranked = x->type == RAY_LIST && q_index_is_nested(x);
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
            if (q_index_is_nested(y)) {
                ray_t* out = ray_list_new(ny > 0 ? ny : 1);
                if (RAY_IS_ERR(out)) return out;
                for (int64_t j = 0; j < ny; j++) {
                    ray_t* rr;
                    if (!e[j] || (ray_is_atom(e[j]) && e[j]->type != -RAY_STR))
                        rr = ray_i64(cnt);           /* rank-0 item: miss */
                    else
                        rr = q_search_find(x, e[j]);
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

/* `t bin row` — bin.md Tables: the LAST row of x whose leading k-1 values MATCH
 * the probe's and whose last value does not exceed it; `0N` when no row matches
 * the leading columns, or none within them is low enough.  Equality on the
 * leading columns is the fused row key; ORDER on the last column stays a binary
 * search, run over the positions of the matching equivalence class — which
 * `group` hands over directly, and within which bin.md requires that column
 * sorted.  Ranks are arbitrary labels, so they are never asked about order. */
static ray_t* bin_rows(ray_t* x, ray_t* y, int right) {
    int64_t k = ray_table_ncols(x), n = ray_table_nrows(x);
    if (k <= 0 || !y) return q_err(QE_TYPE);
    ray_t** pc = (ray_t**)malloc((size_t)k * sizeof *pc);
    if (!pc) return q_err(QE_TYPE);
    int64_t m;
    int rec;
    int bad_probe = probe_cols(x, y, pc, k, &m, &rec);
    if (bad_probe) { free(pc); return probe_err(bad_probe); }
    ray_t *dk, *pk;
    int err = fuse_rows(x, pc, k - 1, n, m, &dk, &pk);
    ray_t* g = err ? NULL : group_of(dk);
    ray_t* gi = q_type_is_plain_dict(g) ? q_search_find(ray_dict_keys(g), pk) : NULL;
    ray_t* out = zero_keys(m);
    if (!gi || RAY_IS_ERR(gi) || !q_type_is_int_vec(gi) || RAY_IS_ERR(out)) err = 1;
    if (!err) {
        ray_t* cls = ray_dict_vals(g);               /* borrowed: class -> positions */
        ray_t* last = ray_table_get_col_idx(x, k - 1);
        int64_t ng = ray_len(ray_dict_keys(g));
        int64_t* od = (int64_t*)ray_data(out);
        for (int64_t i = 0; i < m && !err; i++) {
            int64_t ci = q_type_ivec_get(gi, i);
            od[i] = NULL_I64;
            if (ci < 0 || ci >= ng) {                /* leading columns unmatched */
                out->attrs |= RAY_ATTR_HAS_NULLS;
                continue;
            }
            ray_t* c = q_index_elem_at(cls, ci);
            ray_t* pv = ray_is_atom(pc[k - 1]) ? pc[k - 1] : q_index_elem_at(pc[k - 1], i);
            if (!c || RAY_IS_ERR(c) || !pv || RAY_IS_ERR(pv)) err = 1;
            else {
                int64_t cn = ray_len(c);
                const int64_t* sel = (const int64_t*)ray_data(c);
                int64_t r = bin_probe(last, sel, cn, pv, right, &err);
                if (!err && r >= 0 && r < cn) od[i] = sel[r];
            else if (!err) out->attrs |= RAY_ATTR_HAS_NULLS;
            }
            if (c) ray_release(c);
            if (pv && pv != pc[k - 1]) ray_release(pv);
        }
    }
    for (int64_t j = 0; j < k; j++) ray_release(pc[j]);
    free(pc);
    if (dk) ray_release(dk);
    if (pk) ray_release(pk);
    if (g) ray_release(g);
    if (gi) ray_release(gi);
    if (err) { if (!RAY_IS_ERR(out)) ray_release(out); return q_err(QE_TYPE); }
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
