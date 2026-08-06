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
 * AST shapes (a verb head is its embedded registry VALUE; only a non-verb name
 * stays a reference sym for eval to resolve):
 *   n v e   -> (v; n; e)
 *   t e     -> (t; e)         (lone term collapses to t)
 *   t[E]    -> (t; e1; ...)
 *   {E}     -> the RAY_QFN carrier VALUE (built at parse)
 *   tA      -> (`A; t)
 */

#define _POSIX_C_SOURCE 200809L

#include "qlang/parse/q_parse.h"
#include "qlang/base/q_err.h"
#include "qlang/parse/q_tok.h"    /* q_tok_temporal, q_tok_el — literal magnitudes */
#include "qlang/q_registry.h" /* q_registry_lookup_name, Q_DYADIC */
#include "qlang/q_ops.h"      /* q_lex_is_kw_infix — static lexical manifest */
#include "qlang/eval/q_eval.h" /* q_eval_apply_is_fn, q_eval_apply_carrier_kind */
#include "qlang/q_env.h"     /* q_env_get — registry-alias qSQL phrase heads */
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
#include "qlang/parse/q_parse_internal.h"


/* ===== parse-error escape =====================================================
 * kparser die()s (exit) on malformed input.  A REPL must not exit, so a
 * malformed parse longjmps back to q_parse, which returns a ray_error. */
static jmp_buf q_err_jmp;
static q_err_e g_die_class = QE_PARSE;   /* q_die_err overrides for one longjmp */

static _Noreturn void q_die(const char *msg) {
    (void)msg;   /* class-only errors (bare 'parse); msg documents the call site */
    longjmp(q_err_jmp, 1);
}

static _Noreturn void q_die_err(q_err_e cls) {
    g_die_class = cls;
    longjmp(q_err_jmp, 1);
}

/* ===== ray_t leaf builders =================================================== */

/* name reference (ATTR_QUOTED clear): resolved by eval */
static ray_t *i_name(const char *s, int len) {
    return ray_sym(ray_sym_intern_runtime(s, (size_t)len));
}

/* verb: a name-ref sym of the verb char, e.g. "+", ":".  Monadic vs dyadic
 * is a valence decision left to eval, so we drop kparser's KV1/KV2 split. */
ray_t *q_verb(char c) {
    char b[2] = { c, '\0' };
    return ray_sym(ray_sym_intern_runtime(b, 1));
}

/* verb by name (multi-char glyph like "<=", "<>", or keyword like "div"):
 * a name-ref sym, ATTR_QUOTED clear; the parser embeds its registry value. */
ray_t *q_verb_name(const char *s, int len) {
    return ray_sym(ray_sym_intern_runtime(s, (size_t)len));
}

/* Infix keyword verbs (q keyword functions usable between two nouns).  Derived
 * from the SINGLE-SOURCE op manifest (q_ops.c QLEX_KW_INFIX rows) — no longer a
 * hardcoded memcmp, and no runtime-registry dependency (the manifest is a
 * static table, and the scanner runs before eval).  The manifest's KW_INFIX
 * set is {div, each, in, within}. */
static int q_is_kw_verb(const char *s, int len) {
    return q_lex_is_kw_infix(s, len);
}

/* An iterator IS a value (103h, basics/datatypes.md), so the scanner hands
 * the parser the immutable registry value rather than a name-ref marker —
 * term position, postfix position and `(/;+)` then agree by construction. */
static ray_t *iter_value(int adv) {
    ray_t *v = q_registry_iter_value(adv);
    if (!v) q_die("iterator: registry not initialized");
    ray_retain(v);
    return v;
}

/* Embed the registry function VALUE for a verb sym at the given valence — the
 * 2b parser flip (`parse "2+3"` -> (+<fn>;2;3)).  A monadic-marked spelling
 * ("<g>:") probes the registry under the bare glyph.  On a miss the sym is
 * returned unchanged (unknown -> name-ref, ADR 0002).  Consumes `sym`,
 * returns owned. */
ray_t *q_embed(ray_t *sym, q_valence_t val) {
    if (!sym || sym->type != -RAY_SYM || (sym->attrs & Q_ATTR_QUOTED)) return sym;
    ray_t *s = ray_sym_str(sym->i64);
    if (!s) return sym;
    const char *nm = ray_str_ptr(s);
    size_t nl = ray_str_len(s);
    /* `:`/`::` heads are assignment/return SYNTAX (the walker dispatches on
     * the colon sym); the `:` registry row serves operand position only */
    if (nm[0] == ':') { ray_release(s); return sym; }
    if (val == Q_MONADIC && nl == 2 && nm[1] == ':') nl = 1;   /* "+:" -> "+" */
    ray_t *hit = q_registry_lookup_name(nm, nl, val);
    ray_release(s);
    if (!hit) return sym;
    /* q.q-hosted (QK_QSRC) carriers embed too: eval retains a non-sym head as a
     * value rather than re-walking it (#351) */
    if (!q_eval_apply_is_fn(hit)) return sym;
    ray_retain(hit);
    ray_release(sym);
    return hit;
}

/* True iff a sym spells a glyph verb (1 char from VERB_CHARS, optionally with
 * the monadic marker) — used to keep bare-verb embedding away from user
 * names, which must stay env-resolved name-refs. */
static int sym_is_glyph(ray_t *sym);   /* defined after VERB_CHARS */

/* generic null :: — the elided-argument hole */
ray_t *q_null(void) {
    return ray_sym(ray_sym_intern_runtime("::", 2));
}

/* An ELIDED bracket-call slot `f[a;;b]` — a projection hole.  Same `::`
 * spelling (so every existing hole check still matches), plus Q_ATTR_HOLE so
 * the @/. lowering can tell it from an explicit `::` value. */
static ray_t *hole(void) {
    ray_t *x = q_null();
    if (x && !RAY_IS_ERR(x)) x->attrs |= Q_ATTR_HOLE;
    return x;
}

/* True iff v is a NAME-REF sym (unquoted -RAY_SYM) whose spelling is exactly s
 * — the one home for the "read a sym back and compare it" walk the tree checks
 * repeat (`;` statement heads, `:` alias heads, the `::` value, the join `,`). */
static int sym_name_is(const ray_t *v, const char *s) {
    if (!v || v->type != -RAY_SYM || (v->attrs & Q_ATTR_QUOTED)) return 0;
    ray_t *str = ray_sym_str(v->i64);
    if (!str) return 0;
    size_t sl = strlen(s);
    int r = ray_str_len(str) == sl && memcmp(ray_str_ptr(str), s, sl) == 0;
    ray_release(str);
    return r;
}

/* Append an already-interned symbol id to a RAY_SYM vector.  The id-taking
 * twin of q_symvec_append: callers that hold an id must not round-trip it
 * through the string table just to intern it back. */
static ray_t *symvec_add(ray_t *vec, int64_t id) {
    return ray_vec_append(vec, &id);
}

/* symbol literal (ATTR_QUOTED set) */
static ray_t *symlit(const char *s, int len) {
    ray_t *x = ray_sym(ray_sym_intern_runtime(s, (size_t)len));
    if (x && !RAY_IS_ERR(x)) x->attrs |= Q_ATTR_QUOTED;
    return x;
}

/* Sym CONSTANTS entering a tree are visibly enlisted (parsetrees.md:80 — a
 * bare sym in a tree is a name reference): atom -> 1-elem sym vector (,`x),
 * vector -> 1-elem general list (,`a`b`c).  The scanner's Q_ATTR_QUOTED mark
 * stays token-internal; the emitted tree is attr-free data.  Consumes v. */
static ray_t *noun_tree_value(ray_t *v) {
    if (!v || RAY_IS_ERR(v)) return v;
    if (v->type == -RAY_SYM && (v->attrs & Q_ATTR_QUOTED)) {
        ray_t *vec = symvec_add(ray_sym_vec_new(RAY_SYM_W64, 1), v->i64);
        ray_release(v);
        return vec;
    }
    if (v->type == RAY_SYM) {
        ray_t *l = ray_list_new(1);
        l = ray_list_append(l, v);
        ray_release(v);
        return l;
    }
    return v;
}

/* Append one interned symbol id into a RAY_SYM vector (W64 index width: no
 * public width-picker, and correctness beats compactness for literals). */
ray_t *q_symvec_append(ray_t *vec, const char *s, int len) {
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

/* (head; e0; e1; …) — a fresh list of `head` followed by every element of the
 * BORROWED list e.  `nul` fills an empty slot (NULL: keep the C NULL, which
 * only the top-level statement list is allowed to carry). */
static ray_t *cons_head(ray_t *head, ray_t *e, ray_t *(*nul)(void)) {
    int64_t n = ray_len(e);
    ray_t **es = (ray_t **)ray_data(e);
    ray_t *w = ray_list_new(n + 1);
    w = ray_list_append(w, head);
    for (int64_t i = 0; i < n; i++) {
        if (es[i] || !nul) { w = ray_list_append(w, es[i]); continue; }
        ray_t *x = nul();
        w = ray_list_append(w, x);
        ray_release(x);
    }
    return w;
}

/* ===== verb / adverb tables ================================================== */

const char VERB_CHARS[] = ":+-*%!&|<>=~,^#_$?@.";

const char *ADVERB_NAMES[] = { "'", "/", "\\", "':", "/:", "\\:" };

static int sym_is_glyph(ray_t *sym) {
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
    CLASS[(int)'\n'] |= CL_WS;   /* multiline scripts: a joined logical line
                                  * carries embedded newlines between its
                                  * continuation fragments (q_ctx_run_file). */
    CLASS[(int)'\r'] |= CL_WS;
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

/* Whole literals (numeric / boolean / byte / guid / temporal) are scanned and
 * BUILT by q_tok.c — the single string->value home the `$` Tok scanners already
 * share.  Here it only has to become a token or a 'parse. */
static ray_t *scan_num_literal(const char *src, int *p) {
    const char *err = NULL;
    ray_t *v = q_tok_literal(src, p, &err);
    if (!v) q_die(err);
    return v;
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
                        !q_tok_byte_lit_starts(src, p + 1) &&   /* -0x0a: '-' stays the verb (bytes are unsigned) */
                        (!noun_pos || p == 0 || (CLASS[(uint8_t)src[p-1]] & CL_WS)));

        /* Leading-dot float literal: '.' glued to a digit starts a number
         * (`.2` -> 0.2, `2+.5` -> 2.5, `cos (.2;.3 .4)`) under the SAME gate
         * as the sign rule — glued to a preceding noun (`x.2`, `.2.3` after
         * `.2`) the '.' stays the apply/index verb (kdb's exact behaviour
         * there is not doc-pinned; no-churn).  `.z`-style namespace names
         * are untouched (gate requires a DIGIT after the dot); the strand
         * continuation (`2 .3`) already accepted these via ray_parse_f64. */
        int dot_float = (c == '.' && (CLASS[(uint8_t)src[p+1]] & CL_DIGIT) &&
                         (!noun_pos || p == 0 || (CLASS[(uint8_t)src[p-1]] & CL_WS)));

        /* kdb digit-colon verbs: `0:` (File Text; ref/file-text.md), `1:`/`2:`
         * (File Binary / Dynamic Load — tokenized identically; without a
         * manifest row the name stays a name-ref per the registry-miss rule).
         * A single digit glued to ':' can never start a clock literal (minute
         * / second / time / timespan all need tok_dig_run == 2 before ':'), so
         * this is unambiguous.  `0::` is left alone (the second ':' would be
         * a monadic marker / assign shape, not the verb). */
        if ((c == '0' || c == '1' || c == '2') &&
            src[p+1] == ':' && src[p+2] != ':') {
            char nm[2] = { c, ':' };
            p += 2;
            EMIT(T_VERB, q_verb_name(nm, 2));
            noun_pos = 0;
        }
        else if ((cl & CL_DIGIT) || neg_sign || dot_float) {
            EMIT(T_NOUN, scan_num_literal(src, &p));
            noun_pos = 1;
        }
        /* q names cannot START with '_' (leading '_' is the drop/cut verb);
         * interior '_' stays a name byte (a_b) via the CL_ALPHA continuation
         * loops below, so only the token-start byte is excluded here. */
        else if (((cl & CL_ALPHA) && c != '_') ||
                 (c == '.' && (CLASS[(uint8_t)src[p+1]] & CL_ALPHA))) {
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
                EMIT(T_NOUN, i_name(src + start, len));
                noun_pos = 1;
            }
        }
        else if (c == '"') {
            /* String literal: scan to the closing quote, honouring `\` escapes
             * (the escaped byte is skipped so an escaped quote does not close
             * the string).  Escapes DECODE here (feat/q-file-text): \" \\ \n
             * \t \r and 1..3-digit octal \ooo (the "\001" FIX-separator idiom
             * in ref/file-text.md).  q_fmt re-escapes on display, so literal
             * round-trips are unchanged; `count "a\nb"` becomes kdb-true. */
            p++;                     /* past opening quote */
            int s = p;
            while (src[p] && src[p] != '"') {
                if (src[p] == '\\' && src[p+1]) p += 2;   /* skip escaped char */
                else p++;
            }
            if (src[p] != '"') q_die("unterminated string");
            int len = p - s;
            p++;                     /* past closing quote */
            char* db = malloc((size_t)len + 1);
            if (!db) q_die("out of memory");
            int dl = 0;
            for (int i = 0; i < len; ) {
                char ch = src[s + i];
                if (ch != '\\' || i + 1 >= len) { db[dl++] = ch; i++; continue; }
                char esc = src[s + i + 1];
                if      (esc == 'n')  { db[dl++] = '\n'; i += 2; }
                else if (esc == 't')  { db[dl++] = '\t'; i += 2; }
                else if (esc == 'r')  { db[dl++] = '\r'; i += 2; }
                else if (esc == '"')  { db[dl++] = '"';  i += 2; }
                else if (esc == '\\') { db[dl++] = '\\'; i += 2; }
                else if (esc >= '0' && esc <= '7') {
                    int v = 0, k = 0;
                    while (k < 3 && i + 1 + k < len &&
                           src[s + i + 1 + k] >= '0' && src[s + i + 1 + k] <= '7') {
                        v = v * 8 + (src[s + i + 1 + k] - '0');
                        k++;
                    }
                    db[dl++] = (char)v;
                    i += 1 + k;
                }
                else { db[dl++] = ch; i++; }   /* unknown escape: keep the '\' */
            }
            /* string-C3: "a" is a char ATOM (-10h), anything else a char
             * VECTOR (10h) — "" included (THE empty char vector). */
            if (dl == 1) EMIT(T_NOUN, ray_char((uint8_t)db[0]));
            else         EMIT(T_NOUN, ray_charv(db, (int64_t)dl));
            free(db);
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
                if (src[p] == ':') {
                    /* FILE symbol `:path (ref/file-text.md, hsym): a leading
                     * ':' pulls ':' and '/' into the name so `:/tmp/a.txt is
                     * ONE symbol.  Constrained to the leading-':' shape on
                     * purpose (plan-review round 1): general handle symbols
                     * (`fifo:x, `host:port) stay two tokens — that spelling
                     * is a hard 'arity error today, so no green row can flip.
                     * '-' is a path/name byte HERE only (`:/tmp/a-b.txt is
                     * ONE symbol; greedy, so `:a-1 too) — a BARE symbol body
                     * stays valid-name chars (alnum/_/.) per basics/syntax.md
                     * (its non-name example `a-b!` needs the `"…" quoted
                     * form), so `a-b / `a-`b keep meaning subtraction.
                     * Spaced `:a - 1 stays subtraction (not glued). */
                    p++;
                    while ((CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) ||
                           src[p] == '.' || src[p] == ':' || src[p] == '/' ||
                           src[p] == '-')
                        p++;
                } else {
                    while ((CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) || src[p] == '.') p++;
                }
                if (count == 0) {
                    first = symlit(src + s, p - s);
                } else if (count == 1) {
                    vec = symvec_add(ray_sym_vec_new(RAY_SYM_W64, 4), first->i64);
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
            EMIT(T_ADVERB, iter_value(base + (two ? 3 : 0)));
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
    /* innermost UNSIGNED lambda's implicit-arg tracker (bit0=x bit1=y bit2=z);
     * NULL outside lambdas and inside signed ones.  The T_LBRACE arm
     * saves/restores it so nested lambdas never leak uses outward. */
    uint8_t *xyz_mask;
    /* >0 while parsing a lambda body — enables `:expr` early-return syntax */
    int lambda_depth;
} Parser;

static Token *cur(Parser *p) { return &p->t.t[p->pos]; }
static int    at(Parser *p, TKind k) { return cur(p)->kind == k; }
static void   adv(Parser *p) { p->pos++; }

static void expect(Parser *p, TKind k, const char *msg) {
    if (at(p, k)) adv(p); else q_die(msg);
}

/* qSQL parse context, threaded (as a value arg, not parser state) through the
 * expression spine.  Q_NONE = ordinary q (all non-query parsing); the others
 * make parse_base_q stop a phrase at its legal clause boundary / separator.
 * Brackets reset to Q_NONE structurally via parse_E — no save/restore stack. */
typedef enum { Q_NONE, Q_SELECT, Q_BY, Q_FROM, Q_WHERE } QCtx;

static int qtok_sym_is(const Token *tk, const char *s);      /* name atom == s */
static int qtok_is_query_verb(const Token *tk);              /* select/exec/update/delete */
static int qtok_is_clause_kw(const Token *tk);               /* by/from/where */
static int qtok_is_join_comma(const Token *tk);              /* dyadic bare `,` only */

static ray_t *parse_E(Parser *p, QCtx ctx);
static P       parse_e(Parser *p, QCtx ctx);
static P       parse_e_from(Parser *p, P t, QCtx ctx);
static P       parse_term(Parser *p, QCtx ctx);
static P       parse_base(Parser *p);
static P       parse_base_q(Parser *p, QCtx ctx);

/* Real-parser qSQL path (parse_query) forward decls — definitions further down. */
static P       parse_query(Parser *p);
static ray_t  *parse_phrase_list(Parser *p, QCtx ctx);
static ray_t  *qsql_normalize_phrases(ray_t *phrase_list, QCtx origin, int verb);
/* qSQL statement verb — selects the per-verb slot shape (definition site has the
 * documentation).  Declared here so parse_query (above the normalize section) can
 * name the codes. */
enum { QSQL_V_SELECT, QSQL_V_EXEC, QSQL_V_UPDATE, QSQL_V_DELETE };

/* Statement sequence: one -> its element; two+ -> (";"; ...).  The head is the
 * CHAR ";" (owner-reported kdb display: `parse "a:1;b:2"` shows `";"`, -10h) —
 * the same char-atom head convention the `:` early-return and `'` signal nodes
 * already use, and the reason `value parse` sees a datum here, not a name.
 * Consumes e. */
static ray_t *seq_of(ray_t *e) {
    int64_t n = ray_len(e);
    if (n == 1) {
        ray_t **slots = (ray_t **)ray_data(e);
        ray_t *only = slots[0];
        if (only) ray_retain(only);
        ray_release(e);
        return only;
    }
    ray_t *semi = ray_char(';');
    ray_t *w = cons_head(semi, e, NULL);
    ray_release(semi);
    ray_release(e);
    return w;
}

/* the char-";" sequence head (see seq_of) */
int q_ast_is_seq_head(const ray_t *h) {
    return h && h->type == -RAY_CHARV && h->u8 == ';';
}

/* q_ast_fill_empty_stmts — see q_parse.h.  DATA-boundary twin of seq_of: the
 * eval path needs the C-NULL empty-statement slots (no-op, no output), but a
 * tree handed out as a VALUE must be kdb-shaped: `parse ";"` is (;;();()). */
void q_ast_fill_empty_stmts(ray_t *ast) {
    if (!ast || ast->type != RAY_LIST || ray_len(ast) < 2) return;
    ray_t **e = (ray_t **)ray_data(ast);
    if (!q_ast_is_seq_head(e[0])) return;
    for (int64_t i = 1; i < ray_len(ast); i++)
        if (!e[i]) e[i] = ray_list_new(1);   /* len-0 general list: () */
}

/* ===== qSQL context predicates (Task 1 scaffolding) =========================
 * Pure token reads used by parse_base_q to decide where an ordinary-q phrase
 * must stop inside a qSQL clause.  All are inert while ctx == Q_NONE. */

/* name atom (unquoted -RAY_SYM) whose interned spelling equals s */
static int qtok_sym_is(const Token *tk, const char *s) {
    return tk->kind == T_NOUN && sym_name_is(tk->k, s);
}

/* the qSQL query verbs routed through the unified real-parser path (parse_query).
 * Task 3: ALL four verbs — parse_query captures the keyword and builds the
 * per-verb 5-list (head `?`/`!`, is_exec/is_delete slot shapes) itself.  Enabling
 * a verb here is what lets a nested query appear as an operand (e.g. `count select
 * a from (select b from t)`): parse_base_q routes the nested query verb into
 * parse_query. */
static int qtok_is_query_verb(const Token *tk) {
    return qtok_sym_is(tk, "select") || qtok_sym_is(tk, "exec") ||
           qtok_sym_is(tk, "update") || qtok_sym_is(tk, "delete");
}

/* the clause keywords that separate qSQL sections */
static int qtok_is_clause_kw(const Token *tk) {
    return qtok_sym_is(tk, "by") || qtok_sym_is(tk, "from") ||
           qtok_sym_is(tk, "where");
}

/* the dyadic bare join `,` verb token (NOT monadic enlist `,:`) — mirrors the
 * comma test in qsql_boundary (a len-1 T_VERB whose spelling is ","). */
static int qtok_is_join_comma(const Token *tk) {
    return tk->kind == T_VERB && sym_name_is(tk->k, ",");
}

/* qSQL interception (piece 3): if the cursor sits on a `select`/`delete`/
 * `update`/`exec` keyword, lower it to kdb's functional parse tree
 * (?;`t;c;b;a) / (!;…) via parse_query and return the OWNED tree; otherwise
 * return NULL with p->pos unchanged so the ordinary parser consumes the tokens.
 * Called both at statement start (parse_e) AND as a primary term (parse_base),
 * so a query can appear as an operand — e.g. `show select from t`, the argument
 * of any prefix fn. */
static ray_t *try_parse_qsql(Parser *p) {
    if (cur(p)->kind != T_NOUN) return NULL;
    /* Task 3: all four query verbs (select/exec/update/delete) commit onto the
     * unified real-parser path — parse_query captures the keyword and emits the
     * per-verb 5-list.  It never soft-fails to NULL (the QCtx-threaded parser
     * handles every ordinary-q phrase), so the token is always consumed. */
    if (qtok_is_query_verb(cur(p)))
        return parse_query(p).v;
    return NULL;
}

/* ===== table literal -> kdb's dict-then-flip parse tree ====================
 * (owner ruling 2026-07-24).  Column-name rule: `c:e` -> c, a bare name-ref
 * keeps its name, anything else derives x (deduped x,x1,… by q_name_dedup —
 * cases.tsv row `([] til 10)`; ChangesIn4.1.md `([0;1;2])`). */

static int sym_is_nameref(ray_t *v) {
    return v && v->type == -RAY_SYM && !(v->attrs & Q_ATTR_QUOTED);
}

/* pairwise duplicate WITHIN one W64 sym-id vector */
static int symvec_ids_dup(ray_t *a) {
    if (!a || a->type != RAY_SYM) return 0;
    int64_t n = ray_len(a);
    const int64_t *s = (const int64_t *)ray_data(a);
    for (int64_t i = 1; i < n; i++)
        for (int64_t j = 0; j < i; j++)
            if (s[i] == s[j]) return 1;
    return 0;
}

/* a select-COLUMN name colliding with a by-GROUP name — the cols/groups CROSS
 * collision, which is what covers both of qsql.md:168's parse-error examples
 * (`select b by b from t`, `select a,a by a from t`).  A collision WITHIN one
 * list is NOT checked here: kdb auto-aliases it, openq rejects it at EVAL
 * (the stricter owner ruling — see q_funsql.c names_collide). */
static int qsql_cross_names_dup(ray_t *A, ray_t *B) {
    ray_t *ka = A && A->type == RAY_DICT ? ray_dict_keys(A) : NULL;
    ray_t *kb = B && B->type == RAY_DICT ? ray_dict_keys(B) : NULL;
    if (!ka || ka->type != RAY_SYM || !kb || kb->type != RAY_SYM) return 0;
    int64_t na = ray_len(ka), nb = ray_len(kb);
    const int64_t *sa = (const int64_t *)ray_data(ka);
    const int64_t *sb = (const int64_t *)ray_data(kb);
    for (int64_t i = 0; i < na; i++)
        for (int64_t j = 0; j < nb; j++)
            if (sa[i] == sb[j]) return 1;
    return 0;
}

/* the registry value for spelling s at valence v (q_embed's policy); q_parse
 * fails fast on a cold registry, so a miss cannot occur here */
static ray_t *table_lit_head(const char *s, q_valence_t v) {
    return q_embed(ray_sym(ray_sym_intern_runtime(s, strlen(s))), v);
}

/* (!;a;b) — consumes a,b */
static ray_t *table_lit_bang(ray_t *a, ray_t *b) {
    ray_t *xs[3] = { table_lit_head("!", Q_DYADIC), a, b };
    return q_list(xs, 3);
}

/* col defs -> the dictionary arm (!;names;(enlist;e…)); consumes defs */
static ray_t *table_lit_dict(ray_t *defs) {
    ray_t *lv = q_registry_list_value();
    if (!lv) q_die("table literal: registry not initialized");
    int64_t n = ray_len(defs);
    ray_t **ds = (ray_t **)ray_data(defs);
    int64_t id_colon = ray_sym_intern_runtime(":", 1);
    int64_t id_x     = ray_sym_intern_runtime("x", 1);
    ray_t *keys = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    ray_t *vals = ray_list_new(n + 1);
    vals = ray_list_append(vals, lv);
    for (int64_t i = 0; i < n; i++) {
        ray_t *d = ds[i], *ex = d;
        int64_t nm = -1;
        if (d && d->type == RAY_LIST && ray_len(d) == 3) {
            ray_t **de = (ray_t **)ray_data(d);
            if (sym_is_nameref(de[0]) && de[0]->i64 == id_colon &&
                sym_is_nameref(de[1])) {
                nm = de[1]->i64;
                ex = de[2];
            }
        } else if (sym_is_nameref(d)) {
            nm = d->i64;
        }
        if (nm < 0)
            nm = q_name_dedup(id_x, (int64_t *)ray_data(keys), i, 0);
        keys = ray_vec_append(keys, &nm);
        if (ex) { vals = ray_list_append(vals, ex); }
        else    { ray_t *nul = q_null(); vals = ray_list_append(vals, nul); ray_release(nul); }
    }
    /* a dup column name in the LITERAL dies at parse like a select's
     * (qsql/select_fixes.qcmd pins `([] a:1 2;a:3 4)` -> 'dup); the same
     * names through raw `!`/`flip` stay legal — construction is permissive */
    if (symvec_ids_dup(keys)) {
        ray_release(keys); ray_release(vals); ray_release(defs);
        q_die_err(QE_DUP);
    }
    ray_release(defs);
    return table_lit_bang(keys, vals);
}

/* col defs -> (+:;(!;…)) — flip of the dictionary arm; consumes defs */
static ray_t *table_lit_flip(ray_t *defs) {
    ray_t *xs[2] = { table_lit_head("+", Q_MONADIC), table_lit_dict(defs) };
    return q_list(xs, 2);
}

static P parse_base(Parser *p) {
    Token *tk = cur(p);
    /* A qSQL template may stand as a primary term (an operand), e.g. the
     * argument of `show`.  Attempt the interception here too; it soft-fails and
     * restores on any non-template so ordinary operands are unaffected. */
    if (tk->kind == T_NOUN) {
        ray_t *q = try_parse_qsql(p);
        if (q) return (P){ R_NOUN, q };
        tk = cur(p);                /* pos unchanged on soft-fail, but re-fetch */
    }
    switch (tk->kind) {
    case T_NOUN: {
        /* implicit-arg inference: a bare 1-char x/y/z name inside the current
         * UNSIGNED lambda body bumps its arity (kdb ranks by highest used) */
        if (p->xyz_mask && tk->len == 1 && tk->k && tk->k->type == -RAY_SYM &&
            !(tk->k->attrs & Q_ATTR_QUOTED)) {
            char c = p->src[tk->start];
            if      (c == 'x') *p->xyz_mask |= 1;
            else if (c == 'y') *p->xyz_mask |= 2;
            else if (c == 'z') *p->xyz_mask |= 4;
        }
        ray_t *v = noun_tree_value(tk->k); tk->k = NULL;
        adv(p);
        return (P){ R_NOUN, v };
    }
    case T_VERB: {
        ray_t *v = tk->k; tk->k = NULL;
        adv(p);
        return (P){ R_VERB, v };
    }
    case T_LBRACK: {
        /* Expression block `[e1;e2;…]` — the SAME `;` statement sequence a
         * lambda body is, so it needs no eval machinery of its own (value =
         * last expression's, generic null when empty; the brackets create no
         * scope: docs-v1/ref/control.md "Expression list", q1.txt:1674).
         * Only reached in expression-HEAD position — a `[` after a term is
         * taken by parse_term's postfix loop, so kdb's argument-list-wins rule
         * survives (`3+[…]` is still +'s arguments, not a block). */
        adv(p);
        ray_t *b = seq_of(parse_E(p, Q_NONE));
        expect(p, T_RBRACK, "expected ']' closing expression block");
        return (P){ R_NOUN, b ? b : RAY_NULL_OBJ };
    }
    case T_LPAREN: {
        adv(p);
        /* Table literal `([k…] c1:e1; …)` — a paren whose first token is `[`.
         * Emits kdb's dict-then-flip parse tree: unkeyed (+:;(!;…)); keyed
         * keytab!valtab; NO value columns (`([a:`A;c:`C])`, xcol's rename
         * map) = the DICTIONARY literal (ref/cols.md; ChangesIn4.1.md). */
        if (at(p, T_LBRACK)) {
            adv(p);                                   /* consume '[' */
            if (at(p, T_RBRACK)) {
                expect(p, T_RBRACK, "expected ']' in table literal");
                ray_t *cols = parse_E(p, Q_NONE);
                expect(p, T_RPAREN, "expected ')'");
                return (P){ R_NOUN, table_lit_flip(cols) };
            }
            ray_t *kcols = parse_E(p, Q_NONE);
            expect(p, T_RBRACK, "expected ']' in keyed table literal");
            /* optional `;` after `]` — `([a:`x`y];b:10 20)` == `([a:`x`y]b:10 20)` */
            if (at(p, T_SEMI)) adv(p);
            if (at(p, T_RPAREN)) {
                expect(p, T_RPAREN, "expected ')'");
                return (P){ R_NOUN, table_lit_dict(kcols) };
            }
            ray_t *vcols = parse_E(p, Q_NONE);
            expect(p, T_RPAREN, "expected ')'");
            return (P){ R_NOUN, table_lit_bang(table_lit_flip(kcols), table_lit_flip(vcols)) };
        }
        ray_t *e = parse_E(p, Q_NONE);
        expect(p, T_RPAREN, "expected ')'");
        /* Inside parens an elided element is a projection HOLE — the literal
         * becomes a projection of the list constructor ("omission of values
         * results in projection", releases/ChangesIn4.1.md; the 8-item cap in
         * basics/application.md is the function-rank cap), so `(1;;3) 7` fills
         * while a list holding an explicit `::` value stays data and indexes. */
        if (ray_len(e) > 1) {
            ray_t **slots = (ray_t **)ray_data(e);
            for (int64_t i = 0; i < ray_len(e); i++) {
                if (!slots[i]) { slots[i] = hole(); continue; }
                /* a LONE glyph verb element is the operator VALUE — its
                 * dyadic row (`(+;7;3)` carries Add; eval (+;7;3) -> 10),
                 * the same bare-verb-as-value convention bracket slots use */
                if (sym_is_glyph(slots[i]))
                    slots[i] = q_embed(slots[i], Q_DYADIC);
            }
        }
        if (ray_len(e) == 1) {
            ray_t **slots = (ray_t **)ray_data(e);
            ray_t *only = slots[0];
            /* A NULL slot is the EMPTY paren `()` — the empty general list
             * (type 0h), NOT `::`.  Drop e and fall through to the
             * list-literal block below (it prepends the list-constructor to a
             * zero-element list, yielding the empty list).  `(1)` is grouping
             * -> the lone element; only `()` reaches here with a NULL slot. */
            if (only) {
                /* A parenthesized lone `(::)` is the generic-null VALUE
                 * itself (kdb null-test idiom `x~(::)`), never the `::`
                 * name-ref whose spelling downstream elision checks
                 * (ql_is_hole) would turn into a projection hole.  Emit the
                 * self-evaluating null singleton; q_fmt prints it `::`. */
                if (sym_name_is(only, "::")) {
                    ray_release(e);
                    return (P){ R_NOUN, RAY_NULL_OBJ };
                }
                ray_retain(only);
                ray_release(e);
                /* a parenthesized lone glyph verb `(+)` is the bare-verb VALUE
                 * (dyadic row); user names keep their name-ref. */
                if (sym_is_glyph(only)) only = q_embed(only, Q_DYADIC);
                return (P){ R_NOUN, only };
            }
            ray_release(e);
            e = ray_list_new(1);   /* 0 elements -> empty list literal below */
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
                ray_t *w = cons_head(lv, e, NULL);
                ray_release(e);
                e = w;
            }
        }
        return (P){ R_NOUN, e };
    }
    case T_LBRACE: {
        /* Lambda literal `{[sig] stmt;...}` -> the RAY_QFN carrier VALUE,
         * built AT PARSE (value-heads-at-parse; kdb parse of a lambda returns
         * the function atom).  src is the VERBATIM `{...}` span (kdb echoes
         * it byte-for-byte); params is a RAY_SYM vector — explicit signature,
         * or x/y/z inferred by highest implicit used (min rank 1), or empty
         * for `{[] ...}`.  No env capture: q lambdas resolve globals at CALL
         * time (the carrier holds only params/body/src). */
        int lb_start = tk->start;
        adv(p);
        ray_t *params = NULL;              /* NULL until signed / inferred */
        if (at(p, T_LBRACK)) {
            adv(p);
            params = ray_sym_vec_new(RAY_SYM_W64, 4);
            if (!at(p, T_RBRACK)) {
                for (;;) {
                    Token *nt = cur(p);
                    if (nt->kind != T_NOUN || !nt->k || nt->k->type != -RAY_SYM ||
                        (nt->k->attrs & Q_ATTR_QUOTED)) {
                        ray_release(params);
                        q_die("expected parameter name in lambda signature");
                    }
                    int64_t id = nt->k->i64;
                    params = ray_vec_append(params, &id);
                    adv(p);
                    if (at(p, T_SEMI)) { adv(p); continue; }
                    break;
                }
            }
            expect(p, T_RBRACK, "expected ']' after lambda signature");
        }
        uint8_t mine = 0;
        uint8_t *saved = p->xyz_mask;
        p->xyz_mask = params ? NULL : &mine;
        p->lambda_depth++;
        ray_t *e = parse_E(p, Q_NONE);
        p->lambda_depth--;
        p->xyz_mask = saved;
        expect(p, T_RBRACE, "expected '}'");
        Token *rb = &p->t.t[p->pos - 1];   /* the consumed '}' */
        ray_t *src = ray_str(p->src + lb_start,
                             (size_t)(rb->start + rb->len - lb_start));
        if (!params) {
            int hi = (mine & 4) ? 3 : (mine & 2) ? 2 : 1;
            params = ray_sym_vec_new(RAY_SYM_W64, 4);
            static const char *xyz[3] = { "x", "y", "z" };
            for (int i = 0; i < hi; i++) {
                int64_t id = ray_sym_intern_runtime(xyz[i], 1);
                params = ray_vec_append(params, &id);
            }
        }
        int64_t bn = ray_len(e);
        ray_t **bs = (ray_t **)ray_data(e);
        /* empty statements (`{2*x;}`) become `::` name-refs: the carrier
         * retains every body expr, so a C NULL here would be fatal */
        for (int64_t i = 0; i < bn; i++)
            if (!bs[i]) bs[i] = q_null();
        ray_t *fn = q_eval_apply_lambda_new(params, bs, bn, src);
        ray_release(src);
        ray_release(params);
        ray_release(e);
        return (P){ R_NOUN, fn };
    }
    case T_ADVERB: {
        /* Compose `'[f;g;…]` — the `'` adverb in BRACKET form composes
         * functions (rightmost consumes the args, each leftward applied
         * monadically).  Only this bracketed form is a primary term; a bare
         * postfix adverb (`f'`) is consumed by parse_term's postfix loop and a
         * leading signal `'expr` by parse_e, so a `'` here NOT followed by `[`
         * is not a term.  Emits (compose-value; f; g; …); parse_term's postfix
         * loop then applies any trailing `[args]`. */
        if (tk->len == 1 && p->src[tk->start] == '\'' &&
            p->t.t[p->pos + 1].kind == T_LBRACK) {
            adv(p);                          /* consume ' */
            adv(p);                          /* consume [ */
            ray_t *args = parse_E(p, Q_NONE);
            expect(p, T_RBRACK, "expected ']' in compose '[…]'");
            ray_t *cv = q_registry_compose_value();
            if (!cv) q_die("compose: registry not initialized");
            /* `'[;]` elides to project the COMPOSE value itself — the same
             * projection hole a bracket call marks, not a `::` value */
            ray_t *w = cons_head(cv, args, hole);
            ray_release(args);
            return (P){ R_NOUN, w };
        }
        /* otherwise a bare iterator in TERM position is its own VALUE — the
         * `\` of `type each(…;\;…)` (basics/datatypes.md) */
        ray_t *iv = tk->k;
        tk->k = NULL;
        adv(p);
        return (P){ R_NOUN, iv };
    }
    default:
        return EMPTY;
    }
}

/* ===== parse_query die-path leak guard ======================================
 * parse_query allocates raw phrase lists (a/b/c) and the from-expression (t)
 * BEFORE it can q_die (empty by/where, missing from, trailing junk, or a nested
 * parse_e failure deeper in the clause).  q_die longjmps to q_parse's handler,
 * skipping parse_query's tail, so those refs would leak under ASan.  Register
 * each live slot ADDRESS on this stack; the q_parse / probe error handlers walk
 * it and release *slot for every in-flight ref.  LIFO: nested parse_phrase_list
 * / parse_query calls push and pop within their own frame, so the stack stays
 * balanced.  On the success path parse_query pops its own registrations (the
 * refs have by then been transferred into the result tree or released). */
typedef struct qsql_pend { ray_t **slot; struct qsql_pend *prev; } qsql_pend_t;
static qsql_pend_t *g_qsql_pend = NULL;

static void qsql_pend_push(ray_t **slot) {
    qsql_pend_t *f = (qsql_pend_t *)malloc(sizeof *f);
    if (!f) q_die("out of memory");
    f->slot = slot; f->prev = g_qsql_pend; g_qsql_pend = f;
}
static void qsql_pend_pop(void) {                 /* success: just unlink */
    qsql_pend_t *f = g_qsql_pend;
    if (!f) return;
    g_qsql_pend = f->prev;
    free(f);
}
static void qsql_pend_unwind(void) {              /* q_die: release every ref */
    while (g_qsql_pend) {
        qsql_pend_t *f = g_qsql_pend;
        if (f->slot && *f->slot) { ray_release(*f->slot); *f->slot = NULL; }
        g_qsql_pend = f->prev;
        free(f);
    }
}

/* parse_query: the UNIFIED qSQL query-tree parser.  Parses
 *     verb [phrases] [by L] from e [where L]
 * and emits the SAME functional 5-list the bespoke clones do — but building the
 * c/b/a slots by normalizing raw parse_e phrase lists (qsql_normalize_phrases)
 * instead of the clone's hand-rolled qsql_expr/qsql_term.  Slot order matches
 * the clones exactly: (head; t; c=where; b=by; a=phrases).
 *
 *   head  `?` (select/exec) | `!` (update/delete)
 *   t     the from-expression (a bare name lowers by-name via ql_qsql's
 *         `t->type == -RAY_SYM` check — same as the clone's qsql_colsym)
 *   c     where: `()` when omitted, else enlist(constraint-list)
 *   b     by:    select/update `0b`, exec `()` when omitted, else the by shape
 *   a     phrases: the per-verb select shape (dict / symvec / value)
 *
 * Unlike the clones this NEVER soft-fails — it commits (the QCtx-threaded real
 * parser handles every ordinary-q phrase), so a form the clone rejected (e.g. an
 * undefined-fn application `select f price`, keyword-infix `where s in `a`b`) now
 * PARSES to a functional tree rather than falling back to the ordinary parser.
 * Task 3: all four verbs (select/exec/update/delete) reach this. */
static P parse_query(Parser *p) {
    ray_t *a = NULL, *b = NULL, *c = NULL, *t = NULL;   /* raw / from-expr refs */
    ray_t *A = NULL, *B = NULL, *C = NULL, *head = NULL, *node = NULL;

    /* Register the in-flight raw slots for the q_die leak guard.  Pushed here
     * (all NULL) so any die between here and the pops frees whatever is live. */
    qsql_pend_push(&a); qsql_pend_push(&b);
    qsql_pend_push(&c); qsql_pend_push(&t);

    /* verb keyword (cursor is on it) */
    Token *vk = cur(p);
    int verb;
    if (vk->len == 6 && memcmp(p->src + vk->start, "select", 6) == 0) verb = QSQL_V_SELECT;
    else if (vk->len == 4 && memcmp(p->src + vk->start, "exec", 4) == 0) verb = QSQL_V_EXEC;
    else if (vk->len == 6 && memcmp(p->src + vk->start, "update", 6) == 0) verb = QSQL_V_UPDATE;
    else verb = QSQL_V_DELETE;                     /* the only remaining verb */
    adv(p);                                        /* consume the verb keyword */

    /* select-phrase list (a): stops at by / from */
    a = parse_phrase_list(p, Q_SELECT);

    /* optional by-phrase list (b): stops at from.  delete has NO By clause
     * (ref/delete.md's template omits it; kdb rejects at parse) */
    if (qtok_sym_is(cur(p), "by")) {
        if (verb == QSQL_V_DELETE) q_die("qsql: by is not a delete clause");
        adv(p);
        b = parse_phrase_list(p, Q_BY);
        if (ray_len(b) == 0) q_die("qsql: empty by phrase");
    }

    /* mandatory from + the from-expression (t) */
    if (!qtok_sym_is(cur(p), "from")) q_die("qsql: expected from");
    adv(p);
    P tp = parse_e(p, Q_FROM);
    if (tp.role == R_NONE) q_die("qsql: expected table after from");
    t = tp.v;

    /* optional where-phrase list (c): runs to the statement end */
    if (qtok_sym_is(cur(p), "where")) {
        adv(p);
        c = parse_phrase_list(p, Q_WHERE);
        if (ray_len(c) == 0) q_die("qsql: empty where phrase");
    }

    /* terminator: only a statement boundary may follow a complete query */
    {
        Token *end = cur(p);
        if (end->kind != T_EOF && end->kind != T_SEMI && end->kind != T_RBRACK &&
            end->kind != T_RPAREN && end->kind != T_RBRACE)
            q_die("qsql: unexpected token after query");
    }

    /* ---- normalize the raw phrase lists into the clone's functional slots ---
     * No q_die past this point, so the raw refs can be released as they are
     * consumed.  A (select-phrase) is verb-shaped by qsql_normalize_phrases
     * (empty select -> `()`, empty delete -> empty symvec, …). */
    A = qsql_normalize_phrases(a, Q_SELECT, verb);
    ray_release(a); a = NULL;

    if (b) { B = qsql_normalize_phrases(b, Q_BY, verb); ray_release(b); b = NULL; }
    else   { B = (verb == QSQL_V_EXEC) ? ray_list_new(0) : ray_bool(0); }  /* no by */

    if (c) { C = qsql_normalize_phrases(c, Q_WHERE, verb); ray_release(c); c = NULL; }
    else   { C = ray_list_new(0); }                /* no where -> () */

    /* qsql.md:168: a cols-vs-groups collision "throws a 'dup names for
     * cols/groups error during parse" — AT PARSE, so a script carrying the
     * bug fails to LOAD and the operator is alerted before the code runs.
     * A within-phrase collision is NOT this law's (kdb auto-aliases it;
     * openq rejects it at eval — the q_funsql.c ruling). */
    if (verb == QSQL_V_SELECT && qsql_cross_names_dup(A, B)) {
        ray_release(A); ray_release(B); ray_release(C);
        q_die_err(QE_DUP);
    }

    /* the in-flight raw slots are now all NULL / consumed — retire the guard. */
    qsql_pend_pop(); qsql_pend_pop(); qsql_pend_pop(); qsql_pend_pop();

    /* the VALUE head (value-heads-at-parse): the same immutable registry cell
     * the functional spelling carries — never a bare sym name-ref */
    head = q_embed((verb == QSQL_V_SELECT || verb == QSQL_V_EXEC) ? q_verb('?')
                                                                  : q_verb('!'),
                   Q_DYADIC);
    node = ray_list_new(5);
    node = ray_list_append(node, head); ray_release(head);
    node = ray_list_append(node, t);    ray_release(t);
    node = ray_list_append(node, C);    ray_release(C);
    node = ray_list_append(node, B);    ray_release(B);
    node = ray_list_append(node, A);    ray_release(A);
    return (P){ R_NOUN, node };
}

/* Query-aware wrapper around the UNCHANGED parse_base.  Returns EMPTY (without
 * consuming) at a clause boundary / separator so the enclosing parse_e halts.
 * With ctx == Q_NONE it is behaviourally identical to parse_base. */
static P parse_base_q(Parser *p, QCtx ctx) {
    Token *tk = cur(p);
    if (qtok_is_query_verb(tk)) return parse_query(p);   /* qSQL verb -> unified path */
    if (ctx != Q_NONE && qtok_is_clause_kw(tk)) {
        int is_by = qtok_sym_is(tk, "by"), is_from = qtok_sym_is(tk, "from"),
            is_where = qtok_sym_is(tk, "where");
        switch (ctx) {
        /* Before `from`, `where` cannot be the CLAUSE: ref/select.md's syntax
         * puts it last (`… from texp [where pw]`), so it is the monadic VERB
         * and `seq where cond` is the juxtaposition `seq[where cond]`. */
        case Q_SELECT: if (is_by || is_from) return EMPTY; if (is_where) break;
                       q_die("qsql: unexpected keyword after select (expected by/from)");
        case Q_BY:     if (is_from) return EMPTY; if (is_by || is_where) break;
                       q_die("qsql: unexpected keyword in by phrase (expected from)");
        case Q_FROM:   if (is_where) return EMPTY; if (is_from) break;
                       q_die("qsql: unexpected keyword after from (expected where)");
        case Q_WHERE:  if (is_where) break;
                       q_die("qsql: unexpected keyword in where phrase");
        default: break;
        }
    }
    if (ctx != Q_NONE && ctx != Q_FROM && qtok_is_join_comma(tk)) return EMPTY;
    return parse_base(p);
}

static P parse_term(Parser *p, QCtx ctx) {
    P t = parse_base_q(p, ctx);
    if (t.role == R_NONE) return t;

    for (;;) {
        Token *tk = cur(p);
        if (tk->kind == T_LBRACK) {
            adv(p);
            ray_t *e = parse_E(p, Q_NONE);
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
                if (es[i]) {
                    /* a LONE glyph verb filling a slot is the operator VALUE
                     * — its dyadic row (`@[x;i;*;y]` passes Multiply), the
                     * bare-verb-as-value convention parens use; `:` stays a
                     * name-ref (q_embed's colon guard) */
                    ray_t *slot = es[i];
                    if (slot->type == -RAY_SYM && !(slot->attrs & Q_ATTR_QUOTED) &&
                        sym_is_glyph(slot)) {
                        ray_retain(slot);
                        slot = q_embed(slot, Q_DYADIC);
                        w = ray_list_append(w, slot);
                        ray_release(slot);
                    } else {
                        w = ray_list_append(w, slot);
                    }
                }
                /* an elided bracket slot is a projection hole (Q_ATTR_HOLE),
                 * distinct from an explicit `::` value in the same position */
                else       { ray_t *nul = hole(); w = ray_list_append(w, nul); ray_release(nul); }
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

static P parse_e(Parser *p, QCtx ctx) {
    /* Lambda-body early return `:expr` (basics/function-notation.md): a bare
     * `:` at expression START inside a lambda body.  Infix assignment never
     * reaches here with a leading `:` (its lhs noun is consumed first), and
     * `::` is len 2.  Emits (":";expr) — a char-atom head, NOT a verb or
     * `.q` entry: kdb display shows lambdas verbatim, so no parse row pins it. */
    if (p->lambda_depth > 0) {
        Token *rt = cur(p);
        /* a LONE `:` (next token closes the expression) is the assign-verb
         * OPERAND (`{@[x;1;:;"Z"]}`, ref/amend.md), not an early return */
        TKind nk = p->t.t[p->pos + 1].kind;
        if (rt->kind == T_VERB && rt->len == 1 && p->src[rt->start] == ':' &&
            nk != T_SEMI && nk != T_RBRACK && nk != T_RPAREN && nk != T_RBRACE) {
            adv(p);
            P e = parse_e(p, ctx);
            ray_t *rhs = (e.role != R_NONE && e.v) ? e.v : q_null();
            ray_t *xs[2] = { ray_char(':'), rhs };
            return (P){ R_NOUN, q_list(xs, 2) };
        }
    }
    /* Signal `'expr` (ref/signal.md): a bare `'` adverb at expression start
     * that is NOT the compose form `'[f;g]`.  Emits ("'";expr) — a char-atom
     * head distinct from the each-adverb value (f' -> (';`f)); Signal "is
     * part of q syntax ... not an operator", so never a registry row.  With
     * nothing to signal (`(';/;\)`) the `'` is the ITERATOR VALUE instead. */
    {
        Token *st = cur(p);
        TKind nk = st->kind == T_EOF ? T_EOF : p->t.t[p->pos + 1].kind;
        if (st->kind == T_ADVERB && st->len == 1 && p->src[st->start] == '\'' &&
            nk != T_LBRACK && nk != T_SEMI && nk != T_RPAREN &&
            nk != T_RBRACK && nk != T_RBRACE && nk != T_EOF) {
            adv(p);
            P e = parse_e(p, ctx);
            ray_t *rhs = (e.role != R_NONE && e.v) ? e.v : q_null();
            ray_t *xs[2] = { ray_char('\''), rhs };
            return (P){ R_NOUN, q_list(xs, 2) };
        }
    }
    /* qSQL interception (piece 3): a `select …` statement lowers to kdb's
     * functional parse tree (?;`t;c;b;a).  On any unsupported form parse_qsql
     * soft-fails (restores p->pos, leaves tokens intact) and we fall through to
     * the ordinary parser — so previously-parseable selects never regress. */
    {
        ray_t *q = try_parse_qsql(p);
        if (q) return (P){ R_NOUN, q };
    }
    /* A colon-unary k glyph GLUED to an operand (`*:1 2 3`, `+:5`) is k, NOT q
     * — q spells the monadic with its keyword (first, flip, count, …).  Owner
     * ruling 2026-07-30: reject it at PARSE, so no tree ever exists and the
     * direct and parse-then-eval paths cannot disagree.  Narrow by design: the
     * isolated value (`+:`), bracket apply (`*:[x]`) and the parenthesised
     * value (`(*:) x`) all reach here with a non-operand next token or a
     * non-verb head, and are untouched.  `::`, `0:` and the two-glyph
     * comparisons (`<=`) are excluded by the glyph+colon shape test. */
    {
        Token *ht = cur(p);
        TKind nk = ht->kind == T_EOF ? T_EOF : p->t.t[p->pos + 1].kind;
        if (ht->kind == T_VERB && ht->len == 2 && p->src[ht->start] != ':' &&
            p->src[ht->start + 1] == ':' &&
            strchr(VERB_CHARS, p->src[ht->start]) != NULL &&
            (nk == T_NOUN || nk == T_LPAREN || nk == T_LBRACE))
            q_die("k-unary glyph applied to a glued operand is not q");
    }
    P t = parse_term(p, ctx);
    if (t.role == R_NONE) return EMPTY;
    return parse_e_from(p, t, ctx);
}

static P parse_e_from(Parser *p, P t, QCtx ctx) {
    Token *ut = cur(p);
    P u = parse_term(p, ctx);

    if (u.role == R_NONE) return t;

    /* SPACED `x ::` is APPLICATION — `::` a noun operand, the generic-null
     * VALUE (owner ruling 2026-07-23: `(::)~value ::` is 1b); only GLUED
     * `x::…` keeps global-assign.  Demote the verb to a noun and fall into
     * the ordinary juxtaposition build below. */
    if (t.role == R_NOUN && u.role == R_VERB && u.v &&
        u.v->type == -RAY_SYM && ut->kind == T_VERB && ut->len == 2 &&
        p->src[ut->start] == ':' && p->src[ut->start + 1] == ':' &&
        ut->start > 0 && (p->src[ut->start - 1] == ' ' ||
                          p->src[ut->start - 1] == '\t')) {
        ray_release(u.v);
        u.v = RAY_NULL_OBJ;
        u.role = R_NOUN;
    }

    if (t.role == R_NOUN && u.role == R_VERB) {
        P e = parse_e(p, ctx);
        /* postfix form (`1+`, `-15!`): the missing rhs is a projection HOLE,
         * the same Q_ATTR_HOLE marker bracket elisions carry — an explicit
         * `::` operand stays plain and evaluates to the generic-null VALUE */
        ray_t *rhs = e.v ? e.v : hole();
        u.v = q_embed(u.v, Q_DYADIC);          /* infix head: the dyadic row */
        ray_t *xs[3] = { u.v, t.v, rhs };
        return (P){ R_NOUN, q_list(xs, 3) };
    }

    P e = parse_e_from(p, u, ctx);
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
    if (e.role == R_VERB && sym_is_glyph(e.v)) e.v = q_embed(e.v, Q_DYADIC);
    /* JUXTAPOSED paren-glyph on a DATA operand takes its unary meaning —
     * `(,)2` is `,2`, `(-)5` is `-5` (owner ruling 2026-07-23); bracket-apply
     * `+[10]` stays a projection (assign/apply-forms contract) and a fn-value
     * operand keeps the dyad ((+)(*) pins (+;*) — parse/cases.tsv). */
    if (t.role == R_NOUN && t.v && t.v->type == RAY_BINARY &&
        e.role == R_NOUN && e.v &&
        !(e.v->type == RAY_UNARY || e.v->type == RAY_BINARY ||
          e.v->type == RAY_VARY)) {
        const q_op_t *drow = q_registry_row_of(t.v, Q_DYADIC);
        if (drow && drow->name) {
            ray_t *mv = q_registry_lookup_name(drow->name, strlen(drow->name),
                                               Q_MONADIC);
            if (mv && (mv->type == RAY_UNARY || mv->type == RAY_BINARY ||
                       mv->type == RAY_VARY)) {
                ray_retain(mv);
                ray_release(t.v);
                t.v = mv;
            }
        }
    }
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

/* A callable VALUE usable as a select/exec/update phrase head: a builtin verb
 * (RAY_UNARY/BINARY/VARY), a bare RAY_LAMBDA, or a RAY_QFN carrier (a named
 * q `{…}` binds in the env as one) — so `select f price` embeds the `f`
 * value rather than demoting it to a column symbol. */
static inline int qsql_is_fn_value(const ray_t *v) {
    if (!v) return 0;
    if (v->type >= RAY_LAMBDA && v->type <= RAY_VARY) return 1;
    if (q_eval_apply_carrier_kind(v)) return 1;
    return 0;
}

/* Rightmost column symbol in an expression — the default output-column name
 * (`sum a` -> `a`, `a` -> `a`).  Returns a fresh owned `sym, or NULL. */
static ray_t *qsql_derive_alias(ray_t *expr) {
    if (!expr) return NULL;
    if (expr->type == -RAY_SYM && (expr->attrs & Q_ATTR_QUOTED)) {
        /* a dotted reference keys its column by the LAST segment: `10 xbar
         * time.minute` -> `minute` (ref/xbar.md:33, kb/programming-idioms.md:265) */
        int64_t id = expr->i64;
        ray_t *s = ray_sym_str(id);
        if (s) {
            const char *p = ray_str_ptr(s);
            size_t n = ray_str_len(s), cut = 0;
            for (size_t i = 0; i < n; i++)
                if (p[i] == '.') cut = i + 1;
            if (cut > 0 && cut < n)
                id = ray_sym_intern_runtime(p + cut, (int64_t)(n - cut));
            ray_release(s);
        }
        return qsql_colsym(id);
    }
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

/* Build a q name!expr dict from a column list (consumes the alias/val refs). */
static ray_t *qsql_build_dict(ray_t **aliases, ray_t **vals, int n) {
    ray_t *keys = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    for (int i = 0; i < n; i++) keys = symvec_add(keys, aliases[i]->i64);
    int all_sym = 1;
    for (int i = 0; i < n; i++)
        if (!(vals[i] && vals[i]->type == -RAY_SYM && (vals[i]->attrs & Q_ATTR_QUOTED)))
            { all_sym = 0; break; }
    ray_t *valv;
    if (all_sym) {
        valv = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
        for (int i = 0; i < n; i++) valv = symvec_add(valv, vals[i]->i64);
    } else {
        valv = ray_list_new(n > 0 ? n : 1);
        for (int i = 0; i < n; i++) valv = ray_list_append(valv, vals[i]);
    }
    for (int i = 0; i < n; i++) { ray_release(aliases[i]); ray_release(vals[i]); }
    return ray_dict_new(keys, valv);   /* consumes keys, valv */
}

/* Build the exec By-phrase VALUE from a parsed by-column list.  kdb encodes an
 * exec By as a bare symbol (single group column) or symbol vector (multiple)
 * when the columns are unnamed bare column references — routed to the grouped-
 * exec branch of q_funsql_select_impl (currently deferred).  A named/computed
 * By (`by k:expr`) degrades to a name!expr DICT, the same shape Select uses, so
 * ql_qsql_exec skips it and it lowers via the Select path (keyed-table result).
 * Consumes the bk/bv refs.  Returns an OWNED value. */
static ray_t *qsql_exec_by(ray_t **bk, ray_t **bv, const int *bnamed, int nb) {
    int all_bare = 1;
    for (int i = 0; i < nb; i++)
        if (bnamed[i] || !(bv[i] && bv[i]->type == -RAY_SYM &&
                           (bv[i]->attrs & Q_ATTR_QUOTED))) { all_bare = 0; break; }
    if (all_bare) {
        ray_t *b;
        if (nb == 1) {
            /* the tree QUOTES the group-by symbol constant (parsetrees.md):
             * one eval of `,`n` yields the functional b-value `n */
            b = symvec_add(ray_sym_vec_new(RAY_SYM_W64, 1), bv[0]->i64);
        } else {
            /* a by-symbol VECTOR constant is enlisted like every symvec in a
             * tree (parsetrees.md): ,`a`b evals once to the functional b */
            b = ray_sym_vec_new(RAY_SYM_W64, nb);
            for (int i = 0; i < nb; i++) b = symvec_add(b, bv[i]->i64);
            b = qsql_enlist(b);
        }
        for (int i = 0; i < nb; i++) { ray_release(bk[i]); ray_release(bv[i]); }
        return b;
    }
    return qsql_build_dict(bk, bv, nb);              /* consumes bk/bv */
}

/* ===== qSQL phrase normalization (parser-unification piece) ==================
 * The migration routes qSQL clause phrases through the REAL expression parser
 * (parse_e with a QCtx) instead of the bespoke qsql_expr/qsql_term clones.  The
 * real parser emits RAW phrase trees — a RAY_LIST of `(:;name;expr)` alias
 * nodes, bare name-refs and constraint exprs — NOT the functional slot shapes
 * the query engine + lowering consume.  qsql_normalize_phrases converts a raw
 * phrase list into EXACTLY the slot the clones produce, so `parse` display,
 * display and the downstream consumers are all unchanged.
 *
 * The one representational nuance: the real parser embeds a fn-VALUE at every
 * infix GLYPH head (`>` in `price>10`), whereas the clone left that head a bare
 * glyph name-ref sym.  Both render identically through q_registry provenance
 * (q_fmt) and resolve identically in the engine (funsql_is_fn accepts the
 * value; the lowering walker embeds the bare glyph to the same value), so the
 * pass PRESERVES an applied fn-value head as-is.  Conversely a RESERVED verb
 * APPLIED to an operand (`sum i`) is a function call whose head must be
 * embedded as its registry value (else it would print `` `sum `` and the
 * engine would mistake it for a column); a name-ref STANDING ALONE is a column
 * symbol.  This is the crux we re-express here over the raw phrase trees. */

/* qSQL statement verb codes (QSQL_V_SELECT/EXEC/UPDATE/DELETE) are declared up
 * near the parse_query forward decls; they select the per-verb slot shape. */

static ray_t *qsql_convert_expr(ray_t *x);

/* An APPLIED phrase head: a RESERVED q verb embeds as its registry MONADIC
 * value (kdb shows `.q` definitions in full — funsql.md:730), including the
 * env-bound alias spellings whose value IS a registry object (`not` ~
 * q_builtins' `~:` share).  A USER name stays a name-ref: its meaning follows
 * qsql.md:121-127's eval-time order (column, then enclosing local, then
 * global — funsql.md:706 keeps variables as symbols in the tree), which a
 * parse-time env snapshot cannot honour.  A head that is already a fn value
 * (an embedded infix glyph) is PRESERVED; a nested-list head is converted
 * recursively.  Returns an OWNED value. */
static ray_t *qsql_convert_head(ray_t *h) {
    if (h && h->type == RAY_LIST) return qsql_convert_expr(h);
    if (!h || h->type != -RAY_SYM ||
        (h->attrs & (Q_ATTR_QUOTED | Q_ATTR_HOLE))) {
        if (h) ray_retain(h);
        return h;                                  /* fn value / literal: as-is */
    }
    ray_t *ev = NULL;
    ray_t *s = ray_sym_str(h->i64);
    if (s) { ev = q_registry_lookup_name(ray_str_ptr(s), ray_str_len(s), Q_MONADIC);
             ray_release(s); }
    if (!ev) {
        ray_t *b = q_env_get(h->i64);              /* borrowed */
        if (b && q_registry_provenance(b, NULL)) ev = b;
    }
    if (qsql_is_fn_value(ev)) { ray_retain(ev); return ev; }
    ray_retain(h);
    return h;                                       /* not a verb: leave name-ref */
}

/* Rewrite ONE raw phrase expr into the clone's leaf representation:
 *   - bare name-ref standing alone -> column symbol (a reserved registry verb
 *     standing alone embeds as its value, matching qsql_term);
 *   - `sym scalar literal -> the enlisted 1-element sym VECTOR `,`sym`;
 *   - a paren `(e1;e2;…)` list -> kdb's `(enlist;e1;…)` (monadic enlist head);
 *   - an application `(head; args…)` -> converted head + column-ised args.
 * Returns an OWNED tree; does NOT consume x. */
static ray_t *qsql_convert_expr(ray_t *x) {
    if (!x) return q_null();
    if (x->type == -RAY_SYM) {
        /* `sym constants arrive pre-wrapped (,`sym) from noun_tree_value and
         * pass through the tail below; a sym ATOM here is always a name-ref.
         * EXCEPT an elision hole, whose meaning rides Q_ATTR_HOLE, not its
         * spelling: column-ising it turns `(+[;10]) c` into an application. */
        if (x->attrs & Q_ATTR_HOLE) { ray_retain(x); return x; }
        ray_t *ev = NULL;                          /* standalone name-ref */
        ray_t *s = ray_sym_str(x->i64);
        if (s) { ev = q_registry_lookup_name(ray_str_ptr(s), ray_str_len(s),
                                             Q_MONADIC); ray_release(s); }
        if (qsql_is_fn_value(ev)) { ray_retain(ev); return ev; }
        return qsql_colsym(x->i64);                /* else column symbol */
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t **e = (ray_t **)ray_data(x);
        /* An ENLISTED symvec constant (the parser's ,`a`b wrap) passes through
         * whole: kdb keeps it enlisted (funsql.md:69 `c1 in `b`c` =
         * (in;`c1;enlist[`b`c])) and one phrase eval yields the symvec. */
        if (n == 1 && e[0] && e[0]->type == RAY_SYM) {
            ray_retain(x);
            return x;
        }
        ray_t *node = ray_list_new(n > 0 ? n : 1);
        if (n >= 1 && e[0] == q_registry_list_value()) {
            /* paren literal -> (enlist; e1; …): the monadic enlist VALUE head so
             * the exec fn-head branch enlists the per-column results. */
            ray_t *enl = q_registry_lookup_name(",", 1, Q_MONADIC);
            node = ray_list_append(node, enl ? enl : e[0]);  /* append retains */
            for (int64_t i = 1; i < n; i++) {
                ray_t *c = qsql_convert_expr(e[i]);
                node = ray_list_append(node, c); ray_release(c);
            }
            return node;
        }
        if (n >= 1) {                              /* application */
            ray_t *h = qsql_convert_head(e[0]);
            node = ray_list_append(node, h); if (h) ray_release(h);
            for (int64_t i = 1; i < n; i++) {
                ray_t *c = qsql_convert_expr(e[i]);
                node = ray_list_append(node, c); ray_release(c);
            }
        }
        return node;
    }
    ray_retain(x);        /* number / vector literal (incl. the ,`x atom wrap) */
    return x;
}

/* Detect a `(:;name;val)` alias node (`name:expr` in a select/exec phrase) from
 * the real parser; on success returns the borrowed name-ref and value. */
static int qsql_phrase_alias(ray_t *x, ray_t **name, ray_t **val) {
    if (!x || x->type != RAY_LIST || ray_len(x) != 3) return 0;
    ray_t **e = (ray_t **)ray_data(x);
    if (!sym_name_is(e[0], ":")) return 0;
    ray_t *t = e[1];
    if (!t || t->type != -RAY_SYM || (t->attrs & Q_ATTR_QUOTED)) return 0;
    *name = e[1];
    *val  = e[2];
    return 1;
}

/* Fold a phrase list into a q name!expr DICT (select/update `a`, by-key `b`):
 * an alias phrase keys on its written name; a bare phrase derives its output
 * name via qsql_derive_alias, exactly as qsql_colspec does over the clone. */
static ray_t *qsql_norm_dict(ray_t *phrases) {
    int64_t n = ray_len(phrases);
    ray_t **ph = (ray_t **)ray_data(phrases);
    ray_t *aliases[QSQL_MAXCOLS], *vals[QSQL_MAXCOLS];
    int na = 0;
    for (int64_t i = 0; i < n && na < QSQL_MAXCOLS; i++) {
        ray_t *name = NULL, *val = NULL;
        if (qsql_phrase_alias(ph[i], &name, &val)) {
            aliases[na] = qsql_colsym(name->i64);
            vals[na]    = qsql_convert_expr(val);
        } else {
            ray_t *v  = qsql_convert_expr(ph[i]);
            ray_t *al = qsql_derive_alias(v);
            if (!al) { ray_release(v); continue; }  /* unnameable (clone soft-fails) */
            /* kdb: a bare `select i` (the VIRTUAL row-index column) outputs
             * under the name `x`, not `i` (qsql.md — i is not a real column,
             * so the default rightmost-name alias does not apply). */
            if (v && v->type == -RAY_SYM && al->type == -RAY_SYM) {
                ray_t *als = ray_sym_str(al->i64);
                if (als && ray_str_len(als) == 1 && ray_str_ptr(als)[0] == 'i') {
                    ray_release(al);
                    al = qsql_colsym(ray_sym_intern_runtime("x", 1));
                }
            }
            aliases[na] = al;
            vals[na]    = v;
        }
        na++;
    }
    return qsql_build_dict(aliases, vals, na);       /* consumes aliases/vals */
}

/* exec select-phrase `a`: omitted -> `()`; a single unnamed column -> the
 * QUOTED bare value (parsetrees.md: constants are enlisted, so one eval of
 * the slot yields the functional a-value — a col sym as `,`c1`, a computed
 * expr as its enlisted tree); named / multiple -> a name!expr dict. */
static ray_t *qsql_norm_exec_a(ray_t *phrases) {
    int64_t n = ray_len(phrases);
    ray_t **ph = (ray_t **)ray_data(phrases);
    if (n == 0) return ray_list_new(0);
    ray_t *name = NULL, *val = NULL;
    if (n == 1 && !qsql_phrase_alias(ph[0], &name, &val)) {
        ray_t *v = qsql_convert_expr(ph[0]);
        if (!v || RAY_IS_ERR(v)) return v;
        if (v->type == -RAY_SYM) {
            ray_t *sv = symvec_add(ray_sym_vec_new(RAY_SYM_W64, 1), v->i64);
            ray_release(v);
            return sv;
        }
        return qsql_enlist(v);
    }
    return qsql_norm_dict(phrases);                  /* named / multiple -> dict */
}

/* by-phrase `b`: select/update use the name!expr dict; exec uses the bare/vector
 * By-symbol (or a name!expr dict for a computed By) via qsql_exec_by. */
static ray_t *qsql_norm_by(ray_t *phrases, int verb) {
    if (verb != QSQL_V_EXEC) return qsql_norm_dict(phrases);
    int64_t n = ray_len(phrases);
    ray_t **ph = (ray_t **)ray_data(phrases);
    /* `by 0b` is already a functional b-value: pass it through so exec takes the
     * select (table) shape — ref/exec.md:96 */
    if (n == 1 && ph[0] && ph[0]->type == -RAY_BOOL) {
        ray_retain(ph[0]);
        return ph[0];
    }
    ray_t *bk[QSQL_MAXCOLS], *bv[QSQL_MAXCOLS]; int bnamed[QSQL_MAXCOLS];
    int nb = 0;
    for (int64_t i = 0; i < n && nb < QSQL_MAXCOLS; i++) {
        ray_t *name = NULL, *val = NULL;
        if (qsql_phrase_alias(ph[i], &name, &val)) {
            bk[nb] = qsql_colsym(name->i64);
            bv[nb] = qsql_convert_expr(val);
            bnamed[nb] = 1;
        } else {
            ray_t *v  = qsql_convert_expr(ph[i]);
            ray_t *al = qsql_derive_alias(v);
            if (!al) { ray_release(v); continue; }
            bk[nb] = al; bv[nb] = v; bnamed[nb] = 0;
        }
        nb++;
    }
    return qsql_exec_by(bk, bv, bnamed, nb);         /* consumes bk/bv */
}

/* where-phrase `c`: enlist the already-parsed constraint exprs (each converted),
 * matching qsql_where's enlist(constraint-list). */
static ray_t *qsql_norm_where(ray_t *phrases) {
    int64_t n = ray_len(phrases);
    ray_t **ph = (ray_t **)ray_data(phrases);
    ray_t *clist = ray_list_new(n > 0 ? n : 1);
    for (int64_t i = 0; i < n; i++) {
        ray_t *c = qsql_convert_expr(ph[i]);
        clist = ray_list_append(clist, c); ray_release(c);
    }
    return qsql_enlist(clist);                        /* enlist(list); consumes clist */
}

/* delete column-list `a`: the col names as a symbol VECTOR, ENLISTED like every
 * symvec constant in a tree (parsetrees.md "lists of symbols are enlisted" —
 * `,,`b`), so one eval of the slot yields the functional symvec.  A bare 1-elem
 * symvec would instead eval to the ATOM (parsetrees.md:26 eval enlist`x -> `x). */
static ray_t *qsql_norm_delete_a(ray_t *phrases) {
    int64_t n = ray_len(phrases);
    ray_t **ph = (ray_t **)ray_data(phrases);
    ray_t *a = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    for (int64_t i = 0; i < n; i++) {
        ray_t *x = ph[i];
        if (!x || x->type != -RAY_SYM) continue;     /* non-name (clone soft-fails) */
        a = symvec_add(a, x->i64);
    }
    return qsql_enlist(a);
}

/* Normalize a raw phrase list into the functional slot the clones emit.
 *   origin  which clause the phrases came from (Q_SELECT main phrase / Q_BY /
 *           Q_WHERE) — picks the sub-shaper;
 *   verb    the statement (QSQL_V_*) — picks the per-verb `a`/`b` shape.
 * Returns an OWNED slot; does NOT consume phrase_list. */
static ray_t *qsql_normalize_phrases(ray_t *phrase_list, QCtx origin, int verb) {
    if (origin == Q_WHERE) return qsql_norm_where(phrase_list);
    if (origin == Q_BY)    return qsql_norm_by(phrase_list, verb);
    switch (verb) {                                  /* origin == Q_SELECT main phrase */
    case QSQL_V_EXEC:   return qsql_norm_exec_a(phrase_list);
    case QSQL_V_DELETE: return qsql_norm_delete_a(phrase_list);
    default:                                         /* SELECT / UPDATE */
        if (ray_len(phrase_list) == 0) return ray_list_new(0);   /* select () */
        return qsql_norm_dict(phrase_list);
    }
}

/* Parse one comma-separated qSQL clause into a RAY_LIST of raw phrase trees via
 * the real expression parser, halting at the clause boundary (parse_base_q
 * returns EMPTY at a section keyword / the join comma).  An elided phrase (a
 * doubled comma) becomes the generic null `::`, as qsql_where's elided path
 * uses.  Refcount: ray_list_append RETAINS, so the local ref is released in
 * both branches.  Returns an OWNED list ( `()` when the clause is empty). */
static ray_t *parse_phrase_list(Parser *p, QCtx ctx) {
    ray_t *lst = ray_list_new(0);
    /* A parse_e under a non-Q_NONE ctx can q_die (a misplaced clause keyword in
     * parse_base_q); register the partial list so the longjmp handler frees it. */
    qsql_pend_push(&lst);
    P first = parse_e(p, ctx);
    if (first.role != R_NONE) {
        lst = ray_list_append(lst, first.v); ray_release(first.v);
        while (qtok_is_join_comma(cur(p))) {
            adv(p);
            P f = parse_e(p, ctx);
            ray_t *v = (f.role == R_NONE) ? q_null() : f.v;
            lst = ray_list_append(lst, v);
            ray_release(v);
        }
    }
    qsql_pend_pop();                                 /* success: unlink, keep lst */
    return lst;
}

/* ---- test hook (oracle unit test) ------------------------------------------
 * Scan `src` as a bare clause phrase list in `ctx`, then normalize it to the
 * `verb` slot shape.  Lets test/q_qsql_normalize.c drive the NEW parse_e path
 * over phrase substrings and compare the slot to the CLONE's output.  Not on any
 * runtime path — the migration wires parse_query, not this.  Returns OWNED. */
ray_t *q_qsql_normalize_probe(const char *src, int ctx, int verb) {
    if (!q_registry_ready())
        return q_err(QE_INIT);
    init_class();
    g_toks.t = NULL; g_toks.n = 0;
    if (setjmp(q_err_jmp)) {
        qsql_pend_unwind();
        free_tokens(g_toks);
        g_toks.t = NULL; g_toks.n = 0;
        q_err_e cls = g_die_class; g_die_class = QE_PARSE;
        return q_err(cls);
    }
    Tokens ts = scan(src);
    Parser p = { .src = src, .t = ts, .pos = 0, .xyz_mask = NULL, .lambda_depth = 0 };
    ray_t *phrases = parse_phrase_list(&p, (QCtx)ctx);
    ray_t *slot = qsql_normalize_phrases(phrases, (QCtx)ctx, verb);
    ray_release(phrases);
    free_tokens(ts);
    g_toks.t = NULL; g_toks.n = 0;
    return slot;
}

static ray_t *parse_E(Parser *p, QCtx ctx) {
    ray_t *buf[MAX_VEC]; int n = 0;
    buf[n++] = parse_e(p, Q_NONE).v;
    while (at(p, T_SEMI)) {
        if (n >= MAX_VEC) q_die("too many ';'-separated expressions");
        adv(p);
        buf[n++] = parse_e(p, Q_NONE).v;
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
        return q_err(QE_INIT);
    /* System-command line: a statement starting with '\' (kdb's column-0
     * convention).  `\t`/`\ts expr` time the expression via the base `timeit`
     * special form (kdb returns ms; timing rows are never byte-pinned).  Every
     * other `\X ...` (namespace/precision/console/dir — session state we do not
     * model) is accepted as a SILENT no-op so the line parses and runs rather
     * than raising 'parse.  `\` / `\\` alone are also no-ops here (the REPL
     * intercepts `\\` for exit before ever calling q_parse). */
    {
        const char* s = src;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\\') {
            const char* c = s + 1;
            while ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')) c++;
            size_t clen = (size_t)(c - (s + 1));
            const char* rest = c;
            while (*rest == ' ' || *rest == '\t') rest++;
            int is_t  = (clen == 1 && s[1] == 't');
            int is_ts = (clen == 2 && s[1] == 't' && s[2] == 's');
            if ((is_t || is_ts) && *rest) {
                size_t rl = strlen(rest);
                char* buf = (char*)malloc(rl + 8);
                if (!buf) return q_err(QE_WSFULL);
                memcpy(buf, "timeit ", 7);
                memcpy(buf + 7, rest, rl + 1);
                ray_t* prog = q_parse(buf);      /* buf starts "timeit ": no recursion */
                free(buf);
                return prog;
            }
            return RAY_NULL_OBJ;                  /* no-op: parses + runs silently */
        }
    }
    init_class();
    g_toks.t = NULL;
    g_toks.n = 0;
    if (setjmp(q_err_jmp)) {
        /* q_die() longjmped here; free whatever the scanner had emitted, plus
         * any in-flight qSQL from-expression token snapshots and any raw
         * parse_query phrase/from-expr refs registered on the pending guard. */
        qsql_pend_unwind();
        free_tokens(g_toks);
        g_toks.t = NULL;
        g_toks.n = 0;
        q_err_e cls = g_die_class; g_die_class = QE_PARSE;
        return q_err(cls);
    }

    Tokens ts = scan(src);
    Parser p = { .src = src, .t = ts, .pos = 0 };
    ray_t *e = parse_E(&p, Q_NONE);
    if (!at(&p, T_EOF)) {
        ray_release(e);
        free_tokens(ts);
        g_toks.t = NULL; g_toks.n = 0;
        return q_err(QE_PARSE);
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

/* q_parse_is_assign — see q_parse.h.  Head is the name-ref `:`/`::` (or the
 * modified-assign `<op>:`) with a name/indexed-name target; a `;` statement
 * sequence (char head) asks its last statement. */
int q_parse_is_assign(const ray_t *cast) {
    ray_t *ast = (ray_t *)cast;   /* read-only walk; ray_data lacks a const view */
    if (!ast || ast->type != RAY_LIST || ray_len(ast) < 1) return 0;
    ray_t **e = (ray_t **)ray_data(ast);
    ray_t *h = e[0];
    if (q_ast_is_seq_head(h)) {
        int64_t n = ray_len(ast);
        return n >= 2 ? q_parse_is_assign(e[n - 1]) : 0;
    }
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return 0;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return 0;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int is_colon = (l == 1 && nm[0] == ':') ||
                   (l == 2 && nm[0] == ':' && nm[1] == ':');
    int is_modasg = (l >= 2 && nm[l - 1] == ':' && nm[0] != ':');
    ray_release(s);
    if (!(is_colon || is_modasg) || ray_len(ast) != 3) return 0;
    ray_t *t = e[1];
    if (t && t->type == -RAY_SYM && !(t->attrs & Q_ATTR_QUOTED)) return 1;
    /* indexed assignment `name[i;…]:v` is silent too — kdb console */
    if (t && t->type == RAY_LIST && ray_len(t) >= 2) {
        ray_t *th = ((ray_t **)ray_data(t))[0];
        return th && th->type == -RAY_SYM && !(th->attrs & Q_ATTR_QUOTED);
    }
    return 0;
}
