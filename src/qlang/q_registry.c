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
#include "qlang/q_parse.h"    /* q_parse/q_lower — value-of-string (RUNTIME wrapper only; builders must never parse, rule 6) */
#include "lang/internal.h"    /* ray_where_fn, ray_group_fn (funsql executor) */
#include "ops/ops.h"          /* ray_is_lazy, ray_lazy_materialize (control forms) */
#include "ops/glob.h"         /* ray_glob_match — like/ss/ssr pattern matching */
#include "table/sym.h"        /* ray_sym_vec_cell (funsql executor) */
#include "qlang/q_wire.h"     /* -8!/-9! internal-fn dispatch on dyadic ! */
#include "store/serde.h"      /* ray_serde_set_fn_hooks — wrapper round-trip */
#include "lang/format.h"      /* ray_type_name — wrapper error messages */
#include "mem/heap.h"         /* RAY_ATTR_HAS_NULLS — ? find miss remap */
#include "core/numparse.h"    /* ray_parse_i64/f64 — Tok string parses */
#include <assert.h>
#include <stdint.h>   /* INT*_MAX / INT64_MIN — Tok out-of-domain bounds */
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
static entry_t g_entries[2 * 256];  /* 2 slots per manifest row; grown 96->128 (list-verb 2026-07-06)->256 (2026-07-07: set-ops+sort+control-flow+atomic-math pushed the row count past 128) */
static int     g_count    = 0;
static bool    g_inited   = false;
static bool    g_building = false;   /* debug re-entry guard (see header note) */

/* ---- shared q-name sanitization (.Q.id + construction clash repair) ------ */

static int q_name_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int q_name_is_keyword_reserved(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return 0;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    int nop = 0;
    const q_op_t* ops = q_ops_table(&nop);
    int hit = 0;
    for (int i = 0; i < nop && !hit; i++) {
        if (ops[i].lex == QLEX_GLYPH || ops[i].lex == QLEX_ADVERB) continue;
        size_t m = strlen(ops[i].name);
        hit = (m == n && memcmp(ops[i].name, p, n) == 0);
    }
    ray_release(s);
    return hit;
}

static int q_name_prev_contains(const int64_t* previous, int64_t n_previous,
                                int64_t sym_id) {
    for (int64_t i = 0; i < n_previous; i++)
        if (previous[i] == sym_id) return 1;
    return 0;
}

static int64_t q_name_append_suffix(int64_t sym_id, int64_t suffix) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return sym_id;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    char stack[128];
    char* buf = stack;
    int need = snprintf(NULL, 0, "%.*s%lld", (int)n, p, (long long)suffix);
    if (need < 0) { ray_release(s); return sym_id; }
    if ((size_t)need + 1 > sizeof stack) {
        buf = (char*)malloc((size_t)need + 1);
        if (!buf) { ray_release(s); return sym_id; }
    }
    snprintf(buf, (size_t)need + 1, "%.*s%lld", (int)n, p, (long long)suffix);
    int64_t out = ray_sym_intern_runtime(buf, (size_t)need);
    if (buf != stack) free(buf);
    ray_release(s);
    return out;
}

int64_t q_name_sanitize(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return ray_sym_intern_runtime("a", 1);
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    char stack[128];
    char* buf = (n + 2 <= sizeof stack) ? stack : (char*)malloc(n + 2);
    if (!buf) { ray_release(s); return ray_sym_intern_runtime("a", 1); }
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (q_name_char_ok(p[i])) buf[w++] = p[i];
    if (w == 0) buf[w++] = 'a';
    if (buf[0] == '_' || (buf[0] >= '0' && buf[0] <= '9')) {
        memmove(buf + 1, buf, w);
        buf[0] = 'a';
        w++;
    }
    int64_t out = ray_sym_intern_runtime(buf, w);
    if (buf != stack) free(buf);
    ray_release(s);
    return out;
}

int64_t q_name_dedup(int64_t sym_id, const int64_t* previous, int64_t n_previous,
                     int check_reserved) {
    int64_t base = sym_id;
    if (check_reserved && q_name_is_keyword_reserved(base))
        base = q_name_append_suffix(base, 1);
    if (!q_name_prev_contains(previous, n_previous, base)) return base;
    for (int64_t i = 1; i < INT64_MAX; i++) {
        int64_t cand = q_name_append_suffix(base, i);
        if (!q_name_prev_contains(previous, n_previous, cand)) return cand;
    }
    return base;
}

ray_t* q_name_reserved_words(void) {
    int nop = 0;
    const q_op_t* ops = q_ops_table(&nop);
    int n = 0;
    for (int i = 0; i < nop; i++)
        if (ops[i].lex != QLEX_GLYPH && ops[i].lex != QLEX_ADVERB) n++;
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int i = 0; i < nop; i++) {
        if (ops[i].lex == QLEX_GLYPH || ops[i].lex == QLEX_ADVERB) continue;
        int64_t id = ray_sym_intern_runtime(ops[i].name, strlen(ops[i].name));
        out = ray_vec_append(out, &id);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    }
    return out;
}

/* ---- wrappers (bespoke q semantics over a rayfall primitive) ---- */

/* Gather `count` elements from x starting at logical index `start`.  recycle:
 * indices wrap modulo `total` (reshape recycling); else sequential in-range
 * (chunk / cut).  A string gathers chars into a new string; a vector/list
 * gathers via ray_at_fn over an i64 index vector.  Borrows x. */
static ray_t* q_gather(ray_t* x, int64_t start, int64_t count, int64_t total,
                       int recycle) {
    if (count < 0) count = 0;
    if (x && x->type == -RAY_STR) {
        const char* sp = ray_str_ptr(x);
        char stackb[1024];
        char* b = (count < (int64_t)sizeof stackb) ? stackb
                                                    : malloc((size_t)count + 1);
        if (!b) return ray_error("wsfull", "#: out of memory");
        for (int64_t i = 0; i < count; i++)
            b[i] = sp[recycle && total ? ((start + i) % total) : (start + i)];
        ray_t* row = ray_str(b, (size_t)count);
        if (b != stackb) free(b);
        return row;
    }
    int64_t stacki[1024];
    int64_t* ix = (count <= 1024) ? stacki
                                  : malloc((size_t)(count ? count : 1) * sizeof(int64_t));
    if (!ix) return ray_error("wsfull", "#: out of memory");
    for (int64_t i = 0; i < count; i++)
        ix[i] = recycle && total ? ((start + i) % total) : (start + i);
    ray_t* idx = ray_vec_from_raw(RAY_I64, ix, count);
    if (ix != stacki) free(ix);
    if (RAY_IS_ERR(idx)) return idx;
    ray_t* row = ray_at_fn(x, idx);       /* boxed list of atoms */
    ray_release(idx);
    if (!row || RAY_IS_ERR(row)) return row;
    ray_t* c = q_collapse_list(row);      /* -> typed vector when homogeneous */
    ray_release(row);
    return c;
}

/* q `shape # x` — reshape (kdb ref/take): an int-VECTOR left arg reshapes x
 * row-major into a matrix (2-D case).  A 0N dimension is INFERRED (chunk into
 * that stride, ragged last row); otherwise x is RECYCLED to fill.  Ranks other
 * than 2 fall back to rayfall range-take (unchanged behaviour).  Borrows both. */
static ray_t* q_reshape(ray_t* shape, ray_t* x) {
    int64_t nd = ray_len(shape);
    if (nd != 2) return ray_take_fn(x, shape);
    const int64_t* dv = (const int64_t*)ray_data(shape);
    int64_t d0 = dv[0], d1 = dv[1];
    int is_str = (x && x->type == -RAY_STR);
    int64_t total = is_str ? (int64_t)ray_str_len(x) : (x ? ray_len(x) : 0);
    if (total <= 0) return ray_error("length", "#: reshape of empty");
    int64_t rows, cols, chunk = 0;
    if (d0 == INT64_MIN && d1 != INT64_MIN) {
        if (d1 <= 0) return ray_error("length", "#: inferred reshape needs a positive stride");
        cols = d1; rows = (total + cols - 1) / cols; chunk = 1;
    } else if (d1 == INT64_MIN && d0 != INT64_MIN) {
        if (d0 <= 0) return ray_error("length", "#: inferred reshape needs a positive stride");
        rows = d0; cols = (total + rows - 1) / rows; chunk = 1;
    } else if (d0 == INT64_MIN || d1 == INT64_MIN) {
        return ray_error("length", "#: 0N 0N reshape");
    } else {
        rows = d0; cols = d1;
    }
    if (rows < 0 || cols < 0) return ray_error("length", "#: negative shape");
    ray_t* out = ray_list_new(rows > 0 ? rows : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t r = 0; r < rows; r++) {
        int64_t start = r * cols, rc = cols;
        if (chunk && start + rc > total) rc = total - start;   /* ragged last */
        ray_t* row = q_gather(x, start, rc, total, !chunk);
        if (!row || RAY_IS_ERR(row)) { ray_release(out); return row ? row : ray_error("domain", "#: reshape"); }
        out = ray_list_append(out, row);
        ray_release(row);
        if (RAY_IS_ERR(out)) return out;
    }
    return out;
}

/* q `n # list` — take.  An int-VECTOR left arg (len>=2) is RESHAPE (matrix);
 * an atom is take.  rayfall ray_take_fn(vec, n) has the opposite arg order, so
 * swap.  Borrows both args (does not release them). */
static ray_t* q_take_wrap(ray_t* n, ray_t* list) {
    if (n && n->type == RAY_I64 && ray_len(n) >= 2) return q_reshape(n, list);
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

/* q `n cut x` — cut into pieces (kdb ref/cut).  An int ATOM chunks x into
 * groups of n (ragged last group: `4 cut til 10` -> (0 1 2 3;4 5 6 7;8 9)).
 * An int VECTOR is a positional cut — identical to `_`, so delegate.  Borrows. */
static ray_t* q_cut_wrap(ray_t* n, ray_t* x) {
    if (n && (n->type == -RAY_I64 || n->type == -RAY_I32 || n->type == -RAY_I16)) {
        int64_t sz = (n->type == -RAY_I64) ? n->i64
                   : (n->type == -RAY_I32) ? (int64_t)n->i32 : (int64_t)n->i16;
        if (sz <= 0) return ray_error("domain", "cut: piece size must be positive");
        int is_str = (x && x->type == -RAY_STR);
        int64_t total = is_str ? (int64_t)ray_str_len(x) : (x ? ray_len(x) : 0);
        if (!x || (!is_str && !ray_is_vec(x) && x->type != RAY_LIST))
            return ray_error("type", "cut: expects a list/vector/string rhs");
        int64_t rows = (total + sz - 1) / sz;
        ray_t* out = ray_list_new(rows > 0 ? rows : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t r = 0; r < rows; r++) {
            int64_t start = r * sz, rc = sz;
            if (start + rc > total) rc = total - start;
            ray_t* row = q_gather(x, start, rc, total, 0);   /* chunk, no recycle */
            if (!row || RAY_IS_ERR(row)) { ray_release(out); return row ? row : ray_error("domain", "cut"); }
            out = ray_list_append(out, row);
            ray_release(row);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return q_drop_wrap(n, x);   /* int-vector positional cut == `_` */
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

/* ---- atomic unary math (feat/q-math-atomic) — implement-via-libm ----
 * rayfall has exp/log/sqrt but no trig/reciprocal/signum, so these are q-layer
 * wrappers, one libm call per atom.  All are registered RAY_FN_ATOMIC, so the
 * evaluator (atomic_map_unary) broadcasts them over vectors and nested lists;
 * each wrapper handles the ATOM case only (mirroring ray_sqrt_fn/q_floor_wrap).
 * Float results go through make_f64 (internal.h), which canonicalizes every
 * non-finite (NaN OR ±Inf) to the single float null 0n — so `sin 1%0` -> 0n
 * and reciprocal of 0 -> 0n (kdb's 0w is unrepresentable under this model, a
 * deferred cell).  Null in -> typed float null out (kdb: sin/cos/asin/... of a
 * null is null). */
#define Q_LIBM_UNARY(NAME, FN, GLYPH)                                          \
    static ray_t* NAME(ray_t* x) {                                             \
        if (!x) return ray_error("type", GLYPH ": nil");                       \
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);              \
        if (is_numeric(x)) return make_f64(FN(as_f64(x)));                     \
        return ray_error("type", GLYPH ": expects a numeric argument, got %s", \
                         ray_type_name(x->type));                              \
    }
Q_LIBM_UNARY(q_sin_wrap,  sin,  "sin")
Q_LIBM_UNARY(q_cos_wrap,  cos,  "cos")
Q_LIBM_UNARY(q_tan_wrap,  tan,  "tan")
Q_LIBM_UNARY(q_asin_wrap, asin, "asin")
Q_LIBM_UNARY(q_acos_wrap, acos, "acos")
Q_LIBM_UNARY(q_atan_wrap, atan, "atan")
#undef Q_LIBM_UNARY

/* q `reciprocal x` — 1%x as a float (ref/reciprocal.md).  Null -> null;
 * reciprocal 0 -> +Inf -> 0n under the single-null model (kdb shows 0w). */
static ray_t* q_reciprocal_wrap(ray_t* x) {
    if (!x) return ray_error("type", "reciprocal: nil");
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (is_numeric(x)) return make_f64(1.0 / as_f64(x));
    return ray_error("type", "reciprocal: expects a numeric argument, got %s",
                     ray_type_name(x->type));
}

/* q `signum x` — sign as INT (i32): null or negative -> -1i, zero -> 0i,
 * positive -> 1i (ref/signum.md).  Kdb ALWAYS returns int, whatever the input
 * width.  A float null (0n) tests as null -> -1i (kdb treats null as negative). */
static ray_t* q_signum_wrap(ray_t* x) {
    if (!x) return ray_error("type", "signum: nil");
    if (RAY_ATOM_IS_NULL(x)) return ray_i32(-1);
    if (is_numeric(x)) {
        /* as_f64 handles BOOL + every int/float width and preserves the sign
         * exactly (only magnitude precision is lost, irrelevant to sign), so
         * one branch covers all numeric atoms — avoids as_i64's missing BOOL
         * case (codex review). */
        double v = as_f64(x);
        return ray_i32(v < 0 ? -1 : (v > 0 ? 1 : 0));
    }
    return ray_error("type", "signum: expects a numeric argument, got %s",
                     ray_type_name(x->type));
}

/* q `ceiling x` — least integer >= x, returned as a LONG (kdb `ceiling 2.1` is
 * 3j).  The QK_FLOOR twin: rayfall's `ceil` keeps f64, so this wrapper rounds
 * to i64 exactly like q_floor_wrap.  Ints/bools pass through; f64 null -> long
 * null. */
static ray_t* q_ceiling_wrap(ray_t* x) {
    if (!x) return ray_error("type", "ceiling: nil");
    if (x->type == -RAY_F64) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)ceil(x->f64));
    }
    if (x->type == -RAY_F32) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)ceilf((float)x->f64));
    }
    if (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16 ||
        x->type == -RAY_BOOL) {
        ray_retain(x);
        return x;
    }
    return ray_error("type", "ceiling: expects a numeric argument, got %s",
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

/* q dyadic `&` — min / boolean-and.  The wrapper is registered ATOMIC, so
 * vector/scalar and vector/vector cases are mapped by eval over atom pairs. */
static ray_t* q_min2_wrap(ray_t* a, ray_t* b) {
    if (!a || !b || !ray_is_atom(a) || !ray_is_atom(b))
        return ray_error("type", "&: expects numeric atoms");
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return ray_bool(a->b8 && b->b8);
    if (a->type == -RAY_F64 || b->type == -RAY_F64 ||
        a->type == -RAY_F32 || b->type == -RAY_F32) {
        double av = as_f64(a);
        double bv = as_f64(b);
        return ray_f64(av <= bv ? av : bv);
    }
    if ((a->type == -RAY_I64 || a->type == -RAY_I32 || a->type == -RAY_I16 ||
         a->type == -RAY_U8  || a->type == -RAY_BOOL) &&
        (b->type == -RAY_I64 || b->type == -RAY_I32 || b->type == -RAY_I16 ||
         b->type == -RAY_U8  || b->type == -RAY_BOOL)) {
        int64_t av = (a->type == -RAY_BOOL) ? a->b8 : as_i64(a);
        int64_t bv = (b->type == -RAY_BOOL) ? b->b8 : as_i64(b);
        return ray_i64(av <= bv ? av : bv);
    }
    return ray_error("type", "&: unsupported operands");
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

/* q `til` — kdb accepts a boolean (`til 1b` -> ,0); base ray_til_fn is
 * int-only.  Everything else (int atoms, the error paths) delegates. */
static ray_t* q_til_wrap(ray_t* x) {
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
static ray_t* q_where_wrap(ray_t* x) {
    if (x && (x->type == RAY_I64 || x->type == RAY_I32 || x->type == RAY_I16)) {
        int64_t n = ray_len(x);
        int64_t total = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t c = (x->type == RAY_I64) ? ((const int64_t*)ray_data(x))[i]
                      : (x->type == RAY_I32) ? (int64_t)((const int32_t*)ray_data(x))[i]
                      : (int64_t)((const int16_t*)ray_data(x))[i];
            if (c < 0) return ray_error("domain", "where: negative count");
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

/* ===== q `vs` / `sv` — split-join / base-encode family ===================
 * kdb reference vs.md / sv.md.  Both are strictly dyadic.  Native -RAY_STR
 * strings (split -> boxed list of string atoms, join -> one string atom),
 * symbol split/join, integer base decompose/compose (atom + vector base),
 * and big-endian byte/bit encode/decode.  Genuinely out-of-scope forms
 * (128-bit GUID compose, `1:` reparse, byte-vector base) return 'nyi. */

static int q_is_null_sym(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return 0;
    ray_t* s = ray_sym_str(x->i64);
    int z = s && ray_str_len(s) == 0;
    return z;
}
static int q_is_int_atom(ray_t* x) {
    return x && (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16);
}
static int q_is_int_vec(ray_t* x) {
    return x && (x->type == RAY_I64 || x->type == RAY_I32 || x->type == RAY_I16);
}
static int64_t q_ivec_get(ray_t* v, int64_t i) {
    const void* d = ray_data(v);
    return v->type == RAY_I64 ? ((const int64_t*)d)[i]
         : v->type == RAY_I32 ? (int64_t)((const int32_t*)d)[i]
                              : (int64_t)((const int16_t*)d)[i];
}
static int64_t q_iatom_val(ray_t* x) {
    return x->type == -RAY_I64 ? x->i64
         : x->type == -RAY_I32 ? (int64_t)x->i32 : (int64_t)x->i16;
}

/* split string y on substring sep -> boxed list of -RAY_STR (keeps empties) */
static ray_t* q_str_split(const char* y, size_t yl, const char* sep, size_t sl) {
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    if (sl == 0) {                                 /* empty sep -> one piece */
        ray_t* s = ray_str(y, yl);
        out = ray_list_append(out, s); ray_release(s);
        return out;
    }
    size_t seg = 0;
    for (size_t i = 0; i + sl <= yl; ) {
        if (memcmp(y + i, sep, sl) == 0) {
            ray_t* s = ray_str(y + seg, i - seg);
            out = ray_list_append(out, s); ray_release(s);
            if (RAY_IS_ERR(out)) return out;
            i += sl; seg = i;
        } else i++;
    }
    ray_t* last = ray_str(y + seg, yl - seg);
    out = ray_list_append(out, last); ray_release(last);
    return out;
}

/* newline / host-line-separator split: split on '\n', strip a trailing '\r'
 * from each line, drop a single trailing empty line (kdb ` vs read-lines). */
static ray_t* q_str_split_lines(const char* y, size_t yl) {
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    size_t seg = 0;
    for (size_t i = 0; i <= yl; i++) {
        if (i == yl || y[i] == '\n') {
            size_t end = i;
            if (end > seg && y[end - 1] == '\r') end--;   /* strip CR */
            ray_t* s = ray_str(y + seg, end - seg);
            out = ray_list_append(out, s); ray_release(s);
            if (RAY_IS_ERR(out)) return out;
            seg = i + 1;
            if (i == yl) break;
        }
    }
    /* drop a single trailing empty produced by a terminal '\n' */
    int64_t n = ray_len(out);
    if (n >= 1) {
        ray_t** e = (ray_t**)ray_data(out);
        if (e[n - 1]->type == -RAY_STR && ray_str_len(e[n - 1]) == 0) {
            ray_release(e[n - 1]);
            out->len = n - 1;
        }
    }
    return out;
}

/* ` vs `sym — split a symbol: leading ':' (file handle) splits at the LAST
 * '/' into (dir; file); otherwise split on every '.'.  -> RAY_SYM vector. */
static ray_t* q_sym_split(ray_t* y) {
    ray_t* s = ray_sym_str(y->i64);
    if (!s) return ray_error("type", "vs: bad symbol");
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, 4);
    if (n > 0 && p[0] == ':') {                    /* file handle: last '/' */
        size_t cut = n;
        for (size_t i = n; i-- > 0; ) if (p[i] == '/') { cut = i; break; }
        if (cut == n) {                            /* no '/', single element */
            int64_t id = ray_sym_intern_runtime(p, n);
            out = ray_vec_append(out, &id);
        } else {
            int64_t a = ray_sym_intern_runtime(p, cut);
            int64_t b = ray_sym_intern_runtime(p + cut + 1, n - cut - 1);
            out = ray_vec_append(out, &a);
            out = ray_vec_append(out, &b);
        }
    } else {                                        /* split all '.' */
        size_t seg = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i == n || p[i] == '.') {
                int64_t id = ray_sym_intern_runtime(p + seg, i - seg);
                out = ray_vec_append(out, &id);
                seg = i + 1;
            }
        }
    }
    return out;
}

/* big-endian byte encode of a numeric scalar (0x0 vs y) -> U8 vector */
static ray_t* q_byte_encode(ray_t* y) {
    uint8_t b[8]; int w = 0; uint64_t bits = 0;
    switch (y->type) {
    case -RAY_I16: w = 2; bits = (uint16_t)y->i16; break;
    case -RAY_I32: w = 4; bits = (uint32_t)y->i32; break;
    case -RAY_I64: w = 8; bits = (uint64_t)y->i64; break;
    case -RAY_F32: { float f = (float)y->f64; uint32_t u; memcpy(&u, &f, 4);
                     w = 4; bits = u; break; }
    case -RAY_F64: { double d = y->f64; uint64_t u; memcpy(&u, &d, 8);
                     w = 8; bits = u; break; }
    default: return ray_error("type", "vs: unsupported byte-encode operand");
    }
    for (int i = 0; i < w; i++) b[i] = (uint8_t)(bits >> (8 * (w - 1 - i)));
    return ray_vec_from_raw(RAY_U8, b, w);
}

/* big-endian bit decompose of an integer scalar (0b vs y) -> BOOL vector */
static ray_t* q_bit_decompose(ray_t* y) {
    int w = 0; uint64_t bits = 0;
    switch (y->type) {
    case -RAY_BOOL: w = 1;  bits = y->b8 ? 1 : 0; break;
    case -RAY_U8:   w = 8;  bits = (uint8_t)y->u8; break;
    case -RAY_I16:  w = 16; bits = (uint16_t)y->i16; break;
    case -RAY_I32:  w = 32; bits = (uint32_t)y->i32; break;
    case -RAY_I64:  w = 64; bits = (uint64_t)y->i64; break;
    default: return ray_error("type", "vs: unsupported bit-decompose operand");
    }
    uint8_t stackb[64];
    for (int i = 0; i < w; i++) stackb[i] = (uint8_t)((bits >> (w - 1 - i)) & 1);
    return ray_vec_from_raw(RAY_BOOL, stackb, w);
}

/* decompose scalar v into minimal base-`base` digits (>=1) -> long vector */
static ray_t* q_base_decompose_atom(int64_t base, int64_t v) {
    if (base <= 0) return ray_error("domain", "vs: base must be positive");
    int64_t buf[64]; int n = 0;
    uint64_t u = (uint64_t)v;
    if (u == 0) buf[n++] = 0;
    while (u > 0 && n < 64) { buf[n++] = (int64_t)(u % (uint64_t)base); u /= (uint64_t)base; }
    ray_t* out = ray_vec_new(RAY_I64, n);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    for (int i = 0; i < n; i++) d[i] = buf[n - 1 - i];   /* MSB first */
    return out;
}

/* mixed-radix decompose scalar v by vector base -> long vector len(base) */
static ray_t* q_base_decompose_vec(ray_t* base, int64_t v) {
    int64_t n = ray_len(base);
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    uint64_t u = (uint64_t)v;
    for (int64_t i = n - 1; i >= 0; i--) {
        int64_t bi = q_ivec_get(base, i);
        if (bi <= 0) { d[i] = (int64_t)u; u = 0; }
        else { d[i] = (int64_t)(u % (uint64_t)bi); u /= (uint64_t)bi; }
    }
    return out;
}

static ray_t* q_vs_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "vs: nil operand");
    /* --- string / newline split --- */
    if (x->type == -RAY_STR) {
        if (y->type != -RAY_STR)
            return ray_error("nyi", "vs: string split needs a string rhs (byte-string deferred)");
        return q_str_split(ray_str_ptr(y), ray_str_len(y),
                           ray_str_ptr(x), ray_str_len(x));
    }
    if (q_is_null_sym(x)) {
        if (y->type == -RAY_STR)
            return q_str_split_lines(ray_str_ptr(y), ray_str_len(y));
        if (y->type == -RAY_SYM) return q_sym_split(y);
        return ray_error("type", "vs: ` split expects a string or symbol");
    }
    /* --- byte encode (0x0 vs scalar) --- */
    if (x->type == -RAY_U8) {
        if (ray_is_atom(y) && y->type != -RAY_STR) return q_byte_encode(y);
        return ray_error("nyi", "vs: byte-vector base decompose deferred");
    }
    /* --- bit decompose (0b vs scalar) --- */
    if (x->type == -RAY_BOOL) {
        if (ray_is_atom(y)) return q_bit_decompose(y);
        return ray_error("type", "vs: 0b decompose expects a scalar");
    }
    /* --- integer base decompose --- */
    if (q_is_int_atom(x)) {
        int64_t base = q_iatom_val(x);
        if (q_is_int_atom(y)) return q_base_decompose_atom(base, q_iatom_val(y));
        if (q_is_int_vec(y)) {                     /* matrix: pad to max width */
            int64_t m = ray_len(y);
            ray_t* cols = ray_list_new(m > 0 ? m : 1);
            int64_t maxw = 1;
            for (int64_t j = 0; j < m; j++) {
                ray_t* c = q_base_decompose_atom(base, q_ivec_get(y, j));
                if (RAY_IS_ERR(c)) { ray_release(cols); return c; }
                if (ray_len(c) > maxw) maxw = ray_len(c);
                cols = ray_list_append(cols, c); ray_release(c);
            }
            ray_t* rows = ray_list_new(maxw);
            ray_t** cv = (ray_t**)ray_data(cols);
            for (int64_t r = 0; r < maxw; r++) {
                ray_t* row = ray_vec_new(RAY_I64, m); row->len = m;
                int64_t* rd = (int64_t*)ray_data(row);
                for (int64_t j = 0; j < m; j++) {
                    int64_t cw = ray_len(cv[j]);
                    int64_t pad = maxw - cw;         /* left-pad with 0 */
                    rd[j] = (r < pad) ? 0 : ((const int64_t*)ray_data(cv[j]))[r - pad];
                }
                rows = ray_list_append(rows, row); ray_release(row);
            }
            ray_release(cols);
            return rows;
        }
        return ray_error("type", "vs: integer decompose expects an integer rhs");
    }
    if (q_is_int_vec(x)) {
        if (q_is_int_atom(y)) return q_base_decompose_vec(x, q_iatom_val(y));
        return ray_error("nyi", "vs: vector-base matrix decompose deferred");
    }
    return ray_error("type", "vs: unsupported operand types");
}

/* join a boxed list / vector of strings with separator sep (append trailing
 * when host==1, the ` sv newline form). */
static ray_t* q_str_join(ray_t* y, const char* sep, size_t sl, int host) {
    if (!y || y->type != RAY_LIST)
        return ray_error("type", "sv: join expects a list of strings");
    int64_t n = ray_len(y);
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = (y->type == RAY_LIST) ? ((ray_t**)ray_data(y))[i] : NULL;
        if (!e || e->type != -RAY_STR)
            return ray_error("type", "sv: join expects string elements");
        total += ray_str_len(e);
        if (i + 1 < n) total += sl;
    }
    if (host) total += 1;
    char* buf = malloc(total ? total : 1);
    if (!buf) return ray_error("wsfull", "sv: out of memory");
    size_t w = 0;
    ray_t** ev = (ray_t**)ray_data(y);
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = ev[i];
        size_t el = ray_str_len(e);
        memcpy(buf + w, ray_str_ptr(e), el); w += el;
        if (host) { buf[w++] = '\n'; }
        else if (i + 1 < n) { memcpy(buf + w, sep, sl); w += sl; }
    }
    ray_t* r = ray_str(buf, w);
    free(buf);
    return r;
}

/* ` sv `syms — join symbols: leading ':' (file handle) joins with '/', else
 * with '.'  -> single -RAY_SYM atom. */
static ray_t* q_sym_join(ray_t* y) {
    int64_t n = ray_len(y);
    if (n == 0) return ray_sym(ray_sym_intern_runtime("", 0));
    ray_t* first = ray_sym_vec_cell(y, 0);
    const char* fp = first ? ray_str_ptr(first) : "";
    char joiner = (ray_str_len(first) > 0 && fp[0] == ':') ? '/' : '.';
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* c = ray_sym_vec_cell(y, i);
        total += ray_str_len(c);
        if (i + 1 < n) total += 1;
    }
    char* buf = malloc(total ? total : 1);
    if (!buf) return ray_error("wsfull", "sv: out of memory");
    size_t w = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* c = ray_sym_vec_cell(y, i);
        size_t cl = ray_str_len(c);
        memcpy(buf + w, ray_str_ptr(c), cl); w += cl;
        if (i + 1 < n) buf[w++] = joiner;
    }
    int64_t id = ray_sym_intern_runtime(buf, w);
    free(buf);
    return ray_sym(id);
}

/* big-endian byte decode: interpret a U8 vector as a signed integer of the
 * matching width (2->short, 4->int, 8->long). */
static ray_t* q_byte_decode(ray_t* y) {
    int64_t n = ray_len(y);
    const uint8_t* p = (const uint8_t*)ray_data(y);
    uint64_t v = 0;
    for (int64_t i = 0; i < n; i++) v = (v << 8) | p[i];
    if (n == 2) return ray_i16((int16_t)(uint16_t)v);
    if (n == 4) return ray_i32((int32_t)(uint32_t)v);
    if (n == 8) return ray_i64((int64_t)v);
    if (n == 1) return ray_i16((int16_t)(uint8_t)v);
    return ray_error("nyi", "sv: byte decode width %lld deferred", (long long)n);
}

/* bits -> integer (8->byte, 16->short, 32->int, 64->long; 128->guid deferred) */
static ray_t* q_bit_compose(ray_t* y) {
    int64_t n = ray_len(y);
    const uint8_t* p = (const uint8_t*)ray_data(y);
    if (n == 128) return ray_error("nyi", "sv: 128-bit GUID compose deferred");
    if (n != 8 && n != 16 && n != 32 && n != 64)
        return ray_error("nyi", "sv: bit compose width %lld deferred", (long long)n);
    uint64_t v = 0;
    for (int64_t i = 0; i < n; i++) v = (v << 1) | (p[i] & 1);
    if (n == 8)  return ray_u8((uint8_t)v);
    if (n == 16) return ray_i16((int16_t)(uint16_t)v);
    if (n == 32) return ray_i32((int32_t)(uint32_t)v);
    return ray_i64((int64_t)v);
}

static ray_t* q_sv_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "sv: nil operand");
    /* --- string join --- */
    if (x->type == -RAY_STR)
        return q_str_join(y, ray_str_ptr(x), ray_str_len(x), 0);
    if (q_is_null_sym(x)) {
        if (y->type == RAY_SYM) return q_sym_join(y);            /* sym join */
        return q_str_join(y, "\n", 1, 1);                        /* host lines */
    }
    /* --- byte decode (0x0 sv bytes) --- */
    if (x->type == -RAY_U8) {
        if (y->type == RAY_U8) return q_byte_decode(y);
        return ray_error("nyi", "sv: byte-vector base compose deferred");
    }
    /* --- bit compose (0b sv bits) --- */
    if (x->type == -RAY_BOOL) {
        if (y->type == RAY_BOOL) return q_bit_compose(y);
        return ray_error("type", "sv: 0b compose expects a bool vector");
    }
    /* --- integer base compose (Horner) --- */
    if (q_is_int_atom(x)) {
        int64_t base = q_iatom_val(x);
        if (!q_is_int_vec(y) && y->type != RAY_BOOL)
            return ray_error("type", "sv: integer compose expects an integer vector");
        int64_t n = ray_len(y);
        int64_t acc = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t d = (y->type == RAY_BOOL) ? ((const uint8_t*)ray_data(y))[i]
                                              : q_ivec_get(y, i);
            acc = acc * base + d;
        }
        return ray_i64(acc);
    }
    /* --- mixed-radix compose (vector base) --- */
    if (q_is_int_vec(x)) {
        if (!q_is_int_vec(y)) return ray_error("type", "sv: mixed-radix expects an integer vector rhs");
        int64_t n = ray_len(y), bn = ray_len(x);
        if (n != bn) return ray_error("length", "sv: base and value lengths must match");
        int64_t acc = 0;
        for (int64_t i = 0; i < n; i++)
            acc = acc * q_ivec_get(x, i) + q_ivec_get(y, i);
        return ray_i64(acc);
    }
    return ray_error("type", "sv: unsupported operand types");
}

/* ===== Wave 5 — running / weighted / covariance aggregates ================
 * kdb ref/{sums,prds,maxs,mins,avgs,ratios,wsum,wavg,cov}.md.  Null discipline
 * per page: sum/sums treat null as 0, prd/prds as 1, avg/avgs EXCLUDE nulls,
 * max/min skip nulls (kdb shows -0W/0W for leading nulls — long ±infinity is
 * not representable in this engine's sentinel-null model, so those specific
 * rows are a documented lang-divergence). */

/* Read element i of a numeric vector as a double; *isnull set for the typed
 * null sentinel (int MIN / NaN). */
static double q_velem_f(ray_t* x, int64_t i, int* isnull) {
    *isnull = 0;
    if (ray_is_atom(x)) {                 /* scalar (index ignored) */
        switch (x->type) {
        case -RAY_I64: if (x->i64==NULL_I64){*isnull=1;} return (double)x->i64;
        case -RAY_I32: if (x->i32==NULL_I32){*isnull=1;} return (double)x->i32;
        case -RAY_I16: if (x->i16==NULL_I16){*isnull=1;} return (double)x->i16;
        case -RAY_BOOL:return (double)x->b8;
        case -RAY_U8:  return (double)x->u8;
        case -RAY_F64: case -RAY_F32: if (isnan(x->f64)){*isnull=1;} return x->f64;
        default: *isnull = 1; return 0;
        }
    }
    const void* d = ray_data(x);
    switch (x->type) {
    case RAY_I64: { int64_t v = ((const int64_t*)d)[i]; if (v==NULL_I64){*isnull=1;} return (double)v; }
    case RAY_I32: { int32_t v = ((const int32_t*)d)[i]; if (v==NULL_I32){*isnull=1;} return (double)v; }
    case RAY_I16: { int16_t v = ((const int16_t*)d)[i]; if (v==NULL_I16){*isnull=1;} return (double)v; }
    case RAY_BOOL:return (double)((const uint8_t*)d)[i];
    case RAY_U8:  return (double)((const uint8_t*)d)[i];
    case RAY_F64: { double v = ((const double*)d)[i]; if (isnan(v)){*isnull=1;} return v; }
    case RAY_F32: { float  v = ((const float*)d)[i];  if (isnan(v)){*isnull=1;} return (double)v; }
    default: *isnull = 1; return 0;
    }
}
static int q_vec_is_float(ray_t* x) { return x->type == RAY_F64 || x->type == RAY_F32; }
static int q_vec_is_num(ray_t* x) {
    return x && (x->type==RAY_I64||x->type==RAY_I32||x->type==RAY_I16||
                 x->type==RAY_BOOL||x->type==RAY_U8||x->type==RAY_F64||x->type==RAY_F32);
}

typedef enum { RS_SUMS, RS_PRDS, RS_MAXS, RS_MINS, RS_AVGS } q_rs_kind;

/* running max/min over the bytes of a q string (kdb `maxs "genie"`). */
static ray_t* q_runscan_str(ray_t* x, q_rs_kind k) {
    const char* p = ray_str_ptr(x);
    size_t n = ray_str_len(x);
    char stackb[256];
    char* b = (n <= sizeof stackb) ? stackb : malloc(n ? n : 1);
    if (!b) return ray_error("wsfull", "maxs: out of memory");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (i == 0) b[i] = (char)c;
        else b[i] = (k==RS_MAXS) ? (char)((unsigned char)b[i-1] > c ? (unsigned char)b[i-1] : c)
                                 : (char)((unsigned char)b[i-1] < c ? (unsigned char)b[i-1] : c);
    }
    ray_t* r = ray_str(b, n);
    if (b != stackb) free(b);
    return r;
}

static ray_t* q_runscan(ray_t* x, q_rs_kind k) {
    if (!x) return ray_error("type", "running scan: nil");
    if (x->type == -RAY_STR) {
        if (k==RS_MAXS || k==RS_MINS) return q_runscan_str(x, k);
        return ray_error("type", "running scan: non-numeric");
    }
    if (ray_is_atom(x)) {                 /* atom returned unchanged (avgs->float) */
        if (k == RS_AVGS) { int nu; double v = q_velem_f(x, 0, &nu);
                            return nu ? ray_typed_null(-RAY_F64) : ray_f64(v); }
        ray_retain(x); return x;
    }
    if (!q_vec_is_num(x)) return ray_error("type", "running scan: non-numeric list");
    int64_t n = ray_len(x);
    if (k == RS_AVGS) {
        ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
        double* o = (double*)ray_data(out);
        double s = 0; int64_t c = 0;
        for (int64_t i = 0; i < n; i++) {
            int nu; double v = q_velem_f(x, i, &nu);
            if (!nu) { s += v; c++; }
            if (c == 0) { o[i] = 0; ray_vec_set_null(out, i, true); } else o[i] = s / (double)c;
        }
        return out;
    }
    int isf = q_vec_is_float(x);
    if (isf || k == RS_AVGS) {
        ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
        double* o = (double*)ray_data(out);
        double acc = (k==RS_PRDS) ? 1 : 0; int started = 0; double m = 0;
        for (int64_t i = 0; i < n; i++) {
            int nu; double v = q_velem_f(x, i, &nu);
            if (k==RS_SUMS) { acc += nu?0:v; o[i]=acc; }
            else if (k==RS_PRDS) { acc *= nu?1:v; o[i]=acc; }
            else { if (!nu) { if (!started){m=v;started=1;} else if (k==RS_MAXS?v>m:v<m) m=v; }
                   if (started) o[i]=m; else { o[i]=0; ray_vec_set_null(out,i,true); } }
        }
        return out;
    }
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1); out->len = n;
    int64_t* o = (int64_t*)ray_data(out);
    int64_t acc = (k==RS_PRDS) ? 1 : 0; int started = 0; int64_t m = 0;
    for (int64_t i = 0; i < n; i++) {
        int nu; double vd = q_velem_f(x, i, &nu); int64_t v = (int64_t)vd;
        if (k==RS_SUMS) { acc += nu?0:v; o[i]=acc; }
        else if (k==RS_PRDS) { acc *= nu?1:v; o[i]=acc; }
        else { if (!nu) { if (!started){m=v;started=1;} else if (k==RS_MAXS?v>m:v<m) m=v; }
               if (started) o[i]=m; else { o[i]=NULL_I64; ray_vec_set_null(out,i,true); } }
    }
    return out;
}
static ray_t* q_sums_wrap(ray_t* x){ return q_runscan(x, RS_SUMS); }
static ray_t* q_prds_wrap(ray_t* x){ return q_runscan(x, RS_PRDS); }
static ray_t* q_maxs_wrap(ray_t* x){ return q_runscan(x, RS_MAXS); }
static ray_t* q_mins_wrap(ray_t* x){ return q_runscan(x, RS_MINS); }
static ray_t* q_avgs_wrap(ray_t* x){ return q_runscan(x, RS_AVGS); }

/* q `ratios x` — pairwise ratio: r[0]=x[0], r[i]=x[i] % x[i-1] (float). */
static ray_t* q_ratios_wrap(ray_t* x) {
    if (!x) return ray_error("type", "ratios: nil");
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (!q_vec_is_num(x)) return ray_error("type", "ratios: non-numeric list");
    int64_t n = ray_len(x);
    ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
    double* o = (double*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        if (i == 0) {                       /* r[0] = x[0] (null stays null) */
            if (nu) { o[i] = 0; ray_vec_set_null(out, i, true); } else o[i] = v;
            continue;
        }
        int pnu; double pv = q_velem_f(x, i-1, &pnu);
        /* r[i] = x[i] % x[i-1] with q float-divide semantics: a null operand or
         * a zero divisor yields null (the engine's `%` canonicalizes ±inf/NaN
         * to 0n), not a fabricated value. */
        if (nu || pnu || pv == 0) { o[i] = 0; ray_vec_set_null(out, i, true); }
        else o[i] = v / pv;
    }
    return out;
}

/* q `x wsum y` — weighted sum sum(x*y); `x wavg y` — (sum x*y) % sum x.
 * A pair where EITHER side is null is excluded (kdb).  An atom x broadcasts. */
static ray_t* q_weighted(ray_t* x, ray_t* y, int avg) {
    if (!x || !y) return ray_error("type", "wsum/wavg: nil operand");
    int xatom = ray_is_atom(x);
    if (!q_vec_is_num(y) && !ray_is_atom(y)) return ray_error("type", "wsum/wavg: numeric args only");
    if (!xatom && !q_vec_is_num(x)) return ray_error("type", "wsum/wavg: numeric args only");
    int64_t n = ray_is_atom(y) ? (xatom ? 1 : ray_len(x)) : ray_len(y);
    if (!xatom && !ray_is_atom(y) && ray_len(x) != ray_len(y))
        return ray_error("length", "wsum/wavg: length mismatch");
    double sp = 0, sw = 0;
    for (int64_t i = 0; i < n; i++) {
        int xn, yn;
        double xv = xatom ? q_velem_f(x, 0, &xn) : q_velem_f(x, i, &xn);
        double yv = ray_is_atom(y) ? q_velem_f(y, 0, &yn) : q_velem_f(y, i, &yn);
        if (xn || yn) continue;                 /* exclude null pairs */
        sp += xv * yv; sw += xv;
    }
    if (!avg) return ray_f64(sp);
    if (sw == 0) return ray_typed_null(-RAY_F64);
    return ray_f64(sp / sw);
}
static ray_t* q_wsum_wrap(ray_t* x, ray_t* y){ return q_weighted(x, y, 0); }
static ray_t* q_wavg_wrap(ray_t* x, ray_t* y){ return q_weighted(x, y, 1); }

/* q `x cov y` — population covariance (÷n); `x scov y` — sample (÷n-1).
 * Null pairs excluded. */
static ray_t* q_covariance(ray_t* x, ray_t* y, int sample) {
    if (!x || !y || !q_vec_is_num(x) || !q_vec_is_num(y))
        return ray_error("type", "cov: numeric vectors only");
    int64_t n = ray_len(x);
    if (n != ray_len(y)) return ray_error("length", "cov: length mismatch");
    double sx=0, sy=0; int64_t c=0;
    for (int64_t i = 0; i < n; i++) {
        int xn, yn; double xv=q_velem_f(x,i,&xn), yv=q_velem_f(y,i,&yn);
        if (xn||yn) continue;
        sx+=xv; sy+=yv; c++;
    }
    if (c == 0 || (sample && c < 2)) return ray_typed_null(-RAY_F64);
    double mx=sx/(double)c, my=sy/(double)c, acc=0;
    for (int64_t i = 0; i < n; i++) {
        int xn, yn; double xv=q_velem_f(x,i,&xn), yv=q_velem_f(y,i,&yn);
        if (xn||yn) continue;
        acc += (xv-mx)*(yv-my);
    }
    return ray_f64(acc / (double)(sample ? c-1 : c));
}
static ray_t* q_cov_wrap(ray_t* x, ray_t* y){ return q_covariance(x, y, 0); }
static ray_t* q_scov_wrap(ray_t* x, ray_t* y){ return q_covariance(x, y, 1); }

/* q sliding m-window family `N mf x` — window i covers x[max(0,i-N+1)..i].
 * msum treats null as 0; avg/max/min/dev/count exclude nulls; N<=0 -> empty
 * window (sum/count 0, others null). */
typedef enum { MW_SUM, MW_AVG, MW_MAX, MW_MIN, MW_COUNT, MW_DEV } q_mw_kind;

static ray_t* q_mwin(ray_t* nx, ray_t* x, q_mw_kind k) {
    if (!nx || !(nx->type==-RAY_I64||nx->type==-RAY_I32||nx->type==-RAY_I16))
        return ray_error("type", "m-window: left arg must be an int atom");
    if (!x || !q_vec_is_num(x)) {
        if (x && ray_is_atom(x)) { ray_retain(x); return x; }
        return ray_error("type", "m-window: numeric vector rhs");
    }
    int64_t N = q_iatom_val(nx);
    int64_t n = ray_len(x);
    int isf = q_vec_is_float(x);
    int8_t otype = (k==MW_SUM || k==MW_MAX || k==MW_MIN) ? (isf ? RAY_F64 : RAY_I64)
                 : (k==MW_COUNT) ? RAY_I64 : RAY_F64;
    ray_t* out = ray_vec_new(otype, n > 0 ? n : 1); out->len = n;
    void* o = ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int64_t lo = (N > 0 && i - N + 1 > 0) ? i - N + 1 : 0;
        if (N <= 0) lo = i + 1;                  /* empty window */
        double sum=0, sumsq=0, m=0; int64_t c=0; int started=0;
        for (int64_t j = lo; j <= i; j++) {
            int nu; double v = q_velem_f(x, j, &nu);
            if (k==MW_SUM) { if (!nu) sum += v; continue; }
            if (nu) continue;
            c++; sum += v; sumsq += v*v;
            if (!started) { m=v; started=1; }
            else if (k==MW_MAX ? v>m : v<m) m=v;
        }
        if (otype == RAY_I64) {
            if (k==MW_SUM)   ((int64_t*)o)[i] = (int64_t)sum;
            else if (k==MW_COUNT) ((int64_t*)o)[i] = c;
            else { if (started) ((int64_t*)o)[i] = (int64_t)m;   /* mmax/mmin */
                   else { ((int64_t*)o)[i] = NULL_I64; ray_vec_set_null(out, i, true); } }
        } else {
            double r; int isnull = 0;
            switch (k) {
            case MW_SUM: r = sum; break;
            case MW_MAX: case MW_MIN: if (started) r=m; else { r=0; isnull=1; } break;
            case MW_AVG: if (c) r=sum/(double)c; else { r=0; isnull=1; } break;
            case MW_DEV: if (c) { double mean=sum/(double)c; double var=sumsq/(double)c - mean*mean;
                                  r = var>0 ? sqrt(var) : 0; } else { r=0; isnull=1; } break;
            default: r = 0; break;
            }
            ((double*)o)[i] = r;
            if (isnull) ray_vec_set_null(out, i, true);
        }
    }
    return out;
}
static ray_t* q_msum_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_SUM); }
static ray_t* q_mavg_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_AVG); }
static ray_t* q_mmax_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_MAX); }
static ray_t* q_mmin_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_MIN); }
static ray_t* q_mcount_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_COUNT); }
static ray_t* q_mdev_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_DEV); }

/* q `a ema x` — exponential moving average: e[0]=x[0], e[i]=a*x[i]+(1-a)*e[i-1].
 * `a` is a float atom (the smoothing factor). */
static ray_t* q_ema_wrap(ray_t* a, ray_t* x) {
    if (!a || !ray_is_atom(a)) return ray_error("type", "ema: smoothing factor must be an atom");
    int an; double alpha = q_velem_f(a, 0, &an);
    if (!x || !q_vec_is_num(x)) {
        if (x && ray_is_atom(x)) { ray_retain(x); return x; }
        return ray_error("type", "ema: numeric vector rhs");
    }
    int64_t n = ray_len(x);
    ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
    double* o = (double*)ray_data(out);
    double e = 0; int started = 0;
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        double xv = nu ? 0 : v;
        if (!started) { e = xv; started = 1; }
        else e = alpha * xv + (1.0 - alpha) * e;
        o[i] = e;
    }
    return out;
}

/* q `neg` / monadic `-` — negate.  kdb negates a date's underlying day count
 * PRESERVING the type (function_neg.qcmd: neg 2000.01.01 2012.01.01 ->
 * 2000.01.01 1988.01.01; 0Wd <-> -0Wd; 0Nd passes through), where base
 * ray_neg_fn rejects temporals — so the date arm lives here and every other
 * input delegates.  Registered ATOMIC: eval's atomic_map_unary maps vectors
 * element-wise and its typed-out path already carries RAY_DATE, so a date
 * vector comes back a date vector.  time/timestamp arms arrive with their
 * datatypes (deferred). */
static ray_t* q_neg_wrap(ray_t* x) {
    if (x && x->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_date(-(int64_t)x->i32);
    }
    /* kdb `neg` promotes a boolean to INT and negates (`neg 1b` -> -1i);
     * base ray_neg_fn rejects bools.  Registered ATOMIC, so a bool vector
     * arrives here element-wise and the i32 atoms collapse to an i32 vector. */
    if (x && x->type == -RAY_BOOL)
        return ray_i32(-(int32_t)(x->b8 ? 1 : 0));
    return ray_neg_fn(x);
}

/* q `null x` — elementwise null test.  Drives the engine's atomic `nil?`
 * (ray_nil_fn) through atomic_map_unary so it broadcasts over typed vectors
 * AND nested general lists at every depth; collection_elem reconstructs
 * typed-null atoms, so nulls are SEEN (unlike other atomics, which stay
 * null-avoiding via the dispatch guards).  Registered RAY_FN_NONE — NOT
 * ATOMIC — so it receives the whole argument here and owns the collapse: a
 * heterogeneous input list yields a homogeneous bool-atom run that
 * q_collapse_list folds to a bool vector (`null (1;\`a;2.5;"x")` -> 0000b),
 * while a nested list yields a list of bool VECTORS that q_collapse_list
 * leaves intact (multi-line, `null (0N 1;2 0N)` -> 10b / 01b). */
/* q-layer null test: the base engine's `ray_nil_fn` treats sym id 0 as the
 * EMPTY symbol (a value, include/rayforce.h SYM case), but q treats the null
 * symbol `` ` `` AS null (`null \`` -> 1b).  This wrapper special-cases the
 * null symbol here in the q layer so the divergence stays out of base rayfall,
 * whose own paths rely on sym-0-as-empty.  Drives `q_null_wrap` for both the
 * atom path and the per-element `atomic_map_unary` recursion (nested lists /
 * symbol vectors reconstruct null-sym atoms via collection_elem). */
static ray_t* q_nil_fn(ray_t* x) {
    if (q_is_null_sym(x)) return ray_bool(true);
    return ray_nil_fn(x);
}

static ray_t* q_null_wrap(ray_t* x) {
    ray_t* r = is_collection(x) ? atomic_map_unary(q_nil_fn, x) : q_nil_fn(x);
    if (!r || RAY_IS_ERR(r) || r->type != RAY_LIST) return r;
    ray_t* c = q_collapse_list(r);   /* owned: retains-or-builds */
    ray_release(r);
    return c;
}

/* q `x within y` — bounds check (ref/within.md: 1 3 10 6 4 within 2 6 ->
 * 01011b; inclusive).  Base ray_within_fn takes VECTOR vals only and reads
 * the range buffer at the vals' element width, so: an atom x is enlisted
 * (via list+collapse) and the answer unwrapped back to a bool atom, and the
 * two element widths must agree ('type — a silent misread otherwise).  The
 * flip-of-pairs range form and mixed-width operands are deferred cells. */
static ray_t* q_within_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "within: nil operand");
    if (!ray_is_vec(y) || ray_len(y) != 2)
        return ray_error("type", "within: range must be a 2-item vector");
    ray_t* vals = x;
    ray_t* vals_owned = NULL;
    if (ray_is_atom(x)) {
        ray_t* l = ray_list_new(1);
        if (RAY_IS_ERR(l)) return l;
        l = ray_list_append(l, x);
        if (RAY_IS_ERR(l)) return l;
        vals_owned = q_collapse_list(l);
        ray_release(l);
        if (!vals_owned || RAY_IS_ERR(vals_owned))
            return vals_owned ? vals_owned : ray_error("type", NULL);
        if (!ray_is_vec(vals_owned)) {           /* strings & friends: deferred */
            ray_release(vals_owned);
            return ray_error("type", "within: unsupported value type (deferred)");
        }
        vals = vals_owned;
    }
    if (!ray_is_vec(vals)) {
        if (vals_owned) ray_release(vals_owned);
        return ray_error("type", "within: unsupported value type (deferred)");
    }
    /* Base ray_within_fn dispatches on vals->type ONLY and reads the range
     * buffer as that element type, so ANY type mismatch — not just a width
     * mismatch — would silently reinterpret raw bits (codex: 1 2 within
     * 1.5 2.5 read the doubles as int64 -> 00b).  Same-type operands only;
     * mixed-type coercion is a deferred cell (error, never a wrong answer). */
    if (vals->type != y->type) {
        if (vals_owned) ray_release(vals_owned);
        return ray_error("type", "within: value/range types must match (mixed-type deferred)");
    }
    ray_t* r;
    if (vals->type == RAY_TIMESTAMP) {
        /* base ray_within_fn has no i64-temporal arm; the payload is i64, so
         * relabel both sides through the one cast home and delegate (the
         * same-byte-rep TIMESTAMP<->I64 relabel, builtins.c). */
        ray_t* vi = q_cast_to(RAY_I64, vals);
        if (!vi || RAY_IS_ERR(vi)) { if (vals_owned) ray_release(vals_owned); return vi; }
        ray_t* yi = q_cast_to(RAY_I64, y);
        if (!yi || RAY_IS_ERR(yi)) {
            ray_release(vi);
            if (vals_owned) ray_release(vals_owned);
            return yi;
        }
        r = ray_within_fn(vi, yi);
        ray_release(vi);
        ray_release(yi);
    } else {
        r = ray_within_fn(vals, y);
    }
    if (!vals_owned) return r;                    /* vector x: pass through */
    ray_release(vals_owned);
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* idx = ray_i64(0);                      /* atom x: unwrap 1-vec */
    ray_t* a = ray_at_fn(r, idx);
    ray_release(idx);
    ray_release(r);
    return a;
}

/* ===== q list verbs (feat/q-list-verbs) ===================================
 * rotate / sublist / next / prev / fill (`^`).  kdb rotate.md, sublist.md,
 * next.md, prev.md, and `^` (fill).  rotate/sublist reuse q_gather (index-vector
 * gather via ray_at_fn, collapsed to a typed vector; string-aware); next/prev do
 * a typed-vector shift with a sentinel-null fill; fill coalesces nulls.  Dict /
 * table / mixed-list / non-nullable-type forms are DEFERRED cells (error, never
 * a wrong answer) — their ledger rows live in the *-deferred.qcmd companions. */

/* q `n rotate x` — cyclic shift left by n (negative = right; kdb rotate.md).
 * n is an int atom; the right arg is a vector/list/string.  Empty x returns a
 * copy (no modulo-by-zero).  Reuses q_gather with recycle=1. */
static ray_t* q_rotate_wrap(ray_t* n, ray_t* x) {
    if (!n || !(n->type == -RAY_I64 || n->type == -RAY_I32 || n->type == -RAY_I16))
        return ray_error("type", "rotate: left arg must be an int atom");
    if (RAY_ATOM_IS_NULL(n))
        return ray_error("nyi", "rotate: 0N left arg is deferred");
    if (!x || (!ray_is_vec(x) && x->type != RAY_LIST && x->type != -RAY_STR))
        return ray_error("type", "rotate: right arg must be a list, vector, or string (table deferred)");
    int64_t total = (x->type == -RAY_STR) ? (int64_t)ray_str_len(x) : ray_len(x);
    if (total <= 0) { ray_retain(x); return x; }
    int64_t k = q_iatom_val(n);
    k = ((k % total) + total) % total;         /* normalize into [0,total) */
    return q_gather(x, k, total, total, 1);     /* recycle -> cyclic */
}

/* q `n sublist x` — sub-list (kdb sublist.md).  Atom n>=0 -> first min(n,len);
 * n<0 -> last min(|n|,len).  Int-pair `i j` -> j items from position i (both
 * clamped into range; TRUNCATING, never null-extending).  Reuses q_gather with
 * recycle=0.  Dict / table right args are deferred. */
static ray_t* q_sublist_wrap(ray_t* n, ray_t* x) {
    if (!x || (!ray_is_vec(x) && x->type != RAY_LIST && x->type != -RAY_STR))
        return ray_error("type", "sublist: right arg must be a list, vector, or string (dict/table deferred)");
    int64_t total = (x->type == -RAY_STR) ? (int64_t)ray_str_len(x) : ray_len(x);
    int64_t start, count;
    if (n && (n->type == -RAY_I64 || n->type == -RAY_I32 || n->type == -RAY_I16)) {
        if (RAY_ATOM_IS_NULL(n)) return ray_error("nyi", "sublist: 0N left arg is deferred");
        int64_t k = q_iatom_val(n);
        if (k >= 0) { start = 0; count = (k < total) ? k : total; }
        else { int64_t kk = -k; count = (kk < total) ? kk : total; start = total - count; }
    } else if (n && (n->type == RAY_I64 || n->type == RAY_I32 || n->type == RAY_I16) &&
               ray_len(n) == 2) {
        int64_t i = q_ivec_get(n, 0), j = q_ivec_get(n, 1);
        if (i < 0) i = 0;                        /* clamp negative start to 0 */
        if (j < 0) j = 0;
        start = (i < total) ? i : total;
        count = j;
        if (start + count > total) count = total - start;   /* truncate tail */
        if (count < 0) count = 0;
    } else {
        return ray_error("type", "sublist: left arg must be an int atom or a 2-item int vector");
    }
    return q_gather(x, start, count, total, 0);  /* truncating, no recycle */
}

/* q `next x` / `prev x` — shift a simple vector by one, null-filling the vacated
 * end (kdb next.md / prev.md).  Restricted to the sentinel-nullable element
 * types (int / float / temporal): SYM/BOOL/U8/STR/LIST/atom forms have no
 * shift-in null here and are deferred cells. */
static ray_t* q_shift1(ray_t* x, int forward) {
    if (!x || !ray_is_vec(x))
        return ray_error("nyi", "next/prev: only simple numeric vectors (list/string/sym/atom deferred)");
    int8_t t = x->type;
    if (!(t == RAY_I16 || t == RAY_I32 || t == RAY_I64 || t == RAY_F32 || t == RAY_F64 ||
          t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP))
        return ray_error("nyi", "next/prev: %s vectors are deferred", ray_type_name(t));
    int64_t len = ray_len(x);
    size_t esz = ray_type_sizes[(uint8_t)t];
    ray_t* out = ray_vec_new(t, len > 0 ? len : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = len;
    char* o = (char*)ray_data(out);
    const char* in = (const char*)ray_data(x);
    if (len > 0) {
        if (forward) {                           /* next: o[i]=x[i+1], tail null */
            if (len > 1) memcpy(o, in + esz, (size_t)(len - 1) * esz);
            ray_vec_set_null(out, len - 1, true);
        } else {                                 /* prev: o[i]=x[i-1], head null */
            if (len > 1) memcpy(o + esz, in, (size_t)(len - 1) * esz);
            ray_vec_set_null(out, 0, true);
        }
    }
    return out;
}
static ray_t* q_next_wrap(ray_t* x) { return q_shift1(x, 1); }
static ray_t* q_prev_wrap(ray_t* x) { return q_shift1(x, 0); }

/* q `x^y` — fill: coalesce nulls in y with x (kdb `^`).  x may be an atom
 * (broadcast) or a same-length vector (element-wise).  Numeric result type:
 * F64 if EITHER operand is float, else I64 (the narrower-int-preserving lattice
 * is a deferred refinement — the `type` ledger rows that need it are blocked by
 * a separate `0n 2 3i` parse bug and split out).  Symbol fill is a distinct
 * path.  Dict / `fills` forward-fill / table / fill-scan forms are deferred. */
static int q_is_float_t(int8_t t) {
    return t == RAY_F64 || t == RAY_F32 || t == -RAY_F64 || t == -RAY_F32;
}
static int q_is_num_t(int8_t t) {
    return t == RAY_BOOL || t == RAY_U8 || t == RAY_I16 || t == RAY_I32 || t == RAY_I64 ||
           t == RAY_F32 || t == RAY_F64 || t == -RAY_BOOL || t == -RAY_U8 || t == -RAY_I16 ||
           t == -RAY_I32 || t == -RAY_I64 || t == -RAY_F32 || t == -RAY_F64;
}
static int q_is_sym_t(int8_t t) { return t == RAY_SYM || t == -RAY_SYM; }

static ray_t* q_fill_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "^: nil operand");
    int xatom = ray_is_atom(x), yatom = ray_is_atom(y);

    /* ---- symbol fill ---- */
    if (q_is_sym_t(x->type) || q_is_sym_t(y->type)) {
        if (!q_is_sym_t(x->type) || !q_is_sym_t(y->type))
            return ray_error("type", "^: symbol fill needs symbol operands");
        /* length follows y when it is a vector; a scalar y broadcasts to the
         * length of a vector x (`` `a`b`c^` `` -> 3 items), matching the
         * numeric branch below. */
        int64_t len = yatom ? (xatom ? 1 : ray_len(x)) : ray_len(y);
        if (!xatom && !yatom && ray_len(x) != len)
            return ray_error("length", "^: operand lengths must match");
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
        ray_t* c = q_collapse_list(outl); ray_release(outl);
        if (yatom && xatom) {                    /* scalar^scalar -> atom */
            ray_t* ia = ray_i64(0); ray_t* a = ray_at_fn(c, ia); ray_release(ia); ray_release(c);
            return a;
        }
        return c;
    }

    /* ---- numeric fill ---- */
    if (!q_is_num_t(x->type) || !q_is_num_t(y->type))
        return ray_error("type", "^: unsupported operand types (dict/table/list fill deferred)");
    int is_float = q_is_float_t(x->type) || q_is_float_t(y->type);
    int64_t len = yatom ? (xatom ? 1 : ray_len(x)) : ray_len(y);
    if (!xatom && !yatom && ray_len(x) != ray_len(y))
        return ray_error("length", "^: operand lengths must match");
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

/* A keyed table is a RAY_DICT whose keys AND values are both tables. */
static int q_is_keyed_table(ray_t* y) {
    if (!y || y->type != RAY_DICT) return 0;
    ray_t* k = ray_dict_keys(y);
    ray_t* v = ray_dict_vals(y);
    return k && v && k->type == RAY_TABLE && v->type == RAY_TABLE;
}

/* Flatten a plain-or-keyed table to a single plain table (key cols then value
 * cols).  Returns owned. */
static ray_t* q_table_flatten(ray_t* y) {
    if (y->type == RAY_TABLE) { ray_retain(y); return y; }
    ray_t* kt = ray_dict_keys(y);          /* borrowed */
    ray_t* vt = ray_dict_vals(y);          /* borrowed */
    int64_t knc = ray_table_ncols(kt), vnc = ray_table_ncols(vt);
    ray_t* out = ray_table_new(knc + vnc > 0 ? knc + vnc : 1);
    for (int64_t c = 0; c < knc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(kt, c), ray_table_get_col_idx(kt, c));
    for (int64_t c = 0; c < vnc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(vt, c), ray_table_get_col_idx(vt, c));
    return out;
}

/* q enkey/unkey `N!table`: 0 -> plain table (unkey), N>0 -> key the first N
 * columns into a keyed table (RAY_DICT keycols-table -> valcols-table).
 * Accepts a plain OR already-keyed table (re-keys).  Consumes nothing. */
static ray_t* q_enkey(ray_t* y, int64_t nkey) {
    ray_t* flat = q_table_flatten(y);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    int64_t nc = ray_table_ncols(flat);
    if (nkey <= 0) return flat;                 /* unkey */
    if (nkey >= nc) { ray_release(flat); return ray_error("length", "!: key count exceeds columns"); }
    ray_t* kt = ray_table_new(nkey);
    ray_t* vt = ray_table_new(nc - nkey);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(kt) && !RAY_IS_ERR(vt); c++) {
        int64_t nm = ray_table_col_name(flat, c);
        ray_t* col = ray_table_get_col_idx(flat, c);
        if (c < nkey) kt = ray_table_add_col(kt, nm, col);
        else          vt = ray_table_add_col(vt, nm, col);
    }
    ray_release(flat);
    if (RAY_IS_ERR(kt)) { ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);
}

/* q `x!y` — dict make.  An atom key enlists to a 1-vector first (kdb `a!1`
 * is a dict too; rayfall dict wants vector keys); vals pass through as-is
 * (rayfall dict broadcasts atom vals and boxes the rest itself).  kdb
 * 'length fidelity: rayfall would null-fill a short vals side, so the
 * count check lives here.  String keys are a deferred cell (string model). */
static ray_t* q_bang_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "!: nil operand");
    /* kdb reserves a NEGATIVE integer ATOM lhs for internal functions
     * (`-8!x` serialize, `-9!x` deserialize, ...) — never dict-make.
     * Typed nulls fall through to dict-make (0N is not an internal id). */
    if ((x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16) &&
        !RAY_ATOM_IS_NULL(x)) {
        int64_t id = x->type == -RAY_I64 ? x->i64
                   : x->type == -RAY_I32 ? (int64_t)x->i32 : (int64_t)x->i16;
        if (id < 0) {
            switch (id) {
            case -8: return q_wire_serialize(y, Q_WIRE_ASYNC);
            case -9:
                if (y->type != RAY_U8)
                    return ray_error("type", "-9!: expects a byte vector");
                return q_wire_deserialize(y);
            default:
                return ray_error("nyi", "internal function %lld! not yet implemented", (long long)id);
            }
        }
        /* q enkey/unkey: `N!table` / `N!keyedtable` (N>=0). */
        if (y->type == RAY_TABLE || q_is_keyed_table(y))
            return q_enkey(y, id);
        /* by-reference `N!`name`: unkey/enkey the named global IN PLACE, then
         * return the name (kdb amend-by-reference).  A miss / non-table stays
         * dict-make below. */
        if (y->type == -RAY_SYM) {
            ray_t* g = ray_env_get(y->i64);
            if (g && (g->type == RAY_TABLE || q_is_keyed_table(g))) {
                ray_t* nt = q_enkey(g, id);
                if (!nt || RAY_IS_ERR(nt)) return nt;
                ray_env_bind(y->i64, nt);      /* retains */
                ray_release(nt);
                ray_retain(y);
                return y;
            }
        }
    }
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

/* ===== q `value` (ref/value.md) — the full form matrix =====================
 * value is monadic; its ONE argument selects the behaviour by shape:
 *   dict            -> vals (collapsed)
 *   symbol atom     -> value of the variable it names (env / .q namespace / kw)
 *   enumeration     -> [deferred: no enum type]
 *   string          -> parse+lower+eval in the current context
 *   list            -> SINGLE apply/index of the head over the rest (a value
 *                      OBJECT apply — NOT eval's recursive parse-tree walk; if the
 *                      head is a symbol/string it is resolved first)
 *   projection      -> (function; bound args…)
 *   derived function-> argument of the outer iterator
 *   operator        -> internal opcode integer
 *   lambda          -> the V3.5 structure list
 * eval-vs-value: `value(f;args)` is ONE apply of f to the args-as-data; `eval`
 * would descend recursively.  They coincide only for flat args (ARCHITECTURE.md).
 */

static ray_t* q_call_n(ray_t* f, ray_t** a, int64_t k);   /* fwd; defined ~2726 */

/* Parse+lower+eval a q source string to a value (runtime-only; the registry is
 * warm here — rule 6 forbids q_parse in BUILDERS, not runtime wrappers, per the
 * q_select_exec precedent).  Returns owned value or RAY_ERROR. */
static ray_t* q_value_eval_str(ray_t* strv) {
    const char* sp = ray_str_ptr(strv);
    size_t sl = ray_str_len(strv);
    char* s = malloc(sl + 1);
    if (!s) return ray_error("wsfull", "value: out of memory");
    memcpy(s, sp, sl);
    s[sl] = '\0';
    ray_t* ast = q_parse(s);
    free(s);
    if (!ast || RAY_IS_ERR(ast)) return ast ? ast : ray_error("parse", NULL);
    ast = q_lower(ast);
    if (!ast || RAY_IS_ERR(ast)) return ast ? ast : ray_error("parse", NULL);
    ray_t* r = ray_eval(ast);
    ray_release(ast);
    return r;
}

/* Resolve a q symbol atom to its value (ref/value.md: "symbol atom -> value of
 * the variable it names").  Order: env (user vars + builtins) for the full name;
 * else strip a leading `.q.` and retry env; else the registry keyword table
 * (monadic then dyadic).  Returns a BORROWED ref (env/registry own it) or NULL. */
static ray_t* q_value_resolve_sym(ray_t* symv) {
    if (!symv || symv->type != -RAY_SYM) return NULL;
    ray_t* s = ray_sym_str(symv->i64);
    if (!s) return NULL;
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    ray_t* r = ray_env_get(symv->i64);            /* borrowed; full name */
    const char* base = nm; size_t bl = l;
    if (!r && l > 3 && nm[0] == '.' && nm[1] == 'q' && nm[2] == '.') {
        base = nm + 3; bl = l - 3;
        int64_t bid = ray_sym_intern(base, bl);
        r = ray_env_get(bid);                     /* borrowed */
    }
    if (!r) {
        r = q_registry_lookup_name(base, bl, Q_MONADIC);   /* borrowed */
        if (!r) r = q_registry_lookup_name(base, bl, Q_DYADIC);
    }
    ray_release(s);
    return r;                                     /* borrowed */
}

/* Single-apply a resolved head VALUE to argc data args (borrowed).  Value-object
 * apply: args are DATA — q_call_n routes callables (call_fn1/2, carrier/lambda-
 * aware) and INDEXES a noun head, never re-evaluating the args.  argc==0 => the
 * head itself (retained).  Returns owned result or RAY_ERROR. */
static ray_t* q_value_apply_head(ray_t* head, ray_t** args, int64_t argc) {
    if (!head) return ray_error("type", "value: nil head");
    if (argc == 0) { ray_retain(head); return head; }
    if (argc > 64) return ray_error("limit", "value: too many args");
    return q_call_n(head, args, argc);            /* borrowed args, owned result */
}

/* True iff `base` is one of the six adverb HOF values a q derived function
 * projects over (over/scan/each/prior/map-right/map-left).  A Q_DERIV_PROJ over
 * one of these is a DERIVED FUNCTION (value -> the iterator's argument); over
 * anything else it is a plain PROJECTION (value -> function + bound args).  The
 * env HOF ids are interned once (lazy), mirroring q_deriv's marker cache. */
static int q_value_is_adverb_hof(ray_t* base) {
    if (!base) return 0;
    /* Re-intern the env HOF ids per call: the sym table is recreated per runtime
     * (q_runtime_destroy tears it down), so a `static` id cache would go STALE for
     * a new runtime in the same process (the C unit suites do exactly that).
     * `value` is not hot, and ray_sym_intern is a cheap hash lookup when present. */
    return base == q_registry_over_value()
        || base == ray_env_get(ray_sym_intern("fold", 4))
        || base == q_registry_scan_value()
        || base == q_registry_lookup_name("each", 4, Q_DYADIC)
        || base == q_registry_prior_value()
        || base == ray_env_get(ray_sym_intern("map-right", 9))
        || base == ray_env_get(ray_sym_intern("map-left", 8));
}

ray_t* q_value_wrap(ray_t* x) {
    if (!x) return ray_error("type", "value: nil");

    /* ---- carriers (all RAY_LIST — matched BEFORE the general-list arm) ---- */
    q_deriv_kind dk = q_deriv_kind_of(x);

    /* lambda carrier -> the kdb lambda-structure list (ref/value.md#lambda,
     * V3.5 shape, best-effort fields):
     *   [0] bytecode (empty 0x)  [1] params  [2] locals  [3] globals
     *   [4] source map -1  [5] name ""  [6] file ""  [7] line -1  [8] source
     * The doc-pinned fields are [1] (params) and LAST (source text). */
    if (dk == Q_DERIV_LAMBDA) {
        ray_t* lam = q_deriv_base(x);                 /* borrowed */
        ray_t* src = q_lambda_src(x);                 /* borrowed */
        if (!lam || !src) return ray_error("type", "value: malformed lambda");
        ray_t* l = ray_list_new(9);
        ray_t* t;
        t = ray_vec_new(RAY_U8, 0);           l = ray_list_append(l, t); ray_release(t);
        l = ray_list_append(l, LAMBDA_PARAMS(lam));   /* borrowed; append retains */
        t = ray_sym_vec_new(RAY_SYM_W64, 0);  l = ray_list_append(l, t); ray_release(t);
        t = ray_sym_vec_new(RAY_SYM_W64, 0);  l = ray_list_append(l, t); ray_release(t);
        t = ray_i64(-1);                      l = ray_list_append(l, t); ray_release(t);
        t = ray_str("", 0);                   l = ray_list_append(l, t); ray_release(t);
        t = ray_str("", 0);                   l = ray_list_append(l, t); ray_release(t);
        t = ray_i64(-1);                      l = ray_list_append(l, t); ray_release(t);
        l = ray_list_append(l, src);
        return l;
    }

    if (dk == Q_DERIV_PROJ) {
        ray_t* base   = q_deriv_base(x);              /* borrowed, idx 1 */
        int64_t n     = ray_len(x);
        ray_t** e     = (ray_t**)ray_data(x);
        if (q_value_is_adverb_hof(base)) {
            /* derived function -> the argument of the iterator (idx 4).  The
             * adverb-derived carrier always binds EXACTLY the inner value there
             * (q_lower builds it as `(hof; V, hole)`); assert the invariant so a
             * future multi-arg adverb projection can't silently drop args. */
            uint64_t mask = q_deriv_hole_mask(x);
            int64_t bound = 0;
            for (int64_t i = 0; i < n - 4; i++) if (!(mask & (1ull << i))) bound++;
            if (n < 5 || bound != 1 || !e[4])
                return ray_error("type", "value: unexpected derived-fn shape");
            ray_retain(e[4]);
            return e[4];
        }
        /* plain projection -> (base; bound-args…), holes omitted (ref/value.md:
         * "list: function followed by argument/s"). */
        uint64_t mask = q_deriv_hole_mask(x);
        int64_t slots = n - 4;
        ray_t* out = ray_list_new(slots + 1);
        out = ray_list_append(out, base);             /* borrowed; append retains */
        for (int64_t i = 0; i < slots; i++)
            if (!(mask & (1ull << i))) out = ray_list_append(out, e[4 + i]);
        return out;
    }

    if (dk == Q_DERIV_MONAD || dk == Q_DERIV_COMPOSE)
        return ray_error("nyi", "value: composition/monadic-mark deferred");

    /* ---- symbol atom -> variable/keyword value ---- */
    if (x->type == -RAY_SYM) {
        ray_t* v = q_value_resolve_sym(x);            /* borrowed */
        if (!v) return ray_error("name", "value: unresolved symbol");
        ray_retain(v);
        return v;
    }

    /* ---- string atom -> evaluate as q source ---- */
    if (x->type == -RAY_STR) return q_value_eval_str(x);

    /* ---- general list -> single apply/index of the head over the rest ---- */
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (n < 1) return ray_error("type", "value: empty list");
        ray_t* head = e[0];                           /* borrowed */
        if (head && head->type == -RAY_SYM) {         /* form 2: symbol head */
            ray_t* hv = q_value_resolve_sym(head);    /* borrowed */
            if (!hv) return ray_error("name", "value: unresolved symbol head");
            return q_value_apply_head(hv, e + 1, n - 1);
        }
        if (head && head->type == -RAY_STR) {         /* form 3: string head */
            ray_t* hv = q_value_eval_str(head);       /* owned */
            if (!hv || RAY_IS_ERR(hv)) return hv ? hv : ray_error("parse", NULL);
            ray_t* r = q_value_apply_head(hv, e + 1, n - 1);
            ray_release(hv);
            return r;
        }
        return q_value_apply_head(head, e + 1, n - 1);  /* form 1: value head */
    }

    /* ---- dict -> vals (collapsed) ---- */
    if (x->type == RAY_DICT) {
        ray_t* v = ray_dict_vals(x);                /* borrowed */
        if (!v) return ray_error("type", "value: nil vals");
        if (v->type == RAY_LIST) return q_collapse_list(v);   /* returns owned */
        ray_retain(v);
        return v;
    }

    return ray_error("type", "value: unsupported operand (other forms deferred)");
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

/* q `x union y` — `distinct x,y` (ref/union.md).  A wrapper because rayfall's
 * ray_union_fn KEEPS x-duplicates (it only filters y against x); kdb dedups
 * the whole join in first-occurrence order.  Reuses q join (`,` == rayfall
 * concat) + the q distinct wrapper above — no new set logic.  Operands the
 * distinct wrapper defers (strings, tables) defer here too: error, never a
 * wrong answer. */
static ray_t* q_union_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "union: nil operand");
    ray_t* j = ray_concat_fn(x, y);
    if (!j || RAY_IS_ERR(j)) return j;
    ray_t* r = q_distinct_wrap(j);
    ray_release(j);
    return r;
}

/* q `x inter y` — items of x that are in y, x-duplicates and order kept
 * (ref/inter.md).  rayfall `sect` (ray_sect_fn) IS this for lists, but on
 * DICT operands it returns a wrong-shaped dict where kdb returns the common
 * VALUES as a list — so dict/table operands are guarded 'nyi (error, never a
 * wrong answer); everything else delegates to ray_sect_fn. */
static ray_t* q_inter_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "inter: nil operand");
    if (x->type == RAY_DICT || x->type == RAY_TABLE ||
        y->type == RAY_DICT || y->type == RAY_TABLE)
        return ray_error("nyi", "inter: dict/table operands deferred");
    return ray_sect_fn(x, y);
}

/* q `x cross y` — Cartesian product, `{raze x,/:\:y}` (ref/cross.md): for
 * each item a of x (in order), for each item b of y, the JOIN `a,b`.
 * Composes existing primitives (ray_at_fn item access + q join == rayfall
 * concat) — rayfall has no cartesian primitive.  Atom operands behave as
 * one-item lists (each-left/right over an atom).  Deferred cells ('nyi,
 * never a wrong answer): string operands (kdb iterates a string's CHARS;
 * openq strings are -RAY_STR atoms — string model) and dict/table cross
 * (kdb cross-joins tables). */
static ray_t* q_cross_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "cross: nil operand");
    if (x->type == -RAY_STR || y->type == -RAY_STR)
        return ray_error("nyi", "cross: string operands deferred (string model)");
    if (x->type == RAY_DICT || x->type == RAY_TABLE ||
        y->type == RAY_DICT || y->type == RAY_TABLE)
        return ray_error("nyi", "cross: dict/table operands deferred");
    int xl = ray_is_vec(x) || x->type == RAY_LIST;
    int yl = ray_is_vec(y) || y->type == RAY_LIST;
    int64_t nx = xl ? ray_len(x) : 1;
    int64_t ny = yl ? ray_len(y) : 1;
    ray_t* out = ray_list_new(nx * ny > 0 ? nx * ny : 1);
    for (int64_t i = 0; i < nx; i++) {
        ray_t* a;
        if (xl) { ray_t* ia = ray_i64(i); a = ray_at_fn(x, ia); ray_release(ia); }
        else    { ray_retain(x); a = x; }
        if (!a || RAY_IS_ERR(a)) { ray_release(out); return a; }
        for (int64_t j = 0; j < ny; j++) {
            ray_t* b;
            if (yl) { ray_t* ja = ray_i64(j); b = ray_at_fn(y, ja); ray_release(ja); }
            else    { ray_retain(y); b = y; }
            if (!b || RAY_IS_ERR(b)) { ray_release(a); ray_release(out); return b; }
            ray_t* p = ray_concat_fn(a, b);
            ray_release(b);
            if (!p || RAY_IS_ERR(p)) { ray_release(a); ray_release(out); return p; }
            out = ray_list_append(out, p);
            if (RAY_IS_ERR(out)) { ray_release(p); ray_release(a); return out; }
            ray_release(p);
        }
        ray_release(a);
    }
    return out;
}

/* ===== q sort / grade / bucket family (feat/q-sort-rank) ===================
 * Flat typed-vector cores reuse the rayfall primitives verbatim (ray_asc_fn,
 * ray_desc_fn, ray_iasc_fn, ray_idesc_fn, ray_xbar_fn); the q wrappers add the
 * arg-swap (xbar) and the DICT container arms.  DEFERRED (error, never a wrong
 * answer): the sorted `s#` attribute on asc/desc results (rayfall has no sorted
 * attribute bit — the VALUE is kdb-true, only the attribute display diverges),
 * the mixed-general-list-by-type-number sort, and table / keyed-table sorts. */

/* Reorder a keys-or-vals vector by a grade-index vector (owned grade), then
 * collapse the boxed result back to a typed vector.  Releases `grade`. */
static ray_t* q_reindex_collapse(ray_t* vec, ray_t* grade) {
    if (!grade || RAY_IS_ERR(grade)) return grade;
    ray_t* boxed = ray_at_fn(vec, grade);
    ray_release(grade);
    if (!boxed || RAY_IS_ERR(boxed)) return boxed;
    if (boxed->type == RAY_LIST) {
        ray_t* c = q_collapse_list(boxed);
        ray_release(boxed);
        return c;
    }
    return boxed;
}

/* Grade a dict's VALUE vector.  Dict vals are stored as a boxed RAY_LIST, so
 * collapse to a typed vector before grading (a genuinely mixed value list can't
 * collapse and ray_iasc_fn errors → the by-type-number sort is DEFERRED). */
static ray_t* q_dict_value_grade(ray_t* vals, int desc) {
    ray_t* cv = (vals && vals->type == RAY_LIST) ? q_collapse_list(vals) : NULL;
    ray_t* use = cv ? cv : vals;
    /* Empty dict (e.g. `asc 0#d`): an empty value list can't be graded by
     * ray_iasc_fn (it needs a typed vector, and q_collapse_list leaves an empty
     * RAY_LIST as-is), so return an empty long grade — reindexing then yields an
     * empty dict / key list, matching kdb (codex review). */
    if (use && (ray_is_vec(use) || use->type == RAY_LIST) && ray_len(use) == 0) {
        if (cv) ray_release(cv);
        ray_t* g = ray_vec_new(RAY_I64, 0);
        if (g && !RAY_IS_ERR(g)) g->len = 0;
        return g;
    }
    ray_t* grade = desc ? ray_idesc_fn(use) : ray_iasc_fn(use);
    if (cv) ray_release(cv);
    return grade;
}

/* q `asc`/`desc` on a DICT — sort the entries by VALUE (ascending / descending),
 * carrying the keys along (kdb ref/asc.md, ref/desc.md dictionary form).  Grades
 * the values, then reindexes both keys and vals by that grade. */
static ray_t* q_sort_dict(ray_t* d, int desc) {
    ray_t* keys = ray_dict_keys(d);      /* borrowed */
    ray_t* vals = ray_dict_vals(d);      /* borrowed */
    if (!keys || !vals) return ray_error("type", "asc/desc: malformed dict");
    ray_t* grade = q_dict_value_grade(vals, desc);
    if (!grade || RAY_IS_ERR(grade)) return grade ? grade : ray_error("oom", NULL);
    ray_retain(grade);                   /* one ref per reindex call */
    ray_t* nk = q_reindex_collapse(keys, grade);        /* releases its grade ref */
    if (!nk || RAY_IS_ERR(nk)) { ray_release(grade); return nk; }
    ray_t* nv = q_reindex_collapse(vals, grade);        /* releases the retained ref */
    if (!nv || RAY_IS_ERR(nv)) { ray_release(nk); return nv; }
    return ray_dict_new(nk, nv);         /* consumes nk, nv */
}

/* q `iasc`/`idesc` on a DICT — return the KEYS in ascending / descending VALUE
 * order (kdb ref/asc.md grade form: `iasc d` grades the values, indexes keys). */
static ray_t* q_grade_dict(ray_t* d, int desc) {
    ray_t* keys = ray_dict_keys(d);      /* borrowed */
    ray_t* vals = ray_dict_vals(d);      /* borrowed */
    if (!keys || !vals) return ray_error("type", "iasc/idesc: malformed dict");
    ray_t* grade = q_dict_value_grade(vals, desc);
    return q_reindex_collapse(keys, grade);             /* releases grade */
}

static ray_t* q_asc_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT) return q_sort_dict(x, 0);
    return ray_asc_fn(x);
}
static ray_t* q_desc_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT) return q_sort_dict(x, 1);
    return ray_desc_fn(x);
}
static ray_t* q_iasc_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT) return q_grade_dict(x, 0);
    return ray_iasc_fn(x);
}
static ray_t* q_idesc_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT) return q_grade_dict(x, 1);
    return ray_idesc_fn(x);
}

/* q `width xbar list` — interval bucketing.  rayfall ray_xbar_fn is (col,
 * bucket); q spells it (bucket, col), so swap the arguments.  Everything else
 * (numeric int/float bucket, temporal cols, list zip) is handled by the base
 * kernel; dict/keyed-table/qSQL forms fall through to whatever the base does. */
static ray_t* q_xbar_wrap(ray_t* bucket, ray_t* col) {
    return ray_xbar_fn(col, bucket);
}

/* Deal n distinct values from [0,total) — partial Fisher-Yates over `til total`,
 * take the first n (kdb deal / permute; uses the same libc rand() the roll path
 * does).  n<=total required.  Result is an owned I64 vector. */
static ray_t* q_deal_indices(int64_t n, int64_t total) {
    if (n < 0) return ray_error("domain", "?: deal count must be non-negative");
    if (n > total) return ray_error("length", "?: cannot deal %lld from %lld",
                                    (long long)n, (long long)total);
    ray_t* arr = ray_vec_new(RAY_I64, total > 0 ? total : 1);
    if (RAY_IS_ERR(arr)) return arr;
    arr->len = total;
    int64_t* a = (int64_t*)ray_data(arr);
    for (int64_t i = 0; i < total; i++) a[i] = i;
    for (int64_t i = 0; i < n; i++) {
        int64_t range = total - i;
        int64_t j = i + (int64_t)(rand() % range);
        int64_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
    arr->len = n;                          /* take first n (buffer already sized) */
    return arr;
}

/* Deal/permute n indices then gather them from the list y (collapsed). */
static ray_t* q_deal_pick(int64_t n, ray_t* y) {
    ray_t* idx = q_deal_indices(n, ray_len(y));
    if (!idx || RAY_IS_ERR(idx)) return idx;
    ray_t* out = ray_at_fn(y, idx);
    ray_release(idx);
    if (out && out->type == RAY_LIST) { ray_t* c = q_collapse_list(out); ray_release(out); return c; }
    return out;
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
        if (RAY_ATOM_IS_NULL(x)) {                  /* 0N ? y — permute all items */
            if (y && (y->type == -RAY_I64 || y->type == -RAY_I32)) {
                int64_t m = (y->type == -RAY_I64) ? y->i64 : y->i32;
                if (m < 0) return ray_error("type", "?: permute needs a non-negative count");
                return q_deal_indices(m, m);
            }
            if (y && (ray_is_vec(y) || y->type == RAY_LIST)) return q_deal_pick(ray_len(y), y);
            return ray_error("nyi", "?: permute of this operand is deferred");
        }
        if (nx < 0) {                               /* -n ? y — deal, no replacement */
            int64_t n = -nx;
            if (y && (y->type == -RAY_I64 || y->type == -RAY_I32)) {
                int64_t m = (y->type == -RAY_I64) ? y->i64 : y->i32;
                if (m <= 0) return ray_error("domain", "?: deal max must be positive");
                return q_deal_indices(n, m);
            }
            if (y && (ray_is_vec(y) || y->type == RAY_LIST)) return q_deal_pick(n, y);
            return ray_error("nyi", "?: deal of this operand is deferred");
        }
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

/* ===== q calendar home (see q_registry.h) ==================================
 * Hinnant days_from_civil (public domain, http://howardhinnant.github.io/
 * date_algorithms.html), rebased to the kdb/base date epoch: the algorithm
 * yields days since 1970-01-01, and 2000.01.01 is unix day 10957 (the same
 * constant base temporal.c uses in the inverse direction). */
int64_t q_days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                    /* [0,399] */
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   /* [0,365] */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* [0,146096] */
    return era * 146097 + doe - 719468 - 10957;  /* unix days -> 2000.01.01 epoch */
}

/* kdb literal/value domain: 0001.01.01 .. 9999.12.31 (datatypes.md), real
 * month lengths, proleptic-Gregorian leap rule. */
int q_date_valid(int64_t y, int64_t m, int64_t d) {
    static const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1) return 0;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    return d <= md[m - 1] + ((m == 2 && leap) ? 1 : 0);
}

/* Timestamp payload composition (see q_registry.h for the contract and the
 * boundary rationale — codex plan-review round 1). */
int q_ts_compose_checked(int64_t days, int64_t tod_ns, int64_t* out) {
    __int128 ns = (__int128)days * 86400000000000LL + tod_ns;
    if (ns > INT64_MAX || ns < -(__int128)INT64_MAX) return 0;
    *out = (int64_t)ns;
    return 1;
}
int64_t q_ts_compose(int64_t days, int64_t tod_ns) {
    int64_t ns;
    if (q_ts_compose_checked(days, tod_ns, &ns)) return ns;
    return ((__int128)days * 86400000000000LL + tod_ns) < 0 ? -INT64_MAX
                                                            : INT64_MAX;
}

/* ===== q cast home (see q_registry.h for the reuse contract) ===============
 * Designator resolution is separate from conversion so C callers (future
 * bool-widening / promotion work) can invoke q_cast_to(tag, x) directly. */

int8_t q_cast_designator(ray_t* t, int* is_tok) {
    *is_tok = 0;
    if (!t) return 0;
    if (t->type == -RAY_I16) {          /* kdb type number == rayfall tag */
        if (RAY_ATOM_IS_NULL(t)) return 0;
        int16_t n = t->i16;
        if (n <= 0) { *is_tok = 1; n = (int16_t)-n; }
        switch (n) {
        case RAY_BOOL: case RAY_U8:  case RAY_I16: case RAY_I32:
        case RAY_I64:  case RAY_F32: case RAY_F64: case RAY_SYM:
        case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
            return (int8_t)n;
        default: return 0;    /* guid/char + month/minute/second etc: deferred */
        }
    }
    if (t->type == -RAY_STR && ray_str_len(t) == 1) {
        char c = ray_str_ptr(t)[0];
        if (c >= 'A' && c <= 'Z') { *is_tok = 1; c = (char)(c - 'A' + 'a'); }
        switch (c) {
        case 'b': return RAY_BOOL; case 'x': return RAY_U8;
        case 'h': return RAY_I16;  case 'i': return RAY_I32;
        case 'j': return RAY_I64;  case 'e': return RAY_F32;
        case 'f': return RAY_F64;  case 's': return RAY_SYM;
        case 'd': return RAY_DATE; case 'g': return RAY_GUID;
        case 't': return RAY_TIME; case 'p': return RAY_TIMESTAMP;
        default:  return 0;       /* c m z n u v + "*" identity: deferred */
        }
    }
    if (t->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(t->i64);
        if (!s) return 0;
        const char* nm = ray_str_ptr(s);
        size_t l = ray_str_len(s);
        int8_t r = 0;
        if      (l == 0)                              { *is_tok = 1; r = RAY_SYM; }
        else if (l == 4 && !memcmp(nm, "long",    4)) r = RAY_I64;
        else if (l == 5 && !memcmp(nm, "float",   5)) r = RAY_F64;
        else if (l == 3 && !memcmp(nm, "int",     3)) r = RAY_I32;
        else if (l == 5 && !memcmp(nm, "short",   5)) r = RAY_I16;
        else if (l == 7 && !memcmp(nm, "boolean", 7)) r = RAY_BOOL;
        else if (l == 4 && !memcmp(nm, "byte",    4)) r = RAY_U8;
        else if (l == 4 && !memcmp(nm, "real",    4)) r = RAY_F32;
        else if (l == 6 && !memcmp(nm, "symbol",  6)) r = RAY_SYM;
        else if (l == 4 && !memcmp(nm, "date",    4)) r = RAY_DATE;
        else if (l == 4 && !memcmp(nm, "time",    4)) r = RAY_TIME;
        else if (l == 9 && !memcmp(nm, "timestamp", 9)) r = RAY_TIMESTAMP;
        ray_release(s);
        return r;
    }
    return 0;
}

/* tag -> rayfall `as` type-sym spelling (cast delegation targets only) */
static const char* q_tag_rayname(int8_t tag) {
    switch (tag) {
    case RAY_BOOL: return "BOOL"; case RAY_U8:  return "U8";
    case RAY_I16:  return "I16";  case RAY_I32: return "I32";
    case RAY_I64:  return "I64";  case RAY_F64: return "F64";
    case RAY_DATE: return "DATE"; case RAY_TIME: return "TIME";
    case RAY_TIMESTAMP: return "TIMESTAMP";
    default:       return NULL;
    }
}

/* kdb float->integer casts ROUND (rint = IEEE nearest/ties-even: `long$3.7
 * is 4, "j"$2.5 is 2, `int$6.6 is 7 — KX ref pins), where rayfall `as`
 * truncates — integer targets pre-round here; everything else delegates to
 * base ray_cast_fn.  RAY_LIST distributes per element and collapses. */
ray_t* q_cast_to(int8_t tag, ray_t* x) {
    if (x && x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* r = q_cast_to(tag, e[i]);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);   /* append retains */
            ray_release(r);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    int isint = (tag == RAY_I64 || tag == RAY_I32 || tag == RAY_I16);
    if (isint && x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null((int8_t)-tag);
        double r = rint(x->f64);              /* F32 atoms store f64 payload */
        if (tag == RAY_I64) return ray_i64((int64_t)r);
        if (tag == RAY_I32) return ray_i32((int32_t)r);
        return ray_i16((int16_t)r);
    }
    if (isint && x && (x->type == RAY_F64 || x->type == RAY_F32)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(tag, n);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        int is64 = (x->type == RAY_F64);
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(x))[i]
                            : (double)((const float*)ray_data(x))[i];
            int isnull = isnan(v);
            int64_t iv = isnull ? 0 : (int64_t)rint(v);
            if      (tag == RAY_I64) ((int64_t*)ray_data(out))[i] = iv;
            else if (tag == RAY_I32) ((int32_t*)ray_data(out))[i] = (int32_t)iv;
            else                     ((int16_t*)ray_data(out))[i] = (int16_t)iv;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* kdb `"x"$str` maps CHARS to bytes ("x"$"abc" -> 0x616263, ref/cast.md
     * #byte); base's U8 STR arm parses decimal text instead — pre-empt it.
     * One char = char atom -> byte ATOM; else a byte vector of the raw chars
     * (empty string -> empty byte vector). */
    if (tag == RAY_U8 && x && x->type == -RAY_STR) {
        const char* sp = ray_str_ptr(x);
        size_t sl = ray_str_len(x);
        if (sl == 1) return ray_u8((uint8_t)sp[0]);
        return ray_vec_from_raw(RAY_U8, sp, (int64_t)sl);
    }
    /* kdb integer-family casts ROUND floats (`int$6.6 -> 7, ref/cast.md);
     * byte joins that family (derived — byte float-cast is unpinned).  Float
     * null -> 0x00: byte has no null (basics/datatypes.md blank column). */
    if (tag == RAY_U8 && x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_u8(0);
        return ray_u8((uint8_t)(int64_t)rint(x->f64));  /* F32 stores f64 */
    }
    if (tag == RAY_U8 && x && (x->type == RAY_F64 || x->type == RAY_F32)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_U8, n);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        int is64 = (x->type == RAY_F64);
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(x))[i]
                            : (double)((const float*)ray_data(x))[i];
            ((uint8_t*)ray_data(out))[i] = isnan(v) ? 0 : (uint8_t)(int64_t)rint(v);
        }
        return out;
    }
    /* kdb `timestamp$date: days -> ns, SATURATING outside the timestamp year
     * range (`timestamp$1666.09.02 -> -0Wp, datatypes.md:149) — base's arm
     * multiplies unchecked (i64 overflow, UBSan, builtins.c:1616) — and
     * mapping the date sentinels to the i64 sentinels (0Nd -> 0Np,
     * +-0Wd -> +-0Wp, which the saturation clamp yields for free). */
    if (tag == RAY_TIMESTAMP && x && x->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(q_ts_compose((int64_t)x->i32, 0));
    }
    if (tag == RAY_TIMESTAMP && x && x->type == RAY_DATE) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_TIMESTAMP, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        const int32_t* d = (const int32_t*)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            int isnull = (d[i] == INT32_MIN);
            ((int64_t*)ray_data(out))[i] =
                isnull ? 0 : q_ts_compose((int64_t)d[i], 0);
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* kdb `timestamp$time keeps the TIME OF DAY (ms -> ns on day 0, derived:
     * time is ms-of-day, timestamp ns; base's same-width path relabels the
     * raw ms payload as ns — a wrong answer, caught by the designator audit).
     * Sentinels map across (0Nt -> 0Np, +-0Wt -> +-0Wp). */
    if (tag == RAY_TIMESTAMP && x && x->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_TIMESTAMP);
        if (x->i32 == INT32_MAX)  return ray_timestamp(INT64_MAX);
        if (x->i32 == -INT32_MAX) return ray_timestamp(-INT64_MAX);
        return ray_timestamp((int64_t)x->i32 * 1000000LL);
    }
    if (tag == RAY_TIMESTAMP && x && x->type == RAY_TIME) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_TIMESTAMP, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        const int32_t* d = (const int32_t*)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            int isnull = (d[i] == INT32_MIN);
            ((int64_t*)ray_data(out))[i] =
                isnull ? 0
                : (d[i] == INT32_MAX)  ? INT64_MAX
                : (d[i] == -INT32_MAX) ? -INT64_MAX
                : (int64_t)d[i] * 1000000LL;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* float -> timestamp: base truncates the float to a raw ns count, but
     * kdb's unit semantics here (rint-ns vs datetime-style fractional DAYS)
     * is unpinned in the docs corpus — error beats a wrong answer, so the
     * shape is a deferred cell (designator-audit decision, plan 2026-07-07). */
    if (tag == RAY_TIMESTAMP && x &&
        (x->type == -RAY_F64 || x->type == -RAY_F32 ||
         x->type == RAY_F64  || x->type == RAY_F32))
        return ray_error("nyi", "$: float->timestamp cast is deferred");
    if (tag == RAY_SYM) {                 /* `symbol$sym is identity; rest nyi */
        if (x && (x->type == -RAY_SYM || x->type == RAY_SYM)) {
            ray_retain(x);
            return x;
        }
        return ray_error("nyi", "$: cast to symbol is deferred (use `$ / \"S\"$ on strings)");
    }
    const char* nm = q_tag_rayname(tag);
    if (!nm)                              /* F32 cast: no base arm — deferred */
        return ray_error("nyi", "$: unsupported cast designator (deferred)");
    ray_t* ts = ray_sym(ray_sym_intern(nm, strlen(nm)));
    if (!ts || RAY_IS_ERR(ts)) return ts;
    ray_t* r = ray_cast_fn(ts, x);
    ray_release(ts);
    return r;
}

/* "D"$ date-string scan (ref/tok.md date formats).  Supported subset:
 * yyyymmdd (8 digits, the doc's [yy]yymmdd with an unambiguous 4-digit year)
 * and yyyy.mm.dd / yyyy-mm-dd / yyyy/mm/dd (the doc's separator variants;
 * "D"$"2000-12-12" is letter-pinned).  Two-digit years and MMM month names
 * are deferred.  Returns 1 and fills y/m/d on a shape match; civil validity
 * is the caller's q_date_valid check. */
static int q_date_scan(const char* p, size_t len,
                       int64_t* y, int64_t* m, int64_t* d) {
    if (len == 8) {
        for (int i = 0; i < 8; i++)
            if (p[i] < '0' || p[i] > '9') return 0;
        *y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        *m = (p[4]-'0')*10 + (p[5]-'0');
        *d = (p[6]-'0')*10 + (p[7]-'0');
        return 1;
    }
    if (len == 10 && (p[4] == '.' || p[4] == '-' || p[4] == '/') && p[7] == p[4]) {
        for (int i = 0; i < 10; i++) {
            if (i == 4 || i == 7) continue;
            if (p[i] < '0' || p[i] > '9') return 0;
        }
        *y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        *m = (p[5]-'0')*10 + (p[6]-'0');
        *d = (p[8]-'0')*10 + (p[9]-'0');
        return 1;
    }
    return 0;
}

static int q_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a CANONICAL 36-char UUID (8-4-4-4-12, hyphens at 8/13/18/23, hex
 * elsewhere, case-insensitive) into out[16].  Returns 1 on success, 0 on any
 * shape/char mismatch.  kdb "G"$ ALSO accepts IPv4/IPv6 address forms
 * (test/q/cast/tok.qcmd, skiplisted) — DEFERRED here (see PLAN.md); those
 * inputs fail this shape check and Tok returns 0Ng. */
static int q_parse_uuid(const char* p, size_t len, uint8_t out[16]) {
    if (len != 36) return 0;
    int bi = 0;
    for (size_t i = 0; i < 36; ) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (p[i] != '-') return 0;
            i++;
            continue;
        }
        int h = q_hexval(p[i]);
        int l = q_hexval(p[i + 1]);
        if (h < 0 || l < 0) return 0;
        out[bi++] = (uint8_t)((h << 4) | l);
        i += 2;
    }
    return bi == 16;
}

/* "T"$ time-string scan (ref/tok.md).  Two forms, both -> i32 ms of day:
 *   - PACKED digits HHMMSSmmm (doc-pinned): "T"$"123456789" -> 12:34:56.789,
 *     "T"$"123456123987654" -> 12:34:56.123 (>=6 digits: HH MM SS then up to 3
 *     fractional; extra fractional digits ignored).
 *   - COLON HH:MM:SS[.f…] (derived — the natural literal spelling): the `.`
 *     fractional is optional; only its first 3 digits (millis) are used.
 * mm/ss must be < 60, else out-of-domain.  Returns 1 and fills *ms on success,
 * 0 on any shape/range mismatch (caller -> typed null 0Nt). */
static int q_all_digits(const char* p, size_t len) {
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++)
        if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}
static int q_time_scan(const char* p, size_t len, int32_t* ms) {
    int64_t h, mi, s, frac = 0;
    int has_colon = 0;
    for (size_t i = 0; i < len; i++) if (p[i] == ':') { has_colon = 1; break; }
    /* colon form: H[H]:MM:SS[.f…] */
    if (has_colon) {
        size_t i = 0;
        int64_t hv = 0;
        while (i < len && p[i] >= '0' && p[i] <= '9') { hv = hv * 10 + (p[i] - '0'); i++; }
        if (i == 0 || i > 2 || i >= len || p[i] != ':') return 0;
        i++;
        if (i + 2 > len || !q_all_digits(p + i, 2) || i + 2 >= len || p[i + 2] != ':')
            return 0;
        mi = (p[i] - '0') * 10 + (p[i + 1] - '0');
        i += 3;
        if (i + 2 > len || !q_all_digits(p + i, 2)) return 0;
        s = (p[i] - '0') * 10 + (p[i + 1] - '0');
        i += 2;
        if (i < len) {                        /* optional .fractional */
            if (p[i] != '.') return 0;
            i++;
            int64_t scale = 100;
            size_t seen = 0;
            while (i < len && p[i] >= '0' && p[i] <= '9') {
                if (seen < 3) { frac += (p[i] - '0') * scale; scale /= 10; seen++; }
                i++;
            }
            if (i != len) return 0;           /* trailing junk */
        }
        h = hv;
        if (mi >= 60 || s >= 60) return 0;
        *ms = (int32_t)(h * 3600000 + mi * 60000 + s * 1000 + frac);
        return 1;
    }
    /* packed HHMMSSmmm: >=6 digits, first 6 = HHMMSS, next up to 3 = millis */
    if (len >= 6 && q_all_digits(p, len)) {
        h  = (p[0] - '0') * 10 + (p[1] - '0');
        mi = (p[2] - '0') * 10 + (p[3] - '0');
        s  = (p[4] - '0') * 10 + (p[5] - '0');
        int64_t scale = 100;
        for (size_t i = 6; i < len && i < 9; i++) { frac += (p[i] - '0') * scale; scale /= 10; }
        if (mi >= 60 || s >= 60) return 0;
        *ms = (int32_t)(h * 3600000 + mi * 60000 + s * 1000 + frac);
        return 1;
    }
    return 0;
}

/* tod scan for "P"$: HH:MM:SS[.f{1..9}] -> ns of day (colon form only; the
 * packed date form is split off by the caller).  Returns 1/0. */
static int q_tod_scan_ns(const char* p, size_t len, int64_t* ns) {
    if (len < 8 || !q_all_digits(p, 2) || p[2] != ':' ||
        !q_all_digits(p + 3, 2) || p[5] != ':' || !q_all_digits(p + 6, 2))
        return 0;
    int64_t h  = (p[0]-'0')*10 + (p[1]-'0');
    int64_t mi = (p[3]-'0')*10 + (p[4]-'0');
    int64_t s  = (p[6]-'0')*10 + (p[7]-'0');
    if (mi >= 60 || s >= 60) return 0;
    int64_t frac = 0;
    if (len > 8) {
        if (p[8] != '.' || len == 9) return 0;
        size_t fd = len - 9;
        if (fd > 9) return 0;
        for (size_t k = 0; k < fd; k++) {
            if (p[9 + k] < '0' || p[9 + k] > '9') return 0;
            frac = frac * 10 + (p[9 + k] - '0');
        }
        for (size_t k = fd; k < 9; k++) frac *= 10;
    }
    *ns = (h * 3600 + mi * 60 + s) * 1000000000LL + frac;
    return 1;
}

/* "P"$ timestamp-string scan (ref/tok.md Â§Timestamps).  Subset:
 *   - Unix seconds, 9..11 digits [+ . fraction] (doc-pinned:
 *     "P"$"10129708800" -> 2290.12.31D00:00:00.000000000,
 *     "P"$"10129708800.123456789" -> ...D00:00:00.123456789);
 *   - date part (q_date_scan separator forms or packed yyyymmdd) + one of
 *     "DT- " + colon tod (pins: "PZ"$\:"20191122-11:11:11.123");
 *   - date-only -> midnight (derived).
 * MMM months / 2-digit years / timezone forms deferred.  Returns 1 + payload
 * ns on success; 0 -> caller yields 0Np (tok.md out-of-domain contract —
 * CHECKED compose, never the cast path's saturating +-0Wp). */
static int q_ts_scan(const char* p, size_t len, int64_t* out) {
    /* unix-seconds: 9..11 digits, optionally . + 1..9 fraction digits */
    size_t dot = len;
    for (size_t i = 0; i < len; i++) if (p[i] == '.') { dot = i; break; }
    if (dot >= 9 && dot <= 11 && q_all_digits(p, dot) &&
        (dot == len || (len > dot + 1 && len <= dot + 10 &&
                        q_all_digits(p + dot + 1, len - dot - 1)))) {
        int64_t secs = 0;
        for (size_t i = 0; i < dot; i++) secs = secs * 10 + (p[i] - '0');
        secs -= 946684800LL;                  /* unix epoch -> 2000.01.01 */
        int64_t ns;
        if (__builtin_mul_overflow(secs, 1000000000LL, &ns)) return 0;
        int64_t frac = 0;
        size_t fd = (dot == len) ? 0 : len - dot - 1;
        for (size_t k = 0; k < fd; k++) frac = frac * 10 + (p[dot + 1 + k] - '0');
        for (size_t k = fd; k < 9; k++) frac *= 10;
        if (__builtin_add_overflow(ns, frac, &ns)) return 0;
        *out = ns;
        return 1;
    }
    /* date [sep tod] */
    size_t dl = 0;
    if (len >= 10 && (p[4] == '.' || p[4] == '-' || p[4] == '/')) dl = 10;
    else if (len >= 8 && q_all_digits(p, 8)) dl = 8;
    if (dl == 0 || len < dl) return 0;
    int64_t y, mo, d;
    if (!q_date_scan(p, dl, &y, &mo, &d) || !q_date_valid(y, mo, d)) return 0;
    int64_t tod = 0;
    if (len > dl) {
        char sep = p[dl];
        if (!(sep == 'D' || sep == 'T' || sep == '-' || sep == ' ')) return 0;
        if (!q_tod_scan_ns(p + dl + 1, len - dl - 1, &tod)) return 0;
    }
    return q_ts_compose_checked(q_days_from_civil(y, mo, d), tod, out);
}

/* kdb Tok (ref/tok.md): parse a string as a value of the tag type.  Leading/
 * trailing blanks are trimmed; unparseable or out-of-range -> typed null.
 * Implicit recursion stops at STRINGS, not atoms: lists / string vectors
 * distribute. */
ray_t* q_tok_to(int8_t tag, ray_t* x) {
    if (x && (x->type == RAY_LIST || x->type == RAY_STR)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* xi;
            if (x->type == RAY_LIST) {
                xi = ((ray_t**)ray_data(x))[i];
                ray_retain(xi);
            } else {
                size_t sl = 0;
                const char* sp = ray_str_vec_get(x, i, &sl);
                xi = ray_str(sp ? sp : "", sp ? sl : 0);
            }
            ray_t* r = q_tok_to(tag, xi);
            ray_release(xi);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    if (!x || x->type != -RAY_STR)
        return ray_error("type", "$: Tok right operand must be a string");
    const char* p = ray_str_ptr(x);
    size_t len = p ? ray_str_len(x) : 0;
    while (len && *p == ' ') { p++; len--; }            /* trim outer blanks */
    while (len && p[len - 1] == ' ') len--;
    switch (tag) {
    case RAY_SYM:
        return ray_sym(ray_sym_intern(len ? p : "", len));
    case RAY_BOOL:   /* truthy set pinned by ref/tok.md: "txyTXY1" */
        return ray_bool(len == 1 && strchr("1TtXxYy", p[0]) != NULL);
    case RAY_F64: case RAY_F32: {
        double v = 0;
        size_t used = len ? ray_parse_f64(p, len, &v) : 0;
        if (used != len || len == 0) return ray_typed_null((int8_t)-tag);
        return tag == RAY_F64 ? ray_f64(v) : ray_f32((float)v);
    }
    case RAY_I64: case RAY_I32: case RAY_I16: {
        int64_t v = 0;
        size_t used = len ? ray_parse_i64(p, len, &v) : 0;
        if (used != len || len == 0) return ray_typed_null((int8_t)-tag);
        /* Out-of-domain -> typed null (tok.md).  The bounds are ±INT*_MAX,
         * NOT INT*_MIN: the exact minimum IS the null sentinel (0N/0Ni/0Nh)
         * and must never round-trip as an accepted value. */
        if (tag == RAY_I64)
            return (v == INT64_MIN)
                 ? ray_typed_null(-RAY_I64) : ray_i64(v);
        if (tag == RAY_I32)
            return (v > INT32_MAX || v < -INT32_MAX)
                 ? ray_typed_null(-RAY_I32) : ray_i32((int32_t)v);
        return (v > INT16_MAX || v < -INT16_MAX)
             ? ray_typed_null(-RAY_I16) : ray_i16((int16_t)v);
    }
    case RAY_DATE: {
        /* Unparseable / invalid civil date / out-of-domain -> 0Nd, never an
         * error (tok.md pins "D"$"2147483648" -> 0Nd). */
        int64_t y, mo, d;
        if (!q_date_scan(p, len, &y, &mo, &d) || !q_date_valid(y, mo, d))
            return ray_typed_null(-RAY_DATE);
        return ray_date(q_days_from_civil(y, mo, d));
    }
    case RAY_TIME: {
        /* "T"$str -> time (ref/tok.md).  Unparseable / out-of-domain -> 0Nt,
         * never an error (base ray_cast_fn errors on a bad string). */
        int32_t ms;
        if (!q_time_scan(p, len, &ms))
            return ray_typed_null(-RAY_TIME);
        return ray_time(ms);
    }
    case RAY_TIMESTAMP: {
        /* "P"$str -> timestamp (ref/tok.md Â§Timestamps).  Unparseable /
         * out-of-range -> 0Np, never an error (tok contract). */
        int64_t ns;
        if (!q_ts_scan(p, len, &ns))
            return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(ns);
    }
    case RAY_U8: {
        /* "X"$ reads the string as HEX ("X"$"42" -> 0x42, ref/tok.md).
         * Unparseable or > 0xff -> 0x00 (derived): tok.md pins out-of-
         * domain -> typed null, and byte HAS no null (basics/datatypes.md),
         * so its zero value stands in. */
        uint64_t v = 0;
        size_t i = 0;
        for (; i < len; i++) {
            char c = p[i];
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) break;
            v = (v << 4) | (uint64_t)d;
            if (v > 0xff) break;
        }
        if (len == 0 || i != len || v > 0xff) return ray_u8(0);
        return ray_u8((uint8_t)v);
    }
    case RAY_GUID: {
        /* "G"$str -> guid (basics/datatypes.md §Guid).  Tok contract:
         * unparseable / wrong-shape -> typed null 0Ng, never an error (base
         * ray_cast_fn "GUID" ERRORS on bad input, so parse here).  Canonical
         * 36-char UUID only; IPv4/IPv6 forms deferred (see q_parse_uuid). */
        uint8_t bytes[16];
        if (!q_parse_uuid(p, len, bytes)) return ray_typed_null(-RAY_GUID);
        return ray_guid(bytes);
    }
    default:
        return ray_error("nyi", "$: month/timespan/char Tok is deferred");
    }
}

/* q `w$s` PAD (ref/pad.md): a LONG width w left-justifies the string s in a
 * field of |w| spaces (w<0 right-justifies); longer strings truncate to |w|.
 * Atomic through the container types (a LIST of strings pads each; DICT over
 * values; TABLE over columns).  Non-string leaves are a 'type error. */
static ray_t* q_pad(int64_t w, ray_t* x) {
    if (!x) return ray_error("type", "$: pad nil");
    if (x->type == -RAY_STR) {
        int64_t width = w < 0 ? -w : w;
        int right = w < 0;                 /* w<0 -> right-justify */
        const char* p = ray_str_ptr(x);
        int64_t n = (int64_t)ray_str_len(x);
        int64_t copy = n < width ? n : width;
        char stack[256];
        char* b = (width < (int64_t)sizeof stack) ? stack : malloc((size_t)width + 1);
        if (!b) return ray_error("wsfull", "$: out of memory");
        memset(b, ' ', (size_t)width);
        if (right) memcpy(b + (width - copy), p, (size_t)copy);   /* text at right */
        else       memcpy(b, p, (size_t)copy);                    /* text at left  */
        ray_t* r = ray_str(b, (size_t)width);
        if (b != stack) free(b);
        return r;
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* e = ray_at_fn(x, ia);
            ray_release(ia);
            if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
            ray_t* r = q_pad(w, e);
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);
        ray_t* v = ray_dict_vals(x);
        if (!k || !v) return ray_error("type", "$: bad dict");
        ray_t* nv = q_pad(w, v);
        if (!nv || RAY_IS_ERR(nv)) return nv;
        ray_retain(k);
        return ray_dict_new(k, nv);
    }
    if (x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* out = ray_table_new(nc);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(x, c);
            ray_t* ncol = q_pad(w, col);
            if (!ncol || RAY_IS_ERR(ncol)) { ray_release(out); return ncol; }
            out = ray_table_add_col(out, ray_table_col_name(x, c), ncol);
            ray_release(ncol);
            if (!out || RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_STR) {            /* string vector -> pad each element */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            size_t sn; const char* p = ray_str_vec_get(x, i, &sn);
            ray_t* s = ray_str(p ? p : "", p ? sn : 0);
            ray_t* r = q_pad(w, s);
            ray_release(s);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return ray_error("type", "$: pad expects a string");
}

/* q `t$x` — Cast/Tok dispatcher.  Multi-designator LHS ("fiij", `int`float,
 * 5 6h, (`int;"i";6h)) zips elementwise over x (ref/cast.md pins
 * (`int;"i";6h)$10 -> 10 10 10i: an ATOM rhs is broadcast).  Single
 * designators resolve via q_cast_designator and dispatch to the shared
 * q_cast_to / q_tok_to (the ONE conversion home — see q_registry.h).  A LONG
 * width LHS (`9$"foo"`) is PAD, not a cast — intercepted before designators. */
static ray_t* q_cast_wrap(ray_t* t, ray_t* x) {
    if (t && t->type == -RAY_I64) return q_pad(t->i64, x);
    int multi = t && ((t->type == -RAY_STR && ray_str_len(t) > 1) ||
                      t->type == RAY_SYM || t->type == RAY_I16 ||
                      t->type == RAY_LIST);
    if (multi) {
        int64_t n = (t->type == -RAY_STR) ? (int64_t)ray_str_len(t) : ray_len(t);
        int x_is_list = x && (ray_is_vec(x) || x->type == RAY_LIST);
        if (x_is_list && ray_len(x) != n)
            return ray_error("length", "$: designator/operand length mismatch");
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ti;
            if (t->type == -RAY_STR) ti = ray_str(ray_str_ptr(t) + i, 1);
            else {
                ray_t* idx = ray_i64(i);
                ti = ray_at_fn(t, idx);         /* sym/short vec, list */
                ray_release(idx);
            }
            if (!ti || RAY_IS_ERR(ti)) { ray_release(out); return ti; }
            ray_t* xi;
            if (x_is_list) {
                ray_t* idx = ray_i64(i);
                xi = ray_at_fn(x, idx);
                ray_release(idx);
            } else { xi = x; ray_retain(xi); }  /* atom rhs broadcasts */
            if (!xi || RAY_IS_ERR(xi)) { ray_release(ti); ray_release(out); return xi; }
            ray_t* r = q_cast_wrap(ti, xi);
            ray_release(ti);
            ray_release(xi);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    int is_tok = 0;
    int8_t tag = q_cast_designator(t, &is_tok);
    if (!tag)
        return ray_error("nyi", "$: unsupported cast designator (deferred)");
    return is_tok ? q_tok_to(tag, x) : q_cast_to(tag, x);
}

/* ===== Amend / Apply / Trap — @ and . ternary+ forms (wave-3) ==============
 * ref/amend.md + ref/apply.md.  q_at_wrap / q_dot_wrap are VARY wrappers now;
 * the rayfall VARY dispatch (eval.c) evaluates every bracket arg and
 * short-circuits an arg-eval error BEFORE calling us — exactly kdb Trap's
 * "errors in fx are not caught" rule.  Dispatch on arg count and first-arg
 * kind: a callable first arg -> Trap; a data first arg -> Amend; n==2 keeps
 * the historic Apply/Index path.  Amend is copy-on-write: q_explode to a
 * fresh rc==1 boxed list, run a sequential single-path engine (repeated
 * indices and cross-sections both decompose into it), collapse back. */

/* fwd decls (mutual recursion / define-before-use) */
static ray_t* q_amend_at(ray_t* v, ray_t* idx, ray_t* f, ray_t* y);
static ray_t* q_amend_dot(ray_t* d, ray_t* idx, ray_t* f, ray_t* y);
static ray_t* q_at_apply2(ray_t* f, ray_t* x);
static ray_t* q_dot_apply(ray_t* f, ray_t* a);
static int    q_is_fn_value(ray_t* x);              /* defined below */
static ray_t* q_elem_at(ray_t* v, int64_t i);       /* defined below */
static ray_t* q_call_n(ray_t* f, ray_t** a, int64_t k);  /* defined below */
static int    q_values_match(ray_t* a, ray_t* b);   /* defined below */

/* The `:` (assign / replace) function slot of Amend.  Arrives either as a
 * symbol atom spelled ":" (quoted verb-sym) or, defensively, a registry value
 * whose provenance spelling is ":". */
static int q_is_assign(ray_t* f) {
    if (!f) return 0;
    if (f->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(f->i64);
        int yes = s && ray_str_len(s) == 1 && ray_str_ptr(s)[0] == ':';
        if (s) ray_release(s);
        return yes;
    }
    q_provenance_t pv;
    return q_registry_provenance(f, &pv) && pv.spelling
        && pv.spelling[0] == ':' && pv.spelling[1] == '\0';
}

/* An index element must be a non-negative integer atom.  Returns 1 + writes
 * *out on success; 0 on a non-integer (symbol/float/list/null/error). */
static int q_idx_int(ray_t* e, int64_t* out) {
    if (!e || !ray_is_atom(e)) return 0;
    switch (e->type) {
    case -RAY_BOOL: *out = e->b8;  return 1;
    case -RAY_I16:  *out = e->i16; return 1;
    case -RAY_I32:  *out = e->i32; return 1;
    case -RAY_I64:  *out = e->i64; return 1;
    default: return 0;
    }
}

/* Copy v's items into a fresh rc==1 boxed list (amend in place with
 * ray_list_set, collapse back afterwards).  Borrowed v; owned result. */
static ray_t* q_explode(ray_t* v) {
    int64_t n = ray_len(v);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (!out || RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = q_elem_at(v, i);                 /* owned v[i] */
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
        out = ray_list_append(out, e);              /* RETAINS */
        ray_release(e);
    }
    return out;
}

/* u each v, collapsed to a typed vector (kdb whole-value @[d;::;u] == u'[d]). */
static ray_t* q_each_over(ray_t* f, ray_t* v) {
    ray_t* mapargs[2] = { f, v };
    ray_t* r = ray_map_fn(mapargs, 2);              /* base map == each */
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* c = q_collapse_list(r);
    ray_release(r);
    return c;
}

/* One amend update at an already-selected leaf `cur` (borrowed):
 *   unary   f(cur)          (y == NULL)
 *   binary  f(cur, ry)      (y != NULL)
 *   assign  ry              (f is `:`)
 * ry is y whole (yi < 0) or y[yi] when y is a per-step vector.  Owned leaf. */
static ray_t* q_amend_step(ray_t* f, ray_t* cur, ray_t* y, int64_t yi) {
    if (q_is_assign(f) && !y)
        return ray_error("type", "@: assign (:) needs a replacement value");
    ray_t* ry = NULL;                                /* borrowed-ish (owned) */
    if (y) {
        if (yi >= 0 && (ray_is_vec(y) || y->type == RAY_LIST)) {
            ry = q_elem_at(y, yi);                    /* owned */
            if (!ry || RAY_IS_ERR(ry)) return ry;
        } else { ry = y; ray_retain(ry); }           /* whole y */
    }
    ray_t* r;
    if (q_is_assign(f)) { r = ry; ray_retain(r); }   /* replace: new = ry */
    else if (y)         r = call_fn2(f, cur, ry);    /* binary f(cur, ry) */
    else                r = call_fn1(f, cur);        /* unary  f(cur)     */
    if (ry) ray_release(ry);
    return r;
}

/* @[d;key(s);f;y] — dict amend.  For each key: if present, amend the value at
 * its position; if ABSENT, extend the dict with that key (kdb inserts on a
 * missing-key amend, e.g. @[`a`b!1 2;`c;:;3] -> a|1 b|2 c|3).  Works on
 * exploded keys/vals boxed lists, collapses back, rebuilds. */
static ray_t* q_amend_dict(ray_t* d, ray_t* key, ray_t* f, ray_t* y) {
    ray_t* keys0 = ray_dict_keys(d);                /* borrowed */
    ray_t* vals0 = ray_dict_vals(d);                /* borrowed */
    if (!keys0 || !vals0) return ray_error("type", "@: malformed dictionary");
    ray_t* keys = q_explode(keys0);
    if (!keys || RAY_IS_ERR(keys)) return keys;
    ray_t* vals = q_explode(vals0);
    if (!vals || RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    int key_atom = ray_is_atom(key);
    int64_t steps = key_atom ? 1 : ray_len(key);
    if (!key_atom && y && (ray_is_vec(y) || y->type == RAY_LIST) && ray_len(y) != steps) {
        ray_release(keys); ray_release(vals); return ray_error("length", NULL);
    }
    for (int64_t k = 0; k < steps; k++) {
        ray_t* kk = key_atom ? (ray_retain(key), key) : q_elem_at(key, k);  /* owned */
        if (!kk || RAY_IS_ERR(kk)) { ray_release(keys); ray_release(vals); return kk; }
        /* locate kk in the working keys (linear; dicts are small) */
        int64_t pos = -1, kn = ray_len(keys);
        for (int64_t j = 0; j < kn; j++) {
            ray_t* kj = ray_list_get(keys, j);       /* borrowed */
            if (q_values_match(kj, kk)) { pos = j; break; }
        }
        int64_t yi = key_atom ? -1 : k;
        if (pos >= 0) {
            ray_t* cur = ray_list_get(vals, pos);    /* borrowed */
            ray_t* nv = q_amend_step(f, cur, y, yi);
            if (!nv || RAY_IS_ERR(nv)) { ray_release(kk); ray_release(keys); ray_release(vals); return nv; }
            vals = ray_list_set(vals, pos, nv);
            ray_release(nv);
        } else {                                     /* insert: apply to generic null */
            ray_t* nv = q_amend_step(f, RAY_NULL_OBJ, y, yi);
            if (!nv || RAY_IS_ERR(nv)) { ray_release(kk); ray_release(keys); ray_release(vals); return nv; }
            keys = ray_list_append(keys, kk);        /* RETAINS kk */
            vals = ray_list_append(vals, nv);
            ray_release(nv);
        }
        ray_release(kk);
    }
    ray_t* ck = q_collapse_list(keys);  ray_release(keys);
    ray_t* cv = q_collapse_list(vals);  ray_release(vals);
    if (!ck || RAY_IS_ERR(ck)) { if (cv) ray_release(cv); return ck; }
    if (!cv || RAY_IS_ERR(cv)) { ray_release(ck); return cv; }
    return ray_dict_new(ck, cv);                     /* consumes ck + cv */
}

/* Amend At — @[v;i;f] / @[v;i;f;y] / @[v;::;f...] (whole) / dict amend.
 * v borrowed; returns an owned copy-on-write value or owned error. */
static ray_t* q_amend_at(ray_t* v, ray_t* idx, ray_t* f, ray_t* y) {
    if (RAY_IS_NULL(idx)) {                          /* whole value */
        if (q_is_assign(f)) {
            if (!y) return ray_error("type", "@: assign (:) needs a value");
            ray_retain(y); return y;
        }
        if (y) return ray_error("nyi", "@[d;::;v;vy] deferred");  /* v'[d;vy] */
        return q_each_over(f, v);                     /* u'[d] */
    }
    if (v->type == RAY_DICT) return q_amend_dict(v, idx, f, y);
    if (!ray_is_vec(v) && v->type != RAY_LIST)
        return ray_error("type", "@: cannot amend a %s", ray_type_name(v->type));
    int64_t n = ray_len(v);
    int idx_atom = ray_is_atom(idx);
    int64_t steps = idx_atom ? 1 : ray_len(idx);
    /* vector index + vector y must conform (kdb 'length) */
    if (!idx_atom && y && (ray_is_vec(y) || y->type == RAY_LIST) && ray_len(y) != steps)
        return ray_error("length", NULL);
    ray_t* work = q_explode(v);
    if (!work || RAY_IS_ERR(work)) return work;
    for (int64_t k = 0; k < steps; k++) {
        int64_t pos;
        if (idx_atom) { if (!q_idx_int(idx, &pos)) { ray_release(work); return ray_error("index", NULL); } }
        else { ray_t* p = q_elem_at(idx, k); int ok = q_idx_int(p, &pos); ray_release(p);
               if (!ok) { ray_release(work); return ray_error("index", NULL); } }
        if (pos < 0 || pos >= n) { ray_release(work); return ray_error("index", NULL); }
        ray_t* cur = ray_list_get(work, pos);        /* borrowed slot */
        ray_t* nv  = q_amend_step(f, cur, y, idx_atom ? -1 : k);
        if (!nv || RAY_IS_ERR(nv)) { ray_release(work); return nv; }
        work = ray_list_set(work, pos, nv);          /* retains nv */
        ray_release(nv);
    }
    ray_t* c = q_collapse_list(work);
    ray_release(work);
    return c;
}

/* Deep single-path amend: descend path[0..plen) into d (copy-on-write), apply
 * the update at the leaf.  d borrowed; owned result. */
static ray_t* q_amend_path(ray_t* d, const int64_t* path, int64_t plen,
                           ray_t* f, ray_t* y, int64_t yi) {
    if (plen == 0) return q_amend_step(f, d, y, yi);
    int64_t pos = path[0];
    if (!(ray_is_vec(d) || d->type == RAY_LIST) || pos < 0 || pos >= ray_len(d))
        return ray_error("index", NULL);
    ray_t* work = q_explode(d);
    if (!work || RAY_IS_ERR(work)) return work;
    ray_t* child = ray_list_get(work, pos);          /* borrowed */
    ray_t* nc = q_amend_path(child, path + 1, plen - 1, f, y, yi);
    if (!nc || RAY_IS_ERR(nc)) { ray_release(work); return nc; }
    work = ray_list_set(work, pos, nc);
    ray_release(nc);
    ray_t* c = q_collapse_list(work);
    ray_release(work);
    return c;
}

/* Cross-sectional deep amend: idx items are vectors; amend every path in the
 * cartesian product idx[0] x idx[1] x ... sequentially (repeats accumulate).
 * Core rows use scalar/unary updates (y broadcast whole). */
static ray_t* q_amend_cross(ray_t* d, ray_t* idx, ray_t* f, ray_t* y) {
    int64_t rank = ray_len(idx);
    if (rank < 1 || rank > 8) return ray_error("rank", ".: cross-section 1..8 deep");
    ray_t* dims[8]; int64_t dn[8];
    for (int64_t a = 0; a < rank; a++) {
        ray_t* col = q_elem_at(idx, a);              /* owned; vector or atom */
        if (!col || RAY_IS_ERR(col)) { for (int64_t b=0;b<a;b++) ray_release(dims[b]); return col; }
        dims[a] = col; dn[a] = ray_is_atom(col) ? 1 : ray_len(col);
    }
    int64_t total = 1; for (int64_t a = 0; a < rank; a++) total *= dn[a];
    ray_t* acc = d; ray_retain(acc);
    for (int64_t t = 0; t < total; t++) {
        int64_t path[8], rem = t; int bad = 0;
        for (int64_t a = rank - 1; a >= 0; a--) {
            int64_t ix = dn[a] ? rem % dn[a] : 0; if (dn[a]) rem /= dn[a];
            if (ray_is_atom(dims[a])) { if (!q_idx_int(dims[a], &path[a])) bad = 1; }
            else { ray_t* p = q_elem_at(dims[a], ix); if (!q_idx_int(p, &path[a])) bad = 1; ray_release(p); }
        }
        ray_t* na = bad ? ray_error("index", NULL)
                        : q_amend_path(acc, path, rank, f, y, -1);
        ray_release(acc);
        if (!na || RAY_IS_ERR(na)) { for (int64_t a=0;a<rank;a++) ray_release(dims[a]); return na; }
        acc = na;
    }
    for (int64_t a = 0; a < rank; a++) ray_release(dims[a]);
    return acc;
}

/* Amend (deep) — .[d;i;f] / .[d;i;v;vy] / .[d;();f...] (whole). */
static ray_t* q_amend_dot(ray_t* d, ray_t* idx, ray_t* f, ray_t* y) {
    if (RAY_IS_NULL(idx)) {                          /* .[d;();u] == u[d] */
        if (q_is_assign(f)) {
            if (!y) return ray_error("type", ".: assign (:) needs a value");
            ray_retain(y); return y;
        }
        if (y) return call_fn2(f, d, y);
        return call_fn1(f, d);
    }
    int64_t ilen = ray_is_atom(idx) ? 1 : ray_len(idx);
    int all_atom = 1;
    if (!ray_is_atom(idx) && idx->type == RAY_LIST) {
        ray_t** e = (ray_t**)ray_data(idx);
        for (int64_t k = 0; k < ilen; k++)
            if (e[k] && !ray_is_atom(e[k])) { all_atom = 0; break; }
    }
    if (all_atom) {
        if (ilen > 8) return ray_error("rank", ".: index path too deep");
        int64_t path[8];
        for (int64_t k = 0; k < ilen; k++) {
            int ok;
            if (ray_is_atom(idx)) ok = q_idx_int(idx, &path[k]);
            else { ray_t* p = q_elem_at(idx, k); ok = q_idx_int(p, &path[k]); ray_release(p); }
            if (!ok) return ray_error("index", NULL);
        }
        return q_amend_path(d, path, ilen, f, y, -1);
    }
    return q_amend_cross(d, idx, f, y);              /* items are vectors */
}

/* Trap tail: r is g's (owned) result; on error run/return the handler e. */
static ray_t* q_trap_finish(ray_t* r, ray_t* e) {
    if (!r || !RAY_IS_ERR(r)) return r;              /* success passes through */
    ray_t* text = q_registry_sig_take();             /* full signal text, owned */
    if (!text) { const char* c = ray_err_code(r); text = ray_str(c ? c : "", c ? strlen(c) : 0); }
    ray_error_free(r);
    if (!q_is_fn_value(e)) { ray_release(text); ray_retain(e); return e; }
    ray_t* hr = call_fn1(e, text);
    ray_release(text);
    return hr;
}

/* Trap At — @[f;fx;e] == .[f;enlist fx;e]. */
static ray_t* q_trap(ray_t* f, ray_t* x, ray_t* e) {
    q_registry_sig_clear();                          /* drop stale payload */
    ray_t* args[1] = { x };
    ray_t* r = q_call_n(f, args, 1);
    return q_trap_finish(r, e);
}

/* Trap — .[g;gx;e]: gx is the argument LIST; spread-apply g over it. */
static ray_t* q_trap_dot(ray_t* g, ray_t* gx, ray_t* e) {
    q_registry_sig_clear();
    if (!gx || (!ray_is_vec(gx) && gx->type != RAY_LIST))
        return ray_error("type", ".: trap args must be a list");
    int64_t k = ray_len(gx);
    if (k < 1 || k > 8) return ray_error("rank", ".: 1..8 trap args");
    ray_t* a[8];
    for (int64_t i = 0; i < k; i++) a[i] = q_elem_at(gx, i);   /* owned */
    ray_t* r = q_call_n(g, a, k);
    for (int64_t i = 0; i < k; i++) ray_release(a[i]);
    return q_trap_finish(r, e);
}

/* q `f@x` — Apply At / Index At (ref/apply.md).  A callable f invokes with
 * the single argument; everything else (vector, list, dict, table, 104h
 * carrier) delegates to q_apply_noun — identical semantics to `f[x]`/`f x`. */
static ray_t* q_at_apply2(ray_t* f, ray_t* x) {
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

/* @ VARY: 2 args Apply/Index; 3 args Trap-At (callable) or Amend-At (data);
 * 4 args Amend-At (quaternary). */
static ray_t* q_at_wrap(ray_t** args, int64_t n) {
    if (n == 2) return q_at_apply2(args[0], args[1]);
    if (n == 3) {
        if (q_is_fn_value(args[0])) return q_trap(args[0], args[1], args[2]);
        return q_amend_at(args[0], args[1], args[2], NULL);
    }
    if (n == 4) return q_amend_at(args[0], args[1], args[2], args[3]);
    return ray_error("rank", "@: got %lld args", (long long)n);
}

/* q `v . vx` — Apply / Index (ref/apply.md): the rhs is the ARGUMENT LIST —
 * a rank-n callable spread-calls over vx's n items; a noun depth-indexes
 * (m . 1 2 is m[1;2]).  Atom rhs is not a list -> 'type (kdb wants a list). */
static ray_t* q_dot_apply(ray_t* f, ray_t* a) {
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

/* . VARY: 2 args Apply/Index; 3 args Trap (callable) or Amend deep (data);
 * 4 args Amend deep (quaternary). */
static ray_t* q_dot_wrap(ray_t** args, int64_t n) {
    if (n == 2) return q_dot_apply(args[0], args[1]);
    if (n == 3) {
        if (q_is_fn_value(args[0])) return q_trap_dot(args[0], args[1], args[2]);
        return q_amend_dot(args[0], args[1], args[2], NULL);
    }
    if (n == 4) return q_amend_dot(args[0], args[1], args[2], args[3]);
    return ray_error("rank", ".: got %lld args", (long long)n);
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

/* ===== q iterators: each-both ' , each-prior ': , over / , scan \ ==========
 * (wave-2 adverb completion — docs/superpowers/plans/2026-07-06-q-adverbs.md).
 * These are internal (spelling-less) HOF VALUES that q_lower embeds at adverb
 * heads, plus the runtime cores the over/scan/prior/peach/deltas/differ
 * keyword wrappers delegate to.  Every function operand is applied through
 * call_fn1/call_fn2, which fall through to q_apply_noun for 100h lambda and
 * 104h projection carriers — so lambdas, native ops and projections all work.
 *
 * Rank of a q value: 1 monadic, 2 dyadic, -1 ambiguous (native vary). */
static int q_fn_rank(ray_t* f) {
    if (!f) return -1;
    switch (f->type) {
    case RAY_UNARY:  return 1;
    case RAY_BINARY: return 2;
    case RAY_VARY:   return -1;
    case RAY_LAMBDA: return (int)ray_len(LAMBDA_PARAMS(f));
    default: break;
    }
    q_deriv_kind k = q_deriv_kind_of(f);
    if (k == Q_DERIV_LAMBDA) return q_deriv_valence(f);
    if (k == Q_DERIV_MONAD)  return 1;
    if (k == Q_DERIV_PROJ) {
        uint64_t m = q_deriv_hole_mask(f);
        int c = 0; while (m) { c += (int)(m & 1u); m >>= 1; }
        return c;              /* effective rank = open holes */
    }
    return -1;
}

/* True iff x is a callable q value (native fn or carrier) — distinguishes the
 * `while` test-function argument of `/` `\` from a numeric do-count. */
static int q_is_fn_value(ray_t* x) {
    if (!x) return 0;
    if (x->type == RAY_UNARY || x->type == RAY_BINARY ||
        x->type == RAY_VARY  || x->type == RAY_LAMBDA) return 1;
    return q_deriv_kind_of(x) != Q_DERIV_NONE;
}

/* Whole-value equivalence (converge stop test) — atom_eq handles atoms,
 * vectors and structural lists. */
static int q_values_match(ray_t* a, ray_t* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    return atom_eq(a, b);
}

/* v[i] as an owned atom/element (borrowed v). */
static ray_t* q_elem_at(ray_t* v, int64_t i) {
    ray_t* ia = ray_i64(i);
    ray_t* e  = ray_at_fn(v, ia);   /* owned */
    ray_release(ia);
    return e;
}

/* Apply f to k args (borrowed).  1/2 route via call_fn1/2 (carrier-aware);
 * k>=3 via the noun dispatcher (lambda/proj carriers) or a native vary. */
static ray_t* q_call_n(ray_t* f, ray_t** a, int64_t k) {
    if (k == 1) return call_fn1(f, a[0]);
    if (k == 2) return call_fn2(f, a[0], a[1]);
    if (f && f->type == RAY_VARY) return ((ray_vary_fn)(uintptr_t)f->i64)(a, k);
    ray_t* r = q_apply_noun(f, a, k);
    if (r) return r;
    return ray_error("rank", "each-both: cannot apply to %lld args", (long long)k);
}

/* ---- each-both  x f'y ------------------------------------------------------ */
static ray_t* q_eachboth_apply(ray_t* f, ray_t** ops, int64_t k);

static int q_op_is_dict(ray_t* v) { return v && v->type == RAY_DICT; }

/* dict each-both (binary): keys come from the dict side; a non-dict operand
 * pairs with the dict's VALUES (kdb: d+'10 20 conforms values, keys kept).
 * Mixed operands previously dispatched ray_dict_vals(non-dict)=NULL straight
 * into a crash (codex round-2 P1). */
static ray_t* q_eachboth_dict(ray_t* f, ray_t* x, ray_t* y) {
    ray_t* kd = q_op_is_dict(x) ? x : y;     /* key donor */
    ray_t* xk = ray_dict_keys(kd);           /* borrowed */
    if (!xk) return ray_error("type", "each-both: malformed dictionary");
    ray_t* ops[2] = { q_op_is_dict(x) ? ray_dict_vals(x) : x,
                      q_op_is_dict(y) ? ray_dict_vals(y) : y };
    ray_t* rv = q_eachboth_apply(f, ops, 2);
    if (!rv || RAY_IS_ERR(rv)) return rv;
    ray_retain(xk);
    return ray_dict_new(xk, rv);             /* consumes keys + vals */
}
static int q_op_is_atom(ray_t* v) { return v && ray_is_atom(v) && !q_op_is_dict(v); }

static ray_t* q_eachboth_apply(ray_t* f, ray_t** ops, int64_t k) {
    int any_dict = 0, all_atom = 1;
    for (int64_t j = 0; j < k; j++) {
        if (q_op_is_dict(ops[j])) any_dict = 1;
        if (!q_op_is_atom(ops[j])) all_atom = 0;
    }
    if (any_dict && k == 2) return q_eachboth_dict(f, ops[0], ops[1]);
    if (all_atom) return q_call_n(f, ops, k);      /* all atoms -> one result */

    int64_t L = -1;
    for (int64_t j = 0; j < k; j++) {
        if (!q_op_is_atom(ops[j])) {
            int64_t lj = ray_len(ops[j]);
            if (L < 0) L = lj;
            else if (L != lj) return ray_error("length", "each-both: length mismatch");
        }
    }
    if (L < 0) L = 1;
    ray_t* out = ray_list_new(L > 0 ? L : 1);
    for (int64_t i = 0; i < L; i++) {
        ray_t* a[16]; uint32_t owned = 0;
        int64_t kk = k < 16 ? k : 16;
        for (int64_t j = 0; j < kk; j++) {
            if (!q_op_is_atom(ops[j])) { a[j] = q_elem_at(ops[j], i); owned |= (1u << j); }
            else                       { a[j] = ops[j]; }   /* atom broadcast */
        }
        ray_t* r = q_call_n(f, a, kk);
        for (int64_t j = 0; j < kk; j++) if (owned & (1u << j)) ray_release(a[j]);
        if (!r || RAY_IS_ERR(r)) { ray_release(out); return r ? r : ray_error("type", NULL); }
        out = ray_list_append(out, r);
        ray_release(r);
    }
    ray_t* c = q_collapse_list(out);
    ray_release(out);
    return c;
}

/* internal each-both value: args[0]=f, args[1..] operands. */
static ray_t* q_eachboth_wrap(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("rank", "each-both: needs a function and operand");
    return q_eachboth_apply(args[0], args + 1, n - 1);
}

/* ---- each-prior  (f':)x  /  s f':x ---------------------------------------- */
/* Seed for the UNARY form: operator identity if known to q, else `first 0#x`
 * (typed null of the argument's element type) — ref/maps.md 259-279. */
static ray_t* q_prior_seed(ray_t* f, ray_t* x) {
    q_provenance_t pv;
    if (q_registry_provenance(f, &pv) && pv.spelling && pv.spelling[0] &&
        pv.spelling[1] == '\0') {
        char g = pv.spelling[0];
        if (g == '+' || g == '-') return ray_i64(0);       /* I(+) = I(-) = 0 */
        if (g == '*' || g == '%') return ray_i64(1);       /* I(*) = I(%) = 1 */
        if (g == ',')             return ray_list_new(0);  /* I(,) = ()       */
    }
    if (ray_is_vec(x))    return ray_typed_null((int8_t)(-x->type));
    if (x && ray_is_atom(x) && x->type != RAY_LIST)
        return ray_typed_null(x->type);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* result[0]=f(x0,seed); result[i]=f(xi,x[i-1]).  Borrows f/seed/x. */
static ray_t* q_prior_over_vec(ray_t* f, ray_t* seed, ray_t* x) {
    if (!x || (!ray_is_vec(x) && x->type != RAY_LIST)) {
        if (x && ray_is_atom(x)) return call_fn2(f, x, seed);
        return ray_error("type", "each-prior: expected a list");
    }
    int64_t L = ray_len(x);
    ray_t* out = ray_list_new(L > 0 ? L : 1);
    ray_t* prev = seed; int prev_owned = 0;
    for (int64_t i = 0; i < L; i++) {
        ray_t* cur = q_elem_at(x, i);            /* owned */
        ray_t* r   = call_fn2(f, cur, prev);
        if (prev_owned) ray_release(prev);
        if (!r || RAY_IS_ERR(r)) { ray_release(cur); ray_release(out); return r ? r : ray_error("type", NULL); }
        out = ray_list_append(out, r);
        ray_release(r);
        prev = cur; prev_owned = 1;               /* current becomes next prior */
    }
    if (prev_owned) ray_release(prev);
    ray_t* c = q_collapse_list(out);
    ray_release(out);
    return c;
}

/* internal each-prior value: n==2 (f':)x unary; n==3 s f':x seeded. */
static ray_t* q_prior_wrap(ray_t** args, int64_t n) {
    if (n < 2 || n > 3) return ray_error("rank", "each-prior: bad arity");
    ray_t* f = args[0];
    ray_t* x = args[n - 1];
    ray_t* seed; int seed_owned = 0;
    if (n == 3) seed = args[1];
    else { seed = q_prior_seed(f, x); seed_owned = 1; }
    ray_t* r;
    if (x && x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);
        ray_t* rv = q_prior_over_vec(f, seed, ray_dict_vals(x));
        if (!rv || RAY_IS_ERR(rv)) r = rv;
        else { ray_retain(k); r = ray_dict_new(k, rv); }
    } else {
        r = q_prior_over_vec(f, seed, x);
    }
    if (seed_owned) ray_release(seed);
    return r;
}

/* deltas x == (-':)x ; differ x == not (~':)x  (ref/maps.md prior section). */
static ray_t* q_deltas_wrap(ray_t* x) {
    ray_t* sub = q_registry_lookup_name("-", 1, Q_DYADIC);   /* borrowed */
    if (!sub) return ray_error("type", "deltas: no subtract");
    ray_t* a[2] = { sub, x };
    return q_prior_wrap(a, 2);
}
static ray_t* q_differ_wrap(ray_t* x) {
    ray_t* mt = q_registry_lookup_name("~", 1, Q_DYADIC);    /* borrowed */
    if (!mt) return ray_error("type", "differ: no match");
    ray_t* a[2] = { mt, x };
    ray_t* eq = q_prior_wrap(a, 2);                          /* (~':)x */
    if (!eq || RAY_IS_ERR(eq)) return eq;
    ray_t* notv = ray_env_get(ray_sym_intern_runtime("not", 3));  /* borrowed */
    if (!notv) return eq;
    ray_t* r = call_fn1(notv, eq);
    ray_release(eq);
    return r;
}

/* ---- over / scan  (converge, do, while, and reduce) ----------------------- */
static int q_truthy(ray_t* v) {
    if (!v) return 0;
    if (v->type == -RAY_BOOL) return v->b8 != 0;
    if (ray_is_atom(v) && is_numeric(v)) return as_f64(v) != 0.0;
    return 0;
}

/* Converge: apply f until the result matches the previous OR the initial x.
 * collect=1 keeps every step (scan), else returns the last (over). */
static ray_t* q_converge(ray_t* f, ray_t* x, int collect) {
    ray_t* first = x; ray_retain(first);
    ray_t* cur   = x; ray_retain(cur);
    ray_t* acc   = collect ? ray_list_new(0) : NULL;
    if (collect) acc = ray_list_append(acc, cur);
    int64_t guard = 0;
    for (;;) {
        ray_t* nxt = call_fn1(f, cur);
        if (!nxt || RAY_IS_ERR(nxt)) {
            ray_release(first); ray_release(cur); if (acc) ray_release(acc);
            return nxt ? nxt : ray_error("type", NULL);
        }
        if (q_values_match(nxt, cur) || q_values_match(nxt, first)) { ray_release(nxt); break; }
        if (collect) acc = ray_list_append(acc, nxt);
        ray_release(cur);
        cur = nxt;
        if (++guard > 100000000) {
            ray_release(first); ray_release(cur); if (acc) ray_release(acc);
            return ray_error("limit", "converge: no fixed point");
        }
    }
    ray_release(first);
    if (collect) { ray_release(cur); ray_t* c = q_collapse_list(acc); ray_release(acc); return c; }
    return cur;
}

/* Do: apply f exactly cnt times to x (n f/x).  collect keeps each step. */
static ray_t* q_ntimes(ray_t* f, int64_t cnt, ray_t* x, int collect) {
    if (cnt < 0) cnt = 0;
    ray_t* cur = x; ray_retain(cur);
    ray_t* acc = NULL;
    if (collect) { acc = ray_list_new(cnt + 1); acc = ray_list_append(acc, cur); }
    for (int64_t i = 0; i < cnt; i++) {
        ray_t* nxt = call_fn1(f, cur);
        ray_release(cur);
        if (!nxt || RAY_IS_ERR(nxt)) { if (acc) ray_release(acc); return nxt ? nxt : ray_error("type", NULL); }
        cur = nxt;
        if (collect) acc = ray_list_append(acc, cur);
    }
    if (collect) { ray_release(cur); ray_t* c = q_collapse_list(acc); ray_release(acc); return c; }
    return cur;
}

/* While: apply f while test(cur) holds (test f/x).  collect keeps each step. */
static ray_t* q_while(ray_t* f, ray_t* test, ray_t* x, int collect) {
    ray_t* cur = x; ray_retain(cur);
    ray_t* acc = NULL;
    if (collect) { acc = ray_list_new(0); acc = ray_list_append(acc, cur); }
    int64_t guard = 0;
    for (;;) {
        ray_t* t = call_fn1(test, cur);
        if (!t || RAY_IS_ERR(t)) { ray_release(cur); if (acc) ray_release(acc); return t ? t : ray_error("type", NULL); }
        int go = q_truthy(t);
        ray_release(t);
        if (!go) break;
        ray_t* nxt = call_fn1(f, cur);
        ray_release(cur);
        if (!nxt || RAY_IS_ERR(nxt)) { if (acc) ray_release(acc); return nxt ? nxt : ray_error("type", NULL); }
        cur = nxt;
        if (collect) acc = ray_list_append(acc, cur);
        if (++guard > 100000000) { ray_release(cur); if (acc) ray_release(acc); return ray_error("limit", "while: no termination"); }
    }
    if (collect) { ray_release(cur); ray_t* c = q_collapse_list(acc); ray_release(acc); return c; }
    return cur;
}

/* Seeded scan  x f\y  (kept minimal — ray_scan_fn has no seed slot). */
static ray_t* q_seeded_scan(ray_t* f, ray_t* seed, ray_t* x) {
    if (!x || (!ray_is_vec(x) && x->type != RAY_LIST)) return ray_error("type", "scan: expected a list");
    int64_t L = ray_len(x);
    ray_t* out = ray_list_new(L > 0 ? L : 1);
    ray_t* acc = seed; ray_retain(acc);
    for (int64_t i = 0; i < L; i++) {
        ray_t* cur = q_elem_at(x, i);
        ray_t* nxt = call_fn2(f, acc, cur);
        ray_release(acc); ray_release(cur);
        if (!nxt || RAY_IS_ERR(nxt)) { ray_release(out); return nxt ? nxt : ray_error("type", NULL); }
        out = ray_list_append(out, nxt);
        acc = nxt; ray_retain(acc);
    }
    ray_release(acc);
    ray_t* c = q_collapse_list(out); ray_release(out); return c;
}

/* `/` over — reduce / converge / do / while by operand shape and f rank. */
static ray_t* q_over_wrap(ray_t** args, int64_t n) {
    ray_t* f = args[0];
    int rank = q_fn_rank(f);
    if (n == 2) {
        ray_t* x = args[1];
        if (rank == 1) return q_converge(f, x, 0);
        ray_t* fa[2] = { f, x };
        return ray_fold_fn(fa, 2);                       /* reduce */
    }
    if (n == 3) {
        ray_t* a = args[1], *x = args[2];
        if (q_is_fn_value(a))  return q_while(f, a, x, 0);
        if (rank == 1)         return q_ntimes(f, as_i64(a), x, 0);
        ray_t* fa[3] = { f, a, x };
        return ray_fold_fn(fa, 3);                       /* seeded reduce */
    }
    return ray_error("rank", "over: bad arity");
}

/* `\` scan — like over but every step is retained. */
static ray_t* q_scan_wrap(ray_t** args, int64_t n) {
    ray_t* f = args[0];
    int rank = q_fn_rank(f);
    if (n == 2) {
        ray_t* x = args[1];
        if (rank == 1) return q_converge(f, x, 1);
        ray_t* fa[2] = { f, x };
        ray_t* r = ray_scan_fn(fa, 2);
        if (!r || RAY_IS_ERR(r)) return r;
        ray_t* c = q_collapse_list(r); ray_release(r); return c;
    }
    if (n == 3) {
        ray_t* a = args[1], *x = args[2];
        if (q_is_fn_value(a))  return q_while(f, a, x, 1);
        if (rank == 1)         return q_ntimes(f, as_i64(a), x, 1);
        return q_seeded_scan(f, a, x);
    }
    return ray_error("rank", "scan: bad arity");
}

/* over/scan/prior keyword dyadic wrappers `f over x` / `f scan x` /
 * `f prior x` — delegate to the n==2 core (peach reuses q_each_wrap). */
static ray_t* q_over_kw(ray_t* f, ray_t* x)  { ray_t* a[2] = { f, x }; return q_over_wrap(a, 2); }
static ray_t* q_scan_kw(ray_t* f, ray_t* x)  { ray_t* a[2] = { f, x }; return q_scan_wrap(a, 2); }
static ray_t* q_prior_kw(ray_t* f, ray_t* x) { ray_t* a[2] = { f, x }; return q_prior_wrap(a, 2); }

/* Build a BINARY derived-verb carrier at EVAL time: `hof` with `f` bound in
 * slot 0 and two data operands open.  Used to lower `(f/:)` / `(f\:)` when the
 * operand f is an expression (a lambda) that must be EVALUATED to a value
 * first — a lower-time q_proj would capture the raw `(q.fn …)` tree.  `x f/: y`
 * then == map-right(f;x;y), which lets a stacked outer adverb (`f/:\:`) drive
 * it through map-left. */
static ray_t* q_mkderiv2(ray_t* hof, ray_t* f) {
    ray_t* args[3] = { f, NULL, NULL };
    return q_proj_new(hof, args, 3, 0x6u, 2);
}

static ray_t* g_scan_value     = NULL;
static ray_t* g_over_value     = NULL;
static ray_t* g_eachboth_value = NULL;
static ray_t* g_prior_value    = NULL;
static ray_t* g_mkderiv2_value = NULL;

ray_t* q_registry_scan_value(void)     { return g_scan_value;     }  /* borrowed */
ray_t* q_registry_over_value(void)     { return g_over_value;     }  /* borrowed */
ray_t* q_registry_eachboth_value(void) { return g_eachboth_value; }  /* borrowed */
ray_t* q_registry_prior_value(void)    { return g_prior_value;    }  /* borrowed */
ray_t* q_registry_mkderiv2_value(void) { return g_mkderiv2_value; }  /* borrowed */

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

static int64_t q_expr_rightmost_name(ray_t* el) {
    if (!el) return -1;
    if (el->type == -RAY_SYM && !(el->attrs & 0x20 /* Q_ATTR_QUOTED */))
        return el->i64;
    if (el->type == RAY_LIST) {
        ray_t** e = (ray_t**)ray_data(el);
        for (int64_t i = ray_len(el) - 1; i >= 0; i--) {
            int64_t id = q_expr_rightmost_name(e[i]);
            if (id >= 0) return id;
        }
    }
    return -1;
}

/* If `el` is a lowered assignment node `(set/let name val)`, return the target
 * sym-id; else if `el` is a bare unquoted name-ref sym, return its id (a bare
 * column reference); else use the expression's rightmost name token when one is
 * available, falling back to generated `x`, `x1`, ... names. */
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
    return q_expr_rightmost_name(el);
}

static int64_t q_ctx_generated_name(int64_t ordinal) {
    if (ordinal == 0) return ray_sym_intern_runtime("x", 1);
    char buf[32];
    int n = snprintf(buf, sizeof buf, "x%lld", (long long)ordinal);
    return ray_sym_intern_runtime(buf, (size_t)n);
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
            int64_t used_stack[64];
            int64_t* used = (n <= 64) ? used_stack : (int64_t*)malloc((size_t)n * sizeof(int64_t));
            if (!used) {
                ray_release(out);
                out = ray_error("wsfull", "([]...): out of memory");
            }
            int64_t gen = 0;
            for (int64_t i = 0; i < n && !RAY_IS_ERR(out); i++) {
                int64_t nm;
                if (names[i] < 0) {
                    /* Anonymous column: openq invents x, x1, … and dedups to a
                     * free name (the user supplied none, so this never errors). */
                    nm = q_ctx_generated_name(gen++);
                    nm = q_name_dedup(nm, used, i, 0);
                } else {
                    /* User-given name: taken VERBATIM — no .Q.id-style sanitize
                     * or reserved-word repair (that is opt-in via .Q.id).  A
                     * duplicate is an error, matching kdb (not silently renamed). */
                    nm = names[i];
                    for (int64_t j = 0; j < i; j++) {
                        if (used[j] == nm) {
                            ray_release(out);
                            out = ray_error("dup", "([]…): duplicate column name");
                            break;
                        }
                    }
                    if (RAY_IS_ERR(out)) break;
                }
                used[i] = nm;
                ray_t* col = vals[i]; int owned = 0;
                if (col && col->type < 0) {              /* scalar -> broadcast */
                    ray_t* nn = ray_i64(nrows);
                    col = ray_take_fn(col, nn); ray_release(nn); owned = 1;
                    if (!col || RAY_IS_ERR(col)) {
                        ray_release(out); out = col ? col : ray_error("type", "broadcast");
                        break;
                    }
                }
                out = ray_table_add_col(out, nm, col);
                if (owned) ray_release(col);
            }
            if (used && used != used_stack) free(used);
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

/* q keyed-table literal `([k1:…;k2:…] v1:…; v2:…)` head.  args[0] is an int
 * atom = the KEY column count; args[1..] are the column-def trees (key columns
 * first, then value columns).  All columns are built as ONE table (so value
 * columns can reference key columns via the shared build scope — q_ctx_build's
 * cross-column binding), then split into a key-columns table and a
 * value-columns table joined into a RAY_DICT: "a keyed table is just a
 * dictionary from one table to another" (q_fmt renders it `k| v`). */
static ray_t* q_keyed_table_build(ray_t** args, int64_t n) {
    if (n < 1 || !args[0] || args[0]->type != -RAY_I64)
        return ray_error("parse", "keyed-table literal: missing key count");
    int64_t nk = args[0]->i64;
    ray_t* tbl = q_ctx_build(args + 1, n - 1, 1);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    if (tbl->type != RAY_TABLE) { ray_release(tbl); return ray_error("type", "keyed-table literal: not a table"); }
    int64_t nc = ray_table_ncols(tbl);
    if (nk < 0 || nk > nc) { ray_release(tbl); return ray_error("length", "keyed-table literal: bad key count"); }
    ray_t* kt = ray_table_new(nk > 0 ? nk : 1);
    ray_t* vt = ray_table_new(nc - nk > 0 ? nc - nk : 1);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(kt) && !RAY_IS_ERR(vt); c++) {
        int64_t nm  = ray_table_col_name(tbl, c);
        ray_t*  col = ray_table_get_col_idx(tbl, c);   /* borrowed; add retains */
        if (c < nk) kt = ray_table_add_col(kt, nm, col);
        else        vt = ray_table_add_col(vt, nm, col);
    }
    ray_release(tbl);
    if (RAY_IS_ERR(kt)) { ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);   /* consumes kt, vt */
}

static void q_select_rename_temps(ray_t* tbl, ray_t* tempnames, ray_t* realnames) {
    if (!tbl || tbl->type != RAY_TABLE ||
        !tempnames || tempnames->type != RAY_SYM ||
        !realnames || realnames->type != RAY_SYM)
        return;
    int64_t nt = ray_len(tempnames);
    int64_t nc = ray_table_ncols(tbl);
    int64_t used_stack[64];
    int64_t* used = (nc <= 64) ? used_stack : (int64_t*)malloc((size_t)nc * sizeof(int64_t));
    if (!used) return;
    for (int64_t c = 0; c < nc; c++) used[c] = ray_table_col_name(tbl, c);
    for (int64_t i = 0; i < nt; i++) {
        ray_t* ti = ray_i64(i);
        ray_t* ta = ray_at_fn(tempnames, ti);
        ray_t* ra = ray_at_fn(realnames, ti);
        ray_release(ti);
        int64_t tid = (ta && ta->type == -RAY_SYM) ? ta->i64 : -1;
        int64_t rid = (ra && ra->type == -RAY_SYM) ? ra->i64 : -1;
        if (ta) ray_release(ta);
        if (ra) ray_release(ra);
        if (tid < 0 || rid < 0) continue;
        for (int64_t c = 0; c < nc; c++) {
            if (ray_table_col_name(tbl, c) != tid) continue;
            int64_t nm = q_name_dedup(rid, used, c, 1);
            used[c] = nm;
            ray_table_set_col_name(tbl, c, nm);
            break;
        }
    }
    if (used != used_stack) free(used);
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

    ray_t* keys = (n >= 2) ? args[1] : NULL;
    int64_t nk = (keys && keys->type == RAY_SYM) ? ray_len(keys) : 0;
    if (nk == 0 || res->type != RAY_TABLE) {
        if (n >= 4) q_select_rename_temps(res, args[2], args[3]);
        return res;
    }

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
    if (n >= 4) q_select_rename_temps(vt, args[2], args[3]);
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

    /* EXEC forms — signalled by the By-phrase being EMPTY: the general empty
     * list `()` OR `::` (both are `funsql_empty`; `()` now parses to the empty
     * list, not `::`), as opposed to `0b` (RAY_BOOL) which is Select's "no
     * grouping".  Result is a vector (a is a column/parse-tree), a dictionary
     * (a is a dict), or the last row as a dictionary (a is `()`).  See
     * funsql.md "No grouping". */
    if (funsql_empty(b)) {
        if (a && (a->type == -RAY_SYM ||
                  (a->type == RAY_LIST && ray_len(a) > 0 &&        /* guard `()` (empty list): no head to read */
                   funsql_is_fn(((ray_t**)ray_data(a))[0])))) {
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

/* Scatter for a BRAND-NEW column (absent from the base table): `upd` values land
 * at the `idx` rows, the unselected rows get the type-correct null (kdb:
 * `update newcol:… where …` nulls the rows the where didn't match).  `N` is the
 * full row count. */
static ray_t* funsql_scatter_new(ray_t* idx, ray_t* upd, int64_t N) {
    int64_t M = ray_len(idx);
    /* ray_typed_null wants the ATOM (negative) type: a vector upd of type T
     * nulls as -T; a scalar upd already carries its negative atom type. */
    int8_t et = ray_is_vec(upd) ? (int8_t)(-upd->type)
                                : (upd->type < 0 ? upd->type : (int8_t)(-RAY_I64));
    int64_t* pos = (int64_t*)malloc((N > 0 ? N : 1) * sizeof(int64_t));
    if (!pos) return ray_error("oom", NULL);
    for (int64_t r = 0; r < N; r++) pos[r] = -1;
    int64_t* ii = (int64_t*)ray_data(idx);
    for (int64_t k = 0; k < M; k++)
        if (ii[k] >= 0 && ii[k] < N) pos[ii[k]] = k;
    ray_t* out = ray_list_new(N > 0 ? N : 1);
    for (int64_t r = 0; r < N; r++) {
        ray_t* v;
        if (pos[r] >= 0) { ray_t* iv = ray_i64(pos[r]); v = ray_at_fn(upd, iv); ray_release(iv); }
        else             { v = ray_typed_null(et); }
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
    /* Append columns that `a` CREATED (absent from the base table): the where
     * path filters to a subtable, so these live only in `uft` — scatter them
     * across the full row count with nulls on the unselected rows (parity with
     * the no-where update path, which adds them directly). */
    int64_t ntbl = ray_table_nrows(tbl);
    for (int64_t i = 0; i < na && !RAY_IS_ERR(out); i++) {
        if (ray_table_get_col(tbl, upd_ids[i])) continue;      /* already emitted */
        ray_t* newvals = ray_table_get_col(uft, upd_ids[i]);
        if (!newvals) continue;
        ray_t* merged = funsql_scatter_new(idx, newvals, ntbl);
        if (!merged || RAY_IS_ERR(merged)) { ray_release(out); ray_release(idx); ray_release(uft); ray_release(tbl); return merged ? merged : ray_error("oom", NULL); }
        out = ray_table_add_col(out, upd_ids[i], merged);
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
static ray_t* g_keyed_table_value = NULL;
static ray_t* g_select_value = NULL;
static ray_t* g_compose_value = NULL;

/* q `'[f;g;…]` compose builder — a normal VARY (args are the resolved function
 * VALUES): boxes them into a Q_DERIV_COMPOSE carrier (q_deriv.c). */
static ray_t* q_compose_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("rank", "': compose needs at least one function");
    return q_compose_new(args, n);
}
static ray_t* g_funsql_select_value = NULL;
static ray_t* g_funsql_bang_value   = NULL;
static ray_t* g_lambda_value        = NULL;

/* q.fn — SPECIAL FORM behind every q lambda literal.  q_lower rewrites the
 * parser's `{`-marker node to (q.fn src params body...); at eval this
 * delegates lambda creation to the base env `fn` form (same params/body
 * calling convention) and wraps the resulting RAY_LAMBDA in the 100h
 * .q.lambda carrier that carries q valence + verbatim source for display.
 * kdb caps lambdas at 8 arguments -> 'params — signalled HERE (not at parse:
 * qdoc error rows only match lower/eval errors). */
static ray_t* q_fn_make(ray_t** args, int64_t n) {
    if (n < 3 || !args[0] || args[0]->type != -RAY_STR ||
        !args[1] || args[1]->type != RAY_SYM)
        return ray_error("type", "q.fn: malformed lambda node");
    int64_t rank = ray_len(args[1]);
    if (rank > 8)
        return ray_error("params", "'params: lambdas take at most 8 arguments");
    ray_t* fnv = ray_env_get(ray_sym_intern("fn", 2));       /* borrowed */
    if (!fnv || fnv->type != RAY_VARY)
        return ray_error("type", "q.fn: base fn form unavailable");
    ray_t* lam = ((ray_vary_fn)(uintptr_t)fnv->i64)(args + 1, n - 1);
    if (!lam || RAY_IS_ERR(lam)) return lam;
    ray_t* c = q_lambda_carrier_new(lam, (int)rank, args[0]);
    ray_release(lam);                       /* carrier holds its own ref */
    return c;
}

ray_t* q_registry_lambda_value(void) { return g_lambda_value; }  /* borrowed */

static ray_t* g_ret_value = NULL;
static ray_t* g_sig_value = NULL;
static _Thread_local ray_t* g_qret_payload = NULL;
static _Thread_local ray_t* g_qsig_payload = NULL;

/* `:x` early return (basics/function-notation.md#explicit-return).  The body
 * must unwind NOW: eval aborts a lambda body on any RAY_ERROR, so we ride
 * the error path with the reserved class "q.ret" and stash the payload in a
 * thread-local for the innermost q_lambda_apply to take. */
static ray_t* q_ret_fn(ray_t* x) {
    if (g_qret_payload) { ray_release(g_qret_payload); g_qret_payload = NULL; }
    if (x) ray_retain(x);
    g_qret_payload = x;
    return ray_error("q.ret", NULL);
}

ray_t* q_lambda_ret_take(void) {
    ray_t* v = g_qret_payload;      /* owned by the caller now */
    g_qret_payload = NULL;
    return v;
}

/* Full text of the most recent `'x` signal.  The ≤7-char error class in
 * err->sdata truncates, but kdb Trap hands the handler the WHOLE message, so
 * q_sig_fn stashes the untruncated text here (mirroring the q.ret payload).
 * Owned by the caller; NULL if the last error was not a q signal. */
ray_t* q_registry_sig_take(void) {
    ray_t* v = g_qsig_payload;
    g_qsig_payload = NULL;
    return v;
}

void q_registry_sig_clear(void) {
    if (g_qsig_payload) { ray_release(g_qsig_payload); g_qsig_payload = NULL; }
}

/* `'x` Signal (ref/signal.md): abort with error class = the sym spelling /
 * string text (ray_error copies, 7-char sdata cap — kdb's own classes are
 * short for the same reason).  The full untruncated text is stashed for Trap
 * (q_registry_sig_take). */
static ray_t* q_sig_fn(ray_t* x) {
    char cls[8] = "signal";
    ray_t* full = NULL;                 /* owned full-text string, or NULL */
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        if (s) {
            size_t l = ray_str_len(s); size_t c = l > 7 ? 7 : l;
            memcpy(cls, ray_str_ptr(s), c); cls[c] = '\0';
            full = ray_str(ray_str_ptr(s), l);
            ray_release(s);
        }
    } else if (x && x->type == -RAY_STR) {
        size_t l = ray_str_len(x); size_t c = l > 7 ? 7 : l;
        memcpy(cls, ray_str_ptr(x), c); cls[c] = '\0';
        full = ray_str(ray_str_ptr(x), l);
    }
    if (g_qsig_payload) ray_release(g_qsig_payload);
    g_qsig_payload = full;              /* owned (or NULL) */
    return ray_error(cls, NULL);
}

ray_t* q_registry_ret_value(void) { return g_ret_value; }   /* borrowed */
ray_t* q_registry_sig_value(void) { return g_sig_value; }   /* borrowed */

/* ===== q imperative control constructs =====================================
 * The `;` statement sequence and the `if` / `do` / `while` control words are
 * SPECIAL FORMS (basics/control.md, ref/{if,do,while}.md): they receive their
 * statement args UNEVALUATED and drive evaluation themselves — lazy, strictly
 * left-to-right, with side effects PERSISTING.  Unlike a lambda body they do
 * NOT open a lexical scope: a `:` assignment inside amends the ENCLOSING frame
 * (q_lower already lowered it to set/let against that frame; "the brackets do
 * not create lexical scope").  `if`/`do`/`while` always return the generic
 * null; `;` returns its LAST statement's value.  This is the q-layer home for
 * the semantics (CLAUDE.md rule 4) — rayfall's own `if`/`do` differ (triadic
 * cond / scope-pushing progn) so they are NOT reused here. */
static ray_t* g_seq_value   = NULL;
static ray_t* g_if_value    = NULL;
static ray_t* g_do_value    = NULL;
static ray_t* g_while_value = NULL;

/* q if/while require the test to evaluate to an atom of INTEGRAL type
 * (ref/if.md, ref/while.md); a float / symbol / vector / generic null is a
 * 'type error, not a silent truthiness (kdb `if[1 0;…]` signals `type). */
static int q_ctl_test_ok(ray_t* t) {
    int8_t ty = t->type;
    return ty == -RAY_BOOL || ty == -RAY_I64 || ty == -RAY_I32 ||
           ty == -RAY_I16 || ty == -RAY_U8;
}

/* Evaluate a test/condition arg to a truthiness, materializing a lazy handle.
 * On error (eval failure OR a non-integral-atom test), stashes the owned
 * RAY_ERROR in *err and returns 0. */
static int q_ctl_truth(ray_t* arg, ray_t** err) {
    *err = NULL;
    ray_t* t = ray_eval(arg);
    if (RAY_IS_ERR(t)) { *err = t; return 0; }
    if (ray_is_lazy(t)) t = ray_lazy_materialize(t);
    if (RAY_IS_ERR(t)) { *err = t; return 0; }
    if (!q_ctl_test_ok(t)) {
        ray_release(t);
        *err = ray_error("type", "control test must be an integral atom");
        return 0;
    }
    int truthy = is_truthy(t);
    ray_release(t);
    return truthy;
}

/* Evaluate args[from..n) in order for their side effects, releasing each
 * result.  Returns an owned RAY_ERROR on the first failure, else NULL. */
static ray_t* q_ctl_run_body(ray_t** args, int64_t from, int64_t n) {
    for (int64_t i = from; i < n; i++) {
        ray_t* r = ray_eval(args[i]);
        if (RAY_IS_ERR(r)) return r;
        ray_release(r);
    }
    return NULL;
}

/* `s1; s2; …; sn` — evaluate each statement left-to-right (side effects
 * persist to the enclosing frame); the value is the LAST statement's. */
static ray_t* q_seq_fn(ray_t** args, int64_t n) {
    ray_t* result = RAY_NULL_OBJ;
    for (int64_t i = 0; i < n; i++) {
        ray_release(result);                 /* RAY_NULL_OBJ release is a no-op */
        result = ray_eval(args[i]);
        if (RAY_IS_ERR(result)) return result;
    }
    return result;
}

/* `if[test; e1; …; en]` — evaluate the body once, in order, unless test is
 * zero; result is always the generic null (ref/if.md). */
static ray_t* q_if_fn(ray_t** args, int64_t n) {
    if (n < 1) return RAY_NULL_OBJ;
    ray_t* err = NULL;
    int truthy = q_ctl_truth(args[0], &err);
    if (err) return err;
    if (truthy) { err = q_ctl_run_body(args, 1, n); if (err) return err; }
    return RAY_NULL_OBJ;
}

/* `do[count; e1; …; en]` — evaluate the body `count` times; result is always
 * the generic null (ref/do.md).  `count` is a non-negative integer atom. */
static ray_t* q_do_fn(ray_t** args, int64_t n) {
    if (n < 1) return RAY_NULL_OBJ;
    ray_t* cnt = ray_eval(args[0]);
    if (RAY_IS_ERR(cnt)) return cnt;
    if (ray_is_lazy(cnt)) cnt = ray_lazy_materialize(cnt);
    if (RAY_IS_ERR(cnt)) return cnt;
    if (RAY_ATOM_IS_NULL(cnt) ||
        (cnt->type != -RAY_I64 && cnt->type != -RAY_I32 &&
         cnt->type != -RAY_I16 && cnt->type != -RAY_U8)) {
        ray_release(cnt);
        return ray_error("type", "do: count must be a non-negative integer atom");
    }
    int64_t times = elem_as_i64(cnt);
    ray_release(cnt);
    if (times < 0) return ray_error("type", "do: count must be non-negative");
    for (int64_t k = 0; k < times; k++) {
        ray_t* err = q_ctl_run_body(args, 1, n);
        if (err) return err;
    }
    return RAY_NULL_OBJ;
}

/* `while[test; e1; …; en]` — re-evaluate test each pass; while non-zero run
 * the body in order; result is always the generic null (ref/while.md). */
static ray_t* q_while_fn(ray_t** args, int64_t n) {
    if (n < 1) return RAY_NULL_OBJ;
    for (;;) {
        ray_t* err = NULL;
        int truthy = q_ctl_truth(args[0], &err);
        if (err) return err;
        if (!truthy) break;
        err = q_ctl_run_body(args, 1, n);
        if (err) return err;
    }
    return RAY_NULL_OBJ;
}

ray_t* q_registry_seq_value(void)   { return g_seq_value; }    /* borrowed */
ray_t* q_registry_if_value(void)    { return g_if_value; }     /* borrowed */
ray_t* q_registry_do_value(void)    { return g_do_value; }     /* borrowed */
ray_t* q_registry_while_value(void) { return g_while_value; }  /* borrowed */

ray_t* q_registry_funsql_select_value(void) { return g_funsql_select_value; }
ray_t* q_registry_funsql_bang_value(void)   { return g_funsql_bang_value; }

ray_t* q_registry_select_value(void) {
    return g_select_value;   /* borrowed; NULL before init */
}

ray_t* q_registry_compose_value(void) {
    return g_compose_value;  /* borrowed; NULL before init */
}

ray_t* q_registry_list_value(void) {
    return g_list_value;   /* borrowed; NULL before init */
}

ray_t* q_registry_keyed_table_value(void) {
    return g_keyed_table_value;  /* borrowed; NULL before init */
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
        case -RAY_GUID: {                                      /* 16-byte payload, not i64 */
            const void* g = e[i]->obj ? ray_data(e[i]->obj) : ray_data(e[i]);
            vec = ray_vec_append(vec, g);
        } break;
        default:        vec = ray_vec_append(vec, &e[i]->i64); break; /* i64 + temporals */
        }
        if (RAY_IS_ERR(vec)) return vec;
        if (RAY_ATOM_IS_NULL(e[i])) { ray_vec_set_null(vec, i, true); nulls++; }
    }
    (void)nulls;
    return vec;
}

/* ---- IPC client verb: q `hopen` (feat/q-ipc-client, Phase D) ----
 * Thin wrapper over `.ipc.open` (ray_hopen_fn), which takes a
 * "host:port[:user:password]" string + optional connect-timeout.  q `hopen`
 * accepts an int PORT (localhost), a "host:port[:user:pass]" STRING (with the
 * kdb "::PORT"/":host:port" leading-colon conventions), or a 2-list
 * (conn; timeout-ms).  The kdb hsym-SYMBOL forms (`::PORT`, `:host:port`) do
 * not lex as a single colon-bearing symbol yet (the scanner stops at ':'), so
 * they are deferred to a lexer change; the int/string surface parses today. */

/* Normalize a connection descriptor (int atom or string atom) into the
 * "host:port[:user:password]" form .ipc.open expects.  Owned RAY_STR or error. */
static ray_t* q_hopen_connstr(ray_t* c) {
    if (q_is_int_atom(c)) {
        int64_t p = q_iatom_val(c);
        if (p <= 0 || p > 65535)
            return ray_error("domain", "hopen: port must be in 1..65535, got %lld",
                             (long long)p);
        char buf[32];
        int m = snprintf(buf, sizeof buf, "127.0.0.1:%lld", (long long)p);
        if (m <= 0 || m >= (int)sizeof buf) return ray_error("domain", "hopen: bad port");
        return ray_str(buf, (size_t)m);
    }
    if (c && c->type == -RAY_STR) {
        const char* s = ray_str_ptr(c);
        size_t n = ray_str_len(c);
        if (n > 512) return ray_error("domain", "hopen: descriptor too long");
        /* kdb leading-colon conventions: "::rest" = localhost, ":host:.." = strip 1 */
        if (n >= 2 && s[0] == ':' && s[1] == ':') {
            char buf[600];
            int m = snprintf(buf, sizeof buf, "127.0.0.1:%.*s", (int)(n - 2), s + 2);
            if (m <= 0 || m >= (int)sizeof buf)
                return ray_error("domain", "hopen: descriptor too long");
            return ray_str(buf, (size_t)m);
        }
        if (n >= 1 && s[0] == ':') return ray_str(s + 1, n - 1);   /* strip one ':' */
        return ray_str(s, n);                                      /* "host:port" */
    }
    return ray_error("type",
                     "hopen: expected an int port or a \"host:port\" string, got %s",
                     ray_type_name(c ? c->type : 0));
}

/* q `hopen y` — connect, return an int handle.  Restricted connections must not
 * open outbound sockets (the `.ipc.open` primitive is RAY_FN_RESTRICTED; calling
 * ray_hopen_fn directly bypasses the eval-layer check, so re-assert it here). */
static ray_t* q_hopen_wrap(ray_t* x) {
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    ray_t* conn      = x;
    ray_t* timeout   = NULL;
    ray_t* pair_conn = NULL;   /* owned when a pair was a typed int VECTOR */
    ray_t* pair_to   = NULL;
    if (x && x->type == RAY_LIST && ray_len(x) == 2) {   /* (conn; timeout-ms) */
        ray_t** e = (ray_t**)ray_data(x);
        conn = e[0]; timeout = e[1];                     /* borrowed */
    } else if (q_is_int_vec(x) && ray_len(x) == 2) {
        /* an all-int (port; timeout-ms) pair collapses to a homogeneous int
         * VECTOR (not a general list) — recover the two atoms.  (A symbol/string
         * conn keeps the pair a RAY_LIST, handled above.) */
        pair_conn = ray_i64(q_ivec_get(x, 0));
        pair_to   = ray_i64(q_ivec_get(x, 1));
        conn = pair_conn; timeout = pair_to;
    }
    ray_t* cs = q_hopen_connstr(conn);                   /* owned or error */
    if (!cs || RAY_IS_ERR(cs)) {
        if (pair_conn) ray_release(pair_conn);
        if (pair_to)   ray_release(pair_to);
        return cs;
    }
    ray_t* args[2] = { cs, NULL };
    int64_t nargs = 1;
    ray_t* tv = NULL;
    if (timeout && !q_is_int_atom(timeout)) {
        ray_release(cs);
        if (pair_conn) ray_release(pair_conn);
        if (pair_to)   ray_release(pair_to);
        return ray_error("type", "hopen: timeout must be an integer (milliseconds)");
    }
    if (timeout) {
        tv = make_i64(q_iatom_val(timeout));
        args[1] = tv; nargs = 2;
    }
    ray_t* h = ray_hopen_fn(args, nargs);                /* owned handle or error */
    ray_release(cs);
    if (tv)        ray_release(tv);
    if (pair_conn) ray_release(pair_conn);
    if (pair_to)   ray_release(pair_to);
    if (!h || RAY_IS_ERR(h)) return h;
    /* q handles are 1-BASED: openq's raw poll selector ids start at 0, but kdb
     * reserves 0 (console) and encodes async as a NEGATIVE handle, so 0 must not
     * be a live handle (`neg 0` == 0 could not select async).  Offset the raw id
     * by +1 here; hclose / handle-apply translate back to the raw id. */
    int64_t raw = (h->type == -RAY_I64) ? h->i64 : (int64_t)h->i32;
    ray_release(h);
    return make_i64(raw + 1);
}

/* q `hclose h` — translate the 1-based q handle back to the raw poll id and
 * route to `.ipc.close` (ray_hclose_fn).  Restricted connections are refused,
 * matching hopen / the handle-apply path. */
static ray_t* q_hclose_wrap(ray_t* x) {
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    if (!q_is_int_atom(x) || RAY_ATOM_IS_NULL(x))
        return ray_error("type", "hclose: expected an int handle, got %s",
                         ray_type_name(x ? x->type : 0));
    int64_t qh = q_iatom_val(x);
    if (qh <= 0)
        return ray_error("type", "hclose: invalid handle %lld", (long long)qh);
    ray_t* raw = make_i64(qh - 1);
    ray_t* r = ray_hclose_fn(raw);
    ray_release(raw);
    return r;
}

/* ===== string search family: like / ss / ssr (feat/q-string-fns) =========== */

/* q `x like p` — glob match.  Reuses ray_like_fn for sym/str atoms and
 * vectors; a DICT maps the match over its values (kdb: keys kept). */
static ray_t* q_like_wrap(ray_t* x, ray_t* pattern) {
    if (x && x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);   /* borrowed */
        ray_t* v = ray_dict_vals(x);   /* borrowed */
        if (!k || !v) return ray_error("type", "like: bad dict");
        ray_t* nv = q_like_wrap(v, pattern);
        if (!nv || RAY_IS_ERR(nv)) return nv;
        ray_retain(k);
        return ray_dict_new(k, nv);    /* consumes k + nv */
    }
    if (x && x->type == RAY_LIST) {    /* boxed list (e.g. dict values) */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* e = ray_at_fn(x, ia);
            ray_release(ia);
            if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
            ray_t* r = q_like_wrap(e, pattern);
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        ray_t* c = q_collapse_list(out);   /* homogeneous bool run -> bool vec */
        ray_release(out);
        return c;
    }
    return ray_like_fn(x, pattern);
}

/* Match glob pattern p[0..pn) anchored at s[pos..sn), where every pattern
 * token consumes exactly one input char: `?` (any), `[..]`/`[^..]` (class,
 * optional negation), or a literal.  The variable-width `*` form is not used
 * by q's ss/ssr, so a bare `*` is treated literally.  Returns the number of
 * input chars consumed on a full match, or -1 on mismatch. */
static int64_t q_glob_fixed_at(const char* s, size_t sn, size_t pos,
                               const char* p, size_t pn) {
    size_t si = pos, pi = 0;
    while (pi < pn) {
        if (si >= sn) return -1;
        char pc = p[pi];
        if (pc == '?') { pi++; si++; continue; }
        if (pc == '[') {
            size_t j = pi + 1;
            int neg = 0;
            if (j < pn && p[j] == '^') { neg = 1; j++; }
            int matched = 0;
            for (; j < pn && p[j] != ']'; j++)
                if (p[j] == s[si]) matched = 1;
            if (j < pn) j++;              /* skip ']' */
            if (matched == neg) return -1;
            pi = j; si++; continue;
        }
        if (pc != s[si]) return -1;       /* literal */
        pi++; si++;
    }
    return (int64_t)(si - pos);
}

/* q `s ss p` — string search: 0-based start index of every match of the glob
 * pattern p in the string s (overlapping, kdb-true).  Returns a long vector. */
static ray_t* q_ss_wrap(ray_t* s, ray_t* p) {
    if (!s || s->type != -RAY_STR || !p || p->type != -RAY_STR)
        return ray_error("type", "ss: expects string arguments");
    const char* sp = ray_str_ptr(s); size_t sn = ray_str_len(s);
    const char* pp = ray_str_ptr(p); size_t pn = ray_str_len(p);
    ray_t* out = ray_vec_new(RAY_I64, 8);
    if (RAY_IS_ERR(out)) return out;
    if (pn == 0) return out;                       /* empty pattern -> no hits */
    /* q_glob_fixed_at anchors at each position; the match WIDTH differs from
     * the pattern's byte length (a `[..]` class is 1 input char), so iterate
     * every start position and let the matcher enforce bounds. */
    for (size_t i = 0; i < sn; i++) {
        if (q_glob_fixed_at(sp, sn, i, pp, pn) >= 0) {
            int64_t idx = (int64_t)i;
            out = ray_vec_append(out, &idx);
            if (RAY_IS_ERR(out)) return out;
        }
    }
    return out;
}

/* q `ssr[s;p;r]` — replace every (non-overlapping, left-to-right) match of the
 * glob pattern p in s.  r is either a replacement string, or a function
 * applied to each matched substring (kdb: `ssr[s;"t?r";upper]`). */
ray_t* q_ssr_wrap(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("rank", "ssr: expects 3 args");
    ray_t* s = args[0]; ray_t* p = args[1]; ray_t* r = args[2];
    if (!s || s->type != -RAY_STR || !p || p->type != -RAY_STR)
        return ray_error("type", "ssr: s and p must be strings");
    int r_is_fn = q_is_fn_value(r);
    if (!r_is_fn && (!r || r->type != -RAY_STR))
        return ray_error("type", "ssr: replacement must be a string or function");
    const char* sp = ray_str_ptr(s); size_t sn = ray_str_len(s);
    const char* pp = ray_str_ptr(p); size_t pn = ray_str_len(p);
    size_t cap = sn + 16, blen = 0;
    char* b = (char*)malloc(cap);
    if (!b) return ray_error("wsfull", "ssr: out of memory");
    #define SSR_PUSH(PTR, L) do { \
        size_t _l = (L); \
        if (blen + _l > cap) { cap = (blen + _l) * 2; char* nb = (char*)realloc(b, cap); \
            if (!nb) { free(b); return ray_error("wsfull", "ssr: out of memory"); } b = nb; } \
        memcpy(b + blen, (PTR), _l); blen += _l; } while (0)
    ray_t* err = NULL;
    size_t i = 0;
    while (i < sn) {
        /* q_glob_fixed_at bounds-checks internally (si>=sn -> -1) and a `[..]`
         * class is multiple pattern bytes but ONE input char, so do NOT gate on
         * `i + pn <= sn` (that rejected bracket-class matches near the end). */
        int64_t m = pn ? q_glob_fixed_at(sp, sn, i, pp, pn) : -1;
        if (m >= 0) {
            if (r_is_fn) {
                ray_t* sub = ray_str(sp + i, (size_t)m);
                ray_t* one[1] = { sub };
                ray_t* rep = q_call_n(r, one, 1);
                ray_release(sub);
                if (!rep || RAY_IS_ERR(rep)) { err = rep; break; }
                if (rep->type != -RAY_STR) { ray_release(rep); err = ray_error("type", "ssr: replacement fn must return a string"); break; }
                SSR_PUSH(ray_str_ptr(rep), ray_str_len(rep));
                ray_release(rep);
            } else {
                SSR_PUSH(ray_str_ptr(r), ray_str_len(r));
            }
            i += (m > 0) ? (size_t)m : 1;   /* advance past the match */
        } else {
            SSR_PUSH(sp + i, 1);
            i++;
        }
    }
    #undef SSR_PUSH
    if (err) { free(b); return err; }
    ray_t* out = ray_str(b, blen);
    free(b);
    return out;
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
    case QK_CUT:  return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_cut_wrap);
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
    case QK_AT:   return ray_fn_vary  (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_at_wrap);
    case QK_DOT:  return ray_fn_vary  (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_dot_wrap);
    case QK_MIN2: return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_min2_wrap);
    case QK_NEG:  return ray_fn_unary (lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_neg_wrap);
    case QK_WITHIN: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_within_wrap);
    case QK_OVER:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_over_kw);
    case QK_SCANKW: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_scan_kw);
    case QK_PRIORKW:return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_prior_kw);
    case QK_DELTAS: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_deltas_wrap);
    case QK_DIFFER: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_differ_wrap);
    case QK_TIL:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_til_wrap);
    case QK_WHERE:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_where_wrap);
    case QK_VS:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_vs_wrap);
    case QK_SV:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_sv_wrap);
    case QK_SUMS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_sums_wrap);
    case QK_PRDS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_prds_wrap);
    case QK_MAXS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_maxs_wrap);
    case QK_MINS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mins_wrap);
    case QK_AVGS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_avgs_wrap);
    case QK_RATIOS: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ratios_wrap);
    case QK_WSUM:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_wsum_wrap);
    case QK_WAVG:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_wavg_wrap);
    case QK_COV:    return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_cov_wrap);
    case QK_SCOV:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_scov_wrap);
    case QK_MSUM:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_msum_wrap);
    case QK_MAVG:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mavg_wrap);
    case QK_MMAX:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mmax_wrap);
    case QK_MMIN:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mmin_wrap);
    case QK_MCOUNT: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mcount_wrap);
    case QK_MDEV:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mdev_wrap);
    case QK_EMA:    return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ema_wrap);
    case QK_FILL:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_fill_wrap);
    case QK_ROTATE: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_rotate_wrap);
    case QK_SUBLIST:return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_sublist_wrap);
    case QK_NEXT:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_next_wrap);
    case QK_PREV:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_prev_wrap);
    case QK_HOPEN:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_hopen_wrap);
    case QK_HCLOSE: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_hclose_wrap);
    case QK_NULL:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_null_wrap);
    case QK_LIKE:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_like_wrap);
    case QK_SS:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ss_wrap);
    case QK_SSR:    return ray_fn_vary  (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ssr_wrap);
    case QK_UNION:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_union_wrap);
    case QK_INTER:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_inter_wrap);
    case QK_CROSS:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_cross_wrap);
    case QK_SIN:        return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_sin_wrap);
    case QK_COS:        return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_cos_wrap);
    case QK_TAN:        return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_tan_wrap);
    case QK_ASIN:       return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_asin_wrap);
    case QK_ACOS:       return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_acos_wrap);
    case QK_ATAN:       return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_atan_wrap);
    case QK_RECIPROCAL: return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_reciprocal_wrap);
    case QK_SIGNUM:     return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_signum_wrap);
    case QK_CEILING:    return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_ceiling_wrap);
    case QK_XBAR:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xbar_wrap);
    case QK_ASC:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_asc_wrap);
    case QK_DESC:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_desc_wrap);
    case QK_IASC:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_iasc_wrap);
    case QK_IDESC:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_idesc_wrap);
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
    g_over_value = ray_fn_vary("over", RAY_FN_NONE | RAY_FN_Q_LOWER, q_over_wrap);
    if (!g_over_value || RAY_IS_ERR(g_over_value)) {
        g_over_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_eachboth_value = ray_fn_vary("each-both", RAY_FN_NONE | RAY_FN_Q_LOWER, q_eachboth_wrap);
    if (!g_eachboth_value || RAY_IS_ERR(g_eachboth_value)) {
        g_eachboth_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_prior_value = ray_fn_vary("each-prior", RAY_FN_NONE | RAY_FN_Q_LOWER, q_prior_wrap);
    if (!g_prior_value || RAY_IS_ERR(g_prior_value)) {
        g_prior_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_mkderiv2_value = ray_fn_binary("q.mkderiv2", RAY_FN_NONE | RAY_FN_Q_LOWER, q_mkderiv2);
    if (!g_mkderiv2_value || RAY_IS_ERR(g_mkderiv2_value)) {
        g_mkderiv2_value = NULL;
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
    g_keyed_table_value = ray_fn_vary("keyed-table",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_keyed_table_build);
    if (!g_keyed_table_value || RAY_IS_ERR(g_keyed_table_value)) {
        g_keyed_table_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_select_value = ray_fn_vary("q.select",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_select_exec);
    if (!g_select_value || RAY_IS_ERR(g_select_value)) {
        g_select_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    /* compose builder — a NORMAL vary (args are the resolved function values). */
    g_compose_value = ray_fn_vary("q.compose", RAY_FN_Q_LOWER, q_compose_fn);
    if (!g_compose_value || RAY_IS_ERR(g_compose_value)) {
        g_compose_value = NULL;
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
    g_lambda_value = ray_fn_vary("q.fn",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_fn_make);
    if (!g_lambda_value || RAY_IS_ERR(g_lambda_value)) {
        g_lambda_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_ret_value = ray_fn_unary("q.ret", RAY_FN_Q_LOWER, q_ret_fn);
    if (!g_ret_value || RAY_IS_ERR(g_ret_value)) {
        g_ret_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_sig_value = ray_fn_unary("q.sig", RAY_FN_Q_LOWER, q_sig_fn);
    if (!g_sig_value || RAY_IS_ERR(g_sig_value)) {
        g_sig_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_seq_value = ray_fn_vary("q.seq",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_seq_fn);
    if (!g_seq_value || RAY_IS_ERR(g_seq_value)) {
        g_seq_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_if_value = ray_fn_vary("q.if",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_if_fn);
    if (!g_if_value || RAY_IS_ERR(g_if_value)) {
        g_if_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_do_value = ray_fn_vary("q.do",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_do_fn);
    if (!g_do_value || RAY_IS_ERR(g_do_value)) {
        g_do_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_while_value = ray_fn_vary("q.while",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_while_fn);
    if (!g_while_value || RAY_IS_ERR(g_while_value)) {
        g_while_value = NULL;
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
    if (g_over_value)     { ray_release(g_over_value);     g_over_value     = NULL; }
    if (g_eachboth_value) { ray_release(g_eachboth_value); g_eachboth_value = NULL; }
    if (g_prior_value)    { ray_release(g_prior_value);    g_prior_value    = NULL; }
    if (g_mkderiv2_value) { ray_release(g_mkderiv2_value); g_mkderiv2_value = NULL; }
    if (g_list_value)   { ray_release(g_list_value);   g_list_value   = NULL; }
    if (g_table_value)  { ray_release(g_table_value);  g_table_value  = NULL; }
    if (g_keyed_table_value) { ray_release(g_keyed_table_value); g_keyed_table_value = NULL; }
    if (g_select_value) { ray_release(g_select_value); g_select_value = NULL; }
    if (g_compose_value) { ray_release(g_compose_value); g_compose_value = NULL; }
    if (g_funsql_select_value) { ray_release(g_funsql_select_value); g_funsql_select_value = NULL; }
    if (g_funsql_bang_value)   { ray_release(g_funsql_bang_value);   g_funsql_bang_value   = NULL; }
    if (g_lambda_value)        { ray_release(g_lambda_value);        g_lambda_value        = NULL; }
    if (g_ret_value)           { ray_release(g_ret_value);           g_ret_value           = NULL; }
    if (g_sig_value)           { ray_release(g_sig_value);           g_sig_value           = NULL; }
    if (g_seq_value)           { ray_release(g_seq_value);           g_seq_value           = NULL; }
    if (g_if_value)            { ray_release(g_if_value);            g_if_value            = NULL; }
    if (g_do_value)            { ray_release(g_do_value);            g_do_value            = NULL; }
    if (g_while_value)         { ray_release(g_while_value);         g_while_value         = NULL; }
    if (g_qret_payload)        { ray_release(g_qret_payload);        g_qret_payload        = NULL; }
    if (g_qsig_payload)        { ray_release(g_qsig_payload);        g_qsig_payload        = NULL; }
    g_count  = 0;
    g_inited = false;
}
