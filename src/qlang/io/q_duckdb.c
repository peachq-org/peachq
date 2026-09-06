/* q_duckdb — peachq's `.duckdb` namespace: the DuckDB bridge.  Contract, type
 * mapping and every decision: docs/duckdb-api.md (sidecar design:
 * docs/superpowers/specs/2026-07-14-duckdb-fidelity-design.md).
 *
 * DuckDB is reached ONLY via dlopen + a dlsym'd fn table (q_duckdb_api.h),
 * lazily on first use: PEACHQ_DUCKDB_LIB set = EXCLUSIVE, else $QHOME ->
 * exe dir -> system default; version gated >= 1.4; every failure is the bare
 * 'duckdb class.  Reads drain the chunk API, writes feed the appender whole
 * chunks — both directions columnar.  String columns cross the boundary as
 * 0h lists of charv (string-C3: physical RAY_STR never reaches q-space);
 * floats follow the live-infinity model (ONLY NaN is null). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/io/q_duckdb.h"
#include "qlang/io/q_duckdb_api.h"
#include "qlang/io/q_duckdb_internal.h"
#include "qlang/io/q_duckdb_types.h"
#include "qlang/io/q_exedir.h"
#include "qlang/base/q_err.h"
#include "qlang/q_env.h"
#include "qlang/q_prim.h"     /* q_str_text_bytes (write-path text cells) + q_table_meta_assemble */
#include "lang/env.h"         /* ray_fn_unary / ray_fn_vary */
#include "lang/eval.h"        /* RAY_FN_NONE, ray_at_fn */
#include "table/sym.h"        /* ray_sym_vec_cell */
#include <rayforce.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>          /* strncasecmp */
#else
#define strncasecmp _strnicmp
#endif

#if defined(__EMSCRIPTEN__)
/* no dynamic loading in the wasm build — loader is a constant failure */
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int qd_buf_reserve(qd_buf* b, size_t extra) {
    if (b->oom) return 0;
    if (b->len + extra + 1 <= b->cap) return 1;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + extra + 1) ncap *= 2;
    char* np = realloc(b->p, ncap);
    if (!np) { b->oom = 1; return 0; }
    b->p = np; b->cap = ncap;
    return 1;
}

static void qd_putn(qd_buf* b, const char* s, size_t n) {
    if (!qd_buf_reserve(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void qd_puts(qd_buf* b, const char* s) { qd_putn(b, s, strlen(s)); }

/* SQL identifier: "name" with embedded " doubled. */
void qd_put_ident(qd_buf* b, const char* s, size_t n) {
    qd_putn(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"') qd_putn(b, "\"\"", 2);
        else             qd_putn(b, s + i, 1);
    }
    qd_putn(b, "\"", 1);
}

/* SQL string literal: 'text' with embedded ' doubled. */
void qd_put_strlit(qd_buf* b, const char* s, size_t n) {
    qd_putn(b, "'", 1);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\'') qd_putn(b, "''", 2);
        else              qd_putn(b, s + i, 1);
    }
    qd_putn(b, "'", 1);
}

void qd_buf_free(qd_buf* b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

/* ---- loader: lazy dlopen + dlsym'd table ---- */

/* Order MUST match the duck_api_t field order (slots filled positionally). */
static const char* const QD_SYMS[] = {
    "duckdb_library_version",
    "duckdb_open_ext",
    "duckdb_close",
    "duckdb_create_config",
    "duckdb_set_config",
    "duckdb_destroy_config",
    "duckdb_connect",
    "duckdb_disconnect",
    "duckdb_query",
    "duckdb_destroy_result",
    "duckdb_column_count",
    "duckdb_column_name",
    "duckdb_fetch_chunk",
    "duckdb_destroy_data_chunk",
    "duckdb_data_chunk_get_size",
    "duckdb_data_chunk_get_vector",
    "duckdb_vector_get_data",
    "duckdb_vector_get_validity",
    "duckdb_vector_ensure_validity_writable",
    "duckdb_validity_set_row_invalid",
    "duckdb_vector_assign_string_element_len",
    "duckdb_vector_size",
    "duckdb_create_logical_type",
    "duckdb_destroy_logical_type",
    "duckdb_create_data_chunk",
    "duckdb_data_chunk_reset",
    "duckdb_data_chunk_set_size",
    "duckdb_appender_create_ext",
    "duckdb_appender_destroy",
    "duckdb_append_data_chunk",
    "duckdb_result_error",
    "duckdb_appender_error",
    "duckdb_free",
    "duckdb_appender_flush",
    "duckdb_create_list_type",
    "duckdb_list_type_child_type",
    "duckdb_column_logical_type",
    "duckdb_get_type_id",
    "duckdb_list_vector_get_child",
    "duckdb_list_vector_reserve",
    "duckdb_list_vector_set_size",
};
#define QD_NSYMS (sizeof QD_SYMS / sizeof *QD_SYMS)

_Static_assert(sizeof(duck_api_t) == QD_NSYMS * sizeof(void*),
               "QD_SYMS[] must match duck_api_t, in order");

duck_api_t qd_api;   /* THE dlsym'd table (declared in q_duckdb_internal.h) */

static struct {
    int   state;             /* 0 = untried, 1 = loaded, 2 = failed */
    void* dl;
} g_qd;

#if !defined(__EMSCRIPTEN__)
static void* qd_dlopen(const char* path) {
#if defined(_WIN32)
    return (void*)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void* qd_dlsym(void* dl, const char* name) {
#if defined(_WIN32)
    return (void*)(uintptr_t)GetProcAddress((HMODULE)dl, name);
#else
    return dlsym(dl, name);
#endif
}

static void qd_dlclose(void* dl) {
#if defined(_WIN32)
    FreeLibrary((HMODULE)dl);
#else
    dlclose(dl);
#endif
}

#if defined(_WIN32)
#define QD_LIB_BASENAME "duckdb.dll"
#elif defined(__APPLE__)
#define QD_LIB_BASENAME "libduckdb.dylib"
#else
#define QD_LIB_BASENAME "libduckdb.so"
#endif

#endif /* !__EMSCRIPTEN__ */

/* One load attempt; PEACHQ_DUCKDB_LIB set = EXCLUSIVE (no fallback).  Only
 * SUCCESS latches: a failed attempt retries on the next open, so fixing the
 * lib path (or the env var) works without restarting q. */
static void qd_load(void) {
    if (g_qd.state == 1) return;
#if defined(__EMSCRIPTEN__)
    g_qd.state = 2;
    return;
#else
    void* dl = NULL;
    const char* envp = getenv("PEACHQ_DUCKDB_LIB");
    if (envp && *envp) {
        dl = qd_dlopen(envp);
        if (!dl) { g_qd.state = 2; return; }
    }
    if (!dl) {
        const char* qh = getenv("QHOME");
        if (qh && *qh) {
            char cand[600];
            snprintf(cand, sizeof cand, "%s/%s", qh, QD_LIB_BASENAME);
            dl = qd_dlopen(cand);
        }
    }
    if (!dl) {                                  /* the module ships beside `q` */
        char dir[512];
        if (q_exedir(dir, sizeof dir)) {
            char cand[600];
            snprintf(cand, sizeof cand, "%s/%s", dir, QD_LIB_BASENAME);
            dl = qd_dlopen(cand);
        }
    }
    if (!dl) dl = qd_dlopen(QD_LIB_BASENAME);
    if (!dl) { g_qd.state = 2; return; }
    void** slots = (void**)&qd_api;
    for (size_t i = 0; i < QD_NSYMS; i++) {
        slots[i] = qd_dlsym(dl, QD_SYMS[i]);
        if (!slots[i]) {
            g_qd.state = 2;
            memset(&qd_api, 0, sizeof qd_api);
            qd_dlclose(dl);                     /* a retry re-opens; don't stack refcounts */
            return;
        }
    }
    /* version gate: the bridge's contract is written against >= 1.4 */
    const char* ver = QAPI.library_version();
    int maj = 0, min = 0;
    if (!ver || sscanf(ver, "v%d.%d", &maj, &min) != 2 ||
        maj < 1 || (maj == 1 && min < 4)) {
        g_qd.state = 2;
        memset(&qd_api, 0, sizeof qd_api);
        qd_dlclose(dl);
        return;
    }
    g_qd.dl = dl;
    g_qd.state = 1;
#endif
}

bool q_duckdb_available(void) {
    qd_load();
    return g_qd.state == 1;
}

#define QD_MAX_DB    32
#define QD_MAX_CON   64
#define QD_SLOT_BITS 6           /* low 6 bits = slot (matches QD_MAX_CON) */

static struct {
    char          path[512];     /* normalized: "" = in-memory (`:default:) */
    duck_database db;
    int           refs;
    bool          used;
} g_dbs[QD_MAX_DB];

static struct {
    duck_connection con;
    int             dbslot;
    uint32_t        gen;         /* bumped on close — stale handles error */
    bool            live;
    char            display[512];/* original connect spec, for connections[] */
    char            err[1024];   /* last message behind a 'duckdb here: DuckDB's text or the bridge's reason */
} g_cons[QD_MAX_CON];

duck_connection qd_con(int slot) { return g_cons[slot].con; }

/* most recent captured text, any connection — .duckdb.err[] reads this */
static char g_err_last[1024];

/* THE message channel behind the bare 'duckdb: DuckDB's own text and the bridge's own reasons both land here */
void qd_err_stash(int slot, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err_last, sizeof g_err_last, fmt, ap);
    va_end(ap);
    if (slot >= 0) memcpy(g_cons[slot].err, g_err_last, sizeof g_err_last);
}

static int32_t qd_handle_of(int slot) {
    return (int32_t)((g_cons[slot].gen << QD_SLOT_BITS) | (uint32_t)slot);
}

static int qd_resolve(ray_t* h) {
    int64_t v;
    if (h && h->type == -RAY_I32)      v = h->i32;
    else if (h && h->type == -RAY_I64) v = h->i64;
    else return -1;
    if (v < 0) return -1;
    int slot = (int)(v & ((1 << QD_SLOT_BITS) - 1));
    uint32_t gen = (uint32_t)(v >> QD_SLOT_BITS);
    if (slot >= QD_MAX_CON || !g_cons[slot].live || g_cons[slot].gen != gen) return -1;
    return slot;
}

/* Close one live slot (disconnect; close the db on last ref). */
static void qd_close_slot(int slot) {
    if (!g_cons[slot].live) return;
    QAPI.disconnect(&g_cons[slot].con);
    g_cons[slot].live = false;
    g_cons[slot].gen++;
    int ds = g_cons[slot].dbslot;
    if (ds >= 0 && g_dbs[ds].used && --g_dbs[ds].refs <= 0) {
        QAPI.close(&g_dbs[ds].db);
        g_dbs[ds].used = false;
        g_dbs[ds].path[0] = '\0';
    }
}

void q_duckdb_reset(void) {
    if (g_qd.state != 1) return;
    for (int i = 0; i < QD_MAX_CON; i++) qd_close_slot(i);
    /* no q value outlives its runtime, so generations restart at 0 — the
     * next runtime (suite) sees deterministic handles (codex P1) */
    memset(g_cons, 0, sizeof g_cons);
    memset(g_dbs, 0, sizeof g_dbs);
    g_err_last[0] = '\0';
}
/* the bridge's own refusal: the reason to the message channel, the bare class to the caller */
ray_t* qd_fail(int slot, const char* what, const char* why) {
    qd_err_stash(slot, "%s: %s", what, why);
    return q_err(QE_DUCKDB);
}
/* Run one statement; error => owned 'duckdb (result destroyed), else fills
 * *out (caller destroys).  stash=0: internal probes never pollute err[]. */
ray_t* qd_run2(int slot, const char* sql, duck_result* out, int stash) {
    if (QAPI.query(g_cons[slot].con, sql, out) != QDuckSuccess) {
        if (stash) qd_err_stash(slot, "%s", QD_TEXT(QAPI.result_error(out)));
        QAPI.destroy_result(out);
        return q_err(QE_DUCKDB);
    }
    return NULL;
}

ray_t* qd_run(int slot, const char* sql, duck_result* out) {
    return qd_run2(slot, sql, out, 1);
}

ray_t* qd_exec_stmt(int slot, const char* sql) {
    duck_result res;
    ray_t* e = qd_run(slot, sql, &res);
    if (e) return e;
    QAPI.destroy_result(&res);
    return NULL;
}

static void qd_rollback(int slot) {
    duck_result res;
    if (QAPI.query(g_cons[slot].con, "ROLLBACK", &res) == QDuckSuccess)
        QAPI.destroy_result(&res);
    else
        QAPI.destroy_result(&res);
}
static int qd_sym_text(ray_t* x, char* dst, size_t cap) {
    if (!x || x->type != -RAY_SYM) return 0;
    ray_t* s = ray_sym_str(x->i64);   /* borrowed */
    if (!s) return 0;
    size_t n = ray_str_len(s);
    if (n + 1 > cap) return 0;
    memcpy(dst, ray_str_ptr(s), n);
    dst[n] = '\0';
    return 1;
}




/* .duckdb.connect `:default: | `:path.duckdb | (`:path; configDict) */
static ray_t* qd_connect_fn(ray_t* x) {
    qd_load();
    if (g_qd.state != 1) return q_err(QE_DUCKDB);

    ray_t* spec = x;
    ray_t* cfg_dict = NULL;
    if (x && x->type == RAY_LIST && x->len == 2) {
        spec     = ((ray_t**)ray_data(x))[0];
        cfg_dict = ((ray_t**)ray_data(x))[1];
        if (cfg_dict && cfg_dict->type != RAY_DICT) return q_err(QE_DUCKDB);
    }
    char text[512];
    if (!spec || RAY_IS_NULL(spec))            /* connect[] -> `:default: */
        snprintf(text, sizeof text, ":default:");
    else if (!qd_sym_text(spec, text, sizeof text)) return q_err(QE_DUCKDB);

    /* `:default: -> "" (the shared in-memory db); strip one leading colon */
    const char* path = text;
    char norm[512];
    if (strcmp(text, ":default:") == 0) norm[0] = '\0';
    else {
        if (path[0] == ':') path++;
        if (!*path) return q_err(QE_DUCKDB);
        snprintf(norm, sizeof norm, "%s", path);
    }

    /* connection slot FIRST: capacity failure must not open/cache a db (codex P2) */
    int slot = -1;
    for (int i = 0; i < QD_MAX_CON; i++) if (!g_cons[i].live) { slot = i; break; }
    if (slot < 0) return q_err(QE_DUCKDB);

    /* db cache by normalized path; config on an already-open db -> error */
    int ds = -1;
    for (int i = 0; i < QD_MAX_DB; i++)
        if (g_dbs[i].used && strcmp(g_dbs[i].path, norm) == 0) { ds = i; break; }
    if (ds >= 0 && cfg_dict && ray_dict_len(cfg_dict) > 0)
        return q_err(QE_DUCKDB);
    if (ds < 0) {
        for (int i = 0; i < QD_MAX_DB; i++) if (!g_dbs[i].used) { ds = i; break; }
        if (ds < 0) return q_err(QE_DUCKDB);

        duck_config cfg = NULL;
        if (cfg_dict && ray_dict_len(cfg_dict) > 0) {
            if (QAPI.create_config(&cfg) != QDuckSuccess) return q_err(QE_DUCKDB);
            ray_t* keys = ray_dict_keys(cfg_dict);   /* borrowed */
            ray_t* vals = ray_dict_vals(cfg_dict);   /* borrowed */
            int64_t np = ray_dict_len(cfg_dict);
            for (int64_t i = 0; i < np; i++) {
                char kbuf[128], vbuf[256];
                kbuf[0] = vbuf[0] = '\0';
                if (keys->type == RAY_SYM) {
                    ray_t* ks = ray_sym_vec_cell(keys, i);
                    if (ks) snprintf(kbuf, sizeof kbuf, "%.*s",
                                     (int)ray_str_len(ks), ray_str_ptr(ks));
                }
                if (!kbuf[0]) {
                    QAPI.destroy_config(&cfg);
                    return q_err(QE_DUCKDB);
                }
                ray_t* iv = ray_i64(i);
                ray_t* v = ray_at_fn(vals, iv);
                ray_release(iv);
                if (v) {
                    const char* tp; int64_t tn;
                    if (q_str_text_bytes(v, &tp, &tn))
                        snprintf(vbuf, sizeof vbuf, "%.*s", (int)tn, tp);
                    else if (v->type == -RAY_SYM) {
                        ray_t* vs = ray_sym_str(v->i64);
                        if (vs) snprintf(vbuf, sizeof vbuf, "%.*s",
                                         (int)ray_str_len(vs), ray_str_ptr(vs));
                    }
                    else if (v->type == -RAY_I64)  snprintf(vbuf, sizeof vbuf, "%lld", (long long)v->i64);
                    else if (v->type == -RAY_I32)  snprintf(vbuf, sizeof vbuf, "%d", v->i32);
                    else if (v->type == -RAY_BOOL) snprintf(vbuf, sizeof vbuf, "%s", v->b8 ? "true" : "false");
                    else if (v->type == -RAY_F64)  snprintf(vbuf, sizeof vbuf, "%g", v->f64);
                    ray_release(v);
                }
                if (QAPI.set_config(cfg, kbuf, vbuf) != QDuckSuccess) {
                    QAPI.destroy_config(&cfg);
                    return q_err(QE_DUCKDB);
                }
            }
        }
        char* open_err = NULL;
        duck_state st = QAPI.open_ext(norm[0] ? norm : NULL, &g_dbs[ds].db, cfg, &open_err);
        if (cfg) QAPI.destroy_config(&cfg);
        if (st != QDuckSuccess) {   /* connect-time failure: connection-less stash */
            qd_err_stash(-1, "%s", QD_TEXT(open_err));
            if (open_err) QAPI.duck_free(open_err);
            return q_err(QE_DUCKDB);
        }
        if (open_err) QAPI.duck_free(open_err);
        snprintf(g_dbs[ds].path, sizeof g_dbs[ds].path, "%s", norm);
        g_dbs[ds].refs = 0;
        g_dbs[ds].used = true;
    }

    if (QAPI.connect(g_dbs[ds].db, &g_cons[slot].con) != QDuckSuccess) {
        if (g_dbs[ds].refs == 0) { QAPI.close(&g_dbs[ds].db); g_dbs[ds].used = false; }
        return q_err(QE_DUCKDB);
    }
    g_dbs[ds].refs++;
    g_cons[slot].dbslot = ds;
    g_cons[slot].live   = true;
    snprintf(g_cons[slot].display, sizeof g_cons[slot].display, "%s", text);
    return ray_i32(qd_handle_of(slot));
}

static ray_t* qd_close_fn(ray_t* x) {
    if (g_qd.state != 1) return q_err(QE_DUCKDB);
    int slot = qd_resolve(x);
    if (slot < 0) return q_err(QE_DUCKDB);
    qd_close_slot(slot);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* .duckdb.i.err[] — the last message behind a 'duckdb, any connection; [c] — that connection's */
static ray_t* qd_err_fn(ray_t* x) {
    const char* m = g_err_last;
    if (x && !RAY_IS_NULL(x)) {
        int slot = qd_resolve(x);
        if (slot < 0) return q_err(QE_DUCKDB);
        m = g_cons[slot].err;
    }
    return ray_charv(m, (int64_t)strlen(m));
}

/* .duckdb.i.meta[h;`t] — kdb-exact ([c] t;f;a): t is the char of the column a
 * fetch would yield (catalog row + descriptor refinement, the get law); f/a
 * are uniformly the null sym — DuckDB has no fkey domain or attr concept. */
static ray_t* qd_meta_wrap(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    if (g_qd.state != 1) return q_err(QE_DUCKDB);
    int slot = qd_resolve(args[0]);
    if (slot < 0) return q_err(QE_DUCKDB);
    char tname[256];
    if (!qd_sym_text(args[1], tname, sizeof tname)) return q_err(QE_DUCKDB);

    qd_desc_t desc[256];
    int64_t ndesc = qd_fetch_desc(slot, tname, desc, 256);

    qd_buf b = {0};
    qd_puts(&b, "SELECT column_name, data_type FROM duckdb_columns() "
                "WHERE table_name = ");
    qd_put_strlit(&b, tname, strlen(tname));
    qd_puts(&b, " ORDER BY column_index");
    if (b.oom) { qd_buf_free(&b); return q_err(QE_WSFULL); }

    duck_result res;
    ray_t* e = qd_run(slot, b.p, &res);
    qd_buf_free(&b);
    if (e) return e;
    ray_t* cat = qd_result_to_table(slot, &res, NULL, 0);
    QAPI.destroy_result(&res);
    if (!cat || RAY_IS_ERR(cat)) return cat;

    int64_t nrows = ray_table_nrows(cat);
    if (nrows == 0 || nrows > 256) {
        ray_release(cat);
        return q_err(QE_DUCKDB);
    }
    ray_t* names  = ray_table_get_col_idx(cat, 0);  /* borrowed text col */
    ray_t* dtypes = ray_table_get_col_idx(cat, 1);

    ray_t* cvec = ray_sym_vec_new(RAY_SYM_W64, nrows);
    ray_t* fvec = ray_sym_vec_new(RAY_SYM_W64, nrows);
    ray_t* avec = ray_sym_vec_new(RAY_SYM_W64, nrows);
    char tbuf[256];
    int64_t blank = ray_sym_intern_runtime("", 0);
    for (int64_t i = 0; i < nrows; i++) {
        size_t ln = 0, dl = 0;
        const char* nm = qd_text_cell(names, i, &ln);
        const char* dt = qd_text_cell(dtypes, i, &dl);
        char cname[256];
        snprintf(cname, sizeof cname, "%.*s", (int)ln, nm ? nm : "");
        qd_colmap_t cm;
        bool ok = qd_catalog_col(dt ? dt : "", dl, &cm);
        if (ok) qd_refine(cname, desc, ndesc, &cm);
        tbuf[i] = ok ? qd_meta_char(&cm) : ' ';
        int64_t id = ray_sym_intern_runtime(nm ? nm : "", ln);
        cvec = ray_vec_append(cvec, &id);
        fvec = ray_vec_append(fvec, &blank);
        avec = ray_vec_append(avec, &blank);
    }
    ray_release(cat);
    ray_t* tvec = ray_charv(tbuf, nrows);
    if (!cvec || RAY_IS_ERR(cvec) || !fvec || RAY_IS_ERR(fvec) ||
        !avec || RAY_IS_ERR(avec) || !tvec || RAY_IS_ERR(tvec)) {
        if (cvec && !RAY_IS_ERR(cvec)) ray_release(cvec);
        if (fvec && !RAY_IS_ERR(fvec)) ray_release(fvec);
        if (avec && !RAY_IS_ERR(avec)) ray_release(avec);
        if (tvec && !RAY_IS_ERR(tvec)) ray_release(tvec);
        return q_err(QE_WSFULL);
    }
    return q_table_meta_assemble(cvec, tvec, fvec, avec);
}
/* set body over the flattened table: DDL + data + descriptor, ONE transaction. */
static ray_t* qd_set_impl(int slot, const char* tname, ray_t* tbl,
                          const bool* iskey, ray_t* const* masks) {
    qd_colmap_t tms[256];
    ray_t* e = qd_check_table(tbl, tms);
    if (e) return e;

    qd_buf b = {0};
    qd_create_ddl(&b, tname, tbl, tms);
    if (b.oom) { qd_buf_free(&b); return q_err(QE_WSFULL); }

    if ((e = qd_exec_stmt(slot, "BEGIN TRANSACTION"))) { qd_buf_free(&b); return e; }
    if ((e = qd_exec_stmt(slot, b.p))) { qd_buf_free(&b); qd_rollback(slot); return e; }
    qd_buf_free(&b);
    if ((e = qd_append_table(slot, tname, tbl, tms, masks))) { qd_rollback(slot); return e; }
    if ((e = qd_write_desc(slot, tname, tbl, tms, iskey))) { qd_rollback(slot); return e; }
    if ((e = qd_exec_stmt(slot, "COMMIT"))) { qd_rollback(slot); return e; }
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}
/* .duckdb.set[h;`t;tbl] — create-or-replace; a keyed table flattens
 * key-columns-first with iskey recorded (NO DuckDB PRIMARY KEY). */
static ray_t* qd_set_wrap(ray_t** args, int64_t n) {
    if (n != 3) return q_err(QE_RANK);
    if (g_qd.state != 1) return q_err(QE_DUCKDB);
    int slot = qd_resolve(args[0]);
    if (slot < 0) return q_err(QE_DUCKDB);
    char tname[256];
    if (!qd_sym_text(args[1], tname, sizeof tname)) return q_err(QE_DUCKDB);
    if (qd_reserved_name(tname)) return q_err(QE_DUCKDB);

    ray_t* tbl = args[2];
    bool   iskey[256] = { false };
    ray_t* flat = NULL;
    if (tbl && tbl->type == RAY_DICT) {
        ray_t* kt = ray_dict_keys(tbl);   /* borrowed */
        ray_t* vt = ray_dict_vals(tbl);   /* borrowed */
        if (kt && vt && kt->type == RAY_TABLE && vt->type == RAY_TABLE) {
            int64_t nk = ray_table_ncols(kt), nv = ray_table_ncols(vt);
            if (nk + nv > 256) return q_err(QE_DUCKDB);
            flat = ray_table_new(nk + nv);
            for (int64_t c = 0; c < nk; c++) {
                iskey[c] = true;
                flat = ray_table_add_col(flat, ray_table_col_name(kt, c),
                                         ray_table_get_col_idx(kt, c));  /* retains */
            }
            for (int64_t c = 0; c < nv; c++)
                flat = ray_table_add_col(flat, ray_table_col_name(vt, c),
                                         ray_table_get_col_idx(vt, c));
            if (!flat || RAY_IS_ERR(flat))
                return flat ? flat : q_err(QE_WSFULL);
            tbl = flat;
        }
    }
    if (!tbl || tbl->type != RAY_TABLE) {
        if (flat) ray_release(flat);
        return q_err(QE_DUCKDB);
    }
    ray_t* masks[256];
    ray_t* store = qd_strip_companions(slot, tbl, masks, iskey);
    ray_t* r = RAY_IS_ERR(store) ? store : qd_set_impl(slot, tname, store, iskey, masks);
    if (!RAY_IS_ERR(store)) ray_release(store);
    if (flat) ray_release(flat);
    return r;
}

/* .duckdb.get[h;`t] — whole-table read, refined by its _q_schema rows. */
static ray_t* qd_get_wrap(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    if (g_qd.state != 1) return q_err(QE_DUCKDB);
    int slot = qd_resolve(args[0]);
    if (slot < 0) return q_err(QE_DUCKDB);
    char tname[256];
    if (!qd_sym_text(args[1], tname, sizeof tname)) return q_err(QE_DUCKDB);
    qd_desc_t desc[256];
    int64_t ndesc = qd_fetch_desc(slot, tname, desc, 256);
    qd_buf b = {0};
    qd_puts(&b, "SELECT * FROM ");
    qd_put_ident(&b, tname, strlen(tname));
    if (b.oom) { qd_buf_free(&b); return q_err(QE_WSFULL); }
    duck_result res;
    ray_t* e = qd_run(slot, b.p, &res);
    qd_buf_free(&b);
    if (e) return e;
    ray_t* tbl = qd_result_to_table(slot, &res, desc, ndesc);
    QAPI.destroy_result(&res);
    return qd_rekey(tbl, desc, ndesc);
}
/* .duckdb.append[h;`t;tbl] — blind appender write, schema-checked, atomic. */
static ray_t* qd_append_wrap(ray_t** args, int64_t n) {
    if (n != 3) return q_err(QE_RANK);
    if (g_qd.state != 1) return q_err(QE_DUCKDB);
    int slot = qd_resolve(args[0]);
    if (slot < 0) return q_err(QE_DUCKDB);
    char tname[256];
    if (!qd_sym_text(args[1], tname, sizeof tname)) return q_err(QE_DUCKDB);
    if (qd_reserved_name(tname)) return q_err(QE_DUCKDB);
    ray_t* tbl = args[2];
    if (!tbl || tbl->type != RAY_TABLE) return q_err(QE_DUCKDB);

    ray_t* masks[256];
    ray_t* store = qd_strip_companions(slot, tbl, masks, NULL);
    if (RAY_IS_ERR(store)) return store;
    qd_colmap_t tms[256];
    ray_t* e = qd_check_table(store, tms);
    if (!e) e = qd_schema_check(slot, tname, store, tms);
    if (!e) e = qd_exec_stmt(slot, "BEGIN TRANSACTION");
    if (!e && (e = qd_append_table(slot, tname, store, tms, masks))) qd_rollback(slot);
    if (!e && (e = qd_exec_stmt(slot, "COMMIT"))) qd_rollback(slot);
    ray_release(store);
    if (e) return e;
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}



/* .duckdb.sql[h;"..."] — run any statement (python-API parity): a result WITH
 * columns converts per the default read mapping (no sidecar; unsupported
 * column types error as ever); a column-less result (DDL) answers `::`. */
static ray_t* qd_sql_wrap(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    if (g_qd.state != 1) return q_err(QE_DUCKDB);
    int slot = qd_resolve(args[0]);
    if (slot < 0) return q_err(QE_DUCKDB);
    const char* tp; int64_t tn;
    if (!args[1] || !q_str_text_bytes(args[1], &tp, &tn)) return q_err(QE_DUCKDB);
    qd_buf b = {0};
    qd_putn(&b, tp, (size_t)tn);
    if (b.oom) { qd_buf_free(&b); return q_err(QE_WSFULL); }
    duck_result res;
    ray_t* e = qd_run(slot, b.p, &res);
    qd_buf_free(&b);
    if (e) return e;
    ray_t* out;
    if (QAPI.column_count(&res) == 0) { ray_retain(RAY_NULL_OBJ); out = RAY_NULL_OBJ; }
    else out = qd_result_to_table(slot, &res, NULL, 0);
    QAPI.destroy_result(&res);
    return out;
}

/* ---- registration: dotted env binds (the .Q.c.* pattern) ---- */

static void qd_bind_unary(const char* name, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(name, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

static void qd_bind_vary(const char* name, ray_vary_fn fn) {
    ray_t* obj = ray_fn_vary(name, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

/* .duckdb.i.types[] — QD_TYPES[] projected as a q table `dtype`ktype`logical`canon.
 * The C array is the ONE home of the mapping (append-only contract, fidelity spec
 * 2026-07-14); q-side consumers DERIVE from this table, never re-author it.  Every
 * row is included, ' '-meta rows too — filtering is the consumer's business. */
static ray_t* qd_types_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 1) return q_err(QE_RANK);
    char kt[QD_NTYPES];
    ray_t* dt = ray_list_new((int64_t)QD_NTYPES);
    if (RAY_IS_ERR(dt)) return dt;
    ray_t* lg = ray_list_new((int64_t)QD_NTYPES);
    if (RAY_IS_ERR(lg)) { ray_release(dt); return lg; }
    ray_t* cn = ray_list_new((int64_t)QD_NTYPES);
    if (RAY_IS_ERR(cn)) { ray_release(dt); ray_release(lg); return cn; }
    for (size_t i = 0; i < QD_NTYPES; i++) {
        const qd_tmap_t* r = &QD_TYPES[i];
        kt[i] = r->meta_ch;
        ray_t* a = ray_sym(ray_sym_intern_runtime(r->sql, strlen(r->sql)));
        dt = ray_list_append(dt, a);
        ray_release(a);
        a = ray_sym(ray_sym_intern_runtime(r->logical, strlen(r->logical)));
        lg = ray_list_append(lg, a);
        ray_release(a);
        a = ray_bool(r->read_canon);
        cn = ray_list_append(cn, a);
        ray_release(a);
        if (RAY_IS_ERR(dt) || RAY_IS_ERR(lg) || RAY_IS_ERR(cn)) break;
    }
    ray_t* bad = RAY_IS_ERR(dt) ? dt : RAY_IS_ERR(lg) ? lg : RAY_IS_ERR(cn) ? cn : NULL;
    if (!bad) {
        ray_t* v = q_list_collapse(dt);
        ray_release(dt);
        dt = v;
        v = q_list_collapse(lg);
        ray_release(lg);
        lg = v;
        v = q_list_collapse(cn);
        ray_release(cn);
        cn = v;
        if (!dt || !lg || !cn) bad = q_err(QE_OOM);
        else bad = RAY_IS_ERR(dt) ? dt : RAY_IS_ERR(lg) ? lg : RAY_IS_ERR(cn) ? cn : NULL;
    }
    ray_t* ktv = bad ? NULL : ray_charv(kt, (int64_t)QD_NTYPES);
    if (!bad && RAY_IS_ERR(ktv)) bad = ktv;
    ray_t* tbl = bad ? NULL : ray_table_new(4);
    if (!bad && RAY_IS_ERR(tbl)) bad = tbl;
    if (!bad) {
        tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("dtype", 5), dt);
        if (!RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("ktype", 5), ktv);
        if (!RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("logical", 7), lg);
        if (!RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("canon", 5), cn);
        if (RAY_IS_ERR(tbl)) bad = tbl;
    }
    if (dt && dt != bad) ray_release(dt);
    if (lg && lg != bad) ray_release(lg);
    if (cn && cn != bad) ray_release(cn);
    if (ktv && ktv != bad) ray_release(ktv);
    return bad ? bad : tbl;
}

/* The INTERNAL native surface (`.duckdb.i.*`) the lib/duckdb.q provider hooks
 * are written over — the connection, the typed round-trip and one raw exec.
 * The public bespoke API (connect/sql/select/connections/version) was
 * REPLACED 2026-08-07 by the `:pq:duckdb:` virtual-table surface, not wrapped;
 * `.duckdb.err[]`, the message channel behind the bare 'duckdb, is the reader kept.
 * Registration fires from the `\l pq` gate, so the pre-gate env is kdb-clean. */
void q_duckdb_register(void) {
    /* NO dlopen here — the library is resolved lazily on first use. */
    qd_bind_unary(".duckdb.i.open",   qd_connect_fn);
    qd_bind_unary(".duckdb.i.close",  qd_close_fn);
    qd_bind_unary(".duckdb.i.err",    qd_err_fn);
    qd_bind_vary (".duckdb.i.exec",   qd_sql_wrap);
    qd_bind_vary (".duckdb.i.get",    qd_get_wrap);
    qd_bind_vary (".duckdb.i.set",    qd_set_wrap);
    qd_bind_vary (".duckdb.i.append", qd_append_wrap);
    qd_bind_vary (".duckdb.i.meta",   qd_meta_wrap);
    qd_bind_vary (".duckdb.i.types",  qd_types_fn);
}
