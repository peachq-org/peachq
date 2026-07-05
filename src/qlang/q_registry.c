/* q op registry — see q_registry.h for the contract.
 *
 * The roster is now DRIVEN BY THE SINGLE-SOURCE MANIFEST (src/qlang/q_ops.c
 * Q_OPS[]).  q_registry_init iterates the manifest and, for every (row,
 * valence) whose build-kind is not QK_NONE, builds the owned rayfall function
 * VALUE and records its q-surface provenance.  The lexer derives its keyword
 * classification from the SAME manifest (q_lex_is_kw_infix), so the parser's
 * verb set and eval's verb set can never drift.
 *
 * Ground truth for the roster + the deferred glyphs lives in q_ops.c.  The
 * non-obvious mechanics the wrappers depend on:
 *   - rayfall `%` is MODULO and `/` is FLOAT-DIVIDE, so q `%` renames `/`.
 *   - q `#`/`_` cannot reuse rayfall `take`/`remove` verbatim (opposite arg
 *     order / dict-key semantics), so they are wrappers with the arg-swap /
 *     range-drop baked in.  q monadic `-` is negate (aliased to `neg`).
 *
 * LOWERING METADATA: each wrapper's rayfall aux-name is set to the CANONICAL
 * rayfall verb it lowers as (== != take drop) and it carries RAY_FN_Q_LOWER
 * (RAY_FN_Q_LOWER, lang/eval.h), so the compiler + query DAG dispatch on that name
 * (q `=`/`<>` hit ray_eq/ray_ne) instead of declining every non-canonical
 * value head to the eval fallback.  Pass-through/rename values ARE the env
 * builtin object, so they name-route via the existing canonical-identity path.
 *
 * REGISTRY-INIT PRECONDITION (codex #1): value embedding requires an
 * initialised registry, and a registry builder MUST NEVER call q_parse during
 * bootstrap (it would recurse into a not-yet-ready registry).  Builders here
 * only touch ray_env_get / ray_fn_* — never the parser.  A debug re-entry
 * guard (g_building) marks the bootstrap window and is the seam 2b's
 * parser-flip enforcement extends. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry.h"
#include "qlang/q_ops.h"      /* Q_OPS manifest, q_build_kind */
#include "qlang/q_apply.h"    /* q_apply_noun — @/. noun arms + carrier apply */
#include "qlang/q_deriv.h"    /* q_deriv_kind_of/base — glyph-verb carriers */
#include "lang/env.h"         /* ray_env_get, ray_fn_binary */
#include "lang/eval.h"        /* RAY_FN_* attrs, RAY_FN_Q_LOWER */
#include "lang/internal.h"    /* ray_where_fn, ray_group_fn (funsql executor) */
#include "table/sym.h"        /* ray_sym_vec_cell (funsql executor) */
#include "store/serde.h"      /* ray_serde_set_fn_hooks — wrapper round-trip */
#include "lang/format.h"      /* ray_type_name — wrapper error messages */
#include "mem/heap.h"         /* RAY_ATTR_HAS_NULLS — ? find miss remap */
#include <assert.h>
#include <math.h>     /* floor/floorf — q monadic `_` */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    int64_t     sym_id;
    q_valence_t valence;
    ray_t*      value;       /* owned (rc>=1) */
    /* provenance */
    const char* spelling;    /* q surface name (static, from the manifest) */
    const char* lower_name;  /* canonical rayfall routing name             */
    int         is_wrapper;
} entry_t;

/* Upper bound: every manifest row can contribute at most two entries. */
static entry_t g_entries[2 * 64];
static int     g_count    = 0;
static bool    g_inited   = false;
static bool    g_building = false;   /* debug re-entry guard (see header note) */

/* ---- wrappers (bespoke q semantics over a rayfall primitive) ---- */

/* q `n # list` — take.  rayfall ray_take_fn(vec, n) has the opposite arg
 * order, so swap.  Borrows both args (does not release them). */
static ray_t* q_take_wrap(ray_t* n, ray_t* list) {
    return ray_take_fn(list, n);
}

/* q `n _ list` — count-drop (NOT rayfall's dict key-remove).  n>=0 drops the
 * first n elements; n<0 drops the last |n|.  Implemented as a range-take
 * ray_take_fn(list, (start; amount)), which clamps at the ends.  Borrows args.
 *
 * Length is derived string-aware: a q string is a -RAY_STR atom whose char
 * count lives in ray_str_len, NOT the ->len union field (which aliases the SSO
 * {slen,sdata} bytes), so ray_len would be garbage for strings. */
static ray_t* q_drop_wrap(ray_t* n, ray_t* list) {
    /* q cut: int-VECTOR lhs — `2 4_v` slices [p0,p1) then [p_last,end).
     * Positions non-decreasing within 0..len; result is a boxed list of
     * slices (kdb 0h). */
    if (n && (n->type == RAY_I64 || n->type == RAY_I32 || n->type == RAY_I16)) {
        if (!list || (!ray_is_vec(list) && list->type != RAY_LIST))
            return ray_error("type", "_ (cut): expects a list rhs");
        int64_t len = ray_len(list), np = ray_len(n);
        ray_t* out = ray_list_new(np > 0 ? np : 1);
        int64_t prev = 0;
        for (int64_t i = 0; i < np; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* pe = ray_at_fn(n, ia);
            ray_release(ia);
            if (!pe || RAY_IS_ERR(pe)) { ray_release(out); return pe; }
            int64_t p = pe->i64;
            ray_release(pe);
            int64_t nxt = len;
            if (i + 1 < np) {
                ray_t* ja = ray_i64(i + 1);
                ray_t* ne = ray_at_fn(n, ja);
                ray_release(ja);
                if (!ne || RAY_IS_ERR(ne)) { ray_release(out); return ne; }
                nxt = ne->i64;
                ray_release(ne);
            }
            if (p < 0 || p > len || nxt < p || nxt > len || (i > 0 && p < prev)) {
                ray_release(out);
                return ray_error("domain",
                    "_ (cut): positions must be non-decreasing within 0..%lld",
                    (long long)len);
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
    if (!n || n->type != -RAY_I64)
        return ray_error("type", "_ (drop): count must be an integer");
    if (!list) return ray_error("type", "_ (drop): nil list");
    int64_t len;
    if (list->type == -RAY_STR)
        len = (int64_t)ray_str_len(list);           /* SSO-safe string length */
    else if (ray_is_vec(list) || list->type == RAY_LIST)
        len = ray_len(list);                         /* typed vector / boxed list */
    else
        return ray_error("type", "_ (drop): expects a list, vector, or string");
    int64_t k = n->i64;
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

/* q monadic `_` — floor to LONG (kdb `_ 3.7` is 3j; rayfall floor keeps f64).
 * Ints/bools pass through; f64 null -> long null.  RAY_FN_ATOMIC maps it
 * element-wise over float vectors. */
static ray_t* q_floor_wrap(ray_t* x) {
    if (!x) return ray_error("type", "_ (floor): nil");
    if (x->type == -RAY_F64) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)floor(x->f64));
    }
    if (x->type == -RAY_F32) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)floorf((float)x->f64));
    }
    if (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16 ||
        x->type == -RAY_BOOL) {
        ray_retain(x);
        return x;
    }
    return ray_error("type", "_ (floor): expects a numeric, got %s",
                     ray_type_name(x->type));
}

/* q char-string comparison — q treats a string as a char vector, so `=`/`<>`
 * compare element-wise and yield a boolean vector (`"abc"="abd"` -> 110b).
 * rayfall's `==`/`!=` (ray_eq_fn/ray_neq_fn) compare two -RAY_STR atoms as
 * whole values (a single 0b/1b), so the q verbs wrap them.  Two -RAY_STR
 * operands take the element-wise path here (equal length -> boolean vector,
 * unequal -> a q `length` error); everything else delegates to rayfall. */
static int q_is_str_atom(ray_t* x) { return x && x->type == -RAY_STR; }

static ray_t* q_str_cmp_vec(ray_t* a, ray_t* b, int eq) {
    const char* pa = ray_str_ptr(a); size_t la = ray_str_len(a);
    const char* pb = ray_str_ptr(b); size_t lb = ray_str_len(b);
    if (la != lb)
        return ray_error("length", "%s: string lengths must match, got %zu and %zu",
                         eq ? "=" : "<>", la, lb);
    uint8_t stack[128];
    uint8_t* bits = (la <= sizeof stack) ? stack : (uint8_t*)malloc(la ? la : 1);
    if (!bits) return ray_error("wsfull", "=: out of memory");
    for (size_t i = 0; i < la; i++)
        bits[i] = (uint8_t)(eq ? (pa[i] == pb[i]) : (pa[i] != pb[i]));
    ray_t* r = ray_vec_from_raw(RAY_BOOL, bits, (int64_t)la);
    if (bits != stack) free(bits);
    return r;
}

/* q `=` — element-wise over char strings, else rayfall `==`.  RAY_FN_ATOMIC so
 * eval broadcasts it over numeric vectors (each element pair hits ray_eq_fn). */
static ray_t* q_eq_wrap(ray_t* a, ray_t* b) {
    if (q_is_str_atom(a) && q_is_str_atom(b)) return q_str_cmp_vec(a, b, 1);
    return ray_eq_fn(a, b);
}

/* q `<>` — element-wise over char strings, else rayfall `!=`. */
static ray_t* q_ne_wrap(ray_t* a, ray_t* b) {
    if (q_is_str_atom(a) && q_is_str_atom(b)) return q_str_cmp_vec(a, b, 0);
    return ray_neq_fn(a, b);
}

/* q `x~y` — recursive whole-value equivalence (kdb match): TYPE-strict
 * (`1~1f` is 0b), attribute-blind (`1 2 3~\`s#1 2 3` is 1b), sentinel nulls
 * compare equal (`0n~0n` is 1b — non-finites canonicalize to one payload).
 * Unhandled types conservatively mismatch (kdb ~ never errors). */
static int q_match_rec(ray_t* a, ray_t* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->type != b->type) return 0;
    if (a->type == -RAY_SYM) return a->i64 == b->i64;
    if (a->type == -RAY_STR)
        return ray_str_len(a) == ray_str_len(b) &&
               memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) == 0;
    if (a->type == -RAY_GUID) {
        /* payload lives in a 16-byte U8 buffer behind the obj pointer —
         * an 8-byte union memcmp would compare POINTERS (codex P2). */
        return a->obj && b->obj &&
               memcmp(ray_data(a->obj), ray_data(b->obj), 16) == 0;
    }
    if (ray_is_atom(a)) {
        /* inline-payload scalars ONLY (ray_is_atom also covers LAMBDA and
         * fn values, whose state is NOT in the union slot — those fall to
         * the conservative-mismatch tail below). */
        switch (-a->type) {
        case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
        case RAY_F32: case RAY_F64:
        case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
            return memcmp(&a->i64, &b->i64, 8) == 0;   /* payload union */
        default:
            return 0;
        }
    }
    if (a->type == RAY_DICT || a->type == RAY_TABLE) {
        ray_t** ea = (ray_t**)ray_data(a);
        ray_t** eb = (ray_t**)ray_data(b);
        return q_match_rec(ea[0], eb[0]) && q_match_rec(ea[1], eb[1]);
    }
    if (a->type == RAY_LIST || ray_is_vec(a)) {
        int64_t la = ray_len(a);
        if (la != ray_len(b)) return 0;
        /* same-type numeric vectors: payload memcmp (nulls are in-payload
         * sentinels; attrs deliberately not compared).  SYM vecs vary in
         * index width -> per-element below. */
        if (ray_is_vec(a) && a->type != RAY_SYM && a->type != RAY_STR) {
            size_t esz = (a->type == RAY_I64 || a->type == RAY_F64) ? 8
                       : (a->type == RAY_I32 || a->type == RAY_F32) ? 4
                       : (a->type == RAY_I16) ? 2
                       : (a->type == RAY_BOOL || a->type == RAY_U8) ? 1 : 0;
            if (esz)
                return memcmp(ray_data(a), ray_data(b), (size_t)la * esz) == 0;
        }
        for (int64_t i = 0; i < la; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* xa = ray_at_fn(a, ia);
            ray_t* xb = ray_at_fn(b, ia);
            ray_release(ia);
            int r = (xa && xb && !RAY_IS_ERR(xa) && !RAY_IS_ERR(xb))
                        ? q_match_rec(xa, xb) : 0;
            if (xa) ray_release(xa);
            if (xb) ray_release(xb);
            if (!r) return 0;
        }
        return 1;
    }
    return 0;
}

static ray_t* q_match_wrap(ray_t* a, ray_t* b) {
    return ray_bool(q_match_rec(a, b));
}

/* Call the env-bound BINARY builtin `nm` (the wrapper-over-env pattern:
 * some base fns — dict — are declared only in internal base headers, so the
 * wrapper routes through the audited env value instead of a frozen-header
 * include).  Borrowed args; returns owned. */
static ray_t* q_env_call2(const char* nm, ray_t* a, ray_t* b) {
    ray_t* f = ray_env_get(ray_sym_intern(nm, strlen(nm)));
    if (!f || f->type != RAY_BINARY)
        return ray_error("type", "%s: env builtin missing", nm);
    return ((ray_binary_fn)(uintptr_t)f->i64)(a, b);
}

/* q `x!y` — dict make.  An atom key enlists to a 1-vector first (kdb `a!1`
 * is a dict too; rayfall dict wants vector keys); vals pass through as-is
 * (rayfall dict broadcasts atom vals and boxes the rest itself).  kdb
 * 'length fidelity: rayfall would null-fill a short vals side, so the
 * count check lives here.  String keys are a deferred cell (string model). */
static ray_t* q_bang_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "!: nil operand");
    ray_t* keys = x;
    ray_t* keys_owned = NULL;
    if (ray_is_atom(x)) {
        ray_t* l = ray_list_new(1);
        l = ray_list_append(l, x);
        if (RAY_IS_ERR(l)) return l;
        keys_owned = q_collapse_list(l);
        ray_release(l);
        if (!keys_owned || RAY_IS_ERR(keys_owned))
            return keys_owned ? keys_owned : ray_error("type", NULL);
        if (!ray_is_vec(keys_owned)) {          /* -RAY_STR & friends stay boxed */
            ray_release(keys_owned);
            return ray_error("type", "!: unsupported key type (deferred)");
        }
        keys = keys_owned;
    }
    if ((ray_is_vec(y) || y->type == RAY_LIST) &&
        (ray_is_vec(keys) || keys->type == RAY_LIST) &&
        ray_len(keys) != ray_len(y)) {
        if (keys_owned) ray_release(keys_owned);
        return ray_error("length", "!: key and value counts must match");
    }
    ray_t* r = q_env_call2("dict", keys, y);
    if (keys_owned) ray_release(keys_owned);
    return r;
}

/* q `key d` / monadic `!` — dict keys.  Non-dict operands (file handles,
 * namespaces, enumerations, vectors) are deferred cells: error, never a
 * wrong answer. */
static ray_t* q_key_wrap(ray_t* x) {
    if (!x || x->type != RAY_DICT)
        return ray_error("type", "key: expects a dict (other forms deferred)");
    ray_t* k = ray_dict_keys(x);                /* borrowed */
    if (!k) return ray_error("type", "key: nil keys");
    ray_retain(k);
    return k;
}

/* q `value d` — dict values, collapsed to a simple vector where homogeneous
 * (rayfall's dict stores vals as a boxed list).  Non-dict operands (symbol
 * name resolution, enumerations, string eval, lambdas) are deferred cells. */
static ray_t* q_value_wrap(ray_t* x) {
    if (!x || x->type != RAY_DICT)
        return ray_error("type", "value: expects a dict (other forms deferred)");
    ray_t* v = ray_dict_vals(x);                /* borrowed */
    if (!v) return ray_error("type", "value: nil vals");
    if (v->type == RAY_LIST) return q_collapse_list(v);   /* returns owned */
    ray_retain(v);
    return v;
}

/* q `distinct x` / monadic `?` — unique items in FIRST-OCCURRENCE order
 * (kdb).  rayfall's distinct routes typed vectors through the DAG group
 * path, which SORTS — a rename would pin wrong answers, so this is a
 * match-based dedup (type-strict, nulls equal — the ~ semantics kdb's
 * distinct uses), collapsed back to a typed vector.  String operands are
 * a deferred cell (string model); atoms are kdb 'type. */
static ray_t* q_distinct_wrap(ray_t* x) {
    if (!x) return ray_error("type", "distinct: nil");
    if (x->type == -RAY_STR)
        return ray_error("nyi", "distinct: string operand deferred (string model)");
    if (!ray_is_vec(x) && x->type != RAY_LIST)
        return ray_error("type", "distinct: expects a list, got %s",
                         ray_type_name(x->type));
    int64_t n = ray_len(x);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(x, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
        int dup = 0;
        int64_t m = ray_len(out);
        ray_t** oe = (ray_t**)ray_data(out);
        for (int64_t j = 0; j < m && !dup; j++) dup = q_match_rec(oe[j], e);
        if (!dup) {
            out = ray_list_append(out, e);
            if (RAY_IS_ERR(out)) { ray_release(e); return out; }
        }
        ray_release(e);
    }
    ray_t* c = q_collapse_list(out);
    ray_release(out);
    return c;
}

/* q `x?y` — find / roll / pick (type-dispatch on the operands).
 *   list ? y   -> find.  kdb miss semantics: the smallest index NOT in the
 *                 list, i.e. `count x` — rayfall find returns 0N on a miss
 *                 (atom result) or per-element 0N (vector needle), so both
 *                 shapes are remapped to count here.
 *   n ? int    -> roll: n randoms in [0,int)  (rayfall rand)
 *   n ? list   -> pick: n random indices gathered from the list
 *   -n ? m (deal / 0N?m permute), n ? float — DEFERRED cells (no rayfall
 *   support; error, never a wrong answer). */
static ray_t* q_roll_wrap(ray_t* x, ray_t* y) {
    if (x && (ray_is_vec(x) || x->type == RAY_LIST)) {          /* find */
        int64_t cnt = ray_len(x);
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
    if (x && (x->type == -RAY_I64 || x->type == -RAY_I32)) {
        int64_t nx = (x->type == -RAY_I64) ? x->i64 : x->i32;
        if (RAY_ATOM_IS_NULL(x))
            return ray_error("nyi", "?: permute (0N?y) is deferred");
        if (nx < 0)
            return ray_error("nyi", "?: deal (without replacement) is deferred");
        if (y && (y->type == -RAY_I64 || y->type == -RAY_I32))  /* roll */
            return q_env_call2("rand", x, y);
        if (y && (ray_is_vec(y) || y->type == RAY_LIST)) {      /* pick */
            ray_t* len = ray_i64(ray_len(y));
            ray_t* idx = q_env_call2("rand", x, len);
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
        if (y && (y->type == -RAY_F64 || y->type == -RAY_F32))
            return ray_error("nyi", "?: float roll is deferred");
        return ray_error("nyi", "?: right operand form is deferred");
    }
    return ray_error("type", "?: unsupported operand types");
}

/* q `t$x` — cast.  t is a sym designator (`long`float`int`short`boolean)
 * or a lower-case single-char string ("j" "f" "i" "h" "b"); maps to the
 * rayfall `as` type sym.  Returns NULL for unknown/deferred designators
 * (`real/"e" — no rayfall F32 cast arm; char/byte/guid/temporals). */
static const char* q_cast_target(ray_t* t) {
    char c = 0;
    if (t && t->type == -RAY_STR && ray_str_len(t) == 1) {
        c = ray_str_ptr(t)[0];
    } else if (t && t->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(t->i64);
        if (!s) return NULL;
        const char* nm = ray_str_ptr(s);
        size_t l = ray_str_len(s);
        const char* r = NULL;
        if      (l == 4 && !memcmp(nm, "long",    4)) r = "I64";
        else if (l == 5 && !memcmp(nm, "float",   5)) r = "F64";
        else if (l == 3 && !memcmp(nm, "int",     3)) r = "I32";
        else if (l == 5 && !memcmp(nm, "short",   5)) r = "I16";
        else if (l == 7 && !memcmp(nm, "boolean", 7)) r = "BOOL";
        ray_release(s);
        return r;
    }
    switch (c) {
    case 'j': return "I64";  case 'f': return "F64";  case 'i': return "I32";
    case 'h': return "I16";  case 'b': return "BOOL";
    default:  return NULL;
    }
}

/* kdb float->integer casts ROUND (half-to-even: `long$3.7 is 4, "j"$2.5 is
 * 2 — the KX ref pins `int$6.6 -> 7), where rayfall `as` truncates — so the
 * integer targets pre-round here (rint = IEEE nearest/ties-even, kdb's
 * mode); everything else delegates to ray_cast_fn.  Upper-case designators
 * are Tok string-parses (ref/tok.md) — deferred on the string model. */
static ray_t* q_cast_wrap(ray_t* t, ray_t* x) {
    if (t && t->type == -RAY_STR && ray_str_len(t) == 1 &&
        ray_str_ptr(t)[0] >= 'A' && ray_str_ptr(t)[0] <= 'Z')
        return ray_error("nyi", "$: string-parse casts (Tok) are deferred");
    const char* tgt = q_cast_target(t);
    if (!tgt)
        return ray_error("nyi", "$: unsupported cast designator (deferred)");
    int8_t it = 0;
    if      (!strcmp(tgt, "I64")) it = RAY_I64;
    else if (!strcmp(tgt, "I32")) it = RAY_I32;
    else if (!strcmp(tgt, "I16")) it = RAY_I16;
    if (it && x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null((int8_t)-it);
        double r = rint(x->f64);              /* F32 atoms store f64 payload */
        if (it == RAY_I64) return ray_i64((int64_t)r);
        if (it == RAY_I32) return ray_i32((int32_t)r);
        return ray_i16((int16_t)r);
    }
    if (it && x && (x->type == RAY_F64 || x->type == RAY_F32)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(it, n);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        int is64 = (x->type == RAY_F64);
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(x))[i]
                            : (double)((const float*)ray_data(x))[i];
            int isnull = isnan(v);
            int64_t iv = isnull ? 0 : (int64_t)rint(v);
            if      (it == RAY_I64) ((int64_t*)ray_data(out))[i] = iv;
            else if (it == RAY_I32) ((int32_t*)ray_data(out))[i] = (int32_t)iv;
            else                    ((int16_t*)ray_data(out))[i] = (int16_t)iv;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    ray_t* ts = ray_sym(ray_sym_intern(tgt, strlen(tgt)));
    if (!ts || RAY_IS_ERR(ts)) return ts;
    ray_t* r = ray_cast_fn(ts, x);
    ray_release(ts);
    return r;
}

/* q `f@x` — Apply At / Index At (ref/apply.md).  A callable f invokes with
 * the single argument; everything else (vector, list, dict, table, 104h
 * carrier) delegates to q_apply_noun — identical semantics to `f[x]`/`f x`.
 * Special forms need UNEVALUATED args, which are already gone here -> error.
 * Ternary Trap `@[f;fx;e]` / Amend are deferred cells (arity error today). */
static ray_t* q_at_wrap(ray_t* f, ray_t* x) {
    if (f && (f->type == RAY_UNARY || f->type == RAY_BINARY || f->type == RAY_VARY)
          && (f->attrs & RAY_FN_SPECIAL_FORM))
        return ray_error("type", "@: special forms cannot be applied");
    if (f && f->type == RAY_UNARY)
        return ((ray_unary_fn)(uintptr_t)f->i64)(x);
    if (f && f->type == RAY_VARY) {
        ray_t* one[1] = { x };
        return ((ray_vary_fn)(uintptr_t)f->i64)(one, 1);
    }
    if (f && f->type == RAY_BINARY)
        return ray_error("rank", "@: unary application of a binary verb");
    ray_t* args[1] = { x };
    ray_t* r = q_apply_noun(f, args, 1);
    return r ? r : ray_error("type", "@: not applicable");
}

/* q `v . vx` — Apply / Index (ref/apply.md): the rhs is the ARGUMENT LIST —
 * a rank-n callable spread-calls over vx's n items; a noun depth-indexes
 * (m . 1 2 is m[1;2]).  Atom rhs is not a list -> 'type (kdb wants a list).
 * Ternary Trap `.[g;gx;e]` / Amend are deferred cells (arity error today). */
static ray_t* q_dot_wrap(ray_t* f, ray_t* a) {
    if (!a || (!ray_is_vec(a) && a->type != RAY_LIST))
        return ray_error("type", ".: rhs must be an argument list");
    if (f && (f->type == RAY_UNARY || f->type == RAY_BINARY || f->type == RAY_VARY)
          && (f->attrs & RAY_FN_SPECIAL_FORM))
        return ray_error("type", ".: special forms cannot be applied");
    int64_t n = ray_len(a);
    if (n < 1 || n > 8) return ray_error("rank", ".: 1..8 arguments");
    ray_t* args[8];
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        args[i] = ray_at_fn(a, ia);
        ray_release(ia);
        if (!args[i] || RAY_IS_ERR(args[i])) {
            ray_t* err = args[i];
            for (int64_t j = 0; j < i; j++) ray_release(args[j]);
            return err ? err : ray_error("type", ".: bad argument list");
        }
    }
    ray_t* r;
    if (f && f->type == RAY_UNARY && n == 1)
        r = ((ray_unary_fn)(uintptr_t)f->i64)(args[0]);
    else if (f && f->type == RAY_BINARY && n == 2)
        r = ((ray_binary_fn)(uintptr_t)f->i64)(args[0], args[1]);
    else if (f && f->type == RAY_VARY)
        r = ((ray_vary_fn)(uintptr_t)f->i64)(args, n);
    else if (f && (f->type == RAY_UNARY || f->type == RAY_BINARY))
        r = ray_error("rank", ".: argument count does not match the verb's rank");
    else {
        r = q_apply_noun(f, args, n);
        if (!r) r = ray_error("type", ".: not applicable");
    }
    for (int64_t j = 0; j < n; j++) ray_release(args[j]);
    return r;
}

/* q `f each x` — rayfall map, then collapse the boxed result to a simple
 * vector (kdb: `neg each 1 2 3` is -1 -2 -3, type 7h, not a general list). */
static ray_t* q_each_wrap(ray_t* f, ray_t* x) {
    ray_t* args[2] = { f, x };
    ray_t* r = ray_map_fn(args, 2);
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* c = q_collapse_list(r);
    ray_release(r);
    return c;
}

/* q `(f\)x` — rayfall scan, then collapse (kdb: `(+\)1 2 3` is 1 3 6, a
 * simple vector).  Vary so a future seeded form passes through unchanged.
 * Not a manifest row (no q spelling of its own): an internal value handed
 * out to q_lower via q_registry_scan_value(). */
static ray_t* q_scan_wrap(ray_t** args, int64_t n) {
    ray_t* r = ray_scan_fn(args, n);
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* c = q_collapse_list(r);
    ray_release(r);
    return c;
}

static ray_t* g_scan_value = NULL;

ray_t* q_registry_scan_value(void) {
    return g_scan_value;   /* borrowed; NULL before init */
}

/* ---- shared right-to-left CONTEXT builder (list + table def) --------------
 *
 * THE MANDATE (specs/2026-07-04-table-def.md): list definition `(…)` and table
 * definition `([] …)` are ONE mechanism, not two.  Evaluating either opens an
 * env scope, processes the element expressions RIGHT-TO-LEFT (so `x:e` binds x
 * INTO the scope before a leftward bare `x` RESOLVES from it — this is WHY
 * `(aa; aa:11 12 13)` yields the value twice and `(bb:11 12 13; bb)` errors
 * `'bb`), then pops the scope and assembles: `(…)` → the list of element values
 * (then the existing collapse-to-vector); `([] …)` → a table whose columns are
 * the per-element assignment targets.
 *
 * Both constructor heads are RAY_FN_SPECIAL_FORM: rayforce's VARY arg-eval is
 * LEFT-to-right, and a special form is the only seam that hands a builtin the
 * raw (unevaluated) element trees — a hard prerequisite for right-to-left.
 * q_lower lowers a plain `:` inside a ctx literal to `let` (writes the pushed
 * frame), so the assignments are scoped, not leaked to the global env. */

/* If `el` is a lowered assignment node `(set/let name val)`, return the target
 * sym-id; else if `el` is a bare unquoted name-ref sym, return its id (a bare
 * column reference); else -1 (no column name — a table error, .Q.id deferred). */
static int64_t q_ctx_colname(ray_t* el) {
    if (!el) return -1;
    if (el->type == -RAY_SYM && !(el->attrs & 0x20 /* Q_ATTR_QUOTED */))
        return el->i64;
    if (el->type == RAY_LIST && ray_len(el) == 3) {
        ray_t** e = (ray_t**)ray_data(el);
        ray_t* h = e[0];
        if (h && h->type == RAY_BINARY) {
            const char* nm = ray_fn_name(h);
            if (nm && (strcmp(nm, "set") == 0 || strcmp(nm, "let") == 0) &&
                e[1] && e[1]->type == -RAY_SYM && !(e[1]->attrs & 0x20))
                return e[1]->i64;
        }
    }
    return -1;
}

/* The shared builder.  `elems[0..n)` are the UNEVALUATED element trees. */
static ray_t* q_ctx_build(ray_t** elems, int64_t n, int as_table) {
    if (ray_env_push_scope() != RAY_OK) return ray_error("oom", NULL);

    ray_t**  vals  = (n > 0) ? (ray_t**)calloc((size_t)n, sizeof(ray_t*))   : NULL;
    int64_t* names = (as_table && n > 0)
                        ? (int64_t*)malloc((size_t)n * sizeof(int64_t)) : NULL;
    if (n > 0 && (!vals || (as_table && !names))) {
        free(vals); free(names); ray_env_pop_scope();
        return ray_error("wsfull", "ctx: out of memory");
    }

    ray_t* err = NULL;
    for (int64_t i = n - 1; i >= 0; i--) {
        if (as_table) names[i] = q_ctx_colname(elems[i]);
        ray_t* v = ray_eval(elems[i]);            /* binds `x:e` into the scope */
        if (!v) v = ray_error("type", "ctx: null element");
        if (RAY_IS_ERR(v)) { err = v; break; }
        vals[i] = v;
    }

    ray_env_pop_scope();

    if (err) {
        for (int64_t i = 0; i < n; i++) if (vals[i]) ray_release(vals[i]);
        free(vals); free(names);
        return err;
    }

    ray_t* out;
    if (!as_table) {
        ray_t* l = ray_list_fn(vals, n);          /* borrows; retains each */
        if (l && !RAY_IS_ERR(l)) { out = q_collapse_list(l); ray_release(l); }
        else                     { out = l; }
    } else {
        /* Row count comes from the vector/list columns, which must all share one
         * length (mismatch -> 'length).  A scalar-atom column BROADCASTS to that
         * length (kdb: `([]a:1 2 3;b:0)` -> b is 0 0 0); all-scalar -> 1 row. */
        int64_t nrows = -1;
        ray_t* err2 = NULL;
        for (int64_t i = 0; i < n; i++) {
            ray_t* col = vals[i];
            if (col && (ray_is_vec(col) || col->type == RAY_LIST)) {
                int64_t clen = ray_len(col);
                if (nrows < 0) nrows = clen;
                else if (clen != nrows) {
                    err2 = ray_error("length", "([]…): column length mismatch");
                    break;
                }
            } else if (!col || col->type >= 0) {
                err2 = ray_error("type", "([]…): column must be a vector or list");
                break;
            }
        }
        if (err2) { out = err2; }
        else {
            if (nrows < 0) nrows = 1;
            out = ray_table_new(n);
            for (int64_t i = 0; i < n && !RAY_IS_ERR(out); i++) {
                if (names[i] < 0) {
                    ray_release(out);
                    out = ray_error("type",
                        "([]…): every column needs a name (a:… ; bare/.Q.id deferred)");
                    break;
                }
                ray_t* col = vals[i]; int owned = 0;
                if (col && col->type < 0) {              /* scalar -> broadcast */
                    ray_t* nn = ray_i64(nrows);
                    col = ray_take_fn(col, nn); ray_release(nn); owned = 1;
                    if (!col || RAY_IS_ERR(col)) {
                        ray_release(out); out = col ? col : ray_error("type", "broadcast");
                        break;
                    }
                }
                out = ray_table_add_col(out, names[i], col);
                if (owned) ray_release(col);
            }
        }
    }

    for (int64_t i = 0; i < n; i++) if (vals[i]) ray_release(vals[i]);
    free(vals); free(names);
    return out;
}

/* q paren-list literal `(1;2;3)` head — see q_ctx_build.  The parser embeds
 * this value at the head of every multi-element paren list, which is what
 * DISAMBIGUATES a literal from the shape-identical bracket-index call (v;i) —
 * the distinction only exists at parse time. */
static ray_t* q_list_build(ray_t** args, int64_t n) {
    return q_ctx_build(args, n, 0);
}

/* q table literal `([] a:…; b:…)` head — see q_ctx_build. */
static ray_t* q_table_build(ray_t** args, int64_t n) {
    return q_ctx_build(args, n, 1);
}

/* q qSQL SELECT adapter — lowers the functional 5-list (?;`t;c;b;a) (emitted by
 * BOTH the string form `select…from t` AND `?[t;c;b;a]`) onto the base
 * ray_select engine.  q_lower hands it a fully-built rayfall query dict plus the
 * by-group key column names; this special form calls ray_select and, for a
 * by-group query, splits the flat result into a KEYED table — a dict from the
 * key-columns table to the value-columns table (the mandate: "a keyed table is
 * just a dictionary from one table to another"), which q_fmt renders `k| v`.
 *   args[0] = query dict (unevaluated — ray_select owns clause-in-scope eval)
 *   args[1] = by-key column-name sym vector (empty => unkeyed passthrough) */
static ray_t* q_select_exec(ray_t** args, int64_t n) {
    ray_t* dict = args[0];
    ray_t* res  = ray_select(&dict, 1);
    if (!res || RAY_IS_ERR(res)) return res;

    /* Rename the temp-named output columns (__qcN) back to their real q names
     * (args[2]=temp sym vec, args[3]=real sym vec, parallel). */
    if (n >= 4 && res->type == RAY_TABLE &&
        args[2] && args[2]->type == RAY_SYM && args[3] && args[3]->type == RAY_SYM) {
        int64_t nt = ray_len(args[2]);
        int64_t nc = ray_table_ncols(res);
        for (int64_t i = 0; i < nt; i++) {
            ray_t* ti = ray_i64(i);
            ray_t* ta = ray_at_fn(args[2], ti);
            ray_t* ra = ray_at_fn(args[3], ti);
            ray_release(ti);
            int64_t tid = (ta && ta->type == -RAY_SYM) ? ta->i64 : -1;
            int64_t rid = (ra && ra->type == -RAY_SYM) ? ra->i64 : -1;
            if (ta) ray_release(ta);
            if (ra) ray_release(ra);
            for (int64_t c = 0; c < nc; c++)
                if (ray_table_col_name(res, c) == tid) {
                    ray_table_set_col_name(res, c, rid); break;
                }
        }
    }

    ray_t* keys = (n >= 2) ? args[1] : NULL;
    int64_t nk = (keys && keys->type == RAY_SYM) ? ray_len(keys) : 0;
    if (nk == 0 || res->type != RAY_TABLE) return res;

    int64_t kids[64];
    if (nk > 64) nk = 64;
    for (int64_t k = 0; k < nk; k++) {
        ray_t* ia = ray_i64(k);
        ray_t* ka = ray_at_fn(keys, ia);
        ray_release(ia);
        kids[k] = (ka && ka->type == -RAY_SYM) ? ka->i64 : -1;
        if (ka) ray_release(ka);
    }

    int64_t nc = ray_table_ncols(res);
    ray_t* kt = ray_table_new(nk);
    ray_t* vt = ray_table_new(nc - nk > 0 ? nc - nk : 1);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(kt) && !RAY_IS_ERR(vt); c++) {
        int64_t nm  = ray_table_col_name(res, c);
        ray_t*  col = ray_table_get_col_idx(res, c);
        int iskey = 0;
        for (int64_t k = 0; k < nk; k++) if (kids[k] == nm) { iskey = 1; break; }
        if (iskey) kt = ray_table_add_col(kt, nm, col);
        else       vt = ray_table_add_col(vt, nm, col);
    }
    ray_release(res);
    if (RAY_IS_ERR(kt)) { ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);   /* consumes kt, vt */
}

/* ===== functional qSQL executor (piece 3) =================================
 * Runtime evaluators for the by-VALUE functional forms `?[t;c;b;a]` (select)
 * and `![t;c;b;a]` (update/delete).  q_lower embeds g_funsql_select_value /
 * g_funsql_bang_value at the head of a rank-4 `?`/`!` application; ray_eval
 * evaluates the four operands (t→table, c/b/a→list/dict/sym VALUES) and calls
 * the executor below.  Reuse-first: WHERE is evaluated against the live table
 * columns (handles `in`, `=`, comparisons uniformly, incl. the enlist-a-symbol
 * -constant convention); BY + aggregates + column projection are delegated to
 * the base engine (q_select_exec → ray_select; ray_update for update-by).  See
 * qdocs/.../basics/funsql.md for the observable contract. */

/* Append one interned symbol into a RAY_SYM (W64) vector — local twin of the
 * q_parse helper (that one is file-static there). */
static ray_t* rsymvec_append(ray_t* vec, const char* s, int len) {
    int64_t id = ray_sym_intern_runtime(s, (size_t)len);
    return ray_vec_append(vec, &id);
}

/* A verb usable at a constraint/expression head: a plain fn value, OR a 104h
 * derived-verb carrier (glyph verbs like `>` `*` lower to a projection carrier
 * in non-head list positions — see q_deriv.h / ql_deriv_value). */
/* Multi-column ascending grade (permutation) — defined in src/ops/sort.c; no
 * public header exposes it, so forward-declare for the by-group key sort. */
ray_t* ray_sort_indices(ray_t** cols, uint8_t* descs, uint8_t* nulls_first,
                        uint8_t n_cols, int64_t nrows);

static int funsql_is_fn(const ray_t* x) {
    if (!x) return 0;
    if (x->type == RAY_UNARY || x->type == RAY_BINARY || x->type == RAY_VARY) return 1;
    if (x->type == RAY_LIST && q_deriv_kind_of(x) != Q_DERIV_NONE) return 1;
    return 0;
}

/* The rayfall routing name of a verb head (plain value or carrier), or NULL. */
static const char* funsql_head_name(ray_t* fn) {
    if (fn->type == RAY_LIST && q_deriv_kind_of(fn) != Q_DERIV_NONE) {
        ray_t* base = q_deriv_base(fn);
        return base ? ray_fn_name(base) : NULL;
    }
    return ray_fn_name(fn);
}

/* Call a verb head on already-evaluated args (no re-eval, so a resolved
 * -RAY_SYM constant is NOT looked up as a variable).  Routes through the same
 * call_fn1/call_fn2 dispatch the evaluator uses — this applies the ATOMIC
 * broadcast (a scalar `>`/`=` RHS against a column vector) and lazy
 * materialisation that a raw kernel-pointer call would skip.  Carriers dispatch
 * through the shared q applicator. */
static ray_t* funsql_call(ray_t* fn, ray_t** a, int64_t n) {
    if (fn->type == RAY_LIST && q_deriv_kind_of(fn) != Q_DERIV_NONE)
        return q_apply_noun(fn, a, n);
    if (n == 1) return call_fn1(fn, a[0]);
    if (n == 2) return call_fn2(fn, a[0], a[1]);
    if (fn->type == RAY_VARY) return ((ray_vary_fn)(uintptr_t)fn->i64)(a, n);
    return ray_error("rank", "funsql: verb applied to %lld args", (long long)n);
}

/* Is x an empty operand — the general null `::`, C-NULL, or a 0-length vec/list?
 * The functional degenerate forms use `()` (→ `::`) for "no constraints" / "all
 * columns", and `0b` for "no grouping". */
static int funsql_empty(const ray_t* x) {
    if (!x) return 1;
    if (x->type == RAY_NULL) return 1;
    if (x->type == RAY_LIST || ray_is_vec(x)) return ray_len((ray_t*)x) == 0;
    return 0;
}

/* Evaluate a where-constraint parse-tree (a VALUE) against the table columns.
 *   -RAY_SYM atom        -> the named column (kdb: a bare symbol IS a column ref)
 *   (fn; arg; …)         -> resolve each arg, apply fn (yields a boolean vector)
 *   enlist X (1-list/vec)-> the symbol CONSTANT X (un-enlisted)
 *   literal              -> itself
 * Returns an OWNED value (or owned error). */
static ray_t* funsql_eval(ray_t* x, ray_t* tbl) {
    if (!x) return ray_error("domain", "funsql: null expression");
    if (x->type == -RAY_SYM) {
        ray_t* col = ray_table_get_col(tbl, x->i64);
        if (col) { ray_retain(col); return col; }
        ray_retain(x); return x;                  /* names no column -> literal sym */
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (n >= 1 && funsql_is_fn(e[0])) {
            int64_t na = n - 1;
            if (na < 1 || na > 7)
                return ray_error("rank", "funsql: constraint arity %lld", (long long)na);
            ray_t* av[7];
            for (int64_t i = 0; i < na; i++) {
                av[i] = funsql_eval(e[i + 1], tbl);
                if (!av[i] || RAY_IS_ERR(av[i])) {
                    for (int64_t j = 0; j < i; j++) ray_release(av[j]);
                    return av[i] ? av[i] : ray_error("domain", "funsql: constraint arg");
                }
            }
            ray_t* r = funsql_call(e[0], av, na);
            for (int64_t i = 0; i < na; i++) ray_release(av[i]);
            return r;
        }
        if (n == 1) { ray_retain(e[0]); return e[0]; }   /* enlist X -> X */
        ray_retain(x); return x;
    }
    if (x->type == RAY_SYM && ray_len(x) == 1) {         /* enlist `sym -> `sym */
        ray_t* i0 = ray_i64(0);
        ray_t* r = ray_at_fn(x, i0);
        ray_release(i0);
        return r;
    }
    ray_retain(x); return x;
}

/* Combine the where-constraints into one boolean mask (left-to-right AND), or
 * NULL when there are no constraints.  `c` is a list of constraints; a single
 * unwrapped constraint (function head) is accepted too. */
static ray_t* funsql_build_mask(ray_t* tbl, ray_t* c) {
    if (funsql_empty(c)) return NULL;
    ray_t* one[1];
    ray_t** cons; int64_t nc;
    if (c->type == RAY_LIST) {
        ray_t** e = (ray_t**)ray_data(c);
        int64_t n = ray_len(c);
        if (n >= 1 && funsql_is_fn(e[0])) { one[0] = c; cons = one; nc = 1; }
        else { cons = e; nc = n; }
    } else { one[0] = c; cons = one; nc = 1; }
    ray_t* mask = NULL;
    for (int64_t i = 0; i < nc; i++) {
        ray_t* m = funsql_eval(cons[i], tbl);
        if (!m || RAY_IS_ERR(m)) { if (mask) ray_release(mask); return m ? m : ray_error("domain", "funsql where"); }
        if (!mask) mask = m;
        else {
            ray_t* a = ray_and_fn(mask, m);
            ray_release(mask); ray_release(m);
            mask = a;
            if (!mask || RAY_IS_ERR(mask)) return mask ? mask : ray_error("oom", NULL);
        }
    }
    return mask;
}

/* Filter table rows by the where-constraints.  keep=1 selects matching rows
 * (select/exec/update); keep=0 selects the complement (delete rows).  Returns
 * a retained `tbl` unchanged when there are no constraints. */
static ray_t* funsql_filter(ray_t* tbl, ray_t* c, int keep) {
    ray_t* mask = funsql_build_mask(tbl, c);
    if (!mask) { ray_retain(tbl); return tbl; }
    if (RAY_IS_ERR(mask)) return mask;
    ray_t* use = mask;
    if (!keep) {
        use = ray_not_fn(mask);
        ray_release(mask);
        if (!use || RAY_IS_ERR(use)) return use;
    }
    ray_t* idx = ray_where_fn(use);
    ray_release(use);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("domain", "funsql where");
    ray_t* ft = ray_at_fn(tbl, idx);
    ray_release(idx);
    return ft;
}

/* Lower a by/select expression VALUE into the AST shape the base query engine
 * consumes: a function-VALUE head becomes a bare -RAY_SYM (ray_select /
 * ray_update key their agg/predicate dispatch on the rayfall routing name), a
 * column-ref symbol becomes a bare name-ref, an enlisted constant is passed
 * through.  Returns a fresh OWNED tree. */
static ray_t* funsql_lower_expr(ray_t* x) {
    if (!x) return NULL;
    if (x->type == -RAY_SYM) return ray_sym(x->i64);        /* bare name-ref */
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (!(n >= 1 && funsql_is_fn(e[0]))) { ray_retain(x); return x; }  /* constant */
        ray_t* out = ray_list_new(n);
        for (int64_t i = 0; i < n; i++) {
            ray_t* c;
            if (i == 0) {
                const char* nm = funsql_head_name(e[0]);
                if (nm && nm[0]) c = ray_sym(ray_sym_intern_runtime(nm, strlen(nm)));
                else { ray_retain(e[0]); c = e[0]; }
            } else {
                c = funsql_lower_expr(e[i]);
            }
            if (!c || RAY_IS_ERR(c)) { ray_release(out); return c ? c : ray_error("domain", "funsql expr"); }
            out = ray_list_append(out, c);
            ray_release(c);
        }
        return out;
    }
    ray_retain(x); return x;
}

/* Resolve the `t` operand to a live table (a bare symbol names a table var). */
static ray_t* funsql_resolve_table(ray_t* t) {
    ray_t* tbl = t;
    if (t && t->type == -RAY_SYM) {
        tbl = ray_env_get(t->i64);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", "funsql: table symbol is unbound");
    }
    if (!tbl || tbl->type != RAY_TABLE)
        return ray_error("type", "funsql: `t` must be a table");
    ray_retain(tbl);
    return tbl;
}

/* Append a `by` clause + its key-column names for a `name!col` By-DICT (the
 * only By-phrase Select supports here — symbol By-phrases are grouped-exec and
 * refused upstream).  Values may be a SYM vector (plain column names) or a
 * general list (parse-tree expressions).  Fills the base-query by-dict into
 * (*keyvec,*vallist) and records the key names into *keycols. */
static void funsql_add_by(ray_t* b, ray_t** keyvec, ray_t** vallist, ray_t** keycols) {
    if (!b || b->type != RAY_DICT) return;
    ray_t* bk = ray_dict_keys(b);
    ray_t* bv = ray_dict_vals(b);
    int64_t nb = ray_len(bk);
    int bv_sym = (bv && bv->type == RAY_SYM);
    ray_t* bkeys = ray_sym_vec_new(RAY_SYM_W64, nb);
    ray_t* bvals = ray_list_new(nb);
    for (int64_t i = 0; i < nb; i++) {
        ray_t* kn = ray_sym_vec_cell(bk, i);
        bkeys    = rsymvec_append(bkeys, ray_str_ptr(kn), (int)ray_str_len(kn));
        *keycols = rsymvec_append(*keycols, ray_str_ptr(kn), (int)ray_str_len(kn));
        ray_t* ex;
        if (bv_sym) {
            ray_t* vn = ray_sym_vec_cell(bv, i);
            ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
        } else {
            ex = funsql_lower_expr(((ray_t**)ray_data(bv))[i]);
        }
        bvals = ray_list_append(bvals, ex);
        ray_release(ex);
    }
    ray_t* bydict = ray_dict_new(bkeys, bvals);
    *keyvec  = rsymvec_append(*keyvec, "by", 2);
    *vallist = ray_list_append(*vallist, bydict);
    ray_release(bydict);
}

/* Evaluate one functional operand.  These are SPECIAL FORMs (operands arrive
 * unevaluated) so the empty-list marker `()` — which the parser lowers to the
 * `::` name-ref and which ray_eval would reject as an unbound name — maps to
 * the generic null here; everything else (name-refs, list/dict literals, `0b`,
 * `` `sym$() ``) evaluates normally. */
static ray_t* funsql_operand(ray_t* x) {
    if (!x) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        int is_null = s && ray_str_len(s) == 2 && memcmp(ray_str_ptr(s), "::", 2) == 0;
        if (s) ray_release(s);
        if (is_null) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    }
    return ray_eval(x);
}

/* Sort a by-group keyed table ascending by its key column(s) — kdb's `by`
 * orders groups by key, whereas the base group-by emits in first-appearance
 * order.  `dict` is a keyed table (RAY_DICT whose keys/vals are tables).
 * Consumes `dict`; returns the sorted keyed table (or `dict` unchanged on any
 * non-keyed shape / sort failure). */
static ray_t* funsql_sort_keyed(ray_t* dict) {
    if (!dict || dict->type != RAY_DICT) return dict;
    ray_t* kt = ray_dict_keys(dict);
    ray_t* vt = ray_dict_vals(dict);
    if (!kt || kt->type != RAY_TABLE || !vt || vt->type != RAY_TABLE) return dict;
    int64_t nrows = ray_table_nrows(kt);
    int64_t nk = ray_table_ncols(kt);
    if (nrows <= 1 || nk < 1 || nk > 16) return dict;
    ray_t* cols[16];
    uint8_t descs[16] = {0};
    for (int64_t j = 0; j < nk; j++) cols[j] = ray_table_get_col_idx(kt, j);
    ray_t* perm = ray_sort_indices(cols, descs, NULL, (uint8_t)nk, nrows);
    if (!perm || RAY_IS_ERR(perm)) { if (perm) ray_release(perm); return dict; }
    ray_t* kt2 = ray_at_fn(kt, perm);
    ray_t* vt2 = ray_at_fn(vt, perm);
    ray_release(perm);
    if (!kt2 || RAY_IS_ERR(kt2) || !vt2 || RAY_IS_ERR(vt2)) {
        if (kt2) ray_release(kt2);
        if (vt2) ray_release(vt2);
        return dict;
    }
    ray_t* nd = ray_dict_new(kt2, vt2);   /* consumes kt2, vt2 */
    if (!nd || RAY_IS_ERR(nd)) return dict;
    ray_release(dict);
    return nd;
}

/* `?[t;c;b;a]` select — returns a table (or a keyed table for a by-group). */
static ray_t* q_funsql_select_impl(ray_t* t, ray_t* c, ray_t* b, ray_t* a) {
    ray_t* tbl = funsql_resolve_table(t);
    if (RAY_IS_ERR(tbl)) return tbl;

    ray_t* ft = funsql_filter(tbl, c, 1);
    ray_release(tbl);
    if (!ft || RAY_IS_ERR(ft)) return ft;

    /* EXEC forms — signalled by the By-phrase being the GENERAL EMPTY LIST `()`
     * (parser `::`/RAY_NULL), as opposed to `0b` (RAY_BOOL) which is Select's
     * "no grouping".  Result is a vector (a is a column/parse-tree), a
     * dictionary (a is a dict), or the last row as a dictionary (a is `()`).
     * See funsql.md "No grouping". */
    if (b && b->type == RAY_NULL) {
        if (a && (a->type == -RAY_SYM || (a->type == RAY_LIST && funsql_is_fn(((ray_t**)ray_data(a))[0])))) {
            ray_t* r = funsql_eval(a, ft);          /* exec col / parse-tree -> vector/atom */
            ray_release(ft);
            return r;
        }
        if (a && a->type == RAY_DICT) {             /* exec -> dict {name: eval} */
            ray_t* ak = ray_dict_keys(a);
            ray_t* av = ray_dict_vals(a);
            int64_t na = ray_len(ak);
            int av_sym = (av && av->type == RAY_SYM);
            ray_t* dk = ray_sym_vec_new(RAY_SYM_W64, na > 0 ? na : 1);
            ray_t* dv = ray_list_new(na > 0 ? na : 1);
            for (int64_t i = 0; i < na; i++) {
                ray_t* nm = ray_sym_vec_cell(ak, i);
                dk = rsymvec_append(dk, ray_str_ptr(nm), (int)ray_str_len(nm));
                ray_t* built = NULL, *expr;
                if (av_sym) {
                    ray_t* vn = ray_sym_vec_cell(av, i);
                    built = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
                    expr = built;
                } else {
                    expr = ((ray_t**)ray_data(av))[i];
                }
                ray_t* val = funsql_eval(expr, ft);
                if (built) ray_release(built);
                if (!val || RAY_IS_ERR(val)) {
                    ray_release(dk); ray_release(dv); ray_release(ft);
                    return val ? val : ray_error("domain", "exec");
                }
                dv = ray_list_append(dv, val);
                ray_release(val);
            }
            ray_release(ft);
            return ray_dict_new(dk, dv);
        }
        if (funsql_empty(a)) {                       /* exec last row -> dict */
            int64_t nr = ray_table_nrows(ft);
            ray_t* iv = ray_i64(nr > 0 ? nr - 1 : 0);
            ray_t* r = ray_at_fn(ft, iv);
            ray_release(iv);
            ray_release(ft);
            return r;
        }
    }

    /* A symbol / symbol-vector By-phrase is the grouped-EXEC family (dict-shaped
     * results whose type depends on the a×b matrix) — deferred beyond CORE.
     * Only a `name!col` By-DICT drives a Select by-group (keyed table). */
    if (b && (b->type == -RAY_SYM || b->type == RAY_SYM)) {
        ray_release(ft);
        return ray_error("nyi", "?: symbol By-phrase (grouped exec) is not supported; use a name!col dict");
    }

    int has_by  = b && b->type == RAY_DICT;
    int has_out = a && a->type == RAY_DICT;

    /* select from t [where] — no grouping, all columns */
    if (!has_by && !has_out) return ft;

    /* by-group with an implicit column set (a is `()`) is the keyed
     * all-columns form — deferred (not CORE).  Refuse cleanly rather than build
     * a zero-output query (which trips the base null-invariant check). */
    if (has_by && !has_out) {
        ray_release(ft);
        return ray_error("nyi", "?: by-group without an aggregate dict is not supported");
    }

    ray_t* keyvec   = ray_sym_vec_new(RAY_SYM_W64, 4);
    ray_t* vallist  = ray_list_new(4);
    ray_t* keycols  = ray_sym_vec_new(RAY_SYM_W64, 1);
    ray_t* tempnames = ray_sym_vec_new(RAY_SYM_W64, 1);
    ray_t* outnames  = ray_sym_vec_new(RAY_SYM_W64, 1);

    if (has_by) funsql_add_by(b, &keyvec, &vallist, &keycols);

    if (has_out) {
        ray_t* ak = ray_dict_keys(a);
        ray_t* av = ray_dict_vals(a);
        int64_t na = ray_len(ak);
        int av_sym = (av && av->type == RAY_SYM);
        for (int64_t i = 0; i < na; i++) {
            char tmp[24];
            int tl = snprintf(tmp, sizeof tmp, "Qqc%lld", (long long)i);
            keyvec    = rsymvec_append(keyvec, tmp, tl);
            tempnames = rsymvec_append(tempnames, tmp, tl);
            ray_t* nm = ray_sym_vec_cell(ak, i);
            outnames  = rsymvec_append(outnames, ray_str_ptr(nm), (int)ray_str_len(nm));
            ray_t* ex;
            if (av_sym) {
                ray_t* vn = ray_sym_vec_cell(av, i);
                ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
            } else {
                ex = funsql_lower_expr(((ray_t**)ray_data(av))[i]);
            }
            vallist = ray_list_append(vallist, ex);
            ray_release(ex);
        }
    }

    /* from: the (already-filtered) table VALUE — appended LAST (see ql_qsql). */
    keyvec  = rsymvec_append(keyvec, "from", 4);
    vallist = ray_list_append(vallist, ft);
    ray_release(ft);

    ray_t* dict = ray_dict_new(keyvec, vallist);   /* consumes keyvec, vallist */
    if (!dict || RAY_IS_ERR(dict)) {
        ray_release(keycols); ray_release(tempnames); ray_release(outnames);
        return dict ? dict : ray_error("oom", NULL);
    }
    ray_t* sargs[4] = { dict, keycols, tempnames, outnames };
    ray_t* res = q_select_exec(sargs, 4);
    ray_release(dict); ray_release(keycols); ray_release(tempnames); ray_release(outnames);
    if (res && res->type == RAY_DICT) res = funsql_sort_keyed(res);  /* by-group key order */
    return res;
}

/* Generic scatter: overlay `upd` (length M, aligned to the M rows named by the
 * i64 index vector `idx`) onto `base` (length N), leaving unindexed rows.  Used
 * to write a grouped-update result back onto the original table (where+by
 * update, which the base ray_update refuses to compose in one pass). */
static ray_t* funsql_scatter(ray_t* base, ray_t* idx, ray_t* upd) {
    int64_t N = ray_len(base);
    int64_t M = ray_len(idx);
    int64_t* pos = (int64_t*)malloc((N > 0 ? N : 1) * sizeof(int64_t));
    if (!pos) return ray_error("oom", NULL);
    for (int64_t r = 0; r < N; r++) pos[r] = -1;
    int64_t* ii = (int64_t*)ray_data(idx);
    for (int64_t k = 0; k < M; k++)
        if (ii[k] >= 0 && ii[k] < N) pos[ii[k]] = k;
    ray_t* out = ray_list_new(N > 0 ? N : 1);
    for (int64_t r = 0; r < N; r++) {
        ray_t* src = pos[r] >= 0 ? upd : base;
        ray_t* iv  = ray_i64(pos[r] >= 0 ? pos[r] : r);
        ray_t* v   = ray_at_fn(src, iv);
        ray_release(iv);
        if (!v || RAY_IS_ERR(v)) { free(pos); ray_release(out); return v ? v : ray_error("oom", NULL); }
        out = ray_list_append(out, v);
        ray_release(v);
    }
    free(pos);
    ray_t* col = q_collapse_list(out);
    ray_release(out);
    return col;
}

/* Build an update query dict {from: TBL [by: KEY] OUT:EXPR …} from the a-dict
 * of computed columns.  `bykey` (borrowed, may be NULL) is a single grouping
 * column name-ref for by-update. */
static ray_t* funsql_update_dict(ray_t* tbl, ray_t* bykey, ray_t* a) {
    ray_t* kv = ray_sym_vec_new(RAY_SYM_W64, 4);
    ray_t* vl = ray_list_new(4);
    if (bykey) {
        kv = rsymvec_append(kv, "by", 2);
        ray_retain(bykey);
        vl = ray_list_append(vl, bykey);
        ray_release(bykey);
    }
    ray_t* ak = ray_dict_keys(a);
    ray_t* av = ray_dict_vals(a);
    int64_t na = ray_len(ak);
    int av_sym = (av && av->type == RAY_SYM);
    for (int64_t i = 0; i < na; i++) {
        ray_t* nm = ray_sym_vec_cell(ak, i);
        kv = rsymvec_append(kv, ray_str_ptr(nm), (int)ray_str_len(nm));
        ray_t* ex;
        if (av_sym) {
            ray_t* vn = ray_sym_vec_cell(av, i);
            ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
        } else {
            ex = funsql_lower_expr(((ray_t**)ray_data(av))[i]);
        }
        vl = ray_list_append(vl, ex);
        ray_release(ex);
    }
    kv = rsymvec_append(kv, "from", 4);
    ray_retain(tbl);
    vl = ray_list_append(vl, tbl);
    ray_release(tbl);
    return ray_dict_new(kv, vl);   /* consumes kv, vl */
}

/* Collapse duplicate columns produced by the base update-by path (which
 * appends a fresh `p` column rather than replacing the existing one): keep each
 * name at its first-occurrence position but take the value from its LAST
 * occurrence (kdb: `update col:…` replaces in place).  Returns a fresh table if
 * duplicates were found, else `tbl` retained. */
static ray_t* funsql_dedup_cols(ray_t* tbl) {
    if (!tbl || tbl->type != RAY_TABLE) { if (tbl) ray_retain(tbl); return tbl; }
    int64_t nc = ray_table_ncols(tbl);
    int has_dup = 0;
    for (int64_t c = 0; c < nc && !has_dup; c++)
        for (int64_t d = c + 1; d < nc; d++)
            if (ray_table_col_name(tbl, c) == ray_table_col_name(tbl, d)) { has_dup = 1; break; }
    if (!has_dup) { ray_retain(tbl); return tbl; }
    ray_t* out = ray_table_new((int32_t)(nc > 0 ? nc : 1));
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
        int64_t nm = ray_table_col_name(tbl, c);
        int seen = 0;
        for (int64_t e = 0; e < c; e++) if (ray_table_col_name(tbl, e) == nm) { seen = 1; break; }
        if (seen) continue;
        int64_t last = c;
        for (int64_t d = c + 1; d < nc; d++) if (ray_table_col_name(tbl, d) == nm) last = d;
        ray_t* col = ray_table_get_col_idx(tbl, last);
        out = ray_table_add_col(out, nm, col);
    }
    return out;
}

/* Single grouping column name-ref for update-by, or NULL.  b may be a dict
 * {name:col} (use the col), a -RAY_SYM, or a 1-vector. */
static ray_t* funsql_by_key(ray_t* b) {
    if (!b) return NULL;
    if (b->type == -RAY_SYM) return ray_sym(b->i64);
    if (b->type == RAY_SYM && ray_len(b) >= 1) {
        ray_t* cn = ray_sym_vec_cell(b, 0);
        return ray_sym(ray_sym_intern_runtime(ray_str_ptr(cn), ray_str_len(cn)));
    }
    if (b->type == RAY_DICT) {
        ray_t* bv = ray_dict_vals(b);
        if (bv && bv->type == RAY_SYM && ray_len(bv) >= 1) {
            ray_t* vn = ray_sym_vec_cell(bv, 0);
            return ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
        }
        if (bv && bv->type == RAY_LIST && ray_len(bv) >= 1) {
            ray_t* lowered = funsql_lower_expr(((ray_t**)ray_data(bv))[0]);
            if (lowered && lowered->type == -RAY_SYM) return lowered;
            if (lowered) ray_release(lowered);
        }
    }
    return NULL;
}

/* `![t;c;b;a]` — update (a is a dict) or delete (a is a symbol vector). */
static ray_t* q_funsql_bang_impl(ray_t* t, ray_t* c, ray_t* b, ray_t* a) {
    ray_t* tbl = funsql_resolve_table(t);
    if (RAY_IS_ERR(tbl)) return tbl;

    int is_update = a && a->type == RAY_DICT;

    if (!is_update) {
        /* DELETE.  a non-empty (symbol vector) => drop those columns; else the
         * where-constraints select rows to remove. */
        int del_cols = a && a->type == RAY_SYM && ray_len(a) > 0;
        if (del_cols) {
            int64_t ndrop = ray_len(a);
            int64_t drop_ids[64];
            if (ndrop > 64) ndrop = 64;
            for (int64_t i = 0; i < ndrop; i++) {
                ray_t* cn = ray_sym_vec_cell(a, i);
                drop_ids[i] = ray_sym_intern_runtime(ray_str_ptr(cn), ray_str_len(cn));
            }
            int64_t nc = ray_table_ncols(tbl);
            ray_t* out = ray_table_new((int32_t)(nc > 0 ? nc : 1));
            for (int64_t col = 0; col < nc && !RAY_IS_ERR(out); col++) {
                int64_t nm = ray_table_col_name(tbl, col);
                int drop = 0;
                for (int64_t i = 0; i < ndrop; i++) if (drop_ids[i] == nm) { drop = 1; break; }
                if (drop) continue;
                ray_t* cv = ray_table_get_col_idx(tbl, col);
                out = ray_table_add_col(out, nm, cv);
            }
            ray_release(tbl);
            return out;
        }
        /* delete rows matching c: keep the complement */
        ray_t* res = funsql_filter(tbl, c, 0);
        ray_release(tbl);
        return res;
    }

    /* UPDATE.  where (if any) is applied by filtering, running the update on
     * the subtable, then scattering the changed columns back — this composes
     * where+by, which the base ray_update declines to do in one pass. */
    int has_where = !funsql_empty(c);
    ray_t* bykey = funsql_by_key(b);

    if (!has_where) {
        ray_t* dict = funsql_update_dict(tbl, bykey, a);
        if (bykey) ray_release(bykey);
        ray_release(tbl);
        if (!dict || RAY_IS_ERR(dict)) return dict ? dict : ray_error("oom", NULL);
        ray_t* res = ray_update(&dict, 1);
        ray_release(dict);
        if (!res || RAY_IS_ERR(res)) return res;
        ray_t* dd = funsql_dedup_cols(res);
        ray_release(res);
        return dd;
    }

    ray_t* mask = funsql_build_mask(tbl, c);
    if (!mask || RAY_IS_ERR(mask)) { if (bykey) ray_release(bykey); ray_release(tbl); return mask ? mask : ray_error("domain", "update where"); }
    ray_t* idx = ray_where_fn(mask);
    ray_release(mask);
    if (!idx || RAY_IS_ERR(idx)) { if (bykey) ray_release(bykey); ray_release(tbl); return idx ? idx : ray_error("domain", "update where"); }
    ray_t* ft = ray_at_fn(tbl, idx);
    if (!ft || RAY_IS_ERR(ft)) { if (bykey) ray_release(bykey); ray_release(idx); ray_release(tbl); return ft ? ft : ray_error("domain", "update where"); }

    ray_t* udict = funsql_update_dict(ft, bykey, a);
    if (bykey) ray_release(bykey);
    ray_release(ft);
    if (!udict || RAY_IS_ERR(udict)) { ray_release(idx); ray_release(tbl); return udict ? udict : ray_error("oom", NULL); }
    ray_t* uft0 = ray_update(&udict, 1);
    ray_release(udict);
    if (!uft0 || RAY_IS_ERR(uft0)) { ray_release(idx); ray_release(tbl); return uft0 ? uft0 : ray_error("domain", "update"); }
    ray_t* uft = funsql_dedup_cols(uft0);
    ray_release(uft0);

    /* Rebuild the table, scattering each updated column back at `idx`. */
    ray_t* ak = ray_dict_keys(a);
    int64_t na = ray_len(ak);
    int64_t upd_ids[64];
    if (na > 64) na = 64;
    for (int64_t i = 0; i < na; i++) {
        ray_t* nm = ray_sym_vec_cell(ak, i);
        upd_ids[i] = ray_sym_intern_runtime(ray_str_ptr(nm), ray_str_len(nm));
    }
    int64_t nc = ray_table_ncols(tbl);
    ray_t* out = ray_table_new((int32_t)(nc > 0 ? nc : 1));
    for (int64_t col = 0; col < nc && !RAY_IS_ERR(out); col++) {
        int64_t nm = ray_table_col_name(tbl, col);
        int updated = 0;
        for (int64_t i = 0; i < na; i++) if (upd_ids[i] == nm) { updated = 1; break; }
        ray_t* base = ray_table_get_col_idx(tbl, col);
        if (!updated) { out = ray_table_add_col(out, nm, base); continue; }
        ray_t* newvals = ray_table_get_col(uft, nm);
        if (!newvals) { out = ray_table_add_col(out, nm, base); continue; }
        ray_t* merged = funsql_scatter(base, idx, newvals);
        if (!merged || RAY_IS_ERR(merged)) { ray_release(out); ray_release(idx); ray_release(uft); ray_release(tbl); return merged ? merged : ray_error("oom", NULL); }
        out = ray_table_add_col(out, nm, merged);
        ray_release(merged);
    }
    ray_release(idx);
    ray_release(uft);
    ray_release(tbl);
    return out;
}

/* SPECIAL-FORM entry points: evaluate the four operands (mapping `()`→null),
 * dispatch, then release the evaluated operands. */
static ray_t* q_funsql_dispatch(ray_t** args, int64_t n,
                                ray_t* (*impl)(ray_t*, ray_t*, ray_t*, ray_t*),
                                const char* glyph) {
    if (n != 4) return ray_error("rank", "%s[t;c;b;a]: expects 4 args, got %lld", glyph, (long long)n);
    ray_t* ev[4] = {0};
    for (int i = 0; i < 4; i++) {
        ev[i] = funsql_operand(args[i]);
        if (!ev[i] || RAY_IS_ERR(ev[i])) {
            ray_t* err = ev[i] ? ev[i] : ray_error("domain", "funsql: operand eval");
            for (int j = 0; j < i; j++) ray_release(ev[j]);
            return err;
        }
    }
    ray_t* res = impl(ev[0], ev[1], ev[2], ev[3]);
    for (int i = 0; i < 4; i++) ray_release(ev[i]);
    return res;
}

static ray_t* q_funsql_select(ray_t** args, int64_t n) {
    return q_funsql_dispatch(args, n, q_funsql_select_impl, "?");
}
static ray_t* q_funsql_bang(ray_t** args, int64_t n) {
    return q_funsql_dispatch(args, n, q_funsql_bang_impl, "!");
}

static ray_t* g_list_value   = NULL;
static ray_t* g_table_value  = NULL;
static ray_t* g_select_value = NULL;
static ray_t* g_funsql_select_value = NULL;
static ray_t* g_funsql_bang_value   = NULL;

ray_t* q_registry_funsql_select_value(void) { return g_funsql_select_value; }
ray_t* q_registry_funsql_bang_value(void)   { return g_funsql_bang_value; }

ray_t* q_registry_select_value(void) {
    return g_select_value;   /* borrowed; NULL before init */
}

ray_t* q_registry_list_value(void) {
    return g_list_value;   /* borrowed; NULL before init */
}

ray_t* q_registry_table_value(void) {
    return g_table_value;  /* borrowed; NULL before init */
}

/* ---- collapse: homogeneous atom list -> typed vector (see q_registry.h) ---- */

ray_t* q_collapse_list(ray_t* l) {
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
        switch (t) {
        case -RAY_BOOL: vec = ray_vec_append(vec, &e[i]->b8);  break;
        case -RAY_I16:  vec = ray_vec_append(vec, &e[i]->i16); break;
        case -RAY_I32:  vec = ray_vec_append(vec, &e[i]->i32); break;
        case -RAY_F32: { float f = (float)e[i]->f64;            /* F32 atom stores f64 */
                         vec = ray_vec_append(vec, &f); }       break;
        case -RAY_F64:  vec = ray_vec_append(vec, &e[i]->f64); break;
        default:        vec = ray_vec_append(vec, &e[i]->i64); break; /* i64 + temporals */
        }
        if (RAY_IS_ERR(vec)) return vec;
        if (RAY_ATOM_IS_NULL(e[i])) { ray_vec_set_null(vec, i, true); nulls++; }
    }
    (void)nulls;
    return vec;
}

/* ---- value builders keyed by manifest build-kind ---- */

/* Identity/rename-reuse: snapshot an existing rayfall builtin value by name and
 * retain it (the registry owns one ref).  Returns NULL if the audited source is
 * absent — a real bug, so q_registry_init fails fast. */
static ray_t* build_env(const char* env_name) {
    ray_t* e = ray_env_get(ray_sym_intern(env_name, strlen(env_name)));
    if (!e) return NULL;
    ray_retain(e);
    return e;
}

/* Bespoke wrapper: aux-name = the canonical rayfall lowering name; flagged
 * RAY_FN_Q_LOWER so the compiler/query DAG name-route it.
 *
 * SERDE LIMITATION (codex P1, deferred to 2b): generic function serialization
 * (src/store/serde.c) writes ray_fn_name and deserializes via ray_env_get, so a
 * serialized wrapper would come back as the plain like-named builtin (losing the
 * q string/arg-swap semantics), and `_`->"drop" has no env binding at all.  This
 * is NOT reachable in stage 2a: with no parser flip, wrapper VALUES never become
 * user-visible or serializable — they exist only inside the registry and the
 * transient AST that q_lower feeds straight to eval.  (The pre-2a wrappers, named
 * "="/"#"/"_", were equally non-round-trippable — env has no such names.)  2b,
 * which makes value heads user-visible, must teach serde to recognise
 * RAY_FN_Q_LOWER and re-derive the wrapper from the registry; that fix has a
 * testable surface only once the parser embeds these values. */
static ray_t* build_wrapper(q_build_kind kind, const char* lower_name) {
    switch (kind) {
    case QK_EQ:   return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_eq_wrap);
    case QK_NE:   return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_ne_wrap);
    case QK_TAKE: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_take_wrap);
    case QK_DROP: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_drop_wrap);
    case QK_EACH: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_each_wrap);
    case QK_MATCH:return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_match_wrap);
    case QK_FLOOR:return ray_fn_unary (lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_floor_wrap);
    case QK_BANG: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_bang_wrap);
    case QK_KEY:  return ray_fn_unary (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_key_wrap);
    case QK_VALUE:return ray_fn_unary (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_value_wrap);
    case QK_DISTINCT:
                  return ray_fn_unary (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_distinct_wrap);
    case QK_ROLL: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_roll_wrap);
    case QK_CAST: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_cast_wrap);
    case QK_AT:   return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_at_wrap);
    case QK_DOT:  return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_dot_wrap);
    default:      return NULL;
    }
}

/* Build the owned value for one (kind, target). */
static ray_t* build_value(q_build_kind kind, const char* target) {
    if (kind == QK_NONE) return NULL;
    if (kind == QK_ENV)  return build_env(target);
    return build_wrapper(kind, target);
}

/* Record one (name, valence) entry.  Returns RAY_OK, or RAY_ERR_DOMAIN if the
 * builder produced NULL/err for a non-QK_NONE kind (audited source missing). */
static ray_err_t add_entry(const char* name, q_valence_t valence,
                           q_build_kind kind, const char* target) {
    /* Bootstrap invariant (codex #1): entries are only ever built inside
     * q_registry_init's build window, and a builder must never re-enter the
     * parser.  build_value below touches only ray_env_get / ray_fn_* — never
     * q_parse — so this holds by construction; the assert pins it. */
    assert(g_building);
    if (kind == QK_NONE) return RAY_OK;                 /* nothing at this valence */
    ray_t* val = build_value(kind, target);
    if (!val || RAY_IS_ERR(val)) return RAY_ERR_DOMAIN; /* fail-fast: audited bug */
    entry_t* e    = &g_entries[g_count++];
    e->sym_id     = ray_sym_intern(name, strlen(name));
    e->valence    = valence;
    e->value      = val;
    e->spelling   = name;
    e->lower_name = target;                             /* rayfall routing name */
    e->is_wrapper = (kind != QK_ENV);
    return RAY_OK;
}

/* ---- serde hooks: q wrappers round-trip through the registry -------------
 * A RAY_FN_Q_LOWER wrapper serialized by aux-name would deserialize as the
 * like-named env builtin (wrong arg order for `#`, no binding at all for
 * `_`->"drop"), silently losing q semantics.  The writer claims registry
 * wrappers with a `q!<spelling>!<valence>` wire name (fits the standard
 * 15-byte slot); the reader decodes it back to THE registry value.  Internal
 * spelling-less values (scan/list) have no provenance and fall through to
 * the env path — documented, not silently wrong (env scan/list exist). */

static int q_serde_fn_writer(ray_t* fn, char out[16]) {
    if (!fn || !(fn->attrs & RAY_FN_Q_LOWER)) return 0;
    q_provenance_t pv;
    if (!q_registry_provenance(fn, &pv) || !pv.is_wrapper) return 0;
    int n = snprintf(out, 16, "q!%s!%d", pv.spelling, (int)pv.valence);
    return n > 0 && n < 16;
}

static ray_t* q_serde_fn_reader(const char* name) {
    if (!name || name[0] != 'q' || name[1] != '!') return NULL;
    const char* sp = name + 2;
    const char* bang = strrchr(sp, '!');
    if (!bang || bang == sp) return NULL;
    q_valence_t v = (q_valence_t)atoi(bang + 1);
    if (v != Q_MONADIC && v != Q_DYADIC) return NULL;
    ray_t* hit = q_registry_lookup_name(sp, (size_t)(bang - sp), v);
    if (!hit) return NULL;      /* falls through to the env path -> name error */
    ray_retain(hit);
    return hit;                 /* owned, per the hook contract */
}

/* ---- API ---- */

ray_err_t q_registry_init(void) {
    if (g_inited) return RAY_OK;   /* idempotent */
    g_count    = 0;
    g_building = true;
    int n = 0;
    const q_op_t* ops = q_ops_table(&n);
    /* Cap check: g_entries sized for 2 per row.  Static roster, so this is a
     * build-time invariant, asserted for future growth. */
    assert(2 * n <= (int)(sizeof g_entries / sizeof g_entries[0]));
    for (int i = 0; i < n; i++) {
        const q_op_t* op = &ops[i];
        if (add_entry(op->name, Q_MONADIC, op->mon_kind,  op->mon_target)  != RAY_OK) {
            g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
        }
        if (add_entry(op->name, Q_DYADIC,  op->dyad_kind, op->dyad_target) != RAY_OK) {
            g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
        }
    }
    /* internal (spelling-less) values consumed by q_lower / the parser */
    g_scan_value = ray_fn_vary("scan", RAY_FN_NONE | RAY_FN_Q_LOWER, q_scan_wrap);
    if (!g_scan_value || RAY_IS_ERR(g_scan_value)) {
        g_scan_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    /* Both ctx constructor heads are SPECIAL_FORM: q_ctx_build must receive the
     * raw element trees to evaluate them right-to-left inside a pushed scope. */
    g_list_value = ray_fn_vary("list",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_list_build);
    if (!g_list_value || RAY_IS_ERR(g_list_value)) {
        g_list_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_table_value = ray_fn_vary("table",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_table_build);
    if (!g_table_value || RAY_IS_ERR(g_table_value)) {
        g_table_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_select_value = ray_fn_vary("q.select",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_select_exec);
    if (!g_select_value || RAY_IS_ERR(g_select_value)) {
        g_select_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    /* functional qSQL executors — SPECIAL FORMs: they receive t/c/b/a
     * UNEVALUATED and evaluate them internally (funsql_operand), so the `()`
     * empty marker (parser `::` name-ref) does not trip ray_eval's name check. */
    g_funsql_select_value = ray_fn_vary("q.funsql.select",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_funsql_select);
    if (!g_funsql_select_value || RAY_IS_ERR(g_funsql_select_value)) {
        g_funsql_select_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_funsql_bang_value = ray_fn_vary("q.funsql.bang",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_funsql_bang);
    if (!g_funsql_bang_value || RAY_IS_ERR(g_funsql_bang_value)) {
        g_funsql_bang_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_building = false;
    g_inited   = true;
    ray_serde_set_fn_hooks(q_serde_fn_writer, q_serde_fn_reader);
    return RAY_OK;
}

bool q_registry_ready(void) {
    return g_inited;
}

ray_t* q_registry_lookup(int64_t sym_id, q_valence_t valence) {
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].sym_id == sym_id && g_entries[i].valence == valence)
            return g_entries[i].value;   /* borrowed */
    return NULL;
}

ray_t* q_registry_lookup_name(const char* s, size_t n, q_valence_t valence) {
    return q_registry_lookup(ray_sym_intern(s, n), valence);
}

bool q_registry_provenance(const ray_t* value, q_provenance_t* out) {
    /* Wrapper values are UNIQUE objects (born rc=1 per row) — pointer identity
     * is exact for them.  Pass-through/rename values ARE the shared env builtin
     * object, and several q spellings can alias one env object (e.g. `#`-monadic
     * and `count` both embed env `count`; `-`-monadic and `neg` both embed env
     * `neg`).  For those, pointer identity alone cannot recover THE q spelling —
     * an inherent limitation of the reuse-the-env-object design.  So: prefer the
     * unique WRAPPER entry (always correct); for an aliased pass-through, return
     * the first-registered spelling (2b's formatter disambiguates aliased
     * pass-throughs from the parse-site glyph, not from this value-keyed API). */
    int first = -1;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].value != value) continue;
        if (g_entries[i].is_wrapper) { first = i; break; }   /* unique — exact */
        if (first < 0) first = i;                            /* remember first */
    }
    if (first < 0) return false;
    if (out) {
        out->spelling   = g_entries[first].spelling;
        out->valence    = g_entries[first].valence;
        out->lower_name = g_entries[first].lower_name;
        out->is_wrapper = g_entries[first].is_wrapper;
    }
    return true;
}

/* Idempotent; also serves as partial-cleanup on a failed init (guards on
 * g_count, not g_inited, so a half-built table is fully released). */
void q_registry_destroy(void) {
    ray_serde_set_fn_hooks(NULL, NULL);   /* hooks read g_entries — detach first */
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].value) ray_release(g_entries[i].value);
    if (g_scan_value)  { ray_release(g_scan_value);  g_scan_value  = NULL; }
    if (g_list_value)   { ray_release(g_list_value);   g_list_value   = NULL; }
    if (g_table_value)  { ray_release(g_table_value);  g_table_value  = NULL; }
    if (g_select_value) { ray_release(g_select_value); g_select_value = NULL; }
    if (g_funsql_select_value) { ray_release(g_funsql_select_value); g_funsql_select_value = NULL; }
    if (g_funsql_bang_value)   { ray_release(g_funsql_bang_value);   g_funsql_bang_value   = NULL; }
    g_count  = 0;
    g_inited = false;
}
