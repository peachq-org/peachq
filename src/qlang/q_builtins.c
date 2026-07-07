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
#include "qlang/q_json.h"     /* q_json_register — .j JSON namespace */
#include "qlang/q_registry.h" /* q_registry_init */
#include "qlang/q_fmt.h"      /* q_console_show — show's display sink */
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
static ray_t* q_parse_builtin_fn(ray_t* x) {
    if (!x || x->type != -RAY_STR) return ray_error("type", "parse expects a string");
    const char* sp = ray_str_ptr(x);
    size_t sl = ray_str_len(x);
    if (!sp) return ray_error("domain", "parse: bad source string");
    char* src = malloc(sl + 1);
    if (!src) return ray_error("wsfull", "parse: out of memory");
    memcpy(src, sp, sl);
    src[sl] = '\0';
    ray_t* ast = q_parse(src);
    free(src);
    return ast ? ast : ray_error("parse", NULL);
}

/* (string x) — q cast-to-string.  ATOM: a sym renders bare (`ibm -> "ibm"),
 * a string passes through, other atoms reuse rayfall's formatter (string 42
 * -> "42").  VECTOR / LIST: q maps string over each item, yielding a LIST of
 * strings (`string 192 168 1 23` -> ("192";"168";"1";"23")) — the base
 * formatter would instead render the whole vector as one bracketed string. */
static ray_t* q_string_fn(ray_t* x) {
    if (!x) return ray_error("type", "string: nil");
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);        /* borrowed */
        if (!s) return ray_error("type", "string: bad symbol");
        ray_retain(s);
        return s;
    }
    if (x->type == -RAY_STR) { ray_retain(x); return x; }
    /* Only a simple VECTOR or a boxed LIST maps element-wise; atoms and
     * whole-value containers (tables, dicts, and anything else) keep the base
     * formatter — indexing a table by 0..ncols would fabricate junk rows. */
    if (!ray_is_vec(x) && x->type != RAY_LIST) return ray_fmt(x, 0);
    /* vector or boxed list: per-element string */
    int64_t n = ray_len(x);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(x, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
        ray_t* s = q_string_fn(e);
        ray_release(e);
        if (!s || RAY_IS_ERR(s)) { ray_release(out); return s; }
        out = ray_list_append(out, s);
        ray_release(s);
        if (RAY_IS_ERR(out)) return out;
    }
    return out;
}

/* upper / lower — whole-string per-char ASCII transforms (spec piece 2's
 * in-scope string verbs; char-VECTOR semantics wait on the string model). */
static ray_t* q_case_fn(ray_t* x, int up) {
    if (!x || x->type != -RAY_STR)
        return ray_error("type", "%s: expects a string", up ? "upper" : "lower");
    const char* p = ray_str_ptr(x);
    size_t n = ray_str_len(x);
    char stack[256];
    char* b = (n < sizeof stack) ? stack : malloc(n + 1);
    if (!b) return ray_error("wsfull", "%s: out of memory", up ? "upper" : "lower");
    for (size_t i = 0; i < n; i++)
        b[i] = (char)(up ? toupper((unsigned char)p[i]) : tolower((unsigned char)p[i]));
    ray_t* r = ray_str(b, n);
    if (b != stack) free(b);
    return r;
}
static ray_t* q_upper_fn(ray_t* x) { return q_case_fn(x, 1); }
static ray_t* q_lower_fn(ray_t* x) { return q_case_fn(x, 0); }

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
 * `meta`'s `t` column).  Indexed by the ABSOLUTE type tag; 0 for tags with no
 * char (list 0, guid-gap 3, tables/dicts 98/99).  Lowercase = the element
 * char; `.Q.ty` uppercases it for a uniform list of vectors. */
static char q_type_char(int8_t tag) {
    static const char m[20] = {
        0,   'b', 'g', 0,   'x', 'h', 'i', 'j', 'e', 'f',
        'c', 's', 'p', 'm', 'd', 'z', 'n', 'u', 'v', 't'
    };
    int t = tag < 0 ? -tag : tag;
    return (t >= 0 && t < 20) ? m[t] : 0;
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

/* (cols x) — column names of a table as a symbol vector. */
static ray_t* q_cols_fn(ray_t* x) {
    if (!x) return ray_error("type", "cols: nil");
    return q_table_colnames(x);
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
    ray_t* flat = q_meta_flatten(x);
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
static ray_t* q_count_fn(ray_t* x) {
    if (q_is_fn_value(x)) return ray_i64(1);
    return g_base_count ? g_base_count(x)
                        : ray_error("type", "count: base verb missing");
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
    char* tmp = (char*)ray_sys_alloc(len + 1);
    if (!tmp) return ray_error("oom", "remote eval: out of memory");
    memcpy(tmp, src, len);
    tmp[len] = '\0';
    ray_t* ast = q_parse(tmp);
    ray_sys_free(tmp);
    if (RAY_IS_ERR(ast)) return ast;
    ast = q_lower(ast);
    if (RAY_IS_ERR(ast)) return ast;
    ray_t* r = ray_eval(ast);
    ray_release(ast);
    if (ray_is_lazy(r))
        r = ray_lazy_materialize(r);
    { const char* con = q_console_str();
      if (con && *con) fputs(con, stdout);
      q_console_reset(); }
    return r;
}

void q_builtins_register(void) {
    /* Noun-head application (indexing, dict/table lookup, 104h carriers):
     * register the q dispatcher into eval's apply hook.  q_runtime_destroy
     * clears it before the runtime dies. */
    ray_eval_set_apply_hook(q_apply_noun);
    /* Remote strings (IPC/journal) evaluate as q from now on. */
    ray_eval_set_remote_str_fn(q_remote_eval_str);
    bind_unary("parse", q_parse_builtin_fn);
    /* q keywords with no rayfall counterpart — q-owned env bindings (same
     * mechanism as `parse`), snapshotted by the registry as QK_ENV rows. */
    bind_unary("string", q_string_fn);
    bind_unary("upper",  q_upper_fn);
    bind_unary("lower",  q_lower_fn);
    bind_unary("show",   q_show_fn);
    /* Table introspection — q-owned, snapshotted by the registry's QK_ENV rows
     * (q_ops.c) so the parser embeds these over the base env `meta`/(absent)
     * `cols`.  Bound BEFORE q_registry_init, like `string`/`show`. */
    bind_unary("meta",   q_meta_fn);
    bind_unary("cols",   q_cols_fn);
    bind_value(".Q.an",  ray_str("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", 63));
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
    /* NB `value` is a QK_VALUE manifest row — its lambda/string arms live in
     * the registry wrapper (q_value_wrap), the single home the parser embeds. */
    /* Build q's verb table over the now-populated g_env (ray_lang_init has run).
     * The registry is the authoritative, immutable verb source; it snapshots
     * builtin values and must be torn down via q_runtime_destroy before the
     * runtime.  Fail fast: a missing audited builtin is a bug. */
    assert(q_registry_init() == RAY_OK);
    bind_unary(".Q.id", q_id_fn);
    bind_unary(".Q.ty", q_dotq_ty_fn);
    bind_value(".Q.res", q_name_reserved_words());
    /* .j JSON namespace (.j.j / .j.k / .j.jd) — plain env unaries, same as the
     * .Q.* bindings above; resolved as dotted name-refs, applied via q_apply_noun. */
    q_json_register();
}
