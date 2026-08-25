/* q_json — peachq's `.j` JSON namespace (ref/dotj.md).
 *
 * .j.j  serialize: hand-rolled recursive type-dispatch over ray_t -> compact
 *       JSON (no whitespace).  Types JSON can express get a native arm; the
 *       rest ship their q token as a string via j_tok_flags (json.k's law).
 *       An unimplemented type is still a clean `'nyi` (the extension seam).
 * .j.k  deserialize: vendored yyjson (third_party/yyjson, MIT) does the raw
 *       tokenize/validate/unescape/number-parse; the node->ray_t mapping here
 *       owns the q-type semantics (object->dict, array->collapsed list,
 *       string->char-vector, number->float, true/false->bool, inf->0w,
 *       null/nan->0n).
 *
 * Floats: NaN -> `null`, ±0w -> `inf`/`-inf` (dotj.md:47); the magnitude is
 * q_fmt_float's, so `\P` governs JSON (syscmds.md:546).
 * Pure q-layer: no frozen-base edits. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/net/q_json.h"
#include "qlang/q_prim.h"
#include "qlang/base/q_err.h"
#include "qlang/q_fmt.h"      /* q_fmt / q_fmt_float — the display + `\P` home */
#include "qlang/base/q_type.h"     /* q_type_is_inf — the infinity lane */
#include "lang/eval.h"        /* RAY_FN_NONE, ray_at_fn — dict/table cell reads */
#include "qlang/ops/q_bang.h" /* q_bang_enkey — the unkey primitive */
#include "qlang/ops/q_dollar.h"  /* q_dollar_cast / q_dollar_tok — the one conversion home */
#include "qlang/io/q_io.h"       /* q_io_file_path + the resource-read seam */
#include "qlang/q_env.h"         /* q_env_bind — the .j.i.* bindings */
#include "lang/env.h"            /* ray_fn_vary */
#include "table/sym.h"        /* ray_sym_vec_cell */
#include <rayforce.h>
#include "yyjson.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== *
 *  .j.j — serialize
 * ======================================================================== */

/* Growable byte buffer.  On OOM or an unimplemented type the flag latches and
 * every further append is a no-op; the caller turns the flag into an error. */
typedef struct {
    char*  p;
    size_t len;
    size_t cap;
    int    oom;
    int    nyi;
} jbuf;

static int jbuf_reserve(jbuf* b, size_t extra) {
    if (b->oom || b->nyi) return 0;
    if (b->len + extra <= b->cap) return 1;
    size_t ncap = b->cap ? b->cap * 2 : 64;
    while (ncap < b->len + extra) ncap *= 2;
    char* np = realloc(b->p, ncap);
    if (!np) { b->oom = 1; return 0; }
    b->p = np;
    b->cap = ncap;
    return 1;
}

static void jbuf_putc(jbuf* b, char c) {
    if (!jbuf_reserve(b, 1)) return;
    b->p[b->len++] = c;
}

static void jbuf_putn(jbuf* b, const char* s, size_t n) {
    if (!jbuf_reserve(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void jbuf_puts(jbuf* b, const char* s) { jbuf_putn(b, s, strlen(s)); }

/* Emit a JSON string literal (surrounding quotes + RFC-8259 escaping). */
static void jbuf_str(jbuf* b, const char* s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    jbuf_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  jbuf_putn(b, "\\\"", 2); break;
            case '\\': jbuf_putn(b, "\\\\", 2); break;
            case '\b': jbuf_putn(b, "\\b", 2);  break;
            case '\f': jbuf_putn(b, "\\f", 2);  break;
            case '\n': jbuf_putn(b, "\\n", 2);  break;
            case '\r': jbuf_putn(b, "\\r", 2);  break;
            case '\t': jbuf_putn(b, "\\t", 2);  break;
            default:
                if (c < 0x20) {
                    char u[6] = { '\\', 'u', '0', '0', hex[(c >> 4) & 0xF], hex[c & 0xF] };
                    jbuf_putn(b, u, 6);
                } else {
                    jbuf_putc(b, (char)c);
                }
        }
    }
    jbuf_putc(b, '"');
}

static void jbuf_int(jbuf* b, long long v) {
    char t[24];
    int n = snprintf(t, sizeof t, "%lld", v);
    if (n > 0) jbuf_putn(b, t, (size_t)n);
}

/* Float -> JSON number.  NaN -> `null`, ±inf -> `inf`/`-inf` (dotj.md:47).
 * Finite values inherit q_fmt_float, so `\P` governs JSON (syscmds.md:546);
 * f32=0 always — the `e` suffix is a q type marker, not JSON. */
static void jbuf_flt(jbuf* b, double v) {
    if (isnan(v)) { jbuf_puts(b, "null"); return; }
    if (isinf(v)) { jbuf_puts(b, v < 0 ? "-inf" : "inf"); return; }
    char t[64];
    q_fmt_float(v, 0, t, sizeof t);
    jbuf_puts(b, t);
}

static void j_emit(jbuf* b, ray_t* x);
static void j_table(jbuf* b, ray_t* x);

/* An unimplemented type: latch the flag so the whole serialize fails 'nyi. */
static void j_nyi(jbuf* b, int8_t t) {
    (void)t;
    if (!b->nyi) b->nyi = 1;
}

/* json.k's `J[-t]@$x` (KxSystems/kdb e/json.k, Apache-2.0): types with no JSON
 * counterpart ship their q TOKEN as a string.  DASH8 maps `.`->`-` inside
 * `8#x` — POSITIONAL, which is why month keeps its `m` and a timestamp keeps
 * both its `D` and its fractional dot.  ABSENT (the whole temporal family, and
 * only it — a guid's null IS the zero UUID) sends 0N/0W/-0W to JSON `null`
 * instead of leaking q syntax; owner ruling, see the suite header. */
enum { JT_TOKEN = 1, JT_DASH8 = 2, JT_ABSENT = 4 };

static int j_tok_flags(int8_t t) {
    switch (t) {
        case RAY_TIMESTAMP: case RAY_MONTH: case RAY_DATE: case RAY_DATETIME:
            return JT_TOKEN | JT_DASH8 | JT_ABSENT;
        case RAY_TIMESPAN: case RAY_MINUTE: case RAY_SECOND: case RAY_TIME:
            return JT_TOKEN | JT_ABSENT;
        case RAY_GUID: case RAY_BYTE_ONLY:
            return JT_TOKEN;
        default:
            return 0;
    }
}

/* One tokenised atom.  The token comes from q_fmt (the display home) —
 * `string` would emit `0Ng` for a guid and signal on a byte vector. */
static void j_tok(jbuf* b, ray_t* a, int flags) {
    char t[64];
    if (!a || RAY_IS_ERR(a)) { j_nyi(b, 0); return; }
    if ((flags & JT_ABSENT) && (RAY_ATOM_IS_NULL(a) || q_type_is_inf(a))) {
        jbuf_puts(b, "null");
        return;
    }
    q_fmt(a, t, sizeof t);
    size_t n = strlen(t);
    if (flags & JT_DASH8)
        for (size_t i = 0; i < n && i < 8; i++)
            if (t[i] == '.') t[i] = '-';
    jbuf_str(b, t, n);
}

/* One atom (x->type < 0). */
static void j_atom(jbuf* b, ray_t* x) {
    switch (-x->type) {
        case RAY_BOOL: jbuf_puts(b, x->b8 ? "true" : "false"); break;
        case RAY_I16:  RAY_ATOM_IS_NULL(x) ? jbuf_puts(b, "null") : jbuf_int(b, x->i16); break;
        case RAY_I32:  RAY_ATOM_IS_NULL(x) ? jbuf_puts(b, "null") : jbuf_int(b, x->i32); break;
        case RAY_I64:  RAY_ATOM_IS_NULL(x) ? jbuf_puts(b, "null") : jbuf_int(b, x->i64); break;
        case RAY_F32:  jbuf_flt(b, x->f64); break;   /* F32 atom reuses f64 slot */
        case RAY_F64:  jbuf_flt(b, x->f64); break;
        case RAY_STR:  jbuf_str(b, ray_str_ptr(x), ray_str_len(x)); break;  /* string atom */
        case RAY_CHARV: jbuf_str(b, (const char*)&x->u8, 1); break;         /* char atom */
        case RAY_SYM: {
            ray_t* s = ray_sym_str(x->i64);          /* borrowed */
            if (s) jbuf_str(b, ray_str_ptr(s), ray_str_len(s));
            else   jbuf_str(b, "", 0);
            break;
        }
        default: {
            int fl = j_tok_flags((int8_t)-x->type);
            fl ? j_tok(b, x, fl) : j_nyi(b, x->type);
            break;
        }
    }
}

/* Emit one object key as a JSON string.  A boxed key atom is a sym or a
 * char-vector; anything else is stringified through j_emit and, if that did
 * not already produce a quoted string, wrapped in quotes. */
static void j_key(jbuf* b, ray_t* k) {
    if (k && k->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(k->i64);
        jbuf_str(b, s ? ray_str_ptr(s) : "", s ? ray_str_len(s) : 0);
        return;
    }
    { const char* kp; int64_t kn;
      if (k && k->type != -RAY_SYM && q_str_text_bytes(k, &kp, &kn)) { jbuf_str(b, kp, (size_t)kn); return; } }
    /* numeric / other key: render its JSON, quote if not already a string */
    size_t start = b->len;
    int wrap = 1;
    j_emit(b, k);
    if (b->len > start && b->p && b->p[start] == '"') wrap = 0;
    if (wrap) {
        /* re-emit quoted: shift is awkward, so just re-render into quotes */
        b->len = start;
        jbuf_putc(b, '"');
        j_emit(b, k);
        jbuf_putc(b, '"');
    }
}

/* dict -> JSON object.  A keyed table (keys is a TABLE) unkeys first and emits as a
 * table, the `0!` in doth.md:686's printed `.h.tx[`json]` source. */
static void j_dict(jbuf* b, ray_t* x) {
    ray_t* keys = ray_dict_keys(x);    /* borrowed */
    ray_t* vals = ray_dict_vals(x);    /* borrowed */
    if (!keys) { j_nyi(b, x->type); return; }
    if (keys->type == RAY_TABLE) {
        ray_t* flat = q_bang_enkey(0, x);
        if (!flat || RAY_IS_ERR(flat)) { if (flat) ray_error_free(flat); j_nyi(b, x->type); return; }
        j_table(b, flat);
        ray_release(flat);
        return;
    }
    int64_t n = ray_len(keys);
    jbuf_putc(b, '{');
    for (int64_t i = 0; i < n; i++) {
        if (i) jbuf_putc(b, ',');
        if (keys->type == RAY_SYM) {
            ray_t* s = ray_sym_vec_cell(keys, i);   /* borrowed */
            jbuf_str(b, s ? ray_str_ptr(s) : "", s ? ray_str_len(s) : 0);
        } else {
            ray_t* ia = ray_i64(i);
            ray_t* k = ray_at_fn(keys, ia);
            ray_release(ia);
            j_key(b, k);
            if (k) ray_release(k);
        }
        jbuf_putc(b, ':');
        ray_t* ia = ray_i64(i);
        ray_t* v = ray_at_fn(vals, ia);
        ray_release(ia);
        j_emit(b, v);
        if (v) ray_release(v);
    }
    jbuf_putc(b, '}');
}

/* table -> JSON array of row objects. */
static void j_table(jbuf* b, ray_t* x) {
    int64_t nr = ray_table_nrows(x);
    int64_t nc = ray_table_ncols(x);
    jbuf_putc(b, '[');
    for (int64_t r = 0; r < nr; r++) {
        if (r) jbuf_putc(b, ',');
        jbuf_putc(b, '{');
        for (int64_t c = 0; c < nc; c++) {
            if (c) jbuf_putc(b, ',');
            ray_t* nm = ray_sym_str(ray_table_col_name(x, c));   /* borrowed */
            jbuf_str(b, nm ? ray_str_ptr(nm) : "", nm ? ray_str_len(nm) : 0);
            jbuf_putc(b, ':');
            ray_t* col = ray_table_get_col_idx(x, c);            /* borrowed */
            ray_t* ia = ray_i64(r);
            ray_t* cell = ray_at_fn(col, ia);
            ray_release(ia);
            j_emit(b, cell);
            if (cell) ray_release(cell);
        }
        jbuf_putc(b, '}');
    }
    jbuf_putc(b, ']');
}

/* Emit a homogeneous typed vector as a JSON array. */
static void j_vec(jbuf* b, ray_t* x) {
    int64_t n = ray_len(x);
    jbuf_putc(b, '[');
    switch (x->type) {
        case RAY_BOOL: {
            const uint8_t* d = (const uint8_t*)ray_data(x);
            for (int64_t i = 0; i < n; i++) { if (i) jbuf_putc(b, ','); jbuf_puts(b, d[i] ? "true" : "false"); }
            break;
        }
        case RAY_I16: {
            const int16_t* d = (const int16_t*)ray_data(x);
            for (int64_t i = 0; i < n; i++) { if (i) jbuf_putc(b, ','); d[i] == NULL_I16 ? jbuf_puts(b, "null") : jbuf_int(b, d[i]); }
            break;
        }
        case RAY_I32: {
            const int32_t* d = (const int32_t*)ray_data(x);
            for (int64_t i = 0; i < n; i++) { if (i) jbuf_putc(b, ','); d[i] == NULL_I32 ? jbuf_puts(b, "null") : jbuf_int(b, d[i]); }
            break;
        }
        case RAY_I64: {
            const int64_t* d = (const int64_t*)ray_data(x);
            for (int64_t i = 0; i < n; i++) { if (i) jbuf_putc(b, ','); d[i] == NULL_I64 ? jbuf_puts(b, "null") : jbuf_int(b, (long long)d[i]); }
            break;
        }
        case RAY_F32: {
            const float* d = (const float*)ray_data(x);
            for (int64_t i = 0; i < n; i++) { if (i) jbuf_putc(b, ','); jbuf_flt(b, (double)d[i]); }
            break;
        }
        case RAY_F64: {
            const double* d = (const double*)ray_data(x);
            for (int64_t i = 0; i < n; i++) { if (i) jbuf_putc(b, ','); jbuf_flt(b, d[i]); }
            break;
        }
        case RAY_SYM: {
            for (int64_t i = 0; i < n; i++) {
                if (i) jbuf_putc(b, ',');
                ray_t* s = ray_sym_vec_cell(x, i);   /* borrowed */
                jbuf_str(b, s ? ray_str_ptr(s) : "", s ? ray_str_len(s) : 0);
            }
            break;
        }
        case RAY_STR: {   /* string VECTOR (list of char-vectors) -> array of strings */
            for (int64_t i = 0; i < n; i++) {
                if (i) jbuf_putc(b, ',');
                size_t sl = 0;
                const char* sp = ray_str_vec_get(x, i, &sl);
                jbuf_str(b, sp ? sp : "", sp ? sl : 0);
            }
            break;
        }
        default: {                                   /* byte, guid, temporal vectors */
            int fl = j_tok_flags(x->type);
            if (!fl) { j_nyi(b, x->type); break; }
            for (int64_t i = 0; i < n; i++) {
                if (i) jbuf_putc(b, ',');
                ray_t* ia = ray_i64(i);
                ray_t* c  = ray_at_fn(x, ia);
                ray_release(ia);
                j_tok(b, c, fl);
                ray_release(c);
            }
            break;
        }
    }
    if (!b->nyi) jbuf_putc(b, ']');
}

static void j_emit(jbuf* b, ray_t* x) {
    if (b->oom || b->nyi) return;
    if (!x) { j_nyi(b, 0); return; }
    if (x->type < 0) { j_atom(b, x); return; }
    if (x->type == RAY_CHARV) {                      /* char vector -> ONE string */
        jbuf_str(b, (const char*)ray_data(x), (size_t)ray_len(x));
        return;
    }
    switch (x->type) {
        case RAY_LIST: {
            int64_t n = ray_len(x);
            ray_t** e = (ray_t**)ray_data(x);
            jbuf_putc(b, '[');
            for (int64_t i = 0; i < n; i++) { if (i) jbuf_putc(b, ','); j_emit(b, e[i]); }
            jbuf_putc(b, ']');
            break;
        }
        case RAY_TABLE: j_table(b, x); break;
        case RAY_DICT:  j_dict(b, x);  break;
        default:
            if (ray_is_vec(x)) j_vec(b, x);
            else               j_nyi(b, x->type);
            break;
    }
}

ray_t* q_json_serialize(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    jbuf b = {0};
    j_emit(&b, x);
    if (b.oom) { free(b.p); return q_err(QE_WSFULL); }
    if (b.nyi) {
        free(b.p);
        return q_err(QE_NYI);
    }
    ray_t* r = ray_charv(b.p ? b.p : "", (int64_t)b.len);
    free(b.p);
    return r;
}

/* ======================================================================== *
 *  the written form — the lexical classification jk_node used to throw away
 * ======================================================================== */

/* The sniff lattice.  BOOL does NOT promote into the numbers: JSON states its
 * types, so `true` beside `1` is a genuine mix, not a text ambiguity (DuckDB
 * infers `json` for that column too).  JC_NONE = nothing but `null` observed. */
typedef enum { JC_NONE = 0, JC_BOOL, JC_I64, JC_F64, JC_NATIVE } jc_t;

/* A uint past int64 has no q integer, so it reads as a float. */
static jc_t jr_kind(yyjson_val* v) {
    switch (yyjson_get_type(v)) {
        case YYJSON_TYPE_NULL: return JC_NONE;
        case YYJSON_TYPE_BOOL: return JC_BOOL;
        case YYJSON_TYPE_NUM:
            if (yyjson_get_subtype(v) == YYJSON_SUBTYPE_REAL) return JC_F64;
            return (yyjson_get_subtype(v) == YYJSON_SUBTYPE_UINT &&
                    yyjson_get_uint(v) > (uint64_t)INT64_MAX) ? JC_F64 : JC_I64;
        default: return JC_NATIVE;                   /* string, object, array */
    }
}

static double jr_num(yyjson_val* v) {
    if (yyjson_get_subtype(v) == YYJSON_SUBTYPE_REAL) return yyjson_get_real(v);
    if (yyjson_get_subtype(v) == YYJSON_SUBTYPE_UINT) return (double)yyjson_get_uint(v);
    return (double)yyjson_get_sint(v);
}

/* ======================================================================== *
 *  .j.k — deserialize (yyjson node -> ray_t)
 * ======================================================================== */

/* ONE walker, two number laws: `written` reads the lexical subtype yyjson kept
 * (`1` a long, `1.0` a float) — `.j.read`'s law, at every depth.  `.j.k` passes 0
 * and stays kdb's documented lossy converter. */
static ray_t* j_node(yyjson_val* v, int written) {
    switch (yyjson_get_type(v)) {
        case YYJSON_TYPE_NULL:
            return ray_f64(NULL_F64);                    /* -> 0n */
        case YYJSON_TYPE_BOOL:
            return ray_bool(yyjson_get_bool(v));
        case YYJSON_TYPE_NUM: {
            if (written)
                return jr_kind(v) == JC_I64 ? ray_i64(yyjson_get_sint(v)) : ray_f64(jr_num(v));
            double d = yyjson_get_num(v);
            return isnan(d) ? ray_f64(NULL_F64) : ray_f64(d);  /* inf lives; nan -> 0n */
        }
        case YYJSON_TYPE_STR:
            return ray_charv(yyjson_get_str(v), (int64_t)yyjson_get_len(v));
        case YYJSON_TYPE_ARR: {
            size_t idx, max;
            yyjson_val* e;
            int64_t n = (int64_t)yyjson_arr_size(v);
            ray_t* lst = ray_list_new(n > 0 ? n : 1);
            if (RAY_IS_ERR(lst)) return lst;
            yyjson_arr_foreach(v, idx, max, e) {
                ray_t* c = j_node(e, written);
                if (RAY_IS_ERR(c)) { ray_release(lst); return c; }
                lst = ray_list_append(lst, c);           /* RETAINS c */
                ray_release(c);                           /* drop local ref */
                if (RAY_IS_ERR(lst)) return lst;
            }
            ray_t* col = q_list_collapse(lst);            /* homogeneous -> typed vector */
            ray_release(lst);
            return col;
        }
        case YYJSON_TYPE_OBJ: {
            size_t idx, max;
            yyjson_val *key, *val;
            int64_t n = (int64_t)yyjson_obj_size(v);
            ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
            if (RAY_IS_ERR(keys)) return keys;
            ray_t* vals = ray_list_new(n > 0 ? n : 1);
            if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
            yyjson_obj_foreach(v, idx, max, key, val) {
                int64_t id = ray_sym_intern_runtime(yyjson_get_str(key), yyjson_get_len(key));
                keys = ray_vec_append(keys, &id);
                if (RAY_IS_ERR(keys)) { ray_release(vals); return keys; }
                ray_t* cv = j_node(val, written);
                if (RAY_IS_ERR(cv)) { ray_release(keys); ray_release(vals); return cv; }
                vals = ray_list_append(vals, cv);          /* RETAINS cv */
                ray_release(cv);
                if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
            }
            ray_t* cvals = q_list_collapse(vals);
            ray_release(vals);
            if (RAY_IS_ERR(cvals)) { ray_release(keys); return cvals; }
            return ray_dict_new(keys, cvals);              /* consumes keys + cvals */
        }
        default:
            return q_err(QE_PARSE);
    }
}

ray_t* q_json_deserialize(ray_t* x) {
    const char* sp; int64_t sn;
    if (!q_str_text_bytes(x, &sp, &sn)) return q_err(QE_TYPE);
    size_t n = (size_t)sn;
    char* buf = malloc(n + 1);
    if (!buf) return q_err(QE_WSFULL);
    if (n) memcpy(buf, sp, n);
    buf[n] = '\0';
    yyjson_doc* doc = yyjson_read_opts(buf, n, YYJSON_READ_ALLOW_INF_AND_NAN, NULL, NULL);
    if (!doc) {
        ray_t* e = q_err(QE_PARSE);
        free(buf);
        return e;
    }
    ray_t* r = j_node(yyjson_doc_get_root(doc), 0);
    yyjson_doc_free(doc);
    free(buf);
    return r;
}

/* ======================================================================== *
 *  .j.read / .j.info — the reader.  WHY it is not .j.k: docs/superpowers/adr/0006.
 * ======================================================================== */

#define J_DEF_SAMPLE 20480            /* sniff RECORDS, the .csv.read default */

typedef struct {
    int64_t      sample;
    int64_t*     names;
    char*        chars;               /* the sniffed char per column — what .j.info reports */
    char*        want;                /* the caller's types: 0 none, ' ' drop, '*' native, else a q type char */
    int64_t      ncols;
    yyjson_val** recs;
    int64_t      nrecs;
    int          bare;                /* the array-of-non-records form: ONE column, DuckDB's `json` */
} jr_st;

static void jr_free(jr_st* st) {
    free(st->names);
    free(st->chars);
    free(st->want);
    free(st->recs);
}

static jc_t jr_promote(jc_t a, jc_t b) {
    if (a == JC_NONE) return b;
    if (b == JC_NONE || a == b) return a;
    if (a >= JC_I64 && a <= JC_F64 && b >= JC_I64 && b <= JC_F64) return a > b ? a : b;
    return JC_NATIVE;
}

/* JC_NONE — a column of nothing but `null` — is a float column of 0n: `.j.k`'s
 * own mapping for `null`, kept where inference has nothing to say. */
static char jr_char(jc_t k) {
    switch (k) {
        case JC_BOOL:   return 'b';
        case JC_I64:    return 'j';
        case JC_NATIVE: return '*';
        default:        return 'f';
    }
}

static int64_t jr_find(const int64_t* names, int64_t n, int64_t nm) {
    for (int64_t j = 0; j < n; j++)
        if (names[j] == nm) return j;
    return -1;
}

static int64_t jr_key_sym(yyjson_val* key) {
    return ray_sym_intern_runtime(yyjson_get_str(key), yyjson_get_len(key));
}

/* ---- source ---------------------------------------------------------------- */

/* THE SOURCE LAW: a SYMBOL is a resource, TEXT is content.  yyjson
 * parses a buffer, so the document is materialized whichever way it arrives —
 * and a leading UTF-8 BOM, which yyjson refuses, is stripped here. */
static ray_t* jr_bytes(ray_t* src, char** out, int64_t* outn) {
    const char* p = NULL;                            /* set by the two contiguous forms; the list form
                                                      * writes *out itself, having nothing to point at */
    ray_t* held = NULL;
    int64_t n = 0;
    *out = NULL;
    *outn = 0;
    if (!src) return q_err(QE_TYPE);
    if (src->type == -RAY_SYM) {
        ray_t* path = q_io_file_path(src);
        if (!path) return q_err(QE_TYPE);
        held = q_io_resource_read(path, 0, -1);
        ray_release(path);
        if (!held) return q_err(QE_OOM);
        if (RAY_IS_ERR(held)) return held;
        n = ray_len(held);
        p = n ? (const char*)ray_data(held) : "";
    } else if (!q_str_text_bytes(src, &p, &n)) {
        if (src->type != RAY_LIST) return q_err(QE_TYPE);
        int64_t cnt = ray_len(src);
        ray_t** e = (ray_t**)ray_data(src);
        for (int64_t i = 0; i < cnt; i++) {
            const char* ep;
            int64_t el;
            if (!q_str_text_bytes(e[i], &ep, &el)) return q_err(QE_TYPE);
            n += el + (i ? 1 : 0);
        }
        char* b = (char*)malloc((size_t)n + 1);
        if (!b) return q_err(QE_WSFULL);
        int64_t at = 0;
        for (int64_t i = 0; i < cnt; i++) {
            const char* ep;
            int64_t el;
            q_str_text_bytes(e[i], &ep, &el);
            if (i) b[at++] = '\n';                   /* read0's list form joins, never one record per line */
            if (el) memcpy(b + at, ep, (size_t)el);
            at += el;
        }
        b[n] = 0;
        *out = b;
        p = NULL;
    }
    if (p) {
        *out = (char*)malloc((size_t)n + 1);
        if (*out) {
            if (n) memcpy(*out, p, (size_t)n);
            (*out)[n] = 0;
        }
    }
    if (held) ray_release(held);
    if (!*out) return q_err(QE_WSFULL);
    if (n >= 3 && !memcmp(*out, "\xef\xbb\xbf", 3)) {
        memmove(*out, *out + 3, (size_t)(n - 3) + 1);
        n -= 3;
    }
    *outn = n;
    return NULL;
}

/* ---- shape + schema -------------------------------------------------------- */

/* THE TOP-LEVEL LAW: an object is one record, an array of objects is
 * the records, any other array is DuckDB's single `json` column.  Nothing else is
 * a table shape. */
static ray_t* jr_records(jr_st* st, yyjson_val* root) {
    if (yyjson_is_obj(root)) {
        st->recs = (yyjson_val**)malloc(sizeof *st->recs);
        if (!st->recs) return q_err(QE_WSFULL);
        st->recs[0] = root;
        st->nrecs = 1;
        return NULL;
    }
    if (!yyjson_is_arr(root)) return q_err(QE_TYPE);
    int64_t n = (int64_t)yyjson_arr_size(root);
    st->recs = (yyjson_val**)malloc((size_t)(n > 0 ? n : 1) * sizeof *st->recs);
    if (!st->recs) return q_err(QE_WSFULL);
    st->bare = n == 0;
    size_t idx, max;
    yyjson_val* e;
    yyjson_arr_foreach(root, idx, max, e) {
        if (!yyjson_is_obj(e)) st->bare = 1;
        st->recs[st->nrecs++] = e;
    }
    return NULL;
}

/* THE STAGE-1 KEY-SET LAW, one home for both passes: every record carries the SAME
 * names as the schema, each exactly once.  .j.info runs it over its sample too, so a
 * sniff never answers for a document the read would refuse. */
static ray_t* jr_check_keys(const jr_st* st, yyjson_val* rec, char* seen) {
    size_t idx, max;
    yyjson_val *key, *val;
    memset(seen, 0, (size_t)st->ncols);
    yyjson_obj_foreach(rec, idx, max, key, val) {
        int64_t j = jr_find(st->names, st->ncols, jr_key_sym(key));
        if (j < 0) return q_err(QE_TYPE);
        if (seen[j]) return q_err(QE_DUP);
        seen[j] = 1;
    }
    for (int64_t j = 0; j < st->ncols; j++)
        if (!seen[j]) return q_err(QE_TYPE);
    return NULL;
}

static ray_t* jr_alloc_cols(jr_st* st, int64_t n) {
    st->names = (int64_t*)malloc((size_t)n * sizeof *st->names);
    st->chars = (char*)malloc((size_t)n);
    st->want = (char*)calloc((size_t)n, 1);
    if (!st->names || !st->chars || !st->want) return q_err(QE_WSFULL);
    st->ncols = n;
    return NULL;
}

/* Record 0 names the columns, in its own key order; every later record matches
 * by NAME, so key order is free.  The schema FREEZES after `sample` records. */
static ray_t* jr_schema(jr_st* st) {
    int64_t lim = st->nrecs < st->sample ? st->nrecs : st->sample;
    size_t idx, max;
    yyjson_val *key, *val;
    if (st->bare) {
        ray_t* bad = jr_alloc_cols(st, 1);
        if (bad) return bad;
        st->names[0] = ray_sym_intern("json", 4);
        jc_t k = JC_NONE;
        for (int64_t i = 0; i < lim; i++) k = jr_promote(k, jr_kind(st->recs[i]));
        st->chars[0] = jr_char(k);
        return NULL;
    }
    int64_t nc = (int64_t)yyjson_obj_size(st->recs[0]);
    if (!nc) return q_err(QE_TYPE);                  /* a record with no keys states no schema */
    ray_t* bad = jr_alloc_cols(st, nc);
    if (bad) return bad;
    int64_t j = 0;
    yyjson_obj_foreach(st->recs[0], idx, max, key, val) {
        int64_t nm = jr_key_sym(key);
        if (jr_find(st->names, j, nm) >= 0) return q_err(QE_DUP);
        st->names[j++] = nm;
    }
    jc_t* k = (jc_t*)calloc((size_t)nc, sizeof *k);
    char* seen = (char*)malloc((size_t)nc);
    ray_t* err = (k && seen) ? NULL : q_err(QE_WSFULL);
    for (int64_t i = 0; !err && i < lim; i++) {
        if ((err = jr_check_keys(st, st->recs[i], seen))) break;
        yyjson_obj_foreach(st->recs[i], idx, max, key, val) {
            int64_t c = jr_find(st->names, nc, jr_key_sym(key));
            k[c] = jr_promote(k[c], jr_kind(val));
        }
    }
    for (int64_t c = 0; !err && c < nc; c++) st->chars[c] = jr_char(k[c]);
    free(k);
    free(seen);
    return err;
}

/* ---- the caller's types ---------------------------------------------------- */

static char jr_type_canon(char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c == '*' || c == ' ') return c;
    return q_type_of_char(c) ? c : 0;
}

static ray_t* jr_types(jr_st* st, ray_t* ty) {
    const char* tp;
    int64_t tn;
    if (!ty) return NULL;
    if (q_str_text_bytes(ty, &tp, &tn)) {
        if (tn != st->ncols) return q_err(QE_LENGTH);
        for (int64_t j = 0; j < tn; j++)
            if (!(st->want[j] = jr_type_canon(tp[j]))) return q_err(QE_TYPE);
        return NULL;
    }
    if (ty->type != RAY_DICT) return q_err(QE_TYPE);
    ray_t* ks = ray_dict_keys(ty);
    ray_t* vs = ray_dict_vals(ty);
    int64_t n = ks ? ray_len(ks) : 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* k = ray_at_fn(ks, ia);
        ray_t* v = ray_at_fn(vs, ia);
        ray_release(ia);
        ray_t* bad = NULL;
        if (!k || !v || RAY_IS_ERR(k) || RAY_IS_ERR(v)) bad = q_err(QE_OOM);
        else if (k->type != -RAY_SYM || v->type != -RAY_CHARV) bad = q_err(QE_TYPE);
        else {
            char c = jr_type_canon((char)v->u8);
            int64_t j = jr_find(st->names, st->ncols, k->i64);
            if (!c) bad = q_err(QE_TYPE);
            else if (j < 0) bad = q_err(QE_DOMAIN);
            else st->want[j] = c;
        }
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
        if (bad) return bad;
    }
    return NULL;
}

/* ---- build ----------------------------------------------------------------- */

/* One cell under its FROZEN char.  A post-sample value the char cannot hold is
 * 'type — never a silent null, the same posture .csv.read takes. */
static ray_t* jr_cell(char c, yyjson_val* v) {
    int nul = yyjson_is_null(v);
    switch (c) {
        case 'b':
            if (nul) return ray_bool(0);             /* q booleans have no null */
            if (!yyjson_is_bool(v)) return q_err(QE_TYPE);
            return ray_bool(yyjson_get_bool(v));
        case 'j':
            if (nul) return ray_i64(NULL_I64);
            if (jr_kind(v) != JC_I64) return q_err(QE_TYPE);
            return ray_i64(yyjson_get_sint(v));
        case 'f': {
            if (nul) return ray_f64(NULL_F64);
            jc_t k = jr_kind(v);
            if (k != JC_I64 && k != JC_F64) return q_err(QE_TYPE);
            return ray_f64(jr_num(v));
        }
        default:
            return j_node(v, 1);
    }
}

/* A column the caller TYPED is built natively and converted afterwards: the sniff it
 * replaces must not still be able to refuse a cell on the way in — the freeze rule. */
static char jr_build_char(const jr_st* st, int64_t j) {
    char w = st->want[j];
    return (w && w != ' ') ? '*' : st->chars[j];   /* "*" says the same thing: do not coerce */
}

static ray_t* jr_place(const jr_st* st, ray_t** acc, int64_t j, yyjson_val* v) {
    ray_t* a = jr_cell(jr_build_char(st, j), v);
    if (!a) return q_err(QE_OOM);
    if (RAY_IS_ERR(a)) return a;
    ray_t* l = ray_list_append(acc[j], a);           /* RETAINS a */
    ray_release(a);
    if (RAY_IS_ERR(l)) { acc[j] = NULL; return l; }
    acc[j] = l;
    return NULL;
}

/* A column of JSON strings, collapsed (RAY_STR) or not (a list of char vectors). */
static int jr_col_is_text(ray_t* col) {
    if (col->type == RAY_STR) return 1;
    if (col->type != RAY_LIST || !ray_len(col)) return 0;
    ray_t** e = (ray_t**)ray_data(col);
    for (int64_t i = 0; i < ray_len(col); i++)
        if (!e[i] || e[i]->type != RAY_CHARV) return 0;
    return 1;
}

/* A type char PARSES text and CONVERTS everything else — so `d` on a JSON string
 * reads the date the string spells, while `f` on a long column widens it.  Both
 * halves are q_dollar's, the one conversion home. */
static ray_t* jr_retype(char c, ray_t* col) {
    int8_t tag = q_type_of_char(c);
    return jr_col_is_text(col) ? q_dollar_tok(tag, col) : q_dollar_cast(tag, col);
}

static ray_t* jr_assemble(const jr_st* st, ray_t** acc) {
    int64_t keep = 0;
    for (int64_t j = 0; j < st->ncols; j++)
        if (st->want[j] != ' ') keep++;
    if (!keep) return q_err(QE_TYPE);                /* every column dropped is not a table */
    ray_t* tbl = ray_table_new(keep);
    if (RAY_IS_ERR(tbl)) return tbl;
    for (int64_t j = 0; j < st->ncols; j++) {
        if (st->want[j] == ' ') continue;
        char bc = jr_build_char(st, j);              /* '*' is the string/nested COLUMN's list container */
        ray_t* col = ray_len(acc[j]) ? q_list_collapse(acc[j])
                                     : (bc == '*' ? ray_list_new(1) : q_type_empty(q_type_of_char(bc)));
        if (col && !RAY_IS_ERR(col) && st->want[j] && st->want[j] != '*') {
            ray_t* c2 = jr_retype(st->want[j], col);
            ray_release(col);
            col = c2;
        }
        if (!col || RAY_IS_ERR(col)) {
            ray_release(tbl);
            return col ? col : q_err(QE_OOM);
        }
        tbl = ray_table_add_col(tbl, st->names[j], col);
        ray_release(col);
        if (RAY_IS_ERR(tbl)) return tbl;
    }
    return tbl;
}

static ray_t* jr_build(const jr_st* st) {
    ray_t** acc = (ray_t**)calloc((size_t)st->ncols, sizeof *acc);
    char* seen = (char*)malloc((size_t)st->ncols);
    ray_t* bad = (acc && seen) ? NULL : q_err(QE_WSFULL);
    for (int64_t j = 0; !bad && j < st->ncols; j++) {
        acc[j] = ray_list_new(st->nrecs > 0 ? st->nrecs : 1);
        if (RAY_IS_ERR(acc[j])) { bad = acc[j]; acc[j] = NULL; }
    }
    size_t idx, max;
    yyjson_val *key, *val;
    for (int64_t i = 0; !bad && i < st->nrecs; i++) {
        if (st->bare) { bad = jr_place(st, acc, 0, st->recs[i]); continue; }
        if ((bad = jr_check_keys(st, st->recs[i], seen))) break;
        yyjson_obj_foreach(st->recs[i], idx, max, key, val)
            if ((bad = jr_place(st, acc, jr_find(st->names, st->ncols, jr_key_sym(key)), val))) break;
    }
    ray_t* r = bad ? bad : jr_assemble(st, acc);
    for (int64_t j = 0; j < st->ncols; j++)
        if (acc[j]) ray_release(acc[j]);
    free(acc);
    free(seen);
    return r;
}

static ray_t* jr_info(const jr_st* st) {
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, st->ncols);
    if (RAY_IS_ERR(keys)) return keys;
    for (int64_t j = 0; j < st->ncols; j++) {
        keys = ray_vec_append(keys, &st->names[j]);
        if (RAY_IS_ERR(keys)) return keys;
    }
    ray_t* vals = ray_charv(st->chars, st->ncols);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    return ray_dict_new(keys, vals);                 /* consumes both */
}

/* ---- options + the natives -------------------------------------------------- */

static int jr_opt_is(int64_t sym, const char* name) { return sym == ray_sym_intern(name, strlen(name)); }

static ray_t* jr_opts(jr_st* st, ray_t* opts) {
    if (!opts) return NULL;
    if (opts->type != RAY_DICT) return q_err(QE_TYPE);
    ray_t* ks = ray_dict_keys(opts);
    ray_t* vs = ray_dict_vals(opts);
    int64_t n = ks ? ray_len(ks) : 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* k = ray_at_fn(ks, ia);
        ray_t* v = ray_at_fn(vs, ia);
        ray_release(ia);
        ray_t* bad = NULL;
        if (!k || !v || RAY_IS_ERR(k) || RAY_IS_ERR(v)) bad = q_err(QE_OOM);
        else if (k->type != -RAY_SYM) bad = q_err(QE_TYPE);
        else if (jr_opt_is(k->i64, "")) {
            /* the empty-sym key is padding, value unread — the same short-dict
             * idiom lib/csv.q legalizes, without weakening the never-ignored law */
        } else if (jr_opt_is(k->i64, "sample_size")) {
            int64_t x;
            if (!q_type_strict_i64(v, &x)) bad = q_err(QE_TYPE);
            else if (x <= 0) bad = q_err(QE_DOMAIN);
            else st->sample = x;
        } else if (jr_opt_is(k->i64, "format")) {
            if (v->type != -RAY_SYM) bad = q_err(QE_TYPE);
            else if (!jr_opt_is(v->i64, "array")) bad = q_err(QE_NYI);
        } else {
            bad = q_err(QE_OPTION);                  /* an option is NEVER silently ignored */
        }
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
        if (bad) return bad;
    }
    return NULL;
}

static ray_t* jr_read(ray_t* src, ray_t* types, ray_t* opts, int info_only) {
    jr_st st = { .sample = J_DEF_SAMPLE };
    char* buf = NULL;
    int64_t n = 0;
    yyjson_doc* doc = NULL;
    ray_t* bad = jr_opts(&st, opts);
    if (!bad) bad = jr_bytes(src, &buf, &n);
    if (!bad && !(doc = yyjson_read_opts(buf, (size_t)n, YYJSON_READ_ALLOW_INF_AND_NAN, NULL, NULL)))
        bad = q_err(QE_PARSE);
    if (!bad) bad = jr_records(&st, yyjson_doc_get_root(doc));
    if (!bad) bad = jr_schema(&st);
    if (!bad && !info_only) bad = jr_types(&st, types);
    ray_t* r = bad ? bad : (info_only ? jr_info(&st) : jr_build(&st));
    if (doc) yyjson_doc_free(doc);
    free(buf);
    jr_free(&st);
    return r;
}

static ray_t* jr_arg(ray_t* x) {
    return (x && x->type != RAY_NULL) ? x : NULL;    /* :: and elided read as default */
}

/* .j.i.read[source;target;types;opts] */
static ray_t* j_read_fn(ray_t** args, int64_t n) {
    if (n != 4) return q_err(QE_RANK);
    if (jr_arg(args[1])) return q_err(QE_NYI);       /* the rank is reserved; a target is refused, never ignored */
    return jr_read(args[0], jr_arg(args[2]), jr_arg(args[3]), 0);
}

/* .j.i.info[source;opts] */
static ray_t* j_info_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    return jr_read(args[0], NULL, jr_arg(args[1]), 1);
}

static void jr_bind_fn(const char* name, ray_vary_fn fn) {
    ray_t* f = ray_fn_vary(name, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(name, strlen(name)), f);
    ray_release(f);
}

void q_json_register(void) {
    jr_bind_fn(".j.i.read", j_read_fn);
    jr_bind_fn(".j.i.info", j_info_fn);
}
