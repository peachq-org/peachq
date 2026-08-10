/* q_duckdb — openq's `.duckdb` namespace: the DuckDB bridge.  Contract, type
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
#include "qlang/io/q_duckdb_types.h"
#include "qlang/io/q_exedir.h"
#include "qlang/base/q_err.h"
#include "qlang/q_env.h"
#include "qlang/q_prim.h"     /* q_str_text_bytes — text cells on the write path */
#include "lang/env.h"         /* ray_fn_unary / ray_fn_vary */
#include "lang/eval.h"        /* RAY_FN_NONE, ray_at_fn */
#include "table/sym.h"        /* ray_sym_vec_cell */
#include <rayforce.h>
#include <math.h>
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

typedef struct { char* p; size_t len, cap; int oom; } qd_buf;

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

static void qd_puts(qd_buf* b, const char* s) { qd_putn(b, s, strlen(s)); }

/* SQL identifier: "name" with embedded " doubled. */
static void qd_put_ident(qd_buf* b, const char* s, size_t n) {
    qd_putn(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"') qd_putn(b, "\"\"", 2);
        else             qd_putn(b, s + i, 1);
    }
    qd_putn(b, "\"", 1);
}

/* SQL string literal: 'text' with embedded ' doubled. */
static void qd_put_strlit(qd_buf* b, const char* s, size_t n) {
    qd_putn(b, "'", 1);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\'') qd_putn(b, "''", 2);
        else              qd_putn(b, s + i, 1);
    }
    qd_putn(b, "'", 1);
}

static void qd_buf_free(qd_buf* b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

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
    "duckdb_column_type",
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
};
#define QD_NSYMS (sizeof QD_SYMS / sizeof *QD_SYMS)

_Static_assert(sizeof(duck_api_t) == QD_NSYMS * sizeof(void*),
               "QD_SYMS[] must match duck_api_t, in order");

static struct {
    int        state;        /* 0 = untried, 1 = loaded, 2 = failed */
    void*      dl;
    duck_api_t api;
} g_qd;

#define QAPI (g_qd.api)

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

#if defined(_WIN32)
#define QD_LIB_BASENAME "duckdb.dll"
#elif defined(__APPLE__)
#define QD_LIB_BASENAME "libduckdb.dylib"
#else
#define QD_LIB_BASENAME "libduckdb.so"
#endif

#endif /* !__EMSCRIPTEN__ */

/* One load attempt; PEACHQ_DUCKDB_LIB set = EXCLUSIVE (no fallback). */
static void qd_load(void) {
    if (g_qd.state) return;
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
    void** slots = (void**)&g_qd.api;
    for (size_t i = 0; i < QD_NSYMS; i++) {
        slots[i] = qd_dlsym(dl, QD_SYMS[i]);
        if (!slots[i]) {
            g_qd.state = 2;
            memset(&g_qd.api, 0, sizeof g_qd.api);
            return;
        }
    }
    /* version gate: the bridge's contract is written against >= 1.4 */
    const char* ver = QAPI.library_version();
    int maj = 0, min = 0;
    if (!ver || sscanf(ver, "v%d.%d", &maj, &min) != 2 ||
        maj < 1 || (maj == 1 && min < 4)) {
        g_qd.state = 2;
        memset(&g_qd.api, 0, sizeof g_qd.api);
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
    char            err[1024];   /* last DuckDB-produced error text */
} g_cons[QD_MAX_CON];

/* most recent captured text, any connection — .duckdb.err[] reads this */
static char g_err_last[1024];

static void qd_err_stash(int slot, const char* msg) {
    snprintf(g_err_last, sizeof g_err_last, "%s", msg ? msg : "");
    if (slot >= 0)
        snprintf(g_cons[slot].err, sizeof g_cons[slot].err, "%s", msg ? msg : "");
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

static const qd_tmap_t* qd_map_logical(const char* name, size_t n) {
    for (size_t i = 0; i < QD_NTYPES; i++)
        if (strlen(QD_TYPES[i].logical) == n &&
            memcmp(QD_TYPES[i].logical, name, n) == 0)
            return &QD_TYPES[i];
    return NULL;
}

static bool qd_cell_is_text(ray_t* cell) {
    const char* p; int64_t n;
    return cell && q_str_text_bytes(cell, &p, &n);
}

/* q column -> manifest row (write direction): a RAY_LIST column is utf8 when
 * EVERY cell is text, bytes when every cell is a byte vector. */
static const qd_tmap_t* qd_map_write(ray_t* col) {
    if (col->type == RAY_LIST) {
        bool all_bytes = true, all_text = true;
        for (int64_t i = 0; i < col->len; i++) {
            void* pp = ray_vec_get(col, i);
            ray_t* cell = pp ? *(ray_t**)pp : NULL;
            if (!cell) return NULL;
            if (cell->type != RAY_BYTE_ONLY) all_bytes = false;
            if (!qd_cell_is_text(cell))      all_text  = false;
            if (!all_bytes && !all_text) return NULL;
        }
        int8_t want = (all_bytes || col->len == 0) ? RAY_LIST : RAY_STR;
        for (size_t i = 0; i < QD_NTYPES; i++)
            if (QD_TYPES[i].ray_type == want && QD_TYPES[i].read_canon)
                return &QD_TYPES[i];
        return NULL;
    }
    for (size_t i = 0; i < QD_NTYPES; i++)
        if (QD_TYPES[i].ray_type == col->type) return &QD_TYPES[i];
    return NULL;
}

static const qd_tmap_t* qd_map_read(duck_type t) {
    for (size_t i = 0; i < QD_NTYPES; i++)
        if (QD_TYPES[i].dk_type == t && QD_TYPES[i].read_canon) return &QD_TYPES[i];
    return NULL;
}

/* The q column tag a row SURFACES as (utf8/bytes are 0h lists at q-space). */
static int8_t qd_surface_type(const qd_tmap_t* tm) {
    return (tm->ray_type == RAY_STR || tm->ray_type == RAY_LIST)
               ? RAY_LIST : tm->ray_type;
}

/* ---- UUID <-> hugeint (upper word's sign bit flipped for ordering;
 * bytes are RFC-4122 big-endian) ---- */

static void qd_uuid_to_bytes(duck_hugeint h, uint8_t* b) {
    uint64_t hi = (uint64_t)h.upper ^ 0x8000000000000000ULL;
    for (int i = 0; i < 8; i++) b[i]     = (uint8_t)(hi >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) b[8 + i] = (uint8_t)(h.lower >> (56 - 8 * i));
}

static duck_hugeint qd_bytes_to_uuid(const uint8_t* b) {
    uint64_t hi = 0, lo = 0;
    for (int i = 0; i < 8; i++) hi = (hi << 8) | b[i];
    for (int i = 0; i < 8; i++) lo = (lo << 8) | b[8 + i];
    duck_hugeint h;
    h.upper = (int64_t)(hi ^ 0x8000000000000000ULL);
    h.lower = lo;
    return h;
}

static bool qd_guid_is_null(const uint8_t* b) {
    for (int i = 0; i < 16; i++) if (b[i]) return false;
    return true;
}

/* placeholder elem + set_null (sentinel + HAS_NULLS attr) */
static ray_t* qd_append_null(ray_t* vec) {
    int64_t zero[2] = { 0, 0 };   /* covers up to 16-byte (guid) elems */
    vec = ray_vec_append(vec, zero);
    if (!vec || RAY_IS_ERR(vec)) return vec;
    ray_vec_set_null(vec, vec->len - 1, true);
    return vec;
}

/* n rows of one DuckDB vector -> the accumulating q column (moved), or error. */
static ray_t* qd_read_cells(ray_t* col, const qd_tmap_t* tm,
                            duck_vector dv, duck_idx_t n) {
    void*     data     = QAPI.vector_get_data(dv);
    uint64_t* validity = QAPI.vector_get_validity(dv);
    for (duck_idx_t r = 0; r < n; r++) {
        bool ok = q_duckdb_validity_ok(validity, r);
        switch (tm->ray_type) {
            case RAY_BOOL: {
                if (!ok) { ray_release(col); return q_err(QE_DUCKDB); }
                uint8_t v = ((uint8_t*)data)[r] ? 1 : 0;
                col = ray_vec_append(col, &v);
                break;
            }
            case RAY_BYTE_ONLY: {
                if (!ok) { ray_release(col); return q_err(QE_DUCKDB); }
                uint8_t v = ((uint8_t*)data)[r];
                col = ray_vec_append(col, &v);
                break;
            }
            case RAY_I16: {      /* TINYINT widens losslessly to q short */
                int16_t v = tm->dk_type == QDUCK_TYPE_TINYINT
                                ? (int16_t)((int8_t*)data)[r]
                                : ((int16_t*)data)[r];
                if (!ok || v == NULL_I16) col = qd_append_null(col);
                else                      col = ray_vec_append(col, &v);
                break;
            }
            case RAY_I32:
            case RAY_MONTH:
            case RAY_MINUTE:
            case RAY_SECOND: {   /* month/minute/second: raw int32 counts */
                int32_t v = ((int32_t*)data)[r];
                if (!ok || v == NULL_I32) col = qd_append_null(col);
                else                      col = ray_vec_append(col, &v);
                break;
            }
            case RAY_I64:
            case RAY_TIMESPAN: { /* timespan: raw ns rides BIGINT */
                int64_t v = ((int64_t*)data)[r];
                if (!ok || v == NULL_I64) col = qd_append_null(col);
                else                      col = ray_vec_append(col, &v);
                break;
            }
            case RAY_F32: {      /* live-infinity: ONLY NaN is null */
                float v = ((float*)data)[r];
                if (!ok || v != v) col = qd_append_null(col);
                else               col = ray_vec_append(col, &v);
                break;
            }
            case RAY_F64:
            case RAY_DATETIME: { /* datetime: raw float days ride DOUBLE */
                double v = ((double*)data)[r];
                if (!ok || v != v) col = qd_append_null(col);
                else               col = ray_vec_append(col, &v);
                break;
            }
            case RAY_SYM: {      /* descriptor-refined VARCHAR: intern; NULL
                                  * collapses to ` like '' */
                const duck_string_t* s = &((const duck_string_t*)data)[r];
                int64_t id = ok ? ray_sym_intern_runtime(q_duckdb_string_data(s),
                                                         q_duckdb_string_len(s))
                                : ray_sym_intern_runtime("", 0);
                col = ray_vec_append(col, &id);
                break;
            }
            case RAY_STR: {      /* VARCHAR -> charv cell; NULL -> "" */
                const duck_string_t* s = ok ? &((const duck_string_t*)data)[r] : NULL;
                ray_t* cell = ray_charv(s ? q_duckdb_string_data(s) : "",
                                        s ? (int64_t)q_duckdb_string_len(s) : 0);
                if (!cell || RAY_IS_ERR(cell)) { ray_release(col);
                    return cell ? cell : q_err(QE_WSFULL); }
                col = ray_list_append(col, cell);   /* retains */
                ray_release(cell);
                break;
            }
            case RAY_LIST: {     /* BLOB -> byte-vector cell; NULL -> 0x (empty) */
                ray_t* cell;
                if (!ok) cell = ray_vec_new(RAY_BYTE_ONLY, 1);
                else {
                    const duck_string_t* s = &((const duck_string_t*)data)[r];
                    uint32_t len = q_duckdb_string_len(s);
                    cell = len ? ray_vec_from_raw(RAY_BYTE_ONLY, q_duckdb_string_data(s), len)
                               : ray_vec_new(RAY_BYTE_ONLY, 1);
                }
                if (!cell || RAY_IS_ERR(cell)) { ray_release(col); return cell; }
                col = ray_list_append(col, cell);
                ray_release(cell);
                break;
            }
            case RAY_DATE: {
                int32_t v = ((int32_t*)data)[r];
                if (!ok) { col = qd_append_null(col); break; }
                if (v < INT32_MIN + QD_EPOCH_DAYS)   /* epoch shift underflow */
                    { ray_release(col); return q_err(QE_DUCKDB); }
                int32_t q = v - QD_EPOCH_DAYS;
                col = ray_vec_append(col, &q);
                break;
            }
            case RAY_TIME: {     /* µs -> ms; sub-ms is unrepresentable, not
                                  * truncatable — "exact else error" (codex) */
                int64_t v = ((int64_t*)data)[r];
                if (!ok) { col = qd_append_null(col); break; }
                if (v % 1000) { ray_release(col); return q_err(QE_DUCKDB); }
                int32_t q = (int32_t)(v / 1000);
                col = ray_vec_append(col, &q);
                break;
            }
            case RAY_TIMESTAMP: {  /* int64 ns epoch 1970 -> epoch 2000 */
                int64_t v = ((int64_t*)data)[r];
                if (!ok || v == NULL_I64) { col = qd_append_null(col); break; }
                if (tm->dk_type == QDUCK_TYPE_TIMESTAMP) {   /* µs: exact else error */
                    if (v > INT64_MAX / 1000 || v < INT64_MIN / 1000)
                        { ray_release(col); return q_err(QE_DUCKDB); }
                    v *= 1000;
                }
                if (v < INT64_MIN + QD_EPOCH_NS)     /* epoch shift underflow */
                    { ray_release(col); return q_err(QE_DUCKDB); }
                int64_t q = v - QD_EPOCH_NS;
                col = ray_vec_append(col, &q);
                break;
            }
            case RAY_GUID: {
                if (!ok) { col = qd_append_null(col); break; }   /* NULL -> 0Ng */
                uint8_t b[16];
                qd_uuid_to_bytes(((const duck_hugeint*)data)[r], b);
                col = ray_vec_append(col, b);
                break;
            }
            default:
                ray_release(col);
                return q_err(QE_DUCKDB);
        }
        if (!col || RAY_IS_ERR(col))
            return col ? col : q_err(QE_WSFULL);
    }
    return col;
}

typedef struct {
    char col[256];       /* data column name */
    char logical[64];    /* logical-type name (validated against the manifest) */
    bool iskey;          /* q keyed-table key column */
} qd_desc_t;

/* Descriptor refinement: a row refines identity IFF its logical exists AND
 * agrees with the column's physical type; anything else DEGRADES. */
static const qd_tmap_t* qd_pick_read_row(const char* cname, duck_type dt,
                                         const qd_desc_t* desc, int64_t ndesc) {
    const qd_tmap_t* identity = qd_map_read(dt);
    for (int64_t i = 0; i < ndesc; i++) {
        if (strcmp(desc[i].col, cname) != 0) continue;
        const qd_tmap_t* refined = qd_map_logical(desc[i].logical,
                                                  strlen(desc[i].logical));
        if (refined && refined->dk_type == dt) return refined;
        break;
    }
    return identity;
}

/* duck_result -> q table (does NOT destroy the result); desc = _q_schema rows. */
static ray_t* qd_result_to_table(duck_result* res, const qd_desc_t* desc,
                                 int64_t ndesc) {
    int64_t ncols = (int64_t)QAPI.column_count(res);
    if (ncols == 0) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    if (ncols > 256) return q_err(QE_DUCKDB);

    const qd_tmap_t* tms[256];
    for (int64_t c = 0; c < ncols; c++) {
        duck_type dt = QAPI.column_type(res, (duck_idx_t)c);
        const char* cname = QAPI.column_name(res, (duck_idx_t)c);
        tms[c] = qd_pick_read_row(cname, dt, desc, ndesc);
        if (!tms[c]) return q_err(QE_DUCKDB);
    }

    ray_t* cols[256];
    for (int64_t c = 0; c < ncols; c++) {
        cols[c] = (qd_surface_type(tms[c]) == RAY_LIST) ? ray_list_new(8)
                : (tms[c]->ray_type == RAY_SYM) ? ray_sym_vec_new(RAY_SYM_W64, 8)
                                                : ray_vec_new(tms[c]->ray_type, 8);
        if (!cols[c] || RAY_IS_ERR(cols[c])) {
            for (int64_t k = 0; k < c; k++) ray_release(cols[k]);
            return cols[c] ? cols[c] : q_err(QE_WSFULL);
        }
    }

    duck_data_chunk chunk;
    while ((chunk = QAPI.fetch_chunk(*res)) != NULL) {
        duck_idx_t n = QAPI.data_chunk_get_size(chunk);
        for (int64_t c = 0; c < ncols; c++) {
            duck_vector dv = QAPI.data_chunk_get_vector(chunk, (duck_idx_t)c);
            cols[c] = qd_read_cells(cols[c], tms[c], dv, n);
            if (!cols[c] || RAY_IS_ERR(cols[c])) {
                ray_t* e = cols[c] ? cols[c] : q_err(QE_WSFULL);
                for (int64_t k = 0; k < ncols; k++)
                    if (k != c && cols[k]) ray_release(cols[k]);
                QAPI.destroy_data_chunk(&chunk);
                return e;
            }
        }
        QAPI.destroy_data_chunk(&chunk);
    }

    ray_t* tbl = ray_table_new(ncols);
    for (int64_t c = 0; c < ncols; c++) {
        const char* nm = QAPI.column_name(res, (duck_idx_t)c);
        tbl = ray_table_add_col(tbl, ray_sym_intern_runtime(nm, strlen(nm)), cols[c]);
        ray_release(cols[c]);   /* add_col retains its own ref */
        if (!tbl || RAY_IS_ERR(tbl)) {
            for (int64_t k = c + 1; k < ncols; k++) ray_release(cols[k]);
            return tbl ? tbl : q_err(QE_WSFULL);
        }
    }
    return tbl;
}

/* Run one statement; error => owned 'duckdb (result destroyed), else fills
 * *out (caller destroys).  stash=0: internal probes never pollute err[]. */
static ray_t* qd_run2(int slot, const char* sql, duck_result* out, int stash) {
    if (QAPI.query(g_cons[slot].con, sql, out) != QDuckSuccess) {
        if (stash) qd_err_stash(slot, QAPI.result_error(out));
        QAPI.destroy_result(out);
        return q_err(QE_DUCKDB);
    }
    return NULL;
}

static ray_t* qd_run(int slot, const char* sql, duck_result* out) {
    return qd_run2(slot, sql, out, 1);
}

static ray_t* qd_exec_stmt(int slot, const char* sql) {
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

/* ---- _q_schema descriptor sidecar: ONE table per database, rows keyed
 * (tbl;col), shape `tbl col logical iskey valid v` — a REFINEMENT of the
 * physical catalog, never a second source of truth.  `valid` is reserved
 * for the wire arm (always '' here); v = format version 1; append-only. */

#define QD_SCHEMA_TBL "_q_schema"

static bool qd_reserved_name(const char* tname) {
    return strcmp(tname, QD_SCHEMA_TBL) == 0;
}

static const char* qd_text_cell(ray_t* col, int64_t i, size_t* len) {
    if (!col || col->type != RAY_LIST) return NULL;
    void* pp = ray_vec_get(col, i);
    ray_t* cell = pp ? *(ray_t**)pp : NULL;
    if (!cell || cell->type != RAY_CHARV) return NULL;
    *len = (size_t)ray_len(cell);
    return (const char*)ray_data(cell);
}

/* Fetch a table's descriptor rows; ANY sidecar trouble degrades to 0 rows. */
static int64_t qd_fetch_desc(int slot, const char* tname,
                             qd_desc_t* out, int64_t cap) {
    qd_buf b = {0};
    qd_puts(&b, "SELECT col, logical, iskey FROM \"" QD_SCHEMA_TBL
                "\" WHERE tbl = ");
    qd_put_strlit(&b, tname, strlen(tname));
    if (b.oom) { qd_buf_free(&b); return 0; }
    duck_result res;
    ray_t* e = qd_run2(slot, b.p, &res, 0);   /* probe: no-sidecar is normal */
    qd_buf_free(&b);
    if (e) { ray_release(e); return 0; }
    ray_t* rows = qd_result_to_table(&res, NULL, 0);
    QAPI.destroy_result(&res);
    if (!rows || RAY_IS_ERR(rows) || rows->type != RAY_TABLE) {
        if (rows) ray_release(rows);
        return 0;
    }
    int64_t n = ray_table_nrows(rows);
    if (n > cap) n = 0;
    ray_t* cv = ray_table_get_col_idx(rows, 0);   /* borrowed text col */
    ray_t* lv = ray_table_get_col_idx(rows, 1);
    ray_t* kv = ray_table_get_col_idx(rows, 2);   /* borrowed BOOL */
    for (int64_t i = 0; i < n; i++) {
        size_t cl = 0, ll = 0;
        const char* cn = qd_text_cell(cv, i, &cl);
        const char* ln = qd_text_cell(lv, i, &ll);
        snprintf(out[i].col, sizeof out[i].col, "%.*s", (int)cl, cn ? cn : "");
        snprintf(out[i].logical, sizeof out[i].logical, "%.*s", (int)ll, ln ? ln : "");
        out[i].iskey = kv && kv->type == RAY_BOOL &&
                       *(uint8_t*)ray_vec_get(kv, i) != 0;
    }
    ray_release(rows);
    return n;
}

/* One descriptor row to write (name is NOT NUL-terminated: ptr+len). */
typedef struct {
    const char* name;
    size_t      namelen;
    const char* logical;
    bool        iskey;
} qd_descrow_t;

/* Replace a table's descriptor rows (runs INSIDE the caller's transaction). */
static ray_t* qd_write_desc_rows(int slot, const char* tname,
                                 const qd_descrow_t* rows, int64_t n) {
    ray_t* e = qd_exec_stmt(slot,
        "CREATE TABLE IF NOT EXISTS \"" QD_SCHEMA_TBL "\"("
        "tbl VARCHAR, col VARCHAR, logical VARCHAR, "
        "iskey BOOLEAN, valid VARCHAR, v BIGINT)");
    if (e) return e;

    qd_buf b = {0};
    qd_puts(&b, "DELETE FROM \"" QD_SCHEMA_TBL "\" WHERE tbl = ");
    qd_put_strlit(&b, tname, strlen(tname));
    if (b.oom) { qd_buf_free(&b); return q_err(QE_WSFULL); }
    e = qd_exec_stmt(slot, b.p);
    qd_buf_free(&b);
    if (e) return e;

    if (n == 0) return NULL;
    qd_buf ins = {0};
    qd_puts(&ins, "INSERT INTO \"" QD_SCHEMA_TBL "\" VALUES ");
    for (int64_t i = 0; i < n; i++) {
        if (i) qd_puts(&ins, ", ");
        qd_puts(&ins, "(");
        qd_put_strlit(&ins, tname, strlen(tname));
        qd_puts(&ins, ", ");
        qd_put_strlit(&ins, rows[i].name, rows[i].namelen);
        qd_puts(&ins, ", ");
        qd_put_strlit(&ins, rows[i].logical, strlen(rows[i].logical));
        qd_puts(&ins, rows[i].iskey ? ", TRUE, '', 1)" : ", FALSE, '', 1)");
    }
    if (ins.oom) { qd_buf_free(&ins); return q_err(QE_WSFULL); }
    e = qd_exec_stmt(slot, ins.p);
    qd_buf_free(&ins);
    return e;
}

/* Every column gets a row — identity refinements included (set path). */
static ray_t* qd_write_desc(int slot, const char* tname, ray_t* tbl,
                            const qd_tmap_t* const* tms, const bool* iskey) {
    int64_t ncols = ray_table_ncols(tbl);
    qd_descrow_t rows[256];
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        rows[c].name    = nm ? ray_str_ptr(nm) : "?";
        rows[c].namelen = nm ? ray_str_len(nm) : 1;
        rows[c].logical = tms[c]->logical;
        rows[c].iskey   = iskey[c];
    }
    return qd_write_desc_rows(slot, tname, rows, ncols);
}

/* Rebuild the keyed shape from VALIDATING iskey rows (keyedness has no
 * physical truth — no DuckDB PRIMARY KEY); all-key/no-key stay plain.
 * Consumes tbl. */
static ray_t* qd_rekey(ray_t* tbl, const qd_desc_t* desc, int64_t ndesc) {
    if (!tbl || RAY_IS_ERR(tbl) || tbl->type != RAY_TABLE) return tbl;
    int64_t ncols = ray_table_ncols(tbl);
    if (ncols > 256) return tbl;
    bool iskey[256];
    int64_t nkey = 0;
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* nm  = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        ray_t* col = ray_table_get_col_idx(tbl, c);             /* borrowed */
        iskey[c] = false;
        for (int64_t i = 0; nm && col && i < ndesc; i++) {
            if (strlen(desc[i].col) != ray_str_len(nm) ||
                memcmp(desc[i].col, ray_str_ptr(nm), ray_str_len(nm)) != 0)
                continue;
            const qd_tmap_t* refined = qd_map_logical(desc[i].logical,
                                                      strlen(desc[i].logical));
            iskey[c] = desc[i].iskey && refined &&
                       qd_surface_type(refined) == col->type;
            break;
        }
        if (iskey[c]) nkey++;
    }
    if (nkey == 0 || nkey == ncols) return tbl;
    ray_t* kt = ray_table_new(nkey);
    ray_t* vt = ray_table_new(ncols - nkey);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t** dst = iskey[c] ? &kt : &vt;
        *dst = ray_table_add_col(*dst, ray_table_col_name(tbl, c),
                                 ray_table_get_col_idx(tbl, c));  /* retains */
    }
    ray_release(tbl);
    if (!kt || RAY_IS_ERR(kt) || !vt || RAY_IS_ERR(vt)) {
        if (kt && !RAY_IS_ERR(kt)) ray_release(kt);
        if (vt && !RAY_IS_ERR(vt)) ray_release(vt);
        return q_err(QE_WSFULL);
    }
    return ray_dict_new(kt, vt);   /* consumes both */
}

static void qd_set_invalid(duck_vector dv, duck_idx_t r) {
    QAPI.vector_ensure_validity_writable(dv);
    QAPI.validity_set_row_invalid(QAPI.vector_get_validity(dv), r);
}

static ray_t* qd_write_cells(duck_vector dv, ray_t* col, const qd_tmap_t* tm,
                             int64_t base, int64_t n) {
    void* data = QAPI.vector_get_data(dv);
    for (int64_t i = 0; i < n; i++) {
        int64_t src = base + i;
        duck_idx_t r = (duck_idx_t)i;
        switch (tm->ray_type) {
            case RAY_BOOL:
                ((uint8_t*)data)[r] = *(uint8_t*)ray_vec_get(col, src) ? 1 : 0;
                break;
            case RAY_BYTE_ONLY:
                ((uint8_t*)data)[r] = *(uint8_t*)ray_vec_get(col, src);
                break;
            case RAY_I16: {
                int16_t v = *(int16_t*)ray_vec_get(col, src);
                if (v == NULL_I16) qd_set_invalid(dv, r);
                else               ((int16_t*)data)[r] = v;
                break;
            }
            case RAY_I32:
            case RAY_MONTH:
            case RAY_MINUTE:
            case RAY_SECOND: {
                int32_t v = *(int32_t*)ray_vec_get(col, src);
                if (v == NULL_I32) qd_set_invalid(dv, r);
                else               ((int32_t*)data)[r] = v;
                break;
            }
            case RAY_I64:
            case RAY_TIMESPAN: {
                int64_t v = *(int64_t*)ray_vec_get(col, src);
                if (v == NULL_I64) qd_set_invalid(dv, r);
                else               ((int64_t*)data)[r] = v;
                break;
            }
            case RAY_F32: {
                float v = *(float*)ray_vec_get(col, src);
                if (v != v) qd_set_invalid(dv, r);       /* 0Ne -> NULL */
                else        ((float*)data)[r] = v;
                break;
            }
            case RAY_F64:
            case RAY_DATETIME: {
                double v = *(double*)ray_vec_get(col, src);
                if (v != v) qd_set_invalid(dv, r);       /* 0n/0Nz -> NULL */
                else        ((double*)data)[r] = v;
                break;
            }
            case RAY_SYM: {      /* ` (sym 0 / empty) writes '' — a value */
                ray_t* s = ray_sym_vec_cell(col, src);   /* borrowed */
                QAPI.vector_assign_string_element_len(dv, r,
                        s ? ray_str_ptr(s) : "", s ? (duck_idx_t)ray_str_len(s) : 0);
                break;
            }
            case RAY_STR: {      /* string column: text cells, or physical STR */
                if (col->type == RAY_LIST) {
                    void* pp = ray_vec_get(col, src);
                    ray_t* cell = pp ? *(ray_t**)pp : NULL;
                    const char* tp; int64_t tn;
                    if (!cell || !q_str_text_bytes(cell, &tp, &tn))
                        return q_err(QE_DUCKDB);
                    QAPI.vector_assign_string_element_len(dv, r, tp ? tp : "",
                                                          (duck_idx_t)tn);
                    break;
                }
                size_t len = 0;
                const char* sp = ray_str_vec_get(col, src, &len);
                QAPI.vector_assign_string_element_len(dv, r, sp ? sp : "",
                                                      (duck_idx_t)len);
                break;
            }
            case RAY_LIST: {     /* byte-vector cell -> BLOB */
                void* pp = ray_vec_get(col, src);
                ray_t* cell = pp ? *(ray_t**)pp : NULL;
                if (!cell || cell->type != RAY_BYTE_ONLY)
                    return q_err(QE_DUCKDB);
                const char* bp = cell->len ? (const char*)ray_vec_get(cell, 0) : "";
                QAPI.vector_assign_string_element_len(dv, r, bp,
                                                      (duck_idx_t)cell->len);
                break;
            }
            case RAY_DATE: {
                int32_t v = *(int32_t*)ray_vec_get(col, src);
                if (v == NULL_I32) { qd_set_invalid(dv, r); break; }
                if (v > INT32_MAX - QD_EPOCH_DAYS)   /* epoch shift overflow */
                    return q_err(QE_DUCKDB);
                ((int32_t*)data)[r] = v + QD_EPOCH_DAYS;
                break;
            }
            case RAY_TIME: {
                int32_t v = *(int32_t*)ray_vec_get(col, src);
                if (v == NULL_I32) qd_set_invalid(dv, r);
                else               ((int64_t*)data)[r] = (int64_t)v * 1000;  /* ms -> µs */
                break;
            }
            case RAY_TIMESTAMP: {
                int64_t v = *(int64_t*)ray_vec_get(col, src);
                if (v == NULL_I64) { qd_set_invalid(dv, r); break; }
                if (v > INT64_MAX - QD_EPOCH_NS)     /* epoch shift overflow */
                    return q_err(QE_DUCKDB);
                ((int64_t*)data)[r] = v + QD_EPOCH_NS;
                break;
            }
            case RAY_GUID: {
                const uint8_t* b = (const uint8_t*)ray_vec_get(col, src);
                if (qd_guid_is_null(b)) qd_set_invalid(dv, r);   /* 0Ng -> NULL */
                else ((duck_hugeint*)data)[r] = qd_bytes_to_uuid(b);
                break;
            }
            default:
                return q_err(QE_DUCKDB);
        }
    }
    return NULL;
}

/* Append a q table through the appender in vector-size chunks (tms[] pre-validated). */
static ray_t* qd_append_table(int slot, const char* tname,
                              ray_t* tbl, const qd_tmap_t* const* tms) {
    int64_t ncols = ray_table_ncols(tbl);
    int64_t nrows = ray_table_nrows(tbl);

    duck_appender app = NULL;
    if (QAPI.appender_create_ext(g_cons[slot].con, NULL, "main", tname, &app)
            != QDuckSuccess) {
        qd_err_stash(slot, QAPI.appender_error(app));
        QAPI.appender_destroy(&app);
        return q_err(QE_DUCKDB);
    }

    duck_logical_type ltypes[256];
    for (int64_t c = 0; c < ncols; c++)
        ltypes[c] = QAPI.create_logical_type(tms[c]->dk_type);
    duck_data_chunk chunk = QAPI.create_data_chunk(ltypes, (duck_idx_t)ncols);

    ray_t* err = NULL;
    int64_t vecsz = (int64_t)QAPI.vector_size();
    if (vecsz <= 0) vecsz = 2048;
    for (int64_t base = 0; base < nrows && !err; base += vecsz) {
        int64_t n = nrows - base < vecsz ? nrows - base : vecsz;
        QAPI.data_chunk_reset(chunk);
        for (int64_t c = 0; c < ncols && !err; c++) {
            ray_t* col = ray_table_get_col_idx(tbl, c);   /* borrowed */
            err = qd_write_cells(QAPI.data_chunk_get_vector(chunk, (duck_idx_t)c),
                                 col, tms[c], base, n);
        }
        if (!err) {
            QAPI.data_chunk_set_size(chunk, (duck_idx_t)n);
            if (QAPI.append_data_chunk(app, chunk) != QDuckSuccess) {
                qd_err_stash(slot, QAPI.appender_error(app));
                err = q_err(QE_DUCKDB);
            }
        }
    }

    QAPI.destroy_data_chunk(&chunk);
    for (int64_t c = 0; c < ncols; c++) QAPI.destroy_logical_type(&ltypes[c]);

    /* explicit flush first: destroy frees the error text (codex round 3) */
    if (!err && QAPI.appender_flush(app) != QDuckSuccess) {
        qd_err_stash(slot, QAPI.appender_error(app));
        err = q_err(QE_DUCKDB);
    }
    if (QAPI.appender_destroy(&app) != QDuckSuccess && !err)
        err = q_err(QE_DUCKDB);
    return err;
}

/* Validate every column against the write manifest; fills tms.  Overlong names
 * would silently truncate in qd_desc_t and degrade refinement — rejected. */
static ray_t* qd_check_table(ray_t* tbl, const qd_tmap_t** tms) {
    int64_t ncols = ray_table_ncols(tbl);
    if (ncols > 256) return q_err(QE_DUCKDB);
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);   /* borrowed */
        ray_t* nm  = ray_sym_str(ray_table_col_name(tbl, c));
        if (nm && ray_str_len(nm) >= 256) return q_err(QE_DUCKDB);
        tms[c] = col ? qd_map_write(col) : NULL;
        if (!tms[c]) return q_err(QE_DUCKDB);
    }
    return NULL;
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
            qd_err_stash(-1, open_err);
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


/* Catalog data_type string (base name before '(') -> canonical read row —
 * THE one owner of catalog spellings, meta and schema-check both ride it. */
static const qd_tmap_t* qd_catalog_row(const char* dt, size_t n) {
    size_t base = 0;
    while (base < n && dt[base] != '(') base++;
    /* DuckDB spells REAL as FLOAT in catalog output */
    if (base == 5 && strncmp(dt, "FLOAT", 5) == 0) { dt = "REAL"; base = 4; }
    for (size_t i = 0; i < QD_NTYPES; i++) {
        if (!QD_TYPES[i].read_canon) continue;
        if (strlen(QD_TYPES[i].sql) == base &&
            strncmp(QD_TYPES[i].sql, dt, base) == 0)
            return &QD_TYPES[i];
    }
    return NULL;
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
    ray_t* cat = qd_result_to_table(&res, NULL, 0);
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
        const qd_tmap_t* tm = qd_catalog_row(dt ? dt : "", dl);
        if (tm) tm = qd_pick_read_row(cname, tm->dk_type, desc, ndesc);
        tbuf[i] = tm ? tm->meta_ch : ' ';
        int64_t id = ray_sym_intern_runtime(nm ? nm : "", ln);
        cvec = ray_vec_append(cvec, &id);
        fvec = ray_vec_append(fvec, &blank);
        avec = ray_vec_append(avec, &blank);
    }
    ray_release(cat);
    ray_t* tstr = ray_str(tbuf, (size_t)nrows);
    if (!cvec || RAY_IS_ERR(cvec) || !fvec || RAY_IS_ERR(fvec) ||
        !avec || RAY_IS_ERR(avec) || !tstr || RAY_IS_ERR(tstr)) {
        if (cvec && !RAY_IS_ERR(cvec)) ray_release(cvec);
        if (fvec && !RAY_IS_ERR(fvec)) ray_release(fvec);
        if (avec && !RAY_IS_ERR(avec)) ray_release(avec);
        if (tstr && !RAY_IS_ERR(tstr)) ray_release(tstr);
        return q_err(QE_WSFULL);
    }
    return q_table_meta_assemble(cvec, tstr, fvec, avec);
}

/* set body over the flattened table: DDL + data + descriptor, ONE transaction. */
static ray_t* qd_set_impl(int slot, const char* tname, ray_t* tbl,
                          const bool* iskey) {
    const qd_tmap_t* tms[256];
    ray_t* e = qd_check_table(tbl, tms);
    if (e) return e;

    int64_t ncols = ray_table_ncols(tbl);
    qd_buf b = {0};
    qd_puts(&b, "CREATE OR REPLACE TABLE ");
    qd_put_ident(&b, tname, strlen(tname));
    qd_puts(&b, "(");
    for (int64_t c = 0; c < ncols; c++) {
        if (c) qd_puts(&b, ", ");
        ray_t* nm = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        qd_put_ident(&b, nm ? ray_str_ptr(nm) : "?", nm ? ray_str_len(nm) : 1);
        qd_puts(&b, " ");
        qd_puts(&b, tms[c]->sql);
    }
    qd_puts(&b, ")");
    if (b.oom) { qd_buf_free(&b); return q_err(QE_WSFULL); }

    if ((e = qd_exec_stmt(slot, "BEGIN TRANSACTION"))) { qd_buf_free(&b); return e; }
    if ((e = qd_exec_stmt(slot, b.p))) { qd_buf_free(&b); qd_rollback(slot); return e; }
    qd_buf_free(&b);
    if ((e = qd_append_table(slot, tname, tbl, tms))) { qd_rollback(slot); return e; }
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
    ray_t* r = qd_set_impl(slot, tname, tbl, iskey);
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
    ray_t* tbl = qd_result_to_table(&res, desc, ndesc);
    QAPI.destroy_result(&res);
    return qd_rekey(tbl, desc, ndesc);
}

/* Append's schema check: names, order and canonical types must match the
 * catalog; a VALID descriptor row pins the LOGICAL type too (a string column
 * can't append into a symbol column). */
static ray_t* qd_schema_check(int slot, const char* tname, ray_t* tbl,
                              const qd_tmap_t* const* tms) {
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
    ray_t* cat = qd_result_to_table(&res, NULL, 0);
    QAPI.destroy_result(&res);
    if (!cat || RAY_IS_ERR(cat)) return cat;

    int64_t nrows = ray_table_nrows(cat);
    int64_t ncols = ray_table_ncols(tbl);
    if (nrows == 0 || nrows != ncols) {
        ray_release(cat);
        return q_err(QE_DUCKDB);
    }
    ray_t* names  = ray_table_get_col_idx(cat, 0);   /* borrowed text cols */
    ray_t* dtypes = ray_table_get_col_idx(cat, 1);
    for (int64_t c = 0; c < ncols; c++) {
        size_t nl = 0, dl = 0;
        const char* nm = qd_text_cell(names, c, &nl);
        const char* dt = qd_text_cell(dtypes, c, &dl);
        ray_t* qn = ray_sym_str(ray_table_col_name(tbl, c));   /* borrowed */
        size_t ql = qn ? ray_str_len(qn) : 0;
        if (!nm || !qn || ql != nl || strncmp(nm, ray_str_ptr(qn), nl) != 0) {
            ray_release(cat);
            return q_err(QE_DUCKDB);
        }
        const qd_tmap_t* cr = qd_catalog_row(dt ? dt : "", dl);
        if (!cr || cr->dk_type != tms[c]->dk_type) {
            ray_release(cat);
            return q_err(QE_DUCKDB);
        }
        for (int64_t i = 0; i < ndesc; i++) {
            if (strlen(desc[i].col) != nl || memcmp(desc[i].col, nm, nl) != 0)
                continue;
            const qd_tmap_t* refined = qd_map_logical(desc[i].logical,
                                                      strlen(desc[i].logical));
            if (refined && refined->dk_type == cr->dk_type &&
                strcmp(tms[c]->logical, refined->logical) != 0) {
                ray_release(cat);
                return q_err(QE_DUCKDB);
            }
            break;
        }
    }
    ray_release(cat);
    return NULL;
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

    const qd_tmap_t* tms[256];
    ray_t* e = qd_check_table(tbl, tms);
    if (e) return e;
    if ((e = qd_schema_check(slot, tname, tbl, tms))) return e;

    if ((e = qd_exec_stmt(slot, "BEGIN TRANSACTION"))) return e;
    if ((e = qd_append_table(slot, tname, tbl, tms))) { qd_rollback(slot); return e; }
    if ((e = qd_exec_stmt(slot, "COMMIT"))) { qd_rollback(slot); return e; }
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
    else out = qd_result_to_table(&res, NULL, 0);
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

/* The INTERNAL native surface (`.duckdb.i.*`) the lib/duckdb.q provider hooks
 * are written over — the connection, the typed round-trip and one raw exec.
 * The public bespoke API (connect/sql/select/connections/version/err) was
 * REPLACED 2026-08-07 by the `:pq:duckdb:` virtual-table surface, not wrapped.
 * Registration fires from the `\l pq` gate, so the pre-gate env is kdb-clean. */
void q_duckdb_register(void) {
    /* NO dlopen here — the library is resolved lazily on first use. */
    qd_bind_unary(".duckdb.i.open",   qd_connect_fn);
    qd_bind_unary(".duckdb.i.close",  qd_close_fn);
    qd_bind_vary (".duckdb.i.exec",   qd_sql_wrap);
    qd_bind_vary (".duckdb.i.get",    qd_get_wrap);
    qd_bind_vary (".duckdb.i.set",    qd_set_wrap);
    qd_bind_vary (".duckdb.i.append", qd_append_wrap);
    qd_bind_vary (".duckdb.i.meta",   qd_meta_wrap);
}
