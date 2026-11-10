/* q_eval walker — see q_eval.h.  jq-EvalOp shape: visit -> resolve heads
 * (a keyword head is already its value, bound at parse; a symbol is a global:
 * env, then `.z`) -> eval-time forms (`;` seq,
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
#define _GNU_SOURCE   /* pthread_getattr_np — the C stack's extent for the depth guard */

#include "qlang/q_count.h"
#include "qlang/eval/q_eval.h"
#include "qlang/eval/q_eval_internal.h"
#include "qlang/eval/q_dbg.h"             /* q_dbg_statement_pos — the statement's caret table */
#include "qlang/eval/q_pattern.h"
#include "qlang/eval/q_view.h"
#include "qlang/base/q_err.h"
#include "qlang/parse/q_parse_internal.h"
#include "qlang/q_ops.h"
#include "qlang/q_registry.h"
#include "qlang/q_registry_internal.h"   /* q_type_strict_i64 — the do-count judgment */
#include "qlang/q_prim.h"                /* q_join_amend — the `,:` home; q_list_uncollapse — the sym-vector tree */
#include "qlang/q_dotz.h"
#include "qlang/q_env.h"
#include "qlang/ops/q_index.h"
#include "qlang/ops/q_dollar.h"   /* q_dollar — the cast home `x.attr` rides */
#include "qlang/net/q_wirefile.h"  /* q_wirefile_read — `get `:file */
#include "qlang/net/q_wire.h"      /* q_wire_keyword_lambda — `value` of a keyword with no wire code */
#include "qlang/io/q_provider.h"   /* q_provider_get_carrier — `get `:pq:duckdb:db:t/ */
#include "qlang/io/q_splay.h"      /* q_splay_get — `get `:dir/ maps; value/amend read the aux path */
#include "qlang/io/q_io.h"         /* q_io_resource_table — `get `:f.csv reads the format the ending names */
#include "qlang/q_ctx.h"           /* q_ctx_eval_src — multi-line text under the value law */
#include "ops/ops.h"
#include "table/sym.h"             /* ray_read_sym — sym vectors are width-adaptive */
#include <signal.h>
#include <string.h>
#if defined(RAY_OS_WINDOWS)
#include <windows.h>
#else
#include <pthread.h>
#endif

#define EVAL_MAX_ARGS  60
#define EVAL_MAX_DEPTH 2048
#define EVAL_DBG_DEPTH 64        /* a q)) line's headroom past the depth it suspended at */
_Static_assert(Q_EVAL_DEPTH_CEIL == EVAL_MAX_DEPTH + 8 * EVAL_DBG_DEPTH, "nested suspensions share one bounded reserve");

static _Thread_local int g_depth;
static _Thread_local int g_depth_cap = EVAL_MAX_DEPTH;

/* The C stack guard: recursion through adverbs and kernels spends far more stack per walker level than `{.z.s[]}`
 * does, so a lambda application also stops at a floor near the stack's end — one compare per call.  UINTPTR_MAX
 * until this thread's extent is known (the first compare learns it); 0 where it cannot be learned. */
static _Thread_local uintptr_t g_stack_floor = UINTPTR_MAX;
static _Thread_local uintptr_t g_stack_lo, g_stack_margin, g_stack_dbg;   /* its end, a kernel's margin, q))'s share */
static _Thread_local int       g_stack_known;

static void stack_learn(void) {
    g_stack_known = 1;
    g_stack_floor = 0;
    uintptr_t lo = 0, size = 0;
#if defined(RAY_OS_WINDOWS)
    ULONG_PTR l, h;
    GetCurrentThreadStackLimits(&l, &h);
    lo = (uintptr_t)l;
    size = (uintptr_t)(h - l);
#elif defined(RAY_OS_MACOS)
    size = (uintptr_t)pthread_get_stacksize_np(pthread_self());
    lo = (uintptr_t)pthread_get_stackaddr_np(pthread_self()) - size;
#else
    pthread_attr_t a;
    void* addr = NULL;
    size_t sz = 0;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        pthread_attr_getstack(&a, &addr, &sz);
        pthread_attr_destroy(&a);
    }
    lo = (uintptr_t)addr;
    size = sz;
#endif
    if (!lo || size < (1u << 20)) return;
    g_stack_lo = lo;
    g_stack_margin = size / 16 > (256u << 10) ? size / 16 : (256u << 10);
    g_stack_dbg = size / 8;
    g_stack_floor = lo + g_stack_margin + g_stack_dbg;
}

int q_eval_stack_low(void) {
    if (RAY_LIKELY((uintptr_t)__builtin_frame_address(0) >= g_stack_floor)) return 0;
    if (!g_stack_known) stack_learn();
    return (uintptr_t)__builtin_frame_address(0) < g_stack_floor;
}
_Thread_local const ray_t* q_eval_node;

volatile sig_atomic_t q_eval_stop_pending;
static volatile sig_atomic_t g_unserviced;   /* reset only where a seam SERVED the stop: a trapped loop keeps counting */

int q_eval_interrupt_post(void) {
    ray_request_interrupt();
    q_eval_stop_pending = 1;
    return __atomic_add_fetch(&g_unserviced, 1, __ATOMIC_ACQ_REL);
}

int q_eval_interrupt_consume(void) {
    int n = __atomic_exchange_n(&q_eval_stop_pending, 0, __ATOMIC_ACQ_REL);
    ray_clear_interrupt();
    return n != 0;
}

void q_eval_interrupt_served(void) { g_unserviced = 0; }

int q_eval_active(void) { return g_depth > 0; }

q_eval_reserve_t q_eval_depth_reserve(void) {
    q_eval_reserve_t prev = { g_depth_cap, g_stack_floor };
    int want = g_depth + EVAL_DBG_DEPTH;
    if (want > g_depth_cap) g_depth_cap = want < Q_EVAL_DEPTH_CEIL ? want : Q_EVAL_DEPTH_CEIL;
    if (g_stack_known && g_stack_floor) {   /* an eighth of q))'s share past here, never into the kernels' margin */
        uintptr_t sp = (uintptr_t)__builtin_frame_address(0), step = g_stack_dbg / 4, hard = g_stack_lo + g_stack_margin;
        uintptr_t want_sp = sp > hard + step ? sp - step : hard;
        if (want_sp < g_stack_floor) g_stack_floor = want_sp;
    }
    return prev;
}

void q_eval_depth_restore(q_eval_reserve_t r) {
    g_depth_cap = r.cap;
    g_stack_floor = r.floor;
}

/* Fixed sym ids, interned ONCE per runtime (the env.c ipc-hook-syms
 * discipline).  Process-global like the registry itself — one live q runtime
 * per process (__RUNTIME).  The sym table is recreated per runtime, so
 * q_runtime_destroy resets via q_eval_syms_reset — a surviving cache would
 * hand the next runtime stale ids and control forms would degrade to 'name. */
typedef struct {
    int64_t adv[6];
    int64_t semi, dollar, kif, kwhile, kdo;
    int     ready;
} eval_syms_t;
static eval_syms_t g_syms;

static const eval_syms_t* syms(void) {
    if (!g_syms.ready) {
        for (int i = 0; i < 6; i++)
            g_syms.adv[i] = ray_sym_intern_runtime(ADVERB_NAMES[i],
                                                   strlen(ADVERB_NAMES[i]));
        g_syms.semi   = ray_sym_intern_runtime(";", 1);
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
    return x && x->type == RAY_SYM && q_count(x) == 1;
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

/* A symbol is ONLY ever a global lookup (owner kdb transcript 2026-11-07): env (scopes + globals), then `.z.*`.
 * A keyword exists only at parse, which binds its value, so neither the registry nor `.q` is read here.  Owned
 * result or NULL. */
static ray_t* resolve(ray_t* sym) {
    ray_t* v = q_env_resolve(sym->i64);      /* owned (or an owned error) */
    if (v) {
        if (q_view_is(v)) q_eval_node = sym;  /* a recalc walks the view's own tree: its caret stays on the name */
        return q_view_deref(v);
    }
    return q_dotz_resolve(sym->i64);         /* owned */
}

/* Dot notation whose last segment names a cast target IS that cast:
 * `time.minute` ~ `` `minute$time ``, `t.hh` ~ `` `hh$t `` (ref/cast.md
 * Temporal, `` `year`dd`mm`hh`uu`ss$2015.10.28D03:55:58 ``).  The `$` home
 * owns which designators exist — BOTH readings, the type targets and the
 * `uu`/`year`/`week` components, so it is `$` entire and not q_dollar_cast.
 *
 * Runs LAST, after resolve(), so a dict key or table column of that name wins.
 * On a temporal base this is THE accessor on every path (global, qSQL column,
 * lambda local) and the cast's 'type refusal is the answer (`m.hh`); any
 * other failure leaves the name error.  Only a field name reaches `$`, so
 * `x.dom` never enumerates. */
ray_t* q_eval_dot_cast(int64_t id) {
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
        ray_t* attr = ray_sym(ray_sym_intern_runtime(p + cut + 1, (int64_t)(n - cut - 1)));
        if (base && !RAY_IS_ERR(base) && !q_view_is(base) && q_dollar_names_field(attr)) {
            r = q_dollar(attr, base);
            int8_t bt = base->type < 0 ? (int8_t)-base->type : base->type;
            const char* cls = r ? q_err_class(r) : NULL;
            int refusal = (RAY_IS_TEMPORAL(bt) || RAY_IS_TEMPORALF(bt)) && cls && !strcmp(cls, "type");
            if (r && RAY_IS_ERR(r) && !refusal) { ray_release(r); q_err_drop(); r = NULL; }
        }
        ray_release(attr);
        if (base) ray_release(base);
    }
    ray_release(nm);
    return r;
}

static ray_t* name_value(ray_t* sym) {
    ray_t* v = resolve(sym);
    if (!v) v = q_eval_dot_cast(sym->i64);
    return v ? v : name_error(sym->i64);
}

static ray_t* operand_value(ray_t* F, const q_op_t** row_out) {
    if (row_out) *row_out = NULL;
    if (!F) return q_err(QE_TYPE);
    ray_t* v;
    if (F->type == -RAY_SYM) {
        v = name_value(F);
    } else if (F->type == RAY_LIST) {
        v = q_eval(F);
    } else {
        ray_retain(F);
        v = F;
    }
    /* the manifest row travels with the VALUE, however the operand was spelt:
     * without it the derived call loses `family` and its container lift */
    if (row_out && v && !RAY_IS_ERR(v))
        *row_out = q_registry_operand_row(v);
    return v;
}

/* args RIGHT-to-left into argv (owned).  On error the already-evaluated tail (indices > i) is released.  A
 * `;`-elided slot stays C NULL — open — for the index home and the postfix seam; a lone hole is a value like any
 * other, judged by the apply module's hole law. */
static ray_t* eval_args_rtl(ray_t** e, int64_t argc, ray_t** argv) {
    for (int64_t i = argc - 1; i >= 0; i--) {
        if (e[i] == Q_HOLE_OBJ && argc > 1) { argv[i] = NULL; continue; }
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

/* ray_sym_str is BORROWED (PLAN.md register, 2026-07-30) — no release here */
static int sym_compound(int64_t id) {
    ray_t* s = ray_sym_str(id);
    return s && ray_str_len(s) > 0 && memchr(ray_str_ptr(s), '.', (size_t)ray_str_len(s)) != NULL;
}

/* THE locality law (function-notation.md "Name scope", owner ruling 2026-09-11): a name with no dot is local iff the
 * running lambda's frame holds it — its params and parse-time locals, seeded by lambda_call — else the write is
 * global; a compound name is relative to the K-tree, never local (basics/syntax.md:623).  The depth gate keeps a
 * script load (floor raised) and a view recalc global. */
static int write_is_local(int64_t sym) {
    return !sym_compound(sym) && q_eval_apply_frame_depth() > 0 && q_env_local_get(sym) != NULL;
}

/* A colon assignment is judged by its SPELLING, a `set` by its target (owner rulings 2026-09-23) — q_env_set sees
 * only the re-rooted name.  A syntax word is the one spelling a non-root `\d` may take as a context MEMBER: it
 * carries no value, so nothing is overwritten; a lambda local of that spelling stays refused. */
int q_eval_assign_locked(int64_t sym) {
    int global = !write_is_local(sym);
    if (global && q_env_scope_ctx() && q_registry_is_syntax_word(sym)) return 0;
    return q_registry_is_reserved(sym) || (global && q_registry_dotq_ext(sym));
}

/* `name::v` at the root binds a reserved name's root slot as `set` does; bare `name` still parses to the keyword */
int q_eval_global_assign_locked(int64_t sym) {
    return q_env_scope_ctx() && q_eval_assign_locked(sym);
}

/* undo a park (q_env_take / q_env_local_take) on a failed write: the value
 * read goes back into the slot the write would have rebound */
static void unpark(int local, int64_t sym, ray_t* v) {
    if (local) q_env_local_set(sym, v);
    else q_env_bind(sym, v);
}

/* `a[i;…]:v` / `a[i;…]op:v` (ref/assign.md Indexed assign): rhs first, then
 * indices RTL; the write IS the one amend home — resolve the name, amend the
 * path (`a[i]op:v` ≡ .[a;i;op;v], so repeat-accumulation holds), rebind under
 * THE locality law (write_is_local).  Plain form returns the assigned rhs;
 * an op-assign values as the NEW a[i] (ref/assign.md: `1+a[2]+:5` is 8). */
static ray_t* indexed_assign(ray_t* target, ray_t* opv, ray_t* rhs) {
    ray_t** te = (ray_t**)ray_data(target);
    int64_t k = q_count(target) - 1;
    if (k < 1 || k > EVAL_MAX_ARGS || !nameref(te[0]))
        return q_err(QE_NYI);
    if (q_eval_assign_locked(te[0]->i64)) return q_err(QE_ASSIGN);
    ray_t* rv = q_eval(rhs);
    if (RAY_IS_ERR(rv)) return rv;
    rv = q_eval_apply_concrete(rv);
    if (RAY_IS_ERR(rv)) return rv;
    ray_t* idxv[EVAL_MAX_ARGS];
    ray_t* err = eval_args_rtl(te + 1, k, idxv);
    if (err) { ray_release(rv); return err; }
    ray_t* ret = NULL;
    ray_t* cur = name_value(te[0]);
    if (RAY_IS_ERR(cur)) ret = cur;
    else if (q_splay_table_path(cur)) {          /* a mapped global takes no write (kb/splayed-tables.md:350) */
        ray_release(cur);
        ret = q_err(QE_SPLAY);
    }
    if (!ret) {
        int local = write_is_local(te[0]->i64);
        /* the binding this write REPLACES double-counts the value, so the
         * amend would copy the whole vector: park it (q_env.h q_env_take) */
        int stole = local ? q_env_local_take(te[0]->i64, cur)
                          : q_env_take(te[0]->i64, cur);
        int was = q_index_in_place(1);
        ray_t* amended = q_index_amend(cur, idxv, k, opv, rv);
        q_index_in_place(was);
        if (RAY_IS_ERR(amended)) {
            if (stole) unpark(local, te[0]->i64, cur);
            ray_release(cur);
            ret = amended;
        }
        else {                                       /* cur consumed on success */
            if (!local) q_view_set_index(idxv, k);   /* .z.vs y (ref/dotz.md) */
            ray_err_t e2 = local ? q_env_local_set(te[0]->i64, amended)
                                 : q_env_settle(te[0]->i64, stole, amended);
            q_view_set_index(NULL, 0);               /* never leaks to the next set */
            if (e2 != RAY_OK) ret = q_env_err(e2);
            else if (!opv) { ray_retain(rv); ret = rv; }
            else ret = q_eval_apply_concrete(q_eval_apply_value(amended, idxv, k));
            ray_release(amended);
        }
    }
    release_args(idxv, k);
    ray_release(rv);
    return ret;
}

static ray_err_t name_set(int64_t sym, ray_t* v) {
    return write_is_local(sym) ? q_env_local_set(sym, v) : q_env_set(sym, v);
}

/* A pattern target (basics/pattern.md): the whole match is collected before the first write, so a failed one
 * assigns nothing; the value is the whole right side. */
static ray_t* pattern_assign(ray_t* target, ray_t* rhs) {
    ray_t* v = q_eval_apply_concrete(q_eval(rhs));
    if (RAY_IS_ERR(v)) return v;
    ray_t* binds = ray_list_new(4);
    if (!binds || RAY_IS_ERR(binds)) { ray_release(v); return binds ? binds : q_err(QE_OOM); }
    ray_t* err = q_pattern_collect(target, v, &binds);
    ray_t** b = (ray_t**)ray_data(binds);
    int64_t n = q_count(binds);
    for (int64_t i = 0; !err && i < n; i += 2)
        if (q_eval_assign_locked(b[i]->i64)) err = q_err(QE_ASSIGN);
    for (int64_t i = 0; !err && i < n; i += 2) {
        ray_err_t e = name_set(b[i]->i64, b[i + 1]);
        if (e != RAY_OK) err = q_env_err(e);
    }
    ray_release(binds);
    if (!err) return v;
    ray_release(v);
    return err;
}

/* `:`/`::` assignment — both take THE locality law: a `:` target is in the frame by construction (a parse-time
 * local, seeded at entry), `::` on a param/local writes the local (function-notation.md "Name scope"), anything
 * else is the global.  Returns the assigned value. */
static ray_t* assign_eval(ray_t* target, ray_t* rhs, int global) {
    q_assign_target_t k = q_parse_assign_target(target);
    if (k == Q_TARGET_NONE) return q_err(QE_ASSIGN);
    /* the patterns outside the list slice, and `::` onto one, stay refused */
    if (k == Q_TARGET_PATTERN) return !global && q_pattern_supported(target) ? pattern_assign(target, rhs) : q_err(QE_NYI);
    if (k == Q_TARGET_INDEX) return indexed_assign(target, NULL, rhs);
    if (global ? q_eval_global_assign_locked(target->i64) : q_eval_assign_locked(target->i64)) return q_err(QE_ASSIGN);
    ray_t* v = q_eval_apply_concrete(q_eval(rhs));    /* boundary seam: assignment */
    if (RAY_IS_ERR(v)) return v;
    Q_ASSERT_CONCRETE(v);                  /* env-set tripwire */
    ray_err_t err = name_set(target->i64, v);
    if (err != RAY_OK) { ray_release(v); return q_env_err(err); }
    return v;
}

/* ===== `value` on a lambda (ref/value.md `## Lambda`) ====================
 * Published shape: (bytecode;params;locals;(namespace,globals);constants…;
 * m;n;f;l;s).  A tree-walker has no bytecode and no source-position map, so
 * slot 0 and m are EMPTY rather than invented; n/f/l keep the doc's own "not
 * applicable" values (()/""/-1) until an assignment stamps a name and the
 * loader carries a file.  Scope follows function-notation.md "Name scope": a
 * `:` target is local for the WHOLE body (the CARRIER-BORNE locals slot,
 * q_eval_lambda_locals — the same set lambda_call seeds), a `::` target is
 * global unless the name is a local, compound targets and every other name are
 * global references.  Verbs are fn
 * VALUES after parse so they never read as names, and a nested lambda carrier
 * is opaque: its locals are its own. */

typedef struct {
    ray_t *loc, *glb, *ref, *con;
    int    oom;
} lam_scan_t;

int q_eval_symvec_has(ray_t* v, int64_t id) {
    if (!v || v->type != RAY_SYM) return 0;
    const void* d = ray_data(v);
    for (int64_t i = 0, n = q_count(v); i < n; i++)
        if (ray_read_sym(d, i, RAY_SYM, v->attrs) == id) return 1;
    return 0;
}

static void sym_add(ray_t** v, int64_t id, int* oom) {
    if (*oom || !*v || q_eval_symvec_has(*v, id)) return;
    ray_t* n = ray_vec_append(*v, &id);
    if (!n) { *oom = 1; return; }
    *v = n;
}

int q_eval_ctl_sym(int64_t id) {
    const eval_syms_t* S = syms();
    return id == S->semi || id == S->kif || id == S->kwhile || id == S->kdo;
}

int q_eval_fn_value(ray_t* x) {
    return x && (x->type == RAY_LAMBDA || x->type == RAY_UNARY ||
                 x->type == RAY_BINARY || x->type == RAY_VARY ||
                 x->type == RAY_QFN);
}

/* a pattern target's names are locals, as `n:` is; a compound name stays the global its spelling makes it */
static void lam_scan_local(int64_t sym, void* ctx) {
    lam_scan_t* s = ctx;
    if (!sym_compound(sym)) sym_add(&s->loc, sym, &s->oom);
}

static void lam_scan(ray_t* n, lam_scan_t* s) {
    if (!n || s->oom || q_eval_fn_value(n)) return;
    if (nameref(n)) {
        /* a keyword head is a verb, not a free global: the registry owns the
         * name (assignment to it is 'assign), so it can never be a user's */
        if (!q_eval_ctl_sym(n->i64) && !q_registry_is_reserved(n->i64))
            sym_add(&s->ref, n->i64, &s->oom);
        return;
    }
    if (n->type != RAY_LIST) {
        if (!s->con || Q_IS_GENERIC_NULL(n)) return;
        ray_t* c = ray_list_append(s->con, n);
        if (!c) { s->oom = 1; return; }
        s->con = c;
        return;
    }
    int64_t k = q_count(n);
    ray_t** e = (ray_t**)ray_data(n);
    if (k == 0) return;
    q_assign_head_t asg = k == 3 ? q_parse_assign_head(e[0], NULL, NULL) : Q_HEAD_NONE;
    if (asg == Q_HEAD_ASSIGN || asg == Q_HEAD_GLOBAL) {
        if (nameref(e[1])) {
            int global = asg == Q_HEAD_GLOBAL || sym_compound(e[1]->i64);
            sym_add(global ? &s->glb : &s->loc, e[1]->i64, &s->oom);
        } else if (asg == Q_HEAD_ASSIGN && q_pattern_supported(e[1])) {
            q_pattern_names(e[1], lam_scan_local, s);
        } else {
            lam_scan(e[1], s);
        }
        lam_scan(e[2], s);
        return;
    }
    if (asg == Q_HEAD_THROUGH) {   /* `x op:y` reads x; a marked `<>:` head is no name */
        lam_scan(e[1], s);
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

/* the `:` targets over the WHOLE body (function-notation.md:155 "identified on parsing"), params excluded */
ray_t* q_eval_lambda_locals(ray_t* params, ray_t* body) {
    lam_scan_t s = { ray_sym_vec_new(RAY_SYM_W64, 4), NULL, NULL, NULL, 0 };
    if (!s.loc) return NULL;
    lam_scan(body, &s);
    ray_t* out = s.oom ? NULL : ray_sym_vec_new(RAY_SYM_W64, 4);
    for (int64_t i = 0, n = out ? q_count(s.loc) : 0; i < n && out; i++) {
        int64_t id = ray_read_sym(ray_data(s.loc), i, RAY_SYM, s.loc->attrs);
        if (!q_eval_symvec_has(params, id)) out = ray_vec_append(out, &id);
    }
    ray_release(s.loc);
    return out;
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
    /* a `2:`-loaded function reads out as the call that made it (PR 1 of the `2:` programme) */
    if (q_eval_apply_carrier_kind(v) == Q_EVAL_CAR_KFN) {
        int64_t rank = 0, lib = 0, sym = 0;
        q_eval_apply_kfn_parts(v, NULL, &rank, &lib, &sym);
        ray_t* out = list_put(list_put(list_put(ray_list_new(3), ray_sym(lib)), ray_sym(sym)),
                              ray_i64(rank));
        return out;
    }
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
        /* an UNWRITTEN trailing slot is rank padding, not a bound argument — `+[2]` reads out as the 2-item
         * (+;2); a written hole (`{x+y}[1;]`) is the hole value and stays (D0922h), but a primitive's trailing
         * holes read out as padding too (`value +[2;]` is (+;2)) unless every slot is one */
        int64_t n = q_eval_apply_proj_nslots(v);
        while (n > 0 && !q_eval_apply_proj_arg(v, n - 1)) n--;
        if (q_eval_apply_is_fnval(h) && h != q_registry_list_value()) {
            int64_t k = n;
            while (k > 0 && Q_IS_HOLE(q_eval_apply_proj_arg(v, k - 1))) k--;
            if (k > 0) n = k;
        }
        ray_t* out = list_put_borrowed(ray_list_new(n + 1), h);
        for (int64_t i = 0; i < n; i++)
            out = list_put_borrowed(out, q_eval_apply_proj_arg(v, i));
        return out;
    }
    /* KFN answered above; a bare iterator stays deferred; a foreign is opaque */
    case Q_EVAL_CAR_NONE: case Q_EVAL_CAR_LAMBDA: case Q_EVAL_CAR_ITER:
    case Q_EVAL_CAR_VIEW: case Q_EVAL_CAR_KFN: case Q_EVAL_CAR_FOREIGN: break;
    }
    return NULL;
}

static ray_t* lambda_structure(ray_t* v) {
    ray_t *params = NULL, *body = NULL, *ctx = NULL;
    if (!q_eval_apply_lambda_parts(v, &params, &body, &ctx)) return NULL;
    ray_t* locals = q_eval_apply_lambda_locals(v);                          /* borrowed */

    lam_scan_t s = { NULL, ray_sym_vec_new(RAY_SYM_W64, 4), ray_sym_vec_new(RAY_SYM_W64, 4),
                     ray_list_new(1), 0 };
    if (!s.glb || !s.ref || !s.con) s.oom = 1;
    lam_scan(body, &s);

    /* a referenced name is global unless it is a parameter or a body local */
    for (int64_t i = 0, n = s.ref ? q_count(s.ref) : 0; i < n && !s.oom; i++) {
        int64_t id = ray_read_sym(ray_data(s.ref), i, RAY_SYM, s.ref->attrs);
        if (q_eval_symvec_has(params, id) || q_eval_symvec_has(locals, id)) continue;
        sym_add(&s.glb, id, &s.oom);
    }
    ray_t* nsg = ray_sym_vec_new(RAY_SYM_W64,
                                 1 + (s.glb ? q_count(s.glb) : 0));
    if (!nsg) s.oom = 1;
    else {
        int64_t id = ns_bare(ctx);
        nsg = ray_vec_append(nsg, &id);
        for (int64_t i = 0, n = s.glb ? q_count(s.glb) : 0; i < n && nsg; i++) {
            int64_t g = ray_read_sym(ray_data(s.glb), i, RAY_SYM, s.glb->attrs);
            if (q_eval_symvec_has(locals, g)) continue;     /* `w::9` on a body local writes the local */
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
    if (locals) ray_retain(locals);
    out = list_put(out, locals ? locals : ray_sym_vec_new(RAY_SYM_W64, 1));
    out = list_put(out, nsg);   nsg = NULL;
    for (int64_t i = 0, n = s.con ? q_count(s.con) : 0; i < n && out; i++) {
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

    ray_release(s.glb); ray_release(s.ref); ray_release(s.con); ray_release(nsg);
    return out ? out : q_err(QE_WSFULL);
}

/* THE statement home — see q_eval.h.  parse -> view intercept -> eval -> the
 * assignment law, and every door that runs q TEXT (the repl line, `value` of a
 * string, the IPC string request) gets its value from here.
 *
 * The assignment law is a STATEMENT law, never an eval-tree one: q_eval recurses
 * on itself, so `(a:4)+1` is 5 and `x:a:4` still sets x to 4 — only the value the
 * statement HANDS BACK is the generic null (owner ruling 2026-09-05).  A view
 * definition answers the same way (learn/views.md#parse: only a statement can
 * carry that distinction, which is why the intercept lives here and not in
 * q_eval). */
ray_t* q_eval_statement(const char* src, int* parsed) {
    if (parsed) *parsed = 1;
    int64_t scope = q_env_scope(Q_ENV_SCOPE_SESSION);   /* fresh text resolves at the session `\d`, as a load's frames do */
    ray_t* pos = NULL;
    ray_t* ast = q_parse_at(src, &pos);
    if (!ast || RAY_IS_ERR(ast)) {
        if (parsed) *parsed = 0;         /* the statement never ran — see q_eval.h */
        ray_t* e = ast ? ast : q_err(QE_PARSE);
        if (!q_dbg_statement_pos(pos)) {      /* a text run inside a statement: the frame above its runner */
            q_dbg_parse_fail(e, src, (int64_t)strlen(src), pos, 0);
            if (pos) ray_release(pos);
        }                                     /* else a die the parser located carets frame [0] */
        q_env_scope(scope);
        return e;
    }
    ray_t* text = NULL;
    if (!q_dbg_statement_pos(pos)) {      /* a text run INSIDE a statement (`value "…"`) is its own frame */
        text = ray_charv(src, (int64_t)strlen(src));
        if (!text || RAY_IS_ERR(text)) {
            ray_release(ast);
            if (pos) ray_release(pos);
            q_env_scope(scope);
            return text ? text : q_err(QE_OOM);
        }
        q_dbg_text_push(text, pos);
    }
    ray_t* r;
    int silent = 1;
    if (!q_view_intercept(ast, src, &r)) {
        silent = q_parse_is_assign(ast);
        r = q_eval(ast);
    }
    ray_release(ast);
    if (text) {
        r = q_dbg_filter(r, NULL, NULL, 0);   /* an error no application observed (an undefined name) is the text's */
        q_dbg_frame_pop();
        ray_release(text);
        if (pos) ray_release(pos);
    }
    q_env_scope(scope);
    if (silent && r && !RAY_IS_ERR(r)) {
        ray_release(r);
        ray_retain(RAY_NULL_OBJ);
        r = RAY_NULL_OBJ;
    }
    return r;
}

q_eval_frames_t q_eval_frames_suspend(void) {
    q_eval_frames_t f;
    f.floor  = q_eval_apply_frame_floor(-1);
    f.ffloor = q_env_frame_floor(-1);
    return f;
}

void q_eval_frames_resume(q_eval_frames_t f) {
    q_env_frame_floor(f.ffloor);
    q_eval_apply_frame_floor(f.floor);
}

/* A keyword with no wire code is, to `value`, the root lambda it travels as (Dffqch) */
static ray_t* keyword_lambda_value(const char* src) {
    int64_t scope = q_env_scope(0);
    ray_t* ast = q_parse(src);
    ray_t* lam = !ast ? q_err(QE_PARSE) : RAY_IS_ERR(ast) ? ast : q_eval(ast);
    if (ast && !RAY_IS_ERR(ast)) ray_release(ast);
    q_env_scope(scope);
    if (RAY_IS_ERR(lam)) return lam;
    ray_t* r = q_eval_value_wrap(lam);
    ray_release(lam);
    return r;
}

/* `value`/`get` (ref/value.md; get is the synonym).  q `eval` needs no body
 * here — it is .q.eval:(-6!), the q_bang.c arm over q_eval.  value applies
 * ONCE, non-recursively: a string parses+evaluates in the current context, a
 * `:path sym READS THAT FILE (ref/get.md — kdb's get IS value), any other
 * sym atom names a variable, a dict yields its values, () is itself (owner
 * ruling 2026-10-09), and a list applies its first item (string/sym heads
 * evaluated first, ref/value.md) to the rest AS LITERALS — nested trees stay data.  A sym vector of two or more is
 * that list; value of any OTHER typed vector (incl. the enlisted constant
 * ,`x) is doc-silent: 'nyi, never a guess. */
/* docs/text-entry-law.md: `value "…"` is the IPC column */
ray_t* q_eval_value_wrap(ray_t* x) {
    if (!x) return q_err(QE_TYPE);
    if (RAY_IS_NULL(x)) return ray_i64(0);   /* (::) IS unary primitive 0 —
                                              * ref/value.md operator arm */
    if (q_enum_is(x)) return q_enum_decay(x);   /* `value e` -> resolved values,
                                                 * or bare positions for a
                                                 * reference domain (R1/R4;
                                                 * wp/foreign-keys.md:188-213) */
    if (x->type == RAY_TABLE && q_splay_table_path(x))   /* the flip's dict value: its hsym (ref/value.md:20) */
        return ray_sym(q_splay_table_path(x));
    if (x->type == -RAY_SYM) {
        ray_t* m = q_splay_get(x);           /* NULL unless a `:dir/ table folder */
        if (m) return m;
        ray_t* pc = q_provider_get_fresh(x);     /* `:pq:duckdb:db:t/ -> the carrier, the table read afresh;
                                                  * connection form -> 'domain */
        if (pc) return pc;
        ray_t* rt = q_io_resource_table(x);  /* `:f.csv etc.: the ending `select from` speaks */
        if (rt) return rt;
        ray_t* f = q_wirefile_read(x);       /* NULL unless a `:path sym */
        if (f) return f;
        /* "the name of a GLOBAL variable" (ref/get.md:22): the caller's locals are behind the floor, as for a
         * script load (ctx_run_script) — a lambda's own `t` never shadows the table `t` a name or a string asks for
         * (function-notation.md:153 "strictly local: invisible to other functions applied during evaluation") —
         * and a handle resolves at the session `\d`, as the string form below does (q_env.h, the handle lane) */
        q_eval_frames_t frames = q_eval_frames_suspend();
        int64_t scope = q_env_scope(Q_ENV_SCOPE_SESSION);
        ray_t* v = name_value(x);
        q_env_scope(scope);
        q_eval_frames_resume(frames);
        return v;
    }
    if (x->type == RAY_CHARV || x->type == -RAY_CHARV || x->type == -RAY_STR) {
        const char* p; int64_t n;
        if (!q_str_text_bytes(x, &p, &n)) return q_err(QE_TYPE);
        q_eval_frames_t frames = q_eval_frames_suspend();
        ray_t* r = q_ctx_eval_src(p, (size_t)n);
        q_eval_frames_resume(frames);
        return r;
    }
    if (x->type == RAY_DICT) {
        ray_t* v = ray_dict_vals(x);
        ray_retain(v);
        return v;
    }
    if (x->type == RAY_SYM && q_count(x) > 1) {   /* what `(`f;`)` collapses to (TorQ torq.q:678 `value (`init;`)`) */
        ray_t* l = q_list_uncollapse(x);
        ray_t* r = RAY_IS_ERR(l) ? l : q_eval_value_wrap(l);
        if (!RAY_IS_ERR(l)) ray_release(l);
        return r;
    }
    if (x->type == RAY_LIST && q_count(x) == 0) { ray_retain(x); return x; }
    if (x->type == RAY_LIST && q_count(x) >= 1) {
        int64_t argc = q_count(x) - 1;
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
        char kw[Q_WIRE_KW_LAMBDA_CAP];
        if (q_wire_keyword_lambda(x, kw, sizeof kw)) return keyword_lambda_value(kw);
    }
    if (x->type < 0) return q_err(QE_TYPE);   /* any other atom: SO 64109959 "'value 42' results in a type error" */
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
        if (q_eval_interrupt_take() && (err = q_dbg_stop()))   /* an empty body never reaches the walker's check */
            return err;
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
        if (q_eval_interrupt_take() && (err = q_dbg_stop())) return err;
    }
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* modified assignment `x op: y` == `x: x op y` — opv is op's dyadic value, row its manifest row */
static ray_t* modassign_eval(ray_t* opv, const q_op_t* row, ray_t* target, ray_t* rhs) {
    q_assign_target_t k = q_parse_assign_target(target);
    if (k == Q_TARGET_INDEX) return indexed_assign(target, opv, rhs);
    if (k == Q_TARGET_PATTERN) return q_err(QE_NYI);
    if (k != Q_TARGET_NAME) return q_err(QE_ASSIGN);
    if (q_eval_assign_locked(target->i64)) return q_err(QE_ASSIGN);
    ray_t* rv = q_eval(rhs);
    if (RAY_IS_ERR(rv)) return rv;
    ray_t* cur = name_value(target);
    /* undefined name: the op's identity element is the default (ref/assign.md
     * "Assign through operator"); an op without one keeps signalling 'name */
    int passes = 0;
    if (RAY_IS_ERR(cur) && q_err_is(cur, QE_NAME) && row) {
        ray_t* id = q_ops_identity(row->name, QI_VALUE);
        passes = !id && q_ops_identity_is_null(row->name);
        if (id || passes) { q_err_drop(); ray_error_free(cur); cur = id; }
    }
    if (RAY_IS_ERR(cur)) { ray_release(rv); return cur; }
    int local = write_is_local(target->i64);
    int join = row && !strcmp(row->name, ",");
    int stole = 0;
    ray_t* nv;
    if (join && q_enum_is(cur)) {
        /* `e,:y` ENFORCES the domain in place (R6: el,:`apple coerces,
         * el,:`zz is 'cast) where plain `,` decays — the write path is the
         * one that guards the column's meaning */
        int64_t dom = q_enum_domain(cur);
        nv = q_enum_stamp(q_enum_col_concat(cur, rv), dom);
    } else if (join && !q_splay_table_path(cur)) {
        /* `x,:y` on a name IS the by-name append (`x upsert y <=> x,y`), and
         * the Append home (q_join_amend) owns its type law: park the binding
         * as indexed_assign does so x can grow where it stands.  An enum
         * payload decays first, as the apply catalogue would (`,` keeps 20h
         * only for a same-domain pair, and that pair took the arm above) */
        rv = q_eval_apply_concrete(rv);
        if (!RAY_IS_ERR(rv) && q_enum_is(rv)) { ray_t* d = q_enum_decay(rv); ray_release(rv); rv = d; }
        if (RAY_IS_ERR(rv)) { ray_release(cur); return rv; }
        stole = local ? q_env_local_take(target->i64, cur)
                      : q_env_take(target->i64, cur);
        int was = q_index_in_place(1);
        nv = q_join_amend(&cur, rv, stole);
        q_index_in_place(was);
        if (stole && RAY_IS_ERR(nv)) unpark(local, target->i64, cur);
        else if (stole) cur = NULL;                  /* consumed */
    } else if (passes) {
        ray_retain(rv);
        nv = rv;
    } else {
        ray_t* av[2] = { cur, rv };
        nv = q_eval_apply(opv, row, av, 2);
    }
    if (cur) ray_release(cur);
    ray_release(rv);
    if (RAY_IS_ERR(nv)) return nv;
    nv = q_eval_apply_concrete(nv);
    if (RAY_IS_ERR(nv)) return nv;
    ray_err_t err = local ? q_env_local_set(target->i64, nv)
                          : q_env_settle(target->i64, stole, nv);
    if (err != RAY_OK) { ray_release(nv); return q_env_err(err); }
    return nv;                                       /* q returns the NEW value */
}

/* Signal `'x` / explicit return `:x` — SYNTAX forms beside $/if/do/while,
 * never verbs (ref/signal.md: "not an operator"). */

/* `'x`: symbol atom or string, else 'stype (ref/signal.md Restrictions);
 * signals the text */
static ray_t* signal_text(ray_t* v) {   /* a string or symbol's text, else NULL */
    if (v->type == RAY_CHARV) { ray_retain(v); return v; }
    return v->type == -RAY_SYM ? q_str_charv_of_str(ray_sym_str(v->i64)) : NULL;
}

/* `'(msg;bt)` re-signals msg with a saved backtrace, `'(msg;())` plainly (SO 65303811) */
static ray_t* signal_eval(ray_t* node, ray_t* expr) {
    ray_t* v = q_eval_apply_concrete(q_eval(expr));
    if (RAY_IS_ERR(v)) return v;
    ray_t* bt = v->type == RAY_LIST && q_count(v) == 2 ? ray_list_get(v, 1) : NULL;
    ray_t* txt = signal_text(bt ? ray_list_get(v, 0) : v);
    q_eval_node = node;                        /* the caret sits under the `'`, not inside its operand */
    ray_t* r = !txt || RAY_IS_ERR(txt) ? (txt ? txt : q_err(QE_STYPE))
             : bt && (bt->type != RAY_LIST || q_count(bt)) ? q_dbg_resignal(txt, bt)
                                       : q_err_signal(QE_SIGNAL, txt);
    if (txt && !RAY_IS_ERR(txt)) ray_release(txt);
    ray_release(v);
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
 * element, or one whose VALUE is the hole, is a projection HOLE: the literal
 * is a projection of the list constructor ("omission of values results in projection",
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
        argv[i] = e[i] ? q_eval_apply_concrete(q_eval(e[i])) : RAY_NULL_OBJ;
        if (Q_IS_HOLE(argv[i])) { argv[i] = NULL; holes++; continue; }
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
    if (q_eval_interrupt_take()) {
        if (node->type == RAY_LIST) q_eval_node = node;   /* the caret: the node the stop fell before */
        ray_t* s = q_dbg_stop();
        if (s) return s;
    }
    if (++g_depth > g_depth_cap) { g_depth--; return q_err(QE_LIMIT); }

    ray_t* ret;
    if (node->type == -RAY_SYM) {
        ret = name_value(node);
        if (RAY_IS_ERR(ret)) q_eval_node = node;
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
    /* a longer sym vector is the tree (`f;`x…) kdb's `parse "f x"` prints — only the ENLISTED symbol list is a
     * constant (parsetrees.md:100) — so a qSQL constraint's symbol head is a name (TorQ trackservers.q:350) */
    if (node->type == RAY_SYM && q_count(node) > 1) {
        ray_t* l = q_list_uncollapse(node);
        ret = RAY_IS_ERR(l) ? l : q_eval(l);
        if (!RAY_IS_ERR(l)) ray_release(l);
        goto out;
    }
    if (node->type != RAY_LIST || q_count(node) == 0) {
        ray_retain(node);
        ret = node;
        goto out;
    }

    {
        int64_t n = q_count(node);
        ray_t** e = (ray_t**)ray_data(node);
        ray_t* h = e[0];
        q_eval_node = node;

        /* literal-ctor interception seam (header note).  One item is `enlist x` (a paren of one is grouping), which
         * applies as any monad does: whether a hole VALUE there projects is unwitnessed (parse-tree programme) */
        if (h == q_registry_list_value() && n != 2) { ret = list_lit(e + 1, n - 1); goto out; }

        /* the `;` statement sequence — a CHAR head, like `:` return and `'`
         * signal below (q_parse.h: q_parse_is_seq_head is the one test) */
        if (q_parse_is_seq_head(h)) { ret = seq_eval(e + 1, n - 1); goto out; }

        /* an assignment is its head VALUE, however it was written (`-:[a;1]` assigns as `a-:1` does, ref/assign.md:103
         * `op:[x;y]`); over anything that is no target the head applies as the value it is */
        if (n == 3) {
            ray_t* opv = NULL;
            const q_op_t* row = NULL;
            q_assign_head_t asg = q_parse_assign_head(h, &opv, &row);
            if (asg != Q_HEAD_NONE && q_parse_assign_target(e[1]) != Q_TARGET_NONE) {
                ret = asg == Q_HEAD_THROUGH ? modassign_eval(opv, row, e[1], e[2])
                                            : assign_eval(e[1], e[2], asg == Q_HEAD_GLOBAL);
                goto out;
            }
        }
        if (nameref(h)) {
            const eval_syms_t* S = syms();
            if (h->i64 == S->kif)    { ret = if_eval(e + 1, n - 1, 0); goto out; }
            if (h->i64 == S->kwhile) { ret = if_eval(e + 1, n - 1, 1); goto out; }
            if (h->i64 == S->kdo)    { ret = do_eval(e + 1, n - 1); goto out; }
        }

        /* trees are DATA: the char-atom heads match by CONTENT, so an
         * eval'd `("'";`e)` signals exactly like parsed source */
        if (h && h->type == -RAY_CHARV && n == 2 &&
            (h->u8 == '\'' || h->u8 == ':')) {
            ret = h->u8 == '\'' ? signal_eval(node, e[1]) : return_eval(e[1]);
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
            fv = name_value(h);
        } else if (h->type == RAY_LIST || sym_const(h)) {
            fv = q_eval(h);
        } else {
            ray_retain(h);                          /* embedded value / noun */
            fv = h;
        }
        q_eval_node = node;                         /* the arguments' and a list head's applications moved it */
        if (RAY_IS_ERR(fv)) { release_args(argv, argc); ret = fv; goto out; }
        /* ONE recovery for every spelling of the head: a verb reached through a
         * name, a variable or an expression keeps its family and its enum arm */
        if (!row && (fv->type == RAY_UNARY || fv->type == RAY_BINARY ||
                     fv->type == RAY_VARY)) {
            q_valence_t val = (argc == 1) ? Q_MONADIC
                            : (argc == 2) ? Q_DYADIC : 0;
            if (val) row = q_registry_row_of(fv, val);
        }
        int64_t nap = (node->attrs & Q_ATTR_POSTFIX) && !argv[argc - 1] ? argc - 1 : argc;
        /* the train tail `g::` of a function g is the composition '[::;g] (owner 2026-11-07, Daxwzj); data passes */
        if (fv == RAY_NULL_OBJ && (node->attrs & Q_ATTR_POSTFIX) && argc == 1 && q_eval_apply_is_fn(argv[0])) {
            ray_t* parts[2] = { RAY_NULL_OBJ, argv[0] };
            ret = q_eval_apply_value(q_registry_compose_value(), parts, 2);
        } else {
            ret = q_eval_apply(fv, row, argv, nap);
        }
        release_args(argv, argc);
        ray_release(fv);
    }

out:
    g_depth--;
    return ret;
}
