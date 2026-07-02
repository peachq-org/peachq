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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
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

static ray_t *q_int(int64_t v) { return ray_i64(v); }

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

/* Build a ray list from n owned children, releasing each after append
 * (append retains).  Children must be non-NULL. */
static ray_t *q_list(ray_t **xs, int n) {
    ray_t *l = ray_list_new(n > 0 ? n : 1);
    for (int i = 0; i < n; i++) {
        l = ray_list_append(l, xs[i]);
        if (xs[i]) ray_release(xs[i]);
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

static int64_t scan_int(const char *src, int *p) {
    int neg = 0;
    if (src[*p] == '-') { neg = 1; (*p)++; }
    int64_t limit = (int64_t)INT_MAX + (neg ? 1 : 0);
    int64_t n = 0;
    while (CLASS[(uint8_t)src[*p]] & CL_DIGIT) {
        n = n * 10 + (src[*p] - '0');
        if (n > limit) q_die("integer literal out of range");
        (*p)++;
    }
    return neg ? -n : n;
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
        while (CLASS[(uint8_t)src[p]] & CL_WS) p++;
        if (!src[p]) break;

        int start = p;
        char c = src[p];
        uint8_t cl = CLASS[(uint8_t)c];

        int neg_sign = (c == '-' && (CLASS[(uint8_t)src[p+1]] & CL_DIGIT) && !noun_pos);

        if ((cl & CL_DIGIT) || neg_sign) {
            int64_t buf[MAX_VEC]; int m = 0;
            buf[m++] = scan_int(src, &p);
            for (;;) {
                int sp = p;
                while (CLASS[(uint8_t)src[sp]] & CL_WS) sp++;
                if (sp == p) break;
                int has_dig = CLASS[(uint8_t)src[sp]] & CL_DIGIT;
                int has_neg = src[sp] == '-' && (CLASS[(uint8_t)src[sp+1]] & CL_DIGIT);
                if (!has_dig && !has_neg) break;
                if (m >= MAX_VEC) q_die("int vector literal too long");
                p = sp;
                buf[m++] = scan_int(src, &p);
            }
            ray_t *k = (m == 1) ? q_int(buf[0])
                                : ray_vec_from_raw(RAY_I64, buf, m);
            EMIT(T_NOUN, k);
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
        else if (c == '`') {
            /* One or more `name — a single one is a sym-literal atom; more
             * than one is left as a sym-literal atom of the first for the MVP
             * (multi-sym vectors are out of the tested subset). */
            ray_t *k = NULL;
            int count = 0;
            while (src[p] == '`') {
                p++;
                int s = p;
                while ((CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) || src[p] == '.') p++;
                if (count == 0) k = q_symlit(src + s, p - s);
                count++;
            }
            EMIT(T_NOUN, k);
            noun_pos = 1;
        }
        else if (cl & CL_VERB) {
            p++;
            if (src[p] == ':') p++;   /* absorb monadic marker; valence at eval */
            EMIT(T_VERB, q_verb(c));
            noun_pos = 0;
        }
        else if (cl & CL_ADVERB) {
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
    return q_list(buf, n);
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
    return prog;
}

/* ===== ray_t -> kparser-style lisp printer (test oracle) ===================== */

typedef struct { char *p; size_t rem; } SB;

static void sb_puts(SB *sb, const char *s) {
    size_t n = strlen(s);
    if (n > sb->rem) n = sb->rem;
    memcpy(sb->p, s, n);
    sb->p += n; sb->rem -= n;
    if (sb->rem) *sb->p = '\0';
}

static void sb_putc(SB *sb, char c) { char t[2] = { c, '\0' }; sb_puts(sb, t); }

/* A sym prints bare (no leading `) only when it is a verb or the generic
 * null; names, sym-literals, and the {/ ; markers keep their backtick — the
 * same split kparser's print_k makes between KV1/KV2 and -KS/KS. */
static int q_bare(const char *s, size_t len) {
    if (len == 1 && s[0] && strchr(VERB_CHARS, s[0])) return 1;
    if (len == 2 && s[0] == ':' && s[1] == ':') return 1;
    return 0;
}

static void q_fmt_rec(ray_t *x, SB *sb) {
    if (!x) { sb_puts(sb, "()"); return; }
    if (RAY_IS_ERR(x)) { sb_puts(sb, "err"); return; }

    switch (x->type) {
    case -RAY_I64: {
        char b[32]; snprintf(b, sizeof b, "%lld", (long long)x->i64);
        sb_puts(sb, b);
        break;
    }
    case RAY_I64: {
        int64_t n = ray_len(x);
        int64_t *d = (int64_t *)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            if (i) sb_putc(sb, ' ');
            char b[32]; snprintf(b, sizeof b, "%lld", (long long)d[i]);
            sb_puts(sb, b);
        }
        break;
    }
    case -RAY_SYM: {
        ray_t *s = ray_sym_str(x->i64);
        const char *nm = ray_str_ptr(s);
        size_t len = ray_str_len(s);
        if (!q_bare(nm, len)) sb_putc(sb, '`');
        char b[MAX_NAME + 1];
        int wl = (int)(len < MAX_NAME ? len : MAX_NAME);
        snprintf(b, sizeof b, "%.*s", wl, nm);
        sb_puts(sb, b);
        ray_release(s);
        break;
    }
    case RAY_LIST: {
        sb_putc(sb, '(');
        int64_t n = ray_len(x);
        ray_t **e = (ray_t **)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            if (i) sb_putc(sb, ';');
            q_fmt_rec(e[i], sb);
        }
        sb_putc(sb, ')');
        break;
    }
    default: {
        char b[32]; snprintf(b, sizeof b, "?t=%d", (int)x->type);
        sb_puts(sb, b);
    }
    }
}

void q_ast_fmt(ray_t *ast, char *buf, size_t bufsz) {
    if (bufsz == 0) return;
    SB sb = { buf, bufsz - 1 };
    buf[0] = '\0';
    q_fmt_rec(ast, &sb);
}
