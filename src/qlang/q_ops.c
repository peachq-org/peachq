/* q op manifest table + static lexical classifier — see q_ops.h.
 *
 * ROSTER GROUND TRUTH — audited against ray_register_builtins (src/lang/eval.c)
 * and the divergence matrix (specs/2026-07-03-q-op-registry-complete-design.md,
 * corrected in adr/0003):
 *   - rayfall `/` is FLOAT-divide (7/2 -> 3.5) and `%` is MODULO, so q `%`
 *     (float divide) renames rayfall `/`.
 *   - q monadic `-` is negate (rayfall `neg`); `*` monadic is `first`; `#`
 *     monadic is `count`; `=` monadic is `group`; `<`/`>` monadic are grade
 *     up/down (`iasc`/`idesc`); `|` monadic is `reverse`; `&` monadic is
 *     `where`; `,` monadic is `enlist`; `~` monadic is `not`.
 *   - q `=`/`<>` dyadic wrap rayfall `==`/`!=` (char-string element-wise case);
 *     q `#`/`_` dyadic are arg-swap `take` / count-`drop` wrappers.
 *   - q `,` dyadic is join -> rayfall `concat`.
 * DEFERRED (QR_NONE — no clean rayfall target, do NOT guess): monadic `%`
 *   (reciprocal: no builtin).  Dyadic `&`/`|` are the q_min2_wrap/q_max2_wrap
 *   recipes (Lesser/Greater over ray_min2_fn/ray_max2_fn plus the ref/lesser.md
 *   result-type matrix) — the `and`/`or` keywords reuse them.  `any`/`all`
 *   keep QR_NONE rows to reserve the name and fix the lexical class, but they
 *   are no longer deferred — their VALUE is self-hosted in q.q (see their rows
 *   below).
 *
 * Wrapper recipes carry the wrapper FUNCTION POINTER directly (QR_FN*); the
 * bodies live in the domain q_wrap_*.c files and are documented THERE — the
 * decl in q_registry_internal.h is the roster.  Adding a common verb is one
 * row here + one wrapper body + one decl.
 *
 * KW_INFIX includes {div, each, in, within, and, ...} — a keyword row here is what makes the
 * lexer reclassify the name as an infix verb in noun position (the retired
 * q_is_kw_verb memcmp, now manifest-driven). */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry_internal.h" /* wrapper decls for the QR_FN* recipes (brings q_ops.h) */
#include "qlang/eval/q_eval.h"              /* q_eval_at_wrap / q_eval_dot_wrap — the `@` `.` rows */
#include "qlang/eval/q_funsql.h"            /* q_funsql_ques_wrap / q_funsql_bang_wrap — `?` `!` */
#include "qlang/ops/q_bang.h"               /* q_bang — the `!` row */
#include "qlang/ops/q_dollar.h"             /* q_dollar — the `$` row */
#include "qlang/ops/q_index.h"              /* q_index_assign_wrap — the `:` row */
#include <assert.h>
#include <string.h>

/* ===== deterministic / sideeffect AUDIT (feat/q-ops-introspection) =========
 * Every row carries both flags explicitly (`deterministic`, `sideeffect`), the
 * common case being `1, 0`.  This block is the COMPLETE roster of the
 * NON-default rows, audited mechanically against every manifest row (anything
 * unobvious doc-checked against qdocs/), and is exposed verbatim by `.Q.ops[]`.
 *
 * deterministic = 0 (result varies run-to-run — RNG state):
 *   `?`   — dyadic `n?x` roll / `-n?x` deal / permute draw on the RNG
 *           (ref/deal.md, ref/roll.md).  Monadic `?` (distinct) is
 *           deterministic, but a row carries one flag for the whole verb, so
 *           the RNG valence makes the verb nondeterministic.  (`rand`, the
 *           other RNG keyword, is q.q-hosted — no manifest row.)
 *
 * sideeffect = 1 (observable effect beyond the returned value):
 *   set     — assigns a global through a sym handle (ref/get.md `set`).
 *   insert  — appends rows to a NAMED global table by reference (ref/insert.md).
 *   upsert  — named-target upsert rebinds the global (ref/upsert.md).
 *   show    — writes the console display sink (ref/show.md).
 *   system  — runs a `\`-command / shell command (ref/system.md).
 *   exit    — terminates the process, firing `.z.exit` (ref/exit.md).
 *   hopen   — opens an IPC connection (ref/hopen.md).
 *   hclose  — closes an IPC connection (ref/hopen.md).
 *   0:      — File Text: Save Text writes / Load CSV reads a file (ref/file-text.md).
 *   read0   — reads a file's lines (ref/read0.md).
 *   hdel    — deletes a file or (empty) folder (ref/hdel.md).
 *   setenv  — sets a process environment variable (ref/getenv.md).
 * (getenv READS the environment — deterministic given the process env, no
 * mutation — so it is NOT flagged; value/get resolve data purely, the
 * code-executing `value "…"` arm being the caller's effect, not the verb's.)
 * ========================================================================== */

/* ===== FAMILY AUDIT (uniform-structure-dispatch stage 0 metadata) ==========
 * Every row carries a `family` (vocabulary: q_ops.h; spec: actionable-plans/
 * 2026-07-15-uniform-structure-dispatch.md §2/§3).  OPERATIVE since the eval
 * rebuild: the apply module's family lifts dispatch on it (q_eval_apply.c, the
 * `fam` block), the `aggregate` rows adding a `nested` rank-2 sub-law
 * (QNEST_*).  Classification is per the verb's qdocs page; where
 * the page carries an explicit rank label ("X is an atomic/uniform/aggregate
 * function") that label is cited at the row group and FOLLOWED — with exactly
 * ONE exception, `ratios`, whose label contradicts its own examples (the
 * border-rulings entry below).  The spec's map-vs-index
 * split SUBDIVIDES kdb's "uniform" class: uniform ops that are a pure
 * index-space gather (op t = t @ op til count t — reverse, rotate, sublist,
 * next/prev/xprev, take/drop) are `index` (L4); uniform ops that COMPUTE
 * per-position values (sums, fills, deltas, m-windows) are `map` (L2).
 *
 * DUAL-VALENCE rows carry ONE family (the deterministic-flag precedent): the
 * valence the structure-dispatch spec consumes (its §2/§6 family lists), the
 * other valence noted at the row.  Affected: + - * < > = # _ | & ~ ? (each
 * noted inline).
 *
 * BORDER RULINGS proposed for owner ratification (spec §9; full table in the
 * landing PR):
 *   cut    -> index   (position arithmetic + gather; output is slices, the
 *                      L4 gather composed per cut point — ref/cut.md)
 *   where  -> none    (an index-space GENERATOR, but its dict arm is the keys
 *                      gathered by `where value d` — an index off the RANGE,
 *                      not off `til count d`, so it is NOT the L4 positional
 *                      law the index family now carries — ref/where.md)
 *   group  -> rowid   (needs item equality; spec §3 lists group in
 *                      row-identity explicitly — ref/group.md)
 *   ?      -> index   (deal/roll on a table = random-index gather; the dict
 *                      entries-axis collision for find/deal is spec §9.1,
 *                      owner call — ref/deal.md, ref/find.md)
 *   ~      -> aggregate (match reduces two structures to ONE bool atom,
 *                      structure discarded — but it does not follow the L3
 *                      `agg value d` law (keys compared too); near-border)
 *   ratios -> map     (ref/ratios.md LABELS it "an aggregate function" but its
 *                      own examples are length-preserving `update ret:ratios
 *                      price by sym` — the label is a doc misprint; classified
 *                      by the documented behaviour)
 *   joins  -> structural (lj/ij/uj/pj/asof/ej/aj/wj: schema-defining column
 *                      union/fill rules, beyond key equality; alternative
 *                      reading is rowid via the equality kernel — owner call)
 *   xgroup -> rowid   (the grouping/equality essence; restructuring is the
 *                      output shape — vs xkey which is pure structural)
 *   cross  -> structural (generates the product structure; no identity, no
 *                      index law — ref/cross.md)
 *   # _    -> none    (take/drop ARE index gathers, but the left arg picks the
 *                      arm — int atom, int vector reshape, sym vector keys,
 *                      table keys — so the lift's `op til count x` question is
 *                      only ever asked of ONE of them.  Their container laws
 *                      live in the wrapper, which is why `(count t)#t` is `t[::]`
 *                      and not a materialised `til`; ruled 2026-07-27)
 *   count  -> none    (a mapping counts its DOMAIN, so the L3 `agg value d`
 *                      law is wrong for it — ref/count.md; landed 44d86eb4,
 *                      recorded here 2026-07-24)
 *   within -> none    (ref/within.md labels it "left-uniform", but the label
 *                      describes only the LEFT operand: y is BOUNDS — an
 *                      ordered pair, or the flip of a list of ordered pairs —
 *                      which no lift law conforms against x.  Declaring it
 *                      atomic conformed x with (lo;hi) and broke every
 *                      documented form.  The wrapper takes whole args and
 *                      composes `(x>=y 0)&x<=y 1`, so the atomic lift on the
 *                      COMPARISONS supplies every shape)
 * String-model caveat: string/upper/lower/trim/ltrim/rtrim/like classify
 * `atomic` at STRING granularity — the elementwise unit is the whole string,
 * now a RAY_CHARV vector (string-C3; this audit predates C3 and is pure
 * metadata).  ss/ssr consume a whole string as a search domain -> `none`.
 * Self-distributing exception: `$` is family `none` because it is MULTI-CONCEPT
 * (Cast/Tok/Pad/mmu/enum) — one glyph, no one lift law.  It receives WHOLE args
 * and each overload recurses with its OWN boundary (Cast leaves at typed
 * vectors, Tok/Pad at strings); an exception-catalogue member, its container
 * probes live in q_dollar.c (see tools/lint-type-axis.sh BOTTOM_BAND).
 * ========================================================================== */

/* trailing name_lift zero-defaults on unflagged rows (no 166-row churn) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const q_op_t Q_OPS[] = {
    /* name    lex            monadic-recipe             dyadic-recipe            hof  det eff family mono (mono_scan designated) */
    /* ---- arithmetic / compare glyphs — dyadics all doc-labelled atomic
     * (ref/{add,subtract,multiply,divide,greater-than,less-than,equal,
     * not-equal}.md) ---- */
    /* monadic `+` is flip — a k-ism accepted as a deliberate superset (valid q
     * spells it `flip`, same q_flip_wrap — the `_`/floor precedent).  Family
     * is the dyadic Add; monadic flip is structural (the `flip` row). */
    { "+",     QLEX_GLYPH,     QR_FN1("flip", q_flip_wrap),    QR_ENV("+"),       NULL, 1, 0, "atomic", "sum", .mono_scan = "sums" },
    { "-",     QLEX_GLYPH,     QR_FN1A("neg", q_neg_wrap),     QR_ENV("-"),       NULL, 1, 0, "atomic", NULL },
    /* `*` monadic is first (aggregate — the `first` row); family = dyadic. */
    { "*",     QLEX_GLYPH,     QR_ENV("first"),                QR_ENV("*"),       NULL, 1, 0, "atomic", "prd", .mono_scan = "prds" },
    { "%",     QLEX_GLYPH,     QR_NONE,                        QR_ENV("/"),       NULL, 1, 0, "atomic", NULL },
    /* monadic `<`/`>` are grade up/down (rowid — the iasc/idesc rows); shared
     * wrapper adds the DICT arm (keys in value order); family = dyadic. */
    { "<",     QLEX_GLYPH,     QR_FN1("iasc", q_iasc_wrap),    QR_ENV("<"),       NULL, 1, 0, "atomic", NULL },
    { ">",     QLEX_GLYPH,     QR_FN1("idesc", q_idesc_wrap),  QR_ENV(">"),       NULL, 1, 0, "atomic", NULL },
    { "<=",    QLEX_GLYPH,     QR_NONE,                        QR_ENV("<="),      NULL, 1, 0, "atomic", NULL },
    { ">=",    QLEX_GLYPH,     QR_NONE,                        QR_ENV(">="),      NULL, 1, 0, "atomic", NULL },
    /* `=` monadic is group (rowid — the `group` row); family = dyadic Equal. */
    { "=",     QLEX_GLYPH,     QR_FN1("group", q_group_wrap),  QR_FN2("==", q_eq_wrap) , NULL, 1, 0, "atomic", NULL },
    { "<>",    QLEX_GLYPH,     QR_NONE,                        QR_FN2("!=", q_ne_wrap) , NULL, 1, 0, "atomic", NULL },
    /* ---- structural glyphs ---- */
    /* `#` monadic is count (family `none` — see the `count` row); dyadic Take is
     * `none` too (FAMILY AUDIT): its left arg SELECTS the arm, so a container
     * must reach q_take_wrap whole. */
    { "#",     QLEX_GLYPH,     QR_ENV("count"),                QR_FN2("take", q_take_wrap), NULL, 1, 0, "none", NULL },
    /* monadic `_` is a K-ism (valid q spells it `floor`, atomic) — accepting
     * it is a deliberate SUPERSET of q source; the VALUE is kdb-identical
     * (kdb's own floor IS k `_:`).  Dyadic Drop is `none` for Take's reason. */
    { "_",     QLEX_GLYPH,     QR_FN1A("floor", q_floor_wrap), QR_FN2("drop", q_drop_wrap), NULL, 1, 0, "none", NULL },
    /* `|` monadic is reverse (index — the `reverse` row); family = dyadic
     * Greater/max (atomic, ref/greater.md). */
    { "|",     QLEX_GLYPH,     QR_ENV("reverse"),              QR_FN2A("or", q_max2_wrap), NULL, 1, 0, "atomic", "max", .mono_scan = "maxs" },
    /* `&` monadic is where (index — the `where` row, a §9 border); family =
     * dyadic Lesser/min (atomic, ref/lesser.md). */
    { "&",     QLEX_GLYPH,     QR_FN1("where", q_where_wrap),  QR_FN2A("and", q_min2_wrap), NULL, 1, 0, "atomic", "min", .mono_scan = "mins" },
    /* `,` — enlist / join: both construct structure (table,record-dict is
     * upsert semantics — see q_join_wrap). */
    { ",",     QLEX_GLYPH,     QR_ENV("enlist"),               QR_FN2("concat", q_join_wrap), NULL, 1, 0, "structural", "raze" },
    /* `~` monadic is not (ATOMIC — declared on the recipe, since the family
     * column below speaks for the DYAD); family = dyadic Match — reduces to one
     * bool atom (near-border: does NOT follow the L3 value-law; see AUDIT). */
    { "~",     QLEX_GLYPH,     QR_FN1A("not", q_not_wrap),     QR_FN2("match", q_match_wrap), NULL, 1, 0, "aggregate", NULL },
    /* q `x^y` — fill: coalesce nulls in y with x ("Fill is an atomic
     * function", ref/fill.md).  `^` already lexes as a verb glyph
     * (VERB_CHARS); this row gives it a registry value.  Monadic `^x`
     * (kdb: null-of-type / `fills` sans forward-fill) is a deferred cell. */
    { "^",     QLEX_GLYPH,     QR_NONE,                        QR_FN2("fill", q_fill_wrap), NULL, 1, 0, "atomic", NULL },
    /* ---- type-dispatch glyphs (2c-2) ---- */
    /* monadic `!` (dict keys) is a K-ism accepted as a deliberate superset
     * (valid q spells it `key`, same value — the `_`/floor precedent). */
    /* The dyad is a VARY overload matrix (the `@`/`.` precedent): n==2 the
     * classic q_bang dict-make/enkey/internal band, n==4 functional
     * Update/Delete (basics/funsql.md); family "structural" is documentation
     * only — no lift arm dispatches on it. */
    { "!",     QLEX_GLYPH,     QR_FN1("key", q_key_wrap),      QR_FNV("dict", q_funsql_bang_wrap), NULL, 1, 0, "structural", NULL },
    /* monadic `?` (distinct, rowid — the `distinct` row) is likewise a K-ism
     * superset.  Family = dyadic roll/deal/find (L4 pilot: `-3?t`); the dict
     * entries-axis collision is spec §9.1 (see AUDIT). */
    /* The dyad is a VARY overload matrix: n==2 roll/deal/find (q_roll_wrap),
     * n==3 Simple Exec / Vector Conditional, n==4..6 Select/Exec
     * (basics/funsql.md).  family "index" is KEPT OPERATIVE: the n==2
     * container lift (`-3?t`) reads it before the kernel runs. */
    { "?",     QLEX_GLYPH,     QR_FN1("distinct", q_distinct_wrap), QR_FNV("rand", q_funsql_ques_wrap), NULL, 0, 0, "index", NULL },
    /* monadic `$` stays QR_NONE: `$x` is the cast-vs-cond ambiguity, deferred.
     * Dyadic `$` is MULTI-CONCEPT (Cast/Tok/Pad/mmu/enum) so family "none":
     * one glyph has no one lift law, so the apply hands q_dollar WHOLE args and
     * each overload SELF-RECURSES with its own boundary (ruling 2026-07-24 —
     * cast leaves at typed vectors, Tok/Pad at strings).  The bracket cond form
     * `$[c;t;f]` (3+ args) is a q_eval control form, not a registry cell. */
    { "$",     QLEX_GLYPH,     QR_NONE,                        QR_FN2("as", q_dollar), NULL, 1, 0, "none", NULL },
    /* `:` Assign as an operand VALUE — the dyad returning its rhs, so amend's
     * replace form (`@[x;i;:;v]`, ref/amend.md "If v is Assign (:) ...") is
     * plain composition.  Assignment/return stay SYNTAX: q_embed never embeds
     * a colon head, so this row is reachable only in operand position. */
    { ":",     QLEX_GLYPH,     QR_NONE,                        QR_FN2("assign", q_index_assign_wrap), NULL, 1, 0, "none", NULL },
    /* monadic `@`/`.` stay QR_NONE: `@x` type-of is blocked on the q type
     * renumber; `.x` (value/get) comes with handles/namespaces.  Dyadic rows
     * are the full overload matrix (2 Apply/Index, 3 Trap-on-callable /
     * Amend-ternary, 4 Amend — machinery: ops/q_index.c).  Family none:
     * Apply/Index ARE the application machinery (spec §5), not lifted.
     * name_lift: a sym-atom d in the amend forms names a global. */
    { "@",     QLEX_GLYPH,     QR_NONE,                        QR_FNV("at", q_eval_at_wrap),    NULL, 1, 1, "none", NULL, .name_lift = 1 },
    { ".",     QLEX_GLYPH,     QR_NONE,                        QR_FNV("apply", q_eval_dot_wrap), NULL, 1, 1, "none", NULL, .name_lift = 1 },
    /* ---- keyword-infix ---- */
    { "div",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("div"),     NULL, 1, 0, "atomic", NULL },
    /* q `x mod y` — modulus (ref/mod.md, atomic).  PURE RENAME: rayfall `%` IS
     * floored modulo with the sign following the divisor, exactly kdb mod
     * (audited live: -3 -2 mod 3 -> 0 1; 7 mod -2 -> -1; -7 mod -2.5 -> -2;
     * null passes through) — the mirror trick of q `%` -> rayfall `/`.  Base
     * `%` is registered RAY_FN_ATOMIC, so vector/dict broadcast comes free. */
    { "mod",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("%"),       NULL, 1, 0, "atomic", NULL },
    /* q `x xexp y` / `x xlog y` — dyadic atomic libm wrappers (ref/exp.md,
     * ref/log.md: both atomic); null/divergence details at the bodies. */
    { "xexp",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("xexp", q_xexp_wrap), NULL, 1, 0, "atomic", NULL },
    { "xlog",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("xlog", q_xlog_wrap), NULL, 1, 0, "atomic", NULL },
    /* q `f each x` == `f'x`: a dyadic wrapper over rayfall map (+ vector
     * collapse, since map returns a boxed list where q wants a simple vec). */
    { "each",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("map", q_hof_nyi_wrap), "map", 1, 0, "none", NULL },
    /* q `x in y` — membership (ref/in.md: left-atomic comparison): typed-
     * vector y via base ray_in_fn; generic-list y is whole-item, rank-
     * sensitive.  rowid: needs item equality (set-op predicate). */
    { "in",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("in", q_in_wrap), NULL, 1, 0, "rowid", NULL },
    /* q `and`/`or` — keyword spellings of `&`/`|` (ref/lesser.md,
     * ref/greater.md: atomic), REUSING the same wrappers the glyphs route to.
     * Monadic cells stay QR_NONE: both are dyadic-only in q, so prefix `and x`
     * misses and eval falls through to rayfall's scalar special form. */
    { "and",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("and", q_min2_wrap), NULL, 1, 0, "atomic", NULL },
    { "or",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("or", q_max2_wrap), NULL, 1, 0, "atomic", NULL },
    /* q `x within y` — inclusive bounds check (ref/within.md).  y is BOUNDS,
     * not an operand conforming to x, so no lift law fits: family `none` and
     * the wrapper takes whole args.  Border ruling: see FAMILY AUDIT. */
    { "within",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("within", q_within_wrap), NULL, 1, 0, "none", NULL },
    /* q `n cut x` — chunk (int atom) / positional cut (int vector).  Border
     * ruling: index (see FAMILY AUDIT). */
    { "cut",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("cut", q_cut_wrap), NULL, 1, 0, "index", NULL },
    /* q `n rotate x` / `n sublist x` — dyadic infix keywords (ref/rotate.md
     * "uniform" / ref/sublist.md) — both pure index-space gathers (L4).
     * Not KW_INFIX -> the scanner would split `n rotate x` into two
     * statements, so the manifest row is what makes them infix.  rotate is a
     * WRAPPER, not the q.q derivation it once was: a QSRC lambda short-circuits
     * family dispatch, so only a manifest value can reach the index lift and
     * learn dicts/tables/keyed tables from the one law. */
    { "rotate",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("rotate", q_rotate_wrap), NULL, 1, 0, "index", NULL },
    { "sublist",QLEX_KW_INFIX, QR_NONE,                        QR_QSRC("sublist"), NULL, 1, 0, "index", NULL },
    /* q `x vs y` / `x sv y` — split-join / base-encode family (dyadic infix
     * keywords; wrappers, native -RAY_STR + sym + base + byte).  Monadic form
     * is out of scope (kdb `vs`/`sv` are strictly dyadic).  Codecs: none. */
    { "vs",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("vs", q_vs_wrap), NULL, 1, 0, "none", NULL },
    { "sv",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("sv", q_sv_wrap), NULL, 1, 0, "none", NULL },
    /* ---- set operations (feat/q-setops) — rowid (item identity, spec §3) ---- */
    /* q `x except y` — items of x not in y, x-duplicates and order KEPT
     * (ref/except.md).  rayfall's ray_except_fn is exactly this for lists;
     * the q_except_wrap adds a TABLE-pair arm (row membership) and delegates
     * everything else to ray_except_fn unchanged (was a pure QR_ENV row
     * pre-table-verbs).  Target stays "except" for provenance/serde. */
    { "except",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("except", q_except_wrap), NULL, 1, 0, "rowid", NULL },
    /* q `x union y` == `distinct x,y` — WRAPPER: rayfall union keeps
     * x-duplicates, kdb dedups the whole join (ref/union.md). */
    { "union", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("union", q_union_wrap), NULL, 1, 0, "rowid", NULL },
    /* q `x inter y` — items of x that are in y, x-duplicates and order KEPT
     * (ref/inter.md: "uses the result of x in y to return items from x").
     * rayfall spells it `sect` (ray_sect_fn) and is exactly this for lists;
     * the thin wrapper only guards dict/table operands 'nyi (rayfall sect
     * returns a wrong-shaped dict there; kdb returns common values). */
    { "inter", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("sect", q_inter_wrap), NULL, 1, 0, "rowid", NULL },
    /* q `x cross y` == {raze x,/:\:y} — Cartesian product WRAPPER (no
     * rayfall cartesian primitive; composes item access + q join).  String
     * and dict/table operands are deferred cells (ref/cross.md).  Border
     * ruling: structural (see FAMILY AUDIT). */
    { "cross", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("cross", q_cross_wrap), NULL, 1, 0, "structural", NULL },
    /* ---- q join family (feat/q-joins-rebuild) ----
     * Dyadic infix keywords over the engine hash/asof join (pair-relation
     * core in ops/q_join.c: rowid-augmented key tables through
     * ray_left_join_fn / ray_asof_join_fn, kdb-ordered, gather-assembled).
     * Family: structural (schema-defining; border ruling in the AUDIT). */
    { "lj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("lj", q_lj_wrap),   NULL, 1, 0, "structural", NULL },
    { "ljf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ljf", q_ljf_wrap), NULL, 1, 0, "structural", NULL },
    { "ij",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ij", q_ij_wrap),   NULL, 1, 0, "structural", NULL },
    { "ijf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ijf", q_ijf_wrap), NULL, 1, 0, "structural", NULL },
    { "uj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("uj", q_uj_wrap),   NULL, 1, 0, "structural", NULL },
    { "ujf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ujf", q_ujf_wrap), NULL, 1, 0, "structural", NULL },
    { "pj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("pj", q_pj_wrap),   NULL, 1, 0, "structural", NULL },
    { "asof",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("asof", q_asof_wrap), NULL, 1, 0, "structural", NULL },
    /* Bracket-form joins (ej[c;t1;t2], aj[...], wj[w;f;t;spec]): triadic-plus
     * prefix keywords — q-owned env VARY bindings (q_builtins_register),
     * snapshotted here as QR_ENV rows (the ssr precedent: the parser
     * name-refs `ej[a;b;c]`, so it resolves through the env). */
    { "ej",    QLEX_KW_PREFIX, QR_ENV("ej"),                   QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "aj",    QLEX_KW_PREFIX, QR_ENV("aj"),                   QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "aj0",   QLEX_KW_PREFIX, QR_ENV("aj0"),                  QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "ajf",   QLEX_KW_PREFIX, QR_ENV("ajf"),                  QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "ajf0",  QLEX_KW_PREFIX, QR_ENV("ajf0"),                 QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "wj",    QLEX_KW_PREFIX, QR_ENV("wj"),                   QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "wj1",   QLEX_KW_PREFIX, QR_ENV("wj1"),                  QR_NONE,           NULL, 1, 0, "structural", NULL },
    /* ---- sort / bucket family (feat/q-sort-rank) — dyadic infix ----
     * `bin`/`binr` search a sorted domain (sorted-vec left, value right;
     * ordering-based search -> rowid), bodied in ops/q_search.c — the base
     * kernels are i64-only, so the q surface takes its order from the `<`
     * verb instead.  `xrank` is a q.q
     * derivation over the grade (QR_QSRC; ref/xrank.md "right-uniform" — the
     * ordering essence puts it in rowid).  `xbar` is an ARG-SWAP wrapper
     * (rayfall xbar is (col,bucket); q spells it (bucket,col)) — "xbar is
     * atomic" (ref/xbar.md).  All infix so `a verb b` is one stmt. */
    { "bin",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("bin", q_bin_wrap),   NULL, 1, 0, "rowid", NULL },
    { "binr",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("binr", q_binr_wrap), NULL, 1, 0, "rowid", NULL },
    { "xrank", QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("xrank"),  NULL, 1, 0, "rowid", NULL },
    { "xbar",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("xbar", q_xbar_wrap), NULL, 1, 0, "atomic", NULL },
    /* ---- Wave 5: running scans (monadic prefix keywords) — all doc-labelled
     * uniform (ref/{sum,prd,max,min,avg}.md); null discipline at the bodies
     * (ops/q_agg.c).  `ratios` is a border case: its page's LABEL says
     * aggregate, its documented behaviour is uniform (see FAMILY AUDIT). ---- */
    { "sums",  QLEX_KW_PREFIX, QR_FN1("sums", q_sums_wrap),    QR_NONE,           NULL, 1, 0, "map", NULL },
    { "prds",  QLEX_KW_PREFIX, QR_FN1("prds", q_prds_wrap),    QR_NONE,           NULL, 1, 0, "map", NULL },
    /* q `prd x` — the multiply-over fold twin of prds (ref/prd.md: aggregate):
     * nulls are 1s, bool vector -> int, list-of-lists element-wise, dict/table
     * implicit iteration.  Wrapper (no rayfall product aggregate). */
    /* QNEST_COLUMNS, not QNEST_FOLD: basics/math.md's null-preserving roster is
     * `avg min max sum` — prd keeps nulls-as-1s nested too (`prd (1 0N;2 3)`
     * is `2 3`), which is exactly the per-column law. */
    { "prd",   QLEX_KW_PREFIX, QR_FN1("prd", q_prd_wrap),      QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_COLUMNS },
    { "maxs",  QLEX_KW_PREFIX, QR_FN1("maxs", q_maxs_wrap),    QR_NONE,           NULL, 1, 0, "map", NULL },
    { "mins",  QLEX_KW_PREFIX, QR_FN1("mins", q_mins_wrap),    QR_NONE,           NULL, 1, 0, "map", NULL },
    { "avgs",  QLEX_KW_PREFIX, QR_FN1("avgs", q_avgs_wrap),    QR_NONE,           NULL, 1, 0, "map", NULL },
    { "ratios",QLEX_KW_PREFIX, QR_FN1("ratios", q_ratios_wrap),QR_NONE,           NULL, 1, 0, "map", NULL },
    /* ---- Wave 5: weighted (dyadic infix keywords) — aggregates
     * (ref/sum.md wsum, ref/avg.md wavg).  wsum is q.q-hosted; wavg stays a
     * wrapper (its composition's bool-multiply path measured 16x slower). ---- */
    { "wsum",  QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("wsum"),   NULL, 1, 0, "aggregate", NULL },
    { "wavg",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("wavg", q_wavg_wrap), NULL, 1, 0, "aggregate", NULL },
    /* ---- Wave 5: statistical (renames of audited base aggregates; all
     * doc-labelled aggregate — ref/{med,var,dev,cor,cov}.md) ----
     * kdb `var`/`dev` are POPULATION (÷n); rayfall `var`/`stddev` are SAMPLE
     * (÷n-1) and `var_pop`/`stddev_pop`/`dev` are population — so q `var`->
     * `var_pop`, q `svar`->`var`, q `sdev`->`stddev`, q `dev` stays `dev`. */
    /* `med` is self-hosted in q.q (ref/med.md docs formula) — no row; the
     * bootstrap shadow-rebind points root `med` at `.q.med` (the engine env
     * `med` would otherwise win name resolution).  It takes NO rank-2 sub-law:
     * ref/med.md's own `med t` is `a|3 b|-6`, the middle ITEM (row) — not the
     * per-column median that `each flip` would give. */
    { "var",   QLEX_KW_PREFIX, QR_ENV("var_pop"),              QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_COLUMNS },
    { "svar",  QLEX_KW_PREFIX, QR_ENV("var"),                  QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_COLUMNS },
    { "sdev",  QLEX_KW_PREFIX, QR_ENV("stddev"),               QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_COLUMNS },
    { "cor",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("pearson_corr"), NULL, 1, 0, "aggregate", NULL },
    { "cov",   QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("cov"),    NULL, 1, 0, "aggregate", NULL },
    { "scov",  QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("scov"),   NULL, 1, 0, "aggregate", NULL },
    /* fby: family `none` — the LEFT arg is the PAIR `(aggr;d)`, which must reach
     * the derivation whole; q.q-hosted because ref/fby.md prints the spelling. */
    { "fby",   QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("fby"),    NULL, 1, 0, "none", NULL },
    /* ---- Wave 5: sliding m-windows + ema (dyadic infix keywords) — all
     * doc-labelled uniform (ref/{sum,avg,max,min,dev,count,ema}.md) ---- */
    { "msum",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("msum", q_msum_wrap), NULL, 1, 0, "map", NULL },
    { "mavg",  QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("mavg"),   NULL, 1, 0, "map", NULL },
    { "mmax",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mmax", q_mmax_wrap), NULL, 1, 0, "map", NULL },
    { "mmin",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mmin", q_mmin_wrap), NULL, 1, 0, "map", NULL },
    { "mcount",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mcount", q_mcount_wrap), NULL, 1, 0, "map", NULL },
    { "mdev",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mdev", q_mdev_wrap), NULL, 1, 0, "map", NULL },
    { "ema",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ema", q_ema_wrap), NULL, 1, 0, "map", NULL },
    /* mmu (matrix multiply / dot product) — bespoke matrix op owning its own
     * shape logic (family none, like @ .); `$` reaches it via q_dollar_mmu. */
    { "mmu",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mmu", q_mmu_wrap), NULL, 1, 0, "none", NULL },
    /* iterator mnemonic keywords (wave-2): infix `f over/scan/prior/peach x`,
     * same lexical treatment as `each`.  over/scan dispatch reduce/converge/
     * do/while by f rank; prior is unary each-prior; peach == each. */
    { "over",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("over", q_hof_nyi_wrap), "fold", 1, 0, "none", NULL },
    { "scan",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("scan-kw", q_hof_nyi_wrap), "scan", 1, 0, "none", NULL },
    { "prior", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("prior", q_hof_nyi_wrap), "prior", 1, 0, "none", NULL },
    { "peach", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("peach", q_hof_nyi_wrap), "map", 1, 0, "none", NULL },
    /* ---- keyword-prefix monads (pass-through/rename) ---- */
    { "neg",     QLEX_KW_PREFIX, QR_FN1A("neg", q_neg_wrap),   QR_NONE,           NULL, 1, 0, "atomic", NULL },
    /* q `til` — generator (atom -> vector; no structure input, family none);
     * kdb accepts a boolean (`til 1b` -> ,0), base ray_til_fn is int-only. */
    { "til",     QLEX_KW_PREFIX, QR_FN1("til", q_til_wrap),    QR_NONE,           NULL, 1, 0, "none", NULL },
    { "count",   QLEX_KW_PREFIX, QR_ENV("count"),              QR_NONE,           NULL, 1, 0, "none",      NULL },
    /* first/last — aggregates (ref/first.md: "first is an aggregate"). */
    { "first",   QLEX_KW_PREFIX, QR_ENV("first"),              QR_NONE,           NULL, 1, 0, "aggregate", NULL },
    { "last",    QLEX_KW_PREFIX, QR_ENV("last"),               QR_NONE,           NULL, 1, 0, "aggregate", NULL },
    /* q `n xprev x` — n-item shift (ref/next.md: right-uniform), null-filling
     * the vacated end; dyadic infix like rotate.  L4 index: a shift IS a
     * gather by shifted indices (spec §2 lists next/prev/xprev in the index
     * family).  `next`/`prev` are its unit shifts, self-hosted in q.q
     * (`.q.next:xprev[-1;]` / `.q.prev:xprev[1;]`) — no manifest rows. */
    { "xprev",   QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("xprev", q_xprev_wrap), NULL, 1, 0, "index", NULL },
    /* q `fills x` — forward-fill nulls (ref/fill.md: uniform; the `^\`
     * fill-scan).  Computes values (not a gather) -> map. */
    { "fills",   QLEX_KW_PREFIX, QR_FN1("fills", q_fills_wrap), QR_NONE,          NULL, 1, 0, "map", NULL },
    /* q `where x` — an index-space generator, but NOT the L4 one: its entries
     * index comes from `where value d`, never from `til count d`, so the
     * positional lift would silently mis-answer it.  family none until the
     * values-derived arm lands (see FAMILY AUDIT); int-vector replicate +
     * bool-mask forms at the body. */
    { "where",   QLEX_KW_PREFIX, QR_FN1("where", q_where_wrap), QR_NONE,          NULL, 1, 0, "none", NULL },
    { "reverse", QLEX_KW_PREFIX, QR_ENV("reverse"),            QR_NONE,           NULL, 1, 0, "index", NULL },
    /* q `sum` over a boxed LIST sums the items (`sum(dates;times)` ->
     * timestamps, ref/file-text.md); rayfall sum is vector-only, so wrapper.
     * NB the wrapper is deliberately NOT RAY_FN_AGGR — the eval aggregate
     * fast path claims AGGR fns before the wrapper runs and 'types on a boxed
     * list-of-vectors; name-routing (RAY_FN_Q_LOWER + aux "sum") keeps
     * query/DAG behaviour. */
    { "sum",     QLEX_KW_PREFIX, QR_FN1("sum", q_sum_wrap),    QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_FOLD },
    /* group — rowid (item equality; spec §3 lists it; a §9 border flag).
     * Wrapper: the dict/table arms (funsql wave 1) — vectors still delegate
     * to the base hash kernel inside q_group_wrap. */
    { "group",   QLEX_KW_PREFIX, QR_FN1("group", q_group_wrap), QR_NONE,          NULL, 1, 0, "rowid", NULL },
    /* ---- grade: THE ordering primitive — monadic prefix; rowid (spec §3
     * row-identity, the #174 stable-grade kernel) ----
     * iasc/idesc own ordering for EVERY structure (vector -> ray_iasc_fn; dict
     * -> keys by the value grade; table/keyed -> the engine composite-radix row
     * grade) and share those arms with the `<`/`>` glyphs.  Direction is a
     * kernel flag, NOT `reverse iasc`: ties must not reverse (idesc 2 1 2 =
     * 0 2 1).  asc/desc/rank are q.q derivations over these and carry NO row
     * (prefix names resolve via .q); xasc/xdesc/xrank keep infix rows (QR_QSRC).
     * The `s#` attribute stays a divergence: the attr-take arm accepts long
     * vectors only, so asc cannot set it without regressing symbol sorts. */
    { "iasc",    QLEX_KW_PREFIX, QR_FN1("iasc", q_iasc_wrap),  QR_NONE,           NULL, 1, 0, "rowid", NULL },
    { "idesc",   QLEX_KW_PREFIX, QR_FN1("idesc", q_idesc_wrap), QR_NONE,          NULL, 1, 0, "rowid", NULL },
    { "avg",     QLEX_KW_PREFIX, QR_ENV("avg"),                QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_MEAN },
    /* q `floor` returns LONGS from floats (kdb `floor 3.7` is 3j); rayfall's
     * env floor keeps f64, so this is the q_floor_wrap, not a rename. */
    { "floor",   QLEX_KW_PREFIX, QR_FN1A("floor", q_floor_wrap), QR_NONE,         NULL, 1, 0, "atomic", NULL },
    /* `key` is a wrapper (dict arm; file-handle/namespace/enumeration
     * overloads are deferred cells); `value`/`get`/`eval` bind the evaluator's
     * own homes (rows below).
     * `distinct` must preserve FIRST-OCCURRENCE order (kdb ref/distinct.md),
     * while rayfall's distinct routes typed vectors through the DAG group
     * path, which sorts — so it too is a wrapper, not a rename (audited:
     * `distinct 2 3 7 3 5 3` must be 2 3 7 5, env distinct gives 2 3 5 7). */
    { "key",     QLEX_KW_PREFIX, QR_FN1("key", q_key_wrap),    QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "value",   QLEX_KW_PREFIX, QR_FN1("value", q_eval_value_wrap), QR_NONE,          NULL, 1, 0, "structural", NULL },
    /* q `get` is a SYNONYM of `value` (ref/get.md: "completely interchangeable")
     * — one home.  `nam set y` writes the named global (sym-handle assign /
     * `. context restore); file-read forms are deferred cells ('name today).
     * `eval` has NO row: like md5/hcount it is q.q-hosted over its internal
     * (.q.eval:(-6!), the q_bang.c arm -> q_eval; basics/internal.md). */
    { "get",     QLEX_KW_PREFIX, QR_FN1("value", q_eval_value_wrap), QR_NONE,          NULL, 1, 0, "structural", NULL },
    { "set",     QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("set-g", q_setg_wrap), NULL, 1, 1, "none", NULL },
    { "distinct",QLEX_KW_PREFIX, QR_FN1("distinct", q_distinct_wrap), QR_NONE,    NULL, 1, 0, "rowid", NULL },
    /* q `rand x` == {first 1?x} — self-hosted in q.q verbatim from ref/rand.md;
     * no row.  The bootstrap shadow-rebind points root `rand` at `.q.rand`
     * (rayfall's env `rand` is dyadic and would otherwise win name
     * resolution; the `?` roll wrapper calls ray_rand_fn directly). */
    /* q-implemented keywords: env bindings added by q_builtins_register
     * (same mechanism as `parse`), snapshotted here as pass-through rows.
     * string is atomic (ref/string.md: "atomic … iterates through
     * dictionaries and tables"); upper/lower likewise elementwise.  Atomic
     * here is at STRING granularity (see the FAMILY AUDIT caveat). */
    { "string",  QLEX_KW_PREFIX, QR_ENV("string"),             QR_NONE,           NULL, 1, 0, "atomic", NULL },
    { "upper",   QLEX_KW_PREFIX, QR_ENV("upper"),              QR_NONE,           NULL, 1, 0, "atomic", NULL },
    { "lower",   QLEX_KW_PREFIX, QR_ENV("lower"),              QR_NONE,           NULL, 1, 0, "atomic", NULL },
    /* ---- string trim / hash / search family (feat/q-string-fns) ----
     * trim/ltrim/rtrim are q-owned env unaries (q_builtins_register),
     * snapshotted here as QR_ENV prefix rows (same mechanism as `string`).
     * (trim stays C: its q.q form {ltrim rtrim x} measured 3.7x on a 1M
     * string list — two passes + an intermediate list.)  trim family is
     * elementwise over string atoms (atomic at string granularity).  md5 has
     * NO row: it is `.q.md5:(-15!)` in q.q (reserved via the dynamic `.q`
     * probe), the C body q_md5_fn reached through the `!` internal-fn arm.
     * `like`/`ss` are dyadic infix; `ssr` is a triadic-prefix wrapper. */
    { "trim",    QLEX_KW_PREFIX, QR_ENV("trim"),               QR_NONE,           NULL, 1, 0, "none", NULL },
    { "ltrim",   QLEX_KW_PREFIX, QR_ENV("ltrim"),              QR_NONE,           NULL, 1, 0, "none", NULL },
    { "rtrim",   QLEX_KW_PREFIX, QR_ENV("rtrim"),              QR_NONE,           NULL, 1, 0, "none", NULL },
    { "like",    QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("like", q_like_wrap), NULL, 1, 0, "none", NULL },
    { "ss",      QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("ss", q_ss_wrap), NULL, 1, 0, "none", NULL },
    { "ssr",     QLEX_KW_PREFIX, QR_ENV("ssr"),                QR_NONE,           NULL, 1, 0, "none", NULL },
    { "show",    QLEX_KW_PREFIX, QR_ENV("show"),               QR_NONE,           NULL, 1, 1, "none", NULL },
    /* `system "…"` — q-owned env unary (q_builtins_register), snapshotted here
     * so the parser embeds it; passes through q_sys_run (q_sys.c). */
    { "system",  QLEX_KW_PREFIX, QR_ENV("system"),             QR_NONE,           NULL, 1, 1, "none", NULL },
    /* `exit x` — process termination via q_sys_exit (q_sys.c: `.z.exit`, then
     * exit(x); capability-gated so doctest/wasm runtimes survive it). */
    { "exit",    QLEX_KW_PREFIX, QR_FN1("exit", q_exit_wrap), QR_NONE,           NULL, 1, 1, "none", NULL },
    /* table introspection — q-owned bindings (q_builtins_register), snapshotted
     * here so the parser embeds them over the base env `meta`. */
    { "meta",    QLEX_KW_PREFIX, QR_ENV("meta"),               QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "cols",    QLEX_KW_PREFIX, QR_ENV("cols"),               QR_NONE,           NULL, 1, 0, "structural", NULL },
    /* q `attr x` — column attribute as a single symbol (ref/set-attribute.md,
     * ref/attr.md).  Monadic wrapper over the engine's `.attr.get`, collapsing
     * the full-name sym vector to kdb's one letter.  The set/clear side is the
     * `#` verb's symbol-atom arm (q_take_wrap), not a row here. */
    { "attr",    QLEX_KW_PREFIX, QR_FN1("attr", q_attr_wrap),  QR_NONE,           NULL, 1, 0, "structural", NULL },
    /* ---- additional monadic pass-through keywords (rayfall name == q name,
     * audited kdb-true element-wise / aggregate semantics; atomic labels per
     * ref/{abs,exp,log,sqrt,null}.md, aggregate per ref/{dev,max,min}.md). See
     * docs/recipes/add-q-keyword-verb.md. ---- */
    { "abs",     QLEX_KW_PREFIX, QR_ENV("abs"),                QR_NONE,           NULL, 1, 0, "atomic", NULL },
    /* q `null x` — elementwise null test (ref/null.md: atomic).  Routes to
     * the engine's atomic `nil?` (RAY_FN_ATOMIC): broadcasts over vectors and
     * nested lists at every depth.  The wrapper collapses a homogeneous
     * top-level bool-atom run (heterogeneous input list) to a bool vector
     * for kdb-true display. */
    { "null",    QLEX_KW_PREFIX, QR_FN1("nil?", q_null_wrap),  QR_NONE,           NULL, 1, 0, "atomic", NULL },
    { "dev",     QLEX_KW_PREFIX, QR_ENV("dev"),                QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_COLUMNS },
    { "exp",     QLEX_KW_PREFIX, QR_ENV("exp"),                QR_NONE,           NULL, 1, 0, "atomic", NULL },
    { "log",     QLEX_KW_PREFIX, QR_ENV("log"),                QR_NONE,           NULL, 1, 0, "atomic", NULL },
    { "max",     QLEX_KW_PREFIX, QR_ENV("max"),                QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_FOLD },
    { "min",     QLEX_KW_PREFIX, QR_ENV("min"),                QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_FOLD },
    /* rank == iasc iasc (ref/rank.md) — the grade family, rowid. */
    /* q `raze x` — flattens one level of structure (ref/raze.md; base
     * ray_raze_fn plus the kdb atom arm `raze 42` -> ,42). */
    { "raze",    QLEX_KW_PREFIX, QR_FN1("raze", q_raze_wrap),  QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "sqrt",    QLEX_KW_PREFIX, QR_ENV("sqrt"),               QR_NONE,           NULL, 1, 0, "atomic", NULL },
    /* ---- atomic unary math (feat/q-math-atomic) — implement-via-libm, no
     * rayfall counterpart (rayfall has exp/log/sqrt but not the trig set).
     * All monadic KW_PREFIX, doc-labelled atomic (ref/{trig,
     * signum,ceiling}.md); sentinel-null discipline at the bodies
     * (ops/q_math.c).  `ceiling` is the floor-wrapper twin (float->LONG),
     * NOT rayfall `ceil` (f64).  `signum` is family-atomic but built WITHOUT
     * RAY_FN_ATOMIC — it drives its own broadcast so a top-level boxed-list
     * result collapses to an int vector (see q_signum_wrap). */
    { "sin",       QLEX_KW_PREFIX, QR_FN1A("sin", q_sin_wrap),   QR_NONE, NULL, 1, 0, "atomic", NULL },
    { "cos",       QLEX_KW_PREFIX, QR_FN1A("cos", q_cos_wrap),   QR_NONE, NULL, 1, 0, "atomic", NULL },
    { "tan",       QLEX_KW_PREFIX, QR_FN1A("tan", q_tan_wrap),   QR_NONE, NULL, 1, 0, "atomic", NULL },
    { "asin",      QLEX_KW_PREFIX, QR_FN1A("asin", q_asin_wrap), QR_NONE, NULL, 1, 0, "atomic", NULL },
    { "acos",      QLEX_KW_PREFIX, QR_FN1A("acos", q_acos_wrap), QR_NONE, NULL, 1, 0, "atomic", NULL },
    { "atan",      QLEX_KW_PREFIX, QR_FN1A("atan", q_atan_wrap), QR_NONE, NULL, 1, 0, "atomic", NULL },
    /* `reciprocal` is self-hosted in q.q (`.q.reciprocal:%[1;]`) — no row. */
    { "signum",    QLEX_KW_PREFIX, QR_FN1("signum", q_signum_wrap), QR_NONE, NULL, 1, 0, "atomic", NULL },
    { "ceiling",   QLEX_KW_PREFIX, QR_FN1A("ceiling", q_ceiling_wrap), QR_NONE, NULL, 1, 0, "atomic", NULL },
    /* ---- table verbs (feat/q-table-verbs) — all wrappers in ops/q_table.c
     * EXCEPT xcol/xcols, which are q.q derivations over `.Q.ft` (QR_QSRC:
     * bound post-bootstrap; the row is only what keeps them infix).
     * The wrappers build over the wave-4 keyed primitives (q_bang_enkey/
     * q_table_flatten).  `insert`/`upsert` intentionally SHADOW the base env
     * special forms of the same name: q semantics differ (by-name targets,
     * keyed collision/update, row-index results), so the registry value is a
     * wrapper, never the env snapshot.
     * Families: flip/keys/xkey/xcol/xcols/ungroup/insert/upsert define or
     * rearrange the structures (structural, spec §3); xasc/xdesc sort (rowid —
     * q.q derivations grading the named columns, then ONE row gather);
     * xgroup groups (rowid — border ruling in the AUDIT). */
    { "flip",   QLEX_KW_PREFIX, QR_FN1("flip", q_flip_wrap),   QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "keys",   QLEX_KW_PREFIX, QR_FN1("keys", q_keys_wrap),   QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "ungroup",QLEX_KW_PREFIX, QR_FN1("ungroup", q_ungroup_wrap), QR_NONE,       NULL, 1, 0, "structural", NULL },
    { "xkey",   QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xkey", q_xkey_wrap), NULL, 1, 0, "structural", NULL },
    { "xcol",   QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xcol"),   NULL, 1, 0, "structural", NULL },
    { "xcols",  QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xcols"),  NULL, 1, 0, "structural", NULL },
    { "xasc",   QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xasc"),   NULL, 1, 0, "rowid", NULL },
    { "xdesc",  QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xdesc"),  NULL, 1, 0, "rowid", NULL },
    { "xgroup", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xgroup", q_xgroup_wrap), NULL, 1, 0, "rowid", NULL },
    { "insert", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("insert", q_insert_wrap), NULL, 1, 1, "structural", NULL },
    { "upsert", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("upsert", q_upsert_wrap), NULL, 1, 1, "structural", NULL },
    /* each-prior mnemonics `deltas` ((-':)x) and `differ` (not(~':)x) are
     * self-hosted in q.q — no rows (both ride the each-prior HOF the C
     * wrappers called anyway; measured parity). */
    /* `any`/`all` are self-hosted in q.q (the boolean coercion the DEFER here
     * once held: cast then fold — a `max`/`min` rename is type-wrong, since
     * ref/all-any.md ranges every domain to boolean).  The rows keep the names
     * reserved and fix the lexical class; the VALUE comes from `.q`.
     * NB: `or` is the keyword spelling of `|`-max and still waits for `|`-max
     * (a valueless QLEX_KW_INFIX row would expose rayfall's scalar `or`). */
    { "any",     QLEX_KW_PREFIX, QR_NONE,                      QR_NONE,           NULL, 1, 0, "aggregate", NULL },
    { "all",     QLEX_KW_PREFIX, QR_NONE,                      QR_NONE,           NULL, 1, 0, "aggregate", NULL },
    /* ---- IPC client verbs (feat/q-ipc-client, Phase D) — thin wrappers over the
     * kdb-speaking `.ipc.*` primitives (Phase C).  `hopen` normalizes int|string|
     * (conn;timeout) into the `.ipc.open` string API and returns the connection's
     * socket fd as the handle (kdb-faithful — 0/1/2 reserved, connections at 3+);
     * `hclose` translates it back and routes to `.ipc.close`.  The sync/async send
     * verb `h"query"` is handle-as-verb application (the apply module's int-head arm), not a
     * manifest row.  Both are monadic prefix keywords (KW_PREFIX). ---- */
    { "hopen",  QLEX_KW_PREFIX, QR_FN1("hopen", q_hopen_wrap),   QR_NONE, NULL, 1, 1, "none", NULL },
    { "hclose", QLEX_KW_PREFIX, QR_FN1("hclose", q_hclose_wrap), QR_NONE, NULL, 1, 1, "none", NULL },
    /* ---- File Text (feat/q-file-text) — the `0:` operator + companions.
     * `0:` is tokenized by the scanner's digit-colon arm (a single 0/1/2
     * glued to ':' can never start a clock literal) and dispatches on the
     * LEFT operand's shape (see q_filetext_wrap: Save Text / Prepare Text /
     * Key-Value pairs / Load CSV / Load Fixed — text forms ONLY, binary
     * 1:/2: are an owner-ruled non-goal, so they have NO row and stay
     * name-refs ('name)).  `read0` is also env-bound by q_builtins for the
     * bracket-call form (the ssr/value precedent).  `hsym` is elementwise
     * over sym atoms/vectors (ref/hsym.md: atom or vector). */
    { "0:",     QLEX_GLYPH,     QR_NONE,                       QR_FN2("file-text", q_filetext_wrap), NULL, 1, 1, "none", NULL },
    { "hsym",   QLEX_KW_PREFIX, QR_FN1("hsym", q_hsym_wrap),   QR_NONE,           NULL, 1, 0, "atomic", NULL },
    { "read0",  QLEX_KW_PREFIX, QR_FN1("read0", q_read0_wrap), QR_NONE,           NULL, 1, 1, "none", NULL },
    { "read1",  QLEX_KW_PREFIX, QR_FN1("read1", q_read1_wrap), QR_NONE,           NULL, 1, 1, "none", NULL },
    { "hdel",   QLEX_KW_PREFIX, QR_FN1("hdel", q_hdel_wrap),   QR_NONE,           NULL, 1, 1, "none", NULL },
    /* ---- environment variables (feat/q-getenv-setenv, ref/getenv.md) ----
     * `getenv` is a monadic prefix keyword (sym -> value string, "" unset);
     * `setenv` is a dyadic infix keyword (`sym setenv str`), so it MUST be
     * KW_INFIX or the scanner would split `x setenv y` into two statements
     * (the `set`/`xkey` precedent).  Both reuse the base .os.getenv/.os.setenv
     * C primitives (ray_getenv_fn/ray_setenv_fn) via wrappers that coerce the
     * q symbol arg to the -RAY_STR the C wants — a real divergence, so QR_FN
     * wrappers, not QR_ENV renames. */
    { "getenv", QLEX_KW_PREFIX, QR_FN1("getenv", q_getenv_wrap), QR_NONE,         NULL, 1, 0, "none", NULL },
    { "setenv", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("setenv", q_setenv_wrap), NULL, 1, 1, "none", NULL },
    /* ---- adverbs — native q_eval/apply-module arms; the maps (`'` `':`
     * `/:` `\:`) landed with stage 1, `\` scan is still 'nyi.  `':` is
     * variadic: Each Prior on a rank-2 value, Each Parallel on a rank-1 one
     * (ref/maps.md), and the cell names the dyadic reading. ---- */
    { "'",     QLEX_ADVERB,    QR_NONE,  QR_NONE,  "map",       1, 0, "none", NULL },
    { "/",     QLEX_ADVERB,    QR_NONE,  QR_NONE,  "fold",      1, 0, "none", NULL },
    { "\\",    QLEX_ADVERB,    QR_NONE,  QR_NONE,  "scan",      1, 0, "none", NULL },
    { "':",    QLEX_ADVERB,    QR_NONE,  QR_NONE,  "prior",     1, 0, "none", NULL },
    { "/:",    QLEX_ADVERB,    QR_NONE,  QR_NONE,  "map-right", 1, 0, "none", NULL },
    { "\\:",   QLEX_ADVERB,    QR_NONE,  QR_NONE,  "map-left",  1, 0, "none", NULL },
};
#pragma GCC diagnostic pop
#define N_Q_OPS ((int)(sizeof Q_OPS / sizeof Q_OPS[0]))

/* The identity elements q knows (ref/accumulators.md:261-267 unary-seed,
 * 420-428 empty-Over; ref/maps.md:259-272 the Each Prior seed): the manifest
 * owns this per-verb knowledge, beside the rows it annotates — never a
 * spelling switch in a wrapper (rule 3). */
ray_t* q_ops_acc_identity(const char* spelling) {
    if (!spelling || !spelling[0] || spelling[1] != '\0') return NULL;
    switch (spelling[0]) {
    case '+': return ray_i64(0);
    case '-': return ray_i64(0);
    case '*': return ray_i64(1);
    case ',': return ray_list_new(0);
    default:  return NULL;
    }
}

const q_op_t* q_ops_table(int* n) {
    if (n) *n = N_Q_OPS;
    return Q_OPS;
}

int q_lex_is_kw_infix(const char* s, int len) {
    for (int i = 0; i < N_Q_OPS; i++) {
        if (Q_OPS[i].lex != QLEX_KW_INFIX) continue;
        const char* nm = Q_OPS[i].name;
        if ((int)strlen(nm) == len && memcmp(nm, s, (size_t)len) == 0)
            return 1;
    }
    return 0;
}

/* spelling -> row, via a lazily built hash over the STATIC Q_OPS[] names —
 * derived data that can never go stale, so no invalidation exists. */
#define OPS_IDX_SLOTS 512   /* power of two; load <= 1/2 at the row cap */
static int16_t ops_idx[OPS_IDX_SLOTS];
static int     ops_idx_ready;

static uint32_t ops_hash(const char* s, int len) {
    uint32_t h = 2166136261u;                 /* FNV-1a */
    for (int i = 0; i < len; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

const q_op_t* q_ops_find(const char* s, int len) {
    if (!ops_idx_ready) {
        assert(N_Q_OPS <= OPS_IDX_SLOTS / 2);
        memset(ops_idx, 0xFF, sizeof ops_idx);   /* -1 fill */
        for (int i = 0; i < N_Q_OPS; i++) {
            uint32_t j = ops_hash(Q_OPS[i].name, (int)strlen(Q_OPS[i].name));
            while (ops_idx[j & (OPS_IDX_SLOTS - 1)] >= 0) j++;
            ops_idx[j & (OPS_IDX_SLOTS - 1)] = (int16_t)i;
        }
        ops_idx_ready = 1;
    }
    for (uint32_t j = ops_hash(s, len);; j++) {
        int16_t k = ops_idx[j & (OPS_IDX_SLOTS - 1)];
        if (k < 0) return NULL;
        const char* nm = Q_OPS[k].name;
        if ((int)strlen(nm) == len && memcmp(nm, s, (size_t)len) == 0)
            return &Q_OPS[k];
    }
}

int q_ops_is_reserved(const char* s, int len) { return q_ops_find(s, len) != NULL; }

const q_op_t* q_ops_nested_dyad(const q_op_t* row) {
    /* QNEST_MEAN is the `+` fold scaled by the count, so it borrows sum's dyad */
    const char* agg = !row ? NULL
                    : row->nested == QNEST_FOLD ? row->name
                    : row->nested == QNEST_MEAN ? "sum"
                                                : NULL;
    if (!agg) return NULL;
    for (int i = 0; i < N_Q_OPS; i++)
        if (Q_OPS[i].mono && strcmp(Q_OPS[i].mono, agg) == 0) return &Q_OPS[i];
    return NULL;
}

/* Operator->integer opcode (`value +` -> 1) is DEFERRED: the opcode is a property
 * of the FUNCTION IDENTITY, not the spelling (`get`/`value` are two names for ONE
 * function, kdb `.:`), so a name-keyed table can't model it.  See PLAN.md; the doc
 * rows are pinned red-below-floor in test/q/function/value.qcmd. */
