/* q_fmt — the q-style value formatter (q_fmt.h); non-q shapes -> ray_fmt. */
#include "qlang/q_fmt.h"
#include "qlang/q_fmt_internal.h" /* q_cell — shared with the pipe renderer */
#include "qlang/q_fmt_pipe.h"  /* `--nonlegacy` console table render */
#include "qlang/q_registry.h" /* q_registry_list_value — hidden literal head */
#include "qlang/q_registry_internal.h" /* q_type_qname — the guarded type-name home */
#include "qlang/q_deriv.h"    /* q_deriv_kind_of — 104h carrier display */
#include "qlang/q_sys.h"   /* q_con_display — live `\c rows cols` clip state */
#include "lang/format.h"   /* ray_fmt */
#include "lang/eval.h"     /* ray_at_fn — dict/table element access */
#include "core/ipc.h"      /* ray_ipc_current_handle — handler console write-through */

#include "table/sym.h"     /* ray_sym_vec_cell — resolve a sym-vector cell */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <math.h>

/* `\P n` float precision (default 7, 0=max 17); writer q_sys.c h_P, reader q_float_tok. */
#define Q_PRINT_PREC_DEFAULT 7
static int g_print_prec = Q_PRINT_PREC_DEFAULT;

void q_fmt_set_prec(int p) { g_print_prec = p; }
int  q_fmt_prec(void)      { return g_print_prec; }

/* Name-ref syms of these chars print bare (kdb `(/;+)`, basics/parsetrees.md). */
static const char Q_VERBS[] = ":+-*%!&|<>=~,^#_$?@./\\'";

static int q_sym_bare(const char* nm, size_t l) {
    if (l == 1 && nm[0] && strchr(Q_VERBS, nm[0])) return 1;
    if (l == 2 && nm[1] == ':' && nm[0] && strchr(Q_VERBS, nm[0])) return 1;
    return 0;
}

/* ---- console emitter (qe_*): THE single write path.  q_fmt = plain target;
 * q_fmt_console = the one CLIP-armed target with the `\c rows cols` rule
 * (2026-07-13 golden spec): width — over cols-1 keeps cols-3 chars + `..`
 * (fixed-column, type-blind); height — over rows-2 lines shows rows-3 + a
 * bare `..` row.  All clip logic is in qe_putc/qe_flush_line; early exits
 * are OPTIONAL (overflow is counted-then-swallowed); only the OUTERMOST
 * console render clips. */

typedef struct {
    char*  buf;   /* caller-owned output buffer (NUL-terminated throughout) */
    size_t cap;   /* total bytes incl. the NUL */
    size_t pos;   /* write position */
    int    clip;  /* 1 = the (single) `\c`-armed console target */
} qe_tgt;

#define QE_MAX 128
static qe_tgt g_qe[QE_MAX];
static int    g_qe_n;          /* stack depth; deeper-than-QE_MAX renders empty */
static qe_tgt g_qe_void;       /* overflow sink (buf NULL — all writes no-op) */

static struct {
    int32_t rows, cols;   /* live `\c` size (already clamped to [10,2000]) */
    int32_t nlines;       /* completed (flushed) display lines */
    int     stop;         /* height cap decided — swallow everything else */
    char    line[2048];   /* physical prefix of the current line (>= cols) */
    size_t  llen;         /* buffered physical chars (<= cols) */
    size_t  llog;         /* LOGICAL chars on the line, incl. swallowed overflow */
    size_t  ltrail;       /* trailing-space run at the logical end (qe_trim) */
} g_clip;
static int g_clip_active;

static qe_tgt* qe_top(void) {
    return (g_qe_n > 0 && g_qe_n <= QE_MAX) ? &g_qe[g_qe_n - 1] : &g_qe_void;
}

static void qe_push(char* buf, size_t cap, int clip) {
    g_qe_n++;
    if (g_qe_n > QE_MAX) return;               /* absurd nesting: render empty */
    qe_tgt* t = &g_qe[g_qe_n - 1];
    t->buf = buf; t->cap = cap; t->pos = 0; t->clip = clip;
    if (buf && cap > 0) buf[0] = '\0';
}

static void qe_raw(qe_tgt* t, const char* s, size_t n) {
    if (!t->buf || t->cap == 0) return;
    size_t avail = t->cap - 1 - t->pos;
    if (n > avail) n = avail;
    memcpy(t->buf + t->pos, s, n);
    t->pos += n;
    t->buf[t->pos] = '\0';
}

static void qe_flush_line(qe_tgt* t) {
    if (g_clip.llog > (size_t)(g_clip.cols - 1)) {
        qe_raw(t, g_clip.line, (size_t)(g_clip.cols - 3));
        qe_raw(t, "..", 2);
    } else {
        qe_raw(t, g_clip.line, g_clip.llen);
    }
    g_clip.llen = g_clip.llog = g_clip.ltrail = 0;
}

static void qe_putc(char c) {
    qe_tgt* t = qe_top();
    if (!t->clip) {
        if (t->buf && t->pos + 1 < t->cap) {
            t->buf[t->pos++] = c;
            t->buf[t->pos] = '\0';
        }
        return;
    }
    if (g_clip.stop) return;
    if (c == '\n') {
        if (g_clip.nlines + 1 >= g_clip.rows - 2) {
            /* output never ends in '\n', so more follows: this line becomes `..` */
            g_clip.llen = g_clip.llog = g_clip.ltrail = 0;
            qe_raw(t, "..", 2);
            g_clip.stop = 1;
            return;
        }
        qe_flush_line(t);
        qe_raw(t, "\n", 1);
        g_clip.nlines++;
        return;
    }
    g_clip.llog++;
    g_clip.ltrail = (c == ' ') ? g_clip.ltrail + 1 : 0;
    if (g_clip.llen < (size_t)g_clip.cols && g_clip.llen < sizeof g_clip.line - 1)
        g_clip.line[g_clip.llen++] = c;
}

static void qe_putn(const char* s, size_t n) {
    qe_tgt* t = qe_top();
    if (!t->clip) { qe_raw(t, s, n); return; }
    for (size_t i = 0; i < n && !g_clip.stop; i++) qe_putc(s[i]);
}

static void qe_puts(const char* s) { qe_putn(s, strlen(s)); }

static void qe_printf(const char* fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) n = sizeof tmp - 1;
    qe_putn(tmp, (size_t)n);
}

/* Fits? unclipped: whole-token buffer guard; clipped: only the height stop. */
static int qe_fits(size_t need) {
    qe_tgt* t = qe_top();
    if (t->clip) return !g_clip.stop;
    return t->buf && t->pos + need + 1 <= t->cap;
}

/* qe_done: height cap hit; qe_line_done: line over width — never before qe_trim. */
static int qe_done(void) {
    qe_tgt* t = qe_top();
    return t->clip && g_clip.stop;
}
static int qe_line_done(void) {
    qe_tgt* t = qe_top();
    return t->clip &&
           (g_clip.stop || g_clip.llog > (size_t)(g_clip.cols - 1));
}

static void qe_pad(const char* s, int w) {
    int l = (int)strlen(s);
    for (int i = 0; i < w; i++) qe_putc(i < l ? s[i] : ' ');
}

static void qe_trim(void) {
    qe_tgt* t = qe_top();
    if (!t->clip) {
        if (!t->buf) return;
        while (t->pos > 0 && t->buf[t->pos - 1] == ' ') t->pos--;
        t->buf[t->pos] = '\0';
        return;
    }
    if (g_clip.stop) return;
    g_clip.llog -= g_clip.ltrail;
    if (g_clip.llen > g_clip.llog) g_clip.llen = g_clip.llog;
    g_clip.ltrail = 0;
}

static void qe_pop(void) {
    if (g_qe_n > QE_MAX) { g_qe_n--; return; }
    qe_tgt* t = qe_top();
    if (t->clip && !g_clip.stop) qe_flush_line(t);   /* pending last line */
    g_qe_n--;
}

/* Clip-armed row budget for column sizing (spec §1.4); 0 = all rows. */
static int64_t qe_clip_rows(void) {
    qe_tgt* t = qe_top();
    return t->clip ? (int64_t)g_clip.rows : 0;
}

/* Sym atom: verb/null name-refs bare, else backticked; a DATA sym (Q_ATTR_QUOTED) never bare. */
static void qe_sym(ray_t* val) {
    ray_t* s = ray_sym_str(val->i64);
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int bare = q_sym_bare(nm, l) && !(val->attrs & 0x20 /* Q_ATTR_QUOTED */);
    if (!bare) qe_putc('`');
    qe_putn(nm, l);
    ray_release(s);
}

static void qe_ray_fallback(ray_t* val) {
    ray_t* s = ray_fmt(val, 0);
    if (s && !RAY_IS_ERR(s) && s->type == -RAY_STR)
        qe_putn(ray_str_ptr(s), ray_str_len(s));
    if (s && !RAY_IS_ERR(s)) ray_release(s);
}

static void ray_fallback(ray_t* val, char* buf, size_t bufsz) {
    ray_t* s = ray_fmt(val, 0);
    if (s && !RAY_IS_ERR(s) && s->type == -RAY_STR) {
        size_t n = ray_str_len(s);
        if (n >= bufsz) n = bufsz - 1;
        memcpy(buf, ray_str_ptr(s), n);
        buf[n] = '\0';
    }
    if (s && !RAY_IS_ERR(s)) ray_release(s);
}

void q_fmt(ray_t* val, char* buf, size_t bufsz);   /* fwd */
void q_fmt_krepr(ray_t* val, char* buf, size_t bufsz);   /* fwd (impl at EOF) */

static char*  g_console;
static size_t g_console_len, g_console_cap;

void q_console_reset(void) { g_console_len = 0; if (g_console) g_console[0] = '\0'; }

const char* q_console_str(void) { return g_console ? g_console : ""; }

static void q_console_append(const char* s, size_t n) {
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
static void q_console_emit(const char* s, size_t n) {
    if (ray_ipc_current_handle() >= 0) {
        fwrite(s, 1, n, stdout);
        fflush(stdout);
    } else {
        q_console_append(s, n);
    }
}

void q_console_show(ray_t* val) {
    char buf[8192]; buf[0] = '\0';
    q_fmt_console(val, buf, sizeof buf);   /* `show` obeys the `\c` display clip */
    q_console_emit(buf, strlen(buf));
    q_console_emit("\n", 1);
}

void q_console_show_krepr(ray_t* val) {
    char buf[8192]; buf[0] = '\0';
    q_fmt_krepr(val, buf, sizeof buf);
    q_console_emit(buf, strlen(buf));
    q_console_emit("\n", 1);
}

void q_console_write(const char* s, size_t n) { q_console_emit(s, n); }

/* Tables: padded columns under a dashed rule, keyed tables put key columns left of `|`; NO trailing spaces. */

#define QF_MAXCOL 64

/* Uniformly-singleton nested column collapses (`1| 10`); mixed keeps `,50`. */
static int q_col_uniform_singleton(ray_t* col) {
    if (!col || col->type != RAY_LIST) return 0;
    int64_t n = ray_len(col);
    if (n == 0) return 0;
    ray_t** e = (ray_t**)ray_data(col);
    for (int64_t i = 0; i < n; i++) {
        ray_t* c = e[i];
        if (!c || c->type == -RAY_STR || ray_is_atom(c)) return 0;
        if (!(ray_is_vec(c) || c->type == RAY_LIST) || ray_len(c) != 1) return 0;
    }
    return 1;
}


/* One table cell: sym cells print WITHOUT the backtick (kdb `ibm`). */
void q_cell(ray_t* col, int64_t row, char* out, size_t outsz) {
    out[0] = '\0';
    if (col && col->type == -RAY_STR) {     /* char-column shim: bare char */
        size_t l = ray_str_len(col);
        if (row >= 0 && (size_t)row < l && outsz > 1) {
            out[0] = ray_str_ptr(col)[row];
            out[1] = '\0';
        }
        return;
    }
    if (col && col->type == RAY_CHARV) {    /* char column: bare char cell */
        if (row >= 0 && row < ray_len(col) && outsz > 1) {
            out[0] = ((const char*)ray_data(col))[row];
            out[1] = '\0';
        }
        return;
    }
    ray_t* ia = ray_i64(row);
    ray_t* c  = ray_at_fn(col, ia);
    ray_release(ia);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_release(c); return; }
    if (ray_is_atom(c) && c->type != -RAY_STR && c->type != -RAY_SYM &&
        RAY_ATOM_IS_NULL(c)) {
        /* null cells are BLANK (ref/lj.md) — must precede the float branch */
        ray_release(c);
        return;
    }
    if (c->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(c->i64);
        if (s) { snprintf(out, outsz, "%.*s", (int)ray_str_len(s), ray_str_ptr(s));
                 ray_release(s); }
    } else if (c->type == -RAY_F64 || c->type == -RAY_F32) {
        /* float cells drop the `f` — the column carries the type (`300`) */
        if (RAY_ATOM_IS_NULL(c)) snprintf(out, outsz, c->type == -RAY_F32 ? "0Ne" : "0n");
        else q_float_tok(c->f64, c->type == -RAY_F32, out, outsz);  /* F32 atoms store f64 */
    } else if (c->type == -RAY_BOOL) {
        /* bool cells BARE (ref/greater-than.md:56-59) — no `b` suffix */
        snprintf(out, outsz, "%d", c->u8 ? 1 : 0);
    } else {
        q_fmt(c, out, outsz);
        if (out[0] == ',' && q_col_uniform_singleton(col))
            memmove(out, out + 1, strlen(out));
    }
    ray_release(c);
}

static void q_table_widths(ray_t* tbl, int64_t nc, int64_t nr,
                           int* widths, char hdr[][64]) {
    for (int64_t c = 0; c < nc; c++) {
        ray_t* s = ray_sym_str(ray_table_col_name(tbl, c));
        snprintf(hdr[c], 64, "%.*s", s ? (int)ray_str_len(s) : 0,
                 s ? ray_str_ptr(s) : "");
        if (s) ray_release(s);
        int w = (int)strlen(hdr[c]);
        ray_t* col = ray_table_get_col_idx(tbl, c);
        for (int64_t r = 0; r < nr; r++) {
            char cb[64]; q_cell(col, r, cb, sizeof cb);
            int l = (int)strlen(cb); if (l > w) w = l;
        }
        widths[c] = w;
    }
}

static void q_table_grid(int64_t nc, const int* widths, char hdr[][64]) {
    for (int64_t c = 0; c < nc; c++) {
        if (c) qe_putc(' ');
        qe_pad(hdr[c], widths[c]);
    }
}

static int64_t q_size_rows(int64_t nr) {
    int64_t cr = qe_clip_rows();
    return (cr && cr < nr) ? cr : nr;
}

static void q_fmt_table(ray_t* tbl) {
    int64_t nc = ray_table_ncols(tbl);
    int64_t nr = ray_table_nrows(tbl);
    if (nc <= 0) { qe_puts("+`!()"); return; }   /* empty schema */
    if (nc > QF_MAXCOL) nc = QF_MAXCOL;
    int  widths[QF_MAXCOL];
    char hdr[QF_MAXCOL][64];
    q_table_widths(tbl, nc, q_size_rows(nr), widths, hdr);

    q_table_grid(nc, widths, hdr);
    qe_trim();
    qe_putc('\n');
    int total = (int)(nc - 1);
    for (int64_t c = 0; c < nc; c++) total += widths[c];
    for (int i = 0; i < total; i++) qe_putc('-');
    qe_putc('\n');
    for (int64_t r = 0; r < nr; r++) {
        for (int64_t c = 0; c < nc; c++) {
            if (c) qe_putc(' ');
            char cb[64]; q_cell(ray_table_get_col_idx(tbl, c), r, cb, sizeof cb);
            qe_pad(cb, widths[c]);
        }
        qe_trim();
        if (r + 1 < nr) qe_putc('\n');
        if (qe_done()) break;                    /* height cap hit — early exit */
    }
}

static void q_fmt_keyed(ray_t* kt, ray_t* vt) {
    int64_t knc = ray_table_ncols(kt), knr = ray_table_nrows(kt);
    int64_t vnc = ray_table_ncols(vt), vnr = ray_table_nrows(vt);
    int64_t nr  = knr < vnr ? knr : vnr;
    if (knc > QF_MAXCOL) knc = QF_MAXCOL;
    if (vnc > QF_MAXCOL) vnc = QF_MAXCOL;
    int  kw[QF_MAXCOL], vw[QF_MAXCOL];
    char kh[QF_MAXCOL][64], vh[QF_MAXCOL][64];
    q_table_widths(kt, knc, q_size_rows(nr), kw, kh);
    q_table_widths(vt, vnc, q_size_rows(nr), vw, vh);

    q_table_grid(knc, kw, kh);
    qe_putc('|'); qe_putc(' ');
    q_table_grid(vnc, vw, vh);
    qe_trim();
    qe_putc('\n');
    int kt_tot = (int)(knc - 1); for (int64_t c = 0; c < knc; c++) kt_tot += kw[c];
    int vt_tot = (int)(vnc - 1); for (int64_t c = 0; c < vnc; c++) vt_tot += vw[c];
    for (int i = 0; i < kt_tot; i++) qe_putc('-');
    qe_putc('|'); qe_putc(' ');
    for (int i = 0; i < vt_tot; i++) qe_putc('-');
    qe_trim();
    qe_putc('\n');
    for (int64_t r = 0; r < nr; r++) {
        for (int64_t c = 0; c < knc; c++) {
            if (c) qe_putc(' ');
            char cb[64]; q_cell(ray_table_get_col_idx(kt, c), r, cb, sizeof cb);
            qe_pad(cb, kw[c]);
        }
        qe_putc('|'); qe_putc(' ');
        for (int64_t c = 0; c < vnc; c++) {
            if (c) qe_putc(' ');
            char cb[64]; q_cell(ray_table_get_col_idx(vt, c), r, cb, sizeof cb);
            qe_pad(cb, vw[c]);
        }
        qe_trim();
        if (r + 1 < nr) qe_putc('\n');
        if (qe_done()) break;                    /* height cap hit — early exit */
    }
}

/* Integer-backed sentinels (kdb datatypes table): MIN->0N, MAX->0W, -MAX->-0W, + suffix (0=bare). */
static int q_sentinel_tok(int64_t v, int width, char suffix, char* out, size_t n) {
    int64_t vmax = (width == 2) ? INT16_MAX : (width == 4) ? INT32_MAX : INT64_MAX;
    const char* base = (v == -vmax - 1) ? "0N" : (v == vmax) ? "0W"
                     : (v == -vmax)     ? "-0W" : NULL;
    if (!base) return 0;
    char sfx[2] = { suffix, '\0' };
    snprintf(out, n, "%s%s", base, sfx);
    return 1;
}

static void q_int_tok(int64_t v, int width, char suffix, char* out, size_t n) {
    if (q_sentinel_tok(v, width, suffix, out, n)) return;
    char sfx[2] = { suffix, '\0' };
    snprintf(out, n, "%lld%s", (long long)v, sfx);
}

/* Float token: NaN->0n/0Ne; wholes within `\P` print integral (`5`), past
 * the horizon exponent-form (timespan.qcmd:162 pins 3e+11); else %.*g.  The
 * 10^prec horizon must be EXACT powers of 10 (pow() drift would move the
 * `\P 7` boundary off 1e7).  Infinities: out of scope (sentinel-null). */
void q_float_tok(double v, int f32, char* out, size_t n) {
    if (isnan(v)) { snprintf(out, n, f32 ? "0Ne" : "0n"); return; }
    int prec = g_print_prec ? g_print_prec : 17;
    static const double POW10[18] = {
        1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,
        1e10,1e11,1e12,1e13,1e14,1e15,1e16,1e17 };
    double horizon = POW10[prec < 0 ? 0 : prec > 17 ? 17 : prec];
    if (isfinite(v) && v == floor(v) && v > -horizon && v < horizon) {
        snprintf(out, n, "%lld%s", (long long)v, f32 ? "e" : "");
        return;
    }
    char mag[64];
    snprintf(mag, sizeof mag, "%.*g", prec, v);
    snprintf(out, n, "%.48s%s", mag, f32 ? "e" : "");
}

/* Digit-only token — the shape test behind the f64 `f` suffix (`256f`). */
static int q_tok_is_bare_int(const char* tok) {
    if (*tok == '-') tok++;
    if (!*tok) return 0;
    for (; *tok; tok++)
        if (*tok < '0' || *tok > '9') return 0;
    return 1;
}

/* Payload via a temp base atom (base fmt owns the math); consumes `a`. */
static void q_tok_via_atom(ray_t* a, char* out, size_t n) {
    out[0] = '\0';
    if (a && !RAY_IS_ERR(a)) {
        ray_fallback(a, out, n);
        ray_release(a);
    }
}

/* Date payload; out-of-civil-range displays 0000.00.00 (datatypes.md). */
static void q_date_payload(int64_t v, char* out, size_t n) {
    if (v < q_days_from_civil(1, 1, 1) || v > q_days_from_civil(9999, 12, 31)) {
        snprintf(out, n, "0000.00.00");
        return;
    }
    q_tok_via_atom(ray_date(v), out, n);
}

/* Month payload: months since 2000.01; outside year [1,9999] -> `0000.00`. */
static void q_month_payload(int64_t p, char* out, size_t n) {
    int64_t y = 2000 + (p >= 0 ? p / 12 : -((-p + 11) / 12));
    int64_t m = 1 + (p % 12 + 12) % 12;
    if (y < 1 || y > 9999) { snprintf(out, n, "0000.00"); return; }
    snprintf(out, n, "%04lld.%02lld", (long long)y, (long long)m);
}

/* GUID: 8-4-4-4-12 lowercase hex; null = zero UUID, never 0Ng (datatypes.md). */
static void q_guid_tok(const uint8_t* b16, char* out, size_t n) {
    if (n == 0) return;
    static const char hx[] = "0123456789abcdef";
    static const int groups[] = {4, 2, 2, 2, 6};
    size_t pos = 0; int bi = 0;
    for (int g = 0; g < 5; g++) {
        if (g && pos + 1 < n) out[pos++] = '-';
        for (int j = 0; j < groups[g] && pos + 2 < n; j++) {
            out[pos++] = hx[b16[bi] >> 4];
            out[pos++] = hx[b16[bi] & 0xf];
            bi++;
        }
    }
    out[pos < n ? pos : n - 1] = '\0';
}

/* Datetime (f64 — Q_TTOK width 0): NaN->0Nz (no live 0Wz, 2026-07-09);
 * out-of-range -> 0000.00.00T00:00:00.000; ms precision (tok.md:227). */
static void q_datetime_tok(double v, char* out, size_t n) {
    if (v != v) { snprintf(out, n, "0Nz"); return; }
    if (v < (double)q_days_from_civil(1, 1, 1) ||
        v >= (double)(q_days_from_civil(9999, 12, 31) + 1)) {
        snprintf(out, n, "0000.00.00T00:00:00.000");
        return;
    }
    q_tok_via_atom(ray_datetime(v), out, n);
}

/* THE temporal token table: sentinel suffix (0=bare), once-per-value trailing
 * char (month's `m`, basics/syntax.md:164), width (0 = f64), ctor-or-payload. */
typedef struct {
    int8_t  type;                               /* RAY_* vector type (atom: -type) */
    char    sfx;                                /* sentinel suffix letter */
    char    vsfx;                               /* trailing char, once per value */
    int     width;                              /* element bytes; 0 = f64 */
    ray_t* (*ctor)(int64_t);                    /* payload via temp atom … */
    void   (*payload)(int64_t, char*, size_t);  /* … unless overridden here */
} q_ttok_t;

static const q_ttok_t Q_TTOK[] = {
    { RAY_DATE,      'd', 0,   4, NULL,          q_date_payload },
    { RAY_MONTH,      0,  'm', 4, NULL,          q_month_payload },
    { RAY_TIME,      't', 0,   4, ray_time,      NULL },
    { RAY_MINUTE,    'u', 0,   4, ray_minute,    NULL },
    { RAY_SECOND,    'v', 0,   4, ray_second,    NULL },
    { RAY_TIMESPAN,  'n', 0,   8, ray_timespan,  NULL },
    { RAY_TIMESTAMP, 'p', 0,   8, ray_timestamp, NULL },
    { RAY_DATETIME,   0,  0,   0, NULL,          NULL },
};

static const q_ttok_t* q_ttok_find(int8_t t) {
    for (size_t i = 0; i < sizeof Q_TTOK / sizeof *Q_TTOK; i++)
        if (Q_TTOK[i].type == t) return &Q_TTOK[i];
    return NULL;
}

static void q_ttok_elem(const q_ttok_t* r, ray_t* v, int64_t i,
                        char* out, size_t n) {
    if (r->width == 0) {
        q_datetime_tok(i < 0 ? v->f64 : ((const double*)ray_data(v))[i], out, n);
        return;
    }
    int64_t x = (i < 0)
        ? (r->width == 4 ? (int64_t)v->i32 : v->i64)
        : (r->width == 4 ? (int64_t)((const int32_t*)ray_data(v))[i]
                         : ((const int64_t*)ray_data(v))[i]);
    if (q_sentinel_tok(x, r->width, r->sfx, out, n)) return;
    if (r->payload) { r->payload(x, out, n); return; }
    q_tok_via_atom(r->ctor(x), out, n);
}

static void q_fmt_dict_key(ray_t* key, char* out, size_t cap) {
    out[0] = '\0';
    if (!key || RAY_IS_ERR(key)) return;

    if (key->type == -RAY_SYM) {     /* bare, never backticked */
        ray_t* s = ray_sym_str(key->i64);
        if (s) {
            snprintf(out, cap, "%.*s", (int)ray_str_len(s), ray_str_ptr(s));
            ray_release(s);
        }
        return;
    }
    if (key->type == -RAY_BOOL) {
        snprintf(out, cap, "%d", key->u8 ? 1 : 0);
        return;
    }
    if (key->type == -RAY_I16 || key->type == -RAY_I32 || key->type == -RAY_I64) {
        int width = (key->type == -RAY_I16) ? 2 : (key->type == -RAY_I32) ? 4 : 8;
        int64_t v = (key->type == -RAY_I16) ? (int64_t)key->i16
                  : (key->type == -RAY_I32) ? (int64_t)key->i32
                  : key->i64;
        q_int_tok(v, width, 0, out, cap);
        return;
    }
    if (key->type == -RAY_F32 || key->type == -RAY_F64) {
        q_float_tok(key->f64, key->type == -RAY_F32, out, cap);
        return;
    }

    q_fmt(key, out, cap);
}

static void qe_join(const char* tok, int first) {
    size_t tl = strlen(tok);
    if (!qe_fits(tl + (first ? 0 : 1))) return;   /* historical whole-token skip */
    if (!first) qe_putc(' ');
    qe_putn(tok, tl);
}

/* Parse-tree probe — sole consumer: the q_fmt_console clip exemption (parse
 * display is byte-for-byte contract, CLAUDE.md rule 2, never clips).  Tree
 * iff head is a ctor, fn value, name-ref sym (0x20 clear) or clause list. */
#define Q_ATTR_QUOTED 0x20
static int q_list_is_parse_tree(ray_t* v, int depth) {
    if (!v || v->type != RAY_LIST || depth > 64) return 0;
    int64_t n = ray_len(v);
    if (n < 1) return 0;
    ray_t* h = ((ray_t**)ray_data(v))[0];
    if (!h) return 0;
    if (h == q_registry_list_value() || h == q_registry_table_value()) return 1;
    if (h->type == RAY_UNARY || h->type == RAY_BINARY || h->type == RAY_VARY)
        return 1;
    if (h->type == -RAY_SYM && !(h->attrs & Q_ATTR_QUOTED)) return 1;
    if (h->type == RAY_LIST) return q_list_is_parse_tree(h, depth + 1);
    return 0;
}

/* Empty-vector name (`long$()`) via the guarded q_type_qname home; byte alone
 * stays bare-0x (byte_impl.qcmd:120 pin), so it names no `byte$()`. Composing on
 * the single home means the #209 value-band guard there transitively protects
 * empty-vec display — no parallel table to keep in sync. */
static const char* q_empty_vec_qname(int8_t type) {
    if (type == RAY_BYTE_ONLY) return NULL;
    return q_type_qname(type);
}

/* Escape one byte kdb-style — THE display-inverse of the scanner decode. */
static size_t q_char_esc(unsigned char ch, char out[8]) {
    switch (ch) {
    case '"':  out[0] = '\\'; out[1] = '"';  return 2;
    case '\\': out[0] = '\\'; out[1] = '\\'; return 2;
    case '\n': out[0] = '\\'; out[1] = 'n';  return 2;
    case '\t': out[0] = '\\'; out[1] = 't';  return 2;
    case '\r': out[0] = '\\'; out[1] = 'r';  return 2;
    default:
        if (ch < 32) return (size_t)snprintf(out, 8, "\\%03o", ch);
        out[0] = (char)ch;
        return 1;
    }
}

/* Quoted-text renderer over raw bytes — shared by the -RAY_STR atom form and
 * the charv vector/atom forms. */
static void q_fmt_qtext(const char* p, size_t n, char* buf, size_t bufsz) {
    size_t w = 0;
    if (w + 1 < bufsz) buf[w++] = '"';
    for (size_t i = 0; i < n && w + 6 < bufsz; i++) {
        char e[8];
        size_t el = q_char_esc((unsigned char)p[i], e);
        memcpy(buf + w, e, el);
        w += el;
    }
    if (w + 1 < bufsz) buf[w++] = '"';
    buf[w < bufsz ? w : bufsz - 1] = '\0';
}

static void q_fmt_qstring(ray_t* val, char* buf, size_t bufsz) {
    const char* p = ray_str_ptr(val);
    size_t n = ray_str_len(val);
    size_t w = 0;
    if (w + 1 < bufsz) buf[w++] = '"';
    for (size_t i = 0; i < n && w + 6 < bufsz; i++) {
        char e[8];
        size_t el = q_char_esc((unsigned char)p[i], e);
        memcpy(buf + w, e, el);
        w += el;
    }
    if (w + 1 < bufsz) buf[w++] = '"';
    buf[w < bufsz ? w : bufsz - 1] = '\0';
}

static void qe_qstring(ray_t* val) {
    const char* p = ray_str_ptr(val);
    size_t n = ray_str_len(val);
    qe_putc('"');
    for (size_t i = 0; i < n; i++) {
        if (qe_line_done()) break;               /* clip: line already decided */
        char e[8];
        qe_putn(e, q_char_esc((unsigned char)p[i], e));
    }
    qe_putc('"');
}

/* Alignable = space-separated element types; bool/byte rows print whole. */
static int q_matrix_alignable(int8_t type) {
    return type == RAY_I16 || type == RAY_I32 || type == RAY_I64 ||
           type == RAY_F32 || type == RAY_F64 || type == RAY_SYM ||
           type == RAY_DATE || type == RAY_TIME || type == RAY_TIMESTAMP ||
           type == RAY_MINUTE || type == RAY_SECOND || type == RAY_TIMESPAN ||
           type == RAY_DATETIME;
}

static void q_matrix_cell(ray_t* rv, int64_t c, char* out, size_t outsz) {
    out[0] = '\0';
    switch (rv->type) {
    case RAY_I16: q_int_tok((int64_t)((const int16_t*)ray_data(rv))[c], 2, 0, out, outsz); break;
    case RAY_I32: q_int_tok((int64_t)((const int32_t*)ray_data(rv))[c], 4, 0, out, outsz); break;
    case RAY_I64: q_int_tok(((const int64_t*)ray_data(rv))[c],          8, 0, out, outsz); break;
    case RAY_F32: q_float_tok((double)((const float*)ray_data(rv))[c],  1, out, outsz); break;
    case RAY_F64: q_float_tok(((const double*)ray_data(rv))[c],         0, out, outsz); break;
    case RAY_SYM: {
        ray_t* s = ray_sym_vec_cell(rv, c);   /* borrowed -RAY_STR */
        if (s) snprintf(out, outsz, "%.*s", (int)ray_str_len(s), ray_str_ptr(s));
        break;
    }
    case RAY_LIST: {
        /* boxed row of atoms: syms BARE, strings quoted (len-1 as ,"c") */
        ray_t* a = ((ray_t**)ray_data(rv))[c];
        if (!a) break;
        if (a->type == -RAY_SYM) {
            ray_t* s = ray_sym_str(a->i64);   /* borrowed */
            if (s) snprintf(out, outsz, "%.*s", (int)ray_str_len(s), ray_str_ptr(s));
        } else if (a->type == -RAY_STR) {
            if (ray_str_len(a) == 1 && outsz > 1) {
                out[0] = ',';
                q_fmt_qstring(a, out + 1, outsz - 1);
            } else
                q_fmt_qstring(a, out, outsz);
        } else if (a->type == -RAY_CHARV) {
            q_fmt_qtext((const char*)&a->u8, 1, out, outsz);
        } else if (a->type == RAY_CHARV) {
            if (ray_len(a) == 1 && outsz > 1) {
                out[0] = ',';
                q_fmt_qtext((const char*)ray_data(a), 1, out + 1, outsz - 1);
            } else
                q_fmt_qtext((const char*)ray_data(a), (size_t)ray_len(a), out, outsz);
        } else
            q_fmt(a, out, outsz);
        break;
    }
    default: {
        const q_ttok_t* tr = q_ttok_find(rv->type);
        if (tr) q_ttok_elem(tr, rv, c, out, outsz);
        break;
    }
    }
}

static int q_matrix_row_ok(ray_t* r) {
    if (!r || RAY_IS_ERR(r)) return 0;
    if (r->type > 0 && ray_is_vec(r) && q_matrix_alignable(r->type)) return 1;
    if (r->type == RAY_LIST) {
        ray_t** it = (ray_t**)ray_data(r);
        int64_t n = ray_len(r);
        for (int64_t i = 0; i < n; i++)
            if (!it[i] || RAY_IS_ERR(it[i]) ||
                !(ray_is_atom(it[i]) || it[i]->type == RAY_CHARV) ||   /* strings are cells */
                RAY_IS_NULL(it[i]))
                return 0;
        return 1;
    }
    return 0;
}

/* e[0..n) is >=2 same-length alignable rows: a LEFT-aligned matrix (ref/mmu.md). */
static int q_is_matrix(ray_t** e, int64_t n) {
    if (n < 2) return 0;
    if (!q_matrix_row_ok(e[0])) return 0;
    int64_t w = ray_len(e[0]);
    if (w == 0) return 0;
    for (int64_t i = 1; i < n; i++)
        if (!q_matrix_row_ok(e[i]) || ray_len(e[i]) != w) return 0;
    return 1;
}

static void q_fmt_matrix(ray_t** e, int64_t nr) {
    int64_t nc = ray_len(e[0]);
    /* no fixed column cap; clip-armed sizing scans showable rows only */
    int  stackw[64];
    int* widths = (nc <= 64) ? stackw : malloc((size_t)(nc > 0 ? nc : 1) * sizeof(int));
    if (!widths) return;
    int64_t nr_size = q_size_rows(nr);
    for (int64_t c = 0; c < nc; c++) {
        int w = 0;
        for (int64_t r = 0; r < nr_size; r++) {
            char cb[512]; q_matrix_cell(e[r], c, cb, sizeof cb);
            int l = (int)strlen(cb); if (l > w) w = l;
        }
        widths[c] = w;
    }
    for (int64_t r = 0; r < nr; r++) {
        if (qe_done()) break;                    /* height cap hit — early exit */
        if (r) qe_putc('\n');
        for (int64_t c = 0; c < nc; c++) {
            if (c) qe_putc(' ');
            char cb[512]; q_matrix_cell(e[r], c, cb, sizeof cb);
            qe_pad(cb, widths[c]);                    /* left-align */
        }
        qe_trim();                                    /* no trailing spaces */
    }
    if (widths != stackw) free(widths);
}

static void q_fmt_body(ray_t* val);   /* fwd */

/* Attributed vectors get `` `s#`` (q_attr_letter — shared with `attr`); table columns stay bare via q_cell. */
static void q_fmt_render(ray_t* val) {
    if (!val) return;
    char al = q_attr_letter(val);              /* 0 unless an attributed vector */
    if (al) {
        qe_putc('`'); qe_putc(al); qe_putc('#');
    }
    q_fmt_body(val);
}

/* Public entry, UNCLIPPED (`string`, `-3!`, CSV, recursive renders). */
void q_fmt(ray_t* val, char* buf, size_t bufsz) {
    if (bufsz == 0 || !buf) return;
    qe_push(buf, bufsz, 0);
    q_fmt_render(val);
    qe_pop();
}

/* Public entry, CONSOLE: the `\c` clip when armed; unarmed — or a parse tree — equals q_fmt. */
void q_fmt_console(ray_t* val, char* buf, size_t bufsz) {
    if (bufsz == 0 || !buf) return;
    /* `--nonlegacy` (OFF by default): tables render as the pipe table.  Gated
     * HERE — the console seam — and never in q_fmt_body, which the UNCLIPPED
     * q_fmt (`string`, `-3!`, CSV, every cell) shares and must keep legacy. */
    if (q_pipe_on() && q_pipe_is_table(val)) { q_pipe_console(val, buf, bufsz); return; }
    int32_t rows = 0, cols = 0;
    int armed = q_con_display(&rows, &cols) && !g_clip_active;
    if (armed && val && val->type == RAY_LIST && q_list_is_parse_tree(val, 0))
        armed = 0;                             /* parse display NEVER clips */
    if (!armed || rows < 10 || cols < 10 || cols > 2000) {
        q_fmt(val, buf, bufsz);
        return;
    }
    g_clip_active = 1;
    g_clip.rows = rows;   g_clip.cols = cols;
    g_clip.nlines = 0;    g_clip.stop = 0;
    g_clip.llen = g_clip.llog = g_clip.ltrail = 0;
    qe_push(buf, bufsz, 1);
    q_fmt_render(val);
    qe_pop();
    g_clip_active = 0;
}

static void q_fmt_body(ray_t* val) {
    if (!val) return;

    /* generic null prints `::` (top-level silence is the CALLER's rule) */
    if (RAY_IS_NULL(val)) {
        qe_puts("::");
        return;
    }

    /* char atom / char vector: kdb quoted forms — "a", "abc", ,"a", "" —
     * BEFORE the empty-typed-vector arm ("" is the empty charv, never `char$()) */
    if (val->type == -RAY_CHARV) {
        char e[8];
        qe_putc('"');
        qe_putn(e, q_char_esc(val->u8, e));
        qe_putc('"');
        return;
    }
    if (val->type == RAY_CHARV) {
        int64_t n = ray_len(val);
        const char* p = (const char*)ray_data(val);
        if (n == 1) qe_putc(',');                /* len-1 vector: ,"a" */
        qe_putc('"');
        for (int64_t i = 0; i < n; i++) {
            if (qe_line_done()) break;
            char e[8];
            qe_putn(e, q_char_esc((unsigned char)p[i], e));
        }
        qe_putc('"');
        return;
    }

    /* empty typed vector: `` `type$() `` (byte keeps its bare-0x arm) */
    if (val->type > 0 && ray_is_vec(val) && ray_len(val) == 0) {
        const char* qn = q_empty_vec_qname(val->type);
        if (qn) { qe_printf("`%s$()", qn); return; }
    }

    if (val->type == -RAY_SYM) {
        qe_sym(val);
        return;
    }

    if (val->type == -RAY_STR) {
        qe_qstring(val);
        return;
    }

    /* string vector: one quoted line per item; `,` on singleton and len-1 */
    if (val->type == RAY_STR && ray_is_vec(val)) {
        int64_t n = ray_len(val);
        if (n == 0) { qe_puts("()"); return; }   /* kdb: empty list */
        for (int64_t i = 0; i < n; i++) {
            if (qe_done()) break;                /* height cap hit — early exit */
            ray_t* ia = ray_i64(i);
            ray_t* it = ray_at_fn(val, ia);
            ray_release(ia);
            if (!it || RAY_IS_ERR(it)) { if (it) ray_release(it); return; }
            char elem[2048]; elem[0] = '\0';
            if (n == 1 || (it->type == -RAY_STR && ray_str_len(it) == 1)) {
                elem[0] = ',';
                q_fmt_qstring(it, elem + 1, sizeof elem - 1);
            } else
                q_fmt_qstring(it, elem, sizeof elem);
            ray_release(it);
            if (i) qe_putc('\n');
            qe_puts(elem);
        }
        return;
    }

    /* fn values render their provenance spelling: `+`, `-:`, keywords */
    if (val->type == RAY_UNARY || val->type == RAY_BINARY ||
        val->type == RAY_VARY) {
        /* the paren-list ctor IS kdb's enlist (basics/parsetrees.md fby) */
        if (val == q_registry_list_value()) { qe_puts("enlist"); return; }
        q_provenance_t pv;
        if (q_registry_provenance(val, &pv) && pv.spelling && pv.spelling[0]) {
            char c0 = pv.spelling[0];
            int glyph = !((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'));
            qe_puts(pv.spelling);
            if (pv.valence == Q_MONADIC && glyph) qe_putc(':');
            return;
        }
    }

    {
        char tok[64]; tok[0] = '\0';
        const q_ttok_t* tr = (val->type < 0) ? q_ttok_find((int8_t)-val->type)
                                             : NULL;
        if (tr) {
            q_ttok_elem(tr, val, -1, tok, sizeof tok);
            qe_puts(tok);
            if (tr->vsfx) qe_putc(tr->vsfx);
            return;
        }
        switch (val->type) {
        case -RAY_BOOL: qe_printf("%db", val->u8 ? 1 : 0);                 return;
        case -RAY_BYTE_ONLY: qe_printf("0x%02x", val->u8);                      return;
        case -RAY_I16:  q_int_tok((int64_t)val->i16, 2, 'h', tok, sizeof tok);
                        qe_puts(tok);                                      return;
        case -RAY_I32:  q_int_tok((int64_t)val->i32, 4, 'i', tok, sizeof tok);
                        qe_puts(tok);                                      return;
        case -RAY_I64:  q_int_tok(val->i64,          8, 0,   tok, sizeof tok);
                        qe_puts(tok);                                      return;
        case -RAY_GUID: {
            const uint8_t* b16 = val->obj ? (const uint8_t*)ray_data(val->obj)
                                          : (const uint8_t*)ray_data(val);
            q_guid_tok(b16, tok, sizeof tok);
            qe_puts(tok);
            return;
        }
        case -RAY_F32:  q_float_tok((float)val->f64, 1, tok, sizeof tok);
                        qe_puts(tok);                                      return;
        case -RAY_F64: {
            /* digit-only tokens take `f` (`5f`); `3e+11` self-identifies */
            q_float_tok(val->f64, 0, tok, sizeof tok);
            if (q_tok_is_bare_int(tok)) {
                size_t l = strlen(tok);
                if (l + 1 < sizeof tok) { tok[l] = 'f'; tok[l + 1] = '\0'; }
            }
            qe_puts(tok);
            return;
        }
        default: break;
        }
    }

    if (val->type == RAY_BOOL) {            /* 1001b */
        int64_t n = ray_len(val);
        const uint8_t* d = (const uint8_t*)ray_data(val);
        if (n == 1) qe_putc(',');                          /* enlist: ,0b */
        for (int64_t i = 0; i < n && !qe_line_done(); i++)
            qe_putc(d[i] ? '1' : '0');
        qe_putc('b');
        return;
    }
    /* byte vector: 0x + hex pairs (ref/sv.md); empty is bare `0x` */
    if (val->type == RAY_BYTE_ONLY) {
        int64_t n = ray_len(val);
        const uint8_t* d = (const uint8_t*)ray_data(val);
        static const char hx[] = "0123456789abcdef";
        if (n == 1) qe_putc(',');
        if (qe_fits(2)) { qe_putc('0'); qe_putc('x'); }
        for (int64_t i = 0; i < n && !qe_line_done(); i++) {
            if (!qe_fits(2)) break;
            qe_putc(hx[d[i] >> 4]);
            qe_putc(hx[d[i] & 0xf]);
        }
        return;
    }
    if (val->type == RAY_GUID) {
        int64_t n = ray_len(val);
        const uint8_t* d = (const uint8_t*)ray_data(val);
        if (n == 1) qe_putc(',');
        for (int64_t i = 0; i < n && !qe_line_done(); i++) {
            if (i) qe_putc(' ');
            char e[40];
            q_guid_tok(d + i * 16, e, sizeof e);
            if (qe_fits(strlen(e))) qe_puts(e);
        }
        return;
    }
    /* int vector: bare elements + type char ONCE (`0N 0W -0W 42h`) */
    if (val->type == RAY_I16 || val->type == RAY_I32 || val->type == RAY_I64) {
        int width  = (val->type == RAY_I16) ? 2 : (val->type == RAY_I32) ? 4 : 8;
        char vsuf  = (val->type == RAY_I16) ? 'h' : (val->type == RAY_I32) ? 'i' : 0;
        int64_t n = ray_len(val);
        if (n == 1) qe_putc(',');                          /* enlist: ,42h */
        for (int64_t i = 0; i < n && !qe_line_done(); i++) {
            char e[64];
            int64_t v = (width == 2) ? (int64_t)((const int16_t*)ray_data(val))[i]
                      : (width == 4) ? (int64_t)((const int32_t*)ray_data(val))[i]
                      :                ((const int64_t*)ray_data(val))[i];
            q_int_tok(v, width, 0, e, sizeof e);   /* bare — no per-element suffix */
            qe_join(e, i == 0);
        }
        if (vsuf) qe_putc(vsuf);
        return;
    }
    /* temporal vector: bare tokens space-joined; only month has a trailing
     * type char (`m`) — the other temporals' full tokens self-identify,
     * sentinels included (`2000.01.01 0Nd`) */
    {
        const q_ttok_t* tr = q_ttok_find(val->type);
        if (tr) {
            int64_t n = ray_len(val);
            if (n == 1) qe_putc(',');                      /* enlist: ,2000.01.01 */
            for (int64_t i = 0; i < n && !qe_line_done(); i++) {
                char e[64];
                q_ttok_elem(tr, val, i, e, sizeof e);
                qe_join(e, i == 0);
            }
            if (tr->vsfx) qe_putc(tr->vsfx);
            return;
        }
    }

    if (val->type == RAY_F32 || val->type == RAY_F64) {
        int is64 = (val->type == RAY_F64);
        int64_t n = ray_len(val);
        if (n == 1) qe_putc(',');                          /* enlist: ,1f */
        int all_whole = (n > 0);   /* f64 gets ONE trailing `f` iff every element RENDERS digit-only */
        for (int64_t i = 0; i < n && !qe_line_done(); i++) {
            double v = is64 ? ((const double*)ray_data(val))[i]
                            : (double)((const float*)ray_data(val))[i];
            char e[64];
            q_float_tok(v, is64 ? 0 : 1, e, sizeof e);
            qe_join(e, i == 0);
            if (!q_tok_is_bare_int(e)) all_whole = 0;
        }
        /* all-digit-token f64 vectors take ONE trailing `f` (`1 2 3f`); a
         * clipped early exit's stale all_whole is swallowed past the dots */
        if (is64 && all_whole) qe_putc('f');
        return;
    }

    /* sym vector: `a`b`c — every element backticked (data must round-trip) */
    if (val->type == RAY_SYM) {
        int64_t n = ray_len(val);
        if (n == 1) qe_putc(',');                          /* enlist: ,`a */
        for (int64_t i = 0; i < n && !qe_line_done(); i++) {
            ray_t* s = ray_sym_vec_cell(val, i);   /* borrowed -RAY_STR */
            const char* nm = ray_str_ptr(s);
            size_t l = ray_str_len(s);
            if (!qe_fits(1 + l)) break;
            qe_putc('`');
            qe_putn(nm, l);
        }
        return;
    }

    if (q_deriv_kind_of(val) == Q_DERIV_LAMBDA) {   /* verbatim q source */
        ray_t* s = q_lambda_src(val);
        if (s && s->type == -RAY_STR) {
            qe_putn(ray_str_ptr(s), ray_str_len(s));
            return;
        }
    }

    /* 104h carrier: bound-verb + adverb glyph (+/); base = the HOF value */
    if (q_deriv_kind_of(val) == Q_DERIV_PROJ && ray_len(val) >= 5) {
        ray_t* base = q_deriv_base(val);
        ray_t* v0   = ((ray_t**)ray_data(val))[4];
        const char* g = NULL;
        if (base == q_registry_over_value() ||
            base == ray_env_get(ray_sym_intern("fold", 4)))            g = "/";
        else if (base == q_registry_scan_value())                      g = "\\";
        else if (base == q_registry_lookup_name("each", 4, Q_DYADIC))  g = "'";
        else if (base == q_registry_prior_value())                     g = "':";
        else if (base == ray_env_get(ray_sym_intern("map-right", 9)))  g = "/:";
        else if (base == ray_env_get(ray_sym_intern("map-left", 8)))   g = "\\:";
        if (g && v0) {
            char vb[256]; vb[0] = '\0';
            q_fmt(v0, vb, sizeof vb);
            qe_puts(vb);
            qe_puts(g);
            return;
        }
        /* general projection: base[a0;a1;...] with hole slots EMPTY */
        {
            uint64_t mask  = q_deriv_hole_mask(val);
            int64_t  slots = ray_len(val) - 4;
            ray_t**  e     = (ray_t**)ray_data(val);
            char bb[512]; bb[0] = '\0';
            q_fmt(base, bb, sizeof bb);
            qe_puts(bb);
            qe_putc('[');
            for (int64_t i = 0; i < slots && qe_fits(2); i++) {
                if (i) qe_putc(';');
                if (!(mask & (1ull << i))) {
                    char ab[256]; ab[0] = '\0';
                    q_fmt(e[4 + i], ab, sizeof ab);
                    qe_puts(ab);
                }
            }
            qe_putc(']');
            return;
        }
    }

    if (val->type == RAY_TABLE) {
        q_fmt_table(val);
        return;
    }

    if (val->type == RAY_DICT) {            /* keyed table = table!table */
        ray_t* kk = ray_dict_keys(val);
        ray_t* vv = ray_dict_vals(val);
        if (kk && vv && kk->type == RAY_TABLE && vv->type == RAY_TABLE) {
            q_fmt_keyed(kk, vv);
            return;
        }
    }

    if (val->type == RAY_DICT) {            /* dict: `key| value` rows */
        ray_t* k = ray_dict_keys(val);          /* borrowed */
        ray_t* v = ray_dict_vals(val);          /* borrowed */
        int64_t n = k ? ray_len(k) : 0;
        size_t maxk = 0;
        /* uniform sym value column prints BARE (ref/apply.md); mixed keeps ` */
        int sym_col = v && v->type == RAY_SYM;
        if (v && v->type == RAY_LIST && ray_len(v) > 0) {
            ray_t** vel = (ray_t**)ray_data(v);
            sym_col = 1;
            for (int64_t i = 0; i < ray_len(v) && sym_col; i++)
                if (!vel[i] || vel[i]->type != -RAY_SYM) sym_col = 0;
        }
        int64_t n_size = q_size_rows(n);   /* clip-armed: size showable rows only */
        for (int pass = 0; pass < 2; pass++) {
            int64_t n_pass = (pass == 0) ? n_size : n;
            for (int64_t i = 0; i < n_pass; i++) {
                ray_t* ia = ray_i64(i);
                ray_t* ke = ray_at_fn(k, ia);
                ray_release(ia);
                char kb[256]; kb[0] = '\0';
                q_fmt_dict_key(ke, kb, sizeof kb);
                if (ke && !RAY_IS_ERR(ke)) ray_release(ke);
                size_t kl = strlen(kb);
                if (pass == 0) {
                    if (kl > maxk) maxk = kl;
                    continue;
                }
                char vb[1024]; vb[0] = '\0';
                ray_t* ja = ray_i64(i);
                ray_t* ve = v ? ray_at_fn(v, ja) : NULL;
                ray_release(ja);
                if (ve && !RAY_IS_ERR(ve)) {
                    /* len-1 strings render "c" NOT ,"c" (list/first pins `c| "h"`) */
                    if (sym_col && ve->type == -RAY_SYM)
                        q_fmt_dict_key(ve, vb, sizeof vb);
                    else if (ve->type == -RAY_BOOL)
                        /* bool ATOM rows BARE; vectors keep `100b` */
                        q_fmt_dict_key(ve, vb, sizeof vb);
                    else
                        q_fmt(ve, vb, sizeof vb);
                }
                if (ve && !RAY_IS_ERR(ve)) ray_release(ve);
                size_t need = (i ? 1 : 0) + maxk + 2 + strlen(vb);
                if (!qe_fits(need)) break;
                qe_printf("%s%-*s| %s", i ? "\n" : "", (int)maxk, kb, vb);
            }
        }
        return;
    }

    /* General list, kdb-true: one item per line, each a single-line k-repr —
     * parse trees and values alike (basics/parsetrees.md; implicit-iteration
     * .md pins nested items inline `(110b;0b)`).  TABLE-ctor head hidden;
     * one item = `,x`; rectangular = aligned matrix (ref/mmu.md). */
    if (val->type == RAY_LIST) {
        int64_t n = ray_len(val);
        ray_t** e = (ray_t**)ray_data(val);
        ray_t* tv = q_registry_table_value();
        if (tv && n >= 1 && e[0] == tv) { e++; n--; }
        /* a bare constructor (parse "()") is the empty-list application */
        if (n == 1 && e[0] == q_registry_list_value()) { qe_puts("()"); return; }
        if (n == 0) { qe_puts("()"); return; }
        if (n == 1) {                             /* enlist: ,x */
            char elem[2048]; elem[0] = '\0';
            q_fmt_krepr(e[0], elem, sizeof elem);
            qe_putc(',');
            qe_puts(elem);
            return;
        }
        if (q_is_matrix(e, n)) { q_fmt_matrix(e, n); return; }
        for (int64_t i = 0; i < n; i++) {
            if (qe_done()) break;                /* height cap hit — early exit */
            char elem[2048]; elem[0] = '\0';
            q_fmt_krepr(e[i], elem, sizeof elem);
            if (i) qe_putc('\n');
            qe_puts(elem);
        }
        return;
    }

    qe_ray_fallback(val);
}

/* Bounded append for the krepr assemblers; returns the new write position
 * (reserves 2 bytes for a closing paren + NUL). */
static size_t q_krepr_cat(char* buf, size_t bufsz, size_t pos, const char* s) {
    size_t el = strlen(s);
    if (pos + el + 2 > bufsz) el = bufsz > pos + 2 ? bufsz - pos - 2 : 0;
    memcpy(buf + pos, s, el);
    return pos + el;
}

/* Single-line k-repr (kdb `0N!x`, `-3!`, every list ITEM above): lists
 * inline `(a;b;c)` / `,x`; len-1 string conflation `,"c"`; else q_fmt. */
void q_fmt_krepr(ray_t* val, char* buf, size_t bufsz) {
    if (bufsz == 0) return;
    buf[0] = '\0';
    if (!val) return;
    if (val->type == -RAY_STR) {
        if (ray_str_len(val) == 1 && bufsz > 1) {
            buf[0] = ',';
            q_fmt_qstring(val, buf + 1, bufsz - 1);
        } else
            q_fmt_qstring(val, buf, bufsz);
        return;
    }
    if (val->type == -RAY_CHARV) {                 /* char atom: "a" */
        q_fmt_qtext((const char*)&val->u8, 1, buf, bufsz);
        return;
    }
    if (val->type == RAY_CHARV) {                  /* charv: "abc" / ,"a" */
        if (ray_len(val) == 1 && bufsz > 1) {
            buf[0] = ',';
            q_fmt_qtext((const char*)ray_data(val), 1, buf + 1, bufsz - 1);
        } else
            q_fmt_qtext((const char*)ray_data(val), (size_t)ray_len(val), buf, bufsz);
        return;
    }
    if (val->type == RAY_DICT) {
        /* dict inline `keys!vals` (`(,`a)!,1`); a KEYED TABLE keeps the
         * q_fmt fallback (flip repr = tracked gap) */
        ray_t* kk = ray_dict_keys(val);
        ray_t* vv = ray_dict_vals(val);
        if (kk && vv && !(kk->type == RAY_TABLE && vv->type == RAY_TABLE)) {
            /* boxed homogeneous runs collapse to typed vectors (`1 2`) */
            ray_t* ck = q_collapse_list(kk);   /* owned */
            ray_t* cv = q_collapse_list(vv);   /* owned */
            char kb[2048]; kb[0] = '\0';
            char vb[2048]; vb[0] = '\0';
            q_fmt_krepr(ck, kb, sizeof kb);
            q_fmt_krepr(cv, vb, sizeof vb);
            ray_release(ck);
            ray_release(cv);
            if (kb[0] == ',') snprintf(buf, bufsz, "(%s)!%s", kb, vb);
            else              snprintf(buf, bufsz, "%s!%s", kb, vb);
            return;
        }
    }
    if (val->type == RAY_LIST) {
        /* deriv carriers are lists structurally but display as functions */
        if (q_deriv_kind_of(val) != Q_DERIV_NONE) { q_fmt(val, buf, bufsz); return; }
        int64_t n = ray_len(val);
        ray_t** e = (ray_t**)ray_data(val);
        ray_t* tv = q_registry_table_value();   /* hidden table-literal head */
        if (tv && n >= 1 && e[0] == tv) { e++; n--; }
        if (n == 0 || (n == 1 && e[0] == q_registry_list_value())) {
            snprintf(buf, bufsz, "()");
            return;
        }
        if (n == 1 && bufsz > 1) {
            buf[0] = ',';
            q_fmt_krepr(e[0], buf + 1, bufsz - 1);
            return;
        }
        size_t pos = 0;
        if (pos + 1 < bufsz) buf[pos++] = '(';
        for (int64_t i = 0; i < n; i++) {
            if (i && pos + 1 < bufsz) buf[pos++] = ';';
            char eb[2048]; eb[0] = '\0';
            q_fmt_krepr(e[i], eb, sizeof eb);
            pos = q_krepr_cat(buf, bufsz, pos, eb);
        }
        if (pos + 1 < bufsz) buf[pos++] = ')';
        buf[pos] = '\0';
        return;
    }
    /* string vector inline: `("hello,world";,"1")` (ref/file-text.md:348) */
    if (val->type == RAY_STR && ray_is_vec(val)) {
        int64_t n = ray_len(val);
        if (n == 0) { snprintf(buf, bufsz, "()"); return; }
        size_t pos = 0;
        if (pos + 1 < bufsz) buf[pos++] = (n == 1) ? ',' : '(';
        for (int64_t i = 0; i < n; i++) {
            if (i && pos + 1 < bufsz) buf[pos++] = ';';
            ray_t* ia = ray_i64(i);
            ray_t* it = ray_at_fn(val, ia);
            ray_release(ia);
            if (!it || RAY_IS_ERR(it)) { if (it) ray_release(it); break; }
            char eb[2048];
            if (n > 1 && it->type == -RAY_STR && ray_str_len(it) == 1) {
                eb[0] = ',';
                q_fmt_qstring(it, eb + 1, sizeof eb - 1);
            } else if (it->type == RAY_CHARV || it->type == -RAY_CHARV) {
                q_fmt_krepr(it, eb, sizeof eb);
            } else
                q_fmt_qstring(it, eb, sizeof eb);
            ray_release(it);
            pos = q_krepr_cat(buf, bufsz, pos, eb);
        }
        if (n > 1 && pos + 1 < bufsz) buf[pos++] = ')';
        buf[pos] = '\0';
        return;
    }
    q_fmt(val, buf, bufsz);
}
