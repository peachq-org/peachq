/* q_dbg — see q_dbg.h.  The suspended state IS the C stack: the nested
 * read-eval loop runs where the error was produced, so the failing lambda's
 * q_env locals are live and `:r` feeds the walker in place.  Caret columns
 * need parser token offsets the trees don't carry yet — lines print bare. */
#include "qlang/eval/q_dbg.h"
#include "qlang/eval/q_eval.h"
#include "qlang/base/q_err.h"
#include "qlang/parse/q_parse.h"
#include "qlang/q_fmt.h"
#include "qlang/q_console.h"
#include "qlang/q_env.h"
#include "qlang/ops/q_sys.h"
#include "qlang/ops/q_index.h"
#include <string.h>

#define DBG_LIVE_MAX 256
#define DBG_SNAP_MAX 64
#define DBG_STMT_MAX 4096
#define DBG_LINE_MAX 4096
#define DBG_EY_MAX   8

static _Thread_local ray_t* g_live[DBG_LIVE_MAX];  /* borrowed lambda values */
static _Thread_local int32_t g_live_env[DBG_LIVE_MAX];  /* their q_env frames */
static _Thread_local int    g_live_depth;
static _Thread_local int    g_cursor;              /* navigated frame, 0 = [0] */

static char g_stmt[DBG_STMT_MAX];
static int  g_console_stmt;
static int  g_trap_depth;
static int  g_dbg_depth;

static q_dbg_read_fn g_reader;

typedef struct {
    ray_t* err;                    /* identity only — never dereferenced */
    ray_t* lam[DBG_SNAP_MAX];      /* retained, outermost first */
    int    depth;
    char   stmt[DBG_STMT_MAX];
    ray_t* ex;                     /* retained failed-primitive value */
    ray_t* ey[DBG_EY_MAX];         /* retained args of the failed apply */
    int    eyn;
    int    reported;
} dbg_snap_t;

static dbg_snap_t g_snap;

void q_dbg_frame_push(ray_t* lam) {
    if (g_live_depth < DBG_LIVE_MAX) {
        g_live[g_live_depth] = lam;
        g_live_env[g_live_depth] = q_env_frame_depth() - 1;   /* its own frame */
    }
    g_live_depth++;
}

/* frame i, 1-based; past the stored window the deepest stored lambda stands in */
static int live_clamp(int i) { return i > DBG_LIVE_MAX ? DBG_LIVE_MAX : i; }
static ray_t* live_lam(int i) { return g_live[live_clamp(i) - 1]; }

ray_t* q_dbg_self(void) {
    if (g_live_depth <= 0) return NULL;
    ray_t* lam = live_lam(g_live_depth);
    ray_retain(lam);
    return lam;
}

void q_dbg_frame_pop(void) { g_live_depth--; }

void q_dbg_snapshot_clear(void) {
    for (int i = 0; i < g_snap.depth; i++) ray_release(g_snap.lam[i]);
    if (g_snap.ex) ray_release(g_snap.ex);
    for (int i = 0; i < g_snap.eyn; i++) ray_release(g_snap.ey[i]);
    memset(&g_snap, 0, sizeof g_snap);
}

int q_dbg_statement_begin(const char* s, size_t n, int console) {
    q_dbg_snapshot_clear();
    if (s) {
        if (n >= sizeof g_stmt) n = sizeof g_stmt - 1;
        memcpy(g_stmt, s, n);
        g_stmt[n] = '\0';
    } else {
        g_stmt[0] = '\0';
    }
    int prev = g_console_stmt;
    g_console_stmt = console;
    return prev;
}

void q_dbg_statement_end(int prev_console) { g_console_stmt = prev_console; }

void q_dbg_trap_enter(void) { g_trap_depth++; }
void q_dbg_trap_exit(void)  { g_trap_depth--; }

void q_dbg_set_reader(q_dbg_read_fn fn) { g_reader = fn; }

void q_dbg_reset(void) {
    q_dbg_snapshot_clear();
    g_live_depth = g_cursor = 0;
    g_stmt[0] = '\0';
    g_console_stmt = g_trap_depth = g_dbg_depth = 0;
    g_reader = NULL;
}

/* ---- frame lines ---- */

/* the `g:` prefix — a display-time scan of the definition namespace's
 * functions recovers the global a frame's lambda is bound to */
static int lam_global_name(ray_t* lam, char* buf, size_t cap) {
    ray_t* ctx = NULL;
    q_eval_apply_lambda_parts(lam, NULL, NULL, &ctx);
    int64_t ns = ctx ? ctx->i64 : 0;
    ray_t* names = q_env_ns_names(ns, Q_ENV_NS_FNS);
    if (!names || RAY_IS_ERR(names)) {
        if (names) ray_release(names);
        return 0;
    }
    int found = 0;
    int64_t cnt = ray_len(names);
    for (int64_t i = 0; i < cnt && !found; i++) {
        ray_t* m = q_index_elem_at(names, i);
        if (!m || RAY_IS_ERR(m)) { if (m) ray_release(m); continue; }
        int64_t q = q_env_qualify(ns, m->i64);
        if (q >= 0 && q_env_get(q) == lam) {
            ray_t* s = ray_sym_str(m->i64);
            if (s && !RAY_IS_ERR(s)) {
                size_t sn = ray_str_len(s);
                if (sn + 1 < cap) {
                    memcpy(buf, ray_str_ptr(s), sn);
                    buf[sn] = '\0';
                    found = 1;
                }
                ray_release(s);
            }
        }
        ray_release(m);
    }
    ray_release(names);
    return found;
}

/* one `  [i]  name:src` line; lam NULL = the statement frame (from stmt) */
static size_t frame_line(char* dst, size_t cap, int idx, int mark, ray_t* lam,
                         const char* stmt) {
    const char* src = stmt;
    size_t sn;
    char name[128];
    name[0] = '\0';
    if (lam) {
        ray_t* s = q_eval_apply_lambda_src(lam);
        src = s ? ray_str_ptr(s) : "{}";
        sn = s ? ray_str_len(s) : 2;
        if (lam_global_name(lam, name, sizeof name - 1)) strcat(name, ":");
    } else {
        sn = strlen(src);
    }
    int n = snprintf(dst, cap, "%s[%d]  %s%.*s\n", mark ? ">>" : "  ", idx,
                     name, (int)sn, src);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : (cap ? cap - 1 : 0);
}

/* ---- snapshot ---- */

static void snap_take(ray_t* r, ray_t* fv, ray_t** args, int64_t n) {
    q_dbg_snapshot_clear();
    g_snap.err = r;
    g_snap.depth = g_live_depth < DBG_SNAP_MAX ? g_live_depth : DBG_SNAP_MAX;
    for (int i = 0; i < g_snap.depth; i++) {
        g_snap.lam[i] = g_live[i];
        ray_retain(g_snap.lam[i]);
    }
    memcpy(g_snap.stmt, g_stmt, sizeof g_snap.stmt);
    if (fv) {
        g_snap.ex = fv;
        ray_retain(fv);
        g_snap.eyn = n < DBG_EY_MAX ? (int)n : DBG_EY_MAX;
        for (int i = 0; i < g_snap.eyn; i++) {
            g_snap.ey[i] = args[i];
            ray_retain(args[i]);
        }
    }
}

int q_dbg_reported(ray_t* err) {
    return g_snap.err == err && g_snap.reported;
}

void q_dbg_print_trace(FILE* out, ray_t* err) {
    int64_t tn = 0;
    const char* text = q_err_text(err, &tn);
    fprintf(out, "'%.*s\n", (text && tn) ? (int)tn : 4,
            (text && tn) ? text : "eval");
    char line[DBG_STMT_MAX + 160];
    int depth = (g_snap.err == err) ? g_snap.depth : 0;
    const char* stmt = (g_snap.err == err) ? g_snap.stmt : g_stmt;
    for (int i = depth; i >= 0; i--) {
        frame_line(line, sizeof line, i, 0, i ? g_snap.lam[i - 1] : NULL, stmt);
        fputs(line, out);
    }
}

/* ---- the nested read-eval loop ---- */

static void dbg_show_console(void) {
    const char* con = q_console_str();
    if (con && *con) fputs(con, stdout);
    q_console_reset();
    fflush(stdout);
}

/* one backtrace-shaped line for frame `idx` (0 = the statement frame) */
static void dbg_show_frame(int idx) {
    char line[DBG_STMT_MAX + 160];
    frame_line(line, sizeof line, idx, 0, idx > 0 ? live_lam(idx) : NULL,
               g_stmt);
    fputs(line, stderr);
}

/* error text + the frame the cursor sits on, kdb's suspension banner (and `&`) */
static void dbg_banner(ray_t* err, int idx) {
    fflush(stdout);
    int64_t tn = 0;
    const char* text = q_err_text(err, &tn);
    fprintf(stderr, "'%.*s\n", (text && tn) ? (int)tn : 4,
            (text && tn) ? text : "eval");
    if (g_live_depth > 0) dbg_show_frame(idx);
    fflush(stderr);
}

static void dbg_prompt_str(char* p, size_t cap) {
    int n = q_sys_prompt(p, (int)cap - 16);
    for (int i = 0; i < g_dbg_depth && n + 1 < (int)cap; i++) p[n++] = ')';
    p[n] = '\0';
}

/* parse+eval one debug line in the SUSPENDED scope.  mode 0: interactive —
 * display, return NULL.  mode 1: `:r` — return the value silently.  mode 2:
 * `'err` — return the owned error to re-signal. */
static ray_t* dbg_eval(const char* src, int mode) {
    ray_t* ast = q_parse(src);
    if (RAY_IS_ERR(ast)) {
        fflush(stdout);
        fprintf(stderr, "'parse\n");
        fflush(stderr);
        ray_error_free(ast);
        return NULL;
    }
    int is_assign = q_parse_is_assign(ast);
    ray_t* res = q_eval_apply_concrete(q_eval(ast));
    ray_release(ast);
    dbg_show_console();
    if (RAY_IS_ERR(res)) {
        if (mode == 2) return res;               /* propagate the signal */
        if (!q_dbg_reported(res)) {
            fflush(stdout);
            int64_t tn = 0;
            const char* text = q_err_text(res, &tn);
            fprintf(stderr, "'%.*s\n", (text && tn) ? (int)tn : 4,
                    (text && tn) ? text : "eval");
            fflush(stderr);
        }
        q_err_drop();
        ray_error_free(res);
        return NULL;
    }
    if (mode == 1) return res;
    if (!is_assign && !RAY_IS_NULL(res)) {
        char buf[8192];
        q_fmt_console(res, buf, sizeof buf);
        fputs(buf, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    ray_release(res);
    return NULL;
}

/* the deepest frame keeps the ordinary top-rooted walk; frame [0] has no locals.
 * Past DBG_LIVE_MAX no env depth was stored, so rather than reroot at the
 * clamped slot's UNRELATED frame we decline and stay where the walk already is. */
static int32_t cursor_view(void) {
    if (g_cursor >= g_live_depth || g_cursor > DBG_LIVE_MAX)
        return Q_ENV_FRAME_VIEW_OFF;
    if (g_cursor <= 0) return Q_ENV_FRAME_VIEW_NONE;
    return g_live_env[g_cursor - 1];
}

/* dbg_eval with name READS rooted at the navigated frame; writes are untouched */
static ray_t* dbg_eval_at(const char* src, int mode) {
    int32_t prev = q_env_frame_view(cursor_view());
    ray_t* r = dbg_eval(src, mode);
    q_env_frame_view(prev);
    return r;
}

/* Returns NULL to propagate err (\ or EOF), a value to resume (:r), or a new
 * error to signal from this frame ('err). */
static ray_t* dbg_suspend(ray_t* err) {
    g_dbg_depth++;
    int outer_cursor = g_cursor;
    g_cursor = g_live_depth;
    dbg_show_console();
    dbg_banner(err, g_cursor);
    ray_t* out = NULL;
    char line[DBG_LINE_MAX];
    char prompt[80];
    for (;;) {
        dbg_prompt_str(prompt, sizeof prompt);
        int n = g_reader(prompt, line, sizeof line);
        if (n < 0) break;                                 /* EOF: abort */
        if (n == 0) continue;
        if (n == 1 && line[0] == '\\') break;             /* exit one level */
        if (n == 1 && line[0] == '&') { dbg_banner(err, g_cursor); continue; }
        if (n == 1 && (line[0] == '`' || line[0] == '.')) {   /* up / down */
            if (line[0] == '`') { if (g_cursor > 0) g_cursor--; }
            else if (g_cursor < g_live_depth) g_cursor++;
            fflush(stdout);
            dbg_show_frame(g_cursor);
            fflush(stderr);
            continue;
        }
        if (line[0] == ':') {                             /* :r — resume */
            ray_t* v = (n == 1) ? (ray_retain(RAY_NULL_OBJ), RAY_NULL_OBJ)
                                : dbg_eval_at(line + 1, 1);
            if (v) { out = v; break; }
            continue;
        }
        ray_t* v = dbg_eval_at(line, line[0] == '\'' ? 2 : 0);
        if (v) { out = v; break; }                        /* 'err: re-signal */
    }
    g_cursor = outer_cursor;
    g_dbg_depth--;
    return out;
}

/* ---- the two error seams ---- */

static int dbg_suspendable(ray_t* r) {
    return g_reader && g_console_stmt && g_trap_depth == 0 &&
           q_sys_err_trap_mode() == 1 && q_eval_apply_frame_depth() > 0 &&
           !q_err_is(r, QE_STOP);
}

static ray_t* dbg_observe(ray_t* r, ray_t* fv, ray_t** args, int64_t n) {
    if (!r || !RAY_IS_ERR(r) || q_err_is(r, QE_RETURN)) return r;
    if (g_snap.err == r) return r;             /* already seen deeper */
    snap_take(r, fv, args, n);
    if (dbg_suspendable(r)) {
        ray_t* rep = dbg_suspend(r);
        if (rep) {
            /* an 'err replacement OWNS the pending payload slot — drop only
             * when resuming with a plain value */
            if (!RAY_IS_ERR(rep)) {
                q_err_drop();
                q_dbg_snapshot_clear();
            }
            ray_error_free(r);
            return rep;                        /* resumed value or 'err */
        }
        /* a nested session may have clobbered the snapshot: re-take it so the
         * aborted error propagates as seen-and-reported, not fresh */
        if (g_snap.err != r) snap_take(r, fv, args, n);
        g_snap.reported = 1;
    }
    return r;
}

ray_t* q_dbg_filter(ray_t* r, ray_t* fv, ray_t** args, int64_t n) {
    return dbg_observe(r, fv, args, n);
}

ray_t* q_dbg_lambda_filter(ray_t* r) { return dbg_observe(r, NULL, NULL, 0); }

/* ---- .Q surface ---- */

/* opaque backtrace object: a list of formatted frame-line strings (only
 * .Q.sbt's rendering is contract; kdb's object internals are undocumented) */
static void bt_append(ray_t** l, const char* line, size_t n) {
    ray_t* s = ray_charv(line, (int64_t)n);
    if (s && !RAY_IS_ERR(s)) {
        *l = ray_list_append(*l, s);
        ray_release(s);
    }
}

static ray_t* dbg_bt_object(void) {
    ray_t* l = ray_list_new(g_snap.depth + 2);
    char line[DBG_STMT_MAX + 160];
    for (int i = g_snap.depth; i >= 1; i--) {   /* lambdas: [depth+1] .. [2] */
        size_t n = frame_line(line, sizeof line, i + 1, 0, g_snap.lam[i - 1],
                              g_snap.stmt);
        bt_append(&l, line, n ? n - 1 : 0);     /* strip \n */
    }
    bt_append(&l, "  [1]  (.Q.trp)", 15);
    size_t n = frame_line(line, sizeof line, 0, 0, NULL, g_snap.stmt);
    bt_append(&l, line, n ? n - 1 : 0);
    return l;
}

ray_t* q_dbg_trp_fn(ray_t** args, int64_t n) {
    if (n != 3) return q_err(QE_RANK);
    q_dbg_trap_enter();
    ray_t* r = q_eval_apply_concrete(q_eval_apply_value(args[0], &args[1], 1));
    q_dbg_trap_exit();
    if (!r || !RAY_IS_ERR(r) || q_err_is(r, QE_RETURN))
        return r ? r : q_err(QE_TYPE);
    ray_t* msg = q_err_take();
    if (!msg) {
        int64_t tn = 0;
        const char* tp = q_err_text(r, &tn);
        msg = ray_charv(tp ? tp : "", tn);
    }
    ray_error_free(r);
    if (!msg || RAY_IS_ERR(msg)) return msg ? msg : q_err(QE_OOM);
    ray_t* bt = dbg_bt_object();
    q_dbg_snapshot_clear();
    if (!bt || RAY_IS_ERR(bt)) { ray_release(msg); return bt ? bt : q_err(QE_OOM); }
    ray_t* av[2] = { msg, bt };
    ray_t* c = q_eval_apply_concrete(q_eval_apply_value(args[2], av, 2));
    ray_release(msg);
    ray_release(bt);
    return c;
}

ray_t* q_dbg_sbt_fn(ray_t* x) {
    if (!x || (x->type != RAY_LIST && !ray_is_vec(x))) return q_err(QE_TYPE);
    char buf[8192];
    size_t used = 0;
    int64_t cnt = ray_len(x);
    for (int64_t i = 0; i < cnt; i++) {
        ray_t* s = q_index_elem_at(x, i);
        if (!s || RAY_IS_ERR(s)) { if (s) ray_release(s); continue; }
        if (s->type == RAY_CHARV) {
            size_t sn = (size_t)ray_len(s);
            const char* sp = (const char*)ray_data(s);
            if (used + sn + 1 < sizeof buf) {
                memcpy(buf + used, sp, sn);
                used += sn;
                buf[used++] = '\n';
            }
        }
        ray_release(s);
    }
    return ray_charv(buf, (int64_t)used);
}

ray_t* q_dbg_bt_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;                       /* .Q.bt[] — arg ignored */
    char line[DBG_STMT_MAX + 160];
    for (int i = g_live_depth; i >= 0; i--) {
        int mark = g_dbg_depth > 0 && i == g_cursor;   /* >> = the current frame */
        size_t ln = frame_line(line, sizeof line, i, mark,
                               i ? live_lam(i) : NULL, g_stmt);
        q_console_write(line, ln);
    }
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

ray_t* q_dbg_zex(void) {
    if (!g_snap.ex) return NULL;
    ray_retain(g_snap.ex);
    return g_snap.ex;
}

ray_t* q_dbg_zey(void) {
    if (!g_snap.ex) return NULL;
    ray_t* l = ray_list_new(g_snap.eyn ? g_snap.eyn : 1);
    for (int i = 0; i < g_snap.eyn; i++) l = ray_list_append(l, g_snap.ey[i]);
    return l;
}
