#ifndef Q_BUILTINS_H
#define Q_BUILTINS_H
#include <rayforce.h>
/* Register q's own builtins into the current rayforce env (call once, after
 * the runtime is created).  Starts with `parse`; grows into q's verb table. */
void q_builtins_register(void);

/* q `string x` — cast-to-string (sym bare, atoms via the base formatter,
 * vectors/lists map element-wise).  Exposed as THE q cell formatter so
 * 0: Prepare Text renders cells through the same single home. */
ray_t* q_string_fn(ray_t* x);

/* q `parse` / `md5` / `.Q.btoa` verb implementations — exported so the q_bang.c
 * internal-fn aliases (`-5!`->parse, `-15!`->md5, `-32!`->.Q.btoa) route to the
 * SAME STABLE C single home the keyword is bound to, rather than the mutable env
 * value (so a user `parse:{…}` rebind cannot break `-5!`; kdb keeps `-N!` as the
 * primitive underneath the keyword). */
ray_t* q_parse_builtin_fn(ray_t* x);
ray_t* q_md5_fn(ray_t* x);
ray_t* q_dotq_btoa_fn(ray_t* x);
ray_t* q_dotq_atob_fn(ray_t* x);
ray_t* q_dotq_gz_fn(ray_t** args, int64_t argc);

/* `-14!` quote escape: one CSV cell through 0:'s Save-Text quote rule
 * (ops/q_io.c; file-text.md embrace-on-delimiter/newline, '"' doubled). */
ray_t* q_io_csv_quote(ray_t* x);

/* The string-verb bodies (ops/q_str.c): env-bound by q_builtins_register. */
ray_t* q_upper_fn(ray_t* x);
ray_t* q_lower_fn(ray_t* x);
ray_t* q_trim_fn(ray_t* x);
ray_t* q_ltrim_fn(ray_t* x);
ray_t* q_rtrim_fn(ray_t* x);

/* q `.Q.sha1` — SHA-1 digest of a string/byte vector as a 20-byte bytestream.
 * Exported so the `-33!` bang alias routes to this same stable C single home
 * (kdb keeps `-33!` as the SHA-1 primitive; `.Q.sha1` is the utility twin). */
ray_t* q_dotq_sha1_fn(ray_t* x);

/* q `count x` (fn-value -> 1, else the base count).  Exported so `!`'s length
 * gate (bang_make_dict) measures key/value counts with q semantics.
 * q_builtins_count_long is the int64 specialization for callers wanting the number. */
ray_t* q_count_fn(ray_t* x);
int64_t q_builtins_count_long(ray_t* x);

/* Hot-path count.  The ITERATORS ask for a count once per application, and
 * applications NEST, so at -O0 a cross-TU call per outer item is measurable:
 * routing the adverb arms through the boxed owner took integration/i078 from
 * 5.9s to 28s and timed the C runner out.  These three shapes answer exactly
 * as q_count_fn does; every other shape — dicts, function values, atoms,
 * pooled RAY_STR — falls through to it, so there is still ONE home. */
static inline int64_t q_count_long(ray_t* x) {
    if (x && !RAY_IS_ERR(x)) {
        if (x->type == RAY_LIST || (ray_is_vec(x) && x->type != RAY_STR))
            return ray_len(x);
        if (x->type == RAY_TABLE) return ray_table_nrows(x);
    }
    return q_builtins_count_long(x);
}

/* Type letter of one value as `.Q.ty`/`meta` see it — the type-fact kernel
 * kept here; ops/q_dotq.c and ops/q_table.c consume it. */
char q_ty_char(ray_t* x);

/* The .Q introspection surface (ops/q_dotq.c) + table introspection
 * (ops/q_table.c): bound by q_builtins_register. */
ray_t* q_dotq_ty_fn(ray_t* x);
ray_t* q_dotq_qt_fn(ray_t* x);
ray_t* q_dotq_qp_fn(ray_t* x);
ray_t* q_dotq_s_fn(ray_t* x);
ray_t* q_dotq_ops_fn(ray_t** args, int64_t nargs);
ray_t* q_cols_fn(ray_t* x);
ray_t* q_meta_fn(ray_t* x);
#endif /* Q_BUILTINS_H */
