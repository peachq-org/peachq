/* q_dbg — backtraces + the q)) debugger (basics/debug.md).  The walker keeps a
 * live per-thread lambda-frame list; the first apply/lambda seam to observe an
 * error SNAPSHOTS it (mode-2 display, .Q.trp/.Q.sbt, .z.ex/.z.ey) and — under
 * `\e 1` on a console statement with a reader installed — suspends right there
 * on the C stack in a nested read-eval loop (`:r` resume / `\` abort). */
#ifndef QLANG_Q_DBG_H
#define QLANG_Q_DBG_H

#include <rayforce.h>
#include <stddef.h>
#include <stdio.h>

void q_dbg_frame_push(ray_t* lam);   /* borrowed; lambda application entry */
void q_dbg_frame_pop(void);

/* Statement seam: stash source + set the console flag; returns the previous
 * flag, restored by q_dbg_statement_end (nested \l / system "l").  A NULL s
 * clears the stash. */
int  q_dbg_statement_begin(const char* s, size_t n, int console);
void q_dbg_statement_end(int prev_console);

/* Inside @[;;]/.[;;]/.Q.trp protection: error-trap mode 0, never suspend. */
void q_dbg_trap_enter(void);
void q_dbg_trap_exit(void);

/* Error seams: filter an owned result; may snapshot, suspend, and return a
 * `:r` replacement.  lambda_filter runs at the body-statement boundary with
 * the frame still live (pre-pop). */
ray_t* q_dbg_filter(ray_t* r, ray_t* fv, ray_t** args, int64_t n);
ray_t* q_dbg_lambda_filter(ray_t* r);

/* Seam display: 1 if the debugger already reported err (print nothing). */
int  q_dbg_reported(ray_t* err);
/* mode-2 uncaught display: 'text + numbered frames (no carets yet). */
void q_dbg_print_trace(FILE* out, ray_t* err);
void q_dbg_snapshot_clear(void);     /* a trap consumed the error */

/* .Q surface bodies (bound as .Q.c.* in q_builtins.c; aliased in dotq.q). */
ray_t* q_dbg_trp_fn(ray_t** args, int64_t n);   /* .Q.trp[f;x;g] */
ray_t* q_dbg_sbt_fn(ray_t* x);                  /* .Q.sbt bt-object -> string */
ray_t* q_dbg_bt_fn(ray_t** args, int64_t n);    /* .Q.bt[] -> console dump */

ray_t* q_dbg_zex(void);              /* owned failed-primitive value or NULL */
ray_t* q_dbg_zey(void);              /* owned arg list or NULL */

/* Console line reader for the suspend loop (front end installs; NULL = no
 * suspension possible).  The reader owns prompt display and echo — piped
 * flavours print both, the tty flavour drives the line editor.  Returns line
 * length, or -1 on EOF. */
typedef int (*q_dbg_read_fn)(const char* prompt, char* buf, size_t cap);
void q_dbg_set_reader(q_dbg_read_fn fn);

void q_dbg_reset(void);              /* runtime teardown: drop retained refs */

#endif
