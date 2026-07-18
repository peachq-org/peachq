/* q_builtins — register q's own builtins into the rayforce env.
 *
 * q reuses rayfall's eval/value/show as-is (they operate on ray_t), but needs
 * its own q-syntax `parse` (rayfall's `parse` reads lisp).  We bind names via
 * the public ray_fn_* / ray_env_bind API — never touching the frozen builtin
 * registration site — so `parse` overrides rayfall's lisp parse in the env. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_builtins.h"
#include "qlang/q_apply.h"    /* q_apply_noun — the noun-head dispatcher */
#include "qlang/q_deriv.h"    /* carrier inspectors — fn-value introspection */
#include "qlang/q_parse.h"
#include "qlang/q_json.h"     /* q_json_register — .j JSON namespace */
#include "qlang/q_ops.h"      /* q_ops_table — .Q.ops introspection source */
#include "qlang/q_registry.h" /* q_registry_init */
#include "qlang/q_sys.h"      /* q_system_fn — the q-owned `system` verb */
#include "qlang/q_fmt.h"      /* q_console_show — show's display sink */
#include "lang/env.h"       /* ray_fn_unary, ray_env_bind */
#include "lang/eval.h"      /* RAY_FN_NONE */
#include "lang/format.h"    /* ray_fmt — q string cast */
#include "table/sym.h"      /* ray_sym_vec_cell */
#include "mem/sys.h"        /* ray_sys_alloc — remote-eval scratch */
#include "ops/ops.h"        /* ray_is_lazy, ray_lazy_materialize */
#include <rayforce.h>
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (parse str) — parse q SOURCE into a ray_t AST (overrides rayfall's lisp
 * parse).  q_parse needs a NUL-terminated C string; a RAY_STR atom's bytes are
 * not guaranteed terminated, so copy through a bounded scratch buffer. */
/* Exported (q_builtins.h) so the `-5!` internal-fn alias single-homes here. */
ray_t* q_parse_builtin_fn(ray_t* x) {
    if (!x || x->type != -RAY_STR) return ray_error("type", "parse expects a string");
    const char* sp = ray_str_ptr(x);
    size_t sl = ray_str_len(x);
    if (!sp) return ray_error("domain", "parse: bad source string");
    char* src = malloc(sl + 1);
    if (!src) return ray_error("wsfull", "parse: out of memory");
    memcpy(src, sp, sl);
    src[sl] = '\0';
    ray_t* ast = q_parse(src);
    free(src);
    return ast ? ast : ray_error("parse", NULL);
}

/* (string x) — q cast-to-string.  ATOM: a sym renders bare (`ibm -> "ibm"),
 * a string passes through, other atoms reuse rayfall's formatter (string 42
 * -> "42").  VECTOR / LIST: q maps string over each item, yielding a LIST of
 * strings (`string 192 168 1 23` -> ("192";"168";"1";"23")) — the base
 * formatter would instead render the whole vector as one bracketed string. */
ray_t* q_string_fn(ray_t* x) {
    if (!x) return ray_error("type", "string: nil");
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);        /* borrowed */
        if (!s) return ray_error("type", "string: bad symbol");
        ray_retain(s);
        return s;
    }
    if (x->type == -RAY_STR) { ray_retain(x); return x; }
    /* Only a simple VECTOR or a boxed LIST maps element-wise; atoms and
     * whole-value containers (tables, dicts, and anything else) keep the base
     * formatter — indexing a table by 0..ncols would fabricate junk rows. */
    if (!ray_is_vec(x) && x->type != RAY_LIST) return ray_fmt(x, 0);
    /* vector or boxed list: per-element string */
    int64_t n = ray_len(x);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(x, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
        ray_t* s = q_string_fn(e);
        ray_release(e);
        if (!s || RAY_IS_ERR(s)) { ray_release(out); return s; }
        out = ray_list_append(out, s);
        ray_release(s);
        if (RAY_IS_ERR(out)) return out;
    }
    return out;
}

/* ---- recursive string-verb driver (upper / lower / trim family) -----------
 * q's string verbs are atomic through the container types: a LIST maps the
 * verb over its items, a DICT over its values (keys unchanged), a TABLE over
 * its columns.  q_str_recurse walks that structure once and hands every
 * non-container node to `leaf`, so upper/lower/trim share one traversal. */
typedef ray_t* (*q_str_leaf_fn)(ray_t* x, int mode);

static ray_t* q_str_recurse(ray_t* x, q_str_leaf_fn leaf, int mode) {
    if (!x) return ray_error("type", "string op: nil");
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* e = ray_at_fn(x, ia);
            ray_release(ia);
            if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
            ray_t* r = q_str_recurse(e, leaf, mode);
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);   /* borrowed */
        ray_t* v = ray_dict_vals(x);   /* borrowed */
        if (!k || !v) return ray_error("type", "string op: bad dict");
        ray_t* nv = q_str_recurse(v, leaf, mode);
        if (!nv || RAY_IS_ERR(nv)) return nv;
        ray_retain(k);
        return ray_dict_new(k, nv);    /* consumes k + nv */
    }
    if (x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* out = ray_table_new(nc);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(x, c);   /* borrowed */
            ray_t* ncol = q_str_recurse(col, leaf, mode);
            if (!ncol || RAY_IS_ERR(ncol)) { ray_release(out); return ncol; }
            out = ray_table_add_col(out, ray_table_col_name(x, c), ncol);
            ray_release(ncol);
            if (!out || RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return leaf(x, mode);
}

/* upper / lower — ASCII case shift for a string atom, a symbol atom, and the
 * string/symbol VECTOR forms; the container types recurse via q_str_recurse. */
static void q_case_bytes(const char* p, size_t n, char* b, int up) {
    for (size_t i = 0; i < n; i++)
        b[i] = (char)(up ? toupper((unsigned char)p[i]) : tolower((unsigned char)p[i]));
}
static ray_t* q_case_leaf(ray_t* x, int up) {
    if (!x) return ray_error("type", "%s: nil", up ? "upper" : "lower");
    if (x->type == -RAY_STR) {
        const char* p = ray_str_ptr(x);
        size_t n = ray_str_len(x);
        char stack[256];
        char* b = (n < sizeof stack) ? stack : malloc(n + 1);
        if (!b) return ray_error("wsfull", "%s: out of memory", up ? "upper" : "lower");
        q_case_bytes(p, n, b, up);
        ray_t* r = ray_str(b, n);
        if (b != stack) free(b);
        return r;
    }
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);   /* borrowed */
        if (!s) return ray_error("type", "%s: bad symbol", up ? "upper" : "lower");
        const char* p = ray_str_ptr(s);
        size_t n = ray_str_len(s);
        char stack[256];
        char* b = (n < sizeof stack) ? stack : malloc(n + 1);
        if (!b) return ray_error("wsfull", "%s: out of memory", up ? "upper" : "lower");
        q_case_bytes(p, n, b, up);
        int64_t id = ray_sym_intern(b, n);
        if (b != stack) free(b);
        return ray_sym(id);
    }
    if (x->type == RAY_SYM) {   /* symbol vector */
        int64_t n = ray_len(x);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* s = ray_sym_vec_cell(x, i);   /* borrowed str */
            const char* p = s ? ray_str_ptr(s) : "";
            size_t sn = s ? ray_str_len(s) : 0;
            char stack[256];
            char* b = (sn < sizeof stack) ? stack : malloc(sn + 1);
            if (!b) { ray_release(out); return ray_error("wsfull", "%s: oom", up ? "upper" : "lower"); }
            q_case_bytes(p, sn, b, up);
            int64_t id = ray_sym_intern(b, sn);
            if (b != stack) free(b);
            out = ray_vec_append(out, &id);
            if (!out || RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_STR) {   /* string vector -> per-element case shift */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            size_t sn; const char* p = ray_str_vec_get(x, i, &sn);
            char stack[256];
            char* b = (sn < sizeof stack) ? stack : malloc(sn + 1);
            if (!b) { ray_release(out); return ray_error("wsfull", "%s: oom", up ? "upper" : "lower"); }
            q_case_bytes(p ? p : "", p ? sn : 0, b, up);
            ray_t* r = ray_str(b, p ? sn : 0);
            if (b != stack) free(b);
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return ray_error("type", "%s: expects a string or symbol", up ? "upper" : "lower");
}
static ray_t* q_upper_fn(ray_t* x) { return q_str_recurse(x, q_case_leaf, 1); }
static ray_t* q_lower_fn(ray_t* x) { return q_str_recurse(x, q_case_leaf, 0); }

/* trim / ltrim / rtrim — mode 0=both, 1=leading only, 2=trailing only.
 * A string atom strips ASCII whitespace; a simple (non-string) vector strips
 * leading/trailing NULLs (kdb's `trim 0N 0N 1 2 0N` -> 1 2); any other atom
 * passes through unchanged (`trim 42` -> 42).  Containers recurse. */
static int q_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static ray_t* q_trim_leaf(ray_t* x, int mode) {
    if (!x) return ray_error("type", "trim: nil");
    if (x->type == -RAY_STR) {
        const char* p = ray_str_ptr(x);
        size_t n = ray_str_len(x), a = 0, b = n;
        if (mode != 2) while (a < b && q_is_ws(p[a])) a++;
        if (mode != 1) while (b > a && q_is_ws(p[b - 1])) b--;
        return ray_str(p + a, b - a);
    }
    if (x->type == RAY_STR) {   /* string vector -> trim each element */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            size_t sn; const char* p = ray_str_vec_get(x, i, &sn);
            size_t a = 0, b = p ? sn : 0;
            if (mode != 2) while (a < b && q_is_ws(p[a])) a++;
            if (mode != 1) while (b > a && q_is_ws(p[b - 1])) b--;
            ray_t* r = ray_str(p ? p + a : "", b - a);
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (ray_is_vec(x)) {        /* simple non-string vector -> strip NULL ends */
        int64_t n = ray_len(x), a = 0, b = n;
        if (mode != 2) while (a < b && ray_vec_is_null(x, a)) a++;
        if (mode != 1) while (b > a && ray_vec_is_null(x, b - 1)) b--;
        if (a == 0 && b == n) { ray_retain(x); return x; }
        ray_t* idx = ray_vec_new(RAY_I64, b - a);
        if (RAY_IS_ERR(idx)) return idx;
        for (int64_t i = a; i < b; i++) idx = ray_vec_append(idx, &i);
        ray_t* r = ray_at_fn(x, idx);
        ray_release(idx);
        if (!r || RAY_IS_ERR(r)) return r;
        ray_t* c = q_collapse_list(r);   /* boxed slice -> typed vector display */
        ray_release(r);
        return c;
    }
    ray_retain(x);              /* atom passthrough (trim 42 -> 42) */
    return x;
}
static ray_t* q_trim_fn (ray_t* x) { return q_str_recurse(x, q_trim_leaf, 0); }
static ray_t* q_ltrim_fn(ray_t* x) { return q_str_recurse(x, q_trim_leaf, 1); }
static ray_t* q_rtrim_fn(ray_t* x) { return q_str_recurse(x, q_trim_leaf, 2); }

/* ---- md5 (RFC 1321, public spec — CLEAN ROOM) -----------------------------
 * q `md5 s` hashes the string s to a 16-byte digest (a RAY_U8 byte vector,
 * displayed as `0x…`).  Self-contained implementation of the published
 * algorithm; no external dependency, no kdb reference. */
static int q_md5_compute(const uint8_t* msg, size_t len, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const uint32_t S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    /* padded length: message + 0x80 + zeros to 56 mod 64 + 8-byte bit length */
    size_t nblk = ((len + 8) / 64) + 1;
    uint8_t* buf = calloc(nblk, 64);
    if (!buf) return 0;                 /* OOM -> caller signals 'wsfull */
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[nblk * 64 - 8 + i] = (uint8_t)(bits >> (8 * i));
    for (size_t blk = 0; blk < nblk; blk++) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) {
            const uint8_t* q = buf + blk * 64 + i * 4;
            M[i] = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D);        g = i; }
            else if (i < 32) { F = (D & B) | (~D & C);        g = (5 * i + 1) & 15; }
            else if (i < 48) { F = B ^ C ^ D;                 g = (3 * i + 5) & 15; }
            else             { F = C ^ (B | ~D);              g = (7 * i) & 15; }
            F += A + K[i] + M[g];
            A = D; D = C; C = B;
            B += (F << S[i]) | (F >> (32 - S[i]));
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    free(buf);
    uint32_t words[4] = { a0, b0, c0, d0 };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(words[i] >> (8 * j));
    return 1;
}
/* Exported (q_builtins.h) so the `-15!` internal-fn alias single-homes here. */
ray_t* q_md5_fn(ray_t* x) {
    if (!x || x->type != -RAY_STR) return ray_error("type", "md5: expects a string");
    uint8_t digest[16];
    if (!q_md5_compute((const uint8_t*)ray_str_ptr(x), ray_str_len(x), digest))
        return ray_error("wsfull", "md5: out of memory");
    return ray_vec_from_raw(RAY_U8, digest, 16);
}

/* ---- .Q base64 + SHA-1 encoding primitives (ref/dotq.md; clean-room: RFC 4648
 * base64 + FIPS 180 SHA-1, both public standards, implemented from scratch —
 * zero-dependency, portable (fixed-width stdint, no POSIX calls)) ----------- */

static const char Q_B64_ALPHA[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Borrow the raw bytes of a string atom (-RAY_STR; use ray_str_len — SSO union
 * aliasing makes ray_len garbage on a string atom) or a byte vector (RAY_U8).
 * Returns 0 for any other type. */
static int q_bytes_of(ray_t* x, const uint8_t** p, size_t* n) {
    if (!x) return 0;
    if (x->type == -RAY_STR) { *p = (const uint8_t*)ray_str_ptr(x); *n = ray_str_len(x); return 1; }
    if (x->type == RAY_U8)   { *p = (const uint8_t*)ray_data(x);    *n = (size_t)ray_len(x); return 1; }
    return 0;
}

/* base64-encode n bytes of src into a fresh malloc'd buffer; *outlen set.
 * Returns NULL on OOM (caller frees the result). */
static char* q_base64_encode(const uint8_t* src, size_t n, size_t* outlen) {
    size_t olen = ((n + 2) / 3) * 4;
    char* out = (char*)malloc(olen ? olen : 1);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i+1] << 8) | src[i+2];
        out[o++] = Q_B64_ALPHA[(v >> 18) & 63];
        out[o++] = Q_B64_ALPHA[(v >> 12) & 63];
        out[o++] = Q_B64_ALPHA[(v >> 6) & 63];
        out[o++] = Q_B64_ALPHA[v & 63];
    }
    size_t rem = n - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)src[i] << 16;
        out[o++] = Q_B64_ALPHA[(v >> 18) & 63];
        out[o++] = Q_B64_ALPHA[(v >> 12) & 63];
        out[o++] = '='; out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i+1] << 8);
        out[o++] = Q_B64_ALPHA[(v >> 18) & 63];
        out[o++] = Q_B64_ALPHA[(v >> 12) & 63];
        out[o++] = Q_B64_ALPHA[(v >> 6) & 63];
        out[o++] = '=';
    }
    *outlen = o;
    return out;
}

/* base64-decode n chars of src into a fresh malloc'd byte buffer; *outlen set.
 * Returns NULL on OOM, and sets *bad=1 on malformed / incorrectly-padded input
 * (caller raises 'domain, per ref/dotq.md).  Skips ASCII whitespace; rejects any
 * non-alphabet char, data after padding, a non-multiple-of-4 group length, and
 * (RFC 4648 canonical form) non-zero residual pad bits. */
static uint8_t* q_base64_decode(const char* src, size_t n, size_t* outlen, int* bad) {
    static int8_t rev[256];
    static int inited = 0;
    if (!inited) {
        for (int i = 0; i < 256; i++) rev[i] = -1;
        for (int i = 0; i < 64; i++) rev[(unsigned char)Q_B64_ALPHA[i]] = (int8_t)i;
        inited = 1;
    }
    *bad = 0;
    uint8_t* out = (uint8_t*)malloc((n / 4) * 3 + 3);
    if (!out) return NULL;
    size_t o = 0;
    uint32_t acc = 0;
    int nbits = 0, pad = 0, ngroup = 0;
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') { pad++; ngroup++; continue; }
        if (pad) { *bad = 1; break; }                  /* data after padding */
        int8_t d = rev[(unsigned char)c];
        if (d < 0) { *bad = 1; break; }                /* non-alphabet char */
        acc = (acc << 6) | (uint32_t)d;
        nbits += 6; ngroup++;
        if (nbits >= 8) { nbits -= 8; out[o++] = (uint8_t)((acc >> nbits) & 0xFF); }
    }
    /* canonical form: whole groups of 4, at most 2 pad chars, and no stray
     * (non-zero) leftover bits in the final partial group. */
    if (!*bad && (ngroup % 4 != 0 || pad > 2 || (nbits && (acc & ((1u << nbits) - 1)))))
        *bad = 1;
    if (*bad) { free(out); return NULL; }
    *outlen = o;
    return out;
}

static uint32_t q_sha1_rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

/* SHA-1 (FIPS 180) of n bytes -> 20-byte big-endian digest.  Returns 1 on
 * success, 0 on OOM (caller raises 'wsfull — never a silent all-zero digest). */
static int q_sha1_compute(const uint8_t* msg, size_t n, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;
    uint64_t ml = (uint64_t)n * 8;
    size_t total = n + 1;
    while (total % 64 != 56) total++;
    total += 8;
    uint8_t* buf = (uint8_t*)calloc(total, 1);
    if (!buf) return 0;
    memcpy(buf, msg, n);
    buf[n] = 0x80;
    for (int i = 0; i < 8; i++) buf[total - 1 - i] = (uint8_t)(ml >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const uint8_t* p = buf + off + i * 4;
            w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        }
        for (int i = 16; i < 80; i++)
            w[i] = q_sha1_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t tmp = q_sha1_rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = q_sha1_rol(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(buf);
    uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(hs[i] >> (24 - 8 * j));
    return 1;
}

/* (.Q.btoa x) — base64-encode a string or byte vector to a char string.
 * Exported (q_builtins.h) so the `-32!` internal-fn alias single-homes here. */
ray_t* q_dotq_btoa_fn(ray_t* x) {
    const uint8_t* p; size_t n;
    if (!q_bytes_of(x, &p, &n))
        return ray_error("type", ".Q.btoa: expects a string or byte vector");
    size_t olen = 0;
    char* enc = q_base64_encode(p, n, &olen);
    if (!enc) return ray_error("wsfull", ".Q.btoa: out of memory");
    ray_t* r = ray_str(enc, olen);
    free(enc);
    return r;
}

/* (.Q.atob x) — base64-decode a char/byte input to a byte vector (ref/dotq.md:
 * throws 'domain if the data is not correctly padded). */
static ray_t* q_dotq_atob_fn(ray_t* x) {
    const uint8_t* p; size_t n;
    if (!q_bytes_of(x, &p, &n))
        return ray_error("type", ".Q.atob: expects a string or byte vector");
    size_t olen = 0; int bad = 0;
    uint8_t* dec = q_base64_decode((const char*)p, n, &olen, &bad);
    if (bad) return ray_error("domain", ".Q.atob: invalid base64 padding");
    if (!dec) return ray_error("wsfull", ".Q.atob: out of memory");
    ray_t* r = ray_vec_from_raw(RAY_U8, dec, (int64_t)olen);
    free(dec);
    return r;
}

/* (.Q.sha1 x) — SHA-1 digest of a string (or byte vector) as a 20-byte
 * bytestream (ref/dotq.md).  Exported (see q_builtins.h) so the `-33!` bang
 * alias routes to this single-home C impl. */
ray_t* q_dotq_sha1_fn(ray_t* x) {
    const uint8_t* p; size_t n;
    if (!q_bytes_of(x, &p, &n))
        return ray_error("type", ".Q.sha1: expects a string or byte vector");
    uint8_t digest[20];
    if (!q_sha1_compute(p, n, digest))
        return ray_error("wsfull", ".Q.sha1: out of memory");
    return ray_vec_from_raw(RAY_U8, digest, 20);
}

/* (show x) — print x's q console display as a SIDE EFFECT (buffered in the q
 * console sink; the host drains it), then return generic null.  Overrides
 * rayfall's `show`, which uses the rayfall formatter (`[1 2 3]`) rather than
 * q_fmt.  qdoc/repl print nothing for the null result, so the row shows only
 * the buffered display. */
static ray_t* q_show_fn(ray_t* x) {
    q_console_show(x);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

static ray_t* q_id_table(ray_t* x) {
    int64_t nc = ray_table_ncols(x);
    ray_t* out = ray_table_new(nc);
    int64_t stack[64];
    int64_t* used = (nc <= 64) ? stack : (int64_t*)malloc((size_t)nc * sizeof(int64_t));
    if (!used) { ray_release(out); return ray_error("wsfull", ".Q.id: out of memory"); }
    for (int64_t c = 0; c < nc; c++) {
        int64_t nm = q_name_sanitize(ray_table_col_name(x, c));
        nm = q_name_dedup(nm, used, c, 1);
        used[c] = nm;
        out = ray_table_add_col(out, nm, ray_table_get_col_idx(x, c));
        if (!out || RAY_IS_ERR(out)) break;
    }
    if (used != stack) free(used);
    return out;
}

static ray_t* q_id_dict(ray_t* x) {
    ray_t* k = ray_dict_keys(x);
    ray_t* v = ray_dict_vals(x);
    if (!k || k->type != RAY_SYM)
        return ray_error("type", ".Q.id: dictionary keys must be symbols");
    int64_t n = ray_len(k);
    ray_t* nk = ray_sym_vec_new(RAY_SYM_W64, n);
    int64_t stack[64];
    int64_t* used = (n <= 64) ? stack : (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!used) { ray_release(nk); return ray_error("wsfull", ".Q.id: out of memory"); }
    for (int64_t i = 0; i < n; i++) {
        ray_t* ks = ray_sym_vec_cell(k, i);
        int64_t id = ray_sym_intern_runtime(ray_str_ptr(ks), ray_str_len(ks));
        id = q_name_sanitize(id);
        id = q_name_dedup(id, used, i, 1);
        used[i] = id;
        nk = ray_vec_append(nk, &id);
        if (!nk || RAY_IS_ERR(nk)) break;
    }
    if (used != stack) free(used);
    if (!nk || RAY_IS_ERR(nk)) return nk ? nk : ray_error("oom", NULL);
    ray_retain(v);
    return ray_dict_new(nk, v);   /* consumes nk, retained v */
}

static ray_t* q_id_fn(ray_t* x) {
    if (!x) return ray_error("type", ".Q.id: nil");
    if (x->type == -RAY_SYM) return ray_sym(q_name_sanitize(x->i64));
    if (x->type == RAY_TABLE) return q_id_table(x);
    if (x->type == RAY_DICT)  return q_id_dict(x);
    return ray_error("type", ".Q.id: expects a symbol, table, or dictionary");
}

/* ---- function-value introspection (stage 2): type / count / value ------- */

static ray_unary_fn g_base_type  = NULL;
static ray_unary_fn g_base_count = NULL;

static int q_is_fn_value(ray_t* x) {
    if (!x) return 0;
    if (q_deriv_kind_of(x) != Q_DERIV_NONE) return 1;
    return x->type == RAY_LAMBDA || x->type == RAY_UNARY ||
           x->type == RAY_BINARY || x->type == RAY_VARY;
}

/* q `type` on FUNCTION values only — 100h lambda, 104h projection, 102h
 * operator (ref/datatypes.md).  Everything else stays on the base verb; the
 * full q type map is cast/type.qcmd territory. */
static ray_t* q_type_fn(ray_t* x) {
    if (x) {
        /* Function-VALUE carriers first: they are RAY_LISTs under the hood, so
         * the raw-tag path below would wrongly report them as 0h (general
         * list). */
        switch (q_deriv_kind_of(x)) {
        case Q_DERIV_LAMBDA:  return ray_i16(100);
        case Q_DERIV_PROJ:    return ray_i16(104);
        case Q_DERIV_MONAD:   return ray_i16(102);
        case Q_DERIV_COMPOSE: return ray_i16(105);
        default: break;
        }
        if (x->type == RAY_LAMBDA) return ray_i16(100);
        if (x->type == RAY_UNARY || x->type == RAY_BINARY || x->type == RAY_VARY)
            return ray_i16(102);
        /* q's `type` IS the internal tag: openq's type tags already ARE kdb's
         * type numbers (include/rayforce.h — long 7, sym 11, list 0, table 98,
         * dict 99) and atoms are stored NEGATIVE (`-7h` long atom vs `7h` long
         * vector).  This replaces the rayfall SYMBOL the base `type` returned
         * (` `i64 `/` `I64 `/` `LIST `).  NB a native -RAY_STR string reports
         * -10h (char ATOM); kdb's char VECTOR is 10h — a documented string-model
         * divergence (ARCHITECTURE.md), pinned in cast/type-deferred.qcmd. */
        return ray_i16(x->type);
    }
    return g_base_type ? g_base_type(x)
                       : ray_error("type", "type: base verb missing");
}

/* ---- type-char map + table introspection (type-output feature) ----------- */

/* Single home for the kdb type-number -> type-char map (ref/dotq.md `.Q.ty`,
 * `meta`'s `t` column).  Exhaustive switch over the ABSOLUTE type tag; 0 for
 * tags with no char (list 0, guid-gap 3, out-of-band).  Lowercase = the element
 * char; `.Q.ty` uppercases it for a uniform list of vectors. */
static char q_type_char(int8_t tag) {
    int t = tag < 0 ? -tag : tag;                   /* int arith: |INT8_MIN|==128, no UB */
    if (t <= 0 || t >= RAY_TYPE_COUNT) return 0;    /* list 0, gap 3 & out-of-band: no char */
    switch ((ray_type_e)t) {                        /* exhaustive: a new member demands a lane */
    case RAY_LIST:      return 0;                   /* unreachable: filtered above */
    case RAY_BOOL:      return 'b';
    case RAY_GUID:      return 'g';
    case RAY_U8:        return 'x';
    case RAY_I16:       return 'h';
    case RAY_I32:       return 'i';
    case RAY_I64:       return 'j';
    case RAY_F32:       return 'e';
    case RAY_F64:       return 'f';
    case RAY_STR:       return 'c';
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

/* True iff x is a uniform, matrix-mappable list of simple vectors of ONE
 * element type (kdb `.Q.ty` upper-case case, e.g. `3 2#3 4 5` or
 * `("abc";"de")`).  *elt receives that element tag (RAY_STR for a list of
 * char-vector strings). */
static int q_uniform_list(ray_t* x, int8_t* elt) {
    if (!x || x->type != RAY_LIST) return 0;
    int64_t n = ray_len(x);
    if (n == 0) return 0;
    ray_t** e = (ray_t**)ray_data(x);
    int8_t t0 = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ei = e[i];
        int8_t ti;
        if (ei && ei->type == -RAY_STR)      ti = (int8_t)RAY_STR;   /* char vec */
        else if (ei && ray_is_vec(ei))       ti = (int8_t)ei->type;  /* simple vec */
        else return 0;                                               /* not uniform */
        if (i == 0) t0 = ti; else if (ti != t0) return 0;
    }
    if (elt) *elt = t0;
    return 1;
}

/* type char of one value, as `.Q.ty`/`meta` see it.  Simple vector -> lower;
 * native string (-RAY_STR) -> 'c' (INTROSPECTION shim: openq stores a char
 * vector as one string atom); uniform list of vectors -> UPPER; else blank. */
static char q_ty_char(ray_t* x) {
    int8_t elt;
    if (x && x->type == -RAY_STR) return 'c';                       /* char-vec shim */
    if (x && ray_is_vec(x))       return q_type_char((int8_t)x->type);
    if (q_uniform_list(x, &elt)) {
        char lc = q_type_char(elt);
        return (char)(lc ? (lc - 'a' + 'A') : ' ');
    }
    return ' ';
}

/* (.Q.ty x) — LOWER char for a simple vector / string, UPPER for a uniform
 * list of vectors, blank (" ") otherwise (ref/dotq.md).  Returns a 1-char
 * string (openq has no char atom; `.Q.ty each` over strings therefore yields a
 * list of 1-char strings, not one packed char vector — the `"jc JC"` combined
 * example is a deferred string-model cell). */
static ray_t* q_dotq_ty_fn(ray_t* x) {
    char c = q_ty_char(x);
    return ray_str(&c, 1);
}

/* (.Q.qt x) — is-table predicate (ref/dotq.md `qt`): 1b if x is a table
 * (simple OR keyed), else 0b. */
static ray_t* q_dotq_qt_fn(ray_t* x) {
    return ray_bool(x && (x->type == RAY_TABLE || q_is_keyed_table(x)));
}

/* (.Q.qp x) — is-partitioned predicate (ref/dotq.md `qp`): partitioned table
 * -> 1b, splayed table -> 0b, anything else -> 0 (a LONG, not a bool).  openq
 * has no on-disk partitioned or splayed tables, so nothing in memory is either
 * — every value falls into the "anything else" arm and returns the long 0.
 * The doc's own `.Q.qp select from B -> 0` pins this: an in-memory table (as
 * openq's tables always are) is NOT splayed, so it returns 0 (long), not 0b. */
static ray_t* q_dotq_qp_fn(ray_t* x) {
    (void)x;
    return ray_i64(0);
}

/* (.Q.s x) — x formatted to plain text as the console prints it (ref/dotq.md
 * `.Q.s`), returned as a q string.  SINGLE-HOMES to q_fmt_console — the same
 * DISPLAY seam `show` uses (q_console_show = q_fmt_console + '\n') — so
 * `.Q.s x` is byte-identical to what `show x` prints, INCLUDING the
 * line-terminating trailing newline, and OBEYS the `\c` console width/height
 * ("Obeys console width and height set by \c", ref/dotq.md) and `\P`
 * precision.  We do NOT reuse q_console_show here: that appends into the
 * global console sink the REPL/qdoc host drains after each eval, so routing
 * through it would inject `.Q.s`'s text into the host's output — `.Q.s` must
 * be side-effect-free and RETURN the string.
 * The renderer truncates silently into a bounded buffer, so grow until the
 * rendered length CONVERGES (a larger buffer yields no more bytes).  NB
 * `len < cap-1` is NOT a reliable fit test: the renderer stops at an element
 * boundary when the buffer fills, so a truncated render can still leave room
 * at the tail (e.g. an unclipped `.Q.s til 5000` fills only ~8190 of 8192).
 * Convergence is reliable because the output length is monotonic
 * non-decreasing in buffer size — equal length after doubling means the whole
 * display fit.  Capped to stay bounded. */
static ray_t* q_dotq_s_fn(ray_t* x) {
    size_t cap = 8192;
    char* buf = malloc(cap);
    if (!buf) return ray_error("wsfull", ".Q.s: out of memory");
    buf[0] = '\0';
    q_fmt_console(x, buf, cap);                      /* `.Q.s` OBEYS `\c` */
    size_t len = strlen(buf);
    while (cap < (1u << 24)) {                       /* grow until length settles */
        size_t ncap = cap * 2;
        char* nb = realloc(buf, ncap);
        if (!nb) { free(buf); return ray_error("wsfull", ".Q.s: out of memory"); }
        buf = nb;
        cap = ncap;
        buf[0] = '\0';
        q_fmt_console(x, buf, cap);
        size_t nlen = strlen(buf);
        if (nlen == len) break;                      /* converged: full display */
        len = nlen;
    }
    char* out = malloc(len + 2);
    if (!out) { free(buf); return ray_error("wsfull", ".Q.s: out of memory"); }
    memcpy(out, buf, len);
    out[len] = '\n';                                 /* console line terminator */
    ray_t* r = ray_str(out, len + 1);
    free(out);
    free(buf);
    return r;
}

/* Column name ids of a table as a RAY_SYM vector.  A keyed table (RAY_DICT of
 * key-table -> value-table) yields key cols ++ value cols. */
static ray_t* q_table_colnames(ray_t* x) {
    if (x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, nc > 0 ? nc : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        for (int64_t c = 0; c < nc; c++) {
            int64_t nm = ray_table_col_name(x, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        }
        return out;
    }
    if (x->type == RAY_DICT) {
        ray_t* kt = ray_dict_keys(x);      /* borrowed */
        ray_t* vt = ray_dict_vals(x);      /* borrowed */
        if (!kt || !vt || kt->type != RAY_TABLE || vt->type != RAY_TABLE)
            return ray_error("type", "cols: expects a table");
        int64_t knc = ray_table_ncols(kt), vnc = ray_table_ncols(vt);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, knc + vnc > 0 ? knc + vnc : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        for (int64_t c = 0; c < knc; c++) {
            int64_t nm = ray_table_col_name(kt, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        }
        for (int64_t c = 0; c < vnc; c++) {
            int64_t nm = ray_table_col_name(vt, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        }
        return out;
    }
    return ray_error("type", "cols: expects a table");
}

/* Resolve a by-name table operand (cols`t / meta`t): a -RAY_SYM naming a
 * global plain/keyed table resolves to it (borrowed); else x unchanged. */
static ray_t* q_bi_deref_table(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* g = ray_env_get(x->i64);                 /* borrowed */
        if (g && (g->type == RAY_TABLE || q_is_keyed_table(g))) return g;
    }
    return x;
}

/* (cols x) — column names of a table as a symbol vector. */
static ray_t* q_cols_fn(ray_t* x) {
    if (!x) return ray_error("type", "cols: nil");
    return q_table_colnames(q_bi_deref_table(x));
}

/* Flatten a plain-or-keyed table to a single plain RAY_TABLE (key cols first).
 * Returns owned (retained for a plain table). */
static ray_t* q_meta_flatten(ray_t* x) {
    if (x->type == RAY_TABLE) { ray_retain(x); return x; }
    if (x->type != RAY_DICT) return ray_error("type", "meta: expects a table");
    ray_t* kt = ray_dict_keys(x);          /* borrowed */
    ray_t* vt = ray_dict_vals(x);          /* borrowed */
    if (!kt || !vt || kt->type != RAY_TABLE || vt->type != RAY_TABLE)
        return ray_error("type", "meta: expects a table");
    int64_t knc = ray_table_ncols(kt), vnc = ray_table_ncols(vt);
    ray_t* out = ray_table_new(knc + vnc > 0 ? knc + vnc : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int64_t c = 0; c < knc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(kt, c), ray_table_get_col_idx(kt, c));
    for (int64_t c = 0; c < vnc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(vt, c), ray_table_get_col_idx(vt, c));
    return out;
}

/* (meta x) — table metadata keyed by column name.  Builds the keyed table
 * (c) -> (t; f; a): `c` column names, `t` per-column type char (via the
 * single-home map), `f`/`a` blank (foreign-keys/attributes are out of scope).
 * The result is a RAY_DICT from a 1-col key table to a 3-col value table —
 * "a keyed table is just a dictionary from one table to another" (q_fmt
 * renders it `k| v`). */
static ray_t* q_meta_fn(ray_t* x) {
    if (!x) return ray_error("type", "meta: nil");
    ray_t* flat = q_meta_flatten(q_bi_deref_table(x));
    if (!flat || RAY_IS_ERR(flat)) return flat;
    int64_t nc = ray_table_ncols(flat);
    int64_t cap = nc > 0 ? nc : 1;
    ray_t* cvec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* c: names          */
    ray_t* fvec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* f: blank per col  */
    ray_t* avec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* a: blank per col  */
    char stackt[64];
    char* tbuf = (cap <= (int64_t)sizeof stackt) ? stackt : (char*)malloc((size_t)cap);
    if (!cvec || RAY_IS_ERR(cvec) || !fvec || RAY_IS_ERR(fvec) ||
        !avec || RAY_IS_ERR(avec) || !tbuf) {
        if (cvec && !RAY_IS_ERR(cvec)) ray_release(cvec);
        if (fvec && !RAY_IS_ERR(fvec)) ray_release(fvec);
        if (avec && !RAY_IS_ERR(avec)) ray_release(avec);
        if (tbuf && tbuf != stackt) free(tbuf);
        ray_release(flat);
        return ray_error("wsfull", "meta: out of memory");
    }
    int64_t blank = ray_sym_intern_runtime("", 0);
    int ok = 1;
    for (int64_t c = 0; c < nc && ok; c++) {
        int64_t nm = ray_table_col_name(flat, c);
        ray_t* col = ray_table_get_col_idx(flat, c);   /* borrowed */
        tbuf[c] = q_ty_char(col);
        cvec = ray_vec_append(cvec, &nm);
        fvec = ray_vec_append(fvec, &blank);
        avec = ray_vec_append(avec, &blank);
        if (!cvec || RAY_IS_ERR(cvec) || !fvec || RAY_IS_ERR(fvec) ||
            !avec || RAY_IS_ERR(avec)) ok = 0;
    }
    ray_release(flat);
    ray_t* tstr = ok ? ray_str(tbuf, (size_t)nc) : NULL;
    if (tbuf != stackt) free(tbuf);
    if (!ok || !tstr || RAY_IS_ERR(tstr)) {
        if (cvec && !RAY_IS_ERR(cvec)) ray_release(cvec);
        if (fvec && !RAY_IS_ERR(fvec)) ray_release(fvec);
        if (avec && !RAY_IS_ERR(avec)) ray_release(avec);
        if (tstr && !RAY_IS_ERR(tstr)) ray_release(tstr);
        return ray_error("wsfull", "meta: build failed");
    }
    /* key table: c ; value table: t f a  -> keyed table dict */
    ray_t* kt = ray_table_new(1);
    kt = ray_table_add_col(kt, ray_sym_intern("c", 1), cvec);
    ray_release(cvec);
    ray_t* vt = ray_table_new(3);
    vt = ray_table_add_col(vt, ray_sym_intern("t", 1), tstr); ray_release(tstr);
    if (!RAY_IS_ERR(vt)) { vt = ray_table_add_col(vt, ray_sym_intern("f", 1), fvec); }
    ray_release(fvec);
    if (!RAY_IS_ERR(vt)) { vt = ray_table_add_col(vt, ray_sym_intern("a", 1), avec); }
    ray_release(avec);
    if (RAY_IS_ERR(kt)) { if (!RAY_IS_ERR(vt)) ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);   /* consumes kt, vt */
}

/* q `count` of any function value is 1 (kdb: functions are atoms) — the
 * carrier is a RAY_LIST, so the base count would leak its slot count. */
static ray_t* q_count_fn(ray_t* x) {
    if (q_is_fn_value(x)) return ray_i64(1);
    return g_base_count ? g_base_count(x)
                        : ray_error("type", "count: base verb missing");
}

/* Capture a base unary's fn pointer AND attrs — a wrapper must be bound with
 * the base flags (count is RAY_FN_AGGR | RAY_FN_LAZY_AWARE; dropping them
 * would de-aggregate count in the planner and force lazy materialization —
 * codex stage-2 finding). */
static void capture_base(const char* name, ray_unary_fn* out, uint8_t* attrs) {
    ray_t* v = ray_env_get(ray_sym_intern(name, strlen(name)));
    if (v && v->type == RAY_UNARY) {
        *out = (ray_unary_fn)(uintptr_t)v->i64;
        if (attrs) *attrs = v->attrs;
    }
}

static void bind_unary(const char* name, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(name, RAY_FN_NONE, fn);
    ray_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

static void bind_vary(const char* name, ray_vary_fn fn) {
    ray_t* obj = ray_fn_vary(name, RAY_FN_NONE, fn);
    ray_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

static void bind_value(const char* name, ray_t* val) {
    ray_env_bind(ray_sym_intern(name, strlen(name)), val);
    ray_release(val);
}

/* Remote-source string eval (IPC request payloads, journal replay): the q
 * pipeline, mirroring q_repl.c run_one_line — q_parse -> q_lower ->
 * ray_eval -> materialize.  Buffered q console output (`show`, 0N!) is
 * drained to the SERVER's stdout after each eval (kdb prints show output
 * on the server console) and reset so it can't bleed into later requests.
 * Returns an OWNED value; parse/lower/eval errors return as owned errors
 * (the IPC layer serializes them as -128h responses). */
static ray_t* q_remote_eval_str(const char* src, size_t len) {
    /* A leading `\` is a system command, not q source: kdb runs a solo `\l`/`\p`
     * received on the wire.  `system"X"` is exactly `\X`, so strip the `\` and
     * reuse q_system_fn — single-homing q_sys_run, the restricted-mode guard,
     * and the `\`-shell stdout capture.  A loaded script's `show`/0N! output
     * is drained to the server console just like the q path. */
    if (len > 0 && src[0] == '\\') {
        ray_t* arg = ray_str(src + 1, len - 1);   /* the command minus its leading `\` */
        if (!arg) return ray_error("oom", "remote eval: out of memory");
        ray_t* r = q_system_fn(arg);
        ray_release(arg);
        { const char* con = q_console_str();
          if (con && *con) fputs(con, stdout);
          q_console_reset(); }
        return r;
    }
    char* tmp = (char*)ray_sys_alloc(len + 1);
    if (!tmp) return ray_error("oom", "remote eval: out of memory");
    memcpy(tmp, src, len);
    tmp[len] = '\0';
    ray_t* ast = q_parse(tmp);
    ray_sys_free(tmp);
    if (RAY_IS_ERR(ast)) return ast;
    /* A trailing assignment answers with the generic null, not the assigned
     * value (basics/ipc.md: `h"fn:{2+x}"` displays nothing).  Checked on the
     * PRE-lower shape — q_lower rewrites assignment into a `set` application —
     * which is why q_repl.c/qdoc.c ask here too. */
    const int is_assign = q_ast_is_assign(ast);
    ast = q_lower(ast);
    if (RAY_IS_ERR(ast)) return ast;
    ray_t* r = ray_eval(ast);
    ray_release(ast);
    if (ray_is_lazy(r))
        r = ray_lazy_materialize(r);
    { const char* con = q_console_str();
      if (con && *con) fputs(con, stdout);
      q_console_reset(); }
    if (is_assign && !RAY_IS_ERR(r)) {   /* an error still propagates (-128h) */
        ray_release(r);
        ray_retain(RAY_NULL_OBJ);        /* owned return (q_sys.c's silent-null precedent) */
        return RAY_NULL_OBJ;
    }
    return r;
}

/* Remote (func;args) value-apply (IPC request payloads): the kdb value/apply
 * wire shape.  Reuse the single-home q `value` (q_value_wrap), which already
 * does a SINGLE list-apply — resolving a symbol head (`sum`), a string-source
 * head (`"+"`), a value head, or a 100h lambda carrier (`{x*2}`) — and applies
 * it to the already-evaluated tail args, NEVER re-evaluating them (ADR-0004:
 * value, not eval).  `list` is BORROWED (the ipc layer releases it); q_value_wrap
 * does not consume it and returns an OWNED value.  Restricted mode needs no
 * re-assert here: ipc_dispatch sets ray_eval_get_restricted() around the whole
 * dispatch, and any restricted primitive reached by the apply (`hopen`,
 * `system`…) self-checks it — the exact contract the string hook relies on. */
static ray_t* q_remote_apply(ray_t* list) {
    return q_value_wrap(list);
}

/* ---- .Q.ops — Q_OPS[] as a read-only introspection table ------------------
 * (.Q.ops[]) materializes the verb manifest (src/qlang/q_ops.c) as a fresh
 * unkeyed table each call, so user code can query the op roster without any
 * path into the immutable registry — mutating the returned table cannot change
 * how verbs resolve.  OWNER RULING 2026-07-14: lives under `.Q` (openq
 * extension entry beside .Q.qt/.Q.qp; kdb has no .Q.ops).  Columns:
 *   name          sym   verb spelling
 *   lexclass      sym   `glyph / `kw_infix / `kw_prefix / `adverb (QLEX_*)
 *   monadic       bool  1b iff Q_OPS gives a build recipe at the monadic slot
 *                       (mon.kind != QK_NONE).  NB for the VARY prefix-keyword
 *                       verbs (ej/aj/wj/... — QR_ENV in the monadic slot) this
 *                       means "resolvable in prefix/bracket position", NOT that
 *                       the verb is strictly arity-1; true arity is not modelled
 *                       in the manifest.
 *   dyadic        bool  1b iff Q_OPS gives a build recipe at the dyadic slot
 *   deterministic bool  1b unless the verb's result is nondeterministic
 *   sideeffect    bool  1b iff evaluation performs an observable side effect
 *   family        sym   structure-dispatch family (`atomic`map`aggregate`index
 *                       `rowid`structural`irregular`none — q_ops.h vocabulary,
 *                       FAMILY AUDIT in q_ops.c; pure metadata this stage)
 *   doc           str   one-line help, copied verbatim from qdocs (`\h <verb>`
 *                       prints this).  EMPTY "" iff the verb is deliberately
 *                       undocumented — qdocs gives it no one-liner, or gives a
 *                       wrong one; reason in tools/qdocs/qdocs-docmap.pins.tsv.
 *   docsrc        str   the FULL qdocs ref/ page path + anchor `doc` came from
 *                       ("qdocs/docs/docs/docs/ref/next.md#prev").  "" when doc is
 *                       empty OR when doc is HAND-AUTHORED (paraphrased, cites no
 *                       page — the four `/ \ ': <>` iterators; those get no URL line
 *                       and qdocs-doccheck.sh skips them).
 *   syntax        str   the verb's one-line ```syntax form, verbatim from that page
 *                       ("x+y     +[x;y]"), "" when the page has none.
 *   example       str   a one-line `expr -> output` generated by RUNNING ./q
 *                       ("til 5 -> 0 1 2 3 4"), "" when the verb has no curated
 *                       example.  SPARSE by design (tools/qdocs/qdocs-example.tsv).
 * doc+docsrc+syntax are re-derived by tools/qdocs/qdocs-doccheck.sh (on demand, not
 * gated); example by tools/qdocs/qdocs-examplecheck.sh (in `make q-test`).
 * Strings (not syms): docs are prose, and interning 162 sentences into the
 * symbol table forever would be a leak by another name.
 * The x argument (`.Q.ops[]` passes `::`) is ignored — the table is constant
 * per build, keyed only off the static manifest. */
static const char* q_lexclass_name(q_lex_class c) {
    switch (c) {
    case QLEX_GLYPH:     return "glyph";
    case QLEX_KW_INFIX:  return "kw_infix";
    case QLEX_KW_PREFIX: return "kw_prefix";
    case QLEX_ADVERB:    return "adverb";
    }
    return "unknown";
}

/* Append `s` (NULL -> "") to a list column as a string atom.  ray_list_append
 * RETAINS, so the local ref is dropped here; it consumes `lst` on error. */
static ray_t* q_ops_append_str(ray_t* lst, const char* s) {
    if (!lst || RAY_IS_ERR(lst)) return lst;
    ray_t* v = ray_str(s ? s : "", s ? strlen(s) : 0);
    if (!v || RAY_IS_ERR(v)) { ray_release(lst); return v ? v : ray_error("wsfull", NULL); }
    lst = ray_list_append(lst, v);
    ray_release(v);
    return lst;
}

static ray_t* q_dotq_ops_fn(ray_t** args, int64_t nargs) {
    (void)args; (void)nargs;                   /* `.Q.ops[]` / `.Q.ops x` — arg ignored */
    int n = 0;
    const q_op_t* ops = q_ops_table(&n);
    int64_t cap = n > 0 ? n : 1;
    ray_t* name = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t* lexc = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t* mon  = ray_vec_new(RAY_BOOL, cap);
    ray_t* dya  = ray_vec_new(RAY_BOOL, cap);
    ray_t* det  = ray_vec_new(RAY_BOOL, cap);
    ray_t* eff  = ray_vec_new(RAY_BOOL, cap);
    ray_t* fam  = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t* doc  = ray_list_new(cap);
    ray_t* dsrc = ray_list_new(cap);
    ray_t* syn  = ray_list_new(cap);
    ray_t* exmp = ray_list_new(cap);
    ray_t* cols[11] = { name, lexc, mon, dya, det, eff, fam, doc, dsrc, syn, exmp };
    for (int i = 0; i < 11; i++)
        if (!cols[i] || RAY_IS_ERR(cols[i])) {
            for (int j = 0; j < 11; j++)
                if (cols[j] && !RAY_IS_ERR(cols[j])) ray_release(cols[j]);
            return ray_error("wsfull", ".Q.ops: out of memory");
        }
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        int64_t nm  = ray_sym_intern(ops[i].name, strlen(ops[i].name));
        const char* lc = q_lexclass_name(ops[i].lex);
        int64_t lci = ray_sym_intern(lc, strlen(lc));
        uint8_t bm = ops[i].mon.kind  != QK_NONE;
        uint8_t bd = ops[i].dyad.kind != QK_NONE;
        uint8_t bt = ops[i].deterministic ? 1 : 0;
        uint8_t be = ops[i].sideeffect ? 1 : 0;
        int64_t fmi = ray_sym_intern(ops[i].family, strlen(ops[i].family));
        name = ray_vec_append(name, &nm);
        lexc = ray_vec_append(lexc, &lci);
        mon  = ray_vec_append(mon,  &bm);
        dya  = ray_vec_append(dya,  &bd);
        det  = ray_vec_append(det,  &bt);
        eff  = ray_vec_append(eff,  &be);
        fam  = ray_vec_append(fam,  &fmi);
        /* A NULL doc/docsrc (deliberately undocumented — see q_ops.h) surfaces
         * as the empty string, so the column stays uniformly string-typed and
         * `0 = count each doc` finds the gaps. */
        doc  = q_ops_append_str(doc,  ops[i].doc);
        dsrc = q_ops_append_str(dsrc, ops[i].docsrc);
        syn  = q_ops_append_str(syn,  ops[i].syntax);
        exmp = q_ops_append_str(exmp, ops[i].example);
        if (!name || RAY_IS_ERR(name) || !lexc || RAY_IS_ERR(lexc) ||
            !mon || RAY_IS_ERR(mon) || !dya || RAY_IS_ERR(dya) ||
            !det || RAY_IS_ERR(det) || !eff || RAY_IS_ERR(eff) ||
            !fam || RAY_IS_ERR(fam) || !doc || RAY_IS_ERR(doc) ||
            !dsrc || RAY_IS_ERR(dsrc) || !syn || RAY_IS_ERR(syn) ||
            !exmp || RAY_IS_ERR(exmp)) ok = 0;
    }
    ray_t* built[11] = { name, lexc, mon, dya, det, eff, fam, doc, dsrc, syn, exmp };
    if (!ok) {
        for (int j = 0; j < 11; j++)
            if (built[j] && !RAY_IS_ERR(built[j])) ray_release(built[j]);
        return ray_error("wsfull", ".Q.ops: build failed");
    }
    static const char* colnames[11] =
        { "name", "lexclass", "monadic", "dyadic", "deterministic", "sideeffect",
          "family", "doc", "docsrc", "syntax", "example" };
    ray_t* t = ray_table_new(11);
    for (int i = 0; i < 11; i++) {
        if (!RAY_IS_ERR(t))               /* stop adding once errored... */
            t = ray_table_add_col(t, ray_sym_intern(colnames[i], strlen(colnames[i])), built[i]);
        ray_release(built[i]);            /* ...but always drop our ref (add_col retains) */
    }
    return t;
}

void q_builtins_register(void) {
    /* Noun-head application (indexing, dict/table lookup, 104h carriers):
     * register the q dispatcher into eval's apply hook.  q_runtime_destroy
     * clears it before the runtime dies. */
    ray_eval_set_apply_hook(q_apply_noun);
    /* Remote strings (IPC/journal) evaluate as q from now on. */
    ray_eval_set_remote_str_fn(q_remote_eval_str);
    /* Remote (func;args) value-apply (IPC): single value-object apply via q `value`. */
    ray_eval_set_remote_apply_fn(q_remote_apply);
    bind_unary("parse", q_parse_builtin_fn);
    /* q keywords with no rayfall counterpart — q-owned env bindings (same
     * mechanism as `parse`), snapshotted by the registry as QK_ENV rows. */
    bind_unary("string", q_string_fn);
    bind_unary("upper",  q_upper_fn);
    bind_unary("lower",  q_lower_fn);
    bind_unary("trim",   q_trim_fn);
    bind_unary("ltrim",  q_ltrim_fn);
    bind_unary("rtrim",  q_rtrim_fn);
    bind_unary("md5",    q_md5_fn);
    bind_vary ("ssr",    q_ssr_wrap);
    /* bracket-form joins (feat/q-joins-rebuild): triadic/quaternary prefix
     * keywords resolve through the env (the ssr precedent); implementations
     * in q_registry.c over the engine join machinery. */
    bind_vary ("ej",     q_ej_wrap);
    bind_vary ("aj",     q_aj_wrap);
    bind_vary ("aj0",    q_aj0_wrap);
    bind_vary ("ajf",    q_ajf_wrap);
    bind_vary ("ajf0",   q_ajf0_wrap);
    bind_vary ("wj",     q_wj_wrap);
    bind_vary ("wj1",    q_wj1_wrap);
    bind_unary("show",   q_show_fn);
    /* `system "…"` — q-owned string form; single-homes with the `\`-slash
     * dispatcher (q_sys.c). QK_ENV row in q_ops.c snapshots this binding. */
    bind_unary("system", q_system_fn);
    /* File Text companions (feat/q-file-text): `read0[(f;o)]` bracket calls
     * name-ref through the env (the ssr precedent — same C fn as the registry
     * row); `csv` is kdb's comma global (ref/file-text.md `csv 0: t`). */
    bind_unary("read0",  q_read0_wrap);
    bind_value("csv",    ray_str(",", 1));
    /* Table introspection — q-owned, snapshotted by the registry's QK_ENV rows
     * (q_ops.c) so the parser embeds these over the base env `meta`/(absent)
     * `cols`.  Bound BEFORE q_registry_init, like `string`/`show`. */
    bind_unary("meta",   q_meta_fn);
    bind_unary("cols",   q_cols_fn);
    /* `::` — the generic-null VALUE.  Elided call args parse as unquoted `::`
     * name-refs (`f[]` is (`f;::), cases.tsv:43); binding the value makes them
     * evaluate to RAY_NULL_OBJ instead of a 'name error — which is exactly
     * what lambda application detects as projection holes, and gives a typed
     * `::` its kdb value.  The funsql special forms are unaffected (their `::`
     * markers are never evaluated). */
    ray_retain(RAY_NULL_OBJ);
    bind_value("::", RAY_NULL_OBJ);
    /* Function-value introspection wrappers.  Bound BEFORE q_registry_init so
     * the registry's QK_ENV rows (`#` monadic = count) snapshot the WRAPPED
     * values — one home for the carrier special-cases. */
    uint8_t type_attrs = RAY_FN_NONE, count_attrs = RAY_FN_NONE;
    capture_base("type",  &g_base_type,  &type_attrs);
    capture_base("count", &g_base_count, &count_attrs);
    {
        ray_t* tv = ray_fn_unary("type", type_attrs, q_type_fn);
        ray_env_bind(ray_sym_intern("type", 4), tv);
        ray_release(tv);
        ray_t* cv = ray_fn_unary("count", count_attrs, q_count_fn);
        ray_env_bind(ray_sym_intern("count", 5), cv);
        ray_release(cv);
    }
    /* `value` — env-bind the q wrapper (the q_value_wrap manifest row) so a BARE `value`
     * used as a HOF operand (`value each (::;+;-;*;%)`) resolves to q semantics,
     * not rayfall's dict-only native `value`.  Application heads still embed the
     * registry copy; both call q_value_wrap (single home).  Bound before
     * q_registry_init like the other q-owned overrides. */
    bind_unary("value", q_value_wrap);
    /* `enlist` — q dict arm (1-row table); the `,` monadic QK_ENV snapshot
     * inherits it (bound before q_registry_init, q_value_wrap precedent). */
    bind_vary ("enlist", q_enlist_wrap_vary);
    /* Build q's verb table over the now-populated g_env (ray_lang_init has run).
     * The registry is the authoritative, immutable verb source; it snapshots
     * builtin values and must be torn down via q_runtime_destroy before the
     * runtime.  Fail fast: a missing audited builtin is a bug. */
    assert(q_registry_init() == RAY_OK);
    /* Monadic table verbs (feat/q-table-verbs) are REGISTRY wrappers with no
     * base env builtin behind them — but a bare keyword used as a HOF operand
     * (`flip each x`, `keys each ts`) resolves through the ENV (the q `value`
     * precedent), so bind the registry's own immutable value under the same
     * name.  Application heads still embed the registry copy; both paths call
     * ONE wrapper. */
    {
        static const char* tv_monads[] = { "flip", "keys", "ungroup" };
        for (size_t i = 0; i < sizeof tv_monads / sizeof *tv_monads; i++) {
            const char* nm = tv_monads[i];
            ray_t* v = q_registry_lookup_name(nm, strlen(nm), Q_MONADIC); /* borrowed */
            if (!v) fprintf(stderr, "q_builtins: registry miss for %s\n", nm);
            assert(v != NULL);
            ray_env_bind(ray_sym_intern(nm, strlen(nm)), v);             /* retains */
        }
    }
    /* .Q.c.* — the raw C primitives behind the .Q namespace (internal/unstable).
     * dotq.q delegates each public `.Q.<name>` to these (rule 6, loaded next) →
     * dotq.q is the single .Q manifest.  The value keeps its true `.Q.c.<name>`
     * identity, so `.Q.qt` displays <.Q.c.qt> (owner: show the implementation, it's
     * fine).  New C-backed .Q members (queued .Q.hg/.Q.hp) bind HERE, pre-bootstrap. */
    static const struct { const char* name; ray_unary_fn fn; } dotq_c_unary[] = {
        { ".Q.c.id",   q_id_fn        }, { ".Q.c.ty",   q_dotq_ty_fn   },
        { ".Q.c.qt",   q_dotq_qt_fn   }, { ".Q.c.qp",   q_dotq_qp_fn   },
        { ".Q.c.s",    q_dotq_s_fn    }, { ".Q.c.btoa", q_dotq_btoa_fn },
        { ".Q.c.atob", q_dotq_atob_fn }, { ".Q.c.sha1", q_dotq_sha1_fn },
    };
    for (size_t i = 0; i < sizeof dotq_c_unary / sizeof *dotq_c_unary; i++)
        bind_unary(dotq_c_unary[i].name, dotq_c_unary[i].fn);
    bind_vary (".Q.c.ops", q_dotq_ops_fn);   /* niladic .Q.ops[] + unary .Q.ops x */
    bind_value(".Q.c.res", q_name_reserved_words());
    /* .Q.pn stays UNBOUND (ref/dotq.md): `` `pn in key `.Q `` must be 0b — qStudio's safeCount relies on it. */
    /* .j JSON namespace (.j.j/.j.k/.j.jd) — plain env unaries, resolved as dotted name-refs. */
    q_json_register();
}
