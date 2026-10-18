/* q_openssl — the platform's OpenSSL libraries, found by soname and opened at most once per process (dlopen'd,
 * never linked, never unmapped), shared by the TLS unit and `.crypto`; symbols resolve with q_dl_sym. */
#ifndef QLANG_IO_Q_OPENSSL_H
#define QLANG_IO_Q_OPENSSL_H

/* libssl's handle, NULL when no soname opens. */
void* q_openssl_ssl(void);

/* The handle libcrypto's symbols resolve through: libcrypto, else libssl (whose dependency tree carries them on
 * POSIX); NULL when neither opens. */
void* q_openssl_crypto(void);

#endif /* QLANG_IO_Q_OPENSSL_H */
