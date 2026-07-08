/* q_dotz — see q_dotz.h.  The eval-time `.z.*` command-line resolver. */
#include "qlang/q_dotz.h"
#include <rayforce.h>
#include <string.h>

/* argv is process-lifetime (owned by main), so we cache only the pointers and
 * the script's position and MINT each `.z.*` value on demand.  These values
 * are immutable argv snapshots, cheap to build, and read rarely — caching them
 * as owned `ray_t*` would add lifecycle (init/destroy/retain) without benefit.
 * Adding a `.z.*` name = one Z_TAB row + a small producer; init/destroy and the
 * resolver are untouched. */
static int    g_argc       = 0;
static char** g_argv       = NULL;
static int    g_script_idx = -1;   /* argv index of the `*.q` script, or -1 */

static bool ends_with_dot_q(const char* s) {
    size_t n = strlen(s);
    return n >= 2 && s[n - 2] == '.' && s[n - 1] == 'q';
}

/* A value-consuming q flag: its FOLLOWING argv token is a value, not the
 * script.  WARNING — hand-duplicated from qmain.c's flag parse (-p/--port/
 * -u/-U): if you add a value-taking flag to qmain.c you MUST add it here too,
 * or its value will be mis-detected as the `*.q` script path (wrong .z.f/.z.x). */
static bool flag_takes_value(const char* s) {
    return strcmp(s, "-p") == 0 || strcmp(s, "--port") == 0 ||
           strcmp(s, "-u") == 0 || strcmp(s, "-U") == 0;
}

/* argv[lo..hi) as a q list of strings (empty list, not null, when lo==hi). */
static ray_t* strings_list(int lo, int hi) {
    int    n   = hi - lo;
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    for (int i = lo; i < hi; i++) {
        ray_t* s = ray_str(g_argv[i], strlen(g_argv[i]));
        out = ray_list_append(out, s);   /* append RETAINS */
        ray_release(s);
    }
    return out;
}

/* `.z.*` producers — each mints a FRESH owned ref (rc>=1), matching the
 * name-hook contract (the resolver returns an owned value or NULL). */
static ray_t* z_f(void) {   /* script file symbol; null sym when no script */
    const char* s = g_script_idx < 0 ? "" : g_argv[g_script_idx];
    return ray_sym(ray_sym_intern(s, strlen(s)));
}
static ray_t* z_x(void) {   /* args positioned AFTER the script (empty if none) */
    int lo = g_script_idx < 0 ? g_argc : g_script_idx + 1;
    return strings_list(lo, g_argc);
}
static ray_t* z_X(void) {   /* full raw argv, including the binary */
    return strings_list(0, g_argc);
}

static const struct { const char* name; uint8_t len; ray_t* (*make)(void); }
Z_TAB[] = {
    { ".z.f", 4, z_f },
    { ".z.x", 4, z_x },
    { ".z.X", 4, z_X },
};

void q_dotz_init(int argc, char** argv) {
    g_argc = argc;
    g_argv = argv;

    /* Locate the positional `*.q` script: the first token ending in ".q" that
     * is not the value of a value-consuming flag.  (`.z.x` = every token AFTER
     * it; kdb also drops its own recognized options — position-based is
     * kdb-true for the non-flag args the increment-1 tests pass.) */
    g_script_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && flag_takes_value(argv[i - 1])) continue;  /* skip flag value */
        if (ends_with_dot_q(argv[i])) { g_script_idx = i; break; }
    }
}

const char* q_dotz_script_path(void) {
    return g_script_idx < 0 ? NULL : g_argv[g_script_idx];
}

ray_t* q_dotz_resolve(int64_t sym_id) {
    ray_t* name = ray_sym_str(sym_id);   /* BORROWED: cached arena string atom
                                          * (RAY_ATTR_ARENA); the ray_release
                                          * below is a no-op — do not rely on it
                                          * or "fix a leak" here */
    if (!name) return NULL;
    const char* p = ray_str_ptr(name);
    size_t      n = ray_str_len(name);

    ray_t* out = NULL;
    for (size_t i = 0; i < sizeof Z_TAB / sizeof *Z_TAB; i++)
        if (n == Z_TAB[i].len && memcmp(p, Z_TAB[i].name, n) == 0) {
            out = Z_TAB[i].make();   /* already owned (rc>=1) */
            break;
        }

    ray_release(name);
    return out;
}

void q_dotz_destroy(void) {
    g_argc       = 0;
    g_argv       = NULL;
    g_script_idx = -1;
}
