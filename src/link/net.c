/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The interface and its sockets (net.h). Link runs on Ethernet only, the
 * players' own network (docs/LINK.md): an interface that is not Ethernet
 * is refused, the loopback excepted, which reaches no player and carries
 * the tests. Every socket is tied to the
 * interface with SO_BINDTODEVICE, which Linux allows without privileges
 * on an unbound socket since 5.7; where a kernel refuses it, IP_PKTINFO's
 * arrival interface still drops datagrams from any other (rx_link_recv),
 * and the TCP sockets are bound to the interface's address. */

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rxbox/common.h"
#include "rxbox/link.h"

static uint32_t ipv4_of(const struct sockaddr *sa)
{
    return ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr);
}

void rx_link_addr_text(uint32_t a, char buf[16])
{
    snprintf(buf, 16, "%u.%u.%u.%u", a >> 24, a >> 16 & 0xFF, a >> 8 & 0xFF, a & 0xFF);
}

int rx_link_addr_parse(const char *s, uint32_t *out)
{
    struct in_addr a;
    if (!s || inet_pton(AF_INET, s, &a) != 1)
        return RX_ERROR(EINVAL);
    *out = ntohl(a.s_addr);
    return 0;
}

/* Wi-Fi, which Linux gives Ethernet's link type: the wireless request
 * answers for it, and sysfs names it so too (sysfs shows the host's
 * interfaces even in another network namespace; the request asks the
 * namespace the socket is in). */
static bool is_wireless(const char *name)
{
    char path[64];
    struct stat st;
    snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", name);
    if (!stat(path, &st))
        return true;
    snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", name);
    if (!stat(path, &st))
        return true;
    /* SIOCGIWNAME (linux/wireless.h, which cannot be included with
     * net/if.h); its struct iwreq is a name and 16 bytes, which an ifreq
     * holds. */
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    struct ifreq wr = { 0 };
    snprintf(wr.ifr_name, sizeof(wr.ifr_name), "%s", name);
    bool wireless = ioctl(fd, 0x8B01, &wr) == 0;
    close(fd);
    return wireless;
}

/* Ethernet: the link type of the interface's packet entry ARPHRD_ETHER,
 * and not Wi-Fi. A USB Ethernet adapter is Ethernet; a tunnel, WireGuard
 * or the loopback is not. */
static bool is_ethernet(const struct ifaddrs *ifs, const char *name)
{
    for (const struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_PACKET || strcmp(i->ifa_name, name))
            continue;
        const struct sockaddr_ll *ll = (const struct sockaddr_ll *)i->ifa_addr;
        return ll->sll_hatype == ARPHRD_ETHER && !is_wireless(name);
    }
    return false;
}

/* A point-to-point or loopback interface has no broadcast address of its
 * own: the address with the host bits set stands in. */
int rx_link_if_find(RXLinkIf *out, const char *name, const char *address, const char *broadcast)
{
    *out = (RXLinkIf) { 0 };
    if (!name || strlen(name) >= sizeof(out->name))
        return RX_ERROR(ENODEV);
    snprintf(out->name, sizeof(out->name), "%s", name);
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) < 0)
        return RX_ERROR(errno);
    bool found = false, inet = false, has_bcast = false, loopback = false;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || strcmp(i->ifa_name, name))
            continue;
        found = true;
        loopback |= (i->ifa_flags & IFF_LOOPBACK) != 0;
        if (i->ifa_addr->sa_family == AF_INET && !inet) {
            inet = true;
            out->addr = ipv4_of(i->ifa_addr);
            out->mask = i->ifa_netmask ? ipv4_of(i->ifa_netmask) : 0xFFFFFFFF;
            if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr) {
                out->bcast = ipv4_of(i->ifa_broadaddr);
                has_bcast = true;
            }
        } else if (i->ifa_addr->sa_family == AF_PACKET) {
            const struct sockaddr_ll *ll = (const struct sockaddr_ll *)i->ifa_addr;
            if (ll->sll_halen == 6)
                memcpy(out->mac, ll->sll_addr, 6);
        }
    }
    bool ethernet = found && !loopback && is_ethernet(ifs, name);
    freeifaddrs(ifs);
    if (!found) {
        rx_err("link: no network interface \"%s\"", name);
        return RX_ERROR(ENODEV);
    }
    if (!loopback && !ethernet) {
        rx_err("link: \"%s\" is not Ethernet: Link runs on Ethernet only", name);
        return RX_ERROR(EPROTONOSUPPORT);
    }
    if (!inet) {
        rx_err("link: interface \"%s\" has no IPv4 address", name);
        return RX_ERROR(EADDRNOTAVAIL);
    }
    if (address && rx_link_addr_parse(address, &out->addr) < 0) {
        rx_err("link: \"%s\" is not an IPv4 address", address);
        return RX_ERROR(EINVAL);
    }
    if (broadcast) {
        if (rx_link_addr_parse(broadcast, &out->bcast) < 0) {
            rx_err("link: \"%s\" is not an IPv4 address", broadcast);
            return RX_ERROR(EINVAL);
        }
    } else if (!has_bcast) {
        out->bcast = out->addr | ~out->mask;
    }
    out->index = (int)if_nametoindex(name);
    return out->index ? 0 : RX_ERROR(ENODEV);
}

static bool usable(const struct ifaddrs *ifs, const struct ifaddrs *i)
{
    return i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) &&
           !(i->ifa_flags & IFF_LOOPBACK) && strlen(i->ifa_name) < IFNAMSIZ &&
           is_ethernet(ifs, i->ifa_name);
}

static int interface_order(const void *a, const void *b)
{
    const RXLinkInterface *x = a, *y = b;
    if (x->running != y->running)
        return x->running ? -1 : 1;
    return strcmp(x->name, y->name);
}

int rx_link_interfaces(RXLinkInterface *out, int max)
{
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) < 0)
        return RX_ERROR(errno);
    int nb = 0;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!usable(ifs, i))
            continue;
        /* The first IPv4 address of each, as rx_link_if_find takes. */
        bool seen = false;
        for (struct ifaddrs *k = ifs; k != i && !seen; k = k->ifa_next)
            seen = usable(ifs, k) && !strcmp(k->ifa_name, i->ifa_name);
        if (seen)
            continue;
        if (nb < max) {
            RXLinkInterface *o = &out[nb];
            *o = (RXLinkInterface) { 0 };
            snprintf(o->name, sizeof(o->name), "%s", i->ifa_name);
            o->addr = ipv4_of(i->ifa_addr);
            uint32_t mask = i->ifa_netmask ? ipv4_of(i->ifa_netmask) : 0xFFFFFFFF;
            o->prefix = __builtin_popcount(mask);
            o->running = i->ifa_flags & IFF_RUNNING;
        }
        nb++;
    }
    freeifaddrs(ifs);
    if (out && max > 0)
        qsort(out, (size_t)RX_MIN(nb, max), sizeof(*out), interface_order);
    return nb;
}

static void tie(const RXLinkIf *ifc, int fd)
{
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifc->name, (socklen_t)strlen(ifc->name)) < 0)
        rx_debug("link: SO_BINDTODEVICE %s: %s", ifc->name, strerror(errno));
}

static int bound_port(int fd, uint16_t *bound)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &len) < 0)
        return RX_ERROR(errno);
    *bound = ntohs(sa.sin_port);
    return 0;
}

/* 64 kB each way, as rekordbox sets them (rbxport's static analysis of
 * libFilSiNE): an NFS READ reply is up to RX_NFS_MAX_READ, about 23 IP
 * fragments. */
int rx_link_udp(const RXLinkIf *ifc, uint32_t addr, uint16_t port, uint16_t *bound)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return RX_ERROR(errno);
    int one = 1, buf = 65536;
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(addr),
    };
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0 ||
        setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf)) < 0)
        goto fail;
    tie(ifc, fd);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        goto fail;
    int ret = bound_port(fd, bound);
    if (ret < 0) {
        close(fd);
        return ret;
    }
    return fd;
fail:;
    int err = errno;
    close(fd);
    return RX_ERROR(err);
}

int rx_link_tcp_listen(const RXLinkIf *ifc, uint16_t port, uint16_t *bound)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return RX_ERROR(errno);
    int one = 1;
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(ifc->addr),
    };
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
        goto fail;
    tie(ifc, fd);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 16) < 0)
        goto fail;
    int ret = bound_port(fd, bound);
    if (ret < 0) {
        close(fd);
        return ret;
    }
    return fd;
fail:;
    int err = errno;
    close(fd);
    return RX_ERROR(err);
}

int rx_link_tcp_connect(const RXLinkIf *ifc, uint32_t addr, uint16_t port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return RX_ERROR(errno);
    tie(ifc, fd);
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(ifc->addr) };
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(addr),
    };
    int err = 0;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0)
        err = errno;
    else if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS)
        err = errno;
    if (!err) {
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int n = poll(&p, 1, timeout_ms);
        socklen_t len = sizeof(err);
        if (n <= 0)
            err = n ? errno : ETIMEDOUT;
        else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            err = errno;
    }
    if (err) {
        close(fd);
        return RX_ERROR(err);
    }
    return fd;
}

ssize_t rx_link_recv(const RXLinkIf *ifc, int fd, uint8_t *buf, size_t cap, uint32_t *from,
                     uint16_t *from_port)
{
    struct sockaddr_in sa;
    union {
        char buf[CMSG_SPACE(sizeof(struct in_pktinfo))];
        struct cmsghdr align;
    } ctl;
    struct iovec iov = { .iov_base = buf, .iov_len = cap };
    struct msghdr mh = {
        .msg_name = &sa,
        .msg_namelen = sizeof(sa),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctl.buf,
        .msg_controllen = sizeof(ctl.buf),
    };
    ssize_t n = recvmsg(fd, &mh, MSG_DONTWAIT);
    if (n < 0)
        return RX_ERROR(errno);
    if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC) || mh.msg_namelen < sizeof(sa) ||
        sa.sin_family != AF_INET)
        return 0;
    int index = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
            struct in_pktinfo pi;
            memcpy(&pi, CMSG_DATA(c), sizeof(pi));
            index = pi.ipi_ifindex;
        }
    }
    if (index != ifc->index)
        return 0;
    *from = ntohl(sa.sin_addr.s_addr);
    if (from_port)
        *from_port = ntohs(sa.sin_port);
    return n;
}

void rx_link_send(int fd, uint32_t to, uint16_t port, const void *buf, size_t len)
{
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(to),
    };
    if (sendto(fd, buf, len, MSG_DONTWAIT, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK) {
        char a[16];
        rx_link_addr_text(to, a);
        rx_debug("link: send to %s:%u: %s", a, port, strerror(errno));
    }
}
