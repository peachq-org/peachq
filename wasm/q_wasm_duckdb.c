/* q_wasm_duckdb — the duck_api_t table q_duckdb.c drives (DuckDB's v2 C API), bound to DuckDB's own published wasm
 * build: a second module with its own memory, loaded by the host on the first DuckDB call (wasm/duck-loader.js).
 * Handles pass through as opaque 32-bit values; strings, structs and out-params are copied across around each call;
 * a vector's data and validity are mirrored into this heap on first read and written back before the vector is
 * sized, appended or read by DuckDB.  The replacement-scan callback reaches back through a trampoline in DuckDB's
 * function table.  No q semantics live here: the bridge above it is the desktop one. */
#include "qlang/io/q_duckdb_api.h"
#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t tp;   /* an address in DuckDB's memory */

/* 1 once the host has loaded DuckDB, else 0 with the reason in why.  A failure is remembered until the engine
 * restarts: each attempt fetches tens of megabytes. */
EM_JS(int, dk_load, (const char* version, char* why, int cap), {
    if (Module.peachqDuck) return 1;
    var msg;
    if (typeof Module.peachqDuckLoad !== "function") msg = "bridge: this host offers no DuckDB";
    else if (Module.peachqDuckFailed) msg = Module.peachqDuckFailed;
    else {
        try {
            Module.peachqDuck = Module.peachqDuckLoad(Module, UTF8ToString(version), _q_wasm_duck_rscan);
            Module.peachqDuck.fn = {};
            Module.peachqDuck.kd = {};
            return 1;
        } catch (e) {
            msg = Module.peachqDuckFailed = "duckdb-wasm: " + ((e && e.message) || e);
        }
    }
    var b = new TextEncoder().encode(msg);
    var n = Math.min(b.length, cap - 1);
    HEAPU8.set(b.subarray(0, n), why);
    HEAPU8[why + n] = 0;
    return 0;
});

/* One call into DuckDB's exports: each argument is a 64-bit slot read by its kind (i: i32, j: i64 as a BigInt,
 * f/d: float/double); errslot (DuckDB's memory) is zeroed first, so a stale error handle is never read back. */
EM_JS(int32_t, dk_call, (const char* name, const char* kinds, const uint64_t* a, int n, uint32_t errslot), {
    var D = Module.peachqDuck;
    var f = D.fn[name] || (D.fn[name] = D.X[UTF8ToString(name)]);
    var k = D.kd[kinds] || (D.kd[kinds] = UTF8ToString(kinds));
    var args = [];
    for (var i = 0; i < n; i++) {
        var w = (a >> 2) + 2 * i;
        if (k[i] === "i") args.push(HEAPU32[w] | 0);
        else if (k[i] === "j") args.push(BigInt(HEAPU32[w]) | (BigInt(HEAPU32[w + 1]) << 32n));
        else if (k[i] === "d") args.push(HEAPF64[(a >> 3) + i]);
        else args.push(HEAPF32[w]);
    }
    if (errslot) new Uint32Array(D.mem.buffer, errslot, 1)[0] = 0;
    try {
        return f.apply(null, args) | 0;
    } catch (e) {
        Module.printErr("duckdb-wasm: " + UTF8ToString(name) + ": " + ((e && e.message) || e));
        return -1;
    }
});

/* a copy that cannot be made is a shim bug, and the module stops rather than hand the bridge bytes never copied */
EM_JS(void, dk_get, (void* dst, tp src, uint32_t n), {
    try {
        HEAPU8.set(new Uint8Array(Module.peachqDuck.mem.buffer, src, n), dst);
    } catch (e) {
        abort("duckdb-wasm: read " + n + " bytes at " + src + ": " + ((e && e.message) || e));
    }
});

EM_JS(void, dk_put, (tp dst, const void* src, uint32_t n), {
    try {
        new Uint8Array(Module.peachqDuck.mem.buffer, dst, n).set(HEAPU8.subarray(src, src + n));
    } catch (e) {
        abort("duckdb-wasm: write " + n + " bytes at " + dst + ": " + ((e && e.message) || e));
    }
});

EM_JS(uint32_t, dk_memsize, (void), { return Module.peachqDuck.mem.buffer.byteLength >>> 0; });

EM_JS(int, dk_before_statement, (void), { return Module.peachqDuck.before() ? 1 : 0; });

EM_JS(uint32_t, dk_trampoline, (uint32_t fn), { return Module.peachqDuck.trampoline(fn) >>> 0; });

EM_JS(void, dk_repo, (char* buf, int cap), {
    var b = new TextEncoder().encode(Module.peachqDuck.repo);
    var n = Math.min(b.length, cap - 1);
    HEAPU8.set(b.subarray(0, n), buf);
    HEAPU8[buf + n] = 0;
});

#define NARG(...) ((int)(sizeof((uint64_t[]){__VA_ARGS__}) / sizeof(uint64_t)))
#define DK(nm, kinds, ...) dk_call("duckdb_v2_" nm, kinds, (const uint64_t[]){__VA_ARGS__}, NARG(__VA_ARGS__), SPE)
#define H(x) ((uint32_t)(uintptr_t)(x))
#define I(x) ((uint64_t)(uint32_t)(x))
#define J(x) ((uint64_t)(x))
#define EP(err) I((err) ? SPE : 0)
#define P(x) ((void*)(uintptr_t)(x))

/* out-params land in a scratch block of DuckDB's memory, one block per nesting level: the replacement-scan callback
 * runs inside statement_execute and makes calls of its own */
#define SCR_BYTES 256
#define SCR_DEPTH 8
static tp  g_scr;
static int g_depth;
#define SPB  (g_scr + (tp)g_depth * SCR_BYTES)
#define SPE  (SPB)          /* the error handle */
#define SPO  (SPB + 16)     /* an 8-byte out */
#define SPO2 (SPB + 32)
#define SPS  (SPB + 48)     /* a duck_str */
#define SPS2 (SPB + 64)
#define SPV  (SPB + 80)     /* a duck_view */
#define SPX  (SPB + 112)    /* a 16-byte value */

static char g_repo[1024];   /* the extension repository beside the bundle */

static uint64_t Dd(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static uint64_t Ff(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* Out of memory aborts the module (the Worker reports it and restart() recovers): a half-built mirror would hand the
 * bridge bytes that were never copied. */
static void* xmalloc(size_t n) {
    void* r = malloc(n ? n : 1);
    if (!r) abort();
    return r;
}

static void* xrealloc(void* p, size_t n) {
    void* r = realloc(p, n ? n : 1);
    if (!r) abort();
    return r;
}

static tp       tmalloc(uint32_t n) { return (tp)dk_call("malloc", "i", (const uint64_t[]){I(n)}, 1, 0); }
static void     tfree(tp p) { if (p) dk_call("free", "i", (const uint64_t[]){I(p)}, 1, 0); }
static uint32_t get32(tp p) { uint32_t v; dk_get(&v, p, 4); return v; }
static uint64_t get64(tp p) { uint64_t v; dk_get(&v, p, 8); return v; }
static void     put32(tp p, uint32_t v) { dk_put(p, &v, 4); }
static void     put64(tp p, uint64_t v) { dk_put(p, &v, 8); }

static tp tbytes(const void* s, uint32_t n) {
    tp p = tmalloc(n + 1);
    dk_put(p, s, n);
    dk_put(p + n, "", 1);
    return p;
}

static tp tstr(const char* s) { return s ? tbytes(s, (uint32_t)strlen(s)) : 0; }

/* a duck_str at dst (16 bytes, the wasm32 layout both modules share); answers the text's allocation */
static tp dstr_in(tp dst, const duck_str* s) {
    tp p = s && s->ptr ? tbytes(s->ptr, (uint32_t)s->len) : 0;
    put32(dst, p);
    put64(dst + 8, s ? s->len : 0);
    return p;
}

typedef struct { tp arr; tp* texts; duck_idx_t n; } strs_t;

static strs_t strs_in(const duck_str* a, duck_idx_t n) {
    strs_t r = { 0, NULL, n };
    if (!a || !n) return r;
    r.arr = tmalloc((uint32_t)n * 16);
    r.texts = xmalloc(n * sizeof *r.texts);
    for (duck_idx_t i = 0; i < n; i++) r.texts[i] = dstr_in(r.arr + 16 * (tp)i, &a[i]);
    return r;
}

static void strs_free(strs_t* r) {
    for (duck_idx_t i = 0; r->texts && i < r->n; i++) tfree(r->texts[i]);
    free(r->texts);
    tfree(r->arr);
}

static tp handles_in(void* const* a, duck_idx_t n) {
    if (!a || !n) return 0;
    tp p = tmalloc((uint32_t)n * 4);
    for (duck_idx_t i = 0; i < n; i++) put32(p + 4 * (tp)i, H(a[i]));
    return p;
}

/* the result shapes: a status with its error handle; an out handle / count / id read off the scratch block */
static duck_state fin(duck_state st, duck_err* err) {
    if (st != QDuckSuccess && err) *err = P(get32(SPE));
    return st;
}

static duck_state out_h(duck_state st, void* out, duck_err* err) {
    *(void**)out = st == QDuckSuccess ? P(get32(SPO)) : NULL;
    return fin(st, err);
}

static duck_state out_64(duck_state st, duck_idx_t* out, duck_err* err) {
    if (st == QDuckSuccess) *out = get64(SPO);
    return fin(st, err);
}

static duck_state out_32(duck_state st, int32_t* out, duck_err* err) {
    if (st == QDuckSuccess) *out = (int32_t)get32(SPO);
    return fin(st, err);
}

/* `destroy(&h)`: the handle goes over in the slot and comes back as DuckDB left it */
static duck_state destroy_h(const char* nm, void* h) {
    put32(SPO, H(*(void**)h));
    duck_state st = dk_call(nm, "i", (const uint64_t[]){I(SPO)}, 1, SPE);
    *(void**)h = P(get32(SPO));
    return st;
}

#define DESTROY(nm, h) destroy_h("duckdb_v2_" nm, h)

/* ---- borrowed strings: DuckDB's view copied into this heap, kept until the handle it belongs to is destroyed ---- */

typedef struct { uintptr_t key; tp src; uint64_t len; char* s; } bor_t;
static bor_t* g_bor;
static size_t g_nbor, g_capbor;

static duck_str borrow(uintptr_t key, tp slot) {
    duck_str r = { NULL, 0 };
    tp src = get32(slot);
    if (!src) return r;
    r.len = get64(slot + 8);
    for (size_t i = 0; i < g_nbor; i++)
        if (g_bor[i].key == key && g_bor[i].src == src && g_bor[i].len == r.len) { r.ptr = g_bor[i].s; return r; }
    if (g_nbor == g_capbor) {
        g_capbor = g_capbor ? 2 * g_capbor : 64;
        g_bor = xrealloc(g_bor, g_capbor * sizeof *g_bor);
    }
    char* s = xmalloc((size_t)r.len + 1);
    dk_get(s, src, (uint32_t)r.len);
    s[r.len] = 0;
    g_bor[g_nbor++] = (bor_t){ key, src, r.len, s };
    r.ptr = s;
    return r;
}

static void borrow_drop(uintptr_t key) {
    size_t j = 0;
    for (size_t i = 0; i < g_nbor; i++) {
        if (g_bor[i].key == key) free(g_bor[i].s);
        else g_bor[j++] = g_bor[i];
    }
    g_nbor = j;
}

/* ---- vectors: a mirror per handle, rooted at its chunk ---- */

enum { VK_TOP, VK_LIST, VK_STRUCT, VK_ARRAY };

typedef struct { char* ours; tp theirs; uint32_t n; } ablk_t;   /* one arena_allocate, ours until flushed */

typedef struct {
    tp       handle, root, parent, arena;
    int      kind;
    int32_t  tid;              /* the logical type id, -1 until asked */
    uint32_t width, cap, asize;
    bool     strtype, have_view, have_data, have_val, stale;   /* stale: resized under it, the mirror extends */
    tp       tdata, tval;
    void*    odata;
    uint64_t* oval;
    char*    sarena;           /* the read side's copy of the long strings */
    ablk_t*  blk;
    size_t   nblk, capblk;
    duck_view view;
} vinfo_t;

static vinfo_t* g_v;
static size_t g_nv, g_capv;
static tp* g_wchunks;
static size_t g_nw, g_capw;

static bool is_write_chunk(tp c) {
    for (size_t i = 0; i < g_nw; i++) if (g_wchunks[i] == c) return true;
    return false;
}

static vinfo_t* vfind(tp h) {
    for (size_t i = 0; i < g_nv; i++) if (g_v[i].handle == h) return &g_v[i];
    return NULL;
}

static void mirror_free(vinfo_t* v) {
    for (size_t i = 0; i < v->nblk; i++) free(v->blk[i].ours);
    free(v->blk); free(v->odata); free(v->oval); free(v->sarena);
    v->blk = NULL; v->odata = NULL; v->oval = NULL; v->sarena = NULL;
    v->nblk = v->capblk = 0;
    v->have_view = v->have_data = v->have_val = v->stale = false;
    v->cap = 0;
}

static vinfo_t* vreg(tp h, tp parent, int kind) {
    if (!h) return NULL;
    vinfo_t* p = kind == VK_TOP ? NULL : vfind(parent);
    tp root = kind == VK_TOP ? parent : (p ? p->root : 0);
    vinfo_t* v = vfind(h);
    if (v) {
        if (v->root == root && v->parent == (kind == VK_TOP ? 0 : parent)) return v;
        mirror_free(v);
    } else {
        if (g_nv == g_capv) {
            g_capv = g_capv ? 2 * g_capv : 64;
            g_v = xrealloc(g_v, g_capv * sizeof *g_v);
        }
        v = &g_v[g_nv++];
    }
    memset(v, 0, sizeof *v);
    v->handle = h;
    v->root = root;
    v->parent = kind == VK_TOP ? 0 : parent;
    v->kind = kind;
    v->tid = -1;
    return v;
}

static vinfo_t* vget(duck_vector vec) {
    vinfo_t* v = vfind(H(vec));
    return v ? v : vreg(H(vec), 0, VK_TOP);
}

static bool descends(const vinfo_t* v, tp anc) {
    for (tp p = v->parent; p; ) {
        if (p == anc) return true;
        vinfo_t* pv = vfind(p);
        p = pv ? pv->parent : 0;
    }
    return false;
}

/* parameter i of a logical type as the integer it carries, -1 where it is none */
static int64_t type_param_int(tp lt, duck_idx_t i) {
    int64_t r = -1;
    if (DK("logical_type_get_param", "ijiii", I(lt), J(i), I(SPS), I(SPO), 0) != QDuckSuccess) return -1;
    tp val = get32(SPO);
    if (DK("value_get_utinyint", "iii", I(val), I(SPO2), 0) == QDuckSuccess) r = get32(SPO2) & 0xff;
    else if (DK("value_get_bigint", "iii", I(val), I(SPO2), 0) == QDuckSuccess) r = (int64_t)get64(SPO2);
    put32(SPO, val);
    DK("value_destroy", "i", I(SPO));
    return r;
}

/* the vector's type id and the bytes one cell of its data holds (0 where the data lives in the children); the
 * storage tiers of DECIMAL and ENUM are the header's contract */
static void vtype(vinfo_t* v) {
    if (v->tid >= 0) return;
    v->tid = 0;
    if (DK("vector_get_logical_type", "iii", I(v->handle), I(SPO), 0) != QDuckSuccess) return;
    tp lt = get32(SPO);
    if (DK("logical_type_get_id", "iii", I(lt), I(SPO), 0) == QDuckSuccess) v->tid = (int32_t)get32(SPO);
    switch (v->tid) {
        case QDUCK_TYPE_BOOLEAN: case QDUCK_TYPE_TINYINT: case QDUCK_TYPE_UTINYINT: v->width = 1; break;
        case QDUCK_TYPE_SMALLINT: case QDUCK_TYPE_USMALLINT: v->width = 2; break;
        case QDUCK_TYPE_INTEGER: case QDUCK_TYPE_UINTEGER: case QDUCK_TYPE_FLOAT: case QDUCK_TYPE_DATE: v->width = 4; break;
        case QDUCK_TYPE_BIGINT: case QDUCK_TYPE_UBIGINT: case QDUCK_TYPE_DOUBLE: case QDUCK_TYPE_TIMESTAMP:
        case QDUCK_TYPE_TIMESTAMP_S: case QDUCK_TYPE_TIMESTAMP_MS: case QDUCK_TYPE_TIMESTAMP_NS: case QDUCK_TYPE_TIME:
        case QDUCK_TYPE_TIME_NS: case QDUCK_TYPE_TIME_TZ: case QDUCK_TYPE_TIMESTAMP_TZ: case QDUCK_TYPE_TIMESTAMP_TZ_NS:
            v->width = 8; break;
        case QDUCK_TYPE_HUGEINT: case QDUCK_TYPE_UHUGEINT: case QDUCK_TYPE_UUID: case QDUCK_TYPE_INTERVAL:
        case QDUCK_TYPE_LIST: case QDUCK_TYPE_MAP: v->width = 16; break;
        case QDUCK_TYPE_VARCHAR: case QDUCK_TYPE_BLOB: case QDUCK_TYPE_BIT: case QDUCK_TYPE_BIGNUM:
        case QDUCK_TYPE_GEOMETRY: case QDUCK_TYPE_VARIANT:
            v->width = 16; v->strtype = true; break;
        case QDUCK_TYPE_DECIMAL: {
            int64_t w = type_param_int(lt, 0);
            v->width = w <= 4 ? 2 : w <= 9 ? 4 : w <= 18 ? 8 : 16;
            break;
        }
        case QDUCK_TYPE_ENUM: {
            duck_idx_t n = 0;
            if (DK("logical_type_get_param_count", "iii", I(lt), I(SPO), 0) == QDuckSuccess) n = get64(SPO);
            v->width = n <= 255 ? 1 : n <= 65535 ? 2 : 4;
            break;
        }
        case QDUCK_TYPE_ARRAY: v->asize = (uint32_t)type_param_int(lt, 1); break;
        default: break;
    }
    put32(SPO, lt);
    DK("logical_type_destroy", "i", I(SPO));
}

static int child_kind(vinfo_t* parent) {
    vtype(parent);
    switch (parent->tid) {
        case QDUCK_TYPE_LIST: case QDUCK_TYPE_MAP: return VK_LIST;
        case QDUCK_TYPE_ARRAY: return VK_ARRAY;
        default: return VK_STRUCT;
    }
}

/* The rows a WRITE vector's buffers hold: its own size once set, else a fresh chunk's capacity; a struct child its
 * parent's, an array child its parent's times the array size. */
static uint32_t vcap(vinfo_t* v) {
    if (v->cap && !v->stale) return v->cap;
    duck_idx_t n = 0;
    if (DK("vector_get_size", "iii", I(v->handle), I(SPO), 0) == QDuckSuccess) n = get64(SPO);
    vinfo_t* p = v->kind == VK_TOP ? NULL : vfind(v->parent);
    if (v->kind == VK_STRUCT && p) n = vcap(p);
    else if (v->kind == VK_ARRAY && p) { vtype(p); n = (duck_idx_t)vcap(p) * p->asize; }
    v->cap = n ? (uint32_t)n : 2048;
    return v->cap;
}

static uint32_t vbytes(uint32_t rows) { return ((rows + 63) / 64) * 8; }

static int blk_cmp(const void* a, const void* b) {
    const char* x = ((const ablk_t*)a)->ours;
    const char* y = ((const ablk_t*)b)->ours;
    return x < y ? -1 : x > y;
}

/* A write vector's mirror back into DuckDB: its arena blocks first, then the cells, a long string's pointer
 * translated to the block it was written into.  The mirror holds only what the bridge wrote (it starts zeroed and is
 * never read back from DuckDB), so a pointer in it is ours or a NULL row's. */
static void flush_one(vinfo_t* v) {
    if (v->have_data && v->odata && v->width) {
        size_t nb = (size_t)v->cap * v->width;
        void* out = v->odata;
        if (v->strtype && v->nblk) {
            for (size_t i = 0; i < v->nblk; i++) dk_put(v->blk[i].theirs, v->blk[i].ours, v->blk[i].n);
            qsort(v->blk, v->nblk, sizeof *v->blk, blk_cmp);
            out = xmalloc(nb);
            memcpy(out, v->odata, nb);
            duck_string_t* s = out;
            for (uint32_t r = 0; r < v->cap; r++) {
                if (s[r].value.inlined.length <= QDUCK_STRING_INLINE_MAX) continue;
                const char* p = s[r].value.pointer.ptr;
                size_t lo = 0, hi = v->nblk;
                while (lo < hi) {
                    size_t mid = (lo + hi) / 2;
                    if (v->blk[mid].ours + v->blk[mid].n <= p) lo = mid + 1;
                    else hi = mid;
                }
                if (lo < v->nblk && v->blk[lo].ours <= p)
                    s[r].value.pointer.ptr = (char*)(uintptr_t)(v->blk[lo].theirs + (tp)(p - v->blk[lo].ours));
            }
        }
        dk_put(v->tdata, out, (uint32_t)nb);
        if (out != v->odata) free(out);
    }
    if (v->have_val && v->oval) dk_put(v->tval, v->oval, vbytes(v->cap));
}

/* before a vector is resized: what was written under it goes back, and DuckDB may move or grow those buffers, so
 * their mirrors are stale until the next write asks for them */
static void flush_tree(tp h) {
    vinfo_t* v = vfind(h);
    if (!v || !is_write_chunk(v->root)) return;
    for (size_t i = 0; i < g_nv; i++)
        if (g_v[i].handle == h || descends(&g_v[i], h)) { flush_one(&g_v[i]); g_v[i].stale = true; }
}

static void flush_chunk(tp root) {
    if (!is_write_chunk(root)) return;
    for (size_t i = 0; i < g_nv; i++) if (g_v[i].root == root) flush_one(&g_v[i]);
}

/* a write mirror of rows * width bytes, zeroed (or set to fill) beyond what it already held */
static void* mirror_extend(void* old, uint32_t oldrows, uint32_t rows, uint32_t width, int fill) {
    char* p = xrealloc(old, (size_t)rows * width);
    if (rows > oldrows) memset(p + (size_t)oldrows * width, fill, (size_t)(rows - oldrows) * width);
    return p;
}

static void drop_chunk(tp root) {
    size_t j = 0;
    for (size_t i = 0; i < g_nv; i++) {
        if (g_v[i].root == root) mirror_free(&g_v[i]);
        else g_v[j++] = g_v[i];
    }
    g_nv = j;
    for (size_t i = 0; i < g_nw; i++) if (g_wchunks[i] == root) g_wchunks[i] = g_wchunks[--g_nw];
}

/* ---- the table ---- */

static duck_state s_environment_create(duck_env* out, duck_err* err) {
    return out_h(DK("environment_create", "ii", I(SPO), EP(err)), out, err);
}

static duck_state s_environment_destroy(duck_env* env) { return DESTROY("environment_destroy", env); }

/* The one option the browser adds, FIRST so a caller's own wins: extensions come from the pinned directory beside
 * the bundle, where DuckDB's loader fetches them, never from DuckDB's servers. */
static duck_state s_instance_create_with_options(duck_env env, const duck_str* names, const duck_str* values,
                                                 duck_idx_t n, duck_instance* out, duck_err* err) {
    duck_str* ks = xmalloc((n + 1) * sizeof *ks);
    duck_str* vs = xmalloc((n + 1) * sizeof *vs);
    ks[0] = (duck_str){ "custom_extension_repository", strlen("custom_extension_repository") };
    vs[0] = (duck_str){ g_repo, strlen(g_repo) };
    for (duck_idx_t i = 0; i < n; i++) { ks[i + 1] = names[i]; vs[i + 1] = values[i]; }
    strs_t a = strs_in(ks, n + 1), b = strs_in(vs, n + 1);
    duck_state st = out_h(DK("instance_create_with_options", "iiijii", I(env), I(a.arr), I(b.arr), J(n + 1), I(SPO), EP(err)),
                          out, err);
    strs_free(&a); strs_free(&b);
    free(ks); free(vs);
    return st;
}

static duck_state s_instance_destroy(duck_instance* inst) { return DESTROY("instance_destroy", inst); }

static duck_state s_instance_attach(duck_instance inst, const duck_str* name, const duck_str* path, void* options,
                                    bool make_default, duck_err* err) {
    tp a = name ? dstr_in(SPS, name) : 0, b = path ? dstr_in(SPS2, path) : 0;
    duck_state st = fin(DK("instance_attach", "iiiiii", I(inst), I(name ? SPS : 0), I(path ? SPS2 : 0), I(options),
                           I(make_default), EP(err)), err);
    tfree(a); tfree(b);
    return st;
}

static duck_state s_connection_create(duck_instance inst, duck_connection* out, duck_err* err) {
    return out_h(DK("connection_create", "iii", I(inst), I(SPO), EP(err)), out, err);
}

static duck_state s_connection_destroy(duck_connection* con) { return DESTROY("connection_destroy", con); }

static duck_state s_connection_get_factory(duck_connection con, duck_factory* out, duck_err* err) {
    return out_h(DK("connection_get_factory", "iii", I(con), I(SPO), EP(err)), out, err);
}

static duck_state s_error_info_get_text(duck_err e, duck_str* out) {
    duck_state st = DK("error_info_get_text", "ii", I(e), I(SPS));
    *out = st == QDuckSuccess ? borrow((uintptr_t)e, SPS) : (duck_str){ NULL, 0 };
    return st;
}

static duck_state s_error_info_destroy(duck_err* e) {
    borrow_drop((uintptr_t)*e);
    return DESTROY("error_info_destroy", e);
}

/* a statement runs where q stands: a working directory DuckDB's filesystem cannot follow refuses it, loudly */
static duck_state s_parse_sql(duck_connection con, const char* sql, duck_stmt_iter* out, duck_err* err) {
    if (!dk_before_statement()) { *out = NULL; return -1; }
    tp q = tstr(sql);
    duck_state st = out_h(DK("parse_sql", "iiii", I(con), I(q), I(SPO), EP(err)), out, err);
    tfree(q);
    return st;
}

static duck_state s_statement_iterator_next(duck_stmt_iter it, duck_stmt* out, duck_err* err) {
    return out_h(DK("statement_iterator_next", "iii", I(it), I(SPO), EP(err)), out, err);
}

static duck_state s_statement_iterator_destroy(duck_stmt_iter* it) { return DESTROY("statement_iterator_destroy", it); }

static duck_state s_sql_statement_destroy(duck_stmt* st) {
    borrow_drop((uintptr_t)*st);
    return DESTROY("sql_statement_destroy", st);
}

static duck_state s_sql_statement_get_parameter_count(duck_stmt st, duck_idx_t* out, duck_err* err) {
    return out_64(DK("sql_statement_get_parameter_count", "iii", I(st), I(SPO), EP(err)), out, err);
}

static duck_state s_sql_statement_get_parameter_name(duck_stmt st, duck_idx_t i, duck_str* out, duck_err* err) {
    duck_state r = DK("sql_statement_get_parameter_name", "ijii", I(st), J(i), I(SPS), EP(err));
    *out = r == QDuckSuccess ? borrow((uintptr_t)st, SPS) : (duck_str){ NULL, 0 };
    return fin(r, err);
}

static duck_state s_sql_statement_get_type(duck_stmt st, int32_t* out, duck_err* err) {
    return out_32(DK("sql_statement_get_type", "iii", I(st), I(SPO), EP(err)), out, err);
}

static duck_state s_statement_bind(duck_connection con, duck_stmt st, duck_schema* out, duck_schema* params, duck_err* err) {
    duck_state r = DK("statement_bind", "iiiii", I(con), I(st), I(SPO), I(SPO2), EP(err));
    *params = r == QDuckSuccess ? P(get32(SPO2)) : NULL;
    return out_h(r, out, err);
}

static duck_state s_statement_execute(duck_connection con, duck_stmt st, const duck_str* names, const duck_value* values,
                                      duck_idx_t n, duck_cursor* out, duck_err* err) {
    strs_t a = strs_in(names, n);
    tp b = handles_in((void* const*)values, n);
    duck_state r = out_h(DK("statement_execute", "iiiijii", I(con), I(st), I(a.arr), I(b), J(n), I(SPO), EP(err)), out, err);
    strs_free(&a); tfree(b);
    return r;
}

static duck_state s_result_destroy(duck_cursor* res) { return DESTROY("result_destroy", res); }

static duck_state s_result_fetch_chunk(duck_cursor res, duck_data_chunk* out, duck_err* err) {
    return out_h(DK("result_fetch_chunk", "iii", I(res), I(SPO), EP(err)), out, err);
}

static duck_state s_result_get_result_type(duck_cursor res, int32_t* out, duck_err* err) {
    return out_32(DK("result_get_result_type", "iii", I(res), I(SPO), EP(err)), out, err);
}

static duck_state s_result_get_schema(duck_cursor res, duck_schema* out, duck_err* err) {
    return out_h(DK("result_get_schema", "iii", I(res), I(SPO), EP(err)), out, err);
}

static duck_state s_schema_get_count(duck_schema s, duck_idx_t* out, duck_err* err) {
    return out_64(DK("schema_get_count", "iii", I(s), I(SPO), EP(err)), out, err);
}

static duck_state s_schema_get_field(duck_schema s, duck_idx_t i, duck_str* name, duck_logical_type* type, duck_err* err) {
    duck_state r = DK("schema_get_field", "ijiii", I(s), J(i), I(SPS), I(SPO), EP(err));
    *name = r == QDuckSuccess ? borrow((uintptr_t)s, SPS) : (duck_str){ NULL, 0 };
    return out_h(r, type, err);
}

static duck_state s_schema_destroy(duck_schema* s) {
    borrow_drop((uintptr_t)*s);
    return DESTROY("schema_destroy", s);
}

static duck_state s_data_chunk_create(duck_factory f, const duck_logical_type* types, duck_idx_t n, duck_data_chunk* out,
                                      duck_err* err) {
    tp a = handles_in((void* const*)types, n);
    duck_state r = out_h(DK("data_chunk_create", "iijii", I(f), I(a), J(n), I(SPO), EP(err)), out, err);
    tfree(a);
    if (r == QDuckSuccess && *out) {
        if (g_nw == g_capw) {
            g_capw = g_capw ? 2 * g_capw : 16;
            g_wchunks = xrealloc(g_wchunks, g_capw * sizeof *g_wchunks);
        }
        g_wchunks[g_nw++] = H(*out);
    }
    return r;
}

static duck_state s_data_chunk_destroy(duck_data_chunk* chunk) {
    drop_chunk(H(*chunk));
    return DESTROY("data_chunk_destroy", chunk);
}

static duck_state s_data_chunk_get_size(duck_data_chunk chunk, duck_idx_t* out, duck_err* err) {
    return out_64(DK("data_chunk_get_size", "iii", I(chunk), I(SPO), EP(err)), out, err);
}

static duck_state s_data_chunk_get_vector(duck_data_chunk chunk, duck_idx_t col, duck_vector* out, duck_err* err) {
    duck_state r = out_h(DK("data_chunk_get_vector", "ijii", I(chunk), J(col), I(SPO), EP(err)), out, err);
    if (r == QDuckSuccess) vreg(H(*out), H(chunk), VK_TOP);
    return r;
}

static duck_state s_vector_get_logical_type(duck_vector vec, duck_logical_type* out, duck_err* err) {
    return out_h(DK("vector_get_logical_type", "iii", I(vec), I(SPO), EP(err)), out, err);
}

static duck_state s_vector_flatten(duck_vector vec, duck_err* err) {
    vinfo_t* v = vget(vec);
    if (v->have_view) return QDuckSuccess;
    mirror_free(v);
    return fin(DK("vector_flatten", "ii", I(vec), EP(err)), err);
}

/* The view's data and validity, mirrored once per vector: count rows of the type's width, a string vector's long
 * cells copied into one arena and re-pointed there, never read past DuckDB's heap. */
static duck_state s_vector_get_view(duck_vector vec, duck_view* out, duck_err* err) {
    vinfo_t* v = vget(vec);
    if (v->have_view) { *out = v->view; return QDuckSuccess; }
    if (is_write_chunk(v->root)) { flush_chunk(v->root); mirror_free(v); }
    duck_state r = DK("vector_get_view", "iii", I(vec), I(SPV), EP(err));
    if (r != QDuckSuccess) return fin(r, err);
    tp data = get32(SPV), val = get32(SPV + 4), sel = get32(SPV + 8);
    uint32_t count = (uint32_t)get64(SPV + 16);
    if (sel) return -1;
    vtype(v);
    v->cap = count;
    memset(&v->view, 0, sizeof v->view);
    v->view.count = count;
    if (val) {
        v->oval = xmalloc(vbytes(count));
        dk_get(v->oval, val, vbytes(count));
        v->view.validity = v->oval;
    }
    if (data && v->width) {
        size_t nb = (size_t)count * v->width;
        v->odata = xmalloc(nb);
        dk_get(v->odata, data, (uint32_t)nb);
        v->view.data = v->odata;
    }
    if (v->odata && v->strtype) {
        duck_string_t* s = v->odata;
        uint32_t msz = dk_memsize();
        size_t total = 0;
        for (uint32_t i = 0; i < count; i++)
            if (q_duckdb_validity_ok(v->oval, i) && s[i].value.inlined.length > QDUCK_STRING_INLINE_MAX)
                total += s[i].value.inlined.length;
        v->sarena = xmalloc(total);
        size_t at = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t len = s[i].value.inlined.length;
            if (!q_duckdb_validity_ok(v->oval, i) || len <= QDUCK_STRING_INLINE_MAX) continue;
            tp src = H(s[i].value.pointer.ptr);
            if ((uint64_t)src + len > msz) { s[i].value.pointer.ptr = NULL; continue; }
            dk_get(v->sarena + at, src, len);
            s[i].value.pointer.ptr = v->sarena + at;
            at += len;
        }
    }
    v->have_view = true;
    *out = v->view;
    return QDuckSuccess;
}

/* The write mirrors: data zeroed, validity all valid, as a fresh vector's are; after a resize every mirror the
 * vector has is re-addressed (DuckDB may have moved either buffer) and extended, never re-read, so DuckDB sees
 * exactly the cells the bridge wrote. */
static duck_state mirrors_resize(vinfo_t* v, bool data, bool val, duck_err* err) {
    bool want_data = data || v->have_data, want_val = val || v->have_val;
    duck_state r;
    if (want_data) {
        if ((r = DK("vector_get_data_mutable", "iii", I(v->handle), I(SPO), EP(err))) != QDuckSuccess) return fin(r, err);
        v->tdata = get32(SPO);
    }
    if (want_val) {
        if ((r = DK("vector_flat_get_validity_mutable", "iii", I(v->handle), I(SPO), EP(err))) != QDuckSuccess) return fin(r, err);
        v->tval = get32(SPO);
    }
    vtype(v);
    uint32_t dold = v->have_data ? v->cap : 0, vold = v->have_val ? v->cap : 0, cap = vcap(v);
    if (want_data && v->width) v->odata = mirror_extend(v->odata, dold, cap, v->width, 0);
    if (want_val) v->oval = mirror_extend(v->oval, vbytes(vold) / 8, vbytes(cap) / 8, 8, 0xff);
    v->have_data = want_data;
    v->have_val = want_val;
    v->stale = false;
    return QDuckSuccess;
}

static duck_state s_vector_get_data_mutable(duck_vector vec, void** out, duck_err* err) {
    vinfo_t* v = vget(vec);
    if (!v->have_data || v->stale) {
        duck_state r = mirrors_resize(v, true, false, err);
        if (r != QDuckSuccess) return r;
    }
    *out = v->odata;
    return QDuckSuccess;
}

static duck_state s_vector_flat_get_validity_mutable(duck_vector vec, uint64_t** out, duck_err* err) {
    vinfo_t* v = vget(vec);
    if (!v->have_val || v->stale) {
        duck_state r = mirrors_resize(v, false, true, err);
        if (r != QDuckSuccess) return r;
    }
    *out = v->oval;
    return QDuckSuccess;
}

static duck_state s_vector_get_size(duck_vector vec, duck_idx_t* out, duck_err* err) {
    return out_64(DK("vector_get_size", "iii", I(vec), I(SPO), EP(err)), out, err);
}

static duck_state s_vector_set_size(duck_vector vec, duck_idx_t size, duck_err* err) {
    flush_tree(H(vec));
    return fin(DK("vector_set_size", "iji", I(vec), J(size), EP(err)), err);
}

static duck_state s_vector_get_arena(duck_vector vec, duck_arena* out, duck_err* err) {
    vinfo_t* v = vget(vec);
    duck_state r = out_h(DK("vector_get_arena", "iii", I(vec), I(SPO), EP(err)), out, err);
    if (r == QDuckSuccess) v->arena = H(*out);
    return r;
}

/* the bytes are written here, into a block of ours, and move into DuckDB's allocation when the vector is flushed */
static duck_state s_arena_allocate(duck_arena arena, duck_idx_t n, uint8_t** out, duck_err* err) {
    vinfo_t* v = NULL;
    for (size_t i = 0; i < g_nv && !v; i++) if (g_v[i].arena == H(arena)) v = &g_v[i];
    duck_state r = DK("arena_allocate", "ijii", I(arena), J(n), I(SPO), EP(err));
    if (r != QDuckSuccess || !v) { *out = NULL; return fin(r == QDuckSuccess ? -1 : r, err); }
    if (v->nblk == v->capblk) {
        v->capblk = v->capblk ? 2 * v->capblk : 16;
        v->blk = xrealloc(v->blk, v->capblk * sizeof *v->blk);
    }
    v->blk[v->nblk] = (ablk_t){ xmalloc((size_t)n), get32(SPO), (uint32_t)n };
    *out = (uint8_t*)v->blk[v->nblk++].ours;
    return QDuckSuccess;
}

static duck_state s_vector_get_child(duck_vector vec, duck_idx_t index, duck_vector* out, duck_err* err) {
    duck_state r = out_h(DK("vector_get_child", "ijii", I(vec), J(index), I(SPO), EP(err)), out, err);
    if (r == QDuckSuccess) vreg(H(*out), H(vec), child_kind(vget(vec)));
    return r;
}

static duck_state s_vector_get_value(duck_vector vec, duck_idx_t row, duck_value* out, duck_err* err) {
    flush_chunk(vget(vec)->root);
    return out_h(DK("vector_get_value", "ijii", I(vec), J(row), I(SPO), EP(err)), out, err);
}

static duck_state s_logical_type_copy(duck_logical_type t, duck_logical_type* out, duck_err* err) {
    return out_h(DK("logical_type_copy", "iii", I(t), I(SPO), EP(err)), out, err);
}

static duck_state s_logical_type_destroy(duck_logical_type* t) {
    borrow_drop((uintptr_t)*t);
    return DESTROY("logical_type_destroy", t);
}

static duck_state s_logical_type_get_id(duck_logical_type t, int32_t* out, duck_err* err) {
    return out_32(DK("logical_type_get_id", "iii", I(t), I(SPO), EP(err)), out, err);
}

static duck_state s_logical_type_get_name(duck_logical_type t, duck_str* out, duck_err* err) {
    duck_state r = DK("logical_type_get_name", "iii", I(t), I(SPS), EP(err));
    *out = r == QDuckSuccess ? borrow((uintptr_t)t, SPS) : (duck_str){ NULL, 0 };
    return fin(r, err);
}

static duck_state s_logical_type_get_param_count(duck_logical_type t, duck_idx_t* out, duck_err* err) {
    return out_64(DK("logical_type_get_param_count", "iii", I(t), I(SPO), EP(err)), out, err);
}

static duck_state s_logical_type_get_param(duck_logical_type t, duck_idx_t i, duck_str* name, duck_value* out, duck_err* err) {
    duck_state r = DK("logical_type_get_param", "ijiii", I(t), J(i), I(SPS), I(SPO), EP(err));
    *name = r == QDuckSuccess ? borrow((uintptr_t)t, SPS) : (duck_str){ NULL, 0 };
    return out_h(r, out, err);
}

static duck_state s_factory_create_type_from_id(duck_factory f, int32_t id, const duck_str* names, const duck_value* values,
                                                duck_idx_t n, duck_logical_type* out, duck_err* err) {
    strs_t a = strs_in(names, n);
    tp b = handles_in((void* const*)values, n);
    duck_state r = out_h(DK("factory_create_type_from_id", "iiiijii", I(f), I(id), I(a.arr), I(b), J(n), I(SPO), EP(err)),
                         out, err);
    strs_free(&a); tfree(b);
    return r;
}

static duck_state s_factory_create_type_from_text(duck_factory f, const duck_str* text, duck_logical_type* out, duck_err* err) {
    tp a = dstr_in(SPS, text);
    duck_state r = out_h(DK("factory_create_type_from_text", "iiii", I(f), I(SPS), I(SPO), EP(err)), out, err);
    tfree(a);
    return r;
}

static duck_state s_value_destroy(duck_value* v) {
    borrow_drop((uintptr_t)*v);
    return DESTROY("value_destroy", v);
}

static duck_state s_value_is_null(duck_value v, bool* out, duck_err* err) {
    duck_state r = DK("value_is_null", "iii", I(v), I(SPO), EP(err));
    if (r == QDuckSuccess) *out = (get32(SPO) & 0xff) != 0;
    return fin(r, err);
}

static duck_state s_value_to_string(duck_value v, char* out, duck_idx_t cap, duck_idx_t* len, duck_err* err) {
    tp buf = cap ? tmalloc((uint32_t)cap) : 0;
    duck_state r = DK("value_to_string", "iijii", I(v), I(buf), J(cap), I(SPO), EP(err));
    if (r == QDuckSuccess) {
        *len = get64(SPO);
        if (cap) dk_get(out, buf, (uint32_t)cap);
    }
    r = fin(r, err);
    tfree(buf);
    return r;
}

static duck_state s_value_get_type(duck_value v, duck_logical_type* out, duck_err* err) {
    return out_h(DK("value_get_type", "iii", I(v), I(SPO), EP(err)), out, err);
}

static duck_state s_value_get_varchar(duck_value v, duck_str* out, duck_err* err) {
    duck_state r = DK("value_get_varchar", "iii", I(v), I(SPS), EP(err));
    *out = r == QDuckSuccess ? borrow((uintptr_t)v, SPS) : (duck_str){ NULL, 0 };
    return fin(r, err);
}

static duck_state s_value_get_utinyint(duck_value v, uint8_t* out, duck_err* err) {
    duck_state r = DK("value_get_utinyint", "iii", I(v), I(SPO), EP(err));
    if (r == QDuckSuccess) *out = (uint8_t)get32(SPO);
    return fin(r, err);
}

static duck_state s_value_get_bigint(duck_value v, int64_t* out, duck_err* err) {
    duck_state r = DK("value_get_bigint", "iii", I(v), I(SPO), EP(err));
    if (r == QDuckSuccess) *out = (int64_t)get64(SPO);
    return fin(r, err);
}

#define VALUE_OF(fn, cname, kinds, ctype, arg)                                                       \
    static duck_state s_##fn(duck_factory f, ctype x, duck_value* out, duck_err* err) {              \
        return out_h(DK(cname, kinds, I(f), arg, I(SPO), EP(err)), out, err);                        \
    }

VALUE_OF(value_create_type, "value_create_type", "iiii", duck_logical_type, I(x))
VALUE_OF(value_create_null, "value_create_null", "iiii", duck_logical_type, I(x))
VALUE_OF(value_create_bool, "value_create_bool", "iiii", bool, I(x))
VALUE_OF(value_create_utinyint, "value_create_utinyint", "iiii", uint8_t, I(x))
VALUE_OF(value_create_smallint, "value_create_smallint", "iiii", int16_t, I(x))
VALUE_OF(value_create_int, "value_create_int", "iiii", int32_t, I(x))
VALUE_OF(value_create_bigint, "value_create_bigint", "ijii", int64_t, J(x))
VALUE_OF(value_create_float, "value_create_float", "ifii", float, Ff(x))
VALUE_OF(value_create_double, "value_create_double", "idii", double, Dd(x))
VALUE_OF(value_create_date, "value_create_date", "iiii", int32_t, I(x))
VALUE_OF(value_create_timestamp, "value_create_timestamp", "ijii", int64_t, J(x))
VALUE_OF(value_create_timestamp_ns, "value_create_timestamp_ns", "ijii", int64_t, J(x))

static duck_state s_value_create_varchar(duck_factory f, const duck_str* v, duck_value* out, duck_err* err) {
    tp a = dstr_in(SPS, v);
    duck_state r = out_h(DK("value_create_varchar", "iiii", I(f), I(SPS), I(SPO), EP(err)), out, err);
    tfree(a);
    return r;
}

static duck_state s_value_create_interval(duck_factory f, const duck_interval* v, duck_value* out, duck_err* err) {
    dk_put(SPX, v, sizeof *v);
    return out_h(DK("value_create_interval", "iiii", I(f), I(SPX), I(SPO), EP(err)), out, err);
}

static duck_state s_value_create_uuid(duck_factory f, const duck_hugeint* v, duck_value* out, duck_err* err) {
    dk_put(SPX, v, sizeof *v);
    return out_h(DK("value_create_uuid", "iiii", I(f), I(SPX), I(SPO), EP(err)), out, err);
}

static duck_state s_value_create_list(duck_factory f, duck_logical_type child, const duck_value* kids, duck_idx_t n,
                                      duck_value* out, duck_err* err) {
    tp a = handles_in((void* const*)kids, n);
    duck_state r = out_h(DK("value_create_list", "iiijii", I(f), I(child), I(a), J(n), I(SPO), EP(err)), out, err);
    tfree(a);
    return r;
}

static duck_state s_column_data_collection_create(duck_factory f, const duck_logical_type* types, duck_idx_t n,
                                                  duck_collection* out, duck_err* err) {
    tp a = handles_in((void* const*)types, n);
    duck_state r = out_h(DK("column_data_collection_create", "iijii", I(f), I(a), J(n), I(SPO), EP(err)), out, err);
    tfree(a);
    return r;
}

static duck_state s_column_data_collection_destroy(duck_collection* c) { return DESTROY("column_data_collection_destroy", c); }

static duck_state s_column_data_collection_append_state_create(duck_collection c, duck_append_state* out, duck_err* err) {
    return out_h(DK("column_data_collection_append_state_create", "iii", I(c), I(SPO), EP(err)), out, err);
}

static duck_state s_column_data_collection_append_state_destroy(duck_append_state* s) {
    return DESTROY("column_data_collection_append_state_destroy", s);
}

static duck_state s_column_data_collection_append(duck_collection c, duck_append_state s, duck_data_chunk chunk, duck_err* err) {
    flush_chunk(H(chunk));
    return fin(DK("column_data_collection_append", "iiii", I(c), I(s), I(chunk), EP(err)), err);
}

static duck_state s_replacement_scan_create(duck_factory f, duck_rscan* out, duck_err* err) {
    return out_h(DK("replacement_scan_create", "iii", I(f), I(SPO), EP(err)), out, err);
}

/* DuckDB calls a slot of ITS function table: the host grows it by a trampoline that lands in q_wasm_duck_rscan */
static duck_state s_replacement_scan_set_callback(duck_rscan scan, duck_rscan_fn fn, duck_err* err) {
    return fin(DK("replacement_scan_set_callback", "iii", I(scan), I(dk_trampoline(H(fn))), EP(err)), err);
}

EMSCRIPTEN_KEEPALIVE
void q_wasm_duck_rscan(duck_rscan_fn fn, tp info, tp ctx, tp errp) {
    duck_err e = NULL;
    if (++g_depth >= SCR_DEPTH) abort();
    fn(P(info), P(ctx), &e);
    g_depth--;
    if (errp) put32(errp, H(e));
}

/* the pointer alone crosses: a destructor or comparator of ours could never be called from their side */
static duck_state s_replacement_scan_set_user_data(duck_rscan scan, duck_opaque* data, duck_err* err) {
    put32(SPX, H(data->ptr));
    put32(SPX + 4, 0);
    put32(SPX + 8, 0);
    return fin(DK("replacement_scan_set_user_data", "iii", I(scan), I(SPX), EP(err)), err);
}

static duck_state s_connection_register_replacement_scan(duck_connection con, duck_rscan scan, duck_err* err) {
    return fin(DK("connection_register_replacement_scan", "iii", I(con), I(scan), EP(err)), err);
}

static duck_state s_replacement_scan_destroy(duck_rscan* scan) { return DESTROY("replacement_scan_destroy", scan); }

static duck_state s_replacement_scan_get_user_data(duck_rscan_info info, void** out, duck_err* err) {
    return out_h(DK("replacement_scan_get_user_data", "iii", I(info), I(SPO), EP(err)), out, err);
}

static duck_state s_replacement_scan_get_name(duck_rscan_info info, duck_qname* out, duck_err* err) {
    return out_h(DK("replacement_scan_get_name", "iii", I(info), I(SPO), EP(err)), out, err);
}

static duck_state s_replacement_scan_set_collection(duck_rscan_info info, duck_collection c, const duck_str* names,
                                                    duck_idx_t n, duck_err* err) {
    strs_t a = strs_in(names, n);
    duck_state r = fin(DK("replacement_scan_set_collection", "iiiji", I(info), I(c), I(a.arr), J(n), EP(err)), err);
    strs_free(&a);
    return r;
}

static duck_state s_qname_get_part_count(duck_qname q, duck_idx_t* out, duck_err* err) {
    return out_64(DK("qname_get_part_count", "iii", I(q), I(SPO), EP(err)), out, err);
}

static duck_state s_qname_get_part(duck_qname q, duck_idx_t i, duck_str* out, duck_err* err) {
    duck_state r = DK("qname_get_part", "ijii", I(q), J(i), I(SPS), EP(err));
    *out = r == QDuckSuccess ? borrow((uintptr_t)q, SPS) : (duck_str){ NULL, 0 };
    return fin(r, err);
}

static duck_state s_qname_destroy(duck_qname* q) {
    borrow_drop((uintptr_t)*q);
    return DESTROY("qname_destroy", q);
}

int q_wasm_duck_bind(duck_api_t* api, char* why, size_t cap) {
    if (!dk_load(Q_WASM_DUCKDB_PIN, why, (int)cap)) return 0;
    if (!g_scr) g_scr = tmalloc(SCR_BYTES * SCR_DEPTH);
    dk_repo(g_repo, sizeof g_repo);
    *api = (duck_api_t){
        .environment_create = s_environment_create, .environment_destroy = s_environment_destroy,
        .instance_create_with_options = s_instance_create_with_options, .instance_destroy = s_instance_destroy,
        .instance_attach = s_instance_attach, .connection_create = s_connection_create,
        .connection_destroy = s_connection_destroy, .connection_get_factory = s_connection_get_factory,
        .error_info_get_text = s_error_info_get_text, .error_info_destroy = s_error_info_destroy,
        .parse_sql = s_parse_sql, .statement_iterator_next = s_statement_iterator_next,
        .statement_iterator_destroy = s_statement_iterator_destroy, .sql_statement_destroy = s_sql_statement_destroy,
        .sql_statement_get_parameter_count = s_sql_statement_get_parameter_count,
        .sql_statement_get_parameter_name = s_sql_statement_get_parameter_name,
        .sql_statement_get_type = s_sql_statement_get_type, .statement_bind = s_statement_bind,
        .statement_execute = s_statement_execute, .result_destroy = s_result_destroy,
        .result_fetch_chunk = s_result_fetch_chunk, .result_get_result_type = s_result_get_result_type,
        .result_get_schema = s_result_get_schema, .schema_get_count = s_schema_get_count,
        .schema_get_field = s_schema_get_field, .schema_destroy = s_schema_destroy,
        .data_chunk_create = s_data_chunk_create, .data_chunk_destroy = s_data_chunk_destroy,
        .data_chunk_get_size = s_data_chunk_get_size, .data_chunk_get_vector = s_data_chunk_get_vector,
        .vector_get_logical_type = s_vector_get_logical_type, .vector_flatten = s_vector_flatten,
        .vector_get_view = s_vector_get_view, .vector_get_data_mutable = s_vector_get_data_mutable,
        .vector_flat_get_validity_mutable = s_vector_flat_get_validity_mutable, .vector_get_size = s_vector_get_size,
        .vector_set_size = s_vector_set_size, .vector_get_arena = s_vector_get_arena,
        .arena_allocate = s_arena_allocate, .vector_get_child = s_vector_get_child,
        .vector_get_value = s_vector_get_value, .logical_type_copy = s_logical_type_copy,
        .logical_type_destroy = s_logical_type_destroy, .logical_type_get_id = s_logical_type_get_id,
        .logical_type_get_name = s_logical_type_get_name, .logical_type_get_param_count = s_logical_type_get_param_count,
        .logical_type_get_param = s_logical_type_get_param, .factory_create_type_from_id = s_factory_create_type_from_id,
        .factory_create_type_from_text = s_factory_create_type_from_text, .value_destroy = s_value_destroy,
        .value_is_null = s_value_is_null, .value_to_string = s_value_to_string, .value_get_type = s_value_get_type,
        .value_get_varchar = s_value_get_varchar, .value_get_utinyint = s_value_get_utinyint,
        .value_get_bigint = s_value_get_bigint, .value_create_type = s_value_create_type,
        .value_create_null = s_value_create_null, .value_create_bool = s_value_create_bool,
        .value_create_utinyint = s_value_create_utinyint, .value_create_smallint = s_value_create_smallint,
        .value_create_int = s_value_create_int, .value_create_bigint = s_value_create_bigint,
        .value_create_float = s_value_create_float, .value_create_double = s_value_create_double,
        .value_create_varchar = s_value_create_varchar, .value_create_date = s_value_create_date,
        .value_create_interval = s_value_create_interval, .value_create_timestamp = s_value_create_timestamp,
        .value_create_timestamp_ns = s_value_create_timestamp_ns, .value_create_uuid = s_value_create_uuid,
        .value_create_list = s_value_create_list, .column_data_collection_create = s_column_data_collection_create,
        .column_data_collection_destroy = s_column_data_collection_destroy,
        .column_data_collection_append_state_create = s_column_data_collection_append_state_create,
        .column_data_collection_append_state_destroy = s_column_data_collection_append_state_destroy,
        .column_data_collection_append = s_column_data_collection_append,
        .replacement_scan_create = s_replacement_scan_create,
        .replacement_scan_set_callback = s_replacement_scan_set_callback,
        .replacement_scan_set_user_data = s_replacement_scan_set_user_data,
        .connection_register_replacement_scan = s_connection_register_replacement_scan,
        .replacement_scan_destroy = s_replacement_scan_destroy,
        .replacement_scan_get_user_data = s_replacement_scan_get_user_data,
        .replacement_scan_get_name = s_replacement_scan_get_name,
        .replacement_scan_set_collection = s_replacement_scan_set_collection,
        .qname_get_part_count = s_qname_get_part_count, .qname_get_part = s_qname_get_part,
        .qname_destroy = s_qname_destroy,
    };
    return 1;
}
