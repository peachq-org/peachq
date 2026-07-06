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
#  define _GNU_SOURCE   /* fileno() for fsync-after-fwrite below */
#endif


#ifndef RAY_OS_WINDOWS
#  define _GNU_SOURCE   /* fileno() for fsync-after-fwrite below */
#endif

#include "serde.h"
#include "core/types.h"
#include "mem/heap.h"
#include "vec/vec.h"

#ifndef RAY_OS_WINDOWS
#  include <unistd.h>
#endif
#include "lang/format.h"
#include "qlang/q_wire.h"   /* THE codec (serde mode) — kb/serialization grammar + ext band */
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * serde v5 (RAY_SERDE_WIRE_VERSION 5): the payload format IS kdb's
 * serialization grammar, produced/consumed by the q_wire codec in SERDE
 * mode (src/qlang/q_wire.c/h): kdb bytes for every kdb-expressible value
 * plus the serde-only extension band (unsigned tags 200..236) for engine
 * values kdb cannot express — builtin fns by name (through the hooks
 * below), raw lambdas (params+body), RAY_STR vectors, quoted syms, and
 * BOOL/U8 aux-bit typed nulls.  Payloads are always little-endian.
 *
 * This file keeps the public serde API and the fn-serde hooks; the byte
 * grammar lives in ONE place (q_wire.c — single-home rule).  Known v5
 * limits vs v4 (pre-v1 ruling, no migration): vector counts are int32
 * ('limit beyond 2^31-1 elements), and ray_serde_size performs a real
 * serialization (size/write parity by construction; the discard is the
 * price of exact sizing under the frozen callers' size-then-write
 * pattern).
 * -------------------------------------------------------------------------- */

/* openq fn-serde hooks — see serde.h.  NULL = historic env-name behaviour. */
static ray_serde_fn_writer_t g_fn_writer = NULL;
static ray_serde_fn_reader_t g_fn_reader = NULL;

void ray_serde_set_fn_hooks(ray_serde_fn_writer_t writer,
                            ray_serde_fn_reader_t reader) {
    g_fn_writer = writer;
    g_fn_reader = reader;
}

ray_serde_fn_writer_t ray_serde_fn_writer_hook(void) { return g_fn_writer; }
ray_serde_fn_reader_t ray_serde_fn_reader_hook(void) { return g_fn_reader; }

/* --------------------------------------------------------------------------
 * ray_serde_size — serialized size (excluding IPC header).
 * v5: serialize into a scratch buffer and measure — deterministic, so the
 * follow-up ray_ser_raw writes exactly this many bytes.  Returns 0 on
 * unserializable input (callers treat <= 0 as failure), -1 on overflow.
 * -------------------------------------------------------------------------- */

int64_t ray_serde_size(ray_t* obj) {
    q_wire_wbuf_t b = {0};
    if (q_wire_write_obj_ex(&b, obj, 1)) {
        q_wire_wbuf_free(&b);
        return 0;
    }
    int64_t n = (int64_t)b.len;
    q_wire_wbuf_free(&b);
    return n;
}

/* --------------------------------------------------------------------------
 * ray_ser_raw — serialize into caller buffer (>= ray_serde_size(obj) bytes).
 * Returns bytes written, 0 on error.
 * -------------------------------------------------------------------------- */

int64_t ray_ser_raw(uint8_t* buf, ray_t* obj) {
    q_wire_wbuf_t b = {0};
    if (q_wire_write_obj_ex(&b, obj, 1)) {
        q_wire_wbuf_free(&b);
        return 0;
    }
    memcpy(buf, b.p, b.len);
    int64_t n = (int64_t)b.len;
    q_wire_wbuf_free(&b);
    return n;
}

/* --------------------------------------------------------------------------
 * ray_de_raw — deserialize ONE object from buffer; *len becomes the bytes
 * REMAINING after the object (v4 contract — callers own the framing, so
 * trailing bytes are the caller's business, exactly as in v4).
 * -------------------------------------------------------------------------- */

ray_t* ray_de_raw(uint8_t* buf, int64_t* len) {
    if (!len || *len < 1) return NULL;
    size_t consumed = 0;
    ray_t* r = q_wire_read_obj_ex(buf, (size_t)*len, &consumed, 0, 1);
    *len -= (int64_t)consumed;
    return r;
}

/* --------------------------------------------------------------------------
 * ray_ser — top-level: serialize with IPC header
 * -------------------------------------------------------------------------- */


ray_t* ray_ser(ray_t* obj) {
    bool owned = false;
    if (ray_is_lazy(obj)) {
        ray_retain(obj);
        obj = ray_lazy_materialize(obj); /* consumes the retain */
        if (RAY_IS_ERR(obj)) return obj;
        owned = true;
    }

    int64_t payload = ray_serde_size(obj);
    if (payload <= 0) {
        ray_t* e = ray_error("domain", payload < 0
            ? "serialize: payload size overflow"
            : "serialize: zero serialized size for %s", ray_type_name(obj->type));
        if (owned) ray_release(obj);
        return e;
    }

    int64_t total = (int64_t)sizeof(ray_ipc_header_t) + payload;
    ray_t* buf = ray_vec_new(RAY_U8, total);
    if (!buf || RAY_IS_ERR(buf)) {
        if (owned) ray_release(obj);
        return buf;
    }
    buf->len = total;

    ray_ipc_header_t* hdr = (ray_ipc_header_t*)ray_data(buf);
    hdr->prefix  = RAY_SERDE_PREFIX;
    hdr->version = RAY_SERDE_WIRE_VERSION;
    hdr->flags   = 0;
    hdr->endian  = 0;
    hdr->msgtype = 0;
    hdr->size    = payload;

    int64_t written = ray_ser_raw((uint8_t*)ray_data(buf) + sizeof(ray_ipc_header_t), obj);
    if (written == 0) {
        ray_t* e = ray_error("domain", "serialize: ray_ser_raw wrote 0 bytes for %s", ray_type_name(obj->type));
        ray_release(buf);
        if (owned) ray_release(obj);
        return e;
    }

    if (owned) ray_release(obj);
    return buf;
}

/* --------------------------------------------------------------------------
 * ray_de — top-level: deserialize from U8 vector
 * -------------------------------------------------------------------------- */

ray_t* ray_de(ray_t* bytes) {
    if (!bytes || RAY_IS_ERR(bytes)) return ray_error("type", "deserialize: input must be a u8 byte buffer, got %s", bytes ? ray_type_name(bytes->type) : "null");
    if (bytes->type != RAY_U8 && bytes->type != -RAY_U8)
        return ray_error("type", "deserialize: input must be a u8 byte buffer, got %s", ray_type_name(bytes->type));

    int64_t total = bytes->len;
    uint8_t* buf = (uint8_t*)ray_data(bytes);

    if (total < (int64_t)sizeof(ray_ipc_header_t))
        return ray_error("domain", "deserialize: buffer too small for ipc header, got %lld bytes", (long long)total);

    ray_ipc_header_t* hdr = (ray_ipc_header_t*)buf;
    if (hdr->prefix != RAY_SERDE_PREFIX)
        return ray_error("domain", "deserialize: bad ipc header magic prefix");
    if (hdr->version != RAY_SERDE_WIRE_VERSION)
        return ray_error("version", "serde wire version mismatch");
    if (hdr->size < 0 || hdr->size > 1000000000)
        return ray_error("domain", "deserialize: ipc header payload size %lld out of range", (long long)hdr->size);
    if (hdr->size + (int64_t)sizeof(ray_ipc_header_t) != total)
        return ray_error("domain", "deserialize: ipc header size %lld + header != buffer length %lld", (long long)hdr->size, (long long)total);

    int64_t len = hdr->size;
    return ray_de_raw(buf + sizeof(ray_ipc_header_t), &len);
}

/* --------------------------------------------------------------------------
 * File I/O: save/load any object in binary format
 * -------------------------------------------------------------------------- */

ray_err_t ray_obj_save(ray_t* obj, const char* path) {
    bool owned = false;
    if (ray_is_lazy(obj)) {
        ray_retain(obj);
        obj = ray_lazy_materialize(obj); /* consumes the retain */
        if (RAY_IS_ERR(obj)) {
            ray_err_t code = ray_err_from_obj(obj);
            ray_error_free(obj);
            return code;
        }
        owned = true;
    }

    ray_t* bytes = ray_ser(obj);
    if (!bytes || RAY_IS_ERR(bytes)) {
        if (bytes && RAY_IS_ERR(bytes)) ray_error_free(bytes);
        if (owned) ray_release(obj);
        return RAY_ERR_DOMAIN;
    }

    FILE* f = fopen(path, "wb");
    if (!f) { ray_release(bytes); if (owned) ray_release(obj); return RAY_ERR_IO; }

    size_t total = (size_t)bytes->len;
    size_t n = fwrite(ray_data(bytes), 1, total, f);
    if (n != total) {
        fclose(f); ray_release(bytes);
        if (owned) ray_release(obj);
        return RAY_ERR_IO;
    }

    /* Durability: fflush + fsync BEFORE fclose so a buffered write
     * hitting ENOSPC inside fclose doesn't slip through silently.
     * Callers (esp. ray_journal_snapshot) write to a .tmp then rename
     * — without this fsync the .tmp may be empty/partial on disk
     * when the rename atomically swaps it in. */
    if (fflush(f) != 0) {
        fclose(f); ray_release(bytes);
        if (owned) ray_release(obj);
        return RAY_ERR_IO;
    }
#ifndef RAY_OS_WINDOWS
    if (fsync(fileno(f)) != 0) {
        fclose(f); ray_release(bytes);
        if (owned) ray_release(obj);
        return RAY_ERR_IO;
    }
#endif
    /* fclose itself can fail (final flush of any platform-level
     * buffer).  Check it. */
    int close_rc = fclose(f);
    ray_release(bytes);
    if (owned) ray_release(obj);
    return close_rc == 0 ? RAY_OK : RAY_ERR_IO;
}

ray_t* ray_obj_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return ray_error("io", NULL);

    /* Check fseek/ftell return values — silent failures here let a
     * truncated read through as "valid empty file" or worse. */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ray_error("io", "fseek end"); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return ray_error("io", "ftell"); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ray_error("io", "fseek set"); }

    if (sz == 0) { fclose(f); return ray_error("io", "empty file"); }

    ray_t* buf = ray_vec_new(RAY_U8, sz);
    if (!buf || RAY_IS_ERR(buf)) { fclose(f); return buf; }
    buf->len = sz;

    size_t n = fread(ray_data(buf), 1, (size_t)sz, f);
    fclose(f);

    if ((long)n != sz) { ray_release(buf); return ray_error("io", "short read"); }

    ray_t* result = ray_de(buf);
    ray_release(buf);
    return result;
}
