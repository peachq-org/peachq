/* q_crypto — bind the `.crypto.i.*` natives (digests, HMAC, random bytes over the platform's OpenSSL).
 * Called by `.pq.load_natives`. */
#ifndef QLANG_IO_Q_CRYPTO_H
#define QLANG_IO_Q_CRYPTO_H

void q_crypto_register(void);

#endif /* QLANG_IO_Q_CRYPTO_H */
