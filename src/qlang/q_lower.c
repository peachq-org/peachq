/* q_lower.c — the ADR-0003 lowering pass, split from q_parse.c (2026-07-14,
 * pure moves): adverbs->HOFs, assignment->set, cond/control words, lambda
 * carriers, qSQL select/exec/delete + functional-form lowering, context
 * qualification (namespaces), and the public q_lower / q_ast_is_assign.
 * Shared parser<->lowerer internals live in q_parse_internal.h. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/q_parse.h"
#include "qlang/q_registry.h" /* q_registry_lookup_name, Q_DYADIC */
#include "qlang/q_ns.h"       /* q_ns_current, q_ns_is_unqualifiable */
#include "qlang/q_ops.h"      /* q_lex_is_kw_infix — static lexical manifest */
#include "qlang/q_deriv.h"    /* q_proj_new — 104h derived-verb carriers */
#include "qlang/q_dotz.h"     /* q_dotz_ipc_hook_index — .z.p* handler aliases */
#include "lang/env.h"        /* ray_fn_name; ray_sym_is_ipc_hook — IPC hook slots */
#include "table/sym.h"       /* ray_sym_vec_cell — qSQL dict-key/col names */
#include "core/numparse.h"   /* ray_parse_i64, ray_parse_f64 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include "qlang/q_parse_internal.h"

/* ===== q-semantic verb resolution (eval-time) ================================
 * The AST keeps q-glyph name-ref verb heads so `parse` prints them verbatim.
 * Just before eval, rewrite each DYADIC call `(verb; a; b)` whose head is a
 * q-glyph name-ref into the q op registry's function value for that (name,
 * dyadic) key: `% -> /` (float divide), `= -> ==`, `<> -> !=`.  A registry
 * miss (`:`, `;`, `{`, user names, `f[...]` apply, monadic shapes, and the
 * ordering glyphs `< > <= >=` / `div` whose q spelling already matches the
 * rayfall name) leaves the head untouched — eval then resolves it against
 * rayfall's env exactly as before.
 *
 * RETIRED as a public pass (2b): the PARSER now embeds dyadic registry values
 * at infix/bracket glyph heads, so the only shapes this still serves are
 * keyword-dyadic rows called in bracket/prefix form (`each[count;x]` — the
 * scanner keeps prefix keywords as name-ref nouns and `each` has no env
 * binding).  That residue lives on as ql_dyad_head inside the q_lower walker. */
static void ql_dyad_head(ray_t **slot) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    /* Only monadic (n==2) / dyadic (n==3) keyword calls are candidates — guard
     * BEFORE touching e[0], so an empty list () (e.g. the qSQL `c`/`a` clauses
     * that q_lower now walks) never reads past the end. */
    if (n != 2 && n != 3) return;
    ray_t **e = (ray_t **)ray_data(node);
    if (!e[0] || e[0]->type != -RAY_SYM || (e[0]->attrs & Q_ATTR_QUOTED))
        return;
    /* dyadic bracket/prefix keyword calls (each[f;x]); AND monadic keyword
     * calls whose registry cell is a q WRAPPER diverging from the env
     * builtin (floor: q returns longs, rayfall env floor keeps f64) —
     * pass-through rows swap to the identical env object (harmless). */
    q_valence_t val = (n == 3) ? Q_DYADIC : Q_MONADIC;
    ray_t *s = ray_sym_str(e[0]->i64);
    if (!s) return;
    ray_t *hit = q_registry_lookup_name(ray_str_ptr(s), ray_str_len(s), val);
    ray_release(s);
    /* ray_fn values only — a QK_QSRC cell's carrier value must stay a
     * name-ref (see q_embed): eval would re-walk an embedded carrier. */
    if (hit && (hit->type == RAY_UNARY || hit->type == RAY_BINARY ||
                hit->type == RAY_VARY)) {
        ray_retain(hit);    /* borrowed -> retain one, drop the name-ref */
        ray_release(e[0]);
        e[0] = hit;
    }
}

/* ===== q-lower: the ADR 0003 lowering pass =================================
 * Beyond the dyadic-head resolution above, the lowering walker rewrites the
 * q-only tree shapes rayfall eval cannot run into pure rayfall applications:
 *
 *   ((`/;F); x)     -> (fold; F'; x)          q over        f/x
 *   ((`/;F); x; y)  -> (fold; F'; x; y)       q seeded over x f/ y
 *   ((`\;F); x)     -> (q-scan; F'; x)        q scan        f\x   (+collapse)
 *   ((`';F); x)     -> (q-each; F'; x)        q each        f'x   (+collapse)
 *   (|/)x, (&/)x    -> (max; x), (min; x)     aggregate monomorphization —
 *                      rayfall has no dyadic max/min value to fold with.
 *
 * F' is the q-semantic DYADIC value for a glyph operand (q `%` folds with
 * float-divide, not modulo), the untouched subtree for user names / lambdas
 * (eval resolves those), and the value itself once the parser embeds values.
 * A glyph operand with no dyadic row and no aggregate mapping leaves the node
 * untouched — eval then errors exactly as it does today.  In-place rewrite
 * under the same rc==1 sole-owner precondition as the whole walker. */

static ray_t *ql_env_val(const char *nm) {
    return ray_env_get(ray_sym_intern_runtime(nm, strlen(nm)));
}

/* Registry DYADIC value for nm[0..l) usable EMBEDDED in a lowered tree:
 * ray_fn values only.  A QK_QSRC cell holds a carrier (q.q lambda/projection)
 * that eval would re-walk as an expression — callers get NULL and keep/emit a
 * name-ref sym instead (eval's name path resolves it to the same value). */
static ray_t *ql_reg_fnval(const char *nm, size_t l) {
    ray_t *v = q_registry_lookup_name(nm, l, Q_DYADIC);
    if (v && (v->type == RAY_UNARY || v->type == RAY_BINARY ||
              v->type == RAY_VARY))
        return v;
    return NULL;
}

/* ADVERB_NAMES index of a name-ref sym (0=' 1=/ 2=\ 3=': 4=/: 5=\:), or -1. */
static int ql_adv_id(ray_t *x) {
    if (!x || x->type != -RAY_SYM || (x->attrs & Q_ATTR_QUOTED)) return -1;
    ray_t *s = ray_sym_str(x->i64);
    if (!s) return -1;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int r = -1;
    for (int i = 0; i < 6 && r < 0; i++)
        if (strlen(ADVERB_NAMES[i]) == l && memcmp(ADVERB_NAMES[i], nm, l) == 0)
            r = i;
    ray_release(s);
    return r;
}

/* Rebuild *slot as (hof; fexpr; old-args...).  Borrows hof/fexpr (append
 * retains); releases the old node. */
static void ql_rewrite(ray_t **slot, ray_t *hof, ray_t *fexpr) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *repl = ray_list_new(n + 1);
    repl = ray_list_append(repl, hof);
    repl = ray_list_append(repl, fexpr);
    for (int64_t i = 1; i < n; i++) {
        if (e[i]) { repl = ray_list_append(repl, e[i]); }
        else      { ray_t *nul = q_null(); repl = ray_list_append(repl, nul); ray_release(nul); }
    }
    ray_release(node);
    *slot = repl;
}

/* Is `x` the elided-argument hole `::` (what q_null() emits): an unquoted
 * name-ref sym spelling "::"? */
static int ql_is_hole(ray_t *x) {
    if (!x || x->type != -RAY_SYM || (x->attrs & Q_ATTR_QUOTED)) return 0;
    ray_t *s = ray_sym_str(x->i64);
    if (!s) return 0;
    int r = (ray_str_len(s) == 2 && ray_str_ptr(s)[0] == ':' && ray_str_ptr(s)[1] == ':');
    ray_release(s);
    return r;
}

/* Is `x` a genuine ELIDED bracket slot (Q_ATTR_HOLE), as opposed to an
 * explicit `::` value?  Used only by the @/. lowering, which must treat an
 * explicit `::` as amend/trap data, never a projection hole. */
static int ql_is_elision_hole(ray_t *x) {
    return x && (x->attrs & Q_ATTR_HOLE) && ql_is_hole(x);
}

/* Value/native projection: an application `(Fval; a0; a1; …)` whose head is a
 * plain callable fn-VALUE (unary/binary/vary, NOT a special form) and which
 * carries at least one `::` elision hole becomes a `.q.proj` carrier over Fval,
 * shielded from eval by `quote` (returns its arg unevaluated).  The parser
 * lowers `1+`, `2*`, `+[1;]`, `(3+)`, `(20>)` to `(op;arg;::)`; without this
 * they would eval `op(arg, ::)` and error.  Zero holes = a full application,
 * left untouched.  Reuses the existing q_proj_new + q_apply_noun machinery, so
 * the carrier applies (and stacks under adverbs) exactly like `(+/)`. */
static void ql_project(ray_t **slot) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    if (n < 2) return;
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *h = e[0];
    if (!h) return;
    if (!(h->type == RAY_UNARY || h->type == RAY_BINARY || h->type == RAY_VARY)) return;
    if (h->attrs & RAY_FN_SPECIAL_FORM) return;   /* list/table/if/quote/select/… */
    /* Apply-At / Apply `@` and `.`.  An EXPLICIT `::` argument is REAL data
     * (whole-value amend `@[v;::;f]`, `.[m;();f]`, or a trap fx) — never a
     * projection hole; only a genuine bracket ELISION (Q_ATTR_HOLE) projects.
     * Because openq's parser collapses `::` and an elided slot to the same
     * spelling, this per-arg flag is the sole disambiguator. */
    {
        q_provenance_t pv;
        if (q_registry_provenance(h, &pv) && pv.spelling && pv.spelling[0] &&
            pv.spelling[1] == '\0' &&
            (pv.spelling[0] == '@' || pv.spelling[0] == '.')) {
            /* The `:` (assign / replace) function slot of Amend arrives as a
             * bare verb token that would eval as an unresolved name-ref ('name);
             * quote it so it self-evaluates to the symbol `:` the amend wrapper
             * detects.  `::` (len 2, the whole-value null) is left untouched. */
            for (int64_t i = 1; i < n; i++) {
                ray_t *a = e[i];
                if (a && a->type == -RAY_SYM && !(a->attrs & Q_ATTR_QUOTED)) {
                    ray_t *s = ray_sym_str(a->i64);
                    if (s) {
                        if (ray_str_len(s) == 1 && ray_str_ptr(s)[0] == ':')
                            a->attrs |= Q_ATTR_QUOTED;
                        ray_release(s);
                    }
                }
            }
            /* An elided argument (`@[count;;-1]`, `type @[;;0h]`) makes the
             * operator a PROJECTION.  Lower to the q.mkopproj builder so the
             * bound (non-hole) args are EVALUATED before binding — a name-ref
             * `count` or a lambda literal `{x+1}` becomes its value, which a
             * raw lower-time q_proj_new cannot do.  Explicit `::` args carry no
             * Q_ATTR_HOLE, so amend/trap forms never reach this. */
            int64_t oargc = n - 1;
            uint64_t omask = 0; int oholes = 0;
            if (oargc >= 1 && oargc <= 60)
                for (int64_t i = 0; i < oargc; i++)
                    if (ql_is_elision_hole(e[1 + i])) { omask |= (1ull << i); oholes++; }
            if (oholes > 0) {
                ray_t *mk = q_registry_mkopproj_value();
                if (mk) {
                    ray_t *repl = ray_list_new(oargc + 3);
                    ray_retain(mk); repl = ray_list_append(repl, mk); ray_release(mk);
                    ray_retain(h);  repl = ray_list_append(repl, h);  ray_release(h);
                    ray_t *ni = ray_i64(oargc);
                    repl = ray_list_append(repl, ni); ray_release(ni);
                    ray_t *mi = ray_i64((int64_t)omask);
                    repl = ray_list_append(repl, mi); ray_release(mi);
                    for (int64_t i = 0; i < oargc; i++)
                        if (!(omask & (1ull << i)))
                            repl = ray_list_append(repl, e[1 + i]);  /* bound value, RETAINED */
                    ray_release(node);
                    *slot = repl;
                }
            }
            return;
        }
    }
    int64_t argc = n - 1;
    if (argc < 1 || argc > 60) return;
    uint64_t mask = 0; int holes = 0;
    for (int64_t i = 0; i < argc; i++)
        if (ql_is_hole(e[1 + i])) { mask |= (1ull << i); holes++; }
    if (holes == 0) return;
    ray_t *args[64];
    for (int64_t i = 0; i < argc; i++)
        args[i] = (mask & (1ull << i)) ? NULL : e[1 + i];   /* holes -> NULL */
    ray_t *carrier = q_proj_new(h, args, argc, mask, holes);
    if (!carrier || RAY_IS_ERR(carrier)) { if (carrier) ray_release(carrier); return; }
    ray_t *quote = ql_env_val("quote");
    if (!quote) { ray_release(carrier); return; }
    ray_t *repl = ray_list_new(2);
    repl = ray_list_append(repl, quote);
    repl = ray_list_append(repl, carrier);
    ray_release(carrier);
    ray_release(node);
    *slot = repl;
}

/* Try the adverb-application rewrite on *slot (see the table above). */
static void ql_adv_app(ray_t **slot) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    if (!(n == 2 || n == 3)) return;
    if (!e[0] || e[0]->type != RAY_LIST || ray_len(e[0]) != 2) return;
    ray_t **h = (ray_t **)ray_data(e[0]);
    int adv = ql_adv_id(h[0]);
    if (adv < 0) return;
    ray_t *F = h[1];

    int f_is_value = F && (F->type == RAY_UNARY || F->type == RAY_BINARY ||
                           F->type == RAY_VARY);
    int f_is_name  = F && F->type == -RAY_SYM && !(F->attrs & Q_ATTR_QUOTED);
    int f_is_glyph = 0;
    ray_t *freg = NULL;                    /* registry dyadic value (borrowed) */
    if (f_is_name) {
        ray_t *s = ray_sym_str(F->i64);
        if (!s) return;
        const char *nm = ray_str_ptr(s);
        size_t l = ray_str_len(s);
        f_is_glyph = (l >= 1 && strchr(VERB_CHARS, nm[0]) != NULL);
        freg = ql_reg_fnval(nm, l);  /* carrier cells: keep the name-ref */
        ray_release(s);
    }

    /* the operand expression the HOF receives */
    ray_t *fexpr = NULL;
    if (f_is_value)                 fexpr = F;     /* post-flip embedded value */
    else if (freg)                  fexpr = freg;  /* q-semantic dyadic value  */
    else if (f_is_name && !f_is_glyph) fexpr = F;  /* user name: eval resolves */
    else if (F && F->type == RAY_LIST) fexpr = F;  /* lambda / nested derived  */

    if (adv == 1) {                                       /* `/` over/reduce/converge/do/while */
        if (!fexpr) {
            /* aggregate monomorphization: no dyadic |/& value exists, but
             * (|/)x IS max x and (&/)x IS min x. */
            if (n == 2 && f_is_name && f_is_glyph) {
                ray_t *s = ray_sym_str(F->i64);
                ray_t *agg = NULL;
                if (s && ray_str_len(s) == 1) {
                    char g = ray_str_ptr(s)[0];
                    if (g == '|') agg = ql_env_val("max");
                    if (g == '&') agg = ql_env_val("min");
                }
                if (s) ray_release(s);
                if (agg) {
                    ray_t *repl = ray_list_new(2);
                    repl = ray_list_append(repl, agg);
                    if (e[1]) { repl = ray_list_append(repl, e[1]); }
                    else      { ray_t *nul = q_null(); repl = ray_list_append(repl, nul); ray_release(nul); }
                    ray_release(node);
                    *slot = repl;
                }
            }
            return;
        }
        /* the over value dispatches reduce vs converge/do/while by f rank and
         * operand shape at runtime (q_over_wrap). */
        ray_t *ov = q_registry_over_value();
        if (ov) ql_rewrite(slot, ov, fexpr);
    } else if (adv == 2) {                                /* `\` scan (all forms) */
        if (!fexpr) return;
        ray_t *sc = q_registry_scan_value();
        if (sc) ql_rewrite(slot, sc, fexpr);
    } else if (adv == 0 && n == 2) {                      /* `'` each (monadic) */
        if (!fexpr) return;
        ray_t *ea = q_registry_lookup_name("each", 4, Q_DYADIC);
        if (ea) ql_rewrite(slot, ea, fexpr);
    } else if (adv == 0 && n == 3) {                      /* `'` each-both (dyadic) */
        if (!fexpr) return;
        ray_t *eb = q_registry_eachboth_value();
        if (eb) ql_rewrite(slot, eb, fexpr);
    } else if (adv == 3) {                                /* `':` each-prior (unary/seeded) */
        if (!fexpr) return;
        ray_t *pr = q_registry_prior_value();
        if (pr) ql_rewrite(slot, pr, fexpr);
    } else if ((adv == 4 || adv == 5) && n == 3) {        /* `/:` `\:` each-right/left */
        if (!fexpr) return;
        /* x f\: y -> (map-left f x y): f(x_i, y); x f/: y -> (map-right f x y):
         * f(x, y_i) — arg order matches, no swap needed. */
        ray_t *mr = ql_env_val(adv == 4 ? "map-right" : "map-left");
        if (mr) ql_rewrite(slot, mr, fexpr);
    }
}

/* A bare (adv; V) 2-list in VALUE position (assigned, passed, displayed —
 * NOT the head of an application, which ql_adv_app consumes) becomes a 104h
 * projection carrier: the HOF with V bound and the data operand open,
 * shielded from eval by rayfall `quote` (a special form returning its arg
 * unevaluated).  `g:(+/)` binds the carrier; applying it through a name is
 * 2c (needs the noun-head fallback). */
static void ql_deriv_value(ray_t **slot) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    if (n != 2) return;
    ray_t **e = (ray_t **)ray_data(node);
    int adv = ql_adv_id(e[0]);
    if (adv < 0) return;

    /* `/:` `\:` derive a BINARY verb — `x f/: y` == map-right(f;x;y).  Because
     * the operand f may be an EXPRESSION (a lambda `(q.fn …)` tree) that has to
     * be evaluated to a value first, the 2-hole carrier is built at EVAL time:
     * lower to `(q.mkderiv2; <map HOF>; f)`, which evaluates f then binds it.
     * This is what makes a stacked outer adverb `f/:\:` (map-left over the
     * map-right carrier) work over a lambda. */
    if (adv == 4 || adv == 5) {
        ray_t *hofv = ql_env_val(adv == 4 ? "map-right" : "map-left");
        ray_t *mk   = q_registry_mkderiv2_value();
        if (!hofv || !mk) return;
        ray_t *repl = ray_list_new(3);
        ray_retain(mk);   repl = ray_list_append(repl, mk);   ray_release(mk);
        ray_retain(hofv); repl = ray_list_append(repl, hofv); ray_release(hofv);
        repl = ray_list_append(repl, e[1]);   /* f expression, evaluated at eval */
        ray_release(node);
        *slot = repl;
        return;
    }

    /* `'` `/` `\` `':` derive a MONADIC verb (1 hole after the bound operand).
     * These operands are glyph/keyword values already resolvable at lower time. */
    ray_t *hof = NULL;
    if (adv == 1)      hof = q_registry_over_value();
    else if (adv == 2) hof = q_registry_scan_value();
    else if (adv == 0) hof = q_registry_lookup_name("each", 4, Q_DYADIC);
    else if (adv == 3) hof = q_registry_prior_value();
    if (!hof) return;
    ray_t *quote = ql_env_val("quote");
    if (!quote) return;
    /* A keyword operand (`neg'`) is still a name-ref sym — bind its VALUE in
     * the carrier (kdb derives from the value): env first (keywords, user
     * fns); glyphs were already embedded by the parser.  Miss -> keep the
     * sym (name error surfaces at apply). */
    ray_t *V = e[1];
    if (V && V->type == -RAY_SYM && !(V->attrs & Q_ATTR_QUOTED)) {
        ray_t *rv = ray_env_get(V->i64);
        if (rv) V = rv;                      /* borrowed; q_proj_new retains */
    }
    ray_t *args[2] = { V, NULL };            /* V bound, data operand open */
    ray_t *carrier = q_proj_new(hof, args, 2, 0x2u, 1);
    if (!carrier || RAY_IS_ERR(carrier)) { if (carrier) ray_release(carrier); return; }
    ray_t *repl = ray_list_new(2);
    repl = ray_list_append(repl, quote);
    repl = ray_list_append(repl, carrier);
    ray_release(carrier);
    ray_release(node);
    *slot = repl;
}

/* Assignment: rewrite `(:; name; val)` / `(::; name; val)` heads to rayfall
 * set — or let for a plain `:` INSIDE a lambda body (q locals; `::` stays the
 * global assign there, kdb-faithful) — enforcing the reserved-verb 'assign
 * invariant (ADR 0003 Decision 1).  Returns a RAY_ERROR to abort lowering, or
 * NULL to continue.  Non-sym assign targets (2:3, f[1]:x) are left untouched:
 * eval errors on them exactly as it does today (indexed assign is 2c+). */
static ray_t *ql_assign(ray_t **slot, int in_lambda) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    if (n != 3) return NULL;
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return NULL;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return NULL;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int is_local  = (l == 1 && nm[0] == ':');
    int is_global = (l == 2 && nm[0] == ':' && nm[1] == ':');
    ray_release(s);
    if (!is_local && !is_global) return NULL;
    if (!e[1] || e[1]->type != -RAY_SYM || (e[1]->attrs & Q_ATTR_QUOTED))
        return NULL;

    ray_t *ns = ray_sym_str(e[1]->i64);
    if (!ns) return NULL;
    /* reserved = manifest verbs + `.q`-hosted keywords (`reciprocal:5` stays
     * 'assign with its row moved to q.q; dotted targets never match) */
    int reserved = q_ops_is_reserved(ray_str_ptr(ns), (int)ray_str_len(ns)) ||
                   q_ns_dotq_get(ray_str_ptr(ns), ray_str_len(ns)) != NULL;
    if (reserved) {
        ray_t *err = ray_error("assign", "'assign: %.*s is a reserved verb",
                               (int)ray_str_len(ns), ray_str_ptr(ns));
        ray_release(ns);
        return err;
    }
    /* A leading-dot target (`.foo.x:42`) is a GLOBAL namespace write even
     * inside a lambda body — q dotted names are never locals (q4m3 §12). */
    int is_dotted_global = (ray_str_len(ns) > 0 && ray_str_ptr(ns)[0] == '.');
    /* IPC connection-hook slot? — a kdb `.z.p*` handler alias, OR the native
     * `.ipc.on.*` spelling, OR the `.z.ts` timer handler.  These must route
     * through the REGISTRY set wrapper (q_setg_wrap) so the alias-canonicalization
     * + q-lambda-carrier unwrap run — the env special-form `set` (ray_set_fn)
     * stores the carrier verbatim into a plain env binding, which the C callers
     * (ipc.c hook_lookup / the `.z.ts` timer thunk — both bare-lambda only, and a
     * different slot) never see.  q_ast_is_assign already flagged the pre-lower
     * `:` shape, so REPL/qdoc output stays suppressed after this head swap. */
    size_t nsl = ray_str_len(ns);
    const char* nsp = ray_str_ptr(ns);
    int is_hook_slot = (q_dotz_ipc_hook_index(nsp, nsl) >= 0) ||
                       ray_sym_is_ipc_hook(e[1]->i64) ||
                       (nsl == 5 && memcmp(nsp, ".z.ts", 5) == 0);  /* .z.ts timer slot */
    ray_release(ns);

    if (is_hook_slot) {
        ray_t *setv = q_registry_lookup_name("set", 3, Q_DYADIC);  /* q_setg_wrap; borrowed */
        if (setv) {                        /* cold registry -> fall through */
            ray_retain(setv);
            ray_release(e[0]);
            e[0] = setv;                   /* head = value-set wrapper (args eval'd) */
            e[1]->attrs |= Q_ATTR_QUOTED;  /* target sym self-evaluates to the handle */
            return NULL;
        }
    }

    const char *target = (is_local && in_lambda && !is_dotted_global)
                             ? "let" : "set";
    ray_t *setv = ql_env_val(target);
    if (!setv) return NULL;
    ray_retain(setv);
    ray_release(e[0]);
    e[0] = setv;                       /* in-place head swap, arity unchanged */
    return NULL;
}

/* ===== $[c;t;f] cond ========================================================
 * q Cond is a control construct spelled with the cast glyph: `$[test;et;ef]`
 * (ref/cond.md).  Detected by a `$`-provenance head with >= 3 args: the
 * parser's LBRACK arm embeds the DYADIC registry value (the cast wrapper) at
 * every `$[...]` head, so we probe provenance; a still-unresolved `$`
 * name-ref (cold-registry path) is matched by spelling.  A 2-arg `$[t;x]` is
 * bracket-call CAST and stays untouched. */
static int ql_is_dollar_head(ray_t *h) {
    if (!h) return 0;
    if (h->type == -RAY_SYM && !(h->attrs & Q_ATTR_QUOTED)) {
        ray_t *s = ray_sym_str(h->i64);
        int r = s && ray_str_len(s) == 1 && ray_str_ptr(s)[0] == '$';
        if (s) ray_release(s);
        return r;
    }
    if (h->type == RAY_BINARY) {
        q_provenance_t pv;
        return q_registry_provenance(h, &pv) && pv.spelling &&
               pv.spelling[0] == '$' && pv.spelling[1] == '\0';
    }
    return 0;
}

/* Lower `$[c1;t1;...;f]` onto rayfall `if` (env special form, ray_cond_fn:
 * lazy — branch args reach it UNEVALUATED — but TRIADIC ONLY: extra args are
 * ignored, so the kdb flattened multi-way `$[q;a;r;b;c]` <=> `$[q;a;$[r;b;c]]`
 * (ref/cond.md) is rebuilt as explicitly nested (if q a (if r b c)) lists,
 * folding pairs from the right.  Even arg counts have no kdb reading (the ref
 * defines triads + odd flattenings only) -> 'cond error.  Returns a RAY_ERROR
 * (owned) to abort lowering, else NULL. */
static ray_t *ql_cond(ray_t **slot) {
    ray_t *node = *slot;
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    if (n < 4 || !ql_is_dollar_head(e[0])) return NULL;   /* $[c;t;f] = 4 slots */
    ray_t *ifv = ql_env_val("if");
    if (!ifv) return NULL;                 /* borrowed; append retains below */
    /* Odd expr count (node len even) ends in a final ELSE (fold pairs before
     * it).  Even expr count (node len odd) has NO else: V3.6+ defaults to the
     * generic null (ref/cond.md "Even number of expressions returns either a
     * result or the generic null").  Seed acc / start index accordingly. */
    int64_t start;
    ray_t *acc;
    if (n % 2 == 0) { acc = e[n - 1]; ray_retain(acc); start = n - 3; }
    else            { acc = q_null(); start = n - 2; }   /* q_null: owned `::` */
    for (int64_t i = start; i >= 1; i -= 2) {
        ray_t *w = ray_list_new(4);
        w = ray_list_append(w, ifv);
        w = ray_list_append(w, e[i]);      /* cond_i  (append retains) */
        w = ray_list_append(w, e[i + 1]);  /* then_i */
        w = ray_list_append(w, acc);
        ray_release(acc);
        acc = w;
    }
    ray_release(node);
    *slot = acc;
    return NULL;
}
/* ===== `;` sequence + if/do/while control words ============================
 * The parser leaves a top-level statement sequence as a `;`-marker-headed list
 * (`(`;;s1;s2;…)`) and the control words `if`/`do`/`while` as plain name-ref
 * bracket applications (`(`if;test;e1;…)`).  All four are q SPECIAL FORMS: on
 * the EVAL path only, swap the marker/name head for the matching q-layer
 * special-form registry value so evaluation is lazy, left-to-right and side-
 * effecting (see q_{seq,if,do,while}_fn).  `parse` shows the pre-lower tree, so
 * its display is untouched.  Borrowed registry handout — retain before embed. */
static void ql_head_swap(ray_t **slot, ray_t *v) {
    ray_t *node = *slot;
    ray_t **e = (ray_t **)ray_data(node);
    ray_retain(v);
    ray_release(e[0]);
    e[0] = v;
}

/* `s1;s2;…` -> q.seq.  The `;`-marker head is produced only by seq_of (the
 * top-level program sequence), so a `;`-headed list is unambiguously it. */
static void ql_seq(ray_t **slot) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST || ray_len(node) < 1) return;
    ray_t *h = ((ray_t **)ray_data(node))[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return;
    int is_semi = (ray_str_len(s) == 1 && ray_str_ptr(s)[0] == ';');
    ray_release(s);
    if (!is_semi) return;
    ray_t *v = q_registry_seq_value();
    if (v) ql_head_swap(slot, v);
}

/* `if`/`do`/`while` bracket applications -> q.if/q.do/q.while.  These are q
 * reserved control words (never variable names), so a name-ref head spelled
 * exactly one of them is unambiguously the control construct. */
static void ql_control(ray_t **slot) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST || ray_len(node) < 1) return;
    ray_t *h = ((ray_t **)ray_data(node))[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    ray_t *v = NULL;
    if      (l == 2 && memcmp(nm, "if", 2) == 0)    v = q_registry_if_value();
    else if (l == 2 && memcmp(nm, "do", 2) == 0)    v = q_registry_do_value();
    else if (l == 5 && memcmp(nm, "while", 5) == 0) v = q_registry_while_value();
    ray_release(s);
    if (v) ql_head_swap(slot, v);
}

/* Modified (compound) assignment `x op: y` == `x: x op y` (amend by value,
 * basics/control.md / ref/amend.md).  The parser emits `(op:;`x;y)` with the
 * head a name-ref sym whose spelling is a dyadic verb glyph followed by `:`.
 * Lower to `(set/let `x (op `x y))` — the read-ref `x` resolves the CURRENT
 * value, and the enclosing set/let returns the NEW value (q returns it;
 * `while[x-:1;…]` relies on it).  Only fires for a sym target and a
 * registry-resolvable dyadic op; anything else is left for eval to reject.
 * BORROWED registry/env handouts (append retains; set-head retained). */
static void ql_mod_assign(ray_t **slot, int in_lambda) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST || ray_len(node) != 3) return;
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    /* `<op>:` with a non-empty op that is NOT the plain assign colons */
    if (l < 2 || nm[l - 1] != ':' || nm[0] == ':') { ray_release(s); return; }
    /* target must be a bare (unquoted) sym name-ref */
    if (!e[1] || e[1]->type != -RAY_SYM || (e[1]->attrs & Q_ATTR_QUOTED)) {
        ray_release(s); return;
    }
    ray_t *opv = ql_reg_fnval(nm, l - 1);   /* borrowed */
    /* q.q-hosted op (carrier cell): the inner head becomes a name-ref sym */
    int64_t opsid = (!opv && q_registry_lookup_name(nm, l - 1, Q_DYADIC))
                    ? ray_sym_intern(nm, l - 1) : 0;
    ray_release(s);
    if (!opv && !opsid) return;             /* unknown op -> eval errors as today */
    /* dotted targets (`.foo.x+:1`) are global namespace amends, never locals */
    int mod_dotted = 0;
    {
        ray_t *ts = ray_sym_str(e[1]->i64);
        if (ts) {
            mod_dotted = (ray_str_len(ts) > 0 && ray_str_ptr(ts)[0] == '.');
            ray_release(ts);
        }
    }
    ray_t *setv = ql_env_val((in_lambda && !mod_dotted) ? "let" : "set"); /* borrowed */
    if (!setv) return;
    /* inner (op `x y): op on x's CURRENT value and y (append retains each) */
    ray_t *inner = ray_list_new(3);
    if (opsid) {
        ray_t *opsym = ray_sym(opsid);      /* owned name-ref */
        inner = ray_list_append(inner, opsym);
        ray_release(opsym);
    } else {
        inner = ray_list_append(inner, opv);
    }
    inner = ray_list_append(inner, e[1]);
    inner = ray_list_append(inner, e[2]);
    /* splice node in place -> (set/let `x inner) */
    ray_retain(setv);
    ray_release(e[0]); e[0] = setv;         /* head swap, arity unchanged */
    ray_release(e[2]); e[2] = inner;        /* value slot becomes the op subtree */
    /* e[1] target stays: node keeps its ref, inner holds its own retained ref */
}

/* Indexed assignment `name[i1;…;ik]: v` / `name[i]op:v` — q amend-in-place
 * (ref/amend.md).  Parse shape: `(:; (nameref i1..ik); v)` (or `(op:; …; v)`
 * — ql_mod_assign passed it through, its target not being a bare sym).
 * Lower to
 *   k==1: (set/let `name (@ nameref i1  f v))
 *   k>=2: (set/let `name (. nameref (list i1..ik) f v))
 * where @/. are the registry VARY amend wrappers, and f is the QUOTED `:`
 * sym for plain assign (the spelling q_is_assign detects) or the dyadic op
 * value for a compound `op:`.  The read-ref `nameref` resolves the CURRENT
 * value; set/let returns the amended whole (kdb returns the assigned VALUE —
 * accepted divergence, same as plain-assign today).  Fires only for an
 * identifier-headed application target with no elision holes; anything else
 * is left for eval to reject exactly as today.  kdb note: index assignment
 * never CREATES a local — with only in_lambda visible here we mirror
 * ql_mod_assign (let inside a lambda unless dotted); amending a GLOBAL by
 * index from inside a lambda is a logged known gap (PLAN.md). */
static void ql_indexed_assign(ray_t **slot, int in_lambda) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST || ray_len(node) != 3) return;
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int is_global = (l == 2 && nm[0] == ':' && nm[1] == ':');
    int plain = (l == 1 && nm[0] == ':') || is_global;
    int comp  = (l >= 2 && nm[l - 1] == ':' && nm[0] != ':');
    if (!plain && !comp) { ray_release(s); return; }

    ray_t *tgt = e[1];
    if (!tgt || tgt->type != RAY_LIST || ray_len(tgt) < 2) { ray_release(s); return; }
    ray_t **te = (ray_t **)ray_data(tgt);
    ray_t *nmref = te[0];
    if (!nmref || nmref->type != -RAY_SYM || (nmref->attrs & Q_ATTR_QUOTED)) {
        ray_release(s); return;
    }
    ray_t *ns = ray_sym_str(nmref->i64);
    if (!ns) { ray_release(s); return; }
    const char *tn = ray_str_ptr(ns);
    size_t tl = ray_str_len(ns);
    /* identifier heads only (a paren-literal / verb-value head is not a sym,
     * but markers like `{` are — require an identifier first char) */
    int ident = tl > 0 && ((tn[0] >= 'a' && tn[0] <= 'z') ||
                           (tn[0] >= 'A' && tn[0] <= 'Z') || tn[0] == '.');
    int dotted = tl > 0 && tn[0] == '.';
    ray_release(ns);
    if (!ident) { ray_release(s); return; }

    int64_t k = ray_len(tgt) - 1;
    for (int64_t i = 1; i <= k; i++)
        if (ql_is_hole(te[i])) { ray_release(s); return; }  /* d[;`b]:v deferred */

    ray_t *fval = NULL;                       /* borrowed registry value (op:) */
    int64_t fsid = 0;       /* q.q-hosted op: f seat gets a name-ref sym */
    if (comp) {
        fval = ql_reg_fnval(nm, l - 1);
        if (!fval && q_registry_lookup_name(nm, l - 1, Q_DYADIC))
            fsid = ray_sym_intern(nm, l - 1);
        if (!fval && !fsid) { ray_release(s); return; }
    }
    ray_release(s);
    ray_t *amend = q_registry_lookup_name(k == 1 ? "@" : ".", 1, Q_DYADIC);
    if (!amend) return;                       /* cold registry: leave untouched */
    /* `::` is an explicit GLOBAL assign even inside a lambda (mirrors ql_assign) */
    ray_t *setv = ql_env_val((in_lambda && !dotted && !is_global) ? "let" : "set");
    if (!setv) return;

    /* inner amend call (appends retain; te[] stay alive through it) */
    ray_t *inner = ray_list_new(5);
    inner = ray_list_append(inner, amend);
    inner = ray_list_append(inner, nmref);
    if (k == 1) {
        inner = ray_list_append(inner, te[1]);
    } else {
        ray_t *lc = q_registry_list_value();  /* borrowed paren-literal head */
        if (!lc) { ray_release(inner); return; }
        ray_t *lst = ray_list_new(k + 1);
        lst = ray_list_append(lst, lc);
        for (int64_t i = 1; i <= k; i++) lst = ray_list_append(lst, te[i]);
        inner = ray_list_append(inner, lst);
        ray_release(lst);
    }
    if (comp && fsid) {
        ray_t *fsym = ray_sym(fsid);          /* owned name-ref */
        inner = ray_list_append(inner, fsym);
        ray_release(fsym);
    } else if (comp) {
        inner = ray_list_append(inner, fval);
    } else {
        ray_t *colon = ray_sym(ray_sym_intern_runtime(":", 1));
        if (!colon) { ray_release(inner); return; }
        colon->attrs |= Q_ATTR_QUOTED;        /* self-evals to `: for q_is_assign */
        inner = ray_list_append(inner, colon);
        ray_release(colon);
    }
    inner = ray_list_append(inner, e[2]);

    /* splice in place: (set/let; nameref; inner) — build-first, then swap */
    ray_retain(setv);
    ray_release(e[0]); e[0] = setv;
    ray_retain(nmref);
    ray_release(e[1]); e[1] = nmref;          /* tgt dies; inner holds its own refs */
    ray_release(e[2]); e[2] = inner;
}

/* True iff node is a ctx-literal — a paren list `(…)` or table `([]…)` whose
 * head is the shared context-constructor value.  Its element statements run in
 * a pushed env scope (q_ctx_build), so a plain `:` inside must lower to `let`
 * (bind the scoped frame) exactly like a lambda body — the mandate's "bind INTO
 * the scope, pop back OUT". */
static int ql_is_ctx_literal(ray_t *node) {
    if (!node || node->type != RAY_LIST || ray_len(node) < 1) return 0;
    ray_t *h = ((ray_t **)ray_data(node))[0];
    if (!h) return 0;
    return h == q_registry_list_value() || h == q_registry_table_value() ||
           h == q_registry_keyed_table_value();}

/* True iff node is the `{`-marker lambda form (its body statements bind q
 * locals with plain `:`). */
static int ql_is_lambda(ray_t *node) {
    if (!node || node->type != RAY_LIST || ray_len(node) < 1) return 0;
    ray_t *h = ((ray_t **)ray_data(node))[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return 0;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return 0;
    int r = (ray_str_len(s) == 1 && ray_str_ptr(s)[0] == '{');
    ray_release(s);
    return r;
}

/* Lambda literal: swap the `{`-marker head for the registry q.fn value — a
 * special form that builds the RAY_LAMBDA (via base `fn`) and wraps it in
 * the 100h .q.lambda carrier.  Runs AFTER the children walk, so body-local
 * `:` assignments are already `let`.  Registry handout is BORROWED — retain
 * before embedding.  Cold registry: the marker stays (pre-wave-1 behaviour). */
static void ql_lambda(ray_t **slot) {
    ray_t *node = *slot;
    if (!ql_is_lambda(node)) return;
    ray_t *v = q_registry_lambda_value();
    if (!v) return;
    ray_t **e = (ray_t **)ray_data(node);
    ray_retain(v);
    ray_release(e[0]);
    e[0] = v;
}

/* Early-return / signal statements: swap the parser's `.q.ret` / `.q.sig`
 * marker heads for their registry values (same borrowed-handout discipline
 * as ql_lambda). */
static void ql_ret_sig(ray_t **slot) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST || ray_len(node) != 2) return;
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return;
    ray_t *v = NULL;
    if (ray_str_len(s) == 6 && memcmp(ray_str_ptr(s), ".q.ret", 6) == 0)
        v = q_registry_ret_value();
    else if (ray_str_len(s) == 6 && memcmp(ray_str_ptr(s), ".q.sig", 6) == 0)
        v = q_registry_sig_value();
    ray_release(s);
    if (!v) return;
    ray_retain(v);
    ray_release(e[0]);
    e[0] = v;
}

/* ===== qSQL SELECT lowering (piece 3 executor) ==============================
 * The parser keeps `select … from t` / `?[t;c;b;a]` as the SYMBOLIC functional
 * 5-list (?;`t;c;b;a) so `parse` prints it kdb-true.  On the EVAL path only,
 * q_lower rewrites that 5-list onto the base ray_select engine:
 *
 *   (?;`t;c;b;a)  ->  (q.select; {from:t [where:…] [by:…] out:expr …}; keycols)
 *
 * The q column-symbol references (`a) are turned into bare rayfall column
 * name-refs (a) that ray_select resolves in column scope.  See q_select_exec
 * (q_registry.c) for the runtime side (ray_select + by-group keyed split). */

/* Deep-clear the QUOTED bit on every -RAY_SYM atom in a clause expr, so a q
 * column reference `a becomes a bare column name-ref a.  A genuine `sym literal
 * survives as an (enlisted) RAY_SYM VECTOR, which this leaves untouched. */
static void ql_unquote_cols(ray_t *x) {
    if (!x) return;
    if (x->type == -RAY_SYM) { x->attrs &= ~Q_ATTR_QUOTED; return; }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t **e = (ray_t **)ray_data(x);
        for (int64_t i = 0; i < n; i++) ql_unquote_cols(e[i]);
    }
}

/* Normalize a qSQL OUTPUT-column expr: unquote column refs AND turn an
 * embedded function-VALUE head back into a bare name-ref sym.  ray_select's
 * aggregate detection (is_agg_expr, query.c) keys on a -RAY_SYM head, so
 * `sum a` — which the parser embeds as (sum<value>;`a) — must become
 * (sum;a) or it evaluates element-wise (returning the column, not the sum).
 *
 * Stage-1 general eval (2026-07-11): the conversion is now CONDITIONAL — a
 * head whose routing name the base env cannot resolve (q wrappers like
 * sums/prds/deltas, spelling-less values) KEEPS the VALUE.  Converting those
 * to name syms manufactured a guaranteed 'name at eval; a preserved value
 * head is exactly what ray_select's general-eval recognizer keys on, and
 * compile_expr_dag/ray_eval both apply value heads natively. */
static void ql_qsql_out(ray_t *x) {
    if (!x) return;
    if (x->type == -RAY_SYM) { x->attrs &= ~Q_ATTR_QUOTED; return; }
    if (x->type != RAY_LIST) return;
    int64_t n = ray_len(x);
    ray_t **e = (ray_t **)ray_data(x);
    if (n >= 1 && e[0] && (e[0]->type == RAY_UNARY || e[0]->type == RAY_BINARY ||
                           e[0]->type == RAY_VARY)) {
        const char *nm = ray_fn_name(e[0]);
        if (nm && nm[0]) {
            int64_t id = ray_sym_intern_runtime(nm, strlen(nm));
            if (ray_env_get(id)) {              /* engine can name-dispatch it */
                ray_t *s = ray_sym(id);
                if (s && !RAY_IS_ERR(s)) { ray_release(e[0]); e[0] = s; }
                else if (s) ray_release(s);
            }                                   /* else: keep the VALUE head */
        }
    }
    for (int64_t i = 0; i < n; i++) ql_qsql_out(e[i]);
}

static ray_t *q_lower_walk(ray_t **slot, int in_lambda, int is_head);

/* Lower a phrase subtree that q_lower_walk skipped: name!expr DICT values are
 * not walked (the walker recurses only RAY_LIST children), so select-phrase
 * expressions keep their raw parse heads (`{` lambda markers, control markers)
 * unless lowered here.  Mirrors the loop in ql_qsql_exec.  Operates on the
 * dict's INTERNAL slot (rc==1) — never on a retained copy (the walker asserts
 * sole ownership). */
static void qsql_lower_phrase(ray_t **slot) {
    if (*slot && (*slot)->type == RAY_LIST && (*slot)->rc == 1 &&
        q_deriv_kind_of(*slot) == Q_DERIV_NONE) {
        ray_t *err = q_lower_walk(slot, 0, 0);
        if (err) ray_release(err);      /* an 'assign in a phrase expr: drop */
    }
}

/* An application whose HEAD is an embedded lambda/derived-verb CARRIER (a
 * runtime VALUE) or a bare RAY_LAMBDA would be re-walked by ray_eval's
 * head-eval and die 'name on the internal marker sym.  Wrap such heads as
 * (quote <carrier>): `quote` is a base special form, so head-eval returns the
 * carrier itself and eval's noun-head arm dispatches it through the q apply
 * hook.  Narrow by design (codex plan-review item 7): ordinary value/sym
 * heads are never wrapped.  Mutates only the OWNED application node — the
 * carrier itself is untouched (it may be rc-shared with the env). */
static void qsql_quote_carrier_heads(ray_t *x) {
    if (!x || x->type != RAY_LIST || q_deriv_kind_of(x) != Q_DERIV_NONE) return;
    int64_t n = ray_len(x);
    ray_t **e = (ray_t **)ray_data(x);
    /* A name-ref head with NO env binding whose value is q.q-hosted (a
     * QK_QSRC registry cell or a bare `.q` keyword — deltas, trim, cov, …):
     * ray_select can neither name-dispatch nor general-eval it, so swap in
     * the carrier value; the wrap below quotes it for the q apply hook. */
    if (n >= 2 && e[0] && e[0]->type == -RAY_SYM &&
        !(e[0]->attrs & Q_ATTR_QUOTED) && !ray_env_get(e[0]->i64)) {
        ray_t *s = ray_sym_str(e[0]->i64);
        if (s) {
            const char *nm = ray_str_ptr(s);
            size_t l = ray_str_len(s);
            ray_t *v = q_registry_lookup_name(nm, l,
                                              n == 2 ? Q_MONADIC : Q_DYADIC);
            if (!v) v = q_ns_dotq_get(nm, l);          /* borrowed or NULL */
            ray_release(s);
            if (v && ((v->type == RAY_LIST && q_deriv_kind_of(v) != Q_DERIV_NONE) ||
                      v->type == RAY_LAMBDA)) {
                ray_retain(v);
                ray_release(e[0]);
                e[0] = v;
            }
        }
    }
    if (n >= 2 && e[0] &&
        ((e[0]->type == RAY_LIST && q_deriv_kind_of(e[0]) != Q_DERIV_NONE) ||
         e[0]->type == RAY_LAMBDA)) {
        ray_t *q = q_marker("quote");
        ray_t *wrap = ray_list_new(2);
        wrap = ray_list_append(wrap, q);    ray_release(q);
        wrap = ray_list_append(wrap, e[0]);            /* retains carrier */
        ray_release(e[0]);                             /* drop old slot ref */
        e[0] = wrap;                                   /* own the wrapper */
    }
    if (e[0] && e[0]->type == RAY_LIST && q_deriv_kind_of(e[0]) == Q_DERIV_NONE)
        qsql_quote_carrier_heads(e[0]);                /* computed heads nest */
    for (int64_t i = 1; i < n; i++) qsql_quote_carrier_heads(e[i]);
}

/* Run the two passes above over every phrase expression a Select tree embeds:
 * the where-constraint list (slot 2), a by-DICT's values (slot 3) and the
 * output DICT's values (slot 4).  All three are dict/list INTERNALS the
 * walker skipped; lowering must happen before ql_qsql retains copies. */
static void qsql_lower_select_phrases(ray_t *c, ray_t *b, ray_t *a) {
    if (c && c->type == RAY_LIST && ray_len(c) == 1) {
        ray_t *clist = ((ray_t **)ray_data(c))[0];
        if (clist && clist->type == RAY_LIST) {
            int64_t nc = ray_len(clist);
            ray_t **ce = (ray_t **)ray_data(clist);
            for (int64_t i = 0; i < nc; i++) {
                if (ce[i] && ce[i]->type == RAY_LIST && ce[i]->rc == 1)
                    qsql_lower_phrase(&ce[i]);
                if (ce[i] && ce[i]->rc == 1) qsql_quote_carrier_heads(ce[i]);
            }
        }
    }
    ray_t *dicts[2] = { b, a };
    for (int di = 0; di < 2; di++) {
        ray_t *d = dicts[di];
        if (!d || d->type != RAY_DICT) continue;
        ray_t *dv = ray_dict_vals(d);              /* borrowed internal list */
        if (!dv || dv->type != RAY_LIST) continue;
        int64_t nv = ray_len(dv);
        ray_t **ve = (ray_t **)ray_data(dv);
        for (int64_t i = 0; i < nv; i++) {
            if (ve[i] && ve[i]->type == RAY_LIST && ve[i]->rc == 1)
                qsql_lower_phrase(&ve[i]);
            if (ve[i] && ve[i]->rc == 1) qsql_quote_carrier_heads(ve[i]);
        }
    }
}

/* A bare `&` dyadic application (rayfall min / logical-and) combining two
 * boolean where-masks.  Consumes neither operand's ownership (retained on
 * append); returns owned. */
static ray_t *ql_and(ray_t *l, ray_t *r) {
    ray_t *amp = q_verb('&');
    amp = q_embed(amp, Q_DYADIC);
    ray_t *n = ray_list_new(3);
    n = ray_list_append(n, amp); ray_release(amp);
    n = ray_list_append(n, l);
    n = ray_list_append(n, r);
    return n;
}

static void ql_qsql(ray_t **slot) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST || ray_len(node) != 5) return;
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return;
    ray_t *hs = ray_sym_str(h->i64);
    int is_q = hs && ray_str_len(hs) == 1 && ray_str_ptr(hs)[0] == '?';
    if (hs) ray_release(hs);
    if (!is_q) return;
    ray_t *sv = q_registry_select_value();
    if (!sv) return;

    ray_t *t = e[1], *c = e[2], *b = e[3], *a = e[4];
    if (!t) return;
    /* slot 1: bare name sym (by-name semantics) or an expression subtree —
     * q_lower_walk already lowered it (children walk before the node). */

    /* Stage-1 general eval: lower the phrase subtrees the walker skipped
     * (dict values / where-constraint list) and quote-wrap embedded carrier
     * heads so the trees handed to ray_select are ray_eval-able. */
    qsql_lower_select_phrases(c, b, a);

    ray_t *keyvec  = ray_sym_vec_new(RAY_SYM_W64, 4);
    ray_t *vallist = ray_list_new(4);
    ray_t *keycols = ray_sym_vec_new(RAY_SYM_W64, 1);   /* by-key col names */

    /* NB `from:` is appended LAST: the base ray_select mis-handles a computed
     * output column (`sum a`) when `from:` is the FIRST dict key. */

    /* where: AND the constraints (right-to-left fold), cols unquoted */
    if (c && c->type == RAY_LIST && ray_len(c) == 1) {
        ray_t *clist = ((ray_t **)ray_data(c))[0];
        if (clist && clist->type == RAY_LIST && ray_len(clist) >= 1) {
            int64_t ncon = ray_len(clist);
            ray_t **cons = (ray_t **)ray_data(clist);
            ray_t *acc = NULL;
            for (int64_t i = ncon - 1; i >= 0; i--) {
                ray_t *cc = cons[i];
                if (!cc) continue;
                ray_retain(cc);
                ql_unquote_cols(cc);
                if (!acc) { acc = cc; }
                else { ray_t *nx = ql_and(cc, acc); ray_release(cc); ray_release(acc); acc = nx; }
            }
            if (acc) {
                keyvec  = q_symvec_append(keyvec, "where", 5);
                vallist = ray_list_append(vallist, acc); ray_release(acc);
            }
        }
    }

    /* by: single/multi grouping column(s); record their names as key cols.
     * Single key -> the bare col-ref / expr (base ray_select single-key form).
     * Multi key  -> a name!expr DICT (base by-dict, query.c:4926); a bare list
     * is rejected with 'domain. */
    if (b && b->type == RAY_DICT) {
        ray_t *bk = ray_dict_keys(b);       /* names */
        ray_t *bv = ray_dict_vals(b);       /* sym vec or expr list */
        int64_t nb = ray_len(bk);
        int bv_sym = (bv && bv->type == RAY_SYM);
        ray_t *byexpr = NULL;               /* single-key: bare col/expr */
        ray_t *bynames = NULL, *byvals = NULL;   /* multi-key: name!expr dict */
        for (int64_t i = 0; i < nb; i++) {
            ray_t *kn = ray_sym_vec_cell(bk, i);        /* borrowed -RAY_STR */
            keycols = q_symvec_append(keycols, ray_str_ptr(kn), (int)ray_str_len(kn));
            ray_t *ex;
            if (bv_sym) {
                ray_t *vn = ray_sym_vec_cell(bv, i);
                ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
            } else {
                ex = ((ray_t **)ray_data(bv))[i];
                ray_retain(ex);
                ql_unquote_cols(ex);
            }
            if (nb == 1) { byexpr = ex; }
            else {
                if (!bynames) { bynames = ray_sym_vec_new(RAY_SYM_W64, nb); byvals = ray_list_new(nb); }
                bynames = q_symvec_append(bynames, ray_str_ptr(kn), (int)ray_str_len(kn));
                byvals  = ray_list_append(byvals, ex); ray_release(ex);
            }
        }
        ray_t *byval = byexpr ? byexpr
                              : (bynames ? ray_dict_new(bynames, byvals) : NULL);
        if (byval) {
            keyvec  = q_symvec_append(keyvec, "by", 2);
            vallist = ray_list_append(vallist, byval); ray_release(byval);
        }
    }

    /* output columns: name -> expr.  A computed output column whose q name
     * equals a source column (`select sum a` -> col `a`) breaks the base
     * engine's `name: (expr name)` collision handling, so each output column
     * goes into ray_select under a UNIQUE temp name `QqcN` and q_select_exec
     * renames the result columns back to the real q names (tempnames/outnames
     * pair).  Order-independent (rename matches by temp name).  NB the temp
     * prefix must NOT start with `_` — the base engine treats leading-underscore
     * column names as reserved/hidden and drops them from the output. */
    ray_t *tempnames = ray_sym_vec_new(RAY_SYM_W64, 1);
    ray_t *outnames  = ray_sym_vec_new(RAY_SYM_W64, 1);
    if (a && a->type == RAY_DICT) {
        ray_t *ak = ray_dict_keys(a);
        ray_t *av = ray_dict_vals(a);
        int64_t na = ray_len(ak);
        int av_sym = (av && av->type == RAY_SYM);
        for (int64_t i = 0; i < na; i++) {
            char tmp[24];
            int tl = snprintf(tmp, sizeof tmp, "Qqc%lld", (long long)i);
            keyvec    = q_symvec_append(keyvec, tmp, tl);
            tempnames = q_symvec_append(tempnames, tmp, tl);
            ray_t *nm = ray_sym_vec_cell(ak, i);
            outnames  = q_symvec_append(outnames, ray_str_ptr(nm), (int)ray_str_len(nm));
            ray_t *ex;
            if (av_sym) {
                ray_t *vn = ray_sym_vec_cell(av, i);
                ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
            } else {
                ex = ((ray_t **)ray_data(av))[i];
                ray_retain(ex);
                ql_qsql_out(ex);
            }
            if (ex && ex->type == -RAY_SYM && !(ex->attrs & Q_ATTR_QUOTED) &&
                q_symvec_contains_id(keycols, ex->i64)) {
                /* A by-group output that is a BARE reference to a group-key
                 * column trips the base engine's key/value collision handling
                 * (it drops the column).  Disguise it as a computed column via
                 * a TYPE-AGNOSTIC identity — reverse reverse x — which yields x
                 * for any column type (sym/char/temporal/nested list), unlike
                 * an arithmetic x+0 that type-errors on non-numeric columns.
                 * q_select_exec renames the temp output column back afterwards. */
                for (int rr = 0; rr < 2; rr++) {
                    ray_t *rev = q_verb_name("reverse", 7);
                    rev = q_embed(rev, Q_MONADIC);
                    ray_t *wrap = ray_list_new(2);
                    wrap = ray_list_append(wrap, rev); ray_release(rev);
                    wrap = ray_list_append(wrap, ex);  ray_release(ex);
                    ex = wrap;
                }
            }
            vallist = ray_list_append(vallist, ex); ray_release(ex);
        }
    }

    /* from: — appended LAST (see note above).  ANY sym slot unquotes to a
     * bare name-ref (env lookup): the parser's name form emits a QUOTED `t
     * (qsql_colsym), and a `sym from-EXPRESSION degrades to the same by-name
     * lookup — kdb's by-name semantics (funsql.md: t "may be a table or the
     * name of a table"; needed later for partitioned tables).  Any other
     * slot-1 value is an already-lowered expression subtree embedded as-is —
     * ray_select ray_eval()s the from: value (query.c), so a table value
     * flows in. */
    ray_t *fromv = (t->type == -RAY_SYM) ? ray_sym(t->i64)
                                         : (ray_retain(t), t);
    keyvec  = q_symvec_append(keyvec, "from", 4);
    vallist = ray_list_append(vallist, fromv); ray_release(fromv);

    ray_t *dict = ray_dict_new(keyvec, vallist);   /* consumes keyvec, vallist */
    if (!dict || RAY_IS_ERR(dict)) {
        if (dict) ray_release(dict);
        ray_release(keycols); ray_release(tempnames); ray_release(outnames);
        return;
    }

    ray_t *repl = ray_list_new(5);
    repl = ray_list_append(repl, sv);                  /* borrowed -> retained */
    repl = ray_list_append(repl, dict);      ray_release(dict);
    repl = ray_list_append(repl, keycols);   ray_release(keycols);
    repl = ray_list_append(repl, tempnames); ray_release(tempnames);
    repl = ray_list_append(repl, outnames);  ray_release(outnames);
    ray_release(node);
    *slot = repl;
}

/* String `delete` lowering: the symbolic (!;`t;c;0b;a) statement tree (bare
 * unquoted `!` head, 5-list) head-swaps onto the q.delete special form, whose
 * executor drives q_funsql_bang_impl.  A functional ![t;c;b;a] (fn-VALUE head)
 * is handled by ql_funsql; a dict-make k!v is a 3-list — neither matches. */
static void ql_qsql_bang(ray_t **slot) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST || ray_len(node) != 5) return;
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return;
    ray_t *hs = ray_sym_str(h->i64);
    int is_bang = hs && ray_str_len(hs) == 1 && ray_str_ptr(hs)[0] == '!';
    if (hs) ray_release(hs);
    if (!is_bang) return;
    ray_t *dv = q_registry_delete_value();
    if (!dv) return;
    ray_retain(dv);
    ray_release(e[0]);
    e[0] = dv;   /* (q.delete; t; c; b; a) — args e[1..4] pass through */
}

static ray_t *q_lower_walk(ray_t **slot, int in_lambda, int is_head);

/* String `exec` lowering: the symbolic (?;`t;c;b;a) statement tree (bare
 * unquoted `?` head, 5-list) head-swaps onto the q.exec special form, whose
 * executor drives q_funsql_select_impl (the SAME result-shaping engine as the
 * functional `?[t;c;b;a]` exec) — string and functional exec stay equivalent.
 *
 * Select and exec SHARE the `?` head, so the By-phrase slot (e[3]) disambiguates
 * exactly as kdb encodes it: exec uses the general empty list `()` (no grouping)
 * or a bare/vector SYMBOL By (grouped exec); Select uses `0b` (RAY_BOOL) or a
 * name!expr DICT.  Claim only the exec encodings here (runs BEFORE ql_qsql), so
 * Select trees fall through to ql_qsql unchanged, and a `by k:expr` exec (a DICT
 * By) intentionally degrades to the Select path (keyed-table result). */
static void ql_qsql_exec(ray_t **slot) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST || ray_len(node) != 5) return;
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return;
    ray_t *hs = ray_sym_str(h->i64);
    int is_q = hs && ray_str_len(hs) == 1 && ray_str_ptr(hs)[0] == '?';
    if (hs) ray_release(hs);
    if (!is_q) return;
    ray_t *b = e[3];
    int is_exec = b && ((b->type == RAY_LIST && ray_len(b) == 0) ||   /* () no-grouping */
                        b->type == -RAY_SYM || b->type == RAY_SYM);   /* symbol By */
    if (!is_exec) return;
    ray_t *xv = q_registry_exec_value();
    if (!xv) return;

    /* Lower the Select-phrase DICT's value expressions.  A name!expr `a` dict is
     * SKIPPED by q_lower_walk (it recurses only RAY_LIST children), so its output
     * expressions keep their raw parse-tree operator heads — a bare-list `a`
     * (single unnamed column) is walked and works, but a dict escapes.  The exec
     * executor's funsql_eval needs the lowered (fn-VALUE / carrier) heads, so run
     * the standard walker over each dict value here — exactly the lowering the
     * bare-list case gets, keeping the string form identical to the functional
     * `?[t;c;b;a]` exec.  (Select instead re-normalizes these via ql_qsql_out for
     * ray_select; the two engines want opposite head encodings.) */
    ray_t *a = e[4];
    if (a && a->type == RAY_DICT) {
        ray_t *av = ray_dict_vals(a);              /* borrowed internal list */
        if (av && av->type == RAY_LIST) {
            int64_t nv = ray_len(av);
            ray_t **ve = (ray_t **)ray_data(av);
            for (int64_t i = 0; i < nv; i++)
                if (ve[i] && ve[i]->type == RAY_LIST && ve[i]->rc == 1) {
                    ray_t *err = q_lower_walk(&ve[i], 0, 0);
                    if (err) ray_release(err);     /* an 'assign in an output expr: drop */
                }
        }
    }
    ray_retain(xv);
    ray_release(e[0]);
    e[0] = xv;   /* (q.exec; t; c; b; a) — args e[1..4] pass through */
}

/* ===== functional qSQL `?[t;c;b;a]` / `![t;c;b;a]` =========================
 * The by-VALUE functional forms parse as a rank-4 application whose head is the
 * DYADIC registry value for `?` (select/exec) or `!` (update/delete).  Rewrite
 * that head to the matching runtime executor value (q_registry_funsql_*), which
 * receives the four EVALUATED operands and drives the base query engine.  A
 * dict-make `k!v` (a `!` applied to only 2 args) is a 3-list and never matches.
 * The result-shaping (table / keyed table / row+column delete) lives in the
 * executors — see q_registry.c q_funsql_select / q_funsql_bang. */
static ray_t *ql_funsql_head(ray_t *h, char glyph) {
    if (!h || (h->type != RAY_UNARY && h->type != RAY_BINARY && h->type != RAY_VARY))
        return NULL;
    q_provenance_t pv;
    if (!q_registry_provenance(h, &pv) || !pv.spelling) return NULL;
    if (pv.spelling[0] != glyph || pv.spelling[1] != '\0') return NULL;
    return h;   /* borrowed */
}

static void ql_funsql(ray_t **slot) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST || ray_len(node) != 5) return;  /* head + 4 args */
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *repl_head = NULL;
    if (ql_funsql_head(e[0], '?'))      repl_head = q_registry_funsql_select_value();
    else if (ql_funsql_head(e[0], '!')) repl_head = q_registry_funsql_bang_value();
    if (!repl_head) return;
    ray_retain(repl_head);
    ray_release(e[0]);
    e[0] = repl_head;   /* args e[1..4] stay; eval evaluates them then applies */
}

/* Depth-first rewriting walker.  Children first so nested applications
 * ((+/) each ...) lower inside-out; then the node itself.  Returns a
 * RAY_ERROR (owned) on an 'assign violation, else NULL. */
static ray_t *q_lower_walk(ray_t **slot, int in_lambda, int is_head) {
    ray_t *node = *slot;
    if (!node || node->type != RAY_LIST) return NULL;
    assert(node->rc == 1);                 /* sole-owner precondition */
    int lambda_body = in_lambda || ql_is_lambda(node) || ql_is_ctx_literal(node);
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    for (int64_t i = 0; i < n; i++)
        if (e[i] && e[i]->type == RAY_LIST &&
            q_deriv_kind_of(e[i]) == Q_DERIV_NONE) {
            /* Skip an already-lowered derived-verb / lambda CARRIER: it is an
             * embedded runtime VALUE (e.g. a named q `{…}` fetched from the env
             * for a qSQL phrase head), not a parse subtree to lower — and being
             * shared it has rc>1, so recursing would trip the rc==1 assert. */
            ray_t *err = q_lower_walk(&e[i], lambda_body, i == 0);
            if (err) return err;
        }
    ray_t *err = ql_cond(slot);            /* $[c;t;f] BEFORE any head claim */
    if (err) return err;
    ql_seq(slot);                          /* `;` statement sequence -> q.seq */
    ql_control(slot);                      /* if/do/while -> q.if/q.do/q.while */
    ql_mod_assign(slot, in_lambda);        /* x op: y -> x: x op y (before others) */
    ql_indexed_assign(slot, in_lambda);    /* d[k]:v / d[k]op:v -> set + @/. amend */
    ql_qsql_exec(slot);                    /* (?;`t;c;();a) -> q.exec call (BEFORE ql_qsql) */
    ql_qsql(slot);                         /* (?;`t;c;b;a) -> ray_select call */
    ql_qsql_bang(slot);                    /* (!;`t;c;0b;a) -> q.delete call  */
    ql_funsql(slot);                       /* ?[t;c;b;a] / ![t;c;b;a] runtime */
    ql_dyad_head(slot);                    /* keyword-dyadic bracket calls */
    err = ql_assign(slot, in_lambda);
    if (err) return err;
    ql_lambda(slot);
    ql_ret_sig(slot);
    ql_project(slot);                      /* native/value projection (1+, 2*, +[1;]) */
    ql_adv_app(slot);
    /* head position stays a raw (adv;V) 2-list — the PARENT's ql_adv_app
     * consumes it; everywhere else a bare derived verb becomes a carrier. */
    if (!is_head) ql_deriv_value(slot);
    return NULL;
}

/* q-lower entrypoint — see q_parse.h.  Dyadic-head resolution (the 2a shim
 * behaviour) plus the lowering walker above.  2b's parser flip moves the head
 * resolution into q_parse (the 2b flip).  May return a
 * RAY_ERROR (consuming `ast`): callers must error-check the result. */
/* ===== context qualification (q namespaces, ADR: 2026-07-08 spec) ==========
 * When the `\d` context is not root, every UNQUALIFIED user name-ref in the
 * pre-lower tree is rewritten in place to `.ctx.name` — reads and assignment
 * targets alike.  Because lowering runs at DEFINITION time for lambda bodies,
 * this rewrite IS q's definition-time binding rule (q4m3 §12.7: an unqualified
 * global in a function binds to the context in effect at definition).
 *
 * What is NOT rewritten:
 *   - quoted (data) syms and non-identifier names (verb glyphs, markers),
 *   - q reserved words / control words / BUILTIN env bindings
 *     (q_ns_is_unqualifiable — user globals only),
 *   - lambda locals (params + plain-`:`/`op:` targets) and ctx-literal
 *     element locals — collected per scope below,
 *   - dotted names (already qualified).
 * A scope with more than QNS_SCOPE_CAP distinct locals qualifies NOTHING in
 * its subtree (conservative: preserves the env-cascade behaviour rather than
 * turning spilled locals into 'name errors). */
#define QNS_SCOPE_CAP 64

typedef struct qns_scope {
    struct qns_scope *up;
    int64_t names[QNS_SCOPE_CAP];
    int     n;
    int     spill;
} qns_scope;

static int qns_scope_has(const qns_scope *sc, int64_t id) {
    for (; sc; sc = sc->up) {
        for (int i = 0; i < sc->n; i++)
            if (sc->names[i] == id) return 1;
    }
    return 0;
}

static int qns_scope_spilled(const qns_scope *sc) {
    for (; sc; sc = sc->up)
        if (sc->spill) return 1;
    return 0;
}

static void qns_scope_add(qns_scope *sc, int64_t id) {
    if (qns_scope_has(sc, id)) return;
    if (sc->n >= QNS_SCOPE_CAP) { sc->spill = 1; return; }
    sc->names[sc->n++] = id;
}

static int qns_ident_shaped(const char *p, size_t l) {
    if (l == 0) return 0;
    if (!((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')))
        return 0;
    for (size_t i = 1; i < l; i++) {
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

/* If `node` is a pre-lower assignment `(:;sym;val)` / `(op:;sym;val)` /
 * `(::;sym;val)`, classify the head: 1 = local-binding (`:` or `op:`),
 * 2 = global (`::`), 0 = not an assignment on a sym target. */
static int qns_assign_kind(ray_t *node) {
    if (!node || node->type != RAY_LIST || ray_len(node) != 3) return 0;
    ray_t **e = (ray_t **)ray_data(node);
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return 0;
    if (!e[1] || e[1]->type != -RAY_SYM || (e[1]->attrs & Q_ATTR_QUOTED))
        return 0;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return 0;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int kind = 0;
    if (l == 1 && nm[0] == ':') kind = 1;                       /* `:`   */
    else if (l == 2 && nm[0] == ':' && nm[1] == ':') kind = 2;  /* `::`  */
    else if (l >= 2 && nm[l - 1] == ':' && nm[0] != ':') kind = 1; /* op: */
    ray_release(s);
    return kind;
}

/* Collect local-binding targets of one scope body: plain-`:`/`op:` sym
 * targets at any expression depth, NOT descending into nested lambdas or
 * ctx-literals (each gets its own scope when visited). */
static void qns_collect(ray_t *node, qns_scope *sc, int depth) {
    if (!node || node->type != RAY_LIST || depth > 128) return;
    if (ql_is_lambda(node) || ql_is_ctx_literal(node)) return;
    if (qns_assign_kind(node) == 1) {
        ray_t **e = (ray_t **)ray_data(node);
        ray_t *ts = ray_sym_str(e[1]->i64);
        if (ts) {
            /* dotted targets are global namespace writes, never locals */
            if (ray_str_len(ts) > 0 && ray_str_ptr(ts)[0] != '.')
                qns_scope_add(sc, e[1]->i64);
            ray_release(ts);
        }
    }
    int64_t n = ray_len(node);
    ray_t **e = (ray_t **)ray_data(node);
    for (int64_t i = 0; i < n; i++)
        qns_collect(e[i], sc, depth + 1);
}

/* Rewrite one unquoted name-ref sym slot to `.ctx.name` when it qualifies. */
static void qns_maybe_rewrite(ray_t **slot, const qns_scope *sc,
                              const char *ctx) {
    ray_t *node = *slot;
    if (!node || node->type != -RAY_SYM || (node->attrs & Q_ATTR_QUOTED))
        return;
    if (qns_scope_spilled(sc) || qns_scope_has(sc, node->i64)) return;
    ray_t *s = ray_sym_str(node->i64);
    if (!s) return;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    if (!qns_ident_shaped(nm, l) || q_ns_is_unqualifiable(nm, l)) {
        ray_release(s);
        return;
    }
    char full[192];
    int fl = snprintf(full, sizeof full, "%s.%.*s", ctx, (int)l, nm);
    ray_release(s);
    if (fl <= 0 || (size_t)fl >= sizeof full) return;
    ray_t *repl = ray_sym(ray_sym_intern(full, (size_t)fl));
    if (!repl || RAY_IS_ERR(repl)) return;
    ray_release(node);
    *slot = repl;                      /* name-ref: quoted attr stays clear */
}

static void qns_walk(ray_t **slot, qns_scope *sc, const char *ctx, int depth) {
    ray_t *node = *slot;
    if (!node || depth > 128) return;
    if (node->type == -RAY_SYM) {
        qns_maybe_rewrite(slot, sc, ctx);
        return;
    }
    if (node->type != RAY_LIST) return;
    ray_t **e = (ray_t **)ray_data(node);
    int64_t n = ray_len(node);

    if (ql_is_lambda(node)) {
        /* `({ src params stmt…)` — fresh scope: params + body locals.  The
         * parent chain stays visible (openq's eval resolves outer lambda
         * frames dynamically; not rewriting an outer local preserves that). */
        qns_scope inner = { .up = (qns_scope *)sc, .n = 0, .spill = 0 };
        if (n >= 3 && e[2] && e[2]->type == RAY_SYM) {
            int64_t np = ray_len(e[2]);
            const int64_t *pids = (const int64_t *)ray_data(e[2]);
            for (int64_t i = 0; i < np; i++)
                qns_scope_add(&inner, pids[i]);
        }
        for (int64_t i = 3; i < n; i++)
            qns_collect(e[i], &inner, 0);
        for (int64_t i = 3; i < n; i++)
            qns_walk(&e[i], &inner, ctx, depth + 1);
        return;
    }

    if (ql_is_ctx_literal(node)) {
        /* paren/table literal: element `:` bind a pushed scope (q_ctx_build);
         * enclosing locals stay visible through the parent chain. */
        qns_scope inner = { .up = (qns_scope *)sc, .n = 0, .spill = 0 };
        for (int64_t i = 1; i < n; i++)
            qns_collect(e[i], &inner, 0);
        for (int64_t i = 1; i < n; i++)
            qns_walk(&e[i], &inner, ctx, depth + 1);
        return;
    }

    int ak = qns_assign_kind(node);
    if (ak) {
        /* target: `:`/`op:` bind a local inside a scope (leave it); at the
         * top level — and `::` everywhere — the target is a context global. */
        if (ak == 2 || sc == NULL)
            qns_maybe_rewrite(&e[1], sc, ctx);
        qns_walk(&e[2], sc, ctx, depth + 1);
        return;
    }

    for (int64_t i = 0; i < n; i++)
        qns_walk(&e[i], sc, ctx, depth + 1);
}

ray_t *q_lower(ray_t *ast) {
    /* Context qualification first, on the PRE-lower shapes (assignment heads
     * still `:`/`::`, lambdas still `{`-marked) — see the block comment. */
    const char *qctx = q_ns_current();
    if (ast && !RAY_IS_ERR(ast) && qctx[0])
        qns_walk(&ast, NULL, qctx, 0);
    if (ast && ast->type == RAY_LIST) {
        ray_t *err = q_lower_walk(&ast, 0, 0);
        if (err) { ray_release(ast); return err; }
    }
    return ast;
}

/* q_ast_is_assign — see q_parse.h.  Checks the PRE-lower shape: head is the
 * name-ref `:`/`::` with a sym binding target; a `;` statement sequence asks
 * its last statement. */
int q_ast_is_assign(const ray_t *cast) {
    ray_t *ast = (ray_t *)cast;   /* read-only walk; ray_data lacks a const view */
    if (!ast || ast->type != RAY_LIST || ray_len(ast) < 1) return 0;
    ray_t **e = (ray_t **)ray_data(ast);
    ray_t *h = e[0];
    if (!h || h->type != -RAY_SYM || (h->attrs & Q_ATTR_QUOTED)) return 0;
    ray_t *s = ray_sym_str(h->i64);
    if (!s) return 0;
    const char *nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    int is_semi  = (l == 1 && nm[0] == ';');
    int is_colon = (l == 1 && nm[0] == ':') ||
                   (l == 2 && nm[0] == ':' && nm[1] == ':');
    /* modified assignment `x op: y` (head `<op>:`, op non-empty, not the plain
     * assign colons) is also silent, like a plain assignment */
    int is_modasg = (l >= 2 && nm[l - 1] == ':' && nm[0] != ':');
    ray_release(s);
    if (is_semi) {
        int64_t n = ray_len(ast);
        return n >= 2 ? q_ast_is_assign(e[n - 1]) : 0;
    }
    if (!(is_colon || is_modasg) || ray_len(ast) != 3) return 0;
    ray_t *t = e[1];
    if (t && t->type == -RAY_SYM && !(t->attrs & Q_ATTR_QUOTED)) return 1;
    /* indexed assignment `name[i;…]:v` (target = application headed by an
     * unquoted name-ref) is silent too — kdb console. */
    if (t && t->type == RAY_LIST && ray_len(t) >= 2) {
        ray_t *th = ((ray_t **)ray_data(t))[0];
        return th && th->type == -RAY_SYM && !(th->attrs & Q_ATTR_QUOTED);
    }
    return 0;
}
