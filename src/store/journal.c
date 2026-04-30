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

#ifndef RAY_OS_WINDOWS
#  define _GNU_SOURCE   /* fileno(), gmtime_r() */
#endif

#include "journal.h"
#include "fileio.h"
#include "serde.h"
#include "core/ipc.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "mem/sys.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef RAY_OS_WINDOWS
#  include <unistd.h>
#  include <sys/stat.h>
#endif

#define RAY_JOURNAL_PATH_MAX 1024

/* Module-private state.  Single-threaded by construction (the IPC
 * dispatch loop is single-threaded for eval_payload, and replay runs
 * from main before any worker thread spins up). */
static struct {
    ray_journal_mode_t mode;
    FILE*              fp;
    char               base[RAY_JOURNAL_PATH_MAX];
    char               log_path[RAY_JOURNAL_PATH_MAX];
    bool               in_replay;
} g_journal = {
    .mode      = RAY_JOURNAL_OFF,
    .fp        = NULL,
    .base      = {0},
    .log_path  = {0},
    .in_replay = false,
};

/* ── helpers ──────────────────────────────────────────────────────── */

static bool path_join_ext(char* dst, size_t dstsz, const char* base, const char* ext) {
    int n = snprintf(dst, dstsz, "%s%s", base, ext);
    return n > 0 && (size_t)n < dstsz;
}

static bool file_exists(const char* path) {
#ifdef RAY_OS_WINDOWS
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

/* Read fixed-size buffer in a loop — fread can short-read on signals. */
static size_t read_full(FILE* f, void* buf, size_t want) {
    uint8_t* p = (uint8_t*)buf;
    size_t   got = 0;
    while (got < want) {
        size_t n = fread(p + got, 1, want - got, f);
        if (n == 0) break;
        got += n;
    }
    return got;
}

/* Decompress an IPC payload in place if the COMPRESSED flag is set;
 * otherwise no-op.  Returns owned buffer + length on success (caller
 * frees with ray_sys_free), NULL on failure.  When no decompression
 * is needed, *out_owned is set to false and *out_buf aliases payload. */
static bool decompress_if_needed(const ray_ipc_header_t* hdr,
                                 const uint8_t* payload, int64_t payload_len,
                                 uint8_t** out_buf, int64_t* out_len,
                                 bool* out_owned)
{
    if (!(hdr->flags & RAY_IPC_FLAG_COMPRESSED)) {
        *out_buf   = (uint8_t*)payload;
        *out_len   = payload_len;
        *out_owned = false;
        return true;
    }
    if (payload_len < 4) return false;
    uint32_t uncomp_size;
    memcpy(&uncomp_size, payload, 4);
    if (uncomp_size == 0 || uncomp_size > 256u * 1024u * 1024u) return false;

    uint8_t* tmp = (uint8_t*)ray_sys_alloc(uncomp_size);
    if (!tmp) return false;
    size_t dlen = ray_ipc_decompress(payload + 4, (size_t)payload_len - 4,
                                     tmp, uncomp_size);
    if (dlen != uncomp_size) { ray_sys_free(tmp); return false; }
    *out_buf   = tmp;
    *out_len   = (int64_t)uncomp_size;
    *out_owned = true;
    return true;
}

/* Evaluate a deserialized message exactly as eval_payload would: string
 * payloads run through ray_eval_str, everything else through ray_eval. */
static ray_t* eval_one(ray_t* msg) {
    if (!msg || RAY_IS_ERR(msg)) return msg;
    if (msg->type == -RAY_STR) {
        const char* s = ray_str_ptr(msg);
        size_t      n = ray_str_len(msg);
        if (!s || n == 0) return RAY_NULL_OBJ;
        char* tmp = (char*)ray_sys_alloc(n + 1);
        if (!tmp) return ray_error("oom", NULL);
        memcpy(tmp, s, n);
        tmp[n] = '\0';
        ray_t* r = ray_eval_str(tmp);
        ray_sys_free(tmp);
        return r;
    }
    return ray_eval(msg);
}

/* ── public API ───────────────────────────────────────────────────── */

bool ray_journal_is_open(void) { return g_journal.fp != NULL; }

ray_err_t ray_journal_write_bytes(const ray_ipc_header_t* hdr,
                                  const uint8_t*          payload,
                                  int64_t                 payload_len)
{
    if (!g_journal.fp || g_journal.in_replay) return RAY_OK;
    if (!hdr || !payload || payload_len < 0) return RAY_ERR_DOMAIN;

    if (fwrite(hdr, 1, sizeof(*hdr), g_journal.fp) != sizeof(*hdr))
        return RAY_ERR_IO;
    if (payload_len > 0 &&
        fwrite(payload, 1, (size_t)payload_len, g_journal.fp) != (size_t)payload_len)
        return RAY_ERR_IO;

    if (g_journal.mode == RAY_JOURNAL_SYNC) {
        if (fflush(g_journal.fp) != 0) return RAY_ERR_IO;
#ifndef RAY_OS_WINDOWS
        if (fsync(fileno(g_journal.fp)) != 0) return RAY_ERR_IO;
#else
        FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_journal.fp)));
#endif
    }
    return RAY_OK;
}

ray_err_t ray_journal_replay(const char*           path,
                             int64_t*              out_chunks,
                             int64_t*              out_eval_errors,
                             ray_jreplay_status_t* out_status)
{
    if (out_chunks)      *out_chunks      = 0;
    if (out_eval_errors) *out_eval_errors = 0;
    if (out_status)      *out_status      = RAY_JREPLAY_OK;

    FILE* f = fopen(path, "rb");
    if (!f) {
        if (out_status) *out_status = RAY_JREPLAY_IO;
        return RAY_ERR_IO;
    }

    bool prev_in_replay = g_journal.in_replay;
    g_journal.in_replay = true;

    int64_t chunks = 0;
    int64_t errs   = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;

    for (;;) {
        ray_ipc_header_t hdr;
        size_t r = read_full(f, &hdr, sizeof(hdr));
        if (r == 0) break;                          /* clean EOF */
        if (r != sizeof(hdr))                       { status = RAY_JREPLAY_BADTAIL; break; }
        if (hdr.prefix  != RAY_SERDE_PREFIX)        { status = RAY_JREPLAY_BADTAIL; break; }
        if (hdr.version != RAY_SERDE_WIRE_VERSION)  { status = RAY_JREPLAY_BADTAIL; break; }
        if (hdr.size <= 0 || hdr.size > 256LL*1024*1024)
                                                    { status = RAY_JREPLAY_BADTAIL; break; }

        uint8_t* buf = (uint8_t*)ray_sys_alloc((size_t)hdr.size);
        if (!buf)                                   { status = RAY_JREPLAY_IO; break; }
        if (read_full(f, buf, (size_t)hdr.size) != (size_t)hdr.size) {
            ray_sys_free(buf);
            status = RAY_JREPLAY_BADTAIL;
            break;
        }

        uint8_t* payload  = NULL;
        int64_t  pay_len  = 0;
        bool     owned    = false;
        if (!decompress_if_needed(&hdr, buf, hdr.size,
                                  &payload, &pay_len, &owned)) {
            ray_sys_free(buf);
            status = RAY_JREPLAY_BADTAIL;
            break;
        }

        int64_t consumed = pay_len;
        ray_t*  msg      = ray_de_raw(payload, &consumed);
        if (owned) ray_sys_free(payload);
        ray_sys_free(buf);

        if (!msg || RAY_IS_ERR(msg)) {
            if (msg) ray_release(msg);
            status = RAY_JREPLAY_BADTAIL;
            break;
        }

        ray_t* result = eval_one(msg);
        ray_release(msg);

        if (result && RAY_IS_ERR(result)) {
            const char* code = ray_err_code(result);
            fprintf(stderr, "log: WARN  chunk %lld raised: %s (during replay)\n",
                    (long long)chunks, code ? code : "?");
            errs++;
        }
        if (result && result != RAY_NULL_OBJ) ray_release(result);

        chunks++;
    }

    fclose(f);
    g_journal.in_replay = prev_in_replay;

    if (out_chunks)      *out_chunks      = chunks;
    if (out_eval_errors) *out_eval_errors = errs;
    if (out_status)      *out_status      = status;

    return (status == RAY_JREPLAY_OK) ? RAY_OK : RAY_ERR_DOMAIN;
}

ray_err_t ray_journal_validate(const char* path,
                               int64_t*    out_chunks,
                               int64_t*    out_valid_bytes)
{
    if (out_chunks)      *out_chunks      = 0;
    if (out_valid_bytes) *out_valid_bytes = 0;

    FILE* f = fopen(path, "rb");
    if (!f) return RAY_ERR_IO;

    int64_t chunks = 0;
    int64_t valid_off = 0;
    /* Reuse one growing buffer for payload reads — most logs hold
     * many small entries plus the occasional large one, so growing on
     * demand is simpler than trying to size up front. */
    uint8_t* buf = NULL;
    int64_t  cap = 0;

    for (;;) {
        ray_ipc_header_t hdr;
        size_t r = read_full(f, &hdr, sizeof(hdr));
        if (r == 0) break;                                /* clean EOF */
        if (r != sizeof(hdr))                       break;
        if (hdr.prefix  != RAY_SERDE_PREFIX)        break;
        if (hdr.version != RAY_SERDE_WIRE_VERSION)  break;
        if (hdr.size <= 0 || hdr.size > 256LL*1024*1024) break;

        if (hdr.size > cap) {
            uint8_t* tmp = (uint8_t*)ray_sys_alloc((size_t)hdr.size);
            if (!tmp) break;                              /* OOM mid-validate */
            if (buf) ray_sys_free(buf);
            buf = tmp;
            cap = hdr.size;
        }
        /* Actually consume the payload bytes — fseek would silently
         * succeed past EOF and we'd over-count valid frames on a
         * truncated log. */
        if (read_full(f, buf, (size_t)hdr.size) != (size_t)hdr.size) break;

        valid_off += (int64_t)sizeof(hdr) + hdr.size;
        chunks++;
    }
    if (buf) ray_sys_free(buf);
    fclose(f);

    if (out_chunks)      *out_chunks      = chunks;
    if (out_valid_bytes) *out_valid_bytes = valid_off;
    return RAY_OK;
}

/* Open <base>.log in append mode after replay. */
static ray_err_t open_log_for_append(void) {
    g_journal.fp = fopen(g_journal.log_path, "ab");
    if (!g_journal.fp) return RAY_ERR_IO;
    /* Disable stdio buffering: every fwrite must reach the OS buffer
     * immediately so a SIGTERM (or any non-clean shutdown) still leaves
     * the entry on disk.  Without this, the default block-buffered FILE*
     * keeps recent writes in user-space until 4 KB accumulates — a
     * silent data-loss window that defeats the whole point of -l mode.
     * In RAY_JOURNAL_SYNC mode we additionally fsync per write; here
     * we just need the bytes to leave the process. */
    setvbuf(g_journal.fp, NULL, _IONBF, 0);
    return RAY_OK;
}

ray_err_t ray_journal_open(const char* base, ray_journal_mode_t mode) {
    if (!base || !*base) return RAY_ERR_DOMAIN;
    if (g_journal.fp) return RAY_ERR_DOMAIN;   /* already open */

    size_t blen = strlen(base);
    if (blen + 5 >= sizeof(g_journal.base)) return RAY_ERR_DOMAIN;

    memcpy(g_journal.base, base, blen + 1);
    g_journal.mode = mode;
    if (!path_join_ext(g_journal.log_path, sizeof(g_journal.log_path), base, ".log"))
        return RAY_ERR_DOMAIN;

    char qdb_path[RAY_JOURNAL_PATH_MAX];
    if (!path_join_ext(qdb_path, sizeof(qdb_path), base, ".qdb"))
        return RAY_ERR_DOMAIN;

    /* 1. Snapshot — load <base>.qdb if present. */
    if (file_exists(qdb_path)) {
        bool prev_in_replay = g_journal.in_replay;
        g_journal.in_replay = true;
        ray_t* snap = ray_obj_load(qdb_path);
        g_journal.in_replay = prev_in_replay;

        if (!snap || RAY_IS_ERR(snap)) {
            fprintf(stderr, "log: ERROR  failed to load snapshot %s\n", qdb_path);
            if (snap) ray_release(snap);
            return RAY_ERR_IO;
        }
        if (snap->type != RAY_DICT) {
            fprintf(stderr, "log: ERROR  snapshot %s is not a dict\n", qdb_path);
            ray_release(snap);
            return RAY_ERR_DOMAIN;
        }
        ray_t* keys = ray_dict_keys(snap);
        ray_t* vals = ray_dict_vals(snap);
        int64_t n = keys ? keys->len : 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t sym_id;
            if (keys->type == RAY_SYM) {
                sym_id = ((int64_t*)ray_data(keys))[i];
            } else {
                continue;  /* unexpected key type — skip defensively */
            }
            ray_t* v = ray_list_get(vals, i);
            if (!v) continue;
            ray_retain(v);
            ray_err_t e = ray_env_bind(sym_id, v);
            if (e != RAY_OK) {
                fprintf(stderr, "log: WARN  snapshot bind for sym %lld failed (%d)\n",
                        (long long)sym_id, (int)e);
            }
        }
        ray_release(snap);
        fprintf(stderr, "log: loaded snapshot %s (%lld bindings)\n",
                qdb_path, (long long)n);
    }

    /* 2. Log — replay <base>.log if present. */
    if (file_exists(g_journal.log_path)) {
        int64_t chunks = 0, errs = 0;
        ray_jreplay_status_t status = RAY_JREPLAY_OK;
        ray_journal_replay(g_journal.log_path, &chunks, &errs, &status);
        if (status == RAY_JREPLAY_BADTAIL) {
            int64_t valid_chunks = 0, valid_bytes = 0;
            ray_journal_validate(g_journal.log_path, &valid_chunks, &valid_bytes);
            fprintf(stderr,
                    "log: ERROR badtail in %s after %lld entries (valid bytes = %lld)\n"
                    "log: hint: truncate the file at offset %lld to recover the\n"
                    "log:       valid prefix, then restart\n",
                    g_journal.log_path, (long long)chunks,
                    (long long)valid_bytes, (long long)valid_bytes);
            return RAY_ERR_DOMAIN;
        }
        fprintf(stderr, "log: replayed %lld entries (%lld eval errors) from %s\n",
                (long long)chunks, (long long)errs, g_journal.log_path);
    }

    /* 3. Open log for append. */
    return open_log_for_append();
}

ray_err_t ray_journal_close(void) {
    if (!g_journal.fp) return RAY_OK;
    fflush(g_journal.fp);
    fclose(g_journal.fp);
    g_journal.fp = NULL;
    return RAY_OK;
}

ray_err_t ray_journal_sync(void) {
    if (!g_journal.fp) return RAY_OK;
    if (g_journal.mode == RAY_JOURNAL_SYNC) return RAY_OK;
    if (fflush(g_journal.fp) != 0) return RAY_ERR_IO;
#ifndef RAY_OS_WINDOWS
    if (fsync(fileno(g_journal.fp)) != 0) return RAY_ERR_IO;
#else
    FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_journal.fp)));
#endif
    return RAY_OK;
}

/* UTC ISO-8601 with safe filename chars (no ':'). */
static void utc_stamp(char* buf, size_t bufsz) {
    time_t t = time(NULL);
    struct tm tm_;
#ifdef RAY_OS_WINDOWS
    gmtime_s(&tm_, &t);
#else
    gmtime_r(&t, &tm_);
#endif
    strftime(buf, bufsz, "%Y.%m.%dT%H.%M.%SZ", &tm_);
}

ray_err_t ray_journal_roll(void) {
    if (!g_journal.fp) return RAY_ERR_DOMAIN;

    fflush(g_journal.fp);
    fclose(g_journal.fp);
    g_journal.fp = NULL;

    char stamp[64];
    utc_stamp(stamp, sizeof(stamp));
    char archive[RAY_JOURNAL_PATH_MAX];
    int  n = snprintf(archive, sizeof(archive), "%s.%s.log", g_journal.base, stamp);
    if (n <= 0 || (size_t)n >= sizeof(archive)) return RAY_ERR_DOMAIN;

    if (ray_file_rename(g_journal.log_path, archive) != RAY_OK)
        return RAY_ERR_IO;

    return open_log_for_append();
}

ray_err_t ray_journal_snapshot(void) {
    if (!g_journal.fp) return RAY_ERR_DOMAIN;

    /* Enumerate ONLY user-defined globals (slots last written via
     * ray_env_set).  Builtin function objects must NOT enter the
     * snapshot — they hold absolute pointers from the prior process
     * and would dangle on reload.  ray_env_list_user is the one-bit-
     * per-slot filter maintained by env.c. */
    int32_t cap = ray_env_global_count();
    if (cap <= 0) cap = 1;
    int64_t* sym_ids = (int64_t*)ray_sys_alloc((size_t)cap * sizeof(int64_t));
    ray_t**  vals_buf = (ray_t**) ray_sys_alloc((size_t)cap * sizeof(ray_t*));
    if (!sym_ids || !vals_buf) {
        if (sym_ids)  ray_sys_free(sym_ids);
        if (vals_buf) ray_sys_free(vals_buf);
        return RAY_ERR_OOM;
    }
    int32_t kept = ray_env_list_user(sym_ids, vals_buf, cap);

    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, kept);
    if (!keys || RAY_IS_ERR(keys)) {
        ray_sys_free(sym_ids); ray_sys_free(vals_buf);
        return RAY_ERR_OOM;
    }
    ray_t* vals = ray_list_new(kept);
    if (!vals || RAY_IS_ERR(vals)) {
        ray_release(keys);
        ray_sys_free(sym_ids); ray_sys_free(vals_buf);
        return RAY_ERR_OOM;
    }
    for (int32_t i = 0; i < kept; i++) {
        keys = ray_vec_append(keys, &sym_ids[i]);
        if (RAY_IS_ERR(keys)) {
            ray_release(vals);
            ray_sys_free(sym_ids); ray_sys_free(vals_buf);
            return RAY_ERR_OOM;
        }
        vals = ray_list_append(vals, vals_buf[i]);
        if (RAY_IS_ERR(vals)) {
            ray_release(keys);
            ray_sys_free(sym_ids); ray_sys_free(vals_buf);
            return RAY_ERR_OOM;
        }
    }
    ray_sys_free(sym_ids);
    ray_sys_free(vals_buf);

    ray_t* snap = ray_dict_new(keys, vals);
    if (!snap || RAY_IS_ERR(snap)) return RAY_ERR_OOM;

    char qdb_path[RAY_JOURNAL_PATH_MAX];
    char qdb_tmp[RAY_JOURNAL_PATH_MAX];
    if (!path_join_ext(qdb_path, sizeof(qdb_path), g_journal.base, ".qdb") ||
        !path_join_ext(qdb_tmp,  sizeof(qdb_tmp),  g_journal.base, ".qdb.tmp")) {
        ray_release(snap);
        return RAY_ERR_DOMAIN;
    }

    /* ray_obj_save writes prefix-headered bytes (same wire framing). */
    ray_err_t e = ray_obj_save(snap, qdb_tmp);
    ray_release(snap);
    if (e != RAY_OK) return e;

    if (ray_file_rename(qdb_tmp, qdb_path) != RAY_OK) return RAY_ERR_IO;

    return ray_journal_roll();
}
