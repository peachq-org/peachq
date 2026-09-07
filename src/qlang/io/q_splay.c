/* q_splay — see q_splay.h.  Open splays keyed by the `:dir/ handle sym (the
 * q_handles.c pattern): open reads .d + column HEADERS and binds the domain;
 * get builds the table — fixed columns mmap (mmod==3, rc-driven), compressed
 * ones inflate a block on first touch (the fault handler), the rest decode.
 * Single-threaded q layer. */
#define _GNU_SOURCE            /* MAP_ANONYMOUS / MAP_FIXED */
#include "qlang/io/q_splay.h"
#include "qlang/io/q_provider.h" /* `:pq: is never a splay directory */
#include "qlang/io/q_io.h"           /* q_io_file_path */
#include "qlang/net/q_wirefile.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"      /* q_type_coord_mark — THE aux mark that says "flipped from a coordinate" */
#include "qlang/q_prim.h"            /* q_attr_stamp_trusted — the disk letter on a mapped header */
#include "qlang/q_env.h"             /* q_env_set — the one global-set home */
#include "lang/eval.h"               /* ray_eval_get_restricted */
#include "table/sym.h"               /* ray_sym_intern_runtime, ray_sym_str */
#include "mem/heap.h"                /* attrs + ray_free_set_mapped_fn */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

typedef struct {
    int64_t     name;     /* column sym id */
    q_wf_colhdr h;
} splay_col;

typedef struct {
    int64_t    sym;       /* the `:dir/ handle sym — the table's aux path + key */
    ray_t*     dir;       /* owned RAY_STR path, trailing slash kept */
    ray_t*     keys;      /* owned .d sym vector */
    splay_col* cols;
    int64_t    ncols;
    char       domname[256];   /* enum domain name ("" = no enum columns) */
} splay_ent;

static splay_ent* g_ents = NULL;
static int64_t    g_n = 0, g_cap = 0;

/* ONE reservation: a guard block whose 16-byte TAIL carries the ray_t header's
 * front, payload at base+guard — so kdb's byte-16 payload lands on header+32.
 * `view` is the separately-unmapped Windows file view (POSIX leaves it NULL). */
typedef struct { uint8_t* base; void* view; size_t guard, total; } splay_vm;

/* THE unmap choke point's ledger: one row per live mapped column.  A zip row keeps what inflate-on-touch needs for
 * the region's life (kx: two descriptors per compressed file, kb/file-compression.md:159). */
enum { ZB_NONE, ZB_DONE, ZB_TORN };
typedef struct {
    ray_t*        hdr;
    splay_vm      vm;
    int           fd;         /* < 0: a plain file map */
    q_io_zipmap_t zm;
    uint8_t*      scratch;
    uint8_t*      blk;        /* per block: ZB_* */
} splay_region;
static splay_region* g_regs = NULL;
static int64_t g_regn = 0, g_regcap = 0;
static int64_t g_mapped_bytes = 0;
static int64_t g_reg_created = 0, g_reg_freed = 0;
static int64_t g_zblocks = 0;   /* cumulative blocks inflated — the laziness witness */
static volatile sig_atomic_t g_zfault = 0;   /* a fault-time inflate failed since the last ask */

/* ---- the VM seam: the ONLY platform mechanics under the shared contract ----
 * _mapfile lays `need` bytes of `path` copy-on-write after the guard (the file
 * lane); _reserve/_arm/_commit take the whole plain size and materialize a
 * block at a time — the gather's, then whatever a touch lands in (the region
 * lane); _release serves both, through the choke point below. */
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

/* dwPageSize is the commit grain (the 64K figure is reservation granularity only), so a page-rounded range
 * commits, and arms by decommitting — a touch then raises an access violation into the vectored handler */
static int splay_vm_commit(const splay_vm* r, size_t off, size_t len) {
    return VirtualAlloc(r->base + r->guard + off, len, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
}

static void splay_vm_arm(const splay_vm* r, size_t off, size_t len) {
    size_t g = r->guard, lo = off & ~(g - 1), hi = (off + len + g - 1) & ~(g - 1);
    VirtualFree(r->base + r->guard + lo, hi - lo, MEM_DECOMMIT);
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

/* [off, off+len) page-rounded: arm = unreadable (a touch faults into the handler), commit = readable again.  The
 * mapping was charged read-write at creation, so a commit cannot fail for want of memory (file-compression.md:171). */
static void splay_vm_arm(const splay_vm* r, size_t off, size_t len) {
    size_t g = r->guard, lo = off & ~(g - 1), hi = (off + len + g - 1) & ~(g - 1);
    mprotect(r->base + r->guard + lo, hi - lo, PROT_NONE);
}

static int splay_vm_commit(const splay_vm* r, size_t off, size_t len) {
    size_t g = r->guard, lo = off & ~(g - 1), hi = (off + len + g - 1) & ~(g - 1);
    return mprotect(r->base + r->guard + lo, hi - lo, PROT_READ | PROT_WRITE);
}

static void splay_vm_release(const splay_vm* r) { if (r->base) munmap(r->base, r->total); }

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

static void splay_region_drop(splay_region* r) {
    splay_vm_release(&r->vm);
    if (r->fd >= 0) close(r->fd);
    q_io_zipmap_free(&r->zm);
    free(r->scratch);
    free(r->blk);
}

/* mmod==3 death (installed on ray_free): rc hit zero — release the region. */
static void splay_region_free(ray_t* v) {
    for (int64_t i = 0; i < g_regn; i++) {
        if (g_regs[i].hdr != v) continue;
        splay_region_drop(&g_regs[i]);
        g_mapped_bytes -= (int64_t)g_regs[i].vm.total;
        g_reg_freed++;
        g_regs[i] = g_regs[--g_regn];   /* swap-remove */
        return;
    }
    assert(!"mmod==3 block with no splay region row");
}

static int64_t splay_zip_plain(const splay_region* r, int64_t k, int64_t* off) {
    int64_t bs = r->zm.block_size, n = r->zm.uncompressed - k * bs;
    *off = k * bs;
    return n > bs ? bs : n;
}

/* Block k committed, read+inflated through the row's fd, block 0's first 16 bytes (the ray_t header's tail) kept.
 * Runs inside the fault handler too, so it allocates nothing; a failed stream leaves zeros and ZB_TORN.
 * 0, the reader's -1 'corrupt / -2 'io, or -3 for a commit that failed. */
static int splay_zip_fill(splay_region* r, int64_t k) {
    int64_t off, plain = splay_zip_plain(r, k, &off);
    uint8_t* dst = r->vm.base + r->vm.guard + off;
    if (splay_vm_commit(&r->vm, (size_t)off, (size_t)plain) != 0) return -3;
    uint8_t tail[16];
    if (!k) memcpy(tail, dst, sizeof tail);
    int rc = q_io_zip_block_fd(r->fd, &r->zm, k, r->scratch, dst, (size_t)plain);
    if (rc) memset(dst, 0, (size_t)plain);
    if (!k) memcpy(dst, tail, sizeof tail);
    r->blk[k] = rc ? ZB_TORN : ZB_DONE;
    if (!rc) g_zblocks++;
    return rc;
}

/* Every unfilled block sharing a page with block k — a committed page must never serve zeros for a neighbour. */
static int splay_zip_touch(splay_region* r, int64_t k) {
    int64_t bs = r->zm.block_size, g = (int64_t)r->vm.guard, nb = r->zm.num_blocks;
    int64_t lo = (k * bs) / g * g / bs, hi = (((k + 1) * bs + g - 1) / g * g - 1) / bs;
    if (hi >= nb) hi = nb - 1;
    int rc = 0;
    for (int64_t j = lo; j <= hi; j++)
        if (r->blk[j] == ZB_NONE) { int e = splay_zip_fill(r, j); if (e && !rc) rc = e; }
    return rc;
}

/* The failure the handler could not raise, answered once; torn blocks re-arm so the next touch answers again. */
int q_splay_fault_pending(void) {
    if (!g_zfault) return 0;
    g_zfault = 0;
    for (int64_t i = 0; i < g_regn; i++) {
        splay_region* r = &g_regs[i];
        for (int64_t k = 0; r->fd >= 0 && k < r->zm.num_blocks; k++) {
            int64_t off, plain = splay_zip_plain(r, k, &off);
            if (r->blk[k] == ZB_TORN) splay_vm_arm(&r->vm, (size_t)off, (size_t)plain);
        }
    }
    return 1;
}

/* A fault at `a`: inside a live zip region's payload the block it landed in inflates (a re-armed torn block
 * re-commits, zero-filled, and answers again) and the read resumes — 1; anywhere else 0, the caller chains.
 * Every callee is async-signal-safe (the PR body lists them): no allocation, no ray_*. */
static int splay_fault_serve(uintptr_t a) {
    for (int64_t i = 0; i < g_regn; i++) {
        splay_region* r = &g_regs[i];
        uintptr_t base = (uintptr_t)(r->vm.base + r->vm.guard);
        if (r->fd < 0 || a < base || a >= base + (uintptr_t)r->zm.uncompressed) continue;
        int64_t k = (int64_t)(a - base) / r->zm.block_size, off, plain = splay_zip_plain(r, k, &off);
        int rc = r->blk[k] == ZB_NONE ? splay_zip_touch(r, k)
               : splay_vm_commit(&r->vm, (size_t)off, (size_t)plain) ? -3
               : r->blk[k] == ZB_TORN ? -1 : 0;
        if (rc == -3) {
            static const char msg[] = "peachq: cannot commit a compressed block's pages "
                                      "(kb/file-compression.md:171 - wsfull under a charged reservation)\n";
            if (write(2, msg, sizeof msg - 1) < 0) { /* nothing left to say */ }
            abort();
        }
        if (rc) g_zfault = 1;
        return 1;
    }
    return 0;
}

#ifdef RAY_OS_WINDOWS
static PVOID g_veh = NULL;

static LONG CALLBACK splay_veh(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* x = ep->ExceptionRecord;
    return x->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && x->NumberParameters >= 2 &&
           splay_fault_serve((uintptr_t)x->ExceptionInformation[1])
           ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
#else
static struct sigaction g_prev_segv, g_prev_bus;

/* not ours: the previous handler in place (ASan's, or another installed one), else the default action restored —
 * a kernel fault re-faults into it, a kill/raise-sent signal (si_code <= 0) is raised again */
static void splay_fault(int sig, siginfo_t* si, void* uc) {
    if (splay_fault_serve((uintptr_t)si->si_addr)) return;
    const struct sigaction* p = sig == SIGBUS ? &g_prev_bus : &g_prev_segv;
    if (p->sa_flags & SA_SIGINFO) { p->sa_sigaction(sig, si, uc); return; }
    if (p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN) { p->sa_handler(sig); return; }
    sigaction(sig, p, NULL);
    if (si->si_code <= 0) raise(sig);
}
#endif

void q_splay_init(void) {
    g_ents = NULL; g_n = 0; g_cap = 0;
    g_regs = NULL; g_regn = 0; g_regcap = 0;
    g_mapped_bytes = 0; g_reg_created = 0; g_reg_freed = 0; g_zfault = 0;
    ray_free_set_mapped_fn(splay_region_free);
#ifdef RAY_OS_WINDOWS
    g_veh = AddVectoredExceptionHandler(1, splay_veh);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = splay_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);     /* after ASan's: ours is live, its the fallback */
    sigaction(SIGBUS, &sa, &g_prev_bus);
#endif
}

void q_splay_destroy(void) {
    /* the env died first, so every mapped vector already released its region */
    assert(g_regn == 0 && g_reg_created == g_reg_freed);
    for (int64_t i = 0; i < g_regn; i++) splay_region_drop(&g_regs[i]);
    free(g_regs);
    g_regs = NULL; g_regn = 0; g_regcap = 0; g_mapped_bytes = 0;
    ray_free_set_mapped_fn(NULL);
#ifdef RAY_OS_WINDOWS
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = NULL; }
#else
    sigaction(SIGSEGV, &g_prev_segv, NULL);
    sigaction(SIGBUS, &g_prev_bus, NULL);
#endif
    for (int64_t i = 0; i < g_n; i++) {
        splay_ent* e = &g_ents[i];
        free(e->cols);
        if (e->keys)   ray_release(e->keys);
        if (e->dir)    ray_release(e->dir);
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

static void splay_region_add(ray_t* hdr, const splay_region* r) {
    g_regs[g_regn] = *r;
    g_regs[g_regn].hdr = hdr;
    g_regn++;
    g_mapped_bytes += (int64_t)r->vm.total;
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
    free(e->cols);
    if (e->keys)   ray_release(e->keys);
    if (e->dir)    ray_release(e->dir);
    g_ents[i] = g_ents[--g_n];   /* swap-remove */
}

/* A flat write INSIDE a mapped directory makes that entry a lie too — the
 * kb/linking-columns.md splayed-link workflow writes t1link and .d beside a
 * live mapping, then remaps.  Drop every entry whose dir prefixes the path. */
void q_splay_invalidate_under(const char* path, size_t n) {
    for (int64_t i = 0; i < g_n; ) {
        splay_ent* e = &g_ents[i];
        const char* d = e->dir ? ray_str_ptr(e->dir) : NULL;
        size_t dn = d ? ray_str_len(e->dir) : 0;
        if (d && dn > 0 && dn <= n && memcmp(d, path, dn) == 0 &&
            (d[dn - 1] == '/' || dn == n || path[dn] == '/'))
            q_splay_invalidate(e->sym);              /* swap-remove: re-check i */
        else i++;
    }
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

/* An enum names its domain, not its location: climb from the table directory
 * (a column copied out of its database fails, never walks to /), read the
 * domain FILE and bind it under its own name through q_env_set.  Best-effort:
 * any miss leaves the name unbound and display falls back to raw indices.
 * Re-run on every `get` of the carrier (bind-at-get), so touching another
 * splay's domain between gets cannot leave THIS one resolving through it. */
static void splay_bind_domain(splay_ent* e) {
    const char* name = e->domname;
    size_t nn = strlen(name);
    if (!nn) return;
    const char* p = ray_str_ptr(e->dir);
    size_t n = ray_str_len(e->dir);                     /* trailing '/' kept */
    for (int lvl = 0; lvl < 4 && n; lvl++) {
        n--;                                            /* the '/' ... */
        while (n && p[n - 1] != '/') n--;               /* ...and the name before it */
        if (!n) break;
        char buf[1024];
        if (n + nn + 1 > sizeof buf) return;
        memcpy(buf, p, n);
        memcpy(buf + n, name, nn);
        buf[n + nn] = '\0';
        struct stat st;
        if (stat(buf, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        ray_t* ps = ray_str(buf, n + nn);
        ray_t* d = (ps && !RAY_IS_ERR(ps)) ? q_wirefile_read_column(ps) : NULL;
        if (ps && !RAY_IS_ERR(ps)) ray_release(ps);
        if (d && !RAY_IS_ERR(d) && d->type == RAY_SYM)
            (void)q_env_set(ray_sym_intern_runtime(name, nn), d);   /* retains */
        if (d && !RAY_IS_ERR(d)) ray_release(d);
        else if (d) ray_error_free(d);
        return;
    }
}

/* Does `sym` spell a splay directory — `:…/`, and not a provider coordinate? */
static int splay_dir_sym_is(int64_t sym) {
    ray_t* s = ray_sym_str(sym);                        /* borrowed */
    if (!s) return 0;
    size_t n = ray_str_len(s);
    const char* p = ray_str_ptr(s);
    return n >= 2 && p[0] == ':' && p[n - 1] == '/' && !q_provider_spec_is(p, n);
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
    ray_t* dotd = q_wirefile_read_column(dps);
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
    /* One domain per splay: its domain FILE binds as a global under its own
     * name when the splay opens (and re-binds per get, splay_bind_domain) so
     * display resolves; columns themselves stay positions.  A missing file
     * just leaves the name unbound — display then shows raw indices. */
    for (int64_t i = 0; i < e.ncols; i++) {
        if (!e.cols[i].h.is_enum || !e.cols[i].h.domain[0]) continue;
        strcpy(e.domname, e.cols[i].h.domain);
        splay_bind_domain(&e);
        break;
    }
    if (g_n == g_cap) {
        int64_t nc = g_cap ? g_cap * 2 : 8;
        splay_ent* ne = (splay_ent*)realloc(g_ents, (size_t)nc * sizeof *ne);
        if (!ne) {
            splay_free_partial(&e);
            return q_err(QE_OOM);
        }
        g_ents = ne; g_cap = nc;
    }
    g_ents[g_n] = e;
    *out = &g_ents[g_n++];
    return NULL;
}


int64_t q_splay_table_path(ray_t* t) {
    uint8_t k = q_type_coord_kind(t);
    return k == Q_COORD_SPLAY || k == Q_COORD_SPLAY_UNRESOLVED ? q_type_coord_sym(t) : 0;
}

int q_splay_table_unresolved(ray_t* t) {
    return q_type_coord_kind(t) == Q_COORD_SPLAY_UNRESOLVED;
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
static ray_t* splay_hdr_over(const splay_region* r, splay_col* c) {
    const splay_vm* vm = &r->vm;
    ray_t* v = (ray_t*)(vm->base + vm->guard - 16);     /* data[] lands on byte 16 */
    v->mmod  = 3;                                       /* rc-driven release */
    v->order = 0;
    v->type  = c->h.tag;
    v->attrs = (uint8_t)((c->h.disk_attr == 1 ? RAY_ATTR_SORTED : 0) |
                         (splay_nullable(c->h.tag) ? RAY_ATTR_HAS_NULLS : 0));
    v->rc    = 1;                                       /* the caller's ref */
    v->len   = c->h.count;
    splay_region_add(v, r);
    /* the trusted letter (kx attr byte, else our validated sidecar) — a heap
     * marker block on the mapped header; ray_free releases it before the
     * region choke point, so the ledger needs no new row */
    char l = c->h.disk_attr >= 2 && c->h.disk_attr <= 4 ? "\0supg"[c->h.disk_attr]
           : c->h.side_attr;
    return l ? q_attr_stamp_trusted(v, l) : v;
}

/* Map a fixed-width column: kdb's payload starts at byte 16, a ray_t header
 * is 32 — the header overlays the guard's tail, the file's copy-on-write
 * pages follow it. */
static ray_t* splay_map(splay_ent* e, splay_col* c) {
    if (!splay_region_room()) return q_err(QE_OOM);
    ray_t* cp = splay_col_path(e, c->name);
    if (!cp) return q_err(QE_OOM);
    size_t need = 16 + (size_t)c->h.count * ray_type_sizes[(uint8_t)c->h.tag];
    splay_region r = { .fd = -1 };
    ray_t* bad = splay_vm_mapfile(ray_str_ptr(cp), need, &r.vm);
    ray_release(cp);
    if (bad) return bad;
    return splay_hdr_over(&r, c);
}

/* The KX compressed-read model (kb/file-compression.md:150,171): the full plain size reserved, block 0 inflated up
 * front (the ray_t header's tail lives in its first page), every other block on first touch through the fault
 * handler — a kernel with no index plumbing pays only for the pages it reads.  Same value kind as a mapped column:
 * one rc/munmap lifecycle, one ledger, the table's life. */
static ray_t* splay_zip_col(splay_ent* e, splay_col* c) {
    if (!splay_region_room()) return q_err(QE_OOM);
    ray_t* cp = splay_col_path(e, c->name);
    if (!cp) return q_err(QE_OOM);
    splay_region r = { .fd = -1 };
    ray_t* bad = q_io_zip_open(cp, &r.zm);
    if (!bad) r.fd = open(ray_str_ptr(cp), O_RDONLY | O_BINARY);
    ray_release(cp);
    if (bad) return bad;
    uint8_t esz = ray_type_sizes[(uint8_t)c->h.tag];
    int64_t unc = r.zm.uncompressed, nb = r.zm.num_blocks;
    if (r.fd < 0 || unc < 16 || c->h.count < 0 || 16 + c->h.count * esz > unc) {
        int io = r.fd < 0;
        splay_region_drop(&r);
        return q_err(io ? QE_IO : QE_CORRUPT);
    }
    size_t sc = q_io_zipmap_maxblock(&r.zm);
    r.scratch = (uint8_t*)malloc(sc ? sc : 1);
    r.blk = (uint8_t*)calloc((size_t)nb, 1);
    bad = r.scratch && r.blk ? splay_vm_reserve((size_t)unc, &r.vm) : q_err(QE_OOM);
    if (bad) { splay_region_drop(&r); return bad; }
    splay_vm_arm(&r.vm, r.vm.guard, r.vm.total - 2 * r.vm.guard);   /* page 0 stays readable for the header */
    int rc = splay_zip_touch(&r, 0);
    if (rc) { splay_region_drop(&r); return q_err(rc == -1 ? QE_CORRUPT : rc == -2 ? QE_IO : QE_OOM); }
    return splay_hdr_over(&r, c);                       /* header AFTER the fills */
}

/* Column i whole — the lanes: the zip region with block 0 inflated, a fresh map, or a decode. */
static ray_t* splay_col_read(splay_ent* e, int64_t i) {
    splay_col* c = &e->cols[i];
    if (c->h.zipped && c->h.mappable && !c->h.is_enum) return splay_zip_col(e, c);
    if (c->h.mappable && !c->h.zipped) return splay_map(e, c);
    ray_t* cp = splay_col_path(e, c->name);
    if (!cp) return q_err(QE_OOM);
    ray_t* col = q_wirefile_read_column(cp);
    ray_release(cp);
    return col ? col : q_err(QE_IO);
}

static int64_t splay_col_find(splay_ent* e, int64_t name) {
    for (int64_t i = 0; i < e->ncols; i++) if (e->cols[i].name == name) return i;
    return -1;
}

/* the named columns (cols NULL = every .d column) read whole into a table, the row-count agreement checked, the
 * directory sym marked in aux; NULL when a name is not on disk (the caller's unresolved form) */
static ray_t* splay_table(splay_ent* e, ray_t* cols) {
    int64_t nc = cols ? ray_len(cols) : e->ncols;
    ray_t* tbl = ray_table_new(nc > 0 ? nc : 1);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_OOM);
    int64_t rows = -1;
    for (int64_t c = 0; c < nc; c++) {
        int64_t i = cols ? splay_col_find(e, ray_vec_get_sym_id(cols, c)) : c;
        if (i < 0) { ray_release(tbl); return NULL; }
        ray_t* col = splay_col_read(e, i);
        if (RAY_IS_ERR(col)) { ray_release(tbl); return col; }
        int64_t n = ray_is_vec(col) || col->type == RAY_LIST || col->type == RAY_ENUM ? ray_len(col) : -1;
        if (rows < 0) rows = n;
        if (n < 0 || n != rows) {
            ray_release(col); ray_release(tbl);
            return q_err(QE_CORRUPT);
        }
        tbl = ray_table_add_col(tbl, e->cols[i].name, col);
        ray_release(col);
        if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_OOM);
    }
    q_type_coord_mark(tbl, Q_COORD_SPLAY, e->sym);
    return tbl;
}

ray_t* q_splay_get(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return NULL;
    ray_t* err = NULL;
    splay_ent* e = splay_resolve_sym(x->i64, &err);
    if (err) return err;
    if (!e) return NULL;
    splay_bind_domain(e);                               /* bind-at-get refresh */
    return splay_table(e, NULL);
}

ray_t* q_splay_flip(ray_t* cols, int64_t dirsym) {
    if (!cols || cols->type != RAY_SYM || !splay_dir_sym_is(dirsym)) return NULL;
    ray_t* err = NULL;
    splay_ent* e = splay_resolve_sym(dirsym, &err);
    if (err) return err;
    if (e) {
        splay_bind_domain(e);
        ray_t* t = splay_table(e, cols);
        if (t) return t;
    }
    int64_t nc = ray_len(cols);                         /* unresolved: `()` per name, queried later */
    ray_t* tbl = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t i = 0; i < nc && tbl && !RAY_IS_ERR(tbl); i++) {
        ray_t* empty = ray_list_new(0);
        if (!empty || RAY_IS_ERR(empty)) { ray_release(tbl); return empty ? empty : q_err(QE_OOM); }
        tbl = ray_table_add_col(tbl, ray_vec_get_sym_id(cols, i), empty);
        ray_release(empty);
    }
    if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_OOM);
    q_type_coord_mark(tbl, Q_COORD_SPLAY_UNRESOLVED, dirsym);
    return tbl;
}
