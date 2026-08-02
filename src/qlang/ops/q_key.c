/* q_key.c — the `key` / `keys` verb bodies (contract: q_key.h). */
#include "qlang/ops/q_key.h"
#include "qlang/q_err.h"
#include "qlang/q_env.h"              /* q_env_resolve / _ns_roster / _marker_sym */
#include "qlang/ops/q_type.h"             /* q_type_qname + the is_dict/is_str_atom/is_keyed probes */
#include "qlang/q_registry_internal.h" /* q_til_wrap — the til arm */
#include "qlang/ops/q_table.h"        /* q_table_operand — `keys` takes a table by name */
#include "lang/internal.h"            /* ray_fs_list_fn — the quarry's sorted directory listing */
#include "table/sym.h"                /* ray_sym_intern_runtime, ray_sym_str */
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

ray_t* q_key_dir(int64_t hsym) {
    ray_t* s = ray_sym_str(hsym);
    if (!s) return q_err(QE_TYPE);
    char path[1024];
    size_t n = ray_str_len(s) - 1;                   /* past the leading ':' */
    if (n >= sizeof path) { ray_release(s); return q_err(QE_DOMAIN); }
    memcpy(path, ray_str_ptr(s) + 1, n);
    path[n] = '\0';
    ray_release(s);

    /* Absent is determined POSITIVELY, never inferred from a failed listing:
     * a directory we cannot open (EACCES) or that runs the allocator out is
     * an ERROR, not a missing path (codex review 2026-07-30). */
    struct stat st;
    if (stat(path, &st) != 0)
        return (errno == ENOENT || errno == ENOTDIR)
             ? ray_list_new(1)                       /* () — absent, not empty */
             : q_err(QE_IO);
    if (!S_ISDIR(st.st_mode)) return ray_sym(hsym);  /* a file -> the descriptor */

    /* The quarry's ls sorts ascending, drops `.`/`..`, and yields the empty
     * SYMBOL vector for an empty directory; its own errors propagate. */
    ray_t* p = ray_str(path, n);
    if (!p || RAY_IS_ERR(p)) return p ? p : q_err(QE_WSFULL);
    ray_t* ls = ray_fs_list_fn(p);
    ray_release(p);
    return ls;
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
    if (nm[0] == ':') { ray_release(s); return q_key_dir(sym); }
    int is_root = (l == 1 && nm[0] == '.');
    int qualified = (nm[0] == '.');
    ray_release(s);

    /* ref/key.md: "interpreted relative to the current context if not fully
     * qualified" — the existence test does NOT walk out to the root. */
    int64_t look = qualified ? sym : q_env_qualify(q_env_ctx(), sym);
    ray_t* v = q_env_resolve(look);
    if (!v) return ray_list_new(1);              /* unbound -> () */
    if (RAY_IS_ERR(v)) return v;
    if (!q_type_is_dict(v)) {                    /* bound non-dict -> the name */
        ray_release(v);
        return ray_sym(sym);
    }
    /* The root lists OBJECTS only: env marks every namespace dictionary the
     * same way, but kdb prints that marker for `` `.foo `` and NOT for the
     * root (ref/key.md `key `.` beside `key `.q`). */
    if (is_root) {
        ray_t* marker = ray_sym(q_env_marker_sym());
        if (!marker || RAY_IS_ERR(marker)) {
            ray_release(v);
            return marker ? marker : q_err(QE_WSFULL);
        }
        v = ray_dict_remove(v, marker);          /* COW; consumes v */
        ray_release(marker);
        if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_WSFULL);
    }
    ray_t* k = ray_dict_keys(v);                 /* borrowed */
    if (!k) { ray_release(v); return q_err(QE_TYPE); }
    ray_retain(k);
    ray_release(v);
    return k;
}

ray_t* q_key(ray_t* x) {
    if (x && ray_is_vec(x)) {
        const char* nm = q_type_qname(x->type);
        if (nm) return ray_sym(ray_sym_intern_runtime(nm, strlen(nm)));
        /* an unnamed vector tag keeps the deferred 'type tail below */
    }
    if (q_type_is_str_atom(x)) {                 /* the physical-storage shim */
        const char* nm = q_type_qname(RAY_CHARV);
        return ray_sym(ray_sym_intern_runtime(nm, strlen(nm)));
    }
    if (q_type_is_int_atom(x) && !RAY_ATOM_IS_NULL(x) && q_type_iatom_val(x) >= 0)
        return q_til_wrap(x);                    /* key n == til n */
    if (q_type_is_sym_atom(x))
        return q_key_name(x->i64);
    if (!q_type_is_dict(x))                      /* a keyed table IS a dict */
        return q_err(QE_TYPE);
    ray_t* k = ray_dict_keys(x);                 /* borrowed */
    if (!k) return q_err(QE_TYPE);
    ray_retain(k);
    return k;
}

ray_t* q_key_wrap(ray_t* x) { return q_key(x); }

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
