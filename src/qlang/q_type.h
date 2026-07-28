/* q_type — THE type-axis home: q-side lane accessors, lane predicates, and
 * (future) type-signature tables. The base accessors (as_i64/as_f64 in
 * lang/internal.h) stay in the quarry; q's temporal-lane extensions live HERE,
 * so src/lang/internal.h remains byte-identical to its fork point. This is the
 * edit-boundary rule made physical: base is touched only for type enrollment,
 * every other extension wraps in a q_ home. */
#ifndef QLANG_Q_TYPE_H
#define QLANG_Q_TYPE_H

#include <rayforce.h>
#include <stdint.h>

/* Integer lane read, temporal-complete: the int-backed temporals read
 * explicitly here (TEMPORAL32 from the i32 payload, TEMPORAL64 from i64); every
 * other atom delegates to the base as_i64. DATETIME is f64-backed
 * (rayforce.h:129) — read it via as_f64, never here. Nulls are the caller's to
 * test (RAY_ATOM_IS_NULL) before the read. */
int64_t q_type_as_i64(ray_t* x);

/* Admission for a verb reading any numeric OR temporal lane via as_f64
 * (signum): exactly the atoms as_f64 has a lane for (is_temporal omits
 * DATETIME). cmp.c open-codes this same union — a future drain consumer. */
int q_type_is_numeric_or_temporal(ray_t* x);

/* Bool atom — the lane predicate, so bool's b->i promotion is a named lane
 * test, not an inline type probe in a verb wrapper. */
int q_type_is_bool(ray_t* x);

/* Numeric-family lane predicates over a raw TAG (vector or atom): "is this the
 * float family" / "is this any numeric family". Tag-shaped rather than value-
 * shaped because their callers compare two operands' families (`in`'s mixed-
 * family rule, `^` fill's result-type lattice) before touching a payload. */
int q_type_is_float_tag(int8_t t);
int q_type_is_num_tag(int8_t t);

/* The kdb type number of a value (the `type` verb's knowledge): function
 * values map to 100h/102h/104h/106+adv, everything else IS its internal tag.
 * Callers must handle NULL x themselves (base-verb fallback). */
int8_t q_type_of(ray_t* x);

/* Vector tag -> kdb type name ("long"/"symbol"/...) or NULL (list/physical
 * STR); total over the value band. */
const char* q_type_qname(int8_t t);

/* tag -> rayfall `as` type-sym spelling ("I64"/"DATE"/...) or NULL when the
 * tag has no cast-delegation spelling. */
const char* q_type_rayname(int8_t tag);

/* THE single tag -> type-char map (ref/dotq.md `.Q.ty`, `meta`'s `t` column).
 * Absolute or negative tag; 0 for tags with no char.  Lowercase = the element
 * char; `.Q.ty` uppercases it for a uniform list of vectors. */
char q_type_char(int8_t tag);

/* THE q result-type law for a mixed-type pair — what TYPE the result carries
 * (`d&j` -> d), per the "Domain and range" matrix in ref/lesser.md.  Takes
 * VECTOR-form tags; 0 = the pair has no result type (the guid and symbol rows),
 * which every caller must reject.  O(1); q_type_init must have run. */
int8_t q_type_common(int8_t a, int8_t b);

/* Bakes the published matrix into its tag-indexed form.  Idempotent, depends on
 * nothing but compile-time constants; called from q_registry_init so it is done
 * before any verb can run. */
void q_type_init(void);

/* Int-index width in bytes for tag t (8/4/2/1), 0 = not an int index. */
int q_type_int_index_width(int8_t t);

/* The empty typed vector of tag (sym vectors need their width ctor). */
ray_t* q_type_empty(int8_t tag);

/* The ELEMENT tag of a value — an atom's own tag, a vector's negated one;
 * 0 for every other shape, which has no single element type to name. */
int8_t q_type_elem_tag(ray_t* x);

/* Strict int/float lane admission (cast-or-fail; typed nulls pass as
 * sentinels): 1 + *out on success, 0 on refusal. */
int q_type_strict_i64(ray_t* x, int64_t* out);
int q_type_strict_f64(ray_t* x, double* out);

/* Throwing gate for terminal sites: NULL on success, else the bare 'type. */
ray_t* q_type_i64_or_err(ray_t* x, int64_t* out, const char* what);

/* Int atom/vector facts (I64/I32/I16) and their reads. */
int q_type_is_int_atom(ray_t* x);
int q_type_is_int_vec(ray_t* x);
int64_t q_type_ivec_get(ray_t* v, int64_t i);
int64_t q_type_iatom_val(ray_t* x);

/* True iff x is a table — the row-collection shape the row-wise verbs ask for. */
int q_type_is_table(ray_t* x);

/* True iff y is a keyed table: a RAY_DICT whose keys AND values are tables. */
int q_type_is_keyed(ray_t* y);

/* A plain key!value dict (a dict that is NOT a keyed table). */
int q_type_is_plain_dict(ray_t* x);

/* any dictionary, keyed table included (entries-axis laws) */
int q_type_is_dict(ray_t* x);

/* column the dense group core (agg_group_keys) can read as an int64/str key */
int q_type_is_dense_group_col(ray_t* c);

/* The physical STR atom lane (string-C3): rank-wise a char LIST, not a scalar. */
int q_type_is_str_atom(ray_t* x);

/* Are v's ITEMS themselves collections — a list of lists rather than a flat run
 * of atoms?  THE rank axis: the search family reads it to pick a haystack's
 * search rank (find.md — a rank-n haystack looks for rank n-1 objects) and to
 * tell a record from a run of rows.  A STR atom counts as a collection: kdb
 * strings are char LISTS. */
int q_type_is_nested(ray_t* v);

/* THE iteration domain — what pairs one-for-one with a value's ITEMS.  A table
 * joins the collections here ("the items of a table are its rows", ref/count.md);
 * the INDEX law, reading a table as an axis pair, keeps its own is_coll. */
int q_type_is_iter(ray_t* v);

/* q treats the null symbol ` AS null (base sym-0 is the empty symbol). */
int q_type_is_null_sym(ray_t* x);

/* q `null x` verb body — elementwise null test, owns the collapse. Registered
 * RAY_FN_NONE; keeps its _wrap name across the home move. */
ray_t* q_null_wrap(ray_t* x);

#endif /* QLANG_Q_TYPE_H */
