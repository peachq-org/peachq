/* q_wasm_shell — the `system` shell miss in a browser tab: a mini-shell over the in-memory
 * FS (pwd ls cat echo mkdir rm head tail) answering what /bin/sh would print, so q_sys.c's
 * capture splits it into lines as it splits popen's.  Any other command, and any failure,
 * is a nonzero exit: NULL, which q_sys.c answers with 'os.  Words split on blanks; no
 * quoting, pipes, globs or redirection.
 *
 * `ls` reads directory entries only and never stats them: a lazily mounted file must
 * not be fetched because it was listed. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/ops/q_sys.h"
#include "store/fileio.h"          /* ray_mkdir_p */
#include "q_wasm_buf.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SH_MAX_ARGS 64

static void put_line(wbuf_t* o, const char* s) { wbuf_str(o, s); wbuf_put(o, "\n", 1); }

static int cmp_str(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}

/* The entries of dir, sorted, hidden names skipped (ls without -a).  -1 = not a directory. */
static int list_dir(wbuf_t* o, const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return -1;
    char** names = NULL;
    size_t n = 0, cap = 0;
    struct dirent* e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char** nn = realloc(names, cap * sizeof *names);
            if (!nn) { rc = -1; break; }
            names = nn;
        }
        if (!(names[n] = strdup(e->d_name))) { rc = -1; break; }
        n++;
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_str);
    for (size_t i = 0; i < n; i++) { if (!rc) put_line(o, names[i]); free(names[i]); }
    free(names);
    return rc;
}

static int sh_ls(wbuf_t* o, int argc, char** argv) {
    if (argc == 1) return list_dir(o, ".");
    for (int i = 1; i < argc; i++) {
        if (access(argv[i], F_OK) != 0) return -1;
        DIR* d = opendir(argv[i]);
        if (!d) { put_line(o, argv[i]); continue; }
        closedir(d);
        if (argc > 2) {
            if (i > 1) wbuf_str(o, "\n");
            wbuf_str(o, argv[i]);
            wbuf_str(o, ":\n");
        }
        if (list_dir(o, argv[i]) != 0) return -1;
    }
    return 0;
}

static int cat_file(wbuf_t* o, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) wbuf_put(o, buf, got);
    int bad = ferror(f);
    fclose(f);
    return bad ? -1 : 0;
}

static int sh_cat(wbuf_t* o, int argc, char** argv) {
    if (argc < 2) return -1;
    for (int i = 1; i < argc; i++)
        if (cat_file(o, argv[i]) != 0) return -1;
    return 0;
}

static int sh_echo(wbuf_t* o, int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) wbuf_str(o, " ");
        wbuf_str(o, argv[i]);
    }
    wbuf_str(o, "\n");
    return 0;
}

static int sh_mkdir(int argc, char** argv) {
    int parents = argc > 1 && strcmp(argv[1], "-p") == 0;
    if (argc < 2 + parents) return -1;
    for (int i = 1 + parents; i < argc; i++)
        if (parents ? ray_mkdir_p(argv[i]) != RAY_OK : mkdir(argv[i], 0755) != 0) return -1;
    return 0;
}

static int rm_tree(const char* path) {
    DIR* d = opendir(path);
    if (!d) return unlink(path);
    struct dirent* e;
    int rc = 0;
    while (!rc && (e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        size_t n = strlen(path) + strlen(e->d_name) + 2;
        char* child = malloc(n);
        if (!child) { rc = -1; break; }
        snprintf(child, n, "%s/%s", path, e->d_name);
        rc = rm_tree(child);
        free(child);
    }
    closedir(d);
    return rc ? rc : rmdir(path);
}

/* rm [-r] [-f] path…  (flags combine: -rf, -fr, -R) */
static int sh_rm(int argc, char** argv) {
    int recurse = 0, force = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char* f = argv[i] + 1; *f; f++) {
            if (*f == 'r' || *f == 'R') recurse = 1;
            else if (*f == 'f') force = 1;
            else return -1;
        }
    if (i == argc) return force ? 0 : -1;
    for (; i < argc; i++) {
        if (access(argv[i], F_OK) != 0) { if (force) continue; return -1; }
        if ((recurse ? rm_tree(argv[i]) : unlink(argv[i])) != 0) return -1;
    }
    return 0;
}

/* head/tail [-n N | -N] file — the first/last N lines (default 10) of one file. */
static int sh_head_tail(wbuf_t* o, int tail, int argc, char** argv) {
    long lines = 10;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-n") == 0 && i + 1 < argc) { lines = strtol(argv[i + 1], NULL, 10); i += 2; }
    else if (i < argc && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') lines = strtol(argv[i++] + 1, NULL, 10);
    if (i != argc - 1 || lines < 0) return -1;
    wbuf_t all = {0};
    if (cat_file(&all, argv[i]) != 0 || all.oom) { free(all.p); return -1; }
    size_t from = 0, to = all.n;
    if (tail) {
        size_t k = all.n && all.p[all.n - 1] == '\n' ? all.n - 1 : all.n;
        long seen = 0;
        while (k > 0 && !(all.p[k - 1] == '\n' && ++seen == lines)) k--;
        from = lines ? k : all.n;
    } else {
        long seen = 0;
        for (to = 0; to < all.n && seen < lines; to++) if (all.p[to] == '\n') seen++;
    }
    wbuf_put(o, all.p + from, to - from);
    free(all.p);
    return 0;
}

char* q_sys_host_shell(const char* cmd, size_t* len) {
    char* line = strdup(cmd);
    if (!line) return NULL;
    char* argv[SH_MAX_ARGS];
    int argc = 0;
    for (char* t = strtok(line, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
        if (argc == SH_MAX_ARGS) { free(line); return NULL; }
        argv[argc++] = t;
    }
    wbuf_t o = {0};
    int rc = -1;
    if (argc == 0) rc = 0;
    else if (strcmp(argv[0], "pwd") == 0 && argc == 1) {
        char cwd[4096];
        if (getcwd(cwd, sizeof cwd)) { put_line(&o, cwd); rc = 0; }
    }
    else if (strcmp(argv[0], "ls") == 0)    rc = sh_ls(&o, argc, argv);
    else if (strcmp(argv[0], "cat") == 0)   rc = sh_cat(&o, argc, argv);
    else if (strcmp(argv[0], "echo") == 0)  rc = sh_echo(&o, argc, argv);
    else if (strcmp(argv[0], "mkdir") == 0) rc = sh_mkdir(argc, argv);
    else if (strcmp(argv[0], "rm") == 0)    rc = sh_rm(argc, argv);
    else if (strcmp(argv[0], "head") == 0)  rc = sh_head_tail(&o, 0, argc, argv);
    else if (strcmp(argv[0], "tail") == 0)  rc = sh_head_tail(&o, 1, argc, argv);
    free(line);
    if (rc != 0 || o.oom) { free(o.p); return NULL; }
    wbuf_str(&o, "");                       /* empty stdout is still a success: never NULL */
    *len = o.n;
    return o.p;
}
