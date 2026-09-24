/* q_md — Markdown as q values.  md4c (third_party/md4c) does all of the parsing and the HTML rendering; this file
 * only adapts its callbacks: .md.i.parse folds the block events into one row per block, .md.i.html collects
 * md4c-html's XHTML.  A markdown table's val is its cells as strings, rows of them header first, which lib/md.q
 * hands to .csv.read. */
#include "qlang/q_count.h"
#include "qlang/io/q_md.h"
#include "qlang/q_prim.h"        /* q_str_text_bytes, q_join_grow, q_list_collapse */
#include "qlang/base/q_err.h"
#include "qlang/base/q_utf8.h"   /* q_utf8_enc — a decoded entity's bytes */
#include "qlang/ops/q_table.h"   /* q_table_cols_from_accs — the block table's assembly */
#include "qlang/q_env.h"         /* q_env_bind — the .md.i.* bindings */
#include "lang/env.h"            /* ray_fn_unary */
#include "lang/eval.h"           /* RAY_FN_NONE */
#include "md4c.h"
#include "md4c-html.h"
#include "entity.h"              /* entity_lookup — md4c-html's named-entity table */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MD_PARSE_FLAGS (MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS)

/* *b is an owned char vector that grows in place, or the error that stopped it */
static void md_put(ray_t** b, const char* s, size_t n) {
    if (!n || RAY_IS_ERR(*b)) return;
    ray_t* y = ray_charv(s, (int64_t)n);
    if (RAY_IS_ERR(y)) { ray_release(*b); *b = y; return; }
    ray_t* r = q_join_grow(b, y);
    ray_release(y);
    if (!r || RAY_IS_ERR(r)) { ray_release(*b); r = r ? r : q_err(QE_WSFULL); }
    *b = r;
}

static ray_t* md_take(ray_t** b) {
    ray_t* s = *b;
    *b = ray_charv("", 0);
    return s;
}

typedef struct {
    MD_BLOCKTYPE type;
    int64_t depth, parent, head, level;
    ray_t* info;                    /* owned; NULL reads as "" */
    ray_t* val;
} md_row;

typedef struct {
    md_row* rows;
    int64_t nrows, rowcap;
    int64_t* open;
    int64_t nopen, opencap;
    int64_t head, leaf;             /* the governing heading and the row collecting text, -1 for none */
    int synth;                      /* the leaf is a tight list item's paragraph, which md4c does not report */
    ray_t* text;
    int incell;
    ray_t* cells;                   /* the open table's rows so far, each a list of cell strings */
    ray_t* row;
    ray_t* err;
} md_st;

static int md_grow(void** p, int64_t* cap, int64_t need, size_t esz) {
    if (need <= *cap) return 1;
    int64_t c = *cap ? *cap * 2 : 16;
    void* q = realloc(*p, (size_t)c * esz);
    if (!q) return 0;
    *p = q;
    *cap = c;
    return 1;
}

static int md_fail(md_st* st, ray_t* e) {
    if (!st->err) st->err = e ? e : q_err(QE_WSFULL);
    else if (e) ray_release(e);
    return -1;
}

static int64_t md_push(md_st* st, MD_BLOCKTYPE t) {
    if (!md_grow((void**)&st->rows, &st->rowcap, st->nrows + 1, sizeof *st->rows) ||
        !md_grow((void**)&st->open, &st->opencap, st->nopen + 1, sizeof *st->open))
        return md_fail(st, NULL);
    int64_t r = st->nrows++;
    st->rows[r] = (md_row){ t, st->nopen, st->nopen ? st->open[st->nopen - 1] : NULL_I64,
                            st->head >= 0 ? st->head : NULL_I64, NULL_I64, NULL, NULL };
    st->open[st->nopen++] = r;
    return r;
}

static int md_close_leaf(md_st* st) {
    ray_t* v = md_take(&st->text);
    if (RAY_IS_ERR(v)) return md_fail(st, v);
    st->rows[st->leaf].val = v;
    st->leaf = -1;
    return 0;
}

static int md_close_synth(md_st* st) {
    st->synth = 0;
    st->nopen--;
    return md_close_leaf(st);
}

/* An entity is plain text decoded, as md4c-html renders it; an unknown name stays as written. */
static void md_entity(ray_t** b, const char* s, size_t n) {
    uint32_t cp[2] = { 0, 0 };
    if (n > 3 && s[1] == '#') {
        int hex = s[2] == 'x' || s[2] == 'X';
        for (size_t i = hex ? 3 : 2; i + 1 < n; i++)
            cp[0] = cp[0] * (hex ? 16 : 10) + (uint32_t)(s[i] <= '9' ? s[i] - '0' : (s[i] | 0x20) - 'a' + 10);
    } else {
        const ENTITY* e = entity_lookup(s, n);
        if (!e) { md_put(b, s, n); return; }
        cp[0] = e->codepoints[0];
        cp[1] = e->codepoints[1];
    }
    for (int i = 0; i < 2 && (i == 0 || cp[i]); i++) {
        uint32_t c = cp[i];
        char u[4];
        if (c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
        md_put(b, u, (size_t)q_utf8_enc(c, u));
    }
}

/* Plain text: markup is gone, a hard break is "\n", a soft break the space it renders as, and raw HTML survives
 * only as an html block's own text. */
static void md_put_text(md_st* st, MD_TEXTTYPE t, const MD_CHAR* s, size_t n) {
    switch (t) {
        case MD_TEXT_NULLCHAR: md_put(&st->text, "\xEF\xBF\xBD", 3); break;
        case MD_TEXT_BR:       md_put(&st->text, "\n", 1); break;
        case MD_TEXT_SOFTBR:   md_put(&st->text, " ", 1); break;
        case MD_TEXT_ENTITY:   md_entity(&st->text, s, n); break;
        case MD_TEXT_HTML:
            if (!st->incell && st->rows[st->leaf].type == MD_BLOCK_HTML) md_put(&st->text, s, n);
            break;
        default:               md_put(&st->text, s, n); break;
    }
}

static const char* md_kind(MD_BLOCKTYPE t) {
    switch (t) {
        case MD_BLOCK_DOC:   return "doc";
        case MD_BLOCK_QUOTE: return "quote";
        case MD_BLOCK_UL:    return "ul";
        case MD_BLOCK_OL:    return "ol";
        case MD_BLOCK_LI:    return "li";
        case MD_BLOCK_HR:    return "hr";
        case MD_BLOCK_H:     return "h";
        case MD_BLOCK_CODE:  return "code";
        case MD_BLOCK_HTML:  return "html";
        case MD_BLOCK_P:     return "p";
        case MD_BLOCK_TABLE: return "table";
        default:             return "";   /* the extensions MD_PARSE_FLAGS leaves off never reach here */
    }
}

static int md_enter(MD_BLOCKTYPE t, void* d, void* u) {
    md_st* st = u;
    if (st->synth && md_close_synth(st)) return -1;
    switch (t) {
        case MD_BLOCK_THEAD: case MD_BLOCK_TBODY: return 0;
        case MD_BLOCK_TR:
            st->row = ray_list_new(4);
            return RAY_IS_ERR(st->row) ? md_fail(st, NULL) : 0;
        case MD_BLOCK_TH: case MD_BLOCK_TD: st->incell = 1; return 0;
        default: break;
    }
    int64_t r = md_push(st, t);
    if (r < 0) return -1;
    md_row* w = &st->rows[r];
    switch (t) {
        case MD_BLOCK_H:
            w->level = ((MD_BLOCK_H_DETAIL*)d)->level;
            w->head = st->head = r;
            st->leaf = r;
            break;
        case MD_BLOCK_CODE: {
            const MD_ATTRIBUTE* a = &((MD_BLOCK_CODE_DETAIL*)d)->info;
            for (unsigned i = 0; a->size && a->substr_offsets[i] < a->size; i++)   /* an indented block has no info */
                md_put_text(st, a->substr_types[i], a->text + a->substr_offsets[i],
                            a->substr_offsets[i + 1] - a->substr_offsets[i]);
            w->info = md_take(&st->text);
            if (RAY_IS_ERR(w->info)) return md_fail(st, NULL);
            st->leaf = r;
            break;
        }
        case MD_BLOCK_P: case MD_BLOCK_HTML: st->leaf = r; break;
        case MD_BLOCK_LI: {
            MD_BLOCK_LI_DETAIL* li = d;
            if (li->is_task) w->info = ray_charv(&li->task_mark, 1);
            if (w->info && RAY_IS_ERR(w->info)) return md_fail(st, NULL);
            break;
        }
        case MD_BLOCK_TABLE:
            st->cells = ray_list_new(4);
            if (RAY_IS_ERR(st->cells)) return md_fail(st, NULL);
            break;
        default: break;
    }
    return 0;
}

/* appends s (consumed) to *l, answering the error that stopped it or NULL */
static ray_t* md_append(ray_t** l, ray_t* s) {
    if (RAY_IS_ERR(s)) return s;
    *l = ray_list_append(*l, s);
    ray_release(s);
    return RAY_IS_ERR(*l) ? *l : NULL;
}

static int md_leave(MD_BLOCKTYPE t, void* d, void* u) {
    (void)d;
    md_st* st = u;
    ray_t* e = NULL;
    switch (t) {
        case MD_BLOCK_THEAD: case MD_BLOCK_TBODY: return 0;
        case MD_BLOCK_TR:
            e = md_append(&st->cells, st->row);
            st->row = NULL;
            return e ? md_fail(st, e) : 0;
        case MD_BLOCK_TH: case MD_BLOCK_TD:
            st->incell = 0;
            e = md_append(&st->row, md_take(&st->text));
            return e ? md_fail(st, e) : 0;
        default: break;
    }
    if (st->synth && md_close_synth(st)) return -1;
    int64_t r = st->open[--st->nopen];
    if (st->leaf == r) return md_close_leaf(st);
    if (t == MD_BLOCK_TABLE) {
        st->rows[r].val = st->cells;
        st->cells = NULL;
    }
    return 0;
}

static int md_text(MD_TEXTTYPE t, const MD_CHAR* s, MD_SIZE n, void* u) {
    md_st* st = u;
    if (!st->incell && st->leaf < 0) {
        int64_t r = md_push(st, MD_BLOCK_P);
        if (r < 0) return -1;
        st->leaf = r;
        st->synth = 1;
    }
    md_put_text(st, t, s, n);
    return RAY_IS_ERR(st->text) ? md_fail(st, md_take(&st->text)) : 0;
}

/* inline spans are markup, which plain text drops */
static int md_span(MD_SPANTYPE t, void* d, void* u) { (void)t; (void)d; (void)u; return 0; }

static ray_t* md_rows_table(md_st* st) {
    static const char* nm[] = { "kind", "depth", "parent", "head", "level", "info", "val" };
    int64_t names[7];
    ray_t* accs[7];
    for (int k = 0; k < 7; k++) {
        names[k] = ray_sym_intern_runtime(nm[k], strlen(nm[k]));
        accs[k] = ray_list_new(st->nrows);
    }
    for (int64_t i = 0; i < st->nrows; i++) {
        md_row* w = &st->rows[i];
        const char* kind = md_kind(w->type);
        ray_t* cell[7] = { ray_sym(ray_sym_intern_runtime(kind, strlen(kind))), ray_i64(w->depth), ray_i64(w->parent),
                           ray_i64(w->head), ray_i64(w->level), w->info ? w->info : ray_charv("", 0),
                           w->val ? w->val : ray_charv("", 0) };
        w->info = w->val = NULL;
        for (int k = 0; k < 7; k++) {
            accs[k] = ray_list_append(accs[k], cell[k]);
            ray_release(cell[k]);
        }
    }
    return q_table_cols_from_accs(names, accs, 7, q_list_collapse);
}

/* The document as text; a leading UTF-8 byte-order mark is not part of it. */
static ray_t* md_arg(ray_t* x, const char** p, MD_SIZE* n) {
    int64_t len;
    if (!q_str_text_bytes(x, p, &len)) return q_err(QE_TYPE);
    if (len >= 3 && !memcmp(*p, "\xEF\xBB\xBF", 3)) { *p += 3; len -= 3; }
    if (len > (int64_t)UINT_MAX) return q_err(QE_LIMIT);
    *n = (MD_SIZE)len;
    return NULL;
}

static ray_t* md_parse_fn(ray_t* x) {
    const char* p;
    MD_SIZE n;
    ray_t* e = md_arg(x, &p, &n);
    if (e) return e;
    md_st st = { .head = -1, .leaf = -1, .text = ray_charv("", 0) };
    MD_PARSER parser = { 0, MD_PARSE_FLAGS, md_enter, md_leave, md_span, md_span, md_text, NULL, NULL };
    int rc = md_parse(p, n, &parser, &st);
    ray_t* r = st.err ? st.err : rc ? q_err(QE_WSFULL) : md_rows_table(&st);
    for (int64_t i = 0; i < st.nrows; i++) {
        if (st.rows[i].info) ray_release(st.rows[i].info);
        if (st.rows[i].val) ray_release(st.rows[i].val);
    }
    if (st.cells) ray_release(st.cells);
    if (st.row) ray_release(st.row);
    ray_release(st.text);
    free(st.rows);
    free(st.open);
    return r;
}

static void md_html_out(const MD_CHAR* s, MD_SIZE n, void* u) { md_put(u, s, n); }

static ray_t* md_html_fn(ray_t* x) {
    const char* p;
    MD_SIZE n;
    ray_t* e = md_arg(x, &p, &n);
    if (e) return e;
    ray_t* out = ray_charv("", 0);
    if (md_html(p, n, md_html_out, &out, MD_PARSE_FLAGS, MD_HTML_FLAG_XHTML) == 0) return out;
    ray_release(out);
    return q_err(QE_WSFULL);
}

static void md_bind(const char* nm, ray_unary_fn fn) {
    ray_t* f = ray_fn_unary(nm, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(nm, strlen(nm)), f);
    ray_release(f);
}

void q_md_register(void) {
    md_bind(".md.i.parse", md_parse_fn);
    md_bind(".md.i.html", md_html_fn);
}
