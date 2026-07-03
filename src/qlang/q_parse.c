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
#include "core/numparse.h"   /* ray_parse_i64, ray_parse_f64 */
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

/* marker heads ("{", ";", adverb names): name-ref syms */
static ray_t *q_marker(const char *s) {
    return ray_sym(ray_sym_intern_runtime(s, strlen(s)));
}

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

#define EMIT(TK, KK) do { \
        if (n >= cap) { cap = cap ? cap * 2 : 32; toks = realloc(toks, (size_t)cap * sizeof(Token)); \
                        if (!toks) q_die("out of memory"); } \
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

        int neg_sign = (c == '-' && (CLASS[(uint8_t)src[p+1]] & CL_DIGIT) && !noun_pos);

        if ((cl & CL_DIGIT) || neg_sign) {
            EMIT(T_NOUN, scan_num_literal(src, &p));
            noun_pos = 1;
        }
        else if (cl & CL_ALPHA) {
            while (CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) p++;
            while (src[p] == '.' && (CLASS[(uint8_t)src[p+1]] & CL_ALPHA)) {
                p++;
                while (CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) p++;
            }
            int len = p - start;
            if (len >= MAX_NAME) q_die("name too long");
            EMIT(T_NOUN, q_name(src + start, len));
            noun_pos = 1;
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
            p++;
            if (src[p] == ':') p++;   /* absorb monadic marker; valence at eval */
            EMIT(T_VERB, q_verb(c));
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
        if (ray_len(e) == 1) {
            ray_t **slots = (ray_t **)ray_data(e);
            ray_t *only = slots[0];
            if (only) ray_retain(only);
            ray_release(e);
            return (P){ R_NOUN, only };
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
        ray_t *xs[3] = { u.v, t.v, rhs };
        return (P){ R_NOUN, q_list(xs, 3) };
    }

    P e = parse_e_from(p, u);
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
