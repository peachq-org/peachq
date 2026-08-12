/* q_env — q's K-tree as NESTED DICTS.  See q_env.h for the model.
 *
 * The two NAME-FACING verbs live here rather than in ops/: `key` and `set`
 * read and write the tree itself (root roster, context members, the one
 * .ipc.on.* seam), so their bodies belong beside the tree, not beside the
 * table kernels their dict arms happen to touch. */
#define _POSIX_C_SOURCE 200809L
#define Q_OPS_ENV_GRANDFATHER /* the .ipc.on.* six-slot callback seam: ray_env_set on hook syms only */
#include "qlang/q_env.h"
#include "qlang/q_registry_internal.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"     /* q_type_is_table / _is_keyed — the \v|\a split */
#include "qlang/q_builtins.h"      /* q_type_is_fn — the \f split (needs the apply module) */
#include "qlang/q_comment.h"          /* the armed doc header's one consumption point */
#include "qlang/q_dotz.h"         /* the .z.* handler-slot arms of `set` */
#include "qlang/eval/q_view.h"    /* view hooks: set/unbind invalidation, dot-'nyi */
#include "qlang/io/q_io.h"        /* q_io_set — `set`'s file half */
#include "lang/internal.h"        /* ray_error */
#include "table/sym.h"         /* ray_sym_intern_runtime, ray_sym_str, ray_read_sym */
#include "table/dict.h"        /* ray_dict_* probes/upsert */
#include "ops/linkop.h"        /* ray_link_has / ray_link_deref */
#include "ops/temporal.h"      /* ray_temporal_accessor — the dotted accessor roster */
#include "lang/env.h"          /* the SIX .ipc.on.* hook syms — the one rayfall-env seam */
#include "mem/sys.h"
#include <stdlib.h>            /* qsort — the member listings are sorted */
#include <string.h>

#define ENV_NAME_MAX 256
#define ENV_SEG_MAX 64
#define ENV_FRAME_MAX 2048
#define ENV_FRAME_INLINE 16

/* The three directory dicts.  Each carries the load-bearing ` -> :: marker:
 * it keeps the value slot a general RAY_LIST (same-typed members would
 * otherwise collapse to a typed vector, and ray_dict_probe_sym_borrowed
 * refuses non-LIST values — collapse is the MODEL's no-traversal rule). */
static ray_t* env_root;        /* `.` — user root variables */
static ray_t* env_ns;          /* ``  — top-level namespaces (nested dicts) */
static ray_t* env_boot;        /* bootstrap builtins: q_env_bind plain names,
                                * kept out of the user-visible `key `.` roster */
/* `\d` context: the ns sym as given (`.jab`) plus its SEGMENT (`jab`), which is
 * what env_ns is keyed by.  Deriving the segment once per `\d` keeps relative
 * resolution to one extra dict probe and NO sym-table round trip (#359). */
static int64_t g_ctx, g_ctx_seg;

static int64_t env_marker(void) { return ray_sym_intern_runtime("", 0); }

/* q_env_marker_sym — see q_env.h. */
int64_t q_env_marker_sym(void) { return env_marker(); }

static ray_t* env_marker_dict(void) {
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 1);
    ray_t* vals = ray_list_new(1);
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) {
        if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
        if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
        return NULL;
    }
    int64_t m = env_marker();
    keys = ray_vec_append(keys, &m);
    vals = ray_list_append(vals, RAY_NULL_OBJ);
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) {
        if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
        if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
        return NULL;
    }
    ray_t* d = ray_dict_new(keys, vals);
    return (d && !RAY_IS_ERR(d)) ? d : NULL;
}

static int env_is_marked(ray_t* v) {
    if (!v || RAY_IS_ERR(v) || v->type != RAY_DICT) return 0;
    ray_t* k = ray_dict_keys(v);
    ray_t* vl = ray_dict_vals(v);
    return k && k->type == RAY_SYM && k->len >= 1 &&
           vl && vl->type == RAY_LIST &&
           ray_read_sym(ray_data(k), 0, RAY_SYM, k->attrs) == env_marker();
}

static ray_t* name_str(int64_t sym, const char** p, size_t* n) {
    ray_t* s = ray_sym_str(sym);
    if (!s) return NULL;
    *p = ray_str_ptr(s);
    *n = ray_str_len(s);
    return s;                                  /* caller releases */
}

/* `.a.b` -> [a,b] from start=1; `..a` -> [a] from start=2 (root-qualified);
 * plain from start=0.  Returns the count, -1 on overflow. */
static int env_segs(const char* p, size_t n, size_t start, int64_t* segs, int max) {
    int k = 0;
    for (size_t i = start; i <= n; ) {
        size_t e = i;
        while (e < n && p[e] != '.') e++;
        if (k == max) return -1;
        segs[k++] = ray_sym_intern_runtime(p + i, e - i);
        i = e + 1;
    }
    return k;
}

static size_t env_start(const char* p, size_t n) {
    return p[0] == '.' ? ((n > 1 && p[1] == '.') ? 2 : 1) : 0;
}

/* `\d .ns` re-roots a RELATIVE name: the context segment becomes the path's
 * first step, so a write lands in — and thereby creates — the namespace.
 * Returns the new segment count and re-points *home; k unchanged at root. */
static int ctx_reroot(int64_t* segs, int k, ray_t*** home) {
    if (!g_ctx_seg || k <= 0 || k >= ENV_SEG_MAX) return k;
    memmove(segs + 1, segs, (size_t)k * sizeof *segs);
    segs[0] = g_ctx_seg;
    *home = &env_ns;
    return k + 1;
}

ray_t* q_env_get(int64_t sym) {
    const char* p; size_t n;
    ray_t* s = name_str(sym, &p, &n);
    if (!s) return NULL;
    ray_t* v = NULL;
    if (n == 1 && p[0] == '.') v = env_root;
    else if (n > 0) {
        size_t start = env_start(p, n);
        ray_t* home = (p[0] == '.' && start == 1) ? env_ns : env_root;
        int64_t segs[ENV_SEG_MAX];
        int k = env_segs(p, n, start, segs, ENV_SEG_MAX);
        if (k > 0) {
            v = ray_dict_probe_sym_borrowed(home, segs[0]);
            if (!v && p[0] != '.') v = ray_dict_probe_sym_borrowed(env_boot, segs[0]);
            for (int i = 1; v && i < k; i++)
                v = ray_dict_probe_sym_borrowed(v, segs[i]);
        }
    }
    ray_release(s);
    return v;
}

/* ---- assignment: path-copy amend down the dict chain ---- */

/* Consumes d; owned result, NULL on failure.  Missing ancestors conjure
 * marked dicts (`.fee.fi.fo:42` creates `.fee`, `.fee.fi`); an existing
 * non-dict (or unreachable collapsed slot) intermediate refuses. */
static ray_t* env_amend(ray_t* d, const int64_t* segs, int nseg, int i, ray_t* val) {
    ray_t* k = ray_sym(segs[i]);
    if (!k || RAY_IS_ERR(k)) {
        ray_release(d);
        if (k) ray_release(k);
        return NULL;
    }
    ray_t* r;
    if (i == nseg - 1) {
        r = ray_dict_upsert(d, k, val);
    } else {
        ray_t* child = ray_dict_probe_sym_borrowed(d, segs[i]);
        ray_t* sub = NULL;
        if (child && child->type == RAY_DICT) { ray_retain(child); sub = child; }
        else if (!child && ray_dict_find_sym(d, segs[i]) < 0) sub = env_marker_dict();
        if (!sub) {
            ray_release(d);
            ray_release(k);
            return NULL;
        }
        ray_t* nd = env_amend(sub, segs, nseg, i + 1, val);
        if (!nd) {
            ray_release(d);
            ray_release(k);
            return NULL;
        }
        r = ray_dict_upsert(d, k, nd);
        ray_release(nd);
    }
    ray_release(k);
    if (r && RAY_IS_ERR(r)) { ray_release(r); r = NULL; }
    return r;
}

/* The home is retained across the amend so a mid-path failure never loses
 * the tree (upsert consumes and path-copies via ray_cow at rc>1). */
static ray_err_t env_store(ray_t** home, const int64_t* segs, int nseg, ray_t* val) {
    if (!*home) return RAY_ERR_DOMAIN;
    ray_retain(*home);
    ray_t* nd = env_amend(*home, segs, nseg, 0, val);
    if (!nd) return RAY_ERR_DOMAIN;
    ray_release(*home);
    *home = nd;
    return RAY_OK;
}

static ray_err_t env_put(int64_t sym, ray_t* val, int boot_new) {
    const char* p; size_t n;
    ray_t* s = name_str(sym, &p, &n);
    if (!s) return RAY_ERR_DOMAIN;
    ray_err_t e = RAY_ERR_DOMAIN;
    int64_t segs[ENV_SEG_MAX];
    if (n > 0 && !(n == 1 && p[0] == '.')) {
        size_t start = env_start(p, n);
        int k = env_segs(p, n, start, segs, ENV_SEG_MAX);
        if (k <= 0) e = RAY_ERR_DOMAIN;
        else if (p[0] == '.') {
            e = env_store(start == 1 ? &env_ns : &env_root, segs, k, val);
        } else {
            ray_t** home = &env_root;
            int nk = ctx_reroot(segs, k, &home);
            if (nk == k && ray_dict_find_sym(env_root, segs[0]) < 0 &&
                (ray_dict_find_sym(env_boot, segs[0]) >= 0 || boot_new))
                home = &env_boot;
            e = env_store(home, segs, nk, val);
        }
    }
    ray_release(s);
    return e;
}

ray_err_t q_env_bind(int64_t sym, ray_t* val) { return env_put(sym, val, 1); }

/* q_env_take — see q_env.h.  The park is a plain bind, so it stays invisible
 * to views: only the caller's rebind is an observable write.  Under `\d` a
 * RELATIVE name reads from the root but writes into the context, so the slot
 * q_env_get found is not the one a bind would park — refuse and let it copy. */
int q_env_take(int64_t sym, ray_t* cur) {
    if (!cur) return 0;
    const char* p; size_t n;
    ray_t* s = name_str(sym, &p, &n);
    if (!s) return 0;
    int relative = n > 0 && p[0] != '.';
    ray_release(s);
    if ((g_ctx_seg && relative) || q_env_get(sym) != cur) return 0;
    return q_env_bind(sym, RAY_NULL_OBJ) == RAY_OK;
}

/* `` `. `` names the root itself, so assigning a dict RESTORES its members as
 * globals (ref/get.md `set`) — the ` -> :: marker is representation, skipped.
 * Members go back through q_env_set, so each gets the same name policy. */
static ray_err_t env_root_splat(ray_t* d) {
    if (!d || d->type != RAY_DICT) return RAY_ERR_TYPE;
    ray_t*  dk = ray_dict_keys(d);                  /* borrowed */
    ray_t*  dv = ray_dict_vals(d);                  /* borrowed */
    int64_t n = ray_dict_len(d), marker = env_marker();
    ray_err_t e = RAY_OK;
    for (int64_t i = 0; i < n && e == RAY_OK; i++) {
        ray_t* k = q_join_item(dk, i);              /* owned */
        ray_t* v = q_join_item(dv, i);              /* owned */
        if (!k || RAY_IS_ERR(k) || k->type != -RAY_SYM || !v || RAY_IS_ERR(v))
            e = RAY_ERR_TYPE;
        else if (k->i64 != marker)
            e = q_env_set(k->i64, v);
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
    }
    return e;
}

/* THE one write home: every q form that binds a name (`:` `::`, indexed and
 * modified assign, `@`/`.` name-amend, `set`) lands here, so the whole name
 * policy is stated once — the `` `. `` root splat, the `.z.p*` alias onto the
 * six `.ipc.on.*` hook syms over RAYFALL's env (the one deliberate seam:
 * src/core/ipc.c reads them from g_env at dispatch).  Everything else is an
 * ordinary tree amend, settable `.z.*` handlers and `.z.zd` included — plain
 * globals their C fire sites read by name (q_wirefile reads the zip triple).
 * The branches converge on `e` so that EVERY successful bind — hook aliases
 * the tree amend never sees included — is a definition a doc header can name. */
ray_err_t q_env_set(int64_t sym, ray_t* val) {
    if (!val) return q_env_unbind(sym);
    ray_err_t e;
    if (ray_sym_is_ipc_hook(sym)) {
        e = ray_env_set(sym, val);
    } else {
        const char* p; size_t n;
        ray_t* s = name_str(sym, &p, &n);
        if (!s) return RAY_ERR_DOMAIN;
        int hook = q_dotz_ipc_hook_index(p, n);
        int root = n == 1 && p[0] == '.';
        ray_release(s);
        if (hook >= 0)   e = ray_env_set(ray_sym_ipc_hook(hook), val);
        else if (root)   e = env_root_splat(val);
        else {
            e = env_put(sym, val, 0);
            if (e == RAY_OK) q_view_on_global_set(sym);   /* invalidation + .z.vs */
        }
    }
    if (e == RAY_OK) q_comment_on_global_set(sym);
    return e;
}

/* q_env_err — see q_env.h. */
ray_t* q_env_err(ray_err_t e) {
    if (e == RAY_ERR_NYI)  return q_err(QE_NYI);
    if (e == RAY_ERR_TYPE) return q_err(QE_TYPE);
    return q_err(QE_ASSIGN);
}

ray_err_t q_env_unbind(int64_t sym) {
    const char* p; size_t n;
    ray_t* s = name_str(sym, &p, &n);
    if (!s) return RAY_ERR_TYPE;
    ray_err_t e = RAY_OK;
    int64_t segs[ENV_SEG_MAX];
    if (n > 0 && !(n == 1 && p[0] == '.')) {
        size_t start = env_start(p, n);
        ray_t** home = (p[0] == '.' && start == 1) ? &env_ns : &env_root;
        int k = env_segs(p, n, start, segs, ENV_SEG_MAX);
        if (k > 0 && p[0] != '.') {
            int nk = ctx_reroot(segs, k, &home);
            if (nk == k && ray_dict_find_sym(*home, segs[0]) < 0 &&
                ray_dict_find_sym(env_boot, segs[0]) >= 0)
                home = &env_boot;
            k = nk;
        }
        if (k > 0) {
            ray_t* holder = *home;
            for (int i = 0; holder && i < k - 1; i++)
                holder = ray_dict_probe_sym_borrowed(holder, segs[i]);
            if (holder && ray_dict_find_sym(holder, segs[k - 1]) >= 0) {
                ray_t* key = ray_sym(segs[k - 1]);
                if (!key || RAY_IS_ERR(key)) {
                    if (key) ray_release(key);
                    e = RAY_ERR_OOM;
                } else {
                    ray_retain(holder);
                    ray_t* nd = ray_dict_remove(holder, key);
                    ray_release(key);
                    if (!nd || RAY_IS_ERR(nd)) {
                        if (nd) ray_release(nd);
                        e = RAY_ERR_TYPE;
                    } else if (k == 1) {
                        ray_release(*home);
                        *home = nd;
                    } else {
                        e = env_store(home, segs, k - 1, nd);
                        ray_release(nd);
                    }
                }
            }
        }
    }
    ray_release(s);
    if (e == RAY_OK) q_view_on_global_unbind(sym);   /* dependents go pending */
    return e;
}

int q_env_ns_exists(int64_t path_sym) { return env_is_marked(q_env_get(path_sym)); }

ray_t* q_env_ns_view(int64_t path_sym) {
    ray_t* v = q_env_get(path_sym);
    if (!v || v->type != RAY_DICT) return NULL;
    ray_retain(v);
    return v;
}

/* ---- local frames ---- */

typedef struct {
    int64_t* keys;
    ray_t**  vals;
    int32_t  n, cap;
    uint8_t  barrier;
    int32_t  saved_view;
    int64_t  keys_in[ENV_FRAME_INLINE];
    ray_t*   vals_in[ENV_FRAME_INLINE];
} frame_t;

static _Thread_local frame_t* g_frames;
static _Thread_local int32_t g_fdepth, g_fcap;
static _Thread_local int32_t g_fview = Q_ENV_FRAME_VIEW_OFF;
static _Thread_local int32_t g_ffloor;

int32_t q_env_frame_depth(void) { return g_fdepth; }

int32_t q_env_frame_floor(int32_t floor) {
    int32_t prev = g_ffloor;
    g_ffloor = floor < 0 ? g_fdepth : floor;
    return prev;
}

int32_t q_env_frame_view(int32_t depth) {
    int32_t prev = g_fview;
    g_fview = depth;
    return prev;
}

ray_err_t q_env_frame_push(int barrier) {
    if (g_fdepth >= ENV_FRAME_MAX) return RAY_ERR_OOM;
    if (g_fdepth == g_fcap) {
        int32_t nc = g_fcap ? g_fcap * 2 : 32;
        frame_t* nf = (frame_t*)ray_sys_alloc(sizeof(frame_t) * (size_t)nc);
        if (!nf) return RAY_ERR_OOM;
        if (g_fdepth) memcpy(nf, g_frames, sizeof(frame_t) * (size_t)g_fdepth);
        /* re-point inline storage: frames moved */
        for (int32_t i = 0; i < g_fdepth; i++) {
            if (g_frames[i].keys == g_frames[i].keys_in) nf[i].keys = nf[i].keys_in;
            if (g_frames[i].vals == g_frames[i].vals_in) nf[i].vals = nf[i].vals_in;
        }
        if (g_frames) ray_sys_free(g_frames);
        g_frames = nf; g_fcap = nc;
    }
    frame_t* f = &g_frames[g_fdepth++];
    f->keys = f->keys_in;
    f->vals = f->vals_in;
    f->cap = ENV_FRAME_INLINE;
    f->n = 0;
    f->barrier = (uint8_t)(barrier != 0);
    f->saved_view = g_fview;
    g_fview = Q_ENV_FRAME_VIEW_OFF;
    return RAY_OK;
}

void q_env_frame_pop(void) {
    if (g_fdepth <= 0) return;
    frame_t* f = &g_frames[--g_fdepth];
    g_fview = f->saved_view;
    for (int32_t i = 0; i < f->n; i++)
        if (f->vals[i]) ray_release(f->vals[i]);
    if (f->keys != f->keys_in) ray_sys_free(f->keys);
    if (f->vals != f->vals_in) ray_sys_free(f->vals);
}

ray_err_t q_env_local_set(int64_t sym, ray_t* val) {
    if (g_fdepth <= 0) return q_env_set(sym, val);
    frame_t* f = &g_frames[g_fdepth - 1];
    for (int32_t i = 0; i < f->n; i++) {
        if (f->keys[i] != sym) continue;
        ray_retain(val);
        if (f->vals[i]) ray_release(f->vals[i]);
        f->vals[i] = val;
        return RAY_OK;
    }
    if (f->n == f->cap) {
        int32_t nc = f->cap * 2;
        int64_t* nk = (int64_t*)ray_sys_alloc(sizeof(int64_t) * (size_t)nc);
        ray_t**  nv = (ray_t**)ray_sys_alloc(sizeof(ray_t*) * (size_t)nc);
        if (!nk || !nv) {
            if (nk) ray_sys_free(nk);
            if (nv) ray_sys_free(nv);
            return RAY_ERR_OOM;
        }
        memcpy(nk, f->keys, sizeof(int64_t) * (size_t)f->n);
        memcpy(nv, f->vals, sizeof(ray_t*) * (size_t)f->n);
        if (f->keys != f->keys_in) ray_sys_free(f->keys);
        if (f->vals != f->vals_in) ray_sys_free(f->vals);
        f->keys = nk; f->vals = nv; f->cap = nc;
    }
    f->keys[f->n] = sym;
    ray_retain(val);
    f->vals[f->n] = val;
    f->n++;
    return RAY_OK;
}

ray_t* q_env_local_get(int64_t sym) {
    if (g_fdepth <= 0) return NULL;
    frame_t* f = &g_frames[g_fdepth - 1];
    for (int32_t i = 0; i < f->n; i++)
        if (f->keys[i] == sym) return f->vals[i];
    return NULL;
}

int q_env_local_take(int64_t sym, ray_t* cur) {
    if (g_fdepth <= 0 || !cur) return 0;
    frame_t* f = &g_frames[g_fdepth - 1];
    for (int32_t i = 0; i < f->n; i++) {
        if (f->keys[i] != sym || f->vals[i] != cur) continue;
        f->vals[i] = RAY_NULL_OBJ;              /* the singleton is retain-free */
        ray_release(cur);
        return 1;
    }
    return 0;
}

static ray_t* frames_lookup(int64_t sym) {
    if (g_fview == Q_ENV_FRAME_VIEW_NONE) return NULL;
    int32_t top = g_fdepth - 1;
    if (g_fview >= 0 && g_fview < top) top = g_fview;
    for (int32_t d = top; d >= g_ffloor; d--) {
        frame_t* f = &g_frames[d];
        for (int32_t i = 0; i < f->n; i++)
            if (f->keys[i] == sym) return f->vals[i];
        if (f->barrier) break;
    }
    return NULL;
}

/* ---- resolution ---- */

/* successive per-segment indexing off a borrowed base: dict/table probe,
 * linked-column deref (errors surface), temporal accessor.  Owned result. */
static ray_t* walk_segs(ray_t* v, int fresh, const char* p, size_t n, size_t pos) {
    while (v && pos < n) {
        if (q_view_is(v)) {                  /* "Views do not support dot notation" */
            if (fresh) ray_release(v);
            return q_err(QE_NYI);
        }
        size_t end = pos;
        while (end < n && p[end] != '.') end++;
        int64_t seg = ray_sym_intern_runtime(p + pos, end - pos);
        ray_t* next = NULL;
        int next_fresh = 0;
        if (ray_link_has(v)) {
            next = ray_link_deref(v, seg);
            if (next && RAY_IS_ERR(next)) {
                if (fresh) ray_release(v);
                return next;
            }
            next_fresh = (next != NULL);
        }
        if (!next) next = ray_container_probe_sym(v, seg);
        if (!next && v->type != RAY_DICT) {
            /* never on a dict: `.q.date` must not fire the date clock */
            ray_t* (*fn)(ray_t*) = ray_temporal_accessor(seg);
            if (fn) {
                next = fn(v);
                if (!next || RAY_IS_ERR(next)) {
                    if (next) ray_release(next);
                    next = NULL;
                } else {
                    next_fresh = 1;
                }
            }
        }
        if (fresh) ray_release(v);
        v = next;
        fresh = next_fresh;
        pos = end + 1;
    }
    if (!v) return NULL;
    if (!fresh) ray_retain(v);
    return v;
}

ray_t* q_env_resolve(int64_t sym) {
    ray_t* v = frames_lookup(sym);
    if (v) { ray_retain(v); return v; }
    if (ray_sym_is_ipc_hook(sym)) {            /* the six-slot callback table */
        v = ray_env_get(sym);
        if (v) ray_retain(v);
        return v;
    }
    const char* p; size_t n;
    ray_t* s = name_str(sym, &p, &n);
    if (!s) return NULL;
    if (n == 1 && p[0] == '.' && env_root) {
        ray_release(s);
        ray_retain(env_root);
        return env_root;
    }
    if (n == 0 || (n == 1 && p[0] == '.')) { ray_release(s); return NULL; }
    size_t start = env_start(p, n);
    size_t hend = start;
    while (hend < n && p[hend] != '.') hend++;
    if (hend == start) { ray_release(s); return NULL; }    /* ".."-style names */
    int64_t head = ray_sym_intern_runtime(p + start, hend - start);
    ray_t* base;
    if (p[0] == '.') {
        base = ray_dict_probe_sym_borrowed(start == 1 ? env_ns : env_root, head);
    } else {
        base = frames_lookup(head);
        if (!base && g_ctx_seg) {                   /* `\d .ns` shadows the root */
            ray_t* nsd = ray_dict_probe_sym_borrowed(env_ns, g_ctx_seg);
            if (nsd) base = ray_dict_probe_sym_borrowed(nsd, head);
        }
        if (!base) base = ray_dict_probe_sym_borrowed(env_root, head);
        if (!base) base = ray_dict_probe_sym_borrowed(env_boot, head);
    }
    ray_t* r = NULL;
    if (base) {
        if (hend >= n) { ray_retain(base); r = base; }
        else r = walk_segs(base, 0, p, n, hend + 1);
    }
    ray_release(s);
    return r;
}

/* ---- introspection: rosters and member listings ---- */

int64_t q_env_qualify(int64_t ns_sym, int64_t member_sym) {
    if (!ns_sym) return member_sym;                     /* the root prefixes nothing */
    const char* np; size_t nn;
    ray_t* ns = name_str(ns_sym, &np, &nn);
    if (!ns) return -1;
    if (nn == 1 && np[0] == '.') { ray_release(ns); return member_sym; }
    const char* mp; size_t mn;
    ray_t* m = name_str(member_sym, &mp, &mn);
    char buf[ENV_NAME_MAX];
    int64_t r = -1;                     /* refusal: never a valid sym id */
    if (m && nn + 1 + mn < sizeof buf) {
        memcpy(buf, np, nn);
        buf[nn] = '.';
        memcpy(buf + nn + 1, mp, mn);
        r = ray_sym_intern_runtime(buf, nn + 1 + mn);
    }
    if (m) ray_release(m);
    ray_release(ns);
    return r;
}

/* q_env_fullname — see q_env.h. */
int64_t q_env_fullname(int64_t sym, int64_t* ns) {
    const char* p; size_t n;
    ray_t* s = name_str(sym, &p, &n);
    int64_t full = sym;
    if (s) {
        if (n > 0 && p[0] != '.') {
            int64_t q = q_env_qualify(g_ctx, sym);
            if (q >= 0) full = q;
        }
        ray_release(s);
    }
    if (ns) {
        size_t cut = 0;
        ray_t* f = name_str(full, &p, &n);
        if (f) {
            for (size_t i = 0; i < n; i++) if (p[i] == '.') cut = i;
            *ns = cut > 0 ? ray_sym_intern_runtime(p, cut)
                          : ray_sym_intern_runtime(".", 1);
            ray_release(f);
        } else {
            *ns = ray_sym_intern_runtime(".", 1);
        }
    }
    return full;
}

static int env_kind_match(q_env_ns_kind_t kind, ray_t* v) {
    switch (kind) {
    case Q_ENV_NS_FNS:    return q_type_is_fn(v) && !q_view_is(v);   /* views: `\b` only */
    case Q_ENV_NS_TABLES: return q_type_is_table(v) || q_type_is_keyed(v);
    default:              return !q_type_is_fn(v);   /* `\v` keeps tables */
    }
}

int q_env_name_cmp(const void* a, const void* b) {
    const char* pa; size_t la;
    const char* pb; size_t lb;
    ray_t* sa = name_str(*(const int64_t*)a, &pa, &la);
    ray_t* sb = name_str(*(const int64_t*)b, &pb, &lb);
    int c = 0;
    if (sa && sb) {
        c = memcmp(pa, pb, la < lb ? la : lb);
        if (!c) c = (la > lb) - (la < lb);
    }
    if (sa) ray_release(sa);
    if (sb) ray_release(sb);
    return c;
}

/* d's own names as a fresh sym vector, the marker never among them.  MEMBERS
 * are picked by value kind and sorted (`\v` `\f` `\a`); the NAMESPACE roster
 * keeps creation order and drops `z` (basics/syscmds.md §`\d`) plus `duckdb`
 * (C-bound bridge ns — kdb has no counterpart, so `key `` ` `` stays kdb-shaped). */
static ray_t* env_dict_names(ray_t* d, q_env_ns_kind_t kind, int members) {
    ray_t* dk = ray_dict_keys(d);
    ray_t* dv = ray_dict_vals(d);
    int64_t n = ray_dict_len(d);
    if (!dk || dk->type != RAY_SYM || !dv || dv->type != RAY_LIST) return NULL;
    int64_t* sel = (int64_t*)ray_sys_alloc(sizeof(int64_t) * (size_t)(n > 0 ? n : 1));
    if (!sel) return NULL;
    ray_t** vs = (ray_t**)ray_data(dv);
    int64_t m = 0;
    int64_t marker = env_marker(), zed = ray_sym_intern_runtime("z", 1);
    int64_t duck = ray_sym_intern_runtime("duckdb", 6);
    for (int64_t i = 0; i < n; i++) {
        int64_t id = ray_read_sym(ray_data(dk), i, RAY_SYM, dk->attrs);
        if (id == marker) continue;
        int keep = members ? env_kind_match(kind, vs[i])
                           : (env_is_marked(vs[i]) && id != zed && id != duck);
        if (keep) sel[m++] = id;
    }
    if (members) qsort(sel, (size_t)m, sizeof *sel, q_env_name_cmp);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, m > 0 ? m : 1);
    for (int64_t i = 0; i < m && out && !RAY_IS_ERR(out); i++)
        out = ray_vec_append(out, &sel[i]);
    ray_sys_free(sel);
    return out;
}

ray_t* q_env_ns_names(int64_t ns_sym, q_env_ns_kind_t kind) {
    ray_t* d = ns_sym ? q_env_get(ns_sym) : env_root;
    if (!env_is_marked(d)) return NULL;
    return env_dict_names(d, kind, 1);
}

ray_t* q_env_ns_roster(void) {
    return env_ns ? env_dict_names(env_ns, Q_ENV_NS_VARS, 0) : NULL;
}

/* ---- lifecycle + context ---- */

void q_env_ctx_set(int64_t ns_sym) {
    if (ns_sym == g_ctx) return;
    g_ctx = ns_sym;
    g_ctx_seg = 0;
    if (!ns_sym) return;
    const char* p; size_t n;
    ray_t* s = name_str(ns_sym, &p, &n);
    if (!s) return;
    /* one level below root only (`\d` enforces it — q4m3 §12.7) */
    if (n > 1 && p[0] == '.' && !memchr(p + 1, '.', n - 1))
        g_ctx_seg = ray_sym_intern_runtime(p + 1, n - 1);
    ray_release(s);
}

int64_t q_env_ctx(void) { return g_ctx; }

ray_err_t q_env_init(void) {
    if (env_root) return RAY_OK;
    env_root = env_marker_dict();
    env_ns   = env_marker_dict();
    env_boot = env_marker_dict();
    if (!env_root || !env_ns || !env_boot) {
        q_env_destroy();
        return RAY_ERR_OOM;
    }
    return RAY_OK;
}

void q_env_destroy(void) {
    while (g_fdepth > 0) q_env_frame_pop();
    if (g_frames) { ray_sys_free(g_frames); g_frames = NULL; g_fcap = 0; }
    g_fview = Q_ENV_FRAME_VIEW_OFF;
    if (env_root) { ray_release(env_root); env_root = NULL; }
    if (env_ns)   { ray_release(env_ns);   env_ns   = NULL; }
    if (env_boot) { ray_release(env_boot); env_boot = NULL; }
    g_ctx = g_ctx_seg = 0;
}

/* q `nam set y` (ref/get.md) — assign a global through a symbol handle.  It is
 * `:`-identical by construction: the file handle is the ONE thing a
 * source-level assignment cannot spell.  This wrapper keeps ONLY the
 * name-vs-path classification (env knowledge); every path-shaped target —
 * `:f, the (file;lbs;alg;lvl) compression form, the (dir;sympath) domain
 * overload — goes to q_io_set, which owns the on-disk-format classification.
 * Returns the handle (kdb returns nam). */
ray_t* q_setg_wrap(ray_t* x, ray_t* y) {
    if (x && x->type == -RAY_SYM && !q_io_is_fsym(x)) {
        ray_t* s = ray_sym_str(x->i64);
        if (!s || ray_str_len(s) == 0) {   /* the empty sym is neither name nor path */
            if (s) ray_release(s);
            return q_err(QE_TYPE);
        }
        ray_release(s);
        ray_err_t err = q_env_set(x->i64, y);
        if (err != RAY_OK) return q_env_err(err);
        ray_retain(x);
        return x;
    }
    return q_io_set(x, y);
}
