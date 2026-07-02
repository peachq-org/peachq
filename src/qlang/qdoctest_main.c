/* qdoctest — validate the q examples embedded in markdown docs (and .qcmd
 * transcripts) against the q engine.
 *
 *   qdoctest [--syntax-only] [--verbose] [--skip-file PATH]
 *            [--results PATH] FILE|DIR...
 *
 * --syntax-only: only check each example parses (default: parse + eval + match
 * the expected output, whitespace-insensitive).  One fresh runtime per file
 * (shared session per file).  One line per file, plus a totals summary.
 *
 * --skip-file PATH: load newline-separated path fragments; any corpus file whose
 * path contains a fragment (substring/suffix match) is SKIPPED and counted as a
 * skipped page — it is NOT run (used to fence off docs that crash the process,
 * e.g. pages that call `exit`).
 *
 * --results PATH: ledger mode.  Runs every non-skipped file in EVAL_MATCH and
 * writes STRICTLY ONE line per file with >0 failures: `<path>\tparse P/N  eval
 * E/N`, then a TOTAL summary line.  Paths are sorted for a stable diff and
 * fully-passing files are omitted, so `wc -l` (minus the TOTAL line) counts the
 * failing files. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/qdoc.h"
#include "qlang/q_runtime.h"
#include <rayforce.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void ray_runtime_destroy(ray_runtime_t* rt);

static qdoc_mode_t   g_mode    = QDOC_EVAL_MATCH;
static int           g_verbose = 0;
static qdoc_result_t g_tot     = {0};
static int           g_files   = 0;
static int           g_skipped = 0;   /* pages skipped via the skip-list */

/* ---- skip-list ---------------------------------------------------------- */
static char** g_skip    = NULL;
static int    g_skip_n  = 0;

static void skip_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "qdoctest: cannot open skip-file %s\n", path);
        return;
    }
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        size_t s = 0;
        while (line[s] == ' ' || line[s] == '\t') s++;   /* left-trim */
        if (line[s] == '\0' || line[s] == '#') continue; /* blank / comment */
        char* frag = strdup(line + s);
        if (!frag) continue;
        char** grown = realloc(g_skip, (size_t)(g_skip_n + 1) * sizeof *g_skip);
        if (!grown) { free(frag); break; }
        g_skip = grown;
        g_skip[g_skip_n++] = frag;
    }
    fclose(f);
}

static void skip_free(void) {
    for (int i = 0; i < g_skip_n; i++) free(g_skip[i]);
    free(g_skip);
    g_skip = NULL;
    g_skip_n = 0;
}

static int is_skipped(const char* path) {
    for (int i = 0; i < g_skip_n; i++)
        if (strstr(path, g_skip[i])) return 1;
    return 0;
}

/* ---- deterministic path collection (ledger mode) ------------------------ */
static char** g_paths   = NULL;
static int    g_paths_n = 0;
static int    g_paths_cap = 0;

static void paths_add(const char* path) {
    if (g_paths_n == g_paths_cap) {
        int cap = g_paths_cap ? g_paths_cap * 2 : 64;
        char** grown = realloc(g_paths, (size_t)cap * sizeof *g_paths);
        if (!grown) return;
        g_paths = grown;
        g_paths_cap = cap;
    }
    char* dup = strdup(path);
    if (!dup) return;
    g_paths[g_paths_n++] = dup;
}

static void paths_free(void) {
    for (int i = 0; i < g_paths_n; i++) free(g_paths[i]);
    free(g_paths);
    g_paths = NULL;
    g_paths_n = g_paths_cap = 0;
}

static int path_cmp(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* ---- helpers ------------------------------------------------------------ */
static int ends_with(const char* s, const char* suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

static int is_doc(const char* path) {
    return ends_with(path, ".md") || ends_with(path, ".qcmd");
}

/* Run one file against a fresh runtime; verbose goes to `out`. */
static qdoc_result_t run_file(const char* path, FILE* out) {
    ray_runtime_t* rt = q_runtime_create(0, NULL);   /* fresh per file */
    qdoc_result_t r = qdoc_run_file(path, g_mode, g_verbose || out != stdout,
                                    out);
    if (rt) ray_runtime_destroy(rt);
    return r;
}

/* Normal (non-ledger) per-file run: honour the skip-list, print one line. */
static void run_one(const char* path) {
    if (is_skipped(path)) { g_skipped++; return; }

    qdoc_result_t r = run_file(path, stdout);

    g_tot.examples += r.examples;
    g_tot.parsed   += r.parsed;
    g_tot.passed   += r.passed;
    g_tot.failed   += r.failed;
    g_files++;

    printf("%s %s: %d/%d %s\n", r.failed ? "FAIL" : "PASS", path,
           g_mode == QDOC_PARSE_ONLY ? r.parsed : r.passed, r.examples,
           g_mode == QDOC_PARSE_ONLY ? "parsed" : "ok");
}

static void walk(const char* path, void (*visit)(const char*)) {
    DIR* d = opendir(path);
    if (!d) {                                  /* not a dir: run if it's a doc */
        if (is_doc(path)) visit(path);
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;     /* skip . .. and hidden */
        char child[4096];
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        walk(child, visit);
    }
    closedir(d);
}

/* Ledger mode: collect + sort paths, run each capturing verbose per-file, write
 * only failing files (header + failing lines) plus a TOTAL summary. */
static int ledger(const char* results_path) {
    qsort(g_paths, (size_t)g_paths_n, sizeof *g_paths, path_cmp);

    FILE* rf = fopen(results_path, "w");
    if (!rf) {
        fprintf(stderr, "qdoctest: cannot open results file %s\n", results_path);
        return 2;
    }

    for (int i = 0; i < g_paths_n; i++) {
        const char* path = g_paths[i];
        if (is_skipped(path)) { g_skipped++; continue; }

        char*  buf = NULL;
        size_t bsz = 0;
        FILE*  mem = open_memstream(&buf, &bsz);
        qdoc_result_t r = { .examples = 0 };
        if (mem) {
            r = run_file(path, mem);
            fclose(mem);
        }

        g_tot.examples += r.examples;
        g_tot.parsed   += r.parsed;
        g_tot.passed   += r.passed;
        g_tot.failed   += r.failed;
        g_files++;

        if (r.failed > 0) {
            /* Strictly ONE line per failing file so `wc -l` counts failing
             * files directly (the score shows how many examples fail). Per-
             * example detail is available on demand via `qdoctest --verbose`. */
            fprintf(rf, "%s\tparse %d/%d  eval %d/%d\n",
                    path, r.parsed, r.examples, r.passed, r.examples);
        }
        free(buf);   /* verbose captured only to keep it off stdout; discarded */
    }

    fprintf(rf, "--- TOTAL %d files | parse %d/%d | eval %d/%d | %d skipped\n",
            g_files, g_tot.parsed, g_tot.examples,
            g_tot.passed, g_tot.examples, g_skipped);
    fclose(rf);
    return 0;
}

int main(int argc, char** argv) {
    const char* results_path = NULL;
    const char* targets[64];
    int         ntargets = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--syntax-only"))  g_mode = QDOC_PARSE_ONLY;
        else if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
        else if (!strcmp(argv[i], "--skip-file") && i + 1 < argc)
            skip_load(argv[++i]);
        else if (!strcmp(argv[i], "--results") && i + 1 < argc)
            results_path = argv[++i];
        else if (ntargets < (int)(sizeof targets / sizeof *targets))
            targets[ntargets++] = argv[i];
    }

    if (!ntargets) {
        fprintf(stderr, "usage: qdoctest [--syntax-only] [--verbose] "
                        "[--skip-file PATH] [--results PATH] FILE|DIR...\n");
        skip_free();
        return 2;
    }

    int rc = 0;
    if (results_path) {
        g_mode = QDOC_EVAL_MATCH;              /* ledger is always eval-match */
        for (int i = 0; i < ntargets; i++) walk(targets[i], paths_add);
        rc = ledger(results_path);
        printf("qdoctest: %d/%d parsed, %d/%d ok across %d file(s), %d skipped "
               "-> %s\n",
               g_tot.parsed, g_tot.examples, g_tot.passed, g_tot.examples,
               g_files, g_skipped, results_path);
        paths_free();
    } else {
        for (int i = 0; i < ntargets; i++) walk(targets[i], run_one);
        printf("---\nqdoctest: %d/%d examples %s across %d file(s), %d skipped\n",
               g_mode == QDOC_PARSE_ONLY ? g_tot.parsed : g_tot.passed,
               g_tot.examples,
               g_mode == QDOC_PARSE_ONLY ? "parsed" : "ok",
               g_files, g_skipped);
        rc = g_tot.failed ? 1 : 0;
    }

    skip_free();
    return rc;
}
