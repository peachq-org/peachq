/* q_provider — see q_provider.h.  The handle is the alias SYM; its legacy int twin is a RESERVED real fd (dup of
 * /dev/null), collision-free in the one q_handles space.  Hooks are resolved at CALL time from the live q env; a
 * hook's error propagates unmodified. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_count.h"
#include "qlang/io/q_provider.h"
#include "qlang/io/q_handles.h"
#include "qlang/io/q_worker.h"     /* q_worker_fork / _spawn — the qfork and qspawn opens */
#include "qlang/io/q_io.h"         /* q_io_file_path, q_io_abs_spelling — a worker's stdout/stderr files */
#include "qlang/ops/q_index.h"     /* q_index_amend — storing those files absolute */
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"     /* q_type_is_int_atom; q_type_coord_mark — THE carrier mark */
#include "qlang/q_env.h"
#include "qlang/q_prim.h"          /* q_str_text_bytes, q_meta_fn, q_hopen_wrap */
#include "qlang/q_builtins.h"      /* q_count_fn — the count fallback */
#include "qlang/eval/q_eval.h"     /* q_eval_apply_value / q_eval_apply_call_sym — THE apply seam and its by-name front */
#include "lang/eval.h"             /* ray_eval_get_restricted */
#include "lang/env.h"              /* ray_fn_vary — the .pq.i.load native */
#include "table/sym.h"             /* ray_sym_intern_runtime, ray_sym_str */
#include "core/ipc.h"              /* ray_ipc_conn_stamp — is the peer's socket still THAT connection */
#include "qlang/q_dotz.h"          /* q_dotz_now_ns — the clock connection stamps are on */
#include <rayforce.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>          /* PATH_MAX */
#include <unistd.h>          /* close — releasing the reserved fd */

#define PROV_NAME_MAX 256

typedef struct {
    int64_t  fd;         /* the reserved legacy int (q_handles key) */
    q_pq_kind kind;
    int64_t  alias;      /* sym id */
    int64_t  handle;     /* sym id of `:pq:<kind>:alias` — what hopen answers */
    ray_t*   connid;     /* owned: the TOKEN; NULL after a failed re-dial */
    ray_t*   arg;        /* owned: the hopen arg, replayed by the re-dial; NULL = never re-dials */
    int64_t  stamp;      /* a q peer's IPC connection by its open stamp; 0 = DuckDB's own token */
    int      internal;   /* the kind's own row (DuckDB's main): neither hopen nor hclose may touch it */
} prov_ent;

static prov_ent* g_ents = NULL;
static int64_t   g_cap  = 0;
static int64_t   g_n    = 0;

static int64_t token_fd(ray_t* c) {
    return q_type_is_int_atom(c) ? q_type_iatom_val(c) : -1;
}

/* a q hook on the socket under an alias names the ALIAS int (owner ruling 2026-09-26): the socket is plumbing */
static int64_t hook_fd(int64_t fd, int64_t stamp) {
    for (int64_t i = 0; stamp && i < g_n; i++)
        if (g_ents[i].stamp == stamp && token_fd(g_ents[i].connid) == fd) return g_ents[i].fd;
    return fd;
}

void q_provider_init(void) {
    g_ents = NULL; g_cap = 0; g_n = 0;
    ray_ipc_set_hook_fd_fn(hook_fd);
}

static void ent_free(prov_ent* e) {
    if (e->connid) ray_release(e->connid);
    if (e->arg)    ray_release(e->arg);
}

void q_provider_destroy(void) {
    ray_ipc_set_hook_fd_fn(NULL);
    for (int64_t i = 0; i < g_n; i++) {
        close((int)g_ents[i].fd);      /* teardown: no hooks, just the fd + refs */
        ent_free(&g_ents[i]);
    }
    free(g_ents);
    g_ents = NULL; g_cap = 0; g_n = 0;
}

void q_provider_forget(void) {
    for (int64_t i = 0; i < g_n; i++) ent_free(&g_ents[i]);
    g_n = 0;
}

static prov_ent* find_fd(int64_t fd) {
    for (int64_t i = 0; i < g_n; i++) if (g_ents[i].fd == fd) return &g_ents[i];
    return NULL;
}

static prov_ent* find_alias(q_pq_kind kind, int64_t alias) {
    for (int64_t i = 0; i < g_n; i++)
        if (g_ents[i].kind == kind && g_ents[i].alias == alias) return &g_ents[i];
    return NULL;
}

static int sym_text(ray_t* x, const char** p, size_t* n) {
    if (!x || x->type != -RAY_SYM) return 0;
    ray_t* s = ray_sym_str(x->i64);            /* borrowed */
    if (!s) return 0;
    *p = ray_str_ptr(s); *n = ray_str_len(s);
    return 1;
}

static const char* hook_ns(q_pq_kind kind);

/* THE spelling test on a bare sym id: the flip law reaches it from the wire and from a dict's value slot.  2 only for
 * a well-formed table coordinate of a kind that has tables — the one shape a pointer may carry. */
static int coord_sym_form(int64_t sym) {
    ray_t* s = ray_sym_str(sym);               /* borrowed */
    q_pq_parts p;
    q_pq_kind kind = s ? q_handles_pq(ray_str_ptr(s), ray_str_len(s), &p) : Q_PQ_NONE;
    if (!kind) return 0;
    return p.table && p.ok && hook_ns(kind) ? 2 : 1;
}

int q_provider_coord_sym_form(ray_t* x) {
    return x && x->type == -RAY_SYM ? coord_sym_form(x->i64) : 0;
}

/* where a kind's table hooks live: DuckDB's own namespace, the q peers' internal one; qfork/qspawn have no tables */
static const char* hook_ns(q_pq_kind kind) {
    return kind == Q_PQ_DUCKDB ? "duckdb" : kind == Q_PQ_Q ? "pq.i.q" : NULL;
}

/* ".<ns>.<hook>" interned; -1 when the kind has no hooks */
static int64_t hook_sym(q_pq_kind kind, const char* hook) {
    const char* ns = hook_ns(kind);
    if (!ns) return -1;
    char buf[PROV_NAME_MAX];
    int m = snprintf(buf, sizeof buf, ".%s.%s", ns, hook);
    return ray_sym_intern_runtime(buf, (size_t)m);
}

/* the hook's current value — owned fn or NULL when undefined */
static ray_t* hook_fn(q_pq_kind kind, const char* hook) {
    int64_t s = hook_sym(kind, hook);
    if (s < 0) return NULL;
    ray_t* v = q_env_resolve(s);
    if (v && RAY_IS_ERR(v)) { ray_error_free(v); return NULL; }
    return v;
}

/* a REQUIRED hook: undefined is the ordinary name error for `.ns.hook` */
static ray_t* hook_call(q_pq_kind kind, const char* hook, ray_t** args, int64_t n) {
    int64_t s = hook_sym(kind, hook);
    if (s < 0) return q_err(QE_DOMAIN);
    return q_eval_apply_call_sym(s, args, n);
}

static int key_is(ray_t* ks, const char* name) {
    return ks && ray_str_len(ks) == strlen(name) && memcmp(ray_str_ptr(ks), name, strlen(name)) == 0;
}

/* A dict arg's keys, each one the kind takes: out[j] (owned) is the value of names[j].  'domain for any other key —
 * a key that does not apply is never ignored. */
static ray_t* arg_opts(ray_t* d, const char* const* names, int nn, ray_t** out) {
    ray_t* keys = ray_dict_keys(d);            /* borrowed */
    ray_t* vals = ray_dict_vals(d);
    int64_t m = q_count(d);
    if (m && keys->type != RAY_SYM) return q_err(QE_TYPE);
    for (int64_t i = 0; i < m; i++) {
        ray_t* ks = ray_sym_vec_cell(keys, i);
        int j = 0;
        while (j < nn && !key_is(ks, names[j])) j++;
        if (j == nn) return q_err(QE_DOMAIN);
        ray_t* iv = ray_i64(i);
        if (out[j]) ray_release(out[j]);
        out[j] = ray_at_fn(vals, iv);
        ray_release(iv);
        if (!out[j] || RAY_IS_ERR(out[j])) { ray_t* e = out[j]; out[j] = NULL; return e ? e : q_err(QE_OOM); }
    }
    return NULL;
}

static int is_pair(ray_t* x) {
    return x && (x->type == RAY_LIST || q_type_is_int_vec(x)) && q_count(x) == 2;
}

/* `:pq:q:`: the arg is anything kx hopen takes, handed to the ONE hopen unchanged; the dict names it `conn` beside the
 * shared `timeout`, which may not also ride in a kx (conn;timeout) pair.  Another alias is never a peer's address, and
 * is refused BEFORE hopen, which would open it, even at the head of a pair. */
static ray_t* open_q(ray_t* arg) {
    static const char* const names[] = { "conn", "timeout" };
    ray_t* o[2] = { NULL, NULL };
    ray_t* err = NULL;
    ray_t* conn = arg;
    if (arg->type == RAY_DICT) {
        err = arg_opts(arg, names, 2, o);
        conn = o[0];
        if (!err && (!conn || (o[1] && is_pair(conn)))) err = q_err(QE_DOMAIN);
    }
    const char* cs; size_t cn;
    if (!err && q_handles_pq_of(conn, &cs, &cn)) err = q_err(QE_DOMAIN);
    ray_t* r = err;
    if (!err && o[1]) {
        ray_t* pr = ray_list_new(2);
        pr = ray_list_append(pr, conn);
        if (!RAY_IS_ERR(pr)) pr = ray_list_append(pr, o[1]);
        r = RAY_IS_ERR(pr) ? pr : q_hopen_wrap(pr);
        if (!RAY_IS_ERR(pr)) ray_release(pr);
    } else if (!err) r = q_hopen_wrap(conn);
    for (int i = 0; i < 2; i++) if (o[i]) ray_release(o[i]);
    return r;
}

/* A worker's `stdout`/`stderr` value is a file symbol: true with its path NUL-terminated in buf[cap], else *why */
static bool log_path(ray_t* v, char* buf, size_t cap, q_err_e* why) {
    ray_t* p = v->type == -RAY_SYM ? q_io_file_path(v) : NULL;
    if (!p) { *why = QE_TYPE; return false; }
    size_t n = ray_str_len(p);
    bool ok = n < cap;
    if (ok) { memcpy(buf, ray_str_ptr(p), n); buf[n] = '\0'; }
    else *why = QE_LIMIT;
    ray_release(p);
    return ok;
}

/* `:pq:qfork:`: `::` or a timeout in ms; `:pq:qspawn:`: the argv (`""`/`::` = none).  Each dict takes `timeout`,
 * `stdout` and `stderr`, and qspawn's names its argv `argv`. */
static ray_t* open_worker(q_pq_kind kind, ray_t* arg) {
    static const char* const names[] = { "timeout", "stdout", "stderr", "argv" };
    ray_t* o[4] = { NULL, NULL, NULL, NULL };
    ray_t* err = NULL;
    ray_t* tmo  = NULL;
    ray_t* argv = kind == Q_PQ_QSPAWN ? arg : NULL;
    char buf[2][PATH_MAX];
    const char* log[2] = { NULL, NULL };
    if (arg->type == RAY_DICT) {
        err = arg_opts(arg, names, kind == Q_PQ_QSPAWN ? 4 : 3, o);
        tmo = o[0]; argv = o[3];
        q_err_e why;
        for (int i = 0; i < 2 && !err; i++)
            if (o[i + 1]) {
                if (log_path(o[i + 1], buf[i], sizeof buf[i], &why)) log[i] = buf[i];
                else err = q_err(why);
            }
    } else if (kind == Q_PQ_QFORK && !RAY_IS_NULL(arg)) {
        if (!q_type_is_int_atom(arg)) err = q_err(QE_TYPE);
        tmo = arg;
    }
    ray_t* none = NULL;
    if (!err && kind == Q_PQ_QSPAWN && (!argv || RAY_IS_NULL(argv))) argv = none = ray_list_new(0);
    ray_t* r = err ? err : kind == Q_PQ_QFORK ? q_worker_fork(tmo, log) : q_worker_spawn(argv, tmo, log);
    if (none) ray_release(none);
    for (int i = 0; i < 4; i++) if (o[i]) ray_release(o[i]);
    return r;
}

/* A worker dict's `stdout`/`stderr` file symbols made absolute NOW, so a relaunch after this process `\cd`s writes the
 * same files; any other value is left for open_worker to refuse.  Consumes d, answers the arg to store. */
static ray_t* logs_absolute(ray_t* d) {
    static const char* const names[] = { "stdout", "stderr" };
    for (int i = 0; i < 2 && d->type == RAY_DICT; i++) {
        ray_t* k = ray_sym(ray_sym_intern_runtime(names[i], strlen(names[i])));
        ray_t* v = ray_dict_get(d, k);
        ray_t* e = v && RAY_IS_ERR(v) ? v : NULL;
        char p[PATH_MAX], abs[PATH_MAX + 1];
        q_err_e why;
        if (v && !e && log_path(v, p, sizeof p, &why)) {
            if (!q_io_abs_spelling(p, strlen(p), abs + 1, sizeof abs - 1)) e = q_err(QE_LIMIT);
            else {
                abs[0] = ':';
                ray_t* a = ray_sym(ray_sym_intern_runtime(abs, strlen(abs)));
                ray_t* r = q_index_amend(d, &k, 1, NULL, a);
                ray_release(a);
                if (RAY_IS_ERR(r)) e = r;
                else d = r;
            }
        }
        if (v && !RAY_IS_ERR(v)) ray_release(v);
        ray_release(k);
        if (e) { ray_release(d); return e; }
    }
    return d;
}

/* The one open every hopen and re-dial runs.  A q kind's token must be a q IPC connection this call made — its open
 * stamp, which is also what says the alias is a peer (a file hopen_q reached is closed again, 'domain); DuckDB's is
 * whatever `.duckdb.i.open[alias; arg]` answered. */
static ray_t* conn_open(q_pq_kind kind, int64_t alias, ray_t* arg, ray_t** connid_out, int64_t* stamp_out) {
    *connid_out = NULL;
    *stamp_out = 0;
    if (kind == Q_PQ_DUCKDB) {
        ray_t* a = ray_sym(alias);
        ray_t* args[2] = { a, arg };
        ray_t* c = hook_call(kind, "i.open", args, 2);
        ray_release(a);
        if (!c || RAY_IS_ERR(c)) return c ? c : q_err(QE_TYPE);
        *connid_out = c;
        return NULL;
    }
    int64_t t0 = q_dotz_now_ns(0);
    ray_t* c = kind == Q_PQ_Q ? open_q(arg) : open_worker(kind, arg);
    if (!c || RAY_IS_ERR(c)) return c ? c : q_err(QE_TYPE);
    int64_t fd = token_fd(c);
    int64_t st = fd >= 0 ? ray_ipc_conn_stamp(fd, false) : 0;
    if (st < t0 || !st) {
        ray_t* cr = fd >= 0 ? q_handles_close(fd) : NULL;
        if (cr) ray_release(cr);
        ray_release(c);
        return q_err(QE_DOMAIN);
    }
    *connid_out = c;
    *stamp_out = st;
    return NULL;
}

static void conn_close(q_pq_kind kind, ray_t* connid) {
    if (kind != Q_PQ_DUCKDB) {
        ray_t* r = q_handles_close(token_fd(connid));
        if (r) ray_release(r);
        return;
    }
    ray_t* f = hook_fn(kind, "i.close");
    if (!f) return;
    ray_t* r = q_eval_apply_value(f, &connid, 1);
    ray_release(f);
    if (r) ray_release(r);
}

/* a peer is dead once its fd no longer carries the connection it was opened as; with drain, a pending EOF is read
 * first (and `.z.pc` runs inside — the caller re-finds its entry) */
static int ent_dead(const prov_ent* e, int drain) {
    if (!e->connid) return 1;
    int64_t want = e->stamp;
    return want && ray_ipc_conn_stamp(token_fd(e->connid), drain != 0) != want;
}

/* close only a token that still names its connection — a dead peer's fd may already be someone else's */
static void ent_drop_token(prov_ent* e) {
    if (!e->connid) return;
    if (!ent_dead(e, 0)) conn_close(e->kind, e->connid);
    ray_release(e->connid);
    e->connid = NULL;
}

/* THE reconnect (owner ruling 2026-09-18): an alias whose peer died re-dials ONCE per use, through the same open and
 * with the arg hopen was given; a failed dial errors THIS use and leaves the alias as it was — nothing is queued,
 * remembered or retried.  Both the drain (`.z.pc`) and the dial run q that may close this alias or open others,
 * moving g_ents: *ep is re-found after each. */
static ray_t* ent_live(prov_ent** ep) {
    prov_ent* e = *ep;
    if (!e->arg) return NULL;
    q_pq_kind kind = e->kind;
    int64_t aid = e->alias;
    int dead = ent_dead(e, 1);
    if (!(e = *ep = find_alias(kind, aid))) return q_err(QE_CONN);
    if (!dead) return NULL;
    ent_drop_token(e);
    ray_t* c = NULL;
    int64_t stamp = 0;
    ray_t* arg = e->arg;
    ray_retain(arg);
    ray_t* err = conn_open(kind, aid, arg, &c, &stamp);
    ray_release(arg);
    if (!(e = *ep = find_alias(kind, aid))) {
        if (c) { conn_close(kind, c); ray_release(c); }
        return err ? err : q_err(QE_CONN);
    }
    if (err) return err;
    e->connid = c;
    e->stamp  = stamp;
    return NULL;
}

static prov_ent* ent_push(void) {
    if (g_n == g_cap) {
        int64_t nc = g_cap ? g_cap * 2 : 8;
        prov_ent* ne = (prov_ent*)realloc(g_ents, (size_t)nc * sizeof *ne);
        if (!ne) return NULL;
        g_ents = ne; g_cap = nc;
    }
    memset(&g_ents[g_n], 0, sizeof g_ents[g_n]);
    return &g_ents[g_n++];
}

/* re-point a live alias in place: the old token goes, the new one comes from the new arg; carriers never notice */
static ray_t* repoint(prov_ent* live, ray_t* a) {
    q_pq_kind kind = live->kind;
    int64_t aid = live->alias;
    ent_drop_token(live);
    ray_t* c = NULL;
    int64_t stamp = 0;
    ray_t* e = conn_open(kind, aid, a, &c, &stamp);
    if (!(live = find_alias(kind, aid))) {     /* the open ran q that closed the alias */
        ray_release(a);
        if (c) { conn_close(kind, c); ray_release(c); }
        return e ? e : q_err(QE_CONN);
    }
    if (e) {                                   /* open failed: the alias dies */
        ray_release(a);
        int64_t fd = live->fd;
        close((int)fd);
        q_handles_deregister(fd);
        if (live->arg) ray_release(live->arg);
        *live = g_ents[--g_n];
        return e;
    }
    ent_drop_token(live);                      /* q under the open may have re-pointed it first */
    if (live->arg) ray_release(live->arg);
    live->arg    = a;
    live->connid = c;
    live->stamp  = stamp;
    return ray_sym(live->handle);
}

ray_t* q_provider_hopen(const char* s, size_t n, ray_t* arg) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    q_pq_parts p;
    q_pq_kind kind = q_handles_pq(s, n, &p);
    if (!kind || !p.ok || p.table) return q_err(QE_DOMAIN);   /* a table is never a connection */
    int64_t aid = ray_sym_intern_runtime(p.alias, p.alias_n);
    ray_t* a = arg ? arg : RAY_NULL_OBJ;
    ray_retain(a);
    if (kind == Q_PQ_QFORK || kind == Q_PQ_QSPAWN) a = logs_absolute(a);
    if (RAY_IS_ERR(a)) return a;
    prov_ent* live = find_alias(kind, aid);
    if (live && live->internal) { ray_release(a); return q_err(QE_DOMAIN); }
    if (live) return repoint(live, a);
    int fd = q_handles_reserve_fd();
    if (fd < 0) { ray_release(a); return q_err(QE_IO); }
    if (!q_handles_register(fd, Q_HANDLE_PROVIDER, 1, s, n)) {
        close(fd);
        ray_release(a);
        return q_err(QE_OOM);
    }
    ray_t* c = NULL;
    int64_t stamp = 0;
    ray_t* e = conn_open(kind, aid, a, &c, &stamp);
    prov_ent* ne = e ? NULL : ent_push();
    if (e || !ne) {
        if (c) { conn_close(kind, c); ray_release(c); }
        ray_release(a);
        q_handles_deregister(fd);
        close(fd);
        return e ? e : q_err(QE_OOM);
    }
    *ne = (prov_ent){ .fd = fd, .kind = kind, .alias = aid, .handle = ray_sym_intern_runtime(s, n),
                      .connid = c, .arg = a, .stamp = stamp };
    return ray_sym(ne->handle);
}

int64_t q_provider_register_internal(q_pq_kind kind, const char* alias, ray_t* token) {
    char buf[PROV_NAME_MAX];
    int m = snprintf(buf, sizeof buf, ":pq:%s:%s", q_handles_pq_kind_name(kind), alias);
    if (m <= 0 || m >= (int)sizeof buf || !token) return 0;
    int fd = q_handles_reserve_fd();
    if (fd < 0) return 0;
    if (!q_handles_register(fd, Q_HANDLE_PROVIDER, 1, buf, (size_t)m)) { close(fd); return 0; }
    prov_ent* ne = ent_push();
    if (!ne) { q_handles_deregister(fd); close(fd); return 0; }
    ray_retain(token);
    *ne = (prov_ent){ .fd = fd, .kind = kind, .alias = ray_sym_intern_runtime(alias, strlen(alias)),
                      .handle = ray_sym_intern_runtime(buf, (size_t)m), .connid = token, .internal = 1 };
    return ne->handle;
}

int q_provider_info(int64_t fd, int64_t* provider, int64_t* alias, int64_t* handle, int* open, int64_t* link) {
    prov_ent* e = find_fd(fd);
    if (!e) return 0;
    const char* kn = q_handles_pq_kind_name(e->kind);
    *provider = ray_sym_intern_runtime(kn, strlen(kn));
    *alias    = e->alias;
    *handle   = e->handle;
    *open     = !ent_dead(e, 0);
    *link     = *open && e->stamp ? token_fd(e->connid) : -1;
    return 1;
}

/* the registered row a connection sym names: NULL + *out NULL = a well-formed alias that is not open; 'type for a
 * sym outside `:pq:`, 'domain for a table coordinate or a malformed alias */
static ray_t* ref_find_sym(ray_t* x, prov_ent** out) {
    *out = NULL;
    const char* s; size_t n;
    q_pq_parts p;
    q_pq_kind kind = sym_text(x, &s, &n) ? q_handles_pq(s, n, &p) : Q_PQ_NONE;
    if (!kind) return q_err(QE_TYPE);
    if (!p.ok || p.table) return q_err(QE_DOMAIN);
    *out = find_alias(kind, ray_sym_intern_runtime(p.alias, p.alias_n));
    return NULL;
}

ray_t* q_provider_token(ray_t* handle, q_pq_kind kind) {
    prov_ent* e = NULL;
    if (handle && (handle->type == -RAY_I32 || handle->type == -RAY_I64))
        e = find_fd(handle->type == -RAY_I32 ? handle->i32 : handle->i64);
    else {
        ray_t* err = ref_find_sym(handle, &e);
        if (err) ray_error_free(err);
    }
    return e && e->kind == kind ? e->connid : NULL;
}

/* the kind's own row outlives every user close; the row leaves the table BEFORE the token closes — that may run q,
 * which may hclose this very alias again or open others */
static ray_t* ent_close(prov_ent* e) {
    if (e->internal) return q_err(QE_DOMAIN);
    prov_ent ent = *e;
    *e = g_ents[--g_n];
    ent_drop_token(&ent);
    if (ent.arg) ray_release(ent.arg);
    close((int)ent.fd);
    q_handles_deregister(ent.fd);
    return RAY_NULL_OBJ;
}

ray_t* q_provider_close(int64_t qh) {
    prov_ent* e = find_fd(qh);
    if (e) return ent_close(e);
    q_handles_deregister(qh);
    return RAY_NULL_OBJ;
}

ray_t* q_provider_close_sym(ray_t* x) {
    prov_ent* e;
    ray_t* err = ref_find_sym(x, &e);
    if (err) return err;
    return e ? ent_close(e) : RAY_NULL_OBJ;
}

/* A carrier is a `cols!`:pq:duckdb:db:t/` dict whose aux mark says it is the FLIP of that pair (ref/flip-splayed.md's
 * law for a mapped splay, on a dict because a provider table has no columns until fetched) — so the same shape built
 * by `!` stays plain 99h data.  q_type_coord_mark no-ops on the failed build, so no site guards it. */
static ray_t* carrier_make(q_pq_kind kind, int64_t alias, int64_t name, ray_t* cols) {
    ray_t* as = ray_sym_str(alias);            /* borrowed x2 */
    ray_t* ns = ray_sym_str(name);
    if (!as || !ns) return q_err(QE_TYPE);
    char buf[PROV_NAME_MAX];
    int m = snprintf(buf, sizeof buf, ":pq:%s:%.*s:%.*s/", q_handles_pq_kind_name(kind),
                     (int)ray_str_len(as), ray_str_ptr(as), (int)ray_str_len(ns), ray_str_ptr(ns));
    if (m <= 0 || m >= (int)sizeof buf) return q_err(QE_DOMAIN);
    int64_t coord = ray_sym_intern_runtime(buf, (size_t)m);
    ray_retain(cols);
    ray_t* d = ray_dict_new(cols, ray_sym(coord));
    q_type_coord_mark(d, Q_COORD_PROVIDER, coord);
    return d;
}

/* .X.bind[connid; name] -> advisory column names -> the carrier */
static ray_t* prov_bind(prov_ent* e, ray_t* name) {
    ray_t* args[2] = { e->connid, name };
    ray_t* cols = hook_call(e->kind, "bind", args, 2);
    if (!cols || RAY_IS_ERR(cols)) return cols ? cols : q_err(QE_TYPE);
    if (cols->type != RAY_SYM) { ray_release(cols); return q_err(QE_TYPE); }
    ray_t* car = carrier_make(e->kind, e->alias, name->i64, cols);
    ray_release(cols);
    return car;
}

/* A q peer's alias is exactly a kdb handle: every message goes to the peer unchanged, async is its negative.  On
 * DuckDB text is SQL and a name binds a carrier; nothing else is a message. */
static ray_t* prov_dispatch(prov_ent* e, ray_t* y, int sync) {
    if (e->stamp) {
        int64_t fd = token_fd(e->connid);
        ray_t* h = ray_i32((int32_t)(sync ? fd : -fd));
        ray_t* r = q_handles_apply(h, y);
        ray_release(h);
        return r;
    }
    const char* tp; int64_t tn;
    if (y && q_str_text_bytes(y, &tp, &tn)) {
        ray_t* txt = ray_charv(tp, tn);
        ray_t* flag = ray_bool(sync != 0);
        ray_t* args[3] = { e->connid, txt, flag };
        ray_t* r = hook_call(e->kind, "call", args, 3);
        ray_release(flag);
        ray_release(txt);
        return r;
    }
    const char* bp; size_t bn;
    if (sym_text(y, &bp, &bn)) return q_handles_name_ok(bp, bn) ? prov_bind(e, y) : q_err(QE_DOMAIN);
    return q_err(QE_TYPE);
}

ray_t* q_provider_apply(int64_t qh, ray_t* y) {
    prov_ent* e = find_fd(qh < 0 ? -qh : qh);
    if (!e) return q_err(QE_TYPE);
    ray_t* err = ent_live(&e);
    return err ? err : prov_dispatch(e, y, qh > 0);
}

/* the live alias a well-formed reference names; 'conn when it is not open — a reference never opens anything */
static ray_t* alias_live(q_pq_kind kind, const q_pq_parts* p, prov_ent** out) {
    *out = find_alias(kind, ray_sym_intern_runtime(p->alias, p->alias_n));
    return *out ? ent_live(out) : q_err(QE_CONN);
}

ray_t* q_provider_sym_apply(ray_t* head, ray_t** args, int64_t n) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    if (n != 1) return q_err(QE_RANK);
    const char* s; size_t sl;
    q_pq_parts p;
    q_pq_kind kind = sym_text(head, &s, &sl) ? q_handles_pq(s, sl, &p) : Q_PQ_NONE;
    if (!kind || !p.ok || p.table) return q_err(QE_DOMAIN);   /* a table is never a connection */
    prov_ent* e;
    ray_t* err = alias_live(kind, &p, &e);
    return err ? err : prov_dispatch(e, args[0], 1);
}


int q_provider_carrier_is(ray_t* x) {
    return q_type_coord_kind(x) == Q_COORD_PROVIDER;
}

ray_t* q_provider_flip(ray_t* cols, int64_t sym) {
    if (!cols || cols->type != RAY_SYM || coord_sym_form(sym) != 2) return NULL;
    ray_retain(cols);
    ray_t* d = ray_dict_new(cols, ray_sym(sym));
    q_type_coord_mark(d, Q_COORD_PROVIDER, sym);
    return d;
}

ray_t* q_provider_unflip(ray_t* car) {
    if (!q_provider_carrier_is(car)) return NULL;
    ray_t* k = ray_dict_keys(car);
    ray_t* v = ray_dict_vals(car);
    ray_retain(k);
    ray_retain(v);
    return ray_dict_new(k, v);                 /* a fresh block: aux zero-inits, so the mark is gone */
}

/* a table reference = the live alias + the table name; a kind without tables, a malformed coordinate or the
 * connection form is 'domain */
typedef struct { prov_ent* e; int64_t name; } tref_t;

static ray_t* tref_open(const char* s, size_t n, tref_t* t) {
    q_pq_parts p;
    q_pq_kind kind = q_handles_pq(s, n, &p);
    if (!kind || !p.ok || !p.table || !hook_ns(kind)) return q_err(QE_DOMAIN);
    t->name = ray_sym_intern_runtime(p.table, p.table_n);
    return alias_live(kind, &p, &t->e);
}

/* .X.<hook>[connid; name] on the resolved ref; the optional form answers NULL when the hook is undefined */
static ray_t* tref_hook(tref_t* t, const char* hook) {
    ray_t* nm = ray_sym(t->name);
    ray_t* args[2] = { t->e->connid, nm };
    ray_t* r = hook_call(t->e->kind, hook, args, 2);
    ray_release(nm);
    return r;
}

static ray_t* tref_hook_opt(tref_t* t, const char* hook) {
    ray_t* f = hook_fn(t->e->kind, hook);
    if (!f) return NULL;
    ray_t* nm = ray_sym(t->name);
    ray_t* args[2] = { t->e->connid, nm };
    ray_t* r = q_eval_apply_value(f, args, 2);
    ray_release(f);
    ray_release(nm);
    return r;
}

static ray_t* carrier_tref(ray_t* car, tref_t* t) {
    const char* p; size_t n;
    if (!sym_text(ray_dict_vals(car), &p, &n)) return q_err(QE_DOMAIN);
    return tref_open(p, n, t);
}

ray_t* q_provider_carrier_table(ray_t* car) {
    tref_t t;
    ray_t* err = carrier_tref(car, &t);
    return err ? err : tref_hook(&t, "get");
}

/* count/meta: the kind's own hook, else the host fallback — materialize and ask the table */
static ray_t* carrier_ask(ray_t* car, const char* hook, ray_t* (*fallback)(ray_t*)) {
    tref_t tr;
    ray_t* err = carrier_tref(car, &tr);
    if (err) return err;
    ray_t* r = tref_hook_opt(&tr, hook);
    if (r) return r;
    ray_t* t = tref_hook(&tr, "get");
    if (!t || RAY_IS_ERR(t)) return t ? t : q_err(QE_TYPE);
    r = fallback(t);
    ray_release(t);
    return r;
}

ray_t* q_provider_carrier_count(ray_t* car) { return carrier_ask(car, "count", q_count_fn); }
ray_t* q_provider_carrier_meta(ray_t* car)  { return carrier_ask(car, "meta", q_meta_fn); }

/* `get `:pq:duckdb:db:t/` — the carrier, splay symmetry: cols via bind on the live alias; the carrier keeps the
 * coordinate VERBATIM.  NULL = not a `:pq:` sym. */
ray_t* q_provider_get_carrier(ray_t* x) {
    const char* p; size_t n;
    if (!sym_text(x, &p, &n) || !q_handles_pq(p, n, NULL)) return NULL;
    tref_t tr;
    ray_t* err = tref_open(p, n, &tr);         /* connection form -> 'domain */
    if (err) return err;
    ray_t* cols = tref_hook(&tr, "bind");
    if (!cols || RAY_IS_ERR(cols)) return cols ? cols : q_err(QE_TYPE);
    if (cols->type != RAY_SYM) { ray_release(cols); return q_err(QE_TYPE); }
    ray_t* car = ray_dict_new(cols, ray_sym(x->i64));
    q_type_coord_mark(car, Q_COORD_PROVIDER, x->i64);
    return car;
}

/* the from-slot: carrier value, table coordinate, or a name resolving to a carrier.  0 = not a provider table; a
 * connection sym in a table position fills err_out with 'domain. */
static int from_tref(ray_t* t, tref_t* tr, ray_t** err_out) {
    *err_out = NULL;
    const char* p; size_t n;
    if (q_provider_carrier_is(t)) {
        *err_out = carrier_tref(t, tr);
        return 1;
    }
    int form = q_provider_coord_sym_form(t);
    if (form) {
        *err_out = form == 2 && sym_text(t, &p, &n) ? tref_open(p, n, tr) : q_err(QE_DOMAIN);
        return 1;
    }
    if (t && t->type == -RAY_SYM) {            /* functional `?[`t;...]` names */
        ray_t* v = q_env_handle_resolve(t->i64);
        if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); return 0; }
        int is = q_provider_carrier_is(v);
        if (is) *err_out = carrier_tref(v, tr);
        ray_release(v);
        return is;
    }
    return 0;
}

ray_t* q_provider_from_table(ray_t* t) {
    tref_t tr; ray_t* err;
    if (!from_tref(t, &tr, &err)) return NULL;
    return err ? err : tref_hook(&tr, "get");
}

/* the provider-truth column list for .X.qsql — ONE live bind round-trip at push time (a carrier's embedded keys are
 * advisory and can be stale after remote schema drift — pushing on them substitutes a same-named client global for a
 * NEW column).  A bind ERROR is the answer, once: the fallback would only ask the same provider again through get (a
 * dropped table said no already).  A non-list answer is unavailable (host fallback). */
static ray_t* qsql_cols(tref_t* tr) {
    ray_t* cols = tref_hook(tr, "bind");
    if (cols && (RAY_IS_ERR(cols) || cols->type == RAY_SYM)) return cols;
    if (cols) ray_release(cols);
    return NULL;
}

ray_t* q_provider_qsql_push(ray_t** args, int64_t n) {
    tref_t tr; ray_t* err;
    if (n < 4) return NULL;
    if (!from_tref(args[0], &tr, &err)) return NULL;
    if (err) return err;
    ray_t* f = hook_fn(tr.e->kind, "qsql");
    if (!f) return NULL;                       /* host fallback: the residual law */
    ray_t* cols = qsql_cols(&tr);
    if (!cols || RAY_IS_ERR(cols)) { ray_release(f); return cols; }
    ray_t* tree = ray_list_new(n);
    ray_t* nm = ray_sym(tr.name);
    tree = ray_list_append(tree, nm);          /* slot 0: the BARE underlying name */
    ray_release(nm);
    for (int64_t i = 1; i < n && !RAY_IS_ERR(tree); i++)
        tree = ray_list_append(tree, args[i]);
    if (RAY_IS_ERR(tree)) { ray_release(f); ray_release(cols); return tree; }
    ray_t* cargs[3] = { tr.e->connid, cols, tree };
    ray_t* r = q_eval_apply_value(f, cargs, 3);
    ray_release(f);
    ray_release(cols);
    ray_release(tree);
    if (r && RAY_IS_NULL(r)) { ray_release(r); return NULL; }  /* hook declined */
    return r;
}


/* The link seam.  .X.i.link needs the live connection, .X.i.unlink drops by name and gets :: for a token once the
 * alias is closed.  Best-effort: the global is already bound, so a hook's error is dropped (the .z.vs shape) and a
 * hook that binds a carrier itself does not re-enter. */
static int g_in_link;

static ray_err_t link_hook(const char* hook, int64_t qname, ray_t* car) {
    const char* s; size_t n;
    q_pq_parts p;
    if (g_in_link || !sym_text(ray_dict_vals(car), &s, &n)) return RAY_OK;
    q_pq_kind kind = q_handles_pq(s, n, &p);
    if (!kind || !p.ok || !p.table) return RAY_OK;
    ray_t* f = hook_fn(kind, hook);
    if (!f) return RAY_OK;
    int link = strcmp(hook, "i.link") == 0;
    prov_ent* e = find_alias(kind, ray_sym_intern_runtime(p.alias, p.alias_n));
    if (e || !link) {
        ray_t* qn = ray_sym(qname);
        ray_t* tn = ray_sym(ray_sym_intern_runtime(p.table, p.table_n));
        ray_t* args[3] = { e && e->connid ? e->connid : RAY_NULL_OBJ, qn, tn };
        g_in_link = 1;
        ray_t* r = q_eval_apply_value(f, args, link ? 3 : 2);
        g_in_link = 0;
        if (r && RAY_IS_ERR(r)) { q_err_drop(); ray_error_free(r); }
        else if (r) ray_release(r);
        ray_release(qn);
        ray_release(tn);
    }
    ray_release(f);
    return RAY_OK;
}

ray_err_t q_provider_link(int64_t qname, ray_t* car)   { return link_hook("i.link", qname, car); }
ray_err_t q_provider_unlink(int64_t qname, ray_t* car) { return link_hook("i.unlink", qname, car); }

ray_t* q_provider_write(ray_t* x, ray_t* y, int upsert) {
    const char* p; size_t n;
    if (!sym_text(x, &p, &n) || !q_handles_pq(p, n, NULL)) return NULL;
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    tref_t tr;
    ray_t* err = tref_open(p, n, &tr);
    if (err) return err;
    ray_t* nm = ray_sym(tr.name);
    ray_t* args[3] = { tr.e->connid, nm, y };
    ray_t* r = hook_call(tr.e->kind, upsert ? "upsert" : "set", args, 3);
    ray_release(nm);
    return r;
}

ray_t* q_provider_hdel(ray_t* x) {
    const char* p; size_t n;
    if (!sym_text(x, &p, &n) || !q_handles_pq(p, n, NULL)) return NULL;
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    tref_t tr;
    ray_t* err = tref_open(p, n, &tr);
    if (err) return err;
    ray_t* r = tref_hook_opt(&tr, "hdel");
    if (!r) return q_err(QE_NYI);
    if (RAY_IS_ERR(r)) return r;
    ray_release(r);
    ray_retain(x);
    return x;
}


/* The load door.  The hook answers the NAMES (a sym atom, a sym vector, or an empty list) and the host binds them:
 * the provider knows its catalog, the host owns the pointer (prov_bind) and the root — under `\d` a q-side `set`
 * lands in the context, and a load defines tables in the root (the `\l dir` law). */
static ray_t* load_names_ok(ray_t* names) {
    if (!names || RAY_IS_ERR(names)) return names ? names : q_err(QE_TYPE);
    if (names->type == RAY_SYM || (names->type == RAY_LIST && q_count(names) == 0)) return NULL;
    if (names->type != -RAY_SYM) { ray_release(names); return q_err(QE_TYPE); }
    return NULL;
}

static ray_t* load_bind(prov_ent* e, int64_t name) {
    ray_t* nm = ray_sym(name);
    ray_t* car = prov_bind(e, nm);
    ray_release(nm);
    if (!car || RAY_IS_ERR(car)) return car ? car : q_err(QE_TYPE);
    ray_err_t rc = q_env_set(name, car);
    ray_release(car);
    return rc == RAY_OK ? NULL : q_env_err(rc);
}

ray_t* q_provider_load(const char* s, size_t n, ray_t* tables) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    if (!tables || !(RAY_IS_NULL(tables) || tables->type == -RAY_SYM || tables->type == RAY_SYM ||
                     (tables->type == RAY_LIST && q_count(tables) == 0)))
        return q_err(QE_TYPE);
    q_pq_parts p;
    q_pq_kind kind = q_handles_pq(s, n, &p);
    if (!kind || !p.ok) return q_err(QE_DOMAIN);
    prov_ent* e;
    ray_t* err = alias_live(kind, &p, &e);     /* LIVE alias only: a load never opens a connection */
    if (err) return err;
    ray_t* f = hook_fn(kind, "i.load");
    if (!f) return q_err(QE_NYI);
    ray_t* one = p.table ? ray_sym(ray_sym_intern_runtime(p.table, p.table_n)) : NULL;
    ray_t* args[2] = { e->connid, one ? one : tables };
    prov_ent ent = *e;                         /* the hook may open or close aliases: the row can move */
    ray_retain(ent.connid);
    ray_t* names = q_eval_apply_value(f, args, 2);
    ray_release(f);
    if (one) ray_release(one);
    err = load_names_ok(names);
    if (err) { ray_release(ent.connid); return err; }
    int64_t m = q_count(names);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, m > 0 ? m : 1);
    int64_t scope = q_env_scope(0);            /* bound at the root, whatever the session or a calling lambda says */
    for (int64_t i = 0; i < m && !err; i++) {
        int64_t id = names->type == -RAY_SYM ? names->i64 : ray_vec_get_sym_id(names, i);
        err = load_bind(&ent, id);
        if (!err) out = ray_vec_append(out, &id);
    }
    q_env_scope(scope);
    ray_release(ent.connid);
    ray_release(names);
    if (err) { ray_release(out); return err; }
    return out;
}

/* .pq.i.load[h; tables] — the handle (the alias sym, or the legacy int) spelled back to its coordinate */
static ray_t* pq_load_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    ray_t* h = args[0];
    int64_t fd = h && h->type == -RAY_I32 ? h->i32 : h && h->type == -RAY_I64 ? h->i64 : -1;
    prov_ent* e = fd >= 0 ? find_fd(fd) : NULL;
    ray_t* hs = e ? ray_sym(e->handle) : h;
    const char* p; size_t pn;
    ray_t* r = sym_text(hs, &p, &pn) ? q_provider_load(p, pn, args[1]) : q_err(QE_TYPE);
    if (e) ray_release(hs);
    return r;
}

void q_provider_pq_register(void) {
    static const char nm[] = ".pq.i.load";
    ray_t* obj = ray_fn_vary(nm, RAY_FN_NONE, pq_load_fn);
    q_env_bind_native(nm, obj, 2);
    ray_release(obj);
}
