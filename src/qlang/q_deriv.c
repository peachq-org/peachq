/* q derived-verb / projection carriers — see q_deriv.h.
 *
 * Inert boxed carriers (STAGE 2a): built + inspectable, never evaluated.  A
 * carrier is a RAY_LIST laid out as:
 *   PROJ   : [ `.q.proj , base, hole_mask(i64), eff_valence(i64), arg0, ... ]
 *   MONAD  : [ `.q.monad, base, eff_valence(i64) ]
 * element[0] is a QUOTED marker sym so the box self-identifies.  (Adverbs are
 * NOT carriers — they map to rayfall HOFs in the op manifest; see q_deriv.h.) */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_deriv.h"
#include <string.h>

#define Q_DERIV_QUOTED 0x20   /* ATTR_QUOTED (src/lang/eval.h) — literal sym */

/* Cached marker sym-ids (lazily interned; -1 = not yet). */
static int64_t g_sid_proj   = -1;
static int64_t g_sid_monad  = -1;
static int64_t g_sid_hole   = -1;  /* unbound-arg `;` sentinel in a projection */
static int64_t g_sid_lambda = -1;
static int64_t g_sid_compose = -1;

void q_deriv_reset_markers(void) {
    g_sid_proj = g_sid_monad = g_sid_hole = g_sid_lambda = -1;
}

static void ensure_markers(void) {
    if (g_sid_proj < 0) {
        g_sid_proj   = ray_sym_intern(".q.proj",   7);
        g_sid_monad  = ray_sym_intern(".q.monad",  8);
        g_sid_hole   = ray_sym_intern(".q.hole",   7);
        g_sid_lambda = ray_sym_intern(".q.lambda", 9);
        g_sid_compose = ray_sym_intern(".q.compose", 10);
    }
}

static ray_t* marker_atom(int64_t sid) {
    ray_t* m = ray_sym(sid);
    if (m && !RAY_IS_ERR(m)) m->attrs |= Q_DERIV_QUOTED;
    return m;
}

/* Append one owned child into `l`, releasing it (append retains).  Returns the
 * (possibly-reallocated) list. */
static ray_t* push_owned(ray_t* l, ray_t* child) {
    l = ray_list_append(l, child);
    ray_release(child);
    return l;
}

/* Append `base` (borrowed by caller) — append retains it, so no extra ref. */
static ray_t* push_borrowed(ray_t* l, ray_t* v) {
    return ray_list_append(l, v);
}

ray_t* q_proj_new(ray_t* base, ray_t** args, int64_t argc, uint64_t hole_mask,
                  int eff_valence) {
    if (!base) return ray_error("type", "q_proj_new: nil base");
    ensure_markers();
    ray_t* l = ray_list_new(4 + (argc > 0 ? argc : 0));
    l = push_owned(l, marker_atom(g_sid_proj));
    l = push_borrowed(l, base);
    l = push_owned(l, ray_i64((int64_t)hole_mask));
    l = push_owned(l, ray_i64((int64_t)eff_valence));
    for (int64_t i = 0; i < argc; i++) {
        if (args[i]) l = push_borrowed(l, args[i]);
        else {
            /* an unbound-arg hole — a dedicated quoted `.q.hole` sentinel so the
             * slot count matches argc; the authoritative hole record is
             * `hole_mask` (this filler is never inspected in 2a). */
            ray_t* hole = marker_atom(g_sid_hole);
            l = push_owned(l, hole);
        }
    }
    return l;
}

ray_t* q_lambda_carrier_new(ray_t* base, int rank, ray_t* src) {
    if (!base) return ray_error("type", "q_lambda_carrier_new: nil base");
    if (!src)  return ray_error("type", "q_lambda_carrier_new: nil src");
    ensure_markers();
    ray_t* l = ray_list_new(4);
    l = push_owned(l, marker_atom(g_sid_lambda));
    l = push_borrowed(l, base);
    l = push_owned(l, ray_i64((int64_t)rank));   /* valence at idx 2 = MONAD layout */
    l = push_borrowed(l, src);
    return l;
}

ray_t* q_compose_new(ray_t** fns, int64_t nf) {
    if (nf < 1) return ray_error("rank", "q_compose_new: needs >=1 function");
    ensure_markers();
    ray_t* l = ray_list_new(nf + 1);
    l = push_owned(l, marker_atom(g_sid_compose));
    for (int64_t i = 0; i < nf; i++) {
        if (!fns[i]) { ray_release(l); return ray_error("type", "compose: nil function"); }
        l = push_borrowed(l, fns[i]);   /* append retains */
    }
    return l;
}

ray_t* q_monadic_mark(ray_t* base) {
    if (!base) return ray_error("type", "q_monadic_mark: nil base");
    ensure_markers();
    ray_t* l = ray_list_new(3);
    l = push_owned(l, marker_atom(g_sid_monad));
    l = push_borrowed(l, base);
    l = push_owned(l, ray_i64(1));   /* monadic-marked: valence 1 */
    return l;
}

/* ---- inspectors ---- */

static int64_t head_sid(const ray_t* v) {
    if (!v || v->type != RAY_LIST || ray_len((ray_t*)v) < 2) return -1;
    ray_t* h = ((ray_t**)ray_data((ray_t*)v))[0];
    if (!h || h->type != -RAY_SYM) return -1;
    return h->i64;
}

/* Classify — AND validate the full fixed-field arity so every accessor below
 * can index its slots without bounds-checking (a forged/truncated list that
 * happens to carry a marker in slot 0 but lacks the fixed fields classifies as
 * NONE, never as a carrier whose fields would read past the body). */
q_deriv_kind q_deriv_kind_of(const ray_t* v) {
    ensure_markers();
    int64_t sid = head_sid(v);
    if (sid < 0) return Q_DERIV_NONE;
    int64_t n = ray_len((ray_t*)v);
    if (sid == g_sid_proj   && n >= 4) return Q_DERIV_PROJ;   /* marker,base,mask,val */
    if (sid == g_sid_monad  && n >= 3) return Q_DERIV_MONAD;  /* marker,base,val       */
    if (sid == g_sid_lambda && n >= 4) return Q_DERIV_LAMBDA; /* marker,base,val,src   */
    if (sid == g_sid_compose && n >= 2) return Q_DERIV_COMPOSE; /* marker,fn0,fn1,…    */
    return Q_DERIV_NONE;
}

int64_t q_compose_count(const ray_t* v) {
    if (q_deriv_kind_of(v) != Q_DERIV_COMPOSE) return 0;
    return ray_len((ray_t*)v) - 1;
}

ray_t* q_compose_fn_at(const ray_t* v, int64_t i) {
    if (q_deriv_kind_of(v) != Q_DERIV_COMPOSE) return NULL;
    int64_t nf = ray_len((ray_t*)v) - 1;
    if (i < 0 || i >= nf) return NULL;
    return ((ray_t**)ray_data((ray_t*)v))[1 + i];   /* borrowed */
}

ray_t* q_deriv_base(const ray_t* v) {
    if (q_deriv_kind_of(v) == Q_DERIV_NONE) return NULL;
    return ((ray_t**)ray_data((ray_t*)v))[1];   /* borrowed */
}

uint64_t q_deriv_hole_mask(const ray_t* v) {
    if (q_deriv_kind_of(v) != Q_DERIV_PROJ) return 0;
    ray_t* m = ((ray_t**)ray_data((ray_t*)v))[2];
    return (uint64_t)(m && m->type == -RAY_I64 ? m->i64 : 0);
}

ray_t* q_lambda_src(const ray_t* v) {
    if (q_deriv_kind_of(v) != Q_DERIV_LAMBDA) return NULL;
    return ((ray_t**)ray_data((ray_t*)v))[3];   /* borrowed */
}

int q_deriv_valence(const ray_t* v) {
    q_deriv_kind k = q_deriv_kind_of(v);
    if (k == Q_DERIV_NONE) return 0;
    int idx = (k == Q_DERIV_PROJ) ? 3 : 2;   /* PROJ: [.,.,mask,val]; MONAD: [.,.,val] */
    ray_t* ev = ((ray_t**)ray_data((ray_t*)v))[idx];
    return (int)(ev && ev->type == -RAY_I64 ? ev->i64 : 0);
}
