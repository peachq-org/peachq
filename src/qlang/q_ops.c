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
 * DEFERRED (QR_NONE — no clean rayfall target, do NOT guess): `|` dyadic
 *   (max: rayfall has NO dyadic element-wise max — `max` is register_unary
 *   AGGREGATE, `and`/`or` are register_vary scalar special forms — so wiring
 *   it needs a q_max2_wrap twin of q_min2_wrap = new logic, HELD), monadic `%`
 *   (reciprocal: no builtin).  Dyadic `&` is the q_min2_wrap recipe
 *   (element-wise min / bool-and) — the `and` keyword reuses it.  The bool/null
 *   batch keywords `any`/`all` are RESERVED-but-deferred (see their rows
 *   below); `or` (|-max keyword) is not rostered until `|`-max lands.
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
 *           the RNG valence makes the verb nondeterministic.
 *   rand  — `rand x` == {first 1?x}, an RNG draw (ref/rand.md).
 *
 * sideeffect = 1 (observable effect beyond the returned value):
 *   set     — assigns a global through a sym handle (ref/get.md `set`).
 *   insert  — appends rows to a NAMED global table by reference (ref/insert.md).
 *   upsert  — named-target upsert rebinds the global (ref/upsert.md).
 *   show    — writes the console display sink (ref/show.md).
 *   system  — runs a `\`-command / shell command (ref/system.md).
 *   hopen   — opens an IPC connection (ref/hopen.md).
 *   hclose  — closes an IPC connection (ref/hopen.md).
 *   0:      — File Text: Save Text writes / Load CSV reads a file (ref/file-text.md).
 *   read0   — reads a file's lines (ref/read0.md).
 *   setenv  — sets a process environment variable (ref/getenv.md).
 * (getenv READS the environment — deterministic given the process env, no
 * mutation — so it is NOT flagged; value/get resolve data purely, the
 * code-executing `value "…"` arm being the caller's effect, not the verb's.)
 * ========================================================================== */

/* ===== FAMILY AUDIT (uniform-structure-dispatch stage 0 metadata) ==========
 * Every row carries a `family` (vocabulary: q_ops.h; spec: actionable-plans/
 * 2026-07-15-uniform-structure-dispatch.md §2/§3).  PURE METADATA — nothing
 * dispatches on it yet.  Classification is per the verb's qdocs page; where
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
 *   where  -> index   (the L4 index-space GENERATOR; dict arm = keys gathered
 *                      by `where value d`, the entries-axis law — ref/where.md)
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
 * String-model caveat: string/upper/lower/trim/ltrim/rtrim/like classify
 * `atomic` at STRING granularity — openq strings are RAY_STR atoms, so the
 * elementwise unit is the whole string (kdb: char lists; ARCHITECTURE.md
 * provisional string model).  md5/ss/ssr consume a whole string as a codec /
 * search domain -> `none`.
 * ========================================================================== */

static const q_op_t Q_OPS[] = {
    /* name    lex            monadic-recipe             dyadic-recipe            hof  det eff family */
    /* ---- arithmetic / compare glyphs — dyadics all doc-labelled atomic
     * (ref/{add,subtract,multiply,divide,greater-than,less-than,equal,
     * not-equal}.md) ---- */
    /* monadic `+` is flip — a k-ism accepted as a deliberate superset (valid q
     * spells it `flip`, same q_flip_wrap — the `_`/floor precedent).  Family
     * is the dyadic Add; monadic flip is structural (the `flip` row). */
    { "+",     QLEX_GLYPH,     QR_FN1("flip", q_flip_wrap),    QR_ENV("+"),       NULL, 1, 0, "atomic" },
    { "-",     QLEX_GLYPH,     QR_FN1A("neg", q_neg_wrap),     QR_ENV("-"),       NULL, 1, 0, "atomic" },
    /* `*` monadic is first (aggregate — the `first` row); family = dyadic. */
    { "*",     QLEX_GLYPH,     QR_ENV("first"),                QR_ENV("*"),       NULL, 1, 0, "atomic" },
    { "%",     QLEX_GLYPH,     QR_NONE,                        QR_ENV("/"),       NULL, 1, 0, "atomic" },
    /* monadic `<`/`>` are grade up/down (rowid — the iasc/idesc rows); shared
     * wrapper adds the DICT arm (keys in value order); family = dyadic. */
    { "<",     QLEX_GLYPH,     QR_FN1("iasc", q_iasc_wrap),    QR_ENV("<"),       NULL, 1, 0, "atomic" },
    { ">",     QLEX_GLYPH,     QR_FN1("idesc", q_idesc_wrap),  QR_ENV(">"),       NULL, 1, 0, "atomic" },
    { "<=",    QLEX_GLYPH,     QR_NONE,                        QR_ENV("<="),      NULL, 1, 0, "atomic" },
    { ">=",    QLEX_GLYPH,     QR_NONE,                        QR_ENV(">="),      NULL, 1, 0, "atomic" },
    /* `=` monadic is group (rowid — the `group` row); family = dyadic Equal. */
    { "=",     QLEX_GLYPH,     QR_ENV("group"),                QR_FN2A("==", q_eq_wrap), NULL, 1, 0, "atomic" },
    { "<>",    QLEX_GLYPH,     QR_NONE,                        QR_FN2A("!=", q_ne_wrap), NULL, 1, 0, "atomic" },
    /* ---- structural glyphs ---- */
    /* `#` monadic is count (aggregate — the `count` row); family = dyadic
     * Take, the L4 pilot op (`-3#t`).  The arg-swap lives in q_take_wrap. */
    { "#",     QLEX_GLYPH,     QR_ENV("count"),                QR_FN2("take", q_take_wrap), NULL, 1, 0, "index" },
    /* monadic `_` is a K-ism (valid q spells it `floor`, atomic) — accepting
     * it is a deliberate SUPERSET of q source; the VALUE is kdb-identical
     * (kdb's own floor IS k `_:`).  Family = dyadic Drop (L4). */
    { "_",     QLEX_GLYPH,     QR_FN1A("floor", q_floor_wrap), QR_FN2("drop", q_drop_wrap), NULL, 1, 0, "index" },
    /* `|` dyadic (max) is a deferred cell (atomic when it lands); family =
     * monadic reverse (L4 — spec §2). */
    { "|",     QLEX_GLYPH,     QR_FN1("reverse", q_reverse_wrap), QR_NONE,        NULL, 1, 0, "index" },
    /* `&` monadic is where (index — the `where` row, a §9 border); family =
     * dyadic Lesser/min (atomic, ref/lesser.md). */
    { "&",     QLEX_GLYPH,     QR_FN1("where", q_where_wrap),  QR_FN2A("and", q_min2_wrap), NULL, 1, 0, "atomic" },
    /* `,` — enlist / join: both construct structure (table,record-dict is
     * upsert semantics — see q_join_wrap). */
    { ",",     QLEX_GLYPH,     QR_ENV("enlist"),               QR_FN2("concat", q_join_wrap), NULL, 1, 0, "structural" },
    /* `~` monadic is not (atomic); family = dyadic Match — reduces to one
     * bool atom (near-border: does NOT follow the L3 value-law; see AUDIT). */
    { "~",     QLEX_GLYPH,     QR_ENV("not"),                  QR_FN2("match", q_match_wrap), NULL, 1, 0, "aggregate" },
    /* q `x^y` — fill: coalesce nulls in y with x ("Fill is an atomic
     * function", ref/fill.md).  `^` already lexes as a verb glyph
     * (VERB_CHARS); this row gives it a registry value.  Monadic `^x`
     * (kdb: null-of-type / `fills` sans forward-fill) is a deferred cell. */
    { "^",     QLEX_GLYPH,     QR_NONE,                        QR_FN2("fill", q_fill_wrap), NULL, 1, 0, "atomic" },
    /* ---- type-dispatch glyphs (2c-2) ---- */
    /* monadic `!` (dict keys) is a K-ism accepted as a deliberate superset
     * (valid q spells it `key`, same value — the `_`/floor precedent). */
    { "!",     QLEX_GLYPH,     QR_FN1("key", q_key_wrap),      QR_FN2("dict", q_bang_wrap), NULL, 1, 0, "structural" },
    /* monadic `?` (distinct, rowid — the `distinct` row) is likewise a K-ism
     * superset.  Family = dyadic roll/deal/find (L4 pilot: `-3?t`); the dict
     * entries-axis collision is spec §9.1 (see AUDIT). */
    { "?",     QLEX_GLYPH,     QR_FN1("distinct", q_distinct_wrap), QR_FN2("rand", q_roll_wrap), NULL, 0, 0, "index" },
    /* monadic `$` stays QR_NONE: `$x` is the cast-vs-cond ambiguity, deferred.
     * Dyadic `$` is Cast ("an atomic function", ref/cast.md; kdb float->int
     * ROUNDING; Tok string-parse and unknown designators deferred); the
     * bracket cond form `$[c;t;f]` (3+ args) is a q_lower rewrite onto
     * rayfall `if`, not a registry cell. */
    { "$",     QLEX_GLYPH,     QR_NONE,                        QR_FN2("as", q_cast_wrap), NULL, 1, 0, "atomic" },
    /* monadic `@`/`.` stay QR_NONE: `@x` type-of is blocked on the q type
     * renumber; `.x` (value/get) comes with handles/namespaces.  Ternary+
     * Trap/Amend forms are deferred cells (error today via arity).  Family
     * none: Apply/Index ARE the application machinery (spec §5), not lifted. */
    { "@",     QLEX_GLYPH,     QR_NONE,                        QR_FNV("at", q_at_wrap),    NULL, 1, 0, "none" },
    { ".",     QLEX_GLYPH,     QR_NONE,                        QR_FNV("apply", q_dot_wrap), NULL, 1, 0, "none" },
    /* ---- keyword-infix ---- */
    { "div",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("div"),     NULL, 1, 0, "atomic" },
    /* q `x mod y` — modulus (ref/mod.md, atomic).  PURE RENAME: rayfall `%` IS
     * floored modulo with the sign following the divisor, exactly kdb mod
     * (audited live: -3 -2 mod 3 -> 0 1; 7 mod -2 -> -1; -7 mod -2.5 -> -2;
     * null passes through) — the mirror trick of q `%` -> rayfall `/`.  Base
     * `%` is registered RAY_FN_ATOMIC, so vector/dict broadcast comes free. */
    { "mod",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("%"),       NULL, 1, 0, "atomic" },
    /* q `x xexp y` / `x xlog y` — dyadic atomic libm wrappers (ref/exp.md,
     * ref/log.md: both atomic); null/divergence details at the bodies. */
    { "xexp",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("xexp", q_xexp_wrap), NULL, 1, 0, "atomic" },
    { "xlog",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("xlog", q_xlog_wrap), NULL, 1, 0, "atomic" },
    /* q `f each x` == `f'x`: a dyadic wrapper over rayfall map (+ vector
     * collapse, since map returns a boxed list where q wants a simple vec). */
    { "each",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("map", q_each_wrap), NULL, 1, 0, "none" },
    /* q `x in y` — membership (ref/in.md: left-atomic comparison): typed-
     * vector y via base ray_in_fn; generic-list y is whole-item, rank-
     * sensitive.  rowid: needs item equality (set-op predicate). */
    { "in",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("in", q_in_wrap), NULL, 1, 0, "rowid" },
    /* q `and` — keyword spelling of `&` (element-wise min / logical AND,
     * ref/and.md, ref/lesser.md: atomic).  REUSES the SAME q_min2_wrap kernel
     * the glyph `&` routes to — no new logic.  Numeric/bool are kdb-true
     * (`2 and 3`->2, `1010b and 1100b`->1000b); the char-min arm (`"sat" and
     * "cow"`->"cat") is a shared q_min2_wrap gap (DEFERRED, needs a char arm =
     * new logic).  Monadic cell stays QR_NONE: q `and` is dyadic-only, so
     * prefix `and x` misses and eval falls through to rayfall's scalar `and`
     * special form (DEFERRED edge — a monadic wrapper would be new logic). */
    { "and",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("and", q_min2_wrap), NULL, 1, 0, "atomic" },
    /* q `x within y` — inclusive bounds check (ref/within.md: "left-uniform",
     * i.e. conforms to x — the atomic law on the left operand); wrapper
     * because base ray_within_fn is vector-vals-only and width-blind. */
    { "within",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("within", q_within_wrap), NULL, 1, 0, "atomic" },
    /* q `n cut x` — chunk (int atom) / positional cut (int vector).  Border
     * ruling: index (see FAMILY AUDIT). */
    { "cut",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("cut", q_cut_wrap), NULL, 1, 0, "index" },
    /* q `n rotate x` / `n sublist x` — dyadic infix keywords (ref/rotate.md
     * "uniform" / ref/sublist.md) — both pure index-space gathers (L4).
     * Not KW_INFIX -> the scanner would split `n rotate x` into two
     * statements, so the manifest row is what makes them infix. */
    { "rotate",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("rotate", q_rotate_wrap), NULL, 1, 0, "index" },
    { "sublist",QLEX_KW_INFIX, QR_NONE,                        QR_FN2("sublist", q_sublist_wrap), NULL, 1, 0, "index" },
    /* q `x vs y` / `x sv y` — split-join / base-encode family (dyadic infix
     * keywords; wrappers, native -RAY_STR + sym + base + byte).  Monadic form
     * is out of scope (kdb `vs`/`sv` are strictly dyadic).  Codecs: none. */
    { "vs",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("vs", q_vs_wrap), NULL, 1, 0, "none" },
    { "sv",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("sv", q_sv_wrap), NULL, 1, 0, "none" },
    /* ---- set operations (feat/q-setops) — rowid (item identity, spec §3) ---- */
    /* q `x except y` — items of x not in y, x-duplicates and order KEPT
     * (ref/except.md).  rayfall's ray_except_fn is exactly this for lists;
     * the q_except_wrap adds a TABLE-pair arm (row membership) and delegates
     * everything else to ray_except_fn unchanged (was a pure QR_ENV row
     * pre-table-verbs).  Target stays "except" for provenance/serde. */
    { "except",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("except", q_except_wrap), NULL, 1, 0, "rowid" },
    /* q `x union y` == `distinct x,y` — WRAPPER: rayfall union keeps
     * x-duplicates, kdb dedups the whole join (ref/union.md). */
    { "union", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("union", q_union_wrap), NULL, 1, 0, "rowid" },
    /* q `x inter y` — items of x that are in y, x-duplicates and order KEPT
     * (ref/inter.md: "uses the result of x in y to return items from x").
     * rayfall spells it `sect` (ray_sect_fn) and is exactly this for lists;
     * the thin wrapper only guards dict/table operands 'nyi (rayfall sect
     * returns a wrong-shaped dict there; kdb returns common values). */
    { "inter", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("sect", q_inter_wrap), NULL, 1, 0, "rowid" },
    /* q `x cross y` == {raze x,/:\:y} — Cartesian product WRAPPER (no
     * rayfall cartesian primitive; composes item access + q join).  String
     * and dict/table operands are deferred cells (ref/cross.md).  Border
     * ruling: structural (see FAMILY AUDIT). */
    { "cross", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("cross", q_cross_wrap), NULL, 1, 0, "structural" },
    /* ---- q join family (feat/q-joins-rebuild) ----
     * Dyadic infix keywords over the engine hash/asof join (pair-relation
     * core in q_wrap_join.c: rowid-augmented key tables through
     * ray_left_join_fn / ray_asof_join_fn, kdb-ordered, gather-assembled).
     * Family: structural (schema-defining; border ruling in the AUDIT). */
    { "lj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("lj", q_lj_wrap),   NULL, 1, 0, "structural" },
    { "ljf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ljf", q_ljf_wrap), NULL, 1, 0, "structural" },
    { "ij",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ij", q_ij_wrap),   NULL, 1, 0, "structural" },
    { "ijf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ijf", q_ijf_wrap), NULL, 1, 0, "structural" },
    { "uj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("uj", q_uj_wrap),   NULL, 1, 0, "structural" },
    { "ujf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ujf", q_ujf_wrap), NULL, 1, 0, "structural" },
    { "pj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("pj", q_pj_wrap),   NULL, 1, 0, "structural" },
    { "asof",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("asof", q_asof_wrap), NULL, 1, 0, "structural" },
    /* Bracket-form joins (ej[c;t1;t2], aj[...], wj[w;f;t;spec]): triadic-plus
     * prefix keywords — q-owned env VARY bindings (q_builtins_register),
     * snapshotted here as QR_ENV rows (the ssr precedent: the parser
     * name-refs `ej[a;b;c]`, so it resolves through the env). */
    { "ej",    QLEX_KW_PREFIX, QR_ENV("ej"),                   QR_NONE,           NULL, 1, 0, "structural" },
    { "aj",    QLEX_KW_PREFIX, QR_ENV("aj"),                   QR_NONE,           NULL, 1, 0, "structural" },
    { "aj0",   QLEX_KW_PREFIX, QR_ENV("aj0"),                  QR_NONE,           NULL, 1, 0, "structural" },
    { "ajf",   QLEX_KW_PREFIX, QR_ENV("ajf"),                  QR_NONE,           NULL, 1, 0, "structural" },
    { "ajf0",  QLEX_KW_PREFIX, QR_ENV("ajf0"),                 QR_NONE,           NULL, 1, 0, "structural" },
    { "wj",    QLEX_KW_PREFIX, QR_ENV("wj"),                   QR_NONE,           NULL, 1, 0, "structural" },
    { "wj1",   QLEX_KW_PREFIX, QR_ENV("wj1"),                  QR_NONE,           NULL, 1, 0, "structural" },
    /* ---- sort / bucket family (feat/q-sort-rank) — dyadic infix ----
     * `bin`/`binr` reuse rayfall verbatim (same arg order: sorted-vec left,
     * value right; ordering-based search -> rowid).  `xrank` is a clean
     * pass-through (n left, vec right; ref/xrank.md "right-uniform" — the
     * ordering essence puts it in rowid).  `xbar` is an ARG-SWAP wrapper
     * (rayfall xbar is (col,bucket); q spells it (bucket,col)) — "xbar is
     * atomic" (ref/xbar.md).  All infix so `a verb b` is one stmt. */
    { "bin",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("bin"),     NULL, 1, 0, "rowid" },
    { "binr",  QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("binr"),    NULL, 1, 0, "rowid" },
    { "xrank", QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("xrank"),   NULL, 1, 0, "rowid" },
    { "xbar",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("xbar", q_xbar_wrap), NULL, 1, 0, "atomic" },
    /* ---- Wave 5: running scans (monadic prefix keywords) — all doc-labelled
     * uniform (ref/{sum,prd,max,min,avg}.md); null discipline at the bodies
     * (q_wrap_agg.c).  `ratios` is a border case: its page's LABEL says
     * aggregate, its documented behaviour is uniform (see FAMILY AUDIT). ---- */
    { "sums",  QLEX_KW_PREFIX, QR_FN1("sums", q_sums_wrap),    QR_NONE,           NULL, 1, 0, "map" },
    { "prds",  QLEX_KW_PREFIX, QR_FN1("prds", q_prds_wrap),    QR_NONE,           NULL, 1, 0, "map" },
    /* q `prd x` — the multiply-over fold twin of prds (ref/prd.md: aggregate):
     * nulls are 1s, bool vector -> int, list-of-lists element-wise, dict/table
     * implicit iteration.  Wrapper (no rayfall product aggregate). */
    { "prd",   QLEX_KW_PREFIX, QR_FN1("prd", q_prd_wrap),      QR_NONE,           NULL, 1, 0, "aggregate" },
    { "maxs",  QLEX_KW_PREFIX, QR_FN1("maxs", q_maxs_wrap),    QR_NONE,           NULL, 1, 0, "map" },
    { "mins",  QLEX_KW_PREFIX, QR_FN1("mins", q_mins_wrap),    QR_NONE,           NULL, 1, 0, "map" },
    { "avgs",  QLEX_KW_PREFIX, QR_FN1("avgs", q_avgs_wrap),    QR_NONE,           NULL, 1, 0, "map" },
    { "ratios",QLEX_KW_PREFIX, QR_FN1("ratios", q_ratios_wrap),QR_NONE,           NULL, 1, 0, "map" },
    /* ---- Wave 5: weighted (dyadic infix keywords) — aggregates
     * (ref/sum.md wsum, ref/avg.md wavg) ---- */
    { "wsum",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("wsum", q_wsum_wrap), NULL, 1, 0, "aggregate" },
    { "wavg",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("wavg", q_wavg_wrap), NULL, 1, 0, "aggregate" },
    /* ---- Wave 5: statistical (renames of audited base aggregates; all
     * doc-labelled aggregate — ref/{med,var,dev,cor,cov}.md) ----
     * kdb `var`/`dev` are POPULATION (÷n); rayfall `var`/`stddev` are SAMPLE
     * (÷n-1) and `var_pop`/`stddev_pop`/`dev` are population — so q `var`->
     * `var_pop`, q `svar`->`var`, q `sdev`->`stddev`, q `dev` stays `dev`. */
    { "med",   QLEX_KW_PREFIX, QR_ENV("med"),                  QR_NONE,           NULL, 1, 0, "aggregate" },
    { "var",   QLEX_KW_PREFIX, QR_ENV("var_pop"),              QR_NONE,           NULL, 1, 0, "aggregate" },
    { "svar",  QLEX_KW_PREFIX, QR_ENV("var"),                  QR_NONE,           NULL, 1, 0, "aggregate" },
    { "sdev",  QLEX_KW_PREFIX, QR_ENV("stddev"),               QR_NONE,           NULL, 1, 0, "aggregate" },
    { "cor",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("pearson_corr"), NULL, 1, 0, "aggregate" },
    { "cov",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("cov", q_cov_wrap), NULL, 1, 0, "aggregate" },
    { "scov",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("scov", q_scov_wrap), NULL, 1, 0, "aggregate" },
    /* ---- Wave 5: sliding m-windows + ema (dyadic infix keywords) — all
     * doc-labelled uniform (ref/{sum,avg,max,min,dev,count,ema}.md) ---- */
    { "msum",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("msum", q_msum_wrap), NULL, 1, 0, "map" },
    { "mavg",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mavg", q_mavg_wrap), NULL, 1, 0, "map" },
    { "mmax",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mmax", q_mmax_wrap), NULL, 1, 0, "map" },
    { "mmin",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mmin", q_mmin_wrap), NULL, 1, 0, "map" },
    { "mcount",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mcount", q_mcount_wrap), NULL, 1, 0, "map" },
    { "mdev",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mdev", q_mdev_wrap), NULL, 1, 0, "map" },
    { "ema",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ema", q_ema_wrap), NULL, 1, 0, "map" },
    /* iterator mnemonic keywords (wave-2): infix `f over/scan/prior/peach x`,
     * same lexical treatment as `each`.  over/scan dispatch reduce/converge/
     * do/while by f rank; prior is unary each-prior; peach == each. */
    { "over",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("over", q_over_kw), NULL, 1, 0, "none" },
    { "scan",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("scan-kw", q_scan_kw), NULL, 1, 0, "none" },
    { "prior", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("prior", q_prior_kw), NULL, 1, 0, "none" },
    { "peach", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("peach", q_each_wrap), NULL, 1, 0, "none" },
    /* ---- keyword-prefix monads (pass-through/rename) ---- */
    { "neg",     QLEX_KW_PREFIX, QR_FN1A("neg", q_neg_wrap),   QR_NONE,           NULL, 1, 0, "atomic" },
    /* q `til` — generator (atom -> vector; no structure input, family none);
     * kdb accepts a boolean (`til 1b` -> ,0), base ray_til_fn is int-only. */
    { "til",     QLEX_KW_PREFIX, QR_FN1("til", q_til_wrap),    QR_NONE,           NULL, 1, 0, "none" },
    { "count",   QLEX_KW_PREFIX, QR_ENV("count"),              QR_NONE,           NULL, 1, 0, "aggregate" },
    /* first/last — aggregates (ref/first.md: "first is an aggregate"). */
    { "first",   QLEX_KW_PREFIX, QR_ENV("first"),              QR_NONE,           NULL, 1, 0, "aggregate" },
    { "last",    QLEX_KW_PREFIX, QR_ENV("last"),               QR_NONE,           NULL, 1, 0, "aggregate" },
    /* q `next x` / `prev x` — shift by one, null-filling the vacated end
     * (ref/next.md: uniform).  L4 index: a shift IS a gather by shifted
     * indices (spec §2 lists next/prev/xprev in the index family). */
    { "next",    QLEX_KW_PREFIX, QR_FN1("next", q_next_wrap),  QR_NONE,           NULL, 1, 0, "index" },
    { "prev",    QLEX_KW_PREFIX, QR_FN1("prev", q_prev_wrap),  QR_NONE,           NULL, 1, 0, "index" },
    /* q `n xprev x` — n-item shift (ref/next.md: right-uniform); dyadic infix
     * like rotate. */
    { "xprev",   QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("xprev", q_xprev_wrap), NULL, 1, 0, "index" },
    /* q `fills x` — forward-fill nulls (ref/fill.md: uniform; the `^\`
     * fill-scan).  Computes values (not a gather) -> map. */
    { "fills",   QLEX_KW_PREFIX, QR_FN1("fills", q_fills_wrap), QR_NONE,          NULL, 1, 0, "map" },
    /* q `where x` — the L4 index-space generator (border ruling: index —
     * see FAMILY AUDIT); int-vector replicate + bool-mask forms at the body. */
    { "where",   QLEX_KW_PREFIX, QR_FN1("where", q_where_wrap), QR_NONE,          NULL, 1, 0, "index" },
    { "reverse", QLEX_KW_PREFIX, QR_FN1("reverse", q_reverse_wrap), QR_NONE,      NULL, 1, 0, "index" },
    /* q `sum` over a boxed LIST sums the items (`sum(dates;times)` ->
     * timestamps, ref/file-text.md); rayfall sum is vector-only, so wrapper.
     * NB the wrapper is deliberately NOT RAY_FN_AGGR — the eval aggregate
     * fast path claims AGGR fns before the wrapper runs and 'types on a boxed
     * list-of-vectors; name-routing (RAY_FN_Q_LOWER + aux "sum") keeps
     * query/DAG behaviour. */
    { "sum",     QLEX_KW_PREFIX, QR_FN1("sum", q_sum_wrap),    QR_NONE,           NULL, 1, 0, "aggregate" },
    /* group — rowid (item equality; spec §3 lists it; a §9 border flag). */
    { "group",   QLEX_KW_PREFIX, QR_ENV("group"),              QR_NONE,           NULL, 1, 0, "rowid" },
    /* ---- sort / grade family (feat/q-sort-rank) — monadic prefix; rowid
     * (ordering — spec §3 row-identity, the #174 stable-grade kernels) ----
     * asc/desc reuse ray_asc_fn/ray_desc_fn for flat vectors (VALUE kdb-true;
     * the sorted `s#` attribute is a deferred divergence — rayfall has no sorted
     * attribute) and add a DICT-sort-by-value arm.  iasc/idesc reuse ray_iasc_fn/
     * ray_idesc_fn and share the same DICT-grade arm as the `<`/`>` glyphs. */
    { "asc",     QLEX_KW_PREFIX, QR_FN1("asc", q_asc_wrap),    QR_NONE,           NULL, 1, 0, "rowid" },
    { "desc",    QLEX_KW_PREFIX, QR_FN1("desc", q_desc_wrap),  QR_NONE,           NULL, 1, 0, "rowid" },
    { "iasc",    QLEX_KW_PREFIX, QR_FN1("iasc", q_iasc_wrap),  QR_NONE,           NULL, 1, 0, "rowid" },
    { "idesc",   QLEX_KW_PREFIX, QR_FN1("idesc", q_idesc_wrap), QR_NONE,          NULL, 1, 0, "rowid" },
    { "avg",     QLEX_KW_PREFIX, QR_ENV("avg"),                QR_NONE,           NULL, 1, 0, "aggregate" },
    /* q `floor` returns LONGS from floats (kdb `floor 3.7` is 3j); rayfall's
     * env floor keeps f64, so this is the q_floor_wrap, not a rename. */
    { "floor",   QLEX_KW_PREFIX, QR_FN1A("floor", q_floor_wrap), QR_NONE,         NULL, 1, 0, "atomic" },
    /* q dict accessors: `key`/`value` are wrappers (dict-only in 2c-2 —
     * the file-handle/namespace/enumeration overloads are deferred cells);
     * `distinct` must preserve FIRST-OCCURRENCE order (kdb ref/distinct.md),
     * while rayfall's distinct routes typed vectors through the DAG group
     * path, which sorts — so it too is a wrapper, not a rename (audited:
     * `distinct 2 3 7 3 5 3` must be 2 3 7 5, env distinct gives 2 3 5 7). */
    { "key",     QLEX_KW_PREFIX, QR_FN1("key", q_key_wrap),    QR_NONE,           NULL, 1, 0, "structural" },
    { "value",   QLEX_KW_PREFIX, QR_FN1("value", q_value_wrap), QR_NONE,          NULL, 1, 0, "structural" },
    /* q `get` is a SYNONYM of `value` (ref/get.md: "completely interchangeable")
     * — same q_value_wrap, one home.  `nam set y` writes the named global
     * (sym-handle assign / `. context restore); file forms are 'nyi cells. */
    { "get",     QLEX_KW_PREFIX, QR_FN1("value", q_value_wrap), QR_NONE,          NULL, 1, 0, "structural" },
    { "set",     QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("set-g", q_setg_wrap), NULL, 1, 1, "none" },
    { "distinct",QLEX_KW_PREFIX, QR_FN1("distinct", q_distinct_wrap), QR_NONE,    NULL, 1, 0, "rowid" },
    /* q `rand x` == {first 1?x} (ref/rand.md) — q_rand_wrap reuses the
     * `?` roll/generate arms (family follows `?`: a random-index gather).
     * NOT the rayfall binary `rand` (which stays the env-bound engine builtin
     * the roll wrapper calls). */
    { "rand",    QLEX_KW_PREFIX, QR_FN1("rand-1", q_rand_wrap), QR_NONE,          NULL, 0, 0, "index" },
    /* q-implemented keywords: env bindings added by q_builtins_register
     * (same mechanism as `parse`), snapshotted here as pass-through rows.
     * string is atomic (ref/string.md: "atomic … iterates through
     * dictionaries and tables"); upper/lower likewise elementwise.  Atomic
     * here is at STRING granularity (see the FAMILY AUDIT caveat). */
    { "string",  QLEX_KW_PREFIX, QR_ENV("string"),             QR_NONE,           NULL, 1, 0, "atomic" },
    { "upper",   QLEX_KW_PREFIX, QR_ENV("upper"),              QR_NONE,           NULL, 1, 0, "atomic" },
    { "lower",   QLEX_KW_PREFIX, QR_ENV("lower"),              QR_NONE,           NULL, 1, 0, "atomic" },
    /* ---- string trim / hash / search family (feat/q-string-fns) ----
     * trim/ltrim/rtrim/md5 are q-owned env unaries (q_builtins_register),
     * snapshotted here as QR_ENV prefix rows (same mechanism as `string`).
     * trim family is elementwise over string atoms (atomic at string
     * granularity); md5 consumes a whole string as a digest codec (none).
     * `like`/`ss` are dyadic infix; `ssr` is a triadic-prefix wrapper. */
    { "trim",    QLEX_KW_PREFIX, QR_ENV("trim"),               QR_NONE,           NULL, 1, 0, "atomic" },
    { "ltrim",   QLEX_KW_PREFIX, QR_ENV("ltrim"),              QR_NONE,           NULL, 1, 0, "atomic" },
    { "rtrim",   QLEX_KW_PREFIX, QR_ENV("rtrim"),              QR_NONE,           NULL, 1, 0, "atomic" },
    { "md5",     QLEX_KW_PREFIX, QR_ENV("md5"),                QR_NONE,           NULL, 1, 0, "none" },
    { "like",    QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("like", q_like_wrap), NULL, 1, 0, "atomic" },
    { "ss",      QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("ss", q_ss_wrap), NULL, 1, 0, "none" },
    { "ssr",     QLEX_KW_PREFIX, QR_ENV("ssr"),                QR_NONE,           NULL, 1, 0, "none" },
    { "show",    QLEX_KW_PREFIX, QR_ENV("show"),               QR_NONE,           NULL, 1, 1, "none" },
    /* `system "…"` — q-owned env unary (q_builtins_register), snapshotted here
     * so the parser embeds it; routes through q_sys_dispatch (q_sys.c). */
    { "system",  QLEX_KW_PREFIX, QR_ENV("system"),             QR_NONE,           NULL, 1, 1, "none" },
    /* table introspection — q-owned bindings (q_builtins_register), snapshotted
     * here so the parser embeds them over the base env `meta`. */
    { "meta",    QLEX_KW_PREFIX, QR_ENV("meta"),               QR_NONE,           NULL, 1, 0, "structural" },
    { "cols",    QLEX_KW_PREFIX, QR_ENV("cols"),               QR_NONE,           NULL, 1, 0, "structural" },
    /* q `attr x` — column attribute as a single symbol (ref/set-attribute.md,
     * ref/attr.md).  Monadic wrapper over the engine's `.attr.get`, collapsing
     * the full-name sym vector to kdb's one letter.  The set/clear side is the
     * `#` verb's symbol-atom arm (q_take_wrap), not a row here. */
    { "attr",    QLEX_KW_PREFIX, QR_FN1("attr", q_attr_wrap),  QR_NONE,           NULL, 1, 0, "structural" },
    /* ---- additional monadic pass-through keywords (rayfall name == q name,
     * audited kdb-true element-wise / aggregate semantics; atomic labels per
     * ref/{abs,exp,log,sqrt,null}.md, aggregate per ref/{dev,max,min}.md). See
     * docs/recipes/add-q-keyword-verb.md. ---- */
    { "abs",     QLEX_KW_PREFIX, QR_ENV("abs"),                QR_NONE,           NULL, 1, 0, "atomic" },
    /* q `null x` — elementwise null test (ref/null.md: atomic).  Routes to
     * the engine's atomic `nil?` (RAY_FN_ATOMIC): broadcasts over vectors and
     * nested lists at every depth.  The wrapper collapses a homogeneous
     * top-level bool-atom run (heterogeneous input list) to a bool vector
     * for kdb-true display. */
    { "null",    QLEX_KW_PREFIX, QR_FN1("nil?", q_null_wrap),  QR_NONE,           NULL, 1, 0, "atomic" },
    { "dev",     QLEX_KW_PREFIX, QR_ENV("dev"),                QR_NONE,           NULL, 1, 0, "aggregate" },
    { "exp",     QLEX_KW_PREFIX, QR_ENV("exp"),                QR_NONE,           NULL, 1, 0, "atomic" },
    { "log",     QLEX_KW_PREFIX, QR_ENV("log"),                QR_NONE,           NULL, 1, 0, "atomic" },
    { "max",     QLEX_KW_PREFIX, QR_ENV("max"),                QR_NONE,           NULL, 1, 0, "aggregate" },
    { "min",     QLEX_KW_PREFIX, QR_ENV("min"),                QR_NONE,           NULL, 1, 0, "aggregate" },
    /* rank == iasc iasc (ref/rank.md) — the grade family, rowid. */
    { "rank",    QLEX_KW_PREFIX, QR_ENV("rank"),               QR_NONE,           NULL, 1, 0, "rowid" },
    /* q `raze x` — flattens one level of structure (ref/raze.md; base
     * ray_raze_fn plus the kdb atom arm `raze 42` -> ,42). */
    { "raze",    QLEX_KW_PREFIX, QR_FN1("raze", q_raze_wrap),  QR_NONE,           NULL, 1, 0, "structural" },
    { "sqrt",    QLEX_KW_PREFIX, QR_ENV("sqrt"),               QR_NONE,           NULL, 1, 0, "atomic" },
    /* ---- atomic unary math (feat/q-math-atomic) — implement-via-libm, no
     * rayfall counterpart (rayfall has exp/log/sqrt but not the trig set).
     * All monadic KW_PREFIX, doc-labelled atomic (ref/{trig,reciprocal,
     * signum,ceiling}.md); sentinel-null discipline at the bodies
     * (q_wrap_math.c).  `ceiling` is the floor-wrapper twin (float->LONG),
     * NOT rayfall `ceil` (f64).  `signum` is family-atomic but built WITHOUT
     * RAY_FN_ATOMIC — it drives its own broadcast so a top-level boxed-list
     * result collapses to an int vector (see q_signum_wrap). */
    { "sin",       QLEX_KW_PREFIX, QR_FN1A("sin", q_sin_wrap),   QR_NONE, NULL, 1, 0, "atomic" },
    { "cos",       QLEX_KW_PREFIX, QR_FN1A("cos", q_cos_wrap),   QR_NONE, NULL, 1, 0, "atomic" },
    { "tan",       QLEX_KW_PREFIX, QR_FN1A("tan", q_tan_wrap),   QR_NONE, NULL, 1, 0, "atomic" },
    { "asin",      QLEX_KW_PREFIX, QR_FN1A("asin", q_asin_wrap), QR_NONE, NULL, 1, 0, "atomic" },
    { "acos",      QLEX_KW_PREFIX, QR_FN1A("acos", q_acos_wrap), QR_NONE, NULL, 1, 0, "atomic" },
    { "atan",      QLEX_KW_PREFIX, QR_FN1A("atan", q_atan_wrap), QR_NONE, NULL, 1, 0, "atomic" },
    { "reciprocal",QLEX_KW_PREFIX, QR_FN1A("reciprocal", q_reciprocal_wrap), QR_NONE, NULL, 1, 0, "atomic" },
    { "signum",    QLEX_KW_PREFIX, QR_FN1("signum", q_signum_wrap), QR_NONE, NULL, 1, 0, "atomic" },
    { "ceiling",   QLEX_KW_PREFIX, QR_FN1A("ceiling", q_ceiling_wrap), QR_NONE, NULL, 1, 0, "atomic" },
    /* ---- table verbs (feat/q-table-verbs) — wrappers in q_wrap_table.c over
     * the wave-4 keyed primitives (q_enkey/q_table_flatten) and the base sort
     * kernel (ray_xasc_fn, ARG-SWAPPED like xbar).  `insert`/`upsert`
     * intentionally SHADOW the base env special forms of the same name: q
     * semantics differ (by-name targets, keyed collision/update, row-index
     * results), so the registry value is a wrapper, never the env snapshot.
     * Families: flip/keys/xkey/xcol/xcols/ungroup/insert/upsert define or
     * rearrange the structures (structural, spec §3); xasc/xdesc sort
     * (rowid); xgroup groups (rowid — border ruling in the AUDIT). */
    { "flip",   QLEX_KW_PREFIX, QR_FN1("flip", q_flip_wrap),   QR_NONE,           NULL, 1, 0, "structural" },
    { "keys",   QLEX_KW_PREFIX, QR_FN1("keys", q_keys_wrap),   QR_NONE,           NULL, 1, 0, "structural" },
    { "ungroup",QLEX_KW_PREFIX, QR_FN1("ungroup", q_ungroup_wrap), QR_NONE,       NULL, 1, 0, "structural" },
    { "xkey",   QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xkey", q_xkey_wrap), NULL, 1, 0, "structural" },
    { "xcol",   QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xcol", q_xcol_wrap), NULL, 1, 0, "structural" },
    { "xcols",  QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xcols", q_xcols_wrap), NULL, 1, 0, "structural" },
    { "xasc",   QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xasc", q_xasc_wrap), NULL, 1, 0, "rowid" },
    { "xdesc",  QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xdesc", q_xdesc_wrap), NULL, 1, 0, "rowid" },
    { "xgroup", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xgroup", q_xgroup_wrap), NULL, 1, 0, "rowid" },
    { "insert", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("insert", q_insert_wrap), NULL, 1, 1, "structural" },
    { "upsert", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("upsert", q_upsert_wrap), NULL, 1, 1, "structural" },
    /* each-prior mnemonics: deltas x == (-':)x, differ x == not(~':)x —
     * length-preserving computed values (ref/differ.md: uniform) -> map. */
    { "deltas",  QLEX_KW_PREFIX, QR_FN1("deltas", q_deltas_wrap), QR_NONE,        NULL, 1, 0, "map" },
    { "differ",  QLEX_KW_PREFIX, QR_FN1("differ", q_differ_wrap), QR_NONE,        NULL, 1, 0, "map" },
    /* ---- boolean/null batch: RESERVED-but-DEFERRED (feat/q-bool-null) ----
     * `any`/`all` are real q keywords rostered to reserve the name
     * (`any:5`/`all:5` -> 'assign, kdb-true) with QR_NONE (no value) — a
     * documented DEFER: their range is boolean `b` for every domain
     * (ref/all-any.md: both aggregates) and a `max`/`min` rename is type-wrong
     * for non-boolean input (`all 2000.01.02 2010.01.02`->1b, but max is a
     * DATE); the boolean coercion is new logic (HELD).  KW_PREFIX so the
     * reservation does not reclassify the scanner.
     * NB: `null` LANDED separately as a real atomic verb (q_null_wrap,
     * feat/q-atomic-extend #67) — its reserved row here was dropped on merge to
     * avoid a duplicate.  `or` is the keyword spelling of `|`-max and waits for
     * `|`-max (a valueless QLEX_KW_INFIX row would expose rayfall's scalar `or`). */
    { "any",     QLEX_KW_PREFIX, QR_NONE,                      QR_NONE,           NULL, 1, 0, "aggregate" },
    { "all",     QLEX_KW_PREFIX, QR_NONE,                      QR_NONE,           NULL, 1, 0, "aggregate" },
    /* ---- IPC client verbs (feat/q-ipc-client, Phase D) — thin wrappers over the
     * kdb-speaking `.ipc.*` primitives (Phase C).  `hopen` normalizes int|string|
     * (conn;timeout) into the `.ipc.open` string API and returns the connection's
     * socket fd as the handle (kdb-faithful — 0/1/2 reserved, connections at 3+);
     * `hclose` translates it back and routes to `.ipc.close`.  The sync/async send
     * verb `h"query"` is handle-as-verb application (q_apply.c int-head arm), not a
     * manifest row.  Both are monadic prefix keywords (KW_PREFIX). ---- */
    { "hopen",  QLEX_KW_PREFIX, QR_FN1("hopen", q_hopen_wrap),   QR_NONE, NULL, 1, 1, "none" },
    { "hclose", QLEX_KW_PREFIX, QR_FN1("hclose", q_hclose_wrap), QR_NONE, NULL, 1, 1, "none" },
    /* ---- File Text (feat/q-file-text) — the `0:` operator + companions.
     * `0:` is tokenized by the scanner's digit-colon arm (a single 0/1/2
     * glued to ':' can never start a clock literal) and dispatches on the
     * LEFT operand's shape (see q_filetext_wrap: Save Text / Prepare Text /
     * Key-Value pairs / Load CSV / Load Fixed — text forms ONLY, binary
     * 1:/2: are an owner-ruled non-goal, so they have NO row and stay
     * name-refs ('name)).  `read0` is also env-bound by q_builtins for the
     * bracket-call form (the ssr/value precedent).  `hsym` is elementwise
     * over sym atoms/vectors (ref/hsym.md: atom or vector). */
    { "0:",     QLEX_GLYPH,     QR_NONE,                       QR_FN2("file-text", q_filetext_wrap), NULL, 1, 1, "none" },
    { "hsym",   QLEX_KW_PREFIX, QR_FN1("hsym", q_hsym_wrap),   QR_NONE,           NULL, 1, 0, "atomic" },
    { "read0",  QLEX_KW_PREFIX, QR_FN1("read0", q_read0_wrap), QR_NONE,           NULL, 1, 1, "none" },
    /* ---- environment variables (feat/q-getenv-setenv, ref/getenv.md) ----
     * `getenv` is a monadic prefix keyword (sym -> value string, "" unset);
     * `setenv` is a dyadic infix keyword (`sym setenv str`), so it MUST be
     * KW_INFIX or the scanner would split `x setenv y` into two statements
     * (the `set`/`xkey` precedent).  Both reuse the base .os.getenv/.os.setenv
     * C primitives (ray_getenv_fn/ray_setenv_fn) via wrappers that coerce the
     * q symbol arg to the -RAY_STR the C wants — a real divergence, so QR_FN
     * wrappers, not QR_ENV renames. */
    { "getenv", QLEX_KW_PREFIX, QR_FN1("getenv", q_getenv_wrap), QR_NONE,         NULL, 1, 0, "none" },
    { "setenv", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("setenv", q_setenv_wrap), NULL, 1, 1, "none" },
    /* ---- adverbs — q adverbs ARE rayfall higher-order fns (no bespoke object).
     * `+/` lowers to fold over `+` (q_lower); `/:`/`\:` ARE map-right/map-left
     * (src/ops/collection.c:2279 — map-left iterates LEFT holding right =
     * q each-left, verified against examples/rfl).  Each-prior `':` still has
     * no rayfall counterpart (the scan-* variants are fold-style, not
     * pairwise) — DEFERRED, still lexer-classified. ---- */
    { "'",     QLEX_ADVERB,    QR_NONE,  QR_NONE,  "map",       1, 0, "none" },
    { "/",     QLEX_ADVERB,    QR_NONE,  QR_NONE,  "fold",      1, 0, "none" },
    { "\\",    QLEX_ADVERB,    QR_NONE,  QR_NONE,  "scan",      1, 0, "none" },
    { "':",    QLEX_ADVERB,    QR_NONE,  QR_NONE,  NULL,        1, 0, "none" },  /* each-prior: deferred */
    { "/:",    QLEX_ADVERB,    QR_NONE,  QR_NONE,  "map-right", 1, 0, "none" },
    { "\\:",   QLEX_ADVERB,    QR_NONE,  QR_NONE,  "map-left",  1, 0, "none" },
};
#define N_Q_OPS ((int)(sizeof Q_OPS / sizeof Q_OPS[0]))

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

int q_ops_is_reserved(const char* s, int len) {
    for (int i = 0; i < N_Q_OPS; i++) {
        const char* nm = Q_OPS[i].name;
        if ((int)strlen(nm) == len && memcmp(nm, s, (size_t)len) == 0)
            return 1;
    }
    return 0;
}

/* Operator->integer opcode (`value +` -> 1) is DEFERRED: the opcode is a property
 * of the FUNCTION IDENTITY, not the spelling (`get`/`value` are two names for ONE
 * function, kdb `.:`), so a name-keyed table can't model it.  See PLAN.md; the doc
 * rows are pinned red-below-floor in test/q/function/value.qcmd. */
