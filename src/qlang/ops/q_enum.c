/* ops/q_enum.c — THE enumeration home (kdb 20h; 2026-08-22 enum plan; decls: q_prim.h).
 * i64 positions are the stored truth (domain-NAME sym id at aux 8-15, the link_target
 * pattern); resolution is LAZY through the env as a GLOBAL read (ref/enumerate.md:
 * after `` d[0]:`o `` the enum shows the new symbols while "i"$e is unchanged). */
#include "qlang/q_registry_internal.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"
#include "qlang/q_env.h"
#include "mem/heap.h"   /* RAY_ATTR_SORTED / HAS_INDEX / HAS_NULLS */
#include "lang/internal.h" /* ray_concat_fn — the col-append seam's one concat */
#include "lang/eval.h"     /* ray_take_fn — the empty-source take fill */
#include "qlang/ops/q_index.h" /* q_index_at — the one TOTAL gather (resolve, deref) */
#include "qlang/ops/q_dollar.h" /* q_dollar_cast — the one int-family widen (R2 construction) */
#include "qlang/io/q_splay.h"  /* q_splay_col — deref through a mapped-splay target */
#include "table/dict.h"        /* ray_dict_slots — keyed-target halves at deref */
#include "table/sym.h"
#include <assert.h>
#include <string.h>

int q_enum_is(ray_t* x) {
    return x && !RAY_IS_ERR(x) &&
           (x->type == RAY_ENUM || x->type == -RAY_ENUM);
}

/* The TOP BYTE of the domain slot carries the enum's kdb attribute letter
 * ('u'/'p'/'g', 0 = none): an enum cannot take an engine index block (aux 8-15
 * IS the domain), so the letter rides the id.  Sym ids are intern-table
 * indices, far below 2^56; every reader masks, every plain set clears — so a
 * re-stamp DROPS the letter, matching "removed by any operation". */
#define ENUM_ATTR_MASK 0xff00000000000000ULL

/* Slice-aware: slice headers keep parent/offset in aux, so the domain rides the parent. */
int64_t q_enum_domain(ray_t* x) {
    if (x->attrs & RAY_ATTR_SLICE) x = x->slice_parent;
    uint64_t raw;
    memcpy(&raw, x->aux + 8, 8);
    return (int64_t)(raw & ~ENUM_ATTR_MASK);
}

static void enum_set_domain(ray_t* x, int64_t dom) {
    assert(!((uint64_t)dom & ENUM_ATTR_MASK));   /* an id reaching the letter byte is a broken intern */
    memcpy(x->aux + 8, &dom, 8);
}

/* The letter of a 20h vector (0 = none).  A slice is an operation result, so
 * it reads unattributed rather than borrowing the parent's letter. */
char q_enum_attr(ray_t* x) {
    if (!x || RAY_IS_ERR(x) || x->type != RAY_ENUM || (x->attrs & RAY_ATTR_SLICE)) return 0;
    uint64_t raw;
    memcpy(&raw, x->aux + 8, 8);
    return (char)(raw >> 56);
}

/* Stamp/clear the letter on an EXCLUSIVELY-OWNED 20h vector (a plain domain
 * set clears it, so any re-stamp is attribute-dropping by policy). */
int q_enum_attr_set(ray_t* x, char letter) {
    if (!x || RAY_IS_ERR(x) || x->type != RAY_ENUM || (x->attrs & RAY_ATTR_SLICE)) return 0;
    uint64_t raw;
    memcpy(&raw, x->aux + 8, 8);
    raw = (raw & ~ENUM_ATTR_MASK) | ((uint64_t)(uint8_t)letter << 56);
    memcpy(x->aux + 8, &raw, 8);
    return 1;
}

/* Re-tag an OWNED i64 value (atom / I64 vector / list of those) as the enum of
 * `dom`.  Consumes v; a shared or sliced vector copies first. */
ray_t* q_enum_stamp(ray_t* v, int64_t dom) {
    if (!v || RAY_IS_ERR(v)) return v;
    if (v->type == -RAY_I64) {
        if (v->rc > 1) { ray_t* c = ray_i64(v->i64); ray_release(v); v = c; }
        if (RAY_IS_ERR(v)) return v;
        v->type = -RAY_ENUM;
        enum_set_domain(v, dom);
        return v;
    }
    if (v->type == RAY_I64 || v->type == RAY_ENUM) {   /* re-stamp allowed: the
                                                        * collapse home builds the
                                                        * 20h vector directly */
        if (v->rc > 1 || (v->attrs & RAY_ATTR_SLICE)) {
            ray_t* c = ray_vec_from_raw(RAY_I64, ray_data(v), ray_len(v));
            ray_release(v);
            v = c;
            if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_OOM);
        }
        v->type = RAY_ENUM;
        v->attrs &= (uint8_t)~(RAY_ATTR_SORTED | RAY_ATTR_HAS_INDEX);
        enum_set_domain(v, dom);
        return v;
    }
    if (v->type == RAY_LIST) {                     /* nested index result */
        ray_t** e = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < ray_len(v); i++) {
            ray_t* r = q_enum_stamp(e[i], dom);    /* consumes the slot's ref */
            if (!r || RAY_IS_ERR(r)) { e[i] = NULL; ray_release(v); return r; }
            e[i] = r;
        }
        return v;
    }
    return v;                                      /* errors, nulls: untouched */
}

/* Owned plain-i64 copy of the positions (the strip half of strip->op->stamp). */
ray_t* q_enum_positions(ray_t* e) {
    if (e->type == -RAY_ENUM) return ray_i64(e->i64);
    ray_t* v = ray_vec_from_raw(RAY_I64, ray_data(e), ray_len(e));
    if (v && !RAY_IS_ERR(v) && (e->attrs & RAY_ATTR_HAS_NULLS))
        v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}

/* THE domain-shape classifier (2026-08-23 rework): every consumer — display,
 * value/decay, wire, write-coerce, meta/fkeys, file image — derives its law
 * from the DOMAIN'S CURRENT SHAPE through this one seam, never from
 * construction or a private env probe.  *vals (borrowed, env-owned) is the
 * VALUE LIST when one exists: a symlist global, or the single key column of a
 * keyed-table global, of ANY vector type (ref/fkeys.md's example is int-keyed). */
q_edom_t q_enum_domain_kind(int64_t dom, ray_t** vals) {
    if (vals) *vals = NULL;
    ray_t* d = q_env_get(dom);
    if (!d || RAY_IS_ERR(d)) return Q_EDOM_UNBOUND;
    if (d->type == RAY_SYM) { if (vals) *vals = d; return Q_EDOM_SYMLIST; }
    if (q_type_is_keyed(d)) {
        ray_t* kt = ray_dict_keys(d);
        if (kt && ray_table_ncols(kt) == 1) {
            ray_t* kc = ray_table_get_col_idx(kt, 0);
            if (kc && ray_is_vec(kc)) { if (vals) *vals = kc; return Q_EDOM_KEYED1; }
        }
        return Q_EDOM_KEYEDN;
    }
    if (d->type == RAY_TABLE || q_splay_is(d)) return Q_EDOM_TABLE;
    return Q_EDOM_OTHER;
}

static ray_t* enum_domain_vals(int64_t dom) {
    ray_t* v = NULL;
    (void)q_enum_domain_kind(dom, &v);
    return v;
}

/* sym-typed narrowing of the domain values, for the paths that translate SYM
 * constants into position space (extend, cmp) */
static ray_t* enum_domain_syms(int64_t dom) {
    ray_t* d = enum_domain_vals(dom);
    return (d && d->type == RAY_SYM) ? d : NULL;
}

/* The key TABLE of a compound (>=2 key columns) keyed-table domain, borrowed;
 * NULL otherwise. */
static ray_t* enum_domain_keytbl(int64_t dom) {
    if (q_enum_domain_kind(dom, NULL) != Q_EDOM_KEYEDN) return NULL;
    return ray_dict_keys(q_env_get(dom));
}

/* TOTAL resolution (R4): the index home's total gather, exactly `d idx` — a
 * null OR out-of-range position resolves to the value type's null (d[0N] is `).
 * NULL only when the domain has no value list (unkeyed table, compound keyed,
 * unbound, other); real errors (OOM) propagate owned. */
ray_t* q_enum_resolve(ray_t* e) {
    ray_t* d = enum_domain_vals(q_enum_domain(e));
    if (!d) return NULL;
    ray_t* pos = q_enum_positions(e);
    if (!pos || RAY_IS_ERR(pos)) return pos ? pos : q_err(QE_OOM);
    ray_t* r = q_index_at(d, &pos, 1);
    ray_release(pos);
    return r ? r : q_err(QE_OOM);
}

/* The value image: resolved values when the domain has a value list, else the
 * bare positions (a reference into an opaque target strips to its ints —
 * wp/foreign-keys.md:188-213).  Never a shape error; OOM propagates owned. */
ray_t* q_enum_val_image(ray_t* e) {
    ray_t* r = q_enum_resolve(e);
    return r ? r : q_enum_positions(e);
}

/* Decay (value-verb boundary): the value image, except an UNBOUND domain
 * signals its own name (resolution IS a read of that global and an undefined
 * word signals itself, ref/signal.md). */
ray_t* q_enum_decay(ray_t* e) {
    if (q_enum_domain_kind(q_enum_domain(e), NULL) == Q_EDOM_UNBOUND) {
        ray_t* s = ray_sym_str(q_enum_domain(e));
        if (!s) return q_err(QE_TYPE);
        ray_t* err = q_err_name(ray_str_ptr(s), ray_str_len(s));
        ray_release(s);
        return err;
    }
    return q_enum_val_image(e);
}

static int64_t enum_find(ray_t* d, int64_t sym_id) {
    int64_t dn = ray_len(d);
    for (int64_t k = 0; k < dn; k++)
        if (ray_vec_get_sym_id(d, k) == sym_id) return k;
    return -1;
}

/* Domain value/s (or a same-domain enum) -> owned i64 position/s; out-of-domain
 * is 'cast (ref/enumerate.md; the training doc pins the same on the write
 * paths).  A non-sym single-key domain (ref/fkeys.md) looks values up through
 * the Find home — misses come back as count, kdb's own miss law.  A domain
 * with NO value list (unkeyed table, compound keyed, unbound) has nothing to
 * look values up in: int-family payloads pass as POSITIONS unvalidated —
 * kdb's link / compound-FK non-enforcement, derived from shape. */
ray_t* q_enum_coerce(int64_t dom, ray_t* y) {
    if (q_enum_is(y)) {
        if (q_enum_domain(y) != dom) return q_err(QE_TYPE);
        return q_enum_positions(y);
    }
    ray_t* d = enum_domain_vals(dom);
    if (!d) {
        if (!q_type_is_int_atom(y) && !q_type_is_int_vec(y)) return q_err(QE_TYPE);
        return q_dollar_cast(RAY_I64, y);        /* the one conversion owner:
                                                  * widen, nulls mapped */
    }
    if (d->type == RAY_SYM && y->type != -RAY_SYM && y->type != RAY_SYM)
        return q_err(QE_TYPE);                     /* wrong LANE into a symlist */
    ray_t* f = q_search_find(d, y);                /* hashed, whatever the type */
    if (!f || RAY_IS_ERR(f)) return f ? f : q_err(QE_TYPE);
    int64_t dn = ray_len(d);
    int miss = 0;
    if (f->type == -RAY_I64) miss = f->i64 < 0 || f->i64 >= dn;
    else if (f->type == RAY_I64) {
        const int64_t* p = (const int64_t*)ray_data(f);
        for (int64_t i = 0; i < ray_len(f) && !miss; i++)
            miss = p[i] < 0 || p[i] >= dn;
    } else miss = 1;
    if (miss) { ray_release(f); return q_err(QE_CAST); }
    return f;
}

/* One y row (nkey sym cells: a sym vector, or a list of sym atoms) against
 * the key-table rows -> position, or -1 ('cast) / -2 (not sym-row shaped). */
static int64_t enum_row_find(ray_t* kt, ray_t* row) {
    int64_t nk = ray_table_ncols(kt);
    int64_t ids[8];
    if (nk > 8) return -2;
    if (!row || ray_len(row) != nk) return -2;
    for (int64_t j = 0; j < nk; j++) {
        if (row->type == RAY_SYM) ids[j] = ray_vec_get_sym_id(row, j);
        else if (row->type == RAY_LIST) {
            ray_t* c = ((ray_t**)ray_data(row))[j];
            if (!c || c->type != -RAY_SYM) return -2;
            ids[j] = c->i64;
        } else return -2;
    }
    for (int64_t j = 0; j < nk; j++)
        if (ray_table_get_col_idx(kt, j)->type != RAY_SYM) return -2;
    int64_t n = ray_table_nrows(kt);
    for (int64_t e = 0; e < n; e++) {
        int hit = 1;
        for (int64_t j = 0; j < nk && hit; j++)
            hit = ray_vec_get_sym_id(ray_table_get_col_idx(kt, j), e) == ids[j];
        if (hit) return e;
    }
    return -1;
}

/* Compound-FK Enumerate (wp/foreign-keys.md:128-170): rows of y looked up in
 * the multi-column key table.  The result is the SAME reference structure —
 * positions stamped with the domain name (R1); the compound domain has no
 * single value list, so display/value/write-law derive the int face from the
 * shape classifier (wp:172's "converted to an integer" describes the contents;
 * the training doc's non-enforcement gotcha is the no-value-list coerce law).
 * A single row answers the -20h atom: the wp:154 `t1$`C`NDQ insert-cell
 * spelling. */
static ray_t* enum_dollar_compound(int64_t dom, ray_t* kt, ray_t* y) {
    ray_t* out = NULL;
    if (y->type == RAY_LIST && ray_len(y) == 0) {        /* schema `t$() */
        out = ray_vec_new(RAY_I64, 1);
    } else {
        int64_t one = enum_row_find(kt, y);
        if (one >= 0) return q_enum_stamp(ray_i64(one), dom);
        if (one == -1) return q_err(QE_CAST);
        if (y->type != RAY_LIST) return q_err(QE_TYPE);
        int64_t n = ray_len(y);
        out = ray_vec_new(RAY_I64, n);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        out->len = n;
        int64_t* o = (int64_t*)ray_data(out);
        for (int64_t i = 0; i < n; i++) {
            int64_t p = enum_row_find(kt, ((ray_t**)ray_data(y))[i]);
            if (p < 0) { ray_release(out); return q_err(p == -1 ? QE_CAST : QE_TYPE); }
            o[i] = p;
        }
    }
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    return q_enum_stamp(out, dom);
}

/* `x$y` Enumerate (ref/enumerate.md): x names a global symlist (or keyed
 * table — foreign keys); every item of y must already be in it, else 'cast.
 * An undefined domain signals its name.  An empty y types an empty enum —
 * the table-literal schema column `` sym:`financials$() `` (wp:52-57). */
ray_t* q_enum_dollar(ray_t* x, ray_t* y) {
    if (!x || x->type != -RAY_SYM || !y) return q_err(QE_TYPE);
    ray_t* kt = enum_domain_keytbl(x->i64);
    if (kt) return enum_dollar_compound(x->i64, kt, y);
    if (!enum_domain_vals(x->i64)) {
        ray_t* s = ray_sym_str(x->i64);
        if (!s) return q_err(QE_TYPE);
        ray_t* err = q_err_name(ray_str_ptr(s), ray_str_len(s));
        ray_release(s);
        return err;
    }
    if (y->type == RAY_LIST && ray_len(y) == 0)
        return q_enum_stamp(ray_vec_new(RAY_I64, 1), x->i64);
    ray_t* p = q_enum_coerce(x->i64, y);
    if (!p || RAY_IS_ERR(p)) return p ? p : q_err(QE_TYPE);
    return q_enum_stamp(p, x->i64);
}

/* `x?y` Enum Extend, variable form (ref/enum-extend.md): fill missing items of y
 * into the GLOBAL x (q_env_set — the one global-set home), then enumerate.  An
 * unbound x starts empty, per the doc's Filepath steps (absent file = created).
 * _try is the `?` wrap's probe: NULL = not the extend shape (sym handle ? syms);
 * the filepath form (`:x?y) stays 'nyi. */
static ray_t* enum_extend(ray_t* x, ray_t* y);

ray_t* q_enum_extend_try(ray_t* x, ray_t* y) {
    if (!x || x->type != -RAY_SYM || !y) return NULL;
    if (y->type != -RAY_SYM && y->type != RAY_SYM) return NULL;
    ray_t* s = ray_sym_str(x->i64);
    int isfile = s && ray_str_len(s) > 0 && ray_str_ptr(s)[0] == ':';
    if (s) ray_release(s);
    if (isfile) return q_err(QE_NYI);
    /* a bound non-symlist global (a table domain included) is not extendable —
     * writing the union would CLOBBER it (ref/enum-extend.md's d is a list) */
    ray_t* g = q_env_get(x->i64);
    if (g && !RAY_IS_ERR(g) && g->type != RAY_SYM) return q_err(QE_TYPE);
    return enum_extend(x, y);
}

static ray_t* enum_extend(ray_t* x, ray_t* y) {
    ray_t* d = enum_domain_syms(x->i64);
    ray_t* nd = ray_sym_vec_new(RAY_SYM_W64, d ? ray_len(d) : ray_len(y));
    if (!nd || RAY_IS_ERR(nd)) return nd ? nd : q_err(QE_OOM);
    for (int64_t i = 0; d && i < ray_len(d); i++) {
        int64_t id = ray_vec_get_sym_id(d, i);
        nd = ray_vec_append(nd, &id);
        if (!nd || RAY_IS_ERR(nd)) return nd ? nd : q_err(QE_OOM);
    }
    int64_t grew = 0, yn = y->type == -RAY_SYM ? 1 : ray_len(y);
    for (int64_t i = 0; i < yn; i++) {
        int64_t id = y->type == -RAY_SYM ? y->i64 : ray_vec_get_sym_id(y, i);
        if (enum_find(nd, id) >= 0) continue;
        nd = ray_vec_append(nd, &id);
        if (!nd || RAY_IS_ERR(nd)) return nd ? nd : q_err(QE_OOM);
        grew++;
    }
    if (grew || !d) {
        ray_err_t e = q_env_set(x->i64, nd);          /* retains */
        if (e != RAY_OK) { ray_release(nd); return q_env_err(e); }
    }
    ray_release(nd);
    ray_t* p = q_enum_coerce(x->i64, y);
    if (!p || RAY_IS_ERR(p)) return p ? p : q_err(QE_TYPE);
    return q_enum_stamp(p, x->i64);
}

/* `x!y` Enumeration — ENV-BLIND construction (R2, 2026-08-23): a sym atom
 * paired with int-family positions ALWAYS makes the reference, name and range
 * unvalidated (`a!1 works unbound; `d!0 9 100 stores what it is handed —
 * resolution judges lazily, R4).  Short/int/long widen to i64 (32-bit-kdb
 * carryover); any other payload is 'type. */
ray_t* q_enum_ref(int64_t dom, ray_t* y) {
    if (!q_type_is_int_atom(y) && !q_type_is_int_vec(y)) return q_err(QE_TYPE);
    ray_t* v = q_dollar_cast(RAY_I64, y);        /* atoms + vectors, nulls mapped */
    if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_TYPE);
    return q_enum_stamp(v, dom);
}

/* A sym id moved into position space: its slot in the live domain, else the
 * null position for ` (d[0N] is `), else -1 — a value no stored position
 * takes, so an absent constant matches nothing. */
static int64_t enum_pos_of_id(ray_t* d, int64_t id) {
    int64_t k = enum_find(d, id);
    if (k >= 0) return k;
    return id == 0 ? NULL_I64 : -1;
}

/* Position-space comparison (owner amendment 2026-08-22): the sym CONSTANT
 * translates into the enum's position space — one domain lookup — and the
 * raw stored positions are scanned IN PLACE: no gather, no column-length
 * materialization, the mmapped-HDB point of enums.  op: 0 `=`, 1 `<>`,
 * 2 `e in y` (y is the constant set), 3 `y in e` (result shaped like y).
 * NULL = not this shape (caller falls back to the decay law): y not
 * sym-shaped, domain unbound, or a pairwise length mismatch. */
ray_t* q_enum_cmp(ray_t* e, ray_t* y, int op) {
    if (!y || (y->type != -RAY_SYM && y->type != RAY_SYM)) return NULL;
    ray_t* d = enum_domain_syms(q_enum_domain(e));
    if (!d) return NULL;
    int eatom = e->type == -RAY_ENUM;
    int64_t n = eatom ? 1 : ray_len(e);
    int64_t one = eatom ? e->i64 : 0;
    const int64_t* p = eatom ? &one : (const int64_t*)ray_data(e);
    /* a stored position outside the live domain simply matches nothing (R4:
     * total-indexing semantics — it resolves null, and null-vs-constant is
     * a miss; enum_pos_of_id answers -1 for an absent constant, a value no
     * stored position takes) */
    int yatom = y->type == -RAY_SYM;
    int64_t yn = yatom ? 1 : ray_len(y);
    if (op <= 1) {                                    /* atomic =/<>: pairwise or broadcast */
        if (!yatom && yn != n) return NULL;
        ray_t* out = ray_vec_new(RAY_BOOL, n > 0 ? n : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        out->len = n;
        uint8_t* o = (uint8_t*)ray_data(out);
        int64_t c = yatom ? enum_pos_of_id(d, y->i64) : 0;
        for (int64_t i = 0; i < n; i++) {
            if (!yatom) c = enum_pos_of_id(d, ray_vec_get_sym_id(y, i));
            o[i] = (uint8_t)((p[i] == c) ^ op);
        }
        if (eatom && yatom) { int64_t b = o[0]; ray_release(out); return ray_bool((int32_t)b); }
        return out;
    }
    if (op == 2) {                                    /* e in set */
        int64_t set[64];
        if (yn > 64) return NULL;                     /* huge sets: decay path */
        for (int64_t j = 0; j < yn; j++)
            set[j] = enum_pos_of_id(d, yatom ? y->i64 : ray_vec_get_sym_id(y, j));
        ray_t* out = ray_vec_new(RAY_BOOL, n > 0 ? n : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        out->len = n;
        uint8_t* o = (uint8_t*)ray_data(out);
        for (int64_t i = 0; i < n; i++) {
            uint8_t hit = 0;
            for (int64_t j = 0; j < yn && !hit; j++) hit = p[i] == set[j];
            o[i] = hit;
        }
        if (eatom) { int64_t b = o[0]; ray_release(out); return ray_bool((int32_t)b); }
        return out;
    }
    /* y in e: each constant against the position scan; result shaped like y */
    ray_t* out = ray_vec_new(RAY_BOOL, yn > 0 ? yn : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    out->len = yn;
    uint8_t* o = (uint8_t*)ray_data(out);
    for (int64_t j = 0; j < yn; j++) {
        int64_t c = enum_pos_of_id(d, yatom ? y->i64 : ray_vec_get_sym_id(y, j));
        uint8_t hit = 0;
        for (int64_t i = 0; i < n && !hit; i++) hit = p[i] == c;
        o[j] = hit;
    }
    if (yatom) { int64_t b = o[0]; ray_release(out); return ray_bool((int32_t)b); }
    return out;
}

/* insert/upsert write law (training doc §2): a payload column entering an
 * enum column coerces into its domain ('cast outside it).  Owned PLAIN i64:
 * target positions ++ coerced payload — callers gather/stamp as their op
 * needs (the one seam both the append and the keyed-merge homes ride). */
ray_t* q_enum_col_concat(ray_t* oc, ray_t* pc) {
    ray_t* cp = q_enum_coerce(q_enum_domain(oc), pc);
    if (!cp || RAY_IS_ERR(cp)) return cp ? cp : q_err(QE_TYPE);
    ray_t* po = q_enum_positions(oc);
    if (!po || RAY_IS_ERR(po)) { ray_release(cp); return po ? po : q_err(QE_OOM); }
    ray_t* j = ray_concat_fn(po, cp);
    ray_release(po);
    ray_release(cp);
    return (j && !RAY_IS_ERR(j)) ? j : (j ? j : q_err(QE_TYPE));
}

/* First insert into an EMPTY enum schema column: the payload coerces whole
 * and the column keeps 20h (wp:52-57's `` sym:`financials$() `` flow). */
ray_t* q_enum_col_ingest(ray_t* oc, ray_t* pc) {
    ray_t* cp = q_enum_coerce(q_enum_domain(oc), pc);
    if (!cp || RAY_IS_ERR(cp)) return cp ? cp : q_err(QE_TYPE);
    return q_enum_stamp(cp, q_enum_domain(oc));
}

/* Dot-notation dereference (wp/foreign-keys.md:100-125, kb/linking-columns.md):
 * v is an enum whose domain names a target table (keyed: positions ARE
 * key-row indices; unkeyed / mapped splay: the link shape) — `v.fld` gathers
 * target[fld] at the stored positions through the one TOTAL gather, so
 * out-of-range/null positions land as nulls, never errors (wp:340-352).
 * NULL = not referential / target unbound / no such field (callers fall
 * through); real errors propagate. */
ray_t* q_enum_deref(ray_t* v, int64_t fld) {
    if (!v || RAY_IS_ERR(v)) return NULL;
    ray_t* forced = NULL;      /* a lazy splay column binds as a colref thunk;
                                * force it (idx-narrowed: survivors only) so a
                                * mapped link/FK column derefs like a live one */
    if (q_splay_colref_is(v)) {
        ray_t* car; int64_t nm; ray_t* idx;
        q_splay_colref_parts(v, &car, &nm, &idx);
        forced = q_splay_gather(car, nm, RAY_IS_NULL(idx) ? NULL : idx);
        if (!forced || RAY_IS_ERR(forced)) { if (forced) ray_release(forced); return NULL; }
        v = forced;
        ray_t* r = q_enum_deref(v, fld);
        ray_release(forced);
        return r;
    }
    if (!q_enum_is(v)) return NULL;
    int64_t tgt = q_enum_domain(v);
    ray_t* t = q_env_get(tgt);
    if (!t || RAY_IS_ERR(t)) return NULL;
    ray_t* col = NULL;                                   /* owned */
    if (q_splay_is(t)) {
        col = q_splay_col(t, fld);
        if (col && RAY_IS_ERR(col)) { ray_release(col); col = NULL; }
    } else if (q_type_is_keyed(t)) {
        ray_t* c = ray_table_get_col(ray_dict_slots(t)[0], fld);
        if (!c) c = ray_table_get_col(ray_dict_slots(t)[1], fld);
        if (c) { ray_retain(c); col = c; }
    } else if (t->type == RAY_TABLE) {
        ray_t* c = ray_table_get_col(t, fld);
        if (c) { ray_retain(c); col = c; }
    }
    if (!col) return NULL;
    ray_t* pos = q_enum_positions(v);
    if (!pos || RAY_IS_ERR(pos)) { ray_release(col); return pos ? pos : q_err(QE_OOM); }
    ray_t* r = q_index_at(col, &pos, 1);   /* an enum target column re-stamps in
                                            * the index home, so chains recurse */
    ray_release(pos);
    ray_release(col);
    return r ? r : q_err(QE_OOM);
}

/* empty-source column fill: the engine take kernel's law (i64 fills zeros),
 * stripped/restamped for reference columns */
static ray_t* enum_take_fill(void* nptr, ray_t* col) {
    ray_t* n = (ray_t*)nptr;
    if (col && col->type == RAY_ENUM) {
        ray_t* p = q_enum_positions(col);
        if (!p || RAY_IS_ERR(p)) return p ? p : q_err(QE_OOM);
        ray_t* t = ray_take_fn(p, n);
        ray_release(p);
        if (!t) return q_err(QE_TYPE);
        return q_enum_stamp(t, q_enum_domain(col));
    }
    ray_t* t = ray_take_fn(col, n);
    return t ? t : q_err(QE_TYPE);
}

/* n#x for reference-carrying shapes — an enum vector, a linked int vector, or
 * a table holding either — composed as index generation over the total gather,
 * so the run rides q_index_at's preserve/carry arms (a table column sits BELOW
 * the apply-level preserve arm; ref/take.md's circular law is the index walk).
 * NULL = not that shape / not a plain int n / empty source (the caller keeps
 * the plain kernel). */
ray_t* q_enum_take(ray_t* y, ray_t* n) {
    if (!y || !n || n->type != -RAY_I64 || RAY_ATOM_IS_NULL(n)) return NULL;
    int ref = y->type == RAY_ENUM;
    if (!ref && y->type == RAY_TABLE)
        for (int64_t c = 0; c < ray_table_ncols(y) && !ref; c++)
            ref = q_enum_is(ray_table_get_col_idx(y, c));
    if (!ref) return NULL;
    int64_t cnt = y->type == RAY_TABLE ? ray_table_nrows(y) : ray_len(y);
    if (cnt <= 0)   /* an empty source cannot cycle: the kernel's FILL law,
                     * per column so reference columns strip/restamp (codex r3) */
        return y->type == RAY_TABLE ? q_table_map_cols(enum_take_fill, n, y)
                                    : NULL;
    int64_t m = n->i64, k = m < 0 ? -m : m;
    ray_t* idx = ray_vec_new(RAY_I64, k > 0 ? k : 1);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_OOM);
    idx->len = k;
    int64_t* p = (int64_t*)ray_data(idx);
    int64_t start = m < 0 ? (cnt - (k % cnt ? k % cnt : cnt)) % cnt : 0;
    for (int64_t i = 0; i < k; i++) p[i] = (start + i) % cnt;
    ray_t* r = q_index_at(y, &idx, 1);
    ray_release(idx);
    return r ? r : q_err(QE_OOM);
}

/* meta's per-column reference facts (ref/meta.md: f is "foreign key (enums)";
 * kb/linking-columns.md shows the same f for links): *f_out gets the target
 * name for an FK enum over a table domain or a linked column.  Returns a
 * t-char override — an FK enum reads as its KEY column's type (ref/fkeys.md
 * pins `a| j f` for an int-keyed domain), a table-shaped (link-file) domain
 * as the stored longs — or 0 (keep the caller's char, f untouched). */
char q_enum_meta_f(ray_t* col, int64_t* f_out) {
    if (!col || RAY_IS_ERR(col) || !q_enum_is(col)) return 0;
    int64_t dom = q_enum_domain(col);
    ray_t* vals = NULL;
    switch (q_enum_domain_kind(dom, &vals)) {
    case Q_EDOM_KEYED1:
        *f_out = dom;
        return vals ? q_type_char((int8_t)vals->type) : 0;
    case Q_EDOM_KEYEDN:
    case Q_EDOM_TABLE:
        *f_out = dom;
        return 'j';
    default:
        return 0;         /* symlist: no f, char stays 's'; unbound/other: keep */
    }
}

ray_t* q_enum_null_atom(int64_t dom) {
    return q_enum_stamp(ray_i64(NULL_I64), dom);
}

ray_t* q_enum_null_col(int64_t dom, int64_t n) {
    ray_t* v = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_OOM);
    v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = NULL_I64;
    if (n > 0) v->attrs |= RAY_ATTR_HAS_NULLS;
    return q_enum_stamp(v, dom);
}

/* Wirefile constructor: owned enum vector over raw positions (w=4 widens shape
 * C's i32 indices, nulls included; w=8 takes shape D's i64s).  A negative
 * non-null position is invalid whatever the domain holds -> 'corrupt; positive
 * out-of-range is only judgeable against the live domain, i.e. at resolution. */
ray_t* q_enum_from_indices(int64_t dom, const uint8_t* p, int64_t n, int w) {
    ray_t* out = ray_vec_new(RAY_I64, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    out->len = n;
    int64_t* o = (int64_t*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        if (w == 4) {
            int32_t v; memcpy(&v, p + i * 4, 4);
            o[i] = v == NULL_I32 ? NULL_I64 : (int64_t)v;
        } else
            memcpy(&o[i], p + i * 8, 8);
        if (o[i] < 0 && o[i] != NULL_I64) { ray_release(out); return q_err(QE_CORRUPT); }
        if (o[i] == NULL_I64) out->attrs |= RAY_ATTR_HAS_NULLS;
    }
    return q_enum_stamp(out, dom);
}
