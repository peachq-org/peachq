#include "qlang/q_count.h"
#include "qlang/base/q_utf8.h"

int q_utf8_enc(uint32_t cp, char* b) {
    if (cp < 0x80) { b[0] = (char)cp; return 1; }
    int n = cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    for (int i = n - 1; i > 0; i--, cp >>= 6) b[i] = (char)(0x80 | (cp & 63));
    b[0] = (char)((0xF00 >> n) | cp);               /* the low byte of 0xF00 >> n is the n-byte lead mark */
    return n;
}
