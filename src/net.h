#ifndef ZEUS_NET_INTERNAL_H
#define ZEUS_NET_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "zeus.h"

/* Per-packet buffer used by raw packet opcodes */
typedef struct {
    bool used;
    uint8_t *buf;
    uint32_t size;
    uint32_t len;
    int proto;
    uint32_t src;
    uint32_t dst;
} ZeusPacket;

/* Networking state attached to a VM */
typedef struct ZeusNetState {
    ZeusPacket packets[ZEUS_MAX_PACKETS];
} ZeusNetState;

ZeusNetState *zeus_net_create(void);
void zeus_net_destroy(ZeusNetState *ns);

/* Socket primitives */
int64_t zeus_net_socket_tcp(void);
int64_t zeus_net_socket_udp(void);
ZeusError zeus_net_bind(int fd, int64_t port);
ZeusError zeus_net_listen(int fd, int64_t backlog);
int64_t zeus_net_accept(int fd);
ZeusError zeus_net_connect(int fd, const char *addr, int64_t port);
int64_t zeus_net_send(int fd, const uint8_t *buf, size_t len);
int64_t zeus_net_recv(int fd, uint8_t *buf, size_t len);
ZeusError zeus_net_close(int fd);

/* Raw packet primitives */
int64_t zeus_net_packet_alloc(ZeusNetState *ns, int64_t size);
ZeusError zeus_net_packet_free(ZeusNetState *ns, int64_t id);
ZeusError zeus_net_packet_set_proto(ZeusNetState *ns, int64_t id, int64_t proto);
ZeusError zeus_net_packet_set_dst(ZeusNetState *ns, int64_t id, int64_t addr);
ZeusError zeus_net_packet_set_src(ZeusNetState *ns, int64_t id, int64_t addr);
ZeusError zeus_net_packet_set_payload(ZeusNetState *ns, int64_t id,
                                      const uint8_t *buf, size_t len);
int64_t zeus_net_packet_send(ZeusNetState *ns, int64_t id, const char *iface);
int64_t zeus_net_packet_recv(ZeusNetState *ns, int64_t id, uint8_t *buf, size_t len);

#endif /* ZEUS_NET_INTERNAL_H */
