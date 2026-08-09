/* q_conn — THE connection collector (one home): unions live IPC sockets,
 * q_handles file/fifo/provider records and q_provider detail into one row
 * set; every q-visible connection verb below is a VIEW over that one walk. */
#ifndef QLANG_Q_CONN_H
#define QLANG_Q_CONN_H
#include <rayforce.h>

ray_t* q_conn_table(void);        /* .pq.conns[] — the 13-column superset */
ray_t* q_conn_bang38(ray_t* y);   /* -38!x — socket-only; atom->dict, list->table */
ray_t* q_conn_zW(void);           /* .z.W — socket handles!unsent bytes (I!J) */
ray_t* q_conn_zH(void);           /* .z.H — active socket handles (I, sorted) */
void   q_conn_pq_register(void);  /* bind the .pq.i.conns native (\l pq gate) */

#endif
