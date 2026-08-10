/* q_provider — see q_provider.h.  Grammar v2: connection form
 * `:pq:<ds>:<alias>:<config...>` (config VERBATIM, colons allowed, no
 * trailing slash) vs table form `:pq:<ds>:<alias>:<config...>:<table>/` —
 * the trailing slash IS the table marker (the splay `:dir/` law), and a
 * coordinate is syntactically committed to ONE role.  Names (ds, alias,
 * hooks, tables) obey R1 [a-zA-Z][a-zA-Z0-9_]*; empty alias = aliasless.
 * The handle int is a RESERVED real fd (dup of /dev/null), collision-free
 * in the one q_handles space by construction.  Hooks are resolved at CALL
 * time from the live q env; a hook's error propagates unmodified. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/io/q_provider.h"
#include "qlang/io/q_handles.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"     /* q_type_is_int_atom — the R4 timeout check */
#include "qlang/q_env.h"
#include "qlang/q_prim.h"          /* q_str_text_bytes, q_meta_fn */
#include "qlang/q_builtins.h"      /* q_count_fn — the count fallback */
#include "qlang/q_registry.h"      /* q_registry_provenance — op value -> its spelling */
#include "qlang/eval/q_eval.h"     /* q_eval_apply_value — THE apply seam */
#include "lang/eval.h"             /* ray_eval_get_restricted */
#include "table/sym.h"             /* ray_sym_intern_runtime, ray_sym_str */
#include <rayforce.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>          /* close — releasing the reserved fd */

#define PROV_MAX_ARGS 8
#define PROV_NAME_MAX 256

typedef struct { const char* p; size_t n; } seg_t;

typedef struct {
    int64_t fd;         /* the reserved handle int (q_handles key) */
    int64_t provider;   /* sym id */
    int64_t alias;      /* sym id (never the empty name for a registered row) */
    ray_t*  open3;      /* owned (rest;timeout;config) triad — never exposed;
                         * the host-owned-reconnect groundwork */
    ray_t*  connid;     /* owned: whatever .provider.open returned */
} prov_ent;

static prov_ent* g_ents = NULL;
static int64_t   g_cap  = 0;
static int64_t   g_n    = 0;
static int64_t   g_empty_sym = -1;

void q_provider_init(void) {
    g_ents = NULL; g_cap = 0; g_n = 0;
    g_empty_sym = -1;
}

void q_provider_destroy(void) {
    for (int64_t i = 0; i < g_n; i++) {
        close((int)g_ents[i].fd);      /* teardown: no hooks, just the fd + refs */
        if (g_ents[i].open3)  ray_release(g_ents[i].open3);
        if (g_ents[i].connid) ray_release(g_ents[i].connid);
    }
    free(g_ents);
    g_ents = NULL; g_cap = 0; g_n = 0;
}

static int64_t empty_sym(void) {
    if (g_empty_sym < 0) g_empty_sym = ray_sym_intern_runtime("", 0);
    return g_empty_sym;
}

static prov_ent* find_fd(int64_t fd) {
    for (int64_t i = 0; i < g_n; i++) if (g_ents[i].fd == fd) return &g_ents[i];
    return NULL;
}

static prov_ent* find_alias(int64_t provider, int64_t alias) {
    if (alias == empty_sym()) return NULL;
    for (int64_t i = 0; i < g_n; i++)
        if (g_ents[i].provider == provider && g_ents[i].alias == alias)
            return &g_ents[i];
    return NULL;
}


static int ci_eq(char c, char l) { return c == l || c == (char)(l - 'a' + 'A'); }

int q_provider_spec_is(const char* s, size_t n) {
    return s && n >= 4 && s[0] == ':' && ci_eq(s[1], 'p') && ci_eq(s[2], 'q')
             && s[3] == ':';
}

int q_provider_ns_is(const char* s, size_t n) {
    if (!s) return 0;
    if (n && s[0] == ':') { s++; n--; }        /* the hsym leading colon */
    if (n < 2 || !ci_eq(s[0], 'p') || !ci_eq(s[1], 'q')) return 0;
    return n == 2 || s[2] == ':';              /* "pq" or "pq:..." */
}

/* R1: [a-zA-Z][a-zA-Z0-9_]* */
static int name_ok(const char* p, size_t n) {
    if (!n || !((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')))
        return 0;
    for (size_t i = 1; i < n; i++) {
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

typedef struct {
    seg_t ds, alias, cfg, tbl;   /* tbl.p == NULL -> connection form */
    int   is_table;
} spec_t;

/* Parse + validate a coordinate.  NULL on success, owned 'domain otherwise.
 * Table form: iff the text ends `/`, the final `:<name>/` is the table and
 * the verbatim middle (possibly empty) is config. */
static ray_t* spec_parse(const char* s, size_t n, spec_t* sp) {
    memset(sp, 0, sizeof *sp);
    if (!q_provider_spec_is(s, n)) return q_err(QE_DOMAIN);
    size_t i = 4;
    sp->ds.p = s + 4;
    while (i < n && s[i] != ':') i++;
    sp->ds.n = i - 4;
    if (!name_ok(sp->ds.p, sp->ds.n)) return q_err(QE_DOMAIN);
    if (i < n) {
        sp->alias.p = s + ++i;
        size_t a0 = i;
        while (i < n && s[i] != ':') i++;
        sp->alias.n = i - a0;
        if (sp->alias.n && !name_ok(sp->alias.p, sp->alias.n))
            return q_err(QE_DOMAIN);           /* empty alias = aliasless */
    }
    if (i < n) { sp->cfg.p = s + i + 1; sp->cfg.n = n - i - 1; }
    if (s[n - 1] == '/') {                     /* ds/alias cannot hold '/', so
                                                * the slash implies cfg exists */
        sp->is_table = 1;
        size_t m = sp->cfg.n ? sp->cfg.n - 1 : 0;   /* drop the marker */
        size_t k = m;
        while (k > 0 && sp->cfg.p[k - 1] != ':') k--;
        sp->tbl.p = sp->cfg.p + k;
        sp->tbl.n = m - k;
        if (k == 0) { sp->cfg.p = NULL; sp->cfg.n = 0; }
        else       { sp->cfg.n = k - 1; }
        if (!name_ok(sp->tbl.p, sp->tbl.n)) return q_err(QE_DOMAIN);
    }
    if (sp->cfg.p && sp->cfg.n == 0)           /* present-but-empty = absent */
        sp->cfg.p = NULL;
    return NULL;
}

static int sym_text(ray_t* x, const char** p, size_t* n) {
    if (!x || x->type != -RAY_SYM) return 0;
    ray_t* s = ray_sym_str(x->i64);            /* borrowed */
    if (!s) return 0;
    *p = ray_str_ptr(s); *n = ray_str_len(s);
    return 1;
}

int q_provider_coord_sym_is(ray_t* x) {
    const char* p; size_t n;
    return sym_text(x, &p, &n) && q_provider_spec_is(p, n);
}

/* 0 = not a `:pq:` sym, 1 = connection form, 2 = table form (trailing '/') */
int q_provider_coord_sym_form(ray_t* x) {
    const char* p; size_t n;
    if (!sym_text(x, &p, &n) || !q_provider_spec_is(p, n)) return 0;
    return (n > 4 && p[n - 1] == '/') ? 2 : 1;
}


/* ".<provider>.<hook>" interned; -1 when the names do not fit */
static int64_t hook_sym(int64_t provider, const char* hook) {
    ray_t* ps = ray_sym_str(provider);         /* borrowed */
    if (!ps) return -1;
    char buf[PROV_NAME_MAX];
    int m = snprintf(buf, sizeof buf, ".%.*s.%s",
                     (int)ray_str_len(ps), ray_str_ptr(ps), hook);
    if (m <= 0 || m >= (int)sizeof buf) return -1;
    return ray_sym_intern_runtime(buf, (size_t)m);
}

/* the hook's current value — owned fn or NULL when undefined */
static ray_t* hook_fn(int64_t provider, const char* hook) {
    int64_t s = hook_sym(provider, hook);
    if (s < 0) return NULL;
    ray_t* v = q_env_resolve(s);
    if (v && RAY_IS_ERR(v)) { ray_error_free(v); return NULL; }
    return v;
}

/* undefined hook = the ordinary name error for `.provider.hook` */
static ray_t* hook_name_err(int64_t provider, const char* hook) {
    int64_t s = hook_sym(provider, hook);
    ray_t* ps = s >= 0 ? ray_sym_str(s) : NULL; /* borrowed */
    if (!ps) return q_err(QE_NAME);
    return q_err_name(ray_str_ptr(ps), ray_str_len(ps));
}

static ray_t* hook_call(int64_t provider, const char* hook, ray_t** args, int64_t n) {
    ray_t* f = hook_fn(provider, hook);
    if (!f) return hook_name_err(provider, hook);
    ray_t* r = q_eval_apply_value(f, args, n);
    ray_release(f);
    return r;
}


/* every open form normalizes here: the FROZEN triad (rest; timeout|0N;
 * config|::) — owned 3-list, the alias record keeps it beside CONNID */
static ray_t* open3_make(seg_t rest, ray_t* timeout, ray_t* config) {
    ray_t* r = ray_list_new(3);
    if (!r || RAY_IS_ERR(r)) return r ? r : q_err(QE_OOM);
    ray_t* c = ray_charv(rest.p ? rest.p : "", (int64_t)rest.n);
    if (!c || RAY_IS_ERR(c)) { ray_release(r); return c ? c : q_err(QE_OOM); }
    r = ray_list_append(r, c);
    ray_release(c);
    ray_t* t = timeout ? timeout : ray_i64(NULL_I64);
    if (!RAY_IS_ERR(r)) r = ray_list_append(r, t);
    if (!timeout) ray_release(t);
    if (!RAY_IS_ERR(r)) r = ray_list_append(r, config ? config : RAY_NULL_OBJ);
    return r;
}

/* .provider.open with the triad's three SEPARATE args (arity frozen at 3) */
static ray_t* conn_open(int64_t provider, ray_t* open3, ray_t** connid_out) {
    *connid_out = NULL;
    ray_t* c = hook_call(provider, "open", (ray_t**)ray_data(open3), 3);
    if (!c || RAY_IS_ERR(c)) return c ? c : q_err(QE_TYPE);
    *connid_out = c;
    return NULL;
}

static void conn_close(int64_t provider, ray_t* connid) {
    ray_t* f = hook_fn(provider, "close");     /* optional: no-op if undefined */
    if (!f) return;
    ray_t* r = q_eval_apply_value(f, &connid, 1);
    ray_release(f);
    if (r) ray_release(r);
}

ray_t* q_provider_hopen(const char* s, size_t n, ray_t* timeout, ray_t* config) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    spec_t sp;
    ray_t* pe = spec_parse(s, n, &sp);
    if (pe) return pe;
    if (sp.is_table) return q_err(QE_DOMAIN);  /* a table is never a connection */
    /* R4 — the HOST validates the open tuple, so providers never see junk */
    if (timeout && !q_type_is_int_atom(timeout)) return q_err(QE_TYPE);
    if (config && !RAY_IS_NULL(config) && config->type != RAY_DICT)
        return q_err(QE_TYPE);
    int64_t pid = ray_sym_intern_runtime(sp.ds.p, sp.ds.n);
    int64_t aid = sp.alias.n ? ray_sym_intern_runtime(sp.alias.p, sp.alias.n)
                             : empty_sym();
    ray_t* o3 = open3_make(sp.cfg, timeout, config);
    if (RAY_IS_ERR(o3)) return o3;
    prov_ent* live = find_alias(pid, aid);
    if (live) {
        /* re-point the live alias: close old CONNID, open new, swap — the
         * url-moved-don't-lose-tables affordance; carriers never notice */
        conn_close(pid, live->connid);
        ray_release(live->connid); live->connid = NULL;
        ray_release(live->open3);  live->open3 = NULL;
        ray_t* c = NULL;
        ray_t* e = conn_open(pid, o3, &c);
        if (e) {                               /* open failed: the alias dies */
            ray_release(o3);
            int64_t fd = live->fd;
            close((int)fd);
            q_handles_deregister(fd);
            *live = g_ents[--g_n];
            return e;
        }
        live->connid = c;
        live->open3  = o3;
        return ray_i64(live->fd);
    }
    int fd = q_handles_reserve_fd();
    if (fd < 0) { ray_release(o3); return q_err(QE_IO); }
    /* only ":pq:ds:alias" is registered — the config (credentials, urls)
     * dies here (hopen is where secrets die) */
    size_t redlen = (size_t)((sp.alias.p ? sp.alias.p + sp.alias.n
                                         : sp.ds.p + sp.ds.n) - s);
    if (!q_handles_register(fd, Q_HANDLE_PROVIDER, 1, s, redlen)) {
        close(fd);
        ray_release(o3);
        return q_err(QE_OOM);
    }
    ray_t* c = NULL;
    ray_t* e = conn_open(pid, o3, &c);
    if (e) {
        q_handles_deregister(fd);
        close(fd);
        ray_release(o3);
        return e;
    }
    if (g_n == g_cap) {
        int64_t nc = g_cap ? g_cap * 2 : 8;
        prov_ent* ne = (prov_ent*)realloc(g_ents, (size_t)nc * sizeof *ne);
        if (!ne) {
            conn_close(pid, c);
            ray_release(c);
            q_handles_deregister(fd);
            close(fd);
            ray_release(o3);
            return q_err(QE_OOM);
        }
        g_ents = ne; g_cap = nc;
    }
    g_ents[g_n].fd       = fd;
    g_ents[g_n].provider = pid;
    g_ents[g_n].alias    = aid;
    g_ents[g_n].open3    = o3;
    g_ents[g_n].connid   = c;
    g_n++;
    return ray_i64((int64_t)fd);
}

int q_provider_info(int64_t fd, int64_t* provider, int64_t* alias) {
    prov_ent* e = find_fd(fd);
    if (!e) return 0;
    *provider = e->provider;
    *alias    = e->alias;
    return 1;
}

ray_t* q_provider_close(int64_t qh) {
    prov_ent* e = find_fd(qh);
    if (e) {
        conn_close(e->provider, e->connid);
        ray_release(e->connid);
        if (e->open3) ray_release(e->open3);
        close((int)e->fd);
        *e = g_ents[--g_n];
    }
    q_handles_deregister(qh);
    return RAY_NULL_OBJ;
}


/* THE connection-position resolution (every position but hopen).  The alias
 * slot marks the form: a NON-EMPTY alias is a REFERENCE — config is
 * inexpressible there (alias followed by more segments = 'domain), live ->
 * use, dead -> 'conn (a reference NEVER self-heals into an anonymous
 * connection).  An EMPTY alias is a SPEC — the config IS the identity: a
 * TEMPORARY connection per use (open -> use -> close, NOTHING registered —
 * aliases are born only in hopen).  Caller closes via cref_close. */
typedef struct { prov_ent* e; prov_ent tmp; int is_tmp; } cref_t;

static ray_t* cref_open(const spec_t* sp, cref_t* c) {
    memset(c, 0, sizeof *c);
    int64_t pid = ray_sym_intern_runtime(sp->ds.p, sp->ds.n);
    if (sp->alias.n) {                         /* REFERENCE */
        if (sp->cfg.p) return q_err(QE_DOMAIN);
        c->e = find_alias(pid, ray_sym_intern_runtime(sp->alias.p, sp->alias.n));
        return c->e ? NULL : q_err(QE_CONN);
    }
    ray_t* o3 = open3_make(sp->cfg, NULL, NULL);   /* SPEC: temp connection */
    if (RAY_IS_ERR(o3)) return o3;
    ray_t* conn = NULL;
    ray_t* err = conn_open(pid, o3, &conn);
    ray_release(o3);
    if (err) return err;
    c->tmp = (prov_ent){ .fd = -1, .provider = pid, .alias = empty_sym(),
                         .open3 = NULL, .connid = conn };
    c->e = &c->tmp;
    c->is_tmp = 1;
    return NULL;
}

static void cref_close(cref_t* c) {
    if (!c->is_tmp) return;
    conn_close(c->tmp.provider, c->tmp.connid);
    ray_release(c->tmp.connid);
    c->is_tmp = 0;
}


/* carrier `cols!`:pq:ds:alias:name/ — the splay dict shape, slash-marked */
static ray_t* carrier_make(int64_t provider, int64_t alias, int64_t name, ray_t* cols) {
    ray_t* ps = ray_sym_str(provider);         /* borrowed x3 */
    ray_t* as = ray_sym_str(alias);
    ray_t* ns = ray_sym_str(name);
    if (!ps || !as || !ns) return q_err(QE_TYPE);
    char buf[PROV_NAME_MAX];
    int m = snprintf(buf, sizeof buf, ":pq:%.*s:%.*s:%.*s/",
                     (int)ray_str_len(ps), ray_str_ptr(ps),
                     (int)ray_str_len(as), ray_str_ptr(as),
                     (int)ray_str_len(ns), ray_str_ptr(ns));
    if (m <= 0 || m >= (int)sizeof buf) return q_err(QE_DOMAIN);
    ray_retain(cols);
    return ray_dict_new(cols, ray_sym(ray_sym_intern_runtime(buf, (size_t)m)));
}

/* .X.bind[connid; name] -> advisory column names -> the carrier */
static ray_t* prov_bind(prov_ent* e, ray_t* name) {
    ray_t* args[2] = { e->connid, name };
    ray_t* cols = hook_call(e->provider, "bind", args, 2);
    if (!cols || RAY_IS_ERR(cols)) return cols ? cols : q_err(QE_TYPE);
    if (cols->type != RAY_SYM) { ray_release(cols); return q_err(QE_TYPE); }
    ray_t* car = carrier_make(e->provider, e->alias, name->i64, cols);
    ray_release(cols);
    return car;
}

/* the raw-op-to-English-name law: a sym head IS the hook name; an operator
 * value converts via its registry provenance — the ONE spelling-of-value home
 * (a glyph spelling like "#" then fails the R1 hook check, by design) */
static const char* head_hook_name(ray_t* h, char* buf, size_t bufn) {
    if (h && h->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(h->i64);        /* borrowed; "" fails name_ok */
        if (!s || ray_str_len(s) >= bufn) return NULL;
        memcpy(buf, ray_str_ptr(s), ray_str_len(s));
        buf[ray_str_len(s)] = '\0';
        return buf;
    }
    if (h && (h->type == RAY_UNARY || h->type == RAY_BINARY || h->type == RAY_VARY)) {
        q_provenance_t pv;
        return q_registry_provenance(h, &pv) ? pv.spelling : NULL;
    }
    return NULL;
}

/* `h (HEAD; args...)` — .provider.<name>[connid; args...] */
static ray_t* prov_hook_list(prov_ent* e, ray_t* y) {
    int64_t m = ray_len(y);
    if (m < 1 || m > PROV_MAX_ARGS) return q_err(QE_RANK);
    ray_t* owned[PROV_MAX_ARGS] = { NULL };
    ray_t* args[PROV_MAX_ARGS];
    args[0] = e->connid;
    ray_t* head;
    if (y->type == RAY_SYM) {                  /* (`get;`t) collapses typed */
        head = owned[0] = ray_sym(ray_vec_get_sym_id(y, 0));
        for (int64_t i = 1; i < m; i++)
            args[i] = owned[i] = ray_sym(ray_vec_get_sym_id(y, i));
    } else {
        ray_t** el = (ray_t**)ray_data(y);
        head = el[0];
        for (int64_t i = 1; i < m; i++) args[i] = el[i];
    }
    char nbuf[PROV_NAME_MAX];
    const char* hook = head_hook_name(head, nbuf, sizeof nbuf);
    ray_t* r;
    if (!hook) r = q_err(QE_TYPE);
    /* R1 kills dotted traversal (`i.exec`); `i` is the internals subspace and
     * lifecycle (open/close) is HOST-only — never the generic list door */
    else if (!name_ok(hook, strlen(hook)) || strcmp(hook, "i") == 0 ||
             strcmp(hook, "open") == 0 || strcmp(hook, "close") == 0)
        r = q_err(QE_DOMAIN);
    else r = hook_call(e->provider, hook, args, m);
    for (int64_t i = 0; i < PROV_MAX_ARGS; i++)
        if (owned[i]) ray_release(owned[i]);
    return r;
}

static ray_t* prov_dispatch(prov_ent* e, ray_t* y, int sync) {
    const char* tp; int64_t tn;
    if (y && q_str_text_bytes(y, &tp, &tn)) {  /* text -> .X.call */
        ray_t* txt = ray_charv(tp, tn);
        ray_t* flag = ray_bool(sync != 0);
        ray_t* args[3] = { e->connid, txt, flag };
        ray_t* r = hook_call(e->provider, "call", args, 3);
        ray_release(txt);
        ray_release(flag);
        return r;
    }
    if (y && y->type == -RAY_SYM) {            /* `get (h;`t)` reaches here */
        const char* bp; size_t bn;
        if (!sym_text(y, &bp, &bn) || !name_ok(bp, bn)) return q_err(QE_DOMAIN);
        if (e->alias == empty_sym()) return q_err(QE_NYI);  /* bind needs an alias */
        return prov_bind(e, y);
    }
    if (y && (y->type == RAY_LIST || y->type == RAY_SYM))
        return prov_hook_list(e, y);
    return q_err(QE_TYPE);
}

ray_t* q_provider_apply(int64_t qh, ray_t* y) {
    prov_ent* e = find_fd(qh < 0 ? -qh : qh);
    if (!e) return q_err(QE_TYPE);
    return prov_dispatch(e, y, qh > 0);
}

ray_t* q_provider_sym_apply(ray_t* head, ray_t** args, int64_t n) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    if (n != 1) return q_err(QE_RANK);
    const char* sp; size_t sl;
    spec_t s;
    if (!sym_text(head, &sp, &sl)) return q_err(QE_DOMAIN);
    ray_t* pe = spec_parse(sp, sl, &s);
    if (pe) return pe;
    if (s.is_table) return q_err(QE_DOMAIN);   /* a table is never a connection */
    cref_t c;                                  /* live (config-checked) or one-shot */
    ray_t* e = cref_open(&s, &c);
    if (e) return e;
    ray_t* r = prov_dispatch(c.e, args[0], 1);
    cref_close(&c);
    return r;
}


int q_provider_carrier_is(ray_t* x) {
    if (!x || x->type != RAY_DICT) return 0;
    ray_t* k = ray_dict_keys(x);
    ray_t* v = ray_dict_vals(x);
    if (!k || !v || v->type != -RAY_SYM || k->type != RAY_SYM) return 0;
    const char* p; size_t n;
    return sym_text(v, &p, &n) && q_provider_spec_is(p, n)
           && n > 4 && p[n - 1] == '/';        /* carriers carry the table marker */
}

/* a table reference = the connection resolution + the table name */
typedef struct { cref_t c; int64_t name; } tref_t;

static ray_t* tref_open(const char* p, size_t n, tref_t* t) {
    memset(t, 0, sizeof *t);
    spec_t sp;
    ray_t* pe = spec_parse(p, n, &sp);
    if (pe) return pe;
    if (!sp.is_table) return q_err(QE_DOMAIN); /* a connection is never a table */
    t->name = ray_sym_intern_runtime(sp.tbl.p, sp.tbl.n);
    return cref_open(&sp, &t->c);
}

static void tref_close(tref_t* t) { cref_close(&t->c); }

/* .X.<hook>[connid; name] on the resolved ref */
static ray_t* tref_hook(tref_t* t, const char* hook) {
    ray_t* nm = ray_sym(t->name);
    ray_t* args[2] = { t->c.e->connid, nm };
    ray_t* r = hook_call(t->c.e->provider, hook, args, 2);
    ray_release(nm);
    return r;
}

static ray_t* tref_hook_opt(tref_t* t, const char* hook) {  /* NULL if undefined */
    ray_t* f = hook_fn(t->c.e->provider, hook);
    if (!f) return NULL;
    ray_t* nm = ray_sym(t->name);
    ray_t* args[2] = { t->c.e->connid, nm };
    ray_t* r = q_eval_apply_value(f, args, 2);
    ray_release(f);
    ray_release(nm);
    return r;
}

static ray_t* carrier_tref(ray_t* car, tref_t* t) {
    const char* p; size_t n;
    if (!sym_text(ray_dict_vals(car), &p, &n)) return q_err(QE_DOMAIN);
    return tref_open(p, n, t);
}

ray_t* q_provider_carrier_table(ray_t* car) {
    tref_t t;
    ray_t* err = carrier_tref(car, &t);
    if (err) return err;
    ray_t* r = tref_hook(&t, "get");
    tref_close(&t);
    return r;
}

ray_t* q_provider_carrier_count(ray_t* car) {
    tref_t tr;
    ray_t* err = carrier_tref(car, &tr);
    if (err) return err;
    ray_t* r = tref_hook_opt(&tr, "count");
    if (!r) {
        ray_t* t = tref_hook(&tr, "get");      /* host fallback: materialize */
        if (t && !RAY_IS_ERR(t)) { r = q_count_fn(t); ray_release(t); }
        else r = t ? t : q_err(QE_TYPE);
    }
    tref_close(&tr);
    return r;
}

ray_t* q_provider_carrier_meta(ray_t* car) {
    tref_t tr;
    ray_t* err = carrier_tref(car, &tr);
    if (err) return err;
    ray_t* r = tref_hook_opt(&tr, "meta");
    if (!r) {
        ray_t* t = tref_hook(&tr, "get");
        if (t && !RAY_IS_ERR(t)) { r = q_meta_fn(t); ray_release(t); }
        else r = t ? t : q_err(QE_TYPE);
    }
    tref_close(&tr);
    return r;
}

/* `get `:pq:ds:al[:cfg]:t/` — the carrier, splay symmetry: cols via bind on
 * the live (or temporary) connection; the carrier keeps the coordinate
 * VERBATIM so a self-contained one stays self-contained.  NULL = not `:pq:. */
ray_t* q_provider_get_carrier(ray_t* x) {
    const char* p; size_t n;
    if (!sym_text(x, &p, &n) || !q_provider_spec_is(p, n)) return NULL;
    tref_t tr;
    ray_t* err = tref_open(p, n, &tr);         /* connection form -> 'domain */
    if (err) return err;
    ray_t* cols = tref_hook(&tr, "bind");
    tref_close(&tr);
    if (!cols || RAY_IS_ERR(cols)) return cols ? cols : q_err(QE_TYPE);
    if (cols->type != RAY_SYM) { ray_release(cols); return q_err(QE_TYPE); }
    return ray_dict_new(cols, ray_sym(x->i64));
}

/* the from-slot: carrier value, table-form hsym, or a name resolving to a
 * carrier.  0 = not a provider table; a connection-form `:pq:` sym in a
 * table position fills err_out with 'domain. */
static int from_tref(ray_t* t, tref_t* tr, ray_t** err_out) {
    *err_out = NULL;
    const char* p; size_t n;
    if (q_provider_carrier_is(t)) {
        *err_out = carrier_tref(t, tr);
        return 1;
    }
    int form = q_provider_coord_sym_form(t);
    if (form == 2) {
        sym_text(t, &p, &n);
        *err_out = tref_open(p, n, tr);
        return 1;
    }
    if (form == 1) { *err_out = q_err(QE_DOMAIN); return 1; }
    if (t && t->type == -RAY_SYM) {            /* functional `?[`t;...]` names */
        ray_t* v = q_env_resolve(t->i64);
        if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); return 0; }
        int is = q_provider_carrier_is(v);
        if (is) *err_out = carrier_tref(v, tr);
        ray_release(v);
        return is;
    }
    return 0;
}

ray_t* q_provider_from_table(ray_t* t) {
    tref_t tr; ray_t* err;
    if (!from_tref(t, &tr, &err)) return NULL;
    if (err) return err;
    ray_t* r = tref_hook(&tr, "get");
    tref_close(&tr);
    return r;
}

/* the provider-truth column list for .X.qsql — ONE live bind round-trip at
 * push time (a carrier's embedded keys are advisory and can be stale after
 * remote schema drift — pushing on them substitutes a same-named client
 * global for a NEW column); NULL = unavailable (host fallback) */
static ray_t* qsql_cols(tref_t* tr) {
    ray_t* cols = tref_hook(tr, "bind");
    if (cols && RAY_IS_ERR(cols)) { ray_error_free(cols); return NULL; }
    if (cols && cols->type == RAY_SYM) return cols;
    if (cols) ray_release(cols);
    return NULL;
}

ray_t* q_provider_qsql_push(ray_t** args, int64_t n) {
    tref_t tr; ray_t* err;
    if (n < 4 || n > PROV_MAX_ARGS) return NULL;
    if (!from_tref(args[0], &tr, &err)) return NULL;
    if (err) return err;
    ray_t* f = hook_fn(tr.c.e->provider, "qsql");
    if (!f) { tref_close(&tr); return NULL; }  /* host fallback: the residual law */
    ray_t* cols = qsql_cols(&tr);
    if (!cols) { ray_release(f); tref_close(&tr); return NULL; }
    ray_t* tree = ray_list_new(n);
    ray_t* nm = ray_sym(tr.name);
    tree = ray_list_append(tree, nm);          /* slot 0: the BARE underlying name */
    ray_release(nm);
    for (int64_t i = 1; i < n && !RAY_IS_ERR(tree); i++)
        tree = ray_list_append(tree, args[i]);
    if (RAY_IS_ERR(tree)) { ray_release(f); ray_release(cols); tref_close(&tr); return tree; }
    ray_t* cargs[3] = { tr.c.e->connid, cols, tree };
    ray_t* r = q_eval_apply_value(f, cargs, 3);
    ray_release(f);
    ray_release(cols);
    ray_release(tree);
    tref_close(&tr);
    if (r && RAY_IS_NULL(r)) { ray_release(r); return NULL; }  /* hook declined */
    return r;
}


ray_t* q_provider_write(ray_t* x, ray_t* y, int upsert) {
    const char* p; size_t n;
    if (!sym_text(x, &p, &n) || !q_provider_spec_is(p, n)) return NULL;
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    tref_t tr;
    ray_t* err = tref_open(p, n, &tr);         /* the slash-marked table form */
    if (err) return err;
    ray_t* nm = ray_sym(tr.name);
    ray_t* args[3] = { tr.c.e->connid, nm, y };
    ray_t* r = hook_call(tr.c.e->provider, upsert ? "upsert" : "set", args, 3);
    ray_release(nm);
    tref_close(&tr);
    return r;
}
