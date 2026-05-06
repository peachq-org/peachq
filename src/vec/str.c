/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "str.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * SSO vs long-string detection
 *
 * The slen/sdata and obj fields share the same 8-byte union in ray_t.
 * SSO: slen is 0..7, sdata contains the string bytes.
 * Long: obj is a non-NULL pointer to a U8 vector.
 *
 * Distinction:
 *   - slen 1..7 → always SSO (a 32B-aligned pointer's low byte is a
 *     multiple of 32, never 1..7)
 *   - slen 0 with obj == NULL → empty SSO (all 8 union bytes are zero)
 *   - slen 0 with obj != NULL → long string (pointer's low byte is 0)
 *   - slen > 7 → long string (pointer's low byte is 32, 64, ... or higher)
 * -------------------------------------------------------------------------- */

static bool is_sso(ray_t* s) {
    if (s->slen >= 1 && s->slen <= 7) return true;
    if (s->slen == 0 && s->obj == NULL) return true;
    return false;
}

/* --------------------------------------------------------------------------
 * ray_str_ptr
 * -------------------------------------------------------------------------- */

const char* ray_str_ptr(ray_t* s) {
    if (!s || RAY_IS_ERR(s)) return NULL;
    if (is_sso(s)) return (const char*)s->sdata;
    return (const char*)ray_data(s->obj);
}

/* --------------------------------------------------------------------------
 * ray_str_len
 * -------------------------------------------------------------------------- */

size_t ray_str_len(ray_t* s) {
    if (!s || RAY_IS_ERR(s)) return 0;
    if (is_sso(s)) return (size_t)s->slen;
    return (size_t)s->obj->len;
}

/* --------------------------------------------------------------------------
 * ray_str_cmp -- Compare two string atoms.
 *
 * Compare by memcmp of the min length, then by length difference.
 * -------------------------------------------------------------------------- */

int ray_str_cmp(ray_t* a, ray_t* b) {
    if (!a || RAY_IS_ERR(a) || !b || RAY_IS_ERR(b)) return 0;

    const char* ap = ray_str_ptr(a);
    const char* bp = ray_str_ptr(b);
    size_t alen = ray_str_len(a);
    size_t blen = ray_str_len(b);

    /* Fast first-char rejection.  Sort comparators on SYM/STR columns
     * (ORDER BY name, top-K key compare) hit this hot.  When the two
     * strings differ on byte 0 — the common case for varied user
     * inputs like SearchPhrase — we skip the memcmp call entirely.
     * Per-byte loops in memcmp dominate the cost only when long
     * strings share a prefix; first-byte short-circuit cuts sort
     * compare cost on first-char-diverse data without slowing down
     * the prefix-matching case (one extra load + branch). */
    if (alen > 0 && blen > 0) {
        int d = (int)(unsigned char)ap[0] - (int)(unsigned char)bp[0];
        if (d != 0) return d;
    }

    size_t minlen = alen < blen ? alen : blen;
    int cmp = 0;
    if (minlen > 1) cmp = memcmp(ap + 1, bp + 1, minlen - 1);
    if (cmp != 0) return cmp;

    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}
