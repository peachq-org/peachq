/* q_bang — the single C home for the `!` verb.  Principles:
 *  - PURE value semantics: every function maps values -> values with no env or
 *    runtime state, so the whole `!` surface is unit-testable in isolation.
 *  - Amend-by-reference (`N!`name`) is deliberately NOT here — it needs global
 *    env read/write, so it belongs to a runtime amend layer, not the verb.
 *  - q_bang_dispatch owns every INT left operand: 0N show, negative internal
 *    fns (the `-N!` home; q names delegate `name:-N!`), N>=0 enkey; else dict. 
 * 
 * It contains one fully generic ``ray_t* q_bang(ray_t* x, ray_t* y)`` that works on ray_t objects and exactly matches q public API.
 * and for each significant operation it exposes a specialized function with types where possible e.g. ``q_bang_dispatch(int64_t id, ray_t* y)``, ``q_bang_enkey(int64_t nkey, ray_t* y)``.
 * These more specific names are for reuse in the code base. Having the specific types also helps. 
 */
#include "qlang/ops/q_bang.h"
#include "qlang/base/q_err.h"
#include "qlang/q_registry_internal.h"  /* q_hsym_wrap, q_attr_wrap, q_type_strict_i64,
                                         * q_type_is_int_atom, q_type_iatom_val, q_table_flatten */
#include "qlang/q_builtins.h"   /* q_parse_builtin_fn, q_md5_fn, q_dotq_btoa_fn, q_dotq_sha1_fn */
#include "qlang/net/q_json.h"       /* q_json_serialize (.j.j), q_json_deserialize (.j.k) */
#include "qlang/net/q_wire.h"       /* q_wire_serialize/_deserialize/_compress, Q_WIRE_ASYNC */
#include "qlang/net/q_wirefile.h"   /* q_wirefile_stats — `-21!` compression stats */
#include "qlang/eval/q_eval.h"  /* q_eval — `-6!` (internal.md: eval) */
#include "qlang/q_fmt.h"        /* q_fmt_krepr — `-3!`, .Q.s1 */
#include "qlang/q_console.h"    /* q_console_write — 0N! */
#include "qlang/ops/q_sys.h"        /* q_sys_ts_apply — `-34!` (.Q.ts) */
#include "qlang/net/q_net.h"   /* q_net_host / q_net_addr — `-12!`/`-13!` */
#include "lang/eval.h"          /* ray_eval_get_restricted — `-7!` file gate */
#include "lang/internal.h"      /* ray_dict_fn — the `!` dict kernel */
#include <rayforce.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- multi-statement handlers (VALUE-shaped: borrowed y, OWNED return) ----- */

static ray_t* h_deser(ray_t* y) {                       /* -9! from bytes */
    if (!y || y->type != RAY_BYTE_ONLY)
        return q_err(QE_TYPE);
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
    if (!buf) return q_err(QE_WSFULL);
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
        if (!nb) { free(buf); return q_err(QE_WSFULL); }
        buf = nb;
    }
}

/* -16!x — reference count of x.  DIVERGENCE: kdb returns the number of q-level
 * aliases bound to a variable (`a:b:c:1 2 3` -> 3); openq exposes the ray_t
 * heap refcount, which counts internal owners, not source aliases.  The ledger
 * pins openq's ACTUAL number (smoked), and the PR Decisions flag the gap — we
 * do NOT fabricate kdb's count. */
static ray_t* h_refcnt(ray_t* y) {
    if (!y) return q_err(QE_TYPE);
    return ray_i64((int64_t)y->rc);
}

/* -7!x hcount — ref/hcount.md: file symbol -> size in bytes as a long.  Path
 * norms follow the io family: `:path file sym (leading ':' stripped), charv
 * path tolerated like hopen.  A missing/unstatable file -> 'io (kdb pins
 * path:oserr, unrepresentable in 7-byte codes — the read0 precedent). */
static ray_t* h_hcount(ray_t* y) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    ray_t* xs = q_str_in(y);            /* charv path -> legacy STR form */
    ray_t* path = NULL;                 /* OWNED RAY_STR, NUL-terminated */
    const char* p = NULL;
    size_t n = 0;
    if (xs && xs->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(xs->i64);               /* borrowed */
        if (s) { p = ray_str_ptr(s); n = ray_str_len(s); }
    } else if (xs && xs->type == -RAY_STR) {
        p = ray_str_ptr(xs);
        n = ray_str_len(xs);
    }
    if (p) {                            /* empty path stats and fails -> 'io */
        if (n > 0 && p[0] == ':') { p++; n--; }
        path = ray_str(p, n);
    }
    ray_release(xs);
    if (!path) return q_err(QE_TYPE);
    if (RAY_IS_ERR(path)) return path;
    int64_t sz = -1;
#ifdef RAY_OS_WINDOWS
    struct _stat64 st;                  /* 64-bit twin: st_size past 2 GiB */
    if (_stat64(ray_str_ptr(path), &st) == 0) sz = (int64_t)st.st_size;
#else
    struct stat st;
    if (stat(ray_str_ptr(path), &st) == 0) sz = (int64_t)st.st_size;
#endif
    ray_release(path);
    if (sz < 0) return q_err(QE_IO);
    return ray_i64(sz);
}

/* -34!(f;args) — .Q.ts, functional `\ts` (ref/dotq.md#ts-time-and-space).
 * The one operand packs Apply's two: unpack and delegate to the q_sys home. */
static ray_t* h_timespace(ray_t* y) {
    if (!y || y->type != RAY_LIST || ray_len(y) != 2)
        return q_err(QE_TYPE);
    return q_sys_ts_apply(ray_list_get(y, 0), ray_list_get(y, 1));
}

/* Format one double to `places` decimals with IEEE754 rounding (C's %.*f is
 * exactly round-half-to-even on the stored binary value — matching kdb's
 * `-27!`, which ignores \P).  Returns an owned RAY_STR (or 'wsfull error).
 * A large-magnitude float can need more than the stack buffer (the integer
 * part is unbounded), so allocate exactly what snprintf reports rather than
 * truncating. */
static ray_t* bang_fmt_one(int places, double y) {
    char stackbuf[512];
    int m = snprintf(stackbuf, sizeof stackbuf, "%.*f", places, y);
    if (m < 0) return ray_charv("", 0);
    if ((size_t)m < sizeof stackbuf) return ray_charv(stackbuf, (int64_t)m);
    char* heap = malloc((size_t)m + 1);
    if (!heap) return q_err(QE_WSFULL);
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
        return q_err(QE_TYPE);
    ray_t* px = ray_list_get(arg, 0);    /* borrowed */
    ray_t* y  = ray_list_get(arg, 1);    /* borrowed */
    int64_t places64;
    if (px && px->type == -RAY_BOOL) places64 = px->b8;
    else if (!q_type_strict_i64(px, &places64))
        return q_err(QE_TYPE);
    if (places64 < 0) places64 = 0;
    if (places64 > 320) places64 = 320;       /* guard the snprintf width */
    int places = (int)places64;
    if (!y) return q_err(QE_TYPE);
    if (y->type == -RAY_F64) {                 /* float atom -> one string */
        return bang_fmt_one(places, y->f64);
    }
    if (y->type == RAY_F64) {                  /* float vector -> list of strings */
        int64_t len = ray_len(y);
        const double* d = (const double*)ray_data(y);
        ray_t* out = ray_list_new(len);
        for (int64_t i = 0; i < len; i++) {
            ray_t* s = bang_fmt_one(places, d[i]);
            if (RAY_IS_ERR(s)) { ray_release(out); return s; }
            ray_list_append(out, s);
            ray_release(s);
        }
        return out;
    }
    return q_err(QE_TYPE);
}

/* ---- the non-negative band: enkey / dict-make ------------------------------ */

/* q enkey/unkey `N!table`: 0 -> plain table (unkey), N>0 -> key the first N
 * columns into a keyed table (RAY_DICT keycols-table -> valcols-table).
 * Accepts a plain OR already-keyed table (re-keys).  Consumes nothing. */
ray_t* q_bang_enkey(int64_t nkey, ray_t* y) {
    if (!y || (y->type != RAY_TABLE && !q_type_is_keyed(y)))
        return q_err(QE_TYPE);
    ray_t* flat = q_table_flatten(y);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    int64_t nc = ray_table_ncols(flat);
    if (nkey <= 0) return flat;                 /* unkey */
    if (nkey >= nc) { ray_release(flat); return q_err(QE_LENGTH); }
    ray_t* kt = ray_table_new(nkey);
    ray_t* vt = ray_table_new(nc - nkey);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(kt) && !RAY_IS_ERR(vt); c++) {
        int64_t nm = ray_table_col_name(flat, c);
        ray_t* col = ray_table_get_col_idx(flat, c);
        if (c < nkey) kt = ray_table_add_col(kt, nm, col);
        else          vt = ray_table_add_col(vt, nm, col);
    }
    ray_release(flat);
    if (RAY_IS_ERR(kt)) { ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);
}

/* q `x!y` — dict make.  One `count` gate covers vector!vector AND table!table
 * (rows).  An ATOM side enlists to its 1-item list — the SCALAR dict
 * (ref/key.md pins `key `a _ `a!1` -> `symbol$()`; the table-literal tree
 * (+:;(!;,`x;(enlist;e))) evaluates through this arm).  ()!() is the empty
 * dict; a keyed table is a table!table dict.  vals pass through as-is
 * (rayfall `dict` broadcasts/boxes). */
static ray_t* bang_make_dict(ray_t* x, ray_t* y) {
    if (q_builtins_count_long(x) != q_builtins_count_long(y))
        return q_err(QE_LENGTH);
    /* kdb's rule is TOTAL on equal counts (basics/dictsandtables.md): a table
     * or dict on either side pairs as-is — table!table is the keyed table,
     * `t!list` keys by rows, `list!t` the xtab shape (ref/exec.md). */
    if (q_type_is_table(x) || q_type_is_table(y) ||
        q_type_is_dict(x) || q_type_is_dict(y)) {
        ray_retain(x);
        ray_retain(y);
        return ray_dict_new(x, y);               /* consumes both retains */
    }
    /* An EMPTY dict is built directly: rayfall's dict rejects the empty general
     * list (`()!()`) and drops a typed empty's domain, but `` type value
     * (`long$())!`symbol$() `` must stay 11h. */
    if (q_builtins_count_long(y) == 0) {
        ray_retain(x);
        ray_retain(y);
        return ray_dict_new(x, y);               /* consumes both retains */
    }
    ray_t* ex = NULL;
    ray_t* ey = NULL;
    if (x->type < 0) {                           /* data atom -> its 1-item list */
        ex = q_enlist_wrap(&x, 1);
        if (RAY_IS_ERR(ex)) return ex;
        x = ex;
    }
    if (y->type < 0) {
        ey = q_enlist_wrap(&y, 1);
        if (RAY_IS_ERR(ey)) { if (ex) ray_release(ex); return ey; }
        y = ey;
    }
    /* Two typed vectors make a dict whose VALUES stay that vector: kdb has no
     * boxed homogeneous list, so `type value `a`b!1 2` is 7h, not 0h.  rayfall's
     * `dict` explodes every value into a RAY_LIST — right for a mixed y, wrong
     * here, and the boxing survives into `~` (which compares the raw slots). */
    ray_t* r = (ray_is_vec(x) && ray_is_vec(y))
                   ? (ray_retain(x), ray_retain(y), ray_dict_new(x, y))
               : ((ray_is_vec(x) || x->type == RAY_LIST) &&
                  (ray_is_vec(y) || y->type == RAY_LIST))
                   ? ray_dict_fn(x, y)
                   : q_err(QE_TYPE);
    if (ex) ray_release(ex);
    if (ey) ray_release(ey);
    return r;
}


/* `0N!x` — debug print: write x's single-line k-repr to the console sink and pass
 * x through unchanged (ref/display.md; file-text.md pins the repr).  Borrowed y in,
 * OWNED y out — the retain balances the caller's release of the result. */
static ray_t* bang_show(ray_t* y) {
    char buf[8192]; buf[0] = '\0';
    q_fmt_krepr(y, buf, sizeof buf);
    q_console_write(buf, strlen(buf));
    q_console_write("\n", 1);
    ray_retain(y);
    return y;
}


/* Dispatch every INT left operand of `!`.  See q_bang.h for the contract.
 * Borrowed `y`; returns an OWNED value or error. */
ray_t* q_bang_dispatch(int64_t id, ray_t* y) {
    if (id >= 0) {
        return q_bang_enkey(id, y);
    }
    switch (id) {
        case NULL_I64: return bang_show(y);   /* 0N!x — debug print, pass through */
        case -1:  return q_hsym_wrap(y);
        case -2:  return q_attr_wrap(y);
        case -3:  return h_s1(y);
        case -5:  return q_parse_builtin_fn(y);
        case -6:  return q_eval(y);              /* internal.md: -6! is eval */
        case -7:  return h_hcount(y);
        case -8:  return q_wire_serialize(y, Q_WIRE_ASYNC);
        case -9:  return h_deser(y);
        case -12: return q_net_host(y);
        case -13: return q_net_addr(y);
        case -14: return q_io_filetext_csv_quote(y);
        case -15: return q_md5_fn(y);
        case -16: return h_refcnt(y);
        case -18: return h_zip(y);
        case -27: return h_format(y);
        case -29: return q_json_deserialize(y);
        case -31: return q_json_serialize(y);
        case -32: return q_dotq_btoa_fn(y);
        case -33: return q_dotq_sha1_fn(y);
        case -34: return h_timespace(y);
        case -21: return q_wirefile_stats(y);
        case -35: return q_dotq_gz_fn(&y, 1);

        /* ---- placeholder inventory: known internal fn, not implemented -> 'nyi.
         * Kept as EXPLICIT cases so the -N! id map stays documented in code.
         * (No per-verb string — bare 'nyi, per the no-embedded-error-strings
         * ruling; the comment carries the id's doc name / blocking reason.) */
        case -4:   /* tokens: scanner token list                                */
        case -10:  /* type enum: enumerations                                   */
        case -11:  /* streaming execute: logging + .z.ps                        */
        case -19:  /* set / compress file (AMBIGUOUS doc — see PR Deferrals)    */
        case -20:  /* .Q.gc: garbage collect                                    */
        case -22:  /* uncompressed length: serde length shortcut               */
        case -23:  /* memory map: mmap-backed objects                          */
        case -24:  /* reval: restricted eval — PARKED with `value` itself
                    * (eval-rebuild cutover); needs the RAY_FN_RESTRICTED gate
                    * inside the fresh apply module so a nested value-apply of
                    * a restricted VARY cannot bypass it (codex r1/r2 P1).      */
        case -25:  /* async broadcast: IPC handles + loop                      */
        case -26:  /* SSL: TLS                                                  */
        case -30:  /* deferred response: IPC + .z.w/.z.W                       */
        case -36:  /* load master key: OpenSSL/DARE                            */
        case -37:  /* .Q.prf0: code profiler                                    */
        case -38:  /* socket table: sockets                                     */
        case -120: /* memory domain: .m namespace                               */
            return q_err(QE_NYI);

        default:   /* unknown id -> not an internal function -> 'nyi           */
            return q_err(QE_NYI);
    }
}

/* The `!` verb (registry row in q_ops.c).  An INT atom lhs IS the dispatch band
 * (show / internal / enkey / dict) — delegated whole.  A typed null (0Nh/0Ni/0N)
 * canonicalizes to NULL_I64 so every null width lands on the one `0N!` show case;
 * a plain 0N already IS NULL_I64.  Everything else is a dict. */
ray_t* q_bang(ray_t* x, ray_t* y) {
    if (!x || !y) return q_err(QE_TYPE);
    if (q_type_is_int_atom(x))
        return q_bang_dispatch(RAY_ATOM_IS_NULL(x) ? NULL_I64 : q_type_iatom_val(x), y);
    return bang_make_dict(x, y);
}