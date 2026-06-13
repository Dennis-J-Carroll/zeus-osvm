#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

ZeusNetState *zeus_net_create(void) {
    ZeusNetState *ns = (ZeusNetState *)calloc(1, sizeof(ZeusNetState));
    return ns;
}

void zeus_net_destroy(ZeusNetState *ns) {
    if (!ns) return;
    for (int i = 0; i < ZEUS_MAX_PACKETS; i++) {
        if (ns->packets[i].used && ns->packets[i].buf) {
            free(ns->packets[i].buf);
        }
    }
    free(ns);
}

int64_t zeus_net_socket_tcp(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    return (fd < 0) ? -1 : (int64_t)fd;
}

int64_t zeus_net_socket_udp(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    return (fd < 0) ? -1 : (int64_t)fd;
}

ZeusError zeus_net_bind(int fd, int64_t port) {
    if (fd < 0) return ZEUS_ERR_NET_BIND;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    int opt = 1;
#ifdef SO_REUSEADDR
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        return ZEUS_ERR_NET_BIND;
    }
    return ZEUS_OK;
}

ZeusError zeus_net_listen(int fd, int64_t backlog) {
    if (fd < 0) return ZEUS_ERR_NET_LISTEN;
    if (listen(fd, (int)backlog) < 0) {
        return ZEUS_ERR_NET_LISTEN;
    }
    return ZEUS_OK;
}

int64_t zeus_net_accept(int fd) {
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int newfd = accept(fd, (struct sockaddr *)&addr, &len);
    return (newfd < 0) ? -1 : (int64_t)newfd;
}

ZeusError zeus_net_connect(int fd, const char *addr, int64_t port) {
    if (fd < 0 || !addr) return ZEUS_ERR_NET_CONNECT;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, addr, &sin.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(addr);
        if (!he || !he->h_addr_list[0]) {
            return ZEUS_ERR_NET_CONNECT;
        }
        memcpy(&sin.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        return ZEUS_ERR_NET_CONNECT;
    }
    return ZEUS_OK;
}

int64_t zeus_net_send(int fd, const uint8_t *buf, size_t len) {
    if (fd < 0 || !buf) return -1;
    ssize_t n = send(fd, buf, len, 0);
    return (n < 0) ? -1 : (int64_t)n;
}

int64_t zeus_net_recv(int fd, uint8_t *buf, size_t len) {
    if (fd < 0 || !buf) return -1;
    ssize_t n = recv(fd, buf, len, 0);
    return (n < 0) ? -1 : (int64_t)n;
}

ZeusError zeus_net_close(int fd) {
    if (fd < 0) return ZEUS_OK;
#ifdef _WIN32
    if (closesocket((SOCKET)fd) < 0) return ZEUS_ERR_NET_SOCKET;
#else
    if (close(fd) < 0) return ZEUS_ERR_NET_SOCKET;
#endif
    return ZEUS_OK;
}

/* ---------- Raw packet helpers ---------- */

static ZeusPacket *packet_get(ZeusNetState *ns, int64_t id) {
    if (!ns || id < 0 || id >= ZEUS_MAX_PACKETS) return NULL;
    return ns->packets[id].used ? &ns->packets[id] : NULL;
}

int64_t zeus_net_packet_alloc(ZeusNetState *ns, int64_t size) {
    if (!ns || size <= 0 || size > ZEUS_MAX_PACKET_SIZE) return -1;
    for (int i = 0; i < ZEUS_MAX_PACKETS; i++) {
        if (!ns->packets[i].used) {
            ZeusPacket *p = &ns->packets[i];
            p->buf = (uint8_t *)malloc((size_t)size);
            if (!p->buf) return -1;
            memset(p->buf, 0, (size_t)size);
            p->size = (uint32_t)size;
            p->len = 0;
            p->proto = 0;
            p->src = 0;
            p->dst = 0;
            p->used = true;
            return (int64_t)i;
        }
    }
    return -1;
}

ZeusError zeus_net_packet_free(ZeusNetState *ns, int64_t id) {
    ZeusPacket *p = packet_get(ns, id);
    if (!p) return ZEUS_ERR_PACKET;
    free(p->buf);
    memset(p, 0, sizeof(*p));
    return ZEUS_OK;
}

ZeusError zeus_net_packet_set_proto(ZeusNetState *ns, int64_t id, int64_t proto) {
    ZeusPacket *p = packet_get(ns, id);
    if (!p) return ZEUS_ERR_PACKET;
    p->proto = (int)proto;
    return ZEUS_OK;
}

ZeusError zeus_net_packet_set_dst(ZeusNetState *ns, int64_t id, int64_t addr) {
    ZeusPacket *p = packet_get(ns, id);
    if (!p) return ZEUS_ERR_PACKET;
    p->dst = (uint32_t)addr;
    return ZEUS_OK;
}

ZeusError zeus_net_packet_set_src(ZeusNetState *ns, int64_t id, int64_t addr) {
    ZeusPacket *p = packet_get(ns, id);
    if (!p) return ZEUS_ERR_PACKET;
    p->src = (uint32_t)addr;
    return ZEUS_OK;
}

ZeusError zeus_net_packet_set_payload(ZeusNetState *ns, int64_t id,
                                      const uint8_t *buf, size_t len) {
    ZeusPacket *p = packet_get(ns, id);
    if (!p || !buf || len > p->size) return ZEUS_ERR_PACKET;
    memcpy(p->buf, buf, len);
    p->len = (uint32_t)len;
    return ZEUS_OK;
}

int64_t zeus_net_packet_send(ZeusNetState *ns, int64_t id, const char *iface) {
    ZeusPacket *p = packet_get(ns, id);
    if (!p) return -1;
    (void)iface;

    /* For the first version we send IP-layer raw datagrams. */
#ifndef _WIN32
    int fd = socket(AF_INET, SOCK_RAW, (p->proto > 0) ? p->proto : IPPROTO_RAW);
    if (fd < 0) {
        return -1;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = p->dst;

    ssize_t n = sendto(fd, p->buf, p->len, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
    close(fd);
    return (n < 0) ? -1 : (int64_t)n;
#else
    (void)iface;
    return -1;
#endif
}

int64_t zeus_net_packet_recv(ZeusNetState *ns, int64_t id, uint8_t *buf, size_t len) {
    ZeusPacket *p = packet_get(ns, id);
    if (!p || !buf) return -1;

#ifndef _WIN32
    int fd = socket(AF_INET, SOCK_RAW, (p->proto > 0) ? p->proto : IPPROTO_RAW);
    if (fd < 0) return -1;

    ssize_t n = recv(fd, buf, len, 0);
    close(fd);
    return (n < 0) ? -1 : (int64_t)n;
#else
    return -1;
#endif
}
