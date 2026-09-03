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
#include "qlang/io/q_csv.h"    /* q_csv_register — the .csv.i.* natives */
#include "qlang/io/q_json.h"  /* q_json_register — the .j.i.* natives */
#include "qlang/io/q_ffi.h"    /* q_ffi_register — the .ffi.i.* natives */
#include "qlang/ops/q_regex.h" /* q_regex_register — the .regexp.i.* natives */
#include "qlang/ops/q_strfmt.h" /* q_strfmt_register — the .str.i.printf/.format natives */
#include "qlang/ops/q_strns.h" /* q_strns_register — the .str.i.* strip natives */
#include "qlang/lib_gen.h"     /* PEACHQ_LIB_BOOTSTRAP — the codegen'd lib/ + qlib/src bundle */
#include "qlang/helpdb_gen.h"  /* PEACHQ_HELPDB_BOOTSTRAP — the codegen'd lib/help-db.q */
#include "qlang/q_env.h"       /* q_env_bind — the `.help.i.loaddb` binding */
#include "lang/env.h"          /* ray_fn_unary — the `.help.i.loaddb` value */
#include "lang/eval.h"         /* RAY_FN_NONE — no dispatch attrs on that value */
#include <stdio.h>
#include <string.h>

/* The builtin help DATA, split from the always-on machinery (src/qlang/help.q).
 * FIRST HELP ACCESS is the only thing that brings it in (owner 2026-09-03): not
 * bare boot, and NOT `\l pq` — every reader door calls `.help.i.loaddb`, and the
 * once-per-runtime flag makes every call after the first free.  A FAILED load is
 * not a load — it stays retryable, because caching an abort would starve help for
 * the life of the runtime; RUNNING is its own state so a re-entrant call (which
 * the generated block cannot make today) can never recurse. */
enum { HELPDB_COLD, HELPDB_RUNNING, HELPDB_DONE };
static int g_helpdb_state;

static ray_t* helpdb_load(void) {
    if (g_helpdb_state != HELPDB_COLD) return NULL;
    g_helpdb_state = HELPDB_RUNNING;
    ray_t* esig = NULL;
    int rc = q_ctx_run_src(PEACHQ_HELPDB_BOOTSTRAP, stdout, stderr, &esig);
    ray_t* e = esig ? esig : (rc >= 2 ? q_err((q_err_e)(rc - 2)) : NULL);
    g_helpdb_state = e ? HELPDB_COLD : HELPDB_DONE;
    return e;
}

static ray_t* helpdb_loaddb_fn(ray_t* x) {
    (void)x;
    ray_t* e = helpdb_load();
    if (e) return e;
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

void q_pq_helpdb_register(void) {
    static const char nm[] = ".help.i.loaddb";
    ray_t* obj = ray_fn_unary(nm, RAY_FN_NONE, helpdb_loaddb_fn);
    q_env_bind(ray_sym_intern(nm, strlen(nm)), obj);
    ray_release(obj);
}

void q_pq_reset(void) { g_helpdb_state = HELPDB_COLD; }

ray_t* q_pq_load(void) {
    q_duckdb_register();   /* before the bundle: lib/duckdb.q hooks call these */
    q_conn_pq_register();
    q_console_pq_register();
    q_ffi_register();
    q_csv_register();
    q_json_register();
    q_regex_register();
    q_strfmt_register();
    q_strns_register();
    ray_t* esig = NULL;
    int rc = q_ctx_run_src(PEACHQ_LIB_BOOTSTRAP, stdout, stderr, &esig);
    if (esig) return esig;
    return rc >= 2 ? q_err((q_err_e)(rc - 2)) : NULL;
}
