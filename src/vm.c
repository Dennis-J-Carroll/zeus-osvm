#include "vm.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Read 8 bytes from code at offset as a signed 64-bit little-endian value. */
static int64_t read_i64(const uint8_t *p) {
    uint64_t v = (uint64_t)p[0]
               | ((uint64_t)p[1] << 8)
               | ((uint64_t)p[2] << 16)
               | ((uint64_t)p[3] << 24)
               | ((uint64_t)p[4] << 32)
               | ((uint64_t)p[5] << 40)
               | ((uint64_t)p[6] << 48)
               | ((uint64_t)p[7] << 56);
    return (int64_t)v;
}

/* Write 8 bytes little-endian. */
static void write_i64(uint8_t *p, int64_t v) {
    uint64_t u = (uint64_t)v;
    p[0] = (uint8_t)(u & 0xFF);
    p[1] = (uint8_t)((u >> 8) & 0xFF);
    p[2] = (uint8_t)((u >> 16) & 0xFF);
    p[3] = (uint8_t)((u >> 24) & 0xFF);
    p[4] = (uint8_t)((u >> 32) & 0xFF);
    p[5] = (uint8_t)((u >> 40) & 0xFF);
    p[6] = (uint8_t)((u >> 48) & 0xFF);
    p[7] = (uint8_t)((u >> 56) & 0xFF);
}

void zeus_vm_init(ZeusVM *vm) {
    if (!vm) return;
    memset(vm, 0, sizeof(*vm));
    zeus_program_init(&vm->program);
    vm->net = zeus_net_create();
}

void zeus_vm_free(ZeusVM *vm) {
    if (!vm) return;
    zeus_program_free(&vm->program);
    zeus_net_destroy(vm->net);
    vm->net = NULL;
}

ZeusError zeus_vm_load(ZeusVM *vm, const ZeusProgram *prog) {
    if (!vm || !prog) return ZEUS_ERR_IO;
    zeus_program_free(&vm->program);

    vm->program.header = prog->header;
    if (prog->header.code_len > 0) {
        vm->program.code = (uint8_t *)malloc(prog->header.code_len);
        if (!vm->program.code) return ZEUS_ERR_IO;
        memcpy(vm->program.code, prog->code, prog->header.code_len);
    }
    if (prog->header.data_len > 0) {
        vm->program.data = (uint8_t *)malloc(prog->header.data_len);
        if (!vm->program.data) return ZEUS_ERR_IO;
        memcpy(vm->program.data, prog->data, prog->header.data_len);
    }

    zeus_vm_reset(vm);

    /* Copy program data into VM memory at address 0. */
    if (prog->header.data_len > 0) {
        size_t to_copy = prog->header.data_len;
        if (to_copy > ZEUS_MEMORY_SIZE) to_copy = ZEUS_MEMORY_SIZE;
        memcpy(vm->memory, prog->data, to_copy);
    }

    return ZEUS_OK;
}

void zeus_vm_reset(ZeusVM *vm) {
    if (!vm) return;
    vm->pc = vm->program.header.entry_point;
    vm->sp = 0;
    vm->csp = 0;
    vm->error = ZEUS_OK;
    vm->running = false;
    memset(vm->stack, 0, sizeof(vm->stack));
    memset(vm->call_stack, 0, sizeof(vm->call_stack));
    memset(vm->memory, 0, sizeof(vm->memory));
}

static inline ZeusError vm_push(ZeusVM *vm, int64_t v) {
    if (vm->sp >= ZEUS_STACK_SIZE) {
        vm->running = false;
        vm->error = ZEUS_ERR_STACK_OVERFLOW;
        return vm->error;
    }
    vm->stack[vm->sp++] = v;
    return ZEUS_OK;
}

static inline ZeusError vm_pop(ZeusVM *vm, int64_t *v) {
    if (vm->sp <= 0) {
        vm->running = false;
        vm->error = ZEUS_ERR_STACK_UNDERFLOW;
        return vm->error;
    }
    *v = vm->stack[--vm->sp];
    return ZEUS_OK;
}

static inline ZeusError vm_call_push(ZeusVM *vm, uint32_t addr) {
    if (vm->csp >= ZEUS_CALL_SIZE) {
        vm->running = false;
        vm->error = ZEUS_ERR_CALL_OVERFLOW;
        return vm->error;
    }
    vm->call_stack[vm->csp++] = addr;
    return ZEUS_OK;
}

static inline ZeusError vm_call_pop(ZeusVM *vm, uint32_t *addr) {
    if (vm->csp <= 0) {
        vm->running = false;
        vm->error = ZEUS_ERR_CALL_UNDERFLOW;
        return vm->error;
    }
    *addr = vm->call_stack[--vm->csp];
    return ZEUS_OK;
}

static inline bool code_ok(const ZeusVM *vm, uint32_t offset, uint32_t bytes) {
    uint64_t end = (uint64_t)offset + (uint64_t)bytes;
    return end <= vm->program.header.code_len;
}

static inline bool mem_ok(uint32_t addr, uint32_t bytes) {
    uint64_t end = (uint64_t)addr + (uint64_t)bytes;
    return end <= ZEUS_MEMORY_SIZE;
}

ZeusError zeus_vm_step(ZeusVM *vm) {
    if (!vm->running) return vm->error;

    if (!code_ok(vm, vm->pc, 1)) {
        vm->running = false;
        vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
        return vm->error;
    }

    uint8_t op = vm->program.code[vm->pc++];
    const ZeusOpInfo *info = zeus_op_info((ZeusOpcode)op);

    int64_t a, b, result;
    uint32_t addr;

    /* If the instruction needs an operand, make sure it is present. */
    if (info && info->has_operand && !code_ok(vm, vm->pc, 8)) {
        vm->running = false;
        vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
        return vm->error;
    }

    switch (op) {
        case OP_HALT:
            vm->running = false;
            return ZEUS_OK;

        case OP_PUSH:
            result = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            return vm_push(vm, result);

        case OP_POP:
            return vm_pop(vm, &a);

        case OP_DUP:
            if (vm->sp <= 0) {
                vm->error = ZEUS_ERR_STACK_UNDERFLOW;
                vm->running = false;
                return vm->error;
            }
            return vm_push(vm, vm->stack[vm->sp - 1]);

        case OP_SWAP:
            if (vm->sp < 2) {
                vm->error = ZEUS_ERR_STACK_UNDERFLOW;
                vm->running = false;
                return vm->error;
            }
            a = vm->stack[vm->sp - 1];
            vm->stack[vm->sp - 1] = vm->stack[vm->sp - 2];
            vm->stack[vm->sp - 2] = a;
            return ZEUS_OK;

        case OP_PICK:
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (a < 0 || a >= vm->sp) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            return vm_push(vm, vm->stack[vm->sp - 1 - (size_t)a]);

        case OP_ROLL:
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (a < 0 || a >= vm->sp) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            {
                size_t idx = vm->sp - 1 - (size_t)a;
                int64_t tmp = vm->stack[idx];
                memmove(&vm->stack[idx], &vm->stack[idx + 1],
                        sizeof(int64_t) * (vm->sp - 1 - idx));
                vm->stack[vm->sp - 1] = tmp;
            }
            return ZEUS_OK;

        case OP_ADD:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a + b);

        case OP_SUB:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a - b);

        case OP_MUL:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a * b);

        case OP_DIV:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (b == 0) {
                vm->error = ZEUS_ERR_DIV_ZERO;
                vm->running = false;
                return vm->error;
            }
            return vm_push(vm, a / b);

        case OP_MOD:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (b == 0) {
                vm->error = ZEUS_ERR_DIV_ZERO;
                vm->running = false;
                return vm->error;
            }
            return vm_push(vm, a % b);

        case OP_NEG:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, -a);

        case OP_AND:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a & b);

        case OP_OR:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a | b);

        case OP_XOR:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a ^ b);

        case OP_NOT:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, ~a);

        case OP_SHL:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a << b);

        case OP_SHR:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a >> b);

        case OP_EQ:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a == b ? 1 : 0);

        case OP_NE:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a != b ? 1 : 0);

        case OP_LT:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a < b ? 1 : 0);

        case OP_GT:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a > b ? 1 : 0);

        case OP_LE:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a <= b ? 1 : 0);

        case OP_GE:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, a >= b ? 1 : 0);

        case OP_JMP:
            addr = (uint32_t)read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            vm->pc = addr;
            return ZEUS_OK;

        case OP_JZ:
            addr = (uint32_t)read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (a == 0) vm->pc = addr;
            return ZEUS_OK;

        case OP_JNZ:
            addr = (uint32_t)read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (a != 0) vm->pc = addr;
            return ZEUS_OK;

        case OP_CALL:
            addr = (uint32_t)read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (vm_call_push(vm, vm->pc) != ZEUS_OK) return vm->error;
            vm->pc = addr;
            return ZEUS_OK;

        case OP_RET:
            if (vm_call_pop(vm, &addr) != ZEUS_OK) return vm->error;
            vm->pc = addr;
            return ZEUS_OK;

        case OP_LOAD:
            addr = (uint32_t)read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (!mem_ok( addr, 8)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            return vm_push(vm, read_i64(vm->memory + addr));

        case OP_STORE:
            addr = (uint32_t)read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (!mem_ok( addr, 8)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            write_i64(vm->memory + addr, a);
            return ZEUS_OK;

        case OP_LOADB:
            addr = (uint32_t)read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (!mem_ok( addr, 1)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            return vm_push(vm, (int64_t)(int8_t)vm->memory[addr]);

        case OP_STOREB:
            addr = (uint32_t)read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (!mem_ok( addr, 1)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            vm->memory[addr] = (uint8_t)(a & 0xFF);
            return ZEUS_OK;

        case OP_LOADI:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (!mem_ok( (uint32_t)a, 8)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            return vm_push(vm, read_i64(vm->memory + (uint32_t)a));

        case OP_STOREI:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* address */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* value */
            if (!mem_ok( (uint32_t)a, 8)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            write_i64(vm->memory + (uint32_t)a, b);
            return ZEUS_OK;

        case OP_LOADBI:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (!mem_ok( (uint32_t)a, 1)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            return vm_push(vm, (int64_t)(int8_t)vm->memory[(uint32_t)a]);

        case OP_STOREBI:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* address */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* value */
            if (!mem_ok( (uint32_t)a, 1)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            vm->memory[(uint32_t)a] = (uint8_t)(b & 0xFF);
            return ZEUS_OK;

        case OP_PRINT:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            printf("%lld\n", (long long)a);
            fflush(stdout);
            return ZEUS_OK;

        case OP_PRINTC:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            putchar((int)(a & 0xFF));
            fflush(stdout);
            return ZEUS_OK;

        case OP_READ:
            a = getchar();
            return vm_push(vm, a);

        /* ---------- Socket networking ---------- */
        case OP_SOCK_TCP:
            return vm_push(vm, zeus_net_socket_tcp());

        case OP_SOCK_UDP:
            return vm_push(vm, zeus_net_socket_udp());

        case OP_BIND: {
            int64_t port, fd;
            if (vm_pop(vm, &port) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &fd) != ZEUS_OK) return vm->error;
            ZeusError e = zeus_net_bind((int)fd, port);
            return vm_push(vm, (e == ZEUS_OK) ? 0 : -1);
        }

        case OP_LISTEN: {
            int64_t backlog, fd;
            if (vm_pop(vm, &backlog) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &fd) != ZEUS_OK) return vm->error;
            ZeusError e = zeus_net_listen((int)fd, backlog);
            return vm_push(vm, (e == ZEUS_OK) ? 0 : -1);
        }

        case OP_ACCEPT: {
            int64_t fd;
            if (vm_pop(vm, &fd) != ZEUS_OK) return vm->error;
            return vm_push(vm, zeus_net_accept((int)fd));
        }

        case OP_CONNECT: {
            int64_t port, fd;
            if (vm_pop(vm, &port) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* address pointer */
            if (vm_pop(vm, &fd) != ZEUS_OK) return vm->error;
            if (!mem_ok( (uint32_t)a, 1)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            ZeusError e = zeus_net_connect((int)fd, (const char *)vm->memory + (uint32_t)a, port);
            return vm_push(vm, (e == ZEUS_OK) ? 0 : -1);
        }

        case OP_SEND: {
            int64_t len, bufp, fd;
            if (vm_pop(vm, &len) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &bufp) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &fd) != ZEUS_OK) return vm->error;
            if (!mem_ok((uint32_t)bufp, (uint32_t)len)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            return vm_push(vm, zeus_net_send((int)fd, vm->memory + (uint32_t)bufp, (size_t)len));
        }

        case OP_RECV: {
            int64_t len, bufp, fd;
            if (vm_pop(vm, &len) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &bufp) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &fd) != ZEUS_OK) return vm->error;
            if (!mem_ok((uint32_t)bufp, (uint32_t)len)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            return vm_push(vm, zeus_net_recv((int)fd, vm->memory + (uint32_t)bufp, (size_t)len));
        }

        case OP_CLOSE: {
            int64_t fd;
            if (vm_pop(vm, &fd) != ZEUS_OK) return vm->error;
            ZeusError e = zeus_net_close((int)fd);
            return vm_push(vm, (e == ZEUS_OK) ? 0 : -1);
        }

        /* ---------- Raw packet networking ---------- */
        case OP_RAW_ALLOC: {
            int64_t size;
            if (vm_pop(vm, &size) != ZEUS_OK) return vm->error;
            return vm_push(vm, zeus_net_packet_alloc(vm->net, size));
        }

        case OP_RAW_FREE: {
            int64_t id;
            if (vm_pop(vm, &id) != ZEUS_OK) return vm->error;
            ZeusError e = zeus_net_packet_free(vm->net, id);
            return vm_push(vm, (e == ZEUS_OK) ? 0 : -1);
        }

        case OP_RAW_SET_PROTO: {
            int64_t id, proto;
            if (vm_pop(vm, &proto) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &id) != ZEUS_OK) return vm->error;
            ZeusError e = zeus_net_packet_set_proto(vm->net, id, proto);
            return vm_push(vm, (e == ZEUS_OK) ? 0 : -1);
        }

        case OP_RAW_SET_DST: {
            int64_t id, ip;
            if (vm_pop(vm, &ip) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &id) != ZEUS_OK) return vm->error;
            ZeusError e = zeus_net_packet_set_dst(vm->net, id, ip);
            return vm_push(vm, (e == ZEUS_OK) ? 0 : -1);
        }

        case OP_RAW_SET_SRC: {
            int64_t id, ip;
            if (vm_pop(vm, &ip) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &id) != ZEUS_OK) return vm->error;
            ZeusError e = zeus_net_packet_set_src(vm->net, id, ip);
            return vm_push(vm, (e == ZEUS_OK) ? 0 : -1);
        }

        case OP_RAW_SET_PAYLOAD: {
            int64_t id, len, bufp;
            if (vm_pop(vm, &len) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &bufp) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &id) != ZEUS_OK) return vm->error;
            if (!mem_ok((uint32_t)bufp, (uint32_t)len)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            ZeusError e = zeus_net_packet_set_payload(vm->net, id, vm->memory + (uint32_t)bufp, (size_t)len);
            return vm_push(vm, (e == ZEUS_OK) ? 0 : -1);
        }

        case OP_RAW_SEND: {
            int64_t id;
            const char *iface = NULL;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* iface pointer, may be 0 */
            if (vm_pop(vm, &id) != ZEUS_OK) return vm->error;
            if (a != 0) {
                if (!mem_ok( (uint32_t)a, 1)) {
                    vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                    vm->running = false;
                    return vm->error;
                }
                iface = (const char *)vm->memory + (uint32_t)a;
            }
            return vm_push(vm, zeus_net_packet_send(vm->net, id, iface));
        }

        case OP_RAW_RECV: {
            int64_t id, len, bufp;
            if (vm_pop(vm, &len) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &bufp) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &id) != ZEUS_OK) return vm->error;
            if (!mem_ok((uint32_t)bufp, (uint32_t)len)) {
                vm->error = ZEUS_ERR_OUT_OF_BOUNDS;
                vm->running = false;
                return vm->error;
            }
            return vm_push(vm, zeus_net_packet_recv(vm->net, id, vm->memory + (uint32_t)bufp, (size_t)len));
        }

        default:
            vm->error = ZEUS_ERR_INVALID_OPCODE;
            vm->running = false;
            return vm->error;
    }
}

ZeusError zeus_vm_run(ZeusVM *vm) {
    if (!vm) return ZEUS_ERR_UNKNOWN;
    vm->running = true;
    vm->error = ZEUS_OK;
    while (vm->running) {
        zeus_vm_step(vm);
    }
    return vm->error;
}

int64_t zeus_vm_peek(const ZeusVM *vm, int64_t depth) {
    if (!vm || depth < 0 || depth >= vm->sp) return 0;
    return vm->stack[vm->sp - 1 - depth];
}
