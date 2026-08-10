/* q_exedir — see q_exedir.h.  One home, three primitives: /proc/self/exe on
 * Linux, _NSGetExecutablePath on macOS, GetModuleFileNameA on Windows.
 *
 * NOT dladdr: it reports the INVOCATION path, so `./q` answers "." and every
 * candidate built from it would resolve against the cwd instead of the binary.
 * macOS still hands back the launch spelling, hence the realpath. */
#define _POSIX_C_SOURCE 200809L
#include "qlang/io/q_exedir.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdlib.h>
#elif !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

int q_exedir(char* dst, size_t cap) {
    dst[0] = '\0';
    if (cap < 2) return 0;
    char exe[1024];
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (n == 0 || n >= sizeof exe) return 0;   /* n == size means truncated */
    exe[n] = '\0';
#elif defined(__APPLE__)
    char     raw[1024];
    uint32_t rawn = (uint32_t)sizeof raw;
    if (_NSGetExecutablePath(raw, &rawn) != 0) return 0;
    char* real = realpath(raw, NULL);   /* NULL out: no PATH_MAX buffer to get wrong */
    snprintf(exe, sizeof exe, "%s", real ? real : raw);
    free(real);
#elif defined(__EMSCRIPTEN__)
    return 0;                                   /* nothing ships beside a .wasm */
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = '\0';
#endif
    char* sep = strrchr(exe, '/');
#if defined(_WIN32)
    char* bsep = strrchr(exe, '\\');
    if (bsep && (!sep || bsep > sep)) sep = bsep;   /* never compare against NULL */
#endif
    if (!sep) return 0;
    if (sep == exe) sep++;              /* the root itself: the dir is "/" */
    *sep       = '\0';
    size_t len = strlen(exe);
    if (len + 1 > cap) return 0;
    memcpy(dst, exe, len + 1);
    return 1;
}
