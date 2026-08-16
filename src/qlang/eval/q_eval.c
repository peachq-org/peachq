/* q_eval walker — see q_eval.h.  jq-EvalOp shape: visit -> resolve heads
 * (registry by valence, then env / `.z` / `.q`) -> eval-time forms (`;` seq,
 * `:`/`::`/`op:` assign, `$[c;t;f]` + if/do/while control,
 * literal-ctor interception) -> eval args RIGHT-to-left, THEN the head, which
 * is the node's leftmost expression (#177 + basics/syntax.md: mid-line `x:e`
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
#include "qlang/eval/q_eval_internal.h"
#include "qlang/eval/q_view.h"
#include "qlang/base/q_err.h"
#include "qlang/parse/q_parse_internal.h"
#include "qlang/q_ops.h"
#include "qlang/q_registry.h"
#include "qlang/q_registry_internal.h"   /* q_type_strict_i64 — the do-count judgment */
#include "qlang/q_dotz.h"
#include "qlang/q_env.h"
#include "qlang/ops/q_index.h"
#include "qlang/ops/q_dollar.h"   /* q_dollar — the cast home `x.attr` rides */
#include "qlang/net/q_wirefile.h"  /* q_wirefile_read — `get `:file */
#include "qlang/io/q_provider.h"   /* q_provider_get_carrier — `get `:pq:...:t/ */
#include "qlang/io/q_splay.h"      /* q_splay_get — `get `:dir/ maps, not reads */
#include "lang/eval.h"
#include "ops/ops.h"
#include "table/sym.h"             /* ray_read_sym — sym vectors are width-adaptive */
#include <string.h>

#define EVAL_MAX_ARGS  60
#define EVAL_MAX_DEPTH 2048

static _Thread_local int g_depth;

/* Fixed sym ids, interned ONCE per runtime (the env.c ipc-hook-syms
 * discipline).  Process-global like the registry itself — one live q runtime
 * per process (__RUNTIME).  The sym table is recreated per runtime, so
 * q_runtime_destroy resets via q_eval_syms_reset — a surviving cache would
 * hand the next runtime stale ids and control forms would degrade to 'name. */
typedef struct {
    int64_t adv[6];
    int64_t semi, colon, gcolon, dollar, kif, kwhile, kdo;
    int     ready;
} eval_syms_t;
static eval_syms_t g_syms;

static const eval_syms_t* syms(void) {
    if (!g_syms.ready) {
        for (int i = 0; i < 6; i++)
            g_syms.adv[i] = ray_sym_intern_runtime(ADVERB_NAMES[i],
                                                   strlen(ADVERB_NAMES[i]));
        g_syms.semi   = ray_sym_intern_runtime(";", 1);
        g_syms.colon  = ray_sym_intern_runtime(":", 1);
        g_syms.gcolon = ray_sym_intern_runtime("::", 2);
        g_syms.dollar = ray_sym_intern_runtime("$", 1);
        g_syms.kif    = ray_sym_intern_runtime("if", 2);
        g_syms.kwhile = ray_sym_intern_runtime("while", 5);
        g_syms.kdo    = ray_sym_intern_runtime("do", 2);
        g_syms.ready  = 1;
    }
    return &g_syms;
}

void q_eval_syms_reset(void) {
    memset(&g_syms, 0, sizeof g_syms);
}

/* A sym ATOM in a tree is ALWAYS a name reference (parsetrees.md:80): the
 * parser emits sym CONSTANTS enlisted (,`x), so trees are attr-free data —
 * two identical-printing trees mean the same thing. */
static int nameref(ray_t* x) {
    return x && x->type == -RAY_SYM;
}

/* the enlisted-constant unwrap's tree shape: a 1-element sym vector */
static int sym_const(ray_t* x) {
    return x && x->type == RAY_SYM && ray_len(x) == 1;
}

/* Hole detection: ONLY a parse-marked slot (Q_ATTR_HOLE — bracket elision
 * `f[;2]` or the postfix forms' missing operand `1+`) projects.  A plain
 * `::` is uniformly DATA, the generic null, under every head — kdb passes
 * an explicit `::` argument to the callee (ref/dotq.md `.Q.en[::;t1]`). */
static int is_hole(ray_t* x) {
    return x && x->type == -RAY_SYM && (x->attrs & Q_ATTR_HOLE);
}

/* adverb id (0=' 1=/ 2=\ 3=': 4=/: 5=\:), else -1.  The parser emits the
 * ITERATOR VALUE, so that is the live reading; the sym spelling is still
 * honoured for hand-built trees. */
static int adv_id(ray_t* x) {
    int it = q_eval_apply_iter_id(x);
    if (it >= 0) return it;
    if (!nameref(x)) return -1;
    const eval_syms_t* S = syms();
    for (int i = 0; i < 6; i++)
        if (x->i64 == S->adv[i]) return i;
    return -1;
}

/* ref/signal.md: an undefined word signals the word itself */
static ray_t* name_error(int64_t id) {
    ray_t* s = ray_sym_str(id);
    if (!s || RAY_IS_ERR(s)) return q_err(QE_NAME);
    ray_t* e = q_err_name(ray_str_ptr(s), ray_str_len(s));
    ray_release(s);
    return e;
}

/* Name resolution with the base name hook INLINED (finding 6 / carve-eval
 * quarry): manifest verbs are reserved so the registry wins for them; then
 * env (scopes + globals), `.z.*`, and the `.q` namespace.  Owned result or
 * NULL.  *row_out set when the hit came from the registry. */
static ray_t* resolve(int64_t id, const q_op_t** row_out) {
    if (row_out) *row_out = NULL;
    const q_op_t* row = NULL;
    ray_t* hit = q_registry_lookup_row(id, Q_MONADIC, &row);
    if (!hit) hit = q_registry_lookup_row(id, Q_DYADIC, &row);
    if (hit) {
        if (row_out) *row_out = row;
        ray_retain(hit);
        return hit;
    }
    ray_t* v = q_env_resolve(id);            /* owned (or an owned error) */
    if (v) return q_view_deref(v);           /* a view reference recalcs */
    v = q_dotz_resolve(id);                  /* owned */
    if (v) return v;
    /* bare identifier -> `.q.<name>` (kdb: keywords ARE .q entries) */
    ray_t* nm = ray_sym_str(id);
    if (nm) {
        const char* p = ray_str_ptr(nm);
        size_t n = ray_str_len(nm);
        char full[64];
        if (n > 0 && n + 3 < sizeof full && !memchr(p, '.', n)) {
            memcpy(full, ".q.", 3);
            memcpy(full + 3, p, n);
            ray_release(nm);
            return q_env_resolve(ray_sym_intern_runtime(full, n + 3));
        }
        ray_release(nm);
    }
    return NULL;
}

/* Dot notation whose last segment names a cast target IS that cast:
 * `time.minute` ~ `` `minute$time ``, `t.hh` ~ `` `hh$t `` (ref/cast.md
 * Temporal, `` `year`dd`mm`hh`uu`ss$2015.10.28D03:55:58 ``).  The `$` home
 * owns which designators exist — BOTH readings, the type targets and the
 * `uu`/`year`/`week` components, so it is `$` entire and not q_dollar_cast.
 *
 * Runs LAST, after resolve(): the env walk's accessor roster (q_env.c
 * walk_segs) keeps its owner-ruled `mm`/`minute`/`date`/`time`, and this
 * reaches only what that roster never had — the spellings it records as
 * parked, and bases that are a LOCAL (a qSQL column), which it never sees.
 * Kept only when the cast SUCCEEDS, so nothing resolvable changes meaning.
 *
 * COUPLING: `$` falls through to q_dollar_enum for a sym it cannot read as a
 * cast target — 'nyi today, so a miss still lands on the name error.  When
 * enum domains land, gate this or `x.dom` will silently enumerate. */
static ray_t* dot_cast(int64_t id) {
    ray_t* nm = ray_sym_str(id);
    if (!nm) return NULL;
    const char* p = ray_str_ptr(nm);
    size_t n = ray_str_len(nm), cut = 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] == '.') cut = i;
    ray_t* r = NULL;
    if (cut > 0 && cut + 1 < n) {
        /* the base must be a STORED name (global or qSQL-column local), never
         * the registry or a `.z` builtin: `.z.p.year` is 'name until `.z.p` is
         * assigned to a variable (temporal_dot_accessor.qcmd pins the oddity) */
        ray_t* base = q_env_resolve(ray_sym_intern_runtime(p, (int64_t)cut));
        if (base && !RAY_IS_ERR(base) && !q_view_is(base)) {
            ray_t* attr = ray_sym(ray_sym_intern_runtime(p + cut + 1,
                                                         (int64_t)(n - cut - 1)));
            r = q_dollar(attr, base);
            ray_release(attr);
            if (r && RAY_IS_ERR(r)) { ray_release(r); r = NULL; }
        }
        if (base) ray_release(base);
    }
    ray_release(nm);
    return r;
}

static ray_t* name_value(ray_t* sym, const q_op_t** row_out) {
    if (row_out) *row_out = NULL;
    if (sym->i64 == syms()->gcolon) return RAY_NULL_OBJ;
    ray_t* v = resolve(sym->i64, row_out);
    if (!v) v = dot_cast(sym->i64);
    return v ? v : name_error(sym->i64);
}

/* adverb operand -> VALUE (+ row): `+/` derives from the DYAD */
static ray_t* operand_value(ray_t* F, const q_op_t** row_out) {
    if (row_out) *row_out = NULL;
    if (!F) return q_err(QE_TYPE);
    ray_t* v;
    if (F->type == -RAY_SYM) {
        const q_op_t* row = NULL;
        ray_t* hit = q_registry_lookup_row(F->i64, Q_DYADIC, &row);
        if (hit && (hit->type == RAY_UNARY || hit->type == RAY_BINARY ||
                    hit->type == RAY_VARY)) {
            if (row_out) *row_out = row;
            ray_retain(hit);
            return hit;
        }
        v = name_value(F, row_out);
    } else if (F->type == RAY_LIST) {
        v = q_eval(F);
    } else {
        ray_retain(F);
        v = F;
    }
    /* the manifest row travels with the VALUE, however the operand was spelt:
     * without it the derived call loses `family` and its container lift */
    if (row_out && !*row_out && v && !RAY_IS_ERR(v))
        *row_out = q_registry_operand_row(v);
    return v;
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

/* `;` statement sequence: LEFT-to-right; the sequence's value is its LAST
 * statement's — an empty statement yields the silent null, so `2+2;` prints
 * nothing (kdb console; parser_pinned_defects.qcmd defect 3). */
static ray_t* seq_eval(ray_t** e, int64_t n) {
    ray_t* r = RAY_NULL_OBJ;
    for (int64_t i = 0; i < n; i++) {
        if (!e[i]) { ray_release(r); r = RAY_NULL_OBJ; continue; }
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
    if (q_registry_is_reserved(te[0]->i64)) return q_err(QE_ASSIGN);
    ray_t* s = ray_sym_str(te[0]->i64);
    if (!s) return q_err(QE_TYPE);
    int dotted = ray_str_len(s) > 0 && ray_str_ptr(s)[0] == '.';
    ray_release(s);
    ray_t* rv = q_eval(rhs);
    if (RAY_IS_ERR(rv)) return rv;
    rv = q_eval_apply_concrete(rv);
    if (RAY_IS_ERR(rv)) return rv;
    ray_t* idxv[EVAL_MAX_ARGS];
    ray_t* err = eval_args_rtl(te + 1, k, idxv);
    if (err) { ray_release(rv); return err; }
    ray_t* ret = NULL;
    ray_t* cur = name_value(te[0], NULL);
    if (RAY_IS_ERR(cur)) ret = cur;
    if (!ret) {
        int in_frame = !dotted && q_eval_apply_frame_depth() > 0;
        int local = in_frame &&
                    (!is_global || q_env_local_get(te[0]->i64) != NULL);
        /* the binding this write REPLACES double-counts the value, so the
         * amend would copy the whole vector: park it (q_env.h q_env_take) */
        int stole = local ? q_env_local_take(te[0]->i64, cur)
                          : q_env_take(te[0]->i64, cur);
        ray_t* amended = q_index_amend(cur, idxv, k, opv, rv);
        if (RAY_IS_ERR(amended)) {
            if (stole) {
                if (local) q_env_local_set(te[0]->i64, cur);
                else q_env_bind(te[0]->i64, cur);
            }
            ray_release(cur);
            ret = amended;
        }
        else {                                       /* cur consumed on success */
            if (!local) q_view_set_index(idxv, k);   /* .z.vs y (ref/dotz.md) */
            ray_err_t e2 = local ? q_env_local_set(te[0]->i64, amended)
                                 : q_env_set(te[0]->i64, amended);
            q_view_set_index(NULL, 0);               /* never leaks to the next set */
            /* a failed rebind must not leave the park visible as `::` (a local
             * park cannot get here — its slot exists, so the set cannot fail) */
            if (e2 != RAY_OK) {
                if (stole && !local) q_env_bind(te[0]->i64, amended);
                ret = q_env_err(e2);
            }
            else if (!opv) { ray_retain(rv); ret = rv; }
            else ret = q_eval_apply_concrete(q_eval_apply_value(amended, idxv, k));
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
    if (q_registry_is_reserved(target->i64)) return q_err(QE_ASSIGN);
    ray_t* s = ray_sym_str(target->i64);
    if (!s) return q_err(QE_TYPE);
    int dotted = ray_str_len(s) > 0 && ray_str_ptr(s)[0] == '.';
    ray_release(s);
    ray_t* v = q_eval_apply_concrete(q_eval(rhs));    /* boundary seam: assignment */
    if (RAY_IS_ERR(v)) return v;
    Q_ASSERT_CONCRETE(v);                  /* env-set tripwire */
    int in_frame = !dotted && q_eval_apply_frame_depth() > 0;
    int local = in_frame &&
                (!is_global || q_env_local_get(target->i64) != NULL);
    ray_err_t err = local ? q_env_local_set(target->i64, v)
                          : q_env_set(target->i64, v);
    if (err != RAY_OK) { ray_release(v); return q_env_err(err); }
    return v;
}

/* ===== `value` on a lambda (ref/value.md `## Lambda`) ====================
 * Published shape: (bytecode;params;locals;(namespace,globals);constants…;
 * m;n;f;l;s).  A tree-walker has no bytecode and no source-position map, so
 * slot 0 and m are EMPTY rather than invented; n/f/l keep the doc's own "not
 * applicable" values (()/""/-1) until an assignment stamps a name and the
 * loader carries a file.  Scope follows function-notation.md "Name scope" —
 * the rule assign_eval implements: a `:` target is local for the WHOLE body,
 * `::` and dotted targets are global, every other name is a global reference.
 * Verbs are fn VALUES after parse so they never read as names, and a nested
 * lambda carrier is opaque: its locals are its own. */

typedef struct {
    ray_t *loc, *glb, *ref, *con;
    int    oom;
} lam_scan_t;

int q_eval_symvec_has(ray_t* v, int64_t id) {
    if (!v || v->type != RAY_SYM) return 0;
    const void* d = ray_data(v);
    for (int64_t i = 0, n = ray_len(v); i < n; i++)
        if (ray_read_sym(d, i, RAY_SYM, v->attrs) == id) return 1;
    return 0;
}

static void sym_add(ray_t** v, int64_t id, int* oom) {
    if (*oom || !*v || q_eval_symvec_has(*v, id)) return;
    ray_t* n = ray_vec_append(*v, &id);
    if (!n) { *oom = 1; return; }
    *v = n;
}

/* ray_sym_str is BORROWED (PLAN.md register, 2026-07-30) — no release here */
static int sym_dotted(int64_t id) {
    ray_t* s = ray_sym_str(id);
    return s && ray_str_len(s) > 0 && ray_str_ptr(s)[0] == '.';
}

int q_eval_ctl_sym(int64_t id) {
    const eval_syms_t* S = syms();
    return id == S->semi || id == S->colon || id == S->gcolon ||
           id == S->kif || id == S->kwhile || id == S->kdo;
}

int q_eval_fn_value(ray_t* x) {
    return x && (x->type == RAY_LAMBDA || x->type == RAY_UNARY ||
                 x->type == RAY_BINARY || x->type == RAY_VARY ||
                 x->type == RAY_QFN);
}

static void lam_scan(ray_t* n, lam_scan_t* s) {
    if (!n || s->oom || q_eval_fn_value(n)) return;
    if (nameref(n)) {
        /* a keyword head is a verb, not a free global: the registry owns the
         * name (assignment to it is 'assign), so it can never be a user's */
        if (!q_eval_ctl_sym(n->i64) && !is_hole(n) && !q_registry_is_reserved(n->i64))
            sym_add(&s->ref, n->i64, &s->oom);
        return;
    }
    if (n->type != RAY_LIST) {
        ray_t* c = ray_list_append(s->con, n);
        if (!c) { s->oom = 1; return; }
        s->con = c;
        return;
    }
    int64_t k = ray_len(n);
    ray_t** e = (ray_t**)ray_data(n);
    if (k == 0) return;
    const eval_syms_t* S = syms();
    if (k == 3 && nameref(e[0]) &&
        (e[0]->i64 == S->colon || e[0]->i64 == S->gcolon)) {
        if (nameref(e[1])) {
            int global = e[0]->i64 == S->gcolon || sym_dotted(e[1]->i64);
            sym_add(global ? &s->glb : &s->loc, e[1]->i64, &s->oom);
        } else {
            lam_scan(e[1], s);
        }
        lam_scan(e[2], s);
        return;
    }
    /* the signal/return char-atom heads are syntax, not constants */
    if (k == 2 && e[0] && e[0]->type == -RAY_CHARV &&
        (e[0]->u8 == '\'' || e[0]->u8 == ':')) {
        lam_scan(e[1], s);
        return;
    }
    for (int64_t i = 0; i < k; i++) lam_scan(e[i], s);
}

/* the namespace slot prints bare (`test`d`e), so drop the context's dot */
static int64_t ns_bare(ray_t* ctx) {
    if (!ctx) return ray_sym_intern_runtime("", 0);
    ray_t* s = ray_sym_str(ctx->i64);
    if (!s) return ray_sym_intern_runtime("", 0);
    const char* p = ray_str_ptr(s);
    int64_t n = ray_str_len(s);
    if (n > 0 && p[0] == '.') { p++; n--; }
    return ray_sym_intern_runtime(p, (size_t)n);
}

static ray_t* list_put(ray_t* out, ray_t* v) {
    if (!out) { ray_release(v); return NULL; }
    if (!v) { ray_release(out); return NULL; }
    ray_t* r = ray_list_append(out, v);
    ray_release(v);
    return r;
}

/* borrowed item -> appended (ray_list_append RETAINS); NULL item is `::` */
static ray_t* list_put_borrowed(ray_t* out, ray_t* v) {
    return out ? ray_list_append(out, v ? v : RAY_NULL_OBJ) : NULL;
}

/* ref/value.md's three non-lambda carrier arms: a projection reads out as the
 * "list: function followed by argument/s", a composition as the "list of
 * composed values", a derived function as the bare "argument of the iterator".
 * NULL = not one of those kinds (a bare iterator stays deferred). */
ray_t* q_eval_carrier_value(ray_t* v) {
    ray_t* h = q_eval_apply_car_head(v);
    if (!h) return NULL;
    switch (q_eval_apply_carrier_kind(v)) {
    case Q_EVAL_CAR_DERIV:
        ray_retain(h);
        return h;
    case Q_EVAL_CAR_COMP:
        return list_put_borrowed(list_put_borrowed(ray_list_new(2), h),
                                 q_eval_apply_comp_inner(v));
    case Q_EVAL_CAR_PROJ: {
        /* an ELIDED trailing slot is not a bound argument — `+[2]` fills the
         * dyad's second slot with a hole but reads out as the 2-item (+;2) */
        int64_t n = q_eval_apply_proj_nslots(v);
        while (n > 0 && !q_eval_apply_proj_arg(v, n - 1)) n--;
        ray_t* out = list_put_borrowed(ray_list_new(n + 1), h);
        for (int64_t i = 0; i < n; i++)
            out = list_put_borrowed(out, q_eval_apply_proj_arg(v, i));
        return out;
    }
    default:
        return NULL;
    }
}

static ray_t* lambda_structure(ray_t* v) {
    ray_t *params = NULL, *body = NULL, *ctx = NULL;
    if (!q_eval_apply_lambda_parts(v, &params, &body, &ctx)) return NULL;

    lam_scan_t s = { ray_sym_vec_new(RAY_SYM_W64, 4), ray_sym_vec_new(RAY_SYM_W64, 4),
                     ray_sym_vec_new(RAY_SYM_W64, 4), ray_list_new(1), 0 };
    if (!s.loc || !s.glb || !s.ref || !s.con) s.oom = 1;
    lam_scan(body, &s);

    /* a referenced name is global unless it is a parameter or a body local */
    for (int64_t i = 0, n = s.ref ? ray_len(s.ref) : 0; i < n && !s.oom; i++) {
        int64_t id = ray_read_sym(ray_data(s.ref), i, RAY_SYM, s.ref->attrs);
        if (q_eval_symvec_has(params, id) || q_eval_symvec_has(s.loc, id)) continue;
        sym_add(&s.glb, id, &s.oom);
    }
    ray_t* nsg = ray_sym_vec_new(RAY_SYM_W64, 1 + (s.glb ? ray_len(s.glb) : 0));
    if (!nsg) s.oom = 1;
    else {
        int64_t id = ns_bare(ctx);
        nsg = ray_vec_append(nsg, &id);
        for (int64_t i = 0, n = s.glb ? ray_len(s.glb) : 0; i < n && nsg; i++) {
            int64_t g = ray_read_sym(ray_data(s.glb), i, RAY_SYM, s.glb->attrs);
            nsg = ray_vec_append(nsg, &g);
        }
        if (!nsg) s.oom = 1;
    }

    ray_t* out = s.oom ? NULL : ray_list_new(10);
    ray_t* bc = ray_vec_new(RAY_BYTE_ONLY, 1);          /* no bytecode: 0x */
    if (bc) bc->len = 0;
    out = list_put(out, bc);
    if (params) ray_retain(params);
    out = list_put(out, params ? params : ray_sym_vec_new(RAY_SYM_W64, 1));
    out = list_put(out, s.loc); s.loc = NULL;
    out = list_put(out, nsg);   nsg = NULL;
    for (int64_t i = 0, n = s.con ? ray_len(s.con) : 0; i < n && out; i++) {
        ray_t* c = ((ray_t**)ray_data(s.con))[i];
        ray_retain(c);
        out = list_put(out, c);
    }
    ray_t* m = ray_vec_new(RAY_I64, 1);                 /* no position map */
    if (m) m->len = 0;
    out = list_put(out, m);
    ray_t* nm = NULL;
    int64_t fsym = 0, lno = -1;
    q_eval_apply_lambda_prov(v, &nm, &fsym, &lno);
    if (nm) {                                           /* n: name, () unnamed */
        ray_retain(nm);
        out = list_put(out, nm);
    } else {
        out = list_put(out, ray_list_new(1));
    }
    ray_t* fstr = fsym ? ray_sym_str(fsym) : NULL;      /* borrowed */
    ray_t* fl = fstr ? ray_charv(ray_str_ptr(fstr), ray_str_len(fstr))
                     : ray_vec_new(RAY_CHARV, 1);       /* f: file path or "" */
    if (fl && !fstr) fl->len = 0;
    out = list_put(out, fl);
    out = list_put(out, ray_i64(lno));                  /* l: line or -1 */
    ray_t* src = q_eval_apply_lambda_src(v);
    out = list_put(out, src ? q_str_charv_of_str(src) : ray_vec_new(RAY_CHARV, 1));

    ray_release(s.loc); ray_release(s.glb); ray_release(s.ref);
    ray_release(s.con); ray_release(nsg);
    return out ? out : q_err(QE_WSFULL);
}

/* `value`/`get` (ref/value.md; get is the synonym).  q `eval` needs no body
 * here — it is .q.eval:(-6!), the q_bang.c arm over q_eval.  value applies
 * ONCE, non-recursively: a string parses+evaluates in the current context, a
 * `:path sym READS THAT FILE (ref/get.md — kdb's get IS value), any other
 * sym atom names a variable, a dict yields its values, and a list applies
 * its first item (string/sym heads evaluated first, ref/value.md) to the
 * rest AS LITERALS — nested trees stay data.  value of a TYPED vector
 * (incl. the enlisted constant ,`x) is doc-silent: 'nyi, never a guess. */
ray_t* q_eval_value_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (RAY_IS_NULL(x)) return ray_i64(0);   /* (::) IS unary primitive 0 —
                                              * ref/value.md operator arm */
    if (x->type == -RAY_SYM) {
        ray_t* m = q_splay_get(x);           /* NULL unless a `:dir/ table folder */
        if (m) return m;
        ray_t* pc = q_provider_get_carrier(x);   /* `:pq:...:t/ -> the carrier;
                                                  * connection form -> 'domain */
        if (pc) return pc;
        ray_t* f = q_wirefile_read(x);       /* NULL unless a `:path sym */
        return f ? f : name_value(x, NULL);
    }
    if (x->type == RAY_CHARV || x->type == -RAY_CHARV || x->type == -RAY_STR) {
        const char* p; int64_t n;
        if (!q_str_text_bytes(x, &p, &n)) return q_err(QE_TYPE);
        char* z = (char*)ray_alloc_raw((size_t)n + 1);
        if (!z) return q_err(QE_WSFULL);
        memcpy(z, p, (size_t)n);
        z[n] = 0;
        ray_t* ast = q_parse(z);
        if (!ast || RAY_IS_ERR(ast)) {
            ray_free_raw(z);
            return ast ? ast : q_err(QE_PARSE);
        }
        /* value of SOURCE TEXT is a statement, so `z::e` defines a view here
         * just as it does at the repl seam — unlike eval of a parse TREE,
         * which cannot carry the distinction (learn/views.md#parse). */
        ray_t* r;
        if (!q_view_intercept(ast, z, &r)) r = q_eval(ast);
        ray_free_raw(z);
        ray_release(ast);
        return r;
    }
    if (x->type == RAY_DICT) {
        ray_t* v = ray_dict_vals(x);
        ray_retain(v);
        return v;
    }
    if (x->type == RAY_LIST && ray_len(x) >= 1) {
        int64_t argc = ray_len(x) - 1;
        if (argc > EVAL_MAX_ARGS) return q_err(QE_RANK);
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* h = e[0];
        if (!h) return q_err(QE_TYPE);
        ray_t* fv;
        if (h->type == -RAY_SYM || h->type == RAY_CHARV ||
            h->type == -RAY_CHARV)
            fv = q_eval_value_wrap(h);
        else {
            ray_retain(h);
            fv = h;
        }
        if (RAY_IS_ERR(fv)) return fv;
        if (argc == 0) return fv;                        /* 1-item list: the item */
        ray_t* r = q_eval_apply_value(fv, e + 1, argc);
        ray_release(fv);
        return r;
    }
    if (q_view_is(x)) return q_view_value4(x);   /* value`. `v — ref/value.md */
    if (x->type == RAY_QFN) {
        ray_t* st = lambda_structure(x);
        if (!st) st = q_eval_carrier_value(x);
        if (st) return st;
        int adv = q_eval_apply_iter_id(x);  /* a bare iterator IS its 103h code */
        if (adv >= 0) return ray_i64(adv);
    }
    /* operator -> its kdb primitive code (ref/value.md; the manifest row is
     * the identity, its kdb_op column the number) */
    if (x->type == RAY_UNARY || x->type == RAY_BINARY || x->type == RAY_VARY) {
        int code = q_registry_kdb_op_of(x, NULL);
        if (code >= 0) return ray_i64(code);
    }
    return q_err(QE_NYI);       /* enumeration/view/bare-iterator: deferred */
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
    if (nameref(h)) return h->i64 == syms()->dollar;
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
    cnt = q_eval_apply_concrete(cnt);
    if (RAY_IS_ERR(cnt)) return cnt;
    int64_t times;
    int ok = cnt && !RAY_ATOM_IS_NULL(cnt) && q_type_strict_i64(cnt, &times);
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
    /* undefined name: the op's identity element is the default (ref/assign.md
     * "Assign through operator"); an op without one keeps signalling 'name */
    if (RAY_IS_ERR(cur) && q_err_is(cur, QE_NAME) && row) {
        ray_t* id = q_ops_identity(row->name, QI_VALUE);
        if (id) { q_err_drop(); ray_error_free(cur); cur = id; }
    }
    if (RAY_IS_ERR(cur)) { ray_release(rv); return cur; }
    ray_t* av[2] = { cur, rv };
    ray_t* nv = q_eval_apply(opv, row, av, 2);
    ray_release(cur);
    ray_release(rv);
    if (RAY_IS_ERR(nv)) return nv;
    nv = q_eval_apply_concrete(nv);
    if (RAY_IS_ERR(nv)) return nv;
    int local = q_eval_apply_frame_depth() > 0;
    if (local) {
        ray_t* snm = ray_sym_str(target->i64);
        if (snm) {
            if (ray_str_len(snm) > 0 && ray_str_ptr(snm)[0] == '.') local = 0;
            ray_release(snm);
        }
    }
    ray_err_t err = local ? q_env_local_set(target->i64, nv)
                          : q_env_set(target->i64, nv);
    if (err != RAY_OK) { ray_release(nv); return q_env_err(err); }
    return nv;                                       /* q returns the NEW value */
}

/* Signal `'x` / explicit return `:x` — SYNTAX forms beside $/if/do/while,
 * never verbs (ref/signal.md: "not an operator"). */

/* `'x`: symbol atom or string, else 'stype (ref/signal.md Restrictions);
 * signals the text */
static ray_t* signal_eval(ray_t* expr) {
    ray_t* v = q_eval_apply_concrete(q_eval(expr));
    if (RAY_IS_ERR(v)) return v;
    ray_t* txt = NULL;
    if (v->type == RAY_CHARV) { ray_retain(v); txt = v; }
    else if (v->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(v->i64);
        if (s && !RAY_IS_ERR(s))
            txt = ray_charv(ray_str_ptr(s), (int64_t)ray_str_len(s));
    }
    ray_release(v);
    if (!txt || RAY_IS_ERR(txt)) return txt ? txt : q_err(QE_STYPE);
    ray_t* r = q_err_signal(QE_SIGNAL, txt);
    ray_release(txt);
    return r;
}

/* `:x` rides the error-return path as a QE_RETURN sentinel that the
 * lambda-apply boundary unwraps into an ordinary successful result */
static ray_t* return_eval(ray_t* expr) {
    ray_t* v = q_eval_apply_concrete(q_eval(expr));
    if (RAY_IS_ERR(v)) return v;
    ray_t* r = q_err_signal(QE_RETURN, v);
    ray_release(v);
    return r;
}

/* paren list literal: elements RTL, boxed build + collapse.  An ELIDED
 * element is a projection HOLE: the literal is a projection of the list
 * constructor ("omission of values results in projection",
 * releases/ChangesIn4.1.md; >8 elided = 'rank, basics/application.md).
 * EVAL_MAX_ARGS bounds a CALL's arity, which a literal has none of, so the
 * element buffer spills to the heap rather than capping the literal's length. */
static ray_t* list_lit(ray_t** e, int64_t n) {
    ray_t* stackv[EVAL_MAX_ARGS];
    ray_t** argv = stackv;
    if (n > EVAL_MAX_ARGS) {
        argv = (ray_t**)ray_alloc_raw((size_t)n * sizeof *argv);
        if (!argv) return q_err(QE_WSFULL);
    }
    ray_t* ret;
    int64_t holes = 0;
    for (int64_t i = n - 1; i >= 0; i--) {
        if (is_hole(e[i])) { argv[i] = NULL; holes++; continue; }
        argv[i] = e[i] ? q_eval_apply_concrete(q_eval(e[i])) : RAY_NULL_OBJ;
        if (RAY_IS_ERR(argv[i])) {
            ret = argv[i];
            for (int64_t j = i + 1; j < n; j++)
                if (argv[j]) ray_release(argv[j]);
            goto out;
        }
    }
    if (holes > 0) {
        if (holes > 8) { release_args(argv, n); ret = q_err(QE_RANK); goto out; }
        ray_t* ctor = q_registry_list_value();
        ret = q_eval_apply_proj_new(ctor, q_registry_row_of(ctor, Q_MONADIC), argv, n, n);
        release_args(argv, n);
        goto out;
    }
    {
        ray_t* l = ray_list_new(n > 0 ? n : 1);
        for (int64_t i = 0; i < n; i++) {
            Q_ASSERT_CONCRETE(argv[i]);        /* container-append tripwire */
            l = ray_list_append(l, argv[i]);
        }
        release_args(argv, n);
        if (RAY_IS_ERR(l)) ret = l;
        else { ret = q_list_collapse(l); ray_release(l); }
    }
out:
    if (argv != stackv) ray_free_raw(argv);
    return ret;
}

ray_t* q_eval(ray_t* node) {
    if (!node) return RAY_NULL_OBJ;
    if (RAY_IS_ERR(node)) return node;
    if (ray_eval_is_interrupted()) return q_err(QE_STOP);
    if (++g_depth > EVAL_MAX_DEPTH) { g_depth--; return q_err(QE_LIMIT); }

    ray_t* ret;
    if (node->type == -RAY_SYM) {
        ret = name_value(node, NULL);
        goto out;
    }
    /* the enlisted sym-constant unwrap (parsetrees.md:26: eval enlist`x -> `x):
     * a 1-element sym vector in tree position IS the sym atom constant */
    if (sym_const(node)) {
        ret = q_index_elem_at(node, 0);
        /* the unwrap is the enlist's inverse, so it must restore the DATA mark
         * too: without it a verb-glyph sym (`. `+) prints as a bare name-ref */
        if (ret && !RAY_IS_ERR(ret)) ret->attrs |= Q_ATTR_QUOTED;
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

        /* the `;` statement sequence — a CHAR head, like `:` return and `'`
         * signal below (q_parse.h: q_parse_is_seq_head is the one test) */
        if (q_parse_is_seq_head(h)) { ret = seq_eval(e + 1, n - 1); goto out; }

        if (nameref(h)) {
            const eval_syms_t* S = syms();
            if ((h->i64 == S->colon || h->i64 == S->gcolon) && n == 3) {
                ret = assign_eval(h->i64 == S->gcolon, e[1], e[2]);
                goto out;
            }
            if (h->i64 == S->kif)    { ret = if_eval(e + 1, n - 1, 0); goto out; }
            if (h->i64 == S->kwhile) { ret = if_eval(e + 1, n - 1, 1); goto out; }
            if (h->i64 == S->kdo)    { ret = do_eval(e + 1, n - 1); goto out; }
            if (n == 3) {
                ray_t* ma = modassign_eval(h, e[1], e[2]);
                if (ma) { ret = ma; goto out; }
            }
        }

        /* trees are DATA: the char-atom heads match by CONTENT, so an
         * eval'd `("'";`e)` signals exactly like parsed source */
        if (h && h->type == -RAY_CHARV && n == 2 &&
            (h->u8 == '\'' || h->u8 == ':')) {
            ret = h->u8 == '\'' ? signal_eval(e[1]) : return_eval(e[1]);
            goto out;
        }

        {   /* bare (iterator;F) — the derived value; F resolves as the
             * OPERAND (dyadic-preferring), never as a plain argument */
            int adv = adv_id(h);
            if (adv >= 0 && n == 2) {
                const q_op_t* frow = NULL;
                ray_t* F = operand_value(e[1], &frow);
                if (RAY_IS_ERR(F)) { ret = F; goto out; }
                ret = q_eval_apply_deriv_new(adv, F, frow);
                ray_release(F);
                goto out;
            }
        }

        /* An enlisted node is a CONSTANT: eval of a 1-element list is its item
         * UNEVALUATED — enlist is the quote of parse trees (parsetrees.md:26
         * eval enlist`a`b`c -> `a`b`c; funsql where-slots ,,(op;…) must reach
         * `?` as data).  The typed twin (,`x -> `x) is sym_const above. */
        if (n == 1) {
            ret = h ? h : RAY_NULL_OBJ;
            ray_retain(ret);
            goto out;
        }

        /* `$[c;t;f;...]` Cond — claimed BEFORE argument evaluation (lazy) */
        if (n >= 4 && dollar_head(h)) { ret = cond_eval(e + 1, n - 1); goto out; }

        /* THE application arm, strictly right to left (basics/syntax.md
         * "Precedence and order of evaluation"): arguments first, then the
         * HEAD — the node's LEFTMOST expression — so `a where (a:1 2 3)>1`
         * reads what the argument just bound.  An adverb-headed node needs no
         * arm of its own: q_eval of `(iter;F)` IS the derived value, and the
         * apply module already routes a derived carrier to q_adverb_apply. */
        int64_t argc = n - 1;
        ray_t* argv[EVAL_MAX_ARGS];
        if (argc > EVAL_MAX_ARGS) { ret = q_err(QE_RANK); goto out; }
        ray_t* err = eval_args_rtl(e + 1, argc, argv);
        if (err) { ret = err; goto out; }

        const q_op_t* row = NULL;
        ray_t* fv;
        if (!h) {
            fv = q_err(QE_TYPE);
        } else if (h->type == -RAY_SYM) {
            fv = name_value(h, &row);
        } else if (h->type == RAY_LIST || sym_const(h)) {
            fv = q_eval(h);
        } else {
            ray_retain(h);                          /* embedded value / noun */
            fv = h;
            if (h->type == RAY_UNARY || h->type == RAY_BINARY ||
                h->type == RAY_VARY) {
                q_valence_t val = (argc == 1) ? Q_MONADIC
                                : (argc == 2) ? Q_DYADIC : 0;
                if (val) row = q_registry_row_of(h, val);
            }
        }
        if (RAY_IS_ERR(fv)) { release_args(argv, argc); ret = fv; goto out; }
        ret = q_eval_apply(fv, row, argv, argc);
        release_args(argv, argc);
        ray_release(fv);
    }

out:
    g_depth--;
    return ret;
}
