/* q_wasm_duckdb — the duck_api_t table q_duckdb.c drives, bound to DuckDB's own published wasm build: a second
 * module with its own memory, loaded by the host on the first DuckDB call (wasm/duck-loader.js).  Handles pass
 * through as opaque 32-bit values; strings, out-params and each duckdb_result are copied across around the call;
 * vector buffers are mirrored into this heap on first read and written back before the chunk is sized or appended.
 * The one database is DuckDB-wasm's WebDB.  No q semantics live here: the bridge above it is the desktop one. */
#define _POSIX_C_SOURCE 200809L   /* strdup */
#include "qlang/io/q_duckdb_api.h"
#include "q_wasm_buf.h"
#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t tp;   /* an address in DuckDB's memory */

/* 1 once the host has loaded DuckDB.  A failure is printed once and remembered until the engine restarts: each
 * attempt fetches tens of megabytes. */
EM_JS(int, dk_load, (const char* version), {
    if (Module.peachqDuck) return 1;
    if (typeof Module.peachqDuckLoad !== "function" || Module.peachqDuckFailed) return 0;
    try {
        Module.peachqDuck = Module.peachqDuckLoad(Module, UTF8ToString(version));
    } catch (e) {
        Module.peachqDuckFailed = true;
        Module.printErr("duckdb: " + ((e && e.message) || e));
        return 0;
    }
    Module.peachqDuck.fn = {};
    return 1;
});

EM_JS(int32_t, dk_inv, (const char* name, const uint32_t* a, int n), {
    var D = Module.peachqDuck;
    var f = D.fn[name] || (D.fn[name] = D.X[UTF8ToString(name)]);
    var args = [];
    for (var i = 0; i < n; i++) args.push(HEAPU32[(a >> 2) + i] | 0);
    var r = f.apply(null, args);
    return typeof r === "number" ? r | 0 : (r ? 1 : 0);
});

EM_JS(void, dk_before_query, (void), { Module.peachqDuck.beforeQuery(); });

EM_JS(uint32_t, dk_hi, (void), { return Module.peachqDuck.X.getTempRet0() >>> 0; });

EM_JS(void, dk_get, (void* dst, tp src, uint32_t n), {
    HEAPU8.set(new Uint8Array(Module.peachqDuck.mem.buffer, src, n), dst);
});

EM_JS(void, dk_put, (tp dst, const void* src, uint32_t n), {
    new Uint8Array(Module.peachqDuck.mem.buffer, dst, n).set(HEAPU8.subarray(src, src + n));
});

EM_JS(uint32_t, dk_strlen, (tp p), {
    var h = new Uint8Array(Module.peachqDuck.mem.buffer);
    var e = p;
    while (h[e]) e++;
    return e - p;
});

EM_JS(uint32_t, dk_memsize, (void), { return Module.peachqDuck.mem.buffer.byteLength >>> 0; });

/* (Re)open the WebDB at path (NULL: in memory) with the config pairs `opts` (n bytes of name NUL value NUL ...).
 * 1 ok; else 0 with *err DuckDB's message, malloc'd in this heap. */
EM_JS(int, dk_web_open, (const char* path, const char* opts, uint32_t n, char** err), {
    var msg;
    try {
        var f = n ? new TextDecoder().decode(HEAPU8.subarray(opts, opts + n - 1)).split("\0") : [];
        var pairs = [];
        for (var i = 0; i + 1 < f.length; i += 2) pairs.push([f[i], f[i + 1]]);
        Module.peachqDuck.open(path ? UTF8ToString(path) : null, pairs);
        return 1;
    } catch (e) {
        msg = new TextEncoder().encode(String((e && e.message) || e));
    }
    var p = _malloc(msg.length + 1);
    if (p) {
        HEAPU8.set(msg, p);
        HEAPU8[p + msg.length] = 0;
    }
    HEAPU32[err >> 2] = p;
    return 0;
});

EM_JS(void, dk_web_close, (void), { Module.peachqDuck.close(); });

EM_JS(uint32_t, dk_web_connect, (void), {
    try { return Module.peachqDuck.connect() >>> 0; } catch (e) { return 0; }
});

EM_JS(void, dk_web_disconnect, (uint32_t con), { Module.peachqDuck.disconnect(con >>> 0); });

#define NARG(...) (sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t))
#define DK(nm, ...) dk_inv("duckdb_" nm, (const uint32_t[]){__VA_ARGS__}, (int)NARG(__VA_ARGS__))
#define DK0(nm) dk_inv("duckdb_" nm, (const uint32_t[]){0}, 0)
#define H(x) ((uint32_t)(uintptr_t)(x))
#define LO(x) ((uint32_t)(uint64_t)(x))
#define HI(x) ((uint32_t)((uint64_t)(x) >> 32))
#define U64(lo) u64_of((uint32_t)(lo))

static tp scratch;   /* 256 bytes of DuckDB's memory for out-params; a duckdb_result crosses at TRES */

/* Out of memory aborts the module (the Worker reports it and restart() recovers): a half-built mirror would hand the
 * bridge bytes that were never copied. */
static void* xrealloc(void* p, size_t n) {
    void* r = realloc(p, n ? n : 1);
    if (!r) abort();
    return r;
}

/* the call must run before its high half is read: an operand order would be unspecified */
static uint64_t u64_of(uint32_t lo) { return ((uint64_t)dk_hi() << 32) | lo; }

static tp tmalloc(uint32_t n) { return (tp)dk_inv("malloc", (const uint32_t[]){n}, 1); }
static void tfree(tp p) { if (p) dk_inv("free", (const uint32_t[]){p}, 1); }
static uint32_t get32(tp p) { uint32_t v; dk_get(&v, p, 4); return v; }
static void put32(tp p, uint32_t v) { dk_put(p, &v, 4); }

static tp tstr(const char* s) {
    if (!s) return 0;
    uint32_t n = (uint32_t)strlen(s) + 1;
    tp p = tmalloc(n);
    dk_put(p, s, n);
    return p;
}

static char* ostr(tp p) {
    if (!p) return NULL;
    uint32_t n = dk_strlen(p);
    char* s = xrealloc(NULL, (size_t)n + 1);
    dk_get(s, p, n);
    s[n] = 0;
    return s;
}

static char* ostr_owned(tp p) {
    char* s = ostr(p);
    if (p) DK("free", p);
    return s;
}

typedef struct { uintptr_t key; char* s; } borrowed_t;
static borrowed_t* g_bor;
static size_t g_nbor, g_capbor;

static const char* borrowed(uintptr_t key, tp p) {
    if (!p) return NULL;
    if (g_nbor == g_capbor) {
        g_capbor = g_capbor ? g_capbor * 2 : 64;
        g_bor = xrealloc(g_bor, g_capbor * sizeof *g_bor);
    }
    g_bor[g_nbor] = (borrowed_t){ key, ostr(p) };
    return g_bor[g_nbor++].s;
}

static void borrowed_drop(uintptr_t key) {
    size_t j = 0;
    for (size_t i = 0; i < g_nbor; i++) {
        if (g_bor[i].key == key) free(g_bor[i].s);
        else g_bor[j++] = g_bor[i];
    }
    g_nbor = j;
}

enum { VK_TOP, VK_LIST, VK_STRUCT, VK_ARRAY };

typedef struct {
    tp handle, root, parent;
    int kind;
    uint32_t cap, width;
    bool strtype, have_data, have_val;
    tp tdata, tval;
    void* odata;
    uint64_t* oval;
    char* arena;
} vinfo_t;

static vinfo_t* g_v;
static size_t g_nv, g_capv;
static tp* g_wchunks;
static size_t g_nw, g_capw;

static bool is_write_chunk(tp c) {
    for (size_t i = 0; i < g_nw; i++) if (g_wchunks[i] == c) return true;
    return false;
}

static void mirror_free(vinfo_t* v) {
    free(v->odata); free(v->oval); free(v->arena);
    v->odata = NULL; v->oval = NULL; v->arena = NULL;
    v->have_data = v->have_val = false;
}

static vinfo_t* vfind(tp h) {
    for (size_t i = 0; i < g_nv; i++) if (g_v[i].handle == h) return &g_v[i];
    return NULL;
}

static vinfo_t* vreg(tp h, tp parent, int kind) {
    if (!h) return NULL;
    vinfo_t* p = parent ? vfind(parent) : NULL;
    tp root = kind == VK_TOP ? parent : (p ? p->root : 0);
    vinfo_t* v = vfind(h);
    if (v) {
        if (v->root == root && v->parent == (kind == VK_TOP ? 0 : parent)) return v;
        mirror_free(v);
    } else {
        if (g_nv == g_capv) {
            g_capv = g_capv ? g_capv * 2 : 64;
            g_v = xrealloc(g_v, g_capv * sizeof *g_v);
        }
        v = &g_v[g_nv++];
    }
    memset(v, 0, sizeof *v);
    v->handle = h;
    v->root = root;
    v->parent = kind == VK_TOP ? 0 : parent;
    v->kind = kind;
    return v;
}

/* A string vector is written through assign_string_element_len, never from its mirror. */
static void flush_one(vinfo_t* v) {
    if (v->have_data && v->odata && !v->strtype && v->width) dk_put(v->tdata, v->odata, v->cap * v->width);
    if (v->have_val && v->oval) dk_put(v->tval, v->oval, ((v->cap + 63) / 64) * 8);
}

static bool descends(const vinfo_t* v, tp anc) {
    for (tp p = v->parent; p; ) {
        if (p == anc) return true;
        vinfo_t* pv = vfind(p);
        p = pv ? pv->parent : 0;
    }
    return false;
}

static void flush_chunk(tp root) {
    if (!is_write_chunk(root)) return;
    for (size_t i = 0; i < g_nv; i++) if (g_v[i].root == root) flush_one(&g_v[i]);
}

static void drop_chunk(tp root) {
    size_t j = 0;
    for (size_t i = 0; i < g_nv; i++) {
        if (g_v[i].root == root) mirror_free(&g_v[i]);
        else g_v[j++] = g_v[i];
    }
    g_nv = j;
}

static uint32_t type_width(tp lt, bool* str) {
    uint32_t id = (uint32_t)DK("get_type_id", lt);
    *str = false;
    switch (id) {
        case QDUCK_TYPE_BOOLEAN: case QDUCK_TYPE_TINYINT: case QDUCK_TYPE_UTINYINT: return 1;
        case QDUCK_TYPE_SMALLINT: case QDUCK_TYPE_USMALLINT: return 2;
        case QDUCK_TYPE_INTEGER: case QDUCK_TYPE_UINTEGER: case QDUCK_TYPE_FLOAT: case QDUCK_TYPE_DATE: return 4;
        case QDUCK_TYPE_BIGINT: case QDUCK_TYPE_UBIGINT: case QDUCK_TYPE_DOUBLE: case QDUCK_TYPE_TIMESTAMP:
        case QDUCK_TYPE_TIMESTAMP_S: case QDUCK_TYPE_TIMESTAMP_MS: case QDUCK_TYPE_TIMESTAMP_NS: case QDUCK_TYPE_TIME:
        case QDUCK_TYPE_TIME_NS: case QDUCK_TYPE_TIME_TZ: case QDUCK_TYPE_TIMESTAMP_TZ: return 8;
        case QDUCK_TYPE_HUGEINT: case QDUCK_TYPE_UHUGEINT: case QDUCK_TYPE_UUID: case QDUCK_TYPE_INTERVAL:
        case QDUCK_TYPE_LIST: case QDUCK_TYPE_MAP: return 16;
        case QDUCK_TYPE_VARCHAR: case QDUCK_TYPE_BLOB: case QDUCK_TYPE_BIT: case QDUCK_TYPE_VARINT:
            *str = true;
            return 16;
        case QDUCK_TYPE_DECIMAL:
            switch ((uint32_t)DK("decimal_internal_type", lt)) {
                case QDUCK_TYPE_SMALLINT: return 2;
                case QDUCK_TYPE_INTEGER: return 4;
                case QDUCK_TYPE_BIGINT: return 8;
                default: return 16;
            }
        case QDUCK_TYPE_ENUM:
            switch ((uint32_t)DK("enum_internal_type", lt)) {
                case QDUCK_TYPE_UTINYINT: return 1;
                case QDUCK_TYPE_USMALLINT: return 2;
                default: return 4;
            }
        default: return 0;
    }
}

/* The rows a vector's buffers hold: its chunk's (vector_size while we fill it), a list child's list size, a struct
 * child's parent's, an array child's parent's times the array size. */
static uint32_t vcap(vinfo_t* v) {
    if (v->cap) return v->cap;
    switch (v->kind) {
        case VK_TOP:
            v->cap = is_write_chunk(v->root) ? (uint32_t)DK0("vector_size") : (uint32_t)DK("data_chunk_get_size", v->root);
            break;
        case VK_LIST:
            v->cap = (uint32_t)DK("list_vector_get_size", v->parent);
            break;
        case VK_STRUCT: {
            vinfo_t* p = vfind(v->parent);
            v->cap = p ? vcap(p) : (uint32_t)DK0("vector_size");
            break;
        }
        case VK_ARRAY: {
            vinfo_t* p = vfind(v->parent);
            tp lt = (tp)DK("vector_get_column_type", v->parent);
            uint32_t asz = (uint32_t)DK("array_type_array_size", lt);
            put32(scratch, lt);
            DK("destroy_logical_type", scratch);
            v->cap = (p ? vcap(p) : 0) * asz;
            break;
        }
    }
    return v->cap;
}

static void load_val(vinfo_t* v) {
    if (v->have_val) return;
    v->have_val = true;
    v->tval = (tp)DK("vector_get_validity", v->handle);
    if (!v->tval) return;
    uint32_t nb = ((vcap(v) + 63) / 64) * 8;
    v->oval = xrealloc(NULL, nb);
    dk_get(v->oval, v->tval, nb);
}

static void load_data(vinfo_t* v) {
    if (v->have_data) return;
    v->have_data = true;
    v->tdata = (tp)DK("vector_get_data", v->handle);
    tp lt = (tp)DK("vector_get_column_type", v->handle);
    v->width = type_width(lt, &v->strtype);
    put32(scratch, lt);
    DK("destroy_logical_type", scratch);
    uint32_t cap = vcap(v);
    if (!v->tdata || !v->width) return;
    size_t nb = (size_t)cap * v->width;
    v->odata = xrealloc(NULL, nb);
    dk_get(v->odata, v->tdata, nb);
    if (!v->strtype || is_write_chunk(v->root)) return;
    load_val(v);
    duck_string_t* s = v->odata;
    uint32_t msz = dk_memsize();
    size_t total = 0;
    for (uint32_t r = 0; r < cap; r++)
        if (q_duckdb_validity_ok(v->oval, r) && s[r].value.inlined.length > QDUCK_STRING_INLINE_MAX)
            total += s[r].value.inlined.length;
    v->arena = xrealloc(NULL, total);
    size_t at = 0;
    for (uint32_t r = 0; r < cap; r++) {
        uint32_t len = s[r].value.inlined.length;
        if (!q_duckdb_validity_ok(v->oval, r) || len <= QDUCK_STRING_INLINE_MAX) continue;
        tp src = (tp)(uintptr_t)s[r].value.pointer.ptr;
        if ((uint64_t)src + len > msz) { s[r].value.pointer.ptr = NULL; continue; }   /* never read past their heap */
        dk_get(v->arena + at, src, len);
        s[r].value.pointer.ptr = v->arena + at;
        at += len;
    }
}

static const char* s_library_version(void) {
    static char buf[64];
    char* s = ostr((tp)DK0("library_version"));
    strncpy(buf, s ? s : "", sizeof buf - 1);
    free(s);
    return buf;
}

/* A config is DuckDB's own, so an unknown option fails at set_config as it does natively, and its pairs are also kept
 * for the WebDB, which takes no duckdb_config: the loader maps them onto its open or runs them as SET. */
typedef struct { tp h; wbuf_t pairs; } cfgrec_t;
static cfgrec_t* g_cfg;
static size_t g_ncfg, g_capcfg;

static cfgrec_t* cfg_find(tp h) {
    for (size_t i = 0; i < g_ncfg; i++) if (g_cfg[i].h == h) return &g_cfg[i];
    return NULL;
}

static duck_state s_create_config(duck_config* out) {
    duck_state r = DK("create_config", scratch);
    *out = (duck_config)(uintptr_t)get32(scratch);
    if (r != QDuckSuccess) return r;
    if (g_ncfg == g_capcfg) {
        g_capcfg = g_capcfg ? g_capcfg * 2 : 4;
        g_cfg = xrealloc(g_cfg, g_capcfg * sizeof *g_cfg);
    }
    g_cfg[g_ncfg++] = (cfgrec_t){ .h = H(*out) };
    return r;
}

static duck_state s_set_config(duck_config cfg, const char* name, const char* opt) {
    tp a = tstr(name), b = tstr(opt);
    duck_state r = DK("set_config", H(cfg), a, b);
    tfree(a); tfree(b);
    cfgrec_t* c = cfg_find(H(cfg));
    if (r == QDuckSuccess && c) {
        wbuf_put(&c->pairs, name, strlen(name) + 1);
        wbuf_put(&c->pairs, opt, strlen(opt) + 1);
    }
    return r;
}

static void s_destroy_config(duck_config* cfg) {
    cfgrec_t* c = cfg_find(H(*cfg));
    if (c) {
        free(c->pairs.p);
        *c = g_cfg[--g_ncfg];
    }
    put32(scratch, H(*cfg));
    DK("destroy_config", scratch);
    *cfg = NULL;
}

/* The bridge opens exactly one database, main, and never a second while it lives (a handle is an ATTACH on it), so
 * every open is the WebDB and raw duckdb_open never runs: its httpfs has no network, and an http path invalidates it. */
#define WEB_DB ((duck_database)(uintptr_t)0xFFFFFFF0u)
static bool g_web_open;

static duck_state s_open_ext(const char* path, duck_database* out, duck_config cfg, char** out_err) {
    cfgrec_t* c = cfg ? cfg_find(H(cfg)) : NULL;
    char* err = NULL;
    *out = NULL;
    if (g_web_open) err = strdup("duckdb-wasm: the session's one database is already open");
    else if (dk_web_open(path, c ? c->pairs.p : NULL, c ? (uint32_t)c->pairs.n : 0, &err)) {
        g_web_open = true;
        *out = WEB_DB;
    }
    if (out_err) *out_err = err;
    else free(err);
    return *out ? QDuckSuccess : QDuckError;
}

static void s_close(duck_database* db) {
    if (*db == WEB_DB && g_web_open) {
        dk_web_close();
        g_web_open = false;
    }
    *db = NULL;
}

static duck_state s_connect(duck_database db, duck_connection* out) {
    *out = db == WEB_DB && g_web_open ? (duck_connection)(uintptr_t)dk_web_connect() : NULL;
    return *out ? QDuckSuccess : QDuckError;
}

static void s_disconnect(duck_connection* con) {
    if (*con) dk_web_disconnect(H(*con));
    *con = NULL;
}

static void s_destroy_logical_type(duck_logical_type* t) {
    put32(scratch, H(*t));
    DK("destroy_logical_type", scratch);
    *t = NULL;
}

#define TRES (scratch + 64)

static void res_in(duck_result* r) { dk_put(TRES, r, sizeof *r); }
static void res_out(duck_result* r) { dk_get(r, TRES, sizeof *r); }

static duck_state s_query(duck_connection con, const char* sql, duck_result* out) {
    tp q = tstr(sql);
    dk_before_query();
    memset(out, 0, sizeof *out);
    res_in(out);
    duck_state r = DK("query", H(con), q, TRES);
    res_out(out);
    tfree(q);
    return r;
}

static void s_destroy_result(duck_result* res) {
    borrowed_drop((uintptr_t)res->internal_data);
    res_in(res);
    DK("destroy_result", TRES);
    res_out(res);
}

#define RCALL(res, expr) ({ res_in(res); __auto_type r_ = (expr); res_out(res); r_; })

static duck_idx_t s_column_count(duck_result* res) { return RCALL(res, U64(DK("column_count", TRES))); }
static duck_idx_t s_row_count(duck_result* res) { return RCALL(res, U64(DK("row_count", TRES))); }

static const char* s_column_name(duck_result* res, duck_idx_t col) {
    tp s = RCALL(res, (tp)DK("column_name", TRES, LO(col), HI(col)));
    return borrowed((uintptr_t)res->internal_data, s);
}

static const char* s_result_error(duck_result* res) {
    tp s = RCALL(res, (tp)DK("result_error", TRES));
    return borrowed((uintptr_t)res->internal_data, s);
}

static duck_data_chunk s_fetch_chunk(duck_result res) {
    res_in(&res);
    return (duck_data_chunk)(uintptr_t)(tp)DK("fetch_chunk", TRES);
}

static duck_return_t s_result_return_type(duck_result res) {
    res_in(&res);
    return (duck_return_t)DK("result_return_type", TRES);
}

static void s_destroy_data_chunk(duck_data_chunk* c) {
    tp h = H(*c);
    drop_chunk(h);
    for (size_t i = 0; i < g_nw; i++) if (g_wchunks[i] == h) g_wchunks[i] = g_wchunks[--g_nw];
    put32(scratch, h);
    DK("destroy_data_chunk", scratch);
    *c = (duck_data_chunk)(uintptr_t)get32(scratch);
}

static duck_idx_t s_data_chunk_get_size(duck_data_chunk c) { return U64(DK("data_chunk_get_size", H(c))); }

static duck_vector s_data_chunk_get_vector(duck_data_chunk c, duck_idx_t col) {
    tp v = (tp)DK("data_chunk_get_vector", H(c), LO(col), HI(col));
    vreg(v, H(c), VK_TOP);
    return (duck_vector)(uintptr_t)v;
}

static void* s_vector_get_data(duck_vector vec) {
    vinfo_t* v = vfind(H(vec));
    if (!v) return NULL;
    load_data(v);
    return v->odata;
}

static uint64_t* s_vector_get_validity(duck_vector vec) {
    vinfo_t* v = vfind(H(vec));
    if (!v) return NULL;
    load_val(v);
    return v->oval;
}

/* A mirrored mask is already writable (it is flushed with the chunk); a vector that had none gets DuckDB's, which the
 * next get_validity mirrors. */
static void s_vector_ensure_validity_writable(duck_vector vec) {
    vinfo_t* v = vfind(H(vec));
    if (v && v->have_val && v->oval) return;
    if (v && v->have_val) {
        if (v->oval) dk_put(v->tval, v->oval, ((vcap(v) + 63) / 64) * 8);
        free(v->oval);
        v->oval = NULL;
        v->have_val = false;
    }
    DK("vector_ensure_validity_writable", H(vec));
}

static void s_validity_set_row_invalid(uint64_t* validity, duck_idx_t row) {
    if (validity) validity[row >> 6] &= ~(1ULL << (row & 63));
}

static void s_vector_assign_string_element_len(duck_vector vec, duck_idx_t idx, const char* str, duck_idx_t len) {
    tp p = tmalloc((uint32_t)len + 1);
    dk_put(p, str, (uint32_t)len);
    DK("vector_assign_string_element_len", H(vec), LO(idx), HI(idx), p, LO(len), HI(len));
    tfree(p);
}

static duck_idx_t s_vector_size(void) { return U64(DK0("vector_size")); }

static duck_logical_type s_create_logical_type(duck_type t) {
    return (duck_logical_type)(uintptr_t)(tp)DK("create_logical_type", (uint32_t)t);
}

static tp handles_in(void* const* arr, duck_idx_t n) {
    tp a = tmalloc((uint32_t)n * 4 + 4);
    for (duck_idx_t i = 0; i < n; i++) put32(a + 4 * (uint32_t)i, H(arr[i]));
    return a;
}

static duck_data_chunk s_create_data_chunk(duck_logical_type* types, duck_idx_t n) {
    tp a = handles_in((void* const*)types, n);
    tp c = (tp)DK("create_data_chunk", a, LO(n), HI(n));
    tfree(a);
    if (c) {
        if (g_nw == g_capw) {
            g_capw = g_capw ? g_capw * 2 : 16;
            g_wchunks = xrealloc(g_wchunks, g_capw * sizeof *g_wchunks);
        }
        g_wchunks[g_nw++] = c;
    }
    return (duck_data_chunk)(uintptr_t)c;
}

static void s_data_chunk_reset(duck_data_chunk c) {
    drop_chunk(H(c));
    DK("data_chunk_reset", H(c));
}

static void s_data_chunk_set_size(duck_data_chunk c, duck_idx_t n) {
    flush_chunk(H(c));
    DK("data_chunk_set_size", H(c), LO(n), HI(n));
}

static duck_state s_appender_create_ext(duck_connection con, const char* cat, const char* sch, const char* tbl,
                                        duck_appender* out) {
    tp a = tstr(cat), b = tstr(sch), c = tstr(tbl);
    put32(scratch, 0);
    duck_state r = DK("appender_create_ext", H(con), a, b, c, scratch);
    *out = (duck_appender)(uintptr_t)get32(scratch);
    tfree(a); tfree(b); tfree(c);
    return r;
}

static duck_state s_appender_destroy(duck_appender* app) {
    borrowed_drop((uintptr_t)*app);
    put32(scratch, H(*app));
    duck_state r = DK("appender_destroy", scratch);
    *app = (duck_appender)(uintptr_t)get32(scratch);
    return r;
}

static duck_state s_append_data_chunk(duck_appender app, duck_data_chunk c) {
    flush_chunk(H(c));
    return DK("append_data_chunk", H(app), H(c));
}

static const char* s_appender_error(duck_appender app) {
    return borrowed((uintptr_t)app, (tp)DK("appender_error", H(app)));
}

static duck_state s_appender_flush(duck_appender app) { return DK("appender_flush", H(app)); }

#define LT_UNARY(fn, cname)                                                              \
    static duck_logical_type s_##fn(duck_logical_type t) {                               \
        return (duck_logical_type)(uintptr_t)(tp)DK(cname, H(t));                        \
    }

LT_UNARY(create_list_type, "create_list_type")
LT_UNARY(list_type_child_type, "list_type_child_type")
LT_UNARY(array_type_child_type, "array_type_child_type")
LT_UNARY(map_type_key_type, "map_type_key_type")
LT_UNARY(map_type_value_type, "map_type_value_type")

static duck_logical_type s_column_logical_type(duck_result* res, duck_idx_t col) {
    return (duck_logical_type)(uintptr_t)RCALL(res, (tp)DK("column_logical_type", TRES, LO(col), HI(col)));
}

static duck_type s_get_type_id(duck_logical_type t) { return DK("get_type_id", H(t)); }

static duck_vector s_list_vector_get_child(duck_vector vec) {
    tp c = (tp)DK("list_vector_get_child", H(vec));
    vreg(c, H(vec), VK_LIST);
    return (duck_vector)(uintptr_t)c;
}

static duck_idx_t s_list_vector_get_size(duck_vector vec) { return U64(DK("list_vector_get_size", H(vec))); }

/* Before a list's reserve or set_size, either of which may reallocate its child: what was written below goes back
 * first, and the mirrors under it are dropped. */
static tp list_resize_prep(duck_vector vec) {
    tp h = H(vec);
    for (size_t i = 0; i < g_nv; i++) if (descends(&g_v[i], h)) flush_one(&g_v[i]);
    for (size_t i = 0; i < g_nv; i++) if (descends(&g_v[i], h)) { mirror_free(&g_v[i]); g_v[i].cap = 0; }
    return h;
}

static duck_state s_list_vector_reserve(duck_vector vec, duck_idx_t n) {
    return DK("list_vector_reserve", list_resize_prep(vec), LO(n), HI(n));
}

static duck_state s_list_vector_set_size(duck_vector vec, duck_idx_t n) {
    return DK("list_vector_set_size", list_resize_prep(vec), LO(n), HI(n));
}

static char* s_logical_type_get_alias(duck_logical_type t) { return ostr_owned((tp)DK("logical_type_get_alias", H(t))); }
static uint8_t s_decimal_width(duck_logical_type t) { return (uint8_t)DK("decimal_width", H(t)); }
static uint8_t s_decimal_scale(duck_logical_type t) { return (uint8_t)DK("decimal_scale", H(t)); }
static duck_idx_t s_array_type_array_size(duck_logical_type t) { return U64(DK("array_type_array_size", H(t))); }
static uint32_t s_enum_dictionary_size(duck_logical_type t) { return (uint32_t)DK("enum_dictionary_size", H(t)); }

static char* s_enum_dictionary_value(duck_logical_type t, duck_idx_t i) {
    return ostr_owned((tp)DK("enum_dictionary_value", H(t), LO(i), HI(i)));
}

static duck_logical_type s_vector_get_column_type(duck_vector vec) {
    return (duck_logical_type)(uintptr_t)(tp)DK("vector_get_column_type", H(vec));
}

static duck_type s_enum_internal_type(duck_logical_type t) { return DK("enum_internal_type", H(t)); }
static duck_type s_decimal_internal_type(duck_logical_type t) { return DK("decimal_internal_type", H(t)); }

static duck_vector s_array_vector_get_child(duck_vector vec) {
    tp c = (tp)DK("array_vector_get_child", H(vec));
    vreg(c, H(vec), VK_ARRAY);
    return (duck_vector)(uintptr_t)c;
}

static duck_idx_t s_struct_type_child_count(duck_logical_type t) { return U64(DK("struct_type_child_count", H(t))); }

static char* s_struct_type_child_name(duck_logical_type t, duck_idx_t i) {
    return ostr_owned((tp)DK("struct_type_child_name", H(t), LO(i), HI(i)));
}

static duck_logical_type s_struct_type_child_type(duck_logical_type t, duck_idx_t i) {
    return (duck_logical_type)(uintptr_t)(tp)DK("struct_type_child_type", H(t), LO(i), HI(i));
}

static duck_vector s_struct_vector_get_child(duck_vector vec, duck_idx_t i) {
    tp c = (tp)DK("struct_vector_get_child", H(vec), LO(i), HI(i));
    vreg(c, H(vec), VK_STRUCT);
    return (duck_vector)(uintptr_t)c;
}

static duck_logical_type named_type(const char* cname, duck_logical_type* types, const char** names, duck_idx_t n) {
    tp a = handles_in((void* const*)types, n);
    tp b = tmalloc((uint32_t)n * 4 + 4);
    for (duck_idx_t i = 0; i < n; i++) put32(b + 4 * (uint32_t)i, tstr(names[i]));
    tp r = (tp)dk_inv(cname, (const uint32_t[]){a, b, LO(n), HI(n)}, 4);
    for (duck_idx_t i = 0; i < n; i++) tfree(get32(b + 4 * (uint32_t)i));
    tfree(a); tfree(b);
    return (duck_logical_type)(uintptr_t)r;
}

static duck_logical_type s_create_struct_type(duck_logical_type* t, const char** nm, duck_idx_t n) {
    return named_type("duckdb_create_struct_type", t, nm, n);
}

static duck_logical_type s_create_union_type(duck_logical_type* t, const char** nm, duck_idx_t n) {
    return named_type("duckdb_create_union_type", t, nm, n);
}

static duck_logical_type s_create_map_type(duck_logical_type k, duck_logical_type v) {
    return (duck_logical_type)(uintptr_t)(tp)DK("create_map_type", H(k), H(v));
}

static duck_idx_t s_union_type_member_count(duck_logical_type t) { return U64(DK("union_type_member_count", H(t))); }

static char* s_union_type_member_name(duck_logical_type t, duck_idx_t i) {
    return ostr_owned((tp)DK("union_type_member_name", H(t), LO(i), HI(i)));
}

static duck_logical_type s_union_type_member_type(duck_logical_type t, duck_idx_t i) {
    return (duck_logical_type)(uintptr_t)(tp)DK("union_type_member_type", H(t), LO(i), HI(i));
}

static char* s_value_varchar(duck_result* res, duck_idx_t col, duck_idx_t row) {
    return ostr_owned(RCALL(res, (tp)DK("value_varchar", TRES, LO(col), HI(col), LO(row), HI(row))));
}

static bool s_value_is_null(duck_result* res, duck_idx_t col, duck_idx_t row) {
    return RCALL(res, DK("value_is_null", TRES, LO(col), HI(col), LO(row), HI(row))) != 0;
}

int q_wasm_duck_bind(duck_api_t* api) {
    if (!dk_load(Q_WASM_DUCKDB_PIN)) return 0;
    if (!scratch) scratch = tmalloc(256);
    *api = (duck_api_t){
        .library_version = s_library_version, .open_ext = s_open_ext, .close = s_close,
        .create_config = s_create_config, .set_config = s_set_config, .destroy_config = s_destroy_config,
        .connect = s_connect, .disconnect = s_disconnect, .query = s_query, .destroy_result = s_destroy_result,
        .column_count = s_column_count, .column_name = s_column_name, .fetch_chunk = s_fetch_chunk,
        .destroy_data_chunk = s_destroy_data_chunk, .data_chunk_get_size = s_data_chunk_get_size,
        .data_chunk_get_vector = s_data_chunk_get_vector, .vector_get_data = s_vector_get_data,
        .vector_get_validity = s_vector_get_validity,
        .vector_ensure_validity_writable = s_vector_ensure_validity_writable,
        .validity_set_row_invalid = s_validity_set_row_invalid,
        .vector_assign_string_element_len = s_vector_assign_string_element_len, .vector_size = s_vector_size,
        .create_logical_type = s_create_logical_type, .destroy_logical_type = s_destroy_logical_type,
        .create_data_chunk = s_create_data_chunk, .data_chunk_reset = s_data_chunk_reset,
        .data_chunk_set_size = s_data_chunk_set_size, .appender_create_ext = s_appender_create_ext,
        .appender_destroy = s_appender_destroy, .append_data_chunk = s_append_data_chunk,
        .result_error = s_result_error, .appender_error = s_appender_error, .duck_free = free,
        .appender_flush = s_appender_flush, .create_list_type = s_create_list_type,
        .list_type_child_type = s_list_type_child_type, .column_logical_type = s_column_logical_type,
        .get_type_id = s_get_type_id, .list_vector_get_child = s_list_vector_get_child,
        .list_vector_get_size = s_list_vector_get_size, .list_vector_reserve = s_list_vector_reserve,
        .list_vector_set_size = s_list_vector_set_size, .logical_type_get_alias = s_logical_type_get_alias,
        .decimal_width = s_decimal_width, .decimal_scale = s_decimal_scale,
        .array_type_child_type = s_array_type_child_type, .array_type_array_size = s_array_type_array_size,
        .enum_dictionary_size = s_enum_dictionary_size, .enum_dictionary_value = s_enum_dictionary_value,
        .vector_get_column_type = s_vector_get_column_type, .enum_internal_type = s_enum_internal_type,
        .array_vector_get_child = s_array_vector_get_child, .result_return_type = s_result_return_type,
        .decimal_internal_type = s_decimal_internal_type, .struct_type_child_count = s_struct_type_child_count,
        .struct_type_child_name = s_struct_type_child_name, .struct_type_child_type = s_struct_type_child_type,
        .struct_vector_get_child = s_struct_vector_get_child, .create_struct_type = s_create_struct_type,
        .map_type_key_type = s_map_type_key_type, .map_type_value_type = s_map_type_value_type,
        .create_map_type = s_create_map_type, .union_type_member_count = s_union_type_member_count,
        .union_type_member_name = s_union_type_member_name, .union_type_member_type = s_union_type_member_type,
        .create_union_type = s_create_union_type, .row_count = s_row_count, .value_varchar = s_value_varchar,
        .value_is_null = s_value_is_null,
    };
    return 1;
}
