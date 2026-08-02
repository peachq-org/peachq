/* ops/q_io.c — the q path/byte layer: `:path` resolution, whole-file and
 * slice reads, and the verbs that sit directly on them (hsym, read0, read1,
 * hdel).  Oracle: ref/read0.md, ref/read1.md, ref/hsym.md, ref/hdel.md
 * (CLEAN ROOM).  The `0:` File Text surface builds on this file from
 * ops/q_io_filetext.c; hopen/hclose live with the handle registry
 * (q_handles.c) and getenv/setenv with the process commands (q_sys.c).
 *
 * RAY_FN_RESTRICTED note: the base file primitives carry the flag on their ENV
 * fn objects; calling the C functions directly bypasses the eval-layer check,
 * so every file-touching arm re-asserts ray_eval_get_restricted(). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* q_str_split_lines, q_type_strict_i64 */
#include "qlang/base/q_err.h"
#include "qlang/io/q_handles.h" /* q_handles_read1 — the fifo-handle read1 form */
#include "lang/eval.h"      /* ray_eval_get_restricted, ray_read_file_fn */
#include "table/sym.h"      /* ray_sym_intern_runtime, ray_sym_vec_cell */
#include <stdio.h>          /* fopen/fseek/fread — the slice reader; remove — hdel */
#include <stdlib.h>
#include <string.h>

/* file symbol -> OWNED RAY_STR filesystem path (leading ':' stripped), or
 * NULL when x is not a `:path symbol. */
ray_t* q_io_file_path(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return NULL;
    ray_t* s = ray_sym_str(x->i64);                       /* borrowed */
    if (!s) return NULL;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    if (n < 2 || p[0] != ':') return NULL;
    return ray_str(p + 1, n - 1);
}

/* Read a whole file (restricted-guarded).  OWNED RAY_STR or error. */
ray_t* q_io_read_all(ray_t* pathstr) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    return ray_read_file_fn(pathstr);
}

/* q `hsym x` — sym atom/vector -> file symbol: prefix ':' unless already
 * present (ref/hsym.md). */
static int64_t hsym_id(const char* p, size_t n) {
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
static ray_t* hsym_wrap_impl(ray_t* x);
ray_t* q_hsym_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = hsym_wrap_impl(xs);
    ray_release(xs);
    return r;
}
static ray_t* hsym_wrap_impl(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);                   /* borrowed */
        if (!s) return q_err(QE_TYPE);
        const char* p = ray_str_ptr(s);
        size_t n = ray_str_len(s);
        if (n > 0 && p[0] == ':') { ray_retain(x); return x; }
        return ray_sym(hsym_id(p, n));
    }
    if (x && x->type == RAY_SYM) {
        int64_t n = ray_len(x);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        for (int64_t i = 0; i < n; i++) {
            ray_t* c = ray_sym_vec_cell(x, i);            /* borrowed domain atom */
            if (!c) { ray_release(out); return q_err(QE_TYPE); }
            int64_t id = hsym_id(ray_str_ptr(c), ray_str_len(c));
            out = ray_vec_append(out, &id);
            if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        }
        return out;
    }
    return q_err(QE_TYPE);
}

/* The `(file;offset[;length])` left argument of read0 and read1 (and of `1:`
 * when it lands — ref/file-binary.md gives it the same triple).  On success
 * returns NULL with *path OWNED and *want -1 for the length-less form; else
 * releases the path and returns the owned 'type/'domain.  `0:`'s chunk form
 * reads the same triple but CLAMPS a negative offset where these signal
 * 'domain, so it is deliberately not routed through here. */
static ray_t* io_file_triple(ray_t* fsym, ray_t* offv, ray_t* wantv,
                             ray_t** path, int64_t* off, int64_t* want) {
    *path = q_io_file_path(fsym);
    if (!*path) return q_err(QE_TYPE);
    *want = -1;
    q_err_e e = QE_TYPE;
    if (q_type_strict_i64(offv, off) && (!wantv || q_type_strict_i64(wantv, want))) {
        if (*off >= 0 && (!wantv || *want >= 0)) return NULL;
        e = QE_DOMAIN;
    }
    ray_release(*path);
    *path = NULL;
    return q_err(e);
}

/* q `read0 x` — ref/read0.md.  File sym -> list of line strings (LF/CRLF
 * delimiters removed); (f;o) -> chars from offset o to EOF minus one trailing
 * line break (the doc pins `read0(`:foo;6)` -> "world" on a file ending \n);
 * (f;o;n) -> exactly n chars from o (clamped).  Console (0) and fifo handles
 * are deferred 'nyi.  Offsets accept 0 (superset of the doc wording). */
static ray_t* read0_wrap_impl(ray_t* x);
ray_t* q_read0_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = read0_wrap_impl(xs);
    ray_release(xs);
    return q_str_charv_out(r);              /* lines cross as char vectors */
}
static ray_t* read0_wrap_impl(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* path = q_io_file_path(x);
        if (!path) return q_err(QE_TYPE);
        ray_t* all = q_io_read_all(path);
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
            ray_t* path;
            int64_t off, want;
            ray_t* bad = io_file_triple(e[0], e[1], three ? e[2] : NULL, &path, &off, &want);
            if (bad) return bad;
            ray_t* all = q_io_read_all(path);
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
        return q_err(QE_NYI);
    }
    if (x && q_type_is_int_atom(x))
        return q_err(QE_NYI);
    return q_err(QE_TYPE);
}

/* fseek/fread streams just the slice, so .Q.fsn's lump loop costs O(z) per
 * call, not O(file) — and a header sniff costs its few bytes. */
ray_t* q_io_read_slice(ray_t* pathstr, int64_t off, int64_t want) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    const char* p = ray_str_ptr(pathstr);
    if (!p) return q_err(QE_DOMAIN);
    FILE* fp = fopen(p, "rb");
    if (!fp) return q_err(QE_IO);
    long sz;
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 0) {
        fclose(fp);
        return q_err(QE_IO);
    }
    if (off > sz) off = sz;
    int64_t take = (want < 0 || want > sz - off) ? sz - off : want;
    size_t got = 0;
    uint8_t* buf = NULL;
    if (take > 0) {
        buf = (uint8_t*)malloc((size_t)take);
        if (!buf) { fclose(fp); return q_err(QE_OOM); }
        if (fseek(fp, (long)off, SEEK_SET) != 0) {
            free(buf); fclose(fp);
            return q_err(QE_IO);
        }
        got = fread(buf, 1, (size_t)take, fp);
    }
    fclose(fp);
    ray_t* out = ray_vec_from_raw(RAY_BYTE_ONLY, buf, (int64_t)got);
    free(buf);
    return out;
}
/* q `read1 x` — ref/read1.md.  File sym -> whole content as bytes; (f;o) ->
 * bytes from offset o to EOF; (f;o;n) -> up to n bytes from o (short read at
 * EOF, offsets clamped).  `read1(h;n)` on a fifo handle is q_handles_read1's;
 * any other handle is 'nyi. */
static ray_t* read1_wrap_impl(ray_t* x);
ray_t* q_read1_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = read1_wrap_impl(xs);
    ray_release(xs);
    return r;
}
static ray_t* read1_wrap_impl(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* path = q_io_file_path(x);
        if (!path) return q_err(QE_TYPE);
        ray_t* r = q_io_read_slice(path, 0, -1);
        ray_release(path);
        return r;
    }
    /* read1(handle;count): an all-int (h;count) pair collapses to a homogeneous
     * int VECTOR (not a RAY_LIST) — the fifo-handle streaming form `.Q.fpn` uses.
     * q_handles_read1 answers NULL unless fd is a registered fifo (a Phase-1
     * file handle is a write/append fd — read a file via `read1 `:path`). */
    if (x && q_type_is_int_vec(x) && ray_len(x) == 2) {
        ray_t* c = ray_i64(q_type_ivec_get(x, 1));
        ray_t* r = q_handles_read1(q_type_ivec_get(x, 0), c);
        ray_release(c);
        if (r) return r;
    }
    if (x && x->type == RAY_LIST && (ray_len(x) == 2 || ray_len(x) == 3)) {
        ray_t** e = (ray_t**)ray_data(x);
        int three = ray_len(x) == 3;
        if (e[0] && e[0]->type == -RAY_SYM) {
            ray_t* path;
            int64_t off, want;
            ray_t* bad = io_file_triple(e[0], e[1], three ? e[2] : NULL, &path, &off, &want);
            if (bad) return bad;
            ray_t* r = q_io_read_slice(path, off, want);
            ray_release(path);
            return r;
        }
        if (!three && e[0] && q_type_is_int_atom(e[0])) {   /* read1(handle;count), fifo only */
            int64_t fd;
            if (!q_type_strict_i64(e[0], &fd)) return q_err(QE_TYPE);
            ray_t* r = q_handles_read1(fd, e[1]);
            if (r) return r;
        }
        return q_err(QE_NYI);
    }
    if (x && q_type_is_int_atom(x))
        return q_err(QE_NYI);
    return q_err(QE_TYPE);
}

/* q `hdel x` — ref/hdel.md.  Delete the file or (empty) folder named by the
 * file symbol `:path and return x.  POSIX remove() dispatches unlink/rmdir, so
 * a folder is removed only when empty (the doc's "folders only if empty").  A
 * missing path or non-empty folder surfaces 'io (the read0 ENOENT precedent).
 * WRITES the filesystem, so restricted mode refuses (the file-verb precedent). */
ray_t* q_hdel_wrap(ray_t* x) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    ray_t* path = q_io_file_path(x);            /* NULL unless a `:path symbol atom */
    if (!path) return q_err(QE_TYPE);
    int rc = remove(ray_str_ptr(path));  /* ray_str path is NUL-terminated */
    ray_release(path);
    if (rc != 0) return q_err(QE_IO);
    ray_retain(x);
    return x;
}
