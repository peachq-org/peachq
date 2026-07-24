/* q_registry_internal.h — INTERNAL shared surface of the q_registry.c split
 * (2026-07-14).  These symbols were file-local statics in the monolith; they
 * are exported ONLY so the sibling .c files of the split can keep calling
 * them.  NOT public API — do not include outside src/qlang.
 *
 * Two blocks:
 *   1. REGISTRY ENTRYPOINTS — wrapper bodies living in q_wrap_*.c, referenced
 *      ONLY as manifest build recipes: the Q_OPS[] rows (q_ops.c) carry these
 *      function pointers in their QR_FN* recipes, and q_registry.c's generic
 *      build_wrapper binds them into immutable fn-values (or init/teardown-
 *      managed singletons).  Do not re-staticize (that would move the bodies
 *      back into the monolith); new wrapper => declare it here, define it in
 *      the domain file, point a QR_FN* manifest row at it.
 *   2. SHARED HELPERS — called from sibling files beyond the registry
 *      builders (the defining file typically calls them too); each line
 *      names the consumers.  This block IS the split's lateral-dependency
 *      map: a new cross-file caller means updating the "used by" note (and
 *      asking whether the helper is homed in the right domain file). */
#ifndef Q_REGISTRY_INTERNAL_H
#define Q_REGISTRY_INTERNAL_H
#include "qlang/q_registry.h"
#include "qlang/q_ops.h"
#include "qlang/q_type.h"   /* the type + null axes home — re-exported to ops/ */

/* ===== 1. REGISTRY ENTRYPOINTS — q_registry.c builders only ============= */

/* ---- defined in ops/q_agg.c ---- */
ray_t* q_sums_wrap(ray_t* x);
ray_t* q_prds_wrap(ray_t* x);
ray_t* q_maxs_wrap(ray_t* x);
ray_t* q_mins_wrap(ray_t* x);
ray_t* q_avgs_wrap(ray_t* x);
ray_t* q_ratios_wrap(ray_t* x);
ray_t* q_prd_wrap(ray_t* x);
ray_t* q_wavg_wrap(ray_t* x, ray_t* y);
ray_t* q_msum_wrap(ray_t* n, ray_t* x);
ray_t* q_mmax_wrap(ray_t* n, ray_t* x);
ray_t* q_mmin_wrap(ray_t* n, ray_t* x);
ray_t* q_mcount_wrap(ray_t* n, ray_t* x);
ray_t* q_mdev_wrap(ray_t* n, ray_t* x);
ray_t* q_ema_wrap(ray_t* a, ray_t* x);
ray_t* q_mmu_wrap(ray_t* x, ray_t* y);                        /* matrix multiply / dot product — used by: math, dollar */
enum { QMMU_BAD = -1, QMMU_RAGGED = -2 };                     /* RAGGED is mmu-shaped: mmu owns its 'length */
int q_mmu_class(ray_t* v, int64_t* first);                    /* 0 vec, 1 matrix, else QMMU_*; *first = count(-first) — used by: dollar */
ray_t* q_sum_wrap(ray_t* x);

/* ---- defined in q_builtins.c ---- */
/* 'nyi recipe stubs (rule 3: rows keep their spellings; the bodies died in
 * the eval-rebuild cutover).  q_value_nyi_fn: `value`/`get`/`-6!`.  q_hof_nyi_wrap:
 * the keyword-HOF rows (each/peach/over/scan/prior) — the fresh apply module
 * routes their n==2 applications to native adverb arms BEFORE the recipe
 * value could run, so these fire only on off-shape applications. */
ray_t* q_value_nyi_fn(ray_t* x);
ray_t* q_hof_nyi_wrap(ray_t* f, ray_t* x);

/* ---- defined in ops/q_io.c ---- */
ray_t* q_filetext_wrap(ray_t* x, ray_t* y);
ray_t* q_setenv_wrap(ray_t* x, ray_t* y);
ray_t* q_getenv_wrap(ray_t* x);

/* ---- defined in ops/q_str.c ---- */
ray_t* q_like_wrap(ray_t* x, ray_t* pattern);
ray_t* q_ss_wrap(ray_t* s, ray_t* p);

/* ---- defined in ops/q_join.c ---- */
ray_t* q_lj_wrap(ray_t* x, ray_t* y);
ray_t* q_ljf_wrap(ray_t* x, ray_t* y);
ray_t* q_ij_wrap(ray_t* x, ray_t* y);
ray_t* q_ijf_wrap(ray_t* x, ray_t* y);
ray_t* q_pj_wrap(ray_t* x, ray_t* y);
ray_t* q_uj_wrap(ray_t* x, ray_t* y);
ray_t* q_ujf_wrap(ray_t* x, ray_t* y);
ray_t* q_asof_wrap(ray_t* t, ray_t* d);

/* ---- defined in ops/q_list.c ---- */
ray_t* q_drop_wrap(ray_t* n, ray_t* list);                    /* also shared: index (splice) */
ray_t* q_cut_wrap(ray_t* n, ray_t* x);
ray_t* q_xprev_wrap(ray_t* nx, ray_t* x);
ray_t* q_fills_wrap(ray_t* x);
ray_t* q_in_wrap(ray_t* x, ray_t* y);
ray_t* q_iasc_wrap(ray_t* x);
ray_t* q_idesc_wrap(ray_t* x);
ray_t* q_xbar_wrap(ray_t* bucket, ray_t* col);
ray_t* q_raze_wrap(ray_t* x);
ray_t* q_where_wrap(ray_t* x);

/* ---- defined in ops/q_rand.c ---- */
ray_t* q_roll_wrap(ray_t* x, ray_t* y);

/* ---- defined in ops/q_math.c ---- */
ray_t* q_sin_wrap(ray_t* x);
ray_t* q_cos_wrap(ray_t* x);
ray_t* q_tan_wrap(ray_t* x);
ray_t* q_asin_wrap(ray_t* x);
ray_t* q_acos_wrap(ray_t* x);
ray_t* q_atan_wrap(ray_t* x);
ray_t* q_floor_wrap(ray_t* x);
ray_t* q_signum_wrap(ray_t* x);
ray_t* q_ceiling_wrap(ray_t* x);
ray_t* q_xexp_wrap(ray_t* x, ray_t* y);
ray_t* q_xlog_wrap(ray_t* x, ray_t* y);
ray_t* q_eq_wrap(ray_t* a, ray_t* b);
ray_t* q_ne_wrap(ray_t* a, ray_t* b);
ray_t* q_neg_wrap(ray_t* x);
ray_t* q_within_wrap(ray_t* x, ray_t* y);

/* ---- defined in ops/q_table.c ---- */
ray_t* q_keys_wrap(ray_t* x);
ray_t* q_xkey_wrap(ray_t* x, ray_t* y);
ray_t* q_xgroup_wrap(ray_t* x, ray_t* y);
ray_t* q_ungroup_wrap(ray_t* x);
ray_t* q_insert_wrap(ray_t* x, ray_t* y);
ray_t* q_upsert_wrap(ray_t* x, ray_t* y);
ray_t* q_join_wrap(ray_t* x, ray_t* y);                       /* also shared: index (splice, dict insert) */
ray_t* q_except_wrap(ray_t* x, ray_t* y);
ray_t* q_key_wrap(ray_t* x);
ray_t* q_distinct_wrap(ray_t* x);
ray_t* q_union_wrap(ray_t* x, ray_t* y);
ray_t* q_inter_wrap(ray_t* x, ray_t* y);
ray_t* q_cross_wrap(ray_t* x, ray_t* y);

/* ===== 2. SHARED HELPERS — the lateral-dependency map =================== */

/* ---- defined in q_registry.c ---- */
int64_t q_name_dedup(int64_t sym_id, const int64_t* previous, int64_t n_previous, int check_reserved);/* used by: builtins, table */
ray_t* q_registry_lookup_name(const char* s, size_t n, q_valence_t valence);/* used by: builtins, fmt, parse, agg */
bool q_registry_provenance(const ray_t* value, q_provenance_t* out);/* used by: fmt, join */

/* ---- defined in ops/q_agg.c ---- */
double q_velem_f(ray_t* x, int64_t i, int* isnull);           /* used by: list */
int q_vec_is_float(ray_t* x);                                 /* used by: list */
int q_vec_is_num(ray_t* x);                                   /* used by: list */

/* ---- defined in ops/q_io.c ---- */
ray_t* q_hopen_wrap(ray_t* x);                                /* used by: apply, registry */
ray_t* q_hclose_wrap(ray_t* x);                               /* used by: apply, registry */
ray_t* q_hsym_wrap(ray_t* x);                                 /* used by: bang, registry */
ray_t* q_read0_wrap(ray_t* x);                                /* used by: builtins, registry */
ray_t* q_read1_wrap(ray_t* x);                                /* used by: builtins, registry */
ray_t* q_hdel_wrap(ray_t* x);                                 /* used by: builtins, registry */

/* ---- defined in ops/q_str.c ---- */
ray_t* q_ssr_wrap(ray_t** args, int64_t n);                   /* used by: builtins, registry */
ray_t* q_str_split_lines(const char* y, size_t yl);           /* used by: io, vs_sv */

/* ---- defined in ops/q_vs_sv.c ---- */
ray_t* q_vs_wrap(ray_t* x, ray_t* y);
ray_t* q_sv_wrap(ray_t* x, ray_t* y);

/* ---- defined in ops/q_join.c ---- */
int qj_same_schema(ray_t* a, ray_t* b);                       /* used by: table */
ray_t* qj_table_gather_idx(ray_t* t, const int64_t* idx, int64_t n);/* used by: table, list */
ray_t* qj_ktbl_merge(ray_t* x, ray_t* y, int mode);           /* used by: list, table */

/* ---- defined in ops/q_list.c ---- */
ray_t* q_list_collapse(ray_t* l);                             /* also declared in q_registry.h (env-safe public reach) — used by: eval, builtins, fmt, json, wire, agg, dollar, io, join, list, math, table, index */
ray_t* q_attr_wrap(ray_t* x);                                 /* used by: bang, registry */
ray_t* q_take_wrap(ray_t* n, ray_t* list);                    /* used by: ops, registry, index */
ray_t* q_typed_empty_like(ray_t* collapsed, ray_t* proto);    /* single-file since the corridor pass — staticize candidate */
ray_t* q_til_wrap(ray_t* x);                                  /* used by: registry, table */
ray_t* q_fill_wrap(ray_t* x, ray_t* y);                       /* used by: registry, agg, join */
ray_t* q_env_call2(const char* nm, ray_t* a, ray_t* b);       /* used by: join, table */
ray_t* q_list_find(ray_t* x, ray_t* y);                       /* used by: rand */

/* ---- defined in ops/q_math.c ---- */
ray_t* q_min2_wrap(ray_t* a, ray_t* b);                       /* used by: ops, registry */
int q_match_rec(ray_t* a, ray_t* b);                          /* used by: table */
ray_t* q_match_wrap(ray_t* a, ray_t* b);                      /* used by: registry, table */

/* ---- defined in ops/q_rand.c ---- */
void q_rand_seed(int64_t n);                                  /* used by: sys */

/* ---- defined in ops/q_table.c ---- */
ray_t* q_flip_wrap(ray_t* x);                                 /* used by: registry, list, math */
ray_t* q_table_flatten(ray_t* y);                             /* used by: bang, join */
ray_t* q_table_dict_vals(ray_t* d, int* owned);               /* used by: list */
ray_t* qj_item(ray_t* x, int64_t i);                          /* used by: join */
ray_t* qj_gen_item(ray_t* x, int64_t i);                      /* used by: join */
ray_t* q_setg_wrap(ray_t* x, ray_t* y);                       /* used by: dotz, lower, registry */
ray_t* q_exit_wrap(ray_t* x);                                 /* used by: ops */

/* Verb bodies must not touch the environment — name resolution/binding is
 * the evaluator's (and bootstrap's) domain.  Legitimate owners and the
 * grandfathered ops/ offenders define Q_OPS_ENV_GRANDFATHER (dated one-line
 * reason at the define) before including this header.  The env/eval headers
 * are pre-included so their declarations precede the poison. */
#ifndef Q_OPS_ENV_GRANDFATHER
#include "lang/env.h"
#include "lang/eval.h"
#pragma GCC poison ray_env_get ray_env_set ray_env_bind ray_env_bind_flat ray_env_resolve
#endif

#endif
