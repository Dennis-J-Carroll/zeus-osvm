#include "vm.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Little-endian helpers                                               */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Total arithmetic                                                    */
/*                                                                     */
/* C11 leaves signed overflow undefined. This machine defines it, so   */
/* that execution (and therefore replay and snapshots) is total and    */
/* identical on every host. Semantics are two's complement wraparound, */
/* shifts are masked to 0..63, and INT64_MIN / -1 wraps to INT64_MIN.  */
/* ------------------------------------------------------------------ */

static inline int64_t add_i64(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a + (uint64_t)b);
}

static inline int64_t sub_i64(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a - (uint64_t)b);
}

static inline int64_t mul_i64(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a * (uint64_t)b);
}

static inline int64_t neg_i64(int64_t a) {
    return (int64_t)(0u - (uint64_t)a);
}

static inline unsigned shift_count(int64_t b) {
    return (unsigned)((uint64_t)b & 63u);
}

static inline int64_t shl_i64(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a << shift_count(b));
}

static inline int64_t shr_i64(int64_t a, int64_t b) {
    unsigned n = shift_count(b);
    if (n == 0) return a;
    uint64_t u = (uint64_t)a >> n;
    if (a < 0) u |= (~UINT64_C(0)) << (64 - n);
    return (int64_t)u;
}

/* ------------------------------------------------------------------ */
/* Bounds                                                              */
/* ------------------------------------------------------------------ */

static inline bool code_range_ok(const ZeusVM *vm, int64_t off, int64_t bytes) {
    if (off < 0 || bytes < 0) return false;
    if ((uint64_t)off > (uint64_t)vm->program.header.code_len) return false;
    return (uint64_t)bytes <= (uint64_t)vm->program.header.code_len - (uint64_t)off;
}

/* Signed, non-truncating range check. Negative or oversized operands are
 * rejected here instead of being cast to uint32_t (which is what used to let
 * len=4294967296 through as len=0). */
static inline bool mem_range_ok(int64_t addr, int64_t bytes) {
    if (addr < 0 || bytes < 0) return false;
    if ((uint64_t)addr > ZEUS_MEMORY_SIZE) return false;
    return (uint64_t)bytes <= (uint64_t)ZEUS_MEMORY_SIZE - (uint64_t)addr;
}

/* True if a NUL terminator exists at or after `addr`. */
static inline bool mem_cstr_ok(const ZeusVM *vm, int64_t addr) {
    if (addr < 0 || addr >= ZEUS_MEMORY_SIZE) return false;
    for (int64_t i = addr; i < ZEUS_MEMORY_SIZE; i++) {
        if (vm->memory[i] == '\0') return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

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

    /* Never silently truncate: an oversized data section is a corrupt
     * program, not a program with a small data section. */
    if (prog->header.data_len > ZEUS_MEMORY_SIZE) return ZEUS_ERR_BAD_BYTECODE;
    if (prog->header.code_len > ZEUS_MAX_CODE_SIZE) return ZEUS_ERR_BAD_BYTECODE;

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
        memcpy(vm->memory, prog->data, prog->header.data_len);
    }

    return ZEUS_OK;
}

void zeus_vm_reset(ZeusVM *vm) {
    if (!vm) return;
    vm->pc = vm->program.header.entry_point;
    vm->sp = 0;
    vm->csp = 0;
    vm->error = ZEUS_OK;
    vm->started = false;
    vm->running = false;
    vm->yielded = false;
    memset(&vm->pending, 0, sizeof(vm->pending));
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

static inline ZeusError vm_fail(ZeusVM *vm, ZeusError err) {
    vm->running = false;
    vm->error = err;
    return err;
}

/* Stop at an effect boundary. The instruction is fully consumed (pc advanced,
 * operands popped); only the result is still owed. The effect is staged and
 * reported in one write, so the pending copy can never drift from `out`. */
static ZeusError vm_yield(ZeusVM *vm, ZeusEffect *out, ZeusEffect eff) {
    vm->pending = eff;
    vm->yielded = true;
    *out = eff;
    return ZEUS_OK;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

ZeusError zeus_vm_exec(ZeusVM *vm, ZeusEffect *out) {
    if (!vm || !out) return ZEUS_ERR_UNKNOWN;
    memset(out, 0, sizeof(*out));
    out->kind = ZEUS_EFFECT_NONE;

    if (!vm->running) return vm->error;
    if (vm->yielded) return ZEUS_ERR_EFFECT;

    if (!code_range_ok(vm, vm->pc, 1)) {
        return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
    }

    uint8_t op = vm->program.code[vm->pc++];
    const ZeusOpInfo *info = zeus_op_info((ZeusOpcode)op);

    int64_t a, b, result;

    /* If the instruction needs an operand, make sure it is present. */
    if (info && info->has_operand && !code_range_ok(vm, vm->pc, 8)) {
        return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
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
            if (vm->sp <= 0) return vm_fail(vm, ZEUS_ERR_STACK_UNDERFLOW);
            return vm_push(vm, vm->stack[vm->sp - 1]);

        case OP_SWAP:
            if (vm->sp < 2) return vm_fail(vm, ZEUS_ERR_STACK_UNDERFLOW);
            a = vm->stack[vm->sp - 1];
            vm->stack[vm->sp - 1] = vm->stack[vm->sp - 2];
            vm->stack[vm->sp - 2] = a;
            return ZEUS_OK;

        case OP_PICK:
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (a < 0 || a >= vm->sp) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            return vm_push(vm, vm->stack[vm->sp - 1 - (size_t)a]);

        case OP_ROLL:
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (a < 0 || a >= vm->sp) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            {
                size_t idx = (size_t)(vm->sp - 1 - a);
                int64_t tmp = vm->stack[idx];
                memmove(&vm->stack[idx], &vm->stack[idx + 1],
                        sizeof(int64_t) * (size_t)(vm->sp - 1 - (int64_t)idx));
                vm->stack[vm->sp - 1] = tmp;
            }
            return ZEUS_OK;

        case OP_ADD:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, add_i64(a, b));

        case OP_SUB:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, sub_i64(a, b));

        case OP_MUL:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, mul_i64(a, b));

        case OP_DIV:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (b == 0) return vm_fail(vm, ZEUS_ERR_DIV_ZERO);
            if (b == -1 && a == INT64_MIN) return vm_push(vm, INT64_MIN);
            return vm_push(vm, a / b);

        case OP_MOD:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (b == 0) return vm_fail(vm, ZEUS_ERR_DIV_ZERO);
            if (b == -1) return vm_push(vm, 0);
            return vm_push(vm, a % b);

        case OP_NEG:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, neg_i64(a));

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
            return vm_push(vm, shl_i64(a, b));

        case OP_SHR:
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_push(vm, shr_i64(a, b));

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
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            vm->pc = (uint32_t)a;
            return ZEUS_OK;

        case OP_JZ:
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (b == 0) vm->pc = (uint32_t)a;
            return ZEUS_OK;

        case OP_JNZ:
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (b != 0) vm->pc = (uint32_t)a;
            return ZEUS_OK;

        case OP_CALL:
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (vm_call_push(vm, vm->pc) != ZEUS_OK) return vm->error;
            vm->pc = (uint32_t)a;
            return ZEUS_OK;

        case OP_RET: {
            uint32_t addr;
            if (vm_call_pop(vm, &addr) != ZEUS_OK) return vm->error;
            vm->pc = addr;
            return ZEUS_OK;
        }

        case OP_LOAD:
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (!mem_range_ok(a, 8)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            return vm_push(vm, read_i64(vm->memory + (size_t)a));

        case OP_STORE:
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (!mem_range_ok(a, 8)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            write_i64(vm->memory + (size_t)a, b);
            return ZEUS_OK;

        case OP_LOADB:
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (!mem_range_ok(a, 1)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            return vm_push(vm, (int64_t)(int8_t)vm->memory[(size_t)a]);

        case OP_STOREB:
            a = read_i64(vm->program.code + vm->pc);
            vm->pc += 8;
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;
            if (!mem_range_ok(a, 1)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            vm->memory[(size_t)a] = (uint8_t)(b & 0xFF);
            return ZEUS_OK;

        case OP_LOADI:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (!mem_range_ok(a, 8)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            return vm_push(vm, read_i64(vm->memory + (size_t)a));

        case OP_STOREI:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* address */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* value */
            if (!mem_range_ok(a, 8)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            write_i64(vm->memory + (size_t)a, b);
            return ZEUS_OK;

        case OP_LOADBI:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            if (!mem_range_ok(a, 1)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            return vm_push(vm, (int64_t)(int8_t)vm->memory[(size_t)a]);

        case OP_STOREBI:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* address */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* value */
            if (!mem_range_ok(a, 1)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            vm->memory[(size_t)a] = (uint8_t)(b & 0xFF);
            return ZEUS_OK;

        /* ---------- I/O effects ---------- */

        case OP_PRINT:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_yield(vm, out,
                            (ZeusEffect){ .kind = ZEUS_EFFECT_PRINT, .value = a });

        case OP_PRINTC:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_yield(vm, out,
                            (ZeusEffect){ .kind = ZEUS_EFFECT_PRINTC, .value = a });

        case OP_READ:
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_READ });

        /* ---------- Socket networking ---------- */

        case OP_SOCK_TCP:
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_SOCK_TCP });

        case OP_SOCK_UDP:
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_SOCK_UDP });

        case OP_BIND:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* port */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* fd */
            return vm_yield(vm, out,
                            (ZeusEffect){ .kind = ZEUS_EFFECT_BIND, .fd = b, .value = a });

        case OP_LISTEN:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* backlog */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* fd */
            return vm_yield(vm, out,
                            (ZeusEffect){ .kind = ZEUS_EFFECT_LISTEN, .fd = b, .value = a });

        case OP_ACCEPT:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* fd */
            return vm_yield(vm, out,
                            (ZeusEffect){ .kind = ZEUS_EFFECT_ACCEPT, .fd = a });

        case OP_CONNECT:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;  /* port */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error;  /* host ptr */
            if (vm_pop(vm, &result) != ZEUS_OK) return vm->error; /* fd */
            if (!mem_cstr_ok(vm, b)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_CONNECT,
                                                   .fd = result,
                                                   .value = a,
                                                   .buf = (uint32_t)b });

        case OP_SEND:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* len */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* buf */
            if (vm_pop(vm, &result) != ZEUS_OK) return vm->error; /* fd */
            if (!mem_range_ok(b, a)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_SEND,
                                                   .fd = result,
                                                   .buf = (uint32_t)b,
                                                   .len = (uint32_t)a });

        case OP_RECV:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* len */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* buf */
            if (vm_pop(vm, &result) != ZEUS_OK) return vm->error; /* fd */
            if (!mem_range_ok(b, a)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_RECV,
                                                   .fd = result,
                                                   .buf = (uint32_t)b,
                                                   .len = (uint32_t)a });

        case OP_CLOSE:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_yield(vm, out,
                            (ZeusEffect){ .kind = ZEUS_EFFECT_CLOSE, .fd = a });

        /* ---------- Raw packet networking ---------- */

        case OP_RAW_ALLOC:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_yield(vm, out,
                            (ZeusEffect){ .kind = ZEUS_EFFECT_RAW_ALLOC, .value = a });

        case OP_RAW_FREE:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error;
            return vm_yield(vm, out,
                            (ZeusEffect){ .kind = ZEUS_EFFECT_RAW_FREE, .id = a });

        case OP_RAW_SET_PROTO:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* proto */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* id */
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_RAW_SET_PROTO,
                                                   .id = b, .value = a });

        case OP_RAW_SET_DST:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* ip */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* id */
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_RAW_SET_DST,
                                                   .id = b, .value = a });

        case OP_RAW_SET_SRC:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* ip */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* id */
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_RAW_SET_SRC,
                                                   .id = b, .value = a });

        case OP_RAW_SET_PAYLOAD:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* len */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* buf */
            if (vm_pop(vm, &result) != ZEUS_OK) return vm->error; /* id */
            if (!mem_range_ok(b, a)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_RAW_SET_PAYLOAD,
                                                   .id = result,
                                                   .buf = (uint32_t)b,
                                                   .len = (uint32_t)a });

        case OP_RAW_SEND:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* iface ptr */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* id */
            if (a != 0) {
                if (!mem_cstr_ok(vm, a)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            }
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_RAW_SEND,
                                                   .id = b, .buf = (uint32_t)a });

        case OP_RAW_RECV:
            if (vm_pop(vm, &a) != ZEUS_OK) return vm->error; /* len */
            if (vm_pop(vm, &b) != ZEUS_OK) return vm->error; /* buf */
            if (vm_pop(vm, &result) != ZEUS_OK) return vm->error; /* id */
            if (!mem_range_ok(b, a)) return vm_fail(vm, ZEUS_ERR_OUT_OF_BOUNDS);
            return vm_yield(vm, out, (ZeusEffect){ .kind = ZEUS_EFFECT_RAW_RECV,
                                                   .id = result,
                                                   .buf = (uint32_t)b,
                                                   .len = (uint32_t)a });

        default:
            return vm_fail(vm, ZEUS_ERR_INVALID_OPCODE);
    }
}

/* ------------------------------------------------------------------ */
/* Effect delivery                                                     */
/* ------------------------------------------------------------------ */

bool zeus_apply_native_effect(ZeusVM *vm, const ZeusEffect *eff, int64_t *result) {
    if (!vm || !eff || !result) return false;
    *result = -1;

    switch (eff->kind) {
        case ZEUS_EFFECT_PRINT:
            printf("%lld\n", (long long)eff->value);
            fflush(stdout);
            *result = 0;
            return true;

        case ZEUS_EFFECT_PRINTC:
            putchar((int)(eff->value & 0xFF));
            fflush(stdout);
            *result = 0;
            return true;

        case ZEUS_EFFECT_READ:
            *result = (int64_t)getchar();
            return true;

        case ZEUS_EFFECT_SOCK_TCP:
            *result = zeus_net_socket_tcp();
            return true;

        case ZEUS_EFFECT_SOCK_UDP:
            *result = zeus_net_socket_udp();
            return true;

        case ZEUS_EFFECT_BIND:
            *result = (zeus_net_bind((int)eff->fd, eff->value) == ZEUS_OK) ? 0 : -1;
            return true;

        case ZEUS_EFFECT_LISTEN:
            *result = (zeus_net_listen((int)eff->fd, eff->value) == ZEUS_OK) ? 0 : -1;
            return true;

        case ZEUS_EFFECT_ACCEPT:
            *result = zeus_net_accept((int)eff->fd);
            return true;

        case ZEUS_EFFECT_CONNECT:
            if (eff->buf >= ZEUS_MEMORY_SIZE) return false;
            *result = (zeus_net_connect((int)eff->fd,
                                        (const char *)vm->memory + eff->buf,
                                        eff->value) == ZEUS_OK) ? 0 : -1;
            return true;

        case ZEUS_EFFECT_SEND:
            if (!mem_range_ok((int64_t)eff->buf, (int64_t)eff->len)) return false;
            *result = zeus_net_send((int)eff->fd, vm->memory + eff->buf, (size_t)eff->len);
            return true;

        case ZEUS_EFFECT_RECV:
            if (!mem_range_ok((int64_t)eff->buf, (int64_t)eff->len)) return false;
            *result = zeus_net_recv((int)eff->fd, vm->memory + eff->buf, (size_t)eff->len);
            return true;

        case ZEUS_EFFECT_CLOSE:
            *result = (zeus_net_close((int)eff->fd) == ZEUS_OK) ? 0 : -1;
            return true;

        case ZEUS_EFFECT_RAW_ALLOC:
            *result = zeus_net_packet_alloc(vm->net, eff->value);
            return true;

        case ZEUS_EFFECT_RAW_FREE:
            *result = (zeus_net_packet_free(vm->net, eff->id) == ZEUS_OK) ? 0 : -1;
            return true;

        case ZEUS_EFFECT_RAW_SET_PROTO:
            *result = (zeus_net_packet_set_proto(vm->net, eff->id, eff->value) == ZEUS_OK) ? 0 : -1;
            return true;

        case ZEUS_EFFECT_RAW_SET_DST:
            *result = (zeus_net_packet_set_dst(vm->net, eff->id, eff->value) == ZEUS_OK) ? 0 : -1;
            return true;

        case ZEUS_EFFECT_RAW_SET_SRC:
            *result = (zeus_net_packet_set_src(vm->net, eff->id, eff->value) == ZEUS_OK) ? 0 : -1;
            return true;

        case ZEUS_EFFECT_RAW_SET_PAYLOAD:
            if (!mem_range_ok((int64_t)eff->buf, (int64_t)eff->len)) return false;
            *result = (zeus_net_packet_set_payload(vm->net, eff->id,
                                                   vm->memory + eff->buf,
                                                   (size_t)eff->len) == ZEUS_OK) ? 0 : -1;
            return true;

        case ZEUS_EFFECT_RAW_SEND: {
            const char *iface = NULL;
            if (eff->buf != 0) {
                if (!mem_cstr_ok(vm, (int64_t)eff->buf)) return false;
                iface = (const char *)vm->memory + eff->buf;
            }
            *result = zeus_net_packet_send(vm->net, eff->id, iface);
            return true;
        }

        case ZEUS_EFFECT_RAW_RECV:
            if (!mem_range_ok((int64_t)eff->buf, (int64_t)eff->len)) return false;
            *result = zeus_net_packet_recv(vm->net, eff->id,
                                           vm->memory + eff->buf, (size_t)eff->len);
            return true;

        default:
            return false;
    }
}

/* PRINT and PRINTC are pure sinks: they consume their operand and leave
 * nothing behind. Every other effect produces exactly one stack value. */
static bool effect_has_result(ZeusEffectKind kind) {
    return kind != ZEUS_EFFECT_PRINT && kind != ZEUS_EFFECT_PRINTC;
}

ZeusError zeus_vm_resume(ZeusVM *vm, int64_t result) {
    if (!vm) return ZEUS_ERR_UNKNOWN;
    if (!vm->yielded) return ZEUS_ERR_EFFECT;

    ZeusEffectKind kind = vm->pending.kind;
    vm->yielded = false;
    memset(&vm->pending, 0, sizeof(vm->pending));

    if (!effect_has_result(kind)) return ZEUS_OK;
    return vm_push(vm, result);
}

ZeusError zeus_vm_cancel_effect(ZeusVM *vm, ZeusError reason) {
    if (!vm) return ZEUS_ERR_UNKNOWN;
    if (!vm->yielded) return ZEUS_ERR_EFFECT;

    vm->yielded = false;
    memset(&vm->pending, 0, sizeof(vm->pending));
    vm->running = false;
    vm->error = (reason == ZEUS_OK) ? ZEUS_ERR_EFFECT : reason;
    return vm->error;
}

ZeusError zeus_vm_deliver_native(ZeusVM *vm, int64_t *result) {
    if (!vm || !result) return ZEUS_ERR_UNKNOWN;
    if (!vm->yielded) return ZEUS_ERR_EFFECT;

    *result = -1;
    if (!zeus_apply_native_effect(vm, &vm->pending, result)) {
        /* A failed handler leaves no result to resume with. Consume the
         * effect so the halted machine is consistent and snapshottable; the
         * old "yield stays pending" state could not be serialized at all. */
        return zeus_vm_cancel_effect(vm, ZEUS_ERR_EFFECT);
    }
    return zeus_vm_resume(vm, *result);
}

ZeusError zeus_vm_step(ZeusVM *vm) {
    if (!vm) return ZEUS_ERR_UNKNOWN;

    /* Deliver a pending effect first (this is what makes resume work). */
    if (!vm->yielded) {
        if (!vm->running) return vm->error;

        ZeusEffect eff;
        ZeusError err = zeus_vm_exec(vm, &eff);
        if (err != ZEUS_OK || !vm->yielded) return err;
    }

    int64_t result;
    return zeus_vm_deliver_native(vm, &result);
}

void zeus_vm_start(ZeusVM *vm) {
    if (!vm) return;
    vm->started = true;
    vm->running = true;
    vm->error = ZEUS_OK;
}

ZeusError zeus_vm_run(ZeusVM *vm) {
    if (!vm) return ZEUS_ERR_UNKNOWN;
    zeus_vm_start(vm);
    while (vm->running) {
        zeus_vm_step(vm);
    }
    return vm->error;
}

ZeusVMStatus zeus_vm_status(const ZeusVM *vm) {
    if (!vm || !vm->running) return ZEUS_VM_HALTED;
    return vm->yielded ? ZEUS_VM_YIELDED : ZEUS_VM_RUNNING;
}

bool zeus_effect_operands_valid(const ZeusVM *vm, const ZeusEffect *eff) {
    if (!vm || !eff) return false;

    switch (eff->kind) {
        case ZEUS_EFFECT_PRINT:
        case ZEUS_EFFECT_PRINTC:
        case ZEUS_EFFECT_READ:
        case ZEUS_EFFECT_SOCK_TCP:
        case ZEUS_EFFECT_SOCK_UDP:
        case ZEUS_EFFECT_BIND:
        case ZEUS_EFFECT_LISTEN:
        case ZEUS_EFFECT_ACCEPT:
        case ZEUS_EFFECT_CLOSE:
        case ZEUS_EFFECT_RAW_ALLOC:
        case ZEUS_EFFECT_RAW_FREE:
        case ZEUS_EFFECT_RAW_SET_PROTO:
        case ZEUS_EFFECT_RAW_SET_DST:
        case ZEUS_EFFECT_RAW_SET_SRC:
            /* Scalar-only effects never carry a buffer. */
            return eff->buf == 0 && eff->len == 0;

        case ZEUS_EFFECT_CONNECT:
            return eff->buf < ZEUS_MEMORY_SIZE &&
                   mem_cstr_ok(vm, (int64_t)eff->buf);

        case ZEUS_EFFECT_SEND:
        case ZEUS_EFFECT_RECV:
        case ZEUS_EFFECT_RAW_SET_PAYLOAD:
        case ZEUS_EFFECT_RAW_RECV:
            return mem_range_ok((int64_t)eff->buf, (int64_t)eff->len);

        case ZEUS_EFFECT_RAW_SEND:
            return eff->buf == 0 || mem_cstr_ok(vm, (int64_t)eff->buf);

        default:
            return false;
    }
}

const char *zeus_effect_name(ZeusEffectKind kind) {
    switch (kind) {
        case ZEUS_EFFECT_NONE:            return "none";
        case ZEUS_EFFECT_PRINT:           return "print";
        case ZEUS_EFFECT_PRINTC:          return "printc";
        case ZEUS_EFFECT_READ:            return "read";
        case ZEUS_EFFECT_SOCK_TCP:        return "sock_tcp";
        case ZEUS_EFFECT_SOCK_UDP:        return "sock_udp";
        case ZEUS_EFFECT_BIND:            return "bind";
        case ZEUS_EFFECT_LISTEN:          return "listen";
        case ZEUS_EFFECT_ACCEPT:          return "accept";
        case ZEUS_EFFECT_CONNECT:         return "connect";
        case ZEUS_EFFECT_SEND:            return "send";
        case ZEUS_EFFECT_RECV:            return "recv";
        case ZEUS_EFFECT_CLOSE:           return "close";
        case ZEUS_EFFECT_RAW_ALLOC:       return "raw_alloc";
        case ZEUS_EFFECT_RAW_FREE:        return "raw_free";
        case ZEUS_EFFECT_RAW_SET_PROTO:   return "raw_set_proto";
        case ZEUS_EFFECT_RAW_SET_DST:     return "raw_set_dst";
        case ZEUS_EFFECT_RAW_SET_SRC:     return "raw_set_src";
        case ZEUS_EFFECT_RAW_SET_PAYLOAD: return "raw_set_payload";
        case ZEUS_EFFECT_RAW_SEND:        return "raw_send";
        case ZEUS_EFFECT_RAW_RECV:        return "raw_recv";
        default:                          return "unknown";
    }
}

int64_t zeus_vm_peek(const ZeusVM *vm, int64_t depth) {
    if (!vm || depth < 0 || depth >= vm->sp) return 0;
    return vm->stack[vm->sp - 1 - depth];
}
