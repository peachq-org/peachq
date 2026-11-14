/* q_s3 — see q_s3.h.  Requests follow the S3 API reference (HeadObject, GetObject with Range, ListObjectsV2);
 * endpoint and addressing are as KX's objstor variables say. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L    /* gmtime_r */
#endif
#include "qlang/q_count.h"
#include "qlang/io/q_s3.h"
#include "qlang/base/q_err.h"
#include "qlang/q_env.h"              /* q_env_bind_native */
#include "qlang/eval/q_eval.h"        /* q_eval_apply_call_name — .s3.config[::] is qlib/s3.q's, bound by name */
#include "qlang/q_prim.h"             /* q_str_text_bytes */
#include "qlang/net/q_http_client.h"
#include "qlang/net/q_sigv4.h"
#include "lang/env.h"                 /* ray_fn_unary */
#include "lang/eval.h"                /* ray_eval_get_restricted, RAY_FN_NONE */
#include "table/sym.h"                /* ray_sym_intern_runtime, ray_sym_str */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define S3_CHUNK  (8ll << 20)         /* one GET's slice: well inside the client's 32 MiB response cap */
#define S3_EMPTY_SHA256 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

enum { K_ID, K_SECRET, K_TOKEN, K_REGION, K_PROFILE, K_CREDFILE, K_CONFFILE, K_ENDPOINT, K_PATHSTYLE, S3_NKEYS };
static const char* const KEYS[S3_NKEYS] = {
    "AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN", "AWS_REGION", "AWS_PROFILE",
    "AWS_SHARED_CREDENTIALS_FILE", "AWS_CONFIG_FILE", "KX_S3_ENDPOINT", "KX_S3_USE_PATH_REQUEST_STYLE",
};

static char* g_cfg[S3_NKEYS];
static int   g_cfg_set;

static char* dupn(const char* p, size_t n) {
    char* s = malloc(n + 1);
    if (s) { memcpy(s, p, n); s[n] = '\0'; }
    return s;
}

static void cfg_clear(char** v) {
    for (int i = 0; i < S3_NKEYS; i++) {
        if (v[i]) { memset(v[i], 0, strlen(v[i])); free(v[i]); }
        v[i] = NULL;
    }
}

/* The first access with nothing configured applies `.s3.config[::]` (qlib/s3.q) by name.  Its own reads are local
 * files, but a shared file named by an s3:// path would come back here: that inner access finds it under way and goes
 * anonymous rather than recursing. */
static ray_t* cfg_ensure(void) {
    static int busy;
    if (g_cfg_set || busy) return NULL;
    busy = 1;
    ray_t* none = RAY_NULL_OBJ;
    ray_t* r = q_eval_apply_call_name(".s3.config", 10, &none, 1);
    busy = 0;
    if (!r || RAY_IS_ERR(r)) return r ? r : q_err(QE_OOM);
    ray_release(r);
    return NULL;
}

/* ---- a request ---------------------------------------------------------- */

typedef struct {
    char        bucket[64];
    const char* key;
    size_t      klen;
    const char* url;              /* the path as given, for error text */
    size_t      ulen;
} s3_loc_t;

int q_s3_is(const char* p, size_t n) {
    return p && ((n >= 5 && memcmp(p, "s3://", 5) == 0) || (n >= 6 && (memcmp(p, "s3a://", 6) == 0 ||
                                                                      memcmp(p, "s3n://", 6) == 0)));
}

static ray_t* s3_loc(ray_t* pathstr, s3_loc_t* l) {
    const char* p = ray_str_ptr(pathstr);
    size_t n = ray_str_len(pathstr), at = p[2] == ':' ? 5 : 6;
    size_t b = at;
    while (b < n && p[b] != '/') b++;
    if (b == at || b - at >= sizeof l->bucket) return q_err(QE_DOMAIN);
    memcpy(l->bucket, p + at, b - at);
    l->bucket[b - at] = '\0';
    l->key = b < n ? p + b + 1 : p + n;
    l->klen = b < n ? n - b - 1 : 0;
    l->url = p;
    l->ulen = n;
    return NULL;
}

/* the text between `open` and `close` at or after *at within [p, p+n); advances *at past it */
static int xml_next(const char* p, size_t n, size_t* at, const char* open, const char* close, const char** v, size_t* vn) {
    size_t on = strlen(open), cn = strlen(close);
    for (size_t i = *at; i + on <= n; i++) {
        if (memcmp(p + i, open, on) != 0) continue;
        for (size_t j = i + on; j + cn <= n; j++)
            if (memcmp(p + j, close, cn) == 0) { *v = p + i + on; *vn = j - i - on; *at = j + cn; return 1; }
        return 0;
    }
    return 0;
}

/* XML's five predefined entities, decoded in place; the new length */
static size_t xml_unescape(char* s, size_t n) {
    static const struct { const char* e; char c; } E[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        size_t k = 0;
        while (s[i] == '&' && k < 5 && (n - i < strlen(E[k].e) || memcmp(s + i, E[k].e, strlen(E[k].e)) != 0)) k++;
        if (s[i] == '&' && k < 5) { s[o++] = E[k].c; i += strlen(E[k].e); }
        else s[o++] = s[i++];
    }
    return o;
}

/* The error a failed answer means: 403 'access, anything else 'io, the text naming the object and S3's own code, or
 * the status where there is no body to carry one (a HEAD). */
static ray_t* s3_fail(const s3_loc_t* l, const q_http_resp_t* r) {
    const char* c = NULL;
    size_t cn = 0, at = 0;
    char code[64];
    if (xml_next(r->body, r->body_len, &at, "<Code>", "</Code>", &c, &cn) && cn < sizeof code) {
        memcpy(code, c, cn);
        code[cn] = '\0';
    } else snprintf(code, sizeof code, "HTTP %d", r->status);
    ray_t* e = q_err(r->status == 403 ? QE_ACCESS : QE_IO);
    char buf[1400];
    int m = snprintf(buf, sizeof buf, "%s. %.*s: %s", q_err_class(e), (int)l->ulen, l->url, code);
    ray_t* t = m > 0 ? ray_charv(buf, m < (int)sizeof buf ? m : (int)sizeof buf - 1) : NULL;
    if (t && !RAY_IS_ERR(t)) q_err_put(t);
    return e;
}

static int hdr_line(char* out, size_t cap, size_t* o, const char* name, const char* value) {
    int w = snprintf(out + *o, cap - *o, "%s: %s\r\n", name, value);
    if (w < 0 || (size_t)w >= cap - *o) return -1;
    *o += (size_t)w;
    return 0;
}

/* The request for one region: its URL, and its headers signed when a key id and secret are configured. */
static ray_t* s3_request(const s3_loc_t* l, const char* method, const char* region, const char* query, int64_t roff,
                         int64_t rlen, q_http_url_t* u, char* hdrs, size_t cap) {
    char path[1100], url[1400], range[64] = "";
    const char* ep = g_cfg[K_ENDPOINT];
    int path_style = ep || (g_cfg[K_PATHSTYLE] && strcmp(g_cfg[K_PATHSTYLE], "1") == 0) || strchr(l->bucket, '.');
    int o = path_style ? snprintf(path, sizeof path, "/%s/", l->bucket) : snprintf(path, sizeof path, "/");
    if (q_sigv4_uri_encode(l->key, query ? 0 : l->klen, 1, path + o, sizeof path - (size_t)o) < 0) return q_err(QE_LIMIT);
    int w;
    if (ep) {
        size_t en = strlen(ep);
        while (en && ep[en - 1] == '/') en--;
        int bare = !q_http_client_scheme_is(ep, en);
        w = snprintf(url, sizeof url, "%s%.*s%s%s%s", bare ? "https://" : "", (int)en, ep, path, query ? "?" : "",
                     query ? query : "");
    } else {
        char host[160];
        snprintf(host, sizeof host, "%s%ss3.%s.amazonaws.com", path_style ? "" : l->bucket, path_style ? "" : ".", region);
        w = snprintf(url, sizeof url, "https://%s%s%s%s", host, path, query ? "?" : "", query ? query : "");
    }
    if (w < 0 || (size_t)w >= sizeof url || q_http_client_url_parse(url, (size_t)w, u) != 0) return q_err(QE_DOMAIN);
    char host[300];
    q_http_client_host(u, host, sizeof host);
    size_t at = 0;
    if (roff >= 0) snprintf(range, sizeof range, "bytes=%lld-%lld", (long long)roff, (long long)(roff + rlen - 1));
    if (hdr_line(hdrs, cap, &at, "Host", host) || (range[0] && hdr_line(hdrs, cap, &at, "Range", range)))
        return q_err(QE_LIMIT);
    if (!g_cfg[K_ID] || !g_cfg[K_SECRET]) return NULL;
    char amz[20], canon[4096], signed_h[200], auth[600];
    time_t now = time(NULL);
    struct tm tm;
    if (!gmtime_r(&now, &tm) || !strftime(amz, sizeof amz, "%Y%m%dT%H%M%SZ", &tm)) return q_err(QE_OS);
    q_sigv4_hdr_t h[5] = { { "host", host }, { "x-amz-content-sha256", S3_EMPTY_SHA256 }, { "x-amz-date", amz } };
    size_t nh = 3;
    if (range[0]) h[nh++] = (q_sigv4_hdr_t){ "range", range };
    if (g_cfg[K_TOKEN]) h[nh++] = (q_sigv4_hdr_t){ "x-amz-security-token", g_cfg[K_TOKEN] };
    const char* target = u->path;
    char uri[1100];
    size_t un = strcspn(target, "?");
    if (un >= sizeof uri) return q_err(QE_LIMIT);
    memcpy(uri, target, un);
    uri[un] = '\0';
    if (q_sigv4_canonical(method, uri, target[un] ? target + un + 1 : "", h, nh, S3_EMPTY_SHA256, canon, sizeof canon,
                          signed_h, sizeof signed_h) ||
        q_sigv4_authorization(g_cfg[K_ID], g_cfg[K_SECRET], region, "s3", amz, canon, signed_h, auth, sizeof auth))
        return q_err(QE_NYI);
    int bad = hdr_line(hdrs, cap, &at, "x-amz-content-sha256", S3_EMPTY_SHA256) ||
              hdr_line(hdrs, cap, &at, "x-amz-date", amz) ||
              (g_cfg[K_TOKEN] && hdr_line(hdrs, cap, &at, "x-amz-security-token", g_cfg[K_TOKEN])) ||
              hdr_line(hdrs, cap, &at, "Authorization", auth);
    memset(auth, 0, sizeof auth);
    return bad ? q_err(QE_LIMIT) : NULL;
}

/* One request for the object (query NULL) or the bucket's listing.  A bucket in another region than the one asked
 * answers 301/400 naming its own in x-amz-bucket-region, and the request is made once more there. */
static ray_t* s3_send(const s3_loc_t* l, const char* method, const char* query, int64_t roff, int64_t rlen,
                      q_http_resp_t* r) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    ray_t* bad = cfg_ensure();
    if (bad) return bad;
    char region[64];
    snprintf(region, sizeof region, "%s", g_cfg[K_REGION] ? g_cfg[K_REGION] : "us-east-1");
    for (int tries = 0;; tries++) {
        q_http_url_t u;
        char hdrs[2048];
        bad = s3_request(l, method, region, query, roff, rlen, &u, hdrs, sizeof hdrs);
        if (!bad) bad = q_http_client_fetch(&u, method, hdrs, r);
        memset(hdrs, 0, sizeof hdrs);
        if (bad) return bad;
        q_http_span_t br = q_http_client_resp_header(r, "x-amz-bucket-region");
        if (tries || (r->status != 301 && r->status != 400) || !br.p || br.n >= sizeof region ||
            (strlen(region) == br.n && memcmp(region, br.p, br.n) == 0))
            return NULL;
        memcpy(region, br.p, br.n);
        region[br.n] = '\0';
        q_http_client_resp_free(r);
    }
}

/* the decimal that ends a header value (all of Content-Length, the T of a Content-Range `bytes a-b/T`); -1 when the
 * header is absent or that is no number */
static int64_t s3_num(const q_http_resp_t* r, const char* name) {
    q_http_span_t h = q_http_client_resp_header(r, name);
    size_t i = h.n;
    while (i && h.p[i - 1] >= '0' && h.p[i - 1] <= '9') i--;
    if (!h.p || i == h.n) return -1;
    int64_t t = 0;
    for (; i < h.n; i++) {
        if (t > (INT64_MAX - (h.p[i] - '0')) / 10) return -1;
        t = t * 10 + (h.p[i] - '0');
    }
    return t;
}

/* the object's size from a HEAD's Content-Length; *status the answer's */
static ray_t* s3_size(const s3_loc_t* l, int* status) {
    q_http_resp_t r;
    ray_t* bad = s3_send(l, "HEAD", NULL, -1, 0, &r);
    if (bad) return bad;
    int64_t n = s3_num(&r, "Content-Length");
    *status = r.status;
    ray_t* out = r.status == 200 && n >= 0 ? ray_i64(n) : r.status == 200 ? q_err(QE_IO) : s3_fail(l, &r);
    q_http_client_resp_free(&r);
    return out;
}

ray_t* q_s3_size(ray_t* pathstr) {
    s3_loc_t l;
    int status;
    ray_t* bad = s3_loc(pathstr, &l);
    return bad ? bad : s3_size(&l, &status);
}

ray_t* q_s3_read(ray_t* pathstr, int64_t off, int64_t want) {
    s3_loc_t l;
    ray_t* bad = s3_loc(pathstr, &l);
    if (bad) return bad;
    if (off < 0) off = 0;
    if (off > INT64_MAX - S3_CHUNK) return ray_vec_from_raw(RAY_BYTE_ONLY, NULL, 0);   /* past any object's end */
    uint8_t* buf = NULL;
    int64_t got = 0;
    for (int done = 0; !done && !bad; ) {
        int64_t ask = want < 0 || want - got > S3_CHUNK ? S3_CHUNK : want - got;
        q_http_resp_t r;
        if ((bad = s3_send(&l, "GET", NULL, off + got, ask > 0 ? ask : 1, &r))) break;
        const char* src = r.body;
        int64_t n = (int64_t)r.body_len, t = s3_num(&r, "Content-Range");
        if (r.status == 200) {
            int64_t at = off + got;
            n = at < n ? n - at : 0;
            src += at < (int64_t)r.body_len ? at : (int64_t)r.body_len;
            done = 1;
        } else if (r.status == 416) n = 0, done = 1;
        else if (r.status != 206) bad = s3_fail(&l, &r);
        if (!bad && want >= 0 && n > want - got) n = want - got;
        uint8_t* nb = !bad && n ? realloc(buf, (size_t)(got + n)) : NULL;
        if (nb) {
            buf = nb;
            memcpy(buf + got, src, (size_t)n);
            got += n;
        } else if (!bad && n) bad = q_err(QE_OOM);
        done |= n < ask || (want >= 0 && got >= want) || (t >= 0 && off + got >= t);
        q_http_client_resp_free(&r);
    }
    ray_t* out = bad ? bad : ray_vec_from_raw(RAY_BYTE_ONLY, buf, got);
    free(buf);
    return out;
}

typedef struct { char** v; size_t n, cap; } names_t;

/* the XML text of a key or a common prefix, decoded, less the listing's prefix and a trailing '/'; 0, or -1 on OOM */
static int names_add(names_t* s, const char* p, size_t n, const char* prefix) {
    char* d = dupn(p, n);
    if (!d) return -1;
    size_t dn = xml_unescape(d, n), pn = strlen(prefix);
    if (dn && d[dn - 1] == '/') dn--;
    if (dn <= pn || memcmp(d, prefix, pn) != 0) { free(d); return 0; }
    memmove(d, d + pn, dn - pn);
    d[dn - pn] = '\0';
    if (s->n == s->cap) {
        size_t c = s->cap ? 2 * s->cap : 64;
        char** nv = realloc(s->v, c * sizeof *nv);
        if (!nv) { free(d); return -1; }
        s->v = nv;
        s->cap = c;
    }
    s->v[s->n++] = d;
    return 0;
}

static int name_cmp(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* One ListObjectsV2 page's names under `prefix` into s; *token the next page's, "" when it was the last. */
static ray_t* s3_list_page(const s3_loc_t* l, const char* prefix, char* token, size_t tcap, names_t* s) {
    char ep[1100], et[1100], query[2400];
    if (q_sigv4_uri_encode(prefix, strlen(prefix), 0, ep, sizeof ep) < 0 ||
        q_sigv4_uri_encode(token, strlen(token), 0, et, sizeof et) < 0)
        return q_err(QE_LIMIT);
    snprintf(query, sizeof query, "list-type=2&delimiter=%%2F&prefix=%s%s%s", ep, *token ? "&continuation-token=" : "",
             et);
    q_http_resp_t r;
    ray_t* bad = s3_send(l, "GET", query, -1, 0, &r);
    if (bad) return bad;
    if (r.status != 200) { bad = s3_fail(l, &r); q_http_client_resp_free(&r); return bad; }
    size_t at = 0, in;
    const char *e, *v;
    size_t en, vn;
    while (!bad && xml_next(r.body, r.body_len, &at, "<Contents>", "</Contents>", &e, &en))
        if ((in = 0, xml_next(e, en, &in, "<Key>", "</Key>", &v, &vn)) && names_add(s, v, vn, prefix))
            bad = q_err(QE_OOM);
    at = 0;
    while (!bad && xml_next(r.body, r.body_len, &at, "<CommonPrefixes>", "</CommonPrefixes>", &e, &en))
        if ((in = 0, xml_next(e, en, &in, "<Prefix>", "</Prefix>", &v, &vn)) && names_add(s, v, vn, prefix))
            bad = q_err(QE_OOM);
    at = 0;
    int more = xml_next(r.body, r.body_len, &at, "<IsTruncated>", "</IsTruncated>", &v, &vn) && vn == 4 &&
               memcmp(v, "true", 4) == 0;
    at = 0;
    token[0] = '\0';
    if (more && xml_next(r.body, r.body_len, &at, "<NextContinuationToken>", "</NextContinuationToken>", &v, &vn)) {
        if (vn >= tcap) bad = q_err(QE_LIMIT);
        else { memcpy(token, v, vn); token[xml_unescape(token, vn)] = '\0'; }
    }
    q_http_client_resp_free(&r);
    return bad;
}

ray_t* q_s3_list(ray_t* pathstr) {
    s3_loc_t l;
    ray_t* bad = s3_loc(pathstr, &l);
    if (bad) return bad;
    char prefix[1030], token[1030] = "";
    if (l.klen + 2 > sizeof prefix) return q_err(QE_LIMIT);
    memcpy(prefix, l.key, l.klen);
    size_t pn = l.klen;
    if (pn && prefix[pn - 1] != '/') prefix[pn++] = '/';
    prefix[pn] = '\0';
    names_t s = { 0 };
    do bad = s3_list_page(&l, prefix, token, sizeof token, &s);
    while (!bad && token[0]);
    ray_t* out = bad;
    if (!bad && !s.n && l.klen) {
        int status = 0;
        ray_t* sz = s3_size(&l, &status);
        out = !RAY_IS_ERR(sz) ? NULL : status == 404 ? ray_list_new(1) : sz;
        if (!RAY_IS_ERR(sz)) ray_release(sz);
        else if (out != sz) { ray_error_free(sz); q_err_drop(); }
    } else if (!bad) {
        qsort(s.v, s.n, sizeof *s.v, name_cmp);
        out = ray_sym_vec_new(RAY_SYM_W64, s.n ? (int64_t)s.n : 1);
        for (size_t i = 0; i < s.n && out && !RAY_IS_ERR(out); i++) {
            if (i && strcmp(s.v[i], s.v[i - 1]) == 0) continue;
            int64_t id = ray_sym_intern_runtime(s.v[i], strlen(s.v[i]));
            out = ray_vec_append(out, &id);
        }
        if (!out) out = q_err(QE_OOM);
    }
    for (size_t i = 0; i < s.n; i++) free(s.v[i]);
    free(s.v);
    return out;
}

/* ---- the .s3 natives ---------------------------------------------------- */

/* the settings in v as a dict in KEYS order, the secret and the token shown as "***" */
static ray_t* cfg_dict(char** v) {
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, S3_NKEYS);
    ray_t* d = ray_list_new(S3_NKEYS);
    for (int i = 0; i < S3_NKEYS && !RAY_IS_ERR(k) && !RAY_IS_ERR(d); i++) {
        if (!v[i]) continue;
        int64_t id = ray_sym_intern_runtime(KEYS[i], strlen(KEYS[i]));
        const char* t = i == K_SECRET || i == K_TOKEN ? "***" : v[i];
        ray_t* s = ray_charv(t, (int64_t)strlen(t));
        if (RAY_IS_ERR(s)) { ray_release(k); ray_release(d); return s; }
        k = ray_vec_append(k, &id);
        d = ray_list_append(d, s);
        ray_release(s);
    }
    if (RAY_IS_ERR(k) || RAY_IS_ERR(d)) {
        if (!RAY_IS_ERR(k)) ray_release(k);
        if (!RAY_IS_ERR(d)) ray_release(d);
        return q_err(QE_OOM);
    }
    return ray_dict_new(k, d);
}

/* `.s3.i.config d` — d keyed by the setting names, each value a string; it replaces the whole configuration, kept
 * here and never in a q value, and answers it masked */
static ray_t* s3_config_fn(ray_t* x) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    if (!x || x->type != RAY_DICT) return q_err(QE_TYPE);
    ray_t* k = ray_dict_keys(x);
    ray_t* d = ray_dict_vals(x);
    int64_t n = q_count(k);
    if (n && (k->type != RAY_SYM || d->type != RAY_LIST)) return q_err(QE_TYPE);
    char* v[S3_NKEYS] = { 0 };
    ray_t* bad = NULL;
    for (int64_t i = 0; i < n && !bad; i++) {
        ray_t* nm = ray_sym_str(ray_vec_get_sym_id(k, i));
        int j = 0;
        while (nm && j < S3_NKEYS && (strlen(KEYS[j]) != ray_str_len(nm) || memcmp(KEYS[j], ray_str_ptr(nm), ray_str_len(nm))))
            j++;
        const char* p;
        int64_t pn;
        if (j == S3_NKEYS) bad = q_err(QE_OPTION);
        else if (!q_str_text_bytes(ray_list_get(d, i), &p, &pn)) bad = q_err(QE_TYPE);
        else {
            free(v[j]);
            v[j] = pn ? dupn(p, (size_t)pn) : NULL;
            if (pn && !v[j]) bad = q_err(QE_OOM);
        }
    }
    if (bad) { cfg_clear(v); return bad; }
    cfg_clear(g_cfg);
    memcpy(g_cfg, v, sizeof v);
    g_cfg_set = 1;
    return cfg_dict(g_cfg);
}

void q_s3_register(void) {
    ray_t* c = ray_fn_unary(".s3.i.config", RAY_FN_NONE, s3_config_fn);
    q_env_bind_native(".s3.i.config", c, 1);
    ray_release(c);
}
