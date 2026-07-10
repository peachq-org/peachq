/* q_bang — see q_bang.h.  The `-N!` internal-function manifest + dispatch core,
 * mirroring the q_sys.c Q_SYS[] idiom but with a VALUE-shaped handler signature
 * (`ray_t* (*)(ray_t** args, int64_t n)`) because `-N!` handlers take ray_t
 * VALUES, not string slices.  Group 1 wires the thin aliases whose single home
 * is a verb/keyword openq already has (Direction B); the active-band handlers
 * (`-3! -4! -14! -16! -27! -33!`, no keyword twin) are BLOCKED rows here and
 * land in Group 2 stacked on this branch. */
#include "qlang/q_bang.h"
#include "qlang/q_registry.h"   /* q_value_wrap, q_hsym_wrap, q_attr_wrap */
#include "qlang/q_builtins.h"   /* q_parse_builtin_fn, q_md5_fn, q_dotq_btoa_fn */
#include "qlang/q_json.h"       /* q_json_serialize (.j.j), q_json_deserialize (.j.k) */
#include "qlang/q_wire.h"       /* q_wire_serialize / q_wire_deserialize, Q_WIRE_ASYNC */
#include <rayforce.h>
#include <stdint.h>

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
static ray_t* h_jk   (ray_t** a, int64_t n) { (void)n; return q_json_deserialize(a[0]); }   /* -29! */
static ray_t* h_jj   (ray_t** a, int64_t n) { (void)n; return q_json_serialize(a[0]); }     /* -31! */
static ray_t* h_btoa (ray_t** a, int64_t n) { (void)n; return q_dotq_btoa_fn(a[0]); }       /* -32! */

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
    { -15, "md5",        1, Q_BANG_F_NONE, h_md5   },
    { -29, ".j.k",       1, Q_BANG_F_NONE, h_jk    },
    { -31, ".j.j",       1, Q_BANG_F_NONE, h_jj    },
    { -32, ".Q.btoa",    1, Q_BANG_F_NONE, h_btoa  },

    /* ---- blocked / deferred inventory (NULL handler -> 'nyi) ---------------
     * Active band (no keyword twin) — Group 2 lands the doable ones here: */
    { -3,  ".Q.s1",              1, Q_BANG_F_BLOCKED, NULL },  /* string repr */
    { -4,  "tokens",             1, Q_BANG_F_BLOCKED, NULL },  /* scanner token list */
    { -14, "quote escape",       1, Q_BANG_F_BLOCKED, NULL },  /* CSV quote escaping */
    { -16, "ref count",          1, Q_BANG_F_BLOCKED, NULL },  /* variable refcount */
    { -27, "format",             2, Q_BANG_F_BLOCKED, NULL },  /* IEEE754 precision fmt */
    { -33, "SHA-1 hash",         1, Q_BANG_F_BLOCKED, NULL },  /* needs SHA-1 impl */
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
    { -18, "compress bytes",     1, Q_BANG_F_BLOCKED, NULL },  /* compression codec */
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
