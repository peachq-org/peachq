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

/* Rewrite dyadic q-glyph verb heads in a freshly-parsed AST to their q op
 * registry function values (q semantics: % -> /, = -> ==, <> -> !=), in place.
 * Call between q_parse and ray_eval; do NOT call on the AST returned by the
 * `parse` builtin (that must keep glyphs).  Returns the (possibly copy-on-write
 * replaced) AST — callers must use the returned pointer for both eval and
 * release.  On a cold registry every lookup misses and the AST is unchanged. */
ray_t* q_resolve_verbs(ray_t* ast);

#endif /* Q_PARSE_H */
