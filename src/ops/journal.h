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

/* Rayfall-facing thin wrappers over store/journal.{c,h}.  These are the
 * functions registered under the `.log.*` namespace in eval.c. */
#ifndef RAY_OPS_JOURNAL_H
#define RAY_OPS_JOURNAL_H

#include <rayforce.h>

/* (.log.open args) — args = (`async; "base") or (`sync; "base").
 * Loads <base>.qdb if present, replays <base>.log if present, then
 * opens <base>.log for append. */
ray_t* ray_log_open_fn(ray_t** args, int64_t n);

/* (.log.write expr) — append a synthetic entry containing the
 * serialized form of `expr`.  No-op (returns null) if no journal is
 * open or if a replay is currently in progress. */
ray_t* ray_log_write_fn(ray_t* expr);

/* (.log.replay "path") -> i64 chunks replayed.  Errors with "badtail"
 * if the file ends mid-frame; the error message includes the byte
 * offset of the last good entry. */
ray_t* ray_log_replay_fn(ray_t* path);

/* (.log.validate "path") -> (chunks; valid_bytes) pair.  Maps to
 * q's `-11!(-2; file)` — count valid frames without evaluating. */
ray_t* ray_log_validate_fn(ray_t* path);

/* (.log.roll) — close and rename current log to <base>.<UTC>.log,
 * open a fresh empty <base>.log. */
ray_t* ray_log_roll_fn(ray_t** args, int64_t n);

/* (.log.snapshot) — write the current global env to <base>.qdb,
 * then roll the log. */
ray_t* ray_log_snapshot_fn(ray_t** args, int64_t n);

/* (.log.sync) — fflush + fsync the open log (no-op in -L mode). */
ray_t* ray_log_sync_fn(ray_t** args, int64_t n);

/* (.log.close) — close the active log. */
ray_t* ray_log_close_fn(ray_t** args, int64_t n);

#endif /* RAY_OPS_JOURNAL_H */
