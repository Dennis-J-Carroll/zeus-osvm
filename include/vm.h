#ifndef ZEUS_VM_H
#define ZEUS_VM_H

#include <stdint.h>
#include <stdbool.h>
#include "zeus.h"
#include "bytecode.h"
#include "isa.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration for raw-packet state */
struct ZeusNetState;

/*
 * Effects
 * -------
 * The interpreter never blocks and never touches the host. Every external
 * operation becomes a ZeusEffect: a fully self-describing request that the
 * caller satisfies and hands back as an int64_t result.
 *
 * That is what makes the machine suspendable: a stopped machine is just its
 * registers plus the pending effect, and both are snapshot-safe.
 *
 * Field meanings per kind (buffer operands are offsets into vm->memory):
 *
 *   PRINT, PRINTC      value
 *   READ               (none)
 *   SOCK_TCP, SOCK_UDP (none)
 *   BIND, LISTEN       fd, value = port / backlog
 *   ACCEPT             fd
 *   CONNECT            fd, value = port, buf = NUL-terminated host string
 *   SEND, RECV         fd, buf, len
 *   CLOSE              fd
 *   RAW_ALLOC          value = size
 *   RAW_FREE           id
 *   RAW_SET_PROTO      id, value = proto
 *   RAW_SET_DST/SRC    id, value = packed IPv4 address
 *   RAW_SET_PAYLOAD    id, buf, len
 *   RAW_SEND           id, buf = interface string offset (0 = none)
 *   RAW_RECV           id, buf, len
 */
typedef enum {
    ZEUS_EFFECT_NONE = 0,
    ZEUS_EFFECT_PRINT,
    ZEUS_EFFECT_PRINTC,
    ZEUS_EFFECT_READ,
    ZEUS_EFFECT_SOCK_TCP,
    ZEUS_EFFECT_SOCK_UDP,
    ZEUS_EFFECT_BIND,
    ZEUS_EFFECT_LISTEN,
    ZEUS_EFFECT_ACCEPT,
    ZEUS_EFFECT_CONNECT,
    ZEUS_EFFECT_SEND,
    ZEUS_EFFECT_RECV,
    ZEUS_EFFECT_CLOSE,
    ZEUS_EFFECT_RAW_ALLOC,
    ZEUS_EFFECT_RAW_FREE,
    ZEUS_EFFECT_RAW_SET_PROTO,
    ZEUS_EFFECT_RAW_SET_DST,
    ZEUS_EFFECT_RAW_SET_SRC,
    ZEUS_EFFECT_RAW_SET_PAYLOAD,
    ZEUS_EFFECT_RAW_SEND,
    ZEUS_EFFECT_RAW_RECV,
    ZEUS_EFFECT_KIND_COUNT
} ZeusEffectKind;

typedef struct {
    ZeusEffectKind kind;
    int64_t fd;
    int64_t id;
    int64_t value;
    uint32_t buf;
    uint32_t len;
} ZeusEffect;

typedef enum {
    ZEUS_VM_RUNNING = 0,
    ZEUS_VM_YIELDED = 1,
    ZEUS_VM_HALTED = 2
} ZeusVMStatus;

/* VM execution state. This struct is the machine: registers, both stacks and
 * all memory. Everything a snapshot needs is here (plus the pending effect),
 * apart from the raw-packet buffers behind `net`. */
typedef struct {
    /* Program */
    ZeusProgram program;

    /* Registers */
    uint32_t pc;            /* Program counter (byte offset into code) */
    int64_t sp;             /* Stack pointer (next free slot) */
    int64_t csp;            /* Call-stack pointer */
    ZeusError error;        /* Last error / halt reason */
    bool started;           /* True once execution has begun */
    bool running;           /* True while executing */
    bool yielded;           /* True while an effect is pending */
    ZeusEffect pending;     /* The outstanding effect, if yielded */

    /* Memory */
    int64_t stack[ZEUS_STACK_SIZE];
    uint32_t call_stack[ZEUS_CALL_SIZE];
    uint8_t memory[ZEUS_MEMORY_SIZE];

    /* Networking state */
    struct ZeusNetState *net;
} ZeusVM;

/* Initialize a VM (does not load a program). */
void zeus_vm_init(ZeusVM *vm);

/* Free VM resources. */
void zeus_vm_free(ZeusVM *vm);

/* Load a program into the VM. */
ZeusError zeus_vm_load(ZeusVM *vm, const ZeusProgram *prog);

/* Reset VM state (stacks, pc) but keep loaded program. */
void zeus_vm_reset(ZeusVM *vm);

/* Mark the machine as started and able to execute. A freshly loaded machine
 * is ready but not started; stepping it before this does nothing. */
void zeus_vm_start(ZeusVM *vm);

/* Run until HALT or error. Effects are satisfied by the native handler. */
ZeusError zeus_vm_run(ZeusVM *vm);

/* Execute one instruction, delivering any effect natively.
 * This is the "just works like before" API used by `zeus run`. */
ZeusError zeus_vm_step(ZeusVM *vm);

/* Execute one instruction. If it needs the outside world, *out is filled,
 * vm->yielded is set and no result is pushed. Otherwise *out is
 * ZEUS_EFFECT_NONE. */
ZeusError zeus_vm_exec(ZeusVM *vm, ZeusEffect *out);

/* Deliver the result of the pending effect and clear the yield. */
ZeusError zeus_vm_resume(ZeusVM *vm, int64_t result);

/* Deliver the pending effect with the native handler, then resume the machine
 * with its result. This is the one "effect -> host -> result" path, shared by
 * zeus_vm_step and the CLI driver. On handler failure the effect is abandoned
 * and the machine is halted with ZEUS_ERR_EFFECT (see
 * zeus_vm_cancel_effect). */
ZeusError zeus_vm_deliver_native(ZeusVM *vm, int64_t *result);

/* Abandon the pending effect without delivering it. The machine halts with
 * `reason` and no effect stays staged, so the result is a consistent,
 * snapshottable state. A refused effect is never silently converted into a
 * result the program can ignore: denial kills the machine. */
ZeusError zeus_vm_cancel_effect(ZeusVM *vm, ZeusError reason);

/* True if `eff`'s operands address valid memory for its kind. This is the
 * authoritative effect-shape check; the snapshot layer uses it so a stored
 * effect is exactly one the VM could have produced. */
bool zeus_effect_operands_valid(const ZeusVM *vm, const ZeusEffect *eff);

/* The built-in handler: reproduces host behaviour (stdout, stdin, sockets).
 * Returns false if the effect failed; *result is then -1. */
bool zeus_apply_native_effect(ZeusVM *vm, const ZeusEffect *eff, int64_t *result);

/* Current machine state. */
ZeusVMStatus zeus_vm_status(const ZeusVM *vm);

/* Stable name for an effect kind ("send", "recv", ...). */
const char *zeus_effect_name(ZeusEffectKind kind);

/* Stack helpers */
int64_t zeus_vm_peek(const ZeusVM *vm, int64_t depth);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_VM_H */
