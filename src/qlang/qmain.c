/* q — a minimal q REPL: prompt with `q)`, parse each line to a rayforce ray_t
 * via q_parse, evaluate through the unmodified rayforce engine, print the
 * q-formatted result.  The loop itself lives in q_repl.c so the qcmd tests can
 * drive the identical console behaviour in-process.
 *
 * Deliberately thin: no error traces, progress bar, remote session, or piped
 * bracket accumulation — those live in the (frozen) rayforce repl.c and are
 * reused later.  See ARCHITECTURE.md and the MVP design doc. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/q_repl.h"
#include <rayforce.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) {
    ray_runtime_t* rt = ray_runtime_create(argc, argv);
    if (!rt) { fprintf(stderr, "runtime init failed\n"); return 1; }

    /* Echo input when stdin is not a terminal (piped / redirected) so the
     * output is a faithful console transcript; on a real tty the terminal
     * echoes, so we don't. */
    int echo = !isatty(STDIN_FILENO);
    q_repl_run(stdin, stdout, stderr, echo);

    ray_runtime_destroy(rt);
    return 0;
}
