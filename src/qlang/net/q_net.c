/* q_net — see q_net.h.  Behaviour pinned from qdocs ref/dotq.md
 * (clean room): .Q.addr returns the IPv4 as a host-order int, dotted-quad
 * input parses without DNS, unresolvable -> -1i; .Q.host maps an int address
 * to a hostname sym via the OS resolver (order/choice is OS-governed). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* getaddrinfo/getnameinfo under -std=c17 */
#endif
#include "qlang/net/q_net.h"
#include "qlang/q_err.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef RAY_OS_WINDOWS
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <netdb.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <sys/socket.h>
#endif

#ifdef RAY_OS_WINDOWS
/* Twin of q_dotz.c's net_wsa_ensure (static there): winsock calls fail with
 * WSANOTINITIALISED until a one-time WSAStartup; never WSACleanup. */
static void wsa_ensure(void) {
    static bool started = false;
    if (started) return;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) started = true;
}
#else
static void wsa_ensure(void) {}
#endif

ray_t* q_net_addr(ray_t* y) {
    if (!y || y->type != -RAY_SYM) return q_err(QE_TYPE);
    ray_t* s = ray_sym_str(y->i64);                    /* borrowed */
    if (!s) return q_err(QE_TYPE);
    char name[256];
    size_t n = ray_str_len(s);
    if (n == 0 || n >= sizeof name) return ray_i32(-1);
    memcpy(name, ray_str_ptr(s), n);
    name[n] = '\0';
    wsa_ensure();
    struct in_addr in;
    if (inet_pton(AF_INET, name, &in) == 1)   /* dotted quad: no DNS lookup */
        return ray_i32((int32_t)ntohl(in.s_addr));
    struct addrinfo hints;
    struct addrinfo* res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &res) != 0 || !res)
        return ray_i32(-1);                    /* unresolvable -> -1i */
    struct sockaddr_in* sin = (struct sockaddr_in*)(void*)res->ai_addr;
    int32_t addr = (int32_t)ntohl(sin->sin_addr.s_addr);
    freeaddrinfo(res);
    return ray_i32(addr);
}

ray_t* q_net_host(ray_t* y) {
    if (!y || y->type != -RAY_I32) return q_err(QE_TYPE);
    wsa_ensure();
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl((uint32_t)y->i32);
    char host[1025];
    if (getnameinfo((struct sockaddr*)&sin, sizeof sin,
                    host, sizeof host, NULL, 0, 0) != 0) {
        /* resolver refusal: fall back to the dotted quad (deterministic) */
        uint32_t a = (uint32_t)y->i32;
        snprintf(host, sizeof host, "%u.%u.%u.%u",
                 (a >> 24) & 255u, (a >> 16) & 255u, (a >> 8) & 255u, a & 255u);
    }
    return ray_sym(ray_sym_intern(host, strlen(host)));
}
