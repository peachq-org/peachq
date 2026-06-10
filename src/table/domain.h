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

#ifndef RAY_DOMAIN_H
#define RAY_DOMAIN_H

/*
 * domain.h -- Symbol-resolution domains.
 *
 * A domain is the dictionary a RAY_SYM vector's indices resolve over.
 * Two kinds:
 *
 *   RUNTIME -- an immortal singleton wrapping the global intern table
 *              (g_sym, src/table/sym.c).  Every runtime-created SYM vec
 *              points at it.  retain/release skip it entirely (no atomic
 *              churn on the vec free hot path).
 *
 *   FILE    -- a read-only mmapped symfile (the STRL format ray_sym_save
 *              writes), opened through a process-wide realpath-keyed
 *              refcounted cache: opening the same resolved path twice
 *              yields the SAME object — pointer equality is domain
 *              identity, always.  The reverse index (find) is built
 *              lazily on first use; per-position string atoms are
 *              materialized lazily and owned by the domain (borrowed by
 *              callers, exactly like ray_sym_str).
 *
 * Phase 1 scope: FILE domains are READ-ONLY — base mapping only, no
 * growable in-memory tail yet.  ray_sym_domain_intern on a FILE domain
 * and ray_sym_domain_flush are declared for API completeness but return
 * -1 / RAY_ERR_NYI; the live-write path (in-memory tail, dirty flush
 * under flock, single-writer enforcement) lands in Phase 2.
 *
 * Lifecycle: FILE domains are refcounted.  Every attached SYM column
 * holds a ref (taken on attach and in ray_retain_owned_refs, dropped on
 * vec free next to the str_pool/index owned-ref handling).  The cache
 * entry itself is weak: the last release unlinks the entry, munmaps the
 * base mapping and frees the lazy structures — a subsequent open builds
 * a fresh mapping.
 */

#include <rayforce.h>
#include <stdbool.h>

typedef struct ray_sym_domain_s ray_sym_domain_t;

/* Immortal singleton delegating to the global intern table.
 * (Also declared in rayforce.h for the inline ray_sym_vec_domain.) */
ray_sym_domain_t* ray_sym_runtime_domain(void);

/* Open (or re-use from the cache) the FILE domain for `path` — a symfile
 * in the STRL format written by ray_sym_save.  Returns NULL on I/O or
 * format errors.  The returned object carries one reference per open;
 * release with ray_sym_domain_release. */
ray_sym_domain_t* ray_sym_domain_open(const char* path);

/* No-ops on the runtime singleton. */
void ray_sym_domain_retain(ray_sym_domain_t* dom);
void ray_sym_domain_release(ray_sym_domain_t* dom);

/* Borrowed string atom for position `pos` (NULL if out of range).
 * RUNTIME: delegates to ray_sym_str.  FILE: lazily materialized atom
 * owned by the domain — valid for the domain's lifetime, do not release. */
ray_t* ray_sym_domain_str(ray_sym_domain_t* dom, int64_t pos);

/* Position of `str` in the domain, or -1 if absent.
 * FILE: builds the reverse index on first call (O(|vocabulary|)). */
int64_t ray_sym_domain_find(ray_sym_domain_t* dom, const char* str, size_t len);

/* Find-or-append.  RUNTIME: delegates to ray_sym_intern.  FILE: Phase 2
 * (in-memory tail append) — currently returns -1 (NYI). */
int64_t ray_sym_domain_intern(ray_sym_domain_t* dom, const char* str, size_t len);

/* Number of entries in the domain. */
int64_t ray_sym_domain_count(ray_sym_domain_t* dom);

/* Resolved (realpath) symfile path; NULL for the runtime domain. */
const char* ray_sym_domain_path(ray_sym_domain_t* dom);

/* Write the dirty in-memory tail to the symfile under flock, detecting
 * concurrent writers.  Phase 2 — currently returns RAY_ERR_NYI. */
ray_err_t ray_sym_domain_flush(ray_sym_domain_t* dom, bool durable);

#endif /* RAY_DOMAIN_H */
