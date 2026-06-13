#include "isa.h"
#include <string.h>
#include <ctype.h>

static const ZeusOpInfo op_infos[] = {
    { OP_HALT,  "HALT",  0, 0 },

    { OP_PUSH,  "PUSH",  1, 1 },
    { OP_POP,   "POP",   0, -1 },
    { OP_DUP,   "DUP",   0, 1 },
    { OP_SWAP,  "SWAP",  0, 0 },
    { OP_PICK,  "PICK",  1, 1 },
    { OP_ROLL,  "ROLL",  1, 0 },

    { OP_ADD, "ADD", 0, -1 },
    { OP_SUB, "SUB", 0, -1 },
    { OP_MUL, "MUL", 0, -1 },
    { OP_DIV, "DIV", 0, -1 },
    { OP_MOD, "MOD", 0, -1 },
    { OP_NEG, "NEG", 0, 0 },

    { OP_AND, "AND", 0, -1 },
    { OP_OR,  "OR",  0, -1 },
    { OP_XOR, "XOR", 0, -1 },
    { OP_NOT, "NOT", 0, 0 },
    { OP_SHL, "SHL", 0, -1 },
    { OP_SHR, "SHR", 0, -1 },

    { OP_EQ, "EQ", 0, -1 },
    { OP_NE, "NE", 0, -1 },
    { OP_LT, "LT", 0, -1 },
    { OP_GT, "GT", 0, -1 },
    { OP_LE, "LE", 0, -1 },
    { OP_GE, "GE", 0, -1 },

    { OP_JMP,  "JMP",  1, 0 },
    { OP_JZ,   "JZ",   1, -1 },
    { OP_JNZ,  "JNZ",  1, -1 },
    { OP_CALL, "CALL", 1, 0 },
    { OP_RET,  "RET",  0, 0 },

    { OP_LOAD,    "LOAD",    1, 1 },
    { OP_STORE,   "STORE",   1, -1 },
    { OP_LOADB,   "LOADB",   1, 1 },
    { OP_STOREB,  "STOREB",  1, -1 },
    { OP_LOADI,   "LOADI",   0, 0 },
    { OP_STOREI,  "STOREI",  0, -2 },
    { OP_LOADBI,  "LOADBI",  0, 0 },
    { OP_STOREBI, "STOREBI", 0, -2 },

    { OP_PRINT,  "PRINT",  0, -1 },
    { OP_PRINTC, "PRINTC", 0, -1 },
    { OP_READ,   "READ",   0, 1 },

    { OP_SOCK_TCP, "SOCK_TCP", 0, 1 },
    { OP_SOCK_UDP, "SOCK_UDP", 0, 1 },
    { OP_BIND,     "BIND",     0, -1 },
    { OP_LISTEN,   "LISTEN",   0, -1 },
    { OP_ACCEPT,   "ACCEPT",   0, 0 },
    { OP_CONNECT,  "CONNECT",  0, -2 },
    { OP_SEND,     "SEND",     0, -2 },
    { OP_RECV,     "RECV",     0, -1 },
    { OP_CLOSE,    "CLOSE",    0, -1 },

    { OP_RAW_ALLOC,     "RAW_ALLOC",     0, 0 },
    { OP_RAW_FREE,      "RAW_FREE",      0, -1 },
    { OP_RAW_SET_PROTO, "RAW_SET_PROTO", 0, -1 },
    { OP_RAW_SET_DST,   "RAW_SET_DST",   0, -1 },
    { OP_RAW_SET_SRC,   "RAW_SET_SRC",   0, -1 },
    { OP_RAW_SET_PAYLOAD, "RAW_SET_PAYLOAD", 0, -2 },
    { OP_RAW_SEND,      "RAW_SEND",      0, -1 },
    { OP_RAW_RECV,      "RAW_RECV",      0, -1 },
};

const ZeusOpInfo *zeus_op_info(ZeusOpcode op) {
    for (size_t i = 0; i < sizeof(op_infos) / sizeof(op_infos[0]); i++) {
        if (op_infos[i].opcode == op) {
            return &op_infos[i];
        }
    }
    return NULL;
}

ZeusOpcode zeus_op_by_name(const char *name) {
    char upper[32];
    size_t len = strlen(name);
    if (len >= sizeof(upper)) len = sizeof(upper) - 1;
    for (size_t i = 0; i < len; i++) {
        upper[i] = (char)toupper((unsigned char)name[i]);
    }
    upper[len] = '\0';

    for (size_t i = 0; i < sizeof(op_infos) / sizeof(op_infos[0]); i++) {
        if (strcmp(op_infos[i].name, upper) == 0) {
            return op_infos[i].opcode;
        }
    }
    return OP_COUNT;
}
