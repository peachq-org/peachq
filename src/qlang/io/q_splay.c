/* q_splay — the mapped-file authority, with kdb's lifetime (ref/get.md:29: columns are mapped "in and out of memory
 * when querying"; kb/file-compression.md:150: "Columns are mapped for the duration of the select").
 * `get `:dir/` answers the DEFERRED table (ref/flip-splayed.md: the flip of `cols!`:dir/`): names and the directory
 * sym in the block's aux bytes, nothing mapped.  Each use maps the columns it names into an ordinary table for that
 * use alone, and a map dies by refcount with its last holder through ONE unmap choke point — so a column a result
 * still holds stays mapped and the rest is gone when the application returns.  `get `:dir` maps every column at once.
 * One lane serves a column and a vector file: fixed-width vectors and 64-bit enums mmap (mmod==3), compressed ones
 * inflate a block on first touch (the fault handler), the rest decode.  Open splays are keyed by the `:dir/ handle sym
 * (the q_handles.c pattern): open reads .d + column HEADERS and binds the domain.  Single-threaded q layer. */
#define _GNU_SOURCE            /* MAP_ANONYMOUS / MAP_FIXED */
#include "qlang/q_count.h"
#include "qlang/io/q_splay.h"
#include "qlang/io/q_handles.h" /* a `:pq:` coordinate is never a splay directory */
#include "qlang/io/q_io.h"           /* q_io_file_path */
#include "qlang/net/q_wirefile.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"      /* q_type_coord_mark — THE aux mark that says "flipped from a coordinate" */
#include "qlang/q_env.h"             /* q_env_set — the one global-set home */
#include "qlang/q_prim.h"            /* q_enum_stamp */
#include "lang/eval.h"               /* ray_eval_get_restricted */
#include "table/sym.h"               /* ray_sym_intern_runtime, ray_sym_str */
#include "mem/heap.h"                /* attrs + ray_free_set_mapped_fn */
#include "core/pool.h"               /* ray_pool_get — are the kernels' worker threads admitted (`\s`)? */
#include "ops/idxop.h"               /* ray_index_set_maker / _torn — a p# index is built on its first use */
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
  #include <io.h>
#else
  #include <sys/mman.h>
#endif

typedef struct { int64_t dev, ino, size, sec, nsec; } splay_stamp;

typedef struct {
    int64_t     name;     /* column sym id */
    q_wf_colhdr h;
    splay_stamp st;
    uint8_t     probed;   /* h is the header of the file st stamps */
} splay_col;

typedef struct {
    int64_t    sym;       /* the `:dir/ handle sym — the table's aux path + key */
    ray_t*     dir;       /* owned RAY_STR path, trailing slash kept */
    ray_t*     keys;      /* owned .d sym vector */
    splay_stamp dst;      /* the .d file's */
    splay_col* cols;
    int64_t    ncols;
    char       domname[256];   /* enum domain name ("" = no enum columns) */
} splay_ent;

static splay_ent* g_ents = NULL;
static int64_t    g_n = 0, g_cap = 0;

static void splay_free_partial(splay_ent* e) {
    free(e->cols);
    if (e->keys) ray_release(e->keys);
    if (e->dir)  ray_release(e->dir);
}

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
    size_t        data;       /* the elements' offset in the plain image: the ray_t header is the 32 bytes before it */
    size_t        tail;       /* bytes after the elements — a p# column's index trailer, read on the index's first use */
    char          layout;     /* the probe's, for that read */
} splay_region;
static splay_region* g_regs = NULL;
static int64_t g_regn = 0, g_regcap = 0;
static int64_t g_mapped_bytes = 0;
static int64_t g_reg_created = 0, g_reg_freed = 0;
static int64_t g_asks = 0;      /* cumulative "is this table deferred?" — the witness that no list is searched */
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

/* shared for delete as the file lane's view is: a held compressed column must not pin its file against hdel */
static int splay_open_ro(const char* path) {
    HANDLE fh = CreateFileA(path, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return -1;
    int fd = _open_osfhandle((intptr_t)fh, _O_RDONLY | _O_BINARY);
    if (fd < 0) CloseHandle(fh);
    return fd;
}

#else

static int splay_open_ro(const char* path) { return open(path, O_RDONLY); }

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

/* Block k committed, read+inflated through the row's fd; the part of the ray_t header that lies in block 0 (all of
 * it behind a shape-D page, its last 16 bytes otherwise) is kept.  Runs inside the fault handler too, so it
 * allocates nothing; a failed stream leaves zeros and ZB_TORN.
 * 0, the reader's -1 'corrupt / -2 'io, or -3 for a commit that failed. */
static int splay_zip_fill(splay_region* r, int64_t k) {
    int64_t off, plain = splay_zip_plain(r, k, &off);
    uint8_t* dst = r->vm.base + r->vm.guard + off;
    if (splay_vm_commit(&r->vm, (size_t)off, (size_t)plain) != 0) return -3;
    uint8_t keep[sizeof(ray_t)];
    size_t lo = r->data < sizeof keep ? 0 : r->data - sizeof keep, kn = r->data - lo;
    if (!k) memcpy(keep, dst + lo, kn);
    int rc = q_io_zip_block_fd(r->fd, &r->zm, k, r->scratch, dst, (size_t)plain);
    if (rc) memset(dst, 0, (size_t)plain);
    if (!k) memcpy(dst + lo, keep, kn);
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
    int torn = ray_index_torn();                        /* a trusted p# index its column's cells denied */
    if (!g_zfault) return torn;
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

/* The first consult of a mapped p# column's index: its trailer is read where it lies after the elements — two
 * headers, the distinct values and the group starts, so a compressed column inflates those blocks and no other —
 * and adopted, or the letter dropped.  A copy that left its map has no trailer and keeps scanning. */
static void splay_parted_make(ray_t* col) {
    for (int64_t i = 0; i < g_regn; i++) {
        const splay_region* r = &g_regs[i];
        if (r->hdr != col) continue;
        uint8_t esz = col->type == RAY_ENUM ? 8 : ray_type_sizes[(uint8_t)col->type];
        q_wirefile_parted_adopt(col, (const uint8_t*)ray_data(col) + (size_t)col->len * esz, r->tail, r->layout, esz);
        return;
    }
}

void q_splay_init(void) {
    g_ents = NULL; g_n = 0; g_cap = 0;
    g_regs = NULL; g_regn = 0; g_regcap = 0;
    g_mapped_bytes = 0; g_reg_created = 0; g_reg_freed = 0; g_zfault = 0;
    ray_free_set_mapped_fn(splay_region_free);
    ray_index_set_maker(splay_parted_make);
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
    ray_index_set_maker(NULL);
#ifdef RAY_OS_WINDOWS
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = NULL; }
#else
    sigaction(SIGSEGV, &g_prev_segv, NULL);
    sigaction(SIGBUS, &g_prev_bus, NULL);
#endif
    for (int64_t i = 0; i < g_n; i++) splay_free_partial(&g_ents[i]);
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

/* What says a file is still the one whose header an entry holds: a rename-replace moves the inode, an in-place
 * rewrite the size or the nanosecond mtime.  0 when the path will not stat — and always where stat has no file
 * identity or only second mtimes (Windows CRT), so there every dereference reopens. */
static int splay_stamp_of(const char* path, splay_stamp* s) {
#if defined(RAY_OS_LINUX) || defined(RAY_OS_MACOS)
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    s->dev = (int64_t)st.st_dev; s->ino = (int64_t)st.st_ino; s->size = (int64_t)st.st_size;
    s->sec = (int64_t)st.st_mtime;
#if defined(RAY_OS_LINUX)
    s->nsec = (int64_t)st.st_mtim.tv_nsec;
#else
    s->nsec = (int64_t)st.st_mtimespec.tv_nsec;
#endif
    return 1;
#else
    (void)path; (void)s;
    return 0;
#endif
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
    return n >= 2 && p[0] == ':' && p[n - 1] == '/' && !q_handles_pq(p, n, NULL);
}

/* a plain symbol names a partitioned table (ref/flip-splayed.md:21); the null symbol is id 0, the "no coordinate"
 * sentinel, and names nothing */
static int splay_name_sym_is(int64_t sym) {
    if (sym == 0) return 0;
    ray_t* a = ray_sym(sym);
    int name = !q_io_is_fsym(a);
    ray_release(a);
    return name;
}

static ray_t* splay_open(int64_t sym, ray_t* dir, splay_ent** out);

/* Is the .d the entry read still the one on disk?  A write that finished before this dereference — through another
 * spelling of the directory, or from another process — shows up here, or at the column it rewrote. */
static int splay_fresh(splay_ent* e, const char* dotd) {
    splay_stamp now;
    return splay_stamp_of(dotd, &now) && !memcmp(&now, &e->dst, sizeof now);
}

/* Column i's header, read when the column is first used and again whenever its file's stamp moves — so a use
 * stats and probes the columns it maps and never the table's others (kdb reads only the columns a query names). */
static ray_t* splay_col_ready(splay_ent* e, int64_t i) {
    splay_col* c = &e->cols[i];
    ray_t* cp = splay_col_path(e, c->name);
    if (!cp) return q_err(QE_OOM);
    splay_stamp now;
    int known = splay_stamp_of(ray_str_ptr(cp), &now);  /* stamp BEFORE the read: a racing writer reads as stale */
    ray_t* err = NULL;
    if (!c->probed || !known || memcmp(&now, &c->st, sizeof now)) {
        c->probed = 0;
        err = q_wirefile_probe(cp, &c->h);
        if (!err && known) { c->st = now; c->probed = 1; }
    }
    ray_release(cp);
    return err;
}

/* The door that names a path (get, flip) validates the whole table as a load does: every header, the agreement of
 * the counts they state — failing beats a plausible short table — and the one enum domain's name. */
static ray_t* splay_validate(splay_ent* e) {
    e->domname[0] = '\0';
    for (int64_t i = 0; i < e->ncols; i++) {
        ray_t* err = splay_col_ready(e, i);
        if (err) return err;
        const q_wf_colhdr* h = &e->cols[i].h;
        if (i && h->count >= 0 && e->cols[0].h.count >= 0 && h->count != e->cols[0].h.count) return q_err(QE_CORRUPT);
        if (!e->domname[0] && h->is_enum && h->domain[0]) strcpy(e->domname, h->domain);
    }
    return NULL;
}

/* Entry behind handle-sym `sym`: found and still fresh, or opened from its own path (a cold carrier — wire
 * round-trip, fresh runtime — heals here; a stale one reopens).  NULL with *err NULL = not a table folder; NULL with
 * *err = the open's error. */
static splay_ent* splay_resolve_sym(int64_t sym, ray_t** err) {
    *err = NULL;
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
    int64_t i = has_d ? splay_find(sym) : -1;
    int fresh = i >= 0 && splay_fresh(&g_ents[i], dp);
    free(dp);
    if (fresh) { ray_release(path); return &g_ents[i]; }
    if (i >= 0) { splay_free_partial(&g_ents[i]); g_ents[i] = g_ents[--g_n]; }
    if (!has_d) { ray_release(path); return NULL; }     /* flat reader answers 'type */
    if (ray_eval_get_restricted()) { ray_release(path); *err = q_err(QE_ACCESS); return NULL; }
    splay_ent* e = NULL;
    *err = splay_open(sym, path, &e);                   /* consumes path */
    return e;
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
    (void)splay_stamp_of(dp, &e.dst);                   /* stamp BEFORE the read: a racing writer reads as stale */
    free(dp);
    if (!dps) { splay_free_partial(&e); return q_err(QE_OOM); }
    ray_t* dotd = q_wirefile_read_column(dps);
    ray_release(dps);
    if (!dotd || RAY_IS_ERR(dotd)) { splay_free_partial(&e); return dotd ? dotd : q_err(QE_IO); }
    if (dotd->type != RAY_SYM) { ray_release(dotd); splay_free_partial(&e); return q_err(QE_CORRUPT); }
    e.keys = dotd;
    e.ncols = q_count(dotd);
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
    return k == Q_COORD_SPLAY || k == Q_COORD_SPLAY_DEFERRED || k == Q_COORD_SPLAY_UNRESOLVED ? q_type_coord_sym(t) : 0;
}

int64_t q_splay_asks(void) { return g_asks; }

int q_splay_table_deferred(ray_t* t) {
    g_asks++;
    uint8_t k = q_type_coord_kind(t);
    return k == Q_COORD_SPLAY_DEFERRED || k == Q_COORD_SPLAY_UNRESOLVED;
}

/* HAS_NULLS is PESSIMISTIC on sentinel-capable types: consumers read it as "may contain" (fast paths decline); only
 * a CLEAR flag over hidden sentinels corrupts reductions, and a map that scanned its column to know would read the
 * whole file at first use. */
static int splay_nullable(const q_wf_colhdr* h) {
    if (h->is_enum) return 1;
    switch ((ray_type_e)h->tag) {
    case RAY_I16: case RAY_I32: case RAY_I64: case RAY_F32: case RAY_F64:
    RAY_TEMPORAL32_CASES: RAY_TEMPORAL64_CASES: RAY_TEMPORALF_CASES:
        return 1;
    default:
        return 0;
    }
}

static size_t splay_esz(const q_wf_colhdr* h) { return h->is_enum ? 8 : ray_type_sizes[(uint8_t)h->tag]; }

/* Lay the ray_t header over the 32 bytes before the elements, so they become its data[], and enrol the region — the
 * ONE place a mapped vector is born.  Those bytes are the guard's tail and kdb's 16-byte header, or the end of a
 * shape-D page; an enum's positions are its 20h payload as they lie, never validated (kdb reads them as they are). */
static ray_t* splay_hdr_over(const splay_region* r, const q_wf_colhdr* h) {
    ray_t* v = (ray_t*)(r->vm.base + r->vm.guard + r->data - sizeof(ray_t));
    memset(v, 0, sizeof(ray_t));
    v->mmod  = 3;                                       /* rc-driven release */
    v->type  = h->is_enum ? RAY_I64 : h->tag;
    v->attrs = (uint8_t)(splay_nullable(h) ? RAY_ATTR_HAS_NULLS : 0);
    v->rc    = 1;                                       /* the caller's ref */
    v->len   = h->count;
    splay_region_add(v, r);
    if (h->is_enum) v = q_enum_stamp(v, ray_sym_intern_runtime(h->domain, strlen(h->domain)));
    /* the header byte is the attribute, as kdb reads it: `s` on the writer's word, `p` as a letter whose index
     * waits for its first use (splay_parted_make); u and g column files are unsampled and drop */
    return h->disk_attr == 1 ? q_attr_stamp_trusted(v, 's') : h->disk_attr == 3 ? q_attr_parted_pending(v) : v;
}

/* Map a fixed-width file copy-on-write behind a guard page: the ray_t header overlays the bytes before the
 * elements. */
static ray_t* splay_map(const char* path, const q_wf_colhdr* h) {
    if (!splay_region_room()) return q_err(QE_OOM);
    size_t tail = h->disk_attr == 3 ? (size_t)h->tail : 0;
    splay_region r = { .fd = -1, .data = h->payload, .tail = tail, .layout = h->layout };
    ray_t* bad = splay_vm_mapfile(path, h->payload + (size_t)h->count * splay_esz(h) + tail, &r.vm);
    return bad ? bad : splay_hdr_over(&r, h);
}

/* The KX compressed-read model (kb/file-compression.md:150,171): the full plain size reserved, block 0 inflated up
 * front (the ray_t header lives in it), every other block on first touch through the fault handler — a kernel with
 * no index plumbing pays only for the pages it reads.  Same value kind as a mapped file: one rc/munmap lifecycle,
 * one ledger.  A container whose first block ends before the elements start is decoded whole instead. */
static ray_t* splay_zip(ray_t* path, const q_wf_colhdr* h) {
    if (!splay_region_room()) return q_err(QE_OOM);
    splay_region r = { .fd = -1, .data = h->payload, .tail = (size_t)h->tail, .layout = h->layout };
    ray_t* bad = q_io_zip_open(path, &r.zm);
    if (bad) return bad;
    int64_t unc = r.zm.uncompressed, nb = r.zm.num_blocks;
    if (nb > 1 && r.zm.block_size < (int64_t)r.data) {
        splay_region_drop(&r);
        return q_wirefile_read_column(path);
    }
    r.fd = splay_open_ro(ray_str_ptr(path));
    if (r.fd < 0 || h->count < 0 || (int64_t)r.data + h->count * (int64_t)splay_esz(h) > unc) {
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
    return splay_hdr_over(&r, h);                       /* header AFTER the fills */
}

/* THE first use of a file's bytes, for a splayed column and a vector file alike — the lanes: a map, a zip region
 * with block 0 inflated, or a decode (nested, string, symbol and 32-bit enum files).  The zip region is one
 * thread's: with worker threads admitted (`\s`) a kernel's second thread would read a block the first is still
 * filling, so a compressed file is decoded whole then. */
static ray_t* splay_read(ray_t* path, const q_wf_colhdr* h) {
    ray_pool_t* pool = h->mappable && h->zipped ? ray_pool_get() : NULL;
    if (h->mappable && !(pool && pool->n_active > 0))
        return h->zipped ? splay_zip(path, h) : splay_map(ray_str_ptr(path), h);
    ray_t* v = q_wirefile_read_column(path);
    return v ? v : q_err(QE_IO);
}

static int64_t splay_col_find(splay_ent* e, int64_t name) {
    for (int64_t i = 0; i < e->ncols; i++) if (e->cols[i].name == name) return i;
    return -1;
}

/* the n named columns (ids NULL = every .d column) read whole into a table, the row-count agreement checked, the
 * directory sym marked in aux; NULL when a name is not on disk (the table stays deferred) */
static ray_t* splay_table(splay_ent* e, const int64_t* ids, int64_t n) {
    int64_t nc = ids ? n : e->ncols;
    ray_t* tbl = ray_table_new(nc > 0 ? nc : 1);
    if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_OOM);
    int64_t rows = -1;
    for (int64_t c = 0; c < nc; c++) {
        int64_t i = ids ? splay_col_find(e, ids[c]) : c;
        if (i < 0) { ray_release(tbl); return NULL; }
        ray_t* cp = splay_col_path(e, e->cols[i].name);
        ray_t* col = cp ? splay_col_ready(e, i) : q_err(QE_OOM);
        if (!col) col = splay_read(cp, &e->cols[i].h);
        if (cp) ray_release(cp);
        if (RAY_IS_ERR(col)) { ray_release(tbl); return col; }
        int64_t k = ray_is_vec(col) || col->type == RAY_LIST || col->type == RAY_ENUM ? q_count(col) : -1;
        if (rows < 0) rows = k;
        if (k < 0 || k != rows) {
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

/* The deferred table (ref/flip-splayed.md: the flip of `cols!`:dir/`): a `()` per name and the coordinate in aux —
 * no file is open and nothing is mapped until q_splay_deferred_map.  `kind` says whether its directory answered when
 * it was made (DEFERRED: it must answer again, or the use is 'io) or never did (UNRESOLVED: it fails only when
 * queried). */
static ray_t* splay_deferred(ray_t* cols, int64_t dirsym, uint8_t kind) {
    int64_t nc = q_count(cols);
    ray_t* tbl = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t i = 0; i < nc && tbl && !RAY_IS_ERR(tbl); i++) {
        ray_t* empty = ray_list_new(0);
        if (!empty || RAY_IS_ERR(empty)) { ray_release(tbl); return empty ? empty : q_err(QE_OOM); }
        tbl = ray_table_add_col(tbl, ray_vec_get_sym_id(cols, i), empty);
        ray_release(empty);
    }
    if (!tbl || RAY_IS_ERR(tbl)) return tbl ? tbl : q_err(QE_OOM);
    q_type_coord_mark(tbl, kind, dirsym);
    return tbl;
}

static int splay_wants(ray_t* want, int64_t id) {
    if (want->type == -RAY_SYM) return want->i64 == id;
    for (int64_t i = 0; i < q_count(want); i++) if (ray_vec_get_sym_id(want, i) == id) return 1;
    return 0;
}

/* the map itself, under whatever file restriction stands */
static ray_t* splay_deferred_table(ray_t* t, ray_t* want) {
    ray_t* err = NULL;
    splay_ent* e = splay_resolve_sym(q_type_coord_sym(t), &err);
    if (!e) return err;
    int64_t nc = ray_table_ncols(t), n = 0;
    int64_t* ids = (int64_t*)malloc((size_t)(nc > 0 ? nc : 1) * sizeof *ids);
    if (!ids) return q_err(QE_OOM);
    for (int64_t i = 0; i < nc; i++) {
        int64_t id = ray_table_col_name(t, i);
        if (!want || splay_wants(want, id)) ids[n++] = id;
    }
    if (!n && nc) ids[n++] = ray_table_col_name(t, 0);  /* the row count is a column's */
    ray_t* m = splay_table(e, ids, n);
    free(ids);
    return m;
}

/* One use of a deferred table: the owned mapped table of its columns named in `want` (a sym atom or vector; anything
 * else = all of them; none of them = the first, since a table's count is a column's).  NULL when t is not deferred or
 * its directory, a column file or its partitioned name does not resolve — the caller keeps t; a RAY_ERROR when the
 * files are there and bad.  THE seam a hold-the-maps mode (`.Q.MAP`) would cache behind. */
ray_t* q_splay_deferred_map(ray_t* t, ray_t* want) {
    if (!q_splay_table_deferred(t) || !splay_dir_sym_is(q_type_coord_sym(t))) return NULL;
    if (want && want->type != RAY_SYM && want->type != -RAY_SYM) want = NULL;
    /* a table the session holds re-maps whoever asks: the restriction (-u) was answered where its path was named,
     * and every read below — .d, a header, a compressed or decoded column — is of that table */
    int held = q_type_coord_kind(t) == Q_COORD_SPLAY_DEFERRED;
    bool restricted = held && ray_eval_get_restricted();
    if (restricted) ray_eval_set_restricted(false);
    ray_t* m = splay_deferred_table(t, want);
    if (restricted) ray_eval_set_restricted(true);
    return m || !held ? m : q_err(QE_IO);
}

/* A value entering a list or dict (q_splay_contained, which sends only tables here): a deferred table goes in mapped
 * at once, as `get` without the slash answers it, so no container holds a deferred table and no verb searches one
 * for it.  Consumes t: t itself, its mapped table, or a RAY_ERROR. */
ray_t* q_splay_contained_table(ray_t* t) {
    ray_t* m = q_splay_deferred_map(t, NULL);
    if (m) ray_release(t);
    return m ? m : t;
}

/* One column of a deferred table by name, mapped and owned — a dotted name, a foreign-key walk.  NULL when t is
 * not deferred, does not resolve or has no such column; a RAY_ERROR when its files are bad. */
ray_t* q_splay_deferred_col(ray_t* t, int64_t name) {
    if (!t || t->type != RAY_TABLE || !q_splay_table_deferred(t)) return NULL;
    ray_t* want = ray_sym(name);
    ray_t* m = q_splay_deferred_map(t, want);
    ray_release(want);
    if (!m || RAY_IS_ERR(m)) return m;
    ray_t* col = ray_table_get_col(m, name);
    if (col) ray_retain(col);
    ray_release(m);
    return col;
}

/* `get `:file` of one fixed-width vector maps it; NULL leaves every other file, and one the probe refuses, to the
 * flat reader. */
static ray_t* splay_vec_file(ray_t* path) {
    q_wf_colhdr h;
    ray_t* bad = q_wirefile_probe(path, &h);
    if (bad) { ray_error_free(bad); return NULL; }
    return h.mappable ? splay_read(path, &h) : NULL;
}

/* `get` of a `:path sym atom: `:dir/` the deferred table, `:dir` the same folder mapped at once, a `:file` of one
 * fixed-width vector that vector mapped.  NULL for anything else (the caller falls through to the flat reader), or
 * a RAY_ERROR: a header that does not validate, a file that will not map. */
ray_t* q_splay_get(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return NULL;
    ray_t* s = ray_sym_str(x->i64);                     /* borrowed */
    size_t sn = s ? ray_str_len(s) : 0;
    const char* sp = s ? ray_str_ptr(s) : NULL;
    if (sn < 2 || sp[0] != ':' || q_handles_pq(sp, sn, NULL)) return NULL;
    int64_t dirsym = x->i64;
    int deferred = sp[sn - 1] == '/';
    if (!deferred) {
        ray_t* path = q_io_file_path(x);
        struct stat st;
        int have = path && stat(ray_str_ptr(path), &st) == 0;
        ray_t* v = have && S_ISREG(st.st_mode) && !ray_eval_get_restricted() ? splay_vec_file(path) : NULL;
        if (path) ray_release(path);
        if (v || !have || !S_ISDIR(st.st_mode)) return v;
        /* `get `:db/t` reads the folder `get `:db/t/` names (kb/splayed-tables.md:273), mapped at once */
        char* d = (char*)malloc(sn + 1);
        if (!d) return q_err(QE_OOM);
        memcpy(d, sp, sn);
        d[sn] = '/';
        dirsym = ray_sym_intern_runtime(d, sn + 1);
        free(d);
    }
    ray_t* err = NULL;
    splay_ent* e = splay_resolve_sym(dirsym, &err);
    if (e) err = splay_validate(e);
    if (!e || err) return err;
    splay_bind_domain(e);                               /* bind-at-get refresh */
    return deferred ? splay_deferred(e->keys, e->sym, Q_COORD_SPLAY_DEFERRED) : splay_table(e, NULL, 0);
}

/* `flip cols!`:dir/` and `flip cols!`name`: the deferred table over exactly those columns — DEFERRED when the
 * directory holds them all now, else UNRESOLVED, which fails only when queried (ref/flip-splayed.md); a plain name
 * is a partitioned table, which no mount resolves yet.  NULL for a provider coordinate or the null sym. */
ray_t* q_splay_flip(ray_t* cols, int64_t dirsym) {
    if (!cols || cols->type != RAY_SYM) return NULL;
    uint8_t kind = Q_COORD_SPLAY_UNRESOLVED;
    if (splay_dir_sym_is(dirsym)) {
        ray_t* err = NULL;
        splay_ent* e = splay_resolve_sym(dirsym, &err);
        if (e) err = splay_validate(e);
        if (err) return err;
        int64_t have = 0, nc = q_count(cols);
        while (e && have < nc && splay_col_find(e, ray_vec_get_sym_id(cols, have)) >= 0) have++;
        if (e && have == nc) {
            splay_bind_domain(e);
            kind = Q_COORD_SPLAY_DEFERRED;
        }
    } else if (!splay_name_sym_is(dirsym)) {
        return NULL;
    }
    return splay_deferred(cols, dirsym, kind);
}
