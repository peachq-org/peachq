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

/*
 * domain.c -- Symbol-resolution domains (see domain.h).
 *
 * The FILE-domain reader is INDEPENDENT of g_sym/sym.c: it shares only
 * the on-disk STRL format knowledge (4-byte "STRL" magic, int64 count,
 * then count x [u32 len | bytes] records).  Nothing here touches the
 * global intern table.
 *
 * Phase 1: FILE domains are read-only — base mapping only.  The
 * growable in-memory tail (live interning + dirty flush) is Phase 2.
 *
 * Concurrency: one global spinlock guards the cache list, FILE-domain
 * refcounts, the lazily built reverse index and the runtime-id LUT
 * build.  Domain opens and first-find index builds are rare.  The two
 * hot read paths are LOCK-FREE: ray_sym_domain_str reads the atom array
 * materialized EAGERLY at open (a per-call spinlock would serialize
 * parallel kernels — sym_lex_lt alone does 2 calls per compare per
 * row), and ray_sym_domain_runtime_lut reads an atomically published
 * array after its one-time sequential build.  The runtime singleton
 * never touches the lock (immortal, delegates to sym.c's own locking).
 */

#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#  define _GNU_SOURCE       /* realpath */
#endif

#include "domain.h"
#include "core/platform.h"  /* ray_vm_map_file / ray_vm_unmap_file */
#include "ops/hash.h"       /* ray_hash_bytes (same hash family as g_sym) */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Same on-disk format constant as src/table/sym.c (private there). */
#define DOMAIN_STRL_MAGIC 0x4C525453U /* "STRL" */

typedef enum {
    DOM_RUNTIME = 0,
    DOM_FILE    = 1,
} dom_kind_t;

struct ray_sym_domain_s {
    uint8_t   kind;        /* dom_kind_t */
    bool      immortal;    /* runtime singleton: retain/release skip */
    uint32_t  rc;          /* FILE: open/attach refs (guarded by lock) */
    char*     path;        /* FILE: malloc'd realpath; NULL for runtime */

    /* FILE: read-only base mapping of the symfile (Phase 1: no tail). */
    void*     map;
    size_t    map_size;
    int64_t   count;
    uint64_t* offs;        /* count entries: offset of string bytes in map */
    uint32_t* lens;        /* count entries: string byte length */

    /* FILE: per-position string atoms, owned by the domain.  Materialized
     * EAGERLY at open (atoms ≈ vocabulary size — small by design), so
     * ray_sym_domain_str is a lock-free array read. */
    ray_t**   atoms;

    /* FILE, lazy: position → runtime intern id.  Built ONCE on first
     * request under g_dom_lock (the build INTERNS the vocabulary into the
     * global table — sequential by contract, see the header), published
     * with release semantics; reads are lock-free. */
    _Atomic(int64_t*) runtime_lut;

    /* FILE, lazy: reverse index — (hash32 << 32) | (pos + 1), 0 = empty.
     * Same bucket encoding as g_sym (sym.c). */
    uint64_t* buckets;
    uint64_t  bucket_mask; /* cap - 1; 0 = not built yet */

    struct ray_sym_domain_s* next; /* cache chain */
};

/* ---- runtime singleton --------------------------------------------------- */

static struct ray_sym_domain_s g_runtime_domain = {
    .kind     = DOM_RUNTIME,
    .immortal = true,
};

ray_sym_domain_t* ray_sym_runtime_domain(void) {
    return &g_runtime_domain;
}

/* ---- cache + lock --------------------------------------------------------- */

static ray_sym_domain_t* g_domains = NULL;

static _Atomic(int) g_dom_lock = 0;
static inline void dom_lock(void) {
    while (atomic_exchange_explicit(&g_dom_lock, 1, memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}
static inline void dom_unlock(void) {
    atomic_store_explicit(&g_dom_lock, 0, memory_order_release);
}

/* ---- FILE domain construction / destruction ------------------------------- */

/* Parse the mapped STRL payload into the offs/lens arrays.  Returns false
 * on any structural inconsistency (truncated record, trailing bytes). */
static bool dom_parse_strl(ray_sym_domain_t* d) {
    const uint8_t* base = (const uint8_t*)d->map;
    if (d->map_size < 12) return false;

    uint32_t magic;
    memcpy(&magic, base, 4);
    if (magic != DOMAIN_STRL_MAGIC) return false;

    int64_t count;
    memcpy(&count, base + 4, 8);
    if (count < 0 || count > UINT32_MAX) return false;

    uint64_t* offs = NULL;
    uint32_t* lens = NULL;
    if (count > 0) {
        offs = (uint64_t*)malloc((size_t)count * sizeof(uint64_t));
        lens = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
        if (!offs || !lens) { free(offs); free(lens); return false; }
    }

    size_t off = 12;
    size_t remaining = d->map_size - 12;
    for (int64_t i = 0; i < count; i++) {
        if (remaining < 4) { free(offs); free(lens); return false; }
        uint32_t slen;
        memcpy(&slen, base + off, 4);
        off += 4; remaining -= 4;
        if ((size_t)slen > remaining) { free(offs); free(lens); return false; }
        offs[i] = (uint64_t)off;
        lens[i] = slen;
        off += slen; remaining -= slen;
    }
    if (remaining != 0) { free(offs); free(lens); return false; }

    d->count = count;
    d->offs = offs;
    d->lens = lens;
    return true;
}

/* Free a FILE domain after the last reference dropped.  Caller has
 * already unlinked it from the cache. */
static void dom_destroy(ray_sym_domain_t* d) {
    if (d->atoms) {
        for (int64_t i = 0; i < d->count; i++) {
            if (d->atoms[i]) ray_release(d->atoms[i]);
        }
        free(d->atoms);
    }
    free(atomic_load_explicit(&d->runtime_lut, memory_order_relaxed));
    free(d->buckets);
    free(d->offs);
    free(d->lens);
    if (d->map) ray_vm_unmap_file(d->map, d->map_size);
    free(d->path);
    free(d);
}

ray_sym_domain_t* ray_sym_domain_open(const char* path) {
    if (!path) return NULL;

    /* Realpath-keyed: two spellings of the same file are the same domain. */
    char* rpath = realpath(path, NULL);
    if (!rpath) return NULL;

    dom_lock();
    for (ray_sym_domain_t* d = g_domains; d; d = d->next) {
        if (strcmp(d->path, rpath) == 0) {
            d->rc++;
            dom_unlock();
            free(rpath);
            return d;
        }
    }
    dom_unlock();

    /* Not cached: build outside the lock (mapping + parse can be slow). */
    ray_sym_domain_t* d = (ray_sym_domain_t*)calloc(1, sizeof(*d));
    if (!d) { free(rpath); return NULL; }
    d->kind = DOM_FILE;
    d->rc = 1;
    d->path = rpath;

    d->map = ray_vm_map_file(rpath, &d->map_size);
    if (!d->map || !dom_parse_strl(d)) {
        if (d->map) ray_vm_unmap_file(d->map, d->map_size);
        free(d->path);
        free(d);
        return NULL;
    }

    /* Position-0 reservation: every non-empty symfile must carry the
     * empty string at position 0 (mirrors global id 0 — the SYM null;
     * group kernels and null conventions treat id 0 as "" / null).  A
     * vocabulary that starts with anything else is structurally corrupt
     * for domain use.  Empty files (count == 0) are fine. */
    if (d->count > 0 && d->lens[0] != 0) {
        dom_destroy(d);
        return NULL;
    }

    /* Eagerly materialize the whole borrowed-atom array so that
     * ray_sym_domain_str is a lock-free array read.  Memory: one string
     * atom per VOCABULARY entry (not per row) — small by design. */
    if (d->count > 0) {
        d->atoms = (ray_t**)calloc((size_t)d->count, sizeof(ray_t*));
        if (!d->atoms) { dom_destroy(d); return NULL; }
        for (int64_t i = 0; i < d->count; i++) {
            ray_t* s = ray_str((const char*)d->map + d->offs[i], d->lens[i]);
            if (!s || RAY_IS_ERR(s)) { dom_destroy(d); return NULL; }
            d->atoms[i] = s;  /* owned by the domain; callers borrow */
        }
    }

    /* Insert, unless another thread raced us to the same path — then keep
     * the winner (pointer equality must hold for one resolved path). */
    dom_lock();
    for (ray_sym_domain_t* e = g_domains; e; e = e->next) {
        if (strcmp(e->path, d->path) == 0) {
            e->rc++;
            dom_unlock();
            dom_destroy(d);
            return e;
        }
    }
    d->next = g_domains;
    g_domains = d;
    dom_unlock();
    return d;
}

void ray_sym_domain_retain(ray_sym_domain_t* dom) {
    if (!dom || dom->immortal) return;
    dom_lock();
    dom->rc++;
    dom_unlock();
}

void ray_sym_domain_release(ray_sym_domain_t* dom) {
    if (!dom || dom->immortal) return;
    dom_lock();
    if (--dom->rc > 0) {
        dom_unlock();
        return;
    }
    /* Last ref: unlink the cache entry, then free outside the lock.
     * A subsequent open of the same path builds a fresh mapping. */
    ray_sym_domain_t** pp = &g_domains;
    while (*pp && *pp != dom) pp = &(*pp)->next;
    if (*pp) *pp = dom->next;
    dom_unlock();
    dom_destroy(dom);
}

/* ---- resolution ------------------------------------------------------------ */

ray_t* ray_sym_domain_str(ray_sym_domain_t* dom, int64_t pos) {
    if (!dom) return NULL;
    if (dom->kind == DOM_RUNTIME) return ray_sym_str(pos);

    if (pos < 0 || pos >= dom->count) return NULL;

    /* Lock-free: the atom array is fully materialized at open, immutable
     * for the domain's lifetime (parallel kernels read this path). */
    return dom->atoms[pos];
}

/* Empty-vocabulary FILE domains get a distinct non-NULL LUT so callers
 * can branch on NULL == "runtime domain, ids pass through".  Never
 * indexed: every position is out of range when count == 0. */
static const int64_t g_empty_lut[1] = { -1 };

const int64_t* ray_sym_domain_runtime_lut(ray_sym_domain_t* dom) {
    if (!dom || dom->kind == DOM_RUNTIME) return NULL;

    int64_t* lut = atomic_load_explicit(&dom->runtime_lut, memory_order_acquire);
    if (lut) return lut;
    if (dom->count == 0) return g_empty_lut;

    dom_lock();
    lut = atomic_load_explicit(&dom->runtime_lut, memory_order_relaxed);
    if (!lut) {
        lut = (int64_t*)malloc((size_t)dom->count * sizeof(int64_t));
        if (!lut) { dom_unlock(); return NULL; }
        /* THE sanctioned interning of a file vocabulary into the global
         * table: one sequential pass over the VOCABULARY (never the
         * rows).  Callers that hand ids to ray_pool_dispatch workers
         * must request this LUT during sequential setup — interning
         * inside a worker violates sym.c's frozen-table rule. */
        for (int64_t i = 0; i < dom->count; i++)
            lut[i] = ray_sym_intern((const char*)dom->map + dom->offs[i],
                                    dom->lens[i]);
        atomic_store_explicit(&dom->runtime_lut, lut, memory_order_release);
    }
    dom_unlock();
    return lut;
}

/* Build the reverse index.  Called under the lock. */
static bool dom_build_index_locked(ray_sym_domain_t* d) {
    uint64_t cap = 16;
    while ((double)d->count > 0.7 * (double)cap) {
        if (cap > (UINT64_MAX >> 1)) return false;
        cap <<= 1;
    }
    uint64_t* buckets = (uint64_t*)calloc((size_t)cap, sizeof(uint64_t));
    if (!buckets) return false;

    const uint8_t* base = (const uint8_t*)d->map;
    uint64_t mask = cap - 1;
    for (int64_t i = 0; i < d->count; i++) {
        uint32_t h = (uint32_t)ray_hash_bytes(base + d->offs[i], d->lens[i]);
        uint64_t slot = h & mask;
        while (buckets[slot] != 0) slot = (slot + 1) & mask;
        buckets[slot] = ((uint64_t)h << 32) | ((uint64_t)(uint32_t)i + 1);
    }
    d->buckets = buckets;
    d->bucket_mask = mask;
    return true;
}

int64_t ray_sym_domain_find(ray_sym_domain_t* dom, const char* str, size_t len) {
    if (!dom || !str) return -1;
    if (dom->kind == DOM_RUNTIME) return ray_sym_find(str, len);

    if (dom->count == 0) return -1;

    dom_lock();
    if (!dom->buckets && !dom_build_index_locked(dom)) {
        dom_unlock();
        return -1;
    }
    const uint8_t* base = (const uint8_t*)dom->map;
    uint32_t h = (uint32_t)ray_hash_bytes(str, len);
    uint64_t mask = dom->bucket_mask;
    uint64_t slot = h & mask;
    int64_t found = -1;
    while (dom->buckets[slot] != 0) {
        uint64_t e = dom->buckets[slot];
        if ((uint32_t)(e >> 32) == h) {
            int64_t pos = (int64_t)(uint32_t)e - 1;
            if (dom->lens[pos] == len &&
                (len == 0 || memcmp(base + dom->offs[pos], str, len) == 0)) {
                found = pos;
                break;
            }
        }
        slot = (slot + 1) & mask;
    }
    dom_unlock();
    return found;
}

int64_t ray_sym_domain_intern(ray_sym_domain_t* dom, const char* str, size_t len) {
    if (!dom || !str) return -1;
    if (dom->kind == DOM_RUNTIME) return ray_sym_intern(str, len);
    /* Phase 2: in-memory tail append + single-writer flock.  Until then a
     * FILE domain is read-only — only lookups of existing entries work. */
    return -1;
}

int64_t ray_sym_domain_count(ray_sym_domain_t* dom) {
    if (!dom) return 0;
    if (dom->kind == DOM_RUNTIME) return (int64_t)ray_sym_count();
    return dom->count;  /* immutable while read-only */
}

const char* ray_sym_domain_path(ray_sym_domain_t* dom) {
    if (!dom || dom->kind == DOM_RUNTIME) return NULL;
    return dom->path;
}

ray_err_t ray_sym_domain_flush(ray_sym_domain_t* dom, bool durable) {
    /* Phase 2: write the dirty tail under the symfile flock, verifying the
     * file still matches the base+tail expectation (concurrent-writer
     * detection).  Phase 1 domains have no tail to flush. */
    (void)dom;
    (void)durable;
    return RAY_ERR_NYI;
}
