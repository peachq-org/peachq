/* q_pq — the `\l pq` standard-library gate.  ONE embedded bundle: every
 * top-level .q file under lib/ AND under qlib/src (the portable half), sorted
 * at build — the ANY-ORDER LAW makes order moot — run through the multiline
 * statement seam q_ctx_run_src, so those files carry ordinary kdb script
 * syntax.  Nothing here runs at
 * q_runtime_create — the pre-gate env stays kdb-clean. */
#include "qlang/q_pq.h"
#include "qlang/base/q_err.h"  /* q_err — an aborted bundle load's re-signal */
#include "qlang/q_ctx.h"       /* q_ctx_run_src — THE script seam */
#include "qlang/io/q_duckdb.h" /* q_duckdb_register — the .duckdb.i.* natives */
#include "qlang/io/q_conn.h"   /* q_conn_pq_register — the .pq.i.conns native */
#include "qlang/q_console.h"   /* q_console_pq_register — the .pq.i.termsize native */
#include "qlang/io/q_ffi.h"    /* q_ffi_register — the .ffi.i.* natives */
#include "qlang/ops/q_regex.h" /* q_regex_register — the .regexp.i.* natives */
#include "qlang/ops/q_strns.h" /* q_strns_register — the .str.i.* natives */
#include "qlang/lib_gen.h"     /* PEACHQ_LIB_BOOTSTRAP — the codegen'd lib/ + qlib/src bundle */
#include <stdio.h>

ray_t* q_pq_load(void) {
    q_duckdb_register();   /* before the bundle: lib/duckdb.q hooks call these */
    q_conn_pq_register();
    q_console_pq_register();
    q_ffi_register();
    q_regex_register();
    q_strns_register();
    ray_t* esig = NULL;
    int rc = q_ctx_run_src(PEACHQ_LIB_BOOTSTRAP, stdout, stderr, &esig);
    if (esig) return esig;
    return rc >= 2 ? q_err((q_err_e)(rc - 2)) : NULL;
}
