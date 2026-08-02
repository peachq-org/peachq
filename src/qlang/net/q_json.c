/* q_json — openq's `.j` JSON namespace (ref/dotj.md).
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
#include "qlang/q_err.h"
#include "qlang/q_fmt.h"      /* q_fmt / q_fmt_float — the display + `\P` home */
#include "qlang/q_registry.h" /* q_list_collapse */
#include "qlang/ops/q_type.h"     /* q_type_is_inf — the infinity lane */
#include "lang/eval.h"        /* ray_at_fn — dict/table cell reads */
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

/* dict -> JSON object.  Keyed tables (keys is a TABLE) are deferred -> 'nyi. */
static void j_dict(jbuf* b, ray_t* x) {
    ray_t* keys = ray_dict_keys(x);    /* borrowed */
    ray_t* vals = ray_dict_vals(x);    /* borrowed */
    if (!keys || keys->type == RAY_TABLE) { j_nyi(b, x->type); return; }
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
 *  .j.k — deserialize (yyjson node -> ray_t)
 * ======================================================================== */

static ray_t* jk_node(yyjson_val* v) {
    switch (yyjson_get_type(v)) {
        case YYJSON_TYPE_NULL:
            return ray_f64(NULL_F64);                    /* -> 0n */
        case YYJSON_TYPE_BOOL:
            return ray_bool(yyjson_get_bool(v));
        case YYJSON_TYPE_NUM: {
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
                ray_t* c = jk_node(e);
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
                ray_t* cv = jk_node(val);
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
    ray_t* r = jk_node(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    free(buf);
    return r;
}
