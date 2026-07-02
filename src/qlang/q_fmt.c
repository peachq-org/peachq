/* q_fmt — see q_fmt.h.  The start of a q-correct value formatter (the outputter
 * that will eventually back q's `.Q.s`).  For now it renders simple int vectors
 * q-style and delegates everything else to rayforce's ray_fmt. */
#include "qlang/q_fmt.h"
#include "lang/format.h"   /* ray_fmt */
#include <string.h>
#include <stdio.h>

/* rayforce's formatter, captured as a string — the fallback for values q_fmt
 * doesn't yet render q-style. */
static void ray_fallback(ray_t* val, char* buf, size_t bufsz) {
    ray_t* s = ray_fmt(val, 0);
    if (s && !RAY_IS_ERR(s) && s->type == -RAY_STR) {
        size_t n = ray_str_len(s);
        if (n >= bufsz) n = bufsz - 1;
        memcpy(buf, ray_str_ptr(s), n);
        buf[n] = '\0';
    }
    if (s && !RAY_IS_ERR(s)) ray_release(s);
}

void q_fmt(ray_t* val, char* buf, size_t bufsz) {
    if (bufsz == 0) return;
    buf[0] = '\0';
    if (!val) return;

    /* q prints a simple vector space-separated with NO brackets: `5 6 7`,
     * where rayforce prints `[5 6 7]`.  Handle int vectors here (the common
     * arithmetic / til / reverse result); everything else falls back to the
     * rayforce formatter for now.
     *
     * Note: a 1-element vector is left as e.g. `5` rather than q's enlist form
     * `,5` for the moment — refined when the full q outputter lands. */
    if (val->type == RAY_I64) {
        int64_t n = ray_len(val);
        const int64_t* d = (const int64_t*)ray_data(val);
        size_t pos = 0;
        for (int64_t i = 0; i < n; i++) {
            char e[24];
            int m = snprintf(e, sizeof e, "%s%lld", i ? " " : "", (long long)d[i]);
            if (m < 0) break;
            if (pos + (size_t)m + 1 > bufsz) break;   /* keep room for the NUL */
            memcpy(buf + pos, e, (size_t)m);
            pos += (size_t)m;
        }
        buf[pos] = '\0';
        return;
    }

    ray_fallback(val, buf, bufsz);
}
