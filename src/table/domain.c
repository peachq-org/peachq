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
 * then count x [u32 len | bytes] records).  The ONLY touchpoint with
 * the global intern table is the runtime-id LUT build (sanctioned,
 * sequential) and the RAY_SYM_AUDIT failure path.
 *
 * Post-flip (Task 7b) a FILE domain is base + tail: the mmapped file at
 * open time plus in-memory appended symbols (ray_sym_domain_intern).
 * Append-only; positions are permanent; ray_sym_domain_flush persists
 * base+tail atomically under the symfile's flock.
 *
 * Concurrency: one global spinlock guards the cache list, FILE-domain
 * refcounts, appends, the lazily built reverse index and the runtime-id
 * LUT build.  Domain opens, interns (save merges / live inserts — all
 * inside sequential eval) and first-find index builds are rare.  The
 * two hot read paths are LOCK-FREE: ray_sym_domain_str reads the
 * atomically published (count, atoms) pair — appends publish a slot
 * write with a release store of the count, growth REPLACES the array
 * (old arrays are retired, never freed in place, so a stale pointer is
 * always valid memory covering every position < its publish count) —
 * and ray_sym_domain_runtime_lut reads an atomically published array
 * (invalidated and rebuilt across appends; retired likewise).  The
 * runtime singleton never touches the lock (immortal, delegates to
 * sym.c's own locking).
 */

#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#  define _GNU_SOURCE       /* realpath */
#endif

#include "domain.h"
#include "core/platform.h"  /* ray_vm_map_file / ray_vm_unmap_file */
#include "mem/heap.h"       /* ray_heap_current_id — atom-heap tagging */
#include "store/fileio.h"   /* flock + tmp/rename protocol for flush */
#include "ops/hash.h"       /* ray_hash_bytes (same hash family as g_sym) */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libgen.h>
#include <sys/stat.h>

/* Same on-disk format constant as src/table/sym.c (private there). */
#define DOMAIN_STRL_MAGIC 0x4C525453U /* "STRL" */

typedef enum {
    DOM_RUNTIME = 0,
    DOM_FILE    = 1,
} dom_kind_t;

/* Retired allocations (replaced atom arrays / invalidated LUTs): kept
 * alive until domain destroy so lock-free readers holding a stale
 * pointer never touch freed memory. */
typedef struct dom_retired_s {
    void* ptr;
    struct dom_retired_s* next;
} dom_retired_t;

struct ray_sym_domain_s {
    uint8_t   kind;        /* dom_kind_t */
    bool      immortal;    /* runtime singleton: retain/release skip */
    uint32_t  rc;          /* FILE: open/attach refs (guarded by lock) */
    char*     path;        /* FILE: malloc'd resolved path; NULL for runtime */

    /* FILE: base mapping of the symfile as of open (NULL for a freshly
     * created domain with no file yet). */
    void*     map;
    size_t    map_size;
    int64_t   base_count;  /* entries that came from the mapping */

    /* FILE: persisted state — how much of the vocabulary the file holds
     * (updated at open / extend / flush) and the exact byte size of the
     * file at that point (cache-hit revalidation key). */
    int64_t   disk_count;
    size_t    disk_size;

    /* FILE: published vocabulary — (atoms, count) pair.  atoms[i] is an
     * owned string atom for position i (borrowed by callers).  count is
     * release-published AFTER the slot write; growth REPLACES the array
     * via atomic store and retires the old one.  Lock-free readers load
     * count (acquire) then atoms. */
    _Atomic(int64_t)  count;
    _Atomic(ray_t**)  atoms;
    int64_t   atoms_cap;   /* guarded by g_dom_lock */

    /* FILE, lazy: position → runtime intern id.  Built under g_dom_lock
     * (the build INTERNS the vocabulary into the global table —
     * sequential by contract, see the header), published with release
     * semantics; reads are lock-free.  Appends invalidate it (retire +
     * NULL); the next request rebuilds over the grown vocabulary. */
    _Atomic(int64_t*) runtime_lut;

    /* FILE, lazy: reverse index — (hash32 << 32) | (pos + 1), 0 = empty.
     * Same bucket encoding as g_sym (sym.c).  Invariant: when buckets
     * exist they cover EVERY published entry (lazy build is full; intern
     * inserts incrementally; growth rebuilds).  Guarded by g_dom_lock. */
    uint64_t* buckets;
    uint64_t  bucket_mask; /* cap - 1; 0 = not built yet */

    dom_retired_t* retired; /* replaced atom arrays + old LUTs */

    /* Heap that backs this domain's string atoms (atoms[] are ray_str
     * objects on the buddy heap, created when the domain was opened).
     * The process-global cache outlives a per-thread ray_heap_destroy,
     * so a cached domain whose heap is torn down would dangle; recording
     * the heap lets ray_heap_destroy drop exactly its own domains while
     * the atoms are still valid.  0xFFFF = "no heap was bound at
     * creation" (never matches a real id, so never auto-dropped). */
    uint16_t  atom_heap_id;

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

/* Resolved cache key for `path`.  realpath of the file when it exists;
 * for to-be-created symfiles, realpath of the parent + "/" + basename
 * (the parent must exist).  malloc'd. */
static char* dom_resolve_path(const char* path) {
    char* rpath = realpath(path, NULL);
    if (rpath) return rpath;

    char tmp[1024];
    size_t plen = strlen(path);
    if (plen == 0 || plen >= sizeof(tmp)) return NULL;
    memcpy(tmp, path, plen + 1);
    char* base = basename(tmp);
    char* bdup = strdup(base);
    if (!bdup) return NULL;
    /* basename may have modified tmp; recompute dirname on a fresh copy */
    memcpy(tmp, path, plen + 1);
    char* dir = dirname(tmp);
    char* rdir = realpath(dir, NULL);
    if (!rdir) { free(bdup); return NULL; }
    size_t dlen = strlen(rdir), blen = strlen(bdup);
    char* out = (char*)malloc(dlen + 1 + blen + 1);
    if (!out) { free(rdir); free(bdup); return NULL; }
    memcpy(out, rdir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, bdup, blen + 1);
    free(rdir);
    free(bdup);
    return out;
}

/* Retire an allocation that lock-free readers may still hold. */
static bool dom_retire(ray_sym_domain_t* d, void* p) {
    if (!p) return true;
    dom_retired_t* r = (dom_retired_t*)malloc(sizeof(*r));
    if (!r) return false;
    r->ptr = p;
    r->next = d->retired;
    d->retired = r;
    return true;
}

/* Free a FILE domain after the last reference dropped.  Caller has
 * already unlinked it from the cache. */
static void dom_destroy(ray_sym_domain_t* d) {
    ray_t** atoms = atomic_load_explicit(&d->atoms, memory_order_relaxed);
    int64_t count = atomic_load_explicit(&d->count, memory_order_relaxed);
    if (atoms) {
        for (int64_t i = 0; i < count; i++)
            if (atoms[i]) ray_release(atoms[i]);
        free(atoms);
    }
    free(atomic_load_explicit(&d->runtime_lut, memory_order_relaxed));
    for (dom_retired_t* r = d->retired; r;) {
        dom_retired_t* nxt = r->next;
        free(r->ptr);
        free(r);
        r = nxt;
    }
    free(d->buckets);
    if (d->map) ray_vm_unmap_file(d->map, d->map_size);
    free(d->path);
    free(d);
}

/* Parse a mapped STRL image, materializing one owned string atom per
 * record into a fresh array.  Returns false on structural inconsistency
 * (bad magic, truncated record, trailing bytes) or OOM.  On success
 * *out_atoms (malloc'd, exactly *out_count entries; NULL when empty)
 * carries the vocabulary. */
static bool dom_parse_strl_atoms(const void* map, size_t map_size,
                                 ray_t*** out_atoms, int64_t* out_count) {
    const uint8_t* base = (const uint8_t*)map;
    if (map_size < 12) return false;

    uint32_t magic;
    memcpy(&magic, base, 4);
    if (magic != DOMAIN_STRL_MAGIC) return false;

    int64_t count;
    memcpy(&count, base + 4, 8);
    if (count < 0 || count > UINT32_MAX) return false;

    ray_t** atoms = NULL;
    if (count > 0) {
        atoms = (ray_t**)calloc((size_t)count, sizeof(ray_t*));
        if (!atoms) return false;
    }

    size_t off = 12;
    size_t remaining = map_size - 12;
    int64_t done = 0;
    for (int64_t i = 0; i < count; i++) {
        uint32_t slen;
        if (remaining < 4) goto fail;
        memcpy(&slen, base + off, 4);
        off += 4; remaining -= 4;
        if ((size_t)slen > remaining) goto fail;
        ray_t* s = ray_str((const char*)base + off, slen);
        if (!s || RAY_IS_ERR(s)) goto fail;
        atoms[i] = s;
        done = i + 1;
        off += slen; remaining -= slen;
    }
    if (remaining != 0) goto fail;

    *out_atoms = atoms;
    *out_count = count;
    return true;

fail:
    for (int64_t i = 0; i < done; i++) ray_release(atoms[i]);
    free(atoms);
    return false;
}

/* Cache-hit revalidation: the file changed size since this object last
 * touched it.  Append-only external growth (a reader process catching
 * up with a writer) EXTENDS the object in place; anything else —
 * shrunk/rewritten file, or growth while this process holds unflushed
 * appends (a second writer) — fails.  Called under g_dom_lock with the
 * file's current size in st_size. */
static bool dom_extend_from_file_locked(ray_sym_domain_t* d, size_t st_size) {
    int64_t count = atomic_load_explicit(&d->count, memory_order_relaxed);
    if (st_size < d->disk_size) return false;       /* shrunk: rewritten */
    if (count != d->disk_count) return false;       /* our tail is dirty */

    size_t map_size = 0;
    void* map = ray_vm_map_file(d->path, &map_size);
    if (!map) return false;

    ray_t** fresh = NULL;
    int64_t fresh_count = 0;
    if (!dom_parse_strl_atoms(map, map_size, &fresh, &fresh_count)) {
        ray_vm_unmap_file(map, map_size);
        return false;
    }

    bool ok = fresh_count >= count;
    /* Position-0 reservation also holds for externally grown files. */
    if (ok && count == 0 && fresh_count > 0 && ray_str_len(fresh[0]) != 0)
        ok = false;
    /* Append-only prefix check: every position we already published must
     * be byte-identical in the new image. */
    ray_t** atoms = atomic_load_explicit(&d->atoms, memory_order_relaxed);
    for (int64_t i = 0; ok && i < count; i++) {
        ray_t* a = atoms[i];
        ray_t* b = fresh[i];
        if (ray_str_len(a) != ray_str_len(b) ||
            (ray_str_len(a) > 0 &&
             memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) != 0))
            ok = false;
    }

    if (ok && fresh_count > count) {
        /* Publish the grown vocabulary: reuse the freshly parsed array
         * (it already contains copies of the prefix), retire the old. */
        if (!dom_retire(d, atoms)) {
            ok = false;
        } else {
            /* Old prefix atoms are owned by the OLD array's entries —
             * but we retired the array, not the atoms.  Transfer: keep
             * the old atoms in the published prefix so borrowed pointers
             * stay valid; release the fresh duplicates. */
            for (int64_t i = 0; i < count; i++) {
                ray_release(fresh[i]);
                fresh[i] = atoms[i];
            }
            atomic_store_explicit(&d->atoms, fresh, memory_order_release);
            d->atoms_cap = fresh_count;
            atomic_store_explicit(&d->count, fresh_count, memory_order_release);
            d->disk_count = fresh_count;
            d->disk_size = st_size;
            /* Reverse index + LUT now cover a stale prefix: drop both.
             * A retire-OOM here would keep the stale LUT published over
             * the grown count (OOB reads for lock-free consumers) — the
             * same corner dom_append_locked hits; mirror its loud abort
             * (the count is already published, there is no clean undo). */
            if (d->buckets) { free(d->buckets); d->buckets = NULL; d->bucket_mask = 0; }
            int64_t* lut = atomic_load_explicit(&d->runtime_lut, memory_order_relaxed);
            if (lut) {
                if (!dom_retire(d, lut)) {
                    fprintf(stderr, "rayforce: sym domain '%s': OOM retiring "
                                    "runtime LUT after external extend — cannot "
                                    "keep id translation consistent\n",
                            d->path ? d->path : "?");
                    abort();
                }
                atomic_store_explicit(&d->runtime_lut, NULL, memory_order_release);
            }
        }
    }

    if (!ok || fresh_count == count) {
        /* Failure, or nothing actually new (size-only change): drop the
         * fresh parse.  fresh entries beyond count (failure case) and
         * all entries (no-growth case) are duplicates we own. */
        bool published = ok && fresh_count > count;
        if (!published) {
            for (int64_t i = 0; i < fresh_count; i++) ray_release(fresh[i]);
            free(fresh);
        }
        if (ok && fresh_count == count) { d->disk_size = st_size; }
    }

    ray_vm_unmap_file(map, map_size);
    return ok;
}

static ray_sym_domain_t* dom_open_impl(const char* path, bool create) {
    if (!path) return NULL;

    char* rpath = dom_resolve_path(path);
    if (!rpath) return NULL;

    struct stat st;
    bool exists = (stat(rpath, &st) == 0);
    if (!exists && !create) { free(rpath); return NULL; }

    dom_lock();
    for (ray_sym_domain_t* d = g_domains; d; d = d->next) {
        if (strcmp(d->path, rpath) == 0) {
            /* Revalidate: external append-only growth extends in place;
             * any other divergence is loud (NULL). */
            size_t cur_size = exists ? (size_t)st.st_size : 0;
            if (cur_size != d->disk_size &&
                !dom_extend_from_file_locked(d, cur_size)) {
                dom_unlock();
                free(rpath);
                return NULL;
            }
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
    d->atom_heap_id = ray_heap_current_id();  /* heap that will back atoms */

    if (exists) {
        d->map = ray_vm_map_file(rpath, &d->map_size);
        ray_t** atoms = NULL;
        int64_t count = 0;
        if (!d->map ||
            !dom_parse_strl_atoms(d->map, d->map_size, &atoms, &count)) {
            dom_destroy(d);
            return NULL;
        }
        /* Position-0 reservation: every non-empty symfile must carry the
         * empty string at position 0 (mirrors global id 0 — the SYM
         * null; group kernels and null conventions treat id 0 as "" /
         * null).  A vocabulary that starts with anything else is
         * structurally corrupt for domain use.  Empty files are fine. */
        if (count > 0 && ray_str_len(atoms[0]) != 0) {
            for (int64_t i = 0; i < count; i++) ray_release(atoms[i]);
            free(atoms);
            dom_destroy(d);
            return NULL;
        }
        atomic_store_explicit(&d->atoms, atoms, memory_order_relaxed);
        atomic_store_explicit(&d->count, count, memory_order_relaxed);
        d->atoms_cap = count;
        d->base_count = count;
        d->disk_count = count;
        d->disk_size = d->map_size;
    }
    /* create && !exists: empty domain — vocabulary appears via intern
     * ("" seeded at position 0) and reaches disk on first flush. */

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

ray_sym_domain_t* ray_sym_domain_open(const char* path) {
    return dom_open_impl(path, false);
}

ray_sym_domain_t* ray_sym_domain_open_or_create(const char* path) {
    return dom_open_impl(path, true);
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

/* Drop every cached FILE domain whose string atoms live on `heap_id`.
 * Called from ray_heap_destroy BEFORE that heap's pools are unmapped, so
 * the atoms are still valid and dom_destroy's ray_release path is safe.
 * Without this the process-global cache would keep handing out domains
 * whose atoms were freed when their backing heap was torn down (e.g. the
 * per-test heap_init/destroy in the test harness) — a use-after-free on
 * the next open/extend of that path.  Detach under the lock, free
 * outside it. */
void ray_sym_domain_drop_heap(uint16_t heap_id) {
    dom_lock();
    ray_sym_domain_t* dead = NULL;
    ray_sym_domain_t** pp = &g_domains;
    while (*pp) {
        ray_sym_domain_t* d = *pp;
        if (d->kind == DOM_FILE && d->atom_heap_id == heap_id) {
            *pp = d->next;          /* unlink */
            d->next = dead;         /* stash for freeing outside the lock */
            dead = d;
        } else {
            pp = &d->next;
        }
    }
    dom_unlock();
    while (dead) {
        ray_sym_domain_t* next = dead->next;
        dom_destroy(dead);
        dead = next;
    }
}

/* ---- resolution ------------------------------------------------------------ */

ray_t* ray_sym_domain_str(ray_sym_domain_t* dom, int64_t pos) {
    if (!dom) return NULL;
    if (dom->kind == DOM_RUNTIME) return ray_sym_str(pos);

    /* Lock-free: acquire the published count, then the array.  Appends
     * release-publish the count after the slot write; array replacements
     * are release-published before the count bump and old arrays are
     * retired (valid memory) — a reader can never index a published
     * position into memory that lacks it. */
    int64_t count = atomic_load_explicit(&dom->count, memory_order_acquire);
    if (pos < 0 || pos >= count) return NULL;
    ray_t** atoms = atomic_load_explicit(&dom->atoms, memory_order_acquire);
    return atoms[pos];
}

/* Empty-vocabulary FILE domains get a distinct non-NULL LUT so callers
 * can branch on NULL == "runtime domain, ids pass through".  Never
 * indexed: every position is out of range when count == 0. */
static const int64_t g_empty_lut[1] = { -1 };

const int64_t* ray_sym_domain_runtime_lut(ray_sym_domain_t* dom) {
    if (!dom || dom->kind == DOM_RUNTIME) return NULL;

    int64_t* lut = atomic_load_explicit(&dom->runtime_lut, memory_order_acquire);
    if (lut) return lut;
    if (atomic_load_explicit(&dom->count, memory_order_acquire) == 0)
        return g_empty_lut;

    dom_lock();
    lut = atomic_load_explicit(&dom->runtime_lut, memory_order_relaxed);
    if (!lut) {
        int64_t count = atomic_load_explicit(&dom->count, memory_order_relaxed);
        ray_t** atoms = atomic_load_explicit(&dom->atoms, memory_order_relaxed);
        lut = (int64_t*)malloc((size_t)count * sizeof(int64_t));
        if (!lut) { dom_unlock(); return NULL; }
        /* THE sanctioned interning of a file vocabulary into the global
         * table: one sequential pass over the VOCABULARY (never the
         * rows).  Callers that hand ids to ray_pool_dispatch workers
         * must request this LUT during sequential setup — interning
         * inside a worker violates sym.c's frozen-table rule. */
        for (int64_t i = 0; i < count; i++) {
            lut[i] = ray_sym_intern(ray_str_ptr(atoms[i]), ray_str_len(atoms[i]));
            if (lut[i] < 0) {
                /* 7a-review hardening: never publish a LUT carrying -1
                 * intern failures — a silent -1 makes cross-domain keys
                 * spuriously equal.  Loud NULL instead. */
                free(lut);
                dom_unlock();
                return NULL;
            }
        }
        atomic_store_explicit(&dom->runtime_lut, lut, memory_order_release);
    }
    dom_unlock();
    return lut;
}

/* (Re)build the reverse index over every published entry.  Called under
 * the lock.  `extra` reserves headroom for entries about to be added. */
static bool dom_build_index_locked(ray_sym_domain_t* d, int64_t extra) {
    int64_t count = atomic_load_explicit(&d->count, memory_order_relaxed);
    uint64_t need = (uint64_t)count + (uint64_t)extra;
    uint64_t cap = 16;
    while ((double)need > 0.7 * (double)cap) {
        if (cap > (UINT64_MAX >> 1)) return false;
        cap <<= 1;
    }
    uint64_t* buckets = (uint64_t*)calloc((size_t)cap, sizeof(uint64_t));
    if (!buckets) return false;

    ray_t** atoms = atomic_load_explicit(&d->atoms, memory_order_relaxed);
    uint64_t mask = cap - 1;
    for (int64_t i = 0; i < count; i++) {
        uint32_t h = (uint32_t)ray_hash_bytes(ray_str_ptr(atoms[i]),
                                              ray_str_len(atoms[i]));
        uint64_t slot = h & mask;
        while (buckets[slot] != 0) slot = (slot + 1) & mask;
        buckets[slot] = ((uint64_t)h << 32) | ((uint64_t)(uint32_t)i + 1);
    }
    free(d->buckets);
    d->buckets = buckets;
    d->bucket_mask = mask;
    return true;
}

/* Probe the reverse index.  Called under the lock with buckets built. */
static int64_t dom_probe_locked(ray_sym_domain_t* d, uint32_t h,
                                const char* str, size_t len) {
    ray_t** atoms = atomic_load_explicit(&d->atoms, memory_order_relaxed);
    uint64_t mask = d->bucket_mask;
    uint64_t slot = h & mask;
    while (d->buckets[slot] != 0) {
        uint64_t e = d->buckets[slot];
        if ((uint32_t)(e >> 32) == h) {
            int64_t pos = (int64_t)(uint32_t)e - 1;
            if (ray_str_len(atoms[pos]) == len &&
                (len == 0 || memcmp(ray_str_ptr(atoms[pos]), str, len) == 0))
                return pos;
        }
        slot = (slot + 1) & mask;
    }
    return -1;
}

int64_t ray_sym_domain_find(ray_sym_domain_t* dom, const char* str, size_t len) {
    if (!dom || !str) return -1;
    if (dom->kind == DOM_RUNTIME) return ray_sym_find(str, len);

    if (atomic_load_explicit(&dom->count, memory_order_acquire) == 0)
        return -1;

    dom_lock();
    if (!dom->buckets && !dom_build_index_locked(dom, 0)) {
        dom_unlock();
        return -1;
    }
    uint32_t h = (uint32_t)ray_hash_bytes(str, len);
    int64_t found = dom_probe_locked(dom, h, str, len);
    dom_unlock();
    return found;
}

/* Append one symbol.  Called under the lock with buckets built and NOT
 * containing the symbol.  Publishes (atoms, count) atomically; retires
 * a replaced atom array; invalidates the runtime LUT.  Returns the new
 * position or -1 on OOM (no state change on failure). */
static int64_t dom_append_locked(ray_sym_domain_t* d, uint32_t h,
                                 const char* str, size_t len) {
    int64_t count = atomic_load_explicit(&d->count, memory_order_relaxed);
    if (count >= (int64_t)UINT32_MAX) return -1;

    /* Grow the published array by REPLACING it — never realloc in place:
     * lock-free readers may hold the old pointer. */
    if (count >= d->atoms_cap) {
        int64_t ncap = d->atoms_cap < 8 ? 8 : d->atoms_cap * 2;
        ray_t** narr = (ray_t**)malloc((size_t)ncap * sizeof(ray_t*));
        if (!narr) return -1;
        ray_t** old = atomic_load_explicit(&d->atoms, memory_order_relaxed);
        if (count > 0) memcpy(narr, old, (size_t)count * sizeof(ray_t*));
        if (!dom_retire(d, old)) { free(narr); return -1; }
        atomic_store_explicit(&d->atoms, narr, memory_order_release);
        d->atoms_cap = ncap;
    }

    /* Bucket headroom (rebuild covers everything published so far). */
    uint64_t cap = d->bucket_mask + 1;
    if ((double)(count + 1) > 0.7 * (double)cap &&
        !dom_build_index_locked(d, 1))
        return -1;

    ray_t* s = ray_str(str, len);
    if (!s || RAY_IS_ERR(s)) return -1;

    ray_t** atoms = atomic_load_explicit(&d->atoms, memory_order_relaxed);
    atoms[count] = s;
    atomic_store_explicit(&d->count, count + 1, memory_order_release);

    uint64_t mask = d->bucket_mask;
    uint64_t slot = h & mask;
    while (d->buckets[slot] != 0) slot = (slot + 1) & mask;
    d->buckets[slot] = ((uint64_t)h << 32) | ((uint64_t)(uint32_t)count + 1);

    /* The published LUT (if any) no longer covers the vocabulary: retire
     * it; the next ray_sym_domain_runtime_lut rebuilds over the grown
     * set.  Retire failure (OOM) keeps the stale LUT published — that
     * would be a correctness hole, so treat it as append failure...
     * except the append IS already published.  Instead leak-by-keeping:
     * dom_retire only fails on a 16-byte malloc after we already did
     * larger ones — practically unreachable; fall back to NOT clearing
     * and accept the stale LUT being rebuilt on next miss is impossible,
     * so abort loudly in that corner. */
    int64_t* lut = atomic_load_explicit(&d->runtime_lut, memory_order_relaxed);
    if (lut) {
        if (!dom_retire(d, lut)) {
            fprintf(stderr, "rayforce: sym domain '%s': OOM retiring runtime "
                            "LUT after append — cannot keep id translation "
                            "consistent\n", d->path ? d->path : "?");
            abort();
        }
        atomic_store_explicit(&d->runtime_lut, NULL, memory_order_release);
    }

    return count;
}

int64_t ray_sym_domain_intern(ray_sym_domain_t* dom, const char* str, size_t len) {
    if (!dom || !str) return -1;
    if (dom->kind == DOM_RUNTIME) return ray_sym_intern(str, len);

    dom_lock();
    if (!dom->buckets && !dom_build_index_locked(dom, 1)) {
        dom_unlock();
        return -1;
    }

    /* Position-0 reservation: the first symbol of a brand-new domain is
     * always "" (the SYM null), even when the caller interns something
     * else first. */
    if (atomic_load_explicit(&dom->count, memory_order_relaxed) == 0 &&
        len != 0) {
        uint32_t h0 = (uint32_t)ray_hash_bytes("", 0);
        if (dom_append_locked(dom, h0, "", 0) != 0) {
            dom_unlock();
            return -1;
        }
    }

    uint32_t h = (uint32_t)ray_hash_bytes(str, len);
    int64_t pos = dom_probe_locked(dom, h, str, len);
    if (pos < 0) pos = dom_append_locked(dom, h, str, len);
    dom_unlock();
    return pos;
}

int64_t ray_sym_domain_count(ray_sym_domain_t* dom) {
    if (!dom) return 0;
    if (dom->kind == DOM_RUNTIME) return (int64_t)ray_sym_count();
    return atomic_load_explicit(&dom->count, memory_order_acquire);
}

const char* ray_sym_domain_path(ray_sym_domain_t* dom) {
    if (!dom || dom->kind == DOM_RUNTIME) return NULL;
    return dom->path;
}

/* ---- flush ------------------------------------------------------------------ */

ray_err_t ray_sym_domain_flush(ray_sym_domain_t* dom, bool durable) {
    if (!dom) return RAY_ERR_TYPE;
    if (dom->kind == DOM_RUNTIME) return RAY_OK; /* sym.c owns its own files */

    dom_lock();
    int64_t count = atomic_load_explicit(&dom->count, memory_order_relaxed);
    ray_t** atoms = atomic_load_explicit(&dom->atoms, memory_order_relaxed);
    int64_t disk_count = dom->disk_count;
    size_t  disk_size  = dom->disk_size;
    dom_unlock();

    if (count == disk_count) return RAY_OK; /* nothing new */

    char lock_path[1024];
    char tmp_path[1024];
    if (snprintf(lock_path, sizeof(lock_path), "%s.lk", dom->path) >= (int)sizeof(lock_path))
        return RAY_ERR_IO;
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dom->path) >= (int)sizeof(tmp_path))
        return RAY_ERR_IO;

    ray_fd_t lock_fd = ray_file_open(lock_path,
                                     RAY_OPEN_READ | RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    if (lock_fd == RAY_FD_INVALID) return RAY_ERR_IO;
    ray_err_t err = ray_file_lock_ex(lock_fd);
    if (err != RAY_OK) { ray_file_close(lock_fd); return err; }

    /* Verify-base-unchanged: the file on disk must be EXACTLY what this
     * object last persisted (size + header count).  In-memory interning
     * assigns positions speculatively, so any divergence means a second
     * writer raced us — hard error, never a silent remap. */
    {
        struct stat st;
        bool exists = (stat(dom->path, &st) == 0);
        if (disk_count == 0) {
            if (exists && st.st_size > 0) {
                ray_file_unlock(lock_fd);
                ray_file_close(lock_fd);
                return RAY_ERR_CORRUPT; /* concurrent writer created it */
            }
        } else {
            bool ok = exists && (size_t)st.st_size == disk_size;
            if (ok) {
                FILE* vf = fopen(dom->path, "rb");
                uint32_t magic = 0;
                int64_t fcount = -1;
                if (!vf || fread(&magic, 4, 1, vf) != 1 ||
                    fread(&fcount, 8, 1, vf) != 1 ||
                    magic != DOMAIN_STRL_MAGIC || fcount != disk_count)
                    ok = false;
                if (vf) fclose(vf);
            }
            if (!ok) {
                ray_file_unlock(lock_fd);
                ray_file_close(lock_fd);
                return RAY_ERR_CORRUPT; /* concurrent writer */
            }
        }
    }

    /* Write base + tail to tmp. */
    size_t written_size = 0;
    {
        FILE* f = fopen(tmp_path, "wb");
        if (!f) {
            ray_file_unlock(lock_fd);
            ray_file_close(lock_fd);
            return RAY_ERR_IO;
        }
        uint32_t magic = DOMAIN_STRL_MAGIC;
        err = RAY_OK;
        if (fwrite(&magic, 4, 1, f) != 1 || fwrite(&count, 8, 1, f) != 1)
            err = RAY_ERR_IO;
        written_size = 12;
        for (int64_t i = 0; err == RAY_OK && i < count; i++) {
            ray_t* s = atoms[i];
            size_t slen = ray_str_len(s);
            if (slen > UINT32_MAX) { err = RAY_ERR_RANGE; break; }
            uint32_t len32 = (uint32_t)slen;
            if (fwrite(&len32, 4, 1, f) != 1 ||
                (slen > 0 && fwrite(ray_str_ptr(s), 1, slen, f) != slen)) {
                err = RAY_ERR_IO;
                break;
            }
            written_size += 4 + slen;
        }
        if (fclose(f) != 0 && err == RAY_OK) err = RAY_ERR_IO;
    }
    if (err != RAY_OK) goto fail_tmp;

    if (durable) {
        ray_fd_t tmp_fd = ray_file_open(tmp_path, RAY_OPEN_READ | RAY_OPEN_WRITE);
        if (tmp_fd == RAY_FD_INVALID) { err = RAY_ERR_IO; goto fail_tmp; }
        err = ray_file_sync(tmp_fd);
        ray_file_close(tmp_fd);
        if (err != RAY_OK) goto fail_tmp;
    }

    err = ray_file_rename(tmp_path, dom->path);
    if (err != RAY_OK) goto fail_tmp;

    if (durable) {
        err = ray_file_sync_dir(dom->path);
        if (err != RAY_OK) {
            ray_file_unlock(lock_fd);
            ray_file_close(lock_fd);
            return err;
        }
    }

    dom_lock();
    dom->disk_count = count;
    dom->disk_size = written_size;
    dom_unlock();

    ray_file_unlock(lock_fd);
    ray_file_close(lock_fd);
    return RAY_OK;

fail_tmp:
    remove(tmp_path);
    ray_file_unlock(lock_fd);
    ray_file_close(lock_fd);
    return err;
}

/* ---- RAY_SYM_AUDIT ----------------------------------------------------------- */

/* Cached at ray_sym_init (table/sym.c) from the RAY_SYM_AUDIT env var. */
uint8_t ray_g_sym_audit = 0;

void ray_sym_audit_cell(ray_t* vec, int64_t row, int64_t pos, ray_t* resolved) {
    ray_sym_domain_t* dom = ray_sym_vec_domain(vec);
    int64_t count = ray_sym_domain_count(dom);
    if (resolved != NULL && pos >= 0 && pos < count) return;

    fprintf(stderr,
            "rayforce: RAY_SYM_AUDIT violation: vec=%p type=%d attrs=0x%02x "
            "len=%lld row=%lld pos=%lld domain=%p (%s) count=%lld resolved=%p\n",
            (void*)vec, (int)vec->type, (unsigned)vec->attrs,
            (long long)vec->len, (long long)row, (long long)pos,
            (void*)dom,
            dom == ray_sym_runtime_domain() ? "runtime"
                : (ray_sym_domain_path(dom) ? ray_sym_domain_path(dom) : "file"),
            (long long)count, (void*)resolved);
    abort();
}
