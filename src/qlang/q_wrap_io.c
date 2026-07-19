/* q_wrap_io.c — io wrappers: hopen/hclose (IPC client), File Text (0: hsym read0,
 * save/prepare/load-csv/load-fixed/kv), like/ss/ssr, getenv/setenv
 *
 * Split from q_registry.c (2026-07-14) — pure function moves; the shared
 * internal surface lives in q_registry_internal.h.  See q_registry.h for
 * the registry contract. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* the split's shared surface — brings qlang/q_registry.h + qlang/q_ops.h */
#include "qlang/q_builtins.h" /* q_string_fn — 0: Prepare Text cell text */
#include "qlang/q_sys.h"
#include "lang/eval.h"      /* ray_eval_get_restricted, ray_read_file_fn/ray_write_file_fn */
#include "lang/internal.h"  /* ray_hopen_fn/ray_hclose_fn, ray_like_fn, ray_getenv_fn/ray_setenv_fn, make_i64 */
#include "lang/format.h"    /* ray_type_name — error messages */
#include "table/sym.h"      /* ray_sym_intern_runtime, ray_sym_vec_cell */
#include "store/fileio.h"   /* ray_mkdir_p — 0: Save Text missing dirs */
#include "core/ipc.h"       /* ray_ipc_fd_of_handle/handle_of_fd — q true-fd handles */
#include <stdio.h>          /* snprintf */
#include <string.h>
#include <stdlib.h>         /* getenv/setenv, calloc/realloc */

/* q `exit x` — terminate with exit code x (ref/exit.md; blocked during reval
 * -> 'access, kdb-true).  All processing lives in q_exit (fires `.z.exit`,
 * capability-gated): under the doctest/wasm runtimes q_exit returns and the
 * verb is a silent null — the runner survives corpus `exit 0` rows. */
ray_t* q_exit_wrap(ray_t* x) {
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    if (!q_is_int_atom(x) || RAY_ATOM_IS_NULL(x)) return ray_error("type", NULL);
    q_exit((int)q_iatom_val(x));
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* ---- IPC client verb: q `hopen` (feat/q-ipc-client, Phase D; hsym Bundle 2b) --
 * Thin wrapper over `.ipc.open` (ray_hopen_fn), which takes a
 * "host:port[:user:password]" string + optional connect-timeout.  q `hopen`
 * accepts an int PORT (localhost), a "host:port[:user:pass]" STRING or the
 * equivalent hsym SYMBOL (both with the kdb "::PORT"/":host:port" leading-colon
 * conventions — the leading-`:` scanner lexes `` `::5000 ``/`` `:host:port `` as
 * one colon-bearing symbol, so both surfaces reach q_hopen_norm_descriptor and
 * share ONE parser), or a 2-list (conn; timeout-ms).  DEFERRED (clean 'nyi, not
 * a silent TCP attempt): the transport schemes `unix://` / `tcps://` /
 * `unixs://` / `fifo://` (need a transport layer openq lacks) and FILE-handle
 * hopen (`hopen ":path"`, distinct from IPC). */

/* Normalize a "host:port[:user:password]" descriptor string (raw name text of a
 * string atom or an hsym symbol) into the form .ipc.open expects.  Applies the
 * kdb leading-colon conventions ("::rest" = localhost, ":rest" = strip one ':')
 * and rejects the not-yet-supported transport schemes with a clean 'nyi (so
 * BOTH the string and symbol surfaces decline them the same way, rather than
 * feeding e.g. "unix://5000" into a TCP connect).  Owned RAY_STR or error. */
static ray_t* q_hopen_norm_descriptor(const char* s, size_t n) {
    if (n > 512) return ray_error("domain", "hopen: descriptor too long");
    /* Strip the leading-colon marker: "::rest" localhost, single ":rest" strip 1. */
    const char* rest = s;
    size_t      rn   = n;
    bool        localhost = false;
    if (rn >= 2 && rest[0] == ':' && rest[1] == ':') { rest += 2; rn -= 2; localhost = true; }
    else if (rn >= 1 && rest[0] == ':')              { rest += 1; rn -= 1; }
    /* Deferred transports: a "scheme://" prefix after the leading colon(s). */
    static const char* const schemes[] = { "unixs://", "unix://", "tcps://", "fifo://" };
    for (size_t i = 0; i < sizeof schemes / sizeof *schemes; i++) {
        size_t sl = strlen(schemes[i]);
        if (rn >= sl && memcmp(rest, schemes[i], sl) == 0)
            return ray_error("nyi",
                             "hopen: %.*s transport not supported yet",
                             (int)(sl - 3), schemes[i]);   /* scheme sans "://" */
    }
    if (localhost) {
        char buf[600];
        int m = snprintf(buf, sizeof buf, "127.0.0.1:%.*s", (int)rn, rest);
        if (m <= 0 || m >= (int)sizeof buf)
            return ray_error("domain", "hopen: descriptor too long");
        return ray_str(buf, (size_t)m);
    }
    return ray_str(rest, rn);   /* "host:port[:user:pass]" (host omitted = as-is) */
}

/* Normalize a connection descriptor (int atom, string atom, or hsym symbol)
 * into the "host:port[:user:password]" form .ipc.open expects.  Owned RAY_STR
 * or error. */
static ray_t* q_hopen_connstr(ray_t* c) {
    if (q_is_int_atom(c)) {
        int64_t p = q_iatom_val(c);
        if (p <= 0 || p > 65535)
            return ray_error("domain", "hopen: port must be in 1..65535, got %lld",
                             (long long)p);
        char buf[32];
        int m = snprintf(buf, sizeof buf, "127.0.0.1:%lld", (long long)p);
        if (m <= 0 || m >= (int)sizeof buf) return ray_error("domain", "hopen: bad port");
        return ray_str(buf, (size_t)m);
    }
    if (c && c->type == -RAY_STR)
        return q_hopen_norm_descriptor(ray_str_ptr(c), ray_str_len(c));
    if (c && c->type == -RAY_SYM) {
        /* ray_sym_str returns a BORROWED interned string (table/sym.c) — do NOT
         * release it (that over-releases the sym table's own ref; the q_apply.c
         * `:path` precedent).  q_hopen_norm_descriptor copies out a new atom. */
        ray_t* nm = ray_sym_str(c->i64);
        if (!nm) return ray_error("type", "hopen: bad symbol handle");
        return q_hopen_norm_descriptor(ray_str_ptr(nm), ray_str_len(nm));
    }
    return ray_error("type",
                     "hopen: expected an int port or a \"host:port\" string, got %s",
                     ray_type_name(c ? c->type : 0));
}

/* q `hopen y` — connect, return an int handle.  Restricted connections must not
 * open outbound sockets (the `.ipc.open` primitive is RAY_FN_RESTRICTED; calling
 * ray_hopen_fn directly bypasses the eval-layer check, so re-assert it here). */
static ray_t* q_hopen_wrap_impl(ray_t* x);
ray_t* q_hopen_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = q_hopen_wrap_impl(xs);
    ray_release(xs);
    return r;
}
static ray_t* q_hopen_wrap_impl(ray_t* x) {
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    ray_t* conn      = x;
    ray_t* timeout   = NULL;
    ray_t* pair_conn = NULL;   /* owned when a pair was a typed int VECTOR */
    ray_t* pair_to   = NULL;
    if (x && x->type == RAY_LIST && ray_len(x) == 2) {   /* (conn; timeout-ms) */
        ray_t** e = (ray_t**)ray_data(x);
        conn = e[0]; timeout = e[1];                     /* borrowed */
    } else if (q_is_int_vec(x) && ray_len(x) == 2) {
        /* an all-int (port; timeout-ms) pair collapses to a homogeneous int
         * VECTOR (not a general list) — recover the two atoms.  (A symbol/string
         * conn keeps the pair a RAY_LIST, handled above.) */
        pair_conn = ray_i64(q_ivec_get(x, 0));
        pair_to   = ray_i64(q_ivec_get(x, 1));
        conn = pair_conn; timeout = pair_to;
    }
    ray_t* cs = q_hopen_connstr(conn);                   /* owned or error */
    if (!cs || RAY_IS_ERR(cs)) {
        if (pair_conn) ray_release(pair_conn);
        if (pair_to)   ray_release(pair_to);
        return cs;
    }
    ray_t* args[2] = { cs, NULL };
    int64_t nargs = 1;
    ray_t* tv = NULL;
    int64_t tmo = 0;
    ray_t* terr = timeout ? q_i64_or_err(timeout, &tmo, "hopen: timeout") : NULL;
    if (terr) {
        ray_release(cs);
        if (pair_conn) ray_release(pair_conn);
        if (pair_to)   ray_release(pair_to);
        return terr;
    }
    if (timeout) {
        tv = make_i64(tmo);
        args[1] = tv; nargs = 2;
    }
    ray_t* h = ray_hopen_fn(args, nargs);                /* owned handle or error */
    ray_release(cs);
    if (tv)        ray_release(tv);
    if (pair_conn) ray_release(pair_conn);
    if (pair_to)   ray_release(pair_to);
    if (!h || RAY_IS_ERR(h)) return h;
    /* kdb-faithful "true fd" handle model: a q connection handle IS the socket
     * fd (qdocs basics/handles.md — 0 console, 1 stdout, 2 stderr, connections
     * at 3+).  ray_hopen_fn returns the rayfall poll SELECTOR ID (dense, starts
     * at 0); translate it to the connection's socket fd, which is always >= 3
     * (0/1/2 held by the std streams) and thus disjoint from the console-write
     * handles.  handle-apply / hclose translate the fd back to the selector id
     * the .ipc.* primitives expect. */
    int64_t raw = (h->type == -RAY_I64) ? h->i64 : (int64_t)h->i32;
    ray_release(h);
    int64_t fd = ray_ipc_fd_of_handle(raw);
    if (fd < 0) {
        /* Connection vanished between connect and fd lookup — close the raw
         * selector and surface, rather than hand back a bogus handle. */
        ray_t* rid = make_i64(raw);
        ray_t* cr = ray_hclose_fn(rid);
        ray_release(rid);
        if (cr) ray_release(cr);
        return ray_error("io", "hopen: connection lost");
    }
    return make_i64(fd);
}

/* q `hclose h` — translate the q fd handle back to the poll selector id and
 * route to `.ipc.close` (ray_hclose_fn).  Restricted connections are refused,
 * matching hopen / the handle-apply path.  A q handle is the socket fd; map it
 * to the selector id the primitive expects.  A handle that is not a live IPC
 * connection (already closed, or a console handle) is a no-op, matching kdb's
 * tolerance of hclose on a dead handle. */
ray_t* q_hclose_wrap(ray_t* x) {
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    int64_t qh;
    if (!q_strict_i64(x, &qh) || RAY_ATOM_IS_NULL(x) || qh <= 0)
        return ray_error("type", "hclose: h");
    int64_t id = ray_ipc_handle_of_fd(qh);
    if (id < 0) return RAY_NULL_OBJ;          /* not a live handle — no-op */
    ray_t* raw = make_i64(id);
    ray_t* r = ray_hclose_fn(raw);
    ray_release(raw);
    return r;
}

/* ===== File Text: `0:` + hsym + read0 (feat/q-file-text) ====================
 * Oracle: ref/file-text.md, ref/read0.md, ref/hsym.md (CLEAN ROOM).  TEXT
 * forms only — binary file formats (1:/2:, get/set on data files) are an
 * owner-ruled NON-GOAL (2026-07-09 ruling).
 *
 * Ownership contract (plan round 1): helper inputs are BORROWED, helper
 * outputs are OWNED by the caller; on any partial failure a helper releases
 * everything it allocated before returning the error.
 *
 * RAY_FN_RESTRICTED note: the base file primitives carry the flag on their
 * ENV fn objects; calling the C functions directly bypasses the eval-layer
 * check, so every file-touching arm re-asserts ray_eval_get_restricted()
 * (the q_hopen_wrap precedent). */

/* file symbol -> OWNED RAY_STR filesystem path (leading ':' stripped), or
 * NULL when x is not a `:path symbol. */
static ray_t* q_ft_path(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return NULL;
    ray_t* s = ray_sym_str(x->i64);                       /* borrowed */
    if (!s) return NULL;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    if (n < 2 || p[0] != ':') return NULL;
    return ray_str(p + 1, n - 1);
}

/* Read a whole file (restricted-guarded).  OWNED RAY_STR or error. */
static ray_t* q_ft_read_all(ray_t* pathstr) {
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    return ray_read_file_fn(pathstr);
}

/* q `hsym x` — sym atom/vector -> file symbol: prefix ':' unless already
 * present (ref/hsym.md). */
static int64_t q_hsym_id(const char* p, size_t n) {
    if (n > 0 && p[0] == ':') return ray_sym_intern_runtime(p, n);
    char* buf = (char*)malloc(n + 1);
    if (!buf) return ray_sym_intern_runtime(":", 1);
    buf[0] = ':';
    memcpy(buf + 1, p, n);
    int64_t id = ray_sym_intern_runtime(buf, n + 1);
    free(buf);
    return id;
}
/* Exported (q_registry.h) so the `-1!` internal-fn alias single-homes here. */
static ray_t* q_hsym_wrap_impl(ray_t* x);
ray_t* q_hsym_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = q_hsym_wrap_impl(xs);
    ray_release(xs);
    return r;
}
static ray_t* q_hsym_wrap_impl(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);                   /* borrowed */
        if (!s) return ray_error("type", "hsym: bad symbol");
        const char* p = ray_str_ptr(s);
        size_t n = ray_str_len(s);
        if (n > 0 && p[0] == ':') { ray_retain(x); return x; }
        return ray_sym(q_hsym_id(p, n));
    }
    if (x && x->type == RAY_SYM) {
        int64_t n = ray_len(x);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        for (int64_t i = 0; i < n; i++) {
            ray_t* c = ray_sym_vec_cell(x, i);            /* borrowed domain atom */
            if (!c) { ray_release(out); return ray_error("type", "hsym: bad symbol"); }
            int64_t id = q_hsym_id(ray_str_ptr(c), ray_str_len(c));
            out = ray_vec_append(out, &id);
            if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        }
        return out;
    }
    return ray_error("type", "hsym: expected a symbol, got %s",
                     ray_type_name(x ? x->type : 0));
}

/* q `read0 x` — ref/read0.md.  File sym -> list of line strings (LF/CRLF
 * delimiters removed); (f;o) -> chars from offset o to EOF minus one trailing
 * line break (the doc pins `read0(`:foo;6)` -> "world" on a file ending \n);
 * (f;o;n) -> exactly n chars from o (clamped).  Console (0) and fifo handles
 * are deferred 'nyi.  Offsets accept 0 (superset of the doc wording). */
static ray_t* q_read0_wrap_impl(ray_t* x);
ray_t* q_read0_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = q_read0_wrap_impl(xs);
    ray_release(xs);
    return q_charv_out(r);              /* lines cross as char vectors */
}
static ray_t* q_read0_wrap_impl(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* path = q_ft_path(x);
        if (!path) return ray_error("type", "read0: expected a file symbol `:path");
        ray_t* all = q_ft_read_all(path);
        ray_release(path);
        if (!all || RAY_IS_ERR(all)) return all;
        ray_t* lines = q_str_split_lines(ray_str_ptr(all), ray_str_len(all));
        ray_release(all);
        return lines;
    }
    if (x && x->type == RAY_LIST && (ray_len(x) == 2 || ray_len(x) == 3)) {
        ray_t** e = (ray_t**)ray_data(x);
        int three = ray_len(x) == 3;
        if (e[0] && e[0]->type == -RAY_SYM) {
            ray_t* path = q_ft_path(e[0]);
            if (!path) return ray_error("type", "read0: x");
            int64_t off, want = -1;
            if (!q_strict_i64(e[1], &off) || (three && !q_strict_i64(e[2], &want))) {
                ray_release(path);
                return ray_error("type", "read0: offset/length");
            }
            if (off < 0 || (three && want < 0)) {
                ray_release(path);
                return ray_error("domain", "read0: offset/length");
            }
            ray_t* all = q_ft_read_all(path);
            ray_release(path);
            if (!all || RAY_IS_ERR(all)) return all;
            const char* p = ray_str_ptr(all);
            int64_t n = (int64_t)ray_str_len(all);
            if (off > n) off = n;
            int64_t end = three ? (off + want > n ? n : off + want) : n;
            if (!three) {                          /* strip one trailing \n / \r\n */
                if (end > off && p[end - 1] == '\n') end--;
                if (end > off && p[end - 1] == '\r') end--;
            }
            ray_t* out = ray_str(p + off, (size_t)(end - off));
            ray_release(all);
            return out;
        }
        return ray_error("nyi", "read0: fifo handles are deferred");
    }
    if (x && q_is_int_atom(x))
        return ray_error("nyi", "read0: console/connection handles are deferred");
    return ray_error("type", "read0: expected a file symbol or (filesymbol;offset[;length])");
}

/* ---- Save Text: `:path 0: strings -------------------------------------- */
static ray_t* q_ft_save_text(ray_t* fsym, ray_t* y) {
    ray_t* path = q_ft_path(fsym);
    if (!path) return ray_error("type", "0:: bad file symbol");
    if (ray_eval_get_restricted()) { ray_release(path); return ray_error("access", "restricted"); }
    if (!y || !(y->type == RAY_LIST || y->type == RAY_STR)) {
        ray_release(path);
        return ray_error("type", "0:: Save Text needs a list of strings");
    }
    int64_t n = ray_len(y);
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(y, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { ray_release(path); return e ? e : ray_error("oom", NULL); }
        if (e->type != -RAY_STR) {
            ray_release(e); ray_release(path);
            return ray_error("type", "0:: Save Text needs a list of strings");
        }
        total += ray_str_len(e) + 1;                       /* line + '\n' */
        ray_release(e);
    }
    char* buf = (char*)malloc(total ? total : 1);
    if (!buf) { ray_release(path); return ray_error("oom", NULL); }
    size_t w = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(y, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { free(buf); ray_release(path); return e ? e : ray_error("oom", NULL); }
        size_t l = ray_str_len(e);
        memcpy(buf + w, ray_str_ptr(e), l);
        w += l;
        buf[w++] = '\n';
        ray_release(e);
    }
    /* create missing parent directories (the doc's "any missing containing
     * directories"); ray_mkdir_p is the shared portable impl. */
    {
        const char* pp = ray_str_ptr(path);
        size_t pn = ray_str_len(path);
        size_t cut = pn;
        while (cut > 0 && pp[cut - 1] != '/') cut--;
        if (cut > 1) {
            char* dir = (char*)malloc(cut);
            if (dir) {
                memcpy(dir, pp, cut - 1);
                dir[cut - 1] = '\0';
                (void)ray_mkdir_p(dir);
                free(dir);
            }
        }
    }
    ray_t* content = ray_str(buf, w);
    free(buf);
    if (!content || RAY_IS_ERR(content)) { ray_release(path); return content ? content : ray_error("oom", NULL); }
    ray_t* r = ray_write_file_fn(path, content);
    ray_release(path);
    ray_release(content);
    if (!r || RAY_IS_ERR(r)) return r;
    ray_release(r);
    ray_retain(fsym);
    return fsym;
}

/* ---- Prepare Text: delim 0: table | list-of-columns --------------------- */

/* One cell -> OWNED RAY_STR raw text (no quoting).  Borrows atom. */
static ray_t* q_ft_cell_text(ray_t* atom) {
    ray_t* s0 = q_string_fn(atom);              /* charv post-1b */
    if (!s0 || RAY_IS_ERR(s0)) return s0;
    ray_t* s = q_str_in(s0);                    /* legacy STR for the writers */
    ray_release(s0);
    if (!s || RAY_IS_ERR(s)) return s;
    if (atom->type == -RAY_DATE && s->type == -RAY_STR) {
        /* Prepare Text renders temporals ISO 8601 (doc: 2022-03-14) — the
         * date dots become dashes; other temporals already match. */
        size_t n = ray_str_len(s);
        char* b = (char*)malloc(n ? n : 1);
        if (b) {
            const char* p = ray_str_ptr(s);
            for (size_t i = 0; i < n; i++) b[i] = p[i] == '.' ? '-' : p[i];
            ray_t* d = ray_str(b, n);
            free(b);
            ray_release(s);
            return d;
        }
    }
    return s;
}

/* Quote rule (doc): a cell containing the delimiter or a line break is
 * embraced with '"' and every embedded '"' doubled; otherwise raw.  Appends
 * the (possibly embraced) cell to *buf/(w..cap).  Returns 0 on OOM. */
static int q_ft_quote_append(char** buf, size_t* w, size_t* cap,
                             const char* c, size_t n, char delim) {
    int embrace = 0;
    size_t extra = 2;
    for (size_t i = 0; i < n; i++) {
        if (c[i] == delim || c[i] == '\n') embrace = 1;
        if (c[i] == '"') extra++;
    }
    size_t need = *w + n + (embrace ? extra : 0) + 2;
    if (need > *cap) {
        size_t nc = *cap ? *cap : 64;
        while (nc < need) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb; *cap = nc;
    }
    char* b = *buf;
    if (!embrace) {
        memcpy(b + *w, c, n);
        *w += n;
        return 1;
    }
    b[(*w)++] = '"';
    for (size_t i = 0; i < n; i++) {
        if (c[i] == '"') b[(*w)++] = '"';
        b[(*w)++] = c[i];
    }
    b[(*w)++] = '"';
    return 1;
}

static ray_t* q_ft_prepare(char delim, ray_t* y) {
    /* columns + optional names */
    int64_t nc = 0;
    ray_t* namev = NULL;                 /* borrowed via table introspection */
    ray_t** litems = NULL;
    int is_table = y && y->type == RAY_TABLE;
    if (is_table) nc = ray_table_ncols(y);
    else if (y && y->type == RAY_LIST) { nc = ray_len(y); litems = (ray_t**)ray_data(y); }
    else return ray_error("type", "0:: Prepare Text needs a table or a list of columns");
    (void)namev;
    if (nc == 0) return ray_list_new(1);
    /* validate columns; find the shared row count */
    int64_t L = -1;
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = is_table ? ray_table_get_col_idx(y, c) : litems[c];  /* borrowed */
        int64_t l;
        if (col && col->type == -RAY_STR) l = (int64_t)ray_str_len(col);  /* char column */
        else if (col && (ray_is_vec(col) || col->type == RAY_LIST)) {
            l = ray_len(col);
            if (col->type == RAY_LIST) {                  /* must be all strings */
                ray_t** it = (ray_t**)ray_data(col);
                for (int64_t i = 0; i < l; i++)
                    if (!it[i] || (it[i]->type != -RAY_STR && it[i]->type != RAY_CHARV))
                        return ray_error("type", "0:: column is neither a vector nor a list of strings");
            }
        } else return ray_error("type", "0:: column is neither a vector nor a list of strings");
        if (L < 0) L = l;
        else if (l != L) return ray_error("length", "0:: column lengths differ");
    }
    ray_t* out = ray_list_new(L + 1 > 0 ? L + 1 : 1);
    if (RAY_IS_ERR(out)) return out;
    char* buf = NULL;
    size_t cap = 0;
    /* header row: table column names */
    if (is_table) {
        size_t w = 0;
        for (int64_t c = 0; c < nc; c++) {
            if (c) {
                if (w + 1 > cap) { cap = cap ? cap * 2 : 64; buf = (char*)realloc(buf, cap); if (!buf) { ray_release(out); return ray_error("oom", NULL); } }
                buf[w++] = delim;
            }
            int64_t nm = ray_table_col_name(y, c);
            ray_t* ns = ray_sym_str(nm);                   /* borrowed */
            if (!ns || !q_ft_quote_append(&buf, &w, &cap, ray_str_ptr(ns), ray_str_len(ns), delim)) {
                free(buf); ray_release(out);
                return ray_error("oom", NULL);
            }
        }
        ray_t* line = ray_str(buf ? buf : "", w);
        out = ray_list_append(out, line);
        ray_release(line);
        if (RAY_IS_ERR(out)) { free(buf); return out; }
    }
    for (int64_t i = 0; i < L; i++) {
        size_t w = 0;
        for (int64_t c = 0; c < nc; c++) {
            if (c) {
                if (w + 1 > cap) { cap = cap ? cap * 2 : 64; buf = (char*)realloc(buf, cap); if (!buf) { ray_release(out); return ray_error("oom", NULL); } }
                buf[w++] = delim;
            }
            ray_t* col = is_table ? ray_table_get_col_idx(y, c) : litems[c];  /* borrowed */
            int ok;
            if (col->type == -RAY_STR) {                   /* char column: one char */
                char ch = ray_str_ptr(col)[i];
                ok = q_ft_quote_append(&buf, &w, &cap, &ch, 1, delim);
            } else if (col->type == RAY_CHARV) {           /* char column (charv) */
                char ch = ((const char*)ray_data(col))[i];
                ok = q_ft_quote_append(&buf, &w, &cap, &ch, 1, delim);
            } else if (col->type == RAY_LIST) {            /* string column */
                ray_t** it = (ray_t**)ray_data(col);
                const char* cp; int64_t cn;
                if (!q_text_bytes(it[i], &cp, &cn)) { cp = ""; cn = 0; }
                ok = q_ft_quote_append(&buf, &w, &cap, cp, (size_t)cn, delim);
            } else {
                ray_t* ia = ray_i64(i);
                ray_t* atom = ray_at_fn(col, ia);
                ray_release(ia);
                if (!atom || RAY_IS_ERR(atom)) { free(buf); ray_release(out); return atom ? atom : ray_error("oom", NULL); }
                ray_t* cs = q_ft_cell_text(atom);
                ray_release(atom);
                if (!cs || RAY_IS_ERR(cs)) { free(buf); ray_release(out); return cs ? cs : ray_error("oom", NULL); }
                if (cs->type != -RAY_STR) { ray_release(cs); free(buf); ray_release(out); return ray_error("type", "0:: unformattable cell"); }
                ok = q_ft_quote_append(&buf, &w, &cap, ray_str_ptr(cs), ray_str_len(cs), delim);
                ray_release(cs);
            }
            if (!ok) { free(buf); ray_release(out); return ray_error("oom", NULL); }
        }
        ray_t* line = ray_str(buf ? buf : "", w);
        out = ray_list_append(out, line);
        ray_release(line);
        if (RAY_IS_ERR(out)) { free(buf); return out; }
    }
    free(buf);
    return out;
}

/* ---- Load CSV / Load Fixed shared plumbing ------------------------------ */

/* Type char -> Tok tag via THE cast home (q_cast_designator; upper case =
 * Tok).  '*' keeps the field a string, ' ' skips the column, unknown -> 0. */
static int8_t q_ft_tag(char c, int* is_str, int* is_skip) {
    *is_str = 0; *is_skip = 0;
    if (c == ' ') { *is_skip = 1; return 0; }
    if (c == '*') { *is_str = 1; return 0; }
    if (c < 'A' || c > 'Z') return 0;                      /* doc: upper case */
    ray_t* d = ray_str(&c, 1);
    if (!d || RAY_IS_ERR(d)) return 0;
    int is_tok = 0;
    int8_t tag = q_cast_designator(d, &is_tok);
    ray_release(d);
    return is_tok ? tag : 0;
}

/* Normalize the RIGHT operand of Load CSV / Load Fixed into an OWNED
 * RAY_LIST of row strings.  *single = 1 for the one-string-no-newline form
 * (kdb returns a list of parsed ATOMS for it, not columns). */
static ray_t* q_ft_rows(ray_t* y, int* single) {
    *single = 0;
    if (!y) return ray_error("type", "0:: nil right operand");
    if (y->type == -RAY_STR) {
        const char* p = ray_str_ptr(y);
        size_t n = ray_str_len(y);
        if (memchr(p, '\n', n)) return q_str_split_lines(p, n);
        *single = 1;
        ray_t* out = ray_list_new(1);
        if (RAY_IS_ERR(out)) return out;
        out = ray_list_append(out, y);                     /* retains y */
        return out;
    }
    if (y->type == -RAY_SYM) {
        ray_t* path = q_ft_path(y);
        if (!path) return ray_error("type", "0:: expected a file symbol `:path");
        ray_t* all = q_ft_read_all(path);
        ray_release(path);
        if (!all || RAY_IS_ERR(all)) return all;
        ray_t* rows = q_str_split_lines(ray_str_ptr(all), ray_str_len(all));
        ray_release(all);
        return rows;
    }
    if (y->type == RAY_LIST || y->type == RAY_STR) {
        int64_t n = ray_len(y);
        ray_t** e = y->type == RAY_LIST ? (ray_t**)ray_data(y) : NULL;
        /* (filesymbol; offset[; length]) chunk form */
        if (e && n >= 2 && n <= 3 && e[0] && e[0]->type == -RAY_SYM) {
            ray_t* path = q_ft_path(e[0]);
            if (!path) return ray_error("type", "0:: x");
            int64_t off, want = -1;
            if (!q_strict_i64(e[1], &off) || (n == 3 && !q_strict_i64(e[2], &want))) {
                ray_release(path);
                return ray_error("type", "0:: offset/length");
            }
            ray_t* all = q_ft_read_all(path);
            ray_release(path);
            if (!all || RAY_IS_ERR(all)) return all;
            const char* p = ray_str_ptr(all);
            int64_t len = (int64_t)ray_str_len(all);
            if (off < 0) off = 0;
            if (off > len) off = len;
            int64_t end = want >= 0 && off + want < len ? off + want : len;
            ray_t* rows = q_str_split_lines(p + off, (size_t)(end - off));
            ray_release(all);
            return rows;
        }
        /* list / str-vector of row strings */
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* it = ray_at_fn(y, ia);
            ray_release(ia);
            if (!it || RAY_IS_ERR(it)) { ray_release(out); return it ? it : ray_error("oom", NULL); }
            if (it->type != -RAY_STR) {
                ray_release(it); ray_release(out);
                return ray_error("type", "0:: expected a list of strings");
            }
            out = ray_list_append(out, it);
            ray_release(it);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return ray_error("type", "0:: unsupported right operand");
}

/* flag=1 (embedded line returns): merge physical rows whose quotes are
 * unbalanced with the following row, restoring the '\n'.  Owns+returns. */
static ray_t* q_ft_merge_quoted(ray_t* rows) {
    int64_t n = ray_len(rows);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) { ray_release(rows); return out; }
    ray_t** e = (ray_t**)ray_data(rows);
    char* acc = NULL;                        /* pending logical row */
    size_t used = 0;                         /* bytes of acc in use */
    size_t quotes = 0;                       /* running '"' parity  */
    for (int64_t i = 0; i < n; i++) {
        const char* p = ray_str_ptr(e[i]);
        size_t l = ray_str_len(e[i]);
        char* na = (char*)realloc(acc, used + l + 2);
        if (!na) { free(acc); ray_release(out); ray_release(rows); return ray_error("oom", NULL); }
        acc = na;
        if (used) acc[used++] = '\n';        /* rejoin the split line (codex P2:
                                              * the old spare-byte scheme wrote
                                              * the '\n' where the next row's
                                              * memcpy landed, corrupting it) */
        memcpy(acc + used, p, l);
        used += l;
        for (size_t k = 0; k < l; k++) if (p[k] == '"') quotes++;
        if ((quotes & 1) == 0) {
            ray_t* s = ray_str(acc, used);
            out = ray_list_append(out, s);
            ray_release(s);
            free(acc); acc = NULL; used = 0; quotes = 0;
            if (RAY_IS_ERR(out)) { ray_release(rows); return out; }
        }
    }
    if (acc) {                               /* unterminated tail row */
        ray_t* s = ray_str(acc, used);
        out = ray_list_append(out, s);
        ray_release(s);
        free(acc);
        if (RAY_IS_ERR(out)) { ray_release(rows); return out; }
    }
    ray_release(rows);
    return out;
}

/* Split one row into fields on a delimiter; '"'-opened fields are
 * quote-scanned ('""' -> literal '"').  Appends OWNED strings to `fields`. */
static ray_t* q_ft_fields(ray_t* fields, const char* r, size_t n, char delim) {
    size_t i = 0;
    for (;;) {
        char* fb = (char*)malloc(n + 1);
        if (!fb) { ray_release(fields); return ray_error("oom", NULL); }
        size_t fl = 0;
        if (i < n && r[i] == '"') {
            i++;
            while (i < n) {
                if (r[i] == '"') {
                    if (i + 1 < n && r[i + 1] == '"') { fb[fl++] = '"'; i += 2; }
                    else { i++; break; }
                } else fb[fl++] = r[i++];
            }
            while (i < n && r[i] != delim) i++;             /* junk after quote */
        } else {
            while (i < n && r[i] != delim) fb[fl++] = r[i++];
        }
        ray_t* f = ray_str(fb, fl);
        free(fb);
        fields = ray_list_append(fields, f);
        ray_release(f);
        if (RAY_IS_ERR(fields)) return fields;
        if (i >= n) break;
        i++;                                                /* past delim */
        if (i == n) {                                       /* trailing delim */
            ray_t* z = ray_str("", 0);
            fields = ray_list_append(fields, z);
            ray_release(z);
            break;
        }
    }
    return fields;
}

/* Parse one field per its column recipe.  OWNED atom/string. */
static ray_t* q_ft_parse_field(ray_t* field, int8_t tag, int is_str) {
    if (is_str) { ray_retain(field); return field; }
    return q_tok_to(tag, field);
}

/* Collapse a column accumulator: '*' columns stay lists of strings; typed
 * columns Tok-parse (q_tok_to distributes over lists) then collapse. */
static ray_t* q_ft_finish_col(ray_t* colacc, int8_t tag, int is_str) {
    if (is_str) { ray_retain(colacc); return colacc; }
    ray_t* parsed = q_tok_to(tag, colacc);
    if (!parsed || RAY_IS_ERR(parsed)) return parsed;
    ray_t* v = q_collapse_list(parsed);                     /* owned */
    ray_release(parsed);
    return v;
}

static ray_t* q_ft_load_csv(ray_t* types, ray_t* delimspec, ray_t* flag, ray_t* y) {
    const char* ts = ray_str_ptr(types);
    size_t nt = ray_str_len(types);
    if (nt == 0) return ray_error("type", "0:: empty types string");
    /* delimiter: char atom (len-1 string) or enlisted -> header row */
    char delim;
    int header = 0;
    if (delimspec && delimspec->type == -RAY_STR && ray_str_len(delimspec) == 1)
        delim = ray_str_ptr(delimspec)[0];
    else if (delimspec &&
             (delimspec->type == RAY_LIST || delimspec->type == RAY_STR) &&
             ray_len(delimspec) == 1) {
        /* enlisted delimiter -> first row is column names.  `enlist ","` is
         * an engine STR VECTOR (10h), a boxed 1-list also accepted. */
        ray_t* ia = ray_i64(0);
        ray_t* d0 = ray_at_fn(delimspec, ia);
        ray_release(ia);
        if (!d0 || RAY_IS_ERR(d0)) return d0 ? d0 : ray_error("oom", NULL);
        if (d0->type != -RAY_STR || ray_str_len(d0) != 1) {
            ray_release(d0);
            return ray_error("type", "0:: bad delimiter");
        }
        delim = ray_str_ptr(d0)[0];
        ray_release(d0);
        header = 1;
    } else return ray_error("type", "0:: bad delimiter");
    int embed_nl = 0;
    if (flag) {
        int64_t fv;
        ray_t* ferr = q_i64_or_err(flag, &fv, "0:: flag");
        if (ferr) return ferr;
        embed_nl = fv != 0;
    }
    /* column recipes */
    int8_t* tags = (int8_t*)malloc(nt);
    int* fstr = (int*)malloc(nt * sizeof(int));
    int* fskip = (int*)malloc(nt * sizeof(int));
    if (!tags || !fstr || !fskip) { free(tags); free(fstr); free(fskip); return ray_error("oom", NULL); }
    for (size_t j = 0; j < nt; j++) {
        tags[j] = q_ft_tag(ts[j], &fstr[j], &fskip[j]);
        if (!tags[j] && !fstr[j] && !fskip[j]) {
            char bad = ts[j];
            free(tags); free(fstr); free(fskip);
            if (bad == 'C')
                return ray_error("nyi", "0:: type char C needs the char type (string-model C3)");
            return ray_error("type", "0:: bad column type char '%c'", bad);
        }
    }
    int single = 0;
    ray_t* rows = q_ft_rows(y, &single);
    if (!rows || RAY_IS_ERR(rows)) { free(tags); free(fstr); free(fskip); return rows; }
    if (embed_nl) {
        rows = q_ft_merge_quoted(rows);
        if (!rows || RAY_IS_ERR(rows)) { free(tags); free(fstr); free(fskip); return rows; }
    }
    ray_t** rp = (ray_t**)ray_data(rows);
    int64_t nrows = ray_len(rows);
    ray_t* result = NULL;
    if (single) {
        /* one delimited string -> list of parsed atoms */
        ray_t* fields = ray_list_new((int64_t)nt);
        if (RAY_IS_ERR(fields)) { result = fields; goto done; }
        fields = q_ft_fields(fields, ray_str_ptr(rp[0]), ray_str_len(rp[0]), delim);
        if (RAY_IS_ERR(fields)) { result = fields; goto done; }
        ray_t** fp = (ray_t**)ray_data(fields);
        int64_t nf = ray_len(fields);
        ray_t* out = ray_list_new((int64_t)nt);
        if (RAY_IS_ERR(out)) { ray_release(fields); result = out; goto done; }
        ray_t* empty = ray_str("", 0);
        for (size_t j = 0; j < nt && !RAY_IS_ERR(out); j++) {
            if (fskip[j]) continue;
            ray_t* f = (int64_t)j < nf ? fp[j] : empty;     /* borrowed */
            ray_t* a = q_ft_parse_field(f, tags[j], fstr[j]);
            if (!a || RAY_IS_ERR(a)) { ray_release(empty); ray_release(fields); ray_release(out); result = a ? a : ray_error("oom", NULL); goto done; }
            out = ray_list_append(out, a);
            ray_release(a);
        }
        ray_release(empty);
        ray_release(fields);
        result = out;
        goto done;
    }
    {
        /* rows mode: per-column accumulators over the data rows */
        int64_t first = header ? 1 : 0;
        int64_t ndata = nrows - first;
        if (ndata < 0) ndata = 0;
        int64_t nout = 0;
        for (size_t j = 0; j < nt; j++) if (!fskip[j]) nout++;
        ray_t** acc = (ray_t**)calloc((size_t)(nout > 0 ? nout : 1), sizeof(ray_t*));
        if (!acc) { result = ray_error("oom", NULL); goto done; }
        int64_t k = 0;
        for (size_t j = 0; j < nt; j++) {
            if (fskip[j]) continue;
            acc[k] = ray_list_new(ndata > 0 ? ndata : 1);
            if (RAY_IS_ERR(acc[k])) {
                result = acc[k];
                for (int64_t z = 0; z < k; z++) ray_release(acc[z]);
                free(acc);
                goto done;
            }
            k++;
        }
        ray_t* empty = ray_str("", 0);
        for (int64_t i = first; i < nrows; i++) {
            ray_t* fields = ray_list_new((int64_t)nt);
            if (!RAY_IS_ERR(fields))
                fields = q_ft_fields(fields, ray_str_ptr(rp[i]), ray_str_len(rp[i]), delim);
            if (RAY_IS_ERR(fields)) {
                for (int64_t z = 0; z < nout; z++) ray_release(acc[z]);
                free(acc); ray_release(empty);
                result = fields;
                goto done;
            }
            ray_t** fp = (ray_t**)ray_data(fields);
            int64_t nf = ray_len(fields);
            int64_t c = 0;
            for (size_t j = 0; j < nt; j++) {
                if (fskip[j]) continue;
                ray_t* f = (int64_t)j < nf ? fp[j] : empty; /* borrowed */
                acc[c] = ray_list_append(acc[c], f);
                c++;
            }
            ray_release(fields);
        }
        ray_release(empty);
        /* finish columns */
        ray_t* cols = ray_list_new(nout > 0 ? nout : 1);
        for (int64_t z = 0; z < nout && !RAY_IS_ERR(cols); z++) {
            int64_t j = -1, seen = -1;
            for (size_t t = 0; t < nt; t++) {
                if (fskip[t]) continue;
                if (++seen == z) { j = (int64_t)t; break; }
            }
            ray_t* col = q_ft_finish_col(acc[z], tags[j], fstr[j]);
            if (!col || RAY_IS_ERR(col)) {
                ray_release(cols);
                cols = col ? col : ray_error("oom", NULL);
                break;
            }
            cols = ray_list_append(cols, col);
            ray_release(col);
        }
        for (int64_t z = 0; z < nout; z++) ray_release(acc[z]);
        free(acc);
        if (RAY_IS_ERR(cols)) { result = cols; goto done; }
        if (!header) { result = cols; goto done; }
        /* header: first row = column names -> table */
        ray_t* nmf = ray_list_new((int64_t)nt);
        if (!RAY_IS_ERR(nmf) && nrows > 0)
            nmf = q_ft_fields(nmf, ray_str_ptr(rp[0]), ray_str_len(rp[0]), delim);
        if (RAY_IS_ERR(nmf)) { ray_release(cols); result = nmf; goto done; }
        ray_t** np = (ray_t**)ray_data(nmf);
        int64_t nn = ray_len(nmf);
        ray_t* tbl = ray_table_new(nout > 0 ? nout : 1);
        int64_t c2 = 0;
        ray_t** cp = (ray_t**)ray_data(cols);
        for (size_t j = 0; j < nt && !RAY_IS_ERR(tbl); j++) {
            if (fskip[j]) continue;
            int64_t nm = (int64_t)j < nn
                ? ray_sym_intern_runtime(ray_str_ptr(np[j]), ray_str_len(np[j]))
                : ray_sym_intern_runtime("", 0);
            tbl = ray_table_add_col(tbl, nm, cp[c2]);
            c2++;
        }
        ray_release(nmf);
        ray_release(cols);
        result = tbl;
    }
done:
    ray_release(rows);
    free(tags); free(fstr); free(fskip);
    return result;
}

static ray_t* q_ft_load_fixed(ray_t* types, ray_t* widths, ray_t* y) {
    const char* ts = ray_str_ptr(types);
    size_t nt = ray_str_len(types);
    if (nt == 0 || (int64_t)nt != ray_len(widths))
        return ray_error("length", "0:: types and widths must have equal count");
    /* widths must be positive (codex P1: a negative width made the slice
     * length negative and reached memcpy as a huge size_t). */
    for (int64_t j = 0; j < (int64_t)nt; j++)
        if (q_ivec_get(widths, j) <= 0)
            return ray_error("domain", "0:: field widths must be positive");
    int8_t* tags = (int8_t*)malloc(nt);
    int* fstr = (int*)malloc(nt * sizeof(int));
    int* fskip = (int*)malloc(nt * sizeof(int));
    if (!tags || !fstr || !fskip) { free(tags); free(fstr); free(fskip); return ray_error("oom", NULL); }
    for (size_t j = 0; j < nt; j++) {
        tags[j] = q_ft_tag(ts[j], &fstr[j], &fskip[j]);
        if (!tags[j] && !fstr[j] && !fskip[j]) {
            free(tags); free(fstr); free(fskip);
            return ray_error("type", "0:: bad column type char");
        }
    }
    int single = 0;
    ray_t* rows = q_ft_rows(y, &single);
    if (!rows || RAY_IS_ERR(rows)) { free(tags); free(fstr); free(fskip); return rows; }
    ray_t** rp = (ray_t**)ray_data(rows);
    int64_t nrows = ray_len(rows);
    int64_t nout = 0;
    for (size_t j = 0; j < nt; j++) if (!fskip[j]) nout++;
    ray_t* result = NULL;
    ray_t** acc = (ray_t**)calloc((size_t)(nout > 0 ? nout : 1), sizeof(ray_t*));
    if (!acc) { result = ray_error("oom", NULL); goto done; }
    for (int64_t z = 0; z < nout; z++) {
        acc[z] = ray_list_new(nrows > 0 ? nrows : 1);
        if (RAY_IS_ERR(acc[z])) {
            result = acc[z];
            for (int64_t q = 0; q < z; q++) ray_release(acc[q]);
            free(acc);
            goto done;
        }
    }
    for (int64_t i = 0; i < nrows; i++) {
        const char* p = ray_str_ptr(rp[i]);
        int64_t n = (int64_t)ray_str_len(rp[i]);
        int64_t pos = 0, c = 0;
        for (size_t j = 0; j < nt; j++) {
            int64_t w = q_ivec_get(widths, (int64_t)j);
            int64_t s = pos > n ? n : pos;
            int64_t e = pos + w > n ? n : pos + w;
            pos += w;
            if (fskip[j]) continue;
            ray_t* f = ray_str(p + s, (size_t)(e - s));
            if (!f || RAY_IS_ERR(f)) {
                for (int64_t z = 0; z < nout; z++) ray_release(acc[z]);
                free(acc);
                result = f ? f : ray_error("oom", NULL);
                goto done;
            }
            acc[c] = ray_list_append(acc[c], f);
            ray_release(f);
            c++;
        }
    }
    {
        ray_t* cols = ray_list_new(nout > 0 ? nout : 1);
        int64_t z = 0;
        for (size_t j = 0; j < nt && !RAY_IS_ERR(cols); j++) {
            if (fskip[j]) continue;
            ray_t* col = q_ft_finish_col(acc[z], tags[j], fstr[j]);
            if (!col || RAY_IS_ERR(col)) {
                ray_release(cols);
                cols = col ? col : ray_error("oom", NULL);
                break;
            }
            cols = ray_list_append(cols, col);
            ray_release(col);
            z++;
        }
        for (int64_t q = 0; q < nout; q++) ray_release(acc[q]);
        free(acc);
        result = cols;
    }
done:
    ray_release(rows);
    free(tags); free(fstr); free(fskip);
    return result;
}

/* ---- Key-Value Pairs: "K f [*] r" 0: string ------------------------------ */
static ray_t* q_ft_kv(const char* spec, size_t sn, ray_t* y) {
    char ktype = spec[0];
    int star = sn == 4;
    if (star && spec[2] != '*') return ray_error("type", "0:: bad key-value spec");
    char fsep = spec[1];
    char rsep = star ? spec[3] : spec[2];
    if (!y || y->type != -RAY_STR)
        return ray_error("type", "0:: key-value parse needs a string");
    int8_t ktag;
    {
        int is_str = 0, is_skip = 0;
        ktag = q_ft_tag(ktype, &is_str, &is_skip);
        if (!ktag) return ray_error("type", "0:: bad key-value key type");
    }
    const char* p = ray_str_ptr(y);
    size_t n = ray_str_len(y);
    ray_t* keys = ray_list_new(4);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(4);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    size_t i = 0;
    while (i < n) {
        /* one record: up to rsep (quote-aware in '*' mode) */
        size_t start = i;
        int inq = 0;
        while (i < n && (inq || p[i] != rsep)) {
            if (star && p[i] == '"') inq = !inq;
            i++;
        }
        size_t end = i;
        if (i < n) i++;                                     /* past rsep */
        if (end == start) continue;                         /* empty record */
        /* split at the FIRST fsep */
        size_t f = start;
        while (f < end && p[f] != fsep) f++;
        ray_t* k = ray_str(p + start, f - start);
        size_t vs = f < end ? f + 1 : end;
        ray_t* v;
        if (star && vs < end && p[vs] == '"' && p[end - 1] == '"' && end - vs >= 2) {
            /* quoted value: strip the outer quotes, un-double inner ones */
            char* vb = (char*)malloc(end - vs);
            size_t vl = 0;
            if (!vb) { ray_release(k); ray_release(keys); ray_release(vals); return ray_error("oom", NULL); }
            for (size_t t = vs + 1; t < end - 1; t++) {
                if (p[t] == '"' && t + 1 < end - 1 && p[t + 1] == '"') { vb[vl++] = '"'; t++; }
                else vb[vl++] = p[t];
            }
            v = ray_str(vb, vl);
            free(vb);
        } else v = ray_str(p + vs, end - vs);
        if (!k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v)) {
            if (k && !RAY_IS_ERR(k)) ray_release(k);
            if (v && !RAY_IS_ERR(v)) ray_release(v);
            ray_release(keys); ray_release(vals);
            return ray_error("oom", NULL);
        }
        ray_t* ka = q_tok_to(ktag, k);
        ray_release(k);
        if (!ka || RAY_IS_ERR(ka)) { ray_release(v); ray_release(keys); ray_release(vals); return ka ? ka : ray_error("oom", NULL); }
        keys = ray_list_append(keys, ka);
        ray_release(ka);
        vals = ray_list_append(vals, v);
        ray_release(v);
        if (RAY_IS_ERR(keys) || RAY_IS_ERR(vals)) {
            ray_t* err = RAY_IS_ERR(keys) ? keys : vals;
            if (!RAY_IS_ERR(keys)) ray_release(keys);
            if (!RAY_IS_ERR(vals)) ray_release(vals);
            return err;
        }
    }
    ray_t* kv = q_collapse_list(keys);                      /* typed key vector */
    ray_release(keys);
    if (!kv || RAY_IS_ERR(kv)) { ray_release(vals); return kv ? kv : ray_error("oom", NULL); }
    ray_t* out = ray_list_new(2);
    if (RAY_IS_ERR(out)) { ray_release(kv); ray_release(vals); return out; }
    out = ray_list_append(out, kv);
    ray_release(kv);
    if (RAY_IS_ERR(out)) { ray_release(vals); return out; }
    out = ray_list_append(out, vals);
    ray_release(vals);
    return out;
}


/* ---- the `0:` dispatcher -------------------------------------------------- */
static ray_t* q_filetext_impl(ray_t* x, ray_t* y);
/* x-normalize preserving the bare-vs-ENLISTED delimiter distinction the
 * charv model carries natively: char ATOM -> 1-char STR (bare delim); charv
 * len-1 -> boxed 1-list of a 1-char STR (the legacy enlisted form, header
 * row); other charv -> STR (types/kv spec); LIST -> per-element.  Owned. */
static ray_t* q_ft_norm_x(ray_t* x) {
    if (x && x->type == -RAY_CHARV) { char c = (char)x->u8; return ray_str(&c, 1); }
    if (x && x->type == RAY_CHARV) {
        if (ray_len(x) == 1) {
            ray_t* s = q_str_of_charv(x);
            if (!s || RAY_IS_ERR(s)) return s;
            ray_t* l = ray_list_new(1);
            if (!l || RAY_IS_ERR(l)) { ray_release(s); return l; }
            l = ray_list_append(l, s);
            ray_release(s);
            return l;
        }
        return q_str_of_charv(x);
    }
    if (x && x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (!out || RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* c = q_ft_norm_x(e[i]);
            if (!c) { ray_retain(e[i]); c = e[i]; }
            if (RAY_IS_ERR(c)) { ray_release(out); return c; }
            out = ray_list_append(out, c);
            ray_release(c);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x) ray_retain(x);
    return x;
}
ray_t* q_filetext_wrap(ray_t* x, ray_t* y) {
    ray_t* xs = q_ft_norm_x(x); ray_t* ys = q_str_in(y);
    ray_t* r = q_filetext_impl(xs, ys);
    ray_release(xs); ray_release(ys);
    return q_charv_out(r);              /* parsed strings cross as charv */
}
static ray_t* q_filetext_impl(ray_t* x, ray_t* y) {
    if (!x) return ray_error("type", "0:: nil left operand");
    if (x->type == -RAY_SYM) return q_ft_save_text(x, y);
    if (x->type == -RAY_STR) {
        const char* s = ray_str_ptr(x);
        size_t n = ray_str_len(x);
        if (n == 1) return q_ft_prepare(s[0], y);
        if ((n == 3 || n == 4) && (s[0] == 'S' || s[0] == 'I' || s[0] == 'J'))
            return q_ft_kv(s, n, y);
        return ray_error("type", "0:: bad left operand");
    }
    if (x->type == RAY_LIST && (ray_len(x) == 2 || ray_len(x) == 3)) {
        ray_t** e = (ray_t**)ray_data(x);
        if (e[0] && e[0]->type == -RAY_STR) {
            if (ray_len(x) == 2 && q_is_int_vec(e[1]))
                return q_ft_load_fixed(e[0], e[1], y);
            return q_ft_load_csv(e[0], e[1], ray_len(x) == 3 ? e[2] : NULL, y);
        }
    }
    return ray_error("type", "0:: unsupported left operand");
}

/* ===== string search family: like / ss / ssr (feat/q-string-fns) =========== */

/* q `x like p` — glob match.  Reuses ray_like_fn for sym/str atoms and
 * vectors; a DICT maps the match over its values (kdb: keys kept). */
ray_t* q_like_wrap(ray_t* x, ray_t* pattern) {
    if (x && x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);   /* borrowed */
        ray_t* v = ray_dict_vals(x);   /* borrowed */
        if (!k || !v) return ray_error("type", "like: bad dict");
        ray_t* nv = q_like_wrap(v, pattern);
        if (!nv || RAY_IS_ERR(nv)) return nv;
        ray_retain(k);
        return ray_dict_new(k, nv);    /* consumes k + nv */
    }
    if (x && x->type == RAY_LIST) {    /* boxed list (e.g. dict values) */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* e = ray_at_fn(x, ia);
            ray_release(ia);
            if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
            ray_t* r = q_like_wrap(e, pattern);
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        ray_t* c = q_collapse_list(out);   /* homogeneous bool run -> bool vec */
        ray_release(out);
        return c;
    }
    { /* charv leaf/pattern -> legacy -RAY_STR forms for the engine matcher */
        ray_t* xs = q_str_in(x); ray_t* ps = q_str_in(pattern);
        ray_t* r = ray_like_fn(xs, ps);
        ray_release(xs); ray_release(ps);
        return r;
    }
}

/* Match glob pattern p[0..pn) anchored at s[pos..sn), where every pattern
 * token consumes exactly one input char: `?` (any), `[..]`/`[^..]` (class,
 * optional negation), or a literal.  The variable-width `*` form is not used
 * by q's ss/ssr, so a bare `*` is treated literally.  Returns the number of
 * input chars consumed on a full match, or -1 on mismatch. */
static int64_t q_glob_fixed_at(const char* s, size_t sn, size_t pos,
                               const char* p, size_t pn) {
    size_t si = pos, pi = 0;
    while (pi < pn) {
        if (si >= sn) return -1;
        char pc = p[pi];
        if (pc == '?') { pi++; si++; continue; }
        if (pc == '[') {
            size_t j = pi + 1;
            int neg = 0;
            if (j < pn && p[j] == '^') { neg = 1; j++; }
            int matched = 0;
            for (; j < pn && p[j] != ']'; j++)
                if (p[j] == s[si]) matched = 1;
            if (j < pn) j++;              /* skip ']' */
            if (matched == neg) return -1;
            pi = j; si++; continue;
        }
        if (pc != s[si]) return -1;       /* literal */
        pi++; si++;
    }
    return (int64_t)(si - pos);
}

/* q `s ss p` — string search: 0-based start index of every match of the glob
 * pattern p in the string s (overlapping, kdb-true).  Returns a long vector. */
ray_t* q_ss_wrap(ray_t* s, ray_t* p) {
    const char* sp; int64_t sn64; const char* pp; int64_t pn64;
    if (!q_text_bytes(s, &sp, &sn64) || !q_text_bytes(p, &pp, &pn64))
        return ray_error("type", "ss: expects string arguments");
    size_t sn = (size_t)sn64, pn = (size_t)pn64;
    ray_t* out = ray_vec_new(RAY_I64, 8);
    if (RAY_IS_ERR(out)) return out;
    if (pn == 0) return out;                       /* empty pattern -> no hits */
    /* q_glob_fixed_at anchors at each position; the match WIDTH differs from
     * the pattern's byte length (a `[..]` class is 1 input char), so iterate
     * every start position and let the matcher enforce bounds. */
    for (size_t i = 0; i < sn; i++) {
        if (q_glob_fixed_at(sp, sn, i, pp, pn) >= 0) {
            int64_t idx = (int64_t)i;
            out = ray_vec_append(out, &idx);
            if (RAY_IS_ERR(out)) return out;
        }
    }
    return out;
}

/* q `ssr[s;p;r]` — replace every (non-overlapping, left-to-right) match of the
 * glob pattern p in s.  r is either a replacement string, or a function
 * applied to each matched substring (kdb: `ssr[s;"t?r";upper]`). */
ray_t* q_ssr_wrap(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("rank", "ssr: expects 3 args");
    ray_t* s = args[0]; ray_t* p = args[1]; ray_t* r = args[2];
    const char* sp; int64_t sn64; const char* pp; int64_t pn64;
    if (!q_text_bytes(s, &sp, &sn64) || !q_text_bytes(p, &pp, &pn64))
        return ray_error("type", "ssr: s and p must be strings");
    int r_is_fn = q_is_fn_value(r);
    { const char* rp_; int64_t rn_;
      if (!r_is_fn && !q_text_bytes(r, &rp_, &rn_))
          return ray_error("type", "ssr: replacement must be a string or function"); }
    size_t sn = (size_t)sn64, pn = (size_t)pn64;
    size_t cap = sn + 16, blen = 0;
    char* b = (char*)malloc(cap);
    if (!b) return ray_error("wsfull", "ssr: out of memory");
    #define SSR_PUSH(PTR, L) do { \
        size_t _l = (L); \
        if (blen + _l > cap) { cap = (blen + _l) * 2; char* nb = (char*)realloc(b, cap); \
            if (!nb) { free(b); return ray_error("wsfull", "ssr: out of memory"); } b = nb; } \
        memcpy(b + blen, (PTR), _l); blen += _l; } while (0)
    ray_t* err = NULL;
    size_t i = 0;
    while (i < sn) {
        /* q_glob_fixed_at bounds-checks internally (si>=sn -> -1) and a `[..]`
         * class is multiple pattern bytes but ONE input char, so do NOT gate on
         * `i + pn <= sn` (that rejected bracket-class matches near the end). */
        int64_t m = pn ? q_glob_fixed_at(sp, sn, i, pp, pn) : -1;
        if (m >= 0) {
            if (r_is_fn) {
                ray_t* sub = ray_charv(sp + i, m);      /* matched text, in flight */
                ray_t* one[1] = { sub };
                ray_t* rep = q_call_n(r, one, 1);
                ray_release(sub);
                if (!rep || RAY_IS_ERR(rep)) { err = rep; break; }
                { const char* qp; int64_t qn;
                  if (!q_text_bytes(rep, &qp, &qn)) { ray_release(rep); err = ray_error("type", "ssr: replacement fn must return a string"); break; }
                  SSR_PUSH(qp, (size_t)qn); }
                ray_release(rep);
            } else {
                const char* rp2; int64_t rn2;
                (void)q_text_bytes(r, &rp2, &rn2);
                SSR_PUSH(rp2, (size_t)rn2);
            }
            i += (m > 0) ? (size_t)m : 1;   /* advance past the match */
        } else {
            SSR_PUSH(sp + i, 1);
            i++;
        }
    }
    #undef SSR_PUSH
    if (err) { free(b); return err; }
    ray_t* out = ray_charv(b, (int64_t)blen);
    free(b);
    return out;
}

/* q `getenv x` (ref/getenv.md) — x is a SYMBOL atom naming an environment
 * variable; returns its value as a string, or "" when the variable is unset
 * (kdb-true, and exactly what the base ray_getenv_fn already returns for a
 * missing var).  The base primitive wants a -RAY_STR arg, so coerce the
 * symbol's name to a string atom first — the ONLY divergence from the raw C,
 * hence a wrapper rather than a QK_ENV rename.
 * String-model seam: the result is a native -RAY_STR atom, so `type getenv`X`
 * is -10h where kdb's char vector is 10h (a known, tracked divergence). */
ray_t* q_getenv_wrap(ray_t* x) {
    /* .os.getenv is RAY_FN_RESTRICTED; calling the C fn directly bypasses the
     * eval-layer check, so re-assert it here (the q_hopen_wrap/file precedent). */
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    if (!x || x->type != -RAY_SYM)
        return ray_error("type", "getenv: expected a symbol, got %s",
                         ray_type_name(x ? x->type : 0));
    ray_t* s = ray_sym_str(x->i64);                     /* borrowed */
    if (!s) return ray_error("type", "getenv: bad symbol");
    ray_t* name = ray_str(ray_str_ptr(s), ray_str_len(s));  /* owned -RAY_STR */
    if (!name || RAY_IS_ERR(name)) return name ? name : ray_error("oom", NULL);
    ray_t* r = ray_getenv_fn(name);                     /* "" when unset */
    ray_release(name);
    return q_charv_out(r);
}

/* q `x setenv y` (ref/getenv.md#setenv) — x is a SYMBOL atom (the variable
 * name), y is a string.  Sets the environment variable and returns generic
 * null (kdb: setenv's result displays as nothing in the console).  The base
 * ray_setenv_fn takes two -RAY_STR args and echoes y retained; coerce the sym
 * name to a string, discard that echo, and return :: to match kdb. */
static ray_t* q_setenv_impl(ray_t* x, ray_t* y);
ray_t* q_setenv_wrap(ray_t* x, ray_t* y) {
    ray_t* ys = q_str_in(y);
    ray_t* r = q_setenv_impl(x, ys);
    ray_release(ys);
    return r;
}
static ray_t* q_setenv_impl(ray_t* x, ray_t* y) {
    /* .os.setenv is RAY_FN_RESTRICTED; re-assert here (calling the C fn directly
     * bypasses the eval-layer check — the q_hopen_wrap/file-wrapper precedent). */
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    if (!x || x->type != -RAY_SYM)
        return ray_error("type", "setenv: name must be a symbol, got %s",
                         ray_type_name(x ? x->type : 0));
    if (!y || y->type != -RAY_STR)
        return ray_error("type", "setenv: value must be a string, got %s",
                         ray_type_name(y ? y->type : 0));
    ray_t* s = ray_sym_str(x->i64);                     /* borrowed */
    if (!s) return ray_error("type", "setenv: bad symbol");
    ray_t* name = ray_str(ray_str_ptr(s), ray_str_len(s));  /* owned -RAY_STR */
    if (!name || RAY_IS_ERR(name)) return name ? name : ray_error("oom", NULL);
    ray_t* r = ray_setenv_fn(name, y);                  /* echoes y, or error */
    ray_release(name);
    if (r && RAY_IS_ERR(r)) return r;
    if (r) ray_release(r);                              /* discard echoed value */
    return RAY_NULL_OBJ;                                /* kdb: setenv -> :: */
}
