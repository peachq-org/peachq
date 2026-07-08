/* q_wire — kdb IPC wire-format codec.  Format contract + clean-room
 * provenance: see q_wire.h.  No frozen-base file is touched: this is a
 * q-layer TU; `-8!`/`-9!` dispatch lives in q_registry.c's `!` wrapper. */
#include "qlang/q_wire.h"
#include "qlang/q_deriv.h"      /* lambda carrier inspectors */
#include "qlang/q_registry.h"   /* q_collapse_list */
#include "qlang/q_parse.h"      /* q_parse/q_lower — lambda decode (RUNTIME only) */
#include "lang/eval.h"          /* ray_eval */
#include "table/sym.h"          /* ray_sym_vec_cell */
#include "mem/heap.h"           /* RAY_ATTR_HAS_NULLS */
#include "store/serde.h"        /* fn-serde hook getters (serde mode ext 200) */
#include "lang/env.h"           /* ray_fn_name, ray_env_get */
#include <math.h>               /* isinf */
#include <stdint.h>
#include <string.h>

/* Nesting ceiling shared by writer and reader — mirrors serde.c's
 * RAY_DE_MAX_DEPTH so a crafted frame (or a pathological value) errors
 * instead of overflowing the C stack.  Thread-local: codec may run on
 * worker threads in later phases. */
#define Q_WIRE_MAX_DEPTH 512
static _Thread_local int g_wire_depth = 0;

/* ==========================================================================
 * Writer
 * ========================================================================== */

static int wbuf_fail(q_wire_wbuf_t* b, ray_t* err) {
    if (!b->err) b->err = err;
    else if (err) ray_error_free(err);
    return -1;
}

static int wbuf_reserve(q_wire_wbuf_t* b, size_t need) {
    if (b->len + need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + need) cap *= 2;
    uint8_t* p = (uint8_t*)ray_realloc_raw(b->p, cap);
    if (!p) return wbuf_fail(b, ray_error("wsfull", "q_wire: out of memory"));
    b->p = p;
    b->cap = cap;
    return 0;
}

void q_wire_wbuf_free(q_wire_wbuf_t* b) {
    if (b->p) ray_free_raw(b->p);
    if (b->err) ray_error_free(b->err);
    b->p = NULL; b->len = b->cap = 0; b->err = NULL;
}

static int w_raw(q_wire_wbuf_t* b, const void* src, size_t n) {
    if (wbuf_reserve(b, n)) return -1;
    memcpy(b->p + b->len, src, n);
    b->len += n;
    return 0;
}

/* Little-endian scalar emitters (byte-shift: host-endianness independent). */
static int w_u8(q_wire_wbuf_t* b, uint8_t v) { return w_raw(b, &v, 1); }
static int w_i16(q_wire_wbuf_t* b, int16_t v) {
    uint8_t d[2] = { (uint8_t)v, (uint8_t)((uint16_t)v >> 8) };
    return w_raw(b, d, 2);
}
static int w_i32(q_wire_wbuf_t* b, int32_t v) {
    uint32_t u = (uint32_t)v;
    uint8_t d[4] = { (uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24) };
    return w_raw(b, d, 4);
}
static int w_i64(q_wire_wbuf_t* b, int64_t v) {
    uint64_t u = (uint64_t)v;
    uint8_t d[8];
    for (int i = 0; i < 8; i++) d[i] = (uint8_t)(u >> (8 * i));
    return w_raw(b, d, 8);
}
static int w_f64(q_wire_wbuf_t* b, double v) {
    uint64_t u; memcpy(&u, &v, 8);
    return w_i64(b, (int64_t)u);
}
static int w_f32(q_wire_wbuf_t* b, float v) {
    uint32_t u; memcpy(&u, &v, 4);
    return w_i32(b, (int32_t)u);
}
/* NUL-terminated string (kdb sym / error / lambda-context encoding). */
static int w_cstr(q_wire_wbuf_t* b, const char* s, size_t n) {
    if (w_raw(b, s, n)) return -1;
    return w_u8(b, 0);
}

/* Vector length as the wire's int32 count ('limit beyond 2^31-1). */
static int w_count(q_wire_wbuf_t* b, int64_t len) {
    if (len < 0 || len > INT32_MAX)
        return wbuf_fail(b, ray_error("limit", "q_wire: vector length %lld exceeds wire int32", (long long)len));
    return w_i32(b, (int32_t)len);
}

/* char vector object: 0x0a attrs count bytes */
static int w_charvec(q_wire_wbuf_t* b, const char* s, int64_t n) {
    if (w_u8(b, 10) || w_u8(b, 0) || w_count(b, n)) return -1;
    return w_raw(b, s, (size_t)n);
}

/* sym text by intern id, NUL-terminated (empty text for unknown ids). */
static int w_sym_id(q_wire_wbuf_t* b, int64_t id) {
    ray_t* s = ray_sym_str(id);                       /* borrowed */
    if (s && !RAY_IS_ERR(s)) return w_cstr(b, ray_str_ptr(s), ray_str_len(s));
    return w_u8(b, 0);
}

int q_wire_write_obj(q_wire_wbuf_t* b, ray_t* x) {
    if (b->err) return -1;
    /* serde mode: a C-NULL slot (v4 wrote marker 126, read back as the null
     * singleton) maps to (::) — identical to RAY_NULL_OBJ, matching v4's
     * read-side conflation.  The wire path refuses C NULL outright. */
    if (!x) {
        if (b->serde) return (w_u8(b, 101) || w_u8(b, 0)) ? -1 : 0;
        return wbuf_fail(b, ray_error("type", "q_wire: nil value"));
    }
    if (g_wire_depth >= Q_WIRE_MAX_DEPTH)
        return wbuf_fail(b, ray_error("limit", "q_wire: nesting exceeds max depth %d", Q_WIRE_MAX_DEPTH));
    g_wire_depth++;
    int rc = -1;

    /* error -128h */
    if (RAY_IS_ERR(x)) {
        const char* code = ray_err_code(x);
        rc = (w_u8(b, 0x80) || w_cstr(b, code, strlen(code))) ? -1 : 0;
        goto out;
    }
    /* (::) generic null 101h */
    if (RAY_IS_NULL(x)) {
        rc = (w_u8(b, 101) || w_u8(b, 0)) ? -1 : 0;
        goto out;
    }
    /* q derived-verb carriers — WIRE mode only.  In serde mode ALL carriers
     * (lambda/projection/monad) are ordinary boxed lists and serialize
     * STRUCTURALLY (marker sym + fields; the base RAY_LAMBDA goes through
     * ext 201, builtin bases through ext 200) — exact rebuild, no q-registry
     * dependence at journal replay, matching v4.  On the wire, a lambda
     * carrier is kdb 100h (context + source) and projections are 'nyi. */
    switch (b->serde ? Q_DERIV_NONE : q_deriv_kind_of(x)) {
    case Q_DERIV_LAMBDA: {
        ray_t* src = q_lambda_src(x);                 /* borrowed */
        if (!src || src->type != -RAY_STR) {
            rc = wbuf_fail(b, ray_error("type", "q_wire: lambda carrier without source"));
            goto out;
        }
        rc = (w_u8(b, 100) || w_u8(b, 0) /* root context "" */ ||
              w_charvec(b, ray_str_ptr(src), (int64_t)ray_str_len(src))) ? -1 : 0;
        goto out;
    }
    case Q_DERIV_PROJ:
    case Q_DERIV_MONAD:
        rc = wbuf_fail(b, ray_error("nyi", "q_wire: derived-verb/projection serialization (104h) not yet implemented"));
        goto out;
    default: break;
    }

    int8_t t = x->type;

    /* ---- serde-mode extension records (storage/journal; q_wire.h) ---- */
    if (b->serde) {
        if (t == RAY_UNARY || t == RAY_BINARY || t == RAY_VARY) {
            /* ext 200: builtin fn by name — writer hook first, then aux name */
            char scratch[16];
            const char* name = NULL;
            ray_serde_fn_writer_t wh = ray_serde_fn_writer_hook();
            if (wh && wh(x, scratch)) name = scratch;
            else name = ray_fn_name(x);
            size_t nlen = name ? strlen(name) : 0;
            if (nlen > 15) nlen = 15;
            rc = (w_u8(b, Q_WIRE_EXT_FN) || w_u8(b, (uint8_t)t) ||
                  w_cstr(b, name ? name : "", nlen)) ? -1 : 0;
            goto out;
        }
        if (t == RAY_LAMBDA) {
            /* ext 201: raw engine lambda — attrs + params + body (v4 shape) */
            ray_t** slots = (ray_t**)ray_data(x);
            rc = (w_u8(b, Q_WIRE_EXT_LAMBDA) || w_u8(b, x->attrs) ||
                  q_wire_write_obj(b, slots[0]) ||
                  q_wire_write_obj(b, slots[1])) ? -1 : 0;
            goto out;
        }
        if (t == -RAY_SYM && (x->attrs & ATTR_QUOTED)) {
            /* ext 203: quoted sym atom */
            rc = w_u8(b, Q_WIRE_EXT_QSYM) ? -1 : 0;
            if (rc == 0) rc = w_sym_id(b, x->i64) ? -1 : 0;
            goto out;
        }
        if ((t == -RAY_BOOL || t == -RAY_U8) && RAY_ATOM_IS_NULL(x)) {
            /* ext 204: aux-bit typed null (no in-band sentinel exists) */
            rc = (w_u8(b, Q_WIRE_EXT_TNULL) || w_u8(b, (uint8_t)t)) ? -1 : 0;
            goto out;
        }
        if (t == RAY_STR) {
            /* ext 202: string COLUMN keeps its type (col.c requires it) */
            if (w_u8(b, Q_WIRE_EXT_STRVEC) ||
                w_u8(b, x->attrs & RAY_ATTR_HAS_NULLS) ||
                w_count(b, x->len)) goto out;
            rc = 0;
            for (int64_t i = 0; i < x->len && rc == 0; i++) {
                size_t n = 0;
                const char* s = ray_str_vec_get(x, i, &n);
                if (n > INT32_MAX) {
                    rc = wbuf_fail(b, ray_error("limit", "q_wire: string cell exceeds int32"));
                    break;
                }
                rc = (w_i32(b, (int32_t)n) || w_raw(b, s ? s : "", n)) ? -1 : 0;
            }
            goto out;
        }
    }

    /* ---- atoms (negative type; no attrs byte) ---- */
    if (t < 0) {
        switch (-t) {
        case RAY_BOOL: rc = (w_u8(b, (uint8_t)-RAY_BOOL) || w_u8(b, x->b8 ? 1 : 0)) ? -1 : 0; goto out;
        case RAY_GUID: {
            static const uint8_t zero[16] = {0};
            const uint8_t* g = x->obj ? (const uint8_t*)((char*)x->obj + sizeof(ray_t)) : zero;
            rc = (w_u8(b, (uint8_t)-RAY_GUID) || w_raw(b, g, 16)) ? -1 : 0;
            goto out;
        }
        case RAY_U8:  rc = (w_u8(b, (uint8_t)-RAY_U8)  || w_u8(b, x->u8))   ? -1 : 0; goto out;
        case RAY_I16: rc = (w_u8(b, (uint8_t)-RAY_I16) || w_i16(b, x->i16)) ? -1 : 0; goto out;
        case RAY_I32: rc = (w_u8(b, (uint8_t)-RAY_I32) || w_i32(b, x->i32)) ? -1 : 0; goto out;
        case RAY_I64: rc = (w_u8(b, (uint8_t)-RAY_I64) || w_i64(b, x->i64)) ? -1 : 0; goto out;
        case RAY_F32: rc = (w_u8(b, (uint8_t)-RAY_F32) || w_f32(b, (float)x->f64)) ? -1 : 0; goto out;
        case RAY_F64: rc = (w_u8(b, (uint8_t)-RAY_F64) || w_f64(b, x->f64)) ? -1 : 0; goto out;
        case RAY_STR: {
            /* len 1 -> char atom (kdb "a"); else char vector.  q_wire.h. */
            size_t n = ray_str_len(x);
            const char* p = ray_str_ptr(x);
            if (n == 1) rc = (w_u8(b, 0xf6) || w_u8(b, (uint8_t)p[0])) ? -1 : 0;
            else        rc = w_charvec(b, p, (int64_t)n);
            goto out;
        }
        case RAY_SYM: rc = (w_u8(b, (uint8_t)-RAY_SYM) || w_sym_id(b, x->i64)) ? -1 : 0; goto out;
        case RAY_TIMESTAMP: rc = (w_u8(b, (uint8_t)-RAY_TIMESTAMP) || w_i64(b, x->i64)) ? -1 : 0; goto out;
        /* f64-backed temporal: payload is days since 2000.01.01 (kdb 15h).
         * Null is the in-band NaN; 0Wz cannot exist (float canon), so no
         * infinity handling on the write side. */
        case RAY_DATETIME: rc = (w_u8(b, (uint8_t)-RAY_DATETIME) || w_f64(b, x->f64)) ? -1 : 0; goto out;
        /* i32-class temporal atom payloads are read via the i32 union field:
         * producers are mixed (constructors store i64, ray_typed_null stores
         * i32) and only agree through LE aliasing — x->i32 matches the null
         * producer and every other q-layer reader (internal.h). */
        case RAY_DATE: rc = (w_u8(b, (uint8_t)-RAY_DATE) || w_i32(b, x->i32)) ? -1 : 0; goto out;
        case RAY_TIME: rc = (w_u8(b, (uint8_t)-RAY_TIME) || w_i32(b, x->i32)) ? -1 : 0; goto out;
        case RAY_MONTH: rc = (w_u8(b, (uint8_t)-RAY_MONTH) || w_i32(b, x->i32)) ? -1 : 0; goto out;
        case RAY_TIMESPAN: rc = (w_u8(b, (uint8_t)-RAY_TIMESPAN) || w_i64(b, x->i64)) ? -1 : 0; goto out;
        case RAY_MINUTE: rc = (w_u8(b, (uint8_t)-RAY_MINUTE) || w_i32(b, x->i32)) ? -1 : 0; goto out;
        case RAY_SECOND: rc = (w_u8(b, (uint8_t)-RAY_SECOND) || w_i32(b, x->i32)) ? -1 : 0; goto out;
        default:
            rc = wbuf_fail(b, ray_error("nyi", "q_wire: type %d has no kdb wire tag", (int)t));
            goto out;
        }
    }

    /* ---- vectors / compounds ---- */
    switch (t) {
    case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
    case RAY_F32:  case RAY_F64: case RAY_GUID:
    case RAY_TIMESTAMP: case RAY_DATE: case RAY_TIME:
    case RAY_MONTH: case RAY_MINUTE: case RAY_SECOND: case RAY_TIMESPAN:
    case RAY_DATETIME: {
        /* fixed-width payloads are bit-identical to kdb on LE hosts.
         * serde mode carries HAS_NULLS (the reader rescans sentinel types,
         * but GUID nulls — all-zero payload — are only knowable from the
         * flag); wire mode emits kdb's 0. */
        uint8_t vattrs = b->serde ? (uint8_t)(x->attrs & RAY_ATTR_HAS_NULLS) : 0;
        if (w_u8(b, (uint8_t)t) || w_u8(b, vattrs) || w_count(b, x->len)) goto out;
        uint8_t esz = ray_type_sizes[(uint8_t)t];
        const uint8_t* d = (const uint8_t*)ray_data(x);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        rc = w_raw(b, d, (size_t)x->len * esz);
#else
        rc = 0;
        for (int64_t i = 0; i < x->len && rc == 0; i++) {
            const uint8_t* e = d + (size_t)i * esz;
            switch (esz) {
            case 1: rc = w_u8(b, e[0]); break;
            case 2: { int16_t v; memcpy(&v, e, 2); rc = w_i16(b, v); } break;
            case 4: { int32_t v; memcpy(&v, e, 4); rc = w_i32(b, v); } break;
            case 8: { int64_t v; memcpy(&v, e, 8); rc = w_i64(b, v); } break;
            default: rc = w_raw(b, e, esz); break; /* GUID: byte array */
            }
        }
#endif
        goto out;
    }
    case RAY_STR: {
        /* string column -> kdb list of char vectors (q_wire.h) */
        if (w_u8(b, 0) || w_u8(b, 0) || w_count(b, x->len)) goto out;
        rc = 0;
        for (int64_t i = 0; i < x->len && rc == 0; i++) {
            size_t n = 0;
            const char* s = ray_str_vec_get(x, i, &n);
            rc = w_charvec(b, s ? s : "", (int64_t)n);
        }
        goto out;
    }
    case RAY_SYM: {
        if (w_u8(b, (uint8_t)RAY_SYM) || w_u8(b, 0) || w_count(b, x->len)) goto out;
        rc = 0;
        for (int64_t i = 0; i < x->len && rc == 0; i++) {
            ray_t* s = ray_sym_vec_cell(x, i);        /* borrowed; NULL = empty */
            rc = (s && !RAY_IS_ERR(s)) ? w_cstr(b, ray_str_ptr(s), ray_str_len(s))
                                       : w_u8(b, 0);
        }
        goto out;
    }
    case RAY_LIST: {
        /* WIRE mode: kdb has no boxed homogeneous-atom lists — they ARE
         * typed vectors (a kdb (2i;3i) is 2 3i).  Engine ops (dict vals,
         * map results) box such runs; collapse so the bytes match kdb's
         * canonical form.  SERDE mode: engine boxing is value state —
         * never collapse, and carry the engine attrs byte (QUOTED lists
         * are journaled expression trees). */
        if (!b->serde) {
            ray_t* cx = q_collapse_list(x);
            if (cx && !RAY_IS_ERR(cx) && cx->type != RAY_LIST) {
                rc = q_wire_write_obj(b, cx);
                ray_release(cx);
                goto out;
            }
            if (cx && !RAY_IS_ERR(cx)) ray_release(cx);
        }
        uint8_t attrs = b->serde ? (uint8_t)(x->attrs & ~RAY_ATTR_SLICE) : 0;
        if (w_u8(b, 0) || w_u8(b, attrs) || w_count(b, x->len)) goto out;
        ray_t** e = (ray_t**)ray_data(x);
        rc = 0;
        for (int64_t i = 0; i < x->len && rc == 0; i++)
            rc = q_wire_write_obj(b, e[i]);
        goto out;
    }
    case RAY_DICT: {
        /* generic recursion — keyed tables (dict of two tables) fall out */
        ray_t** slots = (ray_t**)ray_data(x);
        rc = (w_u8(b, 99) || q_wire_write_obj(b, slots[0]) ||
              q_wire_write_obj(b, slots[1])) ? -1 : 0;
        goto out;
    }
    case RAY_TABLE: {
        /* 0x62 attrs 0x63 symvector(names) list(columns) */
        ray_t** slots = (ray_t**)ray_data(x);
        ray_t* schema = slots[0];                     /* I64 vec of name ids */
        ray_t* cols   = slots[1];                     /* RAY_LIST of columns */
        if (!schema || schema->type != RAY_I64 || !cols || cols->type != RAY_LIST) {
            rc = wbuf_fail(b, ray_error("type", "q_wire: malformed table slots"));
            goto out;
        }
        if (w_u8(b, 98) || w_u8(b, 0) || w_u8(b, 99)) goto out;
        if (w_u8(b, (uint8_t)RAY_SYM) || w_u8(b, 0) || w_count(b, schema->len)) goto out;
        const int64_t* ids = (const int64_t*)ray_data(schema);
        rc = 0;
        for (int64_t i = 0; i < schema->len && rc == 0; i++)
            rc = w_sym_id(b, ids[i]);
        if (rc == 0) rc = q_wire_write_obj(b, cols);
        goto out;
    }
    default:
        rc = wbuf_fail(b, ray_error("nyi", "q_wire: type %d has no kdb wire tag", (int)t));
        goto out;
    }

out:
    g_wire_depth--;
    return rc;
}

ray_t* q_wire_serialize(ray_t* x, uint8_t msgtype) {
    q_wire_wbuf_t b = {0};
    /* 8-byte header placeholder: endian=LE, msgtype, uncompressed, pad, len */
    uint8_t hdr[8] = { 0x01, msgtype, 0x00, 0x00, 0, 0, 0, 0 };
    if (w_raw(&b, hdr, 8) || q_wire_write_obj(&b, x)) {
        ray_t* e = b.err ? b.err : ray_error("type", "q_wire: serialize failed");
        b.err = NULL;
        q_wire_wbuf_free(&b);
        return e;
    }
    if (b.len > INT32_MAX) {
        q_wire_wbuf_free(&b);
        return ray_error("limit", "q_wire: message exceeds int32 length");
    }
    uint32_t total = (uint32_t)b.len;
    b.p[4] = (uint8_t)total;
    b.p[5] = (uint8_t)(total >> 8);
    b.p[6] = (uint8_t)(total >> 16);
    b.p[7] = (uint8_t)(total >> 24);
    ray_t* out = ray_vec_from_raw(RAY_U8, b.p, (int64_t)b.len);
    q_wire_wbuf_free(&b);
    return out;
}

/* ==========================================================================
 * Reader
 * ========================================================================== */

/* Host endianness (compile-time).  The `swap` cursor field is FRAME-vs-HOST:
 * nonzero when the frame's byte order differs from the host's, i.e. the raw
 * fixed-width payload needs per-element byte swaps.  Scalar readers assemble
 * from bytes explicitly, driven by the frame's own endianness (`frame_be`),
 * so they are correct on either host. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define Q_WIRE_HOST_BE 1
#else
#define Q_WIRE_HOST_BE 0
#endif

typedef struct {
    const uint8_t* p;
    size_t rem;
    int    frame_be;    /* frame encoded big-endian (header byte 0 == 0x00) */
    int    swap;        /* frame byte order != host byte order */
    int    top_err_ok;  /* the TOP-LEVEL object decoded as a wire -128h error */
    int    depth0;      /* g_wire_depth at cursor creation (top-level marker) */
    int    serde;       /* serde mode: ext band on, list collapse off */
} rcur_t;

static int r_need(rcur_t* c, size_t n) { return c->rem >= n; }

static uint8_t r_u8(rcur_t* c) { uint8_t v = c->p[0]; c->p++; c->rem--; return v; }

static int16_t r_i16(rcur_t* c) {
    const uint8_t* d = c->p; c->p += 2; c->rem -= 2;
    uint16_t v = c->frame_be ? (uint16_t)((uint16_t)d[0] << 8 | d[1])
                             : (uint16_t)((uint16_t)d[1] << 8 | d[0]);
    int16_t r; memcpy(&r, &v, 2); return r;
}
static int32_t r_i32(rcur_t* c) {
    const uint8_t* d = c->p; c->p += 4; c->rem -= 4;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)d[i] << (8 * (c->frame_be ? 3 - i : i));
    int32_t r; memcpy(&r, &v, 4); return r;
}
static int64_t r_i64(rcur_t* c) {
    const uint8_t* d = c->p; c->p += 8; c->rem -= 8;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)d[i] << (8 * (c->frame_be ? 7 - i : i));
    int64_t r; memcpy(&r, &v, 8); return r;
}
static float r_f32(rcur_t* c) {
    int32_t v = r_i32(c); float f; memcpy(&f, &v, 4); return f;
}
static double r_f64(rcur_t* c) {
    int64_t v = r_i64(c); double f; memcpy(&f, &v, 8); return f;
}

/* NUL-terminated string; returns length via *n, advances past the NUL.
 * -1 when no NUL inside the remaining bytes. */
static int r_cstr(rcur_t* c, const char** s, size_t* n) {
    const uint8_t* nul = (const uint8_t*)memchr(c->p, 0, c->rem);
    if (!nul) return -1;
    *s = (const char*)c->p;
    *n = (size_t)(nul - c->p);
    c->rem -= *n + 1;
    c->p = nul + 1;
    return 0;
}

/* The engine's float model has no 0w: canonicalize ±Inf to NaN (null). */
static double f64_canon(double v) { return isinf(v) ? NULL_F64 : v; }
static float  f32_canon(float v)  { return isinf(v) ? NULL_F32 : v; }

static ray_t* trunc_err(const char* what) {
    return ray_error("domain", "q_wire: truncated frame reading %s", what);
}

static ray_t* rd_obj(rcur_t* c);

/* Fixed-width vector body: attrs(ignored) count payload.  Scalars are
 * byte-swapped for BE frames; float payloads canonicalized; the sentinel
 * scan sets RAY_ATTR_HAS_NULLS (invariant 16.4, vec.h). */
static ray_t* rd_fixed_vec(rcur_t* c, int8_t t) {
    if (!r_need(c, 5)) return trunc_err("vector header");
    uint8_t wattrs = r_u8(c);                         /* wire mode: ignored */
    int32_t count = r_i32(c);
    uint8_t esz = ray_type_sizes[(uint8_t)t];
    if (count < 0 || (uint64_t)count * esz > c->rem)
        return ray_error("domain", "q_wire: vector count %d out of range", (int)count);
    ray_t* v = ray_vec_new(t, count);
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("wsfull", NULL);
    v->len = count;
    uint8_t* d = (uint8_t*)ray_data(v);
    memcpy(d, c->p, (size_t)count * esz);
    c->p += (size_t)count * esz;
    c->rem -= (size_t)count * esz;
    if (c->swap && esz > 1 && t != RAY_GUID) {
        for (int32_t i = 0; i < count; i++) {
            uint8_t* e = d + (size_t)i * esz;
            switch (esz) {
            case 2: { uint16_t u; memcpy(&u, e, 2); u = __builtin_bswap16(u); memcpy(e, &u, 2); } break;
            case 4: { uint32_t u; memcpy(&u, e, 4); u = __builtin_bswap32(u); memcpy(e, &u, 4); } break;
            case 8: { uint64_t u; memcpy(&u, e, 8); u = __builtin_bswap64(u); memcpy(e, &u, 8); } break;
            default: break;
            }
        }
    }
    bool has_nulls = false;
    switch (t) {
    case RAY_I16: { int16_t* e = (int16_t*)d;
        for (int32_t i = 0; i < count; i++) if (e[i] == NULL_I16) { has_nulls = true; break; } } break;
    case RAY_I32: case RAY_DATE: case RAY_TIME:
    case RAY_MONTH: case RAY_MINUTE: case RAY_SECOND: { int32_t* e = (int32_t*)d;
        for (int32_t i = 0; i < count; i++) if (e[i] == NULL_I32) { has_nulls = true; break; } } break;
    case RAY_I64: case RAY_TIMESTAMP: case RAY_TIMESPAN: { int64_t* e = (int64_t*)d;
        for (int32_t i = 0; i < count; i++) if (e[i] == NULL_I64) { has_nulls = true; break; } } break;
    case RAY_F32: { float* e = (float*)d;
        for (int32_t i = 0; i < count; i++) { e[i] = f32_canon(e[i]); if (e[i] != e[i]) has_nulls = true; } } break;
    case RAY_F64: case RAY_DATETIME: { double* e = (double*)d;
        for (int32_t i = 0; i < count; i++) { e[i] = f64_canon(e[i]); if (e[i] != e[i]) has_nulls = true; } } break;
    default: break;
    }
    if (has_nulls) v->attrs |= RAY_ATTR_HAS_NULLS;
    /* serde mode: the flag also covers nulls the scan can't see (GUID
     * all-zero sentinel) — restore it from the frame. */
    if (c->serde && (wattrs & RAY_ATTR_HAS_NULLS)) v->attrs |= RAY_ATTR_HAS_NULLS;
    return v;
}

/* serde-mode extension records (q_wire.h band 200..236) */
static ray_t* rd_ext(rcur_t* c, uint8_t tag) {
    switch (tag) {
    case Q_WIRE_EXT_FN: {                 /* valence byte + name cstr */
        if (!r_need(c, 1)) return trunc_err("builtin valence");
        uint8_t valence = r_u8(c);
        if (valence != RAY_UNARY && valence != RAY_BINARY && valence != RAY_VARY)
            return ray_error("domain", "q_wire: bad builtin valence %d", (int)valence);
        const char* s; size_t n;
        if (r_cstr(c, &s, &n)) return trunc_err("builtin name");
        char name[16];
        if (n > sizeof name - 1) n = sizeof name - 1;
        memcpy(name, s, n); name[n] = 0;
        ray_serde_fn_reader_t rh = ray_serde_fn_reader_hook();
        if (rh) {
            ray_t* hooked = rh(name);
            if (hooked) return hooked;    /* owned by contract */
        }
        ray_t* fn = ray_env_get(ray_sym_intern(name, n));
        if (!fn) return ray_error("name", "q_wire: builtin '%s' not in environment", name);
        ray_retain(fn);
        return fn;
    }
    case Q_WIRE_EXT_LAMBDA: {             /* attrs + params + body (v4 shape) */
        if (!r_need(c, 1)) return trunc_err("lambda attrs");
        uint8_t attrs = r_u8(c);
        ray_t* params = rd_obj(c);
        if (!params || RAY_IS_ERR(params)) return params ? params : ray_error("domain", NULL);
        ray_t* body = rd_obj(c);
        if (!body || RAY_IS_ERR(body)) { ray_release(params); return body ? body : ray_error("domain", NULL); }
        ray_t* lam = ray_alloc(7 * sizeof(ray_t*));
        if (!lam || RAY_IS_ERR(lam)) { ray_release(params); ray_release(body); return lam ? lam : ray_error("wsfull", NULL); }
        lam->type  = RAY_LAMBDA;
        lam->attrs = attrs & (uint8_t)~RAY_ATTR_SLICE;
        lam->len   = 0;
        memset(ray_data(lam), 0, 7 * sizeof(ray_t*));
        ((ray_t**)ray_data(lam))[0] = params;
        ((ray_t**)ray_data(lam))[1] = body;
        return lam;
    }
    case Q_WIRE_EXT_STRVEC: {             /* attrs + count + [len bytes]* */
        if (!r_need(c, 5)) return trunc_err("str vector header");
        uint8_t attrs = r_u8(c);
        int32_t count = r_i32(c);
        /* every cell needs at least its 4-byte length prefix */
        if (count < 0 || (uint64_t)count * 4 > c->rem)
            return ray_error("domain", "q_wire: str vector count %d out of range", (int)count);
        ray_t* v = ray_vec_new(RAY_STR, count);
        if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("wsfull", NULL);
        for (int32_t i = 0; i < count; i++) {
            if (!r_need(c, 4)) { ray_release(v); return trunc_err("str cell length"); }
            int32_t n = r_i32(c);
            if (n < 0 || (uint64_t)n > c->rem) { ray_release(v); return ray_error("domain", "q_wire: str cell length %d out of range", (int)n); }
            v = ray_str_vec_append(v, (const char*)c->p, (size_t)n);
            if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("wsfull", NULL);
            c->p += (size_t)n; c->rem -= (size_t)n;
        }
        if (attrs & RAY_ATTR_HAS_NULLS) v->attrs |= RAY_ATTR_HAS_NULLS;
        return v;
    }
    case Q_WIRE_EXT_QSYM: {               /* quoted sym atom */
        const char* s; size_t n;
        if (r_cstr(c, &s, &n)) return trunc_err("quoted sym");
        ray_t* v = ray_sym(ray_sym_intern(s, n));
        if (v && !RAY_IS_ERR(v)) v->attrs |= ATTR_QUOTED;
        return v;
    }
    case Q_WIRE_EXT_TNULL: {              /* aux-bit typed null (BOOL/U8) */
        if (!r_need(c, 1)) return trunc_err("typed null");
        int8_t nt = (int8_t)r_u8(c);
        if (nt != -RAY_BOOL && nt != -RAY_U8)
            return ray_error("domain", "q_wire: typed-null ext for type %d", (int)nt);
        return ray_typed_null(nt);
    }
    default:
        return ray_error("domain", "q_wire: reserved extension tag %d", (int)tag);
    }
}

static ray_t* rd_obj_inner(rcur_t* c) {
    if (!r_need(c, 1)) return trunc_err("type tag");
    uint8_t raw = r_u8(c);
    if (raw >= Q_WIRE_EXT_TAG_FIRST && raw <= Q_WIRE_EXT_TAG_LAST) {
        if (c->serde) return rd_ext(c, raw);
        return ray_error("domain", "q_wire: unsupported wire type %d", (int)(int8_t)raw);
    }
    int8_t t = (int8_t)raw;

    /* ---- atoms ---- */
    if (t < 0) {
        switch (-t) {
        case 128: {                                   /* error -128h */
            const char* s; size_t n;
            if (r_cstr(c, &s, &n)) return trunc_err("error text");
            char code[16];
            if (n > sizeof code - 1) n = sizeof code - 1;
            memcpy(code, s, n); code[n] = 0;
            /* mark a SUCCESSFULLY decoded top-level wire error so the frame
             * gate can still reject trailing junk (a decode FAILURE error
             * must not be masked by the trailing-bytes check). */
            if (g_wire_depth == c->depth0 + 1) c->top_err_ok = 1;
            return ray_error(code, NULL);
        }
        case RAY_BOOL: if (!r_need(c, 1)) return trunc_err("bool"); return ray_bool(r_u8(c) != 0);
        case RAY_GUID: {
            if (!r_need(c, 16)) return trunc_err("guid");
            ray_t* g = ray_guid(c->p);
            c->p += 16; c->rem -= 16;
            return g;
        }
        case RAY_U8:  if (!r_need(c, 1)) return trunc_err("byte");  return ray_u8(r_u8(c));
        case RAY_I16: if (!r_need(c, 2)) return trunc_err("short"); return ray_i16(r_i16(c));
        case RAY_I32: if (!r_need(c, 4)) return trunc_err("int");   return ray_i32(r_i32(c));
        case RAY_I64: if (!r_need(c, 8)) return trunc_err("long");  return ray_i64(r_i64(c));
        case RAY_F32: if (!r_need(c, 4)) return trunc_err("real");  return ray_f32(f32_canon(r_f32(c)));
        case RAY_F64: if (!r_need(c, 8)) return trunc_err("float"); return ray_f64(f64_canon(r_f64(c)));
        case RAY_STR: {                               /* char atom -> 1-char string */
            if (!r_need(c, 1)) return trunc_err("char");
            char ch = (char)r_u8(c);
            return ray_str(&ch, 1);
        }
        case RAY_SYM: {
            const char* s; size_t n;
            if (r_cstr(c, &s, &n)) return trunc_err("sym");
            return ray_sym(ray_sym_intern(s, n));
        }
        case RAY_TIMESTAMP: if (!r_need(c, 8)) return trunc_err("timestamp"); return ray_timestamp(r_i64(c));
        case RAY_DATETIME: if (!r_need(c, 8)) return trunc_err("datetime"); return ray_datetime(f64_canon(r_f64(c)));
        case RAY_DATE: if (!r_need(c, 4)) return trunc_err("date"); return ray_date((int64_t)r_i32(c));
        case RAY_TIME: if (!r_need(c, 4)) return trunc_err("time"); return ray_time((int64_t)r_i32(c));
        case RAY_MONTH: if (!r_need(c, 4)) return trunc_err("month"); return ray_month((int64_t)r_i32(c));
        case RAY_TIMESPAN: if (!r_need(c, 8)) return trunc_err("timespan"); return ray_timespan(r_i64(c));
        case RAY_MINUTE: if (!r_need(c, 4)) return trunc_err("minute"); return ray_minute((int64_t)r_i32(c));
        case RAY_SECOND: if (!r_need(c, 4)) return trunc_err("second"); return ray_second((int64_t)r_i32(c));
        default:
            return ray_error("domain", "q_wire: unsupported wire type %d", (int)t);
        }
    }

    /* ---- vectors ---- */
    if (t >= RAY_BOOL && t <= RAY_TIME) {
        switch (t) {
        case RAY_STR: {                               /* 10h char vector -> string atom */
            if (!r_need(c, 5)) return trunc_err("char vector header");
            (void)r_u8(c);
            int32_t count = r_i32(c);
            if (count < 0 || (uint64_t)count > c->rem)
                return ray_error("domain", "q_wire: char vector count %d out of range", (int)count);
            ray_t* s = ray_str((const char*)c->p, (size_t)count);
            c->p += (size_t)count; c->rem -= (size_t)count;
            return s;
        }
        case RAY_SYM: {
            if (!r_need(c, 5)) return trunc_err("sym vector header");
            (void)r_u8(c);
            int32_t count = r_i32(c);
            if (count < 0 || (uint64_t)count > c->rem)
                return ray_error("domain", "q_wire: sym vector count %d out of range", (int)count);
            ray_t* v = ray_vec_new(RAY_SYM, count);   /* W64 runtime-domain */
            if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("wsfull", NULL);
            v->len = count;
            int64_t* ids = (int64_t*)ray_data(v);
            for (int32_t i = 0; i < count; i++) {
                const char* s; size_t n;
                if (r_cstr(c, &s, &n)) { v->len = i; ray_release(v); return trunc_err("sym vector cell"); }
                ids[i] = ray_sym_intern(s, n);
            }
            return v;
        }
        default: {
            uint8_t esz = ray_type_sizes[(uint8_t)t];
            if (esz == 0)
                return ray_error("domain", "q_wire: unsupported wire type %d", (int)t);
            return rd_fixed_vec(c, t);
        }
        }
    }

    switch (t) {
    case RAY_LIST: {                                  /* 0h general list */
        if (!r_need(c, 5)) return trunc_err("list header");
        uint8_t attrs = r_u8(c);
        int32_t count = r_i32(c);
        if (count < 0 || (uint64_t)count > c->rem)    /* every element >= 1 byte */
            return ray_error("domain", "q_wire: list count %d out of range", (int)count);
        ray_t* l = ray_list_new(count);
        if (!l || RAY_IS_ERR(l)) return l ? l : ray_error("wsfull", NULL);
        for (int32_t i = 0; i < count; i++) {
            ray_t* e = rd_obj(c);
            if (!e || RAY_IS_ERR(e)) { ray_release(l); return e ? e : ray_error("domain", NULL); }
            l = ray_list_append(l, e);
            ray_release(e);
            if (!l || RAY_IS_ERR(l)) return l ? l : ray_error("wsfull", NULL);
        }
        if (c->serde) {
            /* engine boxing is value state: no collapse; restore the engine
             * attrs (QUOTED expression trees) minus the slice bit. */
            l->attrs |= attrs & (uint8_t)~RAY_ATTR_SLICE;
            return l;
        }
        ray_t* out = q_collapse_list(l);              /* owned; parser-identical shape */
        ray_release(l);
        return out;
    }
    case 99: case 127: {                              /* dict / sorted dict */
        ray_t* k = rd_obj(c);
        if (!k || RAY_IS_ERR(k)) return k ? k : ray_error("domain", NULL);
        ray_t* v = rd_obj(c);
        if (!v || RAY_IS_ERR(v)) { ray_release(k); return v ? v : ray_error("domain", NULL); }
        int64_t kl = k->type == RAY_TABLE ? ray_table_nrows(k)
                   : (ray_is_vec(k) || k->type == RAY_LIST) ? ray_len(k) : -1;
        int64_t vl = v->type == RAY_TABLE ? ray_table_nrows(v)
                   : (ray_is_vec(v) || v->type == RAY_LIST) ? ray_len(v) : -1;
        if (kl < 0 || vl < 0 || kl != vl) {
            ray_release(k); ray_release(v);
            return ray_error("length", "q_wire: dict key/value counts must match");
        }
        return ray_dict_new(k, v);                    /* consumes both */
    }
    case 98: {                                        /* table */
        if (!r_need(c, 2)) return trunc_err("table header");
        (void)r_u8(c);                                /* attrs */
        if (r_u8(c) != 99)
            return ray_error("domain", "q_wire: table body must be a dict");
        ray_t* keys = rd_obj(c);
        if (!keys || RAY_IS_ERR(keys)) return keys ? keys : ray_error("domain", NULL);
        if (keys->type != RAY_SYM) {
            ray_release(keys);
            return ray_error("domain", "q_wire: table column names must be a sym vector");
        }
        ray_t* cols = rd_obj(c);
        if (!cols || RAY_IS_ERR(cols)) { ray_release(keys); return cols ? cols : ray_error("domain", NULL); }
        if (cols->type != RAY_LIST || cols->len != keys->len) {
            ray_release(keys); ray_release(cols);
            return ray_error("domain", "q_wire: table column list malformed");
        }
        ray_t* tbl = ray_table_new(keys->len);
        for (int64_t i = 0; i < keys->len && tbl && !RAY_IS_ERR(tbl); i++)
            tbl = ray_table_add_col(tbl, ray_vec_get_sym_id(keys, i),
                                    ray_list_get(cols, i));   /* col NOT consumed */
        ray_release(keys);
        ray_release(cols);
        return tbl ? tbl : ray_error("wsfull", NULL);
    }
    case 100: {                                       /* lambda: context + source */
        const char* ctx; size_t ctxn;
        if (r_cstr(c, &ctx, &ctxn)) return trunc_err("lambda context");
        ray_t* src = rd_obj(c);                       /* char vector -> string atom */
        if (!src || RAY_IS_ERR(src)) return src ? src : ray_error("domain", NULL);
        if (src->type != -RAY_STR) {
            ray_release(src);
            return ray_error("domain", "q_wire: lambda body must be a char vector");
        }
        size_t n = ray_str_len(src);
        const char* sp = ray_str_ptr(src);
        /* only lambda literals may be re-evaluated: the {…} body itself is
         * unevaluated at definition time, so this cannot run arbitrary code. */
        if (n < 2 || sp[0] != '{') {
            ray_release(src);
            return ray_error("domain", "q_wire: lambda source must be a {..} literal");
        }
        char* z = (char*)ray_alloc_raw(n + 1);
        if (!z) { ray_release(src); return ray_error("wsfull", NULL); }
        memcpy(z, sp, n); z[n] = 0;
        ray_release(src);
        ray_t* ast = q_parse(z);
        ray_free_raw(z);
        if (!ast || RAY_IS_ERR(ast)) return ast ? ast : ray_error("parse", NULL);
        ast = q_lower(ast);
        if (!ast || RAY_IS_ERR(ast)) return ast ? ast : ray_error("parse", NULL);
        ray_t* lam = ray_eval(ast);
        ray_release(ast);
        return lam;
    }
    case 101: {                                       /* (::) and unary primitives */
        if (!r_need(c, 1)) return trunc_err("unary primitive");
        uint8_t which = r_u8(c);
        if (which == 0) return RAY_NULL_OBJ;
        return ray_error("nyi", "q_wire: unary primitive %d", (int)which);
    }
    default:
        return ray_error("domain", "q_wire: unsupported wire type %d", (int)t);
    }
}

static ray_t* rd_obj(rcur_t* c) {
    if (g_wire_depth >= Q_WIRE_MAX_DEPTH)
        return ray_error("domain", "q_wire: nesting exceeds max depth %d", Q_WIRE_MAX_DEPTH);
    g_wire_depth++;
    ray_t* r = rd_obj_inner(c);
    g_wire_depth--;
    return r;
}

ray_t* q_wire_read_obj_ex(const uint8_t* buf, size_t len, size_t* consumed,
                          int swap, int serde) {
    /* public contract: `swap` = frame is big-endian (see q_wire.h) */
    rcur_t c = {0};
    c.p = buf;
    c.rem = len;
    c.frame_be = (swap != 0);
    c.swap = (swap != 0) != (Q_WIRE_HOST_BE != 0);
    c.depth0 = g_wire_depth;
    c.serde = serde;
    ray_t* r = rd_obj(&c);
    if (consumed) *consumed = len - c.rem;
    return r;
}

ray_t* q_wire_read_obj(const uint8_t* buf, size_t len, size_t* consumed, int swap) {
    return q_wire_read_obj_ex(buf, len, consumed, swap, 0);
}

int q_wire_write_obj_ex(q_wire_wbuf_t* b, ray_t* x, int serde) {
    b->serde = serde;
    return q_wire_write_obj(b, x);
}

ray_t* q_wire_deserialize(ray_t* bytes) {
    if (!bytes || bytes->type != RAY_U8)
        return ray_error("type", "q_wire: -9! expects a byte vector");
    const uint8_t* p = (const uint8_t*)ray_data(bytes);
    int64_t n = bytes->len;
    if (n < 9)
        return ray_error("domain", "q_wire: frame shorter than header");
    int frame_be;
    if (p[0] == 0x01)      frame_be = 0;
    else if (p[0] == 0x00) frame_be = 1;
    else return ray_error("domain", "q_wire: bad endianness byte 0x%02x", p[0]);
    if (p[2] != 0)
        return ray_error("nyi", "q_wire: compressed frames not yet implemented");
    uint32_t total = frame_be
        ? ((uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7])
        : ((uint32_t)p[7] << 24 | (uint32_t)p[6] << 16 | (uint32_t)p[5] << 8 | p[4]);
    if ((int64_t)total != n)
        return ray_error("domain", "q_wire: frame length %u does not match %lld bytes",
                         (unsigned)total, (long long)n);
    rcur_t c = {0};
    c.p = p + 8;
    c.rem = (size_t)(n - 8);
    c.frame_be = frame_be;
    c.swap = (frame_be != 0) != (Q_WIRE_HOST_BE != 0);
    c.depth0 = g_wire_depth;
    ray_t* r = rd_obj(&c);
    /* the payload must be exactly one object: reject trailing bytes for
     * ordinary values AND for successfully decoded wire errors (-128h);
     * a decode-failure error passes through untouched. */
    if (r && c.rem != 0 && (!RAY_IS_ERR(r) || c.top_err_ok)) {
        if (RAY_IS_ERR(r)) ray_error_free(r); else ray_release(r);
        return ray_error("domain", "q_wire: %lld trailing payload bytes",
                         (long long)c.rem);
    }
    return r;
}
