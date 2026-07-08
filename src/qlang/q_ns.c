/* q_ns — see q_ns.h.  Context state + views over the engine's namespace
 * dicts (env_set_dotted).  All heavy lifting (auto-create, nested walk,
 * delete cascade) already lives in src/lang/env.c; this module is the q
 * presentation layer and the `\d` state machine. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_ns.h"
#include "qlang/q_ops.h"      /* q_ops_is_reserved */
#include "qlang/q_deriv.h"    /* q_deriv_kind_of — function-value detection */
#include "lang/env.h"         /* ray_env_get/list/list_user */
#include "lang/eval.h"        /* ray_at_fn */
#include <stdlib.h>
#include <string.h>

/* ---- `\d` state ----------------------------------------------------------- */

/* Current context name (".jab") or "" at root.  One level below root only,
 * so a fixed buffer is plenty. */
static char g_ctx[64];

void q_ns_reset(void) { g_ctx[0] = '\0'; }

const char* q_ns_current(void) { return g_ctx; }

static int ns_ident_ok(const char* p, size_t len) {
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

ray_t* q_ns_switch(const char* name, size_t len) {
    if (len == 1 && name[0] == '.') {          /* `\d .` — back to root */
        g_ctx[0] = '\0';
        return NULL;
    }
    /* One level below root only (kdb limitation, q4m3 §12.7): `.ident`. */
    if (len >= 2 && len < sizeof g_ctx && name[0] == '.' &&
        ns_ident_ok(name + 1, len - 1)) {
        memcpy(g_ctx, name, len);
        g_ctx[len] = '\0';
        return NULL;
    }
    /* kdb signals the offending name itself: `\d .jab.util` -> '.jab.util */
    char cls[64];
    snprintf(cls, sizeof cls, "%.*s", (int)(len < 63 ? len : 63), name);
    return ray_error(cls, NULL);
}

int q_ns_prompt(char* buf, size_t cap) {
    int n = snprintf(buf, cap, "q%s)", g_ctx);
    return (n < 0 || (size_t)n >= cap) ? 0 : n;
}

/* ---- qualification skip list ---------------------------------------------- */

/* Snapshot of user-defined global sym ids (borrowed values ignored). */
#define NS_MAX_GLOBALS 4096

static int ns_id_is_user(int64_t id) {
    static int64_t ids[NS_MAX_GLOBALS];
    static ray_t*  vals[NS_MAX_GLOBALS];
    int32_t n = ray_env_list_user(ids, vals, NS_MAX_GLOBALS);
    for (int32_t i = 0; i < n; i++)
        if (ids[i] == id) return 1;
    return 0;
}

int q_ns_is_unqualifiable(const char* s, size_t len) {
    /* q reserved words from the single-source manifest. */
    if (q_ops_is_reserved(s, (int)len)) return 1;
    /* control / qSQL statement words + `self` (recursion) — syntax, not env. */
    static const char* words[] = {
        "if", "do", "while", "self",
        "select", "exec", "update", "delete", "from", "by",
    };
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        if (strlen(words[i]) == len && memcmp(words[i], s, len) == 0)
            return 1;
    /* BUILTIN env bindings (q keywords implemented as env fns — parse, type,
     * not, mod, … — plus every rayfall builtin): a builtin-bound name keeps
     * resolving as itself under any context; only USER globals qualify. */
    int64_t id = ray_sym_intern(s, len);
    if (ray_env_get(id) && !ns_id_is_user(id)) return 1;
    return 0;
}

/* ---- helpers over env / context dicts ------------------------------------- */

static const char* const NS_SEEDS[] = { "q", "Q", "o", "h" };
#define NS_NSEEDS ((int)(sizeof NS_SEEDS / sizeof *NS_SEEDS))

static int ns_is_seed(const char* p, size_t len) {
    for (int i = 0; i < NS_NSEEDS; i++)
        if (strlen(NS_SEEDS[i]) == len && memcmp(NS_SEEDS[i], p, len) == 0)
            return 1;
    return 0;
}

/* The env value for a context handle — root (".foo") or NESTED (".fee.fi",
 * codex round-3 P2): ray_env_get walks the dotted path through the engine's
 * namespace dicts, so nested handles resolve the same way.  The value counts
 * as a CONTEXT dict iff it is a RAY_DICT with SYMBOL keys — env_set_dotted
 * always builds sym-keyed dicts, while a DATA dict assigned into a context
 * usually is not (`.foo.a:1 2!3 4` must stay a plain dict, ref/key.md pin).
 * A sym-keyed DATA dict is indistinguishable without kdb's physically-stored
 * `` ` ``->`::` entry — documented divergence (PR #88 Decisions).  Names in
 * the `..name` root-qualified spelling are never context handles.  Returns
 * borrowed, or NULL when not a context. */
static ray_t* ns_ctx_env_dict(const char* dotname, size_t len) {
    if (len < 2 || dotname[0] != '.' || dotname[1] == '.') return NULL;
    ray_t* v = ray_env_get(ray_sym_intern(dotname, len));
    if (!v || v->type != RAY_DICT) return NULL;
    ray_t* k = ray_dict_keys(v);                /* borrowed */
    if (!k || k->type != RAY_SYM) return NULL;  /* context dicts are sym-keyed */
    return v;
}

int q_ns_is_context(const char* dotname, size_t len) {
    return ns_ctx_env_dict(dotname, len) != NULL;
}

/* Append one (sym id, value) pair to parallel builders.  Returns 0 on OOM. */
static int ns_pair_append(ray_t** keys, ray_t** vals, int64_t id, ray_t* v) {
    *keys = ray_vec_append(*keys, &id);
    if (!*keys || RAY_IS_ERR(*keys)) return 0;
    *vals = ray_list_append(*vals, v);          /* append RETAINS v */
    if (!*vals || RAY_IS_ERR(*vals)) return 0;
    return 1;
}

ray_t* q_ns_root_dict(void) {
    static int64_t ids[NS_MAX_GLOBALS];
    static ray_t*  uvals[NS_MAX_GLOBALS];
    int32_t n = ray_env_list_user(ids, uvals, NS_MAX_GLOBALS);
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    ray_t* vals = ray_list_new(n > 0 ? n : 1);
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals))
        return ray_error("wsfull", "get: out of memory");
    for (int32_t i = 0; i < n; i++) {
        ray_t* s = ray_sym_str(ids[i]);         /* table-owned */
        if (!s) continue;
        const char* p = ray_str_ptr(s);
        if (!p || ray_str_len(s) == 0 || p[0] == '.') continue; /* contexts out */
        if (!ns_pair_append(&keys, &vals, ids[i], uvals[i]))
            return ray_error("wsfull", "get: out of memory");
    }
    return ray_dict_new(keys, vals);            /* consumes both */
}

ray_t* q_ns_ctx_dict(const char* dotname, size_t len) {
    ray_t* d = ns_ctx_env_dict(dotname, len);   /* borrowed or NULL */
    if (!d && !(len >= 2 && dotname[0] == '.' &&
                ns_is_seed(dotname + 1, len - 1)))
        return NULL;                            /* no such context */
    int64_t n = d ? ray_dict_len(d) : 0;
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, n + 1);
    ray_t* vals = ray_list_new(n + 1);
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals))
        return ray_error("wsfull", "get: out of memory");
    /* leading `` ` `` -> `::` placeholder (q4m3: keeps the value list general) */
    int64_t empty = ray_sym_intern("", 0);
    if (!ns_pair_append(&keys, &vals, empty, RAY_NULL_OBJ))
        return ray_error("wsfull", "get: out of memory");
    if (d) {
        ray_t* dk = ray_dict_keys(d);           /* borrowed */
        ray_t* dv = ray_dict_vals(d);           /* borrowed */
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* k = ray_at_fn(dk, ia);       /* owned sym atom */
            ray_t* v = ray_at_fn(dv, ia);       /* owned */
            ray_release(ia);
            if (!k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v)) {
                if (k && !RAY_IS_ERR(k)) ray_release(k);
                if (v && !RAY_IS_ERR(v)) ray_release(v);
                ray_release(keys); ray_release(vals);
                return ray_error("type", "get: bad context dict");
            }
            int ok = ns_pair_append(&keys, &vals, k->i64, v);
            ray_release(k);
            ray_release(v);
            if (!ok) return ray_error("wsfull", "get: out of memory");
        }
    }
    return ray_dict_new(keys, vals);            /* consumes both */
}

ray_t* q_ns_key_roster(void) {
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, 8);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int i = 0; i < NS_NSEEDS; i++) {
        int64_t id = ray_sym_intern(NS_SEEDS[i], strlen(NS_SEEDS[i]));
        out = ray_vec_append(out, &id);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    }
    static int64_t ids[NS_MAX_GLOBALS];
    static ray_t*  uvals[NS_MAX_GLOBALS];
    int32_t n = ray_env_list_user(ids, uvals, NS_MAX_GLOBALS);
    for (int32_t i = 0; i < n; i++) {
        if (!uvals[i] || uvals[i]->type != RAY_DICT) continue;
        ray_t* s = ray_sym_str(ids[i]);         /* table-owned */
        if (!s) continue;
        const char* p = ray_str_ptr(s);
        size_t l = ray_str_len(s);
        if (!p || l < 2 || p[0] != '.') continue;
        if (memchr(p + 1, '.', l - 1)) continue;   /* root contexts only */
        if (ns_is_seed(p + 1, l - 1)) continue;    /* already listed */
        int64_t bare = ray_sym_intern(p + 1, l - 1);
        out = ray_vec_append(out, &bare);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    }
    return out;
}

/* ---- \v \f \a member enumeration ------------------------------------------ */

typedef enum { NS_LIST_VARS, NS_LIST_FNS, NS_LIST_TABLES } ns_list_kind;

static int ns_is_fn_value(ray_t* v) {
    if (!v) return 0;
    if (q_deriv_kind_of(v) != Q_DERIV_NONE) return 1;
    return v->type == RAY_LAMBDA || v->type == RAY_UNARY ||
           v->type == RAY_BINARY || v->type == RAY_VARY;
}

static int ns_is_table_value(ray_t* v) {
    if (!v) return 0;
    if (v->type == RAY_TABLE) return 1;
    if (v->type == RAY_DICT) {                  /* keyed table */
        ray_t* k = ray_dict_keys(v);
        ray_t* w = ray_dict_vals(v);
        return k && w && k->type == RAY_TABLE && w->type == RAY_TABLE;
    }
    return 0;
}

static int ns_kind_match(ns_list_kind kind, ray_t* v) {
    switch (kind) {
    case NS_LIST_FNS:    return ns_is_fn_value(v);
    case NS_LIST_TABLES: return ns_is_table_value(v);
    default:             return !ns_is_fn_value(v);
    }
}

static int ns_name_cmp(const void* a, const void* b) {
    ray_t* sa = ray_sym_str(*(const int64_t*)a);
    ray_t* sb = ray_sym_str(*(const int64_t*)b);
    const char* pa = sa ? ray_str_ptr(sa) : "";
    const char* pb = sb ? ray_str_ptr(sb) : "";
    size_t la = sa ? ray_str_len(sa) : 0;
    size_t lb = sb ? ray_str_len(sb) : 0;
    size_t m = la < lb ? la : lb;
    int c = memcmp(pa, pb, m);
    if (c) return c;
    return (la > lb) - (la < lb);
}

/* Members of context `ns` ("" = root) matching kind, sorted ascending, as a
 * RAY_SYM vector.  A named context that does not exist (and is not a seed)
 * yields NULL — the caller errors with the name. */
static ray_t* ns_members(const char* ns, size_t nslen, ns_list_kind kind,
                         int missing_ok) {
    int64_t sel[NS_MAX_GLOBALS];
    int nsel = 0;
    if (nslen == 0) {                            /* root: user non-dotted */
        static int64_t ids[NS_MAX_GLOBALS];
        static ray_t*  uvals[NS_MAX_GLOBALS];
        int32_t n = ray_env_list_user(ids, uvals, NS_MAX_GLOBALS);
        for (int32_t i = 0; i < n && nsel < NS_MAX_GLOBALS; i++) {
            ray_t* s = ray_sym_str(ids[i]);
            const char* p = s ? ray_str_ptr(s) : NULL;
            if (!p || ray_str_len(s) == 0 || p[0] == '.') continue;
            if (ns_kind_match(kind, uvals[i])) sel[nsel++] = ids[i];
        }
    } else {
        ray_t* d = ns_ctx_env_dict(ns, nslen);   /* borrowed or NULL */
        if (!d) {
            int seed = (nslen >= 2 && ns[0] == '.' &&
                        ns_is_seed(ns + 1, nslen - 1));
            if (!seed && !missing_ok) return NULL;
            /* seed or current-but-uncreated: empty listing */
        } else {
            ray_t* dk = ray_dict_keys(d);
            ray_t* dv = ray_dict_vals(d);
            int64_t n = ray_dict_len(d);
            for (int64_t i = 0; i < n && nsel < NS_MAX_GLOBALS; i++) {
                ray_t* ia = ray_i64(i);
                ray_t* k = ray_at_fn(dk, ia);    /* owned sym atom */
                ray_t* v = ray_at_fn(dv, ia);    /* owned */
                ray_release(ia);
                if (k && !RAY_IS_ERR(k) && v && !RAY_IS_ERR(v) &&
                    k->type == -RAY_SYM && ns_kind_match(kind, v))
                    sel[nsel++] = k->i64;
                if (k && !RAY_IS_ERR(k)) ray_release(k);
                if (v && !RAY_IS_ERR(v)) ray_release(v);
            }
        }
    }
    qsort(sel, (size_t)nsel, sizeof sel[0], ns_name_cmp);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, nsel > 0 ? nsel : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int i = 0; i < nsel; i++) {
        out = ray_vec_append(out, &sel[i]);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    }
    return out;
}

/* ---- system-command dispatch ---------------------------------------------- */

ray_t* q_ns_syscmd(const char* line, size_t n, int* handled) {
    *handled = 0;
    size_t i = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= n || line[i] != '\\') return NULL;
    i++;
    if (i >= n) return NULL;
    char cmd = line[i++];
    if (cmd != 'd' && cmd != 'v' && cmd != 'f' && cmd != 'a') return NULL;
    if (i < n && line[i] != ' ' && line[i] != '\t') return NULL; /* \foo etc. */
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    /* first token = the optional namespace argument; the rest of the line is
     * ignored (transcripts carry trailing `/ comments`). */
    size_t a0 = i;
    while (i < n && line[i] != ' ' && line[i] != '\t') i++;
    const char* arg = line + a0;
    size_t alen = i - a0;
    if (alen > 0 && arg[0] == '/') alen = 0;     /* bare comment, no arg */

    *handled = 1;

    if (cmd == 'd') {
        if (alen == 0) {                         /* `\d` — show current */
            const char* c = q_ns_current();
            ray_t* s = (*c) ? ray_sym(ray_sym_intern(c, strlen(c)))
                            : ray_sym(ray_sym_intern(".", 1));
            /* DATA sym, not a name-ref: keeps its backtick in q_fmt (`.) */
            if (s && !RAY_IS_ERR(s)) s->attrs |= 0x20; /* Q_ATTR_QUOTED */
            return s;
        }
        return q_ns_switch(arg, alen);           /* NULL (silent) or error */
    }

    /* \v \f \a [ns] */
    ns_list_kind kind = (cmd == 'v') ? NS_LIST_VARS
                       : (cmd == 'f') ? NS_LIST_FNS : NS_LIST_TABLES;
    const char* ns; size_t nslen; int missing_ok;
    if (alen > 0) {
        if (alen == 1 && arg[0] == '.') { ns = ""; nslen = 0; missing_ok = 1; }
        else { ns = arg; nslen = alen; missing_ok = 0; }
    } else {
        ns = q_ns_current(); nslen = strlen(ns); missing_ok = 1;
    }
    ray_t* out = ns_members(ns, nslen, kind, missing_ok);
    if (!out) {                                  /* named ns doesn't exist */
        char cls[64];
        snprintf(cls, sizeof cls, "%.*s", (int)(alen < 63 ? alen : 63), arg);
        return ray_error(cls, NULL);             /* kdb: `\a .n` -> '.n */
    }
    return out;
}
