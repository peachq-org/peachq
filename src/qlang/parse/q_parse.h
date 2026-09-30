/* q_parse — parse one line of q source into a rayforce ray_t AST.
 *
 * Ported from kparser (Apache-2.0): the recursive-descent scanner/parser
 * control flow is kept; the value layer emits ray_t instead of K structs.
 */
#ifndef Q_PARSE_H
#define Q_PARSE_H

#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>

/* Parse q source into a ray_t AST tree.
 *
 * Returns a ray_t owned by the caller (release with ray_release), or a
 * RAY_ERROR on malformed input.  A statement sequence of one collapses to
 * that single expression; two or more become a (`;; ...) list.
 *
 * Requires an initialised rayforce runtime (symbol interning). */
ray_t* q_parse(const char* src);

/* `(.X.e; "text")` application tree (`q` -> `value`) — the language-handler
 * dispatch q_parse applies to a `<letter>)` prefix, shared with the char-atom
 * apply arm (`"g" "4"`). */
ray_t* q_parse_lang_tree(char letter, const char* p, int64_t n);

/* Test probe (test/q_qsql_normalize.c): scan `src` as a bare clause phrase in
 * scan-context `ctx`, normalized to the `verb` slot shape.  Not on any runtime
 * path.  Returns OWNED. */
ray_t* q_qsql_normalize_probe(const char* src, int ctx, int verb);

/* True iff the statement's RESULT is an assignment's — the q console prints
 * nothing for `a:5` (statement sequences check their LAST statement).
 * Consulted by the REPL/qdoc/remote-eval before printing. */
int q_parse_is_assign(const ray_t* ast);

/* Fill the top-level `;`-headed statement list's C-NULL slots (empty
 * statements) with empty general lists, IN PLACE: kdb `parse ";"` is
 * (;;();()).  ONLY for trees handed out as DATA (the q `parse` builtin and
 * its TSV harness) — the eval path keeps the raw C-NULL slots, which encode
 * "no-op statement, no output".  No-op on any other tree shape. */
void q_ast_fill_empty_stmts(ray_t* ast);

/* True iff h is the statement-sequence HEAD — the char ";" (-10h), not a
 * symbol.  THE one test; q_eval's head dispatch and the parser both ask it. */
int q_parse_is_seq_head(const ray_t* h);

/* `-4!x`: src[0..n) (NUL-terminated at n) cut at the scanner's token boundaries into a list of char vectors;
 * the blanks and comments between them are tokens too, so the pieces raze back to the input. */
ray_t* q_parse_tokens(const char* src, int64_t n);

/* The scanner's view of one statement's text as it grows a line at a time: `{ ( [` left open, and whether it ends
 * inside a string literal.  Start zeroed and feed each piece once, so the cost stays linear.  The first piece takes
 * the text intake: a `\` command or a non-q `<letter>)` is opaque and never open.  Any other scan error keeps what was
 * counted before it — the statement's own parse reports it. */
typedef struct { int depth, in_string, opaque, fed; } q_parse_open_t;
void q_parse_open_feed(q_parse_open_t *st, const char *piece);
static inline int q_parse_open_is(const q_parse_open_t *st) {
    return !st->opaque && (st->in_string || st->depth > 0);
}

#endif /* Q_PARSE_H */
