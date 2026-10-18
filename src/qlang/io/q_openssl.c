/* q_openssl — see q_openssl.h.  The soname tables and the first-that-opens walk, once the TLS unit's own;
 * RTLD_GLOBAL as it always was, so q_dl_open's RTLD_NODELETE path is not the one used here. */
#include "qlang/q_count.h"
#include "qlang/io/q_openssl.h"
#include "core/platform.h"   /* RAY_OS_WINDOWS */

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define DSO_OPEN(p)     ((void*)LoadLibraryA(p))
#else
  #include <dlfcn.h>
  #define DSO_OPEN(p)     dlopen((p), RTLD_NOW | RTLD_GLOBAL)
#endif

/* kb/ssl.md:25; two doc entries lack `.so`, so both spellings are tried. */
static const char* const SSL_SONAMES[] = {
#ifdef RAY_OS_WINDOWS
    "libssl-3-x64.dll", "libssl-1_1-x64.dll", "libssl-3.dll", "libssl-1_1.dll",
#elif defined(__APPLE__)
    "libssl.3.dylib", "libssl.1.1.dylib", "libssl.dylib",
#else
    "libssl.so", "libssl.so.3", "libssl.so.1.1", "libssl.1.1", "libssl.so.1.0",
    "libssl.so.10", "libssl.so.1.0.2", "libssl.so.1.0.1", "libssl.so.1.0.0",
    "libssl.1.0.0",
#endif
    NULL
};
static const char* const CRYPTO_SONAMES[] = {
#ifdef RAY_OS_WINDOWS
    "libcrypto-3-x64.dll", "libcrypto-1_1-x64.dll", "libcrypto-3.dll", "libcrypto-1_1.dll",
#elif defined(__APPLE__)
    "libcrypto.3.dylib", "libcrypto.1.1.dylib", "libcrypto.dylib",
#else
    "libcrypto.so", "libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so.1.0.0",
#endif
    NULL
};

static void* openssl_first(const char* const* names) {
    for (int i = 0; names[i]; i++) {
        void* h = DSO_OPEN(names[i]);
        if (h) return h;
    }
    return NULL;
}

static void* g_ssl, * g_crypto;
static int   g_ssl_walked, g_crypto_walked;   /* a walk that finds nothing is not repeated */

void* q_openssl_ssl(void) {
    if (!g_ssl_walked) { g_ssl_walked = 1; g_ssl = openssl_first(SSL_SONAMES); }
    return g_ssl;
}

void* q_openssl_crypto(void) {
    if (!g_crypto_walked) { g_crypto_walked = 1; g_crypto = openssl_first(CRYPTO_SONAMES); }
    return g_crypto ? g_crypto : q_openssl_ssl();
}
