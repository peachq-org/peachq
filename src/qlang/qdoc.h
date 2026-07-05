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
    int passed;     /* matched, not annotated xfail (green) */
    int failed;     /* mismatched, not annotated xfail — a real regression (red) */
    int skipped;    /* reserved; 0 for now */
    int deferred;   /* mismatched but annotated `;; xfail`/`;; STATUS: deferred` (amber) */
    int ready;      /* annotated xfail that now MATCHES — un-defer me (green + badge) */
} qdoc_result_t;

/* The .qcmd state model (scoreboard): an example is annotated expected-fail by a
 * `;; xfail: <reason>` line placed after its `q)` input, or by a file-level
 * `;; STATUS: deferred <reason>` header marking every example.  A `;;` line is
 * NEVER expected output — it is a directive/comment.  A file with zero q)
 * examples is "pending" (the caller reports SKIP).  Gate rule: only `failed`
 * (real regression) breaks the build; deferred/ready/pending are gate-neutral. */

/* Run every example in `path` against the CURRENT rayforce runtime (the caller
 * owns it — one fresh runtime per file gives the "shared session per file"
 * default).  With verbose != 0, each failing example is written to `out`. */
qdoc_result_t qdoc_run_file(const char* path, qdoc_mode_t mode,
                            int verbose, FILE* out);

#endif /* QDOC_H */
