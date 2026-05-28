/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "core/timer.h"
#include "mem/sys.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int64_t ray_time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

ray_timers_t* ray_timers_create(int64_t initial_cap) {
    if (initial_cap < 1) initial_cap = 1;
    ray_timers_t* t = (ray_timers_t*)ray_sys_alloc(sizeof(ray_timers_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->heap = (ray_timer_t**)ray_sys_alloc((size_t)initial_cap * sizeof(ray_timer_t*));
    if (!t->heap) { ray_sys_free(t); return NULL; }
    t->cap = initial_cap;
    return t;
}

void ray_timers_destroy(ray_timers_t* t) {
    if (!t) return;
    for (int64_t i = 0; i < t->n; i++) {
        ray_release(t->heap[i]->fn);
        ray_sys_free(t->heap[i]);
    }
    ray_sys_free(t->heap);
    ray_sys_free(t);
}

/* Stubs — implemented in later tasks. */
int64_t ray_timers_add(ray_timers_t* t, int64_t tic_ms, int64_t num, ray_t* fn) {
    (void)t; (void)tic_ms; (void)num; (void)fn;
    return -1;
}

bool ray_timers_del(ray_timers_t* t, int64_t id) {
    (void)t; (void)id;
    return false;
}

int64_t ray_timers_next_deadline_ms(ray_timers_t* t) {
    if (!t || t->n == 0) return INT64_MAX;
    return t->heap[0]->exp_ms;
}

void ray_timers_fire_expired(ray_timers_t* t) {
    (void)t;
}
