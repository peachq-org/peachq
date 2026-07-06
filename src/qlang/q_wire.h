/* q_wire — kdb IPC wire-format codec: ray_t* ⇄ kdb bytes, both directions.
 *
 * CLEAN ROOM: implemented from the published reference docs only —
 * qdocs/docs/docs/docs/kb/serialization.md (golden byte dumps) and
 * qdocs/docs/docs/docs/basics/ipc.md (8-byte header, handshake), with
 * javakdb c.java (Apache-2.0, github.com/KxSystems/javakdb) as the cleared
 * interactive reference (2026-07-06).  Never derived from a kdb+ binary.
 *
 * Message layout (basics/ipc.md):
 *   byte 0    endianness of the encoder: 0x01 little, 0x00 big
 *   byte 1    msgtype: 0 async, 1 sync, 2 response
 *   byte 2    compressed flag — Phase A NEVER sets it on emit; a nonzero
 *             inbound flag is refused with 'nyi (compression is Phase F)
 *   byte 3    0x00
 *   bytes 4-7 total message length (int32, includes these 8 header bytes)
 *   bytes 8.. payload (one serialized object)
 *
 * Object grammar (kb/serialization.md):
 *   atom     := typebyte(int8, negative) payload      ; no attribute byte
 *   vector   := typebyte(1..19) attrs(1) count(int32) payload
 *   list 0h  := 0x00 attrs(1) count(int32) object*
 *   dict 99h := 0x63 object(keys) object(vals)        ; 0x7f sorted reads same
 *   table    := 0x62 attrs(1) 0x63 symvector(names) list(columns)
 *   lambda   := 0x64 context-cstr(root = "\0") charvector(source)
 *   error    := 0x80 cstr(code)
 *   (::)     := 0x65 0x00                             ; generic null 101h
 *
 * openq mapping notes (q-observable target, divergences documented):
 *   - Type tags are ALREADY kdb's numbers (include/rayforce.h) — no seam.
 *   - RAY_STR atom of length 1 emits a char ATOM (-10h), matching kdb's
 *     "a"; any other length emits a char vector (10h).  openq has no char
 *     atom type, so -10h decodes to a 1-char string.
 *   - A RAY_STR *vector* (string column) emits a general list of char
 *     vectors (kdb's list-of-strings shape).  Decoding yields the boxed
 *     RAY_LIST-of-strings form — the same value openq's own ("ab";"cd")
 *     literal builds; byte-level roundtrip identity holds.
 *   - RAY_TIMESTAMP is i64 nanoseconds since 2000-01-01 in every live
 *     producer path — identical to kdb 12h; raw payload, no conversion.
 *     RAY_DATE (days since 2000-01-01) and RAY_TIME (ms since midnight)
 *     are also bit-identical to kdb 14h/19h.
 *   - Sentinel nulls map 1:1 (INT_MIN family / NaN).  The reader
 *     canonicalizes inbound ±Inf floats to NaN (the engine's sentinel-null
 *     float model has no 0w) and sets RAY_ATTR_HAS_NULLS when a decoded
 *     fixed-width vector contains sentinels (invariant 16.4, vec.h).
 *   - Attribute bytes emit as 0x00 (openq has no s/u/p/g attrs yet) and
 *     are ignored on read; sorted-dict 0x7f decodes as a plain dict.
 *   - Lambdas serialize their SOURCE text (the .q.lambda carrier's `src`);
 *     decoding re-evaluates it (q_parse → q_lower → ray_eval), so it
 *     requires a warm q registry (runtime-only, like the `value` wrapper).
 *   - Engine values with no kdb tag (builtin fn values, projections,
 *     RAY_INDEX, graph/HNSW handles, lazies) refuse the wire with 'nyi.
 *     Type-tag range 200..250 is RESERVED for journal-only extension
 *     records (serde v5, Phase B); the wire path never emits or accepts
 *     tags in that range.
 */
#ifndef Q_WIRE_H
#define Q_WIRE_H

#include <rayforce.h>
#include <stddef.h>
#include <stdint.h>

/* msgtype byte values (kdb protocol) */
enum { Q_WIRE_ASYNC = 0, Q_WIRE_SYNC = 1, Q_WIRE_RESP = 2 };

/* First tag reserved for journal-only extension records (Phase B). */
#define Q_WIRE_EXT_TAG_FIRST 200
#define Q_WIRE_EXT_TAG_LAST  250

/* Serialize x into a complete kdb IPC message (8-byte LE header + payload).
 * Returns an owned RAY_U8 vector, or a RAY_ERROR ('nyi for unwireable
 * values, 'limit for >2GB frames). */
ray_t* q_wire_serialize(ray_t* x, uint8_t msgtype);

/* Deserialize a complete kdb IPC message.  Accepts either endianness
 * (header byte 0); a nonzero compressed flag is 'nyi; the payload must
 * decode to exactly one object consuming the whole frame.  `bytes` must
 * be a RAY_U8 vector.  Returns an owned value or a RAY_ERROR. */
ray_t* q_wire_deserialize(ray_t* bytes);

/* ---- payload-only entry points (Phase B serde v5 / Phase C ipc.c) ---- */

/* Growable output buffer.  Zero-init, append via q_wire_write_obj, free
 * with q_wire_wbuf_free (safe after success or failure). */
typedef struct q_wire_wbuf {
    uint8_t* p;
    size_t   len, cap;
    ray_t*   err;      /* set on failure (owned RAY_ERROR) */
} q_wire_wbuf_t;

/* Append x's payload bytes (no message header).  0 on success; -1 on
 * failure with b->err set. */
int  q_wire_write_obj(q_wire_wbuf_t* b, ray_t* x);
void q_wire_wbuf_free(q_wire_wbuf_t* b);

/* Decode ONE object from buf[0..len).  swap = nonzero when the frame was
 * encoded big-endian.  On success *consumed is the byte count read.
 * Returns an owned value or a RAY_ERROR. */
ray_t* q_wire_read_obj(const uint8_t* buf, size_t len, size_t* consumed, int swap);

#endif /* Q_WIRE_H */
