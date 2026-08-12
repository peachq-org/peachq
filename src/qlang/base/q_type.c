/* q_type — the type-axis home (contract: q_type.h): the q-side lane accessors,
 * the type-number / null axes, and the tag<->name vocabulary. Relocated out of
 * the frozen base header (lane extensions) and out of q_builtins/q_dollar/
 * q_math/q_table (the type + null knowledge) so each has ONE obvious home. */
#include "qlang/base/q_type.h"
#include "qlang/base/q_err.h"          /* q_err / QE_TYPE — the throwing gates */
#include "lang/internal.h"        /* as_i64, is_numeric/is_temporal, is_collection,
                                   * atomic_map_unary, ray_nil_fn */
#include "table/sym.h"            /* ray_sym_str — the null-symbol divergence */
#include "core/types.h"           /* RAY_TYPE_COUNT — the tag-indexed matrix bound */
#include <string.h>               /* memchr/memset — matrix bake-out (init only) */
#include <math.h>                 /* isinf — the infinity lane */

int64_t q_type_as_i64(ray_t* x) {
    if (RAY_IS_TEMPORAL32(-x->type)) return (int64_t)x->i32;
    if (RAY_IS_TEMPORAL64(-x->type)) return x->i64;
    return as_i64(x);
}

int q_type_is_numeric_or_temporal(ray_t* x) {
    return is_numeric(x) || is_temporal(x) || RAY_IS_TEMPORALF(-x->type);
}

int q_type_is_bool(ray_t* x) {
    return x->type == -RAY_BOOL;
}

int q_type_is_float_tag(int8_t t) {
    return t == RAY_F64 || t == RAY_F32 || t == -RAY_F64 || t == -RAY_F32;
}

int q_type_is_num_tag(int8_t t) {
    return t == RAY_BOOL || t == RAY_BYTE_ONLY || t == RAY_I16 || t == RAY_I32 || t == RAY_I64 ||
           t == RAY_F32 || t == RAY_F64 || t == -RAY_BOOL || t == -RAY_BYTE_ONLY || t == -RAY_I16 ||
           t == -RAY_I32 || t == -RAY_I64 || t == -RAY_F32 || t == -RAY_F64;
}

/* ---- tag <-> name vocabulary --------------------------------------------- */

/* Vector tag -> kdb type name (`meta`/`key` type rows, empty-vec display).
 * No `default:` — total over the value band (#209): a new datatype refuses to
 * build until it names its q-spelling here. LIST/STR have no scalar name; the
 * -RAY_STR atom shim names `char` at the call sites. */
const char* q_type_qname(int8_t t) {
    switch ((ray_type_e)t) {
    case RAY_BOOL:      return "boolean";
    case RAY_BYTE_ONLY: return "byte";
    case RAY_I16:       return "short";
    case RAY_I32:       return "int";
    case RAY_I64:       return "long";
    case RAY_F32:       return "real";
    case RAY_F64:       return "float";
    case RAY_SYM:       return "symbol";
    case RAY_GUID:      return "guid";       /* `key 0#0Ng` -> `guid (was a gap) */
    case RAY_DATE:      return "date";
    case RAY_MONTH:     return "month";
    case RAY_MINUTE:    return "minute";
    case RAY_SECOND:    return "second";
    case RAY_TIME:      return "time";
    case RAY_TIMESPAN:  return "timespan";
    case RAY_TIMESTAMP: return "timestamp";
    case RAY_DATETIME:  return "datetime";
    case RAY_CHARV: return "char";
    case RAY_LIST: case RAY_STR: return NULL;   /* boxed / physical: unnamed */
    }
    return NULL;   /* unreachable: value band is exhausted above */
}

/* tag -> rayfall `as` type-sym spelling (cast delegation targets only) */
const char* q_type_rayname(int8_t tag) {
    switch (tag) {
    case RAY_BOOL: return "BOOL"; case RAY_BYTE_ONLY: return "U8";
    case RAY_I16:  return "I16";  case RAY_I32: return "I32";
    case RAY_I64:  return "I64";  case RAY_F64: return "F64";
    case RAY_DATE: return "DATE"; case RAY_TIME: return "TIME";
    case RAY_MONTH: return "MONTH";
    case RAY_MINUTE: return "MINUTE";
    case RAY_SECOND: return "SECOND";
    case RAY_TIMESPAN: return "TIMESPAN";
    case RAY_TIMESTAMP: return "TIMESTAMP";
    case RAY_DATETIME: return "DATETIME";
    default:       return NULL;
    }
}

char q_type_char(int8_t tag) {
    int t = tag < 0 ? -tag : tag;                   /* int arith: |INT8_MIN|==128, no UB */
    if (t <= 0 || t >= RAY_TYPE_COUNT) return 0;    /* list 0, gap 3 & out-of-band: no char */
    switch ((ray_type_e)t) {                        /* exhaustive: a new member demands a lane */
    case RAY_LIST:      return 0;                   /* unreachable: filtered above */
    case RAY_BOOL:      return 'b';
    case RAY_GUID:      return 'g';
    case RAY_BYTE_ONLY: return 'x';
    case RAY_I16:       return 'h';
    case RAY_I32:       return 'i';
    case RAY_I64:       return 'j';
    case RAY_F32:       return 'e';
    case RAY_F64:       return 'f';
    case RAY_STR:       return 'c';   /* physical string storage: still char text */
    case RAY_CHARV:     return 'c';
    case RAY_SYM:       return 's';
    case RAY_TIMESTAMP: return 'p';
    case RAY_MONTH:     return 'm';
    case RAY_DATE:      return 'd';
    case RAY_DATETIME:  return 'z';
    case RAY_TIMESPAN:  return 'n';
    case RAY_MINUTE:    return 'u';
    case RAY_SECOND:    return 'v';
    case RAY_TIME:      return 't';
    }
    return 0;   /* SEL(20) etc.: no type char (unchanged) */
}

int8_t q_type_of_char(char c) {
    /* derived by scanning q_type_char so the tag<->char map stays one owner;
     * RAY_STR shares 'c' with CHARV and is skipped (physical, q-invisible) */
    if (!c) return 0;   /* charless tags answer 0: never let 0 match one */
    for (int t = 1; t < RAY_TYPE_COUNT; t++) {
        if (t == RAY_STR) continue;
        if (q_type_char((int8_t)t) == c) return (int8_t)t;
    }
    return 0;
}

/* ---- the mixed-pair result-type law (contract: q_type.h) ------------------
 * Distinct from base's promote() (ops/graph.c), which answers "what C lane do I
 * compute in" and collapses DATE to i32: this answers what the RESULT IS.
 *
 * The published table is kept as the DOC-SHAPED source below so a reviewer can
 * diff it against the page line for line, and q_type_init bakes it into a
 * tag-indexed array once — the lookup is a plain 2-D index on the family lift's
 * per-element-pair path, and no tag list is written down twice (q_type_char is
 * the one tag<->char owner, so a new datatype breaks ITS switch rather than
 * silently falling out of a parallel table here). */

/* Row/column order of the printed matrix. */
static const char Q_COMMON_ORDER[] = "bgxhijefcspmdznuvt";

/* ref/lesser.md + ref/greater.md "Domain and range", transcribed cell for cell
 * (both pages print the same matrix).  '.' = no result type.
 * NOT derived: the temporal block is one order (m<d<z<p<u<v<t<n) minus nine
 * hole pairs, eight of which are "date-only x time-only" but `m`x`z` is a lone
 * exception (`d`x`z` and `m`x`p` both resolve), so a rule would need an
 * exception list to reproduce the page. */
static const char* const Q_COMMON[18] = {
    "b.xhijefc.pmdznuvt",   /* b */
    "..................",   /* g */
    "x.xhijefc.pmdznuvt",   /* x */
    "h.hhijefc.pmdznuvt",   /* h */
    "i.iiijefc.pmdznuvt",   /* i */
    "j.jjjjefc.pmdznuvt",   /* j */
    "e.eeeeefc.pmdznuvt",   /* e */
    "f.ffffffc.pmdznuvt",   /* f */
    "c.ccccccc.pmdznuvt",   /* c */
    "..................",   /* s */
    "p.ppppppp.ppppnuvt",   /* p */
    "m.mmmmmmm.pmd.....",   /* m */
    "d.ddddddd.pddz....",   /* d */
    "z.zzzzzzz.p.zznuvt",   /* z */
    "n.nnnnnnn.n..nnnnn",   /* n */
    "u.uuuuuuu.u..unuvt",   /* u */
    "v.vvvvvvv.v..vnvvt",   /* v */
    "t.ttttttt.t..tnttt",   /* t */
};

static int8_t g_common[RAY_TYPE_COUNT][RAY_TYPE_COUNT];

void q_type_init(void) {
    int8_t pos[RAY_TYPE_COUNT], tag_of[sizeof Q_COMMON_ORDER];
    memset(g_common, 0, sizeof g_common);
    memset(pos, -1, sizeof pos);
    memset(tag_of, 0, sizeof tag_of);
    for (int t = 1; t < RAY_TYPE_COUNT; t++) {
        if (t == RAY_STR) continue;   /* physical storage, q-invisible: not a row */
        const char* c = memchr(Q_COMMON_ORDER, q_type_char((int8_t)t), 18);
        if (!c) continue;
        pos[t] = (int8_t)(c - Q_COMMON_ORDER);
        tag_of[pos[t]] = (int8_t)t;
    }
    for (int a = 1; a < RAY_TYPE_COUNT; a++)
        for (int b = 1; b < RAY_TYPE_COUNT; b++) {
            if (pos[a] < 0 || pos[b] < 0) continue;
            const char* c = memchr(Q_COMMON_ORDER, Q_COMMON[pos[a]][pos[b]], 18);
            if (c) g_common[a][b] = tag_of[c - Q_COMMON_ORDER];
        }
}

int8_t q_type_common(int8_t a, int8_t b) {
    if (a < 1 || a >= RAY_TYPE_COUNT || b < 1 || b >= RAY_TYPE_COUNT) return 0;
    return g_common[a][b];
}

/* The empty typed vector of tag (sym vectors need their width ctor). */
ray_t* q_type_empty(int8_t tag) {
    return tag == RAY_SYM ? ray_sym_vec_new(RAY_SYM_W64, 0)
                          : ray_vec_new(tag, 0);
}

int8_t q_type_elem_tag(ray_t* x) {
    if (!x) return 0;
    return ray_is_atom(x) ? x->type : ray_is_vec(x) ? (int8_t)-x->type : 0;
}

/* ---- int/float admission (which numeric lane may a verb read) ------------- */

/* Is vector tag `t` legal as a ROW INDEX (uniform-structure-dispatch §2.2:
 * any int-backed index indexes)?  Returns the element width in bytes so
 * consumers key on WIDTH, never enumerate tags: 8/4/2 read signed, 1 reads
 * unsigned (U8); 0 = not an int index.  SYM is i64-backed but EXCLUDED —
 * a sym index means column access, never a row.  BOOL is excluded pending
 * a doc citation for boolean indexing.  Atom callers pass -atom->type;
 * every accepted ATOM stores its payload in .i64 (vec/atom.c), so as_i64's
 * fallback reads it correctly. */
int q_type_int_index_width(int8_t t) {
    switch (t) {
    case RAY_I64: return 8;
    case RAY_I32: return 4;
    case RAY_I16: return 2;
    case RAY_BYTE_ONLY: return 1;
    default:
        if (RAY_IS_TEMPORAL64(t)) return 8;   /* timestamp, timespan */
        if (RAY_IS_TEMPORAL32(t)) return 4;   /* month date minute second time */
        return 0;
    }
}

/* Strict cast: cast-or-fail, TYPE-strict (an integral-valued float refuses);
 * typed nulls pass through as sentinel payloads — value checks stay at the
 * call site.  Accepted set = the q_type_int_index_width law: I64/I32/I16/U8 +
 * int-backed temporal ATOMS; never sym/bool/float/structures.
 * Returns 1 + *out, or 0 on refusal. */
int q_type_strict_i64(ray_t* x, int64_t* out) {
    if (!x || x->type >= 0) return 0;
    switch (q_type_int_index_width((int8_t)-x->type)) {
    case 8: *out = x->i64;          return 1;
    case 4: *out = (int64_t)x->i32; return 1;
    case 2: *out = (int64_t)x->i16; return 1;
    case 1: *out = (int64_t)x->u8;  return 1;
    default: return 0;
    }
}

/* Float twin: F64/F32/DATETIME (f64-slot payloads) + the q_type_strict_i64 set. */
int q_type_strict_f64(ray_t* x, double* out) {
    if (!x || x->type >= 0) return 0;
    if (x->type == -RAY_F64 || RAY_IS_TEMPORALF(-x->type)) { *out = x->f64; return 1; }
    if (x->type == -RAY_F32) { *out = (double)(float)x->f64; return 1; }
    int64_t v;
    if (!q_type_strict_i64(x, &v)) return 0;
    *out = (double)v;
    return 1;
}

/* Throwing gate for TERMINAL sites (failure = error): NULL on success, else the
 * bare 'type class (q_err carries no string post-#305). `what` is the caller's
 * site context ("verb: role") reserved for a future error-context layer — held,
 * not embedded. The silent probe form above is for dispatch sites (next arm). */
ray_t* q_type_i64_or_err(ray_t* x, int64_t* out, const char* what) {
    return q_type_strict_i64(x, out) ? NULL : q_err(QE_TYPE);
}

/* Type-facts helpers (I64/I32/I16 only — vs/sv base-encode domain). */
int q_type_is_int_atom(ray_t* x) {
    return x && (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16);
}
int q_type_is_int_vec(ray_t* x) {
    return x && (x->type == RAY_I64 || x->type == RAY_I32 || x->type == RAY_I16);
}
int64_t q_type_ivec_get(ray_t* v, int64_t i) {
    const void* d = ray_data(v);
    return v->type == RAY_I64 ? ((const int64_t*)d)[i]
         : v->type == RAY_I32 ? (int64_t)((const int32_t*)d)[i]
                              : (int64_t)((const int16_t*)d)[i];
}
int64_t q_type_iatom_val(ray_t* x) {
    return x->type == -RAY_I64 ? x->i64
         : x->type == -RAY_I32 ? (int64_t)x->i32 : (int64_t)x->i16;
}

/* ---- keyed-table shape (a type question, owner-ruled) -------------------- */

int q_type_is_table(ray_t* x) {
    return x && x->type == RAY_TABLE;
}

/* A keyed table is a RAY_DICT whose keys AND values are both tables. */
int q_type_is_keyed(ray_t* y) {
    if (!y || y->type != RAY_DICT) return 0;
    ray_t* k = ray_dict_keys(y);
    ray_t* v = ray_dict_vals(y);
    return k && v && k->type == RAY_TABLE && v->type == RAY_TABLE;
}

/* A dict that is NOT a keyed table — the plain key!value shape, whose entries
 * axis the search verbs read (find's reverse lookup, bin's sorted values). */
int q_type_is_plain_dict(ray_t* x) {
    return x && x->type == RAY_DICT && !q_type_is_keyed(x);
}

/* Any dictionary, keyed table included — for laws on the entries axis that
 * cover both (group's dict arm, `!`'s equal-count totality). */
int q_type_is_dict(ray_t* x) {
    return x && x->type == RAY_DICT;
}

/* Column readable by the dense group core (agg_group_keys' int64/sym/str key
 * lanes, src/ops/agg_engine.c) — anything else takes a boxed row-compare. */
int q_type_is_dense_group_col(ray_t* c) {
    switch (c ? c->type : RAY_LIST) {
        case RAY_I64: RAY_TEMPORAL64_CASES:
        case RAY_I32: RAY_TEMPORAL32_CASES:
        case RAY_I16: RAY_BYTE_CASES: case RAY_BOOL:
        case RAY_SYM: case RAY_STR:
            return 1;
        default: return 0;
    }
}

/* The physical STR atom lane (string-C3): q-invisible storage that reads back
 * as an ITEM of a string column.  Rank-wise it is a char LIST, not a scalar,
 * so rank-sensitive verbs must ask this rather than probe the tag. */
int q_type_is_str_atom(ray_t* x) {
    return x && x->type == -RAY_STR;
}

int q_type_is_char_atom(ray_t* x) {
    return x && x->type == -RAY_CHARV;
}

int q_type_is_sym_atom(ray_t* x) {
    return x && x->type == -RAY_SYM;
}

int q_type_is_iter(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return 0;
    return v->type == RAY_LIST || v->type == RAY_TABLE ||
           (ray_is_vec(v) && v->type != RAY_STR);
}

/* ---- null axis (owner-ruled: null is not math) --------------------------- */

/* RAY_ATOM_IS_NULL's twin at the other end of the lane: the float lanes carry a
 * real ±Inf, the int-backed ones the ±ray_type_inf sentinel (rayforce.h owns
 * the payload).  A type with no pinned infinity answers 0. */
int q_type_is_inf(ray_t* x) {
    if (!x || x->type >= 0) return 0;
    int8_t t = (int8_t)-x->type;
    if (t == RAY_F64 || t == RAY_F32 || t == RAY_DATETIME) return isinf(x->f64);
    int64_t inf;
    if (!ray_type_inf(t, true, &inf)) return 0;
    int64_t v = q_type_as_i64(x);
    return v == inf || v == -inf;
}

/* q treats the null symbol ` AS null — shared predicate (the `null` verb,
 * ops/q_vs_sv.c dispatch). */
int q_type_is_null_sym(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return 0;
    ray_t* s = ray_sym_str(x->i64);
    int z = s && ray_str_len(s) == 0;
    return z;
}

