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
 *   - the GLYPH monadic cells ARE the k unary forms, rostered verbatim in
 *     basics/exposed-infrastructure.md "Unary forms" (`!:` key/til, `#:` count,
 *     `$:` string, `%:` reciprocal, `&:` where, `*:` first, `+:` flip, `,:`
 *     enlist, `-:` neg, `.:` get, `0::` read0, `1::` read1, `<:` iasc, `=:`
 *     group, `>:` idesc, `?:` distinct, `@:` type, `^:` null, `_:` floor, `|:`
 *     reverse, `~:` not).  Each REUSES the keyword's own build — never a second
 *     implementation of the verb.
 * DEFERRED (QR_NONE — no clean rayfall target, do NOT guess):
 *   Dyadic `&`/`|` are the q_min2_wrap/q_max2_wrap
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
#include "qlang/eval/q_view.h"              /* q_view_wrap — the `view` row */
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
 * per-position values (sums, fills, deltas, m-windows) are `map` (L2).  The
 * map family's rank-2 law is SINGLE — `flip f each flip x`, the outer-axis
 * scan (ref/sum.md sums list-of-lists, ref/max.md maxs over a dict), uniform
 * across the whole roster — so unlike `aggregate` (QNEST_*) it needs no
 * manifest column.
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
 * rlike -> `none` for the same reason like is: a string IS a char vector, so no
 * lift law can tell one subject from a collection of them, and the verb owns its
 * own iteration (a string-boundary exception-catalogue member, q_regex.c).
 * Self-distributing exception: `$` is family `none` because it is MULTI-CONCEPT
 * (Cast/Tok/Pad/mmu/enum) — one glyph, no one lift law.  It receives WHOLE args
 * and each overload recurses with its OWN boundary (Cast leaves at typed
 * vectors, Tok/Pad at strings); an exception-catalogue member, its container
 * probes live in q_dollar.c (see tools/lint-type-axis.sh BOTTOM_BAND).
 * ========================================================================== */

/* trailing name_lift zero-defaults on unflagged rows (no 166-row churn) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
/* QKOP(n): the row's kdb primitive code, stored +1 so the C zero-default is
 * "none" without 166-row churn (`:` really is code 0).  Transcribed from
 * javakdb c.java UnaryOperator/BinaryOperator (Apache-2.0, Copyright (c)
 * 1998-2017 Kx Systems Inc., the cleared reference) — array index IS the
 * 101h/102h wire code.  The two tables coincide for 0..22 (the unary spelling
 * is the `:`-suffixed glyph) and their keyword tails are disjoint, so ONE
 * column serves both valences.
 * THE KEYWORD-COVER RULE (uniform over BOTH valences): a q keyword that IS a k
 * primitive under another spelling carries that primitive's code.  Unary covers
 * per basics/exposed-infrastructure.md "Unary forms" + ref/value.md (`value
 * each (get;value)` -> 19 19): neg -:, count #:, key/til !:, get/value .:, ...
 * Dyadic covers per the ref page that names them SYNONYMS: `and` IS `&`
 * (ref/lesser.md "Lesser and `and` are synonyms") and `or` IS `|`
 * (ref/greater.md).  A keyword the docs call a WRAPPER for an operator rather
 * than its synonym (mmu for `$`, ref/mmu.md) is a distinct value and stays
 * unset; so does a same-page keyword with its own semantics (`cut` vs `_`).
 * Rows without a code (peachq spellings, <=/>=/<>) stay unset: the numbering is
 * KX's frozen constant, never allocated into. */
#define QKOP(n) .kdb_op1 = (int8_t)((n) + 1)
/* Doc citations below name only the NON-derivable ones: unless a comment says
 * otherwise, a row's reference page is ref/<its own name>.md. */
static const q_op_t Q_OPS[] = {
    /* name    lex            monadic-recipe             dyadic-recipe            hof  det eff family mono (mono_scan designated) */
    /* ---- arithmetic / compare glyphs: dyads atomic (ref/{add,subtract,
     * multiply,divide,greater-than,less-than,equal,not-equal}.md) ---- */
    /* monadic `+` is flip, a K-ism superset (the `_`/floor precedent);
     * family is the dyadic Add. */
    { "+",     QLEX_GLYPH,     QR_FN1("flip", q_flip_wrap),    QR_ENV("+"),       NULL, 1, 0, "atomic", "sum", .mono_scan = "sums", QKOP(1) },
    { "-",     QLEX_GLYPH,     QR_FN1A("neg", q_neg_wrap),     QR_ENV("-"),       NULL, 1, 0, "atomic", NULL, QKOP(2) },
    { "*",     QLEX_GLYPH,     QR_FN1("first", q_first_wrap),  QR_ENV("*"),       NULL, 1, 0, "atomic", "prd", .mono_scan = "prds", QKOP(3) },
    { "%",     QLEX_GLYPH,     QR_QSRC("reciprocal"),          QR_ENV("/"),       NULL, 1, 0, "atomic", NULL, QKOP(4) },
    { "<",     QLEX_GLYPH,     QR_FN1("iasc", q_iasc_wrap),    QR_ENV("<"),       NULL, 1, 0, "atomic", NULL, QKOP(9) },
    { ">",     QLEX_GLYPH,     QR_FN1("idesc", q_idesc_wrap),  QR_ENV(">"),       NULL, 1, 0, "atomic", NULL, QKOP(10) },
    { "<=",    QLEX_GLYPH,     QR_NONE,                        QR_ENV("<="),      NULL, 1, 0, "atomic", NULL },
    { ">=",    QLEX_GLYPH,     QR_NONE,                        QR_ENV(">="),      NULL, 1, 0, "atomic", NULL },
    { "=",     QLEX_GLYPH,     QR_FN1("group", q_group_wrap),  QR_FN2("==", q_eq_wrap) , NULL, 1, 0, "atomic", NULL, QKOP(8) },
    { "<>",    QLEX_GLYPH,     QR_NONE,                        QR_FN2("!=", q_ne_wrap) , NULL, 1, 0, "atomic", NULL },
    /* ---- structural glyphs ---- */
    /* `#`/`_` dyads are family `none`: the left arg SELECTS the arm, so a
     * container must reach the wrapper whole (FAMILY AUDIT). */
    { "#",     QLEX_GLYPH,     QR_ENV("count"),                QR_FN2("take", q_take_wrap), NULL, 1, 0, "none", NULL, QKOP(13) },
    /* monadic `_` is a K-ism superset; the VALUE is kdb-identical (kdb's
     * own floor IS k `_:`). */
    { "_",     QLEX_GLYPH,     QR_FN1A("floor", q_floor_wrap), QR_FN2("drop", q_drop_wrap), NULL, 1, 0, "none", NULL, QKOP(14) },
    { "|",     QLEX_GLYPH,     QR_ENV("reverse"),              QR_FN2A("or", q_max2_wrap), NULL, 1, 0, "atomic", "max", .mono_scan = "maxs", QKOP(6) },
    { "&",     QLEX_GLYPH,     QR_FN1("where", q_where_wrap),  QR_FN2A("and", q_min2_wrap), NULL, 1, 0, "atomic", "min", .mono_scan = "mins", QKOP(5) },
    { ",",     QLEX_GLYPH,     QR_ENV("enlist"),               QR_FN2("concat", q_join_wrap), NULL, 1, 0, "structural", "raze", QKOP(12) },
    /* `~` monadic not is ATOMIC on its own recipe, since `family` speaks for
     * the DYAD; Match reduces to one bool atom (near-border, see AUDIT). */
    { "~",     QLEX_GLYPH,     QR_FN1A("not", q_not_wrap),     QR_FN2("match", q_match_wrap), NULL, 1, 0, "aggregate", NULL, QKOP(15) },
    /* `^` fill: "Fill is an atomic function" (ref/fill.md). */
    { "^",     QLEX_GLYPH,     QR_FN1("nil?", q_null_wrap),    QR_FN2("fill", q_fill_wrap), NULL, 1, 0, "atomic", NULL, QKOP(7) },
    /* ---- type-dispatch glyphs ---- */
    /* `!` family "structural" is DOCUMENTATION ONLY — no lift arm dispatches
     * on it. Monadic `!` is a K-ism superset of `key`. */
    { "!",     QLEX_GLYPH,     QR_FN1("key", q_key_wrap),      QR_FNV("dict", q_funsql_bang_wrap), NULL, 1, 0, "structural", NULL, QKOP(16) },
    /* `?` family "index" is KEPT OPERATIVE: the n==2 container lift (`-3?t`)
     * reads it before the kernel runs. Dict entries-axis collision: spec §9.1. */
    { "?",     QLEX_GLYPH,     QR_FN1("distinct", q_distinct_wrap), QR_FNV("rand", q_funsql_ques_wrap), NULL, 0, 0, "index", NULL, QKOP(17) },
    /* `$` is MULTI-CONCEPT (Cast/Tok/Pad/mmu/enum) so family "none": no one
     * lift law fits, so each overload SELF-RECURSES with its own boundary
     * (ruling 2026-07-24). `$[c;t;f]` is a q_eval control form, not a row. */
    { "$",     QLEX_GLYPH,     QR_ENV("string"),               QR_FN2("as", q_dollar), NULL, 1, 0, "none", NULL, QKOP(11) },
    /* `:` Assign as an operand VALUE. Assignment/return stay SYNTAX — q_embed
     * never embeds a colon head, so this row is reachable only as an operand. */
    { ":",     QLEX_GLYPH,     QR_NONE,                        QR_FN2("assign", q_index_assign_wrap), NULL, 1, 0, "none", NULL, QKOP(0) },
    /* `@`/`.` family none: Apply/Index ARE the application machinery (spec §5),
     * not lifted. name_lift: a sym-atom d in the amend forms names a global. */
    { "@",     QLEX_GLYPH,     QR_ENV("type"),                 QR_FNV("at", q_eval_at_wrap),    NULL, 1, 1, "none", NULL, .name_lift = 1, QKOP(18) },
    { ".",     QLEX_GLYPH,     QR_FN1("value", q_eval_value_wrap), QR_FNV("apply", q_eval_dot_wrap), NULL, 1, 1, "none", NULL, .name_lift = 1, QKOP(19) },
    /* ---- keyword-infix ---- */
    { "div",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("div"),     NULL, 1, 0, "atomic", NULL, QKOP(31) },
    /* PURE RENAME: rayfall `%` IS floored modulo, sign following the divisor,
     * exactly kdb mod (audited: -3 -2 mod 3 -> 0 1; 7 mod -2 -> -1). */
    { "mod",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("%"),       NULL, 1, 0, "atomic", NULL },
    /* ref/exp.md, ref/log.md. */
    { "xexp",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("xexp", q_xexp_wrap), NULL, 1, 0, "atomic", NULL, QKOP(32) },
    { "xlog",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("xlog", q_xlog_wrap), NULL, 1, 0, "atomic", NULL },
    { "each",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("map", q_hof_nyi_wrap), "map", 1, 0, "none", NULL },
    /* rowid: `in` needs item equality (a set-op predicate). */
    { "in",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("in", q_in_wrap), NULL, 1, 0, "rowid", NULL, QKOP(23) },
    /* Monadic cells stay QR_NONE: both are dyadic-only in q, so prefix `and x`
     * misses and eval falls through to rayfall's scalar special form. */
    { "and",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("and", q_min2_wrap), NULL, 1, 0, "atomic", NULL, QKOP(5) },
    { "or",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("or", q_max2_wrap), NULL, 1, 0, "atomic", NULL, QKOP(6) },
    /* `within` y is BOUNDS, not an operand conforming to x, so no lift law
     * fits: family `none`, wrapper takes whole args (border ruling: AUDIT). */
    { "within",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("within", q_within_wrap), NULL, 1, 0, "none", NULL, QKOP(24) },
    { "cut",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("cut", q_cut_wrap), NULL, 1, 0, "index", NULL },
    /* KW_INFIX is load-bearing: without it the scanner splits `n rotate x` into
     * two statements. rotate is a WRAPPER, not the q.q derivation it once was —
     * a QSRC lambda short-circuits family dispatch, so only a manifest value
     * reaches the index lift. */
    { "rotate",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("rotate", q_rotate_wrap), NULL, 1, 0, "index", NULL },
    { "sublist",QLEX_KW_INFIX, QR_NONE,                        QR_QSRC("sublist"), NULL, 1, 0, "index", NULL },
    /* monadic vs/sv are out of scope (kdb spells them strictly dyadic). */
    { "vs",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("vs", q_vs_wrap), NULL, 1, 0, "none", NULL },
    { "sv",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("sv", q_sv_wrap), NULL, 1, 0, "none", NULL },
    /* ---- set operations: rowid (item identity, spec §3) ---- */
    { "except",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("except", q_except_wrap), NULL, 1, 0, "rowid", NULL },
    /* WRAPPER, not a rename: rayfall union keeps x-duplicates, kdb dedups. */
    { "union", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("union", q_union_wrap), NULL, 1, 0, "rowid", NULL },
    /* WRAPPER over rayfall `sect`: guards dict/table operands 'nyi (sect returns
     * a wrong-shaped dict there). */
    { "inter", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("sect", q_inter_wrap), NULL, 1, 0, "rowid", NULL },
    { "cross", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("cross", q_cross_wrap), NULL, 1, 0, "structural", NULL },
    /* ---- q join family: structural (schema-defining; border ruling in the
     * AUDIT) ---- */
    { "lj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("lj", q_lj_wrap),   NULL, 1, 0, "structural", NULL },
    { "ljf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ljf", q_ljf_wrap), NULL, 1, 0, "structural", NULL },
    { "ij",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ij", q_ij_wrap),   NULL, 1, 0, "structural", NULL },
    { "ijf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ijf", q_ijf_wrap), NULL, 1, 0, "structural", NULL },
    { "uj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("uj", q_uj_wrap),   NULL, 1, 0, "structural", NULL },
    { "ujf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ujf", q_ujf_wrap), NULL, 1, 0, "structural", NULL },
    { "pj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("pj", q_pj_wrap),   NULL, 1, 0, "structural", NULL },
    { "asof",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("asof", q_asof_wrap), NULL, 1, 0, "structural", NULL },
    /* Bracket-form joins are q-owned env VARY bindings snapshotted as QR_ENV:
     * the parser name-refs `ej[a;b;c]`, so it resolves through the env. */
    { "ej",    QLEX_KW_PREFIX, QR_ENV("ej"),                   QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "aj",    QLEX_KW_PREFIX, QR_ENV("aj"),                   QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "aj0",   QLEX_KW_PREFIX, QR_ENV("aj0"),                  QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "ajf",   QLEX_KW_PREFIX, QR_ENV("ajf"),                  QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "ajf0",  QLEX_KW_PREFIX, QR_ENV("ajf0"),                 QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "wj",    QLEX_KW_PREFIX, QR_ENV("wj"),                   QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "wj1",   QLEX_KW_PREFIX, QR_ENV("wj1"),                  QR_NONE,           NULL, 1, 0, "structural", NULL },
    /* ---- sort / bucket ---- ordering-based search puts bin/binr/xrank in
     * rowid. `xbar` is an ARG-SWAP wrapper (rayfall takes (col,bucket), q
     * spells it (bucket,col)). All infix so `a verb b` is one statement. */
    { "bin",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("bin", q_bin_wrap),   NULL, 1, 0, "rowid", NULL, QKOP(26) },
    { "binr",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("binr", q_binr_wrap), NULL, 1, 0, "rowid", NULL, QKOP(34) },
    { "xrank", QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("xrank"),  NULL, 1, 0, "rowid", NULL },
    { "xbar",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("xbar", q_xbar_wrap), NULL, 1, 0, "atomic", NULL },
    /* ---- running scans ---- `ratios` is a border case: its page's LABEL says
     * aggregate, its documented behaviour is uniform (FAMILY AUDIT). */
    { "sums",  QLEX_KW_PREFIX, QR_FN1("sums", q_sums_wrap),    QR_NONE,           NULL, 1, 0, "map", NULL },
    { "prds",  QLEX_KW_PREFIX, QR_FN1("prds", q_prds_wrap),    QR_NONE,           NULL, 1, 0, "map", NULL },
    /* QNEST_COLUMNS, not QNEST_FOLD: basics/math.md's null-preserving roster is
     * `avg min max sum` — prd keeps nulls-as-1s nested (`prd (1 0N;2 3)` is
     * `2 3`), which is the per-column law. */
    { "prd",   QLEX_KW_PREFIX, QR_FN1("prd", q_prd_wrap),      QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_COLUMNS, QKOP(26) },
    { "maxs",  QLEX_KW_PREFIX, QR_FN1("maxs", q_maxs_wrap),    QR_NONE,           NULL, 1, 0, "map", NULL },
    { "mins",  QLEX_KW_PREFIX, QR_FN1("mins", q_mins_wrap),    QR_NONE,           NULL, 1, 0, "map", NULL },
    { "avgs",  QLEX_KW_PREFIX, QR_FN1("avgs", q_avgs_wrap),    QR_NONE,           NULL, 1, 0, "map", NULL },
    { "ratios",QLEX_KW_PREFIX, QR_FN1("ratios", q_ratios_wrap),QR_NONE,           NULL, 1, 0, "map", NULL },
    /* ---- weighted ---- wavg stays a wrapper: its q.q composition's
     * bool-multiply path measured 16x slower, so the structure that
     * composition would have given it is DECLARED: on a DYAD QNEST_COLUMNS is
     * agg2's column zip.  `cor` declares none — basics/math.md excludes it
     * outright, where wavg/wsum are excluded for TABLES only. */
    { "wsum",  QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("wsum"),   NULL, 1, 0, "aggregate", NULL, QKOP(29) },
    { "wavg",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("wavg", q_wavg_wrap), NULL, 1, 0, "aggregate", NULL, .nested = QNEST_COLUMNS, QKOP(30) },
    /* ---- statistical ---- kdb `var`/`dev` are POPULATION (/n); rayfall
     * `var`/`stddev` are SAMPLE (/n-1) and `var_pop`/`stddev_pop`/`dev` are
     * population — hence q `var`->`var_pop`, `svar`->`var`, `sdev`->`stddev`.
     * `med` is q.q-hosted and takes NO rank-2 sub-law: ref/med.md's `med t` is
     * the middle ITEM, not the per-column median `each flip` would give. */
    { "var",   QLEX_KW_PREFIX, QR_ENV("var_pop"),              QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_COLUMNS, QKOP(42) },
    { "svar",  QLEX_KW_PREFIX, QR_ENV("var"),                  QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_COLUMNS },
    { "sdev",  QLEX_KW_PREFIX, QR_ENV("stddev"),               QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_COLUMNS },
    { "cor",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("pearson_corr"), NULL, 1, 0, "aggregate", NULL, QKOP(36) },
    { "cov",   QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("cov"),    NULL, 1, 0, "aggregate", NULL, QKOP(35) },
    { "scov",  QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("scov"),   NULL, 1, 0, "aggregate", NULL },
    /* fby: family `none` — the LEFT arg is the PAIR `(aggr;d)`, which must reach
     * the derivation whole; q.q-hosted because ref/fby.md prints the spelling. */
    { "fby",   QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("fby"),    NULL, 1, 0, "none", NULL },
    /* ---- sliding m-windows + ema ---- */
    { "msum",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("msum", q_msum_wrap), NULL, 1, 0, "map", NULL },
    { "mavg",  QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("mavg"),   NULL, 1, 0, "map", NULL },
    { "mmax",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mmax", q_mmax_wrap), NULL, 1, 0, "map", NULL },
    { "mmin",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mmin", q_mmin_wrap), NULL, 1, 0, "map", NULL },
    { "mcount",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mcount", q_mcount_wrap), NULL, 1, 0, "map", NULL },
    { "mdev",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mdev", q_mdev_wrap), NULL, 1, 0, "map", NULL },
    { "ema",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ema", q_ema_wrap), NULL, 1, 0, "map", NULL },
    /* mmu owns its own shape logic (family none, like `@`/`.`). */
    { "mmu",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mmu", q_mmu_wrap), NULL, 1, 0, "none", NULL },
    { "over",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("over", q_hof_nyi_wrap), "fold", 1, 0, "none", NULL },
    { "scan",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("scan-kw", q_hof_nyi_wrap), "scan", 1, 0, "none", NULL },
    { "prior", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("prior", q_hof_nyi_wrap), "prior", 1, 0, "none", NULL },
    { "peach", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("peach", q_hof_nyi_wrap), "map", 1, 0, "none", NULL },
    /* ---- keyword-prefix monads (pass-through/rename) ---- */
    { "neg",     QLEX_KW_PREFIX, QR_FN1A("neg", q_neg_wrap),   QR_NONE,           NULL, 1, 0, "atomic", NULL, QKOP(2) },
    /* `til` takes no structure input (family none); kdb accepts a boolean
     * (`til 1b` -> ,0) where base ray_til_fn is int-only. */
    { "til",     QLEX_KW_PREFIX, QR_FN1("til", q_til_wrap),    QR_NONE,           NULL, 1, 0, "none", NULL, QKOP(16) },
    { "count",   QLEX_KW_PREFIX, QR_ENV("count"),              QR_NONE,           NULL, 1, 0, "none",      NULL, QKOP(13) },
    { "first",   QLEX_KW_PREFIX, QR_FN1("first", q_first_wrap),QR_NONE,           NULL, 1, 0, "aggregate", NULL, QKOP(3) },
    { "last",    QLEX_KW_PREFIX, QR_FN1("last", q_last_wrap),  QR_NONE,           NULL, 1, 0, "aggregate", NULL, QKOP(24) },
    /* L4 index: a shift IS a gather by shifted indices (spec §2). `next`/`prev`
     * are its unit shifts, self-hosted in q.q — no rows. */
    { "xprev",   QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("xprev", q_xprev_wrap), NULL, 1, 0, "index", NULL },
    /* `fills` computes values rather than gathering -> map. */
    { "fills",   QLEX_KW_PREFIX, QR_FN1("fills", q_fills_wrap), QR_NONE,          NULL, 1, 0, "map", NULL },
    /* `where` is an index-space generator but NOT the L4 one: its entries index
     * comes from `where value d`, never `til count d`, so the positional lift
     * would silently mis-answer it. family none until the values-derived arm. */
    { "where",   QLEX_KW_PREFIX, QR_FN1("where", q_where_wrap), QR_NONE,          NULL, 1, 0, "none", NULL, QKOP(5) },
    { "reverse", QLEX_KW_PREFIX, QR_ENV("reverse"),            QR_NONE,           NULL, 1, 0, "index", NULL, QKOP(6) },
    /* NB deliberately NOT RAY_FN_AGGR: the eval aggregate fast path claims AGGR
     * fns before the wrapper runs and 'types on a boxed list-of-vectors.
     * Name-routing (RAY_FN_Q_LOWER + aux "sum") keeps query/DAG behaviour. */
    { "sum",     QLEX_KW_PREFIX, QR_FN1("sum", q_sum_wrap),    QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_FOLD, QKOP(25) },
    { "group",   QLEX_KW_PREFIX, QR_FN1("group", q_group_wrap), QR_NONE,          NULL, 1, 0, "rowid", NULL, QKOP(8) },
    /* ---- grade: THE ordering primitive; rowid (spec §3, the #174 stable-grade
     * kernel) ---- Direction is a kernel flag, NOT `reverse iasc`: ties must
     * not reverse (`idesc 2 1 2` = `0 2 1`). asc/desc/rank are q.q derivations
     * carrying NO row. The `s#` attribute stays a divergence: the attr-take arm
     * accepts long vectors only, so asc cannot set it without regressing syms. */
    { "iasc",    QLEX_KW_PREFIX, QR_FN1("iasc", q_iasc_wrap),  QR_NONE,           NULL, 1, 0, "rowid", NULL, QKOP(9) },
    { "idesc",   QLEX_KW_PREFIX, QR_FN1("idesc", q_idesc_wrap), QR_NONE,          NULL, 1, 0, "rowid", NULL, QKOP(10) },
    { "avg",     QLEX_KW_PREFIX, QR_FN1("avg", q_avg_wrap),    QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_MEAN, QKOP(23) },
    { "floor",   QLEX_KW_PREFIX, QR_FN1A("floor", q_floor_wrap), QR_NONE,         NULL, 1, 0, "atomic", NULL, QKOP(14) },
    /* `distinct` must preserve FIRST-OCCURRENCE order (ref/distinct.md) where
     * rayfall's routes typed vectors through the sorting DAG group path —
     * audited: `distinct 2 3 7 3 5 3` is 2 3 7 5, env distinct gives 2 3 5 7. */
    { "key",     QLEX_KW_PREFIX, QR_FN1("key", q_key_wrap),    QR_NONE,           NULL, 1, 0, "structural", NULL, QKOP(16) },
    /* `tables`/`views` are q.q-hosted: the row reserves the name and fixes the
     * lexical class, the VALUE comes from `.q`. The argument is a namespace
     * REFERENCE, never data to lift over. */
    { "tables",  QLEX_KW_PREFIX, QR_NONE,                      QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "views",   QLEX_KW_PREFIX, QR_NONE,                      QR_NONE,           NULL, 1, 0, "structural", NULL },
    /* `view` takes a view by REFERENCE (ref/view.md) — never data to lift. */
    { "view",    QLEX_KW_PREFIX, QR_FN1("view", q_view_wrap),  QR_NONE,           NULL, 1, 0, "none", NULL },
    { "value",   QLEX_KW_PREFIX, QR_FN1("value", q_eval_value_wrap), QR_NONE,          NULL, 1, 0, "structural", NULL, QKOP(19) },
    /* `get` is a SYNONYM of `value` (ref/get.md) — one home. `eval` has NO row:
     * q.q-hosted over its internal (.q.eval:(-6!)). */
    { "get",     QLEX_KW_PREFIX, QR_FN1("value", q_eval_value_wrap), QR_NONE,          NULL, 1, 0, "structural", NULL, QKOP(19) },
    { "set",     QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("set-g", q_setg_wrap), NULL, 1, 1, "none", NULL },
    { "distinct",QLEX_KW_PREFIX, QR_FN1("distinct", q_distinct_wrap), QR_NONE,    NULL, 1, 0, "rowid", NULL, QKOP(17) },
    /* `rand` is q.q-hosted with no row; the bootstrap shadow-rebind points root
     * `rand` at `.q.rand` (rayfall's env `rand` is dyadic and would win). */
    /* string/upper/lower are atomic at STRING granularity (FAMILY AUDIT caveat). */
    { "string",  QLEX_KW_PREFIX, QR_ENV("string"),             QR_NONE,           NULL, 1, 0, "atomic", NULL, QKOP(11) },
    { "upper",   QLEX_KW_PREFIX, QR_ENV("upper"),              QR_NONE,           NULL, 1, 0, "atomic", NULL },
    { "lower",   QLEX_KW_PREFIX, QR_ENV("lower"),              QR_NONE,           NULL, 1, 0, "atomic", NULL },
    /* ---- string trim / hash / search ---- trim stays C: its q.q form
     * {ltrim rtrim x} measured 3.7x on a 1M string list. `md5` has NO row —
     * it is `.q.md5:(-15!)` reached through the `!` internal-fn arm. */
    { "trim",    QLEX_KW_PREFIX, QR_ENV("trim"),               QR_NONE,           NULL, 1, 0, "none", NULL },
    { "ltrim",   QLEX_KW_PREFIX, QR_ENV("ltrim"),              QR_NONE,           NULL, 1, 0, "none", NULL },
    { "rtrim",   QLEX_KW_PREFIX, QR_ENV("rtrim"),              QR_NONE,           NULL, 1, 0, "none", NULL },
    { "like",    QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("like", q_like_wrap), NULL, 1, 0, "none", NULL, QKOP(25) },
    { "ss",      QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("ss", q_ss_wrap), NULL, 1, 0, "none", NULL, QKOP(27) },
    { "ssr",     QLEX_KW_PREFIX, QR_ENV("ssr"),                QR_NONE,           NULL, 1, 0, "none", NULL },
    /* a peachq extension, so no kdb primitive code: RE2 partial match
     * (user-docs/regexp.md).  `.regexp.*` is stdlib q and has no row. */
    { "rlike",   QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("rlike", q_rlike_wrap), NULL, 1, 0, "none", NULL },
    { "show",    QLEX_KW_PREFIX, QR_ENV("show"),               QR_NONE,           NULL, 1, 1, "none", NULL },
    { "system",  QLEX_KW_PREFIX, QR_ENV("system"),             QR_NONE,           NULL, 1, 1, "none", NULL },
    /* `exit` is capability-gated so doctest/wasm runtimes survive it. */
    { "exit",    QLEX_KW_PREFIX, QR_FN1("exit", q_exit_wrap), QR_NONE,           NULL, 1, 1, "none", NULL, QKOP(29) },
    { "meta",    QLEX_KW_PREFIX, QR_ENV("meta"),               QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "cols",    QLEX_KW_PREFIX, QR_ENV("cols"),               QR_NONE,           NULL, 1, 0, "structural", NULL },
    /* The attribute SET/CLEAR side is the `#` verb's symbol-atom arm, not a row. */
    { "attr",    QLEX_KW_PREFIX, QR_FN1("attr", q_attr_wrap),  QR_NONE,           NULL, 1, 0, "structural", NULL },
    /* ---- monadic pass-throughs (rayfall name == q name, audited kdb-true) ---- */
    { "abs",     QLEX_KW_PREFIX, QR_ENV("abs"),                QR_NONE,           NULL, 1, 0, "atomic", NULL, QKOP(31) },
    /* `null` routes to the engine's atomic `nil?`; the wrapper collapses a
     * homogeneous top-level bool-atom run for kdb-true display. */
    { "null",    QLEX_KW_PREFIX, QR_FN1("nil?", q_null_wrap),  QR_NONE,           NULL, 1, 0, "atomic", NULL, QKOP(7) },
    { "dev",     QLEX_KW_PREFIX, QR_ENV("dev"),                QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_COLUMNS, QKOP(43) },
    { "exp",     QLEX_KW_PREFIX, QR_ENV("exp"),                QR_NONE,           NULL, 1, 0, "atomic", NULL, QKOP(34) },
    { "log",     QLEX_KW_PREFIX, QR_ENV("log"),                QR_NONE,           NULL, 1, 0, "atomic", NULL, QKOP(33) },
    { "max",     QLEX_KW_PREFIX, QR_ENV("max"),                QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_FOLD, QKOP(28) },
    { "min",     QLEX_KW_PREFIX, QR_ENV("min"),                QR_NONE,           NULL, 1, 0, "aggregate", NULL, .nested = QNEST_FOLD, QKOP(27) },
    /* rank == iasc iasc (ref/rank.md) — the grade family, rowid. */
    /* `raze` adds the kdb atom arm (`raze 42` -> ,42) over base ray_raze_fn. */
    { "raze",    QLEX_KW_PREFIX, QR_FN1("raze", q_raze_wrap),  QR_NONE,           NULL, 1, 0, "structural", NULL },
    /* `enlist` is its OWN unary primitive 41, NOT a keyword cover for `,:` (12)
     * — owner ruling 2026-07-30.  Its value IS the paren-literal ctor head, so
     * the recipe must build a UNIQUE object: reusing `,`-monadic's env snapshot
     * would alias both rows out of q_registry_row_of and cost BOTH their codes. */
    { "enlist",  QLEX_KW_PREFIX, QR_FNV("enlist", q_enlist_wrap), QR_NONE,        NULL, 1, 0, "structural", NULL, QKOP(41) },
    { "sqrt",    QLEX_KW_PREFIX, QR_ENV("sqrt"),               QR_NONE,           NULL, 1, 0, "atomic", NULL, QKOP(32) },
    /* ---- atomic unary math: implement-via-libm, no rayfall counterpart ----
     * `ceiling` is the floor-wrapper twin (float->LONG), NOT rayfall `ceil`. */
    { "sin",       QLEX_KW_PREFIX, QR_FN1A("sin", q_sin_wrap),   QR_NONE, NULL, 1, 0, "atomic", NULL, QKOP(35) },
    { "cos",       QLEX_KW_PREFIX, QR_FN1A("cos", q_cos_wrap),   QR_NONE, NULL, 1, 0, "atomic", NULL, QKOP(37) },
    { "tan",       QLEX_KW_PREFIX, QR_FN1A("tan", q_tan_wrap),   QR_NONE, NULL, 1, 0, "atomic", NULL, QKOP(39) },
    { "asin",      QLEX_KW_PREFIX, QR_FN1A("asin", q_asin_wrap), QR_NONE, NULL, 1, 0, "atomic", NULL, QKOP(36) },
    { "acos",      QLEX_KW_PREFIX, QR_FN1A("acos", q_acos_wrap), QR_NONE, NULL, 1, 0, "atomic", NULL, QKOP(38) },
    { "atan",      QLEX_KW_PREFIX, QR_FN1A("atan", q_atan_wrap), QR_NONE, NULL, 1, 0, "atomic", NULL, QKOP(40) },
    /* `reciprocal` is self-hosted in q.q (`.q.reciprocal:%[1;]`) — no row. */
    { "signum",    QLEX_KW_PREFIX, QR_FN1("signum", q_signum_wrap), QR_NONE, NULL, 1, 0, "atomic", NULL },
    { "ceiling",   QLEX_KW_PREFIX, QR_FN1A("ceiling", q_ceiling_wrap), QR_NONE, NULL, 1, 0, "atomic", NULL },
    /* ---- table verbs ---- `insert`/`upsert` intentionally SHADOW the base env
     * special forms of the same name: q semantics differ (by-name targets,
     * keyed collision/update, row-index results), so the registry value is a
     * wrapper, never the env snapshot. xasc/xdesc/xgroup are rowid, the rest
     * structural (spec §3; xgroup is a border ruling in the AUDIT). */
    { "flip",   QLEX_KW_PREFIX, QR_FN1("flip", q_flip_wrap),   QR_NONE,           NULL, 1, 0, "structural", NULL, QKOP(1) },
    { "keys",   QLEX_KW_PREFIX, QR_FN1("keys", q_keys_wrap),   QR_NONE,           NULL, 1, 0, "structural", NULL },
    { "ungroup",QLEX_KW_PREFIX, QR_FN1("ungroup", q_ungroup_wrap), QR_NONE,       NULL, 1, 0, "structural", NULL },
    { "xkey",   QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xkey", q_xkey_wrap), NULL, 1, 0, "structural", NULL },
    { "xcol",   QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xcol"),   NULL, 1, 0, "structural", NULL },
    { "xcols",  QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xcols"),  NULL, 1, 0, "structural", NULL },
    { "xasc",   QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xasc"),   NULL, 1, 0, "rowid", NULL },
    { "xdesc",  QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xdesc"),  NULL, 1, 0, "rowid", NULL },
    { "xgroup", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xgroup", q_xgroup_wrap), NULL, 1, 0, "rowid", NULL },
    { "insert", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("insert", q_insert_wrap), NULL, 1, 1, "structural", NULL, QKOP(28) },
    { "upsert", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("upsert", q_upsert_wrap), NULL, 1, 1, "structural", NULL },
    /* `deltas`/`differ`/`any`/`all` are q.q-hosted; their rows keep the names
     * reserved and fix the lexical class, the VALUE comes from `.q`. NB a
     * valueless KW_INFIX row would expose rayfall's scalar `or`. */
    { "any",     QLEX_KW_PREFIX, QR_NONE,                      QR_NONE,           NULL, 1, 0, "aggregate", NULL },
    { "all",     QLEX_KW_PREFIX, QR_NONE,                      QR_NONE,           NULL, 1, 0, "aggregate", NULL },
    /* ---- IPC client ---- `hopen` returns the connection's socket fd as the
     * handle (kdb-faithful: 0/1/2 reserved, connections at 3+). The send verb
     * `h"query"` is handle-as-verb application, not a manifest row. */
    { "hopen",  QLEX_KW_PREFIX, QR_FN1("hopen", q_hopen_wrap),   QR_NONE, NULL, 1, 1, "none", NULL, QKOP(44) },
    { "hclose", QLEX_KW_PREFIX, QR_FN1("hclose", q_hclose_wrap), QR_NONE, NULL, 1, 1, "none", NULL },
    /* ---- File Text / File Binary ---- both are tokenized by the scanner's
     * digit-colon arm and dispatch on the LEFT operand's shape, so both are
     * family "none" exception-catalogue members. `2:` (Dynamic Load) has no
     * row and stays a name-ref ('name). */
    { "0:",     QLEX_GLYPH,     QR_FN1("read0", q_read0_wrap), QR_FN2("file-text", q_io_filetext_wrap), NULL, 1, 1, "none", NULL, QKOP(20) },
    { "1:",     QLEX_GLYPH,     QR_FN1("read1", q_read1_wrap), QR_FN2("file-binary", q_io_filebinary_wrap), NULL, 1, 1, "none", NULL, QKOP(21) },
    { "hsym",   QLEX_KW_PREFIX, QR_FN1("hsym", q_hsym_wrap),   QR_NONE,           NULL, 1, 0, "atomic", NULL },
    { "read0",  QLEX_KW_PREFIX, QR_FN1("read0", q_read0_wrap), QR_NONE,           NULL, 1, 1, "none", NULL, QKOP(20) },
    { "read1",  QLEX_KW_PREFIX, QR_FN1("read1", q_read1_wrap), QR_NONE,           NULL, 1, 1, "none", NULL, QKOP(21) },
    { "hdel",   QLEX_KW_PREFIX, QR_FN1("hdel", q_hdel_wrap),   QR_NONE,           NULL, 1, 1, "none", NULL },
    /* ---- environment variables ---- `setenv` MUST be KW_INFIX or the scanner
     * splits `x setenv y` into two statements (the `set`/`xkey` precedent).
     * Both coerce the q symbol arg to -RAY_STR: a real divergence, so QR_FN
     * wrappers rather than QR_ENV renames. */
    { "getenv", QLEX_KW_PREFIX, QR_FN1("getenv", q_getenv_wrap), QR_NONE,         NULL, 1, 0, "none", NULL, QKOP(30) },
    { "setenv", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("setenv", q_setenv_wrap), NULL, 1, 1, "none", NULL, QKOP(33) },
    /* ---- adverbs ---- `':` is variadic: Each Prior on a rank-2 value, Each
     * Parallel on a rank-1 one (ref/maps.md); the cell names the dyadic
     * reading. */
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
