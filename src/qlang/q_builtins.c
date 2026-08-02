/* q_builtins — register q's own builtins into q's env (q_env), and install
 * the remote-eval hooks.  Rayfall's env is a bootstrap-only kernel catalogue:
 * capture_base and the registry snapshot it here, then q never consults it. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_builtins.h"
#include "qlang/base/q_err.h"
#include "qlang/eval/q_eval.h" /* q_eval / q_eval_apply_value — THE eval pipeline */
#include "qlang/parse/q_parse.h"
#include "qlang/eval/q_view.h"   /* q_view_intercept — `x::e` at the remote-source seam */
#include "qlang/net/q_http_client.h" /* .Q.c.hg / .Q.c.hp — outbound HTTP client */
#define Q_OPS_ENV_GRANDFATHER /* base-kernel snapshots (capture_base) read the bootstrap catalogue */
#include "qlang/q_registry_internal.h" /* q_registry_init (shared; brings q_registry.h) */
#include "qlang/q_env.h"      /* q_env_bind — q's own name surface */
#include "qlang/ops/q_sys.h"      /* q_system_fn — the q-owned `system` verb */
#include "qlang/q_console.h"  /* q_console_show — show's display sink */
#include "qlang/base/q_type.h"     /* q_type_of — the `type` verb's type-number home */
#include "qlang/ops/q_index.h" /* q_index_elem_at — the element-read home */
#include "lang/env.h"       /* ray_fn_unary; ray_env_get = bootstrap catalogue reads */
#include "lang/eval.h"      /* RAY_FN_NONE */
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
    if (!q_str_text_bytes(x, &sp, &sn)) return q_err(QE_TYPE);
    size_t sl = (size_t)sn;
    if (!sp) return q_err(QE_DOMAIN);
    char* src = malloc(sl + 1);
    if (!src) return q_err(QE_WSFULL);
    memcpy(src, sp, sl);
    src[sl] = '\0';
    ray_t* ast = q_parse(src);
    free(src);
    /* handed out as DATA: empty statements become () (kdb parse ";") */
    if (ast && !RAY_IS_ERR(ast)) q_ast_fill_empty_stmts(ast);
    return ast ? ast : q_err(QE_PARSE);
}


/* (show x) — print x's q console display as a SIDE EFFECT (buffered in the q
 * console sink; the host drains it), then return generic null.  Overrides
 * rayfall's `show`, which uses the rayfall formatter (`[1 2 3]`) rather than
 * q_fmt.  qdoc/repl print nothing for the null result, so the row shows only
 * the buffered display. */
static ray_t* show_fn(ray_t* x) {
    q_console_show(x);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

static ray_t* id_table(ray_t* x) {
    int64_t nc = ray_table_ncols(x);
    ray_t* out = ray_table_new(nc);
    int64_t stack[64];
    int64_t* used = (nc <= 64) ? stack : (int64_t*)malloc((size_t)nc * sizeof(int64_t));
    if (!used) { ray_release(out); return q_err(QE_WSFULL); }
    for (int64_t c = 0; c < nc; c++) {
        int64_t nm = q_registry_name_sanitize(ray_table_col_name(x, c));
        nm = q_name_dedup(nm, used, c, 1);
        used[c] = nm;
        out = ray_table_add_col(out, nm, ray_table_get_col_idx(x, c));
        if (!out || RAY_IS_ERR(out)) break;
    }
    if (used != stack) free(used);
    return out;
}

static ray_t* id_dict(ray_t* x) {
    ray_t* k = ray_dict_keys(x);
    ray_t* v = ray_dict_vals(x);
    if (!k || k->type != RAY_SYM)
        return q_err(QE_TYPE);
    int64_t n = ray_len(k);
    ray_t* nk = ray_sym_vec_new(RAY_SYM_W64, n);
    int64_t stack[64];
    int64_t* used = (n <= 64) ? stack : (int64_t*)malloc((size_t)n * sizeof(int64_t));
    if (!used) { ray_release(nk); return q_err(QE_WSFULL); }
    for (int64_t i = 0; i < n; i++) {
        ray_t* ks = ray_sym_vec_cell(k, i);
        int64_t id = ray_sym_intern_runtime(ray_str_ptr(ks), ray_str_len(ks));
        id = q_registry_name_sanitize(id);
        id = q_name_dedup(id, used, i, 1);
        used[i] = id;
        nk = ray_vec_append(nk, &id);
        if (!nk || RAY_IS_ERR(nk)) break;
    }
    if (used != stack) free(used);
    if (!nk || RAY_IS_ERR(nk)) return nk ? nk : q_err(QE_OOM);
    ray_retain(v);
    return ray_dict_new(nk, v);   /* consumes nk, retained v */
}

static ray_t* id_fn(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (x->type == -RAY_SYM) return ray_sym(q_registry_name_sanitize(x->i64));
    if (x->type == RAY_TABLE) return id_table(x);
    if (x->type == RAY_DICT)  return id_dict(x);
    return q_err(QE_TYPE);
}

/* ---- function-value introspection (stage 2): type / count / value ------- */

static ray_unary_fn g_base_type  = NULL;
static ray_unary_fn g_base_count = NULL;

/* q `type` verb: the type-number knowledge lives in q_type_of (the type
 * axis home); this arm keeps only the base-verb fallback for NULL x. */
static ray_t* type_fn(ray_t* x) {
    if (x) return ray_i16(q_type_of(x));
    return g_base_type ? g_base_type(x)
                       : q_err(QE_TYPE);
}

/* ---- table introspection (type-output feature) --------------------------- */
/* The tag -> type-char map is q_type_char (q_type.c, the type-axis home). */

/* True iff x is a uniform, matrix-mappable list of simple vectors of ONE
 * element type (kdb `.Q.ty` upper-case case, e.g. `3 2#3 4 5` or
 * `("abc";"de")`).  *elt receives that element tag (RAY_STR for a list of
 * char-vector strings). */
static int uniform_list(ray_t* x, int8_t* elt) {
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
char q_ty_char(ray_t* x) {
    int8_t elt;
    if (x && x->type == -RAY_STR) return 'c';                       /* char-vec shim */
    if (x && ray_is_vec(x))       return q_type_char((int8_t)x->type);
    if (uniform_list(x, &elt)) {
        char lc = q_type_char(elt);
        return (char)(lc ? (lc - 'a' + 'A') : ' ');
    }
    return ' ';
}


/* q `count` of any function value is 1 (kdb: functions are atoms) — a
 * carrier's raw len is its slot count, which must never leak. */
ray_t* q_count_fn(ray_t* x) {
    if (x && q_eval_apply_is_fn(x)) return ray_i64(1);
    /* a mapping's count is its DOMAIN's count — one rule for a plain dict and a
     * keyed table, whose domain is a table (borrowed ref: do not release). */
    if (x && x->type == RAY_DICT) return q_count_fn(ray_dict_keys(x));
    return g_base_count ? g_base_count(x)
                        : q_err(QE_TYPE);
}

/* C-long specialization of q_count_fn: the count as an int64 (-1 on error),
 * for callers that want the number rather than a q value.  Consumes the
 * intermediate count value. */
int64_t q_builtins_count_long(ray_t* x) {
    /* Allocation-free lane for the shapes the ITERATORS ask about: they call
     * this once per application, and applications nest, so building a count
     * value here costs a malloc per outer item (integration/i078 went 5.9s ->
     * 15.4s when the adverb arms started routing through the boxed path).
     * Same answers as q_count_fn below, which still owns every other shape. */
    if (x && !RAY_IS_ERR(x) && !q_eval_apply_is_fn(x)) {
        if (x->type == RAY_DICT)  return q_builtins_count_long(ray_dict_keys(x));
        if (x->type == RAY_TABLE) return ray_table_nrows(x);
        if (x->type == RAY_LIST || (ray_is_vec(x) && x->type != RAY_STR))
            return ray_len(x);
    }
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
    q_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

static void bind_vary(const char* name, ray_vary_fn fn) {
    ray_t* obj = ray_fn_vary(name, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

static void bind_value(const char* name, ray_t* val) {
    q_env_bind(ray_sym_intern(name, strlen(name)), val);
    ray_release(val);
}

/* Remote-source string eval (IPC request payloads, journal replay): the ONE
 * q pipeline, mirroring q_repl.c run_one_line — q_parse -> q_eval ->
 * materialize.  Buffered q console output (`show`, 0N!) is drained to the
 * SERVER's stdout after each eval (kdb prints show output on the server
 * console) and reset so it can't bleed into later requests.  Returns an
 * OWNED value; parse/eval errors return as owned errors (the IPC layer
 * serializes them as -128h responses). */
static ray_t* remote_eval_str(const char* src, size_t len) {
    /* A leading `\` is a system command, not q source: kdb runs a solo `\l`/`\p`
     * received on the wire.  `system"X"` is exactly `\X`, so strip the `\` and
     * reuse q_system_fn — single-homing q_sys_run, the restricted-mode guard,
     * and the `\`-shell stdout capture.  A loaded script's `show`/0N! output
     * is drained to the server console just like the q path. */
    if (len > 0 && src[0] == '\\') {
        ray_t* arg = ray_str(src + 1, len - 1);   /* the command minus its leading `\` */
        if (!arg) return q_err(QE_OOM);
        ray_t* r = q_system_fn(arg);
        ray_release(arg);
        { const char* con = q_console_str();
          if (con && *con) fputs(con, stdout);
          q_console_reset(); }
        return r;
    }
    char* tmp = (char*)ray_sys_alloc(len + 1);
    if (!tmp) return q_err(QE_OOM);
    memcpy(tmp, src, len);
    tmp[len] = '\0';
    ray_t* ast = q_parse(tmp);
    if (RAY_IS_ERR(ast)) { ray_sys_free(tmp); return ast; }
    /* A trailing assignment answers with the generic null, not the assigned
     * value (basics/ipc.md: `h"fn:{2+x}"` displays nothing) — the same
     * q_parse_is_assign judgment q_repl.c/qdoc.c make.  A view definition is
     * one, and is intercepted here for the same reason: remote source text is
     * a statement, so `h"z::b+c"` defines a view exactly as the line would. */
    ray_t* r;
    int is_assign = 1;
    if (!q_view_intercept(ast, tmp, &r)) {
        is_assign = q_parse_is_assign(ast);
        r = q_eval(ast);
    }
    ray_sys_free(tmp);
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
 * wire shape — a SINGLE list-apply of the head to the already-evaluated tail
 * args, NEVER re-evaluating them (ADR-0004: value, not eval), through the ONE
 * public apply entry (q_eval_apply_value).  A sym/string head resolves/parses
 * to its value first.  `list` is BORROWED (the ipc layer releases it); the
 * result is OWNED.  Restricted mode needs no re-assert here: ipc_dispatch
 * sets it around the whole dispatch, and any restricted primitive reached by
 * the apply self-checks it. */
/* keyword-HOF recipe stub (q_registry_internal.h note) */
ray_t* q_hof_nyi_wrap(ray_t* f, ray_t* x) {
    (void)f; (void)x;
    return q_err(QE_NYI);
}

static ray_t* remote_apply(ray_t* list) {
    if (!list || (list->type != RAY_LIST && !ray_is_vec(list)) ||
        ray_len(list) < 1)
        return q_err(QE_TYPE);
    int64_t n = ray_len(list);
    ray_t* head = q_index_elem_at(list, 0);            /* owned */
    if (!head || RAY_IS_ERR(head)) return head ? head : q_err(QE_TYPE);
    /* string-source head ("+", "{x*2}"): parse + eval to its value */
    if (head->type == -RAY_STR || head->type == RAY_CHARV) {
        const char* sp; int64_t sn;
        if (q_str_text_bytes(head, &sp, &sn)) {
            char* z = malloc((size_t)sn + 1);
            if (!z) { ray_release(head); return q_err(QE_WSFULL); }
            memcpy(z, sp, (size_t)sn);
            z[sn] = '\0';
            ray_t* ast = q_parse(z);
            free(z);
            ray_release(head);
            if (RAY_IS_ERR(ast)) return ast;
            head = q_eval(ast);
            ray_release(ast);
            if (RAY_IS_ERR(head)) return head;
        }
    } else if (head->type == -RAY_SYM) {   /* `sum -> its value (registry-first,
                                            * the evaluator's resolution order) */
        ray_t* v = q_eval_value_wrap(head);
        if (RAY_IS_ERR(v)) { ray_release(head); return v; }
        ray_release(head);
        head = v;
    }
    ray_t* argv[8];
    int64_t argc = n - 1;
    if (argc > 8) { ray_release(head); return q_err(QE_RANK); }
    for (int64_t i = 0; i < argc; i++) {
        argv[i] = q_index_elem_at(list, i + 1);        /* owned */
        if (!argv[i] || RAY_IS_ERR(argv[i])) {
            ray_t* err = argv[i];
            for (int64_t j = 0; j < i; j++) ray_release(argv[j]);
            ray_release(head);
            return err ? err : q_err(QE_TYPE);
        }
    }
    ray_t* r;
    if (argc == 0) {                                      /* (f) -> f[] = f@:: */
        ray_t* nil = RAY_NULL_OBJ;
        r = q_eval_apply_value(head, &nil, 1);
    } else {
        r = q_eval_apply_value(head, argv, argc);
    }
    if (r && ray_is_lazy(r)) r = ray_lazy_materialize(r);
    for (int64_t j = 0; j < argc; j++) ray_release(argv[j]);
    ray_release(head);
    return r;
}


void q_builtins_register(void) {
    /* Remote strings (IPC/journal) evaluate as q from now on. */
    ray_eval_set_remote_str_fn(remote_eval_str);
    /* Remote (func;args) value-apply (IPC): the one public apply entry. */
    ray_eval_set_remote_apply_fn(remote_apply);
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
    bind_unary("show",   show_fn);
    /* `system "…"` — q-owned string form; single-homes with the `\`-slash
     * dispatcher (q_sys.c). QK_ENV row in q_ops.c snapshots this binding. */
    bind_unary("system", q_system_fn);
    /* File Text companions (feat/q-file-text): `read0[(f;o)]` bracket calls
     * name-ref through the env (the ssr precedent — same C fn as the registry
     * row); `csv` is kdb's comma global (ref/file-text.md `csv 0: t`). */
    bind_unary("read0",  q_read0_wrap);
    bind_unary("read1",  q_read1_wrap);
    bind_unary("hdel",   q_hdel_wrap);
    bind_value("csv",    ray_char(','));
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
        ray_t* tv = ray_fn_unary("type", type_attrs, type_fn);
        q_env_bind(ray_sym_intern("type", 4), tv);
        ray_release(tv);
        ray_t* cv = ray_fn_unary("count", count_attrs, q_count_fn);
        q_env_bind(ray_sym_intern("count", 5), cv);
        ray_release(cv);
    }
    /* `value` — the one-apply home (eval/q_eval.c).  Env-bound so a bare
     * `value` operand and the manifest row share it. */
    bind_unary("value", q_eval_value_wrap);
    /* `enlist` — q dict arm (1-row table); the `,` monadic QK_ENV snapshot
     * inherits it (bound before q_registry_init, q_eval_value_wrap precedent). */
    bind_vary ("enlist", q_enlist_wrap);
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
            q_env_bind(ray_sym_intern(nm, strlen(nm)), v);               /* retains */
        }
        /* `not` is the keyword spelling of `~:` and must be the SAME value, not
         * a second row: sharing one object keeps kdb's `~:` parse-tree spelling
         * (provenance is value-keyed) and shadows base's `not`, which reduces a
         * vector to one truthiness bool instead of ref/not.md's per-item 0b/1b. */
        ray_t* nv = q_registry_lookup_name("~", 1, Q_MONADIC);
        assert(nv != NULL);
        q_env_bind(ray_sym_intern("not", 3), nv);
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
        { ".Q.c.id",   id_fn        }, { ".Q.c.ty",   q_dotq_ty_fn   },
        { ".Q.c.qt",   q_dotq_qt_fn   }, { ".Q.c.qp",   q_dotq_qp_fn   },
        { ".Q.c.s",    q_dotq_s_fn    }, { ".Q.c.atob", q_dotq_atob_fn },
        { ".Q.c.hg",   q_dotq_hg_fn   },   /* HTTP GET (ref/dotq.md) */
    };
    for (size_t i = 0; i < sizeof dotq_c_unary / sizeof *dotq_c_unary; i++)
        bind_unary(dotq_c_unary[i].name, dotq_c_unary[i].fn);
    bind_vary (".Q.c.ops", q_dotq_ops_fn);   /* niladic .Q.ops[] + unary .Q.ops x */
    bind_vary (".Q.c.hp", q_dotq_hp_fn);     /* HTTP POST [url;mime;body] (ref/dotq.md) */
    bind_vary (".Q.c.gz", q_dotq_gz_fn);     /* GZip ::/inflate/deflate (ref/dotq.md) */
    bind_value(".Q.c.res", q_registry_name_reserved_words());
    /* .Q.pn stays UNBOUND (ref/dotq.md): `` `pn in key `.Q `` must be 0b — qStudio's safeCount relies on it. */
    /* .j JSON namespace (.j.j/.j.k/.j.jd) is defined in q — src/qlang/j.q, loaded
     * at q_runtime_create; the C homes are the `-31!`/`-29!` bangs (q_bang.c). */
}
