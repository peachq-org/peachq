/* q_pq — the `\l pq` standard-library gate.  ONE embedded bundle: every
 * top-level .q file under lib/ (sorted at build — the ANY-ORDER LAW makes
 * order moot), run through the multiline statement seam q_ctx_run_src, so
 * lib files carry ordinary kdb script syntax.  Nothing here runs at
 * q_runtime_create — the pre-gate env stays kdb-clean. */
#include "qlang/q_pq.h"
#include "qlang/q_ctx.h"       /* q_ctx_run_src — THE script seam */
#include "qlang/io/q_duckdb.h" /* q_duckdb_register — the .duckdb.i.* natives */
#include "qlang/io/q_conn.h"   /* q_conn_pq_register — the .pq.i.conns native */
#include "qlang/io/q_ffi.h"    /* q_ffi_register — the .ffi.i.* natives */
#include "qlang/lib_gen.h"     /* OPENQ_LIB_BOOTSTRAP — the codegen'd lib/ bundle */
#include <stdio.h>

void q_pq_load(void) {
    q_duckdb_register();   /* before the bundle: lib/duckdb.q hooks call these */
    q_conn_pq_register();
    q_ffi_register();
    q_ctx_run_src(OPENQ_LIB_BOOTSTRAP, stdout, stderr);
}
