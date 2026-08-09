/* q_ffi — the .ffi.i.* natives: the KX-ffikdb-compatible FFI core over the
 * vendored libffi (third_party/libffi; surface spec: the published ffikdb
 * reference, adopted Apache-2.0 per the ksql precedent — see lib/ffi.q).
 * The KX law kept verbatim: type LETTERS feed the CIF; VALUES marshal from
 * the ACTUAL q args — atoms via scratch slots, vectors as in-place data
 * pointers (C-side mutation is visible, the KX pointer contract), sym
 * vectors as char*[] with permutation writeback.  Recorded divergences,
 * each a compatible superset:
 *   - char vectors auto-NUL-terminate through a COPY unless the string
 *     already carries an explicit "\000" tail (an explicit tail keeps the
 *     in-place mutation contract, e.g. the sprintf buffer idiom);
 *   - the trailing (::) sentinel is accepted but not required when the
 *     bound arity disambiguates;
 *   - errors are bare q classes, never KX's embedded message strings.
 * Callbacks (the 'k' letter, tuple (fn;"argtypes";"ret")): a q lambda becomes
 * a C pointer via ffi_closure; the closure re-enters THE evaluator through
 * q_eval_apply_value (its own interrupt/depth guards apply).  The error
 * contract — deliberately NOT KX's longjmp-through-C: a q error inside a
 * callback PARKS as the pending error, this and every later invocation
 * inside the same foreign call return neutral zero results, the foreign
 * call completes normally, and the parked error then propagates as the
 * call's q result.  Closures fire only on the evaluator's own thread
 * (single-evaluator law); a foreign-thread invocation returns zeros without
 * touching q.  Closures and their cifs are permanent (KX parity). */
#define _GNU_SOURCE /* RTLD_DEFAULT */
#include "qlang/io/q_ffi.h"
#include "qlang/base/q_err.h"
#include "qlang/q_env.h"
#include "lang/env.h"    /* ray_fn_unary / ray_fn_vary */
#include "lang/eval.h"   /* RAY_FN_NONE */
#include "table/sym.h"   /* ray_sym_vec_cell, ray_read_sym/ray_write_sym */
#include <rayforce.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* RAY_FFI is granted by the Makefile host gate ONLY where the vendored
 * libffi objects actually link (linux x86-64 native + its win64 cross). */
#ifdef RAY_FFI

#include <ffi.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#endif
#include "qlang/eval/q_eval.h" /* q_eval_apply_value — THE value-apply seam */

static uintptr_t qffi_thread_id(void) {
#if defined(_WIN32)
    return (uintptr_t)GetCurrentThreadId();
#else
    return (uintptr_t)pthread_self();
#endif
}

enum { QFFI_MAX_ARGS = 32, QFFI_MAX_BINDS = 1024 };

typedef struct {
    ffi_cif   cif;
    ffi_type* atypes[QFFI_MAX_ARGS];
    char      letters[QFFI_MAX_ARGS];
    char      ret;
    unsigned  nargs;
    void*     fn;
} qffi_bind_t;

/* bindings are permanent (KX parity: bound functions live until exit) */
static qffi_bind_t* qffi_binds[QFFI_MAX_BINDS];
static int          qffi_nbinds;

static ffi_type* qffi_letter_type(char c, int is_ret) {
    if (c >= 'A' && c <= 'Z') return &ffi_type_pointer;
    switch (c) {
        case 'b': case 'c': case 'x': return &ffi_type_uint8;
        case 'h':                     return &ffi_type_sint16;
        case 'i': case 'm': case 'd':
        case 'u': case 'v': case 't': return &ffi_type_sint32;
        case 'j': case 'p': case 'n': return &ffi_type_sint64;
        case 'e':                     return &ffi_type_float;
        case 'f': case 'z':           return &ffi_type_double;
        case 'g': case 's':
        case 'r': case 'k':           return &ffi_type_pointer;
        case 'l': return sizeof(size_t) == 4 ? &ffi_type_sint32 : &ffi_type_sint64;
        case ' ': return is_ret ? &ffi_type_void : NULL;
        default:  return NULL;
    }
}

static char* qffi_strdup_n(const char* p, size_t n) {
    char* s = malloc(n + 1);
    if (s) { memcpy(s, p, n); s[n] = '\0'; }
    return s;
}

/* sym atom id -> owned NUL-terminated copy */
static char* qffi_sym_cstr(int64_t id) {
    ray_t* s = ray_sym_str(id); /* borrowed interned string */
    if (!s) return qffi_strdup_n("", 0);
    return qffi_strdup_n(ray_str_ptr(s), ray_str_len(s));
}

static void* qffi_dlsym_name(ray_t* name, int* bad_shape) {
    char lib[512], fn[512];
    lib[0] = fn[0] = '\0';
    if (name->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(name->i64);
        if (!s || ray_str_len(s) >= sizeof(fn)) { *bad_shape = 1; return NULL; }
        memcpy(fn, ray_str_ptr(s), ray_str_len(s));
        fn[ray_str_len(s)] = '\0';
    } else if (name->type == RAY_SYM && ray_len(name) == 2) {
        for (int i = 0; i < 2; i++) {
            ray_t* s = ray_sym_vec_cell(name, i);
            char*  dst = i ? fn : lib;
            if (!s || ray_str_len(s) >= 512) { *bad_shape = 1; return NULL; }
            memcpy(dst, ray_str_ptr(s), ray_str_len(s));
            dst[ray_str_len(s)] = '\0';
        }
    } else {
        *bad_shape = 1;
        return NULL;
    }
#if defined(_WIN32)
    if (lib[0]) {
        HMODULE h = LoadLibraryA(lib);
        return h ? (void*)GetProcAddress(h, fn) : NULL;
    }
    static const char* wellknown[] = { NULL, "msvcrt.dll", "kernel32.dll" };
    for (size_t i = 0; i < sizeof wellknown / sizeof *wellknown; i++) {
        HMODULE h = wellknown[i] ? LoadLibraryA(wellknown[i]) : GetModuleHandleA(NULL);
        void*   p = h ? (void*)GetProcAddress(h, fn) : NULL;
        if (p) return p;
    }
    return NULL;
#else
    if (lib[0]) {
        void* h = dlopen(lib, RTLD_NOW | RTLD_NODELETE);
        return h ? dlsym(h, fn) : NULL;
    }
    return dlsym(RTLD_DEFAULT, fn);
#endif
}

/* ---- argument marshalling ---------------------------------------------- */

typedef union {
    uint8_t u8; int16_t i16; int32_t i32; int64_t i64;
    float f32; double f64; void* p;
} qffi_slot;

typedef struct { ray_t* vec; char** arr; int64_t* pos; int64_t n; } qffi_symarg;

struct qffi_cb_s;

typedef struct {
    void*        values[QFFI_MAX_ARGS];
    qffi_slot    slots[QFFI_MAX_ARGS];
    void*        owned[QFFI_MAX_ARGS * 2];
    int          nowned;
    qffi_symarg  syms[QFFI_MAX_ARGS];
    int          nsyms;
    /* closures built for THIS call, committed to the permanent registry only
     * once every argument marshalled (a later 'type must not leak them) */
    struct qffi_cb_s* cbs[QFFI_MAX_ARGS];
    void*        cb_closures[QFFI_MAX_ARGS];
    int          ncbs;
} qffi_ctx;

static void qffi_ctx_free(qffi_ctx* c) {
    for (int i = 0; i < c->nowned; i++) free(c->owned[i]);
    for (int i = 0; i < c->nsyms; i++) {
        qffi_symarg* sa = &c->syms[i];
        for (int64_t j = 0; j < sa->n; j++) free(sa->arr[sa->n + j]);
        free(sa->arr);
        free(sa->pos);
    }
}

/* KX qsort-on-symbols contract: the C side permutes an array of char*; map
 * each pointer back to the domain position it left with.  C-introduced
 * strings (pointers we never handed out) are left unmapped — a recorded gap. */
static void qffi_sym_writeback(qffi_symarg* sa) {
    void* data = ray_data(sa->vec);
    for (int64_t i = 0; i < sa->n; i++) {
        for (int64_t j = 0; j < sa->n; j++) {
            if (sa->arr[i] == sa->arr[sa->n + j]) {
                ray_write_sym(data, i, (uint64_t)sa->pos[j], RAY_SYM, sa->vec->attrs);
                break;
            }
        }
    }
}

static int qffi_is_callback_tuple(ray_t* a) {
    if (a->type != RAY_LIST || ray_len(a) < 2 || ray_len(a) > 3) return 0;
    ray_t* head = ((ray_t**)ray_data(a))[0];
    return head->type == RAY_QFN || head->type == RAY_LAMBDA ||
           head->type == RAY_UNARY || head->type == RAY_BINARY || head->type == RAY_VARY;
}

static ray_t* qffi_make_closure(qffi_ctx* c, ray_t* tuple, void** code_out);

static int qffi_atom_num(ray_t* a, int64_t* iv, double* fv, int* isf) {
    switch (a->type) {
        case -RAY_BOOL: case -RAY_BYTE_ONLY: case -RAY_CHARV: *iv = a->u8; *isf = 0; return 1;
        case -RAY_I16: *iv = a->i16; *isf = 0; return 1;
        case -RAY_I32: case -RAY_MONTH: case -RAY_DATE:
        case -RAY_MINUTE: case -RAY_SECOND: case -RAY_TIME: *iv = a->i32; *isf = 0; return 1;
        case -RAY_I64: case -RAY_TIMESTAMP: case -RAY_TIMESPAN: *iv = a->i64; *isf = 0; return 1;
        case -RAY_F32: case -RAY_F64: case -RAY_DATETIME: *fv = a->f64; *isf = 1; return 1;
        default: return 0;
    }
}

static ray_t* qffi_marshal_arg(qffi_ctx* c, int i, ray_t* a, char letter, ffi_type* ft) {
    qffi_slot* s = &c->slots[i];
    c->values[i] = s;
    if (letter == 'r') {
        if (a->type == -RAY_I64) s->p = (void*)(intptr_t)a->i64;
        else if (a->type == -RAY_I32) s->p = (void*)(intptr_t)a->i32;
        else return q_err(QE_TYPE);
        return NULL;
    }
    /* numeric atoms fill the slot at the CIF's width (bind "i" + a short atom
     * must not leave 2 garbage bytes — stricter than KX's raw-payload pass) */
    int64_t iv = 0; double fv = 0; int isf = 0;
    if (qffi_atom_num(a, &iv, &fv, &isf)) {
        if (ft == &ffi_type_uint8)  { s->u8  = (uint8_t)(isf ? (int64_t)fv : iv); return NULL; }
        if (ft == &ffi_type_sint16) { s->i16 = (int16_t)(isf ? (int64_t)fv : iv); return NULL; }
        if (ft == &ffi_type_sint32) { s->i32 = (int32_t)(isf ? (int64_t)fv : iv); return NULL; }
        if (ft == &ffi_type_sint64) { s->i64 = isf ? (int64_t)fv : iv; return NULL; }
        if (ft == &ffi_type_float)  { s->f32 = (float)(isf ? fv : (double)iv); return NULL; }
        if (ft == &ffi_type_double) { s->f64 = isf ? fv : (double)iv; return NULL; }
    }
    switch (a->type) {
        case -RAY_BOOL: case -RAY_BYTE_ONLY: case -RAY_CHARV: s->u8 = a->u8; return NULL;
        case -RAY_I16: s->i16 = a->i16; return NULL;
        case -RAY_I32: case -RAY_MONTH: case -RAY_DATE:
        case -RAY_MINUTE: case -RAY_SECOND: case -RAY_TIME:
            s->i32 = a->i32; return NULL;
        case -RAY_I64: case -RAY_TIMESTAMP: case -RAY_TIMESPAN:
            s->i64 = a->i64; return NULL;
        case -RAY_F32: s->f32 = (float)a->f64; return NULL;
        case -RAY_F64: case -RAY_DATETIME: s->f64 = a->f64; return NULL;
        case -RAY_SYM: {
            char* t = qffi_sym_cstr(a->i64);
            if (!t) return q_err(QE_WSFULL);
            c->owned[c->nowned++] = t;
            s->p = t;
            return NULL;
        }
        case RAY_NULL: s->p = NULL; return NULL;
        case RAY_CHARV: {
            int64_t  n = ray_len(a);
            uint8_t* d = ray_data(a);
            if (n > 0 && d[n - 1] == 0) { s->p = d; return NULL; } /* explicit "\000": in place */
            char* t = qffi_strdup_n((const char*)d, (size_t)n);    /* auto-terminate divergence */
            if (!t) return q_err(QE_WSFULL);
            c->owned[c->nowned++] = t;
            s->p = t;
            return NULL;
        }
        case RAY_SYM: {
            int64_t      n  = ray_len(a);
            qffi_symarg* sa = &c->syms[c->nsyms];
            sa->vec = a;
            sa->n   = n;
            sa->arr = calloc((size_t)(2 * n), sizeof(char*));
            sa->pos = calloc((size_t)n, sizeof(int64_t));
            if (!sa->arr || !sa->pos) { free(sa->arr); free(sa->pos); return q_err(QE_WSFULL); }
            for (int64_t j = 0; j < n; j++) {
                ray_t* cell = ray_sym_vec_cell(a, j);
                sa->pos[j] = ray_read_sym(ray_data(a), j, RAY_SYM, a->attrs);
                sa->arr[n + j] = cell ? qffi_strdup_n(ray_str_ptr(cell), ray_str_len(cell))
                                      : qffi_strdup_n("", 0);
                sa->arr[j] = sa->arr[n + j];
            }
            c->nsyms++;
            s->p = sa->arr;
            return NULL;
        }
        case RAY_LIST:
            if (qffi_is_callback_tuple(a)) return qffi_make_closure(c, a, &s->p);
            s->p = a; /* KX parity: an opaque mixed list crosses as its object pointer */
            return NULL;
        default:
            if (ray_is_vec(a)) { s->p = ray_data(a); return NULL; }
            return q_err(QE_TYPE);
    }
}

/* the inferred (callFunction) CIF applies C integer default promotions —
 * narrow ints ride sint32; floats stay UNpromoted (KX parity: promoting
 * would break prototyped float callees reached through callFunction) */
static ffi_type* qffi_type_of_value(ray_t* a) {
    switch (a->type) {
        case -RAY_BOOL: case -RAY_BYTE_ONLY: case -RAY_CHARV:
        case -RAY_I16: return &ffi_type_sint32;
        case -RAY_I32: case -RAY_MONTH: case -RAY_DATE:
        case -RAY_MINUTE: case -RAY_SECOND: case -RAY_TIME: return &ffi_type_sint32;
        case -RAY_I64: case -RAY_TIMESTAMP: case -RAY_TIMESPAN: return &ffi_type_sint64;
        case -RAY_F32: return &ffi_type_float;
        case -RAY_F64: case -RAY_DATETIME: return &ffi_type_double;
        default: return &ffi_type_pointer;
    }
}

/* ---- return wrapping ---------------------------------------------------- */

static ray_t* qffi_wrap_ret(char letter, void* storage) {
    if (letter >= 'A' && letter <= 'Z') {
        void* p = *(void**)storage;
        if (letter == 'C') {
            const char* q = p;
            return ray_charv(q ? q : "", q ? (int64_t)strlen(q) : 0);
        }
        if (!p) {
            switch (letter) {
                case 'H': return ray_typed_null(RAY_I16);
                case 'I': return ray_typed_null(RAY_I32);
                case 'J': return ray_typed_null(RAY_I64);
                case 'E': return ray_typed_null(RAY_F32);
                case 'F': case 'Z': return ray_typed_null(RAY_F64);
                case 'S': return ray_sym(ray_sym_intern_runtime("", 0));
                default:  return ray_u8(0);
            }
        }
        storage = p;
        letter  = (char)(letter - 'A' + 'a');
    }
    switch (letter) {
        case ' ': ray_retain(RAY_NULL_OBJ); return RAY_NULL_OBJ;
        case 'b': return ray_bool(*(uint8_t*)storage != 0);
        case 'x': case 'g': return ray_u8(*(uint8_t*)storage);
        case 'c': return ray_char(*(uint8_t*)storage);
        case 'h': return ray_i16(*(int16_t*)storage);
        case 'i': return ray_i32(*(int32_t*)storage);
        case 'm': return ray_month(*(int32_t*)storage);
        case 'd': return ray_date(*(int32_t*)storage);
        case 'u': return ray_minute(*(int32_t*)storage);
        case 'v': return ray_second(*(int32_t*)storage);
        case 't': return ray_time(*(int32_t*)storage);
        case 'j': return ray_i64(*(int64_t*)storage);
        case 'p': return ray_timestamp(*(int64_t*)storage);
        case 'n': return ray_timespan(*(int64_t*)storage);
        case 'e': return ray_f32(*(float*)storage);
        case 'f': return ray_f64(*(double*)storage);
        case 'z': return ray_datetime(*(double*)storage);
        case 'l': return sizeof(size_t) == 4 ? ray_i32(*(int32_t*)storage)
                                             : ray_i64(*(int64_t*)storage);
        case 'r': return ray_i64((int64_t)(intptr_t)*(void**)storage);
        case 's': {
            const char* q = *(char**)storage;
            if (!q) q = "";
            return ray_sym(ray_sym_intern_runtime(q, strlen(q)));
        }
        default: return q_err(QE_DOMAIN);
    }
}

/* ---- callbacks (ffi_closure over the value-apply seam) ------------------- */

typedef struct qffi_cb_s {
    ray_t*    fn; /* retained q function value — permanent with its closure */
    char      letters[QFFI_MAX_ARGS];
    char      ret;
    unsigned  nargs;
    ffi_cif   cif;
    ffi_type* atypes[QFFI_MAX_ARGS];
} qffi_cb_t;

static ray_t*    qffi_cb_err; /* parked callback error; save/restored per foreign call */
static uintptr_t qffi_main_thread;

/* global anchor for the permanent closures: their only other pointer lives
 * inside mmap'd closure pages LSan cannot scan */
static qffi_cb_t** qffi_cb_reg;
static int qffi_ncb, qffi_cb_cap;

/* forever-cache: sym id -> NUL copy (callback 's' returns need a stable char*) */
static struct qffi_symc { int64_t id; char* s; }* qffi_symc;
static int qffi_nsymc, qffi_symc_cap;

static const char* qffi_sym_interned_cstr(int64_t id) {
    for (int i = 0; i < qffi_nsymc; i++)
        if (qffi_symc[i].id == id) return qffi_symc[i].s;
    if (qffi_nsymc == qffi_symc_cap) {
        int cap = qffi_symc_cap ? 2 * qffi_symc_cap : 16;
        void* np = realloc(qffi_symc, (size_t)cap * sizeof *qffi_symc);
        if (!np) return "";
        qffi_symc = np;
        qffi_symc_cap = cap;
    }
    char* s = qffi_sym_cstr(id);
    if (!s) return "";
    qffi_symc[qffi_nsymc++] = (struct qffi_symc){ id, s };
    return s;
}

static int qffi_store_cb_ret(char letter, ffi_type* t, ray_t* r, void* out) {
    if (letter == 's') {
        if (r->type != -RAY_SYM) return 0;
        *(void**)out = (void*)qffi_sym_interned_cstr(r->i64);
        return 1;
    }
    int64_t iv = 0; double fv = 0; int isf = 0;
    if (!qffi_atom_num(r, &iv, &fv, &isf)) return 0;
    if (t == &ffi_type_float)   { *(float*)out  = (float)(isf ? fv : (double)iv); return 1; }
    if (t == &ffi_type_double)  { *(double*)out = isf ? fv : (double)iv; return 1; }
    if (t == &ffi_type_pointer) { *(void**)out  = (void*)(intptr_t)(isf ? (int64_t)fv : iv); return 1; }
    *(ffi_sarg*)out = (ffi_sarg)(isf ? (int64_t)fv : iv); /* integral: full ffi_arg width */
    return 1;
}

static void qffi_closure_entry(ffi_cif* cif, void* ret, void** argv, void* ud) {
    qffi_cb_t* cb = ud;
    if (cif->rtype != &ffi_type_void)
        memset(ret, 0, cif->rtype->size < sizeof(ffi_arg) ? sizeof(ffi_arg)
                                                          : cif->rtype->size);
    /* thread check FIRST: a foreign-thread invocation must not even READ
     * evaluator-owned globals (qffi_cb_err) */
    if (qffi_thread_id() != qffi_main_thread) return;
    if (qffi_cb_err) return; /* poisoned foreign call: stay neutral */
    ray_t*   qargs[QFFI_MAX_ARGS];
    unsigned n = cb->nargs;
    for (unsigned i = 0; i < n; i++) {
        qargs[i] = qffi_wrap_ret(cb->letters[i], argv[i]);
        if (RAY_IS_ERR(qargs[i])) {
            for (unsigned j = 0; j < i; j++) ray_release(qargs[j]);
            qffi_cb_err = qargs[i];
            return;
        }
    }
    ray_t* r = q_eval_apply_value(cb->fn, qargs, (int64_t)n);
    for (unsigned i = 0; i < n; i++) ray_release(qargs[i]);
    if (!r) { qffi_cb_err = q_err(QE_TYPE); return; }
    if (RAY_IS_ERR(r)) { qffi_cb_err = r; return; } /* park; zeros already stored */
    if (cb->ret != ' ' && !qffi_store_cb_ret(cb->ret, cif->rtype, r, ret))
        qffi_cb_err = q_err(QE_TYPE); /* unconvertible result must not fake success */
    ray_release(r);
}

static ray_t* qffi_make_closure(qffi_ctx* c, ray_t* tuple, void** code_out) {
    ray_t**     e  = (ray_t**)ray_data(tuple);
    int64_t     tn = ray_len(tuple);
    const char* lp;
    int64_t     ln;
    if (e[1]->type == RAY_CHARV)       { lp = ray_data(e[1]); ln = ray_len(e[1]); }
    else if (e[1]->type == -RAY_CHARV) { lp = (const char*)&e[1]->u8; ln = 1; }
    else return q_err(QE_TYPE);
    char rl = ' '; /* KX default: a 2-tuple callback returns void */
    if (tn == 3) {
        if (e[2]->type != -RAY_CHARV) return q_err(QE_TYPE);
        rl = (char)e[2]->u8;
    }
    if (ln > QFFI_MAX_ARGS || !qffi_letter_type(rl, 1)) return q_err(QE_TYPE);

    qffi_cb_t* cb = calloc(1, sizeof *cb);
    if (!cb) return q_err(QE_WSFULL);
    cb->ret   = rl;
    cb->nargs = (unsigned)ln;
    for (int64_t i = 0; i < ln; i++) {
        cb->letters[i] = lp[i];
        cb->atypes[i]  = qffi_letter_type(lp[i], 0);
        if (!cb->atypes[i]) { free(cb); return q_err(QE_TYPE); }
    }
    if (ffi_prep_cif(&cb->cif, FFI_DEFAULT_ABI, cb->nargs,
                     qffi_letter_type(rl, 1), cb->atypes) != FFI_OK) {
        free(cb);
        return q_err(QE_TYPE);
    }
    void*        code = NULL;
    ffi_closure* cl   = ffi_closure_alloc(sizeof(ffi_closure), &code);
    if (!cl || ffi_prep_closure_loc(cl, &cb->cif, qffi_closure_entry, cb, code) != FFI_OK) {
        if (cl) ffi_closure_free(cl);
        free(cb);
        return q_err(QE_OS);
    }
    cb->fn = e[0];
    ray_retain(cb->fn);
    c->cbs[c->ncbs]         = cb;
    c->cb_closures[c->ncbs] = cl;
    c->ncbs++;
    *code_out = code;
    return NULL;
}

static void qffi_cbs_rollback(qffi_ctx* c) {
    for (int i = 0; i < c->ncbs; i++) {
        ray_release(c->cbs[i]->fn);
        ffi_closure_free(c->cb_closures[i]);
        free(c->cbs[i]);
    }
    c->ncbs = 0;
}

/* commit before ffi_call: C may stash the pointer, so a committed closure is
 * permanent (KX parity), anchored globally where LSan can see it */
static void qffi_cbs_commit(qffi_ctx* c) {
    for (int i = 0; i < c->ncbs; i++) {
        if (qffi_ncb == qffi_cb_cap) {
            int   cap = qffi_cb_cap ? 2 * qffi_cb_cap : 16;
            void* np  = realloc(qffi_cb_reg, (size_t)cap * sizeof *qffi_cb_reg);
            if (np) { qffi_cb_reg = np; qffi_cb_cap = cap; }
        }
        if (qffi_ncb < qffi_cb_cap) qffi_cb_reg[qffi_ncb++] = c->cbs[i];
    }
    c->ncbs = 0;
}

/* ---- argument-list normalization ---------------------------------------- */

/* Strip the KX trailing (::) sentinel; a bare atom is a 1-arg call, a bare
 * (::) a 0-arg one.  `nargs` < 0 = arity unknown (callFunction). */
static ray_t* qffi_args_norm(ray_t* args, int64_t nargs,
                             ray_t** single, ray_t*** items, int64_t* n) {
    if (args->type == RAY_LIST) {
        *items = (ray_t**)ray_data(args);
        *n     = ray_len(args);
        if (*n > 0 && (*items)[*n - 1]->type == RAY_NULL) (*n)--;
        if (nargs >= 0) {
            while (*n > nargs && (*items)[*n - 1]->type == RAY_NULL) (*n)--;
            if (*n != nargs) return q_err(QE_RANK);
        }
        return NULL;
    }
    if (args->type == RAY_NULL) { *items = NULL; *n = 0; }
    else                        { *single = args; *items = single; *n = 1; }
    if (nargs >= 0 && *n != nargs) return q_err(QE_RANK);
    return NULL;
}

/* ---- natives ------------------------------------------------------------ */

static ray_t* qffi_bind_fn(ray_t** args, int64_t n) {
    if (n != 3) return q_err(QE_RANK);
    ray_t*      types = args[1];
    const char* lp;
    int64_t     ln;
    if (types->type == RAY_CHARV)       { lp = ray_data(types); ln = ray_len(types); }
    else if (types->type == -RAY_CHARV) { lp = (const char*)&types->u8; ln = 1; }
    else return q_err(QE_TYPE);
    if (args[2]->type != -RAY_CHARV) return q_err(QE_TYPE);
    char rl = (char)args[2]->u8;
    if (ln > QFFI_MAX_ARGS || !qffi_letter_type(rl, 1)) return q_err(QE_TYPE);
    if (qffi_nbinds >= QFFI_MAX_BINDS) return q_err(QE_LIMIT);

    int   bad = 0;
    void* fn  = qffi_dlsym_name(args[0], &bad);
    if (bad) return q_err(QE_TYPE);
    if (!fn) return q_err(QE_OS);

    qffi_bind_t* b = calloc(1, sizeof *b);
    if (!b) return q_err(QE_WSFULL);
    b->fn    = fn;
    b->ret   = rl;
    b->nargs = (unsigned)ln;
    for (int64_t i = 0; i < ln; i++) {
        b->letters[i] = lp[i];
        b->atypes[i]  = qffi_letter_type(lp[i], 0);
        if (!b->atypes[i]) { free(b); return q_err(QE_TYPE); }
    }
    if (ffi_prep_cif(&b->cif, FFI_DEFAULT_ABI, b->nargs,
                     qffi_letter_type(rl, 1), b->atypes) != FFI_OK) {
        free(b);
        return q_err(QE_TYPE);
    }
    qffi_binds[qffi_nbinds] = b;
    return ray_i64(qffi_nbinds++);
}

static ray_t* qffi_do_call(ffi_cif* cif, void* fn, const char* letters, char rl,
                           ray_t** items, int64_t argc) {
    qffi_ctx c = { .nowned = 0, .nsyms = 0, .ncbs = 0 };
    for (int64_t i = 0; i < argc; i++) {
        ray_t* e = qffi_marshal_arg(&c, (int)i, items[i], letters ? letters[i] : 0,
                                    cif->arg_types[i]);
        if (e) { qffi_cbs_rollback(&c); qffi_ctx_free(&c); return e; }
    }
    qffi_cbs_commit(&c);
    char retbuf[FFI_SIZEOF_ARG > 8 ? FFI_SIZEOF_ARG : 8];
    memset(retbuf, 0, sizeof retbuf);
    ray_t* saved = qffi_cb_err; /* save/restore: nested foreign calls stay independent */
    qffi_cb_err  = NULL;
    ffi_call(cif, FFI_FN(fn), retbuf, c.values);
    ray_t* cberr = qffi_cb_err;
    qffi_cb_err  = saved;
    if (cberr) { qffi_ctx_free(&c); return cberr; } /* owned error, propagated unmodified */
    for (int i = 0; i < c.nsyms; i++) qffi_sym_writeback(&c.syms[i]);
    qffi_ctx_free(&c);
    return qffi_wrap_ret(rl, retbuf);
}

static ray_t* qffi_call_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    int64_t h;
    if (args[0]->type == -RAY_I64)      h = args[0]->i64;
    else if (args[0]->type == -RAY_I32) h = args[0]->i32;
    else return q_err(QE_TYPE);
    if (h < 0 || h >= qffi_nbinds) return q_err(QE_DOMAIN);
    qffi_bind_t* b = qffi_binds[h];

    ray_t*  single;
    ray_t** items;
    int64_t argc;
    ray_t*  e = qffi_args_norm(args[1], (int64_t)b->nargs, &single, &items, &argc);
    if (e) return e;
    return qffi_do_call(&b->cif, b->fn, b->letters, b->ret, items, argc);
}

/* rtf = `fn | `lib`fn | (retchar; `fn-or-pair); default return letter 'i' */
static ray_t* qffi_parse_rtf(ray_t* rtf, char* rl, ray_t** name) {
    *rl = 'i';
    if (rtf->type == -RAY_SYM || rtf->type == RAY_SYM) { *name = rtf; return NULL; }
    if (rtf->type == RAY_LIST && ray_len(rtf) == 2) {
        ray_t** e = (ray_t**)ray_data(rtf);
        if (e[0]->type != -RAY_CHARV) return q_err(QE_TYPE);
        *rl   = (char)e[0]->u8;
        *name = e[1];
        if (!qffi_letter_type(*rl, 1)) return q_err(QE_TYPE);
        return NULL;
    }
    return q_err(QE_TYPE);
}

static ray_t* qffi_callfn_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    char   rl;
    ray_t* name;
    ray_t* e = qffi_parse_rtf(args[0], &rl, &name);
    if (e) return e;
    int   bad = 0;
    void* fn  = qffi_dlsym_name(name, &bad);
    if (bad) return q_err(QE_TYPE);
    if (!fn) return q_err(QE_OS);

    ray_t*  single;
    ray_t** items;
    int64_t argc;
    e = qffi_args_norm(args[1], -1, &single, &items, &argc);
    if (e) return e;
    if (argc > QFFI_MAX_ARGS) return q_err(QE_LIMIT);

    ffi_type* atypes[QFFI_MAX_ARGS];
    for (int64_t i = 0; i < argc; i++) atypes[i] = qffi_type_of_value(items[i]);
    ffi_cif cif;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)argc,
                     qffi_letter_type(rl, 1), atypes) != FFI_OK)
        return q_err(QE_TYPE);
    return qffi_do_call(&cif, fn, NULL, rl, items, argc);
}

static ray_t* qffi_cvar_fn(ray_t* x) {
    char   rl;
    ray_t* name;
    ray_t* e = qffi_parse_rtf(x, &rl, &name);
    if (e) return e;
    int   bad  = 0;
    void* addr = qffi_dlsym_name(name, &bad);
    if (bad) return q_err(QE_TYPE);
    if (!addr) return q_err(QE_OS);
    return qffi_wrap_ret(rl, addr);
}

static ray_t* qffi_errno_fn(ray_t* x) {
    int old = errno;
    if (x && x->type == -RAY_I32) errno = (int)x->i32;
    return ray_i32(old);
}

#else /* !RAY_FFI: hosts without a vendored libffi port answer 'nyi */

static ray_t* qffi_bind_fn(ray_t** args, int64_t n)   { (void)args; (void)n; return q_err(QE_NYI); }
static ray_t* qffi_call_fn(ray_t** args, int64_t n)   { (void)args; (void)n; return q_err(QE_NYI); }
static ray_t* qffi_callfn_fn(ray_t** args, int64_t n) { (void)args; (void)n; return q_err(QE_NYI); }
static ray_t* qffi_cvar_fn(ray_t* x)                  { (void)x; return q_err(QE_NYI); }
static ray_t* qffi_errno_fn(ray_t* x)                 { (void)x; return q_err(QE_NYI); }

#endif /* RAY_FFI */

static void qffi_bind_native_u(const char* name, ray_unary_fn fn) {
    ray_t* obj = ray_fn_unary(name, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

static void qffi_bind_native_v(const char* name, ray_vary_fn fn) {
    ray_t* obj = ray_fn_vary(name, RAY_FN_NONE, fn);
    q_env_bind(ray_sym_intern(name, strlen(name)), obj);
    ray_release(obj);
}

void q_ffi_register(void) {
#ifdef RAY_FFI
    qffi_main_thread = qffi_thread_id();
#endif
    qffi_bind_native_v(".ffi.i.bind",   qffi_bind_fn);
    qffi_bind_native_v(".ffi.i.call",   qffi_call_fn);
    qffi_bind_native_v(".ffi.i.callfn", qffi_callfn_fn);
    qffi_bind_native_u(".ffi.i.cvar",   qffi_cvar_fn);
    qffi_bind_native_u(".ffi.i.errno",  qffi_errno_fn);
}
