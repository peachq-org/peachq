/* q_syntax — `.pq.syntax src`: the tolerant syntax tree of q source text, for tools.  It answers
 * `nodes`errors!(nodes;errors): nodes ([] id; parent; kind; start; end; scope; text) and errors ([] start; end; msg),
 * byte offsets end-exclusive into src (docs/parser-architecture.md § Rules). */
#ifndef Q_SYNTAX_H
#define Q_SYNTAX_H

void q_syntax_pq_register(void);   /* binds .pq.i.syntax */

#endif
