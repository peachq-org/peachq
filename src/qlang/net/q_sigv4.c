/* q_sigv4 — see q_sigv4.h. */
#include "qlang/q_count.h"
#include "qlang/net/q_sigv4.h"
#include "qlang/io/q_crypto.h"   /* SHA-256 and HMAC-SHA-256 from the platform's libcrypto */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIGV4_MAX_PAIRS 64

static void hex(const unsigned char* p, size_t n, char* out) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = hx[p[i] >> 4]; out[2 * i + 1] = hx[p[i] & 15]; }
    out[2 * n] = '\0';
}

int q_sigv4_uri_encode(const char* s, size_t n, int keep_slash, char* out, size_t cap) {
    static const char hx[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/');
        if (o + (plain ? 1 : 3) >= cap) return -1;
        if (plain) out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hx[c >> 4]; out[o++] = hx[c & 15]; }
    }
    if (o >= cap) return -1;
    out[o] = '\0';
    return (int)o;
}

typedef struct { const char* p; size_t n; } span_t;

static int pair_cmp(const void* a, const void* b) {
    const span_t* x = a;
    const span_t* y = b;
    size_t kx = 0, ky = 0;
    while (kx < x->n && x->p[kx] != '=') kx++;
    while (ky < y->n && y->p[ky] != '=') ky++;
    int c = memcmp(x->p, y->p, kx < ky ? kx : ky);
    if (c || kx != ky) return c ? c : kx < ky ? -1 : 1;
    size_t m = x->n < y->n ? x->n : y->n;
    c = memcmp(x->p, y->p, m);
    return c ? c : x->n < y->n ? -1 : x->n > y->n;
}

static int hdr_cmp(const void* a, const void* b) {
    return strcmp(((const q_sigv4_hdr_t*)a)->name, ((const q_sigv4_hdr_t*)b)->name);
}

/* appends to out at *o: 0, or -1 when cap is reached */
static int put(char* out, size_t cap, size_t* o, const char* p, size_t n) {
    if (*o + n >= cap) return -1;
    memcpy(out + *o, p, n);
    *o += n;
    out[*o] = '\0';
    return 0;
}

static int puts0(char* out, size_t cap, size_t* o, const char* s) { return put(out, cap, o, s, strlen(s)); }

int q_sigv4_canonical(const char* method, const char* uri, const char* query, q_sigv4_hdr_t* h, size_t nh,
                      const char* payload_hash, char* out, size_t cap, char* sh, size_t shcap) {
    span_t pairs[SIGV4_MAX_PAIRS];
    size_t np = 0;
    for (const char* q = query; *q; ) {
        const char* e = strchr(q, '&');
        size_t n = e ? (size_t)(e - q) : strlen(q);
        if (n) {
            if (np == SIGV4_MAX_PAIRS) return -1;
            pairs[np++] = (span_t){ q, n };
        }
        q += n + (e != NULL);
    }
    qsort(pairs, np, sizeof *pairs, pair_cmp);
    qsort(h, nh, sizeof *h, hdr_cmp);
    size_t o = 0, so = 0;
    if (puts0(out, cap, &o, method) || puts0(out, cap, &o, "\n") || puts0(out, cap, &o, uri) ||
        puts0(out, cap, &o, "\n"))
        return -1;
    for (size_t i = 0; i < np; i++) {
        int bare = !memchr(pairs[i].p, '=', pairs[i].n);
        if ((i && puts0(out, cap, &o, "&")) || put(out, cap, &o, pairs[i].p, pairs[i].n) ||
            (bare && puts0(out, cap, &o, "=")))
            return -1;
    }
    if (puts0(out, cap, &o, "\n")) return -1;
    sh[0] = '\0';
    for (size_t i = 0; i < nh; i++) {
        const char* v = h[i].value;
        size_t vn = strlen(v);
        while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
        while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;
        if (puts0(out, cap, &o, h[i].name) || puts0(out, cap, &o, ":") || put(out, cap, &o, v, vn) ||
            puts0(out, cap, &o, "\n") || (i && puts0(sh, shcap, &so, ";")) || puts0(sh, shcap, &so, h[i].name))
            return -1;
    }
    if (puts0(out, cap, &o, "\n") || puts0(out, cap, &o, sh) || puts0(out, cap, &o, "\n") ||
        puts0(out, cap, &o, payload_hash))
        return -1;
    return 0;
}

static int mac(const unsigned char* key, size_t klen, const char* msg, unsigned char out[32]) {
    return q_crypto_hmac_sha256(key, klen, msg, strlen(msg), out);
}

int q_sigv4_authorization(const char* akid, const char* secret, const char* region, const char* service,
                          const char* amzdate, const char* canonical, const char* signed_headers,
                          char* out, size_t cap) {
    char date[9], scope[160], sts[300], digest[65], sig[65];
    unsigned char h[32], k[32], k2[32];
    if (strlen(amzdate) != 16) return -1;
    memcpy(date, amzdate, 8);
    date[8] = '\0';
    int n = snprintf(scope, sizeof scope, "%s/%s/%s/aws4_request", date, region, service);
    if (n < 0 || (size_t)n >= sizeof scope || q_crypto_sha256(canonical, strlen(canonical), h)) return -1;
    hex(h, 32, digest);
    n = snprintf(sts, sizeof sts, "AWS4-HMAC-SHA256\n%s\n%s\n%s", amzdate, scope, digest);
    if (n < 0 || (size_t)n >= sizeof sts) return -1;
    size_t sl = strlen(secret);
    unsigned char* k0 = malloc(sl + 4);
    if (!k0) return -1;
    memcpy(k0, "AWS4", 4);
    memcpy(k0 + 4, secret, sl);
    int bad = mac(k0, sl + 4, date, k) || mac(k, 32, region, k2) || mac(k2, 32, service, k) ||
              mac(k, 32, "aws4_request", k2) || mac(k2, 32, sts, h);
    memset(k0, 0, sl + 4);
    free(k0);
    memset(k, 0, sizeof k);
    memset(k2, 0, sizeof k2);
    if (bad) return -1;
    hex(h, 32, sig);
    n = snprintf(out, cap, "AWS4-HMAC-SHA256 Credential=%s/%s,SignedHeaders=%s,Signature=%s",
                 akid, scope, signed_headers, sig);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
