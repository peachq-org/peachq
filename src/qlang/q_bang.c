/* q_bang — see q_bang.h.  The `-N!` internal-function manifest + dispatch core,
 * mirroring the q_sys.c Q_SYS[] idiom but with a VALUE-shaped handler signature
 * (`ray_t* (*)(ray_t** args, int64_t n)`) because `-N!` handlers take ray_t
 * VALUES, not string slices.  Group 1 wires the thin aliases whose single home
 * is a verb/keyword openq already has (Direction B); the active-band handlers
 * (`-3! -4! -14! -16! -27! -33!`, no keyword twin) are BLOCKED rows here and
 * land in Group 2 stacked on this branch. */
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

/* ---- per-id handlers (VALUE-shaped: borrowed args, OWNED return) -----------
 * Each is a THIN ALIAS: it calls the SAME STABLE C impl the keyword is bound
 * to (Direction B, single home).  Routing to the C function rather than the
 * env value is deliberate — kdb keeps `-N!` as the primitive UNDERNEATH the
 * keyword, so a user `parse:{…}` / `md5:{…}` rebind must NOT alter `-5!`/`-15!`.
 * (The env keyword value is the mutable q-level alias; the C fn is the home.) */
static ray_t* h_hsym (ray_t** a, int64_t n) { (void)n; return q_hsym_wrap(a[0]); }          /* -1!  */
static ray_t* h_attr (ray_t** a, int64_t n) { (void)n; return q_attr_wrap(a[0]); }          /* -2!  */
static ray_t* h_parse(ray_t** a, int64_t n) { (void)n; return q_parse_builtin_fn(a[0]); }   /* -5!  */
static ray_t* h_value(ray_t** a, int64_t n) { (void)n; return q_value_wrap(a[0]); }         /* -6!  */
static ray_t* h_ser  (ray_t** a, int64_t n) { (void)n; return q_wire_serialize(a[0], Q_WIRE_ASYNC); } /* -8! */
static ray_t* h_deser(ray_t** a, int64_t n) {                                               /* -9!  */
    (void)n;
    if (!a[0] || a[0]->type != RAY_U8)
        return ray_error("type", "-9!: expects a byte vector");
    return q_wire_deserialize(a[0]);
}
static ray_t* h_md5  (ray_t** a, int64_t n) { (void)n; return q_md5_fn(a[0]); }             /* -15! */
static ray_t* h_zip  (ray_t** a, int64_t n) {                                               /* -18! */
    (void)n;
    ray_t* f = q_wire_serialize(a[0], Q_WIRE_ASYNC);
    if (!f || RAY_IS_ERR(f)) return f;
    ray_t* z = q_wire_compress(f);      /* >2000b + under-half, else f unchanged */
    ray_release(f);
    return z;
}
static ray_t* h_jk   (ray_t** a, int64_t n) { (void)n; return q_json_deserialize(a[0]); }   /* -29! */
static ray_t* h_jj   (ray_t** a, int64_t n) { (void)n; return q_json_serialize(a[0]); }     /* -31! */
static ray_t* h_btoa (ray_t** a, int64_t n) { (void)n; return q_dotq_btoa_fn(a[0]); }       /* -32! */
static ray_t* h_sha1 (ray_t** a, int64_t n) { (void)n; return q_dotq_sha1_fn(a[0]); }       /* -33! */

/* ---- Group 2 active-band handlers (no keyword twin) -----------------------
 * These are the PRIMITIVE homes (kdb has no keyword alias for them), so unlike
 * the Direction-B thin aliases above they carry their own small C impl. */

/* -3!x — single-line string representation (== `.Q.s1`).  Routes to the
 * existing q_fmt_krepr single-line formatter and returns its buffer as a q
 * string (RAY_STR).  Byte-for-byte the same text `0N!x` shows.
 * q_fmt_krepr truncates silently into a caller buffer, so grow-and-retry until
 * the whole repr fits (kdb returns the full string) — cap the growth to stay
 * bounded.  (q_fmt_krepr still caps each NESTED-list element at its own 2048
 * internal buffer; that residual limit is shared with `0N!`/`.Q.s1` and out of
 * scope here.) */
static ray_t* h_s1(ray_t** a, int64_t n) {
    (void)n;
    size_t cap = 8192;
    char* buf = malloc(cap);
    if (!buf) return ray_error("wsfull", "-3!: out of memory");
    for (;;) {
        buf[0] = '\0';
        q_fmt_krepr(a[0], buf, cap);
        size_t len = strlen(buf);
        if (len < cap - 1 || cap >= (1u << 24)) {   /* fit whole (or growth cap) */
            ray_t* r = ray_str(buf, len);
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
static ray_t* h_refcnt(ray_t** a, int64_t n) {
    (void)n;
    if (!a[0]) return ray_error("type", "-16!: nil argument");
    return ray_i64((int64_t)a[0]->rc);
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
    if (m < 0) return ray_str("", 0);
    if ((size_t)m < sizeof stackbuf) return ray_str(stackbuf, (size_t)m);
    char* heap = malloc((size_t)m + 1);
    if (!heap) return ray_error("wsfull", "-27!: out of memory");
    snprintf(heap, (size_t)m + 1, "%.*f", places, y);
    ray_t* r = ray_str(heap, (size_t)m);
    free(heap);
    return r;
}

/* -27!(x;y) — IEEE754 precision format.  `x` is an int atom (decimal places),
 * `y` a float atom or float vector; returns a string (atom) or list of strings
 * (vector), formatted with IEEE754 rounding, ignoring \P. */
static ray_t* h_format(ray_t** a, int64_t n) {
    (void)n;
    int64_t places64;
    if (a[0] && a[0]->type == -RAY_BOOL) places64 = a[0]->b8;
    else if (!q_strict_i64(a[0], &places64))
        return ray_error("type", "-27!: places");
    if (places64 < 0) places64 = 0;
    if (places64 > 320) places64 = 320;       /* guard the snprintf width */
    int places = (int)places64;
    ray_t* y = a[1];
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

/* ---- the single-source manifest -------------------------------------------
 * {id (negative), name (doc heading of the id / its keyword twin), valence
 * (1 = `-N!x`, 2 = `-N!(x;y)`), flags, handler}.  A BLOCKED row (NULL handler)
 * enumerates a known internal fn openq does not implement yet -> 'nyi. */
enum {
    Q_BANG_F_NONE    = 0,
    Q_BANG_F_BLOCKED = 0x01,   /* known internal fn, not implemented -> 'nyi */
};

typedef ray_t* (*q_bang_handler)(ray_t** args, int64_t n);

static const struct {
    int16_t        id;
    const char*    name;
    uint8_t        valence;
    uint8_t        flags;
    q_bang_handler handler;    /* NULL <=> blocked */
} Q_BANG[] = {
    /* ---- wired: thin aliases to the single-home verb/keyword (Direction B) -- */
    { -1,  "hsym",       1, Q_BANG_F_NONE, h_hsym  },
    { -2,  "attr",       1, Q_BANG_F_NONE, h_attr  },
    { -5,  "parse",      1, Q_BANG_F_NONE, h_parse },
    { -6,  "value",      1, Q_BANG_F_NONE, h_value },
    { -8,  "to bytes",   1, Q_BANG_F_NONE, h_ser   },
    { -9,  "from bytes", 1, Q_BANG_F_NONE, h_deser },
    { -18, "compress bytes", 1, Q_BANG_F_NONE, h_zip },
    { -15, "md5",        1, Q_BANG_F_NONE, h_md5   },
    { -29, ".j.k",       1, Q_BANG_F_NONE, h_jk    },
    { -31, ".j.j",       1, Q_BANG_F_NONE, h_jj    },
    { -32, ".Q.btoa",    1, Q_BANG_F_NONE, h_btoa  },

    /* ---- Group 2: active-band handlers (no keyword twin, own C impl) -------- */
    { -3,  ".Q.s1",              1, Q_BANG_F_NONE, h_s1     },  /* string repr */
    { -16, "ref count",          1, Q_BANG_F_NONE, h_refcnt },  /* refcount (openq rc — see PR) */
    { -27, "format",             2, Q_BANG_F_NONE, h_format },  /* IEEE754 precision fmt */
    { -33, "SHA-1 hash",         1, Q_BANG_F_NONE, h_sha1   },  /* SHA-1 -> 20-byte digest */

    /* ---- blocked / deferred inventory (NULL handler -> 'nyi) ---------------
     * Active band, still blocked (clean-room / support gaps — see PR Deferrals): */
    { -4,  "tokens",             1, Q_BANG_F_BLOCKED, NULL },  /* scanner token list */
    { -14, "quote escape",       1, Q_BANG_F_BLOCKED, NULL },  /* CSV quote escaping */
    /* Replaced band whose keyword openq lacks OR whose doc is ambiguous: */
    { -7,  "hcount",             1, Q_BANG_F_BLOCKED, NULL },  /* file size */
    { -12, ".Q.host",            1, Q_BANG_F_BLOCKED, NULL },  /* ip->hostname */
    { -13, ".Q.addr",            1, Q_BANG_F_BLOCKED, NULL },  /* ip/host->int */
    { -19, "set / compress file",1, Q_BANG_F_BLOCKED, NULL },  /* AMBIGUOUS: see Deferrals */
    { -20, ".Q.gc",              1, Q_BANG_F_BLOCKED, NULL },  /* garbage collect */
    { -24, "reval",              1, Q_BANG_F_BLOCKED, NULL },  /* restricted eval */
    { -34, ".Q.ts",              1, Q_BANG_F_BLOCKED, NULL },  /* time and space */
    { -35, ".Q.gz",              1, Q_BANG_F_BLOCKED, NULL },  /* gzip */
    { -37, ".Q.prf0",            1, Q_BANG_F_BLOCKED, NULL },  /* code profiler */
    /* Blocked on a subsystem openq does not have (IPC/TLS/codec/enums/DARE): */
    { -10, "type enum",          1, Q_BANG_F_BLOCKED, NULL },  /* enumerations */
    { -11, "streaming execute",  1, Q_BANG_F_BLOCKED, NULL },  /* logging + .z.ps */
    { -21, "compression stats",  1, Q_BANG_F_BLOCKED, NULL },  /* codec + file compress */
    { -22, "uncompressed length",1, Q_BANG_F_BLOCKED, NULL },  /* serde length shortcut */
    { -23, "memory map",         1, Q_BANG_F_BLOCKED, NULL },  /* mmap-backed objects */
    { -25, "async broadcast",    2, Q_BANG_F_BLOCKED, NULL },  /* IPC handles + loop */
    { -26, "SSL",                1, Q_BANG_F_BLOCKED, NULL },  /* TLS */
    { -30, "deferred response",  1, Q_BANG_F_BLOCKED, NULL },  /* IPC + .z.w/.z.W */
    { -36, "load master key",    2, Q_BANG_F_BLOCKED, NULL },  /* OpenSSL/DARE */
    { -38, "socket table",       1, Q_BANG_F_BLOCKED, NULL },  /* sockets */
    { -120,"memory domain",      1, Q_BANG_F_BLOCKED, NULL },  /* .m namespace */
};

ray_t* q_bang_dispatch(int64_t id, ray_t* y) {
    const size_t nrows = sizeof Q_BANG / sizeof Q_BANG[0];
    for (size_t i = 0; i < nrows; i++) {
        if (Q_BANG[i].id != id) continue;
        if (!Q_BANG[i].handler || (Q_BANG[i].flags & Q_BANG_F_BLOCKED))
            break;                 /* known-but-blocked -> the shared 'nyi below */
        if (Q_BANG[i].valence <= 1) {
            ray_t* args[1] = { y };
            return Q_BANG[i].handler(args, 1);
        }
        /* valence 2: the operand is a 2-element list `(x;y)` */
        if (!y || y->type != RAY_LIST || ray_len(y) != 2)
            return ray_error("type",
                "internal function %lld! expects a 2-element argument list",
                (long long)id);
        ray_t* args[2] = { ray_list_get(y, 0), ray_list_get(y, 1) };
        return Q_BANG[i].handler(args, 2);
    }
    return ray_error("nyi",
        "internal function %lld! not yet implemented", (long long)id);
}
