/* ops/q_str.c — the q string-verb bodies: string / upper / lower / trim
 * family (env-bound in q_builtins_register) + the search trio like / ss /
 * ssr (manifest wraps) + the shared line-splitter q_str_split_lines.
 * Evicted from q_builtins.c + ops/q_io.c so the registration hub registers
 * and the string domain lives once. */
#include "qlang/q_registry_internal.h" /* wrap decls + the string-C3 boundary decls */
#include "qlang/q_err.h"
#include "qlang/eval/q_eval.h"         /* q_eval_apply_is_fn / q_eval_apply_value — ssr fn replacement */
#include "qlang/q_builtins.h"          /* the env-fn decls (q_string_fn, ...) */
#include "qlang/q_fmt.h"               /* q_fmt_float — string's float leaf */
#include "lang/internal.h"             /* ray_str_vec_get — RAY_STR column reads */
#include "lang/format.h"               /* ray_fmt — base formatter fallback */
#include "ops/ops.h"                   /* ray_is_lazy — DAG guard in q_str_charv_out */
#include "table/sym.h"                 /* ray_sym_str — sym renders bare */
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ---- string-C3 boundary conversion (spec Design §3: physical RAY_STR never
 * appears in q-space; values in flight are charv; columns stay pooled) ---- */

/* MATERIALIZES (one O(len) memcpy), never a view: engine amend writes through
 * slices and SSO atoms (<=6 bytes, header-inline) cannot be slice parents
 * (stage-0 audit §2) — zero-copy stays a later constructor-internal option. */
ray_t* q_str_charv_of_str(ray_t* s) {
    if (!s || RAY_IS_ERR(s)) return s;               /* errors pass through (no-op rc) */
    if (s->type != -RAY_STR) return q_err(QE_TYPE);
    return ray_charv(ray_str_ptr(s), (int64_t)ray_str_len(s));
}

ray_t* q_str_of_charv(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;               /* errors pass through (no-op rc) */
    if (x->type == -RAY_CHARV) { char c = (char)x->u8; return ray_str(&c, 1); }
    if (x->type != RAY_CHARV) return q_err(QE_TYPE);
    return ray_str((const char*)ray_data(x), (size_t)ray_len(x));
}

bool q_str_text_bytes(ray_t* x, const char** p, int64_t* n) {
    if (!x || RAY_IS_ERR(x)) return false;
    if (x->type == -RAY_STR)  { *p = ray_str_ptr(x); *n = (int64_t)ray_str_len(x); return true; }
    if (x->type == RAY_CHARV) { *p = (const char*)ray_data(x); *n = ray_len(x); return true; }
    if (x->type == -RAY_CHARV){ *p = (const char*)&x->u8; *n = 1; return true; }
    return false;
}

/* Inverse adapter for the legacy string-verb bodies (vs/sv):
 * BORROWS x, returns OWNED legacy form — charv/char atom -> -RAY_STR atom;
 * LIST elements converted recursively; everything else retained as-is. */
ray_t* q_str_in(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) { if (x) ray_retain(x); return x; }
    if (x->type == RAY_CHARV || x->type == -RAY_CHARV) return q_str_of_charv(x);
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        bool any = false;
        for (int64_t i = 0; i < n && !any; i++)
            any = e[i] && (e[i]->type == RAY_CHARV || e[i]->type == -RAY_CHARV ||
                           e[i]->type == RAY_LIST);
        if (!any) { ray_retain(x); return x; }
        ray_t* out = ray_list_new(n);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        for (int64_t i = 0; i < n; i++) {
            ray_t* c = q_str_in(e[i]);
            if (RAY_IS_ERR(c)) { ray_release(out); return c; }
            out = ray_list_append(out, c);
            if (c) ray_release(c);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    ray_retain(x);
    return x;
}

/* Boundary-out walk (consumes r, returns owned): -RAY_STR atom -> charv;
 * RAY_STR vector -> 0h list of charv; LIST -> elements converted (in place at
 * rc==1, else a fresh list); DICT -> values converted (fresh dict unless
 * nothing converts, or values are a TABLE = keyed table -> untouched); TABLE
 * and everything else pass through (columns stay pooled below the boundary). */
static bool charv_out_needed(ray_t* r) {
    if (!r) return false;
    if (ray_is_lazy(r)) return false;    /* deferred DAG values: never walk */
    if (r->type == -RAY_STR || r->type == RAY_STR) return true;
    if (r->type == RAY_LIST) {
        ray_t** e = (ray_t**)ray_data(r);
        for (int64_t i = 0; i < ray_len(r); i++)
            if (charv_out_needed(e[i])) return true;
    }
    if (r->type == RAY_DICT) {
        ray_t* vals = ray_dict_vals(r);
        return vals && vals->type != RAY_TABLE && charv_out_needed(vals);
    }
    return false;
}

ray_t* q_str_charv_out(ray_t* r) {
    if (!r || RAY_IS_ERR(r)) return r;
    if (ray_is_lazy(r)) return r;        /* deferred DAG values: never walk */
    if (r->type == -RAY_STR) {
        ray_t* v = q_str_charv_of_str(r);
        ray_release(r);
        return v;
    }
    if (r->type == RAY_STR) {                    /* extracted column -> 0h list */
        int64_t n = ray_len(r);
        ray_t* out = ray_list_new(n);
        if (!out || RAY_IS_ERR(out)) { ray_release(r); return out ? out : q_err(QE_OOM); }
        for (int64_t i = 0; i < n; i++) {
            size_t sl = 0;
            const char* sp = ray_str_vec_get(r, i, &sl);
            ray_t* cv = ray_charv(sp ? sp : "", (int64_t)sl);
            if (RAY_IS_ERR(cv)) { ray_release(out); ray_release(r); return cv; }
            out = ray_list_append(out, cv);
            ray_release(cv);
            if (RAY_IS_ERR(out)) { ray_release(r); return out; }
        }
        ray_release(r);
        return out;
    }
    if (r->type == RAY_LIST && charv_out_needed(r)) {
        int64_t n = ray_len(r);
        ray_t** e = (ray_t**)ray_data(r);
        if (r->rc == 1) {                        /* sole owner: rewrite in place */
            for (int64_t i = 0; i < n; i++) {
                ray_t* c = q_str_charv_out(e[i]);    /* consumes the slot's ref */
                if (RAY_IS_ERR(c)) { e[i] = RAY_NULL_OBJ; ray_release(r); return c; }
                e[i] = c;
            }
            return r;
        }
        ray_t* out = ray_list_new(n);
        if (!out || RAY_IS_ERR(out)) { ray_release(r); return out ? out : q_err(QE_OOM); }
        for (int64_t i = 0; i < n; i++) {
            ray_retain(e[i]);
            ray_t* c = q_str_charv_out(e[i]);
            if (RAY_IS_ERR(c)) { ray_release(out); ray_release(r); return c; }
            out = ray_list_append(out, c);
            ray_release(c);
            if (RAY_IS_ERR(out)) { ray_release(r); return out; }
        }
        ray_release(r);
        return out;
    }
    if (r->type == RAY_DICT) {
        ray_t* vals = ray_dict_vals(r);          /* borrowed */
        if (vals && vals->type != RAY_TABLE && charv_out_needed(vals)) {
            ray_retain(vals);
            ray_t* nv = q_str_charv_out(vals);       /* consumes our retain */
            if (RAY_IS_ERR(nv)) { ray_release(r); return nv; }
            ray_t* keys = ray_dict_keys(r);      /* borrowed */
            ray_retain(keys);                    /* dict_new consumes both */
            ray_t* nd = ray_dict_new(keys, nv);
            ray_release(r);
            return nd;
        }
        return r;
    }
    return r;
}


/* (string x) — q cast-to-string.  ATOM: a sym renders bare (`ibm -> "ibm"),
 * a string passes through, floats take the q float->text leaf (q_fmt_float),
 * remaining atoms reuse rayfall's formatter (string 42
 * -> "42").  VECTOR / LIST: q maps string over each item, yielding a LIST of
 * strings (`string 192 168 1 23` -> ("192";"168";"1";"23")) — the base
 * formatter would instead render the whole vector as one bracketed string. */
ray_t* q_string_fn(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (RAY_IS_NULL(x)) return ray_charv("::", 2);   /* its display form
                                                      * (owner ruling 2026-07-23) */
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);        /* borrowed */
        if (!s) return q_err(QE_TYPE);
        return q_str_charv_of_str(s);              /* `ibm -> "ibm" (charv) */
    }
    if (x->type == -RAY_STR) { ray_retain(x); return x; }
    if (x->type == -RAY_CHARV)                                 /* string "a" -> ,"a" */
        return ray_charv((const char*)&x->u8, 1);
    /* NB a charv vector falls to the element-wise arm below: kdb `string
     * "cat"` -> (,"c";,"a";,"t") (ref/string.md:37-39). */
    /* Float atoms take THE q float->text leaf (q_fmt_float, \P-honouring) in
     * suffix-free mode — never rayfall's base formatter, whose ".0" padding /
     * 0Nf null are rayfall conventions, not q's.  0: Prepare Text inherits
     * this arm through ft_cell_text -> q_string_fn. */
    if (x->type == -RAY_F64 || x->type == -RAY_F32) {   /* F32 atoms store f64 */
        char tok[64];
        double v = (x->type == -RAY_F32) ? (double)(float)x->f64 : x->f64;
        q_fmt_float(v, 0, tok, sizeof tok);   /* narrow reals like display does */
        return ray_charv(tok, (int64_t)strlen(tok));
    }
    if (!ray_is_vec(x)) return q_str_charv_out(ray_fmt(x, 0));   /* remaining atoms */
    /* vector: per-element string */
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

/* upper / lower — ASCII case shift for a string atom, a symbol atom, and the
 * string/symbol VECTOR forms. */
static void str_case_bytes(const char* p, size_t n, char* b, int up) {
    for (size_t i = 0; i < n; i++)
        b[i] = (char)(up ? toupper((unsigned char)p[i]) : tolower((unsigned char)p[i]));
}
static ray_t* str_case_leaf(ray_t* x, int64_t up) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == -RAY_CHARV)                       /* char atom stays an atom */
        return ray_char((uint8_t)(up ? toupper(x->u8) : tolower(x->u8)));
    if (x->type == RAY_CHARV) {                      /* char vector, in place-of-copy */
        int64_t n = ray_len(x);
        ray_t* r = ray_charv((const char*)ray_data(x), n);
        if (RAY_IS_ERR(r)) return r;
        str_case_bytes((const char*)ray_data(r), (size_t)n, (char*)ray_data(r), up);
        return r;
    }
    if (x->type == -RAY_STR) {
        const char* p = ray_str_ptr(x);
        size_t n = ray_str_len(x);
        char stack[256];
        char* b = (n < sizeof stack) ? stack : malloc(n + 1);
        if (!b) return q_err(QE_WSFULL);
        str_case_bytes(p, n, b, up);
        ray_t* r = ray_str(b, n);
        if (b != stack) free(b);
        return r;
    }
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);   /* borrowed */
        if (!s) return q_err(QE_TYPE);
        const char* p = ray_str_ptr(s);
        size_t n = ray_str_len(s);
        char stack[256];
        char* b = (n < sizeof stack) ? stack : malloc(n + 1);
        if (!b) return q_err(QE_WSFULL);
        str_case_bytes(p, n, b, up);
        int64_t id = ray_sym_intern(b, n);
        if (b != stack) free(b);
        return ray_sym(id);
    }
    if (x->type == RAY_SYM) {   /* symbol vector */
        int64_t n = ray_len(x);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* s = ray_sym_vec_cell(x, i);   /* borrowed str */
            const char* p = s ? ray_str_ptr(s) : "";
            size_t sn = s ? ray_str_len(s) : 0;
            char stack[256];
            char* b = (sn < sizeof stack) ? stack : malloc(sn + 1);
            if (!b) { ray_release(out); return q_err(QE_WSFULL); }
            str_case_bytes(p, sn, b, up);
            int64_t id = ray_sym_intern(b, sn);
            if (b != stack) free(b);
            out = ray_vec_append(out, &id);
            if (!out || RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_STR) {   /* string vector -> per-element case shift */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            size_t sn; const char* p = ray_str_vec_get(x, i, &sn);
            char stack[256];
            char* b = (sn < sizeof stack) ? stack : malloc(sn + 1);
            if (!b) { ray_release(out); return q_err(QE_WSFULL); }
            str_case_bytes(p ? p : "", p ? sn : 0, b, up);
            ray_t* r = ray_str(b, p ? sn : 0);
            if (b != stack) free(b);
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return q_err(QE_TYPE);
}
ray_t* q_upper_fn(ray_t* x) { return str_case_leaf(x, 1); }
ray_t* q_lower_fn(ray_t* x) { return str_case_leaf(x, 0); }

/* trim / ltrim / rtrim — mode 0=both, 1=leading only, 2=trailing only.
 * A string atom strips ASCII whitespace; a simple (non-string) vector strips
 * leading/trailing NULLs (kdb's `trim 0N 0N 1 2 0N` -> 1 2); any other atom
 * passes through unchanged (`trim 42` -> 42). */
static int str_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static ray_t* str_trim_leaf(ray_t* x, int64_t mode) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == -RAY_STR) {
        const char* p = ray_str_ptr(x);
        size_t n = ray_str_len(x), a = 0, b = n;
        if (mode != 2) while (a < b && str_is_ws(p[a])) a++;
        if (mode != 1) while (b > a && str_is_ws(p[b - 1])) b--;
        return ray_str(p + a, b - a);
    }
    if (x->type == -RAY_CHARV) {                 /* char atom: ws -> "", else itself */
        if (str_is_ws((char)x->u8)) return ray_charv("", 0);
        ray_retain(x); return x;
    }
    if (x->type == RAY_CHARV) {                  /* char vector -> trimmed charv */
        const char* p = (const char*)ray_data(x);
        size_t n = (size_t)ray_len(x), a = 0, b = n;
        if (mode != 2) while (a < b && str_is_ws(p[a])) a++;
        if (mode != 1) while (b > a && str_is_ws(p[b - 1])) b--;
        return ray_charv(p + a, (int64_t)(b - a));
    }
    if (x->type == RAY_STR) {   /* string vector -> trim each element */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            size_t sn; const char* p = ray_str_vec_get(x, i, &sn);
            size_t a = 0, b = p ? sn : 0;
            if (mode != 2) while (a < b && str_is_ws(p[a])) a++;
            if (mode != 1) while (b > a && str_is_ws(p[b - 1])) b--;
            ray_t* r = ray_str(p ? p + a : "", b - a);
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (ray_is_vec(x)) {        /* simple non-string vector -> strip NULL ends */
        int64_t n = ray_len(x), a = 0, b = n;
        if (mode != 2) while (a < b && ray_vec_is_null(x, a)) a++;
        if (mode != 1) while (b > a && ray_vec_is_null(x, b - 1)) b--;
        if (a == 0 && b == n) { ray_retain(x); return x; }
        ray_t* idx = ray_vec_new(RAY_I64, b - a);
        if (RAY_IS_ERR(idx)) return idx;
        for (int64_t i = a; i < b; i++) idx = ray_vec_append(idx, &i);
        ray_t* r = ray_at_fn(x, idx);
        ray_release(idx);
        if (!r || RAY_IS_ERR(r)) return r;
        ray_t* c = q_list_collapse(r);   /* boxed slice -> typed vector display */
        ray_release(r);
        return c;
    }
    ray_retain(x);              /* atom passthrough (trim 42 -> 42) */
    return x;
}
ray_t* q_trim_fn (ray_t* x) { return str_trim_leaf(x, 0); }
ray_t* q_ltrim_fn(ray_t* x) { return str_trim_leaf(x, 1); }
ray_t* q_rtrim_fn(ray_t* x) { return str_trim_leaf(x, 2); }

/* ===== the q pattern grammar: like / ss / ssr =============================
 * basics/regex.md is the WHOLE grammar: `?` any char, `*` any sequence,
 * `[...]` alternatives with an optional leading `^` negation and 0-9/a-z/A-Z
 * ranges; inside a class `?` and `*` are literal.  q's own matcher, NOT the
 * engine's fnmatch-flavoured ray_glob_match (which spells negation `!`). */

static int str_pat_band(char c) {
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'a' && c <= 'z') return 2;
    if (c >= 'A' && c <= 'Z') return 3;
    return 0;
}

/* One `[...]` token: *pi enters at the `[` and leaves past the `]`.  A `]`
 * first in the class is a member; an unterminated class runs to the end. */
static bool str_pat_class(const char* p, size_t pn, size_t* pi, char c) {
    size_t i = *pi + 1;
    bool neg = false, hit = false, first = true;
    if (i < pn && p[i] == '^') { neg = true; i++; }
    for (; i < pn && (first || p[i] != ']'); first = false) {
        int band = str_pat_band(p[i]);
        if (i + 2 < pn && p[i + 1] == '-' && band && str_pat_band(p[i + 2]) == band &&
            p[i] <= p[i + 2]) {
            if (c >= p[i] && c <= p[i + 2]) hit = true;
            i += 3;
        } else {
            if (c == p[i]) hit = true;
            i++;
        }
    }
    if (i < pn && p[i] == ']') i++;
    *pi = i;
    return hit != neg;
}

/* One token, one input char — so a match's width is the pattern's alone. */
static size_t str_pat_width(const char* p, size_t pn) {
    size_t i = 0, w = 0;
    while (i < pn) {
        if (p[i] == '[') (void)str_pat_class(p, pn, &i, '\0');
        else i++;
        w++;
    }
    return w;
}

/* Star-free p anchored at s[pos..); *end takes the first unconsumed index. */
static bool str_pat_run(const char* s, size_t sn, size_t pos,
                        const char* p, size_t pn, size_t* end) {
    size_t si = pos, pi = 0;
    while (pi < pn) {
        if (si >= sn) return false;
        if (p[pi] == '?') { pi++; si++; }
        else if (p[pi] == '[') { if (!str_pat_class(p, pn, &pi, s[si])) return false; si++; }
        else if (p[pi] == s[si]) { pi++; si++; }
        else return false;
    }
    if (end) *end = si;
    return true;
}

/* Positions of the stars OUTSIDE any class — `[*]` is a literal star. */
static size_t str_pat_stars(const char* p, size_t pn, size_t* at, size_t max) {
    size_t i = 0, n = 0;
    while (i < pn) {
        if (p[i] == '[') { (void)str_pat_class(p, pn, &i, '\0'); continue; }
        if (p[i] == '*') { if (n < max) at[n] = i; n++; }
        i++;
    }
    return n;
}

/* Whole-string match.  q's `like` affords ONE floating segment — `A*B` (both
 * ends anchored) or `*B*` (one scan).  A second, the doc's `*the*the`, is
 * "too difficult": return false, *out unset, and the caller says 'nyi. */
static bool str_pat_like(const char* s, size_t sn, const char* p, size_t pn, bool* out) {
    size_t at[3], ns = str_pat_stars(p, pn, at, 3);
    if (ns > 2 || (ns == 2 && !(at[0] == 0 && at[1] == pn - 1))) return false;
    if (ns == 0) {
        size_t end = 0;
        *out = str_pat_run(s, sn, 0, p, pn, &end) && end == sn;
        return true;
    }
    if (ns == 2) {                                    /* *B* — B floats */
        const char* b = p + 1;
        size_t bn = pn - 2, w = str_pat_width(b, bn);
        *out = false;
        for (size_t i = 0; !*out && i + w <= sn; i++)
            *out = str_pat_run(s, sn, i, b, bn, NULL);
        return true;
    }
    {                                                 /* A*B — both anchored */
        const char *pre = p, *suf = p + at[0] + 1;
        size_t pren = at[0], sufn = pn - at[0] - 1;
        size_t pw = str_pat_width(pre, pren), sw = str_pat_width(suf, sufn), end = 0;
        *out = pw + sw <= sn && str_pat_run(s, sn, 0, pre, pren, &end) &&
               str_pat_run(s, sn, sn - sw, suf, sufn, &end) && end == sn;
        return true;
    }
}

/* ss/ssr take neither a star nor an empty pattern — both are 'length. */
static bool str_pat_fixed(const char* p, size_t pn) {
    size_t at[1];
    return pn > 0 && str_pat_stars(p, pn, at, 1) == 0;
}

/* The text of a value that IS one string — charv, char atom, or sym atom. */
static bool str_pat_text(ray_t* x, const char** p, int64_t* n) {
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);               /* borrowed */
        *p = s ? ray_str_ptr(s) : "";
        *n = s ? (int64_t)ray_str_len(s) : 0;
        return true;
    }
    return q_str_text_bytes(x, p, n);
}

static ray_t* str_like_leaf(ray_t* x, const char* pp, size_t pn) {
    const char* sp; int64_t sn; bool m;
    if (!str_pat_text(x, &sp, &sn)) return q_err(QE_TYPE);
    if (!str_pat_like(sp, (size_t)sn, pp, pn, &m)) return q_err(QE_NYI);
    return ray_bool(m);
}

/* ref/like.md "Implicit iteration": lists of strings or symbols, and dicts
 * with them as values.  No doc row reaches a table, so a table is 'type. */
ray_t* q_like_wrap(ray_t* x, ray_t* pattern) {
    const char* pp; int64_t pn;
    if (!x || !q_str_text_bytes(pattern, &pp, &pn)) return q_err(QE_TYPE);
    if (x->type == RAY_DICT) {
        ray_t* v = q_like_wrap(ray_dict_vals(x), pattern);
        if (!v || RAY_IS_ERR(v)) return v;
        ray_t* k = ray_dict_keys(x);
        ray_retain(k);
        return ray_dict_new(k, v);
    }
    if (x->type == RAY_LIST || x->type == RAY_SYM || x->type == RAY_STR) {
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* e = ray_at_fn(x, ia);
            ray_release(ia);
            if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
            ray_t* r = (x->type == RAY_LIST) ? q_like_wrap(e, pattern)
                                             : str_like_leaf(e, pp, (size_t)pn);
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        ray_t* c = q_list_collapse(out);   /* homogeneous bool run -> bool vec */
        ray_release(out);
        return c;
    }
    return str_like_leaf(x, pp, (size_t)pn);
}

/* 0-based start index of every match of p in s, overlapping (kdb-true). */
ray_t* q_ss_wrap(ray_t* s, ray_t* p) {
    const char* sp; int64_t sn64; const char* pp; int64_t pn64;
    if (!q_str_text_bytes(s, &sp, &sn64) || !q_str_text_bytes(p, &pp, &pn64))
        return q_err(QE_TYPE);
    size_t sn = (size_t)sn64, pn = (size_t)pn64;
    if (!str_pat_fixed(pp, pn)) return q_err(QE_LENGTH);
    size_t w = str_pat_width(pp, pn);
    ray_t* out = ray_vec_new(RAY_I64, 8);
    if (RAY_IS_ERR(out)) return out;
    for (size_t i = 0; i + w <= sn; i++) {
        if (str_pat_run(sp, sn, i, pp, pn, NULL)) {
            int64_t idx = (int64_t)i;
            out = ray_vec_append(out, &idx);
            if (RAY_IS_ERR(out)) return out;
        }
    }
    return out;
}

/* q `ssr[s;p;r]` — replace every (non-overlapping, left-to-right) match of the
 * pattern p in s.  r is either a replacement string, or a function
 * applied to each matched substring (kdb: `ssr[s;"t?r";upper]`). */
ray_t* q_ssr_wrap(ray_t** args, int64_t n) {
    if (n != 3) return q_err(QE_RANK);
    ray_t* s = args[0]; ray_t* p = args[1]; ray_t* r = args[2];
    const char* sp; int64_t sn64; const char* pp; int64_t pn64;
    if (!q_str_text_bytes(s, &sp, &sn64) || !q_str_text_bytes(p, &pp, &pn64))
        return q_err(QE_TYPE);
    int r_is_fn = q_eval_apply_is_fn(r);
    { const char* rp_; int64_t rn_;
      if (!r_is_fn && !q_str_text_bytes(r, &rp_, &rn_))
          return q_err(QE_TYPE); }
    size_t sn = (size_t)sn64, pn = (size_t)pn64;
    if (!str_pat_fixed(pp, pn)) return q_err(QE_LENGTH);
    size_t w = str_pat_width(pp, pn);
    size_t cap = sn + 16, blen = 0;
    char* b = (char*)malloc(cap);
    if (!b) return q_err(QE_WSFULL);
    #define SSR_PUSH(PTR, L) do { \
        size_t _l = (L); \
        if (blen + _l > cap) { cap = (blen + _l) * 2; char* nb = (char*)realloc(b, cap); \
            if (!nb) { free(b); return q_err(QE_WSFULL); } b = nb; } \
        memcpy(b + blen, (PTR), _l); blen += _l; } while (0)
    ray_t* err = NULL;
    size_t i = 0;
    while (i < sn) {
        bool m = i + w <= sn && str_pat_run(sp, sn, i, pp, pn, NULL);
        if (m) {
            if (r_is_fn) {
                ray_t* sub = ray_charv(sp + i, (int64_t)w); /* matched text, in flight */
                ray_t* one[1] = { sub };
                ray_t* rep = q_eval_apply_value(r, one, 1);
                ray_release(sub);
                if (!rep || RAY_IS_ERR(rep)) { err = rep; break; }
                { const char* qp; int64_t qn;
                  if (!q_str_text_bytes(rep, &qp, &qn)) { ray_release(rep); err = q_err(QE_TYPE); break; }
                  SSR_PUSH(qp, (size_t)qn); }
                ray_release(rep);
            } else {
                const char* rp2; int64_t rn2;
                (void)q_str_text_bytes(r, &rp2, &rn2);
                SSR_PUSH(rp2, (size_t)rn2);
            }
            i += w;                         /* advance past the match */
        } else {
            SSR_PUSH(sp + i, 1);
            i++;
        }
    }
    #undef SSR_PUSH
    if (err) { free(b); return err; }
    ray_t* out = ray_charv(b, (int64_t)blen);
    free(b);
    return out;
}

/* newline / host-line-separator split: split on '\n', strip a trailing '\r'
 * from each line, drop a single trailing empty line (kdb ` vs read-lines). */
ray_t* q_str_split_lines(const char* y, size_t yl) {
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    size_t seg = 0;
    for (size_t i = 0; i <= yl; i++) {
        if (i == yl || y[i] == '\n') {
            size_t end = i;
            if (end > seg && y[end - 1] == '\r') end--;   /* strip CR */
            ray_t* s = ray_str(y + seg, end - seg);
            out = ray_list_append(out, s); ray_release(s);
            if (RAY_IS_ERR(out)) return out;
            seg = i + 1;
            if (i == yl) break;
        }
    }
    /* drop a single trailing empty produced by a terminal '\n' */
    int64_t n = ray_len(out);
    if (n >= 1) {
        ray_t** e = (ray_t**)ray_data(out);
        if (e[n - 1]->type == -RAY_STR && ray_str_len(e[n - 1]) == 0) {
            ray_release(e[n - 1]);
            out->len = n - 1;
        }
    }
    return out;
}
