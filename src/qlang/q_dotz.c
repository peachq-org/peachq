/* q_dotz — see q_dotz.h.  The eval-time `.z.*` command-line resolver. */
#include "qlang/q_dotz.h"
#include <rayforce.h>
#include <string.h>

/* Cached process-constant values (owned refs, rc>=1) and the script path. */
static ray_t*      g_z_f = NULL;   /* script file, symbol (null sym if none)   */
static ray_t*      g_z_x = NULL;   /* args after the script, list of strings   */
static ray_t*      g_z_X = NULL;   /* full raw argv incl. binary, list of str  */
static const char* g_script = NULL;

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

static ray_t* strings_list(char** argv, int lo, int hi) {
    int    n   = hi - lo;
    ray_t* out = ray_list_new(n > 0 ? n : 1);
    for (int i = lo; i < hi; i++) {
        ray_t* s = ray_str(argv[i], strlen(argv[i]));
        out = ray_list_append(out, s);   /* append RETAINS */
        ray_release(s);
    }
    return out;
}

void q_dotz_init(int argc, char** argv) {
    q_dotz_destroy();

    /* Locate the positional `*.q` script: the first token ending in ".q" that
     * is not the value of a value-consuming flag. */
    int script_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && flag_takes_value(argv[i - 1])) continue;  /* skip flag value */
        if (ends_with_dot_q(argv[i])) { script_idx = i; break; }
    }

    if (script_idx >= 0) {
        g_script = argv[script_idx];
        g_z_f    = ray_sym(ray_sym_intern(g_script, strlen(g_script)));
        /* `.z.x` = every token positioned AFTER the script (kdb also drops its
         * own recognized options; increment-1 tests pass only non-flag args,
         * so position-based is kdb-true here — flag stripping is a follow-on). */
        g_z_x = strings_list(argv, script_idx + 1, argc);
    } else {
        g_script = NULL;
        g_z_f    = ray_sym(ray_sym_intern("", 0));  /* null symbol */
        g_z_x    = ray_list_new(1);                 /* empty list  */
    }

    g_z_X = strings_list(argv, 0, argc);
}

const char* q_dotz_script_path(void) { return g_script; }

ray_t* q_dotz_resolve(int64_t sym_id) {
    ray_t* name = ray_sym_str(sym_id);   /* BORROWED: the cached arena string
                                          * atom (RAY_ATTR_ARENA); the
                                          * ray_release below is a no-op — do
                                          * not rely on it or "fix a leak" here */
    if (!name) return NULL;
    const char* p = ray_str_ptr(name);
    size_t      n = ray_str_len(name);

    ray_t* hit = NULL;
    if (n == 4 && memcmp(p, ".z.f", 4) == 0) hit = g_z_f;
    else if (n == 4 && memcmp(p, ".z.x", 4) == 0) hit = g_z_x;
    else if (n == 4 && memcmp(p, ".z.X", 4) == 0) hit = g_z_X;

    ray_release(name);
    if (!hit) return NULL;
    ray_retain(hit);
    return hit;
}

void q_dotz_destroy(void) {
    if (g_z_f) { ray_release(g_z_f); g_z_f = NULL; }
    if (g_z_x) { ray_release(g_z_x); g_z_x = NULL; }
    if (g_z_X) { ray_release(g_z_X); g_z_X = NULL; }
    g_script = NULL;
}
