#include "zeus.h"

const char *zeus_error_string(ZeusError err) {
    switch (err) {
        case ZEUS_OK:                return "ok";
        case ZEUS_ERR_STACK_UNDERFLOW: return "stack underflow";
        case ZEUS_ERR_STACK_OVERFLOW:  return "stack overflow";
        case ZEUS_ERR_CALL_UNDERFLOW:  return "call stack underflow";
        case ZEUS_ERR_CALL_OVERFLOW:   return "call stack overflow";
        case ZEUS_ERR_OUT_OF_BOUNDS:   return "out of bounds";
        case ZEUS_ERR_DIV_ZERO:        return "division by zero";
        case ZEUS_ERR_INVALID_OPCODE:  return "invalid opcode";
        case ZEUS_ERR_NET_SOCKET:      return "network socket error";
        case ZEUS_ERR_NET_BIND:        return "network bind error";
        case ZEUS_ERR_NET_LISTEN:      return "network listen error";
        case ZEUS_ERR_NET_ACCEPT:      return "network accept error";
        case ZEUS_ERR_NET_CONNECT:     return "network connect error";
        case ZEUS_ERR_NET_SEND:        return "network send error";
        case ZEUS_ERR_NET_RECV:        return "network receive error";
        case ZEUS_ERR_NET_RAW:         return "raw socket error";
        case ZEUS_ERR_PACKET:          return "packet error";
        case ZEUS_ERR_IO:              return "i/o error";
        case ZEUS_ERR_HALT:            return "halted";
        default:                       return "unknown error";
    }
}
