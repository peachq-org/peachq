/* q_conn — see q_conn.h.  Sockets come ONLY from the IPC walk (the liveness
 * authority — a stale q_handles socket record can never resurrect a dead
 * connection); q_handles enriches outbound sockets (user, redacted addr) and
 * contributes the non-socket rows; q_provider adds provider/alias detail.
 * Rows sort by fd.  n/m (unsent msgs/bytes) are 0 for sockets — TRUTHFUL:
 * there is no async output queue yet (Stage 2) — and the long null on rows
 * where the concept does not apply. */
#include "qlang/io/q_conn.h"
#include "qlang/io/q_handles.h"
#include "qlang/io/q_provider.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"
#include "qlang/q_env.h"
#include "core/ipc.h"
#include "lang/env.h"       /* ray_fn_unary — the .pq.i.conns native */
#include "lang/eval.h"      /* RAY_FN_NONE */
#include "table/sym.h"      /* ray_sym_intern / _runtime, RAY_SYM_W64 */
#include <rayforce.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t       fd;
    q_handle_kind kind;
    char          p, f;       /* ' ' = the char null on non-socket rows */
    uint8_t       out;
    int64_t       user_sym;
    ray_t*        addr;       /* BORROWED charv; NULL = none */
    int64_t       provider_sym, alias_sym;
    int64_t       open_ns;    /* NULL_I64 = unknown */
} conn_row;

static int row_cmp(const void* a, const void* b) {
    int64_t x = ((const conn_row*)a)->fd, y = ((const conn_row*)b)->fd;
    return x < y ? -1 : x > y;
}

/* THE walk.  Owned malloc'd row array (free it), *n_out its length; NULL on
 * OOM.  addr fields stay borrowed from the registries — consume before any
 * call that could close a handle. */
static conn_row* conn_rows(int64_t* n_out) {
    *n_out = 0;
    int64_t esym = ray_sym_intern_runtime("", 0);
    int64_t ni = ray_ipc_conn_list(NULL, 0);
    int64_t nh = q_handles_count();
    conn_row* rows = (conn_row*)malloc((size_t)(ni + nh + 1) * sizeof *rows);
    ray_ipc_conn_info_t* infos =
        (ray_ipc_conn_info_t*)malloc((size_t)(ni + 1) * sizeof *infos);
    if (!rows || !infos) { free(rows); free(infos); return NULL; }
    int64_t got = ray_ipc_conn_list(infos, ni);
    if (got < ni) ni = got;
    int64_t n = 0;
    for (int64_t i = 0; i < ni; i++) {
        conn_row* r = &rows[n++];
        r->fd   = infos[i].fd;
        r->kind = Q_HANDLE_SOCKET;
        r->p    = infos[i].ws ? 'w' : 'q';
        r->f    = 't';                    /* TCP is the only transport family */
        r->out  = !infos[i].inbound;
        r->user_sym = esym;
        r->addr = NULL;
        r->provider_sym = esym; r->alias_sym = esym;
        r->open_ns = infos[i].open_ns ? infos[i].open_ns : NULL_I64;
        if (r->out) {                     /* hopen registered the descriptor */
            int64_t us = q_handles_user_sym(r->fd);
            if (us >= 0) r->user_sym = us;
            r->addr = q_handles_open_args(r->fd);
        }
    }
    free(infos);
    for (int64_t i = 0; i < nh; i++) {
        q_handle_info_t hi;
        if (!q_handles_get(i, &hi) || hi.kind == Q_HANDLE_SOCKET) continue;
        conn_row* r = &rows[n++];
        r->fd   = hi.fd;
        r->kind = hi.kind;
        r->p    = ' ';
        r->f    = ' ';
        r->out  = hi.initiated_out != 0;
        r->user_sym = hi.user_sym >= 0 ? hi.user_sym : esym;
        r->addr = hi.open_args;
        r->provider_sym = esym; r->alias_sym = esym;
        r->open_ns = hi.open_time_ns;
        if (hi.kind == Q_HANDLE_PROVIDER)
            (void)q_provider_info(hi.fd, &r->provider_sym, &r->alias_sym);
    }
    qsort(rows, (size_t)n, sizeof *rows, row_cmp);
    *n_out = n;
    return rows;
}

static int64_t kind_sym(q_handle_kind k) {
    switch (k) {
        case Q_HANDLE_SOCKET:   return ray_sym_intern_runtime("socket", 6);
        case Q_HANDLE_PROVIDER: return ray_sym_intern_runtime("provider", 8);
        case Q_HANDLE_FIFO:     return ray_sym_intern_runtime("fifo", 4);
        default:                return ray_sym_intern_runtime("file", 4);
    }
}

static void conn_drop(ray_t* x) {
    if (!x) return;
    if (RAY_IS_ERR(x)) ray_error_free(x);
    else ray_release(x);
}

/* First error among the columns (owned; all siblings freed), NULL if none. */
static ray_t* cols_bad(ray_t** c, int64_t nc) {
    int64_t bad = -1;
    for (int64_t i = 0; i < nc && bad < 0; i++)
        if (!c[i] || RAY_IS_ERR(c[i])) bad = i;
    if (bad < 0) return NULL;
    ray_t* e = c[bad];
    for (int64_t i = 0; i < nc; i++)
        if (i != bad) conn_drop(c[i]);
    return e ? e : q_err(QE_WSFULL);
}

static ray_t* cols_table(ray_t** c, const char* const* names, int64_t nc) {
    ray_t* bad = cols_bad(c, nc);
    if (bad) return bad;
    ray_t* t = ray_table_new(nc);
    for (int64_t i = 0; i < nc; i++) {
        t = ray_table_add_col(t, ray_sym_intern(names[i], strlen(names[i])), c[i]);
        ray_release(c[i]);
    }
    return t;
}

ray_t* q_conn_table(void) {
    int64_t n;
    conn_row* rows = conn_rows(&n);
    if (!rows) return q_err(QE_WSFULL);
    int64_t cap = n ? n : 1;
    static const char* const names[13] = { "h", "kind", "p", "f", "z", "n",
        "m", "out", "user", "addr", "provider", "alias", "opened" };
    ray_t* c[13];
    c[0]  = ray_vec_new(RAY_I64, cap);
    c[1]  = ray_sym_vec_new(RAY_SYM_W64, cap);
    c[2]  = ray_vec_new(RAY_CHARV, cap);
    c[3]  = ray_vec_new(RAY_CHARV, cap);
    c[4]  = ray_vec_new(RAY_BOOL, cap);
    c[5]  = ray_vec_new(RAY_I64, cap);
    c[6]  = ray_vec_new(RAY_I64, cap);
    c[7]  = ray_vec_new(RAY_BOOL, cap);
    c[8]  = ray_sym_vec_new(RAY_SYM_W64, cap);
    c[9]  = ray_list_new(cap);
    c[10] = ray_sym_vec_new(RAY_SYM_W64, cap);
    c[11] = ray_sym_vec_new(RAY_SYM_W64, cap);
    c[12] = ray_vec_new(RAY_TIMESTAMP, cap);
    for (int64_t i = 0; i < n; i++) {
        conn_row* r = &rows[i];
        int64_t ks   = kind_sym(r->kind);
        int64_t nm   = r->kind == Q_HANDLE_SOCKET ? 0 : NULL_I64;
        uint8_t zf   = 0;
        c[0]  = ray_vec_append(c[0], &r->fd);
        c[1]  = ray_vec_append(c[1], &ks);
        c[2]  = ray_vec_append(c[2], &r->p);
        c[3]  = ray_vec_append(c[3], &r->f);
        c[4]  = ray_vec_append(c[4], &zf);
        c[5]  = ray_vec_append(c[5], &nm);
        c[6]  = ray_vec_append(c[6], &nm);
        c[7]  = ray_vec_append(c[7], &r->out);
        c[8]  = ray_vec_append(c[8], &r->user_sym);
        if (c[9] && !RAY_IS_ERR(c[9])) {
            ray_t* a = r->addr ? r->addr : ray_charv("", 0);
            c[9] = ray_list_append(c[9], a);
            if (!r->addr) ray_release(a);
        }
        c[10] = ray_vec_append(c[10], &r->provider_sym);
        c[11] = ray_vec_append(c[11], &r->alias_sym);
        c[12] = ray_vec_append(c[12], &r->open_ns);
    }
    free(rows);
    return cols_table(c, names, 13);
}

ray_t* q_conn_zH(void) {
    int64_t n;
    conn_row* rows = conn_rows(&n);
    if (!rows) return q_err(QE_WSFULL);
    ray_t* v = ray_vec_new(RAY_I32, n ? n : 1);
    for (int64_t i = 0; i < n && v && !RAY_IS_ERR(v); i++) {
        if (rows[i].kind != Q_HANDLE_SOCKET) continue;
        int32_t fd = (int32_t)rows[i].fd;
        v = ray_vec_append(v, &fd);
    }
    free(rows);
    return v ? v : q_err(QE_WSFULL);
}

/* keys ARE q_conn_zH's answer, so `.z.H ~ key .z.W` holds by construction */
ray_t* q_conn_zW(void) {
    ray_t* k = q_conn_zH();
    if (!k || RAY_IS_ERR(k)) return k;
    int64_t n = ray_len(k), zero = 0;
    ray_t* v = ray_vec_new(RAY_I64, n ? n : 1);
    for (int64_t i = 0; i < n && v && !RAY_IS_ERR(v); i++)
        v = ray_vec_append(v, &zero);
    if (!v || RAY_IS_ERR(v)) { ray_release(k); return v ? v : q_err(QE_WSFULL); }
    return ray_dict_new(k, v);          /* consumes both */
}

static const conn_row* row_find(const conn_row* rows, int64_t n, int64_t fd) {
    for (int64_t i = 0; i < n; i++) if (rows[i].fd == fd) return &rows[i];
    return NULL;
}

/* 'domain: no such live handle (the -26! precedent); 'type: live non-socket */
static ray_t* b38_row(const conn_row* rows, int64_t n, int64_t fd,
                      const conn_row** out) {
    const conn_row* r = row_find(rows, n, fd);
    if (!r) return q_err(QE_DOMAIN);
    if (r->kind != Q_HANDLE_SOCKET) return q_err(QE_TYPE);
    *out = r;
    return NULL;
}

static const char* const B38_COLS[5] = { "p", "f", "z", "n", "m" };

static ray_t* b38_dict(const conn_row* r) {
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, 5);
    for (int i = 0; i < 5 && k && !RAY_IS_ERR(k); i++) {
        int64_t s = ray_sym_intern_runtime(B38_COLS[i], 1);
        k = ray_vec_append(k, &s);
    }
    ray_t* v = ray_list_new(5);
    ray_t* e[5] = { ray_char((uint8_t)r->p), ray_char((uint8_t)r->f),
                    ray_bool(false), ray_i64(0), ray_i64(0) };
    for (int i = 0; i < 5; i++) {
        if (!e[i] || RAY_IS_ERR(e[i])) {   /* a failed atom becomes v's error */
            conn_drop(v);
            v = e[i];
            continue;
        }
        if (v && !RAY_IS_ERR(v)) v = ray_list_append(v, e[i]);
        ray_release(e[i]);
    }
    ray_t* kv[2] = { k, v };
    ray_t* bad = cols_bad(kv, 2);
    if (bad) return bad;
    return ray_dict_new(k, v);          /* consumes both */
}

static ray_t* b38_table(const conn_row* rows, int64_t n, ray_t* y) {
    int64_t k = ray_len(y);
    int64_t cap = k ? k : 1;
    ray_t* c[5];
    c[0] = ray_vec_new(RAY_CHARV, cap);
    c[1] = ray_vec_new(RAY_CHARV, cap);
    c[2] = ray_vec_new(RAY_BOOL, cap);
    c[3] = ray_vec_new(RAY_I64, cap);
    c[4] = ray_vec_new(RAY_I64, cap);
    for (int64_t i = 0; i < k; i++) {
        int64_t fd;
        if (y->type == RAY_LIST) {
            ray_t* el = ((ray_t**)ray_data(y))[i];
            if (!q_type_is_int_atom(el)) {
                ray_t* bad = cols_bad(c, 5);   /* frees the columns either way */
                if (bad) conn_drop(bad);
                else for (int j = 0; j < 5; j++) ray_release(c[j]);
                return q_err(QE_TYPE);
            }
            fd = q_type_iatom_val(el);
        } else {
            fd = q_type_ivec_get(y, i);
        }
        const conn_row* r;
        ray_t* e = b38_row(rows, n, fd, &r);
        if (e) {                               /* the semantic error wins */
            ray_t* bad = cols_bad(c, 5);
            if (bad) conn_drop(bad);
            else for (int j = 0; j < 5; j++) ray_release(c[j]);
            return e;
        }
        uint8_t zf = 0;
        int64_t zero = 0;
        c[0] = ray_vec_append(c[0], &r->p);
        c[1] = ray_vec_append(c[1], &r->f);
        c[2] = ray_vec_append(c[2], &zf);
        c[3] = ray_vec_append(c[3], &zero);
        c[4] = ray_vec_append(c[4], &zero);
    }
    return cols_table(c, B38_COLS, 5);
}

ray_t* q_conn_bang38(ray_t* y) {
    if (!y) return q_err(QE_TYPE);
    int atom = q_type_is_int_atom(y);
    if (!atom && !q_type_is_int_vec(y) && y->type != RAY_LIST)
        return q_err(QE_TYPE);
    int64_t n;
    conn_row* rows = conn_rows(&n);
    if (!rows) return q_err(QE_WSFULL);
    ray_t* out;
    if (atom) {
        const conn_row* r = NULL;
        ray_t* e = b38_row(rows, n, q_type_iatom_val(y), &r);
        out = e ? e : b38_dict(r);
    } else {
        out = b38_table(rows, n, y);
    }
    free(rows);
    return out;
}

static ray_t* pq_conns_fn(ray_t* x) { (void)x; return q_conn_table(); }

void q_conn_pq_register(void) {
    static const char nm[] = ".pq.i.conns";
    ray_t* obj = ray_fn_unary(nm, RAY_FN_NONE, pq_conns_fn);
    q_env_bind(ray_sym_intern(nm, strlen(nm)), obj);
    ray_release(obj);
}
