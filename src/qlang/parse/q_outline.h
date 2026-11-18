/* q_outline — `.pq.i.outline src`, the definitions behind `.pq.outline`: `file`defs!(the file's header; ([] name; ns;
 * kind; params; at; doc)), at a byte offset into src and doc the raw header the load would register (docs/
 * help-architecture.md § Rules). */
#ifndef Q_OUTLINE_H
#define Q_OUTLINE_H

void q_outline_pq_register(void);   /* binds .pq.i.outline */

#endif
