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
    Q_HANDLE_PROVIDER = 3,   /* io/q_provider.h — a `:pq:` alias's connection */
} q_handle_kind;

void q_handles_init(void);
void q_handles_destroy(void);

/* `args` = raw descriptor; a socket's `host:port:user:password` tail is
 * redacted at capture.  Returns 0 on OOM — the caller of a file/fifo open must
 * then close the fd (dispatch recognises a raw handle only via registration). */
int q_handles_register(int64_t fd, q_handle_kind kind, int initiated_out,
                       const char* args, size_t args_len);

void q_handles_deregister(int64_t fd);

/* A forked child drops every record and owned worker it inherited, closing nothing. */
void q_handles_forget(void);

/* An OWNED link: the process `pid` is this handle's worker.  When the IPC connection on `fd` closes — hclose, or the
 * worker died — the worker is signalled (TERM, then KILL after a bounded grace) and reaped, so none is left a zombie.
 * 0 on OOM. */
int     q_handles_own(int64_t fd, int64_t pid);
int64_t q_handles_owned_pid(int64_t fd);   /* -1 = not owned */
void    q_handles_owned_closed(int64_t fd);  /* the link on fd is closed: end and reap its worker, if it has one */
void    q_handles_reap(int64_t pid);          /* TERM, a bounded grace, KILL, and reap one child process */
void    q_handles_end_owned(void);           /* every owned worker at once, under one grace; its link is left open */

/* Registered kind of `fd`, or -1 when not a q-registered handle. */
int q_handles_kind(int64_t fd);

/* Stored metadata (Phase-2 `-38!` columns; the redaction unit's observation
 * points).  open_args is BORROWED, NULL when unregistered; user_sym -1. */
ray_t*  q_handles_open_args(int64_t fd);
int64_t q_handles_user_sym(int64_t fd);

/* Enumeration for the connection collector (io/q_conn.c — the ONE walker). */
typedef struct {
    int64_t       fd;
    q_handle_kind kind;
    int           initiated_out;
    int64_t       user_sym;
    int64_t       open_time_ns;
    ray_t*        open_args;   /* BORROWED */
} q_handle_info_t;

int64_t q_handles_count(void);
/* Record `i` (0 <= i < count) into *out; 0 when out of range. */
int q_handles_get(int64_t i, q_handle_info_t* out);

/* Open a file (write/append) or fifo (read) fd and register it.  Owned int
 * handle atom (the real fd, >= 3) or error. */
ray_t* q_handles_open(const char* path, size_t plen, int is_fifo);

/* Reserve a real fd (>= 3) on the null device — collision-free in the one
 * handle space by construction, no I/O behind it.  -1 on failure. */
int q_handles_reserve_fd(void);

/* `h x` dispatch by kind: console (1/-1/2/-2) text-write, FILE write (text and
 * bytes raw, `neg h` appends '\n' per basics/handles.md; any other payload is
 * records — the serialized append), FIFO 'nyi (Phase-1 fifo is a reader),
 * SOCKET/unregistered -> IPC send (positive sync, negative async).
 * `h` is the applied int/long atom (borrowed); a write echoes it, retained. */
ray_t* q_handles_apply(ray_t* h, ray_t* y);

/* `-25!(handles;msg)` async broadcast: `handles` an int/long vector (or atom)
 * of positive live socket handles, `msg` serialized ONCE and written to each.
 * A bad handle or an unwireable msg is 'type before anything is sent; a socket
 * failure mid-list is 'io.  Restricted mode 'access.  Args borrowed; `::` on
 * success. */
ray_t* q_handles_broadcast(ray_t* handles, ray_t* msg);

/* `-30!y` deferred response: `y` the generic null defers the executing sync
 * request's reply; a 3-list `(h;1b;text)` / `(h;0b;msg)` answers it by
 * handle (`text` a string or symbol, raised by the client).  'domain when
 * nothing is executing or `h` is not a live handle expecting a response;
 * any other shape 'type.  Restricted mode 'access.  y borrowed; `::`. */
ray_t* q_handles_deferred(ray_t* y);

/* `0 x` — the console door (.z.ps if defined, else `value`): every message-to-
 * self, and every chunk `-11!` replays, goes through it.  y borrowed, result owned. */
ray_t* q_handles_console_eval(ray_t* y);

/* What a sym denotes, decided HERE once: a global NAME (no leading ':'), a
 * PROCESS handle (a `:pq:` alias `:pq:q:srv`, ws/wss, http/https, IPC `:host:port` /
 * `::port`) — applicable, so ternary @/. is Trap — or a PATH (file, dir, fifo,
 * a `:pq:duckdb:db:t/` table coordinate) — data, so ternary @/. is Amend. */
typedef enum { Q_SYM_NAME = 0, Q_SYM_PATH = 1, Q_SYM_PROCESS = 2 } q_sym_kind;
q_sym_kind q_handles_sym_kind(ray_t* sym);

/* `:pq:<kind>:<alias>` is a connection, `:pq:<kind>:<alias>:<table>/` a table coordinate.  BAD = the reserved `:pq:`
 * namespace holding anything else (never a file). */
typedef enum { Q_PQ_NONE = 0, Q_PQ_Q, Q_PQ_QFORK, Q_PQ_QSPAWN, Q_PQ_DUCKDB, Q_PQ_BAD } q_pq_kind;
typedef struct { const char* alias; size_t alias_n; const char* table; size_t table_n; int ok; } q_pq_parts;

/* The kind `s` names, NONE outside `:pq:`.  parts (may be NULL) gets the alias and table spans (table NULL = the
 * connection) and ok 0 unless the text is exactly one of the two forms with both names valid — 'domain where used. */
q_pq_kind    q_handles_pq(const char* s, size_t n, q_pq_parts* parts);
q_pq_kind    q_handles_pq_of(ray_t* x, const char** s, size_t* n);   /* a sym/string, or the head of a list of them */
const char* q_handles_pq_kind_name(q_pq_kind k);
int         q_handles_name_ok(const char* p, size_t n);   /* [a-zA-Z][a-zA-Z0-9_]* */
/* `neg` of a `:pq:` sym: a leading `-` on the alias toggled (the async form) whatever the kind; NULL unless the
 * positive spelling is well-formed.  Owned. */
ray_t*      q_handles_pq_neg(ray_t* x);

/* `` `:… `` sym-handle apply — the protocol arm (caller has checked
 * q_handles_sym_kind): ws/wss and http/https clients, else one-shot sync IPC
 * on a string argument; a file handle has no apply.  Args borrowed, result owned. */
ray_t* q_handles_sym_apply(ray_t* head, ray_t** args, int64_t n);

/* file/fifo -> close fd + deregister; else deregister + IPC close (dead
 * handle = tolerated no-op).  Caller gates restricted mode + validates qh>0. */
ray_t* q_handles_close(int64_t qh);

/* One blocking read of up to `count` bytes from a registered FIFO handle
 * (`read1(h;n)`, `.Q.fpn`; empty read = EOF).  NULL when `fd` is not a
 * registered fifo — the caller keeps its historic errors. */
ray_t* q_handles_read1(int64_t fd, ray_t* count);

#endif
