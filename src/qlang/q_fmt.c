/* q_fmt — see q_fmt.h.  The start of a q-correct value formatter (the outputter
 * that will eventually back q's `.Q.s`).  It renders int vectors, symbol atoms
 * and general lists q-style and delegates everything else to rayforce's ray_fmt. */
#include "qlang/q_fmt.h"
#include "lang/format.h"   /* ray_fmt */
#include "table/sym.h"     /* ray_sym_vec_cell — resolve a sym-vector cell */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

/* The verb characters — a sym atom of one of these (or the generic null `::`)
 * prints bare; every other sym keeps its leading backtick.  Same split the
 * retired q_ast_fmt made between verbs/null and names/sym-literals. */
static const char Q_VERBS[] = ":+-*%!&|<>=~,^#_$?@.";

static int q_sym_bare(const char* nm, size_t l) {
    if (l == 1 && nm[0] && strchr(Q_VERBS, nm[0])) return 1;
    if (l == 2 && nm[0] == ':' && nm[1] == ':') return 1;
    return 0;
}

/* Format a -RAY_SYM atom into buf: verbs/null bare, everything else backticked. */
static void q_fmt_sym(ray_t* val, char* buf, size_t bufsz) {
    ray_t* s = ray_sym_str(val->i64);
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    snprintf(buf, bufsz, "%s%.*s", q_sym_bare(nm, l) ? "" : "`", (int)l, nm);
    ray_release(s);
}

/* rayforce's formatter, captured as a string — the fallback for values q_fmt
 * doesn't yet render q-style. */
static void ray_fallback(ray_t* val, char* buf, size_t bufsz) {
    ray_t* s = ray_fmt(val, 0);
    if (s && !RAY_IS_ERR(s) && s->type == -RAY_STR) {
        size_t n = ray_str_len(s);
        if (n >= bufsz) n = bufsz - 1;
        memcpy(buf, ray_str_ptr(s), n);
        buf[n] = '\0';
    }
    if (s && !RAY_IS_ERR(s)) ray_release(s);
}

/* Render one integer element q-style with sentinel detection:
 * INT*_MIN -> 0N, INT*_MAX -> 0W, -INT*_MAX -> -0W, else the value; the type
 * suffix (h/i, or none for long) is appended. */
static void q_int_tok(int64_t v, int width, char suffix, char* out, size_t n) {
    int64_t vmin = (width == 2) ? INT16_MIN : (width == 4) ? INT32_MIN : INT64_MIN;
    int64_t vmax = (width == 2) ? INT16_MAX : (width == 4) ? INT32_MAX : INT64_MAX;
    char sfx[2]; sfx[0] = suffix; sfx[1] = '\0';
    if (!suffix) sfx[0] = '\0';
    if (v == vmin)       snprintf(out, n, "0N%s", sfx);
    else if (v == vmax)  snprintf(out, n, "0W%s", sfx);
    else if (v == -vmax) snprintf(out, n, "-0W%s", sfx);
    else                 snprintf(out, n, "%lld%s", (long long)v, sfx);
}

/* Render one float element q-style: NaN -> 0n (f64) / 0Ne (f32); a finite
 * magnitude reuses ray_fmt on a temp atom of the matching type, then appends
 * 'e' for f32 (nothing for f64).  Float infinities are out of scope. */
static void q_float_tok(double v, int f32, char* out, size_t n) {
    if (isnan(v)) { snprintf(out, n, f32 ? "0Ne" : "0n"); return; }
    ray_t* a = f32 ? ray_f32((float)v) : ray_f64(v);
    char mag[64]; mag[0] = '\0';
    ray_t* s = ray_fmt(a, 0);
    if (s && !RAY_IS_ERR(s) && s->type == -RAY_STR) {
        size_t l = ray_str_len(s);
        if (l >= sizeof mag) l = sizeof mag - 1;
        memcpy(mag, ray_str_ptr(s), l);
        mag[l] = '\0';
    }
    if (s && !RAY_IS_ERR(s)) ray_release(s);
    if (a && !RAY_IS_ERR(a)) ray_release(a);
    snprintf(out, n, "%.48s%s", mag, f32 ? "e" : "");
}

/* Join per-element tokens (already rendered) with spaces into buf. */
static void q_join(char* buf, size_t bufsz, size_t* pos, const char* tok, int first) {
    size_t tl = strlen(tok);
    size_t need = tl + (first ? 0 : 1);
    if (*pos + need + 1 > bufsz) return;
    if (!first) buf[(*pos)++] = ' ';
    memcpy(buf + *pos, tok, tl);
    *pos += tl;
    buf[*pos] = '\0';
}

void q_fmt(ray_t* val, char* buf, size_t bufsz) {
    if (bufsz == 0) return;
    buf[0] = '\0';
    if (!val) return;

    /* A symbol atom prints q-style: `sym` (or bare for a verb/null name-ref).
     * Handling it here also renders the -RAY_SYM heads of parse-tree lists. */
    if (val->type == -RAY_SYM) {
        q_fmt_sym(val, buf, bufsz);
        return;
    }

    /* Typed numeric atoms print q-style with a type suffix (bare for long /
     * float): 1b, 42h, 42i, 42, 3.14e, 3.14 — plus the nulls/inf tokens. */
    switch (val->type) {
    case -RAY_BOOL: snprintf(buf, bufsz, "%db", val->u8 ? 1 : 0);          return;
    case -RAY_I16:  q_int_tok((int64_t)val->i16, 2, 'h', buf, bufsz);      return;
    case -RAY_I32:  q_int_tok((int64_t)val->i32, 4, 'i', buf, bufsz);      return;
    case -RAY_I64:  q_int_tok(val->i64,          8, 0,   buf, bufsz);      return;
    case -RAY_F32:  q_float_tok((float)val->f64, 1, buf, bufsz);           return;
    case -RAY_F64:  q_float_tok(val->f64,        0, buf, bufsz);           return;
    default: break;
    }

    /* q prints a simple vector space-separated with NO brackets: `5 6 7`,
     * where rayforce prints `[5 6 7]`.  Booleans concatenate with no spaces
     * plus one trailing `b` (1001b). */
    if (val->type == RAY_BOOL) {
        int64_t n = ray_len(val);
        const uint8_t* d = (const uint8_t*)ray_data(val);
        size_t pos = 0;
        for (int64_t i = 0; i < n && pos + 1 < bufsz; i++)
            buf[pos++] = d[i] ? '1' : '0';
        if (pos + 1 < bufsz) buf[pos++] = 'b';
        buf[pos] = '\0';
        return;
    }
    /* Typed integer vector: each element is rendered BARE (no per-element type
     * char), space-joined, then the type char is appended ONCE at the end —
     * `0N 0W -0W 42h`, not `0Nh 0Wh -0Wh 42h`.  This matches kdb's own
     * formatter (K.java KBaseVector.formatVector: child context showType=false,
     * one trailing typeChar). Long's type char is empty. */
    if (val->type == RAY_I16 || val->type == RAY_I32 || val->type == RAY_I64) {
        int width  = (val->type == RAY_I16) ? 2 : (val->type == RAY_I32) ? 4 : 8;
        char vsuf  = (val->type == RAY_I16) ? 'h' : (val->type == RAY_I32) ? 'i' : 0;
        int64_t n = ray_len(val);
        size_t pos = 0;
        buf[0] = '\0';
        for (int64_t i = 0; i < n; i++) {
            char e[64];
            int64_t v = (width == 2) ? (int64_t)((const int16_t*)ray_data(val))[i]
                      : (width == 4) ? (int64_t)((const int32_t*)ray_data(val))[i]
                      :                ((const int64_t*)ray_data(val))[i];
            q_int_tok(v, width, 0, e, sizeof e);   /* bare — no per-element suffix */
            q_join(buf, bufsz, &pos, e, i == 0);
        }
        if (vsuf && pos + 1 < bufsz) { buf[pos++] = vsuf; buf[pos] = '\0'; }
        return;
    }
    /* Float/real vectors are deferred (float infinities out of scope); the
     * per-element tokens still carry the suffix — revisit with the same
     * bare-element + one-trailing-char rule when float vectors are un-deferred. */
    if (val->type == RAY_F32 || val->type == RAY_F64) {
        int64_t n = ray_len(val);
        size_t pos = 0;
        buf[0] = '\0';
        for (int64_t i = 0; i < n; i++) {
            char e[64];
            if (val->type == RAY_F32) q_float_tok((double)((const float*)ray_data(val))[i], 1, e, sizeof e);
            else                      q_float_tok(((const double*)ray_data(val))[i],        0, e, sizeof e);
            q_join(buf, bufsz, &pos, e, i == 0);
        }
        return;
    }

    /* Symbol vector: backtick-joined, `a`b`c.  Each cell resolves through the
     * vector's own domain.  EVERY element keeps its leading backtick — a sym
     * vector is data, so a verb/null-named element (`+`, `::`) must still round-
     * trip as a symbol literal, not a bare q token.  (The bare-verb rendering
     * in q_fmt_sym is only for a -RAY_SYM ATOM standing as a parse-tree head.)
     * The null symbol is a zero-length name and prints as a bare backtick. */
    if (val->type == RAY_SYM) {
        int64_t n = ray_len(val);
        size_t pos = 0;
        buf[0] = '\0';
        for (int64_t i = 0; i < n; i++) {
            ray_t* s = ray_sym_vec_cell(val, i);   /* borrowed -RAY_STR */
            const char* nm = ray_str_ptr(s);
            size_t l = ray_str_len(s);
            if (pos + 1 + l + 1 > bufsz) break;
            buf[pos++] = '`';
            memcpy(buf + pos, nm, l); pos += l;
            buf[pos] = '\0';
        }
        return;
    }

    /* A general list renders q-style as (a;b;c), recursively.  This is how a
     * parse tree prints: `parse "2+3"` -> (+;2;3). */
    if (val->type == RAY_LIST) {
        size_t pos = 0;
        if (pos + 1 < bufsz) buf[pos++] = '(';
        int64_t n = ray_len(val);
        ray_t** e = (ray_t**)ray_data(val);
        for (int64_t i = 0; i < n; i++) {
            if (i && pos + 1 < bufsz) buf[pos++] = ';';
            char elem[1024];
            elem[0] = '\0';
            q_fmt(e[i], elem, sizeof elem);      /* recurse (syms handled above) */
            size_t el = strlen(elem);
            if (pos + el + 1 >= bufsz) el = (bufsz > pos + 1) ? bufsz - 1 - pos : 0;
            memcpy(buf + pos, elem, el);
            pos += el;
        }
        if (pos + 1 < bufsz) buf[pos++] = ')';
        buf[pos] = '\0';
        return;
    }

    ray_fallback(val, buf, bufsz);
}
