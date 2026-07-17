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

/* openq Phase C: this file speaks the KDB IPC PROTOCOL (clean-room, from
 * qdocs/docs/docs/docs/basics/ipc.md + kb/serialization.md; javakdb c.java
 * is the cleared interactive reference — never a kdb+ binary).
 *
 *   handshake  client sends "user:password" + capability-byte + \0
 *              (older clients omit the capability byte); the server
 *              validates (constant-time -u/-U secret compare first, then
 *              the `.ipc.on.auth` hook may narrow) and replies with ONE
 *              byte min(cap, 3) — or closes the connection on rejection.
 *   header     8 bytes: [endian(01=LE) msgtype(0/1/2) compressed(0/1)
 *              total-len:int32] — total-len INCLUDES the header; both
 *              endiannesses are read, little-endian is emitted.  The
 *              256MB frame guard is enforced here.  Compressed inbound
 *              frames (byte 2 == 1, kdb's >2000-byte non-localhost rule)
 *              decompress via q_wire_uncompress_payload before routing —
 *              Phase F.  EMIT stays uncompressed by wire policy: the
 *              compress-on-send leg is deferred (needed later for the
 *              Python-driver acceptance work).
 *   payload    ONE q_wire object (src/qlang/q_wire.c, kb/serialization.md
 *              grammar).  Whole-payload consumption is enforced; trailing
 *              bytes are protocol corruption and close the connection.
 *   dispatch   STRING-FIRST (human ruling 2026-07-06): `.ipc.on.sync` /
 *              `.ipc.on.async` hooks take precedence (they receive the
 *              decoded object); otherwise a string payload evaluates via
 *              ray_eval_remote_str (the q pipeline when the q runtime is
 *              up, rayfall in the bare engine binary); a general-list
 *              payload — the kdb `(func;args)` typed-apply form — is an
 *              explicit fast-follow answered with 'nyi.
 *              TODO(ipc-phase-c-followup): route (func;args) through
 *              value/apply — a SINGLE application of func to the already-
 *              evaluated args — NEVER through ray_eval (a value object is
 *              not a parse tree; they diverge for nested lists.  See
 *              ARCHITECTURE.md "eval vs value").
 *   auth/eval  matches kdb: an authenticated connection gets full eval;
 *              restriction only via the -U secret + restricted flag,
 *              which still wraps every inbound eval and is re-imposed on
 *              journal replay.
 *   journal    unchanged 16-byte ray_ipc_header_t envelope (serde v5) —
 *              only the SOCKET wire moved to kdb frames.  A sync frame is
 *              journaled ONLY when it will be evaluated by the default
 *              string path: hook-handled frames are not journaled (replay
 *              cannot reproduce hook dispatch, and replaying a hook's
 *              list payload through eval would violate the value-vs-eval
 *              ruling).
 *
 * The native 16-byte-prefix protocol (version handshake, prefix header,
 * verbose flag, wire compression) is REMOVED.  ray_ipc_compress /
 * ray_ipc_decompress remain: journals may hold Phase-B-era compressed
 * frames within wire version 5. */

#ifndef RAY_OS_WINDOWS
  #define _GNU_SOURCE
#endif

#include "core/ipc.h"
#include "mem/sys.h"
#include "ops/ops.h"
#include "store/journal.h"
#include "qlang/q_wire.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <unistd.h>
  #include <sys/socket.h>
#endif

#if defined(__linux__)
  #include <sys/epoll.h>
  #define RAY_IPC_MAX_EVENTS 64
#elif defined(__APPLE__)
  #include <sys/event.h>
  #define RAY_IPC_MAX_EVENTS 64
#endif

#include "core/runtime.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "lang/internal.h"
#include "table/sym.h"

/* ===== Compression (delta + RLE) =====
 *
 * NOT the kdb wire codec (that is q_wire compress/uncompress, Phase F).
 * WIRE-DEAD since Phase C: kdb frames never use this scheme.  KEPT because
 * the journal may contain Phase-B-era compressed frames inside wire
 * version 5 — journal.c's decompress_if_needed still calls
 * ray_ipc_decompress. */

size_t ray_ipc_compress(const uint8_t* src, size_t len,
                        uint8_t* dst, size_t dst_cap)
{
    if (len <= RAY_IPC_COMPRESS_THRESHOLD) return 0;

    /* Step 1: delta-encode into temporary buffer */
    uint8_t* delta = (uint8_t*)ray_sys_alloc(len);
    if (!delta) return 0;

    delta[0] = src[0];
    for (size_t i = 1; i < len; i++)
        delta[i] = (uint8_t)(src[i] - src[i - 1]);

    /* Step 2: RLE-compress the delta stream */
    size_t di = 0;
    size_t si = 0;

    while (si < len) {
        if (si + 1 < len && delta[si] == delta[si + 1]) {
            uint8_t val = delta[si];
            size_t run = 1;
            while (si + run < len && delta[si + run] == val && run < 127)
                run++;
            if (di + 2 > dst_cap) { ray_sys_free(delta); return 0; }
            dst[di++] = (uint8_t)run;
            dst[di++] = val;
            si += run;
        } else {
            size_t start = si;
            size_t llen = 0;
            while (si < len && llen < 128) {
                if (si + 1 < len && delta[si] == delta[si + 1])
                    break;
                si++;
                llen++;
            }
            if (di + 1 + llen > dst_cap) { ray_sys_free(delta); return 0; }
            dst[di++] = (uint8_t)(-(int8_t)llen);
            memcpy(dst + di, delta + start, llen);
            di += llen;
        }
    }

    ray_sys_free(delta);
    if (di >= len) return 0;
    return di;
}

size_t ray_ipc_decompress(const uint8_t* src, size_t clen,
                          uint8_t* dst, size_t dst_len)
{
    uint8_t* decoded = (uint8_t*)ray_sys_alloc(dst_len);
    if (!decoded) return 0;

    size_t si = 0;
    size_t di = 0;

    while (si < clen && di < dst_len) {
        int8_t count = (int8_t)src[si++];
        if (count > 0) {
            if (si >= clen) { ray_sys_free(decoded); return 0; }
            uint8_t val = src[si++];
            size_t n = (size_t)count;
            if (di + n > dst_len) { ray_sys_free(decoded); return 0; }
            memset(decoded + di, val, n);
            di += n;
        } else {
            size_t n = (size_t)(-(int)count);
            if (si + n > clen || di + n > dst_len) {
                ray_sys_free(decoded);
                return 0;
            }
            memcpy(decoded + di, src + si, n);
            si += n;
            di += n;
        }
    }

    /* Un-delta */
    if (di == 0) { ray_sys_free(decoded); return 0; }
    dst[0] = decoded[0];
    for (size_t i = 1; i < di; i++)
        dst[i] = (uint8_t)(decoded[i] + dst[i - 1]);

    ray_sys_free(decoded);
    return di;
}

/* ===== kdb frame helpers ===== */

#define RAY_IPC_PHASE_HANDSHAKE 0
#define RAY_IPC_PHASE_HEADER    1
#define RAY_IPC_PHASE_PAYLOAD   2

#define KDB_HDR_LEN 8
#define KDB_MAX_MSG (256u * 1024u * 1024u)   /* 256MB frame guard */
#define KDB_HS_MAX  512                      /* creds line hard cap */
#define KDB_CAPABILITY 3                     /* what we speak (V3.0 level) */

/* Parse an 8-byte kdb header.  0 ok / -1 bad (caller closes).  *swap is
 * set when the frame is big-endian; *zip when the frame is compressed
 * (byte 2 == 1, decompressed by the payload readers — Phase F);
 * *payload_len EXCLUDES the header. */
static int kdb_hdr_parse(const uint8_t in[KDB_HDR_LEN], uint8_t* msgtype,
                         int* swap, uint8_t* zip, uint32_t* payload_len) {
    if (in[0] != 0x00 && in[0] != 0x01) return -1;
    *swap = (in[0] == 0x00);
    if (in[1] > 2) return -1;
    *msgtype = in[1];
    if (in[2] > 0x01) return -1;
    *zip = in[2];
    if (in[3] != 0x00) return -1;              /* reserved byte must be zero */
    uint32_t n = *swap
        ? ((uint32_t)in[4] << 24) | ((uint32_t)in[5] << 16) | ((uint32_t)in[6] << 8) | in[7]
        : ((uint32_t)in[7] << 24) | ((uint32_t)in[6] << 16) | ((uint32_t)in[5] << 8) | in[4];
    if (n < KDB_HDR_LEN + 1 || n > KDB_MAX_MSG) return -1;
    *payload_len = n - KDB_HDR_LEN;
    return 0;
}

/* Constant-time comparison — prevents timing side-channel on password. */
static bool ct_eq(const void* a, const void* b, size_t len) {
    const volatile uint8_t* x = a;
    const volatile uint8_t* y = b;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++)
        diff |= x[i] ^ y[i];
    return diff == 0;
}

/* Validate a creds line against the -u/-U secret.  creds is the RAW
 * "user:password" byte range from the kdb handshake (NO trailing NUL, NO
 * capability byte — the handshake reader strips both).  secret MUST point
 * to a char[256] buffer (zero-padded beyond the password).  Compares pw
 * against the full 256-byte secret buffer in constant time.  No strlen,
 * no secret-length-dependent copies. */
static bool validate_creds(const uint8_t* buf, size_t cred_len,
                           const char* secret) {
    const char* creds = (const char*)buf;
    const char* colon = cred_len ? memchr(creds, ':', cred_len) : NULL;
    const char* pw = colon ? colon + 1 : creds;
    size_t pw_len = colon ? (size_t)(cred_len - (size_t)(pw - creds)) : cred_len;
    if (pw_len > 255) pw_len = 255;

    /* Zero-pad pw into a 256-byte buffer, then compare all 256 bytes
     * against the secret buffer (also 256 bytes, zero-padded at init).
     * Matching passwords produce identical 256-byte buffers. */
    uint8_t pw_buf[256] = {0};
    memcpy(pw_buf, pw, pw_len);
    return ct_eq(pw_buf, secret, 256);
}

/* ===== Connection hooks (.ipc.on.*) =====
 *
 * Five user-settable lambdas that intercept the connection lifecycle.
 * Lookup is by interned sym id; we cache the ids in `hook_syms[]` so the
 * fast path is a single ray_env_get + RAY_LAMBDA-type check per dispatch.
 *
 * `__VM->ipc_handle` is the per-thread value `.ipc.handle` reads back
 * to Rayfall.  Each dispatch saves/restores it around the invocation,
 * so a hook that opens its own connection (calling `.ipc.handle` inside
 * a nested `.ipc.send` round-trip) still sees the outer handle when its
 * body resumes.  Default value -1 means "no hook is currently on the
 * stack" — exposed verbatim through the builtin.
 *
 * Errors:
 *   - on.open / on.close / on.async: logged to stderr, swallowed.
 *   - on.sync: error becomes the response (same as a raw `eval` error).
 *   - on.auth: error treated as reject (handshake refused). */

/* Hook indices must match the order in `src/lang/env.c`
 * (g_ipc_hook_syms[]) — the `ray_sym_ipc_hook(idx)` getter assumes this
 * mapping.  Keep them in lockstep. */
enum {
    IPC_HOOK_OPEN  = 0,
    IPC_HOOK_CLOSE = 1,
    IPC_HOOK_SYNC  = 2,
    IPC_HOOK_ASYNC = 3,
    IPC_HOOK_AUTH  = 4,
    IPC_HOOK_COUNT = 5,
};

/* The IPC dispatch context — which connection's hook/eval is on this
 * thread's stack, and the poll it lives in — is per-thread state and
 * therefore lives on the VM (__VM->ipc_handle / __VM->ipc_poll, see
 * core/runtime.h).  Connection handles are selector ids, so resolving
 * one needs the poll: inside a hook / inbound eval that's the poll
 * that fired it; everywhere else (REPL, scripts) it's the runtime's
 * main poll. */

static int64_t ipc_ctx_handle(void) {
    return __VM ? __VM->ipc_handle : -1;
}

static ray_poll_t* ipc_ctx_poll(void) {
    return __VM ? (ray_poll_t*)__VM->ipc_poll : NULL;
}

static void ipc_ctx_set(int64_t handle, ray_poll_t* poll) {
    if (!__VM) return;
    __VM->ipc_handle = handle;
    __VM->ipc_poll   = poll;
}

static ray_poll_t* ipc_active_poll(void) {
    ray_poll_t* p = ipc_ctx_poll();
    return p ? p : (ray_poll_t*)ray_runtime_get_poll();
}

int64_t ray_ipc_current_handle(void) {
    return ipc_ctx_handle();
}

/* Fetch the hook lambda if one is installed and is in fact a lambda.
 * Non-lambda bindings (cleared via `set .ipc.on.X 0` or never bound)
 * yield NULL — caller falls back to default behaviour.  Returns a
 * borrowed ref; do not release.  Sym IDs come from env.c's central
 * cache, so a runtime destroy/recreate cycle invalidates them in one
 * place and the lookup here always sees IDs from the current sym
 * table. */
static ray_t* hook_lookup(int idx) {
    int64_t sym = ray_sym_ipc_hook(idx);
    if (sym < 0) return NULL;
    ray_t* fn = ray_env_get(sym);
    if (!fn || fn->type != RAY_LAMBDA) return NULL;
    return fn;
}

/* Call a single-arg hook for lifecycle events (on.open / on.close).
 * Errors are logged and swallowed — a buggy logging hook must never
 * wedge connection teardown.  `poll` is the poll the connection lives
 * in, exposed thread-locally so the hook body can use the handle with
 * `.ipc.post` / `.ipc.send` / `.ipc.close`; the legacy server path
 * passes NULL (its conn-index handles are not selector ids). */
static void hook_call_lifecycle(ray_poll_t* poll, int idx, int64_t handle) {
    ray_t* fn = hook_lookup(idx);
    if (!fn) return;
    ray_t* arg = make_i64(handle);
    if (!arg || RAY_IS_ERR(arg)) { if (arg) ray_release(arg); return; }
    int64_t prev = ipc_ctx_handle();
    ray_poll_t* prev_poll = ipc_ctx_poll();
    ipc_ctx_set(handle, poll ? poll : prev_poll);
    ray_t* r = call_fn1(fn, arg);
    ipc_ctx_set(prev, prev_poll);
    if (r && RAY_IS_ERR(r)) {
        const char* name = (idx == IPC_HOOK_OPEN) ? ".ipc.on.open" : ".ipc.on.close";
        fprintf(stderr, "ipc: %s hook raised an error (handle=%lld)\n",
                name, (long long)handle);
    }
    ray_release(arg);
    if (r) {
        if (RAY_IS_ERR(r)) ray_error_free(r);
        else if (r != RAY_NULL_OBJ) ray_release(r);
    }
}

/* Call the on.auth hook with (user, pass) string atoms.  Returns:
 *  -  1 → hook ran and returned truthy; caller continues the handshake.
 *  -  0 → hook ran and returned falsy (or errored); caller rejects.
 *  - -1 → no hook installed; caller uses the existing pass-through.
 * With a -u/-U secret the constant-time compare always runs FIRST, so
 * this hook can only narrow access there — never widen it.  Without a
 * secret (kdb default: accept) the hook alone decides — the kdb
 * handshake always carries creds, mirroring kdb's -u + .z.pw layering.
 * `cred_buf/cred_len` is the raw "user:pass" range (no NUL, no cap). */
static int hook_call_auth(ray_poll_t* poll, int64_t handle,
                          const uint8_t* cred_buf, size_t cred_len) {
    ray_t* fn = hook_lookup(IPC_HOOK_AUTH);
    if (!fn) return -1;

    const char* creds = (const char*)cred_buf;
    const char* colon = cred_len ? memchr(creds, ':', cred_len) : NULL;
    const char* upart = creds;
    size_t      ulen  = colon ? (size_t)(colon - creds) : 0;
    const char* ppart = colon ? colon + 1 : creds;
    size_t      plen  = colon ? (size_t)(cred_len - (size_t)(ppart - creds))
                              : cred_len;

    ray_t* u = ray_str(upart, ulen);
    ray_t* p = ray_str(ppart, plen);
    if (!u || !p || RAY_IS_ERR(u) || RAY_IS_ERR(p)) {
        if (u && !RAY_IS_ERR(u)) ray_release(u);
        if (p && !RAY_IS_ERR(p)) ray_release(p);
        return 0;  /* allocation failure → reject conservatively */
    }
    int64_t prev = ipc_ctx_handle();
    ray_poll_t* prev_poll = ipc_ctx_poll();
    ipc_ctx_set(handle, poll ? poll : prev_poll);
    ray_t* r = call_fn2(fn, u, p);
    ipc_ctx_set(prev, prev_poll);
    ray_release(u);
    ray_release(p);

    int ok;
    if (!r || RAY_IS_ERR(r)) {
        fprintf(stderr, "ipc: .ipc.on.auth hook raised an error — rejecting\n");
        ok = 0;
    } else {
        ok = is_truthy(r) ? 1 : 0;
    }
    if (r) {
        if (RAY_IS_ERR(r)) ray_error_free(r);
        else if (r != RAY_NULL_OBJ) ray_release(r);
    }
    return ok;
}

/* Serialize + send one kdb frame.  0 on success, -1 on serialization or
 * socket failure.  q_wire_serialize emits the COMPLETE frame (8-byte LE
 * header with msgtype + payload); values with no kdb wire form ('nyi:
 * builtin fns, projections, engine handles) fail serialization. */
static int64_t conn_write_msg(ray_sock_t fd, ray_t* msg, uint8_t msgtype)
{
    ray_t* frame = q_wire_serialize(msg, msgtype);
    if (!frame) return -1;
    if (RAY_IS_ERR(frame)) { ray_error_free(frame); return -1; }
    int64_t rc = ray_sock_send(fd, ray_data(frame), (size_t)frame->len);
    ray_release(frame);
    return rc < 0 ? -1 : 0;
}

/* Send a SYNC response.  A result q_wire cannot express must never leave
 * the client waiting forever (issue #285): substitute the serialization
 * error itself — errors always wire as -128h + code. */
static void send_response(ray_sock_t fd, ray_t* result)
{
    ray_t* frame = q_wire_serialize(result, RAY_IPC_MSG_RESP);
    if (!frame) return;
    if (RAY_IS_ERR(frame)) {
        ray_t* sub = frame;                 /* the 'nyi/'type explains why */
        frame = q_wire_serialize(sub, RAY_IPC_MSG_RESP);
        ray_error_free(sub);
        if (!frame) return;
        if (RAY_IS_ERR(frame)) { ray_error_free(frame); return; }
    }
    ray_sock_send(fd, ray_data(frame), (size_t)frame->len);
    ray_release(frame);
}

/* Decode one inbound payload (exactly one object, whole-buffer).
 * *is_wire_err is set when the payload is a well-formed top-level -128h
 * error — the REMOTE's error object, a valid response value.  Any other
 * decode failure, or trailing bytes, returns NULL: protocol corruption,
 * the caller closes the connection. */
static ray_t* ipc_decode_payload(const uint8_t* p, size_t plen, int swap,
                                 int* is_wire_err)
{
    if (is_wire_err) *is_wire_err = 0;
    if (plen == 0) return NULL;
    if (p[0] == 0x80) {                     /* wire error -128h: 0x80 + code cstr */
        const uint8_t* nul = memchr(p + 1, 0, plen - 1);
        if (!nul || (size_t)(nul - p) != plen - 1) return NULL;
        char code[16];
        size_t n = (size_t)(nul - (p + 1));
        if (n > sizeof code - 1) n = sizeof code - 1;
        memcpy(code, p + 1, n); code[n] = 0;
        if (is_wire_err) *is_wire_err = 1;
        return ray_error(code, NULL);
    }
    size_t consumed = 0;
    ray_t* v = q_wire_read_obj(p, plen, &consumed, swap);
    if (!v) return NULL;
    if (RAY_IS_ERR(v)) { ray_error_free(v); return NULL; }
    if (consumed != plen) { ray_release(v); return NULL; }
    return v;
}

/* Dispatch one inbound request frame (msgtype 0/1) — shared by the poll
 * path and the legacy server path so the two cannot drift.  The caller
 * has already set the restricted flag + ipc ctx for this connection.
 * Returns 0 with *out_result owned (never NULL; RAY_NULL_OBJ for "no
 * result"), or -1 for protocol corruption — the caller must close the
 * connection and send nothing. */
static int ipc_dispatch(uint8_t msgtype, uint8_t* payload, size_t plen,
                        int swap, ray_t** out_result)
{
    *out_result = NULL;
    ray_t* msg = ipc_decode_payload(payload, plen, swap, NULL);
    if (!msg) return -1;

    int hook_idx = (msgtype == RAY_IPC_MSG_SYNC) ? IPC_HOOK_SYNC
                                                 : IPC_HOOK_ASYNC;
    ray_t* hook = hook_lookup(hook_idx);
    ray_t* result = NULL;

    if (hook) {
        /* Hook-handled frames are NOT journaled: replay cannot reproduce
         * hook dispatch, and replaying a hook's list payload through eval
         * would violate the value-vs-eval ruling (Phase C decision). */
        result = call_fn1(hook, msg);
        ray_release(msg);
        /* Async errors have nowhere to go on the wire (async never sends
         * a response) — log + drop so the operator sees the hook
         * misbehaving. */
        if (result && RAY_IS_ERR(result) && msgtype == RAY_IPC_MSG_ASYNC) {
            fprintf(stderr, "ipc: .ipc.on.async hook raised an error\n");
            ray_error_free(result);
            result = NULL;
        }
    } else if (msg->type == -RAY_STR) {
        /* Journal hook: log the mutation channel BEFORE evaluation, so a
         * crash mid-handler still leaves the message on disk for replay.
         * The envelope stays the 16-byte serde header; the payload is the
         * kdb object bytes verbatim (the v5 serde reader speaks the wire
         * grammar).  RESTRICTED is captured so replay re-imposes it —
         * without this a -U client's writes silently elevate to full
         * privilege on crash-recovery.  A failed journal write ABORTS the
         * eval ("the message has not been logged so we cannot accept
         * it") — silently evaluating un-logged mutations defeats -l/-L. */
        if (ray_journal_is_open() && msgtype == RAY_IPC_MSG_SYNC) {
            ray_ipc_header_t log_hdr = {
                .prefix  = RAY_SERDE_PREFIX,
                .version = RAY_SERDE_WIRE_VERSION,
                .flags   = ray_eval_get_restricted() ? RAY_IPC_FLAG_RESTRICTED : 0,
                .endian  = 0,
                .msgtype = msgtype,
                .size    = (int64_t)plen,
            };
            if (ray_journal_write_bytes(&log_hdr, payload, (int64_t)plen) != RAY_OK) {
                fprintf(stderr, "log: ERROR  journal write failed — refusing to evaluate\n");
                ray_release(msg);
                *out_result = ray_error("io", "journal write failed; mutation refused");
                return 0;
            }
        }
        result = ray_eval_remote_str(ray_str_ptr(msg), ray_str_len(msg));
        ray_release(msg);
    } else if (msg->type == RAY_LIST) {
        /* kdb (func; args…) value-apply request (ADR-0004): a SINGLE
         * application of func to the already-evaluated args via the q value
         * hook — NEVER ray_eval (a value object is not a parse tree; human
         * ruling 2026-07-06).  NOT journaled: the journal contract only logs
         * IPC STRING payloads (journal.c eval_one replays a non-string frame
         * through ray_eval, which would mis-evaluate a value object as an
         * AST).  A durable mutation must therefore be sent on the string
         * channel (which IS journaled) — value-apply journaling is a tracked
         * deferral (PLAN.md), not a silent gap.  Ownership: borrowed msg to
         * the hook, owned result back, one ray_release(msg). */
        result = ray_eval_remote_apply(msg);
        ray_release(msg);
    } else {
        ray_release(msg);
        result = ray_error("nyi", "IPC request must be a q source string");
    }

    /* A lazy result is an internal deferred-DAG representation that cannot
     * be serialized — force it to a concrete value before it reaches the
     * wire (issue #285). */
    if (result && ray_is_lazy(result))
        result = ray_lazy_materialize(result);  /* consumes the retain */
    *out_result = result ? result : RAY_NULL_OBJ;
    return 0;
}

/* ======================================================================
 * Poll-based IPC (new API)
 * ====================================================================== */

/* Per-connection state stored in selector->data.  Used for BOTH
 * directions: inbound conns accepted by a listener and outbound conns
 * registered by ray_ipc_connect — the selector id is the one handle
 * namespace either side of the wire works with. */
typedef struct {
    uint8_t          phase;
    uint8_t          msgtype;      /* current frame's msgtype */
    uint8_t          swap;         /* current frame is big-endian */
    uint8_t          zip;          /* current frame is compressed */
    uint32_t         plen;         /* current frame's payload length */
    uint8_t          hs[KDB_HS_MAX];  /* handshake creds accumulator */
    uint16_t         hs_len;
    int64_t          listener_id;  /* id of the listener selector; -1 = outbound */
    bool             auth_required;  /* server has -u/-U */
    bool             restricted;     /* server has -U */
    /* Sync round-trip state: while a ray_ipc_send waits on this conn it
     * pumps the rx machinery itself; a RESP frame is deposited here
     * instead of being evaluated.  Interleaved ASYNC/SYNC frames still
     * dispatch normally during the wait. */
    bool             sync_waiting;
    bool             sync_ready;
    ray_t*           sync_resp;
} ray_ipc_conn_data_t;

static ray_t* ipc_read_handshake(ray_poll_t* poll, ray_selector_t* sel);
static ray_t* ipc_read_header(ray_poll_t* poll, ray_selector_t* sel);
static ray_t* ipc_read_payload(ray_poll_t* poll, ray_selector_t* sel);
static void   ipc_on_close(ray_poll_t* poll, ray_selector_t* sel);

/* Wrappers matching ray_io_fn signature for socket recv */
static int64_t ipc_recv_fn(int64_t fd, uint8_t* buf, int64_t len) {
    return ray_sock_recv((ray_sock_t)fd, buf, (size_t)len);
}

/* Accept callback — called when listener fd is readable */
static ray_t* ipc_accept(ray_poll_t* poll, ray_selector_t* sel)
{
    ray_sock_t new_fd = ray_sock_accept((ray_sock_t)sel->fd);
    if (new_fd == RAY_INVALID_SOCK) return NULL;
    ray_sock_set_nonblocking(new_fd);

    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)ray_sys_alloc(
                                    sizeof(ray_ipc_conn_data_t));
    if (!cd) { ray_sock_close(new_fd); return NULL; }
    memset(cd, 0, sizeof(*cd));
    cd->phase = RAY_IPC_PHASE_HANDSHAKE;
    cd->listener_id = sel->id;
    cd->auth_required = (poll->auth_secret[0] != '\0');
    cd->restricted    = poll->restricted;

    ray_poll_reg_t reg = {0};
    reg.fd       = (int64_t)new_fd;
    reg.type     = RAY_SEL_SOCKET;
    reg.recv_fn  = ipc_recv_fn;
    reg.read_fn  = ipc_read_handshake;
    reg.close_fn = ipc_on_close;
    reg.data     = cd;

    int64_t id = ray_poll_register(poll, &reg);
    if (id < 0) {
        ray_sock_close(new_fd);
        ray_sys_free(cd);
        return NULL;
    }

    /* The kdb creds line is NUL-terminated (variable length): accumulate
     * one byte at a time until the terminator. */
    ray_selector_t* ns = ray_poll_get(poll, id);
    if (ns) ray_poll_rx_request(poll, ns, 1);

    return NULL;
}

/* Shared handshake completion: parse [user:pass][capbyte?] out of the
 * accumulated creds line, authenticate, and either reply with the common
 * capability byte (accept) or signal reject (kdb closes silently).
 * Returns true on accept. */
static bool kdb_handshake_complete(ray_poll_t* poll, int64_t handle,
                                   ray_sock_t fd,
                                   const uint8_t* hs, size_t hs_len,
                                   bool auth_required, const char* secret)
{
    uint8_t cap = 0;
    size_t  clen = hs_len;
    /* Capability byte: last pre-NUL byte when <= 6; older clients omit it
     * (creds are ASCII, so a control byte can only be the capability). */
    if (clen >= 1 && hs[clen - 1] <= 6) { cap = hs[clen - 1]; clen--; }

    bool ok = true;
    if (auth_required)
        ok = validate_creds(hs, clen, secret);
    if (ok) {
        int hook_ok = hook_call_auth(poll, handle, hs, clen);
        if (hook_ok == 0) ok = false;
    }
    if (!ok) return false;                    /* kdb rejects by closing */

    uint8_t common = cap < KDB_CAPABILITY ? cap : KDB_CAPABILITY;
    ray_sock_send(fd, &common, 1);
    return true;
}

static ray_t* ipc_read_handshake(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!sel->rx.buf || sel->rx.buf->offset < 1) return NULL;
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;

    uint8_t b = sel->rx.buf->data[0];
    if (b != 0x00) {
        if (cd->hs_len >= KDB_HS_MAX) {       /* creds line absurdly long */
            ray_poll_deregister(poll, sel->id);
            return NULL;
        }
        cd->hs[cd->hs_len++] = b;
        ray_poll_rx_request(poll, sel, 1);
        return NULL;
    }

    if (!kdb_handshake_complete(poll, sel->id, (ray_sock_t)sel->fd,
                                cd->hs, cd->hs_len,
                                cd->auth_required, poll->auth_secret)) {
        ray_poll_deregister(poll, sel->id);
        return NULL;
    }

    cd->phase = RAY_IPC_PHASE_HEADER;
    sel->rx.read_fn = ipc_read_header;
    ray_poll_rx_request(poll, sel, KDB_HDR_LEN);
    /* Connection is now fully ready for inbound messages.  Fire
     * `.ipc.on.open` AFTER we've requested the next read, so a hook that
     * calls back into the server can't race the read pump. */
    hook_call_lifecycle(poll, IPC_HOOK_OPEN, sel->id);
    return NULL;
}

static ray_t* ipc_read_header(ray_poll_t* poll, ray_selector_t* sel)
{
    if (!sel->rx.buf || sel->rx.buf->offset < KDB_HDR_LEN)
        return NULL;

    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;
    uint8_t msgtype; int swap; uint8_t zip; uint32_t plen;
    if (kdb_hdr_parse(sel->rx.buf->data, &msgtype, &swap, &zip, &plen) != 0) {
        ray_poll_deregister(poll, sel->id);
        return NULL;
    }
    cd->msgtype = msgtype;
    cd->swap    = (uint8_t)swap;
    cd->zip     = zip;
    cd->plen    = plen;

    cd->phase = RAY_IPC_PHASE_PAYLOAD;
    sel->rx.read_fn = ipc_read_payload;
    ray_poll_rx_request(poll, sel, (int64_t)plen);

    return NULL;
}

static ray_t* ipc_read_payload(ray_poll_t* poll, ray_selector_t* sel)
{
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;

    if (!sel->rx.buf || sel->rx.buf->offset < (int64_t)cd->plen)
        return NULL;

    /* Detach the payload buffer and advance the state machine to the
     * next header BEFORE dispatching: the eval below may re-enter this
     * connection's rx pump (a hook doing a nested sync round-trip on
     * the same handle), and that pump must find the selector parked on
     * a fresh header read — not on this very payload, which would
     * re-dispatch it.  Same reason for the local frame-info copies: a
     * nested frame would overwrite cd->msgtype/swap/plen under us. */
    uint8_t  msgtype = cd->msgtype;
    int      swap    = cd->swap;
    uint8_t  zip     = cd->zip;
    uint32_t plen    = cd->plen;
    ray_poll_buf_t*  payload = sel->rx.buf;
    int64_t          id      = sel->id;
    sel->rx.buf = NULL;
    cd->phase = RAY_IPC_PHASE_HEADER;
    sel->rx.read_fn = ipc_read_header;
    ray_poll_rx_request(poll, sel, KDB_HDR_LEN);

    /* Compressed frame (Phase F): decompress BEFORE any routing so the
     * RESP-deposit and dispatch legs below both see plain payload bytes.
     * A corrupt frame is protocol corruption — close, like any other. */
    uint8_t* pdata = payload->data;
    ray_t*   uz    = NULL;
    if (zip) {
        uz = q_wire_uncompress_payload(payload->data, (size_t)plen, swap);
        ray_poll_buf_free(payload);
        payload = NULL;
        if (!uz || RAY_IS_ERR(uz)) {
            if (uz) ray_error_free(uz);
            ray_poll_deregister(poll, id);
            return NULL;
        }
        pdata = (uint8_t*)ray_data(uz);
        plen  = (uint32_t)uz->len;
    }

    /* Response frame: deposit it for the sync send waiting on this
     * conn instead of evaluating it.  A response nobody waits for has
     * no defined meaning — log and drop.  Protocol corruption (decode
     * failure / trailing bytes) closes the connection so the waiter
     * observes a clean 'io error instead of a bogus value. */
    if (msgtype == RAY_IPC_MSG_RESP) {
        int is_wire_err = 0;
        ray_t* obj = ipc_decode_payload(pdata, (size_t)plen, swap,
                                        &is_wire_err);
        if (payload) ray_poll_buf_free(payload);
        if (uz) ray_release(uz);
        if (!obj) {
            ray_poll_deregister(poll, id);
            return NULL;
        }
        if (cd->sync_waiting && !cd->sync_ready) {
            cd->sync_resp  = obj;
            cd->sync_ready = true;
        } else {
            fprintf(stderr, "ipc: dropping unexpected response frame "
                            "(handle=%lld)\n", (long long)id);
            if (RAY_IS_ERR(obj)) ray_error_free(obj);
            else if (obj != RAY_NULL_OBJ) ray_release(obj);
        }
        return NULL;
    }

    bool prev_restricted = ray_eval_get_restricted();
    ray_eval_set_restricted(cd->restricted);

    /* Expose this connection's selector id to `.ipc.handle` (and its
     * poll to handle resolution) for the duration of any hook that
     * runs inside the dispatch.  Save/restore so a hook that itself
     * opens a nested IPC round-trip doesn't leave the wrong handle
     * visible when its caller resumes. */
    int64_t prev_handle = ipc_ctx_handle();
    ray_poll_t* prev_poll = ipc_ctx_poll();
    ipc_ctx_set(id, poll);

    ray_t* result = NULL;
    int rc = ipc_dispatch(msgtype, pdata, (size_t)plen, swap, &result);

    ipc_ctx_set(prev_handle, prev_poll);
    ray_eval_set_restricted(prev_restricted);
    if (payload) ray_poll_buf_free(payload);
    if (uz) ray_release(uz);

    if (rc != 0) {                            /* protocol corruption */
        ray_poll_deregister(poll, id);
        return NULL;
    }

    /* Send response for sync messages.  The eval may have closed this
     * very connection (`.ipc.close` on its own handle) — revalidate the
     * selector before writing to its fd. */
    if (msgtype == RAY_IPC_MSG_SYNC) {
        ray_selector_t* cur = ray_poll_get(poll, id);
        if (cur && cur->data == (void*)cd)
            send_response((ray_sock_t)cur->fd, result);
    }
    if (RAY_IS_ERR(result)) ray_error_free(result);
    else if (result != RAY_NULL_OBJ) ray_release(result);

    return NULL;
}

static void ipc_on_close(ray_poll_t* poll, ray_selector_t* sel)
{
    (void)poll;
    /* Fire `.ipc.on.close` BEFORE tearing the per-conn state down so a
     * hook reading `.ipc.handle` still sees this connection's id, and
     * before the listener's own close path (which would otherwise also
     * route through here) runs the hook with a stale fd.  Guard on:
     *   - sel->data: the listener itself has no conn data.
     *   - listener_id ≥ 0: lifecycle hooks pair with inbound on.open
     *     only — outbound conns (ray_ipc_connect) never fired on.open,
     *     so they must not fire on.close either.
     *   - phase ≥ HEADER: the connection actually completed handshake
     *     (otherwise no matching on.open was fired, so on.close must
     *     also stay silent to keep the pair balanced for the user). */
    if (sel->data) {
        ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;
        if (cd->listener_id >= 0 &&
            (cd->phase == RAY_IPC_PHASE_HEADER ||
             cd->phase == RAY_IPC_PHASE_PAYLOAD)) {
            hook_call_lifecycle(poll, IPC_HOOK_CLOSE, sel->id);
        }
        /* A RESP deposited for a sync wait that never consumed it
         * (connection died mid-round-trip) would otherwise leak. */
        if (cd->sync_resp) {
            if (RAY_IS_ERR(cd->sync_resp)) ray_error_free(cd->sync_resp);
            else if (cd->sync_resp != RAY_NULL_OBJ) ray_release(cd->sync_resp);
            cd->sync_resp = NULL;
        }
        ray_sys_free(sel->data);
        sel->data = NULL;
    }
    ray_sock_close((ray_sock_t)sel->fd);
}

int64_t ray_ipc_listen(ray_poll_t* poll, uint16_t port)
{
    if (!poll) return -1;

    ray_sock_t fd = ray_sock_listen(port);
    if (fd == RAY_INVALID_SOCK) return -1;
    ray_sock_set_nonblocking(fd);

    ray_poll_reg_t reg = {0};
    reg.fd       = (int64_t)fd;
    reg.type     = RAY_SEL_SOCKET;
    reg.read_fn  = ipc_accept;
    reg.close_fn = ipc_on_close;

    int64_t id = ray_poll_register(poll, &reg);
    if (id < 0) {
        ray_sock_close(fd);
        return -1;
    }
    return id;
}

/* ======================================================================
 * Server API (legacy — wraps its own poll; used by C test fixtures)
 * ====================================================================== */

static void conn_close(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    /* `.ipc.on.close` fires only for conns that were actually opened —
     * a slot whose phase never advanced past HANDSHAKE was never
     * announced via on.open and so shouldn't be announced via on.close.
     * Keeps the pair balanced for the user. */
    if (c->phase == RAY_IPC_PHASE_HEADER ||
        c->phase == RAY_IPC_PHASE_PAYLOAD) {
        hook_call_lifecycle(NULL, IPC_HOOK_CLOSE, (int64_t)(c - srv->conns));
    }

#if defined(__linux__)
    epoll_ctl(srv->poll_fd, EPOLL_CTL_DEL, c->fd, NULL);
#elif defined(__APPLE__)
    struct kevent kev;
    EV_SET(&kev, c->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(srv->poll_fd, &kev, 1, NULL, 0, NULL);
#else
    (void)srv;
#endif

    ray_sock_close(c->fd);
    if (c->rx_buf) ray_sys_free(c->rx_buf);
    c->fd      = RAY_INVALID_SOCK;
    c->rx_buf  = NULL;
    c->rx_len  = 0;
    c->rx_need = 0;
    c->hs_len  = 0;

    uint32_t idx = (uint32_t)(c - srv->conns);
    if (idx + 1 < srv->n_conns)
        srv->conns[idx] = srv->conns[srv->n_conns - 1];
    if (srv->n_conns > 0) srv->n_conns--;
}

/* One handshake byte at a time (see the poll path): accumulate into
 * c->hs until the NUL terminator, then authenticate + reply/close. */
static void conn_on_handshake(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    uint8_t b = c->rx_buf[0];
    c->rx_len = 0;                       /* reuse the 1-byte buffer */

    if (b != 0x00) {
        if (c->hs_len >= KDB_HS_MAX) { conn_close(srv, c); return; }
        c->hs[c->hs_len++] = b;
        return;
    }

    bool auth_req = (srv->auth_secret[0] != '\0');
    if (!kdb_handshake_complete(NULL, (int64_t)(c - srv->conns), c->fd,
                                c->hs, c->hs_len, auth_req,
                                srv->auth_secret)) {
        conn_close(srv, c);
        return;
    }

    ray_sys_free(c->rx_buf);
    c->rx_buf  = NULL;
    c->rx_len  = 0;
    c->rx_need = KDB_HDR_LEN;
    c->phase   = RAY_IPC_PHASE_HEADER;
    hook_call_lifecycle(NULL, IPC_HOOK_OPEN, (int64_t)(c - srv->conns));
}

static void conn_on_header(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    uint8_t msgtype; int swap; uint8_t zip; uint32_t plen;
    if (kdb_hdr_parse(c->rx_buf, &msgtype, &swap, &zip, &plen) != 0) {
        conn_close(srv, c);
        return;
    }
    c->msgtype = msgtype;
    c->swap    = (uint8_t)swap;
    c->zip     = zip;
    c->plen    = plen;

    ray_sys_free(c->rx_buf);
    c->rx_buf = (uint8_t*)ray_sys_alloc((size_t)plen);
    if (!c->rx_buf) { conn_close(srv, c); return; }
    c->rx_len  = 0;
    c->rx_need = (size_t)plen;
    c->phase   = RAY_IPC_PHASE_PAYLOAD;
}

static void conn_on_payload(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    /* Compressed frame (Phase F): decompress before dispatch, mirroring
     * the poll path.  Corrupt frames close the connection. */
    uint8_t* pdata = c->rx_buf;
    size_t   pdlen = c->rx_len;
    ray_t*   uz    = NULL;
    if (c->zip) {
        uz = q_wire_uncompress_payload(c->rx_buf, c->rx_len, c->swap);
        if (!uz || RAY_IS_ERR(uz)) {
            if (uz) ray_error_free(uz);
            conn_close(srv, c);
            return;
        }
        pdata = (uint8_t*)ray_data(uz);
        pdlen = (size_t)uz->len;
    }

    bool prev = ray_eval_get_restricted();
    ray_eval_set_restricted(srv->restricted);

    /* Conn-array index doubles as the handle on the legacy path —
     * stable for the connection's lifetime, distinct across active
     * connections, freed back to the pool on close.  Mirrored shape
     * of the poll path's sel->id. */
    int64_t prev_handle = ipc_ctx_handle();
    ray_poll_t* prev_poll = ipc_ctx_poll();
    ipc_ctx_set((int64_t)(c - srv->conns), prev_poll);

    ray_t* result = NULL;
    int rc = ipc_dispatch(c->msgtype, pdata, pdlen, c->swap, &result);

    ipc_ctx_set(prev_handle, prev_poll);
    ray_eval_set_restricted(prev);
    if (uz) ray_release(uz);

    if (rc != 0) { conn_close(srv, c); return; }   /* protocol corruption */

    if (c->msgtype == RAY_IPC_MSG_SYNC)
        send_response(c->fd, result);
    if (RAY_IS_ERR(result)) ray_error_free(result);
    else if (result != RAY_NULL_OBJ) ray_release(result);

    ray_sys_free(c->rx_buf);
    c->rx_buf  = NULL;
    c->rx_len  = 0;
    c->rx_need = KDB_HDR_LEN;
    c->phase   = RAY_IPC_PHASE_HEADER;
}

static void conn_on_readable(ray_ipc_server_t* srv, ray_ipc_conn_t* c)
{
    if (!c->rx_buf) {
        c->rx_buf = (uint8_t*)ray_sys_alloc(c->rx_need);
        if (!c->rx_buf) { conn_close(srv, c); return; }
    }

    int64_t n = ray_sock_recv(c->fd, c->rx_buf + c->rx_len,
                              c->rx_need - c->rx_len);
    if (n <= 0) { conn_close(srv, c); return; }
    c->rx_len += (size_t)n;

    if (c->rx_len < c->rx_need) return;

    switch (c->phase) {
    case RAY_IPC_PHASE_HANDSHAKE: conn_on_handshake(srv, c); break;
    case RAY_IPC_PHASE_HEADER:    conn_on_header(srv, c);    break;
    case RAY_IPC_PHASE_PAYLOAD:   conn_on_payload(srv, c);   break;
    }
}

ray_err_t ray_ipc_server_init(ray_ipc_server_t* srv, uint16_t port)
{
    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = ray_sock_listen(port);
    if (srv->listen_fd == RAY_INVALID_SOCK) return RAY_ERR_IO;
    ray_sock_set_nonblocking(srv->listen_fd);

#if defined(__linux__)
    srv->poll_fd = epoll_create1(0);
    if (srv->poll_fd < 0) {
        ray_sock_close(srv->listen_fd);
        return RAY_ERR_IO;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
    epoll_ctl(srv->poll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);
#elif defined(__APPLE__)
    srv->poll_fd = kqueue();
    if (srv->poll_fd < 0) {
        ray_sock_close(srv->listen_fd);
        return RAY_ERR_IO;
    }
    struct kevent kev;
    EV_SET(&kev, srv->listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(srv->poll_fd, &kev, 1, NULL, 0, NULL);
#else
    srv->poll_fd = -1;
#endif

    srv->running = true;
    return RAY_OK;
}

void ray_ipc_server_destroy(ray_ipc_server_t* srv)
{
    for (uint32_t i = 0; i < srv->n_conns; i++) {
        ray_ipc_conn_t* c = &srv->conns[i];
        if (c->fd != RAY_INVALID_SOCK) {
            if (c->rx_buf) ray_sys_free(c->rx_buf);
            ray_sock_close(c->fd);
        }
    }
    srv->n_conns = 0;

    ray_sock_close(srv->listen_fd);
    srv->listen_fd = RAY_INVALID_SOCK;

    if (srv->poll_fd >= 0) {
#ifndef RAY_OS_WINDOWS
        close(srv->poll_fd);
#endif
    }
    srv->poll_fd = -1;
    srv->running = false;
}

int ray_ipc_poll(ray_ipc_server_t* srv, int timeout_ms)
{
    int ready = 0;

#if defined(__linux__)
    struct epoll_event events[RAY_IPC_MAX_EVENTS];
    int nfds = epoll_wait(srv->poll_fd, events, RAY_IPC_MAX_EVENTS, timeout_ms);
    if (nfds < 0) return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < nfds; i++) {
        int fd = events[i].data.fd;

        if (fd == srv->listen_fd) {
            ray_sock_t new_fd = ray_sock_accept(srv->listen_fd);
            if (new_fd == RAY_INVALID_SOCK) continue;
            ray_sock_set_nonblocking(new_fd);
            if (srv->n_conns >= RAY_IPC_MAX_CONNS) {
                ray_sock_close(new_fd);
                continue;
            }
            ray_ipc_conn_t* c = &srv->conns[srv->n_conns++];
            memset(c, 0, sizeof(*c));
            c->fd      = new_fd;
            c->rx_need = 1;                     /* handshake: byte-at-a-time */
            c->phase   = RAY_IPC_PHASE_HANDSHAKE;
            struct epoll_event cev = { .events = EPOLLIN, .data.fd = new_fd };
            epoll_ctl(srv->poll_fd, EPOLL_CTL_ADD, new_fd, &cev);
        } else {
            bool found = false;
            for (uint32_t j = 0; j < srv->n_conns; j++) {
                if (srv->conns[j].fd == fd) {
                    conn_on_readable(srv, &srv->conns[j]);
                    found = true;
                    break;
                }
            }
            if (!found) ready++;
        }
    }

#elif defined(__APPLE__)
    struct kevent events[RAY_IPC_MAX_EVENTS];
    struct timespec ts;
    struct timespec* tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    int nfds = kevent(srv->poll_fd, NULL, 0, events, RAY_IPC_MAX_EVENTS, tsp);
    if (nfds < 0) return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < nfds; i++) {
        int fd = (int)events[i].ident;

        if (fd == srv->listen_fd) {
            ray_sock_t new_fd = ray_sock_accept(srv->listen_fd);
            if (new_fd == RAY_INVALID_SOCK) continue;
            ray_sock_set_nonblocking(new_fd);
            if (srv->n_conns >= RAY_IPC_MAX_CONNS) {
                ray_sock_close(new_fd);
                continue;
            }
            ray_ipc_conn_t* c = &srv->conns[srv->n_conns++];
            memset(c, 0, sizeof(*c));
            c->fd      = new_fd;
            c->rx_need = 1;                     /* handshake: byte-at-a-time */
            c->phase   = RAY_IPC_PHASE_HANDSHAKE;
            struct kevent kev;
            EV_SET(&kev, new_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
            kevent(srv->poll_fd, &kev, 1, NULL, 0, NULL);
        } else {
            bool found = false;
            for (uint32_t j = 0; j < srv->n_conns; j++) {
                if (srv->conns[j].fd == fd) {
                    conn_on_readable(srv, &srv->conns[j]);
                    found = true;
                    break;
                }
            }
            if (!found) ready++;
        }
    }

#else  /* Windows: select-based fallback */
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(srv->listen_fd, &rfds);
    ray_sock_t maxfd = srv->listen_fd;
    for (uint32_t i = 0; i < srv->n_conns; i++) {
        FD_SET(srv->conns[i].fd, &rfds);
        if (srv->conns[i].fd > maxfd) maxfd = srv->conns[i].fd;
    }

    struct timeval tv;
    struct timeval* tvp = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    int nfds = select((int)(maxfd + 1), &rfds, NULL, NULL, tvp);
    if (nfds < 0) return (errno == EINTR) ? 0 : -1;

    if (FD_ISSET(srv->listen_fd, &rfds)) {
        ray_sock_t new_fd = ray_sock_accept(srv->listen_fd);
        if (new_fd != RAY_INVALID_SOCK) {
            ray_sock_set_nonblocking(new_fd);
            if (srv->n_conns >= RAY_IPC_MAX_CONNS) {
                ray_sock_close(new_fd);
            } else {
                ray_ipc_conn_t* c = &srv->conns[srv->n_conns++];
                memset(c, 0, sizeof(*c));
                c->fd      = new_fd;
                c->rx_need = 1;                 /* handshake: byte-at-a-time */
                c->phase   = RAY_IPC_PHASE_HANDSHAKE;
            }
        }
    }

    for (uint32_t i = srv->n_conns; i > 0; ) {
        --i;
        if (srv->conns[i].fd != RAY_INVALID_SOCK && FD_ISSET(srv->conns[i].fd, &rfds))
            conn_on_readable(srv, &srv->conns[i]);
    }
#endif

    return ready;
}

/* ===== Connection-handle API =====
 *
 * One handle namespace: a handle is the poll selector id of an IPC
 * connection — inbound (accepted by a listener) or outbound (opened by
 * ray_ipc_connect).  Every entry point below resolves the handle in the
 * "active" poll (the poll dispatching the current hook/eval, else the
 * runtime's main poll), so server-side code can write to the very
 * handles its hooks receive — server-initiated push. */

static int64_t recv_full(ray_sock_t fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        int64_t n = ray_sock_recv(fd, (uint8_t*)buf + total, len - total);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return (int64_t)total;
}

/* Resolve `handle` to a handshake-complete IPC connection selector in
 * the active poll.  Rejects anything else living in the selector table
 * (listeners, the REPL's stdin, conns still mid-handshake) so a stray
 * integer can never write to a non-IPC fd. */
static ray_selector_t* conn_resolve(ray_poll_t** poll_out, int64_t handle)
{
    ray_poll_t* poll = ipc_active_poll();
    if (!poll) return NULL;
    ray_selector_t* sel = ray_poll_get(poll, handle);
    if (!sel || sel->type != RAY_SEL_SOCKET || !sel->data) return NULL;
    if (sel->rx.read_fn != ipc_read_header &&
        sel->rx.read_fn != ipc_read_payload) return NULL;
    if (poll_out) *poll_out = poll;
    return sel;
}

/* q-layer support: map a connection handle (poll selector id) to its
 * underlying OS socket fd (kdb presents the handle AS the fd), and back.
 * ray_ipc_fd_of_handle resolves through conn_resolve so only a live,
 * handshake-complete IPC connection yields a fd; ray_ipc_handle_of_fd walks
 * the active poll's selector table for the IPC connection whose fd matches,
 * applying the same read_fn filter conn_resolve uses so a listener/stdin fd
 * can never be selected.  Both return -1 on no match.  These do NOT alter
 * ray_ipc_connect's return value — rayfall still sees the selector id. */
int64_t ray_ipc_fd_of_handle(int64_t handle)
{
    ray_selector_t* sel = conn_resolve(NULL, handle);
    return sel ? sel->fd : -1;
}

int64_t ray_ipc_handle_of_fd(int64_t fd)
{
    ray_poll_t* poll = ipc_active_poll();
    if (!poll || fd < 0) return -1;
    for (uint32_t i = 0; i < poll->n_sels; i++) {
        ray_selector_t* sel = poll->sels[i];
        if (sel && sel->fd == fd && sel->type == RAY_SEL_SOCKET && sel->data &&
            (sel->rx.read_fn == ipc_read_header ||
             sel->rx.read_fn == ipc_read_payload))
            return sel->id;
    }
    return -1;
}

/* Drain whatever is available on one connection's socket through its rx
 * state machine WITHOUT blocking: fill the requested rx buffer, invoke
 * read_fn for each completed phase, repeat until the socket is dry.
 * Mirrors the per-event rx loop in the poll backends; used by sync
 * sends to wait for their RESP while still dispatching any interleaved
 * frames (pushed ASYNCs, nested SYNC requests).  Returns 0 when the
 * socket has no more data right now, -1 if the selector was
 * deregistered (peer closed or protocol error). */
static int conn_pump(ray_poll_t* poll, int64_t id)
{
    for (;;) {
        ray_selector_t* sel = ray_poll_get(poll, id);
        if (!sel) return -1;
        if (!sel->rx.buf || !sel->rx.recv_fn || !sel->rx.read_fn) return 0;
        while (sel->rx.buf->offset < sel->rx.buf->size) {
            int64_t nr = sel->rx.recv_fn(sel->fd,
                                         sel->rx.buf->data + sel->rx.buf->offset,
                                         sel->rx.buf->size - sel->rx.buf->offset);
            if (nr <= 0) {
                if (nr < 0 && errno == EINTR) continue;
                if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    return 0;  /* drained — caller decides whether to wait */
                ray_poll_deregister(poll, id);  /* error or peer closed */
                return -1;
            }
            sel->rx.buf->offset += nr;
        }
        /* Phase buffer complete — advance the state machine.  read_fn
         * may deregister the selector or re-arm it for the next phase;
         * the loop re-validates from scratch either way. */
        sel->rx.read_fn(poll, sel);
    }
}

int64_t ray_ipc_connect(const char* host, uint16_t port,
                         const char* user, const char* password,
                         int timeout_ms)
{
    /* The connection lives in the active poll's selector table — its
     * selector id IS the handle.  No poll, no handle namespace: refuse
     * up front rather than hand out an integer nothing can resolve. */
    ray_poll_t* poll = ipc_active_poll();
    if (!poll) return -1;

    /* Default the connect/handshake budget to 5s when the caller gives
     * no explicit timeout, matching the long-standing handshake timeout. */
    int connect_to = timeout_ms > 0 ? timeout_ms : 5000;
    ray_sock_t fd = ray_sock_connect(host, port, connect_to);
    if (fd == RAY_INVALID_SOCK) return (errno == ETIMEDOUT) ? -5 : -1;

    /* kdb handshake: "user:password" + capability byte + NUL.  The colon
     * is ALWAYS present (the no-credential handshake is ":\3\0") so the
     * server's auth hook sees an unambiguous user/pass split. */
    char cred[300];
    int n = snprintf(cred, sizeof(cred) - 2, "%s:%s",
                     user ? user : "", password ? password : "");
    if (n < 0 || n >= (int)sizeof(cred) - 2) { ray_sock_close(fd); return -1; }
    cred[n++] = KDB_CAPABILITY;
    cred[n++] = 0x00;
    if (ray_sock_send(fd, cred, (size_t)n) < 0) {
        ray_sock_close(fd);
        return -1;
    }

    /* Accept = one common-capability byte; reject = the server closes
     * the connection without sending anything (kdb behaviour). */
    uint8_t common;
    if (recv_full(fd, &common, 1) < 0) {
        ray_sock_close(fd);
        return -3; /* auth rejected (server closed the connection) */
    }
    /* A compliant kdb server replies with the NEGOTIATED capability =
     * min(client, server), so it can never exceed what we offered.  A byte
     * above KDB_CAPABILITY means the peer is not speaking our protocol (or a
     * non-kdb service that accepted the TCP connection): refuse rather than
     * register a live handle that would hang on the first request.  We still
     * do not otherwise ACT on the level (no >2GB frames / no compression yet
     * — Phase F), so any value in [0, KDB_CAPABILITY] is accepted. */
    if (common > KDB_CAPABILITY) {
        ray_sock_close(fd);
        return -1; /* handshake failed: capability byte out of range */
    }

#ifdef RAY_OS_WINDOWS
    { DWORD z = 0;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&z, sizeof(z));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&z, sizeof(z)); }
#else
    { struct timeval z = {0, 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof(z));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &z, sizeof(z)); }
#endif

    /* Handshake + auth done (blocking socket).  Hand the fd over to the
     * poll: nonblocking, parked on a header read, same read pump as an
     * inbound connection — which is exactly what makes pushed frames
     * from the server dispatchable on this side. */
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)ray_sys_alloc(
                                    sizeof(ray_ipc_conn_data_t));
    if (!cd) { ray_sock_close(fd); return -1; }
    memset(cd, 0, sizeof(*cd));
    cd->phase       = RAY_IPC_PHASE_HEADER;
    cd->listener_id = -1;               /* outbound — no lifecycle hooks */
    cd->restricted  = poll->restricted; /* -U narrows pushed evals too */

    ray_sock_set_nonblocking(fd);

    ray_poll_reg_t reg = {0};
    reg.fd       = (int64_t)fd;
    reg.type     = RAY_SEL_SOCKET;
    reg.recv_fn  = ipc_recv_fn;
    reg.read_fn  = ipc_read_header;
    reg.close_fn = ipc_on_close;
    reg.data     = cd;

    int64_t id = ray_poll_register(poll, &reg);
    if (id < 0) {
        ray_sock_close(fd);
        ray_sys_free(cd);
        return -1;
    }
    ray_selector_t* ns = ray_poll_get(poll, id);
    if (ns) ray_poll_rx_request(poll, ns, KDB_HDR_LEN);

    return id;
}

void ray_ipc_close(int64_t handle)
{
    ray_poll_t* poll;
    ray_selector_t* sel = conn_resolve(&poll, handle);
    if (!sel) return;
    /* Deregister fires ipc_on_close: socket closed, conn data freed,
     * and the on.close hook for inbound connections — so a server can
     * kick a client through the same handle its hooks were given. */
    ray_poll_deregister(poll, sel->id);
}

/* Sync round-trip on any connection handle.  Writes a SYNC frame, then
 * pumps this connection's rx machinery until the matching RESP frame is
 * deposited — dispatching (not swallowing) any frames that arrive in
 * between: a pushed ASYNC gets evaluated, a nested SYNC request from
 * the peer gets evaluated and answered.  Full-duplex, either side of
 * the wire. */
static ray_t* sync_send(int64_t handle, ray_t* msg)
{
    bool owned = false;
    if (ray_is_lazy(msg)) {
        ray_retain(msg);
        msg = ray_lazy_materialize(msg); /* consumes the retain */
        if (RAY_IS_ERR(msg)) return msg;
        owned = true;
    }

    ray_poll_t* poll;
    ray_selector_t* sel = conn_resolve(&poll, handle);
    if (!sel) {
        if (owned) ray_release(msg);
        return ray_error("io", "connection closed");
    }
    ray_ipc_conn_data_t* cd = (ray_ipc_conn_data_t*)sel->data;
    if (cd->sync_waiting) {
        /* A sync wait is already pumping this conn further down the
         * stack (hook → nested send on the SAME handle).  Two waiters
         * can't both claim the next RESP — refuse the inner one. */
        if (owned) ray_release(msg);
        return ray_error("io", "nested sync send on busy handle");
    }

    if (conn_write_msg((ray_sock_t)sel->fd, msg, RAY_IPC_MSG_SYNC) < 0) {
        if (owned) ray_release(msg);
        return ray_error("io", "ipc send failed");
    }
    if (owned) ray_release(msg);

    cd->sync_waiting = true;
    cd->sync_ready   = false;
    cd->sync_resp    = NULL;
    int64_t id = sel->id;

    for (;;) {
        /* Process anything already buffered/readable first, then block
         * for more.  The pump can deregister the selector (peer died) —
         * conn data is freed with it, so re-resolve every iteration.
         * The data-pointer compare also guards against this id being
         * reused by a NEW connection opened inside a dispatched eval:
         * that conn's state must not be mistaken for ours. */
        int rc = conn_pump(poll, id);
        sel = ray_poll_get(poll, id);
        if (!sel || sel->data != (void*)cd)
            return ray_error("io", "connection closed");
        if (cd->sync_ready) {
            ray_t* result = cd->sync_resp;
            cd->sync_resp    = NULL;
            cd->sync_ready   = false;
            cd->sync_waiting = false;
            if (!result)
                return ray_error("io", "ipc bad response");
            return result;
        }
        if (rc < 0) {
            /* Deregistered but selector still resolves?  Can't happen —
             * defensive: treat as closed. */
            return ray_error("io", "connection closed");
        }
        if (ray_sock_wait_readable((ray_sock_t)sel->fd, -1) < 0) {
            cd->sync_waiting = false;
            return ray_error("io", "ipc recv failed");
        }
    }
}

ray_t* ray_ipc_send(int64_t handle, ray_t* msg)
{
    return sync_send(handle, msg);
}

ray_err_t ray_ipc_send_async(int64_t handle, ray_t* msg)
{
    bool owned = false;
    if (ray_is_lazy(msg)) {
        ray_retain(msg);
        msg = ray_lazy_materialize(msg); /* consumes the retain */
        if (RAY_IS_ERR(msg)) {
            ray_err_t code = ray_err_from_obj(msg);
            ray_error_free(msg);
            return code;
        }
        owned = true;
    }
    ray_selector_t* sel = conn_resolve(NULL, handle);
    ray_err_t rc = (!sel || conn_write_msg((ray_sock_t)sel->fd, msg,
                                           RAY_IPC_MSG_ASYNC) < 0)
                   ? RAY_ERR_IO : RAY_OK;
    if (owned) ray_release(msg);
    return rc;
}

/* Remote-REPL sync send.  The kdb wire has no output-capture flag — the
 * server prints `show`/display output on ITS console, exactly like kdb.
 * The [captured_str, result] response SHAPE is preserved for repl.c's
 * remote loop, with captured always "" now. */
ray_t* ray_ipc_send_verbose(int64_t handle, ray_t* msg)
{
    ray_t* r = sync_send(handle, msg);
    if (r && RAY_IS_ERR(r)) return r;
    ray_t* cap = ray_str("", 0);
    if (!cap || RAY_IS_ERR(cap)) {
        if (cap) ray_release(cap);
        return r;
    }
    ray_t* out = ray_list_new(2);
    if (!out) { ray_release(cap); return r; }
    ray_list_append(out, cap);
    ray_release(cap);
    ray_list_append(out, r);
    if (r != RAY_NULL_OBJ) ray_release(r);
    return out;
}
