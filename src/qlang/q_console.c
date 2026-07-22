/* q_console — console sink buffer + the `--nonlegacy` pipe-table mode.  See
 * q_console.h for the contract; the value->string core stays in q_fmt.c. */
#include "qlang/q_console.h"
#include "qlang/q_fmt.h"               /* q_fmt_console — show's render; q_fmt — digest atoms */
#include "core/ipc.h"                  /* ray_ipc_current_handle — handler write-through */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the console sink buffer --------------------------------------------- */

static char*  g_console;
static size_t g_console_len, g_console_cap;

void q_console_reset(void) { g_console_len = 0; if (g_console) g_console[0] = '\0'; }

const char* q_console_str(void) { return g_console ? g_console : ""; }

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
static int32_t g_con_trunc = 1;                   /* 0 = unlimited, 1 = clip by `\c` */

bool q_console_clip(int32_t* rows, int32_t* cols) {
    if (rows) *rows = g_con_rows;
    if (cols) *cols = g_con_cols;
    return g_con_trunc != 0;
}

void q_console_clip_set(int64_t rows, int64_t cols) {
    g_con_rows = (int32_t)(rows < 10 ? 10 : rows > 2000 ? 2000 : rows);
    g_con_cols = (int32_t)(cols < 10 ? 10 : cols > 2000 ? 2000 : cols);
    g_con_trunc = 1;
}
