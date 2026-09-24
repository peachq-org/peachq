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

#include "sym.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "mem/arena.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "ops/hash.h"

/* --------------------------------------------------------------------------
 * Symbol table structure (static global, sequential mode only).
 * NOT thread-safe: all interning must happen before ray_parallel_begin().
 * -------------------------------------------------------------------------- */

#define SYM_INIT_CAP     256
#define SYM_LOAD_FACTOR  0.7

/* Cached segment list for a dotted sym: nsegs sym_ids that together make up
 * the dotted path.  segs is arena-allocated (same lifetime as sym table). */
typedef struct {
    uint8_t  nsegs;
    int64_t* segs;   /* length nsegs; NULL for non-dotted entries */
} sym_segs_t;

typedef struct {
    /* Hash table: each bucket stores (hash32 << 32) | (id + 1), 0 = empty */
    uint64_t*  buckets;
    uint32_t   bucket_cap;   /* always power of 2 */

    /* String array: strings[id] = ray_t* string atom */
    ray_t**     strings;
    uint32_t   str_count;
    uint32_t   str_cap;

    /* Per-sym dotted-path metadata, parallel to strings[].
     * `dotted` is a bitmap (1 bit per sym_id); bit set = name is dotted
     *   and segment sym_ids are cached in `segments`.
     * `scanned` is a bitmap; bit set = sym_cache_segments has settled this
     *   sym (either cached successfully, or decided it is a plain name).
     *   Unset = needs to be (re-)scanned on the next intern call, which is
     *   how we recover from a transient cache OOM on first intern: the
     *   bit stays clear, so future interns of the same name retry.
     * `segments` holds cached segment sym_ids; segs == NULL when dotted
     *   bit is clear. */
    uint64_t*   dotted;       /* (str_cap + 63) / 64 words */
    uint64_t*   scanned;      /* (str_cap + 63) / 64 words */
    sym_segs_t* segments;     /* length str_cap */

    /* Arena for string atoms — avoids per-string buddy allocator calls */
    ray_arena_t*  arena;
} sym_table_t;

static sym_table_t g_sym;
static _Atomic(bool) g_sym_inited = false;

/* Spinlock protecting g_sym mutations in ray_sym_intern */
static _Atomic(int) g_sym_lock = 0;
static inline void sym_lock(void) {
    while (atomic_exchange_explicit(&g_sym_lock, 1, memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}
static inline void sym_unlock(void) {
    atomic_store_explicit(&g_sym_lock, 0, memory_order_release);
}

/* String atoms for the global sym table live in its arena (off the per-thread
 * buddy heap) — see ray_arena_str in mem/arena.c. */

/* Forward decl — used from ray_sym_init below to reserve sym ID 0 as
 * the canonical empty string.  Definition is further down with the
 * other intern helpers. */
static int64_t sym_intern_nolock(uint32_t hash, const char* str, size_t len);

/* --------------------------------------------------------------------------
 * ray_sym_init
 * -------------------------------------------------------------------------- */

ray_err_t ray_sym_init(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&g_sym_inited, &expected, true,
            memory_order_acq_rel, memory_order_acquire))
        return RAY_OK; /* already initialized by another thread */

    /* RAY_SYM_AUDIT: cache the env check once (the hot ray_sym_vec_cell
     * inline branches on the cached byte, never on getenv). */
    ray_g_sym_audit = getenv("RAY_SYM_AUDIT") != NULL ? 1 : 0;

    g_sym.bucket_cap = SYM_INIT_CAP;
    /* ray_sys_alloc uses mmap(MAP_ANONYMOUS) which zero-initializes. */
    g_sym.buckets = (uint64_t*)ray_sys_alloc(g_sym.bucket_cap * sizeof(uint64_t));
    if (!g_sym.buckets) {
        atomic_store_explicit(&g_sym_inited, false, memory_order_release);
        return RAY_ERR_OOM;
    }

    g_sym.str_cap = SYM_INIT_CAP;
    g_sym.str_count = 0;
    g_sym.strings = (ray_t**)ray_sys_alloc(g_sym.str_cap * sizeof(ray_t*));
    if (!g_sym.strings) {
        ray_sys_free(g_sym.buckets);
        g_sym.buckets = NULL;
        atomic_store_explicit(&g_sym_inited, false, memory_order_release);
        return RAY_ERR_OOM;
    }

    g_sym.arena = ray_arena_new(1024 * 1024);  /* 1MB chunks */
    if (!g_sym.arena) {
        ray_sys_free(g_sym.strings);
        ray_sys_free(g_sym.buckets);
        g_sym.strings = NULL;
        g_sym.buckets = NULL;
        atomic_store_explicit(&g_sym_inited, false, memory_order_release);
        return RAY_ERR_OOM;
    }

    /* Dotted-path sidecars sized to str_cap.  ray_sys_alloc is MAP_ANONYMOUS
     * so memory is zero-initialised — bitmaps start all-zero, segments[i]
     * structs start {nsegs:0, segs:NULL}.  Failures free prior allocations
     * and roll the sym table back to uninitialised. */
    uint32_t bm_words = (g_sym.str_cap + 63) / 64;
    g_sym.dotted = (uint64_t*)ray_sys_alloc((size_t)bm_words * sizeof(uint64_t));
    g_sym.scanned = (uint64_t*)ray_sys_alloc((size_t)bm_words * sizeof(uint64_t));
    g_sym.segments = (sym_segs_t*)ray_sys_alloc((size_t)g_sym.str_cap * sizeof(sym_segs_t));
    if (!g_sym.dotted || !g_sym.scanned || !g_sym.segments) {
        if (g_sym.dotted) ray_sys_free(g_sym.dotted);
        if (g_sym.scanned) ray_sys_free(g_sym.scanned);
        if (g_sym.segments) ray_sys_free(g_sym.segments);
        g_sym.dotted = NULL;
        g_sym.scanned = NULL;
        g_sym.segments = NULL;
        ray_arena_destroy(g_sym.arena);
        g_sym.arena = NULL;
        ray_sys_free(g_sym.strings);
        ray_sys_free(g_sym.buckets);
        g_sym.strings = NULL;
        g_sym.buckets = NULL;
        atomic_store_explicit(&g_sym_inited, false, memory_order_release);
        return RAY_ERR_OOM;
    }

    /* Reserve sym ID 0 as the empty string.  This makes the empty
     * symbol the canonical "no value" representation for SYM columns:
     * a missing CSV cell, a null-marked SYM atom, and an explicit ""
     * literal all collapse to ID 0.  SYM columns therefore never need
     * a parallel null bitmap — RAY_ATTR_HAS_NULLS is structurally
     * meaningless on SYM and is rejected on set.  Done before
     * returning so every subsequent intern observes ID 0 as taken. */
    int64_t empty_id = sym_intern_nolock(
        (uint32_t)ray_hash_bytes("", 0), "", 0);
    if (empty_id != 0) {
        /* Should be unreachable — table just initialised, no other
         * thread has touched it yet.  If it ever fires, fail loudly. */
        ray_arena_destroy(g_sym.arena);
        ray_sys_free(g_sym.segments);
        ray_sys_free(g_sym.scanned);
        ray_sys_free(g_sym.dotted);
        ray_sys_free(g_sym.strings);
        ray_sys_free(g_sym.buckets);
        g_sym.arena = NULL;
        g_sym.segments = NULL; g_sym.scanned = NULL; g_sym.dotted = NULL;
        g_sym.strings = NULL; g_sym.buckets = NULL;
        atomic_store_explicit(&g_sym_inited, false, memory_order_release);
        return RAY_ERR_OOM;
    }

    /* g_sym_inited already set to true by CAS above */
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * ray_sym_destroy
 * -------------------------------------------------------------------------- */

void ray_sym_destroy(void) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return;

    /* Arena-backed strings: ray_release is a no-op (RAY_ATTR_ARENA).
     * Destroy the arena to free all string atoms at once.
     * segments[i].segs pointers are arena-allocated too, freed with it. */
    if (g_sym.arena) {
        ray_arena_destroy(g_sym.arena);
        g_sym.arena = NULL;
    }

    if (g_sym.segments) ray_sys_free(g_sym.segments);
    if (g_sym.scanned)  ray_sys_free(g_sym.scanned);
    if (g_sym.dotted)   ray_sys_free(g_sym.dotted);
    ray_sys_free(g_sym.strings);
    ray_sys_free(g_sym.buckets);

    memset(&g_sym, 0, sizeof(g_sym));
    atomic_store_explicit(&g_sym_inited, false, memory_order_release);
}

/* --------------------------------------------------------------------------
 * Hash table helpers
 * -------------------------------------------------------------------------- */

static void ht_insert(uint64_t* buckets, uint32_t cap, uint32_t hash, uint32_t id) {
    uint32_t mask = cap - 1;
    uint32_t slot = hash & mask;
    uint64_t entry = ((uint64_t)hash << 32) | ((uint64_t)(id + 1));

    for (;;) {
        if (buckets[slot] == 0) {
            buckets[slot] = entry;
            return;
        }
        slot = (slot + 1) & mask;
    }
}

/* Grow hash table to new_cap (must be power of 2 and > current cap). */
static bool ht_grow_to(uint32_t new_cap) {
    uint64_t* new_buckets = (uint64_t*)ray_sys_alloc((size_t)new_cap * sizeof(uint64_t));
    if (!new_buckets) return false;

    /* Re-insert all existing entries */
    for (uint32_t i = 0; i < g_sym.bucket_cap; i++) {
        uint64_t e = g_sym.buckets[i];
        if (e == 0) continue;
        uint32_t h = (uint32_t)(e >> 32);
        uint32_t id = (uint32_t)(e & 0xFFFFFFFF) - 1;
        ht_insert(new_buckets, new_cap, h, id);
    }

    ray_sys_free(g_sym.buckets);
    g_sym.buckets = new_buckets;
    g_sym.bucket_cap = new_cap;
    return true;
}

static bool ht_grow(void) {
    /* Overflow guard: bucket_cap is always power of 2.
     * At 2^31, doubling overflows uint32_t. */
    if (g_sym.bucket_cap >= (UINT32_MAX / 2 + 1)) return false;
    return ht_grow_to(g_sym.bucket_cap * 2);
}

/* --------------------------------------------------------------------------
 * sym_grow_str_cap — grow strings[], dotted[] bitmap, and segments[] array
 * to hold at least new_cap entries.  Must be called with sym_lock held
 * (or from within single-threaded prehashed intern).  Zero-fills the new
 * portion of segments[] explicitly (realloc of a mapped region may return
 * pages that weren't touched but we don't want to rely on virgin mmap).
 * -------------------------------------------------------------------------- */
static bool sym_grow_str_cap(uint32_t new_cap) {
    uint32_t old_cap = g_sym.str_cap;
    if (new_cap <= old_cap) return true;

    ray_t** new_strings = (ray_t**)ray_sys_realloc(g_sym.strings,
                                                   (size_t)new_cap * sizeof(ray_t*));
    if (!new_strings) return false;
    memset(new_strings + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(ray_t*));
    g_sym.strings = new_strings;

    uint32_t old_bm_words = (old_cap + 63) / 64;
    uint32_t new_bm_words = (new_cap + 63) / 64;
    if (new_bm_words > old_bm_words) {
        uint64_t* new_dotted = (uint64_t*)ray_sys_realloc(g_sym.dotted,
                                                          (size_t)new_bm_words * sizeof(uint64_t));
        if (!new_dotted) return false;
        memset(new_dotted + old_bm_words, 0,
               (size_t)(new_bm_words - old_bm_words) * sizeof(uint64_t));
        g_sym.dotted = new_dotted;

        uint64_t* new_scanned = (uint64_t*)ray_sys_realloc(g_sym.scanned,
                                                           (size_t)new_bm_words * sizeof(uint64_t));
        if (!new_scanned) return false;
        memset(new_scanned + old_bm_words, 0,
               (size_t)(new_bm_words - old_bm_words) * sizeof(uint64_t));
        g_sym.scanned = new_scanned;
    }

    sym_segs_t* new_segments = (sym_segs_t*)ray_sys_realloc(g_sym.segments,
                                                            (size_t)new_cap * sizeof(sym_segs_t));
    if (!new_segments) return false;
    memset(new_segments + old_cap, 0,
           (size_t)(new_cap - old_cap) * sizeof(sym_segs_t));
    g_sym.segments = new_segments;

    g_sym.str_cap = new_cap;
    return true;
}

/* Forward declarations — sym_cache_segments (below) needs these helpers
 * that are defined further down in the file.  ray_sym_bytes_upper is
 * declared in sym.h as a public inline so both the intern path and the
 * test suite can refer to the same formula. */
static int64_t sym_intern_nolock(uint32_t hash, const char* str, size_t len);
static int64_t sym_probe(uint32_t hash, const char* str, size_t len);
static int64_t sym_commit_new(uint32_t hash, const char* str, size_t len);
static bool    sym_reserve_capacity(uint32_t new_sym_count, size_t arena_bytes);

/* --------------------------------------------------------------------------
 * sym_cache_segments — idempotent cache-and-apply for an EXISTING sym.
 * Used by the probe-found branch of sym_intern_nolock, to retry a cache
 * that an earlier intern could not build (OOM left `scanned` clear).
 *
 * Atomic: same inspect + reserve + commit pattern as sym_intern_nolock,
 * so a failure here leaves no orphan segment syms and no half-applied
 * cache state.  Returns false only on real OOM — scanned stays clear
 * in that case so future retries pick up where we left off.
 * -------------------------------------------------------------------------- */
static bool sym_cache_segments(uint32_t new_id, const char* str, size_t len) {
    uint64_t bit = (uint64_t)1 << (new_id & 63);
    uint32_t word = new_id >> 6;
    if (g_sym.scanned[word] & bit) return true;

    const char* first_dot = (const char*)memchr(str, '.', len);
    if (!first_dot) {
        /* Plain — mark settled. */
        g_sym.scanned[word] |= bit;
        return true;
    }

    /* Validate structure.  Trailing dot → not dotted.  Leading `.` is
     * allowed ONLY when followed by another dot (e.g. `.sys.gc`) —
     * in that case segment 0 includes the leading dot (`.sys`), so
     * reserved-namespace names resolve against their root dict via
     * the regular segment walk. */
    if (str[len - 1] == '.') {
        g_sym.scanned[word] |= bit;
        return true;
    }
    bool leading_dot = (str[0] == '.');
    if (leading_dot) {
        /* `.sys` alone (no second dot) is a plain name. */
        const char* second = (const char*)memchr(str + 1, '.', len - 1);
        if (!second) { g_sym.scanned[word] |= bit; return true; }
    }
    size_t sep_dots = 0;
    for (size_t i = (leading_dot ? 1 : 0); i < len; i++)
        if (str[i] == '.') sep_dots++;
    if (sep_dots + 1 > 255) {
        g_sym.scanned[word] |= bit;
        return true;
    }
    uint8_t nsegs = (uint8_t)(sep_dots + 1);

    struct { const char* p; size_t len; uint32_t hash; int64_t id; } descs[256];
    uint32_t new_seg_count = 0;
    size_t   new_seg_bytes = 0;
    {
        const char* p = str;
        size_t remaining = len;
        uint8_t i = 0;
        while (remaining && i < nsegs) {
            /* Segment 0 starts at str[0] but skips the leading `.` when
             * searching for the segment-terminating dot — so seg 0 of
             * `.sys.gc` is `.sys`, not `` (empty). */
            size_t skip = (i == 0 && leading_dot) ? 1 : 0;
            const char* dot = remaining > skip
                ? (const char*)memchr(p + skip, '.', remaining - skip)
                : NULL;
            size_t seg_len = dot ? (size_t)(dot - p) : remaining;
            if (seg_len == 0) { g_sym.scanned[word] |= bit; return true; }
            uint32_t h = (uint32_t)ray_hash_bytes(p, seg_len);
            descs[i].p    = p;
            descs[i].len  = seg_len;
            descs[i].hash = h;
            descs[i].id   = sym_probe(h, p, seg_len);
            if (descs[i].id < 0) {
                new_seg_count++;
                new_seg_bytes += ray_sym_bytes_upper(seg_len);
            }
            i++;
            if (!dot) break;
            remaining -= (seg_len + 1);
            p = dot + 1;
        }
    }

    /* Reserve capacity for new segments + segs array. */
    size_t segs_payload = (size_t)nsegs * sizeof(int64_t);
    size_t arena_bytes  = new_seg_bytes +
                          (((size_t)32 + segs_payload + 31) & ~(size_t)31);
    if (!sym_reserve_capacity(new_seg_count, arena_bytes)) return false;

    /* Commit.  Allocations covered by reservation above. */
    for (uint8_t i = 0; i < nsegs; i++) {
        if (descs[i].id < 0) {
            int64_t sid = sym_commit_new(descs[i].hash, descs[i].p, descs[i].len);
            if (sid < 0) return false;   /* reservation should have prevented */
            descs[i].id = sid;
            g_sym.scanned[sid >> 6] |= ((uint64_t)1 << (sid & 63));
        }
    }

    int64_t* segs = (int64_t*)ray_arena_alloc(g_sym.arena, segs_payload);
    if (!segs) return false;             /* reservation should have prevented */
    for (uint8_t i = 0; i < nsegs; i++) segs[i] = descs[i].id;

    g_sym.segments[new_id].nsegs = nsegs;
    g_sym.segments[new_id].segs  = segs;
    g_sym.dotted[word]  |= bit;
    g_sym.scanned[word] |= bit;
    return true;
}

/* --------------------------------------------------------------------------
 * sym_probe — hash-table lookup only.  Returns sym_id for an existing
 * entry or -1 if not present.  No side effects.
 * -------------------------------------------------------------------------- */
static int64_t sym_probe(uint32_t hash, const char* str, size_t len) {
    uint32_t mask = g_sym.bucket_cap - 1;
    uint32_t slot = hash & mask;
    for (;;) {
        uint64_t e = g_sym.buckets[slot];
        if (e == 0) return -1;
        uint32_t e_hash = (uint32_t)(e >> 32);
        if (e_hash == hash) {
            uint32_t e_id = (uint32_t)(e & 0xFFFFFFFF) - 1;
            ray_t* existing = g_sym.strings[e_id];
            if (ray_str_len(existing) == len &&
                memcmp(ray_str_ptr(existing), str, len) == 0) {
                return (int64_t)e_id;
            }
        }
        slot = (slot + 1) & mask;
    }
}

/* --------------------------------------------------------------------------
 * sym_commit_new — insert a NEW sym (caller must have confirmed it does
 * not already exist).  Grows the hash/strings tables as needed, allocates
 * the string atom from the arena, inserts into the hash table.  Returns
 * new sym_id or -1 on OOM.  No cache side effect.
 * -------------------------------------------------------------------------- */
static int64_t sym_commit_new(uint32_t hash, const char* str, size_t len) {
    /* Grow hash table if load factor exceeds threshold, or if critically
     * full.  Attempt grow before refusing insert.
     * Cast to uint64_t to prevent overflow when bucket_cap >= 2^26. */
    if ((uint64_t)g_sym.str_count * 100 >= (uint64_t)g_sym.bucket_cap * 70) {
        if (!ht_grow()) {
            /* If critically full even after failed grow, refuse insert
             * to prevent infinite probe loops. */
            if ((uint64_t)g_sym.str_count * 100 >= (uint64_t)g_sym.bucket_cap * 95) {
                return -1;
            }
        }
    }

    uint32_t new_id = g_sym.str_count;

    if (new_id >= g_sym.str_cap) {
        if (g_sym.str_cap >= UINT32_MAX / 2) return -1;
        if (!sym_grow_str_cap(g_sym.str_cap * 2)) return -1;
    }

    /* Create string atom from arena — avoids buddy allocator overhead.
     * Arena blocks have rc=1 and RAY_ATTR_ARENA set. */
    ray_t* s = ray_arena_str(g_sym.arena, str, len);
    if (!s) return -1;
    g_sym.strings[new_id] = s;
    g_sym.str_count++;

    /* Insert into hash table.
     * Note: ht_insert probes from hash & mask to find an empty slot,
     * so it works correctly even if ht_grow changed the bucket array. */
    ht_insert(g_sym.buckets, g_sym.bucket_cap, hash, new_id);

    return (int64_t)new_id;
}

/* Reserve hash-table, strings-array, and arena capacity for `new_sym_count`
 * new syms plus `arena_bytes` of additional arena usage (for the segs array
 * if we're interning a dotted name).  Returns true on success; on failure
 * returns false with no commits made. */
static bool sym_reserve_capacity(uint32_t new_sym_count, size_t arena_bytes) {
    /* Hash table — grow if adding new_sym_count entries would exceed 70%. */
    uint64_t new_count = (uint64_t)g_sym.str_count + new_sym_count;
    uint32_t target = g_sym.bucket_cap;
    while (new_count * 100 >= (uint64_t)target * 70) {
        if (target >= (UINT32_MAX / 2 + 1)) return false;
        target *= 2;
    }
    if (target > g_sym.bucket_cap) {
        if (!ht_grow_to(target)) return false;
    }

    /* Strings and sidecars. */
    if (new_count > g_sym.str_cap) {
        uint32_t str_target = g_sym.str_cap;
        while (str_target < new_count) {
            if (str_target >= UINT32_MAX / 2) return false;
            str_target *= 2;
        }
        if (!sym_grow_str_cap(str_target)) return false;
    }

    /* Arena — reserve one chunk large enough for every forthcoming alloc. */
    if (arena_bytes && !ray_arena_reserve(g_sym.arena, arena_bytes)) return false;

    return true;
}

/* --------------------------------------------------------------------------
 * sym_intern_nolock — fully atomic intern.
 *
 * Three phases:
 *   A. Inspect: probe the main name, validate its dotted shape, probe
 *      every segment.  No side effects.
 *   B. Reserve: pre-grow hash/strings/arena to accommodate everything
 *      we might need to commit.  Can fail → return -1 with no state
 *      change (no orphan segment syms, no cache fragments).
 *   C. Commit: all allocations in this phase are guaranteed by the
 *      reservations above, so they cannot fail.  Creates any new
 *      segment syms, creates the main sym, fills the segs cache, sets
 *      scanned + dotted bits.
 *
 * This closes two prior traps:
 *  - A committed main sym whose dotted bit disagrees with its name's
 *    structure (env silently routing dotted-path writes/reads through
 *    the flat path).
 *  - Orphan segment syms persisting when the main-sym commit fails.
 *
 * For an existing sym found in phase A, we still opportunistically try
 * the cache (an earlier cache-OOM left it unsettled).  A cache-OOM there
 * is tolerated (scanned bit stays clear → future interns retry).
 * -------------------------------------------------------------------------- */
static int64_t sym_intern_nolock(uint32_t hash, const char* str, size_t len) {
    /* Phase A.1: probe main. */
    int64_t existing = sym_probe(hash, str, len);
    if (existing >= 0) {
        (void)sym_cache_segments((uint32_t)existing, str, len);
        return existing;
    }

    /* Phase A.2: structural validation + per-segment probe. */
    struct { const char* p; size_t len; uint32_t hash; int64_t id; } descs[256];
    uint8_t  nsegs = 0;
    uint32_t new_seg_count = 0;
    size_t   new_seg_bytes = 0;
    bool     is_dotted = false;

    const char* first_dot = (const char*)memchr(str, '.', len);
    if (first_dot) {
        /* Dotted-name rules (parallel to sym_cache_segments):
         *   - Trailing dot            → plain (not dotted).
         *   - Leading dot alone       → plain (`.sys` with no inner dot).
         *   - Leading dot + inner dot → segment 0 is `.<head>` including
         *                                the leading dot.  This is how
         *                                reserved-namespace names like
         *                                `.sys.gc` resolve against the
         *                                `.sys` root dict. */
        bool valid = str[len - 1] != '.';
        bool leading_dot = (str[0] == '.');
        if (valid && leading_dot) {
            const char* second = (const char*)memchr(str + 1, '.', len - 1);
            if (!second) valid = false;
        }
        size_t sep_dots = 0;
        if (valid) {
            for (size_t i = (leading_dot ? 1 : 0); i < len; i++)
                if (str[i] == '.') sep_dots++;
            if (sep_dots + 1 > 255) valid = false;
        }
        if (valid) {
            nsegs = (uint8_t)(sep_dots + 1);
            const char* p = str;
            size_t remaining = len;
            uint8_t i = 0;
            while (remaining && i < nsegs) {
                size_t skip = (i == 0 && leading_dot) ? 1 : 0;
                const char* dot = remaining > skip
                    ? (const char*)memchr(p + skip, '.', remaining - skip)
                    : NULL;
                size_t seg_len = dot ? (size_t)(dot - p) : remaining;
                if (seg_len == 0) { valid = false; break; }
                uint32_t seg_hash = (uint32_t)ray_hash_bytes(p, seg_len);
                descs[i].p    = p;
                descs[i].len  = seg_len;
                descs[i].hash = seg_hash;
                descs[i].id   = sym_probe(seg_hash, p, seg_len);
                if (descs[i].id < 0) {
                    new_seg_count++;
                    new_seg_bytes += ray_sym_bytes_upper(seg_len);
                }
                i++;
                if (!dot) break;
                remaining -= (seg_len + 1);
                p = dot + 1;
            }
            if (valid) is_dotted = true;
        }
    }

    /* Phase B: reserve capacity for main + new segments + segs array. */
    size_t arena_bytes = ray_sym_bytes_upper(len);
    if (is_dotted) {
        arena_bytes += new_seg_bytes;
        /* segs array is arena-allocated via ray_arena_alloc(_, nsegs*8). */
        size_t segs_payload = (size_t)nsegs * sizeof(int64_t);
        arena_bytes += ((size_t)32 + segs_payload + 31) & ~(size_t)31;
    }
    if (!sym_reserve_capacity(1 + new_seg_count, arena_bytes)) return -1;

    /* Phase C: commit.  Every allocation below is covered by the
     * reservation above, so nothing here can fail. */
    if (is_dotted) {
        for (uint8_t i = 0; i < nsegs; i++) {
            if (descs[i].id < 0) {
                int64_t sid = sym_commit_new(descs[i].hash, descs[i].p, descs[i].len);
                /* Reservation guarantees success; defensive check kept. */
                if (sid < 0) return -1;
                descs[i].id = sid;
                /* Segment is itself a plain name (no dot inside). */
                g_sym.scanned[sid >> 6] |= ((uint64_t)1 << (sid & 63));
            }
        }
    }

    int64_t main_id = sym_commit_new(hash, str, len);
    if (main_id < 0) return -1;

    if (is_dotted) {
        int64_t* segs = (int64_t*)ray_arena_alloc(g_sym.arena,
                                                  (size_t)nsegs * sizeof(int64_t));
        if (!segs) return main_id;   /* reservation should have prevented this */
        for (uint8_t i = 0; i < nsegs; i++) segs[i] = descs[i].id;
        g_sym.segments[main_id].nsegs = nsegs;
        g_sym.segments[main_id].segs  = segs;
        g_sym.dotted[main_id >> 6] |= ((uint64_t)1 << (main_id & 63));
    }
    g_sym.scanned[main_id >> 6] |= ((uint64_t)1 << (main_id & 63));

    return main_id;
}

/* --------------------------------------------------------------------------
 * ray_sym_intern — locked public API
 * -------------------------------------------------------------------------- */

int64_t ray_sym_intern(const char* str, size_t len) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return -1;
    uint32_t hash = (uint32_t)ray_hash_bytes(str, len);
    sym_lock();
    int64_t id = sym_intern_nolock(hash, str, len);
    sym_unlock();
    return id;
}

int64_t ray_sym_intern_runtime(const char* str, size_t len) {
    return ray_sym_intern(str, len);
}

/* --------------------------------------------------------------------------
 * ray_sym_intern_prehashed -- intern with pre-computed hash, no lock.
 *
 * CALLER CONTRACT: must only be called when no other thread is interning
 * (e.g., after ray_pool_dispatch returns during CSV merge).
 * -------------------------------------------------------------------------- */

int64_t ray_sym_intern_prehashed(uint32_t hash, const char* str, size_t len) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return -1;
    return sym_intern_nolock(hash, str, len);
}

/* --------------------------------------------------------------------------
 * Dotted-name accessors
 * -------------------------------------------------------------------------- */

bool ray_sym_is_dotted(int64_t sym_id) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return false;
    if (sym_id < 0 || (uint32_t)sym_id >= g_sym.str_count) return false;
    uint64_t word = g_sym.dotted[(uint32_t)sym_id >> 6];
    return (word >> ((uint32_t)sym_id & 63)) & 1;
}

int ray_sym_segs(int64_t sym_id, const int64_t** out_segs) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return 0;
    if (sym_id < 0 || (uint32_t)sym_id >= g_sym.str_count) return 0;
    sym_segs_t s = g_sym.segments[sym_id];
    if (s.nsegs == 0 || !s.segs) return 0;
    if (out_segs) *out_segs = s.segs;
    return (int)s.nsegs;
}

/* --------------------------------------------------------------------------
 * ray_sym_find
 * -------------------------------------------------------------------------- */

int64_t ray_sym_find(const char* str, size_t len) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return -1;

    /* Lock required: concurrent ray_sym_intern may trigger ht_grow which
     * frees and replaces g_sym.buckets -- reading without lock is UAF. */
    sym_lock();

    uint32_t hash = (uint32_t)ray_hash_bytes(str, len);
    uint32_t mask = g_sym.bucket_cap - 1;
    uint32_t slot = hash & mask;

    for (;;) {
        uint64_t e = g_sym.buckets[slot];
        if (e == 0) { sym_unlock(); return -1; }  /* empty -- not found */

        uint32_t e_hash = (uint32_t)(e >> 32);
        if (e_hash == hash) {
            uint32_t e_id = (uint32_t)(e & 0xFFFFFFFF) - 1;
            ray_t* existing = g_sym.strings[e_id];
            if (ray_str_len(existing) == len &&
                memcmp(ray_str_ptr(existing), str, len) == 0) {
                sym_unlock();
                return (int64_t)e_id;
            }
        }
        slot = (slot + 1) & mask;
    }
}

/* --------------------------------------------------------------------------
 * ray_sym_str
 * -------------------------------------------------------------------------- */

/* Returned pointer is valid only while no concurrent ray_sym_intern occurs.
 * Safe during read-only execution phase (after all interning is complete).
 * Caller must not store the pointer across sym table mutations (ht_grow
 * or strings realloc). */
ray_t* ray_sym_str(int64_t id) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return NULL;

    /* Lock required: concurrent ray_sym_intern may realloc g_sym.strings. */
    sym_lock();
    if (id < 0 || (uint32_t)id >= g_sym.str_count) { sym_unlock(); return NULL; }
    ray_t* s = g_sym.strings[id];
    sym_unlock();
    return s;
}

/* --------------------------------------------------------------------------
 * ray_sym_count
 * -------------------------------------------------------------------------- */

uint32_t ray_sym_count(void) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return 0;

    /* Lock required: concurrent ray_sym_intern may modify str_count. */
    sym_lock();
    uint32_t count = g_sym.str_count;
    sym_unlock();
    return count;
}

/* --------------------------------------------------------------------------
 * ray_sym_strings_borrow
 *
 * Single-shot snapshot of the sym→string table for hot read-only
 * scanners (LIKE, dictionary projection, …).  ray_sym_str takes a spin
 * lock per call; iterating all 1.7M URL dict entries via ray_sym_str
 * means 1.7M lock acquisitions.  This routine takes the lock once,
 * captures the array pointer + length, drops the lock, and lets the
 * caller iterate lock-free.
 *
 * Validity: only safe during read-only phases (no concurrent
 * ray_sym_intern).  ray_sym_intern can realloc g_sym.strings, after
 * which the returned pointer is dangling.  Today's pipeline is one
 * pass: bulk-intern at CSV load, then run queries against the frozen
 * table — exactly the contract this borrow form needs.
 * -------------------------------------------------------------------------- */
void ray_sym_strings_borrow(ray_t*** out_strings, uint32_t* out_count) {
    if (out_strings) *out_strings = NULL;
    if (out_count)   *out_count   = 0;
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return;
    sym_lock();
    if (out_strings) *out_strings = g_sym.strings;
    if (out_count)   *out_count   = g_sym.str_count;
    sym_unlock();
}

/* --------------------------------------------------------------------------
 * ray_sym_ensure_cap -- pre-grow hash table and strings array
 *
 * Ensures the symbol table can hold at least `needed` total symbols without
 * rehashing.  Call before bulk interning (e.g., CSV merge) to prevent
 * mid-insert OOM that silently drops symbols.
 * -------------------------------------------------------------------------- */

bool ray_sym_ensure_cap(uint32_t needed) {
    if (!atomic_load_explicit(&g_sym_inited, memory_order_acquire)) return false;

    sym_lock();

    /* Grow strings array (and sidecars) if needed */
    while (g_sym.str_cap < needed) {
        if (g_sym.str_cap >= UINT32_MAX / 2) { sym_unlock(); return false; }
        uint32_t new_str_cap = g_sym.str_cap * 2;
        if (new_str_cap < needed) { /* jump directly to needed */
            new_str_cap = needed;
            /* Round up to power of 2 */
            new_str_cap--;
            new_str_cap |= new_str_cap >> 1;
            new_str_cap |= new_str_cap >> 2;
            new_str_cap |= new_str_cap >> 4;
            new_str_cap |= new_str_cap >> 8;
            new_str_cap |= new_str_cap >> 16;
            new_str_cap++;
            if (new_str_cap == 0) { sym_unlock(); return false; }
        }
        if (!sym_grow_str_cap(new_str_cap)) { sym_unlock(); return false; }
    }

    /* Grow hash table so load factor stays below threshold after filling */
    double raw_buckets = (double)needed / SYM_LOAD_FACTOR + 1.0;
    if (raw_buckets > (double)UINT32_MAX) { sym_unlock(); return false; }
    uint32_t needed_buckets = (uint32_t)raw_buckets;
    /* Round up to power of 2 */
    needed_buckets--;
    needed_buckets |= needed_buckets >> 1;
    needed_buckets |= needed_buckets >> 2;
    needed_buckets |= needed_buckets >> 4;
    needed_buckets |= needed_buckets >> 8;
    needed_buckets |= needed_buckets >> 16;
    needed_buckets++;

    if (needed_buckets > g_sym.bucket_cap) {
        if (!ht_grow_to(needed_buckets)) { sym_unlock(); return false; }
    }

    sym_unlock();
    return true;
}
