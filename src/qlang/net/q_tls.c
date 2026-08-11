/* q_tls — see q_tls.h.  Observables: kb/ssl.md, internal.md:350, dotz.md:215. */
#include "qlang/net/q_tls.h"
#include "qlang/base/q_err.h"
#include "core/timer.h"      /* ray_time_now_ms — send/handshake deadlines */
#include "table/sym.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  typedef HMODULE dso_t;
  #define DSO_OPEN(p)     LoadLibraryA(p)
  #define DSO_SYM(h, n)   ((void*)GetProcAddress((h), (n)))
  #define DSO_NULL        NULL
#else
  #include <dlfcn.h>
  typedef void* dso_t;
  #define DSO_OPEN(p)     dlopen((p), RTLD_NOW | RTLD_GLOBAL)
  #define DSO_SYM(h, n)   dlsym((h), (n))
  #define DSO_NULL        NULL
#endif

#define TLS_VERIFY_NONE          0x00
#define TLS_VERIFY_PEER          0x01
#define TLS_VERIFY_NEED_CERT     0x02
#define TLS_FILETYPE_PEM         1
#define TLS_RECORD_HANDSHAKE     0x16   /* first byte of every ClientHello (RFC 8446) */
#define TLS_HANDSHAKE_MS         5000
#define TLS_CTRL_SET_HOSTNAME    55
#define TLS_NAMETYPE_HOST        0
#define TLS_ERROR_WANT_READ      2
#define TLS_ERROR_WANT_WRITE     3
#define TLS_ERROR_ZERO_RETURN    6
#define TLS_X509_V_OK            0
#define TLS_VERSION_STRING       0

typedef struct {
    dso_t ssl, crypto;
    int         (*OPENSSL_init_ssl)(uint64_t, const void*);
    int         (*SSL_library_init)(void);
    const void* (*TLS_client_method)(void);
    const void* (*SSLv23_client_method)(void);
    const void* (*TLS_server_method)(void);
    const void* (*SSLv23_server_method)(void);
    void*       (*SSL_CTX_new)(const void*);
    void        (*SSL_CTX_free)(void*);
    void        (*SSL_CTX_set_verify)(void*, int, void*);
    int         (*SSL_CTX_load_verify_locations)(void*, const char*, const char*);
    int         (*SSL_CTX_set_default_verify_paths)(void*);
    int         (*SSL_CTX_set_cipher_list)(void*, const char*);
    void*       (*SSL_new)(void*);
    void        (*SSL_free)(void*);
    int         (*SSL_CTX_use_certificate_chain_file)(void*, const char*);
    int         (*SSL_CTX_use_PrivateKey_file)(void*, const char*, int);
    int         (*SSL_CTX_check_private_key)(const void*);
    int         (*SSL_set_fd)(void*, int);
    int         (*SSL_connect)(void*);
    int         (*SSL_accept)(void*);
    int         (*SSL_read)(void*, void*, int);
    int         (*SSL_write)(void*, const void*, int);
    int         (*SSL_pending)(const void*);
    int         (*SSL_get_error)(const void*, int);
    int         (*SSL_shutdown)(void*);
    long        (*SSL_get_verify_result)(const void*);
    long        (*SSL_ctrl)(void*, int, long, void*);
    int         (*SSL_set1_host)(void*, const char*);
    void*       (*SSL_get0_param)(void*);
    int         (*X509_VERIFY_PARAM_set1_host)(void*, const char*, size_t);
    int         (*X509_VERIFY_PARAM_set1_ip_asc)(void*, const char*);
    const char* (*SSL_get_version)(const void*);
    const void* (*SSL_get_current_cipher)(const void*);
    const char* (*SSL_CIPHER_get_name)(const void*);
    const char* (*OpenSSL_version)(int);
    const char* (*SSLeay_version)(int);
    const char* (*X509_get_default_cert_dir)(void);
    const char* (*X509_get_default_cert_file)(void);
    /* peer-certificate readback for `.z.e`'s CERT (ref/dotz.md:215) */
    void*       (*SSL_get1_peer_certificate)(const void*);
    void*       (*SSL_get_peer_certificate)(const void*);
    void        (*X509_free)(void*);
    void*       (*X509_get_subject_name)(const void*);
    void*       (*X509_get_issuer_name)(const void*);
    char*       (*X509_NAME_oneline)(const void*, char*, int);
    void*       (*X509_get_serialNumber)(void*);
    const void* (*X509_get0_notBefore)(const void*);
    const void* (*X509_get0_notAfter)(const void*);
    int         (*ASN1_TIME_print)(void*, const void*);
    int         (*i2a_ASN1_INTEGER)(void*, const void*);
    void*       (*BIO_new)(const void*);
    const void* (*BIO_s_mem)(void);
    int         (*BIO_read)(void*, void*, int);
    int         (*BIO_free)(void*);
} tls_api_t;

static tls_api_t A;
static int       g_loaded = 0;    /* 0 untried, 1 ready, -1 unavailable */

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

static dso_t dso_first(const char* const* names) {
    for (int i = 0; names[i]; i++) {
        dso_t h = DSO_OPEN(names[i]);
        if (h != DSO_NULL) return h;
    }
    return DSO_NULL;
}

/* Via a void* slot: casting dlsym's result to a function pointer is not ISO C. */
#define SYM(h, f)  (*(void**)(&A.f) = DSO_SYM((h), #f))

static int tls_load(void) {
    if (g_loaded) return g_loaded;
    g_loaded = -1;
    memset(&A, 0, sizeof A);
    A.ssl = dso_first(SSL_SONAMES);
    if (A.ssl == DSO_NULL) return g_loaded;
    A.crypto = dso_first(CRYPTO_SONAMES);
    dso_t cr = (A.crypto != DSO_NULL) ? A.crypto : A.ssl;

    SYM(A.ssl, OPENSSL_init_ssl);     SYM(A.ssl, SSL_library_init);
    SYM(A.ssl, TLS_client_method);    SYM(A.ssl, SSLv23_client_method);
    SYM(A.ssl, TLS_server_method);    SYM(A.ssl, SSLv23_server_method);
    SYM(A.ssl, SSL_CTX_use_certificate_chain_file);
    SYM(A.ssl, SSL_CTX_use_PrivateKey_file);
    SYM(A.ssl, SSL_CTX_check_private_key);
    SYM(A.ssl, SSL_accept);
    SYM(A.ssl, SSL_get1_peer_certificate);
    SYM(A.ssl, SSL_get_peer_certificate);
    SYM(cr,    X509_free);            SYM(cr,    X509_get_subject_name);
    SYM(cr,    X509_get_issuer_name); SYM(cr,    X509_NAME_oneline);
    SYM(cr,    X509_get_serialNumber);
    SYM(cr,    X509_get0_notBefore);  SYM(cr,    X509_get0_notAfter);
    SYM(cr,    ASN1_TIME_print);      SYM(cr,    i2a_ASN1_INTEGER);
    SYM(cr,    BIO_new);              SYM(cr,    BIO_s_mem);
    SYM(cr,    BIO_read);             SYM(cr,    BIO_free);
    SYM(A.ssl, SSL_CTX_new);          SYM(A.ssl, SSL_CTX_free);
    SYM(A.ssl, SSL_CTX_set_verify);   SYM(A.ssl, SSL_CTX_load_verify_locations);
    SYM(A.ssl, SSL_CTX_set_default_verify_paths);
    SYM(A.ssl, SSL_CTX_set_cipher_list);
    SYM(A.ssl, SSL_new);              SYM(A.ssl, SSL_free);
    SYM(A.ssl, SSL_set_fd);           SYM(A.ssl, SSL_connect);
    SYM(A.ssl, SSL_read);             SYM(A.ssl, SSL_write);
    SYM(A.ssl, SSL_pending);          SYM(A.ssl, SSL_get_error);
    SYM(A.ssl, SSL_shutdown);         SYM(A.ssl, SSL_get_verify_result);
    SYM(A.ssl, SSL_ctrl);             SYM(A.ssl, SSL_set1_host);
    SYM(A.ssl, SSL_get0_param);       SYM(cr, X509_VERIFY_PARAM_set1_host);
    SYM(cr,    X509_VERIFY_PARAM_set1_ip_asc);
    SYM(A.ssl, SSL_get_version);      SYM(A.ssl, SSL_get_current_cipher);
    SYM(A.ssl, SSL_CIPHER_get_name);  SYM(A.ssl, OpenSSL_version);
    SYM(A.ssl, SSLeay_version);
    SYM(cr,    X509_get_default_cert_dir);
    SYM(cr,    X509_get_default_cert_file);

    if (!A.SSL_CTX_new || !A.SSL_CTX_free || !A.SSL_new || !A.SSL_free ||
        !A.SSL_set_fd || !A.SSL_connect || !A.SSL_read || !A.SSL_write ||
        !A.SSL_get_error || !A.SSL_CTX_load_verify_locations ||
        !A.SSL_CTX_set_verify || !A.SSL_get_verify_result ||
        (!A.TLS_client_method && !A.SSLv23_client_method))
        return g_loaded;

    if (A.OPENSSL_init_ssl)      A.OPENSSL_init_ssl(0, NULL);
    else if (A.SSL_library_init) A.SSL_library_init();
    g_loaded = 1;
    return g_loaded;
}

/* Re-read per connection; KX_ wins (kb/ssl.md:75). */
enum { CFG_CERT_FILE, CFG_CA_CERT_FILE, CFG_CA_CERT_PATH, CFG_KEY_FILE,
       CFG_CIPHER_LIST, CFG_VERIFY_CLIENT, CFG_VERIFY_SERVER, CFG_N };

static const char* const CFG_NAMES[CFG_N] = {
    "SSL_CERT_FILE", "SSL_CA_CERT_FILE", "SSL_CA_CERT_PATH", "SSL_KEY_FILE",
    "SSL_CIPHER_LIST", "SSL_VERIFY_CLIENT", "SSL_VERIFY_SERVER"
};
static const char* const CFG_DEFAULTS[CFG_N] = {
    /* kb/ssl.md:150,169.  VERIFY_SERVER defaults to HOSTIP, not kdb's YES: the
     * ONE divergence, and -26! reads it back so the effective policy is visible. */
    NULL, NULL, NULL, NULL, NULL, "NO", "HOSTIP"
};

static const char* cfg_get(int k) {
    char kx[64];
    snprintf(kx, sizeof kx, "KX_%s", CFG_NAMES[k]);
    const char* v = getenv(kx);
    if (!v || !*v) v = getenv(CFG_NAMES[k]);
    if (!v || !*v) v = CFG_DEFAULTS[k];
    return v;
}

static const char* tls_version_string(void) {
    if (tls_load() != 1) return NULL;
    if (A.OpenSSL_version) return A.OpenSSL_version(TLS_VERSION_STRING);
    if (A.SSLeay_version)  return A.SSLeay_version(TLS_VERSION_STRING);
    return NULL;
}

typedef struct { void* ctx; void* ssl; ray_sock_t fd; } tls_sess_t;

/* An accepted server connection is NON-BLOCKING (it lives on the poll), so both
 * retries below are live; renegotiation is why a write can want a read.  Each
 * wait is bounded by what is LEFT of the caller's deadline, never by a fresh
 * budget per retry: a peer that stops reading must not be able to hold the poll
 * thread past the deadline its caller set. */
static int64_t sess_send(void* p, const void* buf, size_t len) {
    tls_sess_t* s = (tls_sess_t*)p;
    const uint8_t* q = (const uint8_t*)buf;
    size_t rem = len;
    int     bound    = ray_sock_io_timeout(s->fd);
    int64_t deadline = bound > 0 ? ray_time_now_ms() + bound : 0;
    while (rem > 0) {
        int chunk = rem > 0x7fffffff ? 0x7fffffff : (int)rem;
        int n = A.SSL_write(s->ssl, q, chunk);
        if (n > 0) { q += n; rem -= (size_t)n; continue; }
        int e = A.SSL_get_error(s->ssl, n);
        if (e != TLS_ERROR_WANT_WRITE && e != TLS_ERROR_WANT_READ) return -1;
        int wait = -1;
        if (deadline) {
            int64_t left = deadline - ray_time_now_ms();
            if (left <= 0) return -1;
            wait = left > INT_MAX ? INT_MAX : (int)left;
        }
        int rr = (e == TLS_ERROR_WANT_WRITE) ? ray_sock_wait_writable(s->fd, wait)
                                             : ray_sock_wait_readable(s->fd, wait);
        if (rr <= 0) return -1;
    }
    return (int64_t)len;
}

static int64_t sess_recv(void* p, void* buf, size_t len) {
    tls_sess_t* s = (tls_sess_t*)p;
    int cap = len > 0x7fffffff ? 0x7fffffff : (int)len;
    int n = A.SSL_read(s->ssl, buf, cap);
    if (n > 0) return n;
    int e = A.SSL_get_error(s->ssl, n);
    /* ONLY close-notify ends the stream: a bare FIN is an injectable truncation
     * a close-framed reader would take for a whole response. */
    if (e == TLS_ERROR_ZERO_RETURN) return 0;
    /* An exhausted SSL buffer is the poll pump's EAGAIN, not a dead peer —
     * without this the first drained record would tear the connection down.
     * errno is set on BOTH arms so a caller can tell retry from fatal. */
    errno = (e == TLS_ERROR_WANT_READ || e == TLS_ERROR_WANT_WRITE) ? EAGAIN : EIO;
    return -1;
}

static size_t sess_pending(void* p) {
    tls_sess_t* s = (tls_sess_t*)p;
    if (!A.SSL_pending) return 0;
    int n = A.SSL_pending(s->ssl);
    return n > 0 ? (size_t)n : 0;
}

static void sess_close(void* p) {
    tls_sess_t* s = (tls_sess_t*)p;
    if (!s) return;
    if (s->ssl) { if (A.SSL_shutdown) A.SSL_shutdown(s->ssl); A.SSL_free(s->ssl); }
    if (s->ctx) A.SSL_CTX_free(s->ctx);
    free(s);
}

/* An explicit CA is honoured exactly as kdb does, failures included; the system
 * store is only a fallback on an UNSET variable, read from cert_dir() directly —
 * SSL_CTX_set_default_verify_paths resolves via $SSL_CERT_FILE, kdb's name for
 * the process's OWN cert (kb/ssl.md:89), which would become the CA store.  The
 * two defaults load separately: one call fails if either path is absent. */
static int ctx_trust(void* ctx) {
    const char* ca_file = cfg_get(CFG_CA_CERT_FILE);
    const char* ca_path = cfg_get(CFG_CA_CERT_PATH);
    if (ca_file || ca_path)
        return A.SSL_CTX_load_verify_locations(ctx, ca_file, ca_path) == 1 ? 0 : -1;

    const char* dir  = A.X509_get_default_cert_dir  ? A.X509_get_default_cert_dir()  : NULL;
    const char* file = A.X509_get_default_cert_file ? A.X509_get_default_cert_file() : NULL;
    int ok = 0;
    if (dir  && A.SSL_CTX_load_verify_locations(ctx, NULL, dir)  == 1) ok = 1;
    if (file && A.SSL_CTX_load_verify_locations(ctx, file, NULL) == 1) ok = 1;
    return ok ? 0 : -1;
}

static int sess_attach(ray_sock_t fd, void* ctx, void* ssl) {
    tls_sess_t* s = (tls_sess_t*)calloc(1, sizeof *s);
    if (!s) return -1;
    s->ctx = ctx; s->ssl = ssl; s->fd = fd;
    ray_sock_io_t io = { s, sess_send, sess_recv, sess_close, sess_pending, 0 };
    if (ray_sock_io_attach(fd, &io) != 0) { free(s); return -1; }
    return 0;
}

static int host_is_ipv4(const char* h) {
    int dots = 0;
    for (const char* p = h; *p; p++) {
        if (*p == '.') { dots++; continue; }
        if (*p < '0' || *p > '9') return 0;
    }
    return dots == 3;
}

/* SSL_set1_host is 1.1.0+, the param setters 1.0.2+; an IP literal must match an
 * iPAddress SAN, not a DNS name.  -1 when the stack can bind no peer name. */
static int set_peer_name(void* ssl, const char* host) {
    void* p = A.SSL_get0_param ? A.SSL_get0_param(ssl) : NULL;
    if (host_is_ipv4(host))
        return (p && A.X509_VERIFY_PARAM_set1_ip_asc &&
                A.X509_VERIFY_PARAM_set1_ip_asc(p, host) == 1) ? 0 : -1;
    if (A.SSL_set1_host) return A.SSL_set1_host(ssl, host) == 1 ? 0 : -1;
    if (p && A.X509_VERIFY_PARAM_set1_host)
        return A.X509_VERIFY_PARAM_set1_host(p, host, strlen(host)) == 1 ? 0 : -1;
    return -1;
}

int q_tls_client_start(ray_sock_t fd, const char* host, const char** err) {
    if (tls_load() != 1) { if (err) *err = "nyi"; return -1; }
    if (err) *err = "conn";

    const void* meth = A.TLS_client_method ? A.TLS_client_method()
                                           : A.SSLv23_client_method();
    if (!meth) return -1;
    void* ctx = A.SSL_CTX_new(meth);
    if (!ctx) return -1;

    /* kb/ssl.md:161-169: NO verifies nothing, YES is chain-only, HOSTIP adds
     * host/IP checking.  An unrecognised value takes the strictest reading. */
    const char* vs       = cfg_get(CFG_VERIFY_SERVER);
    int         verify   = strcmp(vs, "NO") != 0;
    int         bindname = verify && strcmp(vs, "YES") != 0;
    const char* ciph     = cfg_get(CFG_CIPHER_LIST);

    if (ciph && A.SSL_CTX_set_cipher_list &&
        A.SSL_CTX_set_cipher_list(ctx, ciph) != 1) goto fail_ctx;
    if (verify) {
        if (ctx_trust(ctx) != 0) goto fail_ctx;
        A.SSL_CTX_set_verify(ctx, TLS_VERIFY_PEER, NULL);
    } else {
        A.SSL_CTX_set_verify(ctx, TLS_VERIFY_NONE, NULL);
    }

    void* ssl = A.SSL_new(ctx);
    if (!ssl) goto fail_ctx;
    if (host && *host) {
        if (A.SSL_ctrl && !host_is_ipv4(host))   /* SNI is names only (RFC 6066) */
            A.SSL_ctrl(ssl, TLS_CTRL_SET_HOSTNAME, TLS_NAMETYPE_HOST, (void*)(uintptr_t)host);
        /* Refuse rather than continue when the stack can bind no peer name. */
        if (bindname && set_peer_name(ssl, host) != 0) goto fail_ssl;
    }
    if (A.SSL_set_fd(ssl, (int)fd) != 1) goto fail_ssl;
    if (A.SSL_connect(ssl) != 1) goto fail_ssl;
    if (verify && A.SSL_get_verify_result(ssl) != TLS_X509_V_OK) goto fail_ssl;

    if (sess_attach(fd, ctx, ssl) != 0) goto fail_ssl;
    return 0;

fail_ssl:
    A.SSL_free(ssl);
fail_ctx:
    A.SSL_CTX_free(ctx);
    return -1;
}

/* `-E` TLS server mode (cmdline.md, syscmds.md `\E`). */
static int g_server_mode = 0;

int  q_tls_server_mode(void) { return g_server_mode; }
void q_tls_server_mode_set(int mode) { g_server_mode = (mode >= 0 && mode <= 2) ? mode : 0; }

static int verify_always_ok(int preverify, void* store_ctx) {
    (void)preverify; (void)store_ctx; return 1;
}

/* kb/ssl.md:150-159.  REQUESTONLY needs the always-ok callback — without it
 * OpenSSL aborts on an invalid client cert, which is IFPRESENT's contract, not
 * REQUESTONLY's.  An unrecognised value takes the strictest reading (YES). */
static int ctx_verify_client(void* ctx) {
    const char* v = cfg_get(CFG_VERIFY_CLIENT);
    if (strcmp(v, "NO") == 0) { A.SSL_CTX_set_verify(ctx, TLS_VERIFY_NONE, NULL); return 0; }
    if (ctx_trust(ctx) != 0) return -1;
    if (strcmp(v, "REQUESTONLY") == 0)
        A.SSL_CTX_set_verify(ctx, TLS_VERIFY_PEER, (void*)(uintptr_t)verify_always_ok);
    else if (strcmp(v, "IFPRESENT") == 0)
        A.SSL_CTX_set_verify(ctx, TLS_VERIFY_PEER, NULL);
    else
        A.SSL_CTX_set_verify(ctx, TLS_VERIFY_PEER | TLS_VERIFY_NEED_CERT, NULL);
    return 0;
}

/* A negotiation spans many poll events, so its ctx/ssl outlive each call.  The
 * state hangs off the CALLER's per-connection record rather than a table here:
 * a fixed table would cap concurrent negotiations, and hitting that cap is
 * exactly what a slow-loris fleet would aim for.  Ownership moves to the
 * overlay session on success. */
typedef struct { void* ctx; void* ssl; } tls_hs_t;

static void hs_free(void** state) {
    tls_hs_t* h = (tls_hs_t*)*state;
    if (!h) return;
    if (h->ssl) A.SSL_free(h->ssl);
    if (h->ctx) A.SSL_CTX_free(h->ctx);
    free(h);
    *state = NULL;
}

/* Build the server context and bind it to fd. */
static int hs_begin(ray_sock_t fd, void** state) {
    if (tls_load() != 1) return -1;
    const void* meth = A.TLS_server_method ? A.TLS_server_method()
                     : A.SSLv23_server_method ? A.SSLv23_server_method() : NULL;
    if (!meth || !A.SSL_accept || !A.SSL_CTX_use_certificate_chain_file ||
        !A.SSL_CTX_use_PrivateKey_file) return -1;

    /* No cert or key configured = no server identity: refuse rather than fall
     * back to plaintext, which is the whole point of `-E 2`. */
    const char* cert = cfg_get(CFG_CERT_FILE);
    const char* key  = cfg_get(CFG_KEY_FILE);
    if (!cert || !key) return -1;

    tls_hs_t* h = (tls_hs_t*)calloc(1, sizeof *h);
    if (!h) return -1;

    void* ctx = A.SSL_CTX_new(meth);
    if (!ctx) { free(h); return -1; }
    const char* ciph = cfg_get(CFG_CIPHER_LIST);
    if (ciph && A.SSL_CTX_set_cipher_list && A.SSL_CTX_set_cipher_list(ctx, ciph) != 1) goto fail_ctx;
    if (A.SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) goto fail_ctx;
    if (A.SSL_CTX_use_PrivateKey_file(ctx, key, TLS_FILETYPE_PEM) != 1) goto fail_ctx;
    if (A.SSL_CTX_check_private_key && A.SSL_CTX_check_private_key(ctx) != 1) goto fail_ctx;
    if (ctx_verify_client(ctx) != 0) goto fail_ctx;

    h->ctx = ctx;
    h->ssl = A.SSL_new(ctx);
    if (!h->ssl || A.SSL_set_fd(h->ssl, (int)fd) != 1) { *state = h; hs_free(state); return -1; }
    *state = h;
    return 0;

fail_ctx:
    A.SSL_CTX_free(ctx);
    free(h);
    return -1;
}

int q_tls_server_handshake(ray_sock_t fd, void** state) {
    if (!*state && hs_begin(fd, state) != 0) { hs_free(state); return -1; }
    tls_hs_t* h = (tls_hs_t*)*state;

    int rc = A.SSL_accept(h->ssl);
    if (rc == 1) {
        if (sess_attach(fd, h->ctx, h->ssl) != 0) { hs_free(state); return -1; }
        free(h); *state = NULL;                 /* the session owns ctx/ssl now */
        return 1;
    }
    int e = A.SSL_get_error(h->ssl, rc);
    if (e == TLS_ERROR_WANT_READ || e == TLS_ERROR_WANT_WRITE) return 0;
    hs_free(state);
    return -1;
}

void q_tls_server_handshake_abort(void** state) { hs_free(state); }

int q_tls_server_sniff(ray_sock_t fd) {
    uint8_t b = 0;
    int64_t n = ray_sock_peek(fd, &b, 1);
    if (n < 0) return -1;                       /* EAGAIN — nothing readable yet */
    if (n == 0) return 0;                       /* peer closed: let plain see EOF */
    return b == TLS_RECORD_HANDSHAKE;
}

static int64_t isym(const char* s) {
    return ray_sym_intern(s ? s : "", s ? strlen(s) : 0);
}

ray_t* q_tls_info(void) {
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, CFG_N + 1);
    ray_t* vals = ray_sym_vec_new(RAY_SYM_W64, CFG_N + 1);
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) goto oom;

    int64_t k = isym("SSLEAY_VERSION"), v = isym(tls_version_string());
    keys = ray_vec_append(keys, &k);
    vals = ray_vec_append(vals, &v);
    for (int i = 0; i < CFG_N; i++) {
        k = isym(CFG_NAMES[i]); v = isym(cfg_get(i));
        keys = ray_vec_append(keys, &k);
        vals = ray_vec_append(vals, &v);
        if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) goto oom;
    }
    return ray_dict_new(keys, vals);
oom:
    if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
    if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
    return q_err(QE_LIMIT);
}

/* `.z.e` per-connection status (ref/dotz.md:215). */
static void* bio_mem(void) {
    if (!A.BIO_new || !A.BIO_s_mem || !A.BIO_read || !A.BIO_free) return NULL;
    return A.BIO_new(A.BIO_s_mem());
}

static ray_t* bio_take(void* bio) {
    char buf[512];
    int  n = A.BIO_read(bio, buf, (int)sizeof buf);
    A.BIO_free(bio);
    return n > 0 ? ray_charv(buf, (int64_t)n) : NULL;
}

static ray_t* asn1_time_str(const void* t) {
    void* b = t ? bio_mem() : NULL;
    if (!b) return NULL;
    if (!A.ASN1_TIME_print || A.ASN1_TIME_print(b, t) != 1) { A.BIO_free(b); return NULL; }
    return bio_take(b);
}

static ray_t* serial_str(void* cert) {
    if (!A.X509_get_serialNumber || !A.i2a_ASN1_INTEGER) return NULL;
    void* b = bio_mem();
    if (!b) return NULL;
    if (A.i2a_ASN1_INTEGER(b, A.X509_get_serialNumber(cert)) < 0) { A.BIO_free(b); return NULL; }
    return bio_take(b);
}

static ray_t* name_str(const void* name) {
    char buf[512];
    if (!name || !A.X509_NAME_oneline || !A.X509_NAME_oneline(name, buf, (int)sizeof buf))
        return NULL;
    return ray_charv(buf, (int64_t)strlen(buf));   /* `.z.e` is q-visible: charv, not RAY_STR */
}

/* Appends iff `v` is present, so an absent field simply does not appear —
 * exactly how the doc treats a peer that presented no certificate. */
static void dict_put(ray_t** keys, ray_t** vals, const char* k, ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;
    int64_t id = isym(k);
    *keys = ray_vec_append(*keys, &id);
    *vals = ray_list_append(*vals, v);
    ray_release(v);
}

static ray_t* peer_cert_dict(void* ssl) {
    void* cert = A.SSL_get1_peer_certificate ? A.SSL_get1_peer_certificate(ssl)
               : A.SSL_get_peer_certificate  ? A.SSL_get_peer_certificate(ssl) : NULL;
    if (!cert) return NULL;
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 7);
    ray_t* vals = ray_list_new(7);
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) {
        if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
        if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
        if (A.X509_free) A.X509_free(cert);
        return NULL;
    }
    long vr = A.SSL_get_verify_result(ssl);
    dict_put(&keys, &vals, "SUBJECT", name_str(A.X509_get_subject_name ? A.X509_get_subject_name(cert) : NULL));
    dict_put(&keys, &vals, "ISSUER",  name_str(A.X509_get_issuer_name  ? A.X509_get_issuer_name(cert)  : NULL));
    dict_put(&keys, &vals, "SERIALNUMBER", serial_str(cert));
    dict_put(&keys, &vals, "NOTVALIDBEFORE", asn1_time_str(A.X509_get0_notBefore ? A.X509_get0_notBefore(cert) : NULL));
    dict_put(&keys, &vals, "NOTVALIDAFTER",  asn1_time_str(A.X509_get0_notAfter  ? A.X509_get0_notAfter(cert)  : NULL));
    dict_put(&keys, &vals, "VERIFIED",    ray_bool(vr == TLS_X509_V_OK));
    dict_put(&keys, &vals, "VERIFYERROR", ray_i64((int64_t)vr));
    if (A.X509_free) A.X509_free(cert);
    return ray_dict_new(keys, vals);
}

ray_t* q_tls_conn_info(ray_sock_t fd) {
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 3);
    ray_t* vals = ray_list_new(3);
    if (!keys || RAY_IS_ERR(keys) || !vals || RAY_IS_ERR(vals)) {
        if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
        if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
        return q_err(QE_LIMIT);
    }
    /* Only q_tls.c attaches overlays, so the ctx is always a tls_sess_t.
     * No overlay = not TLS enabled = the empty dict the doc specifies. */
    tls_sess_t* s = (fd == RAY_INVALID_SOCK) ? NULL : (tls_sess_t*)ray_sock_io_ctx(fd);
    if (s) {
        const void* cip  = A.SSL_get_current_cipher ? A.SSL_get_current_cipher(s->ssl) : NULL;
        const char* name = (cip && A.SSL_CIPHER_get_name) ? A.SSL_CIPHER_get_name(cip) : NULL;
        const char* prot = A.SSL_get_version ? A.SSL_get_version(s->ssl) : NULL;
        if (name) dict_put(&keys, &vals, "CIPHER",   ray_sym(isym(name)));
        if (prot) dict_put(&keys, &vals, "PROTOCOL", ray_sym(isym(prot)));
        dict_put(&keys, &vals, "CERT", peer_cert_dict(s->ssl));
    }
    return ray_dict_new(keys, vals);
}
