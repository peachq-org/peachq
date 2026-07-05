/* q_parse — q source -> rayforce ray_t AST.
 *
 * Ported from kparser (Apache-2.0, https://github.com/ardentsia-cgs/kparser).
 * The scanner + recursive-descent parser control flow is preserved verbatim
 * in spirit (noun_pos sign disambiguation, the nve/te one-term-lookahead
 * split); only the value layer changed: every K becomes a ray_t*, and
 * refcounting is rayforce's retain/release (born rc=1, ray_list_append
 * retains, so each freshly-built child is released after append — the same
 * discipline as src/lang/parse.c:parse_list).
 *
 * AST shapes (heads are rayforce name-reference syms, so eval resolves them):
 *   n v e   -> (v; n; e)
 *   t e     -> (t; e)         (lone term collapses to t)
 *   t[E]    -> (t; e1; ...)
 *   {E}     -> (`{; body)
 *   tA      -> (`A; t)
 */

#define _POSIX_C_SOURCE 200809L

#include "qlang/q_parse.h"
#include "qlang/q_registry.h" /* q_registry_lookup_name, Q_DYADIC */
#include "qlang/q_ops.h"      /* q_lex_is_kw_infix — static lexical manifest */
#include "qlang/q_deriv.h"    /* q_proj_new — 104h derived-verb carriers */
#include "lang/env.h"        /* ray_fn_name — qSQL output-expr head normalize */
#include "table/sym.h"       /* ray_sym_vec_cell — qSQL dict-key/col names */
#include "core/numparse.h"   /* ray_parse_i64, ray_parse_f64 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>

/* ATTR_QUOTED: flag on a -RAY_SYM atom.  SET = literal symbol; CLEAR
 * (default) = name reference, resolved at eval.  Mirrors the definition in
 * src/lang/eval.h (0x20) — kept local so the parser needs no eval header. */
#define Q_ATTR_QUOTED 0x20

#define MAX_VEC  4096
#define MAX_NAME 256

/* ===== parse-error escape =====================================================
 * kparser die()s (exit) on malformed input.  A REPL must not exit, so a
 * malformed parse longjmps back to q_parse, which returns a ray_error. */
static jmp_buf q_err_jmp;
static char    q_err_buf[128];

static void q_die(const char *msg) {
    snprintf(q_err_buf, sizeof q_err_buf, "%s", msg);
    longjmp(q_err_jmp, 1);
}

/* ===== ray_t leaf builders =================================================== */

/* name reference (ATTR_QUOTED clear): resolved by eval */
static ray_t *q_name(const char *s, int len) {
    return ray_sym(ray_sym_intern_runtime(s, (size_t)len));
}

/* verb: a name-ref sym of the verb char, e.g. "+", ":".  Monadic vs dyadic
 * is a valence decision left to eval, so we drop kparser's KV1/KV2 split. */
static ray_t *q_verb(char c) {
    char b[2] = { c, '\0' };
    return ray_sym(ray_sym_intern_runtime(b, 1));
}

/* verb by name (multi-char glyph like "<=", "<>", or keyword like "div"):
 * a name-ref sym, ATTR_QUOTED clear; the parser embeds its registry value. */
static ray_t *q_verb_name(const char *s, int len) {
    return ray_sym(ray_sym_intern_runtime(s, (size_t)len));
}

/* Infix keyword verbs (q keyword functions usable between two nouns).  Derived
 * from the SINGLE-SOURCE op manifest (q_ops.c QLEX_KW_INFIX rows) — no longer a
 * hardcoded memcmp, and no runtime-registry dependency (the manifest is a
 * static table, and the scanner runs before eval).  Classification is
 * byte-identical to the retired memcmp: the manifest's KW_INFIX set is {div}. */
static int q_is_kw_verb(const char *s, int len) {
    return q_lex_is_kw_infix(s, len);
}

/* marker heads ("{", ";", adverb names): name-ref syms */
static ray_t *q_marker(const char *s) {
    return ray_sym(ray_sym_intern_runtime(s, strlen(s)));
}

/* Embed the registry function VALUE for a verb sym at the given valence — the
 * 2b parser flip (`parse "2+3"` -> (+<fn>;2;3)).  A monadic-marked spelling
 * ("<g>:") probes the registry under the bare glyph.  On a miss the sym is
 * returned unchanged (unknown -> name-ref, ADR 0002).  Consumes `sym`,
 * returns owned. */
static ray_t *q_embed(ray_t *sym, q_valence_t val) {
    if (!sym || sym->type != -RAY_SYM || (sym->attrs & Q_ATTR_QUOTED)) return sym;
    ray_t *s = ray_sym_str(sym->i64);
    if (!s) return sym;
    const char *nm = ray_str_ptr(s);
    size_t nl = ray_str_len(s);
    if (val == Q_MONADIC && nl == 2 && nm[1] == ':') nl = 1;   /* "+:" -> "+" */
    ray_t *hit = q_registry_lookup_name(nm, nl, val);
    ray_release(s);
    if (!hit) return sym;
    ray_retain(hit);
    ray_release(sym);
    return hit;
}

/* True iff a sym spells a glyph verb (1 char from VERB_CHARS, optionally with
 * the monadic marker) — used to keep bare-verb embedding away from user
 * names, which must stay env-resolved name-refs. */
static int q_sym_is_glyph(ray_t *sym);   /* defined after VERB_CHARS */

/* generic null :: — the elided-argument hole */
static ray_t *q_null(void) {
    return ray_sym(ray_sym_intern_runtime("::", 2));
}

/* symbol literal (ATTR_QUOTED set) */
static ray_t *q_symlit(const char *s, int len) {
    ray_t *x = ray_sym(ray_sym_intern_runtime(s, (size_t)len));
    if (x && !RAY_IS_ERR(x)) x->attrs |= Q_ATTR_QUOTED;
    return x;
}

/* Append one interned symbol id into a RAY_SYM vector (W64 index width: no
 * public width-picker, and correctness beats compactness for literals). */
static ray_t *q_symvec_append(ray_t *vec, const char *s, int len) {
    int64_t id = ray_sym_intern_runtime(s, (size_t)len);
    return ray_vec_append(vec, &id);
}

/* Build a ray list from n owned children, releasing each after append
 * (append retains).  A C-NULL child (an empty operand, e.g. the value of
 * `()`, or a missing element) is normalised to q_null() so it never reaches
 * ray_eval as a bare C NULL — ray_eval asserts value-nulls are RAY_NULL_OBJ,
 * not C NULL.  The top-level program list is built separately in parse_E,
 * which DOES preserve C NULL (an empty statement / whole-line comment is a
 * no-op that must yield no output, not `::`). */
static ray_t *q_list(ray_t **xs, int n) {
    ray_t *l = ray_list_new(n > 0 ? n : 1);
    for (int i = 0; i < n; i++) {
        if (xs[i]) {
            l = ray_list_append(l, xs[i]);
            ray_release(xs[i]);
        } else {
            ray_t *nul = q_null();
            l = ray_list_append(l, nul);
            ray_release(nul);
        }
    }
    return l;
}

/* ===== verb / adverb tables ================================================== */

static const char VERB_CHARS[] = ":+-*%!&|<>=~,^#_$?@.";

static const char *ADVERB_NAMES[] = { "'", "/", "\\", "':", "/:", "\\:" };

static int q_sym_is_glyph(ray_t *sym) {
    if (!sym || sym->type != -RAY_SYM || (sym->attrs & Q_ATTR_QUOTED)) return 0;
    ray_t *s = ray_sym_str(sym->i64);
    if (!s) return 0;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int r = (l >= 1 && l <= 2 && strchr(VERB_CHARS, nm[0]) != NULL &&
             (l == 1 || nm[1] == ':' || strchr(VERB_CHARS, nm[1]) != NULL));
    ray_release(s);
    return r;
}

/* ===== char classes (copied from kparser) ==================================== */

enum {
    CL_DIGIT  = 1 << 0,
    CL_ALPHA  = 1 << 1,
    CL_VERB   = 1 << 2,
    CL_ADVERB = 1 << 3,
    CL_WS     = 1 << 4,
};

static uint8_t CLASS[256];

static void init_class(void) {
    for (int c = '0'; c <= '9'; c++) CLASS[c] |= CL_DIGIT;
    for (int c = 'a'; c <= 'z'; c++) CLASS[c] |= CL_ALPHA;
    for (int c = 'A'; c <= 'Z'; c++) CLASS[c] |= CL_ALPHA;
    CLASS[(int)'_'] |= CL_ALPHA;
    for (const char *p = VERB_CHARS; *p; p++) CLASS[(uint8_t)*p] |= CL_VERB;
    CLASS[(int)'\''] |= CL_ADVERB;
    CLASS[(int)'/']  |= CL_ADVERB;
    CLASS[(int)'\\'] |= CL_ADVERB;
    CLASS[(int)' ']  |= CL_WS;
    CLASS[(int)'\t'] |= CL_WS;
}

/* ===== scanner =============================================================== */

typedef enum {
    T_EOF,
    T_NOUN,
    T_VERB,
    T_ADVERB,
    T_LPAREN, T_RPAREN,
    T_LBRACE, T_RBRACE,
    T_LBRACK, T_RBRACK,
    T_SEMI
} TKind;

typedef struct {
    TKind  kind;
    int    start, len;
    ray_t *k;      /* owned by the token until the parser lifts it (sets NULL) */
} Token;

typedef struct { Token *t; int n; } Tokens;

/* Live token buffer, kept in a static so a q_die() longjmp (which unwinds past
 * the normal free_tokens call) can still release it — otherwise a malformed
 * input leaks the token array.  Updated as the scanner emits. */
static Tokens g_toks = { NULL, 0 };

/* ===== numeric-literal scanner (q datatypes) ================================
 * One literal is a space-separated run of magnitudes followed by at most one
 * trailing type letter (b/h/i/j/e/f), per qlang.g4: the magnitude is scanned
 * uniformly, the trailing letter fixes the type (no letter => long, or float
 * if any magnitude was fractional).  Nulls / integer infinities are Specials
 * that widen to the chosen type's sentinel. */

typedef enum { EL_INT, EL_FLOAT, EL_NULL, EL_PINF, EL_NINF } el_kind;
typedef struct { el_kind kind; int64_t i; double f; int forces_float; } num_el;

/* q Specials: 0N/0n (null), 0W/0w (+inf), -0W/-0w (-inf).  Lowercase forces a
 * float context.  Returns bytes consumed (0 = not a Special). */
static int scan_special(const char *s, int p, num_el *out) {
    int neg = (s[p] == '-');
    int q = p + (neg ? 1 : 0);
    if (s[q] != '0') return 0;
    char k = s[q + 1];
    if (k != 'N' && k != 'W' && k != 'n' && k != 'w') return 0;
    int is_null = (k == 'N' || k == 'n');
    if (neg && is_null) return 0;            /* -0N is not a literal */
    out->forces_float = (k == 'n' || k == 'w');
    out->i = 0; out->f = 0.0;
    out->kind = is_null ? EL_NULL : (neg ? EL_NINF : EL_PINF);
    return (q + 2) - p;
}

/* Scan one magnitude at src[*p] into *out; return 1 on success, 0 on no match. */
static int scan_one_num(const char *src, int *p, num_el *out) {
    out->forces_float = 0;
    int used = scan_special(src, *p, out);
    if (used) { *p += used; return 1; }

    /* Decide float vs int: a float magnitude contains '.' or an exponent among
     * its own bytes (before the next whitespace / letter).  Peek the digit run. */
    int q = *p;
    if (src[q] == '-' || src[q] == '+') q++;
    int is_float = 0, saw_digit = 0;
    for (int r = q; ; r++) {
        char c = src[r];
        if (c >= '0' && c <= '9') { saw_digit = 1; continue; }
        if (c == '.') { is_float = 1; continue; }
        if ((c == 'e' || c == 'E') && saw_digit &&
            (src[r + 1] == '+' || src[r + 1] == '-' ||
             (src[r + 1] >= '0' && src[r + 1] <= '9'))) { is_float = 1; continue; }
        break;
    }
    if (!saw_digit) return 0;

    size_t rem = strlen(src + *p);
    if (is_float) {
        double v; size_t u = ray_parse_f64(src + *p, rem, &v);
        if (u == 0) return 0;
        *p += (int)u; out->kind = EL_FLOAT; out->f = v; out->forces_float = 1;
        return 1;
    }
    int64_t v; size_t u = ray_parse_i64(src + *p, rem, &v);
    if (u == 0) return 0;
    *p += (int)u; out->kind = EL_INT; out->i = v;
    return 1;
}

/* Widen the long sentinels/inf to a narrow int width (2 or 4 bytes). */
static int64_t narrow_special(el_kind k, int width) {
    int64_t vmin = (width == 2) ? INT16_MIN : (width == 4) ? INT32_MIN : INT64_MIN;
    int64_t vmax = (width == 2) ? INT16_MAX : (width == 4) ? INT32_MAX : INT64_MAX;
    if (k == EL_NULL) return vmin;
    if (k == EL_PINF) return vmax;
    return -vmax;   /* EL_NINF */
}

/* Resolve one element to an int64 in the given integer width. */
static int64_t el_to_int(const num_el *e, int width) {
    if (e->kind == EL_INT)   return e->i;
    if (e->kind == EL_FLOAT) return (int64_t)e->f;
    return narrow_special(e->kind, width);
}

/* Resolve one element to a double (float context). */
static double el_to_float(const num_el *e) {
    if (e->kind == EL_NULL) return NULL_F64;
    if (e->kind == EL_PINF || e->kind == EL_NINF)
        q_die("q float infinity unsupported (deferred)");
    return (e->kind == EL_FLOAT) ? e->f : (double)e->i;
}

/* Read an optional trailing type letter (b/h/i/j/e/f) at src[*p].  The whole
 * literal shares one type, but each element in the source may carry the suffix
 * (q prints `0Nh 0Wh -0Wh 42h`), so we accept a letter after every element and
 * require them to agree. */
static void read_type_letter(const char *src, int *p, char *letter) {
    char c = src[*p];
    if (c && strchr("bhijef", c)) {
        if (*letter && *letter != c) q_die("inconsistent numeric type suffix");
        *letter = c;
        (*p)++;
    }
}

/* Scan a full numeric literal (atom or vector) starting at src[*p]. */
static ray_t *scan_num_literal(const char *src, int *p) {
    int start = *p;
    num_el buf[MAX_VEC]; int m = 0;
    char letter = 0;
    if (!scan_one_num(src, p, &buf[m++])) q_die("bad number");
    read_type_letter(src, p, &letter);
    for (;;) {
        int sp = *p;
        while (CLASS[(uint8_t)src[sp]] & CL_WS) sp++;
        if (sp == *p) break;                     /* no space => run ended */
        num_el e; int q = sp;
        if (!scan_one_num(src, &q, &e)) break;   /* not another magnitude */
        if (m >= MAX_VEC) q_die("numeric literal too long");
        *p = q; buf[m++] = e;
        read_type_letter(src, p, &letter);
    }

    /* Booleans: a 0/1 run ending in 'b' (spaces flattened). */
    if (letter == 'b') {
        uint8_t bits[MAX_VEC]; int nb = 0;
        for (int i = start; i < *p - 1; i++) {
            if (src[i] == ' ' || src[i] == '\t') continue;
            if (src[i] != '0' && src[i] != '1') q_die("bad boolean literal");
            if (nb >= MAX_VEC) q_die("boolean literal too long");
            bits[nb++] = (uint8_t)(src[i] - '0');
        }
        if (nb == 0) q_die("bad boolean literal");
        if (nb == 1) return ray_bool(bits[0]);
        return ray_vec_from_raw(RAY_BOOL, bits, nb);
    }

    /* Float context: explicit e/f letter, or any fractional/lowercase-special. */
    int is_float = (letter == 'e' || letter == 'f');
    for (int i = 0; i < m && !is_float; i++)
        if (buf[i].forces_float) is_float = 1;

    if (is_float) {
        int f32 = (letter == 'e');
        if (m == 1) {
            double v = el_to_float(&buf[0]);
            return f32 ? ray_f32((float)v) : ray_f64(v);
        }
        int8_t type = f32 ? RAY_F32 : RAY_F64;
        ray_t *vec;
        if (f32) {
            float t[MAX_VEC];
            for (int i = 0; i < m; i++) t[i] = (float)el_to_float(&buf[i]);
            vec = ray_vec_from_raw(type, t, m);
        } else {
            double t[MAX_VEC];
            for (int i = 0; i < m; i++) t[i] = el_to_float(&buf[i]);
            vec = ray_vec_from_raw(type, t, m);
        }
        if (vec && !RAY_IS_ERR(vec)) {
            for (int i = 0; i < m; i++)
                if (buf[i].kind == EL_NULL) ray_vec_set_null(vec, i, true);
        }
        return vec;
    }

    /* Integer context: h=i16, i=i32, j/none=i64. */
    int width = (letter == 'h') ? 2 : (letter == 'i') ? 4 : 8;
    int8_t type = (width == 2) ? RAY_I16 : (width == 4) ? RAY_I32 : RAY_I64;
    if (m == 1) {
        int64_t v = el_to_int(&buf[0], width);
        if (width == 2) return ray_i16((int16_t)v);
        if (width == 4) return ray_i32((int32_t)v);
        return ray_i64(v);
    }
    ray_t *vec;
    if (width == 8) {
        int64_t t[MAX_VEC];
        for (int i = 0; i < m; i++) t[i] = el_to_int(&buf[i], 8);
        vec = ray_vec_from_raw(RAY_I64, t, m);
    } else if (width == 4) {
        int32_t t[MAX_VEC];
        for (int i = 0; i < m; i++) t[i] = (int32_t)el_to_int(&buf[i], 4);
        vec = ray_vec_from_raw(RAY_I32, t, m);
    } else {
        int16_t t[MAX_VEC];
        for (int i = 0; i < m; i++) t[i] = (int16_t)el_to_int(&buf[i], 2);
        vec = ray_vec_from_raw(RAY_I16, t, m);
    }
    if (vec && !RAY_IS_ERR(vec) && type == RAY_I64) {
        for (int i = 0; i < m; i++)
            if (buf[i].kind == EL_NULL) ray_vec_set_null(vec, i, true);
    }
    return vec;
}

static Tokens scan(const char *src) {
    Token *toks = NULL;
    int n = 0, cap = 0;
    int p = 0;
    int noun_pos = 0;

/* NOTE: g_toks.t is updated IMMEDIATELY after the realloc — the token payload
 * KK is evaluated as part of the following statement and may q_die (e.g. the
 * deferred `0we` float-infinity literal); the longjmp handler must see the
 * live buffer, not a stale/NULL pointer (leak, or free of a moved block).
 * g_toks.n stays at the OLD count until the slot is actually stored. */
#define EMIT(TK, KK) do { \
        if (n >= cap) { cap = cap ? cap * 2 : 32; toks = realloc(toks, (size_t)cap * sizeof(Token)); \
                        if (!toks) q_die("out of memory"); /* g_toks still tracks the old block */ \
                        g_toks.t = toks; } \
        toks[n++] = (Token){ .kind = (TK), .start = start, .len = p - start, .k = (KK) }; \
        g_toks.t = toks; g_toks.n = n; \
    } while (0)

    for (;;) {
        int ws0 = p;
        while (CLASS[(uint8_t)src[p]] & CL_WS) p++;
        if (!src[p]) break;

        /* A '/' is a comment iff it has whitespace before it or starts the
         * line/input; otherwise it is an adverb (e.g. `2+3/x`).  Leading
         * whitespace — not trailing — is what matters.  (q_parse is single-
         * line today; the `src[p-1]=='\n'` arm is defensive for a future
         * multi-line lexer.) */
        int leading_ws = (p == 0) || (p != ws0) || (src[p - 1] == '\n');

        int start = p;
        char c = src[p];
        uint8_t cl = CLASS[(uint8_t)c];

        /* kdb sign rule: '-' adjacent to a digit is a SIGN when preceded by
         * whitespace or start-of-input (`neg -1` applies neg to -1; `x -1`
         * indexes x at -1); it is the verb only when glued to a noun (a-1). */
        int neg_sign = (c == '-' && (CLASS[(uint8_t)src[p+1]] & CL_DIGIT) &&
                        (!noun_pos || p == 0 || (CLASS[(uint8_t)src[p-1]] & CL_WS)));

        if ((cl & CL_DIGIT) || neg_sign) {
            EMIT(T_NOUN, scan_num_literal(src, &p));
            noun_pos = 1;
        }
        /* q names cannot START with '_' (leading '_' is the drop/cut verb);
         * interior '_' stays a name byte (a_b) via the CL_ALPHA continuation
         * loops below, so only the token-start byte is excluded here. */
        else if ((cl & CL_ALPHA) && c != '_') {
            while (CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) p++;
            while (src[p] == '.' && (CLASS[(uint8_t)src[p+1]] & CL_ALPHA)) {
                p++;
                while (CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) p++;
            }
            int len = p - start;
            if (len >= MAX_NAME) q_die("name too long");
            /* Only reclassify as an infix verb in true infix position (after a
             * noun); a prefix/standalone `div` stays a name-ref noun. */
            if (noun_pos && q_is_kw_verb(src + start, len)) {
                EMIT(T_VERB, q_verb_name(src + start, len));
                noun_pos = 0;
            } else {
                EMIT(T_NOUN, q_name(src + start, len));
                noun_pos = 1;
            }
        }
        else if (c == '"') {
            /* String literal: scan to the closing quote, honouring `\` escapes
             * (the escaped byte is skipped so an escaped quote does not close
             * the string).  Emit the raw inner bytes as a RAY_STR — escape
             * decoding is deferred (the tested subset uses no escapes). */
            p++;                     /* past opening quote */
            int s = p;
            while (src[p] && src[p] != '"') {
                if (src[p] == '\\' && src[p+1]) p += 2;   /* skip escaped char */
                else p++;
            }
            if (src[p] != '"') q_die("unterminated string");
            int len = p - s;
            p++;                     /* past closing quote */
            EMIT(T_NOUN, ray_str(src + s, (size_t)len));
            noun_pos = 1;
        }
        else if (c == '`') {
            /* A run of one-or-more `name symbols.  count == 1 -> the existing
             * -RAY_SYM literal atom (kdb -11h).  count > 1 -> a RAY_SYM vector
             * (kdb 11h), literal self-evaluating data (a vector is not a
             * RAY_LIST, so ray_eval returns it as a value; no ATTR_QUOTED
             * needed — that flag is only for -RAY_SYM name-refs).  The null
             * symbol ` is a zero-length name and is preserved (interned ""). */
            ray_t *first = NULL;   /* the sole atom when count == 1 */
            ray_t *vec   = NULL;   /* the vector once count >= 2 */
            int count = 0;
            while (src[p] == '`') {
                p++;
                int s = p;
                while ((CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) || src[p] == '.') p++;
                if (count == 0) {
                    first = q_symlit(src + s, p - s);
                } else if (count == 1) {
                    vec = ray_sym_vec_new(RAY_SYM_W64, 4);
                    ray_t *fs = ray_sym_str(first->i64);
                    vec = q_symvec_append(vec, ray_str_ptr(fs), (int)ray_str_len(fs));
                    ray_release(fs);
                    ray_release(first); first = NULL;
                    vec = q_symvec_append(vec, src + s, p - s);
                } else {
                    vec = q_symvec_append(vec, src + s, p - s);
                }
                count++;
            }
            EMIT(T_NOUN, count > 1 ? vec : first);
            noun_pos = 1;
        }
        else if (cl & CL_VERB) {
            /* Two-char comparison operators are single q verbs (kdb q.flex:
             * '<=' LESS_OR_EQUAL, '>=' MORE_OR_EQUAL, '<>' NOT_EQUAL).  p is at
             * `c` here, so src[p+1] is the following byte. */
            if ((c == '<' && (src[p+1] == '=' || src[p+1] == '>')) ||
                (c == '>' &&  src[p+1] == '=')) {
                char nm[2] = { c, src[p+1] };
                p += 2;
                EMIT(T_VERB, q_verb_name(nm, 2));
            } else {
                p++;
                if (src[p] == ':') {
                    /* explicit monadic marker: keep it in the token name so the
                     * tree displays kdb-style (+: |: ::) and the parser embeds
                     * the monadic row.  `::` (c==':' marked) is also the q
                     * generic null / global-assign verb — same spelling. */
                    p++;
                    char nm[2] = { c, ':' };
                    EMIT(T_VERB, q_verb_name(nm, 2));
                } else {
                    EMIT(T_VERB, q_verb(c));
                }
            }
            noun_pos = 0;
        }
        else if (cl & CL_ADVERB) {
            /* q line comment: a '/' with leading whitespace (or at the start
             * of the line) runs to end-of-line and emits no token.  Without
             * leading whitespace it stays an adverb (div / each-right etc.).
             * String-internal '/' never reaches here — strings are scanned as
             * a unit above. */
            if (c == '/' && leading_ws) {
                while (src[p] && src[p] != '\n') p++;
                continue;
            }
            int base = (c == '\'') ? 0 : (c == '/') ? 1 : 2;
            p++;
            int two = (src[p] == ':');
            if (two) p++;
            EMIT(T_ADVERB, q_marker(ADVERB_NAMES[base + (two ? 3 : 0)]));
            noun_pos = 0;
        }
        else {
            TKind kk = T_EOF; /* default path q_die()s; keep Clang definite-init happy */
            switch (c) {
            case '(': kk = T_LPAREN; noun_pos = 0; break;
            case ')': kk = T_RPAREN; noun_pos = 1; break;
            case '{': kk = T_LBRACE; noun_pos = 0; break;
            case '}': kk = T_RBRACE; noun_pos = 1; break;
            case '[': kk = T_LBRACK; noun_pos = 0; break;
            case ']': kk = T_RBRACK; noun_pos = 1; break;
            case ';': kk = T_SEMI;   noun_pos = 0; break;
            /* Unknown byte (string quote, hex, stray char, non-ASCII): error
             * rather than silently skip it — a dropped byte turns unsupported
             * q into a false "parse OK". */
            default:  q_die("unexpected character");
            }
            p++;
            EMIT(kk, NULL);
        }
    }

    if (n >= cap) { cap = cap ? cap * 2 : 32; toks = realloc(toks, (size_t)cap * sizeof(Token));
                    if (!toks) q_die("out of memory"); }
    toks[n++] = (Token){ .kind = T_EOF, .start = p, .len = 0, .k = NULL };
    g_toks.t = toks; g_toks.n = n;
#undef EMIT
    return (Tokens){ toks, n };
}

static void free_tokens(Tokens ts) {
    for (int i = 0; i < ts.n; i++)
        if (ts.t[i].k) ray_release(ts.t[i].k);
    free(ts.t);
}

/* ===== parser ================================================================ */

typedef enum { R_NONE, R_NOUN, R_VERB } Role;
typedef struct { Role role; ray_t *v; } P;
static const P EMPTY = { R_NONE, NULL };

typedef struct {
    const char *src;
    Tokens t;
    int    pos;
} Parser;

static Token *cur(Parser *p) { return &p->t.t[p->pos]; }
static int    at(Parser *p, TKind k) { return cur(p)->kind == k; }
static void   adv(Parser *p) { p->pos++; }

static void expect(Parser *p, TKind k, const char *msg) {
    if (at(p, k)) adv(p); else q_die(msg);
}

static ray_t *parse_E(Parser *p);
static P       parse_e(Parser *p);
static P       parse_e_from(Parser *p, P t);
static P       parse_term(Parser *p);
static P       parse_base(Parser *p);
static ray_t  *parse_qsql_select(Parser *p, int *ok);   /* piece 3: qSQL SELECT */

/* Statement sequence: one -> its element; two+ -> (`;; ...).  Consumes e. */
static ray_t *seq_of(ray_t *e) {
    int64_t n = ray_len(e);
    if (n == 1) {
        ray_t **slots = (ray_t **)ray_data(e);
        ray_t *only = slots[0];
        if (only) ray_retain(only);
        ray_release(e);
        return only;
    }
    ray_t *w = ray_list_new(n + 1);
    ray_t *semi = q_marker(";");
    w = ray_list_append(w, semi);
    ray_release(semi);
    ray_t **slots = (ray_t **)ray_data(e);
    for (int64_t i = 0; i < n; i++) w = ray_list_append(w, slots[i]);
    ray_release(e);
    return w;
}

static P parse_base(Parser *p) {
    Token *tk = cur(p);
    switch (tk->kind) {
    case T_NOUN: {
        ray_t *v = tk->k; tk->k = NULL;
        adv(p);
        return (P){ R_NOUN, v };
    }
    case T_VERB: {
        ray_t *v = tk->k; tk->k = NULL;
        adv(p);
        return (P){ R_VERB, v };
    }
    case T_LPAREN: {
        adv(p);
        /* Table literal `([] col:vals; …)` — a paren whose first token is `[`.
         * The bracketed section holds KEY columns (empty here = unkeyed); the
         * body is a `;`-separated column-def sequence.  Emits (table_value;
         * col1; col2; …) — the SAME shape as a paren list but with the table
         * constructor head, so the shared right-to-left context builder
         * (q_ctx_build, table mode) assembles the columns.  q_fmt hides the
         * head. */
        if (at(p, T_LBRACK)) {
            adv(p);                                   /* consume '[' */
            if (!at(p, T_RBRACK))
                q_die("nyi: keyed table literal ([k:..] ..) is deferred");
            expect(p, T_RBRACK, "expected ']' in table literal");
            ray_t *tv = q_registry_table_value();
            if (!tv) q_die("table literal: registry not initialized");
            ray_t *cols = parse_E(p);
            expect(p, T_RPAREN, "expected ')'");
            int64_t cn = ray_len(cols);
            ray_t **cs = (ray_t **)ray_data(cols);
            ray_t *w = ray_list_new(cn + 1);
            w = ray_list_append(w, tv);
            for (int64_t i = 0; i < cn; i++) {
                if (cs[i]) { w = ray_list_append(w, cs[i]); }
                else       { ray_t *nul = q_null(); w = ray_list_append(w, nul); ray_release(nul); }
            }
            ray_release(cols);
            return (P){ R_NOUN, w };
        }
        ray_t *e = parse_E(p);
        expect(p, T_RPAREN, "expected ')'");
        /* Inside parens an elided element is the generic null (kdb: `(;5)` is
         * the 2-list (::;5)) — normalize the C-NULL slots parse_E preserves
         * for top-level statements, so no NULL ever reaches ray_eval through
         * a value expression (e.g. an assignment RHS). */
        if (ray_len(e) > 1) {
            ray_t **slots = (ray_t **)ray_data(e);
            for (int64_t i = 0; i < ray_len(e); i++)
                if (!slots[i]) slots[i] = q_null();
        }
        if (ray_len(e) == 1) {
            ray_t **slots = (ray_t **)ray_data(e);
            ray_t *only = slots[0];
            if (only) ray_retain(only);
            ray_release(e);
            /* a parenthesized lone glyph verb `(+)` is the bare-verb VALUE
             * (dyadic row); user names keep their name-ref. */
            if (q_sym_is_glyph(only)) only = q_embed(only, Q_DYADIC);
            return (P){ R_NOUN, only };
        }
        /* Multi-element paren list is a LITERAL: prepend the internal
         * list-constructor value so eval builds (and collapses) it.  The
         * head value is the only thing distinguishing `(1 2;3 4)` from the
         * shape-identical bracket-index call (v;i); q_fmt hides it so the
         * tree still displays (1;2;3).  Cold registry (no q_runtime): keep
         * the bare list — pre-2b behaviour. */
        {
            ray_t *lv = q_registry_list_value();
            if (lv) {
                int64_t en = ray_len(e);
                ray_t **es = (ray_t **)ray_data(e);
                ray_t *w = ray_list_new(en + 1);
                w = ray_list_append(w, lv);
                for (int64_t i = 0; i < en; i++)
                    w = ray_list_append(w, es[i]);
                ray_release(e);
                e = w;
            }
        }
        return (P){ R_NOUN, e };
    }
    case T_LBRACE: {
        adv(p);
        ray_t *e = parse_E(p);
        expect(p, T_RBRACE, "expected '}'");
        ray_t *xs[2] = { q_marker("{"), seq_of(e) };
        return (P){ R_NOUN, q_list(xs, 2) };
    }
    default:
        return EMPTY;
    }
}

static P parse_term(Parser *p) {
    P t = parse_base(p);
    if (t.role == R_NONE) return t;

    for (;;) {
        Token *tk = cur(p);
        if (tk->kind == T_LBRACK) {
            adv(p);
            ray_t *e = parse_E(p);
            expect(p, T_RBRACK, "expected ']'");
            /* bracket-apply on a bare verb (`+[2;]`) embeds the dyadic row —
             * an underapplied call becomes a projection downstream (2c). */
            if (t.role == R_VERB) t.v = q_embed(t.v, Q_DYADIC);
            int64_t en = ray_len(e);
            ray_t **es = (ray_t **)ray_data(e);
            ray_t *w = ray_list_new(en + 1);
            w = ray_list_append(w, t.v);
            ray_release(t.v);
            for (int64_t i = 0; i < en; i++) {
                if (es[i]) { w = ray_list_append(w, es[i]); }
                else       { ray_t *nul = q_null(); w = ray_list_append(w, nul); ray_release(nul); }
            }
            ray_release(e);
            t.v = w; t.role = R_NOUN;
        } else if (tk->kind == T_ADVERB) {
            /* a glyph verb under an adverb (`+/`) is a bare-verb VALUE:
             * embed its dyadic row (names/lambdas stay for eval to resolve) */
            if (t.role == R_VERB) t.v = q_embed(t.v, Q_DYADIC);
            ray_t *xs[2] = { tk->k, t.v };
            tk->k = NULL;
            t.v = q_list(xs, 2);
            adv(p);
            t.role = R_VERB;
        } else {
            break;
        }
    }
    return t;
}

static P parse_e(Parser *p) {
    /* qSQL interception (piece 3): a `select …` statement lowers to kdb's
     * functional parse tree (?;`t;c;b;a).  On any unsupported form parse_qsql
     * soft-fails (restores p->pos, leaves tokens intact) and we fall through to
     * the ordinary parser — so previously-parseable selects never regress. */
    if (cur(p)->kind == T_NOUN) {
        Token *tk = cur(p);
        if (tk->len == 6 && memcmp(p->src + tk->start, "select", 6) == 0) {
            int ok = 1;
            ray_t *q = parse_qsql_select(p, &ok);
            if (ok && q) return (P){ R_NOUN, q };
        }
    }
    P t = parse_term(p);
    if (t.role == R_NONE) return EMPTY;
    return parse_e_from(p, t);
}

static P parse_e_from(Parser *p, P t) {
    P u = parse_term(p);

    if (u.role == R_NONE) return t;

    if (t.role == R_NOUN && u.role == R_VERB) {
        P e = parse_e(p);
        ray_t *rhs = e.v ? e.v : q_null();
        u.v = q_embed(u.v, Q_DYADIC);          /* infix head: the dyadic row */
        ray_t *xs[3] = { u.v, t.v, rhs };
        return (P){ R_NOUN, q_list(xs, 3) };
    }

    P e = parse_e_from(p, u);
    /* Prefix application of a bare 1-char glyph verb is MONADIC: respell the
     * head `+` -> `+:` so the tree displays kdb-style ((+:;1) for "+1") and
     * the (name, MONADIC) registry row is addressable.  Marked verbs (+:)
     * already carry the spelling; `:` respells to the generic `::`. */
    if (t.role == R_VERB && t.v && t.v->type == -RAY_SYM &&
        !(t.v->attrs & Q_ATTR_QUOTED)) {
        ray_t *s = ray_sym_str(t.v->i64);
        if (s) {
            if (ray_str_len(s) == 1 && strchr(VERB_CHARS, ray_str_ptr(s)[0])) {
                char nm[2] = { ray_str_ptr(s)[0], ':' };
                ray_t *m = q_verb_name(nm, 2);
                ray_release(t.v);
                t.v = m;
            }
            ray_release(s);
        }
    }
    /* Prefix glyph head embeds its MONADIC registry value; a bare glyph verb
     * standing as the rhs OPERAND (`+ -` applies + to the - value) embeds its
     * dyadic row (bare-verb-as-value convention). */
    if (t.role == R_VERB) t.v = q_embed(t.v, Q_MONADIC);
    if (e.role == R_VERB && q_sym_is_glyph(e.v)) e.v = q_embed(e.v, Q_DYADIC);
    ray_t *xs[2] = { t.v, e.v };
    return (P){ R_NOUN, q_list(xs, 2) };
}

/* ===== qSQL SELECT parser (piece 3) =========================================
 * Lowers a `select …` statement to kdb's functional parse tree:
 *
 *     (?; `t; c; b; a)
 *
 *   `t  the from-table NAME as a symbol literal
 *   c   where-constraints: () when none, else enlist(constraint-list) where
 *       each constraint is a parse tree (op;`col;arg) — so one constraint
 *       displays ,,(op;…) and two ,((…);(…)), matching the docs' vertical form.
 *   b   by-clause: 0b when none, else a name!expr dict.
 *   a   select-columns: () when none, else a name!expr dict.
 *
 * Inside clause expressions a bare identifier is a COLUMN reference (a symbol
 * literal `col`) unless it resolves to a function value (an aggregate/keyword
 * like `sum`, embedded as its value so it prints bare); a `sym literal is a
 * VALUE so it is enlisted (,`s); numbers stay as-is.  This is q's qSQL rule.
 *
 * The parser is deliberately CORE-only (no joins/fby/subqueries); on any shape
 * it does not recognise it sets *ok=0, and the caller restores the token
 * position and re-parses the statement the ordinary way (no parse regression). */

#define QSQL_MAXCOLS 256

/* ===== from-expression token snapshot ========================================
 * parse_qsql_select soft-fails by restoring p->pos and letting the ordinary
 * parser re-parse the SAME token array.  The qsql_* helpers only retain tk->k,
 * so that restore is trivially safe — but the from-EXPRESSION path calls
 * parse_term, and parse_base STEALS tk->k (sets it NULL).  Snapshot the k
 * pointers (one extra retain each) from the from-position to EOF before
 * calling parse_term; on soft-fail hand the retained refs back to the stolen
 * slots, on success just drop them.  Frames form a LIFO stack (parse_term
 * recurses into parse_e, which re-enters parse_qsql_select: nested selects
 * take nested snapshots); each frame holds its OWN retains so overlapping
 * frames stay independent.  Heap-allocated and file-static so the q_die
 * longjmp cleanup in q_parse can free every in-flight frame (parse_term can
 * die mid-expression, unwinding past any number of frames). */
typedef struct qsql_snap {
    ray_t           **k;     /* snapshotted token values, one retain each */
    int               n;
    int               at;    /* token index of snapshot start */
    struct qsql_snap *prev;
} qsql_snap_t;

static qsql_snap_t *g_qsql_snap = NULL;   /* stack top */

static void qsql_snap_take(Parser *p) {
    int n = p->t.n - p->pos;
    qsql_snap_t *f = (qsql_snap_t *)malloc(sizeof *f);
    f->k  = (ray_t **)malloc(sizeof(ray_t *) * (size_t)(n > 0 ? n : 1));
    f->n  = n;
    f->at = p->pos;
    for (int i = 0; i < n; i++) {
        ray_t *k = p->t.t[p->pos + i].k;
        if (k) ray_retain(k);
        f->k[i] = k;
    }
    f->prev = g_qsql_snap;
    g_qsql_snap = f;
}

/* success path: pop the top frame, dropping its extra refs */
static void qsql_snap_drop(void) {
    qsql_snap_t *f = g_qsql_snap;
    if (!f) return;
    for (int i = 0; i < f->n; i++)
        if (f->k[i]) ray_release(f->k[i]);
    g_qsql_snap = f->prev;
    free(f->k);
    free(f);
}

/* soft-fail path: pop the top frame, handing refs back to stolen slots */
static void qsql_snap_restore(Parser *p) {
    qsql_snap_t *f = g_qsql_snap;
    if (!f) return;
    for (int i = 0; i < f->n; i++) {
        Token *tk = &p->t.t[f->at + i];
        if (tk->k == NULL) tk->k = f->k[i];            /* transfer our ref */
        else if (f->k[i]) ray_release(f->k[i]);        /* token untouched */
    }
    g_qsql_snap = f->prev;
    free(f->k);
    free(f);
}

/* q_die unwind: free EVERY in-flight frame (the token array is freed too) */
static void qsql_snap_unwind(void) {
    while (g_qsql_snap) qsql_snap_drop();
}

/* token text equals a keyword (only meaningful for T_NOUN identifier tokens) */
static int qtok_is(Parser *p, Token *tk, const char *kw, int kwlen) {
    return tk->kind == T_NOUN && tk->len == kwlen &&
           memcmp(p->src + tk->start, kw, (size_t)kwlen) == 0;
}

/* a clause boundary: end-of-clause markers plus the ',' column/constraint
 * separator (a bare comma verb) and the qSQL section keywords. */
static int qsql_boundary(Parser *p) {
    Token *tk = cur(p);
    switch (tk->kind) {
    case T_EOF: case T_SEMI: case T_RBRACK: case T_RPAREN: case T_RBRACE:
        return 1;
    case T_VERB:
        return tk->len == 1 && p->src[tk->start] == ',';
    case T_NOUN:
        return qtok_is(p, tk, "by", 2) || qtok_is(p, tk, "from", 4) ||
               qtok_is(p, tk, "where", 5);
    default:
        return 0;
    }
}

/* a symbol-literal column reference `col (ATTR_QUOTED set) from an interned id */
static ray_t *qsql_colsym(int64_t id) {
    ray_t *s = ray_sym(id);
    if (s && !RAY_IS_ERR(s)) s->attrs |= Q_ATTR_QUOTED;
    return s;
}

/* enlist an owned value into a 1-element general list (consumes v) */
static ray_t *qsql_enlist(ray_t *v) {
    ray_t *l = ray_list_new(1);
    l = ray_list_append(l, v);
    ray_release(v);
    return l;
}

static ray_t *qsql_expr(Parser *p, int *ok);

/* One clause TERM: a column symbol, an embedded function value, a `sym literal
 * (enlisted), a number/other literal (as-is), or a parenthesised sub-expr. */
static ray_t *qsql_term(Parser *p, int *ok) {
    Token *tk = cur(p);
    if (tk->kind == T_LPAREN) {
        adv(p);
        ray_t *e = qsql_expr(p, ok);
        if (!*ok) { if (e) ray_release(e); return NULL; }
        if (!at(p, T_RPAREN)) { *ok = 0; if (e) ray_release(e); return NULL; }
        adv(p);
        return e;
    }
    if (tk->kind != T_NOUN || !tk->k) { *ok = 0; return NULL; }
    ray_t *k = tk->k;
    if (k->type == -RAY_SYM) {
        if (k->attrs & Q_ATTR_QUOTED) {              /* `sym literal -> ,`sym */
            adv(p);
            ray_t *vec = ray_sym_vec_new(RAY_SYM_W64, 1);
            ray_t *s = ray_sym_str(k->i64);
            vec = q_symvec_append(vec, ray_str_ptr(s), (int)ray_str_len(s));
            ray_release(s);
            return vec;
        }
        int64_t id = k->i64;                          /* name reference */
        adv(p);
        ray_t *ev = ray_env_get(id);
        if (ev && (ev->type == RAY_UNARY || ev->type == RAY_BINARY ||
                   ev->type == RAY_VARY)) {           /* function -> its value */
            ray_retain(ev);
            return ev;
        }
        return qsql_colsym(id);                        /* else column symbol */
    }
    if (k->type == RAY_SYM) {                          /* `a`b vector -> ,`a`b */
        adv(p);
        ray_retain(k);
        return qsql_enlist(k);
    }
    adv(p);                                            /* number / other literal */
    ray_retain(k);
    return k;
}

/* A clause EXPRESSION: right-associative infix / prefix application, stopping
 * at any clause boundary.  Verbs stay bare-glyph symbols so they print `>`,`*`. */
static ray_t *qsql_expr(Parser *p, int *ok) {
    ray_t *t = qsql_term(p, ok);
    if (!*ok) return NULL;
    if (qsql_boundary(p)) return t;
    Token *tk = cur(p);
    if (tk->kind == T_VERB) {                          /* infix: (op; t; rhs) */
        ray_t *op = tk->k;
        if (!op) { *ok = 0; ray_release(t); return NULL; }
        ray_retain(op);
        adv(p);
        ray_t *rhs = qsql_expr(p, ok);
        if (!*ok) { ray_release(op); ray_release(t); if (rhs) ray_release(rhs); return NULL; }
        ray_t *node = ray_list_new(3);
        node = ray_list_append(node, op);  ray_release(op);
        node = ray_list_append(node, t);   ray_release(t);
        node = ray_list_append(node, rhs); ray_release(rhs);
        return node;
    }
    if (tk->kind == T_NOUN || tk->kind == T_LPAREN) {  /* prefix app: (t; rhs) */
        /* Prefix application is well-formed ONLY when the term is a FUNCTION
         * value (an aggregate: `sum col`, `max price`).  A column/literal
         * followed by another token is a keyword-infix predicate (`a in b`,
         * `sym like "x"`) — the scanner leaves in/like/within as name-ref
         * nouns, so we cannot render them kdb-faithfully here.  Soft-fail so
         * the ordinary parser consumes the whole statement, rather than emit a
         * malformed tree that the interceptor would wrongly accept. */
        if (!(t->type == RAY_UNARY || t->type == RAY_BINARY || t->type == RAY_VARY)) {
            *ok = 0; ray_release(t); return NULL;
        }
        ray_t *rhs = qsql_expr(p, ok);
        if (!*ok) { ray_release(t); if (rhs) ray_release(rhs); return NULL; }
        ray_t *node = ray_list_new(2);
        node = ray_list_append(node, t);   ray_release(t);
        node = ray_list_append(node, rhs); ray_release(rhs);
        return node;
    }
    *ok = 0;
    ray_release(t);
    return NULL;
}

/* Rightmost column symbol in an expression — the default output-column name
 * (`sum a` -> `a`, `a` -> `a`).  Returns a fresh owned `sym, or NULL. */
static ray_t *qsql_derive_alias(ray_t *expr) {
    if (!expr) return NULL;
    if (expr->type == -RAY_SYM && (expr->attrs & Q_ATTR_QUOTED))
        return qsql_colsym(expr->i64);
    if (expr->type == RAY_LIST) {
        int64_t n = ray_len(expr);
        ray_t **e = (ray_t **)ray_data(expr);
        for (int64_t i = n - 1; i >= 0; i--) {
            ray_t *a = qsql_derive_alias(e[i]);
            if (a) return a;
        }
    }
    return NULL;
}

/* One column spec: optional `alias:` then an expression.  On success stores an
 * owned alias `sym and value expr; returns 0 (with nothing owned) on failure. */
static int qsql_colspec(Parser *p, ray_t **out_alias, ray_t **out_val) {
    int ok = 1;
    ray_t *alias = NULL;
    Token *t0 = cur(p);
    Token *t1 = &p->t.t[p->pos + 1];
    if (t0->kind == T_NOUN && t0->k && t0->k->type == -RAY_SYM &&
        !(t0->k->attrs & Q_ATTR_QUOTED) &&
        t1->kind == T_VERB && t1->len == 1 && p->src[t1->start] == ':') {
        alias = qsql_colsym(t0->k->i64);
        adv(p); adv(p);
    }
    ray_t *val = qsql_expr(p, &ok);
    if (!ok || !val) { if (alias) ray_release(alias); if (val) ray_release(val); return 0; }
    if (!alias) alias = qsql_derive_alias(val);
    if (!alias) { ray_release(val); return 0; }   /* unnameable -> soft-fail */
    *out_alias = alias; *out_val = val;
    return 1;
}

/* Comma-separated column-spec list up to a section boundary.  Fills the caller
 * arrays; *n stays 0 for an empty list (e.g. `select from`). */
static int qsql_collist(Parser *p, ray_t **aliases, ray_t **vals, int *n) {
    *n = 0;
    if (qsql_boundary(p)) return 1;                 /* empty (immediately by/from) */
    for (;;) {
        if (*n >= QSQL_MAXCOLS) return 0;
        if (!qsql_colspec(p, &aliases[*n], &vals[*n])) return 0;
        (*n)++;
        Token *tk = cur(p);
        if (tk->kind == T_VERB && tk->len == 1 && p->src[tk->start] == ',') { adv(p); continue; }
        break;
    }
    return 1;
}

/* Build a q name!expr dict from a column list (consumes the alias/val refs). */
static ray_t *qsql_build_dict(ray_t **aliases, ray_t **vals, int n) {
    ray_t *keys = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    for (int i = 0; i < n; i++) {
        ray_t *s = ray_sym_str(aliases[i]->i64);
        keys = q_symvec_append(keys, ray_str_ptr(s), (int)ray_str_len(s));
        ray_release(s);
    }
    int all_sym = 1;
    for (int i = 0; i < n; i++)
        if (!(vals[i] && vals[i]->type == -RAY_SYM && (vals[i]->attrs & Q_ATTR_QUOTED)))
            { all_sym = 0; break; }
    ray_t *valv;
    if (all_sym) {
        valv = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
        for (int i = 0; i < n; i++) {
            ray_t *s = ray_sym_str(vals[i]->i64);
            valv = q_symvec_append(valv, ray_str_ptr(s), (int)ray_str_len(s));
            ray_release(s);
        }
    } else {
        valv = ray_list_new(n > 0 ? n : 1);
        for (int i = 0; i < n; i++) valv = ray_list_append(valv, vals[i]);
    }
    for (int i = 0; i < n; i++) { ray_release(aliases[i]); ray_release(vals[i]); }
    return ray_dict_new(keys, valv);   /* consumes keys, valv */
}

/* where-clause: comma-separated constraints -> enlist(constraint-list). */
static ray_t *qsql_where(Parser *p, int *ok) {
    ray_t *cons[QSQL_MAXCOLS]; int nc = 0;
    for (;;) {
        if (nc >= QSQL_MAXCOLS) { *ok = 0; break; }
        ray_t *e = qsql_expr(p, ok);
        if (!*ok) break;
        cons[nc++] = e;
        Token *tk = cur(p);
        if (tk->kind == T_VERB && tk->len == 1 && p->src[tk->start] == ',') { adv(p); continue; }
        break;
    }
    if (!*ok) { for (int i = 0; i < nc; i++) ray_release(cons[i]); return NULL; }
    ray_t *clist = ray_list_new(nc);
    for (int i = 0; i < nc; i++) { clist = ray_list_append(clist, cons[i]); ray_release(cons[i]); }
    return qsql_enlist(clist);
}

/* select COLS [by BYCOLS] from TABLE [where CONSTRAINTS] -> (?;`t;c;b;a). */
static ray_t *parse_qsql_select(Parser *p, int *ok) {
    *ok = 1;
    int save = p->pos;
    int snapped = 0;                       /* from-expression token snapshot */
    ray_t *aliases[QSQL_MAXCOLS], *vals[QSQL_MAXCOLS]; int na = 0;
    ray_t *b = NULL, *t = NULL, *c = NULL, *a = NULL, *head = NULL, *q = NULL;

    adv(p);                                       /* consume `select` */
    if (!qsql_collist(p, aliases, vals, &na)) { *ok = 0; goto fail; }

    if (qtok_is(p, cur(p), "by", 2)) {            /* by-clause */
        adv(p);
        ray_t *bk[QSQL_MAXCOLS], *bv[QSQL_MAXCOLS]; int nb = 0;
        if (!qsql_collist(p, bk, bv, &nb) || nb == 0) {
            for (int i = 0; i < nb; i++) { ray_release(bk[i]); ray_release(bv[i]); }
            *ok = 0; goto fail;
        }
        b = qsql_build_dict(bk, bv, nb);
    } else {
        b = ray_bool(0);
    }

    if (!qtok_is(p, cur(p), "from", 4)) { *ok = 0; goto fail; }
    adv(p);
    /* from TABLE-NAME | from EXPRESSION.
     * A plain name followed by `where` or a statement boundary keeps the kdb
     * by-name form: slot 1 = the symbol literal `t (byte-identical display;
     * by-name semantics matter later for partitioned tables).  Anything else
     * parses ONE ordinary term (table literal ([] …), parenthesised
     * expression — which admits ANY expression — a name[…] indexing chain, …)
     * as the from-expression, under a token snapshot so a later soft-fail can
     * still fall back to the ordinary parser (parse_base steals tk->k). */
    Token *tt = cur(p);
    Token *nx = &p->t.t[p->pos + 1];   /* safe: a T_NOUN is never the T_EOF
                                        * terminator, and nx is only read
                                        * behind the T_NOUN check below */
    int name_form = tt->kind == T_NOUN && tt->k && tt->k->type == -RAY_SYM &&
        !(tt->k->attrs & Q_ATTR_QUOTED) &&
        (qtok_is(p, nx, "where", 5) ||
         nx->kind == T_EOF || nx->kind == T_SEMI || nx->kind == T_RBRACK ||
         nx->kind == T_RPAREN || nx->kind == T_RBRACE);
    if (name_form) {
        t = qsql_colsym(tt->k->i64);
        adv(p);
    } else {
        if (tt->kind == T_EOF) { *ok = 0; goto fail; }
        qsql_snap_take(p);
        snapped = 1;
        P fe = parse_term(p);
        if (fe.role != R_NOUN || !fe.v) {
            if (fe.v) ray_release(fe.v);
            *ok = 0; goto fail;            /* fail: restores the frame */
        }
        t = fe.v;
    }

    if (qtok_is(p, cur(p), "where", 5)) {         /* where-clause */
        adv(p);
        c = qsql_where(p, ok);
        if (!*ok) goto fail;
    } else {
        c = ray_list_new(0);
    }

    /* Anything beyond the CORE grammar left in the statement (a join `from t,u`
     * / `from t lj u`, a trailing phrase, …) means this is not a form we lower.
     * Soft-fail so the ordinary parser consumes the WHOLE statement — otherwise
     * the leftover tokens would trip q_parse's EOF check (a parse regression). */
    {
        Token *end = cur(p);
        if (end->kind != T_EOF && end->kind != T_SEMI && end->kind != T_RBRACK &&
            end->kind != T_RPAREN && end->kind != T_RBRACE) { *ok = 0; goto fail; }
    }

    /* Committed: settle the from-expression snapshot (drop the extra refs).
     * Settle ONLY our own frame — a nested parse_qsql_select inside the
     * from-expression pushed and popped its own (strict LIFO). */
    if (snapped) { qsql_snap_drop(); snapped = 0; }

    a = (na == 0) ? ray_list_new(0) : qsql_build_dict(aliases, vals, na);
    na = 0;                                        /* consumed by build_dict */

    head = q_verb('?');
    q = ray_list_new(5);
    q = ray_list_append(q, head); ray_release(head);
    q = ray_list_append(q, t);    ray_release(t);
    q = ray_list_append(q, c);    ray_release(c);
    q = ray_list_append(q, b);    ray_release(b);
    q = ray_list_append(q, a);    ray_release(a);
    return q;

fail:
    /* Hand the snapshotted token values back to any slots parse_term stole,
     * so the ordinary-parser fallback re-parses pristine tokens. */
    if (snapped) qsql_snap_restore(p);
    for (int i = 0; i < na; i++) { ray_release(aliases[i]); ray_release(vals[i]); }
    if (b) ray_release(b);
    if (t) ray_release(t);
    if (c) ray_release(c);
    if (a) ray_release(a);
    p->pos = save;
    return NULL;
}

static ray_t *parse_E(Parser *p) {
    ray_t *buf[MAX_VEC]; int n = 0;
    buf[n++] = parse_e(p).v;
    while (at(p, T_SEMI)) {
        if (n >= MAX_VEC) q_die("too many ';'-separated expressions");
        adv(p);
        buf[n++] = parse_e(p).v;
    }
    /* Build the expression list PRESERVING C-NULL slots (unlike q_list, which
     * normalises them to q_null()).  An empty statement — a whole-line comment
     * or the `;;` between two expressions — is a C NULL here; seq_of and the
     * evaluator treat that as a no-op that yields no output.  Normalising to
     * q_null() would instead print `::`. */
    ray_t *l = ray_list_new(n > 0 ? n : 1);
    for (int i = 0; i < n; i++) {
        l = ray_list_append(l, buf[i]);
        if (buf[i]) ray_release(buf[i]);
    }
    return l;
}

/* ===== public entry ========================================================== */

ray_t *q_parse(const char *src) {
    /* Value embedding requires a live registry (codex #1): fail fast rather
     * than silently emit a mixed sym/value tree.  Every q entry point
     * bootstraps via q_runtime_create, which initializes the registry. */
    if (!q_registry_ready())
        return ray_error("init", "q_parse: op registry not initialized");
    init_class();
    g_toks.t = NULL;
    g_toks.n = 0;
    if (setjmp(q_err_jmp)) {
        /* q_die() longjmped here; free whatever the scanner had emitted,
         * plus any in-flight qSQL from-expression token snapshots. */
        qsql_snap_unwind();
        free_tokens(g_toks);
        g_toks.t = NULL;
        g_toks.n = 0;
        return ray_error("parse", "%s", q_err_buf);
    }

    Tokens ts = scan(src);
    Parser p = { .src = src, .t = ts, .pos = 0 };
    ray_t *e = parse_E(&p);
    if (!at(&p, T_EOF)) {
        ray_release(e);
        free_tokens(ts);
        g_toks.t = NULL; g_toks.n = 0;
        return ray_error("parse", "unexpected token");
    }
    ray_t *prog = seq_of(e);
    free_tokens(ts);
    g_toks.t = NULL; g_toks.n = 0;
    /* An empty program (a whole-line comment, or blank/whitespace-only input)
     * collapses to a C NULL in seq_of.  Return the value-null singleton
     * instead so callers get an explicit no-op sentinel: RAY_IS_NULL()
     * recognises it (the REPL prints nothing; qdoc matches empty output) and
     * ray_eval() self-evaluates it, rather than every caller having to treat a
     * bare C NULL specially. */
    return prog ? prog : RAY_NULL_OBJ;
}

/* ===== q-semantic verb resolution (eval-time) ================================
 * The AST keeps q-glyph name-ref verb heads so `parse` prints them verbatim.
 * Just before eval, rewrite each DYADIC call `(verb; a; b)` whose head is a
 * q-glyph name-ref into the q op registry's function value for that (name,
 * dyadic) key: `% -> /` (float divide), `= -> ==`, `<> -> !=`.  A registry
 * miss (`:`, `;`, `{`, user names, `f[...]` apply, monadic shapes, and the
 * ordering glyphs `< > <= >=` / `div` whose q spelling already matches the
 * rayfall name) leaves the head untouched — eval then resolves it against
 * rayfall's env exactly as before.
 *
 * RETIRED as a public pass (2b): the PARSER now embeds dyadic registry values
 * at infix/bracket glyph heads, so the only shapes this still serves are
 * keyword-dyadic rows called in bracket/prefix form (`each[count;x]` — the
 * scanner keeps prefix keywords as name-ref nouns and `each` has no env
 * binding).  That residue lives on as ql_dyad_head inside the q_lower walker. */
static void ql_dyad_head(ray_t **slot) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    /* Only monadic (n==2) / dyadic (n==3) keyword calls are candidates — guard
     * BEFORE touching e[0], so an empty list () (e.g. the qSQL `c`/`a` clauses
     * that q_lower now walks) never reads past the end. */
    if (n != 2 && n != 3) return;
    ray_t **e = (ray_t **)ray_data(node);
    if (!e[0] || e[0]->type != -RAY_SYM || (e[0]->attrs & Q_ATTR_QUOTED))
        return;
    /* dyadic bracket/prefix keyword calls (each[f;x]); AND monadic keyword
     * calls whose registry cell is a q WRAPPER diverging from the env
     * builtin (floor: q returns longs, rayfall env floor keeps f64) —
     * pass-through rows swap to the identical env object (harmless). */
    q_valence_t val = (n == 3) ? Q_DYADIC : Q_MONADIC;
    ray_t *s = ray_sym_str(e[0]->i64);
    if (!s) return;
    ray_t *hit = q_registry_lookup_name(ray_str_ptr(s), ray_str_len(s), val);
    ray_release(s);
    if (hit) {              /* borrowed -> retain one, drop the name-ref */
        ray_retain(hit);
        ray_release(e[0]);
        e[0] = hit;
    }
}

/* ===== q-lower: the ADR 0003 lowering pass =================================
 * Beyond the dyadic-head resolution above, the lowering walker rewrites the
 * q-only tree shapes rayfall eval cannot run into pure rayfall applications:
 *
 *   ((`/;F); x)     -> (fold; F'; x)          q over        f/x
 *   ((`/;F); x; y)  -> (fold; F'; x; y)       q seeded over x f/ y
 *   ((`\;F); x)     -> (q-scan; F'; x)        q scan        f\x   (+collapse)
 *   ((`';F); x)     -> (q-each; F'; x)        q each        f'x   (+collapse)
 *   (|/)x, (&/)x    -> (max; x), (min; x)     aggregate monomorphization —
 *                      rayfall has no dyadic max/min value to fold with.
 *
 * F' is the q-semantic DYADIC value for a glyph operand (q `%` folds with
 * float-divide, not modulo), the untouched subtree for user names / lambdas
 * (eval resolves those), and the value itself once the parser embeds values.
 * A glyph operand with no dyadic row and no aggregate mapping leaves the node
 * untouched — eval then errors exactly as it does today.  In-place rewrite
 * under the same rc==1 sole-owner precondition as the whole walker. */

static ray_t *ql_env_val(const char *nm) {
    return ray_env_get(ray_sym_intern_runtime(nm, strlen(nm)));
}

/* ADVERB_NAMES index of a name-ref sym (0=' 1=/ 2=\ 3=': 4=/: 5=\:), or -1. */
static int ql_adv_id(ray_t *x) {
    if (!x || x->type != -RAY_SYM || (x->attrs & Q_ATTR_QUOTED)) return -1;
    ray_t *s = ray_sym_str(x->i64);
    if (!s) return -1;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int r = -1;
    for (int i = 0; i < 6 && r < 0; i++)
        if (strlen(ADVERB_NAMES[i]) == l && memcmp(ADVERB_NAMES[i], nm, l) == 0)
            r = i;
    ray_release(s);
    return r;
}

/* Rebuild *slot as (hof; fexpr; old-args...).  Borrows hof/fexpr (append
 * retains); releases the old node. */
static void ql_rewrite(ray_t **slot, ray_t *hof, ray_t *fexpr) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *repl = ray_list_new(n + 1);
    repl = ray_list_append(repl, hof);
    repl = ray_list_append(repl, fexpr);
    for (int64_t i = 1; i < n; i++) {
        if (e[i]) { repl = ray_list_append(repl, e[i]); }
        else      { ray_t *nul = q_null(); repl = ray_list_append(repl, nul); ray_release(nul); }
    }
    ray_release(node);
    *slot = repl;
}

/* Try the adverb-application rewrite on *slot (see the table above). */
static void ql_adv_app(ray_t **slot) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    if (!(n == 2 || n == 3)) return;
    if (!e[0] || e[0]->type != RAY_LIST || ray_len(e[0]) != 2) return;
    ray_t **h = (ray_t **)ray_data(e[0]);
    int adv = ql_adv_id(h[0]);
    if (adv < 0) return;
    ray_t *F = h[1];

    int f_is_value = F && (F->type == RAY_UNARY || F->type == RAY_BINARY ||
                           F->type == RAY_VARY);
    int f_is_name  = F && F->type == -RAY_SYM && !(F->attrs & Q_ATTR_QUOTED);
    int f_is_glyph = 0;
    ray_t *freg = NULL;                    /* registry dyadic value (borrowed) */
    if (f_is_name) {
        ray_t *s = ray_sym_str(F->i64);
        if (!s) return;
        const char *nm = ray_str_ptr(s);
        size_t l = ray_str_len(s);
        f_is_glyph = (l >= 1 && strchr(VERB_CHARS, nm[0]) != NULL);
        freg = q_registry_lookup_name(nm, l, Q_DYADIC);
        ray_release(s);
    }

    /* the operand expression the HOF receives */
    ray_t *fexpr = NULL;
    if (f_is_value)                 fexpr = F;     /* post-flip embedded value */
    else if (freg)                  fexpr = freg;  /* q-semantic dyadic value  */
    else if (f_is_name && !f_is_glyph) fexpr = F;  /* user name: eval resolves */
    else if (F && F->type == RAY_LIST) fexpr = F;  /* lambda / nested derived  */

    if (adv == 1) {                                       /* `/` over */
        if (!fexpr) {
            /* aggregate monomorphization: no dyadic |/& value exists, but
             * (|/)x IS max x and (&/)x IS min x. */
            if (n == 2 && f_is_name && f_is_glyph) {
                ray_t *s = ray_sym_str(F->i64);
                ray_t *agg = NULL;
                if (s && ray_str_len(s) == 1) {
                    char g = ray_str_ptr(s)[0];
                    if (g == '|') agg = ql_env_val("max");
                    if (g == '&') agg = ql_env_val("min");
                }
                if (s) ray_release(s);
                if (agg) {
                    ray_t *repl = ray_list_new(2);
                    repl = ray_list_append(repl, agg);
                    if (e[1]) { repl = ray_list_append(repl, e[1]); }
                    else      { ray_t *nul = q_null(); repl = ray_list_append(repl, nul); ray_release(nul); }
                    ray_release(node);
                    *slot = repl;
                }
            }
            return;
        }
        ray_t *fold = ql_env_val("fold");
        if (fold) ql_rewrite(slot, fold, fexpr);
    } else if (adv == 2 && n == 2) {                      /* `\` scan (unseeded) */
        if (!fexpr) return;
        ray_t *sc = q_registry_scan_value();
        if (sc) ql_rewrite(slot, sc, fexpr);
    } else if (adv == 0 && n == 2) {                      /* `'` each (monadic) */
        if (!fexpr) return;
        ray_t *ea = q_registry_lookup_name("each", 4, Q_DYADIC);
        if (ea) ql_rewrite(slot, ea, fexpr);
    } else if ((adv == 4 || adv == 5) && n == 3) {        /* `/:` `\:` each-right/left */
        if (!fexpr) return;
        /* x f\: y -> (map-left f x y): f(x_i, y); x f/: y -> (map-right f x y):
         * f(x, y_i) — arg order matches, no swap needed. */
        ray_t *mr = ql_env_val(adv == 4 ? "map-right" : "map-left");
        if (mr) ql_rewrite(slot, mr, fexpr);
    }
    /* ': — each-prior stays deferred (no pairwise rayfall HOF). */
}

/* A bare (adv; V) 2-list in VALUE position (assigned, passed, displayed —
 * NOT the head of an application, which ql_adv_app consumes) becomes a 104h
 * projection carrier: the HOF with V bound and the data operand open,
 * shielded from eval by rayfall `quote` (a special form returning its arg
 * unevaluated).  `g:(+/)` binds the carrier; applying it through a name is
 * 2c (needs the noun-head fallback). */
static void ql_deriv_value(ray_t **slot) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    if (n != 2) return;
    ray_t **e = (ray_t **)ray_data(node);
    int adv = ql_adv_id(e[0]);
    if (adv < 0) return;
    ray_t *hof = NULL;
    if (adv == 1)      hof = ql_env_val("fold");
    else if (adv == 2) hof = q_registry_scan_value();
    else if (adv == 0) hof = q_registry_lookup_name("each", 4, Q_DYADIC);
    if (!hof) return;                        /* ': /: \: stay deferred */
    ray_t *quote = ql_env_val("quote");
    if (!quote) return;
    /* A keyword operand (`neg'`) is still a name-ref sym — bind its VALUE in
     * the carrier (kdb derives from the value): env first (keywords, user
     * fns); glyphs were already embedded by the parser.  Miss -> keep the
     * sym (name error surfaces at apply). */
    ray_t *V = e[1];
    if (V && V->type == -RAY_SYM && !(V->attrs & Q_ATTR_QUOTED)) {
        ray_t *rv = ray_env_get(V->i64);
        if (rv) V = rv;                      /* borrowed; q_proj_new retains */
    }
    ray_t *args[2] = { V, NULL };            /* V bound, data operand open */
    ray_t *carrier = q_proj_new(hof, args, 2, 0x2u, 1);
    if (!carrier || RAY_IS_ERR(carrier)) { if (carrier) ray_release(carrier); return; }
    ray_t *repl = ray_list_new(2);
    repl = ray_list_append(repl, quote);
    repl = ray_list_append(repl, carrier);
    ray_release(carrier);
    ray_release(node);
    *slot = repl;
}

/* Assignment: rewrite `(:; name; val)` / `(::; name; val)` heads to rayfall
 * set — or let for a plain `:` INSIDE a lambda body (q locals; `::` stays the
 * global assign there, kdb-faithful) — enforcing the reserved-verb 'assign
 * invariant (ADR 0003 Decision 1).  Returns a RAY_ERROR to abort lowering, or
 * NULL to continue.  Non-sym assign targets (2:3, f[1]:x) are left untouched:
 * eval errors on them exactly as it does today (indexed assign is 2c+). */
static ray_t *ql_assign(ray_t **slot, int in_lambda) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    if (n != 3) return NULL;
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return NULL;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return NULL;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int is_local  = (l == 1 && nm[0] == ':');
    int is_global = (l == 2 && nm[0] == ':' && nm[1] == ':');
    ray_release(s);
    if (!is_local && !is_global) return NULL;
    if (!e[1] || e[1]->type != -RAY_SYM || (e[1]->attrs & Q_ATTR_QUOTED))
        return NULL;

    ray_t *ns = ray_sym_str(e[1]->i64);
    if (!ns) return NULL;
    int reserved = q_ops_is_reserved(ray_str_ptr(ns), (int)ray_str_len(ns));
    if (reserved) {
        ray_t *err = ray_error("assign", "'assign: %.*s is a reserved verb",
                               (int)ray_str_len(ns), ray_str_ptr(ns));
        ray_release(ns);
        return err;
    }
    ray_release(ns);

    const char *target = (is_local && in_lambda) ? "let" : "set";
    ray_t *setv = ql_env_val(target);
    if (!setv) return NULL;
    ray_retain(setv);
    ray_release(e[0]);
    e[0] = setv;                       /* in-place head swap, arity unchanged */
    return NULL;
}

/* ===== $[c;t;f] cond ========================================================
 * q Cond is a control construct spelled with the cast glyph: `$[test;et;ef]`
 * (ref/cond.md).  Detected by a `$`-provenance head with >= 3 args: the
 * parser's LBRACK arm embeds the DYADIC registry value (the cast wrapper) at
 * every `$[...]` head, so we probe provenance; a still-unresolved `$`
 * name-ref (cold-registry path) is matched by spelling.  A 2-arg `$[t;x]` is
 * bracket-call CAST and stays untouched. */
static int ql_is_dollar_head(ray_t *h) {
    if (!h) return 0;
    if (h->type == -RAY_SYM && !(h->attrs & Q_ATTR_QUOTED)) {
        ray_t *s = ray_sym_str(h->i64);
        int r = s && ray_str_len(s) == 1 && ray_str_ptr(s)[0] == '$';
        if (s) ray_release(s);
        return r;
    }
    if (h->type == RAY_BINARY) {
        q_provenance_t pv;
        return q_registry_provenance(h, &pv) && pv.spelling &&
               pv.spelling[0] == '$' && pv.spelling[1] == '\0';
    }
    return 0;
}

/* Lower `$[c1;t1;...;f]` onto rayfall `if` (env special form, ray_cond_fn:
 * lazy — branch args reach it UNEVALUATED — but TRIADIC ONLY: extra args are
 * ignored, so the kdb flattened multi-way `$[q;a;r;b;c]` <=> `$[q;a;$[r;b;c]]`
 * (ref/cond.md) is rebuilt as explicitly nested (if q a (if r b c)) lists,
 * folding pairs from the right.  Even arg counts have no kdb reading (the ref
 * defines triads + odd flattenings only) -> 'cond error.  Returns a RAY_ERROR
 * (owned) to abort lowering, else NULL. */
static ray_t *ql_cond(ray_t **slot) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    if (n < 4 || !ql_is_dollar_head(e[0])) return NULL;   /* $[c;t;f] = 4 slots */
    if (n % 2 != 0)                        /* head + even arg count: no reading */
        return ray_error("cond", "'cond: $[...] needs an odd number of expressions");
    ray_t *ifv = ql_env_val("if");
    if (!ifv) return NULL;                 /* borrowed; append retains below */
    ray_t *acc = e[n - 1];                 /* the final else */
    ray_retain(acc);
    for (int64_t i = n - 3; i >= 1; i -= 2) {
        ray_t *w = ray_list_new(4);
        w = ray_list_append(w, ifv);
        w = ray_list_append(w, e[i]);      /* cond_i  (append retains) */
        w = ray_list_append(w, e[i + 1]);  /* then_i */
        w = ray_list_append(w, acc);
        ray_release(acc);
        acc = w;
    }
    ray_release(node);
    *slot = acc;
    return NULL;
}
/* True iff node is a ctx-literal — a paren list `(…)` or table `([]…)` whose
 * head is the shared context-constructor value.  Its element statements run in
 * a pushed env scope (q_ctx_build), so a plain `:` inside must lower to `let`
 * (bind the scoped frame) exactly like a lambda body — the mandate's "bind INTO
 * the scope, pop back OUT". */
static int ql_is_ctx_literal(ray_t *node) {
    if (!node || node->type != RAY_LIST || ray_len(node) < 1) return 0;
    ray_t *h = ((ray_t **)ray_data(node))[0];
    if (!h) return 0;
    return h == q_registry_list_value() || h == q_registry_table_value();}

/* True iff node is the `{`-marker lambda form (its body statements bind q
 * locals with plain `:`). */
static int ql_is_lambda(ray_t *node) {
    if (!node || node->type != RAY_LIST || ray_len(node) < 1) return 0;
    ray_t *h = ((ray_t **)ray_data(node))[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return 0;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return 0;
    int r = (ray_str_len(s) == 1 && ray_str_ptr(s)[0] == '{');
    ray_release(s);
    return r;
}

/* ===== qSQL SELECT lowering (piece 3 executor) ==============================
 * The parser keeps `select … from t` / `?[t;c;b;a]` as the SYMBOLIC functional
 * 5-list (?;`t;c;b;a) so `parse` prints it kdb-true.  On the EVAL path only,
 * q_lower rewrites that 5-list onto the base ray_select engine:
 *
 *   (?;`t;c;b;a)  ->  (q.select; {from:t [where:…] [by:…] out:expr …}; keycols)
 *
 * The q column-symbol references (`a) are turned into bare rayfall column
 * name-refs (a) that ray_select resolves in column scope.  See q_select_exec
 * (q_registry.c) for the runtime side (ray_select + by-group keyed split). */

/* Deep-clear the QUOTED bit on every -RAY_SYM atom in a clause expr, so a q
 * column reference `a becomes a bare column name-ref a.  A genuine `sym literal
 * survives as an (enlisted) RAY_SYM VECTOR, which this leaves untouched. */
static void ql_unquote_cols(ray_t *x) {
    if (!x) return;
    if (x->type == -RAY_SYM) { x->attrs &= ~Q_ATTR_QUOTED; return; }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t **e = (ray_t **)ray_data(x);
        for (int64_t i = 0; i < n; i++) ql_unquote_cols(e[i]);
    }
}

/* Normalize a qSQL OUTPUT-column expr: unquote column refs AND turn an
 * embedded function-VALUE head back into a bare name-ref sym.  ray_select's
 * aggregate detection (is_agg_expr, query.c) keys on a -RAY_SYM head, so
 * `sum a` — which the parser embeds as (sum<value>;`a) — must become
 * (sum;a) or it evaluates element-wise (returning the column, not the sum). */
static void ql_qsql_out(ray_t *x) {
    if (!x) return;
    if (x->type == -RAY_SYM) { x->attrs &= ~Q_ATTR_QUOTED; return; }
    if (x->type != RAY_LIST) return;
    int64_t n = ray_len(x);
    ray_t **e = (ray_t **)ray_data(x);
    if (n >= 1 && e[0] && (e[0]->type == RAY_UNARY || e[0]->type == RAY_BINARY ||
                           e[0]->type == RAY_VARY)) {
        const char *nm = ray_fn_name(e[0]);
        if (nm && nm[0]) {
            ray_t *s = ray_sym(ray_sym_intern_runtime(nm, strlen(nm)));
            if (s && !RAY_IS_ERR(s)) { ray_release(e[0]); e[0] = s; }
            else if (s) ray_release(s);
        }
    }
    for (int64_t i = 0; i < n; i++) ql_qsql_out(e[i]);
}

/* A bare `&` dyadic application (rayfall min / logical-and) combining two
 * boolean where-masks.  Consumes neither operand's ownership (retained on
 * append); returns owned. */
static ray_t *ql_and(ray_t *l, ray_t *r) {
    ray_t *amp = q_verb('&');
    ray_t *n = ray_list_new(3);
    n = ray_list_append(n, amp); ray_release(amp);
    n = ray_list_append(n, l);
    n = ray_list_append(n, r);
    return n;
}

static void ql_qsql(ray_t **slot) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST || ray_len(node) != 5) return;
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return;
    ray_t *hs = ray_sym_str(h->i64);
    int is_q = hs && ray_str_len(hs) == 1 && ray_str_ptr(hs)[0] == '?';
    if (hs) ray_release(hs);
    if (!is_q) return;
    ray_t *sv = q_registry_select_value();
    if (!sv) return;

    ray_t *t = e[1], *c = e[2], *b = e[3], *a = e[4];
    if (!t) return;
    /* slot 1: bare name sym (by-name semantics) or an expression subtree —
     * q_lower_walk already lowered it (children walk before the node). */

    ray_t *keyvec  = ray_sym_vec_new(RAY_SYM_W64, 4);
    ray_t *vallist = ray_list_new(4);
    ray_t *keycols = ray_sym_vec_new(RAY_SYM_W64, 1);   /* by-key col names */

    /* NB `from:` is appended LAST: the base ray_select mis-handles a computed
     * output column (`sum a`) when `from:` is the FIRST dict key. */

    /* where: AND the constraints (right-to-left fold), cols unquoted */
    if (c && c->type == RAY_LIST && ray_len(c) == 1) {
        ray_t *clist = ((ray_t **)ray_data(c))[0];
        if (clist && clist->type == RAY_LIST && ray_len(clist) >= 1) {
            int64_t ncon = ray_len(clist);
            ray_t **cons = (ray_t **)ray_data(clist);
            ray_t *acc = NULL;
            for (int64_t i = ncon - 1; i >= 0; i--) {
                ray_t *cc = cons[i];
                if (!cc) continue;
                ray_retain(cc);
                ql_unquote_cols(cc);
                if (!acc) { acc = cc; }
                else { ray_t *nx = ql_and(cc, acc); ray_release(cc); ray_release(acc); acc = nx; }
            }
            if (acc) {
                keyvec  = q_symvec_append(keyvec, "where", 5);
                vallist = ray_list_append(vallist, acc); ray_release(acc);
            }
        }
    }

    /* by: single/multi grouping column(s); record their names as key cols */
    if (b && b->type == RAY_DICT) {
        ray_t *bk = ray_dict_keys(b);       /* names */
        ray_t *bv = ray_dict_vals(b);       /* sym vec or expr list */
        int64_t nb = ray_len(bk);
        int bv_sym = (bv && bv->type == RAY_SYM);
        ray_t *byexpr = NULL;
        for (int64_t i = 0; i < nb; i++) {
            ray_t *kn = ray_sym_vec_cell(bk, i);        /* borrowed -RAY_STR */
            keycols = q_symvec_append(keycols, ray_str_ptr(kn), (int)ray_str_len(kn));
            ray_t *ex;
            if (bv_sym) {
                ray_t *vn = ray_sym_vec_cell(bv, i);
                ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
            } else {
                ex = ((ray_t **)ray_data(bv))[i];
                ray_retain(ex);
                ql_unquote_cols(ex);
            }
            if (nb == 1) { byexpr = ex; }
            else { if (!byexpr) byexpr = ray_list_new(nb); byexpr = ray_list_append(byexpr, ex); ray_release(ex); }
        }
        if (byexpr) {
            keyvec  = q_symvec_append(keyvec, "by", 2);
            vallist = ray_list_append(vallist, byexpr); ray_release(byexpr);
        }
    }

    /* output columns: name -> expr.  A computed output column whose q name
     * equals a source column (`select sum a` -> col `a`) breaks the base
     * engine's `name: (expr name)` collision handling, so each output column
     * goes into ray_select under a UNIQUE temp name `QqcN` and q_select_exec
     * renames the result columns back to the real q names (tempnames/outnames
     * pair).  Order-independent (rename matches by temp name).  NB the temp
     * prefix must NOT start with `_` — the base engine treats leading-underscore
     * column names as reserved/hidden and drops them from the output. */
    ray_t *tempnames = ray_sym_vec_new(RAY_SYM_W64, 1);
    ray_t *outnames  = ray_sym_vec_new(RAY_SYM_W64, 1);
    if (a && a->type == RAY_DICT) {
        ray_t *ak = ray_dict_keys(a);
        ray_t *av = ray_dict_vals(a);
        int64_t na = ray_len(ak);
        int av_sym = (av && av->type == RAY_SYM);
        for (int64_t i = 0; i < na; i++) {
            char tmp[24];
            int tl = snprintf(tmp, sizeof tmp, "Qqc%lld", (long long)i);
            keyvec    = q_symvec_append(keyvec, tmp, tl);
            tempnames = q_symvec_append(tempnames, tmp, tl);
            ray_t *nm = ray_sym_vec_cell(ak, i);
            outnames  = q_symvec_append(outnames, ray_str_ptr(nm), (int)ray_str_len(nm));
            ray_t *ex;
            if (av_sym) {
                ray_t *vn = ray_sym_vec_cell(av, i);
                ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
            } else {
                ex = ((ray_t **)ray_data(av))[i];
                ray_retain(ex);
                ql_qsql_out(ex);
            }
            vallist = ray_list_append(vallist, ex); ray_release(ex);
        }
    }

    /* from: — appended LAST (see note above).  ANY sym slot unquotes to a
     * bare name-ref (env lookup): the parser's name form emits a QUOTED `t
     * (qsql_colsym), and a `sym from-EXPRESSION degrades to the same by-name
     * lookup — kdb's by-name semantics (funsql.md: t "may be a table or the
     * name of a table"; needed later for partitioned tables).  Any other
     * slot-1 value is an already-lowered expression subtree embedded as-is —
     * ray_select ray_eval()s the from: value (query.c), so a table value
     * flows in. */
    ray_t *fromv = (t->type == -RAY_SYM) ? ray_sym(t->i64)
                                         : (ray_retain(t), t);
    keyvec  = q_symvec_append(keyvec, "from", 4);
    vallist = ray_list_append(vallist, fromv); ray_release(fromv);

    ray_t *dict = ray_dict_new(keyvec, vallist);   /* consumes keyvec, vallist */
    if (!dict || RAY_IS_ERR(dict)) {
        if (dict) ray_release(dict);
        ray_release(keycols); ray_release(tempnames); ray_release(outnames);
        return;
    }

    ray_t *repl = ray_list_new(5);
    repl = ray_list_append(repl, sv);                  /* borrowed -> retained */
    repl = ray_list_append(repl, dict);      ray_release(dict);
    repl = ray_list_append(repl, keycols);   ray_release(keycols);
    repl = ray_list_append(repl, tempnames); ray_release(tempnames);
    repl = ray_list_append(repl, outnames);  ray_release(outnames);
    ray_release(node);
    *slot = repl;
}

/* ===== functional qSQL `?[t;c;b;a]` / `![t;c;b;a]` =========================
 * The by-VALUE functional forms parse as a rank-4 application whose head is the
 * DYADIC registry value for `?` (select/exec) or `!` (update/delete).  Rewrite
 * that head to the matching runtime executor value (q_registry_funsql_*), which
 * receives the four EVALUATED operands and drives the base query engine.  A
 * dict-make `k!v` (a `!` applied to only 2 args) is a 3-list and never matches.
 * The result-shaping (table / keyed table / row+column delete) lives in the
 * executors — see q_registry.c q_funsql_select / q_funsql_bang. */
static ray_t *ql_funsql_head(ray_t *h, char glyph) {
    if (!h || (h->type != RAY_UNARY && h->type != RAY_BINARY && h->type != RAY_VARY))
        return NULL;
    q_provenance_t pv;
    if (!q_registry_provenance(h, &pv) || !pv.spelling) return NULL;
    if (pv.spelling[0] != glyph || pv.spelling[1] != '\0') return NULL;
    return h;   /* borrowed */
}

static void ql_funsql(ray_t **slot) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST || ray_len(node) != 5) return;  /* head + 4 args */
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *repl_head = NULL;
    if (ql_funsql_head(e[0], '?'))      repl_head = q_registry_funsql_select_value();
    else if (ql_funsql_head(e[0], '!')) repl_head = q_registry_funsql_bang_value();
    if (!repl_head) return;
    ray_retain(repl_head);
    ray_release(e[0]);
    e[0] = repl_head;   /* args e[1..4] stay; eval evaluates them then applies */
}

/* Depth-first rewriting walker.  Children first so nested applications
 * ((+/) each ...) lower inside-out; then the node itself.  Returns a
 * RAY_ERROR (owned) on an 'assign violation, else NULL. */
static ray_t *q_lower_walk(ray_t **slot, int in_lambda, int is_head) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST) return NULL;
    assert(node->rc == 1);                 /* sole-owner precondition */
    int lambda_body = in_lambda || ql_is_lambda(node) || ql_is_ctx_literal(node);
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    for (int64_t i = 0; i < n; i++)
        if (e[i] && e[i]->type == RAY_LIST) {
            ray_t *err = q_lower_walk(&e[i], lambda_body, i == 0);
            if (err) return err;
        }
    ray_t *err = ql_cond(slot);            /* $[c;t;f] BEFORE any head claim */
    if (err) return err;
    ql_qsql(slot);                         /* (?;`t;c;b;a) -> ray_select call */
    ql_funsql(slot);                       /* ?[t;c;b;a] / ![t;c;b;a] runtime */
    ql_dyad_head(slot);                    /* keyword-dyadic bracket calls */
    err = ql_assign(slot, in_lambda);
    if (err) return err;
    ql_adv_app(slot);
    /* head position stays a raw (adv;V) 2-list — the PARENT's ql_adv_app
     * consumes it; everywhere else a bare derived verb becomes a carrier. */
    if (!is_head) ql_deriv_value(slot);
    return NULL;
}

/* q-lower entrypoint — see q_parse.h.  Dyadic-head resolution (the 2a shim
 * behaviour) plus the lowering walker above.  2b's parser flip moves the head
 * resolution into q_parse (the 2b flip).  May return a
 * RAY_ERROR (consuming `ast`): callers must error-check the result. */
ray_t *q_lower(ray_t *ast) {
    if (ast && ast->type == RAY_LIST) {
        ray_t *err = q_lower_walk(&ast, 0, 0);
        if (err) { ray_release(ast); return err; }
    }
    return ast;
}

/* q_ast_is_assign — see q_parse.h.  Checks the PRE-lower shape: head is the
 * name-ref `:`/`::` with a sym binding target; a `;` statement sequence asks
 * its last statement. */
int q_ast_is_assign(const ray_t *cast) {
    ray_t *ast = (ray_t *)cast;   /* read-only walk; ray_data lacks a const view */
    if (!ast || ast->type != RAY_LIST || ray_len(ast) < 1) return 0;
    ray_t **e = (ray_t **)ray_data(ast);
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return 0;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return 0;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int is_semi  = (l == 1 && nm[0] == ';');
    int is_colon = (l == 1 && nm[0] == ':') ||
                   (l == 2 && nm[0] == ':' && nm[1] == ':');
    ray_release(s);
    if (is_semi) {
        int64_t n = ray_len(ast);
        return n >= 2 ? q_ast_is_assign(e[n - 1]) : 0;
    }
    return is_colon && ray_len(ast) == 3 &&
           e[1] && e[1]->type == -RAY_SYM && !(e[1]->attrs & Q_ATTR_QUOTED);
}
