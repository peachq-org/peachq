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
 *   byte 2    compressed flag — set by q_wire_compress (Phase F codec below)
 *             when ipc.c's send path elects to compress; inbound 0x01
 *             decompresses.  q_wire_serialize itself always emits 0 (it has
 *             no connection context); compression is a send-path decision.
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
 *     Char vectors ARE RAY_CHARV (tag 10, string-C3 1b): emit is the raw
 *     tag, decode of 10h/-10h builds charv/char atoms.  Internal RAY_STR
 *     atoms (length 1 -> -10h, else 10h) still emit kdb-true bytes; on the
 *     live wire into a PURE-RAYFALL process (no q runtime) decode keeps the
 *     legacy 1-char-string form (the dialect seam, eval.h probe).
 *   - A RAY_STR *vector* (string column) emits a general list of char
 *     vectors (kdb's list-of-strings shape); decoding yields the boxed
 *     list-of-charv form; byte-level roundtrip identity holds.
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

/* Serde-only extension tag band (Phase B).  UNSIGNED byte values 200..236:
 * everything >= 237 (0xed..0xff) is a kdb ATOM tag after the signed cast
 * (-19..-1) and 0x80/0x7f are error/sorted-dict — the band must stay clear
 * of them.  The WIRE path ((-8!/-9!, sockets) never emits these and rejects
 * them with 'domain; only serde mode (storage/journal) uses them:
 *   200  builtin fn      := 0xC8 valence(1: 101|102|103) name-cstr
 *   201  engine lambda   := 0xC9 attrs(1) obj(params) obj(body)
 *   202  str vector      := 0xCA attrs(1) count(int32) [len(int32) bytes]*
 *   203  quoted sym atom := 0xCB cstr
 *   204  typed-null atom := 0xCC type(int8)   ; BOOL/U8 aux-bit nulls
 *   205  str atom        := 0xCD len(int32) bytes   ; internal -RAY_STR carrier
 * 206..236 reserved for future serde records. */
#define Q_WIRE_EXT_TAG_FIRST 200
#define Q_WIRE_EXT_TAG_LAST  236
#define Q_WIRE_EXT_FN        200
#define Q_WIRE_EXT_LAMBDA    201
#define Q_WIRE_EXT_STRVEC    202
#define Q_WIRE_EXT_QSYM      203
#define Q_WIRE_EXT_TNULL     204
#define Q_WIRE_EXT_STRATOM   205   /* serde-only: internal -RAY_STR atom (len+bytes) */

/* Serialize x into a complete kdb IPC message (8-byte LE header + payload).
 * Returns an owned RAY_U8 vector, or a RAY_ERROR ('nyi for unwireable
 * values, 'limit for >2GB frames). */
ray_t* q_wire_serialize(ray_t* x, uint8_t msgtype);

/* Deserialize a complete kdb IPC message.  Accepts either endianness
 * (header byte 0) and compressed frames (byte 2 == 1); the payload must
 * decode to exactly one object consuming the whole frame.  `bytes` must
 * be a RAY_U8 vector.  Returns an owned value or a RAY_ERROR. */
ray_t* q_wire_deserialize(ray_t* bytes);

/* ---- Phase F compression codec (javakdb c.java scheme) ----
 * Compress a complete frame: returns the compressed frame when the input is
 * >2000 bytes AND compresses to under half, else the input retained
 * unchanged (kdb's threshold / give-up rules).  Owned RAY_U8 or RAY_ERROR. */
ray_t* q_wire_compress(ray_t* frame);

/* Decompress a compressed frame's payload (the bytes AFTER the 8-byte
 * header).  Bounds-checked against corrupt/bomb frames ('domain).  Returns
 * an owned RAY_U8 holding the uncompressed payload, or a RAY_ERROR. */
ray_t* q_wire_uncompress_payload(const uint8_t* pl, size_t plen, int frame_be);

/* ---- payload-only entry points (Phase B serde v5 / Phase C ipc.c) ---- */

/* Growable output buffer.  Zero-init, append via q_wire_write_obj, free
 * with q_wire_wbuf_free (safe after success or failure). */
typedef struct q_wire_wbuf {
    uint8_t* p;
    size_t   len, cap;
    ray_t*   err;      /* set on failure (owned RAY_ERROR) */
    int      serde;    /* serde mode: ext tags on, list collapse off */
} q_wire_wbuf_t;

/* Append x's payload bytes (no message header).  0 on success; -1 on
 * failure with b->err set. */
int  q_wire_write_obj(q_wire_wbuf_t* b, ray_t* x);
void q_wire_wbuf_free(q_wire_wbuf_t* b);

/* Decode ONE object from buf[0..len).  swap = nonzero when the frame was
 * encoded big-endian.  On success *consumed is the byte count read.
 * Returns an owned value or a RAY_ERROR. */
ray_t* q_wire_read_obj(const uint8_t* buf, size_t len, size_t* consumed, int swap);

/* ---- serde mode (RAY_SERDE_WIRE_VERSION 5, storage/journal) ----
 * Same grammar as the wire PLUS the extension band above, MINUS the wire's
 * q-observable normalizations: boxed lists are never collapsed (engine
 * boxing roundtrips exactly), RAY_STR vectors keep their type (ext 202),
 * builtin fns / raw lambdas / quoted syms / BOOL-U8 typed nulls use their
 * ext records.  Vector/list attr bytes carry the engine attrs (HAS_NULLS /
 * QUOTED restored on read; SLICE never restored).  Payloads are always
 * emitted little-endian. */
int    q_wire_write_obj_ex(q_wire_wbuf_t* b, ray_t* x, int serde);
ray_t* q_wire_read_obj_ex(const uint8_t* buf, size_t len, size_t* consumed,
                          int swap, int serde);

#endif /* Q_WIRE_H */
