/* ops/q_dotq.c — the .Q introspection surface: .Q.ty / .Q.qt / .Q.qp / .Q.s
 * (via the .Q.c.* C seam) and .Q.ops (the whole verb surface as a read-only
 * table: the C manifest unioned with the q-source stdlib).
 * Evicted from q_builtins.c; the type-letter kernel (q_ty_char) stays there. */
#include "qlang/q_builtins.h"   /* q_ty_char + this file's decls */
#include "qlang/base/q_err.h"
#include "qlang/q_ops.h"        /* q_ops_table — the .Q.ops source */
#include "qlang/base/q_type.h"       /* q_type_is_keyed — .Q.qt keyed arm */
#include "qlang/q_fmt.h"        /* .Q.s — the q console display string */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_rank — .Q.ops q-side valence */
#include "qlang/q_registry.h"   /* q_registry_qsrc_ns — the `.q` roster */
#include "qlang/io/q_splay.h"   /* q_splay_mapped_bytes/_zblocks — .Q.w, .Q.c.zblocks */
#include "qlang/net/q_wirefile.h" /* q_wirefile_en — .Q.en's format-home body */
#include "table/dict.h"         /* ray_dict_find_sym — the `.q` roster probe */
#include "mem/heap.h"           /* ray_mem_stats — .Q.w's heap fields */
#include "lang/internal.h"
#include "table/sym.h"
#include <string.h>
#include <stdlib.h>
#ifndef RAY_OS_WINDOWS
#include <unistd.h>             /* sysconf — .Q.w's mphy */
#endif


/* (.Q.ty x) — LOWER char for a simple vector / string, UPPER for a uniform
 * list of vectors, blank (" ") otherwise (ref/dotq.md).  Returns a 1-char
 * string (openq has no char atom; `.Q.ty each` over strings therefore yields a
 * list of 1-char strings, not one packed char vector — the `"jc JC"` combined
 * example is a deferred string-model cell). */
ray_t* q_dotq_ty_fn(ray_t* x) {
    char c = q_ty_char(x);
    return ray_str(&c, 1);
}

/* (.Q.qt x) — is-table predicate (ref/dotq.md `qt`): 1b if x is a table
 * (simple OR keyed), else 0b. */
ray_t* q_dotq_qt_fn(ray_t* x) {
    return ray_bool(x && (x->type == RAY_TABLE || q_type_is_keyed(x)));
}

/* (.Q.qp x) — is-partitioned predicate (ref/dotq.md `qp`): partitioned table
 * -> 1b, splayed table -> 0b, anything else -> 0 (a LONG, not a bool).  openq
 * has no on-disk partitioned or splayed tables, so nothing in memory is either
 * — every value falls into the "anything else" arm and returns the long 0.
 * The doc's own `.Q.qp select from B -> 0` pins this: an in-memory table (as
 * openq's tables always are) is NOT splayed, so it returns 0 (long), not 0b. */
ray_t* q_dotq_qp_fn(ray_t* x) {
    (void)x;
    return ray_i64(0);
}

/* (.Q.s x) — x formatted to plain text as the console prints it (ref/dotq.md
 * `.Q.s`), returned as a q string.  SINGLE-HOMES to q_fmt_console — the same
 * DISPLAY seam `show` uses (q_console_show = q_fmt_console + '\n') — so
 * `.Q.s x` is byte-identical to what `show x` prints, INCLUDING the
 * line-terminating trailing newline, and OBEYS the `\c` console width/height
 * ("Obeys console width and height set by \c", ref/dotq.md) and `\P`
 * precision.  We do NOT reuse q_console_show here: that appends into the
 * global console sink the REPL/qdoc host drains after each eval, so routing
 * through it would inject `.Q.s`'s text into the host's output — `.Q.s` must
 * be side-effect-free and RETURN the string.
 * The renderer truncates silently into a bounded buffer, so grow until the
 * rendered length CONVERGES (a larger buffer yields no more bytes).  NB
 * `len < cap-1` is NOT a reliable fit test: the renderer stops at an element
 * boundary when the buffer fills, so a truncated render can still leave room
 * at the tail (e.g. an unclipped `.Q.s til 5000` fills only ~8190 of 8192).
 * Convergence is reliable because the output length is monotonic
 * non-decreasing in buffer size — equal length after doubling means the whole
 * display fit.  Capped to stay bounded. */
ray_t* q_dotq_s_fn(ray_t* x) {
    size_t cap = 8192;
    char* buf = malloc(cap);
    if (!buf) return q_err(QE_WSFULL);
    buf[0] = '\0';
    q_fmt_console(x, buf, cap);                      /* `.Q.s` OBEYS `\c` */
    size_t len = strlen(buf);
    while (cap < (1u << 24)) {                       /* grow until length settles */
        size_t ncap = cap * 2;
        char* nb = realloc(buf, ncap);
        if (!nb) { free(buf); return q_err(QE_WSFULL); }
        buf = nb;
        cap = ncap;
        buf[0] = '\0';
        q_fmt_console(x, buf, cap);
        size_t nlen = strlen(buf);
        if (nlen == len) break;                      /* converged: full display */
        len = nlen;
    }
    char* out = malloc(len + 2);
    if (!out) { free(buf); return q_err(QE_WSFULL); }
    memcpy(out, buf, len);
    out[len] = '\n';                                 /* console line terminator */
    ray_t* r = ray_charv(out, len + 1);              /* 10h — RAY_STR is q-invisible */
    free(out);
    free(buf);
    return r;
}

/* ---- .Q.ops — the whole verb surface as a read-only introspection table ----
 * (.Q.ops[]) materializes the roster fresh each call, so user code can query it
 * without any path into the immutable registry — mutating the returned table
 * cannot change how verbs resolve.  OWNER RULING 2026-07-14: lives under `.Q`
 * (openq extension beside .Q.qt/.Q.qp; kdb has no .Q.ops).
 * The surface has TWO homes and this is their UNION (`impl` says which):
 *   `c  a Q_OPS[] row (src/qlang/q_ops.c) carrying a C build recipe;
 *   `q  the definition is q source in q.q.  Prefix keywords defined there get
 *       NO manifest row by design (docs/adding-q-function-guide.md) — the bare
 *       name resolves through `.q` — so they are gathered from that namespace,
 *       as are the QK_QSRC rows (a lexer row over a q.q body).
 * Columns: name lexclass monadic dyadic deterministic sideeffect family impl.
 * `monadic`/`dyadic` for an `impl`q` row is the RANK of the value that actually
 * runs (0b/0b when it has no fixed rank); for an `impl`c` row it is "the
 * manifest gives a recipe at that slot" — for the VARY prefix keywords
 * (ej/aj/wj...) that means "resolvable in prefix position", not arity 1.
 * `deterministic`/`sideeffect`/`family` are manifest data: an `impl`q` row with
 * no manifest row of its own carries the neutral 1b/0b, NOT an audit, and a NULL
 * family — `none is an operative lift-law value (exception-catalogue membership),
 * so family is inapplicable here, not unclassified.
 * Per-verb help strings live in docs/q-ops-help.tsv (archival, not the binary).
 * The x argument (`.Q.ops[]` passes `::`) is ignored. */
static const char* dotq_lexclass_name(q_lex_class c) {
    switch (c) {
    case QLEX_GLYPH:     return "glyph";
    case QLEX_KW_INFIX:  return "kw_infix";
    case QLEX_KW_PREFIX: return "kw_prefix";
    case QLEX_ADVERB:    return "adverb";
    }
    return "unknown";
}

#define DOTQ_OPS_NCOLS 8

static void dotq_ops_row(ray_t** c, int* ok, int64_t nm, const char* lex,
                         uint8_t mon, uint8_t dya, uint8_t det, uint8_t eff,
                         const char* fam, const char* impl) {
    if (!*ok) return;
    int64_t lexi = ray_sym_intern(lex, strlen(lex));
    int64_t fami = ray_sym_intern(fam, strlen(fam));
    int64_t impi = ray_sym_intern(impl, strlen(impl));
    const void* cell[DOTQ_OPS_NCOLS] =
        { &nm, &lexi, &mon, &dya, &det, &eff, &fami, &impi };
    for (int i = 0; i < DOTQ_OPS_NCOLS; i++) {
        c[i] = ray_vec_append(c[i], cell[i]);
        if (!c[i] || RAY_IS_ERR(c[i])) { *ok = 0; return; }
    }
}

ray_t* q_dotq_ops_fn(ray_t** args, int64_t nargs) {
    (void)args; (void)nargs;                   /* `.Q.ops[]` / `.Q.ops x` — arg ignored */
    int n = 0;
    const q_op_t* ops = q_ops_table(&n);
    ray_t* ns   = q_registry_qsrc_ns();            /* owned, marker-free */
    ray_t* nsk  = ns ? ray_dict_keys(ns) : NULL;   /* borrowed */
    ray_t* nsv  = ns ? ray_dict_vals(ns) : NULL;   /* borrowed */
    int64_t nsn = nsk ? ray_len(nsk) : 0;
    int64_t cap = n + nsn > 0 ? n + nsn : 1;
    ray_t* c[DOTQ_OPS_NCOLS];
    for (int i = 0; i < DOTQ_OPS_NCOLS; i++)
        c[i] = (i == 2 || i == 3 || i == 4 || i == 5) ? ray_vec_new(RAY_BOOL, cap)
                                                      : ray_sym_vec_new(RAY_SYM_W64, cap);
    uint8_t* taken = calloc(nsn > 0 ? (size_t)nsn : 1, 1);
    int ok = taken != NULL;
    for (int i = 0; i < DOTQ_OPS_NCOLS; i++)
        if (!c[i] || RAY_IS_ERR(c[i])) ok = 0;
    for (int i = 0; i < n && ok; i++) {
        int64_t nm = ray_sym_intern(ops[i].name, strlen(ops[i].name));
        uint8_t bm = ops[i].mon.kind  != QK_NONE;
        uint8_t bd = ops[i].dyad.kind != QK_NONE;
        /* a row is q-implemented only when NO slot carries a C recipe */
        int has_c = (ops[i].mon.kind  != QK_NONE && ops[i].mon.kind  != QK_QSRC) ||
                    (ops[i].dyad.kind != QK_NONE && ops[i].dyad.kind != QK_QSRC);
        int64_t qi = has_c ? -1 : ray_dict_find_sym(ns, nm);
        if (qi >= 0) {
            taken[qi] = 1;
            int64_t r = q_eval_apply_rank(ray_list_get(nsv, qi));
            bm = r == 1;
            bd = r == 2;
        }
        dotq_ops_row(c, &ok, nm, dotq_lexclass_name(ops[i].lex), bm, bd,
                     ops[i].deterministic != 0, ops[i].sideeffect != 0,
                     ops[i].family, qi >= 0 ? "q" : "c");
    }
    for (int64_t j = 0; j < nsn && ok; j++) {
        if (taken[j]) continue;
        int64_t nm = ray_read_sym(ray_data(nsk), j, RAY_SYM, nsk->attrs);
        int64_t r = q_eval_apply_rank(ray_list_get(nsv, j));
        dotq_ops_row(c, &ok, nm, "kw_prefix", r == 1, r == 2, 1, 0, "", "q");
    }
    free(taken);
    if (ns) ray_release(ns);
    if (!ok) {
        for (int i = 0; i < DOTQ_OPS_NCOLS; i++)
            if (c[i] && !RAY_IS_ERR(c[i])) ray_release(c[i]);
        return q_err(QE_WSFULL);
    }
    static const char* colnames[DOTQ_OPS_NCOLS] =
        { "name", "lexclass", "monadic", "dyadic", "deterministic", "sideeffect",
          "family", "impl" };
    ray_t* t = ray_table_new(DOTQ_OPS_NCOLS);
    for (int i = 0; i < DOTQ_OPS_NCOLS; i++) {
        if (!RAY_IS_ERR(t))               /* stop adding once errored... */
            t = ray_table_add_col(t, ray_sym_intern(colnames[i], strlen(colnames[i])), c[i]);
        ray_release(c[i]);                /* ...but always drop our ref (add_col retains) */
    }
    return t;
}

/* (.Q.en[dom;t]) — the thin compat shim over the domain-append primitive
 * (ref/dotq.md#en-enumerate-varchar-cols); the body lives at the format home. */
ray_t* q_dotq_en_fn(ray_t* dom, ray_t* t) {
    return q_wirefile_en(dom, t);
}

/* (.Q.c.zblocks[]) — cumulative compressed blocks inflated (internal witness:
 * the splay-lazy suites assert per-gather deltas). */
ray_t* q_dotq_zblocks_fn(ray_t** args, int64_t nargs) {
    (void)args; (void)nargs;
    return ray_i64(q_splay_zblocks());
}

/* (.Q.w[]) — memory stats as a dict (ref/dotq.md#w-memory-stats): the `\w`
 * slots plus syms/symw.  mmap = the splay registry's LIVE mapped bytes (its
 * region totals — what one munmap sweep would return); mphy = physical RAM;
 * wmax 0 (no -w limit) and symw 0 (no cheap total) are recorded gaps. */
ray_t* q_dotq_w_fn(ray_t** args, int64_t nargs) {
    (void)args; (void)nargs;
    ray_mem_stats_t st;
    ray_mem_stats(&st);
    int64_t mphy = 0;
#ifndef RAY_OS_WINDOWS
    long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && psz > 0) mphy = (int64_t)pages * (int64_t)psz;
#endif
    static const char* names[8] =
        { "used", "heap", "peak", "wmax", "mmap", "mphy", "syms", "symw" };
    int64_t vals[8] = {
        (int64_t)st.bytes_allocated, (int64_t)st.sys_current,
        (int64_t)st.peak_bytes, 0, q_splay_mapped_bytes(), mphy,
        (int64_t)ray_sym_count(), 0,
    };
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, 8);
    ray_t* v = ray_vec_new(RAY_I64, 8);
    for (int i = 0; i < 8 && k && !RAY_IS_ERR(k) && v && !RAY_IS_ERR(v); i++) {
        int64_t id = ray_sym_intern_runtime(names[i], strlen(names[i]));
        k = ray_vec_append(k, &id);
        if (k && !RAY_IS_ERR(k)) v = ray_vec_append(v, &vals[i]);
    }
    if (!k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v)) {
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
        return q_err(QE_WSFULL);
    }
    return ray_dict_new(k, v);                          /* consumes both */
}
