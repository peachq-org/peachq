/* q_strfmt — the q side of `.str.printf` / `.str.format`.  ALL q knowledge is
 * here; the C++ shim beyond q_strfmt_abi.h sees four lanes and a byte string.
 *
 * The scanner rewrites the format string in one pass: every replacement field
 * comes out carrying an EXPLICIT index, `*` and `{}` width/precision come out as
 * literal digits, and the argument vector gets one entry per OCCURRENCE — which
 * is why one q value can travel the numeric lane at `%1$d` and the string lane
 * at `%1$r`.  fmt does no argument stepping of its own by the time it runs. */
#include "qlang/ops/q_strfmt.h"
#include "qlang/io/q_strfmt_abi.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"
#include "qlang/q_env.h"
#include "qlang/q_fmt.h"
#include "qlang/q_prim.h"
#include "lang/env.h" /* ray_fn_unary */
#include "lang/eval.h" /* RAY_FN_NONE */
#include <rayforce.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { SF_KEEP = 0, SF_AS_STRING = 1, SF_AS_NULL = 2 };
enum { SF_ERR_BAD = 1, SF_ERR_OOM = 2 };
#define SF_NUM_MAX 1000000 /* a width nobody means; also the overflow gate */

typedef struct {
    char*   p;
    int64_t n, cap;
    int     oom;
} sfbuf;

typedef struct {
    ray_t*     msg;
    int64_t    argn;
    int64_t    next; /* automatic-index cursor */
    int        saw_auto, saw_manual;
    sfbuf      out;
    pqfmt_arg* av;
    ray_t**    own; /* per-occurrence rendered text, owned */
    int        an, acap;
    int        bad;
} sfscan;

static void sf_put(sfbuf* b, const char* s, int64_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        int64_t cap = b->cap ? b->cap : 128;
        while (cap < b->n + n + 1) cap *= 2;
        char* q = realloc(b->p, (size_t)cap);
        if (!q) { b->oom = 1; return; }
        b->p   = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, (size_t)n);
    b->n += n;
}

static void sf_ch(sfbuf* b, char c) { sf_put(b, &c, 1); }

/* strchr matches the terminator, so an embedded NUL would read as a flag. */
static int sf_in(const char* set, char c) { return c != '\0' && strchr(set, c) != NULL; }

static void sf_num(sfbuf* b, int64_t v) {
    char t[24];
    int  k = 0, neg = v < 0;
    uint64_t u = neg ? -(uint64_t)v : (uint64_t)v;
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) sf_ch(b, '-');
    while (k) sf_ch(b, t[--k]);
}

/* A wrapped index or width would format the WRONG argument, so digits are gated. */
static int64_t sf_dec(sfscan* s, const char* f, int64_t i, int64_t j) {
    int64_t v = 0;
    for (; i < j; i++) {
        v = v * 10 + (f[i] - '0');
        if (v > SF_NUM_MAX) { s->bad = SF_ERR_BAD; return 0; }
    }
    return v;
}

static ray_t* sf_item(sfscan* s, int64_t i) { return ray_list_get(s->msg, i + 1); }

/* exp: 0-based explicit index, -1 = automatic.  Mixing is fmt's own error, which
 * the rewrite hides from fmt — so it is caught here instead. */
static int64_t sf_pick(sfscan* s, int64_t exp) {
    int64_t k;
    if (exp >= 0) { s->saw_manual = 1; k = exp; }
    else { s->saw_auto = 1; k = s->next++; }
    if ((s->saw_manual && s->saw_auto) || k < 0 || k >= s->argn) { s->bad = SF_ERR_BAD; return 0; }
    return k;
}

/* Temporals, guids and BYTES fall to the string lane deliberately: fmt has no
 * lane for any of them, and their q display is the text a reader wants. */
static int sf_type_lane(ray_t* v) {
    if (!v || v->type >= 0) return PQFMT_LANE_STR;
    if (q_type_is_bool(v)) return PQFMT_LANE_BOOL;
    if (q_type_is_float_tag(v->type)) return PQFMT_LANE_F64;
    if (q_type_is_int_atom(v)) return PQFMT_LANE_I64;
    return PQFMT_LANE_STR;
}

/* The lane a conversion NAMES: fmt refuses an int under %f, KX's "j"/"f" casts don't. */
static int sf_conv_lane(char c) {
    switch (c) {
    case 'a': case 'A': case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case '%':
        return PQFMT_LANE_F64;
    case 'b': case 'B': case 'c': case 'd': case 'i': case 'n': case 'o': case 'u': case 'x': case 'X':
        return PQFMT_LANE_I64;
    default: return -1;
    }
}

/* The strict lanes own every read but bool (no int-index width) and the float
 * round, which is `"j"$`'s half-to-even — the cast KX documents for %d. */
static int64_t sf_i64(ray_t* v) {
    int64_t r;
    double  d;
    if (q_type_strict_i64(v, &r)) return r;
    if (q_type_strict_f64(v, &d)) return (int64_t)llrint(d);
    return q_type_as_i64(v);
}

static double sf_f64(ray_t* v) {
    double d;
    return q_type_strict_f64(v, &d) ? d : (double)q_type_as_i64(v);
}

static ray_t* sf_render(ray_t* v) {
    const char* p;
    int64_t     n;
    if (q_str_subject_bytes(v, &p, &n)) return ray_charv(p, n);
    return q_fmt_krepr_charv(v);
}

static int64_t sf_push(sfscan* s, char conv, int64_t qi, int* rewrite) {
    ray_t* v = sf_item(s, qi);
    if (s->an == s->acap) {
        int        cap = s->acap ? s->acap * 2 : 8;
        pqfmt_arg* na  = realloc(s->av, (size_t)cap * sizeof *na);
        ray_t**    no  = realloc(s->own, (size_t)cap * sizeof *no);
        if (na) s->av = na;
        if (no) s->own = no;
        if (!na || !no) { s->bad = SF_ERR_OOM; return 0; }
        s->acap = cap;
    }
    int64_t k = s->an++;
    s->own[k] = NULL;

    int nat  = sf_type_lane(v);
    int lane = nat;
    *rewrite = SF_KEEP;
    if (conv == 'r' || conv == 's') {
        lane     = PQFMT_LANE_STR;
        *rewrite = conv == 'r' ? SF_AS_STRING : SF_KEEP;
    } else if ((nat == PQFMT_LANE_I64 || nat == PQFMT_LANE_F64) && (RAY_ATOM_IS_NULL(v) || q_type_is_inf(v))) {
        lane     = PQFMT_LANE_STR;
        *rewrite = SF_AS_NULL;
    } else if (nat != PQFMT_LANE_STR) {
        int want = sf_conv_lane(conv);
        if (want >= 0) lane = want;
    }

    s->av[k].lane = lane;
    switch (lane) {
    case PQFMT_LANE_BOOL: s->av[k].v.i = q_type_as_i64(v) != 0; break;
    case PQFMT_LANE_I64: s->av[k].v.i = sf_i64(v); break;
    case PQFMT_LANE_F64: s->av[k].v.f = sf_f64(v); break;
    default: {
        ray_t* t = sf_render(v);
        if (!t || RAY_IS_ERR(t)) { ray_release(t); s->bad = SF_ERR_OOM; return 0; }
        s->own[k]     = t;
        s->av[k].v.s.p = (const char*)ray_data(t);
        s->av[k].v.s.n = t->len;
        break;
    }
    }
    return k;
}

static int64_t sf_star(sfscan* s, const char* f, int64_t n, int64_t* ip) {
    int64_t exp = -1, i = *ip, j = i;
    while (j < n && f[j] >= '0' && f[j] <= '9') j++;
    if (j > i && j < n && f[j] == '$') {
        int64_t d = sf_dec(s, f, i, j);
        if (d < 1) { s->bad = SF_ERR_BAD; return 0; }
        exp = d - 1;
        *ip = j + 1;
    }
    int64_t qi = sf_pick(s, exp);
    if (s->bad) return 0;
    ray_t* v = sf_item(s, qi);
    if (sf_type_lane(v) == PQFMT_LANE_STR || RAY_ATOM_IS_NULL(v)) { s->bad = SF_ERR_BAD; return 0; }
    return sf_i64(v);
}

static void sf_scan_printf(sfscan* s, const char* f, int64_t n) {
    for (int64_t i = 0; i < n && !s->bad;) {
        char c = f[i++];
        if (c != '%') { sf_ch(&s->out, c); continue; }
        if (i < n && f[i] == '%') { sf_put(&s->out, "%%", 2); i++; continue; }

        int64_t exp = -1, j = i;
        while (j < n && f[j] >= '0' && f[j] <= '9') j++;
        if (j > i && j < n && f[j] == '$') {
            int64_t d = sf_dec(s, f, i, j);
            if (d < 1) { s->bad = SF_ERR_BAD; return; }
            exp = d - 1;
            i   = j + 1;
        }

        int64_t fl0 = i;
        while (i < n && sf_in("-+ 0#,'_", f[i])) i++;
        int64_t fl1 = i;

        int     havew = 0;
        int64_t w     = 0;
        if (i < n && f[i] == '*') { havew = 1; i++; w = sf_star(s, f, n, &i); }
        else {
            j = i;
            while (i < n && f[i] >= '0' && f[i] <= '9') i++;
            if (i > j) { havew = 1; w = sf_dec(s, f, j, i); }
        }

        int     dot = 0, pstar = 0, havep = 0;
        int64_t p   = 0;
        if (i < n && f[i] == '.') {
            dot = 1;
            i++;
            if (i < n && f[i] == '*') { pstar = havep = 1; i++; p = sf_star(s, f, n, &i); }
            else {
                j = i;
                while (i < n && f[i] >= '0' && f[i] <= '9') i++;
                if (i > j) { havep = 1; p = sf_dec(s, f, j, i); }
            }
        }
        while (i < n && sf_in("hljztL", f[i])) i++; /* q has one integer and one float width */
        if (i >= n || s->bad) { s->bad = SF_ERR_BAD; return; }
        char conv = f[i++];

        int64_t qi = sf_pick(s, exp);
        if (s->bad) return;
        int     rw = SF_KEEP;
        int64_t k  = sf_push(s, conv, qi, &rw);
        if (s->bad) return;

        sf_ch(&s->out, '%');
        sf_num(&s->out, k + 1);
        sf_ch(&s->out, '$');
        if (rw == SF_AS_NULL) {
            for (int64_t t = fl0; t < fl1; t++)
                if (f[t] == '-') sf_ch(&s->out, '-');
        } else {
            sf_put(&s->out, f + fl0, fl1 - fl0);
        }
        /* C: a negative * width IS left-align-with-|width| once the digits follow
         * the flags; a negative * precision is "as if omitted", and must not leave
         * the bare `.` that groups here. */
        if (havew) sf_num(&s->out, w);
        if (dot && rw != SF_AS_NULL && !(pstar && p < 0)) {
            sf_ch(&s->out, '.');
            if (havep) sf_num(&s->out, p);
        }
        sf_ch(&s->out, rw == SF_KEEP ? conv : 's');
    }
}

static int64_t sf_nested(sfscan* s, const char* f, int64_t n, int64_t* ip) {
    int64_t i = *ip + 1, j = i, exp = -1;
    while (j < n && f[j] >= '0' && f[j] <= '9') j++;
    if (j > i) exp = sf_dec(s, f, i, j);
    if (j >= n || f[j] != '}') { s->bad = SF_ERR_BAD; return 0; }
    *ip = j + 1;
    int64_t qi = sf_pick(s, exp);
    if (s->bad) return 0;
    ray_t*  v = sf_item(s, qi);
    int64_t d;
    if (sf_type_lane(v) == PQFMT_LANE_STR || RAY_ATOM_IS_NULL(v)) { s->bad = SF_ERR_BAD; return 0; }
    d = sf_i64(v);
    if (d < 0) { s->bad = SF_ERR_BAD; return 0; }  /* fmt refuses a negative {} width */
    return d;
}

static void sf_scan_format(sfscan* s, const char* f, int64_t n) {
    for (int64_t i = 0; i < n && !s->bad;) {
        char c = f[i++];
        if (c == '}') {
            if (i < n && f[i] == '}') { sf_put(&s->out, "}}", 2); i++; continue; }
            s->bad = SF_ERR_BAD;
            return;
        }
        if (c != '{') { sf_ch(&s->out, c); continue; }
        if (i < n && f[i] == '{') { sf_put(&s->out, "{{", 2); i++; continue; }

        int64_t exp = -1, j = i;
        while (j < n && f[j] >= '0' && f[j] <= '9') j++;
        if (j > i) { exp = sf_dec(s, f, i, j); i = j; }
        if (i >= n || (f[i] != ':' && f[i] != '}')) { s->bad = SF_ERR_BAD; return; }
        /* fmt binds the field's own index BEFORE its nested width/precision */
        int64_t qi = sf_pick(s, exp);
        if (s->bad) return;

        char    fill = 0, align = 0, sgn = 0, tsep = 0;
        int     hash = 0, zero = 0, dot = 0;
        int64_t w = -1, p = -1;
        char    type = 0;
        if (f[i] == ':') {
            i++;
            if (i + 1 < n && sf_in("<>^=", f[i + 1])) { fill = f[i]; align = f[i + 1]; i += 2; }
            else if (i < n && sf_in("<>^=", f[i])) { align = f[i]; i++; }
            if (i < n && sf_in("+- ,_'", f[i])) { sgn = f[i]; i++; }
            else if (i + 1 < n && f[i] == 't') { sgn = 't'; tsep = f[i + 1]; i += 2; }
            if (i < n && f[i] == '#') { hash = 1; i++; }
            if (i < n && f[i] == '0') { zero = 1; i++; }
            if (i < n && f[i] == '{') w = sf_nested(s, f, n, &i);
            else {
                j = i;
                while (i < n && f[i] >= '0' && f[i] <= '9') i++;
                if (i > j) w = sf_dec(s, f, j, i);
            }
            if (i < n && f[i] == '.') {
                dot = 1;
                i++;
                if (i < n && f[i] == '{') p = sf_nested(s, f, n, &i);
                else {
                    j = i;
                    while (i < n && f[i] >= '0' && f[i] <= '9') i++;
                    if (i > j) p = sf_dec(s, f, j, i);
                }
            }
            if (i < n && f[i] != '}') type = f[i++];
        }
        if (i >= n || f[i] != '}' || s->bad) { s->bad = SF_ERR_BAD; return; }
        i++;

        int     rw = SF_KEEP;
        int64_t k  = sf_push(s, type, qi, &rw);   /* type 0 = a bare {}: the value's own lane */
        if (s->bad) return;

        sf_ch(&s->out, '{');
        sf_num(&s->out, k);
        int null_rw = rw == SF_AS_NULL;
        if (align || (!null_rw && (sgn || hash || zero || dot || type)) || w >= 0) {
            sf_ch(&s->out, ':');
            if (fill) sf_ch(&s->out, fill);
            if (align) sf_ch(&s->out, align);
            if (!null_rw) {
                if (sgn) sf_ch(&s->out, sgn);
                if (tsep) sf_ch(&s->out, tsep);
                if (hash) sf_ch(&s->out, '#');
                if (zero) sf_ch(&s->out, '0');
            }
            if (w >= 0) sf_num(&s->out, w);
            if (!null_rw && dot) {
                sf_ch(&s->out, '.');
                if (p >= 0) sf_num(&s->out, p);
            }
            if (!null_rw && type && rw == SF_KEEP) sf_ch(&s->out, type);
        }
        sf_ch(&s->out, '}');
    }
}

static ray_t* sf_run(ray_t* x, int gfmt) {
    if (!x) return q_err(QE_TYPE);
    if (RAY_IS_ERR(x)) { ray_retain(x); return x; }
    if (x->type != RAY_LIST || x->len < 1) return q_err(QE_TYPE);
    const char* fp;
    int64_t     fn;
    ray_t*      f = ray_list_get(x, 0);
    if (!f || !q_str_text_bytes(f, &fp, &fn)) return q_err(QE_TYPE);

    sfscan s;
    memset(&s, 0, sizeof s);
    s.msg  = x;
    s.argn = x->len - 1;
    if (gfmt) sf_scan_format(&s, fp, fn);
    else sf_scan_printf(&s, fp, fn);

    ray_t* r = NULL;
    if (s.out.oom) s.bad = SF_ERR_OOM;
    if (!s.bad) {
        char*   out;
        int64_t outn;
        int st = gfmt ? pqfmt_format(s.out.p ? s.out.p : "", s.out.n, s.av, s.an, &out, &outn)
                      : pqfmt_printf(s.out.p ? s.out.p : "", s.out.n, s.av, s.an, &out, &outn);
        if (st == PQFMT_OK) {
            r = ray_charv(out, outn);
            pqfmt_freestr(out);
        } else {
            s.bad = st == PQFMT_NOMEM ? SF_ERR_OOM : SF_ERR_BAD;
        }
    }
    for (int i = 0; i < s.an; i++) ray_release(s.own[i]);
    free(s.own);
    free(s.av);
    free(s.out.p);
    if (r) return r;
    if (s.bad == SF_ERR_OOM) return q_err(QE_WSFULL);
    return q_err(gfmt ? QE_FORMAT : QE_PRINTF);
}

static ray_t* strfmt_printf_fn(ray_t* x) { return sf_run(x, 0); }
static ray_t* strfmt_format_fn(ray_t* x) { return sf_run(x, 1); }

static void strfmt_bind_fn(const char* name, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(name, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

void q_strfmt_register(void) {
    strfmt_bind_fn(".str.i.printf", strfmt_printf_fn);
    strfmt_bind_fn(".str.i.format", strfmt_format_fn);
}
