/* q_dl — see q_dl.h. */
#include "qlang/io/q_dl.h"
#include <stdio.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

void* q_dl_open(const char* path) {
#if defined(_WIN32)
    char native[1200];
    if (snprintf(native, sizeof native, "%s", path) >= (int)sizeof native) return NULL;
    for (char* p = native; *p; p++) if (*p == '/') *p = '\\';   /* LoadLibrary documents backslashes only */
    return (void*)LoadLibraryA(native);                          /* never FreeLibrary'd */
#else
    return dlopen(path, RTLD_NOW | RTLD_NODELETE);
#endif
}

void* q_dl_sym(void* lib, const char* name) {
#if defined(_WIN32)
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}
