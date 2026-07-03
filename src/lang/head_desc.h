/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/* ── Call-head descriptor (ADR 0002 Option A) ──────────────────────────────
 *
 * openq commits to one object model: a q parse tree is a `ray_t` whose call
 * heads are function VALUES (`parse "2+3"` → `(+<fn>; 2; 3)`).  rayfall's
 * compiler and query/DAG planner historically key on name-reference SYMBOL
 * heads (`-RAY_SYM`) / sym-ids, so a value head would miss special-form
 * detection and `resolve_*_dag`, bypassing the optimizer.
 *
 * This header is the SINGLE place that recognizes a function-value head and
 * yields the two things the sym-keyed sites need: the head's interned NAME
 * (so name/sym-id-keyed resolvers work unchanged) and the value itself (so
 * the compiler can emit it as a constant / inspect its attrs).  Every
 * compiler / query-planner site that matches a symbol head consults this
 * helper for the value-head arm — defined once, not duplicated.
 *
 * Symbol-head handling stays at each call site: the compiler wants an
 * unquoted name-ref (`-RAY_SYM && !ATTR_QUOTED`) while the query planner
 * accepts any `-RAY_SYM`.  This helper deliberately covers ONLY the
 * function-value arm so neither site's symbol semantics shift.
 *
 * A function object stores its DAG opcode in `aux[0..1]` and its name in
 * `aux[2..15]` (`ray_fn_name`, `RAY_FN_OPCODE`), so a value head is not
 * opaque — its canonical name is read directly and interned. */

#ifndef RAY_LANG_HEAD_DESC_H
#define RAY_LANG_HEAD_DESC_H

#include <rayforce.h>       /* ray_t, ray_sym_intern, type tags */
#include "lang/eval.h"      /* RAY_UNARY / RAY_BINARY / RAY_VARY */
#include "lang/env.h"       /* ray_fn_name */
#include <string.h>
#include <stdbool.h>

/* ── openq: q-wrapper lowering marker (fn-attr flag) ───────────────────────
 * Set on a q-registry-blessed wrapper function value (`= <> # _`, …) that is
 * NOT the env binding of the name it lowers as.  A wrapper's aux-name holds
 * the CANONICAL rayfall verb it lowers to (`=`→"==", `<>`→"!=", `#`→"take",
 * `_`→"drop"); this flag authorises `ray_head_is_fn_value` to name-route it
 * (so the query DAG maps `=`→`ray_eq` etc.) even though it is not
 * env-identical, WITHOUT weakening the look-alike guard for an unflagged
 * custom fn that merely shares a builtin's name.
 *
 * INVARIANT: `RAY_FN_Q_LOWER` may only be set on a value whose aux-name is a
 * NON-INTRINSIC, non-special-form rayfall name (not set/let/if/do/fn/self/
 * try/return/eval/resolve) — otherwise `head_named` name-routing in
 * `compile_list` would mis-lower the wrapper as that intrinsic.  The four 2a
 * wrapper targets (== != take drop) all satisfy this.
 *
 * Bit 0x40 is the sole unused fn-attr bit (0x01/02/04/08/10/20/80 taken).
 * Defined here (openq-authored, non-frozen) rather than in eval.h so no
 * frozen base file is touched. */
#define RAY_FN_Q_LOWER 0x40

/* True iff `head` is a function VALUE usable directly as a call head
 * (RAY_UNARY / RAY_BINARY / RAY_VARY).  On a hit, fills `*out_fn` (may be
 * NULL) with the value itself (a BORROWED alias of `head` — no retain) and
 * returns true.
 *
 * `*out_sym` (may be NULL) receives a NAME id ONLY for semantic routing —
 * the compiler's inline special forms and the query planner's `resolve_*_dag`
 * key on this name.  It is set to the value's interned aux-name id *only when
 * the value IS the canonical env binding of that name* (`ray_env_get == head`);
 * otherwise it is -1.  This is the guard against a custom / wrapper function
 * object (built with `ray_fn_*`) that merely SHARES a builtin's display name:
 * such a value must run its own implementation via the generic call / eval
 * path, never be lowered as the like-named builtin (e.g. a custom fn named
 * "return" must not emit OP_RET; a custom fn named "+" must not become the
 * DAG add op).  A non-canonical value still returns true (it IS callable) so
 * the compiler can emit a specialized *direct* call of the value — that call
 * invokes the value's real code and is name-agnostic, hence always safe.
 *
 * Returns false for a symbol head, quoted literal, list, atom, or a bare
 * lambda value (lambda heads carry no canonical aux name and are handled by
 * the generic apply path, not this fast head-check). */
static inline bool ray_head_is_fn_value(ray_t* head, int64_t* out_sym,
                                        ray_t** out_fn) {
    if (!head) return false;
    if (head->type == RAY_UNARY || head->type == RAY_BINARY ||
        head->type == RAY_VARY) {
        if (out_sym) {
            const char* nm  = ray_fn_name(head);
            int64_t     sym = ray_sym_intern(nm, strlen(nm));
            if (head->attrs & RAY_FN_Q_LOWER) {
                /* openq-blessed q wrapper: route by its canonical aux-name
                 * (== != take drop) so it hits the same DAG op / decline as
                 * the like-named builtin.  Its own impl still runs when the
                 * value is CALLED (compile.c uses head_fn; eval uses the fn
                 * pointer), so string `=` / arg-swap `#` semantics survive. */
                *out_sym = sym;
            } else {
                /* Canonical-identity guard: only name-route builtin objects. */
                *out_sym = (ray_env_get(sym) == head) ? sym : -1;
            }
        }
        if (out_fn) *out_fn = head;
        return true;
    }
    return false;
}

#endif /* RAY_LANG_HEAD_DESC_H */
