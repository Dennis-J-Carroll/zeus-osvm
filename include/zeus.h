#ifndef ZEUS_H
#define ZEUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zeus_osVM common definitions */
#define ZEUS_NAME     "Zeus_osVM"
#define ZEUS_VERSION  1

/* Magic bytes for Zeus bytecode files */
#define ZEUS_MAGIC    "ZEUS"
#define ZEUS_MAGIC_LEN 4

/* Resource limits */
#define ZEUS_STACK_SIZE   8192
#define ZEUS_CALL_SIZE    1024
#define ZEUS_MEMORY_SIZE  65536
#define ZEUS_MAX_PACKETS  16
#define ZEUS_MAX_PACKET_SIZE 65535

/* Error codes returned by VM operations */
typedef enum {
    ZEUS_OK = 0,
    ZEUS_ERR_STACK_UNDERFLOW,
    ZEUS_ERR_STACK_OVERFLOW,
    ZEUS_ERR_CALL_UNDERFLOW,
    ZEUS_ERR_CALL_OVERFLOW,
    ZEUS_ERR_OUT_OF_BOUNDS,
    ZEUS_ERR_DIV_ZERO,
    ZEUS_ERR_INVALID_OPCODE,
    ZEUS_ERR_NET_SOCKET,
    ZEUS_ERR_NET_BIND,
    ZEUS_ERR_NET_LISTEN,
    ZEUS_ERR_NET_ACCEPT,
    ZEUS_ERR_NET_CONNECT,
    ZEUS_ERR_NET_SEND,
    ZEUS_ERR_NET_RECV,
    ZEUS_ERR_NET_RAW,
    ZEUS_ERR_PACKET,
    ZEUS_ERR_IO,
    ZEUS_ERR_HALT,
    ZEUS_ERR_UNKNOWN
} ZeusError;

const char *zeus_error_string(ZeusError err);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_H */
