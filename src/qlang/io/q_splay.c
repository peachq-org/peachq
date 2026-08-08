/* q_splay — see q_splay.h.  Open splays keyed by the `:dir/ handle sym (the
 * q_handles.c pattern): open reads .d + column HEADERS and binds the domain;
 * data moves on gather — fixed columns mmap fresh per touch (mmod==3,
 * rc-driven), the rest decode once and cache.  Single-threaded q layer. */
#define _GNU_SOURCE            /* MAP_ANONYMOUS / MAP_FIXED */
#include "qlang/io/q_splay.h"
#include "qlang/io/q_provider.h" /* marker-first disambiguation: `:pq: is never a splay */
#include "qlang/io/q_io.h"           /* q_io_file_path */
#include "qlang/net/q_wirefile.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"       /* int idx accessors — zip block coverage */
#include "qlang/q_prim.h"            /* q_typed_empty_like — the gather's empty law */
#include "qlang/ops/q_index.h"       /* q_index_at — the one gather home */
#include "qlang/q_env.h"             /* q_env_set — the one global-set home */
#include "lang/eval.h"               /* ray_eval_get_restricted */
#include "table/sym.h"               /* ray_sym_intern_runtime, ray_sym_str */
#include "mem/heap.h"                /* attrs + ray_free_set_mapped_fn */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

typedef struct {
    int64_t     name;     /* column sym id */
    q_wf_colhdr h;
    ray_t*      cached;   /* owned decoded column (decode lane) */
} splay_col;

typedef struct {
    int64_t    sym;       /* the `:dir/ handle sym — carrier value + key */
    ray_t*     dir;       /* owned RAY_STR path, trailing slash kept */
    ray_t*     keys;      /* owned .d sym vector */
    splay_col* cols;
    int64_t    ncols;
    ray_t*     domain;    /* owned loaded enum domain, or NULL */
    char       domname[256];
} splay_ent;

static splay_ent* g_ents = NULL;
static int64_t    g_n = 0, g_cap = 0;

/* ONE reservation: a guard block whose 16-byte TAIL carries the ray_t header's
 * front, payload at base+guard — so kdb's byte-16 payload lands on header+32.
 * `view` is the separately-unmapped Windows file view (POSIX leaves it NULL). */
typedef struct { uint8_t* base; void* view; size_t guard, total; } splay_vm;

/* THE unmap choke point's ledger: one row per live mapped column. */
typedef struct { ray_t* hdr; splay_vm vm; } splay_region;
static splay_region* g_regs = NULL;
static int64_t g_regn = 0, g_regcap = 0;
static int64_t g_mapped_bytes = 0;
static int64_t g_reg_created = 0, g_reg_freed = 0;
static int64_t g_zblocks = 0;   /* cumulative blocks inflated — the laziness witness */

static ray_t* g_colref_mark = NULL;   /* address-identity marker, per runtime */

/* ---- the VM seam: the ONLY platform mechanics under the shared contract ----
 * _mapfile lays `need` bytes of `path` copy-on-write after the guard (the file
 * lane); _reserve/_commit take the whole plain size and materialize only a
 * gather's blocks (the region lane); _release serves both, through the choke
 * point below.  Each returns NULL or an owned error. */
#ifdef RAY_OS_WINDOWS

static size_t splay_gran(void) {
    SYSTEM_INFO si; GetSystemInfo(&si); return (size_t)si.dwAllocationGranularity;
}
static size_t splay_page(void) {
    SYSTEM_INFO si; GetSystemInfo(&si); return (size_t)si.dwPageSize;
}

static ray_t* splay_vm_mapfile(const char* path, size_t need, splay_vm* r) {
    HANDLE fh = CreateFileA(path, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return q_err(QE_IO);
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(fh, &sz) || sz.QuadPart < 0 || (uint64_t)sz.QuadPart < need) {
        CloseHandle(fh);
        return q_err(QE_CORRUPT);
    }
    HANDLE mh = CreateFileMappingA(fh, NULL, PAGE_WRITECOPY, 0, 0, NULL);
    CloseHandle(fh);
    if (!mh) return q_err(QE_IO);
    /* A view's address must be allocation-granular, so the guard is a whole
     * granule and the placement is reserve-probe / release / re-take: nothing
     * can steal the run in a single-threaded runtime, and a lost race only
     * costs a retry. */
    size_t g = splay_gran();
    r->guard = g;
    r->total = g + ((need + g - 1) / g) * g;
    r->base = NULL; r->view = NULL;
    for (int i = 0; i < 8 && !r->view; i++) {
        uint8_t* p = (uint8_t*)VirtualAlloc(NULL, r->total, MEM_RESERVE, PAGE_NOACCESS);
        if (!p) break;
        /* a release that FAILS leaves a run this loop can no longer name —
         * retrying past it only strands more address space */
        if (!VirtualFree(p, 0, MEM_RELEASE)) break;
        if (!VirtualAlloc(p, g, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) continue;
        r->view = MapViewOfFileEx(mh, FILE_MAP_COPY, 0, 0, need, p + g);
        if (r->view) r->base = p;
        else if (!VirtualFree(p, 0, MEM_RELEASE)) break;
    }
    CloseHandle(mh);
    return r->view ? NULL : q_err(QE_OOM);
}

static ray_t* splay_vm_reserve(size_t need, splay_vm* r) {
    size_t g = splay_page();
    r->guard = g;
    r->total = g + ((need + g - 1) / g) * g;
    r->view = NULL;
    r->base = (uint8_t*)VirtualAlloc(NULL, r->total, MEM_RESERVE, PAGE_READWRITE);
    /* the header STRADDLES guard|payload, so the first payload page is
     * committed up front — every other page waits for its block */
    if (r->base && !VirtualAlloc(r->base, 2 * g, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(r->base, 0, MEM_RELEASE);
        r->base = NULL;
    }
    return r->base ? NULL : q_err(QE_OOM);
}

static ray_t* splay_vm_commit(const splay_vm* r, size_t off, size_t len) {
    return VirtualAlloc(r->base + r->guard + off, len, MEM_COMMIT, PAGE_READWRITE)
           ? NULL : q_err(QE_OOM);
}

static void splay_vm_release(const splay_vm* r) {
    if (r->view) UnmapViewOfFile(r->view);
    if (r->base) VirtualFree(r->base, 0, MEM_RELEASE);
}

#else

static ray_t* splay_vm_reserve(size_t need, splay_vm* r) {
    size_t g = (size_t)sysconf(_SC_PAGESIZE);
    r->guard = g;
    r->total = g + ((need + g - 1) / g) * g;
    r->view = NULL;
    r->base = (uint8_t*)mmap(NULL, r->total, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r->base == MAP_FAILED) { r->base = NULL; return q_err(QE_OOM); }
    return NULL;
}

/* anonymous pages are already backed — the region lane's commit is a no-op */
static ray_t* splay_vm_commit(const splay_vm* r, size_t off, size_t len) {
    (void)r; (void)off; (void)len;
    return NULL;
}

static void splay_vm_release(const splay_vm* r) { munmap(r->base, r->total); }

static ray_t* splay_vm_mapfile(const char* path, size_t need, splay_vm* r) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return q_err(QE_IO);
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < need) {
        close(fd);
        return q_err(QE_CORRUPT);
    }
    ray_t* bad = splay_vm_reserve(need, r);
    if (bad) { close(fd); return bad; }
    void* fm = mmap(r->base + r->guard, need, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_FIXED, fd, 0);
    close(fd);
    if (fm == MAP_FAILED) { splay_vm_release(r); return q_err(QE_IO); }
    return NULL;
}

#endif

/* mmod==3 death (installed on ray_free): rc hit zero — release the region. */
static void splay_region_free(ray_t* v) {
    for (int64_t i = 0; i < g_regn; i++) {
        if (g_regs[i].hdr != v) continue;
        splay_vm_release(&g_regs[i].vm);
        g_mapped_bytes -= (int64_t)g_regs[i].vm.total;
        g_reg_freed++;
        g_regs[i] = g_regs[--g_regn];   /* swap-remove */
        return;
    }
    assert(!"mmod==3 block with no splay region row");
}

void q_splay_init(void) {
    g_ents = NULL; g_n = 0; g_cap = 0;
    g_regs = NULL; g_regn = 0; g_regcap = 0;
    g_mapped_bytes = 0; g_reg_created = 0; g_reg_freed = 0;
    g_colref_mark = ray_i64(0x5153504c);
    ray_free_set_mapped_fn(splay_region_free);
}

void q_splay_destroy(void) {
    /* the env died first, so every mapped vector already released its region */
    assert(g_regn == 0 && g_reg_created == g_reg_freed);
    for (int64_t i = 0; i < g_regn; i++) splay_vm_release(&g_regs[i].vm);
    free(g_regs);
    g_regs = NULL; g_regn = 0; g_regcap = 0; g_mapped_bytes = 0;
    ray_free_set_mapped_fn(NULL);
    if (g_colref_mark) { ray_release(g_colref_mark); g_colref_mark = NULL; }
    for (int64_t i = 0; i < g_n; i++) {
        splay_ent* e = &g_ents[i];
        for (int64_t c = 0; c < e->ncols; c++)
            if (e->cols[c].cached) ray_release(e->cols[c].cached);
        free(e->cols);
        if (e->keys)   ray_release(e->keys);
        if (e->dir)    ray_release(e->dir);
        if (e->domain) ray_release(e->domain);
    }
    free(g_ents);
    g_ents = NULL; g_n = 0; g_cap = 0;
}

int64_t q_splay_mapped_bytes(void) { return g_mapped_bytes; }
int64_t q_splay_zblocks(void) { return g_zblocks; }

/* Reserve a ledger row up front: the row must exist BEFORE a header goes
 * mmod==3, or a release would find no row and the region would leak. */
static int splay_region_room(void) {
    if (g_regn < g_regcap) return 1;
    int64_t nc = g_regcap ? g_regcap * 2 : 16;
    splay_region* nr = (splay_region*)realloc(g_regs, (size_t)nc * sizeof *nr);
    if (!nr) return 0;
    g_regs = nr; g_regcap = nc;
    return 1;
}

static void splay_region_add(ray_t* hdr, const splay_vm* vm) {
    g_regs[g_regn].hdr = hdr;
    g_regs[g_regn].vm = *vm;
    g_regn++;
    g_mapped_bytes += (int64_t)vm->total;
    g_reg_created++;
}

static int64_t splay_find(int64_t sym) {
    for (int64_t i = 0; i < g_n; i++) if (g_ents[i].sym == sym) return i;
    return -1;
}

/* An overwrite of the directory makes the entry a LIE — drop it whole; the
 * next resolve re-opens from the files (the writer's one hook into here). */
void q_splay_invalidate(int64_t sym) {
    int64_t i = splay_find(sym);
    if (i < 0) return;
    splay_ent* e = &g_ents[i];
    for (int64_t c = 0; c < e->ncols; c++)
        if (e->cols[c].cached) ray_release(e->cols[c].cached);
    free(e->cols);
    if (e->keys)   ray_release(e->keys);
    if (e->dir)    ray_release(e->dir);
    if (e->domain) ray_release(e->domain);
    g_ents[i] = g_ents[--g_n];   /* swap-remove */
}

/* dir + column name as an owned NUL-terminated RAY_STR path. */
static ray_t* splay_col_path(splay_ent* e, int64_t name) {
    ray_t* nm = ray_sym_str(name);                      /* borrowed */
    if (!nm) return NULL;
    size_t dn = ray_str_len(e->dir), nn = ray_str_len(nm);
    char* buf = (char*)malloc(dn + nn + 1);
    if (!buf) return NULL;
    memcpy(buf, ray_str_ptr(e->dir), dn);
    memcpy(buf + dn, ray_str_ptr(nm), nn);
    buf[dn + nn] = '\0';
    ray_t* s = ray_str(buf, dn + nn);
    free(buf);
    return s;
}

static ray_t* splay_carrier(splay_ent* e) {
    ray_retain(e->keys);
    return ray_dict_new(e->keys, ray_sym(e->sym));      /* consumes both */
}

/* Marker-first recognition: the VALUE alone answers — n sym keys against one
 * `:…/ hsym atom, a shape ordinary `!` refuses.  No registry consult. */
int q_splay_is(ray_t* x) {
    if (!x || x->type != RAY_DICT) return 0;
    ray_t* k = ray_dict_keys(x);
    ray_t* v = ray_dict_vals(x);
    if (!k || !v || v->type != -RAY_SYM || k->type != RAY_SYM) return 0;
    ray_t* s = ray_sym_str(v->i64);                     /* borrowed */
    if (!s) return 0;
    size_t n = ray_str_len(s);
    const char* p = ray_str_ptr(s);
    if (q_provider_spec_is(p, n)) return 0;   /* `:pq:...:t/ is a PROVIDER carrier */
    return n >= 2 && p[0] == ':' && p[n - 1] == '/';
}

static ray_t* splay_open(int64_t sym, ray_t* dir, splay_ent** out);

/* Entry behind handle-sym `sym`: found, or opened from its own path (a cold
 * carrier — wire round-trip, fresh runtime — heals here).  NULL with *err
 * NULL = not a table folder; NULL with *err = the open's error. */
static splay_ent* splay_resolve_sym(int64_t sym, ray_t** err) {
    *err = NULL;
    int64_t i = splay_find(sym);
    if (i >= 0) return &g_ents[i];
    ray_t* hs = ray_sym(sym);
    ray_t* path = q_io_file_path(hs);
    ray_release(hs);
    if (!path) return NULL;
    const char* p = ray_str_ptr(path);
    size_t n = ray_str_len(path);
    struct stat st;
    if (!n || p[n - 1] != '/' || stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ray_release(path);
        return NULL;
    }
    char* dp = (char*)malloc(n + 3);
    if (!dp) { ray_release(path); *err = q_err(QE_OOM); return NULL; }
    memcpy(dp, p, n);
    memcpy(dp + n, ".d", 3);
    int has_d = stat(dp, &st) == 0 && S_ISREG(st.st_mode);
    free(dp);
    if (!has_d) { ray_release(path); return NULL; }     /* flat reader answers 'type */
    if (ray_eval_get_restricted()) { ray_release(path); *err = q_err(QE_ACCESS); return NULL; }
    splay_ent* e = NULL;
    *err = splay_open(sym, path, &e);                   /* consumes path */
    return e;
}

static splay_ent* splay_resolve(ray_t* car, ray_t** err) {
    *err = NULL;
    if (!q_splay_is(car)) return NULL;
    return splay_resolve_sym(ray_dict_vals(car)->i64, err);
}

static void splay_free_partial(splay_ent* e) {
    free(e->cols);
    if (e->keys) ray_release(e->keys);
    if (e->dir)  ray_release(e->dir);
}

static ray_t* splay_open(int64_t sym, ray_t* dir, splay_ent** out) {
    splay_ent e = {0};
    e.sym = sym;
    e.dir = dir;                                        /* takes the ref */
    size_t dn = ray_str_len(dir);
    char* dp = (char*)malloc(dn + 3);
    if (!dp) { splay_free_partial(&e); return q_err(QE_OOM); }
    memcpy(dp, ray_str_ptr(dir), dn);
    memcpy(dp + dn, ".d", 3);
    ray_t* dps = ray_str(dp, dn + 2);
    free(dp);
    if (!dps) { splay_free_partial(&e); return q_err(QE_OOM); }
    ray_t* dotd = q_wirefile_read_column(dps, NULL, NULL);
    ray_release(dps);
    if (!dotd || RAY_IS_ERR(dotd)) { splay_free_partial(&e); return dotd ? dotd : q_err(QE_IO); }
    if (dotd->type != RAY_SYM) { ray_release(dotd); splay_free_partial(&e); return q_err(QE_CORRUPT); }
    e.keys = dotd;
    e.ncols = ray_len(dotd);
    e.cols = (splay_col*)calloc((size_t)(e.ncols > 0 ? e.ncols : 1), sizeof(splay_col));
    if (!e.cols) { splay_free_partial(&e); return q_err(QE_OOM); }
    for (int64_t i = 0; i < e.ncols; i++) {
        int64_t id = ray_vec_get_sym_id(dotd, i);
        ray_t* nm = ray_sym_str(id);                    /* borrowed */
        /* a .d name is untrusted file content — a separator would escape the dir */
        if (!nm || !ray_str_len(nm) ||
            memchr(ray_str_ptr(nm), '/', ray_str_len(nm)) ||
            memchr(ray_str_ptr(nm), '\\', ray_str_len(nm))) {
            splay_free_partial(&e);
            return q_err(QE_CORRUPT);
        }
        e.cols[i].name = id;
        e.cols[i].h.count = -1;
        ray_t* cp = splay_col_path(&e, id);
        if (!cp) { splay_free_partial(&e); return q_err(QE_OOM); }
        ray_t* err = q_wirefile_probe(cp, &e.cols[i].h);
        ray_release(cp);
        if (err) { splay_free_partial(&e); return err; }
        /* known header counts must agree — failing beats a plausible short table */
        if (i && e.cols[i].h.count >= 0 && e.cols[0].h.count >= 0 &&
            e.cols[i].h.count != e.cols[0].h.count) {
            splay_free_partial(&e);
            return q_err(QE_CORRUPT);
        }
    }
    /* One domain per splay, loaded now so `sym` binds globally (recorded
     * get-time divergence); a load failure defers to first touch. */
    for (int64_t i = 0; i < e.ncols; i++) {
        if (!e.cols[i].h.is_enum || !e.cols[i].h.domain[0]) continue;
        ray_t* cp = splay_col_path(&e, e.cols[i].name);
        if (!cp) break;
        ray_t* dom = q_wirefile_domain(cp, e.cols[i].h.domain);
        ray_release(cp);
        if (dom && !RAY_IS_ERR(dom)) {
            e.domain = dom;
            strcpy(e.domname, e.cols[i].h.domain);
            int64_t ds = ray_sym_intern_runtime(e.domname, strlen(e.domname));
            (void)q_env_set(ds, dom);                   /* env retains */
        } else if (dom) {
            ray_error_free(dom);                        /* deferral must not leak it */
        }
        break;
    }
    if (g_n == g_cap) {
        int64_t nc = g_cap ? g_cap * 2 : 8;
        splay_ent* ne = (splay_ent*)realloc(g_ents, (size_t)nc * sizeof *ne);
        if (!ne) {
            if (e.domain) ray_release(e.domain);
            splay_free_partial(&e);
            return q_err(QE_OOM);
        }
        g_ents = ne; g_cap = nc;
    }
    g_ents[g_n] = e;
    *out = &g_ents[g_n++];
    return NULL;
}

ray_t* q_splay_get(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return NULL;
    ray_t* err = NULL;
    splay_ent* e = splay_resolve_sym(x->i64, &err);
    if (err) return err;
    return e ? splay_carrier(e) : NULL;
}

/* HAS_NULLS is PESSIMISTIC on sentinel-capable types: consumers read it as
 * "may contain" (fast paths decline); only a CLEAR flag over hidden sentinels
 * corrupts reductions (2026-07-28) — a no-read open cannot scan. */
static int splay_nullable(int8_t t) {
    switch ((ray_type_e)t) {
    case RAY_I16: case RAY_I32: case RAY_I64: case RAY_F32: case RAY_F64:
    RAY_TEMPORAL32_CASES: RAY_TEMPORAL64_CASES: RAY_TEMPORALF_CASES:
        return 1;
    default:
        return 0;
    }
}

/* Lay the ray_t header over the guard's tail so the payload becomes its
 * data[], and enrol the region — the ONE place a mapped column is born. */
static ray_t* splay_hdr_over(const splay_vm* vm, splay_col* c) {
    ray_t* v = (ray_t*)(vm->base + vm->guard - 16);     /* data[] lands on byte 16 */
    v->mmod  = 3;                                       /* rc-driven release */
    v->order = 0;
    v->type  = c->h.tag;
    v->attrs = (uint8_t)((c->h.disk_attr == 1 ? RAY_ATTR_SORTED : 0) |
                         (splay_nullable(c->h.tag) ? RAY_ATTR_HAS_NULLS : 0));
    v->rc    = 1;                                       /* the caller's ref */
    v->len   = c->h.count;
    splay_region_add(v, vm);
    return v;
}

/* Map a fixed-width column: kdb's payload starts at byte 16, a ray_t header
 * is 32 — the header overlays the guard's tail, the file's copy-on-write
 * pages follow it. */
static ray_t* splay_map(splay_ent* e, splay_col* c) {
    if (!splay_region_room()) return q_err(QE_OOM);
    ray_t* cp = splay_col_path(e, c->name);
    if (!cp) return q_err(QE_OOM);
    size_t need = 16 + (size_t)c->h.count * ray_type_sizes[(uint8_t)c->h.tag];
    splay_vm vm;
    ray_t* bad = splay_vm_mapfile(ray_str_ptr(cp), need, &vm);
    ray_release(cp);
    if (bad) return bad;
    return splay_hdr_over(&vm, c);
}

/* The KX compressed-read model: reserve the full plain size as an anonymous
 * region, inflate EXACTLY the blocks `idx` covers into their pages (NULL =
 * all), overlay the ray_t header last.  The region is the same kind of value
 * as a mapped column — one rc/munmap lifecycle, one ledger, and NOTHING is
 * retained between statements (kdb's "for the duration of that operation"). */
static ray_t* splay_zip_col(splay_ent* e, splay_col* c, ray_t* idx) {
    if (!splay_region_room()) return q_err(QE_OOM);
    ray_t* cp = splay_col_path(e, c->name);
    if (!cp) return q_err(QE_OOM);
    q_io_zipmap_t zm;
    ray_t* bad = q_io_zip_open(cp, &zm);
    if (bad) { ray_release(cp); return bad; }
    uint8_t esz = ray_type_sizes[(uint8_t)c->h.tag];
    int64_t unc = zm.uncompressed, bs = zm.block_size, nb = zm.num_blocks;
    if (unc < 16 || c->h.count < 0 || 16 + c->h.count * esz > unc) {
        q_io_zipmap_free(&zm); ray_release(cp);
        return q_err(QE_CORRUPT);
    }
    uint8_t* mark = (uint8_t*)calloc((size_t)nb, 1);
    splay_vm vm;
    bad = mark ? splay_vm_reserve((size_t)unc, &vm) : q_err(QE_OOM);
    if (bad) {
        free(mark); q_io_zipmap_free(&zm); ray_release(cp);
        return bad;
    }
    if (!idx) {
        memset(mark, 1, (size_t)nb);
    } else if (q_type_is_int_atom(idx) || q_type_is_int_vec(idx)) {
        int atom = q_type_is_int_atom(idx);
        int64_t m = atom ? 1 : ray_len(idx);
        for (int64_t j = 0; j < m; j++) {
            int64_t i = atom ? q_type_iatom_val(idx) : q_type_ivec_get(idx, j);
            if (i < 0 || i >= c->h.count) continue;     /* OOB gathers a null */
            int64_t lo = (16 + i * esz) / bs, hi = (16 + (i + 1) * esz - 1) / bs;
            for (int64_t b = lo; b <= hi && b < nb; b++) mark[b] = 1;
        }
    } else {
        memset(mark, 1, (size_t)nb);                    /* unknown idx shape: all */
    }
    for (int64_t k = 0; k < nb && !bad; k++) {
        if (!mark[k]) continue;
        int64_t plain = unc - k * bs;
        if (plain > bs) plain = bs;
        bad = splay_vm_commit(&vm, (size_t)(k * bs), (size_t)plain);
        if (!bad) bad = q_io_zip_block(cp, &zm, k, vm.base + vm.guard + k * bs,
                                       (size_t)plain);
        if (!bad) g_zblocks++;
    }
    free(mark);
    q_io_zipmap_free(&zm);
    ray_release(cp);
    if (bad) { splay_vm_release(&vm); return bad; }
    return splay_hdr_over(&vm, c);                      /* header AFTER the fills */
}

/* Column by index, gathered at idx (NULL = whole) — the lanes: decode cache,
 * a FRESH map per touch, or the zip region with only idx's blocks inflated
 * (kdb's default lifetime — a query's maps/regions die at statement end). */
static ray_t* splay_col_gather_i(splay_ent* e, int64_t i, ray_t* idx) {
    splay_col* c = &e->cols[i];
    ray_t* col = NULL;
    if (c->cached) {
        ray_retain(c->cached);
        col = c->cached;
    } else if (c->h.zipped && c->h.mappable && !c->h.is_enum) {
        col = splay_zip_col(e, c, idx);
    } else if (c->h.mappable && !c->h.zipped) {
        col = splay_map(e, c);
    }
    if (!col) {                                         /* decode lane, cached */
        ray_t* cp = splay_col_path(e, c->name);
        if (!cp) return q_err(QE_OOM);
        col = q_wirefile_read_column(cp, e->domain ? e->domname : NULL, e->domain);
        ray_release(cp);
        if (!col || RAY_IS_ERR(col)) return col ? col : q_err(QE_IO);
        c->cached = col;
        if (c->h.count < 0 && (ray_is_vec(col) || col->type == RAY_LIST))
            c->h.count = ray_len(col);
        ray_retain(col);
    }
    if (RAY_IS_ERR(col) || !idx) return col;
    ray_t* g = q_typed_empty_like(q_index_at(col, &idx, 1), col);
    ray_release(col);
    return g ? g : q_err(QE_TYPE);
}

ray_t* q_splay_gather(ray_t* car, int64_t name, ray_t* idx) {
    ray_t* err = NULL;
    splay_ent* e = splay_resolve(car, &err);
    if (!e) return err;                                 /* NULL = no such folder */
    for (int64_t i = 0; i < e->ncols; i++)
        if (e->cols[i].name == name) return splay_col_gather_i(e, i, idx);
    return NULL;
}

ray_t* q_splay_col(ray_t* car, int64_t sym) {
    return q_splay_gather(car, sym, NULL);
}

ray_t* q_splay_count(ray_t* car) {
    ray_t* err = NULL;
    splay_ent* e = splay_resolve(car, &err);
    if (!e) return err ? err : q_err(QE_TYPE);
    if (e->ncols == 0) return ray_i64(0);
    if (e->cols[0].h.count < 0) {                       /* zipped/shape-A first col */
        ray_t* v = splay_col_gather_i(e, 0, NULL);
        if (!v || RAY_IS_ERR(v)) return v ? v : q_err(QE_IO);
        int64_t n = ray_len(v);
        ray_release(v);
        if (e->cols[0].h.count < 0) e->cols[0].h.count = n;
    }
    return ray_i64(e->cols[0].h.count);
}

/* every column gathered at idx (NULL = whole) into a table, the row-count
 * agreement checked — the one loop rows/prefix/table all ride */
static ray_t* splay_rows_tbl(splay_ent* e, ray_t* idx) {
    ray_t* tbl = ray_table_new(e->ncols > 0 ? e->ncols : 1);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_OOM);
    int64_t rows = -1;
    for (int64_t i = 0; i < e->ncols; i++) {
        ray_t* col = splay_col_gather_i(e, i, idx);
        if (!col || RAY_IS_ERR(col)) {
            ray_release(tbl);
            return col ? col : q_err(QE_IO);
        }
        int64_t n = ray_len(col);
        if (rows < 0) rows = n;
        if (n != rows) {
            ray_release(col); ray_release(tbl);
            return q_err(QE_CORRUPT);
        }
        tbl = ray_table_add_col(tbl, e->cols[i].name, col);
        ray_release(col);
        if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_OOM);
    }
    return tbl;
}

/* THE row-access law (result rank follows index rank, the index home's law):
 * an int VECTOR (NULL = every row) answers the table of those rows, an int
 * ATOM the row as a dict — each column through the gather seam, so a zip
 * column inflates only the covering blocks. */
ray_t* q_splay_rows(ray_t* car, ray_t* idx) {
    ray_t* err = NULL;
    splay_ent* e = splay_resolve(car, &err);
    if (!e) return err ? err : q_err(QE_TYPE);
    if (!idx || !q_type_is_int_atom(idx)) return splay_rows_tbl(e, idx);
    ray_t* vals = ray_list_new(e->ncols > 0 ? e->ncols : 1);
    if (!vals || RAY_IS_ERR(vals)) return vals ? vals : q_err(QE_OOM);
    for (int64_t i = 0; i < e->ncols; i++) {
        ray_t* v = splay_col_gather_i(e, i, idx);
        if (!v || RAY_IS_ERR(v)) { ray_release(vals); return v ? v : q_err(QE_IO); }
        vals = ray_list_append(vals, v);
        ray_release(v);
        if (!vals || RAY_IS_ERR(vals)) return vals ? vals : q_err(QE_OOM);
    }
    ray_t* cv = q_list_collapse(vals);
    ray_release(vals);
    if (!cv || RAY_IS_ERR(cv)) return cv ? cv : q_err(QE_TYPE);
    ray_retain(e->keys);
    return ray_dict_new(e->keys, cv);                   /* consumes both */
}

/* First-k-rows table (k < 0 = every row) — display's row budget rides this. */
ray_t* q_splay_prefix(ray_t* car, int64_t k) {
    if (k < 0) return q_splay_rows(car, NULL);
    ray_t* idx = ray_vec_new(RAY_I64, k > 0 ? k : 1);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_OOM);
    for (int64_t i = 0; i < k; i++) idx = ray_vec_append(idx, &i);
    if (!idx || RAY_IS_ERR(idx)) return idx ? idx : q_err(QE_OOM);
    ray_t* tbl = q_splay_rows(car, idx);
    ray_release(idx);
    return tbl;
}

ray_t* q_splay_table(ray_t* car) {
    return q_splay_prefix(car, -1);
}

int64_t q_splay_ncols(ray_t* car) {
    ray_t* err = NULL;
    splay_ent* e = splay_resolve(car, &err);
    if (err) ray_error_free(err);
    return e ? e->ncols : -1;
}

int64_t q_splay_col_sym(ray_t* car, int64_t i) {
    ray_t* err = NULL;
    splay_ent* e = splay_resolve(car, &err);
    if (err) ray_error_free(err);
    return e && i >= 0 && i < e->ncols ? e->cols[i].name : -1;
}

const q_wf_colhdr* q_splay_col_hdr(ray_t* car, int64_t i) {
    ray_t* err = NULL;
    splay_ent* e = splay_resolve(car, &err);
    if (err) ray_error_free(err);
    return e && i >= 0 && i < e->ncols ? &e->cols[i].h : NULL;
}

/* ---- the lazy column reference (the funsql gather seam's thunk) ---------- */

ray_t* q_splay_colref(ray_t* car, int64_t name, ray_t* idx) {
    ray_t* l = ray_list_new(4);
    if (!l || RAY_IS_ERR(l)) return l ? l : q_err(QE_OOM);
    l = ray_list_append(l, g_colref_mark);
    ray_t* nm = ray_sym(name);
    l = l && !RAY_IS_ERR(l) ? ray_list_append(l, car) : l;
    l = l && !RAY_IS_ERR(l) ? ray_list_append(l, nm) : l;
    l = l && !RAY_IS_ERR(l) ? ray_list_append(l, idx ? idx : RAY_NULL_OBJ) : l;
    ray_release(nm);
    return l ? l : q_err(QE_OOM);
}

int q_splay_colref_is(ray_t* v) {
    return v && g_colref_mark && v->type == RAY_LIST && ray_len(v) == 4 &&
           ((ray_t**)ray_data(v))[0] == g_colref_mark;
}

void q_splay_colref_parts(ray_t* v, ray_t** car, int64_t* name, ray_t** idx) {
    ray_t** e = (ray_t**)ray_data(v);
    *car = e[1];
    *name = e[2]->i64;
    *idx = e[3];
}
