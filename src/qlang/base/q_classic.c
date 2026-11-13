#include "qlang/q_count.h"
#include "qlang/base/q_classic.h"

static bool g_classic = true;

bool q_classic_on(void)      { return g_classic; }
void q_classic_set(bool on)  { g_classic = on; }
