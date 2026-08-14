/* q_console — console sink buffer + the modern pipe-table mode.  See
 * q_console.h for the contract; the value->string core stays in q_fmt.c. */
#include "qlang/q_console.h"
#include "qlang/q_fmt.h"               /* q_fmt_console — show's render; q_fmt — digest atoms */
#include "core/ipc.h"                  /* ray_ipc_current_handle — handler write-through */
#include "core/platform.h"             /* RAY_OS_WINDOWS — the `\c 0N` terminal query */
#include "qlang/q_env.h"               /* q_env_bind — the .pq.i.termsize native */
#include "lang/env.h"                  /* ray_fn_unary */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(RAY_OS_WINDOWS)
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

/* ---- the console sink buffer --------------------------------------------- */

static char*  g_console;
static size_t g_console_len, g_console_cap;

void q_console_reset(void) { g_console_len = 0; if (g_console) g_console[0] = '\0'; }

const char* q_console_str(void) { return g_console ? g_console : ""; }

/* The host drains this buffer BETWEEN statements, so text a statement emits
 * before it exits the process would die with it (peachq issue #23). */
void q_console_flush(void) {
    if (!g_console_len) return;
    fwrite(g_console, 1, g_console_len, stdout);
    fflush(stdout);
    q_console_reset();
}

static void console_append(const char* s, size_t n) {
    if (g_console_len + n + 1 > g_console_cap) {
        size_t nc = g_console_cap ? g_console_cap * 2 : 256;
        while (nc < g_console_len + n + 1) nc *= 2;
        char* nb = realloc(g_console, nc);
        if (!nb) return;                       /* drop on OOM — best effort */
        g_console = nb; g_console_cap = nc;
    }
    memcpy(g_console + g_console_len, s, n);
    g_console_len += n;
    g_console[g_console_len] = '\0';
}

/* g_console buffer for the host to drain; in an IPC handler (no host drain) straight to stdout, kdb's server-console behaviour. */
static void console_emit(const char* s, size_t n) {
    if (ray_ipc_current_handle() >= 0) {
        fwrite(s, 1, n, stdout);
        fflush(stdout);
    } else {
        console_append(s, n);
    }
}

void q_console_show(ray_t* val) {
    char buf[8192]; buf[0] = '\0';
    q_fmt_console(val, buf, sizeof buf);   /* `show` obeys the `\c` display clip */
    console_emit(buf, strlen(buf));
    console_emit("\n", 1);
}

void q_console_write(const char* s, size_t n) { console_emit(s, n); }


/* ---- pipe-mode STATE (the renderer lives in q_fmt.c — a formatting mode) ---- */

static bool g_pipe_on;
void q_console_pipe_enable(void)  { g_pipe_on = true; }
void q_console_pipe_disable(void) { g_pipe_on = false; }
bool q_console_pipe_on(void)      { return g_pipe_on; }


/* ---- `\c` console DISPLAY clip STATE (config beside the sink) ------------- */

static int32_t g_con_rows = 25, g_con_cols = 80;  /* live `\c` size (default 25 80) */
static bool    g_con_rows_auto, g_con_cols_auto;  /* `0N` axes: fit the live terminal */
static int32_t g_con_trunc = 1;                   /* 0 = unlimited, 1 = clip by `\c` */

static int32_t clip_coerce(int64_t v) {
    return (int32_t)(v < 10 ? 10 : v > 2000 ? 2000 : v);
}

/* Live terminal size for the `\c 0N` auto axes, re-queried at every render so a
 * window resize is picked up with no SIGWINCH plumbing.  No tty / failed query
 * falls back to the classic 25 80, and the [10,2000] coercion guards a
 * degenerate 0x0 report — the console can never clip itself unusable. */
static void clip_term_size(int32_t* rows, int32_t* cols) {
    int64_t r = 25, c = 80;
#if defined(RAY_OS_WINDOWS)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &csbi)) {
        r = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        c = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
#else
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 && w.ws_col > 0) {
        r = w.ws_row;
        c = w.ws_col;
    }
#endif
    *rows = clip_coerce(r);
    *cols = clip_coerce(c);
}

bool q_console_clip(int32_t* rows, int32_t* cols) {
    int32_t r = g_con_rows, c = g_con_cols;
    if (g_con_rows_auto || g_con_cols_auto) {
        int32_t tr, tc;
        clip_term_size(&tr, &tc);
        if (g_con_rows_auto) r = tr;
        if (g_con_cols_auto) c = tc;
    }
    if (rows) *rows = r;
    if (cols) *cols = c;
    return g_con_trunc != 0;
}

void q_console_clip_set(int64_t rows, int64_t cols) {
    g_con_rows_auto = (rows == NULL_I64);
    g_con_cols_auto = (cols == NULL_I64);
    if (!g_con_rows_auto) g_con_rows = clip_coerce(rows);
    if (!g_con_cols_auto) g_con_cols = clip_coerce(cols);
    g_con_trunc = 1;
}

void q_console_clip_setting(int64_t* rows, int64_t* cols) {
    if (rows) *rows = g_con_rows_auto ? NULL_I64 : g_con_rows;
    if (cols) *cols = g_con_cols_auto ? NULL_I64 : g_con_cols;
}

/* `.pq.i.termsize[]` — the LIVE terminal (rows;cols;tty), same query + 25/80
 * fallback + [10,2000] coercion as the `\c 0N` auto axes, plus whether stdout
 * IS a terminal (0/1); the q side (help preview, .pq.cancolor) cannot reach
 * ioctl/isatty any other way. */
static ray_t* termsize_fn(ray_t* x) {
    (void)x;
    int32_t r, c;
    clip_term_size(&r, &c);
#if defined(RAY_OS_WINDOWS)
    int64_t tty = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR;
#else
    int64_t tty = isatty(STDOUT_FILENO) == 1;
#endif
    ray_t* v = ray_vec_new(RAY_I64, 3);
    if (RAY_IS_ERR(v)) return v;
    int64_t rr = r, cc = c;
    v = ray_vec_append(v, &rr);
    if (RAY_IS_ERR(v)) return v;
    v = ray_vec_append(v, &cc);
    if (RAY_IS_ERR(v)) return v;
    return ray_vec_append(v, &tty);
}

void q_console_pq_register(void) {
    static const char nm[] = ".pq.i.termsize";
    ray_t* obj = ray_fn_unary(nm, RAY_FN_NONE, termsize_fn);
    q_env_bind(ray_sym_intern(nm, strlen(nm)), obj);
    ray_release(obj);
}
