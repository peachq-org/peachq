/* q op registry — see q_registry.h for the contract.
 *
 * The roster is now DRIVEN BY THE SINGLE-SOURCE MANIFEST (src/qlang/q_ops.c
 * Q_OPS[]).  q_registry_init iterates the manifest and, for every (row,
 * valence) whose build-kind is not QK_NONE, builds the owned rayfall function
 * VALUE and records its q-surface provenance.  The lexer derives its keyword
 * classification from the SAME manifest (q_lex_is_kw_infix), so the parser's
 * verb set and eval's verb set can never drift.
 *
 * Ground truth for the roster + the deferred glyphs lives in q_ops.c.  The
 * non-obvious mechanics the wrappers depend on:
 *   - rayfall `%` is MODULO and `/` is FLOAT-DIVIDE, so q `%` renames `/`.
 *   - q `#`/`_` cannot reuse rayfall `take`/`remove` verbatim (opposite arg
 *     order / dict-key semantics), so they are wrappers with the arg-swap /
 *     range-drop baked in.  q monadic `-` is negate (aliased to `neg`).
 *
 * LOWERING METADATA: each wrapper's rayfall aux-name is set to the CANONICAL
 * rayfall verb it lowers as (== != take drop) and it carries RAY_FN_Q_LOWER
 * (RAY_FN_Q_LOWER, lang/eval.h), so the compiler + query DAG dispatch on that name
 * (q `=`/`<>` hit ray_eq/ray_ne) instead of declining every non-canonical
 * value head to the eval fallback.  Pass-through/rename values ARE the env
 * builtin object, so they name-route via the existing canonical-identity path.
 *
 * REGISTRY-INIT PRECONDITION (codex #1): value embedding requires an
 * initialised registry, and a registry builder MUST NEVER call q_parse during
 * bootstrap (it would recurse into a not-yet-ready registry).  Builders here
 * only touch ray_env_get / ray_fn_* — never the parser.  A debug re-entry
 * guard (g_building) marks the bootstrap window and is the seam 2b's
 * parser-flip enforcement extends. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/q_registry.h"
#include "qlang/q_ops.h"      /* Q_OPS manifest, q_build_kind */
#include "qlang/q_ns.h"       /* q_ns_* — context views (get/key/set arms) */
#include "qlang/q_apply.h"    /* q_apply_noun — @/. noun arms + carrier apply */
#include "qlang/q_deriv.h"    /* q_deriv_kind_of/base — glyph-verb carriers */
#include "lang/env.h"         /* ray_env_get, ray_fn_binary */
#include "lang/eval.h"        /* RAY_FN_* attrs, RAY_FN_Q_LOWER */
#include "qlang/q_parse.h"    /* q_parse/q_lower — value-of-string (RUNTIME wrapper only; builders must never parse, rule 6) */
#include "lang/internal.h"    /* ray_where_fn, ray_group_fn (funsql executor) */
#include "ops/ops.h"          /* ray_is_lazy, ray_lazy_materialize (control forms) */
#include "ops/glob.h"         /* ray_glob_match — like/ss/ssr pattern matching */
#include "table/sym.h"        /* ray_sym_vec_cell (funsql executor) */
#include "qlang/q_wire.h"     /* -8!/-9! internal-fn dispatch on dyadic ! */
#include "store/serde.h"      /* ray_serde_set_fn_hooks — wrapper round-trip */
#include "lang/format.h"      /* ray_type_name — wrapper error messages */
#include "mem/heap.h"         /* RAY_ATTR_HAS_NULLS — ? find miss remap */
#include "core/numparse.h"    /* ray_parse_i64/f64 — Tok string parses */
#include "store/fileio.h"     /* ray_mkdir_p — 0: Save Text missing dirs */
#include "core/ipc.h"         /* ray_ipc_fd_of_handle/handle_of_fd — q true-fd handles */
#include "qlang/q_builtins.h" /* q_string_fn — 0: Prepare Text cell text */
#include "qlang/q_fmt.h"      /* q_console_show_krepr — 0N! debug print */
#include <assert.h>
#include <stdint.h>   /* INT*_MAX / INT64_MIN — Tok out-of-domain bounds */
#include <math.h>     /* floor/floorf — q monadic `_` */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    int64_t     sym_id;
    q_valence_t valence;
    ray_t*      value;       /* owned (rc>=1) */
    /* provenance */
    const char* spelling;    /* q surface name (static, from the manifest) */
    const char* lower_name;  /* canonical rayfall routing name             */
    int         is_wrapper;
} entry_t;

/* Upper bound: every manifest row can contribute at most two entries. */
static entry_t g_entries[2 * 256];  /* 2 slots per manifest row; grown 96->128 (list-verb 2026-07-06)->256 (2026-07-07: set-ops+sort+control-flow+atomic-math pushed the row count past 128) */
static int     g_count    = 0;
static bool    g_inited   = false;
static bool    g_building = false;   /* debug re-entry guard (see header note) */

/* ---- shared q-name sanitization (.Q.id + construction clash repair) ------ */

static int q_name_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int q_name_is_keyword_reserved(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return 0;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    int nop = 0;
    const q_op_t* ops = q_ops_table(&nop);
    int hit = 0;
    for (int i = 0; i < nop && !hit; i++) {
        if (ops[i].lex == QLEX_GLYPH || ops[i].lex == QLEX_ADVERB) continue;
        size_t m = strlen(ops[i].name);
        hit = (m == n && memcmp(ops[i].name, p, n) == 0);
    }
    ray_release(s);
    return hit;
}

static int q_name_prev_contains(const int64_t* previous, int64_t n_previous,
                                int64_t sym_id) {
    for (int64_t i = 0; i < n_previous; i++)
        if (previous[i] == sym_id) return 1;
    return 0;
}

static int64_t q_name_append_suffix(int64_t sym_id, int64_t suffix) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return sym_id;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    char stack[128];
    char* buf = stack;
    int need = snprintf(NULL, 0, "%.*s%lld", (int)n, p, (long long)suffix);
    if (need < 0) { ray_release(s); return sym_id; }
    if ((size_t)need + 1 > sizeof stack) {
        buf = (char*)malloc((size_t)need + 1);
        if (!buf) { ray_release(s); return sym_id; }
    }
    snprintf(buf, (size_t)need + 1, "%.*s%lld", (int)n, p, (long long)suffix);
    int64_t out = ray_sym_intern_runtime(buf, (size_t)need);
    if (buf != stack) free(buf);
    ray_release(s);
    return out;
}

int64_t q_name_sanitize(int64_t sym_id) {
    ray_t* s = ray_sym_str(sym_id);
    if (!s) return ray_sym_intern_runtime("a", 1);
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    char stack[128];
    char* buf = (n + 2 <= sizeof stack) ? stack : (char*)malloc(n + 2);
    if (!buf) { ray_release(s); return ray_sym_intern_runtime("a", 1); }
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (q_name_char_ok(p[i])) buf[w++] = p[i];
    if (w == 0) buf[w++] = 'a';
    if (buf[0] == '_' || (buf[0] >= '0' && buf[0] <= '9')) {
        memmove(buf + 1, buf, w);
        buf[0] = 'a';
        w++;
    }
    int64_t out = ray_sym_intern_runtime(buf, w);
    if (buf != stack) free(buf);
    ray_release(s);
    return out;
}

int64_t q_name_dedup(int64_t sym_id, const int64_t* previous, int64_t n_previous,
                     int check_reserved) {
    int64_t base = sym_id;
    if (check_reserved && q_name_is_keyword_reserved(base))
        base = q_name_append_suffix(base, 1);
    if (!q_name_prev_contains(previous, n_previous, base)) return base;
    for (int64_t i = 1; i < INT64_MAX; i++) {
        int64_t cand = q_name_append_suffix(base, i);
        if (!q_name_prev_contains(previous, n_previous, cand)) return cand;
    }
    return base;
}

ray_t* q_name_reserved_words(void) {
    int nop = 0;
    const q_op_t* ops = q_ops_table(&nop);
    int n = 0;
    for (int i = 0; i < nop; i++)
        if (ops[i].lex != QLEX_GLYPH && ops[i].lex != QLEX_ADVERB) n++;
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int i = 0; i < nop; i++) {
        if (ops[i].lex == QLEX_GLYPH || ops[i].lex == QLEX_ADVERB) continue;
        int64_t id = ray_sym_intern_runtime(ops[i].name, strlen(ops[i].name));
        out = ray_vec_append(out, &id);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    }
    return out;
}

/* ---- wrappers (bespoke q semantics over a rayfall primitive) ---- */

/* Gather `count` elements from x starting at logical index `start`.  recycle:
 * indices wrap modulo `total` (reshape recycling); else sequential in-range
 * (chunk / cut).  A string gathers chars into a new string; a vector/list
 * gathers via ray_at_fn over an i64 index vector.  Borrows x. */
static ray_t* q_gather(ray_t* x, int64_t start, int64_t count, int64_t total,
                       int recycle) {
    if (count < 0) count = 0;
    if (x && x->type == -RAY_STR) {
        const char* sp = ray_str_ptr(x);
        char stackb[1024];
        char* b = (count < (int64_t)sizeof stackb) ? stackb
                                                    : malloc((size_t)count + 1);
        if (!b) return ray_error("wsfull", "#: out of memory");
        for (int64_t i = 0; i < count; i++)
            b[i] = sp[recycle && total ? ((start + i) % total) : (start + i)];
        ray_t* row = ray_str(b, (size_t)count);
        if (b != stackb) free(b);
        return row;
    }
    int64_t stacki[1024];
    int64_t* ix = (count <= 1024) ? stacki
                                  : malloc((size_t)(count ? count : 1) * sizeof(int64_t));
    if (!ix) return ray_error("wsfull", "#: out of memory");
    for (int64_t i = 0; i < count; i++)
        ix[i] = recycle && total ? ((start + i) % total) : (start + i);
    ray_t* idx = ray_vec_from_raw(RAY_I64, ix, count);
    if (ix != stacki) free(ix);
    if (RAY_IS_ERR(idx)) return idx;
    ray_t* row = ray_at_fn(x, idx);       /* boxed list of atoms */
    ray_release(idx);
    if (!row || RAY_IS_ERR(row)) return row;
    ray_t* c = q_collapse_list(row);      /* -> typed vector when homogeneous */
    ray_release(row);
    return c;
}

/* q `shape # x` — reshape (kdb ref/take): an int-VECTOR left arg reshapes x
 * row-major into a matrix (2-D case).  A 0N dimension is INFERRED (chunk into
 * that stride, ragged last row); otherwise x is RECYCLED to fill.  Ranks other
 * than 2 fall back to rayfall range-take (unchanged behaviour).  Borrows both. */
static ray_t* q_reshape(ray_t* shape, ray_t* x) {
    int64_t nd = ray_len(shape);
    if (nd != 2) return ray_take_fn(x, shape);
    const int64_t* dv = (const int64_t*)ray_data(shape);
    int64_t d0 = dv[0], d1 = dv[1];
    int is_str = (x && x->type == -RAY_STR);
    int64_t total = is_str ? (int64_t)ray_str_len(x) : (x ? ray_len(x) : 0);
    if (total <= 0) return ray_error("length", "#: reshape of empty");
    int64_t rows, cols, chunk = 0;
    if (d0 == INT64_MIN && d1 != INT64_MIN) {
        if (d1 <= 0) return ray_error("length", "#: inferred reshape needs a positive stride");
        cols = d1; rows = (total + cols - 1) / cols; chunk = 1;
    } else if (d1 == INT64_MIN && d0 != INT64_MIN) {
        if (d0 <= 0) return ray_error("length", "#: inferred reshape needs a positive stride");
        rows = d0; cols = (total + rows - 1) / rows; chunk = 1;
    } else if (d0 == INT64_MIN || d1 == INT64_MIN) {
        return ray_error("length", "#: 0N 0N reshape");
    } else {
        rows = d0; cols = d1;
    }
    if (rows < 0 || cols < 0) return ray_error("length", "#: negative shape");
    ray_t* out = ray_list_new(rows > 0 ? rows : 1);
    if (RAY_IS_ERR(out)) return out;
    for (int64_t r = 0; r < rows; r++) {
        int64_t start = r * cols, rc = cols;
        if (chunk && start + rc > total) rc = total - start;   /* ragged last */
        ray_t* row = q_gather(x, start, rc, total, !chunk);
        if (!row || RAY_IS_ERR(row)) { ray_release(out); return row ? row : ray_error("domain", "#: reshape"); }
        out = ray_list_append(out, row);
        ray_release(row);
        if (RAY_IS_ERR(out)) return out;
    }
    return out;
}

/* q `n # list` — take.  An int-VECTOR left arg (len>=2) is RESHAPE (matrix);
 * an atom is take.  rayfall ray_take_fn(vec, n) has the opposite arg order, so
 * swap.  Borrows both args (does not release them). */
static ray_t* q_take_wrap(ray_t* n, ray_t* list) {
    if (n && n->type == RAY_I64 && ray_len(n) >= 2) return q_reshape(n, list);
    return ray_take_fn(list, n);
}

/* q `n _ list` — count-drop (NOT rayfall's dict key-remove).  n>=0 drops the
 * first n elements; n<0 drops the last |n|.  Implemented as a range-take
 * ray_take_fn(list, (start; amount)), which clamps at the ends.  Borrows args.
 *
 * Length is derived string-aware: a q string is a -RAY_STR atom whose char
 * count lives in ray_str_len, NOT the ->len union field (which aliases the SSO
 * {slen,sdata} bytes), so ray_len would be garbage for strings. */
static int q_values_match(ray_t* a, ray_t* b);      /* fwd (amend engine, below) */
static ray_t* q_flip_wrap(ray_t* x);                 /* fwd (table verbs, below) */
ray_t* ray_concat_fn(ray_t* x, ray_t* y);            /* base concat (join arms) */

/* q_collapse_list leaves a ZERO-length boxed list untyped (no element to infer
 * from); key-indexing selections must instead inherit the PROTO vector's type
 * so an empty result keeps its domain (codex r3: `` type key `a _ `a!1 `` must
 * be 11h / `` `symbol$() ``, not 0h / `()`).  Consumes `collapsed`, borrows
 * `proto`; passes errors and non-empty results through untouched. */
static ray_t* q_typed_empty_like(ray_t* collapsed, ray_t* proto) {
    if (!collapsed || RAY_IS_ERR(collapsed)) return collapsed;
    if (collapsed->type != RAY_LIST || ray_len(collapsed) != 0) return collapsed;
    if (!proto || !ray_is_vec(proto) || proto->type == RAY_LIST) return collapsed;
    ray_t* tv = (proto->type == RAY_SYM) ? ray_sym_vec_new(RAY_SYM_W64, 0)
                                         : ray_vec_new(proto->type, 0);
    if (!tv || RAY_IS_ERR(tv)) { if (tv) ray_release(tv); return collapsed; }
    ray_release(collapsed);
    return tv;
}

/* Gather table rows [start, start+count) (recycle=1 wraps cyclically) into an
 * OWNED table via ray_at_fn(t, idx) — the same primitive `t[0 2]` reaches
 * through q_apply_noun.  The result is never collapsed: tables stay tables. */
static ray_t* q_row_gather(ray_t* t, int64_t start, int64_t count, int recycle) {
    int64_t n = ray_table_nrows(t);
    ray_t* idx = ray_vec_new(RAY_I64, count > 0 ? count : 1);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("oom", NULL);
    idx->len = count;
    int64_t* p = (int64_t*)ray_data(idx);
    for (int64_t i = 0; i < count; i++) {
        int64_t j = start + i;
        if (recycle && n > 0) j = ((j % n) + n) % n;
        p[i] = j;
    }
    ray_t* r = ray_at_fn(t, idx);                    /* owned */
    ray_release(idx);
    return r;
}

/* d without the entries for keys ks (atom or vector) — kdb Drop is tolerant:
 * keys not present are ignored (ref/drop.md `` `a _ `a`b`c!1 2 3 ``).
 * Borrows both; returns an owned dict.  Same append/release discipline as
 * q_dict_union (q_apply.c). */
static ray_t* q_dict_drop_keys(ray_t* d, ray_t* ks) {
    ray_t* dk = ray_dict_keys(d);                    /* borrowed */
    ray_t* dv = ray_dict_vals(d);                    /* borrowed */
    if (!dk || !dv) return ray_error("type", "_ (drop): malformed dictionary");
    int64_t nd = ray_dict_len(d);
    int64_t nk = (ks && (ray_is_vec(ks) || ks->type == RAY_LIST)) ? ray_len(ks) : -1;
    ray_t* ok = ray_list_new(nd > 0 ? nd : 1);
    ray_t* ov = ray_list_new(nd > 0 ? nd : 1);
    if (!ok || !ov) {
        if (ok) ray_release(ok);
        if (ov) ray_release(ov);
        return ray_error("oom", NULL);
    }
    for (int64_t i = 0; i < nd; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* ke = ray_at_fn(dk, ia);               /* owned key */
        ray_release(ia);
        if (!ke || RAY_IS_ERR(ke)) { ray_release(ok); ray_release(ov); return ke ? ke : ray_error("type", NULL); }
        int hit = 0;
        if (nk < 0) {
            hit = q_values_match(ke, ks);
        } else {
            for (int64_t j = 0; j < nk && !hit; j++) {
                ray_t* ja = ray_i64(j);
                ray_t* kj = ray_at_fn(ks, ja);       /* owned */
                ray_release(ja);
                if (kj && !RAY_IS_ERR(kj)) {
                    hit = q_values_match(ke, kj);
                    ray_release(kj);
                } else if (kj) {
                    ray_release(ke); ray_release(ok); ray_release(ov);
                    return kj;
                }
            }
        }
        if (hit) { ray_release(ke); continue; }
        ray_t* ja2 = ray_i64(i);
        ray_t* ve = ray_at_fn(dv, ja2);              /* owned value */
        ray_release(ja2);
        if (!ve || RAY_IS_ERR(ve)) { ray_release(ke); ray_release(ok); ray_release(ov); return ve ? ve : ray_error("type", NULL); }
        ok = ray_list_append(ok, ke);
        ov = ray_list_append(ov, ve);
        ray_release(ke);
        ray_release(ve);
    }
    ray_t* ck = q_typed_empty_like(q_collapse_list(ok), dk);
    ray_t* cv = q_typed_empty_like(q_collapse_list(ov), dv);
    ray_release(ok);
    ray_release(ov);
    if (!ck || RAY_IS_ERR(ck)) { if (cv && !RAY_IS_ERR(cv)) ray_release(cv); return ck ? ck : ray_error("type", NULL); }
    if (!cv || RAY_IS_ERR(cv)) { ray_release(ck); return cv ? cv : ray_error("type", NULL); }
    return ray_dict_new(ck, cv);                     /* consumes both */
}

static ray_t* q_drop_wrap(ray_t* n, ray_t* list) {
    /* ---- dict arms (ref/drop.md) — claimed BEFORE the int-vector cut arm so
     * `1 2 _ intkeyed_dict` is a key-drop, and before the int-atom count tail
     * so `1_d` drops ENTRIES (keys and values together), never values-only. */
    if (list && list->type == RAY_DICT && !q_is_keyed_table(list)) {
        /* n _ d — int ATOM drops the first/last n entries */
        if (n && n->type == -RAY_I64 && !RAY_ATOM_IS_NULL(n)) {
            ray_t* k = ray_dict_keys(list);          /* borrowed */
            ray_t* v = ray_dict_vals(list);          /* borrowed */
            if (!k || !v) return ray_error("type", "_ (drop): malformed dictionary");
            ray_t* rk = q_drop_wrap(n, k);
            if (rk && ray_is_lazy(rk)) rk = ray_lazy_materialize(rk);
            if (!rk || RAY_IS_ERR(rk)) return rk;
            ray_t* rv = q_drop_wrap(n, v);
            if (rv && ray_is_lazy(rv)) rv = ray_lazy_materialize(rv);
            if (!rv || RAY_IS_ERR(rv)) { ray_release(rk); return rv; }
            return ray_dict_new(rk, rv);             /* consumes both */
        }
        /* keys _ d — sym atom / sym vector / int vector lhs drops entries by
         * key (other lhs kinds keep today's error tail) */
        if (n && (n->type == -RAY_SYM || n->type == RAY_SYM ||
                  n->type == RAY_I64 || n->type == RAY_I32 || n->type == RAY_I16))
            return q_dict_drop_keys(list, n);
    }
    /* d _ key — dict lhs drops the entry at an ATOM key; a vector rhs is
     * 'type (ref/drop.md pins `(`a`b`c!1 2 3) _ `a`b` -> 'type). */
    if (n && n->type == RAY_DICT && !q_is_keyed_table(n)) {
        if (list && ray_is_atom(list))
            return q_dict_drop_keys(n, list);
        return ray_error("type", "_ (drop): dict drop takes an atom key");
    }
    /* syms _ t — table column-drop (ref/drop.md `` `a`b _ t ``): sym-VECTOR
     * lhs only (a sym ATOM lhs on a table stays 'type — pinned rows).  One
     * home: flip -> dict key-drop -> flip back (q_flip_wrap owns its results;
     * q_dict_drop_keys borrows both args). */
    if (n && n->type == RAY_SYM && list && list->type == RAY_TABLE) {
        ray_t* d = q_flip_wrap(list);                /* owned dict */
        if (!d || RAY_IS_ERR(d)) return d;
        ray_t* rd = q_dict_drop_keys(d, n);          /* owned dict */
        ray_release(d);
        if (!rd || RAY_IS_ERR(rd)) return rd;
        ray_t* rt = q_flip_wrap(rd);                 /* owned table */
        ray_release(rd);
        return rt;
    }
    /* x _ i — delete the item at index i (ref/drop.md `0 1 ... 8 _ 5`):
     * list/vector lhs, int-ATOM rhs.  Two clamped range-takes joined; an
     * out-of-range index returns x unchanged (Drop is tolerant). */
    if (n && (ray_is_vec(n) || n->type == RAY_LIST) && n->type != RAY_DICT &&
        list && (list->type == -RAY_I64 || list->type == -RAY_I32 ||
                 list->type == -RAY_I16)) {
        int64_t len = ray_len(n);
        int64_t i = (list->type == -RAY_I64) ? list->i64
                  : (list->type == -RAY_I32) ? (int64_t)list->i32
                  : (int64_t)list->i16;
        if (i < 0 || i >= len) { ray_retain(n); return n; }
        int64_t r1[2] = { 0, i }, r2[2] = { i + 1, len - i - 1 };
        ray_t* rng1 = ray_vec_from_raw(RAY_I64, r1, 2);
        if (RAY_IS_ERR(rng1)) return rng1;
        ray_t* head = ray_take_fn(n, rng1);          /* owned */
        ray_release(rng1);
        if (!head || RAY_IS_ERR(head)) return head;
        ray_t* rng2 = ray_vec_from_raw(RAY_I64, r2, 2);
        if (RAY_IS_ERR(rng2)) { ray_release(head); return rng2; }
        ray_t* tail = ray_take_fn(n, rng2);          /* owned */
        ray_release(rng2);
        if (!tail || RAY_IS_ERR(tail)) { ray_release(head); return tail; }
        ray_t* r = ray_concat_fn(head, tail);        /* owned */
        ray_release(head);
        ray_release(tail);
        return r;
    }
    /* q cut: int-VECTOR lhs — `2 4_v` slices [p0,p1) then [p_last,end).
     * Positions non-decreasing within 0..len; result is a boxed list of
     * slices (kdb 0h). */
    if (n && (n->type == RAY_I64 || n->type == RAY_I32 || n->type == RAY_I16)) {
        if (!list || (!ray_is_vec(list) && list->type != RAY_LIST &&
                      list->type != RAY_TABLE))
            return ray_error("type", "_ (cut): expects a list rhs");
        int64_t len = (list->type == RAY_TABLE) ? ray_table_nrows(list)
                                                : ray_len(list);
        int64_t np = ray_len(n);
        ray_t* out = ray_list_new(np > 0 ? np : 1);
        int64_t prev = 0;
        for (int64_t i = 0; i < np; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* pe = ray_at_fn(n, ia);
            ray_release(ia);
            if (!pe || RAY_IS_ERR(pe)) { ray_release(out); return pe; }
            int64_t p = pe->i64;
            ray_release(pe);
            int64_t nxt = len;
            if (i + 1 < np) {
                ray_t* ja = ray_i64(i + 1);
                ray_t* ne = ray_at_fn(n, ja);
                ray_release(ja);
                if (!ne || RAY_IS_ERR(ne)) { ray_release(out); return ne; }
                nxt = ne->i64;
                ray_release(ne);
            }
            if (p < 0 || p > len || nxt < p || nxt > len || (i > 0 && p < prev)) {
                ray_release(out);
                return ray_error("domain",
                    "_ (cut): positions must be non-decreasing within 0..%lld",
                    (long long)len);
            }
            prev = p;
            ray_t* slice;
            if (list->type == RAY_TABLE) {           /* table cut: row slices */
                slice = q_row_gather(list, p, nxt - p, 0);
            } else {
                int64_t rng[2] = { p, nxt - p };
                ray_t* range = ray_vec_from_raw(RAY_I64, rng, 2);
                if (RAY_IS_ERR(range)) { ray_release(out); return range; }
                slice = ray_take_fn(list, range);
                ray_release(range);
            }
            if (!slice || RAY_IS_ERR(slice)) { ray_release(out); return slice; }
            out = ray_list_append(out, slice);
            ray_release(slice);
        }
        return out;
    }
    if (!n || n->type != -RAY_I64)
        return ray_error("type", "_ (drop): count must be an integer");
    if (!list) return ray_error("type", "_ (drop): nil list");
    int64_t len;
    if (list->type == -RAY_STR)
        len = (int64_t)ray_str_len(list);           /* SSO-safe string length */
    else if (ray_is_vec(list) || list->type == RAY_LIST)
        len = ray_len(list);                         /* typed vector / boxed list */
    else
        return ray_error("type", "_ (drop): expects a list, vector, or string");
    int64_t k = n->i64;
    int64_t start, amount;
    if (k >= 0) { start = (k < len) ? k : len; amount = len - start; }
    else        { start = 0; amount = len + k; if (amount < 0) amount = 0; }
    int64_t rng[2] = { start, amount };
    ray_t* range = ray_vec_from_raw(RAY_I64, rng, 2);
    if (RAY_IS_ERR(range)) return range;
    ray_t* r = ray_take_fn(list, range);
    ray_release(range);
    return r;
}

/* q `n cut x` — cut into pieces (kdb ref/cut).  An int ATOM chunks x into
 * groups of n (ragged last group: `4 cut til 10` -> (0 1 2 3;4 5 6 7;8 9)).
 * An int VECTOR is a positional cut — identical to `_`, so delegate.  Borrows. */
static ray_t* q_cut_wrap(ray_t* n, ray_t* x) {
    if (n && (n->type == -RAY_I64 || n->type == -RAY_I32 || n->type == -RAY_I16)) {
        int64_t sz = (n->type == -RAY_I64) ? n->i64
                   : (n->type == -RAY_I32) ? (int64_t)n->i32 : (int64_t)n->i16;
        if (sz <= 0) return ray_error("domain", "cut: piece size must be positive");
        int is_str = (x && x->type == -RAY_STR);
        int64_t total = is_str ? (int64_t)ray_str_len(x) : (x ? ray_len(x) : 0);
        if (!x || (!is_str && !ray_is_vec(x) && x->type != RAY_LIST))
            return ray_error("type", "cut: expects a list/vector/string rhs");
        int64_t rows = (total + sz - 1) / sz;
        ray_t* out = ray_list_new(rows > 0 ? rows : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t r = 0; r < rows; r++) {
            int64_t start = r * sz, rc = sz;
            if (start + rc > total) rc = total - start;
            ray_t* row = q_gather(x, start, rc, total, 0);   /* chunk, no recycle */
            if (!row || RAY_IS_ERR(row)) { ray_release(out); return row ? row : ray_error("domain", "cut"); }
            out = ray_list_append(out, row);
            ray_release(row);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return q_drop_wrap(n, x);   /* int-vector positional cut == `_` */
}

/* q monadic `_` — floor to LONG (kdb `_ 3.7` is 3j; rayfall floor keeps f64).
 * Ints/bools pass through; f64 null -> long null.  RAY_FN_ATOMIC maps it
 * element-wise over float vectors. */
static ray_t* q_floor_wrap(ray_t* x) {
    if (!x) return ray_error("type", "_ (floor): nil");
    if (x->type == -RAY_F64) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)floor(x->f64));
    }
    if (x->type == -RAY_F32) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)floorf((float)x->f64));
    }
    if (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16 ||
        x->type == -RAY_BOOL) {
        ray_retain(x);
        return x;
    }
    return ray_error("type", "_ (floor): expects a numeric, got %s",
                     ray_type_name(x->type));
}

/* ---- atomic unary math (feat/q-math-atomic) — implement-via-libm ----
 * rayfall has exp/log/sqrt but no trig/reciprocal/signum, so these are q-layer
 * wrappers, one libm call per atom.  All are registered RAY_FN_ATOMIC, so the
 * evaluator (atomic_map_unary) broadcasts them over vectors and nested lists;
 * each wrapper handles the ATOM case only (mirroring ray_sqrt_fn/q_floor_wrap).
 * Float results go through make_f64 (internal.h), which canonicalizes every
 * non-finite (NaN OR ±Inf) to the single float null 0n — so `sin 1%0` -> 0n
 * and reciprocal of 0 -> 0n (kdb's 0w is unrepresentable under this model, a
 * deferred cell).  Null in -> typed float null out (kdb: sin/cos/asin/... of a
 * null is null). */
#define Q_LIBM_UNARY(NAME, FN, GLYPH)                                          \
    static ray_t* NAME(ray_t* x) {                                             \
        if (!x) return ray_error("type", GLYPH ": nil");                       \
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);              \
        if (is_numeric(x)) return make_f64(FN(as_f64(x)));                     \
        return ray_error("type", GLYPH ": expects a numeric argument, got %s", \
                         ray_type_name(x->type));                              \
    }
Q_LIBM_UNARY(q_sin_wrap,  sin,  "sin")
Q_LIBM_UNARY(q_cos_wrap,  cos,  "cos")
Q_LIBM_UNARY(q_tan_wrap,  tan,  "tan")
Q_LIBM_UNARY(q_asin_wrap, asin, "asin")
Q_LIBM_UNARY(q_acos_wrap, acos, "acos")
Q_LIBM_UNARY(q_atan_wrap, atan, "atan")
#undef Q_LIBM_UNARY

/* q `reciprocal x` — 1%x as a float (ref/reciprocal.md).  Null -> null;
 * reciprocal 0 -> +Inf -> 0n under the single-null model (kdb shows 0w). */
static ray_t* q_reciprocal_wrap(ray_t* x) {
    if (!x) return ray_error("type", "reciprocal: nil");
    if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_F64);
    if (is_numeric(x)) return make_f64(1.0 / as_f64(x));
    return ray_error("type", "reciprocal: expects a numeric argument, got %s",
                     ray_type_name(x->type));
}

/* q `signum x` — sign as INT (i32): null or negative -> -1i, zero -> 0i,
 * positive -> 1i (ref/signum.md).  Kdb ALWAYS returns int, whatever the input
 * width.  A float null (0n) tests as null -> -1i (kdb treats null as negative).
 * TEMPORAL atoms sign their underlying payload (ref/signum.md pins
 * `signum 1999.12.31` -> -1i — a pre-epoch date is negative), and every typed
 * null is null (-1i). */
static ray_t* q_signum_atom(ray_t* x) {
    if (!x) return ray_error("type", "signum: nil");
    if (RAY_ATOM_IS_NULL(x)) return ray_i32(-1);
    if (x->type < 0 && RAY_IS_TEMPORAL32(-x->type)) {
        int32_t v = x->i32;
        return ray_i32(v < 0 ? -1 : (v > 0 ? 1 : 0));
    }
    if (x->type < 0 && RAY_IS_TEMPORAL64(-x->type)) {
        int64_t v = x->i64;
        return ray_i32(v < 0 ? -1 : (v > 0 ? 1 : 0));
    }
    if (x->type == -RAY_DATETIME) {                    /* f64-payload temporal */
        double v = x->f64;
        return ray_i32(v < 0 ? -1 : (v > 0 ? 1 : 0));
    }
    if (is_numeric(x)) {
        /* as_f64 handles BOOL + every int/float width and preserves the sign
         * exactly (only magnitude precision is lost, irrelevant to sign), so
         * one branch covers all numeric atoms — avoids as_i64's missing BOOL
         * case (codex review). */
        double v = as_f64(x);
        return ray_i32(v < 0 ? -1 : (v > 0 ? 1 : 0));
    }
    return ray_error("type", "signum: expects a numeric argument, got %s",
                     ray_type_name(x->type));
}

/* Broadcast + collapse carrier (the q_null_wrap pattern): registered
 * RAY_FN_NONE so THIS wrapper drives the broadcast, letting a top-level boxed
 * list of i32 atoms collapse to an int vector — kdb shows `signum (0n;0N;0Nt)`
 * as ONE `-1 -1 -1i` line, not one atom per line. */
static ray_t* q_signum_wrap(ray_t* x) {
    ray_t* r = is_collection(x) ? atomic_map_unary(q_signum_atom, x)
                                : q_signum_atom(x);
    if (!r || RAY_IS_ERR(r) || r->type != RAY_LIST) return r;
    ray_t* c = q_collapse_list(r);   /* owned: retains-or-builds */
    ray_release(r);
    return c;
}

/* q `ceiling x` — least integer >= x, returned as a LONG (kdb `ceiling 2.1` is
 * 3j).  The QK_FLOOR twin: rayfall's `ceil` keeps f64, so this wrapper rounds
 * to i64 exactly like q_floor_wrap.  Ints/bools pass through; f64 null -> long
 * null. */
static ray_t* q_ceiling_wrap(ray_t* x) {
    if (!x) return ray_error("type", "ceiling: nil");
    if (x->type == -RAY_F64) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)ceil(x->f64));
    }
    if (x->type == -RAY_F32) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_I64);
        return ray_i64((int64_t)ceilf((float)x->f64));
    }
    if (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16 ||
        x->type == -RAY_BOOL) {
        ray_retain(x);
        return x;
    }
    return ray_error("type", "ceiling: expects a numeric argument, got %s",
                     ray_type_name(x->type));
}

/* ---- dyadic atomic math (feat/q-math-parse-display) ----------------------
 * q `x xexp y` — x to the power y as a FLOAT (ref/exp.md).  The doc pins the
 * COMPUTATION, not just the value: "The calculation is performed as
 * exp y * log x" (so `2 xexp 3` is 7.999…, NOT C pow's exact 8 — codex r1).
 * All the doc's edge rules fall out of the identity: x null or NEGATIVE ->
 * log NaN -> 0n; y null -> 0n; x=0,y>0 -> 0f; overflow +inf -> 0n via
 * make_f64 (single-null model; kdb shows 0w — documented divergence).
 * Domain is numeric-only: ref/exp.md's table rejects char args. */
static ray_t* q_xexp_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "xexp: nil operand");
    if (!is_numeric(x) && !RAY_ATOM_IS_NULL(x))
        return ray_error("type", "xexp: expects numeric arguments, got %s",
                         ray_type_name(x->type));
    if (!is_numeric(y) && !RAY_ATOM_IS_NULL(y))
        return ray_error("type", "xexp: expects numeric arguments, got %s",
                         ray_type_name(y->type));
    if (RAY_ATOM_IS_NULL(x) || RAY_ATOM_IS_NULL(y))
        return ray_typed_null(-RAY_F64);
    return make_f64(exp(as_f64(y) * log(as_f64(x))));
}

/* q `x xlog y` — base-x logarithm of y as a FLOAT: log(yf)/log(xf) with both
 * operands cast to float first (ref/log.md "the base-xf logarithm of yf").
 * y null -> 0n; y negative -> 0n (log NaN); y zero -> -inf -> 0n (kdb -0w —
 * the documented single-null divergence).  CHAR operands read as their code
 * points (ref/log.md pins `"A" xlog "C"` == `65 xlog 67` -> 1.00726); xexp
 * does NOT share the char arm (its domain table rejects chars). */
static int q_xlog_operand(ray_t* v, double* out) {
    if (!v) return 0;
    if (v->type == -RAY_STR && ray_str_len(v) == 1) {   /* char atom */
        *out = (double)(unsigned char)ray_str_ptr(v)[0];
        return 1;
    }
    /* Temporal operands cast to float via their payload (ref/log.md domain
     * table: p m d n u v t all map to f; z and s are excluded — codex r2).
     * Temporal NULLS pass through here too; the wrap's null gate turns them
     * into 0n before the payload is used. */
    if (v->type < 0 && RAY_IS_TEMPORAL32(-v->type)) { *out = (double)v->i32; return 1; }
    if (v->type < 0 && RAY_IS_TEMPORAL64(-v->type)) { *out = (double)v->i64; return 1; }
    if (is_numeric(v) || RAY_ATOM_IS_NULL(v)) { *out = as_f64(v); return 1; }
    return 0;
}
static ray_t* q_xlog_wrap(ray_t* x, ray_t* y) {
    double xf, yf;
    if (!q_xlog_operand(x, &xf) || !q_xlog_operand(y, &yf))
        return ray_error("type", "xlog: expects numeric or char arguments");
    if ((x->type != -RAY_STR && RAY_ATOM_IS_NULL(x)) ||
        (y->type != -RAY_STR && RAY_ATOM_IS_NULL(y)))
        return ray_typed_null(-RAY_F64);
    return make_f64(log(yf) / log(xf));
}

/* q char-string comparison — q treats a string as a char vector, so `=`/`<>`
 * compare element-wise and yield a boolean vector (`"abc"="abd"` -> 110b).
 * rayfall's `==`/`!=` (ray_eq_fn/ray_neq_fn) compare two -RAY_STR atoms as
 * whole values (a single 0b/1b), so the q verbs wrap them.  Two -RAY_STR
 * operands take the element-wise path here (equal length -> boolean vector,
 * unequal -> a q `length` error); everything else delegates to rayfall. */
static int q_is_str_atom(ray_t* x) { return x && x->type == -RAY_STR; }

static ray_t* q_str_cmp_vec(ray_t* a, ray_t* b, int eq) {
    const char* pa = ray_str_ptr(a); size_t la = ray_str_len(a);
    const char* pb = ray_str_ptr(b); size_t lb = ray_str_len(b);
    if (la != lb)
        return ray_error("length", "%s: string lengths must match, got %zu and %zu",
                         eq ? "=" : "<>", la, lb);
    uint8_t stack[128];
    uint8_t* bits = (la <= sizeof stack) ? stack : (uint8_t*)malloc(la ? la : 1);
    if (!bits) return ray_error("wsfull", "=: out of memory");
    for (size_t i = 0; i < la; i++)
        bits[i] = (uint8_t)(eq ? (pa[i] == pb[i]) : (pa[i] != pb[i]));
    ray_t* r = ray_vec_from_raw(RAY_BOOL, bits, (int64_t)la);
    if (bits != stack) free(bits);
    return r;
}

/* q `=` — element-wise over char strings, else rayfall `==`.  RAY_FN_ATOMIC so
 * eval broadcasts it over numeric vectors (each element pair hits ray_eq_fn). */
static ray_t* q_eq_wrap(ray_t* a, ray_t* b) {
    if (q_is_str_atom(a) && q_is_str_atom(b)) return q_str_cmp_vec(a, b, 1);
    return ray_eq_fn(a, b);
}

/* q `<>` — element-wise over char strings, else rayfall `!=`. */
static ray_t* q_ne_wrap(ray_t* a, ray_t* b) {
    if (q_is_str_atom(a) && q_is_str_atom(b)) return q_str_cmp_vec(a, b, 0);
    return ray_neq_fn(a, b);
}

/* q dyadic `&` — min / boolean-and.  The wrapper is registered ATOMIC, so
 * vector/scalar and vector/vector cases are mapped by eval over atom pairs. */
static ray_t* q_min2_wrap(ray_t* a, ray_t* b) {
    if (!a || !b || !ray_is_atom(a) || !ray_is_atom(b))
        return ray_error("type", "&: expects numeric atoms");
    if (a->type == -RAY_BOOL && b->type == -RAY_BOOL)
        return ray_bool(a->b8 && b->b8);
    if (a->type == -RAY_F64 || b->type == -RAY_F64 ||
        a->type == -RAY_F32 || b->type == -RAY_F32) {
        double av = as_f64(a);
        double bv = as_f64(b);
        return ray_f64(av <= bv ? av : bv);
    }
    if ((a->type == -RAY_I64 || a->type == -RAY_I32 || a->type == -RAY_I16 ||
         a->type == -RAY_U8  || a->type == -RAY_BOOL) &&
        (b->type == -RAY_I64 || b->type == -RAY_I32 || b->type == -RAY_I16 ||
         b->type == -RAY_U8  || b->type == -RAY_BOOL)) {
        int64_t av = (a->type == -RAY_BOOL) ? a->b8 : as_i64(a);
        int64_t bv = (b->type == -RAY_BOOL) ? b->b8 : as_i64(b);
        return ray_i64(av <= bv ? av : bv);
    }
    return ray_error("type", "&: unsupported operands");
}

/* q `x~y` — recursive whole-value equivalence (kdb match): TYPE-strict
 * (`1~1f` is 0b), attribute-blind (`1 2 3~\`s#1 2 3` is 1b), sentinel nulls
 * compare equal (`0n~0n` is 1b — non-finites canonicalize to one payload).
 * Unhandled types conservatively mismatch (kdb ~ never errors). */
static int q_match_rec(ray_t* a, ray_t* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->type != b->type) return 0;
    if (a->type == -RAY_SYM) return a->i64 == b->i64;
    if (a->type == -RAY_STR)
        return ray_str_len(a) == ray_str_len(b) &&
               memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) == 0;
    if (a->type == -RAY_GUID) {
        /* payload lives in a 16-byte U8 buffer behind the obj pointer —
         * an 8-byte union memcmp would compare POINTERS (codex P2). */
        return a->obj && b->obj &&
               memcmp(ray_data(a->obj), ray_data(b->obj), 16) == 0;
    }
    if (ray_is_atom(a)) {
        /* inline-payload scalars ONLY (ray_is_atom also covers LAMBDA and
         * fn values, whose state is NOT in the union slot — those fall to
         * the conservative-mismatch tail below). */
        switch (-a->type) {
        case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
        case RAY_F32: case RAY_F64:
        RAY_TEMPORAL32_CASES: RAY_TEMPORAL64_CASES: RAY_TEMPORALF_CASES:
            return memcmp(&a->i64, &b->i64, 8) == 0;   /* payload union */
        default:
            return 0;
        }
    }
    if (a->type == RAY_DICT || a->type == RAY_TABLE) {
        ray_t** ea = (ray_t**)ray_data(a);
        ray_t** eb = (ray_t**)ray_data(b);
        return q_match_rec(ea[0], eb[0]) && q_match_rec(ea[1], eb[1]);
    }
    if (a->type == RAY_LIST || ray_is_vec(a)) {
        int64_t la = ray_len(a);
        if (la != ray_len(b)) return 0;
        /* same-type numeric vectors: payload memcmp (nulls are in-payload
         * sentinels; attrs deliberately not compared).  SYM vecs vary in
         * index width -> per-element below. */
        if (ray_is_vec(a) && a->type != RAY_SYM && a->type != RAY_STR) {
            size_t esz = (a->type == RAY_I64 || a->type == RAY_F64 ||
                          RAY_IS_TEMPORALF(a->type)) ? 8
                       : (a->type == RAY_I32 || a->type == RAY_F32) ? 4
                       : (a->type == RAY_I16) ? 2
                       : (a->type == RAY_BOOL || a->type == RAY_U8) ? 1 : 0;
            if (esz)
                return memcmp(ray_data(a), ray_data(b), (size_t)la * esz) == 0;
        }
        for (int64_t i = 0; i < la; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* xa = ray_at_fn(a, ia);
            ray_t* xb = ray_at_fn(b, ia);
            ray_release(ia);
            int r = (xa && xb && !RAY_IS_ERR(xa) && !RAY_IS_ERR(xb))
                        ? q_match_rec(xa, xb) : 0;
            if (xa) ray_release(xa);
            if (xb) ray_release(xb);
            if (!r) return 0;
        }
        return 1;
    }
    return 0;
}

static ray_t* q_match_wrap(ray_t* a, ray_t* b) {
    return ray_bool(q_match_rec(a, b));
}

/* q `til` — kdb accepts a boolean (`til 1b` -> ,0); base ray_til_fn is
 * int-only.  Everything else (int atoms, the error paths) delegates. */
static ray_t* q_til_wrap(ray_t* x) {
    if (x && x->type == -RAY_BOOL) {
        ray_t* n = ray_i64(x->b8 ? 1 : 0);
        ray_t* r = ray_til_fn(n);
        ray_release(n);
        return r;
    }
    return ray_til_fn(x);
}

/* Borrow-or-collapse a dict's VALUES for a kernel call: a typed vector passes
 * through BORROWED (*owned=0); a boxed list collapses (q_collapse_list, owned
 * result, *owned=1) so homogeneous literal dicts hit the typed kernels.
 * Caller releases iff *owned.  NULL on a malformed dict. */
static ray_t* q_dict_vals_vec(ray_t* d, int* owned) {
    *owned = 0;
    ray_t* vals = ray_dict_vals(d);              /* borrowed accessor */
    if (!vals) return NULL;
    if (vals->type == RAY_LIST) {
        ray_t* c = q_collapse_list(vals);        /* owned */
        if (c && !RAY_IS_ERR(c)) { *owned = 1; return c; }
        if (c) ray_release(c);
        return vals;                             /* uncollapsible: borrowed */
    }
    return vals;
}

/* q `where` / monadic `&` — an INTEGER vector repeats each index i, x[i] times
 * (`where 2 3 1` -> 0 0 1 1 1 2; `where 0 1 0 1 0 1` -> 1 3 5).  Base
 * ray_where_fn handles the boolean-mask form, so delegate for it and anything
 * else.  Result is a long vector (kdb).  Negative counts are 'domain. */
static ray_t* q_where_wrap(ray_t* x) {
    /* where d — keys replicated by the (int/bool) values (ref/where.md):
     * `where `a`b`c!1 0 2` -> `a`c`c; a bool-valued dict (comparison result)
     * gives the keys where true.  Key-indexing shape keys[where vals], NOT
     * value distribution. */
    if (x && x->type == RAY_DICT && !q_is_keyed_table(x)) {
        ray_t* keys = ray_dict_keys(x);            /* borrowed */
        if (!keys) return ray_error("type", "where: malformed dictionary");
        int vo = 0;
        ray_t* vv = q_dict_vals_vec(x, &vo);
        if (!vv) return ray_error("type", "where: malformed dictionary");
        ray_t* w = q_where_wrap(vv);               /* bool mask or int counts */
        if (vo) ray_release(vv);
        if (!w || RAY_IS_ERR(w)) return w;
        ray_t* r = ray_at_fn(keys, w);
        ray_release(w);
        if (r && r->type == RAY_LIST) { ray_t* c = q_typed_empty_like(q_collapse_list(r), keys); ray_release(r); return c; }
        return r;
    }
    if (x && (x->type == RAY_I64 || x->type == RAY_I32 || x->type == RAY_I16)) {
        int64_t n = ray_len(x);
        int64_t total = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t c = (x->type == RAY_I64) ? ((const int64_t*)ray_data(x))[i]
                      : (x->type == RAY_I32) ? (int64_t)((const int32_t*)ray_data(x))[i]
                      : (int64_t)((const int16_t*)ray_data(x))[i];
            if (c < 0) return ray_error("domain", "where: negative count");
            total += c;
        }
        ray_t* out = ray_vec_new(RAY_I64, total);
        if (RAY_IS_ERR(out)) return out;
        out->len = total;
        int64_t* d = (int64_t*)ray_data(out);
        int64_t w = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t c = (x->type == RAY_I64) ? ((const int64_t*)ray_data(x))[i]
                      : (x->type == RAY_I32) ? (int64_t)((const int32_t*)ray_data(x))[i]
                      : (int64_t)((const int16_t*)ray_data(x))[i];
            for (int64_t k = 0; k < c; k++) d[w++] = i;
        }
        return out;
    }
    return ray_where_fn(x);
}

/* q `reverse x` / monadic `|` — a dict reverses ENTRIES (keys and values
 * together, ref/reverse.md: "on dictionaries, reverses the keys"); everything
 * else delegates to base reverse unchanged. */
static ray_t* q_reverse_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT && !q_is_keyed_table(x)) {
        ray_t* k = ray_dict_keys(x);                 /* borrowed */
        ray_t* v = ray_dict_vals(x);                 /* borrowed */
        if (!k || !v) return ray_error("type", "reverse: malformed dictionary");
        ray_t* rk = ray_reverse_fn(k);
        if (rk && ray_is_lazy(rk)) rk = ray_lazy_materialize(rk);   /* dict slots must be concrete */
        if (!rk || RAY_IS_ERR(rk)) return rk;
        ray_t* rv = ray_reverse_fn(v);
        if (rv && ray_is_lazy(rv)) rv = ray_lazy_materialize(rv);
        if (!rv || RAY_IS_ERR(rv)) { ray_release(rk); return rv; }
        return ray_dict_new(rk, rv);                 /* consumes both */
    }
    return ray_reverse_fn(x);
}

/* ===== q `vs` / `sv` — split-join / base-encode family ===================
 * kdb reference vs.md / sv.md.  Both are strictly dyadic.  Native -RAY_STR
 * strings (split -> boxed list of string atoms, join -> one string atom),
 * symbol split/join, integer base decompose/compose (atom + vector base),
 * and big-endian byte/bit encode/decode.  Genuinely out-of-scope forms
 * (128-bit GUID compose, `1:` reparse, byte-vector base) return 'nyi. */

static int q_is_null_sym(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return 0;
    ray_t* s = ray_sym_str(x->i64);
    int z = s && ray_str_len(s) == 0;
    return z;
}
static int q_is_int_atom(ray_t* x) {
    return x && (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16);
}
static int q_is_int_vec(ray_t* x) {
    return x && (x->type == RAY_I64 || x->type == RAY_I32 || x->type == RAY_I16);
}
static int64_t q_ivec_get(ray_t* v, int64_t i) {
    const void* d = ray_data(v);
    return v->type == RAY_I64 ? ((const int64_t*)d)[i]
         : v->type == RAY_I32 ? (int64_t)((const int32_t*)d)[i]
                              : (int64_t)((const int16_t*)d)[i];
}
static int64_t q_iatom_val(ray_t* x) {
    return x->type == -RAY_I64 ? x->i64
         : x->type == -RAY_I32 ? (int64_t)x->i32 : (int64_t)x->i16;
}

/* split string y on substring sep -> boxed list of -RAY_STR (keeps empties) */
static ray_t* q_str_split(const char* y, size_t yl, const char* sep, size_t sl) {
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    if (sl == 0) {                                 /* empty sep -> one piece */
        ray_t* s = ray_str(y, yl);
        out = ray_list_append(out, s); ray_release(s);
        return out;
    }
    size_t seg = 0;
    for (size_t i = 0; i + sl <= yl; ) {
        if (memcmp(y + i, sep, sl) == 0) {
            ray_t* s = ray_str(y + seg, i - seg);
            out = ray_list_append(out, s); ray_release(s);
            if (RAY_IS_ERR(out)) return out;
            i += sl; seg = i;
        } else i++;
    }
    ray_t* last = ray_str(y + seg, yl - seg);
    out = ray_list_append(out, last); ray_release(last);
    return out;
}

/* newline / host-line-separator split: split on '\n', strip a trailing '\r'
 * from each line, drop a single trailing empty line (kdb ` vs read-lines). */
static ray_t* q_str_split_lines(const char* y, size_t yl) {
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    size_t seg = 0;
    for (size_t i = 0; i <= yl; i++) {
        if (i == yl || y[i] == '\n') {
            size_t end = i;
            if (end > seg && y[end - 1] == '\r') end--;   /* strip CR */
            ray_t* s = ray_str(y + seg, end - seg);
            out = ray_list_append(out, s); ray_release(s);
            if (RAY_IS_ERR(out)) return out;
            seg = i + 1;
            if (i == yl) break;
        }
    }
    /* drop a single trailing empty produced by a terminal '\n' */
    int64_t n = ray_len(out);
    if (n >= 1) {
        ray_t** e = (ray_t**)ray_data(out);
        if (e[n - 1]->type == -RAY_STR && ray_str_len(e[n - 1]) == 0) {
            ray_release(e[n - 1]);
            out->len = n - 1;
        }
    }
    return out;
}

/* ` vs `sym — split a symbol: leading ':' (file handle) splits at the LAST
 * '/' into (dir; file); otherwise split on every '.'.  -> RAY_SYM vector. */
static ray_t* q_sym_split(ray_t* y) {
    ray_t* s = ray_sym_str(y->i64);
    if (!s) return ray_error("type", "vs: bad symbol");
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, 4);
    if (n > 0 && p[0] == ':') {                    /* file handle: last '/' */
        size_t cut = n;
        for (size_t i = n; i-- > 0; ) if (p[i] == '/') { cut = i; break; }
        if (cut == n) {                            /* no '/', single element */
            int64_t id = ray_sym_intern_runtime(p, n);
            out = ray_vec_append(out, &id);
        } else {
            int64_t a = ray_sym_intern_runtime(p, cut);
            int64_t b = ray_sym_intern_runtime(p + cut + 1, n - cut - 1);
            out = ray_vec_append(out, &a);
            out = ray_vec_append(out, &b);
        }
    } else {                                        /* split all '.' */
        size_t seg = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i == n || p[i] == '.') {
                int64_t id = ray_sym_intern_runtime(p + seg, i - seg);
                out = ray_vec_append(out, &id);
                seg = i + 1;
            }
        }
    }
    return out;
}

/* big-endian byte encode of a numeric scalar (0x0 vs y) -> U8 vector */
static ray_t* q_byte_encode(ray_t* y) {
    uint8_t b[8]; int w = 0; uint64_t bits = 0;
    switch (y->type) {
    case -RAY_I16: w = 2; bits = (uint16_t)y->i16; break;
    case -RAY_I32: w = 4; bits = (uint32_t)y->i32; break;
    case -RAY_I64: w = 8; bits = (uint64_t)y->i64; break;
    case -RAY_F32: { float f = (float)y->f64; uint32_t u; memcpy(&u, &f, 4);
                     w = 4; bits = u; break; }
    case -RAY_F64: { double d = y->f64; uint64_t u; memcpy(&u, &d, 8);
                     w = 8; bits = u; break; }
    default: return ray_error("type", "vs: unsupported byte-encode operand");
    }
    for (int i = 0; i < w; i++) b[i] = (uint8_t)(bits >> (8 * (w - 1 - i)));
    return ray_vec_from_raw(RAY_U8, b, w);
}

/* big-endian bit decompose of an integer scalar (0b vs y) -> BOOL vector */
static ray_t* q_bit_decompose(ray_t* y) {
    int w = 0; uint64_t bits = 0;
    switch (y->type) {
    case -RAY_BOOL: w = 1;  bits = y->b8 ? 1 : 0; break;
    case -RAY_U8:   w = 8;  bits = (uint8_t)y->u8; break;
    case -RAY_I16:  w = 16; bits = (uint16_t)y->i16; break;
    case -RAY_I32:  w = 32; bits = (uint32_t)y->i32; break;
    case -RAY_I64:  w = 64; bits = (uint64_t)y->i64; break;
    default: return ray_error("type", "vs: unsupported bit-decompose operand");
    }
    uint8_t stackb[64];
    for (int i = 0; i < w; i++) stackb[i] = (uint8_t)((bits >> (w - 1 - i)) & 1);
    return ray_vec_from_raw(RAY_BOOL, stackb, w);
}

/* decompose scalar v into minimal base-`base` digits (>=1) -> long vector */
static ray_t* q_base_decompose_atom(int64_t base, int64_t v) {
    if (base <= 0) return ray_error("domain", "vs: base must be positive");
    int64_t buf[64]; int n = 0;
    uint64_t u = (uint64_t)v;
    if (u == 0) buf[n++] = 0;
    while (u > 0 && n < 64) { buf[n++] = (int64_t)(u % (uint64_t)base); u /= (uint64_t)base; }
    ray_t* out = ray_vec_new(RAY_I64, n);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    for (int i = 0; i < n; i++) d[i] = buf[n - 1 - i];   /* MSB first */
    return out;
}

/* mixed-radix decompose scalar v by vector base -> long vector len(base) */
static ray_t* q_base_decompose_vec(ray_t* base, int64_t v) {
    int64_t n = ray_len(base);
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    uint64_t u = (uint64_t)v;
    for (int64_t i = n - 1; i >= 0; i--) {
        int64_t bi = q_ivec_get(base, i);
        if (bi <= 0) { d[i] = (int64_t)u; u = 0; }
        else { d[i] = (int64_t)(u % (uint64_t)bi); u /= (uint64_t)bi; }
    }
    return out;
}

static ray_t* q_vs_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "vs: nil operand");
    /* --- string / newline split --- */
    if (x->type == -RAY_STR) {
        if (y->type != -RAY_STR)
            return ray_error("nyi", "vs: string split needs a string rhs (byte-string deferred)");
        return q_str_split(ray_str_ptr(y), ray_str_len(y),
                           ray_str_ptr(x), ray_str_len(x));
    }
    if (q_is_null_sym(x)) {
        if (y->type == -RAY_STR)
            return q_str_split_lines(ray_str_ptr(y), ray_str_len(y));
        if (y->type == -RAY_SYM) return q_sym_split(y);
        return ray_error("type", "vs: ` split expects a string or symbol");
    }
    /* --- byte encode (0x0 vs scalar) --- */
    if (x->type == -RAY_U8) {
        if (ray_is_atom(y) && y->type != -RAY_STR) return q_byte_encode(y);
        return ray_error("nyi", "vs: byte-vector base decompose deferred");
    }
    /* --- bit decompose (0b vs scalar) --- */
    if (x->type == -RAY_BOOL) {
        if (ray_is_atom(y)) return q_bit_decompose(y);
        return ray_error("type", "vs: 0b decompose expects a scalar");
    }
    /* --- integer base decompose --- */
    if (q_is_int_atom(x)) {
        int64_t base = q_iatom_val(x);
        if (q_is_int_atom(y)) return q_base_decompose_atom(base, q_iatom_val(y));
        if (q_is_int_vec(y)) {                     /* matrix: pad to max width */
            int64_t m = ray_len(y);
            ray_t* cols = ray_list_new(m > 0 ? m : 1);
            int64_t maxw = 1;
            for (int64_t j = 0; j < m; j++) {
                ray_t* c = q_base_decompose_atom(base, q_ivec_get(y, j));
                if (RAY_IS_ERR(c)) { ray_release(cols); return c; }
                if (ray_len(c) > maxw) maxw = ray_len(c);
                cols = ray_list_append(cols, c); ray_release(c);
            }
            ray_t* rows = ray_list_new(maxw);
            ray_t** cv = (ray_t**)ray_data(cols);
            for (int64_t r = 0; r < maxw; r++) {
                ray_t* row = ray_vec_new(RAY_I64, m); row->len = m;
                int64_t* rd = (int64_t*)ray_data(row);
                for (int64_t j = 0; j < m; j++) {
                    int64_t cw = ray_len(cv[j]);
                    int64_t pad = maxw - cw;         /* left-pad with 0 */
                    rd[j] = (r < pad) ? 0 : ((const int64_t*)ray_data(cv[j]))[r - pad];
                }
                rows = ray_list_append(rows, row); ray_release(row);
            }
            ray_release(cols);
            return rows;
        }
        return ray_error("type", "vs: integer decompose expects an integer rhs");
    }
    if (q_is_int_vec(x)) {
        if (q_is_int_atom(y)) return q_base_decompose_vec(x, q_iatom_val(y));
        return ray_error("nyi", "vs: vector-base matrix decompose deferred");
    }
    return ray_error("type", "vs: unsupported operand types");
}

/* join a boxed list / vector of strings with separator sep (append trailing
 * when host==1, the ` sv newline form). */
static ray_t* q_str_join(ray_t* y, const char* sep, size_t sl, int host) {
    if (!y || y->type != RAY_LIST)
        return ray_error("type", "sv: join expects a list of strings");
    int64_t n = ray_len(y);
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = (y->type == RAY_LIST) ? ((ray_t**)ray_data(y))[i] : NULL;
        if (!e || e->type != -RAY_STR)
            return ray_error("type", "sv: join expects string elements");
        total += ray_str_len(e);
        if (i + 1 < n) total += sl;
    }
    if (host) total += 1;
    char* buf = malloc(total ? total : 1);
    if (!buf) return ray_error("wsfull", "sv: out of memory");
    size_t w = 0;
    ray_t** ev = (ray_t**)ray_data(y);
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = ev[i];
        size_t el = ray_str_len(e);
        memcpy(buf + w, ray_str_ptr(e), el); w += el;
        if (host) { buf[w++] = '\n'; }
        else if (i + 1 < n) { memcpy(buf + w, sep, sl); w += sl; }
    }
    ray_t* r = ray_str(buf, w);
    free(buf);
    return r;
}

/* ` sv `syms — join symbols: leading ':' (file handle) joins with '/', else
 * with '.'  -> single -RAY_SYM atom. */
static ray_t* q_sym_join(ray_t* y) {
    int64_t n = ray_len(y);
    if (n == 0) return ray_sym(ray_sym_intern_runtime("", 0));
    ray_t* first = ray_sym_vec_cell(y, 0);
    const char* fp = first ? ray_str_ptr(first) : "";
    char joiner = (ray_str_len(first) > 0 && fp[0] == ':') ? '/' : '.';
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* c = ray_sym_vec_cell(y, i);
        total += ray_str_len(c);
        if (i + 1 < n) total += 1;
    }
    char* buf = malloc(total ? total : 1);
    if (!buf) return ray_error("wsfull", "sv: out of memory");
    size_t w = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* c = ray_sym_vec_cell(y, i);
        size_t cl = ray_str_len(c);
        memcpy(buf + w, ray_str_ptr(c), cl); w += cl;
        if (i + 1 < n) buf[w++] = joiner;
    }
    int64_t id = ray_sym_intern_runtime(buf, w);
    free(buf);
    return ray_sym(id);
}

/* big-endian byte decode: interpret a U8 vector as a signed integer of the
 * matching width (2->short, 4->int, 8->long). */
static ray_t* q_byte_decode(ray_t* y) {
    int64_t n = ray_len(y);
    const uint8_t* p = (const uint8_t*)ray_data(y);
    uint64_t v = 0;
    for (int64_t i = 0; i < n; i++) v = (v << 8) | p[i];
    if (n == 2) return ray_i16((int16_t)(uint16_t)v);
    if (n == 4) return ray_i32((int32_t)(uint32_t)v);
    if (n == 8) return ray_i64((int64_t)v);
    if (n == 1) return ray_i16((int16_t)(uint8_t)v);
    return ray_error("nyi", "sv: byte decode width %lld deferred", (long long)n);
}

/* bits -> integer (8->byte, 16->short, 32->int, 64->long; 128->guid deferred) */
static ray_t* q_bit_compose(ray_t* y) {
    int64_t n = ray_len(y);
    const uint8_t* p = (const uint8_t*)ray_data(y);
    if (n == 128) return ray_error("nyi", "sv: 128-bit GUID compose deferred");
    if (n != 8 && n != 16 && n != 32 && n != 64)
        return ray_error("nyi", "sv: bit compose width %lld deferred", (long long)n);
    uint64_t v = 0;
    for (int64_t i = 0; i < n; i++) v = (v << 1) | (p[i] & 1);
    if (n == 8)  return ray_u8((uint8_t)v);
    if (n == 16) return ray_i16((int16_t)(uint16_t)v);
    if (n == 32) return ray_i32((int32_t)(uint32_t)v);
    return ray_i64((int64_t)v);
}

static ray_t* q_sv_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "sv: nil operand");
    /* --- string join --- */
    if (x->type == -RAY_STR)
        return q_str_join(y, ray_str_ptr(x), ray_str_len(x), 0);
    if (q_is_null_sym(x)) {
        if (y->type == RAY_SYM) return q_sym_join(y);            /* sym join */
        return q_str_join(y, "\n", 1, 1);                        /* host lines */
    }
    /* --- byte decode (0x0 sv bytes) --- */
    if (x->type == -RAY_U8) {
        if (y->type == RAY_U8) return q_byte_decode(y);
        return ray_error("nyi", "sv: byte-vector base compose deferred");
    }
    /* --- bit compose (0b sv bits) --- */
    if (x->type == -RAY_BOOL) {
        if (y->type == RAY_BOOL) return q_bit_compose(y);
        return ray_error("type", "sv: 0b compose expects a bool vector");
    }
    /* --- integer base compose (Horner) --- */
    if (q_is_int_atom(x)) {
        int64_t base = q_iatom_val(x);
        if (!q_is_int_vec(y) && y->type != RAY_BOOL)
            return ray_error("type", "sv: integer compose expects an integer vector");
        int64_t n = ray_len(y);
        int64_t acc = 0;
        for (int64_t i = 0; i < n; i++) {
            int64_t d = (y->type == RAY_BOOL) ? ((const uint8_t*)ray_data(y))[i]
                                              : q_ivec_get(y, i);
            acc = acc * base + d;
        }
        return ray_i64(acc);
    }
    /* --- mixed-radix compose (vector base) --- */
    if (q_is_int_vec(x)) {
        if (!q_is_int_vec(y)) return ray_error("type", "sv: mixed-radix expects an integer vector rhs");
        int64_t n = ray_len(y), bn = ray_len(x);
        if (n != bn) return ray_error("length", "sv: base and value lengths must match");
        int64_t acc = 0;
        for (int64_t i = 0; i < n; i++)
            acc = acc * q_ivec_get(x, i) + q_ivec_get(y, i);
        return ray_i64(acc);
    }
    return ray_error("type", "sv: unsupported operand types");
}

/* ===== Wave 5 — running / weighted / covariance aggregates ================
 * kdb ref/{sums,prds,maxs,mins,avgs,ratios,wsum,wavg,cov}.md.  Null discipline
 * per page: sum/sums treat null as 0, prd/prds as 1, avg/avgs EXCLUDE nulls,
 * max/min skip nulls (kdb shows -0W/0W for leading nulls — long ±infinity is
 * not representable in this engine's sentinel-null model, so those specific
 * rows are a documented lang-divergence). */

/* Read element i of a numeric vector as a double; *isnull set for the typed
 * null sentinel (int MIN / NaN). */
static double q_velem_f(ray_t* x, int64_t i, int* isnull) {
    *isnull = 0;
    if (ray_is_atom(x)) {                 /* scalar (index ignored) */
        switch (x->type) {
        case -RAY_I64: if (x->i64==NULL_I64){*isnull=1;} return (double)x->i64;
        case -RAY_I32: if (x->i32==NULL_I32){*isnull=1;} return (double)x->i32;
        case -RAY_I16: if (x->i16==NULL_I16){*isnull=1;} return (double)x->i16;
        case -RAY_BOOL:return (double)x->b8;
        case -RAY_U8:  return (double)x->u8;
        case -RAY_F64: case -RAY_F32: if (isnan(x->f64)){*isnull=1;} return x->f64;
        default: *isnull = 1; return 0;
        }
    }
    const void* d = ray_data(x);
    switch (x->type) {
    case RAY_I64: { int64_t v = ((const int64_t*)d)[i]; if (v==NULL_I64){*isnull=1;} return (double)v; }
    case RAY_I32: { int32_t v = ((const int32_t*)d)[i]; if (v==NULL_I32){*isnull=1;} return (double)v; }
    case RAY_I16: { int16_t v = ((const int16_t*)d)[i]; if (v==NULL_I16){*isnull=1;} return (double)v; }
    case RAY_BOOL:return (double)((const uint8_t*)d)[i];
    case RAY_U8:  return (double)((const uint8_t*)d)[i];
    case RAY_F64: { double v = ((const double*)d)[i]; if (isnan(v)){*isnull=1;} return v; }
    case RAY_F32: { float  v = ((const float*)d)[i];  if (isnan(v)){*isnull=1;} return (double)v; }
    default: *isnull = 1; return 0;
    }
}
static int q_vec_is_float(ray_t* x) { return x->type == RAY_F64 || x->type == RAY_F32; }
static int q_vec_is_num(ray_t* x) {
    return x && (x->type==RAY_I64||x->type==RAY_I32||x->type==RAY_I16||
                 x->type==RAY_BOOL||x->type==RAY_U8||x->type==RAY_F64||x->type==RAY_F32);
}

typedef enum { RS_SUMS, RS_PRDS, RS_MAXS, RS_MINS, RS_AVGS } q_rs_kind;

/* running max/min over the bytes of a q string (kdb `maxs "genie"`). */
static ray_t* q_runscan_str(ray_t* x, q_rs_kind k) {
    const char* p = ray_str_ptr(x);
    size_t n = ray_str_len(x);
    char stackb[256];
    char* b = (n <= sizeof stackb) ? stackb : malloc(n ? n : 1);
    if (!b) return ray_error("wsfull", "maxs: out of memory");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (i == 0) b[i] = (char)c;
        else b[i] = (k==RS_MAXS) ? (char)((unsigned char)b[i-1] > c ? (unsigned char)b[i-1] : c)
                                 : (char)((unsigned char)b[i-1] < c ? (unsigned char)b[i-1] : c);
    }
    ray_t* r = ray_str(b, n);
    if (b != stackb) free(b);
    return r;
}

static ray_t* q_runscan(ray_t* x, q_rs_kind k) {
    if (!x) return ray_error("type", "running scan: nil");
    if (x->type == -RAY_STR) {
        if (k==RS_MAXS || k==RS_MINS) return q_runscan_str(x, k);
        return ray_error("type", "running scan: non-numeric");
    }
    if (ray_is_atom(x)) {                 /* atom returned unchanged (avgs->float) */
        if (k == RS_AVGS) { int nu; double v = q_velem_f(x, 0, &nu);
                            return nu ? ray_typed_null(-RAY_F64) : ray_f64(v); }
        ray_retain(x); return x;
    }
    if (!q_vec_is_num(x)) return ray_error("type", "running scan: non-numeric list");
    int64_t n = ray_len(x);
    if (k == RS_AVGS) {
        ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
        double* o = (double*)ray_data(out);
        double s = 0; int64_t c = 0;
        for (int64_t i = 0; i < n; i++) {
            int nu; double v = q_velem_f(x, i, &nu);
            if (!nu) { s += v; c++; }
            if (c == 0) { o[i] = 0; ray_vec_set_null(out, i, true); } else o[i] = s / (double)c;
        }
        return out;
    }
    int isf = q_vec_is_float(x);
    if (isf || k == RS_AVGS) {
        ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
        double* o = (double*)ray_data(out);
        double acc = (k==RS_PRDS) ? 1 : 0; int started = 0; double m = 0;
        for (int64_t i = 0; i < n; i++) {
            int nu; double v = q_velem_f(x, i, &nu);
            if (k==RS_SUMS) { acc += nu?0:v; o[i]=acc; }
            else if (k==RS_PRDS) { acc *= nu?1:v; o[i]=acc; }
            else { if (!nu) { if (!started){m=v;started=1;} else if (k==RS_MAXS?v>m:v<m) m=v; }
                   if (started) o[i]=m; else { o[i]=0; ray_vec_set_null(out,i,true); } }
        }
        return out;
    }
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1); out->len = n;
    int64_t* o = (int64_t*)ray_data(out);
    int64_t acc = (k==RS_PRDS) ? 1 : 0; int started = 0; int64_t m = 0;
    for (int64_t i = 0; i < n; i++) {
        int nu; double vd = q_velem_f(x, i, &nu); int64_t v = (int64_t)vd;
        if (k==RS_SUMS) { acc += nu?0:v; o[i]=acc; }
        else if (k==RS_PRDS) { acc *= nu?1:v; o[i]=acc; }
        else { if (!nu) { if (!started){m=v;started=1;} else if (k==RS_MAXS?v>m:v<m) m=v; }
               if (started) o[i]=m; else { o[i]=NULL_I64; ray_vec_set_null(out,i,true); } }
    }
    return out;
}
static ray_t* q_sums_wrap(ray_t* x){ return q_runscan(x, RS_SUMS); }
static ray_t* q_prds_wrap(ray_t* x){ return q_runscan(x, RS_PRDS); }
static ray_t* q_maxs_wrap(ray_t* x){ return q_runscan(x, RS_MAXS); }
static ray_t* q_mins_wrap(ray_t* x){ return q_runscan(x, RS_MINS); }
static ray_t* q_avgs_wrap(ray_t* x){ return q_runscan(x, RS_AVGS); }

/* q `ratios x` — pairwise ratio: r[0]=x[0], r[i]=x[i] % x[i-1] (float). */
static ray_t* q_ratios_wrap(ray_t* x) {
    if (!x) return ray_error("type", "ratios: nil");
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (!q_vec_is_num(x)) return ray_error("type", "ratios: non-numeric list");
    int64_t n = ray_len(x);
    ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
    double* o = (double*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        if (i == 0) {                       /* r[0] = x[0] (null stays null) */
            if (nu) { o[i] = 0; ray_vec_set_null(out, i, true); } else o[i] = v;
            continue;
        }
        int pnu; double pv = q_velem_f(x, i-1, &pnu);
        /* r[i] = x[i] % x[i-1] with q float-divide semantics: a null operand or
         * a zero divisor yields null (the engine's `%` canonicalizes ±inf/NaN
         * to 0n), not a fabricated value. */
        if (nu || pnu || pv == 0) { o[i] = 0; ray_vec_set_null(out, i, true); }
        else o[i] = v / pv;
    }
    return out;
}

static ray_t* q_fill_wrap(ray_t* x, ray_t* y);   /* fwd — prd's nulls-as-1 fill */

/* q `prd x` — product aggregate, the multiply-over fold twin of prds (ref/prd.md):
 * an atom is returned unchanged; a numeric vector folds to its product with
 * NULLS TREATED AS 1s (`prd 2 3 0N 7` -> 42); a BOOL vector returns an int
 * (`prd 101b` -> 0i); a list of lists multiplies element-wise (`prd (1 2 3 4;
 * 2 3 5 7)` -> 2 6 15 28 — the fold over the registered atomic multiply);
 * a dict folds over its value list (`prd d` -> 40 105 18); a table returns a
 * per-column dict (`prd t` -> a| 630 …), a keyed table folds its VALUE table
 * (`prd k` — implicit-iteration section).  Non-numeric -> 'type. */
static ray_t* q_prd_wrap(ray_t* x) {
    if (!x) return ray_error("type", "prd: nil");
    if (x->type == -RAY_STR || x->type == RAY_SYM)
        return ray_error("type", "prd: expects numeric values, got %s",
                         ray_type_name(x->type));
    if (ray_is_atom(x)) { ray_retain(x); return x; }   /* doc: atom unchanged */
    if (q_is_keyed_table(x))                           /* prd k == prd value t */
        return q_prd_wrap(ray_dict_vals(x));           /* vals borrowed — fine */
    if (x->type == RAY_TABLE) {                        /* per-column dict */
        int64_t nc = ray_table_ncols(x);
        ray_t* k = ray_sym_vec_new(RAY_SYM_W64, nc > 0 ? nc : 1);
        if (!k || RAY_IS_ERR(k)) return k ? k : ray_error("oom", NULL);
        ray_t* v = ray_list_new(nc > 0 ? nc : 1);
        if (RAY_IS_ERR(v)) { ray_release(k); return v; }
        for (int64_t c = 0; c < nc; c++) {
            int64_t nm = ray_table_col_name(x, c);
            k = ray_vec_append(k, &nm);
            if (!k || RAY_IS_ERR(k)) { ray_release(v); return k ? k : ray_error("oom", NULL); }
            ray_t* p = q_prd_wrap(ray_table_get_col_idx(x, c));
            if (!p || RAY_IS_ERR(p)) { ray_release(k); ray_release(v);
                                       return p ? p : ray_error("type", NULL); }
            v = ray_list_append(v, p);                 /* retains */
            ray_release(p);
            if (RAY_IS_ERR(v)) { ray_release(k); return v; }
        }
        ray_t* cv = q_collapse_list(v);                /* owned */
        ray_release(v);
        if (!cv || RAY_IS_ERR(cv)) { ray_release(k); return cv; }
        return ray_dict_new(k, cv);                    /* consumes both */
    }
    if (x->type == RAY_DICT)                           /* fold the value list */
        return q_prd_wrap(ray_dict_vals(x));           /* vals borrowed — fine */
    if (x->type == RAY_LIST) {                         /* element-wise fold */
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (n == 0) return ray_i64(1);                 /* empty product (derived) */
        /* Nulls are 1s here too (ref/prd.md's unconditional rule — codex r2:
         * `prd (1 0N;2 3)` must be 2 3, not 2 0N), so every operand is
         * null-filled with 1 (q `1^`) before it enters the multiply. */
        ray_t* one = ray_i64(1);
        ray_t* acc = q_fill_wrap(one, e[0]);
        if (!acc || RAY_IS_ERR(acc)) { ray_release(one);
                                       return acc ? acc : ray_error("type", NULL); }
        for (int64_t i = 1; i < n; i++) {
            ray_t* fi = q_fill_wrap(one, e[i]);
            if (!fi || RAY_IS_ERR(fi)) { ray_release(acc); ray_release(one);
                                         return fi ? fi : ray_error("type", NULL); }
            /* ray_mul_fn is the ATOM kernel; atomic_map_binary is eval's
             * broadcast (vector*vector, atom*vector, nested) around it. */
            ray_t* nx = atomic_map_binary(ray_mul_fn, acc, fi);
            ray_release(fi);
            ray_release(acc);
            if (!nx || RAY_IS_ERR(nx)) { ray_release(one);
                                         return nx ? nx : ray_error("type", NULL); }
            acc = nx;
        }
        ray_release(one);
        return acc;
    }
    if (!q_vec_is_num(x))
        return ray_error("type", "prd: expects numeric values, got %s",
                         ray_type_name(x->type));
    int64_t n = ray_len(x);
    if (q_vec_is_float(x)) {
        double acc = 1;
        for (int64_t i = 0; i < n; i++) {
            int nu; double v = q_velem_f(x, i, &nu);
            if (!nu) acc *= v;                         /* nulls are 1s */
        }
        return make_f64(acc);
    }
    int64_t acc = 1;
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        if (!nu) acc *= (int64_t)v;
    }
    if (x->type == RAY_BOOL) return ray_i32((int32_t)acc);   /* prd 101b -> 0i */
    return ray_i64(acc);
}

/* q `x wsum y` — weighted sum sum(x*y); `x wavg y` — (sum x*y) % sum x.
 * A pair where EITHER side is null is excluded (kdb).  An atom x broadcasts. */
static ray_t* q_weighted(ray_t* x, ray_t* y, int avg) {
    if (!x || !y) return ray_error("type", "wsum/wavg: nil operand");
    int xatom = ray_is_atom(x);
    if (!q_vec_is_num(y) && !ray_is_atom(y)) return ray_error("type", "wsum/wavg: numeric args only");
    if (!xatom && !q_vec_is_num(x)) return ray_error("type", "wsum/wavg: numeric args only");
    int64_t n = ray_is_atom(y) ? (xatom ? 1 : ray_len(x)) : ray_len(y);
    if (!xatom && !ray_is_atom(y) && ray_len(x) != ray_len(y))
        return ray_error("length", "wsum/wavg: length mismatch");
    double sp = 0, sw = 0;
    for (int64_t i = 0; i < n; i++) {
        int xn, yn;
        double xv = xatom ? q_velem_f(x, 0, &xn) : q_velem_f(x, i, &xn);
        double yv = ray_is_atom(y) ? q_velem_f(y, 0, &yn) : q_velem_f(y, i, &yn);
        if (xn || yn) continue;                 /* exclude null pairs */
        sp += xv * yv; sw += xv;
    }
    if (!avg) return ray_f64(sp);
    if (sw == 0) return ray_typed_null(-RAY_F64);
    return ray_f64(sp / sw);
}
static ray_t* q_wsum_wrap(ray_t* x, ray_t* y){ return q_weighted(x, y, 0); }
static ray_t* q_wavg_wrap(ray_t* x, ray_t* y){ return q_weighted(x, y, 1); }

/* q `x cov y` — population covariance (÷n); `x scov y` — sample (÷n-1).
 * Null pairs excluded. */
static ray_t* q_covariance(ray_t* x, ray_t* y, int sample) {
    if (!x || !y || !q_vec_is_num(x) || !q_vec_is_num(y))
        return ray_error("type", "cov: numeric vectors only");
    int64_t n = ray_len(x);
    if (n != ray_len(y)) return ray_error("length", "cov: length mismatch");
    double sx=0, sy=0; int64_t c=0;
    for (int64_t i = 0; i < n; i++) {
        int xn, yn; double xv=q_velem_f(x,i,&xn), yv=q_velem_f(y,i,&yn);
        if (xn||yn) continue;
        sx+=xv; sy+=yv; c++;
    }
    if (c == 0 || (sample && c < 2)) return ray_typed_null(-RAY_F64);
    double mx=sx/(double)c, my=sy/(double)c, acc=0;
    for (int64_t i = 0; i < n; i++) {
        int xn, yn; double xv=q_velem_f(x,i,&xn), yv=q_velem_f(y,i,&yn);
        if (xn||yn) continue;
        acc += (xv-mx)*(yv-my);
    }
    return ray_f64(acc / (double)(sample ? c-1 : c));
}
static ray_t* q_cov_wrap(ray_t* x, ray_t* y){ return q_covariance(x, y, 0); }
static ray_t* q_scov_wrap(ray_t* x, ray_t* y){ return q_covariance(x, y, 1); }

/* q sliding m-window family `N mf x` — window i covers x[max(0,i-N+1)..i].
 * msum treats null as 0; avg/max/min/dev/count exclude nulls; N<=0 -> empty
 * window (sum/count 0, others null). */
typedef enum { MW_SUM, MW_AVG, MW_MAX, MW_MIN, MW_COUNT, MW_DEV } q_mw_kind;

static ray_t* q_mwin(ray_t* nx, ray_t* x, q_mw_kind k) {
    if (!nx || !(nx->type==-RAY_I64||nx->type==-RAY_I32||nx->type==-RAY_I16))
        return ray_error("type", "m-window: left arg must be an int atom");
    if (!x || !q_vec_is_num(x)) {
        if (x && ray_is_atom(x)) { ray_retain(x); return x; }
        return ray_error("type", "m-window: numeric vector rhs");
    }
    int64_t N = q_iatom_val(nx);
    int64_t n = ray_len(x);
    int isf = q_vec_is_float(x);
    int8_t otype = (k==MW_SUM || k==MW_MAX || k==MW_MIN) ? (isf ? RAY_F64 : RAY_I64)
                 : (k==MW_COUNT) ? RAY_I64 : RAY_F64;
    ray_t* out = ray_vec_new(otype, n > 0 ? n : 1); out->len = n;
    void* o = ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int64_t lo = (N > 0 && i - N + 1 > 0) ? i - N + 1 : 0;
        if (N <= 0) lo = i + 1;                  /* empty window */
        double sum=0, sumsq=0, m=0; int64_t c=0; int started=0;
        for (int64_t j = lo; j <= i; j++) {
            int nu; double v = q_velem_f(x, j, &nu);
            if (k==MW_SUM) { if (!nu) sum += v; continue; }
            if (nu) continue;
            c++; sum += v; sumsq += v*v;
            if (!started) { m=v; started=1; }
            else if (k==MW_MAX ? v>m : v<m) m=v;
        }
        if (otype == RAY_I64) {
            if (k==MW_SUM)   ((int64_t*)o)[i] = (int64_t)sum;
            else if (k==MW_COUNT) ((int64_t*)o)[i] = c;
            else { if (started) ((int64_t*)o)[i] = (int64_t)m;   /* mmax/mmin */
                   else { ((int64_t*)o)[i] = NULL_I64; ray_vec_set_null(out, i, true); } }
        } else {
            double r; int isnull = 0;
            switch (k) {
            case MW_SUM: r = sum; break;
            case MW_MAX: case MW_MIN: if (started) r=m; else { r=0; isnull=1; } break;
            case MW_AVG: if (c) r=sum/(double)c; else { r=0; isnull=1; } break;
            case MW_DEV: if (c) { double mean=sum/(double)c; double var=sumsq/(double)c - mean*mean;
                                  r = var>0 ? sqrt(var) : 0; } else { r=0; isnull=1; } break;
            default: r = 0; break;
            }
            ((double*)o)[i] = r;
            if (isnull) ray_vec_set_null(out, i, true);
        }
    }
    return out;
}
static ray_t* q_msum_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_SUM); }
static ray_t* q_mavg_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_AVG); }
static ray_t* q_mmax_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_MAX); }
static ray_t* q_mmin_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_MIN); }
static ray_t* q_mcount_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_COUNT); }
static ray_t* q_mdev_wrap(ray_t* n, ray_t* x){ return q_mwin(n, x, MW_DEV); }

/* q `a ema x` — exponential moving average: e[0]=x[0], e[i]=a*x[i]+(1-a)*e[i-1].
 * `a` is a float atom (the smoothing factor). */
static ray_t* q_ema_wrap(ray_t* a, ray_t* x) {
    if (!a || !ray_is_atom(a)) return ray_error("type", "ema: smoothing factor must be an atom");
    int an; double alpha = q_velem_f(a, 0, &an);
    if (!x || !q_vec_is_num(x)) {
        if (x && ray_is_atom(x)) { ray_retain(x); return x; }
        return ray_error("type", "ema: numeric vector rhs");
    }
    int64_t n = ray_len(x);
    ray_t* out = ray_vec_new(RAY_F64, n > 0 ? n : 1); out->len = n;
    double* o = (double*)ray_data(out);
    double e = 0; int started = 0;
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        double xv = nu ? 0 : v;
        if (!started) { e = xv; started = 1; }
        else e = alpha * xv + (1.0 - alpha) * e;
        o[i] = e;
    }
    return out;
}

/* q `neg` / monadic `-` — negate.  kdb negates a date's underlying day count
 * PRESERVING the type (function_neg.qcmd: neg 2000.01.01 2012.01.01 ->
 * 2000.01.01 1988.01.01; 0Wd <-> -0Wd; 0Nd passes through), where base
 * ray_neg_fn rejects temporals — so the date arm lives here and every other
 * input delegates.  Registered ATOMIC: eval's atomic_map_unary maps vectors
 * element-wise and its typed-out path already carries RAY_DATE, so a date
 * vector comes back a date vector.  time/timestamp arms arrive with their
 * datatypes (deferred). */
static ray_t* q_neg_wrap(ray_t* x) {
    if (x && x->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_date(-(int64_t)x->i32);
    }
    if (x && x->type == -RAY_MINUTE) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_minute(-(int64_t)x->i32);
    }
    if (x && x->type == -RAY_SECOND) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_second(-(int64_t)x->i32);
    }
    if (x && x->type == -RAY_TIMESPAN) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_timespan(-x->i64);
    }
    if (x && x->type == -RAY_MONTH) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_month(-(int64_t)x->i32);
    }
    if (x && x->type == -RAY_DATETIME) {
        if (RAY_ATOM_IS_NULL(x)) { ray_retain(x); return x; }
        return ray_datetime(-x->f64);
    }
    /* kdb `neg` promotes a boolean to INT and negates (`neg 1b` -> -1i);
     * base ray_neg_fn rejects bools.  Registered ATOMIC, so a bool vector
     * arrives here element-wise and the i32 atoms collapse to an i32 vector. */
    if (x && x->type == -RAY_BOOL)
        return ray_i32(-(int32_t)(x->b8 ? 1 : 0));
    return ray_neg_fn(x);
}

/* q `null x` — elementwise null test.  Drives the engine's atomic `nil?`
 * (ray_nil_fn) through atomic_map_unary so it broadcasts over typed vectors
 * AND nested general lists at every depth; collection_elem reconstructs
 * typed-null atoms, so nulls are SEEN (unlike other atomics, which stay
 * null-avoiding via the dispatch guards).  Registered RAY_FN_NONE — NOT
 * ATOMIC — so it receives the whole argument here and owns the collapse: a
 * heterogeneous input list yields a homogeneous bool-atom run that
 * q_collapse_list folds to a bool vector (`null (1;\`a;2.5;"x")` -> 0000b),
 * while a nested list yields a list of bool VECTORS that q_collapse_list
 * leaves intact (multi-line, `null (0N 1;2 0N)` -> 10b / 01b). */
/* q-layer null test: the base engine's `ray_nil_fn` treats sym id 0 as the
 * EMPTY symbol (a value, include/rayforce.h SYM case), but q treats the null
 * symbol `` ` `` AS null (`null \`` -> 1b).  This wrapper special-cases the
 * null symbol here in the q layer so the divergence stays out of base rayfall,
 * whose own paths rely on sym-0-as-empty.  Drives `q_null_wrap` for both the
 * atom path and the per-element `atomic_map_unary` recursion (nested lists /
 * symbol vectors reconstruct null-sym atoms via collection_elem). */
static ray_t* q_nil_fn(ray_t* x) {
    if (q_is_null_sym(x)) return ray_bool(true);
    return ray_nil_fn(x);
}

/* q `raze x` — base ray_raze_fn plus the kdb atom arm: an atom comes back as
 * a 1-item list (ref/raze.md `raze 42` -> ,42).  Everything else delegates. */
ray_t* ray_raze_fn(ray_t* x);                        /* base (ops/builtins.c) */
static ray_t* q_raze_wrap(ray_t* x) {
    /* strings are kdb char LISTS (rank 1) — never the atom arm */
    if (x && ray_is_atom(x) && x->type != -RAY_STR) {
        ray_t* l = ray_list_new(1);
        if (RAY_IS_ERR(l)) return l;
        l = ray_list_append(l, x);                   /* retains x */
        if (RAY_IS_ERR(l)) return l;
        ray_t* c = q_collapse_list(l);               /* owned */
        ray_release(l);
        return c;
    }
    return ray_raze_fn(x);
}

/* q `enlist` — base ray_enlist_fn plus the kdb dict arm: enlist of a bare
 * dict is a 1-ROW TABLE (ref/enlist.md: `` enlist `a`b`c!(1;2 3; 4) ``
 * displays a table whose b cell is 2 3).  Construction: the dict with each
 * value ENLISTED (1-item column; atoms collapse to typed 1-vecs, vector
 * cells stay boxed) flipped through the one flip home.  Env-bound by
 * q_builtins_register BEFORE registry init, so the `,` monadic QK_ENV
 * snapshot picks this wrapper up too. */
ray_t* ray_enlist_fn(ray_t** args, int64_t n);       /* base (ops/builtins.c) */
ray_t* q_enlist_wrap_vary(ray_t** args, int64_t n) {
    if (n == 1 && args[0] && args[0]->type == RAY_DICT && !q_is_keyed_table(args[0])) {
        ray_t* d = args[0];
        ray_t* k = ray_dict_keys(d);                 /* borrowed */
        ray_t* v = ray_dict_vals(d);                 /* borrowed */
        if (!k || !v) return ray_error("type", "enlist: malformed dictionary");
        int64_t nd = ray_dict_len(d);
        ray_t* ev = ray_list_new(nd > 0 ? nd : 1);
        if (RAY_IS_ERR(ev)) return ev;
        for (int64_t i = 0; i < nd; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* cell = ray_at_fn(v, ia);          /* owned */
            ray_release(ia);
            if (!cell || RAY_IS_ERR(cell)) { ray_release(ev); return cell ? cell : ray_error("type", NULL); }
            ray_t* col = ray_list_new(1);
            if (RAY_IS_ERR(col)) { ray_release(cell); ray_release(ev); return col; }
            col = ray_list_append(col, cell);        /* retains */
            ray_release(cell);
            if (RAY_IS_ERR(col)) { ray_release(ev); return col; }
            ray_t* cc = q_collapse_list(col);        /* atoms -> typed 1-vec */
            ray_release(col);
            if (!cc || RAY_IS_ERR(cc)) { ray_release(ev); return cc ? cc : ray_error("type", NULL); }
            ev = ray_list_append(ev, cc);            /* retains */
            ray_release(cc);
            if (RAY_IS_ERR(ev)) return ev;
        }
        ray_retain(k);                               /* dict_new consumes */
        ray_t* ed = ray_dict_new(k, ev);             /* consumes k + ev */
        if (!ed || RAY_IS_ERR(ed)) return ed ? ed : ray_error("type", NULL);
        ray_t* t = q_flip_wrap(ed);                  /* owned table */
        ray_release(ed);
        return t;
    }
    return ray_enlist_fn(args, n);
}

static ray_t* q_null_wrap(ray_t* x) {
    ray_t* r = is_collection(x) ? atomic_map_unary(q_nil_fn, x) : q_nil_fn(x);
    if (!r || RAY_IS_ERR(r) || r->type != RAY_LIST) return r;
    ray_t* c = q_collapse_list(r);   /* owned: retains-or-builds */
    ray_release(r);
    return c;
}

/* q `x within y` — bounds check (ref/within.md: 1 3 10 6 4 within 2 6 ->
 * 01011b; inclusive).  Base ray_within_fn takes VECTOR vals only and reads
 * the range buffer at the vals' element width, so: an atom x is enlisted
 * (via list+collapse) and the answer unwrapped back to a bool atom, and the
 * two element widths must agree ('type — a silent misread otherwise).  The
 * flip-of-pairs range form and mixed-width operands are deferred cells. */
static ray_t* q_within_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "within: nil operand");
    if (!ray_is_vec(y) || ray_len(y) != 2)
        return ray_error("type", "within: range must be a 2-item vector");
    ray_t* vals = x;
    ray_t* vals_owned = NULL;
    if (ray_is_atom(x)) {
        ray_t* l = ray_list_new(1);
        if (RAY_IS_ERR(l)) return l;
        l = ray_list_append(l, x);
        if (RAY_IS_ERR(l)) return l;
        vals_owned = q_collapse_list(l);
        ray_release(l);
        if (!vals_owned || RAY_IS_ERR(vals_owned))
            return vals_owned ? vals_owned : ray_error("type", NULL);
        if (!ray_is_vec(vals_owned)) {           /* strings & friends: deferred */
            ray_release(vals_owned);
            return ray_error("type", "within: unsupported value type (deferred)");
        }
        vals = vals_owned;
    }
    if (!ray_is_vec(vals)) {
        if (vals_owned) ray_release(vals_owned);
        return ray_error("type", "within: unsupported value type (deferred)");
    }
    /* Base ray_within_fn dispatches on vals->type ONLY and reads the range
     * buffer as that element type, so ANY type mismatch — not just a width
     * mismatch — would silently reinterpret raw bits (codex: 1 2 within
     * 1.5 2.5 read the doubles as int64 -> 00b).  Same-type operands only;
     * mixed-type coercion is a deferred cell (error, never a wrong answer). */
    if (vals->type != y->type) {
        if (vals_owned) ray_release(vals_owned);
        return ray_error("type", "within: value/range types must match (mixed-type deferred)");
    }
    ray_t* r;
    if (vals->type == RAY_TIMESTAMP) {
        /* base ray_within_fn has no i64-temporal arm; the payload is i64, so
         * relabel both sides through the one cast home and delegate (the
         * same-byte-rep TIMESTAMP<->I64 relabel, builtins.c). */
        ray_t* vi = q_cast_to(RAY_I64, vals);
        if (!vi || RAY_IS_ERR(vi)) { if (vals_owned) ray_release(vals_owned); return vi; }
        ray_t* yi = q_cast_to(RAY_I64, y);
        if (!yi || RAY_IS_ERR(yi)) {
            ray_release(vi);
            if (vals_owned) ray_release(vals_owned);
            return yi;
        }
        r = ray_within_fn(vi, yi);
        ray_release(vi);
        ray_release(yi);
    } else {
        r = ray_within_fn(vals, y);
    }
    if (!vals_owned) return r;                    /* vector x: pass through */
    ray_release(vals_owned);
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* idx = ray_i64(0);                      /* atom x: unwrap 1-vec */
    ray_t* a = ray_at_fn(r, idx);
    ray_release(idx);
    ray_release(r);
    return a;
}

/* ===== q list verbs (feat/q-list-verbs) ===================================
 * rotate / sublist / next / prev / fill (`^`).  kdb rotate.md, sublist.md,
 * next.md, prev.md, and `^` (fill).  rotate/sublist reuse q_gather (index-vector
 * gather via ray_at_fn, collapsed to a typed vector; string-aware); next/prev do
 * a typed-vector shift with a sentinel-null fill; fill coalesces nulls.  Dict /
 * table / mixed-list / non-nullable-type forms are DEFERRED cells (error, never
 * a wrong answer) — their ledger rows live in the *-deferred.qcmd companions. */

/* q `n rotate x` — cyclic shift left by n (negative = right; kdb rotate.md).
 * n is an int atom; the right arg is a vector/list/string.  Empty x returns a
 * copy (no modulo-by-zero).  Reuses q_gather with recycle=1. */
static ray_t* q_rotate_wrap(ray_t* n, ray_t* x) {
    if (!n || !(n->type == -RAY_I64 || n->type == -RAY_I32 || n->type == -RAY_I16))
        return ray_error("type", "rotate: left arg must be an int atom");
    if (RAY_ATOM_IS_NULL(n))
        return ray_error("nyi", "rotate: 0N left arg is deferred");
    /* table arm (ref/rotate.md: a table rotates its ROWS) — cyclic row gather */
    if (x && x->type == RAY_TABLE) {
        int64_t total = ray_table_nrows(x);
        if (total <= 0) { ray_retain(x); return x; }
        int64_t k = q_iatom_val(n);
        k = ((k % total) + total) % total;
        return q_row_gather(x, k, total, 1);
    }
    if (!x || (!ray_is_vec(x) && x->type != RAY_LIST && x->type != -RAY_STR))
        return ray_error("type", "rotate: right arg must be a list, vector, or string (keyed table deferred)");
    int64_t total = (x->type == -RAY_STR) ? (int64_t)ray_str_len(x) : ray_len(x);
    if (total <= 0) { ray_retain(x); return x; }
    int64_t k = q_iatom_val(n);
    k = ((k % total) + total) % total;         /* normalize into [0,total) */
    return q_gather(x, k, total, total, 1);     /* recycle -> cyclic */
}

/* q `n sublist x` — sub-list (kdb sublist.md).  Atom n>=0 -> first min(n,len);
 * n<0 -> last min(|n|,len).  Int-pair `i j` -> j items from position i (both
 * clamped into range; TRUNCATING, never null-extending).  Reuses q_gather with
 * recycle=0.  Dict / table right args are deferred. */
static ray_t* q_sublist_wrap(ray_t* n, ray_t* x) {
    int is_tbl  = x && x->type == RAY_TABLE;
    int is_dict = x && x->type == RAY_DICT && !q_is_keyed_table(x);
    if (!x || (!is_tbl && !is_dict && !ray_is_vec(x) && x->type != RAY_LIST &&
               x->type != -RAY_STR))
        return ray_error("type", "sublist: right arg must be a list, vector, string, dict, or table (keyed table deferred)");
    int64_t total = is_tbl  ? ray_table_nrows(x)
                  : is_dict ? ray_dict_len(x)
                  : (x->type == -RAY_STR) ? (int64_t)ray_str_len(x) : ray_len(x);
    int64_t start, count;
    if (n && (n->type == -RAY_I64 || n->type == -RAY_I32 || n->type == -RAY_I16)) {
        if (RAY_ATOM_IS_NULL(n)) return ray_error("nyi", "sublist: 0N left arg is deferred");
        int64_t k = q_iatom_val(n);
        if (k >= 0) { start = 0; count = (k < total) ? k : total; }
        else { int64_t kk = -k; count = (kk < total) ? kk : total; start = total - count; }
    } else if (n && (n->type == RAY_I64 || n->type == RAY_I32 || n->type == RAY_I16) &&
               ray_len(n) == 2) {
        int64_t i = q_ivec_get(n, 0), j = q_ivec_get(n, 1);
        if (i < 0) i = 0;                        /* clamp negative start to 0 */
        if (j < 0) j = 0;
        start = (i < total) ? i : total;
        count = j;
        if (start + count > total) count = total - start;   /* truncate tail */
        if (count < 0) count = 0;
    } else {
        return ray_error("type", "sublist: left arg must be an int atom or a 2-item int vector");
    }
    if (is_tbl) return q_row_gather(x, start, count, 0);   /* rows stay a table */
    if (is_dict) {
        /* ENTRY-structural: keys and values move together (the drop-arm split;
         * ray_take_fn range takes work on both typed key vectors and boxed
         * value lists).  Borrowed accessors; ray_dict_new consumes both. */
        ray_t* k = ray_dict_keys(x);                 /* borrowed */
        ray_t* v = ray_dict_vals(x);                 /* borrowed */
        if (!k || !v) return ray_error("type", "sublist: malformed dictionary");
        int64_t rng[2] = { start, count };
        ray_t* range = ray_vec_from_raw(RAY_I64, rng, 2);
        if (RAY_IS_ERR(range)) return range;
        ray_t* rk = ray_take_fn(k, range);           /* owned */
        if (rk && ray_is_lazy(rk)) rk = ray_lazy_materialize(rk);
        if (!rk || RAY_IS_ERR(rk)) { ray_release(range); return rk; }
        ray_t* rv = ray_take_fn(v, range);           /* owned */
        ray_release(range);
        if (rv && ray_is_lazy(rv)) rv = ray_lazy_materialize(rv);
        if (!rv || RAY_IS_ERR(rv)) { ray_release(rk); return rv; }
        return ray_dict_new(rk, rv);                 /* consumes both */
    }
    return q_gather(x, start, count, total, 0);  /* truncating, no recycle */
}

/* q `next x` / `prev x` — shift a simple vector by one, null-filling the vacated
 * end (kdb next.md / prev.md).  Restricted to the sentinel-nullable element
 * types (int / float / temporal): SYM/BOOL/U8/STR/LIST/atom forms have no
 * shift-in null here and are deferred cells. */
static ray_t* q_shift1(ray_t* x, int forward) {
    /* generic-list arm (ref/next.md): the vacated slot takes an EMPTY of the
     * FIRST item of the ORIGINAL list (`prev (1 2;"abc";`ibm)` ->
     * (`long$();1 2;"abc")); next fills the TAIL, prev the HEAD.  `0#first`
     * via ray_take_fn; a take that cannot empty (odd atom kinds) degrades to
     * the empty generic list. */
    if (x && x->type == RAY_LIST) {
        int64_t len = ray_len(x);
        if (len == 0) { ray_retain(x); return x; }
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* zero = ray_i64(0);
        ray_t* fill = e[0] ? ray_take_fn(e[0], zero) : NULL;   /* owned 0#first */
        ray_release(zero);
        if (!fill || RAY_IS_ERR(fill)) {
            if (fill) ray_release(fill);
            fill = ray_list_new(1);                  /* empty () fallback */
            if (RAY_IS_ERR(fill)) return fill;
        }
        ray_t* out = ray_list_new(len);
        if (RAY_IS_ERR(out)) { ray_release(fill); return out; }
        if (!forward) {                              /* prev: fill leads */
            out = ray_list_append(out, fill);
            if (RAY_IS_ERR(out)) { ray_release(fill); return out; }
        }
        int64_t from = forward ? 1 : 0, to = forward ? len : len - 1;
        for (int64_t i = from; i < to; i++) {
            out = ray_list_append(out, e[i]);        /* retains */
            if (RAY_IS_ERR(out)) { ray_release(fill); return out; }
        }
        if (forward) {                               /* next: fill trails */
            out = ray_list_append(out, fill);
            if (RAY_IS_ERR(out)) { ray_release(fill); return out; }
        }
        ray_release(fill);
        return out;
    }
    if (!x || !ray_is_vec(x))
        return ray_error("nyi", "next/prev: only simple numeric vectors (string/sym/atom deferred)");
    int8_t t = x->type;
    if (!(t == RAY_I16 || t == RAY_I32 || t == RAY_I64 || t == RAY_F32 || t == RAY_F64 ||
          RAY_IS_TEMPORAL32(t) || RAY_IS_TEMPORAL64(t) || RAY_IS_TEMPORALF(t)))
        return ray_error("nyi", "next/prev: %s vectors are deferred", ray_type_name(t));
    int64_t len = ray_len(x);
    size_t esz = ray_type_sizes[(uint8_t)t];
    ray_t* out = ray_vec_new(t, len > 0 ? len : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = len;
    char* o = (char*)ray_data(out);
    const char* in = (const char*)ray_data(x);
    if (len > 0) {
        if (forward) {                           /* next: o[i]=x[i+1], tail null */
            if (len > 1) memcpy(o, in + esz, (size_t)(len - 1) * esz);
            ray_vec_set_null(out, len - 1, true);
        } else {                                 /* prev: o[i]=x[i-1], head null */
            if (len > 1) memcpy(o + esz, in, (size_t)(len - 1) * esz);
            ray_vec_set_null(out, 0, true);
        }
    }
    return out;
}
static ray_t* q_next_wrap(ray_t* x) { return q_shift1(x, 1); }
static ray_t* q_prev_wrap(ray_t* x) { return q_shift1(x, 0); }

/* q `n xprev x` — shift x by n items (ref/next.md: positive n is prev-by-n,
 * negative is next-by-|n|), null-filling the vacated end — the same typed-
 * vector shift as next/prev generalized to |n|.  A -RAY_STR atom shifts its
 * CHARS, vacated positions taking ' ' (kdb's null char): `1 xprev "abcde"`
 * -> " abcd". */
static ray_t* q_xprev_wrap(ray_t* nx, ray_t* x) {
    if (!nx || !(nx->type == -RAY_I64 || nx->type == -RAY_I32 || nx->type == -RAY_I16) ||
        RAY_ATOM_IS_NULL(nx))
        return ray_error("type", "xprev: left arg must be an int atom");
    int64_t k = q_iatom_val(nx);
    if (x && x->type == -RAY_STR) {
        int64_t len = (int64_t)ray_str_len(x);
        const char* s = ray_str_ptr(x);
        char stackb[256];
        char* b = (len <= (int64_t)sizeof stackb) ? stackb : malloc((size_t)(len > 0 ? len : 1));
        if (!b) return ray_error("oom", NULL);
        for (int64_t i = 0; i < len; i++) {
            int64_t j = i - k;
            b[i] = (j >= 0 && j < len) ? s[j] : ' ';
        }
        ray_t* r = ray_str(b, (size_t)len);
        if (b != stackb) free(b);
        return r;
    }
    if (!x || !ray_is_vec(x))
        return ray_error("nyi", "xprev: only simple vectors and strings (list/dict/table deferred)");
    int8_t t = x->type;
    if (!(t == RAY_I16 || t == RAY_I32 || t == RAY_I64 || t == RAY_F32 || t == RAY_F64 ||
          RAY_IS_TEMPORAL32(t) || RAY_IS_TEMPORAL64(t) || RAY_IS_TEMPORALF(t)))
        return ray_error("nyi", "xprev: %s vectors are deferred", ray_type_name(t));
    int64_t len = ray_len(x);
    size_t esz = ray_type_sizes[(uint8_t)t];
    ray_t* out = ray_vec_new(t, len > 0 ? len : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = len;
    char* o = (char*)ray_data(out);
    const char* in = (const char*)ray_data(x);
    int64_t sh = k >= 0 ? k : -k;
    if (sh > len) sh = len;
    int64_t keep = len - sh;
    if (k >= 0) {                                    /* prev-by-n: head nulls */
        if (keep > 0) memcpy(o + (size_t)sh * esz, in, (size_t)keep * esz);
        for (int64_t i = 0; i < sh; i++) ray_vec_set_null(out, i, true);
    } else {                                         /* next-by-n: tail nulls */
        if (keep > 0) memcpy(o, in + (size_t)sh * esz, (size_t)keep * esz);
        for (int64_t i = keep; i < len; i++) ray_vec_set_null(out, i, true);
    }
    return out;
}

/* q `fills x` — forward-fill: each null takes the last preceding non-null
 * (ref/fill.md; `fills` is the `^\` fill-scan).  Leading nulls stay null.
 * Numeric vectors keep q_fill_wrap's I64/F64 result split; SYM vectors carry
 * the last non-null sym id (id 0 IS q's null sym, same test as q_fill_wrap).
 * Atoms pass through; other shapes are deferred cells. */
static ray_t* q_fills_wrap(ray_t* x) {
    if (x && ray_is_atom(x) && x->type != RAY_DICT) { ray_retain(x); return x; }
    if (x && x->type == RAY_SYM) {
        int64_t n = ray_len(x);
        ray_t* outl = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(outl)) return outl;
        int64_t carry = 0;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* se = ray_at_fn(x, ia);            /* owned sym atom */
            ray_release(ia);
            if (!se || RAY_IS_ERR(se)) { ray_release(outl); return se; }
            int64_t id = se->i64;
            ray_release(se);
            if (id != 0) carry = id;                 /* 0 == null sym */
            ray_t* oe = ray_sym(carry);
            outl = ray_list_append(outl, oe);
            ray_release(oe);
            if (RAY_IS_ERR(outl)) return outl;
        }
        ray_t* c = q_collapse_list(outl);
        ray_release(outl);
        return c;
    }
    if (!x || !q_vec_is_num(x))
        return ray_error("nyi", "fills: numeric or symbol vector (dict/table deferred)");
    int isf = q_vec_is_float(x);
    int64_t n = ray_len(x);
    ray_t* out = ray_vec_new(isf ? RAY_F64 : RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    void* o = ray_data(out);
    double carry = 0; int have = 0;
    for (int64_t i = 0; i < n; i++) {
        int nu; double v = q_velem_f(x, i, &nu);
        if (!nu) { carry = v; have = 1; }
        int isnull = nu && !have;
        double use = nu ? carry : v;
        if (isf) ((double*)o)[i]  = isnull ? NULL_F64 : use;
        else     ((int64_t*)o)[i] = isnull ? NULL_I64 : (int64_t)use;
        if (isnull) ray_vec_set_null(out, i, true);
    }
    return out;
}

/* Whole-item scan: does any ITEM of container y match v (kdb `~`)?  Indexes
 * via ray_at_fn so typed vectors (STR lists-of-strings included) and boxed
 * lists share one home.  Borrows both. */
static int q_seq_has_item(ray_t* y, ray_t* v) {
    int64_t n = ray_len(y);
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* ye = ray_at_fn(y, ia);                /* owned item */
        ray_release(ia);
        if (!ye || RAY_IS_ERR(ye)) { if (ye) ray_release(ye); continue; }
        int hit = q_values_match(ye, v);
        ray_release(ye);
        if (hit) return 1;
    }
    return 0;
}

/* q `x^y` — fill: coalesce nulls in y with x (kdb `^`).  x may be an atom
 * (broadcast) or a same-length vector (element-wise).  Numeric result type:
 * F64 if EITHER operand is float, else I64 (the narrower-int-preserving lattice
 * is a deferred refinement — the `type` ledger rows that need it are blocked by
 * a separate `0n 2 3i` parse bug and split out).  Symbol fill is a distinct
 * path.  Dict / `fills` forward-fill / table / fill-scan forms are deferred. */
static int q_is_float_t(int8_t t) {
    return t == RAY_F64 || t == RAY_F32 || t == -RAY_F64 || t == -RAY_F32;
}
static int q_is_num_t(int8_t t) {
    return t == RAY_BOOL || t == RAY_U8 || t == RAY_I16 || t == RAY_I32 || t == RAY_I64 ||
           t == RAY_F32 || t == RAY_F64 || t == -RAY_BOOL || t == -RAY_U8 || t == -RAY_I16 ||
           t == -RAY_I32 || t == -RAY_I64 || t == -RAY_F32 || t == -RAY_F64;
}
static int q_is_sym_t(int8_t t) { return t == RAY_SYM || t == -RAY_SYM; }

static ray_t* q_fill_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "^: nil operand");
    int xatom = ray_is_atom(x), yatom = ray_is_atom(y);

    /* ---- symbol fill ---- */
    if (q_is_sym_t(x->type) || q_is_sym_t(y->type)) {
        if (!q_is_sym_t(x->type) || !q_is_sym_t(y->type))
            return ray_error("type", "^: symbol fill needs symbol operands");
        /* length follows y when it is a vector; a scalar y broadcasts to the
         * length of a vector x (`` `a`b`c^` `` -> 3 items), matching the
         * numeric branch below. */
        int64_t len = yatom ? (xatom ? 1 : ray_len(x)) : ray_len(y);
        if (!xatom && !yatom && ray_len(x) != len)
            return ray_error("length", "^: operand lengths must match");
        ray_t* outl = ray_list_new(len > 0 ? len : 1);
        if (RAY_IS_ERR(outl)) return outl;
        for (int64_t i = 0; i < len; i++) {
            int64_t yid;
            if (yatom) yid = y->i64;
            else { ray_t* ia = ray_i64(i); ray_t* ye = ray_at_fn(y, ia); ray_release(ia);
                   if (!ye || RAY_IS_ERR(ye)) { ray_release(outl); return ye; }
                   yid = ye->i64; ray_release(ye); }
            int64_t use = yid;
            if (yid == 0) {                      /* empty/null sym -> fill */
                if (xatom) use = x->i64;
                else { ray_t* ia = ray_i64(i); ray_t* xe = ray_at_fn(x, ia); ray_release(ia);
                       if (!xe || RAY_IS_ERR(xe)) { ray_release(outl); return xe; }
                       use = xe->i64; ray_release(xe); }
            }
            ray_t* se = ray_sym(use);
            outl = ray_list_append(outl, se); ray_release(se);
            if (RAY_IS_ERR(outl)) return outl;
        }
        ray_t* c = q_collapse_list(outl); ray_release(outl);
        if (yatom && xatom) {                    /* scalar^scalar -> atom */
            ray_t* ia = ray_i64(0); ray_t* a = ray_at_fn(c, ia); ray_release(ia); ray_release(c);
            return a;
        }
        return c;
    }

    /* ---- numeric fill ---- */
    if (!q_is_num_t(x->type) || !q_is_num_t(y->type))
        return ray_error("type", "^: unsupported operand types (dict/table/list fill deferred)");
    int is_float = q_is_float_t(x->type) || q_is_float_t(y->type);
    int64_t len = yatom ? (xatom ? 1 : ray_len(x)) : ray_len(y);
    if (!xatom && !yatom && ray_len(x) != ray_len(y))
        return ray_error("length", "^: operand lengths must match");
    if (xatom && yatom) {                        /* scalar^scalar -> atom */
        int yn; double yv = q_velem_f(y, 0, &yn);
        if (!yn) return is_float ? ray_f64(yv) : ray_i64((int64_t)yv);
        int xn; double xv = q_velem_f(x, 0, &xn);
        if (xn) return ray_typed_null(is_float ? -RAY_F64 : -RAY_I64);
        return is_float ? ray_f64(xv) : ray_i64((int64_t)xv);
    }
    ray_t* out = ray_vec_new(is_float ? RAY_F64 : RAY_I64, len > 0 ? len : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = len;
    void* o = ray_data(out);
    for (int64_t i = 0; i < len; i++) {
        int yn; double yv = yatom ? q_velem_f(y, 0, &yn) : q_velem_f(y, i, &yn);
        double v; int isnull = 0;
        if (!yn) v = yv;
        else {
            int xn; double xv = xatom ? q_velem_f(x, 0, &xn) : q_velem_f(x, i, &xn);
            if (xn) { v = 0; isnull = 1; } else v = xv;
        }
        if (is_float) ((double*)o)[i] = isnull ? NULL_F64 : v;
        else          ((int64_t*)o)[i] = isnull ? NULL_I64 : (int64_t)v;
        if (isnull) ray_vec_set_null(out, i, true);
    }
    return out;
}

/* q `x in y` — membership (ref/in.md).  Where y is a TYPED vector the test is
 * left-atomic (delegates to base ray_in_fn); where y is a generic LIST there
 * is NO iteration through x — x is tested WHOLE against the ITEMS of y, and
 * the search is rank-sensitive via y's FIRST item (find.md: a rank-n haystack
 * looks for rank n-1 objects): first item non-atom -> whole-x match (a rank-0
 * x is 0b: `3 in (1 2;3)` -> 0b); first item atom (or empty y — undocumented
 * edge, conservative) -> left-atomic over x against y's items.  Mixed numeric
 * families (float x vs int y) are allowed only against an ATOM or 1-item y
 * (elementwise equality); longer/empty mixed vectors are 'type.  A 1-char
 * string x against string y unwraps the base char row to an ATOM bool. */
static ray_t* q_in_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "in: nil operand");
    if (y->type == RAY_LIST) {
        int64_t ny = ray_len(y);
        ray_t** e = (ray_t**)ray_data(y);
        int rank1_seek = ny > 0 && e[0] && !ray_is_atom(e[0]);
        if (rank1_seek) {
            if (ray_is_atom(x)) return ray_bool(false);
            return ray_bool(q_seq_has_item(y, x) != 0);
        }
        if (ray_is_atom(x)) return ray_bool(q_seq_has_item(y, x) != 0);
        int64_t nx = ray_len(x);                     /* left-atomic over x */
        ray_t* outl = ray_list_new(nx > 0 ? nx : 1);
        if (RAY_IS_ERR(outl)) return outl;
        for (int64_t i = 0; i < nx; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* xe = ray_at_fn(x, ia);            /* owned */
            ray_release(ia);
            if (!xe || RAY_IS_ERR(xe)) { ray_release(outl); return xe; }
            ray_t* r = q_in_wrap(xe, y);
            ray_release(xe);
            if (!r || RAY_IS_ERR(r)) { ray_release(outl); return r; }
            outl = ray_list_append(outl, r);         /* retains */
            ray_release(r);
            if (RAY_IS_ERR(outl)) return outl;
        }
        ray_t* c = q_collapse_list(outl);
        ray_release(outl);
        return c;
    }
    /* STR-vector y (openq list-of-strings): whole-item membership -> atom */
    if (y->type == RAY_STR && x->type == -RAY_STR)
        return ray_bool(q_seq_has_item(y, x) != 0);
    /* mixed numeric families (ref/in.md Mixed argument types): allowed only
     * against an ATOM or 1-item y — elementwise numeric equality (q_velem_f
     * reads both families; nulls never match). */
    if (q_is_num_t(x->type) && q_is_num_t(y->type)) {
        int xf = q_is_float_t(x->type), yf = q_is_float_t(y->type);
        if (xf != yf) {
            if (!ray_is_atom(y) && ray_len(y) != 1)
                return ray_error("type", "in: mixed numeric types need an atom or 1-item right arg");
            int yn; double yv = q_velem_f(y, 0, &yn);
            if (ray_is_atom(x)) {
                int nu; double v = q_velem_f(x, 0, &nu);
                return ray_bool(!nu && !yn && v == yv);
            }
            int64_t n = ray_len(x);
            ray_t* out = ray_vec_new(RAY_BOOL, n > 0 ? n : 1);
            if (RAY_IS_ERR(out)) return out;
            out->len = n;
            bool* o = (bool*)ray_data(out);
            for (int64_t i = 0; i < n; i++) {
                int nu; double v = q_velem_f(x, i, &nu);
                o[i] = !nu && !yn && v == yv;
            }
            return out;
        }
    }
    ray_t* r = ray_in_fn(x, y);
    /* 1-char string x: base char membership returns a 1-vec; kdb wants an
     * ATOM (`"x" in "a"` -> 0b). */
    if (r && !RAY_IS_ERR(r) && x->type == -RAY_STR && ray_str_len(x) == 1 &&
        r->type == RAY_BOOL && ray_len(r) == 1) {
        int b = ((const bool*)ray_data(r))[0] != 0;
        ray_release(r);
        return ray_bool(b != 0);
    }
    return r;
}

/* Call the env-bound BINARY builtin `nm` (the wrapper-over-env pattern:
 * some base fns — dict — are declared only in internal base headers, so the
 * wrapper routes through the audited env value instead of a frozen-header
 * include).  Borrowed args; returns owned. */
static ray_t* q_env_call2(const char* nm, ray_t* a, ray_t* b) {
    ray_t* f = ray_env_get(ray_sym_intern(nm, strlen(nm)));
    if (!f || f->type != RAY_BINARY)
        return ray_error("type", "%s: env builtin missing", nm);
    return ((ray_binary_fn)(uintptr_t)f->i64)(a, b);
}

/* Unary sibling of q_env_call2 (guid roll/deal routes through the audited
 * env `guid` value).  Borrowed arg; returns owned. */
static ray_t* q_env_call1(const char* nm, ray_t* a) {
    ray_t* f = ray_env_get(ray_sym_intern(nm, strlen(nm)));
    if (!f || f->type != RAY_UNARY)
        return ray_error("type", "%s: env builtin missing", nm);
    return ((ray_unary_fn)(uintptr_t)f->i64)(a);
}

/* A keyed table is a RAY_DICT whose keys AND values are both tables.
 * Exported (q_registry.h) — q_builtins' by-name deref shares it. */
int q_is_keyed_table(ray_t* y) {
    if (!y || y->type != RAY_DICT) return 0;
    ray_t* k = ray_dict_keys(y);
    ray_t* v = ray_dict_vals(y);
    return k && v && k->type == RAY_TABLE && v->type == RAY_TABLE;
}

/* Flatten a plain-or-keyed table to a single plain table (key cols then value
 * cols).  Returns owned. */
static ray_t* q_table_flatten(ray_t* y) {
    if (y->type == RAY_TABLE) { ray_retain(y); return y; }
    ray_t* kt = ray_dict_keys(y);          /* borrowed */
    ray_t* vt = ray_dict_vals(y);          /* borrowed */
    int64_t knc = ray_table_ncols(kt), vnc = ray_table_ncols(vt);
    ray_t* out = ray_table_new(knc + vnc > 0 ? knc + vnc : 1);
    for (int64_t c = 0; c < knc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(kt, c), ray_table_get_col_idx(kt, c));
    for (int64_t c = 0; c < vnc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, ray_table_col_name(vt, c), ray_table_get_col_idx(vt, c));
    return out;
}

/* q enkey/unkey `N!table`: 0 -> plain table (unkey), N>0 -> key the first N
 * columns into a keyed table (RAY_DICT keycols-table -> valcols-table).
 * Accepts a plain OR already-keyed table (re-keys).  Consumes nothing. */
static ray_t* q_enkey(ray_t* y, int64_t nkey) {
    ray_t* flat = q_table_flatten(y);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    int64_t nc = ray_table_ncols(flat);
    if (nkey <= 0) return flat;                 /* unkey */
    if (nkey >= nc) { ray_release(flat); return ray_error("length", "!: key count exceeds columns"); }
    ray_t* kt = ray_table_new(nkey);
    ray_t* vt = ray_table_new(nc - nkey);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(kt) && !RAY_IS_ERR(vt); c++) {
        int64_t nm = ray_table_col_name(flat, c);
        ray_t* col = ray_table_get_col_idx(flat, c);
        if (c < nkey) kt = ray_table_add_col(kt, nm, col);
        else          vt = ray_table_add_col(vt, nm, col);
    }
    ray_release(flat);
    if (RAY_IS_ERR(kt)) { ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);
}

/* ===== table verbs (feat/q-table-verbs) ====================================
 * flip/keys/xkey/xcol/xcols/xasc/xdesc/xgroup/ungroup/insert/upsert + the
 * table arms of distinct/union/inter/except.  All built over the wave-4
 * keyed primitives above (q_is_keyed_table / q_enkey / q_table_flatten) —
 * NEVER duplicated (the #56 failure mode).  Row-equality is boxed q-match
 * compares: O(n^2) wrapper-tier code at test scale by design (single-home
 * principle; SIMD paths belong to the engine).                              */

/* Extract symbol ids from a -RAY_SYM atom / RAY_SYM vector / LIST of sym
 * atoms.  Returns count, or -1 on a non-symbol operand.  cap-bounded. */
static int64_t q_sym_ids(ray_t* x, int64_t* out, int64_t cap) {
    if (!x) return -1;
    if (x->type == -RAY_SYM) { if (cap < 1) return -1; out[0] = x->i64; return 1; }
    if (x->type == RAY_SYM) {
        int64_t n = ray_len(x);
        if (n > cap) return -1;
        for (int64_t i = 0; i < n; i++) {
            /* borrowed domain atom — never released (table/sym.h) */
            ray_t* s = ray_sym_vec_cell(x, i);
            out[i] = s ? ray_sym_intern_runtime(ray_str_ptr(s), ray_str_len(s)) : 0;
        }
        return n;
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        if (n > cap) return -1;
        ray_t** e = (ray_t**)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            if (!e[i] || e[i]->type != -RAY_SYM) return -1;
            out[i] = e[i]->i64;
        }
        return n;
    }
    return -1;
}

/* Column index of name id in table t, or -1. */
static int64_t q_col_index(ray_t* t, int64_t nm) {
    int64_t nc = ray_table_ncols(t);
    for (int64_t c = 0; c < nc; c++)
        if (ray_table_col_name(t, c) == nm) return c;
    return -1;
}

/* Reorder a plain table: the named columns first (in given order), the rest
 * in original order.  'length when a name is missing.  Returns owned. */
static ray_t* q_table_reorder(ray_t* t, const int64_t* names, int64_t n) {
    int64_t nc = ray_table_ncols(t);
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t i = 0; i < n && !RAY_IS_ERR(out); i++) {
        int64_t c = q_col_index(t, names[i]);
        if (c < 0) { ray_release(out); return ray_error("length", "column not found"); }
        out = ray_table_add_col(out, names[i], ray_table_get_col_idx(t, c));
    }
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
        int64_t nm = ray_table_col_name(t, c);
        int used = 0;
        for (int64_t i = 0; i < n; i++) if (names[i] == nm) { used = 1; break; }
        if (!used) out = ray_table_add_col(out, nm, ray_table_get_col_idx(t, c));
    }
    return out;
}

/* Whole-value equality of two boxed cells (q match semantics). */
static int q_cell_eq(ray_t* a, ray_t* b) {
    return q_match_rec(a, b);
}

/* Row equality over the FIRST ncmp columns of two tables (boxed compare). */
static int q_row_eq(ray_t* ta, int64_t ra, ray_t* tb, int64_t rb, int64_t ncmp) {
    for (int64_t c = 0; c < ncmp; c++) {
        ray_t* ia = ray_i64(ra);
        ray_t* av = ray_at_fn(ray_table_get_col_idx(ta, c), ia);
        ray_release(ia);
        ray_t* ib = ray_i64(rb);
        ray_t* bv = ray_at_fn(ray_table_get_col_idx(tb, c), ib);
        ray_release(ib);
        int eq = (av && bv && !RAY_IS_ERR(av) && !RAY_IS_ERR(bv)) ? q_cell_eq(av, bv) : 0;
        if (av) ray_release(av);
        if (bv) ray_release(bv);
        if (!eq) return 0;
    }
    return 1;
}

/* Row count of a plain OR keyed table (keyed via its key table — never trust
 * ray_len on a string-atom column). */
static int64_t q_any_nrows(ray_t* t) {
    if (q_is_keyed_table(t)) return ray_table_nrows(ray_dict_keys(t));
    return ray_table_nrows(t);
}

/* 0-based long index vector [start, start+n). */
static ray_t* q_idx_range(int64_t start, int64_t n) {
    ray_t* v = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(v)) return v;
    v->len = n;
    int64_t* d = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < n; i++) d[i] = start + i;
    return v;
}

/* n copies of atom `a` as a collapsed column (broadcast helper). */
static ray_t* q_bcast_col(ray_t* a, int64_t n) {
    ray_t* l = ray_list_new(n > 0 ? n : 1);
    if (RAY_IS_ERR(l)) return l;
    for (int64_t i = 0; i < n; i++) {
        l = ray_list_append(l, a);
        if (RAY_IS_ERR(l)) return l;
    }
    ray_t* c = q_collapse_list(l);
    ray_release(l);
    return c;
}

/* Resolve a table operand that may be BY NAME (-RAY_SYM naming a global).
 * Returns the borrowed target (env-owned, or the operand itself) and sets
 * *sym_out to the name id (or -1 for by-value).  NULL => not a table. */
static ray_t* q_table_operand(ray_t* y, int64_t* sym_out) {
    *sym_out = -1;
    if (!y) return NULL;
    if (y->type == -RAY_SYM) {
        ray_t* g = ray_env_get(y->i64);
        if (g && (g->type == RAY_TABLE || q_is_keyed_table(g))) { *sym_out = y->i64; return g; }
        return NULL;
    }
    if (y->type == RAY_TABLE || q_is_keyed_table(y)) return y;
    return NULL;
}

/* q `flip x` / monadic `+` — transpose.
 *   table         -> dict (colnames ! list-of-columns)      [flip flip t ~ t]
 *   dict          -> table (sym keys; vector vals share one length L, atoms
 *                    broadcast to L; mismatched vector length -> 'length)
 *   list of lists -> transposed list (atom items broadcast)
 * Keyed tables, atoms, and an all-atom list are 'rank DEFERRED cells (the
 * all-atom arm is a choice, not verified kdb behaviour). */
static ray_t* q_flip_wrap(ray_t* x) {
    if (!x) return ray_error("type", "flip: nil");
    if (x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* k = ray_sym_vec_new(RAY_SYM_W64, nc > 0 ? nc : 1);
        if (!k || RAY_IS_ERR(k)) return k ? k : ray_error("oom", NULL);
        ray_t* v = ray_list_new(nc > 0 ? nc : 1);
        if (RAY_IS_ERR(v)) { ray_release(k); return v; }
        for (int64_t c = 0; c < nc; c++) {
            int64_t nm = ray_table_col_name(x, c);
            k = ray_vec_append(k, &nm);
            if (!k || RAY_IS_ERR(k)) { ray_release(v); return k ? k : ray_error("oom", NULL); }
            v = ray_list_append(v, ray_table_get_col_idx(x, c));   /* retains */
            if (RAY_IS_ERR(v)) { ray_release(k); return v; }
        }
        return ray_dict_new(k, v);                        /* consumes both */
    }
    if (q_is_keyed_table(x)) return ray_error("rank", "flip: keyed table");
    if (x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);                      /* borrowed */
        ray_t* v = ray_dict_vals(x);                      /* borrowed */
        if (!k || k->type != RAY_SYM || !v)
            return ray_error("type", "flip: dict must map symbols to columns");
        int64_t nc = ray_len(k);
        if (!(v->type == RAY_LIST || ray_is_vec(v)) || ray_len(v) != nc)
            return ray_error("length", "flip: key and value counts differ");
        /* pass 1: L = shared vector length (atoms broadcast; all-atom -> 1) */
        int64_t L = -1;
        for (int64_t c = 0; c < nc; c++) {
            ray_t* ia = ray_i64(c); ray_t* col = ray_at_fn(v, ia); ray_release(ia);
            if (!col || RAY_IS_ERR(col)) return col ? col : ray_error("oom", NULL);
            if (!ray_is_atom(col)) {
                int64_t l = ray_len(col);
                if (L < 0) L = l;
                else if (l != L) { ray_release(col); return ray_error("length", "flip: column lengths differ"); }
            }
            ray_release(col);
        }
        if (L < 0) L = 1;
        /* pass 2: build the table (atoms broadcast to L) */
        ray_t* out = ray_table_new(nc > 0 ? nc : 1);
        for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
            /* borrowed domain atom — never released (table/sym.h) */
            ray_t* s = ray_sym_vec_cell(k, c);
            int64_t nm = s ? ray_sym_intern_runtime(ray_str_ptr(s), ray_str_len(s)) : 0;
            ray_t* ia = ray_i64(c); ray_t* col = ray_at_fn(v, ia); ray_release(ia);
            if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : ray_error("oom", NULL); }
            if (ray_is_atom(col)) {
                ray_t* b = q_bcast_col(col, L);
                ray_release(col);
                col = b;
                if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : ray_error("oom", NULL); }
            }
            out = ray_table_add_col(out, nm, col);
            ray_release(col);
        }
        return out;
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        if (n == 0) { ray_retain(x); return x; }
        ray_t** e = (ray_t**)ray_data(x);
        int64_t L = -1;
        for (int64_t i = 0; i < n; i++) {
            ray_t* it = e[i];
            if (it && (ray_is_vec(it) || it->type == RAY_LIST)) {
                int64_t l = ray_len(it);
                if (L < 0) L = l;
                else if (l != L) return ray_error("length", "flip: row lengths differ");
            }
        }
        if (L < 0) return ray_error("rank", "flip: needs at least one list item");
        ray_t* out = ray_list_new(L > 0 ? L : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t r = 0; r < L; r++) {
            ray_t* rowl = ray_list_new(n);
            if (RAY_IS_ERR(rowl)) { ray_release(out); return rowl; }
            for (int64_t i = 0; i < n; i++) {
                ray_t* it = e[i];
                ray_t* cell;
                if (it && (ray_is_vec(it) || it->type == RAY_LIST)) {
                    ray_t* ia = ray_i64(r); cell = ray_at_fn(it, ia); ray_release(ia);
                } else { cell = it; if (cell) ray_retain(cell); }
                if (!cell || RAY_IS_ERR(cell)) { ray_release(rowl); ray_release(out); return cell ? cell : ray_error("oom", NULL); }
                rowl = ray_list_append(rowl, cell);
                ray_release(cell);
                if (RAY_IS_ERR(rowl)) { ray_release(out); return rowl; }
            }
            ray_t* rowc = q_collapse_list(rowl);
            ray_release(rowl);
            if (!rowc || RAY_IS_ERR(rowc)) { ray_release(out); return rowc ? rowc : ray_error("oom", NULL); }
            out = ray_list_append(out, rowc);
            ray_release(rowc);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return ray_error("rank", "flip: unsupported operand");
}

/* q `keys x` — key column names (empty sym vector if unkeyed; table by value
 * or by name). */
static ray_t* q_keys_wrap(ray_t* x) {
    int64_t sym;
    ray_t* t = q_table_operand(x, &sym);
    if (!t) return ray_error("type", "keys: expects a table");
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    if (q_is_keyed_table(t)) {
        ray_t* kt = ray_dict_keys(t);                     /* borrowed */
        int64_t knc = ray_table_ncols(kt);
        for (int64_t c = 0; c < knc; c++) {
            int64_t nm = ray_table_col_name(kt, c);
            out = ray_vec_append(out, &nm);
            if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        }
    }
    return out;
}

/* q `x xkey y` — set key columns: reorder x-first, enkey count x (reuses
 * q_enkey).  By-reference (y a name): rebind and return the name. */
static ray_t* q_xkey_wrap(ray_t* x, ray_t* y) {
    int64_t names[64];
    int64_t n = q_sym_ids(x, names, 64);
    if (n < 0) return ray_error("type", "xkey: keys must be symbols");
    int64_t sym;
    ray_t* t = q_table_operand(y, &sym);
    if (!t) return ray_error("type", "xkey: expects a table");
    ray_t* flat = q_table_flatten(t);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    ray_t* keyed;
    if (n == 0) keyed = flat;                             /* () xkey t -> unkey */
    else {
        ray_t* reord = q_table_reorder(flat, names, n);
        ray_release(flat);
        if (!reord || RAY_IS_ERR(reord)) return reord;
        keyed = q_enkey(reord, n);
        ray_release(reord);
        if (!keyed || RAY_IS_ERR(keyed)) return keyed;
    }
    if (sym >= 0) {
        ray_env_bind(sym, keyed);                         /* retains */
        ray_release(keyed);
        ray_retain(y);
        return y;
    }
    return keyed;
}

/* q `x xcol y` — rename columns.  x: sym atom/vector renames the FIRST n
 * columns; a dict (`a`c!`A`C) or an all-key keyed table (([a:`A;c:`C]))
 * renames selected columns.  Unknown old name -> 'length (ref/cols.md). */
static ray_t* q_xcol_wrap(ray_t* x, ray_t* y) {
    if (!y || y->type != RAY_TABLE) return ray_error("type", "xcol: expects a table");
    int64_t nc = ray_table_ncols(y);
    if (nc > 64) return ray_error("limit", "xcol: too many columns");
    int64_t newnm[64];
    for (int64_t c = 0; c < nc; c++) newnm[c] = ray_table_col_name(y, c);
    int64_t names[64];
    int64_t n = q_sym_ids(x, names, 64);
    if (n >= 0) {                                         /* positional rename */
        if (n > nc) return ray_error("length", "xcol: more names than columns");
        for (int64_t i = 0; i < n; i++) newnm[i] = names[i];
    } else if (x && x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);                      /* borrowed */
        ray_t* v = ray_dict_vals(x);                      /* borrowed */
        if (k && k->type == RAY_TABLE) {                  /* all-key keyed map */
            int64_t mn = ray_table_ncols(k);
            for (int64_t i = 0; i < mn; i++) {
                int64_t old = ray_table_col_name(k, i);
                ray_t* ia = ray_i64(0);
                ray_t* cell = ray_at_fn(ray_table_get_col_idx(k, i), ia);
                ray_release(ia);
                if (!cell || RAY_IS_ERR(cell) || cell->type != -RAY_SYM) {
                    if (cell && !RAY_IS_ERR(cell)) { ray_release(cell); return ray_error("type", "xcol: bad rename map"); }
                    return cell ? cell : ray_error("type", "xcol: bad rename map");
                }
                int64_t c = q_col_index(y, old);
                if (c < 0) { ray_release(cell); return ray_error("length", "xcol: column not found"); }
                newnm[c] = cell->i64;
                ray_release(cell);
            }
        } else {                                          /* plain sym!sym dict */
            /* keys: RAY_SYM vector; vals: RAY_SYM vector OR a boxed LIST of
             * sym atoms (rayfall dict boxes heterogeneous-looking vals) —
             * read both sides via boxed access. */
            if (!k || k->type != RAY_SYM || !v ||
                !(v->type == RAY_SYM || v->type == RAY_LIST))
                return ray_error("type", "xcol: dict must map symbols to symbols");
            int64_t mn = ray_len(k);
            if (ray_len(v) != mn)
                return ray_error("length", "xcol: rename map is ragged");
            for (int64_t i = 0; i < mn; i++) {
                /* borrowed domain atom — never released (table/sym.h) */
                ray_t* ks = ray_sym_vec_cell(k, i);
                int64_t old = ks ? ray_sym_intern_runtime(ray_str_ptr(ks), ray_str_len(ks)) : 0;
                ray_t* ia = ray_i64(i);
                ray_t* vs = ray_at_fn(v, ia);             /* owned */
                ray_release(ia);
                if (!vs || RAY_IS_ERR(vs) || vs->type != -RAY_SYM) {
                    if (vs && !RAY_IS_ERR(vs)) { ray_release(vs); return ray_error("type", "xcol: dict must map symbols to symbols"); }
                    return vs ? vs : ray_error("type", "xcol: dict must map symbols to symbols");
                }
                int64_t nw = vs->i64;
                ray_release(vs);
                int64_t c = q_col_index(y, old);
                if (c < 0) return ray_error("length", "xcol: column not found");
                newnm[c] = nw;
            }
        }
    } else return ray_error("type", "xcol: expects symbols or a rename dict");
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++)
        out = ray_table_add_col(out, newnm[c], ray_table_get_col_idx(y, c));
    return out;
}

/* q `x xcols y` — reorder: x columns first (ref/cols.md). */
static ray_t* q_xcols_wrap(ray_t* x, ray_t* y) {
    if (!y || y->type != RAY_TABLE) return ray_error("type", "xcols: expects a table");
    int64_t names[64];
    int64_t n = q_sym_ids(x, names, 64);
    if (n < 0) return ray_error("type", "xcols: expects symbol column names");
    return q_table_reorder(y, names, n);
}

/* q `x xasc y` / `x xdesc y` — sort a table by columns (stable base kernel,
 * ARG-SWAP like QK_XBAR).  y by name: sort the global in place, rebind,
 * return the name.  A keyed table sorts its flattened columns and re-keys.
 * No `s#` attribute (deferred divergence, same as QK_ASC). */
static ray_t* q_xsort(ray_t* x, ray_t* y, int desc) {
    int64_t sym;
    ray_t* t = q_table_operand(y, &sym);
    if (!t) return ray_error("type", "xasc/xdesc: expects a table");
    int keyed = q_is_keyed_table(t);
    int64_t nkey = keyed ? ray_table_ncols(ray_dict_keys(t)) : 0;
    ray_t* flat = q_table_flatten(t);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    ray_t* sorted = desc ? ray_xdesc_fn(flat, x) : ray_xasc_fn(flat, x);
    ray_release(flat);
    if (!sorted || RAY_IS_ERR(sorted)) return sorted;
    if (keyed) {
        ray_t* rk = q_enkey(sorted, nkey);
        ray_release(sorted);
        if (!rk || RAY_IS_ERR(rk)) return rk;
        sorted = rk;
    }
    if (sym >= 0) {
        ray_env_bind(sym, sorted);                        /* retains */
        ray_release(sorted);
        ray_retain(y);
        return y;
    }
    return sorted;
}
static ray_t* q_xasc_wrap(ray_t* x, ray_t* y)  { return q_xsort(x, y, 0); }
static ray_t* q_xdesc_wrap(ray_t* x, ray_t* y) { return q_xsort(x, y, 1); }

/* q `x xgroup y` — key by x, remaining columns become per-group nested lists
 * (first-occurrence group order, ref/xgroup.md). */
static ray_t* q_xgroup_wrap(ray_t* x, ray_t* y) {
    int64_t names[64];
    int64_t nk = q_sym_ids(x, names, 64);
    if (nk <= 0) return ray_error("type", "xgroup: expects symbol column names");
    int64_t sym;
    ray_t* t = q_table_operand(y, &sym);
    if (!t) return ray_error("type", "xgroup: expects a table");
    ray_t* flat = q_table_flatten(t);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    ray_t* reord = q_table_reorder(flat, names, nk);      /* key cols first */
    ray_release(flat);
    if (!reord || RAY_IS_ERR(reord)) return reord;
    int64_t nc = ray_table_ncols(reord);
    int64_t nr = ray_table_nrows(reord);
    /* group ids by first occurrence (boxed compare, test-scale O(n*g)) */
    int64_t* gid = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    int64_t* rep = malloc(sizeof(int64_t) * (size_t)(nr > 0 ? nr : 1));
    if (!gid || !rep) { free(gid); free(rep); ray_release(reord); return ray_error("wsfull", "xgroup: out of memory"); }
    int64_t ng = 0;
    for (int64_t r = 0; r < nr; r++) {
        int64_t g = -1;
        for (int64_t j = 0; j < ng && g < 0; j++)
            if (q_row_eq(reord, r, reord, rep[j], nk)) g = j;
        if (g < 0) { rep[ng] = r; g = ng++; }
        gid[r] = g;
    }
    /* key table: first-occurrence cells of the key columns */
    ray_t* kt = ray_table_new(nk);
    for (int64_t c = 0; c < nk && !RAY_IS_ERR(kt); c++) {
        ray_t* acc = ray_list_new(ng > 0 ? ng : 1);
        for (int64_t j = 0; j < ng && !RAY_IS_ERR(acc); j++) {
            ray_t* ia = ray_i64(rep[j]);
            ray_t* cell = ray_at_fn(ray_table_get_col_idx(reord, c), ia);
            ray_release(ia);
            if (!cell || RAY_IS_ERR(cell)) { ray_release(acc); acc = cell ? cell : ray_error("oom", NULL); break; }
            acc = ray_list_append(acc, cell);
            ray_release(cell);
        }
        if (RAY_IS_ERR(acc)) { ray_release(kt); kt = acc; break; }
        ray_t* cc = q_collapse_list(acc);
        ray_release(acc);
        if (!cc || RAY_IS_ERR(cc)) { ray_release(kt); kt = cc ? cc : ray_error("oom", NULL); break; }
        kt = ray_table_add_col(kt, ray_table_col_name(reord, c), cc);
        ray_release(cc);
    }
    /* value table: per group, gather each value column by row indices */
    ray_t* vt = RAY_IS_ERR(kt) ? (ray_retain(kt), kt) : ray_table_new(nc - nk > 0 ? nc - nk : 1);
    for (int64_t c = nk; c < nc && !RAY_IS_ERR(vt); c++) {
        ray_t* acc = ray_list_new(ng > 0 ? ng : 1);
        for (int64_t j = 0; j < ng && !RAY_IS_ERR(acc); j++) {
            int64_t cnt = 0;
            for (int64_t r = 0; r < nr; r++) if (gid[r] == j) cnt++;
            ray_t* idx = ray_vec_new(RAY_I64, cnt > 0 ? cnt : 1);
            if (RAY_IS_ERR(idx)) { ray_release(acc); acc = idx; break; }
            idx->len = 0;
            for (int64_t r = 0; r < nr && !RAY_IS_ERR(idx); r++)
                if (gid[r] == j) idx = ray_vec_append(idx, &r);
            if (RAY_IS_ERR(idx)) { ray_release(acc); acc = idx; break; }
            ray_t* grp = ray_at_fn(ray_table_get_col_idx(reord, c), idx);
            ray_release(idx);
            if (!grp || RAY_IS_ERR(grp)) { ray_release(acc); acc = grp ? grp : ray_error("oom", NULL); break; }
            ray_t* gc;
            if (grp->type == RAY_LIST) { gc = q_collapse_list(grp); ray_release(grp); }
            else gc = grp;
            if (!gc || RAY_IS_ERR(gc)) { ray_release(acc); acc = gc ? gc : ray_error("oom", NULL); break; }
            acc = ray_list_append(acc, gc);
            ray_release(gc);
        }
        if (RAY_IS_ERR(acc)) { ray_release(vt); vt = acc; break; }
        vt = ray_table_add_col(vt, ray_table_col_name(reord, c), acc);
        ray_release(acc);
    }
    free(gid); free(rep);
    ray_release(reord);
    if (RAY_IS_ERR(kt)) { if (vt && !RAY_IS_ERR(vt)) ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);                          /* consumes both */
}

/* q `ungroup x` — inverse of xgroup: explode nested list columns, repeating
 * simple cells; ragged nested rows -> 'length.  Keyed tables flatten first. */
static ray_t* q_ungroup_wrap(ray_t* x) {
    int64_t sym;
    ray_t* t = q_table_operand(x, &sym);
    if (!t) return ray_error("type", "ungroup: expects a table");
    ray_t* flat = q_table_flatten(t);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    int64_t nc = ray_table_ncols(flat);
    int64_t nr = ray_table_nrows(flat);
    if (nc > 64) { ray_release(flat); return ray_error("limit", "ungroup: too many columns"); }
    ray_t* acc[64];
    for (int64_t c = 0; c < nc; c++) {
        acc[c] = ray_list_new(nr > 0 ? nr : 1);
        if (RAY_IS_ERR(acc[c])) {
            ray_t* err = acc[c];
            for (int64_t j = 0; j < c; j++) ray_release(acc[j]);
            ray_release(flat);
            return err;
        }
    }
    ray_t* err = NULL;
    for (int64_t r = 0; r < nr && !err; r++) {
        int64_t cnt = -1;
        for (int64_t c = 0; c < nc && !err; c++) {
            ray_t* col = ray_table_get_col_idx(flat, c);
            if (col && col->type == RAY_LIST) {
                ray_t* ia = ray_i64(r);
                ray_t* cell = ray_at_fn(col, ia);
                ray_release(ia);
                if (!cell || RAY_IS_ERR(cell)) { err = cell ? cell : ray_error("oom", NULL); break; }
                int64_t l = cell->type == -RAY_STR ? (int64_t)ray_str_len(cell)
                          : (ray_is_vec(cell) || cell->type == RAY_LIST) ? ray_len(cell) : 1;
                ray_release(cell);
                if (cnt < 0) cnt = l;
                else if (l != cnt) err = ray_error("length", "ungroup: nested cell lengths differ");
            }
        }
        if (err) break;
        if (cnt < 0) cnt = 1;                             /* no nested column */
        for (int64_t c = 0; c < nc && !err; c++) {
            ray_t* col = ray_table_get_col_idx(flat, c);
            ray_t* ia = ray_i64(r);
            ray_t* cell = ray_at_fn(col, ia);
            ray_release(ia);
            if (!cell || RAY_IS_ERR(cell)) { err = cell ? cell : ray_error("oom", NULL); break; }
            if (col && col->type == RAY_LIST) {           /* explode nested cell */
                for (int64_t i = 0; i < cnt && !err; i++) {
                    ray_t* ib = ray_i64(i);
                    ray_t* e = ray_at_fn(cell, ib);
                    ray_release(ib);
                    if (!e || RAY_IS_ERR(e)) { err = e ? e : ray_error("oom", NULL); break; }
                    acc[c] = ray_list_append(acc[c], e);
                    ray_release(e);
                    if (RAY_IS_ERR(acc[c])) err = acc[c];
                }
            } else {                                      /* repeat simple cell */
                for (int64_t i = 0; i < cnt && !err; i++) {
                    acc[c] = ray_list_append(acc[c], cell);
                    if (RAY_IS_ERR(acc[c])) err = acc[c];
                }
            }
            ray_release(cell);
        }
    }
    if (err) {
        for (int64_t c = 0; c < nc; c++)
            if (acc[c] && !RAY_IS_ERR(acc[c])) ray_release(acc[c]);
        ray_release(flat);
        return err;
    }
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* cc = q_collapse_list(acc[c]);
        ray_release(acc[c]);
        if (!RAY_IS_ERR(out)) {
            if (cc && !RAY_IS_ERR(cc)) {
                out = ray_table_add_col(out, ray_table_col_name(flat, c), cc);
            } else {
                ray_release(out);
                out = cc ? cc : ray_error("oom", NULL);
                cc = NULL;
            }
        }
        if (cc && !RAY_IS_ERR(cc)) ray_release(cc);
        else if (cc && RAY_IS_ERR(cc) && out != cc) ray_release(cc);
    }
    ray_release(flat);
    return out;
}

/* Typed null cell matching a column's element type (missing-column fill). */
static ray_t* q_null_cell_like(ray_t* col) {
    if (!col) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    int8_t t = col->type;
    if (t == RAY_SYM || t == -RAY_SYM) return ray_sym(ray_sym_intern_runtime("", 0));
    if (t == -RAY_STR || t == RAY_STR) return ray_str("", 0);
    if (ray_is_vec(col)) return ray_typed_null((int8_t)-t);
    if (ray_is_atom(col)) return ray_typed_null(t);
    ray_retain(RAY_NULL_OBJ);                             /* list/empty column */
    return RAY_NULL_OBJ;
}

/* Normalize an insert/upsert payload y against the FLAT target schema.
 * Returns an OWNED plain table with the target's column names holding the
 * new rows.  Forms (ref/insert.md, ref/upsert.md; ambiguity rules per the
 * plan's review addendum):
 *   - TABLE (plain/keyed): columns matched BY NAME.  Payload columns unknown
 *     to the target -> 'mismatch (silent drop is never OK).  Target columns
 *     absent from the payload: null-filled when `partial` (upsert), else
 *     'mismatch (insert).
 *   - LIST with count == ncols: columns-form — item i is column i, atoms
 *     broadcast to the longest item (all-atom == the single-record form).
 *   - other LIST: records-form — every item a list/vector of ncols cells.
 *   - DICT: one row, name-matched (strict: key set must be a subset AND
 *     cover; partial: unknown keys ignored, missing columns null-filled).
 *   - 1-column target: an atom/vector payload IS the column. */
static ray_t* q_rows_normalize(ray_t* flat, ray_t* y, int partial) {
    if (!y) return ray_error("type", "insert/upsert: nil payload");
    int64_t nc = ray_table_ncols(flat);
    if (nc <= 0) return ray_error("type", "insert/upsert: target has no columns");
    if (nc > 64) return ray_error("limit", "insert/upsert: too many columns");

    if (y->type == RAY_TABLE || q_is_keyed_table(y)) {
        ray_t* src = q_table_flatten(y);
        if (!src || RAY_IS_ERR(src)) return src;
        int64_t snc = ray_table_ncols(src);
        for (int64_t c = 0; c < snc; c++) {
            if (q_col_index(flat, ray_table_col_name(src, c)) < 0) {
                ray_release(src);
                return ray_error("mismatch", NULL);
            }
        }
        int64_t nr = ray_table_nrows(src);
        ray_t* out = ray_table_new(nc);
        for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
            int64_t nm = ray_table_col_name(flat, c);
            int64_t sc = q_col_index(src, nm);
            if (sc >= 0) {
                out = ray_table_add_col(out, nm, ray_table_get_col_idx(src, sc));
                continue;
            }
            if (!partial) { ray_release(out); ray_release(src); return ray_error("mismatch", NULL); }
            ray_t* acc = ray_list_new(nr > 0 ? nr : 1);
            for (int64_t r = 0; r < nr && !RAY_IS_ERR(acc); r++) {
                ray_t* nl = q_null_cell_like(ray_table_get_col_idx(flat, c));
                acc = ray_list_append(acc, nl);
                ray_release(nl);
            }
            if (RAY_IS_ERR(acc)) { ray_release(out); ray_release(src); return acc; }
            ray_t* cc = q_collapse_list(acc);
            ray_release(acc);
            if (!cc || RAY_IS_ERR(cc)) { ray_release(out); ray_release(src); return cc ? cc : ray_error("oom", NULL); }
            out = ray_table_add_col(out, nm, cc);
            ray_release(cc);
        }
        ray_release(src);
        return out;
    }

    if (y->type == RAY_DICT) {
        ray_t* dk = ray_dict_keys(y);                     /* borrowed */
        if (!dk || dk->type != RAY_SYM)
            return ray_error("type", "insert/upsert: dict row keys must be symbols");
        if (!partial) {
            int64_t dn = ray_len(dk);
            for (int64_t i = 0; i < dn; i++) {
                /* borrowed domain atom — never released (table/sym.h) */
                ray_t* s = ray_sym_vec_cell(dk, i);
                int64_t id = s ? ray_sym_intern_runtime(ray_str_ptr(s), ray_str_len(s)) : 0;
                if (q_col_index(flat, id) < 0) return ray_error("mismatch", NULL);
            }
        }
        ray_t* out = ray_table_new(nc);
        for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
            int64_t nm = ray_table_col_name(flat, c);
            ray_t* ka = ray_sym(nm);
            ray_t* cellv = ray_dict_get(y, ka);           /* owned or NULL */
            ray_release(ka);
            if (!cellv) {
                if (!partial) { ray_release(out); return ray_error("mismatch", NULL); }
                cellv = q_null_cell_like(ray_table_get_col_idx(flat, c));
            }
            if (RAY_IS_ERR(cellv)) { ray_release(out); return cellv; }
            ray_t* col = q_bcast_col(cellv, 1);
            ray_release(cellv);
            if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : ray_error("oom", NULL); }
            out = ray_table_add_col(out, nm, col);
            ray_release(col);
        }
        return out;
    }

    if (nc == 1 && y->type != RAY_LIST) {
        ray_t* col;
        if (ray_is_atom(y)) col = q_bcast_col(y, 1);
        else { ray_retain(y); col = y; }
        if (!col || RAY_IS_ERR(col)) return col ? col : ray_error("oom", NULL);
        ray_t* out = ray_table_new(1);
        if (!RAY_IS_ERR(out)) out = ray_table_add_col(out, ray_table_col_name(flat, 0), col);
        ray_release(col);
        return out;
    }

    if (y->type != RAY_LIST && !ray_is_vec(y))
        return ray_error("type", "insert/upsert: unsupported payload");

    int64_t ny = ray_len(y);

    if (ny == nc) {                                       /* columns-form */
        int64_t L = -1;
        for (int64_t c = 0; c < nc; c++) {
            ray_t* ia = ray_i64(c);
            ray_t* it = ray_at_fn(y, ia);
            ray_release(ia);
            if (!it || RAY_IS_ERR(it)) return it ? it : ray_error("oom", NULL);
            if (!ray_is_atom(it)) {
                int64_t l = ray_len(it);
                if (L < 0) L = l;
                else if (l != L) { ray_release(it); return ray_error("length", NULL); }
            }
            ray_release(it);
        }
        if (L < 0) L = 1;
        ray_t* out = ray_table_new(nc);
        for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
            ray_t* ia = ray_i64(c);
            ray_t* it = ray_at_fn(y, ia);
            ray_release(ia);
            if (!it || RAY_IS_ERR(it)) { ray_release(out); return it ? it : ray_error("oom", NULL); }
            ray_t* col;
            if (ray_is_atom(it)) { col = q_bcast_col(it, L); ray_release(it); }
            else col = it;
            if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : ray_error("oom", NULL); }
            out = ray_table_add_col(out, ray_table_col_name(flat, c), col);
            ray_release(col);
        }
        return out;
    }

    /* records-form */
    {
        ray_t* accs[64];
        for (int64_t c = 0; c < nc; c++) {
            accs[c] = ray_list_new(ny > 0 ? ny : 1);
            if (RAY_IS_ERR(accs[c])) {
                ray_t* e = accs[c];
                for (int64_t j = 0; j < c; j++) ray_release(accs[j]);
                return e;
            }
        }
        ray_t* err = NULL;
        for (int64_t r = 0; r < ny && !err; r++) {
            ray_t* ia = ray_i64(r);
            ray_t* rec = ray_at_fn(y, ia);
            ray_release(ia);
            if (!rec || RAY_IS_ERR(rec) ||
                !(ray_is_vec(rec) || rec->type == RAY_LIST) || ray_len(rec) != nc) {
                if (rec && RAY_IS_ERR(rec)) err = rec;
                else { if (rec) ray_release(rec); err = ray_error("length", NULL); }
                break;
            }
            for (int64_t c = 0; c < nc && !err; c++) {
                ray_t* ib = ray_i64(c);
                ray_t* cell = ray_at_fn(rec, ib);
                ray_release(ib);
                if (!cell || RAY_IS_ERR(cell)) { err = cell ? cell : ray_error("oom", NULL); break; }
                accs[c] = ray_list_append(accs[c], cell);
                ray_release(cell);
                if (RAY_IS_ERR(accs[c])) { err = accs[c]; accs[c] = NULL; }
            }
            ray_release(rec);
        }
        if (err) {
            for (int64_t c = 0; c < nc; c++)
                if (accs[c] && !RAY_IS_ERR(accs[c])) ray_release(accs[c]);
            return err;
        }
        ray_t* out = ray_table_new(nc);
        for (int64_t c = 0; c < nc; c++) {
            ray_t* cc = q_collapse_list(accs[c]);
            ray_release(accs[c]);
            if (!RAY_IS_ERR(out)) {
                if (cc && !RAY_IS_ERR(cc)) {
                    out = ray_table_add_col(out, ray_table_col_name(flat, c), cc);
                } else {
                    ray_release(out);
                    out = cc ? cc : ray_error("oom", NULL);
                    cc = NULL;
                }
            }
            if (cc && !RAY_IS_ERR(cc)) ray_release(cc);
        }
        return out;
    }
}

/* Append normalized rows to a flat table.  An EMPTY target (0 rows — e.g.
 * `([]name:();age:())`) adopts the payload columns wholesale: that is how the
 * first insert types an untyped empty schema (insert.qcmd `meta u`).  Column
 * name set is the target's either way. */
static ray_t* q_table_append(ray_t* flat, ray_t* rows) {
    int64_t nc = ray_table_ncols(flat);
    if (ray_table_nrows(flat) == 0) {
        /* untyped empty columns (RAY_LIST) adopt the payload type; a TYPED
         * 0-row column keeps kdb type-strictness. */
        if (ray_table_nrows(rows) > 0) {
            for (int64_t c = 0; c < nc; c++) {
                ray_t* oc = ray_table_get_col_idx(flat, c);
                ray_t* pc = ray_table_get_col_idx(rows, c);
                if (oc && pc && ray_is_vec(oc) && pc->type != oc->type)
                    return ray_error("type", NULL);
            }
        }
        ray_t* out = ray_table_new(nc > 0 ? nc : 1);
        for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++)
            out = ray_table_add_col(out, ray_table_col_name(flat, c),
                                    ray_table_get_col_idx(rows, c));
        return out;
    }
    /* kdb type-strictness: appending into a simple typed column requires the
     * SAME element type — `insert[`t;(`ferrari;8.22)]` into a long column is
     * 'type, never a silent float promotion.  List (nested) target columns
     * accept anything; 0-row payloads have nothing to check. */
    if (ray_table_nrows(rows) > 0) {
        for (int64_t c = 0; c < nc; c++) {
            ray_t* oc = ray_table_get_col_idx(flat, c);
            ray_t* pc = ray_table_get_col_idx(rows, c);
            if (oc && pc && ray_is_vec(oc) && pc->type != oc->type)
                return ray_error("type", NULL);
        }
    }
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
        ray_t* joined = q_env_call2("concat", ray_table_get_col_idx(flat, c),
                                              ray_table_get_col_idx(rows, c));
        if (!joined || RAY_IS_ERR(joined)) { ray_release(out); return joined ? joined : ray_error("oom", NULL); }
        out = ray_table_add_col(out, ray_table_col_name(flat, c), joined);
        ray_release(joined);
    }
    return out;
}

/* q `x insert y` / insert[x;y] — x MUST name a global (kdb insert is always
 * by reference).  Unbound name + table payload CREATES the global.  Keyed
 * target: key collision -> 'insert.  Returns inserted row indices. */
static ray_t* q_insert_wrap(ray_t* x, ray_t* y) {
    if (!x || x->type != -RAY_SYM)
        return ray_error("type", "insert: target must be a table name (symbol)");
    ray_t* g = ray_env_get(x->i64);                       /* borrowed */
    if (!g) {                                             /* create */
        if (y && (y->type == RAY_TABLE || q_is_keyed_table(y))) {
            ray_env_bind(x->i64, y);                      /* retains */
            return q_idx_range(0, q_any_nrows(y));
        }
        return ray_error("type", "insert: unbound target needs a table value");
    }
    if (!(g->type == RAY_TABLE || q_is_keyed_table(g)))
        return ray_error("type", "insert: target is not a table");
    int keyed = q_is_keyed_table(g);
    int64_t nkey = keyed ? ray_table_ncols(ray_dict_keys(g)) : 0;
    ray_t* flat = q_table_flatten(g);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    ray_t* rows = q_rows_normalize(flat, y, 0);
    if (!rows || RAY_IS_ERR(rows)) { ray_release(flat); return rows ? rows : ray_error("oom", NULL); }
    int64_t before = ray_table_nrows(flat);
    int64_t added  = ray_table_nrows(rows);
    if (keyed) {                                          /* collision -> 'insert */
        ray_t* kt = ray_dict_keys(g);                     /* borrowed */
        int64_t kn = ray_table_nrows(kt);
        for (int64_t r = 0; r < added; r++)
            for (int64_t e = 0; e < kn; e++)
                if (q_row_eq(rows, r, kt, e, nkey)) {
                    ray_release(rows); ray_release(flat);
                    return ray_error("insert", NULL);
                }
    }
    ray_t* nf = q_table_append(flat, rows);
    ray_release(flat); ray_release(rows);
    if (!nf || RAY_IS_ERR(nf)) return nf;
    ray_t* nt;
    if (keyed) { nt = q_enkey(nf, nkey); ray_release(nf); }
    else nt = nf;
    if (!nt || RAY_IS_ERR(nt)) return nt;
    ray_env_bind(x->i64, nt);                             /* retains */
    ray_release(nt);
    return q_idx_range(before, added);
}

/* Keyed-table upsert core: payload rows whose key matches an existing key
 * UPDATE the value cells in place; the rest append (ref/upsert.md).  Both
 * operands flat (key cols first); returns the new FLAT table. */
static ray_t* q_keyed_upsert_flat(ray_t* flat, int64_t nkey, ray_t* rows) {
    int64_t nc = ray_table_ncols(flat);
    int64_t n0 = ray_table_nrows(flat);
    int64_t na = ray_table_nrows(rows);
    if (nc > 64) return ray_error("limit", "upsert: too many columns");
    ray_t* colv[64];
    for (int64_t c = 0; c < nc; c++) colv[c] = NULL;
    ray_t* err = NULL;
    for (int64_t c = 0; c < nc && !err; c++) {
        colv[c] = ray_list_new(n0 + na > 0 ? n0 + na : 1);
        if (RAY_IS_ERR(colv[c])) { err = colv[c]; colv[c] = NULL; break; }
        for (int64_t r = 0; r < n0 && !err; r++) {
            ray_t* ia = ray_i64(r);
            ray_t* cell = ray_at_fn(ray_table_get_col_idx(flat, c), ia);
            ray_release(ia);
            if (!cell || RAY_IS_ERR(cell)) { err = cell ? cell : ray_error("oom", NULL); break; }
            colv[c] = ray_list_append(colv[c], cell);
            ray_release(cell);
            if (RAY_IS_ERR(colv[c])) { err = colv[c]; colv[c] = NULL; }
        }
    }
    int64_t nrows = n0;
    for (int64_t r = 0; r < na && !err; r++) {
        int64_t hit = -1;
        for (int64_t e = 0; e < nrows && hit < 0 && !err; e++) {
            int eq = 1;
            for (int64_t c = 0; c < nkey && eq && !err; c++) {
                ray_t* ia = ray_i64(r);
                ray_t* nv = ray_at_fn(ray_table_get_col_idx(rows, c), ia);
                ray_release(ia);
                if (!nv || RAY_IS_ERR(nv)) { err = nv ? nv : ray_error("oom", NULL); break; }
                ray_t** cells = (ray_t**)ray_data(colv[c]);
                eq = q_cell_eq(cells[e], nv);
                ray_release(nv);
            }
            if (!err && eq) hit = e;
        }
        for (int64_t c = 0; c < nc && !err; c++) {
            ray_t* ia = ray_i64(r);
            ray_t* nv = ray_at_fn(ray_table_get_col_idx(rows, c), ia);
            ray_release(ia);
            if (!nv || RAY_IS_ERR(nv)) { err = nv ? nv : ray_error("oom", NULL); break; }
            if (hit >= 0) {
                if (c >= nkey) {                          /* update value cells */
                    ray_t** cells = (ray_t**)ray_data(colv[c]);
                    ray_t* old = cells[hit];
                    ray_retain(nv);
                    cells[hit] = nv;
                    ray_release(old);
                }
            } else {
                colv[c] = ray_list_append(colv[c], nv);
                if (RAY_IS_ERR(colv[c])) { err = colv[c]; colv[c] = NULL; }
            }
            ray_release(nv);
        }
        if (hit < 0 && !err) nrows++;
    }
    if (err) {
        for (int64_t c = 0; c < nc; c++)
            if (colv[c] && !RAY_IS_ERR(colv[c])) ray_release(colv[c]);
        return err;
    }
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* cc = q_collapse_list(colv[c]);
        ray_release(colv[c]);
        if (!RAY_IS_ERR(out)) {
            if (cc && !RAY_IS_ERR(cc)) {
                out = ray_table_add_col(out, ray_table_col_name(flat, c), cc);
            } else {
                ray_release(out);
                out = cc ? cc : ray_error("oom", NULL);
                cc = NULL;
            }
        }
        if (cc && !RAY_IS_ERR(cc)) ray_release(cc);
    }
    return out;
}

/* q `x upsert y` — plain table appends; keyed table updates-or-appends by
 * key.  Value target returns the new table; a NAMED target rebinds the
 * global and returns the name (unbound name + table payload creates it). */
static ray_t* q_upsert_wrap(ray_t* x, ray_t* y) {
    int64_t sym;
    ray_t* t = q_table_operand(x, &sym);
    if (!t) {
        if (x && x->type == -RAY_SYM && !ray_env_get(x->i64) &&
            y && (y->type == RAY_TABLE || q_is_keyed_table(y))) {
            ray_env_bind(x->i64, y);                      /* create, like insert */
            ray_retain(x);
            return x;
        }
        return ray_error("type", "upsert: expects a table or table name");
    }
    int keyed = q_is_keyed_table(t);
    int64_t nkey = keyed ? ray_table_ncols(ray_dict_keys(t)) : 0;
    ray_t* flat = q_table_flatten(t);
    if (!flat || RAY_IS_ERR(flat)) return flat;
    ray_t* rows = q_rows_normalize(flat, y, 1);
    if (!rows || RAY_IS_ERR(rows)) { ray_release(flat); return rows ? rows : ray_error("oom", NULL); }
    ray_t* nf = keyed ? q_keyed_upsert_flat(flat, nkey, rows)
                      : q_table_append(flat, rows);
    ray_release(flat); ray_release(rows);
    if (!nf || RAY_IS_ERR(nf)) return nf;
    ray_t* nt;
    if (keyed) { nt = q_enkey(nf, nkey); ray_release(nf); }
    else nt = nf;
    if (!nt || RAY_IS_ERR(nt)) return nt;
    if (sym >= 0) {
        ray_env_bind(sym, nt);                            /* retains */
        ray_release(nt);
        ray_retain(x);
        return x;
    }
    return nt;
}

/* q `x,y` join — table , record-dict appends the record (ref/join.md +
 * ref/upsert.md: a simple table's Join of a matching record is the same
 * append upsert performs); EVERY other operand pair delegates to base concat
 * (register_binary("concat") == ray_concat_fn) byte-identically — dict,dict
 * upsert-union and table,table row-join already live there. */
static ray_t* q_join_wrap(ray_t* x, ray_t* y) {
    if (x && x->type == RAY_TABLE && y && y->type == RAY_DICT && !q_is_keyed_table(y))
        return q_upsert_wrap(x, y);
    /* A bare dict joins ONLY with a dict (ref/join.md: `10,d` -> 'type; base
     * concat would wrongly DISTRIBUTE the scalar over the dict's values). */
    {
        int xd = x && x->type == RAY_DICT && !q_is_keyed_table(x);
        int yd = y && y->type == RAY_DICT && !q_is_keyed_table(y);
        if (xd != yd)
            return ray_error("type", ",: cannot join a dictionary with a non-dictionary");
    }
    return ray_concat_fn(x, y);
}

/* ---- table set-ops core (distinct/union/except/inter arms) --------------- */

/* Indices of x-rows [not] present in y (whole-row membership). */
static ray_t* q_table_member_idx(ray_t* x, ray_t* y, int keep_present) {
    int64_t nrx = ray_table_nrows(x), nry = ray_table_nrows(y);
    int64_t ncx = ray_table_ncols(x);
    if (ncx != ray_table_ncols(y)) return ray_error("mismatch", NULL);
    ray_t* idx = ray_vec_new(RAY_I64, nrx > 0 ? nrx : 1);
    if (RAY_IS_ERR(idx)) return idx;
    idx->len = 0;
    for (int64_t r = 0; r < nrx; r++) {
        int found = 0;
        for (int64_t e = 0; e < nry && !found; e++)
            found = q_row_eq(x, r, y, e, ncx);
        if (found == keep_present) {
            idx = ray_vec_append(idx, &r);
            if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("oom", NULL);
        }
    }
    return idx;
}

/* Gather table rows by index vector (columns via ray_at_fn + collapse). */
static ray_t* q_table_gather(ray_t* t, ray_t* idx) {
    int64_t nc = ray_table_ncols(t);
    ray_t* out = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
        ray_t* col = ray_at_fn(ray_table_get_col_idx(t, c), idx);
        if (col && col->type == RAY_LIST) {
            ray_t* cc = q_collapse_list(col);
            ray_release(col);
            col = cc;
        }
        if (!col || RAY_IS_ERR(col)) { ray_release(out); return col ? col : ray_error("oom", NULL); }
        out = ray_table_add_col(out, ray_table_col_name(t, c), col);
        ray_release(col);
    }
    return out;
}

/* q `distinct t` — FIRST-OCCURRENCE row dedup.  The base DAG table-distinct
 * (ray_table_distinct_fn) sorts, so it is NOT reused — the same reason the q
 * vector distinct is a wrapper. */
static ray_t* q_table_distinct(ray_t* t) {
    int64_t nr = ray_table_nrows(t);
    int64_t nc = ray_table_ncols(t);
    ray_t* idx = ray_vec_new(RAY_I64, nr > 0 ? nr : 1);
    if (RAY_IS_ERR(idx)) return idx;
    idx->len = 0;
    for (int64_t r = 0; r < nr; r++) {
        int dup = 0;
        int64_t* kept = (int64_t*)ray_data(idx);
        for (int64_t j = 0; j < idx->len && !dup; j++)
            dup = q_row_eq(t, r, t, kept[j], nc);
        if (!dup) {
            idx = ray_vec_append(idx, &r);
            if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("oom", NULL);
        }
    }
    ray_t* out = q_table_gather(t, idx);
    ray_release(idx);
    return out;
}

/* q `x except y` — table pair: rows of x not in y (x order and duplicates
 * kept, then per kdb the RESULT is over distinct rows of x — ref/except.md
 * operates on items; for tables kdb dedups via distinct semantics of the
 * underlying find, so keep it simple: rows of x not in y, x-dups kept).
 * Non-table operands delegate to base ray_except_fn (pre-wave behaviour). */
static ray_t* q_except_wrap(ray_t* x, ray_t* y) {
    /* keyed tables / dicts are deferred cells — the base list kernel would
     * mangle the dict structure (mirror of the inter guard). */
    if ((x && x->type == RAY_DICT) || (y && y->type == RAY_DICT))
        return ray_error("nyi", "except: dict/keyed-table operands deferred");
    if (x && x->type == RAY_TABLE && y && y->type == RAY_TABLE) {
        ray_t* idx = q_table_member_idx(x, y, 0);
        if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("oom", NULL);
        ray_t* r = q_table_gather(x, idx);
        ray_release(idx);
        return r;
    }
    return ray_except_fn(x, y);
}

/* q `x!y` — dict make.  An atom key enlists to a 1-vector first (kdb `a!1`
 * is a dict too; rayfall dict wants vector keys); vals pass through as-is
 * (rayfall dict broadcasts atom vals and boxes the rest itself).  kdb
 * 'length fidelity: rayfall would null-fill a short vals side, so the
 * count check lives here.  String keys are a deferred cell (string model). */
static ray_t* q_bang_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "!: nil operand");
    /* wire pass 3: null operands are reachable (q_fn_null_ok blesses `!` at
     * the eval null gate; call_fn2 was never gated).  A generic-null VALUE
     * serializes (`-8!(::)` -> 101h in the internal-fn branch below); every
     * other null shape 'types exactly like the historic gate. */
    if (RAY_IS_NULL(x)) return ray_error("type", "!: null key operand");
    /* kdb `0N!x` — debug print: write x's single-line k-repr to the console
     * sink and pass x through unchanged (ref/display.md; the file-text.md KV
     * examples pin the repr).  A NULL integer atom lhs can never be an
     * internal-fn id (negative) or an enkey count (>= 0), so the intercept
     * is exact. */
    if ((x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16) &&
        RAY_ATOM_IS_NULL(x)) {
        q_console_show_krepr(y);
        ray_retain(y);
        return y;
    }
    /* kdb reserves a NEGATIVE integer ATOM lhs for internal functions
     * (`-8!x` serialize, `-9!x` deserialize, ...) — never dict-make.
     * Typed nulls fall through to dict-make (0N is not an internal id). */
    if ((x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16) &&
        !RAY_ATOM_IS_NULL(x)) {
        int64_t id = x->type == -RAY_I64 ? x->i64
                   : x->type == -RAY_I32 ? (int64_t)x->i32 : (int64_t)x->i16;
        if (id < 0) {
            switch (id) {
            case -8: return q_wire_serialize(y, Q_WIRE_ASYNC);
            case -9:
                if (y->type != RAY_U8)
                    return ray_error("type", "-9!: expects a byte vector");
                return q_wire_deserialize(y);
            default:
                return ray_error("nyi", "internal function %lld! not yet implemented", (long long)id);
            }
        }
        /* q enkey/unkey: `N!table` / `N!keyedtable` (N>=0). */
        if (y->type == RAY_TABLE || q_is_keyed_table(y))
            return q_enkey(y, id);
        /* by-reference `N!`name`: unkey/enkey the named global IN PLACE, then
         * return the name (kdb amend-by-reference).  A miss / non-table stays
         * dict-make below. */
        if (y->type == -RAY_SYM) {
            ray_t* g = ray_env_get(y->i64);
            if (g && (g->type == RAY_TABLE || q_is_keyed_table(g))) {
                ray_t* nt = q_enkey(g, id);
                if (!nt || RAY_IS_ERR(nt)) return nt;
                ray_env_bind(y->i64, nt);      /* retains */
                ray_release(nt);
                ray_retain(y);
                return y;
            }
        }
    }
    /* wire pass 3: a generic-null y only makes sense in the internal-fn
     * band handled above — dict-make/enkey with (::) vals keeps the
     * historic 'type. */
    if (RAY_IS_NULL(y)) return ray_error("type", "!: null value operand");
    /* table!table — a keyed table IS a dict from key records to value records
     * (dict.qcmd `([]k..)!([]v..)`); row counts must match ('length, kdb). */
    if (x->type == RAY_TABLE && y->type == RAY_TABLE) {
        if (ray_table_nrows(x) != ray_table_nrows(y))
            return ray_error("length", "!: key and value row counts must match");
        ray_retain(x);
        ray_retain(y);
        return ray_dict_new(x, y);               /* consumes both retains */
    }
    ray_t* keys = x;
    ray_t* keys_owned = NULL;
    if (ray_is_atom(x)) {
        ray_t* l = ray_list_new(1);
        l = ray_list_append(l, x);
        if (RAY_IS_ERR(l)) return l;
        keys_owned = q_collapse_list(l);
        ray_release(l);
        if (!keys_owned || RAY_IS_ERR(keys_owned))
            return keys_owned ? keys_owned : ray_error("type", NULL);
        if (!ray_is_vec(keys_owned)) {          /* -RAY_STR & friends stay boxed */
            ray_release(keys_owned);
            return ray_error("type", "!: unsupported key type (deferred)");
        }
        keys = keys_owned;
    }
    if ((ray_is_vec(y) || y->type == RAY_LIST) &&
        (ray_is_vec(keys) || keys->type == RAY_LIST) &&
        ray_len(keys) != ray_len(y)) {
        if (keys_owned) ray_release(keys_owned);
        return ray_error("length", "!: key and value counts must match");
    }
    ray_t* r = q_env_call2("dict", keys, y);
    if (keys_owned) ray_release(keys_owned);
    return r;
}

/* wire pass 3: registry-blessed null-tolerant dyadics.  ray_eval's binary
 * null gate (tree-walk + VM op_call2) offers a RAY_NULL_OBJ-operand
 * application to the apply hook before raising 'type; q_apply_noun consults
 * this to decide whether the head may run (`-8!(::)` serialize via the `!`
 * internal-fn band, `x~(::)` match).  Everything else declines -> the
 * historic 'type stands.  Fn-pointer identity keeps this single-homed with
 * the wrappers themselves (registry values are immutable snapshots). */
int q_fn_null_ok(const ray_t* fn) {
    if (!fn || fn->type != RAY_BINARY) return 0;
    ray_binary_fn p = (ray_binary_fn)(uintptr_t)fn->i64;
    return p == q_bang_wrap || p == q_match_wrap;
}

/* The join wrapper owns the MIXED bare-dict shape ('type, ref/join.md
 * `10,d`); dict,dict (incl. keyed-table) pairs keep the shim's union path. */
int q_fn_dict_distribute_veto(const ray_t* fn, ray_t** args, int64_t n) {
    if (!fn || fn->type != RAY_BINARY || n != 2) return 0;
    if ((ray_binary_fn)(uintptr_t)fn->i64 != q_join_wrap) return 0;
    int xd = args[0] && args[0]->type == RAY_DICT;
    int yd = args[1] && args[1]->type == RAY_DICT;
    return xd != yd;                 /* exactly one dict side -> wrapper's 'type stands */
}

static ray_t* q_value_resolve_sym_owned(ray_t* symv);   /* fwd (below) */

/* q `key x` (ref/key.md) — dict keys, plus the name/namespace overloads:
 *   `` ` ``      -> root context roster (namespaces other than .z)
 *   `` `. ``     -> objects in the root (user variable names)
 *   `` `.foo ``  -> the context's keys (leading `` ` `` placeholder + members)
 *   `` `name ``  -> keys of the named dict; the sym itself if the name is
 *                   bound to a non-dict; `()` if unbound (context-aware)
 * File handles (`` `:path ``) are the file-I/O wave: 'nyi.  Everything else
 * non-dict stays a deferred 'type cell. */
static const char* q_type_qname(int8_t t);          /* fwd (cast map, below) */

static ray_t* q_key_wrap(ray_t* x) {
    /* type of a vector (ref/key.md): `key 0#5` -> `long; a native string
     * atom IS the provisional char vector -> `char; `key 10` -> til 10. */
    if (x && ray_is_vec(x)) {
        const char* nm = q_type_qname(x->type);
        if (nm) return ray_sym(ray_sym_intern_runtime(nm, strlen(nm)));
        /* unnamed vector types keep the deferred 'type tail below */
    }
    if (x && x->type == -RAY_STR)
        return ray_sym(ray_sym_intern_runtime("char", 4));
    if (x && (x->type == -RAY_I64 || x->type == -RAY_I32 || x->type == -RAY_I16) &&
        !RAY_ATOM_IS_NULL(x)) {
        int64_t v = (x->type == -RAY_I64) ? x->i64
                  : (x->type == -RAY_I32) ? (int64_t)x->i32 : (int64_t)x->i16;
        if (v >= 0) return q_til_wrap(x);           /* key n == til n */
    }
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        if (!s) return ray_error("type", "key: bad symbol");
        const char* nm = ray_str_ptr(s);
        size_t l = ray_str_len(s);
        if (l == 0) { ray_release(s); return q_ns_key_roster(); }
        if (nm[0] == ':') {
            ray_release(s);
            return ray_error("nyi", "key: file handles deferred (file-I/O wave)");
        }
        if (l == 1 && nm[0] == '.') {           /* `. — root objects */
            ray_release(s);
            ray_t* d = q_ns_root_dict();
            if (!d || RAY_IS_ERR(d)) return d ? d : ray_error("oom", NULL);
            ray_t* k = ray_dict_keys(d);        /* borrowed from owned d */
            if (!k) { ray_release(d); return ray_error("type", "key: nil keys"); }
            ray_retain(k);
            ray_release(d);
            return k;
        }
        if (l >= 2 && nm[0] == '.' && nm[1] != '.') {
            /* root OR nested handle (codex round-3 P2): q_ns_ctx_dict walks
             * the dotted path and decides context-ness itself. */
            ray_t* d = q_ns_ctx_dict(nm, l);    /* owned, placeholder first */
            if (d) {
                ray_release(s);
                if (RAY_IS_ERR(d)) return d;
                ray_t* k = ray_dict_keys(d);
                if (!k) { ray_release(d); return ray_error("type", "key: nil keys"); }
                ray_retain(k);
                ray_release(d);
                return k;
            }
            /* not a context — fall through to the named-variable arm */
        }
        ray_release(s);
        /* named variable (context-aware): dict -> keys; bound -> the sym
         * itself; unbound -> () (ref/key.md "whether a name exists"). */
        ray_t* v = q_value_resolve_sym_owned(x);
        if (!v) return ray_list_new(1);         /* () — empty general list */
        if (RAY_IS_ERR(v)) return v;
        if (v->type == RAY_DICT) {
            ray_t* k = ray_dict_keys(v);
            if (!k) { ray_release(v); return ray_error("type", "key: nil keys"); }
            ray_retain(k);
            ray_release(v);
            return k;
        }
        ray_release(v);
        ray_retain(x);
        return x;
    }
    if (!x || x->type != RAY_DICT)
        return ray_error("type", "key: expects a dict (other forms deferred)");
    ray_t* k = ray_dict_keys(x);                /* borrowed */
    if (!k) return ray_error("type", "key: nil keys");
    ray_retain(k);
    return k;
}

/* q `nam set y` (ref/get.md) — assign a global through a symbol handle:
 *   `a set 42        -> bind the global (dotted names create contexts)
 *   `.foo set d      -> restore a context: upsert every member of dict d
 *                       (the empty-sym :: placeholder entry is skipped)
 *   `. set d         -> restore root variables from dict d
 *   `.foo set 42     -> plain rebind: WIPES the context (q4m3's gotcha)
 * File handles (`:path) and the compressed/splay list forms are the
 * file-I/O wave: 'nyi.  Returns the handle (kdb returns nam). */
static ray_t* q_setg_wrap(ray_t* x, ray_t* y) {
    if (!x || x->type != -RAY_SYM)
        return ray_error("nyi", "set: only symbol handles (file forms deferred)");
    ray_t* s = ray_sym_str(x->i64);
    if (!s) return ray_error("type", "set: bad symbol");
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    if (l == 0) {
        ray_release(s);
        return ray_error("type", "set: empty name");
    }
    if (nm[0] == ':') {
        ray_release(s);
        return ray_error("nyi", "set: file handles deferred (file-I/O wave)");
    }
    int is_root = (l == 1 && nm[0] == '.');
    /* Restore semantics: any single-segment `.foo` handle (kdb creates the
     * context), and a NESTED handle only when it ALREADY names a context
     * (codex round-3 P2) — so `.foo.a set 1 2!3 4` keeps binding the data
     * dict instead of erroring on non-symbol keys. */
    int is_ctx  = (!is_root && l >= 2 && nm[0] == '.' && nm[1] != '.' &&
                   (!memchr(nm + 1, '.', l - 1) || q_ns_is_context(nm, l)));
    if ((is_root || is_ctx) && y && y->type == RAY_DICT) {
        /* context restore: upsert each member under the target root */
        ray_t* dk = ray_dict_keys(y);           /* borrowed */
        ray_t* dv = ray_dict_vals(y);           /* borrowed */
        int64_t n = ray_dict_len(y);
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* k = ray_at_fn(dk, ia);       /* owned */
            ray_t* v = ray_at_fn(dv, ia);       /* owned */
            ray_release(ia);
            if (!k || RAY_IS_ERR(k) || k->type != -RAY_SYM || !v || RAY_IS_ERR(v)) {
                if (k && !RAY_IS_ERR(k)) ray_release(k);
                if (v && !RAY_IS_ERR(v)) ray_release(v);
                ray_release(s);
                return ray_error("type", "set: context dict needs symbol keys");
            }
            ray_t* ks = ray_sym_str(k->i64);
            if (!ks || ray_str_len(ks) == 0) {  /* :: placeholder — skip */
                if (ks) ray_release(ks);
                ray_release(k);
                ray_release(v);
                continue;
            }
            char full[192];
            full[0] = '\0';                   /* error paths print `full` */
            int fl = is_root
                ? snprintf(full, sizeof full, "%.*s",
                           (int)ray_str_len(ks), ray_str_ptr(ks))
                : snprintf(full, sizeof full, "%.*s.%.*s", (int)l, nm,
                           (int)ray_str_len(ks), ray_str_ptr(ks));
            ray_release(ks);
            ray_release(k);
            ray_err_t err = (fl > 0 && (size_t)fl < sizeof full)
                ? ray_env_set(ray_sym_intern(full, (size_t)fl), v)
                : RAY_ERR_TYPE;
            ray_release(v);
            if (err == RAY_ERR_RESERVED) {
                ray_release(s);
                return ray_error("reserve", "set: '%s' is reserved", full);
            }
            if (err != RAY_OK) {
                ray_release(s);
                return ray_error(ray_err_code_str(err), "set: '%s' failed", full);
            }
        }
        ray_release(s);
        ray_retain(x);
        return x;
    }
    if (is_root) {                              /* `. set non-dict: no reading */
        ray_release(s);
        return ray_error("type", "set: root handle takes a dictionary");
    }
    ray_release(s);
    ray_err_t err = ray_env_set(x->i64, y);     /* plain/dotted global assign */
    if (err == RAY_ERR_RESERVED)
        return ray_error("reserve", "set: name is reserved");
    if (err != RAY_OK)
        return ray_error(ray_err_code_str(err), "set: assign failed");
    ray_retain(x);
    return x;
}

/* ===== q `value` (ref/value.md) — the full form matrix =====================
 * value is monadic; its ONE argument selects the behaviour by shape:
 *   dict            -> vals (collapsed)
 *   symbol atom     -> value of the variable it names (env / .q namespace / kw)
 *   enumeration     -> [deferred: no enum type]
 *   string          -> parse+lower+eval in the current context
 *   list            -> SINGLE apply/index of the head over the rest (a value
 *                      OBJECT apply — NOT eval's recursive parse-tree walk; if the
 *                      head is a symbol/string it is resolved first)
 *   projection      -> (function; bound args…)
 *   derived function-> argument of the outer iterator
 *   operator        -> internal opcode integer
 *   lambda          -> the V3.5 structure list
 * eval-vs-value: `value(f;args)` is ONE apply of f to the args-as-data; `eval`
 * would descend recursively.  They coincide only for flat args (ARCHITECTURE.md).
 */

static ray_t* q_call_n(ray_t* f, ray_t** a, int64_t k);   /* fwd; defined ~2726 */

/* Parse+lower+eval a q source string to a value (runtime-only; the registry is
 * warm here — rule 6 forbids q_parse in BUILDERS, not runtime wrappers, per the
 * q_select_exec precedent).  Returns owned value or RAY_ERROR. */
static ray_t* q_value_eval_str(ray_t* strv) {
    const char* sp = ray_str_ptr(strv);
    size_t sl = ray_str_len(strv);
    char* s = malloc(sl + 1);
    if (!s) return ray_error("wsfull", "value: out of memory");
    memcpy(s, sp, sl);
    s[sl] = '\0';
    ray_t* ast = q_parse(s);
    free(s);
    if (!ast || RAY_IS_ERR(ast)) return ast ? ast : ray_error("parse", NULL);
    ast = q_lower(ast);
    if (!ast || RAY_IS_ERR(ast)) return ast ? ast : ray_error("parse", NULL);
    ray_t* r = ray_eval(ast);
    ray_release(ast);
    return r;
}

/* Resolve a q symbol atom to its value (ref/value.md: "symbol atom -> value of
 * the variable it names").  Order: env (user vars + builtins) for the full name;
 * else strip a leading `.q.` and retry env; else the registry keyword table
 * (monadic then dyadic).  Returns a BORROWED ref (env/registry own it) or NULL. */
static ray_t* q_value_resolve_sym(ray_t* symv) {
    if (!symv || symv->type != -RAY_SYM) return NULL;
    ray_t* s = ray_sym_str(symv->i64);
    if (!s) return NULL;
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    ray_t* r = ray_env_get(symv->i64);            /* borrowed; full name */
    const char* base = nm; size_t bl = l;
    if (!r && l > 3 && nm[0] == '.' && nm[1] == 'q' && nm[2] == '.') {
        base = nm + 3; bl = l - 3;
        int64_t bid = ray_sym_intern(base, bl);
        r = ray_env_get(bid);                     /* borrowed */
    }
    if (!r) {
        r = q_registry_lookup_name(base, bl, Q_MONADIC);   /* borrowed */
        if (!r) r = q_registry_lookup_name(base, bl, Q_DYADIC);
    }
    ray_release(s);
    return r;                                     /* borrowed */
}

/* Owned-resolution variant for `get`/`value`/`key` sym arms — the q-namespace
 * views SYNTHESIZE fresh dicts, so this returns OWNED refs on every path (the
 * borrowed q_value_resolve_sym contract stays untouched; review-1 decision):
 *   `.        -> root context dict (user vars, no :: placeholder)
 *   `.foo     -> context dict with the leading :: placeholder
 *   `..name   -> the ROOT variable `name` (k-style root qualification)
 *   `name     -> under `\d .ctx`, `.ctx.name` first (kdb: unqualified names
 *                are context-relative; reserved words stay reserved), else
 *                the plain env/registry resolution.
 * Returns NULL when unresolved. */
static ray_t* q_value_resolve_sym_owned(ray_t* symv) {
    if (!symv || symv->type != -RAY_SYM) return NULL;
    ray_t* s = ray_sym_str(symv->i64);
    if (!s) return NULL;
    const char* nm = ray_str_ptr(s);
    size_t l = ray_str_len(s);
    if (l == 1 && nm[0] == '.') {                 /* `. — root view */
        ray_release(s);
        return q_ns_root_dict();
    }
    if (l > 2 && nm[0] == '.' && nm[1] == '.') {  /* `..name — root variable */
        int64_t rid = ray_sym_intern(nm + 2, l - 2);
        ray_release(s);
        ray_t* v = ray_env_get(rid);
        if (!v) return NULL;
        ray_retain(v);
        return v;
    }
    if (l >= 2 && nm[0] == '.' && nm[1] != '.') {
        /* root OR nested context handle (codex round-3 P2) */
        ray_t* d = q_ns_ctx_dict(nm, l);          /* owned or NULL */
        if (d) { ray_release(s); return d; }
        /* not a context — fall through (member reads like `.jab.wrong,
         * flat dotted bindings like `.z.f, data dicts like `.foo.a) */
    }
    /* under \d .ctx an unqualified user name is context-relative */
    const char* ctx = q_ns_current();
    if (ctx[0] && l > 0 && nm[0] != '.' && !q_ns_is_unqualifiable(nm, l)) {
        char full[192];
        int fl = snprintf(full, sizeof full, "%s.%.*s", ctx, (int)l, nm);
        ray_release(s);
        if (fl <= 0 || (size_t)fl >= sizeof full) return NULL;
        ray_t* v = ray_env_get(ray_sym_intern(full, (size_t)fl));
        if (!v) return NULL;
        ray_retain(v);
        return v;
    }
    ray_release(s);
    ray_t* v = q_value_resolve_sym(symv);         /* borrowed */
    if (!v) return NULL;
    ray_retain(v);
    return v;                                     /* owned from here on */
}

/* Public alias — see q_registry.h (q_apply's symbol-handle arm). */
ray_t* q_value_resolve_owned(ray_t* symv) {
    return q_value_resolve_sym_owned(symv);
}

/* Single-apply a resolved head VALUE to argc data args (borrowed).  Value-object
 * apply: args are DATA — q_call_n routes callables (call_fn1/2, carrier/lambda-
 * aware) and INDEXES a noun head, never re-evaluating the args.  argc==0 => the
 * head itself (retained).  Returns owned result or RAY_ERROR. */
static ray_t* q_value_apply_head(ray_t* head, ray_t** args, int64_t argc) {
    if (!head) return ray_error("type", "value: nil head");
    if (argc == 0) { ray_retain(head); return head; }
    if (argc > 64) return ray_error("limit", "value: too many args");
    return q_call_n(head, args, argc);            /* borrowed args, owned result */
}

/* True iff `base` is one of the six adverb HOF values a q derived function
 * projects over (over/scan/each/prior/map-right/map-left).  A Q_DERIV_PROJ over
 * one of these is a DERIVED FUNCTION (value -> the iterator's argument); over
 * anything else it is a plain PROJECTION (value -> function + bound args).  The
 * env HOF ids are interned once (lazy), mirroring q_deriv's marker cache. */
static int q_value_is_adverb_hof(ray_t* base) {
    if (!base) return 0;
    /* Re-intern the env HOF ids per call: the sym table is recreated per runtime
     * (q_runtime_destroy tears it down), so a `static` id cache would go STALE for
     * a new runtime in the same process (the C unit suites do exactly that).
     * `value` is not hot, and ray_sym_intern is a cheap hash lookup when present. */
    return base == q_registry_over_value()
        || base == ray_env_get(ray_sym_intern("fold", 4))
        || base == q_registry_scan_value()
        || base == q_registry_lookup_name("each", 4, Q_DYADIC)
        || base == q_registry_prior_value()
        || base == ray_env_get(ray_sym_intern("map-right", 9))
        || base == ray_env_get(ray_sym_intern("map-left", 8));
}

ray_t* q_value_wrap(ray_t* x) {
    if (!x) return ray_error("type", "value: nil");

    /* ---- carriers (all RAY_LIST — matched BEFORE the general-list arm) ---- */
    q_deriv_kind dk = q_deriv_kind_of(x);

    /* lambda carrier -> the kdb lambda-structure list (ref/value.md#lambda,
     * V3.5 shape, best-effort fields):
     *   [0] bytecode (empty 0x)  [1] params  [2] locals  [3] globals
     *   [4] source map -1  [5] name ""  [6] file ""  [7] line -1  [8] source
     * The doc-pinned fields are [1] (params) and LAST (source text). */
    if (dk == Q_DERIV_LAMBDA) {
        ray_t* lam = q_deriv_base(x);                 /* borrowed */
        ray_t* src = q_lambda_src(x);                 /* borrowed */
        if (!lam || !src) return ray_error("type", "value: malformed lambda");
        ray_t* l = ray_list_new(9);
        ray_t* t;
        t = ray_vec_new(RAY_U8, 0);           l = ray_list_append(l, t); ray_release(t);
        l = ray_list_append(l, LAMBDA_PARAMS(lam));   /* borrowed; append retains */
        t = ray_sym_vec_new(RAY_SYM_W64, 0);  l = ray_list_append(l, t); ray_release(t);
        t = ray_sym_vec_new(RAY_SYM_W64, 0);  l = ray_list_append(l, t); ray_release(t);
        t = ray_i64(-1);                      l = ray_list_append(l, t); ray_release(t);
        t = ray_str("", 0);                   l = ray_list_append(l, t); ray_release(t);
        t = ray_str("", 0);                   l = ray_list_append(l, t); ray_release(t);
        t = ray_i64(-1);                      l = ray_list_append(l, t); ray_release(t);
        l = ray_list_append(l, src);
        return l;
    }

    if (dk == Q_DERIV_PROJ) {
        ray_t* base   = q_deriv_base(x);              /* borrowed, idx 1 */
        int64_t n     = ray_len(x);
        ray_t** e     = (ray_t**)ray_data(x);
        if (q_value_is_adverb_hof(base)) {
            /* derived function -> the argument of the iterator (idx 4).  The
             * adverb-derived carrier always binds EXACTLY the inner value there
             * (q_lower builds it as `(hof; V, hole)`); assert the invariant so a
             * future multi-arg adverb projection can't silently drop args. */
            uint64_t mask = q_deriv_hole_mask(x);
            int64_t bound = 0;
            for (int64_t i = 0; i < n - 4; i++) if (!(mask & (1ull << i))) bound++;
            if (n < 5 || bound != 1 || !e[4])
                return ray_error("type", "value: unexpected derived-fn shape");
            ray_retain(e[4]);
            return e[4];
        }
        /* plain projection -> (base; bound-args…), holes omitted (ref/value.md:
         * "list: function followed by argument/s"). */
        uint64_t mask = q_deriv_hole_mask(x);
        int64_t slots = n - 4;
        ray_t* out = ray_list_new(slots + 1);
        out = ray_list_append(out, base);             /* borrowed; append retains */
        for (int64_t i = 0; i < slots; i++)
            if (!(mask & (1ull << i))) out = ray_list_append(out, e[4 + i]);
        return out;
    }

    if (dk == Q_DERIV_MONAD || dk == Q_DERIV_COMPOSE)
        return ray_error("nyi", "value: composition/monadic-mark deferred");

    /* ---- symbol atom -> variable/keyword/namespace value ---- */
    if (x->type == -RAY_SYM) {
        ray_t* v = q_value_resolve_sym_owned(x);      /* OWNED (or NULL) */
        if (!v) return ray_error("name", "value: unresolved symbol");
        return v;
    }

    /* ---- string atom -> evaluate as q source ---- */
    if (x->type == -RAY_STR) return q_value_eval_str(x);

    /* ---- general list -> single apply/index of the head over the rest ---- */
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (n < 1) return ray_error("type", "value: empty list");
        ray_t* head = e[0];                           /* borrowed */
        if (head && head->type == -RAY_SYM) {         /* form 2: symbol head */
            ray_t* hv = q_value_resolve_sym_owned(head);  /* OWNED (or NULL) */
            if (!hv) return ray_error("name", "value: unresolved symbol head");
            ray_t* r = q_value_apply_head(hv, e + 1, n - 1);
            ray_release(hv);
            return r;
        }
        if (head && head->type == -RAY_STR) {         /* form 3: string head */
            ray_t* hv = q_value_eval_str(head);       /* owned */
            if (!hv || RAY_IS_ERR(hv)) return hv ? hv : ray_error("parse", NULL);
            ray_t* r = q_value_apply_head(hv, e + 1, n - 1);
            ray_release(hv);
            return r;
        }
        return q_value_apply_head(head, e + 1, n - 1);  /* form 1: value head */
    }

    /* ---- dict -> vals (collapsed) ---- */
    if (x->type == RAY_DICT) {
        ray_t* v = ray_dict_vals(x);                /* borrowed */
        if (!v) return ray_error("type", "value: nil vals");
        if (v->type == RAY_LIST) return q_collapse_list(v);   /* returns owned */
        ray_retain(v);
        return v;
    }

    return ray_error("type", "value: unsupported operand (other forms deferred)");
}

/* q `distinct x` / monadic `?` — unique items in FIRST-OCCURRENCE order
 * (kdb).  rayfall's distinct routes typed vectors through the DAG group
 * path, which SORTS — a rename would pin wrong answers, so this is a
 * match-based dedup (type-strict, nulls equal — the ~ semantics kdb's
 * distinct uses), collapsed back to a typed vector.  String operands are
 * a deferred cell (string model); atoms are kdb 'type. */
static ray_t* q_distinct_wrap(ray_t* x) {
    if (!x) return ray_error("type", "distinct: nil");
    if (x->type == RAY_TABLE) return q_table_distinct(x);   /* row dedup */
    if (x->type == -RAY_STR)
        return ray_error("nyi", "distinct: string operand deferred (string model)");
    if (!ray_is_vec(x) && x->type != RAY_LIST)
        return ray_error("type", "distinct: expects a list, got %s",
                         ray_type_name(x->type));
    int64_t n = ray_len(x);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(x, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
        int dup = 0;
        int64_t m = ray_len(out);
        ray_t** oe = (ray_t**)ray_data(out);
        for (int64_t j = 0; j < m && !dup; j++) dup = q_match_rec(oe[j], e);
        if (!dup) {
            out = ray_list_append(out, e);
            if (RAY_IS_ERR(out)) { ray_release(e); return out; }
        }
        ray_release(e);
    }
    ray_t* c = q_collapse_list(out);
    ray_release(out);
    return c;
}

/* q `x union y` — `distinct x,y` (ref/union.md).  A wrapper because rayfall's
 * ray_union_fn KEEPS x-duplicates (it only filters y against x); kdb dedups
 * the whole join in first-occurrence order.  Reuses q join (`,` == rayfall
 * concat) + the q distinct wrapper above — no new set logic.  Operands the
 * distinct wrapper defers (strings, tables) defer here too: error, never a
 * wrong answer. */
static ray_t* q_union_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "union: nil operand");
    /* keyed tables / dicts are deferred cells (mirror of the inter guard). */
    if (x->type == RAY_DICT || y->type == RAY_DICT)
        return ray_error("nyi", "union: dict/keyed-table operands deferred");
    if (x->type == RAY_TABLE && y->type == RAY_TABLE) {   /* distinct of t,u */
        ray_t* j = q_env_call2("concat", x, y);
        if (!j || RAY_IS_ERR(j)) return j ? j : ray_error("oom", NULL);
        ray_t* r = q_table_distinct(j);
        ray_release(j);
        return r;
    }
    ray_t* j = ray_concat_fn(x, y);
    if (!j || RAY_IS_ERR(j)) return j;
    ray_t* r = q_distinct_wrap(j);
    ray_release(j);
    return r;
}

/* q `x inter y` — items of x that are in y, x-duplicates and order kept
 * (ref/inter.md).  rayfall `sect` (ray_sect_fn) IS this for lists, but on
 * DICT operands it returns a wrong-shaped dict where kdb returns the common
 * VALUES as a list — so dict/table operands are guarded 'nyi (error, never a
 * wrong answer); everything else delegates to ray_sect_fn. */
static ray_t* q_inter_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "inter: nil operand");
    if (x->type == RAY_TABLE && y->type == RAY_TABLE) {   /* rows of x in y */
        ray_t* idx = q_table_member_idx(x, y, 1);
        if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("oom", NULL);
        ray_t* r = q_table_gather(x, idx);
        ray_release(idx);
        return r;
    }
    if (x->type == RAY_DICT || x->type == RAY_TABLE ||
        y->type == RAY_DICT || y->type == RAY_TABLE)
        return ray_error("nyi", "inter: dict/keyed-table operands deferred");
    return ray_sect_fn(x, y);
}

/* q `x cross y` — Cartesian product, `{raze x,/:\:y}` (ref/cross.md): for
 * each item a of x (in order), for each item b of y, the JOIN `a,b`.
 * Composes existing primitives (ray_at_fn item access + q join == rayfall
 * concat) — rayfall has no cartesian primitive.  Atom operands behave as
 * one-item lists (each-left/right over an atom).  Deferred cells ('nyi,
 * never a wrong answer): string operands (kdb iterates a string's CHARS;
 * openq strings are -RAY_STR atoms — string model) and dict/table cross
 * (kdb cross-joins tables). */
static ray_t* q_cross_wrap(ray_t* x, ray_t* y) {
    if (!x || !y) return ray_error("type", "cross: nil operand");
    if (x->type == -RAY_STR || y->type == -RAY_STR)
        return ray_error("nyi", "cross: string operands deferred (string model)");
    if (x->type == RAY_DICT || x->type == RAY_TABLE ||
        y->type == RAY_DICT || y->type == RAY_TABLE)
        return ray_error("nyi", "cross: dict/table operands deferred");
    int xl = ray_is_vec(x) || x->type == RAY_LIST;
    int yl = ray_is_vec(y) || y->type == RAY_LIST;
    int64_t nx = xl ? ray_len(x) : 1;
    int64_t ny = yl ? ray_len(y) : 1;
    ray_t* out = ray_list_new(nx * ny > 0 ? nx * ny : 1);
    for (int64_t i = 0; i < nx; i++) {
        ray_t* a;
        if (xl) { ray_t* ia = ray_i64(i); a = ray_at_fn(x, ia); ray_release(ia); }
        else    { ray_retain(x); a = x; }
        if (!a || RAY_IS_ERR(a)) { ray_release(out); return a; }
        for (int64_t j = 0; j < ny; j++) {
            ray_t* b;
            if (yl) { ray_t* ja = ray_i64(j); b = ray_at_fn(y, ja); ray_release(ja); }
            else    { ray_retain(y); b = y; }
            if (!b || RAY_IS_ERR(b)) { ray_release(a); ray_release(out); return b; }
            ray_t* p = ray_concat_fn(a, b);
            ray_release(b);
            if (!p || RAY_IS_ERR(p)) { ray_release(a); ray_release(out); return p; }
            out = ray_list_append(out, p);
            if (RAY_IS_ERR(out)) { ray_release(p); ray_release(a); return out; }
            ray_release(p);
        }
        ray_release(a);
    }
    return out;
}

/* ===== q sort / grade / bucket family (feat/q-sort-rank) ===================
 * Flat typed-vector cores reuse the rayfall primitives verbatim (ray_asc_fn,
 * ray_desc_fn, ray_iasc_fn, ray_idesc_fn, ray_xbar_fn); the q wrappers add the
 * arg-swap (xbar) and the DICT container arms.  DEFERRED (error, never a wrong
 * answer): the sorted `s#` attribute on asc/desc results (rayfall has no sorted
 * attribute bit — the VALUE is kdb-true, only the attribute display diverges),
 * the mixed-general-list-by-type-number sort, and table / keyed-table sorts. */

/* Reorder a keys-or-vals vector by a grade-index vector (owned grade), then
 * collapse the boxed result back to a typed vector.  Releases `grade`. */
static ray_t* q_reindex_collapse(ray_t* vec, ray_t* grade) {
    if (!grade || RAY_IS_ERR(grade)) return grade;
    ray_t* boxed = ray_at_fn(vec, grade);
    ray_release(grade);
    if (!boxed || RAY_IS_ERR(boxed)) return boxed;
    if (boxed->type == RAY_LIST) {
        ray_t* c = q_collapse_list(boxed);
        ray_release(boxed);
        return c;
    }
    return boxed;
}

/* Grade a dict's VALUE vector.  Dict vals are stored as a boxed RAY_LIST, so
 * collapse to a typed vector before grading (a genuinely mixed value list can't
 * collapse and ray_iasc_fn errors → the by-type-number sort is DEFERRED). */
static ray_t* q_dict_value_grade(ray_t* vals, int desc) {
    ray_t* cv = (vals && vals->type == RAY_LIST) ? q_collapse_list(vals) : NULL;
    ray_t* use = cv ? cv : vals;
    /* Empty dict (e.g. `asc 0#d`): an empty value list can't be graded by
     * ray_iasc_fn (it needs a typed vector, and q_collapse_list leaves an empty
     * RAY_LIST as-is), so return an empty long grade — reindexing then yields an
     * empty dict / key list, matching kdb (codex review). */
    if (use && (ray_is_vec(use) || use->type == RAY_LIST) && ray_len(use) == 0) {
        if (cv) ray_release(cv);
        ray_t* g = ray_vec_new(RAY_I64, 0);
        if (g && !RAY_IS_ERR(g)) g->len = 0;
        return g;
    }
    ray_t* grade = desc ? ray_idesc_fn(use) : ray_iasc_fn(use);
    if (cv) ray_release(cv);
    return grade;
}

/* q `asc`/`desc` on a DICT — sort the entries by VALUE (ascending / descending),
 * carrying the keys along (kdb ref/asc.md, ref/desc.md dictionary form).  Grades
 * the values, then reindexes both keys and vals by that grade. */
static ray_t* q_sort_dict(ray_t* d, int desc) {
    ray_t* keys = ray_dict_keys(d);      /* borrowed */
    ray_t* vals = ray_dict_vals(d);      /* borrowed */
    if (!keys || !vals) return ray_error("type", "asc/desc: malformed dict");
    ray_t* grade = q_dict_value_grade(vals, desc);
    if (!grade || RAY_IS_ERR(grade)) return grade ? grade : ray_error("oom", NULL);
    ray_retain(grade);                   /* one ref per reindex call */
    ray_t* nk = q_reindex_collapse(keys, grade);        /* releases its grade ref */
    if (!nk || RAY_IS_ERR(nk)) { ray_release(grade); return nk; }
    ray_t* nv = q_reindex_collapse(vals, grade);        /* releases the retained ref */
    if (!nv || RAY_IS_ERR(nv)) { ray_release(nk); return nv; }
    return ray_dict_new(nk, nv);         /* consumes nk, nv */
}

/* q `iasc`/`idesc` on a DICT — return the KEYS in ascending / descending VALUE
 * order (kdb ref/asc.md grade form: `iasc d` grades the values, indexes keys). */
static ray_t* q_grade_dict(ray_t* d, int desc) {
    ray_t* keys = ray_dict_keys(d);      /* borrowed */
    ray_t* vals = ray_dict_vals(d);      /* borrowed */
    if (!keys || !vals) return ray_error("type", "iasc/idesc: malformed dict");
    ray_t* grade = q_dict_value_grade(vals, desc);
    return q_reindex_collapse(keys, grade);             /* releases grade */
}

static ray_t* q_asc_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT) return q_sort_dict(x, 0);
    return ray_asc_fn(x);
}
static ray_t* q_desc_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT) return q_sort_dict(x, 1);
    return ray_desc_fn(x);
}
static ray_t* q_iasc_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT) return q_grade_dict(x, 0);
    return ray_iasc_fn(x);
}
static ray_t* q_idesc_wrap(ray_t* x) {
    if (x && x->type == RAY_DICT) return q_grade_dict(x, 1);
    return ray_idesc_fn(x);
}

/* q `width xbar list` — interval bucketing.  rayfall ray_xbar_fn is (col,
 * bucket); q spells it (bucket, col), so swap the arguments.  Everything else
 * (numeric int/float bucket, temporal cols, list zip) is handled by the base
 * kernel; dict/keyed-table/qSQL forms fall through to whatever the base does. */
static ray_t* q_xbar_wrap(ray_t* bucket, ray_t* col) {
    return ray_xbar_fn(col, bucket);
}

/* ===== `?` GENERATE arms (ref/deal.md "Generate") ===========================
 * All arms draw from libc rand() — the same stream the roll/deal/permute
 * paths use — so `\S n` (q_ns.c) re-seeds them all.  Each right-operand form
 * yields a result of y's type per the docs table.  Deal (`-n?y`) of a
 * non-integer y is defined only for null-guid y ("y must be a positive long
 * or null GUID"); the sym form additionally deals (distinct syms) per the
 * Generate table's Roll,Deal column. */

/* n?`m — n symbols of m chars each from "abcdefghijklmnop"; m is the numeric
 * symbol's NAME (`2 -> 2), 1<=m<=8 -> 'length otherwise (unpinned error
 * class; chosen to mirror the docs' n<=8 bound).  distinct (deal) draws by
 * generate-and-retry with a linear scan over accepted ids — fine at the
 * 16^m<=4.3e9 space for practical n; n>16^m is 'length. */
static ray_t* q_gen_syms(int64_t n, ray_t* ysym, int distinct) {
    static const char letters[] = "abcdefghijklmnop";
    ray_t* nm = ray_sym_str(ysym->i64);
    if (!nm || RAY_IS_ERR(nm))
        return nm ? nm : ray_error("type", "?: malformed symbol operand");
    const char* s = ray_str_ptr(nm);
    size_t sl = ray_str_len(nm);
    int64_t m = 0;
    int numeric = sl > 0 && sl <= 2;
    for (size_t i = 0; numeric && i < sl; i++) {
        if (s[i] < '0' || s[i] > '9') numeric = 0;
        else m = m * 10 + (s[i] - '0');
    }
    ray_release(nm);
    if (!numeric)
        return ray_error("type", "?: sym generate needs a numeric symbol (`1..`8)");
    if (m < 1 || m > 8)
        return ray_error("length", "?: sym generate length must be 1-8");
    if (distinct) {
        int64_t space = 1;
        for (int64_t j = 0; j < m; j++) space *= 16;
        if (n > space)
            return ray_error("length", "?: cannot deal %lld distinct %lld-char syms",
                             (long long)n, (long long)m);
    }
    ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    for (int64_t i = 0; i < n; i++) {
        char buf[8];
        int64_t id;
        for (;;) {
            for (int64_t j = 0; j < m; j++) buf[j] = letters[rand() % 16];
            id = ray_sym_intern(buf, (size_t)m);
            if (!distinct) break;
            const int64_t* got = (const int64_t*)ray_data(out);
            int dup = 0;
            for (int64_t k = 0; k < i; k++) if (got[k] == id) { dup = 1; break; }
            if (!dup) break;
        }
        out = ray_vec_append(out, &id);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    }
    return out;
}

/* n?f — uniform floats in [0,y); result is y's type (F64 float / F32 real).
 * 62 random bits give the fraction; a rounding hit at the top is clamped
 * back below y so the [0,y) contract holds exactly. */
static ray_t* q_gen_floats(int64_t n, ray_t* y) {
    double fy = y->f64;                     /* F32 atoms reuse the f64 slot */
    int f32 = (y->type == -RAY_F32);
    if (f32) fy = (double)(float)fy;
    if (fy != fy || fy < 0)
        return ray_error("domain", "?: float roll bound must be >= 0");
    ray_t* out = ray_vec_new(f32 ? RAY_F32 : RAY_F64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    for (int64_t i = 0; i < n; i++) {
        uint64_t u = ((uint64_t)rand() << 31) | (uint64_t)rand();
        double v = fy * ((double)u / 4611686018427387904.0);   /* / 2^62 */
        if (f32) {
            float fv = (float)v;
            if (fv >= (float)fy && fy > 0) fv = nextafterf((float)fy, 0.0f);
            ((float*)ray_data(out))[i] = fv;
        } else {
            if (v >= fy && fy > 0) v = nextafter(fy, 0.0);
            ((double*)ray_data(out))[i] = v;
        }
    }
    return out;
}

/* n?0b — random booleans (01b). */
static ray_t* q_gen_bits(int64_t n) {
    ray_t* out = ray_vec_new(RAY_BOOL, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    for (int64_t i = 0; i < n; i++) ((bool*)ray_data(out))[i] = rand() & 1;
    return out;
}

/* n?0x0 — random bytes 0x00-0xff. */
static ray_t* q_gen_bytes(int64_t n) {
    ray_t* out = ray_vec_new(RAY_U8, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    for (int64_t i = 0; i < n; i++)
        ((uint8_t*)ray_data(out))[i] = (uint8_t)(rand() & 0xFF);
    return out;
}

/* n?0 / n?0i — full-range longs/ints.  rand() yields 31 bits, so words are
 * composed from multiple calls.  The engine's sentinel values (0N=INT_MIN,
 * -0W=INT_MIN+1, 0W=INT_MAX) are never generated (rejection loop) — a roll
 * must not fabricate nulls/infinities (decision recorded in the plan). */
static ray_t* q_gen_longs(int64_t n) {
    ray_t* out = ray_vec_new(RAY_I64, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int64_t* d = (int64_t*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int64_t v;
        do {
            uint64_t u = ((uint64_t)rand() << 33) |
                         ((uint64_t)rand() << 2)  |
                         ((uint64_t)rand() & 3);
            v = (int64_t)u;
        } while (v == INT64_MIN || v == INT64_MIN + 1 || v == INT64_MAX);
        d[i] = v;
    }
    return out;
}

static ray_t* q_gen_ints(int64_t n) {
    ray_t* out = ray_vec_new(RAY_I32, n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    int32_t* d = (int32_t*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        int32_t v;
        do {
            uint32_t u = ((uint32_t)rand() << 1) | ((uint32_t)rand() & 1);
            v = (int32_t)u;
        } while (v == INT32_MIN || v == INT32_MIN + 1 || v == INT32_MAX);
        d[i] = v;
    }
    return out;
}

/* Deal n distinct values from [0,total) — partial Fisher-Yates over `til total`,
 * take the first n (kdb deal / permute; uses the same libc rand() the roll path
 * does).  n<=total required.  Result is an owned I64 vector. */
static ray_t* q_deal_indices(int64_t n, int64_t total) {
    if (n < 0) return ray_error("domain", "?: deal count must be non-negative");
    if (n > total) return ray_error("length", "?: cannot deal %lld from %lld",
                                    (long long)n, (long long)total);
    ray_t* arr = ray_vec_new(RAY_I64, total > 0 ? total : 1);
    if (RAY_IS_ERR(arr)) return arr;
    arr->len = total;
    int64_t* a = (int64_t*)ray_data(arr);
    for (int64_t i = 0; i < total; i++) a[i] = i;
    for (int64_t i = 0; i < n; i++) {
        int64_t range = total - i;
        int64_t j = i + (int64_t)(rand() % range);
        int64_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
    arr->len = n;                          /* take first n (buffer already sized) */
    return arr;
}

/* Deal/permute n indices then gather them from the list y (collapsed). */
static ray_t* q_deal_pick(int64_t n, ray_t* y) {
    ray_t* idx = q_deal_indices(n, ray_len(y));
    if (!idx || RAY_IS_ERR(idx)) return idx;
    ray_t* out = ray_at_fn(y, idx);
    ray_release(idx);
    if (out && out->type == RAY_LIST) { ray_t* c = q_collapse_list(out); ray_release(out); return c; }
    return out;
}

/* q `x?y` — find / roll / pick (type-dispatch on the operands).
 *   list ? y   -> find.  kdb miss semantics: the smallest index NOT in the
 *                 list, i.e. `count x` — rayfall find returns 0N on a miss
 *                 (atom result) or per-element 0N (vector needle), so both
 *                 shapes are remapped to count here.
 *   n ? int    -> roll: n randoms in [0,int)  (rayfall rand)
 *   n ? list   -> pick: n random indices gathered from the list
 *   -n ? m / 0N ? m -> deal / permute (q_deal_indices)
 *   generate arms (`m sym, float, 0b, 0x0, 0, 0i, 0Ng) -> q_gen_* above.
 *   Deferred cells (error, never a wrong answer): temporal roll, `n?" "`
 *   char roll (string model), deal of 0/0i. */
/* First index of x (a boxed list) whose ITEM whole-matches v, else cnt
 * (the kdb miss).  Borrows both. */
static int64_t q_list_find_item(ray_t* x, ray_t* v, int64_t cnt) {
    ray_t** ex = (ray_t**)ray_data(x);
    for (int64_t i = 0; i < cnt; i++)
        if (ex[i] && q_values_match(ex[i], v)) return i;
    return cnt;
}

static ray_t* q_roll_wrap(ray_t* x, ray_t* y) {
    /* d?y — reverse dictionary lookup (basics/dictsandtables.md): the key of
     * the FIRST value matching y, i.e. keys[vals?y].  A find miss lands at
     * count vals, and ray_at_fn null-fills that out-of-range key index — the
     * typed null of the key domain, kdb's miss result.  Keyed tables keep
     * their own (deferred) path. */
    if (x && x->type == RAY_DICT && !q_is_keyed_table(x)) {
        ray_t* keys = ray_dict_keys(x);              /* borrowed */
        if (!keys) return ray_error("type", "?: malformed dictionary");
        int vo = 0;
        ray_t* vv = q_dict_vals_vec(x, &vo);
        if (!vv) return ray_error("type", "?: malformed dictionary");
        ray_t* i = q_roll_wrap(vv, y);               /* find arm: miss -> count */
        if (vo) ray_release(vv);
        if (!i || RAY_IS_ERR(i)) return i;
        ray_t* r = ray_at_fn(keys, i);
        ray_release(i);
        if (r && r->type == RAY_LIST) { ray_t* c = q_typed_empty_like(q_collapse_list(r), keys); ray_release(r); return c; }
        return r;
    }
    if (x && (ray_is_vec(x) || x->type == RAY_LIST)) {          /* find */
        int64_t cnt = ray_len(x);
        /* ---- rank-aware arms (ref/find.md): a rank-n haystack looks for
         * rank n-1 objects.  A -RAY_STR atom counts as rank>=1 (kdb strings
         * are char LISTS). ---- */
        int x_ranked = 0;                    /* x is a "list of lists" */
        if (x->type == RAY_LIST && cnt > 0) {
            ray_t* x0 = ((ray_t**)ray_data(x))[0];
            x_ranked = x0 && (!ray_is_atom(x0) || x0->type == -RAY_STR);
        }
        if (x_ranked && y && y->type == RAY_LIST) {
            /* list-of-lists x, MIXED y: items of x matched with ITEMS of y
             * (`u?(2 3;\`ab)` -> 3 3 — never with the whole of y). */
            int64_t ny = ray_len(y);
            ray_t** e = (ray_t**)ray_data(y);
            ray_t* out = ray_vec_new(RAY_I64, ny > 0 ? ny : 1);
            if (RAY_IS_ERR(out)) return out;
            out->len = ny;
            int64_t* o = (int64_t*)ray_data(out);
            for (int64_t j = 0; j < ny; j++)
                o[j] = e[j] ? q_list_find_item(x, e[j], cnt) : cnt;
            return out;
        }
        if (x_ranked && y && !ray_is_atom(y) && ray_is_vec(y) && y->type != RAY_LIST) {
            /* list-of-lists x, SIMPLE vector y: whole-y match (`u?10 2 -6`
             * -> 1). */
            return ray_i64(q_list_find_item(x, y, cnt));
        }
        if (x->type != RAY_LIST && ray_is_vec(x) && y && y->type == RAY_LIST) {
            /* simple-vector x, list y whose first item is a list: RIGHT-
             * ATOMIC item-by-item; an ATOM item in this mode is a rank
             * mismatch and MISSES (w?rt: (10 5 -1;-8;3 17) -> (0 3 4;7;2 7),
             * the doc's own transcript). */
            int64_t ny = ray_len(y);
            ray_t** e = (ray_t**)ray_data(y);
            int y0_ranked = ny > 0 && e[0] &&
                            (!ray_is_atom(e[0]) || e[0]->type == -RAY_STR);
            if (y0_ranked) {
                ray_t* out = ray_list_new(ny > 0 ? ny : 1);
                if (RAY_IS_ERR(out)) return out;
                for (int64_t j = 0; j < ny; j++) {
                    ray_t* rr;
                    if (!e[j] || (ray_is_atom(e[j]) && e[j]->type != -RAY_STR))
                        rr = ray_i64(cnt);           /* rank-0 item: miss */
                    else
                        rr = q_roll_wrap(x, e[j]);
                    if (!rr || RAY_IS_ERR(rr)) { ray_release(out); return rr; }
                    out = ray_list_append(out, rr);  /* retains */
                    ray_release(rr);
                    if (RAY_IS_ERR(out)) return out;
                }
                return out;                          /* mixed shapes stay boxed */
            }
        }
        ray_t* i = ray_find_fn(x, y);
        if (!i || RAY_IS_ERR(i)) return i;
        if (ray_is_atom(i) && i->type == -RAY_I64 && RAY_ATOM_IS_NULL(i)) {
            ray_release(i);
            return ray_i64(cnt);                    /* kdb: miss -> count x */
        }
        if (i->type == RAY_I64) {                   /* vector needle: per-elem */
            int64_t n = ray_len(i);
            int64_t* d = (int64_t*)ray_data(i);     /* fresh rc=1 from find */
            for (int64_t j = 0; j < n; j++)
                if (d[j] == NULL_I64) d[j] = cnt;
            i->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
        }
        return i;
    }
    if (x && (x->type == -RAY_I64 || x->type == -RAY_I32)) {
        int64_t nx = (x->type == -RAY_I64) ? x->i64 : x->i32;
        if (RAY_ATOM_IS_NULL(x)) {                  /* 0N ? y — permute all items */
            if (y && (y->type == -RAY_I64 || y->type == -RAY_I32)) {
                int64_t m = (y->type == -RAY_I64) ? y->i64 : y->i32;
                if (m < 0) return ray_error("type", "?: permute needs a non-negative count");
                return q_deal_indices(m, m);
            }
            if (y && (ray_is_vec(y) || y->type == RAY_LIST)) return q_deal_pick(ray_len(y), y);
            return ray_error("nyi", "?: permute of this operand is deferred");
        }
        int deal = nx < 0;
        int64_t n = deal ? -nx : nx;
        /* ---- generate arms (atom y — non-integer, or the 0/0i forms) ---- */
        if (y && y->type == -RAY_SYM)               /* n?`m sym roll / deal */
            return q_gen_syms(n, y, deal);
        if (y && (y->type == -RAY_F64 || y->type == -RAY_F32)) {
            if (deal) return ray_error("type", "?: deal needs a positive long or 0Ng right operand");
            return q_gen_floats(n, y);              /* n?f uniform [0,y) */
        }
        if (y && y->type == -RAY_BOOL) {
            if (deal) return ray_error("type", "?: deal needs a positive long or 0Ng right operand");
            if (y->b8) return ray_error("nyi", "?: bool roll is defined for 0b only");
            return q_gen_bits(n);                   /* n?0b */
        }
        if (y && y->type == -RAY_U8) {
            if (deal) return ray_error("type", "?: deal needs a positive long or 0Ng right operand");
            if (y->u8) return ray_error("nyi", "?: byte roll is defined for 0x0 only");
            return q_gen_bytes(n);                  /* n?0x0 */
        }
        if (y && y->type == -RAY_GUID) {            /* n?0Ng / -n?0Ng — env guid.
             * Deal reuses the same generator: distinctness rests on the
             * 122-bit space (collisions negligible); kdb's process/time deal
             * seed nuance is NOT reproduced (recorded divergence). */
            if (!RAY_ATOM_IS_NULL(y))
                return ray_error("type", "?: guid generate needs 0Ng");
            ray_t* cnt = ray_i64(n);
            ray_t* g = q_env_call1("guid", cnt);
            ray_release(cnt);
            return g;
        }
        if (y && (y->type == -RAY_I64 || y->type == -RAY_I32) &&
            !RAY_ATOM_IS_NULL(y)) {
            int64_t m = (y->type == -RAY_I64) ? y->i64 : y->i32;
            if (m == 0) {                           /* n?0 / n?0i full-range */
                if (deal)
                    return ray_error("nyi", "?: deal of 0/0i (full-range distinct) is deferred");
                return (y->type == -RAY_I64) ? q_gen_longs(n) : q_gen_ints(n);
            }
        }
        if (deal) {                                 /* -n ? y — deal, no replacement */
            if (y && (y->type == -RAY_I64 || y->type == -RAY_I32)) {
                int64_t m = (y->type == -RAY_I64) ? y->i64 : y->i32;
                if (m <= 0) return ray_error("domain", "?: deal max must be positive");
                return q_deal_indices(n, m);
            }
            if (y && (ray_is_vec(y) || y->type == RAY_LIST)) return q_deal_pick(n, y);
            return ray_error("nyi", "?: deal of this operand is deferred");
        }
        if (y && (y->type == -RAY_I64 || y->type == -RAY_I32))  /* roll */
            return q_env_call2("rand", x, y);
        if (y && (ray_is_vec(y) || y->type == RAY_LIST)) {      /* pick */
            ray_t* len = ray_i64(ray_len(y));
            ray_t* idx = q_env_call2("rand", x, len);
            ray_release(len);
            if (!idx || RAY_IS_ERR(idx)) return idx;
            ray_t* out = ray_at_fn(y, idx);
            ray_release(idx);
            if (out && out->type == RAY_LIST) {
                ray_t* c = q_collapse_list(out);
                ray_release(out);
                return c;
            }
            return out;
        }
        return ray_error("nyi", "?: right operand form is deferred (temporal/char roll)");
    }
    return ray_error("type", "?: unsupported operand types");
}

/* q `rand x` — {first 1?x} exactly (ref/rand.md).  A list picks one random
 * item; an atom yields one random value of x's type via the q_roll_wrap
 * generate arms (so every arm and error class above is reused verbatim). */
static ray_t* q_rand_wrap(ray_t* x) {
    if (x && (ray_is_vec(x) || x->type == RAY_LIST)) {  /* pick one item */
        int64_t len = ray_len(x);
        if (len <= 0) return ray_error("length", "rand: empty list");
        ray_t* ia = ray_i64((int64_t)(rand() % len));
        ray_t* out = ray_at_fn(x, ia);
        ray_release(ia);
        return out;
    }
    ray_t* one = ray_i64(1);
    ray_t* r = q_roll_wrap(one, x);                     /* 1?x */
    ray_release(one);
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* i0 = ray_i64(0);
    ray_t* out = ray_at_fn(r, i0);                      /* first */
    ray_release(i0);
    ray_release(r);
    return out;
}

/* ===== q calendar home (see q_registry.h) ==================================
 * Hinnant days_from_civil (public domain, http://howardhinnant.github.io/
 * date_algorithms.html), rebased to the kdb/base date epoch: the algorithm
 * yields days since 1970-01-01, and 2000.01.01 is unix day 10957 (the same
 * constant base temporal.c uses in the inverse direction). */
int64_t q_days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                    /* [0,399] */
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   /* [0,365] */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* [0,146096] */
    return era * 146097 + doe - 719468 - 10957;  /* unix days -> 2000.01.01 epoch */
}

/* kdb literal/value domain: 0001.01.01 .. 9999.12.31 (datatypes.md), real
 * month lengths, proleptic-Gregorian leap rule. */
int q_date_valid(int64_t y, int64_t m, int64_t d) {
    static const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1) return 0;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    return d <= md[m - 1] + ((m == 2 && leap) ? 1 : 0);
}

/* Timestamp payload composition (see q_registry.h for the contract and the
 * boundary rationale — codex plan-review round 1). */
int q_ts_compose_checked(int64_t days, int64_t tod_ns, int64_t* out) {
    __int128 ns = (__int128)days * 86400000000000LL + tod_ns;
    if (ns > INT64_MAX || ns < -(__int128)INT64_MAX) return 0;
    *out = (int64_t)ns;
    return 1;
}
int64_t q_ts_compose(int64_t days, int64_t tod_ns) {
    int64_t ns;
    if (q_ts_compose_checked(days, tod_ns, &ns)) return ns;
    return ((__int128)days * 86400000000000LL + tod_ns) < 0 ? -INT64_MAX
                                                            : INT64_MAX;
}

/* ===== q cast home (see q_registry.h for the reuse contract) ===============
 * Designator resolution is separate from conversion so C callers (future
 * bool-widening / promotion work) can invoke q_cast_to(tag, x) directly. */

int8_t q_cast_designator(ray_t* t, int* is_tok) {
    *is_tok = 0;
    if (!t) return 0;
    if (t->type == -RAY_I16) {          /* kdb type number == rayfall tag */
        if (RAY_ATOM_IS_NULL(t)) return 0;
        int16_t n = t->i16;
        if (n <= 0) { *is_tok = 1; n = (int16_t)-n; }
        switch (n) {
        case RAY_BOOL: case RAY_U8:  case RAY_I16: case RAY_I32:
        case RAY_I64:  case RAY_F32: case RAY_F64: case RAY_SYM:
        RAY_TEMPORAL32_CASES: RAY_TEMPORAL64_CASES: RAY_TEMPORALF_CASES:
            return (int8_t)n;
        default: return 0;    /* guid/char: deferred */
        }
    }
    if (t->type == -RAY_STR && ray_str_len(t) == 1) {
        char c = ray_str_ptr(t)[0];
        if (c >= 'A' && c <= 'Z') { *is_tok = 1; c = (char)(c - 'A' + 'a'); }
        switch (c) {
        case 'b': return RAY_BOOL; case 'x': return RAY_U8;
        case 'h': return RAY_I16;  case 'i': return RAY_I32;
        case 'j': return RAY_I64;  case 'e': return RAY_F32;
        case 'f': return RAY_F64;  case 's': return RAY_SYM;
        case 'd': return RAY_DATE; case 'g': return RAY_GUID;
        case 't': return RAY_TIME; case 'p': return RAY_TIMESTAMP;
        case 'm': return RAY_MONTH;
        case 'u': return RAY_MINUTE;
        case 'v': return RAY_SECOND;
        case 'n': return RAY_TIMESPAN;
        case 'z': return RAY_DATETIME;
        default:  return 0;       /* c + "*" identity: deferred */
        }
    }
    if (t->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(t->i64);
        if (!s) return 0;
        const char* nm = ray_str_ptr(s);
        size_t l = ray_str_len(s);
        int8_t r = 0;
        if      (l == 0)                              { *is_tok = 1; r = RAY_SYM; }
        else if (l == 4 && !memcmp(nm, "long",    4)) r = RAY_I64;
        else if (l == 5 && !memcmp(nm, "float",   5)) r = RAY_F64;
        else if (l == 3 && !memcmp(nm, "int",     3)) r = RAY_I32;
        else if (l == 5 && !memcmp(nm, "short",   5)) r = RAY_I16;
        else if (l == 7 && !memcmp(nm, "boolean", 7)) r = RAY_BOOL;
        else if (l == 4 && !memcmp(nm, "byte",    4)) r = RAY_U8;
        else if (l == 4 && !memcmp(nm, "real",    4)) r = RAY_F32;
        else if (l == 6 && !memcmp(nm, "symbol",  6)) r = RAY_SYM;
        else if (l == 4 && !memcmp(nm, "date",    4)) r = RAY_DATE;
        else if (l == 5 && !memcmp(nm, "month",   5)) r = RAY_MONTH;
        else if (l == 6 && !memcmp(nm, "minute",  6)) r = RAY_MINUTE;
        else if (l == 6 && !memcmp(nm, "second",  6)) r = RAY_SECOND;
        else if (l == 8 && !memcmp(nm, "timespan",8)) r = RAY_TIMESPAN;
        else if (l == 4 && !memcmp(nm, "time",    4)) r = RAY_TIME;
        else if (l == 9 && !memcmp(nm, "timestamp", 9)) r = RAY_TIMESTAMP;
        else if (l == 8 && !memcmp(nm, "datetime", 8)) r = RAY_DATETIME;
        ray_release(s);
        return r;
    }
    return 0;
}

/* RAY vector type -> q type-name (ref/key.md "type of a vector"; the exact
 * REVERSE of the cast-designator name map above — keep the two in sync). */
static const char* q_type_qname(int8_t t) {
    switch (t) {
    case RAY_BOOL:      return "boolean";
    case RAY_U8:        return "byte";
    case RAY_I16:       return "short";
    case RAY_I32:       return "int";
    case RAY_I64:       return "long";
    case RAY_F32:       return "real";
    case RAY_F64:       return "float";
    case RAY_SYM:       return "symbol";
    case RAY_DATE:      return "date";
    case RAY_MONTH:     return "month";
    case RAY_MINUTE:    return "minute";
    case RAY_SECOND:    return "second";
    case RAY_TIME:      return "time";
    case RAY_TIMESPAN:  return "timespan";
    case RAY_TIMESTAMP: return "timestamp";
    case RAY_DATETIME:  return "datetime";
    default:            return NULL;
    }
}

/* tag -> rayfall `as` type-sym spelling (cast delegation targets only) */
static const char* q_tag_rayname(int8_t tag) {
    switch (tag) {
    case RAY_BOOL: return "BOOL"; case RAY_U8:  return "U8";
    case RAY_I16:  return "I16";  case RAY_I32: return "I32";
    case RAY_I64:  return "I64";  case RAY_F64: return "F64";
    case RAY_DATE: return "DATE"; case RAY_TIME: return "TIME";
    case RAY_MONTH: return "MONTH";
    case RAY_MINUTE: return "MINUTE";
    case RAY_SECOND: return "SECOND";
    case RAY_TIMESPAN: return "TIMESPAN";
    case RAY_TIMESTAMP: return "TIMESTAMP";
    case RAY_DATETIME: return "DATETIME";
    default:       return NULL;
    }
}

/* kdb float->integer casts ROUND (rint = IEEE nearest/ties-even: `long$3.7
 * is 4, "j"$2.5 is 2, `int$6.6 is 7 — KX ref pins), where rayfall `as`
 * truncates — integer targets pre-round here; everything else delegates to
 * base ray_cast_fn.  RAY_LIST distributes per element and collapses. */
ray_t* q_cast_to(int8_t tag, ray_t* x) {
    if (x && x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* r = q_cast_to(tag, e[i]);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);   /* append retains */
            ray_release(r);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    int isint = (tag == RAY_I64 || tag == RAY_I32 || tag == RAY_I16);
    if (isint && x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null((int8_t)-tag);
        double r = rint(x->f64);              /* F32 atoms store f64 payload */
        if (tag == RAY_I64) return ray_i64((int64_t)r);
        if (tag == RAY_I32) return ray_i32((int32_t)r);
        return ray_i16((int16_t)r);
    }
    if (isint && x && (x->type == RAY_F64 || x->type == RAY_F32)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(tag, n);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        int is64 = (x->type == RAY_F64);
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(x))[i]
                            : (double)((const float*)ray_data(x))[i];
            int isnull = isnan(v);
            int64_t iv = isnull ? 0 : (int64_t)rint(v);
            if      (tag == RAY_I64) ((int64_t*)ray_data(out))[i] = iv;
            else if (tag == RAY_I32) ((int32_t*)ray_data(out))[i] = (int32_t)iv;
            else                     ((int16_t*)ray_data(out))[i] = (int16_t)iv;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* kdb `"x"$str` maps CHARS to bytes ("x"$"abc" -> 0x616263, ref/cast.md
     * #byte); base's U8 STR arm parses decimal text instead — pre-empt it.
     * One char = char atom -> byte ATOM; else a byte vector of the raw chars
     * (empty string -> empty byte vector). */
    if (tag == RAY_U8 && x && x->type == -RAY_STR) {
        const char* sp = ray_str_ptr(x);
        size_t sl = ray_str_len(x);
        if (sl == 1) return ray_u8((uint8_t)sp[0]);
        return ray_vec_from_raw(RAY_U8, sp, (int64_t)sl);
    }
    /* kdb integer-family casts ROUND floats (`int$6.6 -> 7, ref/cast.md);
     * byte joins that family (derived — byte float-cast is unpinned).  Float
     * null -> 0x00: byte has no null (basics/datatypes.md blank column). */
    if (tag == RAY_U8 && x && (x->type == -RAY_F64 || x->type == -RAY_F32)) {
        if (RAY_ATOM_IS_NULL(x)) return ray_u8(0);
        return ray_u8((uint8_t)(int64_t)rint(x->f64));  /* F32 stores f64 */
    }
    if (tag == RAY_U8 && x && (x->type == RAY_F64 || x->type == RAY_F32)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_U8, n);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        int is64 = (x->type == RAY_F64);
        for (int64_t i = 0; i < n; i++) {
            double v = is64 ? ((const double*)ray_data(x))[i]
                            : (double)((const float*)ray_data(x))[i];
            ((uint8_t*)ray_data(out))[i] = isnan(v) ? 0 : (uint8_t)(int64_t)rint(v);
        }
        return out;
    }
    /* kdb `timestamp$date: days -> ns, SATURATING outside the timestamp year
     * range (`timestamp$1666.09.02 -> -0Wp, datatypes.md:149) — base's arm
     * multiplies unchecked (i64 overflow, UBSan, builtins.c:1616) — and
     * mapping the date sentinels to the i64 sentinels (0Nd -> 0Np,
     * +-0Wd -> +-0Wp, which the saturation clamp yields for free). */
    if (tag == RAY_TIMESTAMP && x && x->type == -RAY_DATE) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(q_ts_compose((int64_t)x->i32, 0));
    }
    if (tag == RAY_TIMESTAMP && x && x->type == RAY_DATE) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_TIMESTAMP, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        const int32_t* d = (const int32_t*)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            int isnull = (d[i] == INT32_MIN);
            ((int64_t*)ray_data(out))[i] =
                isnull ? 0 : q_ts_compose((int64_t)d[i], 0);
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* kdb `timestamp$time keeps the TIME OF DAY (ms -> ns on day 0, derived:
     * time is ms-of-day, timestamp ns; base's same-width path relabels the
     * raw ms payload as ns — a wrong answer, caught by the designator audit).
     * Sentinels map across (0Nt -> 0Np, +-0Wt -> +-0Wp). */
    if (tag == RAY_TIMESTAMP && x && x->type == -RAY_TIME) {
        if (RAY_ATOM_IS_NULL(x)) return ray_typed_null(-RAY_TIMESTAMP);
        if (x->i32 == INT32_MAX)  return ray_timestamp(INT64_MAX);
        if (x->i32 == -INT32_MAX) return ray_timestamp(-INT64_MAX);
        return ray_timestamp((int64_t)x->i32 * 1000000LL);
    }
    if (tag == RAY_TIMESTAMP && x && x->type == RAY_TIME) {
        int64_t n = ray_len(x);
        ray_t* out = ray_vec_new(RAY_TIMESTAMP, n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        out->len = n;
        const int32_t* d = (const int32_t*)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            int isnull = (d[i] == INT32_MIN);
            ((int64_t*)ray_data(out))[i] =
                isnull ? 0
                : (d[i] == INT32_MAX)  ? INT64_MAX
                : (d[i] == -INT32_MAX) ? -INT64_MAX
                : (int64_t)d[i] * 1000000LL;
            if (isnull) ray_vec_set_null(out, i, true);
        }
        return out;
    }
    /* float -> timestamp: base truncates the float to a raw ns count, but
     * kdb's unit semantics here (rint-ns vs datetime-style fractional DAYS)
     * is unpinned in the docs corpus — error beats a wrong answer, so the
     * shape is a deferred cell (designator-audit decision, plan 2026-07-07). */
    if (tag == RAY_TIMESTAMP && x &&
        (x->type == -RAY_F64 || x->type == -RAY_F32 ||
         x->type == RAY_F64  || x->type == RAY_F32))
        return ray_error("nyi", "$: float->timestamp cast is deferred");
    if (tag == RAY_SYM) {                 /* `symbol$sym is identity; rest nyi */
        if (x && (x->type == -RAY_SYM || x->type == RAY_SYM)) {
            ray_retain(x);
            return x;
        }
        return ray_error("nyi", "$: cast to symbol is deferred (use `$ / \"S\"$ on strings)");
    }
    const char* nm = q_tag_rayname(tag);
    if (!nm)                              /* F32 cast: no base arm — deferred */
        return ray_error("nyi", "$: unsupported cast designator (deferred)");
    ray_t* ts = ray_sym(ray_sym_intern(nm, strlen(nm)));
    if (!ts || RAY_IS_ERR(ts)) return ts;
    ray_t* r = ray_cast_fn(ts, x);
    ray_release(ts);
    return r;
}

/* "D"$ date-string scan (ref/tok.md date formats).  Supported subset:
 * yyyymmdd (8 digits, the doc's [yy]yymmdd with an unambiguous 4-digit year)
 * and yyyy.mm.dd / yyyy-mm-dd / yyyy/mm/dd (the doc's separator variants;
 * "D"$"2000-12-12" is letter-pinned).  Two-digit years and MMM month names
 * are deferred.  Returns 1 and fills y/m/d on a shape match; civil validity
 * is the caller's q_date_valid check. */
static int q_date_scan(const char* p, size_t len,
                       int64_t* y, int64_t* m, int64_t* d) {
    if (len == 8) {
        for (int i = 0; i < 8; i++)
            if (p[i] < '0' || p[i] > '9') return 0;
        *y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        *m = (p[4]-'0')*10 + (p[5]-'0');
        *d = (p[6]-'0')*10 + (p[7]-'0');
        return 1;
    }
    if (len == 10 && (p[4] == '.' || p[4] == '-' || p[4] == '/') && p[7] == p[4]) {
        for (int i = 0; i < 10; i++) {
            if (i == 4 || i == 7) continue;
            if (p[i] < '0' || p[i] > '9') return 0;
        }
        *y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        *m = (p[5]-'0')*10 + (p[6]-'0');
        *d = (p[8]-'0')*10 + (p[9]-'0');
        return 1;
    }
    return 0;
}

static int q_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a CANONICAL 36-char UUID (8-4-4-4-12, hyphens at 8/13/18/23, hex
 * elsewhere, case-insensitive) into out[16].  Returns 1 on success, 0 on any
 * shape/char mismatch.  kdb "G"$ ALSO accepts IPv4/IPv6 address forms
 * (test/q/cast/tok.qcmd, skiplisted) — DEFERRED here (see PLAN.md); those
 * inputs fail this shape check and Tok returns 0Ng. */
static int q_parse_uuid(const char* p, size_t len, uint8_t out[16]) {
    if (len != 36) return 0;
    int bi = 0;
    for (size_t i = 0; i < 36; ) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (p[i] != '-') return 0;
            i++;
            continue;
        }
        int h = q_hexval(p[i]);
        int l = q_hexval(p[i + 1]);
        if (h < 0 || l < 0) return 0;
        out[bi++] = (uint8_t)((h << 4) | l);
        i += 2;
    }
    return bi == 16;
}

/* "T"$ time-string scan (ref/tok.md).  Two forms, both -> i32 ms of day:
 *   - PACKED digits HHMMSSmmm (doc-pinned): "T"$"123456789" -> 12:34:56.789,
 *     "T"$"123456123987654" -> 12:34:56.123 (>=6 digits: HH MM SS then up to 3
 *     fractional; extra fractional digits ignored).
 *   - COLON HH:MM:SS[.f…] (derived — the natural literal spelling): the `.`
 *     fractional is optional; only its first 3 digits (millis) are used.
 * mm/ss must be < 60, else out-of-domain.  Returns 1 and fills *ms on success,
 * 0 on any shape/range mismatch (caller -> typed null 0Nt). */
static int q_all_digits(const char* p, size_t len) {
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++)
        if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}
static int q_time_scan(const char* p, size_t len, int32_t* ms) {
    int64_t h, mi, s, frac = 0;
    int has_colon = 0;
    for (size_t i = 0; i < len; i++) if (p[i] == ':') { has_colon = 1; break; }
    /* colon form: H[H]:MM:SS[.f…] */
    if (has_colon) {
        size_t i = 0;
        int64_t hv = 0;
        while (i < len && p[i] >= '0' && p[i] <= '9') { hv = hv * 10 + (p[i] - '0'); i++; }
        if (i == 0 || i > 2 || i >= len || p[i] != ':') return 0;
        i++;
        if (i + 2 > len || !q_all_digits(p + i, 2) || i + 2 >= len || p[i + 2] != ':')
            return 0;
        mi = (p[i] - '0') * 10 + (p[i + 1] - '0');
        i += 3;
        if (i + 2 > len || !q_all_digits(p + i, 2)) return 0;
        s = (p[i] - '0') * 10 + (p[i + 1] - '0');
        i += 2;
        if (i < len) {                        /* optional .fractional */
            if (p[i] != '.') return 0;
            i++;
            int64_t scale = 100;
            size_t seen = 0;
            while (i < len && p[i] >= '0' && p[i] <= '9') {
                if (seen < 3) { frac += (p[i] - '0') * scale; scale /= 10; seen++; }
                i++;
            }
            if (i != len) return 0;           /* trailing junk */
        }
        h = hv;
        if (mi >= 60 || s >= 60) return 0;
        *ms = (int32_t)(h * 3600000 + mi * 60000 + s * 1000 + frac);
        return 1;
    }
    /* packed HHMMSSmmm: >=6 digits, first 6 = HHMMSS, next up to 3 = millis */
    if (len >= 6 && q_all_digits(p, len)) {
        h  = (p[0] - '0') * 10 + (p[1] - '0');
        mi = (p[2] - '0') * 10 + (p[3] - '0');
        s  = (p[4] - '0') * 10 + (p[5] - '0');
        int64_t scale = 100;
        for (size_t i = 6; i < len && i < 9; i++) { frac += (p[i] - '0') * scale; scale /= 10; }
        if (mi >= 60 || s >= 60) return 0;
        *ms = (int32_t)(h * 3600000 + mi * 60000 + s * 1000 + frac);
        return 1;
    }
    return 0;
}

/* Clock scan for the duration Toks "U"$/"V"$/"N"$ -> ns.  Two forms
 * (the q_time_scan scheme generalised to ns):
 *   - PACKED digits HHMMSS + up to 9 fractional digits right-padded
 *     (doc-pinned for "N": tok.md:200 "N"$"123456123987654" ->
 *     0D12:34:56.123987654); >=4 digits HHMM accepted with SS=0 (derived).
 *   - COLON H[H]:MM[:SS[.f{1..9}]] (derived — the literal spellings).
 * mm/ss must be < 60.  Returns 1 and fills *ns, else 0 (caller -> null). */
static int q_clock_scan_ns(const char* p, size_t len, int64_t* ns) {
    int64_t h = 0, mi = 0, s = 0, frac = 0;
    int has_colon = 0;
    for (size_t i = 0; i < len; i++) if (p[i] == ':') { has_colon = 1; break; }
    if (has_colon) {
        size_t i = 0;
        while (i < len && p[i] >= '0' && p[i] <= '9') { h = h * 10 + (p[i] - '0'); i++; }
        if (i == 0 || i > 2 || i >= len || p[i] != ':') return 0;
        i++;
        if (i + 2 > len || !q_all_digits(p + i, 2)) return 0;
        mi = (p[i] - '0') * 10 + (p[i + 1] - '0');
        i += 2;
        if (i < len) {                        /* optional :SS[.f…] */
            if (p[i] != ':') return 0;
            i++;
            if (i + 2 > len || !q_all_digits(p + i, 2)) return 0;
            s = (p[i] - '0') * 10 + (p[i + 1] - '0');
            i += 2;
            if (i < len) {
                if (p[i] != '.' || i + 1 == len) return 0;
                i++;
                size_t fd = len - i;
                if (fd > 9 || !q_all_digits(p + i, fd)) return 0;
                for (size_t k = 0; k < fd; k++) frac = frac * 10 + (p[i + k] - '0');
                for (size_t k = fd; k < 9; k++) frac *= 10;
            }
        }
    } else if (len >= 4 && q_all_digits(p, len)) {
        h  = (p[0] - '0') * 10 + (p[1] - '0');
        mi = (p[2] - '0') * 10 + (p[3] - '0');
        if (len >= 6) {
            s = (p[4] - '0') * 10 + (p[5] - '0');
            size_t fd = len - 6;
            if (fd > 9) return 0;
            for (size_t k = 0; k < fd; k++) frac = frac * 10 + (p[6 + k] - '0');
            for (size_t k = fd; k < 9; k++) frac *= 10;
        } else if (len != 4) return 0;
    } else return 0;
    if (mi >= 60 || s >= 60) return 0;
    *ns = (h * 3600 + mi * 60 + s) * 1000000000LL + frac;
    return 1;
}

/* tod scan for "P"$: HH:MM:SS[.f{1..9}] -> ns of day (colon form only; the
 * packed date form is split off by the caller).  Returns 1/0. */
static int q_tod_scan_ns(const char* p, size_t len, int64_t* ns) {
    if (len < 8 || !q_all_digits(p, 2) || p[2] != ':' ||
        !q_all_digits(p + 3, 2) || p[5] != ':' || !q_all_digits(p + 6, 2))
        return 0;
    int64_t h  = (p[0]-'0')*10 + (p[1]-'0');
    int64_t mi = (p[3]-'0')*10 + (p[4]-'0');
    int64_t s  = (p[6]-'0')*10 + (p[7]-'0');
    if (mi >= 60 || s >= 60) return 0;
    int64_t frac = 0;
    if (len > 8) {
        if (p[8] != '.' || len == 9) return 0;
        size_t fd = len - 9;
        if (fd > 9) return 0;
        for (size_t k = 0; k < fd; k++) {
            if (p[9 + k] < '0' || p[9 + k] > '9') return 0;
            frac = frac * 10 + (p[9 + k] - '0');
        }
        for (size_t k = fd; k < 9; k++) frac *= 10;
    }
    *ns = (h * 3600 + mi * 60 + s) * 1000000000LL + frac;
    return 1;
}

/* "P"$ timestamp-string scan (ref/tok.md Â§Timestamps).  Subset:
 *   - Unix seconds, 9..11 digits [+ . fraction] (doc-pinned:
 *     "P"$"10129708800" -> 2290.12.31D00:00:00.000000000,
 *     "P"$"10129708800.123456789" -> ...D00:00:00.123456789);
 *   - date part (q_date_scan separator forms or packed yyyymmdd) + one of
 *     "DT- " + colon tod (pins: "PZ"$\:"20191122-11:11:11.123");
 *   - date-only -> midnight (derived).
 * MMM months / 2-digit years / timezone forms deferred.  Returns 1 + payload
 * ns on success; 0 -> caller yields 0Np (tok.md out-of-domain contract —
 * CHECKED compose, never the cast path's saturating +-0Wp). */
static int q_ts_scan(const char* p, size_t len, int64_t* out) {
    /* unix-seconds: 9..11 digits, optionally . + 1..9 fraction digits */
    size_t dot = len;
    for (size_t i = 0; i < len; i++) if (p[i] == '.') { dot = i; break; }
    if (dot >= 9 && dot <= 11 && q_all_digits(p, dot) &&
        (dot == len || (len > dot + 1 && len <= dot + 10 &&
                        q_all_digits(p + dot + 1, len - dot - 1)))) {
        int64_t secs = 0;
        for (size_t i = 0; i < dot; i++) secs = secs * 10 + (p[i] - '0');
        secs -= 946684800LL;                  /* unix epoch -> 2000.01.01 */
        int64_t ns;
        if (__builtin_mul_overflow(secs, 1000000000LL, &ns)) return 0;
        int64_t frac = 0;
        size_t fd = (dot == len) ? 0 : len - dot - 1;
        for (size_t k = 0; k < fd; k++) frac = frac * 10 + (p[dot + 1 + k] - '0');
        for (size_t k = fd; k < 9; k++) frac *= 10;
        if (__builtin_add_overflow(ns, frac, &ns)) return 0;
        *out = ns;
        return 1;
    }
    /* date [sep tod] */
    size_t dl = 0;
    if (len >= 10 && (p[4] == '.' || p[4] == '-' || p[4] == '/')) dl = 10;
    else if (len >= 8 && q_all_digits(p, 8)) dl = 8;
    if (dl == 0 || len < dl) return 0;
    int64_t y, mo, d;
    if (!q_date_scan(p, dl, &y, &mo, &d) || !q_date_valid(y, mo, d)) return 0;
    int64_t tod = 0;
    if (len > dl) {
        char sep = p[dl];
        if (!(sep == 'D' || sep == 'T' || sep == '-' || sep == ' ')) return 0;
        if (!q_tod_scan_ns(p + dl + 1, len - dl - 1, &tod)) return 0;
    }
    return q_ts_compose_checked(q_days_from_civil(y, mo, d), tod, out);
}

/* kdb Tok (ref/tok.md): parse a string as a value of the tag type.  Leading/
 * trailing blanks are trimmed; unparseable or out-of-range -> typed null.
 * Implicit recursion stops at STRINGS, not atoms: lists / string vectors
 * distribute. */
ray_t* q_tok_to(int8_t tag, ray_t* x) {
    if (x && (x->type == RAY_LIST || x->type == RAY_STR)) {
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* xi;
            if (x->type == RAY_LIST) {
                xi = ((ray_t**)ray_data(x))[i];
                ray_retain(xi);
            } else {
                size_t sl = 0;
                const char* sp = ray_str_vec_get(x, i, &sl);
                xi = ray_str(sp ? sp : "", sp ? sl : 0);
            }
            ray_t* r = q_tok_to(tag, xi);
            ray_release(xi);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    if (!x || x->type != -RAY_STR)
        return ray_error("type", "$: Tok right operand must be a string");
    const char* p = ray_str_ptr(x);
    size_t len = p ? ray_str_len(x) : 0;
    while (len && *p == ' ') { p++; len--; }            /* trim outer blanks */
    while (len && p[len - 1] == ' ') len--;
    switch (tag) {
    case RAY_SYM:
        return ray_sym(ray_sym_intern(len ? p : "", len));
    case RAY_BOOL:   /* truthy set pinned by ref/tok.md: "txyTXY1" */
        return ray_bool(len == 1 && strchr("1TtXxYy", p[0]) != NULL);
    case RAY_F64: case RAY_F32: {
        double v = 0;
        size_t used = len ? ray_parse_f64(p, len, &v) : 0;
        if (used != len || len == 0) return ray_typed_null((int8_t)-tag);
        return tag == RAY_F64 ? ray_f64(v) : ray_f32((float)v);
    }
    case RAY_I64: case RAY_I32: case RAY_I16: {
        int64_t v = 0;
        size_t used = len ? ray_parse_i64(p, len, &v) : 0;
        if (used != len || len == 0) return ray_typed_null((int8_t)-tag);
        /* Out-of-domain -> typed null (tok.md).  The bounds are ±INT*_MAX,
         * NOT INT*_MIN: the exact minimum IS the null sentinel (0N/0Ni/0Nh)
         * and must never round-trip as an accepted value. */
        if (tag == RAY_I64)
            return (v == INT64_MIN)
                 ? ray_typed_null(-RAY_I64) : ray_i64(v);
        if (tag == RAY_I32)
            return (v > INT32_MAX || v < -INT32_MAX)
                 ? ray_typed_null(-RAY_I32) : ray_i32((int32_t)v);
        return (v > INT16_MAX || v < -INT16_MAX)
             ? ray_typed_null(-RAY_I16) : ray_i16((int16_t)v);
    }
    case RAY_DATE: {
        /* Unparseable / invalid civil date / out-of-domain -> 0Nd, never an
         * error (tok.md pins "D"$"2147483648" -> 0Nd). */
        int64_t y, mo, d;
        if (!q_date_scan(p, len, &y, &mo, &d) || !q_date_valid(y, mo, d))
            return ray_typed_null(-RAY_DATE);
        return ray_date(q_days_from_civil(y, mo, d));
    }
    case RAY_MONTH: {
        /* "M"$str -> month (ref/tok.md designator table: month | -13 M).
         * Subset: "yyyy.mm" / "yyyy-mm" / "yyyy/mm" / packed yyyymm; the
         * civil month must be 01..12 and the year in the date domain
         * [1,9999].  Unparseable / out-of-domain -> 0Nm, never an error
         * (tok contract, mirrors "D"$). */
        int64_t y = 0, mo = 0;
        int ok = 0;
        if (len == 7 && q_all_digits(p, 4) &&
            (p[4] == '.' || p[4] == '-' || p[4] == '/') &&
            q_all_digits(p + 5, 2)) {
            y  = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
            mo = (p[5]-'0')*10 + (p[6]-'0');
            ok = 1;
        } else if (len == 6 && q_all_digits(p, 6)) {   /* packed yyyymm */
            y  = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
            mo = (p[4]-'0')*10 + (p[5]-'0');
            ok = 1;
        }
        if (!ok || mo < 1 || mo > 12 || y < 1 || y > 9999)
            return ray_typed_null(-RAY_MONTH);
        return ray_month((y - 2000) * 12 + (mo - 1));
    }
    case RAY_TIME: {
        /* "T"$str -> time (ref/tok.md).  Unparseable / out-of-domain -> 0Nt,
         * never an error (base ray_cast_fn errors on a bad string). */
        int32_t ms;
        if (!q_time_scan(p, len, &ms))
            return ray_typed_null(-RAY_TIME);
        return ray_time(ms);
    }
    case RAY_TIMESTAMP: {
        /* "P"$str -> timestamp (ref/tok.md Â§Timestamps).  Unparseable /
         * out-of-range -> 0Np, never an error (tok contract). */
        int64_t ns;
        if (!q_ts_scan(p, len, &ns))
            return ray_typed_null(-RAY_TIMESTAMP);
        return ray_timestamp(ns);
    }
    case RAY_DATETIME: {
        /* "Z"$str -> datetime.  tok.md:222-227 pins "PZ"$\: over ONE input
         * ("20191122-11:11:11.123" -> 2019.11.22T11:11:11.123): Z shares P's
         * accepted shapes at ms display precision, so reuse q_ts_scan (the
         * single P parser) and convert ns -> fractional days.  Unparseable /
         * invalid -> 0Nz, never an error (tok contract). */
        int64_t ns;
        if (!q_ts_scan(p, len, &ns))
            return ray_typed_null(-RAY_DATETIME);
        return ray_datetime((double)ns / 86400000000000.0);
    }
    case RAY_U8: {
        /* "X"$ reads the string as HEX ("X"$"42" -> 0x42, ref/tok.md).
         * Unparseable or > 0xff -> 0x00 (derived): tok.md pins out-of-
         * domain -> typed null, and byte HAS no null (basics/datatypes.md),
         * so its zero value stands in. */
        uint64_t v = 0;
        size_t i = 0;
        for (; i < len; i++) {
            char c = p[i];
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) break;
            v = (v << 4) | (uint64_t)d;
            if (v > 0xff) break;
        }
        if (len == 0 || i != len || v > 0xff) return ray_u8(0);
        return ray_u8((uint8_t)v);
    }
    case RAY_GUID: {
        /* "G"$str -> guid (basics/datatypes.md §Guid).  Tok contract:
         * unparseable / wrong-shape -> typed null 0Ng, never an error (base
         * ray_cast_fn "GUID" ERRORS on bad input, so parse here).  Canonical
         * 36-char UUID only; IPv4/IPv6 forms deferred (see q_parse_uuid). */
        uint8_t bytes[16];
        if (!q_parse_uuid(p, len, bytes)) return ray_typed_null(-RAY_GUID);
        return ray_guid(bytes);
    }
    case RAY_MINUTE: {
        /* "U"$str -> minute, FLOOR to the minute we are in (ref/tok.md:61
         * "U"$"12:13:14" -> 12:13; cast.md:168-170 truncation rule). */
        int64_t ns;
        if (!q_clock_scan_ns(p, len, &ns))
            return ray_typed_null(-RAY_MINUTE);
        return ray_minute(ns / 60000000000LL);
    }
    case RAY_SECOND: {
        /* "V"$str -> second, floor (derived — mirrors "U"$). */
        int64_t ns;
        if (!q_clock_scan_ns(p, len, &ns))
            return ray_typed_null(-RAY_SECOND);
        return ray_second(ns / 1000000000LL);
    }
    case RAY_TIMESPAN: {
        /* "N"$str -> timespan (tok.md:200-201: the digit run is
         * HHMMSS + up to 9 fractional digits right-padded, NOT a raw ns
         * count).  dD… string forms deferred. */
        int64_t ns;
        if (!q_clock_scan_ns(p, len, &ns))
            return ray_typed_null(-RAY_TIMESPAN);
        return ray_timespan(ns);
    }
    default:
        return ray_error("nyi", "$: char Tok is deferred");
    }
}

/* q `w$s` PAD (ref/pad.md): a LONG width w left-justifies the string s in a
 * field of |w| spaces (w<0 right-justifies); longer strings truncate to |w|.
 * Atomic through the container types (a LIST of strings pads each; DICT over
 * values; TABLE over columns).  Non-string leaves are a 'type error. */
static ray_t* q_pad(int64_t w, ray_t* x) {
    if (!x) return ray_error("type", "$: pad nil");
    if (x->type == -RAY_STR) {
        int64_t width = w < 0 ? -w : w;
        int right = w < 0;                 /* w<0 -> right-justify */
        const char* p = ray_str_ptr(x);
        int64_t n = (int64_t)ray_str_len(x);
        int64_t copy = n < width ? n : width;
        char stack[256];
        char* b = (width < (int64_t)sizeof stack) ? stack : malloc((size_t)width + 1);
        if (!b) return ray_error("wsfull", "$: out of memory");
        memset(b, ' ', (size_t)width);
        if (right) memcpy(b + (width - copy), p, (size_t)copy);   /* text at right */
        else       memcpy(b, p, (size_t)copy);                    /* text at left  */
        ray_t* r = ray_str(b, (size_t)width);
        if (b != stack) free(b);
        return r;
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* e = ray_at_fn(x, ia);
            ray_release(ia);
            if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
            ray_t* r = q_pad(w, e);
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);
        ray_t* v = ray_dict_vals(x);
        if (!k || !v) return ray_error("type", "$: bad dict");
        ray_t* nv = q_pad(w, v);
        if (!nv || RAY_IS_ERR(nv)) return nv;
        ray_retain(k);
        return ray_dict_new(k, nv);
    }
    if (x->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(x);
        ray_t* out = ray_table_new(nc);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(x, c);
            ray_t* ncol = q_pad(w, col);
            if (!ncol || RAY_IS_ERR(ncol)) { ray_release(out); return ncol; }
            out = ray_table_add_col(out, ray_table_col_name(x, c), ncol);
            ray_release(ncol);
            if (!out || RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    if (x->type == RAY_STR) {            /* string vector -> pad each element */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            size_t sn; const char* p = ray_str_vec_get(x, i, &sn);
            ray_t* s = ray_str(p ? p : "", p ? sn : 0);
            ray_t* r = q_pad(w, s);
            ray_release(s);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return ray_error("type", "$: pad expects a string");
}

/* q `t$x` — Cast/Tok dispatcher.  Multi-designator LHS ("fiij", `int`float,
 * 5 6h, (`int;"i";6h)) zips elementwise over x (ref/cast.md pins
 * (`int;"i";6h)$10 -> 10 10 10i: an ATOM rhs is broadcast).  Single
 * designators resolve via q_cast_designator and dispatch to the shared
 * q_cast_to / q_tok_to (the ONE conversion home — see q_registry.h).  A LONG
 * width LHS (`9$"foo"`) is PAD, not a cast — intercepted before designators. */
static ray_t* q_cast_wrap(ray_t* t, ray_t* x) {
    if (t && t->type == -RAY_I64) return q_pad(t->i64, x);
    int multi = t && ((t->type == -RAY_STR && ray_str_len(t) > 1) ||
                      t->type == RAY_SYM || t->type == RAY_I16 ||
                      t->type == RAY_LIST);
    if (multi) {
        int64_t n = (t->type == -RAY_STR) ? (int64_t)ray_str_len(t) : ray_len(t);
        int x_is_list = x && (ray_is_vec(x) || x->type == RAY_LIST);
        if (x_is_list && ray_len(x) != n)
            return ray_error("length", "$: designator/operand length mismatch");
        ray_t* out = ray_list_new(n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ti;
            if (t->type == -RAY_STR) ti = ray_str(ray_str_ptr(t) + i, 1);
            else {
                ray_t* idx = ray_i64(i);
                ti = ray_at_fn(t, idx);         /* sym/short vec, list */
                ray_release(idx);
            }
            if (!ti || RAY_IS_ERR(ti)) { ray_release(out); return ti; }
            ray_t* xi;
            if (x_is_list) {
                ray_t* idx = ray_i64(i);
                xi = ray_at_fn(x, idx);
                ray_release(idx);
            } else { xi = x; ray_retain(xi); }  /* atom rhs broadcasts */
            if (!xi || RAY_IS_ERR(xi)) { ray_release(ti); ray_release(out); return xi; }
            ray_t* r = q_cast_wrap(ti, xi);
            ray_release(ti);
            ray_release(xi);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
        }
        ray_t* c = q_collapse_list(out);
        ray_release(out);
        return c;
    }
    int is_tok = 0;
    int8_t tag = q_cast_designator(t, &is_tok);
    if (!tag)
        return ray_error("nyi", "$: unsupported cast designator (deferred)");
    return is_tok ? q_tok_to(tag, x) : q_cast_to(tag, x);
}

/* ===== Amend / Apply / Trap — @ and . ternary+ forms (wave-3) ==============
 * ref/amend.md + ref/apply.md.  q_at_wrap / q_dot_wrap are VARY wrappers now;
 * the rayfall VARY dispatch (eval.c) evaluates every bracket arg and
 * short-circuits an arg-eval error BEFORE calling us — exactly kdb Trap's
 * "errors in fx are not caught" rule.  Dispatch on arg count and first-arg
 * kind: a callable first arg -> Trap; a data first arg -> Amend; n==2 keeps
 * the historic Apply/Index path.  Amend is copy-on-write: q_explode to a
 * fresh rc==1 boxed list, run a sequential single-path engine (repeated
 * indices and cross-sections both decompose into it), collapse back. */

/* fwd decls (mutual recursion / define-before-use) */
static ray_t* q_amend_at(ray_t* v, ray_t* idx, ray_t* f, ray_t* y);
static ray_t* q_amend_dot(ray_t* d, ray_t* idx, ray_t* f, ray_t* y);
static ray_t* q_at_apply2(ray_t* f, ray_t* x);
static ray_t* q_dot_apply(ray_t* f, ray_t* a);
static int    q_is_fn_value(ray_t* x);              /* defined below */
static ray_t* q_elem_at(ray_t* v, int64_t i);       /* defined below */
static ray_t* q_call_n(ray_t* f, ray_t** a, int64_t k);  /* defined below */
static int    q_values_match(ray_t* a, ray_t* b);   /* defined below */

/* The `:` (assign / replace) function slot of Amend.  Arrives either as a
 * symbol atom spelled ":" (quoted verb-sym) or, defensively, a registry value
 * whose provenance spelling is ":". */
static int q_is_assign(ray_t* f) {
    if (!f) return 0;
    if (f->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(f->i64);
        int yes = s && ray_str_len(s) == 1 && ray_str_ptr(s)[0] == ':';
        if (s) ray_release(s);
        return yes;
    }
    q_provenance_t pv;
    return q_registry_provenance(f, &pv) && pv.spelling
        && pv.spelling[0] == ':' && pv.spelling[1] == '\0';
}

/* An index element must be a non-negative integer atom.  Returns 1 + writes
 * *out on success; 0 on a non-integer (symbol/float/list/null/error). */
static int q_idx_int(ray_t* e, int64_t* out) {
    if (!e || !ray_is_atom(e)) return 0;
    switch (e->type) {
    case -RAY_BOOL: *out = e->b8;  return 1;
    case -RAY_I16:  *out = e->i16; return 1;
    case -RAY_I32:  *out = e->i32; return 1;
    case -RAY_I64:  *out = e->i64; return 1;
    default: return 0;
    }
}

/* Copy v's items into a fresh rc==1 boxed list (amend in place with
 * ray_list_set, collapse back afterwards).  Borrowed v; owned result. */
static ray_t* q_explode(ray_t* v) {
    int64_t n = ray_len(v);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (!out || RAY_IS_ERR(out)) return out;
    for (int64_t i = 0; i < n; i++) {
        ray_t* e = q_elem_at(v, i);                 /* owned v[i] */
        if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
        out = ray_list_append(out, e);              /* RETAINS */
        ray_release(e);
    }
    return out;
}

/* u each v, collapsed to a typed vector (kdb whole-value @[d;::;u] == u'[d]). */
static ray_t* q_each_over(ray_t* f, ray_t* v) {
    ray_t* mapargs[2] = { f, v };
    ray_t* r = ray_map_fn(mapargs, 2);              /* base map == each */
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* c = q_collapse_list(r);
    ray_release(r);
    return c;
}

/* One amend update at an already-selected leaf `cur` (borrowed):
 *   unary   f(cur)          (y == NULL)
 *   binary  f(cur, ry)      (y != NULL)
 *   assign  ry              (f is `:`)
 * ry is y whole (yi < 0) or y[yi] when y is a per-step vector.  Owned leaf. */
static ray_t* q_amend_step(ray_t* f, ray_t* cur, ray_t* y, int64_t yi) {
    if (q_is_assign(f) && !y)
        return ray_error("type", "@: assign (:) needs a replacement value");
    ray_t* ry = NULL;                                /* borrowed-ish (owned) */
    if (y) {
        if (yi >= 0 && (ray_is_vec(y) || y->type == RAY_LIST)) {
            ry = q_elem_at(y, yi);                    /* owned */
            if (!ry || RAY_IS_ERR(ry)) return ry;
        } else { ry = y; ray_retain(ry); }           /* whole y */
    }
    ray_t* r;
    if (q_is_assign(f)) { r = ry; ray_retain(r); }   /* replace: new = ry */
    else if (y)         r = call_fn2(f, cur, ry);    /* binary f(cur, ry) */
    else                r = call_fn1(f, cur);        /* unary  f(cur)     */
    if (ry) ray_release(ry);
    return r;
}

/* @[d;key(s);f;y] — dict amend.  For each key: if present, amend the value at
 * its position; if ABSENT, extend the dict with that key (kdb inserts on a
 * missing-key amend, e.g. @[`a`b!1 2;`c;:;3] -> a|1 b|2 c|3).  Works on
 * exploded keys/vals boxed lists, collapses back, rebuilds. */
static ray_t* q_amend_dict(ray_t* d, ray_t* key, ray_t* f, ray_t* y) {
    ray_t* keys0 = ray_dict_keys(d);                /* borrowed */
    ray_t* vals0 = ray_dict_vals(d);                /* borrowed */
    if (!keys0 || !vals0) return ray_error("type", "@: malformed dictionary");
    ray_t* keys = q_explode(keys0);
    if (!keys || RAY_IS_ERR(keys)) return keys;
    ray_t* vals = q_explode(vals0);
    if (!vals || RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    int key_atom = ray_is_atom(key);
    int64_t steps = key_atom ? 1 : ray_len(key);
    if (!key_atom && y && (ray_is_vec(y) || y->type == RAY_LIST) && ray_len(y) != steps) {
        ray_release(keys); ray_release(vals); return ray_error("length", NULL);
    }
    for (int64_t k = 0; k < steps; k++) {
        ray_t* kk = key_atom ? (ray_retain(key), key) : q_elem_at(key, k);  /* owned */
        if (!kk || RAY_IS_ERR(kk)) { ray_release(keys); ray_release(vals); return kk; }
        /* locate kk in the working keys (linear; dicts are small) */
        int64_t pos = -1, kn = ray_len(keys);
        for (int64_t j = 0; j < kn; j++) {
            ray_t* kj = ray_list_get(keys, j);       /* borrowed */
            if (q_values_match(kj, kk)) { pos = j; break; }
        }
        int64_t yi = key_atom ? -1 : k;
        if (pos >= 0) {
            ray_t* cur = ray_list_get(vals, pos);    /* borrowed */
            ray_t* nv = q_amend_step(f, cur, y, yi);
            if (!nv || RAY_IS_ERR(nv)) { ray_release(kk); ray_release(keys); ray_release(vals); return nv; }
            vals = ray_list_set(vals, pos, nv);
            ray_release(nv);
        } else {                                     /* insert: apply to generic null */
            ray_t* nv = q_amend_step(f, RAY_NULL_OBJ, y, yi);
            if (!nv || RAY_IS_ERR(nv)) { ray_release(kk); ray_release(keys); ray_release(vals); return nv; }
            keys = ray_list_append(keys, kk);        /* RETAINS kk */
            vals = ray_list_append(vals, nv);
            ray_release(nv);
        }
        ray_release(kk);
    }
    ray_t* ck = q_collapse_list(keys);  ray_release(keys);
    ray_t* cv = q_collapse_list(vals);  ray_release(vals);
    if (!ck || RAY_IS_ERR(ck)) { if (cv) ray_release(cv); return ck; }
    if (!cv || RAY_IS_ERR(cv)) { ray_release(ck); return cv; }
    return ray_dict_new(ck, cv);                     /* consumes ck + cv */
}

/* Amend At — @[v;i;f] / @[v;i;f;y] / @[v;::;f...] (whole) / dict amend.
 * v borrowed; returns an owned copy-on-write value or owned error. */
static ray_t* q_amend_at(ray_t* v, ray_t* idx, ray_t* f, ray_t* y) {
    if (RAY_IS_NULL(idx)) {                          /* whole value */
        if (q_is_assign(f)) {
            if (!y) return ray_error("type", "@: assign (:) needs a value");
            ray_retain(y); return y;
        }
        if (y) return ray_error("nyi", "@[d;::;v;vy] deferred");  /* v'[d;vy] */
        return q_each_over(f, v);                     /* u'[d] */
    }
    if (v->type == RAY_DICT) return q_amend_dict(v, idx, f, y);
    if (!ray_is_vec(v) && v->type != RAY_LIST)
        return ray_error("type", "@: cannot amend a %s", ray_type_name(v->type));
    int64_t n = ray_len(v);
    int idx_atom = ray_is_atom(idx);
    int64_t steps = idx_atom ? 1 : ray_len(idx);
    /* vector index + vector y must conform (kdb 'length) */
    if (!idx_atom && y && (ray_is_vec(y) || y->type == RAY_LIST) && ray_len(y) != steps)
        return ray_error("length", NULL);
    ray_t* work = q_explode(v);
    if (!work || RAY_IS_ERR(work)) return work;
    for (int64_t k = 0; k < steps; k++) {
        int64_t pos;
        if (idx_atom) { if (!q_idx_int(idx, &pos)) { ray_release(work); return ray_error("index", NULL); } }
        else { ray_t* p = q_elem_at(idx, k); int ok = q_idx_int(p, &pos); ray_release(p);
               if (!ok) { ray_release(work); return ray_error("index", NULL); } }
        if (pos < 0 || pos >= n) { ray_release(work); return ray_error("index", NULL); }
        ray_t* cur = ray_list_get(work, pos);        /* borrowed slot */
        ray_t* nv  = q_amend_step(f, cur, y, idx_atom ? -1 : k);
        if (!nv || RAY_IS_ERR(nv)) { ray_release(work); return nv; }
        work = ray_list_set(work, pos, nv);          /* retains nv */
        ray_release(nv);
    }
    ray_t* c = q_collapse_list(work);
    ray_release(work);
    return c;
}

/* Deep single-path amend: descend path[0..plen) into d (copy-on-write), apply
 * the update at the leaf.  d borrowed; owned result. */
static ray_t* q_amend_path(ray_t* d, const int64_t* path, int64_t plen,
                           ray_t* f, ray_t* y, int64_t yi) {
    if (plen == 0) return q_amend_step(f, d, y, yi);
    int64_t pos = path[0];
    if (!(ray_is_vec(d) || d->type == RAY_LIST) || pos < 0 || pos >= ray_len(d))
        return ray_error("index", NULL);
    ray_t* work = q_explode(d);
    if (!work || RAY_IS_ERR(work)) return work;
    ray_t* child = ray_list_get(work, pos);          /* borrowed */
    ray_t* nc = q_amend_path(child, path + 1, plen - 1, f, y, yi);
    if (!nc || RAY_IS_ERR(nc)) { ray_release(work); return nc; }
    work = ray_list_set(work, pos, nc);
    ray_release(nc);
    ray_t* c = q_collapse_list(work);
    ray_release(work);
    return c;
}

/* Cross-sectional deep amend: idx items are vectors; amend every path in the
 * cartesian product idx[0] x idx[1] x ... sequentially (repeats accumulate).
 * Core rows use scalar/unary updates (y broadcast whole). */
static ray_t* q_amend_cross(ray_t* d, ray_t* idx, ray_t* f, ray_t* y) {
    int64_t rank = ray_len(idx);
    if (rank < 1 || rank > 8) return ray_error("rank", ".: cross-section 1..8 deep");
    ray_t* dims[8]; int64_t dn[8];
    for (int64_t a = 0; a < rank; a++) {
        ray_t* col = q_elem_at(idx, a);              /* owned; vector or atom */
        if (!col || RAY_IS_ERR(col)) { for (int64_t b=0;b<a;b++) ray_release(dims[b]); return col; }
        dims[a] = col; dn[a] = ray_is_atom(col) ? 1 : ray_len(col);
    }
    int64_t total = 1; for (int64_t a = 0; a < rank; a++) total *= dn[a];
    ray_t* acc = d; ray_retain(acc);
    for (int64_t t = 0; t < total; t++) {
        int64_t path[8], rem = t; int bad = 0;
        for (int64_t a = rank - 1; a >= 0; a--) {
            int64_t ix = dn[a] ? rem % dn[a] : 0; if (dn[a]) rem /= dn[a];
            if (ray_is_atom(dims[a])) { if (!q_idx_int(dims[a], &path[a])) bad = 1; }
            else { ray_t* p = q_elem_at(dims[a], ix); if (!q_idx_int(p, &path[a])) bad = 1; ray_release(p); }
        }
        ray_t* na = bad ? ray_error("index", NULL)
                        : q_amend_path(acc, path, rank, f, y, -1);
        ray_release(acc);
        if (!na || RAY_IS_ERR(na)) { for (int64_t a=0;a<rank;a++) ray_release(dims[a]); return na; }
        acc = na;
    }
    for (int64_t a = 0; a < rank; a++) ray_release(dims[a]);
    return acc;
}

/* Amend (deep) — .[d;i;f] / .[d;i;v;vy] / .[d;();f...] (whole). */
static ray_t* q_amend_dot(ray_t* d, ray_t* idx, ray_t* f, ray_t* y) {
    if (RAY_IS_NULL(idx)) {                          /* .[d;();u] == u[d] */
        if (q_is_assign(f)) {
            if (!y) return ray_error("type", ".: assign (:) needs a value");
            ray_retain(y); return y;
        }
        if (y) return call_fn2(f, d, y);
        return call_fn1(f, d);
    }
    int64_t ilen = ray_is_atom(idx) ? 1 : ray_len(idx);
    int all_atom = 1;
    if (!ray_is_atom(idx) && idx->type == RAY_LIST) {
        ray_t** e = (ray_t**)ray_data(idx);
        for (int64_t k = 0; k < ilen; k++)
            if (e[k] && !ray_is_atom(e[k])) { all_atom = 0; break; }
    }
    if (all_atom) {
        if (ilen > 8) return ray_error("rank", ".: index path too deep");
        int64_t path[8];
        for (int64_t k = 0; k < ilen; k++) {
            int ok;
            if (ray_is_atom(idx)) ok = q_idx_int(idx, &path[k]);
            else { ray_t* p = q_elem_at(idx, k); ok = q_idx_int(p, &path[k]); ray_release(p); }
            if (!ok) return ray_error("index", NULL);
        }
        return q_amend_path(d, path, ilen, f, y, -1);
    }
    return q_amend_cross(d, idx, f, y);              /* items are vectors */
}

/* Trap tail: r is g's (owned) result; on error run/return the handler e. */
static ray_t* q_trap_finish(ray_t* r, ray_t* e) {
    if (!r || !RAY_IS_ERR(r)) return r;              /* success passes through */
    ray_t* text = q_registry_sig_take();             /* full signal text, owned */
    if (!text) { const char* c = ray_err_code(r); text = ray_str(c ? c : "", c ? strlen(c) : 0); }
    ray_error_free(r);
    if (!q_is_fn_value(e)) { ray_release(text); ray_retain(e); return e; }
    ray_t* hr = call_fn1(e, text);
    ray_release(text);
    return hr;
}

/* Trap At — @[f;fx;e] == .[f;enlist fx;e]. */
static ray_t* q_trap(ray_t* f, ray_t* x, ray_t* e) {
    q_registry_sig_clear();                          /* drop stale payload */
    ray_t* args[1] = { x };
    ray_t* r = q_call_n(f, args, 1);
    return q_trap_finish(r, e);
}

/* Trap — .[g;gx;e]: gx is the argument LIST; spread-apply g over it. */
static ray_t* q_trap_dot(ray_t* g, ray_t* gx, ray_t* e) {
    q_registry_sig_clear();
    if (!gx || (!ray_is_vec(gx) && gx->type != RAY_LIST))
        return ray_error("type", ".: trap args must be a list");
    int64_t k = ray_len(gx);
    if (k < 1 || k > 8) return ray_error("rank", ".: 1..8 trap args");
    ray_t* a[8];
    for (int64_t i = 0; i < k; i++) a[i] = q_elem_at(gx, i);   /* owned */
    ray_t* r = q_call_n(g, a, k);
    for (int64_t i = 0; i < k; i++) ray_release(a[i]);
    return q_trap_finish(r, e);
}

/* q `f@x` — Apply At / Index At (ref/apply.md).  A callable f invokes with
 * the single argument; everything else (vector, list, dict, table, 104h
 * carrier) delegates to q_apply_noun — identical semantics to `f[x]`/`f x`. */
static ray_t* q_at_apply2(ray_t* f, ray_t* x) {
    if (f && (f->type == RAY_UNARY || f->type == RAY_BINARY || f->type == RAY_VARY)
          && (f->attrs & RAY_FN_SPECIAL_FORM))
        return ray_error("type", "@: special forms cannot be applied");
    if (f && f->type == RAY_UNARY)
        return ((ray_unary_fn)(uintptr_t)f->i64)(x);
    if (f && f->type == RAY_VARY) {
        ray_t* one[1] = { x };
        return ((ray_vary_fn)(uintptr_t)f->i64)(one, 1);
    }
    if (f && f->type == RAY_BINARY)
        return ray_error("rank", "@: unary application of a binary verb");
    ray_t* args[1] = { x };
    ray_t* r = q_apply_noun(f, args, 1);
    return r ? r : ray_error("type", "@: not applicable");
}

/* @ VARY: 2 args Apply/Index; 3 args Trap-At (callable) or Amend-At (data);
 * 4 args Amend-At (quaternary). */
static ray_t* q_at_wrap(ray_t** args, int64_t n) {
    if (n == 2) return q_at_apply2(args[0], args[1]);
    if (n == 3) {
        if (q_is_fn_value(args[0])) return q_trap(args[0], args[1], args[2]);
        return q_amend_at(args[0], args[1], args[2], NULL);
    }
    if (n == 4) return q_amend_at(args[0], args[1], args[2], args[3]);
    return ray_error("rank", "@: got %lld args", (long long)n);
}

/* q `v . vx` — Apply / Index (ref/apply.md): the rhs is the ARGUMENT LIST —
 * a rank-n callable spread-calls over vx's n items; a noun depth-indexes
 * (m . 1 2 is m[1;2]).  Atom rhs is not a list -> 'type (kdb wants a list). */
static ray_t* q_dot_apply(ray_t* f, ray_t* a) {
    if (!a || (!ray_is_vec(a) && a->type != RAY_LIST))
        return ray_error("type", ".: rhs must be an argument list");
    if (f && (f->type == RAY_UNARY || f->type == RAY_BINARY || f->type == RAY_VARY)
          && (f->attrs & RAY_FN_SPECIAL_FORM))
        return ray_error("type", ".: special forms cannot be applied");
    int64_t n = ray_len(a);
    if (n < 1 || n > 8) return ray_error("rank", ".: 1..8 arguments");
    ray_t* args[8];
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        args[i] = ray_at_fn(a, ia);
        ray_release(ia);
        if (!args[i] || RAY_IS_ERR(args[i])) {
            ray_t* err = args[i];
            for (int64_t j = 0; j < i; j++) ray_release(args[j]);
            return err ? err : ray_error("type", ".: bad argument list");
        }
    }
    ray_t* r;
    if (f && f->type == RAY_UNARY && n == 1)
        r = ((ray_unary_fn)(uintptr_t)f->i64)(args[0]);
    else if (f && f->type == RAY_BINARY && n == 2)
        r = ((ray_binary_fn)(uintptr_t)f->i64)(args[0], args[1]);
    else if (f && f->type == RAY_VARY)
        r = ((ray_vary_fn)(uintptr_t)f->i64)(args, n);
    else if (f && (f->type == RAY_UNARY || f->type == RAY_BINARY))
        r = ray_error("rank", ".: argument count does not match the verb's rank");
    else {
        r = q_apply_noun(f, args, n);
        if (!r) r = ray_error("type", ".: not applicable");
    }
    for (int64_t j = 0; j < n; j++) ray_release(args[j]);
    return r;
}

/* . VARY: 2 args Apply/Index; 3 args Trap (callable) or Amend deep (data);
 * 4 args Amend deep (quaternary). */
static ray_t* q_dot_wrap(ray_t** args, int64_t n) {
    if (n == 2) return q_dot_apply(args[0], args[1]);
    if (n == 3) {
        if (q_is_fn_value(args[0])) return q_trap_dot(args[0], args[1], args[2]);
        return q_amend_dot(args[0], args[1], args[2], NULL);
    }
    if (n == 4) return q_amend_dot(args[0], args[1], args[2], args[3]);
    return ray_error("rank", ".: got %lld args", (long long)n);
}

/* q `f each x` — rayfall map, then collapse the boxed result to a simple
 * vector (kdb: `neg each 1 2 3` is -1 -2 -3, type 7h, not a general list). */
static ray_t* q_each_wrap(ray_t* f, ray_t* x) {
    /* table arm: q iterates a table's ROWS (`count each t` -> 2 2 2) — each
     * row is the t[i] row dict; keyed tables are a deferred cell. */
    if (x && x->type == RAY_TABLE) {
        int64_t n = ray_table_nrows(x);
        ray_t* outl = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(outl)) return outl;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* row = ray_at_fn(x, ia);           /* owned row dict */
            ray_release(ia);
            if (!row || RAY_IS_ERR(row)) { ray_release(outl); return row; }
            ray_t* r = call_fn1(f, row);
            ray_release(row);
            if (!r || RAY_IS_ERR(r)) { ray_release(outl); return r; }
            outl = ray_list_append(outl, r);         /* retains */
            ray_release(r);
            if (RAY_IS_ERR(outl)) return outl;
        }
        ray_t* c = q_collapse_list(outl);
        ray_release(outl);
        return c;
    }
    ray_t* args[2] = { f, x };
    ray_t* r = ray_map_fn(args, 2);
    if (!r || RAY_IS_ERR(r)) return r;
    ray_t* c = q_collapse_list(r);
    ray_release(r);
    return c;
}

/* ===== q iterators: each-both ' , each-prior ': , over / , scan \ ==========
 * (wave-2 adverb completion — docs/superpowers/plans/2026-07-06-q-adverbs.md).
 * These are internal (spelling-less) HOF VALUES that q_lower embeds at adverb
 * heads, plus the runtime cores the over/scan/prior/peach/deltas/differ
 * keyword wrappers delegate to.  Every function operand is applied through
 * call_fn1/call_fn2, which fall through to q_apply_noun for 100h lambda and
 * 104h projection carriers — so lambdas, native ops and projections all work.
 *
 * Rank of a q value: 1 monadic, 2 dyadic, -1 ambiguous (native vary). */
static int q_fn_rank(ray_t* f) {
    if (!f) return -1;
    switch (f->type) {
    case RAY_UNARY:  return 1;
    case RAY_BINARY: return 2;
    case RAY_VARY:   return -1;
    case RAY_LAMBDA: return (int)ray_len(LAMBDA_PARAMS(f));
    default: break;
    }
    q_deriv_kind k = q_deriv_kind_of(f);
    if (k == Q_DERIV_LAMBDA) return q_deriv_valence(f);
    if (k == Q_DERIV_MONAD)  return 1;
    if (k == Q_DERIV_PROJ) {
        uint64_t m = q_deriv_hole_mask(f);
        int c = 0; while (m) { c += (int)(m & 1u); m >>= 1; }
        return c;              /* effective rank = open holes */
    }
    return -1;
}

/* True iff x is a callable q value (native fn or carrier) — distinguishes the
 * `while` test-function argument of `/` `\` from a numeric do-count. */
static int q_is_fn_value(ray_t* x) {
    if (!x) return 0;
    if (x->type == RAY_UNARY || x->type == RAY_BINARY ||
        x->type == RAY_VARY  || x->type == RAY_LAMBDA) return 1;
    return q_deriv_kind_of(x) != Q_DERIV_NONE;
}

/* Whole-value equivalence (converge stop test) — atom_eq handles atoms,
 * vectors and structural lists. */
static int q_values_match(ray_t* a, ray_t* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    return atom_eq(a, b);
}

/* v[i] as an owned atom/element (borrowed v). */
static ray_t* q_elem_at(ray_t* v, int64_t i) {
    ray_t* ia = ray_i64(i);
    ray_t* e  = ray_at_fn(v, ia);   /* owned */
    ray_release(ia);
    return e;
}

/* Apply f to k args (borrowed).  1/2 route via call_fn1/2 (carrier-aware);
 * k>=3 via the noun dispatcher (lambda/proj carriers) or a native vary. */
static ray_t* q_call_n(ray_t* f, ray_t** a, int64_t k) {
    if (k == 1) return call_fn1(f, a[0]);
    if (k == 2) return call_fn2(f, a[0], a[1]);
    if (f && f->type == RAY_VARY) return ((ray_vary_fn)(uintptr_t)f->i64)(a, k);
    ray_t* r = q_apply_noun(f, a, k);
    if (r) return r;
    return ray_error("rank", "each-both: cannot apply to %lld args", (long long)k);
}

/* ---- each-both  x f'y ------------------------------------------------------ */
static ray_t* q_eachboth_apply(ray_t* f, ray_t** ops, int64_t k);

static int q_op_is_dict(ray_t* v) { return v && v->type == RAY_DICT; }

/* dict each-both (binary): keys come from the dict side; a non-dict operand
 * pairs with the dict's VALUES (kdb: d+'10 20 conforms values, keys kept).
 * Mixed operands previously dispatched ray_dict_vals(non-dict)=NULL straight
 * into a crash (codex round-2 P1). */
static ray_t* q_eachboth_dict(ray_t* f, ray_t* x, ray_t* y) {
    ray_t* kd = q_op_is_dict(x) ? x : y;     /* key donor */
    ray_t* xk = ray_dict_keys(kd);           /* borrowed */
    if (!xk) return ray_error("type", "each-both: malformed dictionary");
    ray_t* ops[2] = { q_op_is_dict(x) ? ray_dict_vals(x) : x,
                      q_op_is_dict(y) ? ray_dict_vals(y) : y };
    ray_t* rv = q_eachboth_apply(f, ops, 2);
    if (!rv || RAY_IS_ERR(rv)) return rv;
    ray_retain(xk);
    return ray_dict_new(xk, rv);             /* consumes keys + vals */
}
static int q_op_is_atom(ray_t* v) { return v && ray_is_atom(v) && !q_op_is_dict(v); }

static ray_t* q_eachboth_apply(ray_t* f, ray_t** ops, int64_t k) {
    int any_dict = 0, all_atom = 1;
    for (int64_t j = 0; j < k; j++) {
        if (q_op_is_dict(ops[j])) any_dict = 1;
        if (!q_op_is_atom(ops[j])) all_atom = 0;
    }
    if (any_dict && k == 2) return q_eachboth_dict(f, ops[0], ops[1]);
    if (all_atom) return q_call_n(f, ops, k);      /* all atoms -> one result */

    int64_t L = -1;
    for (int64_t j = 0; j < k; j++) {
        if (!q_op_is_atom(ops[j])) {
            int64_t lj = ray_len(ops[j]);
            if (L < 0) L = lj;
            else if (L != lj) return ray_error("length", "each-both: length mismatch");
        }
    }
    if (L < 0) L = 1;
    ray_t* out = ray_list_new(L > 0 ? L : 1);
    for (int64_t i = 0; i < L; i++) {
        ray_t* a[16]; uint32_t owned = 0;
        int64_t kk = k < 16 ? k : 16;
        for (int64_t j = 0; j < kk; j++) {
            if (!q_op_is_atom(ops[j])) { a[j] = q_elem_at(ops[j], i); owned |= (1u << j); }
            else                       { a[j] = ops[j]; }   /* atom broadcast */
        }
        ray_t* r = q_call_n(f, a, kk);
        for (int64_t j = 0; j < kk; j++) if (owned & (1u << j)) ray_release(a[j]);
        if (!r || RAY_IS_ERR(r)) { ray_release(out); return r ? r : ray_error("type", NULL); }
        out = ray_list_append(out, r);
        ray_release(r);
    }
    ray_t* c = q_collapse_list(out);
    ray_release(out);
    return c;
}

/* internal each-both value: args[0]=f, args[1..] operands. */
static ray_t* q_eachboth_wrap(ray_t** args, int64_t n) {
    if (n < 2) return ray_error("rank", "each-both: needs a function and operand");
    return q_eachboth_apply(args[0], args + 1, n - 1);
}

/* ---- each-prior  (f':)x  /  s f':x ---------------------------------------- */
/* Seed for the UNARY form: operator identity if known to q, else `first 0#x`
 * (typed null of the argument's element type) — ref/maps.md 259-279. */
static ray_t* q_prior_seed(ray_t* f, ray_t* x) {
    q_provenance_t pv;
    if (q_registry_provenance(f, &pv) && pv.spelling && pv.spelling[0] &&
        pv.spelling[1] == '\0') {
        char g = pv.spelling[0];
        if (g == '+' || g == '-') return ray_i64(0);       /* I(+) = I(-) = 0 */
        if (g == '*' || g == '%') return ray_i64(1);       /* I(*) = I(%) = 1 */
        if (g == ',')             return ray_list_new(0);  /* I(,) = ()       */
    }
    if (ray_is_vec(x))    return ray_typed_null((int8_t)(-x->type));
    if (x && ray_is_atom(x) && x->type != RAY_LIST)
        return ray_typed_null(x->type);
    ray_retain(RAY_NULL_OBJ);
    return RAY_NULL_OBJ;
}

/* result[0]=f(x0,seed); result[i]=f(xi,x[i-1]).  Borrows f/seed/x. */
static ray_t* q_prior_over_vec(ray_t* f, ray_t* seed, ray_t* x) {
    if (!x || (!ray_is_vec(x) && x->type != RAY_LIST)) {
        if (x && ray_is_atom(x)) return call_fn2(f, x, seed);
        return ray_error("type", "each-prior: expected a list");
    }
    int64_t L = ray_len(x);
    ray_t* out = ray_list_new(L > 0 ? L : 1);
    ray_t* prev = seed; int prev_owned = 0;
    for (int64_t i = 0; i < L; i++) {
        ray_t* cur = q_elem_at(x, i);            /* owned */
        ray_t* r   = call_fn2(f, cur, prev);
        if (prev_owned) ray_release(prev);
        if (!r || RAY_IS_ERR(r)) { ray_release(cur); ray_release(out); return r ? r : ray_error("type", NULL); }
        out = ray_list_append(out, r);
        ray_release(r);
        prev = cur; prev_owned = 1;               /* current becomes next prior */
    }
    if (prev_owned) ray_release(prev);
    ray_t* c = q_collapse_list(out);
    ray_release(out);
    return c;
}

/* internal each-prior value: n==2 (f':)x unary; n==3 s f':x seeded. */
static ray_t* q_prior_wrap(ray_t** args, int64_t n) {
    if (n < 2 || n > 3) return ray_error("rank", "each-prior: bad arity");
    ray_t* f = args[0];
    ray_t* x = args[n - 1];
    ray_t* seed; int seed_owned = 0;
    if (n == 3) seed = args[1];
    else { seed = q_prior_seed(f, x); seed_owned = 1; }
    ray_t* r;
    if (x && x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);
        ray_t* rv = q_prior_over_vec(f, seed, ray_dict_vals(x));
        if (!rv || RAY_IS_ERR(rv)) r = rv;
        else { ray_retain(k); r = ray_dict_new(k, rv); }
    } else {
        r = q_prior_over_vec(f, seed, x);
    }
    if (seed_owned) ray_release(seed);
    return r;
}

/* deltas x == (-':)x ; differ x == not (~':)x  (ref/maps.md prior section). */
static ray_t* q_deltas_wrap(ray_t* x) {
    ray_t* sub = q_registry_lookup_name("-", 1, Q_DYADIC);   /* borrowed */
    if (!sub) return ray_error("type", "deltas: no subtract");
    ray_t* a[2] = { sub, x };
    return q_prior_wrap(a, 2);
}
static ray_t* q_differ_wrap(ray_t* x) {
    ray_t* mt = q_registry_lookup_name("~", 1, Q_DYADIC);    /* borrowed */
    if (!mt) return ray_error("type", "differ: no match");
    ray_t* a[2] = { mt, x };
    ray_t* eq = q_prior_wrap(a, 2);                          /* (~':)x */
    if (!eq || RAY_IS_ERR(eq)) return eq;
    ray_t* notv = ray_env_get(ray_sym_intern_runtime("not", 3));  /* borrowed */
    if (!notv) return eq;
    ray_t* r = call_fn1(notv, eq);
    ray_release(eq);
    return r;
}

/* ---- over / scan  (converge, do, while, and reduce) ----------------------- */
static int q_truthy(ray_t* v) {
    if (!v) return 0;
    if (v->type == -RAY_BOOL) return v->b8 != 0;
    if (ray_is_atom(v) && is_numeric(v)) return as_f64(v) != 0.0;
    return 0;
}

/* Converge: apply f until the result matches the previous OR the initial x.
 * collect=1 keeps every step (scan), else returns the last (over). */
static ray_t* q_converge(ray_t* f, ray_t* x, int collect) {
    ray_t* first = x; ray_retain(first);
    ray_t* cur   = x; ray_retain(cur);
    ray_t* acc   = collect ? ray_list_new(0) : NULL;
    if (collect) acc = ray_list_append(acc, cur);
    int64_t guard = 0;
    for (;;) {
        ray_t* nxt = call_fn1(f, cur);
        if (!nxt || RAY_IS_ERR(nxt)) {
            ray_release(first); ray_release(cur); if (acc) ray_release(acc);
            return nxt ? nxt : ray_error("type", NULL);
        }
        if (q_values_match(nxt, cur) || q_values_match(nxt, first)) { ray_release(nxt); break; }
        if (collect) acc = ray_list_append(acc, nxt);
        ray_release(cur);
        cur = nxt;
        /* kdb has NO cap here (`(not/) 42` hangs until interrupt — doc-pinned
         * "never returns!"); the cap is openq's deliberate divergence.  1e6 keeps
         * ~4 orders of magnitude of headroom over any real fixpoint (tens of
         * iterations) while making the pathological oscillators cheap: at 1e8 the
         * accumulators suite burned ~50s CPU under ASan to produce this same
         * 'limit (2026-07-09). */
        if (++guard > 1000000) {
            ray_release(first); ray_release(cur); if (acc) ray_release(acc);
            return ray_error("limit", "converge: no fixed point");
        }
    }
    ray_release(first);
    if (collect) { ray_release(cur); ray_t* c = q_collapse_list(acc); ray_release(acc); return c; }
    return cur;
}

/* Do: apply f exactly cnt times to x (n f/x).  collect keeps each step. */
static ray_t* q_ntimes(ray_t* f, int64_t cnt, ray_t* x, int collect) {
    if (cnt < 0) cnt = 0;
    ray_t* cur = x; ray_retain(cur);
    ray_t* acc = NULL;
    if (collect) { acc = ray_list_new(cnt + 1); acc = ray_list_append(acc, cur); }
    for (int64_t i = 0; i < cnt; i++) {
        ray_t* nxt = call_fn1(f, cur);
        ray_release(cur);
        if (!nxt || RAY_IS_ERR(nxt)) { if (acc) ray_release(acc); return nxt ? nxt : ray_error("type", NULL); }
        cur = nxt;
        if (collect) acc = ray_list_append(acc, cur);
    }
    if (collect) { ray_release(cur); ray_t* c = q_collapse_list(acc); ray_release(acc); return c; }
    return cur;
}

/* While: apply f while test(cur) holds (test f/x).  collect keeps each step. */
static ray_t* q_while(ray_t* f, ray_t* test, ray_t* x, int collect) {
    ray_t* cur = x; ray_retain(cur);
    ray_t* acc = NULL;
    if (collect) { acc = ray_list_new(0); acc = ray_list_append(acc, cur); }
    int64_t guard = 0;
    for (;;) {
        ray_t* t = call_fn1(test, cur);
        if (!t || RAY_IS_ERR(t)) { ray_release(cur); if (acc) ray_release(acc); return t ? t : ray_error("type", NULL); }
        int go = q_truthy(t);
        ray_release(t);
        if (!go) break;
        ray_t* nxt = call_fn1(f, cur);
        ray_release(cur);
        if (!nxt || RAY_IS_ERR(nxt)) { if (acc) ray_release(acc); return nxt ? nxt : ray_error("type", NULL); }
        cur = nxt;
        if (collect) acc = ray_list_append(acc, cur);
        if (++guard > 100000000) { ray_release(cur); if (acc) ray_release(acc); return ray_error("limit", "while: no termination"); }
    }
    if (collect) { ray_release(cur); ray_t* c = q_collapse_list(acc); ray_release(acc); return c; }
    return cur;
}

/* Seeded scan  x f\y  (kept minimal — ray_scan_fn has no seed slot). */
static ray_t* q_seeded_scan(ray_t* f, ray_t* seed, ray_t* x) {
    if (!x || (!ray_is_vec(x) && x->type != RAY_LIST)) return ray_error("type", "scan: expected a list");
    int64_t L = ray_len(x);
    ray_t* out = ray_list_new(L > 0 ? L : 1);
    ray_t* acc = seed; ray_retain(acc);
    for (int64_t i = 0; i < L; i++) {
        ray_t* cur = q_elem_at(x, i);
        ray_t* nxt = call_fn2(f, acc, cur);
        ray_release(acc); ray_release(cur);
        if (!nxt || RAY_IS_ERR(nxt)) { ray_release(out); return nxt ? nxt : ray_error("type", NULL); }
        out = ray_list_append(out, nxt);
        acc = nxt; ray_retain(acc);
    }
    ray_release(acc);
    ray_t* c = q_collapse_list(out); ray_release(out); return c;
}

/* `/` over — reduce / converge / do / while by operand shape and f rank. */
static ray_t* q_over_wrap(ray_t** args, int64_t n) {
    ray_t* f = args[0];
    int rank = q_fn_rank(f);
    if (n == 2) {
        ray_t* x = args[1];
        if (rank == 1) return q_converge(f, x, 0);
        ray_t* fa[2] = { f, x };
        return ray_fold_fn(fa, 2);                       /* reduce */
    }
    if (n == 3) {
        ray_t* a = args[1], *x = args[2];
        if (q_is_fn_value(a))  return q_while(f, a, x, 0);
        if (rank == 1)         return q_ntimes(f, as_i64(a), x, 0);
        ray_t* fa[3] = { f, a, x };
        return ray_fold_fn(fa, 3);                       /* seeded reduce */
    }
    return ray_error("rank", "over: bad arity");
}

/* `\` scan — like over but every step is retained. */
static ray_t* q_scan_wrap(ray_t** args, int64_t n) {
    ray_t* f = args[0];
    int rank = q_fn_rank(f);
    if (n == 2) {
        ray_t* x = args[1];
        if (rank == 1) return q_converge(f, x, 1);
        ray_t* fa[2] = { f, x };
        ray_t* r = ray_scan_fn(fa, 2);
        if (!r || RAY_IS_ERR(r)) return r;
        ray_t* c = q_collapse_list(r); ray_release(r); return c;
    }
    if (n == 3) {
        ray_t* a = args[1], *x = args[2];
        if (q_is_fn_value(a))  return q_while(f, a, x, 1);
        if (rank == 1)         return q_ntimes(f, as_i64(a), x, 1);
        return q_seeded_scan(f, a, x);
    }
    return ray_error("rank", "scan: bad arity");
}

/* over/scan/prior keyword dyadic wrappers `f over x` / `f scan x` /
 * `f prior x` — delegate to the n==2 core (peach reuses q_each_wrap). */
static ray_t* q_over_kw(ray_t* f, ray_t* x)  { ray_t* a[2] = { f, x }; return q_over_wrap(a, 2); }
static ray_t* q_scan_kw(ray_t* f, ray_t* x)  { ray_t* a[2] = { f, x }; return q_scan_wrap(a, 2); }
static ray_t* q_prior_kw(ray_t* f, ray_t* x) { ray_t* a[2] = { f, x }; return q_prior_wrap(a, 2); }

/* Build a BINARY derived-verb carrier at EVAL time: `hof` with `f` bound in
 * slot 0 and two data operands open.  Used to lower `(f/:)` / `(f\:)` when the
 * operand f is an expression (a lambda) that must be EVALUATED to a value
 * first — a lower-time q_proj would capture the raw `(q.fn …)` tree.  `x f/: y`
 * then == map-right(f;x;y), which lets a stacked outer adverb (`f/:\:`) drive
 * it through map-left. */
static ray_t* q_mkderiv2(ray_t* hof, ray_t* f) {
    ray_t* args[3] = { f, NULL, NULL };
    return q_proj_new(hof, args, 3, 0x6u, 2);
}

static ray_t* g_scan_value     = NULL;
static ray_t* g_over_value     = NULL;
static ray_t* g_eachboth_value = NULL;
static ray_t* g_prior_value    = NULL;
static ray_t* g_mkderiv2_value = NULL;

ray_t* q_registry_scan_value(void)     { return g_scan_value;     }  /* borrowed */
ray_t* q_registry_over_value(void)     { return g_over_value;     }  /* borrowed */
ray_t* q_registry_eachboth_value(void) { return g_eachboth_value; }  /* borrowed */
ray_t* q_registry_prior_value(void)    { return g_prior_value;    }  /* borrowed */
ray_t* q_registry_mkderiv2_value(void) { return g_mkderiv2_value; }  /* borrowed */

/* ---- shared right-to-left CONTEXT builder (list + table def) --------------
 *
 * THE MANDATE (specs/2026-07-04-table-def.md): list definition `(…)` and table
 * definition `([] …)` are ONE mechanism, not two.  Evaluating either opens an
 * env scope, processes the element expressions RIGHT-TO-LEFT (so `x:e` binds x
 * INTO the scope before a leftward bare `x` RESOLVES from it — this is WHY
 * `(aa; aa:11 12 13)` yields the value twice and `(bb:11 12 13; bb)` errors
 * `'bb`), then pops the scope and assembles: `(…)` → the list of element values
 * (then the existing collapse-to-vector); `([] …)` → a table whose columns are
 * the per-element assignment targets.
 *
 * Both constructor heads are RAY_FN_SPECIAL_FORM: rayforce's VARY arg-eval is
 * LEFT-to-right, and a special form is the only seam that hands a builtin the
 * raw (unevaluated) element trees — a hard prerequisite for right-to-left.
 * q_lower lowers a plain `:` inside a ctx literal to `let` (writes the pushed
 * frame), so the assignments are scoped, not leaked to the global env. */

static int64_t q_expr_rightmost_name(ray_t* el) {
    if (!el) return -1;
    if (el->type == -RAY_SYM && !(el->attrs & 0x20 /* Q_ATTR_QUOTED */))
        return el->i64;
    if (el->type == RAY_LIST) {
        ray_t** e = (ray_t**)ray_data(el);
        for (int64_t i = ray_len(el) - 1; i >= 0; i--) {
            int64_t id = q_expr_rightmost_name(e[i]);
            if (id >= 0) return id;
        }
    }
    return -1;
}

/* If `el` is a lowered assignment node `(set/let name val)`, return the target
 * sym-id; else if `el` is a bare unquoted name-ref sym, return its id (a bare
 * column reference); else use the expression's rightmost name token when one is
 * available, falling back to generated `x`, `x1`, ... names. */
static int64_t q_ctx_colname(ray_t* el) {
    if (!el) return -1;
    if (el->type == -RAY_SYM && !(el->attrs & 0x20 /* Q_ATTR_QUOTED */))
        return el->i64;
    if (el->type == RAY_LIST && ray_len(el) == 3) {
        ray_t** e = (ray_t**)ray_data(el);
        ray_t* h = e[0];
        if (h && h->type == RAY_BINARY) {
            const char* nm = ray_fn_name(h);
            if (nm && (strcmp(nm, "set") == 0 || strcmp(nm, "let") == 0) &&
                e[1] && e[1]->type == -RAY_SYM && !(e[1]->attrs & 0x20))
                return e[1]->i64;
        }
    }
    return q_expr_rightmost_name(el);
}

static int64_t q_ctx_generated_name(int64_t ordinal) {
    if (ordinal == 0) return ray_sym_intern_runtime("x", 1);
    char buf[32];
    int n = snprintf(buf, sizeof buf, "x%lld", (long long)ordinal);
    return ray_sym_intern_runtime(buf, (size_t)n);
}

/* The shared builder.  `elems[0..n)` are the UNEVALUATED element trees. */
static ray_t* q_ctx_build(ray_t** elems, int64_t n, int as_table) {
    if (ray_env_push_scope() != RAY_OK) return ray_error("oom", NULL);

    ray_t**  vals  = (n > 0) ? (ray_t**)calloc((size_t)n, sizeof(ray_t*))   : NULL;
    int64_t* names = (as_table && n > 0)
                        ? (int64_t*)malloc((size_t)n * sizeof(int64_t)) : NULL;
    if (n > 0 && (!vals || (as_table && !names))) {
        free(vals); free(names); ray_env_pop_scope();
        return ray_error("wsfull", "ctx: out of memory");
    }

    ray_t* err = NULL;
    for (int64_t i = n - 1; i >= 0; i--) {
        if (as_table) names[i] = q_ctx_colname(elems[i]);
        ray_t* v = ray_eval(elems[i]);            /* binds `x:e` into the scope */
        if (!v) v = ray_error("type", "ctx: null element");
        if (RAY_IS_ERR(v)) { err = v; break; }
        vals[i] = v;
    }

    ray_env_pop_scope();

    if (err) {
        for (int64_t i = 0; i < n; i++) if (vals[i]) ray_release(vals[i]);
        free(vals); free(names);
        return err;
    }

    ray_t* out;
    if (!as_table) {
        ray_t* l = ray_list_fn(vals, n);          /* borrows; retains each */
        if (l && !RAY_IS_ERR(l)) { out = q_collapse_list(l); ray_release(l); }
        else                     { out = l; }
    } else {
        /* Row count comes from the vector/list columns, which must all share one
         * length (mismatch -> 'length).  A scalar-atom column BROADCASTS to that
         * length (kdb: `([]a:1 2 3;b:0)` -> b is 0 0 0); all-scalar -> 1 row. */
        int64_t nrows = -1;
        ray_t* err2 = NULL;
        for (int64_t i = 0; i < n; i++) {
            ray_t* col = vals[i];
            if (col && (ray_is_vec(col) || col->type == RAY_LIST)) {
                int64_t clen = ray_len(col);
                if (nrows < 0) nrows = clen;
                else if (clen != nrows) {
                    err2 = ray_error("length", "([]…): column length mismatch");
                    break;
                }
            } else if (!col || col->type >= 0) {
                err2 = ray_error("type", "([]…): column must be a vector or list");
                break;
            }
        }
        if (err2) { out = err2; }
        else {
            if (nrows < 0) nrows = 1;
            out = ray_table_new(n);
            int64_t used_stack[64];
            int64_t* used = (n <= 64) ? used_stack : (int64_t*)malloc((size_t)n * sizeof(int64_t));
            if (!used) {
                ray_release(out);
                out = ray_error("wsfull", "([]...): out of memory");
            }
            int64_t gen = 0;
            for (int64_t i = 0; i < n && !RAY_IS_ERR(out); i++) {
                int64_t nm;
                if (names[i] < 0) {
                    /* Anonymous column: openq invents x, x1, … and dedups to a
                     * free name (the user supplied none, so this never errors). */
                    nm = q_ctx_generated_name(gen++);
                    nm = q_name_dedup(nm, used, i, 0);
                } else {
                    /* User-given name: taken VERBATIM — no .Q.id-style sanitize
                     * or reserved-word repair (that is opt-in via .Q.id).  A
                     * duplicate is an error, matching kdb (not silently renamed). */
                    nm = names[i];
                    for (int64_t j = 0; j < i; j++) {
                        if (used[j] == nm) {
                            ray_release(out);
                            out = ray_error("dup", "([]…): duplicate column name");
                            break;
                        }
                    }
                    if (RAY_IS_ERR(out)) break;
                }
                used[i] = nm;
                ray_t* col = vals[i]; int owned = 0;
                if (col && col->type < 0) {              /* scalar -> broadcast */
                    ray_t* nn = ray_i64(nrows);
                    col = ray_take_fn(col, nn); ray_release(nn); owned = 1;
                    if (!col || RAY_IS_ERR(col)) {
                        ray_release(out); out = col ? col : ray_error("type", "broadcast");
                        break;
                    }
                }
                out = ray_table_add_col(out, nm, col);
                if (owned) ray_release(col);
            }
            if (used && used != used_stack) free(used);
        }
    }

    for (int64_t i = 0; i < n; i++) if (vals[i]) ray_release(vals[i]);
    free(vals); free(names);
    return out;
}

/* q paren-list literal `(1;2;3)` head — see q_ctx_build.  The parser embeds
 * this value at the head of every multi-element paren list, which is what
 * DISAMBIGUATES a literal from the shape-identical bracket-index call (v;i) —
 * the distinction only exists at parse time. */
static ray_t* q_list_build(ray_t** args, int64_t n) {
    return q_ctx_build(args, n, 0);
}

/* q table literal `([] a:…; b:…)` head — see q_ctx_build. */
static ray_t* q_table_build(ray_t** args, int64_t n) {
    return q_ctx_build(args, n, 1);
}

/* q keyed-table literal `([k1:…;k2:…] v1:…; v2:…)` head.  args[0] is an int
 * atom = the KEY column count; args[1..] are the column-def trees (key columns
 * first, then value columns).  All columns are built as ONE table (so value
 * columns can reference key columns via the shared build scope — q_ctx_build's
 * cross-column binding), then split into a key-columns table and a
 * value-columns table joined into a RAY_DICT: "a keyed table is just a
 * dictionary from one table to another" (q_fmt renders it `k| v`). */
static ray_t* q_keyed_table_build(ray_t** args, int64_t n) {
    if (n < 1 || !args[0] || args[0]->type != -RAY_I64)
        return ray_error("parse", "keyed-table literal: missing key count");
    int64_t nk = args[0]->i64;
    ray_t* tbl = q_ctx_build(args + 1, n - 1, 1);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;
    if (tbl->type != RAY_TABLE) { ray_release(tbl); return ray_error("type", "keyed-table literal: not a table"); }
    int64_t nc = ray_table_ncols(tbl);
    if (nk < 0 || nk > nc) { ray_release(tbl); return ray_error("length", "keyed-table literal: bad key count"); }
    ray_t* kt = ray_table_new(nk > 0 ? nk : 1);
    ray_t* vt = ray_table_new(nc - nk > 0 ? nc - nk : 1);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(kt) && !RAY_IS_ERR(vt); c++) {
        int64_t nm  = ray_table_col_name(tbl, c);
        ray_t*  col = ray_table_get_col_idx(tbl, c);   /* borrowed; add retains */
        if (c < nk) kt = ray_table_add_col(kt, nm, col);
        else        vt = ray_table_add_col(vt, nm, col);
    }
    ray_release(tbl);
    if (RAY_IS_ERR(kt)) { ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    return ray_dict_new(kt, vt);   /* consumes kt, vt */
}

static void q_select_rename_temps(ray_t* tbl, ray_t* tempnames, ray_t* realnames) {
    if (!tbl || tbl->type != RAY_TABLE ||
        !tempnames || tempnames->type != RAY_SYM ||
        !realnames || realnames->type != RAY_SYM)
        return;
    int64_t nt = ray_len(tempnames);
    int64_t nc = ray_table_ncols(tbl);
    int64_t used_stack[64];
    int64_t* used = (nc <= 64) ? used_stack : (int64_t*)malloc((size_t)nc * sizeof(int64_t));
    if (!used) return;
    for (int64_t c = 0; c < nc; c++) used[c] = ray_table_col_name(tbl, c);
    for (int64_t i = 0; i < nt; i++) {
        ray_t* ti = ray_i64(i);
        ray_t* ta = ray_at_fn(tempnames, ti);
        ray_t* ra = ray_at_fn(realnames, ti);
        ray_release(ti);
        int64_t tid = (ta && ta->type == -RAY_SYM) ? ta->i64 : -1;
        int64_t rid = (ra && ra->type == -RAY_SYM) ? ra->i64 : -1;
        if (ta) ray_release(ta);
        if (ra) ray_release(ra);
        if (tid < 0 || rid < 0) continue;
        for (int64_t c = 0; c < nc; c++) {
            if (ray_table_col_name(tbl, c) != tid) continue;
            int64_t nm = q_name_dedup(rid, used, c, 1);
            used[c] = nm;
            ray_table_set_col_name(tbl, c, nm);
            break;
        }
    }
    if (used != used_stack) free(used);
}

/* q qSQL SELECT adapter — lowers the functional 5-list (?;`t;c;b;a) (emitted by
 * BOTH the string form `select…from t` AND `?[t;c;b;a]`) onto the base
 * ray_select engine.  q_lower hands it a fully-built rayfall query dict plus the
 * by-group key column names; this special form calls ray_select and, for a
 * by-group query, splits the flat result into a KEYED table — a dict from the
 * key-columns table to the value-columns table (the mandate: "a keyed table is
 * just a dictionary from one table to another"), which q_fmt renders `k| v`.
 *   args[0] = query dict (unevaluated — ray_select owns clause-in-scope eval)
 *   args[1] = by-key column-name sym vector (empty => unkeyed passthrough) */
static ray_t* q_select_exec(ray_t** args, int64_t n) {
    ray_t* dict = args[0];
    ray_t* res  = ray_select(&dict, 1);
    if (!res || RAY_IS_ERR(res)) return res;

    ray_t* keys = (n >= 2) ? args[1] : NULL;
    int64_t nk = (keys && keys->type == RAY_SYM) ? ray_len(keys) : 0;
    if (nk == 0 || res->type != RAY_TABLE) {
        if (n >= 4) q_select_rename_temps(res, args[2], args[3]);
        return res;
    }

    int64_t kids[64];
    if (nk > 64) nk = 64;
    for (int64_t k = 0; k < nk; k++) {
        ray_t* ia = ray_i64(k);
        ray_t* ka = ray_at_fn(keys, ia);
        ray_release(ia);
        kids[k] = (ka && ka->type == -RAY_SYM) ? ka->i64 : -1;
        if (ka) ray_release(ka);
    }

    int64_t nc = ray_table_ncols(res);
    ray_t* kt = ray_table_new(nk);
    ray_t* vt = ray_table_new(nc - nk > 0 ? nc - nk : 1);
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(kt) && !RAY_IS_ERR(vt); c++) {
        int64_t nm  = ray_table_col_name(res, c);
        ray_t*  col = ray_table_get_col_idx(res, c);
        int iskey = 0;
        for (int64_t k = 0; k < nk; k++) if (kids[k] == nm) { iskey = 1; break; }
        if (iskey) kt = ray_table_add_col(kt, nm, col);
        else       vt = ray_table_add_col(vt, nm, col);
    }
    ray_release(res);
    if (RAY_IS_ERR(kt)) { ray_release(vt); return kt; }
    if (RAY_IS_ERR(vt)) { ray_release(kt); return vt; }
    if (n >= 4) q_select_rename_temps(vt, args[2], args[3]);
    return ray_dict_new(kt, vt);   /* consumes kt, vt */
}

/* ===== functional qSQL executor (piece 3) =================================
 * Runtime evaluators for the by-VALUE functional forms `?[t;c;b;a]` (select)
 * and `![t;c;b;a]` (update/delete).  q_lower embeds g_funsql_select_value /
 * g_funsql_bang_value at the head of a rank-4 `?`/`!` application; ray_eval
 * evaluates the four operands (t→table, c/b/a→list/dict/sym VALUES) and calls
 * the executor below.  Reuse-first: WHERE is evaluated against the live table
 * columns (handles `in`, `=`, comparisons uniformly, incl. the enlist-a-symbol
 * -constant convention); BY + aggregates + column projection are delegated to
 * the base engine (q_select_exec → ray_select; ray_update for update-by).  See
 * qdocs/.../basics/funsql.md for the observable contract. */

/* Append one interned symbol into a RAY_SYM (W64) vector — local twin of the
 * q_parse helper (that one is file-static there). */
static ray_t* rsymvec_append(ray_t* vec, const char* s, int len) {
    int64_t id = ray_sym_intern_runtime(s, (size_t)len);
    return ray_vec_append(vec, &id);
}

/* A verb usable at a constraint/expression head: a plain fn value, OR a 104h
 * derived-verb carrier (glyph verbs like `>` `*` lower to a projection carrier
 * in non-head list positions — see q_deriv.h / ql_deriv_value). */
/* Multi-column ascending grade (permutation) — defined in src/ops/sort.c; no
 * public header exposes it, so forward-declare for the by-group key sort. */
ray_t* ray_sort_indices(ray_t** cols, uint8_t* descs, uint8_t* nulls_first,
                        uint8_t n_cols, int64_t nrows);

static int funsql_is_fn(const ray_t* x) {
    if (!x) return 0;
    if (x->type == RAY_UNARY || x->type == RAY_BINARY || x->type == RAY_VARY) return 1;
    if (x->type == RAY_LIST && q_deriv_kind_of(x) != Q_DERIV_NONE) return 1;
    return 0;
}

/* The rayfall routing name of a verb head (plain value or carrier), or NULL. */
static const char* funsql_head_name(ray_t* fn) {
    if (fn->type == RAY_LIST && q_deriv_kind_of(fn) != Q_DERIV_NONE) {
        ray_t* base = q_deriv_base(fn);
        return base ? ray_fn_name(base) : NULL;
    }
    return ray_fn_name(fn);
}

/* Call a verb head on already-evaluated args (no re-eval, so a resolved
 * -RAY_SYM constant is NOT looked up as a variable).  Routes through the same
 * call_fn1/call_fn2 dispatch the evaluator uses — this applies the ATOMIC
 * broadcast (a scalar `>`/`=` RHS against a column vector) and lazy
 * materialisation that a raw kernel-pointer call would skip.  Carriers dispatch
 * through the shared q applicator. */
static ray_t* funsql_call(ray_t* fn, ray_t** a, int64_t n) {
    if (fn->type == RAY_LIST && q_deriv_kind_of(fn) != Q_DERIV_NONE)
        return q_apply_noun(fn, a, n);
    if (n == 1) return call_fn1(fn, a[0]);
    if (n == 2) return call_fn2(fn, a[0], a[1]);
    if (fn->type == RAY_VARY) return ((ray_vary_fn)(uintptr_t)fn->i64)(a, n);
    return ray_error("rank", "funsql: verb applied to %lld args", (long long)n);
}

/* Is x an empty operand — the general null `::`, C-NULL, or a 0-length vec/list?
 * The functional degenerate forms use `()` (→ `::`) for "no constraints" / "all
 * columns", and `0b` for "no grouping". */
static int funsql_empty(const ray_t* x) {
    if (!x) return 1;
    if (x->type == RAY_NULL) return 1;
    if (x->type == RAY_LIST || ray_is_vec(x)) return ray_len((ray_t*)x) == 0;
    return 0;
}

/* Evaluate a where-constraint parse-tree (a VALUE) against the table columns.
 *   -RAY_SYM atom        -> the named column (kdb: a bare symbol IS a column ref)
 *   (fn; arg; …)         -> resolve each arg, apply fn (yields a boolean vector)
 *   enlist X (1-list/vec)-> the symbol CONSTANT X (un-enlisted)
 *   literal              -> itself
 * Returns an OWNED value (or owned error). */
static ray_t* funsql_eval(ray_t* x, ray_t* tbl) {
    if (!x) return ray_error("domain", "funsql: null expression");
    if (x->type == -RAY_SYM) {
        ray_t* col = ray_table_get_col(tbl, x->i64);
        if (col) { ray_retain(col); return col; }
        ray_retain(x); return x;                  /* names no column -> literal sym */
    }
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (n >= 1 && funsql_is_fn(e[0])) {
            int64_t na = n - 1;
            if (na < 1 || na > 7)
                return ray_error("rank", "funsql: constraint arity %lld", (long long)na);
            ray_t* av[7];
            for (int64_t i = 0; i < na; i++) {
                av[i] = funsql_eval(e[i + 1], tbl);
                if (!av[i] || RAY_IS_ERR(av[i])) {
                    for (int64_t j = 0; j < i; j++) ray_release(av[j]);
                    return av[i] ? av[i] : ray_error("domain", "funsql: constraint arg");
                }
            }
            ray_t* r = funsql_call(e[0], av, na);
            for (int64_t i = 0; i < na; i++) ray_release(av[i]);
            return r;
        }
        if (n == 1) { ray_retain(e[0]); return e[0]; }   /* enlist X -> X */
        ray_retain(x); return x;
    }
    if (x->type == RAY_SYM && ray_len(x) == 1) {         /* enlist `sym -> `sym */
        ray_t* i0 = ray_i64(0);
        ray_t* r = ray_at_fn(x, i0);
        ray_release(i0);
        return r;
    }
    ray_retain(x); return x;
}

/* Combine the where-constraints into one boolean mask (left-to-right AND), or
 * NULL when there are no constraints.  `c` is a list of constraints; a single
 * unwrapped constraint (function head) is accepted too. */
static ray_t* funsql_build_mask(ray_t* tbl, ray_t* c) {
    if (funsql_empty(c)) return NULL;
    ray_t* one[1];
    ray_t** cons; int64_t nc;
    if (c->type == RAY_LIST) {
        ray_t** e = (ray_t**)ray_data(c);
        int64_t n = ray_len(c);
        if (n >= 1 && funsql_is_fn(e[0])) { one[0] = c; cons = one; nc = 1; }
        else { cons = e; nc = n; }
    } else { one[0] = c; cons = one; nc = 1; }
    ray_t* mask = NULL;
    for (int64_t i = 0; i < nc; i++) {
        ray_t* m = funsql_eval(cons[i], tbl);
        if (!m || RAY_IS_ERR(m)) { if (mask) ray_release(mask); return m ? m : ray_error("domain", "funsql where"); }
        if (!mask) mask = m;
        else {
            ray_t* a = ray_and_fn(mask, m);
            ray_release(mask); ray_release(m);
            mask = a;
            if (!mask || RAY_IS_ERR(mask)) return mask ? mask : ray_error("oom", NULL);
        }
    }
    return mask;
}

/* Filter table rows by the where-constraints.  keep=1 selects matching rows
 * (select/exec/update); keep=0 selects the complement (delete rows).  Returns
 * a retained `tbl` unchanged when there are no constraints. */
static ray_t* funsql_filter(ray_t* tbl, ray_t* c, int keep) {
    ray_t* mask = funsql_build_mask(tbl, c);
    if (!mask) { ray_retain(tbl); return tbl; }
    if (RAY_IS_ERR(mask)) return mask;
    ray_t* use = mask;
    if (!keep) {
        use = ray_not_fn(mask);
        ray_release(mask);
        if (!use || RAY_IS_ERR(use)) return use;
    }
    ray_t* idx = ray_where_fn(use);
    ray_release(use);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : ray_error("domain", "funsql where");
    ray_t* ft = ray_at_fn(tbl, idx);
    ray_release(idx);
    return ft;
}

/* Lower a by/select expression VALUE into the AST shape the base query engine
 * consumes: a function-VALUE head becomes a bare -RAY_SYM (ray_select /
 * ray_update key their agg/predicate dispatch on the rayfall routing name), a
 * column-ref symbol becomes a bare name-ref, an enlisted constant is passed
 * through.  Returns a fresh OWNED tree. */
static ray_t* funsql_lower_expr(ray_t* x) {
    if (!x) return NULL;
    if (x->type == -RAY_SYM) return ray_sym(x->i64);        /* bare name-ref */
    if (x->type == RAY_LIST) {
        int64_t n = ray_len(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (!(n >= 1 && funsql_is_fn(e[0]))) { ray_retain(x); return x; }  /* constant */
        ray_t* out = ray_list_new(n);
        for (int64_t i = 0; i < n; i++) {
            ray_t* c;
            if (i == 0) {
                const char* nm = funsql_head_name(e[0]);
                if (nm && nm[0]) c = ray_sym(ray_sym_intern_runtime(nm, strlen(nm)));
                else { ray_retain(e[0]); c = e[0]; }
            } else {
                c = funsql_lower_expr(e[i]);
            }
            if (!c || RAY_IS_ERR(c)) { ray_release(out); return c ? c : ray_error("domain", "funsql expr"); }
            out = ray_list_append(out, c);
            ray_release(c);
        }
        return out;
    }
    ray_retain(x); return x;
}

/* Resolve the `t` operand to a live table (a bare symbol names a table var). */
static ray_t* funsql_resolve_table(ray_t* t) {
    ray_t* tbl = t;
    if (t && t->type == -RAY_SYM) {
        tbl = ray_env_get(t->i64);
        if (!tbl || RAY_IS_ERR(tbl)) return ray_error("domain", "funsql: table symbol is unbound");
    }
    if (!tbl || tbl->type != RAY_TABLE)
        return ray_error("type", "funsql: `t` must be a table");
    ray_retain(tbl);
    return tbl;
}

/* Append a `by` clause + its key-column names for a `name!col` By-DICT (the
 * only By-phrase Select supports here — symbol By-phrases are grouped-exec and
 * refused upstream).  Values may be a SYM vector (plain column names) or a
 * general list (parse-tree expressions).  Fills the base-query by-dict into
 * (*keyvec,*vallist) and records the key names into *keycols. */
static void funsql_add_by(ray_t* b, ray_t** keyvec, ray_t** vallist, ray_t** keycols) {
    if (!b || b->type != RAY_DICT) return;
    ray_t* bk = ray_dict_keys(b);
    ray_t* bv = ray_dict_vals(b);
    int64_t nb = ray_len(bk);
    int bv_sym = (bv && bv->type == RAY_SYM);
    ray_t* bkeys = ray_sym_vec_new(RAY_SYM_W64, nb);
    ray_t* bvals = ray_list_new(nb);
    for (int64_t i = 0; i < nb; i++) {
        ray_t* kn = ray_sym_vec_cell(bk, i);
        bkeys    = rsymvec_append(bkeys, ray_str_ptr(kn), (int)ray_str_len(kn));
        *keycols = rsymvec_append(*keycols, ray_str_ptr(kn), (int)ray_str_len(kn));
        ray_t* ex;
        if (bv_sym) {
            ray_t* vn = ray_sym_vec_cell(bv, i);
            ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
        } else {
            ex = funsql_lower_expr(((ray_t**)ray_data(bv))[i]);
        }
        bvals = ray_list_append(bvals, ex);
        ray_release(ex);
    }
    ray_t* bydict = ray_dict_new(bkeys, bvals);
    *keyvec  = rsymvec_append(*keyvec, "by", 2);
    *vallist = ray_list_append(*vallist, bydict);
    ray_release(bydict);
}

/* Evaluate one functional operand.  These are SPECIAL FORMs (operands arrive
 * unevaluated) so the empty-list marker `()` — which the parser lowers to the
 * `::` name-ref and which ray_eval would reject as an unbound name — maps to
 * the generic null here; everything else (name-refs, list/dict literals, `0b`,
 * `` `sym$() ``) evaluates normally. */
static ray_t* funsql_operand(ray_t* x) {
    if (!x) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        int is_null = s && ray_str_len(s) == 2 && memcmp(ray_str_ptr(s), "::", 2) == 0;
        if (s) ray_release(s);
        if (is_null) { ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ; }
    }
    return ray_eval(x);
}

/* Sort a by-group keyed table ascending by its key column(s) — kdb's `by`
 * orders groups by key, whereas the base group-by emits in first-appearance
 * order.  `dict` is a keyed table (RAY_DICT whose keys/vals are tables).
 * Consumes `dict`; returns the sorted keyed table (or `dict` unchanged on any
 * non-keyed shape / sort failure). */
static ray_t* funsql_sort_keyed(ray_t* dict) {
    if (!dict || dict->type != RAY_DICT) return dict;
    ray_t* kt = ray_dict_keys(dict);
    ray_t* vt = ray_dict_vals(dict);
    if (!kt || kt->type != RAY_TABLE || !vt || vt->type != RAY_TABLE) return dict;
    int64_t nrows = ray_table_nrows(kt);
    int64_t nk = ray_table_ncols(kt);
    if (nrows <= 1 || nk < 1 || nk > 16) return dict;
    ray_t* cols[16];
    uint8_t descs[16] = {0};
    for (int64_t j = 0; j < nk; j++) cols[j] = ray_table_get_col_idx(kt, j);
    ray_t* perm = ray_sort_indices(cols, descs, NULL, (uint8_t)nk, nrows);
    if (!perm || RAY_IS_ERR(perm)) { if (perm) ray_release(perm); return dict; }
    ray_t* kt2 = ray_at_fn(kt, perm);
    ray_t* vt2 = ray_at_fn(vt, perm);
    ray_release(perm);
    if (!kt2 || RAY_IS_ERR(kt2) || !vt2 || RAY_IS_ERR(vt2)) {
        if (kt2) ray_release(kt2);
        if (vt2) ray_release(vt2);
        return dict;
    }
    ray_t* nd = ray_dict_new(kt2, vt2);   /* consumes kt2, vt2 */
    if (!nd || RAY_IS_ERR(nd)) return dict;
    ray_release(dict);
    return nd;
}

/* `?[t;c;b;a]` select — returns a table (or a keyed table for a by-group). */
static ray_t* q_funsql_select_impl(ray_t* t, ray_t* c, ray_t* b, ray_t* a) {
    ray_t* tbl = funsql_resolve_table(t);
    if (RAY_IS_ERR(tbl)) return tbl;

    ray_t* ft = funsql_filter(tbl, c, 1);
    ray_release(tbl);
    if (!ft || RAY_IS_ERR(ft)) return ft;

    /* EXEC forms — signalled by the By-phrase being EMPTY: the general empty
     * list `()` OR `::` (both are `funsql_empty`; `()` now parses to the empty
     * list, not `::`), as opposed to `0b` (RAY_BOOL) which is Select's "no
     * grouping".  Result is a vector (a is a column/parse-tree), a dictionary
     * (a is a dict), or the last row as a dictionary (a is `()`).  See
     * funsql.md "No grouping". */
    if (funsql_empty(b)) {
        if (a && (a->type == -RAY_SYM ||
                  (a->type == RAY_LIST && ray_len(a) > 0 &&        /* guard `()` (empty list): no head to read */
                   funsql_is_fn(((ray_t**)ray_data(a))[0])))) {
            ray_t* r = funsql_eval(a, ft);          /* exec col / parse-tree -> vector/atom */
            ray_release(ft);
            return r;
        }
        if (a && a->type == RAY_DICT) {             /* exec -> dict {name: eval} */
            ray_t* ak = ray_dict_keys(a);
            ray_t* av = ray_dict_vals(a);
            int64_t na = ray_len(ak);
            int av_sym = (av && av->type == RAY_SYM);
            ray_t* dk = ray_sym_vec_new(RAY_SYM_W64, na > 0 ? na : 1);
            ray_t* dv = ray_list_new(na > 0 ? na : 1);
            for (int64_t i = 0; i < na; i++) {
                ray_t* nm = ray_sym_vec_cell(ak, i);
                dk = rsymvec_append(dk, ray_str_ptr(nm), (int)ray_str_len(nm));
                ray_t* built = NULL, *expr;
                if (av_sym) {
                    ray_t* vn = ray_sym_vec_cell(av, i);
                    built = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
                    expr = built;
                } else {
                    expr = ((ray_t**)ray_data(av))[i];
                }
                ray_t* val = funsql_eval(expr, ft);
                if (built) ray_release(built);
                if (!val || RAY_IS_ERR(val)) {
                    ray_release(dk); ray_release(dv); ray_release(ft);
                    return val ? val : ray_error("domain", "exec");
                }
                dv = ray_list_append(dv, val);
                ray_release(val);
            }
            ray_release(ft);
            return ray_dict_new(dk, dv);
        }
        if (funsql_empty(a)) {                       /* exec last row -> dict */
            int64_t nr = ray_table_nrows(ft);
            ray_t* iv = ray_i64(nr > 0 ? nr - 1 : 0);
            ray_t* r = ray_at_fn(ft, iv);
            ray_release(iv);
            ray_release(ft);
            return r;
        }
    }

    /* A symbol / symbol-vector By-phrase is the grouped-EXEC family (dict-shaped
     * results whose type depends on the a×b matrix) — deferred beyond CORE.
     * Only a `name!col` By-DICT drives a Select by-group (keyed table). */
    if (b && (b->type == -RAY_SYM || b->type == RAY_SYM)) {
        ray_release(ft);
        return ray_error("nyi", "?: symbol By-phrase (grouped exec) is not supported; use a name!col dict");
    }

    int has_by  = b && b->type == RAY_DICT;
    int has_out = a && a->type == RAY_DICT;

    /* select from t [where] — no grouping, all columns */
    if (!has_by && !has_out) return ft;

    /* by-group with an implicit column set (a is `()`) is the keyed
     * all-columns form — deferred (not CORE).  Refuse cleanly rather than build
     * a zero-output query (which trips the base null-invariant check). */
    if (has_by && !has_out) {
        ray_release(ft);
        return ray_error("nyi", "?: by-group without an aggregate dict is not supported");
    }

    ray_t* keyvec   = ray_sym_vec_new(RAY_SYM_W64, 4);
    ray_t* vallist  = ray_list_new(4);
    ray_t* keycols  = ray_sym_vec_new(RAY_SYM_W64, 1);
    ray_t* tempnames = ray_sym_vec_new(RAY_SYM_W64, 1);
    ray_t* outnames  = ray_sym_vec_new(RAY_SYM_W64, 1);

    if (has_by) funsql_add_by(b, &keyvec, &vallist, &keycols);

    if (has_out) {
        ray_t* ak = ray_dict_keys(a);
        ray_t* av = ray_dict_vals(a);
        int64_t na = ray_len(ak);
        int av_sym = (av && av->type == RAY_SYM);
        for (int64_t i = 0; i < na; i++) {
            char tmp[24];
            int tl = snprintf(tmp, sizeof tmp, "Qqc%lld", (long long)i);
            keyvec    = rsymvec_append(keyvec, tmp, tl);
            tempnames = rsymvec_append(tempnames, tmp, tl);
            ray_t* nm = ray_sym_vec_cell(ak, i);
            outnames  = rsymvec_append(outnames, ray_str_ptr(nm), (int)ray_str_len(nm));
            ray_t* ex;
            if (av_sym) {
                ray_t* vn = ray_sym_vec_cell(av, i);
                ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
            } else {
                ex = funsql_lower_expr(((ray_t**)ray_data(av))[i]);
            }
            vallist = ray_list_append(vallist, ex);
            ray_release(ex);
        }
    }

    /* from: the (already-filtered) table VALUE — appended LAST (see ql_qsql). */
    keyvec  = rsymvec_append(keyvec, "from", 4);
    vallist = ray_list_append(vallist, ft);
    ray_release(ft);

    ray_t* dict = ray_dict_new(keyvec, vallist);   /* consumes keyvec, vallist */
    if (!dict || RAY_IS_ERR(dict)) {
        ray_release(keycols); ray_release(tempnames); ray_release(outnames);
        return dict ? dict : ray_error("oom", NULL);
    }
    ray_t* sargs[4] = { dict, keycols, tempnames, outnames };
    ray_t* res = q_select_exec(sargs, 4);
    ray_release(dict); ray_release(keycols); ray_release(tempnames); ray_release(outnames);
    if (res && res->type == RAY_DICT) res = funsql_sort_keyed(res);  /* by-group key order */
    return res;
}

/* Generic scatter: overlay `upd` (length M, aligned to the M rows named by the
 * i64 index vector `idx`) onto `base` (length N), leaving unindexed rows.  Used
 * to write a grouped-update result back onto the original table (where+by
 * update, which the base ray_update refuses to compose in one pass). */
static ray_t* funsql_scatter(ray_t* base, ray_t* idx, ray_t* upd) {
    int64_t N = ray_len(base);
    int64_t M = ray_len(idx);
    int64_t* pos = (int64_t*)malloc((N > 0 ? N : 1) * sizeof(int64_t));
    if (!pos) return ray_error("oom", NULL);
    for (int64_t r = 0; r < N; r++) pos[r] = -1;
    int64_t* ii = (int64_t*)ray_data(idx);
    for (int64_t k = 0; k < M; k++)
        if (ii[k] >= 0 && ii[k] < N) pos[ii[k]] = k;
    ray_t* out = ray_list_new(N > 0 ? N : 1);
    for (int64_t r = 0; r < N; r++) {
        ray_t* src = pos[r] >= 0 ? upd : base;
        ray_t* iv  = ray_i64(pos[r] >= 0 ? pos[r] : r);
        ray_t* v   = ray_at_fn(src, iv);
        ray_release(iv);
        if (!v || RAY_IS_ERR(v)) { free(pos); ray_release(out); return v ? v : ray_error("oom", NULL); }
        out = ray_list_append(out, v);
        ray_release(v);
    }
    free(pos);
    ray_t* col = q_collapse_list(out);
    ray_release(out);
    return col;
}

/* Scatter for a BRAND-NEW column (absent from the base table): `upd` values land
 * at the `idx` rows, the unselected rows get the type-correct null (kdb:
 * `update newcol:… where …` nulls the rows the where didn't match).  `N` is the
 * full row count. */
static ray_t* funsql_scatter_new(ray_t* idx, ray_t* upd, int64_t N) {
    int64_t M = ray_len(idx);
    /* ray_typed_null wants the ATOM (negative) type: a vector upd of type T
     * nulls as -T; a scalar upd already carries its negative atom type. */
    int8_t et = ray_is_vec(upd) ? (int8_t)(-upd->type)
                                : (upd->type < 0 ? upd->type : (int8_t)(-RAY_I64));
    int64_t* pos = (int64_t*)malloc((N > 0 ? N : 1) * sizeof(int64_t));
    if (!pos) return ray_error("oom", NULL);
    for (int64_t r = 0; r < N; r++) pos[r] = -1;
    int64_t* ii = (int64_t*)ray_data(idx);
    for (int64_t k = 0; k < M; k++)
        if (ii[k] >= 0 && ii[k] < N) pos[ii[k]] = k;
    ray_t* out = ray_list_new(N > 0 ? N : 1);
    for (int64_t r = 0; r < N; r++) {
        ray_t* v;
        if (pos[r] >= 0) { ray_t* iv = ray_i64(pos[r]); v = ray_at_fn(upd, iv); ray_release(iv); }
        else             { v = ray_typed_null(et); }
        if (!v || RAY_IS_ERR(v)) { free(pos); ray_release(out); return v ? v : ray_error("oom", NULL); }
        out = ray_list_append(out, v);
        ray_release(v);
    }
    free(pos);
    ray_t* col = q_collapse_list(out);
    ray_release(out);
    return col;
}

/* Build an update query dict {from: TBL [by: KEY] OUT:EXPR …} from the a-dict
 * of computed columns.  `bykey` (borrowed, may be NULL) is a single grouping
 * column name-ref for by-update. */
static ray_t* funsql_update_dict(ray_t* tbl, ray_t* bykey, ray_t* a) {
    ray_t* kv = ray_sym_vec_new(RAY_SYM_W64, 4);
    ray_t* vl = ray_list_new(4);
    if (bykey) {
        kv = rsymvec_append(kv, "by", 2);
        ray_retain(bykey);
        vl = ray_list_append(vl, bykey);
        ray_release(bykey);
    }
    ray_t* ak = ray_dict_keys(a);
    ray_t* av = ray_dict_vals(a);
    int64_t na = ray_len(ak);
    int av_sym = (av && av->type == RAY_SYM);
    for (int64_t i = 0; i < na; i++) {
        ray_t* nm = ray_sym_vec_cell(ak, i);
        kv = rsymvec_append(kv, ray_str_ptr(nm), (int)ray_str_len(nm));
        ray_t* ex;
        if (av_sym) {
            ray_t* vn = ray_sym_vec_cell(av, i);
            ex = ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
        } else {
            ex = funsql_lower_expr(((ray_t**)ray_data(av))[i]);
        }
        vl = ray_list_append(vl, ex);
        ray_release(ex);
    }
    kv = rsymvec_append(kv, "from", 4);
    ray_retain(tbl);
    vl = ray_list_append(vl, tbl);
    ray_release(tbl);
    return ray_dict_new(kv, vl);   /* consumes kv, vl */
}

/* Collapse duplicate columns produced by the base update-by path (which
 * appends a fresh `p` column rather than replacing the existing one): keep each
 * name at its first-occurrence position but take the value from its LAST
 * occurrence (kdb: `update col:…` replaces in place).  Returns a fresh table if
 * duplicates were found, else `tbl` retained. */
static ray_t* funsql_dedup_cols(ray_t* tbl) {
    if (!tbl || tbl->type != RAY_TABLE) { if (tbl) ray_retain(tbl); return tbl; }
    int64_t nc = ray_table_ncols(tbl);
    int has_dup = 0;
    for (int64_t c = 0; c < nc && !has_dup; c++)
        for (int64_t d = c + 1; d < nc; d++)
            if (ray_table_col_name(tbl, c) == ray_table_col_name(tbl, d)) { has_dup = 1; break; }
    if (!has_dup) { ray_retain(tbl); return tbl; }
    ray_t* out = ray_table_new((int32_t)(nc > 0 ? nc : 1));
    for (int64_t c = 0; c < nc && !RAY_IS_ERR(out); c++) {
        int64_t nm = ray_table_col_name(tbl, c);
        int seen = 0;
        for (int64_t e = 0; e < c; e++) if (ray_table_col_name(tbl, e) == nm) { seen = 1; break; }
        if (seen) continue;
        int64_t last = c;
        for (int64_t d = c + 1; d < nc; d++) if (ray_table_col_name(tbl, d) == nm) last = d;
        ray_t* col = ray_table_get_col_idx(tbl, last);
        out = ray_table_add_col(out, nm, col);
    }
    return out;
}

/* Single grouping column name-ref for update-by, or NULL.  b may be a dict
 * {name:col} (use the col), a -RAY_SYM, or a 1-vector. */
static ray_t* funsql_by_key(ray_t* b) {
    if (!b) return NULL;
    if (b->type == -RAY_SYM) return ray_sym(b->i64);
    if (b->type == RAY_SYM && ray_len(b) >= 1) {
        ray_t* cn = ray_sym_vec_cell(b, 0);
        return ray_sym(ray_sym_intern_runtime(ray_str_ptr(cn), ray_str_len(cn)));
    }
    if (b->type == RAY_DICT) {
        ray_t* bv = ray_dict_vals(b);
        if (bv && bv->type == RAY_SYM && ray_len(bv) >= 1) {
            ray_t* vn = ray_sym_vec_cell(bv, 0);
            return ray_sym(ray_sym_intern_runtime(ray_str_ptr(vn), ray_str_len(vn)));
        }
        if (bv && bv->type == RAY_LIST && ray_len(bv) >= 1) {
            ray_t* lowered = funsql_lower_expr(((ray_t**)ray_data(bv))[0]);
            if (lowered && lowered->type == -RAY_SYM) return lowered;
            if (lowered) ray_release(lowered);
        }
    }
    return NULL;
}

/* `![t;c;b;a]` — update (a is a dict) or delete (a is a symbol vector). */
static ray_t* q_funsql_bang_impl(ray_t* t, ray_t* c, ray_t* b, ray_t* a) {
    /* Namespace EXPUNGE (q4m3 §12.5): `delete x from `.` / `delete wrong
     * from `.jab` — the "table" is a context handle and `a` names the
     * bindings to remove.  Engine env_set_dotted(NULL) does the dict
     * delete + empty-context cascade; result is the handle (kdb returns
     * `.).  Claimed BEFORE table resolution — `.` is not a table. */
    if (t && t->type == -RAY_SYM && a && a->type == RAY_SYM && ray_len(a) > 0) {
        ray_t* hs = ray_sym_str(t->i64);
        if (hs) {
            const char* hn = ray_str_ptr(hs);
            size_t hl = ray_str_len(hs);
            int is_root = (hl == 1 && hn[0] == '.');
            int is_ctx  = (hl >= 2 && hn[0] == '.' && hn[1] != ':' &&
                           hn[1] != '.' &&
                           (!memchr(hn + 1, '.', hl - 1) ||
                            q_ns_is_context(hn, hl)));
            if (is_root || is_ctx) {
                int64_t nn = ray_len(a);
                for (int64_t i = 0; i < nn; i++) {
                    ray_t* cn = ray_sym_vec_cell(a, i);
                    char full[192];
                    full[0] = '\0';           /* error paths print `full` */
                    int fl = is_root
                        ? snprintf(full, sizeof full, "%.*s",
                                   (int)ray_str_len(cn), ray_str_ptr(cn))
                        : snprintf(full, sizeof full, "%.*s.%.*s", (int)hl, hn,
                                   (int)ray_str_len(cn), ray_str_ptr(cn));
                    ray_err_t err = (fl > 0 && (size_t)fl < sizeof full)
                        ? ray_env_set(ray_sym_intern(full, (size_t)fl), NULL)
                        : RAY_ERR_TYPE;
                    if (err != RAY_OK) {
                        ray_release(hs);
                        return err == RAY_ERR_RESERVED
                            ? ray_error("reserve", "delete: '%s' is reserved", full)
                            : ray_error(ray_err_code_str(err),
                                        "delete: '%s' failed", full);
                    }
                }
                ray_release(hs);
                ray_retain(t);
                return t;
            }
            ray_release(hs);
        }
    }

    ray_t* tbl = funsql_resolve_table(t);
    if (RAY_IS_ERR(tbl)) return tbl;

    int is_update = a && a->type == RAY_DICT;

    if (!is_update) {
        /* DELETE.  a non-empty (symbol vector) => drop those columns; else the
         * where-constraints select rows to remove. */
        int del_cols = a && a->type == RAY_SYM && ray_len(a) > 0;
        if (del_cols) {
            int64_t ndrop = ray_len(a);
            int64_t drop_ids[64];
            if (ndrop > 64) ndrop = 64;
            for (int64_t i = 0; i < ndrop; i++) {
                ray_t* cn = ray_sym_vec_cell(a, i);
                drop_ids[i] = ray_sym_intern_runtime(ray_str_ptr(cn), ray_str_len(cn));
            }
            int64_t nc = ray_table_ncols(tbl);
            ray_t* out = ray_table_new((int32_t)(nc > 0 ? nc : 1));
            for (int64_t col = 0; col < nc && !RAY_IS_ERR(out); col++) {
                int64_t nm = ray_table_col_name(tbl, col);
                int drop = 0;
                for (int64_t i = 0; i < ndrop; i++) if (drop_ids[i] == nm) { drop = 1; break; }
                if (drop) continue;
                ray_t* cv = ray_table_get_col_idx(tbl, col);
                out = ray_table_add_col(out, nm, cv);
            }
            ray_release(tbl);
            return out;
        }
        /* delete rows matching c: keep the complement */
        ray_t* res = funsql_filter(tbl, c, 0);
        ray_release(tbl);
        return res;
    }

    /* UPDATE.  where (if any) is applied by filtering, running the update on
     * the subtable, then scattering the changed columns back — this composes
     * where+by, which the base ray_update declines to do in one pass. */
    int has_where = !funsql_empty(c);
    ray_t* bykey = funsql_by_key(b);

    if (!has_where) {
        ray_t* dict = funsql_update_dict(tbl, bykey, a);
        if (bykey) ray_release(bykey);
        ray_release(tbl);
        if (!dict || RAY_IS_ERR(dict)) return dict ? dict : ray_error("oom", NULL);
        ray_t* res = ray_update(&dict, 1);
        ray_release(dict);
        if (!res || RAY_IS_ERR(res)) return res;
        ray_t* dd = funsql_dedup_cols(res);
        ray_release(res);
        return dd;
    }

    ray_t* mask = funsql_build_mask(tbl, c);
    if (!mask || RAY_IS_ERR(mask)) { if (bykey) ray_release(bykey); ray_release(tbl); return mask ? mask : ray_error("domain", "update where"); }
    ray_t* idx = ray_where_fn(mask);
    ray_release(mask);
    if (!idx || RAY_IS_ERR(idx)) { if (bykey) ray_release(bykey); ray_release(tbl); return idx ? idx : ray_error("domain", "update where"); }
    ray_t* ft = ray_at_fn(tbl, idx);
    if (!ft || RAY_IS_ERR(ft)) { if (bykey) ray_release(bykey); ray_release(idx); ray_release(tbl); return ft ? ft : ray_error("domain", "update where"); }

    ray_t* udict = funsql_update_dict(ft, bykey, a);
    if (bykey) ray_release(bykey);
    ray_release(ft);
    if (!udict || RAY_IS_ERR(udict)) { ray_release(idx); ray_release(tbl); return udict ? udict : ray_error("oom", NULL); }
    ray_t* uft0 = ray_update(&udict, 1);
    ray_release(udict);
    if (!uft0 || RAY_IS_ERR(uft0)) { ray_release(idx); ray_release(tbl); return uft0 ? uft0 : ray_error("domain", "update"); }
    ray_t* uft = funsql_dedup_cols(uft0);
    ray_release(uft0);

    /* Rebuild the table, scattering each updated column back at `idx`. */
    ray_t* ak = ray_dict_keys(a);
    int64_t na = ray_len(ak);
    int64_t upd_ids[64];
    if (na > 64) na = 64;
    for (int64_t i = 0; i < na; i++) {
        ray_t* nm = ray_sym_vec_cell(ak, i);
        upd_ids[i] = ray_sym_intern_runtime(ray_str_ptr(nm), ray_str_len(nm));
    }
    int64_t nc = ray_table_ncols(tbl);
    ray_t* out = ray_table_new((int32_t)(nc > 0 ? nc : 1));
    for (int64_t col = 0; col < nc && !RAY_IS_ERR(out); col++) {
        int64_t nm = ray_table_col_name(tbl, col);
        int updated = 0;
        for (int64_t i = 0; i < na; i++) if (upd_ids[i] == nm) { updated = 1; break; }
        ray_t* base = ray_table_get_col_idx(tbl, col);
        if (!updated) { out = ray_table_add_col(out, nm, base); continue; }
        ray_t* newvals = ray_table_get_col(uft, nm);
        if (!newvals) { out = ray_table_add_col(out, nm, base); continue; }
        ray_t* merged = funsql_scatter(base, idx, newvals);
        if (!merged || RAY_IS_ERR(merged)) { ray_release(out); ray_release(idx); ray_release(uft); ray_release(tbl); return merged ? merged : ray_error("oom", NULL); }
        out = ray_table_add_col(out, nm, merged);
        ray_release(merged);
    }
    /* Append columns that `a` CREATED (absent from the base table): the where
     * path filters to a subtable, so these live only in `uft` — scatter them
     * across the full row count with nulls on the unselected rows (parity with
     * the no-where update path, which adds them directly). */
    int64_t ntbl = ray_table_nrows(tbl);
    for (int64_t i = 0; i < na && !RAY_IS_ERR(out); i++) {
        if (ray_table_get_col(tbl, upd_ids[i])) continue;      /* already emitted */
        ray_t* newvals = ray_table_get_col(uft, upd_ids[i]);
        if (!newvals) continue;
        ray_t* merged = funsql_scatter_new(idx, newvals, ntbl);
        if (!merged || RAY_IS_ERR(merged)) { ray_release(out); ray_release(idx); ray_release(uft); ray_release(tbl); return merged ? merged : ray_error("oom", NULL); }
        out = ray_table_add_col(out, upd_ids[i], merged);
        ray_release(merged);
    }
    ray_release(idx);
    ray_release(uft);
    ray_release(tbl);
    return out;
}

/* SPECIAL-FORM entry points: evaluate the four operands (mapping `()`→null),
 * dispatch, then release the evaluated operands. */
static ray_t* q_funsql_dispatch(ray_t** args, int64_t n,
                                ray_t* (*impl)(ray_t*, ray_t*, ray_t*, ray_t*),
                                const char* glyph) {
    if (n != 4) return ray_error("rank", "%s[t;c;b;a]: expects 4 args, got %lld", glyph, (long long)n);
    ray_t* ev[4] = {0};
    for (int i = 0; i < 4; i++) {
        ev[i] = funsql_operand(args[i]);
        if (!ev[i] || RAY_IS_ERR(ev[i])) {
            ray_t* err = ev[i] ? ev[i] : ray_error("domain", "funsql: operand eval");
            for (int j = 0; j < i; j++) ray_release(ev[j]);
            return err;
        }
    }
    ray_t* res = impl(ev[0], ev[1], ev[2], ev[3]);
    for (int i = 0; i < 4; i++) ray_release(ev[i]);
    return res;
}

static ray_t* q_funsql_select(ray_t** args, int64_t n) {
    return q_funsql_dispatch(args, n, q_funsql_select_impl, "?");
}
static ray_t* q_funsql_bang(ray_t** args, int64_t n) {
    return q_funsql_dispatch(args, n, q_funsql_bang_impl, "!");
}

static ray_t* g_list_value   = NULL;
static ray_t* g_table_value  = NULL;
static ray_t* g_keyed_table_value = NULL;
static ray_t* g_select_value = NULL;
static ray_t* g_compose_value = NULL;

/* q `'[f;g;…]` compose builder — a normal VARY (args are the resolved function
 * VALUES): boxes them into a Q_DERIV_COMPOSE carrier (q_deriv.c). */
static ray_t* q_compose_fn(ray_t** args, int64_t n) {
    if (n < 1) return ray_error("rank", "': compose needs at least one function");
    return q_compose_new(args, n);
}
static ray_t* g_funsql_select_value = NULL;
static ray_t* g_funsql_bang_value   = NULL;
static ray_t* g_lambda_value        = NULL;

/* q.fn — SPECIAL FORM behind every q lambda literal.  q_lower rewrites the
 * parser's `{`-marker node to (q.fn src params body...); at eval this
 * delegates lambda creation to the base env `fn` form (same params/body
 * calling convention) and wraps the resulting RAY_LAMBDA in the 100h
 * .q.lambda carrier that carries q valence + verbatim source for display.
 * kdb caps lambdas at 8 arguments -> 'params — signalled HERE (not at parse:
 * qdoc error rows only match lower/eval errors). */
static ray_t* q_fn_make(ray_t** args, int64_t n) {
    if (n < 3 || !args[0] || args[0]->type != -RAY_STR ||
        !args[1] || args[1]->type != RAY_SYM)
        return ray_error("type", "q.fn: malformed lambda node");
    int64_t rank = ray_len(args[1]);
    if (rank > 8)
        return ray_error("params", "'params: lambdas take at most 8 arguments");
    ray_t* fnv = ray_env_get(ray_sym_intern("fn", 2));       /* borrowed */
    if (!fnv || fnv->type != RAY_VARY)
        return ray_error("type", "q.fn: base fn form unavailable");
    ray_t* lam = ((ray_vary_fn)(uintptr_t)fnv->i64)(args + 1, n - 1);
    if (!lam || RAY_IS_ERR(lam)) return lam;
    ray_t* c = q_lambda_carrier_new(lam, (int)rank, args[0]);
    ray_release(lam);                       /* carrier holds its own ref */
    return c;
}

ray_t* q_registry_lambda_value(void) { return g_lambda_value; }  /* borrowed */

static ray_t* g_ret_value = NULL;
static ray_t* g_sig_value = NULL;
static _Thread_local ray_t* g_qret_payload = NULL;
static _Thread_local ray_t* g_qsig_payload = NULL;

/* `:x` early return (basics/function-notation.md#explicit-return).  The body
 * must unwind NOW: eval aborts a lambda body on any RAY_ERROR, so we ride
 * the error path with the reserved class "q.ret" and stash the payload in a
 * thread-local for the innermost q_lambda_apply to take. */
static ray_t* q_ret_fn(ray_t* x) {
    if (g_qret_payload) { ray_release(g_qret_payload); g_qret_payload = NULL; }
    if (x) ray_retain(x);
    g_qret_payload = x;
    return ray_error("q.ret", NULL);
}

ray_t* q_lambda_ret_take(void) {
    ray_t* v = g_qret_payload;      /* owned by the caller now */
    g_qret_payload = NULL;
    return v;
}

/* Full text of the most recent `'x` signal.  The ≤7-char error class in
 * err->sdata truncates, but kdb Trap hands the handler the WHOLE message, so
 * q_sig_fn stashes the untruncated text here (mirroring the q.ret payload).
 * Owned by the caller; NULL if the last error was not a q signal. */
ray_t* q_registry_sig_take(void) {
    ray_t* v = g_qsig_payload;
    g_qsig_payload = NULL;
    return v;
}

void q_registry_sig_clear(void) {
    if (g_qsig_payload) { ray_release(g_qsig_payload); g_qsig_payload = NULL; }
}

/* `'x` Signal (ref/signal.md): abort with error class = the sym spelling /
 * string text (ray_error copies, 7-char sdata cap — kdb's own classes are
 * short for the same reason).  The full untruncated text is stashed for Trap
 * (q_registry_sig_take). */
static ray_t* q_sig_fn(ray_t* x) {
    char cls[8] = "signal";
    ray_t* full = NULL;                 /* owned full-text string, or NULL */
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        if (s) {
            size_t l = ray_str_len(s); size_t c = l > 7 ? 7 : l;
            memcpy(cls, ray_str_ptr(s), c); cls[c] = '\0';
            full = ray_str(ray_str_ptr(s), l);
            ray_release(s);
        }
    } else if (x && x->type == -RAY_STR) {
        size_t l = ray_str_len(x); size_t c = l > 7 ? 7 : l;
        memcpy(cls, ray_str_ptr(x), c); cls[c] = '\0';
        full = ray_str(ray_str_ptr(x), l);
    }
    if (g_qsig_payload) ray_release(g_qsig_payload);
    g_qsig_payload = full;              /* owned (or NULL) */
    return ray_error(cls, NULL);
}

ray_t* q_registry_ret_value(void) { return g_ret_value; }   /* borrowed */
ray_t* q_registry_sig_value(void) { return g_sig_value; }   /* borrowed */

/* ===== q imperative control constructs =====================================
 * The `;` statement sequence and the `if` / `do` / `while` control words are
 * SPECIAL FORMS (basics/control.md, ref/{if,do,while}.md): they receive their
 * statement args UNEVALUATED and drive evaluation themselves — lazy, strictly
 * left-to-right, with side effects PERSISTING.  Unlike a lambda body they do
 * NOT open a lexical scope: a `:` assignment inside amends the ENCLOSING frame
 * (q_lower already lowered it to set/let against that frame; "the brackets do
 * not create lexical scope").  `if`/`do`/`while` always return the generic
 * null; `;` returns its LAST statement's value.  This is the q-layer home for
 * the semantics (CLAUDE.md rule 4) — rayfall's own `if`/`do` differ (triadic
 * cond / scope-pushing progn) so they are NOT reused here. */
static ray_t* g_seq_value   = NULL;
static ray_t* g_if_value    = NULL;
static ray_t* g_do_value    = NULL;
static ray_t* g_while_value = NULL;

/* q if/while require the test to evaluate to an atom of INTEGRAL type
 * (ref/if.md, ref/while.md); a float / symbol / vector / generic null is a
 * 'type error, not a silent truthiness (kdb `if[1 0;…]` signals `type). */
static int q_ctl_test_ok(ray_t* t) {
    int8_t ty = t->type;
    return ty == -RAY_BOOL || ty == -RAY_I64 || ty == -RAY_I32 ||
           ty == -RAY_I16 || ty == -RAY_U8;
}

/* Evaluate a test/condition arg to a truthiness, materializing a lazy handle.
 * On error (eval failure OR a non-integral-atom test), stashes the owned
 * RAY_ERROR in *err and returns 0. */
static int q_ctl_truth(ray_t* arg, ray_t** err) {
    *err = NULL;
    ray_t* t = ray_eval(arg);
    if (RAY_IS_ERR(t)) { *err = t; return 0; }
    if (ray_is_lazy(t)) t = ray_lazy_materialize(t);
    if (RAY_IS_ERR(t)) { *err = t; return 0; }
    if (!q_ctl_test_ok(t)) {
        ray_release(t);
        *err = ray_error("type", "control test must be an integral atom");
        return 0;
    }
    int truthy = is_truthy(t);
    ray_release(t);
    return truthy;
}

/* Evaluate args[from..n) in order for their side effects, releasing each
 * result.  Returns an owned RAY_ERROR on the first failure, else NULL. */
static ray_t* q_ctl_run_body(ray_t** args, int64_t from, int64_t n) {
    for (int64_t i = from; i < n; i++) {
        ray_t* r = ray_eval(args[i]);
        if (RAY_IS_ERR(r)) return r;
        ray_release(r);
    }
    return NULL;
}

/* `s1; s2; …; sn` — evaluate each statement left-to-right (side effects
 * persist to the enclosing frame); the value is the LAST statement's. */
static ray_t* q_seq_fn(ray_t** args, int64_t n) {
    ray_t* result = RAY_NULL_OBJ;
    for (int64_t i = 0; i < n; i++) {
        ray_release(result);                 /* RAY_NULL_OBJ release is a no-op */
        result = ray_eval(args[i]);
        if (RAY_IS_ERR(result)) return result;
    }
    return result;
}

/* `if[test; e1; …; en]` — evaluate the body once, in order, unless test is
 * zero; result is always the generic null (ref/if.md). */
static ray_t* q_if_fn(ray_t** args, int64_t n) {
    if (n < 1) return RAY_NULL_OBJ;
    ray_t* err = NULL;
    int truthy = q_ctl_truth(args[0], &err);
    if (err) return err;
    if (truthy) { err = q_ctl_run_body(args, 1, n); if (err) return err; }
    return RAY_NULL_OBJ;
}

/* `do[count; e1; …; en]` — evaluate the body `count` times; result is always
 * the generic null (ref/do.md).  `count` is a non-negative integer atom. */
static ray_t* q_do_fn(ray_t** args, int64_t n) {
    if (n < 1) return RAY_NULL_OBJ;
    ray_t* cnt = ray_eval(args[0]);
    if (RAY_IS_ERR(cnt)) return cnt;
    if (ray_is_lazy(cnt)) cnt = ray_lazy_materialize(cnt);
    if (RAY_IS_ERR(cnt)) return cnt;
    if (RAY_ATOM_IS_NULL(cnt) ||
        (cnt->type != -RAY_I64 && cnt->type != -RAY_I32 &&
         cnt->type != -RAY_I16 && cnt->type != -RAY_U8)) {
        ray_release(cnt);
        return ray_error("type", "do: count must be a non-negative integer atom");
    }
    int64_t times = elem_as_i64(cnt);
    ray_release(cnt);
    if (times < 0) return ray_error("type", "do: count must be non-negative");
    for (int64_t k = 0; k < times; k++) {
        ray_t* err = q_ctl_run_body(args, 1, n);
        if (err) return err;
    }
    return RAY_NULL_OBJ;
}

/* `while[test; e1; …; en]` — re-evaluate test each pass; while non-zero run
 * the body in order; result is always the generic null (ref/while.md). */
static ray_t* q_while_fn(ray_t** args, int64_t n) {
    if (n < 1) return RAY_NULL_OBJ;
    for (;;) {
        ray_t* err = NULL;
        int truthy = q_ctl_truth(args[0], &err);
        if (err) return err;
        if (!truthy) break;
        err = q_ctl_run_body(args, 1, n);
        if (err) return err;
    }
    return RAY_NULL_OBJ;
}

ray_t* q_registry_seq_value(void)   { return g_seq_value; }    /* borrowed */
ray_t* q_registry_if_value(void)    { return g_if_value; }     /* borrowed */
ray_t* q_registry_do_value(void)    { return g_do_value; }     /* borrowed */
ray_t* q_registry_while_value(void) { return g_while_value; }  /* borrowed */

ray_t* q_registry_funsql_select_value(void) { return g_funsql_select_value; }
ray_t* q_registry_funsql_bang_value(void)   { return g_funsql_bang_value; }

ray_t* q_registry_select_value(void) {
    return g_select_value;   /* borrowed; NULL before init */
}

ray_t* q_registry_compose_value(void) {
    return g_compose_value;  /* borrowed; NULL before init */
}

ray_t* q_registry_list_value(void) {
    return g_list_value;   /* borrowed; NULL before init */
}

ray_t* q_registry_keyed_table_value(void) {
    return g_keyed_table_value;  /* borrowed; NULL before init */
}

ray_t* q_registry_table_value(void) {
    return g_table_value;  /* borrowed; NULL before init */
}

/* ---- collapse: homogeneous atom list -> typed vector (see q_registry.h) ---- */

ray_t* q_collapse_list(ray_t* l) {
    if (!l || RAY_IS_ERR(l) || l->type != RAY_LIST || ray_len(l) == 0) {
        if (l) ray_retain(l);
        return l;
    }
    int64_t n = ray_len(l);
    ray_t** e = (ray_t**)ray_data(l);
    int8_t t = e[0] ? e[0]->type : 0;
    if (t >= 0 || t == -RAY_STR) { ray_retain(l); return l; }   /* not a scalar-atom run */
    for (int64_t i = 1; i < n; i++)
        if (!e[i] || e[i]->type != t) { ray_retain(l); return l; }

    if (t == -RAY_SYM) {
        ray_t* vec = ray_sym_vec_new(RAY_SYM_W64, n);
        if (RAY_IS_ERR(vec)) return vec;
        for (int64_t i = 0; i < n; i++) vec = ray_vec_append(vec, &e[i]->i64);
        return vec;
    }

    ray_t* vec = ray_vec_new(-t, n);
    if (RAY_IS_ERR(vec)) return vec;
    int64_t nulls = 0;
    for (int64_t i = 0; i < n; i++) {
        switch (t) {
        case -RAY_BOOL: vec = ray_vec_append(vec, &e[i]->b8);  break;
        case -RAY_I16:  vec = ray_vec_append(vec, &e[i]->i16); break;
        case -RAY_I32:  vec = ray_vec_append(vec, &e[i]->i32); break;
        case -RAY_F32: { float f = (float)e[i]->f64;            /* F32 atom stores f64 */
                         vec = ray_vec_append(vec, &f); }       break;
        case -RAY_F64:
        case -RAY_DATETIME:
                        vec = ray_vec_append(vec, &e[i]->f64); break;
        case -RAY_GUID: {                                      /* 16-byte payload, not i64 */
            const void* g = e[i]->obj ? ray_data(e[i]->obj) : ray_data(e[i]);
            vec = ray_vec_append(vec, g);
        } break;
        default:        vec = ray_vec_append(vec, &e[i]->i64); break; /* i64 + temporals */
        }
        if (RAY_IS_ERR(vec)) return vec;
        if (RAY_ATOM_IS_NULL(e[i])) { ray_vec_set_null(vec, i, true); nulls++; }
    }
    (void)nulls;
    return vec;
}

/* ---- IPC client verb: q `hopen` (feat/q-ipc-client, Phase D) ----
 * Thin wrapper over `.ipc.open` (ray_hopen_fn), which takes a
 * "host:port[:user:password]" string + optional connect-timeout.  q `hopen`
 * accepts an int PORT (localhost), a "host:port[:user:pass]" STRING (with the
 * kdb "::PORT"/":host:port" leading-colon conventions), or a 2-list
 * (conn; timeout-ms).  The kdb hsym-SYMBOL forms (`::PORT`, `:host:port`) do
 * not lex as a single colon-bearing symbol yet (the scanner stops at ':'), so
 * they are deferred to a lexer change; the int/string surface parses today. */

/* Normalize a connection descriptor (int atom or string atom) into the
 * "host:port[:user:password]" form .ipc.open expects.  Owned RAY_STR or error. */
static ray_t* q_hopen_connstr(ray_t* c) {
    if (q_is_int_atom(c)) {
        int64_t p = q_iatom_val(c);
        if (p <= 0 || p > 65535)
            return ray_error("domain", "hopen: port must be in 1..65535, got %lld",
                             (long long)p);
        char buf[32];
        int m = snprintf(buf, sizeof buf, "127.0.0.1:%lld", (long long)p);
        if (m <= 0 || m >= (int)sizeof buf) return ray_error("domain", "hopen: bad port");
        return ray_str(buf, (size_t)m);
    }
    if (c && c->type == -RAY_STR) {
        const char* s = ray_str_ptr(c);
        size_t n = ray_str_len(c);
        if (n > 512) return ray_error("domain", "hopen: descriptor too long");
        /* kdb leading-colon conventions: "::rest" = localhost, ":host:.." = strip 1 */
        if (n >= 2 && s[0] == ':' && s[1] == ':') {
            char buf[600];
            int m = snprintf(buf, sizeof buf, "127.0.0.1:%.*s", (int)(n - 2), s + 2);
            if (m <= 0 || m >= (int)sizeof buf)
                return ray_error("domain", "hopen: descriptor too long");
            return ray_str(buf, (size_t)m);
        }
        if (n >= 1 && s[0] == ':') return ray_str(s + 1, n - 1);   /* strip one ':' */
        return ray_str(s, n);                                      /* "host:port" */
    }
    return ray_error("type",
                     "hopen: expected an int port or a \"host:port\" string, got %s",
                     ray_type_name(c ? c->type : 0));
}

/* q `hopen y` — connect, return an int handle.  Restricted connections must not
 * open outbound sockets (the `.ipc.open` primitive is RAY_FN_RESTRICTED; calling
 * ray_hopen_fn directly bypasses the eval-layer check, so re-assert it here). */
static ray_t* q_hopen_wrap(ray_t* x) {
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    ray_t* conn      = x;
    ray_t* timeout   = NULL;
    ray_t* pair_conn = NULL;   /* owned when a pair was a typed int VECTOR */
    ray_t* pair_to   = NULL;
    if (x && x->type == RAY_LIST && ray_len(x) == 2) {   /* (conn; timeout-ms) */
        ray_t** e = (ray_t**)ray_data(x);
        conn = e[0]; timeout = e[1];                     /* borrowed */
    } else if (q_is_int_vec(x) && ray_len(x) == 2) {
        /* an all-int (port; timeout-ms) pair collapses to a homogeneous int
         * VECTOR (not a general list) — recover the two atoms.  (A symbol/string
         * conn keeps the pair a RAY_LIST, handled above.) */
        pair_conn = ray_i64(q_ivec_get(x, 0));
        pair_to   = ray_i64(q_ivec_get(x, 1));
        conn = pair_conn; timeout = pair_to;
    }
    ray_t* cs = q_hopen_connstr(conn);                   /* owned or error */
    if (!cs || RAY_IS_ERR(cs)) {
        if (pair_conn) ray_release(pair_conn);
        if (pair_to)   ray_release(pair_to);
        return cs;
    }
    ray_t* args[2] = { cs, NULL };
    int64_t nargs = 1;
    ray_t* tv = NULL;
    if (timeout && !q_is_int_atom(timeout)) {
        ray_release(cs);
        if (pair_conn) ray_release(pair_conn);
        if (pair_to)   ray_release(pair_to);
        return ray_error("type", "hopen: timeout must be an integer (milliseconds)");
    }
    if (timeout) {
        tv = make_i64(q_iatom_val(timeout));
        args[1] = tv; nargs = 2;
    }
    ray_t* h = ray_hopen_fn(args, nargs);                /* owned handle or error */
    ray_release(cs);
    if (tv)        ray_release(tv);
    if (pair_conn) ray_release(pair_conn);
    if (pair_to)   ray_release(pair_to);
    if (!h || RAY_IS_ERR(h)) return h;
    /* kdb-faithful "true fd" handle model: a q connection handle IS the socket
     * fd (qdocs basics/handles.md — 0 console, 1 stdout, 2 stderr, connections
     * at 3+).  ray_hopen_fn returns the rayfall poll SELECTOR ID (dense, starts
     * at 0); translate it to the connection's socket fd, which is always >= 3
     * (0/1/2 held by the std streams) and thus disjoint from the console-write
     * handles.  handle-apply / hclose translate the fd back to the selector id
     * the .ipc.* primitives expect. */
    int64_t raw = (h->type == -RAY_I64) ? h->i64 : (int64_t)h->i32;
    ray_release(h);
    int64_t fd = ray_ipc_fd_of_handle(raw);
    if (fd < 0) {
        /* Connection vanished between connect and fd lookup — close the raw
         * selector and surface, rather than hand back a bogus handle. */
        ray_t* rid = make_i64(raw);
        ray_t* cr = ray_hclose_fn(rid);
        ray_release(rid);
        if (cr) ray_release(cr);
        return ray_error("io", "hopen: connection lost");
    }
    return make_i64(fd);
}

/* q `hclose h` — translate the q fd handle back to the poll selector id and
 * route to `.ipc.close` (ray_hclose_fn).  Restricted connections are refused,
 * matching hopen / the handle-apply path.  A q handle is the socket fd; map it
 * to the selector id the primitive expects.  A handle that is not a live IPC
 * connection (already closed, or a console handle) is a no-op, matching kdb's
 * tolerance of hclose on a dead handle. */
static ray_t* q_hclose_wrap(ray_t* x) {
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    if (!q_is_int_atom(x) || RAY_ATOM_IS_NULL(x))
        return ray_error("type", "hclose: expected an int handle, got %s",
                         ray_type_name(x ? x->type : 0));
    int64_t qh = q_iatom_val(x);
    if (qh <= 0)
        return ray_error("type", "hclose: invalid handle %lld", (long long)qh);
    int64_t id = ray_ipc_handle_of_fd(qh);
    if (id < 0) return RAY_NULL_OBJ;          /* not a live handle — no-op */
    ray_t* raw = make_i64(id);
    ray_t* r = ray_hclose_fn(raw);
    ray_release(raw);
    return r;
}

/* ===== File Text: `0:` + hsym + read0 (feat/q-file-text) ====================
 * Oracle: ref/file-text.md, ref/read0.md, ref/hsym.md (CLEAN ROOM).  TEXT
 * forms only — binary file formats (1:/2:, get/set on data files) are an
 * owner-ruled NON-GOAL (2026-07-09 ruling).
 *
 * Ownership contract (plan round 1): helper inputs are BORROWED, helper
 * outputs are OWNED by the caller; on any partial failure a helper releases
 * everything it allocated before returning the error.
 *
 * RAY_FN_RESTRICTED note: the base file primitives carry the flag on their
 * ENV fn objects; calling the C functions directly bypasses the eval-layer
 * check, so every file-touching arm re-asserts ray_eval_get_restricted()
 * (the q_hopen_wrap precedent). */

/* file symbol -> OWNED RAY_STR filesystem path (leading ':' stripped), or
 * NULL when x is not a `:path symbol. */
static ray_t* q_ft_path(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return NULL;
    ray_t* s = ray_sym_str(x->i64);                       /* borrowed */
    if (!s) return NULL;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    if (n < 2 || p[0] != ':') return NULL;
    return ray_str(p + 1, n - 1);
}

/* Read a whole file (restricted-guarded).  OWNED RAY_STR or error. */
static ray_t* q_ft_read_all(ray_t* pathstr) {
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    return ray_read_file_fn(pathstr);
}

/* q `hsym x` — sym atom/vector -> file symbol: prefix ':' unless already
 * present (ref/hsym.md). */
static int64_t q_hsym_id(const char* p, size_t n) {
    if (n > 0 && p[0] == ':') return ray_sym_intern_runtime(p, n);
    char* buf = (char*)malloc(n + 1);
    if (!buf) return ray_sym_intern_runtime(":", 1);
    buf[0] = ':';
    memcpy(buf + 1, p, n);
    int64_t id = ray_sym_intern_runtime(buf, n + 1);
    free(buf);
    return id;
}
static ray_t* q_hsym_wrap(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);                   /* borrowed */
        if (!s) return ray_error("type", "hsym: bad symbol");
        const char* p = ray_str_ptr(s);
        size_t n = ray_str_len(s);
        if (n > 0 && p[0] == ':') { ray_retain(x); return x; }
        return ray_sym(q_hsym_id(p, n));
    }
    if (x && x->type == RAY_SYM) {
        int64_t n = ray_len(x);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        for (int64_t i = 0; i < n; i++) {
            ray_t* c = ray_sym_vec_cell(x, i);            /* borrowed domain atom */
            if (!c) { ray_release(out); return ray_error("type", "hsym: bad symbol"); }
            int64_t id = q_hsym_id(ray_str_ptr(c), ray_str_len(c));
            out = ray_vec_append(out, &id);
            if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        }
        return out;
    }
    return ray_error("type", "hsym: expected a symbol, got %s",
                     ray_type_name(x ? x->type : 0));
}

/* q `read0 x` — ref/read0.md.  File sym -> list of line strings (LF/CRLF
 * delimiters removed); (f;o) -> chars from offset o to EOF minus one trailing
 * line break (the doc pins `read0(`:foo;6)` -> "world" on a file ending \n);
 * (f;o;n) -> exactly n chars from o (clamped).  Console (0) and fifo handles
 * are deferred 'nyi.  Offsets accept 0 (superset of the doc wording). */
ray_t* q_read0_wrap(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* path = q_ft_path(x);
        if (!path) return ray_error("type", "read0: expected a file symbol `:path");
        ray_t* all = q_ft_read_all(path);
        ray_release(path);
        if (!all || RAY_IS_ERR(all)) return all;
        ray_t* lines = q_str_split_lines(ray_str_ptr(all), ray_str_len(all));
        ray_release(all);
        return lines;
    }
    if (x && x->type == RAY_LIST && (ray_len(x) == 2 || ray_len(x) == 3)) {
        ray_t** e = (ray_t**)ray_data(x);
        int three = ray_len(x) == 3;
        if (e[0] && e[0]->type == -RAY_SYM) {
            ray_t* path = q_ft_path(e[0]);
            if (!path) return ray_error("type", "read0: expected (filesymbol;offset[;length])");
            if (!q_is_int_atom(e[1]) || (three && !q_is_int_atom(e[2]))) {
                ray_release(path);
                return ray_error("type", "read0: offset/length must be integers");
            }
            int64_t off  = q_iatom_val(e[1]);
            int64_t want = three ? q_iatom_val(e[2]) : -1;
            if (off < 0 || (three && want < 0)) {
                ray_release(path);
                return ray_error("domain", "read0: negative offset/length");
            }
            ray_t* all = q_ft_read_all(path);
            ray_release(path);
            if (!all || RAY_IS_ERR(all)) return all;
            const char* p = ray_str_ptr(all);
            int64_t n = (int64_t)ray_str_len(all);
            if (off > n) off = n;
            int64_t end = three ? (off + want > n ? n : off + want) : n;
            if (!three) {                          /* strip one trailing \n / \r\n */
                if (end > off && p[end - 1] == '\n') end--;
                if (end > off && p[end - 1] == '\r') end--;
            }
            ray_t* out = ray_str(p + off, (size_t)(end - off));
            ray_release(all);
            return out;
        }
        return ray_error("nyi", "read0: fifo handles are deferred");
    }
    if (x && q_is_int_atom(x))
        return ray_error("nyi", "read0: console/connection handles are deferred");
    return ray_error("type", "read0: expected a file symbol or (filesymbol;offset[;length])");
}

/* ---- Save Text: `:path 0: strings -------------------------------------- */
static ray_t* q_ft_save_text(ray_t* fsym, ray_t* y) {
    ray_t* path = q_ft_path(fsym);
    if (!path) return ray_error("type", "0:: bad file symbol");
    if (ray_eval_get_restricted()) { ray_release(path); return ray_error("access", "restricted"); }
    if (!y || !(y->type == RAY_LIST || y->type == RAY_STR)) {
        ray_release(path);
        return ray_error("type", "0:: Save Text needs a list of strings");
    }
    int64_t n = ray_len(y);
    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(y, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { ray_release(path); return e ? e : ray_error("oom", NULL); }
        if (e->type != -RAY_STR) {
            ray_release(e); ray_release(path);
            return ray_error("type", "0:: Save Text needs a list of strings");
        }
        total += ray_str_len(e) + 1;                       /* line + '\n' */
        ray_release(e);
    }
    char* buf = (char*)malloc(total ? total : 1);
    if (!buf) { ray_release(path); return ray_error("oom", NULL); }
    size_t w = 0;
    for (int64_t i = 0; i < n; i++) {
        ray_t* ia = ray_i64(i);
        ray_t* e = ray_at_fn(y, ia);
        ray_release(ia);
        if (!e || RAY_IS_ERR(e)) { free(buf); ray_release(path); return e ? e : ray_error("oom", NULL); }
        size_t l = ray_str_len(e);
        memcpy(buf + w, ray_str_ptr(e), l);
        w += l;
        buf[w++] = '\n';
        ray_release(e);
    }
    /* create missing parent directories (the doc's "any missing containing
     * directories"); ray_mkdir_p is the shared portable impl. */
    {
        const char* pp = ray_str_ptr(path);
        size_t pn = ray_str_len(path);
        size_t cut = pn;
        while (cut > 0 && pp[cut - 1] != '/') cut--;
        if (cut > 1) {
            char* dir = (char*)malloc(cut);
            if (dir) {
                memcpy(dir, pp, cut - 1);
                dir[cut - 1] = '\0';
                (void)ray_mkdir_p(dir);
                free(dir);
            }
        }
    }
    ray_t* content = ray_str(buf, w);
    free(buf);
    if (!content || RAY_IS_ERR(content)) { ray_release(path); return content ? content : ray_error("oom", NULL); }
    ray_t* r = ray_write_file_fn(path, content);
    ray_release(path);
    ray_release(content);
    if (!r || RAY_IS_ERR(r)) return r;
    ray_release(r);
    ray_retain(fsym);
    return fsym;
}

/* ---- Prepare Text: delim 0: table | list-of-columns --------------------- */

/* One cell -> OWNED RAY_STR raw text (no quoting).  Borrows atom. */
static ray_t* q_ft_cell_text(ray_t* atom) {
    ray_t* s = q_string_fn(atom);
    if (!s || RAY_IS_ERR(s)) return s;
    if (atom->type == -RAY_DATE && s->type == -RAY_STR) {
        /* Prepare Text renders temporals ISO 8601 (doc: 2022-03-14) — the
         * date dots become dashes; other temporals already match. */
        size_t n = ray_str_len(s);
        char* b = (char*)malloc(n ? n : 1);
        if (b) {
            const char* p = ray_str_ptr(s);
            for (size_t i = 0; i < n; i++) b[i] = p[i] == '.' ? '-' : p[i];
            ray_t* d = ray_str(b, n);
            free(b);
            ray_release(s);
            return d;
        }
    }
    return s;
}

/* Quote rule (doc): a cell containing the delimiter or a line break is
 * embraced with '"' and every embedded '"' doubled; otherwise raw.  Appends
 * the (possibly embraced) cell to *buf/(w..cap).  Returns 0 on OOM. */
static int q_ft_quote_append(char** buf, size_t* w, size_t* cap,
                             const char* c, size_t n, char delim) {
    int embrace = 0;
    size_t extra = 2;
    for (size_t i = 0; i < n; i++) {
        if (c[i] == delim || c[i] == '\n') embrace = 1;
        if (c[i] == '"') extra++;
    }
    size_t need = *w + n + (embrace ? extra : 0) + 2;
    if (need > *cap) {
        size_t nc = *cap ? *cap : 64;
        while (nc < need) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb; *cap = nc;
    }
    char* b = *buf;
    if (!embrace) {
        memcpy(b + *w, c, n);
        *w += n;
        return 1;
    }
    b[(*w)++] = '"';
    for (size_t i = 0; i < n; i++) {
        if (c[i] == '"') b[(*w)++] = '"';
        b[(*w)++] = c[i];
    }
    b[(*w)++] = '"';
    return 1;
}

static ray_t* q_ft_prepare(char delim, ray_t* y) {
    /* columns + optional names */
    int64_t nc = 0;
    ray_t* namev = NULL;                 /* borrowed via table introspection */
    ray_t** litems = NULL;
    int is_table = y && y->type == RAY_TABLE;
    if (is_table) nc = ray_table_ncols(y);
    else if (y && y->type == RAY_LIST) { nc = ray_len(y); litems = (ray_t**)ray_data(y); }
    else return ray_error("type", "0:: Prepare Text needs a table or a list of columns");
    (void)namev;
    if (nc == 0) return ray_list_new(1);
    /* validate columns; find the shared row count */
    int64_t L = -1;
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = is_table ? ray_table_get_col_idx(y, c) : litems[c];  /* borrowed */
        int64_t l;
        if (col && col->type == -RAY_STR) l = (int64_t)ray_str_len(col);  /* char column */
        else if (col && (ray_is_vec(col) || col->type == RAY_LIST)) {
            l = ray_len(col);
            if (col->type == RAY_LIST) {                  /* must be all strings */
                ray_t** it = (ray_t**)ray_data(col);
                for (int64_t i = 0; i < l; i++)
                    if (!it[i] || it[i]->type != -RAY_STR)
                        return ray_error("type", "0:: column is neither a vector nor a list of strings");
            }
        } else return ray_error("type", "0:: column is neither a vector nor a list of strings");
        if (L < 0) L = l;
        else if (l != L) return ray_error("length", "0:: column lengths differ");
    }
    ray_t* out = ray_list_new(L + 1 > 0 ? L + 1 : 1);
    if (RAY_IS_ERR(out)) return out;
    char* buf = NULL;
    size_t cap = 0;
    /* header row: table column names */
    if (is_table) {
        size_t w = 0;
        for (int64_t c = 0; c < nc; c++) {
            if (c) {
                if (w + 1 > cap) { cap = cap ? cap * 2 : 64; buf = (char*)realloc(buf, cap); if (!buf) { ray_release(out); return ray_error("oom", NULL); } }
                buf[w++] = delim;
            }
            int64_t nm = ray_table_col_name(y, c);
            ray_t* ns = ray_sym_str(nm);                   /* borrowed */
            if (!ns || !q_ft_quote_append(&buf, &w, &cap, ray_str_ptr(ns), ray_str_len(ns), delim)) {
                free(buf); ray_release(out);
                return ray_error("oom", NULL);
            }
        }
        ray_t* line = ray_str(buf ? buf : "", w);
        out = ray_list_append(out, line);
        ray_release(line);
        if (RAY_IS_ERR(out)) { free(buf); return out; }
    }
    for (int64_t i = 0; i < L; i++) {
        size_t w = 0;
        for (int64_t c = 0; c < nc; c++) {
            if (c) {
                if (w + 1 > cap) { cap = cap ? cap * 2 : 64; buf = (char*)realloc(buf, cap); if (!buf) { ray_release(out); return ray_error("oom", NULL); } }
                buf[w++] = delim;
            }
            ray_t* col = is_table ? ray_table_get_col_idx(y, c) : litems[c];  /* borrowed */
            int ok;
            if (col->type == -RAY_STR) {                   /* char column: one char */
                char ch = ray_str_ptr(col)[i];
                ok = q_ft_quote_append(&buf, &w, &cap, &ch, 1, delim);
            } else if (col->type == RAY_LIST) {            /* string column */
                ray_t** it = (ray_t**)ray_data(col);
                ok = q_ft_quote_append(&buf, &w, &cap, ray_str_ptr(it[i]), ray_str_len(it[i]), delim);
            } else {
                ray_t* ia = ray_i64(i);
                ray_t* atom = ray_at_fn(col, ia);
                ray_release(ia);
                if (!atom || RAY_IS_ERR(atom)) { free(buf); ray_release(out); return atom ? atom : ray_error("oom", NULL); }
                ray_t* cs = q_ft_cell_text(atom);
                ray_release(atom);
                if (!cs || RAY_IS_ERR(cs)) { free(buf); ray_release(out); return cs ? cs : ray_error("oom", NULL); }
                if (cs->type != -RAY_STR) { ray_release(cs); free(buf); ray_release(out); return ray_error("type", "0:: unformattable cell"); }
                ok = q_ft_quote_append(&buf, &w, &cap, ray_str_ptr(cs), ray_str_len(cs), delim);
                ray_release(cs);
            }
            if (!ok) { free(buf); ray_release(out); return ray_error("oom", NULL); }
        }
        ray_t* line = ray_str(buf ? buf : "", w);
        out = ray_list_append(out, line);
        ray_release(line);
        if (RAY_IS_ERR(out)) { free(buf); return out; }
    }
    free(buf);
    return out;
}

/* ---- Load CSV / Load Fixed shared plumbing ------------------------------ */

/* Type char -> Tok tag via THE cast home (q_cast_designator; upper case =
 * Tok).  '*' keeps the field a string, ' ' skips the column, unknown -> 0. */
static int8_t q_ft_tag(char c, int* is_str, int* is_skip) {
    *is_str = 0; *is_skip = 0;
    if (c == ' ') { *is_skip = 1; return 0; }
    if (c == '*') { *is_str = 1; return 0; }
    if (c < 'A' || c > 'Z') return 0;                      /* doc: upper case */
    ray_t* d = ray_str(&c, 1);
    if (!d || RAY_IS_ERR(d)) return 0;
    int is_tok = 0;
    int8_t tag = q_cast_designator(d, &is_tok);
    ray_release(d);
    return is_tok ? tag : 0;
}

/* Normalize the RIGHT operand of Load CSV / Load Fixed into an OWNED
 * RAY_LIST of row strings.  *single = 1 for the one-string-no-newline form
 * (kdb returns a list of parsed ATOMS for it, not columns). */
static ray_t* q_ft_rows(ray_t* y, int* single) {
    *single = 0;
    if (!y) return ray_error("type", "0:: nil right operand");
    if (y->type == -RAY_STR) {
        const char* p = ray_str_ptr(y);
        size_t n = ray_str_len(y);
        if (memchr(p, '\n', n)) return q_str_split_lines(p, n);
        *single = 1;
        ray_t* out = ray_list_new(1);
        if (RAY_IS_ERR(out)) return out;
        out = ray_list_append(out, y);                     /* retains y */
        return out;
    }
    if (y->type == -RAY_SYM) {
        ray_t* path = q_ft_path(y);
        if (!path) return ray_error("type", "0:: expected a file symbol `:path");
        ray_t* all = q_ft_read_all(path);
        ray_release(path);
        if (!all || RAY_IS_ERR(all)) return all;
        ray_t* rows = q_str_split_lines(ray_str_ptr(all), ray_str_len(all));
        ray_release(all);
        return rows;
    }
    if (y->type == RAY_LIST || y->type == RAY_STR) {
        int64_t n = ray_len(y);
        ray_t** e = y->type == RAY_LIST ? (ray_t**)ray_data(y) : NULL;
        /* (filesymbol; offset[; length]) chunk form */
        if (e && n >= 2 && n <= 3 && e[0] && e[0]->type == -RAY_SYM) {
            ray_t* path = q_ft_path(e[0]);
            if (!path) return ray_error("type", "0:: expected (filesymbol;offset[;length])");
            if (!q_is_int_atom(e[1]) || (n == 3 && !q_is_int_atom(e[2]))) {
                ray_release(path);
                return ray_error("type", "0:: offset/length must be integers");
            }
            int64_t off  = q_iatom_val(e[1]);
            int64_t want = n == 3 ? q_iatom_val(e[2]) : -1;
            ray_t* all = q_ft_read_all(path);
            ray_release(path);
            if (!all || RAY_IS_ERR(all)) return all;
            const char* p = ray_str_ptr(all);
            int64_t len = (int64_t)ray_str_len(all);
            if (off < 0) off = 0;
            if (off > len) off = len;
            int64_t end = want >= 0 && off + want < len ? off + want : len;
            ray_t* rows = q_str_split_lines(p + off, (size_t)(end - off));
            ray_release(all);
            return rows;
        }
        /* list / str-vector of row strings */
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* it = ray_at_fn(y, ia);
            ray_release(ia);
            if (!it || RAY_IS_ERR(it)) { ray_release(out); return it ? it : ray_error("oom", NULL); }
            if (it->type != -RAY_STR) {
                ray_release(it); ray_release(out);
                return ray_error("type", "0:: expected a list of strings");
            }
            out = ray_list_append(out, it);
            ray_release(it);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    return ray_error("type", "0:: unsupported right operand");
}

/* flag=1 (embedded line returns): merge physical rows whose quotes are
 * unbalanced with the following row, restoring the '\n'.  Owns+returns. */
static ray_t* q_ft_merge_quoted(ray_t* rows) {
    int64_t n = ray_len(rows);
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    if (RAY_IS_ERR(out)) { ray_release(rows); return out; }
    ray_t** e = (ray_t**)ray_data(rows);
    char* acc = NULL;                        /* pending logical row */
    size_t used = 0;                         /* bytes of acc in use */
    size_t quotes = 0;                       /* running '"' parity  */
    for (int64_t i = 0; i < n; i++) {
        const char* p = ray_str_ptr(e[i]);
        size_t l = ray_str_len(e[i]);
        char* na = (char*)realloc(acc, used + l + 2);
        if (!na) { free(acc); ray_release(out); ray_release(rows); return ray_error("oom", NULL); }
        acc = na;
        if (used) acc[used++] = '\n';        /* rejoin the split line (codex P2:
                                              * the old spare-byte scheme wrote
                                              * the '\n' where the next row's
                                              * memcpy landed, corrupting it) */
        memcpy(acc + used, p, l);
        used += l;
        for (size_t k = 0; k < l; k++) if (p[k] == '"') quotes++;
        if ((quotes & 1) == 0) {
            ray_t* s = ray_str(acc, used);
            out = ray_list_append(out, s);
            ray_release(s);
            free(acc); acc = NULL; used = 0; quotes = 0;
            if (RAY_IS_ERR(out)) { ray_release(rows); return out; }
        }
    }
    if (acc) {                               /* unterminated tail row */
        ray_t* s = ray_str(acc, used);
        out = ray_list_append(out, s);
        ray_release(s);
        free(acc);
        if (RAY_IS_ERR(out)) { ray_release(rows); return out; }
    }
    ray_release(rows);
    return out;
}

/* Split one row into fields on a delimiter; '"'-opened fields are
 * quote-scanned ('""' -> literal '"').  Appends OWNED strings to `fields`. */
static ray_t* q_ft_fields(ray_t* fields, const char* r, size_t n, char delim) {
    size_t i = 0;
    for (;;) {
        char* fb = (char*)malloc(n + 1);
        if (!fb) { ray_release(fields); return ray_error("oom", NULL); }
        size_t fl = 0;
        if (i < n && r[i] == '"') {
            i++;
            while (i < n) {
                if (r[i] == '"') {
                    if (i + 1 < n && r[i + 1] == '"') { fb[fl++] = '"'; i += 2; }
                    else { i++; break; }
                } else fb[fl++] = r[i++];
            }
            while (i < n && r[i] != delim) i++;             /* junk after quote */
        } else {
            while (i < n && r[i] != delim) fb[fl++] = r[i++];
        }
        ray_t* f = ray_str(fb, fl);
        free(fb);
        fields = ray_list_append(fields, f);
        ray_release(f);
        if (RAY_IS_ERR(fields)) return fields;
        if (i >= n) break;
        i++;                                                /* past delim */
        if (i == n) {                                       /* trailing delim */
            ray_t* z = ray_str("", 0);
            fields = ray_list_append(fields, z);
            ray_release(z);
            break;
        }
    }
    return fields;
}

/* Parse one field per its column recipe.  OWNED atom/string. */
static ray_t* q_ft_parse_field(ray_t* field, int8_t tag, int is_str) {
    if (is_str) { ray_retain(field); return field; }
    return q_tok_to(tag, field);
}

/* Collapse a column accumulator: '*' columns stay lists of strings; typed
 * columns Tok-parse (q_tok_to distributes over lists) then collapse. */
static ray_t* q_ft_finish_col(ray_t* colacc, int8_t tag, int is_str) {
    if (is_str) { ray_retain(colacc); return colacc; }
    ray_t* parsed = q_tok_to(tag, colacc);
    if (!parsed || RAY_IS_ERR(parsed)) return parsed;
    ray_t* v = q_collapse_list(parsed);                     /* owned */
    ray_release(parsed);
    return v;
}

static ray_t* q_ft_load_csv(ray_t* types, ray_t* delimspec, ray_t* flag, ray_t* y) {
    const char* ts = ray_str_ptr(types);
    size_t nt = ray_str_len(types);
    if (nt == 0) return ray_error("type", "0:: empty types string");
    /* delimiter: char atom (len-1 string) or enlisted -> header row */
    char delim;
    int header = 0;
    if (delimspec && delimspec->type == -RAY_STR && ray_str_len(delimspec) == 1)
        delim = ray_str_ptr(delimspec)[0];
    else if (delimspec &&
             (delimspec->type == RAY_LIST || delimspec->type == RAY_STR) &&
             ray_len(delimspec) == 1) {
        /* enlisted delimiter -> first row is column names.  `enlist ","` is
         * an engine STR VECTOR (10h), a boxed 1-list also accepted. */
        ray_t* ia = ray_i64(0);
        ray_t* d0 = ray_at_fn(delimspec, ia);
        ray_release(ia);
        if (!d0 || RAY_IS_ERR(d0)) return d0 ? d0 : ray_error("oom", NULL);
        if (d0->type != -RAY_STR || ray_str_len(d0) != 1) {
            ray_release(d0);
            return ray_error("type", "0:: bad delimiter");
        }
        delim = ray_str_ptr(d0)[0];
        ray_release(d0);
        header = 1;
    } else return ray_error("type", "0:: bad delimiter");
    int embed_nl = 0;
    if (flag) {
        if (!q_is_int_atom(flag)) return ray_error("type", "0:: flag must be 0 or 1");
        embed_nl = q_iatom_val(flag) != 0;
    }
    /* column recipes */
    int8_t* tags = (int8_t*)malloc(nt);
    int* fstr = (int*)malloc(nt * sizeof(int));
    int* fskip = (int*)malloc(nt * sizeof(int));
    if (!tags || !fstr || !fskip) { free(tags); free(fstr); free(fskip); return ray_error("oom", NULL); }
    for (size_t j = 0; j < nt; j++) {
        tags[j] = q_ft_tag(ts[j], &fstr[j], &fskip[j]);
        if (!tags[j] && !fstr[j] && !fskip[j]) {
            char bad = ts[j];
            free(tags); free(fstr); free(fskip);
            if (bad == 'C')
                return ray_error("nyi", "0:: type char C needs the char type (string-model C3)");
            return ray_error("type", "0:: bad column type char '%c'", bad);
        }
    }
    int single = 0;
    ray_t* rows = q_ft_rows(y, &single);
    if (!rows || RAY_IS_ERR(rows)) { free(tags); free(fstr); free(fskip); return rows; }
    if (embed_nl) {
        rows = q_ft_merge_quoted(rows);
        if (!rows || RAY_IS_ERR(rows)) { free(tags); free(fstr); free(fskip); return rows; }
    }
    ray_t** rp = (ray_t**)ray_data(rows);
    int64_t nrows = ray_len(rows);
    ray_t* result = NULL;
    if (single) {
        /* one delimited string -> list of parsed atoms */
        ray_t* fields = ray_list_new((int64_t)nt);
        if (RAY_IS_ERR(fields)) { result = fields; goto done; }
        fields = q_ft_fields(fields, ray_str_ptr(rp[0]), ray_str_len(rp[0]), delim);
        if (RAY_IS_ERR(fields)) { result = fields; goto done; }
        ray_t** fp = (ray_t**)ray_data(fields);
        int64_t nf = ray_len(fields);
        ray_t* out = ray_list_new((int64_t)nt);
        if (RAY_IS_ERR(out)) { ray_release(fields); result = out; goto done; }
        ray_t* empty = ray_str("", 0);
        for (size_t j = 0; j < nt && !RAY_IS_ERR(out); j++) {
            if (fskip[j]) continue;
            ray_t* f = (int64_t)j < nf ? fp[j] : empty;     /* borrowed */
            ray_t* a = q_ft_parse_field(f, tags[j], fstr[j]);
            if (!a || RAY_IS_ERR(a)) { ray_release(empty); ray_release(fields); ray_release(out); result = a ? a : ray_error("oom", NULL); goto done; }
            out = ray_list_append(out, a);
            ray_release(a);
        }
        ray_release(empty);
        ray_release(fields);
        result = out;
        goto done;
    }
    {
        /* rows mode: per-column accumulators over the data rows */
        int64_t first = header ? 1 : 0;
        int64_t ndata = nrows - first;
        if (ndata < 0) ndata = 0;
        int64_t nout = 0;
        for (size_t j = 0; j < nt; j++) if (!fskip[j]) nout++;
        ray_t** acc = (ray_t**)calloc((size_t)(nout > 0 ? nout : 1), sizeof(ray_t*));
        if (!acc) { result = ray_error("oom", NULL); goto done; }
        int64_t k = 0;
        for (size_t j = 0; j < nt; j++) {
            if (fskip[j]) continue;
            acc[k] = ray_list_new(ndata > 0 ? ndata : 1);
            if (RAY_IS_ERR(acc[k])) {
                result = acc[k];
                for (int64_t z = 0; z < k; z++) ray_release(acc[z]);
                free(acc);
                goto done;
            }
            k++;
        }
        ray_t* empty = ray_str("", 0);
        for (int64_t i = first; i < nrows; i++) {
            ray_t* fields = ray_list_new((int64_t)nt);
            if (!RAY_IS_ERR(fields))
                fields = q_ft_fields(fields, ray_str_ptr(rp[i]), ray_str_len(rp[i]), delim);
            if (RAY_IS_ERR(fields)) {
                for (int64_t z = 0; z < nout; z++) ray_release(acc[z]);
                free(acc); ray_release(empty);
                result = fields;
                goto done;
            }
            ray_t** fp = (ray_t**)ray_data(fields);
            int64_t nf = ray_len(fields);
            int64_t c = 0;
            for (size_t j = 0; j < nt; j++) {
                if (fskip[j]) continue;
                ray_t* f = (int64_t)j < nf ? fp[j] : empty; /* borrowed */
                acc[c] = ray_list_append(acc[c], f);
                c++;
            }
            ray_release(fields);
        }
        ray_release(empty);
        /* finish columns */
        ray_t* cols = ray_list_new(nout > 0 ? nout : 1);
        for (int64_t z = 0; z < nout && !RAY_IS_ERR(cols); z++) {
            int64_t j = -1, seen = -1;
            for (size_t t = 0; t < nt; t++) {
                if (fskip[t]) continue;
                if (++seen == z) { j = (int64_t)t; break; }
            }
            ray_t* col = q_ft_finish_col(acc[z], tags[j], fstr[j]);
            if (!col || RAY_IS_ERR(col)) {
                ray_release(cols);
                cols = col ? col : ray_error("oom", NULL);
                break;
            }
            cols = ray_list_append(cols, col);
            ray_release(col);
        }
        for (int64_t z = 0; z < nout; z++) ray_release(acc[z]);
        free(acc);
        if (RAY_IS_ERR(cols)) { result = cols; goto done; }
        if (!header) { result = cols; goto done; }
        /* header: first row = column names -> table */
        ray_t* nmf = ray_list_new((int64_t)nt);
        if (!RAY_IS_ERR(nmf) && nrows > 0)
            nmf = q_ft_fields(nmf, ray_str_ptr(rp[0]), ray_str_len(rp[0]), delim);
        if (RAY_IS_ERR(nmf)) { ray_release(cols); result = nmf; goto done; }
        ray_t** np = (ray_t**)ray_data(nmf);
        int64_t nn = ray_len(nmf);
        ray_t* tbl = ray_table_new(nout > 0 ? nout : 1);
        int64_t c2 = 0;
        ray_t** cp = (ray_t**)ray_data(cols);
        for (size_t j = 0; j < nt && !RAY_IS_ERR(tbl); j++) {
            if (fskip[j]) continue;
            int64_t nm = (int64_t)j < nn
                ? ray_sym_intern_runtime(ray_str_ptr(np[j]), ray_str_len(np[j]))
                : ray_sym_intern_runtime("", 0);
            tbl = ray_table_add_col(tbl, nm, cp[c2]);
            c2++;
        }
        ray_release(nmf);
        ray_release(cols);
        result = tbl;
    }
done:
    ray_release(rows);
    free(tags); free(fstr); free(fskip);
    return result;
}

static ray_t* q_ft_load_fixed(ray_t* types, ray_t* widths, ray_t* y) {
    const char* ts = ray_str_ptr(types);
    size_t nt = ray_str_len(types);
    if (nt == 0 || (int64_t)nt != ray_len(widths))
        return ray_error("length", "0:: types and widths must have equal count");
    /* widths must be positive (codex P1: a negative width made the slice
     * length negative and reached memcpy as a huge size_t). */
    for (int64_t j = 0; j < (int64_t)nt; j++)
        if (q_ivec_get(widths, j) <= 0)
            return ray_error("domain", "0:: field widths must be positive");
    int8_t* tags = (int8_t*)malloc(nt);
    int* fstr = (int*)malloc(nt * sizeof(int));
    int* fskip = (int*)malloc(nt * sizeof(int));
    if (!tags || !fstr || !fskip) { free(tags); free(fstr); free(fskip); return ray_error("oom", NULL); }
    for (size_t j = 0; j < nt; j++) {
        tags[j] = q_ft_tag(ts[j], &fstr[j], &fskip[j]);
        if (!tags[j] && !fstr[j] && !fskip[j]) {
            free(tags); free(fstr); free(fskip);
            return ray_error("type", "0:: bad column type char");
        }
    }
    int single = 0;
    ray_t* rows = q_ft_rows(y, &single);
    if (!rows || RAY_IS_ERR(rows)) { free(tags); free(fstr); free(fskip); return rows; }
    ray_t** rp = (ray_t**)ray_data(rows);
    int64_t nrows = ray_len(rows);
    int64_t nout = 0;
    for (size_t j = 0; j < nt; j++) if (!fskip[j]) nout++;
    ray_t* result = NULL;
    ray_t** acc = (ray_t**)calloc((size_t)(nout > 0 ? nout : 1), sizeof(ray_t*));
    if (!acc) { result = ray_error("oom", NULL); goto done; }
    for (int64_t z = 0; z < nout; z++) {
        acc[z] = ray_list_new(nrows > 0 ? nrows : 1);
        if (RAY_IS_ERR(acc[z])) {
            result = acc[z];
            for (int64_t q = 0; q < z; q++) ray_release(acc[q]);
            free(acc);
            goto done;
        }
    }
    for (int64_t i = 0; i < nrows; i++) {
        const char* p = ray_str_ptr(rp[i]);
        int64_t n = (int64_t)ray_str_len(rp[i]);
        int64_t pos = 0, c = 0;
        for (size_t j = 0; j < nt; j++) {
            int64_t w = q_ivec_get(widths, (int64_t)j);
            int64_t s = pos > n ? n : pos;
            int64_t e = pos + w > n ? n : pos + w;
            pos += w;
            if (fskip[j]) continue;
            ray_t* f = ray_str(p + s, (size_t)(e - s));
            if (!f || RAY_IS_ERR(f)) {
                for (int64_t z = 0; z < nout; z++) ray_release(acc[z]);
                free(acc);
                result = f ? f : ray_error("oom", NULL);
                goto done;
            }
            acc[c] = ray_list_append(acc[c], f);
            ray_release(f);
            c++;
        }
    }
    {
        ray_t* cols = ray_list_new(nout > 0 ? nout : 1);
        int64_t z = 0;
        for (size_t j = 0; j < nt && !RAY_IS_ERR(cols); j++) {
            if (fskip[j]) continue;
            ray_t* col = q_ft_finish_col(acc[z], tags[j], fstr[j]);
            if (!col || RAY_IS_ERR(col)) {
                ray_release(cols);
                cols = col ? col : ray_error("oom", NULL);
                break;
            }
            cols = ray_list_append(cols, col);
            ray_release(col);
            z++;
        }
        for (int64_t q = 0; q < nout; q++) ray_release(acc[q]);
        free(acc);
        result = cols;
    }
done:
    ray_release(rows);
    free(tags); free(fstr); free(fskip);
    return result;
}

/* ---- Key-Value Pairs: "K f [*] r" 0: string ------------------------------ */
static ray_t* q_ft_kv(const char* spec, size_t sn, ray_t* y) {
    char ktype = spec[0];
    int star = sn == 4;
    if (star && spec[2] != '*') return ray_error("type", "0:: bad key-value spec");
    char fsep = spec[1];
    char rsep = star ? spec[3] : spec[2];
    if (!y || y->type != -RAY_STR)
        return ray_error("type", "0:: key-value parse needs a string");
    int8_t ktag;
    {
        int is_str = 0, is_skip = 0;
        ktag = q_ft_tag(ktype, &is_str, &is_skip);
        if (!ktag) return ray_error("type", "0:: bad key-value key type");
    }
    const char* p = ray_str_ptr(y);
    size_t n = ray_str_len(y);
    ray_t* keys = ray_list_new(4);
    if (RAY_IS_ERR(keys)) return keys;
    ray_t* vals = ray_list_new(4);
    if (RAY_IS_ERR(vals)) { ray_release(keys); return vals; }
    size_t i = 0;
    while (i < n) {
        /* one record: up to rsep (quote-aware in '*' mode) */
        size_t start = i;
        int inq = 0;
        while (i < n && (inq || p[i] != rsep)) {
            if (star && p[i] == '"') inq = !inq;
            i++;
        }
        size_t end = i;
        if (i < n) i++;                                     /* past rsep */
        if (end == start) continue;                         /* empty record */
        /* split at the FIRST fsep */
        size_t f = start;
        while (f < end && p[f] != fsep) f++;
        ray_t* k = ray_str(p + start, f - start);
        size_t vs = f < end ? f + 1 : end;
        ray_t* v;
        if (star && vs < end && p[vs] == '"' && p[end - 1] == '"' && end - vs >= 2) {
            /* quoted value: strip the outer quotes, un-double inner ones */
            char* vb = (char*)malloc(end - vs);
            size_t vl = 0;
            if (!vb) { ray_release(k); ray_release(keys); ray_release(vals); return ray_error("oom", NULL); }
            for (size_t t = vs + 1; t < end - 1; t++) {
                if (p[t] == '"' && t + 1 < end - 1 && p[t + 1] == '"') { vb[vl++] = '"'; t++; }
                else vb[vl++] = p[t];
            }
            v = ray_str(vb, vl);
            free(vb);
        } else v = ray_str(p + vs, end - vs);
        if (!k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v)) {
            if (k && !RAY_IS_ERR(k)) ray_release(k);
            if (v && !RAY_IS_ERR(v)) ray_release(v);
            ray_release(keys); ray_release(vals);
            return ray_error("oom", NULL);
        }
        ray_t* ka = q_tok_to(ktag, k);
        ray_release(k);
        if (!ka || RAY_IS_ERR(ka)) { ray_release(v); ray_release(keys); ray_release(vals); return ka ? ka : ray_error("oom", NULL); }
        keys = ray_list_append(keys, ka);
        ray_release(ka);
        vals = ray_list_append(vals, v);
        ray_release(v);
        if (RAY_IS_ERR(keys) || RAY_IS_ERR(vals)) {
            ray_t* err = RAY_IS_ERR(keys) ? keys : vals;
            if (!RAY_IS_ERR(keys)) ray_release(keys);
            if (!RAY_IS_ERR(vals)) ray_release(vals);
            return err;
        }
    }
    ray_t* kv = q_collapse_list(keys);                      /* typed key vector */
    ray_release(keys);
    if (!kv || RAY_IS_ERR(kv)) { ray_release(vals); return kv ? kv : ray_error("oom", NULL); }
    ray_t* out = ray_list_new(2);
    if (RAY_IS_ERR(out)) { ray_release(kv); ray_release(vals); return out; }
    out = ray_list_append(out, kv);
    ray_release(kv);
    if (RAY_IS_ERR(out)) { ray_release(vals); return out; }
    out = ray_list_append(out, vals);
    ray_release(vals);
    return out;
}

/* q `sum x` — LIST arm sums the items (kdb: `sum(2013.03.15;18:55:40.686)`
 * is a timestamp; Load Fixed pins `sum("DT";8 9)0:enlist"…"`).  Non-lists
 * keep the base vector aggregate. */
static ray_t* q_sum_wrap(ray_t* x) {
    if (x && x->type == RAY_LIST && ray_len(x) > 0) {
        /* fold q `+` over the items via call_fn2 (the ATOMIC dispatch —
         * ray_add_fn alone is the atom kernel; vectors broadcast in eval). */
        ray_t* plus = q_registry_lookup_name("+", 1, Q_DYADIC);   /* borrowed */
        if (!plus) return ray_error("type", "sum: + unavailable");
        ray_t** e = (ray_t**)ray_data(x);
        ray_t* acc = e[0];
        ray_retain(acc);
        for (int64_t i = 1; i < ray_len(x); i++) {
            ray_t* nx = call_fn2(plus, acc, e[i]);
            ray_release(acc);
            if (!nx || RAY_IS_ERR(nx)) return nx ? nx : ray_error("oom", NULL);
            acc = nx;
        }
        return acc;
    }
    return ray_sum_fn(x);
}

/* ---- the `0:` dispatcher -------------------------------------------------- */
static ray_t* q_filetext_wrap(ray_t* x, ray_t* y) {
    if (!x) return ray_error("type", "0:: nil left operand");
    if (x->type == -RAY_SYM) return q_ft_save_text(x, y);
    if (x->type == -RAY_STR) {
        const char* s = ray_str_ptr(x);
        size_t n = ray_str_len(x);
        if (n == 1) return q_ft_prepare(s[0], y);
        if ((n == 3 || n == 4) && (s[0] == 'S' || s[0] == 'I' || s[0] == 'J'))
            return q_ft_kv(s, n, y);
        return ray_error("type", "0:: bad left operand");
    }
    if (x->type == RAY_LIST && (ray_len(x) == 2 || ray_len(x) == 3)) {
        ray_t** e = (ray_t**)ray_data(x);
        if (e[0] && e[0]->type == -RAY_STR) {
            if (ray_len(x) == 2 && q_is_int_vec(e[1]))
                return q_ft_load_fixed(e[0], e[1], y);
            return q_ft_load_csv(e[0], e[1], ray_len(x) == 3 ? e[2] : NULL, y);
        }
    }
    return ray_error("type", "0:: unsupported left operand");
}

/* ===== string search family: like / ss / ssr (feat/q-string-fns) =========== */

/* q `x like p` — glob match.  Reuses ray_like_fn for sym/str atoms and
 * vectors; a DICT maps the match over its values (kdb: keys kept). */
static ray_t* q_like_wrap(ray_t* x, ray_t* pattern) {
    if (x && x->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(x);   /* borrowed */
        ray_t* v = ray_dict_vals(x);   /* borrowed */
        if (!k || !v) return ray_error("type", "like: bad dict");
        ray_t* nv = q_like_wrap(v, pattern);
        if (!nv || RAY_IS_ERR(nv)) return nv;
        ray_retain(k);
        return ray_dict_new(k, nv);    /* consumes k + nv */
    }
    if (x && x->type == RAY_LIST) {    /* boxed list (e.g. dict values) */
        int64_t n = ray_len(x);
        ray_t* out = ray_list_new(n > 0 ? n : 1);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            ray_t* ia = ray_i64(i);
            ray_t* e = ray_at_fn(x, ia);
            ray_release(ia);
            if (!e || RAY_IS_ERR(e)) { ray_release(out); return e; }
            ray_t* r = q_like_wrap(e, pattern);
            ray_release(e);
            if (!r || RAY_IS_ERR(r)) { ray_release(out); return r; }
            out = ray_list_append(out, r);
            ray_release(r);
            if (RAY_IS_ERR(out)) return out;
        }
        ray_t* c = q_collapse_list(out);   /* homogeneous bool run -> bool vec */
        ray_release(out);
        return c;
    }
    return ray_like_fn(x, pattern);
}

/* Match glob pattern p[0..pn) anchored at s[pos..sn), where every pattern
 * token consumes exactly one input char: `?` (any), `[..]`/`[^..]` (class,
 * optional negation), or a literal.  The variable-width `*` form is not used
 * by q's ss/ssr, so a bare `*` is treated literally.  Returns the number of
 * input chars consumed on a full match, or -1 on mismatch. */
static int64_t q_glob_fixed_at(const char* s, size_t sn, size_t pos,
                               const char* p, size_t pn) {
    size_t si = pos, pi = 0;
    while (pi < pn) {
        if (si >= sn) return -1;
        char pc = p[pi];
        if (pc == '?') { pi++; si++; continue; }
        if (pc == '[') {
            size_t j = pi + 1;
            int neg = 0;
            if (j < pn && p[j] == '^') { neg = 1; j++; }
            int matched = 0;
            for (; j < pn && p[j] != ']'; j++)
                if (p[j] == s[si]) matched = 1;
            if (j < pn) j++;              /* skip ']' */
            if (matched == neg) return -1;
            pi = j; si++; continue;
        }
        if (pc != s[si]) return -1;       /* literal */
        pi++; si++;
    }
    return (int64_t)(si - pos);
}

/* q `s ss p` — string search: 0-based start index of every match of the glob
 * pattern p in the string s (overlapping, kdb-true).  Returns a long vector. */
static ray_t* q_ss_wrap(ray_t* s, ray_t* p) {
    if (!s || s->type != -RAY_STR || !p || p->type != -RAY_STR)
        return ray_error("type", "ss: expects string arguments");
    const char* sp = ray_str_ptr(s); size_t sn = ray_str_len(s);
    const char* pp = ray_str_ptr(p); size_t pn = ray_str_len(p);
    ray_t* out = ray_vec_new(RAY_I64, 8);
    if (RAY_IS_ERR(out)) return out;
    if (pn == 0) return out;                       /* empty pattern -> no hits */
    /* q_glob_fixed_at anchors at each position; the match WIDTH differs from
     * the pattern's byte length (a `[..]` class is 1 input char), so iterate
     * every start position and let the matcher enforce bounds. */
    for (size_t i = 0; i < sn; i++) {
        if (q_glob_fixed_at(sp, sn, i, pp, pn) >= 0) {
            int64_t idx = (int64_t)i;
            out = ray_vec_append(out, &idx);
            if (RAY_IS_ERR(out)) return out;
        }
    }
    return out;
}

/* q `ssr[s;p;r]` — replace every (non-overlapping, left-to-right) match of the
 * glob pattern p in s.  r is either a replacement string, or a function
 * applied to each matched substring (kdb: `ssr[s;"t?r";upper]`). */
ray_t* q_ssr_wrap(ray_t** args, int64_t n) {
    if (n != 3) return ray_error("rank", "ssr: expects 3 args");
    ray_t* s = args[0]; ray_t* p = args[1]; ray_t* r = args[2];
    if (!s || s->type != -RAY_STR || !p || p->type != -RAY_STR)
        return ray_error("type", "ssr: s and p must be strings");
    int r_is_fn = q_is_fn_value(r);
    if (!r_is_fn && (!r || r->type != -RAY_STR))
        return ray_error("type", "ssr: replacement must be a string or function");
    const char* sp = ray_str_ptr(s); size_t sn = ray_str_len(s);
    const char* pp = ray_str_ptr(p); size_t pn = ray_str_len(p);
    size_t cap = sn + 16, blen = 0;
    char* b = (char*)malloc(cap);
    if (!b) return ray_error("wsfull", "ssr: out of memory");
    #define SSR_PUSH(PTR, L) do { \
        size_t _l = (L); \
        if (blen + _l > cap) { cap = (blen + _l) * 2; char* nb = (char*)realloc(b, cap); \
            if (!nb) { free(b); return ray_error("wsfull", "ssr: out of memory"); } b = nb; } \
        memcpy(b + blen, (PTR), _l); blen += _l; } while (0)
    ray_t* err = NULL;
    size_t i = 0;
    while (i < sn) {
        /* q_glob_fixed_at bounds-checks internally (si>=sn -> -1) and a `[..]`
         * class is multiple pattern bytes but ONE input char, so do NOT gate on
         * `i + pn <= sn` (that rejected bracket-class matches near the end). */
        int64_t m = pn ? q_glob_fixed_at(sp, sn, i, pp, pn) : -1;
        if (m >= 0) {
            if (r_is_fn) {
                ray_t* sub = ray_str(sp + i, (size_t)m);
                ray_t* one[1] = { sub };
                ray_t* rep = q_call_n(r, one, 1);
                ray_release(sub);
                if (!rep || RAY_IS_ERR(rep)) { err = rep; break; }
                if (rep->type != -RAY_STR) { ray_release(rep); err = ray_error("type", "ssr: replacement fn must return a string"); break; }
                SSR_PUSH(ray_str_ptr(rep), ray_str_len(rep));
                ray_release(rep);
            } else {
                SSR_PUSH(ray_str_ptr(r), ray_str_len(r));
            }
            i += (m > 0) ? (size_t)m : 1;   /* advance past the match */
        } else {
            SSR_PUSH(sp + i, 1);
            i++;
        }
    }
    #undef SSR_PUSH
    if (err) { free(b); return err; }
    ray_t* out = ray_str(b, blen);
    free(b);
    return out;
}

/* q `getenv x` (ref/getenv.md) — x is a SYMBOL atom naming an environment
 * variable; returns its value as a string, or "" when the variable is unset
 * (kdb-true, and exactly what the base ray_getenv_fn already returns for a
 * missing var).  The base primitive wants a -RAY_STR arg, so coerce the
 * symbol's name to a string atom first — the ONLY divergence from the raw C,
 * hence a wrapper rather than a QK_ENV rename.
 * String-model seam: the result is a native -RAY_STR atom, so `type getenv`X`
 * is -10h where kdb's char vector is 10h (a known, tracked divergence). */
static ray_t* q_getenv_wrap(ray_t* x) {
    /* .os.getenv is RAY_FN_RESTRICTED; calling the C fn directly bypasses the
     * eval-layer check, so re-assert it here (the q_hopen_wrap/file precedent). */
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    if (!x || x->type != -RAY_SYM)
        return ray_error("type", "getenv: expected a symbol, got %s",
                         ray_type_name(x ? x->type : 0));
    ray_t* s = ray_sym_str(x->i64);                     /* borrowed */
    if (!s) return ray_error("type", "getenv: bad symbol");
    ray_t* name = ray_str(ray_str_ptr(s), ray_str_len(s));  /* owned -RAY_STR */
    if (!name || RAY_IS_ERR(name)) return name ? name : ray_error("oom", NULL);
    ray_t* r = ray_getenv_fn(name);                     /* "" when unset */
    ray_release(name);
    return r;
}

/* q `x setenv y` (ref/getenv.md#setenv) — x is a SYMBOL atom (the variable
 * name), y is a string.  Sets the environment variable and returns generic
 * null (kdb: setenv's result displays as nothing in the console).  The base
 * ray_setenv_fn takes two -RAY_STR args and echoes y retained; coerce the sym
 * name to a string, discard that echo, and return :: to match kdb. */
static ray_t* q_setenv_wrap(ray_t* x, ray_t* y) {
    /* .os.setenv is RAY_FN_RESTRICTED; re-assert here (calling the C fn directly
     * bypasses the eval-layer check — the q_hopen_wrap/file-wrapper precedent). */
    if (ray_eval_get_restricted()) return ray_error("access", "restricted");
    if (!x || x->type != -RAY_SYM)
        return ray_error("type", "setenv: name must be a symbol, got %s",
                         ray_type_name(x ? x->type : 0));
    if (!y || y->type != -RAY_STR)
        return ray_error("type", "setenv: value must be a string, got %s",
                         ray_type_name(y ? y->type : 0));
    ray_t* s = ray_sym_str(x->i64);                     /* borrowed */
    if (!s) return ray_error("type", "setenv: bad symbol");
    ray_t* name = ray_str(ray_str_ptr(s), ray_str_len(s));  /* owned -RAY_STR */
    if (!name || RAY_IS_ERR(name)) return name ? name : ray_error("oom", NULL);
    ray_t* r = ray_setenv_fn(name, y);                  /* echoes y, or error */
    ray_release(name);
    if (r && RAY_IS_ERR(r)) return r;
    if (r) ray_release(r);                              /* discard echoed value */
    return RAY_NULL_OBJ;                                /* kdb: setenv -> :: */
}

/* ---- value builders keyed by manifest build-kind ---- */

/* Identity/rename-reuse: snapshot an existing rayfall builtin value by name and
 * retain it (the registry owns one ref).  Returns NULL if the audited source is
 * absent — a real bug, so q_registry_init fails fast. */
static ray_t* build_env(const char* env_name) {
    ray_t* e = ray_env_get(ray_sym_intern(env_name, strlen(env_name)));
    if (!e) return NULL;
    ray_retain(e);
    return e;
}

/* Bespoke wrapper: aux-name = the canonical rayfall lowering name; flagged
 * RAY_FN_Q_LOWER so the compiler/query DAG name-route it.
 *
 * SERDE LIMITATION (codex P1, deferred to 2b): generic function serialization
 * (src/store/serde.c) writes ray_fn_name and deserializes via ray_env_get, so a
 * serialized wrapper would come back as the plain like-named builtin (losing the
 * q string/arg-swap semantics), and `_`->"drop" has no env binding at all.  This
 * is NOT reachable in stage 2a: with no parser flip, wrapper VALUES never become
 * user-visible or serializable — they exist only inside the registry and the
 * transient AST that q_lower feeds straight to eval.  (The pre-2a wrappers, named
 * "="/"#"/"_", were equally non-round-trippable — env has no such names.)  2b,
 * which makes value heads user-visible, must teach serde to recognise
 * RAY_FN_Q_LOWER and re-derive the wrapper from the registry; that fix has a
 * testable surface only once the parser embeds these values. */
static ray_t* build_wrapper(q_build_kind kind, const char* lower_name) {
    switch (kind) {
    case QK_EQ:   return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_eq_wrap);
    case QK_NE:   return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_ne_wrap);
    case QK_TAKE: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_take_wrap);
    case QK_DROP: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_drop_wrap);
    case QK_CUT:  return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_cut_wrap);
    case QK_EACH: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_each_wrap);
    case QK_MATCH:return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_match_wrap);
    case QK_FLOOR:return ray_fn_unary (lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_floor_wrap);
    case QK_BANG: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_bang_wrap);
    case QK_KEY:  return ray_fn_unary (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_key_wrap);
    case QK_VALUE:return ray_fn_unary (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_value_wrap);
    case QK_DISTINCT:
                  return ray_fn_unary (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_distinct_wrap);
    case QK_ROLL: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_roll_wrap);
    case QK_RAND: return ray_fn_unary (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_rand_wrap);
    case QK_CAST: return ray_fn_binary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_cast_wrap);
    case QK_AT:   return ray_fn_vary  (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_at_wrap);
    case QK_DOT:  return ray_fn_vary  (lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_dot_wrap);
    case QK_MIN2: return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_min2_wrap);
    case QK_NEG:  return ray_fn_unary (lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_neg_wrap);
    case QK_WITHIN: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_within_wrap);
    case QK_OVER:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_over_kw);
    case QK_SCANKW: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_scan_kw);
    case QK_PRIORKW:return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_prior_kw);
    case QK_DELTAS: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_deltas_wrap);
    case QK_DIFFER: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_differ_wrap);
    case QK_TIL:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_til_wrap);
    case QK_WHERE:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_where_wrap);
    case QK_REV:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_reverse_wrap);
    case QK_JOIN:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_join_wrap);
    case QK_VS:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_vs_wrap);
    case QK_SV:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_sv_wrap);
    case QK_SUMS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_sums_wrap);
    case QK_PRDS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_prds_wrap);
    case QK_PRD:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_prd_wrap);
    case QK_MAXS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_maxs_wrap);
    case QK_MINS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mins_wrap);
    case QK_AVGS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_avgs_wrap);
    case QK_RATIOS: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ratios_wrap);
    case QK_WSUM:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_wsum_wrap);
    case QK_WAVG:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_wavg_wrap);
    case QK_COV:    return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_cov_wrap);
    case QK_SCOV:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_scov_wrap);
    case QK_MSUM:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_msum_wrap);
    case QK_MAVG:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mavg_wrap);
    case QK_MMAX:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mmax_wrap);
    case QK_MMIN:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mmin_wrap);
    case QK_MCOUNT: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mcount_wrap);
    case QK_MDEV:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_mdev_wrap);
    case QK_EMA:    return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ema_wrap);
    case QK_FILL:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_fill_wrap);
    case QK_ROTATE: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_rotate_wrap);
    case QK_SUBLIST:return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_sublist_wrap);
    case QK_NEXT:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_next_wrap);
    case QK_PREV:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_prev_wrap);
    case QK_XPREV:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xprev_wrap);
    case QK_IN:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_in_wrap);
    case QK_FILLS:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_fills_wrap);
    case QK_HOPEN:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_hopen_wrap);
    case QK_HCLOSE: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_hclose_wrap);
    case QK_FILETEXT: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_filetext_wrap);
    case QK_HSYM:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_hsym_wrap);
    case QK_READ0:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_read0_wrap);
    /* NB: deliberately NOT RAY_FN_AGGR — the eval aggregate fast path claims
     * AGGR fns before the wrapper runs and 'types on a boxed list-of-vectors;
     * name-routing (RAY_FN_Q_LOWER + aux "sum") keeps query/DAG behaviour. */
    case QK_SUM:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_sum_wrap);
    case QK_NULL:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_null_wrap);
    case QK_RAZE:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_raze_wrap);
    case QK_LIKE:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_like_wrap);
    case QK_SS:     return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ss_wrap);
    case QK_SSR:    return ray_fn_vary  (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ssr_wrap);
    case QK_UNION:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_union_wrap);
    case QK_INTER:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_inter_wrap);
    case QK_CROSS:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_cross_wrap);
    case QK_SIN:        return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_sin_wrap);
    case QK_COS:        return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_cos_wrap);
    case QK_TAN:        return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_tan_wrap);
    case QK_ASIN:       return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_asin_wrap);
    case QK_ACOS:       return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_acos_wrap);
    case QK_ATAN:       return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_atan_wrap);
    case QK_RECIPROCAL: return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_reciprocal_wrap);
    /* signum drives its own broadcast (q_null_wrap pattern) so a top-level
     * boxed-list result collapses to an int vector — see q_signum_wrap. */
    case QK_SIGNUM:     return ray_fn_unary(lower_name, RAY_FN_NONE   | RAY_FN_Q_LOWER, q_signum_wrap);
    case QK_CEILING:    return ray_fn_unary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_ceiling_wrap);
    case QK_XEXP:       return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_xexp_wrap);
    case QK_XLOG:       return ray_fn_binary(lower_name, RAY_FN_ATOMIC | RAY_FN_Q_LOWER, q_xlog_wrap);
    case QK_XBAR:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xbar_wrap);
    case QK_ASC:    return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_asc_wrap);
    case QK_DESC:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_desc_wrap);
    case QK_IASC:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_iasc_wrap);
    case QK_IDESC:  return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_idesc_wrap);
    /* ---- table verbs (feat/q-table-verbs) ---- */
    case QK_FLIP:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_flip_wrap);
    case QK_KEYS:   return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_keys_wrap);
    case QK_UNGROUP:return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_ungroup_wrap);
    case QK_XKEY:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xkey_wrap);
    case QK_XCOL:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xcol_wrap);
    case QK_XCOLS:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xcols_wrap);
    case QK_XASC:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xasc_wrap);
    case QK_XDESC:  return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xdesc_wrap);
    case QK_XGROUP: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_xgroup_wrap);
    case QK_INSERT: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_insert_wrap);
    case QK_UPSERT: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_upsert_wrap);
    case QK_EXCEPT: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_except_wrap);
    case QK_SETG:   return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_setg_wrap);
    /* ---- environment variables (feat/q-getenv-setenv) ---- */
    case QK_GETENV: return ray_fn_unary (lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_getenv_wrap);
    case QK_SETENV: return ray_fn_binary(lower_name, RAY_FN_NONE | RAY_FN_Q_LOWER, q_setenv_wrap);
    default:      return NULL;
    }
}

/* Build the owned value for one (kind, target). */
static ray_t* build_value(q_build_kind kind, const char* target) {
    if (kind == QK_NONE) return NULL;
    if (kind == QK_ENV)  return build_env(target);
    return build_wrapper(kind, target);
}

/* Record one (name, valence) entry.  Returns RAY_OK, or RAY_ERR_DOMAIN if the
 * builder produced NULL/err for a non-QK_NONE kind (audited source missing). */
static ray_err_t add_entry(const char* name, q_valence_t valence,
                           q_build_kind kind, const char* target) {
    /* Bootstrap invariant (codex #1): entries are only ever built inside
     * q_registry_init's build window, and a builder must never re-enter the
     * parser.  build_value below touches only ray_env_get / ray_fn_* — never
     * q_parse — so this holds by construction; the assert pins it. */
    assert(g_building);
    if (kind == QK_NONE) return RAY_OK;                 /* nothing at this valence */
    ray_t* val = build_value(kind, target);
    if (!val || RAY_IS_ERR(val)) return RAY_ERR_DOMAIN; /* fail-fast: audited bug */
    entry_t* e    = &g_entries[g_count++];
    e->sym_id     = ray_sym_intern(name, strlen(name));
    e->valence    = valence;
    e->value      = val;
    e->spelling   = name;
    e->lower_name = target;                             /* rayfall routing name */
    e->is_wrapper = (kind != QK_ENV);
    return RAY_OK;
}

/* ---- serde hooks: q wrappers round-trip through the registry -------------
 * A RAY_FN_Q_LOWER wrapper serialized by aux-name would deserialize as the
 * like-named env builtin (wrong arg order for `#`, no binding at all for
 * `_`->"drop"), silently losing q semantics.  The writer claims registry
 * wrappers with a `q!<spelling>!<valence>` wire name (fits the standard
 * 15-byte slot); the reader decodes it back to THE registry value.  Internal
 * spelling-less values (scan/list) have no provenance and fall through to
 * the env path — documented, not silently wrong (env scan/list exist). */

static int q_serde_fn_writer(ray_t* fn, char out[16]) {
    if (!fn || !(fn->attrs & RAY_FN_Q_LOWER)) return 0;
    q_provenance_t pv;
    if (!q_registry_provenance(fn, &pv) || !pv.is_wrapper) return 0;
    int n = snprintf(out, 16, "q!%s!%d", pv.spelling, (int)pv.valence);
    return n > 0 && n < 16;
}

static ray_t* q_serde_fn_reader(const char* name) {
    if (!name || name[0] != 'q' || name[1] != '!') return NULL;
    const char* sp = name + 2;
    const char* bang = strrchr(sp, '!');
    if (!bang || bang == sp) return NULL;
    q_valence_t v = (q_valence_t)atoi(bang + 1);
    if (v != Q_MONADIC && v != Q_DYADIC) return NULL;
    ray_t* hit = q_registry_lookup_name(sp, (size_t)(bang - sp), v);
    if (!hit) return NULL;      /* falls through to the env path -> name error */
    ray_retain(hit);
    return hit;                 /* owned, per the hook contract */
}

/* ---- API ---- */

ray_err_t q_registry_init(void) {
    if (g_inited) return RAY_OK;   /* idempotent */
    g_count    = 0;
    g_building = true;
    int n = 0;
    const q_op_t* ops = q_ops_table(&n);
    /* Cap check: g_entries sized for 2 per row.  Static roster, so this is a
     * build-time invariant, asserted for future growth. */
    assert(2 * n <= (int)(sizeof g_entries / sizeof g_entries[0]));
    for (int i = 0; i < n; i++) {
        const q_op_t* op = &ops[i];
        if (add_entry(op->name, Q_MONADIC, op->mon_kind,  op->mon_target)  != RAY_OK) {
            g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
        }
        if (add_entry(op->name, Q_DYADIC,  op->dyad_kind, op->dyad_target) != RAY_OK) {
            g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
        }
    }
    /* internal (spelling-less) values consumed by q_lower / the parser */
    g_scan_value = ray_fn_vary("scan", RAY_FN_NONE | RAY_FN_Q_LOWER, q_scan_wrap);
    if (!g_scan_value || RAY_IS_ERR(g_scan_value)) {
        g_scan_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_over_value = ray_fn_vary("over", RAY_FN_NONE | RAY_FN_Q_LOWER, q_over_wrap);
    if (!g_over_value || RAY_IS_ERR(g_over_value)) {
        g_over_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_eachboth_value = ray_fn_vary("each-both", RAY_FN_NONE | RAY_FN_Q_LOWER, q_eachboth_wrap);
    if (!g_eachboth_value || RAY_IS_ERR(g_eachboth_value)) {
        g_eachboth_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_prior_value = ray_fn_vary("each-prior", RAY_FN_NONE | RAY_FN_Q_LOWER, q_prior_wrap);
    if (!g_prior_value || RAY_IS_ERR(g_prior_value)) {
        g_prior_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_mkderiv2_value = ray_fn_binary("q.mkderiv2", RAY_FN_NONE | RAY_FN_Q_LOWER, q_mkderiv2);
    if (!g_mkderiv2_value || RAY_IS_ERR(g_mkderiv2_value)) {
        g_mkderiv2_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    /* Both ctx constructor heads are SPECIAL_FORM: q_ctx_build must receive the
     * raw element trees to evaluate them right-to-left inside a pushed scope. */
    g_list_value = ray_fn_vary("list",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_list_build);
    if (!g_list_value || RAY_IS_ERR(g_list_value)) {
        g_list_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_table_value = ray_fn_vary("table",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_table_build);
    if (!g_table_value || RAY_IS_ERR(g_table_value)) {
        g_table_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_keyed_table_value = ray_fn_vary("keyed-table",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_keyed_table_build);
    if (!g_keyed_table_value || RAY_IS_ERR(g_keyed_table_value)) {
        g_keyed_table_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_select_value = ray_fn_vary("q.select",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_select_exec);
    if (!g_select_value || RAY_IS_ERR(g_select_value)) {
        g_select_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    /* compose builder — a NORMAL vary (args are the resolved function values). */
    g_compose_value = ray_fn_vary("q.compose", RAY_FN_Q_LOWER, q_compose_fn);
    if (!g_compose_value || RAY_IS_ERR(g_compose_value)) {
        g_compose_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    /* functional qSQL executors — SPECIAL FORMs: they receive t/c/b/a
     * UNEVALUATED and evaluate them internally (funsql_operand), so the `()`
     * empty marker (parser `::` name-ref) does not trip ray_eval's name check. */
    g_funsql_select_value = ray_fn_vary("q.funsql.select",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_funsql_select);
    if (!g_funsql_select_value || RAY_IS_ERR(g_funsql_select_value)) {
        g_funsql_select_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_funsql_bang_value = ray_fn_vary("q.funsql.bang",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_funsql_bang);
    if (!g_funsql_bang_value || RAY_IS_ERR(g_funsql_bang_value)) {
        g_funsql_bang_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_lambda_value = ray_fn_vary("q.fn",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_fn_make);
    if (!g_lambda_value || RAY_IS_ERR(g_lambda_value)) {
        g_lambda_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_ret_value = ray_fn_unary("q.ret", RAY_FN_Q_LOWER, q_ret_fn);
    if (!g_ret_value || RAY_IS_ERR(g_ret_value)) {
        g_ret_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_sig_value = ray_fn_unary("q.sig", RAY_FN_Q_LOWER, q_sig_fn);
    if (!g_sig_value || RAY_IS_ERR(g_sig_value)) {
        g_sig_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_seq_value = ray_fn_vary("q.seq",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_seq_fn);
    if (!g_seq_value || RAY_IS_ERR(g_seq_value)) {
        g_seq_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_if_value = ray_fn_vary("q.if",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_if_fn);
    if (!g_if_value || RAY_IS_ERR(g_if_value)) {
        g_if_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_do_value = ray_fn_vary("q.do",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_do_fn);
    if (!g_do_value || RAY_IS_ERR(g_do_value)) {
        g_do_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_while_value = ray_fn_vary("q.while",
                       RAY_FN_SPECIAL_FORM | RAY_FN_Q_LOWER, q_while_fn);
    if (!g_while_value || RAY_IS_ERR(g_while_value)) {
        g_while_value = NULL;
        g_building = false; q_registry_destroy(); return RAY_ERR_DOMAIN;
    }
    g_building = false;
    g_inited   = true;
    ray_serde_set_fn_hooks(q_serde_fn_writer, q_serde_fn_reader);
    return RAY_OK;
}

bool q_registry_ready(void) {
    return g_inited;
}

ray_t* q_registry_lookup(int64_t sym_id, q_valence_t valence) {
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].sym_id == sym_id && g_entries[i].valence == valence)
            return g_entries[i].value;   /* borrowed */
    return NULL;
}

ray_t* q_registry_lookup_name(const char* s, size_t n, q_valence_t valence) {
    return q_registry_lookup(ray_sym_intern(s, n), valence);
}

bool q_registry_provenance(const ray_t* value, q_provenance_t* out) {
    /* Wrapper values are UNIQUE objects (born rc=1 per row) — pointer identity
     * is exact for them.  Pass-through/rename values ARE the shared env builtin
     * object, and several q spellings can alias one env object (e.g. `#`-monadic
     * and `count` both embed env `count`; `-`-monadic and `neg` both embed env
     * `neg`).  For those, pointer identity alone cannot recover THE q spelling —
     * an inherent limitation of the reuse-the-env-object design.  So: prefer the
     * unique WRAPPER entry (always correct); for an aliased pass-through, return
     * the first-registered spelling (2b's formatter disambiguates aliased
     * pass-throughs from the parse-site glyph, not from this value-keyed API). */
    int first = -1;
    for (int i = 0; i < g_count; i++) {
        if (g_entries[i].value != value) continue;
        if (g_entries[i].is_wrapper) { first = i; break; }   /* unique — exact */
        if (first < 0) first = i;                            /* remember first */
    }
    if (first < 0) return false;
    if (out) {
        out->spelling   = g_entries[first].spelling;
        out->valence    = g_entries[first].valence;
        out->lower_name = g_entries[first].lower_name;
        out->is_wrapper = g_entries[first].is_wrapper;
    }
    return true;
}

/* Idempotent; also serves as partial-cleanup on a failed init (guards on
 * g_count, not g_inited, so a half-built table is fully released). */
void q_registry_destroy(void) {
    ray_serde_set_fn_hooks(NULL, NULL);   /* hooks read g_entries — detach first */
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].value) ray_release(g_entries[i].value);
    if (g_scan_value)  { ray_release(g_scan_value);  g_scan_value  = NULL; }
    if (g_over_value)     { ray_release(g_over_value);     g_over_value     = NULL; }
    if (g_eachboth_value) { ray_release(g_eachboth_value); g_eachboth_value = NULL; }
    if (g_prior_value)    { ray_release(g_prior_value);    g_prior_value    = NULL; }
    if (g_mkderiv2_value) { ray_release(g_mkderiv2_value); g_mkderiv2_value = NULL; }
    if (g_list_value)   { ray_release(g_list_value);   g_list_value   = NULL; }
    if (g_table_value)  { ray_release(g_table_value);  g_table_value  = NULL; }
    if (g_keyed_table_value) { ray_release(g_keyed_table_value); g_keyed_table_value = NULL; }
    if (g_select_value) { ray_release(g_select_value); g_select_value = NULL; }
    if (g_compose_value) { ray_release(g_compose_value); g_compose_value = NULL; }
    if (g_funsql_select_value) { ray_release(g_funsql_select_value); g_funsql_select_value = NULL; }
    if (g_funsql_bang_value)   { ray_release(g_funsql_bang_value);   g_funsql_bang_value   = NULL; }
    if (g_lambda_value)        { ray_release(g_lambda_value);        g_lambda_value        = NULL; }
    if (g_ret_value)           { ray_release(g_ret_value);           g_ret_value           = NULL; }
    if (g_sig_value)           { ray_release(g_sig_value);           g_sig_value           = NULL; }
    if (g_seq_value)           { ray_release(g_seq_value);           g_seq_value           = NULL; }
    if (g_if_value)            { ray_release(g_if_value);            g_if_value            = NULL; }
    if (g_do_value)            { ray_release(g_do_value);            g_do_value            = NULL; }
    if (g_while_value)         { ray_release(g_while_value);         g_while_value         = NULL; }
    if (g_qret_payload)        { ray_release(g_qret_payload);        g_qret_payload        = NULL; }
    if (g_qsig_payload)        { ray_release(g_qsig_payload);        g_qsig_payload        = NULL; }
    g_count  = 0;
    g_inited = false;
}
