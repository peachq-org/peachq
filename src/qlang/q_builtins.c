/* q_builtins — register q's own builtins into the rayforce env.
 *
 * q reuses rayfall's eval/value/show as-is (they operate on ray_t), but needs
 * its own q-syntax `parse` (rayfall's `parse` reads lisp).  We bind names via
 * the public ray_fn_* / ray_env_bind API — never touching the frozen builtin
 * registration site — so `parse` overrides rayfall's lisp parse in the env. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_builtins.h"
#include "qlang/q_apply.h"    /* q_apply_noun — the noun-head dispatcher */
#include "qlang/q_deriv.h"    /* carrier inspectors — fn-value introspection */
#include "qlang/q_parse.h"
#include "qlang/q_http_client.h" /* .Q.c.hg / .Q.c.hp — outbound HTTP client */
#include "qlang/q_gz.h"        /* .Q.c.gz — gzip deflate/inflate seam */
#include "qlang/q_ops.h"      /* q_ops_table — .Q.ops introspection source */
#include "qlang/q_registry.h" /* q_registry_init */
#include "qlang/q_sys.h"      /* q_system_fn — the q-owned `system` verb */
#include "qlang/q_fmt.h"      /* q_console_show — show's display sink; q_float_tok — string's float leaf */
#include "lang/env.h"       /* ray_fn_unary, ray_env_bind */
#include "lang/eval.h"      /* RAY_FN_NONE */
#include "lang/format.h"    /* ray_fmt — q string cast */
#include "table/sym.h"      /* ray_sym_vec_cell */
#include "mem/sys.h"        /* ray_sys_alloc — remote-eval scratch */
#include "ops/ops.h"        /* ray_is_lazy, ray_lazy_materialize */
#include <rayforce.h>
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (parse str) — parse q SOURCE into a ray_t AST (overrides rayfall's lisp
 * parse).  q_parse needs a NUL-terminated C string; a RAY_STR atom's bytes are
 * not guaranteed terminated, so copy through a bounded scratch buffer. */
/* Exported (q_builtins.h) so the `-5!` internal-fn alias single-homes here. */
ray_t* q_parse_builtin_fn(ray_t* x) {
    const char* sp; int64_t sn;
    if (!q_text_bytes(x, &sp, &sn)) return ray_error("type", "parse expects a string");
    size_t sl = (size_t)sn;
    if (!sp) return ray_error("domain", "parse: bad source string");
    char* src = malloc(sl + 1);
    if (!src) return ray_error("wsfull", "parse: out of memory");
    memcpy(src, sp, sl);
    src[sl] = '\0';
    ray_t* ast = q_parse(src);
    free(src);
    /* handed out as DATA: empty statements become () (kdb parse ";") */
    if (ast && !RAY_IS_ERR(ast)) q_ast_fill_empty_stmts(ast);
    return ast ? ast : ray_error("parse", NULL);
}


/* (show x) — print x's q console display as a SIDE EFFECT (buffered in the q
 * console sink; the host drains it), then return generic null.  Overrides
 * rayfall's `show`, which uses the rayfall formatter (`[1 2 3]`) rather than
 * q_fmt.  qdoc/repl print nothing for the null result, so the row shows only
 * the buffered display. */
static ray_t* q_show_fn(ray_t* x) {
    q_console_show(x);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

static ray_t* q_id_table(ray_t* x) {
    int64_t nc = ray_table_ncols(x);
    ray_t* out = ray_table_new(nc);
    int64_t stack[64];
    int64_t* used = (nc <= 64) ? stack : (int64_t*)malloc((size_t)nc * sizeof(int64_t));
    if (!used) { ray_release(out); return ray_error("wsfull", ".Q.id: out of memory"); }
    for (int64_t c = 0; c < nc; c++) {
        int64_t nm = q_name_sanitize(ray_table_col_name(x, c));
        nm = q_name_dedup(nm, used, c, 1);
        used[c] = nm;
        out = ray_table_add_col(out, nm, ray_table_get_col_idx(x, c));
        if (!out || RAY_IS_ERR(out)) break;
    }
    if (used != stack) free(used);
    return out;
}

static ray_t* q_id_dict(ray_t* x) {
    ray_t* k = ray_dict_keys(x);
    ray_t* v = ray_dict_vals(x);
    if (!k || k->type != RAY_SYM)
        return ray_error("type", ".Q.id: dictionary keys must be symbols");
    int64_t n = ray_len(k);
    ray_t* nk = ray_sym_vec_new(RAY_SYM_W64, n);
    int64_t stack[64];
    int64_t* used = (n <= 64) ? stack : (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!used) { ray_release(nk); return ray_error("wsfull", ".Q.id: out of memory"); }
    for (int64_t i = 0; i < n; i++) {
        ray_t* ks = ray_sym_vec_cell(k, i);
        int64_t id = ray_sym_intern_runtime(ray_str_ptr(ks), ray_str_len(ks));
        id = q_name_sanitize(id);
        id = q_name_dedup(id, used, i, 1);
        used[i] = id;
        nk = ray_vec_append(nk, &id);
        if (!nk || RAY_IS_ERR(nk)) break;
    }
    if (used != stack) free(used);
    if (!nk || RAY_IS_ERR(nk)) return nk ? nk : ray_error("oom", NULL);
    ray_retain(v);
    return ray_dict_new(nk, v);   /* consumes nk, retained v */
}

static ray_t* q_id_fn(ray_t* x) {
    if (!x) return ray_error("type", ".Q.id: nil");
    if (x->type == -RAY_SYM) return ray_sym(q_name_sanitize(x->i64));
    if (x->type == RAY_TABLE) return q_id_table(x);
    if (x->type == RAY_DICT)  return q_id_dict(x);
    return ray_error("type", ".Q.id: expects a symbol, table, or dictionary");
}

/* ---- function-value introspection (stage 2): type / count / value ------- */

static ray_unary_fn g_base_type  = NULL;
static ray_unary_fn g_base_count = NULL;

static int q_is_fn_value(ray_t* x) {
    if (!x) return 0;
    if (q_deriv_kind_of(x) != Q_DERIV_NONE) return 1;
    return x->type == RAY_LAMBDA || x->type == RAY_UNARY ||
           x->type == RAY_BINARY || x->type == RAY_VARY;
}

/* q `type` on FUNCTION values only — 100h lambda, 104h projection, 102h
 * operator (ref/datatypes.md).  Everything else stays on the base verb; the
 * full q type map is cast/type.qcmd territory. */
static ray_t* q_type_fn(ray_t* x) {
    if (x) {
        /* Function-VALUE carriers first: they are RAY_LISTs under the hood, so
         * the raw-tag path below would wrongly report them as 0h (general
         * list). */
        switch (q_deriv_kind_of(x)) {
        case Q_DERIV_LAMBDA:  return ray_i16(100);
        case Q_DERIV_PROJ:    return ray_i16(104);
        case Q_DERIV_MONAD:   return ray_i16(102);
        case Q_DERIV_COMPOSE: return ray_i16(105);
        default: break;
        }
        if (x->type == RAY_LAMBDA) return ray_i16(100);
        if (x->type == RAY_UNARY || x->type == RAY_BINARY || x->type == RAY_VARY)
            return ray_i16(102);
        /* q's `type` IS the internal tag: openq's type tags already ARE kdb's
         * type numbers (include/rayforce.h — long 7, sym 11, list 0, table 98,
         * dict 99) and atoms are stored NEGATIVE (`-7h` long atom vs `7h` long
         * vector).  This replaces the rayfall SYMBOL the base `type` returned
         * (` `i64 `/` `I64 `/` `LIST `).  NB a native -RAY_STR string reports
         * -10h (char ATOM); kdb's char VECTOR is 10h — a documented string-model
         * divergence (ARCHITECTURE.md), pinned in cast/type-deferred.qcmd. */
        return ray_i16(x->type);
    }
    return g_base_type ? g_base_type(x)
                       : ray_error("type", "type: base verb missing");
}

/* ---- type-char map + table introspection (type-output feature) ----------- */

/* Single home for the kdb type-number -> type-char map (ref/dotq.md `.Q.ty`,
 * `meta`'s `t` column).  Exhaustive switch over the ABSOLUTE type tag; 0 for
 * tags with no char (list 0, guid-gap 3, out-of-band).  Lowercase = the element
 * char; `.Q.ty` uppercases it for a uniform list of vectors. */
static char q_type_char(int8_t tag) {
    int t = tag < 0 ? -tag : tag;                   /* int arith: |INT8_MIN|==128, no UB */
    if (t <= 0 || t >= RAY_TYPE_COUNT) return 0;    /* list 0, gap 3 & out-of-band: no char */
    switch ((ray_type_e)t) {                        /* exhaustive: a new member demands a lane */
    case RAY_LIST:      return 0;                   /* unreachable: filtered above */
    case RAY_BOOL:      return 'b';
    case RAY_GUID:      return 'g';
    case RAY_BYTE_ONLY: return 'x';
    case RAY_I16:       return 'h';
    case RAY_I32:       return 'i';
    case RAY_I64:       return 'j';
    case RAY_F32:       return 'e';
    case RAY_F64:       return 'f';
    case RAY_STR:       return 'c';   /* physical string storage: still char text */
    case RAY_CHARV:     return 'c';
    case RAY_SYM:       return 's';
    case RAY_TIMESTAMP: return 'p';
    case RAY_MONTH:     return 'm';
    case RAY_DATE:      return 'd';
    case RAY_DATETIME:  return 'z';
    case RAY_TIMESPAN:  return 'n';
    case RAY_MINUTE:    return 'u';
    case RAY_SECOND:    return 'v';
    case RAY_TIME:      return 't';
    }
    return 0;   /* SEL(20) etc.: no type char (unchanged) */
}

/* True iff x is a uniform, matrix-mappable list of simple vectors of ONE
 * element type (kdb `.Q.ty` upper-case case, e.g. `3 2#3 4 5` or
 * `("abc";"de")`).  *elt receives that element tag (RAY_STR for a list of
 * char-vector strings). */
static int q_uniform_list(ray_t* x, int8_t* elt) {
    if (!x || x->type != RAY_LIST) return 0;
    int64_t n = ray_len(x);
    if (n == 0) return 0;
    ray_t** e = (ray_t**)ray_data(x);
    int8_t t0 = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ei = e[i];
        int8_t ti;
        if (ei && ei->type == -RAY_STR)      ti = (int8_t)RAY_STR;   /* char vec */
        else if (ei && ray_is_vec(ei))       ti = (int8_t)ei->type;  /* simple vec */
        else return 0;                                               /* not uniform */
        if (i == 0) t0 = ti; else if (ti != t0) return 0;
    }
    if (elt) *elt = t0;
    return 1;
}

/* type char of one value, as `.Q.ty`/`meta` see it.  Simple vector -> lower;
 * native string (-RAY_STR) -> 'c' (INTROSPECTION shim: openq stores a char
 * vector as one string atom); uniform list of vectors -> UPPER; else blank. */
static char q_ty_char(ray_t* x) {
    int8_t elt;
    if (x && x->type == -RAY_STR) return 'c';                       /* char-vec shim */
    if (x && ray_is_vec(x))       return q_type_char((int8_t)x->type);
    if (q_uniform_list(x, &elt)) {
        char lc = q_type_char(elt);
        return (char)(lc ? (lc - 'a' + 'A') : ' ');
    }
    return ' ';
}

/* (.Q.ty x) — LOWER char for a simple vector / string, UPPER for a uniform
 * list of vectors, blank (" ") otherwise (ref/dotq.md).  Returns a 1-char
 * string (openq has no char atom; `.Q.ty each` over strings therefore yields a
 * list of 1-char strings, not one packed char vector — the `"jc JC"` combined
 * example is a deferred string-model cell). */
static ray_t* q_dotq_ty_fn(ray_t* x) {
    char c = q_ty_char(x);
    return ray_str(&c, 1);
}

/* (.Q.qt x) — is-table predicate (ref/dotq.md `qt`): 1b if x is a table
 * (simple OR keyed), else 0b. */
static ray_t* q_dotq_qt_fn(ray_t* x) {
    return ray_bool(x && (x->type == RAY_TABLE || q_is_keyed_table(x)));
}

/* (.Q.qp x) — is-partitioned predicate (ref/dotq.md `qp`): partitioned table
 * -> 1b, splayed table -> 0b, anything else -> 0 (a LONG, not a bool).  openq
 * has no on-disk partitioned or splayed tables, so nothing in memory is either
 * — every value falls into the "anything else" arm and returns the long 0.
 * The doc's own `.Q.qp select from B -> 0` pins this: an in-memory table (as
 * openq's tables always are) is NOT splayed, so it returns 0 (long), not 0b. */
static ray_t* q_dotq_qp_fn(ray_t* x) {
    (void)x;
    return ray_i64(0);
}

/* (.Q.s x) — x formatted to plain text as the console prints it (ref/dotq.md
 * `.Q.s`), returned as a q string.  SINGLE-HOMES to q_fmt_console — the same
 * DISPLAY seam `show` uses (q_console_show = q_fmt_console + '\n') — so
 * `.Q.s x` is byte-identical to what `show x` prints, INCLUDING the
 * line-terminating trailing newline, and OBEYS the `\c` console width/height
 * ("Obeys console width and height set by \c", ref/dotq.md) and `\P`
 * precision.  We do NOT reuse q_console_show here: that appends into the
 * global console sink the REPL/qdoc host drains after each eval, so routing
 * through it would inject `.Q.s`'s text into the host's output — `.Q.s` must
 * be side-effect-free and RETURN the string.
 * The renderer truncates silently into a bounded buffer, so grow until the
 * rendered length CONVERGES (a larger buffer yields no more bytes).  NB
 * `len < cap-1` is NOT a reliable fit test: the renderer stops at an element
 * boundary when the buffer fills, so a truncated render can still leave room
 * at the tail (e.g. an unclipped `.Q.s til 5000` fills only ~8190 of 8192).
 * Convergence is reliable because the output length is monotonic
 * non-decreasing in buffer size — equal length after doubling means the whole
 * display fit.  Capped to stay bounded. */
static ray_t* q_dotq_s_fn(ray_t* x) {
    size_t cap = 8192;
    char* buf = malloc(cap);
    if (!buf) return ray_error("wsfull", ".Q.s: out of memory");
    buf[0] = '\0';
    q_fmt_console(x, buf, cap);                      /* `.Q.s` OBEYS `\c` */
    size_t len = strlen(buf);
    while (cap < (1u << 24)) {                       /* grow until length settles */
        size_t ncap = cap * 2;
        char* nb = realloc(buf, ncap);
        if (!nb) { free(buf); return ray_error("wsfull", ".Q.s: out of memory"); }
        buf = nb;
        cap = ncap;
        buf[0] = '\0';
        q_fmt_console(x, buf, cap);
        size_t nlen = strlen(buf);
        if (nlen == len) break;                      /* converged: full display */
        len = nlen;
    }
    char* out = malloc(len + 2);
    if (!out) { free(buf); return ray_error("wsfull", ".Q.s: out of memory"); }
    memcpy(out, buf, len);
    out[len] = '\n';                                 /* console line terminator */
    ray_t* r = ray_str(out, len + 1);
    free(out);
    free(buf);
    return r;
}

/* Column name ids of a table as a RAY_SYM vector.  A keyed table (RAY_DICT of
 * key-table -> value-table) yields key cols ++ value cols. */
static ray_t* q_table_colnames(ray_t* x) {
    if (x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, nc > 0 ? nc : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        for (int64_t c = 0; c < nc; c++) {
            int64_t nm = ray_table_col_name(x, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        }
        return out;
    }
    if (x->type == RAY_DICT) {
        ray_t* kt = ray_dict_keys(x);      /* borrowed */
        ray_t* vt = ray_dict_vals(x);      /* borrowed */
        if (!kt || !vt || kt->type != RAY_TABLE || vt->type != RAY_TABLE)
            return ray_error("type", "cols: expects a table");
        int64_t knc = ray_table_ncols(kt), vnc = ray_table_ncols(vt);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, knc + vnc > 0 ? knc + vnc : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        for (int64_t c = 0; c < knc; c++) {
            int64_t nm = ray_table_col_name(kt, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        }
        for (int64_t c = 0; c < vnc; c++) {
            int64_t nm = ray_table_col_name(vt, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        }
        return out;
    }
    return ray_error("type", "cols: expects a table");
}

/* Resolve a by-name table operand (cols`t / meta`t): a -RAY_SYM naming a
 * global plain/keyed table resolves to it (borrowed); else x unchanged. */
static ray_t* q_bi_deref_table(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* g = ray_env_get(x->i64);                 /* borrowed */
        if (g && (g->type == RAY_TABLE || q_is_keyed_table(g))) return g;
    }
    return x;
}

/* (cols x) — column names of a table as a symbol vector. */
static ray_t* q_cols_fn(ray_t* x) {
    if (!x) return ray_error("type", "cols: nil");
    return q_table_colnames(q_bi_deref_table(x));
}

/* Flatten a plain-or-keyed table to a single plain RAY_TABLE (key cols first).
 * Returns owned (retained for a plain table). */
static ray_t* q_meta_flatten(ray_t* x) {
    if (x->type == RAY_TABLE) { ray_retain(x); return x; }
    if (x->type != RAY_DICT) return ray_error("type", "meta: expects a table");
    ray_t* kt = ray_dict_keys(x);          /* borrowed */
    ray_t* vt = ray_dict_vals(x);          /* borrowed */
    if (!kt || !vt || kt->type != RAY_TABLE || vt->type != RAY_TABLE)
        return ray_error("type", "meta: expects a table");
    int64_t knc = ray_table_ncols(kt), vnc = ray_table_ncols(vt);
    ray_t* out = ray_table_new(knc + vnc > 0 ? knc + vnc : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int64_t c = 0; c < knc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(kt, c), ray_table_get_col_idx(kt, c));
    for (int64_t c = 0; c < vnc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(vt, c), ray_table_get_col_idx(vt, c));
    return out;
}

/* (meta x) — table metadata keyed by column name.  Builds the keyed table
 * (c) -> (t; f; a): `c` column names, `t` per-column type char (via the
 * single-home map), `f`/`a` blank (foreign-keys/attributes are out of scope).
 * The result is a RAY_DICT from a 1-col key table to a 3-col value table —
 * "a keyed table is just a dictionary from one table to another" (q_fmt
 * renders it `k| v`). */
static ray_t* q_meta_fn(ray_t* x) {
    if (!x) return ray_error("type", "meta: nil");
    ray_t* flat = q_meta_flatten(q_bi_deref_table(x));
    if (!flat || RAY_IS_ERR(flat)) return flat;
    int64_t nc = ray_table_ncols(flat);
    int64_t cap = nc > 0 ? nc : 1;
    ray_t* cvec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* c: names          */
    ray_t* fvec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* f: blank per col  */
    ray_t* avec = ray_sym_vec_new(RAY_SYM_W64, cap);   /* a: blank per col  */
    char stackt[64];
    char* tbuf = (cap <= (int64_t)sizeof stackt) ? stackt : (char*)malloc((size_t)cap);
    if (!cvec || RAY_IS_ERR(cvec) || !fvec || RAY_IS_ERR(fvec) ||
        !avec || RAY_IS_ERR(avec) || !tbuf) {
        if (cvec && !RAY_IS_ERR(cvec)) ray_release(cvec);
        if (fvec && !RAY_IS_ERR(fvec)) ray_release(fvec);
        if (avec && !RAY_IS_ERR(avec)) ray_release(avec);
        if (tbuf && tbuf != stackt) free(tbuf);
        ray_release(flat);
        return ray_error("wsfull", "meta: out of memory");
    }
    int64_t blank = ray_sym_intern_runtime("", 0);
    int ok = 1;
    for (int64_t c = 0; c < nc && ok; c++) {
        int64_t nm = ray_table_col_name(flat, c);
        ray_t* col = ray_table_get_col_idx(flat, c);   /* borrowed */
        tbuf[c] = q_ty_char(col);
        cvec = ray_vec_append(cvec, &nm);
        fvec = ray_vec_append(fvec, &blank);
        avec = ray_vec_append(avec, &blank);
        if (!cvec || RAY_IS_ERR(cvec) || !fvec || RAY_IS_ERR(fvec) ||
            !avec || RAY_IS_ERR(avec)) ok = 0;
    }
    ray_release(flat);
    ray_t* tstr = ok ? ray_str(tbuf, (size_t)nc) : NULL;
    if (tbuf != stackt) free(tbuf);
    if (!ok || !tstr || RAY_IS_ERR(tstr)) {
        if (cvec && !RAY_IS_ERR(cvec)) ray_release(cvec);
        if (fvec && !RAY_IS_ERR(fvec)) ray_release(fvec);
        if (avec && !RAY_IS_ERR(avec)) ray_release(avec);
        if (tstr && !RAY_IS_ERR(tstr)) ray_release(tstr);
        return ray_error("wsfull", "meta: build failed");
    }
    /* key table: c ; value table: t f a  -> keyed table dict */
    ray_t* kt = ray_table_new(1);
    kt = ray_table_add_col(kt, ray_sym_intern("c", 1), cvec);
    ray_release(cvec);
    ray_t* vt = ray_table_new(3);
    vt = ray_table_add_col(vt, ray_sym_intern("t", 1), tstr); ray_release(tstr);
    if (!RAY_IS_ERR(vt)) { vt = ray_table_add_col(vt, ray_sym_intern("f", 1), fvec); }
    ray_release(fvec);
    if (!RAY_IS_ERR(vt)) { vt = ray_table_add_col(vt, ray_sym_intern("a", 1), avec); }
    ray_release(avec);
    if (RAY_IS_ERR(kt)) { if (!RAY_IS_ERR(vt)) ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);   /* consumes kt, vt */
}

/* q `count` of any function value is 1 (kdb: functions are atoms) — the
 * carrier is a RAY_LIST, so the base count would leak its slot count. */
ray_t* q_count_fn(ray_t* x) {
    if (q_is_fn_value(x)) return ray_i64(1);
    return g_base_count ? g_base_count(x)
                        : ray_error("type", "count: base verb missing");
}

/* C-long specialization of q_count_fn: the count as an int64 (-1 on error),
 * for callers that want the number rather than a q value.  Consumes the
 * intermediate count value. */
int64_t q_count_long(ray_t* x) {
    ray_t* c = q_count_fn(x);
    if (!c || RAY_IS_ERR(c)) { ray_release(c); return -1; }
    int64_t n = c->i64;
    ray_release(c);
    return n;
}

/* Capture a base unary's fn pointer AND attrs — a wrapper must be bound with
 * the base flags (count is RAY_FN_AGGR | RAY_FN_LAZY_AWARE; dropping them
 * would de-aggregate count in the planner and force lazy materialization —
 * codex stage-2 finding). */
static void capture_base(const char* name, ray_unary_fn* out, uint8_t* attrs) {
    ray_t* v = ray_env_get(ray_sym_intern(name, strlen(name)));
    if (v && v->type == RAY_UNARY) {
        *out = (ray_unary_fn)(uintptr_t)v->i64;
        if (attrs) *attrs = v->attrs;
    }
}

static void bind_unary(const char* name, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(name, RAY_FN_NONE, fn);
    ray_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

static void bind_vary(const char* name, ray_vary_fn fn) {
    ray_t* obj = ray_fn_vary(name, RAY_FN_NONE, fn);
    ray_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

static void bind_value(const char* name, ray_t* val) {
    ray_env_bind(ray_sym_intern(name, strlen(name)), val);
    ray_release(val);
}

/* Remote-source string eval (IPC request payloads, journal replay): the q
 * pipeline, mirroring q_repl.c run_one_line — q_parse -> q_lower ->
 * ray_eval -> materialize.  Buffered q console output (`show`, 0N!) is
 * drained to the SERVER's stdout after each eval (kdb prints show output
 * on the server console) and reset so it can't bleed into later requests.
 * Returns an OWNED value; parse/lower/eval errors return as owned errors
 * (the IPC layer serializes them as -128h responses). */
static ray_t* q_remote_eval_str(const char* src, size_t len) {
    /* A leading `\` is a system command, not q source: kdb runs a solo `\l`/`\p`
     * received on the wire.  `system"X"` is exactly `\X`, so strip the `\` and
     * reuse q_system_fn — single-homing q_sys_run, the restricted-mode guard,
     * and the `\`-shell stdout capture.  A loaded script's `show`/0N! output
     * is drained to the server console just like the q path. */
    if (len > 0 && src[0] == '\\') {
        ray_t* arg = ray_str(src + 1, len - 1);   /* the command minus its leading `\` */
        if (!arg) return ray_error("oom", "remote eval: out of memory");
        ray_t* r = q_system_fn(arg);
        ray_release(arg);
        { const char* con = q_console_str();
          if (con && *con) fputs(con, stdout);
          q_console_reset(); }
        return r;
    }
    char* tmp = (char*)ray_sys_alloc(len + 1);
    if (!tmp) return ray_error("oom", "remote eval: out of memory");
    memcpy(tmp, src, len);
    tmp[len] = '\0';
    ray_t* ast = q_parse(tmp);
    ray_sys_free(tmp);
    if (RAY_IS_ERR(ast)) return ast;
    /* A trailing assignment answers with the generic null, not the assigned
     * value (basics/ipc.md: `h"fn:{2+x}"` displays nothing).  Checked on the
     * PRE-lower shape — q_lower rewrites assignment into a `set` application —
     * which is why q_repl.c/qdoc.c ask here too. */
    const int is_assign = q_ast_is_assign(ast);
    ast = q_lower(ast);
    if (RAY_IS_ERR(ast)) return ast;
    ray_t* r = ray_eval(ast);
    ray_release(ast);
    if (ray_is_lazy(r))
        r = ray_lazy_materialize(r);
    { const char* con = q_console_str();
      if (con && *con) fputs(con, stdout);
      q_console_reset(); }
    if (is_assign && !RAY_IS_ERR(r)) {   /* an error still propagates (-128h) */
        ray_release(r);
        ray_retain(RAY_NULL_OBJ);        /* owned return (q_sys.c's silent-null precedent) */
        return RAY_NULL_OBJ;
    }
    return r;
}

/* Remote (func;args) value-apply (IPC request payloads): the kdb value/apply
 * wire shape.  Reuse the single-home q `value` (q_value_wrap), which already
 * does a SINGLE list-apply — resolving a symbol head (`sum`), a string-source
 * head (`"+"`), a value head, or a 100h lambda carrier (`{x*2}`) — and applies
 * it to the already-evaluated tail args, NEVER re-evaluating them (ADR-0004:
 * value, not eval).  `list` is BORROWED (the ipc layer releases it); q_value_wrap
 * does not consume it and returns an OWNED value.  Restricted mode needs no
 * re-assert here: ipc_dispatch sets ray_eval_get_restricted() around the whole
 * dispatch, and any restricted primitive reached by the apply (`hopen`,
 * `system`…) self-checks it — the exact contract the string hook relies on. */
static ray_t* q_remote_apply(ray_t* list) {
    return q_value_wrap(list);
}

/* ---- .Q.ops — Q_OPS[] as a read-only introspection table ------------------
 * (.Q.ops[]) materializes the verb manifest (src/qlang/q_ops.c) as a fresh
 * unkeyed table each call, so user code can query the op roster without any
 * path into the immutable registry — mutating the returned table cannot change
 * how verbs resolve.  OWNER RULING 2026-07-14: lives under `.Q` (openq
 * extension entry beside .Q.qt/.Q.qp; kdb has no .Q.ops).  Columns:
 *   name          sym   verb spelling
 *   lexclass      sym   `glyph / `kw_infix / `kw_prefix / `adverb (QLEX_*)
 *   monadic       bool  1b iff Q_OPS gives a build recipe at the monadic slot
 *                       (mon.kind != QK_NONE).  NB for the VARY prefix-keyword
 *                       verbs (ej/aj/wj/... — QR_ENV in the monadic slot) this
 *                       means "resolvable in prefix/bracket position", NOT that
 *                       the verb is strictly arity-1; true arity is not modelled
 *                       in the manifest.
 *   dyadic        bool  1b iff Q_OPS gives a build recipe at the dyadic slot
 *   deterministic bool  1b unless the verb's result is nondeterministic
 *   sideeffect    bool  1b iff evaluation performs an observable side effect
 *   family        sym   structure-dispatch family (`atomic`map`aggregate`index
 *                       `rowid`structural`irregular`none — q_ops.h vocabulary,
 *                       FAMILY AUDIT in q_ops.c; pure metadata this stage)
 * Per-verb help strings live in docs/q-ops-help.tsv (archival, not in the binary).
 * The x argument (`.Q.ops[]` passes `::`) is ignored — the table is constant
 * per build, keyed only off the static manifest. */
static const char* q_lexclass_name(q_lex_class c) {
    switch (c) {
    case QLEX_GLYPH:     return "glyph";
    case QLEX_KW_INFIX:  return "kw_infix";
    case QLEX_KW_PREFIX: return "kw_prefix";
    case QLEX_ADVERB:    return "adverb";
    }
    return "unknown";
}

static ray_t* q_dotq_ops_fn(ray_t** args, int64_t nargs) {
    (void)args; (void)nargs;                   /* `.Q.ops[]` / `.Q.ops x` — arg ignored */
    int n = 0;
    const q_op_t* ops = q_ops_table(&n);
    int64_t cap = n > 0 ? n : 1;
    ray_t* name = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t* lexc = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t* mon  = ray_vec_new(RAY_BOOL, cap);
    ray_t* dya  = ray_vec_new(RAY_BOOL, cap);
    ray_t* det  = ray_vec_new(RAY_BOOL, cap);
    ray_t* eff  = ray_vec_new(RAY_BOOL, cap);
    ray_t* fam  = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t* cols[7] = { name, lexc, mon, dya, det, eff, fam };
    for (int i = 0; i < 7; i++)
        if (!cols[i] || RAY_IS_ERR(cols[i])) {
            for (int j = 0; j < 7; j++)
                if (cols[j] && !RAY_IS_ERR(cols[j])) ray_release(cols[j]);
            return ray_error("wsfull", ".Q.ops: out of memory");
        }
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        int64_t nm  = ray_sym_intern(ops[i].name, strlen(ops[i].name));
        const char* lc = q_lexclass_name(ops[i].lex);
        int64_t lci = ray_sym_intern(lc, strlen(lc));
        uint8_t bm = ops[i].mon.kind  != QK_NONE;
        uint8_t bd = ops[i].dyad.kind != QK_NONE;
        uint8_t bt = ops[i].deterministic ? 1 : 0;
        uint8_t be = ops[i].sideeffect ? 1 : 0;
        int64_t fmi = ray_sym_intern(ops[i].family, strlen(ops[i].family));
        name = ray_vec_append(name, &nm);
        lexc = ray_vec_append(lexc, &lci);
        mon  = ray_vec_append(mon,  &bm);
        dya  = ray_vec_append(dya,  &bd);
        det  = ray_vec_append(det,  &bt);
        eff  = ray_vec_append(eff,  &be);
        fam  = ray_vec_append(fam,  &fmi);
        if (!name || RAY_IS_ERR(name) || !lexc || RAY_IS_ERR(lexc) ||
            !mon || RAY_IS_ERR(mon) || !dya || RAY_IS_ERR(dya) ||
            !det || RAY_IS_ERR(det) || !eff || RAY_IS_ERR(eff) ||
            !fam || RAY_IS_ERR(fam)) ok = 0;
    }
    ray_t* built[7] = { name, lexc, mon, dya, det, eff, fam };
    if (!ok) {
        for (int j = 0; j < 7; j++)
            if (built[j] && !RAY_IS_ERR(built[j])) ray_release(built[j]);
        return ray_error("wsfull", ".Q.ops: build failed");
    }
    static const char* colnames[7] =
        { "name", "lexclass", "monadic", "dyadic", "deterministic", "sideeffect",
          "family" };
    ray_t* t = ray_table_new(7);
    for (int i = 0; i < 7; i++) {
        if (!RAY_IS_ERR(t))               /* stop adding once errored... */
            t = ray_table_add_col(t, ray_sym_intern(colnames[i], strlen(colnames[i])), built[i]);
        ray_release(built[i]);            /* ...but always drop our ref (add_col retains) */
    }
    return t;
}

void q_builtins_register(void) {
    /* Noun-head application (indexing, dict/table lookup, 104h carriers):
     * register the q dispatcher into eval's apply hook.  q_runtime_destroy
     * clears it before the runtime dies. */
    ray_eval_set_apply_hook(q_apply_noun);
    /* Remote strings (IPC/journal) evaluate as q from now on. */
    ray_eval_set_remote_str_fn(q_remote_eval_str);
    /* Remote (func;args) value-apply (IPC): single value-object apply via q `value`. */
    ray_eval_set_remote_apply_fn(q_remote_apply);
    bind_unary("parse", q_parse_builtin_fn);
    /* q keywords with no rayfall counterpart — q-owned env bindings (same
     * mechanism as `parse`), snapshotted by the registry as QK_ENV rows. */
    bind_unary("string", q_string_fn);
    bind_unary("upper",  q_upper_fn);
    bind_unary("lower",  q_lower_fn);
    bind_unary("trim",   q_trim_fn);
    bind_unary("ltrim",  q_ltrim_fn);
    bind_unary("rtrim",  q_rtrim_fn);
    bind_vary ("ssr",    q_ssr_wrap);
    /* bracket-form joins (feat/q-joins-rebuild): triadic/quaternary prefix
     * keywords resolve through the env (the ssr precedent); implementations
     * in q_registry.c over the engine join machinery. */
    bind_vary ("ej",     q_ej_wrap);
    bind_vary ("aj",     q_aj_wrap);
    bind_vary ("aj0",    q_aj0_wrap);
    bind_vary ("ajf",    q_ajf_wrap);
    bind_vary ("ajf0",   q_ajf0_wrap);
    bind_vary ("wj",     q_wj_wrap);
    bind_vary ("wj1",    q_wj1_wrap);
    bind_unary("show",   q_show_fn);
    /* `system "…"` — q-owned string form; single-homes with the `\`-slash
     * dispatcher (q_sys.c). QK_ENV row in q_ops.c snapshots this binding. */
    bind_unary("system", q_system_fn);
    /* File Text companions (feat/q-file-text): `read0[(f;o)]` bracket calls
     * name-ref through the env (the ssr precedent — same C fn as the registry
     * row); `csv` is kdb's comma global (ref/file-text.md `csv 0: t`). */
    bind_unary("read0",  q_read0_wrap);
    bind_value("csv",    ray_str(",", 1));
    /* Table introspection — q-owned, snapshotted by the registry's QK_ENV rows
     * (q_ops.c) so the parser embeds these over the base env `meta`/(absent)
     * `cols`.  Bound BEFORE q_registry_init, like `string`/`show`. */
    bind_unary("meta",   q_meta_fn);
    bind_unary("cols",   q_cols_fn);
    /* `::` — the generic-null VALUE.  Elided call args parse as unquoted `::`
     * name-refs (`f[]` is (`f;::), cases.tsv:43); binding the value makes them
     * evaluate to RAY_NULL_OBJ instead of a 'name error — which is exactly
     * what lambda application detects as projection holes, and gives a typed
     * `::` its kdb value.  The funsql special forms are unaffected (their `::`
     * markers are never evaluated). */
    ray_retain(RAY_NULL_OBJ);
    bind_value("::", RAY_NULL_OBJ);
    /* Function-value introspection wrappers.  Bound BEFORE q_registry_init so
     * the registry's QK_ENV rows (`#` monadic = count) snapshot the WRAPPED
     * values — one home for the carrier special-cases. */
    uint8_t type_attrs = RAY_FN_NONE, count_attrs = RAY_FN_NONE;
    capture_base("type",  &g_base_type,  &type_attrs);
    capture_base("count", &g_base_count, &count_attrs);
    {
        ray_t* tv = ray_fn_unary("type", type_attrs, q_type_fn);
        ray_env_bind(ray_sym_intern("type", 4), tv);
        ray_release(tv);
        ray_t* cv = ray_fn_unary("count", count_attrs, q_count_fn);
        ray_env_bind(ray_sym_intern("count", 5), cv);
        ray_release(cv);
    }
    /* `value` — env-bind the q wrapper (the q_value_wrap manifest row) so a BARE `value`
     * used as a HOF operand (`value each (::;+;-;*;%)`) resolves to q semantics,
     * not rayfall's dict-only native `value`.  Application heads still embed the
     * registry copy; both call q_value_wrap (single home).  Bound before
     * q_registry_init like the other q-owned overrides. */
    bind_unary("value", q_value_wrap);
    /* `enlist` — q dict arm (1-row table); the `,` monadic QK_ENV snapshot
     * inherits it (bound before q_registry_init, q_value_wrap precedent). */
    bind_vary ("enlist", q_enlist_wrap_vary);
    /* Build q's verb table over the now-populated g_env (ray_lang_init has run).
     * The registry is the authoritative, immutable verb source; it snapshots
     * builtin values and must be torn down via q_runtime_destroy before the
     * runtime.  Fail fast: a missing audited builtin is a bug. */
    assert(q_registry_init() == RAY_OK);
    /* Monadic table verbs (feat/q-table-verbs) are REGISTRY wrappers with no
     * base env builtin behind them — but a bare keyword used as a HOF operand
     * (`flip each x`, `keys each ts`) resolves through the ENV (the q `value`
     * precedent), so bind the registry's own immutable value under the same
     * name.  Application heads still embed the registry copy; both paths call
     * ONE wrapper. */
    {
        static const char* tv_monads[] = { "flip", "keys", "ungroup" };
        for (size_t i = 0; i < sizeof tv_monads / sizeof *tv_monads; i++) {
            const char* nm = tv_monads[i];
            ray_t* v = q_registry_lookup_name(nm, strlen(nm), Q_MONADIC); /* borrowed */
            if (!v) fprintf(stderr, "q_builtins: registry miss for %s\n", nm);
            assert(v != NULL);
            ray_env_bind(ray_sym_intern(nm, strlen(nm)), v);             /* retains */
        }
    }
    /* .Q.c.* — the raw C primitives behind the .Q namespace (internal/unstable).
     * dotq.q delegates each public `.Q.<name>` to these (rule 6, loaded next) →
     * dotq.q is the single .Q manifest.  The value keeps its true `.Q.c.<name>`
     * identity, so `.Q.qt` displays <.Q.c.qt> (owner: show the implementation, it's
     * fine).  New C-backed .Q members (queued .Q.hg/.Q.hp) bind HERE, pre-bootstrap. */
    /* .Q.btoa/.Q.sha1 are NOT here: their C home is the `-32!`/`-33!` bang, and
     * dotq.q delegates `.Q.btoa:-32!` / `.Q.sha1:-33!` (the bang is the single
     * home).  .Q.c.atob stays C-bound — it has no bang twin. */
    static const struct { const char* name; ray_unary_fn fn; } dotq_c_unary[] = {
        { ".Q.c.id",   q_id_fn        }, { ".Q.c.ty",   q_dotq_ty_fn   },
        { ".Q.c.qt",   q_dotq_qt_fn   }, { ".Q.c.qp",   q_dotq_qp_fn   },
        { ".Q.c.s",    q_dotq_s_fn    }, { ".Q.c.atob", q_dotq_atob_fn },
        { ".Q.c.hg",   q_dotq_hg_fn   },   /* HTTP GET (ref/dotq.md) */
    };
    for (size_t i = 0; i < sizeof dotq_c_unary / sizeof *dotq_c_unary; i++)
        bind_unary(dotq_c_unary[i].name, dotq_c_unary[i].fn);
    bind_vary (".Q.c.ops", q_dotq_ops_fn);   /* niladic .Q.ops[] + unary .Q.ops x */
    bind_vary (".Q.c.hp", q_dotq_hp_fn);     /* HTTP POST [url;mime;body] (ref/dotq.md) */
    bind_vary (".Q.c.gz", q_dotq_gz_fn);     /* GZip ::/inflate/deflate (ref/dotq.md) */
    bind_value(".Q.c.res", q_name_reserved_words());
    /* .Q.pn stays UNBOUND (ref/dotq.md): `` `pn in key `.Q `` must be 0b — qStudio's safeCount relies on it. */
    /* .j JSON namespace (.j.j/.j.k/.j.jd) is defined in q — src/qlang/j.q, loaded
     * at q_runtime_create; the C homes are the `-31!`/`-29!` bangs (q_bang.c). */
}
