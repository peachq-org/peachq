/* qdoctest — validate the q examples embedded in markdown docs (and .qcmd
 * transcripts) against the q engine.
 *
 *   qdoctest [--syntax-only] [--verbose] FILE|DIR...
 *
 * --syntax-only: only check each example parses (default: parse + eval + match
 * the expected output, whitespace-insensitive).  One fresh runtime per file
 * (shared session per file).  One line per file, plus a totals summary. */
#define _POSIX_C_SOURCE 200809L

#include "qlang/qdoc.h"
#include "qlang/q_runtime.h"
#include <rayforce.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

extern void ray_runtime_destroy(ray_runtime_t* rt);

static qdoc_mode_t   g_mode    = QDOC_EVAL_MATCH;
static int           g_verbose = 0;
static qdoc_result_t g_tot     = {0, 0, 0, 0};
static int           g_files   = 0;

static int ends_with(const char* s, const char* suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

static void run_one(const char* path) {
    ray_runtime_t* rt = q_runtime_create(0, NULL);   /* fresh per file */
    qdoc_result_t r = qdoc_run_file(path, g_mode, g_verbose, stdout);
    if (rt) ray_runtime_destroy(rt);

    g_tot.examples += r.examples;
    g_tot.passed   += r.passed;
    g_tot.failed   += r.failed;
    g_files++;

    printf("%s %s: %d/%d %s\n", r.failed ? "FAIL" : "PASS", path,
           r.passed, r.examples,
           g_mode == QDOC_PARSE_ONLY ? "parsed" : "ok");
}

static void walk(const char* path) {
    DIR* d = opendir(path);
    if (!d) {                                  /* not a dir: run if it's a doc */
        if (ends_with(path, ".md") || ends_with(path, ".qcmd")) run_one(path);
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;     /* skip . .. and hidden */
        char child[4096];
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        walk(child);
    }
    closedir(d);
}

int main(int argc, char** argv) {
    int targets = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--syntax-only"))      g_mode = QDOC_PARSE_ONLY;
        else if (!strcmp(argv[i], "--verbose"))     g_verbose = 1;
        else { walk(argv[i]); targets++; }
    }
    if (!targets) {
        fprintf(stderr, "usage: qdoctest [--syntax-only] [--verbose] FILE|DIR...\n");
        return 2;
    }

    printf("---\nqdoctest: %d/%d examples %s across %d file(s)\n",
           g_tot.passed, g_tot.examples,
           g_mode == QDOC_PARSE_ONLY ? "parsed" : "ok", g_files);
    return g_tot.failed ? 1 : 0;
}
