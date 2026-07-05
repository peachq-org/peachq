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
#include "qlang/q_apply.h"    /* q_apply_noun — @/. noun arms */
#include "lang/env.h"         /* ray_env_get, ray_fn_binary */
#include "lang/eval.h"        /* RAY_FN_* attrs, RAY_FN_Q_LOWER */
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

static ray_t* g_list_value   = NULL;
static ray_t* g_table_value  = NULL;
static ray_t* g_select_value = NULL;

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
    g_count  = 0;
    g_inited = false;
}
