/* q_utf8 — a Unicode code point as its UTF-8 bytes. */
#ifndef QLANG_Q_UTF8_H
#define QLANG_Q_UTF8_H

#include <stdint.h>

/* cp's encoding into b[0..3]; answers the byte count.  cp must be a scalar value: the caller maps a surrogate or
 * anything past U+10FFFF to U+FFFD first. */
int q_utf8_enc(uint32_t cp, char* b);

#endif
