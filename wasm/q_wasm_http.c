/* q_wasm_http — the wasm answer to q_http_client.c's one network exchange.  The request
 * text is parsed back into method/target/headers/body, handed to the host's
 * Module.peachqFetch (a synchronous XHR in the Worker, a child-process fetch in the node
 * smoke), and the answer is rebuilt as the raw HTTP/1.1 response a socket would have
 * read — so framing, slices, status policy and the raw handle stay in the shared C. */
#define _POSIX_C_SOURCE 200809L   /* strncasecmp */
#include "qlang/net/q_http_client.h"
#include "q_wasm_buf.h"
#include "picohttpparser.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <emscripten.h>

#define WASM_HTTP_MAX_HDRS 64

/* Status, or -1 when there is no hook or it threw (network failure, CORS refusal).
 * *head gets malloc'd "<reason>\n<response header lines>", *body the malloc'd bytes. */
EM_JS(int, host_fetch, (const char* method, const char* url, const char* hdrs,
                        const uint8_t* body, size_t body_len,
                        char** head, uint8_t** rbody, size_t* rbody_len), {
    if (typeof Module.peachqFetch !== "function") return -1;
    var r;
    try {
        r = Module.peachqFetch(UTF8ToString(method), UTF8ToString(url), UTF8ToString(hdrs),
                               HEAPU8.slice(body, body + body_len));
    } catch (e) {
        return -1;
    }
    if (!r || typeof r.status !== "number" || r.status < 100 || r.status > 999) return -1;
    var text = new TextEncoder().encode(String(r.statusText || "") + "\n" + String(r.headers || ""));
    var bytes = r.body instanceof Uint8Array ? r.body : new Uint8Array(r.body || 0);
    var hp = _malloc(text.length + 1);
    var bp = _malloc(bytes.length + 1);
    if (!hp || !bp) { _free(hp); _free(bp); return -1; }
    HEAPU8.set(text, hp);
    HEAPU8[hp + text.length] = 0;
    HEAPU8.set(bytes, bp);
    HEAPU32[head >> 2] = hp;
    HEAPU32[rbody >> 2] = bp;
    HEAPU32[rbody_len >> 2] = bytes.length;
    return r.status;
});

static int name_is(const char* p, size_t n, const char* name) {
    return strlen(name) == n && strncasecmp(p, name, n) == 0;
}

/* Framing belongs to whichever transport carries the bytes: the host's on the way out. */
static int framing_header(const char* p, size_t n) {
    return name_is(p, n, "host") || name_is(p, n, "connection") ||
           name_is(p, n, "content-length") || name_is(p, n, "transfer-encoding");
}

/* ...and ours on the way back: one Content-Length over the body as the host decoded it.
 * A HEAD keeps the length and coding it was told about — that is its whole answer. */
static int keep_response_header(const char* p, size_t n, int no_body) {
    if (name_is(p, n, "content-length") || name_is(p, n, "content-encoding")) return no_body;
    return !name_is(p, n, "transfer-encoding") && !name_is(p, n, "connection");
}

char* q_http_client_host_exchange(const q_http_url_t* u, const char* req, size_t req_len,
                                  const char* body, size_t body_len, int no_body,
                                  size_t* len, const char** err)
{
    const char *method, *target;
    size_t mlen, tlen, nh = WASM_HTTP_MAX_HDRS;
    int minor;
    struct phr_header h[WASM_HTTP_MAX_HDRS];
    int hl = phr_parse_request(req, req_len, &method, &mlen, &target, &tlen, &minor, h, &nh, 0);
    if (hl <= 0) { *err = "domain"; return NULL; }

    wbuf_t verb = {0}, url = {0}, hdrs = {0}, payload = {0}, out = {0};
    char* resp = NULL;
    char* head = NULL;
    uint8_t* rbody = NULL;
    size_t rbody_len = 0;
    char line[64];

    wbuf_put(&verb, method, mlen);
    if (tlen && target[0] != '/') wbuf_put(&url, target, tlen);   /* absolute-form target */
    else {
        char origin[300];
        snprintf(origin, sizeof origin, "%s://%s:%u", u->scheme ? "https" : "http", u->host, u->port);
        wbuf_str(&url, origin);
        wbuf_put(&url, target, tlen);
    }
    wbuf_str(&hdrs, "");
    for (size_t i = 0; i < nh; i++) {
        if (!h[i].name || framing_header(h[i].name, h[i].name_len)) continue;
        wbuf_put(&hdrs, h[i].name, h[i].name_len);
        wbuf_str(&hdrs, ": ");
        wbuf_put(&hdrs, h[i].value, h[i].value_len);
        wbuf_str(&hdrs, "\r\n");
    }
    wbuf_put(&payload, req + hl, req_len - (size_t)hl);           /* the raw form carries its body inline */
    wbuf_put(&payload, body, body_len);
    if (verb.oom || url.oom || hdrs.oom || payload.oom) { *err = "wsfull"; goto done; }

    int status = host_fetch(verb.p, url.p, hdrs.p, (const uint8_t*)payload.p, payload.n, &head, &rbody, &rbody_len);
    if (status < 0) { *err = "conn"; goto done; }

    char* nl = strchr(head, '\n');
    *nl = '\0';
    for (char* r = head; *r; r++) if (*r == '\r') *r = ' ';
    snprintf(line, sizeof line, "HTTP/1.1 %d%s", status, head[0] ? " " : "");
    wbuf_str(&out, line);
    wbuf_str(&out, head);
    wbuf_str(&out, "\r\n");
    for (char* s = nl + 1; *s;) {
        char* e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        size_t k = n && s[n - 1] == '\r' ? n - 1 : n;
        const char* colon = memchr(s, ':', k);
        if (colon && colon > s && keep_response_header(s, (size_t)(colon - s), no_body)) {
            wbuf_put(&out, s, k);
            wbuf_str(&out, "\r\n");
        }
        s = e ? e + 1 : s + n;
    }
    if (!no_body) {
        snprintf(line, sizeof line, "Content-Length: %zu\r\n", rbody_len);
        wbuf_str(&out, line);
    }
    wbuf_str(&out, "\r\n");
    if (!no_body) wbuf_put(&out, rbody, rbody_len);
    if (out.oom) { *err = "wsfull"; goto done; }
    resp = out.p;
    out.p = NULL;
    *len = out.n;
done:
    free(verb.p); free(url.p); free(hdrs.p); free(payload.p); free(out.p);
    free(head); free(rbody);
    return resp;
}
