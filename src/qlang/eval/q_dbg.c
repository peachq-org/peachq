/* q_dbg — see q_dbg.h.  The suspended state IS the C stack: the nested
 * read-eval loop runs where the error was produced, so the failing lambda's
 * q_env locals are live and `:r` feeds the walker in place.  A caret sits under
 * the head token of the application in flight in its frame: the walker's
 * current node (the deepest frame) or the node that called the frame above
 * (every other), resolved through the parser's caret table at snapshot time. */
#include "qlang/q_count.h"
#include "qlang/eval/q_dbg.h"
#include "qlang/eval/q_eval.h"
#include "qlang/eval/q_eval_internal.h"   /* q_eval_node */
#include "qlang/base/q_err.h"
#include "qlang/parse/q_parse.h"
#include "qlang/q_ctx.h"
#include "qlang/q_comment.h"   /* q_comment_origin — a load line's file + line */
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
#define DBG_NEST_MAX 8
#define DBG_KEEP_ERR 4   /* _end token bit: this statement leaves with its own payload */
#define DBG_TRAP_SHIFT 3 /* _end token bits from here: the trap depth the level beneath had */

static _Thread_local ray_t* g_live[DBG_LIVE_MAX];  /* borrowed lambda values, or a `value`d text's charv */
static _Thread_local ray_t* g_live_pos[DBG_LIVE_MAX];  /* a text frame's caret table (borrowed), NULL for a lambda */
static _Thread_local int32_t g_live_env[DBG_LIVE_MAX];  /* their q_env frames */
static _Thread_local const ray_t* g_live_call[DBG_LIVE_MAX];  /* the caller's node in flight at the push */
static _Thread_local const ray_t* g_frozen[DBG_LIVE_MAX + 1];  /* a lambda frame's node, HELD while a nested run walks */
static _Thread_local int    g_live_depth;
static _Thread_local int    g_frame_base;          /* the first live frame a snapshot reports */
static _Thread_local int    g_frame_num;           /* the number its statement frame shows: 0, or a q)) line's */
static _Thread_local int    g_cursor;              /* navigated frame, 0 = [0] */
/* a `'err` typed at q)) leaves the frames from cut_depth up unobserved: kdb destroys that frame (debug.md §Signal) */
static _Thread_local const ray_t* g_cut_err;
static _Thread_local int    g_cut_depth;

/* file/line: the loading script's, for a load line; 0 for console input (a q))
 * line typed into a suspended load included), so its lambdas and frame stay bare.
 * pos: the statement's caret table (owned), set once its text has parsed; depth: the live frames beneath it;
 * node: its own node, held while a nested run walks (NULL = live). */
typedef struct { char text[DBG_STMT_MAX]; int64_t file, line; ray_t* pos; int depth; const ray_t* node; } dbg_stmt_t;

static dbg_stmt_t g_stmts[DBG_NEST_MAX];
static int  g_console_stmt;
static int  g_trap_depth;
static int  g_dbg_depth;
static int  g_prompt_frames;
/* a suspension, on dbg_suspend's C stack: the statement it holds — its frames base+1..top numbered from num — which
 * navigation, the banner and .Q.bt show; outer = the suspension whose q)) line this one interrupted */
typedef struct dbg_sus { int base, num, top; const dbg_stmt_t* stmt; struct dbg_sus* outer; } dbg_sus_t;
static _Thread_local dbg_sus_t* g_sus;

static q_dbg_read_fn g_reader;

typedef struct {
    ray_t* err;                    /* identity only — never dereferenced */
    ray_t* lam[DBG_SNAP_MAX];      /* retained, outermost first */
    int    off[DBG_SNAP_MAX];      /* their caret offsets, -1 = none */
    int    stmt_off;               /* frame [0]'s */
    int    depth;
    int    num;                    /* the number frame [0] shows (g_frame_num) */
    int    taken;                  /* the statement level (unclamped g_depth) that took it: it ends with that level */
    ray_t* ex;                     /* retained failed-primitive value */
    ray_t* ey[DBG_EY_MAX];         /* retained args of the failed apply */
    int    eyn;
    int    reported;
} dbg_snap_t;

/* Statement state is per LEVEL, so a nested statement (a q)) line, a `\l`
 * inside one, a remote callback) cannot take the level beneath its frame-[0]
 * text, its snapshot or its pending error payload.  The LIVE snapshot is a slot
 * INDEX, not a copy: a level reads the one it inherited until it errors and
 * takes its own, so a q)) line still answers .z.ex/.z.ey.  Past DBG_NEST_MAX
 * levels share the last slot: state loss, never corruption. */
static dbg_snap_t g_snaps[DBG_NEST_MAX];
static ray_t*     g_held[DBG_NEST_MAX];    /* the payload the level beneath had */
static int8_t     g_outer[DBG_NEST_MAX];   /* the slot it was reading */
static q_dbg_hold_t g_holds[DBG_NEST_MAX]; /* the frame beneath keeps its node while this level runs */
static int        g_depth, g_snap_lvl;
#define LVL(d)  ((d) <= 0 ? 0 : (d) < DBG_NEST_MAX ? (d) : DBG_NEST_MAX - 1)
#define g_stmt  g_stmts[LVL(g_depth - 1)]
#define g_snap  g_snaps[g_snap_lvl]
#define snap_stmt g_stmts[g_snap_lvl]      /* the statement it was taken in */

void q_dbg_frame_push(ray_t* lam) {
    if (g_live_depth < DBG_LIVE_MAX) {
        g_live[g_live_depth] = lam;
        g_live_pos[g_live_depth] = NULL;
        g_live_env[g_live_depth] = q_env_frame_depth() - 1;   /* its own frame */
        g_live_call[g_live_depth] = q_eval_node;
    }
    g_live_depth++;
}

void q_dbg_text_push(ray_t* text, ray_t* pos) {
    q_dbg_frame_push(text);
    if (g_live_depth > DBG_LIVE_MAX) return;
    g_live_pos[g_live_depth - 1] = pos;
    g_live_env[g_live_depth - 1] = Q_ENV_FRAME_VIEW_NONE;   /* a text reads globals only */
}

static int is_text(const ray_t* v) { return v->type == RAY_CHARV; }

/* frame i, 1-based; past the stored window the deepest stored lambda stands in */
static int live_clamp(int i) { return i > DBG_LIVE_MAX ? DBG_LIVE_MAX : i; }
static ray_t* live_lam(int i) { return g_live[live_clamp(i) - 1]; }

/* the node a frame reports: held while a nested run walks in it (a load line, a q)) line, `value "…"`, a qSQL
 * phrase, a view, a suspension), else the walker's for the deepest frame and the call site for every other */
static const ray_t* lam_node(int i) {                    /* live lambda frame i, 1-based */
    if (i <= DBG_LIVE_MAX && g_frozen[i]) return g_frozen[i];
    return i >= g_live_depth || i >= DBG_LIVE_MAX ? q_eval_node : g_live_call[i];
}

static const ray_t* stmt_node(const dbg_stmt_t* s) {
    if (s->node) return s->node;
    return g_live_depth > s->depth && s->depth < DBG_LIVE_MAX ? g_live_call[s->depth] : q_eval_node;
}

q_dbg_hold_t q_dbg_hold(void) {
    q_dbg_hold_t h;
    h.slot = g_live_depth > g_stmt.depth && g_live_depth <= DBG_LIVE_MAX ? &g_frozen[g_live_depth] : &g_stmt.node;
    h.prev = *h.slot;
    if (!h.prev) *h.slot = q_eval_node;
    return h;
}

void q_dbg_release(q_dbg_hold_t h) {
    q_eval_node = *h.slot;
    *h.slot = h.prev;
}

/* live frame i's caret table: a text frame's own, else its lambda's */
static ray_t* live_tab(int i) {
    ray_t* t = g_live_pos[live_clamp(i) - 1];
    return t ? t : q_eval_apply_lambda_pos(live_lam(i));
}

/* the caret offset of node in a frame's caret table; -1 when unknown.  The newest entry wins — a node the parser
 * dropped may have lent its address to a later one.  A failed parse's table keys its die token by NULL. */
static int frame_off(ray_t* tab, const ray_t* node) {
    if (!tab) return -1;
    const int64_t* p = (const int64_t*)ray_data(tab);
    for (int64_t i = q_count(tab) - 2; i >= 0; i -= 2)
        if (p[i] == (int64_t)(uintptr_t)node) return (int)p[i + 1];
    return -1;
}

/* at q)) with no lambda of the line's own live, the navigated frame's (KDB-X: "It follows you as you navigate") */
ray_t* q_dbg_self(void) {
    int i = g_sus && g_live_depth == g_frame_base ? g_cursor : g_live_depth;
    int floor = g_sus ? g_sus->base : 0;
    while (i > floor && is_text(live_lam(i))) i--;
    if (i <= floor) return NULL;
    ray_t* lam = live_lam(i);
    ray_retain(lam);
    return lam;
}

/* the caller's node comes back: an iterator calling the lambda again would otherwise push with the body's last */
void q_dbg_frame_pop(void) {
    if (--g_live_depth < DBG_LIVE_MAX) q_eval_node = g_live_call[g_live_depth];
}

int q_dbg_frame_base(int base) {
    int prev = g_frame_base;
    g_frame_base = base < 0 ? g_live_depth : base;
    return prev;
}

void q_dbg_snapshot_clear(void) {
    for (int i = 0; i < g_snap.depth; i++) ray_release(g_snap.lam[i]);
    if (g_snap.ex) ray_release(g_snap.ex);
    for (int i = 0; i < g_snap.eyn; i++) ray_release(g_snap.ey[i]);
    memset(&g_snap, 0, sizeof g_snap);
}

static void stmt_pos_drop(dbg_stmt_t* s) {
    if (s->pos) ray_release(s->pos);
    s->pos = NULL;
}

int q_dbg_statement_pos(ray_t* pos) {
    if (g_stmt.pos) return 0;
    g_stmt.pos = pos;
    return 1;
}

int q_dbg_statement_begin(const char* src, size_t n, int console) {
    int load_line = console < 0;
    if (load_line) console = g_console_stmt ? 2 : 0;   /* load line: inherit */
    int d = g_depth;
    if (d > 0 && d < DBG_NEST_MAX) g_holds[d] = q_dbg_hold();   /* the level beneath, before g_stmt moves */
    g_depth++;
    if (d > 0 && d < DBG_NEST_MAX) {
        g_held[d] = q_err_take();
        g_outer[d] = (int8_t)g_snap_lvl;
    } else if (!d) q_err_drop();   /* a stale payload would caption this error */
    if (!g_dbg_depth) q_dbg_snapshot_clear();
    if (n >= sizeof g_stmt.text) n = sizeof g_stmt.text - 1;
    if (src && n) memcpy(g_stmt.text, src, n); else n = 0;
    g_stmt.text[n] = '\0';
    g_stmt.file = load_line ? q_comment_origin(&g_stmt.line) : 0;
    stmt_pos_drop(&g_stmt);
    g_stmt.depth = g_live_depth;
    g_stmt.node = NULL;
    q_eval_node = NULL;
    int prev = g_console_stmt | g_trap_depth << DBG_TRAP_SHIFT;
    if (!load_line) g_trap_depth = 0;   /* a load line is its asker's; any other statement owns its errors */
    g_console_stmt = console;
    return prev;
}

/* once the statement's own text has parsed (its caret table is in), a parse is another text's — `value "…"` — and
 * what it makes is console-made */
int64_t q_dbg_statement_origin(int64_t* line) {
    if (g_depth <= 0 || !g_stmt.file || g_stmt.pos) return 0;   /* between statements the slot is stale */
    *line = g_stmt.line;
    return g_stmt.file;
}

void q_dbg_statement_end(int tok) {
    g_console_stmt = tok & 3;
    g_trap_depth = tok >> DBG_TRAP_SHIFT;
    stmt_pos_drop(&g_stmt);
    g_cut_err = NULL;
    int d = --g_depth;
    /* whatever this level took goes with it: its error is freed now, and a later error at the same address
     * must never read as already seen (the shared slot past DBG_NEST_MAX included) */
    if (g_snap.taken == d + 1) q_dbg_snapshot_clear();
    if (d <= 0 || d >= DBG_NEST_MAX) return;
    q_dbg_release(g_holds[d]);
    g_snap_lvl = g_outer[d];
    if (tok & DBG_KEEP_ERR) { if (g_held[d]) ray_release(g_held[d]); }
    else q_err_put(g_held[d]);
    g_held[d] = NULL;
}

int64_t q_dbg_trap_enter(void) { g_trap_depth++; return q_env_ctx(); }
int     q_dbg_trapped(void) { return g_trap_depth > 0; }
void q_dbg_trap_exit(int64_t ctx, ray_t* r) {
    g_trap_depth--;
    g_cut_err = NULL;
    if (r && RAY_IS_ERR(r) && !q_err_is(r, QE_RETURN)) q_env_ctx_set(ctx);
}

q_dbg_read_fn q_dbg_set_reader(q_dbg_read_fn fn) {
    q_dbg_read_fn prev = g_reader;
    g_reader = fn;
    return prev;
}

void q_dbg_reset(void) {
    q_dbg_snapshot_clear();
    g_live_depth = g_frame_base = g_frame_num = g_cursor = 0;
    g_sus = NULL;
    g_cut_err = NULL;
    memset(g_frozen, 0, sizeof g_frozen);
    for (int i = 0; i < DBG_NEST_MAX; i++) { stmt_pos_drop(&g_stmts[i]); g_stmts[i].node = NULL; g_stmts[i].depth = 0; }
    g_stmt.text[0] = '\0';
    g_stmt.file = 0;
    g_console_stmt = g_trap_depth = g_dbg_depth = g_prompt_frames = g_depth = 0;
    g_reader = NULL;
}

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
    int64_t cnt = q_count(names);
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

/* else the name its first assignment stamped: an anonymous inner lambda's `f@` (basics/debug.md §Errors), a dotted
 * handler's `.z.pg` — a root name's `..` dropped */
static int lam_name(ray_t* lam, char* buf, size_t cap) {
    if (lam_global_name(lam, buf, cap)) return 1;
    ray_t* nm = NULL;
    q_eval_apply_lambda_prov(lam, &nm, NULL, NULL);
    if (!nm) return 0;
    const char* p = (const char*)ray_data(nm);
    size_t n = (size_t)q_count(nm);
    if (n > 2 && p[0] == '.' && p[1] == '.') { p += 2; n -= 2; }
    if (n + 1 >= cap) return 0;
    memcpy(buf, p, n);
    buf[n] = '\0';
    return 1;
}

/* one frame: `  [i]  file:line: name:src` then its caret line (basics/debug.md — file:line only "if such
 * information is available"; the `^` under the head token at `off`, a blank line when off < 0).  A multi-line
 * text shows the lines through the failing one, the caret, then the rest (KDB-X how-to).  lam NULL = the
 * statement frame (from stmt).  OWNED NUL-terminated text (ray_free_raw), *n its length; NULL on OOM. */
static char* frame_text(size_t* n, int idx, int mark, ray_t* lam, const dbg_stmt_t* stmt, int off) {
    const char* src = stmt->text;
    size_t sn = strlen(src);
    int64_t fs = stmt->file, ln = stmt->line;
    char name[128], floc[300], pre[480];
    name[0] = floc[0] = '\0';
    if (lam && is_text(lam)) {
        src = (const char*)ray_data(lam);
        sn = (size_t)q_count(lam);
        fs = 0;
    } else if (lam) {
        ray_t* s = q_eval_apply_lambda_src(lam);
        src = s ? ray_str_ptr(s) : "{}";
        sn = s ? ray_str_len(s) : 2;
        if (lam_name(lam, name, sizeof name - 1)) strcat(name, ":");
        q_eval_apply_lambda_prov(lam, NULL, &fs, &ln);
    }
    ray_t* fp = fs ? ray_sym_str(fs) : NULL;             /* borrowed */
    if (fp)
        snprintf(floc, sizeof floc, "%.*s:%lld: ", (int)ray_str_len(fp),
                 ray_str_ptr(fp), (long long)ln);
    size_t ls = 0, le = sn;                              /* the failing line's span in src */
    if (off >= 0 && (size_t)off < sn) {
        for (size_t i = 0; i < (size_t)off; i++) if (src[i] == '\n') ls = i + 1;
        for (le = (size_t)off; le < sn && src[le] != '\n'; le++) ;
    } else off = -1;
    size_t plen = (size_t)snprintf(pre, sizeof pre, "%s[%d]  %s%s", mark ? ">>" : "  ", idx, floc, name);
    size_t col = off < 0 ? 0 : (ls ? 0 : plen) + ((size_t)off - ls);
    char* dst = (char*)ray_alloc_raw(plen + sn + col + 4);
    if (!dst) return NULL;
    size_t k = plen;
    memcpy(dst, pre, plen);
    memcpy(dst + k, src, le); k += le;
    dst[k++] = '\n';
    memset(dst + k, ' ', col); k += col;
    if (off >= 0) dst[k++] = '^';
    dst[k++] = '\n';
    if (le < sn) { memcpy(dst + k, src + le + 1, sn - le - 1); k += sn - le - 1; dst[k++] = '\n'; }
    dst[k] = '\0';
    *n = k;
    return dst;
}

static void frame_put(FILE* out, char* text) {
    if (!text) return;
    fputs(text, out);
    ray_free_raw(text);
}

static void snap_take(ray_t* r, ray_t* fv, ray_t** args, int64_t n) {
    g_snap_lvl = LVL(g_depth - 1);   /* this level's own slot; an inherited one stands */
    q_dbg_snapshot_clear();
    g_snap.err = r;
    g_snap.taken = g_depth;
    int stored = g_live_depth < DBG_LIVE_MAX ? g_live_depth : DBG_LIVE_MAX;
    int base   = g_frame_base < stored ? g_frame_base : stored;
    g_snap.depth = stored - base < DBG_SNAP_MAX ? stored - base : DBG_SNAP_MAX;
    g_snap.num = g_frame_num;
    for (int i = 0; i < g_snap.depth; i++) {
        g_snap.lam[i] = g_live[base + i];
        ray_retain(g_snap.lam[i]);
        g_snap.off[i] = frame_off(live_tab(base + i + 1), lam_node(base + i + 1));
    }
    g_snap.stmt_off = frame_off(snap_stmt.pos, stmt_node(&snap_stmt));
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

int q_dbg_prompt_frames(void) { return g_dbg_depth > 0 ? g_prompt_frames : 0; }

static _Thread_local unsigned g_errs_shown;   /* error lines printed: a q)) line's error since the banner shows */

static void dbg_err_line(FILE* out, ray_t* err) {
    g_errs_shown++;
    int64_t tn = 0;
    const char* text = q_err_text(err, &tn);
    fprintf(out, "'%.*s\n", (text && tn) ? (int)tn : 4,
            (text && tn) ? text : "eval");
}

/* snapshot frame i (0 = the statement), numbered as `idx` (the bt object shifts the lambdas up by one) */
static char* snap_text(size_t* n, int i, int idx) {
    return frame_text(n, g_snap.num + idx, 0, i ? g_snap.lam[i - 1] : NULL, &snap_stmt, i ? g_snap.off[i - 1] : g_snap.stmt_off);
}

void q_dbg_print_trace(FILE* out, ray_t* err) {
    dbg_err_line(out, err);
    size_t n;
    if (g_snap.err == err)
        for (int i = g_snap.depth; i >= 0; i--) frame_put(out, snap_text(&n, i, i));
    else
        frame_put(out, frame_text(&n, g_frame_num, 0, NULL, &g_stmt, frame_off(g_stmt.pos, stmt_node(&g_stmt))));
    fflush(out);   /* Windows buffers stderr to a file: a worker killed later would lose its trace */
}

/* live frame idx (1-based; base = the statement), numbered from the statement's own [num]: the frames beneath
 * base are its runner's (a transcript line's door, the suspended statement under a q)) line) and never show */
static char* live_text(size_t* n, int idx, int mark, const dbg_stmt_t* stmt, int base, int num) {
    int own = idx > base;
    ray_t* lam = own ? live_lam(idx) : NULL;
    const ray_t* node = own ? lam_node(idx) : stmt_node(stmt);
    return frame_text(n, num + (own ? idx - base : 0), mark, lam, stmt, frame_off(own ? live_tab(idx) : stmt->pos, node));
}

static void dbg_show_frame(int idx) {
    size_t n;
    frame_put(stderr, live_text(&n, idx, 0, g_sus->stmt, g_sus->base, g_sus->num));
}

/* error text + the frame the cursor sits on, kdb's suspension banner (and `&`).
 * With no lambda live (a load's top-level statement) frame [0] IS the frame. */
static void dbg_banner(ray_t* err, int idx) {
    fflush(stdout);
    dbg_err_line(stderr, err);
    dbg_show_frame(idx);
    fflush(stderr);
}

static void dbg_prompt_str(char* p, size_t cap) {
    int n = q_sys_prompt(p, (int)cap - 16);
    for (int i = 0; n && i < g_dbg_depth && n + 1 < (int)cap; i++) p[n++] = ')';
    p[n] = '\0';
}

/* the deepest frame keeps the ordinary top-rooted walk; frame [0] has no locals.
 * Past DBG_LIVE_MAX no env depth was stored, so rather than reroot at the
 * clamped slot's UNRELATED frame we decline and stay where the walk already is. */
static int32_t cursor_view(void) {
    if (g_cursor >= g_live_depth || g_cursor > DBG_LIVE_MAX)
        return Q_ENV_FRAME_VIEW_OFF;
    if (g_cursor <= g_sus->base) return Q_ENV_FRAME_VIEW_NONE;
    return g_live_env[g_cursor - 1];
}

/* Only `:r` and `'e` want a VALUE rather than a display, so only they bypass
 * q_ctx_run_line — and so must open the statement seam themselves.  keep_err
 * hands the error back to re-signal (it leaves carrying its OWN payload);
 * otherwise it is reported here and NULL comes back (stay suspended). */
static ray_t* dbg_value_at(const char* src, int keep_err) {
    int tok = q_dbg_statement_begin(src, strlen(src), 1);
    int32_t prev = q_env_frame_view(cursor_view());
    FILE*   door = q_console_door(stdout);
    ray_t* pos = NULL;
    ray_t* r = q_parse_at(src, &pos);
    if (!RAY_IS_ERR(r)) {
        ray_t* ast = r;
        q_dbg_statement_pos(pos);                  /* a fresh level: nothing to hand back */
        r = q_eval_apply_concrete(q_eval(ast));
        ray_release(ast);
    }
    q_console_door(door);
    q_env_frame_view(prev);
    int resignal = RAY_IS_ERR(r) && keep_err;
    if (RAY_IS_ERR(r) && !keep_err) {
        if (!q_dbg_reported(r)) {
            fflush(stdout);
            dbg_err_line(stderr, r);
            fflush(stderr);
        }
        ray_error_free(r);
        r = NULL;
    }
    q_dbg_statement_end(resignal ? tok | DBG_KEEP_ERR : tok);
    return r;
}

/* Every other debug line is an ORDINARY statement through the ONE console
 * seam — views, `x)` prefixes, system commands and display all behave as at
 * `q)` — with name reads rooted at the navigated frame. */
static void dbg_run_line(const char* line, size_t n) {
    int32_t prev = q_env_frame_view(cursor_view());
    q_ctx_run_debug_line(line, n, stdout, stderr);
    q_env_frame_view(prev);
}

/* Returns NULL to propagate err (\ or EOF), a value to resume (:r), or a new
 * error to signal from this frame ('err). */
/* A q)) line reports its own frames only, numbered past the suspended ones and two hidden frames — the debugger's
 * own and the line's — as kdb numbers them (debug.md `{x+y}[a;y]` -> [5] under a [2] suspension; Q4M `b:a+y`). */
static ray_t* dbg_suspend(ray_t* err) {
    g_dbg_depth++;
    int outer_cursor = g_cursor;
    int outer_frames = g_prompt_frames;
    dbg_sus_t sus = { g_frame_base, g_frame_num, g_live_depth, &g_stmt, g_sus };
    q_dbg_hold_t hold = q_dbg_hold();          /* the q)) lines must not move the suspended frame's caret */
    int32_t env_floor = q_env_frame_floor(0);  /* q)) lines reach every suspended frame, a `value`d text's door floors */
    int     app_floor = q_eval_apply_frame_floor(0);
    g_prompt_frames = q_eval_apply_frame_depth();
    g_cursor = g_live_depth;
    g_sus = &sus;
    g_frame_base = sus.top;
    g_frame_num = sus.num + sus.top - sus.base + 2;
    q_console_flush();
    dbg_banner(err, g_cursor);
    unsigned shown = g_errs_shown;
    ray_t* out = NULL;
    char line[DBG_LINE_MAX];
    char prompt[Q_SYS_PROMPT_CAP + 16];
    for (;;) {
        dbg_prompt_str(prompt, sizeof prompt);
        int n = g_reader(prompt, line, sizeof line);
        /* the debug-only tokens (basics/debug.md) peeled first; everything
         * else is an ordinary statement — `\\` included, so it exits the
         * PROCESS as at `q)`.  Lone `\` exits ONE level (ref/debug.md v1). */
        if (n < 0) break;                                 /* EOF: abort */
        if (n == 0) continue;
        if (n == 1 && line[0] == '\\') break;             /* exit one level */
        /* `.` onto the error frame after another error was shown reprints the banner (Q4M §10.2; KDB-X's plain `.`
         * onto it does not) */
        if (n == 1 && (line[0] == '`' || line[0] == '.' || line[0] == '&')) {   /* up / down / where */
            if (line[0] == '`') { if (g_cursor > sus.base) g_cursor--; }
            else if (line[0] == '.' && g_cursor < g_live_depth) g_cursor++;
            if (line[0] == '&' || (line[0] == '.' && g_cursor == g_live_depth && g_errs_shown != shown)) {
                dbg_banner(err, g_cursor);
                shown = g_errs_shown;
                continue;
            }
            fflush(stdout);
            dbg_show_frame(g_cursor);
            fflush(stderr);
            continue;
        }
        if (line[0] == ':') {                             /* :r — resume */
            ray_t* v = (n == 1) ? (ray_retain(RAY_NULL_OBJ), RAY_NULL_OBJ)
                                : dbg_value_at(line + 1, 0);
            if (v) { out = v; break; }
            continue;
        }
        if (line[0] == '\'') {                            /* 'err — re-signal */
            ray_t* v = dbg_value_at(line, 1);
            if (v && RAY_IS_ERR(v)) { out = v; break; }
            if (v) ray_release(v);
            continue;
        }
        dbg_run_line(line, (size_t)n);
    }
    g_cursor = outer_cursor;
    g_prompt_frames = outer_frames;
    g_frame_base = sus.base;
    g_frame_num = sus.num;
    g_sus = sus.outer;
    q_eval_apply_frame_floor(app_floor);
    q_env_frame_floor(env_floor);
    q_dbg_release(hold);
    g_dbg_depth--;
    return out;
}

/* Local errors ALWAYS suspend (debug.md — no toggle); `\e` gates only whether
 * the REMOTE door marks its statements console (q_ctx.c remote seams). */
/* "inside a lambda": an apply frame above the prompt's, or a `value`d text (whose door floors the apply frames) run
 * above a lambda frame of the statement's own */
static int dbg_in_lambda(void) {
    if (q_eval_apply_frame_depth() > q_dbg_prompt_frames()) return 1;
    int i = g_live_depth;
    if (i > DBG_LIVE_MAX) return 0;
    while (i > g_frame_base && is_text(live_lam(i))) i--;
    return i > g_frame_base && i < g_live_depth;
}

static int dbg_suspendable(ray_t* r) {
    return g_reader && g_console_stmt && g_trap_depth == 0 && dbg_in_lambda() && !q_err_is(r, QE_STOP);
}

static ray_t* dbg_observe(ray_t* r, ray_t* fv, ray_t** args, int64_t n) {
    if (!r || !RAY_IS_ERR(r) || q_err_is(r, QE_RETURN)) return r;
    if (g_snap.err == r) return r;             /* already seen deeper */
    if (r == g_cut_err) {
        if (g_live_depth >= g_cut_depth) return r;
        g_cut_err = NULL;
    }
    snap_take(r, fv, args, n);
    if (dbg_suspendable(r)) {
        ray_t* rep = dbg_suspend(r);
        if (rep) {
            /* an 'err replacement OWNS the pending payload slot — drop only
             * when resuming with a plain value */
            if (!RAY_IS_ERR(rep)) {
                q_err_drop();
                q_dbg_snapshot_clear();
            } else {
                g_cut_err = rep;
                g_cut_depth = g_live_depth;
            }
            ray_error_free(r);
            return rep;                        /* resumed value or 'err */
        }
        if (g_snap.err != r) snap_take(r, fv, args, n);   /* past DBG_NEST_MAX only */
        g_snap.reported = 1;
    }
    return r;
}

ray_t* q_dbg_filter(ray_t* r, ray_t* fv, ray_t** args, int64_t n) {
    return dbg_observe(r, fv, args, n);
}

ray_t* q_dbg_lambda_filter(ray_t* r) { return dbg_observe(r, NULL, NULL, 0); }

/* dbg_suspendable minus the lambda-frame requirement: the script statement IS
 * a frame (debug.md#context — "system commands, including \l, correspond to
 * individual debug stack frames").  Flag 2 keeps every non-load door out. */
ray_t* q_dbg_load_filter(ray_t* r) {
    if (!r || !RAY_IS_ERR(r) || q_err_is(r, QE_RETURN) || q_dbg_reported(r))
        return NULL;
    if (!(g_reader && g_console_stmt == 2 && g_trap_depth == 0 &&
          !q_err_is(r, QE_STOP)))
        return NULL;
    if (g_snap.err != r) snap_take(r, NULL, NULL, 0);
    ray_t* rep = dbg_suspend(r);
    if (rep && !RAY_IS_ERR(rep)) {
        q_err_drop();
        q_dbg_snapshot_clear();
    }
    if (!rep) g_snap.reported = 1;
    return rep;
}

void q_dbg_mark_reported(ray_t* e) {
    if (g_depth <= 0) return;
    g_snap_lvl = LVL(g_depth - 1);
    q_dbg_snapshot_clear();
    g_snap.err = e;
    g_snap.taken = g_depth;
    g_snap.reported = 1;
}

/* opaque backtrace object: a list of formatted frame strings, each its frame line plus caret line (only .Q.sbt's
 * rendering is contract; kdb's object internals are undocumented).  The trap's own frame has an empty caret line. */
static void bt_append(ray_t** l, const char* line, size_t n) {
    ray_t* s = ray_charv(line, (int64_t)n);
    if (s && !RAY_IS_ERR(s)) {
        *l = ray_list_append(*l, s);
        ray_release(s);
    }
}

static void bt_append_snap(ray_t** l, int i, int idx) {   /* the frame without its final \n */
    size_t n = 0;
    char* text = snap_text(&n, i, idx);
    if (!text) return;
    bt_append(l, text, n ? n - 1 : 0);
    ray_free_raw(text);
}

/* the trap's own frame sits where it was called: above the `below` frames that called it (KDB-X safeRun) */
static ray_t* dbg_bt_object(const char* trap, int below) {
    if (below > g_snap.depth) below = g_snap.depth;
    ray_t* l = ray_list_new(g_snap.depth + 2);
    for (int i = g_snap.depth; i > below; i--) bt_append_snap(&l, i, i + 1);
    char door[64];
    bt_append(&l, door, (size_t)snprintf(door, sizeof door, "  [%d]  (%s)\n", g_snap.num + below + 1, trap));
    for (int i = below; i >= 0; i--) bt_append_snap(&l, i, i);
    return l;
}

/* .Q.trp applies f to x; .Q.trpd (= -105!) is its dot form, spreading x as the argument list */
static ray_t* dbg_trap(ray_t** args, int dot) {
    int below = g_live_depth - g_frame_base;
    int64_t ctx = q_dbg_trap_enter();
    ray_t* r = dot ? q_eval_dot_wrap(args, 2) : q_eval_apply_concrete(q_eval_apply_value(args[0], &args[1], 1));
    q_dbg_trap_exit(ctx, r);
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
    ray_t* bt = dbg_bt_object(dot ? ".Q.trpd" : ".Q.trp", below);
    q_dbg_snapshot_clear();
    if (!bt || RAY_IS_ERR(bt)) { ray_release(msg); return bt ? bt : q_err(QE_OOM); }
    ray_t* av[2] = { msg, bt };
    ray_t* c = q_eval_apply_concrete(q_eval_apply_value(args[2], av, 2));
    ray_release(msg);
    ray_release(bt);
    return c;
}

ray_t* q_dbg_trp_fn(ray_t** args, int64_t n) {
    return n == 3 ? dbg_trap(args, 0) : q_err(QE_RANK);
}

ray_t* q_dbg_trpd_fn(ray_t** args, int64_t n) {
    return n == 3 ? dbg_trap(args, 1) : q_err(QE_RANK);
}

ray_t* q_dbg_trp_dot(ray_t* y) {
    if (!y || y->type != RAY_LIST || q_count(y) != 3) return q_err(QE_TYPE);
    ray_t* av[3] = { ray_list_get(y, 0), ray_list_get(y, 1), ray_list_get(y, 2) };
    return q_dbg_trpd_fn(av, 3);
}

ray_t* q_dbg_sbt_fn(ray_t* x) {
    if (!x || (x->type != RAY_LIST && !ray_is_vec(x))) return q_err(QE_TYPE);
    char buf[8192];
    size_t used = 0;
    int64_t cnt = q_count(x);
    for (int64_t i = 0; i < cnt; i++) {
        ray_t* s = q_index_elem_at(x, i);
        if (!s || RAY_IS_ERR(s)) { if (s) ray_release(s); continue; }
        if (s->type == RAY_CHARV) {
            size_t sn = (size_t) q_count(s);
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

static void bt_frames(int hi, int lo, const dbg_stmt_t* stmt, int base, int num, int cursor) {
    for (int i = hi; i >= lo; i--) {
        size_t ln = 0;
        char* text = live_text(&ln, i, i == cursor, stmt, base, num);
        if (!text) continue;
        q_console_write(text, ln, false);
        ray_free_raw(text);
    }
}

/* suspended: the frames a q)) line runs above the suspension — the line's own statement frame hidden, as the page
 * shows none for `.Q.bt[]` typed there — then every suspended statement's down to [0], >> on the current frame */
ray_t* q_dbg_bt_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;                       /* .Q.bt[] — arg ignored */
    bt_frames(g_live_depth, g_frame_base + (g_sus != NULL), &g_stmt, g_frame_base, g_frame_num, -1);
    for (dbg_sus_t* s = g_sus; s; s = s->outer)
        bt_frames(s->top, s->base, s->stmt, s->base, s->num, s == g_sus ? g_cursor : -1);
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
