/* ops/q_str.c — the q string-verb bodies: string / upper / lower / trim
 * family (env-bound in q_builtins_register) + the search trio like / ss /
 * ssr (manifest wraps).  Evicted from q_builtins.c + ops/q_io.c so the
 * registration hub registers and the string domain lives once. */
#include "qlang/q_registry_internal.h" /* wrap decls + q_registry.h (q_text_bytes) */
#include "qlang/q_builtins.h"          /* the env-fn decls (q_string_fn, ...) */
#include "qlang/q_fmt.h"               /* q_float_tok — string's float leaf */
#include "lang/internal.h"             /* ray_like_fn, ray_error */
#include "lang/format.h"               /* ray_fmt — base formatter fallback */
#include "table/sym.h"                 /* ray_sym_str — sym renders bare */
#include <string.h>
#include <ctype.h>
#include <stdlib.h>


/* (string x) — q cast-to-string.  ATOM: a sym renders bare (`ibm -> "ibm"),
 * a string passes through, floats take the q float->text leaf (q_float_tok),
 * remaining atoms reuse rayfall's formatter (string 42
 * -> "42").  VECTOR / LIST: q maps string over each item, yielding a LIST of
 * strings (`string 192 168 1 23` -> ("192";"168";"1";"23")) — the base
 * formatter would instead render the whole vector as one bracketed string.
 * string is atomic through dicts and tables (ref/string.md:29) — q_str_walk. */
static ray_t* string_leaf(ray_t* x, int64_t arg) {
    (void)arg;
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);        /* borrowed */
        if (!s) return ray_error("type", "string: bad symbol");
        return q_charv_of_str(s);              /* `ibm -> "ibm" (charv) */
    }
    if (x->type == -RAY_STR) { ray_retain(x); return x; }
    if (x->type == -RAY_CHARV)                                 /* string "a" -> ,"a" */
        return ray_charv((const char*)&x->u8, 1);
    /* NB a charv vector falls to the element-wise arm below: kdb `string
     * "cat"` -> (,"c";,"a";,"t") (ref/string.md:37-39). */
    /* Float atoms take THE q float->text leaf (q_float_tok, \P-honouring) in
     * suffix-free mode — never rayfall's base formatter, whose ".0" padding /
     * 0Nf null are rayfall conventions, not q's.  0: Prepare Text inherits
     * this arm through q_ft_cell_text -> q_string_fn. */
    if (x->type == -RAY_F64 || x->type == -RAY_F32) {   /* F32 atoms store f64 */
        char tok[64];
        double v = (x->type == -RAY_F32) ? (double)(float)x->f64 : x->f64;
        q_float_tok(v, 0, tok, sizeof tok);   /* narrow reals like display does */
        return ray_charv(tok, (int64_t)strlen(tok));
    }
    if (!ray_is_vec(x)) return q_charv_out(ray_fmt(x, 0));   /* remaining atoms */
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
ray_t* q_string_fn(ray_t* x) { return q_str_walk(x, string_leaf, 0, 0); }

/* q_str_walk — the atomic-through-containers walker (contract at its decl);
 * clients: case/trim/string here, pad/Tok/cast in q_dollar.c. */
ray_t* q_str_walk(ray_t* x, q_str_leaf_fn leaf, int64_t arg, int collapse) {
    if (!x) return ray_error("type", "walk: nil");
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* e = ray_at_fn(x, ia);
            ray_release(ia);
            if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
            ray_t* r = q_str_walk(e, leaf, arg, collapse);
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        if (!collapse) return out;
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    if (x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);   /* borrowed */
        ray_t* v = ray_dict_vals(x);   /* borrowed */
        if (!k || !v) return ray_error("type", "walk: bad dict");
        ray_t* nv = q_str_walk(v, leaf, arg, collapse);
        if (!nv || RAY_IS_ERR(nv)) return nv;
        ray_retain(k);
        return ray_dict_new(k, nv);    /* consumes k + nv */
    }
    if (x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* out = ray_table_new(nc);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(x, c);   /* borrowed */
            ray_t* ncol = q_str_walk(col, leaf, arg, collapse);
            if (!ncol || RAY_IS_ERR(ncol)) { ray_release(out); return ncol; }
            out = ray_table_add_col(out, ray_table_col_name(x, c), ncol);
            ray_release(ncol);
            if (!out || RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return leaf(x, arg);
}

/* upper / lower — ASCII case shift for a string atom, a symbol atom, and the
 * string/symbol VECTOR forms; the container types recurse via q_str_walk. */
static void str_case_bytes(const char* p, size_t n, char* b, int up) {
    for (size_t i = 0; i < n; i++)
        b[i] = (char)(up ? toupper((unsigned char)p[i]) : tolower((unsigned char)p[i]));
}
static ray_t* str_case_leaf(ray_t* x, int64_t up) {
    if (!x) return ray_error("type", "%s: nil", up ? "upper" : "lower");
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
        if (!b) return ray_error("wsfull", "%s: out of memory", up ? "upper" : "lower");
        str_case_bytes(p, n, b, up);
        ray_t* r = ray_str(b, n);
        if (b != stack) free(b);
        return r;
    }
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);   /* borrowed */
        if (!s) return ray_error("type", "%s: bad symbol", up ? "upper" : "lower");
        const char* p = ray_str_ptr(s);
        size_t n = ray_str_len(s);
        char stack[256];
        char* b = (n < sizeof stack) ? stack : malloc(n + 1);
        if (!b) return ray_error("wsfull", "%s: out of memory", up ? "upper" : "lower");
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
            if (!b) { ray_release(out); return ray_error("wsfull", "%s: oom", up ? "upper" : "lower"); }
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
            if (!b) { ray_release(out); return ray_error("wsfull", "%s: oom", up ? "upper" : "lower"); }
            str_case_bytes(p ? p : "", p ? sn : 0, b, up);
            ray_t* r = ray_str(b, p ? sn : 0);
            if (b != stack) free(b);
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return ray_error("type", "%s: expects a string or symbol", up ? "upper" : "lower");
}
ray_t* q_upper_fn(ray_t* x) { return q_str_walk(x, str_case_leaf, 1, 0); }
ray_t* q_lower_fn(ray_t* x) { return q_str_walk(x, str_case_leaf, 0, 0); }

/* trim / ltrim / rtrim — mode 0=both, 1=leading only, 2=trailing only.
 * A string atom strips ASCII whitespace; a simple (non-string) vector strips
 * leading/trailing NULLs (kdb's `trim 0N 0N 1 2 0N` -> 1 2); any other atom
 * passes through unchanged (`trim 42` -> 42).  Containers recurse. */
static int str_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static ray_t* str_trim_leaf(ray_t* x, int64_t mode) {
    if (!x) return ray_error("type", "trim: nil");
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
        ray_t* c = q_collapse_list(r);   /* boxed slice -> typed vector display */
        ray_release(r);
        return c;
    }
    ray_retain(x);              /* atom passthrough (trim 42 -> 42) */
    return x;
}
ray_t* q_trim_fn (ray_t* x) { return q_str_walk(x, str_trim_leaf, 0, 0); }
ray_t* q_ltrim_fn(ray_t* x) { return q_str_walk(x, str_trim_leaf, 1, 0); }
ray_t* q_rtrim_fn(ray_t* x) { return q_str_walk(x, str_trim_leaf, 2, 0); }

/* ===== string search family: like / ss / ssr (feat/q-string-fns) =========== */

/* q `x like p` — glob match.  Reuses ray_like_fn for sym/str atoms and
 * vectors; a DICT maps the match over its values (kdb: keys kept). */
ray_t* q_like_wrap(ray_t* x, ray_t* pattern) {
    if (x && x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);   /* borrowed */
        ray_t* v = ray_dict_vals(x);   /* borrowed */
        if (!k || !v) return ray_error("type", "like: bad dict");
        ray_t* nv = q_like_wrap(v, pattern);
        if (!nv || RAY_IS_ERR(nv)) return nv;
        ray_retain(k);
        return ray_dict_new(k, nv);    /* consumes k + nv */
    }
    if (x && x->type == RAY_LIST) {    /* boxed list (e.g. dict values) */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* e = ray_at_fn(x, ia);
            ray_release(ia);
            if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
            ray_t* r = q_like_wrap(e, pattern);
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        ray_t* c = q_collapse_list(out);   /* homogeneous bool run -> bool vec */
        ray_release(out);
        return c;
    }
    { /* charv leaf/pattern -> legacy -RAY_STR forms for the engine matcher */
        ray_t* xs = q_str_in(x); ray_t* ps = q_str_in(pattern);
        ray_t* r = ray_like_fn(xs, ps);
        ray_release(xs); ray_release(ps);
        return r;
    }
}

/* Match glob pattern p[0..pn) anchored at s[pos..sn), where every pattern
 * token consumes exactly one input char: `?` (any), `[..]`/`[^..]` (class,
 * optional negation), or a literal.  The variable-width `*` form is not used
 * by q's ss/ssr, so a bare `*` is treated literally.  Returns the number of
 * input chars consumed on a full match, or -1 on mismatch. */
static int64_t str_glob_fixed_at(const char* s, size_t sn, size_t pos,
                               const char* p, size_t pn) {
    size_t si = pos, pi = 0;
    while (pi < pn) {
        if (si >= sn) return -1;
        char pc = p[pi];
        if (pc == '?') { pi++; si++; continue; }
        if (pc == '[') {
            size_t j = pi + 1;
            int neg = 0;
            if (j < pn && p[j] == '^') { neg = 1; j++; }
            int matched = 0;
            for (; j < pn && p[j] != ']'; j++)
                if (p[j] == s[si]) matched = 1;
            if (j < pn) j++;              /* skip ']' */
            if (matched == neg) return -1;
            pi = j; si++; continue;
        }
        if (pc != s[si]) return -1;       /* literal */
        pi++; si++;
    }
    return (int64_t)(si - pos);
}

/* q `s ss p` — string search: 0-based start index of every match of the glob
 * pattern p in the string s (overlapping, kdb-true).  Returns a long vector. */
ray_t* q_ss_wrap(ray_t* s, ray_t* p) {
    const char* sp; int64_t sn64; const char* pp; int64_t pn64;
    if (!q_text_bytes(s, &sp, &sn64) || !q_text_bytes(p, &pp, &pn64))
        return ray_error("type", "ss: expects string arguments");
    size_t sn = (size_t)sn64, pn = (size_t)pn64;
    ray_t* out = ray_vec_new(RAY_I64, 8);
    if (RAY_IS_ERR(out)) return out;
    if (pn == 0) return out;                       /* empty pattern -> no hits */
    /* str_glob_fixed_at anchors at each position; the match WIDTH differs from
     * the pattern's byte length (a `[..]` class is 1 input char), so iterate
     * every start position and let the matcher enforce bounds. */
    for (size_t i = 0; i < sn; i++) {
        if (str_glob_fixed_at(sp, sn, i, pp, pn) >= 0) {
            int64_t idx = (int64_t)i;
            out = ray_vec_append(out, &idx);
            if (RAY_IS_ERR(out)) return out;
        }
    }
    return out;
}

/* q `ssr[s;p;r]` — replace every (non-overlapping, left-to-right) match of the
 * glob pattern p in s.  r is either a replacement string, or a function
 * applied to each matched substring (kdb: `ssr[s;"t?r";upper]`). */
ray_t* q_ssr_wrap(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("rank", "ssr: expects 3 args");
    ray_t* s = args[0]; ray_t* p = args[1]; ray_t* r = args[2];
    const char* sp; int64_t sn64; const char* pp; int64_t pn64;
    if (!q_text_bytes(s, &sp, &sn64) || !q_text_bytes(p, &pp, &pn64))
        return ray_error("type", "ssr: s and p must be strings");
    int r_is_fn = q_is_fn_value(r);
    { const char* rp_; int64_t rn_;
      if (!r_is_fn && !q_text_bytes(r, &rp_, &rn_))
          return ray_error("type", "ssr: replacement must be a string or function"); }
    size_t sn = (size_t)sn64, pn = (size_t)pn64;
    size_t cap = sn + 16, blen = 0;
    char* b = (char*)malloc(cap);
    if (!b) return ray_error("wsfull", "ssr: out of memory");
    #define SSR_PUSH(PTR, L) do { \
        size_t _l = (L); \
        if (blen + _l > cap) { cap = (blen + _l) * 2; char* nb = (char*)realloc(b, cap); \
            if (!nb) { free(b); return ray_error("wsfull", "ssr: out of memory"); } b = nb; } \
        memcpy(b + blen, (PTR), _l); blen += _l; } while (0)
    ray_t* err = NULL;
    size_t i = 0;
    while (i < sn) {
        /* str_glob_fixed_at bounds-checks internally (si>=sn -> -1) and a `[..]`
         * class is multiple pattern bytes but ONE input char, so do NOT gate on
         * `i + pn <= sn` (that rejected bracket-class matches near the end). */
        int64_t m = pn ? str_glob_fixed_at(sp, sn, i, pp, pn) : -1;
        if (m >= 0) {
            if (r_is_fn) {
                ray_t* sub = ray_charv(sp + i, m);      /* matched text, in flight */
                ray_t* one[1] = { sub };
                ray_t* rep = q_call_n(r, one, 1);
                ray_release(sub);
                if (!rep || RAY_IS_ERR(rep)) { err = rep; break; }
                { const char* qp; int64_t qn;
                  if (!q_text_bytes(rep, &qp, &qn)) { ray_release(rep); err = ray_error("type", "ssr: replacement fn must return a string"); break; }
                  SSR_PUSH(qp, (size_t)qn); }
                ray_release(rep);
            } else {
                const char* rp2; int64_t rn2;
                (void)q_text_bytes(r, &rp2, &rn2);
                SSR_PUSH(rp2, (size_t)rn2);
            }
            i += (m > 0) ? (size_t)m : 1;   /* advance past the match */
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

/* q `getenv x` (ref/getenv.md) — x is a SYMBOL atom naming an environment
 * variable; returns its value as a string, or "" when the variable is unset
 * (kdb-true, and exactly what the base ray_getenv_fn already returns for a
 * missing var).  The base primitive wants a -RAY_STR arg, so coerce the
 * symbol's name to a string atom first — the ONLY divergence from the raw C,
 * hence a wrapper rather than a QK_ENV rename.
 * String-model seam: the result is a native -RAY_STR atom, so `type getenv`X`
 * is -10h where kdb's char vector is 10h (a known, tracked divergence). */
ray_t* q_getenv_wrap(ray_t* x) {
    /* .os.getenv is RAY_FN_RESTRICTED; calling the C fn directly bypasses the
     * eval-layer check, so re-assert it here (the q_hopen_wrap/file precedent). */
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    if (!x || x->type != -RAY_SYM)
        return ray_error("type", "getenv: expected a symbol, got %s",
                         ray_type_name(x ? x->type : 0));
    ray_t* s = ray_sym_str(x->i64);                     /* borrowed */
    if (!s) return ray_error("type", "getenv: bad symbol");
    ray_t* name = ray_str(ray_str_ptr(s), ray_str_len(s));  /* owned -RAY_STR */
    if (!name || RAY_IS_ERR(name)) return name ? name : ray_error("oom", NULL);
    ray_t* r = ray_getenv_fn(name);                     /* "" when unset */
    ray_release(name);
    return q_charv_out(r);
}

