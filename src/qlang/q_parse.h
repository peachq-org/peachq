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

#endif /* Q_PARSE_H */
