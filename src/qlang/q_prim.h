/* q_prim — the q VALUE primitives: single-home helpers whose bodies live in
 * ops/ but whose callers are everywhere (eval, net, io, fmt, the tests).
 *
 * They were declared in q_registry.h, which made every caller of a pure value
 * helper look like a consumer of the registry contract; the real reason was
 * reach — ops/q_registry_internal.h is poisoned (string-C3: bare RAY_U8), so
 * non-ops callers cannot include it.  This header is that reach, named for what
 * it carries.  Declarations only: nothing here is defined outside ops/. */
#ifndef Q_PRIM_H
#define Q_PRIM_H

#include <rayforce.h>
#include <stdbool.h>
#include <stdint.h>

/* Collapse a boxed RAY_LIST of homogeneous scalar atoms into the matching
 * typed vector (kdb semantics: map/each/scan results and paren-lists of atoms
 * are simple vectors, not general lists).  Mixed types, non-atom elements,
 * string atoms (a list of strings IS kdb 0h) and non-lists are returned
 * unchanged.  Borrows `l`; always returns an OWNED value (a fresh vector, or
 * `l` retained).  DEFINED in ops/q_list.c; declared here so env-using callers
 * (net/, apply) reach it without the poisoned q_registry_internal.h. */
ray_t* q_list_collapse(ray_t* l);

/* THE per-column table walk (colfn owns any lazy-materialize).  DEFINED in
 * ops/q_table.c; declared here (like q_list_collapse) so env-using callers
 * (apply, dollar) reach it without the poisoned q_registry_internal.h. */
ray_t* q_table_map_cols(ray_t* (*colfn)(void* ctx, ray_t* col), void* ctx, ray_t* t);

/* An EMPTY boxed result inherits `proto`'s element type, so a selection that
 * takes nothing keeps its domain.  DEFINED in ops/q_list.c; declared here (like
 * q_list_collapse) for the env-using apply module. */
ray_t* q_typed_empty_like(ray_t* collapsed, ray_t* proto);

/* `~` as a C predicate — THE match home, so Converge's stop test inherits the
 * one comparison tolerance.  DEFINED in ops/q_math.c; declared here (like
 * q_list_collapse) for the env-using apply module. */
int q_match_rec(ray_t* a, ray_t* b);

/* ---- string-C3 boundary conversion (spec Design §3: physical RAY_STR never
 * appears in q-space; values in flight are charv; columns stay pooled).
 * DEFINED in ops/q_str.c; declared here (env-safe public reach). ---- */

/* THE single-home STR->charv constructor.  MATERIALIZES (one O(len) memcpy);
 * borrows s, returns an owned fresh charv vector (always a vector). */
ray_t* q_str_charv_of_str(ray_t* s);

/* charv vector or char atom -> owned -RAY_STR atom (for engine internals that
 * need pooled/NUL-terminated physical form).  Borrows x. */
ray_t* q_str_of_charv(ray_t* x);

/* Text-bytes accessor over all q text forms: -RAY_STR atom, charv vector,
 * char atom.  Borrowed pointer valid while x lives; false = not text. */
bool q_str_text_bytes(ray_t* x, const char** p, int64_t* n);

/* Boundary-out walk: CONSUMES r, returns owned.  -RAY_STR atom -> charv;
 * RAY_STR vector -> 0h list of charv; LIST/DICT values converted (in place
 * only at rc==1); TABLE (incl. keyed-table value side) passes untouched. */
ray_t* q_str_charv_out(ray_t* r);

/* Inverse adapter for legacy string-verb bodies: BORROWS x, returns OWNED
 * legacy form (charv/char atom -> -RAY_STR atom, LIST recursed). */
ray_t* q_str_in(ray_t* x);

/* Column attribute as kdb's single letter: 's'/'u'/'g'/'p', or 0 for none.
 * Reads the block markers/kind DIRECTLY (the kdb u#/p# policy is composed in the
 * q layer, not the rayfall-native engine `.attr.get`, so a hash-backed u#/p#
 * must be labelled by its marker, not the RAY_IDX_HASH kind — see q_registry.c).
 * Shared by the `attr` verb wrapper AND q_fmt's `` `s#``/`u#``/`g#``/`p# ``
 * display prefix so both agree on one mapping.  Borrows v. */
char q_attr_letter(ray_t* v);

/* Set a column attribute via the q `#` surface (`s`/`u`/`g`/`p`, or 0 to clear).
 * The kdb u#/p# accelerator (find-hash + marker) is COMPOSED in the q layer here,
 * not baked into the frozen engine.  Borrows vec; returns an owned attributed
 * column (or a remapped q error).  Exposed for the attribute-acceleration
 * C-unit (test/q_attr_accel.c) to exercise the real q set-attribute path. */
ray_t* q_attr_set_letter(char letter, ray_t* vec);

/* q `ssr[s;p;r]` — string search-and-replace (feat/q-string-fns).  Exposed so
 * q_builtins_register can env-bind it (a triadic prefix keyword: the parser
 * name-refs `ssr[a;b;c]`, so it resolves through the env, not the registry). */
ray_t* q_ssr_wrap(ray_t** args, int64_t n);

/* q join family bracket forms (feat/q-joins-rebuild) — triadic/quaternary
 * prefix keywords env-bound by q_builtins_register (the ssr precedent):
 * ej[c;t1;t2], aj[c;t1;t2] + variants, wj/wj1[w;f;t;(q;aggs..)]. */
ray_t* q_ej_wrap(ray_t** args, int64_t n);
ray_t* q_aj_wrap(ray_t** args, int64_t n);
ray_t* q_aj0_wrap(ray_t** args, int64_t n);
ray_t* q_ajf_wrap(ray_t** args, int64_t n);
ray_t* q_ajf0_wrap(ray_t** args, int64_t n);
ray_t* q_wj_wrap(ray_t** args, int64_t n);
ray_t* q_wj1_wrap(ray_t** args, int64_t n);

/* Universal table row indexing (uniform-structure-dispatch stage 0; defined
 * in ops/q_table.c).  q_table_at: t[idx] for an integer atom (-> the row
 * dict) or an integer vector (-> row gather, misses null-filled per the
 * basics/application.md out-of-bounds law); returns NULL to DECLINE any
 * other index shape.  q_table_row_at: the row-dict arm — row < 0 (incl.
 * int nulls) or >= count t yields the typed all-null row. */
ray_t* q_table_at(ray_t* t, ray_t* idx);
ray_t* q_table_row_at(ray_t* t, int64_t row);

/* `cols x` / `meta x` (ops/q_table.c) — env-bound by q_builtins_register, and
 * q_cols_fn is qSQL's column-name home too. */
ray_t* q_cols_fn(ray_t* x);
ray_t* q_meta_fn(ray_t* x);

/* q `read0 x` (feat/q-file-text) — exposed so q_builtins can ALSO env-bind it
 * for the bracket-call form `read0[(f;o)]` (the ssr/value precedent: two fn
 * objects, one implementation). */
ray_t* q_read0_wrap(ray_t* x);

/* q `hopen`/`hclose` wrappers (q_handles.c): IPC sockets plus the `:path` /
 * `:fifo://path` filesystem transports (feat/q-ipc-client; hsym Bundle 2b).
 * Exposed so the apply module's one-shot sync request (`` `:host:port "query" ``) can
 * REUSE the exact hopen normalization + restricted gate + fd/selector handle
 * translation rather than duplicating them.  q_hopen_wrap: descriptor/port ->
 * owned int fd handle (or error, incl. clean 'nyi for deferred transports).
 * q_hclose_wrap: fd handle -> closed (no-op on a dead/console handle). */
ray_t* q_hopen_wrap(ray_t* x);
ray_t* q_hclose_wrap(ray_t* x);

/* q `hsym x` / `attr x` verb implementations — exported so the q_bang.c
 * internal-fn aliases (`-1!` -> hsym, `-2!` -> attr) route to the SAME single
 * home the registry verb uses (Direction B, bang-ops-internal-status.md). */
ray_t* q_hsym_wrap(ray_t* x);
ray_t* q_attr_wrap(ray_t* x);

/* File symbol -> OWNED RAY_STR filesystem path (leading ':' stripped), NULL
 * when x is not a `:path symbol atom.  The single home every file-touching
 * arm shares (q_io.c's read verbs, q_io_filetext's `0:`, q_wirefile). */
ray_t* q_io_file_path(ray_t* x);

/* `read1 (f;off;want)`'s body, shared with q_wirefile the way q_io_file_path
 * is: OWNED RAY_BYTE_ONLY of up to `want` bytes from `off` (want < 0 = to EOF;
 * both clamped, so a short read is not an error). */
ray_t* q_io_read_slice(ray_t* pathstr, int64_t off, int64_t want);

/* q `enlist` vary wrapper (base ray_enlist_fn + dict -> 1-row table arm) —
 * env-bound by q_builtins_register before registry init so both `enlist`
 * and monadic `,` share it. */
ray_t* q_enlist_wrap(ray_t** args, int64_t n);

#endif /* Q_PRIM_H */
