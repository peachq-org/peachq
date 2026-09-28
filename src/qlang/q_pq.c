/* q_pq — the standard library's C floor, none of it run at q_runtime_create:
 * the `.pq` AUTOLOAD (the first reference to a `.pq` name, read or write, runs
 * lib/pq.q once — q_env's hook fires it), the library AUTOLOAD (a missing
 * `.<file>…` read runs `\l pq/<file>.q` once), the per-file load record `.pq.load`
 * reads, the `.pq.load_natives` root every
 * native-backed lib/ file opens with (so a file loaded from disk carries its
 * natives as the bundle member does), and the bundle MEMBER door behind
 * `\l pq/<file>.q`.  The load SEQUENCE is q — `.pq.load` in lib/pq.q; `\l pq`
 * runs `.pq.i.run` over every file.  Every file runs as its own NAMED script through
 * the multiline seam, so the doc store's file column reads `lib/str.q` and each
 * header is captured.  The help-db seam is bound at boot, loaded at first help
 * access — by neither event. */
#include "qlang/q_count.h"
#include "qlang/q_pq.h"
#include "qlang/base/q_err.h"  /* q_err_name — the unknown-set 'name */
#include "qlang/ops/q_sys.h"   /* q_sys_load / _load_find — the `\l` loader and its search */
#include "qlang/q_ctx.h"       /* q_ctx_run_src / _named_src / _run_abort — THE script seam */
#include "qlang/q_prim.h"      /* q_str_subject_bytes — the set name, a symbol or a string */
#include "qlang/io/q_duckdb.h" /* q_duckdb_register — the .duckdb.i.* natives */
#include "qlang/io/q_conn.h"   /* q_conn_pq_register — the .pq.i.conns native */
#include "qlang/io/q_provider.h" /* q_provider_pq_register — the .pq.i.load native */
#include "qlang/q_console.h"   /* q_console_pq_register — the .pq.i.termsize native */
#include "qlang/q_fmt.h"       /* q_fmt_pq_register — the .pq.i.facts native */
#include "qlang/io/q_csv.h"    /* q_csv_register — the .csv.i.* natives */
#include "qlang/io/q_io.h"     /* q_io_fs_register — the .fs.i.rmtree native */
#include "qlang/io/q_json.h"  /* q_json_register — the .j.i.* natives */
#include "qlang/io/q_md.h"    /* q_md_register — the .md.i.* natives */
#include "qlang/io/q_ffi.h"    /* q_ffi_register — the .ffi.i.* natives */
#include "qlang/io/q_termbox.h" /* q_termbox_register — the .termbox.i.* natives */
#include "qlang/io/q_beep.h"    /* q_beep_register — the .termbox.i.beep native */
#include "qlang/ops/q_regex.h" /* q_regex_register — the .regexp.i.* natives */
#include "qlang/ops/q_strfmt.h" /* q_strfmt_register — the .str.i.printf/.format natives */
#include "qlang/ops/q_strns.h" /* q_strns_register — the .str.i.* strip natives */
#include "qlang/lib_gen.h"     /* PEACHQ_LIB_FILES — the codegen'd lib/ + qlib/src bundle, one entry per file */
#include "qlang/helpdb_gen.h"  /* PEACHQ_HELPDB_BOOTSTRAP — the codegen'd lib/help-db.q */
#include "qlang/q_env.h"       /* q_env_bind_native / q_env_ident_ok — the native bindings, a namespace name */
#include "lang/env.h"          /* ray_fn_unary — the native values */
#include "lang/eval.h"         /* RAY_FN_NONE — no dispatch attrs on those values */
#include <stdio.h>
#include <string.h>

/* one string of q through the script seam, answering its abort as `\l` does */
static ray_t* pq_run(const char* src) {
    ray_t* esig = NULL;
    int rc = q_ctx_run_src(src, stdout, stderr, &esig);
    return q_ctx_run_abort(rc, esig);
}

static void pq_bind(const char* nm, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(nm, RAY_FN_NONE, fn);
    q_env_bind_native(nm, obj, 1);
    ray_release(obj);
}

/* The builtin help DATA, split from the always-on machinery (src/qlang/help.q).
 * FIRST HELP ACCESS is the only thing that brings it in (owner 2026-09-03): not
 * bare boot, and NOT `\l pq` — every reader door calls `.help.i.loaddb`, and the
 * once-per-runtime flag makes every call after the first free.  A FAILED load is
 * not a load — it stays retryable, because caching an abort would starve help for
 * the life of the runtime; RUNNING is its own state so a re-entrant call (which
 * the generated block cannot make today) can never recurse. */
enum { LOAD_COLD, LOAD_RUNNING, LOAD_DONE };
static int g_helpdb_state, g_pq_state;

static ray_t* pq_helpdb_fn(ray_t* x) {
    (void)x;
    if (g_helpdb_state == LOAD_COLD) {
        g_helpdb_state = LOAD_RUNNING;
        ray_t* e = pq_run(PEACHQ_HELPDB_BOOTSTRAP);
        g_helpdb_state = e ? LOAD_COLD : LOAD_DONE;
        if (e) return e;
    }
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

void q_pq_helpdb_register(void) { pq_bind(".help.i.loaddb", pq_helpdb_fn); }

/* The per-file load record: a file is LOADED once `\l pq/<file>.q` has run it (a failed run too: no retry); a MISS is a
 * namespace with no library file, remembered so a probed-for name costs its stats once until the search path moves. */
static ray_t *g_loaded, *g_misses;

static int pq_has(ray_t* v, int64_t sym) {
    for (int64_t i = 0; v && i < v->len; i++) if (((int64_t*)ray_data(v))[i] == sym) return 1;
    return 0;
}

static void pq_note(ray_t** v, int64_t sym) {
    if (pq_has(*v, sym)) return;
    ray_t* r = ray_vec_append(*v ? *v : ray_sym_vec_new(RAY_SYM_W64, 8), &sym);
    if (r && !RAY_IS_ERR(r)) *v = r;
}

static void pq_drop(ray_t** v) { if (*v) ray_release(*v); *v = NULL; }

void q_pq_path_changed(void) { pq_drop(&g_misses); }

void q_pq_reset(void) { g_helpdb_state = g_pq_state = LOAD_COLD; pq_drop(&g_loaded); pq_drop(&g_misses); }

/* `.pq.i.loaded[]` — the files the record holds as loaded, in load order */
static ray_t* pq_loaded_fn(ray_t* x) {
    (void)x;
    if (!g_loaded) return ray_sym_vec_new(RAY_SYM_W64, 1);
    ray_retain(g_loaded);
    return g_loaded;
}

/* The native sets by lib/ file stem: `.pq.load_natives`csv` binds what lib/csv.q
 * calls.  `pq` is the session set lib/pq.q needs (connections, the provider
 * load, the terminal); `str` is both string engines; `termbox` includes the
 * beep.  Re-binding is idempotent, so a reload costs nothing but the bind. */
static void pq_set_pq(void)      { q_conn_pq_register(); q_provider_pq_register(); q_console_pq_register(); q_fmt_pq_register();
                                   q_ctx_pq_register(); pq_bind(".pq.i.loaded", pq_loaded_fn); }
static void pq_set_str(void)     { q_strfmt_register(); q_strns_register(); }
static void pq_set_termbox(void) { q_termbox_register(); q_beep_register(); }
static const struct { const char* name; void (*bind)(void); } PQ_SETS[] = {
    { "pq", pq_set_pq },            { "csv", q_csv_register },     { "duckdb", q_duckdb_register },
    { "ffi", q_ffi_register },      { "fs", q_io_fs_register },    { "j", q_json_register },
    { "md", q_md_register },        { "regexp", q_regex_register }, { "str", pq_set_str },
    { "termbox", pq_set_termbox },
};

/* `.pq.load_natives`name` — the C root of a native-backed lib/ file, its FIRST
 * statement.  A symbol or string names the set; an unknown one is 'name, as an
 * undefined word is. */
static ray_t* pq_load_natives_fn(ray_t* x) {
    const char* p; int64_t n;
    if (!q_str_subject_bytes(x, &p, &n) || n == 0) return q_err(QE_TYPE);
    size_t i = 0, nsets = sizeof PQ_SETS / sizeof *PQ_SETS;
    while (i < nsets && (strlen(PQ_SETS[i].name) != (size_t)n || memcmp(PQ_SETS[i].name, p, (size_t)n) != 0)) i++;
    if (i == nsets) return q_err_name(p, (size_t)n);
    PQ_SETS[i].bind();
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

ray_t* q_pq_autoload(void) { return g_pq_state == LOAD_COLD ? pq_run("\\l pq/pq.q") : NULL; }

ray_t* q_pq_load(void) { return pq_run(".pq.i.run .pq.i.files;"); }

/* the bundle member whose basename is `<stem>.q` (lib/ and qlib/src/ share one namespace of stems) */
static const char* pq_member(const char* stem, size_t n, const char** src) {
    for (size_t i = 0; i < sizeof PEACHQ_LIB_FILES / sizeof *PEACHQ_LIB_FILES; i++) {
        const char* base = strrchr(PEACHQ_LIB_FILES[i].name, '/') + 1;
        if (strncmp(base, stem, n) == 0 && strcmp(base + n, ".q") == 0)
            return *src = PEACHQ_LIB_FILES[i].src, PEACHQ_LIB_FILES[i].name;
    }
    return NULL;
}

ray_t* q_pq_load_file(const char* lit, size_t alen, const char* path) {
    const char* stem = lit + 3;                     /* past "pq/" */
    size_t n = alen - 3;
    if (n > 2 && memcmp(stem + n - 2, ".q", 2) == 0) n -= 2;
    const char* src = NULL;
    const char* nm = path ? NULL : pq_member(stem, n, &src);
    if (!path && !nm) return q_err_name(lit, alen); /* kdb: 'path as given */
    pq_note(&g_loaded, ray_sym_intern_runtime(stem, n));
    int self = g_pq_state == LOAD_COLD && n == 2 && memcmp(stem, "pq", 2) == 0;
    if (self) {                                     /* THE autoload: pq.q's first line can already see the root */
        g_pq_state = LOAD_RUNNING;
        pq_bind(".pq.load_natives", pq_load_natives_fn);
    }
    ray_t* esig = NULL;
    int rc = path ? q_ctx_run_file(path, stdout, stderr, &esig)
                  : q_ctx_run_named_src(nm, src, stdout, stderr, &esig);
    if (self) g_pq_state = LOAD_DONE;               /* a failed load displayed its error; no retry */
    return q_ctx_run_abort(rc, esig);
}

ray_t* q_pq_autoload_ns(const char* stem, size_t n) {
    if (n > 64 || (n == 2 && memcmp(stem, "pq", 2) == 0) || !q_env_ident_ok(stem, n)) return NULL;
    int64_t sym = ray_sym_intern_runtime(stem, n);
    if (pq_has(g_loaded, sym) || pq_has(g_misses, sym)) return NULL;
    char lit[80];
    const char* src;
    int len = snprintf(lit, sizeof lit, "pq/%.*s.q", (int)n, stem);
    if (!pq_member(stem, n, &src) && !q_sys_load_find(lit, (size_t)len, NULL)) {
        pq_note(&g_misses, sym);
        return NULL;
    }
    ray_t* e = q_pq_autoload();                     /* `.pq` first: every autoload bootstraps the library root */
    if (!e) e = q_sys_load(lit, (size_t)len);       /* `\l`, not `.pq.load`: a failing file names its own line */
    if (e && !RAY_IS_ERR(e)) { ray_release(e); e = NULL; }
    if (e) return e;
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}
