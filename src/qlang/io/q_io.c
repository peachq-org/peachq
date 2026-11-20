/* io/q_io.c — THE byte core (q_io.h) and the verbs sitting directly on it:
 * hsym, read0, read1, hdel.  Oracle: ref/read0.md, ref/read1.md, ref/hsym.md,
 * ref/hdel.md, kb/file-compression.md (CLEAN ROOM).  `0:` File Text builds on
 * this file from io/q_io_filetext.c; hopen/hclose live with the handle
 * registry (q_handles.c) and getenv/setenv with the process commands (q_sys.c).
 *
 * Restricted mode: every local file door asks q_io_guard (the cwd path rule); a URL, and what stays forbidden
 * outright (hdel, a tree delete), asks ray_eval_get_restricted(). */
#define _GNU_SOURCE            /* realpath */
#include "qlang/q_count.h"
#include "qlang/io/q_io.h"
#include "qlang/q_registry_internal.h" /* q_str_split_lines, q_type_strict_i64 */
#include "qlang/base/q_err.h"
#include "qlang/io/q_handles.h" /* q_handles_read1 — the fifo-handle read form */
#include "qlang/io/q_provider.h"  /* q_io_set: a table coordinate routes to .X.set; hdel: to .X.hdel */
#include "qlang/io/q_csv.h"     /* the CSV/TSV decoder behind a recognised tabular suffix */
#include "qlang/io/q_json.h"    /* the JSON decoder, and the framing a suffix declares to it */
#include "qlang/io/q_zip.h"     /* q_zip_* — a detected zip archive */
#include "qlang/io/q_tar.h"     /* q_tar_* — a detected tar archive */
#include "qlang/io/q_s3.h"      /* q_s3_* — the s3:// transport of the byte reader */
#include "qlang/eval/q_eval.h"  /* q_eval_apply_call_name — the .parquet doors and the .duckdb transport are bound by `\l pq`, not linked */
#include "qlang/q_env.h"        /* q_env_get — `.h.tx`, the format table `set` reads at call time */
#include "qlang/net/q_gz.h"     /* q_gz_inflate_zlib — the kxzip block codec */
#include "lz4hc.h"               /* the lz4hc (algorithm 4) block codec */
#include "qlang/net/q_wire.h"   /* q_wire_uncompress_into, q_wire_compress — the kdb-IPC (algorithm 1) block codec */
#include "qlang/net/q_wirefile.h" /* the format writers behind q_io_set */
#include "qlang/net/q_http_client.h" /* the http half of the resource-read seam */
#include "qlang/ops/q_sys.h"     /* q_sys_started_restricted / _blocked — the -u 1 and -b a process started with */
#include "lang/eval.h"      /* ray_eval_get_restricted */
#include "lang/env.h"       /* ray_fn_unary — the .fs.i.rmtree native */
#include "lang/internal.h"  /* ray_fs_list_fn — the quarry's sorted directory listing */
#include "store/fileio.h"   /* ray_mkdir_p — the parent directories a write promises */
#include "table/sym.h"      /* ray_sym_intern_runtime, ray_sym_vec_cell */
#include "app/term.h"       /* ray_term_line_begin/end — a terminal `read0 0` shows what is typed */
#include <ctype.h>          /* tolower — an archive's ending in any case */
#include <limits.h>         /* PATH_MAX — q_io_abs_path */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>         /* read, isatty — `read0 0` is a raw read of fd 0; ftruncate */
#include <fcntl.h>          /* open — the kxzip block reader's descriptor */
#include <errno.h>
#include <stdatomic.h>
#ifdef RAY_OS_WINDOWS
#include <io.h>             /* _get_osfhandle — the positional block read */
#include <windows.h>
#else
#include <dirent.h>         /* fdopendir — the tree delete's walk */
#endif
#ifndef O_BINARY
#define O_BINARY 0          /* only Windows has a text mode to opt out of */
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0        /* Windows has no fifo to wait on */
#endif

/* ---- paths, size, write ------------------------------------------------- */

int q_io_abs_path(const char* path, char* abs, size_t cap) {
    char full[PATH_MAX];
#ifdef RAY_OS_WINDOWS
    if (!_fullpath(full, path, sizeof full)) return 0;
    for (char* p = full; *p; p++) if (*p == '\\') *p = '/';
#else
    if (!realpath(path, full)) return 0;
#endif
    size_t n = strlen(full);
    if (n + 1 > cap) return 0;
    memcpy(abs, full, n + 1);
    return 1;
}

int q_io_abs_spelling(const char* path, size_t n, char* abs, size_t cap) {
#ifdef RAY_OS_WINDOWS
    char p[PATH_MAX];                   /* _fullpath is lexical: drive- and root-relative forms resolve, no file needed */
    if (n >= sizeof p) return 0;
    memcpy(p, path, n);
    p[n] = '\0';
    return q_io_abs_path(p, abs, cap);
#else
    size_t c = 0;
    if (!n || path[0] != '/') {
        if (!q_io_abs_path(".", abs, cap)) return 0;
        c = strlen(abs);
        if (c && abs[c - 1] != '/') abs[c++] = '/';
    }
    if (c + n + 1 > cap) return 0;
    memcpy(abs + c, path, n);
    abs[c + n] = '\0';
    return 1;
#endif
}

int q_io_path_inside(const char* p, size_t n) {
    if (!n || p[0] == '/' || memchr(p, ':', n) || memchr(p, '\\', n)) return 0;
    for (size_t i = 0; i + 1 < n; i++)
        if (p[i] == '.' && p[i + 1] == '.' && (!i || p[i - 1] == '/') && (i + 2 == n || p[i + 2] == '/')) return 0;
    return 1;
}

#ifdef RAY_OS_WINDOWS
/* _fullpath is lexical, so a junction or symlink is not followed: a recorded gap (Dbidsz) */
static int io_canonical(const char* p, size_t n, char* out, size_t cap) { return q_io_abs_spelling(p, n, out, cap); }
#else
/* The longest prefix that exists, its links resolved, then the rest applied lexically.  0 refuses: a link that will
 * not resolve (dangling or looping), a path too long, and any `..` past a missing component, which could otherwise
 * climb back onto existing names, links among them, without resolving them. */
static int io_canonical(const char* p, size_t n, char* out, size_t cap) {
    char abs[PATH_MAX], real[PATH_MAX];
    if (!q_io_abs_spelling(p, n, abs, sizeof abs)) return 0;
    size_t len = strlen(abs), cut;
    while (len > 1 && abs[len - 1] == '/') abs[--len] = '\0';
    for (cut = len;;) {
        char at = abs[cut];
        abs[cut] = '\0';
        struct stat st;
        int ok = realpath(abs, real) != NULL, entry = !ok && lstat(abs, &st) == 0;
        abs[cut] = at;
        if (ok) break;
        if (entry || cut <= 1) return 0;
        while (cut > 1 && abs[cut - 1] != '/') cut--;
        while (cut > 1 && abs[cut - 1] == '/') cut--;
    }
    size_t m = strlen(real);
    for (const char* c = abs + cut; *c; ) {
        while (*c == '/') c++;
        size_t k = strcspn(c, "/");
        if (k == 2 && c[0] == '.' && c[1] == '.') return 0;
        if (k && !(k == 1 && c[0] == '.')) {
            if (m + 1 + k + 1 > sizeof real) return 0;
            if (real[m - 1] != '/') real[m++] = '/';
            memcpy(real + m, c, k);
            m += k;
        }
        real[m] = '\0';
        c += k;
    }
    if (m + 1 > cap) return 0;
    memcpy(out, real, m + 1);
    return 1;
}
#endif

/* is p's canonical form root's (NULL = the current directory) or below it, whole components compared? */
static int io_admits(const char* root, size_t rn, const char* p, size_t n) {
    char top[PATH_MAX], at[PATH_MAX];
    if (!(root ? io_canonical(root, rn, top, sizeof top) : q_io_abs_path(".", top, sizeof top))
        || !io_canonical(p, n, at, sizeof at)) return 0;
    size_t tl = strlen(top);
#ifdef RAY_OS_WINDOWS
    int same = _strnicmp(at, top, tl) == 0;      /* the filesystem folds case; _fullpath keeps the spelling's */
#else
    int same = strncmp(at, top, tl) == 0;
#endif
    return same && (!at[tl] || at[tl] == '/' || top[tl - 1] == '/');
}

static _Thread_local const char* g_lent;
static _Thread_local size_t      g_lent_n;

/* a segment's root is lent only where the database spells it inside the current directory (database/segment.md:54-73) */
void q_io_guard_lend(const char* root, size_t rn) {
    int inside = root && q_io_path_inside(root, rn);
    g_lent = inside ? root : NULL;
    g_lent_n = inside ? rn : 0;
}

static int io_allowed(const char* p, size_t n) {
    return !ray_eval_get_restricted() || io_admits(NULL, 0, p, n) || (g_lent && io_admits(g_lent, g_lent_n, p, n));
}

static ray_t* io_refused(const char* p, size_t n, const char* rule) {
    char msg[PATH_MAX + 64];
    int k = snprintf(msg, sizeof msg, "access: %.*s (%s)", (int)(n < PATH_MAX ? n : PATH_MAX), p, rule);
    ray_t* why = ray_charv(msg, k < (int)sizeof msg ? k : (int)sizeof msg - 1);
    ray_t* e = q_err_signal(QE_ACCESS, why);
    if (why && !RAY_IS_ERR(why)) ray_release(why);
    return e;
}

#define IO_CWD_ONLY "restricted to the current directory"

ray_t* q_io_guard(const char* p, size_t n) { return io_allowed(p, n) ? NULL : io_refused(p, n, IO_CWD_ONLY); }

ray_t* q_io_guard_write(const char* p, size_t n) {
    return ray_eval_get_blocked() ? io_refused(p, n, "writes are blocked") : q_io_guard(p, n);
}

int q_io_locked(void) {
    return ray_eval_get_restricted() || ray_eval_get_blocked() || q_sys_started_restricted() || q_sys_started_blocked();
}

static ray_t* io_guard_str(ray_t* pathstr) { return q_io_guard(ray_str_ptr(pathstr), ray_str_len(pathstr)); }

ray_t* q_io_file_path(ray_t* x) {
    const char* p;
    size_t n;
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);                       /* borrowed */
        if (!s) return NULL;
        p = ray_str_ptr(s);
        n = ray_str_len(s);
        if (n < 2 || p[0] != ':') return NULL;                /* not a file symbol */
    } else if (x && x->type == -RAY_STR) {
        p = ray_str_ptr(x);
        n = ray_str_len(x);
        if (!p) return NULL;
    } else {
        return NULL;
    }
    if (n && p[0] == ':') { p++; n--; }
    return ray_str(p, n);
}

ray_t* q_io_path_operand(ray_t* x) {
    ray_t* s = x && x->type == -RAY_SYM ? ray_sym_str(x->i64) : x;   /* borrowed */
    return q_io_file_path(s);
}

/* Bytes as the filesystem counts them; q_io_file_size adds the container law. */
static int64_t io_stat_size(ray_t* pathstr) {
    const char* p = ray_str_ptr(pathstr);
    if (!p) return -1;
#ifdef RAY_OS_WINDOWS
    struct _stat64 st;                  /* 64-bit twin: st_size past 2 GiB */
    return _stat64(p, &st) == 0 ? (int64_t)st.st_size : -1;
#else
    struct stat st;
    return stat(p, &st) == 0 ? (int64_t)st.st_size : -1;
#endif
}

void q_io_mkdir_parents(const char* path, size_t n) {
    while (n > 0 && path[n - 1] != '/') n--;
    if (n <= 1) return;                 /* no parent, or the root itself */
    char* dir = (char*)malloc(n);
    if (!dir) return;
    memcpy(dir, path, n - 1);
    dir[n - 1] = '\0';
    (void)ray_mkdir_p(dir);
    free(dir);
}

int q_io_fwrite(FILE* fp, const void* bytes, size_t n) {
    uint8_t buf[65536];
    for (size_t off = 0; off < n; ) {
        size_t c = n - off < sizeof buf ? n - off : sizeof buf;
        memcpy(buf, (const uint8_t*)bytes + off, c);
        if (fwrite(buf, 1, c, fp) != c) return -1;
        off += c;
    }
    return 0;
}

char* q_io_stage_name(const char* path) {
    static atomic_uint seq;
#ifdef RAY_OS_WINDOWS
    unsigned long pid = GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    unsigned n = atomic_fetch_add(&seq, 1) + 1;
    size_t cap = strlen(path) + 48;
    char* s = (char*)malloc(cap);
    if (s) snprintf(s, cap, "%s.tmp.%lu.%u", path, pid, n);
    return s;
}

#ifdef RAY_OS_WINDOWS
/* Windows refuses to shorten a file a live view maps (a held splay column), but lets it be renamed: the view keeps
 * the old file under a side name and `path` becomes a fresh copy of its first `len` bytes. */
static int io_truncate_aside(const char* path, int64_t len) {
    char* aside = q_io_stage_name(path);
    if (!aside) return 1;
    if (!MoveFileExA(path, aside, 0)) { free(aside); return 1; }
    FILE* in = fopen(aside, "rb");
    FILE* out = in ? fopen(path, "wb") : NULL;
    int bad = !out;
    uint8_t buf[65536];
    for (int64_t left = len; !bad && left > 0; ) {
        size_t c = left < (int64_t)sizeof buf ? (size_t)left : sizeof buf;
        bad = fread(buf, 1, c, in) != c || fwrite(buf, 1, c, out) != c;
        left -= (int64_t)c;
    }
    if (out && fclose(out) != 0) bad = 1;
    if (in) fclose(in);
    if (bad) { DeleteFileA(path); MoveFileExA(aside, path, 0); }
    else DeleteFileA(aside);            /* pending while the view lives */
    free(aside);
    return bad;
}
#endif

ray_t* q_io_truncate(const char* path, int64_t len) {
    FILE* fp = fopen(path, "r+b");
    int bad = !fp || ftruncate(fileno(fp), (off_t)len) != 0;
    if (fp && fclose(fp) != 0) bad = 1;
#ifdef RAY_OS_WINDOWS
    if (bad && fp) bad = io_truncate_aside(path, len);
#endif
    return bad ? q_err(QE_IO) : NULL;
}

int q_io_write_fd(int fd, const void* bytes, size_t n) {
    const char* p = (const char*)bytes;
    while (n) {
        size_t  c = n < INT_MAX ? n : INT_MAX;
        ssize_t w = write(fd, p, c);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

ssize_t q_io_read_fd(int fd, void* buf, size_t n) {
    ssize_t r;
    do r = read(fd, buf, n);
    while (r < 0 && errno == EINTR && !q_eval_stop_pending);
    return r;
}

/* A POSIX record lock, so a kdb writer's lockf waits on it and it on lockf.  The open-file-description form is
 * dropped by this descriptor's close alone; the classic form by any close of the file, hence the one descriptor. */
int q_io_open_locked(const char* path) {
    int fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_BINARY, 0666);
    if (fd < 0) return -1;
#ifdef RAY_OS_WINDOWS
    /* a Windows lock is MANDATORY over its range: on the file's own bytes it fails every other reader, so the one
     * byte locked lies past any file's end — writers still queue on it, as they do on the advisory POSIX lock */
    OVERLAPPED ov = { .OffsetHigh = 0x40000000 };
    int ok = LockFileEx((HANDLE)_get_osfhandle(fd), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov) != 0;
#else
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    int ok = 0, cmds[2] = { F_SETLKW, F_SETLKW };
#ifdef F_OFD_SETLKW
    cmds[0] = F_OFD_SETLKW;
#endif
    for (int k = 0; k < 2 && !ok; k++) {
        do ok = fcntl(fd, cmds[k], &fl) == 0;
        while (!ok && errno == EINTR && !q_eval_stop_pending);   /* Ctrl+C ends the wait */
        if (errno != EINVAL) break;              /* only a kernel without the first form tries the second */
    }
#endif
    if (!ok) { close(fd); return -1; }
    return fd;
}

static ray_t* io_guard_resource(ray_t* pathstr, int write);

ray_t* q_io_write_all(ray_t* pathstr, const void* bytes, size_t n) {
    const char* p = ray_str_ptr(pathstr);
    if (!p) return q_err(QE_DOMAIN);
    ray_t* refused = io_guard_resource(pathstr, 1);
    if (refused) return refused;
    if (q_s3_is(p, ray_str_len(pathstr)) || q_io_is_member(pathstr)) return q_err(QE_NYI);
    q_io_mkdir_parents(p, ray_str_len(pathstr));
    FILE* fp = fopen(p, "wb");
    if (!fp) return q_err(QE_IO);
    int bad = q_io_fwrite(fp, bytes, n);
    fclose(fp);
    return bad ? q_err(QE_IO) : NULL;
}

/* ---- the reads ---------------------------------------------------------- */

#define ZIP_MAGIC     "kxzipped"
#define ZIP_MAGIC_LEN 8
#define ZIP_HEAD_LEN  32   /* the tail's fixed head; then (len i32; alg i32) per block, then numBlocks i64 */
#define ZIP_TAIL_LEN(nb) (ZIP_HEAD_LEN + 8 * (nb) + 8)

int64_t q_io_clamp(int64_t n, int64_t* off, int64_t want) {
    if (*off < 0) *off = 0;
    if (*off > n) *off = n;
    return (want < 0 || want > n - *off) ? n - *off : want;
}

/* Bytes exactly as they sit on disk: the size is fstat's on the open fd, so it cannot change under the read.  The
 * open never waits, so a fifo in a directory being loaded is refused rather than blocking for a writer. */
static ray_t* io_local_read(ray_t* pathstr, int64_t off, int64_t want) {
    const char* p = ray_str_ptr(pathstr);
    if (!p) return q_err(QE_DOMAIN);
    ray_t* refused = io_guard_str(pathstr);
    if (refused) return refused;
    int fd = open(p, O_RDONLY | O_BINARY | O_NONBLOCK);
    struct stat st;
    if (fd < 0) return q_err(QE_IO);
    FILE* fp = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) ? fdopen(fd, "rb") : NULL;
    if (!fp) {
        close(fd);
        return q_err(QE_IO);
    }
    int64_t take = q_io_clamp((int64_t)st.st_size, &off, want);
    uint8_t* buf = take > 0 ? (uint8_t*)malloc((size_t)take) : NULL;
    size_t got = 0;
    ray_t* out;
    if (take > 0 && (!buf || fseek(fp, (long)off, SEEK_SET) != 0)) {
        out = q_err(buf ? QE_IO : QE_OOM);
    } else {
        if (buf) got = fread(buf, 1, (size_t)take, fp);
        out = ray_vec_from_raw(RAY_BYTE_ONLY, buf, (int64_t)got);
    }
    free(buf);
    fclose(fp);
    return out;
}

static ray_t* io_local_size(ray_t* pathstr) {
    ray_t* refused = io_guard_str(pathstr);
    if (refused) return refused;
    int64_t n = io_stat_size(pathstr);
    return n < 0 ? q_err(QE_IO) : ray_i64(n);
}

/* Absent is determined POSITIVELY, never inferred from a failed listing: a directory we cannot open (EACCES) or that
 * runs the allocator out is an ERROR, not a missing path (codex review 2026-07-30).  The quarry's ls sorts ascending,
 * drops `.`/`..`, and yields the empty SYMBOL vector for an empty directory. */
static ray_t* io_local_list(ray_t* pathstr) {
    ray_t* refused = io_guard_str(pathstr);
    if (refused) return refused;
    struct stat st;
    if (stat(ray_str_ptr(pathstr), &st) != 0)
        return errno == ENOENT || errno == ENOTDIR ? ray_list_new(1) : q_err(QE_IO);
    return S_ISDIR(st.st_mode) ? ray_fs_list_fn(pathstr) : NULL;
}

static ray_t* io_none_list(ray_t* pathstr) { (void)pathstr; return ray_list_new(1); }

/* THE byte reader: each transport is these three operations on bytes as stored, and every verb reaches a transport
 * through io_src alone.  The kxzip container is read over them, whatever carries it.  `batch` is how many bytes
 * beside those asked are worth fetching to save a request: none where a read is a syscall, the container's magic
 * with a read near the front and its whole tail in one read where a read is a round trip. */
typedef struct {
    ray_t* (*size)(ray_t* pathstr);
    ray_t* (*read)(ray_t* pathstr, int64_t off, int64_t want);
    ray_t* (*list)(ray_t* pathstr);
    int64_t batch;
} io_src_t;

static ray_t* io_arch_size(ray_t* pathstr);
static ray_t* io_arch_read(ray_t* pathstr, int64_t off, int64_t want);
static ray_t* io_arch_list(ray_t* pathstr);

static const io_src_t IO_LOCAL = { io_local_size, io_local_read, io_local_list, 0 };
static const io_src_t IO_S3    = { q_s3_size, q_s3_read, q_s3_list, 65536 };
static const io_src_t IO_HTTP  = { q_http_client_size, q_http_client_read_slice, io_none_list, 65536 };
static const io_src_t IO_ARCH  = { io_arch_size, io_arch_read, io_arch_list, 0 };

static const io_src_t* io_transport(ray_t* pathstr) {
    const char* p = ray_str_ptr(pathstr);
    size_t n = ray_str_len(pathstr);
    return q_s3_is(p, n) ? &IO_S3 : q_http_client_scheme_is(p, n) ? &IO_HTTP : &IO_LOCAL;
}

/* ---- containers: detected by name, confirmed as files ---- */

enum { ARCH_NONE, ARCH_ZIP, ARCH_TAR, ARCH_TGZ };

static int io_ends_ci(const char* p, size_t n, const char* ext) {
    size_t k = strlen(ext);
    if (n <= k || p[n - k - 1] == '/') return 0;
    for (size_t i = 0; i < k; i++)
        if ((char)tolower((unsigned char)p[n - k + i]) != ext[i]) return 0;
    return 1;
}

/* the archive a component's name claims, by its ending in any case */
static int io_arch_kind(const char* p, size_t n) {
    return io_ends_ci(p, n, ".zip") ? ARCH_ZIP : io_ends_ci(p, n, ".tar") ? ARCH_TAR
         : io_ends_ci(p, n, ".tar.gz") || io_ends_ci(p, n, ".tgz") ? ARCH_TGZ : ARCH_NONE;
}

/* does a component short of the last claim an archive?  Only such a path spends a stat or a HEAD */
static int io_arch_claimed(const char* p, size_t n) {
    for (size_t i = 1; i + 1 < n; i++)
        if (p[i] == '/' && io_arch_kind(p, i)) return 1;
    return 0;
}

static const io_src_t* io_src(ray_t* pathstr) {
    return io_arch_claimed(ray_str_ptr(pathstr), ray_str_len(pathstr)) ? &IO_ARCH : io_transport(pathstr);
}

ray_t* q_io_list(ray_t* pathstr) { return io_src(pathstr)->list(pathstr); }
ray_t* q_io_stored_size(ray_t* pathstr) { return io_src(pathstr)->size(pathstr); }
ray_t* q_io_stored_read(ray_t* pathstr, int64_t off, int64_t want) { return io_src(pathstr)->read(pathstr, off, want); }

/* The stored length of a path that is a FILE, or -1: a directory, an S3 prefix, a URL answering no length, or
 * nothing at all.  A probe, so whatever it signalled is dropped with its text. */
static int64_t io_file_len(ray_t* pathstr) {
    const io_src_t* s = io_src(pathstr);
    if (s == &IO_LOCAL) {
        if (!io_allowed(ray_str_ptr(pathstr), ray_str_len(pathstr))) return -1;
#ifdef RAY_OS_WINDOWS
        struct _stat64 st;
        return _stat64(ray_str_ptr(pathstr), &st) == 0 && (st.st_mode & _S_IFREG) ? (int64_t)st.st_size : -1;
#else
        struct stat st;
        return stat(ray_str_ptr(pathstr), &st) == 0 && S_ISREG(st.st_mode) ? (int64_t)st.st_size : -1;
#endif
    }
    ray_t* sz = s->size(pathstr);
    int64_t n = RAY_IS_ERR(sz) ? -1 : sz->i64;
    ray_release(sz);
    if (n < 0) q_err_drop();
    return n;
}

typedef struct {
    int         kind;
    ray_t*      arch;                       /* owned */
    int64_t     asize;
    const char* m;
    size_t      mn;
} io_split_t;

/* THE detection law: the archive a path walks into is the LAST component short of the end that claims one by name
 * and is a file — one stat, or one HEAD, per such component; a directory so named is a folder.  kind ARCH_NONE when
 * none is. */
static void io_split(ray_t* pathstr, io_split_t* sp) {
    const char* p = ray_str_ptr(pathstr);
    size_t n = ray_str_len(pathstr);
    memset(sp, 0, sizeof *sp);
    for (size_t i = 1; i + 1 < n; i++) {
        int k = p[i] == '/' ? io_arch_kind(p, i) : ARCH_NONE;
        ray_t* a = k ? ray_str(p, i) : NULL;
        if (!a || RAY_IS_ERR(a)) continue;
        int64_t len = io_file_len(a);
        if (len < 0) { ray_release(a); continue; }
        if (sp->arch) ray_release(sp->arch);
        *sp = (io_split_t){ k, a, len, p + i + 1, n - i - 1 };
    }
}

static ray_t* io_split_member(const io_split_t* sp, q_io_stream_t** s, int64_t* size) {
    return sp->kind == ARCH_ZIP ? q_zip_member(sp->arch, sp->asize, sp->m, sp->mn, s, size)
                                : q_tar_member(sp->arch, sp->kind == ARCH_TGZ, sp->m, sp->mn, s, size);
}

/* the member a path names: *s NULL when no component is an archive, and the path is its transport's */
static ray_t* io_member(ray_t* pathstr, q_io_stream_t** s, int64_t* size) {
    *s = NULL;
    io_split_t sp;
    io_split(pathstr, &sp);
    ray_t* bad = sp.kind ? io_split_member(&sp, s, size) : NULL;
    if (sp.arch) ray_release(sp.arch);
    return bad;
}

ray_t* q_io_member_stream(ray_t* pathstr, q_io_stream_t** s) {
    int64_t size;
    *s = NULL;
    return io_arch_claimed(ray_str_ptr(pathstr), ray_str_len(pathstr)) ? io_member(pathstr, s, &size) : NULL;
}

int q_io_is_member(ray_t* pathstr) {
    if (!io_arch_claimed(ray_str_ptr(pathstr), ray_str_len(pathstr))) return 0;
    io_split_t sp;
    io_split(pathstr, &sp);
    if (sp.arch) ray_release(sp.arch);
    return sp.kind != ARCH_NONE;
}

ray_t* q_io_member_fn(ray_t* x) {
    ray_t* path = q_io_path_operand(x);
    if (!path) return q_err(QE_TYPE);
    int m = q_io_is_member(path);
    ray_release(path);
    return ray_bool(m);
}

ray_t* q_io_no_member(ray_t* arch, const char* m, size_t mn) {
    ray_t* e = q_err(QE_DOMAIN);
    char buf[700];
    int k = snprintf(buf, sizeof buf, "domain. %.*s has no entry %.*s", (int)ray_str_len(arch), ray_str_ptr(arch),
                     (int)mn, m);
    ray_t* t = k > 0 ? ray_charv(buf, k < (int)sizeof buf ? k : (int)sizeof buf - 1) : NULL;
    if (t && !RAY_IS_ERR(t)) q_err_put(t);
    return e;
}

static ray_t* io_arch_size(ray_t* pathstr) {
    q_io_stream_t* s;
    int64_t n = 0;
    ray_t* bad = io_member(pathstr, &s, &n);
    if (bad) return bad;
    if (!s) return io_transport(pathstr)->size(pathstr);
    q_io_stream_close(s);
    return ray_i64(n);
}

static ray_t* io_arch_read(ray_t* pathstr, int64_t off, int64_t want) {
    q_io_stream_t* s;
    int64_t n = 0;
    ray_t* bad = io_member(pathstr, &s, &n);
    if (bad) return bad;
    if (!s) return io_transport(pathstr)->read(pathstr, off, want);
    ray_t* out = q_io_stream_slice(s, off < 0 ? 0 : off, want);
    q_io_stream_close(s);
    return out;
}

static ray_t* io_arch_list(ray_t* pathstr) {
    io_split_t sp;
    io_split(pathstr, &sp);
    if (!sp.kind) return io_transport(pathstr)->list(pathstr);
    ray_release(sp.arch);
    return NULL;
}

/* An archive named whole as a table: its one file entry, or 'type naming them all. */
static ray_t* io_arch_table(ray_t* fsym, ray_t* path, int kind) {
    int64_t len = io_file_len(path);
    if (len < 0) return NULL;
    ray_t* names = kind == ARCH_ZIP ? q_zip_names(path, len) : q_tar_names(path, kind == ARCH_TGZ);
    if (RAY_IS_ERR(names)) return names;
    int64_t k = q_count(names);
    ray_t** e = (ray_t**)ray_data(names);
    ray_t* out;
    if (k == 1) {
        ray_t* fs = ray_sym_str(fsym->i64);
        size_t fn = ray_str_len(fs);
        int64_t en = q_count(e[0]);
        char* buf = (char*)malloc(fn + 1 + (size_t)en);
        if (!buf) { ray_release(names); return q_err(QE_OOM); }
        memcpy(buf, ray_str_ptr(fs), fn);
        buf[fn] = '/';
        memcpy(buf + fn + 1, ray_data(e[0]), (size_t)en);
        ray_t* inner = ray_sym(ray_sym_intern_runtime(buf, fn + 1 + (size_t)en));
        free(buf);
        out = q_io_resource_table(inner);
        ray_release(inner);
    } else {
        char buf[1024];
        int at = snprintf(buf, sizeof buf, "type. %.*s holds %lld entries:", (int)ray_str_len(path), ray_str_ptr(path),
                          (long long)k);
        for (int64_t i = 0; i < k && at > 0 && at < (int)sizeof buf; i++)
            at += snprintf(buf + at, sizeof buf - (size_t)at, " %.*s", (int)q_count(e[i]), (const char*)ray_data(e[i]));
        out = q_err(QE_TYPE);
        ray_t* t = at > 0 ? ray_charv(buf, at < (int)sizeof buf ? at : (int)sizeof buf - 1) : NULL;
        if (t && !RAY_IS_ERR(t)) q_err_put(t);
    }
    ray_release(names);
    return out;
}

/* ---- forward streams ---- */

static ray_t* io_next(q_io_stream_t* s, uint8_t* out, size_t want, size_t* got) {
    ray_t* bad = s->next(s, out, want, got);
    if (!bad) s->pos += (int64_t)*got;
    return bad;
}

void q_io_stream_close(q_io_stream_t* s) {
    if (s) s->free(s);
}

/* a buffer grown as the bytes come, never sized from a header's promise alone */
ray_t* q_io_stream_slice(q_io_stream_t* s, int64_t off, int64_t want) {
    if (off < s->pos) return q_err(QE_DOMAIN);
    if (s->seekable) s->pos = off;
    uint8_t* buf = NULL;
    size_t len = 0, cap = 0;
    ray_t* bad = NULL;
    for (;;) {
        int skip = s->pos < off;
        int64_t left = skip ? off - s->pos : want < 0 ? INT64_MAX : want - (int64_t)len;
        if (!left) break;
        size_t step = left < ((int64_t)1 << 20) ? (size_t)left : (size_t)1 << 20;
        size_t at = skip ? 0 : len;
        if (at + step > cap) {
            size_t nc = cap ? cap * 2 : step;
            while (nc < at + step) nc *= 2;
            uint8_t* nb = (uint8_t*)realloc(buf, nc);
            if (!nb) { bad = q_err(QE_OOM); break; }
            buf = nb;
            cap = nc;
        }
        size_t got = 0;
        if ((bad = io_next(s, buf + at, step, &got)) || !got) break;
        if (!skip) len += got;
    }
    ray_t* out = bad ? bad : ray_vec_from_raw(RAY_BYTE_ONLY, buf ? buf : (const uint8_t*)"", (int64_t)len);
    free(buf);
    return out;
}

/* a range read through `read`: a range promised by a container and cut short is 'corrupt */
typedef struct {
    q_io_stream_t io;
    ray_t*  path;
    int64_t base, limit;
    ray_t*  (*read)(ray_t* pathstr, int64_t off, int64_t want);
} io_range_t;

static ray_t* io_range_next(q_io_stream_t* s, uint8_t* out, size_t want, size_t* got) {
    io_range_t* r = (io_range_t*)s;
    *got = 0;
    int64_t left = r->limit < 0 ? INT64_MAX : r->limit - s->pos;
    if (left <= 0) return NULL;
    if ((int64_t)want > left) want = (size_t)left;
    ray_t* b = r->read(r->path, r->base + s->pos, (int64_t)want);
    if (RAY_IS_ERR(b)) return b;
    *got = (size_t)q_count(b);
    memcpy(out, ray_data(b), *got);
    ray_release(b);
    return r->limit >= 0 && *got < want ? q_err(QE_CORRUPT) : NULL;
}

static void io_range_free(q_io_stream_t* s) {
    ray_release(((io_range_t*)s)->path);
    free(s);
}

static q_io_stream_t* io_range_new(ray_t* pathstr, int64_t base, int64_t limit,
                                   ray_t* (*read)(ray_t* pathstr, int64_t off, int64_t want)) {
    io_range_t* r = (io_range_t*)calloc(1, sizeof *r);
    if (!r) return NULL;
    ray_retain(pathstr);
    *r = (io_range_t){ { io_range_next, io_range_free, 1, 0 }, pathstr, base, limit, read };
    return &r->io;
}

q_io_stream_t* q_io_range_stream(ray_t* pathstr, int64_t base, int64_t limit) {
    return io_range_new(pathstr, base, limit, q_io_stored_read);
}

/* bytes held whole, for a resource that cannot serve cheap repeated ranges */
typedef struct { q_io_stream_t io; ray_t* all; } io_held_t;

static ray_t* io_held_next(q_io_stream_t* s, uint8_t* out, size_t want, size_t* got) {
    ray_t* all = ((io_held_t*)s)->all;
    int64_t at = s->pos;
    *got = (size_t)q_io_clamp(q_count(all), &at, (int64_t)want);
    if (*got) memcpy(out, (const uint8_t*)ray_data(all) + at, *got);
    return NULL;
}

static void io_held_free(q_io_stream_t* s) {
    ray_release(((io_held_t*)s)->all);
    free(s);
}

typedef struct { q_io_stream_t io; q_io_stream_t* in; int64_t limit; } io_window_t;

static ray_t* io_window_next(q_io_stream_t* s, uint8_t* out, size_t want, size_t* got) {
    io_window_t* w = (io_window_t*)s;
    *got = 0;
    if ((int64_t)want > w->limit - s->pos) want = s->pos < w->limit ? (size_t)(w->limit - s->pos) : 0;
    ray_t* bad = want ? io_next(w->in, out, want, got) : NULL;
    return bad ? bad : want && !*got ? q_err(QE_CORRUPT) : NULL;
}

static void io_window_free(q_io_stream_t* s) {
    q_io_stream_close(((io_window_t*)s)->in);
    free(s);
}

q_io_stream_t* q_io_window_stream(q_io_stream_t* inner, int64_t limit) {
    io_window_t* w = (io_window_t*)calloc(1, sizeof *w);
    if (!w) { q_io_stream_close(inner); return NULL; }
    *w = (io_window_t){ { io_window_next, io_window_free, 0, 0 }, inner, limit };
    return &w->io;
}

#define IO_INBUF ((size_t)1 << 20)

typedef struct {
    q_io_stream_t io;
    q_io_stream_t* in;
    q_gz_raw_t*   z;
    uint8_t*      buf;
    size_t        len, at;
    int           eof, live;
} io_gunzip_t;

/* zlib stops at a member's end; the next member, if any bytes follow, starts a fresh inflate */
static ray_t* io_gunzip_next(q_io_stream_t* s, uint8_t* out, size_t want, size_t* got) {
    io_gunzip_t* g = (io_gunzip_t*)s;
    *got = 0;
    while (*got < want) {
        if (g->at == g->len && !g->eof) {
            ray_t* bad = io_next(g->in, g->buf, IO_INBUF, &g->len);
            if (bad) return bad;
            g->at = 0;
            g->eof = g->len == 0;
        }
        if (g->at == g->len) return g->live ? q_err(QE_CORRUPT) : NULL;
        if (!g->live && q_gz_raw_reset(g->z)) return q_err(QE_CORRUPT);
        g->live = 1;
        size_t used = 0, made = 0;
        int rc = q_gz_raw_step(g->z, g->buf + g->at, g->len - g->at, &used, out + *got, want - *got, &made);
        g->at += used;
        *got += made;
        if (rc < 0 || (rc == 0 && !used && !made)) return q_err(QE_CORRUPT);
        if (rc == 1) g->live = 0;
    }
    return NULL;
}

static void io_gunzip_free(q_io_stream_t* s) {
    io_gunzip_t* g = (io_gunzip_t*)s;
    q_io_stream_close(g->in);
    q_gz_raw_free(g->z);
    free(g->buf);
    free(g);
}

ray_t* q_io_gunzip_stream(q_io_stream_t* inner, q_io_stream_t** out) {
    io_gunzip_t* g = (io_gunzip_t*)calloc(1, sizeof *g);
    q_gz_raw_t* z = q_gz_member_new();
    uint8_t* buf = (uint8_t*)malloc(IO_INBUF);
    if (!g || !z || !buf) {
        free(g);
        q_gz_raw_free(z);
        free(buf);
        q_io_stream_close(inner);
        *out = NULL;
        return q_err(QE_OOM);
    }
    *g = (io_gunzip_t){ .io = { io_gunzip_next, io_gunzip_free, 0, 0 }, .in = inner, .z = z, .buf = buf };
    *out = &g->io;
    return NULL;
}

ray_t* q_io_resource_stream(ray_t* pathstr, int gzip, q_io_stream_t** out) {
    q_io_stream_t* s = NULL;
    ray_t* bad = q_io_member_stream(pathstr, &s);
    if (bad) return bad;
    if (!s && !q_io_resource_chunkable(pathstr)) {
        ray_t* all = q_io_resource_read(pathstr, 0, -1);
        if (!all || RAY_IS_ERR(all)) return all ? all : q_err(QE_OOM);
        io_held_t* h = (io_held_t*)calloc(1, sizeof *h);
        if (!h) { ray_release(all); return q_err(QE_OOM); }
        *h = (io_held_t){ { io_held_next, io_held_free, 1, 0 }, all };
        s = &h->io;
    } else if (!s && !(s = io_range_new(pathstr, 0, -1, q_io_resource_read))) {
        return q_err(QE_OOM);
    }
    if (!gzip) { *out = s; return NULL; }
    return q_io_gunzip_stream(s, out);
}

static int io_has_magic(ray_t* b) {
    return q_count(b) >= ZIP_MAGIC_LEN && memcmp(ray_data(b), ZIP_MAGIC, ZIP_MAGIC_LEN) == 0;
}

/* does the object open with the kxzip magic?  1/0, or -1 with *err owned.  The magic alone: a truncated container
 * is still one, and must say 'corrupt. */
static int io_magic(const io_src_t* s, ray_t* pathstr, ray_t** err) {
    ray_t* m = s->read(pathstr, 0, ZIP_MAGIC_LEN);
    if (RAY_IS_ERR(m)) { *err = m; return -1; }
    int w = io_has_magic(m);
    ray_release(m);
    return w;
}

/* `want` bytes from `off` of an OWNED whole, under q_io_clamp's law; consumes the whole */
static ray_t* io_slice(ray_t* all, int64_t off, int64_t want) {
    if (off == 0 && want < 0) return all;
    int64_t take = q_io_clamp(q_count(all), &off, want);
    ray_t* out = ray_vec_from_raw(RAY_BYTE_ONLY, (const uint8_t*)ray_data(all) + off, take);
    ray_release(all);
    return out;
}

static ray_t* io_zip_slice(const io_src_t* s, ray_t* pathstr, int64_t off, int64_t want);

ray_t* q_io_read_slice(ray_t* pathstr, int64_t off, int64_t want, int* zipped) {
    if (zipped) *zipped = 0;
    const io_src_t* s = io_src(pathstr);
    if (off < 0) off = 0;
    int64_t lo = off <= s->batch ? 0 : off, span = want < 0 || want > INT64_MAX - off ? -1 : off - lo + want;
    ray_t* b = s->read(pathstr, lo, lo || span < 0 || span >= ZIP_MAGIC_LEN ? span : ZIP_MAGIC_LEN);
    if (RAY_IS_ERR(b)) return b;
    ray_t* err = NULL;
    int wrapped = lo ? io_magic(s, pathstr, &err) : io_has_magic(b);
    if (wrapped <= 0) {
        if (err) { ray_release(b); return err; }
        return io_slice(b, off - lo, want);
    }
    if (zipped) *zipped = 1;
    if (want >= 0) { ray_release(b); return io_zip_slice(s, pathstr, off, want); }
    if (lo) {
        ray_release(b);
        b = s->read(pathstr, 0, -1);
        if (RAY_IS_ERR(b)) return b;
    }
    ray_t* plain = q_io_unzip((const uint8_t*)ray_data(b), (size_t)q_count(b));
    ray_release(b);
    if (!plain) return q_err(QE_CORRUPT);       /* magic promised what unzip denies */
    return RAY_IS_ERR(plain) ? plain : io_slice(plain, off, want);
}

/* ---- the resource-read seam ---- */

/* A scheme-less path IS the file transport, so the only question is whether one
 * the http client speaks is spelled out in front. */
static int io_is_http(ray_t* pathstr) {
    const char* p = ray_str_ptr(pathstr);
    return q_http_client_scheme_is(p, p ? ray_str_len(pathstr) : 0);
}

/* the remote schemes DuckDB's extensions speak (ADR 2026-09-15 § B3): the bytes come and go through .duckdb.i.read1 /
 * .duckdb.i.write0, bound by `\l pq` like the .parquet doors, except that s3:// is READ by the byte reader; az:// is
 * handed over, unsupported */
static int io_is_remote(ray_t* pathstr) {
    static const char* const S[] = { "s3://", "s3a://", "s3n://", "gcs://", "gs://", "r2://", "hf://", "az://" };
    const char* p = ray_str_ptr(pathstr);
    size_t n = p ? ray_str_len(pathstr) : 0;
    for (size_t i = 0; i < sizeof S / sizeof *S; i++)
        if (n > strlen(S[i]) && memcmp(p, S[i], strlen(S[i])) == 0) return 1;
    return 0;
}

static int io_is_url(ray_t* pathstr) { return io_is_http(pathstr) || io_is_remote(pathstr); }

/* a URL has no place in the current directory, so restricted mode refuses one outright, as blocked mode refuses a
 * write to one */
static ray_t* io_guard_resource(ray_t* pathstr, int write) {
    if (io_is_url(pathstr)) return ray_eval_get_restricted() || (write && ray_eval_get_blocked()) ? q_err(QE_ACCESS) : NULL;
    return write ? q_io_guard_write(ray_str_ptr(pathstr), ray_str_len(pathstr)) : io_guard_str(pathstr);
}

/* DuckDB expands a glob and a leading ~ itself, so such a spelling names no one file the rule could judge */
static ray_t* io_guard_duckdb(ray_t* pathstr, int write) {
    const char* p = ray_str_ptr(pathstr);
    size_t n = ray_str_len(pathstr);
    int expands = n && p[0] == '~';
    for (size_t i = 0; i < n && !expands; i++) expands = p[i] == '*' || p[i] == '?' || p[i] == '[' || p[i] == '{';
    return expands && ray_eval_get_restricted() ? io_refused(p, n, IO_CWD_ONLY) : io_guard_resource(pathstr, write);
}

int q_io_resource_chunkable(ray_t* pathstr) {
    return !io_is_url(pathstr) && !io_arch_claimed(ray_str_ptr(pathstr), ray_str_len(pathstr));
}

/* the length of the path that carries a format claim: on a URL a query or fragment names no format */
static size_t io_claim_len(ray_t* pathstr) {
    const char* p = ray_str_ptr(pathstr);
    size_t n = ray_str_len(pathstr);
    if (io_is_url(pathstr)) for (size_t i = 0; i < n; i++) if (p[i] == '?' || p[i] == '#') return i;
    return n;
}

/* read_blob fetches the whole object, so a ranged read slices in memory */
static ray_t* io_remote_read(ray_t* pathstr, int64_t off, int64_t want) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    ray_t* url = q_str_charv_of_str(pathstr);
    ray_t* all = q_eval_apply_call_name(".duckdb.i.read1", 15, &url, 1);
    ray_release(url);
    if (!all || RAY_IS_ERR(all)) return all;
    if (all->type != RAY_BYTE_ONLY) { ray_release(all); return q_err(QE_TYPE); }
    return io_slice(all, off, want);
}

/* A suffix is FINAL and needs something in front of it: a resource named exactly
 * `.json` is a dotfile, not a format claim.  Case-sensitive, like the names q writes. */
static int io_suffix_is(const char* p, size_t n, const char* ext) {
    size_t k = strlen(ext);
    return n > k && memcmp(p + n - k, ext, k) == 0;
}

/* `.parquet.read[fsym;();()]` by name at call time: standard library, never linked; unbound is '.parquet.read */
static ray_t* io_parquet_table(ray_t* fsym, ray_t* path) {
    ray_t* refused = io_guard_duckdb(path, 0);
    if (refused) return refused;
    ray_t* none = ray_list_new(0);
    ray_t* args[3] = { fsym, none, none };
    ray_t* out = q_eval_apply_call_name(".parquet.read", 13, args, 3);
    ray_release(none);
    return out;
}

int q_io_parquet_named(ray_t* fsym) {
    ray_t* path = q_io_is_fsym(fsym) ? q_io_file_path(fsym) : NULL;
    if (!path) return 0;
    int is = io_suffix_is(ray_str_ptr(path), io_claim_len(path), ".parquet");
    ray_release(path);
    return is;
}

/* `.parquet.i.select[fsym;args;env]` by name, as io_parquet_table calls `.parquet.read`; its `::` is the door declining */
ray_t* q_io_parquet_select(ray_t** args, int64_t n, ray_t* env) {
    ray_t* path = q_io_file_path(args[0]);
    if (!path) return NULL;
    ray_t* refused = io_guard_duckdb(path, 0);
    ray_release(path);
    if (refused) return refused;
    ray_t* tree = ray_list_new(n);
    for (int64_t i = 0; i < n && tree && !RAY_IS_ERR(tree); i++) tree = ray_list_append(tree, args[i]);
    if (!tree || RAY_IS_ERR(tree)) return tree ? tree : q_err(QE_WSFULL);
    ray_t* cargs[3] = { args[0], tree, env };
    ray_t* out = q_eval_apply_call_name(".parquet.i.select", 17, cargs, 3);
    ray_release(tree);
    if (out && RAY_IS_NULL(out)) { ray_release(out); return NULL; }
    return out;
}

/* The READ side of the on-disk-format classification q_io_set owns for writes,
 * for the formats that decode to a table by their name (format inference: explicit provider or
 * scheme, then explicit format API, then the recognised final suffix, then error — never
 * Content-Type, never magic bytes).  NULL = no recognised tabular format, so the caller falls back
 * to q's own object load; a REMOTE resource has no such fallback, so there an
 * unrecognised suffix fails here rather than as a filesystem miss. */
ray_t* q_io_resource_table(ray_t* fsym) {
    ray_t* path = q_io_file_path(fsym);
    if (!path) return NULL;
    const char* p = ray_str_ptr(path);
    size_t whole = io_claim_len(path), n = whole - (io_suffix_is(p, whole, ".gz") ? 3 : 0);
    int kind = io_arch_kind(p, whole);
    ray_t* out;
    if (kind && (out = io_arch_table(fsym, path, kind))) { ray_release(path); return out; }
    if (io_suffix_is(p, n, ".csv"))         out = q_csv_read_table(fsym, 0);
    else if (io_suffix_is(p, n, ".tsv"))    out = q_csv_read_table(fsym, '\t');
    else if (io_suffix_is(p, n, ".ndjson")) out = q_json_read_table(fsym, Q_JSON_ND);
    else if (io_suffix_is(p, n, ".jsonl"))  out = q_json_read_table(fsym, Q_JSON_ND);
    else if (io_suffix_is(p, n, ".json"))   out = q_json_read_table(fsym, Q_JSON_AUTO);
    else if (io_suffix_is(p, whole, ".parquet")) out = io_parquet_table(fsym, path);
    else out = io_is_url(path) && !q_s3_is(p, n) ? q_err(QE_TYPE) : NULL;
    ray_release(path);
    return out;
}

int q_io_gz_named(ray_t* pathstr) { return io_suffix_is(ray_str_ptr(pathstr), io_claim_len(pathstr), ".gz"); }

ray_t* q_io_resource_read(ray_t* pathstr, int64_t off, int64_t want) {
    if (io_arch_claimed(ray_str_ptr(pathstr), ray_str_len(pathstr)) && !io_is_remote(pathstr))
        return q_io_read_slice(pathstr, off, want, NULL);
    if (io_is_http(pathstr)) return q_http_client_read_slice(pathstr, off, want);
    if (io_is_remote(pathstr) && !q_s3_is(ray_str_ptr(pathstr), ray_str_len(pathstr)))
        return io_remote_read(pathstr, off, want);
    return q_io_read_slice(pathstr, off, want, NULL);
}

ray_t* q_io_file_triple(ray_t* fsym, ray_t* offv, ray_t* wantv, int clamp,
                        ray_t** path, int64_t* off, int64_t* want) {
    *path = q_io_path_operand(fsym);
    if (!*path) return q_err(QE_TYPE);
    *want = -1;
    q_err_e e = QE_TYPE;
    if (q_type_strict_i64(offv, off) && (!wantv || q_type_strict_i64(wantv, want))) {
        if (clamp || (*off >= 0 && (!wantv || *want >= 0))) return NULL;
        e = QE_DOMAIN;
    }
    ray_release(*path);
    *path = NULL;
    return q_err(e);
}

/* ---- the kxzip container (kb/file-compression.md) ----------------------- */

enum { ZIP_NONE = 0, ZIP_IPC = 1, ZIP_GZIP = 2, ZIP_SNAPPY = 3, ZIP_LZ4 = 4, ZIP_ENCRYPTED = 16 };

/* kb/file-compression.md: a power of 2 between 12 and 20. */
#define ZIP_LOG2_MIN 12
#define ZIP_LOG2_MAX 20

typedef struct {
    int64_t uncompressed;   /* plaintext bytes */
    int64_t block_size;     /* logicalBlockSize in BYTES; -21! reports its log2 */
    int64_t num_blocks;
    int32_t compressed;     /* the data area's bytes, magic and tail excluded */
    int32_t algorithm;      /* the tail's codec byte: what was REQUESTED; each block names its own */
    int32_t level;
    int32_t version;        /* the writer-era stamp: 2 (kdb 3.x corpus) / 3 (2025) */
} zip_hdr_t;

static int64_t io_i64(const uint8_t* p) { int64_t v; memcpy(&v, p, 8); return v; }
static int32_t io_i32(const uint8_t* p) { int32_t v; memcpy(&v, p, 4); return v; }

static int32_t zip_log2(int64_t v) {
    int32_t k = 0;
    while (v > 1) { v >>= 1; k++; }
    return k;
}

/* The block count, the tail's last 8 bytes, as far as the file can hold its tail; 0 = it cannot. */
static int64_t zip_nblocks(const uint8_t* last8, size_t file_len) {
    int64_t nb = io_i64(last8);
    if (file_len < ZIP_MAGIC_LEN + ZIP_TAIL_LEN(1) || nb <= 0) return 0;
    return nb <= (int64_t)((file_len - ZIP_MAGIC_LEN - ZIP_TAIL_LEN(0)) / 8) ? nb : 0;
}

static int32_t zip_blen(const uint8_t* t, int64_t k) { return io_i32(t + ZIP_HEAD_LEN + 8 * k); }
static int32_t zip_balg(const uint8_t* t, int64_t k) { return io_i32(t + ZIP_HEAD_LEN + 8 * k + 4); }

/* Decode the tail at `t` of an nb-block container: version i32 | codec u8 | level u8 | pad u16 | decompressedLength
 * i64 | compressedLength-incl-magic i64 | logicalBlockSize i64 | (length i32; algorithm i32) per block | numBlocks
 * i64.  The blocks sit back to back after the magic; the two-block layout is a KDB-X-written file's (uqf
 * tests/q/test_compression_primitives.q:44-49).  `file_len` closes the arithmetic, so a truncation is detectable.
 * NULL on success, else an owned 'corrupt. */
static ray_t* zip_trailer(const uint8_t* t, int64_t nb, size_t file_len, zip_hdr_t* z) {
    z->version      = io_i32(t);
    z->algorithm    = t[4];
    z->level        = t[5];
    z->uncompressed = io_i64(t + 8);
    z->block_size   = io_i64(t + 24);
    z->num_blocks   = nb;
    int64_t sum = 0;
    for (int64_t k = 0; k < nb; k++) {
        if (zip_blen(t, k) < 0) return q_err(QE_CORRUPT);
        sum += zip_blen(t, k);
    }
    if (z->uncompressed < 0 || sum > INT32_MAX ||
        (size_t)sum != file_len - ZIP_MAGIC_LEN - ZIP_TAIL_LEN(nb) || io_i64(t + 16) != ZIP_MAGIC_LEN + sum)
        return q_err(QE_CORRUPT);
    z->compressed = (int32_t)sum;
    return NULL;
}

/* Stored blocks sit anywhere, whatever the file requested (format findings Q4), so each block is decoded by its own
 * algorithm.  'nyi when one names a codec without a decoder, checked before any block is touched: the splay fault
 * handler that decodes later cannot raise. */
static ray_t* zip_codecs(const uint8_t* t, int64_t nb) {
    for (int64_t k = 0; k < nb; k++) {
        int32_t a = zip_balg(t, k);
        if (a != ZIP_NONE && a != ZIP_IPC && a != ZIP_GZIP && a != ZIP_LZ4) return q_err(QE_NYI);
    }
    return NULL;
}

/* An algorithm-1 block is one whole compressed IPC message: 01 00 01 00, its own length, then the payload. */
static int zip_ipc_decode(const uint8_t* src, size_t clen, uint8_t* dst, size_t plain) {
    static const uint8_t head[4] = { 1, 0, 1, 0 };
    if (clen < 12 || memcmp(src, head, 4) != 0 || (uint32_t)io_i32(src + 4) != clen) return -1;
    return q_wire_uncompress_into(src + 8, clen - 8, 0, dst, plain) == (int64_t)plain ? 0 : -1;
}

/* One block into exactly `plain` bytes: 0, else -1.  A zlib, lz4 or IPC stream must end exactly at the block's end.
 * `work` (Q_GZ_INFLATE_WORK bytes, or NULL for the heap) is where a gzip block's inflate state lives, so a caller
 * that hands one in allocates nothing whatever the codec. */
static int zip_block_decode(int32_t alg, const uint8_t* src, size_t clen, uint8_t* dst, size_t plain, void* work) {
    if (alg == ZIP_NONE) {
        if (clen != plain) return -1;
        memcpy(dst, src, plain);
        return 0;
    }
    if (alg == ZIP_IPC) return zip_ipc_decode(src, clen, dst, plain);
    if (alg == ZIP_LZ4)
        return LZ4_decompress_safe((const char*)src, (char*)dst, (int)clen, (int)plain) == (int)plain ? 0 : -1;
    size_t got = 0, used = 0;
    const char* err = NULL;
    return q_gz_inflate_zlib_in(src, clen, dst, plain, &got, &used, &err, work, work ? Q_GZ_INFLATE_WORK : 0) == 0 &&
           used == clen && got == plain ? 0 : -1;
}

/* The container trailer of an object whose stored length is `file_len`.  NULL
 * with *found=0 when it carries no magic, NULL with *found=1 and `z` filled
 * when it holds a well-formed container; else an owned error — the transport's
 * from the read, 'corrupt from a layout the magic promised and the trailer denies. */
static ray_t* io_zip_hdr(const io_src_t* s, ray_t* pathstr, int64_t file_len, zip_hdr_t* z, int* found,
                         ray_t** tail_out) {
    *found = 0;
    if (tail_out) *tail_out = NULL;
    ray_t* err = NULL;
    int wrapped = io_magic(s, pathstr, &err);
    if (wrapped <= 0) return err;
    int64_t span = s->batch > 8 ? s->batch : 8;
    if (span > file_len - ZIP_MAGIC_LEN) span = file_len - ZIP_MAGIC_LEN;
    if (span < 8) return q_err(QE_CORRUPT);
    ray_t* tail = s->read(pathstr, file_len - span, span);
    if (RAY_IS_ERR(tail)) return tail;
    int64_t nb = q_count(tail) == span ? zip_nblocks((const uint8_t*)ray_data(tail) + span - 8, (size_t)file_len) : 0;
    if (!nb) { ray_release(tail); return q_err(QE_CORRUPT); }
    tail = ZIP_TAIL_LEN(nb) <= span ? io_slice(tail, span - ZIP_TAIL_LEN(nb), ZIP_TAIL_LEN(nb))
         : (ray_release(tail), s->read(pathstr, file_len - ZIP_TAIL_LEN(nb), ZIP_TAIL_LEN(nb)));
    if (RAY_IS_ERR(tail)) return tail;
    ray_t* bad = q_count(tail) != ZIP_TAIL_LEN(nb) ? q_err(QE_CORRUPT)
               : zip_trailer((const uint8_t*)ray_data(tail), nb, (size_t)file_len, z);
    *found = bad == NULL;
    if (!bad && tail_out) *tail_out = tail;
    else ray_release(tail);
    return bad;
}

/* the stored length, as a long, from the transport's size operation */
static ray_t* io_stored_len(const io_src_t* s, ray_t* pathstr, int64_t* n) {
    ray_t* sz = s->size(pathstr);
    if (RAY_IS_ERR(sz)) return sz;
    *n = sz->i64;
    ray_release(sz);
    return NULL;
}

/* ref/hcount.md: a compressed file's size is its ORIGINAL file's, so size is
 * answered past the container exactly as q_io_read_slice answers "what bytes".
 * Too small to hold one, or holding one that will not decode, and the stored
 * length is all there is — the magic is a claim, and `-21!` is what signals. */
ray_t* q_io_file_size(ray_t* pathstr) {
    const io_src_t* s = io_src(pathstr);
    int64_t n;
    ray_t* bad = io_stored_len(s, pathstr, &n);
    if (bad) return bad;
    if (n < ZIP_MAGIC_LEN + ZIP_TAIL_LEN(1)) return ray_i64(n);
    if (s == &IO_LOCAL && io_file_len(pathstr) < 0) return ray_i64(n);   /* a directory has no tail to probe */
    zip_hdr_t z;
    int found = 0;
    bad = io_zip_hdr(s, pathstr, n, &z, &found, NULL);
    if (bad && !q_err_is(bad, QE_CORRUPT)) return bad;
    if (bad) ray_release(bad);
    return ray_i64(found ? z.uncompressed : n);
}

ray_t* q_io_unzip(const uint8_t* buf, size_t len) {
    if (!buf || len < ZIP_MAGIC_LEN || memcmp(buf, ZIP_MAGIC, ZIP_MAGIC_LEN) != 0) return NULL;
    int64_t nb = len >= 8 ? zip_nblocks(buf + len - 8, len) : 0;
    if (!nb) return q_err(QE_CORRUPT);
    const uint8_t* t = buf + len - ZIP_TAIL_LEN(nb);
    zip_hdr_t z;
    ray_t* bad = zip_trailer(t, nb, len, &z);
    if (!bad) bad = zip_codecs(t, nb);
    if (bad) return bad;
    int32_t log2bs = zip_log2(z.block_size);
    if (z.block_size != (int64_t)1 << log2bs ||
        log2bs < ZIP_LOG2_MIN || log2bs > ZIP_LOG2_MAX)
        return q_err(QE_CORRUPT);
    /* Each block inflates to at most logicalBlockSize, so the trailer bounds its
     * own uncompressedLength: a lying header cannot drive the allocation.  The
     * ceiling is spelled out rather than (a+b-1)/b, which would overflow. */
    int64_t need = z.uncompressed / z.block_size + (z.uncompressed % z.block_size ? 1 : 0);
    if (need > z.num_blocks) return q_err(QE_CORRUPT);

    uint8_t* dst = (uint8_t*)malloc(z.uncompressed > 0 ? (size_t)z.uncompressed : 1);
    if (!dst) return q_err(QE_OOM);
    size_t produced = 0, pos = 0;
    int ok = 1;
    for (int64_t k = 0; ok && k < z.num_blocks; k++) {
        size_t start = pos, clen = (size_t)zip_blen(t, k);
        int64_t plain = z.uncompressed - (int64_t)produced;
        if (plain > z.block_size) plain = z.block_size;
        if (plain <= 0 || zip_block_decode(zip_balg(t, k), buf + ZIP_MAGIC_LEN + start, clen, dst + produced,
                                           (size_t)plain, NULL) != 0)
            ok = 0;
        else
            produced += (size_t)plain;
        pos = start + clen;
    }
    ray_t* out = (ok && produced == (size_t)z.uncompressed &&
                  pos == (size_t)z.compressed)
                     ? ray_vec_from_raw(RAY_BYTE_ONLY, dst, z.uncompressed)
                     : q_err(QE_CORRUPT);
    free(dst);
    return out;
}

static ray_t* zip_stats_dict(const zip_hdr_t* z, int64_t file_len) {
    static const char* const names[] = { "compressedLength", "uncompressedLength",
                                         "algorithm", "logicalBlockSize", "zipLevel" };
    ray_t* vals[] = { ray_i64(file_len), ray_i64(z->uncompressed),
                      ray_i32(z->algorithm), ray_i32(zip_log2(z->block_size)),
                      ray_i32(z->level) };
    int64_t n = (int64_t)(sizeof names / sizeof names[0]);
    ray_t* k = ray_sym_vec_new(RAY_SYM_W64, n);
    ray_t* v = ray_list_new(n);
    for (int64_t i = 0; i < n; i++) {
        int64_t id = ray_sym_intern(names[i], strlen(names[i]));
        if (k && !RAY_IS_ERR(k)) k = ray_vec_append(k, &id);
        if (v && !RAY_IS_ERR(v) && vals[i]) v = ray_list_append(v, vals[i]);
        if (vals[i]) ray_release(vals[i]);
    }
    if (!k || RAY_IS_ERR(k) || !v || RAY_IS_ERR(v)) {
        if (k && !RAY_IS_ERR(k)) ray_release(k);
        if (v && !RAY_IS_ERR(v)) ray_release(v);
        return q_err(QE_OOM);
    }
    return ray_dict_new(k, v);
}

ray_t* q_io_zip_stats(ray_t* x) {
    if (!x || x->type != -RAY_SYM) return q_err(QE_TYPE);
    ray_t* path = q_io_file_path(x);
    if (!path) return q_err(QE_TYPE);
    const io_src_t* s = io_src(path);
    int64_t file_len = 0;                        /* the CONTAINER's own length */
    zip_hdr_t z;
    int found = 0;
    ray_t* r = io_stored_len(s, path, &file_len);
    if (!r) r = io_zip_hdr(s, path, file_len, &z, &found, NULL);
    if (!r && !found)                   /* not compressed -> the empty dict */
        r = ray_dict_new(ray_sym_vec_new(RAY_SYM_W64, 0), ray_list_new(0));
    else if (!r) {
        r = zip_stats_dict(&z, file_len);
    }
    ray_release(path);
    return r;
}

/* ---- container block access + write (q_io.h; PR #393 dissection note) --- */

void q_io_zipmap_free(q_io_zipmap_t* zm) {
    free(zm->ends);
    zm->ends = NULL;
    zm->algs = NULL;
}

ray_t* q_io_zip_open(ray_t* pathstr, q_io_zipmap_t* zm) {
    memset(zm, 0, sizeof *zm);
    const io_src_t* s = io_src(pathstr);
    int64_t file_len = 0;
    zip_hdr_t z;
    int found = 0;
    ray_t* tail = NULL;
    ray_t* bad = io_stored_len(s, pathstr, &file_len);
    if (!bad) bad = io_zip_hdr(s, pathstr, file_len, &z, &found, &tail);
    if (bad) return bad;
    if (!found) return q_err(QE_TYPE);
    const uint8_t* t = (const uint8_t*)ray_data(tail);
    int32_t log2bs = zip_log2(z.block_size);
    int sized = z.block_size == (int64_t)1 << log2bs && log2bs >= ZIP_LOG2_MIN && log2bs <= ZIP_LOG2_MAX;
    bad = zip_codecs(t, z.num_blocks);
    if (!bad && (!sized || z.uncompressed / z.block_size + (z.uncompressed % z.block_size ? 1 : 0) > z.num_blocks))
        bad = q_err(QE_CORRUPT);
    zm->ends = bad ? NULL : (int64_t*)malloc((size_t)z.num_blocks * (sizeof(int64_t) + sizeof(int32_t)));
    if (!bad && !zm->ends) bad = q_err(QE_OOM);
    if (!bad) zm->algs = (int32_t*)(zm->ends + z.num_blocks);
    for (int64_t k = 0, pos = 0; !bad && k < z.num_blocks; k++) {
        zm->ends[k] = pos += zip_blen(t, k);
        zm->algs[k] = zip_balg(t, k);
    }
    ray_release(tail);
    if (bad) return bad;
    int64_t mx = 0;
    for (int64_t k = 0; k < z.num_blocks; k++) {
        int64_t c = zm->ends[k] - (k ? zm->ends[k - 1] : 0);
        if (c > mx) mx = c;
        if (zm->algs[k] == ZIP_GZIP) zm->gzip = 1;
    }
    zm->maxblock = ((size_t)mx + 15) & ~(size_t)15;
    zm->uncompressed = z.uncompressed;
    zm->block_size = z.block_size;
    zm->num_blocks = z.num_blocks;
    zm->payload = z.compressed;
    zm->algorithm = z.algorithm;
    zm->level = z.level;
    return NULL;
}

static int64_t zip_block_span(const q_io_zipmap_t* zm, int64_t k, int64_t* start) {
    *start = k ? zm->ends[k - 1] : 0;
    return zm->ends[k] - *start;
}

size_t q_io_zipmap_maxblock(const q_io_zipmap_t* zm) {
    return zm->maxblock + (zm->gzip ? Q_GZ_INFLATE_WORK : 0);
}

#ifdef RAY_OS_WINDOWS
ssize_t q_io_pread(int fd, void* buf, size_t n, int64_t off) {   /* ReadFile at an OVERLAPPED offset */
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    if (!ReadFile((HANDLE)_get_osfhandle(fd), buf, (DWORD)(n > 0x7fffffff ? 0x7fffffff : n), &got, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        errno = EIO;                    /* the callers' EINTR retry must see a fresh errno */
        return -1;
    }
    return (ssize_t)got;
}
#else
ssize_t q_io_pread(int fd, void* buf, size_t n, int64_t off) {
    return pread(fd, buf, n, (off_t)off);
}
#endif

int q_io_pread_all(int fd, void* buf, size_t n, int64_t off) {
    for (size_t got = 0; got < n; ) {
        ssize_t r = q_io_pread(fd, (uint8_t*)buf + got, n - got, off + (int64_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

int q_io_zip_block_fd(int fd, const q_io_zipmap_t* zm, int64_t k,
                      uint8_t* scratch, uint8_t* dst, size_t cap) {
    if (k < 0 || k >= zm->num_blocks) return -1;
    int64_t start, clen = zip_block_span(zm, k, &start);
    int64_t plain = zm->uncompressed - k * zm->block_size;
    if (plain > zm->block_size) plain = zm->block_size;
    if (plain < 0 || (size_t)plain > cap || clen < 0) return -1;
    if (q_io_pread_all(fd, scratch, (size_t)clen, ZIP_MAGIC_LEN + start)) return -2;
    uint8_t* work = zm->algs[k] == ZIP_GZIP ? scratch + zm->maxblock : NULL;
    return zip_block_decode(zm->algs[k], scratch, (size_t)clen, dst, (size_t)plain, work);
}

/* blocks k0..k1 inflated into dst, their compressed bytes fetched in ONE read */
static ray_t* io_zip_blocks(const io_src_t* s, ray_t* pathstr, const q_io_zipmap_t* zm, int64_t k0, int64_t k1,
                            uint8_t* dst) {
    int64_t c0, c1;
    zip_block_span(zm, k0, &c0);
    c1 = zm->ends[k1];
    ray_t* raw = s->read(pathstr, ZIP_MAGIC_LEN + c0, c1 - c0);
    if (RAY_IS_ERR(raw)) return raw;
    ray_t* bad = q_count(raw) == c1 - c0 ? NULL : q_err(QE_CORRUPT);
    for (int64_t k = k0; !bad && k <= k1; k++) {
        int64_t start, clen = zip_block_span(zm, k, &start);
        int64_t plain = zm->uncompressed - k * zm->block_size;
        if (plain > zm->block_size) plain = zm->block_size;
        if (plain <= 0 || zip_block_decode(zm->algs[k], (const uint8_t*)ray_data(raw) + (start - c0), (size_t)clen,
                                           dst + (k - k0) * zm->block_size, (size_t)plain, NULL) != 0)
            bad = q_err(QE_CORRUPT);
    }
    ray_release(raw);
    return bad;
}

ray_t* q_io_zip_block(ray_t* pathstr, const q_io_zipmap_t* zm, int64_t k,
                      uint8_t* dst, size_t cap) {
    int64_t plain = zm->uncompressed - k * zm->block_size;
    if (k < 0 || k >= zm->num_blocks || (size_t)(plain > zm->block_size ? zm->block_size : plain) > cap)
        return q_err(QE_CORRUPT);
    return io_zip_blocks(io_src(pathstr), pathstr, zm, k, k, dst);
}

/* A bounded slice of a container's plaintext: the blocks it spans are fetched in one read and inflated, and no
 * others, so a row read of a compressed column costs its own blocks.  The offset addresses plaintext, under
 * q_io_clamp's law. */
static ray_t* io_zip_slice(const io_src_t* s, ray_t* pathstr, int64_t off, int64_t want) {
    q_io_zipmap_t zm;
    ray_t* bad = q_io_zip_open(pathstr, &zm);
    if (bad) return bad;
    int64_t take = q_io_clamp(zm.uncompressed, &off, want), bs = zm.block_size;
    int64_t k0 = take ? off / bs : 0, k1 = take ? (off + take - 1) / bs : -1;
    uint8_t* blk = (uint8_t*)malloc(take ? (size_t)((k1 - k0 + 1) * bs) : 1);
    if (!blk) bad = q_err(QE_OOM);
    else if (take) bad = io_zip_blocks(s, pathstr, &zm, k0, k1, blk);
    ray_t* out = bad ? bad : ray_vec_from_raw(RAY_BYTE_ONLY, blk + (take ? off - k0 * bs : 0), take);
    free(blk);
    q_io_zipmap_free(&zm);
    return out;
}

int q_io_zip_param(int64_t v) { return v < INT_MIN ? INT_MIN : v > INT_MAX ? INT_MAX : (int)v; }

/* One algorithm-1 block: `src` framed as a little-endian IPC message and run through the IPC compressor, whose
 * answer is the whole block.  *len is 0 where that compressor gives up (2000 bytes or fewer, or not under half):
 * kdb stores exactly those blocks (4,028/4,028).  NULL on OOM. */
static uint8_t* zip_ipc_encode(const uint8_t* src, size_t plain, size_t* len) {
    uint32_t total = (uint32_t)plain + 8;
    uint8_t* out = (uint8_t*)calloc(1, total);
    if (!out) return NULL;
    out[0] = 1;
    memcpy(out + 4, &total, 4);
    memcpy(out + 8, src, plain);
    ray_t* frame = ray_vec_from_raw(RAY_BYTE_ONLY, out, (int64_t)total);
    ray_t* z = frame && !RAY_IS_ERR(frame) ? q_wire_compress(frame) : NULL;
    int ok = z && !RAY_IS_ERR(z);
    *len = ok && z != frame ? (size_t)q_count(z) : 0;
    if (*len) memcpy(out, ray_data(z), *len);
    if (ok) ray_release(z);
    else if (z) ray_error_free(z);
    if (frame && !RAY_IS_ERR(frame)) ray_release(frame);
    else if (frame) ray_error_free(frame);
    if (!ok) { free(out); out = NULL; }
    return out;
}

ray_t* q_io_zip_write(ray_t* pathstr, const uint8_t* img, size_t n,
                      int lbs, int alg, int lvl) {
    if (alg != ZIP_IPC && alg != ZIP_GZIP && alg != ZIP_LZ4) return q_err(QE_NYI);       /* 3/5: the request site */
    if (lbs < ZIP_LOG2_MIN || lbs > ZIP_LOG2_MAX || lvl < 0 || (alg == ZIP_GZIP && lvl > 9) || n == 0)
        return q_err(QE_DOMAIN);
    if (lvl > 16 && alg == ZIP_LZ4) lvl = 16;        /* kb/file-compression.md: lz4hc level>16 is 16 */
    int64_t bs = (int64_t)1 << lbs;
    int64_t nb = ((int64_t)n + bs - 1) / bs;
    uint8_t** blk = (uint8_t**)calloc((size_t)nb, sizeof(uint8_t*));
    size_t* bl = (size_t*)calloc((size_t)nb, sizeof(size_t));
    if (!blk || !bl) { free(blk); free(bl); return q_err(QE_OOM); }
    size_t C = 0;
    int oom = 0;
    for (int64_t k = 0; k < nb && !oom; k++) {
        size_t plain = (size_t)((k + 1) * bs <= (int64_t)n ? bs : (int64_t)n - k * bs);
        const uint8_t* src = img + k * bs;
        if (alg == ZIP_IPC) {
            blk[k] = zip_ipc_encode(src, plain, &bl[k]);
            if (blk[k] && !bl[k]) bl[k] = plain;     /* gave up: stored below */
        } else if (alg == ZIP_GZIP) {
            const char* gerr = NULL;
            blk[k] = q_gz_deflate_zlib(src, plain, lvl, &bl[k], &gerr);
        } else {
            int cap = LZ4_compressBound((int)plain);
            blk[k] = (uint8_t*)malloc((size_t)cap);
            int got = blk[k] ? LZ4_compress_HC((const char*)src, (char*)blk[k], (int)plain, cap, lvl) : 0;
            bl[k] = got > 0 ? (size_t)got : 0;
        }
        oom = !blk[k] || !bl[k];
        /* kdb stores a block that did not pay; for gzip only a one-block file is witnessed (Q4) */
        if (!oom && bl[k] >= plain && (alg != ZIP_GZIP || nb == 1)) {
            free(blk[k]);
            blk[k] = NULL;
            bl[k] = plain;
        }
        C += bl[k];
    }
    size_t flen = ZIP_MAGIC_LEN + C + ZIP_TAIL_LEN(nb);
    uint8_t* out = oom ? NULL : (uint8_t*)malloc(flen);
    ray_t* bad = NULL;
    if (!out) {
        bad = q_err(QE_OOM);
    } else {
        uint8_t* w = out;
        memcpy(w, ZIP_MAGIC, ZIP_MAGIC_LEN); w += ZIP_MAGIC_LEN;
        for (int64_t k = 0; k < nb; k++) { memcpy(w, blk[k] ? blk[k] : img + k * bs, bl[k]); w += bl[k]; }
        int32_t ver = alg == ZIP_IPC ? 2 : 3;        /* as kdb stamps each codec: 156/156 alg-1 files say 2 */
        int64_t unc = (int64_t)n, bsz = bs, nbl = nb;
        int64_t cim = (int64_t)(ZIP_MAGIC_LEN + C);
        memcpy(w, &ver, 4); w += 4;
        *w++ = (uint8_t)alg;                         /* codec: the REQUESTED pair */
        *w++ = (uint8_t)lvl;
        *w++ = 0; *w++ = 0;
        memcpy(w, &unc, 8);  w += 8;
        memcpy(w, &cim, 8);  w += 8;
        memcpy(w, &bsz, 8);  w += 8;
        for (int64_t k = 0; k < nb; k++) {
            int32_t csz = (int32_t)bl[k], walg = blk[k] ? alg : ZIP_NONE;
            memcpy(w, &csz, 4);  w += 4;
            memcpy(w, &walg, 4); w += 4;
        }
        memcpy(w, &nbl, 8);  w += 8;
        bad = q_io_write_all(pathstr, out, flen);
    }
    free(out);
    for (int64_t k = 0; k < nb; k++) free(blk[k]);
    free(blk);
    free(bl);
    return bad;
}

/* ---- the verbs ---------------------------------------------------------- */

/* q `hsym x` — sym atom/vector -> file symbol: prefix ':' unless already
 * present (ref/hsym.md). */
static int64_t hsym_id(const char* p, size_t n) {
    if (n > 0 && p[0] == ':') return ray_sym_intern_runtime(p, n);
    char* buf = (char*)malloc(n + 1);
    if (!buf) return ray_sym_intern_runtime(":", 1);
    buf[0] = ':';
    memcpy(buf + 1, p, n);
    int64_t id = ray_sym_intern_runtime(buf, n + 1);
    free(buf);
    return id;
}
/* Exported (q_registry.h) so the `-1!` internal-fn alias single-homes here. */
static ray_t* hsym_wrap_impl(ray_t* x);
ray_t* q_hsym_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = hsym_wrap_impl(xs);
    ray_release(xs);
    return r;
}
static ray_t* hsym_wrap_impl(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);                   /* borrowed */
        if (!s) return q_err(QE_TYPE);
        const char* p = ray_str_ptr(s);
        size_t n = ray_str_len(s);
        if (n > 0 && p[0] == ':') { ray_retain(x); return x; }
        return ray_sym(hsym_id(p, n));
    }
    if (x && x->type == RAY_SYM) {
        int64_t n = q_count(x);
        ray_t* out = ray_sym_vec_new(RAY_SYM_W64, n > 0 ? n : 1);
        if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        for (int64_t i = 0; i < n; i++) {
            ray_t* c = ray_sym_vec_cell(x, i);            /* borrowed domain atom */
            if (!c) { ray_release(out); return q_err(QE_TYPE); }
            int64_t id = hsym_id(ray_str_ptr(c), ray_str_len(c));
            out = ray_vec_append(out, &id);
            if (!out || RAY_IS_ERR(out)) return out ? out : q_err(QE_OOM);
        }
        return out;
    }
    return q_err(QE_TYPE);
}

/* q `read1 x` — ref/read1.md.  File sym -> whole content as bytes; (f;o) ->
 * bytes from offset o to EOF; (f;o;n) -> up to n bytes from o (short read at
 * EOF, offsets clamped).  A kxzip container is resolved away by the byte core,
 * so what comes back is the uncompressed data the doc's Compression section
 * promises.  `read1(h;n)` and `read1 h` on a fifo handle are q_handles_read1's;
 * any other handle is 'nyi. */
static ray_t* read1_wrap_impl(ray_t* x);
ray_t* q_read1_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = read1_wrap_impl(xs);
    ray_release(xs);
    return r;
}
static ray_t* read1_wrap_impl(ray_t* x) {
    if (x && x->type == -RAY_SYM) {
        ray_t* path = q_io_path_operand(x);
        if (!path) return q_err(QE_TYPE);
        ray_t* r = q_io_resource_read(path, 0, -1);
        ray_release(path);
        return r;
    }
    /* read1(handle;count): an all-int (h;count) pair collapses to a homogeneous
     * int VECTOR (not a RAY_LIST) — the fifo-handle streaming form `.Q.fpn` uses.
     * q_handles_read1 answers NULL unless fd is a registered fifo (a Phase-1
     * file handle is a write/append fd — read a file via `read1 `:path`). */
    if (x && q_type_is_int_vec(x) && q_count(x) == 2) {
        ray_t* c = ray_i64(q_type_ivec_get(x, 1));
        ray_t* r = q_handles_read1(q_type_ivec_get(x, 0), c);
        ray_release(c);
        if (r) return r;
    }
    if (x && x->type == RAY_LIST && (q_count(x) == 2 || q_count(x) == 3)) {
        ray_t** e = (ray_t**)ray_data(x);
        int three = q_count(x) == 3;
        if (e[0] && e[0]->type == -RAY_SYM) {
            ray_t* path;
            int64_t off, want;
            ray_t* bad = q_io_file_triple(e[0], e[1], three ? e[2] : NULL, 0, &path, &off, &want);
            if (bad) return bad;
            ray_t* r = q_io_resource_read(path, off, want);
            ray_release(path);
            return r;
        }
        if (!three && e[0] && q_type_is_int_atom(e[0])) {   /* read1(handle;count), fifo only */
            int64_t fd;
            if (!q_type_strict_i64(e[0], &fd)) return q_err(QE_TYPE);
            ray_t* r = q_handles_read1(fd, e[1]);
            if (r) return r;
        }
        return q_err(QE_NYI);
    }
    if (x && q_type_is_int_atom(x)) {
        int64_t fd;
        ray_t*  r = q_type_strict_i64(x, &fd) ? q_handles_read1(fd, NULL) : NULL;
        return r ? r : q_err(QE_NYI);
    }
    return q_err(QE_TYPE);
}

/* A raw read of fd 0 (ref/read0.md § System or process handle is a terminal
 * transcript; peachq-org/peachq#51 is a pipe).  A tty gives ONE line, delimiter
 * removed — read a byte at a time on purpose: the REPL reads the SAME fd, so
 * consuming past the newline would eat the statement after this one.  A pipe
 * gives the whole stream to EOF as one char vector, bytes untouched (newlines
 * and any CR kept).  EOF before any byte yields the empty vector. */
static ray_t* io_stdin_read(void) {
    char*   buf = NULL;
    size_t  n = 0, cap = 0;
    int     tty = isatty(0);
    char    rbuf[4096];
    ssize_t got;
    ray_term_mode_t prev;
    int     oom = 0;
    if (tty) ray_term_line_begin(&prev);
    while ((got = read(0, rbuf, tty ? 1 : sizeof rbuf)) > 0) {
        if (tty && rbuf[0] == '\n') break;
        if (n + (size_t)got > cap) {
            while (n + (size_t)got > cap) cap = cap ? cap * 2 : 128;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { oom = 1; break; }
            buf = nb;
        }
        memcpy(buf + n, rbuf, (size_t)got);
        n += (size_t)got;
    }
    if (tty) ray_term_line_end(&prev);
    if (oom) { free(buf); return q_err(QE_OOM); }
    if (tty && n && buf[n - 1] == '\r') n--;      /* a CRLF source loses both */
    ray_t* r = ray_charv(buf ? buf : "", (int64_t)n);
    free(buf);
    return r;
}

/* q `read0 x` — ref/read0.md.  read0 IS read1's bytes decoded: a file symbol
 * splits into lines (LF/CRLF delimiters removed), `(f;o)` gives the chars to
 * EOF minus ONE trailing line break (the doc pins `read0(`:foo;6)` -> "world"
 * on a file ending \n), and `(f;o;n)`/`(fifo;n)` give exactly what was read.
 * Handle 0 is stdin: a line on a tty, the whole stream on a pipe.  Offsets accept 0
 * (superset of the doc). */
static ray_t* read0_wrap_impl(ray_t* x);
ray_t* q_read0_wrap(ray_t* x) {
    ray_t* xs = q_str_in(x);            /* charv args -> legacy STR forms */
    ray_t* r = read0_wrap_impl(xs);
    ray_release(xs);
    return q_str_charv_out(r);              /* lines cross as char vectors */
}
static ray_t* read0_wrap_impl(ray_t* x) {
    if (x && q_type_is_int_atom(x) && q_type_iatom_val(x) == 0) return io_stdin_read();
    ray_t* b = read1_wrap_impl(x);
    if (!b || RAY_IS_ERR(b)) return b;
    int64_t n = q_count(b);
    const char* p = n ? (const char*)ray_data(b) : "";
    ray_t* r;
    if (x->type == -RAY_SYM) {
        r = q_str_split_lines(p, (size_t)n);
    } else {
        ray_t** e = x->type == RAY_LIST ? (ray_t**)ray_data(x) : NULL;
        if (e && q_count(x) == 2 && e[0] && e[0]->type == -RAY_SYM) {   /* (f;o): to EOF */
            if (n && p[n - 1] == '\n') n--;
            if (n && p[n - 1] == '\r') n--;
        }
        r = ray_charv(p, n);
    }
    ray_release(b);
    return r;
}

/* ---- `set`'s FILE half (q_env.c's q_setg_wrap keeps name-vs-path only) ----
 * One front door owning the on-disk-format classification: `:f flat (a
 * trailing slash routes the writer to the splay dir; a `.h.tx` suffix routes
 * it to that format), the 4-item (file;lbs;alg;lvl) compression form
 * (ref/file-compression.md), and the (dir;sympath) domain overload — the
 * peachq API extension the splay writer records. */

/* the final suffix of the last path segment as a sym id; -1 when there is none or the segment is a dotfile */
static int64_t io_ext_sym(const char* p, size_t n) {
    size_t i = n;
    while (i > 0 && p[i - 1] != '/' && p[i - 1] != '.') i--;
    if (i == n || i < 2 || p[i - 1] != '.' || p[i - 2] == '/') return -1;
    return ray_sym_intern_runtime(p + i, n - i);
}

/* `:f.EXT set t — the format `.h.tx` keys EXT as (owner ruling 2026-09-16; the write half of the suffix law
 * q_io_resource_table reads by).  `.h.tx` is read from the env at call time, so a user-added entry lights up; the
 * entry's lines are written by 0:'s law, its bytes by 1:'s.  `.parquet` goes straight to .parquet.write (COPY TO —
 * s3 included), by name like the read door; lines to a remote scheme go through .duckdb.i.write0 the same way, and
 * bytes to one have no transport ('nyi).  NULL = no format claim, so the caller writes the binary form. */
static ray_t* io_set_format(ray_t* x, ray_t* y) {
    ray_t* path = q_io_file_path(x);
    if (!path) return NULL;
    int64_t ext = io_ext_sym(ray_str_ptr(path), io_claim_len(path));
    int remote = io_is_remote(path);
    int parquet = ext == ray_sym_intern_runtime("parquet", 7);
    ray_t* tx = ext < 0 || parquet ? NULL : q_env_get(ray_sym_intern_runtime(".h.tx", 5));   /* borrowed */
    int64_t i = 0, n = tx && tx->type == RAY_DICT && ray_dict_keys(tx)->type == RAY_SYM ? q_count(ray_dict_keys(tx)) : 0;
    while (i < n && ray_vec_get_sym_id(ray_dict_keys(tx), i) != ext) i++;
    ray_t* refused = parquet ? io_guard_duckdb(path, 1) : i < n ? io_guard_resource(path, 1) : NULL;
    ray_release(path);
    if (refused) return refused;
    if (parquet) {
        ray_t* none = ray_list_new(0);
        ray_t* args[3] = { x, y, none };
        ray_t* out = q_eval_apply_call_name(".parquet.write", 14, args, 3);
        ray_release(none);
        return out;
    }
    if (i == n) return NULL;
    if (!y || y->type != RAY_TABLE) return q_err(QE_TYPE);
    ray_t* r = q_eval_apply_value(ray_list_get(ray_dict_vals(tx), i), &y, 1);
    if (!r || RAY_IS_ERR(r)) return r ? r : q_err(QE_OOM);
    ray_t* out;
    if (r->type == RAY_BYTE_ONLY) out = remote ? q_err(QE_NYI) : q_io_filebinary_wrap(x, r);
    else if (remote) { ray_t* args[2] = { x, r }; out = q_eval_apply_call_name(".duckdb.i.write0", 16, args, 2); }
    else out = q_io_filetext_wrap(x, r);
    ray_release(r);
    return out;
}

int q_io_is_fsym(ray_t* v) {
    if (!v || v->type != -RAY_SYM) return 0;
    ray_t* s = ray_sym_str(v->i64);
    int is = s && ray_str_len(s) > 0 && ray_str_ptr(s)[0] == ':';
    if (s) ray_release(s);
    return is;
}

ray_t* q_io_dir_path(ray_t* x, const char* leaf, size_t n) {
    ray_t* path = q_io_file_path(x);
    if (!path) return NULL;
    size_t pn = ray_str_len(path);
    int slash = pn && ray_str_ptr(path)[pn - 1] == '/';
    char* buf = (char*)malloc(pn + 1 + n + 1);
    ray_t* r = NULL;
    if (buf) {
        memcpy(buf, ray_str_ptr(path), pn);
        if (!slash) buf[pn++] = '/';
        if (n) memcpy(buf + pn, leaf, n);
        r = ray_str(buf, pn + n);
        free(buf);
    }
    ray_release(path);
    return r;
}

int q_io_names_dir(ray_t* x) {
    const char* ps; size_t pn;
    ray_t* path = q_io_is_fsym(x) && q_handles_pq_of(x, &ps, &pn) == Q_PQ_NONE ? q_io_file_path(x) : NULL;
    if (!path) return 0;
    size_t n = ray_str_len(path);
    struct stat st;
    int dir = (n && ray_str_ptr(path)[n - 1] == '/') || (stat(ray_str_ptr(path), &st) == 0 && S_ISDIR(st.st_mode));
    ray_release(path);
    return dir;
}

ray_t* q_io_set(ray_t* x, ray_t* y) {
    ray_t* pr = q_provider_write(x, y, 0);   /* a table coordinate -> .X.set */
    if (pr) return pr;
    if (x && x->type == RAY_LIST) {
        int64_t n = q_count(x);
        ray_t** e = (ray_t**)ray_data(x);
        if (n == 4 && q_io_is_fsym(e[0])) {
            int64_t lbs, alg, lvl;
            if (!q_type_strict_i64(e[1], &lbs) || !q_type_strict_i64(e[2], &alg) ||
                !q_type_strict_i64(e[3], &lvl))
                return q_err(QE_TYPE);
            /* an explicit alg (0 included) OVERRIDES `.z.zd` — 0 writes plain */
            return q_wirefile_write_zip(e[0], y, q_io_zip_param(lbs), q_io_zip_param(alg), q_io_zip_param(lvl));
        }
        if (n == 2 && q_io_is_fsym(e[0]) && q_io_is_fsym(e[1])) {
            return q_wirefile_write_splay(e[0], e[1], y, -1, -1, -1);
        }
        return q_err(QE_NYI);
    }
    if (x && x->type == RAY_SYM && q_count(x) == 2) {   /* (dir;sympath) collapses */
        ray_t* d = ray_sym(ray_vec_get_sym_id(x, 0));
        ray_t* s = ray_sym(ray_vec_get_sym_id(x, 1));
        ray_t* r = (q_io_is_fsym(d) && q_io_is_fsym(s))
                       ? q_wirefile_write_splay(d, s, y, -1, -1, -1)
                       : q_err(QE_NYI);
        ray_release(s);
        ray_release(d);              /* write_splay retained d when handing it back */
        return r;
    }
    if (!x || x->type != -RAY_SYM || !q_io_is_fsym(x)) return q_err(QE_NYI);
    ray_t* path = q_io_file_path(x);
    int member = path && q_io_is_member(path);
    int s3 = path && q_s3_is(ray_str_ptr(path), ray_str_len(path));
    if (path) ray_release(path);
    if (member) return q_err(QE_NYI);
    ray_t* r = io_set_format(x, y);
    if (!r) r = s3 ? q_err(QE_NYI) : q_wirefile_write(x, y);
    return r ? r : q_err(QE_TYPE);
}

#ifdef RAY_OS_WINDOWS
/* msvcrt remove() only unlinks FILES; the rmdir half POSIX remove(3) dispatches
 * for free is RemoveDirectoryA, which likewise refuses a non-empty folder. */
static int io_remove_any(const char* p) {
    DWORD a = GetFileAttributesA(p);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
        return RemoveDirectoryA(p) ? 0 : -1;
    return remove(p);
}
#else
#define io_remove_any remove
#endif

/* q `hdel x` — ref/hdel.md.  Delete the file or (empty) folder named by the
 * file symbol `:path and return x.  POSIX remove() dispatches unlink/rmdir, so
 * a folder is removed only when empty (the doc's "folders only if empty").  A
 * missing path or non-empty folder surfaces 'io (the read0 ENOENT precedent).
 * WRITES the filesystem, so q_io_locked refuses it.
 * A table coordinate (`:pq:duckdb:db:t/) drops that OBJECT through its provider. */
ray_t* q_hdel_wrap(ray_t* x) {
    if (q_io_locked()) return q_err(QE_ACCESS);
    if (!x || x->type != -RAY_SYM) return q_err(QE_TYPE);  /* a DELETE takes only the
                                                            * file symbol it documents */
    ray_t* pr = q_provider_hdel(x);
    if (pr) return pr;
    ray_t* path = q_io_file_path(x);
    if (!path) return q_err(QE_TYPE);
    int rc = io_remove_any(ray_str_ptr(path));  /* ray_str path is NUL-terminated */
    ray_release(path);
    if (rc != 0) return q_err(QE_IO);
    ray_retain(x);
    return x;
}

/* ---- the tree delete behind .fs.rmtree ---------------------------------- */

#ifdef RAY_OS_WINDOWS
#define RMTREE_SEP(c) ((c) == '/' || (c) == '\\')
#else
#define RMTREE_SEP(c) ((c) == '/')
#endif

/* 'domain before anything is touched: an empty path, a root, a drive root on any platform, a UNC share root on
 * Windows, or a last component of . or .. (rm's own refusal).  n excludes any trailing separator. */
static int rmtree_refused(const char* p, size_t n) {
    if (n == 0 || (n == 1 && RMTREE_SEP(*p))) return 1;
    if (n >= 2 && p[1] == ':' && (p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') {
        size_t i = 2;
        while (i < n && (p[i] == '/' || p[i] == '\\')) i++;
        if (i == n) return 1;
    }
#ifdef RAY_OS_WINDOWS
    if (RMTREE_SEP(p[0]) && RMTREE_SEP(p[1])) {
        int parts = 0;
        for (size_t i = 2; i < n; i++) parts += !RMTREE_SEP(p[i]) && RMTREE_SEP(p[i - 1]);
        if (parts <= 2) return 1;                   /* \\server\share, \\?\c: */
    }
#endif
    size_t i = n;
    while (i && !RMTREE_SEP(p[i - 1])) i--;
    return (n - i == 1 && p[i] == '.') || (n - i == 2 && p[i] == '.' && p[i + 1] == '.');
}

#ifdef RAY_OS_WINDOWS
enum { RMTREE_WPATH = 32768 };

/* No FILE_SHARE_DELETE: while the handle is open nobody can rename the directory away and put a junction in its
 * place, so the walk under it stays inside the tree.  FILE_FLAG_OPEN_REPARSE_POINT opens a link as itself. */
static HANDLE rmtree_open(const wchar_t* p, DWORD share) {
    return CreateFileW(p, FILE_READ_ATTRIBUTES, share, NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
}

/* the path of the open handle with every junction and link resolved, trailing separator dropped; 0 on failure */
static size_t rmtree_final(HANDLE h, wchar_t* out) {
    DWORD n = h == INVALID_HANDLE_VALUE ? 0 : GetFinalPathNameByHandleW(h, out, RMTREE_WPATH, FILE_NAME_NORMALIZED);
    if (n == 0 || n >= RMTREE_WPATH) return 0;
    while (n && out[n - 1] == L'\\') out[--n] = 0;
    return n;
}

static int rmtree_holds_cwd(const wchar_t* dir, size_t n, wchar_t* cwd) {
    HANDLE c = rmtree_open(L".", FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    size_t k = rmtree_final(c, cwd);
    if (c != INVALID_HANDLE_VALUE) CloseHandle(c);
    return !k || (k >= n && _wcsnicmp(dir, cwd, n) == 0 && (k == n || cwd[n] == L'\\'));
}

static int rmtree_entry(wchar_t* p, size_t n, DWORD attrs);

static int rmtree_contents(wchar_t* p, size_t n) {
    if (n + 3 > RMTREE_WPATH) return -1;
    wcscpy(p + n, L"\\*");
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(p, &f);
    p[n] = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    int rc = 0;
    do {
        size_t k = wcslen(f.cFileName);
        if (wcscmp(f.cFileName, L".") == 0 || wcscmp(f.cFileName, L"..") == 0) continue;
        if (n + k + 2 > RMTREE_WPATH) { rc = -1; break; }
        p[n] = L'\\';
        memcpy(p + n + 1, f.cFileName, (k + 1) * sizeof *p);
        rc = rmtree_entry(p, n + 1 + k, f.dwFileAttributes);
        p[n] = 0;
    } while (rc == 0 && FindNextFileW(h, &f));
    FindClose(h);
    return rc;
}

/* the attributes of what the open handle names NOW, not what the listing saw */
static DWORD rmtree_attrs(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION fi;
    return h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &fi) ? fi.dwFileAttributes : INVALID_FILE_ATTRIBUTES;
}

static int rmtree_leaf(const wchar_t* p, DWORD attrs) {
    if (attrs & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
    return (attrs & FILE_ATTRIBUTE_DIRECTORY ? RemoveDirectoryW(p) : DeleteFileW(p)) ? 0 : -1;
}

/* the directory open as h is emptied with h held, then removed; a reparse point is a leaf, never entered */
static int rmtree_dir(wchar_t* p, size_t n, HANDLE h, DWORD attrs) {
    int rc = attrs & FILE_ATTRIBUTE_REPARSE_POINT ? 0 : rmtree_contents(p, n);
    CloseHandle(h);
    return rc ? rc : rmtree_leaf(p, attrs);
}

static int rmtree_entry(wchar_t* p, size_t n, DWORD attrs) {
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return rmtree_leaf(p, attrs);
    HANDLE h = rmtree_open(p, FILE_SHARE_READ | FILE_SHARE_WRITE);
    DWORD now = rmtree_attrs(h);
    if (now == INVALID_FILE_ATTRIBUTES) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        return -1;
    }
    return rmtree_dir(p, n, h, now);
}

static ray_t* rmtree_path(const char* path) {
    wchar_t* w = malloc(3 * RMTREE_WPATH * sizeof *w);
    if (!w) return q_err(QE_OOM);
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, w, RMTREE_WPATH);
    HANDLE h = n ? rmtree_open(w, FILE_SHARE_READ | FILE_SHARE_WRITE) : INVALID_HANDLE_VALUE;
    DWORD a = rmtree_attrs(h);
    int link = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
    wchar_t* dir = w + RMTREE_WPATH;   /* the walk runs under the resolved path, so no junction above it can move it */
    size_t k = 0;
    ray_t* err = NULL;
    if (a == INVALID_FILE_ATTRIBUTES) err = q_err(QE_IO);
    else if (!(a & FILE_ATTRIBUTE_DIRECTORY) && !link) err = q_err(QE_DOMAIN);
    else if (!link && (!(k = rmtree_final(h, dir)) || rmtree_holds_cwd(dir, k, w + 2 * RMTREE_WPATH)))
        err = q_err(QE_DOMAIN);
    if (err) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    } else if (link ? rmtree_dir(w, (size_t)n - 1, h, a) : rmtree_dir(dir, k, h, a)) {
        err = q_err(QE_IO);
    }
    free(w);
    return err;
}
#else
#ifdef O_PATH
#define RMTREE_ID_OPEN (O_PATH | O_DIRECTORY | O_CLOEXEC)   /* an ancestor we may search but not read */
#else
#define RMTREE_ID_OPEN (O_RDONLY | O_DIRECTORY | O_CLOEXEC)
#endif
enum { RMTREE_DEPTH = 256 };

/* The working directory and every directory above it, by identity, walked up by descriptor so a rename cannot hide
 * one.  Every directory the walk opens is checked against it, which also stops a bind mount of one inside the tree. */
typedef struct { int n; dev_t dev[RMTREE_DEPTH]; ino_t ino[RMTREE_DEPTH]; } rmtree_guard_t;

static int rmtree_guard_init(rmtree_guard_t* g) {
    g->n = 0;
    int fd = open(".", RMTREE_ID_OPEN);
    struct stat a;
    while (fd >= 0 && g->n < RMTREE_DEPTH && fstat(fd, &a) == 0) {
        if (g->n && a.st_dev == g->dev[g->n - 1] && a.st_ino == g->ino[g->n - 1]) { close(fd); return 1; }
        g->dev[g->n] = a.st_dev;
        g->ino[g->n++] = a.st_ino;
        int up = openat(fd, "..", RMTREE_ID_OPEN);
        close(fd);
        fd = up;
    }
    if (fd >= 0) close(fd);
    return 0;
}

static int rmtree_guarded(const rmtree_guard_t* g, int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return 1;
    for (int i = 0; i < g->n; i++) if (g->dev[i] == st.st_dev && g->ino[i] == st.st_ino) return 1;
    return 0;
}

static int rmtree_at(int dir, const char* name, const rmtree_guard_t* g);

/* empties the open directory fd, then closes it */
static int rmtree_contents(int fd, const rmtree_guard_t* g) {
    DIR* d = rmtree_guarded(g, fd) ? NULL : fdopendir(fd);
    if (!d) { close(fd); return -1; }
    int rc = 0;
    for (struct dirent* e; rc == 0 && (errno = 0, e = readdir(d));)
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) rc = rmtree_at(dirfd(d), e->d_name, g);
    if (rc == 0 && errno) rc = -1;
    closedir(d);
    return rc;
}

/* O_NOFOLLOW makes a link fail the open, so a link — to a directory or not — is unlinked as a leaf, never entered */
static int rmtree_at(int dir, const char* name, const rmtree_guard_t* g) {
    int fd = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOTDIR || errno == ELOOP ? unlinkat(dir, name, 0) : -1;
    return rmtree_contents(fd, g) ? -1 : unlinkat(dir, name, AT_REMOVEDIR);
}

static ray_t* rmtree_path(const char* p) {
    int fd = open(p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0) {
        if (lstat(p, &st) != 0) return q_err(QE_IO);
        if (S_ISLNK(st.st_mode)) return unlink(p) ? q_err(QE_IO) : NULL;
        return q_err(S_ISDIR(st.st_mode) ? QE_IO : QE_DOMAIN);
    }
    rmtree_guard_t g;
    if (!rmtree_guard_init(&g) || rmtree_guarded(&g, fd)) { close(fd); return q_err(QE_DOMAIN); }
    return rmtree_contents(fd, &g) || rmdir(p) ? q_err(QE_IO) : NULL;
}
#endif

/* `.fs.i.rmtree x` — delete the directory tree at the file symbol x and answer x; a link is removed, never followed */
static ray_t* rmtree_fn(ray_t* x) {
    if (q_io_locked()) return q_err(QE_ACCESS);
    ray_t* path = q_io_path_operand(x);
    if (!path) return q_err(QE_TYPE);
    const char* p = ray_str_ptr(path);
    size_t n = ray_str_len(path);
    while (n > 1 && RMTREE_SEP(p[n - 1])) n--;
    ray_t* err;
    if (rmtree_refused(p, n)) {
        err = q_err(QE_DOMAIN);
    } else {
        ray_t* trimmed = ray_str(p, n);
        err = rmtree_path(ray_str_ptr(trimmed));
        ray_release(trimmed);
    }
    ray_release(path);
    if (err) return err;
    ray_retain(x);
    return x;
}

/* `.fs.i.stamp x` — what tells a file from a rewrite of it and from another of its name, as four longs: its size in
 * bytes, its modification time in ns from 2000.01.01 UTC (whole seconds where stat tells no finer), its device and its
 * file number (both null where stat names no file: the Windows CRT).  Four nulls for a path that will not stat.  x is
 * a file symbol or a path string. */
static ray_t* stamp_fn(ray_t* x) {
    ray_t* s = x && x->type == RAY_CHARV ? q_str_of_charv(x) : NULL;
    ray_t* path = s ? q_io_file_path(s) : q_io_path_operand(x);
    if (s) ray_release(s);
    if (!path) return q_err(QE_TYPE);
    ray_t* refused = io_guard_resource(path, 0);
    int64_t v[4] = { NULL_I64, NULL_I64, NULL_I64, NULL_I64 }, nsec = 0;
#ifdef RAY_OS_WINDOWS
    struct _stat64 st;
    int ok = !refused && _stat64(ray_str_ptr(path), &st) == 0;
#else
    struct stat st;
    int ok = !refused && stat(ray_str_ptr(path), &st) == 0;
    if (ok) { v[2] = (int64_t)st.st_dev; v[3] = (int64_t)st.st_ino; }
#if defined(RAY_OS_MACOS)
    if (ok) nsec = (int64_t)st.st_mtimespec.tv_nsec;
#else
    if (ok) nsec = (int64_t)st.st_mtim.tv_nsec;
#endif
#endif
    if (ok) {
        v[0] = (int64_t)st.st_size;
        v[1] = ((int64_t)st.st_mtime - 946684800LL) * 1000000000LL + nsec;   /* 2000.01.01 UTC in unix seconds */
    }
    ray_release(path);
    if (refused) return refused;
    ray_t* out = ray_vec_new(RAY_I64, 4);
    for (int i = 0; i < 4 && !RAY_IS_ERR(out); i++) out = ray_vec_append(out, &v[i]);
    return out;
}

void q_io_fs_register(void) {
    ray_t* obj = ray_fn_unary(".fs.i.rmtree", RAY_FN_NONE, rmtree_fn);
    q_env_bind_native(".fs.i.rmtree", obj, 1);
    ray_release(obj);
    obj = ray_fn_unary(".fs.i.stamp", RAY_FN_NONE, stamp_fn);
    q_env_bind_native(".fs.i.stamp", obj, 1);
    ray_release(obj);
}
