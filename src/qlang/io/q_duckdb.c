/* q_duckdb — peachq's `.duckdb` namespace: the DuckDB bridge.  Contract, type
 * mapping and every decision: docs/duckdb-api.md (sidecar design:
 * docs/superpowers/specs/2026-07-14-duckdb-fidelity-design.md).
 *
 * DuckDB is reached ONLY via dlopen + a dlsym'd fn table of its v2 C API
 * (q_duckdb_api.h), lazily on first use: PEACHQ_DUCKDB_LIB set = EXCLUSIVE,
 * else $QHOME -> exe dir -> system default, the last three refused before dlopen
 * on an x86-64 CPU without AVX2 (DuckDB ships x86-64-v3 binaries); a library
 * without the v2 surface is refused with the version it has; every failure is
 * the bare 'duckdb class.  Reads fetch a statement's chunks whole, writes fill
 * chunks into a column data collection one INSERT moves — both directions
 * columnar.  String columns cross the boundary as 0h lists of charv
 * (string-C3: physical RAY_STR never reaches q-space); floats follow the
 * live-infinity model (ONLY NaN is null). */
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
#include "qlang/ops/q_bang.h" /* q_bang_enkey — 0! flattens a keyed table at the write door */
#include "qlang/ops/q_index.h" /* q_index_elem_at — the one element read, a parameter value off its list */
#include <stdint.h>
#include "lang/env.h"         /* ray_fn_unary / ray_fn_vary */
#include "lang/eval.h"        /* RAY_FN_NONE, ray_at_fn */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_call_sym — the .duckdb.onsql hook */
#include "table/sym.h"        /* ray_sym_vec_cell */
#include <rayforce.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
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

/* answers whether the text was lost, so a sub-buffer hands its OOM up in the line that frees it */
int q_duckdb_buf_free(qd_buf* b) {
    int oom = b->oom;
    free(b->p);
    *b = (qd_buf){0};
    return oom;
}

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
    "duckdb_v2_environment_create",
    "duckdb_v2_environment_destroy",
    "duckdb_v2_instance_create_with_options",
    "duckdb_v2_instance_destroy",
    "duckdb_v2_instance_attach",
    "duckdb_v2_connection_create",
    "duckdb_v2_connection_destroy",
    "duckdb_v2_connection_get_factory",
    "duckdb_v2_error_info_get_text",
    "duckdb_v2_error_info_destroy",
    "duckdb_v2_parse_sql",
    "duckdb_v2_statement_iterator_next",
    "duckdb_v2_statement_iterator_destroy",
    "duckdb_v2_sql_statement_destroy",
    "duckdb_v2_sql_statement_get_parameter_count",
    "duckdb_v2_sql_statement_get_parameter_name",
    "duckdb_v2_statement_bind",
    "duckdb_v2_statement_execute",
    "duckdb_v2_result_destroy",
    "duckdb_v2_result_fetch_chunk",
    "duckdb_v2_result_get_result_type",
    "duckdb_v2_result_get_schema",
    "duckdb_v2_schema_get_count",
    "duckdb_v2_schema_get_field",
    "duckdb_v2_schema_destroy",
    "duckdb_v2_data_chunk_create",
    "duckdb_v2_data_chunk_destroy",
    "duckdb_v2_data_chunk_get_size",
    "duckdb_v2_data_chunk_get_vector",
    "duckdb_v2_vector_get_logical_type",
    "duckdb_v2_vector_flatten",
    "duckdb_v2_vector_get_view",
    "duckdb_v2_vector_get_data_mutable",
    "duckdb_v2_vector_flat_get_validity_mutable",
    "duckdb_v2_vector_get_size",
    "duckdb_v2_vector_set_size",
    "duckdb_v2_vector_get_arena",
    "duckdb_v2_arena_allocate",
    "duckdb_v2_vector_get_child",
    "duckdb_v2_vector_get_value",
    "duckdb_v2_logical_type_copy",
    "duckdb_v2_logical_type_destroy",
    "duckdb_v2_logical_type_get_id",
    "duckdb_v2_logical_type_get_name",
    "duckdb_v2_logical_type_get_param_count",
    "duckdb_v2_logical_type_get_param",
    "duckdb_v2_factory_create_type_from_id",
    "duckdb_v2_factory_create_type_from_text",
    "duckdb_v2_value_destroy",
    "duckdb_v2_value_is_null",
    "duckdb_v2_value_to_string",
    "duckdb_v2_value_get_type",
    "duckdb_v2_value_get_varchar",
    "duckdb_v2_value_get_utinyint",
    "duckdb_v2_value_get_bigint",
    "duckdb_v2_value_create_type",
    "duckdb_v2_value_create_null",
    "duckdb_v2_value_create_bool",
    "duckdb_v2_value_create_utinyint",
    "duckdb_v2_value_create_smallint",
    "duckdb_v2_value_create_int",
    "duckdb_v2_value_create_bigint",
    "duckdb_v2_value_create_float",
    "duckdb_v2_value_create_double",
    "duckdb_v2_value_create_varchar",
    "duckdb_v2_value_create_date",
    "duckdb_v2_value_create_interval",
    "duckdb_v2_value_create_timestamp",
    "duckdb_v2_value_create_timestamp_ns",
    "duckdb_v2_value_create_uuid",
    "duckdb_v2_value_create_list",
    "duckdb_v2_column_data_collection_create",
    "duckdb_v2_column_data_collection_destroy",
    "duckdb_v2_column_data_collection_append_state_create",
    "duckdb_v2_column_data_collection_append_state_destroy",
    "duckdb_v2_column_data_collection_append",
    "duckdb_v2_replacement_scan_create",
    "duckdb_v2_replacement_scan_set_callback",
    "duckdb_v2_replacement_scan_set_user_data",
    "duckdb_v2_connection_register_replacement_scan",
    "duckdb_v2_replacement_scan_destroy",
    "duckdb_v2_replacement_scan_get_user_data",
    "duckdb_v2_replacement_scan_get_name",
    "duckdb_v2_replacement_scan_set_collection",
    "duckdb_v2_qname_get_part_count",
    "duckdb_v2_qname_get_part",
    "duckdb_v2_qname_destroy",
};
#define QD_NSYMS (sizeof QD_SYMS / sizeof *QD_SYMS)

_Static_assert(sizeof(duck_api_t) == QD_NSYMS * sizeof(void*),
               "QD_SYMS[] must match duck_api_t, in order");

duck_api_t qd_api;   /* THE dlsym'd table (declared in q_duckdb_internal.h) */

static struct {
    int   state;             /* 0 = untried, 1 = loaded, 2 = failed */
    void* dl;
    char  why[160];          /* the refusal behind state 2, when the loader has one of its own */
} g_qd;

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

/* The v2 surface is the floor (docs/duckdb-architecture.md): a library missing any of it is refused, and the v1
 * `duckdb_library_version` — the one symbol every DuckDB since 0.x exports — is used only to SPELL what was found. */
static void qd_floor_refuse(void* dl) {
    const char* (*v1ver)(void) = (const char* (*)(void))(uintptr_t)qd_dlsym(dl, "duckdb_library_version");
    snprintf(g_qd.why, sizeof g_qd.why, "bridge: libduckdb 2.0 or later required (found %s)",
             v1ver ? v1ver() : "no DuckDB version");
}

/* One load attempt; PEACHQ_DUCKDB_LIB set = EXCLUSIVE (no fallback).  Only
 * SUCCESS latches: a failed attempt retries at the next door, so fixing the
 * lib path (or the env var) works without restarting q. */
static void qd_load(void) {
    if (g_qd.state == 1) return;
    g_qd.why[0] = '\0';
    void* dl = NULL;
    const char* envp = getenv("PEACHQ_DUCKDB_LIB");
    if (envp && *envp) {
        dl = qd_dlopen(envp);
        if (!dl) { g_qd.state = 2; return; }
    }
#if defined(__x86_64__) && !defined(_MSC_VER)
    /* the bundled library is DuckDB's x86-64-v3 build: without AVX2 the SIGILL is inside dlopen, before any q code.
     * An explicit PEACHQ_DUCKDB_LIB is the user's own choice of library (a baseline build, a 1.x) and loads as named. */
    if (!dl && !__builtin_cpu_supports("avx2")) {
        snprintf(g_qd.why, sizeof g_qd.why, "bridge: libduckdb needs AVX2 (x86-64-v3); this CPU lacks it");
        g_qd.state = 2;
        return;
    }
#endif
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
        if (slots[i]) continue;
        qd_floor_refuse(dl);
        g_qd.state = 2;
        memset(&qd_api, 0, sizeof qd_api);
        qd_dlclose(dl);                         /* a retry re-opens; don't stack refcounts */
        return;
    }
    g_qd.dl = dl;
    g_qd.state = 1;
}

/* DuckDB's text off a failed call, copied into buf ("" when the call left none); the handle is destroyed either way */
static const char* qd_etext(duck_err* e, char* buf, size_t cap) {
    duck_str t = { NULL, 0 };
    if (*e) QAPI.error_info_get_text(*e, &t);
    snprintf(buf, cap, "%.*s", (int)t.len, t.ptr ? t.ptr : "");
    if (*e) QAPI.error_info_destroy(e);
    return buf;
}

static duck_str qd_str(const char* s) { return (duck_str){ s, strlen(s) }; }

#define QD_MAX_CAT   32
#define QD_MAX_CON   64
#define QD_SLOT_BITS 6           /* low 6 bits = slot (matches QD_MAX_CON) */
#define QD_TOKEN_MARK (1 << 30)  /* on every token, so no token can spell a provider fd (a small positive int) */
#define QD_MAIN_ALIAS "main"
#define QD_LINK_MARK  "peachq link"   /* the COMMENT a link view carries: what tells it from a user's view of that name */

/* ONE instance per process (ADR 2026-09-15 § Main instance): in-memory, or the file `-duckdb`/PEACHQ_DUCKDB_MAIN
 * names.  Opened on first need, when it also takes its own connection — the visible handle `:pq:duckdb:main, on
 * which every ATTACH/DETACH/SET and every link view runs — and registers that handle with the host.  The environment
 * is DuckDB's root for instances; it lives exactly as long as the instance. */
static struct {
    duck_env      env;
    duck_instance inst;
    char          catalog[256];  /* current_database() of a fresh connection: the file stem, or memory */
    int           slot;
    int64_t       handle;        /* the sym id the host registered */
    bool          open;
} g_main;

static char g_main_path[512];    /* the `-duckdb` path; "" = the env var, else in-memory */

/* Every alias is an ATTACHed catalog NAMED BY THE ALIAS and shared by path: a second alias on the same file would
 * be DuckDB's "Unique file handle conflict", so it USEs the first's catalog and the DETACH waits for the last
 * connection (a DETACH from any connection kills the catalog for all).  Path "" is ":memory:", the shared
 * in-memory catalog "default". */
static struct {
    char path[512];              /* as given, normalized only for ":memory:" */
    char name[256];
    int  refs;
    bool used;
} g_cats[QD_MAX_CAT];

/* A connection and, while a write is moving rows, the collection its replacement scan serves under QD_BATCH_TBL
 * (docs/duckdb-architecture.md: a q table reaches DuckDB as a column data collection, never as text). */
static struct {
    duck_connection con;
    duck_factory    fac;         /* the connection's, borrowed: types, values, chunks and collections are made here */
    int             cat;         /* the catalog this connection USEs; -1 = main's own */
    uint32_t        gen;         /* bumped on close — stale handles error */
    bool            live;
    char            err[1024];   /* last message behind a 'duckdb here: DuckDB's text or the bridge's reason */
    duck_collection coll;
    const duck_str* coll_names;
    duck_idx_t      coll_ncols;
} g_cons[QD_MAX_CON];

duck_factory q_duckdb_factory(int slot) { return g_cons[slot].fac; }

/* most recent captured text, any connection — .duckdb.err[] reads this */
static char g_err_last[1024];

/* THE message channel behind the bare 'duckdb: a line per failure, so a cleanup's own failure never hides the cause */
static void qd_err_vstash(int slot, const char* fmt, va_list ap) {
    size_t n = strlen(g_err_last);
    if (n && n + 1 < sizeof g_err_last) g_err_last[n++] = '\n';
    vsnprintf(g_err_last + n, sizeof g_err_last - n, fmt, ap);
    if (slot >= 0) memcpy(g_cons[slot].err, g_err_last, sizeof g_err_last);
}

void q_duckdb_err_stash(int slot, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    qd_err_vstash(slot, fmt, ap);
    va_end(ap);
}

/* the bridge's own refusal, `what: why`: the reason to the message channel, the bare class to the caller */
ray_t* q_duckdb_fail(int slot, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    qd_err_vstash(slot, fmt, ap);
    va_end(ap);
    return q_err(QE_DUCKDB);
}

/* DuckDB's own refusal: its text to the channel, the handle destroyed, the bare class to the caller */
ray_t* q_duckdb_fail_err(int slot, duck_err* e) {
    char m[1024];
    return q_duckdb_fail(slot, "%s", qd_etext(e, m, sizeof m));
}

/* `column <name>: why` — the why cut at 640 bytes, the name never */
ray_t* q_duckdb_fail_col(int slot, const char* name, size_t n, const char* fmt, ...) {
    char why[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    return q_duckdb_fail(slot, "column %.*s: %s", (int)n, name, why);
}

/* a column's name as bytes (borrowed, NUL-terminated); "?" where the symbol has no text */
const char* q_duckdb_col_name(ray_t* tbl, int64_t c, size_t* len) {
    ray_t* s = ray_sym_str(ray_table_col_name(tbl, c));
    if (len) *len = s ? ray_str_len(s) : 1;
    return s ? ray_str_ptr(s) : "?";
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
    ray_t* tok = q_provider_token(h, Q_PQ_DUCKDB);
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

/* THE prologue of every public door: the channel is cleared FIRST, so err[] answers for this call alone; then the
 * library, the arity and (slot given) the handle are checked, each refusal stashed */
static ray_t* qd_door(ray_t** args, int64_t n, int64_t want, int* slot) {
    g_err_last[0] = '\0';
    if (n != want) return q_err(QE_RANK);
    qd_load();
    if (g_qd.state != 1) return q_duckdb_fail(-1, "%s", g_qd.why[0] ? g_qd.why : "bridge: duckdb library not loaded");
    if (!slot) return NULL;
    *slot = qd_resolve(args[0]);
    if (*slot < 0) return q_duckdb_fail(-1, "handle: not an open .duckdb connection");
    g_cons[*slot].err[0] = '\0';
    return NULL;
}

/* ---- .duckdb.onsql: every statement the bridge issued, one at a time ---- */

/* Every SQL statement (q_duckdb_run) reaches the hook, the row move of a write included now that it is an INSERT,
 * so a refused write shows the statement that was actually attempted.  The LOG itself is q's: `.duckdb.sqllog` is
 * an ordinary table and `.duckdb` owns the insert and the cap, so the schema and the retention are readable and
 * changeable there. */

enum { QD_L_TIME, QD_L_DUR, QD_L_OK, QD_L_CONN, QD_L_ROWS, QD_L_COLS, QD_L_SQL, QD_L_ERR, QD_L_NCOL };

/* `.duckdb.onsql` — the per-statement hook, fired from the one place the ring is fed, so a handler sees every
 * statement the bridge issues and not only the ones a public door names.  Unbound costs one borrowed lookup and
 * builds nothing.  A handler that signals is swallowed (the statement already ran) and a nested fire is skipped:
 * the shape `.z.vs` uses at q_view.c.  It fires even with the ring off, so `.duckdb.sqllogmax:0` is a valid way to
 * route the log entirely into q. */
static int g_in_onsql;

/* n names as one symbol vector: the log row's keys, the cell a `companion` row carries */
static ray_t* qd_sym_list(const char* const* names, int n) {
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, n ? n : 1);
    for (int i = 0; i < n && v && !RAY_IS_ERR(v); i++) {
        int64_t id = ray_sym_intern_runtime(names[i], strlen(names[i]));
        v = ray_vec_append(v, &id);
    }
    return v ? v : q_err(QE_WSFULL);
}

static void sqllog_fire(int32_t conn, const char* sql, int64_t t, int64_t dur, bool ok, int64_t rows,
                        int64_t cols, const char* err) {
    if (g_in_onsql) return;
    int64_t hook = ray_sym_intern(".duckdb.onsql", 13);
    if (!q_env_get(hook)) return;                                        /* unbound: build nothing */
    static const char* const NAMES[QD_L_NCOL] = { "time", "dur", "ok", "conn", "rows", "ncols", "sql", "err" };
    uint8_t okb = ok ? 1 : 0;
    ray_t* val[QD_L_NCOL];
    val[QD_L_TIME] = ray_timestamp(t);
    val[QD_L_DUR]  = ray_timespan(dur);
    val[QD_L_OK]   = ray_bool(okb);
    val[QD_L_CONN] = ray_i32(conn);
    val[QD_L_ROWS] = ray_i64(rows);
    val[QD_L_COLS] = ray_i64(cols);
    val[QD_L_SQL]  = ray_charv(sql ? sql : "", sql ? (int64_t)strlen(sql) : 0);
    val[QD_L_ERR]  = ray_charv(err ? err : "", err ? (int64_t)strlen(err) : 0);
    ray_t* k = qd_sym_list(NAMES, QD_L_NCOL);
    ray_t* v = ray_list_new(QD_L_NCOL);
    bool bad = RAY_IS_ERR(k) || !v || RAY_IS_ERR(v);
    for (int c = 0; c < QD_L_NCOL; c++) {
        bad = bad || !val[c] || RAY_IS_ERR(val[c]);
        if (!bad) v = ray_list_append(v, val[c]);
        bad = bad || !v || RAY_IS_ERR(v);
    }
    for (int c = 0; c < QD_L_NCOL; c++) q_duckdb_drop(val[c]);
    if (bad) { q_duckdb_drop(k); q_duckdb_drop(v); return; }
    ray_t* rec = ray_dict_new(k, v);                                     /* consumes both */
    if (!rec || RAY_IS_ERR(rec)) { q_duckdb_drop(rec); return; }
    g_in_onsql = 1;
    /* a handler that re-enters the bridge would clear the channel mid-call: the door that fired it keeps it */
    char keep[2][sizeof g_err_last];
    int  slot = conn == NULL_I32 ? -1 : (int)(conn & ((1 << QD_SLOT_BITS) - 1));
    memcpy(keep[0], g_err_last, sizeof g_err_last);
    if (slot >= 0) memcpy(keep[1], g_cons[slot].err, sizeof g_err_last);
    ray_t* r = q_eval_apply_call_sym(hook, &rec, 1);
    if (r && RAY_IS_ERR(r)) { q_err_drop(); ray_error_free(r); }
    else ray_release(r);
    memcpy(g_err_last, keep[0], sizeof g_err_last);
    if (slot >= 0) memcpy(g_cons[slot].err, keep[1], sizeof g_err_last);
    g_in_onsql = 0;
    ray_release(rec);
}

void q_duckdb_stmt_note(int slot, const char* sql, int64_t t0, bool ok, int64_t rows, int64_t cols,
                        const char* err) {
    sqllog_fire(slot >= 0 ? qd_handle_of(slot) : NULL_I32, sql, t0, q_dotz_now_ns(0) - t0, ok, rows, cols, err);
}

/* the instance and the environment it hangs off, together: the one teardown every path to a dead main takes */
static void qd_instance_drop(void) {
    if (g_main.inst) QAPI.instance_destroy(&g_main.inst);
    if (g_main.env) QAPI.environment_destroy(&g_main.env);
}

void q_duckdb_reset(void) {
    if (g_qd.state != 1) return;
    for (int i = 0; i < QD_MAX_CON; i++) if (g_cons[i].live) QAPI.connection_destroy(&g_cons[i].con);
    qd_instance_drop();
    /* no q value outlives its runtime, so generations restart at 0 — the
     * next runtime (suite) sees deterministic handles (codex P1) */
    memset(g_cons, 0, sizeof g_cons);
    memset(g_cats, 0, sizeof g_cats);
    memset(&g_main, 0, sizeof g_main);
    g_err_last[0] = '\0';
}

void q_duckdb_main_path_set(const char* path) { snprintf(g_main_path, sizeof g_main_path, "%s", path ? path : ""); }

/* ---- the statement seam: a cursor fetched WHOLE into the bridge's duck_result ---- */

void q_duckdb_result_free(duck_result* r) {
    for (duck_idx_t i = 0; i < r->nchunks; i++) QAPI.data_chunk_destroy(&r->chunks[i]);
    for (duck_idx_t c = 0; r->names && c < r->ncols; c++) free(r->names[c]);
    free(r->chunks);
    free(r->names);
    if (r->schema) QAPI.schema_destroy(&r->schema);
    memset(r, 0, sizeof *r);
}

/* the borrowed logical type of column c, valid while the result is */
duck_logical_type q_duckdb_result_col_type(const duck_result* r, duck_idx_t c) {
    duck_str nm;
    duck_logical_type lt = NULL;
    return QAPI.schema_get_field(r->schema, c, &nm, &lt, NULL) == QDuckSuccess ? lt : NULL;
}

/* what a result counts for the log: the rows a query answered, the rows a DML statement changed (its Count chunk),
 * nothing for DDL */
static int64_t qd_result_rows(const duck_result* r) {
    int64_t n = 0;
    for (duck_idx_t i = 0; i < r->nchunks; i++) {
        duck_idx_t sz = 0;
        QAPI.data_chunk_get_size(r->chunks[i], &sz, NULL);
        n += (int64_t)sz;
    }
    if (r->rtype == QDuckReturnQuery) return n;
    if (r->rtype != QDuckReturnChangedRows || n != 1) return NULL_I64;
    duck_vector v = NULL;
    duck_view   w;
    if (QAPI.data_chunk_get_vector(r->chunks[0], 0, &v, NULL) != QDuckSuccess ||
        QAPI.vector_get_view(v, &w, NULL) != QDuckSuccess) return NULL_I64;
    return ((const int64_t*)w.data)[0];
}

/* the columns a query result moved; a DML Count chunk and DDL are no result set */
static int64_t qd_result_cols(const duck_result* r) {
    return r->rtype == QDuckReturnQuery ? (int64_t)r->ncols : NULL_I64;
}

static duck_state qd_result_meta(duck_cursor cur, duck_result* out, duck_err* e) {
    int32_t    rt = 0;
    duck_state st = QAPI.result_get_result_type(cur, &rt, e);
    if (st == QDuckSuccess) out->rtype = (duck_return_t)rt;
    return st == QDuckSuccess ? QAPI.result_get_schema(cur, &out->schema, e) : st;
}

/* Drain one cursor into *out — the result type, the schema, every column name NUL-terminated, every chunk — and
 * destroy it; an execution error (they surface while stepping) lands in `why` and leaves *out empty.  A statement
 * that expands into a group (a dynamic PIVOT) has no metadata until it steps, so the first chunk is fetched first
 * and kept; a NULL one is a group that ended without a row-producing statement.  The cursor dies on every path, so
 * the connection never keeps a live result between calls. */
static duck_state qd_result_take(duck_cursor* cur, duck_result* out, char* why, size_t cap) {
    duck_err        e     = NULL;
    duck_state      st    = QDuckSuccess;
    duck_idx_t      ccap  = 0;
    duck_data_chunk first = NULL;
    bool            ended = false;
    memset(out, 0, sizeof *out);
    if ((st = qd_result_meta(*cur, out, &e)) == QDuckMetadataDeferred) {
        if (e) QAPI.error_info_destroy(&e);
        if ((st = QAPI.result_fetch_chunk(*cur, &first, &e)) == QDuckSuccess) st = qd_result_meta(*cur, out, &e);
        ended = st == QDuckSuccess && !first;
    }
    if (st == QDuckSuccess) st = QAPI.schema_get_count(out->schema, &out->ncols, &e);
    if (st == QDuckSuccess && !(out->names = q_duckdb_cols((int64_t)out->ncols, sizeof *out->names))) st = -1;
    for (duck_idx_t c = 0; st == QDuckSuccess && c < out->ncols; c++) {
        duck_str nm = { NULL, 0 };
        duck_logical_type lt = NULL;
        st = QAPI.schema_get_field(out->schema, c, &nm, &lt, &e);
        if (st == QDuckSuccess && !(out->names[c] = q_duckdb_text(nm.ptr ? nm.ptr : "", nm.len))) st = -1;
    }
    while (st == QDuckSuccess && !ended) {
        duck_data_chunk ch = first;
        first = NULL;
        if (!ch && ((st = QAPI.result_fetch_chunk(*cur, &ch, &e)) != QDuckSuccess || !ch)) break;
        if (out->nchunks == ccap) {
            duck_idx_t nc = ccap ? 2 * ccap : 8;
            duck_data_chunk* grown = realloc(out->chunks, nc * sizeof *grown);
            if (!grown) { QAPI.data_chunk_destroy(&ch); st = -1; break; }
            out->chunks = grown;
            ccap = nc;
        }
        out->chunks[out->nchunks++] = ch;
    }
    if (first) QAPI.data_chunk_destroy(&first);
    QAPI.result_destroy(cur);
    if (st == QDuckSuccess) return st;
    if (e) qd_etext(&e, why, cap);
    else snprintf(why, cap, "bridge: out of memory reading the result");
    q_duckdb_result_free(out);
    return st;
}

/* Every statement of `sql` in order — the LAST one's result stays in *out, the ones before it are run and dropped.
 * Parse and bind errors come back from DuckDB where it finds them (the iterator, the execute), execution errors
 * from the fetch; `why` carries the text, and false is the answer.  No statement at all (empty text) is an empty
 * result. */
static bool qd_run_text(int slot, const char* sql, duck_result* out, char* why, size_t cap) {
    duck_connection con = g_cons[slot].con;
    duck_err        e   = NULL;
    duck_stmt_iter  it  = NULL;
    duck_stmt       st  = NULL, next = NULL;
    memset(out, 0, sizeof *out);
    bool ok = QAPI.parse_sql(con, sql, &it, &e) == QDuckSuccess &&
              QAPI.statement_iterator_next(it, &st, &e) == QDuckSuccess;
    while (ok && st) {
        duck_cursor cur = NULL;
        ok = QAPI.statement_iterator_next(it, &next, &e) == QDuckSuccess;
        if (ok) q_duckdb_result_free(out);
        if (ok) ok = QAPI.statement_execute(con, st, NULL, NULL, 0, &cur, &e) == QDuckSuccess;
        if (ok) ok = qd_result_take(&cur, out, why, cap) == QDuckSuccess;
        QAPI.sql_statement_destroy(&st);
        st = next;
        next = NULL;
    }
    if (st) QAPI.sql_statement_destroy(&st);
    if (it) QAPI.statement_iterator_destroy(&it);
    if (e) qd_etext(&e, why, cap);
    if (!ok) q_duckdb_result_free(out);
    return ok;
}

/* Run one statement (or several, in order); error => owned 'duckdb (nothing left in *out), else *out filled
 * (the caller frees it with q_duckdb_result_free).  stash=0: internal probes never pollute err[]. */
ray_t* q_duckdb_run(int slot, const char* sql, duck_result* out, int stash) {
    int64_t t0 = q_dotz_now_ns(0);
    char why[1024] = "";
    if (!qd_run_text(slot, sql, out, why, sizeof why)) {
        q_duckdb_stmt_note(slot, sql, t0, false, NULL_I64, NULL_I64, why);
        if (stash) q_duckdb_err_stash(slot, "%s", why);
        return q_err(QE_DUCKDB);
    }
    q_duckdb_stmt_note(slot, sql, t0, true, qd_result_rows(out), qd_result_cols(out), NULL);
    return NULL;
}

const char* q_duckdb_err_text(int slot) { return slot >= 0 ? g_cons[slot].err : g_err_last; }

ray_t* q_duckdb_exec(int slot, const char* sql) {
    duck_result res;
    ray_t* e = q_duckdb_run(slot, sql, &res, 1);
    if (!e) q_duckdb_result_free(&res);
    return e;
}

/* the built statement, run and freed; a buffer that lost its text is 'wsfull */
ray_t* q_duckdb_exec_buf(int slot, qd_buf* b) {
    ray_t* e = b->oom ? q_err(QE_WSFULL) : q_duckdb_exec(slot, b->p);
    q_duckdb_buf_free(b);
    return e;
}

/* The row move of a write: `b` — an INSERT or a CREATE … AS over QD_BATCH_TBL — runs while the connection's
 * replacement scan serves `coll` under that name with these column names; the name is claimed for this statement
 * alone.  A table of the user's own under that name would win the catalog lookup first, which is why the name is
 * reserved at the write door (q_duckdb_schema_reserved_name). */
static ray_t* qd_batch_exec(int slot, duck_collection coll, const duck_str* names, duck_idx_t n, qd_buf* b) {
    g_cons[slot].coll       = coll;
    g_cons[slot].coll_names = names;
    g_cons[slot].coll_ncols = n;
    ray_t* e = q_duckdb_exec_buf(slot, b);
    g_cons[slot].coll       = NULL;
    g_cons[slot].coll_names = NULL;
    g_cons[slot].coll_ncols = 0;
    return e;
}

/* the connection's replacement scan: QD_BATCH_TBL, unqualified, while a write has a collection to serve */
static void qd_batch_scan(duck_rscan_info info, duck_context ctx, duck_err* err) {
    (void)ctx;
    void*      ud = NULL;
    duck_qname q  = NULL;
    duck_idx_t np = 0;
    duck_str   part = { NULL, 0 };
    if (QAPI.replacement_scan_get_user_data(info, &ud, NULL) != QDuckSuccess) return;
    int slot = (int)(intptr_t)ud;
    if (!g_cons[slot].coll || QAPI.replacement_scan_get_name(info, &q, NULL) != QDuckSuccess) return;
    bool hit = QAPI.qname_get_part_count(q, &np, NULL) == QDuckSuccess && np == 1 &&
               QAPI.qname_get_part(q, 0, &part, NULL) == QDuckSuccess &&
               part.len == strlen(QD_BATCH_TBL) && memcmp(part.ptr, QD_BATCH_TBL, part.len) == 0;
    QAPI.qname_destroy(&q);
    if (hit) QAPI.replacement_scan_set_collection(info, g_cons[slot].coll, g_cons[slot].coll_names,
                                                  g_cons[slot].coll_ncols, err);
}

/* a connection on the instance into slot, its batch scan registered; DuckDB's text stashed on failure */
static ray_t* qd_connect(int slot) {
    duck_err   e  = NULL;
    duck_rscan rs = NULL;
    duck_opaque ud = { (void*)(intptr_t)slot, NULL, NULL };
    bool ok = QAPI.connection_create(g_main.inst, &g_cons[slot].con, &e) == QDuckSuccess &&
              QAPI.connection_get_factory(g_cons[slot].con, &g_cons[slot].fac, &e) == QDuckSuccess;
    if (ok) ok = QAPI.replacement_scan_create(g_cons[slot].fac, &rs, &e) == QDuckSuccess &&
                 QAPI.replacement_scan_set_callback(rs, qd_batch_scan, &e) == QDuckSuccess &&
                 QAPI.replacement_scan_set_user_data(rs, &ud, &e) == QDuckSuccess &&
                 QAPI.connection_register_replacement_scan(g_cons[slot].con, rs, &e) == QDuckSuccess;
    if (rs) QAPI.replacement_scan_destroy(&rs);
    if (ok) return NULL;
    if (g_cons[slot].con) QAPI.connection_destroy(&g_cons[slot].con);
    return q_duckdb_fail_err(-1, &e);
}

/* the statement's result as a q table (`::` for a column-less one): owned, never NULL */
ray_t* q_duckdb_query(int slot, const char* sql, int stash) {
    duck_result res;
    ray_t* e = q_duckdb_run(slot, sql, &res, stash);
    if (e) return e;
    ray_t* t = q_duckdb_codec_result_to_table(slot, &res, NULL, 0, NULL);
    q_duckdb_result_free(&res);
    return t ? t : q_err(QE_WSFULL);
}

ray_t* q_duckdb_query_buf(int slot, qd_buf* b, int stash) {
    ray_t* t = b->oom ? q_err(QE_WSFULL) : q_duckdb_query(slot, b->p, stash);
    q_duckdb_buf_free(b);
    return t;
}

/* the refusal already owns the channel's first line; a rollback that fails too is the line after it */
static void qd_rollback(int slot) {
    ray_t* e = q_duckdb_exec(slot, "ROLLBACK");
    if (e) q_duckdb_drop(e);
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
    if (!qd_sym_text(x, t, sizeof t)) return q_duckdb_fail(slot, "table name: a symbol of at most 255 bytes");
    if (!q_duckdb_schema_name_parse(t, strlen(t), out))
        return q_duckdb_fail(slot, "table name: quoted or bare identifier parts joined by dots, and nothing else");
    q_duckdb_schema_name_resolve(slot, out);
    return NULL;
}

/* .duckdb.i.qname `t — the name as SQL, so a q-side door spells it exactly as the C doors do. */
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

typedef struct { char k[128]; char t[256]; } qd_opt;

/* THE startup config every open of main starts from, handed to DuckDB as the instance is created (the one moment an
 * option that can only be chosen at startup still can be).  A fat install ships `extensions/` beside `q` (the
 * release's `<dir>/v<ver>/<platform>/<name>.duckdb_extension` tree): that directory becomes DuckDB's
 * extension_directory and autoinstall goes off, so LOAD/autoload find the bundle and never reach for the network.  No
 * such directory (a thin install) leaves both at DuckDB's defaults.  The caller's own keys land after these, so they
 * win; an unknown one is DuckDB's own refusal. */
static ray_t* qd_instance_new(const qd_opt* opts, int64_t n) {
    duck_str* kv = q_duckdb_cols(2 * (n + 2), sizeof *kv);
    if (!kv) return q_err(QE_WSFULL);
    duck_str*  ks = kv;
    duck_str*  vs = kv + n + 2;
    duck_idx_t m  = 0;
    char exe[512], dir[600];
    struct stat sb;
    if (q_exedir(exe, sizeof exe)) {
        snprintf(dir, sizeof dir, "%s/extensions", exe);
        if (stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode)) {
            ks[m] = qd_str("extension_directory");
            vs[m++] = qd_str(dir);
            ks[m] = qd_str("autoinstall_known_extensions");
            vs[m++] = qd_str("false");
        }
    }
    for (int64_t i = 0; i < n; i++) {
        ks[m] = qd_str(opts[i].k);
        vs[m++] = qd_str(opts[i].t);
    }
    duck_err e = NULL;
    bool ok = QAPI.environment_create(&g_main.env, &e) == QDuckSuccess &&
              QAPI.instance_create_with_options(g_main.env, ks, vs, m, &g_main.inst, &e) == QDuckSuccess;
    free(kv);
    if (ok) return NULL;
    qd_instance_drop();
    return q_duckdb_fail_err(-1, &e);
}

/* Open main: the file `-duckdb` named, else PEACHQ_DUCKDB_MAIN, else in-memory, attached as the instance's default
 * database; the instance (qd_instance_new, carrying the SET keys of the open that creates it) starts here. */
static ray_t* qd_main_open(const qd_opt* opts, int64_t n) {
    const char* path = g_main_path[0] ? g_main_path : getenv("PEACHQ_DUCKDB_MAIN");
    if (path && !*path) path = NULL;
    ray_t* e = qd_instance_new(opts, n);
    if (e) return e;
    int slot = qd_free_slot();
    if (slot < 0) { qd_instance_drop(); return q_duckdb_fail(-1, "open: every connection slot is live"); }
    duck_err de  = NULL;
    duck_str ps  = qd_str(path ? path : ":memory:");
    if (QAPI.instance_attach(g_main.inst, NULL, &ps, NULL, true, &de) != QDuckSuccess) {
        qd_instance_drop();
        return q_duckdb_fail_err(-1, &de);
    }
    if ((e = qd_connect(slot))) { qd_instance_drop(); return e; }
    g_cons[slot].cat  = -1;
    g_cons[slot].live = true;
    g_main.slot = slot;
    g_main.open = true;
    ray_t* row = q_duckdb_query(slot, "SELECT current_database() AS d", 0);
    size_t ln = 0;
    const char* d = q_duckdb_schema_text_cell(row, 0, 0, &ln);
    snprintf(g_main.catalog, sizeof g_main.catalog, "%.*s", (int)ln, d ? d : "");
    q_duckdb_drop(row);
    ray_t* tok = ray_i32(qd_handle_of(slot));
    g_main.handle = q_provider_register_internal(Q_PQ_DUCKDB, QD_MAIN_ALIAS, tok);
    ray_release(tok);
    if (g_main.handle) return NULL;
    QAPI.connection_destroy(&g_cons[slot].con);
    qd_instance_drop();
    memset(&g_cons[slot], 0, sizeof g_cons[slot]);
    memset(&g_main, 0, sizeof g_main);
    return q_duckdb_fail(-1, "open: the host could not register the main handle");
}

static ray_t* qd_main_need(void) { return g_main.open ? NULL : qd_main_open(NULL, 0); }

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
 * parenthesised clause, every other key SET on main — through the startup config when this open creates main, else
 * as SET statements, so an open-only setting gets DuckDB's own refusal.  Owned error, else NULL. */
static ray_t* qd_config_apply(ray_t* cfg, qd_buf* attach) {
    ray_t* keys = ray_dict_keys(cfg);   /* borrowed */
    ray_t* vals = ray_dict_vals(cfg);   /* borrowed */
    int64_t np = q_count(cfg);
    if (keys->type != RAY_SYM && np) return q_duckdb_fail(-1, "open: a config key is not a symbol");
    qd_opt* opts = g_main.open ? NULL : q_duckdb_cols(np, sizeof *opts);
    if (!g_main.open && !opts) return q_err(QE_WSFULL);
    int64_t no = 0;
    ray_t*  e  = NULL;
    for (int64_t i = 0; !e && i < np; i++) {
        ray_t* ks = ray_sym_vec_cell(keys, i);
        char k[128], t[256];
        bool quote;
        snprintf(k, sizeof k, "%.*s", ks ? (int)ray_str_len(ks) : 0, ks ? ray_str_ptr(ks) : "");
        if (strcmp(k, "path") == 0) continue;
        if (strcmp(k, "timeout") == 0 || strcmp(k, "stdout") == 0 || strcmp(k, "stderr") == 0) {
            e = q_err(QE_DOMAIN);
            break;
        }
        ray_t* iv = ray_i64(i);
        ray_t* v = ray_at_fn(vals, iv);
        ray_release(iv);
        bool ok = qd_cfg_text(v, t, sizeof t, &quote);
        ray_release(v);
        if (!ok) e = q_duckdb_fail(-1, "%s: a config value must be text, a symbol, a number or a boolean", k);
        else if (qd_attach_option(k)) {
            q_duckdb_puts(attach, attach->len ? ", " : "(");
            q_duckdb_puts(attach, k);
            q_duckdb_puts(attach, " ");
            qd_cfg_put(attach, t, quote);
        } else if (opts) {
            snprintf(opts[no].k, sizeof opts[no].k, "%s", k);
            snprintf(opts[no].t, sizeof opts[no].t, "%s", t);
            no++;
        } else {
            qd_buf sb = {0};
            q_duckdb_puts(&sb, "SET ");
            q_duckdb_put_ident(&sb, k, strlen(k));
            q_duckdb_puts(&sb, " = ");
            qd_cfg_put(&sb, t, quote);
            e = q_duckdb_exec_buf(g_main.slot, &sb);
        }
    }
    if (!e && attach->len) q_duckdb_puts(attach, ")");
    if (!e && attach->oom) e = q_err(QE_WSFULL);
    if (!e && opts) e = qd_main_open(opts, no);
    free(opts);
    return e;
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
    ray_t* e = q_duckdb_exec_buf(g_main.slot, &b);
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
    ray_t* e = q_duckdb_exec_buf(g_main.slot, &b);
    if (e) q_duckdb_drop(e);
    g_cats[c].used = false;
}

/* Close one live slot: disconnect, and the catalog it used goes with its last connection.  Main's own is closed
 * only by q_duckdb_reset. */
static void qd_close_slot(int slot) {
    if (!g_cons[slot].live || (g_main.open && slot == g_main.slot)) return;
    QAPI.connection_destroy(&g_cons[slot].con);
    g_cons[slot].live = false;
    g_cons[slot].gen++;
    qd_cat_release(g_cons[slot].cat);
}

/* the dict arg's `path` (owned) into *out; 0 when there is none */
static int qd_dict_path(ray_t* d, ray_t** out) {
    ray_t* k = ray_sym(ray_sym_intern_runtime("path", 4));
    *out = ray_dict_get(d, k);
    ray_release(k);
    return *out && !RAY_IS_ERR(*out);
}

/* .duckdb.i.open[alias; arg] — the host's open hook.  arg is the db path, or a dict of `path` beside DuckDB's own
 * ATTACH/SET options (qd_config_apply; `timeout`, `stdout` and `stderr` mean nothing to an in-process engine and are
 * 'domain).  ":memory:"
 * is the shared in-memory catalog.  The alias names the catalog the path is attached under.  Answers the TOKEN. */
static ray_t* qd_open_wrap(ray_t** args, int64_t n) {
    ray_t* e = qd_door(args, n, 2, NULL);
    if (e) return e;
    char alias[256];
    const char* tp; int64_t tn;
    if (!qd_sym_text(args[0], alias, sizeof alias)) return q_duckdb_fail(-1, "open: alias is not a symbol");
    if (strcmp(alias, QD_MAIN_ALIAS) == 0) return q_err(QE_DOMAIN);
    ray_t* cfg = args[1] && args[1]->type == RAY_DICT ? args[1] : NULL;
    ray_t* pv = NULL;
    if (cfg && !qd_dict_path(cfg, &pv)) {
        if (pv && RAY_IS_ERR(pv)) ray_error_free(pv);
        return q_err(QE_DOMAIN);
    }
    ray_t* pa = cfg ? pv : args[1];
    int text = q_str_text_bytes(pa, &tp, &tn);
    char path[512];
    if (text && tn > 0 && tn < 512) {
        if (tn == 8 && memcmp(tp, ":memory:", 8) == 0) path[0] = '\0';
        else snprintf(path, sizeof path, "%.*s", (int)tn, tp);
    }
    if (pv) ray_release(pv);
    if (!text) return q_err(QE_TYPE);
    if (tn == 0) return q_err(QE_DOMAIN);
    if (tn >= 512) return q_duckdb_fail(-1, "open: path is longer than 511 bytes");

    qd_buf opts = {0};
    e = cfg ? qd_config_apply(cfg, &opts) : qd_main_need();
    if (e) { q_duckdb_buf_free(&opts); return e; }
    /* connection slot FIRST: capacity failure must not attach a catalog (codex P2) */
    int slot = qd_free_slot();
    if (slot < 0) { q_duckdb_buf_free(&opts); return q_duckdb_fail(-1, "open: every connection slot is live"); }
    int cat = qd_cat_find(path);
    if (cat >= 0 && opts.len) {
        q_duckdb_buf_free(&opts);
        return q_duckdb_fail(-1, "open: ATTACH options on a catalog that is already attached");
    }
    if (cat < 0) cat = qd_cat_attach(path, path[0] ? alias : "default", opts.len ? opts.p : "");
    q_duckdb_buf_free(&opts);
    if (cat < 0) return q_err(QE_DUCKDB);

    if ((e = qd_connect(slot))) {
        qd_cat_release(cat);
        return e;
    }
    g_cats[cat].refs++;
    g_cons[slot].cat  = cat;
    g_cons[slot].live = true;
    qd_buf b = {0};
    q_duckdb_puts(&b, "USE ");
    q_duckdb_put_ident(&b, g_cats[cat].name, strlen(g_cats[cat].name));
    e = q_duckdb_exec_buf(slot, &b);
    if (e) { qd_close_slot(slot); return e; }
    return ray_i32(qd_handle_of(slot));
}

static ray_t* qd_close_fn(ray_t* x) {
    int slot = -1;
    ray_t* e = qd_door(&x, 1, 1, &slot);
    if (e) return e;
    if (g_main.open && slot == g_main.slot) return q_err(QE_DOMAIN);
    qd_close_slot(slot);
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
    if (!e && !qd_sym_text(args[1], qn, sizeof qn)) e = q_duckdb_fail(slot, "link: qname is not a symbol");
    if (!e) e = qd_name_arg(slot, args[2], &name);
    if (e) return e;
    const char* cat = g_cons[slot].cat >= 0 ? g_cats[g_cons[slot].cat].name : g_main.catalog;
    const char* schema = name.schema[0] ? name.schema : "main";
    if (g_cons[slot].cat < 0 && strcasecmp(qn, name.part[name.n - 1]) == 0) return RAY_NULL_OBJ;
    qd_buf b = {0};
    q_duckdb_puts(&b, "CREATE OR REPLACE VIEW ");
    q_duckdb_put_ident(&b, qn, strlen(qn));
    q_duckdb_puts(&b, " AS SELECT * FROM ");
    q_duckdb_put_ident(&b, cat, strlen(cat));
    q_duckdb_puts(&b, ".");
    q_duckdb_put_ident(&b, schema, strlen(schema));
    q_duckdb_puts(&b, ".");
    q_duckdb_put_ident(&b, name.part[name.n - 1], strlen(name.part[name.n - 1]));
    if ((e = q_duckdb_exec_buf(g_main.slot, &b))) return e;
    q_duckdb_puts(&b, "COMMENT ON VIEW ");
    q_duckdb_put_ident(&b, qn, strlen(qn));
    q_duckdb_puts(&b, " IS '" QD_LINK_MARK "'");
    if ((e = q_duckdb_exec_buf(g_main.slot, &b))) return e;
    return RAY_NULL_OBJ;
}

/* .duckdb.i.unlink[token; qname] — the token is not looked at: the view drops by name after its alias is closed too;
 * the marker probe is what keeps a self-pointer symmetric and a user's own view of that name standing */
static ray_t* qd_unlink_wrap(ray_t** args, int64_t n) {
    char qn[256];
    ray_t* e = qd_door(args, n, 2, NULL);
    if (e) return e;
    if (!qd_sym_text(args[1], qn, sizeof qn)) return q_duckdb_fail(-1, "unlink: qname is not a symbol");
    if (!g_main.open) return RAY_NULL_OBJ;
    qd_buf b = {0};
    q_duckdb_puts(&b, "SELECT view_name FROM duckdb_views() WHERE NOT internal AND database_name = current_database() "
                      "AND comment = '" QD_LINK_MARK "' AND view_name = ");
    q_duckdb_put_strlit(&b, qn, strlen(qn));
    ray_t* hit = q_duckdb_query_buf(g_main.slot, &b, 0);
    if (RAY_IS_ERR(hit)) return hit;
    bool is_view = q_count(hit) == 1;
    ray_release(hit);
    if (is_view) {
        q_duckdb_puts(&b, "DROP VIEW ");
        q_duckdb_put_ident(&b, qn, strlen(qn));
        if ((e = q_duckdb_exec_buf(g_main.slot, &b))) return e;
    }
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
    ray_t* hit = q_duckdb_query_buf(slot, &b, 1);
    if (RAY_IS_ERR(hit)) return hit;
    bool is_view = q_count(hit) > 0;
    ray_release(hit);
    q_duckdb_puts(&b, is_view ? "DROP VIEW " : "DROP TABLE ");
    q_duckdb_schema_put_name(&b, &name);
    if ((e = q_duckdb_exec_buf(slot, &b))) return e;
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
    ray_t* rows = q_duckdb_query(slot, "SELECT table_name AS n FROM duckdb_tables() WHERE NOT internal AND database_name = "
                                       "current_database() AND schema_name = current_schema() UNION ALL SELECT view_name FROM "
                                       "duckdb_views() WHERE NOT internal AND database_name = current_database() AND schema_name = "
                                       "current_schema() AND coalesce(comment, '') <> '" QD_LINK_MARK "' ORDER BY 1", 0);
    if (RAY_IS_ERR(rows)) return rows;
    int64_t n = q_count(rows);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    for (int64_t i = 0; i < n && out && !RAY_IS_ERR(out); i++) {
        size_t ln = 0;
        const char* t = q_duckdb_schema_text_cell(rows, 0, i, &ln);
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

    ray_t* cat = q_duckdb_schema_catalog_cols(slot, &name);
    if (RAY_IS_ERR(cat) || q_count(cat) == 0) {
        q_duckdb_schema_desc_free(desc, ndesc);
        if (RAY_IS_ERR(cat)) return cat;
        ray_release(cat);
        return q_duckdb_fail(slot, "table %s: not in the catalog", name.part[name.n - 1]);
    }

    int64_t nrows = q_count(cat);
    ray_t* cvec = ray_sym_vec_new(RAY_SYM_W64, nrows);
    ray_t* fvec = ray_sym_vec_new(RAY_SYM_W64, nrows);
    ray_t* avec = ray_sym_vec_new(RAY_SYM_W64, nrows);
    char*  tbuf = malloc(nrows > 0 ? (size_t)nrows : 1);
    bool   oom  = !tbuf;
    int64_t blank = ray_sym_intern_runtime("", 0);
    for (int64_t i = 0; i < nrows; i++) {
        size_t ln = 0, dl = 0;
        const char* nm = q_duckdb_schema_text_cell(cat, 0, i, &ln);
        const char* dt = q_duckdb_schema_text_cell(cat, 1, i, &dl);
        qd_colmap_t cm;
        bool ok = q_duckdb_schema_catalog_col(dt ? dt : "", dl, &cm);
        if (ok) q_duckdb_codec_refine(nm ? nm : "", ln, desc, ndesc, &cm);
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
    if (q_count(x) != 2) return q_duckdb_fail(slot, "envelope: expected (data;schema)");
    *data = ((ray_t**)ray_data(x))[0];
    return q_duckdb_schema_desc_parse(slot, ((ray_t**)ray_data(x))[1], decl, ndecl);
}

/* A q table reaches DuckDB as a column data collection, never as text (docs/duckdb-architecture.md): the rows are
 * collected, then ONE statement moves them — INSERT INTO the table from the collection, served under QD_BATCH_TBL
 * by the connection's replacement scan for that statement alone, and logged as the statement it is. */
static ray_t* qd_move_rows(int slot, const qd_name_t* nm, ray_t* tbl, const qd_colmap_t* cms, ray_t* const* masks,
                           ray_t* const* keeps) {
    int64_t   ncols = ray_table_ncols(tbl);
    duck_str* names = q_duckdb_cols(ncols, sizeof *names);
    if (!names) return q_err(QE_WSFULL);
    for (int64_t c = 0; c < ncols; c++) {
        size_t nl;
        names[c].ptr = q_duckdb_col_name(tbl, c, &nl);
        names[c].len = nl;
    }
    duck_collection coll = NULL;
    ray_t* e = q_duckdb_schema_batch_clear(slot);
    if (!e) e = q_duckdb_codec_collect(slot, tbl, cms, masks, keeps, &coll);
    if (!e) {
        qd_buf b = {0};
        q_duckdb_puts(&b, "INSERT INTO ");
        q_duckdb_schema_put_name(&b, nm);
        q_duckdb_puts(&b, " SELECT * FROM \"" QD_BATCH_TBL "\"");
        e = qd_batch_exec(slot, coll, names, (duck_idx_t)ncols, &b);
        QAPI.column_data_collection_destroy(&coll);
    }
    free(names);
    return e;
}

/* The data write, inside the caller's transaction: straight from the collection, or — a declared type or a long
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
        e = q_duckdb_exec_buf(slot, &b);
    }
    if (!e) e = qd_move_rows(slot, target, img, cast ? scms : tms, cast ? smasks : masks, cast ? skeeps : keeps);
    if (cast) ray_release(img);
    free(scms);
    if (e || !cast) return e;
    if ((e = q_duckdb_schema_exact_check(slot, tbl, tms, dtypes, masks, offs, keeps))) return e;
    q_duckdb_puts(&b, !create ? "INSERT INTO " : nm->temp ? "CREATE OR REPLACE TEMP TABLE " : "CREATE OR REPLACE TABLE ");
    q_duckdb_schema_put_name(&b, nm);
    q_duckdb_puts(&b, create ? " AS " : " ");
    if ((e = q_duckdb_schema_cast_select(slot, &b, tbl, tms, dtypes, masks, offs, keeps, QD_STAGE_TBL)))
        { q_duckdb_buf_free(&b); return e; }
    if (!(e = q_duckdb_exec_buf(slot, &b))) e = q_duckdb_exec(slot, "DROP TABLE \"" QD_STAGE_TBL "\"");
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
    if (!e && !(e = q_duckdb_exec(slot, "BEGIN TRANSACTION"))) {
        bool create = replace || missing;
        open = true;
        if (!(e = qd_write_rows(slot, nm, tbl, tms, masks, offs, keeps, dtypes, create)) &&
            (!create || !(e = q_duckdb_schema_write_desc(slot, nm, tbl, tms, iskey, en, dtypes))))
            e = q_duckdb_exec(slot, "COMMIT");
    }
    q_duckdb_codec_maps_free(tms, ncols);
    free(tms);
    if (e) { if (open) qd_rollback(slot); return e; }
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
    if (q_duckdb_schema_reserved_name(&name)) return q_duckdb_fail(slot, "table name: reserved for the bridge's own tables");

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
            nk   = ray_table_ncols(kt);
            flat = q_bang_enkey(0, tbl);   /* unkey: the key columns first */
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
                           ? q_duckdb_fail(slot, "keyed: a keyed table needs a value column")
                           : q_duckdb_fail(slot, "value: not a table");
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
    ray_release(flat);
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
    ray_t* tbl = b.oom ? q_err(QE_WSFULL) : q_duckdb_run(slot, b.p, &res, 1);
    q_duckdb_buf_free(&b);
    if (!tbl) {
        tbl = q_duckdb_codec_result_to_table(slot, &res, desc, ndesc, schema);
        q_duckdb_result_free(&res);
        tbl = q_duckdb_schema_rekey(tbl, desc, ndesc);
        if (RAY_IS_ERR(tbl) && schema && *schema) { ray_release(*schema); *schema = NULL; }
    }
    q_duckdb_schema_desc_free(desc, ndesc);
    return tbl;
}

/* (data;schema) from two owned halves; an error in either propagates (a NULL schema is the empty one) */
static ray_t* qd_pair(ray_t* data, ray_t* schema) {
    if (RAY_IS_ERR(data)) { ray_release(schema); return data; }
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

/* the markers ONE parsed statement declares, as DuckDB names them ("1", "2" for $1 and ?), BORROWED from it;
 * np = how many.  false = DuckDB's refusal through *e (or 'wsfull); *names is the caller's to free either way. */
static bool qd_stmt_markers(int slot, duck_stmt st, duck_str** names, int64_t* np, ray_t** e) {
    duck_err   de = NULL;
    duck_idx_t n  = 0;
    *names = NULL;
    *np    = 0;
    bool ok = QAPI.sql_statement_get_parameter_count(st, &n, &de) == QDuckSuccess;
    if (ok && !(*names = q_duckdb_cols((int64_t)n, sizeof **names))) { *e = q_err(QE_WSFULL); return false; }
    for (duck_idx_t i = 0; ok && i < n; i++)
        ok = QAPI.sql_statement_get_parameter_name(st, i, &(*names)[i], &de) == QDuckSuccess;
    if (!ok) { *e = q_duckdb_fail_err(slot, &de); return false; }
    *np = (int64_t)n;
    return true;
}

/* DuckDB folds a marker like an identifier (ASCII case), so a key matches its marker the same way */
static bool qd_marker_eq(const duck_str* m, const char* key, size_t kn) {
    if (m->len != kn) return false;
    for (size_t i = 0; i < kn; i++) {
        char a = m->ptr[i], b = key[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return true;
}

/* ONE statement of the pair, bound and run into *out (whatever it held is dropped first): by NAME from a
 * symbol-keyed dict — the markers this statement names, a key naming none of them ignored (owner 2026-10-14), two
 * keys DuckDB folds onto one marker an error naming both — or by POSITION from any other list, exactly as many
 * items as markers, checked first in DuckDB's own words.  Binder and execution errors are DuckDB's. */
static ray_t* qd_bind_run(int slot, duck_stmt st, ray_t* keys, ray_t* vals, duck_result* out) {
    int64_t   n = q_count(vals), np = 0, nb = 0;
    duck_str* mk = NULL;
    ray_t*    e  = NULL;
    if (!qd_stmt_markers(slot, st, &mk, &np, &e)) { free(mk); return e; }
    /* one name and one value per supplied item; by[] = the key (1-based) that bound each of the np markers */
    duck_str*   names = q_duckdb_cols(n, sizeof *names + sizeof(duck_value));
    duck_value* dv    = names ? (duck_value*)(names + n) : NULL;
    int64_t*    by    = names ? q_duckdb_cols(np, sizeof *by) : NULL;
    if (!by) e = q_err(QE_WSFULL);
    if (!e && !keys && n != np)
        e = q_duckdb_fail(slot, "Expected %lld parameters, but %lld were supplied", (long long)np, (long long)n);
    for (int64_t i = 0; !e && i < n; i++) {
        int64_t j = -1;
        if (keys) {
            ray_t* ks = ray_sym_vec_cell(keys, i);   /* borrowed */
            for (int64_t m = 0; j < 0 && m < np; m++)
                if (ks && qd_marker_eq(&mk[m], ray_str_ptr(ks), ray_str_len(ks))) j = m;
            if (j < 0) continue;
            if (by[j]) {
                ray_t* first = ray_sym_vec_cell(keys, by[j] - 1);
                e = q_duckdb_fail(slot, "parameters %.*s and %.*s bind the same marker", first ? (int)ray_str_len(first) : 0,
                                  first ? ray_str_ptr(first) : "", ks ? (int)ray_str_len(ks) : 0, ks ? ray_str_ptr(ks) : "");
                break;
            }
            by[j] = i + 1;
            names[nb] = mk[j];
        }
        ray_t* v = q_index_elem_at(vals, i);
        if (!v || RAY_IS_ERR(v)) { e = v ? v : q_err(QE_WSFULL); break; }
        e = q_duckdb_codec_bind_value(slot, v, &dv[nb]);
        ray_release(v);
        nb++;
    }
    if (!e) {
        char why[1024] = "";
        duck_cursor cur = NULL;
        duck_err    de  = NULL;
        q_duckdb_result_free(out);
        bool ok = QAPI.statement_execute(g_cons[slot].con, st, keys ? names : NULL, dv, (duck_idx_t)nb, &cur, &de)
                      == QDuckSuccess;
        if (ok) ok = qd_result_take(&cur, out, why, sizeof why) == QDuckSuccess;
        if (de) qd_etext(&de, why, sizeof why);
        if (!ok) e = q_duckdb_fail(slot, "%s", why);
    }
    for (int64_t i = 0; dv && i < nb; i++) if (dv[i]) QAPI.value_destroy(&dv[i]);
    free(names);
    free(by);
    free(mk);
    return e;
}

/* The pair's run — q_duckdb_run with parameters: statement by statement as the bare text runs, the LAST result
 * kept.  Names address, positions do not: a dict binds every statement, each the markers it names; a positional
 * list binds one statement, and text with more is refused before anything runs.  Parse errors are DuckDB's. */
static ray_t* qd_run_bound(int slot, const char* sql, ray_t* params, duck_result* out) {
    int64_t t0 = q_dotz_now_ns(0);
    ray_t*  keys = params->type == RAY_DICT ? ray_dict_keys(params) : NULL;
    ray_t*  vals = keys ? ray_dict_vals(params) : params;
    duck_connection con  = g_cons[slot].con;
    duck_err        de   = NULL;
    duck_stmt_iter  it   = NULL;
    duck_stmt       st   = NULL, next = NULL;
    memset(out, 0, sizeof *out);
    ray_t* e = QAPI.parse_sql(con, sql, &it, &de) == QDuckSuccess &&
               QAPI.statement_iterator_next(it, &st, &de) == QDuckSuccess ? NULL : q_duckdb_fail_err(slot, &de);
    while (!e && st) {
        if (QAPI.statement_iterator_next(it, &next, &de) != QDuckSuccess) e = q_duckdb_fail_err(slot, &de);
        else if (!keys && next) e = q_duckdb_fail(slot, "bridge: a positional list binds one statement");
        else e = qd_bind_run(slot, st, keys, vals, out);
        QAPI.sql_statement_destroy(&st);
        st   = next;
        next = NULL;
    }
    if (st) QAPI.sql_statement_destroy(&st);
    if (it) QAPI.statement_iterator_destroy(&it);
    if (e) q_duckdb_result_free(out);
    q_duckdb_stmt_note(slot, sql, t0, !e, e ? NULL_I64 : qd_result_rows(out), e ? NULL_I64 : qd_result_cols(out),
                       e ? q_duckdb_err_text(slot) : NULL);
    return e;
}

/* The pair and triple laws (docs/duckdb-api.md § Parameters): the pair (sql;params), params a symbol-keyed dict
 * (named) or any other list (positional), an EMPTY one being no parameters (*params NULL); the triple
 * (sql;params;schema), schema the envelope's TABLE (*schema, else NULL).  Every other list is 'type; a non-list is
 * the bare string, judged by the door. */
static ray_t* qd_sql_split(ray_t* x, ray_t** sql, ray_t** params, ray_t** schema) {
    *sql = x;
    *params = *schema = NULL;
    if (!x || x->type != RAY_LIST) return NULL;
    int64_t n = q_count(x);
    ray_t* p = n == 2 || n == 3 ? ray_list_get(x, 1) : NULL;
    bool list = p && p->type >= RAY_LIST && p->type <= RAY_TIME;
    bool dict = p && p->type == RAY_DICT && (ray_dict_keys(p)->type == RAY_SYM || !q_count(p));
    ray_t* s = n == 3 ? ray_list_get(x, 2) : NULL;
    if ((!list && !dict) || (s && s->type != RAY_TABLE)) return q_err(QE_TYPE);
    *sql = ray_list_get(x, 0);
    if (q_count(p)) *params = p;
    *schema = s;
    return NULL;
}

/* .duckdb.exec[h;"..."] / .duckdb.execx — run any statement (python-API parity), or the pair (sql;params) with its
 * parameters bound: a result WITH columns converts per the default read mapping (no sidecar; unsupported column
 * types error as ever), or, given the triple, refined and keyed by its schema as qd_read_table's sidecar rows
 * refine; execx pairs it with its schema; a column-less result (DDL) answers `::` (execx: beside an empty schema). */
static ray_t* qd_sql_any(ray_t** args, int64_t n, bool pair) {
    int slot;
    ray_t* e = qd_door(args, n, 2, &slot);
    if (e) return e;
    ray_t* sql;
    ray_t* params;
    ray_t* decl;
    if ((e = qd_sql_split(args[1], &sql, &params, &decl))) return e;
    const char* tp; int64_t tn;
    if (!sql || !q_str_text_bytes(sql, &tp, &tn))
        return sql == args[1] ? q_duckdb_fail(slot, "sql: not a string") : q_err(QE_TYPE);
    qd_desc_t* desc  = NULL;
    int64_t    ndesc = 0;
    if (decl && (e = q_duckdb_schema_desc_parse(slot, decl, &desc, &ndesc))) return e;
    qd_buf b = {0};
    q_duckdb_putn(&b, tp, (size_t)tn);
    duck_result res;
    e = b.oom ? q_err(QE_WSFULL) : params ? qd_run_bound(slot, b.p, params, &res) : q_duckdb_run(slot, b.p, &res, 1);
    q_duckdb_buf_free(&b);
    ray_t* out = e;
    if (!e) {
        ray_t* schema = NULL;
        out = q_duckdb_codec_result_to_table(slot, &res, desc, ndesc, pair ? &schema : NULL);
        q_duckdb_result_free(&res);
        out = q_duckdb_schema_rekey(out, desc, ndesc);
        if (pair) out = qd_pair(out, schema);
    }
    q_duckdb_schema_desc_free(desc, ndesc);
    return out;
}

static ray_t* qd_sql_wrap(ray_t** args, int64_t n)  { return qd_sql_any(args, n, false); }
static ray_t* qd_sqlx_wrap(ray_t** args, int64_t n) { return qd_sql_any(args, n, true); }

/* .duckdb.i.prepnames[h;sql] — DuckDB's own parameter names of the text, statement by statement in index order:
 * the oracle the scanner (.duckdb.i.params) is tested against, and nothing else's.  Each statement is bound as a
 * prepare would be, so one DuckDB will not bind refuses with its text — except one that expands into a group (a
 * dynamic PIVOT), which DuckDB binds only when it runs: its markers are answered unvalidated. */
static ray_t* qd_prepnames_wrap(ray_t** args, int64_t n) {
    int slot;
    ray_t* e = qd_door(args, n, 2, &slot);
    if (e) return e;
    const char* tp; int64_t tn;
    if (!q_str_text_bytes(args[1], &tp, &tn)) return q_err(QE_TYPE);
    char* sql = q_duckdb_text(tp, (size_t)tn);
    if (!sql) return q_err(QE_WSFULL);
    duck_connection con = g_cons[slot].con;
    duck_err        de  = NULL;
    duck_stmt_iter  it  = NULL;
    duck_stmt       st  = NULL;
    e = QAPI.parse_sql(con, sql, &it, &de) == QDuckSuccess &&
        QAPI.statement_iterator_next(it, &st, &de) == QDuckSuccess ? NULL : q_duckdb_fail_err(slot, &de);
    free(sql);
    ray_t* r = e ? NULL : ray_sym_vec_new(RAY_SYM_W64, 1);
    if (!e && (!r || RAY_IS_ERR(r))) { e = r ? r : q_err(QE_WSFULL); r = NULL; }
    while (!e && st) {
        duck_str*   mk  = NULL;
        int64_t     np  = 0;
        duck_schema sch = NULL;
        duck_state  bs  = qd_stmt_markers(slot, st, &mk, &np, &e) ? QAPI.statement_bind(con, st, &sch, NULL, &de)
                                                                   : QDuckSuccess;
        if (bs == QDuckMetadataDeferred) { if (de) QAPI.error_info_destroy(&de); }
        else if (bs != QDuckSuccess) e = q_duckdb_fail_err(slot, &de);
        for (int64_t i = 0; !e && i < np; i++) {
            int64_t id = ray_sym_intern_runtime(mk[i].ptr, (int64_t)mk[i].len);
            r = ray_vec_append(r, &id);
            if (!r || RAY_IS_ERR(r)) { e = r ? r : q_err(QE_WSFULL); r = NULL; }
        }
        if (sch) QAPI.schema_destroy(&sch);
        free(mk);
        QAPI.sql_statement_destroy(&st);
        if (!e && QAPI.statement_iterator_next(it, &st, &de) != QDuckSuccess) e = q_duckdb_fail_err(slot, &de);
    }
    if (st) QAPI.sql_statement_destroy(&st);
    if (it) QAPI.statement_iterator_destroy(&it);
    if (e) { ray_release(r); return e; }
    return r;
}

/* .duckdb.i.params sql — the $names as the scanner reads them (q_duckdb_params.c): text alone, no library */
static ray_t* qd_params_fn(ray_t* x) {
    const char* tp; int64_t tn;
    return q_str_text_bytes(x, &tp, &tn) ? q_duckdb_params(tp, (size_t)tn) : q_err(QE_TYPE);
}

/* .duckdb.i.spelled sql — the same markers as written (the parser's capture names), internal */
static ray_t* qd_spelled_fn(ray_t* x) {
    const char* tp; int64_t tn;
    return q_str_text_bytes(x, &tp, &tn) ? q_duckdb_params_spelled(tp, (size_t)tn) : q_err(QE_TYPE);
}

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
    char why[1024] = "";
    if (!qd_run_text(slot, s.p, &res, why, sizeof why)) {
        q_duckdb_stmt_note(slot, s.p, t0, false, NULL_I64, NULL_I64, why);
        q_duckdb_buf_free(&s);
        return diag_text(why);
    }
    int64_t ncol = (int64_t)res.ncols;
    qd_buf b = {0};
    bool   walk = ncol > 0;
    for (int64_t c = 0; c < ncol; c++) {
        if (c) q_duckdb_puts(&b, " | ");
        q_duckdb_puts(&b, res.names[c]);
        walk &= q_duckdb_codec_type_id(q_duckdb_result_col_type(&res, (duck_idx_t)c)) == QDUCK_TYPE_VARCHAR;
    }
    int64_t nrow = 0;
    for (duck_idx_t k = 0; k < res.nchunks; k++) {
        duck_idx_t rows = 0;
        QAPI.data_chunk_get_size(res.chunks[k], &rows, NULL);
        for (duck_idx_t r = 0; r < rows; r++) {
            q_duckdb_puts(&b, "\n");
            for (int64_t c = 0; c < ncol; c++) {
                if (c) q_duckdb_puts(&b, " | ");
                duck_vector dv = NULL;
                QAPI.data_chunk_get_vector(res.chunks[k], (duck_idx_t)c, &dv, NULL);
                /* a VARCHAR is read off its bytes, so a NUL inside it survives; every other type is DuckDB's own
                 * rendering of the cell, which a C string carries whole */
                if (walk) {
                    duck_view w;
                    if (!q_duckdb_codec_vec_view(dv, &w) || !q_duckdb_validity_ok(w.validity, r)) { q_duckdb_puts(&b, "NULL"); continue; }
                    const duck_string_t* v = (const duck_string_t*)w.data + r;
                    q_duckdb_putn(&b, q_duckdb_string_data(v), q_duckdb_string_len(v));
                    continue;
                }
                duck_value val = NULL;
                bool       isnull = false;
                char       cell[4096];
                duck_idx_t len = 0;
                bool ok = QAPI.vector_get_value(dv, r, &val, NULL) == QDuckSuccess &&
                          QAPI.value_is_null(val, &isnull, NULL) == QDuckSuccess;
                if (ok && isnull) q_duckdb_puts(&b, "NULL");
                else if (ok && QAPI.value_to_string(val, cell, sizeof cell, &len, NULL) == QDuckSuccess) q_duckdb_puts(&b, cell);
                else if (ok && len >= sizeof cell) {
                    char* big = malloc(len + 1);
                    if (big && QAPI.value_to_string(val, big, len + 1, &len, NULL) == QDuckSuccess) q_duckdb_puts(&b, big);
                    else b.oom = 1;
                    free(big);
                } else {
                    q_duckdb_puts(&b, "<");
                    q_duckdb_puts(&b, q_duckdb_type_name(q_duckdb_codec_type_id(q_duckdb_result_col_type(&res, (duck_idx_t)c))));
                    q_duckdb_puts(&b, " unrenderable>");
                }
                if (val) QAPI.value_destroy(&val);
            }
        }
        nrow += (int64_t)rows;
    }
    q_duckdb_stmt_note(slot, s.p, t0, true, ncol ? nrow : NULL_I64, ncol ? ncol : NULL_I64, NULL);
    q_duckdb_buf_free(&s);
    q_duckdb_result_free(&res);
    ray_t* out = b.oom ? q_err(QE_WSFULL) : ray_charv(b.len ? b.p : "", (int64_t)b.len);
    q_duckdb_buf_free(&b);
    return out;
}

static void qd_bind_unary(const char* name, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(name, RAY_FN_NONE, fn);
    q_env_bind_native(name, obj, 1);
    ray_release(obj);
}

static void qd_bind_vary(const char* name, int rank, ray_vary_fn fn) {
    ray_t* obj = ray_fn_vary(name, RAY_FN_NONE, fn);
    q_env_bind_native(name, obj, rank);
    ray_release(obj);
}

/* .duckdb.i.types[] — QD_TYPES[] projected as a q table `dtype`ktype`logical`canon`needs`companion.
 * The C array is the ONE home of the mapping (append-only contract, fidelity spec
 * 2026-07-14); q-side consumers DERIVE from this table, never re-author it.  Every
 * row is included, ' '-meta rows too — filtering is the consumer's business.  needs
 * = what an envelope row must carry for the type to survive a round trip, companion
 * = which extra column can appear beside it — both the reader's own decisions, read
 * off q_duckdb_schema_needs and q_duckdb_codec_companions_of rather than restated here. */
enum { QD_T_DTYPE, QD_T_LOGICAL, QD_T_NEEDS, QD_T_COMPANION, QD_T_NSYM };

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

/* The INTERNAL native surface (`.duckdb.i.*`) the `.duckdb` provider hooks
 * are written over — the connection, the typed round-trip and one raw exec.
 * The public bespoke API (connect/sql/select/connections/version) was
 * REPLACED 2026-08-07 by the `:pq:duckdb:` virtual-table surface, not wrapped;
 * `.duckdb.err[]`, the message channel behind the bare 'duckdb, is the reader kept.
 * Registration fires from `.pq.load_natives`, so a session that never loads `.duckdb` is kdb-clean. */
void q_duckdb_register(void) {
    /* NO dlopen here — the library is resolved lazily on first use. */
    qd_bind_vary (".duckdb.i.open",   4, qd_open_wrap);
    qd_bind_unary(".duckdb.i.close",  qd_close_fn);
    qd_bind_vary (".duckdb.i.main",   1, qd_main_fn);
    qd_bind_vary (".duckdb.i.link",   3, qd_link_wrap);
    qd_bind_vary (".duckdb.i.unlink", 2, qd_unlink_wrap);
    qd_bind_vary (".duckdb.i.hdel",   2, qd_hdel_wrap);
    qd_bind_unary(".duckdb.i.tables", qd_tables_fn);
    qd_bind_unary(".duckdb.i.err",    qd_err_fn);
    qd_bind_vary (".duckdb.i.exec",   2, qd_sql_wrap);
    qd_bind_vary (".duckdb.i.execx",  2, qd_sqlx_wrap);
    qd_bind_vary (".duckdb.i.prepnames", 2, qd_prepnames_wrap);
    qd_bind_unary(".duckdb.i.params", qd_params_fn);
    qd_bind_unary(".duckdb.i.spelled", qd_spelled_fn);
    qd_bind_unary(".duckdb.i.qname",  qd_qname_fn);
    qd_bind_vary (".duckdb.i.get",    2, qd_get_wrap);
    qd_bind_vary (".duckdb.i.getx",   2, qd_getx_wrap);
    qd_bind_vary (".duckdb.i.set",    3, qd_set_wrap);
    qd_bind_vary (".duckdb.i.append", 3, qd_append_wrap);
    qd_bind_vary (".duckdb.i.meta",   2, qd_meta_wrap);
    qd_bind_vary (".duckdb.i.types",  1, qd_types_fn);
    /* the two DIAGNOSTICS, bound under .duckdb directly: neither has q-side logic, and a shim in front of a
     * diagnostic is one more thing to keep honest.  `unsafe` marks the one that bypasses the type contract. */
    qd_bind_vary (".duckdb.i.unsafeExecText", 2, qd_unsafe_exec_text_wrap);
}
