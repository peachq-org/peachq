/* q_fmt — see q_fmt.h.  The start of a q-correct value formatter (the outputter
 * that will eventually back q's `.Q.s`).  It renders int vectors, symbol atoms
 * and general lists q-style and delegates everything else to rayforce's ray_fmt. */
#include "qlang/q_fmt.h"
#include "qlang/q_registry.h" /* q_registry_list_value — hidden literal head */
#include "qlang/q_deriv.h"    /* q_deriv_kind_of — 104h carrier display */
#include "lang/format.h"   /* ray_fmt */
#include "lang/eval.h"     /* ray_at_fn — dict display element access */
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
    /* monadic-marked verbs print bare too: +: #: |: — and :: (null/assign) */
    if (l == 2 && nm[1] == ':' && nm[0] && strchr(Q_VERBS, nm[0])) return 1;
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
    /* Finite whole-number magnitude prints WITHOUT a fractional part (q shows
     * `5`, not `5.0`); f32 keeps its per-element `e`.  The disambiguating `f`
     * for f64 wholes is appended by the caller (atom) or once per vector. */
    if (isfinite(v) && v == floor(v) && v >= -9.007199254740992e15
                                     && v <=  9.007199254740992e15) {
        snprintf(out, n, "%lld%s", (long long)v, f32 ? "e" : "");
        return;
    }
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

    /* Registry function VALUES render their q spelling from provenance
     * (formatter-from-metadata, spec piece 2): glyph rows bare (`+`), monadic
     * glyph rows with the marker (`-:`), keyword rows the keyword.  Values
     * with no provenance (internal list/scan wrappers, foreign fns) fall
     * through to ray_fallback below. */
    if (val->type == RAY_UNARY || val->type == RAY_BINARY ||
        val->type == RAY_VARY) {
        q_provenance_t pv;
        if (q_registry_provenance(val, &pv) && pv.spelling && pv.spelling[0]) {
            char c0 = pv.spelling[0];
            int glyph = !((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'));
            if (pv.valence == Q_MONADIC && glyph)
                snprintf(buf, bufsz, "%s:", pv.spelling);
            else
                snprintf(buf, bufsz, "%s", pv.spelling);
            return;
        }
    }

    /* Typed numeric atoms print q-style with a type suffix (bare for long /
     * float): 1b, 42h, 42i, 42, 3.14e, 3.14 — plus the nulls/inf tokens. */
    switch (val->type) {
    case -RAY_BOOL: snprintf(buf, bufsz, "%db", val->u8 ? 1 : 0);          return;
    case -RAY_I16:  q_int_tok((int64_t)val->i16, 2, 'h', buf, bufsz);      return;
    case -RAY_I32:  q_int_tok((int64_t)val->i32, 4, 'i', buf, bufsz);      return;
    case -RAY_I64:  q_int_tok(val->i64,          8, 0,   buf, bufsz);      return;
    case -RAY_F32:  q_float_tok((float)val->f64, 1, buf, bufsz);           return;
    case -RAY_F64: {
        /* A whole f64 atom gets a trailing `f` to distinguish it from a long
         * (`5f`, not `5`); a fractional one (`3.14`) needs no suffix. */
        q_float_tok(val->f64, 0, buf, bufsz);
        if (isfinite(val->f64) && val->f64 == floor(val->f64)) {
            size_t l = strlen(buf);
            if (l + 1 < bufsz) { buf[l] = 'f'; buf[l + 1] = '\0'; }
        }
        return;
    }
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
        int is64 = (val->type == RAY_F64);
        int64_t n = ray_len(val);
        size_t pos = 0;
        buf[0] = '\0';
        int all_whole = (n > 0);   /* f64 gets ONE trailing `f` iff every element is finite & whole */
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(val))[i]
                            : (double)((const float*)ray_data(val))[i];
            char e[64];
            q_float_tok(v, is64 ? 0 : 1, e, sizeof e);
            q_join(buf, bufsz, &pos, e, i == 0);
            if (!(isfinite(v) && v == floor(v))) all_whole = 0;
        }
        /* f32 vectors already carry a per-element `e` (kdb records e.g.
         * `0Ne 0We -0We 3.14e`); only f64 wholes take the single trailing `f`
         * (`1 2 3f`).  A vector with any fractional or non-finite element
         * (`0.5 1 1.5`, `0n 0w -0w 3.14`) gets no suffix. */
        if (is64 && all_whole && pos + 1 < bufsz) { buf[pos++] = 'f'; buf[pos] = '\0'; }
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

    /* A 104h derived-verb carrier renders as bound-verb + adverb glyph (+/).
     * The carrier's base is the HOF value (fold / the internal scan wrapper /
     * the each wrapper); the bound verb sits in the first arg slot. */
    if (q_deriv_kind_of(val) == Q_DERIV_PROJ && ray_len(val) >= 5) {
        ray_t* base = q_deriv_base(val);
        ray_t* v0   = ((ray_t**)ray_data(val))[4];
        const char* g = NULL;
        if (base == ray_env_get(ray_sym_intern("fold", 4)))            g = "/";
        else if (base == q_registry_scan_value())                      g = "\\";
        else if (base == q_registry_lookup_name("each", 4, Q_DYADIC))  g = "'";
        if (g && v0) {
            char vb[256]; vb[0] = '\0';
            q_fmt(v0, vb, sizeof vb);
            snprintf(buf, bufsz, "%s%s", vb, g);
            return;
        }
    }

    /* kdb dict display: one `key| value` row per entry, keys padded to the
     * widest key (kdb pads the key column, then "| ").  Keys print BARE
     * (`a| 10`, no backtick); values recurse through q_fmt.  Two passes:
     * first measures the key column, second emits. */
    if (val->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(val);          /* borrowed */
        ray_t* v = ray_dict_vals(val);          /* borrowed */
        int64_t n = k ? ray_len(k) : 0;
        size_t pos = 0, maxk = 0;
        for (int pass = 0; pass < 2; pass++) {
            for (int64_t i = 0; i < n; i++) {
                ray_t* ia = ray_i64(i);
                ray_t* ke = ray_at_fn(k, ia);
                ray_release(ia);
                char kb[256]; kb[0] = '\0';
                if (ke && !RAY_IS_ERR(ke)) {
                    if (ke->type == -RAY_SYM) {     /* bare, never backticked */
                        ray_t* s = ray_sym_str(ke->i64);
                        if (s) {
                            snprintf(kb, sizeof kb, "%.*s",
                                     (int)ray_str_len(s), ray_str_ptr(s));
                            ray_release(s);
                        }
                    } else {
                        q_fmt(ke, kb, sizeof kb);
                    }
                }
                if (ke && !RAY_IS_ERR(ke)) ray_release(ke);
                size_t kl = strlen(kb);
                if (pass == 0) {
                    if (kl > maxk) maxk = kl;
                    continue;
                }
                char vb[1024]; vb[0] = '\0';
                ray_t* ja = ray_i64(i);
                ray_t* ve = v ? ray_at_fn(v, ja) : NULL;
                ray_release(ja);
                if (ve && !RAY_IS_ERR(ve)) q_fmt(ve, vb, sizeof vb);
                if (ve && !RAY_IS_ERR(ve)) ray_release(ve);
                size_t need = (i ? 1 : 0) + maxk + 2 + strlen(vb);
                if (pos + need + 1 > bufsz) break;
                pos += (size_t)snprintf(buf + pos, bufsz - pos, "%s%-*s| %s",
                                        i ? "\n" : "", (int)maxk, kb, vb);
            }
        }
        buf[pos < bufsz ? pos : bufsz - 1] = '\0';
        return;
    }

    /* A general list of strings (kdb: char vectors) or of typed VECTORS
     * (cut output) prints one element per line, not (a;b) — parse trees
     * cannot hit this: their heads are atoms/syms/values, never vectors. */
    if (val->type == RAY_LIST && ray_len(val) >= 2) {
        int64_t n = ray_len(val);
        ray_t** e = (ray_t**)ray_data(val);
        int allstr = 1, allvec = 1;
        for (int64_t i = 0; i < n && (allstr || allvec); i++) {
            if (!e[i] || e[i]->type != -RAY_STR) allstr = 0;
            if (!e[i] || e[i]->type <= 0 || !ray_is_vec(e[i])) allvec = 0;
        }
        if (allstr || allvec) {
            size_t pos = 0;
            for (int64_t i = 0; i < n; i++) {
                char elem[1024]; elem[0] = '\0';
                q_fmt(e[i], elem, sizeof elem);
                size_t el = strlen(elem);
                if (pos + el + 2 >= bufsz) break;
                if (i) buf[pos++] = '\n';
                memcpy(buf + pos, elem, el);
                pos += el;
            }
            buf[pos] = '\0';
            return;
        }
    }

    /* A general list renders q-style as (a;b;c), recursively.  This is how a
     * parse tree prints: `parse "2+3"` -> (+;2;3).  A literal-constructor
     * head (the parser's paren-list marker) is HIDDEN: the tree for `(1;2;3)`
     * is ((list);1;2;3) but must display (1;2;3). */
    if (val->type == RAY_LIST) {
        size_t pos = 0;
        if (pos + 1 < bufsz) buf[pos++] = '(';
        int64_t n = ray_len(val);
        ray_t** e = (ray_t**)ray_data(val);
        ray_t* lv = q_registry_list_value();
        if (lv && n >= 1 && e[0] == lv) { e++; n--; }   /* skip hidden head */
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
