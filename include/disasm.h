#ifndef ZEUS_DISASM_H
#define ZEUS_DISASM_H

#include <stdio.h>
#include "zeus.h"
#include "bytecode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Disassemble a program to a FILE stream. */
ZeusError zeus_disassemble(const ZeusProgram *prog, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_DISASM_H */
