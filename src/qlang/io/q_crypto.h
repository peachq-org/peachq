/* q_crypto — bind the `.crypto.i.*` natives (digests, HMAC, random bytes over the platform's OpenSSL).
 * Called by `.pq.load_natives`. */
#ifndef QLANG_IO_Q_CRYPTO_H
#define QLANG_IO_Q_CRYPTO_H

#include <stddef.h>

void q_crypto_register(void);

/* SHA-256 of n bytes, and HMAC-SHA-256 of them under a key, into out[32] for C callers (the SigV4 signer): 0, or -1
 * when this process has no libcrypto. */
int q_crypto_sha256(const void* p, size_t n, unsigned char out[32]);
int q_crypto_hmac_sha256(const void* key, size_t klen, const void* p, size_t n, unsigned char out[32]);

#endif /* QLANG_IO_Q_CRYPTO_H */
