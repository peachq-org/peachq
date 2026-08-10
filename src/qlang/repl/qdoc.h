/* qdoc — run the q examples embedded in a doc file against the q engine.
 *
 * Extraction is deliberately dumb: the only special token is `q)`.  In a .md
 * file, examples are taken from ```q fenced blocks; each line beginning with
 * `q)` is an input (the text after `q)`, forwarded VERBATIM — comments and
 * \-commands included), and the following non-`q)` lines are its expected
 * output.  A .qcmd file (no fences) is treated as one implicit block.
 *
 * Every row RUNS THROUGH THE PRODUCT — the statement seam the REPL, `\l` and
 * `-f` share — and the gate compares the bytes the REPL would have printed.
 * This runner owns only POLICY: the transcript-prompt pin, the parse pillar,
 * row classification, whitespace normalization and the --emit mirror. */
#ifndef QDOC_H
#define QDOC_H

#include <stdio.h>

typedef enum { QDOC_PARSE_ONLY, QDOC_EVAL_MATCH } qdoc_mode_t;

typedef struct {
    int examples;   /* q) inputs found */
    int parsed;     /* examples that PARSED (regardless of eval outcome) */
    int passed;     /* matched */
    int failed;     /* mismatched */
    int skipped;    /* reserved; 0 for now */
} qdoc_result_t;

/* Run every example in `path` against the CURRENT rayforce runtime (the caller
 * owns it — one fresh runtime per file gives the "shared session per file"
 * default).  With verbose != 0, each failing example is written to `out`.
 * With emit_dir set, also write every MISMATCHING example's ACTUAL output to
 * <emit_dir>/<path> as a .qcmd-shaped transcript (errors as `'type`).  A file
 * that matches everywhere writes nothing, so the emit tree holds exactly the
 * corpus's current wrong answers — diff two of them to see what a change moved.
 * Mismatches only: a diff register, NOT a runnable transcript.  Paths mirror
 * the corpus root (test/q/list/take.qcmd -> <dir>/list/take.qcmd); a short list
 * of clock/counter-valued suites is skipped so the tree stays diff-stable.
 * emit_dir NULL/empty runs the file without emitting anything. */
qdoc_result_t q_qdoc_run_file_emit(const char* path, qdoc_mode_t mode,
                                 int verbose, FILE* out, const char* emit_dir);

/* A FILE* whose bytes land in memory — how this runner captures what the
 * statement seam printed, since the seam speaks FILE* and nothing else.
 * `open_memstream` where the libc has it; mingw has neither it nor fmemopen
 * nor fopencookie, so there the stream is a temp file read back at close.
 * *buf is owned by the caller and readable only AFTER qdoc_memclose, which is
 * the one legal close (a bare fclose strands the temp file on Windows). */
FILE* qdoc_memopen(char** buf, size_t* len);
void  qdoc_memclose(FILE* f, char** buf, size_t* len);

#endif /* QDOC_H */
