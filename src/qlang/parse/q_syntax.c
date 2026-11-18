/* q_syntax — the tolerant syntax tree behind `.pq.syntax`.  Statements are q_ctx's (the walk `\l` runs), tokens are
 * the strict scanner's, and the tree is only brackets and `;` slots, so it recovers at every closer and every
 * statement.  A statement it finds well-formed is parsed again by q_parse_at, whose refusal is an error row, so the
 * strict parser stays the one judge of what is legal q. */
#include "qlang/q_count.h"
#include "qlang/parse/q_syntax.h"
#include "qlang/parse/q_parse_internal.h"
#include "qlang/base/q_err.h"
#include "qlang/q_ctx.h"    /* q_ctx_file_spans — the statement cuts */
#include "qlang/q_env.h"    /* q_env_bind_native */
#include "qlang/q_prim.h"   /* q_str_text_bytes */
#include "qlang/parse/q_parse.h"  /* q_parse_state */
#include "ops/hash.h"       /* ray_hash_bytes */
#include "lang/env.h"       /* ray_fn_unary */
#include "lang/eval.h"      /* RAY_FN_NONE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { K_STMT, K_CMD, K_COMMENT, K_EXPR, K_LAMBDA, K_PARAMS, K_ARGS, K_BLOCK, K_PAREN,
       K_NAME, K_NUM, K_STR, K_SYM, K_VERB, K_ADVERB, K_N };
static const char *KIND_NAME[K_N] = { "stmt", "cmd", "comment", "expr", "lambda", "params", "args", "block", "paren",
                                      "name", "num", "str", "sym", "verb", "adverb" };

typedef struct { int parent, kind, start, end, scope; } syntax_node_t;
typedef struct { int node, closer, expr, open_at, prev, sig; } syntax_frame_t;   /* sig: a lambda's first token next */
typedef struct { size_t start, end; int kind; } syntax_pend_t;

typedef struct {
    char           *s;               /* the source, NUL-terminated; a statement's blocks are blanked as it is read */
    syntax_node_t  *n;
    int             nn, cn;
    syntax_frame_t *fr;
    int             cf;
    syntax_pend_t  *pend;            /* comments the walk yielded ahead of their statement */
    int             np, cp;
    ray_t          *es, *ee, *em;    /* the errors columns */
    int             ne, oom;
} syntax_t;

static int syntax_grow(void **p, int *cap, int need, size_t size) {
    if (need < *cap) return 1;
    int c = *cap ? 2 * *cap : 64;
    void *q = realloc(*p, (size_t)c * size);
    if (!q) return 0;
    *p = q; *cap = c;
    return 1;
}

static int syntax_add(syntax_t *t, int parent, int kind, int start, int end) {
    if (t->oom || !syntax_grow((void **)&t->n, &t->cn, t->nn, sizeof *t->n)) { t->oom = 1; return parent; }
    int pk = parent < 0 ? -1 : t->n[parent].kind;
    int scope = pk == K_LAMBDA || pk == K_PARAMS ? parent : parent < 0 ? -1 : t->n[parent].scope;
    t->n[t->nn] = (syntax_node_t){ parent, kind, start, end, scope };
    return t->nn++;
}

static void syntax_err(syntax_t *t, int start, int end, const char *msg, size_t n) {
    int64_t a = start, b = end;
    ray_t *m = ray_charv(msg, (int64_t)n);
    t->es = ray_vec_append(t->es, &a);
    t->ee = ray_vec_append(t->ee, &b);
    t->em = ray_list_append(t->em, m);
    ray_release(m);
    t->ne++;
}

static void syntax_err_closer(syntax_t *t, const char *what, int closer, int start, int end) {
    char msg[16];
    int n = snprintf(msg, sizeof msg, "%s %c", what, closer == T_RPAREN ? ')' : closer == T_RBRACK ? ']' : '}');
    syntax_err(t, start, end, msg, (size_t)n);
}

/* A q error as the console words it, with the parser's reason for a bare 'parse, in a buffer of its own (*n bytes).
 * It consumes the error's text, which q_err_text reads from the VM: left there, the next statement's error would be
 * worded with it. */
static char *syntax_q_msg(ray_t *e, size_t *n) {
    int64_t tl = 0;
    const char *tx = q_err_text(e, &tl), *why = q_parse_die_msg();
    size_t wl = why ? strlen(why) : 0;
    *n = (size_t)tl + 1 + (why ? wl + 2 : 0);
    char *msg = malloc(*n);
    if (msg) {
        msg[0] = '\'';
        memcpy(msg + 1, tx, (size_t)tl);
        if (why) { memcpy(msg + 1 + tl, ": ", 2); memcpy(msg + 3 + tl, why, wl); }
    }
    q_err_drop();
    return msg;
}

static void syntax_err_q(syntax_t *t, int start, int end, ray_t *e) {
    size_t n;
    char *msg = syntax_q_msg(e, &n);
    if (msg) syntax_err(t, start, end, msg, n);
    else t->oom = 1;
    free(msg);
}

static void syntax_close(syntax_t *t, int d, int end) {
    t->n[t->fr[d].expr].end = end;
    t->n[t->fr[d].node].end = end;
}

static void syntax_token(syntax_t *t, int *d, const q_scan_tok_t *tk, int ts, int te) {
    syntax_frame_t *f = &t->fr[*d];
    TKind k = tk->kind;
    if (k == T_COMMENT) { syntax_add(t, f->expr, K_COMMENT, ts, te); return; }
    int had = f->prev, sig = f->sig;
    f->prev = 1;
    f->sig = 0;
    if (k == T_LPAREN || k == T_LBRACE || k == T_LBRACK) {
        int kind = k == T_LPAREN ? K_PAREN : k == T_LBRACE ? K_LAMBDA : had ? K_ARGS : K_BLOCK, parent = f->expr;
        if (k == T_LBRACK && sig) { kind = K_PARAMS; parent = f->node; f->prev = 0; }
        if (!syntax_grow((void **)&t->fr, &t->cf, *d + 1, sizeof *t->fr)) { t->oom = 1; return; }
        int c = syntax_add(t, parent, kind, ts, -1);
        int closer = k == T_LPAREN ? T_RPAREN : k == T_LBRACE ? T_RBRACE : T_RBRACK;
        t->fr[++*d] = (syntax_frame_t){ c, closer, syntax_add(t, c, K_EXPR, te, -1), ts, 0, kind == K_LAMBDA };
    } else if (k == T_SEMI) {
        t->n[f->expr].end = ts;
        f->expr = syntax_add(t, f->node, K_EXPR, te, -1);
        f->prev = 0;
    } else if (k == T_RPAREN || k == T_RBRACE || k == T_RBRACK) {
        int m = *d;
        while (m > 0 && t->fr[m].closer != (int)k) m--;
        if (!m) { syntax_err_closer(t, "unmatched", (int)k, ts, te); f->prev = had; f->sig = sig; return; }
        for (; *d > m; --*d) {
            syntax_err_closer(t, "missing", t->fr[*d].closer, t->fr[*d].open_at, ts);
            syntax_close(t, *d, ts);
        }
        syntax_close(t, *d, ts);
        t->n[t->fr[*d].node].end = te;
        int params = t->n[t->fr[*d].node].kind == K_PARAMS;
        --*d;
        if (params) t->n[t->fr[*d].expr].start = te;
    } else {
        int kind = k == T_VERB ? K_VERB : k == T_ADVERB ? K_ADVERB : tk->noun == Q_NOUN_NAME ? K_NAME
                 : tk->noun == Q_NOUN_STR ? K_STR : tk->noun == Q_NOUN_SYM ? K_SYM : K_NUM;
        syntax_add(t, f->expr, kind, ts, te);
    }
}

/* The strict parser's verdicts, memoised by statement text under one parse state (q_parse_state): a call that finds
 * another state empties the memo first, so an edit invalidates nothing and nothing else need.  Entries no call of the
 * last STRICT_CALLS used, then the least recently used past STRICT_BYTES in all, are dropped as each call ends, so an
 * editor's memory stays level however long it runs.  A refusal for want of memory is never kept. */
enum { STRICT_BUCKETS = 4096, STRICT_CALLS = 8, STRICT_BYTES = 4 << 20 };

typedef struct strict_s {
    struct strict_s *chain, *older, *newer;
    uint64_t h, call;
    int64_t off;                 /* the refusal's caret from the statement's start, -1 for a statement it accepts */
    char *msg;
    size_t n, ml;
    char text[];
} strict_t;

static struct {
    strict_t *bucket[STRICT_BUCKETS], *oldest, *newest;
    size_t bytes;
    uint64_t call;
    ray_t *state;                /* the parse state every entry was judged under; NULL: the memo is not in use */
    int off;
} g_strict;

static strict_t **strict_slot(uint64_t h) { return &g_strict.bucket[h & (STRICT_BUCKETS - 1)]; }

static void strict_link(strict_t *e) {
    e->call = g_strict.call;
    e->older = g_strict.newest;
    e->newer = NULL;
    if (g_strict.newest) g_strict.newest->newer = e;
    else g_strict.oldest = e;
    g_strict.newest = e;
}

static void strict_unlink(strict_t *e) {
    if (e->older) e->older->newer = e->newer;
    else g_strict.oldest = e->newer;
    if (e->newer) e->newer->older = e->older;
    else g_strict.newest = e->older;
}

static void strict_drop(strict_t *e) {
    strict_t **c = strict_slot(e->h);
    while (*c != e) c = &(*c)->chain;
    *c = e->chain;
    strict_unlink(e);
    g_strict.bytes -= sizeof *e + e->n + e->ml;
    free(e->msg);
    free(e);
}

static void strict_empty(void) {
    while (g_strict.oldest) strict_drop(g_strict.oldest);
    if (g_strict.state) ray_release(g_strict.state);
    g_strict.state = NULL;
}

/* A call's start: the memo kept when the parse state is the one its entries were judged under, else emptied for it. */
static void strict_begin(void) {
    g_strict.call++;
    ray_t *now = g_strict.off ? NULL : q_parse_state();
    if (now && RAY_IS_ERR(now)) { ray_release(now); now = NULL; }
    if (now && g_strict.state && q_match_rec(g_strict.state, now)) { ray_release(now); return; }
    strict_empty();
    g_strict.state = now;
}

static void strict_trim(void) {
    while (g_strict.oldest && (g_strict.oldest->call + STRICT_CALLS <= g_strict.call || g_strict.bytes > STRICT_BYTES))
        strict_drop(g_strict.oldest);
}

static strict_t *strict_find(const char *s, size_t n, uint64_t h) {
    for (strict_t *e = g_strict.state ? *strict_slot(h) : NULL; e; e = e->chain)
        if (e->h == h && e->n == n && !memcmp(e->text, s, n)) {
            strict_unlink(e);
            strict_link(e);
            return e;
        }
    return NULL;
}

/* The verdict kept, msg its own from here; NULL, msg still the caller's, when the memo is not in use or has no room. */
static strict_t *strict_add(const char *s, size_t n, uint64_t h, int64_t off, char *msg, size_t ml) {
    strict_t *e = g_strict.state ? malloc(sizeof *e + n) : NULL;
    if (!e) return NULL;
    *e = (strict_t){ .chain = *strict_slot(h), .h = h, .off = off, .msg = msg, .n = n, .ml = ml };
    memcpy(e->text, s, n);
    *strict_slot(h) = e;
    strict_link(e);
    g_strict.bytes += sizeof *e + n + ml;
    return e;
}

/* The strict parser's verdict on a statement the tree found clean: a refusal spans the token at its caret. */
static void syntax_strict(syntax_t *t, int stmt, int a, int b) {
    const char *s = t->s + a;
    size_t n = (size_t)(b - a), ml = 0;
    uint64_t h = ray_hash_bytes(s, n);
    strict_t *e = strict_find(s, n, h);
    int64_t off = e ? e->off : -1;
    char *msg = e ? e->msg : NULL;
    if (e) ml = e->ml;
    else {
        ray_t *pos = NULL, *r = q_parse_at(s, &pos);
        int keep = r && !(pos && RAY_IS_ERR(pos));
        if (r && RAY_IS_ERR(r)) {
            int64_t at = pos && pos->type == RAY_I64 && pos->len >= 2 ? ((int64_t *)ray_data(pos))[1] : 0;
            off = at >= 0 && at < (int64_t)n ? at : 0;
            keep = keep && !q_parse_lacked_memory(r);
            if (!(msg = syntax_q_msg(r, &ml))) t->oom = 1;
        }
        if (pos) ray_release(pos);
        if (r && RAY_IS_ERR(r)) ray_error_free(r);
        else if (r) ray_release(r);
        if (t->oom) return;
        if (keep) e = strict_add(s, n, h, off, msg, ml);
    }
    if (off >= 0) {
        int ea = a + (int)off, ee = ea + 1;
        for (int i = stmt + 1; i < t->nn; i++)
            if (t->n[i].kind >= K_NAME && t->n[i].start <= ea && ea < t->n[i].end) { ee = t->n[i].end; break; }
        syntax_err(t, ea, ee, msg, ml);
    }
    if (!e) free(msg);
}

static int syntax_statement(syntax_t *t, int a, int b) {
    int opaque;
    int at = a + (int)q_parse_intake(t->s + a, (size_t)(b - a), &opaque);
    if (opaque) return syntax_add(t, -1, K_CMD, a, b);
    int stmt = syntax_add(t, -1, K_STMT, a, b), errs = t->ne, d = 0;
    if (t->oom || !syntax_grow((void **)&t->fr, &t->cf, 0, sizeof *t->fr)) { t->oom = 1; return stmt; }
    t->fr[0] = (syntax_frame_t){ stmt, 0, syntax_add(t, stmt, K_EXPR, at, b), a, 0, 0 };
    while (at < b && !t->oom) {
        q_scan_t sc;
        if (q_parse_scan(t->s + at, &sc) < 0) { t->oom = 1; break; }
        for (int i = 0; i < sc.n && !t->oom; i++)
            syntax_token(t, &d, &sc.tok[i], at + sc.tok[i].start, at + sc.tok[i].start + sc.tok[i].len);
        free(sc.tok);
        if (!sc.err) break;
        int ea = at + sc.err_at, ee = sc.err_len < 0 ? b : ea + sc.err_len;
        syntax_err_q(t, ea, ee, sc.err);
        ray_error_free(sc.err);
        at = ee;
    }
    if (t->oom) return stmt;
    for (; d > 0; d--) {
        syntax_err_closer(t, "missing", t->fr[d].closer, t->fr[d].open_at, b);
        syntax_close(t, d, b);
    }
    t->n[t->fr[0].expr].end = b;
    if (t->ne == errs) syntax_strict(t, stmt, a, b);
    return stmt;
}

static void syntax_comment(syntax_t *t, int parent, const syntax_pend_t *c) {
    syntax_add(t, parent, K_COMMENT, (int)c->start, (int)c->end);
}

/* q_ctx_file_spans yields a statement's comment lines and blocks before the statement itself.  A comment line inside
 * it is the scanner's to find; a block inside it is blanked, as `\l` cuts it, and becomes the statement's child. */
static int syntax_entry(size_t start, size_t end, int64_t line, int kind, void *u) {
    (void)line;
    syntax_t *t = u;
    if (kind != Q_CTX_SPAN_STMT) {
        if (!syntax_grow((void **)&t->pend, &t->cp, t->np, sizeof *t->pend)) return t->oom = 1;
        t->pend[t->np++] = (syntax_pend_t){ start, end, kind };
        return 0;
    }
    int a = (int)start, b = (int)end, i = 0, k = 0;
    for (; i < t->np && (int)t->pend[i].start < a; i++) syntax_comment(t, -1, &t->pend[i]);
    for (int j = i; j < t->np && (int)t->pend[j].start < b; j++)
        if (t->pend[j].kind == Q_CTX_SPAN_BLOCK)
            for (size_t c = t->pend[j].start; c < t->pend[j].end; c++) if (t->s[c] != '\n') t->s[c] = ' ';
    char keep = t->s[b];
    t->s[b] = '\0';
    int stmt = syntax_statement(t, a, b);
    t->s[b] = keep;
    for (; i < t->np && (int)t->pend[i].start < b; i++)
        if (t->pend[i].kind == Q_CTX_SPAN_BLOCK) syntax_comment(t, stmt, &t->pend[i]);
    for (; i < t->np; i++) t->pend[k++] = t->pend[i];
    t->np = k;
    return t->oom;
}

static ray_t *syntax_put(ray_t *tb, const char *name, ray_t *col) {
    tb = ray_table_add_col(tb, ray_sym_intern_runtime(name, strlen(name)), col);
    ray_release(col);
    return tb;
}

static ray_t *syntax_nodes(const syntax_t *t, const char *src) {
    ray_t *c[5], *kind = ray_sym_vec_new(RAY_SYM_W64, t->nn), *text = ray_list_new(t->nn);
    for (int j = 0; j < 5; j++) c[j] = ray_vec_new(RAY_I64, t->nn);
    for (int i = 0; i < t->nn; i++) {
        const syntax_node_t *x = &t->n[i];
        int64_t v[5] = { i, x->parent, x->start, x->end, x->scope };
        for (int j = 0; j < 5; j++) c[j] = ray_vec_append(c[j], &v[j]);
        int64_t k = ray_sym_intern_runtime(KIND_NAME[x->kind], strlen(KIND_NAME[x->kind]));
        kind = ray_vec_append(kind, &k);
        ray_t *s = ray_charv(src + x->start, x->end - x->start);
        text = ray_list_append(text, s);
        ray_release(s);
    }
    ray_t *tb = ray_table_new(7);
    tb = syntax_put(tb, "id", c[0]);
    tb = syntax_put(tb, "parent", c[1]);
    tb = syntax_put(tb, "kind", kind);
    tb = syntax_put(tb, "start", c[2]);
    tb = syntax_put(tb, "end", c[3]);
    tb = syntax_put(tb, "scope", c[4]);
    return syntax_put(tb, "text", text);
}

static ray_t *syntax_fn(ray_t *x) {
    const char *p;
    int64_t n;
    if (!q_str_text_bytes(x, &p, &n)) return q_err(QE_TYPE);
    if (n >= INT32_MAX) return q_err(QE_LIMIT);
    ray_t *held = q_err_take();   /* the caller's error text, which no statement here may be worded with */
    strict_begin();
    syntax_t t = { .s = malloc((size_t)n + 1), .es = ray_vec_new(RAY_I64, 4), .ee = ray_vec_new(RAY_I64, 4),
                   .em = ray_list_new(4) };
    ray_t *out = NULL;
    if (t.s) {
        memcpy(t.s, p, (size_t)n);
        t.s[n] = '\0';
        q_ctx_file_spans(t.s, (size_t)n, syntax_entry, &t);
        for (int i = 0; i < t.np; i++) syntax_comment(&t, -1, &t.pend[i]);
    }
    if (t.s && !t.oom) {
        ray_t *errors = syntax_put(syntax_put(syntax_put(ray_table_new(3), "start", t.es), "end", t.ee), "msg", t.em);
        t.es = t.ee = t.em = NULL;
        ray_t *keys = ray_sym_vec_new(RAY_SYM_W64, 2), *vals = ray_list_new(2), *nodes = syntax_nodes(&t, p);
        int64_t k[2] = { ray_sym_intern_runtime("nodes", 5), ray_sym_intern_runtime("errors", 6) };
        keys = ray_vec_append(ray_vec_append(keys, &k[0]), &k[1]);
        vals = ray_list_append(vals, nodes);
        vals = ray_list_append(vals, errors);
        ray_release(nodes);
        ray_release(errors);
        out = ray_dict_new(keys, vals);
    }
    if (t.es) ray_release(t.es);
    if (t.ee) ray_release(t.ee);
    if (t.em) ray_release(t.em);
    free(t.s); free(t.n); free(t.fr); free(t.pend);
    strict_trim();
    if (out) q_err_put(held);
    else if (held) ray_release(held);
    return out ? out : q_err(QE_OOM);
}

/* .pq.i.syntax_cache b: whether strict verdicts are memoised, answering the setting it replaces.  0b also empties the
 * memo, so a test can set a run with none against a cold one and a warm one. */
static ray_t *syntax_cache_fn(ray_t *x) {
    if (!x || x->type != -RAY_BOOL) return q_err(QE_TYPE);
    int was = !g_strict.off;
    g_strict.off = !x->b8;
    if (g_strict.off) strict_empty();
    return ray_bool(was);
}

/* .pq.i.parse_state[]: q_parse_state, for a q memo of what a parse answers. */
static ray_t *syntax_state_fn(ray_t *x) {
    (void)x;
    return q_parse_state();
}

void q_syntax_pq_register(void) {
    ray_t *f = ray_fn_unary(".pq.i.syntax", RAY_FN_NONE, syntax_fn);
    q_env_bind_native(".pq.i.syntax", f, 1);
    ray_release(f);
    f = ray_fn_unary(".pq.i.syntax_cache", RAY_FN_NONE, syntax_cache_fn);
    q_env_bind_native(".pq.i.syntax_cache", f, 1);
    ray_release(f);
    f = ray_fn_unary(".pq.i.parse_state", RAY_FN_NONE, syntax_state_fn);
    q_env_bind_native(".pq.i.parse_state", f, 1);
    ray_release(f);
}
