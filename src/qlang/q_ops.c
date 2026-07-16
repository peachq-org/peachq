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
 *   (element-wise min / bool-and) — the `and` keyword reuses it.  `any`/`all`
 *   keep QR_NONE rows to reserve the name and fix the lexical class, but they
 *   are no longer deferred — their VALUE is self-hosted in q.q (see their rows
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
 *           the RNG valence makes the verb nondeterministic.  (`rand`, the
 *           other RNG keyword, is q.q-hosted — no manifest row.)
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
    { "+",     QLEX_GLYPH,     QR_FN1("flip", q_flip_wrap),    QR_ENV("+"),       NULL, 1, 0, "atomic",
      "returns the sum of two numeric, boolean or temporal arguments", "qdocs/docs/docs/docs/ref/add.md",
      "x+y     +[x;y]", NULL },
    { "-",     QLEX_GLYPH,     QR_FN1A("neg", q_neg_wrap),     QR_ENV("-"),       NULL, 1, 0, "atomic",
      "returns the difference of its arguments for a wide range of datatypes", "qdocs/docs/docs/docs/ref/subtract.md",
      "x-y     -[x;y]", NULL },
    /* `*` monadic is first (aggregate — the `first` row); family = dyadic. */
    { "*",     QLEX_GLYPH,     QR_ENV("first"),                QR_ENV("*"),       NULL, 1, 0, "atomic",
      "returns the product of its arguments", "qdocs/docs/docs/docs/ref/multiply.md",
      "x*y     *[x;y]", NULL },
    { "%",     QLEX_GLYPH,     QR_NONE,                        QR_ENV("/"),       NULL, 1, 0, "atomic",
      "returns the ratio of its arguments", "qdocs/docs/docs/docs/ref/divide.md",
      "x%y     %[x;y]", NULL },
    /* monadic `<`/`>` are grade up/down (rowid — the iasc/idesc rows); shared
     * wrapper adds the DICT arm (keys in value order); family = dyadic. */
    { "<",     QLEX_GLYPH,     QR_FN1("iasc", q_iasc_wrap),    QR_ENV("<"),       NULL, 1, 0, "atomic",
      "compare the values of their arguments", "qdocs/docs/docs/docs/ref/less-than.md",
      "x<y    <[x;y]", NULL },
    { ">",     QLEX_GLYPH,     QR_FN1("idesc", q_idesc_wrap),  QR_ENV(">"),       NULL, 1, 0, "atomic",
      "compare their arguments", "qdocs/docs/docs/docs/ref/greater-than.md",
      "x>y    >[x;y]", NULL },
    { "<=",    QLEX_GLYPH,     QR_NONE,                        QR_ENV("<="),      NULL, 1, 0, "atomic",
      "compare the values of their arguments", "qdocs/docs/docs/docs/ref/less-than.md",
      "x<=y   <=[x;y]", NULL },
    { ">=",    QLEX_GLYPH,     QR_NONE,                        QR_ENV(">="),      NULL, 1, 0, "atomic",
      "compare their arguments", "qdocs/docs/docs/docs/ref/greater-than.md",
      "x>=y   >=[x;y]", NULL },
    /* `=` monadic is group (rowid — the `group` row); family = dyadic Equal. */
    { "=",     QLEX_GLYPH,     QR_ENV("group"),                QR_FN2A("==", q_eq_wrap), NULL, 1, 0, "atomic",
      "flags where its arguments are equal", "qdocs/docs/docs/docs/ref/equal.md",
      "x=y    =[x;y]", NULL },
    { "<>",    QLEX_GLYPH,     QR_NONE,                        QR_FN2A("!=", q_ne_wrap), NULL, 1, 0, "atomic",
      "Not equal (returns 1b where arguments differ)", NULL,   /* HAND-AUTHORED: docsrc NULL, see tools/qdocs/qdocs-docmap.pins.tsv */
      "x<>y   <>[x;y]", "1 2 3 <> 3 2 1 -> 101b" },
    /* ---- structural glyphs ---- */
    /* `#` monadic is count (aggregate — the `count` row); family = dyadic
     * Take, the L4 pilot op (`-3#t`).  The arg-swap lives in q_take_wrap. */
    { "#",     QLEX_GLYPH,     QR_ENV("count"),                QR_FN2("take", q_take_wrap), NULL, 1, 0, "index",
      "Select leading or trailing items from a list or dictionary, named entries from a dictionary, or named columns from a table", "qdocs/docs/docs/docs/ref/take.md",
      "x#y     #[x;y]", NULL },
    /* monadic `_` is a K-ism (valid q spells it `floor`, atomic) — accepting
     * it is a deliberate SUPERSET of q source; the VALUE is kdb-identical
     * (kdb's own floor IS k `_:`).  Family = dyadic Drop (L4). */
    { "_",     QLEX_GLYPH,     QR_FN1A("floor", q_floor_wrap), QR_FN2("drop", q_drop_wrap), NULL, 1, 0, "index",
      "Drop items from a list, entries from a dictionary or columns from a table.", "qdocs/docs/docs/docs/ref/drop.md",
      "x _ y    _[x;y]", NULL },
    /* `|` dyadic (max) is a deferred cell (atomic when it lands); family =
     * monadic reverse (L4 — spec §2). */
    { "|",     QLEX_GLYPH,     QR_FN1("reverse", q_reverse_wrap), QR_NONE,        NULL, 1, 0, "index",
      "Greater; logical OR", "qdocs/docs/docs/docs/ref/greater.md",
      "x|y       |[x;y]", NULL },
    /* `&` monadic is where (index — the `where` row, a §9 border); family =
     * dyadic Lesser/min (atomic, ref/lesser.md). */
    { "&",     QLEX_GLYPH,     QR_FN1("where", q_where_wrap),  QR_FN2A("and", q_min2_wrap), NULL, 1, 0, "atomic",
      "Lesser of two values; logical AND", "qdocs/docs/docs/docs/ref/lesser.md",
      "x & y         &[x;y]", NULL },
    /* `,` — enlist / join: both construct structure (table,record-dict is
     * upsert semantics — see q_join_wrap). */
    { ",",     QLEX_GLYPH,     QR_ENV("enlist"),               QR_FN2("concat", q_join_wrap), NULL, 1, 0, "structural",
      "Join atoms, lists, dictionaries or tables", "qdocs/docs/docs/docs/ref/join.md#,",
      "x,y    ,[x;y]", NULL },
    /* `~` monadic is not (atomic); family = dyadic Match — reduces to one
     * bool atom (near-border: does NOT follow the L3 value-law; see AUDIT). */
    { "~",     QLEX_GLYPH,     QR_ENV("not"),                  QR_FN2("match", q_match_wrap), NULL, 1, 0, "aggregate",
      "flags whether its arguments have the same value", "qdocs/docs/docs/docs/ref/match.md",
      "x~y    ~[x;y]", NULL },
    /* q `x^y` — fill: coalesce nulls in y with x ("Fill is an atomic
     * function", ref/fill.md).  `^` already lexes as a verb glyph
     * (VERB_CHARS); this row gives it a registry value.  Monadic `^x`
     * (kdb: null-of-type / `fills` sans forward-fill) is a deferred cell. */
    { "^",     QLEX_GLYPH,     QR_NONE,                        QR_FN2("fill", q_fill_wrap), NULL, 1, 0, "atomic",
      "Replace nulls", "qdocs/docs/docs/docs/ref/fill.md#^",
      "x^y    ^[x;y]", NULL },
    /* ---- type-dispatch glyphs (2c-2) ---- */
    /* monadic `!` (dict keys) is a K-ism accepted as a deliberate superset
     * (valid q spells it `key`, same value — the `_`/floor precedent). */
    { "!",     QLEX_GLYPH,     QR_FN1("key", q_key_wrap),      QR_FN2("dict", q_bang_wrap), NULL, 1, 0, "structural",
      "Make a dictionary or keyed table; remove a key from a table", "qdocs/docs/docs/docs/ref/dict.md",
      "x!y    ![x;y]", NULL },
    /* monadic `?` (distinct, rowid — the `distinct` row) is likewise a K-ism
     * superset.  Family = dyadic roll/deal/find (L4 pilot: `-3?t`); the dict
     * entries-axis collision is spec §9.1 (see AUDIT). */
    { "?",     QLEX_GLYPH,     QR_FN1("distinct", q_distinct_wrap), QR_FN2("rand", q_roll_wrap), NULL, 0, 0, "index",
      "Random lists, with or without duplicates", "qdocs/docs/docs/docs/ref/deal.md",
      NULL, NULL },
    /* monadic `$` stays QR_NONE: `$x` is the cast-vs-cond ambiguity, deferred.
     * Dyadic `$` is Cast ("an atomic function", ref/cast.md; kdb float->int
     * ROUNDING; Tok string-parse and unknown designators deferred); the
     * bracket cond form `$[c;t;f]` (3+ args) is a q_lower rewrite onto
     * rayfall `if`, not a registry cell. */
    { "$",     QLEX_GLYPH,     QR_NONE,                        QR_FN2("as", q_cast_wrap), NULL, 1, 0, "atomic",
      "Convert to another datatype", "qdocs/docs/docs/docs/ref/cast.md",
      "x$y     $[x;y]", NULL },
    /* monadic `@`/`.` stay QR_NONE: `@x` type-of is blocked on the q type
     * renumber; `.x` (value/get) comes with handles/namespaces.  Ternary+
     * Trap/Amend forms are deferred cells (error today via arity).  Family
     * none: Apply/Index ARE the application machinery (spec §5), not lifted. */
    { "@",     QLEX_GLYPH,     QR_NONE,                        QR_FNV("at", q_at_wrap),    NULL, 1, 0, "none",
      "Operators Apply (At), Index (At), and Trap (At) apply a function to one or more arguments, get items at depth in a list, and trap errors", "qdocs/docs/docs/docs/ref/apply.md",
      NULL, NULL },
    { ".",     QLEX_GLYPH,     QR_NONE,                        QR_FNV("apply", q_dot_wrap), NULL, 1, 0, "none",
      "Operators Apply (At), Index (At), and Trap (At) apply a function to one or more arguments, get items at depth in a list, and trap errors", "qdocs/docs/docs/docs/ref/apply.md",
      NULL, NULL },
    /* ---- keyword-infix ---- */
    { "div",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("div"),     NULL, 1, 0, "atomic",
      "Integer division", "qdocs/docs/docs/docs/ref/div.md",
      "x div y    div[x;y]", "7 div 2 -> 3" },
    /* q `x mod y` — modulus (ref/mod.md, atomic).  PURE RENAME: rayfall `%` IS
     * floored modulo with the sign following the divisor, exactly kdb mod
     * (audited live: -3 -2 mod 3 -> 0 1; 7 mod -2 -> -1; -7 mod -2.5 -> -2;
     * null passes through) — the mirror trick of q `%` -> rayfall `/`.  Base
     * `%` is registered RAY_FN_ATOMIC, so vector/dict broadcast comes free. */
    { "mod",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("%"),       NULL, 1, 0, "atomic",
      "Modulus", "qdocs/docs/docs/docs/ref/mod.md",
      "x mod y    mod[x;y]", "7 mod 3 -> 1" },
    /* q `x xexp y` / `x xlog y` — dyadic atomic libm wrappers (ref/exp.md,
     * ref/log.md: both atomic); null/divergence details at the bodies. */
    { "xexp",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("xexp", q_xexp_wrap), NULL, 1, 0, "atomic",
      "Raise x to a power", "qdocs/docs/docs/docs/ref/exp.md#xexp",
      "x xexp y    xexp[x;y]", "2 xexp 3 -> 8f" },
    { "xlog",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("xlog", q_xlog_wrap), NULL, 1, 0, "atomic",
      "Logarithm", "qdocs/docs/docs/docs/ref/log.md#xlog",
      "x xlog y    xlog[x;y]", "2 xlog 8 -> 3f" },
    /* q `f each x` == `f'x`: a dyadic wrapper over rayfall map (+ vector
     * collapse, since map returns a boxed list where q wants a simple vec). */
    { "each",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("map", q_each_wrap), NULL, 1, 0, "none",
      "Iterate a unary", "qdocs/docs/docs/docs/ref/each.md",
      " v1 each x   each[v1;x]       v1 peach x   peach[v1;x]", NULL },
    /* q `x in y` — membership (ref/in.md: left-atomic comparison): typed-
     * vector y via base ray_in_fn; generic-list y is whole-item, rank-
     * sensitive.  rowid: needs item equality (set-op predicate). */
    { "in",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("in", q_in_wrap), NULL, 1, 0, "rowid",
      "Whether x is an item of y", "qdocs/docs/docs/docs/ref/in.md",
      "x in y    in[x;y]", "2 in 1 2 3 -> 1b" },
    /* q `and` — keyword spelling of `&` (element-wise min / logical AND,
     * ref/and.md, ref/lesser.md: atomic).  REUSES the SAME q_min2_wrap kernel
     * the glyph `&` routes to — no new logic.  Numeric/bool are kdb-true
     * (`2 and 3`->2, `1010b and 1100b`->1000b); the char-min arm (`"sat" and
     * "cow"`->"cat") is a shared q_min2_wrap gap (DEFERRED, needs a char arm =
     * new logic).  Monadic cell stays QR_NONE: q `and` is dyadic-only, so
     * prefix `and x` misses and eval falls through to rayfall's scalar `and`
     * special form (DEFERRED edge — a monadic wrapper would be new logic). */
    { "and",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2A("and", q_min2_wrap), NULL, 1, 0, "atomic",
      "Lesser of two values, logical AND", "qdocs/docs/docs/docs/ref/and.md",
      "x and y       and[x;y]", NULL },
    /* q `x within y` — inclusive bounds check (ref/within.md: "left-uniform",
     * i.e. conforms to x — the atomic law on the left operand); wrapper
     * because base ray_within_fn is vector-vals-only and width-blind. */
    { "within",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("within", q_within_wrap), NULL, 1, 0, "atomic",
      "Check bounds", "qdocs/docs/docs/docs/ref/within.md",
      "x within y    within[x;y]", "3 within 1 5 -> 1b" },
    /* q `n cut x` — chunk (int atom) / positional cut (int vector).  Border
     * ruling: index (see FAMILY AUDIT). */
    { "cut",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("cut", q_cut_wrap), NULL, 1, 0, "index",
      "Cut a list or table into a matrix of `x` columns", "qdocs/docs/docs/docs/ref/cut.md#cut",
      "x cut y     cut[x;y]", NULL },
    /* q `n rotate x` / `n sublist x` — dyadic infix keywords (ref/rotate.md
     * "uniform" / ref/sublist.md) — both pure index-space gathers (L4).
     * Not KW_INFIX -> the scanner would split `n rotate x` into two
     * statements, so the manifest row is what makes them infix; the VALUE is
     * the q.q derivation over `#`/`_` (QR_QSRC: bound post-bootstrap). */
    { "rotate",QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("rotate"), NULL, 1, 0, "index",
      "Shift the items of a list to the left or right", "qdocs/docs/docs/docs/ref/rotate.md",
      "x rotate y    rotate[x;y]", "2 rotate 1 2 3 4 5 -> 3 4 5 1 2" },
    { "sublist",QLEX_KW_INFIX, QR_NONE,                        QR_QSRC("sublist"), NULL, 1, 0, "index",
      "Select a sublist of a list", "qdocs/docs/docs/docs/ref/sublist.md",
      "x sublist y    sublist[x;y]", "2 sublist 1 2 3 4 -> 1 2" },
    /* q `x vs y` / `x sv y` — split-join / base-encode family (dyadic infix
     * keywords; wrappers, native -RAY_STR + sym + base + byte).  Monadic form
     * is out of scope (kdb `vs`/`sv` are strictly dyadic).  Codecs: none. */
    { "vs",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("vs", q_vs_wrap), NULL, 1, 0, "none",
      "“Vector from scalar”", "qdocs/docs/docs/docs/ref/vs.md",
      "x vs y    vs[x;y]", NULL },
    { "sv",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("sv", q_sv_wrap), NULL, 1, 0, "none",
      "“Scalar from vector”", "qdocs/docs/docs/docs/ref/sv.md",
      "x sv y    sv[x;y]", NULL },
    /* ---- set operations (feat/q-setops) — rowid (item identity, spec §3) ---- */
    /* q `x except y` — items of x not in y, x-duplicates and order KEPT
     * (ref/except.md).  rayfall's ray_except_fn is exactly this for lists;
     * the q_except_wrap adds a TABLE-pair arm (row membership) and delegates
     * everything else to ray_except_fn unchanged (was a pure QR_ENV row
     * pre-table-verbs).  Target stays "except" for provenance/serde. */
    { "except",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("except", q_except_wrap), NULL, 1, 0, "rowid",
      "Exclude items from a list", "qdocs/docs/docs/docs/ref/except.md",
      "x except y    except[x;y]", "1 2 3 except 2 -> 1 3" },
    /* q `x union y` == `distinct x,y` — WRAPPER: rayfall union keeps
     * x-duplicates, kdb dedups the whole join (ref/union.md). */
    { "union", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("union", q_union_wrap), NULL, 1, 0, "rowid",
      "Union of two lists", "qdocs/docs/docs/docs/ref/union.md",
      "x union y    union[x;y]", "1 2 union 2 3 -> 1 2 3" },
    /* q `x inter y` — items of x that are in y, x-duplicates and order KEPT
     * (ref/inter.md: "uses the result of x in y to return items from x").
     * rayfall spells it `sect` (ray_sect_fn) and is exactly this for lists;
     * the thin wrapper only guards dict/table operands 'nyi (rayfall sect
     * returns a wrong-shaped dict there; kdb returns common values). */
    { "inter", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("sect", q_inter_wrap), NULL, 1, 0, "rowid",
      "Intersection of two lists or dictionaries", "qdocs/docs/docs/docs/ref/inter.md",
      "x inter y    inter[x;y]", "1 2 3 inter 2 3 4 -> 2 3" },
    /* q `x cross y` == {raze x,/:\:y} — Cartesian product WRAPPER (no
     * rayfall cartesian primitive; composes item access + q join).  String
     * and dict/table operands are deferred cells (ref/cross.md).  Border
     * ruling: structural (see FAMILY AUDIT). */
    { "cross", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("cross", q_cross_wrap), NULL, 1, 0, "structural",
      "Cross product", "qdocs/docs/docs/docs/ref/cross.md",
      "x cross y    cross[x;y]", NULL },
    /* ---- q join family (feat/q-joins-rebuild) ----
     * Dyadic infix keywords over the engine hash/asof join (pair-relation
     * core in q_wrap_join.c: rowid-augmented key tables through
     * ray_left_join_fn / ray_asof_join_fn, kdb-ordered, gather-assembled).
     * Family: structural (schema-defining; border ruling in the AUDIT). */
    { "lj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("lj", q_lj_wrap),   NULL, 1, 0, "structural",
      "Left join", "qdocs/docs/docs/docs/ref/lj.md",
      "x lj  y     lj [x;y]", NULL },
    { "ljf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ljf", q_ljf_wrap), NULL, 1, 0, "structural",
      "Left join", "qdocs/docs/docs/docs/ref/lj.md",
      "x ljf y     ljf[x;y]", NULL },
    { "ij",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ij", q_ij_wrap),   NULL, 1, 0, "structural",
      "Inner join", "qdocs/docs/docs/docs/ref/ij.md",
      "x ij  y     ij [x;y]", NULL },
    { "ijf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ijf", q_ijf_wrap), NULL, 1, 0, "structural",
      "Inner join", "qdocs/docs/docs/docs/ref/ij.md",
      "x ijf y     ijf[x;y]", NULL },
    { "uj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("uj", q_uj_wrap),   NULL, 1, 0, "structural",
      "Union join", "qdocs/docs/docs/docs/ref/uj.md",
      "x uj  y     uj [x;y]", NULL },
    { "ujf",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ujf", q_ujf_wrap), NULL, 1, 0, "structural",
      "Union join", "qdocs/docs/docs/docs/ref/uj.md",
      "x ujf y     ujf[x;y]", NULL },
    { "pj",    QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("pj", q_pj_wrap),   NULL, 1, 0, "structural",
      "Plus join", "qdocs/docs/docs/docs/ref/pj.md",
      "x pj y     pj[x;y]", NULL },
    { "asof",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("asof", q_asof_wrap), NULL, 1, 0, "structural",
      "As-of join", "qdocs/docs/docs/docs/ref/asof.md",
      "t asof d     asof[t;d]", NULL },
    /* Bracket-form joins (ej[c;t1;t2], aj[...], wj[w;f;t;spec]): triadic-plus
     * prefix keywords — q-owned env VARY bindings (q_builtins_register),
     * snapshotted here as QR_ENV rows (the ssr precedent: the parser
     * name-refs `ej[a;b;c]`, so it resolves through the env). */
    { "ej",    QLEX_KW_PREFIX, QR_ENV("ej"),                   QR_NONE,           NULL, 1, 0, "structural",
      "Equi join", "qdocs/docs/docs/docs/ref/ej.md",
      "ej[c;t1;t2]", NULL },
    { "aj",    QLEX_KW_PREFIX, QR_ENV("aj"),                   QR_NONE,           NULL, 1, 0, "structural",
      "As-of join", "qdocs/docs/docs/docs/ref/aj.md#aj",
      "aj  [c; t1; t2]", NULL },
    { "aj0",   QLEX_KW_PREFIX, QR_ENV("aj0"),                  QR_NONE,           NULL, 1, 0, "structural",
      "As-of join", "qdocs/docs/docs/docs/ref/aj.md#aj0",
      "aj0 [c; t1; t2]", NULL },
    { "ajf",   QLEX_KW_PREFIX, QR_ENV("ajf"),                  QR_NONE,           NULL, 1, 0, "structural",
      "As-of join", "qdocs/docs/docs/docs/ref/aj.md#ajf",
      "ajf [c; t1; t2]", NULL },
    { "ajf0",  QLEX_KW_PREFIX, QR_ENV("ajf0"),                 QR_NONE,           NULL, 1, 0, "structural",
      "As-of join", "qdocs/docs/docs/docs/ref/aj.md#ajf0",
      "ajf0[c; t1; t2]", NULL },
    { "wj",    QLEX_KW_PREFIX, QR_ENV("wj"),                   QR_NONE,           NULL, 1, 0, "structural",
      "Window join", "qdocs/docs/docs/docs/ref/wj.md",
      "wj [w; c; t; (q; (f0;c0); (f1;c1))]", NULL },
    { "wj1",   QLEX_KW_PREFIX, QR_ENV("wj1"),                  QR_NONE,           NULL, 1, 0, "structural",
      "Window join", "qdocs/docs/docs/docs/ref/wj.md",
      "wj1[w; c; t; (q; (f0;c0); (f1;c1))]", NULL },
    /* ---- sort / bucket family (feat/q-sort-rank) — dyadic infix ----
     * `bin`/`binr` reuse rayfall verbatim (same arg order: sorted-vec left,
     * value right; ordering-based search -> rowid).  `xrank` is a q.q
     * derivation over the grade (QR_QSRC; ref/xrank.md "right-uniform" — the
     * ordering essence puts it in rowid).  `xbar` is an ARG-SWAP wrapper
     * (rayfall xbar is (col,bucket); q spells it (bucket,col)) — "xbar is
     * atomic" (ref/xbar.md).  All infix so `a verb b` is one stmt. */
    { "bin",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("bin"),     NULL, 1, 0, "rowid",
      "Binary search", "qdocs/docs/docs/docs/ref/bin.md",
      "x bin  y    bin[x;y]", NULL },
    { "binr",  QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("binr"),    NULL, 1, 0, "rowid",
      "Binary search", "qdocs/docs/docs/docs/ref/bin.md",
      "x binr y    binr[x;y]", NULL },
    { "xrank", QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("xrank"),  NULL, 1, 0, "rowid",
      "Group by value", "qdocs/docs/docs/docs/ref/xrank.md",
      "x xrank y     xrank[x;y]", NULL },
    { "xbar",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("xbar", q_xbar_wrap), NULL, 1, 0, "atomic",
      "Round down", "qdocs/docs/docs/docs/ref/xbar.md",
      "x xbar y    xbar[x;y]", "5 xbar 12 -> 10" },
    /* ---- Wave 5: running scans (monadic prefix keywords) — all doc-labelled
     * uniform (ref/{sum,prd,max,min,avg}.md); null discipline at the bodies
     * (q_wrap_agg.c).  `ratios` is a border case: its page's LABEL says
     * aggregate, its documented behaviour is uniform (see FAMILY AUDIT). ---- */
    { "sums",  QLEX_KW_PREFIX, QR_FN1("sums", q_sums_wrap),    QR_NONE,           NULL, 1, 0, "map",
      "Running totals", "qdocs/docs/docs/docs/ref/sum.md#sums",
      "sums x    sums[x]", "sums 1 2 3 -> 1 3 6" },
    { "prds",  QLEX_KW_PREFIX, QR_FN1("prds", q_prds_wrap),    QR_NONE,           NULL, 1, 0, "map",
      "Products", "qdocs/docs/docs/docs/ref/prd.md#prds",
      "prds x    prds[x]", "prds 1 2 3 -> 1 2 6" },
    /* q `prd x` — the multiply-over fold twin of prds (ref/prd.md: aggregate):
     * nulls are 1s, bool vector -> int, list-of-lists element-wise, dict/table
     * implicit iteration.  Wrapper (no rayfall product aggregate). */
    { "prd",   QLEX_KW_PREFIX, QR_FN1("prd", q_prd_wrap),      QR_NONE,           NULL, 1, 0, "aggregate",
      "Product", "qdocs/docs/docs/docs/ref/prd.md#prd",
      "prd x    prd[x]", "prd 1 2 3 4 -> 24" },
    { "maxs",  QLEX_KW_PREFIX, QR_FN1("maxs", q_maxs_wrap),    QR_NONE,           NULL, 1, 0, "map",
      "Maximums", "qdocs/docs/docs/docs/ref/max.md#maxs",
      "maxs x    maxs[x]", "maxs 1 3 2 -> 1 3 3" },
    { "mins",  QLEX_KW_PREFIX, QR_FN1("mins", q_mins_wrap),    QR_NONE,           NULL, 1, 0, "map",
      "Minimums", "qdocs/docs/docs/docs/ref/min.md#mins",
      "mins x     mins[x]", "mins 3 1 2 -> 3 1 1" },
    { "avgs",  QLEX_KW_PREFIX, QR_FN1("avgs", q_avgs_wrap),    QR_NONE,           NULL, 1, 0, "map",
      "Running averages", "qdocs/docs/docs/docs/ref/avg.md#avgs",
      "avgs x     avgs[x]", "avgs 1 2 3 -> 1 1.5 2" },
    { "ratios",QLEX_KW_PREFIX, QR_FN1("ratios", q_ratios_wrap),QR_NONE,           NULL, 1, 0, "map",
      "Ratios between items", "qdocs/docs/docs/docs/ref/ratios.md",
      "ratios y     ratios[y]", "ratios 1 2 4 -> 1 2 2f" },
    /* ---- Wave 5: weighted (dyadic infix keywords) — aggregates
     * (ref/sum.md wsum, ref/avg.md wavg).  wsum is q.q-hosted; wavg stays a
     * wrapper (its composition's bool-multiply path measured 16x slower). ---- */
    { "wsum",  QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("wsum"),   NULL, 1, 0, "aggregate",
      "Weighted sum", "qdocs/docs/docs/docs/ref/sum.md#wsum",
      "x wsum y    wsum[x;y]", NULL },
    { "wavg",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("wavg", q_wavg_wrap), NULL, 1, 0, "aggregate",
      "Weighted average", "qdocs/docs/docs/docs/ref/avg.md#wavg",
      "x wavg y     wavg[x;y]", NULL },
    /* ---- Wave 5: statistical (renames of audited base aggregates; all
     * doc-labelled aggregate — ref/{med,var,dev,cor,cov}.md) ----
     * kdb `var`/`dev` are POPULATION (÷n); rayfall `var`/`stddev` are SAMPLE
     * (÷n-1) and `var_pop`/`stddev_pop`/`dev` are population — so q `var`->
     * `var_pop`, q `svar`->`var`, q `sdev`->`stddev`, q `dev` stays `dev`. */
    /* `med` is self-hosted in q.q (ref/med.md docs formula) — no row; the
     * bootstrap shadow-rebind points root `med` at `.q.med` (the engine env
     * `med` would otherwise win name resolution). */
    { "var",   QLEX_KW_PREFIX, QR_ENV("var_pop"),              QR_NONE,           NULL, 1, 0, "aggregate",
      "Variance", "qdocs/docs/docs/docs/ref/var.md#var",
      "var x    var[x]", NULL },
    { "svar",  QLEX_KW_PREFIX, QR_ENV("var"),                  QR_NONE,           NULL, 1, 0, "aggregate",
      "Sample variance", "qdocs/docs/docs/docs/ref/var.md#svar",
      "svar x    svar[x]", NULL },
    { "sdev",  QLEX_KW_PREFIX, QR_ENV("stddev"),               QR_NONE,           NULL, 1, 0, "aggregate",
      "Sample standard deviation", "qdocs/docs/docs/docs/ref/dev.md#sdev",
      "sdev x     sdev[x]", NULL },
    { "cor",   QLEX_KW_INFIX,  QR_NONE,                        QR_ENV("pearson_corr"), NULL, 1, 0, "aggregate",
      "Correlation", "qdocs/docs/docs/docs/ref/cor.md",
      "x cor y    cor[x;y]", NULL },
    { "cov",   QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("cov"),    NULL, 1, 0, "aggregate",
      "Covariance", "qdocs/docs/docs/docs/ref/cov.md#cov",
      "x cov y    cov[x;y]", NULL },
    { "scov",  QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("scov"),   NULL, 1, 0, "aggregate",
      "Sample covariance", "qdocs/docs/docs/docs/ref/cov.md#scov",
      "x scov y    scov[x;y]", NULL },
    /* ---- Wave 5: sliding m-windows + ema (dyadic infix keywords) — all
     * doc-labelled uniform (ref/{sum,avg,max,min,dev,count,ema}.md) ---- */
    { "msum",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("msum", q_msum_wrap), NULL, 1, 0, "map",
      "Moving sums", "qdocs/docs/docs/docs/ref/sum.md#msum",
      "x msum y    msum[x;y]", NULL },
    { "mavg",  QLEX_KW_INFIX,  QR_NONE,                        QR_QSRC("mavg"),   NULL, 1, 0, "map",
      "Moving averages", "qdocs/docs/docs/docs/ref/avg.md#mavg",
      "x mavg y     mavg[x;y]", NULL },
    { "mmax",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mmax", q_mmax_wrap), NULL, 1, 0, "map",
      "Moving maximums", "qdocs/docs/docs/docs/ref/max.md#mmax",
      "x mmax y    mmax[x;y]", NULL },
    { "mmin",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mmin", q_mmin_wrap), NULL, 1, 0, "map",
      "Moving minimums", "qdocs/docs/docs/docs/ref/min.md#mmin",
      "x mmin y     mmin[x;y]", NULL },
    { "mcount",QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mcount", q_mcount_wrap), NULL, 1, 0, "map",
      "Moving counts", "qdocs/docs/docs/docs/ref/count.md#mcount",
      "x mcount y     mcount[x;y]", NULL },
    { "mdev",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("mdev", q_mdev_wrap), NULL, 1, 0, "map",
      "Moving deviations", "qdocs/docs/docs/docs/ref/dev.md#mdev",
      "x mdev y     mdev[x;y]", NULL },
    { "ema",   QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("ema", q_ema_wrap), NULL, 1, 0, "map",
      "Exponential moving average", "qdocs/docs/docs/docs/ref/ema.md",
      "x ema y    ema[x;y]", NULL },
    /* iterator mnemonic keywords (wave-2): infix `f over/scan/prior/peach x`,
     * same lexical treatment as `each`.  over/scan dispatch reduce/converge/
     * do/while by f rank; prior is unary each-prior; peach == each. */
    { "over",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("over", q_over_kw), NULL, 1, 0, "none",
      "wrappers for the Over and Scan accumulating iterators", "qdocs/docs/docs/docs/ref/over.md",
      NULL, NULL },
    { "scan",  QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("scan-kw", q_scan_kw), NULL, 1, 0, "none",
      "wrappers for the Over and Scan accumulating iterators", "qdocs/docs/docs/docs/ref/over.md",
      NULL, NULL },
    { "prior", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("prior", q_prior_kw), NULL, 1, 0, "none",
      "a wrapper for the Each Prior iterator", "qdocs/docs/docs/docs/ref/prior.md",
      " v2 prior x      prior[v2;x]", NULL },
    { "peach", QLEX_KW_INFIX,  QR_NONE,                        QR_FN2("peach", q_each_wrap), NULL, 1, 0, "none",
      "Iterate a unary", "qdocs/docs/docs/docs/ref/each.md#peach",
      " v1 each x   each[v1;x]       v1 peach x   peach[v1;x]", NULL },
    /* ---- keyword-prefix monads (pass-through/rename) ---- */
    { "neg",     QLEX_KW_PREFIX, QR_FN1A("neg", q_neg_wrap),   QR_NONE,           NULL, 1, 0, "atomic",
      "Negate", "qdocs/docs/docs/docs/ref/neg.md",
      "neg x    neg[x]", "neg 3 -2 1 -> -3 2 -1" },
    /* q `til` — generator (atom -> vector; no structure input, family none);
     * kdb accepts a boolean (`til 1b` -> ,0), base ray_til_fn is int-only. */
    { "til",     QLEX_KW_PREFIX, QR_FN1("til", q_til_wrap),    QR_NONE,           NULL, 1, 0, "none",
      "First x natural numbers", "qdocs/docs/docs/docs/ref/til.md",
      "til x    til[x]", "til 5 -> 0 1 2 3 4" },
    { "count",   QLEX_KW_PREFIX, QR_ENV("count"),              QR_NONE,           NULL, 1, 0, "aggregate",
      "Number of items", "qdocs/docs/docs/docs/ref/count.md#count",
      "count x     count[x]", "count 10 20 30 -> 3" },
    /* first/last — aggregates (ref/first.md: "first is an aggregate"). */
    { "first",   QLEX_KW_PREFIX, QR_ENV("first"),              QR_NONE,           NULL, 1, 0, "aggregate",
      "First item of a list", "qdocs/docs/docs/docs/ref/first.md#first",
      "first x    first[x]", "first 10 20 30 -> 10" },
    { "last",    QLEX_KW_PREFIX, QR_ENV("last"),               QR_NONE,           NULL, 1, 0, "aggregate",
      "Last item of a list", "qdocs/docs/docs/docs/ref/first.md#last",
      "last x    last[x]", "last 10 20 30 -> 30" },
    /* q `n xprev x` — n-item shift (ref/next.md: right-uniform), null-filling
     * the vacated end; dyadic infix like rotate.  L4 index: a shift IS a
     * gather by shifted indices (spec §2 lists next/prev/xprev in the index
     * family).  `next`/`prev` are its unit shifts, self-hosted in q.q
     * (`.q.next:xprev[-1;]` / `.q.prev:xprev[1;]`) — no manifest rows. */
    { "xprev",   QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("xprev", q_xprev_wrap), NULL, 1, 0, "index",
      "Nearby items in a list", "qdocs/docs/docs/docs/ref/next.md#xprev",
      "x xprev y     xprev[x;y]", NULL },
    /* q `fills x` — forward-fill nulls (ref/fill.md: uniform; the `^\`
     * fill-scan).  Computes values (not a gather) -> map. */
    { "fills",   QLEX_KW_PREFIX, QR_FN1("fills", q_fills_wrap), QR_NONE,          NULL, 1, 0, "map",
      "Replace nulls with preceding non-nulls", "qdocs/docs/docs/docs/ref/fill.md#fills",
      "fills x     fills[x]", NULL },
    /* q `where x` — the L4 index-space generator (border ruling: index —
     * see FAMILY AUDIT); int-vector replicate + bool-mask forms at the body. */
    { "where",   QLEX_KW_PREFIX, QR_FN1("where", q_where_wrap), QR_NONE,          NULL, 1, 0, "index",
      "Copies of indexes of a list or keys of a dictionary", "qdocs/docs/docs/docs/ref/where.md",
      "where x    where[x]", "where 0 1 0 1 1 -> 1 3 4" },
    { "reverse", QLEX_KW_PREFIX, QR_FN1("reverse", q_reverse_wrap), QR_NONE,      NULL, 1, 0, "index",
      "Reverse the order of items of a list or dictionary", "qdocs/docs/docs/docs/ref/reverse.md",
      "reverse x    reverse[x]", "reverse 2 4 6 -> 6 4 2" },
    /* q `sum` over a boxed LIST sums the items (`sum(dates;times)` ->
     * timestamps, ref/file-text.md); rayfall sum is vector-only, so wrapper.
     * NB the wrapper is deliberately NOT RAY_FN_AGGR — the eval aggregate
     * fast path claims AGGR fns before the wrapper runs and 'types on a boxed
     * list-of-vectors; name-routing (RAY_FN_Q_LOWER + aux "sum") keeps
     * query/DAG behaviour. */
    { "sum",     QLEX_KW_PREFIX, QR_FN1("sum", q_sum_wrap),    QR_NONE,           NULL, 1, 0, "aggregate",
      "Total", "qdocs/docs/docs/docs/ref/sum.md#sum",
      "sum x    sum[x]", "sum 1 2 3 4 -> 10" },
    /* group — rowid (item equality; spec §3 lists it; a §9 border flag). */
    { "group",   QLEX_KW_PREFIX, QR_ENV("group"),              QR_NONE,           NULL, 1, 0, "rowid",
      "returns a dictionary in which the keys are the distinct items of its argument, and the values the indexes where the distinct items occur", "qdocs/docs/docs/docs/ref/group.md",
      "group x     group[x]", NULL },
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
    { "iasc",    QLEX_KW_PREFIX, QR_FN1("iasc", q_iasc_wrap),  QR_NONE,           NULL, 1, 0, "rowid",
      "Ascending grade", "qdocs/docs/docs/docs/ref/asc.md#iasc",
      "iasc x    iasc[x]", "iasc 3 1 2 -> 1 2 0" },
    { "idesc",   QLEX_KW_PREFIX, QR_FN1("idesc", q_idesc_wrap), QR_NONE,          NULL, 1, 0, "rowid",
      "Descending grade", "qdocs/docs/docs/docs/ref/desc.md#idesc",
      "idesc x    idesc[x]", "idesc 3 1 2 -> 0 2 1" },
    { "avg",     QLEX_KW_PREFIX, QR_ENV("avg"),                QR_NONE,           NULL, 1, 0, "aggregate",
      "Arithmetic mean", "qdocs/docs/docs/docs/ref/avg.md#avg",
      "avg x     avg[x]", "avg 1 2 3 4 -> 2.5" },
    /* q `floor` returns LONGS from floats (kdb `floor 3.7` is 3j); rayfall's
     * env floor keeps f64, so this is the q_floor_wrap, not a rename. */
    { "floor",   QLEX_KW_PREFIX, QR_FN1A("floor", q_floor_wrap), QR_NONE,         NULL, 1, 0, "atomic",
      "Round down", "qdocs/docs/docs/docs/ref/floor.md",
      "floor x    floor[x]", "floor 2.7 3.1 -> 2 3" },
    /* q dict accessors: `key`/`value` are wrappers (dict-only in 2c-2 —
     * the file-handle/namespace/enumeration overloads are deferred cells);
     * `distinct` must preserve FIRST-OCCURRENCE order (kdb ref/distinct.md),
     * while rayfall's distinct routes typed vectors through the DAG group
     * path, which sorts — so it too is a wrapper, not a rename (audited:
     * `distinct 2 3 7 3 5 3` must be 2 3 7 5, env distinct gives 2 3 5 7). */
    { "key",     QLEX_KW_PREFIX, QR_FN1("key", q_key_wrap),    QR_NONE,           NULL, 1, 0, "structural",
      "Keys of a dictionary; key columns of a keyed table; files in a folder; whether a file or name exists; target of a foreign key, type of a vector; or the enumerator of a list", "qdocs/docs/docs/docs/ref/key.md",
      "key x     key[x]", NULL },
    { "value",   QLEX_KW_PREFIX, QR_FN1("value", q_value_wrap), QR_NONE,          NULL, 1, 0, "structural",
      "Recurse the interpreter", "qdocs/docs/docs/docs/ref/value.md",
      "value x     value[x]", NULL },
    /* q `get` is a SYNONYM of `value` (ref/get.md: "completely interchangeable")
     * — same q_value_wrap, one home.  `nam set y` writes the named global
     * (sym-handle assign / `. context restore); file forms are 'nyi cells. */
    { "get",     QLEX_KW_PREFIX, QR_FN1("value", q_value_wrap), QR_NONE,          NULL, 1, 0, "structural",
      "Read or memory-map a variable or kdb+ data file", "qdocs/docs/docs/docs/ref/get.md#get",
      "get x     get[x]", NULL },
    { "set",     QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("set-g", q_setg_wrap), NULL, 1, 1, "none",
      "Read or set the value of a variable or a kdb+ data file", "qdocs/docs/docs/docs/ref/get.md#set",
      NULL, NULL },
    { "distinct",QLEX_KW_PREFIX, QR_FN1("distinct", q_distinct_wrap), QR_NONE,    NULL, 1, 0, "rowid",
      "Unique items of a list", "qdocs/docs/docs/docs/ref/distinct.md",
      "distinct x    distinct[x]", "distinct 1 1 2 3 3 -> 1 2 3" },
    /* q `rand x` == {first 1?x} — self-hosted in q.q verbatim from ref/rand.md;
     * no row.  The bootstrap shadow-rebind points root `rand` at `.q.rand`
     * (rayfall's env `rand` is dyadic and would otherwise win name
     * resolution; the `?` roll wrapper calls ray_rand_fn directly). */
    /* q-implemented keywords: env bindings added by q_builtins_register
     * (same mechanism as `parse`), snapshotted here as pass-through rows.
     * string is atomic (ref/string.md: "atomic … iterates through
     * dictionaries and tables"); upper/lower likewise elementwise.  Atomic
     * here is at STRING granularity (see the FAMILY AUDIT caveat). */
    { "string",  QLEX_KW_PREFIX, QR_ENV("string"),             QR_NONE,           NULL, 1, 0, "atomic",
      "Cast to string", "qdocs/docs/docs/docs/ref/string.md",
      "string x    string[x]", "string 42 -> \"42\"" },
    { "upper",   QLEX_KW_PREFIX, QR_ENV("upper"),              QR_NONE,           NULL, 1, 0, "atomic",
      "Shift case", "qdocs/docs/docs/docs/ref/lower.md",
      "upper x     upper[x]", "upper \"abc\" -> \"ABC\"" },
    { "lower",   QLEX_KW_PREFIX, QR_ENV("lower"),              QR_NONE,           NULL, 1, 0, "atomic",
      "Shift case", "qdocs/docs/docs/docs/ref/lower.md",
      "lower x     lower[x]", "lower \"ABC\" -> \"abc\"" },
    /* ---- string trim / hash / search family (feat/q-string-fns) ----
     * trim/ltrim/rtrim/md5 are q-owned env unaries (q_builtins_register),
     * snapshotted here as QR_ENV prefix rows (same mechanism as `string`).
     * (trim stays C: its q.q form {ltrim rtrim x} measured 3.7x on a 1M
     * string list — two passes + an intermediate list.)  trim family is
     * elementwise over string atoms (atomic at string granularity); md5
     * consumes a whole string as a digest codec (none).  `like`/`ss` are
     * dyadic infix; `ssr` is a triadic-prefix wrapper. */
    { "trim",    QLEX_KW_PREFIX, QR_ENV("trim"),               QR_NONE,           NULL, 1, 0, "atomic",
      "Remove leading or trailing nulls from a list", "qdocs/docs/docs/docs/ref/trim.md",
      " trim x     trim[x]", NULL },
    { "ltrim",   QLEX_KW_PREFIX, QR_ENV("ltrim"),              QR_NONE,           NULL, 1, 0, "atomic",
      "Remove leading or trailing nulls from a list", "qdocs/docs/docs/docs/ref/trim.md",
      "ltrim x    ltrim[x]", NULL },
    { "rtrim",   QLEX_KW_PREFIX, QR_ENV("rtrim"),              QR_NONE,           NULL, 1, 0, "atomic",
      "Remove leading or trailing nulls from a list", "qdocs/docs/docs/docs/ref/trim.md",
      "rtrim x    rtrim[x]", NULL },
    { "md5",     QLEX_KW_PREFIX, QR_ENV("md5"),                QR_NONE,           NULL, 1, 0, "none",
      "Message Digest hash", "qdocs/docs/docs/docs/ref/md5.md",
      "md5 x    md5[x]", NULL },
    { "like",    QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("like", q_like_wrap), NULL, 1, 0, "atomic",
      "Whether text matches a pattern", "qdocs/docs/docs/docs/ref/like.md",
      "x like y    like[x;y]", NULL },
    { "ss",      QLEX_KW_INFIX,  QR_NONE,                      QR_FN2("ss", q_ss_wrap), NULL, 1, 0, "none",
      "String search", "qdocs/docs/docs/docs/ref/ss.md#ss",
      "x ss y     ss[x;y]", NULL },
    { "ssr",     QLEX_KW_PREFIX, QR_ENV("ssr"),                QR_NONE,           NULL, 1, 0, "none",
      "String search and replace", "qdocs/docs/docs/docs/ref/ss.md#ssr",
      "ssr[x;y;z]", NULL },
    { "show",    QLEX_KW_PREFIX, QR_ENV("show"),               QR_NONE,           NULL, 1, 1, "none",
      "Format and display at the console.", "qdocs/docs/docs/docs/ref/show.md",
      "show x    show[x]", NULL },
    /* `system "…"` — q-owned env unary (q_builtins_register), snapshotted here
     * so the parser embeds it; routes through q_sys_dispatch (q_sys.c). */
    { "system",  QLEX_KW_PREFIX, QR_ENV("system"),             QR_NONE,           NULL, 1, 1, "none",
      "Execute a system command", "qdocs/docs/docs/docs/ref/system.md",
      "system x     system[x]", NULL },
    /* table introspection — q-owned bindings (q_builtins_register), snapshotted
     * here so the parser embeds them over the base env `meta`. */
    { "meta",    QLEX_KW_PREFIX, QR_ENV("meta"),               QR_NONE,           NULL, 1, 0, "structural",
      "Metadata for a table", "qdocs/docs/docs/docs/ref/meta.md",
      "meta x    meta[x]", NULL },
    { "cols",    QLEX_KW_PREFIX, QR_ENV("cols"),               QR_NONE,           NULL, 1, 0, "structural",
      "Column names of a table", "qdocs/docs/docs/docs/ref/cols.md#cols",
      "cols x    cols[x]", NULL },
    /* q `attr x` — column attribute as a single symbol (ref/set-attribute.md,
     * ref/attr.md).  Monadic wrapper over the engine's `.attr.get`, collapsing
     * the full-name sym vector to kdb's one letter.  The set/clear side is the
     * `#` verb's symbol-atom arm (q_take_wrap), not a row here. */
    { "attr",    QLEX_KW_PREFIX, QR_FN1("attr", q_attr_wrap),  QR_NONE,           NULL, 1, 0, "structural",
      "Attribute of an object", "qdocs/docs/docs/docs/ref/attr.md",
      "attr x     attr[x]", NULL },
    /* ---- additional monadic pass-through keywords (rayfall name == q name,
     * audited kdb-true element-wise / aggregate semantics; atomic labels per
     * ref/{abs,exp,log,sqrt,null}.md, aggregate per ref/{dev,max,min}.md). See
     * docs/recipes/add-q-keyword-verb.md. ---- */
    { "abs",     QLEX_KW_PREFIX, QR_ENV("abs"),                QR_NONE,           NULL, 1, 0, "atomic",
      "Absolute value", "qdocs/docs/docs/docs/ref/abs.md",
      "abs x    abs[x]", "abs -3 2 -1 -> 3 2 1" },
    /* q `null x` — elementwise null test (ref/null.md: atomic).  Routes to
     * the engine's atomic `nil?` (RAY_FN_ATOMIC): broadcasts over vectors and
     * nested lists at every depth.  The wrapper collapses a homogeneous
     * top-level bool-atom run (heterogeneous input list) to a bool vector
     * for kdb-true display. */
    { "null",    QLEX_KW_PREFIX, QR_FN1("nil?", q_null_wrap),  QR_NONE,           NULL, 1, 0, "atomic",
      "Is null", "qdocs/docs/docs/docs/ref/null.md",
      "null x     null[x]", NULL },
    { "dev",     QLEX_KW_PREFIX, QR_ENV("dev"),                QR_NONE,           NULL, 1, 0, "aggregate",
      "Standard deviation", "qdocs/docs/docs/docs/ref/dev.md#dev",
      "dev x     dev[x]", NULL },
    { "exp",     QLEX_KW_PREFIX, QR_ENV("exp"),                QR_NONE,           NULL, 1, 0, "atomic",
      "Raise e to a power", "qdocs/docs/docs/docs/ref/exp.md#exp",
      "exp x     exp[x]", "exp 0 -> 1f" },
    { "log",     QLEX_KW_PREFIX, QR_ENV("log"),                QR_NONE,           NULL, 1, 0, "atomic",
      "Natural logarithm", "qdocs/docs/docs/docs/ref/log.md#log",
      "log x    log[x]", "log 1 -> 0f" },
    { "max",     QLEX_KW_PREFIX, QR_ENV("max"),                QR_NONE,           NULL, 1, 0, "aggregate",
      "Maximum", "qdocs/docs/docs/docs/ref/max.md#max",
      "max x    max[x]", "max 3 1 2 -> 3" },
    { "min",     QLEX_KW_PREFIX, QR_ENV("min"),                QR_NONE,           NULL, 1, 0, "aggregate",
      "Minimum", "qdocs/docs/docs/docs/ref/min.md#min",
      "min x     min[x]", "min 3 1 2 -> 1" },
    /* rank == iasc iasc (ref/rank.md) — the grade family, rowid. */
    /* q `raze x` — flattens one level of structure (ref/raze.md; base
     * ray_raze_fn plus the kdb atom arm `raze 42` -> ,42). */
    { "raze",    QLEX_KW_PREFIX, QR_FN1("raze", q_raze_wrap),  QR_NONE,           NULL, 1, 0, "structural",
      "Return the items of `x` joined, collapsing one level of nesting", "qdocs/docs/docs/docs/ref/raze.md",
      "raze x    raze[x]", "raze (1 2;3 4) -> 1 2 3 4" },
    { "sqrt",    QLEX_KW_PREFIX, QR_ENV("sqrt"),               QR_NONE,           NULL, 1, 0, "atomic",
      "Square root", "qdocs/docs/docs/docs/ref/sqrt.md",
      "sqrt x    sqrt[x]", "sqrt 4 9 16 -> 2 3 4f" },
    /* ---- atomic unary math (feat/q-math-atomic) — implement-via-libm, no
     * rayfall counterpart (rayfall has exp/log/sqrt but not the trig set).
     * All monadic KW_PREFIX, doc-labelled atomic (ref/{trig,
     * signum,ceiling}.md); sentinel-null discipline at the bodies
     * (q_wrap_math.c).  `ceiling` is the floor-wrapper twin (float->LONG),
     * NOT rayfall `ceil` (f64).  `signum` is family-atomic but built WITHOUT
     * RAY_FN_ATOMIC — it drives its own broadcast so a top-level boxed-list
     * result collapses to an int vector (see q_signum_wrap). */
    { "sin",       QLEX_KW_PREFIX, QR_FN1A("sin", q_sin_wrap),   QR_NONE, NULL, 1, 0, "atomic",
      "Sine, arcsine", "qdocs/docs/docs/docs/ref/sin.md",
      "sin x     sin[x]", NULL },
    { "cos",       QLEX_KW_PREFIX, QR_FN1A("cos", q_cos_wrap),   QR_NONE, NULL, 1, 0, "atomic",
      "Cosine, arccosine", "qdocs/docs/docs/docs/ref/cos.md",
      "cos x     cos[x]", NULL },
    { "tan",       QLEX_KW_PREFIX, QR_FN1A("tan", q_tan_wrap),   QR_NONE, NULL, 1, 0, "atomic",
      "Tangent and arctangent", "qdocs/docs/docs/docs/ref/tan.md",
      "tan x     tan[x]", NULL },
    { "asin",      QLEX_KW_PREFIX, QR_FN1A("asin", q_asin_wrap), QR_NONE, NULL, 1, 0, "atomic",
      "Sine, arcsine", "qdocs/docs/docs/docs/ref/sin.md",
      "asin x    asin[x]", NULL },
    { "acos",      QLEX_KW_PREFIX, QR_FN1A("acos", q_acos_wrap), QR_NONE, NULL, 1, 0, "atomic",
      "Cosine, arccosine", "qdocs/docs/docs/docs/ref/cos.md",
      "acos x    acos[x]", NULL },
    { "atan",      QLEX_KW_PREFIX, QR_FN1A("atan", q_atan_wrap), QR_NONE, NULL, 1, 0, "atomic",
      "Tangent and arctangent", "qdocs/docs/docs/docs/ref/tan.md",
      "atan x    atan[x]", NULL },
    /* `reciprocal` is self-hosted in q.q (`.q.reciprocal:%[1;]`) — no row. */
    { "signum",    QLEX_KW_PREFIX, QR_FN1("signum", q_signum_wrap), QR_NONE, NULL, 1, 0, "atomic",
      "returns 1, 0,or -1 according to the sign of its argument", "qdocs/docs/docs/docs/ref/signum.md",
      "signum x    signum[x]", "signum -3 0 2 -> -1 0 1i" },
    { "ceiling",   QLEX_KW_PREFIX, QR_FN1A("ceiling", q_ceiling_wrap), QR_NONE, NULL, 1, 0, "atomic",
      "Round up", "qdocs/docs/docs/docs/ref/ceiling.md",
      "ceiling x      ceiling[x]", "ceiling 2.1 3.9 -> 3 4" },
    /* ---- table verbs (feat/q-table-verbs) — all wrappers in q_wrap_table.c
     * EXCEPT xcol/xcols, which are q.q derivations over `.Q.ft` (QR_QSRC:
     * bound post-bootstrap; the row is only what keeps them infix).
     * The wrappers build over the wave-4 keyed primitives (q_enkey/
     * q_table_flatten).  `insert`/`upsert` intentionally SHADOW the base env
     * special forms of the same name: q semantics differ (by-name targets,
     * keyed collision/update, row-index results), so the registry value is a
     * wrapper, never the env snapshot.
     * Families: flip/keys/xkey/xcol/xcols/ungroup/insert/upsert define or
     * rearrange the structures (structural, spec §3); xasc/xdesc sort (rowid —
     * q.q derivations grading the named columns, then ONE row gather);
     * xgroup groups (rowid — border ruling in the AUDIT). */
    { "flip",   QLEX_KW_PREFIX, QR_FN1("flip", q_flip_wrap),   QR_NONE,           NULL, 1, 0, "structural",
      "transposes its argument", "qdocs/docs/docs/docs/ref/flip.md",
      "flip x     flip[x]", NULL },
    { "keys",   QLEX_KW_PREFIX, QR_FN1("keys", q_keys_wrap),   QR_NONE,           NULL, 1, 0, "structural",
      "Key column/s of a table", "qdocs/docs/docs/docs/ref/keys.md#keys",
      "keys x    keys[x]", NULL },
    { "ungroup",QLEX_KW_PREFIX, QR_FN1("ungroup", q_ungroup_wrap), QR_NONE,       NULL, 1, 0, "structural",
      "where x is a table, in which some cells are lists, but for any row, all lists are of the same length, returns the normalized table, with one row for each item of a lists", "qdocs/docs/docs/docs/ref/ungroup.md",
      "ungroup x    ungroup[x]", NULL },
    { "xkey",   QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xkey", q_xkey_wrap), NULL, 1, 0, "structural",
      "Set specified columns as primary keys of a table", "qdocs/docs/docs/docs/ref/keys.md#xkey",
      "x xkey y    xkey[x;y]", NULL },
    { "xcol",   QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xcol"),   NULL, 1, 0, "structural",
      "Rename table columns", "qdocs/docs/docs/docs/ref/cols.md#xcol",
      "x xcol y    xcol[x;y]", NULL },
    { "xcols",  QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xcols"),  NULL, 1, 0, "structural",
      "Reorder table columns", "qdocs/docs/docs/docs/ref/cols.md#xcols",
      "x xcols y    xcols[x;y]", NULL },
    { "xasc",   QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xasc"),   NULL, 1, 0, "rowid",
      "Sort a table in ascending order of specified columns.", "qdocs/docs/docs/docs/ref/asc.md#xasc",
      "x xasc y     xasc[x;y]", NULL },
    { "xdesc",  QLEX_KW_INFIX,  QR_NONE,                       QR_QSRC("xdesc"),  NULL, 1, 0, "rowid",
      "Sorts a table in descending order of specified columns.", "qdocs/docs/docs/docs/ref/desc.md#xdesc",
      "x xdesc y    xdesc[x;y]", NULL },
    { "xgroup", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("xgroup", q_xgroup_wrap), NULL, 1, 0, "rowid",
      "Groups a table by values in selected columns", "qdocs/docs/docs/docs/ref/xgroup.md",
      "x xgroup y    xgroup[x;y]", NULL },
    { "insert", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("insert", q_insert_wrap), NULL, 1, 1, "structural",
      "Insert or append records to a table", "qdocs/docs/docs/docs/ref/insert.md",
      "x insert y    insert[x;y]", NULL },
    { "upsert", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("upsert", q_upsert_wrap), NULL, 1, 1, "structural",
      "Overwrite or append records to a table", "qdocs/docs/docs/docs/ref/upsert.md",
      "x upsert y    upsert[x;y]", NULL },
    /* each-prior mnemonics `deltas` ((-':)x) and `differ` (not(~':)x) are
     * self-hosted in q.q — no rows (both ride the each-prior HOF the C
     * wrappers called anyway; measured parity). */
    /* `any`/`all` are self-hosted in q.q (the boolean coercion the DEFER here
     * once held: cast then fold — a `max`/`min` rename is type-wrong, since
     * ref/all-any.md ranges every domain to boolean).  The rows keep the names
     * reserved and fix the lexical class; the VALUE comes from `.q`.
     * NB: `or` is the keyword spelling of `|`-max and still waits for `|`-max
     * (a valueless QLEX_KW_INFIX row would expose rayfall's scalar `or`). */
    { "any",     QLEX_KW_PREFIX, QR_NONE,                      QR_NONE,           NULL, 1, 0, "aggregate",
      "Is there a true item?", "qdocs/docs/docs/docs/ref/all-any.md#any",
      "any x    any[x]", NULL },
    { "all",     QLEX_KW_PREFIX, QR_NONE,                      QR_NONE,           NULL, 1, 0, "aggregate",
      "Is every item true?", "qdocs/docs/docs/docs/ref/all-any.md#all",
      "all x    all[x]", NULL },
    /* ---- IPC client verbs (feat/q-ipc-client, Phase D) — thin wrappers over the
     * kdb-speaking `.ipc.*` primitives (Phase C).  `hopen` normalizes int|string|
     * (conn;timeout) into the `.ipc.open` string API and returns the connection's
     * socket fd as the handle (kdb-faithful — 0/1/2 reserved, connections at 3+);
     * `hclose` translates it back and routes to `.ipc.close`.  The sync/async send
     * verb `h"query"` is handle-as-verb application (q_apply.c int-head arm), not a
     * manifest row.  Both are monadic prefix keywords (KW_PREFIX). ---- */
    { "hopen",  QLEX_KW_PREFIX, QR_FN1("hopen", q_hopen_wrap),   QR_NONE, NULL, 1, 1, "none",
      "Open a connection to a file or process", "qdocs/docs/docs/docs/ref/hopen.md#hopen",
      "hopen filehandle", NULL },
    { "hclose", QLEX_KW_PREFIX, QR_FN1("hclose", q_hclose_wrap), QR_NONE, NULL, 1, 1, "none",
      "Close a connection to a file or process", "qdocs/docs/docs/docs/ref/hopen.md#hclose",
      "hclose x     hclose[x]", NULL },
    /* ---- File Text (feat/q-file-text) — the `0:` operator + companions.
     * `0:` is tokenized by the scanner's digit-colon arm (a single 0/1/2
     * glued to ':' can never start a clock literal) and dispatches on the
     * LEFT operand's shape (see q_filetext_wrap: Save Text / Prepare Text /
     * Key-Value pairs / Load CSV / Load Fixed — text forms ONLY, binary
     * 1:/2: are an owner-ruled non-goal, so they have NO row and stay
     * name-refs ('name)).  `read0` is also env-bound by q_builtins for the
     * bracket-call form (the ssr/value precedent).  `hsym` is elementwise
     * over sym atoms/vectors (ref/hsym.md: atom or vector). */
    { "0:",     QLEX_GLYPH,     QR_NONE,                       QR_FN2("file-text", q_filetext_wrap), NULL, 1, 1, "none",
      "Read or write text", "qdocs/docs/docs/docs/ref/file-text.md",
      NULL, NULL },
    { "hsym",   QLEX_KW_PREFIX, QR_FN1("hsym", q_hsym_wrap),   QR_NONE,           NULL, 1, 0, "atomic",
      "Symbol/s to file or process symbol/s", "qdocs/docs/docs/docs/ref/hsym.md",
      "hsym x     hsym[x]", NULL },
    { "read0",  QLEX_KW_PREFIX, QR_FN1("read0", q_read0_wrap), QR_NONE,           NULL, 1, 1, "none",
      "Read text from a file or process handle", "qdocs/docs/docs/docs/ref/read0.md",
      "read0 f           read0[f]", NULL },
    /* ---- environment variables (feat/q-getenv-setenv, ref/getenv.md) ----
     * `getenv` is a monadic prefix keyword (sym -> value string, "" unset);
     * `setenv` is a dyadic infix keyword (`sym setenv str`), so it MUST be
     * KW_INFIX or the scanner would split `x setenv y` into two statements
     * (the `set`/`xkey` precedent).  Both reuse the base .os.getenv/.os.setenv
     * C primitives (ray_getenv_fn/ray_setenv_fn) via wrappers that coerce the
     * q symbol arg to the -RAY_STR the C wants — a real divergence, so QR_FN
     * wrappers, not QR_ENV renames. */
    { "getenv", QLEX_KW_PREFIX, QR_FN1("getenv", q_getenv_wrap), QR_NONE,         NULL, 1, 0, "none",
      "Get the value of an environment variable", "qdocs/docs/docs/docs/ref/getenv.md#getenv",
      "getenv x     getenv[x]", NULL },
    { "setenv", QLEX_KW_INFIX,  QR_NONE,                       QR_FN2("setenv", q_setenv_wrap), NULL, 1, 1, "none",
      "Set the value of an environment variable", "qdocs/docs/docs/docs/ref/getenv.md#setenv",
      "x setenv y     setenv[x;y]", NULL },
    /* ---- adverbs — q adverbs ARE rayfall higher-order fns (no bespoke object).
     * `+/` lowers to fold over `+` (q_lower); `/:`/`\:` ARE map-right/map-left
     * (src/ops/collection.c:2279 — map-left iterates LEFT holding right =
     * q each-left, verified against examples/rfl).  Each-prior `':` still has
     * no rayfall counterpart (the scan-* variants are fold-style, not
     * pairwise) — DEFERRED, still lexer-classified. ---- */
    { "'",     QLEX_ADVERB,    QR_NONE,  QR_NONE,  "map",       1, 0, "none",
      "Apply a value item-wise to a dictionary, list, or conforming lists and/or dictionaries.", "qdocs/docs/docs/docs/ref/maps.md#Each",
      "(v1')x    v1'[x]       v1 each x", NULL },
    { "/",     QLEX_ADVERB,    QR_NONE,  QR_NONE,  "fold",      1, 0, "none",
      "Reduce a list with a binary value, keeping the last result", NULL,  /* HAND-AUTHORED: docsrc NULL, see tools/qdocs/qdocs-docmap.pins.tsv */
      "f/ x", "(+/)1 2 3 -> 6" },
    { "\\",    QLEX_ADVERB,    QR_NONE,  QR_NONE,  "scan",      1, 0, "none",
      "Like Over, but returns each intermediate result", NULL,  /* HAND-AUTHORED */
      "f\\ x", "(+\\)1 2 3 -> 1 3 6" },
    { "':",    QLEX_ADVERB,    QR_NONE,  QR_NONE,  NULL,        1, 0, "none",
      "Each Prior: apply a binary value between each item and its predecessor", NULL,  /* HAND-AUTHORED; verb itself DEFERRED (adverb_hof NULL) */
      "(f':)x   f':[x]", NULL },  /* no example: each-prior is not yet wired */
    { "/:",    QLEX_ADVERB,    QR_NONE,  QR_NONE,  "map-right", 1, 0, "none",
      "Apply a binary value between one argument and each item of the other.", "qdocs/docs/docs/docs/ref/maps.md#Each Left and Each Right",
      "Each Left     x v2\\: y    v2\\:[x;y]   |->   v2[;y] each x", NULL },
    { "\\:",   QLEX_ADVERB,    QR_NONE,  QR_NONE,  "map-left",  1, 0, "none",
      "Apply a binary value between one argument and each item of the other.", "qdocs/docs/docs/docs/ref/maps.md#Each Left and Each Right",
      "Each Left     x v2\\: y    v2\\:[x;y]   |->   v2[;y] each x", NULL },
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

const q_op_t* q_ops_find(const char* s, int len) {
    for (int i = 0; i < N_Q_OPS; i++) {
        const char* nm = Q_OPS[i].name;
        if ((int)strlen(nm) == len && memcmp(nm, s, (size_t)len) == 0)
            return &Q_OPS[i];
    }
    return NULL;
}

int q_ops_is_reserved(const char* s, int len) { return q_ops_find(s, len) != NULL; }

/* Operator->integer opcode (`value +` -> 1) is DEFERRED: the opcode is a property
 * of the FUNCTION IDENTITY, not the spelling (`get`/`value` are two names for ONE
 * function, kdb `.:`), so a name-keyed table can't model it.  See PLAN.md; the doc
 * rows are pinned red-below-floor in test/q/function/value.qcmd. */
