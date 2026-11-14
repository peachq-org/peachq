/* q_key.c — the `key` / `keys` verb bodies (contract: q_key.h). */
#include "qlang/q_count.h"
#include "qlang/ops/q_key.h"
#include "qlang/base/q_err.h"
#include "qlang/q_env.h"              /* q_env_handle_resolve / _ns_roster */
#include "qlang/base/q_type.h"             /* q_type_qname + the is_dict/is_str_atom/is_keyed probes */
#include "qlang/q_registry_internal.h" /* q_til_wrap — the til arm; q_mmu_class/q_inv_wrap — the inv arm */
#include "qlang/ops/q_table.h"        /* q_table_operand — `keys` takes a table by name */
#include "qlang/io/q_provider.h"      /* q_provider_key — a `:pq:` alias lists its tables, a coordinate its columns */
#include "qlang/io/q_io.h"            /* q_io_list — the byte reader's listing */
#include "table/sym.h"                /* ray_sym_intern_runtime, ray_sym_str */
#include <string.h>

ray_t* q_key_dir(int64_t hsym) {
    ray_t* s = ray_sym_str(hsym);
    if (!s) return q_err(QE_TYPE);
    ray_t* p = ray_str(ray_str_ptr(s) + 1, ray_str_len(s) - 1);   /* past the leading ':' */
    ray_release(s);
    if (!p || RAY_IS_ERR(p)) return p ? p : q_err(QE_WSFULL);
    ray_t* ls = q_io_list(p);
    ray_release(p);
    return ls ? ls : ray_sym(hsym);                  /* a file -> the descriptor */
}

ray_t* q_key_name(int64_t sym) {
    ray_t* s = ray_sym_str(sym);
    if (!s) return q_err(QE_TYPE);
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    if (l == 0) {                                /* `key ` — the namespaces */
        ray_release(s);
        ray_t* r = q_env_ns_roster();
        return r ? r : q_err(QE_WSFULL);
    }
    if (nm[0] == ':') {
        ray_release(s);
        ray_t* a = ray_sym(sym);
        ray_t* pk = q_provider_key(a);
        ray_release(a);
        return pk ? pk : q_key_dir(sym);
    }
    if (l == 2 && nm[0] == '.' && nm[1] == 'z') {   /* ref/dotz.md: a pseudo-namespace no key enumerates */
        ray_release(s);
        return ray_list_new(1);
    }
    ray_release(s);
    ray_t* v = q_env_handle_resolve(sym);
    if (!v) return ray_list_new(1);              /* unbound -> () */
    if (RAY_IS_ERR(v)) return v;
    if (!q_type_is_dict(v)) {                    /* bound non-dict -> the name */
        ray_release(v);
        return ray_sym(sym);
    }
    ray_t* k = ray_dict_keys(v);                 /* borrowed */
    if (!k) { ray_release(v); return q_err(QE_TYPE); }
    ray_retain(k);
    ray_release(v);
    return k;
}

ray_t* q_key(ray_t* x) {
    if (q_enum_is(x))                            /* `key e` -> the domain name
                                                  * (ref/enumerate.md) */
        return ray_sym(q_enum_domain(x));
    if (x && ray_is_vec(x)) {
        const char* nm = q_type_qname(x->type);
        if (nm) return ray_sym(ray_sym_intern_runtime(nm, strlen(nm)));
        /* an unnamed vector tag keeps the deferred 'type tail below */
    }
    if (q_type_is_str_atom(x)) {                 /* the physical-storage shim */
        const char* nm = q_type_qname(RAY_CHARV);
        return ray_sym(ray_sym_intern_runtime(nm, strlen(nm)));
    }
    if ((q_type_is_int_atom(x) && !RAY_ATOM_IS_NULL(x) && q_type_iatom_val(x) >= 0) || (x && x->type == -RAY_BOOL))
        return q_til_wrap(x);                    /* key n == til n, and `til 1b` is ,0 (ref/til.md) */
    if (q_type_is_sym_atom(x))
        return q_key_name(x->i64);
    if (!q_type_is_dict(x))                      /* a keyed table IS a dict */
        return q_err(QE_TYPE);
    ray_t* k = ray_dict_keys(x);                 /* borrowed */
    if (!k) return q_err(QE_TYPE);
    ray_retain(k);
    return k;
}

/* `!:` is also inv: a float matrix, which key refuses, inverts (kdb writes inv as `!:`) */
ray_t* q_key_wrap(ray_t* x) {
    int64_t w;
    int c = q_mmu_class(x, &w);
    return c == 1 || c == QMMU_RAGGED ? q_inv_wrap(x) : q_key(x);
}

/* q `keys x` — key column names (empty sym vector if unkeyed; table by value
 * or by name). */
ray_t* q_keys_wrap(ray_t* x) {
    int64_t sym;
    ray_t* t = q_table_operand(x, &sym);
    if (!t) return q_err(QE_TYPE);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
    if (q_type_is_keyed(t)) {
        ray_t* kt = ray_dict_keys(t);                     /* borrowed */
        int64_t knc = ray_table_ncols(kt);
        for (int64_t c = 0; c < knc; c++) {
            int64_t nm = ray_table_col_name(kt, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        }
    }
    return out;
}
