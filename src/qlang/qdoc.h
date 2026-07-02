/* qdoc — run the q examples embedded in a doc file against the q engine.
 *
 * Extraction is deliberately dumb: the only special token is `q)`.  In a .md
 * file, examples are taken from ```q fenced blocks; each line beginning with
 * `q)` is an input (the text after `q)`, forwarded VERBATIM — comments and
 * \-commands included), and the following non-`q)` lines are its expected
 * output.  A .qcmd file (no fences) is treated as one implicit block. */
#ifndef QDOC_H
#define QDOC_H

#include <stdio.h>

typedef enum { QDOC_PARSE_ONLY, QDOC_EVAL_MATCH } qdoc_mode_t;

typedef struct {
    int examples;   /* q) inputs found */
    int parsed;     /* examples that PARSED (regardless of eval outcome) */
    int passed;
    int failed;
    int skipped;    /* reserved; 0 for now */
} qdoc_result_t;

/* Run every example in `path` against the CURRENT rayforce runtime (the caller
 * owns it — one fresh runtime per file gives the "shared session per file"
 * default).  With verbose != 0, each failing example is written to `out`. */
qdoc_result_t qdoc_run_file(const char* path, qdoc_mode_t mode,
                            int verbose, FILE* out);

#endif /* QDOC_H */
