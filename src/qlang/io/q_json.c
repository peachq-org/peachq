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
#include "qlang/io/q_json.h"
#include "qlang/q_prim.h"
#include "qlang/base/q_err.h"
#include "qlang/q_fmt.h"      /* q_fmt / q_fmt_float — the display + `\P` home */
#include "qlang/base/q_type.h"     /* q_type_is_inf — the infinity lane */
#include "lang/eval.h"        /* RAY_FN_NONE, ray_at_fn — dict/table cell reads */
#include "qlang/ops/q_bang.h" /* q_bang_enkey — the unkey primitive */
#include "qlang/ops/q_dollar.h"  /* q_dollar_cast — the one conversion home */
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

/* The native-number classifier: which q number a JSON number wrote. */
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

enum { JF_AUTO = 0, JF_ARRAY, JF_ND };               /* the `format` option: how records are FRAMED */
enum { JR_REC_AUTO = -1, JR_REC_NEVER = 0, JR_REC_ALWAYS = 1 };

typedef struct { int key; int64_t v; } jr_step;      /* a SYMBOL indexes an object, a LONG an array */

typedef struct {
    int64_t      sample;
    jr_step*     path;                /* the `path` option, walked over each document's root */
    int64_t      npath;
    int64_t*     names;
    int64_t      namecap;
    char*        chars;               /* the sniffed char per column — what .j.info reports */
    char*        want;                /* the caller's types: 0 none, ' ' drop, '*' native, else a q type char */
    int64_t      ncols;
    yyjson_val** recs;
    int64_t*     lines;               /* per record: physical line under nd, 1-based ordinal under array */
    int64_t      nrecs, reccap;
    yyjson_doc** docs;                /* array framing holds one; the nd stream one per value */
    int64_t      ndocs, doccap;
    int          format;
    int          records;
    int          ignore_errors;
    int          bare;                /* the non-record form: ONE column, DuckDB's `json` */
    q_csv_fmt_t  fmt;                 /* dateformat / timestampformat — legal here too (one cell home) */
    int          store_rej, null_pad; /* the reject channel levers, .csv.read's vocabulary */
    int64_t      rej_name;            /* rejects_table sym; 0 = the reject_errors default */
    ray_t*       rj[4];               /* reject accumulators: line / column / error / record */
    int64_t      rej_total;
    const char*  rej_class;           /* set at the failure site; non-NULL = the fault is rejectable */
    int64_t      rej_col;             /* the failing column's sym; 0 = a record-level fault */
} jr_st;

static void jr_free(jr_st* st) {
    for (int64_t i = 0; i < st->ndocs; i++) yyjson_doc_free(st->docs[i]);
    free(st->docs);
    free(st->path);
    free(st->names);
    free(st->chars);
    free(st->want);
    free(st->recs);
    free(st->lines);
    for (int i = 0; i < 4; i++)
        if (st->rj[i]) ray_release(st->rj[i]);
}

/* one growable-pointer-vector push, for the two stream vectors */
static int jr_push(void*** vec, int64_t* n, int64_t* cap, void* item) {
    if (*n == *cap) {
        int64_t c = *cap ? *cap * 2 : 16;
        void** g = (void**)realloc(*vec, (size_t)c * sizeof *g);
        if (!g) return 0;
        *vec = g;
        *cap = c;
    }
    (*vec)[(*n)++] = item;
    return 1;
}

static int jr_hold(jr_st* st, yyjson_doc* d) { return jr_push((void***)&st->docs, &st->ndocs, &st->doccap, d); }

static int jr_rec(jr_st* st, yyjson_val* v, int64_t line) {
    if (!jr_push((void***)&st->recs, &st->nrecs, &st->reccap, v)) return 0;
    int64_t* nl = (int64_t*)realloc(st->lines, (size_t)st->reccap * sizeof *nl);
    if (!nl) return 0;
    st->lines = nl;
    nl[st->nrecs - 1] = line;
    return 1;
}

/* Per-column sniff state: the SHARED ct_t lattice plus a NATIVE flag for the
 * shapes only JSON has (object / array — and the bool-beside-number mix). */
typedef struct { ct_t ct; char native; } jr_ct;

/* S3, the one-cell-home ruling (R1): a native JSON kind maps into the shared
 * ct_t, and a STRING goes through q_csv_detect — but the verdict is kept ONLY
 * when it names a vocabulary JSON cannot state natively (the temporals, guid, byte).  A
 * CT_I64/CT_F64/CT_BOOL verdict on a QUOTED value is discarded and the cell
 * stays text: the file said it was a string, and .j.j quotes exactly so. */
static ct_t jr_detect(const jr_st* st, yyjson_val* v, int* native) {
    *native = 0;
    switch (yyjson_get_type(v)) {
        case YYJSON_TYPE_NULL: return CT_UNKNOWN;
        case YYJSON_TYPE_BOOL: return CT_BOOL;
        case YYJSON_TYPE_NUM:  return jr_kind(v) == JC_F64 ? CT_F64 : CT_I64;
        case YYJSON_TYPE_STR: {
            ct_t c = q_csv_detect(&st->fmt, yyjson_get_str(v), yyjson_get_len(v));
            return (c >= CT_DATE && c <= CT_BYTE) ? c : CT_STR;
        }
        default: *native = 1; return CT_UNKNOWN;     /* object / array */
    }
}

static void jr_note(jr_ct* s, ct_t c, int native) {
    if (native) { s->native = 1; return; }
    if (c == CT_UNKNOWN) return;                     /* nulls take no part in typing */
    /* JSON STATED both types, so a boolean beside a number is a genuine mix that
     * keeps both as written — the one row where CSV's text lattice cannot speak */
    if ((s->ct == CT_BOOL && (c == CT_I64 || c == CT_F64)) ||
        (c == CT_BOOL && (s->ct == CT_I64 || s->ct == CT_F64))) { s->native = 1; return; }
    s->ct = q_csv_promote(s->ct, c);
}

/* CT_UNKNOWN — nothing but `null` observed — is a float column of 0n: `.j.k`'s
 * own mapping for `null`, kept where inference has nothing to say. */
static char jr_char(const jr_ct* s) {
    if (s->native) return '*';
    if (s->ct == CT_UNKNOWN) return 'f';
    return q_csv_resolve(s->ct);
}

static int64_t jr_find(const int64_t* names, int64_t n, int64_t nm) {
    for (int64_t j = 0; j < n; j++)
        if (names[j] == nm) return j;
    return -1;
}

static int64_t jr_key_sym(yyjson_val* key) {
    return ray_sym_intern_runtime(yyjson_get_str(key), yyjson_get_len(key));
}

/* ---- path ------------------------------------------------------------------ */

static ray_t* jr_path_step(ray_t* a, jr_step* out) {
    if (a->type == -RAY_SYM) { out->key = 1; out->v = a->i64; return NULL; }
    if (a->type == -RAY_I64) { out->key = 0; out->v = a->i64; return NULL; }
    return q_err(QE_TYPE);
}

/* THE PATH is a q PATH VECTOR — `d . path` spelled over the document: symbols index objects, longs
 * index arrays, and an atom is the one-step spelling.  An empty vector selects nothing to select. */
static ray_t* jr_path_set(jr_st* st, ray_t* v) {
    if (v->type == -RAY_SYM || v->type == -RAY_I64) {
        if (!(st->path = (jr_step*)malloc(sizeof *st->path))) return q_err(QE_WSFULL);
        st->npath = 1;
        return jr_path_step(v, st->path);
    }
    if (v->type != RAY_SYM && v->type != RAY_I64 && v->type != RAY_LIST) return q_err(QE_TYPE);
    int64_t n = ray_len(v);
    if (!n) return NULL;
    if (!(st->path = (jr_step*)malloc((size_t)n * sizeof *st->path))) return q_err(QE_WSFULL);
    st->npath = n;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(v, ia);
        ray_release(ia);
        if (!e) return q_err(QE_OOM);
        if (RAY_IS_ERR(e)) return e;
        ray_t* bad = jr_path_step(e, &st->path[i]);
        ray_release(e);
        if (bad) return bad;
    }
    return NULL;
}

/* THE ORDERING IS THE FEATURE: the path selects from the root of EACH document BEFORE inference, so the
 * selected value BECOMES the top-level one and every other rule — the root-kind law, the key union, the
 * sniff, types — speaks about it unchanged.  A step this document cannot take is 'domain: a typo must never
 * read as "no schema" and resurface one step later as 'type from the shape law.  A step of the wrong KIND is
 * 'type, which is where jq's array-index syntax would have gone and deliberately does not. */
static ray_t* jr_select(const jr_st* st, yyjson_val** v) {
    for (int64_t i = 0; i < st->npath; i++) {
        yyjson_val* cur = *v;
        if (!st->path[i].key) {
            if (!yyjson_is_arr(cur)) return q_err(QE_TYPE);
            if (st->path[i].v < 0 || (size_t)st->path[i].v >= yyjson_arr_size(cur)) return q_err(QE_DOMAIN);
            *v = yyjson_arr_get(cur, (size_t)st->path[i].v);
            continue;
        }
        if (!yyjson_is_obj(cur)) return q_err(QE_TYPE);
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_val* hit = NULL;
        yyjson_obj_foreach(cur, idx, max, key, val)
            if (jr_key_sym(key) == st->path[i].v) { hit = val; break; }
        if (!hit) return q_err(QE_DOMAIN);
        *v = hit;
    }
    return NULL;
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

/* ---- the reject channel (R2): .csv.read's channel, restated for records ----- */

/* The audit row a bad record leaves: line / column / error class / raw record.
 * MIRRORS csv_reject_note rather than sharing it — the two accumulate different
 * reader states, and the raw column differs in kind (a physical csvLine there,
 * a re-serialized record here) — the R10 same-outcome-different-code case. */
static ray_t* jr_reject_note(jr_st* st, int64_t line, int64_t col, const char* cls,
                             const char* p, size_t n) {
    for (int i = 0; i < 4; i++)
        if (!st->rj[i]) {
            st->rj[i] = ray_list_new(8);
            if (RAY_IS_ERR(st->rj[i])) {
                ray_t* e = st->rj[i];
                st->rj[i] = NULL;
                return e;
            }
        }
    ray_t* vs[4] = { ray_i64(line), ray_sym(col ? col : ray_sym_intern_runtime("", 0)),
                     ray_sym(ray_sym_intern_runtime(cls, strlen(cls))), ray_charv(p, (int64_t)n) };
    ray_t* bad = NULL;
    for (int i = 0; i < 4; i++) {
        if (!vs[i] || RAY_IS_ERR(vs[i])) {
            if (!bad) bad = vs[i] ? vs[i] : q_err(QE_OOM);
            else if (vs[i]) ray_release(vs[i]);      /* a SECOND failure is still an owned value */
            continue;
        }
        if (!bad) {
            st->rj[i] = ray_list_append(st->rj[i], vs[i]);
            if (RAY_IS_ERR(st->rj[i])) {
                bad = st->rj[i];
                st->rj[i] = NULL;
            }
        }
        ray_release(vs[i]);
    }
    if (bad) return bad;
    st->rej_total++;
    return NULL;
}

/* the record's own bytes are not retained past the parse, so the audit column
 * carries the value re-serialized compact — same value, one spelling */
static ray_t* jr_reject_val(jr_st* st, int64_t line, int64_t col, const char* cls, yyjson_val* rec) {
    size_t tn = 0;
    char* txt = yyjson_val_write(rec, 0, &tn);
    if (!txt) return q_err(QE_WSFULL);               /* an empty audit cell would falsify the record */
    ray_t* bad = jr_reject_note(st, line, col, cls, txt, tn);
    free(txt);
    return bad;
}

/* the reject records as an OWNED table (empty = the schema).  MIRRORS
 * csv_rejects_tbl rather than sharing it, for jr_reject_note's reason: different
 * reader state (csv batches from an index; one load stores whole here), and the
 * raw column is a re-serialized `record`, not a physical csvLine. */
static ray_t* jr_rejects_tbl(jr_st* st) {
    static const char* const cn[4] = { "line", "column", "error", "record" };
    static const char ct[4] = { 'j', 's', 's', '*' };
    ray_t* tbl = ray_table_new(4);
    if (RAY_IS_ERR(tbl)) return tbl;
    for (int c = 0; c < 4; c++) {
        ray_t* col;
        if (!st->rj[c] || !ray_len(st->rj[c]))
            col = ct[c] == '*' ? ray_list_new(1) : q_type_empty(q_type_of_char(ct[c]));
        else
            col = q_list_collapse(st->rj[c]);
        if (!col || RAY_IS_ERR(col)) {
            ray_release(tbl);
            return col ? col : q_err(QE_OOM);
        }
        tbl = ray_table_add_col(tbl, ray_sym_intern_runtime(cn[c], strlen(cn[c])), col);
        ray_release(col);
        if (RAY_IS_ERR(tbl)) return tbl;
    }
    return tbl;
}

/* ---- shape + schema -------------------------------------------------------- */

/* THE TOP-LEVEL LAW: an object is one record, an array of objects is
 * the records, any other array is DuckDB's single `json` column.  Nothing else is
 * a table shape.  It is a law about the ROOT KIND and says nothing about framing.
 * Array framing has no physical lines to report, so a reject's `line` is the
 * record's 1-based ordinal there; under nd it is the real line. */
static ray_t* jr_records(jr_st* st, yyjson_val* root) {
    if (yyjson_is_obj(root)) return jr_rec(st, root, 1) ? NULL : q_err(QE_WSFULL);
    if (!yyjson_is_arr(root)) return q_err(QE_TYPE);
    size_t idx, max;
    yyjson_val* e;
    yyjson_arr_foreach(root, idx, max, e)
        if (!jr_rec(st, e, (int64_t)idx + 1)) return q_err(QE_WSFULL);
    return NULL;
}

/* `newline_delimited`: a stream of whole JSON values.  Two rules bound it, and both are what
 * separate it from the array framing rather than decoration: no value may span a LINE BREAK - CR, LF or
 * CRLF alike, so the framing never depends on which convention wrote the file - and
 * no top-level value may be an ARRAY - an array IS a frame, so one at the top of an nd stream is
 * the array framing mis-stated, which is the refusal `format:`newline_delimited`` owes an array. */
static ray_t* jr_frame_nd(jr_st* st, char* buf, int64_t n) {
    int64_t at = 0, line = 1;
    int tol = st->ignore_errors || st->store_rej;
    while (at < n) {
        while (at < n && (unsigned char)buf[at] <= ' ') {
            line += buf[at] == '\n' || (buf[at] == '\r' && (at + 1 >= n || buf[at + 1] != '\n'));
            at++;
        }
        if (at >= n) break;
        yyjson_doc* doc = yyjson_read_opts(buf + at, (size_t)(n - at),
                                           YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_STOP_WHEN_DONE,
                                           NULL, NULL);
        size_t used = doc ? yyjson_doc_get_read_size(doc) : 0;
        while (used && (unsigned char)buf[at + (int64_t)used - 1] <= ' ') used--;
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : NULL;
        if (!doc || yyjson_is_arr(root) || memchr(buf + at, '\n', used) || memchr(buf + at, '\r', used)) {
            if (doc) yyjson_doc_free(doc);
            if (!tol) return q_err(QE_PARSE);
            /* a continue-mode lever (R2): the bad LINE leaves its reject record — raw
             * bytes, since there is no value to re-serialize — and the stream resumes
             * at the next line break, the only boundary this framing trusts */
            int64_t eol = at;
            while (eol < n && buf[eol] != '\n' && buf[eol] != '\r') eol++;
            ray_t* bad = jr_reject_note(st, line, 0, "parse", buf + at, (size_t)(eol - at));
            if (bad) return bad;
            at = eol;
            continue;
        }
        if (!jr_hold(st, doc)) { yyjson_doc_free(doc); return q_err(QE_WSFULL); }
        ray_t* bad = jr_select(st, &root);           /* per DOCUMENT, and under nd each line is one */
        if (bad) return bad;
        if (!jr_rec(st, root, line)) return q_err(QE_WSFULL);
        at += (int64_t)used;
    }
    /* A source carrying NO document at all is malformed, not empty.  `[]` states a shape and reads
     * as zero records; nothing states none, and we do not invent one - the same law .csv.read keeps
     * for a zero-byte file, where DuckDB invents a one-column schema and we refuse. */
    return st->nrecs ? NULL : q_err(QE_PARSE);
}

/* AUTO ASKS ONE QUESTION: is the whole file ONE document?  If it is, that is the array framing and
 * the root-kind law speaks; if it is not, the file is a stream.  A pretty-printed object is one
 * document, so auto reads it - which is why auto cannot be "does it start with [". */
static ray_t* jr_frame(jr_st* st, char* buf, int64_t n) {
    if (st->format == JF_ND) return jr_frame_nd(st, buf, n);
    yyjson_doc* doc = yyjson_read_opts(buf, (size_t)n, YYJSON_READ_ALLOW_INF_AND_NAN, NULL, NULL);
    if (!doc) return st->format == JF_ARRAY ? q_err(QE_PARSE) : jr_frame_nd(st, buf, n);
    if (!jr_hold(st, doc)) { yyjson_doc_free(doc); return q_err(QE_WSFULL); }
    yyjson_val* root = yyjson_doc_get_root(doc);
    ray_t* bad = jr_select(st, &root);
    return bad ? bad : jr_records(st, root);
}

/* records: `auto` reads objects as records and anything else as the one `json` column; 1b demands
 * records and refuses a non-object; 0b never expands, whatever the values are. */
static ray_t* jr_classify(jr_st* st) {
    int any_bare = st->nrecs == 0;
    for (int64_t i = 0; i < st->nrecs; i++)
        if (!yyjson_is_obj(st->recs[i])) any_bare = 1;
    if (st->records == JR_REC_NEVER) { st->bare = 1; return NULL; }
    if (st->records == JR_REC_ALWAYS) return any_bare ? q_err(QE_TYPE) : NULL;
    st->bare = any_bare;
    return NULL;
}

/* THE KEY-UNION LAW, one home for both passes: a record may OMIT a column (it null-fills),
 * but it may never repeat a key ('dup) nor carry one the frozen schema does not know ('type).
 * `seen` comes back marked, so the caller knows which columns this record left to fill.
 * .j.info runs it over its sample too, so a sniff never answers for a document the read refuses. */
static ray_t* jr_check_keys(jr_st* st, yyjson_val* rec, char* seen) {
    size_t idx, max;
    yyjson_val *key, *val;
    memset(seen, 0, (size_t)st->ncols);
    yyjson_obj_foreach(rec, idx, max, key, val) {
        int64_t nm = jr_key_sym(key);
        int64_t j = jr_find(st->names, st->ncols, nm);
        if (j < 0) {
            st->rej_class = "unknownkey";
            st->rej_col = nm;
            return q_err(QE_TYPE);
        }
        if (seen[j]) {
            st->rej_class = "dupkey";
            st->rej_col = nm;
            return q_err(QE_DUP);
        }
        seen[j] = 1;
    }
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

/* the union grows as the sample is walked, so the column vectors grow with it */
static int jr_add_col(jr_st* st, int64_t nm) {
    if (st->ncols == st->namecap) {
        int64_t c = st->namecap ? st->namecap * 2 : 16;
        int64_t* g = (int64_t*)realloc(st->names, (size_t)c * sizeof *g);
        if (!g) return -1;
        st->names = g;
        st->namecap = c;
    }
    st->names[st->ncols] = nm;
    return (int)st->ncols++;
}

/* THE UNION: every sampled record contributes its keys, in first-appearance order, and a record
 * that omits one is not an error - the column null-fills at build time.  Nulls take no part in
 * typing, so a column is typed by its non-null values wherever they sit.  The schema FREEZES
 * after `sample` records; a key first seen after that is the frozen-schema miss, 'type. */
/* in-record duplicate-key scan that touches NOTHING: a record the build will
 * reject must not add a column or evidence (csv_row_detect's no-poison law) */
static int jr_rec_dup(yyjson_val* rec) {
    size_t i1, m1, i2, m2;
    yyjson_val *k1, *v1, *k2, *v2;
    yyjson_obj_foreach(rec, i1, m1, k1, v1)
        yyjson_obj_foreach(rec, i2, m2, k2, v2) {
            if (i2 >= i1) break;
            if (yyjson_get_len(k1) == yyjson_get_len(k2) &&
                !memcmp(yyjson_get_str(k1), yyjson_get_str(k2), yyjson_get_len(k1))) return 1;
        }
    return 0;
}

static ray_t* jr_schema(jr_st* st) {
    int64_t lim = st->nrecs < st->sample ? st->nrecs : st->sample;
    size_t idx, max;
    yyjson_val *key, *val;
    int tol = st->ignore_errors || st->store_rej;
    if (st->bare) {
        ray_t* bad = jr_alloc_cols(st, 1);
        if (bad) return bad;
        st->names[0] = ray_sym_intern("json", 4);
        jr_ct k = { 0 };
        for (int64_t i = 0; i < lim; i++) {
            int native;
            ct_t c = jr_detect(st, st->recs[i], &native);
            jr_note(&k, c, native);
        }
        st->chars[0] = jr_char(&k);
        return NULL;
    }
    jr_ct* k = NULL;
    ray_t* err = NULL;
    for (int64_t i = 0; !err && i < lim; i++) {
        if (jr_rec_dup(st->recs[i])) {
            if (!tol) { err = q_err(QE_DUP); break; }
            continue;                                /* the build phase rejects it, with its record */
        }
        yyjson_obj_foreach(st->recs[i], idx, max, key, val) {
            int64_t nm = jr_key_sym(key);
            int64_t c = jr_find(st->names, st->ncols, nm);
            if (c < 0) {
                if ((c = jr_add_col(st, nm)) < 0) { err = q_err(QE_WSFULL); break; }
                jr_ct* gk = (jr_ct*)realloc(k, (size_t)st->ncols * sizeof *gk);
                if (!gk) { err = q_err(QE_WSFULL); break; }
                k = gk;
                k[c] = (jr_ct){ 0 };
            }
            int native;
            ct_t ct = jr_detect(st, val, &native);
            jr_note(&k[c], ct, native);
        }
    }
    if (!err && !st->ncols) err = q_err(QE_TYPE);    /* records with no keys state no schema */
    if (!err) {
        st->chars = (char*)malloc((size_t)st->ncols);
        st->want = (char*)calloc((size_t)st->ncols, 1);
        if (!st->chars || !st->want) err = q_err(QE_WSFULL);
    }
    for (int64_t c = 0; !err && c < st->ncols; c++) st->chars[c] = jr_char(&k[c]);
    free(k);
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

/* the shared cell parser signals 'csv; inside this reader that fit failure is
 * 'type — the class lib/j.q declares — so the spelling is remapped, not the law */
static ray_t* jr_cellerr(ray_t* e) {
    int64_t n;
    const char* t = q_err_text(e, &n);
    if (t && n == 3 && !memcmp(t, "csv", 3)) {
        ray_release(e);
        return q_err(QE_TYPE);
    }
    return e;
}

/* One cell under its FROZEN char.  A post-sample value the char cannot hold is
 * 'type — never a silent null, the same posture .csv.read takes.  The temporal
 * chars exist only for STRING cells (S3), and their meaning is the shared cell
 * parser's; a native value under one is the miss law. */
static ray_t* jr_cell(const jr_st* st, char c, yyjson_val* v) {
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
        case 'd': case 'm': case 'p': case 'u': case 'v': case 't': case 'n': case 'g': case 'x': {
            if (nul) return q_csv_cell_atom(&st->fmt, c, "", 0);   /* the typed null */
            if (!yyjson_is_str(v)) return q_err(QE_TYPE);
            ray_t* a = q_csv_cell_atom(&st->fmt, c, yyjson_get_str(v), yyjson_get_len(v));
            return (a && RAY_IS_ERR(a)) ? jr_cellerr(a) : a;
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

/* A KEY A RECORD OMITS IS A JSON `null`, so it writes exactly what a written null writes: the
 * column type's q null.  A boolean column's is 0b, because q booleans have no null, and a native
 * or all-null column's is 0n - .j.k's own mapping for `null`, which is where inference lands
 * when it has nothing to say. */
static ray_t* jr_null_cell(const jr_st* st, char c) {
    switch (c) {
        case 'b': return ray_bool(0);
        case 'j': return ray_i64(NULL_I64);
        case 'd': case 'm': case 'p': case 'u': case 'v': case 't': case 'n': case 'g': case 'x':
            return q_csv_cell_atom(&st->fmt, c, "", 0);
        default:  return ray_f64(NULL_F64);
    }
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
 * reads the date the string spells, while `f` on a long column widens it (a cast,
 * not a parse — q_dollar_cast stays that home).  The TEXT arm is the shared cell
 * parser (S4) for EVERY type char, so both loaders read one cell text into one
 * value, loud on a miss: q_csv_cell_atom answers the whole 18-type roster, which
 * is every char jr_type_canon admits, and its default arm refuses anything else. */
static ray_t* jr_retype(const jr_st* st, char c, ray_t* col) {
    if (!jr_col_is_text(col)) return q_dollar_cast(q_type_of_char(c), col);
    int64_t n = ray_len(col);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        const char* p;
        size_t pl;
        if (col->type == RAY_STR) {
            size_t sl = 0;
            p = ray_str_vec_get(col, i, &sl);
            pl = p ? sl : 0;
        } else {
            ray_t* e = ((ray_t**)ray_data(col))[i];
            p = (const char*)ray_data(e);
            pl = (size_t)ray_len(e);
        }
        ray_t* a = q_csv_cell_atom(&st->fmt, c, p ? p : "", pl);
        if (!a || RAY_IS_ERR(a)) {
            ray_release(out);
            return a ? jr_cellerr(a) : q_err(QE_OOM);
        }
        out = ray_list_append(out, a);               /* RETAINS a */
        ray_release(a);
        if (RAY_IS_ERR(out)) return out;
    }
    ray_t* r = q_list_collapse(out);
    ray_release(out);
    return r;
}

static ray_t* jr_nested(const jr_st* st, yyjson_val** vals, int64_t n);

/* one cell of a mixed or non-record nested column: an array of records recurses
 * (each element a row of a sub-table), anything else is the value as written */
static ray_t* jr_nested_cell(const jr_st* st, yyjson_val* v) {
    if (!v || yyjson_is_null(v)) return ray_f64(NULL_F64);
    if (yyjson_is_arr(v) && yyjson_arr_size(v)) {
        int recs = 1, any = 0;
        size_t idx, max;
        yyjson_val* e;
        yyjson_arr_foreach(v, idx, max, e) {
            if (yyjson_is_null(e)) continue;
            any = 1;
            if (!yyjson_is_obj(e)) { recs = 0; break; }
        }
        if (recs && any) {
            int64_t m = (int64_t)yyjson_arr_size(v);
            yyjson_val** ev = (yyjson_val**)malloc((size_t)m * sizeof *ev);
            if (!ev) return q_err(QE_WSFULL);
            int64_t i = 0;
            yyjson_arr_foreach(v, idx, max, e) ev[i++] = e;
            ray_t* t = jr_nested(st, ev, m);
            free(ev);
            return t;
        }
    }
    return j_node(v, 1);
}

static ray_t* jr_nested_mixed(const jr_st* st, yyjson_val** vals, int64_t n) {
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        ray_t* c = jr_nested_cell(st, vals[i]);
        if (!c || RAY_IS_ERR(c)) {
            ray_release(out);
            return c ? c : q_err(QE_OOM);
        }
        out = ray_list_append(out, c);               /* RETAINS c */
        ray_release(c);
        if (RAY_IS_ERR(out)) return out;
    }
    ray_t* r = q_list_collapse(out);
    ray_release(out);
    return r;
}

/* R5: the recursive column loader over one column's cells (NULL = a null or
 * omitted value).  A column of RECORDS gets the same key-union + ct_t lattice the
 * top level runs — a nested `felt` written 2 beside null is a LONG column holding
 * 0N, so `sum` answers the truth instead of 0n — and its own '*' columns recurse
 * in turn.  Anything else falls to the as-written cell, so a genuine mix stays a
 * mix and recursion cannot MISS: every value is evidence (no sample cutoff), and
 * `types` does not reach in (R6 — pull a nested table apart with q, not `path`). */
static ray_t* jr_nested(const jr_st* st, yyjson_val** vals, int64_t n) {
    int recs = 1, any = 0;
    size_t idx, max;
    yyjson_val *key, *val;
    for (int64_t i = 0; i < n && recs; i++) {
        if (!vals[i] || yyjson_is_null(vals[i])) continue;
        any = 1;
        recs = yyjson_is_obj(vals[i]) && !jr_rec_dup(vals[i]);
    }
    if (!(recs && any)) return jr_nested_mixed(st, vals, n);
    int64_t* names = NULL;
    jr_ct* k = NULL;
    int64_t ncols = 0, cap = 0;
    for (int64_t i = 0; i < n; i++) {
        if (!vals[i] || yyjson_is_null(vals[i])) continue;
        yyjson_obj_foreach(vals[i], idx, max, key, val) {
            int64_t nm = jr_key_sym(key);
            int64_t c = jr_find(names, ncols, nm);
            if (c < 0) {
                if (ncols == cap) {
                    int64_t g = cap ? cap * 2 : 8;
                    int64_t* gn = (int64_t*)realloc(names, (size_t)g * sizeof *gn);
                    jr_ct* gk = (jr_ct*)realloc(k, (size_t)g * sizeof *gk);
                    if (gn) names = gn;
                    if (gk) k = gk;
                    if (!gn || !gk) { free(names); free(k); return q_err(QE_WSFULL); }
                    cap = g;
                }
                c = ncols++;
                names[c] = nm;
                k[c] = (jr_ct){ 0 };
            }
            int native;
            ct_t ct = jr_detect(st, val, &native);
            jr_note(&k[c], ct, native);
        }
    }
    if (!ncols) {                                    /* nothing but empty records */
        free(names);
        free(k);
        return jr_nested_mixed(st, vals, n);
    }
    yyjson_val** cellv = (yyjson_val**)calloc((size_t)(ncols * n), sizeof *cellv);
    ray_t* tbl = cellv ? ray_table_new(ncols) : q_err(QE_WSFULL);
    if (RAY_IS_ERR(tbl)) {
        free(names);
        free(k);
        free(cellv);
        return tbl;
    }
    for (int64_t i = 0; i < n; i++) {
        if (!vals[i] || yyjson_is_null(vals[i])) continue;
        yyjson_obj_foreach(vals[i], idx, max, key, val)
            cellv[jr_find(names, ncols, jr_key_sym(key)) * n + i] = val;
    }
    for (int64_t c = 0; c < ncols && !RAY_IS_ERR(tbl); c++) {
        char bc = jr_char(&k[c]);
        ray_t* col;
        if (bc == '*') col = jr_nested(st, cellv + c * n, n);
        else {
            ray_t* l = ray_list_new(n > 0 ? n : 1);
            for (int64_t i = 0; i < n && !RAY_IS_ERR(l); i++) {
                yyjson_val* v = cellv[c * n + i];
                ray_t* a = v ? jr_cell(st, bc, v) : jr_null_cell(st, bc);
                if (!a || RAY_IS_ERR(a)) {
                    ray_release(l);
                    l = a ? a : q_err(QE_OOM);
                    break;
                }
                l = ray_list_append(l, a);           /* RETAINS a */
                ray_release(a);
            }
            col = RAY_IS_ERR(l) ? l : q_list_collapse(l);
            if (!RAY_IS_ERR(l)) ray_release(l);
        }
        if (!col || RAY_IS_ERR(col)) {
            ray_release(tbl);
            tbl = col ? col : q_err(QE_OOM);
            break;
        }
        tbl = ray_table_add_col(tbl, names[c], col);
        ray_release(col);
    }
    free(names);
    free(k);
    free(cellv);
    return tbl;
}

static ray_t* jr_assemble(const jr_st* st, ray_t** acc, yyjson_val*** vcol, int64_t nbuilt) {
    int64_t keep = 0;
    for (int64_t j = 0; j < st->ncols; j++)
        if (st->want[j] != ' ') keep++;
    if (!keep) return q_err(QE_TYPE);                /* every column dropped is not a table */
    ray_t* tbl = ray_table_new(keep);
    if (RAY_IS_ERR(tbl)) return tbl;
    for (int64_t j = 0; j < st->ncols; j++) {
        if (st->want[j] == ' ') continue;
        char bc = jr_build_char(st, j);              /* '*' is the string/nested COLUMN's list container */
        ray_t* col;
        if (vcol && vcol[j]) {
            if (st->want[j] && st->want[j] != '*') { /* the caller named a type: build as written, convert */
                ray_t* l = ray_list_new(nbuilt > 0 ? nbuilt : 1);
                for (int64_t i = 0; i < nbuilt && !RAY_IS_ERR(l); i++) {
                    ray_t* a = vcol[j][i] ? j_node(vcol[j][i], 1) : ray_f64(NULL_F64);
                    if (!a || RAY_IS_ERR(a)) {
                        ray_release(l);
                        l = a ? a : q_err(QE_OOM);
                        break;
                    }
                    l = ray_list_append(l, a);
                    ray_release(a);
                }
                col = RAY_IS_ERR(l) ? l : q_list_collapse(l);
                if (!RAY_IS_ERR(l)) ray_release(l);
            } else
                col = nbuilt ? jr_nested(st, vcol[j], nbuilt) : ray_list_new(1);
        } else
            col = ray_len(acc[j]) ? q_list_collapse(acc[j])
                                  : (bc == '*' ? ray_list_new(1) : q_type_empty(q_type_of_char(bc)));
        if (col && !RAY_IS_ERR(col) && st->want[j] && st->want[j] != '*') {
            ray_t* c2 = jr_retype(st, st->want[j], col);
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

/* Build ONE record's cells, into row[]/rowv[], without touching the accumulators: a record that
 * fails must leave no half-written row behind, since a column count that drifted by one cell would
 * silently transpose the table.  Missing keys null-fill here, where `seen` says which.  A '*'
 * record-lane column stages the VALUE (defer[j]) so the whole committed column can load recursively. */
static ray_t* jr_row(jr_st* st, yyjson_val* rec, const char* defer, char* seen,
                     ray_t** row, yyjson_val** rowv) {
    size_t idx, max;
    yyjson_val *key, *val;
    ray_t* bad;
    if (st->bare) {
        row[0] = jr_cell(st, jr_build_char(st, 0), rec);
        if (row[0] && !RAY_IS_ERR(row[0])) return NULL;
        if (!row[0]) return q_err(QE_OOM);
        ray_t* e = row[0];
        row[0] = NULL;
        st->rej_class = "cast";                      /* the bare lane's miss is rejectable too */
        st->rej_col = st->names[0];
        return e;
    }
    if ((bad = jr_check_keys(st, rec, seen))) return bad;
    yyjson_obj_foreach(rec, idx, max, key, val) {
        int64_t j = jr_find(st->names, st->ncols, jr_key_sym(key));
        if (defer[j]) { rowv[j] = val; continue; }
        row[j] = jr_cell(st, jr_build_char(st, j), val);
        if (!row[j]) return q_err(QE_OOM);
        if (RAY_IS_ERR(row[j])) {
            ray_t* e = row[j];
            row[j] = NULL;
            st->rej_class = "cast";
            st->rej_col = st->names[j];
            return e;
        }
    }
    for (int64_t j = 0; j < st->ncols; j++) {
        if (seen[j] || defer[j]) continue;
        row[j] = jr_null_cell(st, jr_build_char(st, j));
        if (!row[j]) return q_err(QE_OOM);
    }
    return NULL;
}

static ray_t* jr_build(jr_st* st) {
    ray_t** acc = (ray_t**)calloc((size_t)st->ncols, sizeof *acc);
    ray_t** row = (ray_t**)calloc((size_t)st->ncols, sizeof *row);
    char* seen = (char*)calloc((size_t)st->ncols, 1);
    char* defer = (char*)calloc((size_t)st->ncols, 1);
    yyjson_val** rowv = (yyjson_val**)calloc((size_t)st->ncols, sizeof *rowv);
    yyjson_val*** vcol = (yyjson_val***)calloc((size_t)st->ncols, sizeof *vcol);
    int64_t nbuilt = 0;
    ray_t* bad = (acc && row && seen && defer && rowv && vcol) ? NULL : q_err(QE_WSFULL);
    for (int64_t j = 0; !bad && j < st->ncols; j++) {
        defer[j] = !st->bare && jr_build_char(st, j) == '*';
        if (defer[j]) {
            vcol[j] = (yyjson_val**)calloc((size_t)(st->nrecs > 0 ? st->nrecs : 1), sizeof **vcol);
            if (!vcol[j]) bad = q_err(QE_WSFULL);
        }
        acc[j] = ray_list_new(st->nrecs > 0 ? st->nrecs : 1);
        if (RAY_IS_ERR(acc[j])) { bad = acc[j]; acc[j] = NULL; }
    }
    for (int64_t i = 0; !bad && i < st->nrecs; i++) {
        st->rej_class = NULL;
        st->rej_col = 0;
        memset(rowv, 0, (size_t)st->ncols * sizeof *rowv);
        ray_t* rowbad = jr_row(st, st->recs[i], defer, seen, row, rowv);
        /* the record did not BUILD: nothing has been appended, so dropping it is exact — and under a
         * continue-mode lever (R2) a rejectable fault leaves its reject record.  An APPEND failure
         * below is an allocation failure, not a bad record - it can leave the columns ragged, so it
         * is fatal whatever ignore_errors says.  That split is what makes "whole or not at all" a
         * fact rather than an intention. */
        if (rowbad) {
            for (int64_t j = 0; j < st->ncols; j++) {
                if (row[j]) ray_release(row[j]);
                row[j] = NULL;
            }
            if (st->rej_class && (st->ignore_errors || st->store_rej)) {
                ray_release(rowbad);
                bad = jr_reject_val(st, st->lines[i], st->rej_col, st->rej_class, st->recs[i]);
                continue;
            }
            bad = rowbad;
            break;
        }
        for (int64_t j = 0; !bad && j < st->ncols; j++) {
            if (defer[j]) { vcol[j][nbuilt] = rowv[j]; continue; }
            ray_t* l = ray_list_append(acc[j], row[j]);   /* RETAINS row[j] */
            if (RAY_IS_ERR(l)) { acc[j] = NULL; bad = l; break; }
            acc[j] = l;
        }
        for (int64_t j = 0; j < st->ncols; j++) {
            if (row[j] && row[j] != bad) ray_release(row[j]);
            row[j] = NULL;
        }
        if (!bad) nbuilt++;
    }
    ray_t* r = bad ? bad : jr_assemble(st, acc, vcol, nbuilt);
    for (int64_t j = 0; j < st->ncols; j++) {
        if (acc[j]) ray_release(acc[j]);
        if (vcol) free(vcol[j]);
    }
    free(acc);
    free(row);
    free(seen);
    free(defer);
    free(rowv);
    free(vcol);
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
            else if (jr_opt_is(v->i64, "array")) st->format = JF_ARRAY;
            else if (jr_opt_is(v->i64, "newline_delimited")) st->format = JF_ND;
            else if (jr_opt_is(v->i64, "auto")) st->format = JF_AUTO;
            else bad = q_err(QE_NYI);
        } else if (jr_opt_is(k->i64, "records")) {
            if (v->type == -RAY_BOOL) st->records = v->u8 ? JR_REC_ALWAYS : JR_REC_NEVER;
            else if (v->type == -RAY_SYM && jr_opt_is(v->i64, "auto")) st->records = JR_REC_AUTO;
            else bad = q_err(QE_TYPE);
        } else if (jr_opt_is(k->i64, "path")) {
            if (v->type != RAY_NULL) bad = jr_path_set(st, v);
        } else if (jr_opt_is(k->i64, "ignore_errors") || jr_opt_is(k->i64, "store_rejects") ||
                   jr_opt_is(k->i64, "null_padding")) {
            if (v->type != -RAY_BOOL) bad = q_err(QE_TYPE);
            else if (jr_opt_is(k->i64, "ignore_errors")) st->ignore_errors = v->u8 != 0;
            else if (jr_opt_is(k->i64, "store_rejects")) st->store_rej = v->u8 != 0;
            else st->null_pad = v->u8 != 0;          /* the key-union law already null-fills an omitted
                                                      * key, so this lever is inherently ON; accepted
                                                      * for .csv.read option parity, changing nothing */
        } else if (jr_opt_is(k->i64, "rejects_table")) {
            if (v->type != -RAY_SYM) bad = q_err(QE_TYPE);
            else {
                st->rej_name = v->i64;
                st->store_rej = 1;                   /* naming the table opts in, like .csv.read */
            }
        } else if (jr_opt_is(k->i64, "dateformat") || jr_opt_is(k->i64, "timestampformat")) {
            /* one cell home: the same two format options, validated against the same
             * strptime subset, replacing the writer's-forms grammar for their type */
            int is_ts = jr_opt_is(k->i64, "timestampformat");
            const char* fp;
            int64_t fl;
            if (!q_str_text_bytes(v, &fp, &fl)) bad = q_err(QE_TYPE);
            else if (fl <= 0 || fl >= Q_CSV_FMT_MAX || !q_csv_fmt_valid(fp, (size_t)fl, is_ts))
                bad = q_err(QE_OPTION);
            else {
                char* dst = is_ts ? st->fmt.tsfmt : st->fmt.datefmt;
                memcpy(dst, fp, (size_t)fl);
                dst[fl] = 0;
            }
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
    jr_st st = { .sample = J_DEF_SAMPLE, .records = JR_REC_AUTO };
    char* buf = NULL;
    int64_t n = 0;
    ray_t* bad = jr_opts(&st, opts);
    if (!bad) bad = jr_bytes(src, &buf, &n);
    if (!bad) bad = jr_frame(&st, buf, n);
    if (!bad) bad = jr_classify(&st);
    if (!bad) bad = jr_schema(&st);
    if (!bad && !info_only) bad = jr_types(&st, types);
    ray_t* r = bad ? bad : (info_only ? jr_info(&st) : jr_build(&st));
    if (r && !RAY_IS_ERR(r) && !info_only && st.store_rej) {
        /* one load, one audit: the stored table is REPLACED, empty on a clean load
         * (.csv.read's law; the name lands like any q assignment) */
        ray_t* rt = jr_rejects_tbl(&st);
        if (!rt || RAY_IS_ERR(rt)) {
            ray_release(r);
            r = rt ? rt : q_err(QE_OOM);
        } else {
            int64_t nm = st.rej_name ? st.rej_name : ray_sym_intern_runtime("reject_errors", 13);
            ray_err_t e = q_env_set(nm, rt);
            ray_release(rt);
            if (e) {
                ray_release(r);
                r = q_env_err(e);
            }
        }
    }
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
