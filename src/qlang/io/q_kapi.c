/* q_kapi — the kdb C-API seam and `2:` Dynamic Load.  See q_kapi.h for the overlay identity. */

/* A tag-for-tag marshaller between kdb's type numbers and rayfall's names the byte enumerator RAW:
 * the byte-lane macros answer "byte-like or byte-only?", a question a 1:1 tag table has settled. */
#define RAY_ALLOW_RAW_U8

#include "qlang/q_count.h"
#include "qlang/io/q_kapi.h"
#include "qlang/base/q_calendar.h"   /* q_calendar_days_from_civil — the one civil-calendar home */
#include "qlang/base/q_err.h"
#include "qlang/eval/q_dbg.h"        /* q_dbg_statement_origin, q_dbg_trap_enter/exit */
#include "qlang/eval/q_eval.h"       /* the KFN carrier + q_eval_apply_value */
#include "qlang/q_builtins.h"        /* q_builtins_type_num — THE q type answer a function crosses as */
#include "qlang/q_ctx.h"             /* q_ctx_eval_src — what k(0,…) evaluates through */
#include "qlang/q_registry.h"        /* the `.` verb dot applies */
#include "qlang/q_prim.h"            /* the attribute BYTE both ways, the enum domain, vk's collapse */
#include "qlang/io/q_dl.h"           /* q_dl_open / q_dl_sym — the extension, never unmapped; q_dl_unavailable */
#include "qlang/io/q_io.h"           /* q_io_abs_path — a Windows candidate's lexical absolute path */
#include "qlang/net/q_wire.h"        /* b9/d9/okx are the -8!/-9! codec */
#include "core/poll.h"
#include "mem/heap.h"                /* ray_free_set_qfn_fin_fn — the foreign-destructor choke point */
#include "core/runtime.h"
#include "core/types.h"              /* ray_elem_size — the overlay widths */
#include "ops/idxop.h"               /* ray_attr_drop_fn — a relabelled result sheds its old attribute */
#include "ops/ops.h"                 /* RAY_EXTRACT_* */
#include "ops/temporal.h"            /* ray_temporal_extract — dj's civil fields */
#include "table/sym.h"               /* ray_sym_vec_cell, ray_read_sym */
#include <rayforce.h>

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

/* ===== kdb's object model ================================================================== */

/* struct k0 exactly as k.h declares it, so the extension's own kI()/x->n macros address the same
 * bytes.  Declared here rather than included from third_party/k.h: a build of ./q must not depend on
 * a vendored header for its own ABI — test/kapi/fixture.c compiles against the real one, and
 * test/q/kapi.qcmd is what proves the two agree. */
typedef char* S;
typedef char  C;
typedef unsigned char G;
typedef short H;
typedef int   I;
typedef long long J;
typedef unsigned long long UJ;
typedef float  E;
typedef double F;
typedef void   V;
typedef struct { G g[16]; } U;

typedef struct k0 {
    signed char m, a, t;
    C u;
    I r;
    union {
        G g; H h; I i; J j; E e; F f; S s; struct k0* k;
        struct { J n; G G0[]; };
    };
}* K;

#define kG(x) ((x)->G0)
#define kS(x) ((S*)kG(x))
#define kK(x) ((K*)kG(x))
#define K_HDR offsetof(struct k0, G0)

/* THE overlay identity (q_kapi.h): a ray_t read 16 bytes in IS a struct k0. */
#define K_OF(v)   ((K)((char*)(v) + 16))
#define RAY_OF(x) ((ray_t*)((char*)(x) - 16))

/* A shim-owned k0 (a copy, not an overlay) says so in the `m` byte: its mmod bits hold 2, which no ray
 * header does (mmod is 0 or 3), and kx code never reads `m`.  Built through the header itself, so the
 * value follows the compiler's bitfield layout.  Everything else is an overlay: r1/r0 know which. */
static signed char k_shim_m(void) {
    ray_t h;
    memset(&h, 0, sizeof h);
    h.mmod = 2;
    return ((signed char*)&h)[16];
}
static int k_is_shim(K x) { return x && x->m == k_shim_m(); }

/* 100h–111h — a q FUNCTION VALUE on its way through an extension.  The `u` byte of a function shim is
 * ours (a function has no attribute).  kdb hands a function over opaque: py.c
 * only r1s it, capsules it, and hands it back to `k(0;".";f;args)` (py.c:34), so the one slot holds
 * the ray value RAW (never a K, like a foreign's slots) and `u` says which shape this is, because a
 * 101h shim is also how `::` crosses. */
#define K_UFN 2
static int k_is_qfn(K x) {
    return k_is_shim(x) && x->t >= 100 && x->t <= 111 && x->n == 1 && x->u == K_UFN;
}

/* 112h — an extension's FOREIGN object: `knk(2,destructor,payload)` with `xt` overwritten (py.c:7;
 * pykx and pcre2 use the same convention).  Its two slots are RAW pointers, never K objects, so no
 * walk here may treat them as such.  It is the CANONICAL object: the q-side carrier boxes this k0
 * and owns one reference, and r0 running the destructor at the last one is kdb's own law. */
static int k_is_foreign(K x) { return k_is_shim(x) && x->t == 112 && x->n >= 2; }

/* PUBLISHED OWNERSHIP CONTRACT — interfaces/capiref.md `Tags:` lines, which this file obeys and the
 * one table that records them.  `own` CONSUMES its arguments' references; everything else BORROWS.
 * `ee` returns 0 on error (the caller propagates 0 or calls ee).  capiref.md:103 is the law over all
 * of it: a dynamically-linked module never takes ownership of its parameters — the same rule as the
 * apply module's args-borrowed/result-owned (CLAUDE.md rule 5), which is why q_kapi_invoke borrows.
 *
 *   own       k  vak  knk  vaknk  jk  ktd  xD  xT  vk
 *   ee        knt  ktd  xT  b9  d9  dot
 *   borrow    everything else, knt's `x` explicitly (capiref.md: "does not take ownership of x")
 *   returns   every function that hands back a K hands back a reference the CALLER owns */

/* Payload width per tag.  The tags are rayfall's, so an overlay's width IS ray's (an enum's i64 positions
 * are kdb's J, its domain riding the ray header BELOW the k0); only kdb's two C pointer lanes differ. */
static size_t k_esz(I t) { return t == 0 ? sizeof(K) : t == 11 ? sizeof(S) : ray_elem_size(t); }

static int k_tag_ok(I t) { return t >= 0 && t <= 19 && t != 3; }
/* An extension may build a 20h (kxkdb's simple_to_compound fills one with positions), but only a q value's
 * overlay carries a domain: a shim one is scratch the extension reads, and 'type if it is handed back. */
static int k_tag_vec(I t) { return k_tag_ok(t) || t == RAY_ENUM; }
static int k_growable(K x) { return k_tag_ok(x->t) || (x->t == RAY_ENUM && k_is_shim(x)); }
/* the tags whose bytes a ray_t already IS: everything but the mixed list and the symbol vector */
static int k_tag_overlays(I t) { return k_tag_ok(t) && t != 0 && t != 11; }
/* Atoms: as above, minus the two representation mismatches.  A real ATOM is widened into ray's f64
 * slot, and a 32-bit temporal ATOM (month/date/minute/second/time) is widened into its i64 slot
 * (vec/atom.c), where kdb writes the 4-byte `i` — so a negative date would lose its sign.  Their
 * VECTORS agree element for element and still overlay; only these atoms copy. */
static int k_atom_overlays(I t) { return k_tag_overlays(t) && t != 8 && !RAY_IS_TEMPORAL32(t); }

/* The exported ABI.  These deliberately have NO peachq header — the consumer is an extension
 * compiled against kdb's own k.h, which declares them itself.  Prototyped here only so the file
 * builds under -Wmissing-prototypes, and as the written record of how much of k.h is served.  Not served:
 * the khp* / kclose IPC client, and m4 / sslInfo, which capiref.md gives standalone applications only. */
S sn(const S s, I n);
S ss(const S s);
K krr(const S s);
K orr(const S s);
K ee(K x);
K dl(V* f, J n);
K ka(I t);
K kb(I x);
K kg(I x);
K kh(I x);
K ki(I x);
K kj(J x);
K ke(F x);
K kf(F x);
K kc(I x);
K ks(const S x);
K kd(I x);
K kz(F x);
K kt(I x);
K ktj(I t, J x);
K ktn(I type, J n);
K kpn(const S s, J n);
K kp(const S s);
K knk(I n, ...);
K ku(U u);
K r1(K x);
V r0(K x);
K xD(K x, K y);
K xT(K x);
K ktd(K x);
K knt(J n, K x);
K ja(K* x, V* y);
K js(K* x, S y);
K jk(K* x, K y);
K jv(K* x, K y);
I setm(I m);
I ver(void);
V m9(void);
J gc(J j);
I ymd(I y, I m, I d);
I dj(I date);
K sd1(I d, K (*f)(I));
V sd0x(I d, I f);
V sd0(I d);
K k(I handle, const S s, ...);
K vak(I handle, const S s, va_list ap);
K vaknk(I n, va_list ap);
K dot(K x, K y);
K vk(K x);
K vi(K x, UJ j);
K b9(I mode, K x);
K d9(K x);
I okx(K x);

/* ===== the pending error slot ============================================================== */

/* kdb's krr() records the message and returns NULL; the extension then `return krr("type")`s.  A
 * NULL return with NO pending error is the identity `::`, which is how a void-ish entry point
 * returns.  q_kapi_invoke and k() are the only readers. */
static char g_kerr[256];
static int  g_kerr_set;

K krr(const S s) {
    snprintf(g_kerr, sizeof g_kerr, "%s", s ? s : "");
    g_kerr_set = 1;
    return (K)0;
}

K orr(const S s) {
    const char* e = strerror(errno);
    snprintf(g_kerr, sizeof g_kerr, "%s: %s", s ? s : "", e ? e : "");
    g_kerr_set = 1;
    return (K)0;
}

/* ===== symbol intern ======================================================================= */

/* kdb's `ss` contract is a char* stable for the process lifetime, which a ray sym (a narrow domain
 * id, SSO, not NUL-terminated) cannot hand out.  The intern table is already THE dictionary, so this
 * is only an id-indexed mirror of C strings over it — never a second hash. */
static char** g_symmirror;
static size_t g_symcap;

static S sym_cstr(int64_t id) {
    if (id < 0) return (S)0;
    if ((size_t)id >= g_symcap) {
        size_t nc = g_symcap ? g_symcap : 256;
        while (nc <= (size_t)id) nc *= 2;
        char** nt = (char**)realloc(g_symmirror, nc * sizeof(char*));
        if (!nt) return (S)0;
        memset(nt + g_symcap, 0, (nc - g_symcap) * sizeof(char*));
        g_symmirror = nt;
        g_symcap = nc;
    }
    if (!g_symmirror[id]) {
        ray_t* s = ray_sym_str(id);
        if (!s) return (S)0;
        size_t n = ray_str_len(s);
        char* p = (char*)malloc(n + 1);
        if (!p) return (S)0;
        memcpy(p, ray_str_ptr(s), n);
        p[n] = '\0';
        g_symmirror[id] = p;
    }
    return g_symmirror[id];
}

S sn(const S s, I n) {
    if (!s || n < 0) return (S)0;
    return sym_cstr(ray_sym_intern_runtime(s, (size_t)n));
}

S ss(const S s) { return s ? sn(s, (I)strlen(s)) : (S)0; }

static int64_t sym_id(const S s) {
    return s ? ray_sym_intern_runtime(s, strlen(s)) : ray_sym_intern_runtime("", 0);
}

/* ===== shim-owned k0s ====================================================================== */

/* Capacity in bytes for n elements, rounded to a power of two so ja/js/jk amortise: the growth test
 * is a pure function of n, which is what lets struct k0 stay byte-compatible (no capacity field). */
static size_t shim_cap(J n, size_t esz) {
    size_t need = (size_t)n * (size_t)esz, c = 64;
    while (c < need) c <<= 1;
    return c;
}

static K shim_alloc(size_t extra) {
    K x = (K)calloc(1, K_HDR + extra);
    if (x) x->m = k_shim_m();
    return x;
}

static K shim_vec(I t, J n) {
    if (!k_tag_vec(t) || n < 0 || (uint64_t)n > SIZE_MAX / 16) return (K)0;
    K x = shim_alloc(shim_cap(n, k_esz(t)));
    if (!x) return (K)0;
    x->t = (signed char)t;
    x->n = n;
    return x;
}

static K shim_atom(I t) {
    K x = shim_alloc(0);
    if (x) x->t = (signed char)t;
    return x;
}

/* THE one -128h builder.  kdb's error text is a SYMBOL, so `x->s` OUTLIVES the object it came on —
 * py.c:26 reads the message after r0'ing it — and the intern mirror is what makes a char* that
 * persistent.  `x->s` may still be NULL (capiref.md `ee`), and readers check. */
static K shim_err(const char* m, size_t n) {
    K x = shim_atom(-128);
    if (x) x->s = sn((S)m, (I)n);
    return x;
}

/* capiref.md `ee`: capture AND RESET the pending error into the usual -128h object.  A non-NULL x is
 * its own answer and the error status is untouched — only the NULL path clears it.  A NULL x is ALWAYS
 * answered with a -128h object, whose `x->s` is NULL when nothing was pending: capiref.md's own
 * `ee(dot(a,b))` reads `xt` unchecked, and kxkdb's is_error tells that NULL-s case from a real error.
 * capiref.md:113 notes such an object may only be returned at the top level of a C function called
 * from q; that is the extension's discipline to keep, not ours to enforce. */
K ee(K x) {
    if (x) return x;
    K e = g_kerr_set ? shim_err(g_kerr, strlen(g_kerr)) : shim_atom(-128);
    g_kerr_set = 0;
    return e;
}

K r1(K x) {
    if (!x) return x;
    if (k_is_shim(x)) { x->r++; return x; }
    ray_retain(RAY_OF(x));
    return x;
}

V r0(K x) {
    if (!x) return;
    if (!k_is_shim(x)) { ray_release(RAY_OF(x)); return; }
    if (x->r > 0) { x->r--; return; }
    if (x->t == 0)  { for (J i = 0; i < x->n; i++) r0(kK(x)[i]); }
    if (x->t == 99) { r0(kK(x)[0]); r0(kK(x)[1]); }
    if (x->t == 98) r0(x->k);
    if (k_is_qfn(x)) ray_release((ray_t*)kK(x)[0]);
    /* THE exactly-once destructor call: the last reference to a foreign is where kdb runs it too. */
    if (k_is_foreign(x) && kK(x)[0]) ((V(*)(K))(void*)kK(x)[0])(x);
    free(x);
}

/* ===== constructors ======================================================================== */

/* An overlay atom: the ray atom IS the k0, so the extension writes its payload straight into the
 * union slot at +24.  Real and symbol atoms cannot take this path (k_atom_overlays). */
static K atom_overlay(ray_t* v, I t) {
    if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); return (K)0; }
    v->type = (int8_t)-t;
    return K_OF(v);
}

K ka(I t) {
    if (t < 0 && k_atom_overlays(-t)) return atom_overlay(ray_alloc(-t == RAY_GUID ? 16 : 0), -t);
    return shim_atom(t);   /* real / symbol atoms, `::` (101h), -128h, anything unknown */
}

K kb(I x) { K r = ka(-1);  if (r) r->g = (G)(x ? 1 : 0); return r; }
K kg(I x) { K r = ka(-4);  if (r) r->g = (G)x; return r; }
K kh(I x) { K r = ka(-5);  if (r) r->h = (H)x; return r; }
K ki(I x) { K r = ka(-6);  if (r) r->i = x;    return r; }
K kj(J x) { K r = ka(-7);  if (r) r->j = x;    return r; }
K ke(F x) { K r = ka(-8);  if (r) r->e = (E)x; return r; }
K kf(F x) { K r = ka(-9);  if (r) r->f = x;    return r; }
K kc(I x) { K r = ka(-10); if (r) r->g = (G)x; return r; }
K ks(const S x) { K r = ka(-11); if (r) r->s = ss((S)x); return r; }
K kd(I x) { K r = ka(-14); if (r) r->i = x;    return r; }
K kz(F x) { K r = ka(-15); if (r) r->f = x;    return r; }
K kt(I x) { K r = ka(-19); if (r) r->i = x;    return r; }
K ktj(I t, J x) { K r = ka(t); if (r) r->j = x; return r; }

K ku(U u) {
    ray_t* v = ray_guid(u.g);
    if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); return (K)0; }
    return K_OF(v);
}

K ktn(I type, J n) {
    if (n < 0) return (K)0;
    if (!k_tag_overlays(type)) return shim_vec(type, n);
    ray_t* v = ray_vec_new((int8_t)type, n);
    if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); return (K)0; }
    v->len = n;
    if (n > 0) memset(ray_data(v), 0, (size_t)n * k_esz(type));
    return K_OF(v);
}

K kpn(const S s, J n) {
    if (n < 0) return (K)0;
    ray_t* v = ray_charv(s && n > 0 ? s : "", n);
    if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); return (K)0; }
    return K_OF(v);
}

K kp(const S s) { return kpn(s, s ? (J)strlen(s) : 0); }

K vaknk(I n, va_list ap) {      /* own: takes ownership of references to its arguments */
    K x = shim_vec(0, n);
    if (!x) return (K)0;
    for (I i = 0; i < n; i++) kK(x)[i] = va_arg(ap, K);
    return x;
}

K knk(I n, ...) {
    va_list ap;
    va_start(ap, n);
    K x = vaknk(n, ap);
    va_end(ap);
    return x;
}

I setm(I m) { (void)m; return 0; }
I ver(void) { return 3; }        /* KXVER 3 layouts only (decision 1) */
V m9(void) {}
J gc(J j)  { (void)j; return 0; }

I ymd(I y, I m, I d) { return (I)q_calendar_days_from_civil(y, m, d); }

I dj(I date) {
    ray_t* v = ray_date(date);
    if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); return 0; }
    ray_t* y = ray_temporal_extract(v, RAY_EXTRACT_YEAR);
    ray_t* m = ray_temporal_extract(v, RAY_EXTRACT_MONTH);
    ray_t* d = ray_temporal_extract(v, RAY_EXTRACT_DAY);
    I out = (!y || RAY_IS_ERR(y) || !m || RAY_IS_ERR(m) || !d || RAY_IS_ERR(d))
                ? 0 : (I)(y->i64 * 10000 + m->i64 * 100 + d->i64);
    ray_release(v);
    if (y) { if (RAY_IS_ERR(y)) ray_error_free(y); else ray_release(y); }
    if (m) { if (RAY_IS_ERR(m)) ray_error_free(m); else ray_release(m); }
    if (d) { if (RAY_IS_ERR(d)) ray_error_free(d); else ray_release(d); }
    return out;
}

/* ---- append helpers ---- */

/* The extension call in flight BORROWS its arguments (capiref.md:103), yet kxkdb's concat_list2 grows one
 * with jv and returns it.  kdb grows in place; we cannot, because the caller still holds the value, so the
 * append is lent a reference to consume and its copy is owned by this frame until the call returns.  A
 * count above what the frame holds means the extension took a reference of its own (`z=r1(x);jv(&z,y)`),
 * and then the append consumes that one as usual. */
#define KAPI_MAX_RANK 8

typedef struct { K k; int64_t base; } kapi_held;

typedef struct kapi_frame {
    K* arg;
    int64_t narg;
    int64_t base[KAPI_MAX_RANK];
    kapi_held* own;
    size_t nown, cap;
    struct kapi_frame* up;
} kapi_frame;
static kapi_frame* g_frame;

#define GROW_PLAIN   (-1)
#define GROW_LENT    (-2)
#define GROW_NOROOM  (-3)

static int64_t k_refs(K x) { return k_is_shim(x) ? x->r : (int64_t)RAY_OF(x)->rc; }

static int grow_begin(K x) {
    if (!g_frame) return GROW_PLAIN;
    for (size_t j = 0; j < g_frame->nown; j++)
        if (g_frame->own[j].k == x) return k_refs(x) > g_frame->own[j].base ? GROW_PLAIN : (int)j;
    for (int64_t i = 0; i < g_frame->narg; i++) {
        if (g_frame->arg[i] != x) continue;
        if (k_refs(x) > g_frame->base[i]) return GROW_PLAIN;
        if (g_frame->nown == g_frame->cap) {    /* the slot the copy lands in, before the copy exists */
            size_t nc = g_frame->cap ? g_frame->cap * 2 : 4;
            kapi_held* no = (kapi_held*)realloc(g_frame->own, nc * sizeof *no);
            if (!no) return GROW_NOROOM;
            g_frame->own = no;
            g_frame->cap = nc;
        }
        r1(x);
        return GROW_LENT;
    }
    return GROW_PLAIN;
}

static void grow_end(int slot, K old, K now) {
    if (slot == GROW_PLAIN) return;
    if (slot >= 0) { g_frame->own[slot] = (kapi_held){ now, k_refs(now) }; return; }
    if (now == old) { r0(old); return; }          /* the append failed and consumed nothing */
    g_frame->own[g_frame->nown++] = (kapi_held){ now, k_refs(now) };
}

/* COW, for the same reason ray_vec_append COWs on the overlay path: another reference means the
 * storage is SHARED, and reallocating it would leave that holder pointing at freed memory. */
static K shim_grow(K x, size_t esz, J extra) {
    size_t oc = shim_cap(x->n, esz), nc = shim_cap(x->n + extra, esz);
    if (x->r > 0) {
        K c = shim_alloc(nc);
        if (!c) return (K)0;
        c->t = x->t;
        c->n = x->n;
        memcpy(kG(c), kG(x), (size_t)x->n * esz);
        if (x->t == 0) for (J i = 0; i < x->n; i++) r1(kK(c)[i]);
        x->r--;                        /* this caller's reference moves to the copy */
        return c;
    }
    x->u = 0;                          /* an append voids the attribute label, as an overlay's COW copy does */
    if (nc == oc) return x;
    return (K)realloc(x, K_HDR + nc);
}

/* ja/js/jk/jv all answer "a pointer to the (potentially reallocated) K object" (capiref.md, and
 * k.h's own `extern K … ja(K*,V*),js(K*,S) …`), so every one of them returns *x. */
static K append_ja(K* x, V* y) {
    if (!x || !*x || !k_growable(*x)) return x ? *x : (K)0;
    size_t esz = k_esz((*x)->t);
    if (k_is_shim(*x)) {
        K g = shim_grow(*x, esz, 1);
        if (!g) return *x;
        *x = g;
        memcpy(kG(*x) + (size_t)(*x)->n * esz, y, esz);
        (*x)->n++;
        return *x;
    }
    ray_t* v = ray_vec_append(RAY_OF(*x), y);   /* consumes the old, COWs when shared */
    if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); return *x; }
    *x = K_OF(v);
    return *x;
}

static K append_js(K* x, S y) {
    if (!x || !*x || (*x)->t != 11 || !k_is_shim(*x)) return x ? *x : (K)0;
    K g = shim_grow(*x, sizeof(S), 1);
    if (!g) return *x;
    *x = g;
    kS(*x)[(*x)->n] = y;
    (*x)->n++;
    return *x;
}

static K append_jk(K* x, K y) {      /* own: takes ownership of a reference to y */
    if (!x || !*x || (*x)->t != 0 || !k_is_shim(*x)) { r0(y); return x ? *x : (K)0; }
    K g = shim_grow(*x, sizeof(K), 1);
    if (!g) { r0(y); return *x; }
    *x = g;
    kK(*x)[(*x)->n] = y;
    (*x)->n++;
    return *x;
}

static K append_jv(K* x, K y) {
    if (!x || !*x || !y || (*x)->t != y->t || !k_growable(*x)) return x ? *x : (K)0;
    if (k_is_shim(*x)) {
        size_t esz = k_esz(y->t);
        J yn = y->n, at = (*x)->n;
        int self = (*x == y);     /* `jv(&a,a)`: the growth below may move the source too */
        K g = shim_grow(*x, esz, yn);
        if (!g) return *x;
        if (self) y = g;
        *x = g;
        memcpy(kG(*x) + (size_t)at * esz, kG(y), (size_t)yn * esz);
        /* jv BORROWS y (capiref.md tags it `c.o`, not `own`), so a mixed list's copied children
         * need references of their own — the caller may r0 the source straight after. */
        if (y->t == 0) for (J i = 0; i < yn; i++) r1(kK(*x)[at + i]);
        (*x)->n = at + yn;
        return *x;
    }
    ray_t* v = ray_vec_concat(RAY_OF(*x), RAY_OF(y));
    if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); return *x; }
    ray_release(RAY_OF(*x));
    *x = K_OF(v);
    return *x;
}

/* Every append funnels through here, so a borrowed argument is never consumed in the extension's place.
 * DROP is what a refused append still owes: jk owns y. */
#define K_APPEND(name, Y, DROP)                                              \
    K name(K* x, Y y) {                                                      \
        K old = x ? *x : (K)0;                                               \
        int slot = old ? grow_begin(old) : GROW_PLAIN;                       \
        if (slot == GROW_NOROOM) { DROP; return old; }                       \
        K now = append_##name(x, y);                                         \
        grow_end(slot, old, now);                                            \
        return now;                                                          \
    }
K_APPEND(ja, V*, (void)y)
K_APPEND(js, S, (void)y)
K_APPEND(jk, K, r0(y))
K_APPEND(jv, K, (void)y)

/* ---- dictionaries and tables ---- */

K xD(K x, K y) {       /* own: takes ownership of references to both arguments */
    if (!x || !y) { r0(x); r0(y); return (K)0; }
    K z = shim_alloc(2 * sizeof(K));
    if (!z) { r0(x); r0(y); return (K)0; }
    z->t = 99;
    z->n = 2;
    kK(z)[0] = x;
    kK(z)[1] = y;
    return z;
}

K xT(K x) {            /* own + ee: r0s x and returns 0 when it is not a dictionary */
    if (!x) return (K)0;
    if (x->t != 99) { r0(x); return krr((S) "type"); }
    K z = shim_alloc(0);
    if (!z) { r0(x); return (K)0; }
    z->t = 98;
    z->k = x;          /* k.h: "x->k is XD" — a table's payload IS its dictionary */
    return z;
}

/* keys/values of a table's or dictionary's spine, whichever shape it is */
static K kd_keys(K x) { return x->t == 98 ? kK(x->k)[0] : kK(x)[0]; }
static K kd_vals(K x) { return x->t == 98 ? kK(x->k)[1] : kK(x)[1]; }

static K cols_join(K a, K b, I t) {
    K z = shim_vec(t, a->n + b->n);
    if (!z) return (K)0;
    size_t esz = k_esz(t);
    memcpy(kG(z), kG(a), (size_t)a->n * esz);
    memcpy(kG(z) + (size_t)a->n * esz, kG(b), (size_t)b->n * esz);
    if (t == 0) for (J i = 0; i < z->n; i++) r1(kK(z)[i]);
    return z;
}

K ktd(K x) {           /* own + ee: a simple table from a keyed one */
    if (!x) return (K)0;
    if (x->t == 98) return x;
    if (x->t != 99 || kK(x)[0]->t != 98 || kK(x)[1]->t != 98) { r0(x); return krr((S) "type"); }
    K kt_ = kK(x)[0], vt = kK(x)[1];
    K ks_ = cols_join(kd_keys(kt_), kd_keys(vt), 11);
    K vs  = ks_ ? cols_join(kd_vals(kt_), kd_vals(vt), 0) : (K)0;
    if (!ks_ || !vs) { r0(ks_); r0(vs); r0(x); return (K)0; }
    r0(x);
    return xT(xD(ks_, vs));
}

K knt(J n, K x) {      /* ee, and explicitly BORROWS x (capiref.md) */
    if (!x || x->t != 98) return krr((S) "type");
    K keys = kd_keys(x), vals = kd_vals(x);
    if (n < 0 || n >= keys->n) return krr((S) "length");
    K kk = shim_vec(11, n), kv = shim_vec(0, n);
    K rk = shim_vec(11, keys->n - n), rv = shim_vec(0, keys->n - n);
    if (!kk || !kv || !rk || !rv) { r0(kk); r0(kv); r0(rk); r0(rv); return (K)0; }
    for (J i = 0; i < keys->n; i++) {
        K dst_k = i < n ? kk : rk, dst_v = i < n ? kv : rv;
        J at = i < n ? i : i - n;
        kS(dst_k)[at] = kS(keys)[i];
        kK(dst_v)[at] = r1(kK(vals)[i]);
    }
    return xD(xT(xD(kk, kv)), xT(xD(rk, rv)));
}

/* ===== ray -> K ============================================================================ */

/* Every K this produces is a reference the caller OWNS (r0 releases it): an overlay retains its
 * ray_t, a shim is born r=0.  Leaves inside a copied spine stay overlays — that is the whole point. */
static K k_of_ray(ray_t* v);

static K k_spine(ray_t* keys, ray_t* vals, I tag) {
    K a = k_of_ray(keys), b = a ? k_of_ray(vals) : (K)0;
    if (!a || !b) { r0(a); r0(b); return (K)0; }
    K d = xD(a, b);
    return (tag == 98 && d) ? xT(d) : d;
}

/* Every handoff restamps `u` with the q attribute (kb/serialization.md:39: 0 none, 1 s, 2 u, 3 p, 4 g), so
 * the byte a value keeps after a call is never read stale.  It is not cleared: an outer extension may still
 * hold the overlay a nested `k()` call handed on. */
static K k_view(ray_t* v) {
    K x = K_OF(v);
    C b = (C)q_attr_byte(v);
    if (x->u != b) x->u = b;
    return x;
}

/* a ray table keeps its column names as an i64 vector of sym ids; kdb wants an 11h symbol vector */
static K k_of_schema(ray_t* tbl) {
    int64_t n = ray_table_ncols(tbl);
    K z = shim_vec(11, n);
    if (!z) return (K)0;
    for (int64_t i = 0; i < n; i++) kS(z)[i] = sym_cstr(ray_table_col_name(tbl, i));
    return z;
}

static K k_of_ray(ray_t* v) {
    if (!v) return (K)0;
    int8_t t = v->type;

    /* `::` — kdb's generic null is the 101h unary primitive with value 0, and it is the ARGUMENT a
     * niladic-in-q entry point receives (`f[::]`), so this arm carries real traffic. */
    if (t == RAY_NULL) { K x = shim_atom(101); if (x) x->i = 0; return x; }
    if (RAY_IS_ERR(v)) {
        int64_t n = 0;
        const char* m = q_err_text(v, &n);
        return m ? shim_err(m, (size_t)n) : shim_err("error", 5);
    }

    if (t == RAY_TABLE) {
        K keys = k_of_schema(v);
        ray_t** slots = (ray_t**)ray_data(v);
        K vals = keys ? k_of_ray(slots[1]) : (K)0;
        if (!keys || !vals) { r0(keys); r0(vals); return (K)0; }
        return xT(xD(keys, vals));
    }
    if (t == RAY_DICT) {
        ray_t** slots = (ray_t**)ray_data(v);
        return k_spine(slots[0], slots[1], 99);
    }
    if (t == RAY_LIST) {
        int64_t n = q_count(v);
        K x = shim_vec(0, n);
        if (!x) return (K)0;
        ray_t** items = (ray_t**)ray_data(v);
        for (int64_t i = 0; i < n; i++) {
            K e = k_of_ray(items[i]);
            if (!e) { x->n = i; r0(x); return (K)0; }
            kK(x)[i] = e;
        }
        return x;
    }

    /* A foreign hands back the SAME k0 every crossing — the extension's own object, not a view of
     * it — so `kK(x)[1]` is the pointer it stored and identity survives the round trip. */
    if (t == RAY_QFN || t == RAY_LAMBDA || t == RAY_UNARY || t == RAY_BINARY || t == RAY_VARY) {
        void* obj = NULL;
        if (t == RAY_QFN && q_eval_apply_foreign_parts(v, &obj)) return r1((K)obj);
        I qt = q_builtins_type_num(v);   /* lambda 100h, projection 104h, derived 106h+, … */
        if (qt < 100 || qt > 111) return krr((S) "nyi");
        K x = shim_vec(0, 1);
        if (!x) return (K)0;
        x->t = (signed char)qt;
        x->u = K_UFN;
        ray_retain(v);
        kK(x)[0] = (K)v;
        return x;
    }

    /* physical STR is q-invisible: a char vector is what crosses (string-C3) */
    if (t == -RAY_STR) return kpn((S)ray_str_ptr(v), (J)ray_str_len(v));

    if (t < 0) {                                       /* atom */
        I tag = (I)(-t);
        if (tag == RAY_SYM) return ks(sym_cstr(v->i64));
        if (tag == RAY_F32) { K x = shim_atom(-8); if (x) x->e = (E)v->f64; return x; }
        if (RAY_IS_TEMPORAL32(tag)) { K x = shim_atom(t); if (x) x->i = (I)v->i64; return x; }
        if (!k_atom_overlays(tag) && tag != RAY_ENUM) return krr((S) "nyi");
        ray_retain(v);
        return k_view(v);
    }

    if (t == RAY_SYM) {                                /* symbol vector: ids -> stable char* */
        int64_t n = q_count(v);
        K x = shim_vec(11, n);
        if (!x) return (K)0;
        x->u = (C)q_attr_byte(v);
        for (int64_t i = 0; i < n; i++) {
            int64_t id = ray_read_sym(ray_data(v), i, RAY_SYM, v->attrs);
            kS(x)[i] = sym_cstr(id);
        }
        return x;
    }

    if (k_tag_overlays(t) || t == RAY_ENUM) {
        if (v->attrs & RAY_ATTR_SLICE) {               /* kG(x) must be inline: materialise */
            ray_t* c = ray_vec_from_raw(t == RAY_ENUM ? RAY_I64 : t, ray_data(v), q_count(v));
            if (t == RAY_ENUM) c = q_enum_stamp(c, q_enum_domain(v));
            if (!c || RAY_IS_ERR(c)) { if (c) ray_error_free(c); return (K)0; }
            return k_view(c);
        }
        ray_retain(v);
        return k_view(v);
    }

    return krr((S) "nyi");                             /* lambdas, verbs, physical STR columns */
}

/* ===== K -> ray ============================================================================ */

/* Hands back an OWNED ray value and does NOT consume x.  An overlay is taken by retain (zero copy);
 * every shim shape is rebuilt. */
static ray_t* k_to_ray(K x) {
    if (!x) return q_err(QE_TYPE);
    if (!k_is_shim(x)) {
        /* a changed `u` REPLACES the attribute (0 clears it), trusted as the IPC decoder trusts a byte */
        ray_t* v = RAY_OF(x);
        uint8_t b = (uint8_t)x->u;
        if (b > 4 || b == q_attr_byte(v)) { ray_retain(v); return v; }
        return q_attr_stamp_byte(ray_cow(ray_attr_drop_fn(v)), b);
    }

    I t = x->t;
    if (k_is_qfn(x)) { ray_t* v = (ray_t*)kK(x)[0]; ray_retain(v); return v; }
    if (t == 112) {
        if (!k_is_foreign(x)) return q_err(QE_TYPE);   /* 112h with no (destructor;payload) pair */
        ray_t* c = q_eval_apply_foreign_new(r1(x));
        if (RAY_IS_ERR(c)) r0(x);
        return c;
    }
    if (t == 101) return RAY_NULL_OBJ;
    if (t == -128) return q_err_from_text(x->s ? x->s : "", x->s ? strlen(x->s) : 0);
    if (t == -11) return ray_sym(sym_id(x->s));
    if (t == -8)  return ray_f32(x->e);
    if (t == -13) return ray_month(x->i);      /* `i` is signed, so the widening sign-extends */
    if (t == -14) return ray_date(x->i);
    if (t == -17) return ray_minute(x->i);
    if (t == -18) return ray_second(x->i);
    if (t == -19) return ray_time(x->i);

    if (t == 98 || t == 99) {
        /* a spine is only ever ours to read (xD/xT build shim k0s; an overlay dict holds ray_t*
         * where kK would read K), so an inner spine that is not one is a malformed return */
        if (t == 98 && !k_is_shim(x->k)) return q_err(QE_TYPE);
        K keys = kd_keys(x), vals = kd_vals(x);
        if (!keys || !vals) return q_err(QE_TYPE);
        if (t == 98) {
            if (keys->t != 11 || vals->t != 0 || keys->n != vals->n) return q_err(QE_TYPE);
            ray_t* tbl = ray_table_new(keys->n);
            if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_OOM);
            for (J i = 0; i < keys->n; i++) {
                ray_t* col = k_to_ray(kK(vals)[i]);
                if (!col || RAY_IS_ERR(col)) { ray_release(tbl); return col ? col : q_err(QE_TYPE); }
                tbl = ray_table_add_col(tbl, sym_id(kS(keys)[i]), col);
                ray_release(col);
                if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_OOM);
            }
            return tbl;
        }
        ray_t* k_ = k_to_ray(keys);
        if (!k_ || RAY_IS_ERR(k_)) return k_ ? k_ : q_err(QE_TYPE);
        ray_t* v_ = k_to_ray(vals);
        if (!v_ || RAY_IS_ERR(v_)) { ray_release(k_); return v_ ? v_ : q_err(QE_TYPE); }
        return ray_dict_new(k_, v_);                   /* consumes both */
    }

    if (x->n < 0) return q_err(QE_TYPE);               /* whatever the extension put in n */

    if (t == 0) {
        ray_t* l = ray_list_new(x->n > 0 ? x->n : 1);
        if (!l || RAY_IS_ERR(l)) return l ? l : q_err(QE_OOM);
        for (J i = 0; i < x->n; i++) {
            ray_t* e = k_to_ray(kK(x)[i]);
            if (!e || RAY_IS_ERR(e)) { ray_release(l); return e ? e : q_err(QE_TYPE); }
            l = ray_list_append(l, e);
            ray_release(e);
            if (!l || RAY_IS_ERR(l)) return l ? l : q_err(QE_OOM);
        }
        return l;   /* a 0h list stays 0h — collapsing is what `vk` is for (capiref.md), not this seam */
    }

    if (t == 11) {
        ray_t* v = ray_sym_vec_new(RAY_SYM_W64, x->n);
        if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_OOM);
        for (J i = 0; i < x->n; i++) {
            int64_t id = sym_id(kS(x)[i]);
            v = ray_vec_append(v, &id);
            if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_OOM);
        }
        return q_attr_stamp_byte(v, (uint8_t)x->u);
    }

    return q_err(QE_TYPE);
}

/* ===== the debug-build seam validator ====================================================== */

/* A cheap win over kx, invisible to an unmodified extension: every K crossing BACK is checked, so a
 * bad one raises 'kapi bad-return here instead of corrupting the heap several statements later.
 * Compiled out in release. */
#ifdef DEBUG
static int seam_ok(K x, int depth) {
    if (!x) return 0;
    if (depth > 16) return 1;   /* a depth CAP on the walk, never a verdict: deep nesting is legal */
    I t = x->t;
    /* a carrier overlay (what `dl` hands back) reads its ray tag, which is no kdb tag at all */
    if (!k_is_shim(x) && RAY_OF(x)->type == RAY_QFN) return RAY_OF(x)->rc > 0;
    if (t == 112) return k_is_foreign(x) || k_is_qfn(x);
    if (k_is_qfn(x)) return ((ray_t*)kK(x)[0])->rc > 0;
    if (t != -128 && t != 101 && t != 98 && t != 99 && !k_tag_vec(t < 0 ? -t : t))
        return 0;
    if (k_is_shim(x)) {
        if (x->r < 0) return 0;
        if (t >= 0 && t != 98 && t != 99 && x->n < 0) return 0;
        if (t == 0)  for (J i = 0; i < x->n; i++) if (!seam_ok(kK(x)[i], depth + 1)) return 0;
        if (t == 98) return seam_ok(x->k, depth + 1);
        if (t == 99) return seam_ok(kK(x)[0], depth + 1) && seam_ok(kK(x)[1], depth + 1);
        return 1;
    }
    ray_t* v = RAY_OF(x);
    if (v->rc == 0 || v->rc > (1u << 30)) return 0;           /* freed, or not a live header */
    if (v->type != (int8_t)t) return 0;
    if (t >= 0) {                                             /* n must fit the block we allocated */
        if (v->len < 0) return 0;
        size_t cap = ((size_t)1 << v->order);
        if (v->mmod == 0 && (size_t)v->len * k_esz(t) + 32 > cap) return 0;
    }
    return 1;
}
#define SEAM_OK(x) seam_ok((x), 0)
#else
#define SEAM_OK(x) 1
#endif

/* ===== sd1 / sd0x — an extension fd on the runtime poll ==================================== */

typedef struct { I fd; int64_t id; } sd_reg_t;
static sd_reg_t g_sd[32];
static int      g_nsd;

#if defined(_WIN32)
/* Out of scope on Windows (owner ruling 2026-09-24): `d` there is a SOCKET, and handing one to the iocp
 * poll from an extension is unproven. */
K sd1(I d, K (*f)(I)) {
    (void)d; (void)f;
    return krr((S) "nyi");
}
#else
static ray_t* sd_readable(ray_poll_t* poll, ray_selector_t* sel) {
    (void)poll;
    K (*f)(I) = (K(*)(I))(uintptr_t)sel->data;
    if (!f) return NULL;
    K r = f((I)sel->fd);
    if (r) r0(r);          /* capiref.md sd1: a returned K is passed to r0 */
    g_kerr_set = 0;        /* an sd1 callback has no q frame to raise into */
    return NULL;
}

K sd1(I d, K (*f)(I)) {
    I fd = d < 0 ? -d : d;   /* capiref.md sd1: a negative d asks for a non-blocking socket */
    ray_poll_t* p = (ray_poll_t*)ray_runtime_get_poll();
    if (!p || g_nsd >= (int)(sizeof g_sd / sizeof g_sd[0])) { sd0x(fd, 1); return krr((S) "io"); }

    if (d < 0) {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    ray_poll_reg_t reg = { 0 };
    reg.fd = fd;
    reg.type = RAY_SEL_SOCKET;
    reg.data = (void*)(uintptr_t)f;
    reg.read_fn = sd_readable;

    int64_t id = ray_poll_register(p, &reg);
    if (id < 0) { sd0x(fd, 1); return krr((S) "io"); }
    g_sd[g_nsd].fd = fd;
    g_sd[g_nsd].id = id;
    g_nsd++;
    return ki(fd);           /* capiref.md: on success an integer K holding d */
}
#endif

V sd0x(I d, I f) {
    ray_poll_t* p = (ray_poll_t*)ray_runtime_get_poll();
    for (int i = 0; i < g_nsd; i++) {
        if (g_sd[i].fd != d) continue;
        if (p) ray_poll_deregister(p, g_sd[i].id);
        g_sd[i] = g_sd[--g_nsd];
        break;
    }
#if !defined(_WIN32)
    if (f) close(d);
#else
    (void)f;
#endif
}

V sd0(I d) { sd0x(d, 1); }

/* ===== k() — the extension calling back into q ============================================= */

/* THE one door back into q for k(), vak() and dot(): apply fn (borrowed) to the BORROWED args. */
static ray_t* k_apply(ray_t* fn, K* argv, int argc) {
    ray_t* ra[KAPI_MAX_RANK];
    int built = 0;
    for (; built < argc; built++) {
        ra[built] = k_to_ray(argv[built]);
        if (!ra[built] || RAY_IS_ERR(ra[built])) break;
    }
    ray_t* out;
    if (built < argc) {
        if (ra[built]) ray_error_free(ra[built]);
        out = q_err(QE_TYPE);
    } else {
        out = q_eval_apply_value(fn, ra, argc);
    }
    for (int i = 0; i < built; i++) ray_release(ra[i]);
    return out;
}

/* k()'s contract: evaluate the source text, then apply the result to any args */
static ray_t* k_eval(const char* s, K* argv, int argc) {
    ray_t* res = q_ctx_eval_src(s, strlen(s));
    if (argc == 0 || !res || RAY_IS_ERR(res)) return res;
    ray_t* out = k_apply(res, argv, argc);
    ray_release(res);
    return out;
}

/* A callback's error belongs to the extension that made it (capiref.md:398), so the console must not suspend
 * under the C frame: the same never-suspend region as @[;;] (Dgqrpb). */
static ray_t* k_trapped(const char* s, ray_t* fn, K* argv, int argc) {
    int64_t ctx = q_dbg_trap_enter();
    ray_t* r = fn ? k_apply(fn, argv, argc) : k_eval(s, argv, argc);
    q_dbg_trap_exit(ctx, r);
    return r;
}

/* a q error as the -128h object; consumes e */
static K k_err_of(ray_t* e) {
    K out = k_of_ray(e);
    ray_error_free(e);
    return out;
}

/* the `ee` convention: a q error becomes the pending krr and the answer is 0; consumes e */
static K k_pend(ray_t* e) {
    K x = k_err_of(e);
    krr(x && x->s ? x->s : (S) "error");
    r0(x);
    return (K)0;
}

/* an owned q result handed over as a K; consumes v */
static K k_take(ray_t* v, int ee_) {
    if (!v) return (K)0;
    if (RAY_IS_ERR(v)) return ee_ ? k_pend(v) : k_err_of(v);
    K out = k_of_ray(v);
    ray_release(v);
    return out;
}

K vak(I handle, const S s, va_list ap) {    /* own: takes ownership of references to its arguments */
    K argv[KAPI_MAX_RANK];
    int argc = 0;
    for (;;) {
        K a = va_arg(ap, K);
        if (!a) break;
        if (argc < KAPI_MAX_RANK) argv[argc++] = a;
    }
    /* capiref.md:400 — handle==0 "is valid only for a plugin, and executes against the kdb+ process
     * in which it is loaded".  A real handle is the IPC client, which is not served. */
    K out = handle != 0 ? krr((S) "nyi") : k_take(k_trapped(s ? s : "", NULL, argv, argc), 0);
    for (int i = 0; i < argc; i++) r0(argv[i]);
    return out;
}

K k(I handle, const S s, ...) {
    va_list ap;
    va_start(ap, s);
    K out = vak(handle, s, ap);
    va_end(ap);
    return out;
}

/* `.[x;y]`, borrowing both: capiref.md tags dot `ee` and never `own` */
K dot(K x, K y) {
    K argv[2] = { x, y };
    ray_t* apply = q_registry_lookup_name(".", 1, Q_DYADIC);   /* borrowed */
    return apply ? k_take(k_trapped(NULL, apply, argv, 2), 1) : krr((S) "nyi");
}

/* own: q's own collapse — like atoms to a vector, conforming dictionaries to a table (capiref.md vk) */
K vk(K x) {
    if (!x || x->t != 0) return x;
    ray_t* l = k_to_ray(x);
    r0(x);
    if (!l || RAY_IS_ERR(l)) return k_take(l, 0);
    ray_t* c = q_list_collapse(l);
    ray_release(l);
    return k_take(c, 0);
}

/* capiref.md `vi` serves the 77..97 mapped nested vectors, which peachq holds as 0h lists of vectors, so
 * a 0h list is what it reads — through q's own indexing, which is what gives an out-of-range j the null
 * of the first item's type. */
K vi(K x, UJ j) {
    if (!x || x->t != 0) return krr((S) "type");
    ray_t* l = k_to_ray(x);
    if (!l || RAY_IS_ERR(l)) return k_take(l, 0);
    ray_t* ix = ray_i64((int64_t)j);
    ray_t* e = q_eval_apply_value(l, &ix, 1);
    ray_release(ix);
    ray_release(l);
    return k_take(e, 0);
}

/* ===== b9 / d9 / okx — the -8!/-9! wire codec ============================================== */

/* ee.  Every mode unenumerates — the wire never carries an enum (kb/serialization.md:361) — and allows
 * timestamps.  None compresses, though capiref.md's table says 3 does: kxkdb's published suite has
 * b9(3,x) ~ -8!x for a 6026-byte frame that q_wire_compress takes to 90 (Dclgcn, the doc-vs-witness split). */
K b9(I mode, K x) {
    if (mode < -1 || mode > 6 || mode == 4) return krr((S) "type");
    ray_t* v = k_to_ray(x);
    if (!v || RAY_IS_ERR(v)) return k_take(v ? v : q_err(QE_TYPE), 1);
    ray_t* f = q_wire_serialize(v, Q_WIRE_ASYNC);
    ray_release(v);
    return k_take(f, 1);
}

static ray_t* k_frame_decode(K x, int* sent_err) {
    if (!x || x->t != 4) return q_err(QE_TYPE);
    ray_t* b = k_to_ray(x);
    if (!b || RAY_IS_ERR(b)) return b;
    ray_t* v = q_wire_deserialize_ex(b, sent_err);
    ray_release(b);
    return v;
}

K d9(K x) { return k_take(k_frame_decode(x, NULL), 1); }   /* ee; x is not modified */

/* "Decompressed data only" (capiref.md okx): a compressed frame is not a message this vouches for.  A
 * message whose payload is an error is valid, though it decodes to an error value. */
I okx(K x) {
    if (!x || x->t != 4 || x->n < 8 || kG(x)[2] != 0) return 0;
    int sent_err = 0;
    ray_t* v = k_frame_decode(x, &sent_err);
    if (!v) return 0;
    int ok = !RAY_IS_ERR(v) || sent_err;
    if (RAY_IS_ERR(v)) ray_error_free(v); else ray_release(v);
    return ok;
}

/* ===== `2:` — the loader =================================================================== */

typedef K (*F1)(K);
typedef K (*F2)(K, K);
typedef K (*F3)(K, K, K);
typedef K (*F4)(K, K, K, K);
typedef K (*F5)(K, K, K, K, K);
typedef K (*F6)(K, K, K, K, K, K);
typedef K (*F7)(K, K, K, K, K, K, K);
typedef K (*F8)(K, K, K, K, K, K, K, K);

/* capiref.md `dl`: a C function of rank n, wrapped as a q function.  The SAME Q_EVAL_CAR_KFN carrier
 * `2:` builds — from a pointer the extension already holds, so there is no library and no dlsym name
 * to record and both provenance slots stay the EMPTY symbol (owner ruling 2026-09-22).  The result
 * is a carrier OVERLAY: an extension that reads `xt` on it sees ray's own tag, not 112h — harmless,
 * because the one thing to do with a `dl` result is hand it back to q. */
K dl(V* f, J n) {
    if (!f || n < 1 || n > KAPI_MAX_RANK) return krr((S) "rank");
    int64_t none = ray_sym_intern_runtime("", 0);
    ray_t* c = q_eval_apply_kfn_new(f, n, none, none);
    if (!c || RAY_IS_ERR(c)) { if (c) ray_error_free(c); return krr((S) "wsfull"); }
    return K_OF(c);
}

ray_t* q_kapi_invoke(ray_t* carrier, ray_t** args, int64_t n) {
    void*   fn   = NULL;
    int64_t rank = 0, lib = 0, sym = 0;
    q_eval_apply_kfn_parts(carrier, &fn, &rank, &lib, &sym);
    if (n != rank) return q_err(QE_RANK);

    K a[KAPI_MAX_RANK] = { 0 };
    for (int64_t i = 0; i < n; i++) {
        g_kerr_set = 0;
        a[i] = k_of_ray(args[i]);
        if (!a[i]) {
            for (int64_t j = 0; j < i; j++) r0(a[j]);
            return g_kerr_set ? q_err_from_text(g_kerr, strlen(g_kerr)) : q_err(QE_TYPE);
        }
    }

    g_kerr_set = 0;
    K r = (K)0;
    kapi_frame fr = { .arg = a, .narg = n, .up = g_frame };
    for (int64_t i = 0; i < n; i++) fr.base[i] = k_refs(a[i]);
    g_frame = &fr;
    switch (rank) {
        case 1: r = ((F1)fn)(a[0]); break;
        case 2: r = ((F2)fn)(a[0], a[1]); break;
        case 3: r = ((F3)fn)(a[0], a[1], a[2]); break;
        case 4: r = ((F4)fn)(a[0], a[1], a[2], a[3]); break;
        case 5: r = ((F5)fn)(a[0], a[1], a[2], a[3], a[4]); break;
        case 6: r = ((F6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7: r = ((F7)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8: r = ((F8)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        default: break;
    }
    g_frame = fr.up;

    /* the module BORROWED them (capiref.md:103); a label it set on one it did not return is dropped */
    for (int64_t i = 0; i < n; i++) r0(a[i]);

    ray_t* out;
    if (!r) {                                   /* NULL + a pending krr is the error; NULL alone is `::` */
        out = g_kerr_set ? q_err_from_text(g_kerr, strlen(g_kerr)) : RAY_NULL_OBJ;
    } else if (!SEAM_OK(r)) {
        ray_t* ls = ray_sym_str(lib);
        ray_t* ss_ = ray_sym_str(sym);
        char msg[256];
        snprintf(msg, sizeof msg, "kapi bad-return: %.*s:%.*s",
                 ls ? (int)ray_str_len(ls) : 0, ls ? ray_str_ptr(ls) : "",
                 ss_ ? (int)ray_str_len(ss_) : 0, ss_ ? ray_str_ptr(ss_) : "");
        /* r and the frame's copies deliberately NOT freed: we just judged r malformed, and freeing it
         * is the corruption this check exists to stay out of */
        g_kerr_set = 0;
        free(fr.own);
        return q_err_from_text(msg, strlen(msg));
    } else {
        out = k_to_ray(r);
        r0(r);
    }
    g_kerr_set = 0;
    for (size_t i = 0; i < fr.nown; i++) r0(fr.own[i].k);
    free(fr.own);
    return out;
}

/* borrowed text of a symbol atom / char vector / char atom argument */
static int arg_text(ray_t* v, char* buf, size_t cap) {
    const char* p = NULL;
    size_t n = 0;
    if (!v) return 0;
    if (v->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(v->i64);
        if (!s) return 0;
        p = ray_str_ptr(s);
        n = ray_str_len(s);
    } else if (v->type == RAY_CHARV) {
        p = (const char*)ray_data(v);
        n = (size_t)q_count(v);
    } else if (v->type == -RAY_CHARV) {
        buf[0] = (char)v->u8;
        buf[1] = '\0';
        return 1;
    } else {
        return 0;
    }
    if (n >= cap) return 0;
    memcpy(buf, p, n);
    buf[n] = '\0';
    return 1;
}

#if defined(_WIN32)
#define KAPI_DLEXT ".dll"
#define KAPI_OSDIR "w64"
#elif defined(__APPLE__)
#define KAPI_DLEXT ".so"   /* a kdb extension is `.so` on macOS too */
#define KAPI_OSDIR "m64"
#else
#define KAPI_DLEXT ".so"
#define KAPI_OSDIR "l64"
#endif

/* A path that names a root or a drive takes no `$QHOME` or script-directory fallback. */
static int dl_rooted(const char* p) {
#if defined(_WIN32)
    if (p[0] == '\\' || (p[0] && p[1] == ':')) return 1;
#endif
    return p[0] == '/';
}

static const char* dl_lastsep(const char* p) {
    const char* s = strrchr(p, '/');
#if defined(_WIN32)
    const char* b = strrchr(p, '\\');
    if (b && (!s || b > s)) s = b;
#endif
    return s;
}

/* The candidate paths, in kdb's order (ref/dynamic-load.md) plus one: as given, then under
 * `$QHOME/os`, then beside the script that is loading — a deliberate SUPERSET, because portable code
 * must run on kx and never the reverse.  The error names every path tried, which kx does not. */
typedef struct { char p[4][1200]; int n; } dl_paths;

/* Every candidate is made ABSOLUTE, which is both what kdb does ("From 4.1t it resolves to an
 * absolute path only, without resolving sym-links" — ref/dynamic-load.md) and what makes a bare
 * `` `fixture `` work at all: dlopen (and LoadLibrary) reads a bare name as a library-SEARCH request,
 * which does not start in the current directory.  dl_brief puts the cwd prefix back for the error text. */
static void dl_add(dl_paths* c, const char* fmt, const char* a, const char* b) {
    if (c->n >= 4) return;
    char rel[600];
    snprintf(rel, sizeof rel, fmt, a, b);
#if defined(_WIN32)
    if (!q_io_abs_path(rel, c->p[c->n], sizeof c->p[0])) snprintf(c->p[c->n], sizeof c->p[0], "%s", rel);
#else
    char cwd[512];
    if (rel[0] == '/' || !getcwd(cwd, sizeof cwd)) snprintf(c->p[c->n], sizeof c->p[0], "%s", rel);
    else snprintf(c->p[c->n], sizeof c->p[0], "%s/%s", cwd, rel);
#endif
    for (int i = 0; i < c->n; i++)
        if (strcmp(c->p[i], c->p[c->n]) == 0) return;   /* a script beside the cwd names the cwd's path again */
    c->n++;
}

static void dl_candidates(const char* lib, dl_paths* c) {
    const char* p = lib[0] == ':' ? lib + 1 : lib;
    const char* base = dl_lastsep(p);
    base = base ? base + 1 : p;
    const char* ext = strchr(base, '.') ? "" : KAPI_DLEXT;

    c->n = 0;
    dl_add(c, "%s%s", p, ext);
    if (!dl_rooted(p)) {
        const char* qhome = getenv("QHOME");
        if (qhome) {
            char dir[576];
            snprintf(dir, sizeof dir, "%s/%s/%s", qhome, KAPI_OSDIR, p);
            dl_add(c, "%s%s", dir, ext);
        }
        int64_t ln = 0;
        ray_t* fs = ray_sym_str(q_dbg_statement_origin(&ln));
        if (fs && ray_str_len(fs) < 400) {
            char script[512];
            snprintf(script, sizeof script, "%.*s", (int)ray_str_len(fs), ray_str_ptr(fs));
            const char* slash = dl_lastsep(script);
            if (slash) {
                script[slash - script + 1] = '\0';
                char dir[576];
                snprintf(dir, sizeof dir, "%s%s", script, p);
                dl_add(c, "%s%s", dir, ext);
            }
        }
    }
}

/* A path as the ERROR names it: cwd-relative when it lies under the cwd.  The message lists every
 * candidate, and a `$QHOME` that happens to sit under the cwd would otherwise put this machine's
 * absolute build path into the text (and so into any transcript that records it). */
static const char* dl_brief(const char* p) {
    static char cwd[512];
#if defined(_WIN32)
    if (!q_io_abs_path(".", cwd, sizeof cwd)) return p;   /* spelled as dl_add spells a candidate */
#else
    if (!getcwd(cwd, sizeof cwd)) return p;
#endif
    size_t n = strlen(cwd);
    return (n > 1 && strncmp(p, cwd, n) == 0 && p[n] == '/') ? p + n + 1 : p;
}

typedef struct { char path[512]; void* h; } dlh_t;
static dlh_t g_dlh[32];
static int   g_ndlh;
/* Every function a `2:` resolved, by the lib and symbol text it was asked for with — what the wire names.  A decode
 * reuses the pointer rather than calling dlsym again (a GNU IFUNC resolver runs on every dlsym); two pointers under
 * one spelling (a relative lib loaded from two directories) leave it NULL, so neither is guessed. */
typedef struct { int64_t lib, sym; void* fn; } dl_fn_t;
static dl_fn_t* g_dlfn;
static int      g_ndlfn, g_dlfncap;

static void dl_note(int64_t lib, int64_t sym, void* fn) {
    for (int i = 0; i < g_ndlfn; i++)
        if (g_dlfn[i].lib == lib && g_dlfn[i].sym == sym) {
            if (g_dlfn[i].fn != fn) g_dlfn[i].fn = NULL;
            return;
        }
    if (g_ndlfn == g_dlfncap) {
        int nc = g_dlfncap ? 2 * g_dlfncap : 16;
        dl_fn_t* nt = (dl_fn_t*)realloc(g_dlfn, (size_t)nc * sizeof *nt);
        if (!nt) return;
        g_dlfn = nt;
        g_dlfncap = nc;
    }
    g_dlfn[g_ndlfn++] = (dl_fn_t){ lib, sym, fn };
}

static void* dl_open(const char* path) {
    for (int i = 0; i < g_ndlh; i++)
        if (strcmp(g_dlh[i].path, path) == 0) return g_dlh[i].h;
    void* h = q_dl_open(path);
    if (!h) return NULL;
    if (g_ndlh < (int)(sizeof g_dlh / sizeof g_dlh[0])) {
        snprintf(g_dlh[g_ndlh].path, sizeof g_dlh[g_ndlh].path, "%s", path);
        g_dlh[g_ndlh].h = h;
        g_ndlh++;
    }
    return h;
}

ray_t* q_dl_wrap(ray_t* x, ray_t* y) {
    char lib[512];
    if (!arg_text(x, lib, sizeof lib)) return q_err(QE_TYPE);
    if (!y || y->type != RAY_LIST || q_count(y) != 2) return q_err(QE_TYPE);

    ray_t** yy = (ray_t**)ray_data(y);
    char fname[192];
    if (!arg_text(yy[0], fname, sizeof fname)) return q_err(QE_TYPE);

    int64_t rank;
    switch (yy[1]->type) {
        case -RAY_I64: rank = yy[1]->i64; break;
        case -RAY_I32: rank = yy[1]->i32; break;
        case -RAY_I16: rank = yy[1]->i16; break;
        default: return q_err(QE_TYPE);
    }
    if (rank < 1 || rank > KAPI_MAX_RANK) return q_err(QE_RANK);

    dl_paths c;
    dl_candidates(lib, &c);
    void* h = NULL;
    for (int i = 0; i < c.n && !h; i++) h = dl_open(c.p[i]);
    if (!h) {
        const char* why = q_dl_unavailable();
        if (why) return q_err_from_text(why, strlen(why));
        char msg[640];
        int off = 0;
        for (int i = 0; i < c.n && off >= 0 && off < (int)sizeof msg; i++)
            off += snprintf(msg + off, sizeof msg - (size_t)off, i ? " %s" : "%s", dl_brief(c.p[i]));
        return q_err_from_text(msg, strlen(msg));
    }
    void* fn = q_dl_sym(h, fname);
    if (!fn) return q_err_name(fname, strlen(fname));

    int64_t lsym = ray_sym_intern_runtime(lib, strlen(lib));
    int64_t fsym = ray_sym_intern_runtime(fname, strlen(fname));
    dl_note(lsym, fsym, fn);
    return q_eval_apply_kfn_new(fn, rank, lsym, fsym);
}

ray_t* q_dl_loaded(const char* lib, size_t ln, const char* fn, size_t fnn, int64_t rank) {
    int64_t lsym = ray_sym_find(lib, ln), fsym = ray_sym_find(fn, fnn);
    if (rank < 1 || rank > KAPI_MAX_RANK || lsym < 0 || fsym < 0) return NULL;
    for (int i = 0; i < g_ndlfn; i++) {
        if (g_dlfn[i].lib != lsym || g_dlfn[i].sym != fsym) continue;
        if (!g_dlfn[i].fn) return NULL;
        ray_t* c = q_eval_apply_kfn_new(g_dlfn[i].fn, rank, g_dlfn[i].lib, g_dlfn[i].sym);
        if (c && RAY_IS_ERR(c)) { ray_error_free(c); return NULL; }
        return c;
    }
    return NULL;
}

/* The heap's RAY_QFN choke point (heap.c `ray_free_set_qfn_fin_fn`, the `ray_free_set_mapped_fn`
 * precedent): base cannot know a carrier kind, so the death of a foreign carrier comes back HERE to
 * drop the reference it holds on the extension's own k0 — which is where the destructor runs. */
static void kapi_qfn_fin(ray_t* v) {
    void* obj = NULL;
    if (q_eval_apply_foreign_parts(v, &obj) && obj) r0((K)obj);
}

void q_kapi_init(void) { ray_free_set_qfn_fin_fn(kapi_qfn_fin); }

void q_kapi_reset(void) {
    /* Ordering law: every foreign object must be DEAD before this runs, because its destructor is a
     * function pointer inside the loaded `.so` — q_runtime_destroy calls us after q_env_destroy.  The
     * `.so` itself never unmaps (q_dl_open), so a foreign that outlived
     * a runtime would still find its code; the hook it needs is what goes away here. */
    ray_free_set_qfn_fin_fn(NULL);
    ray_poll_t* p = (ray_poll_t*)ray_runtime_get_poll();
    for (int i = 0; i < g_nsd; i++)
        if (p) ray_poll_deregister(p, g_sd[i].id);
    g_nsd = 0;
    for (size_t i = 0; i < g_symcap; i++) free(g_symmirror[i]);
    free(g_symmirror);
    g_symmirror = NULL;
    g_symcap = 0;
    free(g_dlfn);
    g_dlfn = NULL;
    g_ndlfn = g_dlfncap = 0;
    g_kerr_set = 0;
}
