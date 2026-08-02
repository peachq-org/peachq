/* ops/q_list.c — the general list verbs: collapse (the homogeneous-atom-run ->
 * typed-vector home), raze/enlist, til/where, xprev/fills/fill.
 *
 * Split from q_registry.c (2026-07-14), then narrowed (2026-07-27) when the
 * `#`/`_` glyph homes went to ops/q_takedrop.c, column attributes to
 * ops/q_attr.c and the grade family to ops/q_sort.c — pure function moves in
 * both cases.  The shared internal surface lives in q_registry_internal.h.
 * See q_registry.h for the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_err.h"
#include "qlang/ops/q_type.h"  /* q_type_empty (the one typed-empty ctor), q_type_is_num_tag/_float_tag */
#include "lang/eval.h"     /* ray_take_fn, ray_xbar_fn */
#include "lang/internal.h" /* ray_iasc_fn/ray_idesc_fn, RAY_IS_TEMPORAL64, ray_error */
#include "qlang/ops/q_index.h" /* q_index_elem_at — the element read; q_index_at — the gather */
#include "table/sym.h"     /* ray_sym_intern_runtime, RAY_SYM_W64 */
#include <stdint.h>        /* uintptr_t */
#include <string.h>
#include <stdlib.h>        /* malloc/free */

/* ---- collapse: homogeneous atom list -> typed vector (q_registry_internal.h) ---- */

static ray_t* atom_run_collapse(ray_t* l) {
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

/* THE dict-rows builder: n plain sym-keyed dicts with MATCHING keys are the
 * table they spell (basics/glossary.md — a table "is also a list of
 * dictionaries with the same keys").  Per key the cells gather into one column
 * typed by the ATOM-RUN collapse alone: homogeneous goes typed, anything else
 * stays a legal boxed column.  The row law must NOT recurse into the cells —
 * a table-valued column has no representation here and would break the flip's
 * length accounting.  NULL = not that shape, so the caller keeps its list. */
static ray_t* dict_rows_table(ray_t* const* rows, int64_t n) {
    if (n < 1) return NULL;
    ray_t* k0 = NULL;
    for (int64_t r = 0; r < n; r++) {
        ray_t* d = rows[r];
        if (!d || d->type != RAY_DICT || q_type_is_keyed(d)) return NULL;
        ray_t* k = ray_dict_keys(d);
        if (!k || k->type != RAY_SYM || !ray_dict_vals(d)) return NULL;
        if (!r) k0 = k;
        else if (!q_match_rec(k0, k)) return NULL;   /* same names, same order */
    }
    int64_t nc = ray_len(k0);
    ray_t* cols = ray_list_new(nc > 0 ? nc : 1);
    if (RAY_IS_ERR(cols)) return cols;
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_list_new(n);
        for (int64_t r = 0; r < n && !RAY_IS_ERR(col); r++) {
            ray_t* cell = q_index_elem_at(ray_dict_vals(rows[r]), c);
            if (!cell || RAY_IS_ERR(cell)) {
                ray_release(col); ray_release(cols);
                return cell ? cell : q_err(QE_TYPE);
            }
            col = ray_list_append(col, cell);
            ray_release(cell);
        }
        if (RAY_IS_ERR(col)) { ray_release(cols); return col; }
        ray_t* cc = atom_run_collapse(col);
        ray_release(col);
        if (!cc || RAY_IS_ERR(cc)) { ray_release(cols); return cc ? cc : q_err(QE_TYPE); }
        cols = ray_list_append(cols, cc);
        ray_release(cc);
        if (RAY_IS_ERR(cols)) return cols;
    }
    ray_retain(k0);                              /* dict_new consumes both */
    ray_t* d = ray_dict_new(k0, cols);
    if (!d || RAY_IS_ERR(d)) return d ? d : q_err(QE_TYPE);
    ray_t* t = q_flip_wrap(d);
    ray_release(d);
    return t;
}

ray_t* q_list_collapse(ray_t* l) {
    if (l && !RAY_IS_ERR(l) && l->type == RAY_LIST && ray_len(l) > 0) {
        ray_t** e = (ray_t**)ray_data(l);
        if (e[0] && e[0]->type == RAY_DICT) {   /* a run of like rows IS a table */
            ray_t* tb = dict_rows_table(e, ray_len(l));
            if (tb) return tb;
        }
    }
    return atom_run_collapse(l);
}

/* ---- wrappers (bespoke q semantics over a rayfall primitive) ---- */

/* q_list_collapse leaves a ZERO-length boxed list untyped (no element to infer
 * from); an empty selection must instead inherit the PROTO's element type so it
 * keeps its domain (codex r3: `` type key `a _ `a!1 `` must be 11h / `` `symbol$()
 * ``, not 0h / `()`).  The proto is either a typed vector or — for a dict value
 * side, stored boxed — a list collapsed here to recover its type (heterogeneous
 * lists stay RAY_LIST and fall through untouched).  Consumes `collapsed`, borrows
 * `proto`; passes errors and non-empty results through untouched. */
ray_t* q_typed_empty_like(ray_t* collapsed, ray_t* proto) {
    if (!collapsed || RAY_IS_ERR(collapsed)) return collapsed;
    if (collapsed->type != RAY_LIST || ray_len(collapsed) != 0) return collapsed;
    ray_t* cp = (proto && proto->type == RAY_LIST) ? q_list_collapse(proto) : NULL;
    ray_t* p  = cp ? cp : proto;
    ray_t* tv = (p && ray_is_vec(p) && p->type != RAY_LIST) ? q_type_empty(p->type) : NULL;
    if (cp) ray_release(cp);
    if (!tv || RAY_IS_ERR(tv)) { if (tv) ray_release(tv); return collapsed; }
    ray_release(collapsed);
    return tv;
}

/* q `raze x` — base ray_raze_fn plus the kdb dict and atom arms: a dict razes
 * its values, an atom comes back as a 1-item list (ref/raze.md `raze 42` ->
 * ,42).  Everything else delegates. */
ray_t* q_raze_wrap(ray_t* x) {
    /* a dict razes its VALUES (ref/raze.md).  `raze` IS `,/` by DEFINITION
     * only: the fold recopies its accumulator and measures ~70x slower, so the
     * identity is pinned as a property row, never spelled as the call path. */
    if (x && x->type == RAY_DICT && !q_type_is_keyed(x))
        return ray_raze_fn(ray_dict_vals(x));
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
 * displays a table whose b cell is 2 3) — the one-row case of the shared
 * dict-rows builder.  Env-bound by q_builtins_register BEFORE registry init,
 * so the `,` monadic QK_ENV snapshot picks this wrapper up too. */
ray_t* q_enlist_wrap(ray_t** args, int64_t n) {
    if (n == 1 && args[0] && args[0]->type == RAY_DICT && !q_type_is_keyed(args[0])) {
        ray_t* t = dict_rows_table(args, 1);         /* non-sym keys are no table */
        return t ? t : q_err(QE_TYPE);
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
    /* a dict indexes its RANGE, not `til count d`: the keys gathered by
     * `where value d` (ref/where.md, q_ops.c FAMILY AUDIT).  Recursing reaches
     * the GENERAL integer law below, which is why no boolean arm is written —
     * the mask is that law's 0/1 case. */
    if (x && x->type == RAY_DICT && !q_type_is_keyed(x)) {
        ray_t* idx = q_where_wrap(ray_dict_vals(x));
        if (RAY_IS_ERR(idx)) return idx;
        ray_t* keys = q_index_at(ray_dict_keys(x), &idx, 1);
        ray_release(idx);
        return keys;
    }
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

static int is_sym_t(int8_t t) { return t == RAY_SYM || t == -RAY_SYM; }

ray_t* qj_ktbl_merge(ray_t* x, ray_t* y, int mode);   /* joins wave */

/* q `x^y` — fill: coalesce nulls in y with x (kdb `^`).  x may be an atom
 * (broadcast) or a same-length vector (element-wise).  Numeric result type:
 * F64 if EITHER operand is float, else I64 (the narrower-int-preserving lattice
 * is a deferred refinement — the `type` ledger rows that need it are blocked by
 * a separate `0n 2 3i` parse bug and split out).  Symbol fill is a distinct
 * path.  Dict / `fills` forward-fill / table / fill-scan forms are deferred. */
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
    if (!q_type_is_num_tag(x->type) || !q_type_is_num_tag(y->type))
        return q_err(QE_TYPE);
    int is_float = q_type_is_float_tag(x->type) || q_type_is_float_tag(y->type);
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

