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

/* True iff `head` is a function VALUE usable directly as a call head
 * (RAY_UNARY / RAY_BINARY / RAY_VARY).  On a hit, fills `*out_sym` (may be
 * NULL) with the value's interned aux-name id and `*out_fn` (may be NULL)
 * with the value itself (a BORROWED alias of `head` — no retain), and
 * returns true.  Returns false for a symbol head, quoted literal, list,
 * atom, or a bare lambda value (lambda heads carry no canonical aux name
 * and are handled by the generic apply path, not this fast head-check). */
static inline bool ray_head_is_fn_value(ray_t* head, int64_t* out_sym,
                                        ray_t** out_fn) {
    if (!head) return false;
    if (head->type == RAY_UNARY || head->type == RAY_BINARY ||
        head->type == RAY_VARY) {
        if (out_sym) {
            const char* nm = ray_fn_name(head);
            *out_sym = ray_sym_intern(nm, strlen(nm));
        }
        if (out_fn) *out_fn = head;
        return true;
    }
    return false;
}

#endif /* RAY_LANG_HEAD_DESC_H */
