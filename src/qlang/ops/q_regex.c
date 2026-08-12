/* q_regex — the RE2 verb surface: the `rlike` keyword and the `.regexp.i.*`
 * natives lib/regexp.q is written over.  Specification: user-docs/regexp.md.
 *
 * Regex verbs join the STRING-BOUNDARY exception catalogue beside like/ss/ssr
 * rather than declaring a family: a string IS a char vector, so no lift law can
 * tell one subject from a collection of them.  That distinction and the implicit
 * iteration over it live in ops/q_str.c (q_str_subject_many / _map, shared with
 * `like`) — every native here is one leaf plus one call to that shared map, so a
 * pattern applied to a million-row column compiles once and dispatches once.
 *
 * There is no options argument anywhere: flags ride inside the pattern as
 * (?i)/(?s)/(?m), and the two RE2 Options with no inline form are functions —
 * `escape` (QuoteMeta) and `replace_all`.  So a leaf never validates flags, and
 * `'regex` means exactly one thing: a pattern RE2 would not compile (or no
 * module).  Nesting deeper than one level, non-text elements and tables all fail
 * as `'type` in the leaf, without a guard: rejecting is reversible, accepting is
 * the one-way door. */
#include "qlang/q_registry_internal.h" /* q_rlike_wrap's decl — the manifest wrap */
#include "qlang/base/q_err.h"
#include "qlang/eval/q_eval.h"     /* q_eval_apply_truthy — THE truthiness home */
#include "qlang/io/q_re2.h"
#include "qlang/ops/q_regex.h"
#include "qlang/q_env.h"           /* q_env_bind — the .regexp bindings */
#include "qlang/q_prim.h"          /* q_str_subject_bytes/_many/_map */
#include "lang/env.h"              /* ray_fn_vary */
#include "lang/eval.h"             /* RAY_FN_NONE */
#include <rayforce.h>
#include <stdlib.h>
#include <string.h>

/* What every leaf is handed: the compiled pattern plus whatever that reading
 * needs.  `arg` is replace's replacement (borrowed) and nothing else's. */
typedef struct {
    const q_re2_prog* prog;
    ray_t*            arg;
    int               flag;   /* test: anchor.  replace: global. */
} regex_ctx;

/* pattern -> a cached compiled program.  NULL with *err set: 'type when the
 * pattern is not text, 'regex when RE2 will not compile it or no module is
 * installed. */
static const q_re2_prog* regex_prog(ray_t* pattern, ray_t** err) {
    const char* pp;
    int64_t     pn;
    *err = NULL;
    if (!q_str_subject_bytes(pattern, &pp, &pn)) {
        *err = q_err(QE_TYPE);
        return NULL;
    }
    const q_re2_prog* prog = q_re2_get(pp, pn);
    if (!prog) *err = q_err(QE_REGEX);
    return prog;
}

/* Every matched piece is a SLICE of the subject, so extraction is pure offset
 * arithmetic; only replace and escape make new text.  A capture group that did
 * not participate arrives as offset -1 and reads as "". */
static ray_t* regex_slice(const char* sp, int64_t off, int64_t len) {
    return ray_charv(off < 0 ? "" : sp + off, off < 0 ? 0 : len);
}

static ray_t* regex_slice_append(ray_t* out, const char* sp, int64_t off, int64_t len) {
    ray_t* e = regex_slice(sp, off, len);
    if (RAY_IS_ERR(e)) { ray_release(out); return e; }
    out = ray_list_append(out, e);
    ray_release(e);
    return out;
}

/* An empty match must still advance, and RE2 matches in UTF-8: a single-byte step
 * would restart it INSIDE a multi-byte character, so scanning moves by code
 * point.  This is the one place the byte-uniform string model has to defer to the
 * matcher's own view of the text. */
static int64_t regex_step(const char* sp, int64_t sn, int64_t pos) {
    for (pos++; pos < sn && ((unsigned char)sp[pos] & 0xC0) == 0x80; pos++) {}
    return pos;
}

/* ---- the leaves: one subject in, that verb's answer out ------------------- */

static ray_t* regex_test_one(ray_t* x, void* c) {
    const regex_ctx* k = c;
    const char*      sp;
    int64_t          sn, g[2];
    if (!q_str_subject_bytes(x, &sp, &sn)) return q_err(QE_TYPE);
    return ray_bool(q_re2_match(k->prog, sp, sn, 0, k->flag, g, 0) > 0);
}

static ray_t* regex_extract_one(ray_t* x, void* c) {
    const regex_ctx* k = c;
    const char*      sp;
    int64_t          sn, g[2];
    if (!q_str_subject_bytes(x, &sp, &sn)) return q_err(QE_TYPE);
    if (q_re2_match(k->prog, sp, sn, 0, 0, g, 1) <= 0) return ray_charv("", 0);
    return regex_slice(sp, g[0], g[1]);
}

/* The capture groups of one match, starting at slot `from` in a buffer sized for
 * the pattern.  Shared by groups (the first match) and groups_all (each match). */
static ray_t* regex_group_list(const q_re2_prog* prog, const char* sp, int64_t sn,
                               int64_t start, int64_t* next) {
    int      ng = q_re2_ngroups(prog) + 1;
    int64_t* g  = malloc((size_t)ng * 2 * sizeof *g);
    if (!g) return q_err(QE_WSFULL);
    int    got = q_re2_match(prog, sp, sn, start, 0, g, ng);
    ray_t* out = got > 0 ? ray_list_new(got > 1 ? got - 1 : 1) : NULL;
    for (int i = 1; i < got && out && !RAY_IS_ERR(out); i++)
        out = regex_slice_append(out, sp, g[2 * i], g[2 * i + 1]);
    if (next && got > 0)
        *next = g[1] ? g[0] + g[1] : regex_step(sp, sn, g[0]);
    free(g);
    return out;   /* NULL = no match; the caller decides what that means */
}

static ray_t* regex_groups_one(ray_t* x, void* c) {
    const regex_ctx* k = c;
    const char*      sp;
    int64_t          sn;
    if (!q_str_subject_bytes(x, &sp, &sn)) return q_err(QE_TYPE);
    ray_t* g = regex_group_list(k->prog, sp, sn, 0, NULL);
    return g ? g : ray_list_new(0);   /* no match -> () */
}

/* One group-list per match, so selecting a group across every match is ordinary
 * indexing: groups_all[...][;1] is group 2 everywhere.  This is what DuckDB
 * spells regexp_extract_all(subject, pattern, group). */
static ray_t* regex_groups_all_one(ray_t* x, void* c) {
    const regex_ctx* k = c;
    const char*      sp;
    int64_t          sn;
    if (!q_str_subject_bytes(x, &sp, &sn)) return q_err(QE_TYPE);
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    int64_t pos = 0;
    while (pos <= sn) {
        int64_t next = pos;
        ray_t*  g    = regex_group_list(k->prog, sp, sn, pos, &next);
        if (!g) break;
        if (RAY_IS_ERR(g)) { ray_release(out); return g; }
        out = ray_list_append(out, g);
        ray_release(g);
        if (RAY_IS_ERR(out)) return out;
        pos = next;
    }
    return out;
}

static ray_t* regex_extract_all_one(ray_t* x, void* c) {
    const regex_ctx* k = c;
    const char*      sp;
    int64_t          sn;
    if (!q_str_subject_bytes(x, &sp, &sn)) return q_err(QE_TYPE);
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    int64_t g[2], pos = 0;
    while (pos <= sn && !RAY_IS_ERR(out)) {
        if (q_re2_match(k->prog, sp, sn, pos, 0, g, 1) <= 0) break;
        out = regex_slice_append(out, sp, g[0], g[1]);
        pos = g[1] ? g[0] + g[1] : regex_step(sp, sn, g[0]);
    }
    return out;
}

/* Split scans the REMAINDER, not the whole subject — DuckDB's own algorithm
 * (string_split.cpp StringSplitter::Split), matched deliberately so the mapping
 * `.regexp.split` <-> regexp_split_to_array in the docs is true rather than
 * approximate.  Verified against the v1.4.5 library, which is also where the two
 * non-obvious rules come from: a zero-width match at the remainder's START is not
 * a delimiter at all (it yields one character and moves on), and once the
 * remainder is empty no further match is considered.  Passing the remainder as
 * its own text is what makes `^` behave as DuckDB's does. */
static ray_t* regex_split_one(ray_t* x, void* c) {
    const regex_ctx* k = c;
    const char*      sp;
    int64_t          sn;
    if (!q_str_subject_bytes(x, &sp, &sn)) return q_err(QE_TYPE);
    ray_t* out = ray_list_new(4);
    if (RAY_IS_ERR(out)) return out;
    int64_t base = 0;
    while (base < sn && !RAY_IS_ERR(out)) {
        const char* rp = sp + base;
        int64_t     rn = sn - base, g[2];
        if (q_re2_match(k->prog, rp, rn, 0, 0, g, 1) <= 0) break;
        int64_t pos = g[0], w = g[1];
        if (w == 0 && pos == 0) {
            pos = regex_step(rp, rn, 0);
            if (pos == rn) break;
        }
        out = regex_slice_append(out, rp, 0, pos);
        base += pos + w;
    }
    if (!RAY_IS_ERR(out)) out = regex_slice_append(out, sp, base, sn - base);
    return out;
}

static ray_t* regex_replace_one(ray_t* x, void* c) {
    const regex_ctx* k = c;
    const char*      sp;
    const char*      rp;
    int64_t          sn, rn;
    if (!q_str_subject_bytes(x, &sp, &sn) ||
        !q_str_subject_bytes(k->arg, &rp, &rn))
        return q_err(QE_TYPE);
    char*   buf;
    int64_t bn;
    if (q_re2_replace(k->prog, sp, sn, rp, rn, k->flag != 0, &buf, &bn) != 0)
        return q_err(QE_REGEX);
    ray_t* out = ray_charv(buf, bn);
    q_re2_freestr(buf);
    return out;
}

static ray_t* regex_escape_one(ray_t* x, void* c) {
    const char* sp;
    int64_t     sn;
    (void)c;
    if (!q_str_subject_bytes(x, &sp, &sn)) return q_err(QE_TYPE);
    char*   buf;
    int64_t bn;
    if (q_re2_escape(sp, sn, &buf, &bn) != 0) return q_err(QE_REGEX);
    ray_t* out = ray_charv(buf, bn);
    q_re2_freestr(buf);
    return out;
}

/* ---- the verb surface ----------------------------------------------------- */

/* rlike is `.regexp.matches`, spelled infix. */
ray_t* q_rlike_wrap(ray_t* x, ray_t* pattern) {
    ray_t*            err;
    const q_re2_prog* prog = regex_prog(pattern, &err);
    if (!prog) return err;
    regex_ctx c = { prog, NULL, 0 };
    return q_str_subject_map(x, regex_test_one, &c, RAY_BOOL);
}

/* Every native below is: compile the pattern once, then hand the subject WHOLE to
 * the shared map with this reading's leaf.  `empty_tag` is the answer's type for
 * a collection of no subjects — boolean for the predicates, a general list for
 * the rest, never a bare () where a type is knowable. */
static ray_t* regex_run(ray_t** args, ray_t* (*one)(ray_t*, void*), int8_t empty_tag,
                        ray_t* arg, int flag) {
    ray_t*            err;
    const q_re2_prog* prog = regex_prog(args[1], &err);
    if (!prog) return err;
    regex_ctx c = { prog, arg, flag };
    return q_str_subject_map(args[0], one, &c, empty_tag);
}

/* An internal flag argument still goes through THE truthiness home, so `1` reads
 * like `1b` and junk errors instead of quietly reading false. */
static int regex_flag(ray_t* x, ray_t** err) {
    ray_retain(x);                       /* truthy CONSUMES its argument */
    return q_eval_apply_truthy(x, err);
}

static ray_t* regex_test_fn(ray_t** args, int64_t n) {
    if (n != 3) return q_err(QE_RANK);
    ray_t* err;
    int    anchor = regex_flag(args[2], &err);
    if (err) return err;
    return regex_run(args, regex_test_one, RAY_BOOL, NULL, anchor);
}

static ray_t* regex_extract_fn(ray_t** args, int64_t n) {
    return n == 2 ? regex_run(args, regex_extract_one, 0, NULL, 0) : q_err(QE_RANK);
}

static ray_t* regex_groups_fn(ray_t** args, int64_t n) {
    return n == 2 ? regex_run(args, regex_groups_one, 0, NULL, 0) : q_err(QE_RANK);
}

static ray_t* regex_groups_all_fn(ray_t** args, int64_t n) {
    return n == 2 ? regex_run(args, regex_groups_all_one, 0, NULL, 0) : q_err(QE_RANK);
}

static ray_t* regex_extract_all_fn(ray_t** args, int64_t n) {
    return n == 2 ? regex_run(args, regex_extract_all_one, 0, NULL, 0) : q_err(QE_RANK);
}

static ray_t* regex_split_fn(ray_t** args, int64_t n) {
    return n == 2 ? regex_run(args, regex_split_one, 0, NULL, 0) : q_err(QE_RANK);
}

/* [subject; pattern; replacement; global] */
static ray_t* regex_replace_fn(ray_t** args, int64_t n) {
    if (n != 4) return q_err(QE_RANK);
    ray_t* err;
    int    global = regex_flag(args[3], &err);
    if (err) return err;
    return regex_run(args, regex_replace_one, 0, args[2], global);
}

/* escape takes no pattern, so it needs no program and cannot go through
 * regex_run — but it distributes over the same shared map. */
static ray_t* regex_escape_fn(ray_t** args, int64_t n) {
    if (n != 1) return q_err(QE_RANK);
    return q_str_subject_map(args[0], regex_escape_one, NULL, 0);
}

static void regex_bind(const char* name, ray_t* obj) {
    q_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

static void regex_bind_fn(const char* name, ray_vary_fn fn) {
    regex_bind(name, ray_fn_vary(name, RAY_FN_NONE, fn));
}

void q_regex_register(void) {
    regex_bind_fn(".regexp.i.test",        regex_test_fn);
    regex_bind_fn(".regexp.i.extract",     regex_extract_fn);
    regex_bind_fn(".regexp.i.groups",      regex_groups_fn);
    regex_bind_fn(".regexp.i.groups_all",  regex_groups_all_fn);
    regex_bind_fn(".regexp.i.extract_all", regex_extract_all_fn);
    regex_bind_fn(".regexp.i.split",       regex_split_fn);
    regex_bind_fn(".regexp.i.replace",     regex_replace_fn);
    regex_bind_fn(".regexp.i.escape",      regex_escape_fn);
    /* the pin as DATA, not a call: RE2 has no version string, so what we can
     * report is the DuckDB release the vendored tree came from */
    const char* pin = q_re2_pin();
    regex_bind(".regexp.version", ray_charv(pin, (int64_t)strlen(pin)));
}
