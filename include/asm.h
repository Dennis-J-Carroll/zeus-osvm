#ifndef ZEUS_ASM_H
#define ZEUS_ASM_H

#include <stdint.h>
#include <stddef.h>
#include "zeus.h"
#include "bytecode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Assemble source text into a bytecode program.
 *
 * Source syntax:
 *   LABEL:
 *       OPCODE [operand]
 *   Comments begin with ';'.
 *   Numbers can be decimal, hex (0x...), or octal (0...).
 *   String literals "..." are allowed only in .data segments.
 */
ZeusError zeus_assemble(const char *source, ZeusProgram *out_prog, char *err_buf, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_ASM_H */
