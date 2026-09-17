/* q_termbox — see q_termbox.h.  ONE session at a time; xterm-only by decision
 * (actionable-plans/2026-09-15-tui-option-c-spike.md): every terminal we
 * target speaks xterm, so there is no terminfo and one key table serves
 * Linux, mac and Win 11 (VT input mode hands over the same bytes).
 *
 * The tty contract: init snapshots the termios / console mode IT FOUND — the
 * REPL's eval window, raw with ISIG on — and every restore puts THAT back,
 * never "sane".  ISIG stays as found, so Ctrl-C remains the interrupt path
 * ('stop), never a key.  Restore fires from shutdown, from the statement seam
 * on any error / 'stop (q_ctx_set_tty_restore) and from the exit path, so a
 * q program never has to.  The platform recipe is src/app/term.c's; that
 * struct IS the line editor and is never called from here.
 *
 * A cell is codepoint + fg + bg.  A style long packs disjoint fields so `+`
 * composes: bits 0-8 palette (0 default, 1-256 = xterm 0-255), bit 9 rgb flag
 * with rgb24 in bits 10-33, attributes from bit 40 — ONE packing for every
 * output mode; the mode only decides how the emitter degrades it.
 *
 * Output goes straight to the tty fd: the console buffer (q_console.c) drains
 * between statements, and a game loop lives inside one. */
#define _GNU_SOURCE
#include "qlang/q_count.h"
#include "qlang/io/q_termbox.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"
#include "qlang/q_ctx.h"          /* q_ctx_set_tty_restore — the statement-seam restore */
#include "qlang/q_env.h"
#include "qlang/q_prim.h"         /* q_str_text_bytes */
#include "qlang/io/q_io.h"        /* q_io_path_operand — the headless door's two paths */
#include "qlang/ops/q_index.h"    /* q_index_elem_at — rows and per-row style reads */
#include "app/term.h"             /* ray_term_visual_width — THE cell-width owner */
#include "core/platform.h"
#include "lang/env.h"             /* ray_fn_unary / ray_fn_vary */
#include "lang/eval.h"            /* RAY_FN_NONE */
#include "table/sym.h"            /* ray_sym_vec_new */
#include <rayforce.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(RAY_OS_WINDOWS)
#include <windows.h>
#include <io.h>
#else
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

enum { TB_ESC_MS = 50, TB_IBUF = 256 };
enum { TB_MODE_MONO, TB_MODE_16, TB_MODE_256, TB_MODE_TRUE };
enum { TB_IN_MOUSE = 4 };
enum { TB_PAL_MASK = 0x1FF, TB_RGB_FLAG = 1 << 9, TB_RGB_SHIFT = 10, TB_ATTR_SHIFT = 40 };

typedef struct { uint32_t ch; int64_t fg, bg; } tb_cell;

static struct {
    int      active, headless, invalid, pending_resize;
    int      w, h;
    int64_t  cx, cy;
    int      in_mode, out_mode, cap;
    int64_t  t0_ms;
    tb_cell* back;
    tb_cell* front;
    unsigned char ibuf[TB_IBUF];
    int      ilen;
    char*    obuf;
    size_t   olen, ocap;
    int      fd_in, fd_out;
#if defined(RAY_OS_WINDOWS)
    HANDLE   h_in, h_out;
    DWORD    in_mode0, out_mode0;
    UINT     out_cp0;
#else
    struct termios saved;
#endif
} tb;

/* ---- the transport ------------------------------------------------------- */

static void raw_write(const void* p, size_t n) {
#if defined(RAY_OS_WINDOWS)
    if (tb.headless) { _write(tb.fd_out, p, (unsigned)n); return; }
    DWORD w;
    WriteFile(tb.h_out, p, (DWORD)n, &w, NULL);
#else
    const char* s = (const char*)p;
    while (n) {
        ssize_t w = write(tb.fd_out, s, n);
        if (w <= 0) return;
        s += w; n -= (size_t)w;
    }
#endif
}

static void out_bytes(const char* p, size_t n) {
    if (tb.olen + n > tb.ocap) {
        size_t nc = tb.ocap ? tb.ocap * 2 : 4096;
        while (nc < tb.olen + n) nc *= 2;
        char* nb = realloc(tb.obuf, nc);
        if (!nb) return;
        tb.obuf = nb; tb.ocap = nc;
    }
    memcpy(tb.obuf + tb.olen, p, n);
    tb.olen += n;
}

static void out_str(const char* s) { out_bytes(s, strlen(s)); }
static void out_flush(void) { raw_write(tb.obuf, tb.olen); tb.olen = 0; }

int q_termbox_emit(const char* p, size_t n) {
    if (!tb.active) return 0;
    out_bytes(p, n);
    out_flush();
    return 1;
}

/* 1 = bytes waiting, 0 = timeout / EOF.  A headless input is a file: always
 * readable, EOF = nothing will ever arrive, so it never waits. */
static int input_wait(int ms) {
    if (tb.headless) return 1;
#if defined(RAY_OS_WINDOWS)
    /* the handle signals on ANY record (key-up, focus, mouse, size) but ReadFile waits for BYTES:
     * discard byte-less records and keep waiting out the deadline, else the first frame stalls */
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(ms < 0 ? 0 : ms);
    for (;;) {
        DWORD left = ms < 0 ? INFINITE : (DWORD)(deadline > GetTickCount64() ? deadline - GetTickCount64() : 0);
        if (WaitForSingleObject(tb.h_in, left) != WAIT_OBJECT_0) return 0;
        INPUT_RECORD rec[64];
        DWORD n = 0, got = 0;
        if (!GetNumberOfConsoleInputEvents(tb.h_in, &n) || n == 0) return 1;
        if (n > 64) n = 64;
        if (!PeekConsoleInput(tb.h_in, rec, n, &got) || got == 0) return 1;
        for (DWORD i = 0; i < got; i++)
            if (rec[i].EventType == KEY_EVENT && rec[i].Event.KeyEvent.bKeyDown) return 1;
        ReadConsoleInput(tb.h_in, rec, got, &got);
        if (ms >= 0 && GetTickCount64() >= deadline) return 0;
    }
#else
    struct pollfd p = { tb.fd_in, POLLIN, 0 };
    return poll(&p, 1, ms) > 0;
#endif
}

static int input_fill(int ms) {
    if (tb.ilen >= TB_IBUF || !input_wait(ms)) return 0;
    int room = TB_IBUF - tb.ilen;
#if defined(RAY_OS_WINDOWS)
    DWORD n = 0;
    if (tb.headless) n = (DWORD)_read(tb.fd_in, tb.ibuf + tb.ilen, (unsigned)room);
    else if (!ReadFile(tb.h_in, tb.ibuf + tb.ilen, (DWORD)room, &n, NULL)) n = 0;
#else
    ssize_t n = read(tb.fd_in, tb.ibuf + tb.ilen, (size_t)room);
    if (n < 0) n = 0;
#endif
    tb.ilen += (int)n;
    return (int)n;
}

static void input_consume(int n) {
    memmove(tb.ibuf, tb.ibuf + n, (size_t)(tb.ilen - n));
    tb.ilen -= n;
}

/* ---- the platform door --------------------------------------------------- */

static void probe_size(int* w, int* h) {
    *w = 80; *h = 24;
#if defined(RAY_OS_WINDOWS)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(tb.h_out, &csbi)) {
        *w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        *h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(tb.fd_out, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    }
#endif
}

/* .pq.cancolor's law (lib/pq.q): PEACHQ_COLORS / NO_COLOR / FORCE_COLOR outrank TERM */
static int detect_cap(void) {
    const char* pc = getenv("PEACHQ_COLORS");
    const char* ct = getenv("COLORTERM");
    const char* t  = getenv("TERM");
    int forced = (pc && !strcmp(pc, "1")) || getenv("FORCE_COLOR");
    if ((pc && !strcmp(pc, "0")) || getenv("NO_COLOR")) return TB_MODE_MONO;
    if (ct && (!strcmp(ct, "truecolor") || !strcmp(ct, "24bit"))) return TB_MODE_TRUE;
    if (t && strstr(t, "256color")) return TB_MODE_256;
    if (t && !strcmp(t, "dumb") && !forced) return TB_MODE_MONO;
    return TB_MODE_16;
}

static int tty_open(void) {
#if defined(RAY_OS_WINDOWS)
    tb.h_in  = GetStdHandle(STD_INPUT_HANDLE);
    tb.h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(tb.h_in, &tb.in_mode0) || !GetConsoleMode(tb.h_out, &tb.out_mode0)) return 0;
    DWORD m = tb.in_mode0 & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    SetConsoleMode(tb.h_in, m | ENABLE_VIRTUAL_TERMINAL_INPUT);
    SetConsoleMode(tb.h_out, tb.out_mode0 | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    tb.out_cp0 = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
#else
    tb.fd_in = STDIN_FILENO;
    tb.fd_out = STDOUT_FILENO;
    if (!isatty(tb.fd_in) || !isatty(tb.fd_out) || tcgetattr(tb.fd_in, &tb.saved) != 0) return 0;
    struct termios raw = tb.saved;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(tb.fd_in, TCSANOW, &raw);
#endif
    return 1;
}

static void tty_close(void) {
#if defined(RAY_OS_WINDOWS)
    SetConsoleMode(tb.h_in, tb.in_mode0);
    SetConsoleMode(tb.h_out, tb.out_mode0);
    if (tb.out_cp0) SetConsoleOutputCP(tb.out_cp0);
#else
    tcsetattr(tb.fd_in, TCSANOW, &tb.saved);
#endif
}

static int headless_open(const char* in, const char* out) {
#if defined(RAY_OS_WINDOWS)
    tb.fd_in  = _open(in, _O_RDONLY | _O_BINARY);
    tb.fd_out = _open(out, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
    if (tb.fd_in < 0 || tb.fd_out < 0) { if (tb.fd_in >= 0) _close(tb.fd_in); if (tb.fd_out >= 0) _close(tb.fd_out); return 0; }
#else
    tb.fd_in  = open(in, O_RDONLY);
    tb.fd_out = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tb.fd_in < 0 || tb.fd_out < 0) { if (tb.fd_in >= 0) close(tb.fd_in); if (tb.fd_out >= 0) close(tb.fd_out); return 0; }
#endif
    return 1;
}

static void headless_close(void) {
#if defined(RAY_OS_WINDOWS)
    _close(tb.fd_in); _close(tb.fd_out);
#else
    close(tb.fd_in); close(tb.fd_out);
#endif
}

/* ---- the cell buffers ---------------------------------------------------- */

static void cells_clear(tb_cell* c, int n) {
    for (int i = 0; i < n; i++) { c[i].ch = ' '; c[i].fg = 0; c[i].bg = 0; }
}

/* (re)size both buffers; what was drawn survives a resize in the overlap */
static int cells_alloc(int w, int h) {
    tb_cell* b = calloc((size_t)(w * h) * 2, sizeof *b);
    if (!b) return 0;
    cells_clear(b, 2 * w * h);
    for (int y = 0; tb.back && y < h && y < tb.h; y++)
        for (int x = 0; x < w && x < tb.w; x++) b[y * w + x] = tb.back[y * tb.w + x];
    free(tb.back);
    tb.back = b; tb.front = b + w * h;
    tb.w = w; tb.h = h;
    tb.invalid = 1;
    return 1;
}

/* int64 coords, compared before any narrowing: a huge q long is off-screen, never a wrapped index */
static void cell_set(int64_t x, int64_t y, uint32_t ch, int64_t fg, int64_t bg) {
    if (x < 0 || y < 0 || x >= tb.w || y >= tb.h) return;
    tb_cell* c = &tb.back[y * tb.w + x];
    c->ch = ch; c->fg = fg; c->bg = bg;
}

static void mouse_track(int on) {
    out_str(on ? "\033[?1000h\033[?1002h\033[?1003h\033[?1006h" : "\033[?1006l\033[?1003l\033[?1002l\033[?1000l");
}

static void session_close(void) {
    if (!tb.active) return;
    tb.olen = 0;
    if (tb.in_mode & TB_IN_MOUSE) mouse_track(0);
    out_str("\033[0m\033[?25h\033[?1049l");
    out_flush();
    if (tb.headless) headless_close(); else tty_close();
    free(tb.back); tb.back = tb.front = NULL;
    free(tb.obuf); tb.obuf = NULL; tb.ocap = 0;
    tb.active = 0;
}

static int64_t mono_ms(void) {
#if defined(RAY_OS_WINDOWS)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart * 1000 / f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static ray_t* session_open(int headless, int w, int h) {
    if (!cells_alloc(w, h)) { if (headless) headless_close(); else tty_close(); return q_err(QE_WSFULL); }
    tb.headless = headless;
    tb.active = 1;
    tb.t0_ms = mono_ms();
    tb.cx = tb.cy = -1;
    tb.ilen = 0;
    tb.pending_resize = 0;
    tb.in_mode = 0;
    tb.cap = tb.out_mode = detect_cap();
    out_str("\033[?1049h\033[2J\033[?25l");
    out_flush();
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* ---- colour: one packing, degraded by the emitter ------------------------ */

static const uint32_t PAL16[16] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0,
    0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff };
static const int CUBE[6] = { 0, 95, 135, 175, 215, 255 };

static uint32_t pal_rgb(int n) {
    if (n < 16) return PAL16[n];
    if (n < 232) { n -= 16; return (uint32_t)(CUBE[n / 36] << 16 | CUBE[n / 6 % 6] << 8 | CUBE[n % 6]); }
    int g = 8 + 10 * (n - 232);
    return (uint32_t)(g << 16 | g << 8 | g);
}

static int64_t rgb_dist(uint32_t a, uint32_t b) {
    int dr = (int)(a >> 16 & 255) - (int)(b >> 16 & 255);
    int dg = (int)(a >> 8 & 255) - (int)(b >> 8 & 255);
    int db = (int)(a & 255) - (int)(b & 255);
    return (int64_t)dr * dr + (int64_t)dg * dg + (int64_t)db * db;
}

static int nearest_in(uint32_t rgb, int lo, int hi) {
    int best = lo;
    int64_t bd = rgb_dist(rgb, pal_rgb(lo));
    for (int i = lo + 1; i < hi; i++) {
        int64_t d = rgb_dist(rgb, pal_rgb(i));
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* The emitter's colour: -1 default, 0-255 a palette index, or rgb with *rgb set (kind 2). */
static int color_resolve(int64_t style, int mode, uint32_t* rgb) {
    if (mode == TB_MODE_MONO) return -1;
    int pal = (int)(style & TB_PAL_MASK);
    if (style & TB_RGB_FLAG) {
        *rgb = (uint32_t)(style >> TB_RGB_SHIFT) & 0xFFFFFF;
        if (mode == TB_MODE_TRUE) return 256;
        return mode == TB_MODE_256 ? nearest_in(*rgb, 0, 256) : nearest_in(*rgb, 0, 16);
    }
    if (pal == 0) return -1;
    pal--;
    if (pal > 255) pal = 255;
    return mode == TB_MODE_16 && pal > 15 ? nearest_in(pal_rgb(pal), 0, 16) : pal;
}

static void sgr_color(int64_t style, int is_bg, int mode) {
    uint32_t rgb = 0;
    int c = color_resolve(style, mode, &rgb);
    char b[32];
    if (c < 0) return;
    if (c == 256)     snprintf(b, sizeof b, ";%d;2;%u;%u;%u", is_bg ? 48 : 38, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
    else if (c < 8)   snprintf(b, sizeof b, ";%d", (is_bg ? 40 : 30) + c);
    else if (c < 16)  snprintf(b, sizeof b, ";%d", (is_bg ? 100 : 90) + c - 8);
    else              snprintf(b, sizeof b, ";%d;5;%d", is_bg ? 48 : 38, c);
    out_str(b);
}

static void sgr_emit(int64_t fg, int64_t bg) {
    static const char* const ATTR[6] = { ";1", ";4", ";7", ";3", ";5", ";2" };   /* bold underline reverse italic blink dim */
    int64_t attrs = (fg | bg) >> TB_ATTR_SHIFT;
    out_str("\033[0");
    for (int i = 0; i < 6; i++) if (attrs >> i & 1) out_str(ATTR[i]);
    sgr_color(fg, 0, tb.out_mode);
    sgr_color(bg, 1, tb.out_mode);
    out_str("m");
}

static int utf8_enc(uint32_t cp, char* b) {
    if (cp < 0x80)         { b[0] = (char)cp; return 1; }
    if (cp < 0x800)        { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000)      { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 63)); b[2] = (char)(0x80 | (cp & 63)); return 3; }
    b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 63)); b[2] = (char)(0x80 | (cp >> 6 & 63)); b[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static void utf8_put(uint32_t cp) {
    char b[4];
    out_bytes(b, (size_t)utf8_enc(cp, b));
}

/* bytes -> codepoint; *len = bytes consumed (0 = incomplete, needs more) */
static uint32_t utf8_get(const unsigned char* s, int n, int* len) {
    int need = s[0] < 0x80 ? 1 : (s[0] & 0xE0) == 0xC0 ? 2 : (s[0] & 0xF0) == 0xE0 ? 3 : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (need == 0) { *len = 1; return 0xFFFD; }
    if (n < need) { *len = 0; return 0; }
    uint32_t cp = need == 1 ? s[0] : need == 2 ? s[0] & 31u : need == 3 ? s[0] & 15u : s[0] & 7u;
    for (int i = 1; i < need; i++) {
        if ((s[i] & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
        cp = cp << 6 | (s[i] & 63u);
    }
    *len = need;
    return cp;
}

/* ---- present: cursor moves only across gaps, an SGR only on a style change ---- */

static void cursor_to(int x, int y) {
    char b[32];
    snprintf(b, sizeof b, "\033[%d;%dH", y + 1, x + 1);
    out_str(b);
}

/* the columns a codepoint occupies once emitted (0/1/2), from THE width owner */
static int cp_cols(uint32_t cp) {
    char b[4];
    return ray_term_visual_width(b, utf8_enc(cp, b));
}

static ray_t* tb_present(void) {
    if (!tb.headless) {
        int w, h;
        probe_size(&w, &h);
        if ((w != tb.w || h != tb.h) && cells_alloc(w, h)) tb.pending_resize = 1;
    }
    int64_t lfg = -1, lbg = -1;
    int px = -1, py = -1;
    for (int y = 0; y < tb.h; y++)
        for (int x = 0; x < tb.w; x++) {
            tb_cell* b = &tb.back[y * tb.w + x];
            tb_cell* f = &tb.front[y * tb.w + x];
            int was_wide = cp_cols(f->ch) == 2 && x + 1 < tb.w;   /* the glyph the terminal currently shows here */
            int wide = cp_cols(b->ch) == 2 && x + 1 < tb.w;       /* x+1 is this glyph's continuation, never its own cell */
            tb_cell* bc = wide ? &tb.back[y * tb.w + x + 1] : NULL;
            tb_cell* fc = wide ? &tb.front[y * tb.w + x + 1] : NULL;
            int dirty = tb.invalid || b->ch != f->ch || b->fg != f->fg || b->bg != f->bg
                      || (wide && (bc->ch != fc->ch || bc->fg != fc->fg || bc->bg != fc->bg));
            if (dirty) {
                if (px != x || py != y) cursor_to(x, y);
                if (b->fg != lfg || b->bg != lbg) { sgr_emit(b->fg, b->bg); lfg = b->fg; lbg = b->bg; }
                utf8_put(cp_cols(b->ch) == 2 && x + 1 >= tb.w ? ' ' : b->ch);   /* a wide glyph with no room would wrap: clip to a space */
                *f = *b;
                if (wide) *fc = *bc;                          /* the terminal advanced 2 columns; never redraw the continuation */
                else if (was_wide) tb.front[y * tb.w + x + 1].ch = ~tb.back[y * tb.w + x + 1].ch;   /* a wide glyph vacated x+1: force its repaint */
                int adv = x + (wide ? 2 : 1);
                px = adv < tb.w ? adv : -1;
                py = y;
            }
            if (wide) x++;                                    /* skip the continuation column, drawn or not */
        }
    tb.invalid = 0;
    out_str("\033[0m");
    if (tb.cx >= 0 && tb.cy >= 0 && tb.cx < tb.w && tb.cy < tb.h) { cursor_to((int)tb.cx, (int)tb.cy); out_str("\033[?25h"); }
    else out_str("\033[?25l");
    out_flush();
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* ---- input: the xterm key table + SGR mouse ------------------------------ */

typedef struct { const char* key; uint32_t cp; int64_t mod, x, y, w, h; const char* button; int is_mouse; } tb_ev;

static const char* const CTRL_KEYS[32] = { "ctrl_space",
    "ctrl_a", "ctrl_b", "ctrl_c", "ctrl_d", "ctrl_e", "ctrl_f", "ctrl_g", "ctrl_h", "tab", "enter", "ctrl_k",
    "ctrl_l", "enter", "ctrl_n", "ctrl_o", "ctrl_p", "ctrl_q", "ctrl_r", "ctrl_s", "ctrl_t", "ctrl_u", "ctrl_v",
    "ctrl_w", "ctrl_x", "ctrl_y", "ctrl_z", "esc", "ctrl_backslash", "ctrl_bracket", "ctrl_caret", "ctrl_underscore" };

static const char* tilde_key(int n) {
    switch (n) {
        case 1: case 7: return "home";   case 2: return "insert";  case 3: return "delete";
        case 4: case 8: return "end";    case 5: return "pgup";    case 6: return "pgdn";
        case 11: return "f1"; case 12: return "f2"; case 13: return "f3"; case 14: return "f4"; case 15: return "f5";
        case 17: return "f6"; case 18: return "f7"; case 19: return "f8"; case 20: return "f9"; case 21: return "f10";
        case 23: return "f11"; case 24: return "f12";
        default: return "unknown";
    }
}

static const char* final_key(unsigned char c) {
    switch (c) {
        case 'A': return "up";   case 'B': return "down"; case 'C': return "right"; case 'D': return "left";
        case 'H': return "home"; case 'F': return "end";  case 'P': return "f1";    case 'Q': return "f2";
        case 'R': return "f3";   case 'S': return "f4";   case 'Z': return "tab";
        default:  return "unknown";
    }
}

/* One key from ibuf[0..n) (no leading ESC); the bytes consumed, 0 = incomplete. */
static int decode_plain(const unsigned char* s, int n, tb_ev* e) {
    unsigned char c = s[0];
    if (c < 32)   { e->key = CTRL_KEYS[c]; return 1; }
    if (c == 127 || c == 8) { e->key = "backspace"; return 1; }
    if (c == ' ') { e->key = "space"; e->cp = ' '; return 1; }
    int len;
    uint32_t cp = utf8_get(s, n, &len);
    if (len == 0) return 0;
    e->cp = cp;
    e->key = NULL;                     /* the text itself names the key */
    return len;
}

/* ESC [ ... final / ESC O final.  Returns bytes consumed, 0 = incomplete, -1 = not a sequence. */
static int decode_csi(const unsigned char* s, int n, tb_ev* e) {
    if (n < 2 || (s[1] != '[' && s[1] != 'O')) return -1;
    int i = 2;
    while (i < n && !(s[i] >= 0x40 && s[i] <= 0x7E)) i++;
    if (i >= n) return n >= 8 ? -1 : 0;
    unsigned char fin = s[i];
    int p[4] = { 0, 0, 0, 0 }, np = 0, mouse = 0, j = 2;
    if (s[1] == '[' && j < i && s[j] == '<') { mouse = 1; j++; }
    for (int v = 0; j <= i; j++) {
        if (s[j] >= '0' && s[j] <= '9') v = v * 10 + (s[j] - '0');
        else { if (np < 4) p[np++] = v; v = 0; }
    }
    if (mouse) {
        int b = p[0], motion = b & 32;                   /* 1003: 35 = the pointer moved with no button, 32-34 a drag */
        e->is_mouse = 1;
        e->key = motion ? "move" : "";
        e->x = p[1] - 1; e->y = p[2] - 1;
        e->mod = (b & 4 ? 1 : 0) | (b & 8 ? 2 : 0) | (b & 16 ? 4 : 0);
        e->button = fin == 'm' ? "release" : (b & 64) ? ((b & 3) ? "wheel_down" : "wheel_up")
                  : (b & 3) == 0 ? "left" : (b & 3) == 1 ? "middle" : (b & 3) == 2 ? "right" : "none";
        return i + 1;
    }
    e->mod = np > 1 ? p[1] - 1 : 0;
    if (fin == 'Z') e->mod |= 1;
    e->key = fin == '~' ? tilde_key(p[0]) : final_key(fin);
    return i + 1;
}

/* the whole decode: 1 event filled + consumed, 0 nothing (timeout/EOF) */
static int next_event(int ms, tb_ev* e) {
    memset(e, 0, sizeof *e);
    e->x = e->y = e->w = e->h = NULL_I64;
    e->button = "";
    if (tb.pending_resize) { tb.pending_resize = 0; e->key = ""; e->w = tb.w; e->h = tb.h; return 1; }
    if (tb.ilen == 0 && !input_fill(ms)) return 0;
    for (;;) {
        const unsigned char* s = tb.ibuf;
        int n = tb.ilen, used;
        if (s[0] != 27) used = decode_plain(s, n, e);
        else if (n == 1) used = 0;                          /* ESC alone so far: a sequence may follow */
        else if ((used = decode_csi(s, n, e)) == -1) {      /* ESC + key = alt (xterm's meta) */
            used = decode_plain(s + 1, n - 1, e);
            if (used > 0) { used++; e->mod |= 2; }
        }
        if (used > 0) { input_consume(used); return 1; }
        if (!input_fill(TB_ESC_MS)) {                       /* incomplete and nothing more coming */
            e->key = s[0] == 27 ? "esc" : "unknown";
            input_consume(1);
            return 1;
        }
    }
}

/* ---- q value construction ------------------------------------------------ */

static ray_t* sym_atom(const char* s) { return ray_sym(ray_sym_intern_runtime(s, strlen(s))); }

static ray_t* null_ok(void) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }

static void drop(ray_t* x) {
    if (!x) return;
    if (RAY_IS_ERR(x)) ray_error_free(x); else ray_release(x);
}

/* names!vals — consumes every element of vals */
static ray_t* dict_of(const char* const* names, ray_t** vals, int n) {
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, n);
    ray_t* v = ray_list_new(n);
    int bad = !k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v);
    for (int i = 0; i < n; i++) {
        int64_t id = ray_sym_intern(names[i], strlen(names[i]));
        if (!bad) { k = ray_vec_append(k, &id); bad = RAY_IS_ERR(k); }
        if (!vals[i] || RAY_IS_ERR(vals[i])) bad = 1;
        else if (!bad) { v = ray_list_append(v, vals[i]); bad = RAY_IS_ERR(v); }
        drop(vals[i]);
    }
    if (bad) { drop(k); drop(v); return q_err(QE_WSFULL); }
    return ray_dict_new(k, v);
}

/* `kind`name`ch`mod`x`y`button`w`h — one shape for every event, nulls where a field does not apply
 * (kind/name, not type/key: q keywords cannot be named in qSQL).  A non-key event's name is its kind, so
 * one name switch dispatches everything; a timeout (e NULL) is kind AND name `none, never (). */
static const char* const EV_NAMES[9] = { "kind", "name", "ch", "mod", "x", "y", "button", "w", "h" };

/* the three derived lanes of an event: its kind, its name (a printable names itself into kbuf), its ch */
static void event_lanes(const tb_ev* e, const char** type, const char** key, char* kbuf, ray_t** ch) {
    *type = !e ? "none" : e->is_mouse ? "mouse" : e->w != NULL_I64 ? "resize" : "key";
    *key = !e ? *type : e->key && *e->key ? e->key : e->key ? *type : (kbuf[utf8_enc(e->cp, kbuf)] = 0, kbuf);
    *ch = !e || e->cp == 0 ? ray_char(' ') : e->cp < 128 ? ray_char((uint8_t)e->cp) : ray_i64(e->cp);
}

static ray_t* event_value(const tb_ev* e) {
    char kbuf[5];
    const char *type, *key;
    ray_t* ch;
    event_lanes(e, &type, &key, kbuf, &ch);
    ray_t* v[9] = { sym_atom(type), sym_atom(key), ch, ray_i64(e ? e->mod : NULL_I64),
                    ray_i64(e ? e->x : NULL_I64), ray_i64(e ? e->y : NULL_I64), sym_atom(e ? e->button : ""),
                    ray_i64(e ? e->w : NULL_I64), ray_i64(e ? e->h : NULL_I64) };
    return dict_of(EV_NAMES, v, 9);
}

enum { TB_EVENTS_MAX = 256 };

/* every pending event as a table, one row each in arrival order; the first waits up to ms, the rest do not */
static ray_t* events_table(int ms) {
    static tb_ev evs[TB_EVENTS_MAX];
    int n = 0;
    while (n < TB_EVENTS_MAX && next_event(n ? 0 : ms, &evs[n])) n++;
    ray_t* c[9];
    c[0] = ray_sym_vec_new(RAY_SYM_W64, n + 1); c[1] = ray_sym_vec_new(RAY_SYM_W64, n + 1); c[2] = ray_list_new(n + 1);
    for (int j = 3; j < 9; j++) c[j] = j == 6 ? ray_sym_vec_new(RAY_SYM_W64, n + 1) : ray_vec_new(RAY_I64, n + 1);
    for (int i = 0; i < n; i++) {
        char kbuf[5];
        const char *type, *key;
        ray_t* ch;
        event_lanes(&evs[i], &type, &key, kbuf, &ch);
        int64_t ids[3] = { ray_sym_intern_runtime(type, strlen(type)), ray_sym_intern_runtime(key, strlen(key)),
                           ray_sym_intern_runtime(evs[i].button, strlen(evs[i].button)) };
        int64_t nums[5] = { evs[i].mod, evs[i].x, evs[i].y, evs[i].w, evs[i].h };
        if (c[0] && !RAY_IS_ERR(c[0])) c[0] = ray_vec_append(c[0], &ids[0]);
        if (c[1] && !RAY_IS_ERR(c[1])) c[1] = ray_vec_append(c[1], &ids[1]);
        if (c[6] && !RAY_IS_ERR(c[6])) c[6] = ray_vec_append(c[6], &ids[2]);
        if (c[2] && !RAY_IS_ERR(c[2]) && ch && !RAY_IS_ERR(ch)) c[2] = ray_list_append(c[2], ch);
        drop(ch);
        if (c[3] && !RAY_IS_ERR(c[3])) c[3] = ray_vec_append(c[3], &nums[0]);
        if (c[4] && !RAY_IS_ERR(c[4])) c[4] = ray_vec_append(c[4], &nums[1]);
        if (c[5] && !RAY_IS_ERR(c[5])) c[5] = ray_vec_append(c[5], &nums[2]);
        if (c[7] && !RAY_IS_ERR(c[7])) c[7] = ray_vec_append(c[7], &nums[3]);
        if (c[8] && !RAY_IS_ERR(c[8])) c[8] = ray_vec_append(c[8], &nums[4]);
    }
    ray_t* t = ray_table_new(9);
    for (int j = 0; j < 9; j++) {
        if (c[j] && !RAY_IS_ERR(c[j])) { t = ray_table_add_col(t, ray_sym_intern(EV_NAMES[j], strlen(EV_NAMES[j])), c[j]); ray_release(c[j]); }
        else { drop(t); t = c[j] ? c[j] : q_err(QE_WSFULL); }
    }
    return t;
}

/* ---- argument reads ------------------------------------------------------ */

static ray_t* need_active(void) { return tb.active ? NULL : q_err(QE_DOMAIN); }

static int arg_i64(ray_t* x, int64_t* out) { return q_type_is_int_atom(x) ? (*out = q_type_iatom_val(x), 1) : 0; }

/* ch: a char atom (byte) or an int atom (codepoint) */
static int arg_cp(ray_t* x, uint32_t* cp) {
    int64_t v;
    if (q_type_is_char_atom(x)) { *cp = x->u8; return 1; }
    if (!arg_i64(x, &v) || v < 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;   /* Unicode scalar values only */
    *cp = (uint32_t)v;
    return 1;
}

/* a style for cell x of a row: atom (whole), int vector (per cell), else 'type via 0 */
static int style_at(ray_t* s, int64_t x, int64_t* out) {
    if (arg_i64(s, out)) return 1;
    if (q_type_is_int_vec(s)) { *out = x < q_count(s) ? q_type_ivec_get(s, x) : 0; return 1; }
    return 0;
}

/* decode text into cells from (x,y); width from THE width owner.  x is int64 so a far-right
 * start stays off-screen (cell_set ignores it) instead of wrapping to a valid column. */
static void put_text(int64_t x, int64_t y, const char* p, int64_t n, ray_t* fg, ray_t* bg, int64_t xmax) {
    int64_t i = 0, k = 0;
    while (i < n && x < xmax) {
        int len;
        uint32_t cp = utf8_get((const unsigned char*)p + i, (int)(n - i), &len);
        if (len == 0) len = 1;
        int64_t f = 0, b = 0;
        style_at(fg, k, &f);
        style_at(bg, k, &b);
        cell_set(x, y, cp, f, b);
        x += ray_term_visual_width(p + i, len);
        i += len; k++;
    }
}

/* ---- the natives --------------------------------------------------------- */

static ray_t* tbi_init_fn(ray_t** args, int64_t n) {
    if (tb.active) return q_err(QE_DOMAIN);
    if (n == 1 && (RAY_IS_NULL(args[0]))) {
        if (!tty_open()) return q_err(QE_OS);
        int w, h;
        probe_size(&w, &h);
        return session_open(0, w, h);
    }
    if (n != 4) return q_err(QE_RANK);
    int64_t w, h;
    if (!arg_i64(args[2], &w) || !arg_i64(args[3], &h) || w < 1 || h < 1 || w > 4096 || h > 4096) return q_err(QE_TYPE);
    ray_t* in  = q_io_path_operand(args[0]);
    ray_t* out = q_io_path_operand(args[1]);
    if (!in || !out) { if (in) ray_release(in); if (out) ray_release(out); return q_err(QE_TYPE); }
    int ok = headless_open(ray_str_ptr(in), ray_str_ptr(out));
    ray_release(in);
    ray_release(out);
    if (!ok) return q_err(QE_OS);
    return session_open(1, (int)w, (int)h);
}

static ray_t* tbi_shutdown_fn(ray_t* x) { session_close(); return null_ok(); }

static ray_t* tbi_size_fn(ray_t* x) {
    ray_t* e = need_active();
    if (e) return e;
    int64_t wh[2] = { tb.w, tb.h };
    return ray_vec_from_raw(RAY_I64, wh, 2);
}

static ray_t* tbi_clear_fn(ray_t* x) {
    ray_t* e = need_active();
    if (e) return e;
    cells_clear(tb.back, tb.w * tb.h);
    return null_ok();
}

static ray_t* tbi_present_fn(ray_t* x) {
    ray_t* e = need_active();
    return e ? e : tb_present();
}

static ray_t* tbi_invalidate_fn(ray_t* x) {
    ray_t* e = need_active();
    if (e) return e;
    tb.invalid = 1;
    return null_ok();
}

static ray_t* tbi_set_cursor_fn(ray_t** args, int64_t n) {
    ray_t* e = need_active();
    if (e) return e;
    int64_t cx, cy;
    if (n != 2) return q_err(QE_RANK);
    if (!arg_i64(args[0], &cx) || !arg_i64(args[1], &cy)) return q_err(QE_TYPE);
    tb.cx = cx; tb.cy = cy;
    return null_ok();
}

static ray_t* tbi_set_cell_fn(ray_t** args, int64_t n) {
    ray_t* e = need_active();
    if (e) return e;
    if (n != 5) return q_err(QE_RANK);
    int64_t x, y, fg, bg;
    uint32_t cp;
    if (!arg_i64(args[0], &x) || !arg_i64(args[1], &y) || !arg_cp(args[2], &cp) || !arg_i64(args[3], &fg) || !arg_i64(args[4], &bg))
        return q_err(QE_TYPE);
    cell_set(x, y, cp, fg, bg);
    return null_ok();
}

/* a style operand: an int atom (whole) or an int vector (per cell) */
static int is_style(ray_t* s) { int64_t v; return arg_i64(s, &v) || q_type_is_int_vec(s); }

/* the sparse overlay: columnar x y, ch a char atom / a string (one byte per cell) / a codepoint long or vector /
 * a list of strings each ONE codepoint, fg/bg a style atom or a vector — one cell_set per element, off-screen
 * cells dropped, so no second cell-write path */
static int64_t cells_ch_len(ray_t* ch, const char** p, int64_t* plen) {
    if (q_type_is_char_atom(ch) || q_type_is_int_atom(ch)) return -1;
    if (ch->type == RAY_CHARV && q_str_text_bytes(ch, p, plen)) return *plen;
    if (q_type_is_int_vec(ch) || ch->type == RAY_LIST) return q_count(ch);
    return -2;
}

/* cell i's codepoint from the ch operand; 0 = not a single codepoint */
static uint32_t cells_ch_at(ray_t* ch, const char* p, int64_t i) {
    uint32_t cp;
    if (p) return (unsigned char)p[i];
    if (q_type_is_char_atom(ch) || q_type_is_int_atom(ch)) return arg_cp(ch, &cp) ? cp : 0;
    if (q_type_is_int_vec(ch)) { int64_t v = q_type_ivec_get(ch, i); return v >= 0 && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF) ? (uint32_t)v : 0; }
    ray_t* s = q_index_elem_at(ch, i);
    const char* q; int64_t n; int len = 0;
    int ok = s && !RAY_IS_ERR(s) && q_str_text_bytes(s, &q, &n) && n > 0;
    cp = ok ? utf8_get((const unsigned char*)q, (int)n, &len) : 0;
    drop(s);
    return ok && len == n && cp ? cp : 0;
}

static ray_t* tbi_set_cells_fn(ray_t** args, int64_t n) {
    ray_t* e = need_active();
    if (e) return e;
    if (n != 5) return q_err(QE_RANK);
    ray_t *xs = args[0], *ys = args[1], *ch = args[2], *fg = args[3], *bg = args[4];
    if (!is_style(xs) || !is_style(ys) || !is_style(fg) || !is_style(bg)) return q_err(QE_TYPE);
    const char* p = NULL; int64_t plen = 0;
    int64_t chn = cells_ch_len(ch, &p, &plen);
    if (chn == -2) return q_err(QE_TYPE);
    int64_t cnt = q_type_is_int_vec(xs) ? q_count(xs) : q_type_is_int_vec(ys) ? q_count(ys) : chn >= 0 ? chn : 1;   /* atoms conform */
    if ((q_type_is_int_vec(xs) && q_count(xs) != cnt) || (q_type_is_int_vec(ys) && q_count(ys) != cnt) || (chn >= 0 && chn != cnt) ||
        (q_type_is_int_vec(fg) && q_count(fg) != cnt) || (q_type_is_int_vec(bg) && q_count(bg) != cnt)) return q_err(QE_LENGTH);
    for (int64_t i = 0; p && i < plen; i++) if ((unsigned char)p[i] >= 128) return q_err(QE_TYPE);
    for (int64_t i = 0; i < cnt; i++) {
        int64_t x = 0, y = 0, f = 0, b = 0;
        uint32_t cp = cells_ch_at(ch, p, i);
        if (!cp) return q_err(QE_TYPE);
        style_at(xs, i, &x);
        style_at(ys, i, &y);
        style_at(fg, i, &f);
        style_at(bg, i, &b);
        cell_set(x, y, cp, f, b);
    }
    return null_ok();
}


/* [x;y;fg;bg;text] or [x;y;fg;bg;text;w;align]: the text sits in a w-cell field from x, aligned by CELLS
 * (the buffer's own width law), clipped to the field */
static ray_t* tbi_print_fn(ray_t** args, int64_t n) {
    ray_t* e = need_active();
    if (e) return e;
    if (n != 5 && n != 7) return q_err(QE_RANK);
    int64_t x, y, w = 0, xmax = INT64_MAX;
    const char* p; int64_t len;
    if (!arg_i64(args[0], &x) || !arg_i64(args[1], &y) || !is_style(args[2]) || !is_style(args[3]) ||
        !q_str_text_bytes(args[4], &p, &len))
        return q_err(QE_TYPE);
    if (n == 7) {
        const char* al; int64_t aln;
        if (!arg_i64(args[5], &w) || w < 0 || !q_str_subject_bytes(args[6], &al, &aln)) return q_err(QE_TYPE);
        int64_t cells = ray_term_visual_width(p, (int32_t)len), gap = w > cells ? w - cells : 0;
        xmax = x + w;
        if (aln == 4 && !memcmp(al, "left", 4))        ;
        else if (aln == 5 && !memcmp(al, "right", 5)) x += gap;
        else if (aln == 6 && (!memcmp(al, "centre", 6) || !memcmp(al, "center", 6))) x += gap / 2;
        else return q_err(QE_TYPE);
    }
    put_text(x, y, p, len, args[2], args[3], xmax);
    return null_ok();
}

/* row y's share of a style: an atom is the whole, a vector/list is per row (owned answer; NULL = the atom itself) */
static ray_t* row_style(ray_t* s, int64_t y) {
    if (!s || y >= q_count(s) || !(s->type == RAY_LIST || q_type_is_int_vec(s))) return NULL;
    return s->type == RAY_LIST ? q_index_elem_at(s, y) : ray_i64(q_type_ivec_get(s, y));
}

/* rows: one string, or a list of strings; fg/bg: atom, per-row vector, or per-cell matrix */
static ray_t* tbi_show_fn(ray_t** args, int64_t n) {
    ray_t* e = need_active();
    if (e) return e;
    if (n != 1 && n != 3) return q_err(QE_RANK);
    ray_t* zero = ray_i64(0);
    ray_t* rows = args[0];
    ray_t* fg = n == 3 ? args[1] : zero;
    ray_t* bg = n == 3 ? args[2] : zero;
    if (n == 3 && !(is_style(fg) || fg->type == RAY_LIST) ) { ray_release(zero); return q_err(QE_TYPE); }
    if (n == 3 && !(is_style(bg) || bg->type == RAY_LIST) ) { ray_release(zero); return q_err(QE_TYPE); }
    const char* p; int64_t len;
    int ok = 1;
    if (q_str_text_bytes(rows, &p, &len)) put_text(0, 0, p, len, fg, bg, INT64_MAX);
    else if (rows->type != RAY_LIST) ok = 0;
    for (int64_t y = 0; ok && rows->type == RAY_LIST && y < q_count(rows) && y < tb.h; y++) {
        ray_t* row = q_index_elem_at(rows, y);
        ray_t* rf = row_style(fg, y);
        ray_t* rb = row_style(bg, y);
        ok = row && !RAY_IS_ERR(row) && q_str_text_bytes(row, &p, &len);
        if (ok) put_text(0, y, p, len, rf ? rf : fg, rb ? rb : bg, INT64_MAX);
        drop(row); drop(rf); drop(rb);
    }
    ray_release(zero);
    return ok ? null_ok() : q_err(QE_TYPE);
}

static ray_t* tbi_peek_event_fn(ray_t* x) {
    ray_t* e = need_active();
    if (e) return e;
    int64_t ms;
    if (!arg_i64(x, &ms)) return q_err(QE_TYPE);
    tb_ev ev;
    int got = next_event(ms < 0 ? -1 : (int)(ms > INT32_MAX ? INT32_MAX : ms), &ev);
    return event_value(got ? &ev : NULL);
}

static ray_t* tbi_events_fn(ray_t* x) {
    ray_t* e = need_active();
    if (e) return e;
    int64_t ms;
    if (!arg_i64(x, &ms)) return q_err(QE_TYPE);
    return events_table(ms < 0 ? -1 : (int)(ms > INT32_MAX ? INT32_MAX : ms));
}

/* milliseconds since init on the monotonic clock; 0 before init, so a game can call it unconditionally */
static ray_t* tbi_clock_fn(ray_t* x) { return ray_i64(tb.active ? mono_ms() - tb.t0_ms : 0); }

/* word-wrap to w CELLS: breaks at spaces, a newline starts a line, a word wider than w is split hard.  Pure. */
static ray_t* wrap_emit(ray_t* out, const char* line, int64_t n) {
    ray_t* l = ray_charv(line, n);
    if (!out || RAY_IS_ERR(out) || !l || RAY_IS_ERR(l)) { drop(l); return out; }
    out = ray_list_append(out, l);
    ray_release(l);
    return out;
}

static ray_t* tbi_wrap_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    int64_t w;
    const char* p; int64_t len;
    if (!arg_i64(args[0], &w) || w < 1 || !q_str_text_bytes(args[1], &p, &len)) return q_err(QE_TYPE);
    char* line = malloc((size_t)len + 2);
    char* word = malloc((size_t)len + 2);
    if (!line || !word) { free(line); free(word); return q_err(QE_WSFULL); }
    ray_t* out = ray_list_new(8);
    int64_t ln = 0, lw = 0, wn = 0, ww = 0, i = 0, emitted = 0;   /* line/word: bytes and cells */
    #define PLACE() do { if (wn) { if (lw && lw + 1 + ww > w) { out = wrap_emit(out, line, ln); emitted++; ln = lw = 0; } \
        if (lw) { line[ln++] = ' '; lw++; } \
        memcpy(line + ln, word, (size_t)wn); ln += wn; lw += ww; wn = ww = 0; } } while (0)
    while (i < len) {
        unsigned char c = (unsigned char)p[i];
        if (c == ' ' || c == '\n') { PLACE(); if (c == '\n') { out = wrap_emit(out, line, ln); emitted++; ln = lw = 0; } i++; continue; }
        int l;
        utf8_get((const unsigned char*)p + i, (int)(len - i), &l);
        if (l == 0) l = 1;
        int64_t cw = ray_term_visual_width(p + i, l);
        if (ww + cw > w) PLACE();                      /* a word wider than the line breaks where it must */
        memcpy(word + wn, p + i, (size_t)l); wn += l; ww += cw; i += l;
    }
    PLACE();
    if (ln || !emitted) out = wrap_emit(out, line, ln);
    #undef PLACE
    free(line); free(word);
    return out ? out : q_err(QE_WSFULL);
}

static ray_t* tbi_set_input_mode_fn(ray_t* x) {
    ray_t* e = need_active();
    if (e) return e;
    int64_t m;
    if (!arg_i64(x, &m)) return q_err(QE_TYPE);
    int64_t prev = tb.in_mode;
    if ((m & TB_IN_MOUSE) != (prev & TB_IN_MOUSE)) { mouse_track(m & TB_IN_MOUSE); out_flush(); }
    tb.in_mode = (int)m;
    return ray_i64(prev);
}

static ray_t* tbi_set_output_mode_fn(ray_t* x) {
    ray_t* e = need_active();
    if (e) return e;
    int64_t m;
    if (!arg_i64(x, &m) || m < TB_MODE_MONO || m > TB_MODE_TRUE) return q_err(QE_TYPE);
    int64_t prev = tb.out_mode;
    tb.out_mode = (int)m;
    tb.invalid = 1;
    return ray_i64(prev);
}

static ray_t* tbi_has_truecolor_fn(ray_t* x) {
    ray_t* e = need_active();
    return e ? e : ray_bool(tb.cap == TB_MODE_TRUE);
}

/* the back buffer as a table x y ch fg bg — ch a long codepoint column */
static ray_t* tbi_cells_fn(ray_t* x) {
    ray_t* e = need_active();
    if (e) return e;
    static const char* const NAMES[5] = { "x", "y", "ch", "fg", "bg" };
    int64_t n = (int64_t)tb.w * tb.h;
    ray_t* c[5];
    for (int i = 0; i < 5; i++) c[i] = ray_vec_new(RAY_I64, n);
    for (int64_t i = 0; i < n; i++) {
        int64_t v[5] = { i % tb.w, i / tb.w, tb.back[i].ch, tb.back[i].fg, tb.back[i].bg };
        for (int j = 0; j < 5; j++) if (c[j] && !RAY_IS_ERR(c[j])) c[j] = ray_vec_append(c[j], &v[j]);
    }
    ray_t* t = ray_table_new(5);
    for (int i = 0; i < 5; i++) {
        if (c[i] && !RAY_IS_ERR(c[i])) { t = ray_table_add_col(t, ray_sym_intern(NAMES[i], strlen(NAMES[i])), c[i]); ray_release(c[i]); }
        else { if (t && !RAY_IS_ERR(t)) ray_release(t); t = c[i] ? c[i] : q_err(QE_WSFULL); }
    }
    return t;
}

/* ---- registration -------------------------------------------------------- */

static void bind_u(const char* name, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(name, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

static void bind_v(const char* name, ray_vary_fn fn) {
    ray_t* obj = ray_fn_vary(name, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

void q_termbox_register(void) {
    q_ctx_set_tty_restore(session_close);
    bind_v(".termbox.i.init",            tbi_init_fn);
    bind_u(".termbox.i.shutdown",        tbi_shutdown_fn);
    bind_u(".termbox.i.size",            tbi_size_fn);
    bind_u(".termbox.i.clear",           tbi_clear_fn);
    bind_u(".termbox.i.present",         tbi_present_fn);
    bind_u(".termbox.i.invalidate",      tbi_invalidate_fn);
    bind_v(".termbox.i.set_cursor",      tbi_set_cursor_fn);
    bind_v(".termbox.i.set_cell",        tbi_set_cell_fn);
    bind_v(".termbox.i.set_cells",       tbi_set_cells_fn);
    bind_u(".termbox.i.events",          tbi_events_fn);
    bind_u(".termbox.i.clock",           tbi_clock_fn);
    bind_v(".termbox.i.wrap",            tbi_wrap_fn);
    bind_v(".termbox.i.print",           tbi_print_fn);
    bind_v(".termbox.i.show",            tbi_show_fn);
    bind_u(".termbox.i.peek_event",      tbi_peek_event_fn);
    bind_u(".termbox.i.set_input_mode",  tbi_set_input_mode_fn);
    bind_u(".termbox.i.set_output_mode", tbi_set_output_mode_fn);
    bind_u(".termbox.i.has_truecolor",   tbi_has_truecolor_fn);
    bind_u(".termbox.i.cells",           tbi_cells_fn);
}
