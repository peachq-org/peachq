/* q_handles — the q-layer handle authority (actionable-plans/
 * 2026-07-22-handle-registry.md, Phase 1).  SINGLE home for the handle
 * abstraction: open/apply/close/read for q-opened file+fifo fds, apply/close
 * dispatch onto the IPC layer for sockets, and the fd-keyed registry of
 * open-time metadata (redacted at capture — a password never enters a record).
 * No other file branches on handle kind — the apply module delegates here, and
 * the `hopen`/`hclose` verb bodies live here too (moved off ops/q_io.c
 * 2026-07-31; declared with the other registry entrypoints, not below).
 * The registry is purely internal — it drives dispatch and Phase-2 `-38!`. */
#ifndef QLANG_Q_HANDLES_H
#define QLANG_Q_HANDLES_H
#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    Q_HANDLE_FILE   = 0,
    Q_HANDLE_FIFO   = 1,
    Q_HANDLE_SOCKET = 2,
} q_handle_kind;

void q_handles_init(void);
void q_handles_destroy(void);

/* `args` = raw descriptor; a socket's `host:port:user:password` tail is
 * redacted at capture.  Returns 0 on OOM — the caller of a file/fifo open must
 * then close the fd (dispatch recognises a raw handle only via registration). */
int q_handles_register(int64_t fd, q_handle_kind kind, int initiated_out,
                       const char* args, size_t args_len);

void q_handles_deregister(int64_t fd);

/* Registered kind of `fd`, or -1 when not a q-registered handle. */
int q_handles_kind(int64_t fd);

/* Stored metadata (Phase-2 `-38!` columns; the redaction unit's observation
 * points).  open_args is BORROWED, NULL when unregistered; user_sym -1. */
ray_t*  q_handles_open_args(int64_t fd);
int64_t q_handles_user_sym(int64_t fd);

/* Open a file (write/append) or fifo (read) fd and register it.  Owned int
 * handle atom (the real fd, >= 3) or error. */
ray_t* q_handles_open(const char* path, size_t plen, int is_fifo);

/* `h x` dispatch by kind: console (1/-1/2/-2) text-write, FILE write (raw;
 * `neg h` appends '\n' per basics/handles.md), FIFO 'nyi (Phase-1 fifo is a
 * reader), SOCKET/unregistered -> IPC send (positive sync, negative async).
 * Total over int handles. */
ray_t* q_handles_apply(int64_t qh, ray_t* y);

/* `` `:… `` sym-handle apply — the protocol arm (caller has checked the
 * leading ':'): ws/wss and http/https clients, else one-shot sync IPC on a
 * string argument; a file handle has no apply.  Args borrowed, result owned. */
ray_t* q_handles_sym_apply(ray_t* head, ray_t** args, int64_t n);

/* file/fifo -> close fd + deregister; else deregister + IPC close (dead
 * handle = tolerated no-op).  Caller gates restricted mode + validates qh>0. */
ray_t* q_handles_close(int64_t qh);

/* One blocking read of up to `count` bytes from a registered FIFO handle
 * (`read1(h;n)`, `.Q.fpn`; empty read = EOF).  NULL when `fd` is not a
 * registered fifo — the caller keeps its historic errors. */
ray_t* q_handles_read1(int64_t fd, ray_t* count);

#endif
