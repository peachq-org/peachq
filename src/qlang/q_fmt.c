/* q_fmt — the q-style value formatter (q_fmt.h); non-q shapes -> ray_fmt. */
#include "qlang/q_fmt.h"
#include "qlang/q_fmt_internal.h" /* q_fmt_cell — shared with the pipe renderer */
#include "qlang/q_console.h"  /* q_console_pipe_* — the `--nonlegacy` display config */
#include "qlang/q_builtins.h" /* q_builtins_count_long — THE count owner */
#include "qlang/q_registry.h" /* q_registry_list_value — hidden literal head */
#include "qlang/base/q_calendar.h" /* q_calendar_days_from_civil — date display domain */
#include "qlang/q_registry_internal.h" /* q_type_qname — the guarded type-name home */
#include "qlang/parse/q_parse_internal.h" /* ADVERB_NAMES — the one adverb-spelling table */
#include "qlang/eval/q_eval.h" /* carrier read-out accessors — RAY_QFN display;
                                * q_eval_apply_concrete — the display boundary force */
#include "qlang/eval/q_view.h" /* q_view_text — a view displays as its text */
#include "qlang/io/q_splay.h"  /* a mapped splay displays as its table */
#include "qlang/io/q_provider.h" /* a provider carrier displays as provider truth */
#include "lang/format.h"   /* ray_fmt */
#include "lang/eval.h"     /* ray_at_fn — dict/table element access */
#include "lang/internal.h" /* is_collection — THE boxed-list-or-typed-vector predicate */
#include "ops/hash.h"    /* ray_hash_bytes — pipe digest distinct keys */
#include "core/types.h"  /* ray_elem_size — pipe digest */

#include "table/sym.h"     /* ray_sym_vec_cell — resolve a sym-vector cell */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <math.h>

/* `\P n` float precision (default 7, 0=max 17); writer q_sys.c h_P, reader q_fmt_float. */
#define Q_PRINT_PREC_DEFAULT 7
static int g_print_prec = Q_PRINT_PREC_DEFAULT;

void q_fmt_set_prec(int p) { g_print_prec = p; }
int  q_fmt_prec(void)      { return g_print_prec; }

/* Name-ref syms of these chars print bare (kdb `(/;+)`, basics/parsetrees.md). */
static const char Q_VERBS[] = ":+-*%!&|<>=~,^#_$?@./\\'";

static int sym_bare(const char* nm, size_t l) {
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
    int bare = sym_bare(nm, l) && !(val->attrs & 0x20 /* Q_ATTR_QUOTED */);
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
static size_t char_esc(unsigned char ch, char out[8]);   /* fwd — fmt_dict_elem shares it */
static int elem_tok(ray_t* a, char* out, size_t n);      /* fwd — THE cell law, defined below */
static int col_uniform_type(ray_t* col);                 /* fwd — its precondition */

/* Tables: padded columns under a dashed rule, keyed tables put key columns left of `|`; NO trailing spaces. */

#define QF_MAXCOL 64

/* THE inline-cell law (ref/trim.md, ref/dotq.md:1655): a dict row, a table
 * cell and a list-row cell are ELEMENTS, and an element renders single-line —
 * every NON-ATOM via the k-repr renderer, never the top-level layout. */
static void fmt_elem_inline(ray_t* e, char* out, size_t cap) {
    if (e && !RAY_IS_ERR(e) && !ray_is_atom(e)) q_fmt_krepr(e, out, cap);
    else q_fmt(e, out, cap);
}

/* Uniformly-singleton nested column collapses (`1| 10`); mixed keeps `,50`. */
static int col_uniform_singleton(ray_t* col) {
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


/* THE cell renderer — table, keyed table, dict and aligned row.  `blank_null` =
 * a COLUMN, where a null shows as a gap (ref/lj.md, ref/log.md:50-52); a general
 * list keeps its token.  A cell sheds the suffix only where the CONTAINER names
 * the type — `col_uniform_type`, the same gate the dict rows use. */
void q_fmt_cell(ray_t* col, int64_t row, int blank_null, char* out, size_t outsz) {
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
    if (blank_null && ray_is_atom(c) && c->type != -RAY_STR &&
        c->type != -RAY_SYM && RAY_ATOM_IS_NULL(c)) {
        ray_release(c);
        return;
    }
    if (!col_uniform_type(col) || !elem_tok(c, out, outsz)) {
        fmt_elem_inline(c, out, outsz);
        if (out[0] == ',' && col_uniform_singleton(col))
            memmove(out, out + 1, strlen(out));
    }
    ray_release(c);
}

static void table_widths(ray_t* tbl, int64_t nc, int64_t nr,
                           int* widths, char hdr[][64]) {
    for (int64_t c = 0; c < nc; c++) {
        ray_t* s = ray_sym_str(ray_table_col_name(tbl, c));
        snprintf(hdr[c], 64, "%.*s", s ? (int)ray_str_len(s) : 0,
                 s ? ray_str_ptr(s) : "");
        if (s) ray_release(s);
        int w = (int)strlen(hdr[c]);
        ray_t* col = ray_table_get_col_idx(tbl, c);
        for (int64_t r = 0; r < nr; r++) {
            char cb[64]; q_fmt_cell(col, r, 1, cb, sizeof cb);
            int l = (int)strlen(cb); if (l > w) w = l;
        }
        widths[c] = w;
    }
}

static void table_grid(int64_t nc, const int* widths, char hdr[][64]) {
    for (int64_t c = 0; c < nc; c++) {
        if (c) qe_putc(' ');
        qe_pad(hdr[c], widths[c]);
    }
}

static void grid_cells(ray_t* t, int64_t nc, const int* w, int64_t r) {
    for (int64_t c = 0; c < nc; c++) {
        if (c) qe_putc(' ');
        char cb[64]; q_fmt_cell(ray_table_get_col_idx(t, c), r, 1, cb, sizeof cb);
        qe_pad(cb, w[c]);
    }
}

static void grid_rule(int64_t nc, const int* w) {
    int total = (int)(nc - 1);
    for (int64_t c = 0; c < nc; c++) total += w[c];
    for (int i = 0; i < total; i++) qe_putc('-');
}

static int64_t size_rows(int64_t nr) {
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
    table_widths(tbl, nc, size_rows(nr), widths, hdr);

    table_grid(nc, widths, hdr);
    qe_trim();
    qe_putc('\n');
    grid_rule(nc, widths);
    qe_putc('\n');
    for (int64_t r = 0; r < nr; r++) {
        grid_cells(tbl, nc, widths, r);
        qe_trim();
        if (r + 1 < nr) qe_putc('\n');
        if (qe_done()) break;                    /* height cap hit — early exit */
    }
}

static void fmt_keyed(ray_t* kt, ray_t* vt) {
    int64_t knc = ray_table_ncols(kt), knr = ray_table_nrows(kt);
    int64_t vnc = ray_table_ncols(vt), vnr = ray_table_nrows(vt);
    int64_t nr  = knr < vnr ? knr : vnr;
    if (knc > QF_MAXCOL) knc = QF_MAXCOL;
    if (vnc > QF_MAXCOL) vnc = QF_MAXCOL;
    int  kw[QF_MAXCOL], vw[QF_MAXCOL];
    char kh[QF_MAXCOL][64], vh[QF_MAXCOL][64];
    table_widths(kt, knc, size_rows(nr), kw, kh);
    table_widths(vt, vnc, size_rows(nr), vw, vh);

    table_grid(knc, kw, kh);
    qe_putc('|'); qe_putc(' ');
    table_grid(vnc, vw, vh);
    qe_trim();
    qe_putc('\n');
    grid_rule(knc, kw);
    qe_putc('|'); qe_putc(' ');
    grid_rule(vnc, vw);
    qe_trim();
    qe_putc('\n');
    for (int64_t r = 0; r < nr; r++) {
        grid_cells(kt, knc, kw, r);
        qe_putc('|'); qe_putc(' ');
        grid_cells(vt, vnc, vw, r);
        qe_trim();
        if (r + 1 < nr) qe_putc('\n');
        if (qe_done()) break;                    /* height cap hit — early exit */
    }
}

/* Integer-backed sentinels (kdb datatypes table): MIN->0N, MAX->0W, -MAX->-0W, + suffix (0=bare).
 * "There is no display for short infinity" (basics/datatypes.md:254-259: 0Wh -> 32767h): the
 * h width prints its ±MAX as digits.  Its NULL still displays 0Nh. */
static int sentinel_tok(int64_t v, int width, char suffix, char* out, size_t n) {
    int64_t vmax = (width == 2) ? INT16_MAX : (width == 4) ? INT32_MAX : INT64_MAX;
    const char* base = (v == -vmax - 1) ? "0N"
                     : (width == 2)     ? NULL
                     : (v == vmax)      ? "0W"
                     : (v == -vmax)     ? "-0W" : NULL;
    if (!base) return 0;
    char sfx[2] = { suffix, '\0' };
    snprintf(out, n, "%s%s", base, sfx);
    return 1;
}

static void int_tok(int64_t v, int width, char suffix, char* out, size_t n) {
    if (sentinel_tok(v, width, suffix, out, n)) return;
    char sfx[2] = { suffix, '\0' };
    snprintf(out, n, "%lld%s", (long long)v, sfx);
}

/* Float token: NaN->0n/0Ne, ±inf->0w/-0w (0we/-0we for reals — the display
 * form, datatypes/real.qcmd); wholes within `\P` print integral (`5`), past
 * the horizon exponent-form (timespan.qcmd:162 pins 3e+11); else %.*g.  The
 * 10^prec horizon must be EXACT powers of 10 (pow() drift would move the
 * `\P 7` boundary off 1e7). */
void q_fmt_float(double v, int f32, char* out, size_t n) {
    if (isnan(v)) { snprintf(out, n, f32 ? "0Ne" : "0n"); return; }
    if (isinf(v)) {
        snprintf(out, n, "%s%s", v < 0 ? "-0w" : "0w", f32 ? "e" : "");
        return;
    }
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

/* An f32 token is payload + `e` (`0Ne`, `0we`, `1.5e`) — drop the letter where
 * the CONTAINER prints it instead. */
static void real_shed_e(char* out) {
    size_t l = strlen(out);
    if (l) out[l - 1] = '\0';
}

/* Digit-only token — the shape test behind the f64 `f` suffix (`256f`). */
static int fmt_tok_is_bare_int(const char* tok) {
    if (*tok == '-') tok++;
    if (!*tok) return 0;
    for (; *tok; tok++)
        if (*tok < '0' || *tok > '9') return 0;
    return 1;
}

/* Payload via a temp base atom (base fmt owns the math); consumes `a`. */
static void tok_via_atom(ray_t* a, char* out, size_t n) {
    out[0] = '\0';
    if (a && !RAY_IS_ERR(a)) {
        ray_fallback(a, out, n);
        ray_release(a);
    }
}

/* Date payload; out-of-civil-range displays 0000.00.00 (datatypes.md). */
static void date_payload(int64_t v, char* out, size_t n) {
    if (v < q_calendar_days_from_civil(1, 1, 1) || v > q_calendar_days_from_civil(9999, 12, 31)) {
        snprintf(out, n, "0000.00.00");
        return;
    }
    tok_via_atom(ray_date(v), out, n);
}

/* Month payload: months since 2000.01; outside year [1,9999] -> `0000.00`. */
static void month_payload(int64_t p, char* out, size_t n) {
    int64_t y = 2000 + (p >= 0 ? p / 12 : -((-p + 11) / 12));
    int64_t m = 1 + (p % 12 + 12) % 12;
    if (y < 1 || y > 9999) { snprintf(out, n, "0000.00"); return; }
    snprintf(out, n, "%04lld.%02lld", (long long)y, (long long)m);
}

/* GUID: 8-4-4-4-12 lowercase hex; null = zero UUID, never 0Ng (datatypes.md). */
static void guid_tok(const uint8_t* b16, char* out, size_t n) {
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

/* Datetime (f64 — Q_TTOK width 0): NaN->0Nz, ±inf->0Wz/-0Wz (live 0Wz,
 * 2026-07-28); out-of-range -> 0000.00.00T00:00:00.000; ms (tok.md:227). */
static void datetime_tok(double v, char* out, size_t n) {
    if (v != v) { snprintf(out, n, "0Nz"); return; }
    if (isinf(v)) { snprintf(out, n, v < 0 ? "-0Wz" : "0Wz"); return; }
    if (v < (double)q_calendar_days_from_civil(1, 1, 1) ||
        v >= (double)(q_calendar_days_from_civil(9999, 12, 31) + 1)) {
        snprintf(out, n, "0000.00.00T00:00:00.000");
        return;
    }
    tok_via_atom(ray_datetime(v), out, n);
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
    { RAY_DATE,      'd', 0,   4, NULL,          date_payload },
    { RAY_MONTH,      0,  'm', 4, NULL,          month_payload },
    { RAY_TIME,      't', 0,   4, ray_time,      NULL },
    { RAY_MINUTE,    'u', 0,   4, ray_minute,    NULL },
    { RAY_SECOND,    'v', 0,   4, ray_second,    NULL },
    { RAY_TIMESPAN,  'n', 0,   8, ray_timespan,  NULL },
    { RAY_TIMESTAMP, 'p', 0,   8, ray_timestamp, NULL },
    { RAY_DATETIME,   0,  0,   0, NULL,          NULL },
};

static const q_ttok_t* ttok_find(int8_t t) {
    for (size_t i = 0; i < sizeof Q_TTOK / sizeof *Q_TTOK; i++)
        if (Q_TTOK[i].type == t) return &Q_TTOK[i];
    return NULL;
}

static void ttok_elem(const q_ttok_t* r, ray_t* v, int64_t i,
                        char* out, size_t n) {
    if (r->width == 0) {
        datetime_tok(i < 0 ? v->f64 : ((const double*)ray_data(v))[i], out, n);
        return;
    }
    int64_t x = (i < 0)
        ? (r->width == 4 ? (int64_t)v->i32 : v->i64)
        : (r->width == 4 ? (int64_t)((const int32_t*)ray_data(v))[i]
                         : ((const int64_t*)ray_data(v))[i]);
    if (sentinel_tok(x, r->width, r->sfx, out, n)) return;
    if (r->payload) { r->payload(x, out, n); return; }
    tok_via_atom(r->ctor(x), out, n);
}

static void tok_append(char* out, size_t n, char c) {
    size_t l = strlen(out);
    if (l + 1 < n) { out[l] = c; out[l + 1] = '\0'; }
}

/* One token per atom.  `suffixed` = the VALUE form, which names its own type
 * (`1h`, `1b`, `5f`, `2017.05m`); clear it for the ELEMENT form below.  A
 * -RAY_SYM never reaches the value form — q_fmt_body backticks it first. */
static int atom_tok(ray_t* a, int suffixed, char* out, size_t n) {
    out[0] = '\0';
    if (!a || RAY_IS_ERR(a) || a->type >= 0) return 0;
    const q_ttok_t* tr = ttok_find((int8_t)-a->type);
    if (tr) {
        ttok_elem(tr, a, -1, out, n);
        if (suffixed && tr->vsfx) tok_append(out, n, tr->vsfx);
        return 1;
    }
    switch (a->type) {
    case -RAY_BOOL:      snprintf(out, n, "%d%s", a->u8 ? 1 : 0, suffixed ? "b" : "");
                                                                     return 1;
    case -RAY_BYTE_ONLY: snprintf(out, n, "0x%02x", a->u8);          return 1;
    case -RAY_I16:       int_tok((int64_t)a->i16, 2, suffixed ? 'h' : 0, out, n);
                                                                     return 1;
    case -RAY_I32:       int_tok((int64_t)a->i32, 4, suffixed ? 'i' : 0, out, n);
                                                                     return 1;
    case -RAY_I64:       int_tok(a->i64,          8, 0, out, n);     return 1;
    case -RAY_F32:
        /* the value form narrows through float first; the element form never has */
        q_fmt_float(suffixed ? (double)(float)a->f64 : a->f64, 1, out, n);
        /* the container carries the `e`; a SENTINEL keeps its own (`0Ne`/`0we`) */
        if (!suffixed && isfinite(a->f64)) real_shed_e(out);
        return 1;
    case -RAY_F64:
        q_fmt_float(a->f64, 0, out, n);
        /* digit-only tokens take `f` (`5f`); `3e+11` self-identifies */
        if (suffixed && fmt_tok_is_bare_int(out)) tok_append(out, n, 'f');
        return 1;
    case -RAY_GUID: {
        const uint8_t* b16 = a->obj ? (const uint8_t*)ray_data(a->obj)
                                    : (const uint8_t*)ray_data(a);
        guid_tok(b16, out, n);
        return 1;
    }
    case -RAY_SYM: {
        ray_t* s = ray_sym_str(a->i64);   /* borrowed — never released (sym.h:105) */
        if (s) snprintf(out, n, "%.*s", (int)ray_str_len(s), ray_str_ptr(s));
        return 1;
    }
    default: return 0;
    }
}

/* THE display cell law (#367, ARCHITECTURE.md "One predicate at every cell
 * site", ref/join.md:177): a cell is
 * one ELEMENT, so it sheds the suffix naming a type its container already
 * carries (`1i`->`1`, `2017.05m`->`2017.05`, `1.5e`->`1.5`).  Intrinsic syntax
 * survives — the `0x` radix, guid/temporal punctuation, the sentinels.
 * 0 = no element form; the caller renders the whole value. */
static int elem_tok(ray_t* a, char* out, size_t n) {
    return atom_tok(a, 0, out, n);
}

/* THE cell-law predicate, asked at every cell site: does this container NAME a
 * type its elements may shed?  A typed vector does, and so does a boxed list of
 * one atom type (ref/apply.md's bare sym column).  A general list names none —
 * `meta ([]a:(1;3f))` reports a blank `t` — so its elements keep their own
 * (`xyz| 321f`, `([]a:(1;3f))` -> `3f`). */
static int col_uniform_type(ray_t* col) {
    if (!col || col->type < 0) return 0;
    if (col->type != RAY_LIST) return 1;
    int64_t n = ray_len(col);
    if (n == 0) return 0;
    ray_t** e = (ray_t**)ray_data(col);
    for (int64_t i = 0; i < n; i++)
        if (!e[i] || !ray_is_atom(e[i]) || e[i]->type != e[0]->type) return 0;
    return 1;
}

/* A dict key and a dict value row are both ELEMENTS of `col`.
 * The char atom is the exception: bare like a sym key but via char_esc, keeping
 * control/non-ASCII bytes safe (#320) — an escape a grid cell must not carry,
 * char_esc also quoting `"`. */
static void fmt_dict_elem(ray_t* col, ray_t* e, char* out, size_t cap) {
    out[0] = '\0';
    if (!e || RAY_IS_ERR(e)) return;
    if (col_uniform_type(col)) {
        if (e->type == -RAY_CHARV) {
            char esc[8];
            size_t el = char_esc(e->u8, esc);
            if (el < cap) { memcpy(out, esc, el); out[el] = '\0'; }
            return;
        }
        if (elem_tok(e, out, cap)) return;
    }
    fmt_elem_inline(e, out, cap);
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
static int list_is_parse_tree(ray_t* v, int depth) {
    if (!v || v->type != RAY_LIST || depth > 64) return 0;
    int64_t n = ray_len(v);
    if (n < 1) return 0;
    ray_t* h = ((ray_t**)ray_data(v))[0];
    if (!h) return 0;
    if (h == q_registry_list_value()) return 1;
    if (h->type == RAY_UNARY || h->type == RAY_BINARY || h->type == RAY_VARY)
        return 1;
    if (h->type == -RAY_SYM && !(h->attrs & Q_ATTR_QUOTED)) return 1;
    if (h->type == RAY_LIST) return list_is_parse_tree(h, depth + 1);
    return 0;
}

/* Empty-vector name (`long$()`) via the guarded q_type_qname home; byte alone
 * stays bare-0x (byte_impl.qcmd:120 pin), so it names no `byte$()`. Composing on
 * the single home means the #209 value-band guard there transitively protects
 * empty-vec display — no parallel table to keep in sync. */
static const char* empty_vec_qname(int8_t type) {
    if (type == RAY_BYTE_ONLY) return NULL;
    return q_type_qname(type);
}

/* Escape one byte kdb-style — THE display-inverse of the scanner decode. */
static size_t char_esc(unsigned char ch, char out[8]) {
    switch (ch) {
    case '"':  out[0] = '\\'; out[1] = '"';  return 2;
    case '\\': out[0] = '\\'; out[1] = '\\'; return 2;
    case '\n': out[0] = '\\'; out[1] = 'n';  return 2;
    case '\t': out[0] = '\\'; out[1] = 't';  return 2;
    case '\r': out[0] = '\\'; out[1] = 'r';  return 2;
    default:
        /* non-printable + non-ASCII bytes octal-escaped `\ooo` (basics/datatypes.md) */
        if (ch < 32 || ch > 126) return (size_t)snprintf(out, 8, "\\%03o", ch);
        out[0] = (char)ch;
        return 1;
    }
}

/* Quoted-text renderer over raw bytes — shared by the -RAY_STR atom form and
 * the charv vector/atom forms. */
static void fmt_qtext(const char* p, size_t n, char* buf, size_t bufsz) {
    size_t w = 0;
    if (w + 1 < bufsz) buf[w++] = '"';
    for (size_t i = 0; i < n && w + 6 < bufsz; i++) {
        char e[8];
        size_t el = char_esc((unsigned char)p[i], e);
        memcpy(buf + w, e, el);
        w += el;
    }
    if (w + 1 < bufsz) buf[w++] = '"';
    buf[w < bufsz ? w : bufsz - 1] = '\0';
}

static void fmt_qstring(ray_t* val, char* buf, size_t bufsz) {
    fmt_qtext(ray_str_ptr(val), ray_str_len(val), buf, bufsz);
}

static void qe_qstring(ray_t* val) {
    const char* p = ray_str_ptr(val);
    size_t n = ray_str_len(val);
    qe_putc('"');
    for (size_t i = 0; i < n; i++) {
        if (qe_line_done()) break;               /* clip: line already decided */
        char e[8];
        qe_putn(e, char_esc((unsigned char)p[i], e));
    }
    qe_putc('"');
}

/* Alignable = space-separated element types; bool/byte rows print whole. */
static int matrix_alignable(int8_t type) {
    return type == RAY_I16 || type == RAY_I32 || type == RAY_I64 ||
           type == RAY_F32 || type == RAY_F64 || type == RAY_SYM ||
           type == RAY_CHARV ||
           type == RAY_DATE || type == RAY_TIME || type == RAY_TIMESTAMP ||
           type == RAY_MINUTE || type == RAY_SECOND || type == RAY_TIMESPAN ||
           type == RAY_DATETIME;
}

/* A row of syms prints BARE (`2 4#`Arthur..`); a sym inside a MIXED row keeps
 * its backtick — there it is the only thing telling `x from a char. */
static int row_all_sym(ray_t* v) {
    if (!v) return 0;
    if (v->type == RAY_SYM) return 1;
    if (v->type != RAY_LIST) return 0;
    int64_t n = ray_len(v);
    if (n == 0) return 0;
    ray_t** e = (ray_t**)ray_data(v);
    for (int64_t i = 0; i < n; i++)
        if (!e[i] || e[i]->type != -RAY_SYM) return 0;
    return 1;
}

/* One cell of an aligned row.  A TYPED row's cell IS a column cell — same owner.
 * Only the BOXED row differs: `bare` (row_all_sym, once per row by the caller)
 * decides a sym's backtick, and a boxed atom keeps its suffix. */
static void matrix_cell(ray_t* rv, int64_t c, int bare, int blank_null,
                        char* out, size_t outsz) {
    if (rv->type != RAY_LIST) { q_fmt_cell(rv, c, blank_null, out, outsz); return; }
    out[0] = '\0';
    ray_t* a = ((ray_t**)ray_data(rv))[c];
    if (!a) return;
    if (a->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(a->i64);   /* borrowed */
        const char* tick = bare ? "" : "`";
        if (s) snprintf(out, outsz, "%s%.*s", tick,
                        (int)ray_str_len(s), ray_str_ptr(s));
    } else if (a->type == -RAY_STR) {
        if (ray_str_len(a) == 1 && outsz > 1) {
            out[0] = ',';
            fmt_qstring(a, out + 1, outsz - 1);
        } else
            fmt_qstring(a, out, outsz);
    } else if (a->type == -RAY_CHARV) {
        fmt_qtext((const char*)&a->u8, 1, out, outsz);
    } else if (a->type == RAY_CHARV) {
        if (ray_len(a) == 1 && outsz > 1) {
            out[0] = ',';
            fmt_qtext((const char*)ray_data(a), 1, out + 1, outsz - 1);
        } else
            fmt_qtext((const char*)ray_data(a), (size_t)ray_len(a), out, outsz);
    } else
        fmt_elem_inline(a, out, outsz);
}

/* A boxed row aligns when its cells agree on ONE display class — all collections,
 * or all atoms.  `((1 2;3);4 5)` agrees on neither and prints whole
 * (math/atomic_nested), where `2 3 4#til 5` is all collections and grids
 * (ref/take.md:86).  The char atom joins either class: it is an atom that prints
 * string-shaped, so `(2;10;"a")` (joins/cross) and `("a";"dog ")` (ref/trim.md) align. */
static int matrix_row_ok(ray_t* r) {
    if (!r || RAY_IS_ERR(r)) return 0;
    if (r->type > 0 && ray_is_vec(r) && matrix_alignable(r->type)) return 1;
    if (r->type != RAY_LIST) return 0;
    ray_t** it = (ray_t**)ray_data(r);
    int64_t n = ray_len(r);
    int row_nested = -1;
    for (int64_t i = 0; i < n; i++) {
        ray_t* c = it[i];
        if (!c || RAY_IS_ERR(c)) return 0;
        if (c->type == -RAY_CHARV || c->type == -RAY_STR) continue;
        int nested = is_collection(c);
        if (!nested && (!ray_is_atom(c) || RAY_IS_NULL(c))) return 0;
        if (row_nested < 0) row_nested = nested;
        else if (row_nested != nested) return 0;
    }
    return 1;
}

/* One row into a buffer: cells space-joined, each padded to w[c] (w NULL =
 * unpadded); trailing pad trimmed.  THE row renderer for the dict value column,
 * which IS a column — hence blank_null. */
static void matrix_row_str(ray_t* row, int64_t nc, const int* w,
                           char* out, size_t outsz) {
    size_t pos = 0;
    int bare = row_all_sym(row);
    out[0] = '\0';
    for (int64_t c = 0; c < nc; c++) {
        char cb[512]; matrix_cell(row, c, bare, 1, cb, sizeof cb);
        if (c && pos + 1 < outsz) out[pos++] = ' ';
        int l = (int)strlen(cb);
        int pad = w ? w[c] : l;
        for (int k = 0; k < pad && pos + 1 < outsz; k++)
            out[pos++] = (k < l) ? cb[k] : ' ';
    }
    while (pos > 0 && out[pos - 1] == ' ') pos--;
    out[pos] = '\0';
}

/* e[0..n) is >=2 same-length alignable rows: a LEFT-aligned matrix (ref/mmu.md). */
static int is_matrix(ray_t** e, int64_t n) {
    if (n < 1) return 0;
    if (!matrix_row_ok(e[0])) return 0;
    int64_t w = ray_len(e[0]);
    if (w == 0) return 0;
    /* One row is an enlist (`,1 2 3`) UNLESS it is also one column: a 1x1 drops
     * its commas for the same reason `(1 2;3 4)` does — it is a matrix
     * (ref/file-binary.md `show pi` -> 3.141593, while .Q.s1 keeps ",,"). */
    if (n == 1 && w != 1) return 0;
    /* the 1x1 comma-drop covers SCALAR cells only: `,,(in;`s)` keeps its commas */
    if (n == 1 && e[0]->type == RAY_LIST &&
        is_collection(((ray_t**)ray_data(e[0]))[0]))
        return 0;
    int all_charv = e[0]->type == RAY_CHARV;
    for (int64_t i = 1; i < n; i++) {
        if (!matrix_row_ok(e[i]) || ray_len(e[i]) != w) return 0;
        if (e[i]->type != RAY_CHARV) all_charv = 0;
    }
    /* all-charv is the STRING-LIST idiom, not a char matrix: `("Gfg";"is")`
     * stays quoted per row (.Q.opt, .j.k), while a charv MIXED with another
     * row is cells (ref/sublist.md:36 `b| x y z`). */
    return !all_charv;
}

/* Column widths over nr rows; NULL on OOM.  Caller frees unless == stackw.
 * `blank_null` must match the render pass or the grid misaligns. */
static int* matrix_widths(ray_t** e, int64_t nr, int64_t nc, int blank_null,
                          int* stackw, int64_t stackn) {
    int* w = (nc <= stackn) ? stackw : malloc((size_t)(nc > 0 ? nc : 1) * sizeof(int));
    if (!w) return NULL;
    for (int64_t c = 0; c < nc; c++) w[c] = 0;
    for (int64_t r = 0; r < nr; r++) {          /* rows outer: one row_all_sym each */
        int bare = row_all_sym(e[r]);
        for (int64_t c = 0; c < nc; c++) {
            char cb[512]; matrix_cell(e[r], c, bare, blank_null, cb, sizeof cb);
            int l = (int)strlen(cb); if (l > w[c]) w[c] = l;
        }
    }
    return w;
}

/* A general list is not a column, so its null cells keep their token (no doc in
 * the corpus blanks one) — blank_null 0 throughout. */
static void fmt_matrix(ray_t** e, int64_t nr) {
    int64_t nc = ray_len(e[0]);
    /* no fixed column cap; clip-armed sizing scans showable rows only */
    int  stackw[64];
    int* widths = matrix_widths(e, size_rows(nr), nc, 0, stackw, 64);
    if (!widths) return;
    for (int64_t r = 0; r < nr; r++) {
        if (qe_done()) break;                    /* height cap hit — early exit */
        if (r) qe_putc('\n');
        int bare = row_all_sym(e[r]);
        for (int64_t c = 0; c < nc; c++) {
            if (c) qe_putc(' ');
            char cb[512]; matrix_cell(e[r], c, bare, 0, cb, sizeof cb);
            qe_pad(cb, widths[c]);                    /* left-align */
        }
        qe_trim();                                    /* no trailing spaces */
    }
    if (widths != stackw) free(widths);
}

static void q_fmt_body(ray_t* val);   /* fwd */

/* Attributed vectors get `` `s#`` (q_attr_letter — shared with `attr`); table columns stay bare via q_fmt_cell. */
static void fmt_render(ray_t* val) {
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
    fmt_render(val);
    qe_pop();
}

/* Public entry, CONSOLE: the `\c` clip when armed; unarmed — or a parse tree — equals q_fmt. */
/* ---- pipe-table display: a FORMATTING MODE (state lives in q_console.c).
 * Moved from the console split: the renderer is value->text, its sole caller
 * is q_fmt_console below — both entries are file-static. ---- */
/* ---- `--nonlegacy` pipe-table mode --------------------------------------- */

#define QP_MAXCOL   64
#define QP_CELL     64
#define QP_LINE     8192   /* >= QP_MAXCOL * (QP_CELL + 3) */
#define QP_DIGEST   2      /* digest line cap (spec decision 6) */
#define QP_DGLINE   2048
#define QP_DIST_CAP 10000  /* bounded distinct (spec decision 7 = DBHelper MAX_SIZE) */
#define QP_HT       16384  /* open-addressed slots; load <= 0.61 at the cap */
#define QP_FIXED    3      /* name row + type row + divider (no rows/cols banner) */
#define QP_MIN_ROWS 10     /* digest fires only past this many TABLE rows */

static bool fmt_pipe_is_table(ray_t* val) {
    if (!val) return false;
    if (val->type == RAY_TABLE) return true;
    if (val->type == RAY_DICT) {                     /* keyed table = table!table */
        ray_t* kk = ray_dict_keys(val);              /* borrowed */
        ray_t* vv = ray_dict_vals(val);              /* borrowed */
        return kk && vv && kk->type == RAY_TABLE && vv->type == RAY_TABLE;
    }
    return false;
}

/* ---- output: one line at a time, under the `\c` cols width rule ---------- */

typedef struct { char* buf; size_t cap; size_t pos; int64_t nlines; } qp_out;

static void qp_raw(qp_out* o, const char* s, size_t n) {
    size_t avail = o->cap - 1 - o->pos;
    if (n > avail) n = avail;
    memcpy(o->buf + o->pos, s, n);
    o->pos += n;
    o->buf[o->pos] = '\0';
}

/* Width rule mirrors the legacy emitter: over cols-1 keeps cols-3 chars + `..`.
 * cols == 0 (unarmed `\c`) means unlimited. */
static void qp_line(qp_out* o, const char* s, int32_t cols) {
    if (o->nlines++) qp_raw(o, "\n", 1);
    size_t l = strlen(s);
    if (cols > 0 && l > (size_t)(cols - 1)) {
        qp_raw(o, s, (size_t)(cols - 3));
        qp_raw(o, "..", 2);
    } else {
        qp_raw(o, s, l);
    }
}

/* ---- columns ------------------------------------------------------------- */

typedef struct { ray_t* col; char name[QP_CELL]; const char* type; } qp_col;

/* Type-row name via q_type_qname (the cast home); char-column shim = `char`
 * (as `key x` names it); a nested/mixed list has no vector-type name — blank. */
static const char* qp_typename(ray_t* col) {
    const char* n = col ? q_type_qname(col->type) : NULL;
    if (n) return n;
    if (col && (col->type == -RAY_STR || col->type == RAY_CHARV)) return "char";
    return "";
}

static int64_t qp_take(ray_t* t, qp_col* cs, int64_t at, int64_t max) {
    int64_t nc = ray_table_ncols(t);
    for (int64_t c = 0; c < nc && at < max; c++, at++) {
        ray_t* s = ray_sym_str(ray_table_col_name(t, c));
        snprintf(cs[at].name, QP_CELL, "%.*s", s ? (int)ray_str_len(s) : 0,
                 s ? ray_str_ptr(s) : "");
        if (s) ray_release(s);
        cs[at].col  = ray_table_get_col_idx(t, c);   /* borrowed */
        cs[at].type = qp_typename(cs[at].col);
    }
    return at;
}

/* Flatten val to columns (keyed table = key cols then value cols, one grid).
 * Returns the count TAKEN (<= max). */
static int64_t qp_gather(ray_t* val, qp_col* cs, int64_t max, int64_t* nrows) {
    if (val->type == RAY_TABLE) {
        *nrows = ray_table_nrows(val);
        return qp_take(val, cs, 0, max);
    }
    ray_t* kk = ray_dict_keys(val);                  /* borrowed */
    ray_t* vv = ray_dict_vals(val);                  /* borrowed */
    int64_t kn = ray_table_nrows(kk), vn = ray_table_nrows(vv);
    *nrows = kn < vn ? kn : vn;
    return qp_take(vv, cs, qp_take(kk, cs, 0, max), max);
}

static void qp_widths(qp_col* cs, int64_t nc, int64_t shown, int* w) {
    for (int64_t c = 0; c < nc; c++) {
        int m = (int)strlen(cs[c].name);
        int t = (int)strlen(cs[c].type);
        if (t > m) m = t;
        for (int64_t r = 0; r < shown; r++) {
            char cb[QP_CELL];
            q_fmt_cell(cs[c].col, r, 1, cb, sizeof cb);
            int l = (int)strlen(cb);
            if (l > m) m = l;
        }
        w[c] = m;
    }
}

/* ---- per-column facts ---------------------------------------------------- */

static bool qp_is_temporal(int8_t t) {
    return t == RAY_DATE || t == RAY_MONTH || t == RAY_MINUTE || t == RAY_SECOND ||
           t == RAY_TIME || t == RAY_TIMESPAN || t == RAY_TIMESTAMP || t == RAY_DATETIME;
}

static bool qp_is_numeric(int8_t t) {
    return t == RAY_BYTE_ONLY || t == RAY_I16 || t == RAY_I32 || t == RAY_I64 ||
           t == RAY_F32 || t == RAY_F64;
}

/* Display boundary seam (materialization phase 1): settle a lazy DAG handle
 * before read.  q_eval_apply_concrete CONSUMES its input (cf. src/ops/sort.c). */
static ray_t* qp_solid(ray_t* v) {
    return q_eval_apply_concrete(v);
}

static int64_t qp_agg_i64(ray_t* v, int64_t dflt) {
    v = qp_solid(v);
    if (!v || RAY_IS_ERR(v)) { if (v) ray_release(v); return dflt; }
    int64_t r = q_type_is_int_atom(v) && !RAY_ATOM_IS_NULL(v) ? q_type_iatom_val(v) : dflt;
    ray_release(v);
    return r;
}

/* Nulls compose on the `null`+`sum` PRIMITIVES (no per-type ladder).  q_sum_wrap
 * not ray_sum_fn: q's sum counts a boolean vector, the engine's rejects it. */
static int64_t qp_nulls(ray_t* col) {
    ray_t* nb = qp_solid(q_null_wrap(col));
    if (!nb || RAY_IS_ERR(nb)) { if (nb) ray_release(nb); return 0; }
    int64_t n = qp_agg_i64(q_sum_wrap(nb), 0);
    ray_release(nb);
    return n;
}

static bool qp_ht_add(uint64_t* ht, int64_t* n, uint64_t h) {
    if (!h) h = 1;                                   /* 0 marks an empty slot */
    size_t m = QP_HT - 1, i = h & m;
    while (ht[i]) { if (ht[i] == h) return false; i = (i + 1) & m; }
    ht[i] = h; (*n)++;
    return true;
}

/* Distinct, bounded at QP_DIST_CAP (stop + report `+`).  Keys = the raw
 * fixed-width payload (exact for the sym ids / temporal ints this fact uses),
 * else the rendered cell.  A 64-bit hash collision undercounts — a display
 * digest, not a ledger. */
static int64_t qp_distinct(ray_t* col, int64_t nr, bool* capped) {
    *capped = false;
    uint64_t* ht = calloc(QP_HT, sizeof *ht);
    if (!ht) return 0;
    int esz = (ray_is_vec(col) && col->type != RAY_LIST) ? ray_elem_size(col->type) : 0;
    const char* d = esz > 0 ? (const char*)ray_data(col) : NULL;
    int64_t n = 0;
    for (int64_t i = 0; i < nr; i++) {
        if (n >= QP_DIST_CAP) { *capped = true; break; }
        uint64_t h;
        if (d) {
            h = ray_hash_bytes(d + i * esz, (size_t)esz);
        } else {
            char cb[QP_CELL];
            q_fmt_cell(col, i, 1, cb, sizeof cb);
            h = ray_hash_bytes(cb, strlen(cb));
        }
        qp_ht_add(ht, &n, h);
    }
    free(ht);
    return n;
}

/* A float/real atom carries an `f`/`e` the TYPE ROW already names — drop it, as
 * the shared cell renderer does.  Never touches a null token (`0n`/`0Ne`). */
static void qp_atom_tok(ray_t* a, int8_t ct, char* out, size_t outsz) {
    out[0] = '\0';
    if (!a || RAY_IS_ERR(a)) return;
    q_fmt(a, out, outsz);
    if (ct != RAY_F64 && ct != RAY_F32) return;
    if (strchr(out, 'n') || strchr(out, 'N')) return;
    size_t l = strlen(out);
    if (l && (out[l - 1] == 'f' || out[l - 1] == 'e')) out[l - 1] = '\0';
}

static void qp_minmax(ray_t* col, char* out, size_t outsz) {
    ray_t* lo = qp_solid(ray_min_fn(col));
    ray_t* hi = qp_solid(ray_max_fn(col));
    char a[QP_CELL], b[QP_CELL];
    qp_atom_tok(lo, col->type, a, sizeof a);
    qp_atom_tok(hi, col->type, b, sizeof b);
    if (lo) ray_release(lo);
    if (hi) ray_release(hi);
    snprintf(out, outsz, "%s-%s", a, b);
}

/* One fact per column, in column order (spec "Digest content").  `all distinct`
 * is the phrasing whenever distinct == count; the cap can never say it. */
static void qp_fact(qp_col* c, int64_t nr, char* out, size_t outsz) {
    int8_t t = c->col ? c->col->type : 0;
    int64_t nulls = qp_nulls(c->col);
    char f[QP_CELL * 4];

    if (t == RAY_BOOL) {
        int64_t nt = qp_agg_i64(q_sum_wrap(c->col), 0);
        snprintf(f, sizeof f, "%lld%% true",
                 (long long)(nr ? (nt * 100 + nr / 2) / nr : 0));
    } else if (qp_is_numeric(t)) {
        qp_minmax(c->col, f, sizeof f);
    } else {
        bool capped = false;
        int64_t d = qp_distinct(c->col, nr, &capped);
        if (capped)            snprintf(f, sizeof f, "%d+ distinct", QP_DIST_CAP);
        else if (d == nr)      snprintf(f, sizeof f, "all distinct");
        else if (qp_is_temporal(t)) qp_minmax(c->col, f, sizeof f);
        else                   snprintf(f, sizeof f, "%lld distinct", (long long)d);
    }

    if (nulls > 0)
        snprintf(out, outsz, "%s=%s (%lld nulls).", c->name, f, (long long)nulls);
    else
        snprintf(out, outsz, "%s=%s.", c->name, f);
}

/* ---- digest -------------------------------------------------------------- */

static void qp_mark_overflow(char* line, size_t lim) {
    size_t l = strlen(line);
    if (l + 4 > lim) { if (lim < 4) return; line[lim - 4] = '\0'; }
    strcat(line, " ...");
}

/* Facts left-to-right, wrapped at the `\c` cols budget, hard-capped at QP_DIGEST
 * lines with `...` marking what did not fit.  nc = columns with a fact (display
 * cap).  The row/column counts live once, in the footer.  Returns lines used. */
static int qp_digest(qp_col* cs, int64_t nc, int64_t nr, int32_t cols,
                     char lines[QP_DIGEST][QP_DGLINE]) {
    size_t lim = (size_t)(cols - 1);
    if (lim > QP_DGLINE - 1) lim = QP_DGLINE - 1;
    for (int i = 0; i < QP_DIGEST; i++) lines[i][0] = '\0';

    char tok[QP_CELL * 6];
    int  li = 0;
    for (int64_t c = 0; c < nc; c++) {
        qp_fact(&cs[c], nr, tok, sizeof tok);
        size_t have = strlen(lines[li]), need = strlen(tok);
        if (have + (have ? 1 : 0) + need <= lim) {
            if (have) strcat(lines[li], " ");
            strcat(lines[li], tok);
            continue;
        }
        if (li + 1 < QP_DIGEST && need <= lim) {
            strcpy(lines[++li], tok);
            continue;
        }
        qp_mark_overflow(lines[li], lim);
        break;
    }
    return lines[1][0] ? 2 : 1;
}

/* ---- render -------------------------------------------------------------- */

static void qp_bar(char* line, size_t lsz, const int* w, int64_t nc) {
    size_t p = 0;
    if (p + 1 < lsz) line[p++] = '|';
    for (int64_t c = 0; c < nc; c++) {
        for (int k = 0; k < w[c] + 2 && p + 1 < lsz; k++) line[p++] = '-';
        if (p + 1 < lsz) line[p++] = '|';
    }
    line[p] = '\0';
}

static void qp_cells(char* line, size_t lsz, char (*cells)[QP_CELL],
                     const int* w, int64_t nc) {
    size_t p = 0;
    for (int64_t c = 0; c < nc && p + 1 < lsz; c++)
        p += (size_t)snprintf(line + p, lsz - p, "| %-*s ", w[c], cells[c]);
    if (p + 1 < lsz) { line[p++] = '|'; line[p] = '\0'; }
}

static void fmt_pipe_render(ray_t* val, char* buf, size_t bufsz) {
    if (!buf || bufsz == 0) return;
    buf[0] = '\0';
    qp_out o = { buf, bufsz, 0, 0 };

    int32_t crows = 0, ccols = 0;
    bool armed = q_console_clip(&crows, &ccols);
    if (armed && (crows < 10 || ccols < 10 || ccols > 2000)) armed = false;
    int32_t cols = armed ? ccols : 0;

    qp_col  cs[QP_MAXCOL];
    int64_t nr = 0;
    int64_t nc = qp_gather(val, cs, QP_MAXCOL, &nr);
    if (nc <= 0) { qp_line(&o, "+`!()", cols); return; }

    /* Budget (spec decision 4): the WHOLE render fits `\c` rows, so the digest
     * costs data rows rather than growing the output (crows-2 = legacy height). */
    int64_t budget  = armed ? crows - 2 : 0;
    int64_t shown   = nr;
    bool    clipped = armed && nr > (budget - QP_FIXED > 1 ? budget - QP_FIXED : 1);

    char dl[QP_DIGEST][QP_DGLINE];
    int  dn = (clipped && nr > QP_MIN_ROWS) ? qp_digest(cs, nc, nr, cols, dl) : 0;
    if (clipped) {
        shown = budget - QP_FIXED - 1 - (dn ? dn + 1 : 0);   /* footer; blank + digest */
        if (shown < 1) shown = 1;
    }

    int w[QP_MAXCOL];
    qp_widths(cs, nc, shown, w);

    char line[QP_LINE], cells[QP_MAXCOL][QP_CELL];
    for (int64_t c = 0; c < nc; c++)
        snprintf(cells[c], QP_CELL, "%s", cs[c].name);
    qp_cells(line, sizeof line, cells, w, nc);
    qp_line(&o, line, cols);

    for (int64_t c = 0; c < nc; c++)
        snprintf(cells[c], QP_CELL, "%s", cs[c].type);
    qp_cells(line, sizeof line, cells, w, nc);
    qp_line(&o, line, cols);

    qp_bar(line, sizeof line, w, nc);
    qp_line(&o, line, cols);

    for (int64_t r = 0; r < shown; r++) {
        for (int64_t c = 0; c < nc; c++)
            q_fmt_cell(cs[c].col, r, 1, cells[c], QP_CELL);
        qp_cells(line, sizeof line, cells, w, nc);
        qp_line(&o, line, cols);
    }

    if (!clipped) return;
    snprintf(line, sizeof line, "... (showing first %lld of %lld rows)",
             (long long)shown, (long long)nr);
    qp_line(&o, line, cols);
    for (int i = 0; i < dn; i++) {
        if (!i) qp_line(&o, "", cols);
        qp_line(&o, dl[i], cols);
    }
}


void q_fmt_console(ray_t* val, char* buf, size_t bufsz) {
    if (bufsz == 0 || !buf) return;
    /* `--nonlegacy` (OFF by default): tables render as the pipe table.  Gated
     * HERE — the console seam — and never in q_fmt_body, which the UNCLIPPED
     * q_fmt (`string`, `-3!`, CSV, every cell) shares and must keep legacy. */
    if (q_console_pipe_on() && fmt_pipe_is_table(val)) { fmt_pipe_render(val, buf, bufsz); return; }
    int32_t rows = 0, cols = 0;
    int armed = q_console_clip(&rows, &cols) && !g_clip_active;
    if (armed && val && val->type == RAY_LIST && list_is_parse_tree(val, 0))
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
    fmt_render(val);
    qe_pop();
    g_clip_active = 0;
}

/* RAY_QFN carrier display over the apply module's read-out accessors (the
 * slot layout stays opaque): lambda = verbatim source, iterator/derived =
 * head + adverb glyph, composition = "u g", projection = verbatim head +
 * the bound-argument echo, holes empty — {x+y}[1;], +[;2] (kdb shape,
 * owner ruling 2026-07-23).  Returns 1 iff v was rendered as a carrier. */
static int carrier_fmt(ray_t* v, char* buf, size_t bufsz) {
    int kind = q_eval_apply_carrier_kind(v);
    if (!kind || bufsz == 0) return 0;
    if (kind == Q_EVAL_CAR_VIEW) {         /* `. `d shows the bare text: b+a */
        ray_t* t = q_view_text(v);
        snprintf(buf, bufsz, "%.*s", t ? (int)ray_len(t) : 0,
                 t ? (const char*)ray_data(t) : "");
        if (t) ray_release(t);
        return 1;
    }
    if (kind == Q_EVAL_CAR_LAMBDA) {
        ray_t* src = q_eval_apply_lambda_src(v);
        if (src)
            snprintf(buf, bufsz, "%.*s", (int)ray_str_len(src),
                     ray_str_ptr(src));
        else
            snprintf(buf, bufsz, "{..}");
        return 1;
    }
    if (kind == Q_EVAL_CAR_ITER) {
        int adv = q_eval_apply_iter_id(v);
        snprintf(buf, bufsz, "%s",
                 (adv >= 0 && adv < 6) ? ADVERB_NAMES[adv] : "");
        return 1;
    }
    const char* nm = q_eval_apply_car_head_name(v);
    ray_t* head = q_eval_apply_car_head(v);
    if (kind == Q_EVAL_CAR_DERIV) {
        int adv = q_eval_apply_deriv_adv(v);
        char fb[128] = "";
        if (nm) snprintf(fb, sizeof fb, "%s", nm);
        else if (head) q_fmt(head, fb, sizeof fb);
        snprintf(buf, bufsz, "%s%s", fb,
                 (adv >= 0 && adv < 6) ? ADVERB_NAMES[adv] : "");
        return 1;
    }
    if (kind == Q_EVAL_CAR_COMP) {
        ray_t* g = q_eval_apply_comp_inner(v);
        char ub[128] = "", gb[128] = "";
        if (head) q_fmt(head, ub, sizeof ub);
        if (g) q_fmt(g, gb, sizeof gb);
        snprintf(buf, bufsz, "%s %s", ub, gb);
        return 1;
    }
    char fb[256] = "";
    if (nm) snprintf(fb, sizeof fb, "%s", nm);
    else if (head) q_fmt(head, fb, sizeof fb);
    size_t off = 0;
    int64_t slots = q_eval_apply_proj_nslots(v);
    #define PUT(...) do { \
        if (off < bufsz) { \
            int w = snprintf(buf + off, bufsz - off, __VA_ARGS__); \
            off += (w > 0) ? (size_t)w : 0; \
        } \
    } while (0)
    PUT("%s[", fb);
    for (int64_t i = 0; i < slots; i++) {
        if (i) PUT(";");
        ray_t* a = q_eval_apply_proj_arg(v, i);
        if (a) {
            char ab[128] = "";
            q_fmt(a, ab, sizeof ab);
            PUT("%s", ab);
        }
    }
    PUT("]");
    #undef PUT
    return 1;
}

/* a registry spelling that is a GLYPH (`+`, `%`, `0:`) rather than a keyword —
 * glyphs take the `:` monadic marker on display, keywords never do */
static int fn_glyph_spelling(const char* s) {
    char c = s[0];
    return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

static void q_fmt_body(ray_t* val) {
    if (!val) return;

    /* generic null prints `::` (top-level silence is the CALLER's rule) */
    if (RAY_IS_NULL(val)) {
        qe_puts("::");
        return;
    }

    /* RAY_QFN carrier: lambda verbatim source / F+adverb / projection — EXCEPT
     * that a k PRIMITIVE prints as its glyph whatever machinery implements it
     * (`%:` is a q.q projection here; kdb still shows `%:` — `.q`reciprocal`,
     * basics/funsql.md).  Keyword-spelled q.q definitions keep their source. */
    if (val->type == RAY_QFN) {
        q_provenance_t gpv;
        if (q_registry_provenance(val, &gpv) && gpv.spelling &&
            fn_glyph_spelling(gpv.spelling)) {
            qe_puts(gpv.spelling);
            if (gpv.valence == Q_MONADIC) qe_putc(':');
            return;
        }
        char cb[512];
        cb[0] = '\0';
        if (carrier_fmt(val, cb, sizeof cb))
            qe_puts(cb);
        return;
    }

    /* char atom / char vector: kdb quoted forms — "a", "abc", ,"a", "" —
     * BEFORE the empty-typed-vector arm ("" is the empty charv, never `char$()) */
    if (val->type == -RAY_CHARV) {
        char e[8];
        qe_putc('"');
        qe_putn(e, char_esc(val->u8, e));
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
            qe_putn(e, char_esc((unsigned char)p[i], e));
        }
        qe_putc('"');
        return;
    }

    /* empty typed vector: `` `type$() `` (byte keeps its bare-0x arm) */
    if (val->type > 0 && ray_is_vec(val) && ray_len(val) == 0) {
        const char* qn = empty_vec_qname(val->type);
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
                fmt_qstring(it, elem + 1, sizeof elem - 1);
            } else
                fmt_qstring(it, elem, sizeof elem);
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
            qe_puts(pv.spelling);
            if (pv.valence == Q_MONADIC && fn_glyph_spelling(pv.spelling))
                qe_putc(':');
            return;
        }
    }

    {
        char tok[64];
        if (atom_tok(val, 1, tok, sizeof tok)) { qe_puts(tok); return; }
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
            guid_tok(d + i * 16, e, sizeof e);
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
            int_tok(v, width, 0, e, sizeof e);   /* bare — no per-element suffix */
            qe_join(e, i == 0);
        }
        if (vsuf) qe_putc(vsuf);
        return;
    }
    /* temporal vector: bare tokens space-joined; only month has a trailing
     * type char (`m`) — the other temporals' full tokens self-identify,
     * sentinels included (`2000.01.01 0Nd`) */
    {
        const q_ttok_t* tr = ttok_find(val->type);
        if (tr) {
            int64_t n = ray_len(val);
            if (n == 1) qe_putc(',');                      /* enlist: ,2000.01.01 */
            for (int64_t i = 0; i < n && !qe_line_done(); i++) {
                char e[64];
                ttok_elem(tr, val, i, e, sizeof e);
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
            q_fmt_float(v, !is64, e, sizeof e);
            /* the f32 token minus its letter: `0N` — NOT f64's `0n` — 0w 1.5 */
            if (!is64) real_shed_e(e);
            qe_join(e, i == 0);
            if (!fmt_tok_is_bare_int(e)) all_whole = 0;
        }
        /* the real vector's `e` prints ONCE (gpus.md:81 transcript `…64 81e`);
         * all-digit-token f64 vectors take ONE trailing `f` (`1 2 3f`), and a
         * clipped early exit's stale all_whole is swallowed past the dots */
        if (!is64) qe_putc('e');
        else if (all_whole) qe_putc('f');
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

    if (val->type == RAY_TABLE) {
        q_fmt_table(val);
        return;
    }

    if (val->type == RAY_DICT && q_splay_is(val)) {  /* mapped splay: show the table */
        /* a ROW-BUDGET prefix gather (`\c`): one row past the clip keeps the
         * continuation marker honest while untouched rows stay on disk */
        int64_t k = -1, cr = qe_clip_rows();
        ray_t* cnt = cr > 0 ? q_splay_count(val) : NULL;
        if (cnt && !RAY_IS_ERR(cnt) && cr + 1 < cnt->i64) k = cr + 1;
        if (cnt && !RAY_IS_ERR(cnt)) ray_release(cnt);
        else if (cnt) ray_error_free(cnt);
        ray_t* mt = q_splay_prefix(val, k);
        if (mt && !RAY_IS_ERR(mt)) {
            q_fmt_table(mt);
            ray_release(mt);
            return;
        }
        if (mt) ray_error_free(mt);                  /* unreadable: the raw dict shows */
    }

    if (val->type == RAY_DICT && q_provider_carrier_is(val)) {
        ray_t* mt = q_provider_carrier_table(val);     /* provider truth: show the table */
        if (mt && !RAY_IS_ERR(mt) && mt->type == RAY_TABLE) {
            q_fmt_table(mt);
            ray_release(mt);
            return;
        }
        if (mt && RAY_IS_ERR(mt)) ray_error_free(mt);   /* unreadable: the raw dict shows */
        else if (mt) ray_release(mt);
    }

    if (val->type == RAY_DICT) {            /* keyed table = table!table */
        ray_t* kk = ray_dict_keys(val);
        ray_t* vv = ray_dict_vals(val);
        if (kk && vv && kk->type == RAY_TABLE && vv->type == RAY_TABLE) {
            fmt_keyed(kk, vv);
            return;
        }
    }

    if (val->type == RAY_DICT) {            /* dict: `key| value` rows */
        ray_t* k = ray_dict_keys(val);          /* borrowed */
        ray_t* v = ray_dict_vals(val);          /* borrowed */
        /* entries, not slots: a TABLE domain (the iterators re-key onto one)
         * counts its rows, which ray_len does not report */
        int64_t n = k ? q_builtins_count_long(k) : 0;
        if (n < 0) n = 0;
        size_t maxk = 0;
        int64_t n_size = size_rows(n);   /* clip-armed: size showable rows only */
        /* value rows column-align like a matrix (compare/lesser.qcmd `d&5`) */
        int  dstackw[64];
        int* dw = NULL;
        int64_t dnc = 0;
        if (v && v->type == RAY_LIST && ray_len(v) == n &&
            is_matrix((ray_t**)ray_data(v), n)) {
            dnc = ray_len(((ray_t**)ray_data(v))[0]);
            dw  = matrix_widths((ray_t**)ray_data(v), n_size, dnc, 1, dstackw, 64);
        } else if (v && v->type == RAY_LIST && ray_len(v) == n && n > 0) {
            /* UNIFORM zero-length rows are zero padded columns — blank, where
             * the ragged dict keeps `()` (ref/dotq.md:1322 vs :1655) */
            ray_t** ve = (ray_t**)ray_data(v);
            int64_t i = 0;
            while (i < n && ve[i] && ve[i]->type == RAY_LIST && ray_len(ve[i]) == 0) i++;
            if (i == n) { dnc = 0; dw = dstackw; }
        }
        for (int pass = 0; pass < 2; pass++) {
            int64_t n_pass = (pass == 0) ? n_size : n;
            for (int64_t i = 0; i < n_pass; i++) {
                ray_t* ia = ray_i64(i);
                ray_t* ke = ray_at_fn(k, ia);
                ray_release(ia);
                char kb[256]; kb[0] = '\0';
                fmt_dict_elem(k, ke, kb, sizeof kb);
                if (ke && !RAY_IS_ERR(ke)) ray_release(ke);
                size_t kl = strlen(kb);
                if (pass == 0) {
                    if (kl > maxk) maxk = kl;
                    continue;
                }
                char vb[1024]; vb[0] = '\0';
                if (dw) {
                    matrix_row_str(((ray_t**)ray_data(v))[i], dnc, dw, vb, sizeof vb);
                    size_t aneed = (i ? 1 : 0) + maxk + 2 + strlen(vb);
                    if (!qe_fits(aneed)) break;
                    qe_printf("%s%-*s| %s", i ? "\n" : "", (int)maxk, kb, vb);
                    continue;
                }
                ray_t* ja = ray_i64(i);
                ray_t* ve = v ? ray_at_fn(v, ja) : NULL;
                ray_release(ja);
                /* len-1 strings render "c" NOT ,"c" (list/first pins `c| "h"`) */
                if (ve && !RAY_IS_ERR(ve)) fmt_dict_elem(v, ve, vb, sizeof vb);
                if (ve && !RAY_IS_ERR(ve)) ray_release(ve);
                size_t need = (i ? 1 : 0) + maxk + 2 + strlen(vb);
                if (!qe_fits(need)) break;
                qe_printf("%s%-*s| %s", i ? "\n" : "", (int)maxk, kb, vb);
            }
        }
        if (dw && dw != dstackw) free(dw);
        return;
    }

    /* General list, kdb-true: one item per line, each a single-line k-repr —
     * parse trees and values alike (basics/parsetrees.md; implicit-iteration
     * .md pins nested items inline `(110b;0b)`).  One item = `,x`;
     * rectangular = aligned matrix (ref/mmu.md). */
    if (val->type == RAY_LIST) {
        int64_t n = ray_len(val);
        ray_t** e = (ray_t**)ray_data(val);
        /* a bare constructor (parse "()") is the empty-list application */
        if (n == 1 && e[0] == q_registry_list_value()) { qe_puts("()"); return; }
        if (n == 0) { qe_puts("()"); return; }
        if (is_matrix(e, n)) { fmt_matrix(e, n); return; }
        if (n == 1) {                             /* enlist: ,x */
            char elem[2048]; elem[0] = '\0';
            q_fmt_krepr(e[0], elem, sizeof elem);
            qe_putc(',');
            qe_puts(elem);
            return;
        }
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
static size_t krepr_cat(char* buf, size_t bufsz, size_t pos, const char* s) {
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
            fmt_qstring(val, buf + 1, bufsz - 1);
        } else
            fmt_qstring(val, buf, bufsz);
        return;
    }
    if (val->type == -RAY_CHARV) {                 /* char atom: "a" */
        fmt_qtext((const char*)&val->u8, 1, buf, bufsz);
        return;
    }
    if (val->type == RAY_CHARV) {                  /* charv: "abc" / ,"a" */
        if (ray_len(val) == 1 && bufsz > 1) {
            buf[0] = ',';
            fmt_qtext((const char*)ray_data(val), 1, buf + 1, bufsz - 1);
        } else
            fmt_qtext((const char*)ray_data(val), (size_t)ray_len(val), buf, bufsz);
        return;
    }
    if (val->type == RAY_TABLE) {                  /* `+`a`b!(..)` — ref/dotz.md:723, ref/dotq.md:308 */
        ray_t* d = q_flip_wrap(val);               /* owned: the column dict */
        if (d && RAY_IS_ERR(d)) ray_error_free(d); /* OOM only: fall through, never an empty repr */
        else if (d) {
            if (bufsz > 1) { buf[0] = '+'; q_fmt_krepr(d, buf + 1, bufsz - 1); }
            ray_release(d);
            return;
        }
    }
    if (val->type == RAY_DICT) {
        /* dict inline `keys!vals` (`(,`a)!,1`); a KEYED TABLE falls out as `(+K)!+V` off the table arm
         * (kb/pivoting-tables.md:86).  A splay/provider carrier IS its stored dict here — no gather, no `+`. */
        ray_t* kk = ray_dict_keys(val);
        ray_t* vv = ray_dict_vals(val);
        if (kk && vv) {
            /* boxed homogeneous runs collapse to typed vectors (`1 2`) */
            ray_t* ck = q_list_collapse(kk);   /* owned */
            ray_t* cv = q_list_collapse(vv);   /* owned */
            char kb[2048]; kb[0] = '\0';
            char vb[2048]; vb[0] = '\0';
            q_fmt_krepr(ck, kb, sizeof kb);
            q_fmt_krepr(cv, vb, sizeof vb);
            ray_release(ck);
            ray_release(cv);
            /* left operand of `!` needs parens when compound so the string re-parses: an enlist `,x`, a flip
             * `+x` (`(+(,`k)!,1 2 3)!+..`, kb/pivoting-tables.md:86), or a `$`-form typed empty `` `long$() ``
             * (else RTL binds `$` wrong) */
            if (kb[0] == ',' || kb[0] == '+' || strchr(kb, '$')) snprintf(buf, bufsz, "(%s)!%s", kb, vb);
            else                                                 snprintf(buf, bufsz, "%s!%s", kb, vb);
            return;
        }
    }
    if (val->type == RAY_LIST) {
        int64_t n = ray_len(val);
        ray_t** e = (ray_t**)ray_data(val);
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
            pos = krepr_cat(buf, bufsz, pos, eb);
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
                fmt_qstring(it, eb + 1, sizeof eb - 1);
            } else if (it->type == RAY_CHARV || it->type == -RAY_CHARV) {
                q_fmt_krepr(it, eb, sizeof eb);
            } else
                fmt_qstring(it, eb, sizeof eb);
            ray_release(it);
            pos = krepr_cat(buf, bufsz, pos, eb);
        }
        if (n > 1 && pos + 1 < bufsz) buf[pos++] = ')';
        buf[pos] = '\0';
        return;
    }
    q_fmt(val, buf, bufsz);
}
