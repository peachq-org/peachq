/* q_classic — `\classic`, the one kdb switch.  On, peachq is kdb: its display and every error.  Off, the modern
 * display and the friendly syntax (qSQL trailing commas, all-atom tables) are on.  A bare runtime is classic;
 * front ends (`./q` without -classic, wasm) turn it off.  The console, the parser (at parse time) and flip read it. */
#ifndef QLANG_Q_CLASSIC_H
#define QLANG_Q_CLASSIC_H

#include <stdbool.h>

bool q_classic_on(void);
void q_classic_set(bool on);

#endif
