/* q_comment — see q_comment.h. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/q_comment.h"
#include "qlang/q_env.h"        /* q_env_get / _ctx / _fullname / _frame_depth */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_value — the one apply seam */
#include <string.h>

#define COMMENT_ACC_MAX 4096

static char    g_acc[COMMENT_ACC_MAX];   /* the comment run being read */
static size_t  g_alen;
static ray_t*  g_pending;            /* armed header (owned): the next definition claims it */
static int32_t g_pending_depth;      /* the frame depth the statement itself binds at */
static int64_t g_run_line;           /* first physical line of the run being read */
static ray_t*  g_claimed;            /* `.help.add` arguments awaiting the fire point (owned) */
static int64_t g_file;
static int64_t g_stmt_line;          /* THE one line capture: the statement being
                                      * evaluated names both the doc-store row
                                      * and the lambda's `l` */
static int     g_seen_code;
static int     g_file_run;           /* the run being read is the file's leading one */
static int     g_file_run_done;
static int     g_firing;

/* Interned per call, never cached: sym ids do not survive a runtime teardown,
 * and the test driver builds a fresh runtime per suite. */
static int64_t comment_add_sym(void) { return ray_sym_intern_runtime(".help.add", 9); }

static void comment_claim(int64_t fullname, int64_t ns, ray_t* header, int64_t line) {
    ray_t* rec = ray_list_new(5);
    if (!rec) return;
    ray_t* parts[4] = { ray_sym(fullname), ray_sym(ns), ray_sym(g_file), ray_i64(line) };
    for (int i = 0; i < 4; i++) {
        if (parts[i]) { rec = ray_list_append(rec, parts[i]); ray_release(parts[i]); }
    }
    rec = ray_list_append(rec, header);
    if (!g_claimed) g_claimed = ray_list_new(2);
    if (g_claimed) g_claimed = ray_list_append(g_claimed, rec);
    ray_release(rec);
}

/* The leading comment run of a file documents the FILE — not a value, and so
 * never expressible as a property of one. */
static void comment_claim_file(size_t n, int64_t line) {
    int64_t ctx = q_env_ctx();
    ray_t*  hdr = ray_charv(g_acc, (int64_t)n);
    g_file_run = 0;
    g_file_run_done = 1;
    if (!hdr) return;
    comment_claim(0, ctx ? ctx : ray_sym_intern_runtime(".", 1), hdr, line);
    ray_release(hdr);
}

static void comment_drop_pending(void) {
    if (g_pending) { ray_release(g_pending); g_pending = NULL; }
}

q_comment_script_t q_comment_script_begin(int64_t file_sym) {
    q_comment_script_t saved = { g_file, g_stmt_line, g_seen_code,
                                 g_file_run_done };
    g_alen = 0;
    comment_drop_pending();
    g_file = file_sym;
    g_stmt_line = 0;
    g_seen_code = 0;
    g_file_run = 0;
    g_file_run_done = 0;
    return saved;
}

void q_comment_script_end(q_comment_script_t saved) {
    q_comment_stmt_end();          /* a file of comments alone still records itself */
    g_alen = 0;
    comment_drop_pending();
    g_file = saved.file;
    g_stmt_line = saved.stmt_line;
    g_seen_code = saved.seen_code;
    g_file_run = 0;
    g_file_run_done = saved.file_run_done;
}

void q_comment_line(const char* p, size_t n, int64_t line) {
    if (!g_alen) {
        g_run_line = line;
        if (!g_seen_code && !g_file_run_done) g_file_run = 1;
    }
    if (g_alen && g_alen < sizeof g_acc) g_acc[g_alen++] = '\n';
    size_t room = g_alen < sizeof g_acc ? sizeof g_acc - g_alen : 0;
    if (n > room) n = room;
    memcpy(g_acc + g_alen, p, n);
    g_alen += n;
}

void q_comment_break(void) {
    if (g_file_run && g_alen && q_env_get(comment_add_sym())) comment_claim_file(g_alen, g_run_line);
    g_file_run = 0;
    g_alen = 0;
}

void q_comment_fresh_line(int64_t line) {
    size_t n = g_alen;
    g_alen = 0;
    g_seen_code = 1;
    g_stmt_line = line;
    comment_drop_pending();
    /* A leading run ending on CODE falls through to that definition; only a
     * BREAK-terminated leading run documents the file (q_comment_break). */
    if (g_file_run) { g_file_run = 0; g_file_run_done = 1; }
    /* THE gate: `.help.add` unbound (no `\l pq`, or the always-on core
     * bootstrap, which runs before it) captures nothing at all. */
    if (!n || !q_env_get(comment_add_sym())) return;
    ray_t* hdr = ray_charv(g_acc, (int64_t)n);
    if (!hdr) return;
    g_pending = hdr;
    g_pending_depth = q_env_frame_depth();
}

/* ref/value.md `n`: the fully qualified name spells `ns , "." , name` with the
 * root namespace as "." — a root-context `f` is "..f", the doc's own worked
 * example.  Any dotted fullname already carries its namespace verbatim. */
static void comment_stamp_lambda(int64_t sym, ray_t* val) {
    if (!val || q_eval_apply_carrier_kind(val) != Q_EVAL_CAR_LAMBDA) return;
    int64_t full = q_env_fullname(sym, NULL);
    ray_t* s = ray_sym_str(full);                        /* borrowed */
    if (!s) return;
    const char* p = ray_str_ptr(s);
    int64_t n = ray_str_len(s);
    int64_t line = g_file ? g_stmt_line : -1;
    if (n <= 0) return;
    if (p[0] == '.') {
        q_eval_apply_lambda_name(val, p, n, g_file, line);
        return;
    }
    char* buf = (char*)ray_alloc_raw((size_t)n + 2);
    if (!buf) return;
    buf[0] = buf[1] = '.';
    memcpy(buf + 2, p, (size_t)n);
    q_eval_apply_lambda_name(val, buf, n + 2, g_file, line);
    ray_free_raw(buf);
}

void q_comment_on_global_set(int64_t sym, ray_t* val) {
    comment_stamp_lambda(sym, val);
    if (!g_pending) return;
    /* The STATEMENT's own binding, not a global a lambda wrote on the way to
     * it: `f:helper[]` where helper does `b::1` runs that write one frame
     * deeper, and the header belongs to f. */
    if (q_env_frame_depth() != g_pending_depth) return;
    ray_t* hdr = g_pending;                     /* one-shot: the FIRST set wins */
    g_pending = NULL;
    int64_t ns, full = q_env_fullname(sym, &ns);   /* the tree names its own writes */
    comment_claim(full, ns, hdr, g_stmt_line);
    ray_release(hdr);
}

void q_comment_stmt_end(void) {
    comment_drop_pending();
    if (!g_claimed || g_firing) return;
    ray_t* recs = g_claimed;
    g_claimed = NULL;
    ray_t* fn = q_env_get(comment_add_sym());       /* borrowed */
    if (fn) {
        ray_retain(fn);
        g_firing = 1;
        ray_t** rec = (ray_t**)ray_data(recs);
        for (int64_t i = 0; i < ray_len(recs); i++) {
            ray_t* r = q_eval_apply_value(fn, &rec[i], 1);
            /* A doc-store failure is never a load failure: the record is
             * dropped and the script carries on. */
            if (r) { if (RAY_IS_ERR(r)) ray_error_free(r); else ray_release(r); }
        }
        g_firing = 0;
        ray_release(fn);
    }
    ray_release(recs);
}
