/* q_log — `-11!` streaming execute (see q_log.h).  A log is a plain shape-A
 * general list — ff 01 00 attr count:int32, then one -8! body per chunk
 * (kb/file-compression.md:39: never a compressed one).  The cursor holds ONE
 * pread window of the file plus at most the chunk straddling its end.  The
 * header count is advisory (EOF terminates, as the (-2;x) length answer
 * needs).  The decoder answers one 'domain for "short" and "invalid" alike,
 * so a failure before EOF refills and one at EOF is 'badtail: a chunk that is
 * garbage mid-file costs the window growing to LOG_MAX before it is named. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/io/q_log.h"
#include "qlang/io/q_io.h"        /* q_io_file_path, q_io_pread */
#include "qlang/io/q_handles.h"   /* q_handles_console_eval — the handle-0 door */
#include "qlang/net/q_wire.h"     /* q_wire_read_obj — one chunk, bytes consumed */
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"    /* q_type_is_int_atom / q_type_iatom_val — the (n;x) form */
#include "qlang/ops/q_index.h"    /* q_index_elem_at — the (n;x) pair's items */
#include "lang/eval.h"            /* ray_eval_get_restricted */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif

#define LOG_HDR    8
#define LOG_WINDOW ((size_t)1 << 20)
#define LOG_MAX    ((size_t)1 << 32)        /* a -8! count is int32: 2^31 one-byte items + headers */

typedef struct {
    int      fd;
    int64_t  size, pos;                     /* file length; offset of buf[0] */
    uint8_t* buf;
    size_t   cap, len;
} log_cur_t;

static void log_close(log_cur_t* c) {
    if (c->fd >= 0) close(c->fd);
    free(c->buf);
}

static ray_t* log_open(log_cur_t* c, ray_t* path) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    const char* p = ray_str_ptr(path);
    struct stat st;
    if (stat(p, &st) != 0) return q_err(QE_IO);
    if (!S_ISREG(st.st_mode)) return q_err(QE_NYI);        /* fifo replay: out of scope */
    c->fd = open(p, O_RDONLY | O_BINARY);
    if (c->fd < 0) return q_err(QE_IO);
    c->size = (int64_t)st.st_size;
    c->cap = LOG_WINDOW;
    c->buf = (uint8_t*)malloc(c->cap);
    return c->buf ? NULL : q_err(QE_OOM);
}

enum { LOG_F_IO = -1, LOG_F_TOOBIG = -2, LOG_F_OOM = -3 };

/* Bytes added after buf[len] — one window — 0 at EOF, else a LOG_F_ code. */
static int64_t log_fill(log_cur_t* c) {
    if (c->pos + (int64_t)c->len >= c->size) return 0;
    if (c->len == c->cap) {
        if (c->cap * 2 > LOG_MAX) return LOG_F_TOOBIG;
        uint8_t* nb = (uint8_t*)realloc(c->buf, c->cap * 2);
        if (!nb) return LOG_F_OOM;
        c->buf = nb;
        c->cap *= 2;
    }
    ssize_t r;
    do r = q_io_pread(c->fd, c->buf + c->len, c->cap - c->len, c->pos + (int64_t)c->len);
    while (r < 0 && errno == EINTR);
    if (r < 0) return LOG_F_IO;
    if (r == 0) { c->size = c->pos + (int64_t)c->len; return 0; }   /* the file shrank under us */
    c->len += (size_t)r;
    return (int64_t)r;
}

static ray_t* log_fill_err(int64_t r) {
    return q_err(r == LOG_F_IO ? QE_IO : r == LOG_F_OOM ? QE_OOM : QE_BADTAIL);
}

static ray_t* log_pair(int64_t chunks, int64_t valid) {
    ray_t* v = ray_vec_new(RAY_I64, 2);
    if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_OOM);
    ((int64_t*)ray_data(v))[0] = chunks;
    ((int64_t*)ray_data(v))[1] = valid;
    v->len = 2;
    return v;
}

/* limit < 0 = every chunk; eval 0 is the (-2;x) count, which never evaluates. */
static ray_t* log_scan(log_cur_t* c, int64_t limit, int eval) {
    int64_t r = 0;
    while (c->len < LOG_HDR && (r = log_fill(c)) > 0) {}
    if (c->len < LOG_HDR) return r < 0 ? log_fill_err(r) : q_err(QE_CORRUPT);
    if (c->buf[0] != 0xff || c->buf[1] != 0x01 || c->buf[2] != 0) return q_err(QE_CORRUPT);
    size_t off = LOG_HDR;
    int64_t n = 0, valid = LOG_HDR;
    while (limit < 0 || n < limit) {
        size_t used = 0;
        ray_t* v = off < c->len ? q_wire_read_obj(c->buf + off, c->len - off, &used, 0) : NULL;
        if (v && !RAY_IS_ERR(v) && used) {
            if (eval) {
                ray_t* res = q_handles_console_eval(v);
                ray_release(v);
                if (!res || RAY_IS_ERR(res)) return res ? res : q_err(QE_OOM);
                ray_release(res);
            } else
                ray_release(v);
            off += used;
            n++;
            valid = c->pos + (int64_t)off;
            continue;
        }
        if (v) ray_release(v);
        memmove(c->buf, c->buf + off, c->len - off);      /* the straddling tail to the front */
        c->pos += (int64_t)off;
        c->len -= off;
        off = 0;
        r = log_fill(c);
        if (r < 0 && r != LOG_F_TOOBIG) return log_fill_err(r);
        if (r > 0) continue;
        if (c->len == 0) break;                            /* a clean end: the last chunk closed the file */
        return eval ? q_err(QE_BADTAIL) : log_pair(n, valid);
    }
    return ray_i64(n);
}

ray_t* q_log_replay(ray_t* y) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    int64_t n = -1;
    ray_t* f = y;
    if (y && y->type == RAY_LIST && ray_len(y) == 2) {
        ray_t* a = q_index_elem_at(y, 0);
        if (!a || RAY_IS_ERR(a)) return a ? a : q_err(QE_OOM);
        int ok = q_type_is_int_atom(a);
        if (ok) n = q_type_iatom_val(a);
        ray_release(a);
        if (!ok) return q_err(QE_TYPE);
        if (n < -2) return q_err(QE_DOMAIN);
        f = q_index_elem_at(y, 1);
        if (!f || RAY_IS_ERR(f)) return f ? f : q_err(QE_OOM);
    } else
        ray_retain(f);
    ray_t* path = f ? q_io_file_path(f) : NULL;
    ray_release(f);
    if (!path) return q_err(QE_TYPE);
    if (RAY_IS_ERR(path)) return path;
    log_cur_t c;
    ray_t* res = log_open(&c, path);
    if (!res) res = log_scan(&c, n >= 0 ? n : -1, n != -2);
    log_close(&c);
    ray_release(path);
    return res;
}
