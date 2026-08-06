/* q_view — see q_view.h.  aux[1] bit 1 = pending (the stale cache is kept:
 * it IS the self-reference "previous value"), bit 2 = in-recalc ('loop at
 * recalc, not creation).  The env entry is the authority; the name roster
 * self-heals when a view is replaced or expunged. */
#include "qlang/eval/q_view.h"
#include "qlang/q_prim.h"
#include "qlang/eval/q_eval.h"
#include "qlang/eval/q_eval_internal.h"
#include "qlang/q_env.h"
#include "qlang/base/q_err.h"
#include "qlang/q_registry.h"
#include "qlang/parse/q_parse_internal.h"   /* Q_ATTR_HOLE */
#include "mem/heap.h"                 /* RAY_ATTR_SORTED */
#include "mem/sys.h"                  /* ray_sys_alloc/free — the roster */
#include "table/sym.h"
#include <stdlib.h>
#include <string.h>

#define VIEW_PENDING  1u
#define VIEW_INRECALC 2u

typedef struct {
    int64_t gcolon, zvs;
    int     ready;
} view_syms_t;
static view_syms_t g_syms;

static int64_t* g_names;                 /* creation-order roster of view names */
static int      g_n, g_cap;
static ray_t*   g_setidx;                /* .z.vs y for the in-flight indexed set */
static int      g_in_zvs;

static const view_syms_t* vsyms(void) {
    if (!g_syms.ready) {
        g_syms.gcolon = ray_sym_intern_runtime("::", 2);
        g_syms.zvs    = ray_sym_intern_runtime(".z.vs", 5);
        g_syms.ready  = 1;
    }
    return &g_syms;
}

void q_view_reset(void) {
    if (g_names) { ray_sys_free(g_names); g_names = NULL; }
    g_n = g_cap = 0;
    if (g_setidx) { ray_release(g_setidx); g_setidx = NULL; }
    g_in_zvs = 0;
    memset(&g_syms, 0, sizeof g_syms);
}

int q_view_is(ray_t* v) {
    return q_eval_apply_carrier_kind(v) == Q_EVAL_CAR_VIEW;
}

/* ray_sym_str is BORROWED (PLAN.md register, 2026-07-30) */
static int name_bytes(int64_t id, const char** p, size_t* n) {
    ray_t* s = ray_sym_str(id);
    if (!s) return 0;
    *p = ray_str_ptr(s);
    *n = ray_str_len(s);
    return 1;
}

static int name_dotted(int64_t id) {
    const char* p; size_t n;
    return name_bytes(id, &p, &n) && memchr(p, '.', n) != NULL;
}

/* env entry at sym iff it is (still) a view — BORROWED */
static ray_t* view_of(int64_t sym) {
    ray_t* v = q_env_get(sym);
    return (v && q_view_is(v)) ? v : NULL;
}

static void roster_add(int64_t sym) {
    for (int i = 0; i < g_n; i++)
        if (g_names[i] == sym) return;
    if (g_n == g_cap) {
        int nc = g_cap ? g_cap * 2 : 8;
        int64_t* nn = (int64_t*)ray_sys_alloc(sizeof(int64_t) * (size_t)nc);
        if (!nn) return;                       /* view stays live; roster heals */
        if (g_n) memcpy(nn, g_names, sizeof(int64_t) * (size_t)g_n);
        if (g_names) ray_sys_free(g_names);
        g_names = nn;
        g_cap = nc;
    }
    g_names[g_n++] = sym;
}

static void roster_heal(void) {
    int w = 0;
    for (int i = 0; i < g_n; i++)
        if (view_of(g_names[i])) g_names[w++] = g_names[i];
    g_n = w;
}


/* Free global name refs, first-appearance order.  A 1-element list / sym
 * vector is a CONSTANT (q_eval: enlist is the quote of parse trees) — never
 * descended, so a qSQL view depends only on its table (learn/views.md);
 * dotted names excluded (views + deps live in the default namespace only). */
static void deps_scan(ray_t* n, ray_t** deps, int* oom) {
    if (!n || *oom) return;
    if (n->type == -RAY_SYM) {
        if (n->attrs & Q_ATTR_HOLE) return;
        int64_t id = n->i64;
        if (q_eval_ctl_sym(id) || q_registry_is_reserved(id) || name_dotted(id))
            return;
        const char* p; size_t l;
        if (!name_bytes(id, &p, &l) || l == 0) return;
        if (q_eval_symvec_has(*deps, id)) return;
        ray_t* nv = ray_vec_append(*deps, &id);
        if (!nv) { *oom = 1; return; }
        *deps = nv;
        return;
    }
    if (n->type == RAY_SYM && ray_len(n) == 1) return;
    if (q_eval_fn_value(n) || n->type != RAY_LIST) return;
    int64_t k = ray_len(n);
    if (k == 1) return;
    ray_t** e = (ray_t**)ray_data(n);
    for (int64_t i = 0; i < k; i++) deps_scan(e[i], deps, oom);
}


/* Borrows c; owned result.  A re-entered recalc hands back the previous
 * value, else 'loop; the barrier frame keeps body names global. */
static ray_t* view_recalc(ray_t* c) {
    ray_t** s = q_eval_apply_view_slots(c);
    if (!s) { ray_retain(c); return c; }
    if (!(c->aux[1] & VIEW_PENDING) && s[3]) { ray_retain(s[3]); return s[3]; }
    if (c->aux[1] & VIEW_INRECALC) {
        if (s[3]) { ray_retain(s[3]); return s[3]; }
        return q_err(QE_LOOP);
    }
    if (q_env_frame_push(1) != RAY_OK) return q_err(QE_STACK);
    c->aux[1] |= VIEW_INRECALC;
    ray_t* r = q_eval_apply_concrete(q_eval(s[0]));
    c->aux[1] &= (uint8_t)~VIEW_INRECALC;
    q_env_frame_pop();
    if (!r) return q_err(QE_TYPE);
    if (RAY_IS_ERR(r)) return r;               /* stays pending, cache kept */
    ray_retain(r);
    if (s[3]) ray_release(s[3]);
    s[3] = r;
    c->aux[1] &= (uint8_t)~VIEW_PENDING;
    return r;
}

ray_t* q_view_deref(ray_t* v) {
    if (!q_view_is(v)) return v;
    ray_t* r = view_recalc(v);
    ray_release(v);
    return r;
}

ray_t* q_view_deref_borrowed(ray_t* v) {
    if (!q_view_is(v)) { ray_retain(v); return v; }
    return view_recalc(v);
}


/* Mark views depending on sym pending, then run the pending relation to a
 * fixpoint (iterative): a view depending on a pending view cannot stay valid. */
static void mark_dependents(int64_t sym) {
    for (int i = 0; i < g_n; i++) {
        ray_t* c = view_of(g_names[i]);
        if (!c || (c->aux[1] & VIEW_PENDING)) continue;
        if (q_eval_symvec_has(q_eval_apply_view_slots(c)[1], sym))
            c->aux[1] |= VIEW_PENDING;
    }
    int again = 1;
    while (again) {
        again = 0;
        for (int i = 0; i < g_n; i++) {
            ray_t* c = view_of(g_names[i]);
            if (!c || (c->aux[1] & VIEW_PENDING)) continue;
            ray_t* d = q_eval_apply_view_slots(c)[1];
            const void* dd = ray_data(d);
            for (int64_t j = 0, m = ray_len(d); j < m; j++) {
                ray_t* dep = view_of(ray_read_sym(dd, j, RAY_SYM, d->attrs));
                if (dep && (dep->aux[1] & VIEW_PENDING)) {
                    c->aux[1] |= VIEW_PENDING;
                    again = 1;
                    break;
                }
            }
        }
    }
}

void q_view_set_index(ray_t** idxv, int64_t k) {
    if (g_setidx) { ray_release(g_setidx); g_setidx = NULL; }
    if (!idxv) return;                     /* (NULL,0) = clear */
    ray_t* l = ray_list_new(k > 0 ? k : 1);
    for (int64_t i = 0; i < k && l && !RAY_IS_ERR(l); i++)
        l = ray_list_append(l, idxv[i] ? idxv[i] : RAY_NULL_OBJ);
    if (!l || RAY_IS_ERR(l)) { if (l) ray_release(l); return; }
    ray_t* cl = q_list_collapse(l);
    ray_release(l);
    if (cl && !RAY_IS_ERR(cl)) g_setidx = cl;
    else if (cl) ray_release(cl);
}

/* `.z.vs` fires AFTER any default-namespace global set (ref/dotz.md; `a.b`
 * fires, `.a.b` does not); errors swallowed — the set already happened.  The
 * index snapshot is consumed up front so a nested set never sees a stale one. */
void q_view_on_global_set(int64_t sym) {
    ray_t* idx = g_setidx;
    g_setidx = NULL;
    const char* p; size_t n;
    if (!name_bytes(sym, &p, &n) || n == 0 || p[0] == '.') {
        if (idx) ray_release(idx);
        return;
    }
    if (g_n) {
        if (!view_of(sym)) roster_heal();     /* replaced/expunged views drop */
        mark_dependents(sym);
    }
    if (!g_in_zvs) {
        ray_t* fn = q_env_get(vsyms()->zvs);   /* borrowed */
        if (fn && !RAY_IS_ERR(fn)) {
            ray_retain(fn);                    /* survive re-assign mid-call */
            g_in_zvs = 1;
            ray_t* xs = ray_sym(sym);
            ray_t* y = idx ? idx : ray_list_new(1);
            ray_t* args[2] = { xs, y };
            ray_t* r = (xs && y) ? q_eval_apply_value(fn, args, 2) : NULL;
            if (r && RAY_IS_ERR(r)) { q_err_drop(); ray_error_free(r); }
            else if (r) ray_release(r);
            if (xs) ray_release(xs);
            if (y) { ray_release(y); idx = NULL; }
            g_in_zvs = 0;
            ray_release(fn);
        }
    }
    if (idx) ray_release(idx);
}

void q_view_on_global_unbind(int64_t sym) {
    if (!g_n) return;
    roster_heal();
    mark_dependents(sym);
}


/* consumes body; owned :: or error */
static ray_t* view_define(int64_t name, ray_t* body, const char* txt,
                          int64_t tn) {
    int oom = 0;
    ray_t* deps = ray_sym_vec_new(RAY_SYM_W64, 4);
    if (!deps) { ray_release(body); return q_err(QE_WSFULL); }
    deps_scan(body, &deps, &oom);
    ray_t* text = ray_charv(txt, tn);
    ray_t* c = (oom || !text || RAY_IS_ERR(text))
                   ? NULL : q_eval_apply_view_new(body, deps, text);
    ray_release(body);
    ray_release(deps);
    if (text && !RAY_IS_ERR(text)) ray_release(text);
    if (!c || RAY_IS_ERR(c)) return c ? c : q_err(QE_WSFULL);
    c->aux[1] = VIEW_PENDING;
    roster_add(name);                          /* before the set: never a live
                                                * view the roster cannot see */
    ray_err_t e = q_env_set(name, c);
    ray_release(c);
    if (e != RAY_OK) return q_env_err(e);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* The source after `name::`, comment-stripped and trimmed (`view `v` returns
 * exactly this).  A failed scan DECLINES — the statement falls back to the
 * eager assign, never a half-recorded view. */
static int text_slice(const char* src, int64_t name, const char** txt,
                      int64_t* tn) {
    const char* p; size_t nl;
    if (!src || !name_bytes(name, &p, &nl)) return 0;
    const char* q = src;
    while (*q == ' ' || *q == '\t') q++;
    if (strncmp(q, p, nl) != 0) return 0;
    q += nl;
    while (*q == ' ' || *q == '\t') q++;
    if (q[0] != ':' || q[1] != ':') return 0;
    q += 2;
    while (*q == ' ' || *q == '\t') q++;
    const char* end = q + strlen(q);
    int instr = 0;
    for (const char* r = q; *r; r++) {
        if (instr) {
            if (*r == '\\' && r[1]) r++;
            else if (*r == '"') instr = 0;
        } else if (*r == '"') {
            instr = 1;
        } else if (*r == '/' && (r == q || r[-1] == ' ' || r[-1] == '\t')) {
            end = r;
            break;
        }
    }
    while (end > q && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\n' || end[-1] == '\r'))
        end--;
    *txt = q;
    *tn = end - q;
    return *tn > 0;
}

/* `name::rhs`, or `name::rhs;e2;...` — the view swallows the REST of the
 * line (learn/views.md `v::a;b;select from t where a in b`). */
int q_view_intercept(ray_t* ast, const char* src, ray_t** out) {
    *out = NULL;
    if (!ast || ast->type != RAY_LIST) return 0;
    if (q_eval_apply_frame_depth() > 0) return 0;
    int64_t n = ray_len(ast);
    ray_t** e = (ray_t**)ray_data(ast);
    if (n < 3 || !e[0]) return 0;
    const view_syms_t* S = vsyms();
    ray_t* asn = NULL;
    int seq = 0;
    if (e[0]->type == -RAY_SYM && e[0]->i64 == S->gcolon && n == 3) {
        asn = ast;
    } else if (q_ast_is_seq_head(e[0]) && e[1] && e[1]->type == RAY_LIST &&
               ray_len(e[1]) == 3) {
        ray_t** a1 = (ray_t**)ray_data(e[1]);
        if (a1[0] && a1[0]->type == -RAY_SYM && a1[0]->i64 == S->gcolon) {
            asn = e[1];
            seq = 1;
        }
    }
    if (!asn) return 0;
    ray_t** a = (ray_t**)ray_data(asn);
    ray_t* tgt = a[1];
    if (!tgt || tgt->type != -RAY_SYM || (tgt->attrs & Q_ATTR_HOLE) || !a[2])
        return 0;
    if (q_registry_is_reserved(tgt->i64) || name_dotted(tgt->i64)) return 0;
    const char* txt;
    int64_t tn;
    if (!text_slice(src, tgt->i64, &txt, &tn)) return 0;
    ray_t* body;
    if (!seq) {
        ray_retain(a[2]);
        body = a[2];
    } else {
        ray_t* b = ray_list_new(n);
        b = b ? ray_list_append(b, e[0]) : NULL;
        b = b ? ray_list_append(b, a[2]) : NULL;
        for (int64_t i = 2; i < n && b && !RAY_IS_ERR(b); i++)
            b = ray_list_append(b, e[i] ? e[i] : RAY_NULL_OBJ);
        if (!b || RAY_IS_ERR(b)) {
            *out = b ? b : q_err(QE_WSFULL);
            return 1;
        }
        body = b;
    }
    *out = view_define(tgt->i64, body, txt, tn);
    return 1;
}


ray_t* q_view_text(ray_t* v) {
    ray_t** s = q_eval_apply_view_slots(v);
    if (!s) return NULL;
    ray_retain(s[2]);
    return s[2];
}

ray_t* q_view_value4(ray_t* v) {
    ray_t** s = q_eval_apply_view_slots(v);
    if (!s) return q_err(QE_TYPE);
    ray_t* out = ray_list_new(4);
    out = out ? ray_list_append(out, s[3] ? s[3] : RAY_NULL_OBJ) : NULL;
    out = (out && !RAY_IS_ERR(out)) ? ray_list_append(out, s[0]) : out;
    out = (out && !RAY_IS_ERR(out)) ? ray_list_append(out, s[1]) : out;
    out = (out && !RAY_IS_ERR(out)) ? ray_list_append(out, s[2]) : out;
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_WSFULL);
    return out;
}

ray_t* q_view_wrap(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return q_err(QE_TYPE);
    ray_t* c = view_of(x->i64);
    if (!c) {
        const char* p; size_t n;
        if (!name_bytes(x->i64, &p, &n)) return q_err(QE_TYPE);
        return q_err_name(p, n);
    }
    return q_view_text(c);
}

ray_t* q_view_names(int64_t ns_sym, int pending) {
    int root = ns_sym == 0;
    if (!root) {
        const char* p; size_t n;
        if (name_bytes(ns_sym, &p, &n) && (n == 0 || (n == 1 && p[0] == '.')))
            root = 1;
    }
    if (!root && !q_env_ns_exists(ns_sym)) return NULL;
    roster_heal();
    int64_t* ids = (int64_t*)ray_sys_alloc(sizeof(int64_t) *
                                           (size_t)(g_n > 0 ? g_n : 1));
    if (!ids) return NULL;
    int m = 0;
    if (root) {
        for (int i = 0; i < g_n; i++) {
            ray_t* c = view_of(g_names[i]);
            if (!c) continue;
            if (pending && !(c->aux[1] & VIEW_PENDING)) continue;
            ids[m++] = g_names[i];
        }
    }
    qsort(ids, (size_t)m, sizeof *ids, q_env_name_cmp);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, m > 0 ? m : 1);
    for (int i = 0; i < m && out && !RAY_IS_ERR(out); i++)
        out = ray_vec_append(out, &ids[i]);
    ray_sys_free(ids);
    if (out && !RAY_IS_ERR(out) && !pending && ray_len(out) > 0)
        out->attrs |= RAY_ATTR_SORTED;         /* `\b` prints `s#...` (syscmds.md) */
    return out;
}

/* .z.b: depended-on name -> the views depending on it (ref/dotz.md#zb). */
ray_t* q_view_zb(void) {
    roster_heal();
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 4);
    ray_t* vals = ray_list_new(4);
    if (!keys || !vals) {
        if (keys) ray_release(keys);
        if (vals) ray_release(vals);
        return q_err(QE_WSFULL);
    }
    for (int i = 0; i < g_n; i++) {
        ray_t* c = view_of(g_names[i]);
        if (!c) continue;
        ray_t* d = q_eval_apply_view_slots(c)[1];
        const void* dd = ray_data(d);
        for (int64_t j = 0, m = ray_len(d); j < m; j++) {
            int64_t dep = ray_read_sym(dd, j, RAY_SYM, d->attrs);
            int64_t at = -1;
            const void* kd = ray_data(keys);
            for (int64_t k = 0, kn = ray_len(keys); k < kn; k++)
                if (ray_read_sym(kd, k, RAY_SYM, keys->attrs) == dep) {
                    at = k;
                    break;
                }
            if (at < 0) {
                ray_t* nk = ray_vec_append(keys, &dep);
                ray_t* nv = nk ? ray_sym_vec_new(RAY_SYM_W64, 2) : NULL;
                nv = nv ? ray_vec_append(nv, &g_names[i]) : NULL;
                ray_t* nl = nv ? ray_list_append(vals, nv) : NULL;
                if (nv) ray_release(nv);
                if (!nk || !nl || RAY_IS_ERR(nl)) {
                    ray_release(nk ? nk : keys);
                    ray_release(nl && !RAY_IS_ERR(nl) ? nl : vals);
                    return q_err(QE_WSFULL);
                }
                keys = nk;
                vals = nl;
            } else {
                ray_t** vs = (ray_t**)ray_data(vals);
                ray_t* nv = ray_vec_append(vs[at], &g_names[i]);
                if (!nv) {
                    ray_release(keys);
                    ray_release(vals);
                    return q_err(QE_WSFULL);
                }
                vs[at] = nv;
            }
        }
    }
    return ray_dict_new(keys, vals);           /* consumes both */
}
