/* q_sigv4 — AWS Signature Version 4 over the platform's libcrypto (q_crypto), for the s3:// byte reader.  The
 * algorithm is AWS's published one (S3 API reference, "Signature Calculations for the Authorization Header"); its
 * worked GET example is the C unit test. */
#ifndef QLANG_NET_Q_SIGV4_H
#define QLANG_NET_Q_SIGV4_H

#include <stddef.h>

/* AWS UriEncode of n bytes into out[cap] (NUL-terminated): every byte but A-Z a-z 0-9 - _ . ~ becomes %XX, and `/`
 * too unless keep_slash (an object key's path keeps it, a query value does not).  The length, or -1 when it will
 * not fit. */
int q_sigv4_uri_encode(const char* s, size_t n, int keep_slash, char* out, size_t cap);

/* One signed header: a lowercase name and its value as sent. */
typedef struct { const char* name; const char* value; } q_sigv4_hdr_t;

/* The canonical request into out[cap]: METHOD, the encoded URI, the query's `k=v` pairs sorted by key, the headers
 * sorted by name with their values trimmed, the signed-header list, then the payload hash.  `query` is the encoded
 * query as sent ("" for none); h is reordered.  The signed-header list goes to sh[shcap].  0, or -1 when either will
 * not fit. */
int q_sigv4_canonical(const char* method, const char* uri, const char* query, q_sigv4_hdr_t* h, size_t nh,
                      const char* payload_hash, char* out, size_t cap, char* sh, size_t shcap);

/* The Authorization header's value for a canonical request, signed for `region`/`service` at `amzdate`
 * (YYYYMMDD'T'HHMMSS'Z').  0, or -1 when it will not fit or there is no libcrypto. */
int q_sigv4_authorization(const char* akid, const char* secret, const char* region, const char* service,
                          const char* amzdate, const char* canonical, const char* signed_headers,
                          char* out, size_t cap);

#endif /* QLANG_NET_Q_SIGV4_H */
