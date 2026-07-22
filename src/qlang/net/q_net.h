/* q_net — shared network PLUMBING: name<->address resolution + (win) socket init.
 * CAP: plumbing only — init guards and resolution primitives; protocol logic
 * stays in its subsystem file.  Currently the -12!/-13! internal fns (.Q.host / .Q.addr, ref/dotq.md).
 * Network name<->address resolution lives in net/ so q_bang.c stays free of
 * socket headers and the Windows WSA guard. */
#ifndef Q_NET_H
#define Q_NET_H

#include <rayforce.h>

ray_t* q_net_addr(ray_t* y);   /* -13! hostname/IP sym -> int (host order) */
ray_t* q_net_host(ray_t* y);   /* -12! int address -> hostname sym */

#endif
