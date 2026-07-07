/* q_fmt — see q_fmt.h.  The start of a q-correct value formatter (the outputter
 * that will eventually back q's `.Q.s`).  It renders int vectors, symbol atoms
 * and general lists q-style and delegates everything else to rayforce's ray_fmt. */
#include "qlang/q_fmt.h"
#include "qlang/q_registry.h" /* q_registry_list_value — hidden literal head */
#include "qlang/q_deriv.h"    /* q_deriv_kind_of — 104h carrier display */
#include "lang/format.h"   /* ray_fmt */
#include "lang/eval.h"     /* ray_at_fn — dict/table element access */

/* inline dict display for parse-tree clauses (defined below) */
static void q_fmt_dict_inline(ray_t* d, char* buf, size_t bufsz);
#include "table/sym.h"     /* ray_sym_vec_cell — resolve a sym-vector cell */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

/* The verb characters — a sym atom of one of these (or the generic null `::`)
 * prints bare; every other sym keeps its leading backtick.  Same split the
 * retired q_ast_fmt made between verbs/null and names/sym-literals. */
static const char Q_VERBS[] = ":+-*%!&|<>=~,^#_$?@.";

static int q_sym_bare(const char* nm, size_t l) {
    if (l == 1 && nm[0] && strchr(Q_VERBS, nm[0])) return 1;
    /* monadic-marked verbs print bare too: +: #: |: — and :: (null/assign) */
    if (l == 2 && nm[1] == ':' && nm[0] && strchr(Q_VERBS, nm[0])) return 1;
    return 0;
}

/* Format a -RAY_SYM atom into buf: verbs/null bare, everything else backticked. */
static void q_fmt_sym(ray_t* val, char* buf, size_t bufsz) {
    ray_t* s = ray_sym_str(val->i64);
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    snprintf(buf, bufsz, "%s%.*s", q_sym_bare(nm, l) ? "" : "`", (int)l, nm);
    ray_release(s);
}

/* rayforce's formatter, captured as a string — the fallback for values q_fmt
 * doesn't yet render q-style. */
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

/* ---- q console sink (see q_fmt.h) ---------------------------------------- */
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

void q_console_show(ray_t* val) {
    char buf[8192]; buf[0] = '\0';
    q_fmt(val, buf, sizeof buf);
    q_console_append(buf, strlen(buf));
    q_console_append("\n", 1);
}

/* ---- kdb table / keyed-table display -------------------------------------
 *
 * kdb prints an unkeyed table as space-joined, left-padded columns under a
 * dashed rule:
 *     a b            and a keyed table with the key columns left of a `|`:
 *     ----               a| b
 *     1 10               -| --
 *     2 20               1| 10
 * Column width = max(header, widest cell); interior lines carry NO trailing
 * space (the qdoc compare trims only the whole string's ends). */

#define QF_MAXCOL 64

/* True iff every cell of this LIST column is a 1-item vector/list (and not a
 * string): kdb's console collapses the display of a uniformly-singleton
 * nested column (select_simple.qcmd `1| 10`), but keeps the enlist comma in
 * mixed columns (select.qcmd `c | ,50`, ungroup.md `,"A"`). */
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

/* Format one table cell q-style into out.  A -RAY_SYM cell prints WITHOUT its
 * leading backtick (kdb shows `ibm`, not `` `ibm``, inside a table). */
static void q_cell(ray_t* col, int64_t row, char* out, size_t outsz) {
    out[0] = '\0';
    /* Char-vector column (string-model shim: the whole column is ONE -RAY_STR
     * atom, one char per row — meta's `t` column is the producer).  kdb shows
     * the char BARE (`a| j`, not `a| "j"`). */
    if (col && col->type == -RAY_STR) {
        size_t l = ray_str_len(col);
        if (row >= 0 && (size_t)row < l && outsz > 1) {
            out[0] = ray_str_ptr(col)[row];
            out[1] = '\0';
        }
        return;
    }
    ray_t* ia = ray_i64(row);
    ray_t* c  = ray_at_fn(col, ia);
    ray_release(ia);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_release(c); return; }
    if (c->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(c->i64);
        if (s) { snprintf(out, outsz, "%.*s", (int)ray_str_len(s), ray_str_ptr(s));
                 ray_release(s); }
    } else {
        q_fmt(c, out, outsz);
        /* A nested cell renders with a leading enlist comma (`,10`); kdb's
         * console keeps it for mixed-length columns (`c | ,50`) but shows a
         * UNIFORMLY-singleton nested column bare (`1| 10`). */
        if (out[0] == ',' && q_col_uniform_singleton(col))
            memmove(out, out + 1, strlen(out));
    }
    ray_release(c);
}

/* Append `s` left-padded to `w` into buf at *pos (bounded by bufsz). */
static void q_pad(char* buf, size_t bufsz, size_t* pos, const char* s, int w) {
    int l = (int)strlen(s);
    for (int i = 0; i < w; i++) {
        char ch = (i < l) ? s[i] : ' ';
        if (*pos + 1 < bufsz) buf[(*pos)++] = ch;
    }
    buf[*pos] = '\0';
}

static void q_trim_trailing(char* buf, size_t* pos) {
    while (*pos > 0 && buf[*pos - 1] == ' ') (*pos)--;
    buf[*pos] = '\0';
}

/* Compute per-column widths + header strings for a table's columns. */
static int q_table_widths(ray_t* tbl, int64_t nc, int64_t nr,
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
    return 1;
}

/* Render the ncols/nrows grid of `tbl` (its own columns) into buf at *pos,
 * one space between columns; caller has already emitted any key prefix. */
static void q_table_grid(ray_t* tbl, int64_t nc, int64_t nr, const int* widths,
                         char hdr[][64], char* buf, size_t bufsz, size_t* pos,
                         const char* joiner) {
    for (int64_t c = 0; c < nc; c++) {
        if (c && *pos + 1 < bufsz) { memcpy(buf + *pos, " ", 1); (*pos)++; }
        q_pad(buf, bufsz, pos, hdr[c], widths[c]);
    }
    (void)joiner;
}

/* Render an unkeyed table. */
static void q_fmt_table(ray_t* tbl, char* buf, size_t bufsz) {
    int64_t nc = ray_table_ncols(tbl);
    int64_t nr = ray_table_nrows(tbl);
    if (nc <= 0) { snprintf(buf, bufsz, "+`!()"); return; }   /* empty schema */
    if (nc > QF_MAXCOL) nc = QF_MAXCOL;
    int  widths[QF_MAXCOL];
    char hdr[QF_MAXCOL][64];
    q_table_widths(tbl, nc, nr, widths, hdr);

    size_t pos = 0;
    /* header */
    q_table_grid(tbl, nc, nr, widths, hdr, buf, bufsz, &pos, NULL);
    q_trim_trailing(buf, &pos);
    if (pos + 1 < bufsz) buf[pos++] = '\n';
    /* separator */
    int total = (int)(nc - 1);
    for (int64_t c = 0; c < nc; c++) total += widths[c];
    for (int i = 0; i < total && pos + 1 < bufsz; i++) buf[pos++] = '-';
    if (pos + 1 < bufsz) buf[pos++] = '\n';
    /* rows */
    for (int64_t r = 0; r < nr; r++) {
        size_t line = pos;
        for (int64_t c = 0; c < nc; c++) {
            if (c && pos + 1 < bufsz) buf[pos++] = ' ';
            char cb[64]; q_cell(ray_table_get_col_idx(tbl, c), r, cb, sizeof cb);
            q_pad(buf, bufsz, &pos, cb, widths[c]);
        }
        q_trim_trailing(buf, &pos);
        (void)line;
        if (r + 1 < nr && pos + 1 < bufsz) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
}

/* Render a keyed table: a RAY_DICT whose keys AND vals are both tables
 * (`select … by …`, `([k:…] v:…)`).  Key columns sit left of a `|`. */
static void q_fmt_keyed(ray_t* kt, ray_t* vt, char* buf, size_t bufsz) {
    int64_t knc = ray_table_ncols(kt), knr = ray_table_nrows(kt);
    int64_t vnc = ray_table_ncols(vt), vnr = ray_table_nrows(vt);
    int64_t nr  = knr < vnr ? knr : vnr;
    if (knc > QF_MAXCOL) knc = QF_MAXCOL;
    if (vnc > QF_MAXCOL) vnc = QF_MAXCOL;
    int  kw[QF_MAXCOL], vw[QF_MAXCOL];
    char kh[QF_MAXCOL][64], vh[QF_MAXCOL][64];
    q_table_widths(kt, knc, nr, kw, kh);
    q_table_widths(vt, vnc, nr, vw, vh);

    size_t pos = 0;
    /* header: keyhdrs | valhdrs */
    q_table_grid(kt, knc, nr, kw, kh, buf, bufsz, &pos, NULL);
    if (pos + 2 < bufsz) { buf[pos++] = '|'; buf[pos++] = ' '; }
    q_table_grid(vt, vnc, nr, vw, vh, buf, bufsz, &pos, NULL);
    q_trim_trailing(buf, &pos);
    if (pos + 1 < bufsz) buf[pos++] = '\n';
    /* separator: keydashes| valdashes */
    int kt_tot = (int)(knc - 1); for (int64_t c = 0; c < knc; c++) kt_tot += kw[c];
    int vt_tot = (int)(vnc - 1); for (int64_t c = 0; c < vnc; c++) vt_tot += vw[c];
    for (int i = 0; i < kt_tot && pos + 1 < bufsz; i++) buf[pos++] = '-';
    if (pos + 2 < bufsz) { buf[pos++] = '|'; buf[pos++] = ' '; }
    for (int i = 0; i < vt_tot && pos + 1 < bufsz; i++) buf[pos++] = '-';
    q_trim_trailing(buf, &pos);
    if (pos + 1 < bufsz) buf[pos++] = '\n';
    /* rows */
    for (int64_t r = 0; r < nr; r++) {
        for (int64_t c = 0; c < knc; c++) {
            if (c && pos + 1 < bufsz) buf[pos++] = ' ';
            char cb[64]; q_cell(ray_table_get_col_idx(kt, c), r, cb, sizeof cb);
            q_pad(buf, bufsz, &pos, cb, kw[c]);
        }
        if (pos + 2 < bufsz) { buf[pos++] = '|'; buf[pos++] = ' '; }
        for (int64_t c = 0; c < vnc; c++) {
            if (c && pos + 1 < bufsz) buf[pos++] = ' ';
            char cb[64]; q_cell(ray_table_get_col_idx(vt, c), r, cb, sizeof cb);
            q_pad(buf, bufsz, &pos, cb, vw[c]);
        }
        q_trim_trailing(buf, &pos);
        if (r + 1 < nr && pos + 1 < bufsz) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
}

/* Render one integer element q-style with sentinel detection:
 * INT*_MIN -> 0N, INT*_MAX -> 0W, -INT*_MAX -> -0W, else the value; the type
 * suffix (h/i, or none for long) is appended. */
static void q_int_tok(int64_t v, int width, char suffix, char* out, size_t n) {
    int64_t vmin = (width == 2) ? INT16_MIN : (width == 4) ? INT32_MIN : INT64_MIN;
    int64_t vmax = (width == 2) ? INT16_MAX : (width == 4) ? INT32_MAX : INT64_MAX;
    char sfx[2]; sfx[0] = suffix; sfx[1] = '\0';
    if (!suffix) sfx[0] = '\0';
    if (v == vmin)       snprintf(out, n, "0N%s", sfx);
    else if (v == vmax)  snprintf(out, n, "0W%s", sfx);
    else if (v == -vmax) snprintf(out, n, "-0W%s", sfx);
    else                 snprintf(out, n, "%lld%s", (long long)v, sfx);
}

/* Render one float element q-style: NaN -> 0n (f64) / 0Ne (f32); a finite
 * magnitude reuses ray_fmt on a temp atom of the matching type, then appends
 * 'e' for f32 (nothing for f64).  Float infinities are out of scope. */
static void q_float_tok(double v, int f32, char* out, size_t n) {
    if (isnan(v)) { snprintf(out, n, f32 ? "0Ne" : "0n"); return; }
    /* Finite whole-number magnitude prints WITHOUT a fractional part (q shows
     * `5`, not `5.0`); f32 keeps its per-element `e`.  The disambiguating `f`
     * for f64 wholes is appended by the caller (atom) or once per vector. */
    if (isfinite(v) && v == floor(v) && v >= -9.007199254740992e15
                                     && v <=  9.007199254740992e15) {
        snprintf(out, n, "%lld%s", (long long)v, f32 ? "e" : "");
        return;
    }
    /* kdb console precision is 7 significant digits (\P 7): 1%3 -> 0.3333333,
     * 10%3 -> 3.333333.  %.7g gives exactly that (and kdb-style exponents for
     * very large / small magnitudes). */
    char mag[64];
    snprintf(mag, sizeof mag, "%.7g", v);
    snprintf(out, n, "%.48s%s", mag, f32 ? "e" : "");
}

/* Render one date element q-style: sentinels 0Nd / 0Wd / -0Wd (kdb datatypes
 * table); values inside the kdb literal domain render yyyy.mm.dd via a temp
 * base atom (base fmt_date owns the civil math — the q_float_tok pattern);
 * out-of-range but non-sentinel day counts display 0000.00.00 (datatypes.md:
 * "Out-of-range dates ... display as 0000.00.00", pinned by 0001.01.01-1). */
static void q_date_tok(int32_t v, char* out, size_t n) {
    if (v == INT32_MIN)  { snprintf(out, n, "0Nd");  return; }
    if (v == INT32_MAX)  { snprintf(out, n, "0Wd");  return; }
    if (v == -INT32_MAX) { snprintf(out, n, "-0Wd"); return; }
    if ((int64_t)v < q_days_from_civil(1, 1, 1) ||
        (int64_t)v > q_days_from_civil(9999, 12, 31)) {
        snprintf(out, n, "0000.00.00");
        return;
    }
    out[0] = '\0';
    ray_t* a = ray_date((int64_t)v);
    if (a && !RAY_IS_ERR(a)) {
        ray_fallback(a, out, n);
        ray_release(a);
    }
}

/* GUID token: canonical 8-4-4-4-12 lowercase hex (basics/datatypes.md §Guid).
 * The null guid is all-zero bytes and renders as the zero UUID
 * 00000000-0000-0000-0000-000000000000 — NOT the 0Ng token (the doc pins `0Ng`
 * display to the zero UUID).  Mirrors base fmt_guid; kept here because base
 * ray_fmt would emit the 0Ng token for the null atom. */
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

/* Render one time element q-style: sentinels 0Nt / 0Wt / -0Wt (kdb datatypes
 * table); an in-range ms count renders HH:MM:SS.mmm via a temp base atom (base
 * fmt_time owns the clock math — the q_date_tok pattern).  Time is a plain ms
 * count with no civil domain, so there is no date-style 0000.00.00 out-of-range
 * rule; base fmt_time renders large-hour / negative values directly. */
static void q_time_tok(int32_t v, char* out, size_t n) {
    if (v == INT32_MIN)  { snprintf(out, n, "0Nt");  return; }
    if (v == INT32_MAX)  { snprintf(out, n, "0Wt");  return; }
    if (v == -INT32_MAX) { snprintf(out, n, "-0Wt"); return; }
    out[0] = '\0';
    ray_t* a = ray_time((int64_t)v);
    if (a && !RAY_IS_ERR(a)) {
        ray_fallback(a, out, n);
        ray_release(a);
    }
}

/* Render one timestamp element q-style: sentinels 0Np / 0Wp / -0Wp (kdb
 * datatypes table row 12); any other ns count renders
 * yyyy.mm.ddDHH:MM:SS.nnnnnnnnn via a temp base atom (base fmt owns the
 * civil math — the q_date_tok pattern; probed pre-2000-correct and exact at
 * the +-0Wp-+1 extremes, datatypes.md:161).  Every non-sentinel i64 maps to
 * a valid civil datetime (~1707..2292), so unlike date there is no
 * 0000.00.00-style out-of-range rule. */
static void q_ts_tok(int64_t v, char* out, size_t n) {
    if (v == INT64_MIN)  { snprintf(out, n, "0Np");  return; }
    if (v == INT64_MAX)  { snprintf(out, n, "0Wp");  return; }
    if (v == -INT64_MAX) { snprintf(out, n, "-0Wp"); return; }
    out[0] = '\0';
    ray_t* a = ray_timestamp(v);
    if (a && !RAY_IS_ERR(a)) {
        ray_fallback(a, out, n);
        ray_release(a);
    }
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

/* Join per-element tokens (already rendered) with spaces into buf. */
static void q_join(char* buf, size_t bufsz, size_t* pos, const char* tok, int first) {
    size_t tl = strlen(tok);
    size_t need = tl + (first ? 0 : 1);
    if (*pos + need + 1 > bufsz) return;
    if (!first) buf[(*pos)++] = ' ';
    memcpy(buf + *pos, tok, tl);
    *pos += tl;
    buf[*pos] = '\0';
}

/* ---- value display vs parse-tree display -------------------------------
 *
 * A general RAY_LIST is shape-identical whether it is a VALUE (`(1;2 3;`abc)`,
 * displayed one-item-per-line kdb-style) or a PARSE TREE (`parse "2+3"` ->
 * `(+;2;3)`, displayed compact — a FROZEN byte-for-byte contract).  The signal
 * is intrinsic to the object, so display cannot be driven by a caller flag:
 * `parse` returns a tree that flows through the same eval->q_fmt path as data.
 *
 * A list is a PARSE TREE iff its head element is one of:
 *   - the hidden list/table constructor value (paren-literal marker), or
 *   - a fn value (RAY_UNARY/BINARY/VARY — a registry verb head like `+`), or
 *   - a name-ref/verb/marker sym: a -RAY_SYM with ATTR_QUOTED (0x20) CLEAR
 *     (a DATA sym literal has 0x20 SET — see mem/heap.h), or
 *   - (recursively) a nested clause list, e.g. a qSQL where-clause
 *     `((>;`a;1);(=;`b;2))` whose head is itself a predicate sub-tree.
 * Everything else — a data atom, a typed vector, a string, a dict/table head —
 * marks a VALUE list. */
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

/* The q display name for an empty typed vector: `long$()`, `symbol$()`, ...
 * (kdb `0#0` -> `` `long$() ``).  Byte (`0x`) and bool empties keep their own
 * arms; strings are atoms, not vectors.  Returns NULL for un-named types. */
static const char* q_empty_vec_qname(int8_t type) {
    switch (type) {
    case RAY_I16:  return "short";
    case RAY_I32:  return "int";
    case RAY_I64:  return "long";
    case RAY_F32:  return "real";
    case RAY_F64:  return "float";
    case RAY_SYM:  return "symbol";
    case RAY_DATE: return "date";
    case RAY_TIMESTAMP: return "timestamp";
    case RAY_GUID: return "guid";
    case RAY_TIME: return "time";
    default:       return NULL;
    }
}

/* A matrix column-aligns only the SPACE-SEPARATED element types (numeric /
 * sym / date).  bool (`101b`) and byte (`0x0102`) print each row-vector whole,
 * one per line — no transpose. */
static int q_matrix_alignable(int8_t type) {
    return type == RAY_I16 || type == RAY_I32 || type == RAY_I64 ||
           type == RAY_F32 || type == RAY_F64 || type == RAY_SYM ||
           type == RAY_DATE || type == RAY_TIME || type == RAY_TIMESTAMP;
}

/* Format element `c` of the row-vector `rv` BARE (no per-element type suffix,
 * no backtick) — a matrix cell token. */
static void q_matrix_cell(ray_t* rv, int64_t c, char* out, size_t outsz) {
    out[0] = '\0';
    switch (rv->type) {
    case RAY_I16: q_int_tok((int64_t)((const int16_t*)ray_data(rv))[c], 2, 0, out, outsz); break;
    case RAY_I32: q_int_tok((int64_t)((const int32_t*)ray_data(rv))[c], 4, 0, out, outsz); break;
    case RAY_I64: q_int_tok(((const int64_t*)ray_data(rv))[c],          8, 0, out, outsz); break;
    case RAY_F32: q_float_tok((double)((const float*)ray_data(rv))[c],  1, out, outsz); break;
    case RAY_F64: q_float_tok(((const double*)ray_data(rv))[c],         0, out, outsz); break;
    case RAY_DATE:q_date_tok(((const int32_t*)ray_data(rv))[c],            out, outsz); break;
    case RAY_TIME:q_time_tok(((const int32_t*)ray_data(rv))[c],            out, outsz); break;
    case RAY_TIMESTAMP: q_ts_tok(((const int64_t*)ray_data(rv))[c],           out, outsz); break;
    case RAY_SYM: {
        ray_t* s = ray_sym_vec_cell(rv, c);   /* borrowed -RAY_STR */
        if (s) snprintf(out, outsz, "%.*s", (int)ray_str_len(s), ray_str_ptr(s));
        break;
    }
    default: break;
    }
}

/* True iff `v` is a value list of >=2 same-length, same-alignable-type vectors:
 * a rectangular matrix rendered row-per-line, columns LEFT-aligned with a
 * single-space minimum (qdocs ref/mmu.md). */
static int q_is_matrix(ray_t* v) {
    int64_t n = ray_len(v);
    if (n < 2) return 0;
    ray_t** e = (ray_t**)ray_data(v);
    if (!e[0] || e[0]->type <= 0 || !ray_is_vec(e[0])) return 0;
    if (!q_matrix_alignable(e[0]->type)) return 0;
    int8_t  t = e[0]->type;
    int64_t w = ray_len(e[0]);
    for (int64_t i = 1; i < n; i++) {
        if (!e[i] || e[i]->type != t || !ray_is_vec(e[i]) || ray_len(e[i]) != w)
            return 0;
    }
    return 1;
}

static void q_fmt_matrix(ray_t* v, char* buf, size_t bufsz) {
    int64_t nr = ray_len(v);
    ray_t** e  = (ray_t**)ray_data(v);
    int64_t nc = ray_len(e[0]);
    /* per-column widths sized to the matrix (no fixed column cap — bufsz is the
     * only bound, so wide matrices are not silently truncated). */
    int  stackw[64];
    int* widths = (nc <= 64) ? stackw : malloc((size_t)(nc > 0 ? nc : 1) * sizeof(int));
    if (!widths) { buf[0] = '\0'; return; }
    for (int64_t c = 0; c < nc; c++) {
        int w = 0;
        for (int64_t r = 0; r < nr; r++) {
            char cb[64]; q_matrix_cell(e[r], c, cb, sizeof cb);
            int l = (int)strlen(cb); if (l > w) w = l;
        }
        widths[c] = w;
    }
    size_t pos = 0;
    for (int64_t r = 0; r < nr; r++) {
        if (r && pos + 1 < bufsz) buf[pos++] = '\n';
        for (int64_t c = 0; c < nc; c++) {
            if (c && pos + 1 < bufsz) buf[pos++] = ' ';
            char cb[64]; q_matrix_cell(e[r], c, cb, sizeof cb);
            q_pad(buf, bufsz, &pos, cb, widths[c]);   /* left-align */
        }
        q_trim_trailing(buf, &pos);                   /* no trailing spaces */
    }
    buf[pos < bufsz ? pos : bufsz - 1] = '\0';
    if (widths != stackw) free(widths);
}

/* A VALUE list prints one item per line (kdb): each element formatted with
 * q_fmt, joined by newlines.  `,x` for the 1-element (enlist) case is handled
 * by the caller. */
static void q_fmt_value_list(ray_t* v, char* buf, size_t bufsz) {
    int64_t n = ray_len(v);
    ray_t** e = (ray_t**)ray_data(v);
    size_t pos = 0;
    buf[0] = '\0';
    for (int64_t i = 0; i < n; i++) {
        char elem[2048]; elem[0] = '\0';
        q_fmt(e[i], elem, sizeof elem);
        size_t el = strlen(elem);
        if (i && pos + 1 < bufsz) buf[pos++] = '\n';
        if (pos + el >= bufsz) el = (bufsz > pos + 1) ? bufsz - 1 - pos : 0;
        memcpy(buf + pos, elem, el);
        pos += el;
        buf[pos] = '\0';
    }
}

void q_fmt(ray_t* val, char* buf, size_t bufsz) {
    if (bufsz == 0) return;
    buf[0] = '\0';
    if (!val) return;

    /* An empty typed vector prints `` `type$() `` (kdb `0#0` -> `` `long$() ``).
     * Byte/bool keep their own arms (`0x` / `b`); strings are atoms. */
    if (val->type > 0 && ray_is_vec(val) && ray_len(val) == 0) {
        const char* qn = q_empty_vec_qname(val->type);
        if (qn) { snprintf(buf, bufsz, "`%s$()", qn); return; }
    }

    /* A symbol atom prints q-style: `sym` (or bare for a verb/null name-ref).
     * Handling it here also renders the -RAY_SYM heads of parse-tree lists. */
    if (val->type == -RAY_SYM) {
        q_fmt_sym(val, buf, bufsz);
        return;
    }

    /* Registry function VALUES render their q spelling from provenance
     * (formatter-from-metadata, spec piece 2): glyph rows bare (`+`), monadic
     * glyph rows with the marker (`-:`), keyword rows the keyword.  Values
     * with no provenance (internal list/scan wrappers, foreign fns) fall
     * through to ray_fallback below. */
    if (val->type == RAY_UNARY || val->type == RAY_BINARY ||
        val->type == RAY_VARY) {
        q_provenance_t pv;
        if (q_registry_provenance(val, &pv) && pv.spelling && pv.spelling[0]) {
            char c0 = pv.spelling[0];
            int glyph = !((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'));
            if (pv.valence == Q_MONADIC && glyph)
                snprintf(buf, bufsz, "%s:", pv.spelling);
            else
                snprintf(buf, bufsz, "%s", pv.spelling);
            return;
        }
    }

    /* Typed numeric atoms print q-style with a type suffix (bare for long /
     * float): 1b, 42h, 42i, 42, 3.14e, 3.14 — plus the nulls/inf tokens. */
    switch (val->type) {
    case -RAY_BOOL: snprintf(buf, bufsz, "%db", val->u8 ? 1 : 0);          return;
    case -RAY_U8:   snprintf(buf, bufsz, "0x%02x", val->u8);               return;
    case -RAY_I16:  q_int_tok((int64_t)val->i16, 2, 'h', buf, bufsz);      return;
    case -RAY_I32:  q_int_tok((int64_t)val->i32, 4, 'i', buf, bufsz);      return;
    case -RAY_I64:  q_int_tok(val->i64,          8, 0,   buf, bufsz);      return;
    case -RAY_DATE: q_date_tok(val->i32, buf, bufsz);                      return;
    case -RAY_GUID: {
        const uint8_t* b16 = val->obj ? (const uint8_t*)ray_data(val->obj)
                                      : (const uint8_t*)ray_data(val);
        q_guid_tok(b16, buf, bufsz);
        return;
    }
    case -RAY_TIME: q_time_tok(val->i32, buf, bufsz);                      return;
    case -RAY_TIMESTAMP: q_ts_tok(val->i64, buf, bufsz);                   return;
    case -RAY_F32:  q_float_tok((float)val->f64, 1, buf, bufsz);           return;
    case -RAY_F64: {
        /* A whole f64 atom gets a trailing `f` to distinguish it from a long
         * (`5f`, not `5`); a fractional one (`3.14`) needs no suffix. */
        q_float_tok(val->f64, 0, buf, bufsz);
        if (isfinite(val->f64) && val->f64 == floor(val->f64)) {
            size_t l = strlen(buf);
            if (l + 1 < bufsz) { buf[l] = 'f'; buf[l + 1] = '\0'; }
        }
        return;
    }
    default: break;
    }

    /* q prints a simple vector space-separated with NO brackets: `5 6 7`,
     * where rayforce prints `[5 6 7]`.  Booleans concatenate with no spaces
     * plus one trailing `b` (1001b). */
    if (val->type == RAY_BOOL) {
        int64_t n = ray_len(val);
        const uint8_t* d = (const uint8_t*)ray_data(val);
        size_t pos = 0;
        if (n == 1 && pos + 1 < bufsz) buf[pos++] = ',';   /* enlist: ,0b */
        for (int64_t i = 0; i < n && pos + 1 < bufsz; i++)
            buf[pos++] = d[i] ? '1' : '0';
        if (pos + 1 < bufsz) buf[pos++] = 'b';
        buf[pos] = '\0';
        return;
    }
    /* Byte vector: `0x` prefix + concatenated two-digit lowercase hex, no
     * separators, no trailing type char (ref/sv.md pins 0x0102010201); the
     * length-1 vector takes the enlist comma (ref/vs.md pins ,0x01); an empty
     * vector renders bare `0x` (the literal ref/read1.md pins via `0#0x`). */
    if (val->type == RAY_U8) {
        int64_t n = ray_len(val);
        const uint8_t* d = (const uint8_t*)ray_data(val);
        static const char hx[] = "0123456789abcdef";
        size_t pos = 0;
        if (n == 1 && pos + 1 < bufsz) buf[pos++] = ',';
        if (pos + 2 < bufsz) { buf[pos++] = '0'; buf[pos++] = 'x'; }
        for (int64_t i = 0; i < n && pos + 2 < bufsz; i++) {
            buf[pos++] = hx[d[i] >> 4];
            buf[pos++] = hx[d[i] & 0xf];
        }
        buf[pos] = '\0';
        return;
    }
    /* Guid vector: full canonical UUID per element, space-joined, no trailing
     * type char (basics/datatypes.md: -2?0Ng -> "337714f8-... 0a369037-...");
     * length-1 takes the enlist comma.  Null elements are all-zero bytes ->
     * the zero UUID, self-identifying like the atom. */
    if (val->type == RAY_GUID) {
        int64_t n = ray_len(val);
        const uint8_t* d = (const uint8_t*)ray_data(val);
        size_t pos = 0;
        if (n == 1 && pos + 1 < bufsz) buf[pos++] = ',';
        for (int64_t i = 0; i < n; i++) {
            if (i && pos + 1 < bufsz) buf[pos++] = ' ';
            char e[40];
            q_guid_tok(d + i * 16, e, sizeof e);
            size_t el = strlen(e);
            if (pos + el < bufsz) { memcpy(buf + pos, e, el); pos += el; }
        }
        buf[pos < bufsz ? pos : bufsz - 1] = '\0';
        return;
    }
    /* Typed integer vector: each element is rendered BARE (no per-element type
     * char), space-joined, then the type char is appended ONCE at the end —
     * `0N 0W -0W 42h`, not `0Nh 0Wh -0Wh 42h`.  This matches kdb's own
     * formatter (K.java KBaseVector.formatVector: child context showType=false,
     * one trailing typeChar). Long's type char is empty. */
    if (val->type == RAY_I16 || val->type == RAY_I32 || val->type == RAY_I64) {
        int width  = (val->type == RAY_I16) ? 2 : (val->type == RAY_I32) ? 4 : 8;
        char vsuf  = (val->type == RAY_I16) ? 'h' : (val->type == RAY_I32) ? 'i' : 0;
        int64_t n = ray_len(val);
        size_t pos = 0;
        buf[0] = '\0';
        if (n == 1 && pos + 1 < bufsz) buf[pos++] = ',';   /* enlist: ,42h */
        for (int64_t i = 0; i < n; i++) {
            char e[64];
            int64_t v = (width == 2) ? (int64_t)((const int16_t*)ray_data(val))[i]
                      : (width == 4) ? (int64_t)((const int32_t*)ray_data(val))[i]
                      :                ((const int64_t*)ray_data(val))[i];
            q_int_tok(v, width, 0, e, sizeof e);   /* bare — no per-element suffix */
            q_join(buf, bufsz, &pos, e, i == 0);
        }
        if (vsuf && pos + 1 < bufsz) { buf[pos++] = vsuf; buf[pos] = '\0'; }
        return;
    }
    /* Date vector: space-joined full yyyy.mm.dd tokens — dates have no
     * trailing type char (unlike `0N 0W 42h`), so every element self-
     * identifies, including the sentinels (`2000.01.01 0Nd`). */
    if (val->type == RAY_DATE) {
        int64_t n = ray_len(val);
        const int32_t* d = (const int32_t*)ray_data(val);
        size_t pos = 0;
        buf[0] = '\0';
        if (n == 1 && pos + 1 < bufsz) buf[pos++] = ',';   /* enlist: ,2000.01.01 */
        for (int64_t i = 0; i < n; i++) {
            char e[64];
            q_date_tok(d[i], e, sizeof e);
            q_join(buf, bufsz, &pos, e, i == 0);
        }
        return;
    }
    /* Time vector: space-joined full HH:MM:SS.mmm tokens — like date, times
     * have no trailing type char, so every element (incl. sentinels 0Nt) self-
     * identifies; enlist comma for length-1. */
    if (val->type == RAY_TIME) {
        int64_t n = ray_len(val);
        const int32_t* d = (const int32_t*)ray_data(val);
        size_t pos = 0;
        buf[0] = '\0';
        if (n == 1 && pos + 1 < bufsz) buf[pos++] = ',';   /* enlist: ,09:30:00.000 */
        for (int64_t i = 0; i < n; i++) {
            char e[64];
            q_time_tok(d[i], e, sizeof e);
            q_join(buf, bufsz, &pos, e, i == 0);
        }
        return;
    }
    /* Timestamp vector: space-joined full yyyy.mm.ddDHH:MM:SS.nnnnnnnnn
     * tokens — like date/time, no trailing type char, sentinels (0Np)
     * self-identify; enlist comma for length-1. */
    if (val->type == RAY_TIMESTAMP) {
        int64_t n = ray_len(val);
        const int64_t* d = (const int64_t*)ray_data(val);
        size_t pos = 0;
        buf[0] = '\0';
        if (n == 1 && pos + 1 < bufsz) buf[pos++] = ',';
        for (int64_t i = 0; i < n; i++) {
            char e[64];
            q_ts_tok(d[i], e, sizeof e);
            q_join(buf, bufsz, &pos, e, i == 0);
        }
        return;
    }

    /* Float/real vectors are deferred (float infinities out of scope); the
     * per-element tokens still carry the suffix — revisit with the same
     * bare-element + one-trailing-char rule when float vectors are un-deferred. */
    if (val->type == RAY_F32 || val->type == RAY_F64) {
        int is64 = (val->type == RAY_F64);
        int64_t n = ray_len(val);
        size_t pos = 0;
        buf[0] = '\0';
        if (n == 1 && pos + 1 < bufsz) buf[pos++] = ',';   /* enlist: ,1f */
        int all_whole = (n > 0);   /* f64 gets ONE trailing `f` iff every element is finite & whole */
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(val))[i]
                            : (double)((const float*)ray_data(val))[i];
            char e[64];
            q_float_tok(v, is64 ? 0 : 1, e, sizeof e);
            q_join(buf, bufsz, &pos, e, i == 0);
            if (!(isfinite(v) && v == floor(v))) all_whole = 0;
        }
        /* f32 vectors already carry a per-element `e` (kdb records e.g.
         * `0Ne 0We -0We 3.14e`); only f64 wholes take the single trailing `f`
         * (`1 2 3f`).  A vector with any fractional or non-finite element
         * (`0.5 1 1.5`, `0n 0w -0w 3.14`) gets no suffix. */
        if (is64 && all_whole && pos + 1 < bufsz) { buf[pos++] = 'f'; buf[pos] = '\0'; }
        return;
    }

    /* Symbol vector: backtick-joined, `a`b`c.  Each cell resolves through the
     * vector's own domain.  EVERY element keeps its leading backtick — a sym
     * vector is data, so a verb/null-named element (`+`, `::`) must still round-
     * trip as a symbol literal, not a bare q token.  (The bare-verb rendering
     * in q_fmt_sym is only for a -RAY_SYM ATOM standing as a parse-tree head.)
     * The null symbol is a zero-length name and prints as a bare backtick. */
    if (val->type == RAY_SYM) {
        int64_t n = ray_len(val);
        size_t pos = 0;
        buf[0] = '\0';
        if (n == 1 && pos + 1 < bufsz) buf[pos++] = ',';   /* enlist: ,`a */
        for (int64_t i = 0; i < n; i++) {
            ray_t* s = ray_sym_vec_cell(val, i);   /* borrowed -RAY_STR */
            const char* nm = ray_str_ptr(s);
            size_t l = ray_str_len(s);
            if (pos + 1 + l + 1 > bufsz) break;
            buf[pos++] = '`';
            memcpy(buf + pos, nm, l); pos += l;
            buf[pos] = '\0';
        }
        return;
    }

    /* A 100h lambda carrier echoes its VERBATIM q source (kdb: `q)f` prints
     * `{[x] x*x}` byte-for-byte). */
    if (q_deriv_kind_of(val) == Q_DERIV_LAMBDA) {
        ray_t* s = q_lambda_src(val);
        if (s && s->type == -RAY_STR) {
            size_t l = ray_str_len(s);
            if (l >= bufsz) l = bufsz - 1;
            memcpy(buf, ray_str_ptr(s), l);
            buf[l] = '\0';
            return;
        }
    }

    /* A 104h derived-verb carrier renders as bound-verb + adverb glyph (+/).
     * The carrier's base is the HOF value (fold / the internal scan wrapper /
     * the each wrapper); the bound verb sits in the first arg slot. */
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
            snprintf(buf, bufsz, "%s%s", vb, g);
            return;
        }
        /* general projection display — base[a0;a1;...] with hole slots EMPTY
         * (kdb: `{x+y}[42;]`).  Reached by lambda projections and any other
         * carrier the adverb arm above does not claim. */
        {
            uint64_t mask  = q_deriv_hole_mask(val);
            int64_t  slots = ray_len(val) - 4;
            ray_t**  e     = (ray_t**)ray_data(val);
            char bb[512]; bb[0] = '\0';
            q_fmt(base, bb, sizeof bb);
            size_t pos = (size_t)snprintf(buf, bufsz, "%s[", bb);
            if (pos >= bufsz) pos = bufsz - 1;
            for (int64_t i = 0; i < slots && pos + 2 < bufsz; i++) {
                if (i) buf[pos++] = ';';
                if (!(mask & (1ull << i))) {
                    char ab[256]; ab[0] = '\0';
                    q_fmt(e[4 + i], ab, sizeof ab);
                    size_t al = strlen(ab);
                    if (pos + al + 2 > bufsz) al = bufsz - pos - 2;
                    memcpy(buf + pos, ab, al);
                    pos += al;
                }
            }
            if (pos + 1 < bufsz) buf[pos++] = ']';
            buf[pos] = '\0';
            return;
        }
    }

    /* An unkeyed table prints kdb-style: space-joined columns under a dashed
     * rule (`a b` / `----` / rows). */
    if (val->type == RAY_TABLE) {
        q_fmt_table(val, buf, bufsz);
        return;
    }

    /* A keyed table is a RAY_DICT whose keys AND vals are both tables (the
     * mandate: "a keyed table is just a dictionary from one table to another").
     * `select … by …` and `([k:…] v:…)` produce it.  Renders `k| v`. */
    if (val->type == RAY_DICT) {
        ray_t* kk = ray_dict_keys(val);
        ray_t* vv = ray_dict_vals(val);
        if (kk && vv && kk->type == RAY_TABLE && vv->type == RAY_TABLE) {
            q_fmt_keyed(kk, vv, buf, bufsz);
            return;
        }
    }

    /* Dict VALUE display: kdb `key| value` per row (2c-2).  Keys padded to the
     * widest key, printed BARE; values recurse.  `1 2!1 2`, `` `a`b!1 2 ``. */
    if (val->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(val);          /* borrowed */
        ray_t* v = ray_dict_vals(val);          /* borrowed */
        int64_t n = k ? ray_len(k) : 0;
        size_t pos = 0, maxk = 0;
        for (int pass = 0; pass < 2; pass++) {
            for (int64_t i = 0; i < n; i++) {
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
                if (ve && !RAY_IS_ERR(ve)) q_fmt(ve, vb, sizeof vb);
                if (ve && !RAY_IS_ERR(ve)) ray_release(ve);
                size_t need = (i ? 1 : 0) + maxk + 2 + strlen(vb);
                if (pos + need + 1 > bufsz) break;
                pos += (size_t)snprintf(buf + pos, bufsz - pos, "%s%-*s| %s",
                                        i ? "\n" : "", (int)maxk, kb, vb);
            }
        }
        buf[pos < bufsz ? pos : bufsz - 1] = '\0';
        return;
    }

    /* The functional qSQL parse tree (?;`t;c;b;a) / (!;...) prints VERTICALLY,
     * one element per line - kdb's display, pinned by the parse ledgers.  A
     * dict CLAUSE (by/select) inside it renders INLINE as `keys!vals`
     * (piece-3 display), NOT the k|v value form above - so the 5-list loop
     * uses q_fmt_dict_inline for dict elements. */
    if (val->type == RAY_LIST && ray_len(val) == 5) {
        ray_t** e = (ray_t**)ray_data(val);
        ray_t* h = e[0];
        if (h && h->type == -RAY_SYM && !(h->attrs & 0x20)) {
            ray_t* s = ray_sym_str(h->i64);
            int vert = (s && ray_str_len(s) == 1 &&
                        (ray_str_ptr(s)[0] == '?' || ray_str_ptr(s)[0] == '!'));
            if (s) ray_release(s);
            if (vert) {
                size_t pos = 0;
                buf[0] = '\0';
                for (int64_t i = 0; i < 5; i++) {
                    char elem[2048]; elem[0] = '\0';
                    if (e[i] && e[i]->type == RAY_DICT)
                        q_fmt_dict_inline(e[i], elem, sizeof elem);
                    else
                        q_fmt(e[i], elem, sizeof elem);
                    size_t el = strlen(elem);
                    if (i && pos + 1 < bufsz) buf[pos++] = '\n';
                    if (pos + el >= bufsz) el = (bufsz > pos + 1) ? bufsz - 1 - pos : 0;
                    memcpy(buf + pos, elem, el);
                    pos += el;
                    buf[pos] = '\0';
                }
                return;
            }
        }
    }

    /* A general RAY_LIST is either a PARSE TREE (compact `(a;b;c)`, a frozen
     * display contract) or a VALUE (kdb one-item-per-line / a matrix,
     * row-per-line column-aligned).  The classifier inspects the head. */
    if (val->type == RAY_LIST) {
        int64_t n = ray_len(val);
        ray_t** e = (ray_t**)ray_data(val);

        if (q_list_is_parse_tree(val, 0)) {
            /* PARSE TREE — compact `(a;b;c)`.  A literal-constructor head (the
             * parser's paren-list marker) is HIDDEN: `((list);1;2;3)` displays
             * (1;2;3); a 1-element tail is an enlist `,x`. */
            ray_t* lv = q_registry_list_value();
            ray_t* tv = q_registry_table_value();
            if (lv && n >= 1 && e[0] == lv) { e++; n--; }
            else if (tv && n >= 1 && e[0] == tv) { e++; n--; }
            if (n == 1) {
                char elem[2048]; elem[0] = '\0';
                q_fmt(e[0], elem, sizeof elem);
                snprintf(buf, bufsz, ",%s", elem);
                return;
            }
            size_t pos = 0;
            if (pos + 1 < bufsz) buf[pos++] = '(';
            for (int64_t i = 0; i < n; i++) {
                if (i && pos + 1 < bufsz) buf[pos++] = ';';
                char elem[1024]; elem[0] = '\0';
                q_fmt(e[i], elem, sizeof elem);   /* recurse */
                size_t el = strlen(elem);
                if (pos + el + 1 >= bufsz) el = (bufsz > pos + 1) ? bufsz - 1 - pos : 0;
                memcpy(buf + pos, elem, el);
                pos += el;
            }
            if (pos + 1 < bufsz) buf[pos++] = ')';
            buf[pos] = '\0';
            return;
        }

        /* VALUE list. */
        if (n == 0) { snprintf(buf, bufsz, "()"); return; }
        if (n == 1) {                             /* enlist: ,x */
            char elem[2048]; elem[0] = '\0';
            q_fmt(e[0], elem, sizeof elem);
            snprintf(buf, bufsz, ",%s", elem);
            return;
        }
        if (q_is_matrix(val)) { q_fmt_matrix(val, buf, bufsz); return; }
        q_fmt_value_list(val, buf, bufsz);
        return;
    }

    ray_fallback(val, buf, bufsz);
}


/* Inline dict `keys!vals` (parenthesised key when enlisted: `(,`a)!,`a`) -
 * ONLY for by/select clause dicts inside a functional qSQL parse tree, where
 * kdb shows the dict inline rather than as k|v rows. */
static void q_fmt_dict_inline(ray_t* d, char* buf, size_t bufsz) {
    ray_t* keys = ray_dict_keys(d);
    ray_t* vals = ray_dict_vals(d);
    char kb[2048]; kb[0] = '\0';
    char vb[2048]; vb[0] = '\0';
    q_fmt(keys, kb, sizeof kb);
    q_fmt(vals, vb, sizeof vb);
    if (kb[0] == ',') snprintf(buf, bufsz, "(%s)!%s", kb, vb);
    else              snprintf(buf, bufsz, "%s!%s", kb, vb);
}
