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
#include "qlang/base/q_type.h"    /* q_type_is_int_atom — a frame record's numbers */
#include "qlang/q_prim.h"         /* q_str_charv_of_str, q_str_text_bytes — its strings */
#include "qlang/parse/q_parse.h"
#include "qlang/q_ctx.h"
#include "qlang/q_comment.h"   /* q_comment_origin — a load line's file + line */
#include "qlang/q_console.h"
#include "qlang/q_env.h"
#include "qlang/ops/q_sys.h"
#include "qlang/ops/q_index.h"
#include <string.h>

#define DBG_LIVE_MAX Q_EVAL_DEPTH_CEIL   /* every live frame is stored, the innermost included, q)) reserves too */
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
static _Thread_local int    g_level_base;          /* the first statement level .Q.Ll reports (the runner's are beneath) */
static _Thread_local int    g_frame_num;           /* the number its statement frame shows: 0, or a q)) line's */
static _Thread_local int    g_cursor;              /* navigated frame, 0 = [0] */
/* a `'err` typed at q)) leaves the frames from cut_depth up unobserved: kdb destroys that frame (debug.md §Signal) */
static _Thread_local const ray_t* g_cut_err;
static _Thread_local int    g_cut_depth;

/* file/line: the loading script's, for a load line; 0 for console input (a q))
 * line typed into a suspended load included), so its lambdas and frame stay bare.
 * pos: the statement's caret table (owned), set once its text has parsed; depth: the live frames beneath it;
 * node: its own node, held while a nested run walks (NULL = live). */
typedef struct { char text[DBG_STMT_MAX]; int64_t file, line, ctx; ray_t* pos; int depth, num, hidden; const ray_t* node; } dbg_stmt_t;

static dbg_stmt_t g_stmts[DBG_NEST_MAX];
typedef struct { int live; const dbg_stmt_t* s; int num, synth; } dbg_fr_t;   /* live 0: s's statement, or its `\l` */

/* A statement with no text — a client callback's handler call — has no frame of its own: its first lambda (the
 * handler) is [0] (SO 64109959 `[0]  .z.pg:{value x}`; debug.md trp `[0]  .z.pg:{.Q.trp[…]}`). */
static int stmt_bare(const dbg_stmt_t* s) { return !s->text[0]; }
/* the number live frame idx shows in a statement whose frame [base] is numbered num */
static int frame_number(const dbg_stmt_t* s, int num, int idx, int base) {
    return idx > base ? num + idx - base - stmt_bare(s) : num;
}

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
    ray_t* saved;                  /* `'(msg;bt)`: the retained frame records that replace frames sig and up */
    int    sig, own;                /* own: bt holds the signalling frame itself (.Q.Ll here): its numbers stand */
    int    skip;                   /* live frames between the statement and the stored innermost DBG_SNAP_MAX */
    ray_t* ptext;                  /* a failed parse: its retained text and die offset, frames above the live ones */
    int64_t poff;
    int    pkw;                    /* 1: by `parse`, which has its keyword frame (.q.parse) beneath the text */
} dbg_snap_t;

/* a frame's fields: the one shape the renderer, the record and .Q.sbt share; own = the name's charv when owned */
typedef struct {
    const char *name, *file, *text;
    size_t nn, fn, tn;
    int64_t line, off;
    ray_t* own;
    const dbg_stmt_t* prompt;   /* a statement typed, not loaded: its name item is ("q";`ns) (kdb-common ns.q:179) */
} dbg_frame_t;

/* an error made by `'(msg;bt)` or by `parse`, waiting for the seam that snapshots it */
static _Thread_local struct { ray_t* err; ray_t* bt; int sig, lvl, own; } g_resig;
static _Thread_local struct { ray_t* err; ray_t* text; int64_t off; int kw; } g_pf;

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
static int        g_pnum[DBG_NEST_MAX], g_pbase[DBG_NEST_MAX];   /* the frame number and base the level beneath had */
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

static int is_text(const ray_t* v) { return v->type == RAY_CHARV; }
static int is_view(const ray_t* v) { return v->type == RAY_LIST; }   /* a view recalculation: (text;pos;file;line) */
static ray_t* live_lam(int i);

/* a view's frame, held by the view itself: (text;pos;file;line) of the statement that defined it */
ray_t* q_dbg_view_frame(void) {
    ray_t* text = g_live_depth > g_stmt.depth && g_live_depth <= DBG_LIVE_MAX && is_text(live_lam(g_live_depth))
                      ? live_lam(g_live_depth) : NULL;   /* `value "v::…"`: the text frame's own */
    ray_t* pos = text ? g_live_pos[g_live_depth - 1] : g_stmt.pos;
    ray_t* f = ray_list_new(4);
    if (!f || RAY_IS_ERR(f)) return NULL;
    ray_t* t = text ? text : ray_charv(g_stmt.text, (int64_t)strlen(g_stmt.text));
    f = ray_list_append(f, t);
    if (!text) ray_release(t);
    f = ray_list_append(f, pos ? pos : RAY_NULL_OBJ);
    ray_t* fl = ray_i64(text ? 0 : g_stmt.file), *ln = ray_i64(text ? 0 : g_stmt.line);
    f = ray_list_append(f, fl);
    f = ray_list_append(f, ln);
    ray_release(fl);
    ray_release(ln);
    if (RAY_IS_ERR(f)) return NULL;
    return f;
}

void q_dbg_view_push(ray_t* frame) {
    ray_t* pos = ray_list_get(frame, 1);
    q_dbg_text_push(frame, RAY_IS_NULL(pos) ? NULL : pos);
}

void q_dbg_text_push(ray_t* text, ray_t* pos) {
    q_dbg_frame_push(text);
    if (g_live_depth > DBG_LIVE_MAX) return;
    g_live_pos[g_live_depth - 1] = pos;
    g_live_env[g_live_depth - 1] = Q_ENV_FRAME_VIEW_NONE;   /* a text reads globals only */
}


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
static int g_out;   /* navigated below the suspended statement: the g_out-th frame beneath it (0 = within it) */
static int outer_live(void);

ray_t* q_dbg_self(void) {
    int i = g_sus && g_live_depth == g_frame_base ? (g_out ? outer_live() : g_cursor) : g_live_depth;
    int floor = g_sus && !g_out ? g_sus->base : 0;
    while (i > floor && (is_text(live_lam(i)) || is_view(live_lam(i)))) i--;
    if (i <= floor) return NULL;
    ray_t* lam = live_lam(i);
    ray_retain(lam);
    return lam;
}

/* the caller's node comes back: an iterator calling the lambda again would otherwise push with the body's last */
void q_dbg_frame_pop(void) {
    if (--g_live_depth < DBG_LIVE_MAX) q_eval_node = g_live_call[g_live_depth];
}

#define DBG_LVL_SHIFT 20   /* q_dbg_frame_base's token: the level base above the frame base */
int q_dbg_frame_base(int base) {
    int prev = g_frame_base | g_level_base << DBG_LVL_SHIFT;
    if (base < 0) { g_frame_base = g_live_depth; g_level_base = g_depth; }
    else { g_frame_base = base & ((1 << DBG_LVL_SHIFT) - 1); g_level_base = base >> DBG_LVL_SHIFT; }
    return prev;
}

static void resig_drop(void) {
    if (g_resig.bt) ray_release(g_resig.bt);
    memset(&g_resig, 0, sizeof g_resig);
}

static void pf_drop(void) {
    if (g_pf.text) ray_release(g_pf.text);
    memset(&g_pf, 0, sizeof g_pf);
}

void q_dbg_snapshot_clear(void) {
    for (int i = 0; i < g_snap.depth; i++) ray_release(g_snap.lam[i]);
    if (g_snap.ex) ray_release(g_snap.ex);
    for (int i = 0; i < g_snap.eyn; i++) ray_release(g_snap.ey[i]);
    if (g_snap.saved) ray_release(g_snap.saved);
    if (g_snap.ptext) ray_release(g_snap.ptext);
    memset(&g_snap, 0, sizeof g_snap);
}

static void stmt_pos_drop(dbg_stmt_t* s) {
    if (s->pos) ray_release(s->pos);
    s->pos = NULL;
}

int q_dbg_statement_pos(ray_t* pos) {
    if (g_stmt.pos || stmt_bare(&g_stmt)) return 0;   /* a bare statement's texts are all a handler's */
    g_stmt.pos = pos;
    return 1;
}

void q_dbg_statement_startup(void) { if (g_depth > 0) g_stmt.hidden = 1; }

int q_dbg_statement_begin(const char* src, size_t n, int console) {
    int load_line = console < 0;
    if (load_line) console = g_console_stmt ? 2 : 0;   /* load line: inherit */
    int d = g_depth;
    int top = d > 0 ? frame_number(&g_stmt, g_frame_num, g_live_depth, g_frame_base) : 0;   /* beneath's top frame */
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
    if (d > 0 && d < DBG_NEST_MAX) { g_pnum[d] = g_frame_num; g_pbase[d] = g_frame_base; }
    const dbg_stmt_t* below = &g_stmts[LVL(d - 1)];
    /* a script line numbers past the frames beneath it and the `\l` between (debug.md §Resume: func is [3] under a
     * console `\l test.q`), as a q)) line numbers past the suspension and the debugger's frame; a command-line
     * script is a `\l` with nothing beneath: [0] the `\l`, [1] its line */
    g_stmt.hidden = d > 0 && below->hidden && !g_stmt.file && !strncmp(g_stmt.text, "\\l", 2);
    if (load_line && g_stmt.file && d > 0 && d < DBG_NEST_MAX) {
        g_frame_num = below->hidden ? below->num + 1 : top + 2;
        g_frame_base = g_live_depth;
    }
    g_stmt.num = g_frame_num;
    g_stmt.ctx = q_env_ctx();
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
    pf_drop();
    int d = --g_depth;
    if (g_resig.err && g_resig.lvl == d + 1) {   /* a `'e` at q)) leaves its level carrying the error */
        if (tok & DBG_KEEP_ERR) g_resig.lvl = d;
        else resig_drop();
    }
    /* whatever this level took goes with it: its error is freed now, and a later error at the same address
     * must never read as already seen (the shared slot past DBG_NEST_MAX included) */
    if (g_snap.taken == d + 1) q_dbg_snapshot_clear();
    if (d <= 0 || d >= DBG_NEST_MAX) return;
    g_frame_num = g_pnum[d];
    g_frame_base = g_pbase[d];
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
    resig_drop();
    pf_drop();
    g_live_depth = g_frame_base = g_level_base = g_frame_num = g_cursor = 0;
    g_sus = NULL;
    g_cut_err = NULL;
    memset(g_frozen, 0, sizeof g_frozen);
    for (int i = 0; i < DBG_NEST_MAX; i++) { stmt_pos_drop(&g_stmts[i]); g_stmts[i].node = NULL; g_stmts[i].depth = 0; }
    g_stmt.text[0] = '\0';
    g_stmt.file = 0;
    g_console_stmt = g_trap_depth = g_dbg_depth = g_prompt_frames = g_depth = 0;
    g_reader = NULL;
}

/* a lambda's name, OWNED charv or NULL: the global it is bound to — a display-time scan of its definition namespace,
 * qualified, a root one as `..f` (SO 76408267, kb/profiler.md) — else the name its first assignment stamped (an
 * anonymous inner lambda's `..f@`, basics/debug.md §Errors; a dotted handler's `.z.pg`) */
static ray_t* lam_name(ray_t* lam) {
    ray_t* ctx = NULL;
    q_eval_apply_lambda_parts(lam, NULL, NULL, &ctx);
    int64_t ns = ctx ? ctx->i64 : 0;
    ray_t* names = q_env_ns_names(ns, Q_ENV_NS_FNS);
    ray_t* out = NULL;
    int64_t cnt = names && !RAY_IS_ERR(names) ? q_count(names) : 0;
    for (int64_t i = 0; i < cnt && !out; i++) {
        ray_t* m = q_index_elem_at(names, i);
        if (!m || RAY_IS_ERR(m)) { if (m) ray_release(m); continue; }
        int64_t q = q_env_qualify(ns, m->i64);
        ray_t* s = q >= 0 && q_env_get(q) == lam ? ray_sym_str(q) : NULL;
        if (s && !RAY_IS_ERR(s)) {
            size_t sn = ray_str_len(s);
            char* b = ns ? NULL : (char*)ray_alloc_raw(sn + 2);
            if (ns) out = q_str_charv_of_str(s);
            else if (b) {
                memcpy(b, "..", 2);
                memcpy(b + 2, ray_str_ptr(s), sn);
                out = ray_charv(b, (int64_t)sn + 2);
                ray_free_raw(b);
            }
            ray_release(s);
        }
        ray_release(m);
    }
    if (names) ray_release(names);
    if (out && RAY_IS_ERR(out)) { ray_release(out); out = NULL; }
    if (out) return out;
    q_eval_apply_lambda_prov(lam, &out, NULL, NULL);
    if (out) ray_retain(out);
    return out;
}

/* the fields of a lambda, a `value`d text (charv) or — lam NULL — the statement; a file-less frame's line is 0 */
static void fields_of(dbg_frame_t* f, ray_t* lam, const dbg_stmt_t* stmt, int64_t off) {
    memset(f, 0, sizeof *f);
    f->name = f->file = "";
    f->text = stmt->text;
    f->tn = strlen(stmt->text);
    f->off = off;
    int64_t fs = stmt->file;
    f->line = stmt->line;
    if (lam && is_text(lam)) {
        f->text = (const char*)ray_data(lam);
        f->tn = (size_t)q_count(lam);
        fs = 0;
    } else if (lam && is_view(lam)) {
        ray_t* t = ray_list_get(lam, 0);
        f->text = (const char*)ray_data(t);
        f->tn = (size_t)q_count(t);
        fs = ray_list_get(lam, 2)->i64;
        f->line = ray_list_get(lam, 3)->i64;
    } else if (lam) {
        ray_t* s = q_eval_apply_lambda_src(lam);
        f->text = s ? ray_str_ptr(s) : "{}";
        f->tn = s ? ray_str_len(s) : 2;
        if ((f->own = lam_name(lam))) { f->name = (const char*)ray_data(f->own); f->nn = (size_t)q_count(f->own); }
        q_eval_apply_lambda_prov(lam, NULL, &fs, &f->line);
    }
    ray_t* fp = fs ? ray_sym_str(fs) : NULL;             /* borrowed */
    if (fp) { f->file = ray_str_ptr(fp); f->fn = ray_str_len(fp); }
    else f->line = 0;
}

static void fields_drop(dbg_frame_t* f) { if (f->own) ray_release(f->own); }

/* one frame: `  [n]  file:line: name:text` then its caret line (basics/debug.md — file:line only "if such
 * information is available"; the `^` under the head token at off, a blank line when off is outside the text).  A
 * multi-line text shows the lines through the failing one, the caret, then the rest (KDB-X how-to).  A frame with no
 * text is `(name)` — the trap's own, a keyword's (debug.md `(.Q.trp)`, `(.q.parse)`).  A root `..` never shows.
 * OWNED NUL-terminated text (ray_free_raw), *n its length; NULL on OOM. */
static char* frame_render(size_t* n, const dbg_frame_t* f, int64_t num, int mark) {
    const char* nm = f->name;
    size_t nn = f->nn;
    if (nn > 2 && nm[0] == '.' && nm[1] == '.') { nm += 2; nn -= 2; }
    int paren = !f->tn && nn;
    const char* src = paren ? nm : f->text;
    size_t sn = paren ? nn : f->tn;
    char head[48], ln[32];
    size_t hl = (size_t)snprintf(head, sizeof head, "%s[%lld]  ", mark ? ">>" : "  ", (long long)num);
    size_t ll = f->fn ? (size_t)snprintf(ln, sizeof ln, ":%lld: ", (long long)f->line) : 0;
    size_t named = paren || !nn ? 0 : nn + 1;
    size_t plen = hl + f->fn + ll + named + (size_t)paren;
    int64_t off = f->off >= 0 && (size_t)f->off < sn && !paren ? f->off : -1;
    size_t ls = 0, le = sn;                              /* the failing line's span in src */
    if (off >= 0) {
        for (size_t i = 0; i < (size_t)off; i++) if (src[i] == '\n') ls = i + 1;
        for (le = (size_t)off; le < sn && src[le] != '\n'; le++) ;
    }
    size_t col = off < 0 ? 0 : (ls ? 0 : plen) + ((size_t)off - ls);
    char* dst = (char*)ray_alloc_raw(plen + sn + col + 5);
    if (!dst) return NULL;
    size_t k = 0;
    memcpy(dst, head, hl); k += hl;
    memcpy(dst + k, f->file, f->fn); k += f->fn;
    memcpy(dst + k, ln, ll); k += ll;
    if (named) { memcpy(dst + k, nm, nn); k += nn; dst[k++] = ':'; }
    if (paren) dst[k++] = '(';
    memcpy(dst + k, src, le); k += le;
    if (paren) dst[k++] = ')';
    dst[k++] = '\n';
    memset(dst + k, ' ', col); k += col;
    if (off >= 0) dst[k++] = '^';
    dst[k++] = '\n';
    if (le < sn) { memcpy(dst + k, src + le + 1, sn - le - 1); k += sn - le - 1; dst[k++] = '\n'; }
    dst[k] = '\0';
    *n = k;
    return dst;
}

/* lam NULL = the statement frame (from stmt) */
static char* frame_text(size_t* n, int idx, int mark, ray_t* lam, const dbg_stmt_t* stmt, int off) {
    dbg_frame_t f;
    fields_of(&f, lam, stmt, off);
    char* t = frame_render(n, &f, idx, mark);
    fields_drop(&f);
    return t;
}

static void frame_put(FILE* out, char* text) {
    if (!text) return;
    fputs(text, out);
    ray_free_raw(text);
}

/* ===== the frame record (code;(name;file;line;text);pos;n), built only when asked: .Q.Ll, a trap, a trace ===== */

/* consumes item; a failed item fails the list, so a record is never short (its builder answers 'wsfull) */
static void push(ray_t** l, ray_t* item) {
    if (*l && !RAY_IS_ERR(*l) && item && !RAY_IS_ERR(item)) *l = ray_list_append(*l, item);
    else if (*l && !RAY_IS_ERR(*l)) { ray_release(*l); *l = q_err(QE_WSFULL); }
    if (item) ray_release(item);
}

/* enlist v — item 0, a list whose first is the function (kx-vscode `(.Q.trp ~ first first @) each bt`) */
static ray_t* boxed(ray_t* v) {
    ray_t* c = ray_list_new(1);
    return c && !RAY_IS_ERR(c) ? ray_list_append(c, v ? v : RAY_NULL_OBJ) : c;
}

static ray_t* prompt_name(const dbg_stmt_t* s) {
    ray_t* l = ray_list_new(2);
    push(&l, ray_char((uint8_t)'q'));   /* a char atom: kdb-common compares ("q";`) */
    push(&l, ray_sym(s->ctx));
    return l;
}

static ray_t* rec_new(ray_t* code, const dbg_frame_t* f, int64_t num) {   /* code borrowed */
    ray_t* info = ray_list_new(4);
    push(&info, f->prompt ? prompt_name(f->prompt) : ray_charv(f->name, (int64_t)f->nn));
    push(&info, ray_charv(f->file, (int64_t)f->fn));
    push(&info, ray_i64(f->line));
    push(&info, ray_charv(f->text, (int64_t)f->tn));
    ray_t* r = ray_list_new(4);
    push(&r, boxed(code));
    push(&r, info);
    push(&r, ray_i64(f->off));
    push(&r, ray_i64(num));
    return r;
}

/* a frame of no text: the trap's own, a keyword's; its code is the global `global` names */
static ray_t* rec_named(const char* name, const char* global, int64_t num) {
    dbg_frame_t f = { .name = name, .file = "", .text = "", .nn = strlen(name), .off = -1 };
    return rec_new(q_env_get(ray_sym_intern(global, strlen(global))), &f, num);
}

static ray_t* rec_live(ray_t* lam, const dbg_stmt_t* stmt, int64_t off, int64_t num) {   /* lam NULL = stmt's */
    dbg_frame_t f;
    fields_of(&f, lam, stmt, off);
    if (!lam && !stmt->file) f.prompt = stmt;
    ray_t* code = !lam ? ray_charv(stmt->text, (int64_t)strlen(stmt->text)) : is_view(lam) ? ray_list_get(lam, 0) : lam;
    ray_t* r = rec_new(code, &f, num);
    if (!lam && code) ray_release(code);
    fields_drop(&f);
    return r;
}

static int rec_str(ray_t* v, const char** p, size_t* n) {   /* a string, or () */
    int64_t k = 0;
    if (v && v->type == RAY_LIST && !q_count(v)) { *p = ""; *n = 0; return 1; }
    if (!q_str_text_bytes(v, p, &k)) return 0;
    *n = (size_t)k;
    return 1;
}

static int rec_int(ray_t* v, int64_t* x, int64_t null) {   /* an integer atom, its null read as `null` */
    if (!q_type_is_int_atom(v)) return 0;
    *x = RAY_ATOM_IS_NULL(v) ? null : q_type_iatom_val(v);
    return 1;
}

/* a record's fields (borrowed from it) and number; 0 when it is not (code;(name;file;line;text);pos;n) */
static int rec_fields(ray_t* r, dbg_frame_t* f, int64_t* num) {
    if (!r || r->type != RAY_LIST || q_count(r) != 4) return 0;
    ray_t* info = ray_list_get(r, 1);
    if (!info || info->type != RAY_LIST || q_count(info) != 4) return 0;
    memset(f, 0, sizeof *f);
    if (!rec_str(ray_list_get(info, 0), &f->name, &f->nn)) f->name = "";   /* the prompt's ("q";`) names nothing */
    if (!rec_str(ray_list_get(info, 1), &f->file, &f->fn) || !rec_int(ray_list_get(info, 2), &f->line, 0) ||
        !rec_str(ray_list_get(info, 3), &f->text, &f->tn) || !rec_int(ray_list_get(r, 2), &f->off, -1) ||
        !rec_int(ray_list_get(r, 3), num, 0))
        return 0;
    return 1;
}

static ray_t* rec_renum(ray_t* r, int64_t num) {   /* r borrowed: shared, so the set copies it */
    ray_t* n = ray_i64(num);
    ray_retain(r);
    ray_t* c = ray_list_set(r, 3, n);
    ray_release(n);
    return c;
}

/* `'(msg;bt)` takes a list of records (SO 65303811); anything else is 'stype */
static int rec_list_ok(ray_t* bt) {
    dbg_frame_t f;
    int64_t num;
    if (!bt || bt->type != RAY_LIST) return 0;
    for (int64_t i = 0; i < q_count(bt); i++)
        if (!rec_fields(ray_list_get(bt, i), &f, &num)) return 0;
    return 1;
}

static void snap_take(ray_t* r, ray_t* fv, ray_t** args, int64_t n) {
    g_snap_lvl = LVL(g_depth - 1);   /* this level's own slot; an inherited one stands */
    q_dbg_snapshot_clear();
    g_snap.err = r;
    g_snap.taken = g_depth;
    int stored = g_live_depth < DBG_LIVE_MAX ? g_live_depth : DBG_LIVE_MAX;
    int base   = g_frame_base < stored ? g_frame_base : stored;
    g_snap.skip = stored - base > DBG_SNAP_MAX ? stored - base - DBG_SNAP_MAX : 0;   /* the innermost are kept */
    base += g_snap.skip;
    g_snap.depth = stored - base;
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
    if (r == g_resig.err) {
        g_snap.saved = g_resig.bt;
        g_resig.bt = NULL;
        g_snap.sig = g_resig.sig > base ? g_resig.sig - base : 0;
        g_snap.own = g_resig.own;
        resig_drop();
    }
    if (r == g_pf.err) {
        g_snap.ptext = g_pf.text;
        g_snap.poff = g_pf.off;
        g_snap.pkw = g_pf.kw;
        g_pf.text = NULL;
    }
    pf_drop();
}

int q_dbg_reported(ray_t* err) {
    return g_snap.err == err && g_snap.reported;
}

int q_dbg_prompt_frames(void) { return g_dbg_depth > 0 ? g_prompt_frames : 0; }

static _Thread_local unsigned g_errs_shown;   /* error lines printed: a q)) line's error since the banner shows */

static const char* err_text(ray_t* err, int* n) {
    int64_t tn = 0;
    const char* text = q_err_text(err, &tn);
    *n = (text && tn) ? (int)tn : 4;
    return (text && tn) ? text : "eval";
}

static void dbg_err_line(FILE* out, ray_t* err) {
    g_errs_shown++;
    int n;
    const char* text = err_text(err, &n);
    fprintf(out, "'%.*s\n", n, text);
}

/* The snapshot as records, innermost first, numbered by position up from the statement's [num]: a failed parse's
 * text and `(.q.parse)` frames (debug.md §Trap); a re-signal's saved frames, which REPLACE the signalling frame and
 * stack on the live frames beneath it (SO 65303811); the live frames, with a trap's own frame (trap non-NULL) above
 * the `below` frames that called it (KDB-X safeRun); a bare statement has no frame [0]. */
static ray_t* snap_records(const char* trap, int below) {
    int top = g_snap.depth, lo = stmt_bare(&snap_stmt);
    int64_t ns = g_snap.saved ? q_count(g_snap.saved) : 0;
    if (g_snap.saved && g_snap.own) {   /* the saved list already is this stack: the trap goes beneath the frames it ran */
        int64_t at = g_snap.num + below;
        ray_t* l = ray_list_new(ns + 1);
        int trapped = trap == NULL;
        for (int64_t k = 0; k < ns; k++) {
            ray_t* r = ray_list_get(g_snap.saved, k);
            dbg_frame_t f;
            int64_t n;
            int above = rec_fields(r, &f, &n) && n > at;
            if (!trapped && !above) { push(&l, rec_named(trap, trap, at + 1)); trapped = 1; }
            if (above && !trapped) push(&l, rec_renum(r, n + 1));
            else { ray_retain(r); push(&l, r); }
        }
        if (!trapped) push(&l, rec_named(trap, trap, at + 1));
        return l;
    }
    if (g_snap.saved && g_snap.sig - 1 < top) top = g_snap.sig - 1;
    below -= g_snap.skip;                    /* a trap called beneath the dropped frames sits on the statement */
    int sunk = below < lo && g_snap.skip;
    if (below < lo) below = lo;
    if (below > top) below = top;
    int64_t total = (g_snap.ptext ? 1 + g_snap.pkw : 0) + ns + (top >= lo ? top - lo + 1 : 0) + (trap != NULL);
    int64_t num = g_snap.num + total - 1 + g_snap.skip;
    ray_t* l = ray_list_new(total ? total : 1);
    if (g_snap.ptext) {
        push(&l, rec_live(g_snap.ptext, &snap_stmt, g_snap.poff, num--));
        if (g_snap.pkw) push(&l, rec_named(".q.parse", "parse", num--));
    }
    for (int64_t k = 0; k < ns; k++) push(&l, rec_renum(ray_list_get(g_snap.saved, k), num--));
    int trapped = trap == NULL, skipped = 0;
    for (int i = top; i >= lo; i--) {
        if (i <= below && sunk && !skipped) { num -= g_snap.skip; skipped = 1; }
        if (!trapped && i <= below) { push(&l, rec_named(trap, trap, num--)); trapped = 1; }
        if (!i && !skipped) { num -= g_snap.skip; skipped = 1; }
        push(&l, rec_live(i ? g_snap.lam[i - 1] : NULL, &snap_stmt, i ? g_snap.off[i - 1] : g_snap.stmt_off, num--));
    }
    if (!trapped) push(&l, rec_named(trap, trap, num--));
    return l;
}

static void text_cat(char** buf, size_t* len, char* part, size_t n) {   /* consumes part */
    if (!part) return;
    char* b = *buf ? (char*)ray_realloc_raw(*buf, *len + n + 1) : NULL;
    if (b) { memcpy(b + *len, part, n + 1); *buf = b; *len += n; }
    ray_free_raw(part);
}

/* the records rendered onto buf, each with its own [n]; 0 when one is not a record (a string item prints as a line) */
static int recs_cat(char** buf, size_t* len, ray_t* recs) {
    for (int64_t i = 0; recs && !RAY_IS_ERR(recs) && i < q_count(recs); i++) {
        ray_t* r = ray_list_get(recs, i);
        dbg_frame_t f;
        int64_t num;
        size_t n = 0;
        if (r && r->type == RAY_CHARV) {
            char* line = (char*)ray_alloc_raw((size_t)q_count(r) + 2);
            if (!line) continue;
            memcpy(line, ray_data(r), (size_t)q_count(r));
            line[q_count(r)] = '\n';
            line[q_count(r) + 1] = '\0';
            text_cat(buf, len, line, (size_t)q_count(r) + 1);
            continue;
        }
        if (!rec_fields(r, &f, &num)) return 0;
        char* part = frame_render(&n, &f, num, 0);
        text_cat(buf, len, part, n);
    }
    return 1;
}

char* q_dbg_trace(ray_t* err, int frames, size_t* len) {
    int en;
    const char* et = err_text(err, &en);
    char* buf = (char*)ray_alloc_raw((size_t)en + 3);
    if (!buf) return NULL;
    *len = (size_t)snprintf(buf, (size_t)en + 3, "'%.*s\n", en, et);
    size_t n = 0;
    if (!frames) return buf;
    if (g_snap.err != err && err == g_resig.err) snap_take(err, NULL, NULL, 0);   /* a top-level `'(msg;bt)` */
    if (g_snap.err == err) {
        ray_t* recs = snap_records(NULL, 0);
        recs_cat(&buf, len, recs);
        if (recs) ray_release(recs);
    } else if (!stmt_bare(&g_stmt)) {
        char* part = frame_text(&n, g_frame_num, 0, NULL, &g_stmt, frame_off(g_stmt.pos, stmt_node(&g_stmt)));
        text_cat(&buf, len, part, n);
    }
    return buf;
}

void q_dbg_print_trace(FILE* out, ray_t* err, int frames) {
    g_errs_shown++;
    size_t n = 0;
    char* t = q_dbg_trace(err, frames, &n);
    if (t) fwrite(t, 1, n, out);
    ray_free_raw(t);
    fflush(out);   /* Windows buffers stderr to a file: a worker killed later would lose its trace */
}

/* live frame idx (1-based; base = the statement), numbered from the statement's own [num]: the frames beneath
 * base are its runner's (a transcript line's door, the suspended statement under a q)) line) and never show */
static char* live_text(size_t* n, int idx, int mark, const dbg_stmt_t* stmt, int base, int num) {
    int own = idx > base;
    if (!own && stmt_bare(stmt)) return NULL;
    ray_t* lam = own ? live_lam(idx) : NULL;
    const ray_t* node = own ? lam_node(idx) : stmt_node(stmt);
    return frame_text(n, frame_number(stmt, num, idx, base), mark, lam, stmt,
                      frame_off(own ? live_tab(idx) : stmt->pos, node));
}

static char* fr_text(size_t* n, const dbg_fr_t* f, int mark);
static int outer_frame(int k, dbg_fr_t* hit);

static void dbg_show_frame(int idx) {
    size_t n;
    dbg_fr_t f;
    if (g_out && outer_frame(g_out, &f)) frame_put(stderr, fr_text(&n, &f, 0));
    else frame_put(stderr, live_text(&n, idx, 0, g_sus->stmt, g_sus->base, g_sus->num));
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
    if (g_out) {
        int i = outer_live();
        return i && !is_text(live_lam(i)) && !is_view(live_lam(i)) ? g_live_env[i - 1] : Q_ENV_FRAME_VIEW_NONE;
    }
    if (g_cursor <= g_sus->base || (g_cursor <= DBG_LIVE_MAX && (is_text(live_lam(g_cursor)) || is_view(live_lam(g_cursor)))))
        return Q_ENV_FRAME_VIEW_NONE;
    if (g_cursor >= g_live_depth || g_cursor > DBG_LIVE_MAX)
        return Q_ENV_FRAME_VIEW_OFF;
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

/* the `\d` a frame runs in: a lambda's, where it was written; a statement's or a text's, the session's at the error */
static int64_t frame_ctx(int idx, int64_t session) {
    if (g_out) idx = outer_live();
    ray_t* lam = idx > 0 && idx <= g_live_depth ? live_lam(idx) : NULL;
    ray_t* ctx = NULL;
    if (!lam || is_text(lam) || !q_eval_apply_lambda_parts(lam, NULL, NULL, &ctx)) return session;
    return ctx ? ctx->i64 : 0;
}

/* Returns NULL to propagate err (\ or EOF), a value to resume (:r), or a new error to signal from this frame
 * ('err).  A q)) line reports its own frames only, numbered past the suspended ones and two hidden frames — the
 * debugger's own and the line's — as kdb numbers them (debug.md `{x+y}[a;y]` -> [5] under a [2] suspension). */
static _Thread_local int g_resume_bare;   /* the last suspension ended with a bare `:` */

static ray_t* dbg_suspend(ray_t* err) {
    g_dbg_depth++;
    q_eval_reserve_t depth_cap = q_eval_depth_reserve();   /* at 'limit too, the prompt's lines run */
    int bare = 0;
    int outer_cursor = g_cursor, outer_out = g_out;
    int outer_frames = g_prompt_frames;
    dbg_sus_t sus = { g_frame_base, g_frame_num, g_live_depth, &g_stmt, g_sus };
    q_dbg_hold_t hold = q_dbg_hold();          /* the q)) lines must not move the suspended frame's caret */
    int32_t env_floor = q_env_frame_floor(0);  /* q)) lines reach every suspended frame, a `value`d text's door floors */
    int     app_floor = q_eval_apply_frame_floor(0);
    g_prompt_frames = q_eval_apply_frame_depth();
    g_cursor = g_live_depth;
    g_out = 0;
    g_sus = &sus;
    g_frame_base = sus.top;
    g_frame_num = frame_number(sus.stmt, sus.num, sus.top, sus.base) + 2;
    q_console_flush();
    dbg_banner(err, g_cursor);
    unsigned shown = g_errs_shown;
    int64_t session = q_env_ctx();
    ray_t* out = NULL;
    char line[DBG_LINE_MAX];
    char prompt[Q_SYS_PROMPT_CAP + 16];
    int navigated = 1;   /* a `\d` typed at q)) holds until the next navigation applies the frame's own again */
    for (;;) {
        if (navigated) q_env_ctx_set(frame_ctx(g_cursor, session));   /* each frame's namespace (debug.md §Context) */
        navigated = 0;
        dbg_prompt_str(prompt, sizeof prompt);
        int n = g_reader(prompt, line, sizeof line);
        /* the debug-only tokens (basics/debug.md) peeled first; everything
         * else is an ordinary statement — `\\` included, so it exits the
         * PROCESS as at `q)`.  Lone `\` exits ONE level (ref/debug.md v1). */
        if (n < 0) break;                                 /* EOF: abort */
        if (n == 0) continue;
        if (n == 1 && line[0] == '\\') break;             /* exit one level */
        if (n == 2 && line[0] == '\\' && line[1] == '\\') q_sys_exit(0);   /* never evaluated: it works at the limit */
        /* `.` onto the error frame after another error was shown reprints the banner (Q4M §10.2; KDB-X's plain `.`
         * onto it does not) */
        if (n == 1 && (line[0] == '`' || line[0] == '.' || line[0] == '&')) {   /* up / down / where */
            navigated = 1;
            if (line[0] == '`') {
                if (!g_out && g_cursor > sus.base + stmt_bare(sus.stmt)) g_cursor--;
                else if (outer_frame(g_out + 1, NULL)) g_out++;   /* past [base]: the `\l`, the line beneath */
            } else if (line[0] == '.') {
                if (g_out) g_out--;
                else if (g_cursor < g_live_depth) g_cursor++;
            }
            if (line[0] == '&' || (line[0] == '.' && !g_out && g_cursor == g_live_depth && g_errs_shown != shown)) {
                dbg_banner(err, g_cursor);
                shown = g_errs_shown;
                continue;
            }
            fflush(stdout);
            dbg_show_frame(g_cursor);
            fflush(stderr);
            continue;
        }
        navigated = line[0] == ':' || line[0] == '\'';
        if (line[0] == ':') {                             /* :r — resume */
            ray_t* v = (n == 1) ? (ray_retain(RAY_NULL_OBJ), RAY_NULL_OBJ)
                                : dbg_value_at(line + 1, 0);
            if (v) { out = v; bare = n == 1; break; }
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
    g_resume_bare = bare;
    q_eval_depth_restore(depth_cap);
    q_env_ctx_set(session);
    g_cursor = outer_cursor;
    g_out = outer_out;
    g_prompt_frames = outer_frames;
    g_frame_base = sus.base;
    g_frame_num = sus.num;
    g_sus = sus.outer;
    q_eval_apply_frame_floor(app_floor);
    q_env_frame_floor(env_floor);
    q_dbg_release(hold);
    if (!--g_dbg_depth && g_reader) g_reader(NULL, NULL, 0);   /* a callback's suspension: the console prompt comes back */
    return out;
}

/* "inside a lambda": an apply frame above the prompt's, or a `value`d text (whose door floors the apply frames) run
 * above a lambda frame of the statement's own */
static int dbg_in_lambda(void) {
    if (q_eval_apply_frame_depth() > q_dbg_prompt_frames()) return 1;
    int i = g_live_depth;
    if (i > DBG_LIVE_MAX) return 0;
    while (i > g_frame_base && is_text(live_lam(i))) i--;
    return i > g_frame_base && (i < g_live_depth || is_view(live_lam(i)));
}

/* Local errors ALWAYS suspend (debug.md — no toggle); `\e` gates only whether
 * the REMOTE door marks its statements console (q_ctx.c remote seams). */
static int dbg_suspendable(ray_t* r) {
    return g_reader && g_console_stmt && g_trap_depth == 0 && dbg_in_lambda();
}

/* rep replaces r: a resumed value drops the pending payload, an 'err OWNS it */
static ray_t* dbg_replace(ray_t* r, ray_t* rep) {
    if (!RAY_IS_ERR(rep)) {
        q_err_drop();
        q_dbg_snapshot_clear();
    } else {
        g_cut_err = rep;
        g_cut_depth = g_live_depth;
    }
    ray_error_free(r);
    return rep;
}

static ray_t* dbg_observe(ray_t* r, ray_t* fv, ray_t** args, int64_t n) {
    if (!r || !RAY_IS_ERR(r) || q_err_is(r, QE_RETURN)) return r;
    if (g_snap.err == r) return r;             /* already seen deeper */
    if (r == g_cut_err) {
        if (g_live_depth >= g_cut_depth) return r;
        g_cut_err = NULL;
    }
    snap_take(r, fv, args, n);
    if (!g_snap.saved && dbg_suspendable(r)) {   /* a re-signal's frames are not live: nothing to suspend in */
        ray_t* rep = dbg_suspend(r);
        if (rep) return dbg_replace(r, rep);
        if (g_snap.err != r) snap_take(r, fv, args, n);   /* past DBG_NEST_MAX only */
        g_snap.reported = 1;
    }
    return r;
}

/* An interrupt suspends where it was taken, so resuming continues the interrupted operation (a bare `:`); `:e`
 * substitutes e for that operation as for any error, `\` or a trap sees the 'stop. */
ray_t* q_dbg_stop(void) {
    ray_t* r = q_err(QE_STOP);
    if (!dbg_suspendable(r)) return r;
    snap_take(r, NULL, NULL, 0);
    ray_t* rep = dbg_suspend(r);
    if (!rep) {
        if (g_snap.err != r) snap_take(r, NULL, NULL, 0);
        g_snap.reported = 1;
        return r;
    }
    int bare = g_resume_bare;
    rep = dbg_replace(r, rep);
    if (!bare) return rep;
    ray_release(rep);
    return NULL;
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
    if (!msg || RAY_IS_ERR(msg)) { ray_error_free(r); return msg ? msg : q_err(QE_OOM); }
    if (g_snap.err != r) snap_take(r, NULL, NULL, 0);   /* no frame observed it: `.Q.trp[1;2;g]` */
    ray_t* bt = snap_records(dot ? ".Q.trpd" : ".Q.trp", below);
    ray_error_free(r);
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

/* records render with their own numbers, text and caret — an edited list (kx-vscode) prints what it says */
ray_t* q_dbg_sbt_fn(ray_t* x) {
    if (!x || (x->type != RAY_LIST && !ray_is_vec(x))) return q_err(QE_TYPE);
    char* buf = (char*)ray_alloc_raw(1);
    if (!buf) return q_err(QE_OOM);
    buf[0] = '\0';
    size_t len = 0;
    int ok = x->type != RAY_LIST || recs_cat(&buf, &len, x);
    ray_t* s = ok ? ray_charv(buf, (int64_t)len) : q_err(QE_TYPE);
    ray_free_raw(buf);
    return s;
}

/* bt holds the frame that signals — a .Q.Ll taken right here — so it is already this stack, numbers and all */
static int resig_owns(ray_t* bt) {
    ray_t* top = g_live_depth > g_frame_base && g_live_depth <= DBG_LIVE_MAX ? live_lam(g_live_depth) : NULL;
    size_t tn = strlen(g_stmt.text);
    for (int64_t i = 0; i < q_count(bt); i++) {
        ray_t* code = ray_list_get(ray_list_get(bt, i), 0);
        ray_t* c = code && code->type == RAY_LIST && q_count(code) ? ray_list_get(code, 0) : NULL;
        if (!c) continue;
        if (top ? c == top : c->type == RAY_CHARV && (size_t)q_count(c) == tn && !memcmp(ray_data(c), g_stmt.text, tn))
            return 1;
    }
    return 0;
}

/* `'(msg;bt)`: msg signalled carrying bt, which the snapshot of this error adopts */
ray_t* q_dbg_resignal(ray_t* msg, ray_t* bt) {
    if (!rec_list_ok(bt)) return q_err(QE_STYPE);
    ray_t* r = q_err_signal(QE_SIGNAL, msg);
    resig_drop();
    ray_retain(bt);
    g_resig.err = r;
    g_resig.bt = bt;
    g_resig.sig = g_live_depth;
    g_resig.lvl = g_depth;
    g_resig.own = resig_owns(bt);
    return r;
}

void q_dbg_parse_fail(ray_t* err, const char* src, int64_t n, ray_t* pos, int keyword) {
    pf_drop();
    g_pf.text = ray_charv(src, n);
    if (!g_pf.text || RAY_IS_ERR(g_pf.text)) { if (g_pf.text) ray_release(g_pf.text); g_pf.text = NULL; return; }
    g_pf.err = err;
    g_pf.off = frame_off(pos, NULL);
    g_pf.kw = keyword;
}

/* The live stack (debug.md §Context: "system commands, including \l, correspond to individual debug stack frames"):
 * every statement level from lv down to the runner's floor — its lambda, text and view frames below hi, its
 * statement frame, and beneath a script's line the `\l` that loads it (finos_init.q reads it), numbered as the
 * printed frames are.  A startup container and its typed `\l` line are no frames. */
typedef int (*dbg_fr_fn)(const dbg_fr_t* f, void* u);                          /* nonzero stops the walk */

static int lvl_of(const dbg_stmt_t* s) { return (int)(s - g_stmts); }

static void fr_walk(int lv, int hi, dbg_fr_fn fn, void* u) {
    int floor = LVL(g_level_base);
    for (; g_depth > 0 && lv >= floor; lv--) {
        const dbg_stmt_t* s = &g_stmts[lv];
        dbg_fr_t f = { 0, s, 0, 0 };
        for (int i = hi < DBG_LIVE_MAX ? hi : DBG_LIVE_MAX; i > s->depth; i--) {
            f.live = i;
            f.num = frame_number(s, s->num, i, s->depth);
            if (fn(&f, u)) return;
        }
        hi = s->depth;
        f.live = 0;
        f.num = s->num;
        if (!stmt_bare(s) && !s->hidden && fn(&f, u)) return;
        f.num = s->num - 1;
        f.synth = 1;
        if (s->file && lv > floor && fn(&f, u)) return;
    }
}

/* a script line's `\l <file>` frame text: OWNED (ray_free_raw), *n its length */
static char* load_text(const dbg_stmt_t* s, size_t* n) {
    ray_t* fp = ray_sym_str(s->file);   /* borrowed */
    size_t fn = fp ? ray_str_len(fp) : 0;
    char* t = (char*)ray_alloc_raw(fn + 4);
    if (!t) return NULL;
    memcpy(t, "\\l ", 3);
    if (fn) memcpy(t + 3, ray_str_ptr(fp), fn);
    t[fn + 3] = '\0';
    *n = fn + 3;
    return t;
}

static char* fr_text(size_t* n, const dbg_fr_t* f, int mark) {
    if (f->live) return frame_text(n, f->num, mark, live_lam(f->live), f->s, frame_off(live_tab(f->live), lam_node(f->live)));
    if (!f->synth) return frame_text(n, f->num, mark, NULL, f->s, frame_off(f->s->pos, stmt_node(f->s)));
    size_t tn = 0;
    char* t = load_text(f->s, &tn);
    if (!t) return NULL;
    dbg_frame_t d = { .name = "", .file = "", .text = t, .tn = tn, .off = -1 };
    char* out = frame_render(n, &d, f->num, mark);
    ray_free_raw(t);
    return out;
}

static ray_t* fr_rec(const dbg_fr_t* f) {
    if (f->live) return rec_live(live_lam(f->live), f->s, frame_off(live_tab(f->live), lam_node(f->live)), f->num);
    if (!f->synth) return rec_live(NULL, f->s, frame_off(f->s->pos, stmt_node(f->s)), f->num);
    size_t tn = 0;
    char* t = load_text(f->s, &tn);
    if (!t) return q_err(QE_WSFULL);
    dbg_frame_t d = { .name = "", .file = "", .text = t, .tn = tn, .off = -1 };
    ray_t* code = ray_charv(t, (int64_t)tn);
    ray_t* r = rec_new(code, &d, f->num);
    if (code) ray_release(code);
    ray_free_raw(t);
    return r;
}

static int fr_count(const dbg_fr_t* f, void* u) { (void)f; ++*(int64_t*)u; return 0; }
static int fr_push(const dbg_fr_t* f, void* u) { push((ray_t**)u, fr_rec(f)); return 0; }

static ray_t* ll_list(void) {
    int64_t total = 0;
    fr_walk(LVL(g_depth - 1), g_live_depth, fr_count, &total);
    ray_t* l = ray_list_new(total ? total : 1);
    fr_walk(LVL(g_depth - 1), g_live_depth, fr_push, &l);
    return l;
}

/* the frames beneath the suspended statement, reached by `` ` `` past it: its `\l`, the console line, outer levels */
typedef struct { int want, seen; dbg_fr_t hit; } fr_pick_t;
static int fr_pick(const dbg_fr_t* f, void* u) {
    fr_pick_t* p = (fr_pick_t*)u;
    if (f->s == g_sus->stmt && !f->synth) return 0;
    if (++p->seen == p->want) { p->hit = *f; return 1; }
    return 0;
}

static int outer_frame(int k, dbg_fr_t* hit) {
    fr_pick_t p = { k, 0, { 0, NULL, 0, 0 } };
    fr_walk(lvl_of(g_sus->stmt), g_sus->top, fr_pick, &p);
    if (p.seen < k) return 0;
    if (hit) *hit = p.hit;
    return 1;
}

static int outer_live(void) {
    dbg_fr_t f;
    return g_out && outer_frame(g_out, &f) ? f.live : 0;
}

ray_t* q_dbg_ll_fn(ray_t* x) {
    (void)x;                                   /* .Q.Ll` / .Q.Ll[] — any argument */
    return ll_list();
}

ray_t* q_dbg_btx_fn(ray_t* x) {
    if (!x || x->type != RAY_LIST) return q_err(QE_TYPE);
    ray_retain(x);
    return x;
}

/* .Q.bt[]: the live stack, every level down to [0], >> on the navigated frame.  At q)) the line's own statement
 * frame is hidden, as the page shows none for `.Q.bt[]` typed there. */
typedef struct { const dbg_stmt_t* hide; int in, outer; } fr_bt_t;
static int fr_print(const dbg_fr_t* f, void* u) {
    fr_bt_t* b = (fr_bt_t*)u;
    if (f->s == b->hide && !f->live && !f->synth) return 0;
    int own = g_sus && f->s == g_sus->stmt && !f->synth, mark = 0;
    if (own) b->in = 1;
    if (own && !g_out) mark = f->live ? f->live == g_cursor : g_cursor == g_sus->base;
    else if (g_sus && g_out && b->in && !own) mark = ++b->outer == g_out;
    size_t n = 0;
    char* t = fr_text(&n, f, mark);
    if (t) { q_console_write(t, n, false); ray_free_raw(t); }
    return 0;
}

ray_t* q_dbg_bt_fn(ray_t** args, int64_t n) {
    (void)args; (void)n;                       /* .Q.bt[] — arg ignored */
    fr_bt_t b = { g_sus ? &g_stmt : NULL, 0, 0 };
    fr_walk(LVL(g_depth - 1), g_live_depth, fr_print, &b);
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
