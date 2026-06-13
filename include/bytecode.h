#ifndef ZEUS_BYTECODE_H
#define ZEUS_BYTECODE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "zeus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* On-disk bytecode header (17 bytes) */
typedef struct {
    uint8_t magic[ZEUS_MAGIC_LEN];
    uint8_t version;
    uint32_t entry_point;
    uint32_t code_len;
    uint32_t data_len;
} ZeusHeader;

/* In-memory representation of a bytecode program */
typedef struct {
    ZeusHeader header;
    uint8_t *code;
    uint8_t *data;
} ZeusProgram;

/* Initialize an empty program structure. */
void zeus_program_init(ZeusProgram *prog);

/* Free program code/data memory. Safe to call multiple times. */
void zeus_program_free(ZeusProgram *prog);

/* Load a program from a file. Returns ZEUS_OK or an error code. */
ZeusError zeus_program_load(ZeusProgram *prog, const char *path);

/* Write a program to a file. Returns ZEUS_OK or an error code. */
ZeusError zeus_program_save(const ZeusProgram *prog, const char *path);

/* Validate header magic and version. */
bool zeus_header_valid(const ZeusHeader *hdr);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_BYTECODE_H */
