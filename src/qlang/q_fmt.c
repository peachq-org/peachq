/* q_fmt — see q_fmt.h.  The start of a q-correct value formatter (the outputter
 * that will eventually back q's `.Q.s`).  It renders int vectors, symbol atoms
 * and general lists q-style and delegates everything else to rayforce's ray_fmt. */
#include "qlang/q_fmt.h"
#include "lang/format.h"   /* ray_fmt */
#include <string.h>
#include <stdio.h>

/* The verb characters — a sym atom of one of these (or the generic null `::`)
 * prints bare; every other sym keeps its leading backtick.  Same split the
 * retired q_ast_fmt made between verbs/null and names/sym-literals. */
static const char Q_VERBS[] = ":+-*%!&|<>=~,^#_$?@.";

static int q_sym_bare(const char* nm, size_t l) {
    if (l == 1 && nm[0] && strchr(Q_VERBS, nm[0])) return 1;
    if (l == 2 && nm[0] == ':' && nm[1] == ':') return 1;
    return 0;
}

/* Format a -RAY_SYM atom into buf: verbs/null bare, everything else backticked. */
static void q_fmt_sym(ray_t* val, char* buf, size_t bufsz) {
    ray_t* s = ray_sym_str(val->i64);
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    snprintf(buf, bufsz, "%s%.*s", q_sym_bare(nm, l) ? "" : "`", (int)l, nm);
    ray_release(s);
}

/* rayforce's formatter, captured as a string — the fallback for values q_fmt
 * doesn't yet render q-style. */
static void ray_fallback(ray_t* val, char* buf, size_t bufsz) {
    ray_t* s = ray_fmt(val, 0);
    if (s && !RAY_IS_ERR(s) && s->type == -RAY_STR) {
        size_t n = ray_str_len(s);
        if (n >= bufsz) n = bufsz - 1;
        memcpy(buf, ray_str_ptr(s), n);
        buf[n] = '\0';
    }
    if (s && !RAY_IS_ERR(s)) ray_release(s);
}

void q_fmt(ray_t* val, char* buf, size_t bufsz) {
    if (bufsz == 0) return;
    buf[0] = '\0';
    if (!val) return;

    /* A symbol atom prints q-style: `sym` (or bare for a verb/null name-ref).
     * Handling it here also renders the -RAY_SYM heads of parse-tree lists. */
    if (val->type == -RAY_SYM) {
        q_fmt_sym(val, buf, bufsz);
        return;
    }

    /* q prints a simple vector space-separated with NO brackets: `5 6 7`,
     * where rayforce prints `[5 6 7]`.  Handle int vectors here (the common
     * arithmetic / til / reverse result); everything else falls back to the
     * rayforce formatter for now.
     *
     * Note: a 1-element vector is left as e.g. `5` rather than q's enlist form
     * `,5` for the moment — refined when the full q outputter lands. */
    if (val->type == RAY_I64) {
        int64_t n = ray_len(val);
        const int64_t* d = (const int64_t*)ray_data(val);
        size_t pos = 0;
        for (int64_t i = 0; i < n; i++) {
            char e[24];
            int m = snprintf(e, sizeof e, "%s%lld", i ? " " : "", (long long)d[i]);
            if (m < 0) break;
            if (pos + (size_t)m + 1 > bufsz) break;   /* keep room for the NUL */
            memcpy(buf + pos, e, (size_t)m);
            pos += (size_t)m;
        }
        buf[pos] = '\0';
        return;
    }

    /* A general list renders q-style as (a;b;c), recursively.  This is how a
     * parse tree prints: `parse "2+3"` -> (+;2;3). */
    if (val->type == RAY_LIST) {
        size_t pos = 0;
        if (pos + 1 < bufsz) buf[pos++] = '(';
        int64_t n = ray_len(val);
        ray_t** e = (ray_t**)ray_data(val);
        for (int64_t i = 0; i < n; i++) {
            if (i && pos + 1 < bufsz) buf[pos++] = ';';
            char elem[1024];
            elem[0] = '\0';
            q_fmt(e[i], elem, sizeof elem);      /* recurse (syms handled above) */
            size_t el = strlen(elem);
            if (pos + el + 1 >= bufsz) el = (bufsz > pos + 1) ? bufsz - 1 - pos : 0;
            memcpy(buf + pos, elem, el);
            pos += el;
        }
        if (pos + 1 < bufsz) buf[pos++] = ')';
        buf[pos] = '\0';
        return;
    }

    ray_fallback(val, buf, bufsz);
}
