/* io/q_crypto.c — the `.crypto.i.*` natives: digests, HMAC and random bytes from the platform's OpenSSL (libcrypto
 * through q_openssl, never linked), answering what DuckDB's crypto extension answers for the same name and bytes;
 * ALGOS below is its name map, the output law is in qlib/crypto.q. */
#include "qlang/q_count.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"    /* q_type_i64_or_err — the randbytes count gate */
#include "qlang/io/q_crypto.h"
#include "qlang/io/q_dl.h"        /* q_dl_sym; q_dl_unavailable — a static build cannot dlopen OpenSSL */
#include "qlang/io/q_openssl.h"
#include "qlang/q_env.h"          /* q_env_bind_native */
#include "qlang/q_prim.h"         /* q_str_subject_bytes / q_str_text_or_bytevec — the input readers; q_str_subject_map */
#include "lang/env.h"             /* ray_fn_vary */
#include "lang/eval.h"            /* RAY_FN_NONE */
#include <limits.h>
#include <string.h>

#define CRYPTO_MAX_MD      64              /* EVP_MAX_MD_SIZE */
#define CRYPTO_MAX_RANDOM  4294967295LL    /* the extension's crypto_random_bytes ceiling */

typedef struct {
    const void*    (*EVP_get_digestbyname)(const char*);
    int            (*EVP_Digest)(const void*, size_t, unsigned char*, unsigned int*, const void*, void*);
    unsigned char* (*HMAC)(const void*, const void*, int, const unsigned char*, size_t, unsigned char*, unsigned int*);
    int            (*RAND_bytes)(unsigned char*, int);
    void           (*ERR_clear_error)(void);
} crypto_api_t;

static crypto_api_t A;
static int          g_loaded = 0;    /* 0 untried, 1 ready, -1 unavailable */

/* Via a void* slot: casting dlsym's result to a function pointer is not ISO C. */
#define SYM(h, f)  (*(void**)(&A.f) = q_dl_sym((h), #f))

static int crypto_load(void) {
    if (g_loaded) return g_loaded;
    g_loaded = -1;
    void* h = q_openssl_crypto();
    if (!h) return g_loaded;
    SYM(h, EVP_get_digestbyname); SYM(h, EVP_Digest); SYM(h, HMAC); SYM(h, RAND_bytes); SYM(h, ERR_clear_error);
    if (A.EVP_get_digestbyname && A.EVP_Digest && A.HMAC && A.RAND_bytes && A.ERR_clear_error) g_loaded = 1;
    return g_loaded;
}

static ray_t* crypto_unavailable(void) {
    const char* why = q_dl_unavailable();
    return why ? q_err_from_text(why, strlen(why)) : q_err(QE_NYI);
}

/* a failed call leaves an entry on OpenSSL's error queue that the TLS unit's SSL_get_error would misread */
static ray_t* crypto_failed(void) {
    A.ERR_clear_error();
    return q_err(QE_OS);
}

/* the extension's names over OpenSSL's: keccak* IS SHA3-* there (its own test says so), so here */
static struct { const char* name; const char* ossl; const void* md; } ALGOS[] = {
    { "md4", "MD4", NULL },           { "md5", "MD5", NULL },           { "sha1", "SHA1", NULL },
    { "sha2-224", "SHA224", NULL },   { "sha2-256", "SHA256", NULL },   { "sha2-384", "SHA384", NULL },
    { "sha2-512", "SHA512", NULL },   { "sha3-224", "SHA3-224", NULL }, { "sha3-256", "SHA3-256", NULL },
    { "sha3-384", "SHA3-384", NULL }, { "sha3-512", "SHA3-512", NULL }, { "keccak224", "SHA3-224", NULL },
    { "keccak256", "SHA3-256", NULL }, { "keccak384", "SHA3-384", NULL }, { "keccak512", "SHA3-512", NULL },
    { "blake2b-512", "BLAKE2b512", NULL }, { "blake2s-256", "BLAKE2s256", NULL },
};
#define NALGOS (sizeof ALGOS / sizeof *ALGOS)

/* a row's digest, proved by computing once: OpenSSL 3 names MD4 but keeps it in the legacy provider, and only a
 * digest tells; success is kept, failure is asked again */
static const void* crypto_md(size_t i) {
    if (ALGOS[i].md) return ALGOS[i].md;
    const void* md = A.EVP_get_digestbyname(ALGOS[i].ossl);
    unsigned char out[CRYPTO_MAX_MD]; unsigned int n;
    if (md && A.EVP_Digest("", 0, out, &n, md, NULL) == 1) return ALGOS[i].md = md;
    A.ERR_clear_error();
    return NULL;
}

/* the digest `algo` names: NULL and *md set, else the owned error ('type: not a name; 'domain: not one here) */
static ray_t* crypto_lookup(ray_t* algo, const void** md) {
    const char* p; int64_t n;
    if (!q_str_subject_bytes(algo, &p, &n)) return q_err(QE_TYPE);
    size_t i = 0;
    while (i < NALGOS && (strlen(ALGOS[i].name) != (size_t)n || memcmp(ALGOS[i].name, p, (size_t)n) != 0)) i++;
    if (i == NALGOS || !(*md = crypto_md(i))) return q_err(QE_DOMAIN);
    return NULL;
}

/* one subject: a string, char atom, symbol atom or byte vector */
static ray_t* crypto_hash_one(ray_t* x, void* md) {
    const char* p; int64_t len;
    if (!q_str_subject_bytes(x, &p, &len) && !q_str_text_or_bytevec(x, &p, &len)) return q_err(QE_TYPE);
    unsigned char out[CRYPTO_MAX_MD]; unsigned int olen;
    if (A.EVP_Digest(p, (size_t)len, out, &olen, md, NULL) != 1) return crypto_failed();
    return ray_vec_from_raw(RAY_BYTE_ONLY, out, olen);
}

/* [algo; x] — x one subject, or a symbol vector / list of them mapped elementwise by the string boundary */
static ray_t* crypto_hash_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    if (crypto_load() != 1) return crypto_unavailable();
    const void* md;
    ray_t* e = crypto_lookup(args[0], &md);
    if (e) return e;
    return q_str_subject_map(args[1], crypto_hash_one, (void*)md, 0);
}

typedef struct { const void* md; const char* key; int klen; } hmac_ctx;

static ray_t* crypto_hmac_one(ray_t* x, void* vctx) {
    const hmac_ctx* c = vctx;
    const char* p; int64_t len;
    if (!q_str_subject_bytes(x, &p, &len) && !q_str_text_or_bytevec(x, &p, &len)) return q_err(QE_TYPE);
    unsigned char out[CRYPTO_MAX_MD]; unsigned int olen;
    if (!A.HMAC(c->md, c->key, c->klen, (const unsigned char*)p, (size_t)len, out, &olen)) return crypto_failed();
    return ray_vec_from_raw(RAY_BYTE_ONLY, out, olen);
}

/* [algo; key; x] — the key is ONE subject; x maps as .crypto.i.hash's does */
static ray_t* crypto_hmac_fn(ray_t** args, int64_t n) {
    if (n != 3) return q_err(QE_RANK);
    if (crypto_load() != 1) return crypto_unavailable();
    hmac_ctx c;
    ray_t* e = crypto_lookup(args[0], &c.md);
    if (e) return e;
    int64_t klen;
    if (!q_str_subject_bytes(args[1], &c.key, &klen) && !q_str_text_or_bytevec(args[1], &c.key, &klen)) return q_err(QE_TYPE);
    if (klen > INT_MAX) return q_err(QE_LIMIT);    /* HMAC() counts its key in an int */
    c.klen = (int)klen;
    if (!klen) c.key = "";
    return q_str_subject_map(args[2], crypto_hmac_one, &c, 0);
}

/* [n] */
static ray_t* crypto_randbytes_fn(ray_t** args, int64_t argc) {
    if (argc != 1) return q_err(QE_RANK);
    if (crypto_load() != 1) return crypto_unavailable();
    int64_t n;
    ray_t* e = q_type_i64_or_err(args[0], &n, ".crypto.randbytes: n");
    if (e) return e;
    if (n < 1 || n > CRYPTO_MAX_RANDOM) return q_err(QE_DOMAIN);
    ray_t* out = ray_vec_new(RAY_BYTE_ONLY, n);
    if (RAY_IS_ERR(out)) return out;
    out->len = n;
    unsigned char* p = ray_data(out);
    for (int64_t done = 0; done < n; done += INT_MAX) {        /* RAND_bytes counts in an int */
        int chunk = n - done > INT_MAX ? INT_MAX : (int)(n - done);
        if (A.RAND_bytes(p + done, chunk) != 1) { ray_release(out); return crypto_failed(); }
    }
    return out;
}

/* [] — the names .crypto.hash accepts in this process, in table order */
static ray_t* crypto_algos_fn(ray_t** args, int64_t argc) {
    (void)args;
    if (argc != 1) return q_err(QE_RANK);
    if (crypto_load() != 1) return crypto_unavailable();
    ray_t* v = ray_sym_vec_new(RAY_SYM_W64, NALGOS);
    if (RAY_IS_ERR(v)) return v;
    for (size_t i = 0; i < NALGOS; i++) {
        if (!crypto_md(i)) continue;
        int64_t sym = ray_sym_intern_runtime(ALGOS[i].name, strlen(ALGOS[i].name));
        ray_t* r = ray_vec_append(v, &sym);
        if (!r || RAY_IS_ERR(r)) { ray_release(v); return r ? r : q_err(QE_WSFULL); }
        v = r;
    }
    return v;
}

static void crypto_bind(const char* name, int rank, ray_vary_fn fn) {
    ray_t* obj = ray_fn_vary(name, RAY_FN_NONE, fn);
    q_env_bind_native(name, obj, rank);
    ray_release(obj);
}

void q_crypto_register(void) {
    crypto_bind(".crypto.i.hash",      2, crypto_hash_fn);
    crypto_bind(".crypto.i.hmac",      3, crypto_hmac_fn);
    crypto_bind(".crypto.i.randbytes", 1, crypto_randbytes_fn);
    crypto_bind(".crypto.i.algos",     1, crypto_algos_fn);
}
