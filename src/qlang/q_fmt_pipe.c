/* q_fmt_pipe — the `--nonlegacy` pipe-table console render.  See q_fmt_pipe.h
 * for the contract and the spec reference; this file owns the whole mode. */
#include "qlang/q_fmt_pipe.h"
#include "qlang/q_fmt.h"               /* q_fmt — digest min/max atom render */
#include "qlang/q_fmt_internal.h"      /* q_cell — the shared cell renderer */
#include "qlang/q_sys.h"               /* q_con_display — live `\c rows cols` */
#include "qlang/q_registry_internal.h" /* q_type_qname, q_null_wrap, q_sum_wrap, q_iatom_val */
#include "lang/eval.h"                 /* ray_min_fn, ray_max_fn */
#include "ops/ops.h"                   /* ray_is_lazy, ray_lazy_materialize */
#include "ops/hash.h"                  /* ray_hash_bytes — distinct keys */
#include "core/types.h"                /* ray_elem_size */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QP_MAXCOL   64
#define QP_CELL     64
#define QP_LINE     8192   /* >= QP_MAXCOL * (QP_CELL + 3) */
#define QP_DIGEST   2      /* digest line cap (spec decision 6) */
#define QP_DGLINE   2048
#define QP_DIST_CAP 10000  /* bounded distinct (spec decision 7 = DBHelper MAX_SIZE) */
#define QP_HT       16384  /* open-addressed slots; load <= 0.61 at the cap */
#define QP_FIXED    3      /* name row + type row + divider (no rows/cols banner) */
#define QP_MIN_ROWS 10     /* digest fires only past this many TABLE rows */

static bool g_pipe_on;
void q_pipe_enable(void)  { g_pipe_on = true; }
void q_pipe_disable(void) { g_pipe_on = false; }
bool q_pipe_on(void)      { return g_pipe_on; }

bool q_pipe_is_table(ray_t* val) {
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
            q_cell(cs[c].col, r, cb, sizeof cb);
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

/* An aggregate over a vector returns a LAZY DAG handle — settle it before read.
 * ray_lazy_materialize CONSUMES its input (cf. src/ops/sort.c): never released
 * on top of this. */
static ray_t* qp_solid(ray_t* v) {
    return ray_is_lazy(v) ? ray_lazy_materialize(v) : v;
}

static int64_t qp_agg_i64(ray_t* v, int64_t dflt) {
    v = qp_solid(v);
    if (!v || RAY_IS_ERR(v)) { if (v) ray_release(v); return dflt; }
    int64_t r = q_is_int_atom(v) && !RAY_ATOM_IS_NULL(v) ? q_iatom_val(v) : dflt;
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
            q_cell(col, i, cb, sizeof cb);
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

void q_pipe_console(ray_t* val, char* buf, size_t bufsz) {
    if (!buf || bufsz == 0) return;
    buf[0] = '\0';
    qp_out o = { buf, bufsz, 0, 0 };

    int32_t crows = 0, ccols = 0;
    bool armed = q_con_display(&crows, &ccols);
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
            q_cell(cs[c].col, r, cells[c], QP_CELL);
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
