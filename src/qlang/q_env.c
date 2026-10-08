/* q_env — q's K-tree as NESTED DICTS.  See q_env.h for the model.
 *
 * The two NAME-FACING verbs live here rather than in ops/: `key` and `set`
 * read and write the tree itself (root roster, context members), so their
 * bodies belong beside the tree, not beside the table kernels their dict arms
 * happen to touch. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_count.h"
#include "qlang/q_env.h"
#include "qlang/q_registry_internal.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"     /* q_type_is_table / _is_keyed — the \v|\a split */
#include "qlang/q_builtins.h"      /* q_type_is_fn — the \f split (needs the apply module) */
#include "qlang/q_comment.h"          /* the armed doc header's one consumption point */
#include "qlang/eval/q_view.h"    /* view hooks: set/unbind invalidation, dot-'nyi */
#include "qlang/io/q_io.h"        /* q_io_set — `set`'s file half */
#include "qlang/io/q_provider.h"  /* the link seam: q_provider_carrier_is, _link/_unlink — the HOST, never a provider */
#include "qlang/q_pq.h"           /* q_pq_autoload / _autoload_ns — a first `.pq` or missed library reference loads its file */
#include "qlang/q_prim.h"         /* q_enum_deref — FK/link dotted-walk gather */
#include "lang/internal.h"        /* ray_error */
#include "table/sym.h"         /* ray_sym_intern_runtime, ray_sym_str, ray_read_sym */
#include "table/dict.h"        /* ray_dict_* probes/upsert */
#include "ops/linkop.h"        /* ray_link_has / ray_link_deref */
#include "ops/temporal.h"      /* ray_temporal_accessor — the dotted accessor roster */
#include "mem/sys.h"
#include <stdlib.h>            /* qsort — the member listings are sorted */
#include <string.h>

#define ENV_NAME_MAX 256
#define ENV_SEG_MAX 64
#define ENV_CTX_MAX 16
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
/* A context is the ns sym as given (`.a.b`) plus its segment PATH (`a`,`b`), walked down from env_ns.  Deriving the
 * path once per switch keeps a k-level context to k dict probes and NO sym-table round trip (#359). */
typedef struct { int n; int64_t seg[ENV_CTX_MAX]; } env_path_t;
static int64_t    g_ctx, g_scope;       /* the session `\d`; a running lambda's defining context */
static env_path_t g_ctx_path, g_scope_path;
static int        g_scoped;
#define ENV_CTX (g_scoped ? g_scope : g_ctx)
#define ENV_PATH (g_scoped ? &g_scope_path : &g_ctx_path)
#define ENV_DEPTH (ENV_PATH->n)
static int64_t g_pq_seg;                /* `pq`: the one namespace that loads itself */
static int64_t g_q_seg;                 /* `q`: the namespace whose builtin entries are locked */

/* `.pq` autoloads on its first reference, read or write (owner 2026-09-20) — THE
 * hook, at the seam every name crosses: a dotted `.pq…` name, or a relative one
 * under `\d .pq`.  q_pq_autoload holds the once guard (re-entrant references from
 * pq.q itself answer NULL), so a loaded `.pq` costs one prefix compare. */
static ray_t* env_pq_hook(const char* p, size_t n) {
    int hit = p[0] == '.' ? n >= 3 && p[1] == 'p' && p[2] == 'q' && (n == 3 || p[3] == '.')
                          : ENV_DEPTH && ENV_PATH->seg[0] == g_pq_seg;
    return hit ? q_pq_autoload() : NULL;
}

/* Every other library namespace autoloads on a MISSED read of a dotted name (owner 2026-09-25): a found name never
 * gets here, so a user's definition wins and core `.j` still gains the library's `.j`. */
static ray_t* env_lib_hook(const char* p, size_t n) {
    if (n < 2 || p[0] != '.' || p[1] == '.') return NULL;
    size_t k = 1;
    while (k < n && p[k] != '.') k++;
    return q_pq_autoload_ns(p + 1, k - 1);
}

static int64_t env_marker(void) { return ray_sym_intern_runtime("", 0); }

/* q_env_marker_sym — see q_env.h. */
int64_t q_env_marker_sym(void) { return env_marker(); }

ray_t* q_env_marker_dict(void) {
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
    return k && k->type == RAY_SYM && q_count(k) >= 1 &&
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

static void ctx_path_of(int64_t ns_sym, env_path_t* path) {
    path->n = 0;
    if (!ns_sym) return;
    const char* p; size_t n;
    ray_t* s = name_str(ns_sym, &p, &n);
    if (!s) return;
    if (n > 1 && p[0] == '.' && q_env_ctx_ok(p + 1, n - 1))
        path->n = env_segs(p, n, 1, path->seg, ENV_CTX_MAX);
    if (path->n < 0) path->n = 0;
    ray_release(s);
}

static ray_t* ctx_home(void) {
    const env_path_t* c = ENV_PATH;
    ray_t* v = env_ns;
    for (int i = 0; v && i < c->n; i++) v = ray_dict_probe_sym_borrowed(v, c->seg[i]);
    return v;
}

static size_t env_start(const char* p, size_t n) {
    return p[0] == '.' ? ((n > 1 && p[1] == '.') ? 2 : 1) : 0;
}

/* `\d .a.b` re-roots a RELATIVE name: the context path becomes its first steps, so a write lands in — and thereby
 * creates — the namespace and its intermediates.  Returns the new segment count (-1 past ENV_SEG_MAX) and re-points
 * *home; k unchanged at root. */
static int ctx_reroot(int64_t* segs, int k, ray_t*** home) {
    const env_path_t* c = ENV_PATH;
    if (!c->n || k <= 0) return k;
    if (k + c->n > ENV_SEG_MAX) return -1;
    memmove(segs + c->n, segs, (size_t)k * sizeof *segs);
    memcpy(segs, c->seg, (size_t)c->n * sizeof *segs);
    *home = &env_ns;
    return k + c->n;
}

static ray_t* env_get(int64_t sym, int load) {
    if (!env_ns) return NULL;
    const char* p; size_t n;
    ray_t* s = name_str(sym, &p, &n);
    if (!s) return NULL;
    ray_t* v = NULL;
    if (n == 1 && p[0] == '.') v = env_root;
    else if (n > 0) {
        ray_t* e = load ? env_pq_hook(p, n) : NULL;
        if (e) ray_release(e);               /* the load displayed it; a borrowed probe answers absence */
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
        ray_t* le = !v && load ? env_lib_hook(p, n) : NULL;
        if (le) { ray_release(le); v = env_get(sym, 0); }
    }
    ray_release(s);
    return v;
}

ray_t* q_env_get(int64_t sym)  { return env_get(sym, 1); }
ray_t* q_env_peek(int64_t sym) { return env_get(sym, 0); }

/* ---- assignment: path-copy amend down the dict chain ---- */

/* The provider carrier a leaf amend or remove displaces, retained: the link seam's unlink reads it back out
 * (q_env_set / q_env_unbind).  One probe + one mark test per write is the whole cost to an ordinary assignment. */
static ray_t* env_carrier_of(ray_t* holder, int64_t seg) {
    ray_t* prev = ray_dict_probe_sym_borrowed(holder, seg);
    if (!prev || !q_provider_carrier_is(prev)) return NULL;
    ray_retain(prev);
    return prev;
}

/* Consumes d; owned result, NULL on failure.  Missing ancestors conjure
 * marked dicts (`.fee.fi.fo:42` creates `.fee`, `.fee.fi`); an existing
 * non-dict (or unreachable collapsed slot) intermediate refuses. */
static ray_t* env_amend(ray_t* d, const int64_t* segs, int nseg, int i, ray_t* val, ray_t** disp) {
    ray_t* k = ray_sym(segs[i]);
    if (!k || RAY_IS_ERR(k)) {
        ray_release(d);
        if (k) ray_release(k);
        return NULL;
    }
    ray_t* r;
    if (i == nseg - 1) {
        if (disp) *disp = env_carrier_of(d, segs[i]);
        r = ray_dict_upsert(d, k, val);
    } else {
        ray_t* child = ray_dict_probe_sym_borrowed(d, segs[i]);
        ray_t* sub = NULL;
        if (child && child->type == RAY_DICT) { ray_retain(child); sub = child; }
        else if (!child && ray_dict_find_sym(d, segs[i]) < 0) sub = q_env_marker_dict();
        if (!sub) {
            ray_release(d);
            ray_release(k);
            return NULL;
        }
        ray_t* nd = env_amend(sub, segs, nseg, i + 1, val, disp);
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
static ray_err_t env_store(ray_t** home, const int64_t* segs, int nseg, ray_t* val, ray_t** disp) {
    if (!*home) return RAY_ERR_DOMAIN;
    ray_retain(*home);
    ray_t* nd = env_amend(*home, segs, nseg, 0, val, disp);
    if (!nd) return RAY_ERR_DOMAIN;
    ray_release(*home);
    *home = nd;
    return RAY_OK;
}

/* a write of `.q` whole is judged by its effect: each builtin entry keeps its very object (NULL drops them all) */
static int env_q_changes_builtin(ray_t* nq) {
    ray_t* q = ray_dict_probe_sym_borrowed(env_ns, g_q_seg);
    ray_t* qk = q ? ray_dict_keys(q) : NULL;
    for (int64_t i = 0; qk && i < q_count(qk); i++) {
        int64_t key = ray_read_sym(ray_data(qk), i, RAY_SYM, qk->attrs);
        if (!q_registry_locked(key)) continue;
        if (!nq || nq->type != RAY_DICT || ray_dict_probe_sym_borrowed(nq, key) != ray_dict_probe_sym_borrowed(q, key))
            return 1;
    }
    return 0;
}

/* Judged by the TARGET after `\d` re-rooting — a root reserved word or a `.q` builtin entry, and `.q` itself by
 * effect; a colon assignment's bare SPELLING is refused upstream in q_eval (owner ruling 2026-09-23). */
static int env_locked(ray_t** home, const int64_t* segs, int k, ray_t* val) {
    if (home != &env_ns) return k == 1 && q_registry_locked(segs[0]);
    if (segs[0] != g_q_seg) return 0;
    return k == 1 ? env_q_changes_builtin(val) : q_registry_locked(segs[1]);
}

/* the dict a name's segments are keyed under — a relative name re-rooted by `\d`, a new plain name into the shed */
static ray_t** env_home(const char* p, size_t start, int64_t* segs, int* k, int boot_new) {
    if (p[0] == '.') return start == 1 ? &env_ns : &env_root;
    ray_t** home = &env_root;
    *k = ctx_reroot(segs, *k, &home);
    if (*k <= 0) return NULL;
    if (home == &env_root && ray_dict_find_sym(env_root, segs[0]) < 0 &&
        (ray_dict_find_sym(env_boot, segs[0]) >= 0 || boot_new))
        home = &env_boot;
    return home;
}

static ray_err_t env_put(int64_t sym, ray_t* val, int boot_new, ray_t** disp) {
    const char* p; size_t n;
    ray_t* s = name_str(sym, &p, &n);
    if (!s) return RAY_ERR_DOMAIN;
    ray_err_t e = RAY_ERR_DOMAIN;
    int64_t segs[ENV_SEG_MAX];
    if (n > 0 && !(n == 1 && p[0] == '.')) {
        size_t start = env_start(p, n);
        int k = env_segs(p, n, start, segs, ENV_SEG_MAX);
        ray_t** home = k > 0 ? env_home(p, start, segs, &k, boot_new) : NULL;
        if (home && !env_locked(home, segs, k, val)) e = env_store(home, segs, k, val, disp);
    }
    ray_release(s);
    return e;
}

/* a bind is a park or a bootstrap write, invisible to the link seam as it is to views */
ray_err_t q_env_bind(int64_t sym, ray_t* val) {
    return env_put(sym, val, 1, NULL);
}

typedef struct { int64_t name; ray_t* fn; int rank; } env_native_t;
static env_native_t* g_natives;
static int           g_nnat, g_natcap;

ray_err_t q_env_bind_native(const char* name, ray_t* fn, int rank) {
    int64_t sym = ray_sym_intern_runtime(name, strlen(name));
    int i = 0;
    while (i < g_nnat && g_natives[i].name != sym) i++;
    if (i < g_nnat && g_natives[i].fn->type == fn->type && g_natives[i].fn->i64 == fn->i64 && g_natives[i].rank == rank)
        return q_env_bind(sym, g_natives[i].fn);
    if (i == g_natcap) {
        int nc = g_natcap ? 2 * g_natcap : 64;
        env_native_t* nn = (env_native_t*)ray_sys_alloc(sizeof *nn * (size_t)nc);
        if (!nn) return RAY_ERR_OOM;
        if (g_natives) { memcpy(nn, g_natives, sizeof *nn * (size_t)g_nnat); ray_sys_free(g_natives); }
        g_natives = nn;
        g_natcap = nc;
    }
    ray_retain(fn);
    if (i < g_nnat) ray_release(g_natives[i].fn);
    else g_nnat++;
    g_natives[i] = (env_native_t){ sym, fn, rank };
    return q_env_bind(sym, fn);
}

int64_t q_env_native_name(ray_t* fn, int* rank) {
    for (int i = 0; i < g_nnat; i++)
        if (g_natives[i].fn == fn) { *rank = g_natives[i].rank; return g_natives[i].name; }
    return 0;
}

ray_t* q_env_native_get(const char* name, size_t n, int rank) {
    int64_t sym = ray_sym_find(name, n);
    for (int i = 0; sym >= 0 && i < g_nnat; i++)
        if (g_natives[i].name == sym && (rank < 0 || g_natives[i].rank == rank)) return g_natives[i].fn;
    return NULL;
}

/* the link seam: the carrier a write displaced, then the one it bound, under the name the write LANDED on */
static ray_err_t env_link(int64_t sym, ray_t* old, ray_t* val) {
    int64_t full = q_env_fullname(sym, NULL);
    ray_err_t e = old ? q_provider_unlink(full, old) : RAY_OK;
    if (e == RAY_OK && q_provider_carrier_is(val)) e = q_provider_link(full, val);
    return e;
}

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
    if ((ENV_DEPTH && relative) || q_env_get(sym) != cur) return 0;
    return q_env_bind(sym, RAY_NULL_OBJ) == RAY_OK;
}

ray_err_t q_env_settle(int64_t sym, int stole, ray_t* val) {
    ray_err_t e = q_env_set(sym, val);
    if (e != RAY_OK && stole) q_env_bind(sym, val);
    return e;
}

/* The scope switch as a saved triple: a handle op restores it byte-for-byte, because a `.z.vs` hook fired
 * mid-call runs a lambda whose exit resets the switch through q_env_scope. */
typedef struct { int64_t scope; env_path_t path; int scoped; } env_scope_t;

static void path_copy(env_path_t* d, const env_path_t* s) {
    d->n = s->n;
    memcpy(d->seg, s->seg, (size_t)s->n * sizeof *s->seg);
}

static env_scope_t env_scope_as(int64_t ctx, int scoped) {
    env_scope_t prev;
    prev.scope = g_scope;
    prev.scoped = g_scoped;
    path_copy(&prev.path, &g_scope_path);
    g_scope = ctx;
    ctx_path_of(ctx, &g_scope_path);
    g_scoped = scoped;
    return prev;
}

static void env_scope_restore(const env_scope_t* prev) {
    g_scope = prev->scope;
    path_copy(&g_scope_path, &prev->path);
    g_scoped = prev->scoped;
}

/* `` `. `` names the root itself, so assigning a dict RESTORES its members as
 * globals (ref/get.md `set`) — the ` -> :: marker is representation, skipped.
 * Members go back through q_env_set, so each gets the same name policy, AT THE
 * ROOT whatever `\d` or a lambda scope says (`` `. `` is absolute); a member
 * already bound to that very value is the amend's untouched node, not a write. */
static ray_err_t env_root_splat(ray_t* d) {
    if (!d || d->type != RAY_DICT) return RAY_ERR_TYPE;
    ray_t*  dk = ray_dict_keys(d);                  /* borrowed */
    ray_t*  dv = ray_dict_vals(d);                  /* borrowed */
    int64_t n = q_count(d), marker = env_marker();
    ray_err_t e = RAY_OK;
    env_scope_t sc = env_scope_as(0, 1);
    for (int64_t i = 0; i < n && e == RAY_OK; i++) {
        ray_t* k = q_join_item(dk, i);              /* owned */
        ray_t* v = q_join_item(dv, i);              /* owned */
        if (!k || RAY_IS_ERR(k) || k->type != -RAY_SYM || !v || RAY_IS_ERR(v))
            e = RAY_ERR_TYPE;
        else if (k->i64 != marker && q_env_get(k->i64) != v)
            e = q_env_set(k->i64, v);
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
    }
    env_scope_restore(&sc);
    return e;
}

/* THE one write home: every q form that binds a name (`:` `::`, indexed and
 * modified assign, `@`/`.` name-amend, `set`) lands here, so the whole TARGET
 * policy is stated once (a colon's spelling lock is q_eval's) — the `` `. `` root splat, and otherwise an ordinary
 * tree amend, settable `.z.*` handlers included: they are plain globals their
 * C fire sites read by name (src/core/ipc.c reads the connection six,
 * q_wirefile the `.z.zd` zip triple).  Both branches converge on `e` so that
 * EVERY successful bind is a definition an armed doc header can name. */
ray_err_t q_env_set(int64_t sym, ray_t* val) {
    if (!val) return q_env_unbind(sym);
    const char* p; size_t n;
    ray_t* s = name_str(sym, &p, &n);
    if (!s) return RAY_ERR_DOMAIN;
    int root = n == 1 && p[0] == '.';
    ray_t* le = n > 0 ? env_pq_hook(p, n) : NULL;
    if (le) ray_release(le);                 /* the load displayed it; the write itself still lands */
    ray_release(s);
    ray_err_t e;
    if (root) e = env_root_splat(val);
    else {
        ray_t* old = NULL;
        e = env_put(sym, val, 0, &old);
        if (e == RAY_OK) {
            q_view_on_global_set(sym);       /* invalidation + .z.vs */
            if (old || q_provider_carrier_is(val)) e = env_link(sym, old, val);
        }
        if (old) ray_release(old);
    }
    if (e == RAY_OK) q_comment_on_global_set(sym, val);
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
    ray_t* old = NULL;
    int64_t segs[ENV_SEG_MAX];
    if (n > 0 && !(n == 1 && p[0] == '.')) {
        size_t start = env_start(p, n);
        int k = env_segs(p, n, start, segs, ENV_SEG_MAX);
        ray_t** home = k > 0 ? env_home(p, start, segs, &k, 0) : NULL;
        if (home && env_locked(home, segs, k, NULL)) e = RAY_ERR_DOMAIN;
        else if (home) {
            ray_t* holder = *home;
            for (int i = 0; holder && i < k - 1; i++)
                holder = ray_dict_probe_sym_borrowed(holder, segs[i]);
            if (holder && ray_dict_find_sym(holder, segs[k - 1]) >= 0) {
                ray_t* key = ray_sym(segs[k - 1]);
                if (!key || RAY_IS_ERR(key)) {
                    if (key) ray_release(key);
                    e = RAY_ERR_OOM;
                } else {
                    old = env_carrier_of(holder, segs[k - 1]);
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
                        e = env_store(home, segs, k - 1, nd, NULL);
                        ray_release(nd);
                    }
                }
            }
        }
    }
    ray_release(s);
    if (e == RAY_OK) {
        q_view_on_global_unbind(sym);   /* dependents go pending */
        if (old) e = q_provider_unlink(q_env_fullname(sym, NULL), old);
    }
    if (old) ray_release(old);
    return e;
}

/* ---- the handle lane (q_env.h): the ordinary env operations forced to ROOT scope, because a handle names a global ---- */

/* the node a relative name's write LANDS on under `\d .ns` — ctx_reroot's read twin, so read-name == write-name
 * by construction; a dotted name and the root context read as q_env_get */
static ray_t* env_addr_get(int64_t sym) {
    const char* p; size_t n;
    ray_t* s = name_str(sym, &p, &n);
    if (!s) return NULL;
    ray_t* v;
    if (!ENV_DEPTH || n == 0 || p[0] == '.') v = q_env_get(sym);
    else {
        int64_t segs[ENV_SEG_MAX];
        int k = env_segs(p, n, 0, segs, ENV_SEG_MAX);
        v = k > 0 ? ctx_home() : NULL;
        for (int i = 0; v && i < k; i++) v = ray_dict_probe_sym_borrowed(v, segs[i]);
    }
    ray_release(s);
    return v;
}

ray_t* q_env_handle_get(int64_t sym) {
    env_scope_t sc = env_scope_as(0, 0);
    ray_t* v = env_addr_get(sym);
    env_scope_restore(&sc);
    return v;
}

ray_t* q_env_handle_resolve(int64_t sym) {
    env_scope_t sc = env_scope_as(0, 0);
    int32_t floor = q_env_frame_floor(-1);      /* a handle names a global: no local shadows it */
    ray_t* v = q_env_resolve(q_env_fullname(sym, NULL));
    q_env_frame_floor(floor);
    env_scope_restore(&sc);
    return v;
}

ray_err_t q_env_handle_set(int64_t sym, ray_t* val) {
    env_scope_t sc = env_scope_as(0, 0);
    ray_err_t e = q_env_set(sym, val);
    env_scope_restore(&sc);
    return e;
}

int q_env_handle_take(int64_t sym, ray_t* cur) {
    if (!cur) return 0;
    env_scope_t sc = env_scope_as(0, 0);
    int r = env_addr_get(sym) == cur && q_env_bind(sym, RAY_NULL_OBJ) == RAY_OK;
    env_scope_restore(&sc);
    return r;
}

ray_err_t q_env_handle_settle(int64_t sym, int stole, ray_t* val) {
    env_scope_t sc = env_scope_as(0, 0);
    ray_err_t e = q_env_settle(sym, stole, val);
    env_scope_restore(&sc);
    return e;
}

ray_err_t q_env_handle_bind(int64_t sym, ray_t* val) {
    env_scope_t sc = env_scope_as(0, 0);
    ray_err_t e = q_env_bind(sym, val);
    env_scope_restore(&sc);
    return e;
}

int q_env_ns_exists(int64_t path_sym) { return env_is_marked(q_env_get(path_sym)); }

ray_t* q_env_ns_probe(int64_t ns_sym, const char* member, size_t n) {
    int64_t id = ray_sym_find(member, n);
    ray_t* d = id < 0 ? NULL : q_env_get(ns_sym);
    return d && d->type == RAY_DICT ? ray_dict_probe_sym_borrowed(d, id) : NULL;
}

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

/* `skip_scopes` stops ABOVE the first lambda scope (a barrier frame): no local ever carries a dotted spelling
 * (q_eval.c write_is_local refuses one), so a dotted name is global by construction and a parameter must not
 * shadow it — `{[a;i;b] i.gl[a;1]}` means the global `i.gl`.  qSQL column scopes are barrier-FREE and stay
 * visible, so `sym.name` foreign-key notation still reads a column. */
static ray_t* frames_lookup_at(int64_t sym, int skip_scopes) {
    if (g_fview == Q_ENV_FRAME_VIEW_NONE) return NULL;
    int32_t top = g_fdepth - 1;
    if (g_fview >= 0 && g_fview < top) top = g_fview;
    for (int32_t d = top; d >= g_ffloor; d--) {
        frame_t* f = &g_frames[d];
        if (skip_scopes && f->barrier) break;
        for (int32_t i = 0; i < f->n; i++)
            if (f->keys[i] == sym) return f->vals[i];
        if (f->barrier) break;
    }
    return NULL;
}

static ray_t* frames_lookup(int64_t sym) { return frames_lookup_at(sym, 0); }

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
        /* referential deref first — FK enums and link columns gather from
         * their q-env target (q_enum_deref); ray_link_deref stays as the
         * rayfall-env fallback for links whose target lives only there */
        next = q_enum_deref(v, seg);
        if (!next && ray_link_has(v)) next = ray_link_deref(v, seg);
        if (next && RAY_IS_ERR(next)) {
            if (fresh) ray_release(v);
            return next;
        }
        next_fresh = (next != NULL);
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

static ray_t* env_resolve(int64_t sym, int lib) {
    ray_t* v = frames_lookup(sym);
    if (v) { ray_retain(v); return v; }
    const char* p; size_t n;
    ray_t* s = name_str(sym, &p, &n);
    if (!s) return NULL;
    if (n == 1 && p[0] == '.' && env_root) {
        ray_release(s);
        ray_retain(env_root);
        return env_root;
    }
    if (n == 0 || (n == 1 && p[0] == '.')) { ray_release(s); return NULL; }
    ray_t* le = env_pq_hook(p, n);
    if (le) { ray_release(s); return le; }   /* the reference answers the load's abort */
    size_t start = env_start(p, n);
    size_t hend = start;
    while (hend < n && p[hend] != '.') hend++;
    if (hend == start) { ray_release(s); return NULL; }    /* ".."-style names */
    int64_t head = ray_sym_intern_runtime(p + start, hend - start);
    ray_t* base;
    if (p[0] == '.') {
        base = ray_dict_probe_sym_borrowed(start == 1 ? env_ns : env_root, head);
    } else {
        base = frames_lookup_at(head, hend < n);
        if (!base) {                                /* a global is BOUND to its context: no root search */
            ray_t* home = ENV_DEPTH ? ctx_home() : env_root;
            if (home) base = ray_dict_probe_sym_borrowed(home, head);
        }
        if (!base) base = ray_dict_probe_sym_borrowed(env_boot, head);
    }
    ray_t* r = NULL;
    if (base) {
        if (hend >= n) { ray_retain(base); r = base; }
        else r = walk_segs(base, 0, p, n, hend + 1);
    }
    ray_t* miss = r || !lib ? NULL : env_lib_hook(p, n);
    ray_release(s);
    if (miss && !RAY_IS_ERR(miss)) { ray_release(miss); return env_resolve(sym, 0); }
    return r ? r : miss;
}

ray_t* q_env_resolve(int64_t sym) { return env_resolve(sym, 1); }

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
            int64_t q = q_env_qualify(ENV_CTX, sym);
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
    case Q_ENV_NS_TABLES: return q_type_is_table(v) || q_type_is_keyed(v) || q_provider_carrier_is(v);
    default:              return !q_type_is_fn(v);   /* `\v` keeps tables */
    }
}

int q_env_ident_ok(const char* p, size_t len) {
    if (len == 0) return 0;
    if (!((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')))
        return 0;
    for (size_t i = 1; i < len; i++) {
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

int q_env_ctx_ok(const char* p, size_t len) {
    int depth = 0;
    for (size_t i = 0; i <= len; ) {
        size_t e = i;
        while (e < len && p[e] != '.') e++;
        if (++depth > ENV_CTX_MAX || !q_env_ident_ok(p + i, e - i)) return 0;
        i = e + 1;
    }
    return 1;
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
    int64_t n = q_count(d);
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
    ctx_path_of(ns_sym, &g_ctx_path);
}

int64_t q_env_ctx(void) { return g_ctx; }

int64_t q_env_scope(int64_t scope) {
    int64_t prev = g_scoped ? g_scope : Q_ENV_SCOPE_SESSION;
    if (scope == prev) return prev;         /* a same-namespace call derives no path */
    g_scoped = scope != Q_ENV_SCOPE_SESSION;
    g_scope = g_scoped ? scope : 0;
    ctx_path_of(g_scope, &g_scope_path);
    return prev;
}

int64_t q_env_scope_ctx(void) { return ENV_CTX; }

ray_err_t q_env_init(void) {
    if (env_root) return RAY_OK;
    env_root = q_env_marker_dict();
    env_ns   = q_env_marker_dict();
    env_boot = q_env_marker_dict();
    g_pq_seg = ray_sym_intern_runtime("pq", 2);
    g_q_seg  = ray_sym_intern_runtime("q", 1);
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
    for (int i = 0; i < g_nnat; i++) ray_release(g_natives[i].fn);
    if (g_natives) ray_sys_free(g_natives);
    g_natives = NULL;
    g_nnat = g_natcap = 0;
    g_ctx = g_scope = g_scoped = 0;
    g_ctx_path.n = g_scope_path.n = 0;
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
        ray_err_t err = q_env_handle_set(x->i64, y);
        if (err != RAY_OK) return q_env_err(err);
        ray_retain(x);
        return x;
    }
    return q_io_set(x, y);
}
