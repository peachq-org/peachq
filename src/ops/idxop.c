/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "idxop.h"
#include "ops/internal.h"
#include "mem/heap.h"
#include "mem/cow.h"
#include "vec/vec.h"
#include "table/table.h"
#include "table/sym.h"
#include "lang/eval.h"
#include "ops/ops.h"
#include "ops/rowsel.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Routing observability counters (diagnostic, unsynchronized) ── */
uint64_t ray_idx_consults[IDX_SITE__N];
uint64_t ray_idx_hits[IDX_SITE__N];

static void idx_stats_dump(void) {
    static const char* names[IDX_SITE__N] = {
        "filter-zone", "filter-bloom", "filter-hash", "filter-range",
        "in", "find", "sort", "distinct",
    };
    for (int i = 0; i < IDX_SITE__N; i++)
        if (ray_idx_consults[i] || ray_idx_hits[i])
            fprintf(stderr, "idx_route %-12s consults=%llu hits=%llu\n",
                    names[i],
                    (unsigned long long)ray_idx_consults[i],
                    (unsigned long long)ray_idx_hits[i]);
}

void ray_idx_stats_init(void) {
    if (getenv("RAY_IDX_STATS")) atexit(idx_stats_dump);
}

/* Width of one element of a numeric vector type, or 0 if unsupported. */
static int numeric_elem_size(int8_t t) {
    switch (t) {
    case RAY_BOOL: case RAY_U8:                       return 1;
    case RAY_I16:                                     return 2;
    case RAY_I32: case RAY_DATE: case RAY_TIME: case RAY_F32:  return 4;
    case RAY_I64: case RAY_TIMESTAMP: case RAY_F64:            return 8;
    default:                                          return 0;
    }
}

/* Read row i of a numeric vector as a 64-bit hash-input word.  Mirrors the
 * canonical-equality semantics in the rest of the codebase: -0.0 / +0.0
 * collapse, NaNs route per-row (caller treats NaN as its own bucket). */
static uint64_t numeric_key_word(const uint8_t* base, int8_t type, int64_t i) {
    int es = numeric_elem_size(type);
    if (type == RAY_F32 || type == RAY_F64) {
        double v;
        if (es == 4) { float t; memcpy(&t, base + i*4, 4); v = (double)t; }
        else         {           memcpy(&v, base + i*8, 8);                }
        v = clear_neg_zero(v);
        if (v != v) {                   /* NaN: per-row bucket via row hash */
            return (uint64_t)i * 0x9E3779B97F4A7C15ULL;
        }
        uint64_t bits;
        memcpy(&bits, &v, 8);
        return bits;
    }
    int64_t k = 0;
    switch (es) {
    case 1: k = (int64_t)base[i]; break;
    case 2: { int16_t t; memcpy(&t, base + i*2, 2); k = (int64_t)t; break; }
    case 4: { int32_t t; memcpy(&t, base + i*4, 4); k = (int64_t)t; break; }
    case 8: { int64_t t; memcpy(&t, base + i*8, 8); k =          t; break; }
    }
    return (uint64_t)k;
}

/* Returns true iff numeric vector v is non-descending.  v1 scope: rejects
 * (returns false) if any null or NaN is present — callers turn false into a
 * verify error.  Caller has already ensured numeric_elem_size(v->type) > 0. */
static bool vec_is_ascending(const ray_t* v) {
    int64_t n = v->len;
    if (n < 2) return true;
    if (v->attrs & RAY_ATTR_HAS_NULLS) {
        for (int64_t i = 0; i < n; i++)
            if (ray_vec_is_null((ray_t*)v, i)) return false;
    }
    const uint8_t* b = (const uint8_t*)ray_data((ray_t*)v);
    switch (v->type) {
    case RAY_BOOL: case RAY_U8: {
        const uint8_t* p = b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_I16: {
        const int16_t* p = (const int16_t*)b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_I32: case RAY_DATE: case RAY_TIME: {  /* TIME is 4-byte int32 */
        const int32_t* p = (const int32_t*)b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_I64: case RAY_TIMESTAMP: {
        const int64_t* p = (const int64_t*)b;
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_F32: {
        const float* p = (const float*)b;
        for (int64_t i = 0; i < n; i++) if (p[i] != p[i]) return false; /* NaN */
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    case RAY_F64: {
        const double* p = (const double*)b;
        for (int64_t i = 0; i < n; i++) if (p[i] != p[i]) return false; /* NaN */
        for (int64_t i = 1; i < n; i++) if (p[i] < p[i-1]) return false;
        return true;
    }
    default: return false;
    }
}

/* 64-bit avalanche mix (splittable hash from Stafford / xxhash). */
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* Smallest power of two >= n, clamped to >= 1. */
static uint64_t next_pow2(uint64_t n) {
    if (n <= 1) return 1;
    uint64_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* True iff all non-null rows are distinct.  Open-addressing probe over a
 * power-of-two table sized ~2x the row count.  v1: numeric vectors only.
 * Returns false on OOM so the caller raises a verify error (conservative). */
static bool vec_all_distinct(const ray_t* v) {
    int64_t n = v->len;
    if (n < 2) return true;
    uint64_t cap = next_pow2((uint64_t)n * 2 + 1);
    if (cap < 16) cap = 16;
    uint64_t mask = cap - 1;
    /* Transient scratch table, deliberately off-heap (freed before return);
     * it is never an owned ray_t, unlike the persistent hash-index table. */
    int64_t* slot = (int64_t*)calloc((size_t)cap, sizeof(int64_t)); /* 0 = empty; store i+1 */
    if (!slot) return false;
    const uint8_t* base = (const uint8_t*)ray_data((ray_t*)v);
    bool ok = true;
    for (int64_t i = 0; i < n && ok; i++) {
        if (ray_vec_is_null((ray_t*)v, i)) continue;
        uint64_t hi = numeric_key_word(base, v->type, i);
        uint64_t h = mix64(hi) & mask;
        for (;;) {
            int64_t cur = slot[h];
            if (cur == 0) { slot[h] = i + 1; break; }
            if (numeric_key_word(base, v->type, cur - 1) == hi) { ok = false; break; } /* dup */
            h = (h + 1) & mask;
        }
    }
    free(slot);
    return ok;
}

/* --------------------------------------------------------------------------
 * Index ray_t allocation / destruction helpers
 *
 * The block layout: 32-byte ray_t header + ray_index_t payload in data[].
 * type = RAY_INDEX, attrs = 0 (the index itself is never sliced or aliased),
 * len = sizeof(ray_index_t) (so callers can sanity-check the payload size).
 * -------------------------------------------------------------------------- */

static ray_t* ray_index_alloc(ray_idx_kind_t kind, int8_t parent_type, int64_t parent_len) {
    ray_t* idx = ray_alloc(sizeof(ray_index_t));
    if (!idx || RAY_IS_ERR(idx)) return idx;
    idx->type  = RAY_INDEX;
    idx->attrs = 0;
    idx->len   = (int64_t)sizeof(ray_index_t);
    memset(idx->data, 0, sizeof(ray_index_t));
    ray_index_t* ix = ray_index_payload(idx);
    ix->kind         = (uint8_t)kind;
    ix->parent_type  = parent_type;
    ix->built_for_len = parent_len;
    return idx;
}

/* --------------------------------------------------------------------------
 * Saved-aux retain / release
 *
 * The 16 byte snapshot preserves the parent's original aux-union bytes
 * across attach/detach.  Since index attach is restricted to numeric
 * types (see prepare_attach), the snapshot contains either:
 *   - all-zero bytes (no link, no nulls), or
 *   - bytes 8-15 hold an int64 link_target (HAS_LINK on I32/I64 cols).
 * Neither case carries an owning ray_t* reference, so retain/release
 * are no-ops.  The functions remain to preserve the heap.c / vec.c
 * call sites symmetric with the pre-migration layout. */

void ray_index_release_saved(ray_index_t* ix) {
    (void)ix;
}

void ray_index_retain_saved(ray_index_t* ix) {
    (void)ix;
}

/* --------------------------------------------------------------------------
 * Per-kind payload retain / release
 * -------------------------------------------------------------------------- */

void ray_index_release_payload(ray_index_t* ix) {
    switch ((ray_idx_kind_t)ix->kind) {
    case RAY_IDX_HASH:
        if (ix->u.hash.table && !RAY_IS_ERR(ix->u.hash.table))
            ray_release(ix->u.hash.table);
        if (ix->u.hash.chain && !RAY_IS_ERR(ix->u.hash.chain))
            ray_release(ix->u.hash.chain);
        ix->u.hash.table = ix->u.hash.chain = NULL;
        break;
    case RAY_IDX_SORT:
        if (ix->u.sort.perm && !RAY_IS_ERR(ix->u.sort.perm))
            ray_release(ix->u.sort.perm);
        ix->u.sort.perm = NULL;
        break;
    case RAY_IDX_BLOOM:
        if (ix->u.bloom.bits && !RAY_IS_ERR(ix->u.bloom.bits))
            ray_release(ix->u.bloom.bits);
        ix->u.bloom.bits = NULL;
        break;
    case RAY_IDX_CHUNK_ZONE:
        if (ix->u.chunk_zone.mins && !RAY_IS_ERR(ix->u.chunk_zone.mins))
            ray_release(ix->u.chunk_zone.mins);
        if (ix->u.chunk_zone.maxs && !RAY_IS_ERR(ix->u.chunk_zone.maxs))
            ray_release(ix->u.chunk_zone.maxs);
        if (ix->u.chunk_zone.null_bits && !RAY_IS_ERR(ix->u.chunk_zone.null_bits))
            ray_release(ix->u.chunk_zone.null_bits);
        ix->u.chunk_zone.mins = NULL;
        ix->u.chunk_zone.maxs = NULL;
        ix->u.chunk_zone.null_bits = NULL;
        break;
    case RAY_IDX_PART:
        if (ix->u.part.keys   && !RAY_IS_ERR(ix->u.part.keys))   ray_release(ix->u.part.keys);
        if (ix->u.part.starts && !RAY_IS_ERR(ix->u.part.starts)) ray_release(ix->u.part.starts);
        if (ix->u.part.lens   && !RAY_IS_ERR(ix->u.part.lens))   ray_release(ix->u.part.lens);
        ix->u.part.keys = ix->u.part.starts = ix->u.part.lens = NULL;
        break;
    case RAY_IDX_ZONE:
    case RAY_IDX_NONE:
        break;
    }
}

void ray_index_retain_payload(ray_index_t* ix) {
    switch ((ray_idx_kind_t)ix->kind) {
    case RAY_IDX_HASH:
        if (ix->u.hash.table && !RAY_IS_ERR(ix->u.hash.table))
            ray_retain(ix->u.hash.table);
        if (ix->u.hash.chain && !RAY_IS_ERR(ix->u.hash.chain))
            ray_retain(ix->u.hash.chain);
        break;
    case RAY_IDX_SORT:
        if (ix->u.sort.perm && !RAY_IS_ERR(ix->u.sort.perm))
            ray_retain(ix->u.sort.perm);
        break;
    case RAY_IDX_BLOOM:
        if (ix->u.bloom.bits && !RAY_IS_ERR(ix->u.bloom.bits))
            ray_retain(ix->u.bloom.bits);
        break;
    case RAY_IDX_CHUNK_ZONE:
        if (ix->u.chunk_zone.mins && !RAY_IS_ERR(ix->u.chunk_zone.mins))
            ray_retain(ix->u.chunk_zone.mins);
        if (ix->u.chunk_zone.maxs && !RAY_IS_ERR(ix->u.chunk_zone.maxs))
            ray_retain(ix->u.chunk_zone.maxs);
        if (ix->u.chunk_zone.null_bits && !RAY_IS_ERR(ix->u.chunk_zone.null_bits))
            ray_retain(ix->u.chunk_zone.null_bits);
        break;
    case RAY_IDX_PART:
        if (ix->u.part.keys   && !RAY_IS_ERR(ix->u.part.keys))   ray_retain(ix->u.part.keys);
        if (ix->u.part.starts && !RAY_IS_ERR(ix->u.part.starts)) ray_retain(ix->u.part.starts);
        if (ix->u.part.lens   && !RAY_IS_ERR(ix->u.part.lens))   ray_retain(ix->u.part.lens);
        break;
    case RAY_IDX_ZONE:
    case RAY_IDX_NONE:
        break;
    }
}

/* Deep-clone a RAY_INDEX block, sharing (retaining) its payload child vectors.
 * ray_alloc_copy CANNOT be used here: a RAY_INDEX block's type (97) is outside
 * the vector-type range, so ray_alloc_copy computes data_size==0 and copies
 * only the 32-byte header, silently dropping the ray_index_t payload (kind,
 * markers, child pointers).  Used when a marker must be set on an index block
 * that is shared after copy-on-write. */
static ray_t* clone_index_block(ray_t* blk) {
    ray_index_t* src = ray_index_payload(blk);
    ray_t* nb = ray_index_alloc((ray_idx_kind_t)src->kind, src->parent_type,
                                src->built_for_len);
    if (!nb || RAY_IS_ERR(nb)) return nb ? nb : ray_error("oom", NULL);
    ray_index_t* dst = ray_index_payload(nb);
    memcpy(dst, src, sizeof(ray_index_t));   /* kind, markers, saved_aux, union */
    ray_index_retain_payload(dst);           /* child vectors now referenced twice */
    ray_index_retain_saved(dst);             /* no-op for numeric, kept symmetric */
    return nb;
}

/* --------------------------------------------------------------------------
 * Zone scan -- compute min/max + null count
 *
 * Reads the parent vector before the aux is displaced.  Integer paths
 * cover BOOL/U8/I16/I32/I64/DATE/TIME/TIMESTAMP (all stored in int slots);
 * float paths cover F32/F64.  RAY_SYM/STR/GUID return RAY_ERR_NYI for now;
 * those types will get string-aware min/max in the P4 zone work.
 * -------------------------------------------------------------------------- */

static ray_err_t zone_scan_int(ray_t* v, ray_index_t* ix, int elem_size) {
    int64_t n = v->len;
    int64_t mn = INT64_MAX, mx = INT64_MIN;
    int64_t nn = 0;
    bool any_value = false;
    const uint8_t* base = (const uint8_t*)ray_data(v);

    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(v, i)) { nn++; continue; }
        int64_t val = 0;
        switch (elem_size) {
        case 1: val = (int64_t)base[i]; break;
        case 2: { int16_t t; memcpy(&t, base + i*2, 2); val = (int64_t)t; break; }
        case 4: { int32_t t; memcpy(&t, base + i*4, 4); val = (int64_t)t; break; }
        case 8: { int64_t t; memcpy(&t, base + i*8, 8); val = t;          break; }
        default: return RAY_ERR_TYPE;
        }
        if (val < mn) mn = val;
        if (val > mx) mx = val;
        any_value = true;
    }
    if (!any_value) { mn = 0; mx = 0; }
    ix->u.zone.min_i  = mn;
    ix->u.zone.max_i  = mx;
    ix->u.zone.n_nulls = nn;
    return RAY_OK;
}

static ray_err_t zone_scan_float(ray_t* v, ray_index_t* ix, int elem_size) {
    int64_t n = v->len;
    double mn = INFINITY, mx = -INFINITY;
    int64_t nn = 0;
    bool any_value = false;
    const uint8_t* base = (const uint8_t*)ray_data(v);

    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(v, i)) { nn++; continue; }
        double val = 0.0;
        if (elem_size == 4) {
            float t; memcpy(&t, base + i*4, 4); val = (double)t;
        } else {
            memcpy(&val, base + i*8, 8);
        }
        if (isnan(val)) continue;  /* NaNs don't participate in min/max */
        if (val < mn) mn = val;
        if (val > mx) mx = val;
        any_value = true;
    }
    if (!any_value) { mn = 0.0; mx = 0.0; }
    ix->u.zone.min_f  = mn;
    ix->u.zone.max_f  = mx;
    ix->u.zone.n_nulls = nn;
    return RAY_OK;
}

static ray_err_t zone_scan(ray_t* v, ray_index_t* ix) {
    switch (v->type) {
    case RAY_BOOL:
    case RAY_U8:        return zone_scan_int(v, ix, 1);
    case RAY_I16:       return zone_scan_int(v, ix, 2);
    case RAY_I32:
    case RAY_DATE:
    case RAY_TIME:      return zone_scan_int(v, ix, 4);  /* TIME is 4-byte int32 */
    case RAY_I64:
    case RAY_TIMESTAMP: return zone_scan_int(v, ix, 8);
    case RAY_F32:       return zone_scan_float(v, ix, 4);
    case RAY_F64:       return zone_scan_float(v, ix, 8);
    default:            return RAY_ERR_NYI;
    }
}

/* --------------------------------------------------------------------------
 * Chunk-zone scan -- per-(1<<chunk_log2)-row min/max + null flag
 *
 * For each chunk g in [0, n_chunks) the scan computes the chunk's min and
 * max value across its row range and sets the chunk's null-bit if any row
 * in that chunk is a null sentinel.  Whole-column extrema fall out as
 * min(mins[*]) / max(maxs[*]) so the reduce min/max path can consume this
 * index without needing a separate column-wide zone.
 * -------------------------------------------------------------------------- */

static ray_err_t chunk_zone_scan_int(ray_t* v, ray_index_t* ix,
                                     int elem_size) {
    uint32_t n_chunks = ix->u.chunk_zone.n_chunks;
    uint8_t  log2     = ix->u.chunk_zone.chunk_log2;
    int64_t  csz      = 1LL << log2;
    int64_t  n        = v->len;
    int64_t* mins     = (int64_t*)ray_data(ix->u.chunk_zone.mins);
    int64_t* maxs     = (int64_t*)ray_data(ix->u.chunk_zone.maxs);
    uint8_t* nbits    = (uint8_t*)ray_data(ix->u.chunk_zone.null_bits);
    const uint8_t* base = (const uint8_t*)ray_data(v);

    for (uint32_t g = 0; g < n_chunks; g++) {
        int64_t s = (int64_t)g * csz;
        int64_t e = s + csz; if (e > n) e = n;
        int64_t mn = INT64_MAX, mx = INT64_MIN;
        bool any_null = false;
        for (int64_t i = s; i < e; i++) {
            if (ray_vec_is_null(v, i)) { any_null = true; continue; }
            int64_t val = 0;
            switch (elem_size) {
            case 1: val = (int64_t)base[i]; break;
            case 2: { int16_t t; memcpy(&t, base + i*2, 2); val = (int64_t)t; break; }
            case 4: { int32_t t; memcpy(&t, base + i*4, 4); val = (int64_t)t; break; }
            case 8: { int64_t t; memcpy(&t, base + i*8, 8); val = t;          break; }
            default: return RAY_ERR_TYPE;
            }
            if (val < mn) mn = val;
            if (val > mx) mx = val;
        }
        /* Empty (all-null) chunks keep mn=INT64_MAX / mx=INT64_MIN so
         * the reduce path's min(mins[*]) / max(maxs[*]) ignores them. */
        mins[g] = mn;
        maxs[g] = mx;
        if (any_null) nbits[g >> 3] |= (uint8_t)(1u << (g & 7));
    }
    return RAY_OK;
}

static ray_err_t chunk_zone_scan_float(ray_t* v, ray_index_t* ix,
                                       int elem_size) {
    uint32_t n_chunks = ix->u.chunk_zone.n_chunks;
    uint8_t  log2     = ix->u.chunk_zone.chunk_log2;
    int64_t  csz      = 1LL << log2;
    int64_t  n        = v->len;
    double*  mins     = (double*)ray_data(ix->u.chunk_zone.mins);
    double*  maxs     = (double*)ray_data(ix->u.chunk_zone.maxs);
    uint8_t* nbits    = (uint8_t*)ray_data(ix->u.chunk_zone.null_bits);
    const uint8_t* base = (const uint8_t*)ray_data(v);

    for (uint32_t g = 0; g < n_chunks; g++) {
        int64_t s = (int64_t)g * csz;
        int64_t e = s + csz; if (e > n) e = n;
        double mn = INFINITY, mx = -INFINITY;
        bool any_null = false;
        for (int64_t i = s; i < e; i++) {
            if (ray_vec_is_null(v, i)) { any_null = true; continue; }
            double val = 0.0;
            if (elem_size == 4) {
                float t; memcpy(&t, base + i*4, 4); val = (double)t;
            } else {
                memcpy(&val, base + i*8, 8);
            }
            if (isnan(val)) { any_null = true; continue; }
            if (val < mn) mn = val;
            if (val > mx) mx = val;
        }
        /* Empty (all-null) chunks keep mn=+inf / mx=-inf so reduce
         * (min/max across mins[]/maxs[]) ignores them. */
        mins[g] = mn;
        maxs[g] = mx;
        if (any_null) nbits[g >> 3] |= (uint8_t)(1u << (g & 7));
    }
    return RAY_OK;
}

static ray_err_t chunk_zone_scan(ray_t* v, ray_index_t* ix) {
    switch (v->type) {
    case RAY_BOOL:
    case RAY_U8:        return chunk_zone_scan_int(v, ix, 1);
    case RAY_I16:       return chunk_zone_scan_int(v, ix, 2);
    case RAY_I32:
    case RAY_DATE:
    case RAY_TIME:      return chunk_zone_scan_int(v, ix, 4);  /* TIME is 4-byte int32 */
    case RAY_I64:
    case RAY_TIMESTAMP: return chunk_zone_scan_int(v, ix, 8);
    case RAY_F32:       return chunk_zone_scan_float(v, ix, 4);
    case RAY_F64:       return chunk_zone_scan_float(v, ix, 8);
    default:            return RAY_ERR_NYI;
    }
}

/* --------------------------------------------------------------------------
 * Attach
 *
 * The 16-byte snapshot preserves the parent's aux-union bytes across
 * the attachment so detach can restore them byte-for-byte.  For numeric
 * vectors (the only types that may attach) bytes 0-7 are unused and
 * bytes 8-15 carry link_target when HAS_LINK is set — no owned pointers
 * either way.  We do NOT retain anything here; the index pointer install
 * at bytes 0-7 transfers a single ref to the parent (no extra retain).
 * -------------------------------------------------------------------------- */

static ray_t* attach_finalize(ray_t* parent, ray_t* idx) {
    ray_index_t* ix = ray_index_payload(idx);
    /* Snapshot the parent's 16 raw bytes verbatim. */
    memcpy(ix->saved_aux, parent->aux, 16);
    ix->saved_attrs = parent->attrs & RAY_ATTR_HAS_NULLS;

    /* Install the index pointer — overwrites bytes 0-7 with the index ptr.
     * Bytes 8-15 carry link_target when HAS_LINK is set; preserve them.
     * Otherwise zero _idx_pad as a tidy default.
     *
     * IMPORTANT: HAS_NULLS is *preserved* on the parent so the many call
     * sites that use it as a cheap "do I need null logic at all?" gate
     * continue to give correct answers.  The actual null state is read
     * via ray_vec_is_null (sentinel-based), which is unaffected by the
     * index pointer overlay at bytes 0-7. */
    parent->index    = idx;
    if (!(parent->attrs & RAY_ATTR_HAS_LINK)) parent->_idx_pad = NULL;
    parent->attrs   |= RAY_ATTR_HAS_INDEX;
    return parent;
}

/* Validate + COW + drop existing index.  Returns the (possibly new) parent
 * pointer and updates *vp.  On error returns a RAY_ERROR; caller must
 * propagate without further modifying *vp. */
static ray_t* prepare_attach(ray_t** vp, const char* what) {
    if (!vp || !*vp || RAY_IS_ERR(*vp))
        return ray_error("type", "%s: null/error vector", what);
    ray_t* v = *vp;
    if (!ray_is_vec(v))
        return ray_error("type", "%s: index can only attach to a vector", what);
    if (v->attrs & RAY_ATTR_SLICE)
        return ray_error("type", "%s: cannot index a slice; materialize first", what);
    if (v->attrs & RAY_ATTR_HAS_INDEX) {
        ray_index_drop(&v);
        if (RAY_IS_ERR(v)) return v;
        *vp = v;
    }
    v = ray_cow(v);
    if (!v || RAY_IS_ERR(v)) return v;
    *vp = v;
    if (numeric_elem_size(v->type) == 0) {
        return ray_error("nyi", "%s: only numeric vectors supported in v1 (got type %d)",
                         what, (int)v->type);
    }
    return v;
}

ray_t* ray_index_attach_zone(ray_t** vp) {
    ray_t* v = prepare_attach(vp, "zone");
    if (RAY_IS_ERR(v)) return v;

    ray_t* idx = ray_index_alloc(RAY_IDX_ZONE, v->type, v->len);
    if (!idx || RAY_IS_ERR(idx)) return idx;

    ray_err_t err = zone_scan(v, ray_index_payload(idx));
    if (err != RAY_OK) {
        ray_release(idx);
        return ray_error(ray_err_code_str(err), "zone scan failed for type %d", (int)v->type);
    }
    return attach_finalize(v, idx);
}

ray_t* ray_index_attach_chunk_zone(ray_t** vp, uint8_t chunk_log2) {
    ray_t* v = prepare_attach(vp, "chunk_zone");
    if (RAY_IS_ERR(v)) return v;

    if (chunk_log2 == 0) chunk_log2 = 16;          /* default 64 K rows / chunk */
    if (chunk_log2 < 8 || chunk_log2 > 22)
        return ray_error("domain", "chunk_zone: chunk_log2 out of range [8, 22]");
    int64_t csz = 1LL << chunk_log2;
    /* No point indexing a column smaller than one chunk — fall back to
     * the column-wide zone (or no index at all) at that size. */
    if (v->len < csz)
        return ray_error("domain", "chunk_zone: column has fewer rows than one chunk");

    uint32_t n_chunks = (uint32_t)((v->len + csz - 1) / csz);

    ray_t* idx = ray_index_alloc(RAY_IDX_CHUNK_ZONE, v->type, v->len);
    if (!idx || RAY_IS_ERR(idx)) return idx;
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.chunk_zone.n_chunks   = n_chunks;
    ix->u.chunk_zone.chunk_log2 = chunk_log2;
    ix->u.chunk_zone.is_f64     = (v->type == RAY_F64 || v->type == RAY_F32) ? 1 : 0;

    int8_t arr_type = ix->u.chunk_zone.is_f64 ? RAY_F64 : RAY_I64;
    ray_t* mins = ray_vec_new(arr_type, (int64_t)n_chunks);
    ray_t* maxs = ray_vec_new(arr_type, (int64_t)n_chunks);
    int64_t nb_len = (int64_t)((n_chunks + 7) / 8);
    ray_t* nbits = ray_vec_new(RAY_U8, nb_len);
    if (!mins || RAY_IS_ERR(mins) || !maxs || RAY_IS_ERR(maxs) ||
        !nbits || RAY_IS_ERR(nbits))
    {
        if (mins && !RAY_IS_ERR(mins)) ray_release(mins);
        if (maxs && !RAY_IS_ERR(maxs)) ray_release(maxs);
        if (nbits && !RAY_IS_ERR(nbits)) ray_release(nbits);
        ray_release(idx);
        return ray_error("oom", "chunk_zone: arrays alloc");
    }
    mins->len  = (int64_t)n_chunks;
    maxs->len  = (int64_t)n_chunks;
    nbits->len = nb_len;
    memset(ray_data(nbits), 0, (size_t)nb_len);
    ix->u.chunk_zone.mins      = mins;
    ix->u.chunk_zone.maxs      = maxs;
    ix->u.chunk_zone.null_bits = nbits;

    ray_err_t err = chunk_zone_scan(v, ix);
    if (err != RAY_OK) {
        ray_release(idx);   /* releases mins/maxs/nbits via release_payload */
        return ray_error(ray_err_code_str(err),
                         "chunk_zone scan failed for type %d", (int)v->type);
    }
    return attach_finalize(v, idx);
}

/* --------------------------------------------------------------------------
 * Hash index — chained open addressing
 *
 * table[capacity]: each slot is rid+1 of the most recent row that hashed
 *   into the bucket (0 = empty bucket).
 * chain[parent->len]: each slot is rid+1 of the next-older row in the same
 *   bucket's chain (0 = end of chain).
 *
 * Lookup `k`: rid = table[hash(k) & mask] - 1; while rid >= 0 compare
 * parent->data[rid] == k, on miss step rid = chain[rid] - 1.
 * -------------------------------------------------------------------------- */

ray_t* ray_index_attach_hash(ray_t** vp) {
    ray_t* v = prepare_attach(vp, "hash");
    if (RAY_IS_ERR(v)) return v;

    int64_t n = v->len;
    /* Capacity: at least 8, at most 2*n.  Power of two for cheap masking. */
    uint64_t cap = next_pow2((uint64_t)(n < 4 ? 8 : 2 * n));
    if (cap < 8) cap = 8;

    ray_t* table = ray_vec_new(RAY_I64, (int64_t)cap);
    if (!table || RAY_IS_ERR(table)) return table ? table : ray_error("oom", NULL);
    table->len = (int64_t)cap;
    memset(ray_data(table), 0, (size_t)cap * sizeof(int64_t));

    ray_t* chain = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (!chain || RAY_IS_ERR(chain)) {
        ray_release(table);
        return chain ? chain : ray_error("oom", NULL);
    }
    chain->len = n;
    if (n > 0) memset(ray_data(chain), 0, (size_t)n * sizeof(int64_t));

    int64_t* tbl = (int64_t*)ray_data(table);
    int64_t* chn = (int64_t*)ray_data(chain);
    const uint8_t* base = (const uint8_t*)ray_data(v);
    int64_t n_keys = 0;
    uint64_t mask = cap - 1;

    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(v, i)) continue;
        uint64_t h = mix64(numeric_key_word(base, v->type, i));
        uint64_t slot = h & mask;
        chn[i] = tbl[slot];     /* link previous head into chain */
        tbl[slot] = i + 1;      /* this row becomes new head */
        n_keys++;
    }

    ray_t* idx = ray_index_alloc(RAY_IDX_HASH, v->type, n);
    if (!idx || RAY_IS_ERR(idx)) {
        ray_release(table);
        ray_release(chain);
        return idx ? idx : ray_error("oom", NULL);
    }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.hash.table  = table;
    ix->u.hash.chain  = chain;
    ix->u.hash.mask   = mask;
    ix->u.hash.n_keys = n_keys;

    return attach_finalize(v, idx);
}

/* --------------------------------------------------------------------------
 * Hash-index point-lookup probe — public entry point for the eq-filter
 * fast path (ray_index_hash_eq_rowsel).
 *
 * Callers present the index with an int64 key; we mix64 it with the
 * same hash the builder used, walk the bucket chain, collect matches,
 * and emit a ray_rowsel sized for O(matches) memory (no intermediate
 * row-wide BOOL pred vec).
 *
 * Type matrix.  An index built on column type T accepts a key only
 * when T's storage width covers it without truncation — i.e. asking
 * for `u8_col == 300` would never match, so we fail eligibility and
 * the caller falls back to the scan (which folds out-of-range via
 * fp_fold_t).  Float keys are not supported here — equality on
 * F32/F64 has NaN / -0 semantics the unfused engine handles. */

static int hash_key_in_range(int8_t t, int64_t k) {
    switch (t) {
    case RAY_BOOL: case RAY_U8:        return k >= 0 && k <= UINT8_MAX;
    case RAY_I16:                      return k >= INT16_MIN && k <= INT16_MAX;
    case RAY_I32: case RAY_DATE:
    case RAY_TIME:                     return k >= INT32_MIN && k <= INT32_MAX;
    case RAY_I64:
    case RAY_TIMESTAMP:                return 1;
    default:                           return 0;
    }
}

/* Read row `i` of a numeric column as int64 for equality compare. */
static int64_t hash_col_read_i64(const uint8_t* base, int8_t t, int64_t i) {
    int es;
    switch (t) {
    case RAY_BOOL: case RAY_U8:        es = 1; break;
    case RAY_I16:                      es = 2; break;
    case RAY_I32: case RAY_DATE:
    case RAY_TIME:                     es = 4; break;  /* TIME is 4-byte int32 */
    case RAY_I64:
    case RAY_TIMESTAMP:                es = 8; break;
    default:                           return 0;
    }
    switch (es) {
    case 1:  return (int64_t)base[i];
    case 2:  { int16_t v; memcpy(&v, base + i*2, 2); return (int64_t)v; }
    case 4:  { int32_t v; memcpy(&v, base + i*4, 4); return (int64_t)v; }
    default: { int64_t v; memcpy(&v, base + i*8, 8); return v;          }
    }
}

/* Mirror numeric_key_word for an int64 key probed against a column of
 * element size `es`: the canonical hash input is the raw bit pattern of
 * the storage width.  We zero-extend U8/BOOL and sign-extend others up
 * to int64; mix64 then folds them — the builder did the same per row. */
static uint64_t hash_key_bits(int es, int64_t key) {
    switch (es) {
    case 1:  return (uint64_t)(uint8_t)key;
    case 2:  return (uint64_t)(int64_t)(int16_t)key;
    case 4:  return (uint64_t)(int64_t)(int32_t)key;
    default: return (uint64_t)key;
    }
}

/* Validate eligibility, return the index payload + computed start row.
 * On miss leaves *start = -1 so the caller can short-circuit. */
static ray_index_t* hash_probe_setup(ray_t* col, int64_t key,
                                     int64_t* start_rid) {
    *start_rid = -1;
    if (!col || RAY_IS_ERR(col) || !ray_is_vec(col)) return NULL;
    if (!(col->attrs & RAY_ATTR_HAS_INDEX) || !col->index) return NULL;
    ray_index_t* ix = ray_index_payload(col->index);
    if (ix->kind != RAY_IDX_HASH) return NULL;
    if (ix->built_for_len != col->len) return NULL;
    if (!hash_key_in_range(col->type, key)) return NULL;
    if (numeric_elem_size(col->type) == 0) return NULL;
    if (!ix->u.hash.table || !ix->u.hash.chain) return NULL;

    uint64_t h = mix64(hash_key_bits(numeric_elem_size(col->type), key));
    uint64_t slot = h & ix->u.hash.mask;
    const int64_t* tbl = (const int64_t*)ray_data(ix->u.hash.table);
    *start_rid = tbl[slot] - 1;
    return ix;
}

/* v1 routing eligibility — structural gate shared by every consult function.
 * Checks that col is a flat (non-parted, non-MAPCOMMON) vector carrying a
 * fresh index of the requested kind.  Does NOT gate on HAS_NULLS: hash and
 * bloom indexes skip null rows during build, so probing a null-bearing column
 * is correct (the chain simply won't surface null-row matches).  Callers that
 * must exclude null-bearing columns for semantic reasons (sort perm, range
 * scan) should add their own HAS_NULLS check. */
static bool idx_fresh(ray_t* col, ray_idx_kind_t kind) {
    if (!col || RAY_IS_ERR(col)) return false;
    if (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON) return false;
    if (!ray_index_has(col)) return false;
    ray_index_t* ix = ray_index_payload(col->index);
    return ix->kind == (uint8_t)kind && ix->built_for_len == col->len;
}

/* v1 routing gate for the NEW consult paths: like idx_fresh but also
 * excludes null-bearing columns.  Sort perm orders nulls LAST while
 * sentinel values order FIRST, and comparison semantics on null rows
 * follow null-as-minimum — none of the new consults model that in v1.
 * (The pre-existing hash-eq probe keeps bare idx_fresh: its builder
 * skips null rows, making null-bearing probes structurally correct.) */
static bool idx_fresh_nonull(ray_t* col, ray_idx_kind_t kind) {
    return idx_fresh(col, kind) && !(col->attrs & RAY_ATTR_HAS_NULLS);
}

/* --------------------------------------------------------------------------
 * Zone-index all/none classification
 * -------------------------------------------------------------------------- */

/* Classify (col cmp_op key) using the zone index min/max for O(1) decision.
 * Integer family (BOOL/U8/I16/I32/I64/DATE/TIME/TIMESTAMP) uses key_i and
 * the zone's min_i/max_i; float family (F32/F64) uses key_f and min_f/max_f.
 * NaN key_f → UNKNOWN immediately (NaN comparison is the null-aware kernel's
 * business).  Returns UNKNOWN when not eligible (no zone, stale, null-bearing,
 * or unsupported type). */
ray_zone_class_t ray_index_zone_class(ray_t* col, uint16_t cmp_op,
                                      int64_t key_i, double key_f) {
    if (!idx_fresh_nonull(col, RAY_IDX_ZONE)) return RAY_ZONE_UNKNOWN;
    ray_index_t* ix = ray_index_payload(col->index);

    /* Dispatch to integer or float path based on column type. */
    bool is_float = (col->type == RAY_F32 || col->type == RAY_F64);
    bool is_int   = !is_float && (numeric_elem_size(col->type) > 0);
    if (!is_float && !is_int) return RAY_ZONE_UNKNOWN;

    if (is_float && isnan(key_f)) return RAY_ZONE_UNKNOWN;

#define ZONE_CLASSIFY(mn, mx, key) do {                                \
    switch (cmp_op) {                                                  \
    case OP_EQ:                                                        \
        if ((key) < (mn) || (key) > (mx)) return RAY_ZONE_NONE;       \
        if ((mn) == (mx) && (mn) == (key)) return RAY_ZONE_ALL;       \
        break;                                                         \
    case OP_NE:                                                        \
        if ((mn) == (mx) && (mn) == (key)) return RAY_ZONE_NONE;      \
        if ((key) < (mn) || (key) > (mx)) return RAY_ZONE_ALL;        \
        break;                                                         \
    case OP_LT:                                                        \
        if ((mx) <  (key)) return RAY_ZONE_ALL;                        \
        if ((mn) >= (key)) return RAY_ZONE_NONE;                       \
        break;                                                         \
    case OP_LE:                                                        \
        if ((mx) <= (key)) return RAY_ZONE_ALL;                        \
        if ((mn) >  (key)) return RAY_ZONE_NONE;                       \
        break;                                                         \
    case OP_GT:                                                        \
        if ((mn) >  (key)) return RAY_ZONE_ALL;                        \
        if ((mx) <= (key)) return RAY_ZONE_NONE;                       \
        break;                                                         \
    case OP_GE:                                                        \
        if ((mn) >= (key)) return RAY_ZONE_ALL;                        \
        if ((mx) <  (key)) return RAY_ZONE_NONE;                       \
        break;                                                         \
    }                                                                  \
} while (0)

    if (is_int)   { ZONE_CLASSIFY(ix->u.zone.min_i, ix->u.zone.max_i, key_i); }
    else          { ZONE_CLASSIFY(ix->u.zone.min_f, ix->u.zone.max_f, key_f); }

#undef ZONE_CLASSIFY

    return RAY_ZONE_UNKNOWN;
}

/* qsort comparator: ascending int64 row ids, used by the rowsel
 * builder to put matches into per-segment order. */
static int hash_match_cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a;
    int64_t y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

/* Build a rowsel block from a SORTED ascending array of matching row ids
 * over a column of n rows.  Returns fresh rowsel (rc=1) or NULL on OOM.
 * mcnt==0 yields a valid all-NONE rowsel, NOT NULL — NULL means "no fast
 * path" to every consumer (idxop.h contract). */
static ray_t* rowsel_from_sorted_ids(int64_t n, const int64_t* ids, int64_t mcnt) {
    ray_t* block = ray_rowsel_new(n, mcnt, mcnt);
    if (!block) return NULL;

    uint32_t n_segs = ray_rowsel_meta(block)->n_segs;
    uint8_t*  seg_flags   = ray_rowsel_flags(block);
    uint32_t* seg_offsets = ray_rowsel_offsets(block);
    uint16_t* idx_arr     = ray_rowsel_idx(block);

    /* All segments default to NONE; the loop below flips MIX where
     * a match lands.  ray_alloc does NOT zero the data area
     * (only the 32-byte header), so explicit init is required. */
    memset(seg_flags, RAY_SEL_NONE, (size_t)n_segs);
    /* seg_offsets is built by linear sweep below — initialize to a
     * sentinel that the sweep will overwrite. */
    /* (no memset needed; the sweep writes every entry [0..n_segs]) */

    /* Single sweep over the sorted matches: emit per-segment offsets
     * and morsel-local indices into idx_arr.  cur_seg tracks the
     * segment we're filling; gaps get RAY_SEL_NONE and zero spans. */
    int64_t mi = 0;
    uint32_t cum = 0;
    for (uint32_t s = 0; s < n_segs; s++) {
        seg_offsets[s] = cum;
        int64_t seg_start = (int64_t)s * RAY_MORSEL_ELEMS;
        int64_t seg_end   = seg_start + RAY_MORSEL_ELEMS;
        if (seg_end > n) seg_end = n;
        uint32_t pc = 0;
        while (mi < mcnt && ids[mi] < seg_end) {
            idx_arr[cum + pc] = (uint16_t)(ids[mi] - seg_start);
            pc++;
            mi++;
        }
        if (pc == 0) {
            seg_flags[s] = RAY_SEL_NONE;
        } else if ((int64_t)pc == seg_end - seg_start) {
            seg_flags[s] = RAY_SEL_ALL;
            /* Roll back the indices — ALL segments contribute zero
             * idx[] entries in the rowsel contract. */
            cum -= pc;  /* idx_arr writes for this seg get overwritten
                          by the next MIX segment's writes; idx_count
                          was sized for all matches, so this is safe. */
        } else {
            seg_flags[s] = RAY_SEL_MIX;
            cum += pc;
        }
    }
    seg_offsets[n_segs] = cum;
    /* Adjust meta total_pass / idx layout — ALL-segment rows count
     * toward total_pass but not idx_count.  We initially passed
     * (mcnt, mcnt); fix up if any ALL segments collapsed. */
    ray_rowsel_meta(block)->total_pass = mcnt;
    (void)cum;

    return block;
}

/* Public wrapper: build an all-NONE rowsel for n rows.  Returns NULL on OOM. */
ray_t* ray_index_empty_rowsel(int64_t n) {
    return rowsel_from_sorted_ids(n, NULL, 0);
}

ray_t* ray_index_hash_eq_rowsel(ray_t* col, int64_t key) {
    /* Sanity precheck — idx_fresh validates parted/nulls/kind/staleness;
     * hash_probe_setup below also validates key-range, elem-size, and
     * payload pointers, so these checks are complementary. */
    if (!idx_fresh(col, RAY_IDX_HASH)) return NULL;
    int64_t rid = -1;
    ray_index_t* ix = hash_probe_setup(col, key, &rid);
    if (!ix) return NULL;

    int64_t n = col->len;
    /* Collect matching row ids.  The chain length is bounded by the
     * bucket fill factor; for keys appearing rarely the bound is tight
     * (~1 row).  For highly-duplicated keys it can degenerate to O(n)
     * — but only if the value really occurs that many times, in which
     * case the existing scan path also reads the same number of rows.
     * We size the collect buffer dynamically; cap at n to bound memory
     * in the pathological case. */
    const int64_t* chn  = (const int64_t*)ray_data(ix->u.hash.chain);
    const uint8_t* base = (const uint8_t*)ray_data(col);
    int8_t t = col->type;

    int64_t mcap = 16;
    int64_t mcnt = 0;
    ray_t* match_hdr = ray_alloc(mcap * (int64_t)sizeof(int64_t));
    if (!match_hdr) return NULL;
    int64_t* matches = (int64_t*)ray_data(match_hdr);

    while (rid >= 0) {
        if (hash_col_read_i64(base, t, rid) == key) {
            if (mcnt == mcap) {
                int64_t new_cap = mcap * 2;
                if (new_cap > n) new_cap = n + 1;  /* defensive bound */
                ray_t* new_hdr = ray_alloc(new_cap * (int64_t)sizeof(int64_t));
                if (!new_hdr) { ray_release(match_hdr); return NULL; }
                memcpy(ray_data(new_hdr), matches,
                       (size_t)mcnt * sizeof(int64_t));
                ray_release(match_hdr);
                match_hdr = new_hdr;
                matches = (int64_t*)ray_data(match_hdr);
                mcap = new_cap;
            }
            matches[mcnt++] = rid;
        }
        rid = chn[rid] - 1;
    }

    /* Sort ascending so we can fill seg_flags / seg_offsets / idx[]
     * in a single linear pass.  qsort dominates only when matches are
     * many — in that case the hash probe itself is the larger cost
     * and this is still O(matches log matches). */
    if (mcnt > 1)
        qsort(matches, (size_t)mcnt, sizeof(int64_t), hash_match_cmp_i64);

    ray_t* block = rowsel_from_sorted_ids(n, matches, mcnt);
    ray_release(match_hdr);
    return block;
}

/* --------------------------------------------------------------------------
 * Hash-index IN probe
 *
 * For each unique in-range element of set_vec, walk the hash chain and
 * collect matching row ids into a single shared buffer, then build a
 * rowsel in one pass.
 * -------------------------------------------------------------------------- */

/* Read element i of a set vec (integer-family only) as int64.  Mirrors
 * hash_col_read_i64 but operates on the set_vec type, not the column type. */
static int64_t set_vec_read_i64(const uint8_t* base, int8_t t, int64_t i) {
    switch (t) {
    case RAY_BOOL: case RAY_U8:        return (int64_t)base[i];
    case RAY_I16:  { int16_t v; memcpy(&v, base + i*2, 2); return (int64_t)v; }
    case RAY_I32: case RAY_DATE: case RAY_TIME:
                   { int32_t v; memcpy(&v, base + i*4, 4); return (int64_t)v; }
    case RAY_I64: case RAY_TIMESTAMP:
                   { int64_t v; memcpy(&v, base + i*8, 8); return v; }
    default:       return 0;
    }
}

static int cmp_i64_plain(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a;
    int64_t y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

ray_t* ray_index_in_rowsel(ray_t* col, ray_t* set_vec) {
    /* Gate: integer-family column with fresh hash index, no nulls. */
    if (!idx_fresh_nonull(col, RAY_IDX_HASH)) return NULL;
    bool col_is_float = (col->type == RAY_F32 || col->type == RAY_F64);
    if (col_is_float) return NULL;

    /* set_vec must be a non-atom integer-family vec. */
    if (!set_vec || RAY_IS_ERR(set_vec) || ray_is_atom(set_vec)) return NULL;
    int8_t st = set_vec->type;
    bool set_is_float = (st == RAY_F32 || st == RAY_F64);
    if (set_is_float) return NULL;
    /* Check set type is integer-family (numeric_elem_size covers all int types) */
    if (numeric_elem_size(st) == 0) return NULL;

    int64_t set_len = set_vec->len;
    int64_t n       = col->len;

    /* Canonicalize set: copy to int64 scratch, sort, unique, drop out-of-range. */
    int64_t* set_scratch = NULL;
    ray_t*   set_hdr     = NULL;
    if (set_len > 0) {
        set_hdr = ray_alloc(set_len * (int64_t)sizeof(int64_t));
        if (!set_hdr) return NULL;
        set_scratch = (int64_t*)ray_data(set_hdr);
        const uint8_t* sb = (const uint8_t*)ray_data(set_vec);
        int64_t ulen = 0;
        for (int64_t i = 0; i < set_len; i++) {
            int64_t v = set_vec_read_i64(sb, st, i);
            if (hash_key_in_range(col->type, v))
                set_scratch[ulen++] = v;
        }
        if (ulen == 0) {
            /* All elements out of range — no possible matches. */
            ray_release(set_hdr);
            return rowsel_from_sorted_ids(n, NULL, 0);
        }
        /* Sort and deduplicate. */
        qsort(set_scratch, (size_t)ulen, sizeof(int64_t), cmp_i64_plain);
        int64_t wlen = 1;
        for (int64_t i = 1; i < ulen; i++)
            if (set_scratch[i] != set_scratch[i-1])
                set_scratch[wlen++] = set_scratch[i];
        set_len = wlen;
    } else {
        /* Empty set → all-NONE rowsel. */
        return rowsel_from_sorted_ids(n, NULL, 0);
    }

    /* Fetch the hash payload ONCE; per-key probing below only needs the
     * bucket-head lookup (every key already passed hash_key_in_range, and
     * the column-level gates were validated by idx_fresh_nonull above). */
    ray_index_t* ix = ray_index_payload(col->index);
    if (!ix->u.hash.table || !ix->u.hash.chain) {
        ray_release(set_hdr);
        return NULL;
    }
    const int64_t* tbl  = (const int64_t*)ray_data(ix->u.hash.table);
    const int64_t* chn  = (const int64_t*)ray_data(ix->u.hash.chain);
    const uint8_t* base = (const uint8_t*)ray_data(col);
    uint64_t mask = ix->u.hash.mask;
    int8_t t  = col->type;
    int    es = numeric_elem_size(t);

    /* Shared match buffer: starts at 16, grows by doubling, capped at n. */
    int64_t mcap = 16;
    int64_t mcnt = 0;
    ray_t*   match_hdr  = ray_alloc(mcap * (int64_t)sizeof(int64_t));
    if (!match_hdr) { ray_release(set_hdr); return NULL; }
    int64_t* matches = (int64_t*)ray_data(match_hdr);

    /* Selectivity guard threshold: if total collected > n/4, abandon
     * (rowsel build + sort overhead approaches scan cost at that point).
     * For small tables (n <= 64) the guard never fires — the overhead is
     * trivial regardless of selectivity. */
    int64_t guard = (n > 64) ? n / 4 : n;

    for (int64_t si = 0; si < set_len; si++) {
        int64_t key = set_scratch[si];
        /* Bucket head for this key — same kbits/mix64 the builder used. */
        int64_t rid = tbl[mix64(hash_key_bits(es, key)) & mask] - 1;

        while (rid >= 0) {
            if (hash_col_read_i64(base, t, rid) == key) {
                /* Grow match buffer if needed. */
                if (mcnt == mcap) {
                    int64_t new_cap = mcap * 2;
                    if (new_cap > n) new_cap = n + 1;
                    ray_t* new_hdr = ray_alloc(new_cap * (int64_t)sizeof(int64_t));
                    if (!new_hdr) {
                        ray_release(match_hdr);
                        ray_release(set_hdr);
                        return NULL;
                    }
                    memcpy(ray_data(new_hdr), matches,
                           (size_t)mcnt * sizeof(int64_t));
                    ray_release(match_hdr);
                    match_hdr = new_hdr;
                    matches   = (int64_t*)ray_data(match_hdr);
                    mcap      = new_cap;
                }
                matches[mcnt++] = rid;
                /* Selectivity guard: abandon if too many matches. */
                if (mcnt > guard) {
                    ray_release(match_hdr);
                    ray_release(set_hdr);
                    return NULL;
                }
            }
            rid = chn[rid] - 1;
        }
    }

    ray_release(set_hdr);

    /* Sort collected ids (distinct keys → no duplicate row ids across different
     * probes; a row holds exactly one value so different keys can't hit the
     * same row). */
    if (mcnt > 1)
        qsort(matches, (size_t)mcnt, sizeof(int64_t), hash_match_cmp_i64);

    ray_t* block = rowsel_from_sorted_ids(n, matches, mcnt);
    ray_release(match_hdr);
    return block;
}

/* --------------------------------------------------------------------------
 * Sort-index range probe
 *
 * Binary search over the sort permutation.  Two typed helpers — one for
 * integer-family columns, one for float-family — avoid branching inside the
 * hot search loop.
 *
 * Guard: O(m log m) row-id sort + rowsel build must stay under O(n).  At
 * IDX_RANGE_MAX_FRAC == 4 the break-even is roughly 25% selectivity.
 * -------------------------------------------------------------------------- */

/* Bail when the qualifying span exceeds len/IDX_RANGE_MAX_FRAC — the
 * O(m log m) row-id sort below must stay under the scan's O(n) cost.
 * 4 (25%) is the initial setting; the perf gate tunes it. */
#define IDX_RANGE_MAX_FRAC 4

/* Read row rid of an integer-family column as int64. */
static int64_t sort_read_i64(const uint8_t* base, int8_t t, int64_t rid) {
    return hash_col_read_i64(base, t, rid);
}

/* Read row rid of a float-family column as double. */
static double sort_read_f64(const uint8_t* base, int8_t t, int64_t rid) {
    if (t == RAY_F32) {
        float v; memcpy(&v, base + rid * 4, 4); return (double)v;
    }
    double v; memcpy(&v, base + rid * 8, 8); return v;
}

/* lower_bound_i: first sorted position pos where value_at(perm[pos]) >= key.
 * Positions in [0, n) are searched; perm is the sort permutation. */
static int64_t sort_lower_i(const int64_t* perm, int64_t n,
                            const uint8_t* base, int8_t t, int64_t key) {
    int64_t lo = 0, hi = n;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (sort_read_i64(base, t, perm[mid]) < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* upper_bound_i: first sorted position pos where value_at(perm[pos]) > key. */
static int64_t sort_upper_i(const int64_t* perm, int64_t n,
                            const uint8_t* base, int8_t t, int64_t key) {
    int64_t lo = 0, hi = n;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (sort_read_i64(base, t, perm[mid]) <= key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* lower_bound_f: first position where value >= key (NaN-safe: NaN is > any). */
static int64_t sort_lower_f(const int64_t* perm, int64_t n,
                            const uint8_t* base, int8_t t, double key) {
    int64_t lo = 0, hi = n;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        double v = sort_read_f64(base, t, perm[mid]);
        if (!isnan(v) && v < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* upper_bound_f: first position where value > key. */
static int64_t sort_upper_f(const int64_t* perm, int64_t n,
                            const uint8_t* base, int8_t t, double key) {
    int64_t lo = 0, hi = n;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        double v = sort_read_f64(base, t, perm[mid]);
        if (!isnan(v) && v <= key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

ray_t* ray_index_range_rowsel(ray_t* col, uint16_t cmp_op,
                              int64_t key_i, double key_f, bool is_float) {
    /* NE = two spans — unsupported. */
    if (cmp_op == OP_NE) return NULL;

    /* Freshness + no-null gate. */
    if (!idx_fresh_nonull(col, RAY_IDX_SORT)) return NULL;

    /* Consistency check: is_float must agree with the column's type family. */
    bool col_is_float = (col->type == RAY_F32 || col->type == RAY_F64);
    if ((bool)is_float != col_is_float) return NULL;

    ray_index_t* ix = ray_index_payload(col->index);
    if (!ix->u.sort.perm) return NULL;

    int64_t n = col->len;
    const int64_t* perm = (const int64_t*)ray_data(ix->u.sort.perm);
    const uint8_t* base = (const uint8_t*)ray_data(col);
    int8_t t = col->type;

    /* Compute [lo, hi) span in sorted order. */
    int64_t lo, hi;
    if (!is_float) {
        int64_t lower = sort_lower_i(perm, n, base, t, key_i);
        int64_t upper = sort_upper_i(perm, n, base, t, key_i);
        switch (cmp_op) {
        case OP_LT: lo = 0;     hi = lower; break;
        case OP_LE: lo = 0;     hi = upper; break;
        case OP_GT: lo = upper; hi = n;     break;
        case OP_GE: lo = lower; hi = n;     break;
        case OP_EQ: lo = lower; hi = upper; break;
        default:    return NULL;
        }
    } else {
        int64_t lower = sort_lower_f(perm, n, base, t, key_f);
        int64_t upper = sort_upper_f(perm, n, base, t, key_f);
        switch (cmp_op) {
        case OP_LT: lo = 0;     hi = lower; break;
        case OP_LE: lo = 0;     hi = upper; break;
        case OP_GT: lo = upper; hi = n;     break;
        case OP_GE: lo = lower; hi = n;     break;
        case OP_EQ: lo = lower; hi = upper; break;
        default:    return NULL;
        }
    }

    int64_t span = hi - lo;
    if (span <= 0) return ray_index_empty_rowsel(n);
    /* Selectivity guard: only worth paying the O(m log m) sort when the
     * span is small relative to the column AND the column is large enough
     * that a scan would be expensive.  Below 64 rows the scan is trivial
     * regardless of selectivity, so skip the guard. */
    if (n >= 64 && span > n / IDX_RANGE_MAX_FRAC) return NULL;

    /* Copy perm[lo..hi) into a scratch buffer, sort ascending, build rowsel. */
    ray_t* scratch = ray_alloc(span * (int64_t)sizeof(int64_t));
    if (!scratch) return NULL;
    int64_t* ids = (int64_t*)ray_data(scratch);
    for (int64_t i = 0; i < span; i++) ids[i] = perm[lo + i];

    if (span > 1)
        qsort(ids, (size_t)span, sizeof(int64_t), hash_match_cmp_i64);

    ray_t* block = rowsel_from_sorted_ids(n, ids, span);
    ray_release(scratch);
    return block;
}

/* --------------------------------------------------------------------------
 * Bloom-index definite-absent probe
 *
 * Builder formula (ray_index_attach_bloom above):
 *   h  = mix64(numeric_key_word(base, type, row))
 *   h1 = h
 *   h2 = mix64(h ^ 0xc6a4a7935bd1e995ULL) | 1ULL   -- ensure odd (double-hashing)
 *   for kk in 0..k-1:
 *       pos = (h1 + kk * h2) & m_mask
 *       bits[pos >> 3] |= 1 << (pos & 7)
 *
 * For an integer key the raw bit representation used by numeric_key_word is:
 *   zero-extend  for U8/BOOL (1-byte storage)
 *   sign-extend  for I16 (2-byte storage)
 *   sign-extend  for I32/DATE/TIME (4-byte storage)
 *   identity     for I64/TIMESTAMP (8-byte storage)
 * which is exactly (uint64_t)(int64_t)key after the appropriate truncation.
 *
 * We derive kbits the same way hash_probe_setup does for the HASH index
 * (see hash_probe_setup, ~line 773), then run the same double-hash probe.
 * -------------------------------------------------------------------------- */

bool ray_index_bloom_absent(ray_t* col, int64_t key) {
    /* idx_fresh_nonull: freshness + kind + no-null gate. */
    if (!idx_fresh_nonull(col, RAY_IDX_BLOOM)) return false;

    /* Integer-family only in v1.  F32/F64 equality has NaN/-0 semantics
     * that the unfused kernel handles; skip bloom for float columns. */
    int8_t t = col->type;
    if (t == RAY_F32 || t == RAY_F64) return false;

    /* Derive kbits: mirror numeric_key_word for an integer scalar key.
     * numeric_key_word uses the raw bit pattern of the storage width; for
     * integer types that means zero-extension (U8/BOOL) or sign-extension
     * (I16/I32/I64 family) to uint64.  No range check needed: an out-of-
     * range key hashes to some bit positions; if all happen to be set we
     * fall through harmlessly — the absent proof is still sound. */
    int es = numeric_elem_size(t);
    uint64_t kbits;
    switch (es) {
    case 1:  kbits = (uint64_t)(uint8_t)key;                break;
    case 2:  kbits = (uint64_t)(int64_t)(int16_t)key;       break;
    case 4:  kbits = (uint64_t)(int64_t)(int32_t)key;       break;
    default: kbits = (uint64_t)key;                         break;
    }

    ray_index_t* ix    = ray_index_payload(col->index);
    uint64_t     mask  = ix->u.bloom.m_mask;
    uint32_t     k     = ix->u.bloom.k;
    const uint8_t* bbuf = (const uint8_t*)ray_data(ix->u.bloom.bits);

    /* Double-hashing probe — exact mirror of the builder loop. */
    uint64_t h  = mix64(kbits);
    uint64_t h1 = h;
    uint64_t h2 = mix64(h ^ 0xc6a4a7935bd1e995ULL) | 1ULL;

    for (uint32_t kk = 0; kk < k; kk++) {
        uint64_t pos = (h1 + (uint64_t)kk * h2) & mask;
        if (!(bbuf[pos >> 3] & (uint8_t)(1u << (pos & 7))))
            return true;   /* bit clear → key provably absent */
    }
    return false;  /* all bits set → maybe present, fall through */
}

/* --------------------------------------------------------------------------
 * Sort index — ascending permutation of row ids
 *
 * Delegates to the existing parallel sort builder.  Result is an I64 vec of
 * length parent->len with default null-handling (nulls last for asc).
 * -------------------------------------------------------------------------- */

ray_t* ray_index_attach_sort(ray_t** vp) {
    ray_t* v = prepare_attach(vp, "sort");
    if (RAY_IS_ERR(v)) return v;

    ray_t* col = v;
    ray_t* perm = ray_sort_indices(&col, NULL, NULL, 1, v->len);
    if (!perm || RAY_IS_ERR(perm)) return perm ? perm : ray_error("oom", NULL);

    ray_t* idx = ray_index_alloc(RAY_IDX_SORT, v->type, v->len);
    if (!idx || RAY_IS_ERR(idx)) {
        ray_release(perm);
        return idx ? idx : ray_error("oom", NULL);
    }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.sort.perm = perm;

    return attach_finalize(v, idx);
}

/* --------------------------------------------------------------------------
 * Bloom filter — m bits, k=3 hashes via double-hashing
 *
 * Layout: m is rounded to the next power of two >= max(64, 8*n_non_null).
 * Each row sets bits at positions (h1 + i*h2) mod m for i in [0..k).
 * h1, h2 are derived from a single 64-bit mix of the key word.
 * -------------------------------------------------------------------------- */

ray_t* ray_index_attach_bloom(ray_t** vp) {
    ray_t* v = prepare_attach(vp, "bloom");
    if (RAY_IS_ERR(v)) return v;

    int64_t n = v->len;
    /* Count non-null rows for sizing. */
    int64_t n_set = 0;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_vec_is_null(v, i)) n_set++;
    }
    uint64_t target_bits = (uint64_t)(n_set < 8 ? 64 : 8 * n_set);
    uint64_t m = next_pow2(target_bits);
    if (m < 64) m = 64;
    uint64_t mbytes = m / 8;
    uint32_t k = 3;

    ray_t* bits = ray_vec_new(RAY_U8, (int64_t)mbytes);
    if (!bits || RAY_IS_ERR(bits)) return bits ? bits : ray_error("oom", NULL);
    bits->len = (int64_t)mbytes;
    memset(ray_data(bits), 0, (size_t)mbytes);

    uint8_t* bbuf = (uint8_t*)ray_data(bits);
    uint64_t mask = m - 1;
    const uint8_t* base = (const uint8_t*)ray_data(v);

    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(v, i)) continue;
        uint64_t h = mix64(numeric_key_word(base, v->type, i));
        uint64_t h1 = h;
        uint64_t h2 = mix64(h ^ 0xc6a4a7935bd1e995ULL) | 1ULL;  /* ensure odd */
        for (uint32_t kk = 0; kk < k; kk++) {
            uint64_t pos = (h1 + (uint64_t)kk * h2) & mask;
            bbuf[pos >> 3] |= (uint8_t)(1u << (pos & 7));
        }
    }

    ray_t* idx = ray_index_alloc(RAY_IDX_BLOOM, v->type, n);
    if (!idx || RAY_IS_ERR(idx)) {
        ray_release(bits);
        return idx ? idx : ray_error("oom", NULL);
    }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.bloom.bits   = bits;
    ix->u.bloom.m_mask = mask;
    ix->u.bloom.k      = k;
    ix->u.bloom.n_keys = n_set;

    return attach_finalize(v, idx);
}

/* --------------------------------------------------------------------------
 * Part index — contiguous ascending value-blocks
 *
 * Build a RAY_IDX_PART index, verifying the column is laid out as contiguous,
 * ascending value-blocks.  Non-descending order is necessary AND sufficient:
 * it guarantees each distinct value occupies exactly one contiguous run.
 * v1: numeric vectors only (enforced by prepare_attach).
 * -------------------------------------------------------------------------- */

static ray_t* ray_index_attach_part(ray_t** vp) {
    ray_t* v = prepare_attach(vp, "part");
    if (RAY_IS_ERR(v)) return v;
    int64_t n = v->len;

    if (!vec_is_ascending(v))
        return ray_error("domain", "parted: column is not laid out as ascending value-blocks");

    const uint8_t* base = (const uint8_t*)ray_data(v);
    int es = numeric_elem_size(v->type);

    /* Row i starts a new value-block iff it differs from i-1.  vec_is_ascending
     * above already rejected any null/NaN, so numeric_key_word's NaN branch is
     * unreachable here and its equality is exact for every accepted type.  Used
     * by both the count and the fill loop so they cannot drift out of sync. */
    #define PART_NEW_BLOCK(i) \
        (numeric_key_word(base, v->type, (i)) != numeric_key_word(base, v->type, (i)-1))

    /* Count runs of equal values. */
    int64_t nparts = (n > 0) ? 1 : 0;
    for (int64_t i = 1; i < n; i++)
        if (PART_NEW_BLOCK(i))
            nparts++;

    int64_t cap = nparts > 0 ? nparts : 1;
    ray_t* starts = ray_vec_new(RAY_I64, cap);
    ray_t* lens   = ray_vec_new(RAY_I64, cap);
    ray_t* keys   = ray_vec_new(v->type, cap);
    if (RAY_IS_ERR(starts) || RAY_IS_ERR(lens) || RAY_IS_ERR(keys)) {
        if (!RAY_IS_ERR(starts)) ray_release(starts);
        if (!RAY_IS_ERR(lens))   ray_release(lens);
        if (!RAY_IS_ERR(keys))   ray_release(keys);
        return ray_error("oom", NULL);
    }
    starts->len = lens->len = keys->len = nparts;
    int64_t* st = (int64_t*)ray_data(starts);
    int64_t* ln = (int64_t*)ray_data(lens);
    uint8_t* kb = (uint8_t*)ray_data(keys);

    int64_t p = 0, run_start = 0;
    for (int64_t i = 1; i <= n; i++) {
        bool boundary = (i == n) || PART_NEW_BLOCK(i);
        if (boundary && n > 0) {
            st[p] = run_start;
            ln[p] = i - run_start;
            memcpy(kb + (size_t)p*es, base + (size_t)run_start*es, (size_t)es);
            p++;
            run_start = i;
        }
    }
    #undef PART_NEW_BLOCK

    ray_t* idx = ray_index_alloc(RAY_IDX_PART, v->type, n);
    if (!idx || RAY_IS_ERR(idx)) {
        ray_release(starts); ray_release(lens); ray_release(keys);
        return idx ? idx : ray_error("oom", NULL);
    }
    ray_index_t* ix = ray_index_payload(idx);
    ix->u.part.keys    = keys;
    ix->u.part.starts  = starts;
    ix->u.part.lens    = lens;
    ix->u.part.n_parts = nparts;
    return attach_finalize(v, idx);
}

/* --------------------------------------------------------------------------
 * Detach (drop)
 *
 * Restore the parent's 16-byte aux union from the saved snapshot, then
 * release the index ray_t.  The release path of RAY_INDEX would otherwise
 * also try to release the saved-aux pointers, so we clear the saved
 * snapshot and saved_attrs first to neutralize that — ownership is moving
 * back to the parent.
 * -------------------------------------------------------------------------- */

ray_t* ray_index_drop(ray_t** vp) {
    if (!vp || !*vp || RAY_IS_ERR(*vp)) return *vp;
    ray_t* v = *vp;
    if (!(v->attrs & RAY_ATTR_HAS_INDEX)) return v;

    /* Detach mutates the parent in place; require sole ownership. */
    v = ray_cow(v);
    if (!v || RAY_IS_ERR(v)) { *vp = v; return v; }
    *vp = v;

    /* After ray_cow, *vp may be a freshly copied block.  In ray_alloc_copy,
     * the index pointer was retained by ray_retain_owned_refs (via the
     * RAY_ATTR_HAS_INDEX branch we add in heap.c), so v->index here is
     * still the live, owned index ray_t. */
    ray_t* idx = v->index;
    ray_index_t* ix = ray_index_payload(idx);

    /* Shared-index case: another vec may share this RAY_INDEX block via
     * ray_alloc_copy (rc>1).  Don't clobber the snapshot in that case —
     * the other holder still reads it.  See vec_drop_index_inplace for
     * the same pattern. */
    bool shared = ray_atomic_load(&idx->rc) > 1;
    if (shared) {
        ray_index_retain_saved(ix);
    }
    memcpy(v->aux, ix->saved_aux, 16);
    if (!shared) {
        memset(ix->saved_aux, 0, 16);
        ix->saved_attrs = 0;
    }

    /* Restore parent attrs.  HAS_NULLS was preserved through the
     * attachment so it needs no restoration. */
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_INDEX;

    /* Release the index.  Per-kind children are released by the RAY_INDEX
     * branch of ray_release_owned_refs (added in heap.c). */
    ray_release(idx);
    return v;
}

/* --------------------------------------------------------------------------
 * Info
 * -------------------------------------------------------------------------- */

static const char* kind_name(ray_idx_kind_t k) {
    switch (k) {
    case RAY_IDX_HASH:       return "hash";
    case RAY_IDX_SORT:       return "sort";
    case RAY_IDX_ZONE:       return "zone";
    case RAY_IDX_BLOOM:      return "bloom";
    case RAY_IDX_CHUNK_ZONE: return "chunk_zone";
    case RAY_IDX_PART:       return "part";
    default:                 return "none";
    }
}

static ray_t* dict_append_sym_i64(ray_t** keys, ray_t** vals, const char* k, int64_t n) {
    int64_t kid = ray_sym_intern(k, strlen(k));
    *keys = ray_vec_append(*keys, &kid);
    if (RAY_IS_ERR(*keys)) return *keys;
    ray_t* nv = ray_i64(n);
    *vals = ray_list_append(*vals, nv);
    ray_release(nv);
    return *vals;
}

static ray_t* dict_append_sym_sym(ray_t** keys, ray_t** vals, const char* k, const char* s) {
    int64_t kid = ray_sym_intern(k, strlen(k));
    *keys = ray_vec_append(*keys, &kid);
    if (RAY_IS_ERR(*keys)) return *keys;
    int64_t sid = ray_sym_intern(s, strlen(s));
    ray_t* sv = ray_sym(sid);
    *vals = ray_list_append(*vals, sv);
    ray_release(sv);
    return *vals;
}

ray_t* ray_index_info(ray_t* v) {
    if (!ray_index_has(v)) return RAY_NULL_OBJ;
    ray_index_t* ix = ray_index_payload(v->index);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 8);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(8);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }

    ray_t* r;
    r = dict_append_sym_sym(&keys, &vals, "kind", kind_name((ray_idx_kind_t)ix->kind));
    if (RAY_IS_ERR(r)) goto fail;
    r = dict_append_sym_i64(&keys, &vals, "length", ix->built_for_len);
    if (RAY_IS_ERR(r)) goto fail;
    r = dict_append_sym_i64(&keys, &vals, "parent_type", (int64_t)ix->parent_type);
    if (RAY_IS_ERR(r)) goto fail;
    r = dict_append_sym_i64(&keys, &vals, "saved_attrs", (int64_t)ix->saved_attrs);
    if (RAY_IS_ERR(r)) goto fail;

    switch ((ray_idx_kind_t)ix->kind) {
    case RAY_IDX_ZONE:
        if (ix->parent_type == RAY_F32 || ix->parent_type == RAY_F64) {
            int64_t kmin = ray_sym_intern("min", 3);
            keys = ray_vec_append(keys, &kmin);
            ray_t* mn = ray_f64(ix->u.zone.min_f);
            vals = ray_list_append(vals, mn); ray_release(mn);
            int64_t kmax = ray_sym_intern("max", 3);
            keys = ray_vec_append(keys, &kmax);
            ray_t* mx = ray_f64(ix->u.zone.max_f);
            vals = ray_list_append(vals, mx); ray_release(mx);
        } else {
            r = dict_append_sym_i64(&keys, &vals, "min", ix->u.zone.min_i);
            if (RAY_IS_ERR(r)) goto fail;
            r = dict_append_sym_i64(&keys, &vals, "max", ix->u.zone.max_i);
            if (RAY_IS_ERR(r)) goto fail;
        }
        r = dict_append_sym_i64(&keys, &vals, "n_nulls", ix->u.zone.n_nulls);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_HASH:
        r = dict_append_sym_i64(&keys, &vals, "capacity", (int64_t)(ix->u.hash.mask + 1));
        if (RAY_IS_ERR(r)) goto fail;
        r = dict_append_sym_i64(&keys, &vals, "n_keys",   ix->u.hash.n_keys);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_SORT:
        r = dict_append_sym_i64(&keys, &vals, "perm_len",
                                ix->u.sort.perm ? ix->u.sort.perm->len : 0);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_BLOOM:
        r = dict_append_sym_i64(&keys, &vals, "m_bits", (int64_t)(ix->u.bloom.m_mask + 1));
        if (RAY_IS_ERR(r)) goto fail;
        r = dict_append_sym_i64(&keys, &vals, "k", (int64_t)ix->u.bloom.k);
        if (RAY_IS_ERR(r)) goto fail;
        r = dict_append_sym_i64(&keys, &vals, "n_keys", ix->u.bloom.n_keys);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_CHUNK_ZONE:
        r = dict_append_sym_i64(&keys, &vals, "n_chunks",
                                (int64_t)ix->u.chunk_zone.n_chunks);
        if (RAY_IS_ERR(r)) goto fail;
        r = dict_append_sym_i64(&keys, &vals, "chunk_log2",
                                (int64_t)ix->u.chunk_zone.chunk_log2);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_PART:
        r = dict_append_sym_i64(&keys, &vals, "n_parts", ix->u.part.n_parts);
        if (RAY_IS_ERR(r)) goto fail;
        break;
    case RAY_IDX_NONE:
        break;
    }

    return ray_dict_new(keys, vals);

fail:
    if (!RAY_IS_ERR(keys)) ray_release(keys);
    if (!RAY_IS_ERR(vals)) ray_release(vals);
    return r;
}

/* --------------------------------------------------------------------------
 * Rayfall builtins (registered from src/lang/eval.c)
 * -------------------------------------------------------------------------- */

/* Common entry shape: take a borrowed ref, return an owning ref of the
 * (possibly COW-copied) parent.  See heap.c:ray_release on rc transfer. */
static ray_t* attach_via(ray_t* v, ray_t* (*fn)(ray_t**)) {
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* w = v;
    ray_retain(w);
    ray_t* r = fn(&w);
    if (RAY_IS_ERR(r)) { ray_release(w); return r; }
    return w;
}

ray_t* ray_idx_zone_fn (ray_t* v) { return attach_via(v, ray_index_attach_zone);  }
ray_t* ray_idx_hash_fn (ray_t* v) { return attach_via(v, ray_index_attach_hash);  }
ray_t* ray_idx_sort_fn (ray_t* v) { return attach_via(v, ray_index_attach_sort);  }
ray_t* ray_idx_bloom_fn(ray_t* v) { return attach_via(v, ray_index_attach_bloom); }

ray_t* ray_idx_drop_fn(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* w = v;
    ray_retain(w);
    ray_t* r = ray_index_drop(&w);
    if (RAY_IS_ERR(r)) { ray_release(w); return r; }
    return w;
}

ray_t* ray_idx_has_fn(ray_t* v) {
    return ray_bool(ray_index_has(v) ? 1 : 0);
}

ray_t* ray_idx_info_fn(ray_t* v) {
    return ray_index_info(v);
}

/* --------------------------------------------------------------------------
 * Semantic attributes — (.attr.*) family.  See docs spec
 * 2026-06-01-column-attributes-design.md.
 * -------------------------------------------------------------------------- */

/* unique: verify distinctness, then set a block-resident marker bit.  If the
 * column already carries an index block, clone it (it is shared post-cow) and
 * mark the clone; otherwise attach a marker-only block (kind NONE). */
static ray_t* attr_set_unique(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("type", "attr: null");
    if (!ray_is_vec(v))
        return ray_error("type", "unique: attribute applies to vectors only");
    if (numeric_elem_size(v->type) == 0)
        return ray_error("nyi", "unique: only numeric vectors supported in v1 (type %d)", (int)v->type);
    if (v->attrs & RAY_ATTR_SLICE)
        return ray_error("type", "unique: cannot attribute a slice; materialize first");
    if (!vec_all_distinct(v))
        return ray_error("domain", "unique: column contains duplicate values");

    ray_retain(v);
    ray_t* w = ray_cow(v);
    if (!w || RAY_IS_ERR(w)) return w ? w : ray_error("oom", NULL);

    if (w->attrs & RAY_ATTR_HAS_INDEX) {
        /* Post-cow w->index is SHARED with the original (ray_cow retains the
         * block, it does not deep-copy it).  Clone it before mutating markers
         * so the original's block is untouched.  Then the clone is sole-owned. */
        ray_t* nb = clone_index_block(w->index);
        if (!nb || RAY_IS_ERR(nb)) { ray_release(w); return nb ? nb : ray_error("oom", NULL); }
        ray_release(w->index);
        w->index = nb;
        ray_index_payload(nb)->markers |= RAY_MARK_UNIQUE;
        return w;
    }
    /* No backing index: attach a marker-only block (kind NONE). */
    ray_t* idx = ray_index_alloc(RAY_IDX_NONE, w->type, w->len);
    if (!idx || RAY_IS_ERR(idx)) { ray_release(w); return idx ? idx : ray_error("oom", NULL); }
    ray_index_payload(idx)->markers = RAY_MARK_UNIQUE;
    return attach_finalize(w, idx);
}

/* Build a backing index via `fn` (drop-and-rebuild through prepare_attach),
 * carrying any block-resident markers across the rebuild.  Used by grouped and
 * parted, which replace the backing index but must preserve markers such as
 * unique.  Only RAY_MARK_UNIQUE exists today; revisit the blanket carry if a
 * marker is ever added that should NOT survive replacing the backing index.
 * The sorted marker lives in attrs (not the block) and survives attach. */
static ray_t* attach_backing_index_carry_markers(ray_t* v, ray_t* (*fn)(ray_t**)) {
    uint8_t carry = (v && !RAY_IS_ERR(v) && ray_index_has(v))
                    ? ray_index_payload(v->index)->markers : 0;
    ray_t* w = attach_via(v, fn);
    if (w && !RAY_IS_ERR(w) && carry && (w->attrs & RAY_ATTR_HAS_INDEX))
        ray_index_payload(w->index)->markers |= carry;
    return w;
}

/* grouped == hash index. */
static ray_t* attr_set_grouped(ray_t* v) {
    return attach_backing_index_carry_markers(v, ray_index_attach_hash);
}

/* parted == contiguous ascending value-block index. */
static ray_t* attr_set_parted(ray_t* v) {
    return attach_backing_index_carry_markers(v, ray_index_attach_part);
}

/* Set the sorted marker after verifying.  Borrowed v in, owning ref out. */
static ray_t* attr_set_sorted(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("type", "attr: null");
    if (!ray_is_vec(v))
        return ray_error("type", "sorted: attribute applies to vectors only");
    if (numeric_elem_size(v->type) == 0)
        return ray_error("nyi", "sorted: only numeric vectors supported in v1 (type %d)",
                         (int)v->type);
    if (!vec_is_ascending(v))
        return ray_error("domain", "sorted: column is not in non-descending order");
    ray_retain(v);
    ray_t* w = ray_cow(v);
    if (!w || RAY_IS_ERR(w)) return w ? w : ray_error("oom", NULL);
    w->attrs |= RAY_ATTR_SORTED;
    return w;
}

/* (.attr.get v) -> symbol vector of attributes held (sorted, unique, grouped,
 * parted), or the empty symbol vector. */
ray_t* ray_attr_get_fn(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* syms = ray_vec_new(RAY_SYM, 4);
    if (!syms || RAY_IS_ERR(syms)) return syms ? syms : ray_error("oom", NULL);
    syms->len = 0;
    int64_t* out = (int64_t*)ray_data(syms);
    if (ray_attr_is_sorted(v))
        out[syms->len++] = ray_sym_intern_runtime("sorted", 6);
    if (ray_index_has(v)) {
        ray_index_t* ix = ray_index_payload(v->index);
        if (ix->markers & RAY_MARK_UNIQUE)
            out[syms->len++] = ray_sym_intern_runtime("unique", 6);
        if (ix->kind == RAY_IDX_HASH)
            out[syms->len++] = ray_sym_intern_runtime("grouped", 7);
        else if (ix->kind == RAY_IDX_PART)
            out[syms->len++] = ray_sym_intern_runtime("parted", 6);
    }
    return syms;
}

/* (.attr.drop v) -> v with all attributes and any backing index removed. */
ray_t* ray_attr_drop_fn(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* w = v;
    ray_retain(w);
    if (w->attrs & RAY_ATTR_HAS_INDEX) {
        ray_t* r = ray_index_drop(&w);
        if (RAY_IS_ERR(r)) { ray_release(w); return r; }
    }
    if (w->attrs & RAY_ATTR_SORTED) {
        ray_t* c = ray_cow(w);
        if (!c || RAY_IS_ERR(c)) return c ? c : ray_error("oom", NULL);
        c->attrs &= ~RAY_ATTR_SORTED;
        w = c;
    }
    return w;
}

/* (.attr.set 'name v) — dispatch on the symbol name. */
ray_t* ray_attr_set_fn(ray_t* name, ray_t* v) {
    if (RAY_IS_ERR(name)) return name;
    if (!name || name->type != -RAY_SYM)
        return ray_error("type", "attr.set: first arg must be a symbol");
    int64_t id = name->i64;
    if (id == ray_sym_intern_runtime("sorted", 6))  return attr_set_sorted(v);
    if (id == ray_sym_intern_runtime("unique", 6))   return attr_set_unique(v);
    if (id == ray_sym_intern_runtime("grouped", 7))  return attr_set_grouped(v);
    if (id == ray_sym_intern_runtime("parted", 6))   return attr_set_parted(v);
    return ray_error("domain", "attr.set: unknown attribute (want sorted/unique/grouped/parted)");
}

/* --------------------------------------------------------------------------
 * Hash-index find — minimum row id whose value equals key, or -1 / -2.
 * -------------------------------------------------------------------------- */

int64_t ray_index_find_row(ray_t* col, int64_t key) {
    /* Float-family: equality has NaN/-0 semantics owned by the scan kernel. */
    if (!col || RAY_IS_ERR(col)) return -2;
    int8_t t = col->type;
    if (t == RAY_F32 || t == RAY_F64) return -2;

    /* idx_fresh_nonull: freshness + kind check + null-bearing gate. */
    if (!idx_fresh_nonull(col, RAY_IDX_HASH)) return -2;

    /* Out-of-range key cannot equal any stored value of this type. */
    if (!hash_key_in_range(t, key)) return -1;

    int64_t start_rid = -1;
    ray_index_t* ix = hash_probe_setup(col, key, &start_rid);
    if (!ix) return -2;   /* unexpected failure after eligibility passed */

    const int64_t* chn  = (const int64_t*)ray_data(ix->u.hash.chain);
    const uint8_t* base = (const uint8_t*)ray_data(col);

    int64_t min_rid = -1;
    int64_t rid = start_rid;
    while (rid >= 0) {
        if (hash_col_read_i64(base, t, rid) == key) {
            if (min_rid < 0 || rid < min_rid)
                min_rid = rid;
        }
        rid = chn[rid] - 1;
    }
    return min_rid;  /* -1 when nothing matched = key provably absent */
}
