/* q_highlight — see q_highlight.h.  Token shapes follow q_parse.c's scanner; literals go through q_tok itself. */
#include "qlang/repl/q_highlight.h"
#include "qlang/parse/q_tok.h"             /* q_tok_magnitude / q_tok_type_letter / q_tok_byte_lit_starts */
#include "qlang/parse/q_parse_internal.h"  /* VERB_CHARS */
#include "qlang/base/q_type.h"             /* q_type_of_char */
#include "qlang/hl_names_gen.h"            /* Q_HL_NAMES */
#include <stdlib.h>
#include <string.h>

/* 256-colour, mid-luminance: every role must read on a dark and on a light background. */
#define HL_KEYWORD  "\033[38;5;33m"
#define HL_STRING   "\033[38;5;34m"
#define HL_ESCAPE   "\033[38;5;168m"
#define HL_COMMENT  "\033[38;5;244m"
#define HL_SYMBOL   "\033[38;5;37m"
#define HL_NUMBER   "\033[38;5;166m"
#define HL_TEMPORAL "\033[38;5;136m"
#define HL_OP       "\033[38;5;98m"
#define HL_SYSTEM   "\033[38;5;127m"
#define HL_COMMAND  "\033[38;5;160m"
#define HL_MATCH    "\033[7m"
#define HL_RESET    "\033[0m"
#define HL_RESET_N  ((int32_t)sizeof HL_RESET - 1)

typedef struct {
    char*       dst;
    int32_t     cap;
    int32_t     n;
    int         full;
    int         noun;
    int32_t     m1, m2;
    const char* buf;
    int32_t     len;
} hl_t;

typedef struct { const char* s; size_t n; } hl_key;

/* Every write but a reset leaves room for one, so a full buffer still ends on a coherent, uncoloured prefix. */
static int32_t room(const hl_t* h) { return h->full ? 0 : h->cap - 1 - HL_RESET_N - h->n; }

static void text(hl_t* h, const char* s, int32_t n) {
    if (n > room(h)) { n = room(h) > 0 ? room(h) : 0; h->full = 1; }
    memcpy(h->dst + h->n, s, (size_t)n);
    h->n += n;
}

static int code(hl_t* h, const char* s) {
    int32_t n = (int32_t)strlen(s);
    if (n >= room(h)) { h->full = 1; return 0; }
    memcpy(h->dst + h->n, s, (size_t)n);
    h->n += n;
    return 1;
}

static void reset(hl_t* h) {
    memcpy(h->dst + h->n, HL_RESET, HL_RESET_N);
    h->n += HL_RESET_N;
}

static void span(hl_t* h, const char* colour, int32_t from, int32_t to) {
    int open = 0;
    for (int32_t i = from; i < to && !h->full; ) {
        if (i == h->m1 || i == h->m2) {
            if (open) { reset(h); open = 0; }
            if (code(h, HL_MATCH)) { text(h, h->buf + i, 1); reset(h); }
            i++;
            continue;
        }
        int32_t j = i + 1;
        while (j < to && j != h->m1 && j != h->m2) j++;
        if (colour && !open && !(open = code(h, colour))) break;
        text(h, h->buf + i, j - i);
        i = j;
    }
    if (open) reset(h);
}

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_word(char c)  { return is_alpha(c) || is_digit(c) || c == '_'; }

static int key_cmp(const void* k, const void* e) {
    const hl_key* key = k;
    const char*   ent = *(const char* const*)e;
    size_t        en  = strlen(ent);
    int           r   = memcmp(key->s, ent, key->n < en ? key->n : en);
    return r ? r : (key->n > en) - (key->n < en);
}

static int documented(const char* s, int32_t n) {
    hl_key k = { s, (size_t)n };
    return bsearch(&k, Q_HL_NAMES, sizeof Q_HL_NAMES / sizeof *Q_HL_NAMES, sizeof *Q_HL_NAMES, key_cmp) != NULL;
}

static const char* name_colour(const char* s, int32_t n) {
    if (n > 3 && memcmp(s, ".q.", 3) == 0) return documented(s + 3, n - 3) ? HL_KEYWORD : NULL;
    if (n > 3 && memcmp(s, ".z.", 3) == 0) return HL_SYSTEM;
    if (!documented(s, n)) return NULL;
    return s[0] == '.' ? HL_SYSTEM : HL_KEYWORD;
}

static int32_t rest_of_line(hl_t* h, const char* colour, int32_t i) {
    int32_t j = i;
    while (j < h->len && h->buf[j] != '\n') j++;
    span(h, colour, i, j);
    return j;
}

/* The escapes the parser decodes: \n \t \r \" \\ and 1..3 octal digits.  Returns j when the `\` starts none. */
static int32_t escape_end(const hl_t* h, int32_t j) {
    char e = h->buf[j + 1];
    if (e == 'n' || e == 't' || e == 'r' || e == '"' || e == '\\') return j + 2;
    int32_t k = j + 1;
    while (k < h->len && k < j + 4 && h->buf[k] >= '0' && h->buf[k] <= '7') k++;
    return k > j + 1 ? k : j;
}

static int32_t lex_string(hl_t* h, int32_t i) {
    int32_t run = i, j = i + 1;
    while (j < h->len && h->buf[j] != '"') {
        if (h->buf[j] != '\\' || j + 1 >= h->len) { j++; continue; }
        int32_t e = escape_end(h, j);
        if (e == j) { j += 2; continue; }
        span(h, HL_STRING, run, j);
        span(h, HL_ESCAPE, j, e);
        run = j = e;
    }
    if (j < h->len) j++;
    span(h, HL_STRING, run, j);
    return j;
}

/* A handle takes ':' and '/' anywhere; a bare symbol only where more body follows (`a: is still the verb). */
static int32_t lex_symbol(hl_t* h, int32_t i) {
    const char* b = h->buf;
    int32_t     j = i + 1;
    if (j < h->len && b[j] == ':') {
        while (++j < h->len && (is_word(b[j]) || b[j] == '.' || b[j] == ':' || b[j] == '/')) {}
    } else {
        for (;;) {
            while (j < h->len && (is_word(b[j]) || b[j] == '.')) j++;
            int32_t sep = j;
            while (sep < h->len && (b[sep] == ':' || b[sep] == '/')) sep++;
            if (sep == j || sep >= h->len || !(is_word(b[sep]) || b[sep] == '.')) break;
            j = sep;
        }
    }
    span(h, HL_SYMBOL, i, j);
    return j;
}

static int32_t lex_name(hl_t* h, int32_t i) {
    const char* b = h->buf;
    int32_t     j = i;
    while (j < h->len && is_word(b[j])) j++;
    while (j + 1 < h->len && b[j] == '.' && is_word(b[j + 1]))
        for (j++; j < h->len && is_word(b[j]); j++) {}
    span(h, name_colour(b + i, j - i), i, j);
    return j;
}

static const char* literal_colour(const q_tok_el* el, char letter) {
    int8_t t = q_type_of_char(letter);
    if (el->kind == Q_TOK_EL_NULL || el->kind == Q_TOK_EL_PINF || el->kind == Q_TOK_EL_NINF) return HL_TEMPORAL;
    if (t) return RAY_IS_TEMPORAL32(t) || RAY_IS_TEMPORAL64(t) || RAY_IS_TEMPORALF(t) ? HL_TEMPORAL : HL_NUMBER;
    return el->kind == Q_TOK_EL_INT || el->kind == Q_TOK_EL_FLOAT || el->kind == Q_TOK_EL_MONTH ? HL_NUMBER : HL_TEMPORAL;
}

/* i sits on a digit, a `.` before a digit, or a sign `-`. */
static int32_t lex_number(hl_t* h, int32_t i) {
    const char* b = h->buf;
    int32_t     d = i + (b[i] == '-'), j = d;
    while (j < h->len && is_digit(b[j])) j++;
    if (j > d && j < h->len && (b[i] == '-' ? b[j] == '!' : b[j] == ':' && documented(b + i, j + 1 - i))) {
        span(h, HL_SYSTEM, i, j + 1);
        return j + 1;
    }
    char    z[128];
    int32_t zn = h->len - i < (int32_t)sizeof z - 1 ? h->len - i : (int32_t)sizeof z - 1;
    memcpy(z, b + i, (size_t)zn);
    z[zn] = '\0';
    if (q_tok_byte_lit_starts(z, d - i)) {
        if (d > i) { span(h, HL_OP, i, d); return d; }
        for (j = i + 2; j < h->len && strchr("0123456789abcdefABCDEF", b[j]); j++) {}
        span(h, HL_NUMBER, i, j);
        return j;
    }
    q_tok_el    el;
    const char* err    = NULL;
    char        letter = 0;
    int         p      = 0;
    if (q_tok_magnitude(z, &p, &el, &err) != 1 || p == 0 || !q_tok_type_letter(z, &p, &letter, &el, &err)) {
        for (j = d; j < h->len && (is_word(b[j]) || b[j] == '.'); j++) {}
        span(h, HL_NUMBER, i, j);
        return j;
    }
    span(h, literal_colour(&el, letter), i, i + p);
    return i + p;
}

/* The parser's sign gate: a `-` or `.` glued to a noun is the verb; after a blank or a verb it starts a number. */
static int32_t lex_one(hl_t* h, int32_t i) {
    const char* b     = h->buf;
    char        c     = b[i];
    char        prev  = i > 0 ? b[i - 1] : '\n';
    char        n1    = i + 1 < h->len ? b[i + 1] : '\0';
    char        n2    = i + 2 < h->len ? b[i + 2] : '\0';
    int         blank = prev == '\n' || prev == ' ' || prev == '\t';
    int         num   = (!h->noun || blank) &&
                        (c == '.' ? is_digit(n1) : c == '-' && (is_digit(n1) || (n1 == '.' && is_digit(n2))));
    int32_t     j     = i + 1;
    int         noun  = 1;
    if (c == '\\' && prev == '\n')                                      { j = rest_of_line(h, HL_COMMAND, i); noun = 0; }
    else if (c == '/' && blank)                                         { j = rest_of_line(h, HL_COMMENT, i); noun = 0; }
    else if (c == '"')                                                  j = lex_string(h, i);
    else if (c == '`')                                                  j = lex_symbol(h, i);
    else if (is_alpha(c) || (c == '.' && (is_alpha(n1) || n1 == '_'))) j = lex_name(h, i);
    else if (is_digit(c) || num)                                        j = lex_number(h, i);
    else {
        noun = c == ')' || c == ']' || c == '}' || ((c == ' ' || c == '\t') && h->noun);
        span(h, c && (strchr(VERB_CHARS, c) || strchr("'/\\", c)) ? HL_OP : NULL, i, j);
    }
    h->noun = noun;
    return j;
}

int32_t q_highlight(char* dst, int32_t dst_cap, const char* buf, int32_t buf_len, int32_t match_pos1,
                    int32_t match_pos2) {
    hl_t h = { dst, dst_cap, 0, 0, 0, match_pos1, match_pos2, buf, buf_len };
    for (int32_t i = 0; i < buf_len && !h.full; ) i = lex_one(&h, i);
    return h.n;
}
