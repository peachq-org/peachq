/* ops/q_hash.c — the `.hash.i.hash[algo;seed;x]` native: the vendored xxHash, rapidhash and MurmurHash3 over the
 * bytes of a string, byte vector or symbol, answering what DuckDB's hashfuncs extension answers for the same
 * bytes and seed (the result law is in qlib/hash.q).  One native; an algorithm is a table row. */
#include "qlang/q_count.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"   /* q_type_strict_i64 — the seed gate */
#include "qlang/ops/q_hash.h"
#include "qlang/q_env.h"         /* q_env_bind_native */
#include "qlang/q_prim.h"        /* q_str_subject_bytes / q_str_text_or_bytevec — the input readers; q_str_subject_map */
#include "lang/env.h"            /* ray_fn_vary */
#include "lang/eval.h"           /* RAY_FN_NONE */
#include "MurmurHash3.h"
#include "rapidhash.h"
#include "xxhash.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef void (*hash_compute)(const void* p, size_t n, uint64_t seed, uint64_t out[2]);

static void hash_xxh32(const void* p, size_t n, uint64_t seed, uint64_t out[2])    { out[0] = XXH32(p, n, (uint32_t)seed); }
static void hash_xxh64(const void* p, size_t n, uint64_t seed, uint64_t out[2])    { out[0] = XXH64(p, n, seed); }
static void hash_xxh3_64(const void* p, size_t n, uint64_t seed, uint64_t out[2])  { out[0] = XXH3_64bits_withSeed(p, n, seed); }
static void hash_rapidhash(const void* p, size_t n, uint64_t seed, uint64_t out[2]) { out[0] = rapidhash_withSeed(p, n, seed); }
static void hash_xxh3_128(const void* p, size_t n, uint64_t seed, uint64_t out[2]) {
    XXH128_canonical_t c;
    XXH128_canonicalFromHash(&c, XXH3_128bits_withSeed(p, n, seed));
    memcpy(out, c.digest, sizeof c.digest);
}
static void hash_murmur3_32(const void* p, size_t n, uint64_t seed, uint64_t out[2]) {
    uint32_t h;
    MurmurHash3_x86_32(p, (int)n, (uint32_t)seed, &h);
    out[0] = h;
}
static void hash_murmur3_128(const void* p, size_t n, uint64_t seed, uint64_t out[2])     { MurmurHash3_x86_128(p, (int)n, (uint32_t)seed, out); }
static void hash_murmur3_x64_128(const void* p, size_t n, uint64_t seed, uint64_t out[2]) { MurmurHash3_x64_128(p, (int)n, (uint32_t)seed, out); }

/* murmur: the reference takes `int len` and reads whole words in place, so it gets a bounded, aligned input */
static const struct { const char* name; int seed32; int width; int murmur; hash_compute fn; } HASH_ALGOS[] = {
    { "xxh32",           1,  4, 0, hash_xxh32 },
    { "xxh64",           0,  8, 0, hash_xxh64 },
    { "xxh3_64",         0,  8, 0, hash_xxh3_64 },
    { "xxh3_128",        0, 16, 0, hash_xxh3_128 },
    { "rapidhash",       0,  8, 0, hash_rapidhash },
    { "murmur3_32",      1,  4, 1, hash_murmur3_32 },
    { "murmur3_128",     1, 16, 1, hash_murmur3_128 },
    { "murmur3_x64_128", 1, 16, 1, hash_murmur3_x64_128 },
};

typedef struct { size_t algo; uint64_t seed; } hash_ctx;

/* one subject: a string, char atom, symbol atom or byte vector */
static ray_t* hash_one(ray_t* x, void* vctx) {
    const hash_ctx* c = vctx;
    const char* tp; int64_t tn;
    if (!q_str_subject_bytes(x, &tp, &tn) && !q_str_text_or_bytevec(x, &tp, &tn)) return q_err(QE_TYPE);
    const void* p = tp; size_t len = (size_t)tn;
    if (HASH_ALGOS[c->algo].murmur && len > INT_MAX) return q_err(QE_LIMIT);
    uint64_t stack[2]; void* heap = NULL;
    if (HASH_ALGOS[c->algo].murmur && ((uintptr_t)p & 7)) {    /* a short symbol's text sits in its header at an odd offset */
        void* a = len <= sizeof stack ? stack : (heap = malloc(len));
        if (!a) return q_err(QE_WSFULL);
        p = memcpy(a, p, len);
    }
    uint64_t out[2] = { 0, 0 };
    HASH_ALGOS[c->algo].fn(p, len, c->seed, out);
    free(heap);
    return HASH_ALGOS[c->algo].width == 16 ? ray_vec_from_raw(RAY_BYTE_ONLY, out, 16) : ray_i64((int64_t)out[0]);
}

/* [algo; seed; x] — x one subject, or a symbol vector / list of them mapped elementwise by the string boundary */
static ray_t* hash_fn(ray_t** args, int64_t n) {
    if (n != 3) return q_err(QE_RANK);
    const char* ap; int64_t an;
    if (!q_str_subject_bytes(args[0], &ap, &an)) return q_err(QE_TYPE);
    hash_ctx c = { 0, 0 };
    size_t nalgos = sizeof HASH_ALGOS / sizeof *HASH_ALGOS;
    while (c.algo < nalgos && (strlen(HASH_ALGOS[c.algo].name) != (size_t)an || memcmp(HASH_ALGOS[c.algo].name, ap, (size_t)an) != 0)) c.algo++;
    if (c.algo == nalgos) return q_err(QE_DOMAIN);
    int64_t seed;
    if (!q_type_strict_i64(args[1], &seed)) return q_err(QE_TYPE);
    if (HASH_ALGOS[c.algo].seed32 && (seed < 0 || seed > UINT32_MAX)) return q_err(QE_DOMAIN);
    c.seed = (uint64_t)seed;
    return q_str_subject_map(args[2], hash_one, &c, HASH_ALGOS[c.algo].width == 16 ? 0 : RAY_I64);
}

void q_hash_register(void) {
    ray_t* obj = ray_fn_vary(".hash.i.hash", RAY_FN_NONE, hash_fn);
    q_env_bind_native(".hash.i.hash", obj, 3);
    ray_release(obj);
}
