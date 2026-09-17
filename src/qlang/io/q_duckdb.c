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
#include "qlang/q_count.h"
#include "qlang/io/q_duckdb.h"
#include "qlang/io/q_duckdb_api.h"
#include "qlang/io/q_duckdb_internal.h"
#include "qlang/io/q_duckdb_types.h"
#include "qlang/io/q_exedir.h"
#include "qlang/io/q_provider.h"  /* q_provider_token — the alias handle behind a public verb's c; _register_internal — main's row */
#include "qlang/base/q_err.h"
#include "qlang/q_env.h"
#include "qlang/q_dotz.h"     /* q_dotz_now_ns — the sqllog clock */
#include "qlang/q_prim.h"     /* q_str_text_bytes (write-path text cells) + q_table_meta_assemble */
#include "lang/env.h"         /* ray_fn_unary / ray_fn_vary */
#include "lang/eval.h"        /* RAY_FN_NONE, ray_at_fn */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_call_sym — the .duckdb.onsql hook */
#include "table/sym.h"        /* ray_sym_vec_cell */
#include <rayforce.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

void q_duckdb_putn(qd_buf* b, const char* s, size_t n) {
    if (!qd_buf_reserve(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void q_duckdb_puts(qd_buf* b, const char* s) { q_duckdb_putn(b, s, strlen(s)); }

/* SQL identifier: "name" with embedded " doubled. */
void q_duckdb_put_ident(qd_buf* b, const char* s, size_t n) {
    q_duckdb_putn(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"') q_duckdb_putn(b, "\"\"", 2);
        else             q_duckdb_putn(b, s + i, 1);
    }
    q_duckdb_putn(b, "\"", 1);
}

/* SQL string literal: 'text' with embedded ' doubled. */
void q_duckdb_put_strlit(qd_buf* b, const char* s, size_t n) {
    q_duckdb_putn(b, "'", 1);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\'') q_duckdb_putn(b, "''", 2);
        else              q_duckdb_putn(b, s + i, 1);
    }
    q_duckdb_putn(b, "'", 1);
}

void q_duckdb_buf_free(qd_buf* b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

/* an owned NUL-terminated copy of n bytes; NULL on OOM */
char* q_duckdb_text(const char* s, size_t n) {
    char* p = malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = '\0'; }
    return p;
}

/* one zeroed block of n * per bytes for a call's per-column arrays (never NULL for n = 0); NULL on OOM */
void* q_duckdb_cols(int64_t n, size_t per) { return calloc(n > 0 ? (size_t)n : 1, per); }

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
    "duckdb_list_vector_get_size",
    "duckdb_list_vector_reserve",
    "duckdb_list_vector_set_size",
    "duckdb_logical_type_get_alias",
    "duckdb_decimal_width",
    "duckdb_decimal_scale",
    "duckdb_array_type_child_type",
    "duckdb_array_type_array_size",
    "duckdb_enum_dictionary_size",
    "duckdb_enum_dictionary_value",
    "duckdb_vector_get_column_type",
    "duckdb_enum_internal_type",
    "duckdb_array_vector_get_child",
    "duckdb_result_return_type",
    "duckdb_decimal_internal_type",
    "duckdb_struct_type_child_count",
    "duckdb_struct_type_child_name",
    "duckdb_struct_type_child_type",
    "duckdb_struct_vector_get_child",
    "duckdb_create_struct_type",
    "duckdb_map_type_key_type",
    "duckdb_map_type_value_type",
    "duckdb_create_map_type",
    "duckdb_union_type_member_count",
    "duckdb_union_type_member_name",
    "duckdb_union_type_member_type",
    "duckdb_create_union_type",
    "duckdb_row_count",
    "duckdb_value_varchar",
    "duckdb_value_is_null",
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
 * SUCCESS latches: a failed attempt retries at the next door, so fixing the
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

#define QD_MAX_CAT   32
#define QD_MAX_CON   64
#define QD_SLOT_BITS 6           /* low 6 bits = slot (matches QD_MAX_CON) */
#define QD_TOKEN_MARK (1 << 30)  /* on every token, so no token can spell a provider fd (a small positive int) */
#define QD_MAIN_ALIAS "main"
#define QD_LINK_MARK  "peachq link"   /* the COMMENT a link view carries: what tells it from a user's view of that name */

/* ONE database per process (ADR 2026-09-15 § Main instance): in-memory, or the file `-duckdb`/PEACHQ_DUCKDB_MAIN
 * names.  Opened on first need, when it also takes its own connection — the visible handle `:pq:duckdb:main, on
 * which every ATTACH/DETACH/SET and every link view runs — and registers that handle with the host. */
static struct {
    duck_database db;
    char          catalog[256];  /* current_database() of a fresh connection: the file stem, or memory */
    int           slot;
    int64_t       handle;        /* the sym id the host registered */
    bool          open;
} g_main;

static char g_main_path[512];    /* the `-duckdb` path; "" = the env var, else in-memory */

/* Every alias is an ATTACHed catalog NAMED BY THE ALIAS and shared by path: a second alias on the same file would
 * be DuckDB's "Unique file handle conflict", so it USEs the first's catalog and the DETACH waits for the last
 * connection (a DETACH from any connection kills the catalog for all).  Path "" is `:default:`, the shared
 * in-memory catalog "default"; a one-shot on a file attaches under a generated name and detaches on the way out. */
static struct {
    char path[512];              /* as given, normalized only for `:default:` */
    char name[256];
    int  refs;
    bool used;
} g_cats[QD_MAX_CAT];

static struct {
    duck_connection con;
    int             cat;         /* the catalog this connection USEs; -1 = main's own */
    uint32_t        gen;         /* bumped on close — stale handles error */
    bool            live;
    char            err[1024];   /* last message behind a 'duckdb here: DuckDB's text or the bridge's reason */
} g_cons[QD_MAX_CON];

duck_connection q_duckdb_con(int slot) { return g_cons[slot].con; }

/* most recent captured text, any connection — .duckdb.err[] reads this */
static char g_err_last[1024];

/* THE message channel behind the bare 'duckdb: a line per failure, so a cleanup's own failure never hides the cause */
void q_duckdb_err_stash(int slot, const char* fmt, ...) {
    size_t n = strlen(g_err_last);
    if (n && n + 1 < sizeof g_err_last) g_err_last[n++] = '\n';
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err_last + n, sizeof g_err_last - n, fmt, ap);
    va_end(ap);
    if (slot >= 0) memcpy(g_cons[slot].err, g_err_last, sizeof g_err_last);
}

/* a PROBE whose failure the caller drops leaves no line: mark the channel before it, rewind when its error is freed */
size_t q_duckdb_err_mark(void) { return strlen(g_err_last); }
void   q_duckdb_err_rewind(int slot, size_t mark) {
    g_err_last[mark] = '\0';
    if (slot >= 0) memcpy(g_cons[slot].err, g_err_last, sizeof g_err_last);
}

static int32_t qd_handle_of(int slot) {
    return (int32_t)(QD_TOKEN_MARK | (g_cons[slot].gen << QD_SLOT_BITS) | (uint32_t)slot);
}

/* a public verb's c: the alias handle hopen answered (or its legacy int) resolves through the host to the token;
 * a bare token (what .duckdb.main[] answers) decodes directly — the two int spaces are disjoint by the mark, so
 * neither can be mistaken for the other.  -1 = no live connection. */
static int qd_resolve(ray_t* h) {
    int64_t v;
    ray_t* tok = q_provider_token(h, "duckdb");
    if (tok) h = tok;
    if (h && h->type == -RAY_I32)      v = h->i32;
    else if (h && h->type == -RAY_I64) v = h->i64;
    else return -1;
    if (v < 0 || !(v & QD_TOKEN_MARK)) return -1;
    v &= ~(int64_t)QD_TOKEN_MARK;
    int slot = (int)(v & ((1 << QD_SLOT_BITS) - 1));
    uint32_t gen = (uint32_t)(v >> QD_SLOT_BITS);
    if (slot >= QD_MAX_CON || !g_cons[slot].live || g_cons[slot].gen != gen) return -1;
    return slot;
}

static void qd_load(void);

/* THE prologue of every public door: the channel is cleared FIRST, so err[] answers for this call alone; then the
 * library, the arity and (slot given) the handle are checked, each refusal stashed */
static ray_t* qd_door(ray_t** args, int64_t n, int64_t want, int* slot) {
    g_err_last[0] = '\0';
    if (n != want) return q_err(QE_RANK);
    qd_load();
    if (g_qd.state != 1) return q_duckdb_fail(-1, "bridge", "duckdb library not loaded");
    if (!slot) return NULL;
    *slot = qd_resolve(args[0]);
    if (*slot < 0) return q_duckdb_fail(-1, "handle", "not an open .duckdb connection");
    g_cons[*slot].err[0] = '\0';
    return NULL;
}

/* ---- .duckdb.onsql: every statement the bridge issued, one at a time ---- */

/* Every SQL statement (q_duckdb_run2) and the appender's non-SQL batch reaches the hook, so a refused write shows
 * the CAST statement that was actually attempted.  The LOG itself is q's: `.duckdb.sqllog` is an ordinary table and
 * `lib/duckdb.q` owns the insert and the cap, so the schema and the retention are readable and changeable there. */

enum { QD_L_TIME, QD_L_DUR, QD_L_OK, QD_L_CONN, QD_L_ROWS, QD_L_SQL, QD_L_ERR, QD_L_NCOL };

/* `.duckdb.onsql` — the per-statement hook, fired from the one place the ring is fed, so a handler sees every
 * statement the bridge issues and not only the ones a public door names.  Unbound costs one borrowed lookup and
 * builds nothing.  A handler that signals is swallowed (the statement already ran) and a nested fire is skipped:
 * the shape `.z.vs` uses at q_view.c.  It fires even with the ring off, so `.duckdb.sqllogmax:0` is a valid way to
 * route the log entirely into q. */
static int g_in_onsql;

static void sqllog_fire(int32_t conn, const char* sql, int64_t t, int64_t dur, bool ok, int64_t rows,
                        const char* err) {
    if (g_in_onsql) return;
    int64_t hook = ray_sym_intern(".duckdb.onsql", 13);
    if (!q_env_get(hook)) return;                                        /* unbound: build nothing */
    static const char* const NAMES[QD_L_NCOL] = { "time", "dur", "ok", "conn", "rows", "sql", "err" };
    uint8_t okb = ok ? 1 : 0;
    ray_t* val[QD_L_NCOL];
    val[QD_L_TIME] = ray_timestamp(t);
    val[QD_L_DUR]  = ray_timespan(dur);
    val[QD_L_OK]   = ray_bool(okb);
    val[QD_L_CONN] = ray_i32(conn);
    val[QD_L_ROWS] = ray_i64(rows);
    val[QD_L_SQL]  = ray_charv(sql ? sql : "", sql ? (int64_t)strlen(sql) : 0);
    val[QD_L_ERR]  = ray_charv(err ? err : "", err ? (int64_t)strlen(err) : 0);
    ray_t* k = ray_list_new(QD_L_NCOL);
    ray_t* v = ray_list_new(QD_L_NCOL);
    bool bad = !k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v);
    for (int c = 0; c < QD_L_NCOL; c++) {
        if (!val[c] || RAY_IS_ERR(val[c])) bad = true;
        if (bad) continue;
        ray_t* key = ray_sym(ray_sym_intern_runtime(NAMES[c], strlen(NAMES[c])));
        if (!key || RAY_IS_ERR(key)) { q_duckdb_drop(key); bad = true; continue; }
        k = ray_list_append(k, key);
        v = ray_list_append(v, val[c]);
        ray_release(key);
        bad |= !k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v);
    }
    for (int c = 0; c < QD_L_NCOL; c++) q_duckdb_drop(val[c]);
    ray_t* kc = bad ? NULL : q_list_collapse(k);                         /* borrows k, answers owned */
    q_duckdb_drop(k);
    bool built = kc && !RAY_IS_ERR(kc) && v && !RAY_IS_ERR(v);
    ray_t* rec = built ? ray_dict_new(kc, v) : NULL;                     /* consumes both */
    if (!built) { q_duckdb_drop(kc); q_duckdb_drop(v); return; }
    if (!rec || RAY_IS_ERR(rec)) { q_duckdb_drop(rec); return; }
    g_in_onsql = 1;
    /* a handler that re-enters the bridge would clear the channel mid-call: the door that fired it keeps it */
    char keep[2][sizeof g_err_last];
    int  slot = conn == NULL_I32 ? -1 : (int)(conn & ((1 << QD_SLOT_BITS) - 1));
    memcpy(keep[0], g_err_last, sizeof g_err_last);
    if (slot >= 0) memcpy(keep[1], g_cons[slot].err, sizeof g_err_last);
    ray_t* r = q_eval_apply_call_sym(hook, &rec, 1);
    if (r && RAY_IS_ERR(r)) { q_err_drop(); ray_error_free(r); }
    else if (r) ray_release(r);
    memcpy(g_err_last, keep[0], sizeof g_err_last);
    if (slot >= 0) memcpy(g_cons[slot].err, keep[1], sizeof g_err_last);
    g_in_onsql = 0;
    ray_release(rec);
}

void q_duckdb_stmt_note(int slot, const char* sql, int64_t t0, bool ok, int64_t rows, const char* err) {
    sqllog_fire(slot >= 0 ? qd_handle_of(slot) : NULL_I32, sql, t0, q_dotz_now_ns(0) - t0, ok, rows, err);
}

void q_duckdb_reset(void) {
    if (g_qd.state != 1) return;
    for (int i = 0; i < QD_MAX_CON; i++) if (g_cons[i].live) QAPI.disconnect(&g_cons[i].con);
    if (g_main.open) QAPI.close(&g_main.db);
    /* no q value outlives its runtime, so generations restart at 0 — the
     * next runtime (suite) sees deterministic handles (codex P1) */
    memset(g_cons, 0, sizeof g_cons);
    memset(g_cats, 0, sizeof g_cats);
    memset(&g_main, 0, sizeof g_main);
    g_err_last[0] = '\0';
}

void q_duckdb_main_path_set(const char* path) { snprintf(g_main_path, sizeof g_main_path, "%s", path ? path : ""); }
/* the bridge's own refusal: the reason to the message channel, the bare class to the caller */
ray_t* q_duckdb_fail(int slot, const char* what, const char* why) {
    q_duckdb_err_stash(slot, "%s: %s", what, why);
    return q_err(QE_DUCKDB);
}
/* Run one statement; error => owned 'duckdb (result destroyed), else fills
 * *out (caller destroys).  stash=0: internal probes never pollute err[]. */
ray_t* q_duckdb_run2(int slot, const char* sql, duck_result* out, int stash) {
    int64_t t0 = q_dotz_now_ns(0);
    if (QAPI.query(g_cons[slot].con, sql, out) != QDuckSuccess) {
        const char* m = QD_TEXT(QAPI.result_error(out));
        q_duckdb_stmt_note(slot, sql, t0, false, NULL_I64, m);
        if (stash) q_duckdb_err_stash(slot, "%s", m);
        QAPI.destroy_result(out);
        return q_err(QE_DUCKDB);
    }
    /* rows stays unknown here: duckdb_rows_changed forces the LEGACY materialisation, after which the caller's
     * duckdb_fetch_chunk reads nothing back off this result. */
    q_duckdb_stmt_note(slot, sql, t0, true, NULL_I64, NULL);
    return NULL;
}

/* the connection's last stashed message — what the appender's log row reports; g_err_last with no connection */
const char* q_duckdb_err_text(int slot) { return slot >= 0 ? g_cons[slot].err : g_err_last; }

ray_t* q_duckdb_run(int slot, const char* sql, duck_result* out) {
    return q_duckdb_run2(slot, sql, out, 1);
}

ray_t* q_duckdb_exec_stmt(int slot, const char* sql) {
    duck_result res;
    ray_t* e = q_duckdb_run(slot, sql, &res);
    if (e) return e;
    QAPI.destroy_result(&res);
    return NULL;
}

/* the refusal already owns the channel's first line; a rollback that fails too is the line after it */
static void qd_rollback(int slot) {
    duck_result res;
    ray_t* e = q_duckdb_run(slot, "ROLLBACK", &res);
    if (e) q_duckdb_drop(e);
    else   QAPI.destroy_result(&res);
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




/* A symbol argument through THE name grammar (q_duckdb_schema.c): the parts every door then spells from. */
static ray_t* qd_name_arg(int slot, ray_t* x, qd_name_t* out) {
    char t[256];
    if (!qd_sym_text(x, t, sizeof t)) return q_duckdb_fail(slot, "table name", "a symbol of at most 255 bytes");
    if (!q_duckdb_schema_name_parse(t, strlen(t), out))
        return q_duckdb_fail(slot, "table name", "quoted or bare identifier parts joined by dots, and nothing else");
    q_duckdb_schema_name_resolve(slot, out);
    return NULL;
}

/* .duckdb.i.qname `t — the name as SQL, so a q-side door spells it exactly as the C doors do (lib/duckdb.q). */
static ray_t* qd_qname_fn(ray_t* x) {
    char t[256];
    qd_name_t name;
    if (!qd_sym_text(x, t, sizeof t)) return q_err(QE_DUCKDB);
    if (!q_duckdb_schema_name_parse(t, strlen(t), &name)) return q_err(QE_DUCKDB);
    qd_buf b = {0};
    q_duckdb_schema_put_name(&b, &name);
    ray_t* r = b.oom ? q_err(QE_WSFULL) : ray_charv(b.p, (int64_t)b.len);
    q_duckdb_buf_free(&b);
    return r;
}

/* ---- the instance: main, the catalogs, the connections ---- */

static int qd_free_slot(void) {
    for (int i = 0; i < QD_MAX_CON; i++) if (!g_cons[i].live) return i;
    return -1;
}

/* THE config every open of main starts from.  A fat install ships `extensions/` beside `q` (the release's
 * `<dir>/v<ver>/<platform>/<name>.duckdb_extension` tree): that directory becomes DuckDB's extension_directory and
 * autoinstall goes off, so LOAD/autoload find the bundle and never reach for the network.  No such directory (a thin
 * install) leaves both at DuckDB's defaults.  The caller's own keys land after these, so they win. */
static duck_state qd_config_new(duck_config* out) {
    duck_state st = QAPI.create_config(out);
    if (st != QDuckSuccess) return st;
    char exe[512], dir[600];
    struct stat sb;
    if (!q_exedir(exe, sizeof exe)) return st;
    snprintf(dir, sizeof dir, "%s/extensions", exe);
    if (stat(dir, &sb) != 0 || !S_ISDIR(sb.st_mode)) return st;
    QAPI.set_config(*out, "extension_directory", dir);
    QAPI.set_config(*out, "autoinstall_known_extensions", "false");
    return st;
}

/* Open main: the file `-duckdb` named, else PEACHQ_DUCKDB_MAIN, else in-memory; cfg (consumed) carries the SET
 * keys of the open that creates it, so an open-only setting reaches the one place DuckDB takes it. */
static ray_t* qd_main_open(duck_config cfg) {
    const char* path = g_main_path[0] ? g_main_path : getenv("PEACHQ_DUCKDB_MAIN");
    if (path && !*path) path = NULL;
    if (!cfg && qd_config_new(&cfg) != QDuckSuccess) cfg = NULL;
    int slot = qd_free_slot();
    if (slot < 0) { if (cfg) QAPI.destroy_config(&cfg); return q_duckdb_fail(-1, "open", "every connection slot is live"); }
    char* open_err = NULL;
    duck_state st = QAPI.open_ext(path, &g_main.db, cfg, &open_err);
    if (cfg) QAPI.destroy_config(&cfg);
    if (st != QDuckSuccess) {
        q_duckdb_err_stash(-1, "%s", QD_TEXT(open_err));
        if (open_err) QAPI.duck_free(open_err);
        return q_err(QE_DUCKDB);
    }
    if (open_err) QAPI.duck_free(open_err);
    if (QAPI.connect(g_main.db, &g_cons[slot].con) != QDuckSuccess) {
        QAPI.close(&g_main.db);
        return q_duckdb_fail(-1, "open", "duckdb_connect failed");
    }
    g_cons[slot].cat  = -1;
    g_cons[slot].live = true;
    g_main.slot = slot;
    g_main.open = true;
    duck_result res;
    ray_t* e = q_duckdb_run2(slot, "SELECT current_database() AS d", &res, 0);
    if (!e) {
        ray_t* row = q_duckdb_codec_result_to_table(slot, &res, NULL, 0, NULL);
        QAPI.destroy_result(&res);
        if (row && !RAY_IS_ERR(row) && q_count(row) == 1) {
            size_t ln = 0;
            const char* d = q_duckdb_schema_text_cell(ray_table_get_col_idx(row, 0), 0, &ln);
            snprintf(g_main.catalog, sizeof g_main.catalog, "%.*s", (int)(d ? ln : 0), d ? d : "");
        }
        q_duckdb_drop(row);
    } else q_duckdb_drop(e);
    ray_t* tok = ray_i32(qd_handle_of(slot));
    g_main.handle = q_provider_register_internal("duckdb", QD_MAIN_ALIAS, tok);
    ray_release(tok);
    if (g_main.handle) return NULL;
    QAPI.disconnect(&g_cons[slot].con);
    QAPI.close(&g_main.db);
    memset(&g_cons[slot], 0, sizeof g_cons[slot]);
    memset(&g_main, 0, sizeof g_main);
    return q_duckdb_fail(-1, "open", "the host could not register the main handle");
}

static ray_t* qd_main_need(void) { return g_main.open ? NULL : qd_main_open(NULL); }

/* the ATTACH options (docs: sql/statements/attach), case-insensitive; every other config key is a SET on main */
static bool qd_attach_option(const char* k) {
    static const char* const OPTS[] = { "READ_ONLY", "COMPRESS", "TYPE", "DEFAULT_TABLE", "BLOCK_SIZE", "ROW_GROUP_SIZE",
                                        "STORAGE_VERSION", "ENCRYPTION_KEY", "ENCRYPTION_CIPHER", "RECOVERY_MODE" };
    for (size_t i = 0; i < sizeof OPTS / sizeof *OPTS; i++) if (strcasecmp(k, OPTS[i]) == 0) return true;
    return false;
}

/* text and symbols are quoted in SQL, numbers and booleans bare; the config API takes every value as text */
static bool qd_cfg_text(ray_t* v, char* out, size_t cap, bool* quote) {
    const char* tp; int64_t tn;
    *quote = true;
    if (!v) return false;
    if (q_str_text_bytes(v, &tp, &tn)) { snprintf(out, cap, "%.*s", (int)tn, tp); return true; }
    if (v->type == -RAY_SYM) {
        ray_t* vs = ray_sym_str(v->i64);
        if (!vs) return false;
        snprintf(out, cap, "%.*s", (int)ray_str_len(vs), ray_str_ptr(vs));
        return true;
    }
    *quote = false;
    if (v->type == -RAY_I64)       snprintf(out, cap, "%lld", (long long)v->i64);
    else if (v->type == -RAY_I32)  snprintf(out, cap, "%d", v->i32);
    else if (v->type == -RAY_BOOL) snprintf(out, cap, "%s", v->b8 ? "true" : "false");
    else if (v->type == -RAY_F64)  snprintf(out, cap, "%g", v->f64);
    else return false;
    return true;
}

static void qd_cfg_put(qd_buf* b, const char* t, bool quote) {
    if (quote) q_duckdb_put_strlit(b, t, strlen(t));
    else       q_duckdb_puts(b, t);
}

/* The config dict split by key (ADR: DuckDB has no per-database settings): ATTACH options into *attach as the
 * parenthesised clause, every other key SET on main — through the open config when this open creates main, else
 * as SET statements, so an open-only setting gets DuckDB's own refusal.  Owned error, else NULL. */
static ray_t* qd_config_apply(ray_t* cfg, qd_buf* attach) {
    ray_t* keys = ray_dict_keys(cfg);   /* borrowed */
    ray_t* vals = ray_dict_vals(cfg);   /* borrowed */
    int64_t np = q_count(cfg);
    if (keys->type != RAY_SYM && np) return q_duckdb_fail(-1, "open", "a config key is not a symbol");
    duck_config oc = NULL;
    for (int64_t i = 0; i < np; i++) {
        ray_t* ks = ray_sym_vec_cell(keys, i);
        char k[128], t[256];
        bool quote;
        snprintf(k, sizeof k, "%.*s", ks ? (int)ray_str_len(ks) : 0, ks ? ray_str_ptr(ks) : "");
        ray_t* iv = ray_i64(i);
        ray_t* v = ray_at_fn(vals, iv);
        ray_release(iv);
        bool ok = qd_cfg_text(v, t, sizeof t, &quote);
        if (v) ray_release(v);
        ray_t* e = NULL;
        if (!ok) e = q_duckdb_fail(-1, k, "a config value must be text, a symbol, a number or a boolean");
        else if (qd_attach_option(k)) {
            q_duckdb_puts(attach, attach->len ? ", " : "(");
            q_duckdb_puts(attach, k);
            q_duckdb_puts(attach, " ");
            qd_cfg_put(attach, t, quote);
        } else if (!g_main.open) {
            if (!oc && qd_config_new(&oc) != QDuckSuccess) e = q_duckdb_fail(-1, "open", "duckdb_create_config failed");
            else if (QAPI.set_config(oc, k, t) != QDuckSuccess) e = q_duckdb_fail(-1, k, "not a DuckDB config option");
        } else {
            qd_buf sb = {0};
            q_duckdb_puts(&sb, "SET ");
            q_duckdb_put_ident(&sb, k, strlen(k));
            q_duckdb_puts(&sb, " = ");
            qd_cfg_put(&sb, t, quote);
            e = sb.oom ? q_err(QE_WSFULL) : q_duckdb_exec_stmt(g_main.slot, sb.p);
            q_duckdb_buf_free(&sb);
        }
        if (e) { if (oc) QAPI.destroy_config(&oc); return e; }
    }
    if (attach->len) q_duckdb_puts(attach, ")");
    if (attach->oom) { if (oc) QAPI.destroy_config(&oc); return q_err(QE_WSFULL); }
    return g_main.open ? NULL : qd_main_open(oc);
}

static int qd_cat_find(const char* path) {
    for (int i = 0; i < QD_MAX_CAT; i++) if (g_cats[i].used && strcmp(g_cats[i].path, path) == 0) return i;
    return -1;
}

static int qd_cat_attach(const char* path, const char* name, const char* opts) {
    int c = -1;
    for (int i = 0; i < QD_MAX_CAT; i++) if (!g_cats[i].used) { c = i; break; }
    if (c < 0) { q_duckdb_err_stash(-1, "open: every catalog slot is live"); return -1; }
    qd_buf b = {0};
    q_duckdb_puts(&b, "ATTACH ");
    q_duckdb_put_strlit(&b, path[0] ? path : ":memory:", strlen(path[0] ? path : ":memory:"));
    q_duckdb_puts(&b, " AS ");
    q_duckdb_put_ident(&b, name, strlen(name));
    if (opts[0]) { q_duckdb_puts(&b, " "); q_duckdb_puts(&b, opts); }
    ray_t* e = b.oom ? q_err(QE_WSFULL) : q_duckdb_exec_stmt(g_main.slot, b.p);
    q_duckdb_buf_free(&b);
    if (e) { q_duckdb_drop(e); return -1; }
    snprintf(g_cats[c].path, sizeof g_cats[c].path, "%s", path);
    snprintf(g_cats[c].name, sizeof g_cats[c].name, "%s", name);
    g_cats[c].refs = 0;
    g_cats[c].used = true;
    return c;
}

static void qd_cat_release(int c) {
    if (c < 0 || !g_cats[c].used || --g_cats[c].refs > 0) return;
    qd_buf b = {0};
    q_duckdb_puts(&b, "DETACH ");
    q_duckdb_put_ident(&b, g_cats[c].name, strlen(g_cats[c].name));
    ray_t* e = b.oom ? NULL : q_duckdb_exec_stmt(g_main.slot, b.p);
    if (e) q_duckdb_drop(e);
    q_duckdb_buf_free(&b);
    g_cats[c].used = false;
}

/* Close one live slot: disconnect, and the catalog it used goes with its last connection.  Main's own is closed
 * only by q_duckdb_reset. */
static void qd_close_slot(int slot) {
    if (!g_cons[slot].live || (g_main.open && slot == g_main.slot)) return;
    QAPI.disconnect(&g_cons[slot].con);
    g_cons[slot].live = false;
    g_cons[slot].gen++;
    qd_cat_release(g_cons[slot].cat);
}

/* .duckdb.i.open[alias; rest; timeout; config] — the host's open hook.  rest is the coordinate's config text: the
 * db path, "default:" (or empty) = the shared in-memory catalog; timeout means nothing to an in-process engine;
 * config is :: or the option dict (qd_config_apply).  The alias names the catalog the path is attached under; the
 * empty alias is a one-shot, served under a generated name unless the path is already attached.  Answers the TOKEN. */
static ray_t* qd_open_wrap(ray_t** args, int64_t n) {
    ray_t* e = qd_door(args, n, 4, NULL);
    if (e) return e;
    char alias[256];
    const char* tp; int64_t tn;
    if (!qd_sym_text(args[0], alias, sizeof alias)) return q_duckdb_fail(-1, "open", "alias is not a symbol");
    if (strcmp(alias, QD_MAIN_ALIAS) == 0) return q_err(QE_DOMAIN);
    if (!q_str_text_bytes(args[1], &tp, &tn)) return q_duckdb_fail(-1, "open", "path is not text");
    if (tn >= 512) return q_duckdb_fail(-1, "open", "path is longer than 511 bytes");
    ray_t* cfg = args[3] && !RAY_IS_NULL(args[3]) ? args[3] : NULL;
    if (cfg && cfg->type != RAY_DICT) return q_duckdb_fail(-1, "open", "config is not a dict");
    char path[512];
    if (tn == 0 || (tn == 8 && memcmp(tp, "default:", 8) == 0)) path[0] = '\0';
    else snprintf(path, sizeof path, "%.*s", (int)tn, tp);

    qd_buf opts = {0};
    e = cfg ? qd_config_apply(cfg, &opts) : qd_main_need();
    if (e) { q_duckdb_buf_free(&opts); return e; }
    /* connection slot FIRST: capacity failure must not attach a catalog (codex P2) */
    int slot = qd_free_slot();
    if (slot < 0) { q_duckdb_buf_free(&opts); return q_duckdb_fail(-1, "open", "every connection slot is live"); }
    int cat = qd_cat_find(path);
    if (cat >= 0 && opts.len) { q_duckdb_buf_free(&opts); return q_duckdb_fail(-1, "open", "ATTACH options on a catalog that is already attached"); }
    if (cat < 0) {
        static unsigned oneshot;
        char name[256];
        if (!path[0]) snprintf(name, sizeof name, "default");
        else if (alias[0]) snprintf(name, sizeof name, "%s", alias);
        else snprintf(name, sizeof name, "_t%u", ++oneshot);
        cat = qd_cat_attach(path, name, opts.len ? opts.p : "");
    }
    q_duckdb_buf_free(&opts);
    if (cat < 0) return q_err(QE_DUCKDB);

    if (QAPI.connect(g_main.db, &g_cons[slot].con) != QDuckSuccess) {
        qd_cat_release(cat);
        return q_duckdb_fail(-1, "open", "duckdb_connect failed");
    }
    g_cats[cat].refs++;
    g_cons[slot].cat  = cat;
    g_cons[slot].live = true;
    qd_buf b = {0};
    q_duckdb_puts(&b, "USE ");
    q_duckdb_put_ident(&b, g_cats[cat].name, strlen(g_cats[cat].name));
    e = b.oom ? q_err(QE_WSFULL) : q_duckdb_exec_stmt(slot, b.p);
    q_duckdb_buf_free(&b);
    if (e) { qd_close_slot(slot); return e; }
    return ray_i32(qd_handle_of(slot));
}

static ray_t* qd_close_fn(ray_t* x) {
    int slot;
    ray_t* e = qd_door(&x, 1, 1, &slot);
    if (e) return e;
    if (g_main.open && slot == g_main.slot) return q_err(QE_DOMAIN);
    qd_close_slot(slot);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* .duckdb.i.main[] — the handle; a native rather than a q-cached token so the first call is what opens main */
static ray_t* qd_main_fn(ray_t** args, int64_t n) {
    ray_t* e = qd_door(args, n, 1, NULL);
    if (!e) e = qd_main_need();
    return e ? e : ray_sym(g_main.handle);
}

/* .duckdb.i.link[token; qname; table] — the q global qname, bound to the table the token's connection sees, becomes
 * the same-named VIEW in main's own catalog: CREATE OR REPLACE VIEW "qname" AS SELECT * FROM "cat"."schema"."table",
 * marked by its COMMENT so unlink and the loader can tell it from a user's view of that name.  A view, never a
 * copy; once the catalog detaches it errors on use, as the pointer's own get does.  A pointer into main's own
 * catalog under the global's own name already IS that name there: nothing to make (the view would collide with the
 * table it names; DuckDB folds identifier case). */
static ray_t* qd_link_wrap(ray_t** args, int64_t n) {
    int slot;
    qd_name_t name;
    char qn[256];
    ray_t* e = qd_door(args, n, 3, &slot);
    if (!e && !qd_sym_text(args[1], qn, sizeof qn)) e = q_duckdb_fail(slot, "link", "qname is not a symbol");
    if (!e) e = qd_name_arg(slot, args[2], &name);
    if (e) return e;
    const char* cat = g_cons[slot].cat >= 0 ? g_cats[g_cons[slot].cat].name : g_main.catalog;
    const char* schema = name.schema[0] ? name.schema : "main";
    if (g_cons[slot].cat < 0 && strcasecmp(qn, name.part[name.n - 1]) == 0) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    qd_buf b = {0};
    q_duckdb_puts(&b, "CREATE OR REPLACE VIEW ");
    q_duckdb_put_ident(&b, qn, strlen(qn));
    q_duckdb_puts(&b, " AS SELECT * FROM ");
    q_duckdb_put_ident(&b, cat, strlen(cat));
    q_duckdb_puts(&b, ".");
    q_duckdb_put_ident(&b, schema, strlen(schema));
    q_duckdb_puts(&b, ".");
    q_duckdb_put_ident(&b, name.part[name.n - 1], strlen(name.part[name.n - 1]));
    e = b.oom ? q_err(QE_WSFULL) : q_duckdb_exec_stmt(g_main.slot, b.p);
    q_duckdb_buf_free(&b);
    if (e) return e;
    q_duckdb_puts(&b, "COMMENT ON VIEW ");
    q_duckdb_put_ident(&b, qn, strlen(qn));
    q_duckdb_puts(&b, " IS '" QD_LINK_MARK "'");
    e = b.oom ? q_err(QE_WSFULL) : q_duckdb_exec_stmt(g_main.slot, b.p);
    q_duckdb_buf_free(&b);
    if (e) return e;
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* .duckdb.i.unlink[token; qname] — the token is not looked at: the view drops by name after its alias is closed too;
 * the marker probe is what keeps a self-pointer symmetric and a user's own view of that name standing */
static ray_t* qd_unlink_wrap(ray_t** args, int64_t n) {
    char qn[256];
    ray_t* e = qd_door(args, n, 2, NULL);
    if (e) return e;
    if (!qd_sym_text(args[1], qn, sizeof qn)) return q_duckdb_fail(-1, "unlink", "qname is not a symbol");
    if (!g_main.open) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    qd_buf b = {0};
    q_duckdb_puts(&b, "SELECT view_name FROM duckdb_views() WHERE NOT internal AND database_name = current_database() "
                      "AND comment = '" QD_LINK_MARK "' AND view_name = ");
    q_duckdb_put_strlit(&b, qn, strlen(qn));
    duck_result res;
    e = b.oom ? q_err(QE_WSFULL) : q_duckdb_run2(g_main.slot, b.p, &res, 0);
    q_duckdb_buf_free(&b);
    if (e) return e;
    ray_t* hit = q_duckdb_codec_result_to_table(g_main.slot, &res, NULL, 0, NULL);
    QAPI.destroy_result(&res);
    bool is_view = hit && !RAY_IS_ERR(hit) && q_count(hit) == 1;
    q_duckdb_drop(hit);
    if (is_view) {
        q_duckdb_puts(&b, "DROP VIEW ");
        q_duckdb_put_ident(&b, qn, strlen(qn));
        e = b.oom ? q_err(QE_WSFULL) : q_duckdb_exec_stmt(g_main.slot, b.p);
        q_duckdb_buf_free(&b);
        if (e) return e;
    }
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* .duckdb.i.hdel[c; t] — drop the OBJECT t names (a view when that is what it is, else a table) and, with a table,
 * its sidecar rows: the one key law spells them, so nothing of a dropped table outlives it in _q_schema. */
static ray_t* qd_hdel_wrap(ray_t** args, int64_t n) {
    int slot;
    qd_name_t name;
    ray_t* e = qd_door(args, n, 2, &slot);
    if (!e) e = qd_name_arg(slot, args[1], &name);
    if (e) return e;
    qd_buf b = {0};
    q_duckdb_puts(&b, "SELECT 1 AS v FROM duckdb_views() WHERE NOT internal AND database_name = current_database() "
                      "AND view_name = ");
    q_duckdb_put_strlit(&b, name.part[name.n - 1], strlen(name.part[name.n - 1]));
    duck_result res;
    e = b.oom ? q_err(QE_WSFULL) : q_duckdb_run(slot, b.p, &res);
    q_duckdb_buf_free(&b);
    if (e) return e;
    ray_t* hit = q_duckdb_codec_result_to_table(slot, &res, NULL, 0, NULL);
    QAPI.destroy_result(&res);
    bool is_view = hit && !RAY_IS_ERR(hit) && q_count(hit) > 0;
    q_duckdb_drop(hit);
    q_duckdb_puts(&b, is_view ? "DROP VIEW " : "DROP TABLE ");
    q_duckdb_schema_put_name(&b, &name);
    e = b.oom ? q_err(QE_WSFULL) : q_duckdb_exec_stmt(slot, b.p);
    q_duckdb_buf_free(&b);
    if (e) return e;
    if (!is_view) q_duckdb_schema_drop_desc(slot, &name);
    ray_retain(args[1]);
    return args[1];
}

/* .duckdb.i.tables[c] — the LOADABLE names of the connection's catalog, sorted: the tables and views of its current
 * schema (a bare name binds there) less the bridge's own (the _q_schema sidecar; the link views, each already a q
 * name).  What .duckdb.load[h;::] binds. */
static ray_t* qd_tables_fn(ray_t* x) {
    int slot;
    ray_t* e = qd_door(&x, 1, 1, &slot);
    if (e) return e;
    duck_result res;
    e = q_duckdb_run2(slot, "SELECT table_name AS n FROM duckdb_tables() WHERE NOT internal AND database_name = current_database() "
                            "AND schema_name = current_schema() UNION ALL SELECT view_name FROM duckdb_views() WHERE NOT internal "
                            "AND database_name = current_database() AND schema_name = current_schema() AND coalesce(comment, '') <> '"
                            QD_LINK_MARK "' ORDER BY 1", &res, 0);
    if (e) return e;
    ray_t* rows = q_duckdb_codec_result_to_table(slot, &res, NULL, 0, NULL);
    QAPI.destroy_result(&res);
    if (!rows || RAY_IS_ERR(rows)) return rows ? rows : q_err(QE_WSFULL);
    int64_t n = q_count(rows);
    ray_t* col = ray_table_get_col_idx(rows, 0);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    for (int64_t i = 0; i < n && out && !RAY_IS_ERR(out); i++) {
        size_t ln = 0;
        const char* t = q_duckdb_schema_text_cell(col, i, &ln);
        qd_name_t nm;
        if (!t || !q_duckdb_schema_name_parse(t, ln, &nm) || q_duckdb_schema_reserved_name(&nm)) continue;
        int64_t id = ray_sym_intern_runtime(t, ln);
        out = ray_vec_append(out, &id);
    }
    q_duckdb_drop(rows);
    return out;
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
    int slot;
    qd_name_t name;
    ray_t* ne = qd_door(args, n, 2, &slot);
    if (!ne) ne = qd_name_arg(slot, args[1], &name);
    if (ne) return ne;

    qd_desc_t* desc  = NULL;
    int64_t    ndesc = q_duckdb_schema_fetch_desc(slot, &name, &desc);

    qd_buf b = {0};
    q_duckdb_puts(&b, "SELECT column_name, data_type FROM duckdb_columns() WHERE ");
    q_duckdb_schema_put_catalog_where(&b, &name);
    q_duckdb_puts(&b, " ORDER BY column_index");
    duck_result res;
    ray_t* cat = b.oom ? q_err(QE_WSFULL) : q_duckdb_run(slot, b.p, &res);
    q_duckdb_buf_free(&b);
    if (!cat) {
        cat = q_duckdb_codec_result_to_table(slot, &res, NULL, 0, NULL);
        QAPI.destroy_result(&res);
    }
    if (!cat || RAY_IS_ERR(cat) || q_count(cat) == 0) {
        q_duckdb_schema_desc_free(desc, ndesc);
        if (cat && !RAY_IS_ERR(cat)) {
            ray_release(cat);
            char what[300];
            snprintf(what, sizeof what, "table %s", name.part[name.n - 1]);
            cat = q_duckdb_fail(slot, what, "not in the catalog");
        }
        return cat;
    }

    int64_t nrows = q_count(cat);
    ray_t* names  = ray_table_get_col_idx(cat, 0);  /* borrowed text col */
    ray_t* dtypes = ray_table_get_col_idx(cat, 1);

    ray_t* cvec = ray_sym_vec_new(RAY_SYM_W64, nrows);
    ray_t* fvec = ray_sym_vec_new(RAY_SYM_W64, nrows);
    ray_t* avec = ray_sym_vec_new(RAY_SYM_W64, nrows);
    char*  tbuf = malloc(nrows > 0 ? (size_t)nrows : 1);
    bool   oom  = !tbuf;
    int64_t blank = ray_sym_intern_runtime("", 0);
    for (int64_t i = 0; i < nrows; i++) {
        size_t ln = 0, dl = 0;
        const char* nm = q_duckdb_schema_text_cell(names, i, &ln);
        const char* dt = q_duckdb_schema_text_cell(dtypes, i, &dl);
        qd_buf cn = {0};
        q_duckdb_putn(&cn, nm ? nm : "", ln);
        qd_colmap_t cm;
        bool ok = !cn.oom && q_duckdb_schema_catalog_col(dt ? dt : "", dl, &cm);
        if (ok) q_duckdb_codec_refine(QD_TEXT(cn.p), desc, ndesc, &cm);
        oom |= cn.oom;
        q_duckdb_buf_free(&cn);
        if (tbuf) tbuf[i] = ok ? q_duckdb_codec_meta_char(&cm) : ' ';
        int64_t id = ray_sym_intern_runtime(nm ? nm : "", ln);
        cvec = ray_vec_append(cvec, &id);
        fvec = ray_vec_append(fvec, &blank);
        avec = ray_vec_append(avec, &blank);
    }
    ray_release(cat);
    q_duckdb_schema_desc_free(desc, ndesc);
    ray_t* tvec = tbuf ? ray_charv(tbuf, nrows) : NULL;
    free(tbuf);
    if (oom || !cvec || RAY_IS_ERR(cvec) || !fvec || RAY_IS_ERR(fvec) ||
        !avec || RAY_IS_ERR(avec) || !tvec || RAY_IS_ERR(tvec)) {
        q_duckdb_drop(cvec);
        q_duckdb_drop(fvec);
        q_duckdb_drop(avec);
        q_duckdb_drop(tvec);
        return q_err(QE_WSFULL);
    }
    return q_table_meta_assemble(cvec, tvec, fvec, avec);
}
/* The write door's value is a bare table (keyed or not) or the envelope pair (data;schema): the data half comes
 * back in *data, the schema half parsed into decl (ADR 9a); anything else is 'duckdb, reason stashed. */
static ray_t* qd_envelope_split(int slot, ray_t* x, ray_t** data, qd_desc_t** decl, int64_t* ndecl, bool* bare) {
    *data  = x;
    *decl  = NULL;
    *ndecl = 0;
    *bare  = !x || x->type != RAY_LIST;
    if (*bare) return NULL;
    if (q_count(x) != 2) return q_duckdb_fail(slot, "envelope", "expected (data;schema)");
    *data = ((ray_t**)ray_data(x))[0];
    return q_duckdb_schema_desc_parse(slot, ((ray_t**)ray_data(x))[1], decl, ndecl);
}

/* The data write, inside the caller's transaction: straight through the appender, or — a declared type or a long
 * companion in play — staged in a temp table in the q-derived types (the raw/hi longs as columns of their own),
 * held to the exactness law, and moved by ONE statement whose CASTs DuckDB judges exact-else-error; create =
 * CREATE OR REPLACE the target (set), else INSERT INTO it (append). */
static ray_t* qd_write_rows(int slot, const qd_name_t* nm, ray_t* tbl, const qd_colmap_t* tms,
                            ray_t* const* masks, ray_t* const* offs, ray_t* const* keeps,
                            const char* const* dtypes, bool create) {
    int64_t ncols = ray_table_ncols(tbl);
    bool cast = false;
    for (int64_t c = 0; c < ncols; c++)
        cast |= dtypes[c] != NULL || offs[c] != NULL || q_duckdb_codec_mask_kind(&tms[c], masks[c]) >= QD_CO_RAW ||
                q_duckdb_codec_keep_kind(&tms[c], keeps[c]) >= QD_CO_RAW;
    qd_name_t stage;
    q_duckdb_schema_name_parse(QD_STAGE_TBL, strlen(QD_STAGE_TBL), &stage);
    const qd_name_t* target = cast ? &stage : nm;
    /* the image's maps, then its companions — a column and, behind it, at most three per column (ADR 16/21) */
    qd_colmap_t* scms   = cast ? q_duckdb_cols(4 * ncols, sizeof *scms + 2 * sizeof(ray_t*)) : NULL;
    ray_t**      smasks = scms ? (ray_t**)(scms + 4 * ncols) : NULL;
    ray_t**      skeeps = smasks ? smasks + 4 * ncols : NULL;
    if (cast && !scms) return q_err(QE_WSFULL);
    ray_t* img = cast ? q_duckdb_codec_stage_image(tbl, tms, masks, offs, keeps, scms, smasks, skeeps) : tbl;
    if (RAY_IS_ERR(img)) { free(scms); return img; }
    ray_t* e = NULL;
    qd_buf b = {0};
    if (create || cast) {
        q_duckdb_schema_create_ddl(&b, target, img, cast ? scms : tms, target->temp);
        e = b.oom ? q_err(QE_WSFULL) : q_duckdb_exec_stmt(slot, b.p);
        q_duckdb_buf_free(&b);
    }
    if (!e) e = q_duckdb_codec_append_table(slot, target, target->temp, img, cast ? scms : tms, cast ? smasks : masks,
                                           cast ? skeeps : keeps);
    if (cast) ray_release(img);
    free(scms);
    if (e || !cast) return e;
    if ((e = q_duckdb_schema_exact_check(slot, tbl, tms, dtypes, masks, offs, keeps))) return e;
    q_duckdb_puts(&b, !create ? "INSERT INTO " : nm->temp ? "CREATE OR REPLACE TEMP TABLE " : "CREATE OR REPLACE TABLE ");
    q_duckdb_schema_put_name(&b, nm);
    q_duckdb_puts(&b, create ? " AS " : " ");
    if ((e = q_duckdb_schema_cast_select(slot, &b, tbl, tms, dtypes, masks, offs, keeps, QD_STAGE_TBL)))
        { q_duckdb_buf_free(&b); return e; }
    if (b.oom) { q_duckdb_buf_free(&b); return q_err(QE_WSFULL); }
    e = q_duckdb_exec_stmt(slot, b.p);
    q_duckdb_buf_free(&b);
    if (!e) e = q_duckdb_exec_stmt(slot, "DROP TABLE \"" QD_STAGE_TBL "\"");
    return e;
}

/* The write body over the flattened table, ONE transaction: `create` lays the DDL and records the descriptor
 * (the set law), else the rows go into a table whose schema this batch was already checked against.  An enum
 * column is already its symbols (decayed at the door); `en` carries what the descriptor records of it. */
static ray_t* qd_write_impl(int slot, const qd_name_t* nm, ray_t* tbl, const bool* iskey, const qd_enum_t* en,
                            ray_t* const* masks, ray_t* const* offs, ray_t* const* keeps, qd_desc_t* decl,
                            int64_t ndecl, bool replace) {
    int64_t ncols = ray_table_ncols(tbl);
    qd_colmap_t* tms = q_duckdb_cols(ncols, sizeof *tms + sizeof(char*));   /* the maps, then the declared types */
    if (!tms) return q_err(QE_WSFULL);
    const char** dtypes = (const char**)(tms + ncols);
    bool missing = false;
    ray_t* e = q_duckdb_schema_check_names(slot, tbl);
    if (!e) e = q_duckdb_codec_check_table(slot, tbl, tms);
    if (!e) e = q_duckdb_schema_declare(slot, tbl, tms, decl, ndecl, dtypes);
    if (!e && !replace) e = q_duckdb_schema_check(slot, nm, tbl, tms, dtypes, &missing);
    bool open = false;
    if (!e && !(e = q_duckdb_exec_stmt(slot, "BEGIN TRANSACTION"))) {
        bool create = replace || missing;
        open = true;
        if (!(e = qd_write_rows(slot, nm, tbl, tms, masks, offs, keeps, dtypes, create)) &&
            (!create || !(e = q_duckdb_schema_write_desc(slot, nm, tbl, tms, iskey, en, dtypes))))
            e = q_duckdb_exec_stmt(slot, "COMMIT");
    }
    q_duckdb_codec_maps_free(tms, ncols);
    free(tms);
    if (e) { if (open) qd_rollback(slot); return e; }
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}
/* THE write door.  `.duckdb.set` create-or-replaces; `.duckdb.append` grows a table and, when there is none yet,
 * IS a set — so the first batch declares the schema (ADR 11).  A keyed table flattens key-columns-first with
 * iskey recorded (NO DuckDB PRIMARY KEY); the envelope pair declares types the q carrier alone cannot, and is
 * the ONLY value that may carry a companion. */
static ray_t* qd_write_wrap(ray_t** args, int64_t n, bool replace) {
    int slot;
    qd_name_t name;
    ray_t* e = qd_door(args, n, 3, &slot);
    if (!e) e = qd_name_arg(slot, args[1], &name);
    if (e) return e;
    if (q_duckdb_schema_reserved_name(&name)) return q_duckdb_fail(slot, "table name", "reserved for the bridge's own tables");

    ray_t*     tbl;
    qd_desc_t* decl;
    int64_t    ndecl, nk = 0;
    bool       bare;
    if ((e = qd_envelope_split(slot, args[2], &tbl, &decl, &ndecl, &bare))) return e;
    ray_t* flat = NULL;
    if (tbl && tbl->type == RAY_DICT) {
        ray_t* kt = ray_dict_keys(tbl);   /* borrowed */
        ray_t* vt = ray_dict_vals(tbl);   /* borrowed */
        if (kt && vt && kt->type == RAY_TABLE && vt->type == RAY_TABLE) {
            int64_t nv = ray_table_ncols(vt);
            nk   = ray_table_ncols(kt);
            flat = ray_table_new(nk + nv);
            for (int64_t c = 0; c < nk; c++)
                flat = ray_table_add_col(flat, ray_table_col_name(kt, c),
                                         ray_table_get_col_idx(kt, c));  /* retains */
            for (int64_t c = 0; c < nv; c++)
                flat = ray_table_add_col(flat, ray_table_col_name(vt, c),
                                         ray_table_get_col_idx(vt, c));
            if (!flat || RAY_IS_ERR(flat)) {
                q_duckdb_schema_desc_free(decl, ndecl);
                return flat ? flat : q_err(QE_WSFULL);
            }
            tbl = flat;
        }
    }
    ray_t*  r     = NULL;
    int64_t ncols = tbl && tbl->type == RAY_TABLE ? ray_table_ncols(tbl) : -1;
    /* the shape mirrors, then the zoned offsets (ADR 21), then a nested column's element companion, then what an
     * enum column was, then keyedness */
    ray_t** masks = ncols < 0 ? NULL : q_duckdb_cols(ncols, 3 * sizeof *masks + sizeof(qd_enum_t) + sizeof(bool));
    /* `([k:1 2])` reaches here as the column dict it parses to: a keyed table with no value columns is not valid q */
    if (ncols < 0) r = tbl && tbl->type == RAY_DICT && ray_dict_keys(tbl)->type == RAY_SYM &&
                       ray_dict_vals(tbl)->type == RAY_LIST
                           ? q_duckdb_fail(slot, "keyed", "a keyed table needs a value column")
                           : q_duckdb_fail(slot, "value", "not a table");
    else if (!masks) r = q_err(QE_WSFULL);
    else {
        ray_t**  offs  = masks + ncols;
        ray_t**  keeps = offs + ncols;
        qd_enum_t* en    = (qd_enum_t*)(keeps + ncols);
        bool*      iskey = (bool*)(en + ncols);
        for (int64_t c = 0; c < nk; c++) iskey[c] = true;
        /* THE one enum site: a 20h column is its symbols before anything downstream sees it (2026-09-14) */
        ray_t* syms  = q_duckdb_codec_decay_enums(slot, tbl, en);
        ray_t* store = RAY_IS_ERR(syms) ? syms
                     : q_duckdb_codec_strip_companions(slot, syms, masks, offs, keeps, iskey, en, bare);
        r = RAY_IS_ERR(store) ? store
                              : qd_write_impl(slot, &name, store, iskey, en, masks, offs, keeps, decl, ndecl, replace);
        if (!RAY_IS_ERR(store)) ray_release(store);
        if (!RAY_IS_ERR(syms) && syms != store) ray_release(syms);
        free(masks);
    }
    if (flat) ray_release(flat);
    q_duckdb_schema_desc_free(decl, ndecl);
    return r;
}

/* .duckdb.set[h;`t;tbl] and .duckdb.append[h;`t;tbl] */
static ray_t* qd_set_wrap(ray_t** args, int64_t n)    { return qd_write_wrap(args, n, true); }
static ray_t* qd_append_wrap(ray_t** args, int64_t n) { return qd_write_wrap(args, n, false); }

/* THE table reader: the whole table refined by its _q_schema rows, the envelope's schema beside it when asked. */
static ray_t* qd_read_table(int slot, const qd_name_t* nm, ray_t** schema) {
    qd_desc_t* desc  = NULL;
    int64_t    ndesc = q_duckdb_schema_fetch_desc(slot, nm, &desc);
    qd_buf b = {0};
    q_duckdb_puts(&b, "SELECT * FROM ");
    q_duckdb_schema_put_name(&b, nm);
    duck_result res;
    ray_t* tbl = b.oom ? q_err(QE_WSFULL) : q_duckdb_run(slot, b.p, &res);
    q_duckdb_buf_free(&b);
    if (!tbl) {
        tbl = q_duckdb_codec_result_to_table(slot, &res, desc, ndesc, schema);
        QAPI.destroy_result(&res);
        tbl = q_duckdb_schema_rekey(tbl, desc, ndesc);
        if (RAY_IS_ERR(tbl) && schema && *schema) { ray_release(*schema); *schema = NULL; }
    }
    q_duckdb_schema_desc_free(desc, ndesc);
    return tbl;
}

/* (data;schema) from two owned halves; an error in either propagates (a NULL schema is the empty one) */
static ray_t* qd_pair(ray_t* data, ray_t* schema) {
    if (RAY_IS_ERR(data)) { if (schema) ray_release(schema); return data; }
    if (!schema) schema = q_duckdb_schema_desc_table(NULL, 0);
    if (RAY_IS_ERR(schema)) { ray_release(data); return schema; }
    ray_t* l = ray_list_new(2);
    if (l && !RAY_IS_ERR(l)) l = ray_list_append(l, data);
    if (l && !RAY_IS_ERR(l)) l = ray_list_append(l, schema);
    ray_release(data);
    ray_release(schema);
    return l ? l : q_err(QE_WSFULL);
}

static ray_t* qd_get_any(ray_t** args, int64_t n, bool pair) {
    int slot;
    qd_name_t name;
    ray_t* e = qd_door(args, n, 2, &slot);
    if (!e) e = qd_name_arg(slot, args[1], &name);
    if (e) return e;
    ray_t* schema = NULL;
    ray_t* data = qd_read_table(slot, &name, pair ? &schema : NULL);
    return pair ? qd_pair(data, schema) : data;
}

/* .duckdb.get[h;`t] — whole-table read, refined by its _q_schema rows: the pair with its schema dropped. */
static ray_t* qd_get_wrap(ray_t** args, int64_t n) { return qd_get_any(args, n, false); }

/* .duckdb.getx[h;`t] — the envelope: (data;schema), schema = the declared types the q carriers cannot spell. */
static ray_t* qd_getx_wrap(ray_t** args, int64_t n) { return qd_get_any(args, n, true); }

/* .duckdb.exec[h;"..."] / .duckdb.execx — run any statement (python-API parity): a result WITH columns converts
 * per the default read mapping (no sidecar; unsupported column types error as ever), execx pairing it with its
 * schema; a column-less result (DDL) answers `::` (execx: beside an empty schema). */
static ray_t* qd_sql_any(ray_t** args, int64_t n, bool pair) {
    int slot;
    ray_t* e = qd_door(args, n, 2, &slot);
    if (e) return e;
    const char* tp; int64_t tn;
    if (!args[1] || !q_str_text_bytes(args[1], &tp, &tn)) return q_duckdb_fail(slot, "sql", "not a string");
    qd_buf b = {0};
    q_duckdb_putn(&b, tp, (size_t)tn);
    if (b.oom) { q_duckdb_buf_free(&b); return q_err(QE_WSFULL); }
    duck_result res;
    e = q_duckdb_run(slot, b.p, &res);
    q_duckdb_buf_free(&b);
    if (e) return e;
    ray_t* schema = NULL;
    ray_t* out = q_duckdb_codec_result_to_table(slot, &res, NULL, 0, pair ? &schema : NULL);
    QAPI.destroy_result(&res);
    return pair ? qd_pair(out, schema) : out;
}

static ray_t* qd_sql_wrap(ray_t** args, int64_t n)  { return qd_sql_any(args, n, false); }
static ray_t* qd_sqlx_wrap(ray_t** args, int64_t n) { return qd_sql_any(args, n, true); }

/* the diagnostic's own answer: a q string, never a signal — the LAYER that refused is the first word */
static ray_t* diag_text(const char* s) { return ray_charv(s, (int64_t)strlen(s)); }

/* .duckdb.unsafeExecText[c;sql] — run ANY statement and answer DuckDB's OWN text.  Cells are DuckDB's own per-cell
 * text (which reader, below), so nothing crosses the bridge's type codec, and — the reason it is not a SQL cast — a
 * value outside its own type's domain is DECLINED rather than raising the INTERNAL error that invalidates the
 * whole database.  A failure comes back AS DATA (DuckDB's message), which is what makes the refusing layer
 * obvious.  It is a DIAGNOSTIC — nothing in the bridge routes through it, and it never touches .duckdb.err[], so
 * the channel stays as the door that refused left it.  Never signals; only a wrong-arity call is still 'rank.
 * Rendering: header line of column names, then one line per row, fields joined " | ", SQL NULL as "NULL" and a
 * cell the renderer declines as "<TYPE unrenderable>"; a column-less statement (DDL) answers "".  The whole
 * result is materialised — say LIMIT if that matters. */
static ray_t* qd_unsafe_exec_text_wrap(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    if (g_qd.state != 1) return diag_text("bridge: duckdb library not loaded");
    int slot = qd_resolve(args[0]);
    if (slot < 0) return diag_text("bridge: not an open .duckdb connection");
    const char* tp;
    int64_t tn;
    if (!args[1] || !q_str_text_bytes(args[1], &tp, &tn)) return diag_text("bridge: sql must be a string");
    qd_buf s = {0};
    q_duckdb_putn(&s, tp, (size_t)tn);
    if (s.oom) { q_duckdb_buf_free(&s); return q_err(QE_WSFULL); }

    duck_result res;
    int64_t t0 = q_dotz_now_ns(0);
    if (QAPI.query(g_cons[slot].con, s.p, &res) != QDuckSuccess) {
        const char* m = QD_TEXT(QAPI.result_error(&res));
        q_duckdb_stmt_note(slot, s.p, t0, false, NULL_I64, m);
        ray_t* r = ray_charv(m, (int64_t)strlen(m));
        QAPI.destroy_result(&res);
        q_duckdb_buf_free(&s);
        return r;
    }
    int64_t dur  = q_dotz_now_ns(0) - t0;
    int64_t ncol = (int64_t)QAPI.column_count(&res);

    /* the type name per column, so an unrenderable cell can say which type defeated the renderer */
    char (*tname)[32] = ncol ? q_duckdb_cols(ncol, sizeof *tname) : NULL;
    if (ncol && !tname) { QAPI.destroy_result(&res); q_duckdb_buf_free(&s); return q_err(QE_WSFULL); }
    qd_buf b = {0};
    bool   walk = ncol > 0;
    for (int64_t c = 0; c < ncol; c++) {
        if (c) q_duckdb_puts(&b, " | ");
        q_duckdb_puts(&b, QD_TEXT(QAPI.column_name(&res, (duck_idx_t)c)));
        duck_logical_type lt = QAPI.column_logical_type(&res, (duck_idx_t)c);
        duck_type         ty = lt ? QAPI.get_type_id(lt) : 0;
        snprintf(tname[c], sizeof *tname, "%s", q_duckdb_type_name(ty));
        if (lt) QAPI.destroy_logical_type(&lt);
        walk &= ty == QDUCK_TYPE_VARCHAR;
    }
    int64_t nrow = 0;
    if (walk) {
        /* DuckDB lets one result be read ONE way: the chunk walk sees every byte of a VARCHAR (a NUL included)
         * but has no text for any other type, and the legacy per-cell text has every type but stores a VARCHAR
         * as a C string — so the walk is taken exactly when it is total for the result */
        duck_data_chunk chunk;
        while ((chunk = QAPI.fetch_chunk(res)) != NULL) {
            duck_idx_t n = QAPI.data_chunk_get_size(chunk);
            for (duck_idx_t r = 0; r < n; r++) {
                q_duckdb_puts(&b, "\n");
                for (int64_t c = 0; c < ncol; c++) {
                    if (c) q_duckdb_puts(&b, " | ");
                    duck_vector dv = QAPI.data_chunk_get_vector(chunk, (duck_idx_t)c);
                    if (!q_duckdb_validity_ok(QAPI.vector_get_validity(dv), r)) { q_duckdb_puts(&b, "NULL"); continue; }
                    const duck_string_t* v = (const duck_string_t*)QAPI.vector_get_data(dv) + r;
                    q_duckdb_putn(&b, q_duckdb_string_data(v), q_duckdb_string_len(v));
                }
            }
            nrow += (int64_t)n;
            QAPI.destroy_data_chunk(&chunk);
        }
    } else if (ncol) {
        nrow = (int64_t)QAPI.row_count(&res);
        for (int64_t r = 0; r < nrow; r++) {
            q_duckdb_puts(&b, "\n");
            for (int64_t c = 0; c < ncol; c++) {
                if (c) q_duckdb_puts(&b, " | ");
                char* v = QAPI.value_varchar(&res, (duck_idx_t)c, (duck_idx_t)r);
                /* no-text-and-not-null is the renderer giving up (every nested type, and a value outside its own
                 * domain): SAY so rather than print a NULL the reader would believe */
                if (v) q_duckdb_puts(&b, v);
                else if (QAPI.value_is_null(&res, (duck_idx_t)c, (duck_idx_t)r)) q_duckdb_puts(&b, "NULL");
                else { q_duckdb_puts(&b, "<"); q_duckdb_puts(&b, tname[c]); q_duckdb_puts(&b, " unrenderable>"); }
                if (v) QAPI.duck_free(v);
            }
        }
    }
    sqllog_fire(qd_handle_of(slot), s.p, t0, dur, true, ncol ? nrow : NULL_I64, NULL);
    q_duckdb_buf_free(&s);
    QAPI.destroy_result(&res);
    free(tname);
    ray_t* out = b.oom ? q_err(QE_WSFULL) : ray_charv(b.len ? b.p : "", (int64_t)b.len);
    q_duckdb_buf_free(&b);
    return out;
}

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

/* .duckdb.i.types[] — QD_TYPES[] projected as a q table `dtype`ktype`logical`canon`needs`companion.
 * The C array is the ONE home of the mapping (append-only contract, fidelity spec
 * 2026-07-14); q-side consumers DERIVE from this table, never re-author it.  Every
 * row is included, ' '-meta rows too — filtering is the consumer's business.  needs
 * = what an envelope row must carry for the type to survive a round trip, companion
 * = which extra column can appear beside it — both the reader's own decisions, read
 * off q_duckdb_schema_needs and q_duckdb_codec_companion_of rather than restated here. */
enum { QD_T_DTYPE, QD_T_LOGICAL, QD_T_NEEDS, QD_T_COMPANION, QD_T_NSYM };

/* n names as one symbol vector, the cell a `companion` row carries */
static ray_t* qd_sym_list(const char* const* names, int n) {
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, n ? n : 1);
    for (int i = 0; i < n && v && !RAY_IS_ERR(v); i++) {
        int64_t id = ray_sym_intern_runtime(names[i], strlen(names[i]));
        v = ray_vec_append(v, &id);
    }
    return v ? v : q_err(QE_WSFULL);
}

static ray_t* qd_types_fn(ray_t** args, int64_t n) {
    ray_t* e = qd_door(args, n, 1, NULL);
    if (e) return e;
    static const char* const NEEDS[4] = { "none", "logical", "dtype", "both" };
    ray_t* sym[QD_T_NSYM] = { NULL };
    ray_t* cn  = NULL;
    ray_t* bad = NULL;
    char   kt[QD_NTYPES];
    for (int c = 0; !bad && c < QD_T_NSYM; c++)
        if (RAY_IS_ERR(sym[c] = ray_list_new((int64_t)QD_NTYPES))) { bad = sym[c]; sym[c] = NULL; }
    if (!bad && RAY_IS_ERR(cn = ray_list_new((int64_t)QD_NTYPES))) { bad = cn; cn = NULL; }
    for (size_t i = 0; !bad && i < QD_NTYPES; i++) {
        const qd_tmap_t* r = &QD_TYPES[i];
        const qd_colmap_t cm = { r, NULL, 0, false };
        const char* co[QD_CO_MAX];
        int         nco = q_duckdb_codec_companions_of(&cm, co);
        const char* cell[QD_T_NSYM] = { r->sql, r->logical,   /* a manifest row is a TYPE, so it has no emptiness */
                                        NEEDS[q_duckdb_schema_needs(&cm, q_duckdb_type_name(r->dk_type), false) & 3],
                                        NULL };
        kt[i] = r->meta_ch;
        for (int c = 0; c < QD_T_NSYM; c++) {
            /* ADR 21: a zoned temporal grows TWO, so the cell is the LIST of them — one symbol is still one symbol */
            ray_t* a = cell[c] ? ray_sym(ray_sym_intern_runtime(cell[c], strlen(cell[c]))) : qd_sym_list(co, nco);
            sym[c] = ray_list_append(sym[c], a);
            ray_release(a);
            if (RAY_IS_ERR(sym[c])) bad = sym[c];
        }
        ray_t* b = ray_bool(r->read_canon);
        cn = ray_list_append(cn, b);
        ray_release(b);
        if (RAY_IS_ERR(cn)) bad = cn;
    }
    for (int c = 0; !bad && c <= QD_T_NSYM; c++) {
        ray_t** slot = c == QD_T_NSYM ? &cn : &sym[c];
        ray_t*  v    = q_list_collapse(*slot);
        ray_release(*slot);
        *slot = v;
        if (!v) bad = q_err(QE_OOM);
        else if (RAY_IS_ERR(v)) bad = v;
    }
    ray_t* ktv = bad ? NULL : ray_charv(kt, (int64_t)QD_NTYPES);
    if (!bad && RAY_IS_ERR(ktv)) bad = ktv;
    ray_t* tbl = bad ? NULL : ray_table_new(6);
    if (!bad && RAY_IS_ERR(tbl)) bad = tbl;
    if (!bad) {
        tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("dtype", 5), sym[QD_T_DTYPE]);
        if (!RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("ktype", 5), ktv);
        if (!RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("logical", 7), sym[QD_T_LOGICAL]);
        if (!RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("canon", 5), cn);
        if (!RAY_IS_ERR(tbl)) tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("needs", 5), sym[QD_T_NEEDS]);
        if (!RAY_IS_ERR(tbl))
            tbl = ray_table_add_col(tbl, ray_sym_intern_runtime("companion", 9), sym[QD_T_COMPANION]);
        if (RAY_IS_ERR(tbl)) bad = tbl;
    }
    for (int c = 0; c < QD_T_NSYM; c++) if (sym[c] && sym[c] != bad) ray_release(sym[c]);
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
    qd_bind_vary (".duckdb.i.open",   qd_open_wrap);
    qd_bind_unary(".duckdb.i.close",  qd_close_fn);
    qd_bind_vary (".duckdb.i.main",   qd_main_fn);
    qd_bind_vary (".duckdb.i.link",   qd_link_wrap);
    qd_bind_vary (".duckdb.i.unlink", qd_unlink_wrap);
    qd_bind_vary (".duckdb.i.hdel",   qd_hdel_wrap);
    qd_bind_unary(".duckdb.i.tables", qd_tables_fn);
    qd_bind_unary(".duckdb.i.err",    qd_err_fn);
    qd_bind_vary (".duckdb.i.exec",   qd_sql_wrap);
    qd_bind_vary (".duckdb.i.execx",  qd_sqlx_wrap);
    qd_bind_unary(".duckdb.i.qname",  qd_qname_fn);
    qd_bind_vary (".duckdb.i.get",    qd_get_wrap);
    qd_bind_vary (".duckdb.i.getx",   qd_getx_wrap);
    qd_bind_vary (".duckdb.i.set",    qd_set_wrap);
    qd_bind_vary (".duckdb.i.append", qd_append_wrap);
    qd_bind_vary (".duckdb.i.meta",   qd_meta_wrap);
    qd_bind_vary (".duckdb.i.types",  qd_types_fn);
    /* the two DIAGNOSTICS, bound under .duckdb directly: neither has q-side logic, and a shim in front of a
     * diagnostic is one more thing to keep honest.  `unsafe` marks the one that bypasses the type contract. */
    qd_bind_vary (".duckdb.i.unsafeExecText", qd_unsafe_exec_text_wrap);
}
