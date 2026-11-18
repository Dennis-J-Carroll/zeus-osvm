#include "bytecode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Explicit little-endian serialization                                */
/*                                                                     */
/* The old code fwrite()'d a struct and let the compiler's padding     */
/* become part of the file format. It is now byte-by-byte, so the      */
/* on-disk header is exactly ZEUS_HEADER_SIZE (17) bytes on every host.*/
/* ------------------------------------------------------------------ */

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

void zeus_program_init(ZeusProgram *prog) {
    if (!prog) return;
    memset(prog, 0, sizeof(*prog));
}

void zeus_program_free(ZeusProgram *prog) {
    if (!prog) return;
    free(prog->code);
    free(prog->data);
    prog->code = NULL;
    prog->data = NULL;
    prog->header.code_len = 0;
    prog->header.data_len = 0;
}

bool zeus_header_valid(const ZeusHeader *hdr) {
    if (!hdr) return false;
    if (memcmp(hdr->magic, ZEUS_MAGIC, ZEUS_MAGIC_LEN) != 0) {
        return false;
    }
    if (hdr->version != ZEUS_VERSION) {
        return false;
    }
    return true;
}

ZeusError zeus_program_load(ZeusProgram *prog, const char *path) {
    if (!prog || !path) return ZEUS_ERR_IO;

    zeus_program_free(prog);

    FILE *f = fopen(path, "rb");
    if (!f) return ZEUS_ERR_IO;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ZEUS_ERR_IO;
    }
    long fsize = ftell(f);
    if (fsize < 0 || (unsigned long)fsize < ZEUS_HEADER_SIZE) {
        fclose(f);
        return ZEUS_ERR_BAD_BYTECODE;
    }
    rewind(f);

    uint8_t raw[ZEUS_HEADER_SIZE];
    if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
        fclose(f);
        return ZEUS_ERR_IO;
    }

    if (memcmp(raw, ZEUS_MAGIC, ZEUS_MAGIC_LEN) != 0) {
        fclose(f);
        return ZEUS_ERR_BAD_BYTECODE;
    }
    if (raw[4] != ZEUS_VERSION) {
        fclose(f);
        return ZEUS_ERR_BAD_BYTECODE;
    }

    ZeusHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, raw, ZEUS_MAGIC_LEN);
    hdr.version = raw[4];
    hdr.entry_point = read_u32_le(raw + 5);
    hdr.code_len = read_u32_le(raw + 9);
    hdr.data_len = read_u32_le(raw + 13);

    if (hdr.code_len == 0 || hdr.code_len > ZEUS_MAX_CODE_SIZE ||
        hdr.data_len > ZEUS_MEMORY_SIZE ||
        hdr.entry_point > hdr.code_len) {
        fclose(f);
        return ZEUS_ERR_BAD_BYTECODE;
    }

    unsigned long long expected = (unsigned long long)ZEUS_HEADER_SIZE
                                + (unsigned long long)hdr.code_len
                                + (unsigned long long)hdr.data_len;
    if ((unsigned long long)fsize != expected) {
        fclose(f);
        return ZEUS_ERR_BAD_BYTECODE;   /* truncated or padded */
    }

    prog->header = hdr;

    if (hdr.code_len > 0) {
        prog->code = (uint8_t *)malloc(hdr.code_len);
        if (!prog->code) {
            fclose(f);
            return ZEUS_ERR_IO;
        }
        if (fread(prog->code, 1, hdr.code_len, f) != hdr.code_len) {
            fclose(f);
            zeus_program_free(prog);
            return ZEUS_ERR_IO;
        }
    }

    if (hdr.data_len > 0) {
        prog->data = (uint8_t *)malloc(hdr.data_len);
        if (!prog->data) {
            fclose(f);
            zeus_program_free(prog);
            return ZEUS_ERR_IO;
        }
        if (fread(prog->data, 1, hdr.data_len, f) != hdr.data_len) {
            fclose(f);
            zeus_program_free(prog);
            return ZEUS_ERR_IO;
        }
    }

    fclose(f);
    return ZEUS_OK;
}

ZeusError zeus_program_save(const ZeusProgram *prog, const char *path) {
    if (!prog || !path) return ZEUS_ERR_IO;
    if (prog->header.code_len == 0 || prog->header.code_len > ZEUS_MAX_CODE_SIZE) {
        return ZEUS_ERR_BAD_BYTECODE;
    }
    if (prog->header.data_len > ZEUS_MEMORY_SIZE) {
        return ZEUS_ERR_BAD_BYTECODE;
    }

    FILE *f = fopen(path, "wb");
    if (!f) return ZEUS_ERR_IO;

    uint8_t raw[ZEUS_HEADER_SIZE];
    memcpy(raw, ZEUS_MAGIC, ZEUS_MAGIC_LEN);
    raw[4] = ZEUS_VERSION;
    write_u32_le(raw + 5, prog->header.entry_point);
    write_u32_le(raw + 9, prog->header.code_len);
    write_u32_le(raw + 13, prog->header.data_len);

    if (fwrite(raw, 1, sizeof(raw), f) != sizeof(raw)) {
        fclose(f);
        return ZEUS_ERR_IO;
    }

    if (prog->header.code_len > 0) {
        if (fwrite(prog->code, 1, prog->header.code_len, f) != prog->header.code_len) {
            fclose(f);
            return ZEUS_ERR_IO;
        }
    }

    if (prog->header.data_len > 0) {
        if (fwrite(prog->data, 1, prog->header.data_len, f) != prog->header.data_len) {
            fclose(f);
            return ZEUS_ERR_IO;
        }
    }

    if (fclose(f) != 0) return ZEUS_ERR_IO;
    return ZEUS_OK;
}
