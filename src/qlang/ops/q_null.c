/* ops/q_null.c — the `null` verb body.  Split from base/q_type.c (2026-08-02)
 * so base/ holds only the type-axis PREDICATES other code asks: a verb wrapper
 * needs q_list_collapse, and base/ must not reach the registry. */
#include "qlang/q_registry_internal.h"
#include "qlang/base/q_type.h"  /* q_type_is_null_sym — the null-symbol divergence */
#include "lang/internal.h"      /* is_collection, atomic_map_unary, ray_nil_fn */

/* Base `ray_nil_fn` reads sym id 0 as the EMPTY symbol (a value); q reads the
 * null symbol ` AS null (`null \`` -> 1b).  The divergence stays here so base
 * rayfall's own sym-0-as-empty paths are untouched. */
static ray_t* nil_fn(ray_t* x) {
    if (q_type_is_null_sym(x)) return ray_bool(true);
    return ray_nil_fn(x);
}

/* Registered RAY_FN_NONE — NOT atomic — so it receives the whole argument and
 * owns the collapse: a heterogeneous list yields a homogeneous bool-atom run
 * that folds to a bool vector (`null (1;`a;2.5;"x")` -> 0000b), while a nested
 * list yields bool VECTORS that q_list_collapse leaves intact. */
ray_t* q_null_wrap(ray_t* x) {
    ray_t* r = is_collection(x) ? atomic_map_unary(nil_fn, x) : nil_fn(x);
    if (!r || RAY_IS_ERR(r) || r->type != RAY_LIST) return r;
    ray_t* c = q_list_collapse(r);   /* owned: retains-or-builds */
    ray_release(r);
    return c;
}
