/* q_mount — see q_mount.h.  Binds ride q_env_set (the one global-set home, so views and
 * `.z.vs` see them), under an ABSOLUTE sym so a later `\cd` cannot orphan the mapping. */
#define _GNU_SOURCE            /* realpath, strdup */
#include "qlang/io/q_mount.h"
#include "qlang/io/q_splay.h"
#include "qlang/net/q_wirefile.h"
#include "qlang/base/q_err.h"
#include "qlang/q_env.h"
#include "qlang/q_ctx.h"
#include "table/sym.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef RAY_OS_WINDOWS
  #include <direct.h>
#else
  #include <unistd.h>
#endif

/* A partition-typed dir name — date/month/year/long (basics/syscmds.md#l): stage 4's. */
static int mount_part_name(const char* s, size_t n) {
    size_t digits = 0, dots = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] >= '0' && s[i] <= '9') digits++;
        else if (s[i] == '.') dots++;
        else return 0;
    }
    if (digits == n) return n > 0;                                  /* year / long */
    if (n == 7)  return dots == 1 && s[4] == '.';                   /* YYYY.MM */
    if (n == 10) return dots == 2 && s[4] == '.' && s[7] == '.';    /* YYYY.MM.DD */
    return 0;
}

static int mount_is_dir(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mount_has_dotd(const char* dir, size_t n) {
    char p[PATH_MAX];
    struct stat st;
    return snprintf(p, sizeof p, "%.*s/.d", (int)n, dir) < (int)sizeof p &&
           stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Bind `name` to `dir`'s mapped table — `.d` and column HEADERS only, no column data
 * (kb/splayed-tables.md:217). */
static ray_t* mount_bind_splay(const char* dir, size_t n, const char* name, size_t nn) {
    char h[PATH_MAX + 2];
    if (snprintf(h, sizeof h, ":%.*s/", (int)n, dir) >= (int)sizeof h) return q_err(QE_OS);
    ray_t* hs = ray_sym(ray_sym_intern_runtime(h, n + 2));
    if (!hs || RAY_IS_ERR(hs)) return hs ? hs : q_err(QE_OOM);
    ray_t* car = q_splay_get(hs);
    ray_release(hs);
    if (!car) return q_err(QE_IO);
    if (RAY_IS_ERR(car)) return car;
    ray_err_t rc = q_env_set(ray_sym_intern_runtime(name, nn), car);
    ray_release(car);
    return rc == RAY_OK ? NULL : q_env_err(rc);
}

static ray_t* mount_bind_object(const char* path, size_t n, const char* name, size_t nn) {
    ray_t* ps = ray_str(path, n);
    if (!ps || RAY_IS_ERR(ps)) return ps ? ps : q_err(QE_OOM);
    ray_t* v = q_wirefile_read_column(ps);
    ray_release(ps);
    if (!v) return q_err(QE_IO);
    if (RAY_IS_ERR(v)) return v;
    ray_err_t rc = q_env_set(ray_sym_intern_runtime(name, nn), v);
    ray_release(v);
    return rc == RAY_OK ? NULL : q_env_err(rc);
}

static int mount_name_cmp(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* A bind error the walk survives: content this mount cannot LOAD (foreign file, corrupt
 * or deferred table).  Resource/policy failure — OOM, restricted eval — aborts instead. */
static int mount_skippable(ray_t* e) {
    return !q_err_is(e, QE_OOM) && !q_err_is(e, QE_ACCESS);
}

/* Database-root walk, sorted for a deterministic bind/execute order.  A name that is not
 * a plain q identifier is SKIPPED — kx's binding of odd names is undocumented, and refusing
 * keeps `.pqattr` sidecars, `par.txt` and `sym##` interns out of the env for free. */
static ray_t* mount_root(const char* abs, size_t n, int scripts) {
    DIR* d = opendir(abs);
    if (!d) return q_err(QE_OS);
    char** names = NULL;
    size_t cnt = 0, cap = 0;
    int oom = 0;
    struct dirent* de;
    while ((de = readdir(d))) {
        const char* nm = de->d_name;
        size_t nn = strlen(nm);
        /* hidden entries and the `$`-suffixed ("Never mind the dollars") are ignored */
        if (nm[0] == '.' || nm[nn - 1] == '$') continue;
        if (cnt == cap) {
            size_t nc = cap ? cap * 2 : 16;
            char** nv = (char**)realloc(names, nc * sizeof *nv);
            if (!nv) { oom = 1; break; }
            names = nv; cap = nc;
        }
        if (!(names[cnt] = strdup(nm))) { oom = 1; break; }
        cnt++;
    }
    closedir(d);
    qsort(names, cnt, sizeof *names, mount_name_cmp);
    ray_t* bad = oom ? q_err(QE_OOM) : NULL;   /* a partial roster must not mount */
    for (size_t i = 0; i < cnt && !bad; i++) {
        const char* nm = names[i];
        size_t nn = strlen(nm);
        char full[PATH_MAX];
        if (snprintf(full, sizeof full, "%.*s/%s", (int)n, abs, nm) >= (int)sizeof full) {
            bad = q_err(QE_OS);            /* an entry we cannot even NAME is not a skip */
            break;
        }
        size_t fn = n + 1 + nn;
        if (mount_is_dir(full)) {
            /* stage 4's partitions, a plain subdir, an unbindable name */
            if (mount_part_name(nm, nn) || !mount_has_dotd(full, fn) ||
                !q_env_ident_ok(nm, nn)) continue;
            ray_t* e = mount_bind_splay(full, fn, nm, nn);
            if (e && mount_skippable(e)) ray_error_free(e);
            else if (e) bad = e;
        } else if (nn > 2 && memcmp(nm + nn - 2, ".q", 2) == 0) {
            if (!scripts) continue;                               /* `\l .` reloads data only */
            ray_t* esig = NULL;
            int rc = q_ctx_run_file(full, stdout, stderr, &esig);
            if (esig) bad = esig;         /* a script the walk RAN aborts it; rc 1 = unreadable */
            else if (rc) bad = rc == 1 ? q_err(QE_OS) : q_err((q_err_e)(rc - 2));
        } else if (q_env_ident_ok(nm, nn)) {
            ray_t* e = mount_bind_object(full, fn, nm, nn);
            if (e && mount_skippable(e)) ray_error_free(e);       /* not a kdb file */
            else if (e) bad = e;
        }
    }
    for (size_t i = 0; i < cnt; i++) free(names[i]);
    free(names);
    return bad;
}

ray_t* q_mount_dir(const char* path, int scripts) {
    char abs[PATH_MAX];
#ifdef RAY_OS_WINDOWS
    if (!_fullpath(abs, path, sizeof abs)) return q_err(QE_OS);
    for (char* p = abs; *p; p++) if (*p == '\\') *p = '/';
    if (!mount_is_dir(abs)) return q_err(QE_OS);
#else
    if (!realpath(path, abs)) return q_err(QE_OS);
#endif
    size_t n = strlen(abs);
    while (n > 1 && abs[n - 1] == '/') abs[--n] = '\0';
    /* the opened directory becomes the current directory (syscmds.md) */
    if (chdir(abs) != 0) return q_err(QE_OS);
    if (!mount_has_dotd(abs, n)) return mount_root(abs, n, scripts);
    const char* base = strrchr(abs, '/');
    base = base ? base + 1 : abs;
    size_t bn = strlen(base);
    if (!q_env_ident_ok(base, bn)) return q_err_name(path, strlen(path));
    ray_t* e = mount_bind_splay(abs, n, base, bn);
    if (e) return e;
    return ray_sym(ray_sym_intern_runtime(base, bn));   /* kx echoes the name */
}
