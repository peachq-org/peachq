/* io/q_kafka.c — the `.kafka.i.*` natives: the librdkafka clients behind a `:pq:kafka:` alias, a consumer and a
 * producer, each made on its first use.  The library is dlopen'd on first use (q_kafka_api.h is the whole surface):
 * PEACHQ_KAFKA_LIB, else $QHOME, beside `q`, then the system soname.  A client's queue writes one byte to a
 * socketpair when it goes non-empty; that fd is a poll selector, and its wake drains the queue into ONE table per
 * wake and hands it to `.kafka.i.deliver` (qlib/kafka.q) on the main loop — librdkafka's threads never run q.  A
 * bounded read of a topic (the table coordinate) runs on a throwaway consumer over the alias's consumer config, so a
 * subscription's assignment is never disturbed.  Error texts are `'kafka: <librdkafka's text>`; q-side misuse is
 * 'type/'domain/'conn. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "qlang/q_count.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"      /* the int lane: config values, the cells of a partition table, a ms argument */
#include "qlang/eval/q_eval.h"      /* q_eval_apply_call_sym — the deliver and errcb hooks; _is_fn/_rank — a target's rank */
#include "qlang/io/q_dl.h"          /* q_dl_open / q_dl_sym / q_dl_unavailable */
#include "qlang/io/q_exedir.h"      /* q_exedir — the library beside `q` */
#include "qlang/io/q_handles.h"     /* q_handles_pq_of / _pq_kind_name — the alias spellings */
#include "qlang/io/q_kafka.h"
#include "qlang/io/q_kafka_api.h"
#include "qlang/io/q_provider.h"    /* q_provider_token — the alias behind a public verb's h */
#include "qlang/ops/q_index.h"      /* q_index_elem_at — the one element read */
#include "qlang/q_console.h"        /* q_console_door / _write_err — the loop's q runs on the server console */
#include "qlang/q_env.h"            /* q_env_bind_native / q_env_get */
#include "qlang/q_prim.h"           /* q_str_subject_bytes / q_str_text_or_bytes — names, keys and payloads */
#include "core/poll.h"
#include "core/sock.h"              /* ray_sock_pair / _set_nonblocking / _close — the wake's two ends */
#include "core/runtime.h"           /* ray_runtime_get_poll */
#include "lang/env.h"               /* ray_fn_vary */
#include "lang/eval.h"              /* RAY_FN_NONE */
#include "table/sym.h"              /* ray_sym_str */
#include <rayforce.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#define KFK_TOKEN_MARK  (1 << 29)     /* on every token, so no token spells a provider fd */
#define KFK_MAX_CLIENTS 64
#define KFK_TIMEOUT_MS  5000          /* one broker round-trip: metadata, committed, watermarks, seek */
#define KFK_MAX_WAKE    10000         /* messages one wake drains before it yields and re-arms, unless the client says */
#define KFK_NAME_MAX    256
#define KFK_EPOCH_NS    946684800000000000LL

typedef struct kfk_client kfk_client;

/* one librdkafka client of an alias and its wake; rk NULL = not made yet */
typedef struct {
    kfk_client*       owner;
    int               role;        /* KFK_CONSUMER / KFK_PRODUCER, its index in owner->side */
    rd_kafka_t*       rk;
    rd_kafka_queue_t* q;
    int               fd[2];       /* [0] the poll's end, [1] librdkafka's */
    int64_t           sel;         /* poll selector id, -1 = none (an embedder without a loop) */
    rd_kafka_topic_partition_list_t* owed;   /* a consumer's async commits, made durable by a sync one at close */
} kfk_side;

struct kfk_client {
    kfk_side          side[2];     /* made on first use: the consumer by a read, the producer by a publish */
    int               live;
    int               serving;     /* inside a drain, poll or flush: a close waits for it */
    int               closing;
    int64_t           handle;      /* sym id of `:pq:kafka:<alias>` */
    int64_t           dr_failed;   /* deliveries reported failed since the last flush */
    rd_kafka_resp_err_t dr_err;    /* the last of them */
    int64_t           max_wake;
    ray_t*            cfg[2];      /* owned, by role: the hopen config less the other role's keys */
};

static kfk_api_t   A;
static kfk_client  g_cl[KFK_MAX_CLIENTS];
static int         g_loaded;       /* 1 once the table is filled */
static int         g_atexit;
static int         g_shutdown;     /* callbacks run no q once the process is leaving */
static int         g_reads;        /* bounded reads so far: each one's throwaway group is its own */

#define KFK_NSYMS (sizeof KFK_SYMS / sizeof *KFK_SYMS - 1)
_Static_assert(sizeof(kfk_api_t) == KFK_NSYMS * sizeof(void*), "KFK_SYMS[] must match kfk_api_t, in order");

static ray_t* kfk_err_text(const char* text) {
    char b[600];
    int n = snprintf(b, sizeof b, "kafka: %s", text);
    return q_err_from_text(b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static ray_t* kfk_err(rd_kafka_resp_err_t e) { return kfk_err_text(A.err2str(e)); }

static ray_t* kfk_err_obj(rd_kafka_error_t* e) {
    ray_t* r = kfk_err_text(A.error_string(e));
    A.error_destroy(e);
    return r;
}

static const char* const KFK_SONAMES[] = {
#if defined(_WIN32)
    "librdkafka.dll", "rdkafka.dll",
#elif defined(__APPLE__)
    "librdkafka.1.dylib", "librdkafka.dylib",
#else
    "librdkafka.so.1", "librdkafka.so",
#endif
    NULL
};

static void* kfk_dl_in(const char* dir, const char* name) {
    char cand[600];
    if (snprintf(cand, sizeof cand, "%s/%s", dir, name) >= (int)sizeof cand) return NULL;
    return q_dl_open(cand);
}

/* the DuckDB doors: PEACHQ_KAFKA_LIB names the library outright, else $QHOME, beside `q`, then the system */
static void* kfk_dl_find(void) {
    const char* envp = getenv("PEACHQ_KAFKA_LIB");
    if (envp && *envp) return q_dl_open(envp);
    const char* qh = getenv("QHOME");
    char dir[512];
    int exe = q_exedir(dir, sizeof dir);
    void* dl = NULL;
    for (int i = 0; !dl && KFK_SONAMES[i]; i++) {
        if (qh && *qh) dl = kfk_dl_in(qh, KFK_SONAMES[i]);
        if (!dl && exe) dl = kfk_dl_in(dir, KFK_SONAMES[i]);
        if (!dl) dl = q_dl_open(KFK_SONAMES[i]);
    }
    return dl;
}

static ray_t* kfk_load(void) {
    if (g_loaded) return NULL;
    const char* why = q_dl_unavailable();
    if (why) return q_err_from_text(why, strlen(why));
    void* dl = kfk_dl_find();
    if (!dl) return kfk_err_text("librdkafka not found - set PEACHQ_KAFKA_LIB or install librdkafka 2.0 or later");
    /* 1.x can lose a failed target's held batch (Dsfxqt) and leave a dropped member's refused commit untold (Dvcryr) */
    int (*vnum)(void) = (int (*)(void))(uintptr_t)q_dl_sym(dl, "rd_kafka_version");
    int ok = vnum && vnum() >= 0x02000000;
    void** slots = (void**)&A;
    for (size_t i = 0; ok && i < KFK_NSYMS; i++) ok = (slots[i] = q_dl_sym(dl, KFK_SYMS[i])) != NULL;
    if (!ok) {
        const char* (*ver)(void) = (const char* (*)(void))(uintptr_t)q_dl_sym(dl, "rd_kafka_version_str");
        char b[256];
        snprintf(b, sizeof b, "librdkafka 2.0 or later required (found %s)", ver ? ver() : "no version");
        memset(&A, 0, sizeof A);
        return kfk_err_text(b);
    }
    g_loaded = 1;
    return NULL;
}

/* ---- the client behind an argument ------------------------------------------------------------------------ */

/* h is the handle sym, its legacy int, or the token itself; 'conn for an alias that is not open, 'type otherwise */
static ray_t* kfk_client_of(ray_t* h, kfk_client** out) {
    *out = NULL;
    if (h && h->type == -RAY_I32 && (h->i32 & KFK_TOKEN_MARK)) {
        int slot = h->i32 & ~KFK_TOKEN_MARK;
        if (slot < 0 || slot >= KFK_MAX_CLIENTS || !g_cl[slot].live) return q_err(QE_CONN);
        *out = &g_cl[slot];
        return NULL;
    }
    ray_t* tok = q_provider_token(h, Q_PQ_KAFKA);
    if (tok) return kfk_client_of(tok, out);
    const char* s; size_t n;
    return q_handles_pq_of(h, &s, &n) == Q_PQ_KAFKA ? q_err(QE_CONN) : q_err(QE_TYPE);
}

/* a sym atom's text NUL-terminated into buf; 0 = not a sym or too long */
static int kfk_sym_cstr(ray_t* x, char* buf, size_t cap) {
    ray_t* s = x && x->type == -RAY_SYM ? ray_sym_str(x->i64) : NULL;
    if (!s || ray_str_len(s) >= cap) return 0;
    memcpy(buf, ray_str_ptr(s), ray_str_len(s));
    buf[ray_str_len(s)] = '\0';
    return 1;
}

/* ---- values ------------------------------------------------------------------------------------------------- */

static ray_t* kfk_sym_of(const char* s) { return ray_sym(ray_sym_intern_runtime(s, strlen(s))); }

/* append an OWNED item to a list column, answering the list (an error ends the build) */
static ray_t* kfk_list_push(ray_t* l, ray_t* item) {
    if (!l || RAY_IS_ERR(l)) { if (item) ray_release(item); return l; }
    if (!item || RAY_IS_ERR(item)) { ray_release(l); return item ? item : q_err(QE_OOM); }
    ray_t* r = ray_list_append(l, item);
    ray_release(item);
    return r;
}

static ray_t* kfk_vec_push(ray_t* v, const void* elem) {
    if (!v || RAY_IS_ERR(v)) return v;
    return ray_vec_append(v, elem);
}

/* a table from OWNED columns: every column is consumed, the first error (or OOM) is the answer */
static ray_t* kfk_table(const char* const* names, ray_t** cols, int n) {
    ray_t* bad = NULL;
    for (int i = 0; i < n; i++)
        if (!bad && (!cols[i] || RAY_IS_ERR(cols[i]))) bad = cols[i] ? cols[i] : q_err(QE_OOM);
    ray_t* t = bad ? NULL : ray_table_new(n);
    for (int i = 0; i < n; i++) {
        if (t && !RAY_IS_ERR(t)) t = ray_table_add_col(t, ray_sym_intern_runtime(names[i], strlen(names[i])), cols[i]);
        if (cols[i] && cols[i] != bad) ray_release(cols[i]);
    }
    return bad ? bad : t;
}

/* a `topic partition [offset]` table -> a partition list (NULL + *err on a bad shape) */
static rd_kafka_topic_partition_list_t* kfk_tpl_of(ray_t* t, int with_offset, ray_t** err) {
    *err = NULL;
    if (!t || t->type != RAY_TABLE) { *err = q_err(QE_TYPE); return NULL; }
    ray_t* tc = ray_table_get_col(t, ray_sym_intern_runtime("topic", 5));
    ray_t* pc = ray_table_get_col(t, ray_sym_intern_runtime("partition", 9));
    ray_t* oc = with_offset ? ray_table_get_col(t, ray_sym_intern_runtime("offset", 6)) : NULL;
    if (!tc || tc->type != RAY_SYM || !q_type_is_int_vec(pc) || (with_offset && !q_type_is_int_vec(oc))) {
        *err = q_err(QE_TYPE);
        return NULL;
    }
    int64_t n = q_count(t);
    rd_kafka_topic_partition_list_t* l = A.topic_partition_list_new((int)(n ? n : 1));
    for (int64_t i = 0; i < n; i++) {
        ray_t* s = ray_sym_str(ray_vec_get_sym_id(tc, i));
        char topic[KFK_NAME_MAX];
        if (!s || ray_str_len(s) >= sizeof topic) { *err = q_err(QE_DOMAIN); break; }
        memcpy(topic, ray_str_ptr(s), ray_str_len(s));
        topic[ray_str_len(s)] = '\0';
        rd_kafka_topic_partition_t* tp = A.topic_partition_list_add(l, topic, (int32_t)q_type_ivec_get(pc, i));
        tp->offset = oc ? q_type_ivec_get(oc, i) : KFK_OFFSET_INVALID;
    }
    if (*err) { A.topic_partition_list_destroy(l); return NULL; }
    return l;
}

/* ---- the batch one wake delivers ------------------------------------------------------------------------- */

typedef struct { ray_t* topic; ray_t* part; ray_t* off; ray_t* ts; ray_t* key; ray_t* hdr; ray_t* data; } kfk_batch;

static void kfk_batch_init(kfk_batch* b) {
    b->topic = ray_sym_vec_new(RAY_SYM_W64, 16);
    b->part  = ray_vec_new(RAY_I32, 16);
    b->off   = ray_vec_new(RAY_I64, 16);
    b->ts    = ray_vec_new(RAY_TIMESTAMP, 16);
    b->key   = ray_list_new(16);
    b->hdr   = ray_list_new(16);
    b->data  = ray_list_new(16);
}

/* a message's headers as `names!values` (byte vectors), the empty dict for none */
static ray_t* kfk_headers_of(const rd_kafka_message_t* m) {
    rd_kafka_headers_t* h = NULL;
    size_t n = A.message_headers(m, &h) == KFK_ERR_NO_ERROR && h ? A.header_cnt(h) : 0;
    ray_t* ks = ray_sym_vec_new(RAY_SYM_W64, n ? (int64_t)n : 1);
    ray_t* vs = ray_list_new(n ? (int64_t)n : 1);
    for (size_t i = 0; i < n; i++) {
        const char* name; const void* vp; size_t vn;
        if (A.header_get_all(h, i, &name, &vp, &vn) != KFK_ERR_NO_ERROR) break;
        int64_t id = ray_sym_intern_runtime(name, strlen(name));
        ks = kfk_vec_push(ks, &id);
        vs = kfk_list_push(vs, ray_vec_from_raw(RAY_BYTE_ONLY, vp ? vp : "", vp ? (int64_t)vn : 0));
    }
    if (!ks || RAY_IS_ERR(ks) || !vs || RAY_IS_ERR(vs)) {
        if (ks && !RAY_IS_ERR(ks)) ray_release(ks);
        if (vs && !RAY_IS_ERR(vs)) ray_release(vs);
        return q_err(QE_OOM);
    }
    return ray_dict_new(ks, vs);
}

static void kfk_batch_add(kfk_batch* b, const rd_kafka_message_t* m) {
    int64_t topic = ray_sym_intern_runtime(A.topic_name(m->rkt), strlen(A.topic_name(m->rkt)));
    rd_kafka_timestamp_type_t tt;
    int64_t ms = A.message_timestamp(m, &tt);
    int64_t ns = tt == KFK_TIMESTAMP_NOT_AVAILABLE ? NULL_I64 : ms * 1000000LL - KFK_EPOCH_NS;
    b->topic = kfk_vec_push(b->topic, &topic);
    b->part  = kfk_vec_push(b->part, &m->partition);
    b->off   = kfk_vec_push(b->off, &m->offset);
    b->ts    = kfk_vec_push(b->ts, &ns);
    b->key   = kfk_list_push(b->key, ray_vec_from_raw(RAY_BYTE_ONLY, m->key ? m->key : "", m->key ? (int64_t)m->key_len : 0));
    b->hdr   = kfk_list_push(b->hdr, kfk_headers_of(m));
    /* a tombstone (Kafka's null value) is :: beside an empty value's 0x */
    ray_t* val = m->payload ? ray_vec_from_raw(RAY_BYTE_ONLY, m->payload, (int64_t)m->len) : (ray_retain(RAY_NULL_OBJ), RAY_NULL_OBJ);
    b->data  = kfk_list_push(b->data, val);
}

static ray_t* kfk_batch_table(kfk_batch* b) {
    static const char* const names[7] = { "topic", "partition", "offset", "msgtime", "msgkey", "headers", "data" };
    ray_t* cols[7] = { b->topic, b->part, b->off, b->ts, b->key, b->hdr, b->data };
    return kfk_table(names, cols, 7);
}

/* ---- q from the loop -------------------------------------------------------------------------------------- */

/* an error out of loop-run q has no frame to raise into: it is reported on the server's stderr, as .z.ts's is */
static void kfk_report(ray_t* r) {
    if (!r) return;
    if (!RAY_IS_ERR(r)) { ray_release(r); return; }
    int64_t n;
    const char* t = q_err_text(r, &n);
    char b[600];
    int m = snprintf(b, sizeof b, "kafka: '%.*s", (int)(n > 500 ? 500 : n), t ? t : "");
    q_console_write_err(b, (size_t)m, true);
    q_err_drop();
    ray_error_free(r);
}

static ray_t* kfk_call(const char* name, ray_t** args, int64_t n) {
    FILE* door = q_console_door(stdout);
    ray_t* r = q_eval_apply_call_sym(ray_sym_intern_runtime(name, strlen(name)), args, n);
    q_console_door(door);
    return r;
}

/* .kafka.errcb[h; code; `topic`partition`offset`error!(...)] when the user defined it */
static void kfk_errcb(kfk_client* c, const char* code, const char* topic, int32_t partition, int64_t offset,
                      const char* text) {
    if (g_shutdown || !q_env_get(ray_sym_intern_runtime(".kafka.errcb", 12))) return;
    static const char* const kn[4] = { "topic", "partition", "offset", "error" };
    ray_t* ks = ray_sym_vec_new(RAY_SYM_W64, 4);
    for (int i = 0; i < 4 && ks && !RAY_IS_ERR(ks); i++) {
        int64_t id = ray_sym_intern_runtime(kn[i], strlen(kn[i]));
        ks = ray_vec_append(ks, &id);
    }
    ray_t* vs = ray_list_new(4);
    vs = kfk_list_push(vs, topic ? kfk_sym_of(topic) : ray_sym(0));
    vs = kfk_list_push(vs, ray_i32(topic ? partition : NULL_I32));
    vs = kfk_list_push(vs, ray_i64(topic ? offset : NULL_I64));
    vs = kfk_list_push(vs, ray_charv(text, (int64_t)strlen(text)));
    if (!ks || RAY_IS_ERR(ks) || !vs || RAY_IS_ERR(vs)) {
        if (ks && !RAY_IS_ERR(ks)) ray_release(ks);
        if (vs && !RAY_IS_ERR(vs)) ray_release(vs);
        return;
    }
    ray_t* args[3] = { ray_sym(c->handle), kfk_sym_of(code), ray_dict_new(ks, vs) };
    kfk_report(kfk_call(".kafka.errcb", args, 3));
    for (int i = 0; i < 3; i++) ray_release(args[i]);
}

/* librdkafka serves these from the thread that polls — ours, inside a drain */
static void kfk_error_cb(rd_kafka_t* rk, int err, const char* reason, void* opaque) {
    (void)rk;
    kfk_client* c = opaque;
    if (c && c->live) kfk_errcb(c, A.err2name(err), NULL, 0, 0, reason);
}

static int kfk_stats_cb(rd_kafka_t* rk, char* json, size_t json_len, void* opaque) {
    (void)rk;
    kfk_client* c = opaque;
    if (g_shutdown || !c || !c->live || !q_env_get(ray_sym_intern_runtime(".kafka.statcb", 13))) return 0;
    ray_t* args[2] = { ray_sym(c->handle), ray_charv(json, (int64_t)json_len) };
    kfk_report(kfk_call(".kafka.statcb", args, 2));
    ray_release(args[0]);
    ray_release(args[1]);
    return 0;                                   /* librdkafka frees json */
}

/* a delivery report: a failed one is counted for the next flush and told to .kafka.errcb */
static void kfk_dr_cb(rd_kafka_t* rk, const rd_kafka_message_t* m, void* opaque) {
    (void)rk;
    kfk_client* c = opaque;
    if (!m->err || !c) return;
    c->dr_failed++;
    c->dr_err = m->err;
    kfk_errcb(c, A.err2name(m->err), m->rkt ? A.topic_name(m->rkt) : NULL, m->partition, m->offset, A.err2str(m->err));
}

/* an async (per-batch) commit's outcome: each partition the broker refused is told to .kafka.errcb */
static void kfk_commit_cb(rd_kafka_t* rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t* offsets,
                          void* opaque) {
    (void)rk;
    kfk_client* c = opaque;
    if (!c || !c->live || err == KFK_ERR_NO_OFFSET) return;
    int told = 0;
    for (int i = 0; offsets && i < offsets->cnt; i++) {
        const rd_kafka_topic_partition_t* tp = &offsets->elems[i];
        if (!tp->err) continue;
        kfk_errcb(c, A.err2name(tp->err), tp->topic, tp->partition, tp->offset, A.err2str(tp->err));
        told = 1;
    }
    if (err && !told) kfk_errcb(c, A.err2name(err), NULL, 0, 0, A.err2str(err));
}

/* a sync commit's outcome: the first partition the broker refused, else the commit's own error (topic "") */
typedef struct { rd_kafka_resp_err_t err; char topic[KFK_NAME_MAX]; int32_t partition; int64_t offset; } kfk_refusal;

static void kfk_sync_cb(rd_kafka_t* rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t* offsets,
                        void* opaque) {
    (void)rk;
    kfk_refusal* r = opaque;
    for (int i = 0; offsets && i < offsets->cnt && !r->err; i++) {
        const rd_kafka_topic_partition_t* tp = &offsets->elems[i];
        if (!tp->err) continue;
        *r = (kfk_refusal){ .err = tp->err, .partition = tp->partition, .offset = tp->offset };
        snprintf(r->topic, sizeof r->topic, "%s", tp->topic);
    }
    if (!r->err && err != KFK_ERR_NO_OFFSET) r->err = err;
}

/* served by this call on a queue of its own, so the async commits' callback never sees it */
static kfk_refusal kfk_commit_sync(rd_kafka_t* rk, const rd_kafka_topic_partition_list_t* l) {
    kfk_refusal r = { 0 };
    rd_kafka_resp_err_t e = A.commit_queue(rk, l, NULL, kfk_sync_cb, &r);
    if (!r.err && e != KFK_ERR_NO_OFFSET) r.err = e;
    return r;
}

/* an async commit is owed a sync one at close; a sync commit settles what it names */
static void kfk_owe(kfk_side* s, const rd_kafka_topic_partition_list_t* l, int async) {
    if (async && !s->owed) s->owed = A.topic_partition_list_new(l->cnt ? l->cnt : 1);
    for (int i = 0; s->owed && i < l->cnt; i++) {
        const rd_kafka_topic_partition_t* e = &l->elems[i];
        rd_kafka_topic_partition_t* tp = A.topic_partition_list_find(s->owed, e->topic, e->partition);
        if (!tp && async) tp = A.topic_partition_list_add(s->owed, e->topic, e->partition);
        if (tp) tp->offset = async ? e->offset : KFK_OFFSET_INVALID;
    }
}

/* the owed commits made durable before the consumer leaves, where they still move the group forward: a partition
 * it no longer holds, or one revoked and won back, may have moved on under another member, and an old offset
 * committed there would rewind it.  When the broker cannot say what the group committed, nothing is: redelivery is
 * the safe side. */
static void kfk_settle(kfk_side* s) {
    rd_kafka_topic_partition_list_t* held = NULL;
    if (s->owed && !A.assignment(s->rk, &held)) {
        rd_kafka_topic_partition_list_t* now = A.topic_partition_list_new(held->cnt ? held->cnt : 1);
        for (int i = 0; i < held->cnt; i++) {
            rd_kafka_topic_partition_t* tp = A.topic_partition_list_find(s->owed, held->elems[i].topic,
                                                                         held->elems[i].partition);
            if (tp && tp->offset >= 0) A.topic_partition_list_add(now, tp->topic, tp->partition);
        }
        kfk_refusal r = { .err = now->cnt ? A.committed(s->rk, now, KFK_TIMEOUT_MS) : KFK_ERR_NO_ERROR };
        rd_kafka_topic_partition_list_t* due = A.topic_partition_list_new(now->cnt ? now->cnt : 1);
        for (int i = 0; i < now->cnt && !r.err; i++) {
            const rd_kafka_topic_partition_t* e = &now->elems[i];
            int64_t mine = A.topic_partition_list_find(s->owed, e->topic, e->partition)->offset;
            if (e->offset < mine) A.topic_partition_list_add(due, e->topic, e->partition)->offset = mine;
        }
        if (!r.err && due->cnt) r = kfk_commit_sync(s->rk, due);
        if (r.err) kfk_errcb(s->owner, A.err2name(r.err), r.topic[0] ? r.topic : NULL, r.partition, r.offset,
                             A.err2str(r.err));
        A.topic_partition_list_destroy(due);
        A.topic_partition_list_destroy(now);
    }
    if (held) A.topic_partition_list_destroy(held);
    if (s->owed) A.topic_partition_list_destroy(s->owed);
    s->owed = NULL;
}

static void kfk_drop(kfk_client* c);

/* librdkafka serves callbacks from inside poll and flush: q may run there and close this very client, so the close
 * is deferred until the call is off the stack (the same guard a drain uses) */
static void kfk_poll(kfk_side* s, int timeout_ms) {
    kfk_client* c = s->owner;
    if (c->serving) { A.poll(s->rk, timeout_ms); return; }
    c->serving = 1;
    A.poll(s->rk, timeout_ms);
    c->serving = 0;
    if (c->closing) kfk_drop(c);
}

/* ONE wake: the queue drained into one table and delivered once; a drain that hit the cap re-arms the fd so the
 * loop comes back for the rest.  A producer's wake serves its delivery reports.  Answers the messages delivered. */
static int64_t kfk_serve(kfk_side* s, int timeout_ms) {
    kfk_client* c = s->owner;
    if (c->serving) return 0;
    c->serving = 1;
    int64_t n = 0;
    if (s->role == KFK_PRODUCER) A.poll(s->rk, timeout_ms);
    else {
        kfk_batch b;
        kfk_batch_init(&b);
        for (int64_t seen = 0; n < c->max_wake && seen / 2 < c->max_wake && !c->closing; seen++) {
            rd_kafka_message_t* m = A.consumer_poll(s->rk, n ? 0 : timeout_ms);
            if (!m) break;
            if (m->err) kfk_errcb(c, A.err2name(m->err), m->rkt ? A.topic_name(m->rkt) : NULL, m->partition,
                                  m->offset, A.message_errstr(m));
            else { kfk_batch_add(&b, m); n++; }
            A.message_destroy(m);
        }
        ray_t* t = kfk_batch_table(&b);
        if (n && !c->closing && !RAY_IS_ERR(t)) {
            ray_t* args[2] = { ray_sym(c->handle), t };
            kfk_report(kfk_call(".kafka.i.deliver", args, 2));
            ray_release(args[0]);
        }
        if (t) kfk_report(t);
#if !defined(_WIN32)
        if (n >= c->max_wake && s->fd[1] >= 0) (void)!write(s->fd[1], "1", 1);
#endif
    }
    c->serving = 0;
    if (c->closing) kfk_drop(c);
    return n;
}

#if !defined(_WIN32)
static ray_t* kfk_readable(ray_poll_t* poll, ray_selector_t* sel) {
    (void)poll;
    kfk_side* s = sel->data;
    char buf[64];
    while (read((int)sel->fd, buf, sizeof buf) > 0) {}
    if (s->owner->live) kfk_serve(s, 0);
    return NULL;
}

/* the io-event fd on the poll, non-blocking at both ends */
static ray_t* kfk_wire(kfk_side* s) {
    if (ray_sock_pair(s->fd) != 0) return q_err(QE_OS);
    for (int i = 0; i < 2; i++) ray_sock_set_nonblocking(s->fd[i]);
    A.queue_io_event_enable(s->q, s->fd[1], "1", 1);
    ray_poll_t* p = (ray_poll_t*)ray_runtime_get_poll();
    if (!p) return NULL;
    ray_poll_reg_t reg = { 0 };
    reg.fd = s->fd[0];
    reg.type = RAY_SEL_SOCKET;
    reg.data = s;
    reg.read_fn = kfk_readable;
    s->sel = ray_poll_register(p, &reg);
    return s->sel < 0 ? q_err(QE_IO) : NULL;
}
#endif

/* a closing producer delivers what it holds within its own message.timeout.ms; the rest is purged and each one told
 * to .kafka.errcb, or at exit, where no q runs, counted on one stderr line: never dropped in silence */
static void kfk_deliver_out(kfk_side* s) {
    kfk_client* c = s->owner;
    char v[32];
    size_t vn = sizeof v;
    int ms = A.conf_get(A.conf(s->rk), "message.timeout.ms", v, &vn) == KFK_CONF_OK ? atoi(v) : KFK_TIMEOUT_MS;
    c->dr_failed = 0;
    A.flush(s->rk, ms);
    A.purge(s->rk, KFK_PURGE_F_QUEUE | KFK_PURGE_F_INFLIGHT);
    A.flush(s->rk, 100);
    ray_t* h = ray_sym_str(c->handle);
    if (g_shutdown && c->dr_failed && h)
        fprintf(stderr, "kafka: %.*s: %lld messages undelivered at exit\n", (int)ray_str_len(h), ray_str_ptr(h),
                (long long)c->dr_failed);
}

static void kfk_side_close(kfk_side* s) {
    ray_poll_t* p = (ray_poll_t*)ray_runtime_get_poll();
    if (s->sel >= 0 && p) ray_poll_deregister(p, s->sel);
    if (s->q) {
        A.queue_io_event_enable(s->q, -1, NULL, 0);
        A.queue_destroy(s->q);
    }
    if (s->rk) {
        if (s->role == KFK_CONSUMER) { kfk_settle(s); A.consumer_close(s->rk); }
        else kfk_deliver_out(s);
        A.destroy(s->rk);
    }
#if !defined(_WIN32)
    for (int i = 0; i < 2; i++) if (s->fd[i] >= 0) ray_sock_close(s->fd[i]);
#endif
    *s = (kfk_side){ .owner = s->owner, .role = s->role, .fd = { -1, -1 }, .sel = -1 };
}

static void kfk_drop(kfk_client* c) {
    if (c->serving) { c->closing = 1; return; }
    c->live = 0;
    for (int r = 0; r < 2; r++) {
        kfk_side_close(&c->side[r]);
        if (c->cfg[r]) ray_release(c->cfg[r]);
    }
    memset(c, 0, sizeof *c);
}

/* the process is leaving: every client leaves its group now, so the broker does not wait a session timeout */
static void kfk_shutdown(void) {
    g_shutdown = 1;
    for (int i = 0; i < KFK_MAX_CLIENTS; i++) if (g_cl[i].live) { g_cl[i].serving = 0; kfk_drop(&g_cl[i]); }
}

/* ---- config ------------------------------------------------------------------------------------------------- */

static ray_t* kfk_conf_put(rd_kafka_conf_t* conf, const char* k, const char* v) {
    char errstr[512];
    return A.conf_set(conf, k, v, errstr, sizeof errstr) == KFK_CONF_OK ? NULL : kfk_err_text(errstr);
}

/* librdkafka's keys verbatim, the value a symbol, string, char, integer or boolean */
static ray_t* kfk_conf_apply(rd_kafka_conf_t* conf, ray_t* cfg) {
    if (!cfg || RAY_IS_NULL(cfg)) return NULL;
    if (cfg->type != RAY_DICT) return q_err(QE_TYPE);
    ray_t* keys = ray_dict_keys(cfg);
    ray_t* vals = ray_dict_vals(cfg);
    int64_t m = q_count(cfg);
    if (m && keys->type != RAY_SYM) return q_err(QE_TYPE);
    for (int64_t i = 0; i < m; i++) {
        char k[KFK_NAME_MAX], v[1024];
        ray_t* ks = ray_sym_str(ray_vec_get_sym_id(keys, i));
        if (!ks || ray_str_len(ks) >= sizeof k) return q_err(QE_DOMAIN);
        memcpy(k, ray_str_ptr(ks), ray_str_len(ks));
        k[ray_str_len(ks)] = '\0';
        ray_t* x = q_index_elem_at(vals, i);
        if (!x || RAY_IS_ERR(x)) return x ? x : q_err(QE_OOM);
        const char* p; int64_t n;
        ray_t* e = NULL;
        if (q_str_subject_bytes(x, &p, &n)) {
            if (n >= (int64_t)sizeof v) e = q_err(QE_LIMIT);
            else { memcpy(v, p, (size_t)n); v[n] = '\0'; }
        } else if (q_type_is_bool(x)) snprintf(v, sizeof v, "%s", x->i64 ? "true" : "false");
        else if (q_type_is_int_atom(x)) snprintf(v, sizeof v, "%lld", (long long)q_type_as_i64(x));
        else e = q_err(QE_TYPE);
        ray_release(x);
        if (!e) e = kfk_conf_put(conf, k, v);
        if (e) return e;
    }
    return NULL;
}

/* a role's conf from its config: logs are served by OUR poll, never a thread; a consumer commits only when told */
static rd_kafka_conf_t* kfk_conf_of(ray_t* cfg, int role, ray_t** err) {
    rd_kafka_conf_t* conf = A.conf_new();
    *err = kfk_conf_apply(conf, cfg);
    if (!*err) *err = kfk_conf_put(conf, "log.queue", "true");
    if (!*err && role == KFK_CONSUMER) *err = kfk_conf_put(conf, "enable.auto.commit", "false");
    if (*err) { A.conf_destroy(conf); return NULL; }
    return conf;
}

/* the alias's client of a role, made on first use */
static ray_t* kfk_side_open(kfk_client* c, int role) {
    kfk_side* s = &c->side[role];
    if (s->rk) return NULL;
    ray_t* e;
    rd_kafka_conf_t* conf = kfk_conf_of(c->cfg[role], role, &e);
    if (!conf) return e;
    A.conf_set_opaque(conf, c);
    A.conf_set_error_cb(conf, kfk_error_cb);
    A.conf_set_stats_cb(conf, kfk_stats_cb);
    A.conf_set_dr_msg_cb(conf, kfk_dr_cb);
    A.conf_set_offset_commit_cb(conf, kfk_commit_cb);
    char errstr[512];
    s->rk = A.new(role, conf, errstr, sizeof errstr);
    if (!s->rk) { A.conf_destroy(conf); return kfk_err_text(errstr); }
    rd_kafka_resp_err_t r = role == KFK_CONSUMER ? A.poll_set_consumer(s->rk) : KFK_ERR_NO_ERROR;
    s->q = r ? NULL : role == KFK_CONSUMER ? A.queue_get_consumer(s->rk) : A.queue_get_main(s->rk);
    if (!s->q) { kfk_side_close(s); return r ? kfk_err(r) : q_err(QE_DOMAIN); }   /* a consumer without group.id */
#if !defined(_WIN32)
    e = kfk_wire(s);
    if (e) kfk_side_close(s);
#endif
    return e;
}

static ray_t* kfk_side_of(ray_t* h, int role, kfk_side** out) {
    kfk_client* c;
    ray_t* e = kfk_client_of(h, &c);
    if (!e) e = kfk_side_open(c, role);
    *out = e ? NULL : &c->side[role];
    return e;
}

/* ---- the natives ---------------------------------------------------------------------------------------------- */

/* [alias; consumer cfg; producer cfg] -> the token.  Both configs are judged now; the clients wait for first use. */
static ray_t* kfk_new_fn(ray_t** args, int64_t n) {
    if (n != 3) return q_err(QE_RANK);
#if defined(_WIN32)
    return q_err(QE_NYI);
#else
    ray_t* e = kfk_load();
    if (e) return e;
    char alias[KFK_NAME_MAX];
    if (!kfk_sym_cstr(args[0], alias, sizeof alias)) return q_err(QE_TYPE);
    for (int role = 0; role < 2; role++) {
        rd_kafka_conf_t* conf = kfk_conf_of(args[role == KFK_CONSUMER ? 1 : 2], role, &e);
        if (!conf) return e;
        A.conf_destroy(conf);
    }
    int slot = 0;
    while (slot < KFK_MAX_CLIENTS && (g_cl[slot].live || g_cl[slot].handle)) slot++;   /* a closing one still owns its slot */
    if (slot == KFK_MAX_CLIENTS) return q_err(QE_LIMIT);
    kfk_client* c = &g_cl[slot];
    memset(c, 0, sizeof *c);
    for (int role = 0; role < 2; role++) {
        c->side[role] = (kfk_side){ .owner = c, .role = role, .fd = { -1, -1 }, .sel = -1 };
        c->cfg[role] = args[role == KFK_CONSUMER ? 1 : 2];
        ray_retain(c->cfg[role]);
    }
    c->live = 1;
    c->max_wake = KFK_MAX_WAKE;
    char hs[KFK_NAME_MAX + 16];
    int m = snprintf(hs, sizeof hs, ":pq:%s:%s", q_handles_pq_kind_name(Q_PQ_KAFKA), alias);
    c->handle = ray_sym_intern_runtime(hs, (size_t)m);
    if (!g_atexit) { g_atexit = 1; atexit(kfk_shutdown); }
    return ray_i32(KFK_TOKEN_MARK | slot);
#endif
}

static ray_t* kfk_ok(void) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }

/* [h] -> ::  (a close from inside a delivery completes when the delivery returns) */
static ray_t* kfk_destroy_fn(ray_t** args, int64_t n) {
    if (n != 1) return q_err(QE_RANK);
    kfk_client* c;
    ray_t* e = kfk_client_of(args[0], &c);
    if (e) return e;
    kfk_drop(c);
    return kfk_ok();
}

/* [h] -> the handle sym, whatever spelling named the client */
static ray_t* kfk_handle_fn(ray_t** args, int64_t n) {
    if (n != 1) return q_err(QE_RANK);
    kfk_client* c;
    ray_t* e = kfk_client_of(args[0], &c);
    return e ? e : ray_sym(c->handle);
}

/* [] -> librdkafka's version text; the one call that loads the library and nothing else */
static ray_t* kfk_version_fn(ray_t** args, int64_t n) {
    (void)args;
    if (n != 1) return q_err(QE_RANK);
    ray_t* e = kfk_load();
    if (e) return e;
    const char* v = A.version_str();
    return ray_charv(v, (int64_t)strlen(v));
}

/* [h; topics] -> ::  the whole subscription list; an empty one unsubscribes */
static ray_t* kfk_subscribe_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    kfk_side* s;
    ray_t* e = kfk_side_of(args[0], KFK_CONSUMER, &s);
    if (e) return e;
    ray_t* ts = args[1];
    int64_t m = ts->type == -RAY_SYM ? 1 : ts->type == RAY_SYM || (ts->type == RAY_LIST && !q_count(ts)) ? q_count(ts) : -1;
    if (m < 0) return q_err(QE_TYPE);
    rd_kafka_topic_partition_list_t* l = A.topic_partition_list_new((int)(m ? m : 1));
    for (int64_t i = 0; i < m; i++) {
        char topic[KFK_NAME_MAX];
        ray_t* nm = ray_sym_str(ts->type == -RAY_SYM ? ts->i64 : ray_vec_get_sym_id(ts, i));
        if (!nm || ray_str_len(nm) >= sizeof topic) { A.topic_partition_list_destroy(l); return q_err(QE_DOMAIN); }
        memcpy(topic, ray_str_ptr(nm), ray_str_len(nm));
        topic[ray_str_len(nm)] = '\0';
        A.topic_partition_list_add(l, topic, KFK_PARTITION_UA);
    }
    rd_kafka_resp_err_t r = m ? A.subscribe(s->rk, l) : A.unsubscribe(s->rk);
    A.topic_partition_list_destroy(l);
    return r ? kfk_err(r) : kfk_ok();
}

static rd_kafka_resp_err_t kfk_produce_one(rd_kafka_t* rk, const char* topic, ray_t* key, ray_t* val, ray_t** err) {
    const char* vp; int64_t vn;
    const char* kp = NULL; int64_t kn = 0;
    *err = NULL;
    if (RAY_IS_NULL(val)) { vp = NULL; vn = 0; }   /* :: publishes a tombstone, Kafka's null value */
    else if (!q_str_text_or_bytes(val, &vp, &vn)) { *err = q_err(QE_TYPE); return 0; }
    if (key && !RAY_IS_NULL(key) && !q_str_subject_bytes(key, &kp, &kn) && !q_str_text_or_bytes(key, &kp, &kn)) {
        *err = q_err(QE_TYPE);
        return 0;
    }
    if (kp) return A.producev(rk, KFK_VTYPE_TOPIC, topic, KFK_VTYPE_PARTITION, KFK_PARTITION_UA,
                              KFK_VTYPE_MSGFLAGS, KFK_MSG_F_COPY, KFK_VTYPE_VALUE, (void*)vp, (size_t)vn,
                              KFK_VTYPE_KEY, (const void*)kp, (size_t)kn, KFK_VTYPE_END);
    return A.producev(rk, KFK_VTYPE_TOPIC, topic, KFK_VTYPE_PARTITION, KFK_PARTITION_UA,
                      KFK_VTYPE_MSGFLAGS, KFK_MSG_F_COPY, KFK_VTYPE_VALUE, (void*)vp, (size_t)vn, KFK_VTYPE_END);
}

/* [h; topic; key; data] -> ::  data one payload (string or bytes) or a list of them; key `::`, one key for all, or
 * one per payload (a symbol vector or a list of the same count) */
static ray_t* kfk_produce_fn(ray_t** args, int64_t n) {
    if (n != 4) return q_err(QE_RANK);
    kfk_side* s;
    ray_t* e = kfk_side_of(args[0], KFK_PRODUCER, &s);
    if (e) return e;
    char topic[KFK_NAME_MAX];
    if (!kfk_sym_cstr(args[1], topic, sizeof topic)) return q_err(QE_TYPE);
    ray_t* key = args[2];
    ray_t* data = args[3];
    int64_t m = data->type == RAY_LIST ? q_count(data) : -1;
    if (m < 0) {
        rd_kafka_resp_err_t r = kfk_produce_one(s->rk, topic, key, data, &e);
        if (!e && r) e = kfk_err(r);
    } else {
        int many = key->type == RAY_LIST || key->type == RAY_SYM;
        if (many && q_count(key) != m) return q_err(QE_LENGTH);
        for (int64_t i = 0; i < m && !e; i++) {
            ray_t* k = many ? q_index_elem_at(key, i) : key;
            if (!k || RAY_IS_ERR(k)) { e = k ? k : q_err(QE_OOM); break; }
            rd_kafka_resp_err_t r = kfk_produce_one(s->rk, topic, k, ray_list_get(data, i), &e);
            if (!e && r) e = kfk_err(r);
            if (many) ray_release(k);
        }
    }
    kfk_poll(s, 0);
    return e ? e : kfk_ok();
}

/* [h; ms] -> ::  every queued message delivered, or 'kafka: Local: Timed out */
static ray_t* kfk_flush_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    kfk_side* s;
    ray_t* e = kfk_side_of(args[0], KFK_PRODUCER, &s);
    if (e) return e;
    int64_t ms;
    if ((e = q_type_i64_or_err(args[1], &ms, ".kafka.flush: ms"))) return e;
    kfk_client* c = s->owner;
    int outer = !c->serving;
    c->serving = 1;
    rd_kafka_resp_err_t r = A.flush(s->rk, (int)ms);
    if (!r && c->dr_failed) r = c->dr_err;
    c->dr_failed = 0;
    if (outer) { c->serving = 0; if (c->closing) kfk_drop(c); }
    return r ? kfk_err(r) : kfk_ok();
}

/* [h; ms] -> the messages delivered by one drain of the consumer that waits up to ms for the first; a producer made
 * so far has its delivery reports served first */
static ray_t* kfk_drain_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    kfk_client* c;
    ray_t* e = kfk_client_of(args[0], &c);
    if (e) return e;
    int64_t ms;
    if ((e = q_type_i64_or_err(args[1], &ms, ".kafka.drain: ms"))) return e;
    if (c->side[KFK_PRODUCER].rk) kfk_serve(&c->side[KFK_PRODUCER], 0);
    return ray_i64(c->live && c->side[KFK_CONSUMER].rk ? kfk_serve(&c->side[KFK_CONSUMER], (int)ms) : 0);
}

/* [h; offsets; async] -> ::  offsets a `topic partition offset` table.  Async is the per-batch commit, its refusals
 * told to .kafka.errcb; sync is .kafka.commit's, and signals naming the partition the broker refused. */
static ray_t* kfk_commit_fn(ray_t** args, int64_t n) {
    if (n != 3) return q_err(QE_RANK);
    kfk_side* s;
    ray_t* e = kfk_side_of(args[0], KFK_CONSUMER, &s);
    if (e) return e;
    if (!q_type_is_bool(args[2])) return q_err(QE_TYPE);
    rd_kafka_topic_partition_list_t* l = kfk_tpl_of(args[1], 1, &e);
    if (e) return e;
    int async = (int)args[2]->i64;
    kfk_refusal r = { 0 };
    if (async) r.err = A.commit(s->rk, l, 1);
    else r = kfk_commit_sync(s->rk, l);
    if (r.err == KFK_ERR_NO_OFFSET) r.err = KFK_ERR_NO_ERROR;
    if (!r.err) kfk_owe(s, l, async);
    A.topic_partition_list_destroy(l);
    if (!r.err) return kfk_ok();
    if (!r.topic[0]) return kfk_err(r.err);
    char b[KFK_NAME_MAX + 300];
    snprintf(b, sizeof b, "commit %s partition %d: %s", r.topic, (int)r.partition, A.err2str(r.err));
    return kfk_err_text(b);
}

/* [h; x] -> ::  x a `topic partition [offset]` table, or `::` to clear the assignment */
static ray_t* kfk_assign_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    kfk_side* s;
    ray_t* e = kfk_side_of(args[0], KFK_CONSUMER, &s);
    if (e) return e;
    int with_offset = args[1]->type == RAY_TABLE && ray_table_get_col(args[1], ray_sym_intern_runtime("offset", 6));
    rd_kafka_topic_partition_list_t* l = RAY_IS_NULL(args[1]) ? NULL : kfk_tpl_of(args[1], with_offset, &e);
    if (e) return e;
    rd_kafka_resp_err_t r = A.assign(s->rk, l);
    if (l) A.topic_partition_list_destroy(l);
    return r ? kfk_err(r) : kfk_ok();
}

/* the assignment, or NULL with *err */
static rd_kafka_topic_partition_list_t* kfk_assigned(rd_kafka_t* rk, ray_t** err) {
    rd_kafka_topic_partition_list_t* l = NULL;
    rd_kafka_resp_err_t r = A.assignment(rk, &l);
    *err = r ? kfk_err(r) : NULL;
    return *err ? NULL : l;
}

/* every listed partition's low and high watermark, marks[0] and marks[1], in one broker question each for the whole
 * list (a question per partition queues behind a subscribed consumer's fetch); a partition's refusal is its err */
static rd_kafka_resp_err_t kfk_marks(rd_kafka_t* rk, const rd_kafka_topic_partition_list_t* l, int timeout_ms,
                                     rd_kafka_topic_partition_list_t* marks[2]) {
    rd_kafka_resp_err_t r = KFK_ERR_NO_ERROR;
    for (int k = 0; k < 2; k++) {
        marks[k] = A.topic_partition_list_new(l->cnt ? l->cnt : 1);
        for (int i = 0; i < l->cnt; i++)
            A.topic_partition_list_add(marks[k], l->elems[i].topic, l->elems[i].partition)->offset =
                k ? KFK_OFFSET_END : KFK_OFFSET_BEGINNING;
        if (!r && l->cnt) r = A.offsets_for_times(rk, marks[k], timeout_ms);
    }
    return r;
}

static rd_kafka_resp_err_t kfk_mark_err(rd_kafka_topic_partition_list_t* marks[2], int i) {
    return marks[0]->elems[i].err ? marks[0]->elems[i].err : marks[1]->elems[i].err;
}

static void kfk_marks_destroy(rd_kafka_topic_partition_list_t* marks[2]) {
    for (int k = 0; k < 2; k++) A.topic_partition_list_destroy(marks[k]);
}

static int kfk_tpl_has_topic(const rd_kafka_topic_partition_list_t* l, const char* topic) {
    for (int i = 0; i < l->cnt; i++) if (strcmp(l->elems[i].topic, topic) == 0) return 1;
    return 0;
}

/* [h; held] -> ::  every assigned partition of the topics in a `topic partition offset` table paused, the listed
 * partitions rewound to their offsets */
static ray_t* kfk_hold_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    kfk_side* s;
    ray_t* e = kfk_side_of(args[0], KFK_CONSUMER, &s);
    if (e) return e;
    rd_kafka_topic_partition_list_t* held = kfk_tpl_of(args[1], 1, &e);
    if (e) return e;
    rd_kafka_topic_partition_list_t* all = kfk_assigned(s->rk, &e);
    if (e) { A.topic_partition_list_destroy(held); return e; }
    rd_kafka_topic_partition_list_t* pick = A.topic_partition_list_new(all->cnt ? all->cnt : 1);
    for (int i = 0; i < all->cnt; i++)
        if (kfk_tpl_has_topic(held, all->elems[i].topic))
            A.topic_partition_list_add(pick, all->elems[i].topic, all->elems[i].partition);
    rd_kafka_resp_err_t r = A.pause_partitions(s->rk, pick);
    rd_kafka_error_t* se = r ? NULL : A.seek_partitions(s->rk, held, KFK_TIMEOUT_MS);
    A.topic_partition_list_destroy(pick);
    A.topic_partition_list_destroy(all);
    A.topic_partition_list_destroy(held);
    return r ? kfk_err(r) : se ? kfk_err_obj(se) : kfk_ok();
}

/* [h; topic] -> ::  every assigned partition of the topic (`::` = every assigned partition) fetching again */
static ray_t* kfk_resume_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    kfk_side* s;
    ray_t* e = kfk_side_of(args[0], KFK_CONSUMER, &s);
    if (e) return e;
    char topic[KFK_NAME_MAX];
    int one = kfk_sym_cstr(args[1], topic, sizeof topic);
    if (!one && !RAY_IS_NULL(args[1])) return q_err(QE_TYPE);
    rd_kafka_topic_partition_list_t* l = kfk_assigned(s->rk, &e);
    if (e) return e;
    rd_kafka_topic_partition_list_t* pick = A.topic_partition_list_new(l->cnt ? l->cnt : 1);
    for (int i = 0; i < l->cnt; i++)
        if (!one || strcmp(l->elems[i].topic, topic) == 0)
            A.topic_partition_list_add(pick, l->elems[i].topic, l->elems[i].partition);
    rd_kafka_resp_err_t r = A.resume_partitions(s->rk, pick);
    A.topic_partition_list_destroy(pick);
    A.topic_partition_list_destroy(l);
    return r ? kfk_err(r) : kfk_ok();
}

/* [h] -> `topic partition committed position low high lag` over the assignment */
static ray_t* kfk_offsets_fn(ray_t** args, int64_t n) {
    if (n != 1) return q_err(QE_RANK);
    kfk_side* s;
    ray_t* e = kfk_side_of(args[0], KFK_CONSUMER, &s);
    if (e) return e;
    rd_kafka_topic_partition_list_t* l = kfk_assigned(s->rk, &e);
    if (e) return e;
    rd_kafka_topic_partition_list_t* pos = A.topic_partition_list_new(l->cnt ? l->cnt : 1);
    for (int i = 0; i < l->cnt; i++) A.topic_partition_list_add(pos, l->elems[i].topic, l->elems[i].partition);
    rd_kafka_resp_err_t r = A.committed(s->rk, l, KFK_TIMEOUT_MS);
    if (!r) r = A.position(s->rk, pos);
    rd_kafka_topic_partition_list_t* marks[2];
    if (!r) r = kfk_marks(s->rk, l, KFK_TIMEOUT_MS, marks);
    else marks[0] = marks[1] = NULL;
    int64_t cap = l->cnt ? l->cnt : 1;
    ray_t* cols[7] = { ray_sym_vec_new(RAY_SYM_W64, cap), ray_vec_new(RAY_I32, cap), ray_vec_new(RAY_I64, cap),
                       ray_vec_new(RAY_I64, cap), ray_vec_new(RAY_I64, cap), ray_vec_new(RAY_I64, cap),
                       ray_vec_new(RAY_I64, cap) };
    for (int i = 0; i < l->cnt && !r; i++) {
        if ((r = kfk_mark_err(marks, i))) break;
        int64_t lo = marks[0]->elems[i].offset, hi = marks[1]->elems[i].offset;
        int64_t topic = ray_sym_intern_runtime(l->elems[i].topic, strlen(l->elems[i].topic));
        int64_t committed = l->elems[i].offset < 0 ? NULL_I64 : l->elems[i].offset;
        /* the next offset this alias reads: nothing fetched yet (a caught-up restart) leaves librdkafka's unset */
        int64_t position = pos->elems[i].offset >= 0 ? pos->elems[i].offset : committed != NULL_I64 ? committed : lo;
        int64_t lag = hi == NULL_I64 || position == NULL_I64 ? NULL_I64 : hi - position;
        cols[0] = kfk_vec_push(cols[0], &topic);
        cols[1] = kfk_vec_push(cols[1], &l->elems[i].partition);
        cols[2] = kfk_vec_push(cols[2], &committed);
        cols[3] = kfk_vec_push(cols[3], &position);
        cols[4] = kfk_vec_push(cols[4], &lo);
        cols[5] = kfk_vec_push(cols[5], &hi);
        cols[6] = kfk_vec_push(cols[6], &lag);
    }
    if (marks[0]) kfk_marks_destroy(marks);
    A.topic_partition_list_destroy(pos);
    A.topic_partition_list_destroy(l);
    static const char* const names[7] = { "topic", "partition", "committed", "position", "low", "high", "lag" };
    ray_t* t = kfk_table(names, cols, 7);
    if (r) { if (!RAY_IS_ERR(t)) ray_release(t); return kfk_err(r); }
    return t;
}

/* a question for the broker goes through a client the alias already made, else its producer (which needs no group) */
static ray_t* kfk_any_rk(ray_t* h, rd_kafka_t** out) {
    kfk_client* c;
    ray_t* e = kfk_client_of(h, &c);
    if (e) return e;
    int role = c->side[KFK_CONSUMER].rk && !c->side[KFK_PRODUCER].rk ? KFK_CONSUMER : KFK_PRODUCER;
    if ((e = kfk_side_open(c, role))) return e;
    *out = c->side[role].rk;
    return NULL;
}

/* [h] -> `topic partition leader replicas low high` for every topic the broker reports */
static ray_t* kfk_metadata_fn(ray_t** args, int64_t n) {
    if (n != 1) return q_err(QE_RANK);
    rd_kafka_t* rk;
    ray_t* e = kfk_any_rk(args[0], &rk);
    if (e) return e;
    const rd_kafka_metadata_t* md = NULL;
    rd_kafka_resp_err_t r = A.metadata(rk, 1, NULL, &md, KFK_TIMEOUT_MS);
    if (r) return kfk_err(r);
    rd_kafka_topic_partition_list_t* l = A.topic_partition_list_new(16);
    for (int t = 0; t < md->topic_cnt; t++)
        for (int p = 0; p < md->topics[t].partition_cnt; p++)
            A.topic_partition_list_add(l, md->topics[t].topic, md->topics[t].partitions[p].id);
    rd_kafka_topic_partition_list_t* marks[2];
    r = kfk_marks(rk, l, KFK_TIMEOUT_MS, marks);
    ray_t* cols[6] = { ray_sym_vec_new(RAY_SYM_W64, 16), ray_vec_new(RAY_I32, 16), ray_vec_new(RAY_I32, 16),
                       ray_list_new(16), ray_vec_new(RAY_I64, 16), ray_vec_new(RAY_I64, 16) };
    for (int t = 0, at = 0; t < md->topic_cnt && !r; t++) {
        const rd_kafka_metadata_topic_t* mt = &md->topics[t];
        int64_t topic = ray_sym_intern_runtime(mt->topic, strlen(mt->topic));
        for (int p = 0; p < mt->partition_cnt && !r; p++, at++) {
            const rd_kafka_metadata_partition_t* mp = &mt->partitions[p];
            rd_kafka_resp_err_t pe = kfk_mark_err(marks, at);
            if (pe == KFK_ERR_UNKNOWN_TOPIC_OR_PART) continue;   /* deleted since the listing: not a topic now */
            if ((r = pe)) break;
            int64_t lo = marks[0]->elems[at].offset, hi = marks[1]->elems[at].offset;
            cols[0] = kfk_vec_push(cols[0], &topic);
            cols[1] = kfk_vec_push(cols[1], &mp->id);
            cols[2] = kfk_vec_push(cols[2], &mp->leader);
            cols[3] = kfk_list_push(cols[3], mp->replica_cnt ? ray_vec_from_raw(RAY_I32, mp->replicas, mp->replica_cnt)
                                                              : ray_vec_new(RAY_I32, 1));
            cols[4] = kfk_vec_push(cols[4], &lo);
            cols[5] = kfk_vec_push(cols[5], &hi);
        }
    }
    kfk_marks_destroy(marks);
    A.topic_partition_list_destroy(l);
    A.metadata_destroy(md);
    static const char* const names[6] = { "topic", "partition", "leader", "replicas", "low", "high" };
    ray_t* t = kfk_table(names, cols, 6);
    if (r) { if (!RAY_IS_ERR(t)) ray_release(t); return kfk_err(r); }
    return t;
}

/* [h; n] -> ::  the messages one wake drains at most */
static ray_t* kfk_maxwake_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    kfk_client* c;
    ray_t* e = kfk_client_of(args[0], &c);
    if (e) return e;
    int64_t m;
    if ((e = q_type_i64_or_err(args[1], &m, ".kafka.sub: max_per_wake"))) return e;
    if (m < 1) return q_err(QE_DOMAIN);
    c->max_wake = m;
    return kfk_ok();
}

/* the topic's partition ids from the broker's metadata for that one topic, malloc'd into *ids; NULL on success */
static ray_t* kfk_partitions_of(rd_kafka_t* rk, const char* topic, int32_t** ids, int* cnt) {
    rd_kafka_topic_t* rkt = A.topic_new(rk, topic, NULL);
    if (!rkt) return kfk_err(KFK_ERR_UNKNOWN_TOPIC);
    const rd_kafka_metadata_t* md = NULL;
    rd_kafka_resp_err_t r = A.metadata(rk, 0, rkt, &md, KFK_TIMEOUT_MS);
    A.topic_destroy(rkt);
    if (r) return kfk_err(r);
    if (md->topic_cnt < 1 || md->topics[0].err) {
        r = md->topic_cnt < 1 ? KFK_ERR_UNKNOWN_TOPIC : md->topics[0].err;
        A.metadata_destroy(md);
        return kfk_err(r);
    }
    *cnt = md->topics[0].partition_cnt;
    *ids = malloc(sizeof(int32_t) * (size_t)(*cnt ? *cnt : 1));
    for (int i = 0; i < *cnt; i++) (*ids)[i] = md->topics[0].partitions[i].id;
    A.metadata_destroy(md);
    return NULL;
}

/* [h; topic] -> `partition low high` */
static ray_t* kfk_watermarks_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    rd_kafka_t* rk;
    ray_t* e = kfk_any_rk(args[0], &rk);
    if (e) return e;
    char topic[KFK_NAME_MAX];
    if (!kfk_sym_cstr(args[1], topic, sizeof topic)) return q_err(QE_TYPE);
    int32_t* ids; int cnt;
    if ((e = kfk_partitions_of(rk, topic, &ids, &cnt))) return e;
    int64_t cap = cnt ? cnt : 1;
    rd_kafka_topic_partition_list_t* l = A.topic_partition_list_new((int)cap);
    for (int i = 0; i < cnt; i++) A.topic_partition_list_add(l, topic, ids[i]);
    rd_kafka_topic_partition_list_t* marks[2];
    rd_kafka_resp_err_t r = kfk_marks(rk, l, KFK_TIMEOUT_MS, marks);
    ray_t* cols[3] = { ray_vec_new(RAY_I32, cap), ray_vec_new(RAY_I64, cap), ray_vec_new(RAY_I64, cap) };
    for (int i = 0; i < cnt && !r; i++) {
        if ((r = kfk_mark_err(marks, i))) break;
        cols[0] = kfk_vec_push(cols[0], &ids[i]);
        cols[1] = kfk_vec_push(cols[1], &marks[0]->elems[i].offset);
        cols[2] = kfk_vec_push(cols[2], &marks[1]->elems[i].offset);
    }
    kfk_marks_destroy(marks);
    A.topic_partition_list_destroy(l);
    free(ids);
    static const char* const names[3] = { "partition", "low", "high" };
    ray_t* t = kfk_table(names, cols, 3);
    if (r) { if (!RAY_IS_ERR(t)) ray_release(t); return kfk_err(r); }
    return t;
}

/* a throwaway consumer on the alias's consumer config: its own group, nothing committed, an EOF event per partition */
static rd_kafka_t* kfk_reader_new(kfk_client* c, ray_t** err) {
    rd_kafka_conf_t* conf = kfk_conf_of(c->cfg[KFK_CONSUMER], KFK_CONSUMER, err);
    if (!conf) return NULL;
    char gid[64];
    snprintf(gid, sizeof gid, "peachq-read-%ld-%d", (long)getpid(), ++g_reads);
    *err = kfk_conf_put(conf, "group.id", gid);
    if (!*err) *err = kfk_conf_put(conf, "enable.partition.eof", "true");
    if (*err) { A.conf_destroy(conf); return NULL; }
    char errstr[512];
    rd_kafka_t* rk = A.new(KFK_CONSUMER, conf, errstr, sizeof errstr);
    if (!rk) { A.conf_destroy(conf); *err = kfk_err_text(errstr); return NULL; }
    A.poll_set_consumer(rk);
    return rk;
}

typedef struct { int64_t lo, hi, timeout; int ts; } kfk_range;   /* lo < 0 = a tail count; ts: lo is ms since 1970 */

/* the partition list a bounded read assigns, start offsets from the watermarks (or the timestamp), with the
 * exclusive end per element in *ends; an empty partition is left out */
static rd_kafka_topic_partition_list_t* kfk_read_plan(rd_kafka_t* rk, const char* topic, const int32_t* ids, int cnt,
                                                      const kfk_range* r, int64_t** ends, ray_t** err) {
    rd_kafka_topic_partition_list_t* l = A.topic_partition_list_new(cnt ? cnt : 1);
    *ends = malloc(sizeof(int64_t) * (size_t)(cnt ? cnt : 1));
    *err = NULL;
    int n = 0;
    rd_kafka_topic_partition_list_t* asked = A.topic_partition_list_new(cnt ? cnt : 1);
    for (int i = 0; i < cnt; i++) A.topic_partition_list_add(asked, topic, ids[i]);
    rd_kafka_topic_partition_list_t* marks[2];
    rd_kafka_resp_err_t me = kfk_marks(rk, asked, (int)r->timeout, marks);
    if (me) *err = kfk_err(me);
    for (int i = 0; i < cnt && !*err; i++) {
        rd_kafka_resp_err_t e = kfk_mark_err(marks, i);
        if (e) { *err = kfk_err(e); break; }
        int64_t lo = marks[0]->elems[i].offset, hi = marks[1]->elems[i].offset;
        int64_t start = r->ts ? lo : r->lo < 0 ? (hi + r->lo > lo ? hi + r->lo : lo) : (r->lo > lo ? r->lo : lo);
        int64_t end = r->hi < 0 || r->hi > hi ? hi : r->hi;
        if (start >= end) continue;
        A.topic_partition_list_add(l, topic, ids[i])->offset = r->ts ? r->lo : start;
        (*ends)[n++] = end;
    }
    kfk_marks_destroy(marks);
    A.topic_partition_list_destroy(asked);
    if (!*err && r->ts && l->cnt) {
        rd_kafka_resp_err_t e = A.offsets_for_times(rk, l, (int)r->timeout);
        if (e) *err = kfk_err(e);
        for (int i = 0; !*err && i < l->cnt; i++)
            if (l->elems[i].offset < 0) l->elems[i].offset = (*ends)[i];   /* nothing at or after the time */
    }
    if (*err) { A.topic_partition_list_destroy(l); free(*ends); *ends = NULL; return NULL; }
    return l;
}

/* [h; topic; partitions; lo; hi; timeout] -> the batch table of every message in [lo; hi) of the partitions (`::` =
 * all): lo a long (negative = the last -lo per partition) or a timestamp, hi a long or `::` = the high-water mark at
 * the call; a partition that goes quiet for timeout ms before its end is 'kafka: Local: Timed out, never a partial */
static ray_t* kfk_read_fn(ray_t** args, int64_t n) {
    if (n != 6) return q_err(QE_RANK);
    kfk_client* c;
    ray_t* e = kfk_client_of(args[0], &c);
    if (e) return e;
    char topic[KFK_NAME_MAX];
    if (!kfk_sym_cstr(args[1], topic, sizeof topic)) return q_err(QE_TYPE);
    kfk_range r = { 0, -1, 0, 0 };
    if (args[3]->type == -RAY_TIMESTAMP) { r.ts = 1; r.lo = (args[3]->i64 + KFK_EPOCH_NS) / 1000000LL; }
    else if ((e = q_type_i64_or_err(args[3], &r.lo, ".kafka: offset"))) return e;
    if (!RAY_IS_NULL(args[4]) && (e = q_type_i64_or_err(args[4], &r.hi, ".kafka: offset"))) return e;
    if ((e = q_type_i64_or_err(args[5], &r.timeout, ".kafka: timeout"))) return e;
    ray_t* want = args[2];
    if (!RAY_IS_NULL(want) && !q_type_is_int_atom(want) && !q_type_is_int_vec(want)) return q_err(QE_TYPE);
    rd_kafka_t* rk = kfk_reader_new(c, &e);
    if (e) return e;
    int32_t* ids = NULL; int cnt = 0;
    if (RAY_IS_NULL(want)) e = kfk_partitions_of(rk, topic, &ids, &cnt);
    else {                                          /* the asked-for partitions as given: an unknown one is the broker's refusal */
        cnt = q_type_is_int_atom(want) ? 1 : (int)q_count(want);
        ids = malloc(sizeof(int32_t) * (size_t)(cnt ? cnt : 1));
        for (int i = 0; i < cnt; i++) ids[i] = (int32_t)(q_type_is_int_atom(want) ? q_type_as_i64(want) : q_type_ivec_get(want, i));
    }
    int64_t* ends = NULL;
    rd_kafka_topic_partition_list_t* l = e ? NULL : kfk_read_plan(rk, topic, ids, cnt, &r, &ends, &e);
    free(ids);
    kfk_batch b;
    kfk_batch_init(&b);
    rd_kafka_resp_err_t err = 0;
    if (!e && l->cnt && (err = A.assign(rk, l))) e = kfk_err(err);
    int left = e ? 0 : l->cnt;
    char* done = calloc((size_t)(left ? left : 1), 1);
    while (left > 0) {
        rd_kafka_message_t* m = A.consumer_poll(rk, (int)r.timeout);
        if (!m) { e = kfk_err(KFK_ERR_TIMED_OUT); break; }
        int i = 0;
        while (i < l->cnt && l->elems[i].partition != m->partition) i++;
        int fin = m->err == KFK_ERR_PARTITION_EOF || (!m->err && m->offset >= ends[i] - 1);
        if (m->err && m->err != KFK_ERR_PARTITION_EOF) e = kfk_err(m->err);
        else if (!m->err && m->offset < ends[i] && !done[i]) kfk_batch_add(&b, m);
        if (fin && !done[i]) { done[i] = 1; left--; }
        A.message_destroy(m);
        if (e) break;
    }
    free(done);
    free(ends);
    if (l) A.topic_partition_list_destroy(l);
    A.consumer_close(rk);
    A.destroy(rk);
    ray_t* t = kfk_batch_table(&b);
    if (e) { if (t && !RAY_IS_ERR(t)) ray_release(t); return e; }
    return t;
}

/* [f] -> a function's rank as the apply module counts it (a projection's is its holes); not a function is 'rank */
static ray_t* kfk_rank_fn(ray_t** args, int64_t n) {
    if (n != 1 || !q_eval_apply_is_fn(args[0])) return q_err(QE_RANK);
    return ray_i64(q_eval_apply_rank(args[0]));
}

static void kfk_bind(const char* name, int rank, ray_vary_fn fn) {
    ray_t* obj = ray_fn_vary(name, RAY_FN_NONE, fn);
    q_env_bind_native(name, obj, rank);
    ray_release(obj);
}

void q_kafka_register(void) {
    kfk_bind(".kafka.i.new",       3, kfk_new_fn);
    kfk_bind(".kafka.i.destroy",   1, kfk_destroy_fn);
    kfk_bind(".kafka.i.handle",    1, kfk_handle_fn);
    kfk_bind(".kafka.i.version",   1, kfk_version_fn);
    kfk_bind(".kafka.i.subscribe", 2, kfk_subscribe_fn);
    kfk_bind(".kafka.i.produce",   4, kfk_produce_fn);
    kfk_bind(".kafka.i.flush",     2, kfk_flush_fn);
    kfk_bind(".kafka.i.drain",     2, kfk_drain_fn);
    kfk_bind(".kafka.i.commit",    3, kfk_commit_fn);
    kfk_bind(".kafka.i.assign",    2, kfk_assign_fn);
    kfk_bind(".kafka.i.hold",      2, kfk_hold_fn);
    kfk_bind(".kafka.i.resume",    2, kfk_resume_fn);
    kfk_bind(".kafka.i.offsets",   1, kfk_offsets_fn);
    kfk_bind(".kafka.i.metadata",  1, kfk_metadata_fn);
    kfk_bind(".kafka.i.max_wake",  2, kfk_maxwake_fn);
    kfk_bind(".kafka.i.watermarks", 2, kfk_watermarks_fn);
    kfk_bind(".kafka.i.read",      6, kfk_read_fn);
    kfk_bind(".kafka.i.rank",      1, kfk_rank_fn);
}
