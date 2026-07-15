/* q_registry_internal.h — INTERNAL shared surface of the q_registry.c split
 * (2026-07-14).  These symbols were file-local statics in the monolith; they
 * are exported ONLY so the sibling .c files of the split can keep calling
 * them.  NOT public API — do not include outside src/qlang.
 *
 * Three blocks:
 *   1. q_funsql.c retirement seam — the functional-qSQL executor's WHOLE
 *      exported surface.  eval-unification stage 3 retires q_funsql.c by
 *      deleting the file once these five lose their last caller; keep the
 *      seam from growing.
 *   2. REGISTRY ENTRYPOINTS — wrapper bodies living in q_wrap_*.c, referenced
 *      ONLY as manifest build recipes: the Q_OPS[] rows (q_ops.c) carry these
 *      function pointers in their QR_FN* recipes, and q_registry.c's generic
 *      build_wrapper binds them into immutable fn-values (or init/teardown-
 *      managed singletons).  Do not re-staticize (that would move the bodies
 *      back into the monolith); new wrapper => declare it here, define it in
 *      the domain file, point a QR_FN* manifest row at it.
 *   3. SHARED HELPERS — called from sibling files beyond the registry
 *      builders (the defining file typically calls them too); each line
 *      names the consumers.  This block IS the split's lateral-dependency
 *      map: a new cross-file caller means updating the "used by" note (and
 *      asking whether the helper is homed in the right domain file). */
#ifndef Q_REGISTRY_INTERNAL_H
#define Q_REGISTRY_INTERNAL_H
#include "qlang/q_registry.h"
#include "qlang/q_ops.h"

/* ===== 1. q_funsql.c retirement seam (see file header there) ============ */
ray_t* q_select_exec(ray_t** args, int64_t n);                /* used by: lower, registry, castcal */
ray_t* q_funsql_select(ray_t** args, int64_t n);              /* used by: lower, registry */
ray_t* q_funsql_bang(ray_t** args, int64_t n);                /* used by: lower, registry */
ray_t* q_delete_exec(ray_t** args, int64_t n);                /* used by: registry */
ray_t* q_exec_exec(ray_t** args, int64_t n);                  /* used by: registry */

/* ===== 2. REGISTRY ENTRYPOINTS — q_registry.c builders only ============= */

/* ---- defined in q_wrap_agg.c ---- */
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
ray_t* q_neg_wrap(ray_t* x);
ray_t* q_raze_wrap(ray_t* x);
ray_t* q_within_wrap(ray_t* x, ray_t* y);
ray_t* q_sum_wrap(ray_t* x);

/* ---- defined in q_wrap_applyiter.c ---- */
ray_t* q_at_wrap(ray_t** args, int64_t n);
ray_t* q_dot_wrap(ray_t** args, int64_t n);
ray_t* q_each_wrap(ray_t* f, ray_t* x);
ray_t* q_eachboth_wrap(ray_t** args, int64_t n);
ray_t* q_prior_wrap(ray_t** args, int64_t n);
ray_t* q_scan_wrap(ray_t** args, int64_t n);
ray_t* q_over_kw(ray_t* f, ray_t* x);
ray_t* q_scan_kw(ray_t* f, ray_t* x);
ray_t* q_prior_kw(ray_t* f, ray_t* x);
ray_t* q_mkderiv2(ray_t* hof, ray_t* f);
ray_t* q_mkopproj(ray_t** args, int64_t k);
extern ray_t* g_scan_value;
extern ray_t* g_over_value;
extern ray_t* g_eachboth_value;
extern ray_t* g_prior_value;
extern ray_t* g_mkderiv2_value;
extern ray_t* g_mkopproj_value;
extern ray_t* g_list_value;
extern ray_t* g_table_value;
extern ray_t* g_keyed_table_value;
extern ray_t* g_select_value;
extern ray_t* g_delete_value;
extern ray_t* g_exec_value;
extern ray_t* g_compose_value;
ray_t* q_compose_fn(ray_t** args, int64_t n);
extern ray_t* g_lambda_value;
ray_t* q_fn_make(ray_t** args, int64_t n);
extern ray_t* g_ret_value;
extern ray_t* g_sig_value;
extern _Thread_local ray_t* g_qret_payload;
extern _Thread_local ray_t* g_qsig_payload;
ray_t* q_ret_fn(ray_t* x);
ray_t* q_sig_fn(ray_t* x);
extern ray_t* g_seq_value;
extern ray_t* g_if_value;
extern ray_t* g_do_value;
extern ray_t* g_while_value;
ray_t* q_seq_fn(ray_t** args, int64_t n);
ray_t* q_if_fn(ray_t** args, int64_t n);
ray_t* q_do_fn(ray_t** args, int64_t n);
ray_t* q_while_fn(ray_t** args, int64_t n);

/* ---- defined in q_wrap_castcal.c ---- */
ray_t* q_cast_wrap(ray_t* t, ray_t* x);

/* ---- defined in q_wrap_io.c ---- */
ray_t* q_filetext_wrap(ray_t* x, ray_t* y);
ray_t* q_like_wrap(ray_t* x, ray_t* pattern);
ray_t* q_ss_wrap(ray_t* s, ray_t* p);
ray_t* q_getenv_wrap(ray_t* x);
ray_t* q_setenv_wrap(ray_t* x, ray_t* y);

/* ---- defined in q_wrap_join.c ---- */
ray_t* q_lj_wrap(ray_t* x, ray_t* y);
ray_t* q_ljf_wrap(ray_t* x, ray_t* y);
ray_t* q_ij_wrap(ray_t* x, ray_t* y);
ray_t* q_ijf_wrap(ray_t* x, ray_t* y);
ray_t* q_pj_wrap(ray_t* x, ray_t* y);
ray_t* q_uj_wrap(ray_t* x, ray_t* y);
ray_t* q_ujf_wrap(ray_t* x, ray_t* y);
ray_t* q_asof_wrap(ray_t* t, ray_t* d);

/* ---- defined in q_wrap_list.c ---- */
ray_t* q_drop_wrap(ray_t* n, ray_t* list);
ray_t* q_cut_wrap(ray_t* n, ray_t* x);
ray_t* q_xprev_wrap(ray_t* nx, ray_t* x);
ray_t* q_fills_wrap(ray_t* x);
ray_t* q_in_wrap(ray_t* x, ray_t* y);
ray_t* q_asc_wrap(ray_t* x);
ray_t* q_desc_wrap(ray_t* x);
ray_t* q_iasc_wrap(ray_t* x);
ray_t* q_idesc_wrap(ray_t* x);
ray_t* q_xbar_wrap(ray_t* bucket, ray_t* col);
ray_t* q_roll_wrap(ray_t* x, ray_t* y);

/* ---- defined in q_wrap_math.c ---- */
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
ray_t* q_where_wrap(ray_t* x);
ray_t* q_reverse_wrap(ray_t* x);
ray_t* q_vs_wrap(ray_t* x, ray_t* y);
ray_t* q_sv_wrap(ray_t* x, ray_t* y);

/* ---- defined in q_wrap_table.c ---- */
ray_t* q_keys_wrap(ray_t* x);
ray_t* q_xkey_wrap(ray_t* x, ray_t* y);
ray_t* q_xcol_wrap(ray_t* x, ray_t* y);
ray_t* q_xcols_wrap(ray_t* x, ray_t* y);
ray_t* q_xasc_wrap(ray_t* x, ray_t* y);
ray_t* q_xdesc_wrap(ray_t* x, ray_t* y);
ray_t* q_xgroup_wrap(ray_t* x, ray_t* y);
ray_t* q_ungroup_wrap(ray_t* x);
ray_t* q_insert_wrap(ray_t* x, ray_t* y);
ray_t* q_upsert_wrap(ray_t* x, ray_t* y);
ray_t* q_join_wrap(ray_t* x, ray_t* y);
ray_t* q_except_wrap(ray_t* x, ray_t* y);
ray_t* q_bang_wrap(ray_t* x, ray_t* y);
ray_t* q_key_wrap(ray_t* x);
ray_t* q_distinct_wrap(ray_t* x);
ray_t* q_union_wrap(ray_t* x, ray_t* y);
ray_t* q_inter_wrap(ray_t* x, ray_t* y);
ray_t* q_cross_wrap(ray_t* x, ray_t* y);
ray_t* q_list_build(ray_t** args, int64_t n);
ray_t* q_table_build(ray_t** args, int64_t n);
ray_t* q_keyed_table_build(ray_t** args, int64_t n);

/* ===== 3. SHARED HELPERS — the lateral-dependency map =================== */

/* ---- defined in q_registry.c ---- */
int64_t q_name_dedup(int64_t sym_id, const int64_t* previous, int64_t n_previous, int check_reserved);/* used by: builtins, funsql, table */
ray_t* q_collapse_list(ray_t* l);                             /* used by: apply, builtins, fmt, funsql, json, wire, agg, applyiter, castcal, io, join, list, math, table */
ray_t* q_registry_lookup_name(const char* s, size_t n, q_valence_t valence);/* used by: builtins, fmt, lower, parse, runtime, agg, applyiter, castcal */
bool q_registry_provenance(const ray_t* value, q_provenance_t* out);/* used by: fmt, lower, applyiter, join */

/* ---- defined in q_wrap_agg.c ---- */
double q_velem_f(ray_t* x, int64_t i, int* isnull);           /* used by: list */
int q_vec_is_float(ray_t* x);                                 /* used by: list */
int q_vec_is_num(ray_t* x);                                   /* used by: list */
ray_t* q_null_wrap(ray_t* x);                                 /* used by: registry, math */

/* ---- defined in q_wrap_applyiter.c ---- */
int q_is_fn_value(ray_t* x);                                  /* used by: builtins, io */
int q_values_match(ray_t* a, ray_t* b);                       /* used by: list */
ray_t* q_call_n(ray_t* f, ray_t** a, int64_t k);              /* used by: castcal, io */
ray_t* q_over_wrap(ray_t** args, int64_t n);                  /* used by: lower, registry */
ray_t* q_registry_scan_value(void);                           /* used by: fmt, lower, castcal */
ray_t* q_registry_over_value(void);                           /* used by: fmt, lower, castcal */
ray_t* q_registry_prior_value(void);                          /* used by: fmt, lower, castcal */
extern ray_t* g_funsql_select_value;                          /* used by: funsql, registry */
extern ray_t* g_funsql_bang_value;                            /* used by: funsql, registry */
ray_t* q_registry_list_value(void);                           /* used by: fmt, lower, parse, join */

/* ---- defined in q_wrap_castcal.c ---- */
ray_t* q_value_resolve_sym_owned(ray_t* symv);                /* used by: table */
ray_t* q_value_wrap(ray_t* x);                                /* used by: bang, builtins, registry */
int8_t q_cast_designator(ray_t* t, int* is_tok);              /* used by: io */
const char* q_type_qname(int8_t t);                           /* used by: table */
ray_t* q_cast_to(int8_t tag, ray_t* x);                       /* used by: agg */
int    q_int_index_width(int8_t t);                           /* used by: table */
int    q_strict_i64(ray_t* x, int64_t* out);                  /* strict-cast probe (type-judgment home) — used by: applyiter, bang, io, list */
int    q_strict_f64(ray_t* x, double* out);                   /* used by: list */
ray_t* q_i64_or_err(ray_t* x, int64_t* out, const char* what);/* throwing gate — used by: agg, io, list */
ray_t* q_f64_or_err(ray_t* x, double* out, const char* what); /* f64 twin (no gate site yet) */
int q_is_int_atom(ray_t* x);                                  /* used by: io, table */
int q_is_int_vec(ray_t* x);                                   /* used by: io, list */
int64_t q_ivec_get(ray_t* v, int64_t i);                      /* used by: io, list */
int64_t q_iatom_val(ray_t* x);                                /* used by: io, list, table */
ray_t* q_tok_to(int8_t tag, ray_t* x);                        /* used by: io */

/* ---- defined in q_wrap_io.c ---- */
ray_t* q_hopen_wrap(ray_t* x);                                /* used by: apply, registry */
ray_t* q_hclose_wrap(ray_t* x);                               /* used by: apply, registry */
ray_t* q_hsym_wrap(ray_t* x);                                 /* used by: bang, registry */
ray_t* q_read0_wrap(ray_t* x);                                /* used by: builtins, registry */
ray_t* q_ssr_wrap(ray_t** args, int64_t n);                   /* used by: builtins, registry */

/* ---- defined in q_wrap_join.c ---- */
int qj_same_schema(ray_t* a, ray_t* b);                       /* used by: table */
ray_t* qj_table_gather_idx(ray_t* t, const int64_t* idx, int64_t n);/* used by: table, list */
ray_t* qj_ktbl_merge(ray_t* x, ray_t* y, int mode);           /* used by: list, table */

/* ---- defined in q_wrap_list.c ---- */
ray_t* q_attr_wrap(ray_t* x);                                 /* used by: bang, registry */
ray_t* q_take_wrap(ray_t* n, ray_t* list);                    /* used by: ops, registry */
ray_t* q_typed_empty_like(ray_t* collapsed, ray_t* proto);    /* used by: math */
ray_t* q_fill_wrap(ray_t* x, ray_t* y);                       /* used by: registry, agg, join */
ray_t* q_env_call2(const char* nm, ray_t* a, ray_t* b);       /* used by: join, table */
int q_is_keyed_table(ray_t* y);                               /* used by: apply, builtins, agg, join, math, table */
ray_t* q_table_flatten(ray_t* y);                             /* used by: ops, join, table */
ray_t* q_enkey(ray_t* y, int64_t nkey);                       /* used by: ops, join, table */

/* ---- defined in q_wrap_math.c ---- */
ray_t* q_min2_wrap(ray_t* a, ray_t* b);                       /* used by: ops, registry */
int q_match_rec(ray_t* a, ray_t* b);                          /* used by: table */
ray_t* q_match_wrap(ray_t* a, ray_t* b);                      /* used by: registry, table */
ray_t* q_til_wrap(ray_t* x);                                  /* used by: registry, table */
ray_t* q_dict_vals_vec(ray_t* d, int* owned);                 /* used by: list */
int q_is_null_sym(ray_t* x);                                  /* used by: agg */
ray_t* q_str_split_lines(const char* y, size_t yl);           /* used by: io */

/* ---- defined in q_wrap_table.c ---- */
ray_t* q_flip_wrap(ray_t* x);                                 /* used by: registry, agg, list */
ray_t* qj_item(ray_t* x, int64_t i);                          /* used by: join */
ray_t* qj_gen_item(ray_t* x, int64_t i);                      /* used by: join */
ray_t* q_setg_wrap(ray_t* x, ray_t* y);                       /* used by: dotz, lower, registry */

#endif
