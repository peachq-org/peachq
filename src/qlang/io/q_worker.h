/* q_worker — a worker is another peachq process on the far end of one socket, spoken to in the kdb protocol like any
 * server, and OWNED by the handle that reaches it (q_handles: hclose kills and reaps it).  qfork makes one by forking
 * this process: the child is a snapshot that only computes (docs/ipc-architecture.md § Rules) and serves its end of
 * the link until the link closes.  qspawn launches this executable afresh: an ordinary q whose recipe is its argv. */
#ifndef QLANG_Q_WORKER_H
#define QLANG_Q_WORKER_H
#include <rayforce.h>

/* Serve `link` as the only connection of a fresh event poll; once it closes, end its own workers and _exit(0). */
_Noreturn void q_worker_serve(int link);

/* The `:pq:qfork:` open: fork, hand the child to q_worker_serve, and answer the parent's end as an int socket handle
 * once the kdb handshake is done (timeout ms; NULL, null, 0 or negative = 5000).  'nofork when this process cannot be
 * copied safely — a swap-backed heap pool, not the main thread, a parallel region running, no fork on the platform;
 * 'fork when the OS refuses or the child never answers; 'access under -u. */
ray_t* q_worker_fork(ray_t* tmo);

/* The `:pq:qspawn:` open: launch this executable with exactly argv (a list of strings; no shell, no flag of ours), its
 * end of a socketpair at Q_WORKER_FD, and answer the parent's end once the kdb handshake is done (timeout ms; NULL,
 * null, 0 or negative = 10000).  'proc when it cannot be launched or ends before it answers; 'nyi under wasm;
 * 'access under -u. */
ray_t* q_worker_spawn(ray_t* argv, ray_t* tmo);

/* A launched worker learns its link from Q_WORKER_FD_ENV, which this unsets before any q can read it: the link's fd,
 * bound to die with its parent and every other inherited fd closed; -1 = not a worker.  POSIX names "fd:parent pid";
 * Windows names an inherited pipe carrying the WSAPROTOCOL_INFOW the parent duplicated the link into, and the parent's
 * job object is what ties the worker's life to it. */
#define Q_WORKER_FD_ENV "PEACHQ_WORKER_FD"
#define Q_WORKER_FD 3
int q_worker_link(void);

#endif
