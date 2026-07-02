#ifndef Q_RUNTIME_H
#define Q_RUNTIME_H
#include <rayforce.h>
/* Create a rayforce runtime with q's builtins registered.  Use everywhere a q
 * environment is needed (binary, REPL, doctest, tests) so `parse` etc. resolve. */
ray_runtime_t* q_runtime_create(int argc, char** argv);
#endif /* Q_RUNTIME_H */
