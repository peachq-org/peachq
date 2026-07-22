/* ops/q_dotq.c — the .Q introspection surface: .Q.ty / .Q.qt / .Q.qp / .Q.s
 * (via the .Q.c.* C seam) and .Q.ops (the manifest as a read-only table).
 * Evicted from q_builtins.c; the type-letter kernel (q_ty_char) stays there. */
#include "qlang/q_builtins.h"   /* q_ty_char + this file's decls */
#include "qlang/q_ops.h"        /* q_ops_table — the .Q.ops source */
#include "qlang/q_registry.h"   /* q_table_is_keyed — .Q.qt keyed arm */
#include "qlang/q_fmt.h"        /* .Q.s — the q console display string */
#include "lang/internal.h"
#include "table/sym.h"
#include <string.h>
#include <stdlib.h>


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
    return ray_bool(x && (x->type == RAY_TABLE || q_table_is_keyed(x)));
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
    if (!buf) return ray_error("wsfull", ".Q.s: out of memory");
    buf[0] = '\0';
    q_fmt_console(x, buf, cap);                      /* `.Q.s` OBEYS `\c` */
    size_t len = strlen(buf);
    while (cap < (1u << 24)) {                       /* grow until length settles */
        size_t ncap = cap * 2;
        char* nb = realloc(buf, ncap);
        if (!nb) { free(buf); return ray_error("wsfull", ".Q.s: out of memory"); }
        buf = nb;
        cap = ncap;
        buf[0] = '\0';
        q_fmt_console(x, buf, cap);
        size_t nlen = strlen(buf);
        if (nlen == len) break;                      /* converged: full display */
        len = nlen;
    }
    char* out = malloc(len + 2);
    if (!out) { free(buf); return ray_error("wsfull", ".Q.s: out of memory"); }
    memcpy(out, buf, len);
    out[len] = '\n';                                 /* console line terminator */
    ray_t* r = ray_str(out, len + 1);
    free(out);
    free(buf);
    return r;
}

/* ---- .Q.ops — Q_OPS[] as a read-only introspection table ------------------
 * (.Q.ops[]) materializes the verb manifest (src/qlang/q_ops.c) as a fresh
 * unkeyed table each call, so user code can query the op roster without any
 * path into the immutable registry — mutating the returned table cannot change
 * how verbs resolve.  OWNER RULING 2026-07-14: lives under `.Q` (openq
 * extension entry beside .Q.qt/.Q.qp; kdb has no .Q.ops).  Columns:
 *   name          sym   verb spelling
 *   lexclass      sym   `glyph / `kw_infix / `kw_prefix / `adverb (QLEX_*)
 *   monadic       bool  1b iff Q_OPS gives a build recipe at the monadic slot
 *                       (mon.kind != QK_NONE).  NB for the VARY prefix-keyword
 *                       verbs (ej/aj/wj/... — QR_ENV in the monadic slot) this
 *                       means "resolvable in prefix/bracket position", NOT that
 *                       the verb is strictly arity-1; true arity is not modelled
 *                       in the manifest.
 *   dyadic        bool  1b iff Q_OPS gives a build recipe at the dyadic slot
 *   deterministic bool  1b unless the verb's result is nondeterministic
 *   sideeffect    bool  1b iff evaluation performs an observable side effect
 *   family        sym   structure-dispatch family (`atomic`map`aggregate`index
 *                       `rowid`structural`irregular`none — q_ops.h vocabulary,
 *                       FAMILY AUDIT in q_ops.c; pure metadata this stage)
 * Per-verb help strings live in docs/q-ops-help.tsv (archival, not in the binary).
 * The x argument (`.Q.ops[]` passes `::`) is ignored — the table is constant
 * per build, keyed only off the static manifest. */
static const char* dotq_lexclass_name(q_lex_class c) {
    switch (c) {
    case QLEX_GLYPH:     return "glyph";
    case QLEX_KW_INFIX:  return "kw_infix";
    case QLEX_KW_PREFIX: return "kw_prefix";
    case QLEX_ADVERB:    return "adverb";
    }
    return "unknown";
}

ray_t* q_dotq_ops_fn(ray_t** args, int64_t nargs) {
    (void)args; (void)nargs;                   /* `.Q.ops[]` / `.Q.ops x` — arg ignored */
    int n = 0;
    const q_op_t* ops = q_ops_table(&n);
    int64_t cap = n > 0 ? n : 1;
    ray_t* name = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t* lexc = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t* mon  = ray_vec_new(RAY_BOOL, cap);
    ray_t* dya  = ray_vec_new(RAY_BOOL, cap);
    ray_t* det  = ray_vec_new(RAY_BOOL, cap);
    ray_t* eff  = ray_vec_new(RAY_BOOL, cap);
    ray_t* fam  = ray_sym_vec_new(RAY_SYM_W64, cap);
    ray_t* cols[7] = { name, lexc, mon, dya, det, eff, fam };
    for (int i = 0; i < 7; i++)
        if (!cols[i] || RAY_IS_ERR(cols[i])) {
            for (int j = 0; j < 7; j++)
                if (cols[j] && !RAY_IS_ERR(cols[j])) ray_release(cols[j]);
            return ray_error("wsfull", ".Q.ops: out of memory");
        }
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        int64_t nm  = ray_sym_intern(ops[i].name, strlen(ops[i].name));
        const char* lc = dotq_lexclass_name(ops[i].lex);
        int64_t lci = ray_sym_intern(lc, strlen(lc));
        uint8_t bm = ops[i].mon.kind  != QK_NONE;
        uint8_t bd = ops[i].dyad.kind != QK_NONE;
        uint8_t bt = ops[i].deterministic ? 1 : 0;
        uint8_t be = ops[i].sideeffect ? 1 : 0;
        int64_t fmi = ray_sym_intern(ops[i].family, strlen(ops[i].family));
        name = ray_vec_append(name, &nm);
        lexc = ray_vec_append(lexc, &lci);
        mon  = ray_vec_append(mon,  &bm);
        dya  = ray_vec_append(dya,  &bd);
        det  = ray_vec_append(det,  &bt);
        eff  = ray_vec_append(eff,  &be);
        fam  = ray_vec_append(fam,  &fmi);
        if (!name || RAY_IS_ERR(name) || !lexc || RAY_IS_ERR(lexc) ||
            !mon || RAY_IS_ERR(mon) || !dya || RAY_IS_ERR(dya) ||
            !det || RAY_IS_ERR(det) || !eff || RAY_IS_ERR(eff) ||
            !fam || RAY_IS_ERR(fam)) ok = 0;
    }
    ray_t* built[7] = { name, lexc, mon, dya, det, eff, fam };
    if (!ok) {
        for (int j = 0; j < 7; j++)
            if (built[j] && !RAY_IS_ERR(built[j])) ray_release(built[j]);
        return ray_error("wsfull", ".Q.ops: build failed");
    }
    static const char* colnames[7] =
        { "name", "lexclass", "monadic", "dyadic", "deterministic", "sideeffect",
          "family" };
    ray_t* t = ray_table_new(7);
    for (int i = 0; i < 7; i++) {
        if (!RAY_IS_ERR(t))               /* stop adding once errored... */
            t = ray_table_add_col(t, ray_sym_intern(colnames[i], strlen(colnames[i])), built[i]);
        ray_release(built[i]);            /* ...but always drop our ref (add_col retains) */
    }
    return t;
}
