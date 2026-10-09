/* q_worker — see q_worker.h.  What a forked child keeps and drops is the state table in
 * actionable-plans/2026-09-26-forkq-procq-handles.md; each owner forgets its own part without closing or calling a
 * hook (q_sys_forked, q_console_forked, q_handles_forget, q_provider_forget). */
#define _GNU_SOURCE
#include "qlang/q_count.h"
#include "qlang/io/q_worker.h"
#include "qlang/io/q_exedir.h"
#include "qlang/io/q_handles.h"
#include "qlang/io/q_provider.h"
#include "qlang/ops/q_sys.h"
#include "qlang/q_console.h"
#include "qlang/q_dotz.h"
#include "qlang/q_env.h"
#include "qlang/eval/q_dbg.h"
#include "qlang/eval/q_eval.h"
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"
#include "core/ipc.h"
#include "core/poll.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "mem/heap.h"
#include "lang/eval.h"
#include <rayforce.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#if !defined(__EMSCRIPTEN__)
#define Q_WORKER_SPAWN 1
#include "core/sock.h"
#endif
#if !defined(RAY_OS_WINDOWS) && !defined(__EMSCRIPTEN__)
#define Q_WORKER_FORK 1
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#endif
#ifdef RAY_OS_WINDOWS
#include <winsock2.h>
#include <windows.h>
#endif

#ifdef Q_WORKER_SPAWN
static const char* const log_env[2] = { "PEACHQ_WORKER_STDOUT", "PEACHQ_WORKER_STDERR" };

static void env_unset(const char* name) {
#ifdef RAY_OS_WINDOWS
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/* `\1`/`\2` for each path given: 0, or -1 when one will not open */
static int logs_apply(const char* const log[2]) {
    for (int i = 0; i < 2; i++)
        if (log[i] && q_sys_redirect(i + 1, log[i], strlen(log[i])) != 0) return -1;
    return 0;
}

/* a launched worker's log paths, taken out of its environment and applied */
static int logs_from_env(void) {
    char buf[2][PATH_MAX];
    const char* log[2] = { NULL, NULL };
    for (int i = 0; i < 2; i++) {
        const char* v = getenv(log_env[i]);
        if (v && *v && strlen(v) < sizeof buf[i]) log[i] = strcpy(buf[i], v);
        env_unset(log_env[i]);
    }
    return logs_apply(log);
}
#endif

#ifdef Q_WORKER_FORK
_Noreturn void q_worker_serve(int link) {
    ray_poll_t* poll = ray_poll_create();
    if (!poll) _exit(1);
    ray_runtime_set_poll(poll);
    if (ray_ipc_serve_link(link) < 0) _exit(1);
    ray_poll_run(poll);
    q_handles_end_owned();
    q_console_flush();
    _exit(0);
}

static int on_main_thread(void) {
#if defined(__linux__)
    return getpid() == (pid_t)syscall(SYS_gettid);
#elif defined(__APPLE__) || defined(__FreeBSD__)
    return pthread_main_np() == 1;
#else
    return 1;
#endif
}

/* heap.c maps a swap-backed pool MAP_SHARED: a child writing it would write the parent's memory */
static int heap_swap_backed(void) {
    for (int i = 0; i < RAY_HEAP_REGISTRY_SIZE; i++) {
        ray_heap_t* h = ray_heap_registry[i];
        for (uint32_t j = 0; h && j < h->pool_count; j++)
            if (h->pools[j].backed) return 1;
    }
    return 0;
}

static void close_inherited(int keep) {
#if defined(__linux__) && defined(SYS_close_range)
    if ((keep == 3 || syscall(SYS_close_range, 3u, (unsigned)keep - 1, 0u) == 0) &&
        syscall(SYS_close_range, (unsigned)keep + 1, ~0u, 0u) == 0)
        return;
#endif
    long max = sysconf(_SC_OPEN_MAX);
    for (int fd = 3; fd < (max > 0 ? max : 1024); fd++)
        if (fd != keep) close(fd);
}

#if !defined(__linux__)
static void* orphan_watch(void* parent) {
    while (getppid() == (pid_t)(intptr_t)parent) usleep(100000);
    _exit(1);
}
#endif

/* 0 once this process is bound to die with `parent`; -1 when it cannot be, or `parent` is already gone */
static int die_with_parent(pid_t parent) {
#if defined(__linux__)
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) return -1;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, orphan_watch, (void*)(intptr_t)parent) != 0) return -1;
    pthread_detach(t);
#endif
    return getppid() == parent ? 0 : -1;
}

/* a child that cannot be isolated never answers the handshake, so the parent's open fails with 'fork */
static _Noreturn void child(int link, pid_t parent, const char* const log[2]) {
    if (logs_apply(log) != 0 || setpgid(0, 0) != 0 || die_with_parent(parent) != 0) _exit(1);
    signal(SIGTERM, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    if (link < 3) {
        int hi = fcntl(link, F_DUPFD, 3);
        if (hi < 0) _exit(1);
        close(link);
        link = hi;
    }
    int nul = open("/dev/null", O_RDONLY);
    if (nul > 0) { dup2(nul, 0); close(nul); }
    close_inherited(link);
    q_sys_forked();
    q_console_forked();
    q_handles_forget();
    q_provider_forget();
    q_dotz_doors_restore();
    q_dbg_reset();
    (void)q_eval_frames_suspend();
    q_env_scope(Q_ENV_SCOPE_SESSION);
    if (__VM) { __VM->ipc_handle = -1; __VM->ipc_poll = NULL; }
    q_worker_serve(link);
}

#endif

#ifdef Q_WORKER_SPAWN
/* a hook's timeout: an int atom in ms, null, 0 or negative = dflt; -1 = not an int */
static int64_t timeout_ms(ray_t* tmo, int64_t dflt) {
    if (!tmo || RAY_IS_NULL(tmo)) return dflt;
    if (!q_type_is_int_atom(tmo)) return -1;
    int64_t ms = RAY_ATOM_IS_NULL(tmo) ? dflt : q_type_iatom_val(tmo);
    return ms > 0 && ms < INT32_MAX ? ms : dflt;
}

/* the parent's end of a new worker's link: owned by its handle, then the kdb handshake within ms; `fail` names a
 * worker that never answered, and that worker is reaped */
static ray_t* link_adopt(ray_sock_t fd, int64_t pid, int64_t ms, q_err_e fail) {
    if (!q_handles_own(fd, pid)) {
        ray_sock_close(fd);
        q_handles_reap(pid);
        return q_err(fail);
    }
    ray_sock_set_timeout(fd, (int)ms);
    if (ray_ipc_adopt_client(fd, NULL, NULL) < 0) {
        q_handles_owned_closed(fd);
        return q_err(fail);
    }
    return ray_i32((int32_t)fd);
}

static void argv_free(char** a) {
    for (size_t i = 1; a && a[i]; i++) free(a[i]);
    free(a);
}

/* argv is the worker's whole recipe: a list of strings passed to the new process as they are */
static ray_t* argv_make(ray_t* x, const char* exe, char*** out) {
    int64_t n = x->type < 0 ? -1 : q_count(x);
    if (n < 0 || (n && x->type != RAY_LIST)) return q_err(QE_TYPE);
    char** a = (char**)calloc((size_t)n + 2, sizeof *a);
    if (!a) return q_err(QE_OOM);
    a[0] = (char*)exe;
    for (int64_t i = 0; i < n; i++) {
        ray_t* s = ((ray_t**)ray_data(x))[i];
        if (s->type != RAY_CHARV && s->type != -RAY_CHARV) { argv_free(a); return q_err(QE_TYPE); }
        const char* p = s->type == RAY_CHARV ? (const char*)ray_data(s) : (const char*)&s->u8;
        size_t len = s->type == RAY_CHARV ? (size_t)q_count(s) : 1;
        if (memchr(p, 0, len)) { argv_free(a); return q_err(QE_DOMAIN); }
        if (!(a[i + 1] = (char*)malloc(len + 1))) { argv_free(a); return q_err(QE_OOM); }
        memcpy(a[i + 1], p, len);
        a[i + 1][len] = 0;
    }
    *out = a;
    return NULL;
}
#endif

#ifdef Q_WORKER_FORK
ray_t* q_worker_fork(ray_t* tmo, const char* const log[2]) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    int64_t ms = timeout_ms(tmo, 5000);
    if (ms < 0) return q_err(QE_TYPE);
    ray_pool_t* pool = ray_pool_get();
    if (!on_main_thread() || atomic_load(&ray_parallel_flag) || (pool && atomic_load(&pool->n_running)) ||
        heap_swap_backed())
        return q_err(QE_NOFORK);
    ray_sock_t sv[2];
    if (ray_sock_pair(sv) != 0) return q_err(QE_FORK);
    q_console_flush();
    fflush(NULL);
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) { close(sv[0]); close(sv[1]); return q_err(QE_FORK); }
    if (pid == 0) { close(sv[0]); child(sv[1], parent, log); }
    close(sv[1]);
    return link_adopt(sv[0], pid, ms, QE_FORK);
}

int q_worker_link(void) {
    const char* v = getenv(Q_WORKER_FD_ENV);
    if (!v) return -1;
    char* end;
    long fd = strtol(v, &end, 10);
    pid_t parent = (pid_t)strtol(*end == ':' ? end + 1 : "0", NULL, 10);
    env_unset(Q_WORKER_FD_ENV);
    struct stat st;
    if (*end != ':' || fd < 3 || fd > INT32_MAX || fstat((int)fd, &st) != 0 || !S_ISSOCK(st.st_mode)) return -1;
    if (die_with_parent(parent) != 0) _exit(1);
    fcntl((int)fd, F_SETFD, FD_CLOEXEC);
    close_inherited((int)fd);
    signal(SIGPIPE, SIG_IGN);
    if (logs_from_env() != 0) _exit(1);
    return (int)fd;
}

static int env_named(const char* var, const char* name) {
    size_t m = strlen(name);
    return strncmp(var, name, m) == 0 && var[m] == '=';
}

/* this process's environment with the link and log paths named in it (add[], NULL = none): the child reads them and
 * unsets them before any q runs */
static char** env_make(char* const add[3]) {
    extern char** environ;
    size_t n = 0;
    while (environ[n]) n++;
    char** e = (char**)calloc(n + 4, sizeof *e);
    if (!e) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (!env_named(environ[i], Q_WORKER_FD_ENV) && !env_named(environ[i], log_env[0]) &&
            !env_named(environ[i], log_env[1]))
            e[k++] = environ[i];
    for (int j = 0; j < 3; j++)
        if (add[j]) e[k++] = add[j];
    return e;
}

/* stdin /dev/null, the link at Q_WORKER_FD, its own process group, every signal at its default and none blocked */
static int spawn(pid_t* pid, const char* exe, int link, char** argv, const char* const log[2]) {
    char var[64];
    snprintf(var, sizeof var, "%s=%d:%ld", Q_WORKER_FD_ENV, Q_WORKER_FD, (long)getpid());
    char* add[3] = { var, NULL, NULL };
    for (int i = 0; i < 2; i++) {
        size_t n = log[i] ? strlen(log_env[i]) + strlen(log[i]) + 2 : 0;
        if (n && (add[i + 1] = (char*)malloc(n))) snprintf(add[i + 1], n, "%s=%s", log_env[i], log[i]);
        else if (n) { free(add[1]); return ENOMEM; }
    }
    char** envp = env_make(add);
    if (!envp) { free(add[1]); free(add[2]); return ENOMEM; }
    sigset_t none, dfl;
    sigemptyset(&none);
    sigfillset(&dfl);
    sigdelset(&dfl, SIGKILL);
    sigdelset(&dfl, SIGSTOP);
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    int rc = posix_spawn_file_actions_init(&fa);
    if (rc == 0) {
        if ((rc = posix_spawnattr_init(&at)) == 0) {
            if ((rc = posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0)) == 0 &&
                (rc = posix_spawn_file_actions_adddup2(&fa, link, Q_WORKER_FD)) == 0 &&
                (rc = posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                                        POSIX_SPAWN_SETSIGDEF)) == 0 &&
                (rc = posix_spawnattr_setpgroup(&at, 0)) == 0 && (rc = posix_spawnattr_setsigmask(&at, &none)) == 0 &&
                (rc = posix_spawnattr_setsigdefault(&at, &dfl)) == 0)
                rc = posix_spawn(pid, exe, &fa, &at, argv, envp);
            posix_spawnattr_destroy(&at);
        }
        posix_spawn_file_actions_destroy(&fa);
    }
    free(envp);
    free(add[1]);
    free(add[2]);
    return rc;
}

/* 0 once argv runs with `link` as its worker link, else an errno; *proc is what launched_release lets go of */
static int launch(int64_t* pid, void** proc, const char* exe, ray_sock_t link, char** argv,
                  const char* const log[2]) {
    *proc = NULL;
    int hi = link == Q_WORKER_FD ? fcntl(link, F_DUPFD_CLOEXEC, Q_WORKER_FD + 1) : link;
    if (hi < 0) return EBADF;
    pid_t p = -1;
    int rc = spawn(&p, exe, hi, argv, log);
    if (hi != link) close(hi);
    *pid = p;
    return rc;
}

static void launched_release(void* proc) { (void)proc; }
#elif defined(RAY_OS_WINDOWS)
ray_t* q_worker_fork(ray_t* tmo, const char* const log[2]) { (void)tmo; (void)log; return q_err(QE_NOFORK); }

/* the variable names an inherited pipe carrying the WSAPROTOCOL_INFOW the parent duplicated the link into */
int q_worker_link(void) {
    const char* v = getenv(Q_WORKER_FD_ENV);
    if (!v) return -1;
    char* end;
    unsigned long long h = strtoull(v, &end, 10);
    int named = *end == 0 && h != 0;
    env_unset(Q_WORKER_FD_ENV);
    if (!named) return -1;
    WSAPROTOCOL_INFOW info;
    char* at = (char*)&info;
    DWORD got = 0, left = sizeof info;
    while (left && ReadFile((HANDLE)(uintptr_t)h, at, left, &got, NULL) && got) { at += got; left -= got; }
    CloseHandle((HANDLE)(uintptr_t)h);
    WSADATA wsa;
    if (left || WSAStartup(MAKEWORD(2, 2), &wsa) != 0) _exit(1);
    SOCKET s = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
                          WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET || logs_from_env() != 0) _exit(1);
    return (int)s;
}

typedef struct { wchar_t* p; size_t n, cap; } wbuf;

static int wput(wbuf* b, const wchar_t* s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->n + n + 1) * 2;
        wchar_t* p = (wchar_t*)realloc(b->p, cap * sizeof *p);
        if (!p) return 0;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n * sizeof *s);
    b->p[b->n += n] = 0;
    return 1;
}

/* one argument as the msvcrt startup and CommandLineToArgvW split it back: backslashes are literal except before a
 * quote, where they double and the quote is escaped */
static int wquote(wbuf* b, const wchar_t* w, size_t m) {
    int plain = m > 0;
    for (size_t i = 0; i < m && plain; i++) plain = !wcschr(L" \t\n\v\"", w[i]);
    if (plain) return wput(b, w, m);
    if (!wput(b, L"\"", 1)) return 0;
    for (size_t i = 0; i <= m; i++) {
        size_t bs = 0;
        while (i < m && w[i] == L'\\') bs++, i++;
        for (size_t k = i == m ? 2 * bs : w[i] == L'"' ? 2 * bs + 1 : bs; k; k--)
            if (!wput(b, L"\\", 1)) return 0;
        if (i < m && !wput(b, w + i, 1)) return 0;
    }
    return wput(b, L"\"", 1);
}

/* the child's argv comes back through its ANSI code page, so the same code page carries each string's bytes there */
static int cmdline(wbuf* b, const wchar_t* exe, char** argv) {
    if (!wquote(b, exe, wcslen(exe))) return 0;
    for (size_t i = 1; argv[i]; i++) {
        int len = (int)strlen(argv[i]);
        int wn = len ? MultiByteToWideChar(CP_ACP, 0, argv[i], len, NULL, 0) : 0;
        wchar_t* w = (wchar_t*)malloc(((size_t)wn + 1) * sizeof *w);
        int ok = w && (!len || MultiByteToWideChar(CP_ACP, 0, argv[i], len, w, wn) == wn) && wput(b, L" ", 1) &&
                 wquote(b, w, (size_t)wn);
        free(w);
        if (!ok) return 0;
    }
    return 1;
}

/* every launched worker is in this job, which closes with this process and takes them with it */
static HANDLE worker_job(void) {
    static HANDLE job;
    if (!job && (job = CreateJobObjectW(NULL, NULL))) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li)) {
            CloseHandle(job);
            job = NULL;
        }
    }
    return job;
}

static HANDLE inheritable(HANDLE h) {
    HANDLE d = NULL;
    if (h && h != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS);
    return d;
}

/* Created suspended, in the job and its own process group, inheriting only NUL as stdin, this process's stdout and
 * stderr, and the read end of a pipe named in Q_WORKER_FD_ENV; the link is duplicated for its pid and the
 * WSAPROTOCOL_INFOW written down that pipe before it runs. */
static int launch(int64_t* pid, void** proc, const char* exe, ray_sock_t link, char** argv,
                  const char* const log[2]) {
    (void)exe;
    *proc = NULL;
    static wchar_t self[32768];
    DWORD sn = GetModuleFileNameW(NULL, self, sizeof self / sizeof *self);
    if (sn == 0 || sn >= sizeof self / sizeof *self) return EIO;
    wbuf cmd = { 0 };
    if (!cmdline(&cmd, self, argv)) { free(cmd.p); return ENOMEM; }
    SECURITY_ATTRIBUTES inherit = { sizeof inherit, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL, job = worker_job();
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
    HANDLE out = inheritable(GetStdHandle(STD_OUTPUT_HANDLE)), err = inheritable(GetStdHandle(STD_ERROR_HANDLE));
    HANDLE list[4];
    DWORD nl = 0;
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(asz);
    int al_live = 0, rc = EIO;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (job && nul != INVALID_HANDLE_VALUE && al && CreatePipe(&rd, &wr, NULL, 0) &&
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) &&
        (al_live = InitializeProcThreadAttributeList(al, 1, 0, &asz))) {
        list[nl++] = nul;
        list[nl++] = rd;
        if (out) list[nl++] = out;
        if (err) list[nl++] = err;
        STARTUPINFOEXW si;
        memset(&si, 0, sizeof si);
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul;
        si.StartupInfo.hStdOutput = out ? out : nul;
        si.StartupInfo.hStdError = err ? err : nul;
        si.lpAttributeList = al;
        char var[32];
        snprintf(var, sizeof var, "%llu", (unsigned long long)(uintptr_t)rd);
        SetEnvironmentVariableA(log_env[0], NULL);
        SetEnvironmentVariableA(log_env[1], NULL);
        if (UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, nl * sizeof *list, NULL, NULL) &&
            SetEnvironmentVariableA(Q_WORKER_FD_ENV, var) && (!log[0] || SetEnvironmentVariableA(log_env[0], log[0])) &&
            (!log[1] || SetEnvironmentVariableA(log_env[1], log[1]))) {
            BOOL made = CreateProcessW(self, cmd.p, NULL, NULL, TRUE,
                                       CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
                                       NULL, NULL, &si.StartupInfo, &pi);
            SetEnvironmentVariableA(Q_WORKER_FD_ENV, NULL);
            SetEnvironmentVariableA(log_env[0], NULL);
            SetEnvironmentVariableA(log_env[1], NULL);
            WSAPROTOCOL_INFOW info;
            DWORD put = 0;
            if (made && AssignProcessToJobObject(job, pi.hProcess) &&
                WSADuplicateSocketW((SOCKET)link, pi.dwProcessId, &info) == 0 &&
                WriteFile(wr, &info, sizeof info, &put, NULL) && put == sizeof info &&
                ResumeThread(pi.hThread) != (DWORD)-1)
                rc = 0;
            else if (made) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, INFINITE);
                CloseHandle(pi.hProcess);
            }
            if (made) CloseHandle(pi.hThread);
        }
    }
    if (al_live) DeleteProcThreadAttributeList(al);
    free(al);
    free(cmd.p);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    if (out) CloseHandle(out);
    if (err) CloseHandle(err);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (rc == 0) {
        *pid = pi.dwProcessId;
        *proc = pi.hProcess;
    }
    return rc;
}

/* held until the handle owns the worker, so its pid cannot be reused before then */
static void launched_release(void* proc) {
    if (proc) CloseHandle((HANDLE)proc);
}
#endif

#ifdef Q_WORKER_SPAWN
ray_t* q_worker_spawn(ray_t* args, ray_t* tmo, const char* const log[2]) {
    if (ray_eval_get_restricted()) return q_err(QE_ACCESS);
    int64_t ms = timeout_ms(tmo, 10000);
    if (ms < 0) return q_err(QE_TYPE);
    char exe[1024];
    if (!q_exedir_path(exe, sizeof exe)) return q_err(QE_PROC);
    char** argv = NULL;
    ray_t* e = argv_make(args, exe, &argv);
    if (e) return e;
    ray_sock_t sv[2];
    if (ray_sock_pair(sv) != 0) { argv_free(argv); return q_err(QE_PROC); }
    int64_t pid = -1;
    void* proc = NULL;
    int rc = launch(&pid, &proc, exe, sv[1], argv, log);
    argv_free(argv);
    ray_sock_close(sv[1]);
    if (rc != 0) { ray_sock_close(sv[0]); return q_err(rc == ENOMEM ? QE_OOM : QE_PROC); }
    ray_t* r = link_adopt(sv[0], pid, ms, QE_PROC);
    launched_release(proc);
    return r;
}
#else
ray_t* q_worker_fork(ray_t* tmo, const char* const log[2]) { (void)tmo; (void)log; return q_err(QE_NOFORK); }
int q_worker_link(void) { return -1; }
ray_t* q_worker_spawn(ray_t* args, ray_t* tmo, const char* const log[2]) {
    (void)args; (void)tmo; (void)log;
    return q_err(QE_NYI);
}
#endif
