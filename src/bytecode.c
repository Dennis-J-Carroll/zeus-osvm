#include "bytecode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (memcmp(hdr->magic, ZEUS_MAGIC, ZEUS_MAGIC_LEN) != 0) {
        return false;
    }
    if (hdr->version != ZEUS_VERSION) {
        return false;
    }
    return true;
}

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

ZeusError zeus_program_load(ZeusProgram *prog, const char *path) {
    if (!prog || !path) return ZEUS_ERR_IO;

    zeus_program_free(prog);

    FILE *f = fopen(path, "rb");
    if (!f) return ZEUS_ERR_IO;

    ZeusHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return ZEUS_ERR_IO;
    }

    if (!zeus_header_valid(&hdr)) {
        fclose(f);
        return ZEUS_ERR_IO;
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

    FILE *f = fopen(path, "wb");
    if (!f) return ZEUS_ERR_IO;

    /* Header already stored little-endian by the assembler, but normalize. */
    ZeusHeader hdr = prog->header;
    write_u32_le(hdr.magic + 0, read_u32_le(hdr.magic)); /* magic is ascii, fine */
    write_u32_le((uint8_t *)&hdr.entry_point, hdr.entry_point);
    write_u32_le((uint8_t *)&hdr.code_len, hdr.code_len);
    write_u32_le((uint8_t *)&hdr.data_len, hdr.data_len);

    if (fwrite(&prog->header, sizeof(prog->header), 1, f) != 1) {
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

    fclose(f);
    return ZEUS_OK;
}
