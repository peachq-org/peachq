/* q_fmt — see q_fmt.h.  Currently delegates to rayforce's ray_fmt; this is the
 * designated seam for q-correct rendering later. */
#include "qlang/q_fmt.h"
#include "lang/format.h"   /* ray_fmt */
#include <string.h>

void q_fmt(ray_t* val, char* buf, size_t bufsz) {
    if (bufsz == 0) return;
    buf[0] = '\0';
    if (!val) return;

    /* mode 0 = single-line form.  TODO: replace with q-correct rendering. */
    ray_t* s = ray_fmt(val, 0);
    if (s && !RAY_IS_ERR(s) && s->type == -RAY_STR) {
        size_t n = ray_str_len(s);
        if (n >= bufsz) n = bufsz - 1;
        memcpy(buf, ray_str_ptr(s), n);
        buf[n] = '\0';
    }
    if (s && !RAY_IS_ERR(s)) ray_release(s);
}
