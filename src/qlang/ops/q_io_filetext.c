/* ops/q_io_filetext.c — the q `0:` File Text surface: Save Text, Prepare Text,
 * Load CSV, Load Fixed, Key-Value Pairs, plus the `-14!` CSV-quote alias.
 * Oracle: ref/file-text.md (CLEAN ROOM).  Split out of ops/q_io.c (2026-07-31),
 * which keeps the path/byte layer this file stands on (q_io_file_path,
 * q_io_read_all) — the base-plus-specialization shape net/q_wire + q_wirefile
 * already use.  Binary file formats (`1:`/`2:`) are not implemented here yet.
 *
 * Ownership: helper inputs are BORROWED, helper outputs are OWNED by the
 * caller; on any partial failure a helper releases everything it allocated
 * before returning the error.
 *
 * RAY_FN_RESTRICTED note: the base file primitives carry the flag on their ENV
 * fn objects; calling the C functions directly bypasses the eval-layer check,
 * so every file-touching arm re-asserts ray_eval_get_restricted(). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* q_io_read_all, q_str_split_lines, q_list_collapse */
#include "qlang/q_err.h"
#include "qlang/ops/q_dollar.h" /* q_cast_designator, q_dollar_tok — Tok column parses */
#include "qlang/q_builtins.h" /* q_string_fn — Prepare Text cell text; q_io_filetext_csv_quote decl */
#include "lang/eval.h"      /* ray_eval_get_restricted, ray_write_file_fn, ray_at_fn */
#include "table/sym.h"      /* ray_sym_intern_runtime, ray_sym_str */
#include "store/fileio.h"   /* ray_mkdir_p — Save Text missing dirs */
#include <stdlib.h>
#include <string.h>

/* ---- Save Text: `:path 0: strings -------------------------------------- */
static ray_t* ft_save_text(ray_t* fsym, ray_t* y) {
    ray_t* path = q_io_file_path(fsym);
    if (!path) return q_err(QE_TYPE);
    if (ray_eval_get_restricted()) { ray_release(path); return q_err(QE_ACCESS); }
    if (!y || !(y->type == RAY_LIST || y->type == RAY_STR)) {
        ray_release(path);
        return q_err(QE_TYPE);
    }
    int64_t n = ray_len(y);
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(y, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { ray_release(path); return e ? e : q_err(QE_OOM); }
        if (e->type != -RAY_STR) {
            ray_release(e); ray_release(path);
            return q_err(QE_TYPE);
        }
        total += ray_str_len(e) + 1;                       /* line + '\n' */
        ray_release(e);
    }
    char* buf = (char*)malloc(total ? total : 1);
    if (!buf) { ray_release(path); return q_err(QE_OOM); }
    size_t w = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(y, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { free(buf); ray_release(path); return e ? e : q_err(QE_OOM); }
        size_t l = ray_str_len(e);
        memcpy(buf + w, ray_str_ptr(e), l);
        w += l;
        buf[w++] = '\n';
        ray_release(e);
    }
    /* create missing parent directories (the doc's "any missing containing
     * directories"); ray_mkdir_p is the shared portable impl. */
    {
        const char* pp = ray_str_ptr(path);
        size_t pn = ray_str_len(path);
        size_t cut = pn;
        while (cut > 0 && pp[cut - 1] != '/') cut--;
        if (cut > 1) {
            char* dir = (char*)malloc(cut);
            if (dir) {
                memcpy(dir, pp, cut - 1);
                dir[cut - 1] = '\0';
                (void)ray_mkdir_p(dir);
                free(dir);
            }
        }
    }
    ray_t* content = ray_str(buf, w);
    free(buf);
    if (!content || RAY_IS_ERR(content)) { ray_release(path); return content ? content : q_err(QE_OOM); }
    ray_t* r = ray_write_file_fn(path, content);
    ray_release(path);
    ray_release(content);
    if (!r || RAY_IS_ERR(r)) return r;
    ray_release(r);
    ray_retain(fsym);
    return fsym;
}

/* ---- Prepare Text: delim 0: table | list-of-columns --------------------- */

/* One cell -> OWNED RAY_STR raw text (no quoting).  Borrows atom. */
static ray_t* ft_cell_text(ray_t* atom) {
    ray_t* s0 = q_string_fn(atom);              /* charv post-1b */
    if (!s0 || RAY_IS_ERR(s0)) return s0;
    ray_t* s = q_str_in(s0);                    /* legacy STR for the writers */
    ray_release(s0);
    if (!s || RAY_IS_ERR(s)) return s;
    if (atom->type == -RAY_DATE && s->type == -RAY_STR) {
        /* Prepare Text renders temporals ISO 8601 (doc: 2022-03-14) — the
         * date dots become dashes; other temporals already match. */
        size_t n = ray_str_len(s);
        char* b = (char*)malloc(n ? n : 1);
        if (b) {
            const char* p = ray_str_ptr(s);
            for (size_t i = 0; i < n; i++) b[i] = p[i] == '.' ? '-' : p[i];
            ray_t* d = ray_str(b, n);
            free(b);
            ray_release(s);
            return d;
        }
    }
    return s;
}

/* Quote rule (doc): a cell containing the delimiter or a line break is
 * embraced with '"' and every embedded '"' doubled; otherwise raw.  Appends
 * the (possibly embraced) cell to *buf/(w..cap).  Returns 0 on OOM. */
static int ft_quote_append(char** buf, size_t* w, size_t* cap,
                             const char* c, size_t n, char delim) {
    int embrace = 0;
    size_t extra = 2;
    for (size_t i = 0; i < n; i++) {
        if (c[i] == delim || c[i] == '\n') embrace = 1;
        if (c[i] == '"') extra++;
    }
    size_t need = *w + n + (embrace ? extra : 0) + 2;
    if (need > *cap) {
        size_t nc = *cap ? *cap : 64;
        while (nc < need) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb; *cap = nc;
    }
    char* b = *buf;
    if (!embrace) {
        memcpy(b + *w, c, n);
        *w += n;
        return 1;
    }
    b[(*w)++] = '"';
    for (size_t i = 0; i < n; i++) {
        if (c[i] == '"') b[(*w)++] = '"';
        b[(*w)++] = c[i];
    }
    b[(*w)++] = '"';
    return 1;
}

/* `-14!x` quote escape (basics/internal.md: prepare data for CSV export) — ONE
 * cell through the Save-Text quote rule above, delimiter fixed at ','. */
ray_t* q_io_filetext_csv_quote(ray_t* x) {
    const char* p;
    int64_t n;
    if (!q_str_text_bytes(x, &p, &n)) return q_err(QE_TYPE);
    char* buf = NULL;
    size_t w = 0, cap = 0;
    if (!ft_quote_append(&buf, &w, &cap, p, (size_t)n, ',')) {
        free(buf);
        return q_err(QE_WSFULL);
    }
    ray_t* r = ray_charv(buf ? buf : "", (int64_t)w);
    free(buf);
    return r;
}

static ray_t* ft_prepare(char delim, ray_t* y) {
    /* columns + optional names */
    int64_t nc = 0;
    ray_t* namev = NULL;                 /* borrowed via table introspection */
    ray_t** litems = NULL;
    int is_table = y && y->type == RAY_TABLE;
    if (is_table) nc = ray_table_ncols(y);
    else if (y && y->type == RAY_LIST) { nc = ray_len(y); litems = (ray_t**)ray_data(y); }
    else return q_err(QE_TYPE);
    (void)namev;
    if (nc == 0) return ray_list_new(1);
    /* validate columns; find the shared row count */
    int64_t L = -1;
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = is_table ? ray_table_get_col_idx(y, c) : litems[c];  /* borrowed */
        int64_t l;
        if (col && col->type == -RAY_STR) l = (int64_t)ray_str_len(col);  /* char column */
        else if (col && (ray_is_vec(col) || col->type == RAY_LIST)) {
            l = ray_len(col);
            if (col->type == RAY_LIST) {                  /* must be all strings */
                ray_t** it = (ray_t**)ray_data(col);
                for (int64_t i = 0; i < l; i++)
                    if (!it[i] || (it[i]->type != -RAY_STR && it[i]->type != RAY_CHARV))
                        return q_err(QE_TYPE);
            }
        } else return q_err(QE_TYPE);
        if (L < 0) L = l;
        else if (l != L) return q_err(QE_LENGTH);
    }
    ray_t* out = ray_list_new(L + 1 > 0 ? L + 1 : 1);
    if (RAY_IS_ERR(out)) return out;
    char* buf = NULL;
    size_t cap = 0;
    /* header row: table column names */
    if (is_table) {
        size_t w = 0;
        for (int64_t c = 0; c < nc; c++) {
            if (c) {
                if (w + 1 > cap) { cap = cap ? cap * 2 : 64; buf = (char*)realloc(buf, cap); if (!buf) { ray_release(out); return q_err(QE_OOM); } }
                buf[w++] = delim;
            }
            int64_t nm = ray_table_col_name(y, c);
            ray_t* ns = ray_sym_str(nm);                   /* borrowed */
            if (!ns || !ft_quote_append(&buf, &w, &cap, ray_str_ptr(ns), ray_str_len(ns), delim)) {
                free(buf); ray_release(out);
                return q_err(QE_OOM);
            }
        }
        ray_t* line = ray_str(buf ? buf : "", w);
        out = ray_list_append(out, line);
        ray_release(line);
        if (RAY_IS_ERR(out)) { free(buf); return out; }
    }
    for (int64_t i = 0; i < L; i++) {
        size_t w = 0;
        for (int64_t c = 0; c < nc; c++) {
            if (c) {
                if (w + 1 > cap) { cap = cap ? cap * 2 : 64; buf = (char*)realloc(buf, cap); if (!buf) { ray_release(out); return q_err(QE_OOM); } }
                buf[w++] = delim;
            }
            ray_t* col = is_table ? ray_table_get_col_idx(y, c) : litems[c];  /* borrowed */
            int ok;
            if (col->type == -RAY_STR) {                   /* char column: one char */
                char ch = ray_str_ptr(col)[i];
                ok = ft_quote_append(&buf, &w, &cap, &ch, 1, delim);
            } else if (col->type == RAY_CHARV) {           /* char column (charv) */
                char ch = ((const char*)ray_data(col))[i];
                ok = ft_quote_append(&buf, &w, &cap, &ch, 1, delim);
            } else if (col->type == RAY_LIST) {            /* string column */
                ray_t** it = (ray_t**)ray_data(col);
                const char* cp; int64_t cn;
                if (!q_str_text_bytes(it[i], &cp, &cn)) { cp = ""; cn = 0; }
                ok = ft_quote_append(&buf, &w, &cap, cp, (size_t)cn, delim);
            } else {
                ray_t* ia = ray_i64(i);
                ray_t* atom = ray_at_fn(col, ia);
                ray_release(ia);
                if (!atom || RAY_IS_ERR(atom)) { free(buf); ray_release(out); return atom ? atom : q_err(QE_OOM); }
                ray_t* cs = ft_cell_text(atom);
                ray_release(atom);
                if (!cs || RAY_IS_ERR(cs)) { free(buf); ray_release(out); return cs ? cs : q_err(QE_OOM); }
                if (cs->type != -RAY_STR) { ray_release(cs); free(buf); ray_release(out); return q_err(QE_TYPE); }
                ok = ft_quote_append(&buf, &w, &cap, ray_str_ptr(cs), ray_str_len(cs), delim);
                ray_release(cs);
            }
            if (!ok) { free(buf); ray_release(out); return q_err(QE_OOM); }
        }
        ray_t* line = ray_str(buf ? buf : "", w);
        out = ray_list_append(out, line);
        ray_release(line);
        if (RAY_IS_ERR(out)) { free(buf); return out; }
    }
    free(buf);
    return out;
}

/* ---- Load CSV / Load Fixed shared plumbing ------------------------------ */

/* Type char -> Tok tag via THE cast home (q_cast_designator; upper case =
 * Tok).  '*' keeps the field a string, ' ' skips the column, unknown -> 0. */
static int8_t ft_tag(char c, int* is_str, int* is_skip) {
    *is_str = 0; *is_skip = 0;
    if (c == ' ') { *is_skip = 1; return 0; }
    if (c == '*') { *is_str = 1; return 0; }
    if (c < 'A' || c > 'Z') return 0;                      /* doc: upper case */
    ray_t* d = ray_str(&c, 1);
    if (!d || RAY_IS_ERR(d)) return 0;
    int is_tok = 0;
    int8_t tag = q_cast_designator(d, &is_tok, NULL);
    ray_release(d);
    return is_tok ? tag : 0;
}

/* Normalize the RIGHT operand of Load CSV / Load Fixed into an OWNED
 * RAY_LIST of row strings.  *single = 1 for the one-string-no-newline form
 * (kdb returns a list of parsed ATOMS for it, not columns). */
static ray_t* ft_rows(ray_t* y, int* single) {
    *single = 0;
    if (!y) return q_err(QE_TYPE);
    if (y->type == -RAY_STR) {
        const char* p = ray_str_ptr(y);
        size_t n = ray_str_len(y);
        if (memchr(p, '\n', n)) return q_str_split_lines(p, n);
        *single = 1;
        ray_t* out = ray_list_new(1);
        if (RAY_IS_ERR(out)) return out;
        out = ray_list_append(out, y);                     /* retains y */
        return out;
    }
    if (y->type == -RAY_SYM) {
        ray_t* path = q_io_file_path(y);
        if (!path) return q_err(QE_TYPE);
        ray_t* all = q_io_read_all(path);
        ray_release(path);
        if (!all || RAY_IS_ERR(all)) return all;
        ray_t* rows = q_str_split_lines(ray_str_ptr(all), ray_str_len(all));
        ray_release(all);
        return rows;
    }
    if (y->type == RAY_LIST || y->type == RAY_STR) {
        int64_t n = ray_len(y);
        ray_t** e = y->type == RAY_LIST ? (ray_t**)ray_data(y) : NULL;
        /* (filesymbol; offset[; length]) chunk form */
        if (e && n >= 2 && n <= 3 && e[0] && e[0]->type == -RAY_SYM) {
            ray_t* path = q_io_file_path(e[0]);
            if (!path) return q_err(QE_TYPE);
            int64_t off, want = -1;
            if (!q_type_strict_i64(e[1], &off) || (n == 3 && !q_type_strict_i64(e[2], &want))) {
                ray_release(path);
                return q_err(QE_TYPE);
            }
            ray_t* all = q_io_read_all(path);
            ray_release(path);
            if (!all || RAY_IS_ERR(all)) return all;
            const char* p = ray_str_ptr(all);
            int64_t len = (int64_t)ray_str_len(all);
            if (off < 0) off = 0;
            if (off > len) off = len;
            int64_t end = want >= 0 && off + want < len ? off + want : len;
            ray_t* rows = q_str_split_lines(p + off, (size_t)(end - off));
            ray_release(all);
            return rows;
        }
        /* list / str-vector of row strings */
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* it = ray_at_fn(y, ia);
            ray_release(ia);
            if (!it || RAY_IS_ERR(it)) { ray_release(out); return it ? it : q_err(QE_OOM); }
            if (it->type != -RAY_STR) {
                ray_release(it); ray_release(out);
                return q_err(QE_TYPE);
            }
            out = ray_list_append(out, it);
            ray_release(it);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return q_err(QE_TYPE);
}

/* flag=1 (embedded line returns): merge physical rows whose quotes are
 * unbalanced with the following row, restoring the '\n'.  Owns+returns. */
static ray_t* ft_merge_quoted(ray_t* rows) {
    int64_t n = ray_len(rows);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) { ray_release(rows); return out; }
    ray_t** e = (ray_t**)ray_data(rows);
    char* acc = NULL;                        /* pending logical row */
    size_t used = 0;                         /* bytes of acc in use */
    size_t quotes = 0;                       /* running '"' parity  */
    for (int64_t i = 0; i < n; i++) {
        const char* p = ray_str_ptr(e[i]);
        size_t l = ray_str_len(e[i]);
        char* na = (char*)realloc(acc, used + l + 2);
        if (!na) { free(acc); ray_release(out); ray_release(rows); return q_err(QE_OOM); }
        acc = na;
        if (used) acc[used++] = '\n';        /* rejoin BEFORE the copy — the '\n'
                                              * must not land on memcpy's target */
        memcpy(acc + used, p, l);
        used += l;
        for (size_t k = 0; k < l; k++) if (p[k] == '"') quotes++;
        if ((quotes & 1) == 0) {
            ray_t* s = ray_str(acc, used);
            out = ray_list_append(out, s);
            ray_release(s);
            free(acc); acc = NULL; used = 0; quotes = 0;
            if (RAY_IS_ERR(out)) { ray_release(rows); return out; }
        }
    }
    if (acc) {                               /* unterminated tail row */
        ray_t* s = ray_str(acc, used);
        out = ray_list_append(out, s);
        ray_release(s);
        free(acc);
        if (RAY_IS_ERR(out)) { ray_release(rows); return out; }
    }
    ray_release(rows);
    return out;
}

/* Split one row into fields on a delimiter; '"'-opened fields are
 * quote-scanned ('""' -> literal '"').  Appends OWNED strings to `fields`. */
static ray_t* ft_fields(ray_t* fields, const char* r, size_t n, char delim) {
    size_t i = 0;
    for (;;) {
        char* fb = (char*)malloc(n + 1);
        if (!fb) { ray_release(fields); return q_err(QE_OOM); }
        size_t fl = 0;
        if (i < n && r[i] == '"') {
            i++;
            while (i < n) {
                if (r[i] == '"') {
                    if (i + 1 < n && r[i + 1] == '"') { fb[fl++] = '"'; i += 2; }
                    else { i++; break; }
                } else fb[fl++] = r[i++];
            }
            while (i < n && r[i] != delim) i++;             /* junk after quote */
        } else {
            while (i < n && r[i] != delim) fb[fl++] = r[i++];
        }
        ray_t* f = ray_str(fb, fl);
        free(fb);
        fields = ray_list_append(fields, f);
        ray_release(f);
        if (RAY_IS_ERR(fields)) return fields;
        if (i >= n) break;
        i++;                                                /* past delim */
        if (i == n) {                                       /* trailing delim */
            ray_t* z = ray_str("", 0);
            fields = ray_list_append(fields, z);
            ray_release(z);
            break;
        }
    }
    return fields;
}

/* Parse one field per its column recipe.  OWNED atom/string. */
static ray_t* ft_parse_field(ray_t* field, int8_t tag, int is_str) {
    if (is_str) { ray_retain(field); return field; }
    return q_dollar_tok(tag, field);
}

/* Collapse a column accumulator: '*' columns stay lists of strings; typed
 * columns Tok-parse (q_dollar_tok distributes over lists) then collapse. */
static ray_t* ft_finish_col(ray_t* colacc, int8_t tag, int is_str) {
    if (is_str) { ray_retain(colacc); return colacc; }
    ray_t* parsed = q_dollar_tok(tag, colacc);
    if (!parsed || RAY_IS_ERR(parsed)) return parsed;
    ray_t* v = q_list_collapse(parsed);                     /* owned */
    ray_release(parsed);
    return v;
}

static ray_t* ft_load_csv(ray_t* types, ray_t* delimspec, ray_t* flag, ray_t* y) {
    const char* ts = ray_str_ptr(types);
    size_t nt = ray_str_len(types);
    if (nt == 0) return q_err(QE_TYPE);
    /* delimiter: char atom (len-1 string) or enlisted -> header row */
    char delim;
    int header = 0;
    if (delimspec && delimspec->type == -RAY_STR && ray_str_len(delimspec) == 1)
        delim = ray_str_ptr(delimspec)[0];
    else if (delimspec &&
             (delimspec->type == RAY_LIST || delimspec->type == RAY_STR) &&
             ray_len(delimspec) == 1) {
        /* enlisted delimiter -> first row is column names.  `enlist ","` is
         * an engine STR VECTOR (10h), a boxed 1-list also accepted. */
        ray_t* ia = ray_i64(0);
        ray_t* d0 = ray_at_fn(delimspec, ia);
        ray_release(ia);
        if (!d0 || RAY_IS_ERR(d0)) return d0 ? d0 : q_err(QE_OOM);
        if (d0->type != -RAY_STR || ray_str_len(d0) != 1) {
            ray_release(d0);
            return q_err(QE_TYPE);
        }
        delim = ray_str_ptr(d0)[0];
        ray_release(d0);
        header = 1;
    } else return q_err(QE_TYPE);
    int embed_nl = 0;
    if (flag) {
        int64_t fv;
        ray_t* ferr = q_type_i64_or_err(flag, &fv, "0:: flag");
        if (ferr) return ferr;
        embed_nl = fv != 0;
    }
    /* column recipes */
    int8_t* tags = (int8_t*)malloc(nt);
    int* fstr = (int*)malloc(nt * sizeof(int));
    int* fskip = (int*)malloc(nt * sizeof(int));
    if (!tags || !fstr || !fskip) { free(tags); free(fstr); free(fskip); return q_err(QE_OOM); }
    for (size_t j = 0; j < nt; j++) {
        tags[j] = ft_tag(ts[j], &fstr[j], &fskip[j]);
        if (!tags[j] && !fstr[j] && !fskip[j]) {
            char bad = ts[j];
            free(tags); free(fstr); free(fskip);
            if (bad == 'C')
                return q_err(QE_NYI);
            return q_err(QE_TYPE);
        }
    }
    int single = 0;
    ray_t* rows = ft_rows(y, &single);
    if (!rows || RAY_IS_ERR(rows)) { free(tags); free(fstr); free(fskip); return rows; }
    if (embed_nl) {
        rows = ft_merge_quoted(rows);
        if (!rows || RAY_IS_ERR(rows)) { free(tags); free(fstr); free(fskip); return rows; }
    }
    ray_t** rp = (ray_t**)ray_data(rows);
    int64_t nrows = ray_len(rows);
    ray_t* result = NULL;
    if (single) {
        /* one delimited string -> list of parsed atoms */
        ray_t* fields = ray_list_new((int64_t)nt);
        if (RAY_IS_ERR(fields)) { result = fields; goto done; }
        fields = ft_fields(fields, ray_str_ptr(rp[0]), ray_str_len(rp[0]), delim);
        if (RAY_IS_ERR(fields)) { result = fields; goto done; }
        ray_t** fp = (ray_t**)ray_data(fields);
        int64_t nf = ray_len(fields);
        ray_t* out = ray_list_new((int64_t)nt);
        if (RAY_IS_ERR(out)) { ray_release(fields); result = out; goto done; }
        ray_t* empty = ray_str("", 0);
        for (size_t j = 0; j < nt && !RAY_IS_ERR(out); j++) {
            if (fskip[j]) continue;
            ray_t* f = (int64_t)j < nf ? fp[j] : empty;     /* borrowed */
            ray_t* a = ft_parse_field(f, tags[j], fstr[j]);
            if (!a || RAY_IS_ERR(a)) { ray_release(empty); ray_release(fields); ray_release(out); result = a ? a : q_err(QE_OOM); goto done; }
            out = ray_list_append(out, a);
            ray_release(a);
        }
        ray_release(empty);
        ray_release(fields);
        result = out;
        goto done;
    }
    {
        /* rows mode: per-column accumulators over the data rows */
        int64_t first = header ? 1 : 0;
        int64_t ndata = nrows - first;
        if (ndata < 0) ndata = 0;
        int64_t nout = 0;
        for (size_t j = 0; j < nt; j++) if (!fskip[j]) nout++;
        ray_t** acc = (ray_t**)calloc((size_t)(nout > 0 ? nout : 1), sizeof(ray_t*));
        if (!acc) { result = q_err(QE_OOM); goto done; }
        int64_t k = 0;
        for (size_t j = 0; j < nt; j++) {
            if (fskip[j]) continue;
            acc[k] = ray_list_new(ndata > 0 ? ndata : 1);
            if (RAY_IS_ERR(acc[k])) {
                result = acc[k];
                for (int64_t z = 0; z < k; z++) ray_release(acc[z]);
                free(acc);
                goto done;
            }
            k++;
        }
        ray_t* empty = ray_str("", 0);
        for (int64_t i = first; i < nrows; i++) {
            ray_t* fields = ray_list_new((int64_t)nt);
            if (!RAY_IS_ERR(fields))
                fields = ft_fields(fields, ray_str_ptr(rp[i]), ray_str_len(rp[i]), delim);
            if (RAY_IS_ERR(fields)) {
                for (int64_t z = 0; z < nout; z++) ray_release(acc[z]);
                free(acc); ray_release(empty);
                result = fields;
                goto done;
            }
            ray_t** fp = (ray_t**)ray_data(fields);
            int64_t nf = ray_len(fields);
            int64_t c = 0;
            for (size_t j = 0; j < nt; j++) {
                if (fskip[j]) continue;
                ray_t* f = (int64_t)j < nf ? fp[j] : empty; /* borrowed */
                acc[c] = ray_list_append(acc[c], f);
                c++;
            }
            ray_release(fields);
        }
        ray_release(empty);
        /* finish columns */
        ray_t* cols = ray_list_new(nout > 0 ? nout : 1);
        for (int64_t z = 0; z < nout && !RAY_IS_ERR(cols); z++) {
            int64_t j = -1, seen = -1;
            for (size_t t = 0; t < nt; t++) {
                if (fskip[t]) continue;
                if (++seen == z) { j = (int64_t)t; break; }
            }
            ray_t* col = ft_finish_col(acc[z], tags[j], fstr[j]);
            if (!col || RAY_IS_ERR(col)) {
                ray_release(cols);
                cols = col ? col : q_err(QE_OOM);
                break;
            }
            cols = ray_list_append(cols, col);
            ray_release(col);
        }
        for (int64_t z = 0; z < nout; z++) ray_release(acc[z]);
        free(acc);
        if (RAY_IS_ERR(cols)) { result = cols; goto done; }
        if (!header) { result = cols; goto done; }
        /* header: first row = column names -> table */
        ray_t* nmf = ray_list_new((int64_t)nt);
        if (!RAY_IS_ERR(nmf) && nrows > 0)
            nmf = ft_fields(nmf, ray_str_ptr(rp[0]), ray_str_len(rp[0]), delim);
        if (RAY_IS_ERR(nmf)) { ray_release(cols); result = nmf; goto done; }
        ray_t** np = (ray_t**)ray_data(nmf);
        int64_t nn = ray_len(nmf);
        ray_t* tbl = ray_table_new(nout > 0 ? nout : 1);
        int64_t c2 = 0;
        ray_t** cp = (ray_t**)ray_data(cols);
        for (size_t j = 0; j < nt && !RAY_IS_ERR(tbl); j++) {
            if (fskip[j]) continue;
            int64_t nm = (int64_t)j < nn
                ? ray_sym_intern_runtime(ray_str_ptr(np[j]), ray_str_len(np[j]))
                : ray_sym_intern_runtime("", 0);
            tbl = ray_table_add_col(tbl, nm, cp[c2]);
            c2++;
        }
        ray_release(nmf);
        ray_release(cols);
        result = tbl;
    }
done:
    ray_release(rows);
    free(tags); free(fstr); free(fskip);
    return result;
}

static ray_t* ft_load_fixed(ray_t* types, ray_t* widths, ray_t* y) {
    const char* ts = ray_str_ptr(types);
    size_t nt = ray_str_len(types);
    if (nt == 0 || (int64_t)nt != ray_len(widths))
        return q_err(QE_LENGTH);
    /* widths must be positive (codex P1: a negative width made the slice
     * length negative and reached memcpy as a huge size_t). */
    for (int64_t j = 0; j < (int64_t)nt; j++)
        if (q_type_ivec_get(widths, j) <= 0)
            return q_err(QE_DOMAIN);
    int8_t* tags = (int8_t*)malloc(nt);
    int* fstr = (int*)malloc(nt * sizeof(int));
    int* fskip = (int*)malloc(nt * sizeof(int));
    if (!tags || !fstr || !fskip) { free(tags); free(fstr); free(fskip); return q_err(QE_OOM); }
    for (size_t j = 0; j < nt; j++) {
        tags[j] = ft_tag(ts[j], &fstr[j], &fskip[j]);
        if (!tags[j] && !fstr[j] && !fskip[j]) {
            free(tags); free(fstr); free(fskip);
            return q_err(QE_TYPE);
        }
    }
    int single = 0;
    ray_t* rows = ft_rows(y, &single);
    if (!rows || RAY_IS_ERR(rows)) { free(tags); free(fstr); free(fskip); return rows; }
    ray_t** rp = (ray_t**)ray_data(rows);
    int64_t nrows = ray_len(rows);
    int64_t nout = 0;
    for (size_t j = 0; j < nt; j++) if (!fskip[j]) nout++;
    ray_t* result = NULL;
    ray_t** acc = (ray_t**)calloc((size_t)(nout > 0 ? nout : 1), sizeof(ray_t*));
    if (!acc) { result = q_err(QE_OOM); goto done; }
    for (int64_t z = 0; z < nout; z++) {
        acc[z] = ray_list_new(nrows > 0 ? nrows : 1);
        if (RAY_IS_ERR(acc[z])) {
            result = acc[z];
            for (int64_t q = 0; q < z; q++) ray_release(acc[q]);
            free(acc);
            goto done;
        }
    }
    for (int64_t i = 0; i < nrows; i++) {
        const char* p = ray_str_ptr(rp[i]);
        int64_t n = (int64_t)ray_str_len(rp[i]);
        int64_t pos = 0, c = 0;
        for (size_t j = 0; j < nt; j++) {
            int64_t w = q_type_ivec_get(widths, (int64_t)j);
            int64_t s = pos > n ? n : pos;
            int64_t e = pos + w > n ? n : pos + w;
            pos += w;
            if (fskip[j]) continue;
            ray_t* f = ray_str(p + s, (size_t)(e - s));
            if (!f || RAY_IS_ERR(f)) {
                for (int64_t z = 0; z < nout; z++) ray_release(acc[z]);
                free(acc);
                result = f ? f : q_err(QE_OOM);
                goto done;
            }
            acc[c] = ray_list_append(acc[c], f);
            ray_release(f);
            c++;
        }
    }
    {
        ray_t* cols = ray_list_new(nout > 0 ? nout : 1);
        int64_t z = 0;
        for (size_t j = 0; j < nt && !RAY_IS_ERR(cols); j++) {
            if (fskip[j]) continue;
            ray_t* col = ft_finish_col(acc[z], tags[j], fstr[j]);
            if (!col || RAY_IS_ERR(col)) {
                ray_release(cols);
                cols = col ? col : q_err(QE_OOM);
                break;
            }
            cols = ray_list_append(cols, col);
            ray_release(col);
            z++;
        }
        for (int64_t q = 0; q < nout; q++) ray_release(acc[q]);
        free(acc);
        result = cols;
    }
done:
    ray_release(rows);
    free(tags); free(fstr); free(fskip);
    return result;
}

/* ---- Key-Value Pairs: "K f [*] r" 0: string ------------------------------ */
static ray_t* ft_kv(const char* spec, size_t sn, ray_t* y) {
    char ktype = spec[0];
    int star = sn == 4;
    if (star && spec[2] != '*') return q_err(QE_TYPE);
    char fsep = spec[1];
    char rsep = star ? spec[3] : spec[2];
    if (!y || y->type != -RAY_STR)
        return q_err(QE_TYPE);
    int8_t ktag;
    {
        int is_str = 0, is_skip = 0;
        ktag = ft_tag(ktype, &is_str, &is_skip);
        if (!ktag) return q_err(QE_TYPE);
    }
    const char* p = ray_str_ptr(y);
    size_t n = ray_str_len(y);
    ray_t* keys = ray_list_new(4);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(4);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    size_t i = 0;
    while (i < n) {
        /* one record: up to rsep (quote-aware in '*' mode) */
        size_t start = i;
        int inq = 0;
        while (i < n && (inq || p[i] != rsep)) {
            if (star && p[i] == '"') inq = !inq;
            i++;
        }
        size_t end = i;
        if (i < n) i++;                                     /* past rsep */
        if (end == start) continue;                         /* empty record */
        /* split at the FIRST fsep */
        size_t f = start;
        while (f < end && p[f] != fsep) f++;
        ray_t* k = ray_str(p + start, f - start);
        size_t vs = f < end ? f + 1 : end;
        ray_t* v;
        if (star && vs < end && p[vs] == '"' && p[end - 1] == '"' && end - vs >= 2) {
            /* quoted value: strip the outer quotes, un-double inner ones */
            char* vb = (char*)malloc(end - vs);
            size_t vl = 0;
            if (!vb) { ray_release(k); ray_release(keys); ray_release(vals); return q_err(QE_OOM); }
            for (size_t t = vs + 1; t < end - 1; t++) {
                if (p[t] == '"' && t + 1 < end - 1 && p[t + 1] == '"') { vb[vl++] = '"'; t++; }
                else vb[vl++] = p[t];
            }
            v = ray_str(vb, vl);
            free(vb);
        } else v = ray_str(p + vs, end - vs);
        if (!k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v)) {
            if (k && !RAY_IS_ERR(k)) ray_release(k);
            if (v && !RAY_IS_ERR(v)) ray_release(v);
            ray_release(keys); ray_release(vals);
            return q_err(QE_OOM);
        }
        ray_t* ka = q_dollar_tok(ktag, k);
        ray_release(k);
        if (!ka || RAY_IS_ERR(ka)) { ray_release(v); ray_release(keys); ray_release(vals); return ka ? ka : q_err(QE_OOM); }
        keys = ray_list_append(keys, ka);
        ray_release(ka);
        vals = ray_list_append(vals, v);
        ray_release(v);
        if (RAY_IS_ERR(keys) || RAY_IS_ERR(vals)) {
            ray_t* err = RAY_IS_ERR(keys) ? keys : vals;
            if (!RAY_IS_ERR(keys)) ray_release(keys);
            if (!RAY_IS_ERR(vals)) ray_release(vals);
            return err;
        }
    }
    ray_t* kv = q_list_collapse(keys);                      /* typed key vector */
    ray_release(keys);
    if (!kv || RAY_IS_ERR(kv)) { ray_release(vals); return kv ? kv : q_err(QE_OOM); }
    ray_t* out = ray_list_new(2);
    if (RAY_IS_ERR(out)) { ray_release(kv); ray_release(vals); return out; }
    out = ray_list_append(out, kv);
    ray_release(kv);
    if (RAY_IS_ERR(out)) { ray_release(vals); return out; }
    out = ray_list_append(out, vals);
    ray_release(vals);
    return out;
}


/* ---- the `0:` dispatcher -------------------------------------------------- */
static ray_t* io_filetext_impl(ray_t* x, ray_t* y);
/* x-normalize preserving the bare-vs-ENLISTED delimiter distinction the
 * charv model carries natively: char ATOM -> 1-char STR (bare delim); charv
 * len-1 -> boxed 1-list of a 1-char STR (the legacy enlisted form, header
 * row); other charv -> STR (types/kv spec); LIST -> per-element.  Owned. */
static ray_t* ft_norm_x(ray_t* x) {
    if (x && x->type == -RAY_CHARV) { char c = (char)x->u8; return ray_str(&c, 1); }
    if (x && x->type == RAY_CHARV) {
        if (ray_len(x) == 1) {
            ray_t* s = q_str_of_charv(x);
            if (!s || RAY_IS_ERR(s)) return s;
            ray_t* l = ray_list_new(1);
            if (!l || RAY_IS_ERR(l)) { ray_release(s); return l; }
            l = ray_list_append(l, s);
            ray_release(s);
            return l;
        }
        return q_str_of_charv(x);
    }
    if (x && x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (!out || RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* c = ft_norm_x(e[i]);
            if (!c) { ray_retain(e[i]); c = e[i]; }
            if (RAY_IS_ERR(c)) { ray_release(out); return c; }
            out = ray_list_append(out, c);
            ray_release(c);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x) ray_retain(x);
    return x;
}
ray_t* q_io_filetext_wrap(ray_t* x, ray_t* y) {
    ray_t* xs = ft_norm_x(x); ray_t* ys = q_str_in(y);
    ray_t* r = io_filetext_impl(xs, ys);
    ray_release(xs); ray_release(ys);
    return q_str_charv_out(r);              /* parsed strings cross as charv */
}
static ray_t* io_filetext_impl(ray_t* x, ray_t* y) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == -RAY_SYM) return ft_save_text(x, y);
    if (x->type == -RAY_STR) {
        const char* s = ray_str_ptr(x);
        size_t n = ray_str_len(x);
        if (n == 1) return ft_prepare(s[0], y);
        if ((n == 3 || n == 4) && (s[0] == 'S' || s[0] == 'I' || s[0] == 'J'))
            return ft_kv(s, n, y);
        return q_err(QE_TYPE);
    }
    if (x->type == RAY_LIST && (ray_len(x) == 2 || ray_len(x) == 3)) {
        ray_t** e = (ray_t**)ray_data(x);
        if (e[0] && e[0]->type == -RAY_STR) {
            if (ray_len(x) == 2 && q_type_is_int_vec(e[1]))
                return ft_load_fixed(e[0], e[1], y);
            return ft_load_csv(e[0], e[1], ray_len(x) == 3 ? e[2] : NULL, y);
        }
    }
    return q_err(QE_TYPE);
}
