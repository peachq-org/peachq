/* peachq: compiles upstream MurmurHash3.cpp as C17 unchanged; redeclaring its two plain-`inline` helpers makes
 * this TU emit their external definitions, which an -O0 build otherwise fails to link (C17 6.7.4p7). */
#include "MurmurHash3.cpp"
uint32_t rotl32(uint32_t x, int8_t r);
uint64_t rotl64(uint64_t x, int8_t r);
