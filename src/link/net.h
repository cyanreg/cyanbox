/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Sockets on one network interface, for the service thread (service.c)
 * and the fake player (client.c): the interface's addresses, UDP and TCP
 * sockets tied to it, and datagrams received with the interface they came
 * in on (docs/LINK.md, "Security": one interface). Addresses are IPv4 in
 * host order throughout. */

#ifndef CYANBOX_LINK_NET_H
#define CYANBOX_LINK_NET_H

#include <net/if.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct CBLinkIf {
    char     name[IFNAMSIZ];
    int      index;
    uint32_t addr, mask, bcast;
    uint8_t  mac[6];            /* zeros when it has none (loopback) */
} CBLinkIf;

/* The interface `name`: its index, first IPv4 address, netmask, broadcast
 * address and MAC. `broadcast`, when given, replaces the broadcast address
 * (the tests' loopback); `address` the address (one of the loopback's
 * 127/8, which are all the loopback's without being listed). -ENODEV for
 * no such interface, -EADDRNOTAVAIL for one without IPv4. */
int  cb_link_if_find(CBLinkIf *out, const char *name, const char *address, const char *broadcast);

/* A UDP socket bound to addr:port (port 0: any), broadcasts allowed, the
 * arrival interface reported (IP_PKTINFO), 64 kB buffers. No
 * SO_REUSEADDR: a port someone holds is -EADDRINUSE, not a share. */
int  cb_link_udp(const CBLinkIf *ifc, uint32_t addr, uint16_t port, uint16_t *bound);
/* A listening TCP socket bound to the interface's address. */
int  cb_link_tcp_listen(const CBLinkIf *ifc, uint16_t port, uint16_t *bound);
/* A TCP connection to addr:port from the interface's address, waiting up
 * to timeout_ms. */
int  cb_link_tcp_connect(const CBLinkIf *ifc, uint32_t addr, uint16_t port, int timeout_ms);

/* One datagram into buf: its length, 0 when there was one to drop (another
 * interface, cut short), -EAGAIN when none is waiting. */
ssize_t cb_link_recv(const CBLinkIf *ifc, int fd, uint8_t *buf, size_t cap, uint32_t *from,
                     uint16_t *from_port);
void cb_link_send(int fd, uint32_t to, uint16_t port, const void *buf, size_t len);

void cb_link_addr_text(uint32_t a, char buf[16]);
int  cb_link_addr_parse(const char *s, uint32_t *out);

#endif /* CYANBOX_LINK_NET_H */
