/* q_console — the console sink + the modern pipe-table mode.  See
 * q_console.h for the contract; the value->string core stays in q_fmt.c. */
#include "qlang/q_count.h"
#include "qlang/q_console.h"
#include "qlang/q_fmt.h"               /* q_fmt_console_alloc — show's render */
#include "qlang/io/q_io.h"             /* q_io_fwrite / q_io_write_fd — the byte writers */
#include "core/ipc.h"                  /* ray_ipc_current_handle — a handler writes the server console */
#include "core/platform.h"             /* RAY_OS_WINDOWS — the `\c 0N` terminal query; RAY_TLS */
#include "qlang/q_env.h"               /* q_env_bind — the .pq.i.termsize/.pq.i.cancolor natives */
#include "lang/env.h"                  /* ray_fn_unary */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(RAY_OS_WINDOWS)
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

/* ---- the console sink: write-through, one lock, whole writes ------------- */

static _Atomic(int)  g_con_lock;
static RAY_TLS FILE* g_door;   /* per thread: a worker writes the process stdout */

static void con_lock(void) {
    while (atomic_exchange_explicit(&g_con_lock, 1, memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}
static void con_unlock(void) { atomic_store_explicit(&g_con_lock, 0, memory_order_release); }

void q_console_forked(void) {
    atomic_store_explicit(&g_con_lock, 0, memory_order_relaxed);
    g_door = NULL;
}

FILE* q_console_door(FILE* out) {
    FILE* prev = g_door;
    g_door = out;
    return prev;
}

/* An IPC handler writes the SERVER console whichever door is open (kdb). */
static FILE* con_out(void) {
    return ray_ipc_current_handle() >= 0 || !g_door ? stdout : g_door;
}

void q_console_flush(void) {
    con_lock();
    fflush(con_out());
    fflush(stdout);
    con_unlock();
}

/* A stdout stream error is not the statement's to escalate: a closed pipe drops the display, as it always has. */
int q_console_write(const char* s, size_t n, bool nl) {
    FILE* f = con_out();
    con_lock();
    if (!q_io_fwrite(f, s, n) && nl) fputc('\n', f);
    fflush(f);
    con_unlock();
    return 0;
}

/* Raw fd 2, so `\2` redirects it; stdout is flushed first so the two streams keep issue order. */
int q_console_write_err(const char* s, size_t n, bool nl) {
    FILE* f = con_out();
    con_lock();
    fflush(f);
    if (f != stdout) fflush(stdout);
    int bad = q_io_write_fd(2, s, n) || (nl && q_io_write_fd(2, "\n", 1));
    con_unlock();
    return bad ? -1 : 0;
}

int q_console_show(ray_t* val) {
    size_t n;
    char*  txt = q_fmt_console_alloc(val, &n);   /* `show` obeys the `\c` display clip */
    if (!txt) return -1;
    int rc = q_console_write(txt, n, true);
    free(txt);
    return rc;
}


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

bool q_console_color(bool tty) {
    const char* pc = getenv("PEACHQ_COLORS");
    const char* nc = getenv("NO_COLOR");
    const char* fc = getenv("FORCE_COLOR");
    const char* t  = getenv("TERM");
    if (pc && (!strcmp(pc, "1") || !strcmp(pc, "0"))) return pc[0] == '1';
    if (nc && *nc) return false;
    if (fc && *fc) return true;
    return tty && !(t && !strcmp(t, "dumb"));
}

/* `.pq.i.termsize[]` — the LIVE terminal (rows;cols), same query + 25/80 fallback + [10,2000] coercion as the `\c 0N`
 * auto axes; the q side (help preview, .pq.termsize) cannot reach ioctl any other way. */
static ray_t* termsize_fn(ray_t* x) {
    (void)x;
    int32_t r, c;
    clip_term_size(&r, &c);
    ray_t* v = ray_vec_new(RAY_I64, 2);
    if (RAY_IS_ERR(v)) return v;
    int64_t rr = r, cc = c;
    v = ray_vec_append(v, &rr);
    if (RAY_IS_ERR(v)) return v;
    return ray_vec_append(v, &cc);
}

/* `.pq.i.cancolor[]` — the colour law for stdout, which q cannot ask isatty about. */
static ray_t* cancolor_fn(ray_t* x) {
    (void)x;
#if defined(RAY_OS_WINDOWS)
    bool tty = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR;
#else
    bool tty = isatty(STDOUT_FILENO) == 1;
#endif
    return ray_bool(q_console_color(tty));
}

static void console_bind(const char* nm, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(nm, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(nm, strlen(nm)), obj);
    ray_release(obj);
}
void q_console_pq_register(void) {
    console_bind(".pq.i.termsize", termsize_fn);
    console_bind(".pq.i.cancolor", cancolor_fn);
}
void q_console_help_register(void) { console_bind(".help.i.termsize", termsize_fn); }
