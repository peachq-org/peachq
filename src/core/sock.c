/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_OS_WINDOWS
  #define _GNU_SOURCE
#endif

#include "core/sock.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #if __has_include(<afunix.h>)
    #include <afunix.h>
  #else
    struct sockaddr_un { ADDRESS_FAMILY sun_family; char sun_path[108]; };
  #endif
#else
  #include <fcntl.h>
  #include <netdb.h>
  #include <strings.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netinet/tcp.h>
  #include <poll.h>
  #include <unistd.h>
#endif

/* ===== Socket Implementation ===== */

/* ---- error channel --------------------------------------------------------
 * Winsock reports failures via WSAGetLastError(), NOT errno (mingw's errno
 * stays stale after a socket call).  sock_errno() returns the last socket
 * error mapped onto the POSIX codes this file tests for, and mirrors it
 * into errno so callers up the stack (ipc.c reads errno for EINTR /
 * EWOULDBLOCK / ETIMEDOUT) see the same value on both platforms.  On POSIX
 * both helpers are the identity / plain errno. */
#ifdef RAY_OS_WINDOWS
static int sock_map_err(int e) {
    switch (e) {
    case WSAEINTR:        return EINTR;
    case WSAEWOULDBLOCK:  return EWOULDBLOCK;
    case WSAEINPROGRESS:  return EINPROGRESS;
    case WSAEALREADY:     return EALREADY;
    case WSAEINVAL:       return EINVAL;
    case WSAETIMEDOUT:    return ETIMEDOUT;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAECONNRESET:   return ECONNRESET;
    case WSAECONNABORTED: return ECONNABORTED;
    case WSAENETUNREACH:  return ENETUNREACH;
    case WSAEHOSTUNREACH: return EHOSTUNREACH;
    case WSAEADDRINUSE:   return EADDRINUSE;
    case WSAENOTCONN:     return ENOTCONN;
    default:              return EIO;
    }
}
static int sock_errno(void) { errno = sock_map_err(WSAGetLastError()); return errno; }
#else
static int sock_map_err(int e) { return e; }
static int sock_errno(void) { return errno; }
#endif

#ifndef RAY_OS_WINDOWS
/* musl matches /etc/hosts case-sensitively, but host names are case-insensitive (RFC 4343):
 * retry under the file's own spelling so the libc still returns every address listed under it. */
static int hosts_lookup(const char* host, const char* port, const struct addrinfo* hints, struct addrinfo** res)
{
    FILE* f = fopen("/etc/hosts", "r");
    if (!f) return -1;
    char line[512];
    int rc = -1;
    while (rc != 0 && fgets(line, sizeof line, f)) {
        line[strcspn(line, "#")] = '\0';
        char* save = NULL;
        char* addr = strtok_r(line, " \t\r\n", &save);
        for (char* name; rc != 0 && addr && (name = strtok_r(NULL, " \t\r\n", &save)); )
            if (strcasecmp(name, host) == 0 && strcmp(name, host) != 0
                && getaddrinfo(name, port, hints, res) == 0 && *res) rc = 0;
    }
    fclose(f);
    return rc;
}
#endif

static int sock_lookup(const char* host, const char* port, int family, struct addrinfo** res)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, res) == 0 && *res) return 0;
#ifdef RAY_OS_WINDOWS
    return -1;
#else
    return hosts_lookup(host, port, &hints, res);
#endif
}

int ray_sock_resolve4(const char* host, uint32_t* ip)
{
    if (!host || !*host) { *ip = htonl(INADDR_ANY); return 0; }
    struct addrinfo* res = NULL;
    if (sock_lookup(host, NULL, AF_INET, &res) != 0) { errno = EADDRNOTAVAIL; return -1; }
    *ip = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return 0;
}

uint16_t ray_sock_service_port(const char* name)
{
    struct servent* se = getservbyname(name, "tcp");
    return se ? ntohs((uint16_t)se->s_port) : 0;
}

ray_sock_t ray_sock_listen(uint16_t port)
{
    return ray_sock_listen_at(htonl(INADDR_ANY), port, false);
}

ray_sock_t ray_sock_listen_at(uint32_t ip, uint16_t port, bool reuseport)
{
    ray_sock_t fd = (ray_sock_t)socket(AF_INET, SOCK_STREAM, 0);
    if (fd == RAY_INVALID_SOCK) { (void)sock_errno(); return RAY_INVALID_SOCK; }
    /* A `system` child that inherits the listener keeps the port after `\p 0` and swallows its connections. */
#ifdef RAY_OS_WINDOWS
    SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0);
#else
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif

    int yes = 1;
#ifdef RAY_OS_WINDOWS
    /* Windows SO_REUSEADDR lets another process bind the same port and take its connections; own the port instead. */
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&yes, sizeof(yes));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
#endif
#ifdef SO_REUSEPORT
    if (reuseport && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&yes, sizeof(yes)) < 0) {
        (void)sock_errno();
        ray_sock_close(fd);
        return RAY_INVALID_SOCK;
    }
#else
    (void)reuseport;
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = ip;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        (void)sock_errno();     /* capture before close overwrites it */
        ray_sock_close(fd);
        return RAY_INVALID_SOCK;
    }
    if (listen(fd, 128) < 0) {
        (void)sock_errno();
        ray_sock_close(fd);
        return RAY_INVALID_SOCK;
    }
    return fd;
}

ray_sock_t ray_sock_accept(ray_sock_t srv)
{
    ray_sock_t fd;
    do {
        fd = (ray_sock_t)accept(srv, NULL, NULL);
    } while (fd == RAY_INVALID_SOCK && sock_errno() == EINTR);

    if (fd == RAY_INVALID_SOCK) return RAY_INVALID_SOCK;

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
    return fd;
}

#ifdef RAY_OS_WINDOWS
static bool sock_same_addr(const struct sockaddr_in* a, const struct sockaddr_in* b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}
#endif

int ray_sock_pair(ray_sock_t sv[2])
{
#ifdef RAY_OS_WINDOWS
    /* Winsock has no socketpair: a loopback listener lives only until it accepts, and a stranger that got in first
     * fails the pair rather than taking its end. */
    ray_sock_t l = ray_sock_listen_at(htonl(INADDR_LOOPBACK), 0, false);
    if (l == RAY_INVALID_SOCK) return -1;
    struct sockaddr_in at, mine, peer;
    int n = sizeof at, nm = sizeof mine, np = sizeof peer;
    ray_sock_t c = RAY_INVALID_SOCK, s = RAY_INVALID_SOCK;
    if (getsockname(l, (struct sockaddr*)&at, &n) == 0 &&
        (c = (ray_sock_t)socket(AF_INET, SOCK_STREAM, 0)) != RAY_INVALID_SOCK &&
        connect(c, (struct sockaddr*)&at, sizeof at) == 0)
        s = ray_sock_accept(l);
    ray_sock_close(l);
    if (s == RAY_INVALID_SOCK || getsockname(c, (struct sockaddr*)&mine, &nm) != 0 ||
        getpeername(s, (struct sockaddr*)&peer, &np) != 0 || !sock_same_addr(&mine, &peer)) {
        (void)sock_errno();
        ray_sock_close(c);
        ray_sock_close(s);
        return -1;
    }
    int yes = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
    SetHandleInformation((HANDLE)c, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    sv[0] = s;
    sv[1] = c;
#else
    int fd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0) return -1;
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    sv[0] = fd[0];
    sv[1] = fd[1];
#endif
    return 0;
}

int ray_sock_pair_unix(ray_sock_t sv[2])
{
#ifdef RAY_OS_WINDOWS
    /* Windows 10 1803+ AF_UNIX: the listener's path is fresh, in this user's temp dir, and deleted at its one
     * accept, so no TCP port and no reachable name outlive the pairing. */
    static LONG seq;
    struct sockaddr_un a = { AF_UNIX, "" };
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    DWORD dn = GetTempPathW(MAX_PATH, dir);
    int pn = dn && dn < MAX_PATH ? swprintf(path, MAX_PATH, L"%lspeachq-%lu-%ld.sock", dir, GetCurrentProcessId(),
                                            InterlockedIncrement(&seq)) : -1;
    /* sun_path is UTF-8 whatever the ANSI code page */
    if (pn < 0 || !WideCharToMultiByte(CP_UTF8, 0, path, -1, a.sun_path, sizeof a.sun_path, NULL, NULL)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    ray_sock_t l = (ray_sock_t)socket(AF_UNIX, SOCK_STREAM, 0), c = RAY_INVALID_SOCK, s = RAY_INVALID_SOCK;
    if (l != RAY_INVALID_SOCK && bind(l, (struct sockaddr*)&a, sizeof a) == 0 && listen(l, 1) == 0 &&
        (c = (ray_sock_t)socket(AF_UNIX, SOCK_STREAM, 0)) != RAY_INVALID_SOCK &&
        connect(c, (struct sockaddr*)&a, sizeof a) == 0)
        s = (ray_sock_t)accept(l, NULL, NULL);
    if (s == RAY_INVALID_SOCK) (void)sock_errno();
    ray_sock_close(l);
    DeleteFileW(path);
    if (s == RAY_INVALID_SOCK) {
        ray_sock_close(c);
        return -1;
    }
    SetHandleInformation((HANDLE)c, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    sv[0] = s;
    sv[1] = c;
    return 0;
#else
    return ray_sock_pair(sv);
#endif
}

ray_sock_t ray_sock_dup(ray_sock_t s)
{
#ifdef RAY_OS_WINDOWS
    WSAPROTOCOL_INFOW info;
    if (WSADuplicateSocketW((SOCKET)s, GetCurrentProcessId(), &info) != 0) { (void)sock_errno(); return RAY_INVALID_SOCK; }
    SOCKET d = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
                          WSA_FLAG_NO_HANDLE_INHERIT);
    if (d == INVALID_SOCKET) { (void)sock_errno(); return RAY_INVALID_SOCK; }
    return (ray_sock_t)d;
#else
    return fcntl(s, F_DUPFD_CLOEXEC, 3);
#endif
}

void ray_sock_shutdown_send(ray_sock_t s)
{
#ifdef RAY_OS_WINDOWS
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
}

/* Connect an already-created socket `fd` to one resolved address.  With
 * timeout_ms > 0 the connect is driven non-blocking + poll (a blocking
 * connect() ignores SO_*TIMEO) and the same budget is then applied as the
 * handshake SO_RCVTIMEO/SO_SNDTIMEO; otherwise it is a plain blocking
 * connect.  Returns 0 on success, -1 on failure with errno set
 * (ETIMEDOUT on a connect timeout). */
static int sock_connect_one(ray_sock_t fd, const struct sockaddr* addr,
                            socklen_t addrlen, int timeout_ms)
{
    if (timeout_ms <= 0) {
        if (connect(fd, addr, addrlen) < 0) { (void)sock_errno(); return -1; }
        return 0;
    }

    ray_sock_set_nonblocking(fd);
    int rc = connect(fd, addr, addrlen);
    if (rc < 0) {
        /* Non-blocking connect in flight: POSIX says EINPROGRESS (AF_UNIX
         * may say EWOULDBLOCK); winsock says WSAEWOULDBLOCK — sock_errno
         * maps it, so one test covers both platforms. */
        int e = sock_errno();
        int in_progress = (e == EINPROGRESS || e == EWOULDBLOCK);
        if (!in_progress) return -1;
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr;
#ifdef RAY_OS_WINDOWS
        pr = WSAPoll(&pfd, 1, timeout_ms);
        if (pr < 0) { (void)sock_errno(); return -1; }
#else
        do { pr = poll(&pfd, 1, timeout_ms); } while (pr < 0 && errno == EINTR);
        if (pr < 0) return -1;
#endif
        if (pr == 0) { errno = ETIMEDOUT; return -1; }
        /* Writable: harvest the pending connect result via SO_ERROR
         * (a winsock SO_ERROR is a WSA code — map it for callers). */
        int soerr = 0;
        socklen_t soerr_len = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&soerr, &soerr_len) < 0
            || soerr != 0) {
            if (soerr != 0) errno = sock_map_err(soerr);
            else (void)sock_errno();
            return -1;
        }
    }
    ray_sock_set_blocking(fd);

    /* Apply the same budget as the handshake send/recv timeout. */
    ray_sock_set_timeout(fd, timeout_ms);
    return 0;
}

ray_sock_t ray_sock_connect(const char* host, uint16_t port, int timeout_ms)
{
    struct addrinfo *res = NULL, *rp;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    if (sock_lookup(host, port_str, AF_UNSPEC, &res) != 0) {
        errno = ENXIO;
        return RAY_INVALID_SOCK;
    }

    /* Try every resolved address in turn, not just the first.  `localhost`
     * commonly resolves to BOTH ::1 (IPv6, often first) and 127.0.0.1; a
     * server bound IPv4-only refuses the ::1 attempt, so we must fall
     * through to the next candidate rather than give up. */
    ray_sock_t fd = RAY_INVALID_SOCK;
    int saved_errno = 0;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = (ray_sock_t)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == RAY_INVALID_SOCK) { saved_errno = sock_errno(); continue; }
        if (sock_connect_one(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen,
                             timeout_ms) == 0)
            break;                          /* connected */
        saved_errno = errno;                /* sock_connect_one mapped it */
        ray_sock_close(fd);
        fd = RAY_INVALID_SOCK;
    }
    freeaddrinfo(res);

    if (fd == RAY_INVALID_SOCK) {
        errno = saved_errno;                /* preserve ETIMEDOUT / refused */
        return RAY_INVALID_SOCK;
    }

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
    return fd;
}

/* ---- overlay transport registry (see sock.h) ------------------------------
 * Fixed table, linear scan, guarded by a count that is zero in every non-TLS
 * process — so the hot IPC path pays one load and one predictable branch. */
#define RAY_SOCK_IO_MAX 64
static struct { ray_sock_t s; ray_sock_io_t io; } g_io[RAY_SOCK_IO_MAX];
static int g_io_n = 0;

static ray_sock_io_t* io_find(ray_sock_t s)
{
    if (g_io_n == 0 || s == RAY_INVALID_SOCK) return NULL;
    for (int i = 0; i < RAY_SOCK_IO_MAX; i++)
        if (g_io[i].io.ctx && g_io[i].s == s) return &g_io[i].io;
    return NULL;
}

int ray_sock_io_attach(ray_sock_t s, const ray_sock_io_t* io)
{
    if (s == RAY_INVALID_SOCK || !io || !io->ctx) return -1;
    ray_sock_io_detach(s);
    for (int i = 0; i < RAY_SOCK_IO_MAX; i++) {
        if (g_io[i].io.ctx) continue;
        g_io[i].s = s; g_io[i].io = *io; g_io_n++;
        return 0;
    }
    return -1;
}

bool ray_sock_io_active(ray_sock_t s) { return io_find(s) != NULL; }

void* ray_sock_io_ctx(ray_sock_t s) { ray_sock_io_t* io = io_find(s); return io ? io->ctx : NULL; }

int ray_sock_io_timeout(ray_sock_t s) { ray_sock_io_t* io = io_find(s); return io ? io->timeout_ms : 0; }

size_t ray_sock_io_pending(ray_sock_t s)
{
    ray_sock_io_t* io = io_find(s);
    return (io && io->pending) ? io->pending(io->ctx) : 0;
}

void ray_sock_io_detach(ray_sock_t s)
{
    ray_sock_io_t* io = io_find(s);
    if (!io) return;
    void* ctx = io->ctx;
    void (*fn)(void*) = io->close;
    memset(io, 0, sizeof *io);      /* clear BEFORE the callback: close() must
                                     * not see a half-live entry if it re-enters */
    g_io_n--;
    if (fn) fn(ctx);
}

int64_t ray_sock_send(ray_sock_t s, const void* buf, size_t len)
{
    ray_sock_io_t* ov = io_find(s);
    return ov ? ov->send(ov->ctx, buf, len) : ray_sock_send_plain(s, buf, len);
}

int64_t ray_sock_send_plain(ray_sock_t s, const void* buf, size_t len)
{
    const uint8_t* p   = (const uint8_t*)buf;
    size_t         rem = len;
    while (rem > 0) {
#ifdef RAY_OS_WINDOWS
        int n = send(s, (const char*)p, (int)rem, 0);
#else
        ssize_t n = send(s, p, rem, MSG_NOSIGNAL);
#endif
        if (n < 0) {
            int e = sock_errno();
            if (e == EINTR) continue;
            if (e == EAGAIN || e == EWOULDBLOCK) {
                /* Wait for write-readiness before retry */
                struct pollfd pfd = { .fd = s, .events = POLLOUT };
#ifdef RAY_OS_WINDOWS
                WSAPoll(&pfd, 1, -1);
#else
                poll(&pfd, 1, -1);
#endif
                continue;
            }
            return -1;
        }
        p   += n;
        rem -= (size_t)n;
    }
    return (int64_t)len;
}

int64_t ray_sock_recv(ray_sock_t s, void* buf, size_t len)
{
    ray_sock_io_t* ov = io_find(s);
    return ov ? ov->recv(ov->ctx, buf, len) : ray_sock_recv_plain(s, buf, len);
}

int64_t ray_sock_recv_plain(ray_sock_t s, void* buf, size_t len)
{
    for (;;) {
#ifdef RAY_OS_WINDOWS
        int n = recv(s, (char*)buf, (int)len, 0);
#else
        ssize_t n = recv(s, buf, len, 0);
#endif
        if (n < 0) {
            if (sock_errno() == EINTR) continue;
            return -1;
        }
        return (int64_t)n;   /* 0 = peer closed */
    }
}

bool ray_sock_is_loopback(ray_sock_t s)
{
    /* Peer address decides: kdb compresses only on a non-local link.  An
     * unresolvable peer (getpeername failure) is treated as local so the
     * caller errs toward NOT compressing. */
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    if (getpeername(s, (struct sockaddr*)&ss, &slen) != 0) return true;
    if (ss.ss_family == AF_INET) {
        const struct sockaddr_in* a4 = (const struct sockaddr_in*)&ss;
        return (ntohl(a4->sin_addr.s_addr) >> 24) == 127;   /* 127.0.0.0/8 */
    }
    if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)&ss;
        if (IN6_IS_ADDR_LOOPBACK(&a6->sin6_addr)) return true;
        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr))
            return a6->sin6_addr.s6_addr[12] == 127;        /* ::ffff:127.x */
        return false;
    }
    return true;                                            /* AF_UNIX / local */
}

static int sock_wait(ray_sock_t s, short ev, int timeout_ms)
{
    /* Same wait primitive ray_sock_send uses for write-readiness. */
    struct pollfd pfd = { .fd = s, .events = ev };
    for (;;) {
#ifdef RAY_OS_WINDOWS
        int n = WSAPoll(&pfd, 1, timeout_ms);
#else
        int n = poll(&pfd, 1, timeout_ms);
#endif
        if (n < 0) {
            if (sock_errno() == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0;
        return 1;  /* POLLIN/POLLHUP/POLLERR all mean "go recv" */
    }
}

int ray_sock_wait_readable(ray_sock_t s, int timeout_ms) { return sock_wait(s, POLLIN,  timeout_ms); }
int ray_sock_wait_writable(ray_sock_t s, int timeout_ms) { return sock_wait(s, POLLOUT, timeout_ms); }

int64_t ray_sock_peek(ray_sock_t s, void* buf, size_t len)
{
    for (;;) {
#ifdef RAY_OS_WINDOWS
        int n = recv(s, (char*)buf, (int)len, MSG_PEEK);
#else
        ssize_t n = recv(s, buf, len, MSG_PEEK);
#endif
        if (n < 0 && sock_errno() == EINTR) continue;
        return (int64_t)n;
    }
}

void ray_sock_set_timeout(ray_sock_t s, int ms)
{
    if (ms < 0) ms = 0;
    ray_sock_io_t* io = io_find(s);
    if (io) io->timeout_ms = ms;   /* the overlay obeys the caller's deadline too */
#ifdef RAY_OS_WINDOWS
    DWORD tv = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

void ray_sock_close(ray_sock_t s)
{
    if (s == RAY_INVALID_SOCK) return;
    ray_sock_io_detach(s);
#ifdef RAY_OS_WINDOWS
    closesocket(s);
#else
    close(s);
#endif
}

ray_err_t ray_sock_set_nonblocking(ray_sock_t s)
{
#ifdef RAY_OS_WINDOWS
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode) != 0)
        { (void)sock_errno(); return RAY_ERR_IO; }
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return RAY_ERR_IO;
    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
        return RAY_ERR_IO;
#endif
    return RAY_OK;
}

ray_err_t ray_sock_set_blocking(ray_sock_t s)
{
#ifdef RAY_OS_WINDOWS
    u_long mode = 0;
    if (ioctlsocket(s, FIONBIO, &mode) != 0)
        { (void)sock_errno(); return RAY_ERR_IO; }
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return RAY_ERR_IO;
    if (fcntl(s, F_SETFL, flags & ~O_NONBLOCK) < 0)
        return RAY_ERR_IO;
#endif
    return RAY_OK;
}
