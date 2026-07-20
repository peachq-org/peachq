/* q_bang — the `-N!` internal-function dispatcher (see q_bang.h): a switch over the
 * negative bang id.  The bang is the single C home; the q name (`.j.k`, `.Q.btoa`)
 * delegates to it in q (`name:-N!`).  Monadic (`-27!` takes a 2-list); unknown -> 'nyi. */
#include "qlang/q_bang.h"
#include "qlang/q_registry_internal.h"  /* q_value_wrap, q_hsym_wrap, q_attr_wrap, q_strict_i64 */
#include "qlang/q_builtins.h"   /* q_parse_builtin_fn, q_md5_fn, q_dotq_btoa_fn, q_dotq_sha1_fn */
#include "qlang/q_json.h"       /* q_json_serialize (.j.j), q_json_deserialize (.j.k) */
#include "qlang/q_wire.h"       /* q_wire_serialize/_deserialize/_compress, Q_WIRE_ASYNC */
#include "qlang/q_fmt.h"        /* q_fmt_krepr — single-line repr backing `-3!` / .Q.s1 */
#include <rayforce.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- multi-statement handlers (VALUE-shaped: borrowed y, OWNED return) ----- */

static ray_t* h_deser(ray_t* y) {                       /* -9! from bytes */
    if (!y || y->type != RAY_BYTE_ONLY)
        return ray_error("type", "-9!: expects a byte vector");
    return q_wire_deserialize(y);
}

static ray_t* h_zip(ray_t* y) {                         /* -18! compress bytes */
    ray_t* f = q_wire_serialize(y, Q_WIRE_ASYNC);
    if (!f || RAY_IS_ERR(f)) return f;
    ray_t* z = q_wire_compress(f);      /* >2000b + under-half, else f unchanged */
    ray_release(f);
    return z;
}

/* -3!x — single-line string representation (== `.Q.s1`).  Routes to the
 * existing q_fmt_krepr single-line formatter and returns its buffer as a q
 * string (RAY_STR).  Byte-for-byte the same text `0N!x` shows.
 * q_fmt_krepr truncates silently into a caller buffer, so grow-and-retry until
 * the whole repr fits (kdb returns the full string) — cap the growth to stay
 * bounded.  (q_fmt_krepr still caps each NESTED-list element at its own 2048
 * internal buffer; that residual limit is shared with `0N!`/`.Q.s1` and out of
 * scope here.) */
static ray_t* h_s1(ray_t* y) {
    size_t cap = 8192;
    char* buf = malloc(cap);
    if (!buf) return ray_error("wsfull", "-3!: out of memory");
    for (;;) {
        buf[0] = '\0';
        q_fmt_krepr(y, buf, cap);
        size_t len = strlen(buf);
        if (len < cap - 1 || cap >= (1u << 24)) {   /* fit whole (or growth cap) */
            ray_t* r = ray_charv(buf, (int64_t)len);
            free(buf);
            return r;
        }
        cap *= 2;
        char* nb = realloc(buf, cap);
        if (!nb) { free(buf); return ray_error("wsfull", "-3!: out of memory"); }
        buf = nb;
    }
}

/* -16!x — reference count of x.  DIVERGENCE: kdb returns the number of q-level
 * aliases bound to a variable (`a:b:c:1 2 3` -> 3); openq exposes the ray_t
 * heap refcount, which counts internal owners, not source aliases.  The ledger
 * pins openq's ACTUAL number (smoked), and the PR Decisions flag the gap — we
 * do NOT fabricate kdb's count. */
static ray_t* h_refcnt(ray_t* y) {
    if (!y) return ray_error("type", "-16!: nil argument");
    return ray_i64((int64_t)y->rc);
}

/* Format one double to `places` decimals with IEEE754 rounding (C's %.*f is
 * exactly round-half-to-even on the stored binary value — matching kdb's
 * `-27!`, which ignores \P).  Returns an owned RAY_STR (or 'wsfull error).
 * A large-magnitude float can need more than the stack buffer (the integer
 * part is unbounded), so allocate exactly what snprintf reports rather than
 * truncating. */
static ray_t* q_bang_fmt_one(int places, double y) {
    char stackbuf[512];
    int m = snprintf(stackbuf, sizeof stackbuf, "%.*f", places, y);
    if (m < 0) return ray_charv("", 0);
    if ((size_t)m < sizeof stackbuf) return ray_charv(stackbuf, (int64_t)m);
    char* heap = malloc((size_t)m + 1);
    if (!heap) return ray_error("wsfull", "-27!: out of memory");
    snprintf(heap, (size_t)m + 1, "%.*f", places, y);
    ray_t* r = ray_charv(heap, (int64_t)m);
    free(heap);
    return r;
}

/* -27!(x;y) — IEEE754 precision format.  The single operand is a 2-element list
 * `(x;y)`: `x` is an int atom (decimal places), `y` a float atom or float vector;
 * returns a string (atom) or list of strings (vector), formatted with IEEE754
 * rounding, ignoring \P. */
static ray_t* h_format(ray_t* arg) {
    if (!arg || arg->type != RAY_LIST || ray_len(arg) != 2)
        return ray_error("type", "-27!: expects a 2-element (places;y) list");
    ray_t* px = ray_list_get(arg, 0);    /* borrowed */
    ray_t* y  = ray_list_get(arg, 1);    /* borrowed */
    int64_t places64;
    if (px && px->type == -RAY_BOOL) places64 = px->b8;
    else if (!q_strict_i64(px, &places64))
        return ray_error("type", "-27!: places");
    if (places64 < 0) places64 = 0;
    if (places64 > 320) places64 = 320;       /* guard the snprintf width */
    int places = (int)places64;
    if (!y) return ray_error("type", "-27!: nil float argument");
    if (y->type == -RAY_F64) {                 /* float atom -> one string */
        return q_bang_fmt_one(places, y->f64);
    }
    if (y->type == RAY_F64) {                  /* float vector -> list of strings */
        int64_t len = ray_len(y);
        const double* d = (const double*)ray_data(y);
        ray_t* out = ray_list_new(len);
        for (int64_t i = 0; i < len; i++) {
            ray_t* s = q_bang_fmt_one(places, d[i]);
            if (RAY_IS_ERR(s)) { ray_release(out); return s; }
            ray_list_append(out, s);
            ray_release(s);
        }
        return out;
    }
    return ray_error("type", "-27!: y");
}

/* Dispatch a q internal function `-N!`.  See q_bang.h for the operand contract.
 * Borrowed `y`; returns an OWNED value or error. */
ray_t* q_bang_dispatch(int64_t id, ray_t* y) {
    switch (id) {
        case -1:  return q_hsym_wrap(y);
        case -2:  return q_attr_wrap(y);
        case -3:  return h_s1(y);
        case -5:  return q_parse_builtin_fn(y);
        case -6:  return q_value_wrap(y);
        case -8:  return q_wire_serialize(y, Q_WIRE_ASYNC);
        case -9:  return h_deser(y);
        case -15: return q_md5_fn(y);
        case -16: return h_refcnt(y);
        case -18: return h_zip(y);
        case -27: return h_format(y);
        case -29: return q_json_deserialize(y);
        case -31: return q_json_serialize(y);
        case -32: return q_dotq_btoa_fn(y);
        case -33: return q_dotq_sha1_fn(y);

        /* ---- placeholder inventory: known internal fn, not implemented -> 'nyi.
         * Kept as EXPLICIT cases so the -N! id map stays documented in code.
         * (No per-verb string — bare 'nyi, per the no-embedded-error-strings
         * ruling; the comment carries the id's doc name / blocking reason.) */
        case -4:   /* tokens: scanner token list                                */
        case -7:   /* hcount: file size                                         */
        case -10:  /* type enum: enumerations                                   */
        case -11:  /* streaming execute: logging + .z.ps                        */
        case -12:  /* .Q.host: ip -> hostname                                   */
        case -13:  /* .Q.addr: ip/host -> int                                   */
        case -14:  /* quote escape: CSV quote escaping                          */
        case -19:  /* set / compress file (AMBIGUOUS doc — see PR Deferrals)    */
        case -20:  /* .Q.gc: garbage collect                                    */
        case -21:  /* compression stats: codec + file compress                 */
        case -22:  /* uncompressed length: serde length shortcut               */
        case -23:  /* memory map: mmap-backed objects                          */
        case -24:  /* reval: restricted eval                                    */
        case -25:  /* async broadcast: IPC handles + loop                      */
        case -26:  /* SSL: TLS                                                  */
        case -30:  /* deferred response: IPC + .z.w/.z.W                       */
        case -34:  /* .Q.ts: time and space                                     */
        case -35:  /* .Q.gz: gzip                                               */
        case -36:  /* load master key: OpenSSL/DARE                            */
        case -37:  /* .Q.prf0: code profiler                                    */
        case -38:  /* socket table: sockets                                     */
        case -120: /* memory domain: .m namespace                               */
            return ray_error("nyi", NULL);

        default:   /* unknown id -> not an internal function -> 'nyi           */
            return ray_error("nyi", NULL);
    }
}
