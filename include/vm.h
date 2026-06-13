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

/* VM execution state */
typedef struct {
    /* Program */
    ZeusProgram program;

    /* Registers */
    uint32_t pc;            /* Program counter (byte offset into code) */
    int64_t sp;             /* Stack pointer (next free slot) */
    int64_t csp;            /* Call-stack pointer */
    ZeusError error;        /* Last error / halt reason */
    bool running;           /* True while executing */

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

/* Run until HALT or error. Returns final error/halt code. */
ZeusError zeus_vm_run(ZeusVM *vm);

/* Step one instruction. */
ZeusError zeus_vm_step(ZeusVM *vm);

/* Stack helpers */
int64_t zeus_vm_peek(const ZeusVM *vm, int64_t depth);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_VM_H */
