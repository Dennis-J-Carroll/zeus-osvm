#ifndef ZEUS_ISA_H
#define ZEUS_ISA_H

#include <stdint.h>
#include "zeus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Zeus_osVM Instruction Set Architecture
 *
 * Opcodes are 8-bit. Most instructions operate on the operand stack.
 * Immediates (when present) are signed 64-bit values stored little-endian
 * after the opcode byte.
 */
typedef enum {
    /* Halt */
    OP_HALT = 0x00,

    /* Stack manipulation */
    OP_PUSH  = 0x01,  /* PUSH imm: push immediate onto stack */
    OP_POP   = 0x02,  /* POP: discard top of stack */
    OP_DUP   = 0x03,  /* DUP: duplicate top of stack */
    OP_SWAP  = 0x04,  /* SWAP: swap top two stack items */
    OP_PICK  = 0x05,  /* PICK n: copy n-th item from top (0 = top) */
    OP_ROLL  = 0x06,  /* ROLL n: move n-th item to top */

    /* Arithmetic */
    OP_ADD = 0x10,
    OP_SUB = 0x11,
    OP_MUL = 0x12,
    OP_DIV = 0x13,
    OP_MOD = 0x14,
    OP_NEG = 0x15,

    /* Bitwise */
    OP_AND = 0x20,
    OP_OR  = 0x21,
    OP_XOR = 0x22,
    OP_NOT = 0x23,
    OP_SHL = 0x24,
    OP_SHR = 0x25,

    /* Comparison */
    OP_EQ = 0x30,
    OP_NE = 0x31,
    OP_LT = 0x32,
    OP_GT = 0x33,
    OP_LE = 0x34,
    OP_GE = 0x35,

    /* Control flow */
    OP_JMP  = 0x40,  /* JMP offset: unconditional relative jump */
    OP_JZ   = 0x41,  /* JZ offset: jump if top of stack is zero */
    OP_JNZ  = 0x42,  /* JNZ offset: jump if top of stack is non-zero */
    OP_CALL = 0x43,  /* CALL addr: push return address, jump to addr */
    OP_RET  = 0x44,  /* RET: pop return address and jump */

    /* Memory */
    OP_LOAD   = 0x50,  /* LOAD addr: push memory[addr] (64-bit) */
    OP_STORE  = 0x51,  /* STORE addr: pop value, store at memory[addr] */
    OP_LOADB  = 0x52,  /* LOADB addr: push memory[addr] (8-bit, sign-extended) */
    OP_STOREB = 0x53,  /* STOREB addr: pop value, store low byte at memory[addr] */
    OP_LOADI  = 0x54,  /* LOADI: pop addr, push memory[addr] (64-bit) */
    OP_STOREI = 0x55,  /* STOREI: pop addr, pop value, store at memory[addr] */
    OP_LOADBI = 0x56,  /* LOADBI: pop addr, push byte at memory[addr] */
    OP_STOREBI = 0x57, /* STOREBI: pop addr, pop value, store low byte */

    /* I/O */
    OP_PRINT  = 0x60,  /* PRINT: pop value, print as decimal with newline */
    OP_PRINTC = 0x61,  /* PRINTC: pop value, print as ASCII char */
    OP_READ   = 0x62,  /* READ: read one char from stdin, push it */

    /* Socket networking */
    OP_SOCK_TCP = 0x70,  /* SOCK_TCP: push TCP socket fd or -1 */
    OP_SOCK_UDP = 0x71,  /* SOCK_UDP: push UDP socket fd or -1 */
    OP_BIND     = 0x72,  /* port fd -- result   (result: 0 ok, -1 err) */
    OP_LISTEN   = 0x73,  /* backlog fd -- result */
    OP_ACCEPT   = 0x74,  /* fd -- new_fd */
    OP_CONNECT  = 0x75,  /* port addr fd -- result */
    OP_SEND     = 0x76,  /* len buf fd -- sent_bytes */
    OP_RECV     = 0x77,  /* len buf fd -- recv_bytes */
    OP_CLOSE    = 0x78,  /* fd -- result */

    /* Raw packet networking */
    OP_RAW_ALLOC    = 0x80,  /* size -- pkt_id */
    OP_RAW_FREE     = 0x81,  /* pkt_id -- result */
    OP_RAW_SET_PROTO = 0x82, /* proto pkt_id -- result */
    OP_RAW_SET_DST  = 0x83,  /* addr pkt_id -- result */
    OP_RAW_SET_SRC  = 0x84,  /* addr pkt_id -- result */
    OP_RAW_SET_PAYLOAD = 0x85, /* len buf pkt_id -- result */
    OP_RAW_SEND     = 0x86,  /* pkt_id iface -- result */
    OP_RAW_RECV     = 0x87,  /* len buf pkt_id -- recv_bytes */

    OP_COUNT
} ZeusOpcode;

/* Metadata for an opcode */
typedef struct {
    ZeusOpcode opcode;
    const char *name;
    int has_operand;   /* 1 if instruction is followed by a 64-bit immediate */
    int stack_delta;   /* Approximate stack effect (negative = consumes) */
} ZeusOpInfo;

/* Return static info for an opcode, or NULL if unknown. */
const ZeusOpInfo *zeus_op_info(ZeusOpcode op);

/* Lookup opcode by name (case-insensitive). Returns OP_COUNT on failure. */
ZeusOpcode zeus_op_by_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_ISA_H */
