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

#include "qlang/q_count.h"
#include "qlang/parse/q_parse.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"
#include "qlang/parse/q_tok.h"    /* q_tok_literal, q_tok_byte_lit_starts — the literal scanner */
#include "qlang/q_registry.h" /* q_registry_lookup_name, q_registry_is_infix, Q_DYADIC */
#include "qlang/q_ops.h"      /* q_ops_find — the static lexical manifest */
#include "qlang/q_env.h"      /* q_env_ident_ok — a captured marker must be a q name */
#include "qlang/io/q_duckdb.h" /* q_duckdb_params — the markers of a capturing text literal */
#include "qlang/eval/q_eval.h" /* q_eval_apply_is_fn(val) */
#include "qlang/eval/q_dbg.h"  /* q_dbg_statement_origin — the lambda's file + line */
#include "qlang/eval/q_pattern.h"
#include "table/sym.h"       /* ray_sym_vec_cell — qSQL dict-key/col names */
#include <ctype.h>       /* isalpha — the `<letter>)` intake prefix */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <setjmp.h>
#include "qlang/parse/q_parse_internal.h"


/* ===== parse-error escape =====================================================
 * kparser die()s (exit) on malformed input.  A REPL must not exit, so a
 * malformed parse longjmps back to its entry point, whose handler answers
 * die_answer().  die_err is the one longjmp; the others only stage its class
 * and payload. */
static jmp_buf q_err_jmp;
static q_err_e g_die_class = QE_PARSE;

/* a classed error whose TEXT is the payload — the handler answers q_err_signal:
 * a refused k-unary glyph AS the class (`$42` -> '$, `+d` -> '+, the
 * offending-text-itself shape of undefined names, basics/parsetrees.md) and
 * kx 4.0's `dup names for cols/groups b` (qsql.md:168) */
static ray_t *g_die_payload = NULL;   /* owned charv across the longjmp */

static _Noreturn void die_err(q_err_e cls) {
    g_die_class = cls;
    longjmp(q_err_jmp, 1);
}

static _Noreturn void q_die(const char *msg) {
    (void)msg;   /* class-only errors (bare 'parse); msg documents the call site */
    die_err(QE_PARSE);
}

static _Noreturn void die_signal(q_err_e cls, const char *p, size_t n) {
    g_die_payload = ray_charv(p, (int64_t)n);
    die_err(cls);
}

static int g_die_at = -1;   /* the die token's source offset when the parser knows it: the error frame's caret */

/* ===== ray_t leaf builders =================================================== */

/* name reference (ATTR_QUOTED clear): resolved by eval.  A verb is one too —
 * monadic vs dyadic is a valence decision, so kparser's KV1/KV2 split is gone. */
static ray_t *i_name(const char *s, int len) {
    return ray_sym(ray_sym_intern_runtime(s, (size_t)len));
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

static int sym_is_nameref(const ray_t *v) {
    return v && v->type == -RAY_SYM && !(v->attrs & Q_ATTR_QUOTED);
}

/* the spelling of a NAME-REF sym, owned, or NULL for anything else */
static ray_t *nameref_str(const ray_t *v) {
    return sym_is_nameref(v) ? ray_sym_str(v->i64) : NULL;
}

/* True iff v is a NAME-REF sym spelled exactly s — the one home for the
 * "compare a sym to a spelling" walk the tree checks repeat (`;` statement
 * heads, `:` alias heads, the `::` token, the join `,`). */
static int sym_name_is(const ray_t *v, const char *s) {
    return sym_is_nameref(v) && v->i64 == ray_sym_intern_runtime(s, strlen(s));
}

/* THE MONADIC MARKER, one home.  A trailing `:` on a spelling whose remainder
 * is a GLYPH manifest row is the unary-form marker (basics/variadic.md "The
 * unary form can be selected with a suffixed colon"): `+:` `,:` `0::`.  Returns
 * that row, or NULL when the spelling is not marked — which is what tells the
 * marker apart from the two-char verbs (`<=`), from the digit-colon verbs' own
 * bare spelling (`0:`), and from the colon adverbs (`':` `/:`, non-GLYPH rows). */
static const q_op_t *marker_row(const char *nm, size_t len) {
    if (len < 2 || nm[len - 1] != ':' || nm[0] == ':') return NULL;
    const q_op_t *r = q_ops_find(nm, (int)len - 1);
    return (r && r->lex == QLEX_GLYPH) ? r : NULL;
}

/* Embed the registry function VALUE for a verb sym at the given valence — the
 * 2b parser flip (`parse "2+3"` -> (+<fn>;2;3)).  A monadic-marked spelling
 * ("<g>:") probes the registry under the bare glyph.  On a miss the sym is
 * returned unchanged (unknown -> name-ref, ADR 0002).  Consumes `sym`,
 * returns owned. */
static ray_t *embed_sym(ray_t *sym, q_valence_t val) {
    ray_t *s = nameref_str(sym);
    if (!s) return sym;
    const char *nm = ray_str_ptr(s);
    size_t nl = ray_str_len(s);
    /* `::` IS the identity value wherever it lands (a tree carries values, never a `::` symbol: the global-assign
     * head, an empty call, a bare term) */
    if (nl == 2 && nm[0] == ':' && nm[1] == ':') { ray_release(s); ray_release(sym); return RAY_NULL_OBJ; }
    /* the marker IS the valence, so it wins over whatever the SITE asked for —
     * `(*:)`, `*:[x]` and `@[|:;x]` all reach here as operands, asking dyadic */
    if (marker_row(nm, nl)) { val = Q_MONADIC; nl--; }   /* "+:"->"+", "0::"->"0:" */
    ray_t *hit = q_registry_lookup_name(nm, nl, val);
    ray_release(s);
    if (!hit) hit = q_registry_keyword(sym->i64);
    /* q.q-hosted (QK_QSRC) carriers embed too: eval retains a non-sym head as a
     * value rather than re-walking it (#351) */
    if (!hit || !q_eval_apply_is_fn(hit)) return sym;
    ray_retain(hit);
    ray_release(sym);
    return hit;
}

static ray_t *embed_glyph(const char *g, q_valence_t val) {
    return embed_sym(i_name(g, (int)strlen(g)), val);
}

/* Append an already-interned symbol id to a RAY_SYM vector (W64 index width:
 * no public width-picker, and correctness beats compactness for literals). */
static ray_t *symvec_add(ray_t *vec, int64_t id) {
    return ray_vec_append(vec, &id);
}

/* symbol literal (ATTR_QUOTED set) */
static ray_t *symlit_id(int64_t id) {
    ray_t *x = ray_sym(id);
    if (x && !RAY_IS_ERR(x)) x->attrs |= Q_ATTR_QUOTED;
    return x;
}

/* Build a ray list from n owned children, releasing each after append
 * (append retains).  A C-NULL child (an empty operand, e.g. the value of
 * `()`, or a missing element) is normalised to the generic null so it never
 * reaches the walker as a bare C NULL.  The top-level program list is built
 * separately in parse_E, which DOES preserve C NULL (an empty statement /
 * whole-line comment is a no-op that must yield no output, not `::`). */
static ray_t *q_list(ray_t **xs, int n) {
    ray_t *l = ray_list_new(n > 0 ? n : 1);
    for (int i = 0; i < n; i++) {
        l = ray_list_append(l, xs[i] ? xs[i] : RAY_NULL_OBJ);
        if (xs[i]) ray_release(xs[i]);
    }
    return l;
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
    return v->type == RAY_SYM ? q_list(&v, 1) : v;
}

/* (head; e0; e1; …) — a fresh list of `head` followed by every element of the
 * BORROWED list e.  `nul` fills an empty slot (NULL: keep the C NULL, which
 * only the top-level statement list is allowed to carry). */
static ray_t *cons_head(ray_t *head, ray_t *e, ray_t *nul) {
    int64_t n = q_count(e);
    ray_t **es = (ray_t **)ray_data(e);
    ray_t *w = ray_list_new(n + 1);
    w = ray_list_append(w, head);
    for (int64_t i = 0; i < n; i++)
        w = ray_list_append(w, es[i] ? es[i] : nul);
    return w;
}

/* ===== verb / adverb tables ================================================== */

const char *ADVERB_NAMES[] = { "'", "/", "\\", "':", "/:", "\\:" };

/* THE glyph predicate, read off the manifest: how a spelling stands as a glyph verb.  GF_ONE is a one-byte glyph,
 * GF_MARKED one with the monadic `:` (`::` included), GF_OTHER the longer glyphs (`<=`, `0:`) and their markers. */
enum { GF_NONE, GF_ONE, GF_MARKED, GF_OTHER };

static int glyph_form(const char *nm, size_t l) {
    const q_op_t *row = q_ops_find(nm, (int)l);
    if (row && row->lex == QLEX_GLYPH) return l == 1 ? GF_ONE : GF_OTHER;
    if (marker_row(nm, l) || (l == 2 && nm[0] == ':' && nm[1] == ':')) return l == 2 ? GF_MARKED : GF_OTHER;
    return GF_NONE;
}

static int sym_glyph_form(const ray_t *sym) {
    ray_t *s = nameref_str(sym);
    if (!s) return GF_NONE;
    int r = glyph_form(ray_str_ptr(s), ray_str_len(s));
    ray_release(s);
    return r;
}

/* True iff a verb sym carries the MONADIC MARKER.  Standing INFIX that spelling
 * is modified assignment (`x,:y` == `x:x,y`). */
static int verb_marked(const ray_t *sym) {
    ray_t *s = nameref_str(sym);
    if (!s) return 0;
    int r = marker_row(ray_str_ptr(s), ray_str_len(s)) != NULL;
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
static int g_glyph_max;   /* the longest glyph spelling in the manifest */

/* A byte starts a verb iff the manifest spells a one-byte glyph with it (CLAUDE.md rule 3: no scanner glyph list). */
static void init_class(void) {
    if (g_glyph_max) return;
    int n = 0;
    const q_op_t *ops = q_ops_table(&n);
    for (int i = 0; i < n; i++) {
        if (ops[i].lex != QLEX_GLYPH) continue;
        int l = (int)strlen(ops[i].name);
        if (l == 1) CLASS[(uint8_t)ops[i].name[0]] |= CL_VERB;
        if (l > g_glyph_max) g_glyph_max = l;
    }
    for (int c = '0'; c <= '9'; c++) CLASS[c] |= CL_DIGIT;
    for (int c = 'a'; c <= 'z'; c++) CLASS[c] |= CL_ALPHA;
    for (int c = 'A'; c <= 'Z'; c++) CLASS[c] |= CL_ALPHA;
    CLASS[(int)'_'] |= CL_ALPHA;
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
    int    capture;   /* a sql/SELECT text literal: parse_base lowers it through text_capture */
} Token;

typedef struct { Token *t; int n; } Tokens;

/* Live token buffer, kept in a static so a q_die() longjmp (which unwinds past
 * the normal free_tokens call) can still release it — otherwise a malformed
 * input leaks the token array.  Updated as the scanner emits. */
static Tokens g_toks = { NULL, 0 };
static int g_scan_open_str;   /* the last scan ran off the end inside a string literal */
static int g_scan_open_lit;   /* … or inside a text literal: its fence length */

/* Whole literals (numeric / boolean / byte / guid / temporal) are scanned and
 * BUILT by q_tok.c — the single string->value home the `$` Tok scanners already
 * share.  Here it only has to become a token, or the error that names the
 * malformed one (`'0Wd`). */
static ray_t *scan_num_literal(const char *src, int *p) {
    int bad = 0;
    ray_t *v = q_tok_literal(src, p, &bad);
    if (!v) die_signal(QE_NAME, src + *p, (size_t)bad);
    if (RAY_IS_ERR(v)) { ray_release(v); die_err(QE_WSFULL); }
    return v;
}

/* A lone `:` next (past spaces) makes the token before it a source NAME, not an
 * infix verb.  ref/index.md:81 publishes the assign-through-operator set as an
 * explicit GLYPH list (`+: -: … ,:`); ref/assign.md scopes `x op:y` to "a binary
 * OPERATOR", and its tip routes keywords to Amend At instead.  `::` binds no name. */
static int lone_colon_next(const char *src, int p) {
    while (src[p] == ' ' || src[p] == '\t') p++;
    return src[p] == ':' && src[p + 1] != ':';
}

/* The tagged text literal (docs/parser-architecture.md § Rules): at a token start, N >= 2 backticks + a tag + one
 * whitespace byte open it and the next run of exactly N backticks closes it; the value is the bytes between, verbatim,
 * always a char vector.  NULL when no tag follows the run: it is symbols, so p is untouched. */
static ray_t *scan_text_literal(const char *src, int *pp, int *capture) {
    static const struct { const char *word; int kept, capture; } TAGS[] =
        { { "sql", 0, 1 }, { "raw", 0, 0 }, { "SELECT", 1, 1 } };
    int p = *pp, n = 0, s = 0;
    while (src[p] == '`') { p++; n++; }
    if (n < 2) return NULL;
    for (size_t i = 0; i < sizeof TAGS / sizeof *TAGS && !s; i++) {
        int l = (int)strlen(TAGS[i].word);
        if (strncmp(src + p, TAGS[i].word, (size_t)l) != 0) continue;
        char after = src[p + l];
        if (after != ' ' && after != '\t' && after != '\n') continue;
        s = TAGS[i].kept ? p : p + l + 1;
        *capture = TAGS[i].capture;
    }
    if (!s) return NULL;
    int e = s;
    for (;;) {
        if (!src[e]) { g_scan_open_lit = n; q_die("unterminated literal"); }
        if (src[e] != '`') { e++; continue; }
        int r = e;
        while (src[r] == '`') r++;
        if (r - e == n) break;
        e = r;
    }
    *pp = e + n;
    return ray_charv(src + s, (int64_t)(e - s));
}

/* The longest manifest glyph spelled at s — `<=` over `<`, `0:` over the digit — or 0. */
static int glyph_len(const char *s) {
    int best = 0;
    for (int l = 1; l <= g_glyph_max && s[l - 1]; l++) {
        const q_op_t *r = q_ops_find(s, l);
        if (r && r->lex == QLEX_GLYPH) best = l;
    }
    return best;
}

static Tokens scan(const char *src) {
    Token *toks = NULL;
    int n = 0, cap = 0;
    int p = 0;
    int noun_pos = 0, in_sig = 0;

/* KK runs FIRST: a literal payload advances p (so .len is only right after it) and may q_die (the deferred `0we`
 * float-infinity literal) before the slot exists.  g_toks.t follows every realloc so the longjmp handler frees the
 * live buffer; g_toks.n stays at the OLD count until the slot is stored. */
#define EMIT(TK, KK) do { \
        ray_t *kk_ = (KK); \
        if (n >= cap) { cap = cap ? cap * 2 : 32; toks = realloc(toks, (size_t)cap * sizeof(Token)); \
                        if (!toks) { if (kk_) ray_release(kk_); q_die("out of memory"); } \
                        g_toks.t = toks; } \
        toks[n++] = (Token){ .kind = (TK), .start = start, .len = p - start, .k = kk_ }; \
        g_toks.n = n; \
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
        ray_t *lit;
        int capture = 0;

        /* kdb sign rule: '-' adjacent to a digit, or to a leading-dot magnitude (`-.5`, qtips/deriv.q:38), is a SIGN
         * when preceded by whitespace or start-of-input (`neg -1` applies neg to -1; `x -1` indexes x at -1); it is
         * the verb only when glued to a noun (a-1). */
        int neg_sign = (c == '-' &&
                        ((CLASS[(uint8_t)src[p+1]] & CL_DIGIT) ||
                         (src[p+1] == '.' && (CLASS[(uint8_t)src[p+2]] & CL_DIGIT))) &&
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

        /* A glyph is the longest manifest spelling at p.  It may start with a digit (`0:` File Text, `1:` File
         * Binary, `2:` Dynamic Load), which can never start a clock literal: every clock needs two hour digits. */
        int name_start = ((cl & CL_ALPHA) && !q_parse_drop_lead(c)) || (c == '.' && (CLASS[(uint8_t)src[p+1]] & CL_ALPHA));
        int g = neg_sign || dot_float || name_start ? 0 : glyph_len(src + p);
        if (g) {
            /* A trailing `:` is the MONADIC MARKER, kept in the token so the tree displays kdb-style (`+:` `0::`) and
             * the parser embeds the monadic row; `::` is also the generic null / global-assign verb, the same
             * spelling.  `x op::y` is the op-assign head too (interfaces/kafka.md:53 `data,::enlist msg`), and
             * `x<>:y` assigns through the operator (ref/assign.md:100). */
            int marked = src[p + g] == ':';
            p += g + marked;
            if (marked && !(g == 1 && c == ':') && src[p] == ':') p++;
            EMIT(T_VERB, i_name(src + start, g + marked));
            noun_pos = 0;
        }
        else if ((cl & CL_DIGIT) || neg_sign || dot_float) {
            EMIT(T_NOUN, scan_num_literal(src, &p));
            noun_pos = 1;
        }
        else if (name_start) {
            while (CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) p++;
            while (src[p] == '.' && (CLASS[(uint8_t)src[p+1]] & (CL_ALPHA | CL_DIGIT))) {
                p++;
                while (CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) p++;
            }
            if (src[p] == '.') p++;   /* `k.` names k[`]: the empty last component is the null-symbol key */
            int len = p - start;
            if (len >= MAX_NAME) q_die("name too long");
            /* Reclassify only in infix position (after a noun) or glued to an iterator (`xexp'[a;b]` has the head
             * `a xexp' b` has): a prefix, standalone or BINDING `div` stays a name-ref noun — else the
             * reserved-name gate never sees a name to refuse, and the `:` is
             * silently discarded (`w[1] div:3` computing `2 div 3`). */
            if ((noun_pos || (CLASS[(uint8_t)src[p]] & CL_ADVERB)) && !lone_colon_next(src, p) &&
                q_registry_is_infix(src + start, (size_t)len)) {
                EMIT(T_VERB, i_name(src + start, len));
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
            if (src[p] != '"') { g_scan_open_str = 1; g_die_at = s - 1; q_die("unterminated string"); }
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
        else if (c == '`' && (lit = scan_text_literal(src, &p, &capture))) {
            EMIT(T_NOUN, lit);
            toks[n - 1].capture = capture;
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
                    /* FILE/handle symbol `:path — ':' and '/' are path bytes
                     * (ref/hopen.md `:localhost:5000, `:unix://5010), but '-'
                     * is not: `:a-b is subtraction, hsym `$"a-b" the escape. */
                    p++;
                    while ((CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) ||
                           src[p] == '.' || src[p] == ':' || src[p] == '/')
                        p++;
                } else {
                    /* ':' is an ordinary bare-symbol byte (`a: 1 is the symbol `a: then 1; owner 2026-11-09,
                     * timestored man.q:519); '/' joins only where more body follows (ref/doth.md .h.ha:
                     * `http://www.example.com), and '-' never does (`a-b stays subtraction).
                     * Only a run's FIRST item may not lead with '_' (`a`_ is two symbols, Dvybco). */
                    if (count > 0 || !q_parse_drop_lead(src[p])) for (;;) {
                        while ((CLASS[(uint8_t)src[p]] & (CL_ALPHA | CL_DIGIT)) ||
                               src[p] == '.' || src[p] == ':')
                            p++;
                        int sep = p;
                        while (src[sep] == ':' || src[sep] == '/') sep++;
                        if (sep == p ||
                            !((CLASS[(uint8_t)src[sep]] & (CL_ALPHA | CL_DIGIT)) ||
                              src[sep] == '.'))
                            break;
                        p = sep;
                    }
                }
                int64_t id = ray_sym_intern_runtime(src + s, (size_t)(p - s));
                if (count == 0) {
                    first = symlit_id(id);
                } else {
                    if (count == 1) {
                        vec = symvec_add(ray_sym_vec_new(RAY_SYM_W64, 4), first->i64);
                        ray_release(first); first = NULL;
                    }
                    vec = symvec_add(vec, id);
                }
                count++;
            }
            EMIT(T_NOUN, count > 1 ? vec : first);
            noun_pos = 1;
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
            case '[': kk = T_LBRACK; noun_pos = 0; in_sig = n > 0 && toks[n - 1].kind == T_LBRACE; break;
            /* a lambda signature's `]` starts the body, so `{[y;p].5*y}` reads a float (funq/ml.q:932) */
            case ']': kk = T_RBRACK; noun_pos = !in_sig; in_sig = 0; break;
            case ';': kk = T_SEMI;   noun_pos = 0; break;
            /* Unknown byte (string quote, hex, stray char, non-ASCII): error
             * rather than silently skip it — a dropped byte turns unsupported
             * q into a false "parse OK". */
            default:  g_die_at = p; q_die("unexpected character");
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
/* train: the rightmost operand is WRITTEN as a verb — a postfix elision (`count@`, `-9!`), a lone glyph (`0|+`) or
 * an iterator-suffixed expression (`neg\`) or a `g ::` tail, spaced or glued — a value that applies LATER, so what
 * stands to its left COMPOSES onto it (ref/apply.md "u v w@", ref/compose.md "(0|+)") as a compose node.  Carried left
 * through infix and juxtaposition; a keyword, a lambda, a name, parens and brackets end it (composition is syntactic,
 * 2026-09-17).  glyph: the term is a glyph WRITTEN alone in parens, `(+)` — a keyword is
 * a value too, so only the token can tell `(+)(*)` (composes) from `count distinct` (applies). */
typedef struct { Role role; ray_t *v; int train; int glyph; int at; /* source offset of the term's head token */ } P;
static const P EMPTY = { R_NONE, NULL, 0, 0, 0 };

/* The caret table (basics/debug.md): every application node, name reference and signal built, with the source
 * offset of its head token.  A lambda takes the entries made inside its body as it closes; the statement takes
 * the rest.  Static like g_toks so a q_die unwind can drop it. */
static struct { ray_t **node; int *at; int n, cap; } g_pos;

static void pos_note(ray_t *node, int at) {
    if (!node || RAY_IS_ERR(node)) return;
    if (g_pos.n == g_pos.cap) {
        int cap = g_pos.cap ? 2 * g_pos.cap : 64;
        ray_t **nd = (ray_t **)realloc(g_pos.node, (size_t)cap * sizeof *nd);
        int *at2 = (int *)realloc(g_pos.at, (size_t)cap * sizeof *at2);
        if (nd) g_pos.node = nd;
        if (at2) g_pos.at = at2;
        if (!nd || !at2) q_die("out of memory");
        g_pos.cap = cap;
    }
    g_pos.node[g_pos.n] = node;
    g_pos.at[g_pos.n++] = at;
}

/* the entries from `from` on as an OWNED i64 (node; at - base) pair vector, the table truncated back to `from` */
static ray_t *pos_take(int from, int base) {
    int n = g_pos.n - from;
    ray_t *v = ray_vec_new(RAY_I64, n > 0 ? 2 * n : 1);
    for (int i = from; v && !RAY_IS_ERR(v) && i < g_pos.n; i++) {
        int64_t ptr = (int64_t)(uintptr_t)g_pos.node[i], off = g_pos.at[i] - base;
        v = ray_vec_append(v, &ptr);
        if (v && !RAY_IS_ERR(v)) v = ray_vec_append(v, &off);
    }
    if (!v || RAY_IS_ERR(v)) q_die("out of memory");
    g_pos.n = from;
    return v;
}

static void pos_drop(void) {
    free(g_pos.node);
    free(g_pos.at);
    memset(&g_pos, 0, sizeof g_pos);
}

typedef struct {
    const char *src;
    Tokens t;
    int    pos;
    /* innermost UNSIGNED lambda's implicit-argument USES, counted per name (x y z); NULL outside lambdas and inside
     * signed ones.  parse_lambda saves/restores it so nested lambdas never leak uses outward. */
    int *xyz;
    /* >0 while parsing a lambda body — enables `:expr` early-return syntax */
    int lambda_depth;
    /* innermost lambda's names assigned so far (a pend slot, W64 syms: id for `::`, ~id for `:`) and its signature
     * (NULL: x y z) */
    ray_t **globals, *params;
    /* recursive descents in flight — see Q_PARSE_MAX_DEPTH */
    int depth;
} Parser;

/* The parser's recursion budget, charged by EVERY mutually-recursive descent
 * so no one site can exhaust the C stack alone.  Roughly two charges per level
 * of (…)/[…]/{…} and per term of `1+1+1+…`, so this supports nesting and flat
 * chains a couple of hundred deep and answers anything past that with the
 * 'limit q_eval already raises at EVAL_MAX_DEPTH. */
#define Q_PARSE_MAX_DEPTH 512

static Token *cur(Parser *p) { return &p->t.t[p->pos]; }
static int    at(Parser *p, TKind k) { return cur(p)->kind == k; }
static void   adv(Parser *p) { p->pos++; }

/* Bounded by the token COUNT, not by the trailing-T_EOF convention: a line
 * ending on the token under test must not read one past the array. */
static TKind peek(Parser *p) {
    return p->pos + 1 < p->t.n ? p->t.t[p->pos + 1].kind : T_EOF;
}

static void expect(Parser *p, TKind k, const char *msg) {
    if (at(p, k)) adv(p); else q_die(msg);
}

/* qSQL parse context, threaded (as a value arg, not parser state) through the
 * expression spine.  Q_NONE = ordinary q (all non-query parsing); the others
 * make parse_base_q stop a phrase at its legal clause boundary / separator.
 * Brackets reset to Q_NONE structurally via parse_E — no save/restore stack. */
typedef enum { Q_NONE, Q_SELECT, Q_BY, Q_FROM, Q_WHERE } QCtx;

static int qtok_sym_is(const Token *tk, const char *s);      /* name atom == s */
static int qtok_query_verb(const Token *tk);                 /* select/exec/update/delete -> QSQL_V_*, else -1 */
static int qtok_is_clause_kw(const Token *tk);               /* by/from/where */
static int qtok_is_join_comma(const Token *tk);              /* dyadic bare `,` only */

static ray_t *parse_E(Parser *p, QCtx ctx);
static P       parse_e(Parser *p, QCtx ctx);
static P       parse_e_body(Parser *p, QCtx ctx);
static P       parse_e_from(Parser *p, P t, QCtx ctx);
static P       parse_e_from_body(Parser *p, P t, QCtx ctx);
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
_Static_assert(Q_SYNTAX_EXEC - Q_SYNTAX_SELECT == QSQL_V_EXEC && Q_SYNTAX_UPDATE - Q_SYNTAX_SELECT == QSQL_V_UPDATE &&
               Q_SYNTAX_DELETE - Q_SYNTAX_SELECT == QSQL_V_DELETE, "qtok_query_verb reads the verbs by index");
#define QSQL_MAXCOLS 1024

/* Statement sequence: one -> its element; two+ -> (";"; ...).  The head is the
 * CHAR ";" (owner-reported kdb display: `parse "a:1;b:2"` shows `";"`, -10h) —
 * the same char-atom head convention the `:` early-return and `'` signal nodes
 * already use, and the reason `value parse` sees a datum here, not a name.
 * Consumes e. */
static ray_t *seq_of(ray_t *e) {
    int64_t n = q_count(e);
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

/* A sym atom in a tree is a name (parsetrees.md:80); a constant, `n included, is a pattern (owner ruling 2026-11-08). */
q_assign_target_t q_parse_assign_target(const ray_t *lhs) {
    if (!lhs || Q_IS_HOLE(lhs)) return Q_TARGET_NONE;   /* `neg[;1]` projects */
    if (lhs->type == -RAY_SYM) return Q_TARGET_NAME;
    if (lhs->type != RAY_LIST) return q_eval_apply_is_fn((ray_t *)lhs) ? Q_TARGET_NONE : Q_TARGET_PATTERN;
    const ray_t *h = q_count(lhs) > 1 ? ((ray_t **)ray_data((ray_t *)lhs))[0] : NULL;
    if (!h) return Q_TARGET_PATTERN;   /* () or an enclosed constant, ,`a`b */
    /* `p:`c` is the type-check pattern; `:` is no variable's name, so any other `p:e` (a filter) is no target */
    if (q_parse_assign_head(h, NULL, NULL) == Q_HEAD_ASSIGN)
        return q_count(lhs) == 3 && q_pattern_type_check(((ray_t **)ray_data((ray_t *)lhs))[2]) ? Q_TARGET_PATTERN : Q_TARGET_NONE;
    if (h->type == -RAY_SYM) return Q_TARGET_INDEX;
    /* the heads the parser gives a pattern: a list, a dictionary (and keyed table), a table, and the type check
     * `(:`f)` (basics/pattern.md) */
    if (h == q_registry_list_value() || h == q_registry_lookup_name("!", 1, Q_DYADIC) ||
        h == q_registry_lookup_name("+", 1, Q_MONADIC) || RAY_IS_NULL(h)) return Q_TARGET_PATTERN;
    return Q_TARGET_NONE;
}

/* the head test app code run on kdb trees applies, qchk.q:83 `$[101=type e0;20>value e0;e0~(:)]` */
q_assign_head_t q_parse_assign_head(const ray_t *h, ray_t **op, const q_op_t **row) {
    if (RAY_IS_NULL(h)) return Q_HEAD_GLOBAL;
    /* `<>:` `<=:` `>=:` (ref/assign.md:100) have no unary value to stand for them, so their marked spelling stays */
    ray_t *s = nameref_str(h);
    const q_op_t *mr = s ? marker_row(ray_str_ptr(s), ray_str_len(s)) : NULL;
    if (s) ray_release(s);
    if (mr) {
        if (op) *op = q_registry_row_value(mr, Q_DYADIC);
        if (row) *row = mr;
        return Q_HEAD_THROUGH;
    }
    q_valence_t val = Q_DYADIC;
    /* only a primitive's own ray type asks: a miss in the registry scans the manifest, and this runs per node */
    int code = h && (h->type == RAY_UNARY || h->type == RAY_BINARY || h->type == RAY_VARY) ? q_registry_kdb_op_of(h, &val) : -1;
    if (code == 0) return val == Q_DYADIC ? Q_HEAD_ASSIGN : Q_HEAD_GLOBAL;
    if (code < 0 || code >= 20 || val != Q_MONADIC) return Q_HEAD_NONE;
    ray_t *v = q_registry_kdb_op_value(code, Q_DYADIC);
    if (op) *op = v;
    if (row) *row = v ? q_registry_row_of(v, Q_DYADIC) : NULL;
    return v ? Q_HEAD_THROUGH : Q_HEAD_NONE;
}

/* the char-";" sequence head (see seq_of) */
int q_parse_is_seq_head(const ray_t *h) {
    return h && h->type == -RAY_CHARV && h->u8 == ';';
}

/* q_ast_fill_empty_stmts — see q_parse.h.  DATA-boundary twin of seq_of: the
 * eval path needs the C-NULL empty-statement slots (no-op, no output), but a
 * tree handed out as a VALUE must be kdb-shaped: `parse ";"` is (;;();()). */
void q_ast_fill_empty_stmts(ray_t *ast) {
    if (!ast || ast->type != RAY_LIST || q_count(ast) < 2) return;
    ray_t **e = (ray_t **)ray_data(ast);
    if (!q_parse_is_seq_head(e[0])) return;
    for (int64_t i = 1; i < q_count(ast); i++)
        if (!e[i]) e[i] = ray_list_new(1);   /* len-0 general list: () */
}

/* ===== qSQL context predicates ===============================================
 * Pure token reads used by parse_base_q to decide where an ordinary-q phrase
 * must stop inside a qSQL clause.  All are inert while ctx == Q_NONE. */

static int qtok_sym_is(const Token *tk, const char *s) {
    return tk->kind == T_NOUN && sym_name_is(tk->k, s);
}

/* the qSQL query verbs, read off the manifest's syntax words in QSQL_V_* order: parse_base_q routes each into
 * parse_query, in statement and operand position alike (`count select a from (select b from t)`) */
static int qtok_query_verb(const Token *tk) {
    for (int v = QSQL_V_SELECT; v <= QSQL_V_DELETE; v++)
        if (qtok_sym_is(tk, q_ops_syntax_word(Q_SYNTAX_SELECT + v))) return v;
    return -1;
}

/* the clause keywords that separate qSQL sections */
static int qtok_is_clause_kw(const Token *tk) {
    return qtok_sym_is(tk, "by") || qtok_sym_is(tk, "from") ||
           qtok_sym_is(tk, "where");
}

/* the dyadic bare join `,` verb token (NOT monadic enlist `,:`) */
static int qtok_is_join_comma(const Token *tk) {
    return tk->kind == T_VERB && sym_name_is(tk->k, ",");
}

/* ===== source column names — the two shapes both column syntaxes read ========
 * A table literal and a qSQL clause name a column the same two ways: written
 * `name:expr`, or a bare name-ref lending its own spelling (syntax.md:218). */

/* The (:;name;expr) node.  Rejects `::`, `+:` (an assign-through head) and a QUOTED
 * lhs, so ``(`a:1)`` is not a binding. */
static int binding_node(ray_t *x, ray_t **name, ray_t **val) {
    if (!x || x->type != RAY_LIST || q_count(x) != 3) return 0;
    ray_t **e = (ray_t **)ray_data(x);
    if (q_parse_assign_head(e[0], NULL, NULL) != Q_HEAD_ASSIGN || !sym_is_nameref(e[1])) return 0;
    if (name) *name = e[1];
    if (val)  *val  = e[2];
    return 1;
}

static int qsql_glyph_names_right(const q_op_t *row);

/* A term built of quoted symbols only: ,`s or ,`s`t as noun_tree_value wraps them, a paren list of them, or one
 * applied to another (`. `sym, TorQ tickerlogreplay.q:310). */
static int sym_only_term(ray_t *e) {
    if (e && e->type == RAY_SYM) return 1;
    if (!e || e->type != RAY_LIST || q_count(e) < 1) return 0;
    ray_t **a = (ray_t **)ray_data(e);
    int64_t n = q_count(e), i = a[0] == q_registry_list_value();
    if (i >= n) return 0;
    for (; i < n; i++)
        if (!sym_only_term(a[i])) return 0;
    return 1;
}

/* Which argument names an unnamed phrase (qsql.md:234): a glyph's first, any other function's last; -1 for none.
 * A quoted symbol is skipped for the next term (funq decisiontree.q:56, TorQ tickerlogreplay.q:310, Diljhu). */
static int alias_arg(ray_t *expr) {
    if (!expr || expr->type != RAY_LIST || q_count(expr) < 2) return -1;
    ray_t **a = (ray_t **)ray_data(expr);
    int n = (int)q_count(expr);
    const q_op_t *row = q_registry_operand_row(a[0]);
    int first = a[0] == q_registry_list_value() || (row && row->lex == QLEX_GLYPH && !qsql_glyph_names_right(row));
    int k = first ? 1 : n - 1;
    while (k < n && sym_only_term(a[k])) k++;
    return k < n ? k : -1;
}

/* The binding on a phrase's naming path (the phrase itself, or `g#sym:…'s), else NULL. */
static ray_t *alias_binding(ray_t *expr) {
    for (int k; expr && !binding_node(expr, NULL, NULL); expr = ((ray_t **)ray_data(expr))[k])
        if ((k = alias_arg(expr)) < 0) return NULL;
    return expr;
}

/* Count one use (d=+1) or retract one (d=-1) of an implicit-argument name spelled nm[0..nl). */
static void xyz_note(int *xyz, const char *nm, size_t nl, int d) {
    if (xyz && nl == 1 && nm[0] >= 'x' && nm[0] <= 'z') xyz[nm[0] - 'x'] += d;
}

/* A column a phrase list BINDS is not a use (`{t:([] x:til 3)}` has rank 1, Dmjqjn; `{update z:0 from t}` too,
 * Dvxzov): its name token was counted when consumed, so retract it.  A column READ stays a use. */
static void xyz_unbind(int *xyz, ray_t *phrases) {
    if (!xyz || !phrases || phrases->type != RAY_LIST) return;
    for (int64_t i = 0; i < q_count(phrases); i++) {
        ray_t *name = NULL, *s;
        if (!binding_node(alias_binding(((ray_t **)ray_data(phrases))[i]), &name, NULL) || !(s = nameref_str(name)))
            continue;
        xyz_note(xyz, ray_str_ptr(s), ray_str_len(s), -1);
        ray_release(s);
    }
}

static int keyword_phrase(const ray_t *x) {
    return x && x->type != -RAY_SYM && x->type != RAY_LIST && q_registry_operand_row(x);
}

/* Reserved-ness is LEXICAL — refused wherever SOURCE names a column, exactly as
 * `null:5` is (owner ruling 2026-08-15: "columns cant be reserved words"), in
 * BOTH spellings above.  A name arriving as DATA never reaches here, so
 * ``flip `null`b!(1 2;3 4)`` still builds the column and .Q.id keeps its repair
 * job (ref/dotq.md:1308).  A manifest keyword standing alone as a phrase is
 * already its value (the head law), and lends its spelling all the same.
 * Answers rather than dies: each caller owns different refs at the point it can
 * afford to longjmp. */
static int names_have_reserved(ray_t *phrases) {
    if (!phrases || phrases->type != RAY_LIST) return 0;
    int64_t n = q_count(phrases);
    ray_t **ph = (ray_t **)ray_data(phrases);
    for (int64_t i = 0; i < n; i++) {
        ray_t *name = NULL;
        if (keyword_phrase(ph[i])) return 1;
        if (!binding_node(ph[i], &name, NULL) && sym_is_nameref(ph[i])) name = ph[i];
        if (name && (q_registry_is_reserved(name->i64) || q_registry_dotq_ext(name->i64))) return 1;
    }
    return 0;
}

/* ===== table literal -> kdb's dict-then-flip parse tree ====================
 * (owner ruling 2026-07-24).  An unnamed column takes qSQL's derived name
 * (qsql_derive_alias), so `([] f a)` is `a` exactly as `select f a` is. */

static int64_t qsql_derive_alias(ray_t *expr);
static ray_t  *alias_unbind(ray_t *expr);

/* pairwise duplicate WITHIN one W64 sym-id vector */
static int symvec_ids_dup(ray_t *a) {
    if (!a || a->type != RAY_SYM) return 0;
    int64_t n = q_count(a);
    const int64_t *s = (const int64_t *)ray_data(a);
    for (int64_t i = 1; i < n; i++)
        for (int64_t j = 0; j < i; j++)
            if (s[i] == s[j]) return 1;
    return 0;
}

/* the first select-COLUMN name (phrase order) also a by-GROUP name, or -1 —
 * the cols/groups CROSS collision of qsql.md:168's two parse-error examples
 * (`select b by b from t` -> b, `select a,a by a from t` -> a) */
static int64_t qsql_cross_names_dup(ray_t *A, ray_t *B) {
    ray_t *ka = A && A->type == RAY_DICT ? ray_dict_keys(A) : NULL;
    ray_t *kb = B && B->type == RAY_DICT ? ray_dict_keys(B) : NULL;
    if (!ka || ka->type != RAY_SYM || !kb || kb->type != RAY_SYM) return -1;
    int64_t na = q_count(ka), nb = q_count(kb);
    const int64_t *sa = (const int64_t *)ray_data(ka);
    const int64_t *sb = (const int64_t *)ray_data(kb);
    for (int64_t i = 0; i < na; i++)
        for (int64_t j = 0; j < nb; j++)
            if (sa[i] == sb[j]) return sa[i];
    return -1;
}

/* (!;a;b) — consumes a,b */
static ray_t *bang_tree(ray_t *a, ray_t *b) {
    ray_t *xs[3] = { embed_glyph("!", Q_DYADIC), a, b };
    return q_list(xs, 3);
}

/* col defs -> the dictionary arm (!;names;(enlist;e…)); consumes defs */
static ray_t *table_lit_dict(ray_t *defs) {
    ray_t *lv = q_registry_list_value();
    if (!lv) q_die("table literal: registry not initialized");
    if (names_have_reserved(defs)) { ray_release(defs); die_err(QE_ASSIGN); }
    int64_t n = q_count(defs);
    ray_t **ds = (ray_t **)ray_data(defs);
    int64_t id_x = ray_sym_intern_runtime("x", 1);
    ray_t *keys = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    ray_t *vals = ray_list_new(n + 1);
    if (n > 0) vals = ray_list_append(vals, lv);       /* no columns: the empty list itself */
    for (int64_t i = 0; i < n; i++) {
        ray_t *d = ds[i], *ex = d, *nmv = NULL;
        int64_t nm = -1;
        if (binding_node(d, &nmv, &ex)) {
            nm = nmv->i64;
            ray_retain(ex);
        } else {
            nm = qsql_derive_alias(d);
            nm = q_name_dedup(nm < 0 ? id_x : nm, (int64_t *)ray_data(keys), i, 0);
            ex = d ? alias_unbind(d) : NULL;
        }
        keys = ray_vec_append(keys, &nm);
        vals = ray_list_append(vals, ex ? ex : RAY_NULL_OBJ);
        ray_release(ex);
    }
    /* a dup column name in the LITERAL dies at parse like a select's
     * (qsql/select_fixes.qcmd pins `([] a:1 2;a:3 4)` -> 'dup); the same
     * names through raw `!`/`flip` stay legal — construction is permissive */
    if (symvec_ids_dup(keys)) {
        ray_release(keys); ray_release(vals); ray_release(defs);
        die_err(QE_DUP);
    }
    /* the names are a sym-vector CONSTANT, enclosed as every symvec in a tree is: ,`a`b (parsetrees.md:26), and
     * one name ,,`a — bare, ,`a would be the enlisted atom and `a`b the tree a[b] */
    if (n > 0) keys = noun_tree_value(keys);
    ray_release(defs);
    return bang_tree(keys, vals);
}

/* col defs -> (+:;(!;…)) — flip of the dictionary arm; consumes defs */
static ray_t *table_lit_flip(ray_t *defs) {
    ray_t *xs[2] = { embed_glyph("+", Q_MONADIC), table_lit_dict(defs) };
    return q_list(xs, 2);
}

/* ===== the capturing text literal ============================================
 * A sql/SELECT literal, or an s) line, whose text has NAMED markers lowers to the pair `(text; names!refs)` spelled
 * as a hand would — `("…";(enlist `v)!enlist v)` for one name, `("…";`v`w!(v;w))` for more — so each name is an
 * ordinary reference, resolved by q scoping at eval (docs/parser-architecture.md § Rules). */

/* Consumes text and answers it unchanged when nothing captures; a captured x/y/z is an implicit lambda argument. */
static ray_t *text_capture(ray_t *text, int *xyz) {
    const char *s = (const char *)ray_data(text);
    int64_t n = q_count(text);
    ray_t *all = q_duckdb_params_spelled(s, (size_t)n);
    ray_t *folded = q_duckdb_params(s, (size_t)n);
    if (RAY_IS_ERR(all) || RAY_IS_ERR(folded)) {
        ray_release(all); ray_release(folded); ray_release(text);
        die_err(QE_WSFULL);
    }
    int collide = q_count(folded) < q_count(all);
    ray_release(folded);
    if (collide) { ray_release(all); ray_release(text); q_die("two marker spellings fold to one name"); }
    ray_t *names = ray_sym_vec_new(RAY_SYM_W64, 1);
    for (int64_t k = 0; k < q_count(all); k++) {
        ray_t *str = ray_sym_vec_cell(all, k);
        const char *nm = ray_str_ptr(str);
        size_t nl = ray_str_len(str);
        if (isdigit((unsigned char)nm[0])) continue;
        if (!q_env_ident_ok(nm, nl)) {
            ray_release(all); ray_release(names); ray_release(text);
            q_die("marker is not a q name");
        }
        xyz_note(xyz, nm, nl, 1);
        names = ray_vec_append(names, ray_vec_get(all, k));
    }
    ray_release(all);
    int64_t m = q_count(names);
    if (!m) { ray_release(names); return text; }
    ray_t *refs = ray_list_new(m);
    for (int64_t k = 0; k < m; k++) {
        ray_t *r = ray_sym(*(const int64_t *)ray_vec_get(names, k));
        refs = ray_list_append(refs, r);
        ray_release(r);
    }
    ray_t *hd = q_registry_list_value(), *keys;
    if (m == 1) {
        ray_retain(hd);
        ray_t *kx[2] = { hd, names };
        keys = q_list(kx, 2);
    } else {
        keys = noun_tree_value(names);
    }
    ray_t *vals = cons_head(hd, refs, NULL);
    ray_release(refs);
    ray_t *px[2] = { text, bang_tree(keys, vals) };
    ray_t *e = q_list(px, 2);
    ray_t *pair = cons_head(q_registry_list_value(), e, NULL);
    ray_release(e);
    return pair;
}

/* A `.q` keyword's value as a tree constant (funsql.md:769 — a keyword is replaced by its definition): a symbol or
 * a list would read back as a name or a call, so it is enlisted as a sym literal is (parsetrees.md:80). */
static ray_t *keyword_constant(ray_t *v) {
    if (v->type == RAY_LIST) return ray_list_append(ray_list_new(1), v);
    if (v->type == -RAY_SYM) {
        v = ray_sym(v->i64);
        if (v && !RAY_IS_ERR(v)) v->attrs |= Q_ATTR_QUOTED;
    } else ray_retain(v);
    return noun_tree_value(v);
}

/* what follows token i makes the name before it the TARGET of an assignment (`k:`, `k+:`, `k[i]:`, `k::y`); a
 * `k::` with nothing after is a train tail, spaced or glued */
static int assign_target_at(Parser *p, int i) {
    while (i < p->t.n && p->t.t[i].kind == T_LBRACK)
        for (int depth = 0; i < p->t.n; ) {
            TKind k = p->t.t[i++].kind;
            depth += (k == T_LBRACK) - (k == T_RBRACK);
            if (!depth) break;
        }
    if (i >= p->t.n || p->t.t[i].kind != T_VERB) return 0;
    Token *t = &p->t.t[i];
    if (sym_name_is(t->k, "::")) {
        TKind after = i + 1 < p->t.n ? p->t.t[i + 1].kind : T_EOF;
        return after != T_EOF && after != T_SEMI && after != T_RPAREN && after != T_RBRACK && after != T_RBRACE;
    }
    return sym_name_is(t->k, ":") || verb_marked(t->k);
}

/* `k::y` with no index is a global write, which binds a keyword's root slot (owner ruling 2026-11-08) */
static int global_assign_at(Parser *p, int i) {
    return i < p->t.n && p->t.t[i].kind == T_VERB && sym_name_is(p->t.t[i].k, "::") && assign_target_at(p, i);
}

/* ===== the parser's die-path leak guard ======================================
 * A frame that holds an owned ref across a descent that can die (a phrase list,
 * a from-expression, a lambda's signature and body, a statement list) parks it
 * in a slot on this stack; every entry point's handler (die_answer) releases
 * what is still parked.  LIFO: each frame pops what it pushed before returning,
 * having moved the refs into its result or released them.
 *
 * The slot lives IN the frame, on the heap: longjmp resets the stack pointer,
 * so the handler's own calls reuse the abandoned parser frames as it walks —
 * a slot addressed into a caller's locals would be read after something else
 * had overwritten it. */
typedef struct pend { ray_t *v; struct pend *prev; } pend_t;
static pend_t *g_pend = NULL;

/* the caller's handle on its in-flight ref, valid until its pop */
static ray_t **pend_push(void) {
    pend_t *f = (pend_t *)malloc(sizeof *f);
    if (!f) q_die("out of memory");
    f->v = NULL; f->prev = g_pend; g_pend = f;
    return &f->v;
}
static void pend_pop(void) {                      /* success: just unlink */
    pend_t *f = g_pend;
    if (!f) return;
    g_pend = f->prev;
    free(f);
}
static void pend_unwind(void) {                   /* a die: release every ref */
    while (g_pend) {
        if (g_pend->v) ray_release(g_pend->v);
        pend_pop();
    }
}

/* Table literal `([k…] c1:e1; …)`, the cursor past its `(` and on the `[`.
 * Emits kdb's dict-then-flip parse tree: unkeyed (+:;(!;…)); keyed
 * keytab!valtab; NO value columns (`([a:`A;c:`C])`, xcol's rename
 * map) = the DICTIONARY literal (ref/cols.md; ChangesIn4.1.md). */
static ray_t *table_lit_cols(Parser *p) {
    adv(p);
    if (at(p, T_RBRACK)) {
        adv(p);
        /* `([])` is the ZERO-column empty table (qlib/src/unit.q:180
         * `tmpDirs:([])`), not one elided column auto-named x. */
        ray_t *cols = at(p, T_RPAREN) ? ray_list_new(1) : parse_E(p, Q_NONE);
        expect(p, T_RPAREN, "expected ')'");
        xyz_unbind(p->xyz, cols);
        return table_lit_flip(cols);
    }
    ray_t *kcols = parse_E(p, Q_NONE);
    expect(p, T_RBRACK, "expected ']' in keyed table literal");
    xyz_unbind(p->xyz, kcols);
    /* optional `;` after `]` — `([a:`x`y];b:10 20)` == `([a:`x`y]b:10 20)` */
    if (at(p, T_SEMI)) adv(p);
    if (at(p, T_RPAREN)) {
        adv(p);
        return table_lit_dict(kcols);
    }
    ray_t *vcols = parse_E(p, Q_NONE);
    expect(p, T_RPAREN, "expected ')'");
    xyz_unbind(p->xyz, vcols);
    return bang_tree(table_lit_flip(kcols), table_lit_flip(vcols));
}

/* A column label is no assignment, so the global-as-local check skips the literal (`{sym::1; ([]sym:())}`) */
static ray_t *parse_table_lit(Parser *p) {
    ray_t **globals = p->globals;
    p->globals = NULL;
    ray_t *t = table_lit_cols(p);
    p->globals = globals;
    return t;
}

/* `(+)` or `((+))`, the cursor past the first `(`: a glyph written alone in parens */
static int lone_glyph(Parser *p) {
    int i = p->pos, n = 0;
    for (; i < p->t.n && p->t.t[i].kind == T_LPAREN; i++) n++;
    if (i >= p->t.n || p->t.t[i].kind != T_VERB || sym_glyph_form(p->t.t[i].k) == GF_NONE) return 0;
    while (n-- >= 0)
        if (++i >= p->t.n || p->t.t[i].kind != T_RPAREN) return 0;
    return 1;
}

/* `( … )`, the cursor past its `(`: a table literal, grouping, or a list literal */
static ray_t *parse_paren(Parser *p) {
    if (at(p, T_LBRACK)) return parse_table_lit(p);
    ray_t *e = parse_E(p, Q_NONE);
    expect(p, T_RPAREN, "expected ')'");
    int64_t n = q_count(e);
    ray_t **slots = (ray_t **)ray_data(e);
    /* Inside parens an elided element is a projection HOLE — the literal
     * becomes a projection of the list constructor ("omission of values
     * results in projection", releases/ChangesIn4.1.md; the 8-item cap in
     * basics/application.md is the function-rank cap), so `(1;;3) 7` fills
     * while a list holding an explicit `::` value stays data and indexes. */
    if (n > 1)
        for (int64_t i = 0; i < n; i++)
            if (!slots[i]) slots[i] = Q_HOLE_OBJ;
    /* `(1)` is grouping -> the lone element.  A NULL slot is the EMPTY paren
     * `()`: the empty general list is its own constant tree, NOT `::` and not
     * a bare constructor application (parsetrees.md:18). */
    if (n == 1) {
        ray_t *only = slots[0];
        if (only) ray_retain(only);
        ray_release(e);
        return only ? only : ray_list_new(0);
    }
    /* A list literal: prepend the internal list-constructor value so eval
     * builds (and collapses) it.  The head value is the only thing
     * distinguishing `(1 2;3 4)` from the shape-identical bracket-index call
     * (v;i); q_fmt hides it so the tree still displays (1;2;3).  A cold
     * registry (no q_runtime) has no constructor, so the list stays bare. */
    ray_t *lv = q_registry_list_value();
    if (!lv) return e;
    ray_t *w = cons_head(lv, e, NULL);
    ray_release(e);
    return w;
}

/* a parameter is a binding position, so a reserved name or a user `.q` keyword is refused */
static void sig_name_check(int64_t id, void *ctx) {
    (void)ctx;
    if (q_registry_is_reserved(id) || q_registry_dotq_ext(id)) die_err(QE_ASSIGN);
}

/* a general list parallel to the signature's slots so far, the generic null on each */
static ray_t *sig_pats_new(int64_t nslots) {
    ray_t *pats = ray_list_new(nslots + 1);
    for (int64_t i = 0; i < nslots; i++) pats = ray_list_append(pats, RAY_NULL_OBJ);
    return pats;
}

/* A parenthesised slot, the cursor on its `(`: the same tree an assignment target parses to, appended to *pats.
 * It reads no implicit x/y/z and declares nothing in the enclosing lambda. */
static void sig_pattern(Parser *p, ray_t **pats, int64_t nslots) {
    int *xyz = p->xyz;
    ray_t **globals = p->globals;
    p->xyz = NULL;
    p->globals = NULL;
    adv(p);
    ray_t *pat = parse_paren(p);
    p->xyz = xyz;
    p->globals = globals;
    if (!*pats) *pats = sig_pats_new(nslots);
    *pats = ray_list_append(*pats, pat);
    ray_release(pat);
    pat = ray_list_get(*pats, q_count(*pats) - 1);   /* borrowed from the parked list, so a die below leaks nothing */
    if (!q_pattern_supported(pat)) q_die("expected parameter name in lambda signature");
    q_pattern_names(pat, sig_name_check, NULL);
}

/* A lambda signature `[a;b:`j;(c;d);;…]`, the cursor on its `[`, into the parked *params (a RAY_SYM vector),
 * *ptypes (i64 kdb type nums, 0 = untyped slot; NULL while no slot is typed) and *pats (a slot's list pattern,
 * the generic null on a plain slot; NULL while no slot destructures).  An elided slot is the null pattern and a
 * destructured one binds only its pattern's names: both are named by the null symbol, as `{[]}` is. */
static void parse_signature(Parser *p, ray_t **params, ray_t **ptypes, ray_t **pats) {
    adv(p);
    *params = ray_sym_vec_new(RAY_SYM_W64, 4);
    int64_t nul = ray_sym_intern_runtime("", 0);
    if (!at(p, T_RBRACK)) for (;;) {
        Token *nt = cur(p);
        int64_t id = nul, nslots = q_count(*params);
        int typed = nt->kind == T_VERB && sym_name_is(nt->k, ":");
        int destructured = nt->kind == T_LPAREN;
        if (destructured) sig_pattern(p, pats, nslots);
        if (!destructured && !typed && !at(p, T_SEMI) && !at(p, T_RBRACK)) {
            if (nt->kind != T_NOUN || !sym_is_nameref(nt->k))
                q_die("expected parameter name in lambda signature");
            id = nt->k->i64;
            sig_name_check(id, NULL);
            adv(p);
        }
        if (!destructured && *pats) *pats = ray_list_append(*pats, RAY_NULL_OBJ);
        *params = ray_vec_append(*params, &id);
        /* `slot:`c` type annotation (kx type-check pattern, 4.1+): ONE quoted
         * one-char sym; lowercase = atom (negative kdb type num), uppercase =
         * list.  Every other spelling — bare identifier (kx's filter slot,
         * reserved), sym-vector union `j`J, long name `long, unknown char —
         * dies at DEFINITION so its grammar stays reachable later. */
        int64_t tnum = 0;
        if (cur(p)->kind == T_VERB && sym_name_is(cur(p)->k, ":")) {
            adv(p);
            Token *tt = cur(p);
            if (tt->kind == T_NOUN && tt->k && tt->k->type == -RAY_SYM && (tt->k->attrs & Q_ATTR_QUOTED))
                tnum = q_pattern_type_num(tt->k->i64);
            if (!tnum) q_die("expected `<typechar> in lambda signature");
            adv(p);
        }
        if (tnum && !*ptypes) {
            *ptypes = ray_vec_new(RAY_I64, 4);
            for (int64_t z = 0; z < nslots; z++) {
                int64_t zero = 0;
                *ptypes = ray_vec_append(*ptypes, &zero);
            }
        }
        if (*ptypes) *ptypes = ray_vec_append(*ptypes, &tnum);
        if (!at(p, T_SEMI)) break;
        adv(p);
    }
    expect(p, T_RBRACK, "expected ']' after lambda signature");
    if (q_count(*params) > 8) die_err(QE_PARAMS);   /* basics/errors.md: "Too many parameters (8 max)" */
    /* `{[] ...}` is RANK 1 with an UNNAMEABLE slot: the null symbol is no
     * variable's name, so a spare argument lands where no body can read it
     * and `x` stays GLOBAL — default names come with OMITTING the signature
     * (function-notation.md "Signature").  Owner ruling 2026-08-12. */
    if (!q_count(*params)) *params = ray_vec_append(*params, &nul);
}

/* a destructured slot's names are locals before the first statement, as a parameter is: `a::` writes the local */
static void sig_local(int64_t id, void *ctx) {
    ray_t **globals = ctx;
    *globals = symvec_add(*globals, ~id);
}

/* Lambda literal `{[sig] stmt;...}` -> the RAY_QFN carrier VALUE, built AT
 * PARSE (value-heads-at-parse; kdb parse of a lambda returns the function
 * atom).  src is the VERBATIM `{...}` span (kdb echoes it byte-for-byte);
 * params is an explicit signature, x/y/z inferred by highest implicit used
 * (min rank 1), or ONE null-named slot for `{[] ...}`.  No VALUE capture:
 * globals resolve at CALL time, in the `\d` context the carrier records
 * (q_eval_apply_lambda_new's layout). */
static ray_t *parse_lambda(Parser *p) {
    int lb_start = cur(p)->start, mark = g_pos.n;
    adv(p);
    ray_t **params = pend_push(), **ptypes = pend_push(), **pats = pend_push(), **body = pend_push();
    if (at(p, T_LBRACK)) parse_signature(p, params, ptypes, pats);
    int mine[3] = { 0, 0, 0 };
    int *saved = p->xyz;
    ray_t **saved_globals = p->globals, *saved_params = p->params, **globals = pend_push();
    *globals = ray_sym_vec_new(RAY_SYM_W64, 4);
    for (int64_t i = 0, n = *pats ? q_count(*pats) : 0; i < n; i++)
        q_pattern_names(ray_list_get(*pats, i), sig_local, globals);
    p->xyz = *params ? NULL : mine;
    p->globals = globals;
    p->params = *params;
    p->lambda_depth++;
    *body = parse_E(p, Q_NONE);
    p->lambda_depth--;
    p->xyz = saved;
    p->globals = saved_globals;
    p->params = saved_params;
    expect(p, T_RBRACE, "expected '}'");
    Token *rb = &p->t.t[p->pos - 1];
    ray_t *src = ray_str(p->src + lb_start, (size_t)(rb->start + rb->len - lb_start));
    if (!*params) {
        int hi = mine[2] > 0 ? 3 : mine[1] > 0 ? 2 : 1;
        *params = ray_sym_vec_new(RAY_SYM_W64, 4);
        static const char *xyz[3] = { "x", "y", "z" };
        for (int i = 0; i < hi; i++)
            *params = symvec_add(*params, ray_sym_intern_runtime(xyz[i], 1));
    }
    int64_t bn = q_count(*body);
    ray_t **bs = (ray_t **)ray_data(*body);
    /* empty statements (`{2*x;}`) become the generic null: the carrier
     * retains every body expr, so a C NULL here would be fatal */
    for (int64_t i = 0; i < bn; i++)
        if (!bs[i]) bs[i] = RAY_NULL_OBJ;
    ray_t *pos = pos_take(mark, lb_start);
    ray_t *fn = q_eval_apply_lambda_new(*params, bs, bn, src, *ptypes, *pats, pos);
    ray_release(pos);
    /* ref/value.md `f`/`l`: the lambda's origin is where it is PARSED — the loading script's
     * file, and the line its `{` sits on (a multi-line statement's inner lambda has its own) */
    int64_t line = 0, file = q_dbg_statement_origin(&line);
    if (file) {
        for (int i = 0; i < lb_start; i++) line += p->src[i] == '\n';
        q_eval_apply_lambda_origin(fn, file, line);
    }
    ray_release(src);
    ray_release(*globals);
    ray_release(*body);
    ray_release(*pats);
    ray_release(*ptypes);
    ray_release(*params);
    pend_pop(); pend_pop(); pend_pop(); pend_pop(); pend_pop();
    return fn;
}

static P parse_base(Parser *p) {
    Token *tk = cur(p);
    switch (tk->kind) {
    case T_NOUN: {
        /* implicit-arg inference: a bare x/y/z name inside the current UNSIGNED lambda body is a use (kdb ranks by
         * the highest used) until a column binding retracts it (xyz_unbind) */
        if (sym_is_nameref(tk->k)) xyz_note(p->xyz, p->src + tk->start, (size_t)tk->len, 1);
        ray_t *v = tk->k; tk->k = NULL;
        v = tk->capture ? text_capture(v, p->xyz) : noun_tree_value(v);
        adv(p);
        ray_t *kw = sym_is_nameref(v) && !global_assign_at(p, p->pos) ? q_registry_keyword(v->i64) : NULL;
        if (kw && assign_target_at(p, p->pos)) {
            /* refused HERE, not at the write: dropping the entry later must not turn a parsed `k:` into a global */
            ray_release(v);
            die_err(QE_ASSIGN);
        } else if (kw) {
            ray_release(v);
            v = keyword_constant(kw);
        } else if (sym_is_nameref(v)) {
            pos_note(v, tk->start);            /* an undefined name's error points at the name */
        }
        return (P){ R_NOUN, v, 0, 0, tk->start };
    }
    case T_VERB: {
        ray_t *v = tk->k; tk->k = NULL;
        adv(p);
        return (P){ R_VERB, v, 0, 0, tk->start };
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
        return (P){ R_NOUN, b ? b : RAY_NULL_OBJ, 0, 0, tk->start };
    }
    case T_LPAREN: {
        adv(p);
        int glyph = lone_glyph(p);
        return (P){ R_NOUN, parse_paren(p), 0, glyph, tk->start };
    }
    case T_LBRACE:
        return (P){ R_NOUN, parse_lambda(p), 0, 0, tk->start };
    case T_ADVERB: {
        /* `'[f;g;…]` applies the one `'` iterator value, as `f'` does (`'[;]` resolves the overloads on `'`,
         * ref/compose.md:38), so `'[+]` and `+'` are one tree (unparse testcases.q:100, run on KDB-X); the apply
         * module composes two or more values.  A bare postfix adverb (`f'`) is consumed by parse_term's postfix
         * loop and a leading signal `'expr` by parse_e, so a `'` here NOT followed by `[` is not a term. */
        if (tk->len == 1 && p->src[tk->start] == '\'' &&
            peek(p) == T_LBRACK) {
            adv(p);                          /* consume ' */
            adv(p);                          /* consume [ */
            ray_t *args = parse_E(p, Q_NONE);
            expect(p, T_RBRACK, "expected ']' in compose '[...]'");
            ray_t *w = cons_head(tk->k, args, Q_HOLE_OBJ);   /* `'[;]` projects the iterator value itself */
            ray_release(args);
            pos_note(w, tk->start);
            return (P){ R_NOUN, w, 0, 0, tk->start };
        }
        /* otherwise a bare iterator in TERM position is its own VALUE — the
         * `\` of `type each(…;\;…)` (basics/datatypes.md) */
        ray_t *iv = tk->k;
        tk->k = NULL;
        adv(p);
        return (P){ R_NOUN, iv, 0, 0, tk->start };
    }
    default:
        return EMPTY;
    }
}

/* select[…] sort spec `<c` / `>c` -> kdb's ENLISTED k-unary pair `,(<:;`c)`; the key may be an expression
 * (`select[>x+1]`, uqf lib/unparse/testcases.q, which KDB-X parses)
 * (extracted/basics/funsql.qcmd:391) — the registry VALUE at the head, never
 * an iasc/idesc spelling; the enlist is the tree quote, so the pair reaches
 * `?` as DATA in slot 6. */
static int qtok_is_sort_glyph(const Token *tk) {
    return tk->kind == T_VERB && (sym_name_is(tk->k, "<") || sym_name_is(tk->k, ">"));
}

static ray_t *qsql_sortspec(Parser *p) {
    int desc = sym_name_is(cur(p)->k, ">");
    adv(p);
    P key = parse_e(p, Q_NONE);
    if (key.role == R_NONE) q_die("qsql: expected a sort key after < / > in select[...]");
    ray_t *xs[2] = { embed_glyph(desc ? ">" : "<", Q_MONADIC), key.v };
    ray_t *pair = q_list(xs, 2);
    return q_list(&pair, 1);
}

/* parse_query: THE qSQL query-tree parser.  Parses
 *     verb [phrases] [by L] from e [where L]
 * into the functional list (head; t; c=where; b=by; a=phrases), building the
 * c/b/a slots by normalizing raw parse_e phrase lists (qsql_normalize_phrases);
 * a select[…] bracket appends n (slot 5) and the enlisted sort pair (slot 6).
 *
 *   head  `?` (select/exec) | `!` (update/delete)
 *   t     the from-expression (a bare name stays a name-ref)
 *   c     where: `()` when omitted, else enlist(constraint-list)
 *   b     by:    select/update `0b`, exec `()` when omitted, else the by shape
 *   a     phrases: the per-verb select shape (dict / symvec / value)
 *
 * It commits once on the verb keyword: every ordinary-q phrase parses through
 * the QCtx-threaded expression parser, so there is no fallback. */
static P parse_query(Parser *p) {
    ray_t *A = NULL, *B = NULL, *C = NULL, *head = NULL, *node = NULL;

    /* The raw phrase lists and the from-expression live in the q_die leak
     * guard's own slots, taken here (all NULL) so any die between here and the
     * pops frees whatever is live. */
    ray_t **a = pend_push();      /* select phrases */
    ray_t **b = pend_push();      /* by phrases    */
    ray_t **c = pend_push();      /* where phrases */
    ray_t **t = pend_push();      /* from-expr     */
    ray_t **lim = pend_push();    /* select[n] limit expr (slot 5) */
    ray_t **ord = pend_push();    /* select[n;<c] sort spec (slot 6) */

    int verb = qtok_query_verb(cur(p)), start = cur(p)->start;   /* the cursor is on the verb keyword */
    adv(p);

    /* select[n] / select[m n] / select[n;<c] / select[<c] (ref/select.md:104-121):
     * the bracket belongs to the TEMPLATE — limit expr -> slot 5 (0W when order
     * alone), sort spec -> slot 6 — never to the phrase list. */
    if (verb == QSQL_V_SELECT && at(p, T_LBRACK)) {
        adv(p);
        if (!qtok_is_sort_glyph(cur(p))) {
            P le = parse_e(p, Q_NONE);
            if (le.role == R_NONE) q_die("qsql: expected limit expression in select[...]");
            *lim = le.v;
            /* an empty order slot is no order: unparse writes `select[n;]` for `select[n]` (testcases.q:43,63,71) */
            if (at(p, T_SEMI)) {
                adv(p);
                if (!at(p, T_RBRACK)) *ord = qsql_sortspec(p);
            }
        } else {
            *ord = qsql_sortspec(p);
            *lim = ray_i64(INT64_MAX);             /* order alone: n = 0W */
        }
        expect(p, T_RBRACK, "expected ']' closing select[...]");
    }

    /* `select distinct` claims the b slot (basics/funsql.md:161-175) — SELECT
     * only: exec/update/delete keep distinct as an ordinary phrase verb, and
     * `distinct:e` is a column alias, not the modifier */
    int dist = verb == QSQL_V_SELECT && qtok_sym_is(cur(p), "distinct") &&
               !(p->pos + 1 < p->t.n && p->t.t[p->pos + 1].kind == T_VERB &&
                 sym_name_is(p->t.t[p->pos + 1].k, ":"));
    if (dist) adv(p);

    /* select-phrase list: stops at by / from */
    *a = parse_phrase_list(p, Q_SELECT);
    if (q_count(*a) > QSQL_MAXCOLS) die_err(QE_LIMIT);

    /* ref/delete.md:63: a delete phrase list is "a list of column names" —
     * anything else once slipped through qsql_norm_delete_a as a silent skip,
     * leaving an EMPTY name list, which deletes every row */
    if (verb == QSQL_V_DELETE) {
        int64_t dn = q_count(*a);
        ray_t **dp = (ray_t **)ray_data(*a);
        for (int64_t i = 0; i < dn; i++)
            if (!sym_is_nameref(dp[i]) && !keyword_phrase(dp[i])) q_die("qsql: delete takes column names");
    }

    /* optional by-phrase list: stops at from.  delete has NO By clause
     * (ref/delete.md's template omits it; kdb rejects at parse) */
    if (qtok_sym_is(cur(p), "by")) {
        if (verb == QSQL_V_DELETE) q_die("qsql: by is not a delete clause");
        if (dist) q_die("qsql: distinct with by (b is one slot)");
        adv(p);
        *b = parse_phrase_list(p, Q_BY);
        if (q_count(*b) == 0) q_die("qsql: empty by phrase");
        if (q_count(*b) > QSQL_MAXCOLS) die_err(QE_LIMIT);
    }

    /* mandatory from + the from-expression */
    if (!qtok_sym_is(cur(p), "from")) q_die("qsql: expected from");
    adv(p);
    P tp = parse_e(p, Q_FROM);
    if (tp.role == R_NONE) q_die("qsql: expected table after from");
    *t = tp.v;

    /* optional where-phrase list: runs to the statement end */
    if (qtok_sym_is(cur(p), "where")) {
        adv(p);
        *c = parse_phrase_list(p, Q_WHERE);
        if (q_count(*c) == 0) q_die("qsql: empty where phrase");
    }

    /* terminator: only a statement boundary may follow a complete query */
    {
        Token *end = cur(p);
        if (end->kind != T_EOF && end->kind != T_SEMI && end->kind != T_RBRACK &&
            end->kind != T_RPAREN && end->kind != T_RBRACE)
            q_die("qsql: unexpected token after query");
    }

    /* An output alias — written or lent by a bare column name — is a source name,
     * so a reserved one dies like `null:5` instead of silently vanishing from the
     * item list.  Asked while the raw phrases are still the pend guard's, which keeps
     * the "no q_die past this point" invariant below true.  `where` is exempt —
     * it names no column. */
    if (names_have_reserved(*a) || names_have_reserved(*b)) die_err(QE_ASSIGN);
    xyz_unbind(p->xyz, *a);
    xyz_unbind(p->xyz, *b);

    /* ---- normalize the raw phrase lists into the functional slots ----------
     * No q_die past this point, so the raw refs can be released as they are
     * consumed (qsql_norm_delete_a's non-name die is unreachable from here —
     * the delete prevalidation above already excludes that shape; only the
     * normalize probe can reach it, and it pend-holds its list).  A
     * (select-phrase) is verb-shaped by qsql_normalize_phrases
     * (empty select -> `()`, empty delete -> empty symvec, …). */
    A = qsql_normalize_phrases(*a, Q_SELECT, verb);
    ray_release(*a); *a = NULL;

    if (*b) { B = qsql_normalize_phrases(*b, Q_BY, verb); ray_release(*b); *b = NULL; }
    else    { B = dist ? ray_bool(1)
                       : (verb == QSQL_V_EXEC) ? ray_list_new(0) : ray_bool(0); } /* no by */

    if (*c) { C = qsql_normalize_phrases(*c, Q_WHERE, verb); ray_release(*c); *c = NULL; }
    else    { C = ray_list_new(0); }               /* no where -> () */

    /* qsql.md:168: a cols-vs-groups collision "throws a 'dup names for
     * cols/groups error during parse" — AT PARSE, so a script carrying the
     * bug fails to LOAD and the operator is alerted before the code runs.
     * A within-phrase collision is NOT this law's (a derived name auto-aliases,
     * a written duplicate dies at eval — q_funsql.c names_collide). */
    int64_t dup = verb == QSQL_V_SELECT ? qsql_cross_names_dup(A, B) : -1;
    if (dup >= 0) {
        ray_release(A); ray_release(B); ray_release(C);
        char msg[256];
        ray_t *nm = ray_sym_str(dup);
        int n = snprintf(msg, sizeof msg, "dup names for cols/groups %.*s",
                         nm ? (int)ray_str_len(nm) : 0, nm ? ray_str_ptr(nm) : "");
        ray_release(nm);
        die_signal(QE_DUP, msg, n < (int)sizeof msg ? (size_t)n : sizeof msg - 1);
    }

    /* a/b/c are consumed; the from-expr and the limit/order slots move out
     * into the tree below, so take them before the pops free their frames. */
    ray_t *from = *t;
    ray_t *limv = *lim, *ordv = *ord;
    pend_pop(); pend_pop(); pend_pop();
    pend_pop(); pend_pop(); pend_pop();

    /* the VALUE head (value-heads-at-parse): the same immutable registry cell
     * the functional spelling carries — never a bare sym name-ref */
    head = embed_glyph(verb == QSQL_V_SELECT || verb == QSQL_V_EXEC ? "?" : "!", Q_DYADIC);
    node = ray_list_new(limv ? (ordv ? 7 : 6) : 5);
    node = ray_list_append(node, head); ray_release(head);
    node = ray_list_append(node, from); ray_release(from);
    node = ray_list_append(node, C);    ray_release(C);
    node = ray_list_append(node, B);    ray_release(B);
    node = ray_list_append(node, A);    ray_release(A);
    if (limv) { node = ray_list_append(node, limv); ray_release(limv); }
    if (ordv) { node = ray_list_append(node, ordv); ray_release(ordv); }
    pos_note(node, start);
    return (P){ R_NOUN, node, 0, 0, start };
}

/* Query-aware wrapper around parse_base.  Returns EMPTY (without consuming) at
 * a clause boundary / separator so the enclosing parse_e halts.  With
 * ctx == Q_NONE only a query verb is intercepted. */
static P parse_base_q(Parser *p, QCtx ctx) {
    Token *tk = cur(p);
    if (qtok_query_verb(tk) >= 0) {
        /* reserved in every context (`.Q.res`, ref/dotq.md:1911-1916; owner ruling 2026-11-08), unlike `from:` */
        if (assign_target_at(p, p->pos + 1)) die_err(QE_ASSIGN);
        return parse_query(p);
    }
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
            if (t.role == R_VERB) t.v = embed_sym(t.v, Q_DYADIC);
            /* an empty call `f[]` passes identity (ref/identity.md); an elided `;` slot is the projection hole */
            ray_t *w = cons_head(t.v, e, q_count(e) == 1 ? RAY_NULL_OBJ : Q_HOLE_OBJ);
            ray_release(t.v);
            ray_release(e);
            pos_note(w, t.at);
            t.v = w; t.role = R_NOUN; t.glyph = 0;
        } else if (tk->kind == T_ADVERB) {
            /* a glyph verb under an adverb (`+/`) is a bare-verb VALUE:
             * embed its dyadic row (names/lambdas stay for eval to resolve) */
            if (t.role == R_VERB) t.v = embed_sym(t.v, Q_DYADIC);
            ray_t *xs[2] = { tk->k, t.v };
            tk->k = NULL;
            t.v = q_list(xs, 2);
            adv(p);
            t.role = R_VERB;
            t.glyph = 0;
        } else {
            break;
        }
    }
    return t;
}

/* The two charged wrappers: every recursion cycle in the descent crosses
 * parse_E, parse_e or parse_e_from, so charging these three bounds them all.
 * die_err longjmps to the entry point's setjmp, which abandons the whole Parser —
 * an unwound-past decrement is unreachable, so no cleanup handler is needed. */
static P parse_e(Parser *p, QCtx ctx) {
    if (++p->depth > Q_PARSE_MAX_DEPTH) die_err(QE_LIMIT);
    P r = parse_e_body(p, ctx);
    p->depth--;
    return r;
}

static P parse_e_from(Parser *p, P t, QCtx ctx) {
    if (++p->depth > Q_PARSE_MAX_DEPTH) die_err(QE_LIMIT);
    P r = parse_e_from_body(p, t, ctx);
    p->depth--;
    return r;
}

/* (c; expr) for the syntax word c at expression start — a char-atom head, NOT a verb or `.q` entry */
static P char_head_node(Parser *p, QCtx ctx, char c) {
    int start = cur(p)->start;
    adv(p);
    P e = parse_e(p, ctx);
    ray_t *xs[2] = { ray_char(c), (e.role != R_NONE && e.v) ? e.v : RAY_NULL_OBJ };
    ray_t *node = q_list(xs, 2);
    pos_note(node, start);
    return (P){ R_NOUN, node, 0, 0, start };
}

static P parse_e_body(Parser *p, QCtx ctx) {
    Token *st = cur(p);
    TKind nk = peek(p);
    /* Lambda-body early return `:expr` (basics/function-notation.md): a bare
     * `:` at expression START inside a lambda body.  Infix assignment never
     * reaches here with a leading `:` (its lhs noun is consumed first), and
     * `::` is len 2.  Emits (":";expr): kdb display shows lambdas verbatim, so
     * no parse row pins it.  A LONE `:` (next token closes the expression) is
     * the assign-verb OPERAND (`{@[x;1;:;"Z"]}`, ref/amend.md). */
    if (p->lambda_depth > 0 && st->kind == T_VERB && st->len == 1 && p->src[st->start] == ':' &&
        nk != T_SEMI && nk != T_RBRACK && nk != T_RPAREN && nk != T_RBRACE) {
        /* `:[v;p1;r1;…;rd]` is the pattern conditional (basics/pattern.md:42), deferred by owner ruling
         * 2026-11-07: refused, never read as a return of the expression block `[…]`, which answers its last item */
        if (nk == T_LBRACK) die_err(QE_NYI);
        return char_head_node(p, ctx, ':');
    }
    /* Signal `'expr` (ref/signal.md): a bare `'` adverb at expression start
     * that is NOT the compose form `'[f;g]`.  Emits ("'";expr) — a char-atom
     * head distinct from the each-adverb value (f' -> (';`f)); Signal "is
     * part of q syntax ... not an operator", so never a registry row.  With
     * nothing to signal (`(';/;\)`) the `'` is the ITERATOR VALUE instead. */
    if (st->kind == T_ADVERB && st->len == 1 && p->src[st->start] == '\'' &&
        nk != T_LBRACK && nk != T_SEMI && nk != T_RPAREN &&
        nk != T_RBRACK && nk != T_RBRACE && nk != T_EOF)
        return char_head_node(p, ctx, '\'');
    /* A k-unary glyph applied to a juxtaposed operand is k, NOT q — q spells
     * the monadic with its keyword (first, flip, count, …), the glyph wrapped
     * ((,)2, (,/)x) or bracketed (,/[x]).  Owner rulings 2026-07-30 (the colon
     * form `*:1 2 3`) and 2026-08-11 (the bare form `,2`, the derived `,/x`,
     * and verb operands `+ -`): reject at PARSE, so no tree ever exists.  The
     * SHAPE test covers every glyph — the leak was uniform, inherited from the
     * kparser port — never a glyph list.  Excluded by shape: `:`-first
     * spellings (assignment / early return / `::`), digit-colon verbs (`0:`),
     * two-glyph comparisons (`<=`), and every non-operand next token past the
     * adverb run, which keeps `+:` alone, `f:+/`, `,/[x]`, slots, postfix and
     * the parenthesised values intact. */
    if (st->kind == T_VERB && p->src[st->start] != ':') {
        int gf = glyph_form(p->src + st->start, (size_t)st->len);
        if (gf == GF_ONE || gf == GF_MARKED) {
            int j = p->pos + 1;
            while (j < p->t.n && p->t.t[j].kind == T_ADVERB) j++;
            TKind ak = (j < p->t.n) ? p->t.t[j].kind : T_EOF;
            /* kdb-published class: THE GLYPH ITSELF (`$42` -> '$, `+d` -> '+
             * — basics/parsetrees.md "//$ unary form disabled in q") */
            if (ak == T_NOUN || ak == T_LPAREN || ak == T_LBRACE || ak == T_VERB) {
                g_die_at = st->start;
                die_signal(QE_NAME, &p->src[st->start], 1);
            }
        }
    }
    P t = parse_term(p, ctx);
    if (t.role == R_NONE) return EMPTY;
    return parse_e_from(p, t, ctx);
}

/* What stands left of a train tail composes onto it: the node (';f;tail), one per level, right-nested — kdb's
 * `parse "sum abs max::"` is (';sum;(';abs;(::;max))).  A keyword operand is its VALUE, as kdb displays it. */
static P compose_node(ray_t *f, ray_t *tail) {
    ray_t *cv = q_registry_compose_value();
    if (!cv) q_die("compose: registry not initialized");
    ray_retain(cv);
    ray_t *xs[3] = { cv, embed_sym(f, Q_MONADIC), tail };
    return (P){ R_NOUN, q_list(xs, 3), 1, 0, 0 };
}

static int symvec_has(const ray_t *v, int64_t id) {
    for (int64_t i = 0; i < q_count(v); i++) if (ray_read_sym(ray_data((ray_t *)v), i, RAY_SYM, v->attrs) == id) return 1;
    return 0;
}

static int lam_param(const Parser *p, int64_t id) {
    if (p->params) return symvec_has(p->params, id);
    return id == ray_sym_intern_runtime("x", 1) || id == ray_sym_intern_runtime("y", 1) ||
           id == ray_sym_intern_runtime("z", 1);
}

/* `a::1` then `a:1` in one lambda is a global used as a local: 'a at the local's target, at definition
 * (basics/debug.md §Errors, ChangesIn4.0.md).  Assignments are checked in evaluation order, so `a:1;a::2` (the
 * local first) stays legal — `::` on a local writes the local. */
static void global_local_check(Parser *p, int64_t id, ray_t *verb, int at) {
    int local = sym_name_is(verb, ":");
    if (!p->globals || lam_param(p, id) || (!local && !sym_name_is(verb, "::"))) return;
    ray_t *s = ray_sym_str(id);
    if (!s || !ray_str_len(s) || ray_str_ptr(s)[0] == '.') return;   /* dotted names are global by spelling */
    if (symvec_has(*p->globals, ~id)) return;                        /* a local already: `::` writes it */
    if (local && symvec_has(*p->globals, id)) { g_die_at = at; die_signal(QE_NAME, ray_str_ptr(s), ray_str_len(s)); }
    *p->globals = symvec_add(*p->globals, local ? ~id : id);
}

typedef struct { Parser *p; ray_t *verb; int at; } pattern_check_t;

static void pattern_local_check(int64_t id, void *ctx) {
    pattern_check_t *c = ctx;
    global_local_check(c->p, id, c->verb, c->at);
}

/* The parse-time half of the assignment law: the left side must be a target (q_parse_assign_target).  A lambda's
 * `n:` makes n a local, so a reserved spelling
 * is refused here, at definition, as eval refuses it at the call (D500a; a syntax word is a context member only
 * outside a lambda).  An application such as `[`$"test"]` is no target (Dlnkjq).  Consumes all three on a refusal. */
static void assign_check(Parser *p, ray_t *lhs, ray_t *verb, ray_t *rhs, int at) {
    q_assign_target_t k = q_parse_assign_target(lhs);
    int colon = sym_name_is(verb, ":") || verb_marked(verb);
    if (k == Q_TARGET_NONE ||
        (k == Q_TARGET_NAME && colon && p->lambda_depth > 0 && q_registry_is_reserved(lhs->i64))) {
        ray_release(lhs); ray_release(verb); ray_release(rhs);
        die_err(QE_ASSIGN);
    }
    if (k == Q_TARGET_NAME) global_local_check(p, lhs->i64, verb, at);
    else if (sym_name_is(verb, ":") && q_pattern_supported(lhs))
        q_pattern_names(lhs, pattern_local_check, &(pattern_check_t){ p, verb, at });
}

/* `t u e` with u written as a verb: infix application, assignment, or a postfix elision */
static P parse_infix(Parser *p, P t, P u, QCtx ctx) {
    P e = parse_e(p, ctx);
    /* postfix form (`1+`, `-15!`): the missing rhs is the projection HOLE, as a bracket elision's is — an
     * explicit `::` operand is the identity VALUE.  A `g ::` with nothing to assign, spaced or glued, is the train
     * tail (::;g) (funq/ml.q `hdist:sum (<>)::`), a composition keeping g's rank (ref/compose.md:23). */
    if (!e.v && sym_name_is(u.v, "::")) {
        ray_t *xs[2] = { embed_sym(u.v, Q_DYADIC), embed_sym(embed_sym(t.v, Q_MONADIC), Q_DYADIC) };
        ray_t *r = q_list(xs, 2);
        if (r && !RAY_IS_ERR(r)) r->attrs |= Q_ATTR_POSTFIX;
        return (P){ R_NOUN, r, 1, 0, t.at };
    }
    int assign = verb_marked(u.v) || sym_name_is(u.v, ":") || sym_name_is(u.v, "::");
    if (assign) assign_check(p, t.v, u.v, e.v, t.at);
    u.v = embed_sym(u.v, Q_DYADIC);      /* the dyadic row; an `op:` marker selects the monad, kdb's assign-through head */
    /* onto a train tail the infix head is its own postfix elision (`1~` of `1~count@`), composed onto the tail;
     * an assignment (`hdist:sum (<>)::`) binds the train instead */
    int onto = e.train && !assign;
    ray_t *xs[3] = { u.v, t.v, e.v && !onto ? e.v : Q_HOLE_OBJ };
    ray_t *r = q_list(xs, 3);
    if ((!e.v || onto) && !assign && r && !RAY_IS_ERR(r) && q_eval_apply_is_fn(u.v))
        r->attrs |= Q_ATTR_POSTFIX;
    pos_note(r, u.at);
    if (onto) return compose_node(r, e.v);
    return (P){ R_NOUN, r, !e.v, 0, t.at };
}

/* `t e`: t applied to the expression e right of it (juxtaposition) */
static P parse_juxt(P t, P e) {
    /* `:` at prefix position respells to the generic `::` ((::;5) for ":5") —
     * assignment syntax, not a manifest verb, so the k-unary law does not
     * govern it.  Any OTHER bare glyph reaching here bypassed the
     * expression-start check (`count - 5` — the `-` never stands at expression
     * start): same law, same refusal. */
    ray_t *s = t.role == R_VERB ? nameref_str(t.v) : NULL;
    if (s && glyph_form(ray_str_ptr(s), ray_str_len(s)) == GF_ONE) {
        char g = ray_str_ptr(s)[0];
        ray_release(t.v);
        if (g != ':') {
            ray_release(s);
            ray_release(e.v);
            g_die_at = t.at;
            die_signal(QE_NAME, &g, 1);
        }
        t.v = RAY_NULL_OBJ;
    }
    ray_release(s);
    /* A derived FUNCTION has no prefix-juxtaposed form in q, whatever its
     * root — `,/x`, `f'1 2 3`, `{x}/1 2 3` and `2 3\4 5` are the permissive-grammar
     * spellings; q requires the bracket (`f'[x]`, `2 3\[4 5]`) or the parens
     * (`(f')x`).  The upstream kparser kept the cleaner grammar KNOWINGLY and
     * named the stricter q surface as the alternative; the owner ruled for the
     * stricter surface (2026-08-11), and for a DATA root too (2026-11-05,
     * Dlhphb: the derived function has infix syntax, basics/syntax.md:442-446).
     * A node is DERIVED iff its head is one of the six immutable iterator
     * singletons — identity, so a call tree like (enlist;{x}) is never mistaken
     * for one.  Infix (`x f' y`) and bracket forms never carry R_VERB list heads
     * here. */
    if (t.role == R_VERB && t.v && t.v->type == RAY_LIST && q_count(t.v) == 2 &&
        q_registry_iter_of(((ray_t **)ray_data(t.v))[0]) >= 0) {
        ray_release(t.v);
        ray_release(e.v);
        q_die("a derived function applies with brackets or parens in q");
    }
    /* Prefix glyph head embeds its MONADIC registry value */
    if (t.role == R_VERB) t.v = embed_sym(t.v, Q_MONADIC);
    /* JUXTAPOSED paren-glyph on a DATA operand takes its unary meaning —
     * `(,)2` is `,2`, `(-)5` is `-5` (owner ruling 2026-07-23), `(!)10` is
     * `til 10` and `(@)t` is `type t`; bracket-apply `+[10]` stays a projection
     * (assign/apply-forms contract, ref/value.md `value +[2]`) and a fn-value
     * operand keeps the dyad (`(+)(*)` composes below).  The
     * MANIFEST decides: any dyadic row with a monadic cell has a unary form,
     * whatever shape that cell's value takes (`%:` is a q.q projection). */
    if (t.glyph && (t.v->type == RAY_BINARY || t.v->type == RAY_VARY) &&
        e.role == R_NOUN && e.v &&
        !(e.v->type == RAY_UNARY || e.v->type == RAY_BINARY ||
          e.v->type == RAY_VARY)) {
        const q_op_t *drow = q_registry_row_of(t.v, Q_DYADIC);
        if (drow && drow->name) {
            ray_t *mv = q_registry_lookup_name(drow->name, strlen(drow->name),
                                               Q_MONADIC);
            if (mv && q_eval_apply_is_fn(mv)) {
                ray_retain(mv);
                ray_release(t.v);
                t.v = mv;
            }
        }
    }
    /* two juxtaposed WRITTEN glyphs compose too (`(+)(*)`, owner ruling 2026-11-06); `count distinct` applies */
    if (e.train || (t.glyph && e.glyph)) return compose_node(t.v, e.v);
    ray_t *xs[2] = { t.v, e.v };
    ray_t *node = q_list(xs, 2);
    pos_note(node, t.at);
    return (P){ R_NOUN, node, 0, 0, t.at };
}

static P parse_e_from_body(Parser *p, P t, QCtx ctx) {
    P u = parse_term(p, ctx);

    if (u.role == R_NONE) {
        /* a LONE glyph is the bare-verb VALUE: the dyadic row, a suffixed-colon marker (`#:`) selecting the monad, a
         * bare `:` the assign verb (`@[x;i;:;y]`; the assignment syntax sym never stands alone) and `::` the identity
         * value.  Every expression end — `(+)`, slots, statements, lambda bodies, an assignment rhs — arrives here, so
         * this is the ONE site beside the bracket head and adverb root.  Written as a verb, the value is a train tail
         * (`0|+`, `-2_(1_)\`); a lambda or a prefix keyword there is a noun. */
        if (t.role == R_VERB && sym_glyph_form(t.v) != GF_NONE) {
            int assign = sym_name_is(t.v, ":");
            ray_t *v = embed_sym(t.v, Q_DYADIC);
            return (P){ R_NOUN, v, !assign && v->type != -RAY_SYM && v != RAY_NULL_OBJ, 0, t.at };
        }
        if (t.role == R_VERB && t.v && t.v->type == RAY_LIST) t.train = 1;
        /* an infix name standing last is written as a verb too, so `-8!union` composes as `-8!~` does */
        Token *last = &p->t.t[p->pos - 1];
        if (t.role == R_NOUN && last->kind == T_NOUN && q_registry_is_infix(p->src + last->start, (size_t)last->len))
            t.train = 1;
        return t;
    }

    if (t.role == R_NOUN && u.role == R_VERB) return parse_infix(p, t, u, ctx);
    return parse_juxt(t, parse_e_from(p, u, ctx));
}

/* ===== qSQL phrase normalization ==============================================
 * parse_query parses each clause through the expression parser (parse_e with a
 * QCtx), which emits RAW phrase trees — `(:;name;expr)` alias nodes, bare
 * name-refs and constraint exprs.  qsql_normalize_phrases turns a raw phrase
 * list into kdb's functional slot:
 *
 *   c   where-constraints: () when none, else enlist(constraint-list) where
 *       each constraint is a parse tree (op;`col;arg) — so one constraint
 *       displays ,,(op;…) and two ,((…);(…)), matching the docs' vertical form.
 *   b   by-clause: 0b when none, else a name!expr dict.
 *   a   select-columns: () when none, else a name!expr dict.
 *
 * Inside clause expressions a bare identifier STANDING ALONE is a COLUMN
 * reference (a symbol literal `col`); a keyword is already its value, wherever
 * it stands; a `sym literal is a VALUE so it is enlisted (,`s); numbers stay
 * as-is. */

/* the glyphs whose LEFT operand is a parameter and RIGHT the data: an unnamed
 * phrase under one names by the data, so `update `g#sym`, `update `dom$col`
 * and `update 0^size` land on the column itself (kb/performance-tips.md:283,
 * learn/brief-introduction.md:381, core/kdb/taq/taq.q:65) */
static int qsql_glyph_names_right(const q_op_t *row) {
    return row->name[1] == 0 && strchr("#_^$@.", row->name[0]) != NULL;
}

/* The derived output name of an unnamed phrase as a sym id, or -1 (-> x).
 * The head's manifest row picks the naming ARGUMENT: a glyph (or the raw
 * paren list a table literal holds, select's `,`) its first
 * (qsql.md:234 "leftmost term"), a keyword or any other fn value its last
 * (xbar.md:33 `10 xbar time.minute` -> minute; tpcd.q:12 `q wsum x` read
 * back as x).  The virtual `i` never names a result (qsql.md:234). */
static int64_t qsql_derive_alias(ray_t *expr) {
    if (!expr) return -1;
    if (expr->type == -RAY_SYM) {
        int64_t id = expr->i64;
        ray_t *s = ray_sym_str(id);
        if (!s) return id;
        const char *p = ray_str_ptr(s);
        size_t n = ray_str_len(s), cut = 0;
        for (size_t i = 0; i < n; i++)
            if (p[i] == '.') cut = i + 1;
        if (cut > 0 && cut < n) { p += cut; n -= cut; }
        id = (n == 1 && p[0] == 'i') ? -1 : ray_sym_intern_runtime(p, (int64_t)n);
        ray_release(s);
        return id;
    }
    ray_t *bound = NULL;
    if (binding_node(expr, &bound, NULL)) return qsql_derive_alias(bound);   /* `g#sym:… is sym (Dyvqvc) */
    int k = alias_arg(expr);
    return k < 0 ? -1 : qsql_derive_alias(((ray_t **)ray_data(expr))[k]);
}

/* expr with the binding on its naming path replaced by the bound value: `g#sym:… names the column sym and assigns
 * nothing (Dyvqvc).  Borrowed in, owned out (expr itself, retained, when there is none). */
static ray_t *alias_unbind(ray_t *expr) {
    ray_t *val = NULL;
    if (binding_node(expr, NULL, &val)) return alias_unbind(val);
    int k = alias_arg(expr);
    ray_t *in = k < 0 ? NULL : ((ray_t **)ray_data(expr))[k], *out = in ? alias_unbind(in) : NULL;
    if (out == in) {
        ray_release(out);
        ray_retain(expr);
        return expr;
    }
    ray_t *w = ray_list_new(q_count(expr));
    for (int64_t i = 0; i < q_count(expr); i++) w = ray_list_append(w, i == k ? out : ((ray_t **)ray_data(expr))[i]);
    ray_release(out);
    return w;
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

/* The exec By-phrase VALUE.  One bare column is the quoted symbol (parsetrees.md); anything else, several bare
 * columns included, is the name!expr dict Select uses, so `exec … by a,b` keys by a table (SO 49112236) where a
 * functional sym VECTOR b would answer funsql.md:348's one empty-symbol group.  Consumes bk/bv; OWNED result. */
static ray_t *qsql_exec_by(ray_t **bk, ray_t **bv, const int *bnamed, int nb) {
    if (nb == 1 && !bnamed[0] && bv[0] && bv[0]->type == -RAY_SYM && (bv[0]->attrs & Q_ATTR_QUOTED)) {
        ray_t *b = symvec_add(ray_sym_vec_new(RAY_SYM_W64, 1), bv[0]->i64);
        ray_release(bk[0]);
        ray_release(bv[0]);
        return b;
    }
    return qsql_build_dict(bk, bv, nb);              /* consumes bk/bv */
}

static ray_t *qsql_convert_expr(ray_t *x);

/* An APPLIED phrase head: a keyword arrives as its value (the head law, parse_base), a nested-list head is
 * converted recursively, and a USER name stays a name-ref: its meaning follows qsql.md:121-127's eval-time order
 * (column, then enclosing local, then global — funsql.md:706 keeps variables as symbols in the tree), which a
 * parse-time env snapshot cannot honour.  Returns an OWNED value. */
static ray_t *qsql_convert_head(ray_t *h) {
    if (h && h->type == RAY_LIST) return qsql_convert_expr(h);
    if (h) ray_retain(h);
    return h;
}

/* Rewrite ONE raw phrase expr into its functional leaf representation:
 *   - bare name-ref standing alone -> column symbol;
 *   - `sym scalar literal -> the enlisted 1-element sym VECTOR `,`sym`;
 *   - a paren `(e1;e2;…)` list keeps its top-level `(enlist;e1;…)` tree (parsetrees.md:107-113);
 *   - an application `(head; args…)` -> converted head + column-ised args.
 * Returns an OWNED tree; does NOT consume x. */
static ray_t *qsql_convert_expr(ray_t *x) {
    if (!x) return RAY_NULL_OBJ;
    if (x->type == -RAY_SYM) {
        /* `sym constants arrive pre-wrapped (,`sym) from noun_tree_value and
         * pass through the tail below; a sym ATOM here is always a name-ref. */
        return symlit_id(x->i64);
    }
    if (x->type == RAY_LIST) {
        int64_t n = q_count(x);
        ray_t **e = (ray_t **)ray_data(x);
        /* An ENLISTED symvec constant (the parser's ,`a`b wrap) passes through
         * whole: kdb keeps it enlisted (funsql.md:69 `c1 in `b`c` =
         * (in;`c1;enlist[`b`c])) and one phrase eval yields the symvec. */
        if (n == 1 && e[0] && e[0]->type == RAY_SYM) {
            ray_retain(x);
            return x;
        }
        ray_t *node = ray_list_new(n > 0 ? n : 1);
        if (n >= 1) {                    /* application; a paren literal keeps its enlist head */
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

/* a derived name in use suffixes 1,2,… (qsql.md:234 `c1 c11`) — never dropped */
static ray_t *qsql_alias_dedup(int64_t id, ray_t **aliases, int na) {
    int64_t used[QSQL_MAXCOLS];
    for (int j = 0; j < na; j++) used[j] = aliases[j]->i64;
    if (id < 0) id = ray_sym_intern_runtime("x", 1);
    return symlit_id(q_name_dedup(id, used, na, 0));
}

/* A phrase list's columns: an alias phrase keys on its written name (*named
 * set); a bare phrase derives its output name via qsql_derive_alias over the
 * converted phrase.  Fills the OWNED aliases/vals; answers the count. */
static int qsql_phrase_cols(ray_t *phrases, ray_t **aliases, ray_t **vals, int *named) {
    int64_t n = q_count(phrases);
    ray_t **ph = (ray_t **)ray_data(phrases);
    int na = 0;
    for (int64_t i = 0; i < n && na < QSQL_MAXCOLS; i++) {
        ray_t *name = NULL, *val = NULL;
        named[na] = binding_node(ph[i], &name, &val);
        if (named[na]) {
            aliases[na] = symlit_id(name->i64);
            vals[na]    = qsql_convert_expr(val);
        } else {
            ray_t *raw  = alias_unbind(ph[i]), *b = alias_binding(ph[i]);
            ray_t *v    = qsql_convert_expr(raw);
            aliases[na] = qsql_alias_dedup(qsql_derive_alias(b ? b : v), aliases, na);
            vals[na]    = v;
            ray_release(raw);
        }
        na++;
    }
    return na;
}

/* select/update `a`, select/update by-key `b`: a name!expr DICT */
static ray_t *qsql_norm_dict(ray_t *phrases) {
    ray_t *aliases[QSQL_MAXCOLS], *vals[QSQL_MAXCOLS];
    int named[QSQL_MAXCOLS];
    int na = qsql_phrase_cols(phrases, aliases, vals, named);
    return qsql_build_dict(aliases, vals, na);       /* consumes aliases/vals */
}

/* exec select-phrase `a`: omitted -> `()`; a single unnamed column -> its tree
 * enlisted in a 1-item general list, a bare name included (unparse.q:28-29,
 * testcases.q:47); named / multiple -> a name!expr dict. */
static ray_t *qsql_norm_exec_a(ray_t *phrases) {
    int64_t n = q_count(phrases);
    ray_t **ph = (ray_t **)ray_data(phrases);
    if (n == 0) return ray_list_new(0);
    ray_t *name = NULL, *val = NULL;
    if (n == 1 && !binding_node(ph[0], &name, &val)) {
        ray_t *v = qsql_convert_expr(ph[0]);
        if (!v || RAY_IS_ERR(v)) return v;
        return q_list(&v, 1);
    }
    return qsql_norm_dict(phrases);                  /* named / multiple -> dict */
}

/* by-phrase `b`: select/update use the name!expr dict; exec uses the bare By-symbol for one bare column, else the
 * same dict, via qsql_exec_by. */
static ray_t *qsql_norm_by(ray_t *phrases, int verb) {
    if (verb != QSQL_V_EXEC) return qsql_norm_dict(phrases);
    int64_t n = q_count(phrases);
    ray_t **ph = (ray_t **)ray_data(phrases);
    /* `by 0b` is already a functional b-value: pass it through so exec takes the
     * select (table) shape — ref/exec.md:96 */
    if (n == 1 && ph[0] && ph[0]->type == -RAY_BOOL) {
        ray_retain(ph[0]);
        return ph[0];
    }
    ray_t *bk[QSQL_MAXCOLS], *bv[QSQL_MAXCOLS];
    int bnamed[QSQL_MAXCOLS];
    int nb = qsql_phrase_cols(phrases, bk, bv, bnamed);
    return qsql_exec_by(bk, bv, bnamed, nb);         /* consumes bk/bv */
}

/* where-phrase `c`: the converted constraint exprs, enlisted */
static ray_t *qsql_norm_where(ray_t *phrases) {
    int64_t n = q_count(phrases);
    ray_t **ph = (ray_t **)ray_data(phrases);
    ray_t *clist = ray_list_new(n > 0 ? n : 1);
    for (int64_t i = 0; i < n; i++) {
        ray_t *c = qsql_convert_expr(ph[i]);
        clist = ray_list_append(clist, c); ray_release(c);
    }
    return q_list(&clist, 1);
}

/* delete column-list `a`: the col names as a symbol VECTOR, ENLISTED like every
 * symvec constant in a tree (parsetrees.md "lists of symbols are enlisted" —
 * `,,`b`), so one eval of the slot yields the functional symvec.  A bare 1-elem
 * symvec would instead eval to the ATOM (parsetrees.md:26 eval enlist`x -> `x). */
static ray_t *qsql_norm_delete_a(ray_t *phrases) {
    int64_t n = q_count(phrases);
    ray_t **ph = (ray_t **)ray_data(phrases);
    ray_t *a = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    for (int64_t i = 0; i < n; i++) {
        ray_t *x = ph[i];
        /* parse_query pre-validates; a skip here would silently empty the name
         * list, which deletes every row */
        if (!x || x->type != -RAY_SYM) {
            ray_release(a);
            q_die("qsql: delete takes column names");
        }
        a = symvec_add(a, x->i64);
    }
    return q_list(&a, 1);
}

/* Normalize a raw phrase list into its functional slot.
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
        if (q_count(phrase_list) == 0) return ray_list_new(0);   /* select () */
        return qsql_norm_dict(phrase_list);
    }
}

/* THE clause-fragment refusal (owner ruling 2026-08-27): the comma stays the
 * phrase separator, so in `a,\:2` the fragment after the split BEGINS with a
 * bare adverb — no legitimate clause phrase does, and every silent-drop in the
 * class started here.  Refuse at the split, in the one home all six clause
 * positions ride (select/exec/update/delete phrases, by, where), rather than
 * letting the fragment reach a normalizer that discards it.  The one adverb-led
 * primary term, compose `'[f;g;…]`, stays a phrase. */
static void phrase_head_guard(Parser *p) {
    Token *tk = cur(p);
    if (tk->kind == T_ADVERB &&
        !(tk->len == 1 && p->src[tk->start] == '\'' && peek(p) == T_LBRACK))
        q_die("qsql: clause phrase begins with an adverb");
}

/* Parse one comma-separated qSQL clause into a RAY_LIST of raw phrase trees via
 * the real expression parser, halting at the clause boundary (parse_base_q
 * returns EMPTY at a section keyword / the join comma).  A separator comma must
 * be FOLLOWED by a phrase — a trailing `select a, from t` dies rather than
 * minting a phantom `::` phrase.  Refcount: ray_list_append RETAINS, so the
 * local ref is released in both branches.  Returns an OWNED list ( `()` when
 * the clause is empty). */
static ray_t *parse_phrase_list(Parser *p, QCtx ctx) {
    /* A parse_e under a non-Q_NONE ctx can q_die (a misplaced clause keyword in
     * parse_base_q); hold the partial list where the longjmp handler frees it. */
    ray_t **lst = pend_push();
    *lst = ray_list_new(0);
    ray_t **globals = p->globals;   /* a phrase's `n:` labels a column: no assignment for the global-as-local check */
    p->globals = NULL;
    phrase_head_guard(p);
    P first = parse_e(p, ctx);
    if (first.role != R_NONE) {
        *lst = ray_list_append(*lst, first.v); ray_release(first.v);
        while (qtok_is_join_comma(cur(p))) {
            adv(p);
            phrase_head_guard(p);
            P f = parse_e(p, ctx);
            if (f.role == R_NONE) q_die("qsql: empty phrase after ','");
            *lst = ray_list_append(*lst, f.v);
            ray_release(f.v);
        }
    }
    p->globals = globals;
    ray_t *done = *lst;
    pend_pop();                                 /* success: unlink, keep it */
    return done;
}

/* ---- entry-point plumbing ----------------------------------------------------
 * Every entry point opens with parse_begin, then its own setjmp, whose handler
 * answers die_answer(): the parked refs released, the live token buffer freed,
 * the staged class (and payload) answered as the error. */
static void parse_begin(void) {
    init_class();
    g_die_at = -1;
    g_toks.t = NULL;
    g_toks.n = 0;
}

static void drop_tokens(void) {
    free_tokens(g_toks);
    g_toks.t = NULL;
    g_toks.n = 0;
    pos_drop();
}

static ray_t *die_answer(void) {
    pend_unwind();
    drop_tokens();
    q_err_e cls = g_die_class; g_die_class = QE_PARSE;
    if (g_die_payload) {
        ray_t *e = q_err_signal(cls, g_die_payload);
        ray_release(g_die_payload); g_die_payload = NULL;
        return e;
    }
    return q_err(cls);
}

/* ---- test hook --------------------------------------------------------------
 * Scan `src` as a bare clause phrase list in `ctx`, then normalize it to the
 * `verb` slot shape, so test/q_qsql_normalize.c can drive the parse_e path over
 * phrase substrings.  Not on any runtime path.  Returns OWNED. */
ray_t *q_qsql_normalize_probe(const char *src, int ctx, int verb) {
    if (!q_registry_ready())
        return q_err(QE_INIT);
    parse_begin();
    if (setjmp(q_err_jmp)) return die_answer();
    Parser p = { .src = src, .t = scan(src) };
    /* pend-held across normalize: qsql_norm_delete_a can q_die on a non-name
     * phrase (parse_query prevalidates; this probe does not) */
    ray_t **ph = pend_push();
    *ph = parse_phrase_list(&p, (QCtx)ctx);
    ray_t *slot = qsql_normalize_phrases(*ph, (QCtx)ctx, verb);
    ray_release(*ph);
    pend_pop();
    drop_tokens();
    return slot;
}

static ray_t *parse_E(Parser *p, QCtx ctx) {
    /* Appended one at a time, so a `;` list has no length cap.  ray_list_append stores a C
     * NULL without retaining, so the list PRESERVES the C-NULL slots (unlike
     * q_list, which normalises them to the generic null).  An empty statement — a
     * whole-line comment, the `;;` between two expressions — is such a NULL;
     * seq_of and the evaluator read it as a no-op that yields no output, where
     * the generic null would print `::`. */
    if (++p->depth > Q_PARSE_MAX_DEPTH) die_err(QE_LIMIT);
    ray_t **l = pend_push();
    *l = ray_list_new(1);
    for (;;) {
        ray_t *v = parse_e(p, Q_NONE).v;
        *l = ray_list_append(*l, v);
        ray_release(v);
        if (!at(p, T_SEMI)) break;
        adv(p);
    }
    ray_t *done = *l;
    pend_pop();
    p->depth--;
    return done;
}

/* ===== text intake =========================================================
 *
 * THE one home for what a BUFFER of q text means before it is q source: the
 * `<letter>)` handler prefix (ref/parse.md:54-56 — `parse "k)!10"`) and the
 * leading `\` console command.  Both answer with an ordinary application
 * tree, so every door that takes text (repl, `value`, `parse`, the wire) sees
 * the same thing from the same bytes. */

/* `(name; text)` — a keyword head (`system`, `value`) is its value, as anywhere in a tree; a handler head is a NAME
 * ref, so an undefined one fails as ordinary resolution (`'.g.e`).  Consumes `arg`. */
static ray_t *intake_tree(const char *name, size_t nl, ray_t *arg) {
    ray_t *xs[2] = { embed_sym(i_name(name, (int)nl), Q_MONADIC), arg };
    return q_list(xs, 2);
}

static ray_t *parse_text(const char *src, ray_t **pos, int shift);

/* k texts a client or library is witnessed sending, byte for byte bar a trailing comment and blanks, beside their q
 * transliteration — qStudio's Server Properties (qstudio-private ServerSlashConfig.java:36, ServerReport.java:54-63,74),
 * embedPy's compose helpers (KxSystems/embedPy p.q:23-24) and kdb-q-ext-vscode's corpus (test/fixtures/corpus.q:316,
 * its kdb value in corpus.ipc.json:279).  Not a k evaluator: an entry needs a witness of the text (the real k mode is
 * Dirscx). */
static const struct { const char *k, *q; } k_known[] = {
    { ".:'[\"\\\\\",/:\"cCegopPstTWz\"]",
      "value each \"\\\\\",/:\"cCegopPstTWz\"" },
    { "({$[min `PV`PD`D`pf in !: `.Q; ([] desc:! b; val:. b:(`Segments`Partitions`PartitionType)!(#.Q.D;#.Q.PD;.Q.pf)); "
      "([] Data:,\"Not partitioned or segmented\")]}[];+:{,x}'.Q.w[];"
      ".:'[$`.z.a`.z.h`.z.i`.z.k`.z.K`.z.l`.z.o`.z.u`.z.x,`$\"\\\\s\"])",
      "({$[min `PV`PD`D`pf in key `.Q; flip `desc`val!(key b; value b:(`Segments`Partitions`PartitionType)!"
      "(count .Q.D;count .Q.PD;.Q.pf)); ([] Data:enlist \"Not partitioned or segmented\")]}[]; "
      "flip {enlist x} each .Q.w[]; value each string `.z.a`.z.h`.z.i`.z.k`.z.K`.z.l`.z.o`.z.u`.z.x,`$\"\\\\s\")" },
    { "c:{'[y;x]}/|:",         "c:{{'[y;x]}/[reverse x]}" },
    { "ce:{'[y;x]}/enlist,|:", "ce:{{'[y;x]}/[enlist,reverse x]}" },
    { ".corpus.k:+/1 2 3",     ".corpus.k:(+/)1 2 3" },
};

/* The text before a trailing comment (a `/` after a blank, outside a string, as the scanner reads one), blanks trimmed.
 * A comment with code on a later line is not trailing: the whole text is kept, so it matches no entry. */
static size_t k_code_len(const char *p, size_t n) {
    size_t m = n;
    int str = 0;
    init_class();
    for (size_t i = 0; i < n; i++) {
        if (str) { if (p[i] == '\\') i++; else if (p[i] == '"') str = 0; }
        else if (p[i] == '"') str = 1;
        else if (p[i] == '/' && i && (CLASS[(uint8_t)p[i - 1]] & CL_WS)) {
            size_t j = i;
            while (j < n && p[j] != '\n') j++;
            while (j < n && (CLASS[(uint8_t)p[j]] & CL_WS)) j++;
            if (j == n) m = i;
            break;
        }
    }
    while (m && (CLASS[(uint8_t)p[m - 1]] & CL_WS)) m--;
    return m;
}

ray_t *q_parse_lang_tree(char letter, const char *p, int64_t n) {
    char nm[5] = { '.', letter, '.', 'e', '\0' };
    if (letter == 'q') return intake_tree("value", 5, ray_charv(p, n));
    if (letter == 'k') {
        size_t m = k_code_len(p, (size_t)n);
        for (size_t i = 0; i < sizeof k_known / sizeof k_known[0]; i++)
            if (strlen(k_known[i].k) == m && !memcmp(k_known[i].k, p, m)) return parse_text(k_known[i].q, NULL, 0);
    }
    if (letter == 's') {
        parse_begin();
        if (setjmp(q_err_jmp)) return die_answer();
        return intake_tree(nm, 4, text_capture(ray_charv(p, n), NULL));
    }
    return intake_tree(nm, 4, ray_charv(p, n));
}

/* ===== public entry ========================================================== */

static const char *intake_lang(const char *src, size_t *n, char *lang) {
    *lang = 0;
    while (*n >= 2 && isalpha((unsigned char)src[0]) && src[1] == ')' && !(*n >= 3 && src[2] == ')')) {
        *lang = src[0];
        src += 2;
        *n -= 2;
    }
    return src;
}

static const char *intake_sys(const char *src) {
    while (*src == ' ' || *src == '\t') src++;
    return *src == '\\' ? src : NULL;
}

ray_t *q_parse(const char *src) { return q_parse_at(src, NULL); }

ray_t *q_parse_at(const char *text, ray_t **pos) {
    const char *src = text;
    if (pos) *pos = NULL;
    /* Value embedding requires a live registry (codex #1): fail fast rather
     * than silently emit a mixed sym/value tree.  Every q entry point
     * bootstraps via q_runtime_create, which initializes the registry. */
    if (!q_registry_ready())
        return q_err(QE_INIT);
    /* Text intake, read ONCE at the START of the buffer — so a `q)` or `\` on a
     * LATER line of multi-line text is q source and fails as such (owner ruling
     * 2026-09-04; test/qscript/ipc/backslash_load_over_ipc pins it over the
     * wire).  Every leading `<letter>)` is stripped and the RIGHTMOST letter is
     * the language (`q))` is the debug prompt and never matches); a letter other
     * than `q` routes the rest to `.X.e`, and a BARE prefix carries no text to
     * hand anyone, so it is the empty statement.  A leading `\` (blanks skipped,
     * as the console has always allowed) is `system "…"` — the same tree
     * `parse "system \"p\""` builds, so `\X` and `system"X"` are one path
     * (ref/system.md). */
    {
        size_t n = strlen(src);
        char lang;
        src = intake_lang(src, &n, &lang);
        if (n && lang && lang != 'q') return q_parse_lang_tree(lang, src, (int64_t)n);
        const char* b = intake_sys(src);
        if (b) {
            const char* r = b + 1;
            int64_t rn = (int64_t)n - (r - src);
            /* the q string-literal shape: one char is an ATOM (string-C3) */
            return intake_tree("system", 6,
                               rn == 1 ? ray_char((uint8_t)r[0]) : ray_charv(r, rn));
        }
    }
    return parse_text(src, pos, (int)(src - text));
}

/* shift: how far the intake moved src past the caller's text, so the statement's caret offsets index THAT text */
static Parser *g_parser;   /* parse_text's, while it runs: the token a die stopped at */

static ray_t *parse_text(const char *src, ray_t **pos, int shift) {
    parse_begin();
    g_parser = NULL;
    if (setjmp(q_err_jmp)) {
        if (g_die_at < 0 && g_parser) {   /* unwitnessed column: the first token the parser could not take */
            int k = g_parser->pos < g_parser->t.n ? g_parser->pos : g_parser->t.n - 1;
            int n = (int)strlen(src);
            g_die_at = k >= 0 && g_parser->t.t[k].start < n ? g_parser->t.t[k].start : n - 1;
        }
        g_parser = NULL;
        int64_t die[2] = { 0, g_die_at + shift };   /* node NULL: the die token, frame [0]'s caret */
        if (pos && g_die_at >= 0) *pos = ray_vec_from_raw(RAY_I64, die, 2);
        return die_answer();
    }
    Parser p = { .src = src, .t = scan(src) };
    g_parser = &p;
    ray_t *e = parse_E(&p, Q_NONE);
    g_parser = NULL;
    int whole = at(&p, T_EOF);
    int stop = !whole && p.pos < p.t.n ? p.t.t[p.pos].start : -1;   /* the token after the whole expression */
    if (pos && whole) *pos = pos_take(0, -shift);
    drop_tokens();
    if (!whole) {
        int64_t die[2] = { 0, stop + shift };
        if (pos && stop >= 0) *pos = ray_vec_from_raw(RAY_I64, die, 2);
        ray_release(e);
        return q_err(QE_PARSE);
    }
    /* An empty program (a whole-line comment, or blank/whitespace-only input)
     * collapses to a C NULL in seq_of.  Return the value-null singleton
     * instead so callers get an explicit no-op sentinel: RAY_IS_NULL()
     * recognises it (the REPL prints nothing; qdoc matches empty output) and
     * ray_eval() self-evaluates it, rather than every caller having to treat a
     * bare C NULL specially. */
    ray_t *prog = seq_of(e);
    return prog ? prog : RAY_NULL_OBJ;
}

static ray_t *tokens_push(ray_t *l, const char *s, int a, int b) {
    ray_t *t = ray_charv(s + a, b - a);
    l = ray_list_append(l, t);
    ray_release(t);
    return l;
}

/* A gap between scanned tokens holds only blanks and comments; a comment takes the blank run before it. */
static ray_t *tokens_gap(ray_t *l, const char *s, int a, int b) {
    while (a < b) {
        int e = a;
        while (e < b && (CLASS[(uint8_t)s[e]] & CL_WS)) e++;
        if (e < b) while (e < b && s[e] != '\n') e++;
        l = tokens_push(l, s, a, e);
        a = e;
    }
    return l;
}

ray_t *q_parse_tokens(const char *src, int64_t n) {
    if (n > INT_MAX - 1) return q_err(QE_DOMAIN);
    if (!q_registry_ready()) return q_err(QE_INIT);
    parse_begin();
    if (setjmp(q_err_jmp)) return die_answer();
    Tokens ts = scan(src);
    ray_t *l = ray_list_new(ts.n > 1 ? ts.n * 2 : 1);
    int at = 0;
    for (int i = 0; i < ts.n - 1; i++) {
        l = tokens_gap(l, src, at, ts.t[i].start);
        int a = ts.t[i].start, b = a + ts.t[i].len;
        /* kdb tokenises a numeric strand item by item, its blanks between (Dbzjyl; kdb-q-ext-vscode lexing.json) */
        int strand = ts.t[i].kind == T_NOUN && src[a] != '"' && src[a] != '`';
        for (int e = a; strand && e < b; e++)
            if (CLASS[(uint8_t)src[e]] & CL_WS) {
                int w = e;
                while (CLASS[(uint8_t)src[w]] & CL_WS) w++;
                l = tokens_push(tokens_push(l, src, a, e), src, e, w);
                a = e = w;
            }
        l = tokens_push(l, src, a, b);
        at = b;
    }
    l = tokens_gap(l, src, at, (int)n);
    drop_tokens();
    return l;
}

static void open_scan(q_parse_open_t *st, const char *src) {
    parse_begin();
    g_scan_open_str = 0;
    g_scan_open_lit = 0;
    if (setjmp(q_err_jmp)) {
        ray_error_free(die_answer());
    } else {
        scan(src);
        drop_tokens();
    }
    st->in_string = g_scan_open_str;
    st->in_literal = g_scan_open_lit;
}

void q_parse_open_feed(q_parse_open_t *st, const char *piece) {
    if (st->opaque) return;
    if (!st->fed) {
        st->fed = 1;
        size_t n = strlen(piece);
        char   lang;
        piece = intake_lang(piece, &n, &lang);
        if ((n && lang && lang != 'q') || intake_sys(piece)) { st->opaque = 1; return; }
    }
    if (!q_registry_ready()) return;
    if (!st->in_string && !st->in_literal) { open_scan(st, piece); return; }
    size_t n = strlen(piece), fence = (size_t)st->in_literal;   /* the piece starts inside the literal: reopen it for the scanner */
    char  *quoted = malloc(n + fence + 5);
    if (!quoted) return;
    size_t k = 0;
    if (fence) { memset(quoted, '`', fence); k = fence; memcpy(quoted + k, "raw ", 4); k += 4; }
    else quoted[k++] = '"';
    memcpy(quoted + k, piece, n + 1);
    open_scan(st, quoted);
    free(quoted);
}

/* q_parse_is_assign — see q_parse.h.  An assignment head (q_parse_assign_head) with a name/indexed-name target or a
 * `:` pattern; a `;` statement sequence (char head) asks its last statement. */
int q_parse_is_assign(const ray_t *cast) {
    ray_t *ast = (ray_t *)cast;   /* read-only walk; ray_data lacks a const view */
    if (!ast || ast->type != RAY_LIST || q_count(ast) < 1) return 0;
    ray_t **e = (ray_t **)ray_data(ast);
    ray_t *h = e[0];
    if (q_parse_is_seq_head(h)) {
        int64_t n = q_count(ast);
        return n >= 2 ? q_parse_is_assign(e[n - 1]) : 0;
    }
    q_assign_head_t asg = q_count(ast) == 3 ? q_parse_assign_head(h, NULL, NULL) : Q_HEAD_NONE;
    /* every target is silent, an indexed `name[i;…]:v` and a `:` pattern too — kdb console (basics/pattern.md:14-20) */
    q_assign_target_t k = asg ? q_parse_assign_target(e[1]) : Q_TARGET_NONE;
    return k == Q_TARGET_NAME || k == Q_TARGET_INDEX || (k == Q_TARGET_PATTERN && asg != Q_HEAD_GLOBAL);
}
