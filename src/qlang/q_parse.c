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
            TKind kk;
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
        /* q_die() longjmped here; free whatever the scanner had emitted. */
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
    ray_t **e = (ray_t **)ray_data(node);
    if (n != 3 || !e[0] || e[0]->type != -RAY_SYM || (e[0]->attrs & Q_ATTR_QUOTED))
        return;
    ray_t *s = ray_sym_str(e[0]->i64);
    if (!s) return;
    ray_t *hit = q_registry_lookup_name(ray_str_ptr(s), ray_str_len(s), Q_DYADIC);
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
    ray_t *args[2] = { e[1], NULL };         /* V bound, data operand open */
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

/* Depth-first rewriting walker.  Children first so nested applications
 * ((+/) each ...) lower inside-out; then the node itself.  Returns a
 * RAY_ERROR (owned) on an 'assign violation, else NULL. */
static ray_t *q_lower_walk(ray_t **slot, int in_lambda, int is_head) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST) return NULL;
    assert(node->rc == 1);                 /* sole-owner precondition */
    int lambda_body = in_lambda || ql_is_lambda(node);
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    for (int64_t i = 0; i < n; i++)
        if (e[i] && e[i]->type == RAY_LIST) {
            ray_t *err = q_lower_walk(&e[i], lambda_body, i == 0);
            if (err) return err;
        }
    ql_dyad_head(slot);                    /* keyword-dyadic bracket calls */
    ray_t *err = ql_assign(slot, in_lambda);
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
