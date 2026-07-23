/* q_eval walker — see q_eval.h.  jq-EvalOp shape: visit -> resolve heads
 * (registry by valence, then env / `.z` / `.q`) -> eval-time forms (`;` seq,
 * `:`/`::`/`op:` assign, `$[c;t;f]` + if/do/while control, `{` capture,
 * literal-ctor interception) -> eval args RIGHT-to-left (#177: mid-line `x:e`
 * binds before a leftward read; error unwinds release the evaluated tail) ->
 * q_eval_apply.
 *
 * LITERAL-CTOR SEAM (finding 1, deferred parser change): the parser embeds
 * the registry list/table ctor VALUES at literal heads, and those special
 * forms evaluated their element trees through base ray_eval — unusable here.
 * The walker intercepts the ctor heads BY POINTER IDENTITY and builds
 * literals natively; the follow-up PR makes the parser emit literal nodes
 * the q evaluator owns, retiring this seam. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/eval/q_eval.h"
#include "qlang/q_err.h"
#include "qlang/q_parse_internal.h"
#include "qlang/q_ops.h"
#include "qlang/q_registry.h"
#define Q_OPS_ENV_GRANDFATHER /* legitimate owner: THE evaluator (name resolution/rebinding) */
#include "qlang/q_registry_internal.h"   /* q_strict_i64 — the do-count judgment */
#include "qlang/q_dotz.h"
#include "qlang/ops/q_index.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "ops/ops.h"
#include <string.h>

#define EVAL_MAX_ARGS  60
#define EVAL_MAX_DEPTH 2048

static _Thread_local int g_depth;

static int64_t sym_id_of(const char* s) {
    return ray_sym_intern_runtime(s, strlen(s));
}

static int nameref(ray_t* x) {
    return x && x->type == -RAY_SYM && !(x->attrs & Q_ATTR_QUOTED);
}

/* Hole detection: ONLY a parse-marked slot (Q_ATTR_HOLE — bracket elision
 * `f[;2]` or the postfix forms' missing operand `1+`) projects.  A plain
 * `::` is uniformly DATA, the generic null, under every head — kdb passes
 * an explicit `::` argument to the callee (ref/dotq.md `.Q.en[::;t1]`). */
static int is_hole(ray_t* x) {
    return x && x->type == -RAY_SYM && (x->attrs & Q_ATTR_HOLE) &&
           !(x->attrs & Q_ATTR_QUOTED);
}

/* adverb spelling -> id (0=' 1=/ 2=\ 3=': 4=/: 5=\:), else -1.
 * Re-interned per call: the sym table is recreated per runtime, so a static
 * id cache goes stale across the C-unit harness's runtimes (interning an
 * existing spelling is a cheap hash hit). */
static int adv_id(ray_t* x) {
    if (!nameref(x)) return -1;
    for (int i = 0; i < 6; i++)
        if (x->i64 == sym_id_of(ADVERB_NAMES[i])) return i;
    return -1;
}

static ray_t* name_error(int64_t id) {
    (void)id;   /* kdb names the undefined sym; we emit bare 'name (recorded gap) */
    return q_err(QE_NAME);
}

/* Name resolution with the base name hook INLINED (finding 6 / carve-eval
 * quarry): manifest verbs are reserved so the registry wins for them; then
 * env (scopes + globals), `.z.*`, and the `.q` namespace.  Owned result or
 * NULL.  *row_out set when the hit came from the registry. */
static ray_t* resolve(int64_t id, const q_op_t** row_out) {
    if (row_out) *row_out = NULL;
    ray_t* nm = ray_sym_str(id);
    if (nm) {
        if (q_ops_find(ray_str_ptr(nm), (int)ray_str_len(nm))) {
            const q_op_t* row = NULL;
            ray_t* hit = q_registry_lookup_row(id, Q_MONADIC, &row);
            if (!hit) hit = q_registry_lookup_row(id, Q_DYADIC, &row);
            if (hit) {
                ray_release(nm);
                if (row_out) *row_out = row;
                ray_retain(hit);
                return hit;
            }
        }
        ray_release(nm);
    }
    ray_t* v = ray_env_resolve(id);          /* owned (or an owned error) */
    if (v) return v;
    v = q_dotz_resolve(id);                  /* owned */
    if (v) return v;
    /* bare identifier -> `.q.<name>` (kdb: keywords ARE .q entries) */
    nm = ray_sym_str(id);
    if (nm) {
        const char* p = ray_str_ptr(nm);
        size_t n = ray_str_len(nm);
        char full[64];
        if (n > 0 && n + 3 < sizeof full && !memchr(p, '.', n)) {
            memcpy(full, ".q.", 3);
            memcpy(full + 3, p, n);
            ray_release(nm);
            return ray_env_resolve(ray_sym_intern_runtime(full, n + 3));
        }
        ray_release(nm);
    }
    return NULL;
}

static ray_t* name_value(ray_t* sym, const q_op_t** row_out) {
    if (row_out) *row_out = NULL;
    if (sym->attrs & Q_ATTR_QUOTED) { ray_retain(sym); return sym; }
    if (sym->i64 == sym_id_of("::")) return RAY_NULL_OBJ;
    ray_t* v = resolve(sym->i64, row_out);
    return v ? v : name_error(sym->i64);
}

/* application-head name: registry at the call's valence first (rule 3 —
 * the verb table is authoritative), then general resolution */
static ray_t* head_name_value(ray_t* sym, int64_t argc, const q_op_t** row_out) {
    if (row_out) *row_out = NULL;
    if (!(sym->attrs & Q_ATTR_QUOTED)) {
        q_valence_t val = (argc == 1) ? Q_MONADIC : (argc == 2) ? Q_DYADIC : 0;
        if (val) {
            const q_op_t* row = NULL;
            ray_t* v = q_registry_lookup_row(sym->i64, val, &row);
            if (v && (v->type == RAY_UNARY || v->type == RAY_BINARY ||
                      v->type == RAY_VARY)) {
                if (row_out) *row_out = row;
                ray_retain(v);
                return v;
            }
        }
    }
    return name_value(sym, row_out);
}

/* adverb operand -> VALUE (+ row): `+/` derives from the DYAD */
static ray_t* operand_value(ray_t* F, const q_op_t** row_out) {
    if (row_out) *row_out = NULL;
    if (!F) return q_err(QE_TYPE);
    if (F->type == RAY_UNARY || F->type == RAY_BINARY || F->type == RAY_VARY) {
        if (row_out) *row_out = q_registry_row_of(F, Q_DYADIC);
        ray_retain(F);
        return F;
    }
    if (F->type == -RAY_SYM && !(F->attrs & Q_ATTR_QUOTED)) {
        const q_op_t* row = NULL;
        ray_t* v = q_registry_lookup_row(F->i64, Q_DYADIC, &row);
        if (v && (v->type == RAY_UNARY || v->type == RAY_BINARY ||
                  v->type == RAY_VARY)) {
            if (row_out) *row_out = row;
            ray_retain(v);
            return v;
        }
        return name_value(F, row_out);
    }
    if (F->type == RAY_LIST) return q_eval(F);
    ray_retain(F);
    return F;
}

/* args RIGHT-to-left into argv (owned; holes stay C-NULL).  On error the
 * already-evaluated tail (indices > i) is released.  A LONE elided slot is
 * `f[]` — kdb applies to the generic null, only `;`-elision projects. */
static ray_t* eval_args_rtl(ray_t** e, int64_t argc, ray_t** argv) {
    for (int64_t i = argc - 1; i >= 0; i--) {
        if (is_hole(e[i]) && argc == 1) { argv[i] = RAY_NULL_OBJ; continue; }
        if (is_hole(e[i])) { argv[i] = NULL; continue; }
        argv[i] = q_eval(e[i]);
        if (RAY_IS_ERR(argv[i])) {
            ray_t* err = argv[i];
            for (int64_t j = i + 1; j < argc; j++)
                if (argv[j]) ray_release(argv[j]);
            return err;
        }
    }
    return NULL;
}

static void release_args(ray_t** argv, int64_t argc) {
    for (int64_t i = 0; i < argc; i++)
        if (argv[i]) ray_release(argv[i]);
}

/* literal elements are stored into containers: materialize boundary */
static ray_t* store_mat(ray_t* r) {
    if (r && ray_is_lazy(r)) return ray_lazy_materialize(r);
    return r;
}

/* `;` statement sequence: LEFT-to-right, empty slots are no-ops */
static ray_t* seq_eval(ray_t** e, int64_t n) {
    ray_t* r = RAY_NULL_OBJ;
    for (int64_t i = 0; i < n; i++) {
        if (!e[i]) continue;
        ray_t* nr = q_eval(e[i]);
        if (RAY_IS_ERR(nr)) { ray_release(r); return nr; }
        ray_release(r);
        r = nr;
    }
    return r;
}

/* `a[i;…]:v` / `a[i;…]op:v` (ref/assign.md Indexed assign): rhs first, then
 * indices RTL; the write IS the one amend home — resolve the name, amend the
 * path (`a[i]op:v` ≡ .[a;i;op;v], so repeat-accumulation holds), rebind under
 * the plain-assignment locality rule.  Plain form returns the assigned rhs;
 * an op-assign values as the NEW a[i] (ref/assign.md: `1+a[2]+:5` is 8). */
static ray_t* indexed_assign(int is_global, ray_t* target, ray_t* opv,
                             ray_t* rhs) {
    ray_t** te = (ray_t**)ray_data(target);
    int64_t k = ray_len(target) - 1;
    if (k < 1 || k > EVAL_MAX_ARGS || !nameref(te[0]))
        return q_err(QE_NYI);
    ray_t* s = ray_sym_str(te[0]->i64);
    if (!s) return q_err(QE_TYPE);
    int reserved = q_ops_is_reserved(ray_str_ptr(s), (int)ray_str_len(s));
    int dotted = ray_str_len(s) > 0 && ray_str_ptr(s)[0] == '.';
    ray_release(s);
    if (reserved) return q_err(QE_ASSIGN);
    ray_t* rv = q_eval(rhs);
    if (RAY_IS_ERR(rv)) return rv;
    rv = store_mat(rv);
    if (RAY_IS_ERR(rv)) return rv;
    ray_t* idxv[EVAL_MAX_ARGS];
    ray_t* err = eval_args_rtl(te + 1, k, idxv);
    if (err) { ray_release(rv); return err; }
    ray_t* ret = NULL;
    ray_t* cur = name_value(te[0], NULL);
    if (RAY_IS_ERR(cur)) ret = cur;
    if (!ret) {
        ray_t* amended = q_index_amend(cur, idxv, k, opv, rv);
        if (RAY_IS_ERR(amended)) { ray_release(cur); ret = amended; }
        else {                                       /* cur consumed on success */
            int in_frame = !dotted && q_eval_apply_frame_depth() > 0;
            int local = in_frame &&
                        (!is_global || ray_env_get_local(te[0]->i64) != NULL);
            ray_err_t e2 = local ? ray_env_set_local(te[0]->i64, amended)
                                 : ray_env_set(te[0]->i64, amended);
            if (e2 != RAY_OK) ret = q_err(QE_ASSIGN);
            else if (!opv) { ray_retain(rv); ret = rv; }
            else ret = store_mat(q_eval_apply_value(amended, idxv, k));
            ray_release(amended);
        }
    }
    release_args(idxv, k);
    ray_release(rv);
    return ret;
}

/* `:`/`::` assignment.  `:` inside a lambda frame binds a local; `::` and
 * dotted names are global — UNLESS the `::` name is an argument or already
 * defined as a local, which assigns the local and leaves the global alone
 * (function-notation.md "Name scope").  Returns the assigned value. */
static ray_t* assign_eval(int is_global, ray_t* target, ray_t* rhs) {
    if (!nameref(target)) {
        if (target && target->type == RAY_LIST && ray_len(target) >= 2)
            return indexed_assign(is_global, target, NULL, rhs);
        return q_err(QE_NYI);
    }
    ray_t* s = ray_sym_str(target->i64);
    if (!s) return q_err(QE_TYPE);
    int reserved = q_ops_is_reserved(ray_str_ptr(s), (int)ray_str_len(s));
    int dotted = ray_str_len(s) > 0 && ray_str_ptr(s)[0] == '.';
    ray_release(s);
    if (reserved) return q_err(QE_ASSIGN);
    ray_t* v = q_eval(rhs);
    if (RAY_IS_ERR(v)) return v;
    int in_frame = !dotted && q_eval_apply_frame_depth() > 0;
    int local = in_frame &&
                (!is_global || ray_env_get_local(target->i64) != NULL);
    ray_err_t err = local ? ray_env_set_local(target->i64, v)
                          : ray_env_set(target->i64, v);
    if (err != RAY_OK) { ray_release(v); return q_err(QE_ASSIGN); }
    return v;
}

/* ===== control forms (basics/control.md, ref/{cond,if,do,while}.md) ======
 * Eval-time forms: statement args arrive UNEVALUATED and are driven lazily,
 * left-to-right, side effects persisting (no lexical scope of their own);
 * conditions decide at the ONE truthiness home (q_eval_apply_truthy). */

/* evaluate a test tree and decide it; 0 with *err set on failure */
static int ctl_truth(ray_t* test, ray_t** err) {
    return q_eval_apply_truthy(q_eval(test), err);
}

/* evaluate e[from..n) for their side effects; owned error on failure */
static ray_t* ctl_run_body(ray_t** e, int64_t from, int64_t n) {
    for (int64_t i = from; i < n; i++) {
        if (!e[i]) continue;
        ray_t* r = q_eval(e[i]);
        if (RAY_IS_ERR(r)) return r;
        ray_release(r);
    }
    return NULL;
}

/* `$[c1;t1;...;f]` — pairs decide left to right; a trailing lone expr is the
 * else; an even expr count with no hit returns the generic null. */
static ray_t* cond_eval(ray_t** e, int64_t n) {
    int64_t i = 0;
    while (i + 1 < n) {
        ray_t* err = NULL;
        int go = ctl_truth(e[i], &err);
        if (err) return err;
        if (go) return q_eval(e[i + 1]);
        i += 2;
    }
    if (i < n) return q_eval(e[i]);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* `$`-headed with >= 3 args is Cond: the parser embeds the DYADIC registry
 * value (the cast wrapper) at every `$[...]` head; a still-unresolved `$`
 * name-ref is matched by spelling. */
static int dollar_head(ray_t* h) {
    if (!h) return 0;
    if (nameref(h)) return h->i64 == sym_id_of("$");
    if (h->type == RAY_BINARY) {
        const q_op_t* row = q_registry_row_of(h, Q_DYADIC);
        return row && row->name[0] == '$' && row->name[1] == '\0';
    }
    return 0;
}

/* if[t;…] / while[t;…] — generic-null result (ref/if.md, ref/while.md) */
static ray_t* if_eval(ray_t** e, int64_t n, int loop) {
    for (;;) {
        ray_t* err = NULL;
        int go = n >= 1 && ctl_truth(e[0], &err);
        if (err) return err;
        if (!go) break;
        err = ctl_run_body(e, 1, n);
        if (err) return err;
        if (!loop) break;
    }
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* do[n;…] — n a non-negative integer atom (ref/do.md) */
static ray_t* do_eval(ray_t** e, int64_t n) {
    if (n < 1) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    ray_t* cnt = q_eval(e[0]);
    if (RAY_IS_ERR(cnt)) return cnt;
    cnt = store_mat(cnt);
    if (RAY_IS_ERR(cnt)) return cnt;
    int64_t times;
    int ok = cnt && !RAY_ATOM_IS_NULL(cnt) && q_strict_i64(cnt, &times);
    ray_release(cnt);
    if (!ok || times < 0) return q_err(QE_TYPE);
    for (int64_t k = 0; k < times; k++) {
        ray_t* err = ctl_run_body(e, 1, n);
        if (err) return err;
    }
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* modified assignment `x op: y` == `x: x op y` — head `<op>:` where op is a
 * registry dyad; name targets only (indexed mod-assign: rebuild wave) */
static ray_t* modassign_eval(ray_t* h, ray_t* target, ray_t* rhs) {
    ray_t* s = ray_sym_str(h->i64);
    if (!s) return NULL;
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    if (l < 2 || nm[l - 1] != ':' || nm[0] == ':') { ray_release(s); return NULL; }
    const q_op_t* row = NULL;
    ray_t* opv = q_registry_lookup_row(
        ray_sym_intern_runtime(nm, l - 1), Q_DYADIC, &row);
    ray_release(s);
    if (!opv || !(opv->type == RAY_UNARY || opv->type == RAY_BINARY ||
                  opv->type == RAY_VARY))
        return NULL;                                 /* not an op: -> 'name path */
    if (!nameref(target)) {
        if (target && target->type == RAY_LIST && ray_len(target) >= 2)
            return indexed_assign(0, target, opv, rhs);
        return q_err(QE_NYI);
    }
    ray_t* rv = q_eval(rhs);
    if (RAY_IS_ERR(rv)) return rv;
    const q_op_t* trow = NULL;
    ray_t* cur = name_value(target, &trow);
    if (RAY_IS_ERR(cur)) { ray_release(rv); return cur; }
    ray_t* av[2] = { cur, rv };
    ray_t* nv = q_eval_apply(opv, row, av, 2);
    ray_release(cur);
    ray_release(rv);
    if (RAY_IS_ERR(nv)) return nv;
    nv = store_mat(nv);
    if (RAY_IS_ERR(nv)) return nv;
    int local = q_eval_apply_frame_depth() > 0;
    if (local) {
        ray_t* snm = ray_sym_str(target->i64);
        if (snm) {
            if (ray_str_len(snm) > 0 && ray_str_ptr(snm)[0] == '.') local = 0;
            ray_release(snm);
        }
    }
    ray_err_t err = local ? ray_env_set_local(target->i64, nv)
                          : ray_env_set(target->i64, nv);
    if (err != RAY_OK) { ray_release(nv); return q_err(QE_ASSIGN); }
    return nv;                                       /* q returns the NEW value */
}

/* paren list literal: elements RTL, boxed build + collapse */
static ray_t* list_lit(ray_t** e, int64_t n) {
    ray_t* argv[EVAL_MAX_ARGS];
    if (n > EVAL_MAX_ARGS) return q_err(QE_LIMIT);
    for (int64_t i = n - 1; i >= 0; i--) {
        argv[i] = e[i] ? store_mat(q_eval(e[i])) : RAY_NULL_OBJ;
        if (RAY_IS_ERR(argv[i])) {
            ray_t* err = argv[i];
            for (int64_t j = i + 1; j < n; j++) ray_release(argv[j]);
            return err;
        }
    }
    ray_t* l = ray_list_new(n > 0 ? n : 1);
    for (int64_t i = 0; i < n; i++)
        l = ray_list_append(l, argv[i]);
    release_args(argv, n);
    if (RAY_IS_ERR(l)) return l;
    ray_t* c = q_collapse_list(l);
    ray_release(l);
    return c;
}

/* table literal `([]a:…;b:…)`: col defs are raw parser (:;name;expr) trees;
 * exprs RTL, atoms broadcast.  Punted vs q_ctx_build: anonymous columns,
 * cross-column name scope. */
static ray_t* table_lit(ray_t** defs, int64_t n) {
    int64_t names[EVAL_MAX_ARGS];
    ray_t* cols[EVAL_MAX_ARGS];
    int64_t id_colon = sym_id_of(":");
    if (n > EVAL_MAX_ARGS) return q_err(QE_LIMIT);
    for (int64_t i = n - 1; i >= 0; i--) {
        ray_t* d = defs[i];
        ray_t* v = NULL;
        int64_t nm = -1;
        if (d && d->type == RAY_LIST && ray_len(d) == 3) {
            ray_t** de = (ray_t**)ray_data(d);
            if (nameref(de[0]) && de[0]->i64 == id_colon && nameref(de[1])) {
                nm = de[1]->i64;
                v = store_mat(q_eval(de[2]));
            }
        } else if (nameref(d)) {
            nm = d->i64;
            v = name_value(d, NULL);
        }
        if (!v) v = q_err(QE_NYI);
        if (nm < 0 || RAY_IS_ERR(v)) {
            for (int64_t j = i + 1; j < n; j++) ray_release(cols[j]);
            if (RAY_IS_ERR(v)) return v;
            ray_release(v);
            return q_err(QE_NYI);
        }
        names[i] = nm;
        cols[i] = v;
    }
    int64_t nrows = -1;
    ray_t* out = NULL;
    for (int64_t i = 0; i < n && !out; i++) {
        if (cols[i]->type >= 0 && cols[i]->type != RAY_DICT &&
            cols[i]->type != RAY_TABLE) {
            int64_t cl = ray_len(cols[i]);
            if (nrows < 0) nrows = cl;
            else if (cl != nrows) out = q_err(QE_LENGTH);
        }
    }
    if (nrows < 0) nrows = 1;
    if (!out) out = ray_table_new(n);
    for (int64_t i = 0; i < n && !RAY_IS_ERR(out); i++) {
        ray_t* col = cols[i];
        int owned = 0;
        if (col->type < 0) {                        /* atom -> broadcast */
            ray_t* nn = ray_i64(nrows);
            col = ray_take_fn(col, nn);
            ray_release(nn);
            owned = 1;
            if (!col || RAY_IS_ERR(col)) {
                ray_release(out);
                out = col ? col : q_err(QE_TYPE);
                break;
            }
        }
        out = ray_table_add_col(out, names[i], col);
        if (owned) ray_release(col);
    }
    release_args(cols, n);
    return out;
}

/* lambda literal ({; src; params; body...) -> RAY_QFN carrier */
static ray_t* lambda_lit(ray_t* node) {
    int64_t n = ray_len(node);
    ray_t** e = (ray_t**)ray_data(node);
    if (n < 3) return q_err(QE_PARSE);
    return q_eval_apply_lambda_new(e[2], e + 3, n - 3, e[1]);
}

ray_t* q_eval(ray_t* node) {
    if (!node) return RAY_NULL_OBJ;
    if (RAY_IS_ERR(node)) return node;
    if (ray_eval_is_interrupted()) return q_err(QE_STOP);
    if (++g_depth > EVAL_MAX_DEPTH) { g_depth--; return q_err(QE_LIMIT); }
    int64_t id_semi   = sym_id_of(";");
    int64_t id_colon  = sym_id_of(":");
    int64_t id_gcolon = sym_id_of("::");
    int64_t id_brace  = sym_id_of("{");

    ray_t* ret;
    if (node->type == -RAY_SYM) {
        ret = name_value(node, NULL);
        goto out;
    }
    if (node->type != RAY_LIST || ray_len(node) == 0) {
        ray_retain(node);
        ret = node;
        goto out;
    }

    {
        int64_t n = ray_len(node);
        ray_t** e = (ray_t**)ray_data(node);
        ray_t* h = e[0];

        /* literal-ctor interception seam (header note) */
        if (h == q_registry_list_value())  { ret = list_lit(e + 1, n - 1); goto out; }
        if (h == q_registry_table_value()) { ret = table_lit(e + 1, n - 1); goto out; }
        if (h == q_registry_keyed_table_value()) { ret = q_err(QE_NYI); goto out; }

        if (nameref(h)) {
            if (h->i64 == id_semi) { ret = seq_eval(e + 1, n - 1); goto out; }
            if ((h->i64 == id_colon || h->i64 == id_gcolon) && n == 3) {
                ret = assign_eval(h->i64 == id_gcolon, e[1], e[2]);
                goto out;
            }
            if (h->i64 == id_brace) { ret = lambda_lit(node); goto out; }
            if (h->i64 == sym_id_of("if"))    { ret = if_eval(e + 1, n - 1, 0); goto out; }
            if (h->i64 == sym_id_of("while")) { ret = if_eval(e + 1, n - 1, 1); goto out; }
            if (h->i64 == sym_id_of("do"))    { ret = do_eval(e + 1, n - 1); goto out; }
            if (n == 3) {
                ray_t* ma = modassign_eval(h, e[1], e[2]);
                if (ma) { ret = ma; goto out; }
            }
            int adv = adv_id(h);
            if (adv >= 0 && n == 2) {               /* bare (adv;F) value */
                const q_op_t* frow = NULL;
                ray_t* F = operand_value(e[1], &frow);
                if (RAY_IS_ERR(F)) { ret = F; goto out; }
                ret = q_eval_apply_deriv_new(adv, F, frow);
                ray_release(F);
                goto out;
            }
        }

        /* adverb-headed application ((adv;F); args...) — native, no HOF */
        if (h && h->type == RAY_LIST && ray_len(h) == 2) {
            int adv = adv_id(((ray_t**)ray_data(h))[0]);
            if (adv >= 0) {
                const q_op_t* frow = NULL;
                ray_t* F = operand_value(((ray_t**)ray_data(h))[1], &frow);
                if (RAY_IS_ERR(F)) { ret = F; goto out; }
                int64_t argc = n - 1;
                ray_t* argv[EVAL_MAX_ARGS];
                if (argc > EVAL_MAX_ARGS) {
                    ray_release(F);
                    ret = q_err(QE_RANK);
                    goto out;
                }
                ray_t* err = eval_args_rtl(e + 1, argc, argv);
                if (err) { ray_release(F); ret = err; goto out; }
                ret = q_eval_apply_adverb(adv, F, frow, argv, argc);
                release_args(argv, argc);
                ray_release(F);
                goto out;
            }
        }

        /* `$[c;t;f;...]` Cond — claimed BEFORE argument evaluation (lazy) */
        if (n >= 4 && dollar_head(h)) { ret = cond_eval(e + 1, n - 1); goto out; }

        /* general application: resolve head, args RTL, one q_eval_apply */
        const q_op_t* row = NULL;
        ray_t* fv;
        if (h && h->type == -RAY_SYM) {
            fv = head_name_value(h, n - 1, &row);
        } else if (h && h->type == RAY_LIST) {
            fv = q_eval(h);
        } else if (h) {
            ray_retain(h);                          /* embedded value / noun */
            fv = h;
            if (h->type == RAY_UNARY || h->type == RAY_BINARY ||
                h->type == RAY_VARY) {
                q_valence_t val = (n - 1 == 1) ? Q_MONADIC
                                : (n - 1 == 2) ? Q_DYADIC : 0;
                if (val) row = q_registry_row_of(h, val);
            }
        } else {
            ret = q_err(QE_TYPE);
            goto out;
        }
        if (RAY_IS_ERR(fv)) { ret = fv; goto out; }

        int64_t argc = n - 1;
        ray_t* argv[EVAL_MAX_ARGS];
        if (argc > EVAL_MAX_ARGS) {
            ray_release(fv);
            ret = q_err(QE_RANK);
            goto out;
        }
        ray_t* err = eval_args_rtl(e + 1, argc, argv);
        if (err) { ray_release(fv); ret = err; goto out; }
        ret = q_eval_apply(fv, row, argv, argc);
        release_args(argv, argc);
        ray_release(fv);
    }

out:
    g_depth--;
    return ret;
}
