/* q_parse — parse one line of q source into a rayforce ray_t AST.
 *
 * Ported from kparser (Apache-2.0): the recursive-descent scanner/parser
 * control flow is kept; the value layer emits ray_t instead of K structs.
 */
#ifndef Q_PARSE_H
#define Q_PARSE_H

#include <rayforce.h>
#include <stddef.h>

/* Parse q source into a ray_t AST tree.
 *
 * Returns a ray_t owned by the caller (release with ray_release), or a
 * RAY_ERROR on malformed input.  A statement sequence of one collapses to
 * that single expression; two or more become a (`;; ...) list.
 *
 * Requires an initialised rayforce runtime (symbol interning). */
ray_t* q_parse(const char* src);

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
int q_ast_is_seq_head(const ray_t* h);

#endif /* Q_PARSE_H */
